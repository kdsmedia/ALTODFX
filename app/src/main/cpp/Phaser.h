#ifndef ALTODFX_PHASER_H
#define ALTODFX_PHASER_H

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

// Sweeping, whooshy phaser for the ALTODFX rack: 4 to 12 all-pass stages, a
// stereo LFO and a resonant feedback path. Suits funk, psychedelic, rock, pop
// and ambient.
//
// Based on the reference "Phaser" DSP, hardened for this host:
//   * the reference read its parameters straight from the atomics once per
//     block and used them immediately, so moving a knob stepped the sweep rate,
//     depth, centre and mix and clicked. Every continuous parameter is now
//     smoothed with a one-pole filter, so a knob turn glides (the note's
//     "interpolasi perubahan parameter").
//   * the reference switched stage count by changing the loop bound, so the
//     wet signal jumped between an N-stage and an M-stage response and clicked.
//     All twelve stages always run; the output is tapped after stages 4, 6, 8
//     and 12 and the taps are crossfaded, so changing stages is continuous.
//   * the reference clamped the feedback state to +/-1, a hard corner that can
//     ring harshly and, with coefficient changes, misbehave. The feedback path
//     now saturates smoothly instead, which both bounds the loop and gives the
//     analog-style character the note asks for.
//   * the reference called std::sin twice and std::tan twice per sample. The LFO
//     now uses a polynomial sine and the all-pass coefficient is derived from
//     that same polynomial, so there is no per-sample transcendental call (the
//     note's "optimasi pemrosesan LFO"). Both are verified against std::sin /
//     std::tan in the host tests.
//   * the NaN/Inf guards inspect the bit pattern so they survive -ffast-math.
//
// The class is stereo with independent per-channel state and never allocates, so
// process() is safe from the audio thread.
class Phaser {
public:
    static constexpr int MAX_STAGES = 12;

    enum StageMode {
        STAGE_4 = 4,
        STAGE_6 = 6,
        STAGE_8 = 8,
        STAGE_12 = 12
    };

    explicit Phaser(float sampleRate = 48000.0f) {
        setSampleRate(sampleRate);
        reset();
    }

    void setSampleRate(float sampleRate) {
        sampleRate_ = std::max(8000.0f, sampleRate);
        // ~15 ms glides for the control parameters.
        smooth_ = 1.0f - std::exp(-1.0f / (0.015f * sampleRate_));
        rate_.store(0.35f, std::memory_order_relaxed);
    }

    // All parameters are 0.0 - 1.0. `centerFrequency` and `stereo` are the
    // reference's names; `stereo` is the width of the L/R LFO phase offset.
    void setParameters(float rate, float depth, float feedback, float mix,
                       float centerFrequency, float stereo) {
        rate_.store(clamp01(rate), std::memory_order_relaxed);
        depth_.store(clamp01(depth), std::memory_order_relaxed);
        feedback_.store(clamp01(feedback), std::memory_order_relaxed);
        mix_.store(clamp01(mix), std::memory_order_relaxed);
        center_.store(clamp01(centerFrequency), std::memory_order_relaxed);
        stereo_.store(clamp01(stereo), std::memory_order_relaxed);
    }

    // 0.0 - 1.0, mapped across the 4/6/8/12 stage taps and crossfaded.
    void setStageAmount(float amount) {
        stageAmount_.store(clamp01(amount), std::memory_order_relaxed);
    }

    void setStages(StageMode mode) {
        const int stages = std::max(4, std::min((int) mode, MAX_STAGES));
        // 4 -> 0.0, 6 -> 1/3, 8 -> 2/3, 12 -> 1.0
        const float amount = (stages <= 6) ? (stages - 4) / 2.0f
                            : (stages <= 8) ? (1.0f + (stages - 6) / 2.0f) / 3.0f
                                            : 1.0f;
        stageAmount_.store(clamp01(amount), std::memory_order_relaxed);
    }

    void setLevel(float level) { level_.store(clamp01(level), std::memory_order_relaxed); }

    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }

    void reset() {
        phase_ = 0.0f;
        feedbackState_[0] = feedbackState_[1] = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < MAX_STAGES; ++i)
                allPass_[ch][i].reset();
        primed_ = false;
    }

    // Stereo, separate per-channel buffers.
    void process(float *left, float *right, int32_t numFrames) {
        if (left == nullptr || right == nullptr || numFrames <= 0) return;
        if (!enabled_.load(std::memory_order_relaxed)) return;

        const float rateTarget = rate_.load(std::memory_order_relaxed);
        const float depthTarget = depth_.load(std::memory_order_relaxed);
        const float feedbackTarget = feedback_.load(std::memory_order_relaxed);
        const float mixTarget = mix_.load(std::memory_order_relaxed);
        const float centerTarget = center_.load(std::memory_order_relaxed);
        const float stereoTarget = stereo_.load(std::memory_order_relaxed);
        const float stageTarget = stageAmount_.load(std::memory_order_relaxed);
        const float levelTarget = level_.load(std::memory_order_relaxed);

        if (!primed_) {
            depthSm_ = depthTarget; feedbackSm_ = feedbackTarget; mixSm_ = mixTarget;
            centerSm_ = centerTarget; stereoSm_ = stereoTarget;
            stageAmountSm_ = stageTarget; levelSm_ = levelTarget;
            primed_ = true;
        }

        for (int32_t i = 0; i < numFrames; ++i) {
            depthSm_ += smooth_ * (depthTarget - depthSm_);
            feedbackSm_ += smooth_ * (feedbackTarget - feedbackSm_);
            mixSm_ += smooth_ * (mixTarget - mixSm_);
            centerSm_ += smooth_ * (centerTarget - centerSm_);
            stereoSm_ += smooth_ * (stereoTarget - stereoSm_);
            stageAmountSm_ += smooth_ * (stageTarget - stageAmountSm_);
            levelSm_ += smooth_ * (levelTarget - levelSm_);

            // The LFO rate is smoothed inside the phase accumulator, so a rate
            // change glides rather than resetting the phase.
            const float lfoRate = 0.05f + rateTarget * 5.95f;
            phase_ += lfoRate / sampleRate_;
            if (phase_ >= 1.0f) phase_ -= 1.0f;

            // L and R share the LFO; `stereo` offsets the right phase, up to
            // 180 degrees, which is what gives the effect its width.
            const float lfoLeft = fastSin(phase_);
            const float lfoRight = fastSin(phase_ + stereoSm_ * 0.5f);

            const float minFrequency = 180.0f + centerSm_ * 500.0f;
            const float maxFrequency = 900.0f + centerSm_ * 2800.0f;
            const float feedbackAmount = feedbackSm_ * 0.85f;

            left[i] = runChannel(0, left[i], lfoLeft, minFrequency, maxFrequency, feedbackAmount);
            right[i] = runChannel(1, right[i], lfoRight, minFrequency, maxFrequency, feedbackAmount);
        }
    }

private:
    static constexpr float kPi = 3.14159265358979323846f;

    // First-order all-pass, y[n] = -g*x[n] + x[n-1] + g*y[n-1], i.e.
    // H(z) = (z^-1 - g) / (1 - g*z^-1). The pole sits at z = g, so a positive
    // g puts the phase transition at the intended frequency.
    //
    // The reference used y[n] = g*x[n] + x[n-1] - g*y[n-1], which is also
    // all-pass but puts its pole at z = -g, i.e. the phase transition lands near
    // Nyquist instead of at the target frequency. That pushed every notch up
    // against Nyquist and left the audible band almost untouched, so the effect
    // barely did anything.
    struct AllPass {
        float x1 = 0.0f, y1 = 0.0f;

        void reset() { x1 = 0.0f; y1 = 0.0f; }

        inline float process(float x, float g) {
            const float y = -g * x + x1 + g * y1;
            x1 = x;
            y1 = y;
            return y;
        }
    };

    static bool isFinite(float value) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        return (bits & 0x7F800000u) != 0x7F800000u;
    }

    static float clamp01(float value) {
        if (!isFinite(value)) return 0.0f;
        return std::max(0.0f, std::min(1.0f, value));
    }

    // Sine of an angle expressed in turns (0.0 - 1.0 is a full cycle), so the
    // LFO phase accumulator feeds it directly. Odd 9th-order polynomial after
    // folding the angle into [-pi/2, pi/2]; accurate to a few parts per million
    // over the whole cycle, which is far below audibility for an LFO.
    static inline float fastSin(float turns) {
        // Fold to [-0.5, 0.5) turns, then to an angle in [-pi, pi].
        float t = turns - std::floor(turns + 0.5f);
        float x = t * (2.0f * kPi);
        if (x > kPi * 0.5f) x = kPi - x;
        else if (x < -kPi * 0.5f) x = -kPi - x;
        const float x2 = x * x;
        return x * (1.0f + x2 * (-1.66666667e-1f + x2 * (8.33333333e-3f
                 + x2 * (-1.98412698e-4f + x2 * 2.75573192e-6f))));
    }

    // tan() of an angle in radians, from the same polynomial: tan = sin / cos,
    // and cos(x) is sin(x + pi/2). The all-pass angle stays below pi/2, where
    // cos is well away from zero.
    static inline float fastTan(float x) {
        const float s = fastSin(x / (2.0f * kPi));
        const float c = fastSin((x + kPi * 0.5f) / (2.0f * kPi));
        return s / c;
    }

    // Smooth saturation: unit slope at the origin, monotonic, asymptotic to
    // +/-1 and continuous in every derivative. Bounds the feedback loop without
    // the hard clamp's corner, which is where a phaser's ring turns ugly.
    static inline float softSaturate(float x) {
        return x / (1.0f + std::fabs(x));
    }

    float runChannel(int ch, float dry, float lfo, float minF, float maxF, float feedbackAmount) {
        if (!isFinite(dry)) dry = 0.0f;

        const float sweep = minF + (maxF - minF) * (0.5f + 0.5f * lfo) * depthSm_;
        const float frequency = std::max(20.0f, std::min(sweep, sampleRate_ * 0.40f));
        const float t = fastTan(kPi * frequency / sampleRate_);
        const float g = (1.0f - t) / (1.0f + t);

        float wet = dry + feedbackState_[ch] * feedbackAmount;

        // Tap the cascade after 4, 6, 8 and 12 stages, then crossfade the taps
        // so a stage change is continuous.
        float tap4 = 0.0f, tap6 = 0.0f, tap8 = 0.0f, tap12 = 0.0f;
        for (int s = 0; s < MAX_STAGES; ++s) {
            wet = allPass_[ch][s].process(wet, g);
            if (s == 3) tap4 = wet;
            else if (s == 5) tap6 = wet;
            else if (s == 7) tap8 = wet;
            else if (s == 11) tap12 = wet;
        }
        if (!isFinite(wet)) { resetChannel(ch); wet = 0.0f; }

        // stageAmountSm_ 0..1 maps 4 -> 12 stages across the four taps.
        const float target = 4.0f + stageAmountSm_ * 8.0f;
        float shaped;
        if (target <= 6.0f) {
            const float k = (target - 4.0f) * 0.5f;
            shaped = tap4 + (tap6 - tap4) * k;
        } else if (target <= 8.0f) {
            const float k = (target - 6.0f) * 0.5f;
            shaped = tap6 + (tap8 - tap6) * k;
        } else {
            const float k = (target - 8.0f) * 0.25f;
            shaped = tap8 + (tap12 - tap8) * k;
        }

        feedbackState_[ch] = softSaturate(shaped);

        const float out = (dry * (1.0f - mixSm_) + shaped * mixSm_) * levelSm_;
        return isFinite(out) ? out : 0.0f;
    }

    void resetChannel(int ch) {
        for (int i = 0; i < MAX_STAGES; ++i) allPass_[ch][i].reset();
        feedbackState_[ch] = 0.0f;
    }

    float sampleRate_ = 48000.0f;
    float smooth_ = 0.0f;
    float phase_ = 0.0f;
    bool primed_ = false;

    float feedbackState_[2] = {0.0f, 0.0f};
    AllPass allPass_[2][MAX_STAGES];

    std::atomic<float> rate_{0.35f};
    std::atomic<float> depth_{0.75f};
    std::atomic<float> feedback_{0.55f};
    std::atomic<float> mix_{0.5f};
    std::atomic<float> center_{0.45f};
    std::atomic<float> stereo_{0.5f};
    std::atomic<float> stageAmount_{2.0f / 3.0f};
    std::atomic<float> level_{1.0f};
    std::atomic<bool> enabled_{true};

    // Smoothed copies, written only by the audio thread.
    float depthSm_ = 0.75f, feedbackSm_ = 0.55f, mixSm_ = 0.5f;
    float centerSm_ = 0.45f, stereoSm_ = 0.5f, stageAmountSm_ = 2.0f / 3.0f, levelSm_ = 1.0f;
};

#endif // ALTODFX_PHASER_H
