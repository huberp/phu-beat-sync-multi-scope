cmake_minimum_required(VERSION 3.15)
project(phu-beat-sync-multi-scope VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE
# Build JUCE extras/examples OFF for faster builds
set(JUCE_BUILD_EXTRAS OFF CACHE BOOL "Build JUCE Extras")
set(JUCE_BUILD_EXAMPLES OFF CACHE BOOL "Build JUCE Examples")

set(JUCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/JUCE" CACHE PATH "Path to JUCE root")
add_subdirectory("${JUCE_ROOT}")

add_subdirectory(lib)
add_subdirectory(src)
add_subdirectory(tests)
