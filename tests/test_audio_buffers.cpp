#include <cassert>
#include <cmath>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

// Pull in the classes under test (header-only, no JUCE dependency)
#include "audio/PpqAddressedRingBuffer.h"
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
// PpqAddressedRingBuffer tests
// ---------------------------------------------------------------------------

void test_ringbuffer_default_state() {
    phu::audio::PpqAddressedRingBuffer<float> buf;
    EXPECT(buf.size() == 0);
}

void test_ringbuffer_prepare_size() {
    phu::audio::PpqAddressedRingBuffer<float> buf;
    // 1 beat at 120 BPM, 48000 Hz  →  N = ceil(0.5 × 48000) = 24000
    buf.prepare(1.0, 120.0, 48000.0);
    EXPECT(buf.size() == 24000);
}

void test_ringbuffer_prepare_size_ceil() {
    phu::audio::PpqAddressedRingBuffer<float> buf;
    // 1 beat at 100 BPM, 44100 Hz  →  seconds = 60/100 = 0.6
    //   N = ceil(0.6 × 44100) = ceil(26460) = 26460
    buf.prepare(1.0, 100.0, 44100.0);
    EXPECT(buf.size() == 26460);
}

void test_ringbuffer_prepare_clears() {
    phu::audio::PpqAddressedRingBuffer<float> buf;
    buf.prepare(1.0, 120.0, 48000.0);

    // Write something
    buf.write(1.0f, 0.0);

    // Re-prepare should zero everything
    buf.prepare(1.0, 120.0, 48000.0);
    EXPECT(buf.size() == 24000);
    for (int i = 0; i < buf.size(); ++i)
        EXPECT(buf.data()[i] == 0.0f);
}

void test_ringbuffer_write_index_zero_ppq() {
    phu::audio::PpqAddressedRingBuffer<float> buf;
    buf.prepare(1.0, 120.0, 48000.0);

    auto r = buf.write(0.5f, 0.0);
    // normPos = fmod(0, 1) / 1 = 0  →  idx = 0
    EXPECT(r.start == 0);
    EXPECT(r.end   == 1);
    EXPECT(buf.data()[0] == 0.5f);
}

void test_ringbuffer_write_index_mid() {
    phu::audio::PpqAddressedRingBuffer<float> buf;
    buf.prepare(1.0, 60.0, 1000.0); // 1 beat at 60 BPM, SR=1000 → N=1000

    // ppq = 0.5  →  normPos = 0.5/1.0 = 0.5  →  idx = 500
    auto r = buf.write(0.25f, 0.5);
    EXPECT(r.start == 500);
    EXPECT(r.end   == 501);
    EXPECT(buf.data()[500] == 0.25f);
}

void test_ringbuffer_write_wraps_display_range() {
    phu::audio::PpqAddressedRingBuffer<float> buf;
    // displayBeats = 2, N = 2000
    buf.prepare(2.0, 60.0, 1000.0); // 2 beats at 60 BPM, SR=1000 → N=2000
    EXPECT(buf.size() == 2000);

    // ppq = 3.0  →  fmod(3.0, 2.0) = 1.0  →  normPos = 1.0/2.0 = 0.5
    //  idx = 0.5 * 2000 = 1000
    auto r = buf.write(1.0f, 3.0);
    EXPECT(r.start == 1000);
    EXPECT(r.end   == 1001);
}

void test_ringbuffer_write_returns_range() {
    phu::audio::PpqAddressedRingBuffer<float> buf;
    buf.prepare(1.0, 60.0, 1000.0);

    auto r = buf.write(1.0f, 0.75);
    // normPos = 0.75, idx = 750
    EXPECT(r.start == 750);
    EXPECT(r.end   == r.start + 1);
}

void test_ringbuffer_clear() {
    phu::audio::PpqAddressedRingBuffer<float> buf;
    buf.prepare(1.0, 60.0, 10.0); // N = ceil(60/60 * 10) = 10
    buf.write(1.0f, 0.0);
    buf.clear();
    for (int i = 0; i < buf.size(); ++i)
        EXPECT(buf.data()[i] == 0.0f);
}

void test_ringbuffer_empty_write_returns_zero_range() {
    phu::audio::PpqAddressedRingBuffer<float> buf; // size == 0
    auto r = buf.write(1.0f, 0.5);
    EXPECT(r.start == 0);
    EXPECT(r.end   == 0);
}

void test_ringbuffer_write_negative_ppq_wraps() {
    phu::audio::PpqAddressedRingBuffer<float> buf;
    // N=1000, displayBeats=1.0; ppq=-0.5 → fmod(-0.5,1.0) = -0.5 → +1.0 = 0.5 → idx=500
    buf.prepare(1.0, 60.0, 1000.0);
    auto r = buf.write(0.75f, -0.5);
    EXPECT(r.start == 500);
    EXPECT(r.end   == 501);
    EXPECT(buf.data()[500] == 0.75f);
}

void test_ringbuffer_insert_no_wrap() {
    phu::audio::PpqAddressedRingBuffer<float> buf;
    buf.prepare(1.0, 60.0, 1000.0); // N=1000

    float samples[] = {1.0f, 2.0f, 3.0f};
    auto result = buf.insert(0.0, samples, 3);
    EXPECT(result.ok);
    EXPECT(result.range1.start == 0);
    EXPECT(result.range1.end == 3);
    EXPECT(!result.range2.valid());
    EXPECT(buf.data()[0] == 1.0f);
    EXPECT(buf.data()[1] == 2.0f);
    EXPECT(buf.data()[2] == 3.0f);
}

void test_ringbuffer_insert_with_wrap() {
    phu::audio::PpqAddressedRingBuffer<float> buf;
    buf.prepare(1.0, 60.0, 100.0); // N=100

    // Write 5 samples starting near the end: ppq=0.98 → idx=98
    float samples[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
    auto result = buf.insert(0.98, samples, 5);
    EXPECT(result.ok);
    EXPECT(result.range1.start == 98);
    EXPECT(result.range1.end == 100);
    EXPECT(result.range2.valid());
    EXPECT(result.range2.start == 0);
    EXPECT(result.range2.end == 3);
}

// ---------------------------------------------------------------------------
// BucketSet tests
// ---------------------------------------------------------------------------

void test_bucketset_initialize_by_size() {
    phu::audio::BucketSet bs;
    bs.initializeBySize(48000, 16);
    EXPECT(bs.bucketCount() == 16);
}

void test_bucketset_initialize_multi_bucket() {
    phu::audio::BucketSet bs;
    bs.initializeBySize(48000, 32);
    EXPECT(bs.bucketCount() == 32);
}

void test_bucketset_initialize_max_capped() {
    phu::audio::BucketSet bs;
    // Request 128 buckets for 48000 samples
    bs.initializeBySize(48000, 128);
    EXPECT(bs.bucketCount() == 128);
}

void test_bucketset_initialize_exceeds_buffer() {
    phu::audio::BucketSet bs;
    // Request more buckets than samples — capped to bufferSize
    bs.initializeBySize(10, 100);
    EXPECT(bs.bucketCount() == 10);
}

void test_bucketset_boundaries_contiguous() {
    phu::audio::BucketSet bs;
    bs.initializeBySize(48000, 16);

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

void test_bucketset_all_dirty_after_initialize() {
    phu::audio::BucketSet bs;
    bs.initializeBySize(48000, 16);
    for (int i = 0; i < bs.bucketCount(); ++i)
        EXPECT(bs.bucket(i).dirty);
}

void test_bucketset_cancel_style_bucket_count() {
    phu::audio::BucketSet bs;
    // Mimic cancel-style: 48000 Hz → ~4ms per bucket → ceil(48000*0.004) = 192 samples/bucket
    //   48000 / 192 = 250 buckets
    const int cancelBucketSize = static_cast<int>(std::ceil(48000.0 * 0.004)); // 192
    const int cancelBucketCount = std::min(256, std::max(1, (48000 + cancelBucketSize - 1) / cancelBucketSize));
    bs.initializeBySize(48000, cancelBucketCount);
    EXPECT(bs.bucketCount() == cancelBucketCount);
}

void test_bucketset_cancel_boundaries_contiguous() {
    phu::audio::BucketSet bs;
    const int cancelBucketSize = static_cast<int>(std::ceil(48000.0 * 0.004));
    const int cancelBucketCount = std::min(256, std::max(1, (48000 + cancelBucketSize - 1) / cancelBucketSize));
    bs.initializeBySize(48000, cancelBucketCount);

    const auto& buckets = bs.buckets();
    EXPECT(!buckets.empty());
    EXPECT(buckets.front().startIdx == 0);
    for (int i = 1; i < static_cast<int>(buckets.size()); ++i)
        EXPECT(buckets[i].startIdx == buckets[i - 1].endIdx);
    EXPECT(buckets.back().endIdx == 48000);
}

void test_bucketset_cancel_max_capped() {
    phu::audio::BucketSet bs;
    // Very large buffer; request 300 buckets — capped to buffer if smaller
    const int cancelBucketSize = static_cast<int>(std::ceil(44100.0 * 0.004));
    const int rawCount = (200000 + cancelBucketSize - 1) / cancelBucketSize;
    const int cancelBucketCount = std::min(256, std::max(1, rawCount));
    bs.initializeBySize(200000, cancelBucketCount);
    EXPECT(bs.bucketCount() == 256);
}

// ---------------------------------------------------------------------------
// BucketSet — markDirty tests
// ---------------------------------------------------------------------------

void test_bucketset_markdirty_clears_and_marks() {
    phu::audio::BucketSet bs;
    bs.initializeBySize(48000, 16);

    // Clear all dirty flags manually
    for (int i = 0; i < bs.bucketCount(); ++i)
        bs.bucket(i).dirty = false;

    // Mark dirty for range [3000, 6000) — proportional bucket boundaries
    bs.markDirty(3000, 6000);

    // At least one bucket should be dirty
    int dirtyCount = 0;
    for (int i = 0; i < bs.bucketCount(); ++i)
        if (bs.bucket(i).dirty) ++dirtyCount;
    EXPECT(dirtyCount >= 1);

    // All dirty buckets should overlap [3000, 6000)
    for (int i = 0; i < bs.bucketCount(); ++i) {
        if (bs.bucket(i).dirty) {
            EXPECT(bs.bucket(i).startIdx < 6000);
            EXPECT(bs.bucket(i).endIdx > 3000);
        }
    }
}

void test_bucketset_markdirty_partial_overlap() {
    phu::audio::BucketSet bs;
    bs.initializeBySize(48000, 16);

    for (int i = 0; i < bs.bucketCount(); ++i)
        bs.bucket(i).dirty = false;

    // Narrow range that should hit 1-2 buckets
    bs.markDirty(2999, 3001);

    int dirtyCount = 0;
    for (int i = 0; i < bs.bucketCount(); ++i)
        if (bs.bucket(i).dirty) ++dirtyCount;
    EXPECT(dirtyCount >= 1);
    EXPECT(dirtyCount <= 2);
}

void test_bucketset_markdirty_no_overlap() {
    phu::audio::BucketSet bs;
    bs.initializeBySize(48000, 16);

    for (int i = 0; i < bs.bucketCount(); ++i)
        bs.bucket(i).dirty = false;

    // Range [3000, 3001) should hit exactly one bucket
    bs.markDirty(3000, 3001);
    int dirtyCount = 0;
    for (int i = 0; i < bs.bucketCount(); ++i)
        if (bs.bucket(i).dirty) ++dirtyCount;
    EXPECT(dirtyCount == 1);
}

void test_bucketset_markdirty_all() {
    phu::audio::BucketSet bs;
    bs.initializeBySize(48000, 16);

    for (int i = 0; i < bs.bucketCount(); ++i)
        bs.bucket(i).dirty = false;

    bs.markDirty(0, 48000);

    for (int i = 0; i < bs.bucketCount(); ++i)
        EXPECT(bs.bucket(i).dirty);
}

// ---------------------------------------------------------------------------
// BucketSet — setDirty with Range/RingBufferInsertResult
// ---------------------------------------------------------------------------

void test_bucketset_setdirty_range() {
    phu::audio::BucketSet bs;
    bs.initializeBySize(1000, 10);

    for (int i = 0; i < bs.bucketCount(); ++i)
        bs.bucket(i).dirty = false;

    phu::audio::Range range{100, 200};
    bs.setDirty(range);

    int dirtyCount = 0;
    for (int i = 0; i < bs.bucketCount(); ++i)
        if (bs.bucket(i).dirty) ++dirtyCount;
    EXPECT(dirtyCount >= 1);
}

void test_bucketset_setdirty_insert_result_no_wrap() {
    phu::audio::BucketSet bs;
    bs.initializeBySize(1000, 10);

    for (int i = 0; i < bs.bucketCount(); ++i)
        bs.bucket(i).dirty = false;

    phu::audio::RingBufferInsertResult result;
    result.ok = true;
    result.range1 = {100, 300};

    bs.setDirty(result);

    int dirtyCount = 0;
    for (int i = 0; i < bs.bucketCount(); ++i)
        if (bs.bucket(i).dirty) ++dirtyCount;
    EXPECT(dirtyCount >= 1);
}

void test_bucketset_setdirty_insert_result_with_wrap() {
    phu::audio::BucketSet bs;
    bs.initializeBySize(1000, 10);

    for (int i = 0; i < bs.bucketCount(); ++i)
        bs.bucket(i).dirty = false;

    phu::audio::RingBufferInsertResult result;
    result.ok = true;
    result.range1 = {900, 1000};  // tail
    result.range2 = {0, 100};     // wrap to beginning

    bs.setDirty(result);

    // First and last buckets should be dirty
    EXPECT(bs.bucket(0).dirty);                      // covers [0, 100)
    EXPECT(bs.bucket(bs.bucketCount() - 1).dirty);   // covers [900, 1000)
}

// ---------------------------------------------------------------------------
// BucketSet — dirty iterator tests
// ---------------------------------------------------------------------------

void test_bucketset_dirty_iterator_all_dirty() {
    phu::audio::BucketSet bs;
    bs.initializeBySize(48000, 16);

    int count = 0;
    for (auto it = bs.dirtyBegin(); it != bs.dirtyEnd(); ++it)
        ++count;

    EXPECT(count == bs.bucketCount());
}

void test_bucketset_dirty_iterator_none_dirty() {
    phu::audio::BucketSet bs;
    bs.initializeBySize(48000, 16);

    for (int i = 0; i < bs.bucketCount(); ++i)
        bs.bucket(i).dirty = false;

    int count = 0;
    for (auto it = bs.dirtyBegin(); it != bs.dirtyEnd(); ++it)
        ++count;

    EXPECT(count == 0);
}

void test_bucketset_dirty_iterator_mixed() {
    phu::audio::BucketSet bs;
    bs.initializeBySize(48000, 16);

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
    phu::audio::BucketSet bs;
    const int cancelBucketSize = static_cast<int>(std::ceil(48000.0 * 0.004));
    const int cancelBucketCount = std::min(256, std::max(1, (48000 + cancelBucketSize - 1) / cancelBucketSize));
    bs.initializeBySize(48000, cancelBucketCount);

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

void test_bucketset_recompute_reinitialize() {
    phu::audio::BucketSet bs;

    // First init with 16 buckets
    bs.initializeBySize(48000, 16);
    EXPECT(bs.bucketCount() == 16);

    // Re-init with different size and count
    bs.initializeBySize(96000, 32);
    EXPECT(bs.bucketCount() == 32);
    EXPECT(bs.buckets().back().endIdx == 96000);
}

void test_bucketset_recompute_empty_buffer() {
    phu::audio::BucketSet bs;
    bs.initializeBySize(0, 16);
    EXPECT(bs.bucketCount() == 0);
}

// ---------------------------------------------------------------------------
// PpqAddressedRingBuffer + BucketSet integration
// ---------------------------------------------------------------------------

void test_integration_write_and_setdirty() {
    phu::audio::PpqAddressedRingBuffer<float> buf;
    buf.prepare(1.0, 60.0, 1000.0); // N = 1000

    phu::audio::BucketSet rms;
    rms.initializeBySize(buf.size(), 16);

    // Clear all dirty
    for (int i = 0; i < rms.bucketCount(); ++i)
        rms.bucket(i).dirty = false;

    // Write at ppq = 0.5  →  idx = 500
    auto range = buf.write(1.0f, 0.5);
    rms.setDirty(range);

    // Only the bucket covering index 500 should be dirty
    int dirtyCount = 0;
    for (auto it = rms.dirtyBegin(); it != rms.dirtyEnd(); ++it) {
        EXPECT(it->startIdx <= 500 && it->endIdx > 500);
        ++dirtyCount;
    }
    EXPECT(dirtyCount == 1);
}

void test_integration_insert_and_setdirty() {
    phu::audio::PpqAddressedRingBuffer<float> buf;
    buf.prepare(1.0, 60.0, 1000.0); // N = 1000

    phu::audio::BucketSet bs;
    bs.initializeBySize(buf.size(), 10);

    for (int i = 0; i < bs.bucketCount(); ++i)
        bs.bucket(i).dirty = false;

    // Block insert at ppq = 0.0
    float samples[50];
    for (int i = 0; i < 50; ++i) samples[i] = static_cast<float>(i) * 0.01f;
    auto result = buf.insert(0.0, samples, 50);
    bs.setDirty(result);

    // At least the first bucket should be dirty
    EXPECT(bs.bucket(0).dirty);
}

void test_integration_resize_then_reinitialize() {
    // Pre-allocate enough capacity for the worst case (lowest BPM)
    phu::audio::PpqAddressedRingBuffer<float> buf;
    buf.reserveByWorstCase(60.0, 48000.0, 1.0); // capacity for 60 BPM
    buf.prepare(1.0, 120.0, 48000.0); // N = 24000

    phu::audio::BucketSet bs;
    bs.initializeBySize(buf.size(), 16);
    const int countBefore = bs.bucketCount(); // 16

    // BPM changes → buf resized within pre-allocated capacity, buckets reinitialized
    buf.prepare(1.0, 60.0, 48000.0); // N = 48000 (fits in reserved capacity)
    bs.initializeBySize(buf.size(), 16);
    const int countAfter = bs.bucketCount(); // still 16

    EXPECT(buf.size() == 48000);
    EXPECT(countBefore == 16);
    EXPECT(countAfter  == 16);
    // All buckets should be dirty after initializeBySize
    for (int i = 0; i < bs.bucketCount(); ++i)
        EXPECT(bs.bucket(i).dirty);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    // PpqAddressedRingBuffer tests
    runTest("ringbuffer_default_state",            test_ringbuffer_default_state);
    runTest("ringbuffer_prepare_size",             test_ringbuffer_prepare_size);
    runTest("ringbuffer_prepare_size_ceil",        test_ringbuffer_prepare_size_ceil);
    runTest("ringbuffer_prepare_clears",           test_ringbuffer_prepare_clears);
    runTest("ringbuffer_write_index_zero_ppq",     test_ringbuffer_write_index_zero_ppq);
    runTest("ringbuffer_write_index_mid",          test_ringbuffer_write_index_mid);
    runTest("ringbuffer_write_wraps_display_range", test_ringbuffer_write_wraps_display_range);
    runTest("ringbuffer_write_returns_range",      test_ringbuffer_write_returns_range);
    runTest("ringbuffer_clear",                    test_ringbuffer_clear);
    runTest("ringbuffer_empty_write_returns_zero_range", test_ringbuffer_empty_write_returns_zero_range);
    runTest("ringbuffer_write_negative_ppq_wraps",       test_ringbuffer_write_negative_ppq_wraps);
    runTest("ringbuffer_insert_no_wrap",           test_ringbuffer_insert_no_wrap);
    runTest("ringbuffer_insert_with_wrap",         test_ringbuffer_insert_with_wrap);

    // BucketSet tests
    runTest("bucketset_initialize_by_size",        test_bucketset_initialize_by_size);
    runTest("bucketset_initialize_multi_bucket",   test_bucketset_initialize_multi_bucket);
    runTest("bucketset_initialize_max_capped",     test_bucketset_initialize_max_capped);
    runTest("bucketset_initialize_exceeds_buffer",  test_bucketset_initialize_exceeds_buffer);
    runTest("bucketset_boundaries_contiguous",     test_bucketset_boundaries_contiguous);
    runTest("bucketset_all_dirty_after_initialize", test_bucketset_all_dirty_after_initialize);
    runTest("bucketset_cancel_style_bucket_count",  test_bucketset_cancel_style_bucket_count);
    runTest("bucketset_cancel_boundaries_contiguous", test_bucketset_cancel_boundaries_contiguous);
    runTest("bucketset_cancel_max_capped",          test_bucketset_cancel_max_capped);

    // markDirty tests
    runTest("bucketset_markdirty_clears_and_marks", test_bucketset_markdirty_clears_and_marks);
    runTest("bucketset_markdirty_partial_overlap",  test_bucketset_markdirty_partial_overlap);
    runTest("bucketset_markdirty_no_overlap",       test_bucketset_markdirty_no_overlap);
    runTest("bucketset_markdirty_all",              test_bucketset_markdirty_all);

    // setDirty with Range/RingBufferInsertResult
    runTest("bucketset_setdirty_range",             test_bucketset_setdirty_range);
    runTest("bucketset_setdirty_insert_result_no_wrap", test_bucketset_setdirty_insert_result_no_wrap);
    runTest("bucketset_setdirty_insert_result_with_wrap", test_bucketset_setdirty_insert_result_with_wrap);

    // Dirty iterator tests
    runTest("bucketset_dirty_iterator_all_dirty",  test_bucketset_dirty_iterator_all_dirty);
    runTest("bucketset_dirty_iterator_none_dirty", test_bucketset_dirty_iterator_none_dirty);
    runTest("bucketset_dirty_iterator_mixed",      test_bucketset_dirty_iterator_mixed);
    runTest("bucketset_dirty_iterator_range_for",  test_bucketset_dirty_iterator_range_for);

    // Recompute tests
    runTest("bucketset_recompute_reinitialize",    test_bucketset_recompute_reinitialize);
    runTest("bucketset_recompute_empty_buffer",    test_bucketset_recompute_empty_buffer);

    // Integration tests
    runTest("integration_write_and_setdirty",      test_integration_write_and_setdirty);
    runTest("integration_insert_and_setdirty",     test_integration_insert_and_setdirty);
    runTest("integration_resize_then_reinitialize", test_integration_resize_then_reinitialize);

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
