#pragma once

#include "../lib/audio/BucketSet.h"
#include "../lib/audio/RawSampleBuffer.h"
#include "../lib/LinkwitzRileyFilter.h"
#include "../lib/network/CtrlBroadcaster.h"
#include "../lib/network/SampleBroadcaster.h"
#include "gl/ScopeGLCoordinator.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <map>
#include <vector>

using phu::network::SampleBroadcaster;

/**
 * ScopeDisplay — beat-synced oscilloscope display component.
 *
 * Renders waveform data for up to 8 instances (1 local + 7 remotes) aligned to
 * musical position.  Each instance's raw sample stream is written into a
 * RawSampleBuffer (position-addressed overwrite ring), with the HP/LP display
 * filter applied per-sample before the write.  BucketSet-driven dirty tracking
 * ensures only changed regions are recomputed for the RMS and cancellation
 * overlays.
 *
 * - X axis: musical position [0, 1) normalized within display range
 * - Y axis: sample amplitude [-1, +1] mapped to component height
 * - Playhead marker showing current PPQ position
 */
class ScopeDisplay : public juce::Component {
  public:
    /** Maximum concurrent instances (1 local + 7 remotes). */
    static constexpr int MAX_INSTANCES = 8;
    /** Number of display scatter bins per instance (last-write-wins, 4096). */
    static constexpr int DISPLAY_BINS  = 4096;

    ScopeDisplay();
    ~ScopeDisplay() override;

    void paint(juce::Graphics& g) override;

    /** Called when the component is added to a parent; used to initialise the OpenGL context. */
    void parentHierarchyChanged() override;

    /** Returns true if the OpenGL renderer is active and drawing waveforms. */
    bool isOpenGLActive() const;

    // -------------------------------------------------------------------------
    // Configuration
    // -------------------------------------------------------------------------

    /**
     * Prepare the pipeline for new display parameters.
     * Clears and resizes all RawSampleBuffers and BucketSets.
     * Call whenever displayBeats, bpm, or sampleRate changes.
     */
    void prepare(double displayBeats, double bpm, double sampleRate);

    /**
     * Update HP/LP display filter parameters (applied symmetrically to all
     * active instances).  Resets all per-instance filter states.
     * Call whenever filter enable/frequency or sample rate changes.
     */
    void setFilterParams(bool hpEnabled, float hpFreq,
                         bool lpEnabled, float lpFreq,
                         double sampleRate);

    /** Set the colour used for the local waveform trace. */
    void setLocalColour(juce::Colour colour) { m_localColour = colour; }

    /** Enable or disable rendering of local data. */
    void setLocalDisplayEnabled(bool enabled) { m_showLocal = enabled; }
    bool isLocalDisplayEnabled() const { return m_showLocal; }

    /** Enable or disable rendering of remote data. */
    void setRemoteDisplayEnabled(bool enabled) { m_showRemote = enabled; }
    bool isRemoteDisplayEnabled() const { return m_showRemote; }

    /** Set current PPQ position for the playhead marker. */
    void setCurrentPpq(double ppq) { m_currentPpq = ppq; }

    /** Inform ScopeDisplay of the current display range (must match prepare()). */
    void setDisplayRangeBeats(double beats) { m_displayRangeBeats = beats; }

    /** Show per-1/16-beat RMS envelope as horizontal step lines. */
    void setRmsOverlayEnabled(bool enabled) { m_showRms = enabled; }

    /**
     * Show inter-instance cancellation index as a coloured bar at the bottom.
     * Green = no cancellation, yellow = partial, red = high cancellation.
     * Meaningful only when at least two plugin instances are active.
     */
    void setCancellationOverlayEnabled(bool enabled) { m_showCancellation = enabled; }

    /** Y-axis amplitude scale factor [0.5, 4.0]. Double-click resets to 1.0. */
    void setAmplitudeScale(float scale) { m_amplitudeScale = juce::jlimit(0.5f, 4.0f, scale); }
    float getAmplitudeScale() const { return m_amplitudeScale; }

    /** Show an overlay that indicates broadcast-only mode and suppresses playhead rendering. */
    void setBroadcastOnlyOverlayEnabled(bool enabled) { m_broadcastOnlyOverlayEnabled = enabled; }

    // -------------------------------------------------------------------------
    // Data ingestion (called from the UI timer thread)
    // -------------------------------------------------------------------------

    /**
     * Write one local mono sample at the given absolute PPQ position.
     * The configured HP/LP display filter is applied before writing into the
     * ring buffer; the affected RMS and cancellation buckets are marked dirty.
     */
    void writeLocalSample(float sample, double ppq);

    /**
     * Consume remote raw-sample packets.
     * For each packet, per-instance HP/LP filter state is applied to every sample
     * before writing into its RawSampleBuffer.  Remote instances beyond
     * MAX_INSTANCES − 1 are silently discarded.
     *
     * @param packets  Latest snapshot from SampleBroadcaster::getReceivedPackets().
     * @param infos    Latest snapshot from CtrlBroadcaster::getRemoteInfos().
     */
    void writeRemotePackets(
        const std::vector<SampleBroadcaster::RemoteRawPacket>& packets,
        const std::vector<phu::network::RemoteInstanceInfo>&    infos);

    /**
     * Deactivate all remote instance slots (e.g. when receive is toggled off).
     */
    void clearRemoteInstances();

    /**
     * Update the local instance's display slot index.
     * @param newIndex  User-assigned channel index [1, 8].
     */
    void setLocalInstanceIndex(int newIndex);

    /**
     * Scatter every active RawSampleBuffer to its 4096-bin display array
     * (dirty RMS buckets only), then recompute dirty RMS and cancellation
     * buckets when their respective overlays are enabled.
     * Call once per frame after all writeLocalSample / writeRemotePackets calls
     * and before repaint().
     */
    void computeFrame();

    /** Returns true if new sample data was written since the last clearNewFrameData(). */
    bool hasNewFrameData() const { return m_frameHasNewData; }

    /** Clears the new-data flag; call after computeFrame() + repaint(). */
    void clearNewFrameData() { m_frameHasNewData = false; }

  private:
    // -------------------------------------------------------------------------
    // Per-instance data
    // -------------------------------------------------------------------------

    struct InstanceSlot {
        phu::audio::RawSampleBuffer  buffer;
        phu::audio::BucketSet        rmsBuckets   { phu::audio::BucketSet::Kind::Rms    };
        phu::audio::BucketSet        cancelBuckets{ phu::audio::BucketSet::Kind::Cancel };
        LinkwitzRiley::LinkwitzRileyFilter<float> filterHP;
        LinkwitzRiley::LinkwitzRileyFilter<float> filterLP;

        bool     active     = false;
        bool     isLocal    = false;   ///< true only for the local instance slot
        uint32_t instanceID = 0;

        /** Per-rmsBucket RMS for this instance; size == rmsBuckets.bucketCount(). */
        std::vector<float> rmsValues;

        /** 4096-bin scatter array for waveform rendering. */
        std::vector<float> displayBins;

        /** Last display-cycle start seen on this instance (for filter reset on wrap). */
        double lastWindowStart = -1e18;

        /** Remote packet sequence deduplication. */
        uint32_t lastSeqNum = 0;
        bool     hasSeq     = false;
    };

    /** Slot 0 = local instance; slots 1..7 = remote instances (in arrival order). */
    std::array<InstanceSlot, MAX_INSTANCES> m_instances;

    /** Per-instance identity (colour, label) from CtrlBroadcaster. */
    std::map<uint32_t, phu::network::RemoteInstanceInfo> m_remoteInfoMap;

    // -------------------------------------------------------------------------
    // Configuration
    // -------------------------------------------------------------------------

    double m_displayRangeBeats = 4.0;
    double m_bpm               = 120.0;
    double m_sampleRate        = 44100.0;
    bool   m_hpEnabled         = false;
    float  m_hpFreq            = 80.0f;
    bool   m_lpEnabled         = false;
    float  m_lpFreq            = 8000.0f;

    /** User-assigned channel index for the local instance [1, 8].  Determines which
     *  InstanceSlot the local audio stream is written into. */
    int m_localInstanceIndex = 1;

    // -------------------------------------------------------------------------
    // Computed metric arrays (filled by computeFrame, read by paint)
    // -------------------------------------------------------------------------

    /** Maximum number of RMS display slots (8 beats × 16 = 128). */
    static constexpr int MAX_METRIC_SLOTS = 128;
    /** Maximum number of cancellation display slots. */
    static constexpr int MAX_CANCEL_SLOTS = 256;

    /** RMS of the local instance per 1/16-beat slot. */
    float m_rmsLocal[MAX_METRIC_SLOTS]{};
    /** Sum of per-instance RMSes per 1/16-beat slot (local + all remotes). */
    float m_rmsSum  [MAX_METRIC_SLOTS]{};
    /** Cancellation index per fine-grained slot. */
    float m_cancellationIndex[MAX_CANCEL_SLOTS]{};

    /** Actual number of active RMS buckets (from first active instance). */
    int m_numActiveRmsBuckets    = 0;
    /** Actual number of active cancel buckets (from first active instance). */
    int m_numActiveCancelBuckets = 0;

    // -------------------------------------------------------------------------
    // Display state
    // -------------------------------------------------------------------------

    juce::Colour m_localColour { 0xFF00FF88 }; // default bright green
    bool   m_showLocal        = true;
    bool   m_showRemote       = true;
    double m_currentPpq       = 0.0;
    bool   m_showRms          = false;
    bool   m_showCancellation = false;
    float  m_amplitudeScale   = 1.0f;
    bool   m_broadcastOnlyOverlayEnabled = false;

    // -------------------------------------------------------------------------
    // Overlay image cache
    // -------------------------------------------------------------------------

    juce::Image m_rmsOverlayImage;
    juce::Image m_cancelOverlayImage;
    bool m_rmsOverlayDirty    = true;
    bool m_cancelOverlayDirty = true;
    int  m_lastOverlayWidth   = 0;
    int  m_lastOverlayHeight  = 0;

    /** Set to true whenever applyFilterAndWriteBatch() or clearRemoteInstances()
     *  writes data this frame.  Consumed and cleared by the editor's timerCallback()
     *  to skip computeFrame() + repaint() when nothing changed (DAW stopped). */
    bool m_frameHasNewData = false;

    // -------------------------------------------------------------------------
    // OpenGL accelerated rendering (optional)
    // -------------------------------------------------------------------------

    std::unique_ptr<ScopeGLCoordinator> m_glCoordinator;
    bool m_glAttachAttempted = false;

    /** Push current frame data to the OpenGL renderer (if active). */
    void updateGLFrameData();

    // -------------------------------------------------------------------------
    // Private helpers
    // -------------------------------------------------------------------------

    /** Prepare one instance slot for current display parameters (resize + clear). */
    void prepareInstance(InstanceSlot& inst);

    /** Set filter coefficients on a slot from the stored m_hp/lp params. */
    void updateInstanceFilter(InstanceSlot& inst);

    /**
     * Apply the HP/LP filter to every sample in [samples, samples+numSamples)
     * and write them into the slot's ring buffer starting at the index that
     * corresponds to ppqOfFirstSample.
     *
     * The start index is computed once via indexForPpq().  Subsequent indices
     * advance by 1 modulo buffer size (guaranteed by the math:  consecutive
     * PPQ values spaced bpm/(60*sr) apart map to consecutive ring indices).
     * When the write wraps past the end of the ring it continues from index 0
     * (two-phase memcpy-style split).
     *
     * Dirty marking is deferred to a single markDirtyRange() call after all
     * samples are written, covering only the buckets that were actually touched.
     *
     * @param samples           Array of raw (unfiltered) mono samples.
     * @param numSamples        Number of samples in the array.
     * @param ppqOfFirstSample  Absolute PPQ position of samples[0].
     * @param ppqPerSample      PPQ advance per sample = bpm / (60 * sampleRate).
     */
    void applyFilterAndWriteBatch(InstanceSlot& inst,
                                  const float* samples, int numSamples,
                                  double ppqOfFirstSample, double ppqPerSample);

    /** Scatter dirty-bucket samples to the slot's 4096-bin display array. */
    void scatterInstance(InstanceSlot& inst);

    /** Recompute dirty RMS buckets for all active instances; fill m_rmsLocal/rmsSum. */
    void recomputeRms();

    /** Recompute dirty cancellation buckets; fill m_cancellationIndex. */
    void recomputeCancellation();

    // Drawing helpers
    void drawGrid(juce::Graphics& g, juce::Rectangle<float> area);
    void drawWaveform(juce::Graphics& g, juce::Rectangle<float> area,
                      const float* data, int numBins,
                      juce::Colour colour, float alpha = 1.0f);
    void drawPlayhead(juce::Graphics& g, juce::Rectangle<float> area);
    void drawRmsOverlay(juce::Graphics& g, juce::Rectangle<float> area);
    void drawCancellationOverlay(juce::Graphics& g, juce::Rectangle<float> area);

    float sampleToY(float sample, float top, float height) const;
    static juce::Colour getRemoteColour(int index);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScopeDisplay)
};
