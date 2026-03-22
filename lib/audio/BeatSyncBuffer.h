#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace phu {
namespace audio {

/**
 * BeatSyncBuffer — position-indexed display buffer for beat-synced rendering.
 *
 * Each bin corresponds to a fractional position in a musical range [0, 1).
 * Audio-thread writes via write(); UI-thread reads via data()/getBin().
 *
 * Thread safety: single float stores/loads are naturally atomic on x86/x64.
 * Worst case the UI reads a slightly stale value — sub-pixel visual difference.
 */
class BeatSyncBuffer {
  public:
    BeatSyncBuffer() = default;

    /** Allocate bins. Call from prepareToPlay (not real-time). */
    void prepare(int numBins, float clearValue = -60.0f) {
        m_bins.resize(static_cast<size_t>(numBins), clearValue);
        m_numBins = numBins;
        clear(clearValue);
    }

    /** Write a value at a normalised position [0, 1). O(1). Audio-thread safe. */
    void write(double normalizedPos, float value) {
        if (m_numBins <= 0) return;
        int idx = static_cast<int>(normalizedPos * m_numBins);
        if (idx < 0) idx = 0;
        if (idx >= m_numBins) idx = m_numBins - 1;
        m_bins[static_cast<size_t>(idx)] = value;
    }

    /** Reset all bins to a given value. Not real-time safe. */
    void clear(float value = -60.0f) {
        std::fill(m_bins.begin(), m_bins.end(), value);
    }

    /** Direct read access for rendering. */
    const float* data() const { return m_bins.data(); }

    /** Number of bins. */
    int size() const { return m_numBins; }

    /** Bounds-checked single-bin read. */
    float getBin(int index) const {
        if (index < 0 || index >= m_numBins) return -60.0f;
        return m_bins[static_cast<size_t>(index)];
    }

  private:
    std::vector<float> m_bins;
    int m_numBins = 0;
};

} // namespace audio
} // namespace phu
