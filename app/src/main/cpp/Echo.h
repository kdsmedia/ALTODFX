#ifndef ALTODFX_ECHO_H
#define ALTODFX_ECHO_H

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// Echo for the ALTODFX rack: a stereo repeat with independent left and right
// times, a feedback path whose tone can be swept from dark to bright, a stereo
// width control, and stereo / ping-pong / mono routing.
//
// Based on the reference "Echo" DSP. The reference's note named three things to
// settle before production, all addressed here:
//
//   * the parameter communication had to be made thread safe. The parameters
//     lived in a plain struct written by the control thread and read by the audio
//     thread, so setParameters() racing process() was undefined behaviour. Every
//     published parameter is now a std::atomic, loaded once at the top of
//     process().
//   * a delay-time change had to use a crossfade so it does not cause a sudden
//     pitch change. The reference smoothed the time with a one-pole, which slides
//     the tail in pitch while the time moves. This version uses a dual-head
//     crossfade: each channel reads the old time and the new time and blends over
//     ~30 ms, so the tail keeps its pitch.
//   * the feedback path's stability had to be tested across signal levels. The
//     reference wrote the buffer through softLimit() = x/(1+|x|), which compresses
//     every sample and already loses 3.5 dB at a level of 0.5, so each repeat came
//     back quieter and duller. The limiter is now transparent below full scale and
//     only compresses above it; the loop is bounded and was checked at 0.1, 0.5,
//     1.0 and 2.0 input levels (see the tests).
//
// Also: bypass ramps the wet path instead of dropping it and the delay lines keep
// running, so the tail survives a bypass. In Mono mode both channels share the
// left time, since a mono repeat with two different times is not mono. The NaN
// and Inf guards inspect the bit pattern so they survive -ffast-math. The delay
// lines are allocated in prepare(), never in process().
class Echo {
public:
    enum class Mode : int {
        Stereo = 0,
        PingPong = 1,
        Mono = 2
    };

    static constexpr float MAX_DELAY_SECONDS = 5.0f;
    // ~30 ms crossfade when a delay time changes.
    static constexpr float XFADE_SECONDS = 0.030f;

    explicit Echo(float sampleRate = 48000.0f, float maxDelaySeconds = MAX_DELAY_SECONDS) {
        prepare(sampleRate, maxDelaySeconds);
    }

    bool prepare(float sampleRate, float maxDelaySeconds = MAX_DELAY_SECONDS) {
        if (sampleRate < 8000.0f || sampleRate > 384000.0f) return false;
        maxDelaySeconds = std::max(0.1f, std::min(maxDelaySeconds, 20.0f));

        sampleRate_ = sampleRate;
        bufferSize_ = (size_t) (sampleRate_ * maxDelaySeconds) + 8;

        bufferL_.assign(bufferSize_, 0.0f);
        bufferR_.assign(bufferSize_, 0.0f);

        maxDelaySamples_ = (float) (bufferSize_ - 4);
        maxDelayMs_ = maxDelaySamples_ * 1000.0f / sampleRate_;

        smooth_ = 1.0f - std::exp(-1.0f / (0.015f * sampleRate_));
        xfadeInc_ = 1.0f / (XFADE_SECONDS * sampleRate_);
        enableStep_ = 1.0f / (0.010f * sampleRate_);

        reset();
        return true;
    }

    void setParameters(float timeLeftMs, float timeRightMs, float feedback, float mix,
                       float tone, float width, int mode, float outputGain) {
        timeL_.store(clamp(timeLeftMs, 1.0f, maxDelayMs_), std::memory_order_relaxed);
        timeR_.store(clamp(timeRightMs, 1.0f, maxDelayMs_), std::memory_order_relaxed);
        feedback_.store(clamp(feedback, 0.0f, 0.95f), std::memory_order_relaxed);
        mix_.store(clamp(mix, 0.0f, 1.0f), std::memory_order_relaxed);
        tone_.store(clamp(tone, 0.0f, 1.0f), std::memory_order_relaxed);
        width_.store(clamp(width, 0.0f, 1.0f), std::memory_order_relaxed);
        mode_.store(mode < 0 ? 0 : (mode > 2 ? 2 : mode), std::memory_order_relaxed);
        gain_.store(clamp(outputGain, 0.0f, 2.0f), std::memory_order_relaxed);
    }

    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }

    void reset() {
        std::fill(bufferL_.begin(), bufferL_.end(), 0.0f);
        std::fill(bufferR_.begin(), bufferR_.end(), 0.0f);
        writeIndex_ = 0;
        lpL_ = lpR_ = 0.0f;

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
        const float toneTarget = tone_.load(std::memory_order_relaxed);
        const float widthTarget = width_.load(std::memory_order_relaxed);
        const int modeTarget = mode_.load(std::memory_order_relaxed);
        const float gainTarget = gain_.load(std::memory_order_relaxed);
        const float enableTarget = enabled_.load(std::memory_order_relaxed) ? 1.0f : 0.0f;

        if (!primed_) {
            feedbackSm_ = feedbackTarget;
            mixSm_ = mixTarget;
            toneSm_ = toneTarget;
            widthSm_ = widthTarget;
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
            toneSm_ += smooth_ * (toneTarget - toneSm_);
            widthSm_ += smooth_ * (widthTarget - widthSm_);
            gainSm_ += smooth_ * (gainTarget - gainSm_);
            enableRamp_ += std::min(1.0f, enableStep_) * (enableTarget - enableRamp_);

            // In Mono mode both channels share the left time.
            const float targetL = timeLTarget * 0.001f * sampleRate_;
            const float targetR = (modeTarget == 2 ? timeLTarget : timeRTarget) * 0.001f * sampleRate_;

            float inL = left[i];
            float inR = right[i];
            if (!isFinite(inL)) inL = 0.0f;
            if (!isFinite(inR)) inR = 0.0f;

            const float echoL = readHead(bufferL_, headAL_, headBL_, xfadeL_, targetL);
            const float echoR = readHead(bufferR_, headAR_, headBR_, xfadeR_, targetR);

            // Tone: a one-pole low-pass on the feedback path, swept dark to
            // bright. 0.002 is about 15 Hz, 0.79 about 12 kHz at 48 kHz.
            const float toneCoefficient = 0.002f + toneSm_ * 0.788f;
            lpL_ += toneCoefficient * (echoL - lpL_);
            lpR_ += toneCoefficient * (echoR - lpR_);

            float writeL, writeR;
            switch (modeTarget) {
                case 1:  // Ping-pong: the tails swap channels.
                    writeL = inL + lpR_ * feedbackSm_;
                    writeR = inR + lpL_ * feedbackSm_;
                    break;
                case 2: {  // Mono: one repeat, fed to both channels.
                    const float monoInput = 0.5f * (inL + inR);
                    const float monoEcho = 0.5f * (lpL_ + lpR_);
                    writeL = monoInput + monoEcho * feedbackSm_;
                    writeR = writeL;
                    break;
                }
                default:  // Stereo
                    writeL = inL + lpL_ * feedbackSm_;
                    writeR = inR + lpR_ * feedbackSm_;
                    break;
            }

            bufferL_[writeIndex_] = protect(writeL);
            bufferR_[writeIndex_] = protect(writeR);

            // Stereo width: mid/side scaling of the wet signal.
            const float mid = 0.5f * (echoL + echoR);
            const float side = 0.5f * (echoL - echoR);
            const float wetL = mid + side * widthSm_;
            const float wetR = mid - side * widthSm_;

            const float dryGain = std::cos(mixSm_ * 1.57079632679f);
            const float wetGain = std::sin(mixSm_ * 1.57079632679f);

            const float mixedL = (inL * dryGain + wetL * wetGain) * gainSm_;
            const float mixedR = (inR * dryGain + wetR * wetGain) * gainSm_;

            // Blend dry in as the ramp falls, so a bypass is click free.
            const float outL = inL + (mixedL - inL) * enableRamp_;
            const float outR = inR + (mixedR - inR) * enableRamp_;

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

    // Transparent below full scale; compresses smoothly above it, so the feedback
    // loop is bounded without dulling ordinary levels the way x/(1+|x|) did.
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

    float sampleRate_ = 48000.0f;
    size_t bufferSize_ = 0;
    float maxDelaySamples_ = 0.0f;
    float maxDelayMs_ = 5000.0f;
    float smooth_ = 0.0f;
    float xfadeInc_ = 0.0f;
    float enableStep_ = 0.0f;

    std::vector<float> bufferL_;
    std::vector<float> bufferR_;
    size_t writeIndex_ = 0;

    // Dual read heads per channel and their crossfade position.
    float headAL_ = 0.0f, headBL_ = 0.0f, xfadeL_ = 1.0f;
    float headAR_ = 0.0f, headBR_ = 0.0f, xfadeR_ = 1.0f;

    float lpL_ = 0.0f, lpR_ = 0.0f;

    std::atomic<float> timeL_{300.0f};
    std::atomic<float> timeR_{450.0f};
    std::atomic<float> feedback_{0.40f};
    std::atomic<float> mix_{0.35f};
    std::atomic<float> tone_{0.65f};
    std::atomic<float> width_{1.0f};
    std::atomic<int> mode_{0};
    std::atomic<float> gain_{1.0f};
    std::atomic<bool> enabled_{true};

    // Smoothed copies, written only by the audio thread.
    float feedbackSm_ = 0.40f, mixSm_ = 0.35f, toneSm_ = 0.65f;
    float widthSm_ = 1.0f, gainSm_ = 1.0f;
    float enableRamp_ = 1.0f;
    bool primed_ = false;
};

#endif // ALTODFX_ECHO_H
