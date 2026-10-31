cmake_minimum_required(VERSION 3.28)
project(phu-beat-sync-multi-scope VERSION 0.1.0 LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJC OBJCXX)
endif()

# Global settings (before fetching, so they also apply to fetched dependencies)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# JUCE feature flags and build options; included before the fetch so that the
# options and compile definitions also reach JUCE and phu-audio-lib targets.
include(cmake/juce_config.cmake)

# Fetch dependencies (reusable helpers in cmake/PhuDependencies.cmake)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PhuDependencies)
phu_fetch_juce(TAG 8.0.15)
phu_fetch_audio_lib(TAG main)

# Warnings/sanitizers/coverage; included after the fetch so they only apply to this project
include(cmake/compiler_flags.cmake)

# PhuNetworkLib's .cpp files include juce_core headers; link the JUCE
# target that the parent project already provides.
target_link_libraries(PhuNetworkLib PUBLIC juce::juce_core)

# Beat-sync multi-scope support library
add_library(BeatSyncLib INTERFACE)
target_link_libraries(BeatSyncLib INTERFACE
    PhuAudioLib
    PhuNetworkLib
    PhuGLUtils
)

add_subdirectory(src)
add_subdirectory(tests)
