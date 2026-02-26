# -----------------------------------------------------------------------------
# StrigidEngine - Static Library
# -----------------------------------------------------------------------------
project(StrigidEngine VERSION 0.1 LANGUAGES CXX)

# -----------------------------------------------------------------------------
# SOURCE MANAGEMENT
# -----------------------------------------------------------------------------
# Gather all engine source files
file(GLOB_RECURSE ENGINE_SOURCES "${CMAKE_SOURCE_DIR}/src/*.cpp")
file(GLOB_RECURSE ENGINE_HEADERS "${CMAKE_SOURCE_DIR}/src/*.h")

# -----------------------------------------------------------------------------
# LIBRARY DEFINITION
# -----------------------------------------------------------------------------
add_library(${PROJECT_NAME} STATIC
    ${ENGINE_SOURCES}
    ${ENGINE_HEADERS}
    ${TRACY_SOURCES}
        ../src/Runtime/Core/Public/ComponentView.h
)

# Disable strict warnings for Tracy sources
if(TRACY_SOURCES AND NOT MSVC)
    set_source_files_properties(${TRACY_SOURCES} PROPERTIES
        COMPILE_FLAGS "-Wno-error"
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

# D. Tracy platform-specific libraries
if(ENABLE_TRACY)
    if(WIN32)
        target_link_libraries(${PROJECT_NAME} PUBLIC ws2_32 dbghelp)
    elseif(UNIX)
        target_link_libraries(${PROJECT_NAME} PUBLIC pthread dl)
    endif()
endif()
