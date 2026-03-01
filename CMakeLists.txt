cmake_minimum_required(VERSION 3.30)

# Solution/Workspace level project
project(StrigidSolution VERSION 0.1 LANGUAGES CXX)

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
option(STRIGID_ALIGN_64 "Use 64-byte alignment for field arrays (zero cache line splits, more memory)" OFF)

# -----------------------------------------------------------------------------
# 3. GLOBAL PATHS
# -----------------------------------------------------------------------------
set(SDL3_PATH  "${CMAKE_SOURCE_DIR}/libs/SDL3")
set(TRACY_PATH "${CMAKE_SOURCE_DIR}/libs/tracy")
set(VOLK_PATH  "${CMAKE_SOURCE_DIR}/libs/volk")
set(VMA_PATH   "${CMAKE_SOURCE_DIR}/libs/vma")
set(SLANG_PATH "${CMAKE_SOURCE_DIR}/libs/slang")

# Slang runtime API — headers + importable library target.
# Nothing in the engine links it yet; the target exists so rendering code can
# do: target_link_libraries(MyTarget PRIVATE Slang::Compiler)
add_library(Slang::Compiler SHARED IMPORTED GLOBAL)
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
    if(STRIGID_ALIGN_64)
        add_compile_definitions(STRIGID_ALIGN_64)
        message(STATUS "Field array alignment: 64 bytes (zero cache line splits)")
    else()
        message(STATUS "Field array alignment: 32 bytes (half memory overhead)")
    endif()

    # Enable the C++20-compliant preprocessor
    add_compile_options(/Zc:preprocessor)

    # Optional: Ensure __cplusplus macro reports the correct version
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
# 6. SUBDIRECTORIES (Projects in Solution)
# -----------------------------------------------------------------------------
add_subdirectory(StrigidEngine)
add_subdirectory(Testbed)