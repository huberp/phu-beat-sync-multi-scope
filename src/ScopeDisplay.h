#pragma once

#include "../lib/audio/BeatSyncBuffer.h"
#include "../lib/network/SampleBroadcaster.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <map>
#include <vector>

using phu::audio::BeatSyncBuffer;
using phu::network::SampleBroadcaster;

/**
 * ScopeDisplay — beat-synced oscilloscope display component.
 *
 * Renders local BeatSyncBuffer data as a waveform trace indexed by musical position.
 * Also renders remote waveform data received via SampleBroadcaster, with an option
 * to show/hide remote traces (following the pattern from phu-splitter).
 *
 * The display shows raw audio sample data mapped to pixel coordinates:
 * - X axis: musical position [0, 1) normalized within display range
 * - Y axis: sample amplitude [-1, +1] mapped to component height
 * - Playhead marker showing current PPQ position
 */
class ScopeDisplay : public juce::Component {
  public:
    ScopeDisplay();
    ~ScopeDisplay() override = default;

    void paint(juce::Graphics& g) override;

    /** Set local beat-synced waveform data for rendering (from BeatSyncBuffer). */
    void setLocalData(const float* data, int numBins);

    /** Set remote waveform data received from other instances. */
    void setRemoteData(const std::vector<SampleBroadcaster::RemoteSampleData>& remoteData);

    /** Enable or disable rendering of local data (toggle). */
    void setLocalDisplayEnabled(bool enabled) { m_showLocal = enabled; }
    bool isLocalDisplayEnabled() const { return m_showLocal; }

    /** Enable or disable rendering of remote data (toggle). */
    void setRemoteDisplayEnabled(bool enabled) { m_showRemote = enabled; }
    bool isRemoteDisplayEnabled() const { return m_showRemote; }

    /** Set current PPQ position for playhead marker. */
    void setCurrentPpq(double ppq) { m_currentPpq = ppq; }

    /** Set display range in beats. */
    void setDisplayRangeBeats(double beats) { m_displayRangeBeats = beats; }

    /** Show per-1/16-beat RMS envelope as horizontal step lines. */
    void setRmsOverlayEnabled(bool enabled) { m_showRms = enabled; }

    /**
     * Show inter-instance cancellation index as a coloured bar at the bottom.
     * Green = no cancellation, yellow = partial, red = high cancellation.
     * Only meaningful when at least two plugin instances are active.
     */
    void setCancellationOverlayEnabled(bool enabled) { m_showCancellation = enabled; }

  private:
    // Local waveform data (copied from BeatSyncBuffer on UI thread)
    std::vector<float> m_localData;

    // Remote waveform data from other instances (raw, kept for metadata only)
    std::vector<SampleBroadcaster::RemoteSampleData> m_remoteData;

    // Per-instance accumulation buffers already projected into the receiver's coordinate
    // space. Populated incrementally by setRemoteData() so bins written by previous
    // packets persist until overwritten — prevents blinking when senderRange < receiverRange
    // and prevents double-painting when senderRange > receiverRange.
    struct RemoteAccumEntry {
        std::vector<float> bins;          // size == REMOTE_ACCUM_BINS, receiver-normalised
        double lastWindowStart = -1e18;   // sender's ppq window start from last packet
    };
    std::map<uint32_t, RemoteAccumEntry> m_remoteAccumBuffers;
    double m_lastAccumReceiverRange = -1.0; // invalidated when receiver range changes

    static constexpr int REMOTE_ACCUM_BINS = SampleBroadcaster::MAX_SAMPLE_BINS;

    // Display state
    bool m_showLocal = true;
    bool m_showRemote = true;
    double m_currentPpq = 0.0;
    double m_displayRangeBeats = 4.0;

    // Analysis overlays
    bool m_showRms = false;
    bool m_showCancellation = false;

    // RMS: 1/16-beat slots, max 8 beats × 16 = 128 slots
    static constexpr int MAX_METRIC_SLOTS = 128;
    // Cancellation: fixed fine-grained windows (~4ms each at 4-beat display range)
    static constexpr int MAX_CANCEL_SLOTS = 256;

    // Per-slot metric arrays, filled by computeMetrics() on the paint thread
    float m_rmsLocal[MAX_METRIC_SLOTS]{};
    float m_rmsSum[MAX_METRIC_SLOTS]{};       // RMS of (local + all remotes) per slot
    float m_cancellationIndex[MAX_CANCEL_SLOTS]{};

    // Drawing helpers
    void drawGrid(juce::Graphics& g, juce::Rectangle<float> area);
    void drawWaveform(juce::Graphics& g, juce::Rectangle<float> area,
                      const float* data, int numBins, juce::Colour colour, float alpha = 1.0f);
    void drawPlayhead(juce::Graphics& g, juce::Rectangle<float> area);

    /** Compute m_rmsLocal[] and m_cancellationIndex[] from the current local/remote buffers. */
    void computeMetrics();
    /** Draw RMS step-envelope lines (called after drawGrid, before waveforms). */
    void drawRmsOverlay(juce::Graphics& g, juce::Rectangle<float> area);
    /** Draw the cancellation colour bar at the bottom of the display. */
    void drawCancellationOverlay(juce::Graphics& g, juce::Rectangle<float> area);

    // Map raw sample value [-1, +1] to Y coordinate
    static float sampleToY(float sample, float top, float height);

    // Colour palette for remote instances
    static juce::Colour getRemoteColour(int index);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScopeDisplay)
};
