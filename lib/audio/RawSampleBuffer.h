#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace phu {
namespace audio {

/**
 * RawSampleBuffer — position-addressed overwrite ring buffer for beat-synced audio.
 *
 * Owns a flat float array of size N = ceil(displayBeats × 60/bpm × sampleRate).
 * Samples are written at position-mapped indices derived from the absolute PPQ:
 *
 *   normPos  = fmod(ppq, displayBeats) / displayBeats   // [0, 1)
 *   writeIdx = (int)(normPos × N)
 *
 * This is a position-addressed overwrite ring — not a FIFO. Old data at a given
 * position is silently overwritten as the playhead advances.
 *
 * The buffer must be cleared and resized (via prepare() or resize()) whenever
 * displayBeats, bpm, or sampleRate changes because N depends on all three.
 *
 * Thread safety: all operations are intended to run exclusively on the UI thread.
 */
class RawSampleBuffer {
  public:
    /** Half-open index range returned by write(); used to mark bucket regions dirty. */
    struct WriteRange {
        int from; ///< inclusive start index
        int to;   ///< exclusive end index (always from + 1)
    };

    RawSampleBuffer() = default;

    /**
     * Prepare the buffer for the given display parameters.
     *
     * Computes N = ceil(displayBeats × 60/bpm × sampleRate), then calls resize().
     * Safe to call repeatedly; always clears previous content.
     *
     * @param displayBeats  Musical range covered by the buffer (e.g. 1.0 or 2.0 beats).
     * @param bpm           Current tempo in beats per minute.
     * @param sampleRate    Current audio sample rate in Hz.
     */
    void prepare(double displayBeats, double bpm, double sampleRate) {
        m_displayBeats = (displayBeats > 0.0) ? displayBeats : 1.0;
        const double seconds = m_displayBeats * 60.0 / ((bpm > 0.0) ? bpm : 120.0);
        const int newSize = static_cast<int>(std::ceil(seconds * sampleRate));
        resize(newSize > 0 ? newSize : 1);
    }

    /**
     * Resize and clear the buffer to newSize samples.
     *
     * All existing data is discarded. Callers should re-mark all dependent bucket
     * sets dirty after calling this method.
     */
    void resize(int newSize) {
        m_buffer.assign(static_cast<size_t>(newSize > 0 ? newSize : 0), 0.0f);
        m_size = newSize > 0 ? newSize : 0;
    }

    /**
     * Write a single sample at the position corresponding to absolute PPQ ppq.
     *
     * @param sample  Audio sample value to store.
     * @param ppq     Absolute DAW playhead position in beats.
     * @return        WriteRange{writeIdx, writeIdx+1} for dirty-marking bucket sets.
     *                Returns {0, 0} if the buffer is empty.
     */
    WriteRange write(float sample, double ppq) {
        if (m_size <= 0)
            return {0, 0};

        const double normPos0 = std::fmod(ppq, m_displayBeats) / m_displayBeats;
        // fmod can return a negative value for negative ppq; normalize to [0, 1)
        const double normPos  = (normPos0 < 0.0) ? normPos0 + 1.0 : normPos0;
        int idx = static_cast<int>(normPos * static_cast<double>(m_size));
        if (idx < 0) idx = 0;
        if (idx >= m_size) idx = m_size - 1;

        m_buffer[static_cast<size_t>(idx)] = sample;
        return {idx, idx + 1};
    }

    /**
     * Compute the ring-buffer index that corresponds to the given PPQ position
     * without writing anything.  Used by callers that need to know the start
     * index before issuing a batch of writeAt() calls.
     *
     * @param ppq  Absolute DAW playhead position in beats.
     * @return     Buffer index in [0, size()-1], or 0 if the buffer is empty.
     */
    int indexForPpq(double ppq) const {
        if (m_size <= 0) return 0;
        const double normPos0 = std::fmod(ppq, m_displayBeats) / m_displayBeats;
        const double normPos  = (normPos0 < 0.0) ? normPos0 + 1.0 : normPos0;
        int idx = static_cast<int>(normPos * static_cast<double>(m_size));
        if (idx < 0) idx = 0;
        if (idx >= m_size) idx = m_size - 1;
        return idx;
    }

    /**
     * Write a single pre-filtered sample at a pre-computed buffer index.
     *
     * The index must have been obtained from indexForPpq() (or computed by
     * advancing a previous index by 1 modulo size()).  No bounds computation
     * or PPQ math is performed here.
     *
     * @param idx   Buffer index in [0, size()-1].
     * @param val   Sample value to store.
     */
    void writeAt(int idx, float val) {
        m_buffer[static_cast<size_t>(idx)] = val;
    }

    /** Direct read access to the sample array for scatter/RMS passes. */
    const float* data() const { return m_buffer.data(); }

    /** Number of samples currently allocated in the buffer. */
    int size() const { return m_size; }

    /** Clear all samples to 0.0 without reallocating. */
    void clear() { std::fill(m_buffer.begin(), m_buffer.end(), 0.0f); }

    /** The display range (beats) this buffer was last prepared for. */
    double displayBeats() const { return m_displayBeats; }

  private:
    std::vector<float> m_buffer;
    int    m_size         = 0;
    double m_displayBeats = 1.0;
};

} // namespace audio
} // namespace phu
