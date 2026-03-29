#pragma once

#include <array>
#include <cstring>
#include <juce_core/juce_core.h>

namespace phu {
namespace audio {

/**
 * Lock-free SPSC ring buffer for mono float samples with per-sample PPQ tracking.
 *
 * Used to transfer raw audio samples from the audio thread to the message thread
 * for UDP broadcast. Each entry stores one mono float sample alongside its absolute
 * PPQ position, allowing the broadcast timer to construct packets with an accurate
 * ppqOfFirstSample field.
 *
 * Uses juce::AbstractFifo for lock-free single-writer / single-reader indexing.
 *
 * Thread safety:
 *   - push() must be called from exactly one thread (the audio thread).
 *   - drain() must be called from exactly one other thread (the message thread).
 *   - getNumAvailable() is safe to call from either thread.
 */
class AudioSampleRingBuffer {
  public:
    /** Ring buffer capacity in samples. Sized for ~370 ms at 44.1 kHz. */
    static constexpr int CAPACITY = 16384;

    AudioSampleRingBuffer() : fifo(CAPACITY) {}

    /**
     * Push a single mono sample with its PPQ position (audio thread).
     * Returns true on success, false if the buffer is full and the sample was dropped.
     * In debug builds a message is printed to the JUCE logger on the first drop.
     */
    bool push(float mono, double ppq) {
        const auto scope = fifo.write(1);
        if (scope.blockSize1 > 0) {
            samples[static_cast<size_t>(scope.startIndex1)] = mono;
            ppqs[static_cast<size_t>(scope.startIndex1)]    = ppq;
            return true;
        }
        // blockSize1 == 0 means the ring is full — sample dropped
        DBG("AudioSampleRingBuffer: buffer full, sample dropped (ppq=" << ppq << ")");
        return false;
    }

    /**
     * Drain up to maxSamples from the ring buffer (message thread).
     *
     * @param destSamples  Destination for mono float values (must have room for maxSamples).
     * @param destPpqs     Destination for PPQ values, or nullptr to discard (must have room for maxSamples).
     * @param maxSamples   Maximum number of samples to drain.
     * @return             Number of samples actually drained.
     */
    int drain(float* destSamples, double* destPpqs, int maxSamples) {
        const int available = fifo.getNumReady();
        const int toDrain   = juce::jmin(available, maxSamples);
        if (toDrain <= 0)
            return 0;

        const auto scope = fifo.read(toDrain);

        if (scope.blockSize1 > 0) {
            std::memcpy(destSamples,
                        samples.data() + scope.startIndex1,
                        sizeof(float) * static_cast<size_t>(scope.blockSize1));
            if (destPpqs)
                std::memcpy(destPpqs,
                            ppqs.data() + scope.startIndex1,
                            sizeof(double) * static_cast<size_t>(scope.blockSize1));
        }

        if (scope.blockSize2 > 0) {
            std::memcpy(destSamples + scope.blockSize1,
                        samples.data() + scope.startIndex2,
                        sizeof(float) * static_cast<size_t>(scope.blockSize2));
            if (destPpqs)
                std::memcpy(destPpqs + scope.blockSize1,
                            ppqs.data() + scope.startIndex2,
                            sizeof(double) * static_cast<size_t>(scope.blockSize2));
        }

        return scope.blockSize1 + scope.blockSize2;
    }

    /** Returns the number of samples available for reading. */
    int getNumAvailable() const { return fifo.getNumReady(); }

    /** Reset the ring buffer (discards all pending data). */
    void reset() { fifo.reset(); }

  private:
    juce::AbstractFifo fifo;
    std::array<float,  CAPACITY> samples{};
    std::array<double, CAPACITY> ppqs{};
};

} // namespace audio
} // namespace phu
