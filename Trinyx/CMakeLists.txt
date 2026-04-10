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
# Gather engine source files (Runtime only — Editor is added conditionally below).
file(GLOB_RECURSE ENGINE_SOURCES "${CMAKE_SOURCE_DIR}/src/Runtime/*.cpp")
file(GLOB_RECURSE ENGINE_HEADERS "${CMAKE_SOURCE_DIR}/src/Runtime/*.h")

# Exclude Net sources when networking is disabled.
if (NOT TNX_ENABLE_NETWORK)
    list(FILTER ENGINE_SOURCES EXCLUDE REGEX ".*/Net/.*\\.cpp$")
    list(FILTER ENGINE_HEADERS EXCLUDE REGEX ".*/Net/.*\\.h$")
endif ()

# volk: VOLK_IMPLEMENTATION is defined in VulkanContext.cpp — no standalone volk.c needed.
set(VOLK_SOURCES "")

# Dear ImGui + Editor sources (editor builds only).
if (TNX_ENABLE_EDITOR)
    file(GLOB_RECURSE EDITOR_SOURCES "${CMAKE_SOURCE_DIR}/src/Editor/*.cpp")
    file(GLOB_RECURSE EDITOR_HEADERS "${CMAKE_SOURCE_DIR}/src/Editor/*.h")
    set(GUIZMO_PATH "${CMAKE_SOURCE_DIR}/libs/guizmo")
    set(IMGUI_SOURCES
            "${IMGUI_PATH}/imgui.cpp"
            "${IMGUI_PATH}/imgui_demo.cpp"
            "${IMGUI_PATH}/imgui_draw.cpp"
            "${IMGUI_PATH}/imgui_tables.cpp"
            "${IMGUI_PATH}/imgui_widgets.cpp"
            "${IMGUI_PATH}/backends/imgui_impl_vulkan.cpp"
            "${IMGUI_PATH}/backends/imgui_impl_sdl3.cpp"
            "${GUIZMO_PATH}/ImGuizmo.cpp"
    )
else ()
    set(EDITOR_SOURCES "")
    set(EDITOR_HEADERS "")
    set(IMGUI_SOURCES "")
endif ()

# -----------------------------------------------------------------------------
# LIBRARY DEFINITION
# -----------------------------------------------------------------------------
add_library(${PROJECT_NAME} STATIC
    ${ENGINE_SOURCES}
    ${ENGINE_HEADERS}
        ${EDITOR_SOURCES}
        ${EDITOR_HEADERS}
    ${TRACY_SOURCES}
    ${VOLK_SOURCES}
        ${IMGUI_SOURCES}
        ../src/Runtime/Core/Public/ComponentView.h
        ../src/Runtime/Core/Public/TrinyxJobs.h
)

# Suppress all warnings from Tracy (third-party, not our code)
if(TRACY_SOURCES AND NOT MSVC)
    set_source_files_properties(${TRACY_SOURCES} PROPERTIES
        COMPILE_FLAGS "-w"
    )
endif()

# Suppress all warnings from Dear ImGui (third-party, not our code)
if (TNX_ENABLE_EDITOR AND NOT MSVC)
    set_source_files_properties(${IMGUI_SOURCES} PROPERTIES
            COMPILE_FLAGS "-w"
    )
endif ()

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
        "${CMAKE_SOURCE_DIR}/src/Runtime/Physics/Public"
        "${CMAKE_SOURCE_DIR}/src/Runtime/Assets/Public"
        "${CMAKE_SOURCE_DIR}/src/Runtime/Net/Public"
        "${CMAKE_SOURCE_DIR}/src/Runtime/Construct/Public"
        "${CMAKE_SOURCE_DIR}/src/Runtime/Entities/Public"
        "${CMAKE_SOURCE_DIR}/src/Runtime/Flow/Public"
        "${CMAKE_SOURCE_DIR}/src/Editor/Public"
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

# C. Vulkan + volk + VMA + cgltf
target_include_directories(${PROJECT_NAME} SYSTEM PUBLIC
    "${VOLK_PATH}"
    "${VMA_PATH}"
    "${Vulkan_INCLUDE_DIRS}"
        "${CMAKE_SOURCE_DIR}/libs/cgltf"
)

# E. Dear ImGui (editor builds only)
if (TNX_ENABLE_EDITOR)
    target_include_directories(${PROJECT_NAME} SYSTEM PUBLIC
            "${IMGUI_PATH}"
            "${IMGUI_PATH}/backends"
            "${GUIZMO_PATH}"
    )
endif ()
target_link_libraries(${PROJECT_NAME} PUBLIC Vulkan::Vulkan)

# D. Jolt Physics
# Jolt::Jolt works whether Jolt was found via find_package (installed/vcpkg)
# or built from the vendored submodule (source build creates a Jolt::Jolt alias).
# Include dirs come from the target's interface — no manual path needed.
target_link_libraries(${PROJECT_NAME} PUBLIC Jolt::Jolt)

# F. GameNetworkingSockets (static)
if (TNX_ENABLE_NETWORK AND TARGET GameNetworkingSockets_s)
    target_link_libraries(${PROJECT_NAME} PUBLIC GameNetworkingSockets_s)
    target_include_directories(${PROJECT_NAME} SYSTEM PUBLIC "${GNS_PATH}/include")
endif()

# volk: VOLK_IMPLEMENTATION lives in VulkanContext.cpp; no separate volk.c TU to flag.

# Shader output directory baked into the binary as a compile-time constant.
# vulkan.hpp: use dynamic dispatch (compatible with volk global function pointers).
target_compile_definitions(${PROJECT_NAME} PUBLIC
    TNX_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders"
    VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1
        $<$<BOOL:${TNX_ENABLE_EDITOR}>:IMGUI_IMPL_VULKAN_USE_VOLK>
        $<$<NOT:$<CONFIG:Release>>:TNX_DEV_METRICS=1>
        $<$<BOOL:${TNX_DETAILED_METRICS}>:TNX_DEV_METRICS_DETAILED=1>
)

# ---------------------------------------------------------------------------
# Slang shader compilation
# ---------------------------------------------------------------------------
# slangc is a pre-built binary — not built from source. The libs/slang/
# directory holds headers + runtime libs. The compiler binary is fetched
# from GitHub Releases on first configure if it isn't already present.
# Version must match the runtime libs in libs/slang/lib/.
set(_slang_version "2026.3.1")

if(WIN32)
    set(SLANGC "${CMAKE_SOURCE_DIR}/libs/slang/bin/slangc.exe")
    set(_slang_archive_name "slang-${_slang_version}-windows-x86_64.zip")
else()
    set(SLANGC "${CMAKE_SOURCE_DIR}/libs/slang/bin/slangc")
    set(_slang_archive_name "slang-${_slang_version}-linux-x86_64.tar.gz")
endif()

if(NOT EXISTS "${SLANGC}")
    set(_slang_url "https://github.com/shader-slang/slang/releases/download/v${_slang_version}/${_slang_archive_name}")
    set(_slang_archive "${CMAKE_BINARY_DIR}/${_slang_archive_name}")

    message(STATUS "slangc not found — downloading Slang ${_slang_version}...")
    file(DOWNLOAD "${_slang_url}" "${_slang_archive}"
        SHOW_PROGRESS
        STATUS _slang_dl_status
        TLS_VERIFY ON
    )
    list(GET _slang_dl_status 0 _slang_dl_code)
    if(NOT _slang_dl_code EQUAL 0)
        list(GET _slang_dl_status 1 _slang_dl_error)
        message(FATAL_ERROR "Failed to download slangc: ${_slang_dl_error}\nURL: ${_slang_url}")
    endif()

    message(STATUS "Extracting Slang ${_slang_version}...")
    set(_slang_tmp "${CMAKE_BINARY_DIR}/_slang_extract_tmp")
    file(MAKE_DIRECTORY "${_slang_tmp}")
    file(ARCHIVE_EXTRACT INPUT "${_slang_archive}" DESTINATION "${_slang_tmp}")
    file(REMOVE "${_slang_archive}")

    # The archive may have a top-level subdirectory or place files at the root.
    # GLOB_RECURSE finds slangc regardless of nesting depth.
    file(GLOB_RECURSE _slangc_found "${_slang_tmp}/slangc*" "${_slang_tmp}/slangc.exe")
    if(NOT _slangc_found)
        file(REMOVE_RECURSE "${_slang_tmp}")
        message(FATAL_ERROR "slangc not found after extraction. Check archive: ${_slang_url}")
    endif()
    # Determine the directory where slangc lives inside the extracted tree.
    list(GET _slangc_found 0 _slangc_in_tmp)
    get_filename_component(_slang_bin_in_tmp "${_slangc_in_tmp}" DIRECTORY)

    # Copy all binaries from that directory into libs/slang/bin/.
    file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/libs/slang/bin")
    file(GLOB _slang_bin_files "${_slang_bin_in_tmp}/*")
    foreach(_f ${_slang_bin_files})
        file(COPY "${_f}" DESTINATION "${CMAKE_SOURCE_DIR}/libs/slang/bin")
    endforeach()
    file(REMOVE_RECURSE "${_slang_tmp}")

    if(NOT EXISTS "${SLANGC}")
        message(FATAL_ERROR
            "slangc extraction failed. Please manually download Slang ${_slang_version} from:\n"
            "  ${_slang_url}\n"
            "and place slangc(.exe) in libs/slang/bin/")
    endif()
    message(STATUS "slangc ready: ${SLANGC}")
endif()

# ---------------------------------------------------------------------------
# Shader sources — organized into subdirectories.
# Each entry produces a .spv in the corresponding output subdirectory.
# ---------------------------------------------------------------------------
set(SLANG_SHADER_SOURCES
        "${CMAKE_SOURCE_DIR}/shaders/compute/predicate.slang"
        "${CMAKE_SOURCE_DIR}/shaders/compute/prefix_sum.slang"
        "${CMAKE_SOURCE_DIR}/shaders/compute/scatter.slang"
        "${CMAKE_SOURCE_DIR}/shaders/compute/build_draws.slang"
        "${CMAKE_SOURCE_DIR}/shaders/compute/sort_instances.slang"
        "${CMAKE_SOURCE_DIR}/shaders/graphics/cube.vert.slang"
        "${CMAKE_SOURCE_DIR}/shaders/graphics/cube.frag.slang"
)

# Shared header that all shaders include — listed in DEPENDS so any shader
# rebuilds automatically when GpuFrameData.slang changes.
set(SLANG_COMMON_HEADERS "${CMAKE_SOURCE_DIR}/shaders/common/GpuFrameData.slang")

# Helper: compile a .slang source to .spv with optional extra defines.
function(compile_slang SLANG_SRC EXTRA_DEFINES SPV_OUTPUT_LIST)
    string(REGEX REPLACE "\\.slang$" "" SHADER_STEM "${SLANG_SRC}")
    get_filename_component(FINAL_NAME "${SHADER_STEM}" NAME)     # e.g. cube.vert, predicate
    get_filename_component(SHADER_DIR "${SLANG_SRC}" DIRECTORY)   # e.g. .../shaders/compute
    get_filename_component(SUBDIR "${SHADER_DIR}" NAME)       # e.g. compute, graphics

    # Suffix for picking variants
    set(VARIANT_SUFFIX "")
    if (EXTRA_DEFINES)
        set(VARIANT_SUFFIX "_pick")
    endif ()

    set(SPV_OUT "${CMAKE_SOURCE_DIR}/shaders/${SUBDIR}/${FINAL_NAME}${VARIANT_SUFFIX}.spv")

    # Determine the Slang stage from the file name suffix.
    if("${FINAL_NAME}" MATCHES "\\.vert$")
        set(SLANG_STAGE "-stage vertex")
    elseif("${FINAL_NAME}" MATCHES "\\.frag$")
        set(SLANG_STAGE "-stage fragment")
    else()
        set(SLANG_STAGE "-stage compute")
    endif()

    separate_arguments(SLANG_STAGE_ARGS UNIX_COMMAND "${SLANG_STAGE}")

    # Build define flags
    set(DEFINE_FLAGS "")
    foreach (DEF ${EXTRA_DEFINES})
        list(APPEND DEFINE_FLAGS "-D${DEF}")
    endforeach ()

    add_custom_command(
        OUTPUT  "${SPV_OUT}"
        COMMAND "${SLANGC}"
                ${SLANG_STAGE_ARGS}
                -target spirv
                -emit-spirv-directly
                -I "${CMAKE_SOURCE_DIR}/shaders"
            ${DEFINE_FLAGS}
                -o "${SPV_OUT}"
                "${SLANG_SRC}"
        DEPENDS "${SLANG_SRC}" ${SLANG_COMMON_HEADERS}
            COMMENT "slangc ${FINAL_NAME}${VARIANT_SUFFIX}.slang → ${SUBDIR}/${FINAL_NAME}${VARIANT_SUFFIX}.spv"
        VERBATIM
    )

    # Propagate to parent scope
    set(${SPV_OUTPUT_LIST} "${${SPV_OUTPUT_LIST}};${SPV_OUT}" PARENT_SCOPE)
endfunction()

# --- Base shader compilation (no picking) ---
set(SLANG_SPIRV_OUTPUTS "")
foreach (SLANG_SRC ${SLANG_SHADER_SOURCES})
    compile_slang("${SLANG_SRC}" "" SLANG_SPIRV_OUTPUTS)
endforeach()

# --- TNX_GPU_PICKING variants (scatter writes cache index, vert/frag have pick output) ---
if (TNX_GPU_PICKING)
    set(PICK_SHADER_SOURCES
            "${CMAKE_SOURCE_DIR}/shaders/compute/scatter.slang"
            "${CMAKE_SOURCE_DIR}/shaders/compute/sort_instances.slang"
            "${CMAKE_SOURCE_DIR}/shaders/graphics/cube.vert.slang"
            "${CMAKE_SOURCE_DIR}/shaders/graphics/cube.frag.slang"
    )
    foreach (SLANG_SRC ${PICK_SHADER_SOURCES})
        compile_slang("${SLANG_SRC}" "TNX_GPU_PICKING" SLANG_SPIRV_OUTPUTS)
    endforeach ()
endif ()

add_custom_target(TrinyxShaders ALL DEPENDS ${SLANG_SPIRV_OUTPUTS})
add_dependencies(${PROJECT_NAME} TrinyxShaders)

# E. Tracy — target-scoped includes, definitions, and platform link libraries.
# Kept here (not in root CMakeLists) so Tracy headers/defs don't leak into
# third-party subprojects (GNS, Jolt, etc.).
if(ENABLE_TRACY)
    target_include_directories(${PROJECT_NAME} SYSTEM PUBLIC "${TRACY_PATH}/public")
    target_compile_definitions(${PROJECT_NAME} PUBLIC
        TRACY_ENABLE
        TRACY_PROFILE_LEVEL=${TRACY_PROFILE_LEVEL}
        TRACY_ON_DEMAND
        TRACY_NO_ROCTRACER=1
        TRACY_NO_ROCPROF=1
    )
    if(WIN32)
        target_link_libraries(${PROJECT_NAME} PUBLIC ws2_32 dbghelp)
    elseif(UNIX)
        target_link_libraries(${PROJECT_NAME} PUBLIC pthread dl)
        # Some toolchains / distros still put clock_gettime in librt
        find_library(_librt rt)
        if (_librt)
            target_link_libraries(${PROJECT_NAME} PUBLIC rt)
        endif ()
    endif()
endif()
