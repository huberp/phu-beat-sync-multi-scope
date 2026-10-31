#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

// Pull in the function under test (header-only, no JUCE dependency)
#include "../src/DisplayRangeLimits.h"

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

using phu::display::getMaxDisplayBeatsForBpm;

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_boundaries() {
    EXPECT(getMaxDisplayBeatsForBpm(39.99) == 1.0);
    EXPECT(getMaxDisplayBeatsForBpm(40.0)  == 2.0);
    EXPECT(getMaxDisplayBeatsForBpm(59.99) == 2.0);
    EXPECT(getMaxDisplayBeatsForBpm(60.0)  == 4.0);
    EXPECT(getMaxDisplayBeatsForBpm(79.99) == 4.0);
    EXPECT(getMaxDisplayBeatsForBpm(80.0)  == 8.0);
}

void test_high_tempos() {
    EXPECT(getMaxDisplayBeatsForBpm(120.0) == 8.0);
    EXPECT(getMaxDisplayBeatsForBpm(300.0) == 8.0);
}

void test_low_and_invalid_tempos() {
    EXPECT(getMaxDisplayBeatsForBpm(0.0)   == 1.0);
    EXPECT(getMaxDisplayBeatsForBpm(-1.0)  == 1.0);
    EXPECT(getMaxDisplayBeatsForBpm(-120.0) == 1.0);
    EXPECT(getMaxDisplayBeatsForBpm(10.0)  == 1.0);
}

void test_monotonic_and_valid_values() {
    double previous = 0.0;
    for (int i = 1; i <= 4000; ++i) {
        const double bpm    = i * 0.1; // 0.1 .. 400 BPM
        const double result = getMaxDisplayBeatsForBpm(bpm);
        EXPECT(result == 1.0 || result == 2.0 || result == 4.0 || result == 8.0);
        EXPECT(result >= previous);
        previous = result;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main() {
    runTest("display_range_boundaries",           test_boundaries);
    runTest("display_range_high_tempos",          test_high_tempos);
    runTest("display_range_low_invalid_tempos",   test_low_and_invalid_tempos);
    runTest("display_range_monotonic_valid_set",  test_monotonic_and_valid_values);

    int failed = 0;
    for (const auto& r : g_results) {
        if (r.passed) {
            std::printf("[PASS] %s\n", r.name.c_str());
        } else {
            std::printf("[FAIL] %s: %s\n", r.name.c_str(), r.message.c_str());
            ++failed;
        }
    }

    std::printf("\n%d/%d tests passed\n",
                static_cast<int>(g_results.size()) - failed,
                static_cast<int>(g_results.size()));
    return failed == 0 ? 0 : 1;
}
