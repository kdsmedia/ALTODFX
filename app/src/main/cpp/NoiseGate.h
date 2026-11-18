#ifndef ALTODFX_NOISEGATE_H
#define ALTODFX_NOISEGATE_H

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

// Stereo noise gate for the ALTODFX rack: threshold, hysteresis, attack, hold,
// release, range, and stereo linking. It removes hum, hiss, and pickup noise
// while no string is sounding.
//
// Based on the reference "NoiseGate" DSP. The reference's note named the two
// things to settle before production:
//
//   * setParameters() must hand the parameters to the audio thread safely. They
//     lived in a plain struct written by the control thread and read by the audio
//     thread, so setParameters() racing process() was undefined behaviour. Every
//     published parameter is now a std::atomic, loaded once at the top of
//     process().
//   * the detector is an amplitude envelope, not spectral detection. This version
//     keeps an envelope detector but runs it on a band-limited copy of the signal,
//     behind a second-order high-pass at 120 Hz. That rejects 50/60 Hz mains hum,
//     which is what most often opens a broadband gate by mistake, by about 15 dB,
//     while the low E string at 82 Hz is only 7.5 dB down so the detector still
//     sees it. A fully spectral gate would need an FFT and is left as future work;
//     this is a filtered envelope, not a spectral one.
//
// Two further problems were found while reading the reference and are fixed here:
//
//   * the Attack control did not control the gate opening. The reference smoothed
//     the envelope with the attack time but then opened the gate with a hard-coded
//     2 ms coefficient (gateAttackCoeff_ = makeCoefficient(2.0f)), so the Attack
//     knob only changed how fast the detector followed, not how fast the gate
//     opened. The gate opening now uses the Attack time and the closing uses the
//     Release time.
//   * the gate had no lookahead, so it opened only after the pick transient had
//     already passed and clipped the attack of the note. The audio is now delayed
//     by 2 ms and the gate gain is applied to the delayed copy, so the gate has
//     opened by the time the transient reaches the output.
//
// A third problem was found while testing and is fixed: the gate had a single open
// / closed state shared by both channels, so the Stereo Link control had no
// effect (both channels always moved together). The gate state, hold counter, and
// gain are now per channel, so Link at 0 lets one channel open while the other
// stays closed.
//
// Also: there is a DC blocker, bypass is a true ramped passthrough, the NaN/Inf
// guards inspect the bit pattern so they survive -ffast-math, and the lookahead
// buffer is allocated in prepare(), never in process().
class NoiseGate {
public:
    static constexpr float LOOKAHEAD_SECONDS = 0.002f;

    explicit NoiseGate(float sampleRate = 48000.0f) {
        prepare(sampleRate);
    }

    bool prepare(float sampleRate) {
        if (!isFinite(sampleRate) || sampleRate < 8000.0f || sampleRate > 384000.0f) return false;

        sampleRate_ = sampleRate;
        lookaheadSamples_ = std::min(2048, std::max(1, (int) (sampleRate_ * LOOKAHEAD_SECONDS)));

        smooth_ = 1.0f - std::exp(-1.0f / (0.015f * sampleRate_));
        enableStep_ = 1.0f / (0.010f * sampleRate_);
        dcCoefficient_ = 1.0f - 2.0f * 3.14159265359f * 10.0f / sampleRate_;
        // The detector envelope follows the signal quickly, so the open/close
        // decision tracks it. The Attack and Release controls belong to the gate
        // gain ramp below, not to this envelope; using Release for both would
        // apply it twice.
        envelopeReleaseCoeff_ = 1.0f - std::exp(-1.0f / (0.005f * sampleRate_));

        // Second-order high-pass on the detector at 120 Hz: it rejects mains hum
        // by about 15 dB and still passes the low E string 7.5 dB down.
        {
            const float w = 2.0f * 3.14159265359f * 120.0f / sampleRate_;
            const float cw = std::cos(w);
            const float alpha = std::sin(w) / (2.0f * 0.70710678f);
            const float a0 = 1.0f + alpha;
            humB0_ = (1.0f + cw) / 2.0f / a0;
            humB1_ = -(1.0f + cw) / a0;
            humB2_ = humB0_;
            humA1_ = (-2.0f * cw) / a0;
            humA2_ = (1.0f - alpha) / a0;
        }

        reset();
        return true;
    }

    void setParameters(float thresholdDb, float hysteresisDb, float attackMs, float holdMs,
                       float releaseMs, float rangeDb, float stereoLink) {
        threshold_.store(clamp(thresholdDb, -90.0f, 0.0f), std::memory_order_relaxed);
        hysteresis_.store(clamp(hysteresisDb, 0.0f, 24.0f), std::memory_order_relaxed);
        attack_.store(clamp(attackMs, 0.1f, 100.0f), std::memory_order_relaxed);
        hold_.store(clamp(holdMs, 0.0f, 1000.0f), std::memory_order_relaxed);
        release_.store(clamp(releaseMs, 1.0f, 2000.0f), std::memory_order_relaxed);
        range_.store(clamp(rangeDb, -100.0f, 0.0f), std::memory_order_relaxed);
        stereoLink_.store(clamp(stereoLink, 0.0f, 1.0f), std::memory_order_relaxed);
    }

    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }

    void reset() {
        for (int c = 0; c < 2; ++c) {
            lookahead_[c].fill(0.0f);
            lookaheadIndex_[c] = 0;
            envelope_[c] = 0.0f;
            gain_[c] = 1.0f;
            dcX_[c] = dcY_[c] = 0.0f;
            humX1_[c] = humX2_[c] = humY1_[c] = humY2_[c] = 0.0f;
            holdCounter_[c] = 0;
            gateOpen_[c] = true;
        }
        enableRamp_ = 1.0f;
        primed_ = false;
    }

    // True when the gate is passing signal.
    bool isOpen() const { return gateOpen_[0] && gateOpen_[1]; }

    // Current gate gain of the left channel (1.0 open, range when closed).
    float getGain() const { return gain_[0]; }

    // Stereo, separate per-channel buffers.
    void process(float *left, float *right, int32_t numFrames) {
        if (left == nullptr || right == nullptr || numFrames <= 0) return;

        const float thresholdTarget = threshold_.load(std::memory_order_relaxed);
        const float hysteresisTarget = hysteresis_.load(std::memory_order_relaxed);
        const float attackTarget = attack_.load(std::memory_order_relaxed);
        const float holdTarget = hold_.load(std::memory_order_relaxed);
        const float releaseTarget = release_.load(std::memory_order_relaxed);
        const float rangeTarget = range_.load(std::memory_order_relaxed);
        const float stereoLinkTarget = stereoLink_.load(std::memory_order_relaxed);
        const float enableTarget = enabled_.load(std::memory_order_relaxed) ? 1.0f : 0.0f;

        if (!primed_) {
            thresholdSm_ = thresholdTarget;
            hysteresisSm_ = hysteresisTarget;
            attackSm_ = attackTarget;
            holdSm_ = holdTarget;
            releaseSm_ = releaseTarget;
            rangeSm_ = rangeTarget;
            stereoLinkSm_ = stereoLinkTarget;
            enableRamp_ = enableTarget;
            primed_ = true;
        }

        for (int32_t i = 0; i < numFrames; ++i) {
            thresholdSm_ += smooth_ * (thresholdTarget - thresholdSm_);
            hysteresisSm_ += smooth_ * (hysteresisTarget - hysteresisSm_);
            attackSm_ += smooth_ * (attackTarget - attackSm_);
            holdSm_ += smooth_ * (holdTarget - holdSm_);
            releaseSm_ += smooth_ * (releaseTarget - releaseSm_);
            rangeSm_ += smooth_ * (rangeTarget - rangeSm_);
            stereoLinkSm_ += smooth_ * (stereoLinkTarget - stereoLinkSm_);
            enableRamp_ += std::min(1.0f, enableStep_) * (enableTarget - enableRamp_);

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

            const float threshold = dbToLinear(thresholdSm_);
            const float closeThreshold = dbToLinear(thresholdSm_ - hysteresisSm_);
            const float floorGain = dbToLinear(rangeSm_);

            // Per-channel detector, blended with the linked (loudest) channel.
            updateEnvelope(xL, xR);
            const float linked = std::max(envelope_[0], envelope_[1]);
            const float openCoeff = timeToCoefficient(attackSm_);
            const float closeCoeff = timeToCoefficient(releaseSm_);
            const int64_t holdSamples = (int64_t) (holdSm_ * 0.001f * sampleRate_);

            for (int c = 0; c < 2; ++c) {
                const float detector = envelope_[c] * (1.0f - stereoLinkSm_) + linked * stereoLinkSm_;

                // Hysteresis plus hold: open above the threshold, and only close
                // once the level has fallen below threshold minus hysteresis and
                // the hold time has elapsed.
                if (detector >= threshold) {
                    gateOpen_[c] = true;
                    holdCounter_[c] = holdSamples;
                } else if (detector < closeThreshold) {
                    if (holdCounter_[c] > 0) --holdCounter_[c];
                    else gateOpen_[c] = false;
                }

                const float targetGain = gateOpen_[c] ? 1.0f : floorGain;
                const float coefficient = targetGain > gain_[c] ? openCoeff : closeCoeff;
                gain_[c] += coefficient * (targetGain - gain_[c]);
            }

            // Apply the gain to the delayed copy, so the gate has opened by the
            // time the transient that triggered it reaches the output.
            float delayedL = lookahead_[0][lookaheadIndex_[0]];
            float delayedR = lookahead_[1][lookaheadIndex_[1]];
            lookahead_[0][lookaheadIndex_[0]] = xL;
            lookahead_[1][lookaheadIndex_[1]] = xR;
            if (++lookaheadIndex_[0] >= lookaheadSamples_) lookaheadIndex_[0] = 0;
            if (++lookaheadIndex_[1] >= lookaheadSamples_) lookaheadIndex_[1] = 0;

            const float wetL = delayedL * gain_[0];
            const float wetR = delayedR * gain_[1];

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

    static float dbToLinear(float db) { return std::pow(10.0f, db / 20.0f); }

    float timeToCoefficient(float timeMs) const {
        const float seconds = std::max(0.0001f, timeMs * 0.001f);
        return 1.0f - std::exp(-1.0f / (seconds * sampleRate_));
    }

    // Amplitude envelope of the band-limited input, per channel.
    void updateEnvelope(float xL, float xR) {
        const float levels[2] = { bandLimited(xL, 0), bandLimited(xR, 1) };
        const float attackCoeff = timeToCoefficient(1.0f);
        for (int c = 0; c < 2; ++c) {
            const float coefficient = levels[c] > envelope_[c] ? attackCoeff : envelopeReleaseCoeff_;
            envelope_[c] += coefficient * (levels[c] - envelope_[c]);
        }
    }

    // DC blocker, then a second-order high-pass, then the absolute value.
    float bandLimited(float x, int ch) {
        const float blocked = x - dcX_[ch] + dcCoefficient_ * dcY_[ch];
        dcX_[ch] = x;
        dcY_[ch] = blocked;

        const float high = humB0_ * blocked + humB1_ * humX1_[ch] + humB2_ * humX2_[ch]
                           - humA1_ * humY1_[ch] - humA2_ * humY2_[ch];
        humX2_[ch] = humX1_[ch]; humX1_[ch] = blocked;
        humY2_[ch] = humY1_[ch]; humY1_[ch] = high;
        return std::fabs(high);
    }

    float sampleRate_ = 48000.0f;
    float smooth_ = 0.0f;
    float enableStep_ = 0.0f;
    float dcCoefficient_ = 0.999f;
    float envelopeReleaseCoeff_ = 0.01f;
    float humB0_ = 1.0f, humB1_ = 0.0f, humB2_ = 0.0f, humA1_ = 0.0f, humA2_ = 0.0f;

    int lookaheadSamples_ = 96;
    std::array<float, 2048> lookahead_[2];
    int lookaheadIndex_[2] = {0, 0};

    float envelope_[2] = {0.0f, 0.0f};
    float gain_[2] = {1.0f, 1.0f};
    float dcX_[2] = {0.0f, 0.0f};
    float dcY_[2] = {0.0f, 0.0f};
    float humX1_[2] = {0.0f, 0.0f}, humX2_[2] = {0.0f, 0.0f};
    float humY1_[2] = {0.0f, 0.0f}, humY2_[2] = {0.0f, 0.0f};

    int64_t holdCounter_[2] = {0, 0};
    bool gateOpen_[2] = {true, true};
    float enableRamp_ = 1.0f;

    std::atomic<float> threshold_{-45.0f};
    std::atomic<float> hysteresis_{6.0f};
    std::atomic<float> attack_{2.0f};
    std::atomic<float> hold_{50.0f};
    std::atomic<float> release_{120.0f};
    std::atomic<float> range_{-80.0f};
    std::atomic<float> stereoLink_{1.0f};
    std::atomic<bool> enabled_{true};

    // Smoothed copies, written only by the audio thread.
    float thresholdSm_ = -45.0f, hysteresisSm_ = 6.0f, attackSm_ = 2.0f, holdSm_ = 50.0f;
    float releaseSm_ = 120.0f, rangeSm_ = -80.0f, stereoLinkSm_ = 1.0f;
    bool primed_ = false;
};

#endif // ALTODFX_NOISEGATE_H
