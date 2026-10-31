# Semicolon-separated JUCE formats; set by the "formats-*" presets in CMakePresets.json.
if(APPLE)
    set(_phu_default_formats "VST3;AU")
else()
    set(_phu_default_formats "VST3")
endif()
set(PHU_PLUGIN_FORMATS "${_phu_default_formats}" CACHE STRING "JUCE plugin formats to build (e.g. \"VST3;AU\")")

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
