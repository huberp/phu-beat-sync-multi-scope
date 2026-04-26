#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

class PhuBeatSyncMultiScopeAudioProcessor;
namespace phu { namespace debug { class EditorLogger; } }

#ifndef NDEBUG // Debug builds only

#include "debug/DebugLogSink.h"
#include "debug/DebugLogEventQueue.h"

/**
 * Reusable debug log display panel component.
 *
 * Implements DebugLogSink to consume messages from EditorLogger at a decoupled, low rate.
 * Owns a TextEditor for display and a dedicated low-rate timer (8–12 Hz) to batch-append messages.
 * This decouples the logger from the 60 Hz scope rendering timer.
 *
 * Features:
 * - Bounded history (max ~1000 lines; older messages scroll off)
 * - Batch append logic (collects up to N messages, joins into one string, appends once)
 * - Overflow indicator (optional; can display drop count)
 * - Independent UI timer at configurable rate (e.g., 10 Hz)
 */
class DebugLogPanel : public juce::Component, public DebugLogSink
{
  public:
    explicit DebugLogPanel(PhuBeatSyncMultiScopeAudioProcessor* processor);
    ~DebugLogPanel() override;

    // Component rendering
    void resized() override;
    void paint(juce::Graphics& g) override;

    // DebugLogSink implementation
    void onLogMessage(const juce::String& message) override;
    void onLogQueueOverflow(int numDropped) override;

    /**
     * Attach the logger to this panel.
     * Panel will immediately start consuming messages at its own rate.
     */
    void attachLogger(phu::debug::EditorLogger* logger);

    /**
     * Detach the logger (call when logger is destroyed).
     */
    void detachLogger();

    /**
     * Query current message count in display (for diagnostics)
     */
    int getDisplayMessageCount() const noexcept { return m_displayedMessageCount; }

    /**
     * Clear all displayed messages
     */
    void clearDisplay();

    /**
     * Set the drain/flush rate in Hz (e.g., 10 for 10 Hz = ~100ms batches)
     */
    void setFlushRateHz(double Hz) noexcept;

    /**
     * Set the maximum number of lines to keep in the display.
     * When the limit is exceeded the oldest EVICT_LINES lines are removed.
     * Default: 150.  Minimum enforced: 20.
     */
    void setMaxLines(int maxLines) noexcept;

private:
    static constexpr int EVICT_LINES    = 10;   // Lines dropped per eviction
    static constexpr int BATCH_BUFFER_SIZE = 20;

    class PhuBeatSyncMultiScopeAudioProcessor* m_processor = nullptr;
    phu::debug::EditorLogger* m_logger = nullptr;
    std::array<DebugLogEventQueue::LogEntry, BATCH_BUFFER_SIZE> m_batchBuffer;

    juce::TextEditor m_logTextEditor;
    
    int m_displayedMessageCount = 0;
    int m_displayedLineCount = 0;
    int m_maxDisplayLines = 150;          // Configurable via setMaxLines()
    uint32_t m_previousDroppedCount = 0;  // To detect overflow events

    // Low-rate timer for draining queue and batch-appending to UI
    class LogFlushTimer : public juce::Timer
    {
    public:
        explicit LogFlushTimer(DebugLogPanel* parent) : m_parent(parent) {}
        void timerCallback() override;

    private:
        DebugLogPanel* m_parent = nullptr;
    };

    LogFlushTimer m_flushTimer { this };

    void flushQueueBatch();
    void enforceMaxLines();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DebugLogPanel)
};

#else // Release builds
// Empty stub for release builds
class DebugLogPanel : public juce::Component
{
  public:
    explicit DebugLogPanel(PhuBeatSyncMultiScopeAudioProcessor*) {}
    void attachLogger(phu::debug::EditorLogger*) {}
    void detachLogger() {}
    void clearDisplay() {}
    void setFlushRateHz(double) {}
    void setMaxLines(int) {}
};
  #endif // NDEBUG
