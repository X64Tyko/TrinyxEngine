#pragma once
#include <vector>
#include <cstdint>

// Fully decoded audio asset. PCM data is float32 interleaved, normalised to [-1, 1].
// Load via LoadSound(); free via FreeSound().  Both functions are thread-safe (no shared state).
struct SoundAsset
{
	std::vector<float> PCM; // float32 interleaved samples
	int Channels   = 0;
	int SampleRate = 0;
	int Frames     = 0; // total sample frames  (PCM.size() / Channels)
};

// Decode a WAV, OGG, or .tnxaudio file to a heap-allocated SoundAsset.
// .tnxaudio is the canonical engine format (header + compressed payload).
// .wav and .ogg are accepted directly as a dev-time convenience.
// Returns nullptr and logs on failure.
SoundAsset* LoadSound(const char* path);

void FreeSound(SoundAsset* asset);

// Convert a .wav or .ogg source file into a .tnxaudio file at dstPath.
// The .tnxaudio format stores the original compressed bytes verbatim behind a
// small header — no PCM expansion, same file size as the source.
// Returns true on success.
bool ExportTnxAudio(const char* srcPath, const char* dstPath);
