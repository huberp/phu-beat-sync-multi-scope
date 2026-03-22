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
    : AudioProcessorEditor(&p), audioProcessor(p) {
    // Window size
    setSize(800, 500);

    // --- Scope Display ---
    addAndMakeVisible(scopeDisplay);

    // --- Display Range ---
    displayRangeLabel.setText("Range:", juce::dontSendNotification);
    displayRangeLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(displayRangeLabel);

    displayRangeCombo.addItem("1/4 Beat", 1);
    displayRangeCombo.addItem("1/2 Beat", 2);
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
        static const double ranges[] = {0.25, 0.5, 1.0, 2.0, 4.0, 8.0};
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
    // Update scope with local beat-sync buffer data
    auto& syncBuf = audioProcessor.getInputSyncBuffer();
    if (syncBuf.size() > 0) {
        scopeDisplay.setLocalData(syncBuf.data(), syncBuf.size());
    }

    // Update PPQ position for playhead
    scopeDisplay.setCurrentPpq(audioProcessor.getSyncGlobals().getPpqEndOfBlock());
    scopeDisplay.setDisplayRangeBeats(audioProcessor.getDisplayRangeBeats());

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
// Debug Log
// ============================================================================

#ifndef NDEBUG
void PhuBeatSyncMultiScopeAudioProcessorEditor::addLogMessage(const juce::String& message) {
    logTextEditor.moveCaretToEnd();
    logTextEditor.insertTextAtCaret(message + "\n");
}
#endif
