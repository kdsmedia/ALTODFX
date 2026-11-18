#ifndef ALTODFX_AUTO_WAH_H
#define ALTODFX_AUTO_WAH_H

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

// Envelope-following auto-wah for the ALTODFX rack.
//
// A resonant band-pass sweeps with the picking strength: an envelope follower
// tracks the input amplitude and its output drives the filter centre
// frequency, so hard picking opens the wah and a soft touch closes it.
//
// Adapted from the reference "AutoWah" DSP to this host:
//   * this class processes one channel. The host runs a stereo chain, so it
//     instantiates the effect twice (one per channel) and each channel keeps
//     its own envelope and filter state; the reference class instead processed
//     interleaved stereo in a single instance;
//   * the reference never configured its DC blockers (setSampleRate() left
//     them at the default coefficient), so the high-pass corner was wrong; the
//     blockers are now initialised for the real sample rate;
//   * the band-pass is retuned every 32 samples from a smoothed centre
//     frequency instead of on every sample, which removes both the per-sample
//     sin/cos cost and the zipper noise of a stepwise sweep.
//
// Nothing here allocates: all state is fixed-size and process() is safe to call
// from the audio thread.
class AutoWah {
public:
    explicit AutoWah(float sampleRate = 48000.0f) {
        setSampleRate(sampleRate);
        reset();
    }

    void setSampleRate(float sampleRate) {
        sampleRate_ = std::max(8000.0f, sampleRate);
        freqSmoothCoef_ = 1.0f - std::exp(-1.0f / (0.020f * sampleRate_));
        dcFilter_.setSampleRate(sampleRate_);
        reset();
    }

    // All parameters are 0.0 - 1.0.
    void setParameters(float sensitivity, float resonance, float depth,
                       float range, float attack, float release) {
        sensitivity_.store(clamp01(sensitivity), std::memory_order_relaxed);
        resonance_.store(clamp01(resonance), std::memory_order_relaxed);
        depth_.store(clamp01(depth), std::memory_order_relaxed);
        range_.store(clamp01(range), std::memory_order_relaxed);
        attack_.store(clamp01(attack), std::memory_order_relaxed);
        release_.store(clamp01(release), std::memory_order_relaxed);
    }

    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }

    void reset() {
        envelope_ = 0.0f;
        frequency_ = 800.0f;
        filter_.reset();
        dcFilter_.reset();
    }

    // In-place, mono: audio[0..numFrames-1].
    void process(float *audio, int32_t numFrames) {
        if (audio == nullptr || numFrames <= 0) return;
        if (!enabled_.load(std::memory_order_relaxed)) return;

        const float sensitivity = sensitivity_.load(std::memory_order_relaxed);
        const float resonance = resonance_.load(std::memory_order_relaxed);
        const float depth = depth_.load(std::memory_order_relaxed);
        const float range = range_.load(std::memory_order_relaxed);
        const float attack = attack_.load(std::memory_order_relaxed);
        const float release = release_.load(std::memory_order_relaxed);

        const float attackCoeff = timeCoefficient(1.0f + attack * 99.0f);
        const float releaseCoeff = timeCoefficient(20.0f + release * 480.0f);

        const float minFreq = 250.0f + range * 350.0f;
        const float maxFreq = 1200.0f + range * 2800.0f;
        const float q = 0.7f + resonance * 11.3f;
        const float detectorGain = 2.0f + sensitivity * 38.0f;
        const float sweepGain = 0.35f + sensitivity * 2.0f;

        for (int32_t i = 0; i < numFrames; ++i) {
            const float x = sanitize(audio[i]);

            const float target = clamp01(std::fabs(x) * detectorGain);
            const float coeff = target > envelope_ ? attackCoeff : releaseCoeff;
            envelope_ += (target - envelope_) * coeff;

            const float sweep = std::pow(clamp01(envelope_ * sweepGain), 0.75f);
            const float frequencyTarget = minFreq + (maxFreq - minFreq) * sweep * depth;
            frequency_ += freqSmoothCoef_ * (frequencyTarget - frequency_);

            // Retune the biquad at most every 32 samples: the centre frequency
            // is already smoothed, so this is inaudible and much cheaper.
            if ((i & 31) == 0) filter_.setBandPass(sampleRate_, frequency_, q);

            float out = filter_.process(x);
            out = dcFilter_.process(out);

            audio[i] = isFinite(out) ? std::tanh(out * 1.35f) : 0.0f;
        }
    }

private:
    static constexpr float PI = 3.14159265358979323846f;

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

    static float sanitize(float value) { return isFinite(value) ? value : 0.0f; }

    float timeCoefficient(float milliseconds) const {
        const float seconds = milliseconds * 0.001f;
        return 1.0f - std::exp(-1.0f / (seconds * sampleRate_));
    }

    struct Biquad {
        // RBJ band-pass, constant 0 dB peak gain.
        void setBandPass(float sampleRate, float frequency, float q) {
            frequency = std::max(20.0f, std::min(frequency, sampleRate * 0.40f));
            q = std::max(0.5f, std::min(q, 12.0f));

            const float omega = 2.0f * PI * frequency / sampleRate;
            const float sinW = std::sin(omega);
            const float cosW = std::cos(omega);
            const float alpha = sinW / (2.0f * q);
            const float a0 = 1.0f + alpha;

            b0 = alpha / a0;
            b1 = 0.0f;
            b2 = -alpha / a0;
            a1 = (-2.0f * cosW) / a0;
            a2 = (1.0f - alpha) / a0;
        }

        float process(float x) {
            const float output = b0 * x + z1;
            z1 = b1 * x - a1 * output + z2;
            z2 = b2 * x - a2 * output;

            if (!isFinite(output) || !isFinite(z1) || !isFinite(z2)) {
                reset();
                return 0.0f;
            }

            return output;
        }

        void reset() {
            z1 = 0.0f;
            z2 = 0.0f;
        }

        float b0 = 0.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f, z1 = 0.0f, z2 = 0.0f;
    };

    struct DCFilter {
        void setSampleRate(float sampleRate) {
            const float cutoff = 20.0f;
            const float rc = 1.0f / (2.0f * PI * cutoff);
            const float dt = 1.0f / sampleRate;
            coefficient_ = rc / (rc + dt);
        }

        float process(float x) {
            const float y = coefficient_ * (lastY_ + x - lastX_);
            lastX_ = x;
            lastY_ = isFinite(y) ? y : 0.0f;
            return lastY_;
        }

        void reset() {
            lastX_ = 0.0f;
            lastY_ = 0.0f;
        }

        float coefficient_ = 0.99f;
        float lastX_ = 0.0f;
        float lastY_ = 0.0f;
    };

    float sampleRate_ = 48000.0f;
    float envelope_ = 0.0f;
    float frequency_ = 800.0f;
    float freqSmoothCoef_ = 0.0f;

    Biquad filter_;
    DCFilter dcFilter_;

    std::atomic<float> sensitivity_{0.5f};
    std::atomic<float> resonance_{0.55f};
    std::atomic<float> depth_{0.85f};
    std::atomic<float> range_{0.5f};
    std::atomic<float> attack_{0.2f};
    std::atomic<float> release_{0.4f};
    std::atomic<bool> enabled_{true};
};

#endif // ALTODFX_AUTO_WAH_H
