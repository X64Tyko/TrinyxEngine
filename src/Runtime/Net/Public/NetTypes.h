#pragma once
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "RegistryTypes.h" // NetOwnerID_Bits

// ---------------------------------------------------------------------------
// NetMessageType — discriminator for network messages.
// ---------------------------------------------------------------------------
enum class NetMessageType : uint8_t
{
	ConnectionHandshake = 0,  // Client<->Server: join request / accept+reject + assigned PlayerID
	InputFrame          = 1,  // Client->Server: input state for frame N
	StateCorrection     = 2,  // Server->Client: authoritative state snapshot for frame N
	EntitySpawn         = 3,  // Server->Client: new entity creation command
	EntityDestroy       = 4,  // Server->Client: entity destruction command
	Ping                = 5,  // Bidirectional: RTT measurement
	Pong                = 6,  // Bidirectional: RTT measurement response
	FlowEvent           = 7,  // Server->Client: session flow signal (ServerReady, etc.)
	SpawnRequest        = 8,  // Client->Server: request to spawn a player body
	SpawnConfirm        = 9,  // Server->Client: authoritative spawn confirmation
	SpawnReject         = 10, // Server->Client: spawn request denied
	ClockSync           = 11, // Bidirectional: frame clock synchronisation
	TravelNotify        = 12, // Server->Client: load this level (path + server frame)
	LevelReady          = 13, // Client->Server: level load complete, ready for entity flush
	GameModeManifest    = 14, // Server->Client: game-layer context publish (mode, level, rules, etc.)
	ClientModeManifest  = 15, // Client->Server: optional reply to a GameModeManifest (preferences, loadout, etc.)
	Custom              = 16, // Game-defined: first slot for user-extended message types
	Unknown             = 17, // Sentinel: unrecognised message type — receiver must drop
	Count
};

// ---------------------------------------------------------------------------
// Packet header field sizes — adjust these to resize the wire format.
// Changing any value will change sizeof(PacketHeader) and break the
// static_assert below so the impact is immediately visible.
// ---------------------------------------------------------------------------
static constexpr size_t PacketType_Bytes     = 1;                         // NetMessageType discriminator
static constexpr size_t PacketFlags_Bytes    = 1;                         // bit0=HasAck, bit1=HasTimestamp
static constexpr size_t PayloadSize_Bytes    = 2;                         // max payload: (1 << 16) - 1 = 65535
static constexpr size_t SequenceNum_Bytes    = 4;                         // per-connection monotonic counter
static constexpr size_t FrameNumber_Bytes    = 4;                         // simulation frame this message relates to
static constexpr size_t SenderID_Bytes       = (NetOwnerID_Bits + 7) / 8; // derived from NetOwnerID_Bits (currently 8 → 1 byte)
static constexpr size_t Timestamp_Bytes      = 2;                         // wrapping milliseconds for RTT estimation
static constexpr size_t AckSequenceNum_Bytes = 4;                         // latest sequence received from remote
static constexpr size_t AckBitfield_Bytes    = 4;                         // 32-packet sliding ack window

static constexpr size_t PacketHeader_ExpectedSize =
	PacketType_Bytes + PacketFlags_Bytes + PayloadSize_Bytes +
	SequenceNum_Bytes + FrameNumber_Bytes +
	SenderID_Bytes + 1 /* padding */ + Timestamp_Bytes +
	AckSequenceNum_Bytes + AckBitfield_Bytes;

// ---------------------------------------------------------------------------
// Packet flags
// ---------------------------------------------------------------------------
namespace PacketFlag
{
	static constexpr uint8_t HasAck       = 1 << 0; // AckSequenceNum + AckBitfield are meaningful
	static constexpr uint8_t HasTimestamp = 1 << 1; // Timestamp is meaningful (default on)
	static constexpr uint8_t DefaultFlags = HasTimestamp;
}

// ---------------------------------------------------------------------------
// PacketHeader — prepended to every network message.
//
// Fixed 24-byte layout. Optional fields (Ack, Timestamp) are always present
// on the wire for parse simplicity; Flags bits indicate whether the receiver
// should act on them. Zeroed when not in use.
//
// If you change any *_Bytes constant above, the static_assert fires and you
// must reconcile the struct layout with the new sizes.
// ---------------------------------------------------------------------------
struct PacketHeader
{
	uint8_t Type;            // NetMessageType
	uint8_t Flags;           // PacketFlag bitmask
	uint16_t PayloadSize;    // Bytes following this header (max 65535)
	uint32_t SequenceNum;    // Per-connection monotonic sequence number
	uint32_t FrameNumber;    // Simulation frame this message relates to
	uint8_t SenderID;        // NetOwnerID — 0 = server/global, 1-255 = connected players
	uint8_t _Pad0;           // Alignment / future use
	uint16_t Timestamp;      // Wrapping milliseconds for per-packet RTT estimation
	uint32_t AckSequenceNum; // Latest SequenceNum received from remote (valid if HasAck)
	uint32_t AckBitfield;    // Bit i = received (AckSequenceNum - 1 - i) (valid if HasAck)

	// --- Helpers ---

	bool HasAck() const { return Flags & PacketFlag::HasAck; }
	bool HasTimestamp() const { return Flags & PacketFlag::HasTimestamp; }

	// --- Serialization (little-endian wire format) ---

	/// Write header + payload into a flat buffer. Returns total bytes written.
	/// Buffer must be at least sizeof(PacketHeader) + PayloadSize bytes.
	static uint32_t Serialize(uint8_t* outBuf, const PacketHeader& header, const uint8_t* payload)
	{
		std::memcpy(outBuf, &header, sizeof(PacketHeader));
		if (header.PayloadSize > 0 && payload) std::memcpy(outBuf + sizeof(PacketHeader), payload, header.PayloadSize);
		return sizeof(PacketHeader) + header.PayloadSize;
	}

	/// Read header from buffer. Returns pointer to payload (buffer + sizeof(PacketHeader)).
	static const uint8_t* Deserialize(const uint8_t* buf, uint32_t bufSize, PacketHeader& outHeader)
	{
		if (bufSize < sizeof(PacketHeader)) return nullptr;
		std::memcpy(&outHeader, buf, sizeof(PacketHeader));
		if (bufSize < sizeof(PacketHeader) + outHeader.PayloadSize) return nullptr;
		return buf + sizeof(PacketHeader);
	}
};

static_assert(sizeof(PacketHeader) == PacketHeader_ExpectedSize,
			  "PacketHeader size mismatch — if you changed a *_Bytes constant, update the struct layout to match");
static_assert(sizeof(PacketHeader) == 24, "PacketHeader must be 24 bytes");

// ---------------------------------------------------------------------------
// BaseNetPayload — CRTP base for all game-layer network payloads.
//
// Provides a compile-time PayloadSize constant so NetChannel::Send can stamp
// the header without any runtime computation or struct field overhead.
// The static_assert enforces that all payloads are trivially copyable —
// preventing silent corruption from vtables, std::string, etc. on the wire.
//
// Usage:
//   struct RoyaleManifestData : BaseNetPayload<RoyaleManifestData>
//   {
//       uint8_t TeamCount;
//       uint32_t RuleFlags;
//   };
//   ch.Send<NetMessageType::GameModeManifest>(data, /*reliable=*/true);
//
// Receiver validates: hdr.PayloadSize == RoyaleManifestData::PayloadSize
// before handing bytes to the GameMode — mismatched builds drop the packet.
// ---------------------------------------------------------------------------
template <typename TDerived>
struct BaseNetPayload
{
	static constexpr uint16_t PayloadSize = sizeof(TDerived);
	// Trivial-copy check deferred to first use — TDerived incomplete at base instantiation time.
	static constexpr void ValidateTrivial()
	{
		static_assert(std::is_trivially_copyable_v<TDerived>,
					  "Net payloads must be trivially copyable -- no vtable, no std::string, no unique_ptr");
	}
};

// ---------------------------------------------------------------------------
// EntitySpawnPayload — server tells client to create an entity.
//
// Sent reliable with NetMessageType::EntitySpawn. One entity per message.
// Client uses Manifest.ClassType to call CreateByClassID, then writes fields.
// TODO: Replace ClassType-based spawning with PrefabID once the asset system supports it.
// TODO: Batch spawns — one message per entity is significant overhead for initial world flush.
//       Replace with a variable-length batch message (count + entity array) once the
//       reliable send path can handle variable-size payloads efficiently.
//
// SpawnFlags layout (Generation_Bits = 16, so both halves are 16 bits):
//
//   bits [31 : 32-Generation_Bits]  =  entity generation  (Generation_Bits wide)
//   bits [Generation_Bits-1 : 0]   =  spawn flags        (32-Generation_Bits wide)
//
// Use Pack / GetFlags / GetGeneration — do not read SpawnFlags raw. If
// Generation_Bits changes, static_assert on EntityRef catches mismatches.
// ---------------------------------------------------------------------------
struct EntitySpawnPayload
{
	// Identity
	uint32_t NetHandle; // EntityNetHandle.Value — network identity
	uint32_t Manifest;  // EntityNetManifest.Value — ClassType + flags

	// TransRot (Temporal)
	float PosX, PosY, PosZ;
	float RotQx, RotQy, RotQz, RotQw;

	// Scale (Volatile)
	float ScaleX, ScaleY, ScaleZ;

	// Color (Volatile)
	float ColorR, ColorG, ColorB, ColorA;

	// Mesh (Volatile)
	uint32_t MeshID;

	// SpawnFlags — generation packed in high Generation_Bits bits,
	// spawn flags in the remaining low bits. Use helpers below.
	int32_t SpawnFlags;

	// --- Spawn flag constants (fit in lower 16 bits when Generation_Bits == 16) ---
	static constexpr uint32_t SpawnFlag_Background = 1u << 0; // Alive-only; client sweeps Active on ServerReady

	// --- SpawnFlags helpers ---
	static constexpr uint32_t FlagsMask = (1u << (32u - Generation_Bits)) - 1u;

	static int32_t Pack(uint32_t flagBits, uint32_t generation)
	{
		return static_cast<int32_t>(
			(generation << (32u - Generation_Bits)) | (flagBits & FlagsMask));
	}

	static uint32_t GetFlags(int32_t spawnFlags)
	{
		return static_cast<uint32_t>(spawnFlags) & FlagsMask;
	}

	static uint16_t GetGeneration(int32_t spawnFlags)
	{
		return static_cast<uint16_t>(static_cast<uint32_t>(spawnFlags) >> (32u - Generation_Bits));
	}
};

static_assert(sizeof(EntitySpawnPayload) == 72, "EntitySpawnPayload must be 72 bytes");

// ---------------------------------------------------------------------------
// StateCorrectionEntry — one entity's authoritative transform.
//
// Batched: a StateCorrection message payload contains N of these.
// Count = PacketHeader.PayloadSize / sizeof(StateCorrectionEntry).
// Sent unreliable at NetworkUpdateHz.
// ---------------------------------------------------------------------------
struct StateCorrectionEntry
{
	uint32_t NetHandle; // EntityNetHandle.Value

	// Authoritative position + rotation from server
	float PosX, PosY, PosZ;
	float RotQx, RotQy, RotQz, RotQw;
};

static_assert(sizeof(StateCorrectionEntry) == 32, "StateCorrectionEntry must be 32 bytes");

// ---------------------------------------------------------------------------
// HandshakePayload — server accept response carries session bootstrap data.
//
// Sent reliable with NetMessageType::ConnectionHandshake when the server
// accepts a client join. OwnerID is carried in the header SenderID field;
// this payload adds the context the client needs to start clock sync.
// ---------------------------------------------------------------------------
struct HandshakePayload
{
	uint32_t TickRate;    // Server's FixedUpdateHz (e.g., 512)
	uint32_t ServerFrame; // Server's FrameNumber at accept time — clock sync anchor
};

static_assert(sizeof(HandshakePayload) == 8, "HandshakePayload must be 8 bytes");

// ---------------------------------------------------------------------------
// ClientRepState — per-connection session state machine (server-side).
//
// Transitions:
//   PendingHandshake → Synchronizing  (HandshakeAccept sent)
//   Synchronizing    → Loading        (ClockSync complete, InputLead known)
//   Loading          → LevelLoading   (TravelNotify sent to client)
//   LevelLoading     → LevelLoaded    (LevelReady received from client)
//   LevelLoaded      → Loaded         (initial EntitySpawn batch flushed, ServerReady sent)
//   Loaded           → Playing        (SpawnConfirm sent)
// ---------------------------------------------------------------------------
enum class ClientRepState : uint8_t
{
	PendingHandshake = 0, // Waiting for HandshakeRequest
	Synchronizing    = 1, // Clock sync probes in flight
	Loading          = 2, // Clock sync done; waiting to send TravelNotify
	LevelLoading     = 3, // TravelNotify sent; client loading level
	LevelLoaded      = 4, // LevelReady received; flushing initial entity batch
	Loaded           = 5, // Batch flushed, ServerReady sent; client sweeping Alive→Active
	Playing          = 6, // SpawnConfirm sent; full simulation sync active
};

// ---------------------------------------------------------------------------
// FlowEvent identifiers — carried in FlowEventPayload.EventID.
// ---------------------------------------------------------------------------
enum class FlowEventID : uint8_t
{
	ServerReady  = 0, // Server has flushed initial entity batch; client sweeps Alive→Active then sends SpawnRequest
	TravelNotify = 1, // Server tells client to load a level (payload: TravelPayload)
};

// ---------------------------------------------------------------------------
// FlowEventPayload — server signals a session flow event to the client.
// Sent reliable (NetMessageType::FlowEvent).
// ---------------------------------------------------------------------------
struct FlowEventPayload
{
	uint8_t EventID; // FlowEventID
	uint8_t _Pad[3];
};

static_assert(sizeof(FlowEventPayload) == 4, "FlowEventPayload must be 4 bytes");

// ---------------------------------------------------------------------------
// SpawnRequestPayload — client requests a player body spawn.
// Sent reliable (NetMessageType::SpawnRequest).
// ---------------------------------------------------------------------------
struct SpawnRequestPayload
{
	int64_t PrefabID;      // AssetID raw value of the requested Construct prefab
	uint32_t PredictionID; // Client-local prediction token; echoed in Confirm/Reject
	uint32_t _Pad;
	float PosX, PosY, PosZ; // Desired spawn position hint (server may override)
	float _Pad2;
};

static_assert(sizeof(SpawnRequestPayload) == 32, "SpawnRequestPayload must be 32 bytes");

// ---------------------------------------------------------------------------
// SpawnConfirmPayload — server confirms an authoritative spawn.
// Sent reliable (NetMessageType::SpawnConfirm).
// ---------------------------------------------------------------------------
struct SpawnConfirmPayload
{
	uint32_t NetHandle;     // Authoritative EntityNetHandle.Value for the spawned body
	uint32_t PredictionID;  // Echoed from SpawnRequestPayload
	float PosX, PosY, PosZ; // Authoritative spawn position
	uint16_t Generation;    // Entity generation — client uses this to form a valid EntityRef
	uint16_t _Pad;
};

static_assert(sizeof(SpawnConfirmPayload) == 24, "SpawnConfirmPayload must be 24 bytes");

// ---------------------------------------------------------------------------
// SpawnRejectPayload — server rejects a spawn request.
// Sent reliable (NetMessageType::SpawnReject).
// ---------------------------------------------------------------------------
struct SpawnRejectPayload
{
	uint32_t PredictionID; // Echoed from SpawnRequestPayload
	uint8_t Reason;        // Implementation-defined reject code
	uint8_t _Pad[3];
};

static_assert(sizeof(SpawnRejectPayload) == 8, "SpawnRejectPayload must be 8 bytes");

// ---------------------------------------------------------------------------
// ClockSyncPayload — bidirectional clock synchronisation probe.
//
// Client → Server (request):  ClientTimestamp set, ServerFrame = 0.
// Server → Client (response): ServerFrame set to current FrameNumber,
//                              ClientTimestamp echoed for RTT calculation.
// Sent unreliable (NetMessageType::ClockSync).
// ---------------------------------------------------------------------------
struct ClockSyncPayload
{
	uint64_t ClientTimestamp; // SDL_GetPerformanceCounter() at send time
	uint32_t ServerFrame;     // Server current FrameNumber (0 in request, filled in response)
	uint32_t _Pad;
};

static_assert(sizeof(ClockSyncPayload) == 16, "ClockSyncPayload must be 16 bytes");

// ---------------------------------------------------------------------------
// TravelPayload — server tells client to load a level.
//
// Sent reliable (NetMessageType::TravelNotify) after the server finishes loading
// the level. Client loads the level, sends LevelReady when done.
// LevelPath is a null-terminated UTF-8 string (max 255 chars + null).
//
// IMPORTANT: LevelPath must be the content-relative path (e.g. "Arena.tnxscene"),
// NOT the server's absolute path. Absolute paths silently corrupt on any client
// whose ProjectDir differs from the server's — the path would be truncated by
// PathLength or accepted verbatim but fail to open. The sender is responsible for
// stripping the ProjectDir+"/content/" prefix before filling this field.
// ---------------------------------------------------------------------------
struct TravelPayload
{
	uint8_t PathLength;  // Length of LevelPath (excluding null terminator)
	char LevelPath[255]; // Content-relative path to .tnxscene (null-terminated)
};

static_assert(sizeof(TravelPayload) == 256, "TravelPayload must be 256 bytes");

// ---------------------------------------------------------------------------
// GameModeManifestPayload<TDerived> — CRTP base for GameMode→client context
// publishes. Inherits BaseNetPayload<TDerived> so PayloadSize reflects the
// full derived struct size, not just the base.
//
// Game-specific rules go in a derived struct:
//   struct RoyaleManifest : GameModeManifestPayload<RoyaleManifest>
//   {
//       uint8_t TeamCount;
//       uint32_t RuleFlags;
//   };
//   ch.Send<NetMessageType::GameModeManifest>(manifest, reliable);
//
// Engine validates the base size (>= sizeof GameModeManifestPayload<T>) before
// handing bytes to the GameMode; the GameMode casts to its concrete type.
// SequenceID is echoed in ClientModeManifestPayload to correlate replies.
// ---------------------------------------------------------------------------
template <typename TDerived>
struct GameModeManifestPayload : BaseNetPayload<TDerived>
{
	uint8_t SequenceID;      // Monotonic per-mode counter; echoed by client reply
	uint8_t ModeNameLength;  // Length of ModeName (excluding null terminator)
	char ModeName[62];       // Display name of the active GameMode (null-terminated)
	uint8_t LevelNameLength; // Length of LevelName (excluding null terminator)
	char LevelName[62];      // Display name of the active level (null-terminated)
	uint8_t MaxPlayers;      // Max player count for UI display
	uint8_t _pad[1];         // alignment
};

// ---------------------------------------------------------------------------
// ClientModeManifestPayload<TDerived> — CRTP base for client→GameMode replies.
// Game-specific preference fields go in a derived struct:
//   struct RoyaleClientManifest : ClientModeManifestPayload<RoyaleClientManifest>
//   {
//       uint8_t PreferredTeam;
//       uint32_t CosmeticFlags;
//   };
//
// ManifestSequenceID must match the GameModeManifestPayload.SequenceID being
// replied to — the server discards stale replies.
// ---------------------------------------------------------------------------
template <typename TDerived>
struct ClientModeManifestPayload : BaseNetPayload<TDerived>
{
	uint8_t ManifestSequenceID; // SequenceID of the GameModeManifest being acknowledged
	uint8_t _pad[3];            // alignment
};

// ---------------------------------------------------------------------------
// PredictionLedger — client-side record of in-flight spawn predictions.
//
// Lives in ConnectionInfo (transport layer). Keyed by PredictionID echoed
// in SpawnConfirm/SpawnReject. Single-entry today; same structure scales to
// projectile/ability prediction by expanding capacity.
//
// On SpawnConfirm: promote the predicted Construct to authoritative, call
//   Soul::ClaimBody(ref), clear the ledger entry.
// On SpawnReject: destroy the predicted Construct, clear the ledger entry.
// ---------------------------------------------------------------------------
struct PredictionLedger
{
	static constexpr uint8_t Capacity = 1; // expand when projectile prediction lands

	struct Entry
	{
		uint32_t PredictionID = 0;  // echoed by server in Confirm/Reject
		uint32_t RequestFrame = 0;  // local frame when SpawnRequest was sent
		ConstructRef LocalRef = {}; // predicted Construct handle (client-side)
		int64_t PrefabUUID    = 0;  // prefab used for prediction
		bool Active           = false;
	};

	Entry Entries[Capacity] = {};

	void Set(uint32_t predID, uint32_t frame, ConstructRef ref, int64_t uuid)
	{
		for (auto& e : Entries)
		{
			if (!e.Active)
			{
				e = {predID, frame, ref, uuid, true};
				return;
			}
		}
	}

	Entry* Find(uint32_t predID)
	{
		for (auto& e : Entries) if (e.Active && e.PredictionID == predID) return &e;
		return nullptr;
	}

	void Clear(uint32_t predID)
	{
		for (auto& e : Entries) if (e.PredictionID == predID)
		{
			e = {};
			return;
		}
	}
};


// 64 bytes of key state + 2 floats of mouse delta.
// ---------------------------------------------------------------------------
struct InputFramePayload
{
	uint8_t KeyState[64];
	float MouseDX;
	float MouseDY;
	uint8_t MouseButtons;
	uint8_t _Pad[3];
};

static_assert(sizeof(InputFramePayload) == 76, "InputFramePayload must be 76 bytes");

