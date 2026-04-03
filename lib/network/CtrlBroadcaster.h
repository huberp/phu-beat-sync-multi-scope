#pragma once

#include "MulticastBroadcasterBase.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

// Forward declaration to avoid circular dependency
namespace phu { namespace debug { class EditorLogger; } }

namespace phu {
namespace network {

// CtrlEventType: identifies the purpose of each CTRL packet
enum class CtrlEventType : uint8_t {
    Announce    = 0x01,  ///< prepareToPlay, initialize, periodic heartbeat
    LabelChange = 0x02,  ///< user changed the channel label
    RangeChange = 0x03,  ///< displayRangeBeats changed
    Goodbye     = 0x04,  ///< releaseResources / shutdown
  PeersBroadcastOnly = 0x05, ///< request all peers to enter broadcast-only mode
};

// Wire format for CTRL packets (packed, ~96 bytes).
// magic = 0x4354524C ("CTRL")
#pragma pack(push, 1)
struct CtrlPacket {
    uint32_t magic;               // 0x4354524C ("CTRL")
    uint32_t version;             // protocol version (see PROTOCOL_VERSION)
    uint32_t instanceID;
    uint32_t sequenceNumber;      // monotonic, for ordering

    uint8_t  eventType;           // CtrlEventType
    uint8_t  instanceIndex;       // user-assigned channel index [1, 8]
    uint8_t  _pad[2];

    double   sampleRate;          // actual DAW sample rate — fixes ASSUMED_SAMPLE_RATE gap
    uint32_t maxBufferSize;
    float    displayRangeBeats;
    float    bpm;                 // last known BPM, for late-join context

    char     channelLabel[32];    // null-terminated UTF-8, user-assigned
    uint8_t  colourRGBA[4];       // optional user-set colour

    char     pluginType[16];      // null-terminated ASCII plugin-family identifier
    uint32_t pluginVersion;       // plugin version (same encoding as protocol version)

    // Total: ~96 bytes
};
#pragma pack(pop)

// Received state for a remote plugin instance (populated from CtrlPackets)
struct RemoteInstanceInfo {
    uint32_t instanceID        = 0;
    uint8_t  instanceIndex     = 1;   ///< user-assigned channel index [1, 8]
    int64_t  lastSeenMs        = 0;   ///< from getCurrentTimeMs()
    bool     isOnline          = false;
    double   sampleRate        = 44100.0;
    uint32_t maxBufferSize     = 512;
    float    displayRangeBeats = 4.0f;
    float    bpm               = 0.0f;
    char     channelLabel[32]  = {};
    uint8_t  colourRGBA[4]     = {0x88, 0x88, 0x88, 0xFF};
    char     pluginType[16]    = {};      ///< null-terminated ASCII plugin-family identifier
    uint32_t pluginVersion     = 0;       ///< plugin version
};

/**
 * CtrlBroadcaster: UDP multicast broadcaster/receiver for sharing instance
 * identity and control information between plugin instances on the same machine.
 *
 * Sent only on meaningful state changes and as a periodic heartbeat — not on
 * every timer tick.  Uses a dedicated port (49423) separate from SampleBroadcaster
 * (49422), so each subclass has its own socket with no dispatch complexity.
 *
 * Trigger → eventType mapping:
 *   prepareToPlay()        → Announce
 *   releaseResources()     → Goodbye
 *   channel label changed  → LabelChange
 *   displayRangeBeats changes → RangeChange
 *   every 5 s heartbeat    → Announce (late-join safety net)
 *
 * LOCALHOST-ONLY: see MulticastBroadcasterBase for the MTU assumption.
 */
class CtrlBroadcaster : public MulticastBroadcasterBase {
  public:
    /** Multicast group address (same as SampleBroadcaster). */
    static constexpr const char* MULTICAST_GROUP       = "239.255.42.1";

    /** UDP port for CTRL multicasts (dedicated, separate from SMPL port 49422). */
    static constexpr int         MULTICAST_PORT        = 49423;

    /** Minimum interval between periodic heartbeat Announce packets. */
    static constexpr int64_t     HEARTBEAT_INTERVAL_MIN_MS = 5000;

    /** Maximum interval: reached when sustained inbound Ctrl traffic is high. */
    static constexpr int64_t     HEARTBEAT_INTERVAL_MAX_MS = 12000;

    /** Stale timeout: raised to ≥ 2× MAX interval so peers at max cadence survive
     *  at least 2 missed heartbeats before pruning (30 s >> 2 × 12 s). */
    static constexpr int64_t     STALE_TIMEOUT_MS          = 30000;

    /** Protocol version — bumped when wire format changes. */
    static constexpr uint32_t    PROTOCOL_VERSION      = 3;

    CtrlBroadcaster();
    ~CtrlBroadcaster() override = default;

    CtrlBroadcaster(const CtrlBroadcaster&) = delete;
    CtrlBroadcaster& operator=(const CtrlBroadcaster&) = delete;

    /**
     * Set the optional editor logger for debug output.
     * Called from the processor after CtrlBroadcaster construction.
     */
    void setEditorLogger(phu::debug::EditorLogger* logger) { m_editorLogger = logger; }

    /** Enable or disable receiving CTRL packets (default: enabled). */
    void setReceiveEnabled(bool enabled) { receiveEnabled.store(enabled); }
    bool isReceiveEnabled() const { return receiveEnabled.load(); }

    /**
     * Atomically read and reset the inbound Ctrl packet counter.
     * Called once per processor timer tick to feed the EWMA rate estimator.
     * Thread-safe (receiver thread writes, message thread reads).
     */
    uint32_t consumeInboundCount() noexcept {
        return m_inboundCtrlPackets.exchange(0, std::memory_order_relaxed);
    }

    /**
     * Consume one-shot peer command: enter broadcast-only mode.
     * Returns true once after a matching remote command is received.
     */
    bool consumePeersBroadcastOnlyCommand() noexcept {
      return m_receivedPeersBroadcastOnly.exchange(false, std::memory_order_relaxed);
    }

    /**
     * Send a CTRL packet to all instances on the multicast group.
     *
     * @param eventType          The type of event triggering this packet
     * @param label              Channel label (null-terminated, max 31 chars)
     * @param displayRangeBeats  Current display range in beats
     * @param bpm                Current BPM (for late-join context)
     * @param sampleRate         Actual DAW sample rate
     * @param maxBufferSize      Maximum audio buffer size
     * @param colourRGBA         User colour (4-byte RGBA)
     * @param pluginType         Plugin-family identifier (null-terminated, max 15 chars)
     * @param pluginVersion      Plugin version number
     * @return true if the packet was sent successfully
     */
    bool sendCtrl(CtrlEventType eventType, const char* label,
                  float displayRangeBeats, float bpm,
                  double sampleRate, uint32_t maxBufferSize,
                  const uint8_t colourRGBA[4],
                  const char* pluginType, uint32_t pluginVersion);

    /**
     * Fill @p out with the latest info for each online remote instance.
     * Prunes stale (> STALE_TIMEOUT_MS) and offline (Goodbye received) entries.
     * Thread-safe.
     */
    void getRemoteInfos(std::vector<RemoteInstanceInfo>& out);

    /** Value-return variant. */
    std::vector<RemoteInstanceInfo> getRemoteInfos();

  protected:
    // MulticastBroadcasterBase overrides
    void receiverThreadRun() override;
    void onShutdown() override;

  private:
    std::mutex m_mutex;
    std::map<uint32_t, RemoteInstanceInfo> m_remoteInfos;
    uint32_t m_sequenceNumber = 0;
    phu::debug::EditorLogger* m_editorLogger = nullptr;
    std::atomic<bool> receiveEnabled{true};

    /** Counts successfully upserted inbound Ctrl packets since last consumeInboundCount().
     *  Written by receiver thread, consumed by message thread via consumeInboundCount(). */
    std::atomic<uint32_t> m_inboundCtrlPackets{0};

    /** Set by receiver thread when a remote PeersBroadcastOnly command arrives. */
    std::atomic<bool> m_receivedPeersBroadcastOnly{false};
};

} // namespace network
} // namespace phu
