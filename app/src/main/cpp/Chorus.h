#ifndef ALTODFX_CHORUS_H
#define ALTODFX_CHORUS_H

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

// Stereo chorus for the ALTODFX rack: a modulated fractional delay line per
// channel, a sine or triangle LFO with a phase offset between the channels,
// bounded cross-routed feedback and equal-power dry/wet.
//
// Based on the reference "Chorus" DSP. The reference's own note said it was not
// production ready because setParameters() was not safe to call while the audio
// callback ran, and that the parameters had to be published through a
// thread-safe mechanism. That is the change at the centre of this file:
//   * every published parameter is now a std::atomic, loaded once at the top of
//     process() into plain locals, so the control thread can call the setters at
//     any time. This is the atomic-snapshot mechanism the note asks for, without
//     the allocation an SPSC queue would need.
//   * the reference kept `waveform_` and `enabled_` in plain members and read
//     them straight from the audio loop, so a change raced the audio thread and
//     stepped the delay. Waveform is now an atomic int and changes with a short
//     crossfade between the old and new LFO shapes; enabled now ramps the wet
//     path in and out, so neither clicks.
//   * the reference used std::vector for the delay lines, so the object
//     allocated and the buffer size was a runtime value. Fixed-size arrays are
//     used instead, so nothing allocates and process() is real-time safe.
//   * the reference re-derived the phase offset from a double every sample and
//     wrapped it with loops; the phase is now a single float accumulator wrapped
//     with one subtraction, and the right-channel offset is a plain add.
//   * the NaN/Inf guards inspect the bit pattern so they survive -ffast-math.
//
// The delay lines are per channel and the class never allocates, so process() is
// safe from the audio thread.
class Chorus {
public:
    enum class Waveform : int {
        Sine = 0,
        Triangle = 1
    };

    static constexpr int MAX_DELAY_MS = 60;
    // Enough for 60 ms at 384 kHz, plus the interpolation tail.
    static constexpr int MAX_DELAY_SAMPLES = 384000 * MAX_DELAY_MS / 1000 + 8;

    explicit Chorus(float sampleRate = 48000.0f) {
        setSampleRate(sampleRate);
        reset();
    }

    void setSampleRate(float sampleRate) {
        sampleRate_ = std::max(8000.0f, sampleRate);
        // ~15 ms glides for the control parameters.
        smooth_ = 1.0f - std::exp(-1.0f / (0.015f * sampleRate_));
        // ~10 ms crossfade for a waveform change.
        waveformStep_ = 1.0f / (0.010f * sampleRate_);
        // ~10 ms ramp for enable/disable.
        enableStep_ = 1.0f / (0.010f * sampleRate_);
    }

    void setParameters(float rateHz, float depth, float baseDelayMs, float feedback,
                       float mix, float stereoWidth, float outputGain) {
        rate_.store(clamp(rateHz, 0.01f, 8.0f), std::memory_order_relaxed);
        depth_.store(clamp(depth, 0.0f, 1.0f), std::memory_order_relaxed);
        baseDelay_.store(clamp(baseDelayMs, 5.0f, 40.0f), std::memory_order_relaxed);
        feedback_.store(clamp(feedback, -0.85f, 0.85f), std::memory_order_relaxed);
        mix_.store(clamp(mix, 0.0f, 1.0f), std::memory_order_relaxed);
        stereoWidth_.store(clamp(stereoWidth, 0.0f, 1.0f), std::memory_order_relaxed);
        outputGain_.store(clamp(outputGain, 0.0f, 2.0f), std::memory_order_relaxed);
    }

    // 0 = sine, 1 = triangle.
    void setWaveform(int waveform) {
        waveform_.store(std::max(0, std::min(waveform, 1)), std::memory_order_relaxed);
    }

    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }

    void reset() {
        std::memset(delayL_, 0, sizeof(delayL_));
        std::memset(delayR_, 0, sizeof(delayR_));
        writeIndex_ = 0;
        phase_ = 0.0f;
        waveformCurrent_ = 0;
        waveformPrevious_ = 0;
        waveformBlend_ = 1.0f;
        enableRamp_ = 1.0f;
        primed_ = false;
    }

    // Stereo, separate per-channel buffers.
    void process(float *left, float *right, int32_t numFrames) {
        if (left == nullptr || right == nullptr || numFrames <= 0) return;

        const float rateTarget = rate_.load(std::memory_order_relaxed);
        const float depthTarget = depth_.load(std::memory_order_relaxed);
        const float baseDelayTarget = baseDelay_.load(std::memory_order_relaxed);
        const float feedbackTarget = feedback_.load(std::memory_order_relaxed);
        const float mixTarget = mix_.load(std::memory_order_relaxed);
        const float widthTarget = stereoWidth_.load(std::memory_order_relaxed);
        const float gainTarget = outputGain_.load(std::memory_order_relaxed);
        const int waveformTarget = waveform_.load(std::memory_order_relaxed);
        const float enableTarget = enabled_.load(std::memory_order_relaxed) ? 1.0f : 0.0f;

        if (!primed_) {
            depthSm_ = depthTarget; baseDelaySm_ = baseDelayTarget;
            feedbackSm_ = feedbackTarget; mixSm_ = mixTarget;
            widthSm_ = widthTarget; gainSm_ = gainTarget;
            waveformCurrent_ = waveformTarget;
            enableRamp_ = enableTarget;
            primed_ = true;
        }

        const float maxDelay = (float) (MAX_DELAY_SAMPLES - 4);

        for (int32_t i = 0; i < numFrames; ++i) {
            depthSm_ += smooth_ * (depthTarget - depthSm_);
            baseDelaySm_ += smooth_ * (baseDelayTarget - baseDelaySm_);
            feedbackSm_ += smooth_ * (feedbackTarget - feedbackSm_);
            mixSm_ += smooth_ * (mixTarget - mixSm_);
            widthSm_ += smooth_ * (widthTarget - widthSm_);
            gainSm_ += smooth_ * (gainTarget - gainSm_);
            // The LFO rate is used through the phase accumulator, so a rate
            // change glides rather than resetting the phase.
            phase_ += rateTarget / sampleRate_;
            if (phase_ >= 1.0f) phase_ -= 1.0f;

            // Crossfade the LFO shape when the waveform changes, so the delay
            // does not step.
            if (waveformTarget != waveformCurrent_) {
                waveformPrevious_ = waveformCurrent_;
                waveformCurrent_ = waveformTarget;
                waveformBlend_ = 0.0f;
            }
            if (waveformBlend_ < 1.0f) {
                waveformBlend_ = std::min(1.0f, waveformBlend_ + waveformStep_);
            }

            // Ramp the wet path in and out so enabling never clicks.
            enableRamp_ += std::min(1.0f, enableStep_) * (enableTarget - enableRamp_);

            float lfoRPhase = phase_ + widthSm_ * 0.25f;
            lfoRPhase -= std::floor(lfoRPhase);

            const float lfoL = lfoAt(phase_);
            const float lfoR = lfoAt(lfoRPhase);

            const float baseSamples = baseDelaySm_ * 0.001f * sampleRate_;
            const float modulation = depthSm_ * baseSamples * 0.85f;
            const float delaySamplesL = clamp(baseSamples + lfoL * modulation, 2.0f, maxDelay);
            const float delaySamplesR = clamp(baseSamples + lfoR * modulation, 2.0f, maxDelay);

            const int index = (int) writeIndex_;

            float inL = left[i];
            float inR = right[i];
            if (!isFinite(inL)) inL = 0.0f;
            if (!isFinite(inR)) inR = 0.0f;

            const float delayedL = readDelay(delayL_, delaySamplesL);
            const float delayedR = readDelay(delayR_, delaySamplesR);

            // Cross-channel feedback is what gives the stereo movement; the soft
            // limit on the write keeps the loop bounded.
            delayL_[index] = softLimit(inL + delayedR * feedbackSm_);
            delayR_[index] = softLimit(inR + delayedL * feedbackSm_);

            const float dryGain = std::cos(mixSm_ * 1.57079632679f);
            const float wetGain = std::sin(mixSm_ * 1.57079632679f);

            const float wetOutL = (inL * dryGain + delayedL * wetGain) * gainSm_;
            const float wetOutR = (inR * dryGain + delayedR * wetGain) * gainSm_;

            // Blend dry in as the ramp falls, so a bypass is click free.
            const float outL = inL + (wetOutL - inL) * enableRamp_;
            const float outR = inR + (wetOutR - inR) * enableRamp_;

            left[i] = isFinite(outL) ? outL : inL;
            right[i] = isFinite(outR) ? outR : inR;

            ++writeIndex_;
            if (writeIndex_ >= MAX_DELAY_SAMPLES) writeIndex_ = 0;
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

    static float softLimit(float x) {
        return x / (1.0f + std::fabs(x));
    }

    static float lfoShape(int waveform, float phase) {
        if (waveform == 1)  // Triangle
            return 1.0f - 4.0f * std::fabs(phase - 0.5f);
        return std::sin(phase * 6.283185307179586f);  // Sine
    }

    // Blends the previous and current shapes while a change is in flight.
    float lfoAt(float phase) const {
        float p = phase;
        p -= std::floor(p);
        const float current = lfoShape(waveformCurrent_, p);
        if (waveformBlend_ >= 1.0f) return current;
        const float previous = lfoShape(waveformPrevious_, p);
        return previous + (current - previous) * waveformBlend_;
    }

    // Cubic Hermite read at a fractional distance behind the write head.
    float readDelay(const float *buffer, float delaySamples) const {
        const float size = (float) MAX_DELAY_SAMPLES;
        float position = (float) writeIndex_ - delaySamples;
        if (position < 0.0f) position += size;

        const int i1 = (int) position;
        const float t = position - (float) i1;
        const int i0 = (i1 - 1 + MAX_DELAY_SAMPLES) % MAX_DELAY_SAMPLES;
        const int i2 = (i1 + 1) % MAX_DELAY_SAMPLES;
        const int i3 = (i1 + 2) % MAX_DELAY_SAMPLES;

        const float y0 = buffer[i0], y1 = buffer[i1];
        const float y2 = buffer[i2], y3 = buffer[i3];

        const float c0 = y1;
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * t + c2) * t + c1) * t + c0;
    }

    float sampleRate_ = 48000.0f;
    float smooth_ = 0.0f;
    float waveformStep_ = 0.0f;
    float enableStep_ = 0.0f;

    float phase_ = 0.0f;
    size_t writeIndex_ = 0;
    bool primed_ = false;

    float delayL_[MAX_DELAY_SAMPLES];
    float delayR_[MAX_DELAY_SAMPLES];

    std::atomic<float> rate_{0.8f};
    std::atomic<float> depth_{0.65f};
    std::atomic<float> baseDelay_{18.0f};
    std::atomic<float> feedback_{0.0f};
    std::atomic<float> mix_{0.35f};
    std::atomic<float> stereoWidth_{1.0f};
    std::atomic<float> outputGain_{1.0f};
    std::atomic<int> waveform_{0};
    std::atomic<bool> enabled_{true};

    // Smoothed copies, written only by the audio thread.
    float depthSm_ = 0.65f, baseDelaySm_ = 18.0f, feedbackSm_ = 0.0f;
    float mixSm_ = 0.35f, widthSm_ = 1.0f, gainSm_ = 1.0f;
    float enableRamp_ = 1.0f;
    int waveformCurrent_ = 0, waveformPrevious_ = 0;
    float waveformBlend_ = 1.0f;
};

#endif // ALTODFX_CHORUS_H
