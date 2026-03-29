#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "../lib/network/SampleBroadcaster.h"
#include <cmath>

#ifndef NDEBUG
#include "../lib/debug/EditorLogger.h"
#endif

// ============================================================================
// Construction / Destruction
// ============================================================================

PhuBeatSyncMultiScopeAudioProcessor::PhuBeatSyncMultiScopeAudioProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "Parameters", createParameterLayout()) {
#ifndef NDEBUG
    editorLogger = std::make_unique<phu::debug::EditorLogger>();
#endif

    // Initialize sample broadcaster networking
    m_sampleBroadcaster.initialize();

    // Start processor-owned broadcast timer (30 Hz) so broadcasting continues
    // even when the plugin editor is closed. The timer drains the raw sample
    // ring buffer in full chunks of BROADCAST_CHUNK_SAMPLES, sending as many
    // packets as are available per tick to keep the receiver current even if
    // a timer tick was delayed.
    startTimerHz(30);
}

PhuBeatSyncMultiScopeAudioProcessor::~PhuBeatSyncMultiScopeAudioProcessor() {
    stopTimer(); // Stop before destroying members that the callback uses
    m_sampleBroadcaster.shutdown();
}

// ============================================================================
// Parameter Layout
// ============================================================================

juce::AudioProcessorValueTreeState::ParameterLayout
PhuBeatSyncMultiScopeAudioProcessor::createParameterLayout() {
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // Display range in beats (1/4, 1/2, 1, 2, 4, 8)
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        "display_range", "Display Range",
        juce::StringArray{"1/4 Beat", "1/2 Beat", "1 Beat", "2 Beats", "4 Beats", "8 Beats"},
        4)); // Default: 4 beats

    // --- Display filter parameters ---
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        "display_hp_enabled", "HP Filter", false));

    {
        // HP upper bound is 18 kHz (below the LP ceiling of 20 kHz) to ensure at
        // least one valid LP position always exists above any HP setting.
        auto hpRange = juce::NormalisableRange<float>(20.0f, 18000.0f, 0.0f, 0.25f);
        hpRange.setSkewForCentre(600.0f);
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            "display_hp_freq", "HP Freq", hpRange, 80.0f));
    }

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        "display_lp_enabled", "LP Filter", false));

    {
        // LP range extends to 20 kHz; HP range only goes to 18 kHz, so there is
        // always room to place the LP above the maximum HP frequency.
        auto lpRange = juce::NormalisableRange<float>(20.0f, 20000.0f, 0.0f, 0.25f);
        lpRange.setSkewForCentre(1000.0f);
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            "display_lp_freq", "LP Freq", lpRange, 8000.0f));
    }

    return {params.begin(), params.end()};
}

// ============================================================================
// BPM / Display-Range Helpers
// ============================================================================

double PhuBeatSyncMultiScopeAudioProcessor::getMaxDisplayBeatsForBpm(double bpm) {
    if (bpm >= 80.0) return 8.0;
    if (bpm >= 60.0) return 4.0;
    if (bpm >= 40.0) return 2.0;
    return 1.0;
}

int PhuBeatSyncMultiScopeAudioProcessor::computeInputFifoCapacity(double sampleRate) {
    // Worst-case window: 8 beats at the 80-BPM threshold = 6 × sampleRate samples.
    // Supports sample rates up to 192 kHz and display windows down to 40 BPM
    // within the allowed range restrictions.
    return static_cast<int>(std::ceil(6.0 * sampleRate));
}

// ============================================================================
// Audio Processing
// ============================================================================

void PhuBeatSyncMultiScopeAudioProcessor::prepareToPlay(double sampleRate, int /*samplesPerBlock*/) {
    m_syncGlobals.updateSampleRate(sampleRate);
    m_inputSyncBuf.prepare(NUM_SYNC_BINS);

    // Resize the FIFO to hold the largest possible display window at this sample rate.
    // This prevents data loss at slow tempos and high sample rates (up to 192 kHz).
    m_inputFifo.resize(computeInputFifoCapacity(sampleRate));

    m_inputFifo.reset();

#ifndef NDEBUG
    if (editorLogger)
        editorLogger->markCurrentThreadAsAudioThread();
#endif
}

void PhuBeatSyncMultiScopeAudioProcessor::releaseResources() {
    // Nothing to release
}

void PhuBeatSyncMultiScopeAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                                       juce::MidiBuffer& midiMessages) {
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();

    // 1. Track DAW state (BPM, transport, PPQ)
    auto positionInfo = getPlayHead() ? getPlayHead()->getPosition()
                                      : juce::Optional<juce::AudioPlayHead::PositionInfo>();
    m_syncGlobals.updateDAWGlobals(buffer, midiMessages, positionInfo);

    // 2. Push audio to input FIFO (for UI thread)
    const float* channelPtrs[2] = {nullptr, nullptr};
    for (int ch = 0; ch < juce::jmin(numChannels, 2); ++ch)
        channelPtrs[ch] = buffer.getReadPointer(ch);
    // If mono, duplicate to second channel
    if (numChannels == 1)
        channelPtrs[1] = channelPtrs[0];
    m_inputFifo.push(channelPtrs, numSamples);

    // 3. Write beat-sync buffer (PPQ → normalized position → bin)
    double bpm = m_syncGlobals.getBPM();
    double displayRange = m_displayRangeBeats.load();
    double sampleRate = m_syncGlobals.getSampleRate();

    if (bpm > 0.0 && displayRange > 0.0 && sampleRate > 0.0) {
        double blockPpq = m_syncGlobals.getPpqBlockStart();
        double ppqPerSample = bpm / (60.0 * sampleRate);

        // Detect cycle boundary by block end PPQ. For short ranges (<= 1 beat),
        // hard-clear on cycle transition to prevent previous-beat events from
        // lingering into the next beat. For longer ranges, keep history and let
        // per-sample writes naturally roll through the window.
        const double blockEndPpq    = blockPpq + numSamples * ppqPerSample;
        const double endWindowStart = std::floor(blockEndPpq / displayRange) * displayRange;
        if (std::abs(endWindowStart - m_lastWriteWindowStart) > 1e-9) {
            if (displayRange <= 1.0 + 1e-9)
                m_inputSyncBuf.clear();
            m_lastWriteWindowStart = endWindowStart;
        }

        // Hoist channel count and raw pointers out of the per-sample loop.
        // Using read pointers allows the compiler to auto-vectorize the mono mix,
        // and avoids bounds-checked getSample() overhead per sample.
        const int activeCh = juce::jmin(numChannels, 2);
        const float* ch0 = buffer.getReadPointer(0);
        const float* ch1 = (activeCh > 1) ? buffer.getReadPointer(1) : ch0;
        const float monoScale = (activeCh == 1) ? 1.0f : 0.5f;

        // Replace std::fmod per sample with a linear normalized-position increment.
        // Start at the block's normalized position and advance by normStep each sample,
        // wrapping with a single branch — eliminates N libm fmod calls per block.
        const double normStep = ppqPerSample / displayRange;
        double normPos = std::fmod(blockPpq, displayRange) / displayRange;
        if (normPos < 0.0) normPos += 1.0;

        for (int i = 0; i < numSamples; ++i) {
            const float monoIn = (ch0[i] + ch1[i]) * monoScale;
            m_inputSyncBuf.write(normPos, monoIn);

            // Push every mono sample with its absolute PPQ position into the
            // raw broadcast ring buffer. The message-thread timer drains this
            // in full chunks and sends RawSamplesPackets to remote instances.
            if (m_broadcastEnabled.load(std::memory_order_relaxed)) {
                const double samplePpq = blockPpq + static_cast<double>(i) * ppqPerSample;
                m_rawBroadcastRing.push(monoIn, samplePpq);
            }

            normPos += normStep;
            if (normPos >= 1.0) normPos -= 1.0;
        }

        m_syncGlobals.setPpqEndOfBlock(blockPpq + numSamples * ppqPerSample);
    }

    m_syncGlobals.finishRun(numSamples);
}

// ============================================================================
// Processor Timer — raw sample broadcast
// ============================================================================

void PhuBeatSyncMultiScopeAudioProcessor::timerCallback() {
    if (!m_broadcastEnabled.load(std::memory_order_relaxed))
        return;

    const int chunkSize = phu::network::SampleBroadcaster::BROADCAST_CHUNK_SAMPLES;

    // Ensure work buffers are large enough (allocated once, never reallocated)
    if (static_cast<int>(m_broadcastSampleBuf.size()) < chunkSize) {
        m_broadcastSampleBuf.resize(static_cast<size_t>(chunkSize));
        m_broadcastPpqBuf.resize(static_cast<size_t>(chunkSize));
    }

    const double bpm         = m_syncGlobals.getBPM();
    const float  displayRange = static_cast<float>(m_displayRangeBeats.load());

    // Drain all available full chunks and send one packet per chunk.
    // If the timer was delayed, multiple packets are sent in this tick.
    // Partial chunks (< chunkSize) are left in the ring buffer for the next tick.
    while (m_rawBroadcastRing.getNumAvailable() >= chunkSize) {
        const int drained = m_rawBroadcastRing.drain(
            m_broadcastSampleBuf.data(), m_broadcastPpqBuf.data(), chunkSize);

        if (drained < chunkSize)
            break; // Should not happen, but guard against partial drain

        if (bpm > 0.0) {
            m_sampleBroadcaster.broadcastRawSamples(
                m_broadcastSampleBuf.data(), chunkSize,
                m_broadcastPpqBuf[0], bpm, displayRange,
                m_broadcastSeqNum++);
        }
    }
}

// ============================================================================
// Broadcast Control
// ============================================================================

void PhuBeatSyncMultiScopeAudioProcessor::setBroadcastEnabled(bool enabled) {
    m_broadcastEnabled.store(enabled);
    m_sampleBroadcaster.setBroadcastEnabled(enabled);
}

void PhuBeatSyncMultiScopeAudioProcessor::setReceiveEnabled(bool enabled) {
    m_receiveEnabled.store(enabled);
    m_sampleBroadcaster.setReceiveEnabled(enabled);
}

// ============================================================================
// Plugin Metadata
// ============================================================================

const juce::String PhuBeatSyncMultiScopeAudioProcessor::getName() const {
    return JucePlugin_Name;
}

bool PhuBeatSyncMultiScopeAudioProcessor::acceptsMidi() const { return false; }
bool PhuBeatSyncMultiScopeAudioProcessor::producesMidi() const { return false; }
bool PhuBeatSyncMultiScopeAudioProcessor::isMidiEffect() const { return false; }
double PhuBeatSyncMultiScopeAudioProcessor::getTailLengthSeconds() const { return 0.0; }

bool PhuBeatSyncMultiScopeAudioProcessor::isBusesLayoutSupported(
    const BusesLayout& layouts) const {
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;
    if (layouts.getMainInputChannelSet() != juce::AudioChannelSet::stereo())
        return false;
    return true;
}

int PhuBeatSyncMultiScopeAudioProcessor::getNumPrograms() { return 1; }
int PhuBeatSyncMultiScopeAudioProcessor::getCurrentProgram() { return 0; }
void PhuBeatSyncMultiScopeAudioProcessor::setCurrentProgram(int) {}
const juce::String PhuBeatSyncMultiScopeAudioProcessor::getProgramName(int) { return {}; }
void PhuBeatSyncMultiScopeAudioProcessor::changeProgramName(int, const juce::String&) {}

// ============================================================================
// State Save / Restore
// ============================================================================

void PhuBeatSyncMultiScopeAudioProcessor::getStateInformation(juce::MemoryBlock& destData) {
    auto state = apvts.copyState();

    // Save broadcast/receive enabled state
    state.setProperty("broadcastEnabled", m_broadcastEnabled.load(), nullptr);
    state.setProperty("receiveEnabled", m_receiveEnabled.load(), nullptr);

    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void PhuBeatSyncMultiScopeAudioProcessor::setStateInformation(const void* data, int sizeInBytes) {
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));
    if (xmlState != nullptr) {
        if (xmlState->hasTagName(apvts.state.getType())) {
            auto state = juce::ValueTree::fromXml(*xmlState);
            apvts.replaceState(state);

            // Restore broadcast/receive enabled state
            if (state.hasProperty("broadcastEnabled"))
                setBroadcastEnabled(static_cast<bool>(state.getProperty("broadcastEnabled")));
            if (state.hasProperty("receiveEnabled"))
                setReceiveEnabled(static_cast<bool>(state.getProperty("receiveEnabled")));
        }
    }
}

// ============================================================================
// Editor
// ============================================================================

juce::AudioProcessorEditor* PhuBeatSyncMultiScopeAudioProcessor::createEditor() {
    return new PhuBeatSyncMultiScopeAudioProcessorEditor(*this);
}

bool PhuBeatSyncMultiScopeAudioProcessor::hasEditor() const { return true; }

// ============================================================================
// Plugin Instantiation
// ============================================================================

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new PhuBeatSyncMultiScopeAudioProcessor();
}
