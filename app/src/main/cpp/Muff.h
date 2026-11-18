#ifndef ALTODFX_MUFF_H
#define ALTODFX_MUFF_H

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

// Thick, long-sustain Muff for the ALTODFX rack: strong low end, smooth top,
// soft cascaded distortion. Suits lead, solo and psychedelic / alternative rock.
//
// Based on the reference "Muff" DSP, hardened for this host:
//   * the reference ran three cascaded tanh clipping stages with no oversampling
//     at all, so the clipping harmonics folded back into the audible band. The
//     whole gain/clip chain now runs inside a polyphase windowed-sinc 4x
//     oversampler (the design already proven in TubeDrive / Fuzz / Distortion).
//   * the reference's tone control only crossfaded a lowpass and a highpass at a
//     fixed 1 kHz split, and its updateTone() ignored the tone argument entirely.
//     The split point now tracks the knob (and is smoothed), so the control is
//     genuinely responsive instead of being a fixed-frequency blend.
//   * Bass and Treble were a cut-only highpass and lowpass. They are now low and
//     high tilt shelves, so they cut below the 0.5 detent and boost above it.
//   * the reference's two inter-stage filters (clipFilter1_/2_) were declared but
//     never given coefficients, so they were silently bypassed. They are now the
//     gentle inter-stage rolloff that gives the cascaded stages their smooth,
//     "high-end halus" character.
//   * the final tanh() saturated the whole signal and flattened the tone stack;
//     it is replaced by a soft-limiter that is transparent below the knee.
//   * parameters are smoothed with one-pole filters, so turning a knob while
//     playing does not click, and the NaN/Inf guards inspect the bit pattern so
//     they survive -ffast-math.
//
// The class is stereo with independent per-channel state and never allocates, so
// process() is safe from the audio thread.
class Muff {
public:
    explicit Muff(float sampleRate = 48000.0f) {
        rebuildOversampling();
        setSampleRate(sampleRate);
        reset();
    }

    void setSampleRate(float sampleRate) {
        sampleRate_ = std::max(8000.0f, sampleRate);
        smoothCoef_ = 1.0f - std::exp(-1.0f / (0.010f * sampleRate_));
        toneSmoothCoef_ = 1.0f - std::exp(-1.0f / (0.020f * sampleRate_));

        // The inter-stage filters live inside the 4x oversampled chain, so their
        // cutoffs are expressed against the oversampled rate.
        const float osRate = sampleRate_ * (float) kL;
        for (int ch = 0; ch < 2; ++ch) {
            Channel &c = channels_[ch];
            c.inputFilter.setHighPass(sampleRate_, 35.0f);
            c.clipFilter1.setLowPass(osRate, 8000.0f);
            c.clipFilter2.setLowPass(osRate, 6000.0f);
            c.dcFilter.setHighPass(sampleRate_, 15.0f);
            c.toneLow.setLowPass(sampleRate_, 1000.0f);
            c.toneHigh.setHighPass(sampleRate_, 1000.0f);
        }
        reset();
    }

    // All parameters are 0.0 - 1.0.
    void setParameters(float sustain, float tone, float volume,
                       float bass, float treble) {
        sustain_.store(clamp01(sustain), std::memory_order_relaxed);
        tone_.store(clamp01(tone), std::memory_order_relaxed);
        volume_.store(clamp01(volume), std::memory_order_relaxed);
        bass_.store(clamp01(bass), std::memory_order_relaxed);
        treble_.store(clamp01(treble), std::memory_order_relaxed);
    }

    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }

    void reset() {
        for (int ch = 0; ch < 2; ++ch) {
            Channel &c = channels_[ch];
            c.inputFilter.reset();
            c.clipFilter1.reset();
            c.clipFilter2.reset();
            c.toneLow.reset();
            c.toneHigh.reset();
            c.bassShelf.reset();
            c.trebleShelf.reset();
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

        const float sustainTarget = sustain_.load(std::memory_order_relaxed);
        const float toneTarget = tone_.load(std::memory_order_relaxed);
        const float volumeTarget = volume_.load(std::memory_order_relaxed);
        const float bassTarget = bass_.load(std::memory_order_relaxed);
        const float trebleTarget = treble_.load(std::memory_order_relaxed);

        const float gainTarget = 1.0f + sustainTarget * 23.0f;
        const float toneMixTarget = toneTarget;
        // The tone split moves with the knob so the control has real range.
        const float toneCornerTarget = 700.0f + toneTarget * 900.0f;
        const float outputGainTarget = volumeTarget * 1.9f;
        // Tilts are 0 dB at the 0.5 detent, cut below and boost above.
        const float bassDbTarget = (bassTarget - 0.5f) * 24.0f;
        const float trebleDbTarget = (trebleTarget - 0.5f) * 24.0f;

        if (!primed_) {
            gain_ = gainTarget;
            toneMix_ = toneMixTarget;
            toneCorner_ = toneCornerTarget;
            outputGain_ = outputGainTarget;
            bassDb_ = bassDbTarget;
            trebleDb_ = trebleDbTarget;
            updateFilters(toneCorner_, bassDb_, trebleDb_);
            primed_ = true;
        }

        for (int32_t i = 0; i < numFrames; ++i) {
            gain_ += smoothCoef_ * (gainTarget - gain_);
            toneMix_ += smoothCoef_ * (toneMixTarget - toneMix_);
            outputGain_ += smoothCoef_ * (outputGainTarget - outputGain_);
            bassDb_ += smoothCoef_ * (bassDbTarget - bassDb_);
            trebleDb_ += smoothCoef_ * (trebleDbTarget - trebleDb_);
            toneCorner_ += toneSmoothCoef_ * (toneCornerTarget - toneCorner_);

            if ((i & 31) == 0) updateFilters(toneCorner_, bassDb_, trebleDb_);

            left[i] = runChannel(channels_[0], left[i]);
            right[i] = runChannel(channels_[1], right[i]);
        }
    }

private:
    static constexpr int kL = 4;              // 4x oversampling
    static constexpr int kTapsPerPhase = 16;
    static constexpr int kPrototypeLen = kL * kTapsPerPhase;
    static constexpr float kPi = 3.14159265358979323846f;

    struct Biquad {
        void setLowPass(float sampleRate, float cutoff) {
            const float w = 2.0f * kPi * std::min(cutoff, sampleRate * 0.45f) / sampleRate;
            const float c = std::cos(w);
            const float alpha = std::sin(w) * 0.70710678f;
            const float a0 = 1.0f + alpha;
            set(((1.0f - c) * 0.5f) / a0, (1.0f - c) / a0, ((1.0f - c) * 0.5f) / a0,
                (-2.0f * c) / a0, (1.0f - alpha) / a0);
        }

        void setHighPass(float sampleRate, float cutoff) {
            const float w = 2.0f * kPi * std::min(cutoff, sampleRate * 0.45f) / sampleRate;
            const float c = std::cos(w);
            const float alpha = std::sin(w) * 0.70710678f;
            const float a0 = 1.0f + alpha;
            set(((1.0f + c) * 0.5f) / a0, -(1.0f + c) / a0, ((1.0f + c) * 0.5f) / a0,
                (-2.0f * c) / a0, (1.0f - alpha) / a0);
        }

        // RBJ low shelf; gainDb > 0 boosts the band below `corner`.
        void setLowShelf(float sampleRate, float corner, float gainDb) {
            const float a = std::pow(10.0f, gainDb / 40.0f);
            const float w = 2.0f * kPi * std::min(corner, sampleRate * 0.45f) / sampleRate;
            const float c = std::cos(w);
            const float s = std::sin(w);
            const float alpha = s * 0.5f * 1.41421356f;  // S = 1
            const float sqrtA2alpha = 2.0f * std::sqrt(a) * alpha;
            const float a0 = (a + 1.0f) + (a - 1.0f) * c + sqrtA2alpha;
            set(a * ((a + 1.0f) - (a - 1.0f) * c + sqrtA2alpha) / a0,
                2.0f * a * ((a - 1.0f) - (a + 1.0f) * c) / a0,
                a * ((a + 1.0f) - (a - 1.0f) * c - sqrtA2alpha) / a0,
                -2.0f * ((a - 1.0f) + (a + 1.0f) * c) / a0,
                ((a + 1.0f) + (a - 1.0f) * c - sqrtA2alpha) / a0);
        }

        // RBJ high shelf; gainDb > 0 boosts the band above `corner`.
        void setHighShelf(float sampleRate, float corner, float gainDb) {
            const float a = std::pow(10.0f, gainDb / 40.0f);
            const float w = 2.0f * kPi * std::min(corner, sampleRate * 0.45f) / sampleRate;
            const float c = std::cos(w);
            const float s = std::sin(w);
            const float alpha = s * 0.5f * 1.41421356f;  // S = 1
            const float sqrtA2alpha = 2.0f * std::sqrt(a) * alpha;
            const float a0 = (a + 1.0f) - (a - 1.0f) * c + sqrtA2alpha;
            set(a * ((a + 1.0f) + (a - 1.0f) * c + sqrtA2alpha) / a0,
                -2.0f * a * ((a - 1.0f) + (a + 1.0f) * c) / a0,
                a * ((a + 1.0f) + (a - 1.0f) * c - sqrtA2alpha) / a0,
                2.0f * ((a - 1.0f) - (a + 1.0f) * c) / a0,
                ((a + 1.0f) - (a - 1.0f) * c - sqrtA2alpha) / a0);
        }

        float process(float x) {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return isFinite(y) ? y : 0.0f;
        }

        void reset() { z1 = 0.0f; z2 = 0.0f; }

    private:
        void set(float nb0, float nb1, float nb2, float na1, float na2) {
            b0 = nb0; b1 = nb1; b2 = nb2; a1 = na1; a2 = na2;
        }

        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f, z1 = 0.0f, z2 = 0.0f;
    };

    struct Channel {
        Biquad inputFilter;
        Biquad clipFilter1;
        Biquad clipFilter2;
        Biquad toneLow;
        Biquad toneHigh;
        Biquad bassShelf;
        Biquad trebleShelf;
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

    // Output safety: transparent below the knee, compresses the excess smoothly
    // toward 1.0. A plain tanh() here would flatten the tone stack.
    static float softLimit(float x) {
        const float knee = 0.85f;
        const float a = std::fabs(x);
        if (a <= knee) return x;
        const float range = 1.0f - knee;
        const float over = a - knee;
        const float compressed = knee + range * (over / (over + range));
        return x < 0.0f ? -compressed : compressed;
    }

    // The cascaded gain/clip chain. Runs at the oversampled rate.
    //
    // The inter-stage gains stay near unity on purpose. The reference used 2.8x
    // and 1.8x, which drove the later stages into full saturation no matter what
    // Sustain was set to, so the knob stopped doing anything above about a
    // quarter turn. With near-unity inter-stage gains the first stage's drive is
    // what sets the distortion, so Sustain sweeps from nearly clean to fully
    // saturated.
    static float clipChain(Channel &c, float x, float gain) {
        x *= gain * 0.55f;
        x = std::tanh(x * 1.6f);          // stage one: silicon-like
        x = c.clipFilter1.process(x);

        x *= 1.2f;
        x = std::tanh(x * 1.8f);          // stage two: stronger, for sustain
        x = c.clipFilter2.process(x);

        x *= 1.1f;
        return std::tanh(x * 1.2f);       // final saturation
    }

    void updateFilters(float toneCorner, float bassDb, float trebleDb) {
        for (int ch = 0; ch < 2; ++ch) {
            Channel &c = channels_[ch];
            c.toneLow.setLowPass(sampleRate_, toneCorner);
            c.toneHigh.setHighPass(sampleRate_, toneCorner);
            c.bassShelf.setLowShelf(sampleRate_, 120.0f, bassDb);
            c.trebleShelf.setHighShelf(sampleRate_, 3000.0f, trebleDb);
        }
    }

    float runChannel(Channel &c, float input) {
        float x = input;
        if (!isFinite(x)) x = 0.0f;

        x = c.inputFilter.process(x);
        x = oversampledClip(c, x);

        // Classic Muff tone control: blend the low and high halves of the split.
        const float low = c.toneLow.process(x);
        const float high = c.toneHigh.process(x);
        x = low + (high - low) * toneMix_;

        x = c.bassShelf.process(x);
        x = c.trebleShelf.process(x);
        x = c.dcFilter.process(x);
        x *= outputGain_;

        return softLimit(x);
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

    // Upsamples x by 4, runs the clip chain at the oversampled rate, then
    // decimates back to the base rate. One call consumes one input sample and
    // produces one output sample.
    float oversampledClip(Channel &c, float x) {
        for (int k = kTapsPerPhase - 1; k > 0; --k) c.upHistory[k] = c.upHistory[k - 1];
        c.upHistory[0] = x;

        float out = 0.0f;
        for (int j = 0; j < kL; ++j) {
            float acc = 0.0f;
            for (int k = 0; k < kTapsPerPhase; ++k) acc += phases_[j][k] * c.upHistory[k];

            const float u = clipChain(c, acc, gain_);

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
    float gain_ = 1.0f;
    float toneMix_ = 0.5f;
    float toneCorner_ = 1000.0f;
    float outputGain_ = 0.7f;
    float bassDb_ = 0.0f;
    float trebleDb_ = 0.0f;
    bool primed_ = false;

    float phases_[kL][kTapsPerPhase] = {};
    float decim_[kPrototypeLen] = {};
    Channel channels_[2];

    std::atomic<float> sustain_{0.75f};
    std::atomic<float> tone_{0.5f};
    std::atomic<float> volume_{0.7f};
    std::atomic<float> bass_{0.6f};
    std::atomic<float> treble_{0.5f};
    std::atomic<bool> enabled_{true};
};

#endif // ALTODFX_MUFF_H
