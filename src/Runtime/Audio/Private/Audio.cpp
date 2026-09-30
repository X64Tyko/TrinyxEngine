#include "Audio.h"
#include "AudioManager.h"

namespace Audio
{
SoundHandle Play(TnxName name, PlayParams params)
{
	AudioManager* m = GetManager();
	return m ? m->Play(name, params) : SoundHandle::Invalid();
}

SoundHandle Play(AssetID id, PlayParams params)
{
	AudioManager* m = GetManager();
	return m ? m->Play(id, params) : SoundHandle::Invalid();
}

SoundHandle Trigger(TnxName name, PlayParams overrides)
{
	AudioManager* m = GetManager();
	return m ? m->Trigger(name, overrides) : SoundHandle::Invalid();
}

void Stop(SoundHandle handle)
{
	if (AudioManager* m = GetManager()) m->Stop(handle);
}

void FadeOut(SoundHandle handle, float durationSeconds)
{
	if (AudioManager* m = GetManager()) m->FadeOut(handle, durationSeconds);
}

void SetVolume(SoundHandle handle, float volume)
{
	if (AudioManager* m = GetManager()) m->SetVolume(handle, volume);
}

bool IsPlaying(SoundHandle handle)
{
	AudioManager* m = GetManager();
	return m && m->IsPlaying(handle);
}

void RegisterEvent(TnxName eventName, AssetID asset, PlayParams defaults)
{
	if (AudioManager* m = GetManager()) m->RegisterEvent(eventName, asset, defaults);
}

void RegisterEvent(TnxName eventName, TnxName assetName, PlayParams defaults)
{
	if (AudioManager* m = GetManager()) m->RegisterEvent(eventName, assetName, defaults);
}

void UpdateEvent(TnxName eventName, PlayParams defaults)
{
	if (AudioManager* m = GetManager()) m->UpdateEvent(eventName, defaults);
}

void DeregisterEvent(TnxName eventName)
{
	if (AudioManager* m = GetManager()) m->DeregisterEvent(eventName);
}
} // namespace Audio
