#ifndef ALTODFX_DELAY_H
#define ALTODFX_DELAY_H

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// Stereo delay for the ALTODFX rack: independent left and right delay times, a
// feedback path with a low-cut and a high-cut filter for an analog-ish tail, an
// equal-power dry/wet mix, and a normal or ping-pong routing.
//
// Based on the reference "StereoDelay" DSP. The reference's own note said it was
// a foundation rather than a finished effect, and named two things to fix:
//
//   * the parameters had to be published through a thread-safe mechanism. They
//     lived in a plain struct that the control thread wrote and the audio thread
//     read, so setParameters() racing process() was undefined behaviour. Every
//     published parameter is now a std::atomic, loaded once at the top of
//     process().
//   * a change of delay time should use a transition that avoids a sudden pitch
//     change (a delay-time glide or a dual-head crossfade). The reference glided
//     the delay time with a one-pole, which is a tape-style slide: the tail
//     audibly bends in pitch while the time moves. This version uses a dual-head
//     crossfade instead. Each channel reads from the old delay time and the new
//     one and crossfades over ~30 ms, so the tail keeps its pitch and the change
//     is seamless. A target that arrives mid-crossfade is picked up when the
//     current fade completes, which keeps the blend continuous.
//
// Also fixed: the reference ran the write through softLimit() = x/(1+|x|), which
// at an ordinary level of 0.5 already loses 3.5 dB and colours every repeat. The
// limiter is now transparent below full scale and only compresses above it, so
// normal levels pass untouched while the feedback loop still cannot run away.
// Bypass now ramps the wet path instead of dropping it, and the NaN/Inf guards
// inspect the bit pattern so they survive -ffast-math.
//
// The delay lines are allocated in prepare(), never in process(), so the audio
// callback does not allocate.
class Delay {
public:
    static constexpr float MAX_DELAY_SECONDS = 5.0f;
    // ~30 ms crossfade when a delay time changes.
    static constexpr float XFADE_SECONDS = 0.030f;

    explicit Delay(float sampleRate = 48000.0f, float maxDelaySeconds = MAX_DELAY_SECONDS) {
        prepare(sampleRate, maxDelaySeconds);
    }

    bool prepare(float sampleRate, float maxDelaySeconds = MAX_DELAY_SECONDS) {
        if (sampleRate < 8000.0f || sampleRate > 384000.0f) return false;
        maxDelaySeconds = std::max(0.1f, std::min(maxDelaySeconds, 20.0f));

        sampleRate_ = sampleRate;
        bufferSize_ = (size_t) (sampleRate_ * maxDelaySeconds) + 8;

        // Allocated here, not in process().
        delayL_.assign(bufferSize_, 0.0f);
        delayR_.assign(bufferSize_, 0.0f);

        maxDelaySamples_ = (float) (bufferSize_ - 4);
        maxDelayMs_ = maxDelaySamples_ * 1000.0f / sampleRate_;

        smooth_ = 1.0f - std::exp(-1.0f / (0.015f * sampleRate_));
        xfadeInc_ = 1.0f / (XFADE_SECONDS * sampleRate_);
        enableStep_ = 1.0f / (0.010f * sampleRate_);

        reset();
        return true;
    }

    void setParameters(float timeLeftMs, float timeRightMs, float feedback, float mix,
                       float lowCutHz, float highCutHz, int mode, float outputGain) {
        timeL_.store(clamp(timeLeftMs, 1.0f, maxDelayMs_), std::memory_order_relaxed);
        timeR_.store(clamp(timeRightMs, 1.0f, maxDelayMs_), std::memory_order_relaxed);
        feedback_.store(clamp(feedback, 0.0f, 0.95f), std::memory_order_relaxed);
        mix_.store(clamp(mix, 0.0f, 1.0f), std::memory_order_relaxed);
        lowCut_.store(clamp(lowCutHz, 10.0f, 1000.0f), std::memory_order_relaxed);
        highCut_.store(clamp(highCutHz, 100.0f, 20000.0f), std::memory_order_relaxed);
        mode_.store(mode != 0 ? 1 : 0, std::memory_order_relaxed);
        gain_.store(clamp(outputGain, 0.0f, 2.0f), std::memory_order_relaxed);
    }

    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }

    void reset() {
        std::fill(delayL_.begin(), delayL_.end(), 0.0f);
        std::fill(delayR_.begin(), delayR_.end(), 0.0f);
        writeIndex_ = 0;

        lpL_ = lpR_ = 0.0f;
        hpInL_ = hpInR_ = 0.0f;
        hpOutL_ = hpOutR_ = 0.0f;

        // Idle state: head A holds the current time, no crossfade in flight.
        headAL_ = timeL_.load(std::memory_order_relaxed) * 0.001f * sampleRate_;
        headAR_ = timeR_.load(std::memory_order_relaxed) * 0.001f * sampleRate_;
        headBL_ = headAL_;
        headBR_ = headAR_;
        xfadeL_ = 1.0f;
        xfadeR_ = 1.0f;
        enableRamp_ = 1.0f;
        primed_ = false;
    }

    // Stereo, separate per-channel buffers.
    void process(float *left, float *right, int32_t numFrames) {
        if (left == nullptr || right == nullptr || numFrames <= 0) return;

        const float timeLTarget = timeL_.load(std::memory_order_relaxed);
        const float timeRTarget = timeR_.load(std::memory_order_relaxed);
        const float feedbackTarget = feedback_.load(std::memory_order_relaxed);
        const float mixTarget = mix_.load(std::memory_order_relaxed);
        const float lowCutTarget = lowCut_.load(std::memory_order_relaxed);
        const float highCutTarget = highCut_.load(std::memory_order_relaxed);
        const int modeTarget = mode_.load(std::memory_order_relaxed);
        const float gainTarget = gain_.load(std::memory_order_relaxed);
        const float enableTarget = enabled_.load(std::memory_order_relaxed) ? 1.0f : 0.0f;

        if (!primed_) {
            feedbackSm_ = feedbackTarget;
            mixSm_ = mixTarget;
            lowCutSm_ = lowCutTarget;
            highCutSm_ = highCutTarget;
            gainSm_ = gainTarget;
            enableRamp_ = enableTarget;
            headAL_ = timeLTarget * 0.001f * sampleRate_;
            headAR_ = timeRTarget * 0.001f * sampleRate_;
            headBL_ = headAL_;
            headBR_ = headAR_;
            primed_ = true;
        }

        for (int32_t i = 0; i < numFrames; ++i) {
            feedbackSm_ += smooth_ * (feedbackTarget - feedbackSm_);
            mixSm_ += smooth_ * (mixTarget - mixSm_);
            lowCutSm_ += smooth_ * (lowCutTarget - lowCutSm_);
            highCutSm_ += smooth_ * (highCutTarget - highCutSm_);
            gainSm_ += smooth_ * (gainTarget - gainSm_);
            enableRamp_ += std::min(1.0f, enableStep_) * (enableTarget - enableRamp_);

            const float targetL = timeLTarget * 0.001f * sampleRate_;
            const float targetR = timeRTarget * 0.001f * sampleRate_;

            float inL = left[i];
            float inR = right[i];
            if (!isFinite(inL)) inL = 0.0f;
            if (!isFinite(inR)) inR = 0.0f;

            const float delayedL = readHead(delayL_, headAL_, headBL_, xfadeL_, targetL);
            const float delayedR = readHead(delayR_, headAR_, headBR_, xfadeR_, targetR);

            // Feedback tone shaping: high-cut then low-cut, for an analog tail.
            const float filteredL = shapeFeedback(delayedL, lpL_, hpInL_, hpOutL_);
            const float filteredR = shapeFeedback(delayedR, lpR_, hpInR_, hpOutR_);

            float writeL, writeR;
            if (modeTarget == 1) {  // Ping-pong: the tails swap channels.
                writeL = inL + filteredR * feedbackSm_;
                writeR = inR + filteredL * feedbackSm_;
            } else {
                writeL = inL + filteredL * feedbackSm_;
                writeR = inR + filteredR * feedbackSm_;
            }

            delayL_[writeIndex_] = protect(writeL);
            delayR_[writeIndex_] = protect(writeR);

            const float dryGain = std::cos(mixSm_ * 1.57079632679f);
            const float wetGain = std::sin(mixSm_ * 1.57079632679f);

            const float wetOutL = (inL * dryGain + delayedL * wetGain) * gainSm_;
            const float wetOutR = (inR * dryGain + delayedR * wetGain) * gainSm_;

            // Blend dry in as the ramp falls, so a bypass is click free. The
            // delay lines keep running either way, so the tail survives a bypass.
            const float outL = inL + (wetOutL - inL) * enableRamp_;
            const float outR = inR + (wetOutR - inR) * enableRamp_;

            left[i] = isFinite(outL) ? outL : inL;
            right[i] = isFinite(outR) ? outR : inR;

            ++writeIndex_;
            if (writeIndex_ >= bufferSize_) writeIndex_ = 0;
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

    // Transparent below full scale; compresses smoothly above it. Unlike the
    // reference's x/(1+|x|), an ordinary level is left untouched.
    static float protect(float x) {
        const float a = std::fabs(x);
        if (a <= 1.0f) return x;
        const float limited = 1.0f + (1.0f - std::exp(-(a - 1.0f)));
        return x > 0.0f ? limited : -limited;
    }

    // Reads the crossfaded delay time, advancing the dual-head fade. Head A holds
    // the settled time; when the target moves away, head B takes the new time and
    // the two are blended so the tail never changes pitch.
    float readHead(const std::vector<float> &buffer, float &headA, float &headB,
                   float &xfade, float target) {
        if (xfade >= 1.0f) {
            if (std::fabs(target - headA) > 1.0f) {
                headB = target;
                xfade = 0.0f;
            }
        }
        if (xfade < 1.0f) {
            xfade += xfadeInc_;
            if (xfade >= 1.0f) {
                xfade = 1.0f;
                headA = headB;
            }
        }

        const float a = readDelay(buffer, headA);
        if (xfade >= 1.0f) return a;
        const float b = readDelay(buffer, headB);
        return a + (b - a) * xfade;
    }

    // Cubic Hermite read at a fractional distance behind the write head.
    float readDelay(const std::vector<float> &buffer, float delaySamples) const {
        const int size = (int) buffer.size();
        float position = (float) writeIndex_ - delaySamples;
        while (position < 0.0f) position += (float) size;
        while (position >= (float) size) position -= (float) size;

        const int i1 = (int) position;
        const float t = position - (float) i1;
        const int i0 = (i1 - 1 + size) % size;
        const int i2 = (i1 + 1) % size;
        const int i3 = (i1 + 2) % size;

        const float y0 = buffer[i0], y1 = buffer[i1];
        const float y2 = buffer[i2], y3 = buffer[i3];

        const float c0 = y1;
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * t + c2) * t + c1) * t + c0;
    }

    // One-pole low-pass in series with a one-pole high-pass.
    float shapeFeedback(float input, float &lpState, float &hpInput, float &hpOutput) {
        const float nyquist = sampleRate_ * 0.5f;
        const float lpCutoff = clamp(highCutSm_, 20.0f, nyquist * 0.95f);
        const float hpCutoff = clamp(lowCutSm_, 5.0f, nyquist * 0.45f);

        const float lpCoefficient = 1.0f - std::exp(-6.28318530718f * lpCutoff / sampleRate_);
        const float dt = 1.0f / sampleRate_;
        const float hpRC = 1.0f / (6.28318530718f * hpCutoff);
        const float hpCoefficient = hpRC / (hpRC + dt);

        lpState += lpCoefficient * (input - lpState);
        const float highPassed = hpCoefficient * (hpOutput + lpState - hpInput);
        hpInput = lpState;
        hpOutput = highPassed;
        return highPassed;
    }

    float sampleRate_ = 48000.0f;
    size_t bufferSize_ = 0;
    float maxDelaySamples_ = 0.0f;
    float maxDelayMs_ = 5000.0f;
    float smooth_ = 0.0f;
    float xfadeInc_ = 0.0f;
    float enableStep_ = 0.0f;

    std::vector<float> delayL_;
    std::vector<float> delayR_;
    size_t writeIndex_ = 0;

    // Dual read heads per channel and their crossfade position.
    float headAL_ = 0.0f, headBL_ = 0.0f, xfadeL_ = 1.0f;
    float headAR_ = 0.0f, headBR_ = 0.0f, xfadeR_ = 1.0f;

    float lpL_ = 0.0f, lpR_ = 0.0f;
    float hpInL_ = 0.0f, hpInR_ = 0.0f;
    float hpOutL_ = 0.0f, hpOutR_ = 0.0f;

    std::atomic<float> timeL_{350.0f};
    std::atomic<float> timeR_{450.0f};
    std::atomic<float> feedback_{0.40f};
    std::atomic<float> mix_{0.30f};
    std::atomic<float> lowCut_{80.0f};
    std::atomic<float> highCut_{8000.0f};
    std::atomic<int> mode_{0};
    std::atomic<float> gain_{1.0f};
    std::atomic<bool> enabled_{true};

    // Smoothed copies, written only by the audio thread.
    float feedbackSm_ = 0.40f, mixSm_ = 0.30f, lowCutSm_ = 80.0f;
    float highCutSm_ = 8000.0f, gainSm_ = 1.0f;
    float enableRamp_ = 1.0f;
    bool primed_ = false;
};

#endif // ALTODFX_DELAY_H
