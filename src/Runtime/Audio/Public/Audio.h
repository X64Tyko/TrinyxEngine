#pragma once
#include "AudioHandle.h"
#include "AudioManager.h"
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
namespace Audio
{
	// Set by TrinyxEngine on init/shutdown — not for game code.
	inline void SetManager(AudioManager* mgr) noexcept;
	inline AudioManager* GetManager() noexcept;

	// --- Fire-and-forget play ------------------------------------------------

	inline SoundHandle Play(TnxName name, PlayParams params = {})
	{
		AudioManager* m = GetManager();
		return m ? m->Play(name, params) : SoundHandle::Invalid();
	}

	inline SoundHandle Play(AssetID id, PlayParams params = {})
	{
		AudioManager* m = GetManager();
		return m ? m->Play(id, params) : SoundHandle::Invalid();
	}

	// Play via registered audio event (falls back to direct asset lookup).
	inline SoundHandle Trigger(TnxName name, PlayParams overrides = {})
	{
		AudioManager* m = GetManager();
		return m ? m->Trigger(name, overrides) : SoundHandle::Invalid();
	}

	// --- Handle-based control ------------------------------------------------

	inline void Stop(SoundHandle handle)
	{
		if (AudioManager* m = GetManager()) m->Stop(handle);
	}

	inline void FadeOut(SoundHandle handle, float durationSeconds)
	{
		if (AudioManager* m = GetManager()) m->FadeOut(handle, durationSeconds);
	}

	inline void SetVolume(SoundHandle handle, float volume)
	{
		if (AudioManager* m = GetManager()) m->SetVolume(handle, volume);
	}

	inline bool IsPlaying(SoundHandle handle)
	{
		AudioManager* m = GetManager();
		return m && m->IsPlaying(handle);
	}

	// --- Event registration --------------------------------------------------

	inline void RegisterEvent(TnxName eventName, AssetID asset, PlayParams defaults = {})
	{
		if (AudioManager* m = GetManager()) m->RegisterEvent(eventName, asset, defaults);
	}

	// Resolve asset by name — eventName and assetName must differ.
	inline void RegisterEvent(TnxName eventName, TnxName assetName, PlayParams defaults = {})
	{
		if (AudioManager* m = GetManager()) m->RegisterEvent(eventName, assetName, defaults);
	}

	inline void UpdateEvent(TnxName eventName, PlayParams defaults)
	{
		if (AudioManager* m = GetManager()) m->UpdateEvent(eventName, defaults);
	}

	inline void DeregisterEvent(TnxName eventName)
	{
		if (AudioManager* m = GetManager()) m->DeregisterEvent(eventName);
	}

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
	}

	inline AudioManager* GetManager() noexcept { return Detail::ManagerPtr(); }
}
