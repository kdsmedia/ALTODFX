#ifndef ALTODFX_REVERB_H
#define ALTODFX_REVERB_H

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// Schroeder/Moorer hybrid reverb for the ALTODFX rack: eight damped comb filters
// and four all-pass diffusers per channel, fed through a pre-delay line, with
// room size, decay, damping, diffusion, mix, width and output level.
//
// Based on the reference "Reverb" DSP. The reference's note named four things to
// settle before production, all addressed here:
//
//   * the pre-delay path. The reference read the line through a
//     const_cast on a const reference and wrote the input inside the read, which
//     made the zero-delay case read the sample it had just written. The path is
//     now an explicit line: write the input, then read at a fractional distance
//     behind the write head, with no cast.
//   * the comb feedback design, which the note said should be based on RT60. The
//     reference used a single fixed mapping, feedback = 0.20 + decay*0.75 +
//     room*0.04 clamped at 0.95, the same for every comb. That is not tied to a
//     decay time, and because it ignores the comb length a long comb rings for a
//     different time than a short one. Each comb now sets its feedback from its
//     own delay length and the requested RT60: g = 10^(-3*D/RT60), which is the
//     feedback that makes that comb decay by 60 dB in RT60. Room Size scales the
//     comb lengths, so a larger room lengthens the delays and, for the same RT60,
//     raises g exactly as much as needed. g stays below 1 for every setting.
//   * parameter synchronisation between the UI and the audio thread. The
//     parameters lived in a plain struct written by the control thread and read
//     by the audio thread, so setParameters() racing process() was undefined
//     behaviour. Every published parameter is now a std::atomic, loaded once at
//     the top of process().
//   * stability and frequency-response testing. The comb feedback is bounded
//     below 1 by construction and was checked across the room-size and RT60
//     range; the damping control was checked to darken the tail; the RT60 was
//     measured against the requested value.
//
// Also: bypass ramps the wet path instead of dropping it, and the lines keep
// running so the tail survives a bypass. The NaN/Inf guards inspect the bit
// pattern so they survive -ffast-math. The buffers are allocated in prepare(),
// never in process().
class Reverb {
public:
    static constexpr int COMB_COUNT = 8;
    static constexpr int ALLPASS_COUNT = 4;
    static constexpr float MAX_PREDELAY_SECONDS = 0.25f;

    explicit Reverb(float sampleRate = 48000.0f) {
        prepare(sampleRate);
    }

    bool prepare(float sampleRate) {
        if (sampleRate < 8000.0f || sampleRate > 384000.0f) return false;

        sampleRate_ = sampleRate;

        // Comb and all-pass lengths in samples at 44.1 kHz; scaled below.
        static const int combBaseL[COMB_COUNT] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        static const int combBaseR[COMB_COUNT] = {1139, 1211, 1300, 1379, 1445, 1514, 1580, 1640};
        static const int allpassBaseL[ALLPASS_COUNT] = {556, 441, 341, 225};
        static const int allpassBaseR[ALLPASS_COUNT] = {579, 464, 364, 248};

        const float scale = sampleRate_ / 44100.0f;
        for (int i = 0; i < COMB_COUNT; ++i) {
            combL_[i].prepare((int) (combBaseL[i] * scale), sampleRate_);
            combR_[i].prepare((int) (combBaseR[i] * scale), sampleRate_);
        }
        for (int i = 0; i < ALLPASS_COUNT; ++i) {
            allpassL_[i].prepare((int) (allpassBaseL[i] * scale));
            allpassR_[i].prepare((int) (allpassBaseR[i] * scale));
        }

        preDelaySize_ = (size_t) (sampleRate_ * MAX_PREDELAY_SECONDS) + 8;
        preDelayL_.assign(preDelaySize_, 0.0f);
        preDelayR_.assign(preDelaySize_, 0.0f);

        smooth_ = 1.0f - std::exp(-1.0f / (0.015f * sampleRate_));
        enableStep_ = 1.0f / (0.010f * sampleRate_);

        reset();
        return true;
    }

    void setParameters(float roomSize, float decayRt60, float damping, float preDelayMs,
                       float diffusion, float mix, float width, float outputGain) {
        roomSize_.store(clamp(roomSize, 0.0f, 1.0f), std::memory_order_relaxed);
        decay_.store(clamp(decayRt60, 0.2f, 8.0f), std::memory_order_relaxed);
        damping_.store(clamp(damping, 0.0f, 1.0f), std::memory_order_relaxed);
        preDelay_.store(clamp(preDelayMs, 0.0f, 200.0f), std::memory_order_relaxed);
        diffusion_.store(clamp(diffusion, 0.0f, 1.0f), std::memory_order_relaxed);
        mix_.store(clamp(mix, 0.0f, 1.0f), std::memory_order_relaxed);
        width_.store(clamp(width, 0.0f, 1.0f), std::memory_order_relaxed);
        gain_.store(clamp(outputGain, 0.0f, 2.0f), std::memory_order_relaxed);
    }

    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }

    void reset() {
        for (auto &c : combL_) c.reset();
        for (auto &c : combR_) c.reset();
        for (auto &a : allpassL_) a.reset();
        for (auto &a : allpassR_) a.reset();

        std::fill(preDelayL_.begin(), preDelayL_.end(), 0.0f);
        std::fill(preDelayR_.begin(), preDelayR_.end(), 0.0f);
        preDelayIndex_ = 0;
        enableRamp_ = 1.0f;
        primed_ = false;
    }

    // Stereo, separate per-channel buffers.
    void process(float *left, float *right, int32_t numFrames) {
        if (left == nullptr || right == nullptr || numFrames <= 0) return;

        const float roomTarget = roomSize_.load(std::memory_order_relaxed);
        const float decayTarget = decay_.load(std::memory_order_relaxed);
        const float dampingTarget = damping_.load(std::memory_order_relaxed);
        const float preDelayTarget = preDelay_.load(std::memory_order_relaxed);
        const float diffusionTarget = diffusion_.load(std::memory_order_relaxed);
        const float mixTarget = mix_.load(std::memory_order_relaxed);
        const float widthTarget = width_.load(std::memory_order_relaxed);
        const float gainTarget = gain_.load(std::memory_order_relaxed);
        const float enableTarget = enabled_.load(std::memory_order_relaxed) ? 1.0f : 0.0f;

        if (!primed_) {
            roomSm_ = roomTarget;
            decaySm_ = decayTarget;
            dampingSm_ = dampingTarget;
            preDelaySm_ = preDelayTarget;
            diffusionSm_ = diffusionTarget;
            mixSm_ = mixTarget;
            widthSm_ = widthTarget;
            gainSm_ = gainTarget;
            enableRamp_ = enableTarget;
            primed_ = true;
        }

        for (int32_t i = 0; i < numFrames; ++i) {
            roomSm_ += smooth_ * (roomTarget - roomSm_);
            decaySm_ += smooth_ * (decayTarget - decaySm_);
            dampingSm_ += smooth_ * (dampingTarget - dampingSm_);
            preDelaySm_ += smooth_ * (preDelayTarget - preDelaySm_);
            diffusionSm_ += smooth_ * (diffusionTarget - diffusionSm_);
            mixSm_ += smooth_ * (mixTarget - mixSm_);
            widthSm_ += smooth_ * (widthTarget - widthSm_);
            gainSm_ += smooth_ * (gainTarget - gainSm_);
            enableRamp_ += std::min(1.0f, enableStep_) * (enableTarget - enableRamp_);

            float inL = left[i];
            float inR = right[i];
            if (!isFinite(inL)) inL = 0.0f;
            if (!isFinite(inR)) inR = 0.0f;

            // Pre-delay: write first, then read behind the write head.
            const float preDelaySamples = preDelaySm_ * 0.001f * sampleRate_;
            preDelayL_[preDelayIndex_] = inL;
            preDelayR_[preDelayIndex_] = inR;
            const float delayedL = readPreDelay(preDelayL_, preDelaySamples);
            const float delayedR = readPreDelay(preDelayR_, preDelaySamples);

            // Room Size scales the comb lengths. The damping coefficient and the
            // per-comb feedback both follow from that and the requested RT60.
            const float roomScale = 0.4f + roomSm_ * 1.2f;
            const float damping = dampingSm_ * 0.9f;
            const float rt60 = std::max(0.2f, decaySm_);

            const float excitationL = delayedL * 0.25f;
            const float excitationR = delayedR * 0.25f;

            float wetL = 0.0f, wetR = 0.0f;
            for (int c = 0; c < COMB_COUNT; ++c) {
                wetL += combL_[c].process(excitationL, rt60, damping, roomScale);
                wetR += combR_[c].process(excitationR, rt60, damping, roomScale);
            }
            wetL *= 0.125f;
            wetR *= 0.125f;

            for (int a = 0; a < ALLPASS_COUNT; ++a) {
                wetL = allpassL_[a].process(wetL, diffusionSm_);
                wetR = allpassR_[a].process(wetR, diffusionSm_);
            }

            const float mid = 0.5f * (wetL + wetR);
            const float side = 0.5f * (wetL - wetR);
            wetL = mid + side * widthSm_;
            wetR = mid - side * widthSm_;

            const float dryGain = std::cos(mixSm_ * 1.57079632679f);
            const float wetGain = std::sin(mixSm_ * 1.57079632679f);

            const float mixedL = (inL * dryGain + wetL * wetGain) * gainSm_;
            const float mixedR = (inR * dryGain + wetR * wetGain) * gainSm_;

            // Blend dry in as the ramp falls, so a bypass is click free.
            const float outL = inL + (mixedL - inL) * enableRamp_;
            const float outR = inR + (mixedR - inR) * enableRamp_;

            left[i] = isFinite(outL) ? outL : inL;
            right[i] = isFinite(outR) ? outR : inR;

            ++preDelayIndex_;
            if (preDelayIndex_ >= preDelaySize_) preDelayIndex_ = 0;
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

    // Linear-interpolated read behind the write head.
    float readPreDelay(const std::vector<float> &buffer, float delaySamples) const {
        const int size = (int) buffer.size();
        float position = (float) preDelayIndex_ - delaySamples;
        while (position < 0.0f) position += (float) size;
        while (position >= (float) size) position -= (float) size;

        const int i1 = (int) position;
        const float t = position - (float) i1;
        const int i2 = (i1 + 1) % size;
        return buffer[i1] * (1.0f - t) + buffer[i2] * t;
    }

    // Damped comb with a fractional delay. The feedback is derived from the
    // comb's own delay length and the requested RT60, so every comb decays by
    // 60 dB in the same time.
    class CombFilter {
    public:
        static constexpr float MAX_ROOM_SCALE = 1.6f;

        void prepare(int size, float sampleRate) {
            baseLength_ = (float) std::max(1, size);
            buffer_.assign((size_t) (baseLength_ * MAX_ROOM_SCALE) + 4, 0.0f);
            sampleRate_ = sampleRate;
            index_ = 0;
            filterState_ = 0.0f;
        }

        void reset() {
            std::fill(buffer_.begin(), buffer_.end(), 0.0f);
            index_ = 0;
            filterState_ = 0.0f;
        }

        float process(float input, float rt60, float damping, float roomScale) {
            // Room Size stretches the delay length, which both lengthens the tail
            // and, through the feedback below, deepens it for a given RT60.
            const float delaySamples = std::min(baseLength_ * roomScale,
                                                (float) buffer_.size() - 2.0f);
            const float output = read(delaySamples);
            const float delaySeconds = delaySamples / sampleRate_;

            // g such that g^(rt60/delaySeconds) = 10^-3.
            const float feedback = std::min(0.9995f,
                std::pow(10.0f, -3.0f * delaySeconds / rt60));

            filterState_ = output * (1.0f - damping) + filterState_ * damping;
            buffer_[index_] = input + filterState_ * feedback;

            ++index_;
            if (index_ >= buffer_.size()) index_ = 0;
            return output;
        }

    private:
        // Linear-interpolated read at a fractional distance behind the write head.
        float read(float delaySamples) const {
            const int size = (int) buffer_.size();
            float position = (float) index_ - delaySamples;
            while (position < 0.0f) position += (float) size;
            while (position >= (float) size) position -= (float) size;

            const int i1 = (int) position;
            const float t = position - (float) i1;
            const int i2 = (i1 + 1) % size;
            return buffer_[i1] * (1.0f - t) + buffer_[i2] * t;
        }

        std::vector<float> buffer_;
        float baseLength_ = 1.0f;
        float sampleRate_ = 48000.0f;
        size_t index_ = 0;
        float filterState_ = 0.0f;
    };

    // Schroeder all-pass diffuser.
    class AllpassFilter {
    public:
        void prepare(int size) {
            buffer_.assign(std::max(1, size), 0.0f);
            index_ = 0;
        }

        void reset() {
            std::fill(buffer_.begin(), buffer_.end(), 0.0f);
            index_ = 0;
        }

        float process(float input, float diffusion) {
            const float delayed = buffer_[index_];
            const float g = 0.5f * (0.5f + diffusion * 0.45f);
            const float output = delayed - g * input;
            buffer_[index_] = input + g * output;

            ++index_;
            if (index_ >= buffer_.size()) index_ = 0;
            return output;
        }

    private:
        std::vector<float> buffer_;
        size_t index_ = 0;
    };

    float sampleRate_ = 48000.0f;
    float smooth_ = 0.0f;
    float enableStep_ = 0.0f;

    std::vector<float> preDelayL_;
    std::vector<float> preDelayR_;
    size_t preDelaySize_ = 0;
    size_t preDelayIndex_ = 0;

    CombFilter combL_[COMB_COUNT];
    CombFilter combR_[COMB_COUNT];
    AllpassFilter allpassL_[ALLPASS_COUNT];
    AllpassFilter allpassR_[ALLPASS_COUNT];

    std::atomic<float> roomSize_{0.75f};
    std::atomic<float> decay_{2.5f};
    std::atomic<float> damping_{0.45f};
    std::atomic<float> preDelay_{20.0f};
    std::atomic<float> diffusion_{0.75f};
    std::atomic<float> mix_{0.30f};
    std::atomic<float> width_{1.0f};
    std::atomic<float> gain_{1.0f};
    std::atomic<bool> enabled_{true};

    // Smoothed copies, written only by the audio thread.
    float roomSm_ = 0.75f, decaySm_ = 2.5f, dampingSm_ = 0.45f;
    float preDelaySm_ = 20.0f, diffusionSm_ = 0.75f;
    float mixSm_ = 0.30f, widthSm_ = 1.0f, gainSm_ = 1.0f;
    float enableRamp_ = 1.0f;
    bool primed_ = false;
};

#endif // ALTODFX_REVERB_H
