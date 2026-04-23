cmake_minimum_required(VERSION 3.20)

# Force dynamic runtime library for all targets (including Jolt)
# MUST be set before project() call
if(MSVC OR CMAKE_GENERATOR MATCHES "Visual Studio")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# Solution/Workspace level project
project(TrinyxSolution VERSION 0.1 LANGUAGES C CXX)

# -----------------------------------------------------------------------------
# 1. STANDARDS & COMPILER SETTINGS
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Required for third-party static libs when linked into shared libs (Linux).
# Harmless on Windows/MSVC; prevents relocation errors on ELF targets.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# -----------------------------------------------------------------------------
# 2. BUILD OPTIONS
# -----------------------------------------------------------------------------
# Usage: cmake -DENABLE_TRACY=OFF -DGENERATE_ASSEMBLY=ON ..
option(ENABLE_TRACY "Enable Tracy profiler" ON)
option(GENERATE_ASSEMBLY "Generate assembly listings (.cod files)" OFF)
option(VECTORIZATION_REPORTS "Enable compiler vectorization reports" OFF)
option(ENABLE_AVX2 "Enable AVX2 instruction set" ON)
option(TNX_ALIGN_64 "Use 64-byte alignment for field arrays (zero cache line splits, more memory)" OFF)
option(TNX_ENABLE_ROLLBACK "Enable N-frame rollback history. Off = TNX_TEMPORAL_FIELDS components use Volatile (5-frame) storage." OFF)
option(TNX_DETERMINISM "Enable cross-platform determinism: disables FMA/fast-math in the engine and Jolt (JPH_CROSS_PLATFORM_DETERMINISTIC). Automatically forced ON by TNX_ENABLE_ROLLBACK." OFF)
option(TNX_DETAILED_METRICS "Log per-frame latency breakdown (Buffer/Pipeline/Scanout). Noisy." OFF)
if (TNX_DETAILED_METRICS)
    add_compile_definitions(TNX_DETAILED_METRICS)
    message(STATUS "Detailed metrics: ENABLED (noisy FPS + latency logs)")
else ()
    message(STATUS "Detailed metrics: DISABLED")
endif ()
option(TNX_ENABLE_EDITOR "Enable editor UI (ImGui integration). Adds editor panels and overlay." OFF)
option(TNX_ENABLE_NETWORK "Enable networking (GameNetworkingSockets + Protobuf). Required for multiplayer." ON)
# TNX_NET_MODEL selects the networking role baked into the build.
#   PIE    — Editor in-process: one AuthorityNet + N OwnerNet, ticked by PIENetThread.
#            Automatically selected when TNX_ENABLE_EDITOR is ON.
#   Server — Dedicated server: AuthorityNet only. Implies TNX_HEADLESS (no window, no GPU).
#   Client — Standalone client: OwnerNet only.
set(TNX_NET_MODEL "Client" CACHE STRING "Networking role: PIE | Server | Client")
set_property(CACHE TNX_NET_MODEL PROPERTY STRINGS "PIE" "Server" "Client")
option(TNX_TESTING "Enable test harnesses (rollback determinism test, etc.). Debug/profiling tool." OFF)
option(TNX_GPU_PICKING "Enable GPU-based entity picking (click-to-select). Requires editor." OFF)
option(TNX_GPU_PICKING_FAST "Enable fast GPU picking mode (picks every frame at mouse cursor). Requires TNX_GPU_PICKING." OFF)
option(TNX_HEADLESS "Headless build: no window, no GPU/Vulkan. Implied by TNX_NET_MODEL=Server. Also used standalone for CI/CD." OFF)

# Dedicated server implies headless — a server never needs a window or GPU.
if (TNX_NET_MODEL STREQUAL "Server" AND NOT TNX_HEADLESS)
    message(STATUS "Server model detected — forcing TNX_HEADLESS=ON")
    set(TNX_HEADLESS ON CACHE BOOL "" FORCE)
endif ()

if (TNX_HEADLESS)
    add_compile_definitions(TNX_HEADLESS)
    message(STATUS "Headless mode: ENABLED (no window, no GPU)")
    # Headless and editor are mutually exclusive.
    if (TNX_ENABLE_EDITOR)
        message(WARNING "TNX_HEADLESS and TNX_ENABLE_EDITOR are mutually exclusive — disabling editor")
        set(TNX_ENABLE_EDITOR OFF CACHE BOOL "" FORCE)
    endif ()
else ()
    message(STATUS "Headless mode: DISABLED")
endif ()

if (TNX_ENABLE_EDITOR)
    add_compile_definitions(TNX_ENABLE_EDITOR)
    message(STATUS "Editor UI: ENABLED (ImGui)")
    # Editor always implies PIE networking model
    set(TNX_NET_MODEL "PIE" CACHE STRING "" FORCE)
    # PIE server always runs with rollback for server-side reconciliation.
    if (NOT TNX_ENABLE_ROLLBACK)
        message(STATUS "TNX_ENABLE_EDITOR forces TNX_ENABLE_ROLLBACK=ON (PIE server requires rollback)")
        set(TNX_ENABLE_ROLLBACK ON CACHE BOOL "" FORCE)
    endif ()
else ()
    message(STATUS "Editor UI: DISABLED")
endif ()

if (TNX_ENABLE_NETWORK)
    add_compile_definitions(TNX_ENABLE_NETWORK)
    if (TNX_NET_MODEL STREQUAL "PIE")
        add_compile_definitions(TNX_NET_MODEL_PIE)
        message(STATUS "Networking: ENABLED — PIE model (AuthorityNet + OwnerNet)")
    elseif (TNX_NET_MODEL STREQUAL "Server")
        add_compile_definitions(TNX_NET_MODEL_SERVER)
        message(STATUS "Networking: ENABLED — Server model (AuthorityNet)")
    else ()
        add_compile_definitions(TNX_NET_MODEL_CLIENT)
        message(STATUS "Networking: ENABLED — Client model (OwnerNet)")
    endif ()
else ()
    message(STATUS "Networking: DISABLED (standalone builds only)")
endif ()

if (TNX_ENABLE_ROLLBACK)
    add_compile_definitions(TNX_ENABLE_ROLLBACK)
    message(STATUS "Rollback history: ENABLED")
    # Rollback requires bit-identical simulation — force determinism on.
    if (NOT TNX_DETERMINISM)
        message(STATUS "TNX_ENABLE_ROLLBACK forces TNX_DETERMINISM=ON")
        set(TNX_DETERMINISM ON CACHE BOOL "" FORCE)
    endif ()
else ()
    message(STATUS "Rollback history: DISABLED (Temporal fields stored in Volatile slab)")
endif ()

if (TNX_DETERMINISM)
    add_compile_definitions(TNX_DETERMINISM)
    message(STATUS "Determinism: ENABLED (FMA disabled, fp:precise / -ffp-contract=off applied to engine and Jolt)")
else ()
    message(STATUS "Determinism: DISABLED (fast-math allowed)")
endif ()

if (TNX_TESTING)
    add_compile_definitions(TNX_TESTING)
    message(STATUS "Test harnesses: ENABLED")
else ()
    message(STATUS "Test harnesses: DISABLED")
endif ()

if (TNX_ENABLE_EDITOR)
    # Editor always uses GPU picking in FAST mode
    set(TNX_GPU_PICKING ON)
    set(TNX_GPU_PICKING_FAST ON)
    add_compile_definitions(TNX_GPU_PICKING)
    add_compile_definitions(TNX_GPU_PICKING_FAST)
    message(STATUS "GPU Picking: ENABLED (FAST mode - required for editor)")
elseif (TNX_GPU_PICKING)
    set(TNX_GPU_PICKING OFF CACHE BOOL "" FORCE)
else ()
    message(STATUS "GPU Picking: DISABLED")
endif ()

# -----------------------------------------------------------------------------
# 3. GLOBAL PATHS
# -----------------------------------------------------------------------------
set(SDL3_PATH "${CMAKE_SOURCE_DIR}/libs/SDL3")
set(SDL3_SRC_PATH "${CMAKE_SOURCE_DIR}/libs/SDL3-src")
set(TRACY_PATH "${CMAKE_SOURCE_DIR}/libs/tracy")
set(VOLK_PATH  "${CMAKE_SOURCE_DIR}/libs/volk")
set(VMA_PATH   "${CMAKE_SOURCE_DIR}/libs/vma")
set(SLANG_PATH "${CMAKE_SOURCE_DIR}/libs/slang")
set(JOLT_PATH "${CMAKE_SOURCE_DIR}/libs/jolt")
set(IMGUI_PATH "${CMAKE_SOURCE_DIR}/libs/imgui")
set(GNS_PATH "${CMAKE_SOURCE_DIR}/libs/gns")

# Slang runtime API — headers + importable library target.
# Nothing in the engine links it yet; the target exists so rendering code can
# do: target_link_libraries(MyTarget PRIVATE Slang::Compiler)
add_library(Slang::Compiler SHARED IMPORTED GLOBAL
        src/Runtime/Core/Public/TrinyxJobs.h
        src/Runtime/Core/Public/ThreadPinning.h
        src/Runtime/Core/Public/TrinyxRingBuffer.h
        src/Runtime/Core/Public/RegistryTypes.h
        src/Runtime/Core/Public/Events.h
        src/Runtime/Entities/Public/EInstanced.h
        src/Runtime/Entities/Public/EPoint.h
        src/Runtime/Physics/Public/JoltCharacter.h
        src/Runtime/Physics/Public/JoltLayers.h
        src/Runtime/Physics/Private/JoltCharacter.cpp
        src/Runtime/Entities/Public/EPlayer.h
        src/Runtime/Flow/Public/GameState.h)
set_target_properties(Slang::Compiler PROPERTIES
        IMPORTED_LOCATION             "${SLANG_PATH}/lib/libslang-compiler.so"
        INTERFACE_INCLUDE_DIRECTORIES "${SLANG_PATH}/include"
)

# -----------------------------------------------------------------------------
# 3.5 SUBMODULE AUTO-INITIALIZATION
# -----------------------------------------------------------------------------
# Populate Jolt and Tracy if someone did a plain `git clone` without --recurse.
find_package(Git QUIET)
if (GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    set(_missing_submodules "")
    if (NOT EXISTS "${CMAKE_SOURCE_DIR}/libs/tracy/public/TracyClient.cpp")
        list(APPEND _missing_submodules "libs/tracy")
    endif ()
    if (NOT EXISTS "${CMAKE_SOURCE_DIR}/libs/jolt/Build/CMakeLists.txt")
        list(APPEND _missing_submodules "libs/jolt")
    endif ()
    if (_missing_submodules)
        message(STATUS "Initializing submodules: ${_missing_submodules}")
        execute_process(
            COMMAND ${GIT_EXECUTABLE} submodule update --init ${_missing_submodules}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            RESULT_VARIABLE _submodule_result
        )
        if (NOT _submodule_result EQUAL 0)
            message(FATAL_ERROR
                "Failed to initialize submodules: ${_missing_submodules}\n"
                "Run manually: git submodule update --init")
        endif ()
    endif ()
endif ()

# -----------------------------------------------------------------------------
# 4. TRACY CONFIGURATION
# -----------------------------------------------------------------------------
# Tracy is compiled as a single .cpp TU into the Trinyx library target.
# Includes and compile definitions are applied target-scoped in Trinyx/CMakeLists.txt
# so they don't bleed into third-party subprojects.
if(ENABLE_TRACY)
    set(TRACY_PROFILE_LEVEL "3" CACHE STRING "Tracy profiling detail level (1=Coarse, 2=Medium, 3=Fine)")
    set_property(CACHE TRACY_PROFILE_LEVEL PROPERTY STRINGS "1" "2" "3")
    set(TRACY_SOURCES "${TRACY_PATH}/public/TracyClient.cpp")
    message(STATUS "Tracy profiler enabled (Level ${TRACY_PROFILE_LEVEL})")
else()
    set(TRACY_SOURCES "")
    message(STATUS "Tracy profiler disabled")
endif()

# -----------------------------------------------------------------------------
# 5. GLOBAL COMPILER SETTINGS
# -----------------------------------------------------------------------------
if(MSVC)
    # Assembly listing generation
    if(GENERATE_ASSEMBLY)
        add_compile_options(/FAcs)
        message(STATUS "Assembly listings enabled (build/*.cod files)")
    endif()

    # Vectorization reports
    if(VECTORIZATION_REPORTS)
        add_compile_options(/Qvec-report:2)
        message(STATUS "Vectorization reports enabled")
    endif()

    # AVX2 instruction set
    if(ENABLE_AVX2)
        add_compile_options(/arch:AVX2)
        add_compile_definitions(TNX_HAS_AVX2)
        message(STATUS "AVX2 instructions enabled")
    endif()

    # Field array alignment
    if(TNX_ALIGN_64)
        add_compile_definitions(TNX_ALIGN_64)
        message(STATUS "Field array alignment: 64 bytes (zero cache line splits)")
    else()
        message(STATUS "Field array alignment: 32 bytes (half memory overhead)")
    endif()

    # Enable the C++20-compliant preprocessor
    add_compile_options(/Zc:preprocessor)

    # Optional: Ensure __cplusplus macro reports the correct version6
    add_compile_options(/Zc:__cplusplus)

    # Determinism: use fp:precise to suppress FMA fusion on MSVC.
    if (TNX_DETERMINISM)
        add_compile_options(/fp:precise)
    endif ()

else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror -Wno-ignored-attributes -Wno-missing-field-initializers)

    # Assembly listing generation (GCC/Clang)
    if(GENERATE_ASSEMBLY)
        add_compile_options(-save-temps=obj -fverbose-asm)
        message(STATUS "Assembly listings enabled (build/*.s files)")
    endif()

    # Vectorization reports (GCC/Clang)
    if(VECTORIZATION_REPORTS)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            add_compile_options(-Rpass=loop-vectorize -Rpass-missed=loop-vectorize)
        else()
            add_compile_options(-fopt-info-vec-optimized -fopt-info-vec-missed)
        endif()
        message(STATUS "Vectorization reports enabled")
    endif()

    # AVX2 instruction set
    if(ENABLE_AVX2)
        add_compile_options(-march=native)
        add_compile_definitions(TNX_HAS_AVX2)
        message(STATUS "Native architecture instructions enabled")
    endif()

    # Determinism: disable FMA and float contraction so the engine produces
    # bit-identical results across GCC/Clang builds and platforms.
    if (TNX_DETERMINISM)
        add_compile_options(-ffp-contract=off)
    endif ()
endif()

# -----------------------------------------------------------------------------
# 6. JOLT PHYSICS
# -----------------------------------------------------------------------------
# Prefer an already-installed Jolt (vcpkg or system) to avoid duplicate-target
# conflicts. Fall back to the vendored submodule only when nothing is found.
find_package(Jolt QUIET)

if (Jolt_FOUND)
    message(STATUS "Jolt Physics: using installed package")
else ()
    message(STATUS "Jolt Physics: building from source (libs/jolt)")

    # Configure before add_subdirectory so options take effect.
    set(OVERRIDE_CXX_FLAGS          OFF CACHE BOOL "" FORCE)
    set(ENABLE_ALL_WARNINGS         OFF CACHE BOOL "" FORCE)
    set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
    set(ENABLE_INSTALL              OFF CACHE BOOL "" FORCE)
    set(CPP_RTTI_ENABLED            ON  CACHE BOOL "" FORCE)
    set(CPP_EXCEPTIONS_ENABLED      OFF CACHE BOOL "" FORCE)
    set(ENABLE_OBJECT_STREAM        OFF CACHE BOOL "" FORCE)
    set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
    set(PROFILER_IN_DEBUG_AND_RELEASE       OFF CACHE BOOL "" FORCE)

    # Determinism: propagates JPH_CROSS_PLATFORM_DETERMINISTIC via Jolt's PUBLIC interface.
    if (TNX_DETERMINISM)
        set(CROSS_PLATFORM_DETERMINISTIC ON CACHE BOOL "" FORCE)
    endif ()

    add_subdirectory("${JOLT_PATH}/Build" jolt-build)

    set_property(TARGET Jolt PROPERTY CXX_STANDARD 20)
    set_property(TARGET Jolt PROPERTY CXX_STANDARD_REQUIRED ON)

    if (MSVC)
        set_property(TARGET Jolt PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        target_compile_options(Jolt PRIVATE /w)
    else ()
        target_compile_options(Jolt PRIVATE -w)
    endif ()
endif ()

# -----------------------------------------------------------------------------
# 6.5. SDL3 (Linux: build from submodule; Windows: bundled pre-built)
# -----------------------------------------------------------------------------
if (UNIX)
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL3_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    set(SDL_WERROR OFF CACHE BOOL "" FORCE)
    # Headless CI runners have no X11/Wayland — skip the display check.
    # SDL timers, events, and audio still work; only windowing is omitted.
    set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "" FORCE)
    add_subdirectory("${SDL3_SRC_PATH}" SDL3-build EXCLUDE_FROM_ALL)
    # SDL3's libm sources fail our global -Werror — suppress like we do for Jolt
    foreach (_sdl3_target SDL3-shared SDL_uclibc)
        if (TARGET ${_sdl3_target})
            target_compile_options(${_sdl3_target} PRIVATE -w)
        endif ()
    endforeach ()
endif ()

# -----------------------------------------------------------------------------
# 7. PROTOBUF (vendored submodule — required by GameNetworkingSockets)
# -----------------------------------------------------------------------------
if (TNX_ENABLE_NETWORK)
set(ABSL_PROPAGATE_CXX_STD         ON  CACHE BOOL "" FORCE)
set(protobuf_BUILD_TESTS           OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_CONFORMANCE     OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_EXAMPLES        OFF CACHE BOOL "" FORCE)
# NOTE: protobuf_INSTALL must remain ON (the default). The install.cmake it
# guards contains the configure_file() calls that generate protobuf-config.cmake
# into the build tree. Without that file, find_package(Protobuf CONFIG) finds
# nothing and falls back to the system FindProtobuf.cmake, which fails.
set(protobuf_WITH_ZLIB             OFF CACHE BOOL "" FORCE)
set(protobuf_MSVC_STATIC_RUNTIME   OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_SHARED_LIBS     OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_PROTOC_BINARIES ON  CACHE BOOL "" FORCE)
set(protobuf_BUILD_LIBUPB          OFF CACHE BOOL "" FORCE)
# GNS uses protobuf_generate_cpp(), which is defined in protobuf-module.cmake.
# That file is only included in protobuf-config.cmake when MODULE_COMPATIBLE=ON.
set(protobuf_MODULE_COMPATIBLE     ON  CACHE BOOL "" FORCE)

# Save current compile options, then strip project-level warnings so they don't
# leak into protobuf/abseil (abseil uses __int128 which trips -Wpedantic).
get_directory_property(_saved_compile_opts COMPILE_OPTIONS)
set_directory_properties(PROPERTIES COMPILE_OPTIONS "")

add_subdirectory("${CMAKE_SOURCE_DIR}/libs/protobuf" protobuf-build)

# Restore project-level compile options.
set_directory_properties(PROPERTIES COMPILE_OPTIONS "${_saved_compile_opts}")

# Match the DLL runtime for all protobuf targets.
if (MSVC)
    foreach(_pb_tgt libprotobuf libprotobuf-lite libprotoc protoc)
        if (TARGET ${_pb_tgt})
            set_property(TARGET ${_pb_tgt} PROPERTY
                MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
            target_compile_options(${_pb_tgt} PRIVATE /w)
        endif ()
    endforeach()
endif ()

# protobuf-config.cmake unconditionally includes protobuf-targets.cmake, which
# is normally produced only by cmake --install (install(EXPORT protobuf-targets
# ...)). For build-tree usage we write a stub — all protobuf::* targets are
# already defined as ALIAS targets by add_subdirectory above, so no IMPORTED
# target definitions are needed.
set(_pb_cmake_dir "${CMAKE_BINARY_DIR}/protobuf-build/cmake/protobuf")
file(WRITE "${_pb_cmake_dir}/protobuf-targets.cmake"
    "# Stub: all protobuf::* targets were defined as ALIAS targets by\n"
    "# add_subdirectory(libs/protobuf). No IMPORTED targets needed.\n"
)

# Point find_package(Protobuf) to our build-tree config file.
# protobuf_CMAKE_SUBDIR = "cmake/protobuf", so the config lives at
# <binary_dir>/cmake/protobuf/protobuf-config.cmake.
set(Protobuf_DIR "${_pb_cmake_dir}" CACHE PATH "" FORCE)

# By default find_package tries Module mode (FindProtobuf.cmake) first. That
# module raises FATAL_ERROR before Config mode gets a chance — even with
# Protobuf_DIR set. PREFER_CONFIG reverses the order so our Config file runs
# first, before the system FindProtobuf.cmake can abort configure.
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON)

# protobuf-module.cmake (the FindProtobuf compat layer, included when
# protobuf_MODULE_COMPATIBLE=ON) tries to populate Protobuf_LIBRARIES and
# Protobuf_PROTOC_EXECUTABLE by reading LOCATION_* properties from targets.
# Reading LOCATION from non-IMPORTED (build) targets is forbidden in modern
# CMake (CMP0026). Pre-setting these variables triggers the early-exit guard
# in _protobuf_find_libraries() and skips all LOCATION reads.
# Note: protobuf_generate() uses protobuf::protoc directly — it does NOT
# read Protobuf_PROTOC_EXECUTABLE at build time — so any non-empty value works.
set(Protobuf_LIBRARIES        "protobuf::libprotobuf"      CACHE STRING   "" FORCE)
set(Protobuf_LITE_LIBRARIES   "protobuf::libprotobuf-lite" CACHE STRING   "" FORCE)
set(Protobuf_PROTOC_LIBRARIES "protobuf::libprotoc"        CACHE STRING   "" FORCE)
set(Protobuf_PROTOC_EXECUTABLE "$<TARGET_FILE:protoc>"     CACHE FILEPATH "" FORCE)

# -----------------------------------------------------------------------------
# 8. GAMENETWORKINGSOCKETS (vendored, built from source)
# -----------------------------------------------------------------------------
# On Windows use BCrypt (built-in Windows API) — eliminates the OpenSSL
# install requirement entirely. Linux/macOS keep the default (OpenSSL via
# system package manager or vcpkg).
if (WIN32)
    set(USE_CRYPTO "BCrypt" CACHE STRING "GNS crypto backend" FORCE)
endif ()

# Static lib only — we link it into Trinyx, don't ship a shared lib.
set(BUILD_STATIC_LIB        ON  CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIB        OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES          OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS             OFF CACHE BOOL "" FORCE)
set(BUILD_TOOLS             OFF CACHE BOOL "" FORCE)
set(ENABLE_ICE              OFF CACHE BOOL "" FORCE)
set(USE_STEAMWEBRTC         OFF CACHE BOOL "" FORCE)
set(LTO                     OFF CACHE BOOL "" FORCE)
# protobuf is built as a static lib. Tell GNS so it doesn't add PROTOBUF_USE_DLLS,
# which would decorate all symbol references with __declspec(dllimport) and cause
# LNK4286 / LNK1120 unresolved externals when linking against libprotobuf.lib.
set(Protobuf_USE_STATIC_LIBS ON  CACHE BOOL "" FORCE)

add_subdirectory("${GNS_PATH}" gns-build)

# Suppress all warnings on GNS (third-party code).
if (TARGET GameNetworkingSockets_s)
    if (MSVC)
        target_compile_options(GameNetworkingSockets_s PRIVATE /w)
    else ()
        target_compile_options(GameNetworkingSockets_s PRIVATE -w)
    endif ()
endif ()

endif () # TNX_ENABLE_NETWORK

# -----------------------------------------------------------------------------
# 9. SUBDIRECTORIES (Projects in Solution)
# -----------------------------------------------------------------------------
add_subdirectory(Trinyx)
add_subdirectory(Testbed)
add_subdirectory(Playground)
add_subdirectory(DeterminismTest)