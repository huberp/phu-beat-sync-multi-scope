#pragma once

#if PHU_DEBUG_UI // Debug builds only

#include <atomic>
#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "DebugLogEventQueue.h"
#include "DebugLogSink.h"

// Forward declaration
class PhuBeatSyncMultiScopeAudioProcessorEditor;

namespace phu {
namespace debug {

/**
 * EditorLogger (Refactored)
 *
 * Custom JUCE Logger that forwards log messages to a DebugLogSink via a lock-free MPSC queue.
 * Thread-safe; all writers (audio and non-audio threads) push to shared queue.
 * Consumer (DebugLogPanel UI timer) drains queue in batches at independent rate.
 * No AsyncUpdater; sink manages its own UI update cadence.
 *
 * Usage:
 *   auto logger = std::make_unique<EditorLogger>();
 *   logger->setSink(&myLogPanel);
 *   juce::Logger::setCurrentLogger(logger.get());
 *   LOG_MESSAGE(logger, "Message from any thread");
 *
 * Design:
 * - All threads call logMessage() → tryPush to MPSC queue
 * - Sink calls getQueueBatch() at its own rate (e.g., 8–12 Hz)
 * - Dropped messages tracked and reported on overflow
 */
class EditorLogger : public juce::Logger {
  public:
    EditorLogger() = default;
    ~EditorLogger() override = default;

    /**
     * Set the sink that will consume batched log messages
     */
    void setSink(DebugLogSink* sink) noexcept
    {
        m_sink.store(sink, std::memory_order_release);
    }

    /**
     * Get current sink (for verification/debugging)
     */
    DebugLogSink* getSink() const noexcept
    {
        return m_sink.load(std::memory_order_acquire);
    }

    /**
     * Logger override - called from any thread
     * Atomically tries to push message to MPSC queue; drops and counts on overflow
     */
    void logMessage(const juce::String& message) override;

    /**
     * Drain up to maxItems messages from the queue into the provided output span.
     * Single-consumer operation; typically called by the sink's UI timer.
     *
     * @param out Output span to fill with LogEntry copies
     * @param maxItems Maximum number to pop (typically 32)
     * @return Number of messages actually popped
     */
    int getQueueBatch(juce::Span<DebugLogEventQueue::LogEntry> out, int maxItems) noexcept
    {
        return m_queue.popBatch(out, maxItems);
    }

    /**
     * Query queue fill ratio (for diagnostics)
     */
    double getQueueFillRatio() const noexcept
    {
        return m_queue.getApproximateFillRatio();
    }

    /**
     * Get count of messages dropped due to queue overflow
     */
    uint32_t getDroppedMessageCount() const noexcept
    {
        return m_droppedMessages.load(std::memory_order_acquire);
    }

    /**
     * Reset dropped message counter (optional; for diagnostics)
     */
    void resetDroppedMessageCount() noexcept
    {
        m_droppedMessages.store(0, std::memory_order_release);
    }

  private:
    DebugLogEventQueue m_queue;
    std::atomic<DebugLogSink*> m_sink { nullptr };
    std::atomic<uint32_t> m_droppedMessages { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EditorLogger)
};

// Convenience macro for instance-scoped logging
#define LOG_MESSAGE(loggerPtr, msg)                                                                \
    do {                                                                                           \
        if ((loggerPtr) != nullptr)                                                                \
            (loggerPtr)->logMessage(msg);                                                          \
    } while (0)

} // namespace debug
} // namespace phu

#else // Release builds

// No-op macro for release builds
#define LOG_MESSAGE(loggerPtr, msg)                                                                \
    do {                                                                                           \
        (void)(loggerPtr);                                                                         \
        (void)(msg);                                                                               \
    } while (0)

#endif // PHU_DEBUG_UI
