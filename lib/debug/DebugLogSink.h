#pragma once

#include <juce_core/juce_core.h>

/**
 * Abstract sink for consuming batched debug log messages.
 * Allows EditorLogger to operate independently of UI implementation details.
 * A DebugLogPanel or other UI component implements this interface to receive messages.
 */
class DebugLogSink
{
public:
    virtual ~DebugLogSink() = default;

    /**
     * Append a single message to the sink display.
     * Called in batches from the low-rate UI timer rather than per-message.
     *
     * @param message UTF-8 encoded log message (may contain newlines)
     */
    virtual void onLogMessage(const juce::String& message) = 0;

    /**
     * Called when the queue overflows (optional; default no-op).
     * Sink can increment a counter or display a warning indicator.
     *
     * @param numDropped Number of messages that could not fit in the queue
     */
    virtual void onLogQueueOverflow(int numDropped)
    {
        // Default: no-op
        (void)numDropped;
    }
};
