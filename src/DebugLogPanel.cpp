#include "DebugLogPanel.h"
#include "../lib/debug/EditorLogger.h"
#include "PluginProcessor.h"
#include <sstream>

#if PHU_DEBUG_UI // Debug builds only

DebugLogPanel::DebugLogPanel(PhuBeatSyncMultiScopeAudioProcessor* processor)
    : m_processor(processor)
{
    // Configure text editor — no title bar, full area used for log content
    m_logTextEditor.setMultiLine(true);
    m_logTextEditor.setReadOnly(true);
    m_logTextEditor.setCaretVisible(false);
    m_logTextEditor.setWantsKeyboardFocus(false);
    m_logTextEditor.setFont(juce::Font(juce::FontOptions(11.0f)));
    m_logTextEditor.setColour(juce::TextEditor::backgroundColourId,  juce::Colour(0xFF101820));
    m_logTextEditor.setColour(juce::TextEditor::textColourId,        juce::Colour(0xFF90EE90));
    m_logTextEditor.setColour(juce::TextEditor::outlineColourId,     juce::Colour(0xFF304060));
    addAndMakeVisible(m_logTextEditor);

    // Default flush rate: 10 Hz
    m_flushTimer.startTimer(100);  // 100 ms = 10 Hz
}

DebugLogPanel::~DebugLogPanel()
{
    m_flushTimer.stopTimer();
    detachLogger();
}

void DebugLogPanel::resized()
{
    m_logTextEditor.setBounds(getLocalBounds().reduced(2));
}

void DebugLogPanel::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xFF101820));
    g.setColour(juce::Colour(0xFF304060));
    g.drawRect(getLocalBounds(), 1);
}

void DebugLogPanel::onLogMessage(const juce::String& message)
{
    // This is called from the logger thread, but we batch-append
    // on our own low-rate UI timer, so we don't do anything here.
    // The flushQueueBatch() method on the timer will pull messages.
    (void)message;  // Unused; flush timer batches directly from queue
}

void DebugLogPanel::onLogQueueOverflow(int numDropped)
{
    // Could display an overflow indicator here
    (void)numDropped;
}

void DebugLogPanel::attachLogger(phu::debug::EditorLogger* logger)
{
    m_logger = logger;
    if (m_logger != nullptr) {
        m_logger->setSink(this);
    }
}

void DebugLogPanel::detachLogger()
{
    if (m_logger != nullptr) {
        m_logger->setSink(nullptr);
    }
    m_logger = nullptr;
}

void DebugLogPanel::clearDisplay()
{
    m_logTextEditor.clear();
    m_displayedMessageCount = 0;
    m_displayedLineCount = 0;
}

void DebugLogPanel::setFlushRateHz(double Hz) noexcept
{
    if (Hz <= 0.1)
        Hz = 0.1;  // Minimum 1 message per 10 seconds
    if (Hz > 100.0)
        Hz = 100.0;  // Maximum 100 Hz

    int intervalMs = static_cast<int>(1000.0 / Hz);
    m_flushTimer.startTimer(intervalMs);
}

void DebugLogPanel::flushQueueBatch()
{
    if (m_logger == nullptr)
        return;

    // Drain queue batch
    int numPopped = m_logger->getQueueBatch(
        juce::Span<DebugLogEventQueue::LogEntry>(m_batchBuffer.data(), BATCH_BUFFER_SIZE),
        BATCH_BUFFER_SIZE
    );

    if (numPopped <= 0)
        return;

    // Build single string from all popped entries
    std::ostringstream oss;
    for (int i = 0; i < numPopped; ++i) {
        oss << m_batchBuffer[i].asString().toStdString() << "\n";
    }

    std::string batchStr = oss.str();
    if (batchStr.empty())
        return;

    // Append once to editor (low contention; no per-message overhead)
    m_logTextEditor.insertTextAtCaret(juce::String(batchStr));
    m_displayedMessageCount += numPopped;
    m_displayedLineCount += numPopped;

    // Enforce max display lines
    enforceMaxLines();

    // Check for overflow indicator
    uint32_t droppedNow = m_logger->getDroppedMessageCount();
    if (droppedNow > m_previousDroppedCount) {
        uint32_t newDrops = droppedNow - m_previousDroppedCount;
        m_logTextEditor.insertTextAtCaret(
            "[Logger] Dropped " + juce::String(newDrops) + " messages\n"
        );
        m_previousDroppedCount = droppedNow;
    }
}

void DebugLogPanel::setMaxLines(int maxLines) noexcept
{
    m_maxDisplayLines = (maxLines >= 20) ? maxLines : 20;
}

void DebugLogPanel::enforceMaxLines()
{
    if (m_displayedLineCount <= m_maxDisplayLines)
        return;

    juce::String full = m_logTextEditor.getText();
    juce::StringArray lines;
    lines.addLines(full);

    // Evict exactly EVICT_LINES oldest lines (not the entire excess)
    const int toRemove = std::min(EVICT_LINES, lines.size() - m_maxDisplayLines + EVICT_LINES);
    if (toRemove <= 0 || toRemove >= lines.size())
        return;

    juce::String newText;
    for (int i = toRemove; i < lines.size(); ++i)
    {
        if (i > toRemove)
            newText += "\n";
        newText += lines[i];
    }

    m_logTextEditor.setText(newText, false);
    m_displayedLineCount = lines.size() - toRemove;
}

void DebugLogPanel::LogFlushTimer::timerCallback()
{
    if (m_parent != nullptr)
        m_parent->flushQueueBatch();
}

#endif // PHU_DEBUG_UI

