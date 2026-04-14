#include "SampleBroadcaster.h"

#include <algorithm>
#include <cstring>

#ifndef NDEBUG
    #include <cstdio>
#endif

// Socket headers must be included before any namespace block.
#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
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
    #define INVALID_SOCKET_VALUE -1
#endif

namespace phu {
namespace network {

// Protocol magic number: "SMPL" in ASCII
static constexpr uint32_t PROTOCOL_MAGIC = 0x534D504C;

// Fixed header size in bytes (byte offset of the samples array within RawSamplesPacket)
static constexpr int kRawPacketHeaderSize =
    static_cast<int>(offsetof(SampleBroadcaster::RawSamplesPacket, samples));

// ============================================================================
// Construction
// ============================================================================

SampleBroadcaster::SampleBroadcaster()
    : MulticastBroadcasterBase(MULTICAST_GROUP, MULTICAST_PORT) {}

// ============================================================================
// Shutdown hook
// ============================================================================

void SampleBroadcaster::onShutdown() {
    std::lock_guard<std::mutex> lock(receiveMutex);
    latestPackets.clear();
#ifndef NDEBUG
    lastSeqNums.clear();
#endif
}

// ============================================================================
// Broadcasting
// ============================================================================

bool SampleBroadcaster::broadcastRawSamples(const float* samples, int numSamples,
                                            double ppqOfFirstSample, double bpm,
                                            float displayRangeBeats, uint32_t seqNum) {
    if (!networkInitialized || !broadcastEnabled.load() || sendSocket == INVALID_SOCKET_VALUE)
        return false;

    if (numSamples <= 0 || numSamples > BROADCAST_CHUNK_SAMPLES)
        return false;

    RawSamplesPacket packet;
    std::memset(&packet, 0, sizeof(packet));
    packet.magic             = PROTOCOL_MAGIC;
    packet.version           = PROTOCOL_VERSION;
    packet.instanceID        = instanceID;
    packet.instanceIndex     = m_instanceIndex.load(std::memory_order_relaxed);
    packet.sequenceNumber    = seqNum;
    packet.ppqOfFirstSample  = ppqOfFirstSample;
    packet.bpm               = bpm;
    packet.displayRangeBeats = displayRangeBeats;
    packet.numSamples        = static_cast<uint16_t>(numSamples);
    std::memcpy(packet.samples, samples, sizeof(float) * static_cast<size_t>(numSamples));

    auto* addr = static_cast<sockaddr_in*>(multicastAddr);
    const int payloadSize = kRawPacketHeaderSize + static_cast<int>(sizeof(float)) * numSamples;
    int bytesSent =
        sendto(sendSocket, reinterpret_cast<const char*>(&packet), static_cast<size_t>(payloadSize), 0,
               reinterpret_cast<struct sockaddr*>(addr), sizeof(sockaddr_in));

#ifndef NDEBUG
    if (bytesSent <= 0)
        std::fprintf(stderr,
            "[SampleBroadcaster] broadcastRawSamples failed (numSamples=%d, bytesSent=%d)\n",
            numSamples, bytesSent);
#endif

    return bytesSent > 0;
}

// ============================================================================
// Receiving
// ============================================================================

std::vector<SampleBroadcaster::RemoteRawPacket> SampleBroadcaster::getReceivedPackets() {
    std::vector<RemoteRawPacket> results;
    getReceivedPackets(results);
    return results;
}

void SampleBroadcaster::getReceivedPackets(std::vector<RemoteRawPacket>& out) {
    out.clear();
    int64_t now = getCurrentTimeMs();

    std::lock_guard<std::mutex> lock(receiveMutex);

    auto it = latestPackets.begin();
    while (it != latestPackets.end()) {
        if (now - it->second.timestamp > STALE_TIMEOUT_MS) {
            it = latestPackets.erase(it); // Prune stale entry
        } else {
            out.push_back(it->second);
            ++it;
        }
    }
}

int SampleBroadcaster::getNumRemoteInstances() const {
    std::lock_guard<std::mutex> lock(receiveMutex);
    return static_cast<int>(latestPackets.size());
}

void SampleBroadcaster::receiverThreadRun() {
    RawSamplesPacket packet;

    while (running.load()) {
        if (!receiveEnabled.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        // Receive packet (blocking with 100ms timeout set in socket options)
        int bytesReceived =
            recvfrom(recvSocket, reinterpret_cast<char*>(&packet), sizeof(packet), 0, nullptr, nullptr);

        if (bytesReceived < kRawPacketHeaderSize)
            continue; // Timeout or truncated header

        // Validate magic and version
        if (packet.magic != PROTOCOL_MAGIC)
            continue;
        if (packet.version != PROTOCOL_VERSION)
            continue;

        // Ignore our own broadcasts
        if (packet.instanceID == instanceID)
            continue;

        // Validate sample count
        if (packet.numSamples == 0 || packet.numSamples > BROADCAST_CHUNK_SAMPLES)
            continue;

        const int expectedSize = kRawPacketHeaderSize + static_cast<int>(sizeof(float)) * packet.numSamples;
        if (bytesReceived < expectedSize)
            continue;

#ifndef NDEBUG
        // Check sequence number continuity — log gaps (loopback should never drop)
        {
            auto seqIt = lastSeqNums.find(packet.instanceID);
            if (seqIt != lastSeqNums.end()) {
                const uint32_t expected = seqIt->second + 1;
                if (packet.sequenceNumber != expected) {
                    std::fprintf(stderr,
                        "[SampleBroadcaster] seq gap from instance %u: expected %u got %u\n",
                        packet.instanceID, expected, packet.sequenceNumber);
                }
            }
            lastSeqNums[packet.instanceID] = packet.sequenceNumber;
        }
#endif

        RemoteRawPacket data;
        data.instanceID       = packet.instanceID;
        data.instanceIndex    = packet.instanceIndex;
        data.timestamp        = getCurrentTimeMs();
        data.sequenceNumber   = packet.sequenceNumber;
        data.ppqOfFirstSample = packet.ppqOfFirstSample;
        data.bpm              = packet.bpm;
        data.displayRangeBeats = packet.displayRangeBeats;
        data.numSamples       = packet.numSamples;
        std::memcpy(data.samples, packet.samples, sizeof(float) * packet.numSamples);

        {
            std::lock_guard<std::mutex> lock(receiveMutex);
            // Wrap-safe freshness check: only store if this packet is newer than
            // whatever we already have (or if this is the first packet from this
            // instance). Uses signed subtraction to handle uint32_t wraparound
            // correctly: positive result means 'data' is strictly newer.
            auto it = latestPackets.find(data.instanceID);
            if (it == latestPackets.end() ||
                static_cast<int32_t>(data.sequenceNumber - it->second.sequenceNumber) > 0)
            {
                latestPackets[data.instanceID] = std::move(data);
            }
        }
    }
}

} // namespace network
} // namespace phu
