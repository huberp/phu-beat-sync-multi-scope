# =============================================================================
# JUCE Configuration
# =============================================================================

# --- JUCE Feature Flags ---
# Disable features not needed by this project.
# These must be set as both CMake cache variables (for JUCE's CMake module system)
# and as compile definitions (so they reach any target that compiles JUCE module
# files directly, e.g. PhuNetworkLib building juce_core.cpp).
set(JUCE_USE_CURL 0 CACHE INTERNAL "Disable CURL")
set(JUCE_WEB_BROWSER 0 CACHE INTERNAL "Disable web browser")
set(JUCE_VST3_CAN_REPLACE_VST2 0 CACHE INTERNAL "Disable VST3 replacing VST2")
set(JUCE_USE_CAMERA 0 CACHE INTERNAL "Disable camera")
set(JUCE_USE_MP3AUDIOFORMAT 0 CACHE INTERNAL "Disable MP3 audio format")
set(JUCE_USE_OGGVORBIS 0 CACHE INTERNAL "Disable Ogg Vorbis")
set(JUCE_USE_FLAC 0 CACHE INTERNAL "Disable FLAC")

add_compile_definitions(
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_USE_CAMERA=0
    JUCE_USE_MP3AUDIOFORMAT=0
    JUCE_USE_OGGVORBIS=0
    JUCE_USE_FLAC=0
)

# --- JUCE Build Options ---
# Disable JUCE extras and examples for faster builds
set(JUCE_BUILD_EXTRAS OFF CACHE BOOL "Build JUCE Extras")
set(JUCE_BUILD_EXAMPLES OFF CACHE BOOL "Build JUCE Examples")

# --- Platform-Specific JUCE Flags ---
if(WIN32)
    # Prevent Windows.h from defining min/max macros
    add_compile_definitions(NOMINMAX)
endif()
