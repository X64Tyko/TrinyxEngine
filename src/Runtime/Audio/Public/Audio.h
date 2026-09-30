#pragma once
#include "AssetTypes.h"
#include "AudioHandle.h"
#include "AudioTypes.h"
#include "TnxName.h"

// ---------------------------------------------------------------------------
// Audio — global fire-and-forget audio API
//
// Wraps AudioManager so gameplay code never needs to grab it from the engine.
//
//   Audio::Play(TNX_NAME("gunshot"));
//   Audio::Play(TNX_NAME("gunshot"), {.Volume = 0.5f});
//   Audio::Trigger(TNX_NAME("explosion"));
//
//   SoundHandle h = Audio::Play(TNX_NAME("music"), {.Loop = true});
//   Audio::FadeOut(h, 1.5f);
//
// All calls are no-ops (return Invalid) if called before engine init or
// after engine shutdown — safe to call from any game code path.
// ---------------------------------------------------------------------------

class AudioManager;

namespace Audio
{
// --- Fire-and-forget play ------------------------------------------------

SoundHandle Play(TnxName name, PlayParams params = {});
SoundHandle Play(AssetID id, PlayParams params = {});

// Play via registered audio event (falls back to direct asset lookup).
SoundHandle Trigger(TnxName name, PlayParams overrides = {});

// --- Handle-based control ------------------------------------------------

void Stop(SoundHandle handle);
void FadeOut(SoundHandle handle, float durationSeconds);
void SetVolume(SoundHandle handle, float volume);
bool IsPlaying(SoundHandle handle);

// --- Event registration --------------------------------------------------

void RegisterEvent(TnxName eventName, AssetID asset, PlayParams defaults = {});

// Resolve asset by name — eventName and assetName must differ.
void RegisterEvent(TnxName eventName, TnxName assetName, PlayParams defaults = {});

void UpdateEvent(TnxName eventName, PlayParams defaults);
void DeregisterEvent(TnxName eventName);

// -------------------------------------------------------------------------
// Shared state — definition lives here so both Audio.h and AudioInternal.h
// reference the same static-local instance across translation units.

namespace Detail
{
inline AudioManager*& ManagerPtr() noexcept
{
	static AudioManager* s_Manager = nullptr;
	return s_Manager;
}
} // namespace Detail

inline AudioManager* GetManager() noexcept
{
	return Detail::ManagerPtr();
}
} // namespace Audio
