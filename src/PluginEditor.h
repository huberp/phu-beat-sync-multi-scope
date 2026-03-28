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
      public juce::AudioProcessorValueTreeState::Listener {
  public:
    PhuBeatSyncMultiScopeAudioProcessorEditor(PhuBeatSyncMultiScopeAudioProcessor&);
    ~PhuBeatSyncMultiScopeAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void timerCallback() override;

    /** APVTS listener — enforces HP freq < LP freq constraint. */
    void parameterChanged(const juce::String& parameterID, float newValue) override;

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

    // Display filter controls
    juce::GroupComponent filtersGroup;
    DisplayFilterStrip hpFilterStrip;
    DisplayFilterStrip lpFilterStrip;

    // Display filter DSP (GUI thread only — never touched by audio thread)
    LinkwitzRiley::LinkwitzRileyFilter<float> m_displayHP;
    LinkwitzRiley::LinkwitzRileyFilter<float> m_displayLP;
    double m_lastSampleRate = 0.0;
    float  m_lastHpFreq = -1.0f;
    float  m_lastLpFreq = -1.0f;

    // Working buffer for display filter application (persistent to avoid per-tick allocation)
    std::vector<float> m_displayWorkBuf;

    // Minimum gap between HP and LP frequencies (Hz)
    static constexpr float MIN_FREQ_GAP = 10.0f;

#ifndef NDEBUG
    juce::TextEditor logTextEditor;
    juce::Label logLabel;
#endif

    // APVTS attachment for display range
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> displayRangeAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PhuBeatSyncMultiScopeAudioProcessorEditor)
};
