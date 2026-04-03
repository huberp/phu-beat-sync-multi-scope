#pragma once

#include "../lib/StringUtil.h"
#include "../lib/events/SyncGlobals.h"
#include "../lib/network/CtrlBroadcaster.h"
#include "../lib/network/SampleBroadcaster.h"
#include <array>
#include <atomic>
#include <juce_audio_processors/juce_audio_processors.h>

// Forward declarations
#ifndef NDEBUG
namespace phu { namespace debug { class EditorLogger; } }
#endif

using phu::events::GlobalsEventListener;
using phu::network::CtrlBroadcaster;
using phu::network::SampleBroadcaster;

class PhuBeatSyncMultiScopeAudioProcessor : public juce::AudioProcessor,
                                            public GlobalsEventListener,
                                            public juce::Timer {
  public:
    PhuBeatSyncMultiScopeAudioProcessor();
    ~PhuBeatSyncMultiScopeAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

#ifndef NDEBUG
    phu::debug::EditorLogger* getEditorLogger() const {
        return editorLogger.get();
    }
#endif

    // Parameter tree state
    juce::AudioProcessorValueTreeState& getAPVTS() { return apvts; }

    // DAW sync globals (UI-safe reads via atomic PPQ)
    phu::events::SyncGlobals& getSyncGlobals() { return m_syncGlobals; }

    // Display range in beats (settable from UI thread)
    void setDisplayRangeBeats(double beats);
    double getDisplayRangeBeats() const { return m_displayRangeBeats.load(); }

    // Sample broadcasting (owned by processor for headless operation)
    SampleBroadcaster& getSampleBroadcaster() { return m_sampleBroadcaster; }
    bool isBroadcastEnabled() const { return m_broadcastEnabled.load(); }
    void setBroadcastEnabled(bool enabled);

    /** Active mode = false; Broadcast-only mode = true. */
    bool isBroadcastOnlyMode() const { return m_broadcastOnlyMode.load(); }
    void setBroadcastOnlyMode(bool enabled);

    // Control broadcaster (instance identity, sample rate, label)
    CtrlBroadcaster& getCtrlBroadcaster() { return m_ctrlBroadcaster; }

    /** Set the user-visible channel label (max 31 UTF-8 bytes). Sends a LabelChange ctrl packet. */
    void setChannelLabel(const juce::String& label);
    juce::String getChannelLabel() const;

    /** Set the user colour for this instance. Sends an Announce ctrl packet. */
    void setInstanceColour(juce::Colour colour);
    juce::Colour getInstanceColour() const;

    /** Get the user-assigned channel index [1, 8] from the instance_channel APVTS param. */
    int getInstanceIndex() const;

    /** juce::Timer callback — drains raw sample ring buffer and sends packets (~30 Hz). */
    void timerCallback() override;

    // Sample receiving (independent from broadcasting)
    bool isReceiveEnabled() const { return m_receiveEnabled.load(); }
    void setReceiveEnabled(bool enabled);

    // ---- Processor timer -------------------------------------------------------
    /** Timer frequency (Hz) for the processor-owned heartbeat / index-sync timer. */
    static constexpr int PROCESSOR_TIMER_HZ = 30;

    // ---- Broadcast slot sizing -------------------------------------------------
    /** Duration (seconds) each ping-pong broadcast slot covers (~33 ms). */
    static constexpr double BROADCAST_SLOT_DURATION_S = 0.033;

    /** Minimum broadcast slot capacity (samples): avoids degenerate rates at non-standard
     *  sample rates and ensures at least one full packet per timer tick. */
    static constexpr int MIN_BROADCAST_SLOT_SAMPLES = 128;

    // ---- Channel label buffer --------------------------------------------------
    /** Byte capacity of the channel label buffer, including the null terminator. */
    static constexpr int CHANNEL_LABEL_CAPACITY = 32;

    // ---- Phase-3 local sample ring: (monoSample, absolutePpq) pairs --------
    // Audio thread writes one pair per sample; the UI timer drains this ring and
    // forwards each pair to ScopeDisplay::writeLocalSample().  Capacity = 2 ×
    // broadcast chunk size (~66 ms @ 192 kHz), providing ≥ 4× headroom for the
    // 60 Hz UI drain rate.
    static constexpr int LOCAL_RING_CAPACITY =
        phu::network::SampleBroadcaster::BROADCAST_CHUNK_SAMPLES * 2; // 12700

    juce::AbstractFifo& getLocalRingFifo() { return m_localRingFifo; }
    const std::array<float,  LOCAL_RING_CAPACITY>& getLocalRingSamples() const { return m_localRingSamples; }
    const std::array<double, LOCAL_RING_CAPACITY>& getLocalRingPpqs()    const { return m_localRingPpqs; }

    /**
     * Returns the maximum allowed display range (in beats) for a given BPM.
     *
     * At very slow tempos a wide beat window spans many seconds, making the display
     * impractical.  The thresholds below were chosen to keep the longest
     * displayable window within ≈ 6 s across the supported sample-rate range:
     *
     *   BPM ≥ 80  → up to 8 beats  (8 beats @ 80 BPM = 6 s)
     *   BPM ≥ 60  → up to 4 beats  (4 beats @ 60 BPM = 4 s)
     *   BPM ≥ 40  → up to 2 beats  (2 beats @ 40 BPM = 3 s)
     *   BPM <  40 → up to 1 beat
     */
    static double getMaxDisplayBeatsForBpm(double bpm);

  private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // DAW synchronization globals
    phu::events::SyncGlobals m_syncGlobals;

#ifndef NDEBUG
    std::unique_ptr<phu::debug::EditorLogger> editorLogger;
#endif

    // APVTS for state save/restore
    juce::AudioProcessorValueTreeState apvts;

    // Display range in beats (UI-settable, atomic for cross-thread access)
    std::atomic<double> m_displayRangeBeats{4.0}; // Default: 4 beats (1 bar at 4/4)

    // Sample broadcasting
    SampleBroadcaster m_sampleBroadcaster;
    std::atomic<bool> m_broadcastEnabled{false};
    std::atomic<bool> m_receiveEnabled{true};
    std::atomic<bool> m_broadcastOnlyMode{false};
    std::atomic<bool> m_receiveEnabledWhenActive{true};

    // Control broadcaster (instance identity)
    CtrlBroadcaster m_ctrlBroadcaster;

    // Channel identity (persisted in state)
    std::array<char, CHANNEL_LABEL_CAPACITY> m_channelLabel{};
    uint8_t m_colourRGBA[4] = {0x00, 0xFF, 0x88, 0xFF}; // default: bright green

    // Current DAW stream parameters — updated in prepareToPlay, read by ctrl send helpers
    double m_currentSampleRate   = 44100.0;
    int    m_currentMaxBufSize   = 512;

    // Heartbeat scheduling state (message thread only)
    int64_t m_nextCtrlHeartbeatMs     = 0;    ///< Absolute deadline for next Announce
    int64_t m_ctrlHeartbeatIntervalMs = CtrlBroadcaster::HEARTBEAT_INTERVAL_MIN_MS; ///< Adaptive interval [MIN, MAX]
    float   m_inboundRateEwma         = 0.0f; ///< EWMA of inbound Ctrl pkts per timer tick
    int     m_intervalBackoffTicks    = 0;    ///< Consecutive ticks above high-load threshold
    int     m_intervalRecoverTicks    = 0;    ///< Consecutive ticks below low-load threshold

    // Last instance index broadcast to the network layer (message thread only)
    int m_lastBroadcastInstanceIndex = -1;

    /** Push current instance_channel APVTS value to both broadcasters. */
    void syncInstanceIndexToBroadcasters();

    // Ping-pong broadcast buffer: the audio thread accumulates mono samples into
    // one slot; when full (≈33 ms) it calls broadcastRawSamples() directly via
    // loopback sendto() and flips to the other slot. No timer drain needed.
    struct BroadcastSlot {
        std::array<float, SampleBroadcaster::BROADCAST_CHUNK_SAMPLES> samples{};
        double ppqOfFirstSample = 0.0;
        int    count            = 0;
        int    capacity         = 0; // set in prepareToPlay: round(sampleRate × 0.033)
    };
    BroadcastSlot m_broadcastSlots[2];
    int           m_broadcastWriteSlot = 0; // audio thread only

    // Phase-3 local SPSC ring: (monoSample, absolutePpq) pairs
    // Audio thread writes; UI thread drains via getLocalRingFifo().
    juce::AbstractFifo                           m_localRingFifo   { LOCAL_RING_CAPACITY };
    std::array<float,  LOCAL_RING_CAPACITY>      m_localRingSamples{};
    std::array<double, LOCAL_RING_CAPACITY>      m_localRingPpqs   {};

    // Sequence number for outgoing RawSamplesPackets (audio thread only)
    uint32_t m_broadcastSeqNum = 0;

    /** Copy a juce::String into the fixed label buffer (max CHANNEL_LABEL_CAPACITY-1 chars + null). */
    static void copyLabelToBuffer(const juce::String& label, char* buf) {
        std::memset(buf, 0, CHANNEL_LABEL_CAPACITY);
        phu::StringUtil::safe_strncpy(buf, label.substring(0, CHANNEL_LABEL_CAPACITY - 1).toRawUTF8(),
                                      CHANNEL_LABEL_CAPACITY);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PhuBeatSyncMultiScopeAudioProcessor)
};
