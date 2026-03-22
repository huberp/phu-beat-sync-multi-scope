#pragma once

#ifndef NDEBUG // Debug builds only

#include <array>
#include <atomic>
#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>

// Forward declaration
class PhuBeatSyncMultiScopeAudioProcessorEditor;

namespace phu {
namespace debug {

/**
 * EditorLogger
 *
 * Custom JUCE Logger that forwards log messages to the plugin editor's log view.
 * Thread-safe and uses AsyncUpdater to ensure GUI updates happen on the message thread.
 *
 * Usage:
 *   editorLogger->logMessage("Your message");
 *   // or
 *   LOG_MESSAGE(editorLogger, "Your message");
 */
class EditorLogger : public juce::Logger, public juce::AsyncUpdater {
  public:
    EditorLogger() = default;
    ~EditorLogger() override = default;

    /**
     * Mark the calling thread as the audio thread for this plugin instance.
     */
    void markCurrentThreadAsAudioThread() noexcept;

    /**
     * Set the editor that will receive log messages
     */
    void setEditor(PhuBeatSyncMultiScopeAudioProcessorEditor* editor);

    /**
     * Clear the editor reference (call when editor is destroyed)
     */
    void clearEditor();

    /**
     * Logger override - called from any thread
     */
    void logMessage(const juce::String& message) override;

  protected:
    /**
     * AsyncUpdater override - called on message thread
     */
    void handleAsyncUpdate() override;

  private:
    static constexpr int rtQueueCapacity = 1024;
    static constexpr size_t rtMaxMessageBytes = 256;

    struct RtSlot {
        std::array<char, rtMaxMessageBytes> text{};
        uint16_t length = 0;
    };

    juce::AbstractFifo rtFifo{rtQueueCapacity};
    std::array<RtSlot, rtQueueCapacity> rtSlots;
    std::atomic<uint32_t> rtDroppedMessages{0};

    std::atomic<uintptr_t> audioThreadId{0};

    juce::CriticalSection nonRealtimeLock;
    juce::StringArray pendingMessages;

    std::atomic<bool> asyncUpdateRequested{false};

    juce::Component::SafePointer<PhuBeatSyncMultiScopeAudioProcessorEditor> editor;

    void requestAsyncUpdate() noexcept;
    void pushRealtime(const juce::String& message) noexcept;

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

#endif // !NDEBUG
