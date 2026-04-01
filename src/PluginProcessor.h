#pragma once

#include "../lib/audio/AudioSampleFifo.h"
#include "../lib/audio/BeatSyncBuffer.h"
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

using phu::audio::AudioSampleFifo;
using phu::audio::BeatSyncBuffer;
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

    // Beat-sync display buffers (audio thread writes, UI thread reads)
    BeatSyncBuffer& getInputSyncBuffer() { return m_inputSyncBuf; }

    // Lock-free FIFO for sample transfer to UI thread
    AudioSampleFifo<2>& getInputFifo() { return m_inputFifo; }

    // DAW sync globals (UI-safe reads via atomic PPQ)
    phu::events::SyncGlobals& getSyncGlobals() { return m_syncGlobals; }

    // Display range in beats (settable from UI thread)
    void setDisplayRangeBeats(double beats);
    double getDisplayRangeBeats() const { return m_displayRangeBeats.load(); }

    // Sample broadcasting (owned by processor for headless operation)
    SampleBroadcaster& getSampleBroadcaster() { return m_sampleBroadcaster; }
    bool isBroadcastEnabled() const { return m_broadcastEnabled.load(); }
    void setBroadcastEnabled(bool enabled);

    // Control broadcaster (instance identity, sample rate, label)
    CtrlBroadcaster& getCtrlBroadcaster() { return m_ctrlBroadcaster; }

    /** Set the user-visible channel label (max 31 UTF-8 bytes). Sends a LabelChange ctrl packet. */
    void setChannelLabel(const juce::String& label);
    juce::String getChannelLabel() const;

    /** Set the user colour for this instance. Sends an Announce ctrl packet. */
    void setInstanceColour(juce::Colour colour);
    juce::Colour getInstanceColour() const;

    /** juce::Timer callback — drains raw sample ring buffer and sends packets (~30 Hz). */
    void timerCallback() override;

    // Sample receiving (independent from broadcasting)
    bool isReceiveEnabled() const { return m_receiveEnabled.load(); }
    void setReceiveEnabled(bool enabled);

    // Number of bins for BeatSyncBuffer
    static constexpr int NUM_SYNC_BINS = 4096;

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
     * impractical and causing the AudioSampleFifo to overflow before a full window
     * can be assembled.  The thresholds below were chosen to keep the longest
     * displayable window within ≈ 6 s across the supported sample-rate range:
     *
     *   BPM ≥ 80  → up to 8 beats  (8 beats @ 80 BPM = 6 s)
     *   BPM ≥ 60  → up to 4 beats  (4 beats @ 60 BPM = 4 s)
     *   BPM ≥ 40  → up to 2 beats  (2 beats @ 40 BPM = 3 s)
     *   BPM <  40 → up to 1 beat
     */
    static double getMaxDisplayBeatsForBpm(double bpm);

    /**
     * Returns the required AudioSampleFifo capacity (samples per channel) for
     * a given sample rate.  Sized for 8 beats at the 80 BPM threshold, which is
     * the worst-case window within the supported tempo range:
     *   capacity = ceil(8 × (60/80) × sampleRate) = ceil(6 × sampleRate)
     */
    static int computeInputFifoCapacity(double sampleRate);

  private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // DAW synchronization globals
    phu::events::SyncGlobals m_syncGlobals;

#ifndef NDEBUG
    std::unique_ptr<phu::debug::EditorLogger> editorLogger;
#endif

    // APVTS for state save/restore
    juce::AudioProcessorValueTreeState apvts;

    // Beat-synced display buffer (4096-bin, position-indexed by PPQ)
    BeatSyncBuffer m_inputSyncBuf;

    // Lock-free FIFO for transferring audio samples to UI thread
    AudioSampleFifo<2> m_inputFifo;

    // Display range in beats (UI-settable, atomic for cross-thread access)
    std::atomic<double> m_displayRangeBeats{4.0}; // Default: 4 beats (1 bar at 4/4)

    // Sample broadcasting
    SampleBroadcaster m_sampleBroadcaster;
    std::atomic<bool> m_broadcastEnabled{false};
    std::atomic<bool> m_receiveEnabled{true};

    // Control broadcaster (instance identity)
    CtrlBroadcaster m_ctrlBroadcaster;

    // Channel identity (persisted in state)
    std::array<char, 32> m_channelLabel{};
    uint8_t m_colourRGBA[4] = {0x00, 0xFF, 0x88, 0xFF}; // default: bright green

    // Current DAW stream parameters — updated in prepareToPlay, read by ctrl send helpers
    double m_currentSampleRate   = 44100.0;
    int    m_currentMaxBufSize   = 512;

    // Heartbeat tracking (message thread only)
    int64_t m_lastCtrlHeartbeatMs = 0;

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

    double m_lastWriteWindowStart = -1.0; // audio-thread only: last cycle start written

    /** Copy a juce::String into the fixed 32-byte label buffer (max 31 chars + null). */
    static void copyLabelToBuffer(const juce::String& label, char* buf) {
        std::memset(buf, 0, 32);
        std::strncpy(buf, label.substring(0, 31).toRawUTF8(), 31);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PhuBeatSyncMultiScopeAudioProcessor)
};
