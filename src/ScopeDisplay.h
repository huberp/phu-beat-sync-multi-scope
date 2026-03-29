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
 * Also renders remote waveform data received via SampleBroadcaster, reconstructed
 * from raw sample packets using per-sample PPQ computation.
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

    /**
     * Set remote raw sample packets received from other instances.
     * Scatter-writes each sample into the receiver's coordinate space using
     * per-sample PPQ reconstruction, and accumulates into RMS/cancellation
     * metric buffers. sampleRate must be set via setSampleRate() first.
     */
    void setRemoteRawData(const std::vector<SampleBroadcaster::RemoteRawPacket>& remoteData);

    /** Set the sample rate used for per-sample PPQ reconstruction. */
    void setSampleRate(double sampleRate) { m_sampleRate = sampleRate; }

    /** Enable or disable rendering of local data (toggle). */
    void setLocalDisplayEnabled(bool enabled) { m_showLocal = enabled; }
    bool isLocalDisplayEnabled() const { return m_showLocal; }

    /** Enable or disable rendering of remote data (toggle). */
    void setRemoteDisplayEnabled(bool enabled) { m_showRemote = enabled; }
    bool isRemoteDisplayEnabled() const { return m_showRemote; }

    /** Set current PPQ position for playhead marker. */
    void setCurrentPpq(double ppq) { m_currentPpq = ppq; }

    /** Set display range in beats. */
    void setDisplayRangeBeats(double beats) {
        if (beats != m_displayRangeBeats) {
            m_displayRangeBeats  = beats;
            m_rmsOverlayDirty    = true;
            m_cancelOverlayDirty = true;
        }
    }

    /** Show per-1/16-beat RMS envelope as horizontal step lines. */
    void setRmsOverlayEnabled(bool enabled) { m_showRms = enabled; }

    /**
     * Show inter-instance cancellation index as a coloured bar at the bottom.
     * Green = no cancellation, yellow = partial, red = high cancellation.
     * Only meaningful when at least two plugin instances are active.
     */
    void setCancellationOverlayEnabled(bool enabled) { m_showCancellation = enabled; }

    /** Set the Y-axis amplitude scale factor [0.5, 4.0]. Double-click on the UI slider resets to 1.0. */
    void setAmplitudeScale(float scale) { m_amplitudeScale = juce::jlimit(0.5f, 4.0f, scale); }
    float getAmplitudeScale() const { return m_amplitudeScale; }

  private:
    // Local waveform data (copied from BeatSyncBuffer on UI thread)
    std::vector<float> m_localData;

    // Per-instance accumulation buffers in the receiver's coordinate space.
    // Populated incrementally by setRemoteRawData(). Each entry persists across
    // packets so bins written by earlier packets remain visible until overwritten.
    struct RemoteAccumEntry {
        // Waveform display: 4096-bin scatter-write (last-write-wins per bin)
        std::vector<float> bins;           // size == REMOTE_ACCUM_BINS

        // RMS accumulation: per MAX_METRIC_SLOTS slot, sum-of-squares and count
        float rmsAccum[128]{};             // matches MAX_METRIC_SLOTS
        int   rmsCount[128]{};

        // Cancellation accumulation: per MAX_CANCEL_SLOTS slot, sum-of-squares and count
        float cancelAccum[256]{};          // matches MAX_CANCEL_SLOTS
        int   cancelCount[256]{};

        double   lastWindowStart = -1e18;  // receiver-space cycle start, for clearing
        uint32_t lastSeqNum      = 0;      // last processed sequence number
        bool     hasSeq          = false;  // true once at least one packet has been processed
    };
    std::map<uint32_t, RemoteAccumEntry> m_remoteAccumBuffers;
    double m_lastAccumReceiverRange = -1.0; // invalidated when receiver range changes

    static constexpr int REMOTE_ACCUM_BINS = 4096;

    // Sample rate for per-sample PPQ reconstruction in setRemoteRawData()
    double m_sampleRate = 44100.0;

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
    float m_rmsSum[MAX_METRIC_SLOTS]{};       // rmsLocal + sum of per-remote RMS, per slot
    float m_cancellationIndex[MAX_CANCEL_SLOTS]{};

    // Drawing helpers
    void drawGrid(juce::Graphics& g, juce::Rectangle<float> area);
    void drawWaveform(juce::Graphics& g, juce::Rectangle<float> area,
                      const float* data, int numBins, juce::Colour colour, float alpha = 1.0f);
    void drawPlayhead(juce::Graphics& g, juce::Rectangle<float> area);

    /** Compute m_rmsLocal[], m_rmsSum[], and m_cancellationIndex[] from current buffers. */
    void computeMetrics();
    /** Draw RMS step-envelope lines (called after drawGrid, before waveforms). */
    void drawRmsOverlay(juce::Graphics& g, juce::Rectangle<float> area);
    /** Draw the cancellation colour bar at the bottom of the display. */
    void drawCancellationOverlay(juce::Graphics& g, juce::Rectangle<float> area);

    // Amplitude scale factor applied to Y-axis display [0.5, 4.0]
    float m_amplitudeScale = 1.0f;

    // Reusable scratch vectors for computeMetrics() — allocated once, reused every frame
    std::vector<const float*> m_metricRemotePtrs;
    std::vector<int>          m_metricRemoteSizes;

    // Cached overlay images — rebuilt only when data changes, blitted each repaint
    juce::Image m_rmsOverlayImage;
    juce::Image m_cancelOverlayImage;
    bool        m_rmsOverlayDirty    = true;
    bool        m_cancelOverlayDirty = true;
    int         m_lastOverlayWidth   = 0;
    int         m_lastOverlayHeight  = 0;

    // Map raw sample value [-1, +1] to Y coordinate
    float sampleToY(float sample, float top, float height) const;

    // Colour palette for remote instances
    static juce::Colour getRemoteColour(int index);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScopeDisplay)
};
