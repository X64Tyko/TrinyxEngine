#pragma once
#include <cstdint>

#include "Input.h"
#include "Logger.h"
#include "RegistryTypes.h"
#ifdef TNX_ENABLE_NETWORK
#include "NetChannel.h"
#include "RPC.h"
#endif

class FlowManager;
class Soul;
class World;

// ---------------------------------------------------------------------------
// SoulRole — bitmask flags describing a Soul's stance on a given machine.
//
// Flags are orthogonal and can be combined:
//
//   Echo          = 0          non-owning peer: mirrors corrections only
//   Owner         = bit 0      drives input from the local keyboard
//   Authority     = bit 1      simulation authority; has the ground truth
//   Owner|Authority            listen-server local player — both at once
//
// Input routing priority (GetSimInput / GetVizInput):
//   Owner flag set   → local keyboard buffer (GetSimInput / GetVizInput)
//   Authority only   → network-injected buffer (GetPlayerSimInput / GetPlayerVizInput)
//   Echo (0)         → nullptr (follow corrections only, no input)
// ---------------------------------------------------------------------------
enum class SoulRole : uint8_t
{
	Echo      = 0,      // No flags — follow corrections only
	Owner     = 1 << 0, // Local keyboard drives this player
	Authority = 1 << 1, // Simulation authority (server-side or listen-server)
};

// ---------------------------------------------------------------------------
// Soul — per-player session object, one per connected OwnerID.
//
// Soul bridges the network layer and the gameplay layer. It is the stable
// identity for a player across the full session — it survives Body death
// and respawns.
//
// One Soul per OwnerID, always. Even in splitscreen, two players on one
// machine each get their own Soul and OwnerID.
//
// Lifetime: Session. Created by FlowManager::OnClientLoaded, destroyed by
// FlowManager::OnClientDisconnected. Not a Construct — it is pure data and
// an event dispatch point. Game code subclasses Soul to add stats, loadouts,
// and other session-persistent data.
//
// Thread safety: Soul is created and destroyed on the Sentinel thread.
// Gameplay reads (GetOwnerID, GetInputLead) are safe from any thread once
// created. Writes to mutable fields must happen from Sentinel or under the
// spawn handshake contract.
// ---------------------------------------------------------------------------
class Soul
{
public:
	explicit Soul(uint8_t ownerID)
		: OwnerID(ownerID)
	{
	}

	virtual ~Soul() = default;

	Soul(const Soul&)            = delete;
	Soul& operator=(const Soul&) = delete;

	uint8_t GetOwnerID() const { return OwnerID; }
	uint32_t GetInputLead() const { return InputLead; }

	SoulRole GetRole() const { return Role; }
	void SetRole(SoulRole role) { Role = role; }
	/// Add one or more role flags without clearing existing ones.
	void AddRole(SoulRole flag) { Role = static_cast<SoulRole>(static_cast<uint8_t>(Role) | static_cast<uint8_t>(flag)); }
	/// Returns true if ALL bits in flag are set on this Soul's role.
	bool HasRole(SoulRole flag) const { return (static_cast<uint8_t>(Role) & static_cast<uint8_t>(flag)) == static_cast<uint8_t>(flag); }

	bool HasBody() const { return ConfirmedBodyHandle.IsValid(); }
	ConstructRef GetBodyHandle() const { return ConfirmedBodyHandle; }

	// -------------------------------------------------------------------------
	// Input routing — the single correct call site for gameplay input access.
	//
	// GetSimInput  — returns the deterministic sim buffer (used in PrePhysics /
	//               fixed-tick gameplay). Null for Echo souls (no input).
	// GetVizInput  — returns the presentation buffer (mouse look, camera, UI).
	//               Null for Echo souls.
	//
	// Priority (Owner wins over Authority):
	//   Owner flag set   → local keyboard   (GetSimInput / GetVizInput)
	//   Authority only   → injected net buf  (GetPlayerSimInput(ownerID) / GetPlayerVizInput(ownerID))
	//   Echo (0)         → nullptr
	// -------------------------------------------------------------------------
	InputBuffer* GetSimInput(World* world) const;
	InputBuffer* GetVizInput(World* world) const;

	/// Called by the GameMode (via WithSpawnManagement or directly) after the
	/// server confirms a spawn. Stores the handle and fires OnBodyConfirmed().
	void ClaimBody(ConstructRef ref)
	{
		ConfirmedBodyHandle = ref;
		OnBodyConfirmed();
	}

	/// Called by FlowManager after creation — game code can override for init.
	virtual void OnJoined()
	{
	}

	/// Called by FlowManager just before destruction.
	virtual void OnLeft()
	{
	}

	/// Called immediately after ClaimBody() — wire input routing, show HUD, etc.
	virtual void OnBodyConfirmed()
	{
	}

	/// Called when the Body is destroyed (death, despawn, respawn cycle start).
	/// ConfirmedBodyHandle is cleared before this fires.
	virtual void OnBodyLost()
	{
	}

	/// Clears ConfirmedBodyHandle and fires OnBodyLost(). Called by GameMode
	/// on Body destruction (e.g. death, level unload).
	void ReleaseBody()
	{
		ConfirmedBodyHandle = {};
		OnBodyLost();
	}

	// -------------------------------------------------------------------------
	// RPC dispatch — called by FlowManager when a SoulRPC packet arrives.
	// The channel is refreshed from ctx before the handler runs so that any
	// reply thunk called inside the handler routes correctly.
	// -------------------------------------------------------------------------
#ifdef TNX_ENABLE_NETWORK
	void DispatchServerRPC(const RPCContext& ctx, const RPCHeader& hdr, const uint8_t* params);
	void DispatchClientRPC(const RPCContext& ctx, const RPCHeader& hdr, const uint8_t* params);

	// Returns the channel used by TNX_IMPL_SERVER / TNX_IMPL_CLIENT thunks to send.
	// Refreshed internally on every RPC dispatch — not accessible externally.
	NetChannel& GetNetChannel() { return Channel; }
#endif

	FlowManager* GetFlowManager() { return FlowMgr; }

	// -------------------------------------------------------------------------
	// Engine-reserved Soul RPCs — PlayerBegin lifecycle.
	// Client calls PlayerBegin(params) → sends to server.
	// Server calls PlayerBeginConfirm/Reject(params) → sends back to client.
	// Implemented in Soul.cpp; game code never touches these directly.
	// -------------------------------------------------------------------------
#ifdef TNX_ENABLE_NETWORK
	TNX_SERVER(PlayerBegin, PlayerBeginRequestPayload);
	TNX_CLIENT(PlayerBeginConfirm, PlayerBeginConfirmPayload);
	TNX_CLIENT(PlayerBeginReject, PlayerBeginRejectPayload);
#endif

private:
	friend class FlowManager;

	uint8_t OwnerID                  = 0; // Assigned at connection — stable for session
	uint32_t InputLead               = 0; // Frames client leads the server
	SoulRole Role                    = SoulRole::Authority;
	ConstructRef ConfirmedBodyHandle = {}; // Valid once ClaimBody() is called

#ifdef TNX_ENABLE_NETWORK
	// Set by FlowManager at creation and refreshed on every RPC dispatch.
	NetChannel Channel   = {};
#endif
	FlowManager* FlowMgr = nullptr;
};

// ---------------------------------------------------------------------------
// Soul role tag helper — converts a Soul's bitmask role into a short display
// string used by the LOG_NET_* macros below.
//
//   Echo (0)              → "ECHO"
//   Owner                 → "OWNER"
//   Authority             → "AUTH"
//   Owner | Authority     → "LISTEN"  (listen-server local player)
//   nullptr               → "NULL"
// ---------------------------------------------------------------------------
inline const char* SoulRoleTag(const Soul* soul)
{
	if (!soul) return "NULL";
	const uint8_t r              = static_cast<uint8_t>(soul->GetRole());
	constexpr uint8_t kOwner     = static_cast<uint8_t>(SoulRole::Owner);
	constexpr uint8_t kAuthority = static_cast<uint8_t>(SoulRole::Authority);
	if ((r & (kOwner | kAuthority)) == (kOwner | kAuthority)) return "LISTEN";
	if (r & kOwner) return "OWNER";
	if (r & kAuthority) return "AUTH";
	return "ECHO";
}

// ---------------------------------------------------------------------------
// LOG_NET_* — Soul-aware logging macros.
//
// Each macro prepends the Soul's role tag so log lines read:
//   [14:56:27.031] [DEBUG] (Soul.cpp:120) [AUTH] Player 2 joined
//
// Usage:
//   LOG_NET_INFO(soul,    "Player joined");
//   LOG_NET_INFO_F(soul,  "Player %u joined", ownerID);
//
// soul must be a const Soul* (or Soul*). nullptr is handled safely → [NULL].
// ---------------------------------------------------------------------------

// Non-formatted variants
#define LOG_NET_TRACE(soul, msg) do { \
    char _TnxNetBuf[512]; \
    snprintf(_TnxNetBuf, sizeof(_TnxNetBuf), "[%s] %s", SoulRoleTag(soul), (msg)); \
    LOG_ENG_TRACE(_TnxNetBuf); \
} while(0)

#define LOG_NET_DEBUG(soul, msg) do { \
    char _TnxNetBuf[512]; \
    snprintf(_TnxNetBuf, sizeof(_TnxNetBuf), "[%s] %s", SoulRoleTag(soul), (msg)); \
    LOG_ENG_DEBUG(_TnxNetBuf); \
} while(0)

#define LOG_NET_INFO(soul, msg) do { \
    char _TnxNetBuf[512]; \
    snprintf(_TnxNetBuf, sizeof(_TnxNetBuf), "[%s] %s", SoulRoleTag(soul), (msg)); \
    LOG_ENG_INFO(_TnxNetBuf); \
} while(0)

#define LOG_NET_WARN(soul, msg) do { \
    char _TnxNetBuf[512]; \
    snprintf(_TnxNetBuf, sizeof(_TnxNetBuf), "[%s] %s", SoulRoleTag(soul), (msg)); \
    LOG_ENG_WARN(_TnxNetBuf); \
} while(0)

#define LOG_NET_ERROR(soul, msg) do { \
    char _TnxNetBuf[512]; \
    snprintf(_TnxNetBuf, sizeof(_TnxNetBuf), "[%s] %s", SoulRoleTag(soul), (msg)); \
    LOG_ENG_ERROR(_TnxNetBuf); \
} while(0)

// Formatted variants — fmt must be a string literal so the role prefix
// can be concatenated at compile time.
#define LOG_NET_TRACE_F(soul, fmt, ...) do { \
    char _TnxNetBuf[512]; \
    snprintf(_TnxNetBuf, sizeof(_TnxNetBuf), "[%s] " fmt, SoulRoleTag(soul), ##__VA_ARGS__); \
    LOG_ENG_TRACE(_TnxNetBuf); \
} while(0)

#define LOG_NET_DEBUG_F(soul, fmt, ...) do { \
    char _TnxNetBuf[512]; \
    snprintf(_TnxNetBuf, sizeof(_TnxNetBuf), "[%s] " fmt, SoulRoleTag(soul), ##__VA_ARGS__); \
    LOG_ENG_DEBUG(_TnxNetBuf); \
} while(0)

#define LOG_NET_INFO_F(soul, fmt, ...) do { \
    char _TnxNetBuf[512]; \
    snprintf(_TnxNetBuf, sizeof(_TnxNetBuf), "[%s] " fmt, SoulRoleTag(soul), ##__VA_ARGS__); \
    LOG_ENG_INFO(_TnxNetBuf); \
} while(0)

#define LOG_NET_WARN_F(soul, fmt, ...) do { \
    char _TnxNetBuf[512]; \
    snprintf(_TnxNetBuf, sizeof(_TnxNetBuf), "[%s] " fmt, SoulRoleTag(soul), ##__VA_ARGS__); \
    LOG_ENG_WARN(_TnxNetBuf); \
} while(0)

#define LOG_NET_ERROR_F(soul, fmt, ...) do { \
    char _TnxNetBuf[512]; \
    snprintf(_TnxNetBuf, sizeof(_TnxNetBuf), "[%s] " fmt, SoulRoleTag(soul), ##__VA_ARGS__); \
    LOG_ENG_ERROR(_TnxNetBuf); \
} while(0)
