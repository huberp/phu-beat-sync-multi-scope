#pragma once

#include "DisplayFilterStrip.h"
#include "ScopeDisplay.h"
#include "../lib/LinkwitzRileyFilter.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <vector>

class PhuBeatSyncMultiScopeAudioProcessor;

class PhuBeatSyncMultiScopeAudioProcessorEditor
    : public juce::AudioProcessorEditor,
      public juce::Timer,
      public juce::AudioProcessorValueTreeState::Listener,
      public juce::ChangeListener {
  public:
    PhuBeatSyncMultiScopeAudioProcessorEditor(PhuBeatSyncMultiScopeAudioProcessor&);
    ~PhuBeatSyncMultiScopeAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void timerCallback() override;

    /** APVTS listener — enforces HP freq < LP freq constraint. */
    void parameterChanged(const juce::String& parameterID, float newValue) override;

    /** ChangeListener — handles colour selector changes. */
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

    /**
     * Greys out display-range combo-box items that exceed the maximum supported
     * beat window for the current BPM, and auto-switches the selection down to the
     * highest allowed range when necessary.  Called from timerCallback().
     */
    void updateDisplayRangeConstraints();

#ifndef NDEBUG
    void addLogMessage(const juce::String& message);
#endif

  private:
    PhuBeatSyncMultiScopeAudioProcessor& audioProcessor;

    // Beat-synced oscilloscope display
    ScopeDisplay scopeDisplay;

    // Display range combo box
    juce::Label displayRangeLabel;
    juce::ComboBox displayRangeCombo;

    // Remote waveform controls (following phu-splitter pattern)
    juce::GroupComponent remoteGroup;
    juce::ToggleButton localDisplayToggle;    // Show/hide local waveform
    juce::ToggleButton remoteDisplayToggle;  // Show/hide remote waveforms
    juce::ToggleButton broadcastToggle;       // Enable/disable broadcasting

    // Channel identity controls
    juce::GroupComponent identityGroup;
    juce::Label          channelLabelTextLabel;  // "Label:" label
    juce::TextEditor     channelLabelEditor;     // editable channel name (max 31 chars)
    juce::TextButton     colourSwatchButton;     // shows current colour; click to change

    // Display filter controls
    juce::GroupComponent filtersGroup;
    DisplayFilterStrip hpFilterStrip;
    DisplayFilterStrip lpFilterStrip;

    // Analysis overlays
    juce::GroupComponent analysisGroup;
    juce::ToggleButton rmsToggle;          // Toggle RMS envelope lines
    juce::ToggleButton cancellationToggle; // Toggle inter-instance cancellation bar
    juce::Slider       amplitudeSlider;    // Y-scale leveler [0.5, 4.0], double-click resets to 1.0
    juce::Label        amplitudeLabel;

    // Display filter DSP (GUI thread only — never touched by audio thread)
    LinkwitzRiley::LinkwitzRileyFilter<float> m_displayHP;
    LinkwitzRiley::LinkwitzRileyFilter<float> m_displayLP;
    double m_lastSampleRate = 0.0;
    float  m_lastHpFreq = -1.0f;
    float  m_lastLpFreq = -1.0f;

    // Working buffer for display filter application (persistent to avoid per-tick allocation)
    std::vector<float> m_displayWorkBuf;

    // Persistent cache for remote raw packets — reused each frame to avoid heap allocation
    std::vector<SampleBroadcaster::RemoteRawPacket> m_remoteDataCache;

    // Persistent cache for remote instance infos — updated each frame from CtrlBroadcaster
    std::vector<phu::network::RemoteInstanceInfo> m_remoteInfosCache;

    // Track last BPM-derived max display range to avoid redundant combo-box updates
    double m_lastMaxDisplayBeats = 8.0;

    // Minimum gap between HP and LP frequencies (Hz)
    static constexpr float MIN_FREQ_GAP = 10.0f;

    // Cached APVTS parameter pointers — avoid string-keyed map lookup at 60 Hz
    std::atomic<float>* m_pHpEnabled  = nullptr;
    std::atomic<float>* m_pHpFreq     = nullptr;
    std::atomic<float>* m_pLpEnabled  = nullptr;
    std::atomic<float>* m_pLpFreq     = nullptr;

    // Deferred state sync: on the first timer tick, re-read processor state
    // to handle hosts that call setStateInformation after createEditor.
    bool m_needsStateSync = true;
    void syncUIFromProcessorState();

#ifndef NDEBUG
    juce::TextEditor logTextEditor;
    juce::Label logLabel;
#endif

    // APVTS attachment for display range
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> displayRangeAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PhuBeatSyncMultiScopeAudioProcessorEditor)
};
