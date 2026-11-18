#ifndef ALTODFX_OCTAVER_H
#define ALTODFX_OCTAVER_H

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

// Octaver for the ALTODFX rack: generates a tone one octave below, two octaves
// below and one octave above the detected pitch of the input, mixed with the
// dry signal. The octaves are produced by phase-locked oscillators driven by an
// autocorrelation pitch estimate, not by resampling the input.
//
// Based on the reference "Octaver" DSP. The reference's note said it was a
// prototype whose autocorrelation needed fixing to avoid octave errors. Three
// things were corrected:
//
//   * the generator frequencies did not match their labels. The reference
//     advanced phase1_ by f/sr under the name "Sub Octave -1" and phase2_ by
//     0.5*f/sr under "Sub Octave -2", so the "one octave down" output was really
//     unison and the "two octaves down" output was really one octave down; there
//     was no two-octave-down tone at all. The increments are now f/2 (Sub -1),
//     f/4 (Sub -2) and 2f (Upper +1).
//   * the detector picked the global autocorrelation maximum, which lands on a
//     subharmonic (a longer lag that still correlates) about as often as the
//     true period, dropping the estimate by an octave. It now takes the first
//     local peak that reaches 90% of the global maximum, which is the true
//     period, and interpolates the peak position to sub-sample accuracy.
//   * the live parameters were plain members written by the control thread and
//     read by the audio thread, so setParameters() racing process() was
//     undefined behaviour. Every published parameter is now a std::atomic,
//     loaded once at the top of process().
//
// The reference also smoothed the generated squares with a one-pole, which keeps
// the sub octaves from sounding like a raw buzzer; that is kept. The detector
// runs on a 2x decimated copy of the mono sum so its cost stays bounded, and the
// class holds no allocation.
//
// Latency: the detector needs a couple of periods of history, so a note is
// tracked within roughly 20 ms.
class Octaver {
public:
    // Detector decimation factor. Pitch detection only needs the fundamental
    // band, so running it at half the sample rate quarters the cost.
    static constexpr int DECIM = 2;
    static constexpr int MAX_HISTORY = 2048;
    // Re-estimate every this many decimated samples.
    static constexpr int DETECT_INTERVAL = 256;

    explicit Octaver(float sampleRate = 48000.0f) {
        setSampleRate(sampleRate);
        reset();
    }

    void setSampleRate(float sampleRate) {
        sampleRate_ = std::max(22050.0f, sampleRate);
        const float decimatedRate = sampleRate_ / (float) DECIM;

        // Track roughly 55 Hz to 1200 Hz.
        minLag_ = std::max(4, (int) (decimatedRate / 1200.0f));
        maxLag_ = std::min(MAX_HISTORY / 2, (int) (decimatedRate / 55.0f));

        // ~15 ms glides for the control parameters.
        smooth_ = 1.0f - std::exp(-1.0f / (0.015f * sampleRate_));
        // Decimation pre-filter, a one-pole a little below the new Nyquist.
        decAlpha_ = 1.0f - std::exp(-6.2831853f * 8000.0f / sampleRate_);
        // ~10 ms ramp for enable/disable.
        enableStep_ = 1.0f / (0.010f * sampleRate_);
        // Input envelope follower: fast attack, slow release.
        envAttack_ = 1.0f - std::exp(-1.0f / (0.002f * sampleRate_));
        envRelease_ = 1.0f - std::exp(-1.0f / (0.080f * sampleRate_));
    }

    void setParameters(float subOctave1, float subOctave2, float upperOctave,
                       float dryLevel, float sensitivity, float outputGain) {
        sub1_.store(clamp(subOctave1, 0.0f, 1.0f), std::memory_order_relaxed);
        sub2_.store(clamp(subOctave2, 0.0f, 1.0f), std::memory_order_relaxed);
        upper_.store(clamp(upperOctave, 0.0f, 1.0f), std::memory_order_relaxed);
        dry_.store(clamp(dryLevel, 0.0f, 1.0f), std::memory_order_relaxed);
        sensitivity_.store(clamp(sensitivity, 0.0f, 1.0f), std::memory_order_relaxed);
        gain_.store(clamp(outputGain, 0.0f, 2.0f), std::memory_order_relaxed);
    }

    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }

    void reset() {
        std::memset(history_, 0, sizeof(history_));
        writeIndex_ = 0;
        decCounter_ = 0;
        detectCounter_ = 0;
        decState_ = 0.0f;
        phase1_ = 0.0f;
        phase2_ = 0.0f;
        phaseUpper_ = 0.0f;
        estimatedFrequency_ = 220.0f;
        smoothedFrequency_ = 220.0f;
        confidence_ = 0.0f;
        sub1StateL_ = sub1StateR_ = 0.0f;
        sub2StateL_ = sub2StateR_ = 0.0f;
        upperStateL_ = upperStateR_ = 0.0f;
        enableRamp_ = 1.0f;
        env_ = 0.0f;
        primed_ = false;
    }

    // Stereo, separate per-channel buffers. Pitch is detected on the mono sum,
    // so a note panned anywhere is still tracked.
    void process(float *left, float *right, int32_t numFrames) {
        if (left == nullptr || right == nullptr || numFrames <= 0) return;

        const float sub1Target = sub1_.load(std::memory_order_relaxed);
        const float sub2Target = sub2_.load(std::memory_order_relaxed);
        const float upperTarget = upper_.load(std::memory_order_relaxed);
        const float dryTarget = dry_.load(std::memory_order_relaxed);
        const float sensTarget = sensitivity_.load(std::memory_order_relaxed);
        const float gainTarget = gain_.load(std::memory_order_relaxed);
        const float enableTarget = enabled_.load(std::memory_order_relaxed) ? 1.0f : 0.0f;

        if (!primed_) {
            sub1Sm_ = sub1Target; sub2Sm_ = sub2Target; upperSm_ = upperTarget;
            drySm_ = dryTarget; sensSm_ = sensTarget; gainSm_ = gainTarget;
            enableRamp_ = enableTarget;
            primed_ = true;
        }

        const float decimatedRate = sampleRate_ / (float) DECIM;

        for (int32_t i = 0; i < numFrames; ++i) {
            sub1Sm_ += smooth_ * (sub1Target - sub1Sm_);
            sub2Sm_ += smooth_ * (sub2Target - sub2Sm_);
            upperSm_ += smooth_ * (upperTarget - upperSm_);
            drySm_ += smooth_ * (dryTarget - drySm_);
            sensSm_ += smooth_ * (sensTarget - sensSm_);
            gainSm_ += smooth_ * (gainTarget - gainSm_);
            enableRamp_ += std::min(1.0f, enableStep_) * (enableTarget - enableRamp_);

            float inL = left[i];
            float inR = right[i];
            if (!isFinite(inL)) inL = 0.0f;
            if (!isFinite(inR)) inR = 0.0f;

            const float mono = 0.5f * (inL + inR);

            // Track the input level so the oscillators are silenced when there
            // is nothing to track; otherwise they would drone on forever.
            const float rectified = std::fabs(mono);
            const float envCoeff = rectified > env_ ? envAttack_ : envRelease_;
            env_ += envCoeff * (rectified - env_);

            // Decimate the mono sum for the detector.
            decState_ += decAlpha_ * (mono - decState_);
            if (++decCounter_ >= DECIM) {
                decCounter_ = 0;
                history_[writeIndex_] = decState_;
                writeIndex_ = (writeIndex_ + 1) % MAX_HISTORY;
                if (++detectCounter_ >= DETECT_INTERVAL) {
                    detectCounter_ = 0;
                    detectPitch(decimatedRate, sensSm_);
                }
            }

            // Glide the oscillator frequency toward the estimate so a new note
            // does not jump.
            smoothedFrequency_ += 0.02f * (estimatedFrequency_ - smoothedFrequency_);

            // Phase-continuous oscillators: the phase is never reset on a pitch
            // change, so retuning does not click.
            const float fundamental = smoothedFrequency_ / sampleRate_;
            phase1_ += fundamental * 0.5f;        // one octave down
            phase2_ += fundamental * 0.25f;       // two octaves down
            phaseUpper_ += fundamental * 2.0f;    // one octave up
            phase1_ -= std::floor(phase1_);
            phase2_ -= std::floor(phase2_);
            phaseUpper_ -= std::floor(phaseUpper_);

            // A small phase offset on the right channel keeps the sub octaves
            // from collapsing to a mono point source.
            const float sub1L = square(phase1_);
            const float sub1R = square(phase1_ + 0.02f);
            const float sub2L = square(phase2_);
            const float sub2R = square(phase2_ + 0.02f);
            const float upperL = square(phaseUpper_);
            const float upperR = square(phaseUpper_ + 0.02f);

            // Smooth the squares so they sound like a tone rather than a buzzer.
            sub1StateL_ += 0.12f * (sub1L - sub1StateL_);
            sub1StateR_ += 0.12f * (sub1R - sub1StateR_);
            sub2StateL_ += 0.12f * (sub2L - sub2StateL_);
            sub2StateR_ += 0.12f * (sub2R - sub2StateR_);
            upperStateL_ += 0.12f * (upperL - upperStateL_);
            upperStateR_ += 0.12f * (upperR - upperStateR_);

            // Gate: no input energy, no generated octaves. The threshold sits
            // just above the noise floor and the knee avoids a hard switch.
            const float gate = std::min(1.0f, env_ / 0.004f);

            const float wetL = (sub1StateL_ * sub1Sm_ + sub2StateL_ * sub2Sm_ + upperStateL_ * upperSm_) * gate;
            const float wetR = (sub1StateR_ * sub1Sm_ + sub2StateR_ * sub2Sm_ + upperStateR_ * upperSm_) * gate;

            const float octaveL = softLimit((inL * drySm_ + wetL) * gainSm_);
            const float octaveR = softLimit((inR * drySm_ + wetR) * gainSm_);

            // Blend the dry signal in as the ramp falls, so a bypass is click
            // free and never leaves the sub oscillators audible.
            const float outL = inL + (octaveL - inL) * enableRamp_;
            const float outR = inR + (octaveR - inR) * enableRamp_;

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

    static float square(float phase) {
        return phase < 0.5f ? 1.0f : -1.0f;
    }

    static float softLimit(float x) {
        return x / (1.0f + std::fabs(x));
    }

    // Normalized autocorrelation over the decimated history. The most recent
    // sample sits just behind writeIndex_. Results are cached in correlation_ so
    // the peak search below does not recompute them.
    void detectPitch(float decimatedRate, float sensitivity) {
        // Two periods of history are enough for a stable normalization.
        const int window = std::min(MAX_HISTORY - maxLag_ - 2, 2 * maxLag_);
        if (window <= minLag_) return;

        float best = 0.0f;
        int bestLag = 0;
        for (int lag = minLag_ - 1; lag <= maxLag_ + 1; ++lag) {
            double correlation = 0.0, energyA = 0.0, energyB = 0.0;
            for (int n = 0; n < window; ++n) {
                const int aIndex = (int) ((writeIndex_ + MAX_HISTORY - 1 - n) % MAX_HISTORY);
                const int bIndex = (int) ((aIndex + MAX_HISTORY - lag) % MAX_HISTORY);
                const float a = history_[aIndex];
                const float b = history_[bIndex];
                correlation += (double) a * b;
                energyA += (double) a * a;
                energyB += (double) b * b;
            }
            correlation_[lag] = (float) (correlation / std::sqrt(energyA * energyB + 1.0e-12));
            if (lag >= minLag_ && lag <= maxLag_ && correlation_[lag] > best) {
                best = correlation_[lag];
                bestLag = lag;
            }
        }

        if (bestLag == 0 || best < 0.35f) return;

        // The global maximum can sit on a subharmonic (a longer lag that still
        // correlates), which would drop the estimate by an octave. Take the first
        // local peak from the short end that reaches 90% of the global maximum;
        // that is the true period.
        const float accept = 0.90f * best;
        int chosenLag = bestLag;
        for (int lag = minLag_ + 1; lag < maxLag_; ++lag) {
            if (correlation_[lag] >= accept &&
                correlation_[lag] >= correlation_[lag - 1] &&
                correlation_[lag] >= correlation_[lag + 1]) {
                chosenLag = lag;
                break;
            }
        }

        // Parabolic interpolation around the chosen peak for a sub-sample period.
        const float previous = correlation_[chosenLag - 1];
        const float current = correlation_[chosenLag];
        const float next = correlation_[chosenLag + 1];
        const float denominator = previous - 2.0f * current + next;
        float refined = (float) chosenLag;
        if (std::fabs(denominator) > 1.0e-9f)
            refined += 0.5f * (previous - next) / denominator;

        const float confidence = clamp(current, 0.0f, 1.0f);
        if (confidence > 0.35f && refined > 0.0f) {
            const float detected = decimatedRate / refined;
            const float alpha = 0.08f + 0.30f * sensitivity * confidence;
            estimatedFrequency_ += alpha * (detected - estimatedFrequency_);
            confidence_ = confidence;
        }
    }

    float sampleRate_ = 48000.0f;
    float smooth_ = 0.0f;
    float decAlpha_ = 0.0f;
    float enableStep_ = 0.0f;
    float envAttack_ = 0.0f;
    float envRelease_ = 0.0f;
    float env_ = 0.0f;

    float history_[MAX_HISTORY];
    // Autocorrelation cache for lags minLag_-1 .. maxLag_+1.
    float correlation_[MAX_HISTORY / 2 + 2];
    size_t writeIndex_ = 0;
    int decCounter_ = 0;
    int detectCounter_ = 0;
    float decState_ = 0.0f;

    int minLag_ = 20;
    int maxLag_ = 436;

    float phase1_ = 0.0f;
    float phase2_ = 0.0f;
    float phaseUpper_ = 0.0f;
    float estimatedFrequency_ = 220.0f;
    float smoothedFrequency_ = 220.0f;
    float confidence_ = 0.0f;
    bool primed_ = false;

    float sub1StateL_ = 0.0f, sub1StateR_ = 0.0f;
    float sub2StateL_ = 0.0f, sub2StateR_ = 0.0f;
    float upperStateL_ = 0.0f, upperStateR_ = 0.0f;

    std::atomic<float> sub1_{0.65f};
    std::atomic<float> sub2_{0.0f};
    std::atomic<float> upper_{0.0f};
    std::atomic<float> dry_{0.80f};
    std::atomic<float> sensitivity_{0.35f};
    std::atomic<float> gain_{1.0f};
    std::atomic<bool> enabled_{true};

    // Smoothed copies, written only by the audio thread.
    float sub1Sm_ = 0.65f, sub2Sm_ = 0.0f, upperSm_ = 0.0f;
    float drySm_ = 0.80f, sensSm_ = 0.35f, gainSm_ = 1.0f;
    float enableRamp_ = 1.0f;
};

#endif // ALTODFX_OCTAVER_H
