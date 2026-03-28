#pragma once

#include "../lib/audio/AudioSampleFifo.h"
#include "../lib/audio/BeatSyncBuffer.h"
#include "../lib/events/SyncGlobals.h"
#include "../lib/network/SampleBroadcaster.h"
#include <atomic>
#include <juce_audio_processors/juce_audio_processors.h>

// Forward declarations
#ifndef NDEBUG
namespace phu { namespace debug { class EditorLogger; } }
#endif

using phu::audio::AudioSampleFifo;
using phu::audio::BeatSyncBuffer;
using phu::events::GlobalsEventListener;
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
    void setDisplayRangeBeats(double beats) { m_displayRangeBeats.store(beats); }
    double getDisplayRangeBeats() const { return m_displayRangeBeats.load(); }

    // Sample broadcasting (owned by processor for headless operation)
    SampleBroadcaster& getSampleBroadcaster() { return m_sampleBroadcaster; }
    bool isBroadcastEnabled() const { return m_broadcastEnabled.load(); }
    void setBroadcastEnabled(bool enabled);

    /**
     * Check whether a beat-boundary has been crossed since the last call and
     * clear the flag atomically. Called from the editor timer to decide when
     * to broadcast the filtered display buffer. Returns true once per beat
     * interval; subsequent calls return false until the audio thread sets the
     * flag again.
     */
    bool checkAndClearBroadcastReady() {
        return m_broadcastReady.exchange(false, std::memory_order_acq_rel);
    }

    /** juce::Timer callback — sends broadcast on the message thread (~10 Hz). */
    void timerCallback() override;

    // Sample receiving (independent from broadcasting)
    bool isReceiveEnabled() const { return m_receiveEnabled.load(); }
    void setReceiveEnabled(bool enabled);

    // Number of bins for BeatSyncBuffer
    static constexpr int NUM_SYNC_BINS = 4096;

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

    // Beat-driven broadcast: audio thread sets flag at quarter-beat boundaries
    std::atomic<bool> m_broadcastReady{false};
    double m_lastBroadcastPpq = 0.0;   // audio-thread only
    double m_lastWriteWindowStart = -1.0; // audio-thread only: last cycle start written
    static constexpr double BROADCAST_BEAT_INTERVAL = 0.25; // broadcast every 1/4 beat

    // Work buffer for headless broadcast (message-thread only)
    std::vector<float> m_broadcastWorkBuf;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PhuBeatSyncMultiScopeAudioProcessor)
};
