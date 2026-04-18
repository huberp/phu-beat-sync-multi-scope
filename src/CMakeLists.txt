if(APPLE)
    set(PHU_PLUGIN_FORMATS VST3 AU)
else()
    set(PHU_PLUGIN_FORMATS VST3)
endif()

juce_add_plugin(phu-beat-sync-multi-scope
    PLUGIN_MANUFACTURER_CODE Phub
    PLUGIN_CODE Pbsm
    FORMATS ${PHU_PLUGIN_FORMATS}
    PRODUCT_NAME "PHU BEAT SYNC MULTI SCOPE"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
)

target_sources(phu-beat-sync-multi-scope PRIVATE
    PluginProcessor.cpp
    PluginProcessor.h
    PluginEditor.cpp
    PluginEditor.h
    ScopeDisplay.cpp
    ScopeDisplay.h
    DisplayFilterStrip.cpp
    DisplayFilterStrip.h
    DebugLogPanel.cpp
    DebugLogPanel.h

    #opengl renderers
    gl/WaveformGLRenderer.cpp
    gl/WaveformGLRenderer.h
    gl/ScopeGLCoordinator.cpp
    gl/ScopeGLCoordinator.h
    gl/GridPlayheadGLRenderer.cpp
    gl/GridPlayheadGLRenderer.h
    gl/RmsOverlayRenderer.cpp
    gl/RmsOverlayRenderer.h
    gl/CancellationOverlayRenderer.cpp
    gl/CancellationOverlayRenderer.h
)

target_compile_definitions(phu-beat-sync-multi-scope PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_USE_CAMERA=0
    JUCE_USE_MP3AUDIOFORMAT=0
    JUCE_USE_OGGVORBIS=0
    JUCE_USE_FLAC=0
    $<$<CONFIG:Debug>:PHU_DEBUG_UI=1>
    $<$<NOT:$<CONFIG:Debug>>:PHU_DEBUG_UI=0>
)

# Prevent Windows.h from defining min/max macros
if(WIN32)
    target_compile_definitions(phu-beat-sync-multi-scope PUBLIC NOMINMAX)
endif()

target_link_libraries(phu-beat-sync-multi-scope
    PRIVATE
        BeatSyncLib
        juce::juce_audio_processors
        juce::juce_dsp
        juce::juce_opengl
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
