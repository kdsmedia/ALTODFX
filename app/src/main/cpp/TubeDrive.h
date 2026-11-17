#ifndef ALTODFX_TUBE_DRIVE_H
#define ALTODFX_TUBE_DRIVE_H

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

// Tube-style overdrive for the ALTODFX rack.
//
// Based on the reference "Overdrive" DSP, adapted to this host:
//   * the engine's stream is mono (oboe::ChannelCount::Mono), so this effect is
//     mono as well; the reference class processed interleaved stereo;
//   * the reference oversampler was linear interpolation, which does not reject
//     images; it is replaced by a polyphase FIR (windowed sinc) interpolator and
//     a matching decimator, so the 4x rate is genuinely anti-aliased;
//   * Drive/Tone/Level are smoothed with one-pole filters, so turning a knob
//     while playing does not click.
//
// Nothing here allocates: all state is fixed-size and process() is safe to call
// from the audio thread.
class TubeDrive {
public:
    explicit TubeDrive(float sampleRate = 48000.0f) {
        rebuildOversampling();
        setSampleRate(sampleRate);
        reset();
    }

    void setSampleRate(float sampleRate) {
        sampleRate_ = std::max(8000.0f, sampleRate);
        gainSmoothCoef_ = 1.0f - std::exp(-1.0f / (0.010f * sampleRate_));
        toneSmoothCoef_ = 1.0f - std::exp(-1.0f / (0.020f * sampleRate_));

        const float drive = drive_.load(std::memory_order_relaxed);
        const float tone = tone_.load(std::memory_order_relaxed);
        const float level = level_.load(std::memory_order_relaxed);
        inputGain_ = 1.0f + drive * 14.0f;
        outputGain_ = 0.20f + level * 1.80f;
        cutoff_ = 1800.0f + tone * 9000.0f;

        preFilter_.setHighPass(sampleRate_, 90.0f);
        dcFilter_.setHighPass(sampleRate_, 20.0f);
        toneFilter_.setLowPass(sampleRate_, cutoff_);
    }

    void setParameters(float drive, float tone, float level) {
        drive_.store(clamp01(drive), std::memory_order_relaxed);
        tone_.store(clamp01(tone), std::memory_order_relaxed);
        level_.store(clamp01(level), std::memory_order_relaxed);
    }

    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }

    void reset() {
        preFilter_.reset();
        toneFilter_.reset();
        dcFilter_.reset();
        for (int i = 0; i < kTapsPerPhase; ++i) upHistory_[i] = 0.0f;
        for (int i = 0; i < kPrototypeLen; ++i) downHistory_[i] = 0.0f;
        decimPhase_ = 0;
        primed_ = false;
    }

    // In-place, mono: audio[0..numFrames-1].
    void process(float *audio, int32_t numFrames) {
        if (audio == nullptr || numFrames <= 0) return;
        if (!enabled_.load(std::memory_order_relaxed)) return;

        const float driveTarget = drive_.load(std::memory_order_relaxed);
        const float toneTarget = tone_.load(std::memory_order_relaxed);
        const float levelTarget = level_.load(std::memory_order_relaxed);
        const float inputGainTarget = 1.0f + driveTarget * 14.0f;
        const float outputGainTarget = 0.20f + levelTarget * 1.80f;
        const float cutoffTarget = 1800.0f + toneTarget * 9000.0f;

        // Jump straight to the requested values on the first block so a freshly
        // loaded patch does not fade in; smooth only subsequent changes.
        if (!primed_) {
            inputGain_ = inputGainTarget;
            outputGain_ = outputGainTarget;
            cutoff_ = cutoffTarget;
            toneFilter_.setLowPass(sampleRate_, cutoff_);
            primed_ = true;
        }

        for (int32_t i = 0; i < numFrames; ++i) {
            inputGain_ += gainSmoothCoef_ * (inputGainTarget - inputGain_);
            outputGain_ += gainSmoothCoef_ * (outputGainTarget - outputGain_);
            cutoff_ += toneSmoothCoef_ * (cutoffTarget - cutoff_);
            // Recomputing the tone coefficients every 32 samples keeps the
            // per-sample cost low; the cutoff itself is already smoothed.
            if ((i & 31) == 0) toneFilter_.setLowPass(sampleRate_, cutoff_);

            float x = audio[i];
            if (!isFinite(x)) x = 0.0f;

            x = preFilter_.process(x);
            x *= inputGain_;

            const float shaped = oversampledClip(x);

            float out = toneFilter_.process(shaped);
            out = dcFilter_.process(out);
            out *= outputGain_;
            out = std::tanh(out);

            audio[i] = isFinite(out) ? out : 0.0f;
        }
    }

private:
    static constexpr int kL = 4;
    static constexpr int kTapsPerPhase = 16;
    static constexpr int kPrototypeLen = kL * kTapsPerPhase;

    static float clamp01(float value) {
        if (!isFinite(value)) return 0.0f;
        return std::max(0.0f, std::min(1.0f, value));
    }

    // std::isfinite() is folded away under -ffast-math, so a NaN or Inf would
    // slip through and poison the filter state. Inspect the bit pattern instead,
    // which behaves the same at every optimisation level.
    static bool isFinite(float value) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        return (bits & 0x7F800000u) != 0x7F800000u;
    }

    struct Biquad {
        void setLowPass(float sampleRate, float cutoff) {
            const float pi = 3.14159265358979323846f;
            const float w = 2.0f * pi * std::min(cutoff, sampleRate * 0.45f) / sampleRate;
            const float c = std::cos(w);
            const float s = std::sin(w);
            const float alpha = s * 0.70710678f;
            const float a0 = 1.0f + alpha;
            b0 = ((1.0f - c) * 0.5f) / a0;
            b1 = (1.0f - c) / a0;
            b2 = b0;
            a1 = (-2.0f * c) / a0;
            a2 = (1.0f - alpha) / a0;
        }

        void setHighPass(float sampleRate, float cutoff) {
            const float pi = 3.14159265358979323846f;
            const float w = 2.0f * pi * std::min(cutoff, sampleRate * 0.45f) / sampleRate;
            const float c = std::cos(w);
            const float s = std::sin(w);
            const float alpha = s * 0.70710678f;
            const float a0 = 1.0f + alpha;
            b0 = ((1.0f + c) * 0.5f) / a0;
            b1 = -(1.0f + c) / a0;
            b2 = b0;
            a1 = (-2.0f * c) / a0;
            a2 = (1.0f - alpha) / a0;
        }

        float process(float x) {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return isFinite(y) ? y : 0.0f;
        }

        void reset() {
            z1 = 0.0f;
            z2 = 0.0f;
        }

        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f, z1 = 0.0f, z2 = 0.0f;
    };

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

    float oversampledClip(float x) {
        for (int k = kTapsPerPhase - 1; k > 0; --k) upHistory_[k] = upHistory_[k - 1];
        upHistory_[0] = x;

        float out = 0.0f;
        for (int j = 0; j < kL; ++j) {
            float acc = 0.0f;
            for (int k = 0; k < kTapsPerPhase; ++k) acc += phases_[j][k] * upHistory_[k];

            const float u = std::tanh(acc);

            for (int m = kPrototypeLen - 1; m > 0; --m) downHistory_[m] = downHistory_[m - 1];
            downHistory_[0] = u;

            if (++decimPhase_ == kL) {
                decimPhase_ = 0;
                float acc2 = 0.0f;
                for (int m = 0; m < kPrototypeLen; ++m) acc2 += decim_[m] * downHistory_[m];
                out = acc2;
            }
        }

        return out;
    }

    float sampleRate_ = 48000.0f;
    float gainSmoothCoef_ = 0.0f;
    float toneSmoothCoef_ = 0.0f;
    float inputGain_ = 1.0f;
    float outputGain_ = 1.0f;
    float cutoff_ = 6500.0f;
    int decimPhase_ = 0;
    bool primed_ = false;

    float phases_[kL][kTapsPerPhase] = {};
    float decim_[kPrototypeLen] = {};
    float upHistory_[kTapsPerPhase] = {};
    float downHistory_[kPrototypeLen] = {};

    Biquad preFilter_;
    Biquad toneFilter_;
    Biquad dcFilter_;

    std::atomic<float> drive_{0.45f};
    std::atomic<float> tone_{0.55f};
    std::atomic<float> level_{0.75f};
    std::atomic<bool> enabled_{true};
};

#endif // ALTODFX_TUBE_DRIVE_H
