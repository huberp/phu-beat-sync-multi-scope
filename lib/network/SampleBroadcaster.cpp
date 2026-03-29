#include "SampleBroadcaster.h"

#include <algorithm>
#include <cstring>

namespace phu {
namespace network {

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
    #define INVALID_SOCKET_VALUE -1
#endif

// Protocol magic number: "SMPL" in ASCII
static constexpr uint32_t PROTOCOL_MAGIC = 0x534D504C;

// Byte offset of the samples array within SamplePacket (fixed header size)
static constexpr int kSamplePacketHeaderSize =
    static_cast<int>(offsetof(SampleBroadcaster::SamplePacket, samples));

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
    latestSamples.clear();
}

// ============================================================================
// Broadcasting
// ============================================================================

bool SampleBroadcaster::broadcastSamples(const float* sampleData, int numBins,
                                         double ppqPosition, double bpm,
                                         float displayRangeBeats) {
    if (!networkInitialized || !broadcastEnabled.load() || sendSocket == INVALID_SOCKET_VALUE) {
        return false;
    }

    // Throttle broadcasts
    int64_t now = getCurrentTimeMs();
    if (now - lastBroadcastTime < minBroadcastIntervalMs) {
        return false; // Throttled
    }
    lastBroadcastTime = now;

    // Create packet
    SamplePacket packet;
    std::memset(&packet, 0, sizeof(packet));
    packet.magic = PROTOCOL_MAGIC;
    packet.version = PROTOCOL_VERSION;
    packet.instanceID = instanceID;
    packet.timestamp = static_cast<uint64_t>(now);
    packet.ppqPosition = ppqPosition;
    packet.bpm = bpm;
    packet.displayRangeBeats = displayRangeBeats;

    // Downsample to MAX_SAMPLE_BINS using stride sampling to preserve the bin-to-position
    // coordinate mapping. A flat first-N copy would transmit only the first half of the
    // sender's range (BeatSyncBuffer has 4096 bins, wire cap is 2048). The receiver
    // reconstructs PPQ as: absolutePpq = senderWindowStart + (j / numBins) * senderRange.
    // With stride = numBins / outputBins, output bin j comes from input bin j * stride,
    // whose normalized position j * stride / numBins = j / outputBins — the same fraction
    // the receiver will compute — so PPQ reconstruction is exact.
    int outputBins = (std::min)(numBins, MAX_SAMPLE_BINS);
    packet.numBins = static_cast<uint16_t>(outputBins);
    if (numBins <= MAX_SAMPLE_BINS) {
        std::memcpy(packet.samples, sampleData, sizeof(float) * static_cast<size_t>(outputBins));
    } else {
        const int stride = numBins / outputBins;
        for (int j = 0; j < outputBins; ++j)
            packet.samples[j] = sampleData[j * stride];
    }

    // Send only the header + the bins actually used (variable-length)
    auto* addr = static_cast<sockaddr_in*>(multicastAddr);
    const int payloadSize = kSamplePacketHeaderSize + static_cast<int>(sizeof(float)) * outputBins;
    int bytesSent =
        sendto(sendSocket, reinterpret_cast<const char*>(&packet), payloadSize, 0,
               reinterpret_cast<struct sockaddr*>(addr), sizeof(sockaddr_in));

    return bytesSent > 0;
}

// ============================================================================
// Receiving
// ============================================================================

std::vector<SampleBroadcaster::RemoteSampleData> SampleBroadcaster::getReceivedSamples() {
    std::vector<RemoteSampleData> results;
    getReceivedSamples(results);
    return results;
}

void SampleBroadcaster::getReceivedSamples(std::vector<RemoteSampleData>& out) {
    out.clear();
    int64_t now = getCurrentTimeMs();

    std::lock_guard<std::mutex> lock(receiveMutex);

    // Collect all non-stale entries and prune stale ones
    auto it = latestSamples.begin();
    while (it != latestSamples.end()) {
        if (now - it->second.timestamp > STALE_TIMEOUT_MS) {
            it = latestSamples.erase(it); // Prune stale entry
        } else {
            out.push_back(it->second);
            ++it;
        }
    }
}

int SampleBroadcaster::getNumRemoteInstances() const {
    std::lock_guard<std::mutex> lock(receiveMutex);
    return static_cast<int>(latestSamples.size());
}

void SampleBroadcaster::receiverThreadRun() {
    SamplePacket packet;

    while (running.load()) {
        if (!receiveEnabled.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        // Receive packet (blocking with 100ms timeout set in socket options)
        int bytesReceived =
            recvfrom(recvSocket, reinterpret_cast<char*>(&packet), sizeof(packet), 0, nullptr, nullptr);

        if (bytesReceived < kSamplePacketHeaderSize) {
            // Not enough data for a valid header — timeout or truncated packet
            continue;
        }

        // Validate packet
        if (packet.magic != PROTOCOL_MAGIC) {
            continue; // Not our protocol
        }

        // Reject packets from incompatible protocol versions
        if (packet.version != PROTOCOL_VERSION) {
            continue;
        }

        // Ignore our own broadcasts
        if (packet.instanceID == instanceID) {
            continue;
        }

        // Validate bin count and that we received enough bytes for the declared bins
        if (packet.numBins == 0 || packet.numBins > MAX_SAMPLE_BINS) {
            continue;
        }
        const int expectedSize = kSamplePacketHeaderSize + static_cast<int>(sizeof(float)) * packet.numBins;
        if (bytesReceived < expectedSize) {
            continue;
        }

        // Copy float samples directly and store in map
        RemoteSampleData data;
        data.instanceID        = packet.instanceID;
        data.timestamp         = getCurrentTimeMs(); // Use local time for staleness check
        data.ppqPosition       = packet.ppqPosition;
        data.bpm               = packet.bpm;
        data.displayRangeBeats = packet.displayRangeBeats;
        data.samples.resize(packet.numBins);
        std::memcpy(data.samples.data(), packet.samples, sizeof(float) * packet.numBins);

        {
            std::lock_guard<std::mutex> lock(receiveMutex);
            latestSamples[packet.instanceID] = std::move(data);
        }
    }
}

} // namespace network
} // namespace phu
