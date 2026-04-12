#pragma once
#include <cstdint>
#include <vector>

#include "Events.h"
#include "NetTypes.h"

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
	uint32_t InputLead              = 0;    // Frames to lead the server — set after ClockSync
	uint8_t ClockSyncProbesSent     = 0;    // Ping probes sent during Synchronizing phase
	uint8_t ClockSyncProbesRecvd    = 0;    // Pong responses received during Synchronizing phase
	double LastHeartbeatTime        = 0.0;  // SDL_GetPerformanceCounter() / freq at last Ping send
	ClientRepState RepState         = ClientRepState::PendingHandshake;
	bool bConnected                 = false;
	bool bServerSide                = false; // True for server-accepted handles, false for client-initiated
	bool bClientInitiated           = false; // True only for connections we opened via Connect() — reliable even in GNS loopback
	bool bInitialSpawnFlushed       = false; // Server-side: true after first full entity batch sent to this client

	// Client-side input tracking — used to build FirstClientFrame spans in InputFramePayload.
	uint32_t LastSentInputFrame = 0;

	// Client-side only — tracks in-flight spawn predictions awaiting Confirm/Reject.
	PredictionLedger Predictions;
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