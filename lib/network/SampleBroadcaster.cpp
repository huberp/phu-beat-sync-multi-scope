#include "SampleBroadcaster.h"

#include <algorithm>
#include <cmath>
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
static constexpr uint32_t PROTOCOL_VERSION = 1;

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

    // Compress samples (downsample and dB-quantize)
    int outputBins = (std::min)(numBins, MAX_SAMPLE_BINS);
    packet.numBins = static_cast<uint16_t>(outputBins);
    compressSamples(sampleData, numBins, packet.samples, outputBins);

    // Send packet
    auto* addr = static_cast<sockaddr_in*>(multicastAddr);
    int bytesSent =
        sendto(sendSocket, reinterpret_cast<const char*>(&packet), sizeof(packet), 0,
               reinterpret_cast<struct sockaddr*>(addr), sizeof(sockaddr_in));

    return bytesSent > 0;
}

// ============================================================================
// Receiving
// ============================================================================

std::vector<SampleBroadcaster::RemoteSampleData> SampleBroadcaster::getReceivedSamples() {
    std::vector<RemoteSampleData> results;
    int64_t now = getCurrentTimeMs();

    std::lock_guard<std::mutex> lock(receiveMutex);

    // Collect all non-stale entries and prune stale ones
    auto it = latestSamples.begin();
    while (it != latestSamples.end()) {
        if (now - it->second.timestamp > STALE_TIMEOUT_MS) {
            it = latestSamples.erase(it); // Prune stale entry
        } else {
            results.push_back(it->second);
            ++it;
        }
    }

    return results;
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

        if (bytesReceived < static_cast<int>(sizeof(SamplePacket) - MAX_SAMPLE_BINS)) {
            // Not enough data for a valid header — timeout or truncated packet
            continue;
        }

        // Validate packet
        if (packet.magic != PROTOCOL_MAGIC) {
            continue; // Not our protocol
        }

        // Ignore our own broadcasts
        if (packet.instanceID == instanceID) {
            continue;
        }

        // Validate bin count
        if (packet.numBins == 0 || packet.numBins > MAX_SAMPLE_BINS) {
            continue;
        }

        // Decompress and store in map
        RemoteSampleData data;
        data.instanceID = packet.instanceID;
        data.timestamp = getCurrentTimeMs(); // Use local time for staleness check
        data.ppqPosition = packet.ppqPosition;
        data.bpm = packet.bpm;
        data.displayRangeBeats = packet.displayRangeBeats;
        decompressSamples(packet.samples, packet.numBins, data.samples);

        {
            std::lock_guard<std::mutex> lock(receiveMutex);
            latestSamples[packet.instanceID] = std::move(data);
        }
    }
}

// ============================================================================
// Sample Compression (linear 8-bit quantization)
// ============================================================================

void SampleBroadcaster::compressSamples(const float* input, int inputBins,
                                        uint8_t* output, int outputBins) {
    // Quantize linear samples [-1, +1] to 8-bit [0, 255]
    // 0 → -1.0, 128 → 0.0, 255 → +1.0
    auto quantize = [](float sample) -> uint8_t {
        float normalized = (sample + 1.0f) * 0.5f; // [-1,+1] → [0,1]
        normalized = (std::max)(0.0f, (std::min)(1.0f, normalized));
        return static_cast<uint8_t>(normalized * 255.0f);
    };

    if (inputBins <= outputBins) {
        for (int i = 0; i < inputBins; ++i)
            output[i] = quantize(input[i]);
        // Fill remaining bins with midpoint (silence = 0.0)
        for (int i = inputBins; i < outputBins; ++i)
            output[i] = 128;
    } else {
        // Downsample: pick sample with largest absolute value in each range
        float binRatio = static_cast<float>(inputBins) / static_cast<float>(outputBins);
        for (int i = 0; i < outputBins; ++i) {
            float start = static_cast<float>(i) * binRatio;
            float end = static_cast<float>(i + 1) * binRatio;
            int startBin = static_cast<int>(start);
            int endBin = (std::min)(static_cast<int>(std::ceil(end)), inputBins);

            float peakSample = 0.0f;
            for (int j = startBin; j < endBin; ++j) {
                if (std::abs(input[j]) > std::abs(peakSample))
                    peakSample = input[j];
            }
            output[i] = quantize(peakSample);
        }
    }
}

void SampleBroadcaster::decompressSamples(const uint8_t* input, int numBins,
                                          std::vector<float>& output) {
    // Dequantize 8-bit [0, 255] back to linear [-1, +1]
    output.resize(static_cast<size_t>(numBins));

    for (int i = 0; i < numBins; ++i) {
        float normalized = static_cast<float>(input[i]) / 255.0f; // [0, 1]
        output[static_cast<size_t>(i)] = normalized * 2.0f - 1.0f; // [-1, +1]
    }
}

} // namespace network
} // namespace phu
