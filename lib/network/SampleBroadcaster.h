#pragma once

#include "MulticastBroadcasterBase.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace phu {
namespace network {

/**
 * SampleBroadcaster: UDP multicast broadcaster/receiver for sharing
 * beat-synced waveform (oscilloscope) data between plugin instances.
 *
 * This extends the SpectrumBroadcaster pattern from phu-splitter,
 * adapted to send beat-synced sample data (BeatSyncBuffer contents)
 * instead of FFT spectrum data. Each packet includes a PPQ reference
 * so receivers can align their display to the same musical position.
 *
 * Architecture:
 * - Sender: called from UI timer thread, copies float BeatSyncBuffer
 *   contents and multicasts via variable-length UDP packets with PPQ info.
 * - Receiver: dedicated background thread receives packets and stores
 *   the latest sample data per remote instance in a mutex-protected map.
 * - Reader: UI thread calls getReceivedSamples() to get a snapshot of all
 *   currently active remote instances' waveform data.
 *
 * Thread safety:
 * - broadcastSamples() is safe to call from any single thread (timer/UI)
 * - getReceivedSamples() is safe to call from any thread
 * - Receiver thread is managed internally
 */
class SampleBroadcaster : public MulticastBroadcasterBase {
  public:
    /** Multicast group address (administratively scoped, local org). */
    static constexpr const char* MULTICAST_GROUP = "239.255.42.1";

    /** UDP port for sample multicasts (separate from spectrum port 49421). */
    static constexpr int MULTICAST_PORT = 49423;

    /** Maximum sample bins to transmit per packet. */
    static constexpr int MAX_SAMPLE_BINS = 2048;

    /** Protocol version — bumped when wire format changes. */
    static constexpr uint32_t PROTOCOL_VERSION = 2;

    /** Time in milliseconds after which a remote instance is considered stale. */
    static constexpr int64_t STALE_TIMEOUT_MS = 3000;

    // Sample packet structure (packed for network transmission)
    #pragma pack(push, 1)
    struct SamplePacket {
        uint32_t magic;                    // Protocol magic: 0x534D504C ("SMPL")
        uint32_t version;                  // Protocol version: 2
        uint32_t instanceID;               // Unique instance identifier
        uint64_t timestamp;                // Timestamp in milliseconds
        double ppqPosition;                // PPQ position reference for beat sync
        double bpm;                        // Current BPM for receiver display sync
        float displayRangeBeats;           // Musical range this buffer covers
        uint16_t numBins;                  // Number of sample bins (up to MAX_SAMPLE_BINS)
        float    samples[MAX_SAMPLE_BINS]; // Float waveform data [-1, +1]
    };
    #pragma pack(pop)

    /**
     * Received sample data from a remote plugin instance (unpacked for rendering).
     * Values are raw linear samples [-1, +1], suitable for direct oscilloscope rendering.
     */
    struct RemoteSampleData {
        uint32_t instanceID = 0;
        int64_t timestamp = 0;
        double ppqPosition = 0.0;
        double bpm = 0.0;
        float displayRangeBeats = 1.0f;
        std::vector<float> samples; ///< linear sample values [-1, +1]
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
     * Set minimum interval between broadcasts (throttling).
     * @param intervalMs Minimum milliseconds between broadcasts (default: 33ms = ~30Hz)
     */
    void setBroadcastInterval(int intervalMs) { minBroadcastIntervalMs = intervalMs; }

    /**
     * Broadcast beat-synced sample data to all instances on the multicast group.
     * Sample values are sent as raw floats in a variable-length packet.
     *
     * @param sampleData   BeatSyncBuffer data array (linear [-1, +1])
     * @param numBins      Number of bins in the sample data array
     * @param ppqPosition  Current PPQ position for beat sync alignment
     * @param bpm          Current BPM
     * @param displayRangeBeats Musical range this buffer covers (e.g., 1.0 = one beat)
     * @return true if broadcast succeeded, false if throttled or error
     */
    bool broadcastSamples(const float* sampleData, int numBins,
                          double ppqPosition, double bpm, float displayRangeBeats);

    /**
     * Get latest received sample data for each active remote instance.
     * Returns a snapshot — does not drain a queue. Stale entries (> STALE_TIMEOUT_MS)
     * are automatically pruned.
     *
     * @return Vector of the latest sample data per remote instance
     */
    std::vector<RemoteSampleData> getReceivedSamples();

    /** Get number of currently active remote instances. */
    int getNumRemoteInstances() const;

  protected:
    // MulticastBroadcasterBase overrides
    void receiverThreadRun() override;
    void onShutdown() override;

  private:
    // Sample-specific state
    std::atomic<bool> broadcastEnabled{false};
    std::atomic<bool> receiveEnabled{true};

    // Broadcast throttling
    int minBroadcastIntervalMs = 33; // ~30 Hz default
    int64_t lastBroadcastTime = 0;

    // Mutex-protected map: latest sample data per remote instance ID.
    // The receiver thread writes, the UI thread reads via getReceivedSamples().
    mutable std::mutex receiveMutex;
    std::map<uint32_t, RemoteSampleData> latestSamples;
};

} // namespace network
} // namespace phu
