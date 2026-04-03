#if PHU_DEBUG_UI // Debug builds only

#include "EditorLogger.h"

namespace phu {
namespace debug {

void EditorLogger::logMessage(const juce::String& message) {
    // Convert JUCE String to UTF-8 bytes
    const char* utf8 = message.toRawUTF8();
    const size_t len = std::strlen(utf8);

    // Try to push to lock-free MPSC queue
    if (!m_queue.tryPush(utf8, static_cast<uint16_t>(len))) {
        // Queue full; increment dropped counter
        m_droppedMessages.fetch_add(1, std::memory_order_relaxed);
    }

    // Note: Sink (DebugLogPanel) calls getQueueBatch() at its own rate
    // No AsyncUpdater here; decoupled from UI thread
}

} // namespace debug
} // namespace phu

#endif // PHU_DEBUG_UI

