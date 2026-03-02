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
target_link_libraries(${PROJECT_NAME} PRIVATE Trinyx)

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
