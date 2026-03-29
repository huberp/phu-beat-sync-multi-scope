#include "ScopeDisplay.h"

// ============================================================================
// Construction
// ============================================================================

ScopeDisplay::ScopeDisplay() {
    setOpaque(true);
}

// ============================================================================
// Data Updates (called from UI timer)
// ============================================================================

void ScopeDisplay::setLocalData(const float* data, int numBins) {
    m_localData.assign(data, data + numBins);
    m_rmsOverlayDirty    = true;
    m_cancelOverlayDirty = true;
}

void ScopeDisplay::setRemoteRawData(
    const std::vector<SampleBroadcaster::RemoteRawPacket>& remoteData) {

    const double receiverRange = m_displayRangeBeats > 0.0 ? m_displayRangeBeats : 1.0;
    const double sampleRate    = m_sampleRate > 0.0 ? m_sampleRate : 44100.0;

    // Invalidate all accum buffers when the receiver's display range changes
    if (receiverRange != m_lastAccumReceiverRange) {
        m_remoteAccumBuffers.clear();
        m_lastAccumReceiverRange = receiverRange;
    }

    for (const auto& pkt : remoteData) {
        if (pkt.numSamples == 0) continue;

        auto& accum = m_remoteAccumBuffers[pkt.instanceID];

        // Ensure display bins are allocated
        if (static_cast<int>(accum.bins.size()) != REMOTE_ACCUM_BINS)
            accum.bins.assign(static_cast<size_t>(REMOTE_ACCUM_BINS), 0.0f);

        // Skip packets we have already processed (deduplication via sequence number).
        // At 30 Hz send / 60 Hz read, the same packet may be returned twice by
        // getReceivedPackets(). Duplicate processing would double-count RMS sums.
        if (accum.hasSeq && pkt.sequenceNumber == accum.lastSeqNum)
            continue;
        accum.lastSeqNum = pkt.sequenceNumber;
        accum.hasSeq     = true;

        // PPQ advance per sample: derived from sender BPM and receiver sample rate
        const double bpm = pkt.bpm > 0.0 ? pkt.bpm : 120.0;
        const double ppqPerSample = bpm / (60.0 * sampleRate);

        // Number of active RMS slots for the current display range — must match
        // computeMetrics() / drawRmsOverlay() so accumulation and read-back use
        // the same slot granularity (normPos × numRmsSlots maps into 0..numRmsSlots-1).
        const int numRmsSlots = juce::jlimit(1, MAX_METRIC_SLOTS,
                                             (int)std::round(m_displayRangeBeats * 16.0));

        // Detect receiver-space cycle boundary from the first sample's PPQ.
        // On a new cycle, clear all accum arrays so stale data from the previous
        // cycle does not corrupt RMS/cancellation metrics.
        const double firstPpq      = pkt.ppqOfFirstSample;
        const double windowStart   = std::floor(firstPpq / receiverRange) * receiverRange;

        if (std::abs(windowStart - accum.lastWindowStart) > 1e-9) {
            std::fill(accum.bins.begin(), accum.bins.end(), 0.0f);
            std::fill(std::begin(accum.rmsAccum),    std::end(accum.rmsAccum),    0.0f);
            std::fill(std::begin(accum.rmsCount),    std::end(accum.rmsCount),    0);
            std::fill(std::begin(accum.cancelAccum), std::end(accum.cancelAccum), 0.0f);
            std::fill(std::begin(accum.cancelCount), std::end(accum.cancelCount), 0);
            accum.lastWindowStart = windowStart;
        }

        // Scatter-write each sample into the receiver's coordinate space
        for (int i = 0; i < static_cast<int>(pkt.numSamples); ++i) {
            const double ppq_i  = firstPpq + static_cast<double>(i) * ppqPerSample;
            double normPos      = std::fmod(ppq_i, receiverRange) / receiverRange;
            if (normPos < 0.0) normPos += 1.0;

            const float s = pkt.samples[i];

            // Waveform display bin (last-write-wins)
            const int bin = static_cast<int>(normPos * REMOTE_ACCUM_BINS);
            if (bin >= 0 && bin < REMOTE_ACCUM_BINS)
                accum.bins[static_cast<size_t>(bin)] = s;

            // RMS accumulation: slot index uses numRmsSlots (= displayRangeBeats × 16)
            // so it matches the slot granularity that computeMetrics() reads back.
            const int rmsSlot = static_cast<int>(normPos * numRmsSlots);
            if (rmsSlot >= 0 && rmsSlot < numRmsSlots) {
                accum.rmsAccum[rmsSlot] += s * s;
                accum.rmsCount[rmsSlot]++;
            }

            // Cancellation accumulation (fine-grained slots)
            const int cancelSlot = static_cast<int>(normPos * MAX_CANCEL_SLOTS);
            if (cancelSlot >= 0 && cancelSlot < MAX_CANCEL_SLOTS) {
                accum.cancelAccum[cancelSlot] += s * s;
                accum.cancelCount[cancelSlot]++;
            }
        }
    }

    // Prune accum buffers for instances that have gone stale (disappeared from
    // the received set). Only prune when remoteData is non-empty so toggling
    // "Show Remote" off (which passes an empty vector) doesn't discard state.
    if (!remoteData.empty()) {
        auto it = m_remoteAccumBuffers.begin();
        while (it != m_remoteAccumBuffers.end()) {
            bool found = false;
            for (const auto& r : remoteData)
                if (r.instanceID == it->first) { found = true; break; }
            it = found ? std::next(it) : m_remoteAccumBuffers.erase(it);
        }
    }

    m_rmsOverlayDirty    = true;
    m_cancelOverlayDirty = true;
}

// ============================================================================
// Painting
// ============================================================================

void ScopeDisplay::paint(juce::Graphics& g) {
    auto bounds = getLocalBounds().toFloat();

    // Background
    g.fillAll(juce::Colour(0xFF1A1A2E));

    // Grid lines
    drawGrid(g, bounds);

    // Analysis overlays — compute metrics and rebuild cached images when needed,
    // then blit in a single drawImageAt call each repaint.
    if (m_showRms || m_showCancellation)
        computeMetrics();

    // Invalidate overlay images when the display area changes
    const int w = static_cast<int>(bounds.getWidth());
    const int h = static_cast<int>(bounds.getHeight());
    if (w != m_lastOverlayWidth || h != m_lastOverlayHeight) {
        m_rmsOverlayDirty    = true;
        m_cancelOverlayDirty = true;
        m_lastOverlayWidth   = w;
        m_lastOverlayHeight  = h;
    }

    if (m_showRms) {
        if (m_rmsOverlayDirty) {
            m_rmsOverlayImage = juce::Image(
                juce::Image::ARGB, juce::jmax(1, w), juce::jmax(1, h), true);
            juce::Graphics ig(m_rmsOverlayImage);
            drawRmsOverlay(ig, { 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h) });
            m_rmsOverlayDirty = false;
        }
        g.drawImageAt(m_rmsOverlayImage,
                      static_cast<int>(bounds.getX()),
                      static_cast<int>(bounds.getY()));
    }
    if (m_showCancellation && !m_remoteAccumBuffers.empty()) {
        if (m_cancelOverlayDirty) {
            m_cancelOverlayImage = juce::Image(
                juce::Image::ARGB, juce::jmax(1, w), juce::jmax(1, h), true);
            juce::Graphics ig(m_cancelOverlayImage);
            drawCancellationOverlay(ig, { 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h) });
            m_cancelOverlayDirty = false;
        }
        g.drawImageAt(m_cancelOverlayImage,
                      static_cast<int>(bounds.getX()),
                      static_cast<int>(bounds.getY()));
    }

    // Remote waveforms (drawn first, underneath local)
    if (m_showRemote && !m_remoteAccumBuffers.empty()) {
        int colourIdx = 0;
        for (const auto& kv : m_remoteAccumBuffers) {
            if (!kv.second.bins.empty())
                drawWaveform(g, bounds, kv.second.bins.data(),
                             static_cast<int>(kv.second.bins.size()),
                             getRemoteColour(colourIdx), 0.5f);
            ++colourIdx;
        }
    }

    // Local waveform (on top)
    if (m_showLocal && !m_localData.empty()) {
        drawWaveform(g, bounds, m_localData.data(),
                     static_cast<int>(m_localData.size()),
                     juce::Colour(0xFF00FF88), 1.0f);
    }

    // Playhead
    drawPlayhead(g, bounds);

    // Border
    g.setColour(juce::Colour(0xFF333355));
    g.drawRect(bounds, 1.0f);
}

// ============================================================================
// Grid
// ============================================================================

void ScopeDisplay::drawGrid(juce::Graphics& g, juce::Rectangle<float> area) {
    g.setColour(juce::Colour(0xFF333355));

    // Horizontal grid lines at linear levels
    const float levels[] = {1.0f, 0.5f, 0.0f, -0.5f, -1.0f};
    for (float lv : levels) {
        float y = sampleToY(lv, area.getY(), area.getHeight());
        g.drawHorizontalLine(static_cast<int>(y), area.getX(), area.getRight());
    }

    // Vertical grid lines at beat divisions
    if (m_displayRangeBeats > 0.0) {
        int numBeats = static_cast<int>(m_displayRangeBeats);
        if (numBeats < 1) numBeats = 1;
        for (int i = 0; i <= numBeats; ++i) {
            float x = area.getX() +
                      (static_cast<float>(i) / static_cast<float>(numBeats)) * area.getWidth();
            g.drawVerticalLine(static_cast<int>(x), area.getY(), area.getBottom());
        }
    }

    // Linear labels
    g.setColour(juce::Colour(0xFF666688));
    g.setFont(10.0f);
    const float labelLevels[] = {1.0f, 0.5f, 0.0f, -0.5f, -1.0f};
    for (float lv : labelLevels) {
        float y = sampleToY(lv, area.getY(), area.getHeight());
        juce::String text = (lv == 0.0f) ? "0" : juce::String(lv, 1);
        g.drawText(text,
                   static_cast<int>(area.getX() + 2), static_cast<int>(y - 6), 30, 12,
                   juce::Justification::centredLeft);
    }
}

// ============================================================================
// Waveform Drawing
// ============================================================================

void ScopeDisplay::drawWaveform(juce::Graphics& g, juce::Rectangle<float> area,
                                const float* data, int numBins,
                                juce::Colour colour, float alpha) {
    if (numBins < 2) return;

    // Decimate to screen pixel width: draw at most one point per pixel column.
    // When multiple bins map to the same pixel, take the min/max pair and draw
    // a vertical line segment — this preserves sharp transients at all zoom levels
    // without aliasing, while reducing path operations from O(numBins) to O(width).
    const int screenW = juce::jmax(2, static_cast<int>(area.getWidth()));
    juce::Path path;
    bool started = false;

    for (int px = 0; px < screenW; ++px) {
        const float x = area.getX() + static_cast<float>(px);

        // Map pixel column to bin range [binStart, binEnd)
        const int binStart = px       * numBins / screenW;
        const int binEnd   = (px + 1) * numBins / screenW;

        if (binStart >= numBins) break;

        if (binEnd <= binStart + 1) {
            // One bin per pixel: simple line-to
            const float y = sampleToY(data[binStart], area.getY(), area.getHeight());
            if (!started) { path.startNewSubPath(x, y); started = true; }
            else            path.lineTo(x, y);
        } else {
            // Multiple bins per pixel: find min/max for vertical stroke
            float minV = data[binStart], maxV = data[binStart];
            for (int b = binStart + 1; b < binEnd && b < numBins; ++b) {
                if (data[b] < minV) minV = data[b];
                if (data[b] > maxV) maxV = data[b];
            }
            const float yMax = sampleToY(maxV, area.getY(), area.getHeight()); // larger amp = smaller y
            const float yMin = sampleToY(minV, area.getY(), area.getHeight());

            // Start a new subpath for each vertical min/max stroke so columns
            // are not connected across X by diagonal segments.
            path.startNewSubPath(x, yMax);
            path.lineTo(x, yMin);
        }
    }

    g.setColour(colour.withAlpha(alpha));
    g.strokePath(path, juce::PathStrokeType(1.5f));
}

// ============================================================================
// Analysis Metrics (RMS + Inter-Instance Cancellation)
// ============================================================================

void ScopeDisplay::computeMetrics() {
    const int numRmsSlots = juce::jlimit(1, MAX_METRIC_SLOTS,
                                         (int)std::round(m_displayRangeBeats * 16.0));

    for (int s = 0; s < MAX_METRIC_SLOTS; ++s) {
        m_rmsLocal[s] = 0.0f;
        m_rmsSum[s]   = 0.0f;
    }
    for (int s = 0; s < MAX_CANCEL_SLOTS; ++s)
        m_cancellationIndex[s] = 0.0f;

    const int  localBins  = (int)m_localData.size();
    const int  numRemotes = (int)m_remoteAccumBuffers.size();
    const bool doCancel   = m_showCancellation && !m_remoteAccumBuffers.empty();

    // Collect pointers to remote display bins (used for cancellation numerator)
    m_metricRemotePtrs.clear();
    m_metricRemoteSizes.clear();
    m_metricRemotePtrs.reserve(static_cast<size_t>(numRemotes));
    m_metricRemoteSizes.reserve(static_cast<size_t>(numRemotes));
    for (const auto& kv : m_remoteAccumBuffers) {
        m_metricRemotePtrs.push_back(kv.second.bins.data());
        m_metricRemoteSizes.push_back((int)kv.second.bins.size());
    }

    // ---- RMS per 1/16-beat slot ----
    // rmsLocal[s]  = sqrt( mean(local_bins²) ) over the slot window
    // rmsRemote[s] = sqrt( rmsAccum[id][s] / rmsCount[id][s] ) per remote id
    // rmsSum[s]    = rmsLocal[s] + Σ_id rmsRemote[id][s]   (sum of individual RMSes)
    for (int s = 0; s < numRmsSlots; ++s) {
        const int wStart = s       * REMOTE_ACCUM_BINS / numRmsSlots;
        const int wEnd   = (s + 1) * REMOTE_ACCUM_BINS / numRmsSlots;
        const int wCount = wEnd - wStart;
        if (wCount <= 0) continue;

        float sumSqLocal = 0.0f;
        for (int b = wStart; b < wEnd; ++b) {
            const int   lb = localBins > 0 ? b * localBins / REMOTE_ACCUM_BINS : 0;
            const float lv = (localBins > 0 && lb < localBins)
                                 ? m_localData[static_cast<size_t>(lb)] : 0.0f;
            sumSqLocal += lv * lv;
        }
        m_rmsLocal[s] = std::sqrt(sumSqLocal / static_cast<float>(wCount));

        float rmsSum = m_rmsLocal[s];
        for (const auto& kv : m_remoteAccumBuffers) {
            const float* ra = kv.second.rmsAccum;
            const int*   rc = kv.second.rmsCount;
            rmsSum += (rc[s] > 0) ? std::sqrt(ra[s] / static_cast<float>(rc[s])) : 0.0f;
        }
        m_rmsSum[s] = rmsSum;
    }

    // ---- Cancellation: fine-grained windows (~4ms each at 4-beat display) ----
    // Denominator D[s] = rmsLocal[s] + Σ_id sqrt(cancelAccum[id][s] / cancelCount[id][s])
    // Numerator   N[s] = RMS of (local_bins + Σ_id remote_bins) over the cancel slot window
    // CI[s] = 1 - N[s] / D[s], level-weighted
    if (doCancel) {
        for (int s = 0; s < MAX_CANCEL_SLOTS; ++s) {
            const int wStart = s       * REMOTE_ACCUM_BINS / MAX_CANCEL_SLOTS;
            const int wEnd   = (s + 1) * REMOTE_ACCUM_BINS / MAX_CANCEL_SLOTS;
            const int wCount = wEnd - wStart;
            if (wCount <= 0) continue;

            // Denominator: sum of individual per-slot RMSes from dedicated accum
            float sumSqLocal = 0.0f;
            for (int b = wStart; b < wEnd; ++b) {
                const int   lb = localBins > 0 ? b * localBins / REMOTE_ACCUM_BINS : 0;
                const float lv = (localBins > 0 && lb < localBins)
                                     ? m_localData[static_cast<size_t>(lb)] : 0.0f;
                sumSqLocal += lv * lv;
            }
            float D = std::sqrt(sumSqLocal / static_cast<float>(wCount));
            for (const auto& kv : m_remoteAccumBuffers) {
                const float* ca = kv.second.cancelAccum;
                const int*   cc = kv.second.cancelCount;
                D += (cc[s] > 0) ? std::sqrt(ca[s] / static_cast<float>(cc[s])) : 0.0f;
            }

            // Numerator: RMS of summed waveform over the cancel slot window
            float sumSqSum = 0.0f;
            for (int b = wStart; b < wEnd; ++b) {
                const int   lb = localBins > 0 ? b * localBins / REMOTE_ACCUM_BINS : 0;
                const float lv = (localBins > 0 && lb < localBins)
                                     ? m_localData[static_cast<size_t>(lb)] : 0.0f;
                float sumVal = lv;
                for (int ri = 0; ri < numRemotes; ++ri) {
                    const float rv = (b < m_metricRemoteSizes[ri])
                                         ? m_metricRemotePtrs[ri][b] : 0.0f;
                    sumVal += rv;
                }
                sumSqSum += sumVal * sumVal;
            }
            const float N = std::sqrt(sumSqSum / static_cast<float>(wCount));

            // Noise floor at ~-100 dBFS
            if (D > 1e-4f) {
                const float ci = juce::jlimit(0.0f, 1.0f, 1.0f - N / D);

                // Level-weighted CI: cancellation is only meaningful when signal is present.
                // Weight rises as sqrt(D / D_ref), clamped to 1 above D_ref = 0.1 (-20 dBFS).
                constexpr float D_REF = 0.1f;
                const float levelWeight = std::sqrt(
                    juce::jlimit(0.0f, 1.0f, D / D_REF));
                m_cancellationIndex[s] = ci * levelWeight;
            }
        }
    }
}

void ScopeDisplay::drawRmsOverlay(juce::Graphics& g, juce::Rectangle<float> area) {
    const int   numSlots = juce::jlimit(1, MAX_METRIC_SLOTS,
                                        (int)std::round(m_displayRangeBeats * 16.0));
    const float slotW = area.getWidth() / static_cast<float>(numSlots);

    // When remote display is off, show only local RMS so the line never reflects
    // hidden data. When remote is on, show the combined sum RMS.
    const bool useSum = m_showRemote && !m_remoteAccumBuffers.empty();

    for (int s = 0; s < numSlots; ++s) {
        const float rms = useSum ? m_rmsSum[s] : m_rmsLocal[s];
        if (rms < 1e-6f) continue; // skip only truly silent bins (~-120 dBFS)

        const float x0  = area.getX() + static_cast<float>(s)     * slotW;
        const float w   = slotW;
        const float lineY = sampleToY(rms, area.getY(), area.getHeight());

        // Layered glow: outer halo → inner glow → bright core  (blue-white palette)
        g.setColour(juce::Colour(0x1A44AAFF)); // outer halo, 5px spread
        g.fillRect(x0, lineY - 2.5f, w, 5.0f);

        g.setColour(juce::Colour(0x5566CCFF)); // inner glow, 3px
        g.fillRect(x0, lineY - 1.5f, w, 3.0f);

        g.setColour(juce::Colour(0xEEAAEEFF)); // bright core, 2px (ice-blue/white)
        g.fillRect(x0, lineY - 0.5f, w, 2.0f);
    }
}

void ScopeDisplay::drawCancellationOverlay(juce::Graphics& g, juce::Rectangle<float> area) {
    const int   numSlots = MAX_CANCEL_SLOTS;  // fine-grained, constant 256 windows
    const float slotW = area.getWidth() / static_cast<float>(numSlots);
    constexpr float barH = 6.0f;
    const float barY = area.getBottom() - barH;

    for (int s = 0; s < numSlots; ++s) {
        const float ci = m_cancellationIndex[s];

        // Colour: green (0) → yellow (0.4) → red (1.0)
        juce::Colour col;
        if (ci < 0.4f)
            col = juce::Colour(0xFF00BB55)
                      .interpolatedWith(juce::Colour(0xFFFFCC00), ci / 0.4f);
        else
            col = juce::Colour(0xFFFFCC00)
                      .interpolatedWith(juce::Colour(0xFFFF3300), (ci - 0.4f) / 0.6f);

        g.setColour(col.withAlpha(0.85f));
        g.fillRect(area.getX() + static_cast<float>(s) * slotW, barY, slotW, barH);
    }
}

// ============================================================================
// Playhead
// ============================================================================

void ScopeDisplay::drawPlayhead(juce::Graphics& g, juce::Rectangle<float> area) {
    if (m_displayRangeBeats <= 0.0) return;

    double normPos = std::fmod(m_currentPpq, m_displayRangeBeats) / m_displayRangeBeats;
    if (normPos < 0.0) normPos += 1.0;

    float x = area.getX() + static_cast<float>(normPos) * area.getWidth();

    g.setColour(juce::Colour(0xAAFFFFFF));
    g.drawVerticalLine(static_cast<int>(x), area.getY(), area.getBottom());
}

// ============================================================================
// Helpers
// ============================================================================

float ScopeDisplay::sampleToY(float sample, float top, float height) const {
    // Apply visual Y-scale then map [-1, +1] to [bottom, top]: +1 at top, -1 at bottom
    const float scaled = juce::jlimit(-1.0f, 1.0f, sample * m_amplitudeScale);
    float normalized = (scaled + 1.0f) * 0.5f; // [-1,+1] → [0,1]
    normalized = juce::jlimit(0.0f, 1.0f, normalized);
    return top + (1.0f - normalized) * height;
}

juce::Colour ScopeDisplay::getRemoteColour(int index) {
    // Colour palette for remote instances
    static const juce::Colour colours[] = {
        juce::Colour(0xFFFF6B6B), // Red
        juce::Colour(0xFF4ECDC4), // Teal
        juce::Colour(0xFFFFE66D), // Yellow
        juce::Colour(0xFFA8E6CF), // Mint
        juce::Colour(0xFFFF8C94), // Salmon
        juce::Colour(0xFF88D8B0), // Light green
        juce::Colour(0xFFB8A9C9), // Lavender
        juce::Colour(0xFFF6CD61), // Gold
    };
    static const int numColours = sizeof(colours) / sizeof(colours[0]);
    return colours[index % numColours];
}
