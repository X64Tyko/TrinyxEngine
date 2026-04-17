#include "AudioManager.h"

#include <algorithm>
#include <SDL3/SDL.h>
#include <SDL3/SDL_audio.h>

#include "AudioAsset.h"
#include "Logger.h"

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

AudioManager::AudioManager() = default;
AudioManager::~AudioManager() { Shutdown(); }

bool AudioManager::Initialize(int maxVoices)
{
	if (bInitialized) return true;

	if (maxVoices <= 0)
	{
		LOG_ENG_ERROR("[Audio] AudioManager::Initialize: maxVoices must be > 0");
		return false;
	}

	// ---- Open default playback device ----------------------------------------
	// Request float32 stereo 48kHz; SDL will give us the closest available.
	SDL_AudioSpec desired{SDL_AUDIO_F32, 2, 48000};
	DeviceID = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired);
	if (DeviceID == 0)
	{
		LOG_ENG_ERROR_F("[Audio] SDL_OpenAudioDevice failed: %s", SDL_GetError());
		// Non-fatal: audio just won't play.  System marks itself not-initialized so
		// callers can continue without audio rather than crashing.
		return false;
	}

	// Query the actual format the device was opened with.
	int sampleFrames = 0;
	SDL_GetAudioDeviceFormat(DeviceID, &DeviceSpec, &sampleFrames);

	LOG_ENG_INFO_F("[Audio] Device opened: format=%d channels=%d freq=%d (buffer ~%d frames)",
				   DeviceSpec.format, DeviceSpec.channels, DeviceSpec.freq, sampleFrames);

	// ---- Allocate voice pool ------------------------------------------------
	MaxVoices = maxVoices;
	Pool      = std::make_unique<Voice[]>(maxVoices);

	bInitialized = true;
	LOG_ENG_INFO_F("[Audio] AudioManager initialized (%d voice slots)", MaxVoices);
	return true;
}

void AudioManager::Shutdown()
{
	if (!bInitialized) return;

	// Stop and release all active voices.
	for (int i = 0; i < MaxVoices; ++i)
	{
		if (Pool[i].Handle.IsValid()) ReleaseVoice(Pool[i]);
	}

	if (DeviceID)
	{
		SDL_CloseAudioDevice(DeviceID);
		DeviceID = 0;
	}

	Pool.reset();
	MaxVoices = 0;

	// Free all loaded sound assets.
	for (uint32_t i = 0; i < SoundCount; ++i)
	{
		FreeSound(Slots[i].Asset);
		Slots[i].Asset = nullptr;
	}
	SoundCount = 0;

	bInitialized = false;
	LOG_ENG_INFO("[Audio] AudioManager shut down");
}

uint32_t AudioManager::CommitToSlot(SoundAsset* asset, AssetID id, bool bPinned)
{
	uint32_t slotID = UINT32_MAX;
	for (uint32_t i = 0; i < SoundCount; ++i)
	{
		if (Slots[i].Asset == nullptr)
		{
			slotID = i;
			break;
		}
	}
	if (slotID == UINT32_MAX)
	{
		if (SoundCount >= MAX_AUDIO_SLOTS)
		{
			LOG_ENG_ERROR("[Audio] CommitToSlot: audio slot limit reached");
			return UINT32_MAX;
		}
		slotID = SoundCount++;
	}

	Slots[slotID].Asset   = asset;
	Slots[slotID].bPinned = bPinned;
	SlotIDs[slotID]       = id;

	// Claim: register slot → UUID mapping immediately so CheckinBySlot can find
	// a pending checkout even if the entity is despawned before data is ready.
	if (id.IsValid()) AssetRegistry::Get().RegisterSlot(AssetType::Audio, slotID, id);

	if (id.IsValid())
	{
		if (AssetEntry* e = AssetRegistry::Get().FindMutable(id))
		{
			e->Data  = reinterpret_cast<void*>(static_cast<uintptr_t>(slotID));
			e->State = RuntimeFlags::Loaded;
			e->OnLoaded(slotID);
			e->OnLoaded.Reset();
		}
	}

	return slotID;
}

// ---------------------------------------------------------------------------
// Asset loading
// ---------------------------------------------------------------------------

uint32_t AudioManager::LoadSound(const char* path, const std::string& name, AssetID id, bool bPinned)
{
	SoundAsset* asset = ::LoadSound(path);
	if (!asset)
	{
		LOG_ENG_ERROR_F("[Audio] LoadSound: failed to decode '%s'", path ? path : "(null)");
		return UINT32_MAX;
	}

	if (id.IsValid()) AssetRegistry::Get().Register(id, name, path, AssetType::Audio);

	uint32_t slotID = CommitToSlot(asset, id, bPinned);
	if (slotID == UINT32_MAX)
	{
		FreeSound(asset);
		return UINT32_MAX;
	}

	LOG_ENG_INFO_F("[Audio] Loaded sound slot %u '%s' (%d frames, %dHz %dch)",
				   slotID, name.empty() ? path : name.c_str(),
				   asset->Frames, asset->SampleRate, asset->Channels);
	return slotID;
}

uint32_t AudioManager::LoadSound(AssetID id, bool bPinned)
{
	// If already loaded, ensure pin status and return existing slot.
	uint32_t slot = FindSlotByID(id);
	if (slot != UINT32_MAX)
	{
		if (bPinned) Slots[slot].bPinned = true;
		return slot;
	}

	const AssetEntry* entry = AssetRegistry::Get().Find(id);
	if (!entry || entry->Type != AssetType::Audio)
	{
		LOG_ENG_ERROR("[Audio] LoadSound: AssetID not in registry");
		return UINT32_MAX;
	}

	std::string path = AssetRegistry::Get().ResolvePath(id);
	if (path.empty())
	{
		LOG_ENG_ERROR("[Audio] LoadSound: no resolvable path for AssetID");
		return UINT32_MAX;
	}

	slot = CommitToSlot(::LoadSound(path.c_str()), id, bPinned);
	if (slot == UINT32_MAX)
		LOG_ENG_ERROR_F("[Audio] LoadSound: failed to decode '%s'", path.c_str());
	return slot;
}

uint32_t AudioManager::LoadSound(TnxName name, bool bPinned)
{
	const AssetEntry* entry = AssetRegistry::Get().FindByTName(name);
	if (!entry || entry->Type != AssetType::Audio)
	{
		LOG_ENG_ERROR_F("[Audio] LoadSound: TnxName '%s' not in registry", name.GetStr());
		return UINT32_MAX;
	}
	return LoadSound(entry->ID, bPinned);
}

void AudioManager::UnloadSound(AssetID id)
{
	uint32_t slot = FindSlotByID(id);
	if (slot == UINT32_MAX) return;

	// Stop any voices playing this asset.
	for (int i = 0; i < MaxVoices; ++i)
	{
		if (Pool[i].Handle.IsValid() && Pool[i].Asset == Slots[slot].Asset) ReleaseVoice(Pool[i]);
	}

	FreeSound(Slots[slot].Asset);
	Slots[slot].Asset   = nullptr;
	Slots[slot].bPinned = false;

	if (AssetEntry* e = AssetRegistry::Get().FindMutable(id))
	{
		e->Data  = nullptr;
		e->State = RuntimeFlags::None;
	}
}

void AudioManager::UnloadSound(TnxName name)
{
	const AssetEntry* entry = AssetRegistry::Get().FindByTName(name);
	if (entry && entry->Type == AssetType::Audio) UnloadSound(entry->ID);
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

Voice* AudioManager::FindVoice(SoundHandle handle)
{
	if (!handle.IsValid() || handle.Index >= static_cast<uint16_t>(MaxVoices)) return nullptr;
	Voice& v = Pool[handle.Index];
	return (v.Handle == handle) ? &v : nullptr;
}

Voice* AudioManager::AllocateVoice(uint8_t priority)
{
	if (MaxVoices == 0) return nullptr;

	// Fast path: find a free slot.
	for (int i = 0; i < MaxVoices; ++i)
	{
		if (!Pool[i].Handle.IsValid()) return &Pool[i];
	}

	// Pool full — steal the lowest-priority voice; only steal if its priority <= requester's.
	// Ties broken by lowest index (oldest slot).
	Voice* victim = nullptr;
	for (int i = 0; i < MaxVoices; ++i)
	{
		if (!victim || Pool[i].Priority < victim->Priority) victim = &Pool[i];
	}

	if (victim && victim->Priority <= priority)
	{
		ReleaseVoice(*victim);
		return victim;
	}

	return nullptr; // All voices have higher priority — drop this sound.
}

void AudioManager::ReleaseVoice(Voice& v)
{
	if (v.Stream)
	{
		SDL_UnbindAudioStream(v.Stream);
		SDL_DestroyAudioStream(v.Stream);
		v.Stream = nullptr;
	}
	v.Asset    = nullptr;
	v.bLoop    = false;
	v.FadeRate = 0.f;
	v.Volume.store(1.f, std::memory_order_relaxed);
	v.Handle = SoundHandle::Invalid();
}

// ---------------------------------------------------------------------------
// Playback API
// ---------------------------------------------------------------------------

SoundHandle AudioManager::Play(TnxName name, PlayParams params)
{
	const AssetEntry* entry = AssetRegistry::Get().FindByTName(name);
	if (!entry || entry->Type != AssetType::Audio)
	{
		LOG_ENG_ERROR_F("[Audio] Play: TnxName '%s' not found in registry", name.GetStr());
		return SoundHandle::Invalid();
	}
	return Play(entry->ID, params);
}

SoundHandle AudioManager::Play(AssetID id, PlayParams params)
{
	uint32_t slot = FindSlotByID(id);

	// Lazy load: asset is registered in AssetRegistry but not yet decoded into a slot.
	if (slot == UINT32_MAX)
	{
		const AssetEntry* entry = AssetRegistry::Get().Find(id);
		if (!entry || entry->Type != AssetType::Audio)
		{
			LOG_ENG_ERROR("[Audio] Play: AssetID not registered");
			return SoundHandle::Invalid();
		}

		std::string path = AssetRegistry::Get().ResolvePath(id);
		if (path.empty())
		{
			LOG_ENG_ERROR("[Audio] Play: no resolvable path for AssetID");
			return SoundHandle::Invalid();
		}

		SoundAsset* asset = ::LoadSound(path.c_str());
		if (!asset)
		{
			LOG_ENG_ERROR_F("[Audio] Play: failed to lazy-load '%s'", path.c_str());
			return SoundHandle::Invalid();
		}

		slot = CommitToSlot(asset, id, /*bPinned=*/false);
		if (slot == UINT32_MAX)
		{
			FreeSound(asset);
			LOG_ENG_ERROR_F("[Audio] Play: slot commit failed for '%s'", path.c_str());
			return SoundHandle::Invalid();
		}
	}

	return PlayAsset(Slots[slot].Asset, params);
}

SoundHandle AudioManager::PlayAsset(const SoundAsset* asset, PlayParams params)
{
	if (!bInitialized || !asset || asset->PCM.empty()) return SoundHandle::Invalid();

	Voice* v = AllocateVoice(params.Priority);
	if (!v)
	{
		LOG_ENG_INFO("[Audio] Voice pool exhausted and no stealable voice — dropping sound");
		return SoundHandle::Invalid();
	}

	// Build source spec matching the decoded asset.
	SDL_AudioSpec srcSpec{SDL_AUDIO_F32, asset->Channels, asset->SampleRate};

	v->Stream = SDL_CreateAudioStream(&srcSpec, &DeviceSpec);
	if (!v->Stream)
	{
		LOG_ENG_ERROR_F("[Audio] SDL_CreateAudioStream failed: %s", SDL_GetError());
		return SoundHandle::Invalid();
	}

	// Bind to device — SDL will mix this stream into the output automatically.
	SDL_BindAudioStream(DeviceID, v->Stream);

	// Apply initial gain and pitch.
	SDL_SetAudioStreamGain(v->Stream, params.Volume);
	if (params.Pitch != 1.f) SDL_SetAudioStreamFrequencyRatio(v->Stream, params.Pitch);

	// Push the entire PCM buffer.
	SDL_PutAudioStreamData(v->Stream,
						   asset->PCM.data(),
						   static_cast<int>(asset->PCM.size() * sizeof(float)));

	// Assign handle.
	const uint16_t gen = NextGeneration++;
	if (NextGeneration == 0) NextGeneration = 1; // wrap: skip 0 so {idx,0} stays invalid

	const uint16_t idx = static_cast<uint16_t>(v - Pool.get());
	v->Handle          = {idx, gen};
	v->Asset           = asset;
	v->bLoop           = params.Loop;
	v->Priority        = params.Priority;
	v->Volume.store(params.Volume, std::memory_order_relaxed);
	v->FadeTarget = 0.f;
	v->FadeRate   = 0.f;

	return v->Handle;
}

void AudioManager::Stop(SoundHandle handle)
{
	Voice* v = FindVoice(handle);
	if (v) ReleaseVoice(*v);
}

void AudioManager::FadeOut(SoundHandle handle, float durationSeconds)
{
	if (durationSeconds <= 0.f)
	{
		Stop(handle);
		return;
	}
	Voice* v = FindVoice(handle);
	if (!v) return;

	v->FadeTarget = 0.f;
	v->FadeRate   = -(v->Volume.load(std::memory_order_relaxed) / durationSeconds);
}

bool AudioManager::IsPlaying(SoundHandle handle) const
{
	if (!handle.IsValid() || handle.Index >= static_cast<uint16_t>(MaxVoices)) return false;
	return Pool[handle.Index].Handle == handle;
}

void AudioManager::SetVolume(SoundHandle handle, float volume)
{
	Voice* v = FindVoice(handle);
	if (!v) return;
	volume = std::clamp(volume, 0.f, 1.f);
	v->Volume.store(volume, std::memory_order_relaxed);
	SDL_SetAudioStreamGain(v->Stream, volume);
}

// ---------------------------------------------------------------------------
// Sentinel-driven update
// ---------------------------------------------------------------------------

void AudioManager::Update(float dt)
{
	if (!bInitialized) return;

	for (int i = 0; i < MaxVoices; ++i)
	{
		Voice& v = Pool[i];
		if (!v.Handle.IsValid()) continue;

		// ---- Advance fade ---------------------------------------------------
		if (v.FadeRate != 0.f)
		{
			float vol = v.Volume.load(std::memory_order_relaxed);
			vol       += v.FadeRate * dt;

			if (vol <= v.FadeTarget)
			{
				// Fade complete.
				ReleaseVoice(v);
				continue;
			}

			vol = std::clamp(vol, 0.f, 1.f);
			v.Volume.store(vol, std::memory_order_relaxed);
			SDL_SetAudioStreamGain(v.Stream, vol);
		}

		// ---- Looping: refill when empty -------------------------------------
		// TODO(streaming): this is whole-file refill — fine for SFX, not for music.
		// Music needs chunked stb_vorbis decode pushed in Update() so the full PCM
		// is never in RAM. SDL_GetAudioStreamQueued threshold + a per-voice read
		// cursor is the hook point; the SoundAsset would carry a vorbis_file* instead
		// of a decoded PCM vector for streaming assets.
		if (SDL_GetAudioStreamQueued(v.Stream) == 0)
		{
			if (v.bLoop)
			{
				SDL_PutAudioStreamData(v.Stream,
									   v.Asset->PCM.data(),
									   static_cast<int>(v.Asset->PCM.size() * sizeof(float)));
			}
			else
			{
				// Non-looping and finished — auto-unload slot if not pinned and no other voice uses it.
				const SoundAsset* finishedAsset = v.Asset;
				ReleaseVoice(v);
				TryAutoUnload(finishedAsset);
			}
		}
	}
}

void AudioManager::TryAutoUnload(const SoundAsset* asset)
{
	if (!asset) return;

	// Find which slot owns this asset.
	uint32_t slot = UINT32_MAX;
	for (uint32_t i = 0; i < SoundCount; ++i)
	{
		if (Slots[i].Asset == asset)
		{
			slot = i;
			break;
		}
	}
	if (slot == UINT32_MAX || Slots[slot].bPinned) return;

	// Check if any live voice is still using this asset.
	for (int i = 0; i < MaxVoices; ++i)
	{
		if (Pool[i].Handle.IsValid() && Pool[i].Asset == asset) return;
	}

	// Safe to unload.
	LOG_ENG_INFO_F("[Audio] Auto-unloading slot %u (unpinned, no active voices)", slot);
	FreeSound(Slots[slot].Asset);
	Slots[slot].Asset   = nullptr;
	Slots[slot].bPinned = false;

	if (AssetEntry* e = AssetRegistry::Get().FindMutable(SlotIDs[slot]))
	{
		e->Data  = nullptr;
		e->State = RuntimeFlags::None;
	}
}


// ---------------------------------------------------------------------------
// Audio Event table
// ---------------------------------------------------------------------------

uint32_t AudioManager::FindEventIndex(TnxName name) const
{
	for (uint32_t i = 0; i < EventCount; ++i)
	{
		if (Events[i].Name == name) return i;
	}
	return UINT32_MAX;
}

void AudioManager::RegisterEventInternal(TnxName eventName, const AssetEntry& asset, PlayParams defaults)
{
	// Fail if already registered — use UpdateEvent to change defaults.
	if (FindEventIndex(eventName) != UINT32_MAX)
	{
		LOG_ENG_WARN_F("[Audio] RegisterEvent: '%s' already registered — use UpdateEvent to change defaults", eventName.GetStr());
		return;
	}

	// Event name must not shadow a different audio asset — Trigger's fallback would be ambiguous.
	const AssetEntry* clash = AssetRegistry::Get().FindByTName(eventName);
	if (clash && clash->Type == AssetType::Audio && clash->ID != asset.ID)
	{
		LOG_ENG_WARN_F("[Audio] RegisterEvent: event name '%s' collides with audio asset '%s' — rename the event", eventName.GetStr(), clash->Name.GetStr());
		return;
	}

	if (EventCount >= MAX_AUDIO_EVENTS)
	{
		LOG_ENG_WARN("[Audio] RegisterEvent: event table full");
		return;
	}

	Events[EventCount++] = {eventName, asset.ID, defaults};
}

void AudioManager::RegisterEvent(TnxName eventName, AssetID id, PlayParams defaults)
{
	// Fail if already registered — use UpdateEvent to change defaults.
	if (FindEventIndex(eventName) != UINT32_MAX)
	{
		LOG_ENG_WARN_F("[Audio] RegisterEvent: '%s' already registered — use UpdateEvent to change defaults", eventName.GetStr());
		return;
	}

	const AssetEntry* entry = AssetRegistry::Get().Find(id);
	if (!entry || entry->Type != AssetType::Audio)
	{
		LOG_ENG_ERROR_F("[Audio] RegisterEvent: AssetID not found in registry for event '%s'", eventName.GetStr());
		return;
	}
	RegisterEventInternal(eventName, *entry, defaults);
}

void AudioManager::RegisterEvent(TnxName eventName, TnxName assetName, PlayParams defaults)
{
	// Fail if already registered — use UpdateEvent to change defaults.
	if (FindEventIndex(eventName) != UINT32_MAX)
	{
		LOG_ENG_WARN_F("[Audio] RegisterEvent: '%s' already registered — use UpdateEvent to change defaults", eventName.GetStr());
		return;
	}
	
	const AssetEntry* entry = AssetRegistry::Get().FindByTName(assetName);
	if (!entry || entry->Type != AssetType::Audio)
	{
		LOG_ENG_ERROR_F("[Audio] RegisterEvent: asset '%s' not found in registry", assetName.GetStr());
		return;
	}

	if (eventName == assetName)
	{
		LOG_ENG_WARN_F("[Audio] RegisterEvent: event name '%s' is identical to its asset name — Trigger resolves assets by name without a registered event", eventName.GetStr());
		return;
	}

	RegisterEventInternal(eventName, *entry, defaults);
}

void AudioManager::UpdateEvent(TnxName eventName, PlayParams defaults)
{
	const uint32_t idx = FindEventIndex(eventName);
	if (idx == UINT32_MAX)
	{
		LOG_ENG_WARN_F("[Audio] UpdateEvent: no event registered for '%s'", eventName.GetStr());
		return;
	}
	Events[idx].Defaults = defaults;
}

void AudioManager::DeregisterEvent(TnxName eventName)
{
	const uint32_t idx = FindEventIndex(eventName);
	if (idx == UINT32_MAX) return;

	// Swap with last to keep the array packed.
	Events[idx] = Events[--EventCount];
}

SoundHandle AudioManager::Trigger(TnxName name, PlayParams overrides)
{
	const uint32_t idx = FindEventIndex(name);
	if (idx == UINT32_MAX)
	{
		// No registered event — fall back to direct asset lookup by name.
		// Allows Trigger(TnxName) without a prior RegisterEvent call.
		const AssetEntry* entry = AssetRegistry::Get().FindByTName(name);
		if (!entry || entry->Type != AssetType::Audio)
		{
			LOG_ENG_WARN_F("[Audio] Trigger: no event or asset found for '%s'", name.GetStr());
			return SoundHandle::Invalid();
		}
		return Play(entry->ID, overrides);
	}

	const AudioEventEntry& ev = Events[idx];

	// Merge overrides on top of defaults. PlayParams fields use their zero-value as
	// "not overridden", so we compare against the default-constructed defaults.
	PlayParams merged = ev.Defaults;
	const PlayParams kDefault{};
	if (overrides.Volume != kDefault.Volume) merged.Volume = overrides.Volume;
	if (overrides.Pitch != kDefault.Pitch) merged.Pitch = overrides.Pitch;
	if (overrides.Loop != kDefault.Loop) merged.Loop = overrides.Loop;
	if (overrides.Bus != kDefault.Bus) merged.Bus = overrides.Bus;
	if (overrides.Priority != kDefault.Priority) merged.Priority = overrides.Priority;

	return Play(ev.Asset, merged);
}
