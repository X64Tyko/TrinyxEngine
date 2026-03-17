# TrinyxEngine Build Options

> **Navigation:** [← Back to README](../README.md) | [Schema Errors →](SCHEMA_ERROR_EXAMPLES.md)

## Prerequisites

### System Requirements
- **CMake:** 3.20 or newer
- **Compiler:**
  - GCC 10+ / Clang 12+ (Linux)
  - MSVC 2022+ (Windows)
- **C++ Standard:** C++20

### Required Git Submodules

**⚠️ Important:** All dependencies are managed as git submodules. You **must** initialize them before building.

```bash
# If cloning for the first time
git clone --recursive https://github.com/YourRepo/TrinyxEngine.git

# If already cloned without submodules
git submodule update --init --recursive
```

**Submodule versions (pinned to stable releases):**

| Library | Version | Size | Build Time | Purpose |
|---------|---------|------|------------|---------|
| Jolt Physics | v5.5.0 | ~50MB | ~2 min | Physics simulation |
| Tracy | v0.13.1-391 | ~20MB | ~1 min | Performance profiler |
| Dear ImGui | v1.92.6-docking | ~15MB | <1 min | Editor UI (docking branch) |
| ImGuizmo | master | ~5MB | <1 min | 3D gizmo manipulation |
| GameNetworkingSockets | v1.4.0-244 | ~140MB | ~3 min | Networking layer |
| OpenSSL | 3.3.3 (stable) | ~480MB | ~10 min | Crypto for networking |
| Protocol Buffers | v3.29.2 (stable) | ~1.2GB | ~15 min | Serialization |

**Total download:** ~1.8GB
**First build time:** 20-30 minutes (subsequent builds: 1-2 minutes)

**Note:** OpenSSL and Protobuf are large and slow to compile. This is expected. Grab a coffee on first build ☕

**Vendored libraries** (already included, no action needed):
- `libs/SDL3` — Window/input/graphics context
- `libs/volk` — Volk (Vulkan meta-loader)
- `libs/vma` — Vulkan Memory Allocator
- `libs/slang` — Slang shader compiler

---

## How to Toggle Options Without Editing CMakeLists.txt

### Method 1: Command Line (Recommended)

```bash
# Configure with options
cmake -DENABLE_TRACY=ON -DGENERATE_ASSEMBLY=ON -DVECTORIZATION_REPORTS=ON ..

# Build
cmake --build . --config Debug
```

### Method 2: CMake GUI

```bash
# Open CMake GUI
cmake-gui .

# Or if not in build directory:
cmake-gui path/to/TrinyxEngine
```

Then check/uncheck options in the GUI and click "Configure" → "Generate".

### Method 3: CMake TUI (ccmake)

```bash
ccmake .

# Press 't' to toggle advanced options
# Use arrow keys to navigate
# Press Enter to edit values
# Press 'c' to configure
# Press 'g' to generate
```

### Method 4: Cache File Editing

```bash
# Edit build/CMakeCache.txt directly
# Find lines like:
ENABLE_TRACY:BOOL=ON
GENERATE_ASSEMBLY:BOOL=OFF

# Change values and re-run:
cmake ..
```

---

## Available Options

### Engine Feature Flags

#### TNX_ENABLE_EDITOR (default: OFF)
Enable the editor UI with ImGui and GPU-based entity picking.

```bash
# Enable editor
cmake -DTNX_ENABLE_EDITOR=ON ..

# Disable editor (default)
cmake -DTNX_ENABLE_EDITOR=OFF ..
```

**What it does:**
- Enables ImGui docking interface
- Enables GPU picking (automatically sets TNX_GPU_PICKING and TNX_GPU_PICKING_FAST)
- Adds editor panels (World Outliner, Details, Stats)
- Enables ImGuizmo for 3D manipulation

**When to enable:**
- Development and content authoring
- Scene editing and entity inspection

**When to disable:**
- Final release builds
- Headless server builds

---

#### TNX_ENABLE_ROLLBACK (default: OFF)
Enable N-frame rollback history for deterministic netcode.

```bash
# Enable rollback
cmake -DTNX_ENABLE_ROLLBACK=ON ..

# Disable rollback (default)
cmake -DTNX_ENABLE_ROLLBACK=OFF ..
```

**What it does:**
- Temporal components use ring buffer storage (N frames of history)
- Enables rollback/resimulation for network correction
- Increases memory usage per temporal entity

**When to enable:**
- Networked builds with client-side prediction
- Competitive multiplayer games

**When to disable:**
- Single-player games
- Non-deterministic simulations
- When memory is constrained

**Note:** If disabled, Temporal components are stored in Volatile tier (3-frame buffer, no rollback).

---

#### TNX_DETAILED_METRICS (default: OFF)
Log per-frame latency breakdown (Buffer/Pipeline/Scanout).

```bash
# Enable detailed metrics
cmake -DTNX_DETAILED_METRICS=ON ..
```

**What it does:**
- Logs frame timing breakdown every frame
- Shows buffer acquisition, pipeline execution, scanout time

**When to enable:**
- Diagnosing frame pacing issues
- Analyzing GPU/CPU sync points

**Warning:** Very noisy logging, only enable when debugging latency.

---

### Performance and Profiling Options

#### ENABLE_TRACY (default: ON)
Enable Tracy profiler integration.

```bash
# Disable Tracy
cmake -DENABLE_TRACY=OFF ..

# Enable Tracy (default)
cmake -DENABLE_TRACY=ON ..
```

**What it does:**
- Adds `TRACY_ENABLE` define
- Links Tracy client library
- Enables profiling zones (TNX_ZONE macros work)

**When to disable:**
- Final release builds (removes ~1-10% overhead)
- When not actively profiling

---

### TRACY_PROFILE_LEVEL (default: 1)
Controls Tracy profiling detail level.

```bash
# Level 1: Coarse (frame/system level, ~1-2% overhead)
cmake -DTRACY_PROFILE_LEVEL=1 ..

# Level 2: Medium (per-chunk, ~5-10% overhead)
cmake -DTRACY_PROFILE_LEVEL=2 ..

# Level 3: Fine (per-entity, ~50%+ overhead)
cmake -DTRACY_PROFILE_LEVEL=3 ..
```

**What each level includes:**
- Level 1: `TNX_ZONE_COARSE()` only
- Level 2: + `TNX_ZONE_MEDIUM()`
- Level 3: + `TNX_ZONE_FINE()`

**Recommendation:**
- Development: Level 1 or 2
- Deep profiling: Level 3 (expect FPS drop)

---

### GENERATE_ASSEMBLY (default: OFF)
Generate assembly listings for inspection.

```bash
# Enable assembly generation
cmake -DGENERATE_ASSEMBLY=ON ..
```

**Output location:**
- MSVC: `build/TrinyxEngine.dir/Debug/TrinyxEngine.cod`
- GCC/Clang: `build/*.s` files

**What it does:**
- MSVC: Adds `/FAcs` flag (assembly + source + machine code)
- GCC/Clang: Adds `-save-temps=obj -fverbose-asm`

**When to use:**
- Checking if loops are vectorized
- Understanding performance bottlenecks
- Learning assembly optimization

**Warning:** Creates large files, disable for normal development.

---

### VECTORIZATION_REPORTS (default: OFF)
Enable compiler reports about loop vectorization.

```bash
# Enable vectorization reports
cmake -DVECTORIZATION_REPORTS=ON ..
```

**Output:** Build console will show messages like:
```
TrinyxEngine.cpp(230): info: loop vectorized
TrinyxEngine.cpp(245): info: loop not vectorized: complex loop body
```

**What it does:**
- MSVC: Adds `/Qvec-report:2`
- GCC: Adds `-fopt-info-vec-optimized -fopt-info-vec-missed`
- Clang: Adds `-Rpass=loop-vectorize -Rpass-missed=loop-vectorize`

**When to use:**
- Optimizing hot loops
- Understanding why loops aren't vectorizing
- Verifying SIMD optimizations

---

### ENABLE_AVX2 (default: ON)
Enable AVX2 SIMD instruction set.

```bash
# Disable AVX2 (use SSE only)
cmake -DENABLE_AVX2=OFF ..

# Enable AVX2 (default)
cmake -DENABLE_AVX2=ON ..
```

**What it does:**
- MSVC: Adds `/arch:AVX2`
- GCC/Clang: Adds `-march=native`

**Performance impact:**
- 8 floats per instruction vs 4 (SSE)
- ~2x speedup for vectorized code

**When to disable:**
- Compatibility with older CPUs (pre-2013)
- Targeting specific hardware

---

### TNX_ALIGN_64 (default: OFF)
Use 64-byte alignment for field arrays (cache line aligned).

```bash
# Enable 64-byte alignment (zero cache line splits)
cmake -DTNX_ALIGN_64=ON ..

# Use 32-byte alignment (default, lower memory overhead)
cmake -DTNX_ALIGN_64=OFF ..
```

**What it does:**
- OFF (32-byte): Field arrays aligned to 32 bytes (minimum for AVX2)
- ON (64-byte): Field arrays aligned to 64 bytes (cache line aligned)

**Performance impact:**
- 32-byte: ~0.02-0.18ms penalty at 100k-1M entities (~25% of loads cross cache lines)
- 64-byte: Zero cache line splits, maximum performance

**Memory impact:**
- 32-byte: ~15 bytes avg padding per field array
- 64-byte: ~31 bytes avg padding per field array (2x overhead)

**Recommendation:**
- Use default (32-byte) unless profiling shows cache line splits are a bottleneck
- 64-byte adds ~2MB per 100k entities for negligible performance gain

---

## Common Configurations

### Editor Development (Content authoring)
```bash
cmake -B build-editor \
      -DTNX_ENABLE_EDITOR=ON \
      -DENABLE_TRACY=ON \
      -DTRACY_PROFILE_LEVEL=1 \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-editor
```

### Networked Game (Rollback netcode)
```bash
cmake -B build-netcode \
      -DTNX_ENABLE_ROLLBACK=ON \
      -DENABLE_TRACY=ON \
      -DTRACY_PROFILE_LEVEL=1 \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-netcode
```

### Development (Fast iteration, basic profiling)
```bash
cmake -DENABLE_TRACY=ON -DTRACY_PROFILE_LEVEL=1 -DENABLE_AVX2=ON ..
cmake --build . --config Debug
```

### Performance Analysis (See what's happening)
```bash
cmake -DENABLE_TRACY=ON -DTRACY_PROFILE_LEVEL=2 \
      -DVECTORIZATION_REPORTS=ON \
      -DENABLE_AVX2=ON ..
cmake --build . --config Debug
```

### Assembly Investigation (Check vectorization)
```bash
cmake -DGENERATE_ASSEMBLY=ON \
      -DVECTORIZATION_REPORTS=ON \
      -DENABLE_AVX2=ON ..
cmake --build . --config Debug
```

### Deep Profiling (Find hotspots)
```bash
cmake -DENABLE_TRACY=ON -DTRACY_PROFILE_LEVEL=3 -DENABLE_AVX2=ON ..
cmake --build . --config Debug
```

### Maximum Performance (Benchmark/Stress Test)
```bash
cmake -DENABLE_AVX2=ON -DTNX_ALIGN_64=ON ..
cmake --build . --config RelWithDebInfo
```
Note: 64-byte alignment adds ~2MB per 100k entities

### Release Build (Ship it!)
```bash
cmake -DENABLE_TRACY=OFF -DENABLE_AVX2=ON ..
cmake --build . --config Release
```

---

## Build Types

CMake has built-in build types:

```bash
# Debug: No optimization, full debug info
cmake --build . --config Debug

# Release: Full optimization, no debug info
cmake --build . --config Release

# RelWithDebInfo: Optimization + debug info (best for profiling!)
cmake --build . --config RelWithDebInfo

# MinSizeRel: Optimize for size
cmake --build . --config MinSizeRel
```

**Recommended for profiling:** `RelWithDebInfo`
- Get optimized performance
- Still see function names in Tracy
- Can debug if needed

---

## Checking Current Options

```bash
# View all cache variables
cmake -L ..

# View with help text
cmake -LH ..

# View only custom options
cmake -L .. | grep -E "ENABLE|GENERATE|TRACY"
```

Or just read `build/CMakeCache.txt`:
```bash
grep -E "ENABLE_TRACY|GENERATE_ASSEMBLY|VECTORIZATION_REPORTS" CMakeCache.txt
```

---

## Resetting to Defaults

```bash
# Delete cache and reconfigure
rm CMakeCache.txt
cmake ..

# Or nuke the whole build directory
cd ..
rm -rf build
mkdir build
cd build
cmake ..
```

---

## IDE Integration

### Visual Studio
Options appear in the CMake Settings UI:
1. Project → CMake Settings
2. Check/uncheck options
3. Ctrl+S to save and regenerate

### CLion / Rider
Options appear in Settings → Build, Execution, Deployment → CMake:
1. Add to CMake options field:
   ```
   -DENABLE_TRACY=ON -DGENERATE_ASSEMBLY=ON
   ```
2. Reload CMake project

### VS Code
Edit `.vscode/settings.json`:
```json
{
    "cmake.configureArgs": [
        "-DENABLE_TRACY=ON",
        "-DGENERATE_ASSEMBLY=ON",
        "-DVECTORIZATION_REPORTS=ON"
    ]
}
```

---

## Quick Reference Card

### Engine Features
| Option | Default | Use When |
|--------|---------|----------|
| `TNX_ENABLE_EDITOR` | OFF | Content authoring, scene editing |
| `TNX_ENABLE_ROLLBACK` | OFF | Networked multiplayer with prediction |
| `TNX_DETAILED_METRICS` | OFF | Debugging frame pacing |

### Performance & Profiling
| Option | Default | Use When |
|--------|---------|----------|
| `ENABLE_TRACY` | ON | Always (except final release) |
| `TRACY_PROFILE_LEVEL` | 1 | 1=dev, 2=analysis, 3=deep dive |
| `GENERATE_ASSEMBLY` | OFF | Checking vectorization |
| `VECTORIZATION_REPORTS` | OFF | Optimizing loops |
| `ENABLE_AVX2` | ON | Always (unless old CPU) |
| `TNX_ALIGN_64` | OFF | Extreme performance tuning |

**Most common commands:**
```bash
# Editor development
cmake -B build-editor -DTNX_ENABLE_EDITOR=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-editor

# Performance analysis
cmake -DGENERATE_ASSEMBLY=ON -DVECTORIZATION_REPORTS=ON ..
cmake --build . --config Debug
```

---

## Next Steps

1. **Enable vectorization reports** to see if your loop is vectorizing:
   ```bash
   cmake -DVECTORIZATION_REPORTS=ON ..
   cmake --build . --config Debug
   ```

2. **Generate assembly** to confirm:
   ```bash
   cmake -DGENERATE_ASSEMBLY=ON ..
   cmake --build . --config Debug
   # Look at build/TrinyxEngine.dir/Debug/TrinyxEngine.cod
   ```

3. **Profit!** 🚀

---

## Troubleshooting

### Submodules not initialized

**Error:**
```
CMake Error: Could not find git for clone of tracy
```

**Solution:**
```bash
git submodule update --init --recursive
```

### ImGui docking features missing

**Error:**
```
Cannot resolve symbol 'ImGuiConfigFlags_DockingEnable'
```

**Solution:** ImGui must be on the `docking` branch. Verify with:
```bash
cd libs/imgui
git branch
# Should show: * docking
```

If not on docking branch:
```bash
cd libs/imgui
git checkout docking
cd ../..
git add libs/imgui
```

### OpenSSL/Protobuf build takes forever

**Not an error!** OpenSSL (~10 min) and Protobuf (~15 min) are large libraries. First build is slow, subsequent builds are fast.

**Tip:** Use multiple cores to speed up:
```bash
# Linux/macOS
cmake --build build -j$(nproc)

# Windows
cmake --build build --config RelWithDebInfo --parallel
```

### Out of disk space during build

OpenSSL and Protobuf build artifacts are large (~2-3GB). Ensure you have at least **5GB free space** for a full build.

### Wrong compiler version

**Error:**
```
This project requires a C++20 compiler
```

**Solution:** Update your compiler:
- **Linux:** GCC 10+ or Clang 12+
- **Windows:** Visual Studio 2022 or newer

Check version:
```bash
# GCC
g++ --version

# Clang
clang++ --version

# MSVC
cl.exe
```

---

## Getting Help

If you encounter build issues not covered here:

1. Check you ran `git submodule update --init --recursive`
2. Verify compiler version meets requirements (C++20)
3. Ensure at least 5GB disk space available
4. Check [GitHub Issues](https://github.com/YourRepo/TrinyxEngine/issues) for similar problems
