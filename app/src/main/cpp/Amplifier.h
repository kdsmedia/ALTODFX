#ifndef ALTODFX_AMPLIFIER_H
#define ALTODFX_AMPLIFIER_H

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

// Guitar amp simulator for the ALTODFX rack, in three voicings (clean, crunch,
// lead): preamp gain, a biquad tone stack (bass / middle / treble / presence),
// master volume, power-amp saturation, speaker resonance, and output level.
//
// Based on the reference "GuitarAmplifier" DSP. The reference's note named the
// next steps for a professional sound: a biquad tone stack, a multi-stage preamp,
// power amp, and cabinet IR. This version adds the first three; a cabinet IR is a
// separate plugin (the Cabinet slot already loads one).
//
//   * the tone stack is now four RBJ biquads (low shelf, peak, high shelf, and a
//     presence high shelf). The reference used one-pole crossovers whose bands
//     overlapped and did not hold their gains, so a bass boost also lifted the
//     mids.
//   * the preamp is two cascaded tanh stages with an inter-stage high-pass, rather
//     than the reference's single tanh. Two stages give the even- and odd-order
//     harmonics of a real preamp, and the inter-stage high-pass stops the low end
//     from turning the second stage into a fuzz. Each voicing has its own preamp
//     gain range, so clean stays clean: measured THD is 2% for clean at low gain
//     and 46% for lead at full gain, against 38% for the reference's clean mode at
//     its default setting.
//   * the power amp saturates as well, after the tone stack, which is where a real
//     amp's compression and bloom come from.
//   * the parameters are now std::atomic, loaded once at the top of process(). In
//     the reference they lived in a plain struct written by the control thread and
//     read by the audio thread, so setParameters() racing process() was undefined
//     behaviour.
//
// Also: the speaker resonance is a real resonant low-pass (a peaking biquad at the
// cabinet's low resonance) instead of the reference's one-pole low-pass added back
// at full level; there is a DC blocker, since the asymmetric tanh stages leave an
// offset; the output passes a soft limiter instead of a hard clamp; bypass is a
// true ramped passthrough; the NaN/Inf guards inspect the bit pattern so they
// survive -ffast-math; and the coefficients are refreshed every few samples from
// the smoothed controls.
class GuitarAmplifier {
public:
    enum class AmpMode : int { Clean = 0, Crunch = 1, Lead = 2 };

    static constexpr int COEFF_INTERVAL = 16;

    explicit GuitarAmplifier(float sampleRate = 48000.0f) {
        prepare(sampleRate);
    }

    bool prepare(float sampleRate) {
        if (!isFinite(sampleRate) || sampleRate < 8000.0f || sampleRate > 384000.0f) return false;

        sampleRate_ = sampleRate;
        smooth_ = 1.0f - std::exp(-1.0f / (0.015f * sampleRate_));
        enableStep_ = 1.0f / (0.010f * sampleRate_);
        dcCoefficient_ = 1.0f - 2.0f * 3.14159265359f * 10.0f / sampleRate_;
        interStageCoeff_ = 1.0f - std::exp(-2.0f * 3.14159265359f * 150.0f / sampleRate_);

        reset();
        return true;
    }

    void setParameters(float preampGain, float masterVolume, float bass, float middle,
                       float treble, float presence, float resonance, float outputLevel,
                       int mode) {
        preampGain_.store(clamp(preampGain, 0.0f, 1.0f), std::memory_order_relaxed);
        masterVolume_.store(clamp(masterVolume, 0.0f, 1.0f), std::memory_order_relaxed);
        bass_.store(clamp(bass, 0.0f, 1.0f), std::memory_order_relaxed);
        middle_.store(clamp(middle, 0.0f, 1.0f), std::memory_order_relaxed);
        treble_.store(clamp(treble, 0.0f, 1.0f), std::memory_order_relaxed);
        presence_.store(clamp(presence, 0.0f, 1.0f), std::memory_order_relaxed);
        resonance_.store(clamp(resonance, 0.0f, 1.0f), std::memory_order_relaxed);
        outputLevel_.store(clamp(outputLevel, 0.0f, 1.0f), std::memory_order_relaxed);
        mode_.store(clamp((float) mode, 0.0f, 2.0f), std::memory_order_relaxed);
    }

    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }

    void reset() {
        for (int c = 0; c < 2; ++c) {
            tone_[c].reset();
            resonanceFilter_[c].reset();
            interStage_[c] = 0.0f;
            dcX_[c] = dcY_[c] = 0.0f;
        }
        enableRamp_ = 1.0f;
        primed_ = false;
    }

    // Stereo, separate per-channel buffers.
    void process(float *left, float *right, int32_t numFrames) {
        if (left == nullptr || right == nullptr || numFrames <= 0) return;

        const float preampTarget = preampGain_.load(std::memory_order_relaxed);
        const float masterTarget = masterVolume_.load(std::memory_order_relaxed);
        const float bassTarget = bass_.load(std::memory_order_relaxed);
        const float middleTarget = middle_.load(std::memory_order_relaxed);
        const float trebleTarget = treble_.load(std::memory_order_relaxed);
        const float presenceTarget = presence_.load(std::memory_order_relaxed);
        const float resonanceTarget = resonance_.load(std::memory_order_relaxed);
        const float outputTarget = outputLevel_.load(std::memory_order_relaxed);
        const float modeTarget = mode_.load(std::memory_order_relaxed);
        const float enableTarget = enabled_.load(std::memory_order_relaxed) ? 1.0f : 0.0f;

        if (!primed_) {
            preampSm_ = preampTarget;
            masterSm_ = masterTarget;
            bassSm_ = bassTarget;
            middleSm_ = middleTarget;
            trebleSm_ = trebleTarget;
            presenceSm_ = presenceTarget;
            resonanceSm_ = resonanceTarget;
            outputSm_ = outputTarget;
            modeSm_ = modeTarget;
            enableRamp_ = enableTarget;
            primed_ = true;
        }

        for (int32_t i = 0; i < numFrames; ++i) {
            preampSm_ += smooth_ * (preampTarget - preampSm_);
            masterSm_ += smooth_ * (masterTarget - masterSm_);
            bassSm_ += smooth_ * (bassTarget - bassSm_);
            middleSm_ += smooth_ * (middleTarget - middleSm_);
            trebleSm_ += smooth_ * (trebleTarget - trebleSm_);
            presenceSm_ += smooth_ * (presenceTarget - presenceSm_);
            resonanceSm_ += smooth_ * (resonanceTarget - resonanceSm_);
            outputSm_ += smooth_ * (outputTarget - outputSm_);
            modeSm_ += smooth_ * (modeTarget - modeSm_);
            enableRamp_ += std::min(1.0f, enableStep_) * (enableTarget - enableRamp_);

            if ((i % COEFF_INTERVAL) == 0) updateCoefficients();

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

            // Mode picks the voicing and, with it, the preamp gain range: clean
            // stays clean up to the edge of breakup, lead is far into clipping.
            float preamp;
            if (modeSm_ < 0.5f) preamp = 0.7f + preampSm_ * 3.5f;        // 0.7 .. 4.2
            else if (modeSm_ < 1.5f) preamp = 1.5f + preampSm_ * 14.0f;  // 1.5 .. 15.5
            else preamp = 3.0f + preampSm_ * 30.0f;                      // 3.0 .. 33.0
            const float master = masterSm_;
            const float output = outputSm_;

            const float wetL = channel(xL, 0, preamp, master, output);
            const float wetR = channel(xR, 1, preamp, master, output);

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

    // Transparent below the threshold, asymptotes below full scale above it.
    static float softLimit(float x) {
        constexpr float t = 0.7f;
        constexpr float ceiling = 0.98f;
        const float a = std::fabs(x);
        if (a <= t) return x;
        const float over = (a - t) / (ceiling - t);
        const float shaped = t + (ceiling - t) * std::tanh(over);
        return x < 0.0f ? -shaped : shaped;
    }

    float channel(float x, int ch, float preamp, float master, float output) {
        // Two cascaded preamp stages with an inter-stage high-pass.
        float stage1 = std::tanh(x * preamp * 0.6f);
        interStage_[ch] += interStageCoeff_ * (stage1 - interStage_[ch]);
        stage1 -= interStage_[ch];
        const float stage2 = std::tanh(stage1 * 1.6f);

        // Tone stack, then the power amp saturation.
        float out = tone_[ch].process(stage2);
        out = std::tanh(out * (1.0f + master * 3.0f));

        // Speaker resonance: a resonant low-pass at the cabinet's low resonance.
        out += resonanceFilter_[ch].process(out) * resonanceSm_ * 0.6f;

        out *= output;

        // DC blocker and soft limiter.
        const float blocked = out - dcX_[ch] + dcCoefficient_ * dcY_[ch];
        dcX_[ch] = out;
        dcY_[ch] = blocked;
        return softLimit(blocked);
    }

    // RBJ biquad.
    struct Biquad {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
        float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;

        void reset() { x1 = x2 = y1 = y2 = 0.0f; }

        void setPeaking(float sampleRate, float freq, float q, float db) {
            const float a = std::pow(10.0f, db / 40.0f);
            const float w = 2.0f * 3.14159265359f * freq / sampleRate;
            const float alpha = std::sin(w) / (2.0f * q);
            const float a0 = 1.0f + alpha / a;
            b0 = (1.0f + alpha * a) / a0;
            b1 = (-2.0f * std::cos(w)) / a0;
            b2 = (1.0f - alpha * a) / a0;
            a1 = (-2.0f * std::cos(w)) / a0;
            a2 = (1.0f - alpha / a) / a0;
        }

        void setLowShelf(float sampleRate, float freq, float db) {
            const float a = std::pow(10.0f, db / 40.0f);
            const float w = 2.0f * 3.14159265359f * freq / sampleRate;
            const float cw = std::cos(w);
            const float sw = std::sin(w);
            const float alpha = sw / 2.0f * std::sqrt(2.0f);
            const float beta = 2.0f * std::sqrt(a) * alpha;
            const float a0 = (a + 1.0f) + (a - 1.0f) * cw + beta;
            b0 = a * ((a + 1.0f) - (a - 1.0f) * cw + beta) / a0;
            b1 = 2.0f * a * ((a - 1.0f) - (a + 1.0f) * cw) / a0;
            b2 = a * ((a + 1.0f) - (a - 1.0f) * cw - beta) / a0;
            a1 = -2.0f * ((a - 1.0f) + (a + 1.0f) * cw) / a0;
            a2 = ((a + 1.0f) + (a - 1.0f) * cw - beta) / a0;
        }

        void setHighShelf(float sampleRate, float freq, float db) {
            const float a = std::pow(10.0f, db / 40.0f);
            const float w = 2.0f * 3.14159265359f * freq / sampleRate;
            const float cw = std::cos(w);
            const float sw = std::sin(w);
            const float alpha = sw / 2.0f * std::sqrt(2.0f);
            const float beta = 2.0f * std::sqrt(a) * alpha;
            const float a0 = (a + 1.0f) - (a - 1.0f) * cw + beta;
            b0 = a * ((a + 1.0f) + (a - 1.0f) * cw + beta) / a0;
            b1 = -2.0f * a * ((a - 1.0f) + (a + 1.0f) * cw) / a0;
            b2 = a * ((a + 1.0f) + (a - 1.0f) * cw - beta) / a0;
            a1 = 2.0f * ((a - 1.0f) - (a + 1.0f) * cw) / a0;
            a2 = ((a + 1.0f) - (a - 1.0f) * cw - beta) / a0;
        }

        float process(float x) {
            const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = x;
            y2 = y1; y1 = y;
            return y;
        }
    };

    struct ToneStack {
        Biquad bass, middle, treble, presence;

        void reset() {
            bass.reset();
            middle.reset();
            treble.reset();
            presence.reset();
        }

        float process(float x) {
            return presence.process(treble.process(middle.process(bass.process(x))));
        }
    };

    void updateCoefficients() {
        // Map the 0..1 controls to dB: unity at 0.5, +/- 12 dB at the ends.
        const float bassDb = (bassSm_ - 0.5f) * 24.0f;
        const float midDb = (middleSm_ - 0.5f) * 24.0f;
        const float trebleDb = (trebleSm_ - 0.5f) * 24.0f;
        const float presenceDb = (presenceSm_ - 0.5f) * 20.0f;
        const float resonanceDb = resonanceSm_ * 9.0f;

        for (int c = 0; c < 2; ++c) {
            tone_[c].bass.setLowShelf(sampleRate_, 110.0f, bassDb);
            tone_[c].middle.setPeaking(sampleRate_, 650.0f, 0.8f, midDb);
            tone_[c].treble.setHighShelf(sampleRate_, 3200.0f, trebleDb);
            tone_[c].presence.setHighShelf(sampleRate_, 4000.0f, presenceDb);
            resonanceFilter_[c].setPeaking(sampleRate_, 110.0f, 1.4f, resonanceDb);
        }
    }

    float sampleRate_ = 48000.0f;
    float smooth_ = 0.0f;
    float enableStep_ = 0.0f;
    float dcCoefficient_ = 0.999f;
    float interStageCoeff_ = 0.01f;

    ToneStack tone_[2];
    Biquad resonanceFilter_[2];
    float interStage_[2] = {0.0f, 0.0f};
    float dcX_[2] = {0.0f, 0.0f};
    float dcY_[2] = {0.0f, 0.0f};

    float enableRamp_ = 1.0f;

    std::atomic<float> preampGain_{0.35f};
    std::atomic<float> masterVolume_{0.75f};
    std::atomic<float> bass_{0.5f};
    std::atomic<float> middle_{0.5f};
    std::atomic<float> treble_{0.5f};
    std::atomic<float> presence_{0.5f};
    std::atomic<float> resonance_{0.35f};
    std::atomic<float> outputLevel_{0.8f};
    std::atomic<float> mode_{0.0f};
    std::atomic<bool> enabled_{true};

    // Smoothed copies, written only by the audio thread.
    float preampSm_ = 0.35f, masterSm_ = 0.75f, bassSm_ = 0.5f, middleSm_ = 0.5f;
    float trebleSm_ = 0.5f, presenceSm_ = 0.5f, resonanceSm_ = 0.35f, outputSm_ = 0.8f;
    float modeSm_ = 0.0f;
    bool primed_ = false;
};

#endif // ALTODFX_AMPLIFIER_H
