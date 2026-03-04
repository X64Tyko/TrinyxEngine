# -----------------------------------------------------------------------------
# Trinyx - Static Library
# -----------------------------------------------------------------------------
project(Trinyx VERSION 0.1 LANGUAGES CXX)

# -----------------------------------------------------------------------------
# VULKAN
# -----------------------------------------------------------------------------
find_package(Vulkan REQUIRED)

# -----------------------------------------------------------------------------
# SOURCE MANAGEMENT
# -----------------------------------------------------------------------------
# Gather all engine source files
file(GLOB_RECURSE ENGINE_SOURCES "${CMAKE_SOURCE_DIR}/src/*.cpp")
file(GLOB_RECURSE ENGINE_HEADERS "${CMAKE_SOURCE_DIR}/src/*.h")

# volk.c provides the Vulkan function loader implementation (compiled once here).
set(VOLK_SOURCES "${VOLK_PATH}/volk.c")

# -----------------------------------------------------------------------------
# LIBRARY DEFINITION
# -----------------------------------------------------------------------------
add_library(${PROJECT_NAME} STATIC
    ${ENGINE_SOURCES}
    ${ENGINE_HEADERS}
    ${TRACY_SOURCES}
    ${VOLK_SOURCES}
        ../src/Runtime/Core/Public/ComponentView.h
)

# Suppress all warnings from Tracy (third-party, not our code)
if(TRACY_SOURCES AND NOT MSVC)
    set_source_files_properties(${TRACY_SOURCES} PROPERTIES
        COMPILE_FLAGS "-w"
    )
endif()

# -----------------------------------------------------------------------------
# FORCE RELWITHDEBINFO FOR ENGINE
# -----------------------------------------------------------------------------
# Always build the engine with optimizations + debug info
set_target_properties(${PROJECT_NAME} PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)

if(MSVC)
    # Remove default Debug flags that conflict with optimization
    #string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    #string(REPLACE "/Od" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    #set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")

    # Apply optimized flags to Debug builds
    #target_compile_options(${PROJECT_NAME} PRIVATE
    #    $<$<CONFIG:Debug>:/O2 /Ob2 /Zi>
    #)

    #target_link_options(${PROJECT_NAME} PRIVATE
    #    $<$<CONFIG:Debug>:/DEBUG:FULL /OPT:REF /OPT:ICF>
    #)
endif()

# -----------------------------------------------------------------------------
# INCLUDES & LINKS
# -----------------------------------------------------------------------------
# A. Engine Public Headers
target_include_directories(${PROJECT_NAME} PUBLIC
    "${CMAKE_SOURCE_DIR}/src/Runtime/Core/Public"
    "${CMAKE_SOURCE_DIR}/src/Runtime/Memory/Public"
    "${CMAKE_SOURCE_DIR}/src/Runtime/Rendering/Public"
    "${CMAKE_SOURCE_DIR}/src/Runtime/Profiling/Public"
    "${CMAKE_SOURCE_DIR}/src/Runtime/Logging/Public"
    "${CMAKE_SOURCE_DIR}/src/Runtime/Components/Public"
    "${CMAKE_SOURCE_DIR}/src/Runtime/Input/Public"
)

# B. SDL3 - Platform specific
if(WIN32)
    # Windows: Use bundled SDL3
    target_include_directories(${PROJECT_NAME} PUBLIC
        "${SDL3_PATH}/include"
    )
    target_link_libraries(${PROJECT_NAME} PUBLIC
        "${SDL3_PATH}/lib/x64/SDL3.lib"
    )
elseif(UNIX)
    # Linux: Use system SDL3
    find_package(SDL3 REQUIRED CONFIG)
    target_link_libraries(${PROJECT_NAME} PUBLIC SDL3::SDL3)
endif()

# C. Vulkan + volk + VMA
target_include_directories(${PROJECT_NAME} SYSTEM PUBLIC
    "${VOLK_PATH}"
    "${VMA_PATH}"
    "${Vulkan_INCLUDE_DIRS}"
)
target_link_libraries(${PROJECT_NAME} PUBLIC Vulkan::Vulkan)

# volk.c must not be compiled with -Werror (upstream code, not ours)
if(NOT MSVC)
    set_source_files_properties("${VOLK_PATH}/volk.c" PROPERTIES
        COMPILE_FLAGS "-Wno-error"
    )
endif()

# Shader output directory baked into the binary as a compile-time constant.
# vulkan.hpp: use dynamic dispatch (compatible with volk global function pointers).
target_compile_definitions(${PROJECT_NAME} PUBLIC
    TNX_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders"
    VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1
)

# ---------------------------------------------------------------------------
# Slang shader compilation
# ---------------------------------------------------------------------------
if(WIN32)
    set(SLANGC "${CMAKE_SOURCE_DIR}/libs/slang/bin/slangc.exe")
else()
    set(SLANGC "${CMAKE_SOURCE_DIR}/libs/slang/bin/slangc")
endif()

# List every .slang shader source here.  Each entry produces a .spv in
# ${CMAKE_SOURCE_DIR}/shaders/ using the same base name.
set(SLANG_SHADER_SOURCES
    "${CMAKE_SOURCE_DIR}/shaders/predicate.slang"
    "${CMAKE_SOURCE_DIR}/shaders/prefix_sum.slang"
    "${CMAKE_SOURCE_DIR}/shaders/scatter.slang"
    "${CMAKE_SOURCE_DIR}/shaders/cube.vert.slang"
    "${CMAKE_SOURCE_DIR}/shaders/cube.frag.slang"
)

# Shared header that all shaders include — listed in DEPENDS so any shader
# rebuilds automatically when GpuFrameData.slang changes.
set(SLANG_COMMON_HEADERS "${CMAKE_SOURCE_DIR}/shaders/GpuFrameData.slang")

set(SLANG_SPIRV_OUTPUTS "")
foreach(SLANG_SRC ${SLANG_SHADER_SOURCES})
    get_filename_component(SHADER_NAME "${SLANG_SRC}" NAME_WE)
    get_filename_component(SHADER_EXT  "${SLANG_SRC}" EXT)       # e.g. .slang or .vert.slang
    # Strip the .slang suffix to get the real extension (e.g. .vert, .frag, empty for compute)
    string(REGEX REPLACE "\\.slang$" "" SHADER_STEM "${SLANG_SRC}")
    get_filename_component(FINAL_NAME "${SHADER_STEM}" NAME)     # e.g. cube.vert, predicate
    set(SPV_OUT "${CMAKE_SOURCE_DIR}/shaders/${FINAL_NAME}.spv")

    # Determine the Slang stage from the file name suffix.
    if("${FINAL_NAME}" MATCHES "\\.vert$")
        set(SLANG_STAGE "-stage vertex")
    elseif("${FINAL_NAME}" MATCHES "\\.frag$")
        set(SLANG_STAGE "-stage fragment")
    else()
        set(SLANG_STAGE "-stage compute")
    endif()

    separate_arguments(SLANG_STAGE_ARGS UNIX_COMMAND "${SLANG_STAGE}")

    add_custom_command(
        OUTPUT  "${SPV_OUT}"
        COMMAND "${SLANGC}"
                ${SLANG_STAGE_ARGS}
                -target spirv
                -emit-spirv-directly
                -I "${CMAKE_SOURCE_DIR}/shaders"
                -o "${SPV_OUT}"
                "${SLANG_SRC}"
        DEPENDS "${SLANG_SRC}" ${SLANG_COMMON_HEADERS}
        COMMENT "slangc ${FINAL_NAME}.slang → ${FINAL_NAME}.spv"
        VERBATIM
    )
    list(APPEND SLANG_SPIRV_OUTPUTS "${SPV_OUT}")
endforeach()

add_custom_target(TrinyxShaders ALL DEPENDS ${SLANG_SPIRV_OUTPUTS})
add_dependencies(${PROJECT_NAME} TrinyxShaders)

# D. Tracy platform-specific libraries
if(ENABLE_TRACY)
    if(WIN32)
        target_link_libraries(${PROJECT_NAME} PUBLIC ws2_32 dbghelp)
    elseif(UNIX)
        target_link_libraries(${PROJECT_NAME} PUBLIC pthread dl)
    endif()
endif()
