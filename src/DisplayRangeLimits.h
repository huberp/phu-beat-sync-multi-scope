#pragma once

namespace phu::display {

/**
 * Returns the maximum allowed display range (in beats) for a given BPM.
 *
 * At very slow tempos a wide beat window spans many seconds, making the display
 * impractical.  The thresholds below were chosen to keep the longest
 * displayable window within ≈ 6 s across the supported sample-rate range:
 *
 *   BPM ≥ 80  → up to 8 beats  (8 beats @ 80 BPM = 6 s)
 *   BPM ≥ 60  → up to 4 beats  (4 beats @ 60 BPM = 4 s)
 *   BPM ≥ 40  → up to 2 beats  (2 beats @ 40 BPM = 3 s)
 *   BPM <  40 → up to 1 beat
 */
inline double getMaxDisplayBeatsForBpm(double bpm) {
    if (bpm >= 80.0) return 8.0;
    if (bpm >= 60.0) return 4.0;
    if (bpm >= 40.0) return 2.0;
    return 1.0;
}

} // namespace phu::display
