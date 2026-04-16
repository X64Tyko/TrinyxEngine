# -----------------------------------------------------------------------------
# Testbed - Executable Application
# -----------------------------------------------------------------------------
project(Testbed VERSION 0.1 LANGUAGES CXX)

# -----------------------------------------------------------------------------
# SOURCE MANAGEMENT
# -----------------------------------------------------------------------------
file(GLOB_RECURSE TESTBED_SOURCES "src/*.cpp")
file(GLOB_RECURSE TESTBED_HEADERS "src/*.h")

# -----------------------------------------------------------------------------
# EXECUTABLE DEFINITION
# -----------------------------------------------------------------------------
add_executable(${PROJECT_NAME}
    ${TESTBED_SOURCES}
    ${TESTBED_HEADERS}
)

# -----------------------------------------------------------------------------
# INCLUDES & LINKS
# -----------------------------------------------------------------------------
# Link against the Trinyx library
target_link_libraries(${PROJECT_NAME} PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wl,--whole-archive>
        Trinyx
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wl,--no-whole-archive>
)

# Project directory for INI config discovery (source dir, not build dir)
target_compile_definitions(${PROJECT_NAME} PRIVATE
        TNX_PROJECT_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        TNX_TESTING   # Testbed always builds with test/observer APIs enabled
)

# Propagate TNX_TESTING to the engine library so that guarded APIs in Trinyx
# .cpp files (e.g. LocalOwnerID in NetConnectionManager) are compiled in.
# PRIVATE here is correct — it applies only to the Trinyx library's own TUs
# when Testbed is included in the build (each build dir is independent).
target_compile_definitions(Trinyx PRIVATE TNX_TESTING)

# Testbed-specific includes (for TestEntity.h and similar)
target_include_directories(${PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
        
target_include_directories(${PROJECT_NAME} PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Public"
)


# -----------------------------------------------------------------------------
# POST-BUILD STEPS
# -----------------------------------------------------------------------------
# Copy SDL3.dll to the executable folder
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${SDL3_PATH}/lib/x64/SDL3.dll"
    $<TARGET_FILE_DIR:${PROJECT_NAME}>
)
