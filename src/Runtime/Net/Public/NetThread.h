#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "NetTypes.h"

// Forward declarations
class GNSContext;
class FlowManager;
class NetConnectionManager;
class ReplicationSystem;
class World;
struct InputBuffer;
struct EngineConfig;

// ---------------------------------------------------------------------------
// NetThread — network coordinator.
//
// Two modes of operation:
//   1. Threaded: call Start() — spins up a dedicated thread that polls at
//      NetworkUpdateHz. Used when running alongside Sentinel (Client,
//      ListenServer).
//   2. Inline: call Tick() from your own loop — no thread created. Used on
//      dedicated servers where the main thread becomes the network poller
//      instead of wasting a core on a sleeping Sentinel.
//
// In both modes the work is identical: poll GNS, deserialize messages,
// route InputFrames to the appropriate World's InputBuffer.
// ---------------------------------------------------------------------------
class NetThread
{
public:
	NetThread();
	~NetThread();

	/// Bind to GNS context and config. Must be called before Start() or Tick().
	void Initialize(GNSContext* gns, const EngineConfig* config);

	// ── Threaded mode ───────────────────────────────────────────────────
	void Start();
	void Stop();
	void Join();

	// ── Inline mode ─────────────────────────────────────────────────────
	/// Execute one poll cycle. Call this from your own loop (e.g. main thread
	/// on a dedicated server). Does the same work as one iteration of the
	/// internal thread loop.
	void Tick();

	bool IsRunning() const { return bIsRunning.load(std::memory_order_relaxed); }

	/// Register a World to receive routed input from a specific OwnerID.
	/// OwnerID 0 = server world (no input routing needed).
	/// OwnerID 1-255 = client connections whose InputFrame messages route
	/// to this World's SimInput buffer.
	void MapConnectionToWorld(uint8_t ownerID, World* world);

	/// Register a FlowManager for a specific OwnerID.
	/// OwnerID 0 = server FlowManager (used for server-side level path queries).
	/// OwnerID 1-255 = per-client FlowManagers (receive TravelNotify and FlowEvents).
	void MapConnectionToFlow(uint8_t ownerID, FlowManager* flow);

	/// Set the FlowManager so the NetThread can post flow events to Sentinel.
	/// Must be set before Start(). Not required for dedicated server mode.
	void SetFlowManager(FlowManager* flow) { FlowMgr = flow; }

	/// Access the connection manager (for server Listen / client Connect).
	NetConnectionManager* GetConnectionManager() { return ConnectionMgr.get(); }

	/// Attach a server-side replication system. Called during PIE setup.
	/// NetThread does NOT own this pointer — caller manages lifetime.
	void SetReplicationSystem(ReplicationSystem* repl) { Replicator = repl; }

	// FPS tracking
	float GetNetFPS() const { return NetFPS.load(std::memory_order_relaxed); }
	float GetNetFrameMs() const { return NetFrameMs.load(std::memory_order_relaxed); }

private:
	void ThreadMain();
	void RouteMessage(const struct ReceivedMessage& msg);

	GNSContext* GNS               = nullptr;
	const EngineConfig* Config    = nullptr;
	ReplicationSystem* Replicator = nullptr;
	FlowManager* FlowMgr          = nullptr; // Non-owning; set by engine before Start()

	std::unique_ptr<NetConnectionManager> ConnectionMgr;

	// Connection → World routing table.
	// Index = OwnerID (0-255), value = World* (nullptr if unmapped).
	World* WorldMap[256]{};

	// Connection → FlowManager routing table.
	// Index = OwnerID (0-255), value = FlowManager* (nullptr if unmapped).
	// OwnerID 0 = server; 1-255 = per-client flows for TravelNotify / FlowEvent dispatch.
	FlowManager* FlowMap[256]{};

	std::thread Thread;
	std::atomic<bool> bIsRunning{false};

	// FPS tracking
	std::atomic<float> NetFPS{0.0f};
	std::atomic<float> NetFrameMs{0.0f};
	double FpsTimer     = 0.0;
	double LastFPSCheck = 0.0;
	int FpsFrameCount   = 0;
};