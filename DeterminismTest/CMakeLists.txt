# -----------------------------------------------------------------------------
# DeterminismTest — Standalone rollback determinism test harness
# Build with TNX_ENABLE_ROLLBACK=ON, TNX_ENABLE_NETWORK=OFF
# -----------------------------------------------------------------------------
project(DeterminismTest VERSION 0.1 LANGUAGES CXX)

# -----------------------------------------------------------------------------
# SOURCE MANAGEMENT
# -----------------------------------------------------------------------------
file(GLOB_RECURSE DETERMINISM_SOURCES "src/*.cpp")
file(GLOB_RECURSE DETERMINISM_HEADERS "src/*.h")

# Separate the entry point from the game sources.
set(DETERMINISM_MAIN_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/DeterminismMain.cpp")
list(REMOVE_ITEM DETERMINISM_SOURCES "${DETERMINISM_MAIN_SOURCE}")

# -----------------------------------------------------------------------------
# GAME STATIC LIBRARY
# Compiling game sources into a static lib lets us apply --whole-archive so
# that static initializers in registration TUs (TNX_DEFINE_ENTITY etc.) are
# guaranteed to run even when the linker sees no external references.
# -----------------------------------------------------------------------------
add_library(DeterminismGame STATIC
        ${DETERMINISM_SOURCES}
        ${DETERMINISM_HEADERS}
)

target_include_directories(DeterminismGame PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_definitions(DeterminismGame PRIVATE
        TNX_PROJECT_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(DeterminismGame PRIVATE Trinyx)

# -----------------------------------------------------------------------------
# EXECUTABLE
# -----------------------------------------------------------------------------
add_executable(${PROJECT_NAME} "${DETERMINISM_MAIN_SOURCE}")

target_include_directories(${PROJECT_NAME} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
        TNX_PROJECT_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

# Wrap both Trinyx AND DeterminismGame with --whole-archive so every
# registration TU fires its static initializer.
target_link_libraries(${PROJECT_NAME} PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wl,--whole-archive>
        Trinyx
        DeterminismGame
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wl,--no-whole-archive>
)

target_link_options(${PROJECT_NAME} PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/WHOLEARCHIVE:$<TARGET_FILE:Trinyx>>
        $<$<CXX_COMPILER_ID:MSVC>:/WHOLEARCHIVE:$<TARGET_FILE:DeterminismGame>>
)

# -----------------------------------------------------------------------------
# POST-BUILD STEPS
# -----------------------------------------------------------------------------
if (WIN32)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SDL3_PATH}/lib/x64/SDL3.dll"
            $<TARGET_FILE_DIR:${PROJECT_NAME}>
    )
endif ()
