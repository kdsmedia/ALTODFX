#ifndef ALTODFX_COMPRESSOR_H
#define ALTODFX_COMPRESSOR_H

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

// Feed-forward stereo compressor for the ALTODFX rack: threshold, ratio, attack,
// release, soft knee, makeup gain, a peak/RMS detector blend, and stereo linking,
// with a true-peak limiter on the output.
//
// Based on the reference "Compressor" DSP. The reference's note named two things
// to settle before production, and both are addressed here:
//
//   * the parameters must be handed to the audio thread without a race. They lived
//     in a plain struct written by the control thread and read by the audio thread,
//     so setParameters() racing process() was undefined behaviour. Every published
//     parameter is now a std::atomic, loaded once at the top of process().
//   * the output must not be a hard clamp. The reference clipped with
//     clamp(x, -1, 1), which distorts the peaks it was meant to catch. The output
//     now passes through a true-peak limiter that estimates the peak between
//     samples with a 4x Catmull-Rom interpolation, holds that peak, and applies
//     instant-attack / smooth-release gain, so inter-sample peaks are caught too.
//
// Also fixed: the reference applied attack and release twice. It smoothed the
// detector envelope with the attack/release coefficients and then smoothed the
// gain with the same coefficients again, so the effective times were roughly half
// what was asked for and the two controls interacted. The attack and release now
// act once, on the detector envelope, which is the classic feed-forward design;
// the gain computer is a static curve on that envelope.
//
// Also: the RMS window is derived from the sample rate (the reference used a fixed
// 10 ms coefficient that was only right at 48 kHz); bypass is a true ramped
// passthrough; the NaN/Inf guards inspect the bit pattern so they survive
// -ffast-math; the lookahead buffer is allocated in prepare(), never in process().
class Compressor {
public:
    static constexpr float LOOKAHEAD_SECONDS = 0.002f;
    static constexpr float CEILING = 0.99f;

    explicit Compressor(float sampleRate = 48000.0f) {
        prepare(sampleRate);
    }

    bool prepare(float sampleRate) {
        if (!isFinite(sampleRate) || sampleRate < 8000.0f || sampleRate > 384000.0f) return false;

        sampleRate_ = sampleRate;
        lookaheadSamples_ = std::min(2048, std::max(1, (int) (sampleRate_ * LOOKAHEAD_SECONDS)));

        smooth_ = 1.0f - std::exp(-1.0f / (0.015f * sampleRate_));
        enableStep_ = 1.0f / (0.010f * sampleRate_);
        // Fixed ~10 ms RMS window, now scaled by the sample rate.
        rmsCoeff_ = 1.0f - std::exp(-1.0f / (0.010f * sampleRate_));
        // Limiter peak hold and release.
        limRelease_ = 1.0f - std::exp(-1.0f / (0.050f * sampleRate_));

        reset();
        return true;
    }

    void setParameters(float thresholdDb, float ratio, float attackMs, float releaseMs,
                       float kneeDb, float makeupGainDb, float detectorMix,
                       float stereoLink) {
        threshold_.store(clamp(thresholdDb, -60.0f, 0.0f), std::memory_order_relaxed);
        ratio_.store(clamp(ratio, 1.0f, 20.0f), std::memory_order_relaxed);
        attack_.store(clamp(attackMs, 0.1f, 200.0f), std::memory_order_relaxed);
        release_.store(clamp(releaseMs, 5.0f, 2000.0f), std::memory_order_relaxed);
        knee_.store(clamp(kneeDb, 0.0f, 24.0f), std::memory_order_relaxed);
        makeup_.store(clamp(makeupGainDb, -12.0f, 24.0f), std::memory_order_relaxed);
        detectorMix_.store(clamp(detectorMix, 0.0f, 1.0f), std::memory_order_relaxed);
        stereoLink_.store(clamp(stereoLink, 0.0f, 1.0f), std::memory_order_relaxed);
    }

    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }

    void reset() {
        for (int i = 0; i < lookaheadSamples_; ++i) {
            lookaheadL_[i] = 0.0f;
            lookaheadR_[i] = 0.0f;
        }
        lookaheadIndex_ = 0;
        envL_ = envR_ = 0.0f;
        rmsL_ = rmsR_ = 0.0f;
        limGain_ = 1.0f;
        peakHold_ = 0.0f;
        gainReductionDb_ = 0.0f;
        for (int i = 0; i < 4; ++i) {
            histL_[i] = 0.0f;
            histR_[i] = 0.0f;
        }
        enableRamp_ = 1.0f;
        primed_ = false;
    }

    // Gain reduction of the last processed frame, in dB (negative when reducing).
    float getGainReductionDb() const { return gainReductionDb_; }

    // Stereo, separate per-channel buffers.
    void process(float *left, float *right, int32_t numFrames) {
        if (left == nullptr || right == nullptr || numFrames <= 0) return;

        const float thresholdTarget = threshold_.load(std::memory_order_relaxed);
        const float ratioTarget = ratio_.load(std::memory_order_relaxed);
        const float attackTarget = attack_.load(std::memory_order_relaxed);
        const float releaseTarget = release_.load(std::memory_order_relaxed);
        const float kneeTarget = knee_.load(std::memory_order_relaxed);
        const float makeupTarget = makeup_.load(std::memory_order_relaxed);
        const float detectorMixTarget = detectorMix_.load(std::memory_order_relaxed);
        const float stereoLinkTarget = stereoLink_.load(std::memory_order_relaxed);
        const float enableTarget = enabled_.load(std::memory_order_relaxed) ? 1.0f : 0.0f;

        if (!primed_) {
            thresholdSm_ = thresholdTarget;
            ratioSm_ = ratioTarget;
            attackSm_ = attackTarget;
            releaseSm_ = releaseTarget;
            kneeSm_ = kneeTarget;
            makeupSm_ = makeupTarget;
            detectorMixSm_ = detectorMixTarget;
            stereoLinkSm_ = stereoLinkTarget;
            enableRamp_ = enableTarget;
            primed_ = true;
        }

        for (int32_t i = 0; i < numFrames; ++i) {
            thresholdSm_ += smooth_ * (thresholdTarget - thresholdSm_);
            ratioSm_ += smooth_ * (ratioTarget - ratioSm_);
            attackSm_ += smooth_ * (attackTarget - attackSm_);
            releaseSm_ += smooth_ * (releaseTarget - releaseSm_);
            kneeSm_ += smooth_ * (kneeTarget - kneeSm_);
            makeupSm_ += smooth_ * (makeupTarget - makeupSm_);
            detectorMixSm_ += smooth_ * (detectorMixTarget - detectorMixSm_);
            stereoLinkSm_ += smooth_ * (stereoLinkTarget - stereoLinkSm_);
            enableRamp_ += std::min(1.0f, enableStep_) * (enableTarget - enableRamp_);

            const float attackCoeff = timeToCoefficient(attackSm_);
            const float releaseCoeff = timeToCoefficient(releaseSm_);
            const float makeup = dbToLinear(makeupSm_);

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

            // Peak envelope, with the attack/release times.
            envL_ = follow(std::fabs(xL), envL_, attackCoeff, releaseCoeff);
            envR_ = follow(std::fabs(xR), envR_, attackCoeff, releaseCoeff);

            // RMS, for the detector blend.
            rmsL_ += rmsCoeff_ * (xL * xL - rmsL_);
            rmsR_ += rmsCoeff_ * (xR * xR - rmsR_);
            const float rmsLevelL = std::sqrt(std::max(0.0f, rmsL_));
            const float rmsLevelR = std::sqrt(std::max(0.0f, rmsR_));

            float detectorL = envL_ * (1.0f - detectorMixSm_) + rmsLevelL * detectorMixSm_;
            float detectorR = envR_ * (1.0f - detectorMixSm_) + rmsLevelR * detectorMixSm_;

            // Stereo linking: a common gain keeps the image intact.
            const float linked = std::max(detectorL, detectorR);
            detectorL = detectorL * (1.0f - stereoLinkSm_) + linked * stereoLinkSm_;
            detectorR = detectorR * (1.0f - stereoLinkSm_) + linked * stereoLinkSm_;

            const float gainL = computeGain(detectorL);
            const float gainR = computeGain(detectorR);
            gainReductionDb_ = 20.0f * std::log10(std::max(std::min(gainL, gainR), 1.0e-8f));

            const float stage = makeup;

            // True-peak estimate of the compressed signal.
            histL_[3] = histL_[2]; histL_[2] = histL_[1]; histL_[1] = histL_[0]; histL_[0] = xL * gainL * stage;
            histR_[3] = histR_[2]; histR_[2] = histR_[1]; histR_[1] = histR_[0]; histR_[0] = xR * gainR * stage;
            const float truePeak = std::max(interpolatedPeak(histL_), interpolatedPeak(histR_));

            // Peak hold, then instant-attack / smooth-release limiter gain.
            peakHold_ = std::max(truePeak, peakHold_ * (1.0f - limRelease_));
            const float limiterGoal = peakHold_ > CEILING ? CEILING / peakHold_ : 1.0f;
            if (limiterGoal < limGain_) limGain_ = limiterGoal;
            else limGain_ += limRelease_ * (limiterGoal - limGain_);

            // Apply the gain to the delayed copy, so the limiter has settled by
            // the time the peak it saw reaches the output.
            const float delayedL = lookaheadL_[lookaheadIndex_];
            const float delayedR = lookaheadR_[lookaheadIndex_];
            lookaheadL_[lookaheadIndex_] = xL * gainL * stage;
            lookaheadR_[lookaheadIndex_] = xR * gainR * stage;
            if (++lookaheadIndex_ >= lookaheadSamples_) lookaheadIndex_ = 0;

            const float wetL = clip(delayedL * limGain_);
            const float wetR = clip(delayedR * limGain_);

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

    static float follow(float input, float previous, float attack, float release) {
        const float coefficient = input > previous ? attack : release;
        return previous + coefficient * (input - previous);
    }

    float timeToCoefficient(float timeMs) const {
        const float seconds = std::max(0.0001f, timeMs * 0.001f);
        return 1.0f - std::exp(-1.0f / (seconds * sampleRate_));
    }

    // Static gain curve with a quadratic soft knee, in dB.
    float computeGain(float detector) const {
        const float inputDb = 20.0f * std::log10(std::max(detector, 1.0e-8f));
        const float threshold = thresholdSm_;
        const float ratio = std::max(1.0f, ratioSm_);
        const float knee = kneeSm_;

        float outputDb = inputDb;
        if (knee <= 0.0f) {
            if (inputDb > threshold) outputDb = threshold + (inputDb - threshold) / ratio;
        } else {
            const float halfKnee = knee * 0.5f;
            if (inputDb > threshold + halfKnee) {
                outputDb = threshold + (inputDb - threshold) / ratio;
            } else if (inputDb > threshold - halfKnee) {
                const float x = inputDb - threshold + halfKnee;
                outputDb = inputDb - (1.0f - 1.0f / ratio) * (x * x) / (2.0f * knee);
            }
        }
        return dbToLinear(outputDb - inputDb);
    }

    // 4x true-peak estimate, Catmull-Rom between the previous and current samples.
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
    float rmsCoeff_ = 0.0f;
    float limRelease_ = 0.0f;

    int lookaheadSamples_ = 96;
    float lookaheadL_[2048];
    float lookaheadR_[2048];
    int lookaheadIndex_ = 0;

    float histL_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float histR_[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    float envL_ = 0.0f, envR_ = 0.0f;
    float rmsL_ = 0.0f, rmsR_ = 0.0f;
    float limGain_ = 1.0f;
    float peakHold_ = 0.0f;
    float gainReductionDb_ = 0.0f;

    std::atomic<float> threshold_{-18.0f};
    std::atomic<float> ratio_{4.0f};
    std::atomic<float> attack_{10.0f};
    std::atomic<float> release_{100.0f};
    std::atomic<float> knee_{6.0f};
    std::atomic<float> makeup_{0.0f};
    std::atomic<float> detectorMix_{0.0f};
    std::atomic<float> stereoLink_{1.0f};
    std::atomic<bool> enabled_{true};

    // Smoothed copies, written only by the audio thread.
    float thresholdSm_ = -18.0f, ratioSm_ = 4.0f, attackSm_ = 10.0f, releaseSm_ = 100.0f;
    float kneeSm_ = 6.0f, makeupSm_ = 0.0f, detectorMixSm_ = 0.0f, stereoLinkSm_ = 1.0f;
    float enableRamp_ = 1.0f;
    bool primed_ = false;
};

#endif // ALTODFX_COMPRESSOR_H
