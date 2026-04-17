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

enum class BusID : uint8_t { Master = 0 };

struct PlayParams
{
	float Volume     = 1.f;
	float Pitch      = 1.f; // > 1 = faster/higher
	bool Loop        = false;
	BusID Bus        = BusID::Master;
	uint8_t Priority = 128; // 0 = lowest; voice stealing picks smallest value
};

// Fixed heap array — std::atomic<float> is not movable.
struct Voice
{
	SDL_AudioStream* Stream   = nullptr;
	SoundHandle Handle        = SoundHandle::Invalid();
	const SoundAsset* Asset   = nullptr;
	std::atomic<float> Volume = {1.f};
	float FadeTarget          = 0.f;
	float FadeRate            = 0.f; // units/sec; negative = fading out
	uint8_t Priority          = 128;
	bool bLoop                = false;
};

// One SDL audio device, one voice pool, fixed-size asset slot table.
// AssetRegistry is the name/ID authority; AudioManager holds decoded PCM.
// Prefer the Audio:: namespace (Audio.h) over calling this directly.
class AudioManager
{
public:
	static constexpr uint32_t MAX_AUDIO_SLOTS  = 256;
	static constexpr uint32_t MAX_AUDIO_EVENTS = 256;

	struct AudioSlot
	{
		SoundAsset* Asset = nullptr; // owning
		bool bPinned      = false;   // if true, PCM survives playback completion
	};

	AudioManager();
	~AudioManager();
	AudioManager(const AudioManager&)            = delete;
	AudioManager& operator=(const AudioManager&) = delete;

	bool Initialize(int maxVoices = 64);
	void Shutdown();

	// --- Asset loading ---

	// Decode from disk, assign slot, record in AssetRegistry. Returns slot or UINT32_MAX.
	uint32_t LoadSound(const char* path, const std::string& name = {}, AssetID id = {}, bool bPinned = false);
	uint32_t LoadSound(AssetID id, bool bPinned = false);   // resolve path from registry
	uint32_t LoadSound(TnxName name, bool bPinned = false); // resolve by name from registry

	// Explicit eviction — ignores pin status.
	void UnloadSound(AssetID id);
	void UnloadSound(TnxName name);

	// --- Slot lookup ---

	uint32_t FindSlotByTName(TnxName name) const
	{
		const AssetEntry* e = AssetRegistry::Get().FindByTName(name);
		if (!e || e->Type != AssetType::Audio
			|| (static_cast<uint8_t>(e->State) & static_cast<uint8_t>(RuntimeFlags::Loaded)) == 0)
			return UINT32_MAX;
		return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(e->Data));
	}

	uint32_t FindSlotByName(const std::string& name) const { return FindSlotByTName(TnxName(name.c_str())); }

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

	AssetID GetSlotID(uint32_t slot) const { return (slot < SoundCount) ? SlotIDs[slot] : AssetID{}; }
	uint32_t GetSoundCount() const { return SoundCount; }

	// --- Audio events ---
	// Events bind a name to an asset + default PlayParams.
	// Trigger() merges per-call overrides on top of the registered defaults.
	// If no event is registered, Trigger() falls back to a direct asset name lookup.

	// Warns and no-ops if eventName collides with a different audio asset name.
	void RegisterEvent(TnxName eventName, AssetID asset, PlayParams defaults = {});
	void RegisterEvent(TnxName eventName, TnxName assetName, PlayParams defaults = {});

	// Update only the PlayParams of an existing event. No-op if not found.
	void UpdateEvent(TnxName eventName, PlayParams defaults);

	// Remove a registered event. No-op if not found.
	void DeregisterEvent(TnxName eventName);

	// Play an event, merging overrides onto its defaults. Falls back to asset name lookup.
	SoundHandle Trigger(TnxName name, PlayParams overrides = {});

	// --- Playback ---

	SoundHandle Play(AssetID id, PlayParams params = {});
	SoundHandle Play(TnxName name, PlayParams params = {}); // resolves + lazy-loads

	void Stop(SoundHandle handle);
	void FadeOut(SoundHandle handle, float durationSeconds);
	void SetVolume(SoundHandle handle, float volume);
	bool IsPlaying(SoundHandle handle) const;

	// Driven by the Sentinel thread at EngineConfig::AudioUpdateHz.
	void Update(float dt);

private:
	SoundHandle PlayAsset(const SoundAsset* asset, PlayParams params);
	void TryAutoUnload(const SoundAsset* asset);
	Voice* FindVoice(SoundHandle handle);
	Voice* AllocateVoice(uint8_t priority);
	void ReleaseVoice(Voice& v);
	uint32_t CommitToSlot(SoundAsset* asset, AssetID id, bool bPinned);
	void RegisterEventInternal(TnxName eventName, const AssetEntry& asset, PlayParams defaults);

	struct AudioEventEntry
	{
		TnxName Name;
		AssetID Asset;
		PlayParams Defaults;
	};

	AudioEventEntry Events[MAX_AUDIO_EVENTS]{};
	uint32_t EventCount = 0;
	uint32_t FindEventIndex(TnxName name) const;

	AudioSlot Slots[MAX_AUDIO_SLOTS]{};
	AssetID SlotIDs[MAX_AUDIO_SLOTS]{};
	uint32_t SoundCount = 0;

	SDL_AudioDeviceID DeviceID = 0;
	SDL_AudioSpec DeviceSpec{};
	std::unique_ptr<Voice[]> Pool;
	int MaxVoices           = 0;
	uint16_t NextGeneration = 1; // always >= 1; {idx,0} stays Invalid
	bool bInitialized       = false;
};

