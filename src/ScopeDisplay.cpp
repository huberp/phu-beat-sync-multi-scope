#include "ScopeDisplay.h"

// ============================================================================
// Construction
// ============================================================================

ScopeDisplay::ScopeDisplay() {
    setOpaque(true);
    // Slot 0 is always the local instance
    m_instances[0].active     = true;
    m_instances[0].instanceID = 0; // local has no remote ID
}

// ============================================================================
// Configuration
// ============================================================================

void ScopeDisplay::prepare(double displayBeats, double bpm, double sampleRate) {
    m_displayRangeBeats = (displayBeats > 0.0) ? displayBeats : 1.0;
    m_bpm               = (bpm         > 0.0) ? bpm          : 120.0;
    m_sampleRate        = (sampleRate  > 0.0) ? sampleRate   : 44100.0;

    for (auto& inst : m_instances) {
        if (inst.active)
            prepareInstance(inst);
    }

    m_rmsOverlayDirty    = true;
    m_cancelOverlayDirty = true;
}

void ScopeDisplay::prepareInstance(InstanceSlot& inst) {
    inst.buffer.prepare(m_displayRangeBeats, m_bpm, m_sampleRate);
    const int N = inst.buffer.size();
    inst.rmsBuckets.recompute(m_bpm, m_sampleRate, m_displayRangeBeats, N);
    inst.cancelBuckets.recompute(m_bpm, m_sampleRate, m_displayRangeBeats, N);
    inst.rmsValues.assign(static_cast<size_t>(inst.rmsBuckets.bucketCount()), 0.0f);
    inst.displayBins.assign(DISPLAY_BINS, 0.0f);
    inst.lastWindowStart = -1e18;
    updateInstanceFilter(inst);
}

void ScopeDisplay::updateInstanceFilter(InstanceSlot& inst) {
    if (m_sampleRate <= 0.0) return;
    inst.filterHP.setParams(LinkwitzRiley::FilterType::HighPass,
                            LinkwitzRiley::Slope::DB48,
                            m_hpFreq,
                            static_cast<float>(m_sampleRate));
    inst.filterHP.reset();
    inst.filterLP.setParams(LinkwitzRiley::FilterType::LowPass,
                            LinkwitzRiley::Slope::DB48,
                            m_lpFreq,
                            static_cast<float>(m_sampleRate));
    inst.filterLP.reset();
}

void ScopeDisplay::setFilterParams(bool hpEnabled, float hpFreq,
                                    bool lpEnabled, float lpFreq,
                                    double sampleRate) {
    m_hpEnabled = hpEnabled;
    m_hpFreq    = hpFreq;
    m_lpEnabled = lpEnabled;
    m_lpFreq    = lpFreq;
    m_sampleRate = (sampleRate > 0.0) ? sampleRate : m_sampleRate;

    for (auto& inst : m_instances) {
        if (inst.active)
            updateInstanceFilter(inst);
    }

    m_rmsOverlayDirty    = true;
    m_cancelOverlayDirty = true;
}

// ============================================================================
// Data Ingestion
// ============================================================================

void ScopeDisplay::applyFilterAndWrite(InstanceSlot& inst, float sample, double ppq) {
    if (inst.buffer.size() <= 0) return;

    // Detect display-cycle boundary to reset filter state on PPQ wrap
    if (m_displayRangeBeats > 0.0) {
        const double windowStart =
            std::floor(ppq / m_displayRangeBeats) * m_displayRangeBeats;
        if (std::abs(windowStart - inst.lastWindowStart) > 1e-9) {
            inst.filterHP.reset();
            inst.filterLP.reset();
            inst.lastWindowStart = windowStart;
        }
    }

    // Apply HP/LP display filter per-sample
    float filtered = sample;
    if (m_hpEnabled) filtered = inst.filterHP.processSample(filtered);
    if (m_lpEnabled) filtered = inst.filterLP.processSample(filtered);

    // Write at PPQ-mapped position and mark affected buckets dirty
    const auto range = inst.buffer.write(filtered, ppq);
    if (range.from < range.to) {
        inst.rmsBuckets.markDirty(range.from, range.to);
        inst.cancelBuckets.markDirty(range.from, range.to);
    }
}

void ScopeDisplay::writeLocalSample(float sample, double ppq) {
    applyFilterAndWrite(m_instances[0], sample, ppq);
    m_rmsOverlayDirty    = true;
    m_cancelOverlayDirty = true;
}

void ScopeDisplay::writeRemotePackets(
    const std::vector<SampleBroadcaster::RemoteRawPacket>& packets,
    const std::vector<phu::network::RemoteInstanceInfo>&   infos)
{
    // Update identity info map
    m_remoteInfoMap.clear();
    for (const auto& info : infos)
        m_remoteInfoMap[info.instanceID] = info;

    // Deactivate slots for instances that are no longer in the packet set.
    // Skip when packets is empty (receive toggled off) — clearRemoteInstances() handles that.
    if (!packets.empty()) {
        for (int i = 1; i < MAX_INSTANCES; ++i) {
            if (!m_instances[i].active) continue;
            bool found = false;
            for (const auto& pkt : packets)
                if (pkt.instanceID == m_instances[i].instanceID) { found = true; break; }
            if (!found) {
                m_remoteSlotMap.erase(m_instances[i].instanceID);
                m_instances[i].active     = false;
                m_instances[i].instanceID = 0;
                m_instances[i].hasSeq     = false;
                m_instances[i].displayBins.assign(DISPLAY_BINS, 0.0f);
                m_instances[i].rmsValues.clear();
            }
        }
    }

    // Process each packet
    for (const auto& pkt : packets) {
        if (pkt.numSamples == 0) continue;

        // Find or allocate a slot
        int slotIdx = -1;
        auto it = m_remoteSlotMap.find(pkt.instanceID);
        if (it != m_remoteSlotMap.end()) {
            slotIdx = it->second;
        } else {
            for (int i = 1; i < MAX_INSTANCES; ++i) {
                if (!m_instances[i].active) {
                    slotIdx = i;
                    m_instances[i].active     = true;
                    m_instances[i].instanceID = pkt.instanceID;
                    m_instances[i].hasSeq     = false;
                    m_remoteSlotMap[pkt.instanceID] = i;
                    prepareInstance(m_instances[i]);
                    break;
                }
            }
            if (slotIdx == -1) continue; // Max instances reached; discard
        }

        auto& inst = m_instances[slotIdx];

        // Sequence deduplication: skip packets we have already processed
        if (inst.hasSeq && pkt.sequenceNumber == inst.lastSeqNum) continue;
        inst.lastSeqNum = pkt.sequenceNumber;
        inst.hasSeq     = true;

        // Reconstruct per-sample PPQ using sender's BPM and (preferably) sender's sample rate
        const double bpm = pkt.bpm > 0.0 ? pkt.bpm : 120.0;
        double remoteSr  = m_sampleRate > 0.0 ? m_sampleRate : 44100.0;
        {
            auto infoIt = m_remoteInfoMap.find(pkt.instanceID);
            if (infoIt != m_remoteInfoMap.end() && infoIt->second.sampleRate > 0.0)
                remoteSr = infoIt->second.sampleRate;
        }
        const double ppqPerSample = bpm / (60.0 * remoteSr);

        for (int i = 0; i < static_cast<int>(pkt.numSamples); ++i) {
            const double ppq = pkt.ppqOfFirstSample + static_cast<double>(i) * ppqPerSample;
            applyFilterAndWrite(inst, pkt.samples[i], ppq);
        }
    }

    m_rmsOverlayDirty    = true;
    m_cancelOverlayDirty = true;
}

void ScopeDisplay::clearRemoteInstances() {
    for (int i = 1; i < MAX_INSTANCES; ++i) {
        m_instances[i].active     = false;
        m_instances[i].instanceID = 0;
        m_instances[i].hasSeq     = false;
        m_instances[i].displayBins.assign(DISPLAY_BINS, 0.0f);
        m_instances[i].rmsValues.clear();
    }
    m_remoteSlotMap.clear();
    m_remoteInfoMap.clear();
    m_rmsOverlayDirty    = true;
    m_cancelOverlayDirty = true;
}

// ============================================================================
// Frame Computation
// ============================================================================

void ScopeDisplay::computeFrame() {
    for (auto& inst : m_instances)
        if (inst.active)
            scatterInstance(inst);
    if (m_showRms)
        recomputeRms();
    if (m_showCancellation)
        recomputeCancellation();
}

void ScopeDisplay::scatterInstance(InstanceSlot& inst) {
    const int N = inst.buffer.size();
    if (N <= 0) return;
    if (static_cast<int>(inst.displayBins.size()) != DISPLAY_BINS)
        inst.displayBins.assign(DISPLAY_BINS, 0.0f);

    const float* raw         = inst.buffer.data();
    const int    numBuckets  = inst.rmsBuckets.bucketCount();

    if (numBuckets == 0) {
        // No buckets configured — full scatter as fallback
        for (int i = 0; i < N; ++i) {
            const int bin = i * DISPLAY_BINS / N;
            if (bin >= 0 && bin < DISPLAY_BINS)
                inst.displayBins[static_cast<size_t>(bin)] = raw[i];
        }
        return;
    }

    // Only scatter samples that fall within dirty RMS buckets.
    // Dirty flags are read here and cleared later by recomputeRms().
    for (int bi = 0; bi < numBuckets; ++bi) {
        const auto& b = inst.rmsBuckets.bucket(bi);
        if (!b.dirty) continue;

        const int start = b.startIdx;
        const int end   = juce::jmin(b.endIdx, N);
        for (int i = start; i < end; ++i) {
            const int bin = i * DISPLAY_BINS / N;
            if (bin >= 0 && bin < DISPLAY_BINS)
                inst.displayBins[static_cast<size_t>(bin)] = raw[i];
        }
    }
}

void ScopeDisplay::recomputeRms() {
    std::fill(std::begin(m_rmsLocal), std::end(m_rmsLocal), 0.0f);
    std::fill(std::begin(m_rmsSum),   std::end(m_rmsSum),   0.0f);
    m_numActiveRmsBuckets = 0;

    // Determine bucket count from the first active instance
    for (const auto& inst : m_instances) {
        if (inst.active && inst.rmsBuckets.bucketCount() > 0) {
            m_numActiveRmsBuckets = inst.rmsBuckets.bucketCount();
            break;
        }
    }
    if (m_numActiveRmsBuckets == 0) return;

    // Recompute dirty RMS buckets for each instance independently
    for (auto& inst : m_instances) {
        if (!inst.active) continue;
        const int numBuckets = inst.rmsBuckets.bucketCount();
        if (numBuckets == 0) continue;

        if (static_cast<int>(inst.rmsValues.size()) != numBuckets)
            inst.rmsValues.assign(static_cast<size_t>(numBuckets), 0.0f);

        const float* raw = inst.buffer.data();
        const int    N   = inst.buffer.size();

        for (int bi = 0; bi < numBuckets; ++bi) {
            auto& b = inst.rmsBuckets.bucket(bi);
            if (!b.dirty) continue;

            const int start = b.startIdx;
            const int end   = juce::jmin(b.endIdx, N);
            const int count = end - start;
            if (count <= 0) { b.dirty = false; continue; }

            float sumSq = 0.0f;
            for (int i = start; i < end; ++i)
                sumSq += raw[i] * raw[i];
            inst.rmsValues[static_cast<size_t>(bi)] =
                std::sqrt(sumSq / static_cast<float>(count));
            b.dirty = false;
        }
    }

    // Sum per-instance RMS values into m_rmsLocal and m_rmsSum
    const int numOut = juce::jmin(m_numActiveRmsBuckets, MAX_METRIC_SLOTS);
    for (int s = 0; s < numOut; ++s) {
        float sum = 0.0f;
        for (int ii = 0; ii < MAX_INSTANCES; ++ii) {
            const auto& inst = m_instances[ii];
            if (!inst.active) continue;
            if (s >= static_cast<int>(inst.rmsValues.size())) continue;
            const float rms = inst.rmsValues[static_cast<size_t>(s)];
            if (ii == 0) m_rmsLocal[s] = rms;
            sum += rms;
        }
        m_rmsSum[s] = sum;
    }
}

void ScopeDisplay::recomputeCancellation() {
    std::fill(std::begin(m_cancellationIndex), std::end(m_cancellationIndex), 0.0f);
    m_numActiveCancelBuckets = 0;

    // Need at least 2 active instances for cancellation to be meaningful
    int numActive = 0;
    for (const auto& inst : m_instances)
        if (inst.active) ++numActive;
    if (numActive < 2) return;

    // Get bucket count from the first active instance
    int numBuckets = 0;
    const InstanceSlot* refInst = nullptr;
    for (const auto& inst : m_instances) {
        if (inst.active && inst.cancelBuckets.bucketCount() > 0) {
            numBuckets = inst.cancelBuckets.bucketCount();
            refInst    = &inst;
            break;
        }
    }
    if (numBuckets == 0 || refInst == nullptr) return;
    m_numActiveCancelBuckets = juce::jmin(numBuckets, MAX_CANCEL_SLOTS);

    for (int bi = 0; bi < m_numActiveCancelBuckets; ++bi) {
        // Check whether any active instance has this bucket dirty
        bool dirty = false;
        for (const auto& inst : m_instances) {
            if (inst.active && bi < inst.cancelBuckets.bucketCount() &&
                inst.cancelBuckets.bucket(bi).dirty) {
                dirty = true;
                break;
            }
        }
        if (!dirty) continue;

        const auto& refBucket = refInst->cancelBuckets.bucket(bi);
        const int start = refBucket.startIdx;
        const int end   = refBucket.endIdx;
        const int count = end - start;
        if (count <= 0) continue;

        // Clamp loop end to the minimum buffer size across all active instances.
        // All instances are normally prepared with the same parameters (same N),
        // but a newly activated remote instance may have a slightly different size
        // if BPM or sample rate changed between prepare() calls.
        int safeEnd = end;
        for (const auto& inst : m_instances)
            if (inst.active)
                safeEnd = juce::jmin(safeEnd, inst.buffer.size());
        if (safeEnd <= start) continue;
        const int safeCount = safeEnd - start;

        // Denominator: sum of individual per-instance RMSes over this range
        float D = 0.0f;
        for (const auto& inst : m_instances) {
            if (!inst.active) continue;
            const float* raw = inst.buffer.data();
            float sumSq = 0.0f;
            for (int i = start; i < safeEnd; ++i)
                sumSq += raw[i] * raw[i];
            D += std::sqrt(sumSq / static_cast<float>(safeCount));
        }

        if (D <= 1e-4f) {
            // Below noise floor — mark clean and skip
            for (auto& inst : m_instances)
                if (inst.active && bi < inst.cancelBuckets.bucketCount())
                    inst.cancelBuckets.bucket(bi).dirty = false;
            continue;
        }

        // Numerator: RMS of the summed waveform over the same range
        float sumSqSum = 0.0f;
        for (int i = start; i < safeEnd; ++i) {
            float v = 0.0f;
            for (const auto& inst : m_instances) {
                if (!inst.active) continue;
                v += inst.buffer.data()[i];
            }
            sumSqSum += v * v;
        }
        const float N_val = std::sqrt(sumSqSum / static_cast<float>(safeCount));

        const float ci = juce::jlimit(0.0f, 1.0f, 1.0f - N_val / D);
        constexpr float D_REF = 0.1f;
        const float levelWeight = std::sqrt(juce::jlimit(0.0f, 1.0f, D / D_REF));
        m_cancellationIndex[bi] = ci * levelWeight;

        // Mark all instances' cancel bucket clean
        for (auto& inst : m_instances)
            if (inst.active && bi < inst.cancelBuckets.bucketCount())
                inst.cancelBuckets.bucket(bi).dirty = false;
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

    // Determine whether any remote instance is active
    bool hasActiveRemotes = false;
    for (int i = 1; i < MAX_INSTANCES; ++i)
        if (m_instances[i].active) { hasActiveRemotes = true; break; }

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

    if (m_showCancellation && hasActiveRemotes) {
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
    if (m_showRemote && hasActiveRemotes) {
        int colourIdx = 0;
        for (int i = 1; i < MAX_INSTANCES; ++i) {
            const auto& inst = m_instances[i];
            if (!inst.active || inst.displayBins.empty()) { ++colourIdx; continue; }

            juce::Colour colour;
            juce::String label;
            auto infoIt = m_remoteInfoMap.find(inst.instanceID);
            if (infoIt != m_remoteInfoMap.end()) {
                const auto& info = infoIt->second;
                colour = juce::Colour(info.colourRGBA[0], info.colourRGBA[1],
                                      info.colourRGBA[2], info.colourRGBA[3]);
                label  = juce::String(info.channelLabel);
            } else {
                colour = getRemoteColour(colourIdx);
            }

            drawWaveform(g, bounds, inst.displayBins.data(),
                         static_cast<int>(inst.displayBins.size()),
                         colour, 0.5f);

            if (label.isNotEmpty()) {
                g.setColour(colour.withAlpha(0.85f));
                g.setFont(juce::Font(juce::FontOptions(11.0f)));
                g.drawText(label,
                           static_cast<int>(bounds.getX() + 4),
                           static_cast<int>(bounds.getY() + 4 + colourIdx * 14),
                           150, 13,
                           juce::Justification::centredLeft, true);
            }
            ++colourIdx;
        }
    }

    // Local waveform (on top)
    if (m_showLocal && !m_instances[0].displayBins.empty()) {
        drawWaveform(g, bounds,
                     m_instances[0].displayBins.data(),
                     static_cast<int>(m_instances[0].displayBins.size()),
                     m_localColour, 1.0f);
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

    const int screenW = juce::jmax(2, static_cast<int>(area.getWidth()));
    juce::Path path;
    bool started = false;

    for (int px = 0; px < screenW; ++px) {
        const float x = area.getX() + static_cast<float>(px);

        const int binStart = px       * numBins / screenW;
        const int binEnd   = (px + 1) * numBins / screenW;

        if (binStart >= numBins) break;

        if (binEnd <= binStart + 1) {
            const float y = sampleToY(data[binStart], area.getY(), area.getHeight());
            if (!started) { path.startNewSubPath(x, y); started = true; }
            else            path.lineTo(x, y);
        } else {
            float minV = data[binStart], maxV = data[binStart];
            for (int b = binStart + 1; b < binEnd && b < numBins; ++b) {
                if (data[b] < minV) minV = data[b];
                if (data[b] > maxV) maxV = data[b];
            }
            const float yMax = sampleToY(maxV, area.getY(), area.getHeight());
            const float yMin = sampleToY(minV, area.getY(), area.getHeight());

            if (!started) { path.startNewSubPath(x, yMax); started = true; }
            else            path.lineTo(x, yMax);
            path.lineTo(x, yMin);
        }
    }

    g.setColour(colour.withAlpha(alpha));
    g.strokePath(path, juce::PathStrokeType(1.5f));
}

// ============================================================================
// RMS Overlay
// ============================================================================

void ScopeDisplay::drawRmsOverlay(juce::Graphics& g, juce::Rectangle<float> area) {
    const int numSlots = juce::jmax(1, m_numActiveRmsBuckets);
    const float slotW  = area.getWidth() / static_cast<float>(numSlots);

    // When remote display is off, show only local RMS.
    // When remote is on and there are active remotes, show the combined sum.
    bool hasActiveRemotes = false;
    for (int i = 1; i < MAX_INSTANCES; ++i)
        if (m_instances[i].active) { hasActiveRemotes = true; break; }
    const bool useSum = m_showRemote && hasActiveRemotes;

    for (int s = 0; s < numSlots && s < MAX_METRIC_SLOTS; ++s) {
        const float rms = useSum ? m_rmsSum[s] : m_rmsLocal[s];
        if (rms < 1e-6f) continue;

        const float x0    = area.getX() + static_cast<float>(s) * slotW;
        const float lineY = sampleToY(rms, area.getY(), area.getHeight());

        g.setColour(juce::Colour(0x1A44AAFF));
        g.fillRect(x0, lineY - 2.5f, slotW, 5.0f);

        g.setColour(juce::Colour(0x5566CCFF));
        g.fillRect(x0, lineY - 1.5f, slotW, 3.0f);

        g.setColour(juce::Colour(0xEEAAEEFF));
        g.fillRect(x0, lineY - 0.5f, slotW, 2.0f);
    }
}

// ============================================================================
// Cancellation Overlay
// ============================================================================

void ScopeDisplay::drawCancellationOverlay(juce::Graphics& g, juce::Rectangle<float> area) {
    const int   numSlots = juce::jmax(1, m_numActiveCancelBuckets);
    const float slotW    = area.getWidth() / static_cast<float>(numSlots);
    constexpr float barH = 6.0f;
    const float barY     = area.getBottom() - barH;

    for (int s = 0; s < numSlots && s < MAX_CANCEL_SLOTS; ++s) {
        const float ci = m_cancellationIndex[s];

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
    const float scaled = juce::jlimit(-1.0f, 1.0f, sample * m_amplitudeScale);
    float normalized = (scaled + 1.0f) * 0.5f;
    normalized = juce::jlimit(0.0f, 1.0f, normalized);
    return top + (1.0f - normalized) * height;
}

juce::Colour ScopeDisplay::getRemoteColour(int index) {
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
