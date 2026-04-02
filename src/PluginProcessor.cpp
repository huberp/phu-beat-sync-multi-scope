#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "../lib/network/SampleBroadcaster.h"
#include <chrono>
#include <cmath>
#include <cstring>

#ifndef NDEBUG
#include "../lib/debug/EditorLogger.h"
#endif

// ============================================================================
// Local helper
// ============================================================================

static int64_t getProcessorTimeMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
}

// Plugin-family identifier broadcast in every CTRL packet.
// Receivers can use this to filter out instances from other plugin families.
static constexpr char     PLUGIN_TYPE[]    = "phu-beat-sync";
static constexpr uint32_t PLUGIN_VERSION   = 1;

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

    // Share the same instance ID so receivers can correlate CTRL info with sample data
    m_ctrlBroadcaster.setInstanceID(m_sampleBroadcaster.getInstanceID());

    // Initialize ctrl broadcaster networking
    m_ctrlBroadcaster.initialize();

    // Start processor-owned timer so CTRL heartbeats continue even when the
    // plugin editor is closed. Broadcast sample packets are now sent directly
    // from the audio thread (ping-pong buffer), so the timer no longer drains
    // any ring buffer.
    startTimerHz(30);
}

PhuBeatSyncMultiScopeAudioProcessor::~PhuBeatSyncMultiScopeAudioProcessor() {
    stopTimer(); // Stop before destroying members that the callback uses
    m_ctrlBroadcaster.shutdown();
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

// ============================================================================
// Audio Processing
// ============================================================================

void PhuBeatSyncMultiScopeAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    m_syncGlobals.updateSampleRate(sampleRate);

    // Track DAW stream parameters for ctrl packets
    m_currentSampleRate = sampleRate;
    m_currentMaxBufSize = samplesPerBlock;

    // Compute the broadcast slot capacity so each slot covers ~33 ms.
    // Lower bound of 128 avoids degenerate packet rates at non-standard sample rates.
    // Upper bound keeps the write within the struct's samples array.
    const int slotCapacity = juce::jlimit(
        128,
        phu::network::SampleBroadcaster::BROADCAST_CHUNK_SAMPLES,
        static_cast<int>(std::round(sampleRate * 0.033)));
    m_broadcastSlots[0].capacity = slotCapacity;
    m_broadcastSlots[1].capacity = slotCapacity;
    m_broadcastSlots[0].count    = 0;
    m_broadcastSlots[1].count    = 0;
    m_broadcastWriteSlot         = 0;

    // Announce this instance to peers (carries actual sampleRate, fixing ASSUMED_SAMPLE_RATE)
    m_ctrlBroadcaster.sendCtrl(
        phu::network::CtrlEventType::Announce,
        m_channelLabel.data(),
        static_cast<float>(m_displayRangeBeats.load()),
        static_cast<float>(m_syncGlobals.getBPM()),
        sampleRate,
        static_cast<uint32_t>(samplesPerBlock),
        m_colourRGBA,
        PLUGIN_TYPE, PLUGIN_VERSION);

    // Reset heartbeat timer so we don't send a redundant Announce too soon
    m_lastCtrlHeartbeatMs = getProcessorTimeMs();

#ifndef NDEBUG
    if (editorLogger)
        editorLogger->markCurrentThreadAsAudioThread();
#endif
}

void PhuBeatSyncMultiScopeAudioProcessor::releaseResources() {
    // Send Goodbye so peers know this instance is going offline
    m_ctrlBroadcaster.sendCtrl(
        phu::network::CtrlEventType::Goodbye,
        m_channelLabel.data(),
        static_cast<float>(m_displayRangeBeats.load()),
        static_cast<float>(m_syncGlobals.getBPM()),
        m_currentSampleRate,
        static_cast<uint32_t>(m_currentMaxBufSize),
        m_colourRGBA,
        PLUGIN_TYPE, PLUGIN_VERSION);
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

    // 2. Push samples to local ring and accumulate broadcast slot
    double bpm = m_syncGlobals.getBPM();
    double displayRange = m_displayRangeBeats.load();
    double sampleRate = m_syncGlobals.getSampleRate();

    if (bpm > 0.0 && displayRange > 0.0 && sampleRate > 0.0) {
        double blockPpq = m_syncGlobals.getPpqBlockStart();
        double ppqPerSample = bpm / (60.0 * sampleRate);

        // Hoist channel count and raw pointers out of the per-sample loop.
        // Using read pointers allows the compiler to auto-vectorize the mono mix,
        // and avoids bounds-checked getSample() overhead per sample.
        const int activeCh = juce::jmin(numChannels, 2);
        const float* ch0 = buffer.getReadPointer(0);
        const float* ch1 = (activeCh > 1) ? buffer.getReadPointer(1) : ch0;
        const float monoScale = (activeCh == 1) ? 1.0f : 0.5f;

        // --- Phase-3: batch-push (monoSample, absolutePpq) to the local SPSC ring.
        // The UI thread drains this ring to write into RawSampleBuffer[local].
        // AbstractFifo::write() silently reserves fewer slots when the ring is near
        // full; any overflow samples are dropped (ring is sized for 4× headroom).
        {
            const auto scope = m_localRingFifo.write(numSamples);
            for (int i = 0; i < scope.blockSize1; ++i) {
                m_localRingSamples[static_cast<size_t>(scope.startIndex1 + i)] =
                    (ch0[i] + ch1[i]) * monoScale;
                m_localRingPpqs[static_cast<size_t>(scope.startIndex1 + i)] =
                    blockPpq + static_cast<double>(i) * ppqPerSample;
            }
            for (int i = 0; i < scope.blockSize2; ++i) {
                const int si = scope.blockSize1 + i;
                m_localRingSamples[static_cast<size_t>(scope.startIndex2 + i)] =
                    (ch0[si] + ch1[si]) * monoScale;
                m_localRingPpqs[static_cast<size_t>(scope.startIndex2 + i)] =
                    blockPpq + static_cast<double>(si) * ppqPerSample;
            }
        }

        for (int i = 0; i < numSamples; ++i) {
            const float monoIn = (ch0[i] + ch1[i]) * monoScale;

            // Accumulate mono samples into the current ping-pong broadcast slot.
            // When the slot is full (≈33 ms at the current sample rate), send the
            // packet directly via loopback sendto() and flip to the other slot.
            if (m_broadcastEnabled.load(std::memory_order_relaxed)) {
                auto& slot = m_broadcastSlots[m_broadcastWriteSlot];
                if (slot.capacity > 0) {
                    // Capture PPQ of the very first sample in this slot.
                    if (slot.count == 0)
                        slot.ppqOfFirstSample = blockPpq + static_cast<double>(i) * ppqPerSample;
                    // capacity is clamped ≤ BROADCAST_CHUNK_SAMPLES == samples.size()
                    jassert(slot.count < static_cast<int>(slot.samples.size()));
                    slot.samples[static_cast<size_t>(slot.count++)] = monoIn;
                    if (slot.count >= slot.capacity) {
                        m_sampleBroadcaster.broadcastRawSamples(
                            slot.samples.data(), slot.count,
                            slot.ppqOfFirstSample,
                            bpm,
                            static_cast<float>(displayRange),
                            m_broadcastSeqNum++);
                        m_broadcastWriteSlot ^= 1;
                        m_broadcastSlots[m_broadcastWriteSlot].count = 0;
                    }
                }
            }
        }

        m_syncGlobals.setPpqEndOfBlock(blockPpq + numSamples * ppqPerSample);
    }

    m_syncGlobals.finishRun(numSamples);
}

// ============================================================================
// Processor Timer — CTRL heartbeat only (broadcast packets sent from audio thread)
// ============================================================================

void PhuBeatSyncMultiScopeAudioProcessor::timerCallback() {
    // --- Ctrl heartbeat (every 5 s): keeps late-joining peers up to date ---
    const int64_t nowMs = getProcessorTimeMs();
    if (nowMs - m_lastCtrlHeartbeatMs > CtrlBroadcaster::HEARTBEAT_INTERVAL_MS) {
        m_ctrlBroadcaster.sendCtrl(
            phu::network::CtrlEventType::Announce,
            m_channelLabel.data(),
            static_cast<float>(m_displayRangeBeats.load()),
            static_cast<float>(m_syncGlobals.getBPM()),
            m_currentSampleRate,
            static_cast<uint32_t>(m_currentMaxBufSize),
            m_colourRGBA,
            PLUGIN_TYPE, PLUGIN_VERSION);
        m_lastCtrlHeartbeatMs = nowMs;
    }
}

// ============================================================================
// Broadcast Control
// ============================================================================

void PhuBeatSyncMultiScopeAudioProcessor::setDisplayRangeBeats(double beats) {
    m_displayRangeBeats.store(beats);
    m_ctrlBroadcaster.sendCtrl(
        phu::network::CtrlEventType::RangeChange,
        m_channelLabel.data(),
        static_cast<float>(beats),
        static_cast<float>(m_syncGlobals.getBPM()),
        m_currentSampleRate,
        static_cast<uint32_t>(m_currentMaxBufSize),
        m_colourRGBA,
        PLUGIN_TYPE, PLUGIN_VERSION);
}

void PhuBeatSyncMultiScopeAudioProcessor::setBroadcastEnabled(bool enabled) {
    m_broadcastEnabled.store(enabled);
    m_sampleBroadcaster.setBroadcastEnabled(enabled);
}

void PhuBeatSyncMultiScopeAudioProcessor::setReceiveEnabled(bool enabled) {
    m_receiveEnabled.store(enabled);
    m_sampleBroadcaster.setReceiveEnabled(enabled);
}

void PhuBeatSyncMultiScopeAudioProcessor::setChannelLabel(const juce::String& label) {
    copyLabelToBuffer(label, m_channelLabel.data());

    m_ctrlBroadcaster.sendCtrl(
        phu::network::CtrlEventType::LabelChange,
        m_channelLabel.data(),
        static_cast<float>(m_displayRangeBeats.load()),
        static_cast<float>(m_syncGlobals.getBPM()),
        m_currentSampleRate,
        static_cast<uint32_t>(m_currentMaxBufSize),
        m_colourRGBA,
        PLUGIN_TYPE, PLUGIN_VERSION);
}

juce::String PhuBeatSyncMultiScopeAudioProcessor::getChannelLabel() const {
    return juce::String::fromUTF8(m_channelLabel.data());
}

void PhuBeatSyncMultiScopeAudioProcessor::setInstanceColour(juce::Colour colour) {
    m_colourRGBA[0] = colour.getRed();
    m_colourRGBA[1] = colour.getGreen();
    m_colourRGBA[2] = colour.getBlue();
    m_colourRGBA[3] = colour.getAlpha();

    m_ctrlBroadcaster.sendCtrl(
        phu::network::CtrlEventType::Announce,
        m_channelLabel.data(),
        static_cast<float>(m_displayRangeBeats.load()),
        static_cast<float>(m_syncGlobals.getBPM()),
        m_currentSampleRate,
        static_cast<uint32_t>(m_currentMaxBufSize),
        m_colourRGBA,
        PLUGIN_TYPE, PLUGIN_VERSION);
}

juce::Colour PhuBeatSyncMultiScopeAudioProcessor::getInstanceColour() const {
    return juce::Colour(m_colourRGBA[0], m_colourRGBA[1], m_colourRGBA[2], m_colourRGBA[3]);
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

    // Save channel identity
    state.setProperty("channelLabel", juce::String::fromUTF8(m_channelLabel.data()), nullptr);
    state.setProperty("colourR", static_cast<int>(m_colourRGBA[0]), nullptr);
    state.setProperty("colourG", static_cast<int>(m_colourRGBA[1]), nullptr);
    state.setProperty("colourB", static_cast<int>(m_colourRGBA[2]), nullptr);
    state.setProperty("colourA", static_cast<int>(m_colourRGBA[3]), nullptr);

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

            // Restore channel identity
            if (state.hasProperty("channelLabel"))
                copyLabelToBuffer(state.getProperty("channelLabel").toString(),
                                  m_channelLabel.data());
            if (state.hasProperty("colourR"))
                m_colourRGBA[0] = static_cast<uint8_t>(static_cast<int>(state.getProperty("colourR")));
            if (state.hasProperty("colourG"))
                m_colourRGBA[1] = static_cast<uint8_t>(static_cast<int>(state.getProperty("colourG")));
            if (state.hasProperty("colourB"))
                m_colourRGBA[2] = static_cast<uint8_t>(static_cast<int>(state.getProperty("colourB")));
            if (state.hasProperty("colourA"))
                m_colourRGBA[3] = static_cast<uint8_t>(static_cast<int>(state.getProperty("colourA")));
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
