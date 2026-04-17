#pragma once
#include <cstdint>
#include <functional>

// -----------------------------------------------------------------------
// AssetID — 64-bit packed asset identifier
//
// Bit layout:
//   63-56  AssetType        (8 bits)   — immutable classification
//   55-54  AssetLoadPriority (2 bits)  — streamer hint
//   53-50  AssetPlatform    (4 bits)   — cooker strip target (16 platforms)
//   49-48  AssetCompression (2 bits)   — pak decompression hint
//   47-8   UUID             (40 bits)  — identity (what gets hashed/compared)
//   7-0    AssetFlags       (8 bits)   — immutable classification flags
//
// Equality and hashing always use UUID bits only.
// Embedded metadata (type, priority, platform, compression) is readable
// without a registry lookup — streamer, cooker, and GPU upload read directly.
// -----------------------------------------------------------------------

// --- Immutable classification — set at import, baked into ID, never change ---

enum class AssetType : uint8_t
{
	Invalid      = 0x00,
	DataAsset    = 0x01, // generic data (config, JSON, etc.)
	StaticMesh   = 0x02,
	SkeletalMesh = 0x03,
	Material     = 0x04,
	Texture      = 0x05,
	Audio        = 0x06,
	Animation    = 0x07,
	Level        = 0x08, // scene files
	Prefab       = 0x09,
	// 0x0A-0xEF reserved for file-based asset types

	// Code-registered types (not file assets — registered via macros)
	FlowState  = 0xF0,
	GameMode   = 0xF1,
	EntityType = 0xF2,
};

enum class AssetLoadPriority : uint8_t
{
	Default  = 0x00, // standard streaming queue
	Low      = 0x01, // background, never block
	High     = 0x02, // load before scene is playable
	Critical = 0x03, // block until resident
};

enum class AssetPlatform : uint8_t
{
	All         = 0x00, // ships on every platform
	Windows     = 0x01,
	Linux       = 0x02,
	Mac         = 0x03,
	PlayStation = 0x04,
	Xbox        = 0x05,
	Switch      = 0x06,
	SteamDeck   = 0x07,
	iOS         = 0x08,
	Android     = 0x09,
	Web         = 0x0A,
	// 5 slots reserved
};

enum class AssetCompression : uint8_t
{
	Default = 0x00,
	None    = 0x01, // already compressed (textures, audio)
	Fast    = 0x02, // LZ4 — fast decompress, moderate ratio
	Max     = 0x03, // Zstd — slow decompress, best ratio
};

enum class AssetFlags : uint8_t
{
	None       = 0x00,
	Cooked     = 0x01, // processed for target platform
	EditorOnly = 0x02, // stripped from shipping builds
	Streaming  = 0x04, // always streamed, never blocking loaded
	// 5 bits reserved
};

// --- Mutable runtime state — lives exclusively in registry entry, never in the ID ---

enum class RuntimeFlags : uint8_t
{
	None      = 0x00,
	Loaded    = 0x01,
	Streaming = 0x02, // load currently in flight
	Pinned    = 0x04,
	Missing   = 0x08, // failed reconcile
	Dirty     = 0x10, // content hash mismatch
};

// -----------------------------------------------------------------------
// AssetID
// -----------------------------------------------------------------------

struct AssetID
{
	int64_t Raw = 0;

	// --- Identity ---
	// UUID is bits [47:8] — the sole identity used for equality and hashing.
	int64_t GetUUID() const { return Raw & 0x0000FFFFFFFFFF00LL; }

	// --- Embedded metadata — readable without registry lookup ---
	AssetType GetType() const { return static_cast<AssetType>((Raw >> 56) & 0xFF); }
	AssetLoadPriority GetLoadPriority() const { return static_cast<AssetLoadPriority>((Raw >> 54) & 0x03); }
	AssetPlatform GetPlatform() const { return static_cast<AssetPlatform>((Raw >> 50) & 0x0F); }
	AssetCompression GetCompression() const { return static_cast<AssetCompression>((Raw >> 48) & 0x03); }
	AssetFlags GetFlags() const { return static_cast<AssetFlags>(Raw & 0xFF); }

	bool IsValid() const { return GetUUID() != 0; }

	// --- Setters for construction (import pipeline only) ---
	void SetType(AssetType t) { Raw = (Raw & ~(0xFFLL << 56)) | (static_cast<int64_t>(t) << 56); }
	void SetLoadPriority(AssetLoadPriority p) { Raw = (Raw & ~(0x03LL << 54)) | (static_cast<int64_t>(p) << 54); }
	void SetPlatform(AssetPlatform p) { Raw = (Raw & ~(0x0FLL << 50)) | (static_cast<int64_t>(p) << 50); }
	void SetCompression(AssetCompression c) { Raw = (Raw & ~(0x03LL << 48)) | (static_cast<int64_t>(c) << 48); }
	void SetFlags(AssetFlags f) { Raw = (Raw & ~0xFFLL) | static_cast<int64_t>(f); }
	void SetUUID(int64_t uuid) { Raw = (Raw & ~0x0000FFFFFFFFFF00LL) | (uuid & 0x0000FFFFFFFFFF00LL); }

	// --- Build a complete AssetID from parts ---
	static AssetID Create(int64_t uuid, AssetType type,
						  AssetLoadPriority priority = AssetLoadPriority::Default,
						  AssetPlatform platform     = AssetPlatform::All,
						  AssetCompression compress  = AssetCompression::Default,
						  AssetFlags flags           = AssetFlags::None)
	{
		AssetID id;
		id.SetUUID(uuid);
		id.SetType(type);
		id.SetLoadPriority(priority);
		id.SetPlatform(platform);
		id.SetCompression(compress);
		id.SetFlags(flags);
		return id;
	}

	// Equality strips everything except UUID
	bool operator==(const AssetID& other) const { return GetUUID() == other.GetUUID(); }
	bool operator!=(const AssetID& other) const { return GetUUID() != other.GetUUID(); }
};

struct AssetIDHash
{
	size_t operator()(const AssetID& id) const
	{
		return std::hash<int64_t>{}(id.GetUUID());
	}
};

// -----------------------------------------------------------------------
// AssetType ↔ file extension mapping
// -----------------------------------------------------------------------

inline AssetType AssetTypeFromExtension(const char* ext)
{
	if (!ext || ext[0] == '\0') return AssetType::Invalid;

	// Skip leading dot
	if (ext[0] == '.') ++ext;

	// Scene/prefab (engine formats)
	if (ext[0] == 't' && ext[1] == 'n' && ext[2] == 'x')
	{
		if (ext[3] == 's') return AssetType::Level;  // .tnxscene
		if (ext[3] == 'p') return AssetType::Prefab; // .tnxprefab
		if (ext[3] == 'a') return AssetType::Audio;  // .tnxaudio
	}

	// Common asset formats
	if ((ext[0] == 'p' && ext[1] == 'n' && ext[2] == 'g' && ext[3] == '\0') ||
		(ext[0] == 'j' && ext[1] == 'p' && ext[2] == 'g' && ext[3] == '\0') ||
		(ext[0] == 'b' && ext[1] == 'm' && ext[2] == 'p' && ext[3] == '\0') ||
		(ext[0] == 't' && ext[1] == 'g' && ext[2] == 'a' && ext[3] == '\0'))
		return AssetType::Texture;

	if ((ext[0] == 'o' && ext[1] == 'b' && ext[2] == 'j' && ext[3] == '\0') ||
		(ext[0] == 'g' && ext[1] == 'l' && ext[2] == 't' && ext[3] == 'f') ||
		(ext[0] == 'g' && ext[1] == 'l' && ext[2] == 'b' && ext[3] == '\0') ||
		(ext[0] == 'f' && ext[1] == 'b' && ext[2] == 'x' && ext[3] == '\0'))
		return AssetType::StaticMesh;

	// Engine binary mesh format
	if (ext[0] == 't' && ext[1] == 'n' && ext[2] == 'x' && ext[3] == 'm'
		&& ext[4] == 'e' && ext[5] == 's' && ext[6] == 'h' && ext[7] == '\0')
		return AssetType::StaticMesh;

	if ((ext[0] == 'w' && ext[1] == 'a' && ext[2] == 'v' && ext[3] == '\0') ||
		(ext[0] == 'o' && ext[1] == 'g' && ext[2] == 'g' && ext[3] == '\0') ||
		(ext[0] == 'm' && ext[1] == 'p' && ext[2] == '3' && ext[3] == '\0'))
		return AssetType::Audio;

	if ((ext[0] == 'j' && ext[1] == 's' && ext[2] == 'o' && ext[3] == 'n')) return AssetType::DataAsset;

	if (ext[0] == 'p' && ext[1] == 'r' && ext[2] == 'e' && ext[3] == 'f'
		&& ext[4] == 'a' && ext[5] == 'b' && ext[6] == '\0')
		return AssetType::Prefab;

	return AssetType::Invalid;
}

inline const char* AssetTypeName(AssetType type)
{
	switch (type)
	{
		case AssetType::DataAsset: return "Data";
		case AssetType::StaticMesh: return "Static Mesh";
		case AssetType::SkeletalMesh: return "Skeletal Mesh";
		case AssetType::Material: return "Material";
		case AssetType::Texture: return "Texture";
		case AssetType::Audio: return "Audio";
		case AssetType::Animation: return "Animation";
		case AssetType::Level: return "Level";
		case AssetType::Prefab: return "Prefab";
		case AssetType::FlowState: return "Game State";
		case AssetType::GameMode: return "Game Mode";
		case AssetType::EntityType: return "Entity Type";
		default: return "Unknown";
	}
}