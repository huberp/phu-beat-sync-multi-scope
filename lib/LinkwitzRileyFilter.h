#pragma once

#include <array>
#include <cmath>
#include <stdexcept>
#include <vector>

/**
 * Linkwitz-Riley Filter Implementation for C++
 * Based on https://www.musicdsp.org/en/latest/Filters/266-4th-order-linkwitz-riley-filters.html
 *
 * Supports:
 * - 24 dB/octave (LR4 - 4th order, 2 cascaded biquads)
 * - 48 dB/octave (LR8 - 8th order, 4 cascaded biquads)
 *
 * Template parameter SampleType can be float or double.
 */

namespace LinkwitzRiley {

// ============================================================================
// Enums
// ============================================================================

/**
 * Filter slope enumeration
 * DB24 = 24 dB/octave (4th order, 2 cascaded biquads)
 * DB48 = 48 dB/octave (8th order, 4 cascaded biquads)
 */
enum class Slope { DB24 = 24, DB48 = 48 };

/**
 * Filter type enumeration
 */
enum class FilterType { LowPass, HighPass, AllPass };

// ============================================================================
// Constants - Q values for Butterworth filters
// ============================================================================

namespace Constants {
// LR4 (24dB/oct) = 2nd order Butterworth squared: Q = 1/sqrt(2) = 0.7071
template <typename T> constexpr T Q_LR4 = static_cast<T>(0.7071067811865476);

// LR8 (48dB/oct) = 4th order Butterworth squared: Q1 = 0.5412, Q2 = 1.3065
// Q1 = 1 / (2 * cos(pi/8)) = 0.5412 (for stages 1,2)
template <typename T> constexpr T Q_LR8_1 = static_cast<T>(0.5411961001461971);

// Q2 = 1 / (2 * cos(3*pi/8)) = 1.3065 (for stages 3,4)
template <typename T> constexpr T Q_LR8_2 = static_cast<T>(1.3065629648763766);

template <typename T> constexpr T PI = static_cast<T>(3.14159265358979323846);
} // namespace Constants

// ============================================================================
// Biquad State and Coefficients
// ============================================================================

/**
 * Biquad filter state (delay line history)
 */
template <typename SampleType> struct BiquadState {
    SampleType x1 = static_cast<SampleType>(0); // previous input sample
    SampleType x2 = static_cast<SampleType>(0); // input sample before x1
    SampleType y1 = static_cast<SampleType>(0); // previous output sample
    SampleType y2 = static_cast<SampleType>(0); // output sample before y1

    void reset() {
        x1 = x2 = y1 = y2 = static_cast<SampleType>(0);
    }
};

/**
 * Biquad filter coefficients (normalized)
 */
template <typename SampleType> struct BiquadCoeffs {
    SampleType b0 = static_cast<SampleType>(1); // feedforward coefficient 0
    SampleType b1 = static_cast<SampleType>(0); // feedforward coefficient 1
    SampleType b2 = static_cast<SampleType>(0); // feedforward coefficient 2
    SampleType a1 = static_cast<SampleType>(0); // feedback coefficient 1
    SampleType a2 = static_cast<SampleType>(0); // feedback coefficient 2
};

// ============================================================================
// Coefficient Calculation Functions
// ============================================================================

/**
 * Calculate biquad coefficients for 2nd order filter with specified Q
 * @param type Filter type: LowPass, HighPass, or AllPass
 * @param freq Cutoff frequency in Hz
 * @param sampleRate Sample rate in Hz
 * @param Q Quality factor
 * @return Normalized biquad coefficients
 */
template <typename SampleType>
BiquadCoeffs<SampleType> calcBiquadCoeffsWithQ(FilterType type, SampleType freq,
                                               SampleType sampleRate, SampleType Q) {
    const SampleType omega =
        static_cast<SampleType>(2) * Constants::PI<SampleType> * freq / sampleRate;
    const SampleType sn = std::sin(omega);
    const SampleType cs = std::cos(omega);
    const SampleType alpha = sn / (static_cast<SampleType>(2) * Q);

    SampleType b0, b1, b2, a0, a1, a2;

    switch (type) {
        case FilterType::LowPass:
            b0 = (static_cast<SampleType>(1) - cs) / static_cast<SampleType>(2);
            b1 = static_cast<SampleType>(1) - cs;
            b2 = (static_cast<SampleType>(1) - cs) / static_cast<SampleType>(2);
            a0 = static_cast<SampleType>(1) + alpha;
            a1 = static_cast<SampleType>(-2) * cs;
            a2 = static_cast<SampleType>(1) - alpha;
            break;

        case FilterType::HighPass:
            b0 = (static_cast<SampleType>(1) + cs) / static_cast<SampleType>(2);
            b1 = -(static_cast<SampleType>(1) + cs);
            b2 = (static_cast<SampleType>(1) + cs) / static_cast<SampleType>(2);
            a0 = static_cast<SampleType>(1) + alpha;
            a1 = static_cast<SampleType>(-2) * cs;
            a2 = static_cast<SampleType>(1) - alpha;
            break;

        case FilterType::AllPass:
            b0 = static_cast<SampleType>(1) - alpha;
            b1 = static_cast<SampleType>(-2) * cs;
            b2 = static_cast<SampleType>(1) + alpha;
            a0 = static_cast<SampleType>(1) + alpha;
            a1 = static_cast<SampleType>(-2) * cs;
            a2 = static_cast<SampleType>(1) - alpha;
            break;

        default:
            throw std::invalid_argument("Unknown filter type");
    }

    // Normalize coefficients
    BiquadCoeffs<SampleType> coeffs;
    coeffs.b0 = b0 / a0;
    coeffs.b1 = b1 / a0;
    coeffs.b2 = b2 / a0;
    coeffs.a1 = a1 / a0;
    coeffs.a2 = a2 / a0;

    return coeffs;
}

// ============================================================================
// Mono Linkwitz-Riley Filter
// ============================================================================

/**
 * Mono Linkwitz-Riley Filter
 *
 * LP/HP: LR4 = 2 stages, LR8 = 4 stages
 * Allpass (for phase matching LP+HP sum): LR4 = 1 stage, LR8 = 2 stages
 */
template <typename SampleType> class LinkwitzRileyFilter {
  public:
    LinkwitzRileyFilter() = default;

    /**
     * Create a new mono Linkwitz-Riley filter
     * @param type Filter type: LowPass, HighPass, or AllPass
     * @param slope Filter slope: DB24 or DB48
     * @param freq Cutoff frequency in Hz
     * @param sampleRate Sample rate in Hz
     */
    LinkwitzRileyFilter(FilterType type, Slope slope, SampleType freq, SampleType sampleRate) {
        setParams(type, slope, freq, sampleRate);
    }

    /**
     * Update filter parameters
     */
    void setParams(FilterType type, Slope slope, SampleType freq, SampleType sampleRate) {
        m_type = type;
        m_slope = slope;
        m_freq = freq;
        m_sampleRate = sampleRate;

        // Determine number of stages based on filter type and slope
        // LP/HP: LR4 = 2 stages, LR8 = 4 stages
        // Allpass (for phase matching LP+HP sum): LR4 = 1 stage, LR8 = 2 stages
        if (type == FilterType::AllPass) {
            // Allpass for phase compensation has HALF the stages of LP/HP
            m_numStages = (slope == Slope::DB48) ? 2 : 1;
        } else {
            m_numStages = (slope == Slope::DB48) ? 4 : 2;
        }

        // Resize arrays
        m_stages.resize(m_numStages);
        m_coeffsList.resize(m_numStages);

        // Calculate coefficients for each stage with appropriate Q
        if (type == FilterType::AllPass) {
            // Allpass for phase compensation
            if (slope == Slope::DB24) {
                // LR4 LP+HP = 2nd order allpass with Q = 0.7071
                m_coeffsList[0] = calcBiquadCoeffsWithQ<SampleType>(
                    FilterType::AllPass, freq, sampleRate, Constants::Q_LR4<SampleType>);
            } else {
                // LR8 LP+HP = 4th order allpass with Q1 = 0.5412, Q2 = 1.3065
                m_coeffsList[0] = calcBiquadCoeffsWithQ<SampleType>(
                    FilterType::AllPass, freq, sampleRate, Constants::Q_LR8_1<SampleType>);
                m_coeffsList[1] = calcBiquadCoeffsWithQ<SampleType>(
                    FilterType::AllPass, freq, sampleRate, Constants::Q_LR8_2<SampleType>);
            }
        } else if (slope == Slope::DB24) {
            // LR4: 2 stages, both with Q = 0.7071
            auto c = calcBiquadCoeffsWithQ<SampleType>(type, freq, sampleRate,
                                                       Constants::Q_LR4<SampleType>);
            m_coeffsList[0] = c;
            m_coeffsList[1] = c;
        } else {
            // LR8: 4 stages - stages 1,2 with Q1, stages 3,4 with Q2
            auto c1 = calcBiquadCoeffsWithQ<SampleType>(type, freq, sampleRate,
                                                        Constants::Q_LR8_1<SampleType>);
            auto c2 = calcBiquadCoeffsWithQ<SampleType>(type, freq, sampleRate,
                                                        Constants::Q_LR8_2<SampleType>);
            m_coeffsList[0] = c1;
            m_coeffsList[1] = c1;
            m_coeffsList[2] = c2;
            m_coeffsList[3] = c2;
        }
    }

    /**
     * Reset all filter state (clear delay line history)
     */
    void reset() {
        for (auto& stage : m_stages) {
            stage.reset();
        }
    }

    /**
     * Process a single sample
     * @param x Input sample
     * @return Filtered output sample
     */
    SampleType processSample(SampleType x) {
        SampleType y = x;
        for (size_t i = 0; i < m_numStages; ++i) {
            const auto& c = m_coeffsList[i];
            auto& s = m_stages[i];

            SampleType y0 = c.b0 * y + c.b1 * s.x1 + c.b2 * s.x2 - c.a1 * s.y1 - c.a2 * s.y2;
            s.x2 = s.x1;
            s.x1 = y;
            s.y2 = s.y1;
            s.y1 = y0;
            y = y0;
        }
        return y;
    }

    FilterType getType() const {
        return m_type;
    }
    Slope getSlope() const {
        return m_slope;
    }
    SampleType getFreq() const {
        return m_freq;
    }
    SampleType getSampleRate() const {
        return m_sampleRate;
    }
    size_t getNumStages() const {
        return m_numStages;
    }

  private:
    FilterType m_type = FilterType::LowPass;
    Slope m_slope = Slope::DB24;
    SampleType m_freq = static_cast<SampleType>(1000);
    SampleType m_sampleRate = static_cast<SampleType>(44100);
    size_t m_numStages = 2;
    std::vector<BiquadState<SampleType>> m_stages;
    std::vector<BiquadCoeffs<SampleType>> m_coeffsList;
};

// ============================================================================
// Stereo Linkwitz-Riley Filter
// ============================================================================

/**
 * Stereo-capable Linkwitz-Riley filter class
 * Wraps two mono filters for left and right channels
 */
template <typename SampleType> class StereoLinkwitzRileyFilter {
  public:
    StereoLinkwitzRileyFilter() = default;

    /**
     * Create a new stereo Linkwitz-Riley filter
     */
    StereoLinkwitzRileyFilter(FilterType type, Slope slope, SampleType freq, SampleType sampleRate)
        : m_left(type, slope, freq, sampleRate), m_right(type, slope, freq, sampleRate) {
    }

    /**
     * Update filter parameters for both channels
     */
    void setParams(FilterType type, Slope slope, SampleType freq, SampleType sampleRate) {
        m_left.setParams(type, slope, freq, sampleRate);
        m_right.setParams(type, slope, freq, sampleRate);
    }

    /**
     * Reset filter state for both channels
     */
    void reset() {
        m_left.reset();
        m_right.reset();
    }

    /**
     * Process a single stereo sample
     */
    void processSample(SampleType inL, SampleType inR, SampleType& outL, SampleType& outR) {
        outL = m_left.processSample(inL);
        outR = m_right.processSample(inR);
    }

    LinkwitzRileyFilter<SampleType>& getLeft() {
        return m_left;
    }
    LinkwitzRileyFilter<SampleType>& getRight() {
        return m_right;
    }

  private:
    LinkwitzRileyFilter<SampleType> m_left;
    LinkwitzRileyFilter<SampleType> m_right;
};

// ============================================================================
// CrossOver - 2-band stereo crossover filter
// ============================================================================

/**
 * 2-band stereo crossover filter
 * Splits signal into low-pass and high-pass bands at the crossover frequency
 */
template <typename SampleType> class CrossOver {
  public:
    CrossOver() = default;

    /**
     * Create a new 2-band crossover
     */
    CrossOver(Slope slope, SampleType freq, SampleType sampleRate)
        : m_slope(slope), m_lp(FilterType::LowPass, slope, freq, sampleRate),
          m_hp(FilterType::HighPass, slope, freq, sampleRate),
          m_ap(FilterType::AllPass, slope, freq, sampleRate) {
    }

    /**
     * Update crossover parameters
     */
    void setParams(Slope slope, SampleType freq, SampleType sampleRate) {
        m_slope = slope;
        m_lp.setParams(FilterType::LowPass, slope, freq, sampleRate);
        m_hp.setParams(FilterType::HighPass, slope, freq, sampleRate);
        m_ap.setParams(FilterType::AllPass, slope, freq, sampleRate);
    }

    /**
     * Reset all filter states
     */
    void reset() {
        m_lp.reset();
        m_hp.reset();
        m_ap.reset();
    }

    /**
     * Process a single stereo sample and split into LP and HP bands
     */
    void processSample(SampleType inL, SampleType inR, SampleType& lpL, SampleType& lpR,
                       SampleType& hpL, SampleType& hpR) {
        m_lp.processSample(inL, inR, lpL, lpR);
        m_hp.processSample(inL, inR, hpL, hpR);
    }

    StereoLinkwitzRileyFilter<SampleType>& getLowPass() {
        return m_lp;
    }
    StereoLinkwitzRileyFilter<SampleType>& getHighPass() {
        return m_hp;
    }
    StereoLinkwitzRileyFilter<SampleType>& getAllPass() {
        return m_ap;
    }

  private:
    Slope m_slope = Slope::DB24;
    StereoLinkwitzRileyFilter<SampleType> m_lp;
    StereoLinkwitzRileyFilter<SampleType> m_hp;
    StereoLinkwitzRileyFilter<SampleType> m_ap;
};

// ============================================================================
// MultiBandN - Configurable N-band stereo crossover (2 to 7 bands)
// ============================================================================

/**
 * MultiBandN class: configurable N-band stereo crossover (2 to 7 bands)
 *
 * Uses SERIAL CASCADING with ALLPASS COMPENSATION for flat frequency response.
 * At each crossover, LP+HP process SAME signal (LP+HP = allpass).
 * Lower bands get allpass filters at higher crossover frequencies to match phase.
 *
 * Band 1 = LP(f1) + AP(f2) + AP(f3) + ...
 * Band 2 = LP(f2) of HP(f1) output + AP(f3) + AP(f4) + ...
 * Band N = HP(fN-1) output (no allpass needed)
 */
template <typename SampleType> class MultiBandN {
  public:
    static constexpr size_t MAX_BANDS = 7;
    static constexpr size_t MAX_FREQS = 6;

    MultiBandN() = default;

    /**
     * Create a new N-band crossover
     * @param slope Filter slope: DB24 or DB48
     * @param freqs Array of crossover frequencies (N-1 frequencies for N bands)
     * @param numFreqs Number of crossover frequencies
     * @param sampleRate Sample rate in Hz
     * Example: 3 bands needs 2 frequencies: {300, 3000} -> bands: 0-300, 300-3000, 3000+
     */
    MultiBandN(Slope slope, const SampleType* freqs, size_t numFreqs, SampleType sampleRate) {
        initialize(slope, freqs, numFreqs, sampleRate);
    }

    /**
     * Initialize the crossover
     */
    void initialize(Slope slope, const SampleType* freqs, size_t numFreqs, SampleType sampleRate) {
        if (numFreqs < 1 || numFreqs > MAX_FREQS) {
            throw std::invalid_argument(
                "MultiBandN requires 1-6 crossover frequencies (2-7 bands)");
        }

        m_slope = slope;
        m_numBands = numFreqs + 1;
        m_numFreqs = numFreqs;
        m_sampleRate = sampleRate;

        // Store frequencies
        for (size_t i = 0; i < numFreqs; ++i) {
            m_freqs[i] = freqs[i];
        }

        // Create LP and HP filter pairs for each crossover frequency
        for (size_t i = 0; i < numFreqs; ++i) {
            m_lpFiltersL[i].setParams(FilterType::LowPass, slope, freqs[i], sampleRate);
            m_lpFiltersR[i].setParams(FilterType::LowPass, slope, freqs[i], sampleRate);
            m_hpFiltersL[i].setParams(FilterType::HighPass, slope, freqs[i], sampleRate);
            m_hpFiltersR[i].setParams(FilterType::HighPass, slope, freqs[i], sampleRate);
        }

        // Create allpass compensation filters
        // Band i needs allpass at frequencies freqs[i+1] through freqs[numFreqs-1]
        // This compensates for phase shift introduced by LP filters in higher crossover stages
        for (size_t band = 0; band < numFreqs; ++band) {
            for (size_t f = band + 1; f < numFreqs; ++f) {
                m_allpassL[band][f].setParams(FilterType::AllPass, slope, freqs[f], sampleRate);
                m_allpassR[band][f].setParams(FilterType::AllPass, slope, freqs[f], sampleRate);
            }
        }
    }

    /**
     * Update crossover parameters
     */
    void setParams(Slope slope, const SampleType* freqs, size_t numFreqs, SampleType sampleRate) {
        if (numFreqs != m_numFreqs) {
            throw std::invalid_argument("Number of frequencies must match original band count");
        }

        m_slope = slope;
        m_sampleRate = sampleRate;

        for (size_t i = 0; i < numFreqs; ++i) {
            m_freqs[i] = freqs[i];
            m_lpFiltersL[i].setParams(FilterType::LowPass, slope, freqs[i], sampleRate);
            m_lpFiltersR[i].setParams(FilterType::LowPass, slope, freqs[i], sampleRate);
            m_hpFiltersL[i].setParams(FilterType::HighPass, slope, freqs[i], sampleRate);
            m_hpFiltersR[i].setParams(FilterType::HighPass, slope, freqs[i], sampleRate);
        }

        // Update allpass filters
        for (size_t band = 0; band < numFreqs; ++band) {
            for (size_t f = band + 1; f < numFreqs; ++f) {
                m_allpassL[band][f].setParams(FilterType::AllPass, slope, freqs[f], sampleRate);
                m_allpassR[band][f].setParams(FilterType::AllPass, slope, freqs[f], sampleRate);
            }
        }
    }

    /**
     * Reset all filter states
     */
    void reset() {
        for (size_t i = 0; i < m_numFreqs; ++i) {
            m_lpFiltersL[i].reset();
            m_lpFiltersR[i].reset();
            m_hpFiltersL[i].reset();
            m_hpFiltersR[i].reset();
        }

        // Reset allpass filters
        for (size_t band = 0; band < m_numFreqs; ++band) {
            for (size_t f = band + 1; f < m_numFreqs; ++f) {
                m_allpassL[band][f].reset();
                m_allpassR[band][f].reset();
            }
        }
    }

    /**
     * Get the number of frequency bands
     */
    size_t getNumBands() const {
        return m_numBands;
    }

    /**
     * Get the crossover frequencies
     */
    const SampleType* getFrequencies() const {
        return m_freqs.data();
    }

    /**
     * Process a single stereo sample and split into N frequency bands
     *
     * Uses SERIAL CASCADING with ALLPASS COMPENSATION:
     * 1. LP and HP both process the SAME signal at each stage
     * 2. LP output = this band, HP output = input to next stage
     * 3. Each band gets allpass filters at higher crossover frequencies
     *    to compensate for phase shift, ensuring flat sum
     *
     * @param inL Left input sample
     * @param inR Right input sample
     * @param bandsL Output array for left channel bands (must have numBands elements)
     * @param bandsR Output array for right channel bands (must have numBands elements)
     */
    void processSample(SampleType inL, SampleType inR, SampleType* bandsL, SampleType* bandsR) {
        SampleType inputL = inL;
        SampleType inputR = inR;

        for (size_t f = 0; f < m_numFreqs; ++f) {
            // Both LP and HP process the SAME input signal
            SampleType lpL = m_lpFiltersL[f].processSample(inputL);
            SampleType lpR = m_lpFiltersR[f].processSample(inputR);
            SampleType hpL = m_hpFiltersL[f].processSample(inputL);
            SampleType hpR = m_hpFiltersR[f].processSample(inputR);

            // Apply allpass compensation to LP output (this band)
            // Band f needs allpass at frequencies f+1 through numFreqs-1
            SampleType bandL = lpL;
            SampleType bandR = lpR;
            for (size_t apFreq = f + 1; apFreq < m_numFreqs; ++apFreq) {
                bandL = m_allpassL[f][apFreq].processSample(bandL);
                bandR = m_allpassR[f][apFreq].processSample(bandR);
            }

            // Store compensated band output
            bandsL[f] = bandL;
            bandsR[f] = bandR;

            // HP output becomes input to next stage
            inputL = hpL;
            inputR = hpR;
        }

        // Last band = final HP output (no allpass needed - phase already in cascade)
        bandsL[m_numBands - 1] = inputL;
        bandsR[m_numBands - 1] = inputR;
    }

    /**
     * Process a block of stereo samples and split into N frequency bands
     *
     * @param inL Left input buffer
     * @param inR Right input buffer
     * @param bandsL Array of output buffers for left channel bands
     * @param bandsR Array of output buffers for right channel bands
     * @param numSamples Number of samples to process
     */
    void processBlock(const SampleType* inL, const SampleType* inR, SampleType** bandsL,
                      SampleType** bandsR, size_t numSamples) {
        for (size_t i = 0; i < numSamples; ++i) {
            // Temporary arrays for this sample
            std::array<SampleType, MAX_BANDS> tempBandsL;
            std::array<SampleType, MAX_BANDS> tempBandsR;

            processSample(inL[i], inR[i], tempBandsL.data(), tempBandsR.data());

            // Copy to output buffers
            for (size_t b = 0; b < m_numBands; ++b) {
                bandsL[b][i] = tempBandsL[b];
                bandsR[b][i] = tempBandsR[b];
            }
        }
    }

    /**
     * Sum all bands with optional per-band gains
     *
     * @param bandsL Array of input buffers for left channel bands
     * @param bandsR Array of input buffers for right channel bands
     * @param outL Left output buffer
     * @param outR Right output buffer
     * @param numSamples Number of samples to process
     * @param gains Optional array of gain values per band (nullptr = unity gain for all)
     */
    void sumBands(const SampleType* const* bandsL, const SampleType* const* bandsR,
                  SampleType* outL, SampleType* outR, size_t numSamples,
                  const SampleType* gains = nullptr) {
        for (size_t i = 0; i < numSamples; ++i) {
            SampleType sumL = static_cast<SampleType>(0);
            SampleType sumR = static_cast<SampleType>(0);

            for (size_t b = 0; b < m_numBands; ++b) {
                SampleType g = gains ? gains[b] : static_cast<SampleType>(1);
                sumL += g * bandsL[b][i];
                sumR += g * bandsR[b][i];
            }

            outL[i] = sumL;
            outR[i] = sumR;
        }
    }

  private:
    Slope m_slope = Slope::DB24;
    size_t m_numBands = 2;
    size_t m_numFreqs = 1;
    SampleType m_sampleRate = static_cast<SampleType>(44100);
    std::array<SampleType, MAX_FREQS> m_freqs{};

    // LP filters for each crossover frequency
    std::array<LinkwitzRileyFilter<SampleType>, MAX_FREQS> m_lpFiltersL;
    std::array<LinkwitzRileyFilter<SampleType>, MAX_FREQS> m_lpFiltersR;

    // HP filters for each crossover frequency
    std::array<LinkwitzRileyFilter<SampleType>, MAX_FREQS> m_hpFiltersL;
    std::array<LinkwitzRileyFilter<SampleType>, MAX_FREQS> m_hpFiltersR;

    // Allpass compensation filters [band][freqIndex]
    std::array<std::array<LinkwitzRileyFilter<SampleType>, MAX_FREQS>, MAX_FREQS> m_allpassL;
    std::array<std::array<LinkwitzRileyFilter<SampleType>, MAX_FREQS>, MAX_FREQS> m_allpassR;
};

} // namespace LinkwitzRiley

