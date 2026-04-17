#include "ScopeDisplay.h"

// ============================================================================
// Construction
// ============================================================================

ScopeDisplay::ScopeDisplay() {
    setOpaque(true);
    // Slot 0 is always the local instance by default (index 1)
    m_instances[0].active     = true;
    m_instances[0].isLocal    = true;
    m_instances[0].instanceID = 0; // local has no remote ID
}

ScopeDisplay::~ScopeDisplay() {
    if (m_glRenderer)
        m_glRenderer->detach();
}

void ScopeDisplay::parentHierarchyChanged() {
    // Attempt to attach the OpenGL context once this component has a parent
    // (and therefore a valid native window handle).
    if (!m_glAttachAttempted && getParentComponent() != nullptr) {
        m_glAttachAttempted = true;
        m_glRenderer = std::make_unique<OpenGLScopeRenderer>();
        m_glRenderer->attachTo(*this);
    }
}

bool ScopeDisplay::isOpenGLActive() const {
    return m_glRenderer && m_glRenderer->isAvailable();
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

void ScopeDisplay::applyFilterAndWriteBatch(InstanceSlot& inst,
                                             const float* samples, int numSamples,
                                             double ppqOfFirstSample, double ppqPerSample) {
    const int N = inst.buffer.size();
    if (N <= 0 || numSamples <= 0) return;

    // Clamp incoming batch to the ring capacity so we never overwrite a position
    // more than once within a single call.  Any excess samples would just
    // overwrite earlier ones in this same batch — not useful.
    const int count = std::min(numSamples, N);

    // Compute the start index once from the first sample's PPQ.
    const int startIdx = inst.buffer.indexForPpq(ppqOfFirstSample);

    // Handle display-cycle boundary: reset filter state if the first sample of
    // this batch belongs to a different display window than the previous batch.
    if (m_displayRangeBeats > 0.0) {
        const double windowStart =
            std::floor(ppqOfFirstSample / m_displayRangeBeats) * m_displayRangeBeats;
        if (std::abs(windowStart - inst.lastWindowStart) > 1e-9) {
            inst.filterHP.reset();
            inst.filterLP.reset();
            inst.lastWindowStart = windowStart;
        }
        // If the batch itself crosses a display-window boundary, also detect and
        // reset mid-batch.  Compute the PPQ of the last sample to check.
        if (count > 1 && ppqPerSample > 0.0) {
            const double lastPpq = ppqOfFirstSample + static_cast<double>(count - 1) * ppqPerSample;
            const double lastWindowStart =
                std::floor(lastPpq / m_displayRangeBeats) * m_displayRangeBeats;
            if (std::abs(lastWindowStart - windowStart) > 1e-9) {
                // Find the crossing point and reset there; process the first
                // sub-range with current filter state, reset, then continue.
                // For simplicity (rare event: once per display window), find
                // the exact crossing sample index.
                const double nextWindowStart = windowStart + m_displayRangeBeats;
                // samples_until_wrap = ceil((nextWindowStart - ppqOfFirstSample) / ppqPerSample)
                int crossIdx = static_cast<int>(std::ceil(
                    (nextWindowStart - ppqOfFirstSample) / ppqPerSample));
                crossIdx = std::max(1, std::min(crossIdx, count - 1));

                // Phase A: samples[0 .. crossIdx-1] — before the boundary
                const int countA = std::min(crossIdx, std::min(count, N - startIdx));
                for (int i = 0; i < countA; ++i) {
                    float v = samples[i];
                    if (m_hpEnabled) v = inst.filterHP.processSample(v);
                    if (m_lpEnabled) v = inst.filterLP.processSample(v);
                    inst.buffer.writeAt(startIdx + i, v);
                }
                // Wrap phase A if needed
                for (int i = countA; i < crossIdx; ++i) {
                    float v = samples[i];
                    if (m_hpEnabled) v = inst.filterHP.processSample(v);
                    if (m_lpEnabled) v = inst.filterLP.processSample(v);
                    inst.buffer.writeAt(i - (N - startIdx), v);
                }

                // Reset at the boundary
                inst.filterHP.reset();
                inst.filterLP.reset();
                inst.lastWindowStart = lastWindowStart;

                // Phase B: samples[crossIdx .. count-1] — after the boundary
                for (int i = crossIdx; i < count; ++i) {
                    const int idx = (startIdx + i) % N;
                    float v = samples[i];
                    if (m_hpEnabled) v = inst.filterHP.processSample(v);
                    if (m_lpEnabled) v = inst.filterLP.processSample(v);
                    inst.buffer.writeAt(idx, v);
                }

                // Dirty mark: wrap-aware (startIdx, lastIdx)
                const int lastIdx = (startIdx + count - 1) % N;
                inst.rmsBuckets.markDirtyRange(startIdx, lastIdx);
                inst.cancelBuckets.markDirtyRange(startIdx, lastIdx);
                m_frameHasNewData = true;
                return;
            }
        }
    }

    // Fast path — no display-window boundary within this batch.
    // Phase A: samples[0 .. countA-1] fill indices [startIdx .. min(startIdx+count-1, N-1)]
    const int countA = std::min(count, N - startIdx);
    for (int i = 0; i < countA; ++i) {
        float v = samples[i];
        if (m_hpEnabled) v = inst.filterHP.processSample(v);
        if (m_lpEnabled) v = inst.filterLP.processSample(v);
        inst.buffer.writeAt(startIdx + i, v);
    }
    // Phase B (ring wrap): remaining samples fill indices [0 .. countB-1]
    const int countB = count - countA;
    for (int i = 0; i < countB; ++i) {
        float v = samples[countA + i];
        if (m_hpEnabled) v = inst.filterHP.processSample(v);
        if (m_lpEnabled) v = inst.filterLP.processSample(v);
        inst.buffer.writeAt(i, v);
    }

    // Single dirty-range call covering the whole batch.
    const int lastIdx = (startIdx + count - 1) % N;
    inst.rmsBuckets.markDirtyRange(startIdx, lastIdx);
    inst.cancelBuckets.markDirtyRange(startIdx, lastIdx);
    m_frameHasNewData = true;
}

void ScopeDisplay::writeLocalSample(float sample, double ppq) {
    const double ppqPerSample = (m_bpm > 0.0 && m_sampleRate > 0.0)
                                    ? m_bpm / (60.0 * m_sampleRate)
                                    : 0.0;
    applyFilterAndWriteBatch(m_instances[m_localInstanceIndex - 1], &sample, 1, ppq, ppqPerSample);
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

    // Deactivate slots for instances no longer in the packet set.
    // Skip when packets is empty (receive toggled off) — clearRemoteInstances() handles that.
    if (!packets.empty()) {
        for (int i = 0; i < MAX_INSTANCES; ++i) {
            if (m_instances[i].isLocal || !m_instances[i].active) continue;
            bool found = false;
            for (const auto& pkt : packets)
                if (pkt.instanceID == m_instances[i].instanceID) { found = true; break; }
            if (!found) {
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

        // Determine slot index directly from the sender's instanceIndex.
        // instanceIndex is in [1, 8] on the wire; clamp to valid range just in case.
        const int slotIdx = juce::jlimit(0, MAX_INSTANCES - 1,
                                         static_cast<int>(pkt.instanceIndex) - 1);

        // Local always wins: skip remote packets that claim the local slot
        if (slotIdx == m_localInstanceIndex - 1) continue;

        auto& inst = m_instances[slotIdx];

        // Activate slot on first packet from this instance (or if instanceID changed)
        if (!inst.active || inst.instanceID != pkt.instanceID) {
            inst.active     = true;
            inst.isLocal    = false;
            inst.instanceID = pkt.instanceID;
            inst.hasSeq     = false;
            prepareInstance(inst);
        }

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

        applyFilterAndWriteBatch(inst, pkt.samples, static_cast<int>(pkt.numSamples),
                                 pkt.ppqOfFirstSample, ppqPerSample);
    }

    m_rmsOverlayDirty    = true;
    m_cancelOverlayDirty = true;
}

void ScopeDisplay::clearRemoteInstances() {
    for (int i = 0; i < MAX_INSTANCES; ++i) {
        if (m_instances[i].isLocal) continue;
        m_instances[i].active     = false;
        m_instances[i].instanceID = 0;
        m_instances[i].hasSeq     = false;
        m_instances[i].displayBins.assign(DISPLAY_BINS, 0.0f);
        m_instances[i].rmsValues.clear();
    }
    m_remoteInfoMap.clear();
    m_rmsOverlayDirty    = true;
    m_cancelOverlayDirty = true;
    m_frameHasNewData    = true;
}

void ScopeDisplay::setLocalInstanceIndex(int newIndex) {
    const int clamped = juce::jlimit(1, MAX_INSTANCES, newIndex);
    if (clamped == m_localInstanceIndex) return;

    // Deactivate old local slot
    const int oldSlot = m_localInstanceIndex - 1;
    m_instances[oldSlot].isLocal     = false;
    m_instances[oldSlot].active      = false;
    m_instances[oldSlot].instanceID  = 0;
    m_instances[oldSlot].hasSeq      = false;
    m_instances[oldSlot].displayBins.assign(DISPLAY_BINS, 0.0f);
    m_instances[oldSlot].rmsValues.clear();

    // Activate new local slot
    const int newSlot = clamped - 1;
    m_instances[newSlot].isLocal     = true;
    m_instances[newSlot].active      = true;
    m_instances[newSlot].instanceID  = 0;
    m_instances[newSlot].hasSeq      = false;
    prepareInstance(m_instances[newSlot]);

    m_localInstanceIndex = clamped;
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

    // Push frame data to the OpenGL renderer if active
    if (isOpenGLActive())
        updateGLFrameData();
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
            if (inst.isLocal) m_rmsLocal[s] = rms;
            sum += rms;
        }
        m_rmsSum[s] = sum;
    }
}

void ScopeDisplay::recomputeCancellation() {
    m_numActiveCancelBuckets = 0;

    // Need at least 2 active instances for cancellation to be meaningful.
    // When fewer are active, reset the display to all-green (zero) so stale
    // values from a previous session don't linger after remote is toggled off.
    int numActive = 0;
    for (const auto& inst : m_instances)
        if (inst.active) ++numActive;
    if (numActive < 2) {
        std::fill(std::begin(m_cancellationIndex), std::end(m_cancellationIndex), 0.0f);
        return;
    }

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
            // Below noise floor — mark clean, zero this slot, and skip
            m_cancellationIndex[bi] = 0.0f;
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

    const bool glActive = isOpenGLActive();

    // When OpenGL is active, it renders the background, grid, waveforms, and
    // playhead via the GL pipeline.  We only need to draw overlays (RMS,
    // cancellation), remote labels, broadcast-only overlay, and the border
    // using JUCE's software Graphics context.
    if (!glActive) {
        // Software fallback: full paint
        g.fillAll(juce::Colour(0xFF1A1A2E));
        drawGrid(g, bounds);
    }

    // Determine whether any remote instance is active
    bool hasActiveRemotes = false;
    const int localSlot = m_localInstanceIndex - 1;
    for (int i = 0; i < MAX_INSTANCES; ++i)
        if (i != localSlot && m_instances[i].active) { hasActiveRemotes = true; break; }

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
        for (int i = 0; i < MAX_INSTANCES; ++i) {
            const auto& inst = m_instances[i];
            if (inst.isLocal) continue;
            if (!inst.active || inst.displayBins.empty()) continue;

            juce::Colour colour;
            juce::String label;
            auto infoIt = m_remoteInfoMap.find(inst.instanceID);
            if (infoIt != m_remoteInfoMap.end()) {
                const auto& info = infoIt->second;
                colour = juce::Colour(info.colourRGBA[0], info.colourRGBA[1],
                                      info.colourRGBA[2], info.colourRGBA[3]);
                label  = juce::String(info.channelLabel);
            } else {
                colour = getRemoteColour(i);
            }

            // Software waveform drawing (skipped when OpenGL is active)
            if (!glActive) {
                drawWaveform(g, bounds, inst.displayBins.data(),
                             static_cast<int>(inst.displayBins.size()),
                             colour, 0.5f);
            }

            // Labels are always drawn via software (text rendering)
            if (label.isNotEmpty()) {
                g.setColour(colour.withAlpha(0.85f));
                g.setFont(juce::Font(juce::FontOptions(11.0f)));
                g.drawText(label,
                           static_cast<int>(bounds.getX() + 4),
                           static_cast<int>(bounds.getY() + 4 + i * 14),
                           150, 13,
                           juce::Justification::centredLeft, true);
            }
        }
    }

    // Local waveform (on top) — software only
    if (!glActive) {
        const auto& localInst = m_instances[localSlot];
        if (m_showLocal && !localInst.displayBins.empty()) {
            drawWaveform(g, bounds,
                         localInst.displayBins.data(),
                         static_cast<int>(localInst.displayBins.size()),
                         m_localColour, 1.0f);
        }
    }

    // Playhead — software only
    if (!glActive && !m_broadcastOnlyOverlayEnabled)
        drawPlayhead(g, bounds);

    if (m_broadcastOnlyOverlayEnabled) {
        g.setColour(juce::Colours::black.withAlpha(0.58f));
        g.fillRect(bounds);

        g.setColour(juce::Colours::white.withAlpha(0.92f));
        g.setFont(juce::Font(juce::FontOptions(22.0f)).boldened());
        g.drawText("BROADCAST MODE",
                   bounds.toNearestInt(),
                   juce::Justification::centred,
                   true);
    }

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

    // When remote display is off, show only local RMS.
    // When remote is on and there are active remotes, show the combined sum.
    bool hasActiveRemotes = false;
    for (int i = 1; i < MAX_INSTANCES; ++i)
        if (m_instances[i].active) { hasActiveRemotes = true; break; }
    const bool useSum = m_showRemote && hasActiveRemotes;

    // Find the reference instance so we can map bucket index-ranges to pixel
    // positions exactly as RawSampleBuffer does (startIdx/N → beat fraction).
    // This avoids the off-by-fractional-bucket alignment that occurs when
    // integer truncation in computeBucketSize() creates a small leftover bucket.
    const InstanceSlot* refInst = nullptr;
    for (const auto& inst : m_instances) {
        if (inst.active && inst.rmsBuckets.bucketCount() > 0) {
            refInst = &inst;
            break;
        }
    }
    const int N = refInst ? refInst->buffer.size() : 0;
    const float fallbackSlotW = area.getWidth() / static_cast<float>(numSlots);

    for (int s = 0; s < numSlots && s < MAX_METRIC_SLOTS; ++s) {
        const float rms = useSum ? m_rmsSum[s] : m_rmsLocal[s];
        if (rms < 1e-6f) continue;

        // Map bucket to screen x using its actual buffer-index range so that
        // slot boundaries align with the beat-position grid lines.
        float x0, slotW;
        if (refInst && N > 0 && s < refInst->rmsBuckets.bucketCount()) {
            const auto& b = refInst->rmsBuckets.bucket(s);
            x0    = area.getX() + (static_cast<float>(b.startIdx) / static_cast<float>(N)) * area.getWidth();
            slotW = (static_cast<float>(b.endIdx - b.startIdx) / static_cast<float>(N)) * area.getWidth();
        } else {
            x0    = area.getX() + static_cast<float>(s) * fallbackSlotW;
            slotW = fallbackSlotW;
        }

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

void ScopeDisplay::updateGLFrameData() {
    if (!isOpenGLActive()) return;

    std::array<OpenGLScopeRenderer::WaveformInstance, MAX_INSTANCES> glInstances;
    const int localSlot = m_localInstanceIndex - 1;

    for (int i = 0; i < MAX_INSTANCES; ++i) {
        const auto& inst = m_instances[i];
        auto& glInst = glInstances[static_cast<size_t>(i)];
        glInst.active  = inst.active;
        glInst.isLocal = inst.isLocal;

        if (inst.isLocal) {
            glInst.colour = m_localColour;
            glInst.alpha  = 1.0f;
        } else {
            auto infoIt = m_remoteInfoMap.find(inst.instanceID);
            if (infoIt != m_remoteInfoMap.end()) {
                const auto& info = infoIt->second;
                glInst.colour = juce::Colour(info.colourRGBA[0], info.colourRGBA[1],
                                              info.colourRGBA[2], info.colourRGBA[3]);
            } else {
                glInst.colour = getRemoteColour(i);
            }
            glInst.alpha = 0.5f;
        }

        if (inst.active && !inst.displayBins.empty())
            glInst.bins = inst.displayBins.data();
    }

    m_glRenderer->setFrameData(glInstances,
                               m_amplitudeScale,
                               m_displayRangeBeats,
                               m_currentPpq,
                               m_showLocal,
                               m_showRemote,
                               m_broadcastOnlyOverlayEnabled,
                               localSlot);
}

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
