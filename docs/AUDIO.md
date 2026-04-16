# Audio System

Baseline that plays sounds and music today; designed as the correct substrate for a
full Wwise-class system and GPU Physics-Based Audio (PBA) synthesis without future
architectural rewrites.

---

## Stack

| Component | Choice | Rationale |
|-----------|--------|-----------|
| Device output | SDL3 `SDL_AudioStream` | Already linked, handles mixing, thread-safe push API |
| WAV decode | SDL3 `SDL_LoadWAV` | Zero-dependency, already available |
| OGG decode | `stb_vorbis.h` (single-header) | Public domain, 30KB, no build system impact |
| GPU PBA | Vulkan compute shader (new pass) | Fits existing 3-pass GPU pipeline; synthesizes modal audio from physics events |

---

## Threading

Audio update runs on **Sentinel thread** (currently 1000Hz input polling).

Rate is configurable in `EngineConfig`:
```cpp
uint32_t AudioUpdateHz = 250; // Default 250Hz — imperceptible fade resolution, cheap
```
Sentinel skips audio tick when `FrameNumber % (SentinelHz / AudioUpdateHz) != 0`.

SDL's internal audio callback thread is the only other audio thread — it pulls from
`SDL_AudioStream` independently and is internally safe.

---

## Key Design: Forward-Compatibility First

### PlayParams (not positional args)
Call sites never break as features are added:
```cpp
struct PlayParams
{
    float    Volume   = 1.f;
    float    Pitch    = 1.f;
    bool     Loop     = false;
    BusID    Bus      = BusID::Master; // stub enum — routing added later
    uint8_t  Priority = 128;           // 0=lowest; voice stealing uses this
};
SoundHandle Play(const SoundAsset* asset, PlayParams params = {});
```
`AudioEvent::Trigger()` will fill a `PlayParams` from RTPC state and forward here.
Existing call sites never change.

### Handle Hierarchy
```
AudioEventHandle  →  { SoundHandle[0..N] }   (future — layered events, N voices)
                            ↓
                    AudioSystem::Voice pool   (this PR)
```
`FadeOut(AudioEventHandle)` fans out to `FadeOut` on each `SoundHandle`.
Reconciler always calls at the event level — one call site regardless of layering.

### SoundHandle
```cpp
struct SoundHandle
{
    uint16_t Index;       // slot in voice pool
    uint16_t Generation;  // stale-handle detection — bumped on Stop/reuse
    bool IsValid() const  { return Index != 0 || Generation != 0; }
    static SoundHandle Invalid() { return {0, 0}; }
};
```

---

## Voice Pool

Size configurable in `EngineConfig`:
```cpp
uint32_t MaxAudioVoices = 64; // default
```

Voice stealing: when pool is full, lowest-priority active voice is evicted.
Ties broken by age (oldest stolen first). Priority stored per-voice, set from `PlayParams`.

```cpp
struct Voice
{
    SDL_AudioStream*   Stream     = nullptr;
    SoundHandle        Handle     = SoundHandle::Invalid();
    const SoundAsset*  Asset      = nullptr;
    std::atomic<float> Volume     = {1.f};
    float              FadeTarget = 1.f;
    float              FadeRate   = 0.f;   // volume/sec; 0 = no fade
    uint8_t            Priority   = 128;
    bool               bLoop      = false;
};
```

---

## GPU Physics-Based Audio (PBA) — Architecture Notes

This is the long game. Trinyx already has:
- Jolt generating per-frame contact events (impact force, contact point, body IDs)
- A Vulkan compute pipeline (predicate → prefix_sum → scatter)
- Material/mesh data on GPU

### What PBA enables
Instead of triggering a pre-baked "metal_impact.wav" on collision, the engine synthesizes
the sound from the physical properties of the colliding objects:
- **Material modal data**: resonant frequencies + decay rates (e.g. steel = high freq, long decay)
- **Collision impulse**: excites those modes at the contact frame
- **Output**: a sum of decaying sinusoids → raw PCM → SDL_AudioStream

Result: 1000 physics objects = 1000 unique, physically correct sounds, no sample library.

### Pipeline
```
Jolt contact events → AudioTriggerBuffer (ring, written by Brain thread)
         ↓
Sentinel AudioSystem::Update → consumes triggers, issues GPU PBA dispatch
         ↓
Vulkan compute: modal_synth.slang
  input:  AudioTriggerBuffer (contact point, impulse magnitude, material IDs)
  input:  MaterialModalData  (per-material: N modes, freq[], decay[], gain[])
  output: PcmSynthBuffer     (float32, 256-sample blocks per trigger, summed)
         ↓
CPU readback (async, 1-frame latency acceptable) → SDL_AudioStream push
```

### Modal synthesis (compute shader sketch)
```glsl
// For each trigger in parallel:
//   synthesize 256 samples = sum over modes of: gain * exp(-decay*t) * sin(2π*freq*t)
// Sum all triggers into output buffer (additive)
```

### Integration with AudioSystem
`AudioSystem` has two parallel paths that **share the same GPU DSP stage**:
1. **Sampled path** — voice pool decodes PCM on CPU → fed into GPU PBA pipeline for DSP processing → SDL_AudioStream
2. **PBA path** — GPU modal synthesis generates PCM → same GPU DSP pipeline → SDL_AudioStream

The GPU PBA pipeline is not synthesis-only. Any sampled audio (music, VO, designed SFX) can be
routed through it to receive geometry-aware echo, material dampening, air absorption, etc.
The CPU-decoded PCM is uploaded to a GPU input buffer alongside synthesized PCM; the DSP pass
processes both together in a single dispatch.

`AudioEvent` decides routing: Contact sounds → PBA synthesis. All voices optionally → GPU DSP
(controlled per-bus or per-event). If PBA is disabled, sampled audio goes direct to SDL_AudioStream
(no GPU DSP), fallback behaviour is seamless.

### PBA data requirements (on Entity/Component)
- `CMaterialAudio` component (cold tier, static per entity type):
  - `uint8_t ModalCount` (max 8 modes per material)
  - `float Frequencies[8]`, `float Decays[8]`, `float Gains[8]`
- Physics contact callback writes `(bodyA, bodyB, impulseMagnitude)` into ring buffer

### PBA considerations
- Output sample rate must match SDL device (48kHz typical) — compute shader parameterised
- Synthesis buffer length = audio callback buffer size (256–1024 samples, SDL negotiated)
- Spatialisation of PBA output: 3D position known from contact point → simple pan+attenuation
  pre-mix before push to stream (same as sampled path)
- Anti-Events on PBA: simply stop pushing PCM to the stream for that trigger ID — no FadeOut
  needed since synthesis is per-block and naturally decays

---

## Feature Parity Target (Wwise + FMOD audit)

Everything below is **future work**. Listed here so architectural decisions today don't
foreclose them.

### Buses & Routing
| Feature | Wwise | FMOD | Notes |
|---------|-------|------|-------|
| Master/Group buses | ✓ | ✓ | `BusID` stub in PlayParams is the hook |
| Auxiliary (send) buses | ✓ | ✓ (Return buses) | Shared reverb/delay |
| VCA (volume group control) | — | ✓ | Control disparate buses without routing |
| Sidechain/ducking | ✓ | ✓ | Music ducks under VO |
| Platform-specific routing | ✓ | ✓ | Per-platform bus override |
| Mixer snapshots | — | ✓ | Save/restore mixer state (e.g. underwater) |

### Parameters / RTPCs
| Feature | Wwise | FMOD | Notes |
|---------|-------|------|-------|
| Float RTPC (continuous) | ✓ | ✓ (continuous param) | Speed → pitch, health → filter |
| Discrete / labeled param | — | ✓ | Named states as parameters |
| RTPC curve editor | ✓ | ✓ | In-editor, maps game value → audio property |
| Timeline parameters | — | ✓ | Sync to event playback progress |
| Built-in spatial params | ✓ | ✓ | Distance, cone, etc. auto-wired |
| LFO / envelope modulation | — | ✓ | Modulate properties without game code |
| Parameter presets | — | ✓ | Share param across events |
| Live RTPC editing | ✓ (2024) | ✓ | Edit curves while connected to live game |

### Switches & States
| Feature | Wwise | FMOD | Notes |
|---------|-------|------|-------|
| Switch (per-object) | ✓ | — | Choose variant by game context (surface type) |
| State (global) | ✓ | — | Game-wide condition (combat, pause) |
| Labeled parameter | — | ✓ | FMOD equivalent of Switch |
| Fade on state change | ✓ | ✓ | Cross-fade between state variants |

### Containers (SFX variation)
| Feature | Wwise | FMOD | Notes |
|---------|-------|------|-------|
| Random container | ✓ | ✓ | Random pick from set |
| Sequence container | ✓ | ✓ | Ordered playback |
| Switch container | ✓ | ✓ | Context-driven selection |
| Blend container | ✓ | — | Cross-fade by RTPC value |
| Avoid repeat | ✓ | — | No same-sound twice in a row |
| Weighted randomisation | ✓ | ✓ | Probability per variant |

### Interactive Music
| Feature | Wwise | FMOD | Notes |
|---------|-------|------|-------|
| Music segments (intro/main/outro) | ✓ | ✓ | Loop points, sync points |
| Music transitions (bar/beat snap) | ✓ | ✓ | Bar-sync transition rules |
| Playlist container | ✓ | — | Ordered/random segment sequences |
| Switch container (music) | ✓ | ✓ | Combat ↔ exploration switch |
| Layered music | ✓ | ✓ | Enable/disable stems by RTPC |
| Custom cue callbacks | ✓ | ✓ | Game code hook at musical beat |
| MIDI-driven music | ✓ | — | Dynamic MIDI for adaptive music |

### Spatial Audio
| Feature | Wwise | FMOD | Notes |
|---------|-------|------|-------|
| 3D positioning + attenuation | ✓ | ✓ | Distance/orientation curves |
| HRTF binaural | ✓ | ✓ (plugin) | Headphone-optimised |
| Room/portal simulation | ✓ | — | Occlusion through architecture |
| Reverb zones (aux sends) | ✓ | ✓ | Environment-driven reverb |
| Air absorption (HF rolloff) | ✓ | ✓ | High-freq loss with distance |
| Directivity patterns | — | ✓ | Source radiation pattern |
| Doppler | ✓ | ✓ | Velocity-driven pitch shift |
| Listener priority | ✓ | ✓ | Which voices get priority near camera |
| Steam Audio integration | ✓ | ✓ | Valve's open-source spatial plugin |
| ADM 3D audio format | ✓ (2024) | — | Broadcast-standard spatial format |
| PBA spatialisation | — | — | **Trinyx native** — contact point is the position |

### DSP Effects
| Feature | FMOD | Wwise (via plugins) |
|---------|------|---------------------|
| Parametric EQ | ✓ | ✓ |
| Compressor / Limiter | ✓ | ✓ |
| Convolution reverb | ✓ | ✓ |
| Algorithmic reverb | ✓ | ✓ |
| Delay / Echo | ✓ | ✓ |
| Chorus / Flanger / Phaser | ✓ | ✓ |
| Pitch shift | ✓ | ✓ |
| Distortion | ✓ | ✓ |
| Multiband dynamics | ✓ | ✓ |
| Spectrum analyser | ✓ | — |
| Custom DSP plugin | ✓ | ✓ |
| Wet/dry automation | ✓ | ✓ |
| GPU DSP (PBA path) | — | — | **Trinyx native** — compute shader |

### Sound Banks & Asset Management
| Feature | Wwise | FMOD | Notes |
|---------|-------|------|-------|
| Sound bank grouping | ✓ | — | Load/unload asset groups |
| Streaming (not fully decoded) | ✓ | ✓ | Large music files |
| Async load | ✓ | ✓ | Non-blocking bank load |
| Memory budgets per bank | ✓ | — | |
| Prefetch (short intro in memory) | ✓ | — | |

### Profiler / Debug
| Feature | Wwise | FMOD |
|---------|-------|------|
| Live voice count | ✓ | ✓ |
| Bus metering | ✓ | ✓ |
| Real-time RTPC values | ✓ | ✓ |
| Voice priority / steal log | ✓ | ✓ |
| Memory usage | ✓ | ✓ |
| Live edit while connected | ✓ (2024) | ✓ |
| Capture / playback | ✓ | ✓ |

---

---

## Baseline Implementation

**Implementation order:**

1. Vendor `stb_vorbis.h` → `libs/stb_vorbis/stb_vorbis.h`
2. `AudioHandle.h` — `SoundHandle` (16+16 bit index/generation)
3. `AudioAsset.h` — `SoundAsset` + `LoadSound` (WAV via SDL, OGG via stb_vorbis)
4. `AudioMixer.h` — internal `Voice` pool struct
5. `AudioSystem.h/.cpp` — public API, `PlayParams`, voice pool, `Update(dt)`
6. CMakeLists — Audio module, stb_vorbis include, SDL3 audio link
7. `EngineConfig` fields — `AudioUpdateHz` (default 250), `MaxAudioVoices` (default 64)
8. Wire `AudioSystem` into World lifetime via `GetAudio()`
9. Wire `AudioSystem::Update(dt)` into Sentinel at configurable rate

## Future Layers (planned order)

1. `AudioBus` — named groups, master volume, send routing
2. `AudioEvent` — name-keyed trigger dispatching to voice pool or PBA path
3. `RTPC` — float param → curve → audio property
4. `Switch` / `State` — per-object and global variant selection
5. Spatial — 3D position + listener, pan/attenuation, Doppler
6. `CMaterialAudio` component + PBA GPU compute pass
7. Interactive Music — segments, transitions, bar-sync
8. DSP effect chain on buses
9. Sound banks + streaming
10. Editor — AudioEvent browser, RTPC curve editor, bus graph, profiler view
