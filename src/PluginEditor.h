#pragma once

#include "DisplayFilterStrip.h"
#include "RemoteChannelChooser.h"
#include "ScopeDisplay.h"
#include "DebugLogPanel.h"
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

  private:
    PhuBeatSyncMultiScopeAudioProcessor& audioProcessor;

    // Beat-synced oscilloscope display
    ScopeDisplay scopeDisplay;

    // Display range combo box
    juce::Label displayRangeLabel;
    juce::ComboBox displayRangeCombo;

    // Remote waveform controls (following phu-splitter pattern)
    juce::GroupComponent remoteGroup;
    juce::ToggleButton localDisplayToggle;   // Show/hide local waveform

    // Remote mode controls (replaces the old "Show Remote" toggle)
    enum class RemoteMode { All = 0, Selected = 1, None = 2 };
    juce::Label     remoteModeLabel;
    juce::ComboBox  remoteModeCombo;
    juce::TextButton remoteChooseButton { "Choose\xe2\x80\xa6" }; // "Choose…"

    juce::ToggleButton broadcastToggle;       // Enable/disable broadcasting
    juce::TextButton broadcastOnlyToggle;     // Active vs Broadcast-only mode
    juce::TextButton peersBroadcastOnlyButton; // Sends command to peers to enter broadcast-only mode

    // Channel identity controls
    juce::GroupComponent identityGroup;
    juce::ComboBox       channelIndexCombo;      // "Ch 1" ... "Ch 8"
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

    // Display filter DSP parameters (GUI thread only — filter state now lives in ScopeDisplay)
    double m_lastSampleRate  = 0.0;
    float  m_lastHpFreq      = -1.0f;
    float  m_lastLpFreq      = -1.0f;
    bool   m_lastHpEnabled   = false;
    bool   m_lastLpEnabled   = false;
    double m_lastBpm         = -1.0;
    double m_lastDisplayRangeBeats = -1.0;

    // Persistent cache for remote raw packets — reused each frame to avoid heap allocation
    std::vector<SampleBroadcaster::RemoteRawPacket> m_remoteDataCache;

    // Persistent cache for remote instance infos — updated each frame from CtrlBroadcaster
    std::vector<phu::network::RemoteInstanceInfo> m_remoteInfosCache;

    // Track last BPM-derived max display range to avoid redundant combo-box updates
    double m_lastMaxDisplayBeats = 8.0;

    // Track remote-display mode state to detect transitions between frames.
    RemoteMode m_remoteMode         = RemoteMode::All;
    RemoteMode m_lastRemoteMode     = RemoteMode::All;
    uint8_t    m_remoteChannelMask  = 0xFF; // all channels enabled by default
    uint8_t    m_lastRemoteChannelMask = 0xFF;

    // Track local instance index so we only call setLocalInstanceIndex() when it changes
    int m_lastLocalInstanceIndex = -1;

    // Track broadcast-only mode transitions so peer-triggered mode changes update
    // the full UI state (button latch, overlay, disabled controls) immediately.
    bool m_lastBroadcastOnlyMode = false;

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
    void applyBroadcastOnlyUiState(bool enabled);
    void openChannelChooser();

#if PHU_DEBUG_UI
    // Reusable debug log panel with decoupled low-rate UI timer
    std::unique_ptr<DebugLogPanel> m_debugLogPanel;
#endif

    // APVTS attachment for display range
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> displayRangeAttachment;

    // APVTS attachment for channel index
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> channelIndexAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PhuBeatSyncMultiScopeAudioProcessorEditor)
};
