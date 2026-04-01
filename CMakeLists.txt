cmake_minimum_required(VERSION 3.30)

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
option(TNX_DETAILED_METRICS "Log per-frame latency breakdown (Buffer/Pipeline/Scanout). Noisy." OFF)
option(TNX_ENABLE_EDITOR "Enable editor UI (ImGui integration). Adds editor panels and overlay." OFF)
option(TNX_TESTING "Enable test harnesses (rollback determinism test, etc.). Debug/profiling tool." OFF)
option(TNX_GPU_PICKING "Enable GPU-based entity picking (click-to-select). Requires editor." OFF)
option(TNX_GPU_PICKING_FAST "Enable fast GPU picking mode (picks every frame at mouse cursor). Requires TNX_GPU_PICKING." OFF)

if (TNX_ENABLE_EDITOR)
    add_compile_definitions(TNX_ENABLE_EDITOR)
    message(STATUS "Editor UI: ENABLED (ImGui)")
else ()
    message(STATUS "Editor UI: DISABLED")
endif ()

if (TNX_ENABLE_ROLLBACK)
    add_compile_definitions(TNX_ENABLE_ROLLBACK)
    message(STATUS "Rollback history: ENABLED")
else ()
    message(STATUS "Rollback history: DISABLED (Temporal fields stored in Volatile slab)")
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
set(SDL3_PATH  "${CMAKE_SOURCE_DIR}/libs/SDL3")
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
        src/Runtime/Core/Public/Events.h)
set_target_properties(Slang::Compiler PROPERTIES
        IMPORTED_LOCATION             "${SLANG_PATH}/lib/libslang-compiler.so"
        INTERFACE_INCLUDE_DIRECTORIES "${SLANG_PATH}/include"
)

# -----------------------------------------------------------------------------
# 4. GLOBAL TRACY CONFIGURATION
# -----------------------------------------------------------------------------
if(ENABLE_TRACY)
    # Tracy profiling level
    # 1 = Coarse (frame/system level, ~1-2% overhead)
    # 2 = Medium (includes per-chunk/subsystem zones, ~5-10% overhead)
    # 3 = Fine (includes per-entity zones, ~50%+ overhead)
    set(TRACY_PROFILE_LEVEL "3" CACHE STRING "Tracy profiling detail level (1=Coarse, 2=Medium, 3=Fine)")
    set_property(CACHE TRACY_PROFILE_LEVEL PROPERTY STRINGS "1" "2" "3")

    # Tracy requires these source files
    set(TRACY_SOURCES
            "${TRACY_PATH}/public/TracyClient.cpp"
    )

    # Add Tracy include directory globally as SYSTEM (silences warnings)
    include_directories(SYSTEM "${TRACY_PATH}/public")

    # Enable Tracy profiling and disable ROCm support
    add_compile_definitions(TRACY_ENABLE)
    add_compile_definitions(TRACY_PROFILE_LEVEL=${TRACY_PROFILE_LEVEL})
    add_compile_definitions(TRACY_ON_DEMAND)
    add_compile_definitions(TRACY_NO_ROCTRACER=1)
    add_compile_definitions(TRACY_NO_ROCPROF=1)

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
        message(STATUS "Native architecture instructions enabled")
    endif()
endif()

# -----------------------------------------------------------------------------
# 6. JOLT PHYSICS (vendored, built from source)
# -----------------------------------------------------------------------------
# Configure Jolt before add_subdirectory so options take effect.
# Disable flags that conflict with our own compiler settings.
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)
set(CPP_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
set(ENABLE_OBJECT_STREAM OFF CACHE BOOL "" FORCE)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)

# When rollback is enabled, turn on Jolt's cross-platform deterministic mode.
# This disables FMA and sets /fp:precise (MSVC) or -ffp-contract=off (Clang)
# so that Jolt produces identical results across rebuild-from-slab resimulations.
if (TNX_ENABLE_ROLLBACK)
    set(CROSS_PLATFORM_DETERMINISTIC ON CACHE BOOL "" FORCE)
endif ()

add_subdirectory("${JOLT_PATH}/Build" jolt-build)

# Force Jolt to use the same runtime library as the rest of the project
if(MSVC)
    set_property(TARGET Jolt PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# Suppress all warnings on Jolt (third-party code, same treatment as Tracy).
if (NOT MSVC)
    target_compile_options(Jolt PRIVATE -w)
endif ()

# -----------------------------------------------------------------------------
# 7. GAMENETWORKINGSOCKETS (vendored, built from source)
# -----------------------------------------------------------------------------
# Static lib only — we link it into Trinyx, don't ship a shared lib.
set(BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ENABLE_ICE OFF CACHE BOOL "" FORCE)
set(USE_STEAMWEBRTC OFF CACHE BOOL "" FORCE)
set(Protobuf_USE_STATIC_LIBS OFF CACHE BOOL "" FORCE)
set(LTO OFF CACHE BOOL "" FORCE)

add_subdirectory("${GNS_PATH}" gns-build)

# Suppress all warnings on GNS (third-party code).
if (TARGET GameNetworkingSockets_s AND NOT MSVC)
    target_compile_options(GameNetworkingSockets_s PRIVATE -w)
endif ()

# On systems where protobuf depends on abseil (e.g. Arch/Manjaro with protobuf 29+),
# CMake's FindProtobuf doesn't propagate the transitive abseil dependency.
# Detect and link it explicitly so the final executable resolves all symbols.
find_package(absl QUIET)
if (absl_FOUND AND TARGET GameNetworkingSockets_s)
    target_link_libraries(GameNetworkingSockets_s PUBLIC absl::log_internal_check_op)
endif ()

# -----------------------------------------------------------------------------
# 8. SUBDIRECTORIES (Projects in Solution)
# -----------------------------------------------------------------------------
add_subdirectory(Trinyx)
add_subdirectory(Testbed)
add_subdirectory(Playground)