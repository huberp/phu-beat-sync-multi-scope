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
                                            private juce::Timer {
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

    // Sample receiving (independent from broadcasting)
    bool isReceiveEnabled() const { return m_receiveEnabled.load(); }
    void setReceiveEnabled(bool enabled);

    // Number of bins for BeatSyncBuffer
    static constexpr int NUM_SYNC_BINS = 4096;

  private:
    void timerCallback() override;
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PhuBeatSyncMultiScopeAudioProcessor)
};
