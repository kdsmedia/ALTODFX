#ifndef ALTODFX_TREMOLO_H
#define ALTODFX_TREMOLO_H

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

// Amplitude tremolo for the ALTODFX rack: a sine, triangle, square, saw or
// sample-and-hold LFO modulates the gain (never the pitch), with a stereo phase
// offset and a bias control.
//
// Based on the reference "Tremolo" DSP. The reference's own notes flagged two
// things, both addressed here:
//   * setParameters() and process() had to communicate through a thread-safe
//     mechanism. The live parameters lived in plain members, so a control-thread
//     write raced the audio thread. Every published parameter is now a
//     std::atomic, loaded once at the top of process() into plain locals.
//   * sample-and-hold needed special handling for the stereo phase offset so
//     both channels produce a consistent random pattern. The reference advanced
//     two independent xorshift draws, each on its own channel's phase wrap, so
//     with any stereo offset the two channels held unrelated noise. The hold
//     value is now a pure hash of the LFO cycle index, computed per channel from
//     that channel's own cycle count. Both channels therefore read from the same
//     random sequence, just offset in time by the stereo width, which is the
//     coherent pattern the note asks for.
//   * waveform and enabled were plain members read straight from the audio loop,
//     so a change stepped the gain. Waveform is now an atomic int and changes
//     with a short crossfade between the old and new shapes; enabled now ramps
//     the modulation depth to zero and back, so neither clicks.
//   * the NaN/Inf guards inspect the bit pattern so they survive -ffast-math.
//
// The class is stereo, holds no allocation and its process() touches only plain
// members, so it is safe from the audio thread.
class Tremolo {
public:
    enum class Waveform : int {
        Sine = 0,
        Triangle,
        Square,
        SawUp,
        SawDown,
        SampleHold
    };

    explicit Tremolo(float sampleRate = 48000.0f) {
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

    void setParameters(float rateHz, float depth, float bias, float stereoWidth, float outputGain) {
        rate_.store(clamp(rateHz, 0.01f, 20.0f), std::memory_order_relaxed);
        depth_.store(clamp(depth, 0.0f, 1.0f), std::memory_order_relaxed);
        bias_.store(clamp(bias, -1.0f, 1.0f), std::memory_order_relaxed);
        stereoWidth_.store(clamp(stereoWidth, 0.0f, 1.0f), std::memory_order_relaxed);
        outputGain_.store(clamp(outputGain, 0.0f, 2.0f), std::memory_order_relaxed);
    }

    // 0 = sine, 1 = triangle, 2 = square, 3 = saw up, 4 = saw down, 5 = S&H.
    void setWaveform(int waveform) {
        waveform_.store(std::max(0, std::min(waveform, 5)), std::memory_order_relaxed);
    }

    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }

    void reset() {
        totalPhaseL_ = 0.0f;
        cycleL_ = 0;
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
        const float biasTarget = bias_.load(std::memory_order_relaxed);
        const float widthTarget = stereoWidth_.load(std::memory_order_relaxed);
        const float gainTarget = outputGain_.load(std::memory_order_relaxed);
        const int waveformTarget = waveform_.load(std::memory_order_relaxed);
        const float enableTarget = enabled_.load(std::memory_order_relaxed) ? 1.0f : 0.0f;

        if (!primed_) {
            depthSm_ = depthTarget; biasSm_ = biasTarget;
            widthSm_ = widthTarget; gainSm_ = gainTarget;
            waveformCurrent_ = waveformTarget;
            enableRamp_ = enableTarget;
            primed_ = true;
        }

        const float increment = rateTarget / sampleRate_;

        for (int32_t i = 0; i < numFrames; ++i) {
            depthSm_ += smooth_ * (depthTarget - depthSm_);
            biasSm_ += smooth_ * (biasTarget - biasSm_);
            widthSm_ += smooth_ * (widthTarget - widthSm_);
            gainSm_ += smooth_ * (gainTarget - gainSm_);

            totalPhaseL_ += increment;
            if (totalPhaseL_ >= 1.0f) { totalPhaseL_ -= 1.0f; ++cycleL_; }

            // The right channel is the same LFO, offset by up to half a cycle.
            float phaseR = totalPhaseL_ + widthSm_ * 0.5f;
            uint32_t cycleR = cycleL_;
            if (phaseR >= 1.0f) { phaseR -= 1.0f; ++cycleR; }

            // Crossfade the LFO shape when the waveform changes, so the gain
            // does not step.
            if (waveformTarget != waveformCurrent_) {
                waveformPrevious_ = waveformCurrent_;
                waveformCurrent_ = waveformTarget;
                waveformBlend_ = 0.0f;
            }
            if (waveformBlend_ < 1.0f) {
                waveformBlend_ = std::min(1.0f, waveformBlend_ + waveformStep_);
            }

            // Ramp the modulation in and out so enabling never clicks.
            enableRamp_ += std::min(1.0f, enableStep_) * (enableTarget - enableRamp_);

            float inL = left[i];
            float inR = right[i];
            if (!isFinite(inL)) inL = 0.0f;
            if (!isFinite(inR)) inR = 0.0f;

            // Both channels hash the cycle index, so a sample-and-hold step draws
            // the same random value on each side; only the phase differs.
            const float lfoL = lfoAt(totalPhaseL_, cycleL_);
            const float lfoR = lfoAt(phaseR, cycleR);

            // Bipolar LFO to a unipolar gain between 1-depth and 1.
            const float modL = 0.5f * (lfoL + 1.0f);
            const float modR = 0.5f * (lfoR + 1.0f);

            float gainL = (1.0f - depthSm_) + depthSm_ * modL;
            float gainR = (1.0f - depthSm_) + depthSm_ * modR;

            // Bias shifts the whole curve up or down.
            gainL = std::max(0.0f, std::min(1.5f, gainL + biasSm_ * depthSm_ * 0.5f));
            gainR = std::max(0.0f, std::min(1.5f, gainR + biasSm_ * depthSm_ * 0.5f));

            // Blend the modulated signal with the dry one as the ramp falls, so a
            // bypass is click free.
            const float wetL = inL * gainL * gainSm_;
            const float wetR = inR * gainR * gainSm_;
            const float outL = inL + (wetL - inL) * enableRamp_;
            const float outR = inR + (wetR - inR) * enableRamp_;

            left[i] = isFinite(outL) ? outL : inL;
            right[i] = isFinite(outR) ? outR : inR;
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

    static float shape(int waveform, float phase, uint32_t cycle) {
        switch (waveform) {
            case 1: return 1.0f - 4.0f * std::fabs(phase - 0.5f);  // Triangle
            case 2: return phase < 0.5f ? 1.0f : -1.0f;            // Square
            case 3: return 2.0f * phase - 1.0f;                    // Saw up
            case 4: return 1.0f - 2.0f * phase;                    // Saw down
            case 5: return randomForStep(cycle);                   // S&H
            default: return std::sin(phase * 6.283185307179586f);  // Sine
        }
    }

    // Deterministic value in [-1, 1) for a cycle index. Hashing the index (rather
    // than drawing from a running generator) means both channels can ask for the
    // value of a given step independently and always agree, which is what makes
    // the stereo sample-and-hold coherent.
    static float randomForStep(uint32_t step) {
        uint32_t x = step * 0x9E3779B1u + 0x85EBCA6Bu;
        x ^= x >> 16;
        x *= 0x7FEB352Du;
        x ^= x >> 15;
        x *= 0x846CA68Bu;
        x ^= x >> 16;
        return (float) (x >> 8) * (2.0f / 16777216.0f) - 1.0f;
    }

    // Blends the previous and current shapes while a change is in flight.
    float lfoAt(float phase, uint32_t cycle) const {
        const float p = phase - std::floor(phase);
        const float current = shape(waveformCurrent_, p, cycle);
        if (waveformBlend_ >= 1.0f) return current;
        const float previous = shape(waveformPrevious_, p, cycle);
        return previous + (current - previous) * waveformBlend_;
    }

    float sampleRate_ = 48000.0f;
    float smooth_ = 0.0f;
    float waveformStep_ = 0.0f;
    float enableStep_ = 0.0f;

    float totalPhaseL_ = 0.0f;
    uint32_t cycleL_ = 0;
    bool primed_ = false;

    std::atomic<float> rate_{4.0f};
    std::atomic<float> depth_{0.75f};
    std::atomic<float> bias_{0.0f};
    std::atomic<float> stereoWidth_{0.0f};
    std::atomic<float> outputGain_{1.0f};
    std::atomic<int> waveform_{0};
    std::atomic<bool> enabled_{true};

    // Smoothed copies, written only by the audio thread.
    float depthSm_ = 0.75f, biasSm_ = 0.0f, widthSm_ = 0.0f, gainSm_ = 1.0f;
    float enableRamp_ = 1.0f;
    int waveformCurrent_ = 0, waveformPrevious_ = 0;
    float waveformBlend_ = 1.0f;
};

#endif // ALTODFX_TREMOLO_H
