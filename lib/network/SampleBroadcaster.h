#pragma once

#include "MulticastBroadcasterBase.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace phu {
namespace network {

/**
 * SampleBroadcaster: UDP multicast broadcaster/receiver for sharing raw audio
 * sample streams between plugin instances on the same machine (localhost only).
 *
 * Sender path: the audio thread pushes every mono sample into a ring buffer;
 * the processor timer (~30 Hz) drains full chunks and sends each chunk as a
 * RawSamplesPacket tagged with the PPQ position of its first sample.
 *
 * Receiver path: a dedicated background thread receives packets and stores
 * the latest packet per remote instance in a mutex-protected map.  The UI
 * thread calls getReceivedPackets() to get a snapshot and passes it to
 * ScopeDisplay::setRemoteRawData(), which performs all projection and metric
 * accumulation.
 *
 * Thread safety:
 * - broadcastRawSamples() is safe to call from any single thread (timer/message)
 * - getReceivedPackets() is safe to call from any thread
 * - Receiver thread is managed internally
 *
 * LOCALHOST-ONLY: see MulticastBroadcasterBase for the MTU assumption.
 */
class SampleBroadcaster : public MulticastBroadcasterBase {
  public:
    /** Multicast group address (administratively scoped, local org). */
    static constexpr const char* MULTICAST_GROUP = "239.255.42.1";

    /** UDP port for sample multicasts (separate from spectrum port 49421 and CTRL port 49423). */
    static constexpr int MULTICAST_PORT = 49422;

    /**
     * Number of raw mono samples sent per packet.
     * Chosen so that one packet covers approximately 33 ms at 44.1 kHz.
     * Loopback only — packet size is not constrained by the Ethernet MTU.
     */
    static constexpr int BROADCAST_CHUNK_SAMPLES = 1470;

    /** Protocol version — bumped when wire format changes. */
    static constexpr uint32_t PROTOCOL_VERSION = 3;

    /** Time in milliseconds after which a remote instance is considered stale. */
    static constexpr int64_t STALE_TIMEOUT_MS = 3000;

    // Raw-samples packet structure (packed for network transmission).
    // Carries a complete chunk of raw mono float samples tagged with the PPQ
    // position of the first sample and enough DAW context for the receiver to
    // reconstruct per-sample PPQ via:
    //   ppq_i = ppqOfFirstSample + i * (bpm / (60.0 * receiverSampleRate))
    #pragma pack(push, 1)
    struct RawSamplesPacket {
        uint32_t magic;               // Protocol magic: 0x534D504C ("SMPL")
        uint32_t version;             // Protocol version: 3
        uint32_t instanceID;          // Unique instance identifier
        uint32_t sequenceNumber;      // Monotonic per-sender, wraps at UINT32_MAX
        double   ppqOfFirstSample;    // Absolute PPQ position of samples[0]
        double   bpm;                 // Sender BPM (used by receiver for ppq_i reconstruction)
        float    displayRangeBeats;   // Sender's display range (beats)
        uint16_t numSamples;          // Number of samples in this packet (≤ BROADCAST_CHUNK_SAMPLES)
        float    samples[BROADCAST_CHUNK_SAMPLES]; // Raw mono float audio [-1, +1]
    };
    #pragma pack(pop)

    /**
     * Received raw-sample packet from a remote plugin instance.
     * Mirrors RawSamplesPacket plus a local receive timestamp for staleness checks.
     */
    struct RemoteRawPacket {
        uint32_t instanceID          = 0;
        int64_t  timestamp           = 0;    ///< local receive time (ms) for staleness
        uint32_t sequenceNumber      = 0;
        double   ppqOfFirstSample    = 0.0;
        double   bpm                 = 0.0;
        float    displayRangeBeats   = 1.0f;
        uint16_t numSamples          = 0;
        float    samples[BROADCAST_CHUNK_SAMPLES]{};
    };

    SampleBroadcaster();
    ~SampleBroadcaster() override = default;

    // Delete copy/move constructors
    SampleBroadcaster(const SampleBroadcaster&) = delete;
    SampleBroadcaster& operator=(const SampleBroadcaster&) = delete;

    /** Enable or disable broadcasting (default: disabled). */
    void setBroadcastEnabled(bool enabled) { broadcastEnabled.store(enabled); }

    /** Enable or disable receiving (default: enabled after initialize). */
    void setReceiveEnabled(bool enabled) { receiveEnabled.store(enabled); }

    /**
     * Broadcast a chunk of raw mono samples to all instances on the multicast group.
     *
     * @param samples            Raw mono float samples [-1, +1]
     * @param numSamples         Number of samples (must be ≤ BROADCAST_CHUNK_SAMPLES)
     * @param ppqOfFirstSample   Absolute PPQ position of samples[0]
     * @param bpm                Current BPM
     * @param displayRangeBeats  Sender's display range in beats
     * @param seqNum             Monotonic sequence number (message-thread only)
     * @return true if the packet was sent, false if disabled or a socket error occurred
     */
    bool broadcastRawSamples(const float* samples, int numSamples,
                             double ppqOfFirstSample, double bpm,
                             float displayRangeBeats, uint32_t seqNum);

    /**
     * Get the latest received raw packet for each active remote instance.
     * Returns a snapshot — does not drain a queue. Stale entries (> STALE_TIMEOUT_MS)
     * are automatically pruned.
     *
     * @return Vector of the latest raw packet per remote instance
     */
    std::vector<RemoteRawPacket> getReceivedPackets();

    /**
     * Output-parameter variant: fills @p out with the latest snapshot, reusing
     * the vector's existing capacity to avoid per-frame heap allocation.
     * Prefer this overload on hot paths (e.g. 60 Hz UI timer).
     */
    void getReceivedPackets(std::vector<RemoteRawPacket>& out);

    /** Get number of currently active remote instances. */
    int getNumRemoteInstances() const;

  protected:
    // MulticastBroadcasterBase overrides
    void receiverThreadRun() override;
    void onShutdown() override;

  private:
    // Broadcasting state
    std::atomic<bool> broadcastEnabled{false};
    std::atomic<bool> receiveEnabled{true};

    // Mutex-protected map: latest raw packet per remote instance ID.
    // The receiver thread writes, the UI thread reads via getReceivedPackets().
    mutable std::mutex receiveMutex;
    std::map<uint32_t, RemoteRawPacket> latestPackets;

    // Per-sender last sequence number for gap detection (receiver thread only)
    std::map<uint32_t, uint32_t> lastSeqNums;
};

} // namespace network
} // namespace phu
