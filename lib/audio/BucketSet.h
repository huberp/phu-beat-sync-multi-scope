#pragma once

#include <algorithm>
#include <cmath>
#include <iterator>
#include <vector>

namespace phu {
namespace audio {

/**
 * Bucket — a half-open index range [startIdx, endIdx) into a RawSampleBuffer,
 * together with a dirty flag indicating that the cached computation result is stale.
 */
struct Bucket {
    int  startIdx = 0;   ///< Inclusive start index into the ring buffer.
    int  endIdx   = 0;   ///< Exclusive end index into the ring buffer.
    bool dirty    = true; ///< True when the samples in this range have changed.
};

/**
 * BucketSet — partitions a buffer [0, bufferSize) into contiguous Bucket ranges.
 *
 * Two kinds are supported, each using a different bucket-size formula:
 *
 *   Kind::Rms     — bucket size = max(1, bufferSize / (displayBeats × 16))
 *                   Intended for per-1/16-beat RMS display.
 *                   Maximum 128 buckets.
 *
 *   Kind::Cancel  — bucket size = ceil(sampleRate × 0.004)  (~4 ms)
 *                   Intended for the cancellation-index computation.
 *                   Maximum 256 buckets.
 *
 * Call recompute() after any change to BPM, sample rate, display beats, or buffer size.
 * Call markDirty() each time samples are written to the buffer.
 * Iterate over only dirty buckets via dirtyBegin() / dirtyEnd().
 */
class BucketSet {
  public:
    /** Selects the bucket-size formula used in recompute(). */
    enum class Kind { Rms, Cancel };

    static constexpr int kMaxRmsBuckets    = 128;
    static constexpr int kMaxCancelBuckets = 256;

    explicit BucketSet(Kind kind) : m_kind(kind) {}

    // -------------------------------------------------------------------------
    // Recompute / rebuild
    // -------------------------------------------------------------------------

    /**
     * Rebuild all bucket boundaries and mark every bucket dirty.
     *
     * Must be called after BPM, sample rate, display-beats, or buffer size changes.
     *
     * @param bpm          Current tempo (used indirectly through bufferSize; kept for
     *                     API symmetry with the data model).
     * @param sampleRate   Current audio sample rate in Hz (used by Kind::Cancel).
     * @param displayBeats Musical range covered by the buffer (used by Kind::Rms).
     * @param bufferSize   Total number of samples in the RawSampleBuffer.
     */
    void recompute(double /*bpm*/, double sampleRate, double displayBeats, int bufferSize) {
        m_buckets.clear();

        if (bufferSize <= 0)
            return;

        const int bucketSize = computeBucketSize(sampleRate, displayBeats, bufferSize);
        const int maxBuckets = (m_kind == Kind::Rms) ? kMaxRmsBuckets : kMaxCancelBuckets;

        int start = 0;
        while (start < bufferSize && static_cast<int>(m_buckets.size()) < maxBuckets) {
            const int end = std::min(start + bucketSize, bufferSize);
            m_buckets.push_back({start, end, true});
            start = end;
        }
    }

    // -------------------------------------------------------------------------
    // Dirty marking
    // -------------------------------------------------------------------------

    /**
     * Mark every bucket whose range overlaps [fromIdx, toIdx) as dirty.
     *
     * Performs a linear scan; O(bucketsInRange) for contiguous write regions.
     *
     * @param fromIdx  Inclusive start of the written region.
     * @param toIdx    Exclusive end of the written region.
     */
    void markDirty(int fromIdx, int toIdx) {
        for (auto& b : m_buckets) {
            if (b.startIdx < toIdx && b.endIdx > fromIdx)
                b.dirty = true;
        }
    }

    // -------------------------------------------------------------------------
    // Dirty-bucket iteration
    // -------------------------------------------------------------------------

    /**
     * Forward iterator that yields only Bucket entries whose dirty flag is true.
     */
    class DirtyIterator {
      public:
        using iterator_category = std::forward_iterator_tag;
        using value_type        = Bucket;
        using difference_type   = std::ptrdiff_t;
        using pointer           = Bucket*;
        using reference         = Bucket&;

        DirtyIterator(std::vector<Bucket>::iterator cur,
                      std::vector<Bucket>::iterator end) noexcept
            : m_cur(cur), m_end(end) {
            skipClean();
        }

        reference  operator*()  const noexcept { return *m_cur; }
        pointer    operator->() const noexcept { return &*m_cur; }

        DirtyIterator& operator++() noexcept {
            ++m_cur;
            skipClean();
            return *this;
        }

        bool operator==(const DirtyIterator& o) const noexcept { return m_cur == o.m_cur; }
        bool operator!=(const DirtyIterator& o) const noexcept { return m_cur != o.m_cur; }

      private:
        void skipClean() noexcept {
            while (m_cur != m_end && !m_cur->dirty)
                ++m_cur;
        }

        std::vector<Bucket>::iterator m_cur;
        std::vector<Bucket>::iterator m_end;
    };

    /** Iterator to the first dirty bucket (or dirtyEnd() if none). */
    DirtyIterator dirtyBegin() { return {m_buckets.begin(), m_buckets.end()}; }

    /** Past-the-end sentinel for the dirty iterator. */
    DirtyIterator dirtyEnd()   { return {m_buckets.end(),   m_buckets.end()}; }

    // -------------------------------------------------------------------------
    // Accessors
    // -------------------------------------------------------------------------

    /** Total number of buckets (dirty and clean). */
    int bucketCount() const { return static_cast<int>(m_buckets.size()); }

    /** Bounds-checked direct access to a bucket by index. */
    const Bucket& bucket(int i) const { return m_buckets[static_cast<size_t>(i)]; }
    Bucket&       bucket(int i)       { return m_buckets[static_cast<size_t>(i)]; }

    /** Read-only access to the full bucket vector. */
    const std::vector<Bucket>& buckets() const { return m_buckets; }

    /** The Kind this BucketSet was constructed with. */
    Kind kind() const { return m_kind; }

  private:
    int computeBucketSize(double sampleRate, double displayBeats, int bufferSize) const {
        if (m_kind == Kind::Rms) {
            if (displayBeats <= 0.0) return 1;
            const int sz = static_cast<int>(static_cast<double>(bufferSize)
                                            / (displayBeats * 16.0));
            return std::max(1, sz);
        } else {
            // Kind::Cancel: ~4 ms per bucket
            const int sz = static_cast<int>(std::ceil(sampleRate * 0.004));
            return std::max(1, sz);
        }
    }

    Kind                m_kind;
    std::vector<Bucket> m_buckets;
};

} // namespace audio
} // namespace phu
