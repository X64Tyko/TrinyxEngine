#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <SDL3/SDL_audio.h>
#include "AssetRegistry.h"
#include "AssetTypes.h"
#include "AudioHandle.h"
#include "TnxName.h"

struct SoundAsset;

// Stub bus enum — routing expanded in a future AudioBus layer.
enum class BusID : uint8_t { Master = 0 };

// Per-play parameters. Call sites never break as fields are added.
struct PlayParams
{
	float Volume     = 1.f;
	float Pitch      = 1.f; // > 1 = faster/higher; SDL stream frequency ratio
	bool Loop        = false;
	BusID Bus        = BusID::Master;
	uint8_t Priority = 128; // 0 = lowest; voice stealing picks smallest value
};

// Internal voice slot. Fixed heap array — std::atomic<float> is not movable.
struct Voice
{
	SDL_AudioStream* Stream   = nullptr;
	SoundHandle Handle        = SoundHandle::Invalid();
	const SoundAsset* Asset   = nullptr;
	std::atomic<float> Volume = {1.f};
	float FadeTarget          = 0.f;
	float FadeRate            = 0.f; // volume/sec; negative = fading out; 0 = no fade
	uint8_t Priority          = 128;
	bool bLoop                = false;
};

// ---------------------------------------------------------------------------
// AudioManager
//
// Engine-singleton (owned by TrinyxEngine). One SDL audio device, one voice pool,
// and a fixed-size audio asset slot table. Mirrors MeshManager's architecture:
// AssetRegistry is the name/ID authority; AudioManager holds the decoded PCM slots
// and a slot→AssetID reverse map.
//
// Asset loading:
//   LoadSound(path, name, id)      — decode from disk, assign slot (import/internal path)
//   LoadSound(AssetID, bPinned)    — resolve path from AssetRegistry, decode, assign slot
//   LoadSound(TnxName, bPinned)    — resolve by name from AssetRegistry, decode, assign slot
//
// Callers play by AssetID or via AudioEvent::Trigger(TnxName).
// Update() is driven by the Sentinel thread at EngineConfig::AudioUpdateHz.
// ---------------------------------------------------------------------------
class AudioManager
{
public:
	static constexpr uint32_t MAX_AUDIO_SLOTS = 256;

	// One loaded sound asset in the slot table.
	struct AudioSlot
	{
		SoundAsset* Asset = nullptr; // owning; freed on Unload/Shutdown
		bool bPinned      = false;   // if true, PCM is never auto-freed after playback
	};

	AudioManager();
	~AudioManager();
	AudioManager(const AudioManager&)            = delete;
	AudioManager& operator=(const AudioManager&) = delete;

	// Opens the default SDL playback device and allocates the voice pool.
	bool Initialize(int maxVoices = 64);
	void Shutdown();

	// --- Asset loading ---

	// Decode a sound file from disk, assign it a slot, and record it in AssetRegistry.
	// bPinned=false: PCM is freed automatically once all voices finish playing it.
	// bPinned=true:  PCM stays resident until UnloadSound is called explicitly.
	// Returns the slot index, or UINT32_MAX on failure.
	uint32_t LoadSound(const char* path, const std::string& name = {}, AssetID id = {}, bool bPinned = false);

	// Resolve path from AssetRegistry by ID, decode, and assign a slot.
	// If already loaded, ensures bPinned is set if requested and returns existing slot.
	uint32_t LoadSound(AssetID id, bool bPinned = false);

	// Resolve by TnxName from AssetRegistry, decode, and assign a slot.
	// If already loaded, ensures bPinned is set if requested and returns existing slot.
	uint32_t LoadSound(TnxName name, bool bPinned = false);

	// Release the PCM data for a slot and unmark it in AssetRegistry.
	// Always unloads regardless of pin status — this is the explicit eviction path.
	void UnloadSound(AssetID id);
	void UnloadSound(TnxName name);

	// --- Slot lookup (delegates to AssetRegistry — same pattern as MeshManager) ---

	// Primary API: look up by TnxName.
	uint32_t FindSlotByTName(TnxName name) const
	{
		const AssetEntry* e = AssetRegistry::Get().FindByTName(name);
		if (!e || e->Type != AssetType::Audio
			|| (static_cast<uint8_t>(e->State) & static_cast<uint8_t>(RuntimeFlags::Loaded)) == 0)
			return UINT32_MAX;
		return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(e->Data));
	}

	// String shim — hashes at call time, delegates to FindSlotByTName.
	uint32_t FindSlotByName(const std::string& name) const
	{
		return FindSlotByTName(TnxName(name.c_str()));
	}

	uint32_t FindSlotByID(AssetID id) const
	{
		const AssetEntry* e = AssetRegistry::Get().Find(id);
		if (!e || e->Type != AssetType::Audio
			|| (static_cast<uint8_t>(e->State) & static_cast<uint8_t>(RuntimeFlags::Loaded)) == 0)
			return UINT32_MAX;
		return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(e->Data));
	}

	const char* GetSlotName(uint32_t slot) const
	{
		if (slot >= SoundCount) return "";
		const AssetEntry* e = AssetRegistry::Get().Find(SlotIDs[slot]);
		return e ? e->Name.GetStr() : "";
	}

	AssetID GetSlotID(uint32_t slot) const
	{
		return (slot < SoundCount) ? SlotIDs[slot] : AssetID{};
	}

	uint32_t GetSoundCount() const { return SoundCount; }

	// --- Audio Events ---
	//
	// Events are name-keyed sound triggers. RegisterEvent() binds a TnxName to an
	// AssetID + default PlayParams. Trigger() resolves the event and plays it.
	// PlayParams overrides are merged at call time: non-default fields win.
	//
	// Usage:
	//   constexpr TnxName kExplosion = TNX_NAME("explosion");
	//   engine->GetAudio()->RegisterEvent(kExplosion, explosionID);
	//   // ... later ...
	//   engine->GetAudio()->Trigger(kExplosion);
	//   engine->GetAudio()->Trigger(kExplosion, {.Volume = 0.5f});

	// Bind a TnxName to an asset + default play parameters.
	// Re-registering the same name updates the entry in place.
	void RegisterEvent(TnxName name, AssetID asset, PlayParams defaults = {});

	// Play the event registered under `name`.
	// Overrides are applied on top of the registered defaults: a non-default (changed)
	// field in `overrides` replaces the corresponding default field.
	// Returns an invalid handle if the event is not found.
	SoundHandle Trigger(TnxName name, PlayParams overrides = {});

	// --- Playback API ---

	// Primary API: play by AssetID. Resolves slot → SoundAsset* internally.
	SoundHandle Play(AssetID id, PlayParams params = {});

	// Immediately stop and recycle the voice.
	void Stop(SoundHandle handle);

	// Linearly ramp volume to 0 over `durationSeconds`, then stop the voice.
	void FadeOut(SoundHandle handle, float durationSeconds);

	// True if the handle still refers to a live, playing voice.
	bool IsPlaying(SoundHandle handle) const;

	// Directly set volume (no fade).
	void SetVolume(SoundHandle handle, float volume);

	// --- Sentinel-driven update ---
	// Call at EngineConfig::AudioUpdateHz from the Sentinel thread.
	// Advances fades, refills looping streams, recycles finished voices.
	void Update(float dt);

private:
	// Internal play from a raw asset pointer — used by LoadSound tests and Play(AssetID).
	SoundHandle PlayAsset(const SoundAsset* asset, PlayParams params);

	// Free PCM for a slot if it's unpinned and no voices are using it.
	void TryAutoUnload(const SoundAsset* asset);

	Voice* FindVoice(SoundHandle handle);
	Voice* AllocateVoice(uint8_t priority);
	void ReleaseVoice(Voice& v);

	// Assign a decoded SoundAsset* into the next free slot and update AssetRegistry
	// Data/State. Does NOT call Register() — caller owns registration.
	uint32_t CommitToSlot(SoundAsset* asset, AssetID id, bool bPinned);

	// ---- Audio event table --------------------------------------------------
	struct AudioEventEntry
	{
		TnxName Name;
		AssetID Asset;
		PlayParams Defaults;
	};

	static constexpr uint32_t MAX_AUDIO_EVENTS = 256;
	AudioEventEntry Events[MAX_AUDIO_EVENTS]{};
	uint32_t EventCount = 0;

	// Find event index by TnxName. Returns UINT32_MAX if not found.
	uint32_t FindEventIndex(TnxName name) const;

	// ---- Asset slots --------------------------------------------------------
	AudioSlot Slots[MAX_AUDIO_SLOTS]{};
	AssetID SlotIDs[MAX_AUDIO_SLOTS]{}; // slot → AssetID reverse map
	uint32_t SoundCount = 0;

	// ---- Voice pool ---------------------------------------------------------
	SDL_AudioDeviceID DeviceID = 0;
	SDL_AudioSpec DeviceSpec{};
	std::unique_ptr<Voice[]> Pool;
	int MaxVoices           = 0;
	uint16_t NextGeneration = 1; // always >= 1; {idx,0} stays Invalid
	bool bInitialized       = false;
};
