#pragma once

#include <array>
#include <cstring>
#include <juce_core/juce_core.h>

namespace phu {
namespace audio {

/**
 * Lock-free FIFO for transferring audio samples from the audio thread to the UI thread.
 *
 * Uses juce::AbstractFifo for lock-free single-writer / single-reader indexing
 * over a statically-allocated ring buffer. One instance per measurement point:
 *   - AudioSampleFifo<2> for stereo input
 *   - AudioSampleFifo<2> for stereo output sum
 *
 * The audio thread calls push() to write samples.
 * The UI thread calls pull() to read the most recent N samples.
 *
 * Template parameter NumChannels: number of interleaved channels (typically 2 for stereo).
 */
template <int NumChannels>
class AudioSampleFifo {
  public:
    /** Ring buffer capacity per channel. Must be power-of-two for efficient wrapping.
     *  32768 samples = ~0.7s at 48kHz, 2x the max FFT size of 16384. */
    static constexpr int kFifoSize = 32768;

    AudioSampleFifo() : fifo(kFifoSize) {
        // Zero-initialize all channel buffers
        for (auto& ch : buffer)
            std::memset(ch.data(), 0, sizeof(float) * kFifoSize);
    }

    /**
     * Push samples from the audio thread into the FIFO.
     *
     * @param channelData  Array of NumChannels float pointers, each pointing to numSamples floats.
     * @param numSamples   Number of samples per channel to push.
     *
     * If there isn't enough free space, the oldest un-read samples will simply remain
     * un-overwritten; excess write requests are silently truncated by AbstractFifo.
     * In practice the UI thread reads frequently enough that this doesn't happen.
     */
    void push(const float* const* channelData, int numSamples) {
        const auto scope = fifo.write(numSamples);

        // Block 1 (contiguous region before wrap)
        if (scope.blockSize1 > 0) {
            for (int ch = 0; ch < NumChannels; ++ch) {
                std::memcpy(buffer[ch].data() + scope.startIndex1,
                            channelData[ch],
                            sizeof(float) * static_cast<size_t>(scope.blockSize1));
            }
        }

        // Block 2 (wrapped region at start of ring buffer)
        if (scope.blockSize2 > 0) {
            for (int ch = 0; ch < NumChannels; ++ch) {
                std::memcpy(buffer[ch].data() + scope.startIndex2,
                            channelData[ch] + scope.blockSize1,
                            sizeof(float) * static_cast<size_t>(scope.blockSize2));
            }
        }
    }

    /**
     * Pull the most recent numSamples from the FIFO (UI thread).
     *
     * This discards any older samples that precede the requested window,
     * then reads the final numSamples. This is the correct behavior for
     * display: we always want the latest data, not a queue.
     *
     * @param destination  Array of NumChannels float pointers, each with room for numSamples.
     * @param numSamples   Number of samples per channel to read.
     * @return             Number of samples actually read (may be < numSamples if not enough data).
     */
    int pull(float* const* destination, int numSamples) {
        const int available = fifo.getNumReady();

        if (available <= 0)
            return 0;

        // Discard older samples so we only read the most recent numSamples
        const int toDrop = available - numSamples;
        if (toDrop > 0) {
            // Read and discard
            const auto dropScope = fifo.read(toDrop);
            // ScopedRead destructor calls finishedRead — samples are discarded
            juce::ignoreUnused(dropScope);
        }

        const int toRead = juce::jmin(numSamples, fifo.getNumReady());
        if (toRead <= 0)
            return 0;

        const auto scope = fifo.read(toRead);

        // Block 1
        if (scope.blockSize1 > 0) {
            for (int ch = 0; ch < NumChannels; ++ch) {
                std::memcpy(destination[ch],
                            buffer[ch].data() + scope.startIndex1,
                            sizeof(float) * static_cast<size_t>(scope.blockSize1));
            }
        }

        // Block 2
        if (scope.blockSize2 > 0) {
            for (int ch = 0; ch < NumChannels; ++ch) {
                std::memcpy(destination[ch] + scope.blockSize1,
                            buffer[ch].data() + scope.startIndex2,
                            sizeof(float) * static_cast<size_t>(scope.blockSize2));
            }
        }

        return scope.blockSize1 + scope.blockSize2;
    }

    /** Returns the number of samples available for reading. */
    int getNumAvailable() const {
        return fifo.getNumReady();
    }

    /** Clears the FIFO. Call from prepareToPlay when sample rate changes or playback restarts. */
    void reset() {
        fifo.reset();
    }

  private:
    juce::AbstractFifo fifo;
    std::array<std::array<float, kFifoSize>, NumChannels> buffer;
};

} // namespace audio
} // namespace phu
