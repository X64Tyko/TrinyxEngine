#pragma once
#include <cstdint>
#include <cstring>

#include "RegistryTypes.h" // NetOwnerID_Bits

// ---------------------------------------------------------------------------
// NetMessageType — discriminator for network messages.
// ---------------------------------------------------------------------------
enum class NetMessageType : uint8_t
{
	ConnectionHandshake = 0, // Client<->Server: join request / accept+reject + assigned PlayerID
	InputFrame          = 1, // Client->Server: input state for frame N
	StateCorrection     = 2, // Server->Client: authoritative state snapshot for frame N
	EntitySpawn         = 3, // Server->Client: new entity creation command
	EntityDestroy       = 4, // Server->Client: entity destruction command
	Ping                = 5, // Bidirectional: RTT measurement
	Pong                = 6,
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
// EntitySpawnPayload — server tells client to create an entity.
//
// Sent reliable with NetMessageType::EntitySpawn. One entity per message.
// Client uses Manifest.ClassType to call CreateByClassID, then writes fields.
// TODO: Replace ClassType-based spawning with PrefabID once the asset system supports it.
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

	uint32_t _Pad0; // Align to 72 bytes
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
