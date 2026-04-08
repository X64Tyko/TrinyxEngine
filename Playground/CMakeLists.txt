# -----------------------------------------------------------------------------
# Playground - Network Playtest Application
# -----------------------------------------------------------------------------
project(Playground VERSION 0.1 LANGUAGES CXX)

# -----------------------------------------------------------------------------
# SOURCE MANAGEMENT
# -----------------------------------------------------------------------------
file(GLOB_RECURSE PLAYGROUND_SOURCES "src/*.cpp")
file(GLOB_RECURSE PLAYGROUND_HEADERS "src/*.h")

# -----------------------------------------------------------------------------
# EXECUTABLE DEFINITION
# -----------------------------------------------------------------------------
add_executable(${PROJECT_NAME}
        ${PLAYGROUND_SOURCES}
        ${PLAYGROUND_HEADERS}
        src/ArenaMode.h
)

# -----------------------------------------------------------------------------
# INCLUDES & LINKS
# -----------------------------------------------------------------------------
target_link_libraries(${PROJECT_NAME} PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wl,--whole-archive>
        Trinyx
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wl,--no-whole-archive>
)

# Playground-specific includes
target_include_directories(${PROJECT_NAME} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# Project directory for INI config discovery (source dir, not build dir)
target_compile_definitions(${PROJECT_NAME} PRIVATE
        TNX_PROJECT_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

# -----------------------------------------------------------------------------
# POST-BUILD STEPS
# -----------------------------------------------------------------------------
if (WIN32)
    # Copy SDL3.dll to the executable folder
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SDL3_PATH}/lib/x64/SDL3.dll"
            $<TARGET_FILE_DIR:${PROJECT_NAME}>
    )
endif ()
