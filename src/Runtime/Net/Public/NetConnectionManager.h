#pragma once
#include <cstdint>
#include <vector>
#include <algorithm>
#include <cstring>

#include "Events.h"
#include "NetTypes.h"

// ---------------------------------------------------------------------------
// ClientInputAccumulator — persistent outbound input payload for one client
// connection. Lives in ConnectionInfo; updated each TickInputSend tick.
//
// Design: the payload IS the persistent object. Key state is always the most
// recent snapshot. Events are tagged with their absolute sim frame so they can
// be trimmed by LastServerAckedFrame and have FrameUSOffset reconstructed at
// send time relative to the current FirstClientFrame.
// ---------------------------------------------------------------------------
struct PendingNetInputEvent
{
	uint32_t SimFrame; // absolute sim frame this event belongs to
	uint32_t Key;      // SDL_Scancode
	uint8_t Pressed;   // 1 = down, 0 = up
};

struct ClientInputAccumulator
{
	InputSnapshot State{};
	std::vector<PendingNetInputEvent> PendingEvents;

	// Remove events that the server has already consumed.
	void TrimAcked(uint32_t lastAckedFrame)
	{
		PendingEvents.erase(
			std::remove_if(PendingEvents.begin(), PendingEvents.end(),
						   [lastAckedFrame](const PendingNetInputEvent& e) { return e.SimFrame <= lastAckedFrame; }),
			PendingEvents.end());
	}

	// Build wire payload. FrameUSOffset per event is reconstructed relative to firstFrame.
	// Events are capped at 8 on the wire; oldest are sent first (lowest SimFrame).
	InputFramePayload BuildPayload(uint32_t firstFrame, uint32_t lastFrame, uint32_t frameTimeUS) const
	{
		InputFramePayload p{};
		p.State            = State;
		p.FirstClientFrame = firstFrame;
		p.LastClientFrame  = lastFrame;

		uint8_t count = 0;
		for (const PendingNetInputEvent& pe : PendingEvents)
		{
			if (count >= 8) break;
			p.Events[count].Key     = pe.Key;
			p.Events[count].Pressed = pe.Pressed;
			p.Events[count]._Pad    = 0;
			// Reconstruct μs offset relative to the current send window's FirstClientFrame.
			p.Events[count].FrameUSOffset = static_cast<uint16_t>(
				(pe.SimFrame - firstFrame) * frameTimeUS);
			count++;
		}
		p.EventCount = count;
		return p;
	}
};

// Forward declarations — avoid pulling GNS headers into every consumer
class ISteamNetworkingSockets;
class GNSContext;
struct SteamNetConnectionStatusChangedCallback_t;
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
	HSteamNetConnection Handle      = 0;    // GNS connection handle
	uint8_t OwnerID                 = 0;    // Assigned NetOwnerID (0 = server/unassigned)
	uint32_t NextSeqOut             = 0;    // Next outgoing sequence number
	uint32_t LastSeqIn              = 0;    // Latest received sequence number (for ack piggybacking)
	uint32_t AckBitfield            = 0;    // Received-packet bitfield (for ack piggybacking)
	float RTT_ms                    = 0.0f; // Smoothed round-trip time in milliseconds
	uint32_t ServerFrameAtHandshake = 0;    // Server FrameNumber when HandshakeAccept was sent
	uint32_t LocalFrameAtHandshake  = 0;    // Client's own frame when HandshakeAccept was received
	uint32_t InputLead              = 0;    // Frames to lead the server — set after ClockSync
	uint8_t ClockSyncProbesSent     = 0;    // Ping probes sent during Synchronizing phase
	uint8_t ClockSyncProbesRecvd    = 0;    // Pong responses received during Synchronizing phase
	double LastHeartbeatTime        = 0.0;  // SDL_GetPerformanceCounter() / freq at last Ping send
	ClientRepState RepState         = ClientRepState::PendingHandshake;
	bool bConnected                 = false;
	bool bServerSide                = false; // True for server-accepted handles, false for client-initiated
	bool bClientInitiated           = false; // True only for connections we opened via Connect() — reliable even in GNS loopback
	bool bInitialSpawnFlushed       = false; // Server-side: true after first full entity batch sent to this client

	// Client-side: last frame the server confirmed it consumed. Advances on inbound
	// AckedClientFrame — never on send. Drives TrimAcked() and FirstClientFrame floor.
	uint32_t LastServerAckedFrame = 0;

	// Server-side: last client input frame consumed by the injector. Stamped into every
	// outbound header so the client can trim its send window.
	uint32_t LastAckedClientFrame = 0;

	// Client-side: persistent outbound input payload. Key state is always current;
	// events accumulate until acked, trimmed by LastServerAckedFrame before each send.
	ClientInputAccumulator InputAccum;

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
			  const uint8_t* payload, bool reliable = false);

	/// Send raw bytes (header already serialized into buffer).
	bool SendRaw(HSteamNetConnection conn, const uint8_t* data, uint32_t size, bool reliable = false);

	/// Run GNS internal callbacks (connection status changes, etc.).
	/// Must be called regularly from the polling thread.
	void RunCallbacks();

	// --- Connection bookkeeping ---

	/// Find connection info by GNS handle. Returns nullptr if not found.
	ConnectionInfo* FindConnection(HSteamNetConnection conn);

	/// Find connection info by OwnerID. Returns nullptr if not found.
	/// O(n) over active connections — only use outside hot-path code.
	/// In PIE, two ConnectionInfo entries share the same OwnerID (bServerSide and
	/// bClientInitiated). Pass requireServerSide=true to get the server-accepted leg.
	ConnectionInfo* FindConnectionByOwnerID(uint8_t ownerID, bool requireServerSide = false)
	{
		for (ConnectionInfo& ci : Connections)
		{
			if (ci.OwnerID != ownerID) continue;
			if (requireServerSide && !ci.bServerSide) continue;
			return &ci;
		}
		return nullptr;
	}

	/// Get all active connections.
	const std::vector<ConnectionInfo>& GetConnections() const { return Connections; }
	std::vector<ConnectionInfo>& GetConnections() { return Connections; }

	/// Number of active connections.
	int GetConnectionCount() const { return static_cast<int>(Connections.size()); }

	/// Assign a NetOwnerID to a connection.
	void AssignOwnerID(HSteamNetConnection conn, uint8_t ownerID);

	/// Generate and Assign a NetOwnerID to a connection. Called during handshake.
	void GenerateNetID(HSteamNetConnection conn);

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

	ISteamNetworkingSockets* Sockets = nullptr; // Borrowed from GNSContext
	HSteamListenSocket ListenSocket  = 0;
	HSteamNetPollGroup PollGroup     = 0;

	std::vector<ConnectionInfo> Connections;

	/// Singleton pointer for static callback routing.
	/// Only one NetConnectionManager should exist per process.
	static NetConnectionManager* s_Instance;
};