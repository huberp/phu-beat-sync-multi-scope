#include "CtrlBroadcaster.h"

#include <cstring>

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
                                const uint8_t colourRGBA[4]) {
    if (!networkInitialized || sendSocket == INVALID_SOCKET_VALUE)
        return false;

    CtrlPacket packet;
    std::memset(&packet, 0, sizeof(packet));
    packet.magic             = CTRL_MAGIC;
    packet.version           = PROTOCOL_VERSION;
    packet.instanceID        = instanceID;
    packet.sequenceNumber    = m_sequenceNumber++;
    packet.eventType         = static_cast<uint8_t>(eventType);
    packet.sampleRate        = sampleRate;
    packet.maxBufferSize     = maxBufferSize;
    packet.displayRangeBeats = displayRangeBeats;
    packet.bpm               = bpm;
    if (label != nullptr) {
        std::strncpy(packet.channelLabel, label, 31);
        packet.channelLabel[31] = '\0';
    }
    if (colourRGBA != nullptr)
        std::memcpy(packet.colourRGBA, colourRGBA, 4);

    auto* addr = static_cast<sockaddr_in*>(multicastAddr);
    int bytesSent =
        sendto(sendSocket, reinterpret_cast<const char*>(&packet), sizeof(packet), 0,
               reinterpret_cast<struct sockaddr*>(addr), sizeof(sockaddr_in));

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
            info.lastSeenMs        = getCurrentTimeMs();
            info.isOnline          = true;
            info.sampleRate        = packet.sampleRate;
            info.maxBufferSize     = packet.maxBufferSize;
            info.displayRangeBeats = packet.displayRangeBeats;
            info.bpm               = packet.bpm;
            std::memcpy(info.channelLabel, packet.channelLabel, 32);
            info.channelLabel[31]  = '\0'; // ensure null-terminated
            std::memcpy(info.colourRGBA, packet.colourRGBA, 4);

#ifndef NDEBUG
            std::fprintf(stderr,
                "[CtrlBroadcaster] received event %u from instance %u (label='%s')\n",
                static_cast<unsigned>(eventType),
                packet.instanceID,
                info.channelLabel);
#endif
        }
    }
}

} // namespace network
} // namespace phu
