#ifndef ALTODFX_FUZZ_H
#define ALTODFX_FUZZ_H

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

// Thick, aggressive fuzz for the ALTODFX rack: long sustain, vintage through
// modern character.
//
// Based on the reference "Fuzz" DSP, hardened for this host:
//   * the reference shaped the signal with no oversampling at all, so the hard
//     clipping harmonics folded back into the audible band as aliasing. The
//     waveshaper now runs inside a polyphase windowed-sinc 4x oversampler (the
//     design already proven in TubeDrive / Distortion).
//   * Character switched transfer functions at 0.5, which steps the curve; it
//     now crossfades between the vintage and modern curves.
//   * the reference added a bias offset and then removed a fixed fraction of it
//     (biasOffset * 0.15), which never matched the DC it created and, once the DC
//     blocker ran, mostly just gated the signal. Bias now sets the shaper's
//     asymmetry directly (one half clips before the other), which gives the
//     gated / octave-ish character without depending on residual DC.
//   * Sustain now drives the shaper harder and compensates the level it adds, so
//     it squares off the note (which is what holds a long fuzz sustain) instead
//     of simply getting louder.
//   * the final tanh() saturated the whole signal and flattened Tone/Bias; it is
//     replaced by a soft-limiter that is transparent below the knee.
//   * parameters are smoothed with one-pole filters, so turning a knob while
//     playing does not click, and the NaN/Inf guards inspect the bit pattern so
//     they survive -ffast-math.
//
// The class is stereo with independent per-channel state and never allocates,
// so process() is safe from the audio thread.
class Fuzz {
public:
    explicit Fuzz(float sampleRate = 48000.0f) {
        rebuildOversampling();
        setSampleRate(sampleRate);
        reset();
    }

    void setSampleRate(float sampleRate) {
        sampleRate_ = std::max(8000.0f, sampleRate);
        smoothCoef_ = 1.0f - std::exp(-1.0f / (0.010f * sampleRate_));
        toneSmoothCoef_ = 1.0f - std::exp(-1.0f / (0.020f * sampleRate_));

        for (int ch = 0; ch < 2; ++ch) {
            channels_[ch].inputFilter.setHighPass(sampleRate_, 35.0f);
            channels_[ch].toneFilter.setLowPass(sampleRate_, 6500.0f);
            channels_[ch].dcFilter.setHighPass(sampleRate_, 12.0f);
        }
        reset();
    }

    // All parameters are 0.0 - 1.0.
    void setParameters(float fuzz, float tone, float sustain,
                       float bias, float level, float character) {
        fuzz_.store(clamp01(fuzz), std::memory_order_relaxed);
        tone_.store(clamp01(tone), std::memory_order_relaxed);
        sustain_.store(clamp01(sustain), std::memory_order_relaxed);
        bias_.store(clamp01(bias), std::memory_order_relaxed);
        level_.store(clamp01(level), std::memory_order_relaxed);
        character_.store(clamp01(character), std::memory_order_relaxed);
    }

    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }

    void reset() {
        for (int ch = 0; ch < 2; ++ch) {
            Channel &c = channels_[ch];
            c.inputFilter.reset();
            c.toneFilter.reset();
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

        const float fuzzTarget = fuzz_.load(std::memory_order_relaxed);
        const float toneTarget = tone_.load(std::memory_order_relaxed);
        const float sustainTarget = sustain_.load(std::memory_order_relaxed);
        const float biasTarget = bias_.load(std::memory_order_relaxed);
        const float levelTarget = level_.load(std::memory_order_relaxed);
        const float characterTarget = character_.load(std::memory_order_relaxed);

        const float driveTarget = 1.0f + fuzzTarget * 14.0f;
        const float sustainGainTarget = 1.0f + sustainTarget * 8.0f;
        const float toneCutoffTarget = 900.0f + toneTarget * 8500.0f;
        const float outputGainTarget = 0.12f + levelTarget * 0.80f;
        // Bias is an asymmetry amount: 0 at the 0.5 detent (symmetric), toward
        // +1/-1 one half of the shaper clips before the other.
        const float biasAsymTarget = (biasTarget - 0.5f) * 2.0f;

        // Jump straight to the requested values on the first block so a freshly
        // loaded patch does not fade in; smooth only subsequent changes.
        if (!primed_) {
            drive_ = driveTarget;
            sustainGain_ = sustainGainTarget;
            biasAsym_ = biasAsymTarget;
            toneCutoff_ = toneCutoffTarget;
            outputGain_ = outputGainTarget;
            characterSm_ = characterTarget;
            for (int ch = 0; ch < 2; ++ch)
                channels_[ch].toneFilter.setLowPass(sampleRate_, toneCutoff_);
            primed_ = true;
        }

        for (int32_t i = 0; i < numFrames; ++i) {
            drive_ += smoothCoef_ * (driveTarget - drive_);
            sustainGain_ += smoothCoef_ * (sustainGainTarget - sustainGain_);
            biasAsym_ += smoothCoef_ * (biasAsymTarget - biasAsym_);
            outputGain_ += smoothCoef_ * (outputGainTarget - outputGain_);
            characterSm_ += smoothCoef_ * (characterTarget - characterSm_);
            toneCutoff_ += toneSmoothCoef_ * (toneCutoffTarget - toneCutoff_);

            if ((i & 31) == 0) {
                for (int ch = 0; ch < 2; ++ch)
                    channels_[ch].toneFilter.setLowPass(sampleRate_, toneCutoff_);
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

    // Vintage: asymmetric transistor-like curve, softer on the negative swing.
    // `asym` biases the two halves: positive when asym > 0, negative when < 0.
    static float vintageShape(float x, float asym) {
        // The 0.72 factor is the transistor-like asymmetry the vintage fuzz has
        // on its own; Bias then shifts how far the two halves diverge.
        const float posGain = 1.0f + asym * 0.5f;
        const float negGain = 0.72f - asym * 0.5f;
        if (x >= 0.0f) return std::tanh(x * posGain);
        return -std::tanh(-x * negGain);
    }

    // Modern: aggressive, near-square clip with a soft transition near zero.
    static float modernShape(float x, float asym) {
        const float posGain = 1.0f + asym * 0.8f;
        const float negGain = 1.0f - asym * 0.8f;
        const float value = x * 2.8f * (x >= 0.0f ? posGain : negGain);
        if (value > 1.0f) return 1.0f;
        if (value < -1.0f) return -1.0f;
        return std::tanh(value * 2.0f) / std::tanh(2.0f);
    }

    // Crossfades between the vintage and modern curves so changing Character
    // never steps the transfer function.
    static float shape(float x, float character, float asym) {
        const float vintage = vintageShape(x, asym);
        const float modern = modernShape(x, asym);
        return vintage + (modern - vintage) * character;
    }

    // Output safety: transparent below the knee, compresses the excess smoothly
    // toward 1.0. A plain tanh() here would flatten the Tone/Bias response.
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

        // Sustain adds gain into the shaper. Because the shaper saturates, a
        // loud note barely changes while a decaying note is pushed back into
        // clipping, which is what holds the tail.
        x *= drive_ * sustainGain_;

        // Bias is a shaper asymmetry (see shape()); it needs no DC handling
        // because it never adds a DC offset to the signal.
        const float shaped = oversampledShape(c, x);

        float out = c.toneFilter.process(shaped);
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
    float oversampledShape(Channel &c, float x) {
        for (int k = kTapsPerPhase - 1; k > 0; --k) c.upHistory[k] = c.upHistory[k - 1];
        c.upHistory[0] = x;

        float out = 0.0f;
        for (int j = 0; j < kL; ++j) {
            float acc = 0.0f;
            for (int k = 0; k < kTapsPerPhase; ++k) acc += phases_[j][k] * c.upHistory[k];

            const float u = shape(acc, characterSm_, biasAsym_);

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
    float smoothCoef_ = 0.0f;
    float toneSmoothCoef_ = 0.0f;
    float drive_ = 1.0f;
    float sustainGain_ = 1.0f;
    float biasAsym_ = 0.0f;
    float toneCutoff_ = 6500.0f;
    float outputGain_ = 1.0f;
    float characterSm_ = 0.25f;
    bool primed_ = false;

    float phases_[kL][kTapsPerPhase] = {};
    float decim_[kPrototypeLen] = {};
    Channel channels_[2];

    std::atomic<float> fuzz_{0.75f};
    std::atomic<float> tone_{0.55f};
    std::atomic<float> sustain_{0.7f};
    std::atomic<float> bias_{0.5f};
    std::atomic<float> level_{0.65f};
    std::atomic<float> character_{0.25f};
    std::atomic<bool> enabled_{true};
};

#endif // ALTODFX_FUZZ_H
