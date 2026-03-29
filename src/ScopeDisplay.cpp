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
}

void ScopeDisplay::setRemoteData(
    const std::vector<SampleBroadcaster::RemoteSampleData>& remoteData) {
    m_remoteData = remoteData;

    const double receiverRange = m_displayRangeBeats > 0.0 ? m_displayRangeBeats : 1.0;

    // Invalidate all accum buffers when the receiver's display range changes
    if (receiverRange != m_lastAccumReceiverRange) {
        m_remoteAccumBuffers.clear();
        m_lastAccumReceiverRange = receiverRange;
    }

    // Scatter-write each incoming packet's bins into the receiver-space accum buffer.
    //
    // Two-layer freshness guard:
    //   1. Sender-side: PluginProcessor clears its BeatSyncBuffer on cycle transition,
    //      so silent bins are already zero in the packet — primary fix.
    //   2. Receiver-side (this code): on a new sender cycle, clear the affected
    //      receiver bins, then write only 0..playheadBin (bins the sender has swept
    //      past in the current cycle) — defence-in-depth against in-flight stale packets.
    for (const auto& remote : remoteData) {
        const int numBins = static_cast<int>(remote.samples.size());
        if (numBins == 0) continue;

        const double senderRange = remote.displayRangeBeats > 0.0f
                                       ? static_cast<double>(remote.displayRangeBeats)
                                       : 1.0;
        const double senderWindowStart =
            std::floor(remote.ppqPosition / senderRange) * senderRange;

        // How far into the current sender cycle is the playhead? [0, numBins)
        const double normPlayhead =
            std::fmod(remote.ppqPosition, senderRange) / senderRange;
        const int freshBins = static_cast<int>(normPlayhead * numBins);

        auto& accum = m_remoteAccumBuffers[remote.instanceID];
        if (static_cast<int>(accum.bins.size()) != REMOTE_ACCUM_BINS)
            accum.bins.assign(static_cast<size_t>(REMOTE_ACCUM_BINS), 0.0f);

        // Cycle transition: sender has entered a new display window.
        // For short windows (<= 1 beat), clear aggressively to avoid previous-beat
        // bleed. For longer windows, preserve accumulated history.
        if (std::abs(senderWindowStart - accum.lastWindowStart) > 1e-9) {
            if (senderRange >= receiverRange) {
                // Sender covers full receiver range. Only hard-clear for short windows.
                if (receiverRange <= 1.0 + 1e-9)
                    std::fill(accum.bins.begin(), accum.bins.end(), 0.0f);
            } else {
                // Sender covers a sub-range — clear only the affected receiver bins.
                const double normStart =
                    std::fmod(senderWindowStart, receiverRange) / receiverRange;
                const int binStart = static_cast<int>(normStart * REMOTE_ACCUM_BINS);
                const int binCount =
                    static_cast<int>((senderRange / receiverRange) * REMOTE_ACCUM_BINS);
                for (int b = 0; b < binCount; ++b)
                    accum.bins[static_cast<size_t>(
                        (binStart + b) % REMOTE_ACCUM_BINS)] = 0.0f;
            }
            accum.lastWindowStart = senderWindowStart;
        }

        // Write only the freshly-swept portion of the sender's buffer.
        // Bins ahead of the playhead are stale from the previous cycle — skip them.
        for (int i = 0; i <= freshBins && i < numBins; ++i) {
            const double absolutePpq =
                senderWindowStart +
                (static_cast<double>(i) / static_cast<double>(numBins)) * senderRange;

            double normPos = std::fmod(absolutePpq, receiverRange) / receiverRange;
            if (normPos < 0.0) normPos += 1.0;

            const int bin = static_cast<int>(normPos * REMOTE_ACCUM_BINS);
            if (bin >= 0 && bin < REMOTE_ACCUM_BINS)
                accum.bins[static_cast<size_t>(bin)] = remote.samples[static_cast<size_t>(i)];
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

    // Analysis overlays — computed and drawn before waveforms so traces sit on top
    if (m_showRms || m_showCancellation)
        computeMetrics();
    if (m_showRms)
        drawRmsOverlay(g, bounds);
    if (m_showCancellation && !m_remoteAccumBuffers.empty())
        drawCancellationOverlay(g, bounds);

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

    juce::Path path;
    bool started = false;

    for (int i = 0; i < numBins; ++i) {
        float x = area.getX() + (static_cast<float>(i) / static_cast<float>(numBins - 1)) *
                                     area.getWidth();
        float y = sampleToY(data[i], area.getY(), area.getHeight());

        if (!started) {
            path.startNewSubPath(x, y);
            started = true;
        } else {
            path.lineTo(x, y);
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

    // Cache remote bin pointers for fast inner-loop access
    std::vector<const float*> remotePtrs;
    std::vector<int>          remoteSizes;
    remotePtrs.reserve(static_cast<size_t>(numRemotes));
    remoteSizes.reserve(static_cast<size_t>(numRemotes));
    for (const auto& kv : m_remoteAccumBuffers) {
        remotePtrs.push_back(kv.second.bins.data());
        remoteSizes.push_back((int)kv.second.bins.size());
    }

    // ---- RMS per 1/16-beat slot: RMS of (local + all remotes sum) ----
    // Remote accum is at REMOTE_ACCUM_BINS (2048), local is at NUM_SYNC_BINS (4096).
    // Every other local bin is sampled — no peak-picking asymmetry since the wire
    // encoding is now lossless float.
    for (int s = 0; s < numRmsSlots; ++s) {
        const int wStart = s       * REMOTE_ACCUM_BINS / numRmsSlots;
        const int wEnd   = (s + 1) * REMOTE_ACCUM_BINS / numRmsSlots;
        const int wCount = wEnd - wStart;
        if (wCount <= 0) continue;

        double sumSqLocal = 0.0;
        double sumSqSum   = 0.0;

        for (int b = wStart; b < wEnd; ++b) {
            const int   lb = localBins > 0 ? b * localBins / REMOTE_ACCUM_BINS : 0;
            const float lv = (localBins > 0 && lb < localBins)
                                 ? m_localData[static_cast<size_t>(lb)] : 0.0f;
            sumSqLocal += lv * lv;

            float sumVal = lv;
            for (int ri = 0; ri < numRemotes; ++ri) {
                const float rv = (b < remoteSizes[ri]) ? remotePtrs[ri][b] : 0.0f;
                sumVal += rv;
            }
            sumSqSum += sumVal * sumVal;
        }

        m_rmsLocal[s] = std::sqrt((float)(sumSqLocal / wCount));
        m_rmsSum[s]   = std::sqrt((float)(sumSqSum   / wCount));
    }

    // ---- Cancellation: fine-grained windows (~4ms each at 4-beat display) ----
    if (doCancel) {
        std::vector<double> remSumSq(static_cast<size_t>(numRemotes), 0.0);

        for (int s = 0; s < MAX_CANCEL_SLOTS; ++s) {
            const int wStart = s       * REMOTE_ACCUM_BINS / MAX_CANCEL_SLOTS;
            const int wEnd   = (s + 1) * REMOTE_ACCUM_BINS / MAX_CANCEL_SLOTS;
            const int wCount = wEnd - wStart;
            if (wCount <= 0) continue;

            double sumSqLocal = 0.0;
            double sumSqSum   = 0.0;
            std::fill(remSumSq.begin(), remSumSq.end(), 0.0);

            for (int b = wStart; b < wEnd; ++b) {
                const int   lb = localBins > 0 ? b * localBins / REMOTE_ACCUM_BINS : 0;
                const float lv = (localBins > 0 && lb < localBins)
                                     ? m_localData[static_cast<size_t>(lb)] : 0.0f;
                sumSqLocal += lv * lv;

                float sumVal = lv;
                for (int ri = 0; ri < numRemotes; ++ri) {
                    const float rv = (b < remoteSizes[ri]) ? remotePtrs[ri][b] : 0.0f;
                    remSumSq[static_cast<size_t>(ri)] += rv * rv;
                    sumVal += rv;
                }
                sumSqSum += sumVal * sumVal;
            }

            double sumIndividualRms = std::sqrt(sumSqLocal / wCount);
            for (int ri = 0; ri < numRemotes; ++ri)
                sumIndividualRms += std::sqrt(remSumSq[static_cast<size_t>(ri)] / wCount);

            // Noise floor at ~-100 dBFS. Was 0.01 to mask 8-bit quantization DC offset.
            if (sumIndividualRms > 1e-4) {
                const float rmsSum = std::sqrt((float)(sumSqSum / wCount));
                const float ci = juce::jlimit(
                    0.0f, 1.0f, 1.0f - rmsSum / (float)sumIndividualRms);

                // Level-weighted CI: cancellation is only meaningful when signal is present.
                // Weight rises as sqrt(D / D_ref), clamped to 1 above D_ref = 0.1 (-20 dBFS).
                // At the noise floor (~0.01) weight ≈ 0.32; at -20 dBFS weight = 1.0.
                constexpr float D_REF = 0.1f;
                const float levelWeight = std::sqrt(
                    juce::jlimit(0.0f, 1.0f, (float)(sumIndividualRms / D_REF)));
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
