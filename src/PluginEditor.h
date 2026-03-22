#pragma once

#include "ScopeDisplay.h"
#include <juce_audio_processors/juce_audio_processors.h>

class PhuBeatSyncMultiScopeAudioProcessor;

class PhuBeatSyncMultiScopeAudioProcessorEditor : public juce::AudioProcessorEditor,
                                                  public juce::Timer {
  public:
    PhuBeatSyncMultiScopeAudioProcessorEditor(PhuBeatSyncMultiScopeAudioProcessor&);
    ~PhuBeatSyncMultiScopeAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void timerCallback() override;

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
    juce::ToggleButton remoteDisplayToggle;  // Show/hide remote waveforms
    juce::ToggleButton broadcastToggle;       // Enable/disable broadcasting

#ifndef NDEBUG
    juce::TextEditor logTextEditor;
    juce::Label logLabel;
#endif

    // APVTS attachment for display range
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> displayRangeAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PhuBeatSyncMultiScopeAudioProcessorEditor)
};
