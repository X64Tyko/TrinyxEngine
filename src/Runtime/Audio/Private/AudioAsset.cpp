#include "AudioAsset.h"

#include <cstring>
#include <cstdio>
#include <vector>
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_iostream.h>
#include "Logger.h"

// stb_vorbis — single-header implementation unit.
// Suppress all warnings; this is third-party code.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif

#define STB_VORBIS_IMPLEMENTATION
#include "stb_vorbis.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

// ---------------------------------------------------------------------------
// .tnxaudio on-disk format
//
//   Offset  Size  Field
//   0       4     Magic: "TNXA"
//   4       1     Version: 1
//   5       1     InnerFormat: 0=WAV  1=OGG
//   6       2     Reserved (zeroed)
//   8       4     DataSize: payload byte count (little-endian uint32)
//   12      N     Payload: verbatim compressed source bytes
// ---------------------------------------------------------------------------
static constexpr uint32_t kTnxAudioMagic   = 0x41584E54u; // "TNXA" little-endian
static constexpr uint8_t  kTnxAudioVersion = 1;
static constexpr uint8_t  kInnerWAV        = 0;
static constexpr uint8_t  kInnerOGG        = 1;

#pragma pack(push, 1)
struct TnxAudioHeader
{
	uint32_t Magic;
	uint8_t  Version;
	uint8_t  InnerFormat;
	uint16_t Reserved;
	uint32_t DataSize;
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool HasExtension(const char* path, const char* ext)
{
	const char* dot = strrchr(path, '.');
	if (!dot) return false;
	return SDL_strcasecmp(dot, ext) == 0;
}

static std::vector<float> S16ToFloat(const int16_t* src, int sampleCount)
{
	std::vector<float> out(sampleCount);
	constexpr float kInv = 1.f / 32768.f;
	for (int i = 0; i < sampleCount; ++i)
		out[i] = static_cast<float>(src[i]) * kInv;
	return out;
}

static SoundAsset* DecodeWAVFromMemory(const uint8_t* data, uint32_t size, const char* logName)
{
	SDL_IOStream* io = SDL_IOFromConstMem(data, static_cast<size_t>(size));
	if (!io)
	{
		LOG_ENG_ERROR_F("[Audio] SDL_IOFromConstMem failed for '%s': %s", logName, SDL_GetError());
		return nullptr;
	}

	SDL_AudioSpec spec{};
	uint8_t* buf  = nullptr;
	uint32_t len  = 0;

	if (!SDL_LoadWAV_IO(io, true, &spec, &buf, &len))
	{
		LOG_ENG_ERROR_F("[Audio] SDL_LoadWAV_IO failed for '%s': %s", logName, SDL_GetError());
		return nullptr;
	}

	auto* asset       = new SoundAsset();
	asset->Channels   = spec.channels;
	asset->SampleRate = spec.freq;

	if (spec.format == SDL_AUDIO_F32)
	{
		int n = static_cast<int>(len / sizeof(float));
		asset->PCM.assign(reinterpret_cast<float*>(buf), reinterpret_cast<float*>(buf) + n);
	}
	else if (spec.format == SDL_AUDIO_S16)
	{
		int n = static_cast<int>(len / sizeof(int16_t));
		asset->PCM = S16ToFloat(reinterpret_cast<int16_t*>(buf), n);
	}
	else
	{
		SDL_AudioSpec dstSpec{ SDL_AUDIO_F32, spec.channels, spec.freq };
		SDL_AudioStream* cvt = SDL_CreateAudioStream(&spec, &dstSpec);
		if (!cvt)
		{
			LOG_ENG_ERROR_F("[Audio] SDL_CreateAudioStream failed for '%s': %s", logName, SDL_GetError());
			SDL_free(buf);
			delete asset;
			return nullptr;
		}
		SDL_PutAudioStreamData(cvt, buf, static_cast<int>(len));
		SDL_FlushAudioStream(cvt);
		int avail = SDL_GetAudioStreamAvailable(cvt);
		asset->PCM.resize(avail / sizeof(float));
		SDL_GetAudioStreamData(cvt, asset->PCM.data(), avail);
		SDL_DestroyAudioStream(cvt);
	}

	SDL_free(buf);
	asset->Frames = static_cast<int>(asset->PCM.size()) / asset->Channels;
	return asset;
}

static SoundAsset* DecodeOGGFromMemory(const uint8_t* data, uint32_t size, const char* logName)
{
	int channels   = 0;
	int sampleRate = 0;
	short* decoded = nullptr;

	int frameCount = stb_vorbis_decode_memory(data, static_cast<int>(size),
	                                           &channels, &sampleRate, &decoded);
	if (frameCount < 0 || !decoded)
	{
		LOG_ENG_ERROR_F("[Audio] stb_vorbis_decode_memory failed for '%s'", logName);
		return nullptr;
	}

	auto* asset       = new SoundAsset();
	asset->Channels   = channels;
	asset->SampleRate = sampleRate;
	asset->Frames     = frameCount;
	asset->PCM        = S16ToFloat(decoded, frameCount * channels);
	free(decoded);
	return asset;
}

// ---------------------------------------------------------------------------
// LoadSound
// ---------------------------------------------------------------------------

SoundAsset* LoadSound(const char* path)
{
	if (!path || path[0] == '\0')
	{
		LOG_ENG_ERROR("[Audio] LoadSound: null/empty path");
		return nullptr;
	}

	// ---- .tnxaudio (canonical engine format) --------------------------------
	if (HasExtension(path, ".tnxaudio"))
	{
		FILE* f = fopen(path, "rb");
		if (!f)
		{
			LOG_ENG_ERROR_F("[Audio] LoadSound: cannot open '%s'", path);
			return nullptr;
		}

		TnxAudioHeader hdr{};
		if (fread(&hdr, sizeof(hdr), 1, f) != 1
		    || hdr.Magic != kTnxAudioMagic
		    || hdr.Version != kTnxAudioVersion)
		{
			LOG_ENG_ERROR_F("[Audio] LoadSound: invalid .tnxaudio header in '%s'", path);
			fclose(f);
			return nullptr;
		}

		std::vector<uint8_t> payload(hdr.DataSize);
		if (fread(payload.data(), 1, hdr.DataSize, f) != hdr.DataSize)
		{
			LOG_ENG_ERROR_F("[Audio] LoadSound: truncated payload in '%s'", path);
			fclose(f);
			return nullptr;
		}
		fclose(f);

		SoundAsset* asset = nullptr;
		if (hdr.InnerFormat == kInnerWAV)
			asset = DecodeWAVFromMemory(payload.data(), hdr.DataSize, path);
		else if (hdr.InnerFormat == kInnerOGG)
			asset = DecodeOGGFromMemory(payload.data(), hdr.DataSize, path);
		else
		{
			LOG_ENG_ERROR_F("[Audio] LoadSound: unknown inner format %u in '%s'", hdr.InnerFormat, path);
			return nullptr;
		}

		if (asset)
			LOG_ENG_INFO_F("[Audio] Loaded .tnxaudio '%s': %dHz %dch %d frames",
			               path, asset->SampleRate, asset->Channels, asset->Frames);
		return asset;
	}

	// ---- WAV (dev-time convenience path) ------------------------------------
	if (HasExtension(path, ".wav"))
	{
		SDL_AudioSpec spec{};
		uint8_t* buf = nullptr;
		uint32_t len = 0;

		if (!SDL_LoadWAV(path, &spec, &buf, &len))
		{
			LOG_ENG_WARN_F("[Audio] SDL_LoadWAV failed for '%s': %s", path, SDL_GetError());
			return nullptr;
		}

		auto* asset = new SoundAsset();
		asset->Channels   = spec.channels;
		asset->SampleRate = spec.freq;

		if (spec.format == SDL_AUDIO_F32)
		{
			int n = static_cast<int>(len / sizeof(float));
			asset->PCM.assign(reinterpret_cast<float*>(buf), reinterpret_cast<float*>(buf) + n);
		}
		else if (spec.format == SDL_AUDIO_S16)
		{
			int n = static_cast<int>(len / sizeof(int16_t));
			asset->PCM = S16ToFloat(reinterpret_cast<int16_t*>(buf), n);
		}
		else
		{
			SDL_AudioSpec dstSpec{ SDL_AUDIO_F32, spec.channels, spec.freq };
			SDL_AudioStream* cvt = SDL_CreateAudioStream(&spec, &dstSpec);
			if (!cvt)
			{
				LOG_ENG_ERROR_F("[Audio] SDL_CreateAudioStream failed for '%s': %s", path, SDL_GetError());
				SDL_free(buf);
				delete asset;
				return nullptr;
			}
			SDL_PutAudioStreamData(cvt, buf, static_cast<int>(len));
			SDL_FlushAudioStream(cvt);
			int avail = SDL_GetAudioStreamAvailable(cvt);
			asset->PCM.resize(avail / sizeof(float));
			SDL_GetAudioStreamData(cvt, asset->PCM.data(), avail);
			SDL_DestroyAudioStream(cvt);
		}

		SDL_free(buf);
		asset->Frames = static_cast<int>(asset->PCM.size()) / asset->Channels;
		LOG_ENG_INFO_F("[Audio] Loaded WAV '%s': %dHz %dch %d frames",
		               path, asset->SampleRate, asset->Channels, asset->Frames);
		return asset;
	}

	// ---- OGG (dev-time convenience path) ------------------------------------
	if (HasExtension(path, ".ogg"))
	{
		int channels   = 0;
		int sampleRate = 0;
		short* decoded = nullptr;

		int frameCount = stb_vorbis_decode_filename(path, &channels, &sampleRate, &decoded);
		if (frameCount < 0 || !decoded)
		{
			LOG_ENG_ERROR_F("[Audio] stb_vorbis_decode_filename failed for '%s'", path);
			return nullptr;
		}

		auto* asset       = new SoundAsset();
		asset->Channels   = channels;
		asset->SampleRate = sampleRate;
		asset->Frames     = frameCount;
		asset->PCM        = S16ToFloat(decoded, frameCount * channels);
		free(decoded);

		LOG_ENG_INFO_F("[Audio] Loaded OGG '%s': %dHz %dch %d frames",
		               path, asset->SampleRate, asset->Channels, asset->Frames);
		return asset;
	}

	LOG_ENG_ERROR_F("[Audio] LoadSound: unsupported format for '%s' (use .tnxaudio, .wav, or .ogg)", path);
	return nullptr;
}

void FreeSound(SoundAsset* asset)
{
	delete asset;
}

// ---------------------------------------------------------------------------
// ExportTnxAudio
// ---------------------------------------------------------------------------

bool ExportTnxAudio(const char* srcPath, const char* dstPath)
{
	if (!srcPath || !dstPath)
	{
		LOG_ENG_ERROR("[Audio] ExportTnxAudio: null path");
		return false;
	}

	// Determine inner format from source extension.
	uint8_t innerFormat;
	if (HasExtension(srcPath, ".wav"))
		innerFormat = kInnerWAV;
	else if (HasExtension(srcPath, ".ogg"))
		innerFormat = kInnerOGG;
	else
	{
		LOG_ENG_ERROR_F("[Audio] ExportTnxAudio: unsupported source format '%s' (only .wav and .ogg)", srcPath);
		return false;
	}

	// Read source file into memory.
	FILE* src = fopen(srcPath, "rb");
	if (!src)
	{
		LOG_ENG_ERROR_F("[Audio] ExportTnxAudio: cannot open source '%s'", srcPath);
		return false;
	}
	fseek(src, 0, SEEK_END);
	long srcSize = ftell(src);
	rewind(src);
	if (srcSize <= 0 || srcSize > 256 * 1024 * 1024) // sanity: 256 MB max
	{
		LOG_ENG_ERROR_F("[Audio] ExportTnxAudio: implausible file size for '%s'", srcPath);
		fclose(src);
		return false;
	}
	std::vector<uint8_t> payload(static_cast<size_t>(srcSize));
	if (fread(payload.data(), 1, payload.size(), src) != payload.size())
	{
		LOG_ENG_ERROR_F("[Audio] ExportTnxAudio: read error on '%s'", srcPath);
		fclose(src);
		return false;
	}
	fclose(src);

	// Write .tnxaudio file.
	FILE* dst = fopen(dstPath, "wb");
	if (!dst)
	{
		LOG_ENG_ERROR_F("[Audio] ExportTnxAudio: cannot create '%s'", dstPath);
		return false;
	}

	TnxAudioHeader hdr{};
	hdr.Magic       = kTnxAudioMagic;
	hdr.Version     = kTnxAudioVersion;
	hdr.InnerFormat = innerFormat;
	hdr.Reserved    = 0;
	hdr.DataSize    = static_cast<uint32_t>(payload.size());

	bool ok = (fwrite(&hdr, sizeof(hdr), 1, dst) == 1)
	       && (fwrite(payload.data(), 1, payload.size(), dst) == payload.size());
	fclose(dst);

	if (!ok)
		LOG_ENG_ERROR_F("[Audio] ExportTnxAudio: write error on '%s'", dstPath);
	else
		LOG_ENG_INFO_F("[Audio] Exported '%s' → '%s' (%zu bytes)", srcPath, dstPath, payload.size() + sizeof(TnxAudioHeader));

	return ok;
}
