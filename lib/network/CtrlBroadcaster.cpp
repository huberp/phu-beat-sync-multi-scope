#include "CtrlBroadcaster.h"

#include "../StringUtil.h"
#include "../debug/EditorLogger.h"
#include <chrono>
#include <cstring>
#include <thread>
#include <juce_core/juce_core.h>

#ifndef NDEBUG
    #include <cstdio>
#endif

// Include socket headers only in implementation file (needed for sendto/recvfrom)
#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #define INVALID_SOCKET_VALUE INVALID_SOCKET
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #define INVALID_SOCKET_VALUE (-1)
#endif

namespace phu {
namespace network {

// Protocol magic number: "CTRL" in ASCII
static constexpr uint32_t CTRL_MAGIC = 0x4354524C;

#ifndef NDEBUG
static const char* ctrlEventTypeToString(const CtrlEventType eventType) {
    switch (eventType) {
        case CtrlEventType::Announce:
            return "Announce";
        case CtrlEventType::LabelChange:
            return "LabelChange";
        case CtrlEventType::RangeChange:
            return "RangeChange";
        case CtrlEventType::Goodbye:
            return "Goodbye";
        case CtrlEventType::PeersBroadcastOnly:
            return "PeersBroadcastOnly";
        default:
            return "Unknown";
    }
}
#endif

// ============================================================================
// Construction
// ============================================================================

CtrlBroadcaster::CtrlBroadcaster()
    : MulticastBroadcasterBase(MULTICAST_GROUP, MULTICAST_PORT) {}

// ============================================================================
// Shutdown hook
// ============================================================================

void CtrlBroadcaster::onShutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_remoteInfos.clear();
}

// ============================================================================
// Sending
// ============================================================================

bool CtrlBroadcaster::sendCtrl(CtrlEventType eventType, const char* label,
                                float displayRangeBeats, float bpm,
                                double sampleRate, uint32_t maxBufferSize,
                                const uint8_t colourRGBA[4],
                                const char* pluginType, uint32_t pluginVersion) {
    if (!networkInitialized || sendSocket == INVALID_SOCKET_VALUE)
        return false;

    CtrlPacket packet;
    std::memset(&packet, 0, sizeof(packet));
    packet.magic             = CTRL_MAGIC;
    packet.version           = PROTOCOL_VERSION;
    packet.instanceID        = instanceID;
    packet.sequenceNumber    = m_sequenceNumber++;
    packet.eventType         = static_cast<uint8_t>(eventType);
    packet.instanceIndex     = m_instanceIndex.load(std::memory_order_relaxed);
    packet.sampleRate        = sampleRate;
    packet.maxBufferSize     = maxBufferSize;
    packet.displayRangeBeats = displayRangeBeats;
    packet.bpm               = bpm;
    if (label != nullptr) {
        phu::StringUtil::safe_strncpy(packet.channelLabel, label, 32);
    }
    if (colourRGBA != nullptr)
        std::memcpy(packet.colourRGBA, colourRGBA, 4);
    if (pluginType != nullptr) {
        phu::StringUtil::safe_strncpy(packet.pluginType, pluginType, 16);
    }
    packet.pluginVersion = pluginVersion;

    auto* addr = static_cast<sockaddr_in*>(multicastAddr);
    int bytesSent =
        sendto(sendSocket, reinterpret_cast<const char*>(&packet), sizeof(packet), 0,
               reinterpret_cast<struct sockaddr*>(addr), sizeof(sockaddr_in));

#ifndef NDEBUG
    const juce::String eventName = juce::String(ctrlEventTypeToString(eventType));
    const juce::String labelText = juce::String(packet.channelLabel);

    if (bytesSent <= 0) {
        auto msg = juce::String("[CtrlBroadcaster] sendCtrl failed (event=") +
                   eventName + ", ch=" + juce::String(packet.instanceIndex) +
                   ", label='" + labelText + "', bytesSent=" + juce::String(bytesSent) + ")";
        LOG_MESSAGE(m_editorLogger, msg);
    } else {
        auto msg = juce::String("[CtrlBroadcaster] sent event ") +
                   eventName + " from instance " + juce::String(packet.instanceID) +
                   " (ch=" + juce::String(packet.instanceIndex) +
                   ", label='" + labelText + "')";
        LOG_MESSAGE(m_editorLogger, msg);
    }
#endif

    return bytesSent > 0;
}

// ============================================================================
// Receiving
// ============================================================================

std::vector<RemoteInstanceInfo> CtrlBroadcaster::getRemoteInfos() {
    std::vector<RemoteInstanceInfo> result;
    getRemoteInfos(result);
    return result;
}

void CtrlBroadcaster::getRemoteInfos(std::vector<RemoteInstanceInfo>& out) {
    out.clear();
    const int64_t now = getCurrentTimeMs();

    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_remoteInfos.begin();
    while (it != m_remoteInfos.end()) {
        const bool stale   = (now - it->second.lastSeenMs > STALE_TIMEOUT_MS);
        const bool offline = !it->second.isOnline;

        if (offline || stale) {
            // Remove instances that said Goodbye or have been silent too long
            it = m_remoteInfos.erase(it);
        } else {
            out.push_back(it->second);
            ++it;
        }
    }
}

void CtrlBroadcaster::receiverThreadRun() {
    CtrlPacket packet;

    while (running.load()) {
        if (!receiveEnabled.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        // Receive packet (blocking with 100ms timeout set in socket options)
        int bytesReceived =
            recvfrom(recvSocket, reinterpret_cast<char*>(&packet), sizeof(packet), 0,
                     nullptr, nullptr);

        if (bytesReceived < static_cast<int>(sizeof(CtrlPacket)))
            continue; // Timeout or truncated packet

        // Validate magic and version
        if (packet.magic != CTRL_MAGIC)
            continue;
        if (packet.version != PROTOCOL_VERSION)
            continue;

        // Ignore our own broadcasts
        if (packet.instanceID == instanceID)
            continue;

        const CtrlEventType eventType = static_cast<CtrlEventType>(packet.eventType);

#ifndef NDEBUG
        bool shouldLogReceive = false;
        char loggedLabel[32] = {};
#endif

        {
            std::lock_guard<std::mutex> lock(m_mutex);

            if (eventType == CtrlEventType::Goodbye) {
                // Immediate offline — do not wait for stale timeout
                auto infoIt = m_remoteInfos.find(packet.instanceID);
                if (infoIt != m_remoteInfos.end())
                    infoIt->second.isOnline = false;
            } else {
                // Announce, LabelChange, RangeChange — upsert the info record
                auto& info             = m_remoteInfos[packet.instanceID];
                info.instanceID        = packet.instanceID;
                info.instanceIndex     = packet.instanceIndex;
                info.lastSeenMs        = getCurrentTimeMs();
                info.isOnline          = true;
                info.sampleRate        = packet.sampleRate;
                info.maxBufferSize     = packet.maxBufferSize;
                info.displayRangeBeats = packet.displayRangeBeats;
                info.bpm               = packet.bpm;
                std::memcpy(info.channelLabel, packet.channelLabel, 32);
                info.channelLabel[31]  = '\0'; // ensure null-terminated
                std::memcpy(info.colourRGBA, packet.colourRGBA, 4);
                std::memcpy(info.pluginType, packet.pluginType, 16);
                info.pluginType[15]    = '\0'; // ensure null-terminated
                info.pluginVersion     = packet.pluginVersion;

                if (eventType == CtrlEventType::PeersBroadcastOnly)
                    m_receivedPeersBroadcastOnly.store(true, std::memory_order_relaxed);

                // Increment inbound rate counter (consumed by processor timer EWMA)
                m_inboundCtrlPackets.fetch_add(1, std::memory_order_relaxed);

#ifndef NDEBUG
                shouldLogReceive = true;
                std::memcpy(loggedLabel, info.channelLabel, sizeof(loggedLabel));
#endif
            }
        }

#ifndef NDEBUG
        if (shouldLogReceive) {
            const juce::String eventName = juce::String(ctrlEventTypeToString(eventType));
            auto msg = juce::String("[CtrlBroadcaster] received event ") +
                       eventName +
                       " from instance " + juce::String(packet.instanceID) +
                       " (ch=" + juce::String(packet.instanceIndex) +
                       ", label='" + juce::String(loggedLabel) + "')";
            LOG_MESSAGE(m_editorLogger, msg);
        }
#endif
    }
}

} // namespace network
} // namespace phu
