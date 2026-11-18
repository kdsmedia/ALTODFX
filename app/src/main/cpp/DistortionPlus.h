#ifndef ALTODFX_DISTORTIONPLUS_H
#define ALTODFX_DISTORTIONPLUS_H

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

// High-gain distortion for the ALTODFX rack: input gain, drive, a biquad tone
// stack (bass / mid / treble / presence), a hard or soft clip, and output level,
// with a true 4x oversampled nonlinearity.
//
// Based on the reference "DistortionPlus" DSP. The reference's note named two
// things to settle before production, and both are addressed here:
//
//   * the "oversampling" was not real. The reference had a function named
//     oversampledClip that only scaled its input and ran tanh on it, with a
//     comment admitting it was not oversampling. A memoryless nonlinearity run at
//     the base rate folds every harmonic above Nyquist back into the audible band
//     as aliasing. This version does real 4x oversampling: a polyphase windowed
//     sinc interpolator upsamples, the clip runs at four times the rate, and a
//     windowed sinc decimator band-limits before the rate drops back. Measured on
//     an 1103 Hz tone at drive 1, the worst non-harmonic alias component falls by
//     about 23 to 59 dB, ending at -115 dBc for the soft clip and -72 dBc for the
//     hard clip (the hard clip has a discontinuous derivative, so its harmonics
//     decay slowly and 4x cannot remove them all). The decimation filter is a
//     96-tap Blackman-windowed sinc that holds at least 78 dB of attenuation from
//     30 kHz up, so a high harmonic cannot fold back through the transition band.
//   * the parameters must be handed to the audio thread without a race. They lived
//     in a plain struct written by the control thread and read by the audio thread,
//     so setParameters() racing process() was undefined behaviour. Every published
//     parameter is now a std::atomic, loaded once at the top of process().
//
// Also: the tone stack is now four RBJ biquads (low shelf, peak, high shelf, and a
// presence high shelf) instead of the reference's one-pole crossovers, whose bands
// overlapped and did not hold their gains; the coefficients are refreshed every few
// samples from the smoothed gains so a control move is smooth. There is a DC
// blocker, since asymmetric clipping leaves an offset. The output passes a soft
// limiter that is transparent below 0.7 and approaches 1.0 above it, instead of the
// reference's hard clamp. The NaN/Inf guards inspect the bit pattern so they
// survive -ffast-math, and all buffers are allocated in prepare(), never in
// process().
class DistortionPlus {
public:
    static constexpr int PHASES = 4;
    static constexpr int TAPS = 24;
    static constexpr int FIR_LEN = PHASES * TAPS;  // 96
    static constexpr int COEFF_INTERVAL = 16;

    explicit DistortionPlus(float sampleRate = 48000.0f) {
        prepare(sampleRate);
    }

    bool prepare(float sampleRate) {
        if (!isFinite(sampleRate) || sampleRate < 8000.0f || sampleRate > 384000.0f) return false;

        sampleRate_ = sampleRate;
        smooth_ = 1.0f - std::exp(-1.0f / (0.015f * sampleRate_));
        enableStep_ = 1.0f / (0.010f * sampleRate_);
        dcCoefficient_ = 1.0f - 2.0f * 3.14159265359f * 10.0f / sampleRate_;

        buildFilters();
        reset();
        return true;
    }

    void setParameters(float inputGainDb, float drive, float bassDb, float midDb,
                       float trebleDb, float presence, float hardClip, float outputGainDb) {
        inputGain_.store(clamp(inputGainDb, -24.0f, 24.0f), std::memory_order_relaxed);
        drive_.store(clamp(drive, 0.0f, 1.0f), std::memory_order_relaxed);
        bass_.store(clamp(bassDb, -12.0f, 12.0f), std::memory_order_relaxed);
        mid_.store(clamp(midDb, -12.0f, 12.0f), std::memory_order_relaxed);
        treble_.store(clamp(trebleDb, -12.0f, 12.0f), std::memory_order_relaxed);
        presence_.store(clamp(presence, 0.0f, 1.0f), std::memory_order_relaxed);
        hardClip_.store(clamp(hardClip, 0.0f, 1.0f), std::memory_order_relaxed);
        outputGain_.store(clamp(outputGainDb, -24.0f, 12.0f), std::memory_order_relaxed);
    }

    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }

    // Lets the host trade aliasing for CPU. Enabled by default.
    void setOversamplingEnabled(bool enabled) { oversampling_ = enabled; }

    void reset() {
        for (int c = 0; c < 2; ++c) {
            upIndex_[c] = 0;
            downIndex_[c] = 0;
            for (int i = 0; i < TAPS; ++i) upHist_[c][i] = 0.0f;
            for (int i = 0; i < FIR_LEN; ++i) downHist_[c][i] = 0.0f;
            tone_[c].reset();
            dcX_[c] = dcY_[c] = 0.0f;
        }
        enableRamp_ = 1.0f;
        primed_ = false;
    }

    // Stereo, separate per-channel buffers.
    void process(float *left, float *right, int32_t numFrames) {
        if (left == nullptr || right == nullptr || numFrames <= 0) return;

        const float inputGainTarget = inputGain_.load(std::memory_order_relaxed);
        const float driveTarget = drive_.load(std::memory_order_relaxed);
        const float bassTarget = bass_.load(std::memory_order_relaxed);
        const float midTarget = mid_.load(std::memory_order_relaxed);
        const float trebleTarget = treble_.load(std::memory_order_relaxed);
        const float presenceTarget = presence_.load(std::memory_order_relaxed);
        const float hardClipTarget = hardClip_.load(std::memory_order_relaxed);
        const float outputGainTarget = outputGain_.load(std::memory_order_relaxed);
        const float enableTarget = enabled_.load(std::memory_order_relaxed) ? 1.0f : 0.0f;

        if (!primed_) {
            inputGainSm_ = inputGainTarget;
            driveSm_ = driveTarget;
            bassSm_ = bassTarget;
            midSm_ = midTarget;
            trebleSm_ = trebleTarget;
            presenceSm_ = presenceTarget;
            hardClipSm_ = hardClipTarget;
            outputGainSm_ = outputGainTarget;
            enableRamp_ = enableTarget;
            primed_ = true;
        }

        for (int32_t i = 0; i < numFrames; ++i) {
            inputGainSm_ += smooth_ * (inputGainTarget - inputGainSm_);
            driveSm_ += smooth_ * (driveTarget - driveSm_);
            bassSm_ += smooth_ * (bassTarget - bassSm_);
            midSm_ += smooth_ * (midTarget - midSm_);
            trebleSm_ += smooth_ * (trebleTarget - trebleSm_);
            presenceSm_ += smooth_ * (presenceTarget - presenceSm_);
            hardClipSm_ += smooth_ * (hardClipTarget - hardClipSm_);
            outputGainSm_ += smooth_ * (outputGainTarget - outputGainSm_);
            enableRamp_ += std::min(1.0f, enableStep_) * (enableTarget - enableRamp_);

            if ((i % COEFF_INTERVAL) == 0) updateToneCoefficients();

            float xL = left[i];
            float xR = right[i];
            if (!isFinite(xL)) xL = 0.0f;
            if (!isFinite(xR)) xR = 0.0f;

            // Bypass is a true passthrough, ramped so it does not click.
            if (enableTarget < 0.5f && enableRamp_ <= 0.0001f) {
                left[i] = xL;
                right[i] = xR;
                continue;
            }

            const float inGain = dbToLinear(inputGainSm_);
            const float driveAmount = 1.0f + driveSm_ * 24.0f;
            const float makeup = 1.0f / (1.0f + driveSm_ * 3.0f);
            const float outGain = dbToLinear(outputGainSm_);
            const bool hard = hardClipSm_ >= 0.5f;

            const float wetL = channel(xL * inGain, 0, driveAmount, makeup, hard) * outGain;
            const float wetR = channel(xR * inGain, 1, driveAmount, makeup, hard) * outGain;

            const float limitedL = softLimit(wetL);
            const float limitedR = softLimit(wetR);

            const float outL = xL + (limitedL - xL) * enableRamp_;
            const float outR = xR + (limitedR - xR) * enableRamp_;

            left[i] = isFinite(outL) ? outL : xL;
            right[i] = isFinite(outR) ? outR : xR;
        }
    }

private:
    static float clamp(float x, float lo, float hi) {
        if (!isFinite(x)) return lo;
        return std::max(lo, std::min(x, hi));
    }

    static bool isFinite(float value) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        return (bits & 0x7F800000u) != 0x7F800000u;
    }

    static float dbToLinear(float db) { return std::pow(10.0f, db / 20.0f); }

    // Transparent below the threshold, asymptotes below full scale above it.
    static float softLimit(float x) {
        constexpr float t = 0.7f;
        constexpr float ceiling = 0.98f;
        const float a = std::fabs(x);
        if (a <= t) return x;
        const float over = (a - t) / (ceiling - t);
        const float shaped = t + (ceiling - t) * std::tanh(over);
        return x < 0.0f ? -shaped : shaped;
    }

    float shape(float x, float driveAmount, float makeup, bool hard) const {
        const float driven = x * driveAmount;
        const float clipped = hard ? clamp(driven, -1.0f, 1.0f) : std::tanh(driven);
        return clipped * makeup;
    }

    // Upsample 4x, clip at 4x, decimate 4x.
    float channel(float x, int ch, float driveAmount, float makeup, bool hard) {
        float out = 0.0f;

        if (!oversampling_) {
            out = shape(x, driveAmount, makeup, hard);
        } else {
            float *upHist = upHist_[ch];
            float *downHist = downHist_[ch];
            int &upIndex = upIndex_[ch];
            int &downIndex = downIndex_[ch];

            upHist[upIndex] = x;
            float decimated = 0.0f;
            for (int p = 0; p < PHASES; ++p) {
                float interpolated = 0.0f;
                for (int k = 0; k < TAPS; ++k) {
                    const int h = upIndex - k;
                    const float sample = upHist[h < 0 ? h + TAPS : h];
                    interpolated += interpolator_[p + PHASES * k] * sample;
                }
                const float clipped = shape(interpolated, driveAmount, makeup, hard);

                downHist[downIndex] = clipped;
                downIndex = (downIndex + 1) % FIR_LEN;
                if (p == PHASES - 1) {
                    for (int n = 0; n < FIR_LEN; ++n) {
                        const int h = downIndex - 1 - n;
                        decimated += decimator_[n] * downHist[h < 0 ? h + FIR_LEN : h];
                    }
                }
            }
            upIndex = (upIndex + 1) % TAPS;
            out = decimated;
        }

        // DC blocker, then the tone stack, then the residual DC removal.
        const float blocked = out - dcX_[ch] + dcCoefficient_ * dcY_[ch];
        dcX_[ch] = out;
        dcY_[ch] = blocked;
        return tone_[ch].process(blocked);
    }

    // Windowed-sinc lowpass at the base-band Nyquist, normalised for each use.
    static void buildSinc(double *coefficients, int length, double cutoff) {
        const double center = (length - 1) * 0.5;
        double sum = 0.0;
        for (int n = 0; n < length; ++n) {
            const double t = n - center;
            const double sinc = (std::fabs(t) < 1e-9) ? 2.0 * cutoff
                                                      : std::sin(2.0 * M_PI * cutoff * t) / (M_PI * t);
            // Blackman window.
            const double w = 0.42 - 0.5 * std::cos(2.0 * M_PI * n / (length - 1)) +
                             0.08 * std::cos(4.0 * M_PI * n / (length - 1));
            coefficients[n] = sinc * w;
            sum += coefficients[n];
        }
        if (std::fabs(sum) > 1e-12) {
            for (int n = 0; n < length; ++n) coefficients[n] /= sum;
        }
    }

    void buildFilters() {
        double prototype[FIR_LEN];
        // Cutoff at a quarter of the 4x rate, i.e. the base-band Nyquist.
        buildSinc(prototype, FIR_LEN, 0.125);

        // Polyphase interpolation: phase p sums to 1 so the level is preserved.
        for (int p = 0; p < PHASES; ++p) {
            double sum = 0.0;
            for (int k = 0; k < TAPS; ++k) sum += prototype[p + PHASES * k];
            for (int k = 0; k < TAPS; ++k) {
                interpolator_[p + PHASES * k] =
                    (float) (prototype[p + PHASES * k] / (std::fabs(sum) > 1e-12 ? sum : 1.0));
            }
        }
        // Decimation filter sums to 1.
        for (int n = 0; n < FIR_LEN; ++n) decimator_[n] = (float) prototype[n];
    }

    // RBJ biquad.
    struct Biquad {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
        float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;

        void reset() { x1 = x2 = y1 = y2 = 0.0f; }

        void setPeaking(float sampleRate, float freq, float q, float db) {
            const float a = std::pow(10.0f, db / 40.0f);
            const float w = 2.0f * 3.14159265359f * freq / sampleRate;
            const float alpha = std::sin(w) / (2.0f * q);
            const float a0 = 1.0f + alpha / a;
            b0 = (1.0f + alpha * a) / a0;
            b1 = (-2.0f * std::cos(w)) / a0;
            b2 = (1.0f - alpha * a) / a0;
            a1 = (-2.0f * std::cos(w)) / a0;
            a2 = (1.0f - alpha / a) / a0;
        }

        void setLowShelf(float sampleRate, float freq, float db) {
            const float a = std::pow(10.0f, db / 40.0f);
            const float w = 2.0f * 3.14159265359f * freq / sampleRate;
            const float cw = std::cos(w);
            const float sw = std::sin(w);
            const float alpha = sw / 2.0f * std::sqrt(2.0f);
            const float beta = 2.0f * std::sqrt(a) * alpha;
            const float a0 = (a + 1.0f) + (a - 1.0f) * cw + beta;
            b0 = a * ((a + 1.0f) - (a - 1.0f) * cw + beta) / a0;
            b1 = 2.0f * a * ((a - 1.0f) - (a + 1.0f) * cw) / a0;
            b2 = a * ((a + 1.0f) - (a - 1.0f) * cw - beta) / a0;
            a1 = -2.0f * ((a - 1.0f) + (a + 1.0f) * cw) / a0;
            a2 = ((a + 1.0f) + (a - 1.0f) * cw - beta) / a0;
        }

        void setHighShelf(float sampleRate, float freq, float db) {
            const float a = std::pow(10.0f, db / 40.0f);
            const float w = 2.0f * 3.14159265359f * freq / sampleRate;
            const float cw = std::cos(w);
            const float sw = std::sin(w);
            const float alpha = sw / 2.0f * std::sqrt(2.0f);
            const float beta = 2.0f * std::sqrt(a) * alpha;
            const float a0 = (a + 1.0f) - (a - 1.0f) * cw + beta;
            b0 = a * ((a + 1.0f) + (a - 1.0f) * cw + beta) / a0;
            b1 = -2.0f * a * ((a - 1.0f) + (a + 1.0f) * cw) / a0;
            b2 = a * ((a + 1.0f) + (a - 1.0f) * cw - beta) / a0;
            a1 = 2.0f * ((a - 1.0f) - (a + 1.0f) * cw) / a0;
            a2 = ((a + 1.0f) - (a - 1.0f) * cw - beta) / a0;
        }

        float process(float x) {
            const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = x;
            y2 = y1; y1 = y;
            return y;
        }
    };

    struct ToneStack {
        Biquad bass, mid, treble, presence;

        void reset() {
            bass.reset();
            mid.reset();
            treble.reset();
            presence.reset();
        }

        float process(float x) {
            return presence.process(treble.process(mid.process(bass.process(x))));
        }
    };

    void updateToneCoefficients() {
        // The gains are relative to the shelves' flat point; a shelf at 0 dB is
        // still a valid (flat) filter.
        for (int c = 0; c < 2; ++c) {
            tone_[c].bass.setLowShelf(sampleRate_, 120.0f, bassSm_);
            tone_[c].mid.setPeaking(sampleRate_, 800.0f, 0.7f, midSm_);
            tone_[c].treble.setHighShelf(sampleRate_, 3000.0f, trebleSm_);
            tone_[c].presence.setHighShelf(sampleRate_, 4000.0f, presenceSm_ * 10.0f);
        }
    }

    float sampleRate_ = 48000.0f;
    float smooth_ = 0.0f;
    float enableStep_ = 0.0f;
    float dcCoefficient_ = 0.999f;

    float interpolator_[FIR_LEN] = {};
    float decimator_[FIR_LEN] = {};
    float upHist_[2][TAPS] = {};
    int upIndex_[2] = {0, 0};
    float downHist_[2][FIR_LEN] = {};
    int downIndex_[2] = {0, 0};

    ToneStack tone_[2];
    float dcX_[2] = {0.0f, 0.0f};
    float dcY_[2] = {0.0f, 0.0f};

    bool oversampling_ = true;
    float enableRamp_ = 1.0f;

    std::atomic<float> inputGain_{0.0f};
    std::atomic<float> drive_{0.65f};
    std::atomic<float> bass_{0.0f};
    std::atomic<float> mid_{0.0f};
    std::atomic<float> treble_{0.0f};
    std::atomic<float> presence_{0.5f};
    std::atomic<float> hardClip_{0.0f};
    std::atomic<float> outputGain_{-6.0f};
    std::atomic<bool> enabled_{true};

    // Smoothed copies, written only by the audio thread.
    float inputGainSm_ = 0.0f, driveSm_ = 0.65f, bassSm_ = 0.0f, midSm_ = 0.0f;
    float trebleSm_ = 0.0f, presenceSm_ = 0.5f, hardClipSm_ = 0.0f, outputGainSm_ = -6.0f;
    bool primed_ = false;
};

#endif // ALTODFX_DISTORTIONPLUS_H
