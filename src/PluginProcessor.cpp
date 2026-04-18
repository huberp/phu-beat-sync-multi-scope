#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "network/SampleBroadcaster.h"
#include <chrono>
#include <cmath>
#include <cstring>

#if PHU_DEBUG_UI
#include "debug/EditorLogger.h"
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
#if PHU_DEBUG_UI
    editorLogger = std::make_unique<phu::debug::EditorLogger>();
#endif

    // Initialize sample broadcaster networking
    m_sampleBroadcaster.initialize();

    // Share the same instance ID so receivers can correlate CTRL info with sample data
    m_ctrlBroadcaster.setInstanceID(m_sampleBroadcaster.getInstanceID());

    // Initialize ctrl broadcaster networking
    m_ctrlBroadcaster.initialize();

#if PHU_DEBUG_UI
    // Connect the editor logger for network debug output
    if (editorLogger)
        m_ctrlBroadcaster.setEditorLogger(editorLogger.get());
#endif

    // Deterministic per-instance phase offset in [0, HEARTBEAT_INTERVAL_MIN_MS)
    // derived from a bijective integer hash of the instanceID.  Each instance
    // gets a unique offset so first heartbeat fires are staggered across the
    // 5 s window without any randomness or coordination.
    {
        uint32_t h = m_ctrlBroadcaster.getInstanceID();
        h ^= h >> 16;
        h *= 0x45d9f3bU;
        h ^= h >> 16;
        const auto phaseMs = static_cast<int64_t>(
            h % static_cast<uint32_t>(CtrlBroadcaster::HEARTBEAT_INTERVAL_MIN_MS));
        m_nextCtrlHeartbeatMs = getProcessorTimeMs() + phaseMs;
    }

    // Start processor-owned timer so CTRL heartbeats continue even when the
    // plugin editor is closed. Broadcast sample packets are now sent directly
    // from the audio thread (ping-pong buffer), so the timer no longer drains
    // any ring buffer.
    startTimerHz(PROCESSOR_TIMER_HZ);
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

    // User-assigned channel index [1, 8]: determines which display slot this instance
    // maps to in ScopeDisplay and which packet slot remote receivers render into.
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        "instance_channel", "Channel",
        juce::StringArray{"Ch 1", "Ch 2", "Ch 3", "Ch 4",
                          "Ch 5", "Ch 6", "Ch 7", "Ch 8"},
        0)); // Default: Ch 1

    return {params.begin(), params.end()};
}

int PhuBeatSyncMultiScopeAudioProcessor::getInstanceIndex() const {
    // instance_channel is a 0-based choice ("Ch 1"=0 … "Ch 8"=7) → return 1-based index
    if (auto* p = apvts.getRawParameterValue("instance_channel"))
        return static_cast<int>(p->load(std::memory_order_relaxed)) + 1;
    return 1;
}

void PhuBeatSyncMultiScopeAudioProcessor::syncInstanceIndexToBroadcasters() {
    const int idx = getInstanceIndex();
    m_sampleBroadcaster.setInstanceIndex(static_cast<uint8_t>(idx));
    m_ctrlBroadcaster.setInstanceIndex(static_cast<uint8_t>(idx));
    m_lastBroadcastInstanceIndex = idx;
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

    // Compute the broadcast slot capacity so each slot covers ~BROADCAST_SLOT_DURATION_S s.
    // Lower bound of MIN_BROADCAST_SLOT_SAMPLES avoids degenerate packet rates at non-standard sample rates.
    // Upper bound keeps the write within the struct's samples array.
    const int slotCapacity = juce::jlimit(
        MIN_BROADCAST_SLOT_SAMPLES,
        phu::network::SampleBroadcaster::BROADCAST_CHUNK_SAMPLES,
        static_cast<int>(std::round(sampleRate * BROADCAST_SLOT_DURATION_S)));
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

    // Sync user-assigned channel index to both broadcasters
    syncInstanceIndexToBroadcasters();

    // Reset deadline: startup Announce was just sent — don't fire again until
    // a full adaptive interval has elapsed.
    m_nextCtrlHeartbeatMs = getProcessorTimeMs() + m_ctrlHeartbeatIntervalMs;
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
        // The UI thread drains this ring to write into PpqAddressedRingBuffer[local].
        // AbstractFifo::write() silently reserves fewer slots when the ring is near
        // full; any overflow samples are dropped (ring is sized for 4× headroom).
        if (!m_broadcastOnlyMode.load(std::memory_order_relaxed)) {
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
    // --- Apply remote peer commands even when editor is closed ---
    if (m_ctrlBroadcaster.consumePeersBroadcastOnlyCommand())
        setBroadcastOnlyMode(true);

    // --- Sync channel index if user changed it since last heartbeat ---
    const int currentIdx = getInstanceIndex();
    if (currentIdx != m_lastBroadcastInstanceIndex)
        syncInstanceIndexToBroadcasters();

    // --- Adaptive Ctrl heartbeat ---
    // 1. Update EWMA of inbound Ctrl packet rate (packets received since last tick)
    {
        constexpr float kAlpha        = 0.07f;  // time constant ≈ 470 ms @ 30 Hz
        constexpr float kHighThresh   = 2.5f;   // pkts/tick — back off
        constexpr float kLowThresh    = 0.5f;   // pkts/tick — recover
        constexpr int   kHystTicks    = 10;     // ticks before a step fires (≈ 330 ms)
        constexpr int64_t kStepUp     = 500;    // ms added per backoff step
        constexpr int64_t kStepDown   = 250;    // ms removed per recover step

        const float newCount = static_cast<float>(m_ctrlBroadcaster.consumeInboundCount());
        m_inboundRateEwma = kAlpha * newCount + (1.0f - kAlpha) * m_inboundRateEwma;

        if (m_inboundRateEwma > kHighThresh) {
            ++m_intervalBackoffTicks;
            m_intervalRecoverTicks = 0;
        } else if (m_inboundRateEwma < kLowThresh) {
            ++m_intervalRecoverTicks;
            m_intervalBackoffTicks = 0;
        } else {
            m_intervalBackoffTicks = 0;
            m_intervalRecoverTicks = 0;
        }

        if (m_intervalBackoffTicks >= kHystTicks) {
            m_intervalBackoffTicks = 0;
            m_ctrlHeartbeatIntervalMs = std::min(
                m_ctrlHeartbeatIntervalMs + kStepUp,
                CtrlBroadcaster::HEARTBEAT_INTERVAL_MAX_MS);
        }
        if (m_intervalRecoverTicks >= kHystTicks) {
            m_intervalRecoverTicks = 0;
            m_ctrlHeartbeatIntervalMs = std::max(
                m_ctrlHeartbeatIntervalMs - kStepDown,
                CtrlBroadcaster::HEARTBEAT_INTERVAL_MIN_MS);
        }
    }

    // 2. Fire on absolute deadline (advance by interval, not by now — preserves phase)
    const int64_t nowMs = getProcessorTimeMs();
    if (nowMs >= m_nextCtrlHeartbeatMs) {
        m_ctrlBroadcaster.sendCtrl(
            phu::network::CtrlEventType::Announce,
            m_channelLabel.data(),
            static_cast<float>(m_displayRangeBeats.load()),
            static_cast<float>(m_syncGlobals.getBPM()),
            m_currentSampleRate,
            static_cast<uint32_t>(m_currentMaxBufSize),
            m_colourRGBA,
            PLUGIN_TYPE, PLUGIN_VERSION);
        m_nextCtrlHeartbeatMs += m_ctrlHeartbeatIntervalMs;
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
    const bool broadcastOnly = m_broadcastOnlyMode.load(std::memory_order_relaxed);
    m_sampleBroadcaster.setReceiveEnabled(broadcastOnly ? false : enabled);
    m_ctrlBroadcaster.setReceiveEnabled(broadcastOnly ? false : enabled);

    if (!broadcastOnly)
        m_receiveEnabledWhenActive.store(enabled);
}

void PhuBeatSyncMultiScopeAudioProcessor::setBroadcastOnlyMode(bool enabled) {
    const bool wasEnabled = m_broadcastOnlyMode.load(std::memory_order_relaxed);
    if (wasEnabled == enabled)
        return;

    if (enabled) {
        m_receiveEnabledWhenActive.store(m_receiveEnabled.load(std::memory_order_relaxed));
        m_broadcastOnlyMode.store(true, std::memory_order_relaxed);
        setBroadcastEnabled(true);
        m_sampleBroadcaster.setReceiveEnabled(false);
        m_ctrlBroadcaster.setReceiveEnabled(false);
    } else {
        m_broadcastOnlyMode.store(false, std::memory_order_relaxed);
        setReceiveEnabled(m_receiveEnabledWhenActive.load(std::memory_order_relaxed));
    }
}

void PhuBeatSyncMultiScopeAudioProcessor::requestPeersBroadcastOnlyMode() {
    // Localhost UDP is reliable in practice, but send a short burst to make
    // the peer command resilient to occasional packet loss.
    for (int i = 0; i < 3; ++i) {
        m_ctrlBroadcaster.sendCtrl(
            phu::network::CtrlEventType::PeersBroadcastOnly,
            m_channelLabel.data(),
            static_cast<float>(m_displayRangeBeats.load()),
            static_cast<float>(m_syncGlobals.getBPM()),
            m_currentSampleRate,
            static_cast<uint32_t>(m_currentMaxBufSize),
            m_colourRGBA,
            PLUGIN_TYPE, PLUGIN_VERSION);
    }
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

// Identifier for the child ValueTree node that stores plugin-specific state
// (separate from APVTS parameters so the two namespaces can't collide).
static const juce::Identifier kPluginStateId     { "PluginState" };
static const juce::Identifier kPropBcastEnabled  { "broadcastEnabled" };
static const juce::Identifier kPropRecvEnabled   { "receiveEnabled" };
static const juce::Identifier kPropChannelLabel  { "channelLabel" };
static const juce::Identifier kPropColourR       { "colourR" };
static const juce::Identifier kPropColourG       { "colourG" };
static const juce::Identifier kPropColourB       { "colourB" };
static const juce::Identifier kPropColourA       { "colourA" };
static const juce::Identifier kPropBroadcastOnly { "broadcastOnlyMode" };

void PhuBeatSyncMultiScopeAudioProcessor::getStateInformation(juce::MemoryBlock& destData) {
    auto state = apvts.copyState();

    // Store plugin-specific state in a dedicated child ValueTree node to keep
    // it separate from APVTS parameter state and avoid property name collisions.
    while (state.getChildWithName(kPluginStateId).isValid())
        state.removeChild(state.getChildWithName(kPluginStateId), nullptr);

    juce::ValueTree pluginState(kPluginStateId);
    pluginState.setProperty(kPropBcastEnabled, m_broadcastEnabled.load(),      nullptr);
    pluginState.setProperty(kPropRecvEnabled,  m_receiveEnabled.load(),        nullptr);
    pluginState.setProperty(kPropChannelLabel, juce::String::fromUTF8(m_channelLabel.data()), nullptr);
    pluginState.setProperty(kPropColourR,      static_cast<int>(m_colourRGBA[0]), nullptr);
    pluginState.setProperty(kPropColourG,      static_cast<int>(m_colourRGBA[1]), nullptr);
    pluginState.setProperty(kPropColourB,      static_cast<int>(m_colourRGBA[2]), nullptr);
    pluginState.setProperty(kPropColourA,      static_cast<int>(m_colourRGBA[3]), nullptr);
    pluginState.setProperty(kPropBroadcastOnly, m_broadcastOnlyMode.load(), nullptr);
    state.addChild(pluginState, -1, nullptr);

    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void PhuBeatSyncMultiScopeAudioProcessor::setStateInformation(const void* data, int sizeInBytes) {
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));
    if (xmlState != nullptr) {
        if (xmlState->hasTagName(apvts.state.getType())) {
            auto state = juce::ValueTree::fromXml(*xmlState);
            apvts.replaceState(state);

            // Sync channel index after state is restored (instance_channel may have changed)
            syncInstanceIndexToBroadcasters();

            // Restore plugin-specific state.  Try the dedicated child node first
            // (current format); fall back to flat properties for backward compatibility
            // with states saved before this change.
            juce::ValueTree pluginState;
            for (int i = 0; i < state.getNumChildren(); ++i) {
                auto child = state.getChild(i);
                if (child.hasType(kPluginStateId))
                    pluginState = child;
            }
            const auto& src = pluginState.isValid() ? pluginState : state;

            if (src.hasProperty(kPropBcastEnabled))
                setBroadcastEnabled(static_cast<bool>(src.getProperty(kPropBcastEnabled)));
            if (src.hasProperty(kPropRecvEnabled))
                setReceiveEnabled(static_cast<bool>(src.getProperty(kPropRecvEnabled)));
            if (src.hasProperty(kPropChannelLabel))
                copyLabelToBuffer(src.getProperty(kPropChannelLabel).toString(),
                                  m_channelLabel.data());
            if (src.hasProperty(kPropColourR))
                m_colourRGBA[0] = static_cast<uint8_t>(static_cast<int>(src.getProperty(kPropColourR)));
            if (src.hasProperty(kPropColourG))
                m_colourRGBA[1] = static_cast<uint8_t>(static_cast<int>(src.getProperty(kPropColourG)));
            if (src.hasProperty(kPropColourB))
                m_colourRGBA[2] = static_cast<uint8_t>(static_cast<int>(src.getProperty(kPropColourB)));
            if (src.hasProperty(kPropColourA))
                m_colourRGBA[3] = static_cast<uint8_t>(static_cast<int>(src.getProperty(kPropColourA)));
            if (src.hasProperty(kPropBroadcastOnly))
                setBroadcastOnlyMode(static_cast<bool>(src.getProperty(kPropBroadcastOnly)));
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
