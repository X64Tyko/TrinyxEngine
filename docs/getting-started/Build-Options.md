# Build Options

> [← Installation](Installation.md) | [Configuration →](Configuration.md) | [Home](../Home.md)

---

## Quick Reference

### Engine Features

| Option | Default | Purpose |
|---|---|---|
| `TNX_ENABLE_EDITOR` | OFF | ImGui editor, GPU picking, PIE |
| `TNX_ENABLE_ROLLBACK` | OFF | N-frame rollback history (independent of `TNX_DETERMINISM`; forced on by the editor) |
| `TNX_DETERMINISM` | OFF | Cross-platform determinism: Fixed32 math, disables defrag |
| `TNX_ENABLE_NETWORK` | ON | GNS + Protobuf — disable for offline-only builds |
| `TNX_NET_MODEL` | Client | `PIE` / `Server` / `Client` — baked into build |
| `TNX_GPU_PICKING` | OFF | GPU click-to-select (auto-ON with editor) |
| `TNX_GPU_PICKING_FAST` | OFF | Per-frame picking (auto-ON with editor) |
| `TNX_TESTING` | OFF | Rollback determinism test harness (F5 trigger) |
| `TNX_DETAILED_METRICS` | OFF | Per-frame latency breakdown logging |
| `TNX_ALIGN_64` | OFF | 64-byte vs 32-byte field array alignment |

### Profiling & Analysis

| Option | Default | Purpose |
|---|---|---|
| `ENABLE_TRACY` | ON | Tracy profiler integration |
| `TRACY_PROFILE_LEVEL` | 3 | `1`=coarse (~1%), `2`=medium (~5%), `3`=per-entity (~50%+) |
| `ENABLE_AVX2` | ON | `-march=native` on GCC/Clang, `/arch:AVX2` on MSVC |
| `GENERATE_ASSEMBLY` | OFF | Emit `.s` / `.cod` for vectorization inspection |
| `VECTORIZATION_REPORTS` | OFF | Compiler loop-vectorization diagnostics |

---

## Engine Feature Details

### `TNX_ENABLE_EDITOR`

Enables the 8-panel ImGui editor (World Outliner, Details, Content Browser, Engine Stats, Log, Node Script, Component Generator, Debugger). Also enables ImGuizmo gizmo and GPU picking. Forces `TNX_NET_MODEL=PIE`.

When to enable: content authoring, scene editing, PIE testing.

```bash
cmake -B build-editor -DTNX_ENABLE_EDITOR=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

### `TNX_ENABLE_ROLLBACK`

Enables the N-frame rollback ring buffer (Temporal tier) and Jolt physics snapshots for resimulation. Independent of `TNX_DETERMINISM`: without it, resimulation is reproducible on the same machine and binary (enough for server-side reconciliation and editor history); cross-machine bit-identical results also need `TNX_DETERMINISM`, which enables Fixed32 math and `JPH_CROSS_PLATFORM_DETERMINISTIC` on Jolt. Forced on in editor builds.

When disabled: Temporal components are treated as Volatile (3-frame buffer). Games that don't need rollback pay zero memory cost.

```bash
cmake -B build-netcode -DTNX_ENABLE_ROLLBACK=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

### `TNX_DETERMINISM`

Switches `SimFloat` from `float` to `Fixed32` (int32, 0.1mm precision). Disables defrag for live authoritative entities to keep `EntityCacheIndex` stable across rollback windows. Enables deterministic deferred-destruction ordering.

### `TNX_NET_MODEL`

Bakes the networking role into the binary. Three options:

| Value | Role | When |
|---|---|---|
| `PIE` | Authority + Owner in one process (loopback) | Editor builds, auto-set with `TNX_ENABLE_EDITOR=ON` |
| `Server` | Authority-only (headless) | Dedicated server builds |
| `Client` | Owner-only | Default; standard client build |

### `TNX_TESTING`

Enables the rollback determinism test harness. F5 trigger: backup ECS slab + Jolt state, resimulate N frames, `memcmp` result against backup, restore. Used to verify byte-perfect determinism.

### `TNX_ALIGN_64`

| Setting | Alignment | Performance | Memory overhead |
|---|---|---|---|
| OFF (default) | 32-byte | ~0.02–0.18ms penalty at 100K–1M entities (~25% of loads cross cache lines) | ~15 bytes avg padding per field array |
| ON | 64-byte (cache line) | Zero cache line splits | ~31 bytes avg padding per field array (~2MB per 100K entities) |

Use default unless profiling identifies cache line splits as a bottleneck.

---

## Profiling Details

### `TRACY_PROFILE_LEVEL`

| Level | Zones enabled | Overhead |
|---|---|---|
| 1 | `TNX_ZONE_COARSE()` — frame/system level | ~1–2% |
| 2 | + `TNX_ZONE_MEDIUM()` — per-chunk | ~5–10% |
| 3 | + `TNX_ZONE_FINE()` — per-entity | ~50%+ |

Use level 1 or 2 for normal development. Level 3 only when profiling specific hot paths.

### `GENERATE_ASSEMBLY`

| Compiler | Output location |
|---|---|
| MSVC | `build/TrinyxEngine.dir/Debug/TrinyxEngine.cod` |
| GCC/Clang | `build/*.s` files |

Use to verify that hot loops vectorize and to understand register allocation.

---

## Common Configurations

### Editor Development

```bash
cmake -B build-editor \
      -DTNX_ENABLE_EDITOR=ON \
      -DENABLE_TRACY=ON \
      -DTRACY_PROFILE_LEVEL=1 \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-editor
```

### Networked + Rollback

```bash
cmake -B build-netcode \
      -DTNX_ENABLE_ROLLBACK=ON \
      -DENABLE_TRACY=ON \
      -DTRACY_PROFILE_LEVEL=1 \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-netcode
```

### Deep Performance Analysis

```bash
cmake -B build-perf \
      -DENABLE_TRACY=ON \
      -DTRACY_PROFILE_LEVEL=3 \
      -DVECTORIZATION_REPORTS=ON \
      -DGENERATE_ASSEMBLY=ON \
      -DENABLE_AVX2=ON \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-perf
```

### Release

```bash
cmake -B build-release \
      -DENABLE_TRACY=OFF \
      -DENABLE_AVX2=ON \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

---

## IDE Integration

### CLion / Rider

Settings → Build, Execution, Deployment → CMake → CMake options:

```
-DTNX_ENABLE_EDITOR=ON -DENABLE_TRACY=ON -DTRACY_PROFILE_LEVEL=1
```

### VS Code

`.vscode/settings.json`:

```json
{
    "cmake.configureArgs": [
        "-DTNX_ENABLE_EDITOR=ON",
        "-DENABLE_TRACY=ON",
        "-DTRACY_PROFILE_LEVEL=1"
    ]
}
```

### Visual Studio

Project → CMake Settings → check/uncheck options → Ctrl+S to regenerate.

---

## Inspecting Current Options

```bash
# All cache variables
cmake -L build

# Custom options only
cmake -L build | grep -E "TNX_|TRACY|ENABLE|GENERATE"

# Or read directly
cat build/CMakeCache.txt | grep -E "TNX_|TRACY"
```
