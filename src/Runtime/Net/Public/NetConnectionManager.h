#pragma once
#include <atomic>
#include <cstdint>
#include <list>
#include <vector>
#include <algorithm>
#include <cstring>

#include "Events.h"
#include "GNSContext.h"
#include "NetTypes.h"

// Forward declarations — avoid pulling GNS headers into every consumer
typedef uint32_t HSteamNetConnection;
typedef uint32_t HSteamListenSocket;
typedef uint32_t HSteamNetPollGroup;

// Max simultaneous connections, derived from NetOwnerID bit width.
static constexpr size_t MaxNetConnections = 1 << NetOwnerID_Bits;

// ---------------------------------------------------------------------------
// ConnectionInfo — engine-side bookkeeping per connection.
// ---------------------------------------------------------------------------
struct ConnectionInfo
{
	HSteamNetConnection Handle           = 0;    // GNS connection handle
	uint8_t OwnerID                      = 0;    // Assigned NetOwnerID (0 = server/unassigned)
	uint32_t NextSeqOut                  = 0;    // Next outgoing sequence number
	uint32_t LastSeqIn                   = 0;    // Latest received sequence number (for ack piggybacking)
	uint32_t AckBitfield                 = 0;    // Received-packet bitfield (for ack piggybacking)
	float RTT_ms                         = 0.0f; // Smoothed round-trip time in milliseconds
	uint32_t ServerFrameAtHandshake      = 0;    // Server FrameNumber when HandshakeAccept was sent
	uint32_t ClientLocalFrameAtHandshake = 0;    // Client's local frame at handshake.
	//   Client-side: set when HandshakeAccept is received.
	//   Server-side: set from ClockSyncRequest.LocalFrameAtHandshake.

	// ---------------------------------------------------------------------------
	// Frame-space translation helpers.
	//
	// offset = ServerFrameAtHandshake - ClientLocalFrameAtHandshake.
	// Negative when the client leads the server (the common case with InputLead > 0).
	// All arithmetic uses int64_t intermediates to avoid overflow at large frame counts.
	// ---------------------------------------------------------------------------
	int32_t GetFrameOffset() const
	{
		return static_cast<int32_t>(
			static_cast<int64_t>(ServerFrameAtHandshake) -
			static_cast<int64_t>(ClientLocalFrameAtHandshake));
	}

	// Convert a server-space frame number to client-local frame space.
	uint32_t ToClientFrame(uint32_t serverFrame) const
	{
		return static_cast<uint32_t>(static_cast<int64_t>(serverFrame) - GetFrameOffset());
	}

	// Convert a client-local frame number to server-space frame.
	uint32_t ToServerFrame(uint32_t clientFrame) const
	{
		return static_cast<uint32_t>(static_cast<int64_t>(clientFrame) + GetFrameOffset());
	}

	uint32_t InputLead              = 0;    // Frames to lead the server — set after ClockSync
	uint8_t ClockSyncProbesSent     = 0;    // Ping probes sent during Synchronizing phase
	uint8_t ClockSyncProbesRecvd    = 0;    // Pong responses received during Synchronizing phase
	double LastHeartbeatTime        = 0.0;  // SDL_GetPerformanceCounter() / freq at last Ping send
	ClientRepState RepState         = ClientRepState::PendingHandshake;
	bool bConnected                 = false;
	bool bAuthoritySide                = false; // True for server-accepted handles, false for client-initiated
	bool bOwnerInitiated           = false; // True only for connections we opened via Connect() — reliable even in GNS loopback
	bool bInitialSpawnFlushed       = false; // Server-side: true after first full entity batch sent to this client

	// Client-side: last frame the server confirmed it consumed — in client-local frame space
	// (the server converts from server-frame via FrameOffset before stamping the header).
	// Advances on inbound AckedClientFrame — never on send.
	// Drives DropFront() on the MPSC accumulator ring.
	uint32_t LastServerAckedFrame = 0;

	// Server-side: last client-local input frame consumed by the injector (client-frame space).
	// Stamped into every outbound header so the client can trim its send window.
	uint32_t LastAckedClientFrame = 0;

	// Client-side only — tracks in-flight spawn predictions awaiting Confirm/Reject.
	PredictionLedger Predictions;

	// Client-side: SDL_GetTicks() ms at which the last PlayerBeginRequest was sent.
	// Used by TickReplication to detect stalls and trigger retransmission.
	uint64_t PlayerBeginSentAt = 0;
};

// ---------------------------------------------------------------------------
// ReceivedMessage — a complete header + payload received from the network.
// ---------------------------------------------------------------------------
struct ReceivedMessage
{
	PacketHeader Header;
	std::vector<uint8_t> Payload;
	HSteamNetConnection Connection = 0; // Which connection it arrived on
};

DEFINE_FIXED_MULTICALLBACK(OnClientConnectedEvent, MaxNetConnections, const ConnectionInfo&)
DEFINE_FIXED_MULTICALLBACK(OnClientDisconnectedEvent, MaxNetConnections, uint8_t /*ownerID*/)

// ---------------------------------------------------------------------------
// NetConnectionManager
//
// Wraps GNS socket operations into a simple server/client API.
// - Server: Listen() → accept connections via status callback → PollIncoming() → Send()
// - Client: Connect() → PollIncoming() → Send()
//
// All public methods are intended to be called from a single thread (NetThread).
// The status callback (OnConnectionStatusChanged) may fire from GNS's service
// thread — it only touches the PendingConnections queue which NetThread drains.
// ---------------------------------------------------------------------------
class NetConnectionManager
{
public:
	NetConnectionManager() = default;
	~NetConnectionManager();

	/// Bind to GNS context. Must be called before Listen/Connect.
	void Initialize(GNSContext* gns);
	void Shutdown();

	// --- Server API ---

	/// Start listening on the given port. Returns true on success.
	bool Listen(uint16_t port);

	/// Stop listening and close all connections.
	void StopListening();

	/// Accept a pending connection (called from status callback context).
	void AcceptConnection(HSteamNetConnection conn);

	// --- Client API ---

	/// Connect to a remote server. Returns the GNS connection handle (0 on failure).
	HSteamNetConnection Connect(const char* address, uint16_t port);

	// --- Shared API ---

	/// Close a specific connection.
	void CloseConnection(HSteamNetConnection conn, const char* reason = "Closed");

	/// Poll GNS for incoming messages on all connections. Appends to outMessages.
	/// Returns number of messages received.
	int PollIncoming(std::vector<ReceivedMessage>& outMessages);

	/// Send a header + payload to a specific connection. Returns true on success.
	bool Send(HSteamNetConnection conn, const PacketHeader& header,
			  const uint8_t* payload, bool reliable = false, bool noNagle = false);

	/// Send a pre-built header (sequence already stamped by Sentinel) + payload.
	/// Use from job workers — does NOT touch NextSeqOut on ConnectionInfo.
	bool SendPrebuilt(HSteamNetConnection conn, const PacketHeader& header,
					  const uint8_t* payload, bool noNagle = false)
	{
		return Send(conn, header, payload, /*reliable=*/false, noNagle);
	}

	/// Send raw bytes (header already serialized into buffer).
	bool SendRaw(HSteamNetConnection conn, const uint8_t* data, uint32_t size, bool reliable = false, bool noNagle = false);

	/// Force NoNagle on all unreliable sends from this manager.
	/// Equivalent to the EngineConfig::NoNagle setting. Safe to call before Start().
	void SetNoNagle(bool enabled) { bGlobalNoNagle = enabled; }

	/// Set GNS per-connection send rate (bytes/sec). Applied to every connection
	/// at Connect/Accept time. -1 = leave GNS default (256KB/s).
	/// Set both min and max to the same value to pin the rate.
	void SetSendRate(int minBytesPerSec, int maxBytesPerSec)
	{
		SendRateMin = minBytesPerSec;
		SendRateMax = maxBytesPerSec;
	}

	/// Run GNS internal callbacks (connection status changes, etc.).
	/// Must be called regularly from the polling thread.
	void RunCallbacks();

	// --- Connection bookkeeping ---

	/// Find connection info by GNS handle. Returns nullptr if not found.
	ConnectionInfo* FindConnection(HSteamNetConnection conn);

	/// Find connection info by OwnerID. Returns nullptr if not found.
	/// O(n) over active connections — only use outside hot-path code.
	/// In PIE, two ConnectionInfo entries share the same OwnerID (bAuthoritySide and
	/// bOwnerInitiated). Pass requireServerSide=true to get the server-accepted leg.
	ConnectionInfo* FindConnectionByOwnerID(uint8_t ownerID, bool requireServerSide = false)
	{
		for (ConnectionInfo& ci : Connections)
		{
			if (ci.OwnerID != ownerID) continue;
			if (requireServerSide && !ci.bAuthoritySide) continue;
			return &ci;
		}
		return nullptr;
	}

	/// Get all active connections.
	const std::list<ConnectionInfo>& GetConnections() const { return Connections; }
	std::list<ConnectionInfo>& GetConnections() { return Connections; }

	/// Number of active connections.
	int GetConnectionCount() const { return static_cast<int>(Connections.size()); }

	/// Assign a NetOwnerID to a connection.
	void AssignOwnerID(HSteamNetConnection conn, uint8_t ownerID);

	/// Generate and Assign a NetOwnerID to a connection. Called during handshake.
	void GenerateNetID(HSteamNetConnection conn);

#if defined(TNX_TESTING) || defined(TNX_ENABLE_EDITOR)
	/// The OwnerID assigned to this process's own client-initiated connection.
	/// Set atomically in AssignOwnerID when the connection is client-initiated.
	/// Safe to poll from any thread — 0 means not yet assigned.
	/// Available in testing and editor builds only.
	uint8_t GetLocalOwnerID() const { return LocalOwnerID.load(std::memory_order_acquire); }
#endif

	/// Fired when a server-side connection is fully confirmed (GNS handshake complete).
	/// Fires on the NetThread during RunCallbacks(). Safe to call World::Spawn() from here.
	OnClientConnectedEvent OnClientConnected;

	/// Fired when a server-side connection closes (ClosedByPeer or ProblemDetectedLocally).
	/// ownerID is 0 if the client disconnected before completing the handshake.
	OnClientDisconnectedEvent OnClientDisconnected;

	/// GNS status callback — routes into this manager. Must be static for GNS.
	static void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* info);

private:
	/// Register a new connection in our bookkeeping.
	void AddConnection(HSteamNetConnection conn);

	/// Remove a connection from bookkeeping.
	void RemoveConnection(HSteamNetConnection conn);

	SocketHandle& Sockets           = SocketHandle::Invalid(); // Borrowed from GNSContext
	HSteamListenSocket ListenSocket = 0;
	HSteamNetPollGroup PollGroup    = 0;

	bool bGlobalNoNagle = false;
	int  SendRateMin    = -1; // -1 = use GNS default
	int SendRateMax     = -1;

	std::list<ConnectionInfo> Connections;

	/// Atomically updated when AssignOwnerID is called on a client-initiated connection.
	/// Readable from any thread — used by tests and the editor to observe handshake completion.
#if defined(TNX_TESTING) || defined(TNX_ENABLE_EDITOR)
	std::atomic<uint8_t> LocalOwnerID{0};
#endif

	/// Singleton pointer for static callback routing.
	/// Only one NetConnectionManager should exist per process.
	static NetConnectionManager* s_Instance;
};