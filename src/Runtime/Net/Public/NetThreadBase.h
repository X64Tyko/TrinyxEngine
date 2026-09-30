#pragma once
#include <cstdint>
#include <memory>
#include <vector>

#include "NetTypes.h"
#include "NetConnectionManager.h"
#include "StreamingTypes.h"

#include <SDL3/SDL_timer.h>

class GNSContext;
class NetConnectionManager;
class WorldBase;
struct EngineConfig;
struct InputBuffer;
struct ReceivedMessage;

// ---------------------------------------------------------------------------
// NetThreadBase<Derived>  (CRTP)
//
// Owns the GNS transport and rate-limited Tick logic. No OS thread — polling
// and rate gating run on the Sentinel thread in TrinyxEngine::RunMainLoop.
//
// Derived must implement two hooks (TickInputSend has a public no-op default):
//   void HandleMessage(const ReceivedMessage& msg)   — role-specific routing
//   void TickReplication()                            — no-op on client
//   void TickInputSend()  [optional override]         — no-op on server; client sends InputFrame
//
// All instances run in the same mode:
//   Sentinel calls PumpMessages() each 1ms tick (Poll + recv + HandleMessage).
//   Sentinel calls TickDispatch() each 1ms tick — fires spawn/correction jobs when a new frame is ready.
//   Sentinel calls TickInputSend() gated at InputNetHz (128Hz).
//   Sentinel calls Tick() gated at NetworkUpdateHz (30Hz) — wire flush + clock sync.
// ---------------------------------------------------------------------------

template <typename Derived>
class NetThreadBase
{
public:
	NetThreadBase()  = default;
	~NetThreadBase() = default;

	NetThreadBase(const NetThreadBase&)            = delete;
	NetThreadBase& operator=(const NetThreadBase&) = delete;

	void Initialize(GNSContext* gns, const EngineConfig* config);

	// Initialize as a PIE child handler — shares an externally-owned ConnectionMgr.
	void InitAsHandler(GNSContext* gns, const EngineConfig* config, NetConnectionManager* sharedMgr);

	void Tick();

	/// One message-processing iteration: Poll + RunCallbacks + PollIncoming + HandleMessage.
	/// Called from Sentinel on every 1ms tick.
	void PumpMessages();

	/// Convenience alias — equivalent to PumpMessages().
	void PollAndDispatch() { Self().PumpMessages(); }

	NetConnectionManager* GetConnectionManager() { return ConnectionMgr; }
	const NetConnectionManager* GetConnectionManager() const { return ConnectionMgr; }

	// Per-client world routing for input-frame send path.
	// OwnerID 0 = server world. 1-255 = client connections.
	void MapConnectionToWorld(uint8_t ownerID, WorldBase* world) { WorldMap[ownerID] = world; }

	// Default no-op — server inherits this; client and PIE override.
	void TickInputSend()
	{
	}

	// Default no-op — OwnerNet overrides to send the LevelReady ack after background load.
	void AcknowledgeLevelReady(StreamingRequestID /*requestID*/) {}


	// Default no-op — AuthorityNet and PIE override.
	void TickDispatch()
	{
	}

protected:
	Derived& Self() { return *static_cast<Derived*>(this); }

	GNSContext* GNS            = nullptr;
	const EngineConfig* Config = nullptr;

	// Owning pointer — only set when this instance called Initialize() (not InitAsHandler()).
	std::unique_ptr<NetConnectionManager> OwnedConnectionMgr;
	// Raw view — always valid after Initialize() or InitAsHandler().
	NetConnectionManager* ConnectionMgr = nullptr;

	// OwnerID → World for the input-send path (client legs only)
	WorldBase* WorldMap[MaxOwnerIDs]{};

private:
	void TickClockSync(double nowSec);
};

// Member function bodies live in Net/Private/NetThreadBase.cpp, explicitly instantiated for the
// closed set of derived handlers (AuthorityNet, OwnerNet, PIENetThread).
