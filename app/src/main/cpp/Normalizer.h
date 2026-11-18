#ifndef ALTODFX_NORMALIZER_H
#define ALTODFX_NORMALIZER_H

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

// Level normalizer for the ALTODFX rack: brings the signal to a target level with
// an automatic gain control, an optional noise gate, and a true-peak limiter that
// catches the peaks the gain control cannot.
//
// Based on the reference "Normalizer" DSP. The reference's note said a peak-based
// AGC should become loudness/RMS-aware leveling with lookahead, a true-peak
// limiter, and safe parameter smoothing between the UI and the audio thread. All
// four are addressed here:
//
//   * the level is measured as RMS, not peak. The reference derived the gain from
//     a peak envelope, so a single transient set the gain for the whole release
//     and the level drifted with the crest factor. The AGC now tracks the RMS, so
//     it holds a steady perceived level, and a separate true-peak limiter handles
//     the peaks.
//   * there is lookahead. The audio is delayed by ~2 ms and the gain is applied to
//     the delayed copy, so the gain has settled before the transient arrives. That
//     is what keeps the limiter from overshooting instead of only reacting.
//   * the final stage is a true-peak limiter. The reference clamped the output
//     with x/(1+|x|), which colours the signal at any level and only catches
//     sample peaks. The limiter here estimates the peak between samples with a
//     4x Catmull-Rom interpolation, so inter-sample peaks are caught too, and it
//     only reduces gain when the signal would actually exceed the ceiling.
//   * the parameters are published safely. They lived in a plain struct written by
//     the control thread and read by the audio thread, so setParameters() racing
//     process() was undefined behaviour. Every published parameter is now a
//     std::atomic, loaded once at the top of process().
//
// Also: bypass is a true passthrough (the reference still applied the input gain
// while bypassed) and it ramps instead of switching; the NaN/Inf guards inspect
// the bit pattern so they survive -ffast-math; the lookahead buffer is allocated
// in prepare(), never in process(). The minimum gain is fixed at -24 dB.
class Normalizer {
public:
    static constexpr float LOOKAHEAD_SECONDS = 0.002f;
    static constexpr float MIN_GAIN_DB = -24.0f;
    static constexpr float CEILING = 0.99f;

    explicit Normalizer(float sampleRate = 48000.0f) {
        prepare(sampleRate);
    }

    bool prepare(float sampleRate) {
        if (sampleRate < 8000.0f || sampleRate > 384000.0f) return false;

        sampleRate_ = sampleRate;
        lookaheadSamples_ = std::min(2048, std::max(1, (int) (sampleRate_ * LOOKAHEAD_SECONDS)));

        smooth_ = 1.0f - std::exp(-1.0f / (0.015f * sampleRate_));
        enableStep_ = 1.0f / (0.010f * sampleRate_);
        // ~10 ms RMS window, independent of the attack/release controls.
        rmsCoefficient_ = 1.0f - std::exp(-1.0f / (0.010f * sampleRate_));
        // Limiter release, and the peak hold that feeds it. The hold rises to the
        // true peak at once and decays over the release time, so the gain it calls
        // for is smooth instead of following the signal down to zero at every
        // zero crossing.
        limRelease_ = 1.0f - std::exp(-1.0f / (0.050f * sampleRate_));
        peakHoldDecay_ = 1.0f - std::exp(-1.0f / (0.050f * sampleRate_));
        gateAttack_ = 1.0f - std::exp(-1.0f / (0.002f * sampleRate_));
        gateRelease_ = 1.0f - std::exp(-1.0f / (0.100f * sampleRate_));

        reset();
        return true;
    }

    void setParameters(float inputGainDb, float targetLevelDb, float maxGainDb,
                       float attackMs, float releaseMs, float gateDb,
                       float outputGainDb, bool gateEnabled) {
        inputGain_.store(clamp(inputGainDb, -24.0f, 24.0f), std::memory_order_relaxed);
        target_.store(clamp(targetLevelDb, -36.0f, 0.0f), std::memory_order_relaxed);
        maxGain_.store(clamp(maxGainDb, 0.0f, 30.0f), std::memory_order_relaxed);
        attack_.store(clamp(attackMs, 0.1f, 200.0f), std::memory_order_relaxed);
        release_.store(clamp(releaseMs, 10.0f, 3000.0f), std::memory_order_relaxed);
        gate_.store(clamp(gateDb, -100.0f, -20.0f), std::memory_order_relaxed);
        outputGain_.store(clamp(outputGainDb, -24.0f, 24.0f), std::memory_order_relaxed);
        gateEnabled_.store(gateEnabled, std::memory_order_relaxed);
    }

    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }

    void reset() {
        for (int i = 0; i < lookaheadSamples_; ++i) {
            lookaheadL_[i] = 0.0f;
            lookaheadR_[i] = 0.0f;
        }
        lookaheadIndex_ = 0;
        rms_ = 0.0f;
        agcGain_ = 1.0f;
        gateGain_ = 1.0f;
        limGain_ = 1.0f;
        peakHold_ = 0.0f;
        for (int i = 0; i < 4; ++i) {
            histL_[i] = 0.0f;
            histR_[i] = 0.0f;
        }
        enableRamp_ = 1.0f;
        primed_ = false;
    }

    // Stereo, separate per-channel buffers.
    void process(float *left, float *right, int32_t numFrames) {
        if (left == nullptr || right == nullptr || numFrames <= 0) return;

        const float inputGainTarget = inputGain_.load(std::memory_order_relaxed);
        const float targetTarget = target_.load(std::memory_order_relaxed);
        const float maxGainTarget = maxGain_.load(std::memory_order_relaxed);
        const float attackTarget = attack_.load(std::memory_order_relaxed);
        const float releaseTarget = release_.load(std::memory_order_relaxed);
        const float gateTarget = gate_.load(std::memory_order_relaxed);
        const float outputGainTarget = outputGain_.load(std::memory_order_relaxed);
        const bool gateOnTarget = gateEnabled_.load(std::memory_order_relaxed);
        const float enableTarget = enabled_.load(std::memory_order_relaxed) ? 1.0f : 0.0f;

        if (!primed_) {
            inputGainSm_ = inputGainTarget;
            targetSm_ = targetTarget;
            maxGainSm_ = maxGainTarget;
            attackSm_ = attackTarget;
            releaseSm_ = releaseTarget;
            gateSm_ = gateTarget;
            outputGainSm_ = outputGainTarget;
            enableRamp_ = enableTarget;
            primed_ = true;
        }

        for (int32_t i = 0; i < numFrames; ++i) {
            inputGainSm_ += smooth_ * (inputGainTarget - inputGainSm_);
            targetSm_ += smooth_ * (targetTarget - targetSm_);
            maxGainSm_ += smooth_ * (maxGainTarget - maxGainSm_);
            attackSm_ += smooth_ * (attackTarget - attackSm_);
            releaseSm_ += smooth_ * (releaseTarget - releaseSm_);
            gateSm_ += smooth_ * (gateTarget - gateSm_);
            outputGainSm_ += smooth_ * (outputGainTarget - outputGainSm_);
            enableRamp_ += std::min(1.0f, enableStep_) * (enableTarget - enableRamp_);

            const float inGain = dbToLinear(inputGainSm_);
            const float outGain = dbToLinear(outputGainSm_);
            const float targetLevel = dbToLinear(targetSm_);
            const float maxGain = dbToLinear(maxGainSm_);
            const float minGain = dbToLinear(MIN_GAIN_DB);
            const float gateThreshold = dbToLinear(gateSm_);

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

            float inL = xL * inGain;
            float inR = xR * inGain;

            const float peak = std::max(std::fabs(inL), std::fabs(inR));

            // RMS envelope, for the level the AGC actually holds.
            const float power = 0.5f * (inL * inL + inR * inR);
            rms_ += rmsCoefficient_ * (power - rms_);
            const float rms = std::sqrt(std::max(0.0f, rms_));

            // Automatic gain control on the RMS level.
            float desiredGain = targetLevel / std::max(rms, 1.0e-6f);
            desiredGain = clamp(desiredGain, minGain, maxGain);

            const float attackCoefficient = timeToCoefficient(attackSm_);
            const float releaseCoefficient = timeToCoefficient(releaseSm_);
            const float agcCoefficient = desiredGain < agcGain_ ? attackCoefficient
                                                                 : releaseCoefficient;
            agcGain_ += agcCoefficient * (desiredGain - agcGain_);

            // Noise gate, keyed on the input peak.
            if (gateOnTarget) {
                const float gateGoal = peak >= gateThreshold ? 1.0f : 0.0f;
                const float gateCoefficient = gateGoal < gateGain_ ? gateAttack_
                                                                   : gateRelease_;
                gateGain_ += gateCoefficient * (gateGoal - gateGain_);
            } else {
                gateGain_ += gateAttack_ * (1.0f - gateGain_);
            }

            // True-peak estimate of what the gain stage would produce, so the
            // limiter can act on the interpolated peak, not just the sample peak.
            histL_[3] = histL_[2]; histL_[2] = histL_[1]; histL_[1] = histL_[0]; histL_[0] = inL;
            histR_[3] = histR_[2]; histR_[2] = histR_[1]; histR_[1] = histR_[0]; histR_[0] = inR;
            const float truePeak = std::max(interpolatedPeak(histL_), interpolatedPeak(histR_));

            const float stage = agcGain_ * gateGain_ * outGain;

            // Peak hold over the lookahead window: rise at once, decay slowly.
            peakHold_ = std::max(truePeak, peakHold_ * (1.0f - peakHoldDecay_));

            const float predicted = peakHold_ * stage;
            const float limiterGoal = predicted > CEILING ? CEILING / predicted : 1.0f;

            // Instant attack, smooth release: the gain never lets a held peak
            // through, and recovers gently once the peak has passed.
            if (limiterGoal < limGain_) limGain_ = limiterGoal;
            else limGain_ += limRelease_ * (limiterGoal - limGain_);

            // The gain is applied to the delayed copy, so it has settled by the
            // time the transient it was computed for reaches the output.
            const float delayedL = lookaheadL_[lookaheadIndex_];
            const float delayedR = lookaheadR_[lookaheadIndex_];
            lookaheadL_[lookaheadIndex_] = inL;
            lookaheadR_[lookaheadIndex_] = inR;
            if (++lookaheadIndex_ >= lookaheadSamples_) lookaheadIndex_ = 0;

            const float totalGain = stage * limGain_;
            const float wetL = clip(delayedL * totalGain);
            const float wetR = clip(delayedR * totalGain);

            // Blend dry in as the ramp falls, so a bypass is click free.
            const float outL = xL + (wetL - xL) * enableRamp_;
            const float outR = xR + (wetR - xR) * enableRamp_;

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

    static float dbToLinear(float db) {
        return std::pow(10.0f, db / 20.0f);
    }

    // Last-resort clip. The limiter keeps the signal below CEILING, so this only
    // ever trims the rare sample that slips through.
    static float clip(float x) {
        if (x > 1.0f) return 1.0f;
        if (x < -1.0f) return -1.0f;
        return x;
    }

    float timeToCoefficient(float timeMs) const {
        const float seconds = std::max(0.0001f, timeMs * 0.001f);
        return 1.0f - std::exp(-1.0f / (seconds * sampleRate_));
    }

    // 4x true-peak estimate around the current sample, using the Catmull-Rom
    // interpolation between the previous and current samples.
    static float interpolatedPeak(const float *h) {
        const float y0 = h[3], y1 = h[2], y2 = h[1], y3 = h[0];
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        float peak = std::fabs(y2);
        for (float t = 0.25f; t < 1.0f; t += 0.25f) {
            const float v = ((c3 * t + c2) * t + c1) * t + y1;
            peak = std::max(peak, std::fabs(v));
        }
        return peak;
    }

    float sampleRate_ = 48000.0f;
    float smooth_ = 0.0f;
    float enableStep_ = 0.0f;
    float rmsCoefficient_ = 0.0f;
    float limRelease_ = 0.0f;
    float peakHoldDecay_ = 0.0f;
    float gateAttack_ = 0.0f;
    float gateRelease_ = 0.0f;

    int lookaheadSamples_ = 96;
    float lookaheadL_[2048];
    float lookaheadR_[2048];
    int lookaheadIndex_ = 0;

    float histL_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float histR_[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    float rms_ = 0.0f;
    float agcGain_ = 1.0f;
    float gateGain_ = 1.0f;
    float limGain_ = 1.0f;
    float peakHold_ = 0.0f;

    std::atomic<float> inputGain_{0.0f};
    std::atomic<float> target_{-12.0f};
    std::atomic<float> maxGain_{18.0f};
    std::atomic<float> attack_{10.0f};
    std::atomic<float> release_{250.0f};
    std::atomic<float> gate_{-70.0f};
    std::atomic<float> outputGain_{0.0f};
    std::atomic<bool> gateEnabled_{false};
    std::atomic<bool> enabled_{true};

    // Smoothed copies, written only by the audio thread.
    float inputGainSm_ = 0.0f, targetSm_ = -12.0f, maxGainSm_ = 18.0f;
    float attackSm_ = 10.0f, releaseSm_ = 250.0f, gateSm_ = -70.0f, outputGainSm_ = 0.0f;
    float enableRamp_ = 1.0f;
    bool primed_ = false;
};

#endif // ALTODFX_NORMALIZER_H
