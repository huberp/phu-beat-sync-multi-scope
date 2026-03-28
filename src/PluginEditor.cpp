#include "PluginEditor.h"
#include "PluginProcessor.h"

#ifndef NDEBUG
#include "../lib/debug/EditorLogger.h"
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
    // Window size
    setSize(800, 612);

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

    // Register as APVTS listener to enforce constraint on external parameter changes
    audioProcessor.getAPVTS().addParameterListener("display_hp_freq", this);
    audioProcessor.getAPVTS().addParameterListener("display_lp_freq", this);

#ifndef NDEBUG
    // --- Debug Log ---
    logLabel.setText("Debug Log:", juce::dontSendNotification);
    addAndMakeVisible(logLabel);

    logTextEditor.setMultiLine(true);
    logTextEditor.setReadOnly(true);
    logTextEditor.setScrollbarsShown(true);
    logTextEditor.setCaretVisible(false);
    logTextEditor.setFont(juce::Font(juce::FontOptions(11.0f)));
    addAndMakeVisible(logTextEditor);

    // Register with logger
    if (auto* logger = audioProcessor.getEditorLogger())
        logger->setEditor(this);
#endif

    // Restore toggle states from processor
    remoteDisplayToggle.setToggleState(audioProcessor.isReceiveEnabled(),
                                       juce::dontSendNotification);
    broadcastToggle.setToggleState(audioProcessor.isBroadcastEnabled(),
                                   juce::dontSendNotification);

    // Initialize scope display range
    scopeDisplay.setDisplayRangeBeats(audioProcessor.getDisplayRangeBeats());
    scopeDisplay.setRemoteDisplayEnabled(audioProcessor.isReceiveEnabled());

    // Start UI refresh timer at 60 Hz
    startTimerHz(60);
}

PhuBeatSyncMultiScopeAudioProcessorEditor::~PhuBeatSyncMultiScopeAudioProcessorEditor() {
    stopTimer();

    audioProcessor.getAPVTS().removeParameterListener("display_hp_freq", this);
    audioProcessor.getAPVTS().removeParameterListener("display_lp_freq", this);

#ifndef NDEBUG
    if (auto* logger = audioProcessor.getEditorLogger())
        logger->clearEditor();
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
    g.drawText("PHU BEAT SYNC MULTI SCOPE", getLocalBounds().removeFromTop(40),
               juce::Justification::centred);
}

void PhuBeatSyncMultiScopeAudioProcessorEditor::resized() {
    auto area = getLocalBounds();

    // Title bar
    area.removeFromTop(40);

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
    auto remoteArea = controlStrip.removeFromLeft(420);
    remoteGroup.setBounds(remoteArea);
    auto remoteContent = remoteArea.reduced(10, 0);
    remoteContent.removeFromTop(16); // Space for group title
    remoteContent.removeFromBottom(4);
    localDisplayToggle.setBounds(remoteContent.removeFromLeft(100));
    remoteContent.removeFromLeft(6);
    remoteDisplayToggle.setBounds(remoteContent.removeFromLeft(110));
    remoteContent.removeFromLeft(6);
    broadcastToggle.setBounds(remoteContent.removeFromLeft(110));

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

    // Main display area
    area.reduce(10, 5);

#ifndef NDEBUG
    // Debug log at bottom
    auto logArea = area.removeFromBottom(80);
    logLabel.setBounds(logArea.removeFromTop(16));
    logTextEditor.setBounds(logArea);
    area.removeFromBottom(5);
#endif

    // Scope display fills remaining space
    scopeDisplay.setBounds(area);
}

// ============================================================================
// Timer Callback (60 Hz UI refresh)
// ============================================================================

void PhuBeatSyncMultiScopeAudioProcessorEditor::timerCallback() {
    auto& syncBuf = audioProcessor.getInputSyncBuffer();
    if (syncBuf.size() > 0) {
        // --- Update filter coefficients if sample rate or frequency changed ---
        const double sampleRate = audioProcessor.getSampleRate();
        const float hpFreq = audioProcessor.getAPVTS()
                                 .getRawParameterValue("display_hp_freq")->load();
        const float lpFreq = audioProcessor.getAPVTS()
                                 .getRawParameterValue("display_lp_freq")->load();

        if (sampleRate > 0.0) {
            const bool srChanged = (sampleRate != m_lastSampleRate);
            if (srChanged || hpFreq != m_lastHpFreq) {
                m_displayHP.setParams(LinkwitzRiley::FilterType::HighPass,
                                      LinkwitzRiley::Slope::DB48,
                                      hpFreq, static_cast<float>(sampleRate));
                m_displayHP.reset();
                m_lastHpFreq = hpFreq;
            }
            if (srChanged || lpFreq != m_lastLpFreq) {
                m_displayLP.setParams(LinkwitzRiley::FilterType::LowPass,
                                      LinkwitzRiley::Slope::DB48,
                                      lpFreq, static_cast<float>(sampleRate));
                m_displayLP.reset();
                m_lastLpFreq = lpFreq;
            }
            m_lastSampleRate = sampleRate;
        }

        // --- Copy sync buffer into a persistent working vector and apply display filters ---
        const int numBins = static_cast<int>(syncBuf.size());
        if (static_cast<int>(m_displayWorkBuf.size()) != numBins)
            m_displayWorkBuf.resize(static_cast<size_t>(numBins));
        std::copy(syncBuf.data(), syncBuf.data() + numBins, m_displayWorkBuf.begin());

        const bool hpEnabled = audioProcessor.getAPVTS()
                                   .getRawParameterValue("display_hp_enabled")->load() > 0.5f;
        const bool lpEnabled = audioProcessor.getAPVTS()
                                   .getRawParameterValue("display_lp_enabled")->load() > 0.5f;

        if (hpEnabled) {
            for (auto& s : m_displayWorkBuf)
                s = m_displayHP.processSample(s);
        }
        if (lpEnabled) {
            for (auto& s : m_displayWorkBuf)
                s = m_displayLP.processSample(s);
        }

        // --- Pass filtered data to the scope display ---
        scopeDisplay.setLocalData(m_displayWorkBuf.data(), static_cast<int>(m_displayWorkBuf.size()));

    }

    // Update PPQ position for playhead
    scopeDisplay.setCurrentPpq(audioProcessor.getSyncGlobals().getPpqEndOfBlock());
    scopeDisplay.setDisplayRangeBeats(audioProcessor.getDisplayRangeBeats());

    // Enforce BPM-aware display range constraints (grey out/auto-switch)
    updateDisplayRangeConstraints();

    // Get remote data if enabled (network receive on UI thread, per requirement)
    if (remoteDisplayToggle.getToggleState()) {
        auto remoteSamples = audioProcessor.getSampleBroadcaster().getReceivedSamples();
        scopeDisplay.setRemoteData(remoteSamples);
    } else {
        scopeDisplay.setRemoteData({});
    }

    // Repaint scope
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

// ============================================================================
// Debug Log
// ============================================================================

#ifndef NDEBUG
void PhuBeatSyncMultiScopeAudioProcessorEditor::addLogMessage(const juce::String& message) {
    logTextEditor.moveCaretToEnd();
    logTextEditor.insertTextAtCaret(message + "\n");
}
#endif
