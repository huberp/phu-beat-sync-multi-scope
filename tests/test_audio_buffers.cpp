#include <cassert>
#include <cmath>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

// Pull in the classes under test (header-only, no JUCE dependency)
#include "audio/RawSampleBuffer.h"
#include "audio/BucketSet.h"

// ---------------------------------------------------------------------------
// Minimal test harness
// ---------------------------------------------------------------------------

namespace {

struct TestResult {
    std::string name;
    bool        passed = false;
    std::string message;
};

std::vector<TestResult> g_results;

void runTest(const std::string& name, std::function<void()> fn) {
    TestResult r;
    r.name = name;
    try {
        fn();
        r.passed = true;
    } catch (const std::exception& e) {
        r.passed  = false;
        r.message = e.what();
    } catch (...) {
        r.passed  = false;
        r.message = "unknown exception";
    }
    g_results.push_back(r);
}

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            throw std::runtime_error("EXPECT failed: " #cond               \
                                     "  (" __FILE__ ":" +                   \
                                     std::to_string(__LINE__) + ")");       \
        }                                                                     \
    } while (false)

#define EXPECT_APPROX(a, b, eps)                                              \
    do {                                                                      \
        if (std::abs((a) - (b)) > (eps)) {                                    \
            throw std::runtime_error(                                         \
                "EXPECT_APPROX failed: |" #a " - " #b "| > " #eps           \
                "  (" __FILE__ ":" + std::to_string(__LINE__) + ")");        \
        }                                                                     \
    } while (false)

} // namespace

// ---------------------------------------------------------------------------
// RawSampleBuffer tests
// ---------------------------------------------------------------------------

void test_rawbuffer_default_state() {
    phu::audio::RawSampleBuffer buf;
    EXPECT(buf.size() == 0);
    EXPECT(buf.data() == nullptr);
}

void test_rawbuffer_prepare_size() {
    phu::audio::RawSampleBuffer buf;
    // 1 beat at 120 BPM, 48000 Hz  →  N = ceil(0.5 × 48000) = 24000
    buf.prepare(1.0, 120.0, 48000.0);
    EXPECT(buf.size() == 24000);
}

void test_rawbuffer_prepare_size_ceil() {
    phu::audio::RawSampleBuffer buf;
    // 1 beat at 100 BPM, 44100 Hz  →  seconds = 60/100 = 0.6
    //   N = ceil(0.6 × 44100) = ceil(26460) = 26460
    buf.prepare(1.0, 100.0, 44100.0);
    EXPECT(buf.size() == 26460);
}

void test_rawbuffer_resize_clears() {
    phu::audio::RawSampleBuffer buf;
    buf.prepare(1.0, 120.0, 48000.0);

    // Write something
    buf.write(1.0f, 0.0);

    // Resize should zero everything
    buf.resize(100);
    EXPECT(buf.size() == 100);
    EXPECT(buf.data() != nullptr);
    for (int i = 0; i < buf.size(); ++i)
        EXPECT(buf.data()[i] == 0.0f);
}

void test_rawbuffer_write_index_zero_ppq() {
    phu::audio::RawSampleBuffer buf;
    buf.prepare(1.0, 120.0, 48000.0);

    auto r = buf.write(0.5f, 0.0);
    // normPos = fmod(0, 1) / 1 = 0  →  idx = 0
    EXPECT(r.from == 0);
    EXPECT(r.to   == 1);
    EXPECT(buf.data()[0] == 0.5f);
}

void test_rawbuffer_write_index_mid() {
    phu::audio::RawSampleBuffer buf;
    // N = 1000 for simplicity (resize directly)
    buf.resize(1000);
    // displayBeats defaults to 1.0 after default-construct; but resize() doesn't
    // change m_displayBeats.  Use prepare() to set up correctly.
    buf.prepare(1.0, 60.0, 1000.0); // 1 beat at 60 BPM, SR=1000 → N=1000

    // ppq = 0.5  →  normPos = 0.5/1.0 = 0.5  →  idx = 500
    auto r = buf.write(0.25f, 0.5);
    EXPECT(r.from == 500);
    EXPECT(r.to   == 501);
    EXPECT(buf.data()[500] == 0.25f);
}

void test_rawbuffer_write_wraps_display_range() {
    phu::audio::RawSampleBuffer buf;
    // displayBeats = 2, N = 2000
    buf.prepare(2.0, 60.0, 1000.0); // 2 beats at 60 BPM, SR=1000 → N=2000
    EXPECT(buf.size() == 2000);

    // ppq = 3.0  →  fmod(3.0, 2.0) = 1.0  →  normPos = 1.0/2.0 = 0.5
    //  idx = 0.5 * 2000 = 1000
    auto r = buf.write(1.0f, 3.0);
    EXPECT(r.from == 1000);
    EXPECT(r.to   == 1001);
}

void test_rawbuffer_write_returns_range() {
    phu::audio::RawSampleBuffer buf;
    buf.prepare(1.0, 60.0, 1000.0);

    auto r = buf.write(1.0f, 0.75);
    // normPos = 0.75, idx = 750
    EXPECT(r.from == 750);
    EXPECT(r.to   == r.from + 1);
}

void test_rawbuffer_clear() {
    phu::audio::RawSampleBuffer buf;
    buf.resize(10);
    buf.write(1.0f, 0.0);
    buf.clear();
    for (int i = 0; i < buf.size(); ++i)
        EXPECT(buf.data()[i] == 0.0f);
}

void test_rawbuffer_empty_write_returns_zero_range() {
    phu::audio::RawSampleBuffer buf; // size == 0
    auto r = buf.write(1.0f, 0.5);
    EXPECT(r.from == 0);
    EXPECT(r.to   == 0);
}

void test_rawbuffer_write_negative_ppq_wraps() {
    phu::audio::RawSampleBuffer buf;
    // N=1000, displayBeats=1.0; ppq=-0.5 → fmod(-0.5,1.0)/1.0 = -0.5 → normalized = 0.5 → idx=500
    buf.prepare(1.0, 60.0, 1000.0);
    auto r = buf.write(0.75f, -0.5);
    EXPECT(r.from == 500);
    EXPECT(r.to   == 501);
    EXPECT(buf.data()[500] == 0.75f);
}

// ---------------------------------------------------------------------------
// BucketSet — Rms kind tests
// ---------------------------------------------------------------------------

void test_bucketset_rms_count() {
    using phu::audio::BucketSet;
    BucketSet bs(BucketSet::Kind::Rms);
    // 1 beat display, 48000-sample buffer  →  bucketSize = 48000/(1*16) = 3000
    //   numBuckets = ceil(48000/3000) = 16  (16 per beat × 1 beat)
    bs.recompute(120.0, 48000.0, 1.0, 48000);
    EXPECT(bs.bucketCount() == 16);
}

void test_bucketset_rms_count_multi_beat() {
    using phu::audio::BucketSet;
    BucketSet bs(BucketSet::Kind::Rms);
    // 2 beats  →  32 sub-beat buckets
    bs.recompute(120.0, 48000.0, 2.0, 48000);
    EXPECT(bs.bucketCount() == 32);
}

void test_bucketset_rms_max_capped() {
    using phu::audio::BucketSet;
    BucketSet bs(BucketSet::Kind::Rms);
    // 8 beats  →  8*16 = 128 sub-beat buckets (at the cap)
    bs.recompute(120.0, 48000.0, 8.0, 48000);
    EXPECT(bs.bucketCount() == 128);
}

void test_bucketset_rms_exceeds_max_capped() {
    using phu::audio::BucketSet;
    BucketSet bs(BucketSet::Kind::Rms);
    // 16 beats would give 256 buckets, but max is 128
    bs.recompute(120.0, 48000.0, 16.0, 384000);
    EXPECT(bs.bucketCount() == BucketSet::kMaxRmsBuckets);
}

void test_bucketset_rms_boundaries_contiguous() {
    using phu::audio::BucketSet;
    BucketSet bs(BucketSet::Kind::Rms);
    bs.recompute(120.0, 48000.0, 1.0, 48000);

    const auto& buckets = bs.buckets();
    EXPECT(!buckets.empty());
    // First bucket starts at 0
    EXPECT(buckets.front().startIdx == 0);
    // Each bucket starts where the previous ended
    for (int i = 1; i < static_cast<int>(buckets.size()); ++i)
        EXPECT(buckets[i].startIdx == buckets[i - 1].endIdx);
    // Last bucket ends at bufferSize
    EXPECT(buckets.back().endIdx == 48000);
}

void test_bucketset_rms_all_dirty_after_recompute() {
    using phu::audio::BucketSet;
    BucketSet bs(BucketSet::Kind::Rms);
    bs.recompute(120.0, 48000.0, 1.0, 48000);
    for (int i = 0; i < bs.bucketCount(); ++i)
        EXPECT(bs.bucket(i).dirty);
}

// ---------------------------------------------------------------------------
// BucketSet — Cancel kind tests
// ---------------------------------------------------------------------------

void test_bucketset_cancel_bucket_size() {
    using phu::audio::BucketSet;
    BucketSet bs(BucketSet::Kind::Cancel);
    // 48000 Hz  →  ceil(48000 × 0.004) = ceil(192) = 192 samples/bucket
    //   48000 / 192 = 250 buckets (≤ 256)
    bs.recompute(120.0, 48000.0, 1.0, 48000);
    EXPECT(bs.bucketCount() == 250);
    EXPECT(bs.bucket(0).startIdx == 0);
    EXPECT(bs.bucket(0).endIdx   == 192);
}

void test_bucketset_cancel_boundaries_contiguous() {
    using phu::audio::BucketSet;
    BucketSet bs(BucketSet::Kind::Cancel);
    bs.recompute(120.0, 48000.0, 1.0, 48000);

    const auto& buckets = bs.buckets();
    EXPECT(!buckets.empty());
    EXPECT(buckets.front().startIdx == 0);
    for (int i = 1; i < static_cast<int>(buckets.size()); ++i)
        EXPECT(buckets[i].startIdx == buckets[i - 1].endIdx);
    EXPECT(buckets.back().endIdx == 48000);
}

void test_bucketset_cancel_max_capped() {
    using phu::audio::BucketSet;
    BucketSet bs(BucketSet::Kind::Cancel);
    // Very large buffer; 44100 Hz → bucketSize = ceil(176.4) = 177
    //   177 × 256 = 45312 samples → bufferSize 200000 would give >256 buckets
    bs.recompute(120.0, 44100.0, 4.0, 200000);
    EXPECT(bs.bucketCount() == BucketSet::kMaxCancelBuckets);
}

// ---------------------------------------------------------------------------
// BucketSet — markDirty tests
// ---------------------------------------------------------------------------

void test_bucketset_markdirty_clears_and_marks() {
    using phu::audio::BucketSet;
    BucketSet bs(BucketSet::Kind::Rms);
    bs.recompute(120.0, 48000.0, 1.0, 48000); // 16 buckets, 3000 each

    // Clear all dirty flags manually
    for (int i = 0; i < bs.bucketCount(); ++i)
        bs.bucket(i).dirty = false;

    // Mark dirty for range [3000, 6000) — this overlaps only bucket[1]
    bs.markDirty(3000, 6000);

    EXPECT(!bs.bucket(0).dirty);
    EXPECT( bs.bucket(1).dirty);
    EXPECT(!bs.bucket(2).dirty);
}

void test_bucketset_markdirty_partial_overlap() {
    using phu::audio::BucketSet;
    BucketSet bs(BucketSet::Kind::Rms);
    bs.recompute(120.0, 48000.0, 1.0, 48000); // 16 buckets, 3000 each

    for (int i = 0; i < bs.bucketCount(); ++i)
        bs.bucket(i).dirty = false;

    // Overlap with buckets 0 ([0,3000)) and 1 ([3000,6000))
    bs.markDirty(2999, 3001);

    EXPECT( bs.bucket(0).dirty);
    EXPECT( bs.bucket(1).dirty);
    EXPECT(!bs.bucket(2).dirty);
}

void test_bucketset_markdirty_no_overlap() {
    using phu::audio::BucketSet;
    BucketSet bs(BucketSet::Kind::Rms);
    bs.recompute(120.0, 48000.0, 1.0, 48000);

    for (int i = 0; i < bs.bucketCount(); ++i)
        bs.bucket(i).dirty = false;

    // Range [3000, 3001) only hits bucket[1]
    bs.markDirty(3000, 3001);
    EXPECT(!bs.bucket(0).dirty);
    EXPECT( bs.bucket(1).dirty);
}

void test_bucketset_markdirty_all() {
    using phu::audio::BucketSet;
    BucketSet bs(BucketSet::Kind::Rms);
    bs.recompute(120.0, 48000.0, 1.0, 48000);

    for (int i = 0; i < bs.bucketCount(); ++i)
        bs.bucket(i).dirty = false;

    bs.markDirty(0, 48000);

    for (int i = 0; i < bs.bucketCount(); ++i)
        EXPECT(bs.bucket(i).dirty);
}

// ---------------------------------------------------------------------------
// BucketSet — dirty iterator tests
// ---------------------------------------------------------------------------

void test_bucketset_dirty_iterator_all_dirty() {
    using phu::audio::BucketSet;
    BucketSet bs(BucketSet::Kind::Rms);
    bs.recompute(120.0, 48000.0, 1.0, 48000);

    int count = 0;
    for (auto it = bs.dirtyBegin(); it != bs.dirtyEnd(); ++it)
        ++count;

    EXPECT(count == bs.bucketCount());
}

void test_bucketset_dirty_iterator_none_dirty() {
    using phu::audio::BucketSet;
    BucketSet bs(BucketSet::Kind::Rms);
    bs.recompute(120.0, 48000.0, 1.0, 48000);

    for (int i = 0; i < bs.bucketCount(); ++i)
        bs.bucket(i).dirty = false;

    int count = 0;
    for (auto it = bs.dirtyBegin(); it != bs.dirtyEnd(); ++it)
        ++count;

    EXPECT(count == 0);
}

void test_bucketset_dirty_iterator_mixed() {
    using phu::audio::BucketSet;
    BucketSet bs(BucketSet::Kind::Rms);
    bs.recompute(120.0, 48000.0, 1.0, 48000); // 16 buckets

    for (int i = 0; i < bs.bucketCount(); ++i)
        bs.bucket(i).dirty = (i % 2 == 0); // even buckets dirty

    int count = 0;
    for (auto it = bs.dirtyBegin(); it != bs.dirtyEnd(); ++it) {
        EXPECT(it->dirty);
        ++count;
    }

    EXPECT(count == 8); // 8 even indices out of 16
}

void test_bucketset_dirty_iterator_range_for() {
    using phu::audio::BucketSet;
    // Verify the iterator works in a range-for style adapter (manual)
    BucketSet bs(BucketSet::Kind::Cancel);
    bs.recompute(120.0, 48000.0, 1.0, 48000);

    for (int i = 0; i < bs.bucketCount(); ++i)
        bs.bucket(i).dirty = false;

    bs.markDirty(5000, 6000);

    int dirtyCount = 0;
    for (auto it = bs.dirtyBegin(); it != bs.dirtyEnd(); ++it) {
        EXPECT(it->dirty);
        EXPECT(it->startIdx < 6000 && it->endIdx > 5000); // overlap check
        ++dirtyCount;
    }
    EXPECT(dirtyCount >= 1);
}

// ---------------------------------------------------------------------------
// BucketSet — recompute on parameter change
// ---------------------------------------------------------------------------

void test_bucketset_recompute_different_samplerate() {
    using phu::audio::BucketSet;
    BucketSet bs(BucketSet::Kind::Cancel);

    // 44100 Hz: bucketSize = ceil(44100 * 0.004) = ceil(176.4) = 177
    bs.recompute(120.0, 44100.0, 1.0, 44100);
    EXPECT(bs.bucket(0).startIdx == 0);
    EXPECT(bs.bucket(0).endIdx   == 177);

    // 48000 Hz: bucketSize = ceil(48000 * 0.004) = 192
    bs.recompute(120.0, 48000.0, 1.0, 48000);
    EXPECT(bs.bucket(0).startIdx == 0);
    EXPECT(bs.bucket(0).endIdx   == 192);
}

void test_bucketset_recompute_empty_buffer() {
    using phu::audio::BucketSet;
    BucketSet bs(BucketSet::Kind::Rms);
    bs.recompute(120.0, 48000.0, 1.0, 0);
    EXPECT(bs.bucketCount() == 0);
}

// ---------------------------------------------------------------------------
// RawSampleBuffer + BucketSet integration
// ---------------------------------------------------------------------------

void test_integration_write_and_mark_dirty() {
    using phu::audio::RawSampleBuffer;
    using phu::audio::BucketSet;

    RawSampleBuffer buf;
    buf.prepare(1.0, 60.0, 1000.0); // N = 1000

    BucketSet rms(BucketSet::Kind::Rms);
    rms.recompute(60.0, 1000.0, 1.0, buf.size()); // bucketSize = 1000/16 = 62, ~16 buckets

    // Clear all dirty
    for (int i = 0; i < rms.bucketCount(); ++i)
        rms.bucket(i).dirty = false;

    // Write at ppq = 0.5  →  idx = 500
    auto range = buf.write(1.0f, 0.5);
    rms.markDirty(range.from, range.to);

    // Only the bucket covering index 500 should be dirty
    int dirtyCount = 0;
    for (auto it = rms.dirtyBegin(); it != rms.dirtyEnd(); ++it) {
        EXPECT(it->startIdx <= 500 && it->endIdx > 500);
        ++dirtyCount;
    }
    EXPECT(dirtyCount == 1);
}

void test_integration_resize_then_recompute() {
    using phu::audio::RawSampleBuffer;
    using phu::audio::BucketSet;

    RawSampleBuffer buf;
    buf.prepare(1.0, 120.0, 48000.0); // N = 24000

    BucketSet bs(BucketSet::Kind::Rms);
    bs.recompute(120.0, 48000.0, 1.0, buf.size());
    const int countBefore = bs.bucketCount(); // bucketSize = 24000/(1*16) = 1500 → 16 buckets

    // BPM changes → buf resized, buckets recomputed
    buf.prepare(1.0, 60.0, 48000.0); // N = 48000
    bs.recompute(60.0, 48000.0, 1.0, buf.size());
    const int countAfter = bs.bucketCount(); // bucketSize = 48000/(1*16) = 3000 → 16 buckets

    EXPECT(buf.size() == 48000);
    // For Rms with 1 beat, bucket count = displayBeats*16 = 16, regardless of N
    // (because bucketSize scales proportionally with N)
    EXPECT(countBefore == 16);
    EXPECT(countAfter  == 16);
    // All buckets should be dirty after recompute
    for (int i = 0; i < bs.bucketCount(); ++i)
        EXPECT(bs.bucket(i).dirty);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    // RawSampleBuffer tests
    runTest("rawbuffer_default_state",            test_rawbuffer_default_state);
    runTest("rawbuffer_prepare_size",             test_rawbuffer_prepare_size);
    runTest("rawbuffer_prepare_size_ceil",        test_rawbuffer_prepare_size_ceil);
    runTest("rawbuffer_resize_clears",            test_rawbuffer_resize_clears);
    runTest("rawbuffer_write_index_zero_ppq",     test_rawbuffer_write_index_zero_ppq);
    runTest("rawbuffer_write_index_mid",          test_rawbuffer_write_index_mid);
    runTest("rawbuffer_write_wraps_display_range", test_rawbuffer_write_wraps_display_range);
    runTest("rawbuffer_write_returns_range",      test_rawbuffer_write_returns_range);
    runTest("rawbuffer_clear",                    test_rawbuffer_clear);
    runTest("rawbuffer_empty_write_returns_zero_range", test_rawbuffer_empty_write_returns_zero_range);
    runTest("rawbuffer_write_negative_ppq_wraps",       test_rawbuffer_write_negative_ppq_wraps);

    // BucketSet::Rms tests
    runTest("bucketset_rms_count",               test_bucketset_rms_count);
    runTest("bucketset_rms_count_multi_beat",    test_bucketset_rms_count_multi_beat);
    runTest("bucketset_rms_max_capped",          test_bucketset_rms_max_capped);
    runTest("bucketset_rms_exceeds_max_capped",  test_bucketset_rms_exceeds_max_capped);
    runTest("bucketset_rms_boundaries_contiguous", test_bucketset_rms_boundaries_contiguous);
    runTest("bucketset_rms_all_dirty_after_recompute", test_bucketset_rms_all_dirty_after_recompute);

    // BucketSet::Cancel tests
    runTest("bucketset_cancel_bucket_size",       test_bucketset_cancel_bucket_size);
    runTest("bucketset_cancel_boundaries_contiguous", test_bucketset_cancel_boundaries_contiguous);
    runTest("bucketset_cancel_max_capped",        test_bucketset_cancel_max_capped);

    // markDirty tests
    runTest("bucketset_markdirty_clears_and_marks", test_bucketset_markdirty_clears_and_marks);
    runTest("bucketset_markdirty_partial_overlap",  test_bucketset_markdirty_partial_overlap);
    runTest("bucketset_markdirty_no_overlap",       test_bucketset_markdirty_no_overlap);
    runTest("bucketset_markdirty_all",              test_bucketset_markdirty_all);

    // Dirty iterator tests
    runTest("bucketset_dirty_iterator_all_dirty",  test_bucketset_dirty_iterator_all_dirty);
    runTest("bucketset_dirty_iterator_none_dirty", test_bucketset_dirty_iterator_none_dirty);
    runTest("bucketset_dirty_iterator_mixed",      test_bucketset_dirty_iterator_mixed);
    runTest("bucketset_dirty_iterator_range_for",  test_bucketset_dirty_iterator_range_for);

    // Recompute tests
    runTest("bucketset_recompute_different_samplerate", test_bucketset_recompute_different_samplerate);
    runTest("bucketset_recompute_empty_buffer",         test_bucketset_recompute_empty_buffer);

    // Integration tests
    runTest("integration_write_and_mark_dirty",   test_integration_write_and_mark_dirty);
    runTest("integration_resize_then_recompute",  test_integration_resize_then_recompute);

    // Print results
    int passed = 0, failed = 0;
    for (const auto& r : g_results) {
        if (r.passed) {
            std::printf("  PASS  %s\n", r.name.c_str());
            ++passed;
        } else {
            std::printf("  FAIL  %s — %s\n", r.name.c_str(), r.message.c_str());
            ++failed;
        }
    }
    std::printf("\n%d passed, %d failed\n", passed, failed);
    return (failed == 0) ? 0 : 1;
}
