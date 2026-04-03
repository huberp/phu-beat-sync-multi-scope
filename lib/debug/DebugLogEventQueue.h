#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <cstring>
#include <array>

/**
 * Thread-safe, bounded ring-buffer MPSC (multi-producer, single-consumer) queue
 * for low-latency debug log message batching.
 *
 * Multiple threads can call tryPush(); single consumer calls popBatch().
 * No locks; uses atomic indices and dynamic storage for message content.
 *
 * Design:
 * - Fixed-size ring buffer for indices (NUM_SLOTS entries)
 * - Dynamic string storage for message content (each slot up to MAX_MESSAGE_LENGTH)
 * - Atomic write/read heads prevent race conditions
 * - Writers check capacity atomically; consumers batch-drain
 */
class DebugLogEventQueue
{
public:
    static constexpr int NUM_SLOTS = 512;               // Ring buffer size
    static constexpr int MAX_MESSAGE_LENGTH = 256;      // Max bytes per message
    static constexpr int BATCH_SIZE = 32;               // Max messages per popBatch call

    struct LogEntry
    {
        std::array<char, MAX_MESSAGE_LENGTH> buffer;
        uint16_t length = 0;  // Actual message length

        juce::String asString() const
        {
            return juce::String::fromUTF8(buffer.data(), static_cast<int>(length));
        }
    };

    DebugLogEventQueue() = default;
    ~DebugLogEventQueue() = default;

    // Deleted copy/move (unsafe for concurrent access)
    DebugLogEventQueue(const DebugLogEventQueue&) = delete;
    DebugLogEventQueue& operator=(const DebugLogEventQueue&) = delete;
    DebugLogEventQueue(DebugLogEventQueue&&) = delete;
    DebugLogEventQueue& operator=(DebugLogEventQueue&&) = delete;

    /**
     * Try to enqueue a UTF-8 log message.
     * Thread-safe; can be called from any number of threads.
     *
     * @param utf8 Pointer to UTF-8 bytes (may not be null-terminated)
     * @param length Number of bytes to copy (clamped to MAX_MESSAGE_LENGTH - 1)
     * @return true if message was enqueued; false if buffer full
     */
    bool tryPush(const char* utf8, uint16_t length) noexcept
    {
        if (!utf8 || length == 0)
            return false;

        // Clamp message length to buffer size (leave room for null terminator internally)
        uint16_t actualLength = juce::jmin(static_cast<uint16_t>(length), 
                                           static_cast<uint16_t>(MAX_MESSAGE_LENGTH - 1));

        // Load current write head atomically
        int writeIdx = m_writeHead.load(std::memory_order_acquire);
        int nextWrite = (writeIdx + 1) % NUM_SLOTS;

        // Check if buffer full (next write == read head)
        if (nextWrite == m_readHead.load(std::memory_order_acquire))
            return false;

        // Copy message into slot
        auto& slot = m_slots[writeIdx];
        std::memcpy(slot.buffer.data(), utf8, actualLength);
        slot.buffer[actualLength] = '\0';  // Null-terminate for safety
        slot.length = actualLength;

        // Atomically advance write head to publish the message
        m_writeHead.store(nextWrite, std::memory_order_release);
        return true;
    }

    /**
     * Dequeue up to maxItems messages into the output array.
     * Single-consumer only; safe to call only from one thread.
     *
     * @param out Output span to fill with LogEntry copies
     * @param maxItems Maximum number of items to pop (clamped to BATCH_SIZE)
     * @return Number of messages actually popped (0 if queue empty)
     */
    int popBatch(juce::Span<LogEntry> out, int maxItems) noexcept
    {
        maxItems = juce::jmin(maxItems, BATCH_SIZE);
        maxItems = juce::jmin(maxItems, static_cast<int>(out.size()));

        if (maxItems <= 0)
            return 0;

        int readIdx = m_readHead.load(std::memory_order_acquire);
        int writeIdx = m_writeHead.load(std::memory_order_acquire);

        if (readIdx == writeIdx)
            return 0;  // Queue empty

        // Copy available messages into output
        int itemsPopped = 0;
        while (itemsPopped < maxItems && readIdx != writeIdx)
        {
            out[itemsPopped] = m_slots[readIdx];
            readIdx = (readIdx + 1) % NUM_SLOTS;
            ++itemsPopped;
        }

        // Atomically update read head to consume the batch
        m_readHead.store(readIdx, std::memory_order_release);
        return itemsPopped;
    }

    /**
     * Check if queue has any pending messages (non-blocking snapshot).
     * @return true if at least one message is available
     */
    bool hasPending() const noexcept
    {
        return m_readHead.load(std::memory_order_acquire) != 
               m_writeHead.load(std::memory_order_acquire);
    }

    /**
     * Query approximate queue fill ratio (for diagnostics/monitoring).
     * Not exact due to concurrent access, but useful for overflow detection.
     * @return Ratio in range [0.0, 1.0]
     */
    double getApproximateFillRatio() const noexcept
    {
        int read = m_readHead.load(std::memory_order_acquire);
        int write = m_writeHead.load(std::memory_order_acquire);
        int used = (write - read + NUM_SLOTS) % NUM_SLOTS;
        return static_cast<double>(used) / NUM_SLOTS;
    }

private:
    std::array<LogEntry, NUM_SLOTS> m_slots;
    std::atomic<int> m_writeHead { 0 };  // Protected by atomic; producers advance
    std::atomic<int> m_readHead { 0 };   // Protected by atomic; consumer advances
};
