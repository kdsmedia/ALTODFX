#ifndef ALTODFX_DISTORTION_H
#define ALTODFX_DISTORTION_H

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

// High-gain distortion for the ALTODFX rack: more aggressive than the overdrive
// with long sustain, aimed at rock through metal.
//
// Based on the reference "Distortion" DSP, hardened for this host:
//   * the reference oversampler was linear interpolation between two input
//     samples, which does not reject the images the upsampling creates, so the
//     clipping harmonics folded back as aliasing. It is replaced by a polyphase
//     windowed-sinc interpolator and a matching decimator, so the 4x rate is
//     genuinely anti-aliased.
//   * Drive/Tone/Level/Bass/Presence/Hardness are smoothed with one-pole
//     filters, so turning a knob while playing does not click.
//   * Bass and Presence were a full-band makeup gain and a boost-only high
//     shelf. They are now a proper low/high tilt around the 0.5 detent, so
//     turning them down cuts and turning them up boosts.
//   * Hardness morphs between the soft (tanh) and hard (clip) curve instead of
//     switching at 0.5, which removes the discontinuity the reference had.
//   * the NaN/Inf guards inspect the bit pattern, so they survive -ffast-math.
//
// The class is stereo: both channels are processed with independent filter
// state, and nothing allocates, so process() is safe from the audio thread.
class Distortion {
public:
    explicit Distortion(float sampleRate = 48000.0f) {
        rebuildOversampling();
        setSampleRate(sampleRate);
        reset();
    }

    void setSampleRate(float sampleRate) {
        sampleRate_ = std::max(8000.0f, sampleRate);
        gainSmoothCoef_ = 1.0f - std::exp(-1.0f / (0.010f * sampleRate_));
        toneSmoothCoef_ = 1.0f - std::exp(-1.0f / (0.020f * sampleRate_));

        for (int ch = 0; ch < 2; ++ch) {
            channels_[ch].inputFilter.setHighPass(sampleRate_, 85.0f);
            channels_[ch].bassFilter.setLowPass(sampleRate_, 220.0f);
            channels_[ch].presenceFilter.setHighPass(sampleRate_, 1600.0f);
            channels_[ch].dcFilter.setHighPass(sampleRate_, 15.0f);
            channels_[ch].toneFilter.setLowPass(sampleRate_, 6500.0f);
        }
        reset();
    }

    // All parameters are 0.0 - 1.0.
    void setParameters(float drive, float tone, float level,
                       float bass, float presence, float hardness) {
        drive_.store(clamp01(drive), std::memory_order_relaxed);
        tone_.store(clamp01(tone), std::memory_order_relaxed);
        level_.store(clamp01(level), std::memory_order_relaxed);
        bass_.store(clamp01(bass), std::memory_order_relaxed);
        presence_.store(clamp01(presence), std::memory_order_relaxed);
        hardness_.store(clamp01(hardness), std::memory_order_relaxed);
    }

    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }

    void reset() {
        for (int ch = 0; ch < 2; ++ch) {
            Channel &c = channels_[ch];
            c.inputFilter.reset();
            c.toneFilter.reset();
            c.bassFilter.reset();
            c.presenceFilter.reset();
            c.dcFilter.reset();
            for (int i = 0; i < kTapsPerPhase; ++i) c.upHistory[i] = 0.0f;
            for (int i = 0; i < kPrototypeLen; ++i) c.downHistory[i] = 0.0f;
            c.decimPhase = 0;
        }
        primed_ = false;
    }

    // Stereo, separate per-channel buffers: left[0..numFrames-1] and right.
    void process(float *left, float *right, int32_t numFrames) {
        if (left == nullptr || right == nullptr || numFrames <= 0) return;
        if (!enabled_.load(std::memory_order_relaxed)) return;

        const float driveTarget = drive_.load(std::memory_order_relaxed);
        const float toneTarget = tone_.load(std::memory_order_relaxed);
        const float levelTarget = level_.load(std::memory_order_relaxed);
        const float bassTarget = bass_.load(std::memory_order_relaxed);
        const float presenceTarget = presence_.load(std::memory_order_relaxed);
        const float hardnessTarget = hardness_.load(std::memory_order_relaxed);

        const float inputGainTarget = 1.0f + driveTarget * 29.0f;
        const float outputGainTarget = 0.12f + levelTarget * 0.70f;
        const float cutoffTarget = 1800.0f + toneTarget * 8500.0f;
        // Bass/Presence are tilts around the 0.5 detent.
        const float bassTiltTarget = (bassTarget * 2.0f - 1.0f) * 0.55f;
        const float presenceTiltTarget = (presenceTarget * 2.0f - 1.0f) * 0.50f;

        // Jump straight to the requested values on the first block so a freshly
        // loaded patch does not fade in; smooth only subsequent changes.
        if (!primed_) {
            inputGain_ = inputGainTarget;
            outputGain_ = outputGainTarget;
            cutoff_ = cutoffTarget;
            bassTilt_ = bassTiltTarget;
            presenceTilt_ = presenceTiltTarget;
            hardnessSm_ = hardnessTarget;
            for (int ch = 0; ch < 2; ++ch)
                channels_[ch].toneFilter.setLowPass(sampleRate_, cutoff_);
            primed_ = true;
        }

        for (int32_t i = 0; i < numFrames; ++i) {
            inputGain_ += gainSmoothCoef_ * (inputGainTarget - inputGain_);
            outputGain_ += gainSmoothCoef_ * (outputGainTarget - outputGain_);
            bassTilt_ += gainSmoothCoef_ * (bassTiltTarget - bassTilt_);
            presenceTilt_ += gainSmoothCoef_ * (presenceTiltTarget - presenceTilt_);
            hardnessSm_ += gainSmoothCoef_ * (hardnessTarget - hardnessSm_);
            cutoff_ += toneSmoothCoef_ * (cutoffTarget - cutoff_);

            // Recomputing the tone coefficients every 32 samples keeps the
            // per-sample cost low; the cutoff itself is already smoothed.
            if ((i & 31) == 0) {
                for (int ch = 0; ch < 2; ++ch)
                    channels_[ch].toneFilter.setLowPass(sampleRate_, cutoff_);
            }

            left[i] = runChannel(channels_[0], left[i]);
            right[i] = runChannel(channels_[1], right[i]);
        }
    }

private:
    static constexpr int kL = 4;              // 4x oversampling
    static constexpr int kTapsPerPhase = 16;
    static constexpr int kPrototypeLen = kL * kTapsPerPhase;

    struct Biquad {
        void setLowPass(float sampleRate, float cutoff) {
            const float pi = 3.14159265358979323846f;
            const float w = 2.0f * pi * std::min(cutoff, sampleRate * 0.45f) / sampleRate;
            const float c = std::cos(w);
            const float s = std::sin(w);
            const float alpha = s * 0.70710678f;
            const float a0 = 1.0f + alpha;
            b0 = ((1.0f - c) * 0.5f) / a0;
            b1 = (1.0f - c) / a0;
            b2 = b0;
            a1 = (-2.0f * c) / a0;
            a2 = (1.0f - alpha) / a0;
        }

        void setHighPass(float sampleRate, float cutoff) {
            const float pi = 3.14159265358979323846f;
            const float w = 2.0f * pi * std::min(cutoff, sampleRate * 0.45f) / sampleRate;
            const float c = std::cos(w);
            const float s = std::sin(w);
            const float alpha = s * 0.70710678f;
            const float a0 = 1.0f + alpha;
            b0 = ((1.0f + c) * 0.5f) / a0;
            b1 = -(1.0f + c) / a0;
            b2 = b0;
            a1 = (-2.0f * c) / a0;
            a2 = (1.0f - alpha) / a0;
        }

        float process(float x) {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return isFinite(y) ? y : 0.0f;
        }

        void reset() { z1 = 0.0f; z2 = 0.0f; }

        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f, z1 = 0.0f, z2 = 0.0f;
    };

    struct Channel {
        Biquad inputFilter;
        Biquad toneFilter;
        Biquad bassFilter;
        Biquad presenceFilter;
        Biquad dcFilter;
        float upHistory[kTapsPerPhase] = {};
        float downHistory[kPrototypeLen] = {};
        int decimPhase = 0;
    };

    // std::isfinite() is folded away under -ffast-math, so inspect the bit
    // pattern instead; it behaves the same at every optimisation level.
    static bool isFinite(float value) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        return (bits & 0x7F800000u) != 0x7F800000u;
    }

    static float clamp01(float value) {
        if (!isFinite(value)) return 0.0f;
        return std::max(0.0f, std::min(1.0f, value));
    }

    // Smoothly morphs between the soft (tanh) and hard (clip) curve so that a
    // change of `hardness` never steps the transfer function.
    static float waveshape(float x, float hardness) {
        const float soft = std::tanh(x);
        const float hard = std::max(-1.0f, std::min(1.0f, x));
        return soft + (hard - soft) * hardness;
    }

    // Output safety: transparent below the knee, compresses the excess smoothly
    // toward 1.0. A plain tanh() here would saturate the whole signal and squash
    // the Bass/Presence/Tone controls that run before it.
    static float softLimit(float x) {
        const float knee = 0.85f;
        const float a = std::fabs(x);
        if (a <= knee) return x;
        const float range = 1.0f - knee;
        const float over = a - knee;
        const float compressed = knee + range * (over / (over + range));
        return x < 0.0f ? -compressed : compressed;
    }

    float runChannel(Channel &c, float input) {
        float x = input;
        if (!isFinite(x)) x = 0.0f;

        x = c.inputFilter.process(x);
        x *= inputGain_;

        const float shaped = oversampledClip(c, x);

        // Bass and presence tilts around the 0.5 detent.
        float out = shaped;
        out += c.bassFilter.process(out) * bassTilt_;
        out += c.presenceFilter.process(out) * presenceTilt_;

        out = c.toneFilter.process(out);
        out = c.dcFilter.process(out);
        out *= outputGain_;
        out = softLimit(out);

        return isFinite(out) ? out : 0.0f;
    }

    // Windowed-sinc prototype, split into kL polyphase subfilters for
    // interpolation and normalised to unity DC gain for decimation.
    void rebuildOversampling() {
        const double pi = 3.14159265358979323846;
        double proto[kPrototypeLen];
        const double center = (kPrototypeLen - 1) * 0.5;
        double sum = 0.0;

        for (int n = 0; n < kPrototypeLen; ++n) {
            const double t = (n - center) / (double) kL;
            const double sinc = (std::fabs(t) < 1e-9) ? 1.0 : std::sin(pi * t) / (pi * t);
            const double window = 0.54 - 0.46 * std::cos(2.0 * pi * n / (kPrototypeLen - 1));
            proto[n] = sinc * window;
            sum += proto[n];
        }

        for (int n = 0; n < kPrototypeLen; ++n)
            decim_[n] = (float) (proto[n] / sum);

        for (int j = 0; j < kL; ++j) {
            double phaseSum = 0.0;
            for (int k = 0; k < kTapsPerPhase; ++k) phaseSum += proto[j + kL * k];
            const double scale = (std::fabs(phaseSum) < 1e-12) ? 0.0 : 1.0 / phaseSum;
            for (int k = 0; k < kTapsPerPhase; ++k)
                phases_[j][k] = (float) (proto[j + kL * k] * scale);
        }
    }

    // Upsamples x by 4, applies the waveshaper at the oversampled rate, then
    // decimates back to the base rate. One call consumes one input sample and
    // produces one output sample.
    float oversampledClip(Channel &c, float x) {
        for (int k = kTapsPerPhase - 1; k > 0; --k) c.upHistory[k] = c.upHistory[k - 1];
        c.upHistory[0] = x;

        float out = 0.0f;
        for (int j = 0; j < kL; ++j) {
            float acc = 0.0f;
            for (int k = 0; k < kTapsPerPhase; ++k) acc += phases_[j][k] * c.upHistory[k];

            const float u = waveshape(acc, hardnessSm_);

            for (int m = kPrototypeLen - 1; m > 0; --m) c.downHistory[m] = c.downHistory[m - 1];
            c.downHistory[0] = u;

            if (++c.decimPhase == kL) {
                c.decimPhase = 0;
                float acc2 = 0.0f;
                for (int m = 0; m < kPrototypeLen; ++m) acc2 += decim_[m] * c.downHistory[m];
                out = acc2;
            }
        }

        return out;
    }

    float sampleRate_ = 48000.0f;
    float gainSmoothCoef_ = 0.0f;
    float toneSmoothCoef_ = 0.0f;
    float inputGain_ = 1.0f;
    float outputGain_ = 1.0f;
    float cutoff_ = 6500.0f;
    float bassTilt_ = 0.0f;
    float presenceTilt_ = 0.0f;
    float hardnessSm_ = 0.7f;
    bool primed_ = false;

    float phases_[kL][kTapsPerPhase] = {};
    float decim_[kPrototypeLen] = {};
    Channel channels_[2];

    std::atomic<float> drive_{0.7f};
    std::atomic<float> tone_{0.6f};
    std::atomic<float> level_{0.65f};
    std::atomic<float> bass_{0.5f};
    std::atomic<float> presence_{0.5f};
    std::atomic<float> hardness_{0.7f};
    std::atomic<bool> enabled_{true};
};

#endif // ALTODFX_DISTORTION_H
