cmake_minimum_required(VERSION 3.15)
project(phu-beat-sync-multi-scope VERSION 0.1.0 LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJC OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE
# Build JUCE extras/examples OFF for faster builds
set(JUCE_BUILD_EXTRAS OFF CACHE BOOL "Build JUCE Extras")
set(JUCE_BUILD_EXAMPLES OFF CACHE BOOL "Build JUCE Examples")

set(JUCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/JUCE" CACHE PATH "Path to JUCE root")
add_subdirectory("${JUCE_ROOT}")

add_subdirectory(phu-audio-lib)
# PhuNetworkLib's .cpp files include juce_core headers; link the JUCE
# target that the parent project already provides, with matching defines.
target_link_libraries(PhuNetworkLib PUBLIC juce::juce_core)
target_compile_definitions(PhuNetworkLib PUBLIC
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
)
target_compile_definitions(PhuGLUtils PUBLIC
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
)
# Static libs linked into a shared library (VST3) need PIC
set_target_properties(PhuNetworkLib PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(PhuGLUtils PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_subdirectory(lib)
add_subdirectory(src)
add_subdirectory(tests)
