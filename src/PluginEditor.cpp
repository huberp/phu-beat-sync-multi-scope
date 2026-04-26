#include "PluginEditor.h"
#include "PluginProcessor.h"

#ifndef NDEBUG
#include "debug/EditorLogger.h"
#endif

// ============================================================================
// Construction / Destruction
// ============================================================================

PhuBeatSyncMultiScopeAudioProcessorEditor::PhuBeatSyncMultiScopeAudioProcessorEditor(
    PhuBeatSyncMultiScopeAudioProcessor& p)
    : AudioProcessorEditor(&p),
      audioProcessor(p),
      hpFilterStrip("HP", p.getAPVTS(), "display_hp_enabled", "display_hp_freq"),
      lpFilterStrip("LP", p.getAPVTS(), "display_lp_enabled", "display_lp_freq") {
    // NOTE: setSize() is called at the END of the constructor so that all child
    // components (including the debug panel) exist when resized() first fires.

    // --- Scope Display ---
    addAndMakeVisible(scopeDisplay);

    // --- Display Range ---
    displayRangeLabel.setText("Range:", juce::dontSendNotification);
    displayRangeLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(displayRangeLabel);

    //displayRangeCombo.addItem("1/4 Beat", 1);
    //displayRangeCombo.addItem("1/2 Beat", 2);
    displayRangeCombo.addItem("1 Beat", 3);
    displayRangeCombo.addItem("2 Beats", 4);
    displayRangeCombo.addItem("4 Beats", 5);
    displayRangeCombo.addItem("8 Beats", 6);
    addAndMakeVisible(displayRangeCombo);

    displayRangeAttachment = std::make_unique<
        juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        audioProcessor.getAPVTS(), "display_range", displayRangeCombo);

    // Update display range when combo changes
    displayRangeCombo.onChange = [this]() {
        // Map combo box index to beat range, based on one beat ... so index 1 corresponds to 0,25 and therefore 1/4 note
        //static const double ranges[] = {0.25, 0.5, 1.0, 2.0, 4.0, 8.0};
        static const double ranges[] = {1.0, 2.0, 4.0, 8.0};
        int idx = displayRangeCombo.getSelectedItemIndex();
        if (idx >= 0 && idx < 6) {
            audioProcessor.setDisplayRangeBeats(ranges[idx]);
            scopeDisplay.setDisplayRangeBeats(ranges[idx]);
        }
    };

    // --- Remote Controls Group ---
    remoteGroup.setText("Network");
    addAndMakeVisible(remoteGroup);

    localDisplayToggle.setButtonText("Show Local");
    localDisplayToggle.setToggleState(true, juce::dontSendNotification);
    localDisplayToggle.onClick = [this]() {
        scopeDisplay.setLocalDisplayEnabled(localDisplayToggle.getToggleState());
        scopeDisplay.repaint();
    };
    addAndMakeVisible(localDisplayToggle);

    remoteDisplayToggle.setButtonText("Show Remote");
    remoteDisplayToggle.setToggleState(true, juce::dontSendNotification);
    remoteDisplayToggle.onClick = [this]() {
        bool enabled = remoteDisplayToggle.getToggleState();
        audioProcessor.setReceiveEnabled(enabled);
        scopeDisplay.setRemoteDisplayEnabled(enabled);
        scopeDisplay.repaint();
    };
    addAndMakeVisible(remoteDisplayToggle);

    broadcastToggle.setButtonText("Broadcast");
    broadcastToggle.setToggleState(audioProcessor.isBroadcastEnabled(),
                                   juce::dontSendNotification);
    broadcastToggle.onClick = [this]() {
        bool enabled = broadcastToggle.getToggleState();
        audioProcessor.setBroadcastEnabled(enabled);
    };
    addAndMakeVisible(broadcastToggle);

    broadcastOnlyToggle.setButtonText("B/Cast on/off");
    broadcastOnlyToggle.setClickingTogglesState(true);
    broadcastOnlyToggle.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF2A3F66));
    broadcastOnlyToggle.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xFFFF9F1C));
    broadcastOnlyToggle.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    broadcastOnlyToggle.setColour(juce::TextButton::textColourOnId, juce::Colour(0xFF1A1A1A));
    broadcastOnlyToggle.setToggleState(audioProcessor.isBroadcastOnlyMode(),
                                       juce::dontSendNotification);
    broadcastOnlyToggle.onClick = [this]() {
        const bool enabled = broadcastOnlyToggle.getToggleState();
        audioProcessor.setBroadcastOnlyMode(enabled);
        applyBroadcastOnlyUiState(enabled);
    };
    addAndMakeVisible(broadcastOnlyToggle);

    peersBroadcastOnlyButton.setButtonText("Peers B/Cast Only");
    peersBroadcastOnlyButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF3A355F));
    peersBroadcastOnlyButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    peersBroadcastOnlyButton.onClick = [this]() {
        audioProcessor.requestPeersBroadcastOnlyMode();
    };
    addAndMakeVisible(peersBroadcastOnlyButton);

    // --- Channel Identity Group ---
    identityGroup.setText("Identity");
    addAndMakeVisible(identityGroup);

    // Channel index combo ("Ch 1" – "Ch 8") — backed by the instance_channel APVTS parameter
    for (int i = 1; i <= 8; ++i)
        channelIndexCombo.addItem("Ch " + juce::String(i), i);
    channelIndexCombo.setSelectedId(audioProcessor.getInstanceIndex(), juce::dontSendNotification);
    channelIndexAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        audioProcessor.getAPVTS(), "instance_channel", channelIndexCombo);
    addAndMakeVisible(channelIndexCombo);

    channelLabelTextLabel.setText("Label:", juce::dontSendNotification);
    channelLabelTextLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(channelLabelTextLabel);

    channelLabelEditor.setMultiLine(false);
    channelLabelEditor.setReturnKeyStartsNewLine(false);
    channelLabelEditor.setInputRestrictions(31); // max 31 chars (+ null terminator = 32)
    channelLabelEditor.setText(audioProcessor.getChannelLabel(), juce::dontSendNotification);
    channelLabelEditor.onReturnKey = [this]() {
        audioProcessor.setChannelLabel(channelLabelEditor.getText());
        channelLabelEditor.giveAwayKeyboardFocus();
    };
    channelLabelEditor.onFocusLost = [this]() {
        audioProcessor.setChannelLabel(channelLabelEditor.getText());
    };
    addAndMakeVisible(channelLabelEditor);

    // Colour swatch: shows current instance colour; click opens JUCE ColourSelector
    const juce::Colour initialColour = audioProcessor.getInstanceColour();
    colourSwatchButton.setButtonText({});
    colourSwatchButton.setColour(juce::TextButton::buttonColourId, initialColour);
    colourSwatchButton.setTooltip("Click to change instance colour");
    colourSwatchButton.onClick = [this]() {
        auto* selector = new juce::ColourSelector(
            juce::ColourSelector::showColourAtTop |
            juce::ColourSelector::showSliders    |
            juce::ColourSelector::showColourspace);
        selector->setName("Colour");
        selector->setCurrentColour(audioProcessor.getInstanceColour());
        selector->setSize(300, 280);
        selector->addChangeListener(this);
        juce::CallOutBox::launchAsynchronously(
            std::unique_ptr<juce::Component>(selector),
            colourSwatchButton.getScreenBounds(), nullptr);
    };
    addAndMakeVisible(colourSwatchButton);

    // --- Display Filters Group ---
    filtersGroup.setText("Display Filters");
    addAndMakeVisible(filtersGroup);

    // Enforce HP < LP constraint when HP freq changes
    hpFilterStrip.onFreqChanged = [this](float /*newFreq*/) {
        juce::Component::SafePointer<PhuBeatSyncMultiScopeAudioProcessorEditor> safeThis(this);
        juce::MessageManager::callAsync([safeThis]() {
            if (safeThis == nullptr) return;
            auto& apvts = safeThis->audioProcessor.getAPVTS();
            const float hpFreq = apvts.getRawParameterValue("display_hp_freq")->load();
            const float lpFreq = apvts.getRawParameterValue("display_lp_freq")->load();
            if (hpFreq > lpFreq - MIN_FREQ_GAP) {
                if (auto* param = apvts.getParameter("display_hp_freq"))
                    param->setValueNotifyingHost(param->convertTo0to1(lpFreq - MIN_FREQ_GAP));
            }
        });
    };

    // Enforce HP < LP constraint when LP freq changes
    lpFilterStrip.onFreqChanged = [this](float /*newFreq*/) {
        juce::Component::SafePointer<PhuBeatSyncMultiScopeAudioProcessorEditor> safeThis(this);
        juce::MessageManager::callAsync([safeThis]() {
            if (safeThis == nullptr) return;
            auto& apvts = safeThis->audioProcessor.getAPVTS();
            const float hpFreq = apvts.getRawParameterValue("display_hp_freq")->load();
            const float lpFreq = apvts.getRawParameterValue("display_lp_freq")->load();
            if (lpFreq < hpFreq + MIN_FREQ_GAP) {
                if (auto* param = apvts.getParameter("display_lp_freq"))
                    param->setValueNotifyingHost(param->convertTo0to1(hpFreq + MIN_FREQ_GAP));
            }
        });
    };

    addAndMakeVisible(hpFilterStrip);
    addAndMakeVisible(lpFilterStrip);

    // --- Analysis Overlays Group ---
    analysisGroup.setText("Analysis");
    addAndMakeVisible(analysisGroup);

    rmsToggle.setButtonText("RMS Envelope");
    rmsToggle.setToggleState(false, juce::dontSendNotification);
    rmsToggle.onClick = [this]() {
        scopeDisplay.setRmsOverlayEnabled(rmsToggle.getToggleState());
        scopeDisplay.repaint();
    };
    addAndMakeVisible(rmsToggle);

    cancellationToggle.setButtonText("Cancellation");
    cancellationToggle.setToggleState(false, juce::dontSendNotification);
    cancellationToggle.onClick = [this]() {
        scopeDisplay.setCancellationOverlayEnabled(cancellationToggle.getToggleState());
        scopeDisplay.repaint();
    };
    addAndMakeVisible(cancellationToggle);

    amplitudeLabel.setText("Scale:", juce::dontSendNotification);
    amplitudeLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(amplitudeLabel);

    amplitudeSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    amplitudeSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 40, 18);
    amplitudeSlider.setRange(0.5, 4.0, 0.01);
    amplitudeSlider.setValue(1.0, juce::dontSendNotification);
    amplitudeSlider.setDoubleClickReturnValue(true, 1.0);
    amplitudeSlider.onValueChange = [this]() {
        scopeDisplay.setAmplitudeScale(static_cast<float>(amplitudeSlider.getValue()));
        scopeDisplay.repaint();
    };
    addAndMakeVisible(amplitudeSlider);

    // Cache raw parameter pointers once — avoids repeated string-keyed map lookups at 60 Hz
    m_pHpEnabled = audioProcessor.getAPVTS().getRawParameterValue("display_hp_enabled");
    m_pHpFreq    = audioProcessor.getAPVTS().getRawParameterValue("display_hp_freq");
    m_pLpEnabled = audioProcessor.getAPVTS().getRawParameterValue("display_lp_enabled");
    m_pLpFreq    = audioProcessor.getAPVTS().getRawParameterValue("display_lp_freq");

    // Register as APVTS listener to enforce constraint on external parameter changes
    audioProcessor.getAPVTS().addParameterListener("display_hp_freq", this);
    audioProcessor.getAPVTS().addParameterListener("display_lp_freq", this);

#ifndef NDEBUG
    // --- Debug Log Panel ---
    // Create reusable DebugLogPanel with independent low-rate UI timer
    m_debugLogPanel = std::make_unique<DebugLogPanel>(&audioProcessor);
    m_debugLogPanel->setFlushRateHz(10.0);  // Drain queue at 10 Hz (~100 ms batches)
    addAndMakeVisible(*m_debugLogPanel);

    // Attach logger to panel
    if (auto* logger = audioProcessor.getEditorLogger())
        m_debugLogPanel->attachLogger(logger);
#endif

    // Restore toggle states from processor
    syncUIFromProcessorState();

    // Set window size LAST — ensures all children (including debug panel) exist
    // when resized() fires for the first time.
#ifndef NDEBUG
    setSize(800, 732);  // Extra 120px for debug panel
#else
    setSize(800, 612);
#endif

    // Start UI refresh timer at 60 Hz
    startTimerHz(60);
}

PhuBeatSyncMultiScopeAudioProcessorEditor::~PhuBeatSyncMultiScopeAudioProcessorEditor() {
    stopTimer();

    audioProcessor.getAPVTS().removeParameterListener("display_hp_freq", this);
    audioProcessor.getAPVTS().removeParameterListener("display_lp_freq", this);

#ifndef NDEBUG
    // Detach logger from panel (panel destructor stops its timer)
    if (m_debugLogPanel)
        m_debugLogPanel->detachLogger();
#endif
}

// ============================================================================
// Layout
// ============================================================================

void PhuBeatSyncMultiScopeAudioProcessorEditor::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(0xFF16213E));

    // Title
    g.setColour(juce::Colours::white);
    g.setFont(juce::Font(juce::FontOptions(28.0f)).boldened());
    auto titleArea = getLocalBounds().removeFromTop(40).reduced(10, 0);
    titleArea.removeFromLeft(312); // Reserve top-left space for mode + peer command buttons
    g.drawText("PHU BEAT SYNC MULTI SCOPE", titleArea,
               juce::Justification::centred);
}

void PhuBeatSyncMultiScopeAudioProcessorEditor::resized() {
    auto area = getLocalBounds();

    // Title bar
    auto titleBar = area.removeFromTop(40).reduced(10, 6);
    broadcastOnlyToggle.setBounds(titleBar.removeFromLeft(128));
    titleBar.removeFromLeft(8);
    peersBroadcastOnlyButton.setBounds(titleBar.removeFromLeft(168));

    // Controls strip (top)
    auto controlStrip = area.removeFromTop(56);
    controlStrip.reduce(10, 3);

    // Display range (vertically centred in strip)
    auto rangeArea = controlStrip.removeFromLeft(160);
    int rangeY = rangeArea.getY() + (rangeArea.getHeight() - 24) / 2;
    displayRangeLabel.setBounds(rangeArea.getX(), rangeY, 50, 24);
    displayRangeCombo.setBounds(rangeArea.getX() + 55, rangeY, 100, 24);

    controlStrip.removeFromLeft(20); // Spacing

    // Remote controls group
    auto remoteArea = controlStrip.removeFromLeft(360);
    remoteGroup.setBounds(remoteArea);
    auto remoteContent = remoteArea.reduced(10, 0);
    remoteContent.removeFromTop(16); // Space for group title
    remoteContent.removeFromBottom(4);
    auto networkRow = remoteContent.removeFromTop(20);
    localDisplayToggle.setBounds(networkRow.removeFromLeft(92));
    networkRow.removeFromLeft(6);
    remoteDisplayToggle.setBounds(networkRow.removeFromLeft(100));
    networkRow.removeFromLeft(6);
    broadcastToggle.setBounds(networkRow.removeFromLeft(88));

    controlStrip.removeFromLeft(8); // Spacing

    // Identity group (channel index combo + channel label + colour swatch)
    auto identityArea = controlStrip;
    identityGroup.setBounds(identityArea);
    auto identityContent = identityArea.reduced(8, 0);
    identityContent.removeFromTop(16);
    identityContent.removeFromBottom(4);
    channelIndexCombo.setBounds(identityContent.removeFromLeft(60));
    identityContent.removeFromLeft(6);
    channelLabelTextLabel.setBounds(identityContent.removeFromLeft(40));
    identityContent.removeFromLeft(4);
    colourSwatchButton.setBounds(identityContent.removeFromRight(24));
    identityContent.removeFromRight(4);
    channelLabelEditor.setBounds(identityContent);

    // Display Filters strip (second control row)
    auto filtersStrip = area.removeFromTop(70);
    filtersStrip.reduce(10, 3);
    filtersGroup.setBounds(filtersStrip);
    auto filtersContent = filtersStrip.reduced(10, 0);
    filtersContent.removeFromTop(16); // Space for group title
    filtersContent.removeFromBottom(4);

    // Two sub-strips side by side
    auto hpArea = filtersContent.removeFromLeft(filtersContent.getWidth() / 2 - 4);
    filtersContent.removeFromLeft(8); // Gap between strips
    auto lpArea = filtersContent;

    hpFilterStrip.setBounds(hpArea);
    lpFilterStrip.setBounds(lpArea);

    // Analysis overlays strip (third control row)
    auto analysisStrip = area.removeFromTop(46);
    analysisStrip.reduce(10, 3);
    analysisGroup.setBounds(analysisStrip);
    auto analysisContent = analysisStrip.reduced(10, 0);
    analysisContent.removeFromTop(16);
    analysisContent.removeFromBottom(4);
    rmsToggle.setBounds(analysisContent.removeFromLeft(130));
    analysisContent.removeFromLeft(6);
    cancellationToggle.setBounds(analysisContent.removeFromLeft(130));
    analysisContent.removeFromLeft(16);
    amplitudeLabel.setBounds(analysisContent.removeFromLeft(38));
    amplitudeSlider.setBounds(analysisContent.removeFromLeft(160));

    // Main display area
    area.reduce(10, 5);

#ifndef NDEBUG
    // Debug log panel at bottom with independent low-rate timer
    if (m_debugLogPanel) {
        auto logArea = area.removeFromBottom(120);
        logArea.removeFromBottom(5);
        m_debugLogPanel->setBounds(logArea);
    }
#endif

    // Scope display fills remaining space
    scopeDisplay.setBounds(area);
}

// ============================================================================
// State Sync — reads all processor state into UI controls
// ============================================================================

void PhuBeatSyncMultiScopeAudioProcessorEditor::syncUIFromProcessorState() {
    const bool broadcastOnlyMode = audioProcessor.isBroadcastOnlyMode();

    broadcastToggle.setToggleState(audioProcessor.isBroadcastEnabled(),
                                   juce::dontSendNotification);
    remoteDisplayToggle.setToggleState(audioProcessor.isReceiveEnabled(),
                                       juce::dontSendNotification);
    broadcastOnlyToggle.setToggleState(broadcastOnlyMode,
                                       juce::dontSendNotification);

    // Channel identity
    channelLabelEditor.setText(audioProcessor.getChannelLabel(), juce::dontSendNotification);
    const juce::Colour colour = audioProcessor.getInstanceColour();
    colourSwatchButton.setColour(juce::TextButton::buttonColourId, colour);

    // Sync channel index combo and notify ScopeDisplay
    const int idx = audioProcessor.getInstanceIndex();
    channelIndexCombo.setSelectedId(idx, juce::dontSendNotification);
    scopeDisplay.setLocalInstanceIndex(idx);
    m_lastLocalInstanceIndex = idx;

    // Scope display
    scopeDisplay.setDisplayRangeBeats(audioProcessor.getDisplayRangeBeats());
    scopeDisplay.setRemoteDisplayEnabled(audioProcessor.isReceiveEnabled());
    scopeDisplay.setLocalColour(colour);

    applyBroadcastOnlyUiState(broadcastOnlyMode);
    m_lastBroadcastOnlyMode = broadcastOnlyMode;
}

// ============================================================================
// Timer Callback (60 Hz UI refresh)
// ============================================================================

void PhuBeatSyncMultiScopeAudioProcessorEditor::timerCallback() {
    // Deferred state sync: handle hosts that call setStateInformation after createEditor
    if (m_needsStateSync) {
        syncUIFromProcessorState();
        m_needsStateSync = false;
    }

    const double sampleRate   = audioProcessor.getSampleRate();
    const double bpm          = audioProcessor.getSyncGlobals().getBPM();
    const double displayBeats = audioProcessor.getDisplayRangeBeats();

    // Read filter parameters from APVTS
    const float hpFreqNow    = m_pHpFreq->load(std::memory_order_relaxed);
    const float lpFreqNow    = m_pLpFreq->load(std::memory_order_relaxed);
    const bool  hpEnabledNow = m_pHpEnabled->load(std::memory_order_relaxed) > 0.5f;
    const bool  lpEnabledNow = m_pLpEnabled->load(std::memory_order_relaxed) > 0.5f;

    const bool broadcastOnlyMode = audioProcessor.isBroadcastOnlyMode();
    if (broadcastOnlyMode != m_lastBroadcastOnlyMode) {
        applyBroadcastOnlyUiState(broadcastOnlyMode);
        m_lastBroadcastOnlyMode = broadcastOnlyMode;
    }

    if (broadcastOnlyMode)
        return;

    // --- Prepare pipeline when display parameters change ---
    const bool pipelineParamsChanged = (bpm != m_lastBpm ||
                                        sampleRate != m_lastSampleRate ||
                                        displayBeats != m_lastDisplayRangeBeats);
    if (pipelineParamsChanged && bpm > 0.0 && sampleRate > 0.0 && displayBeats > 0.0)
        scopeDisplay.prepare(displayBeats, bpm, sampleRate);

    // --- Update filter coefficients when they change ---
    // Also update after prepare() since prepare() may reset filter state.
    const bool filterParamsChanged = (hpFreqNow    != m_lastHpFreq    ||
                                      lpFreqNow    != m_lastLpFreq    ||
                                      hpEnabledNow != m_lastHpEnabled ||
                                      lpEnabledNow != m_lastLpEnabled ||
                                      sampleRate   != m_lastSampleRate);
    if (filterParamsChanged && sampleRate > 0.0)
        scopeDisplay.setFilterParams(hpEnabledNow, hpFreqNow,
                                     lpEnabledNow, lpFreqNow,
                                     sampleRate);

    // Update tracking variables
    m_lastSampleRate          = sampleRate;
    m_lastBpm                 = bpm;
    m_lastDisplayRangeBeats   = displayBeats;
    m_lastHpFreq              = hpFreqNow;
    m_lastLpFreq              = lpFreqNow;
    m_lastHpEnabled           = hpEnabledNow;
    m_lastLpEnabled           = lpEnabledNow;

    // --- Drain local (monoSample, absolutePpq) SPSC ring → write to ScopeDisplay ---
    {
        auto& fifo          = audioProcessor.getLocalRingFifo();
        const auto& samples = audioProcessor.getLocalRingSamples();
        const auto& ppqs    = audioProcessor.getLocalRingPpqs();
        const int numAvail  = fifo.getNumReady();
        if (numAvail > 0) {
            const auto scope = fifo.read(numAvail);
            for (int i = 0; i < scope.blockSize1; ++i)
                scopeDisplay.writeLocalSample(
                    samples[static_cast<size_t>(scope.startIndex1 + i)],
                    ppqs   [static_cast<size_t>(scope.startIndex1 + i)]);
            for (int i = 0; i < scope.blockSize2; ++i)
                scopeDisplay.writeLocalSample(
                    samples[static_cast<size_t>(scope.startIndex2 + i)],
                    ppqs   [static_cast<size_t>(scope.startIndex2 + i)]);
        }
    }

    // Update PPQ position for playhead and display range
    scopeDisplay.setCurrentPpq(audioProcessor.getSyncGlobals().getPpqEndOfBlock());
    scopeDisplay.setDisplayRangeBeats(displayBeats);

    // Enforce BPM-aware display range constraints (grey out/auto-switch)
    updateDisplayRangeConstraints();

    // --- Poll channel index: sync to ScopeDisplay whenever the user changes it ---
    {
        const int currentIdx = audioProcessor.getInstanceIndex();
        if (currentIdx != m_lastLocalInstanceIndex) {
            scopeDisplay.setLocalInstanceIndex(currentIdx);
            m_lastLocalInstanceIndex = currentIdx;
        }
    }

    // --- Consume remote packets (network receive on UI thread, per requirement) ---
    const bool remoteEnabled = remoteDisplayToggle.getToggleState();
    if (remoteEnabled) {
        audioProcessor.getCtrlBroadcaster().getRemoteInfos(m_remoteInfosCache);
        audioProcessor.getSampleBroadcaster().getReceivedPackets(m_remoteDataCache);
        scopeDisplay.writeRemotePackets(m_remoteDataCache, m_remoteInfosCache);
    } else if (m_lastRemoteEnabled) {
        // Remote display just turned off — clear instance slots once instead of
        // zeroing 114 KB of buffers unconditionally at 60 Hz.
        scopeDisplay.clearRemoteInstances();
    }
    m_lastRemoteEnabled = remoteEnabled;

    // --- Compute frame and repaint only when new data arrived ---
    // Skipping computeFrame() + repaint() when the DAW is stopped (and no remote
    // packets are incoming) eliminates ~10 ms of idle CPU work per 60-Hz tick.
    if (scopeDisplay.hasNewFrameData()) {
        scopeDisplay.computeFrame();
        scopeDisplay.repaint();
        scopeDisplay.clearNewFrameData();
    }
}

void PhuBeatSyncMultiScopeAudioProcessorEditor::applyBroadcastOnlyUiState(bool enabled) {
    broadcastOnlyToggle.setToggleState(enabled, juce::dontSendNotification);
    displayRangeCombo.setEnabled(!enabled);
    localDisplayToggle.setEnabled(!enabled);
    remoteDisplayToggle.setEnabled(!enabled);
    hpFilterStrip.setEnabled(!enabled);
    lpFilterStrip.setEnabled(!enabled);
    rmsToggle.setEnabled(!enabled);
    cancellationToggle.setEnabled(!enabled);
    amplitudeSlider.setEnabled(!enabled);

    if (enabled) {
        scopeDisplay.setLocalDisplayEnabled(false);
        scopeDisplay.setRemoteDisplayEnabled(false);
        scopeDisplay.setBroadcastOnlyOverlayEnabled(true);
        scopeDisplay.clearRemoteInstances();
        m_lastRemoteEnabled = false;
    } else {
        scopeDisplay.setLocalDisplayEnabled(localDisplayToggle.getToggleState());
        scopeDisplay.setRemoteDisplayEnabled(remoteDisplayToggle.getToggleState());
        scopeDisplay.setBroadcastOnlyOverlayEnabled(false);
        m_lastRemoteEnabled = remoteDisplayToggle.getToggleState();
    }

    scopeDisplay.repaint();
}

// ============================================================================
// BPM-aware display range constraints
// ============================================================================

void PhuBeatSyncMultiScopeAudioProcessorEditor::updateDisplayRangeConstraints() {
    // Display-range choices match the combo-box items (item IDs are 1-based)
    static constexpr double kRanges[] = {0.25, 0.5, 1.0, 2.0, 4.0, 8.0};
    static constexpr int kNumRanges = 6;

    const double bpm = audioProcessor.getSyncGlobals().getBPM();
    const double maxBeats = (bpm > 0.0)
                                ? PhuBeatSyncMultiScopeAudioProcessor::getMaxDisplayBeatsForBpm(bpm)
                                : 8.0;

    // Skip update if the BPM category hasn't changed.
    // getMaxDisplayBeatsForBpm() always returns an exact integer, so cast to int
    // to avoid a -Wfloat-equal warning on the comparison.
    if (static_cast<int>(maxBeats) == static_cast<int>(m_lastMaxDisplayBeats))
        return;
    m_lastMaxDisplayBeats = maxBeats;

    // Enable/disable combo-box items based on the current BPM threshold
    for (int i = 0; i < kNumRanges; ++i)
        displayRangeCombo.setItemEnabled(i + 1, kRanges[i] <= maxBeats);

    // If the currently selected range is now disabled, auto-switch to the
    // highest range that is still allowed.
    const int currentIdx = displayRangeCombo.getSelectedItemIndex();
    if (currentIdx >= 0 && currentIdx < kNumRanges && kRanges[currentIdx] > maxBeats) {
        for (int i = kNumRanges - 1; i >= 0; --i) {
            if (kRanges[i] <= maxBeats) {
                displayRangeCombo.setSelectedItemIndex(i, juce::sendNotificationSync);
                break;
            }
        }
    }
}

// ============================================================================
// ChangeListener — handles colour selector popup
// ============================================================================

void PhuBeatSyncMultiScopeAudioProcessorEditor::changeListenerCallback(
    juce::ChangeBroadcaster* source) {
    if (auto* selector = dynamic_cast<juce::ColourSelector*>(source)) {
        const juce::Colour newColour = selector->getCurrentColour();
        audioProcessor.setInstanceColour(newColour);
        colourSwatchButton.setColour(juce::TextButton::buttonColourId, newColour);
        colourSwatchButton.repaint();
        scopeDisplay.setLocalColour(newColour);
    }
}

// ============================================================================
// APVTS Listener — HP/LP frequency constraint enforcement
// ============================================================================

void PhuBeatSyncMultiScopeAudioProcessorEditor::parameterChanged(
    const juce::String& parameterID, float /*newValue*/) {
    if (parameterID != "display_hp_freq" && parameterID != "display_lp_freq")
        return;

    const bool isHP = (parameterID == "display_hp_freq");

    // Defer to the message thread to safely access APVTS parameters.
    // Use SafePointer so the callback is a no-op if the editor is deleted first.
    juce::Component::SafePointer<PhuBeatSyncMultiScopeAudioProcessorEditor> safeThis(this);
    juce::MessageManager::callAsync([safeThis, isHP]() {
        if (safeThis == nullptr) return;

        auto& apvts = safeThis->audioProcessor.getAPVTS();
        const float hpFreq = apvts.getRawParameterValue("display_hp_freq")->load();
        const float lpFreq = apvts.getRawParameterValue("display_lp_freq")->load();

        if (hpFreq > lpFreq - MIN_FREQ_GAP) {
            if (isHP) {
                // HP moved up past LP − gap → clamp HP
                if (auto* param = apvts.getParameter("display_hp_freq"))
                    param->setValueNotifyingHost(param->convertTo0to1(lpFreq - MIN_FREQ_GAP));
            } else {
                // LP moved down past HP + gap → clamp LP
                if (auto* param = apvts.getParameter("display_lp_freq"))
                    param->setValueNotifyingHost(param->convertTo0to1(hpFreq + MIN_FREQ_GAP));
            }
        }
    });
}
