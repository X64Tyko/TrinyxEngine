# TrinyxEngine Build Options

> **Navigation:** [← Back to README](../README.md) | [Schema Errors →](SCHEMA_ERROR_EXAMPLES.md)

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

### ENABLE_TRACY (default: ON)
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

| Option | Default | Use When |
|--------|---------|----------|
| `ENABLE_TRACY` | ON | Always (except final release) |
| `TRACY_PROFILE_LEVEL` | 1 | 1=dev, 2=analysis, 3=deep dive |
| `GENERATE_ASSEMBLY` | OFF | Checking vectorization |
| `VECTORIZATION_REPORTS` | OFF | Optimizing loops |
| `ENABLE_AVX2` | ON | Always (unless old CPU) |

**Most common command:**
```bash
# Development with assembly checking
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
