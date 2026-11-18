#ifndef ALTODFX_EQUALIZER_H
#define ALTODFX_EQUALIZER_H

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

// Ten-band graphic equalizer for the ALTODFX rack, with the peaking, low-shelf
// and high-shelf building blocks of a parametric EQ underneath.
//
// Based on the reference "Equalizer" DSP, hardened for this host:
//   * the reference recomputed every biquad's coefficients once per audio block
//     and swapped them in immediately, so moving a band gain stepped the
//     transfer function and clicked. Here the coefficients are only recomputed
//     when a parameter actually changed, and the working coefficients are
//     interpolated toward the target one sample at a time, so a gain change
//     glides instead of jumping (the note's "interpolasi koefisien").
//   * the reference ran the filter update on every block even when nothing had
//     changed, which wasted the audio thread's budget; a dirty flag now gates it.
//   * the reference recomputed coefficients on the control thread, racing the
//     audio thread. All filter maths now happens on the audio thread; the control
//     thread only publishes parameter values and raises the dirty flag.
//   * the reference's `customMode_` (graphic vs parametric) had no UI to switch
//     it, so the parametric path was unreachable. The DSP keeps both: the rack
//     exposes the ten graphic bands, and setBand()/setShelfBand() remain for
//     parametric use.
//   * the output is soft-limited, so a stack of overlapping boosts cannot clip
//     the stages after the EQ.
//   * the NaN/Inf guards inspect the bit pattern so they survive -ffast-math.
//
// The class is stereo with independent per-channel state and never allocates, so
// process() is safe from the audio thread.
class Equalizer {
public:
    static constexpr int NUM_BANDS = 10;

    // ISO octave centres of a standard 10-band graphic equalizer.
    static constexpr float GRAPHIC_FREQUENCIES[NUM_BANDS] = {
        31.25f, 62.5f, 125.0f, 250.0f, 500.0f,
        1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f
    };

    // Q of the octave-spaced graphic bands: about 1.41 keeps neighbouring bands
    // overlapping smoothly instead of leaving dips between them.
    static constexpr float GRAPHIC_Q = 1.4142f;

    explicit Equalizer(float sampleRate = 48000.0f) {
        setSampleRate(sampleRate);
        reset();
    }

    void setSampleRate(float sampleRate) {
        sampleRate_ = std::max(8000.0f, sampleRate);
        // ~15 ms glide, so a knob turn is heard as a sweep, not a click.
        coeffSmooth_ = 1.0f - std::exp(-1.0f / (0.015f * sampleRate_));
        for (int band = 0; band < NUM_BANDS; ++band) {
            frequency_[band].store(GRAPHIC_FREQUENCIES[band], std::memory_order_relaxed);
            q_[band].store(GRAPHIC_Q, std::memory_order_relaxed);
        }
        dirty_.store(true, std::memory_order_relaxed);
    }

    // Graphic EQ: one gain in dB per band, clamped to -12 .. +12 dB.
    void setGraphicEQ(float b1, float b2, float b3, float b4, float b5,
                      float b6, float b7, float b8, float b9, float b10) {
        const float gains[NUM_BANDS] = {b1, b2, b3, b4, b5, b6, b7, b8, b9, b10};
        for (int band = 0; band < NUM_BANDS; ++band) {
            graphicGain_[band].store(clampDb(gains[band]), std::memory_order_relaxed);
            shape_[band].store(kPeaking, std::memory_order_relaxed);
        }
        dirty_.store(true, std::memory_order_relaxed);
    }

    // Parametric band: frequency in Hz, gain in dB, Q. Setting a band also
    // switches it to a peaking shape.
    void setBand(int band, float frequency, float gainDb, float q) {
        if (band < 0 || band >= NUM_BANDS) return;
        frequency_[band].store(std::max(20.0f, frequency), std::memory_order_relaxed);
        graphicGain_[band].store(clampDb(gainDb), std::memory_order_relaxed);
        q_[band].store(std::max(0.1f, std::min(q, 10.0f)), std::memory_order_relaxed);
        shape_[band].store(kPeaking, std::memory_order_relaxed);
        dirty_.store(true, std::memory_order_relaxed);
    }

    // Parametric band using a shelf: a low shelf cuts/boosts below `frequency`,
    // a high shelf cuts/boosts above it.
    void setShelfBand(int band, float frequency, float gainDb, bool highShelf) {
        if (band < 0 || band >= NUM_BANDS) return;
        frequency_[band].store(std::max(20.0f, frequency), std::memory_order_relaxed);
        graphicGain_[band].store(clampDb(gainDb), std::memory_order_relaxed);
        shape_[band].store(highShelf ? kHighShelf : kLowShelf, std::memory_order_relaxed);
        dirty_.store(true, std::memory_order_relaxed);
    }

    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }

    void reset() {
        for (int ch = 0; ch < 2; ++ch)
            for (int band = 0; band < NUM_BANDS; ++band)
                filters_[ch][band].reset();
    }

    // Stereo, separate per-channel buffers.
    void process(float *left, float *right, int32_t numFrames) {
        if (left == nullptr || right == nullptr || numFrames <= 0) return;
        if (!enabled_.load(std::memory_order_relaxed)) return;

        if (dirty_.load(std::memory_order_relaxed)) {
            updateTargets();
            dirty_.store(false, std::memory_order_relaxed);
        }

        for (int32_t i = 0; i < numFrames; ++i) {
            left[i] = softLimit(runChannel(filters_[0], left[i]));
            right[i] = softLimit(runChannel(filters_[1], right[i]));
        }
    }

private:
    static constexpr float kPi = 3.14159265358979323846f;
    enum Shape { kPeaking, kLowShelf, kHighShelf };

    struct Biquad {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;     // working
        float tb0 = 1.0f, tb1 = 0.0f, tb2 = 0.0f, ta1 = 0.0f, ta2 = 0.0f;  // target
        float z1 = 0.0f, z2 = 0.0f;

        void reset() { z1 = 0.0f; z2 = 0.0f; }

        // Snap the working coefficients onto the target, so the first block does
        // not sweep up from unity.
        void settle() { b0 = tb0; b1 = tb1; b2 = tb2; a1 = ta1; a2 = ta2; }

        // Move the working coefficients one step toward the target. Coefficients
        // (not parameters) are interpolated: recomputing cos/sin/pow per sample
        // would be far too expensive.
        inline void glide(float k) {
            b0 += k * (tb0 - b0);
            b1 += k * (tb1 - b1);
            b2 += k * (tb2 - b2);
            a1 += k * (ta1 - a1);
            a2 += k * (ta2 - a2);
        }

        inline float process(float x) {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    static bool isFinite(float value) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        return (bits & 0x7F800000u) != 0x7F800000u;
    }

    static float clampDb(float value) {
        if (!isFinite(value)) return 0.0f;
        return std::max(-12.0f, std::min(12.0f, value));
    }

    // Transparent below the knee; a stack of overlapping boosts can still push
    // the level up, and this keeps it inside range without flattening the EQ.
    static float softLimit(float x) {
        const float knee = 0.95f;
        const float a = std::fabs(x);
        if (a <= knee) return x;
        const float range = 1.0f - knee;
        const float over = a - knee;
        const float compressed = knee + range * (over / (over + range));
        return x < 0.0f ? -compressed : compressed;
    }

    // RBJ peaking filter, written into a Biquad's target slots.
    static void setPeaking(Biquad &f, float sr, float freq, float gainDb, float q) {
        const float x = std::max(20.0f, std::min(freq, sr * 0.45f));
        const float A = std::pow(10.0f, gainDb / 40.0f);
        const float w = 2.0f * kPi * x / sr;
        const float c = std::cos(w);
        const float s = std::sin(w);
        const float alpha = s / (2.0f * std::max(0.1f, std::min(q, 10.0f)));
        const float a0 = 1.0f + alpha / A;
        f.tb0 = (1.0f + alpha * A) / a0;
        f.tb1 = (-2.0f * c) / a0;
        f.tb2 = (1.0f - alpha * A) / a0;
        f.ta1 = (-2.0f * c) / a0;
        f.ta2 = (1.0f - alpha / A) / a0;
    }

    static void setLowShelf(Biquad &f, float sr, float freq, float gainDb) {
        const float x = std::max(20.0f, std::min(freq, sr * 0.45f));
        const float A = std::pow(10.0f, gainDb / 40.0f);
        const float w = 2.0f * kPi * x / sr;
        const float c = std::cos(w);
        const float s = std::sin(w);
        const float beta = std::sqrt(A) * s * 0.5f * 1.41421356f;  // S = 1
        const float a0 = (A + 1.0f) + (A - 1.0f) * c + 2.0f * beta;
        f.tb0 = A * ((A + 1.0f) - (A - 1.0f) * c + 2.0f * beta) / a0;
        f.tb1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * c) / a0;
        f.tb2 = A * ((A + 1.0f) - (A - 1.0f) * c - 2.0f * beta) / a0;
        f.ta1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * c) / a0;
        f.ta2 = ((A + 1.0f) + (A - 1.0f) * c - 2.0f * beta) / a0;
    }

    static void setHighShelf(Biquad &f, float sr, float freq, float gainDb) {
        const float x = std::max(20.0f, std::min(freq, sr * 0.45f));
        const float A = std::pow(10.0f, gainDb / 40.0f);
        const float w = 2.0f * kPi * x / sr;
        const float c = std::cos(w);
        const float s = std::sin(w);
        const float beta = std::sqrt(A) * s * 0.5f * 1.41421356f;  // S = 1
        const float a0 = (A + 1.0f) - (A - 1.0f) * c + 2.0f * beta;
        f.tb0 = A * ((A + 1.0f) + (A - 1.0f) * c + 2.0f * beta) / a0;
        f.tb1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * c) / a0;
        f.tb2 = A * ((A + 1.0f) + (A - 1.0f) * c - 2.0f * beta) / a0;
        f.ta1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * c) / a0;
        f.ta2 = ((A + 1.0f) - (A - 1.0f) * c - 2.0f * beta) / a0;
    }

    // Recomputes the target coefficients for every band. Runs on the audio
    // thread and only when a parameter changed.
    void updateTargets() {
        for (int band = 0; band < NUM_BANDS; ++band) {
            const float frequency = frequency_[band].load(std::memory_order_relaxed);
            const float gainDb = graphicGain_[band].load(std::memory_order_relaxed);
            const float q = q_[band].load(std::memory_order_relaxed);
            const Shape shape = (Shape) shape_[band].load(std::memory_order_relaxed);
            for (int ch = 0; ch < 2; ++ch) {
                Biquad &f = filters_[ch][band];
                switch (shape) {
                    case kLowShelf: setLowShelf(f, sampleRate_, frequency, gainDb); break;
                    case kHighShelf: setHighShelf(f, sampleRate_, frequency, gainDb); break;
                    default: setPeaking(f, sampleRate_, frequency, gainDb, q); break;
                }
                if (!primed_) f.settle();
            }
        }
        primed_ = true;
    }

    float runChannel(Biquad *bands, float input) {
        float x = input;
        if (!isFinite(x)) x = 0.0f;
        for (int band = 0; band < NUM_BANDS; ++band) {
            bands[band].glide(coeffSmooth_);
            x = bands[band].process(x);
            if (!isFinite(x)) { bands[band].reset(); x = 0.0f; }
        }
        return x;
    }

    float sampleRate_ = 48000.0f;
    float coeffSmooth_ = 0.0f;
    bool primed_ = false;

    Biquad filters_[2][NUM_BANDS];

    std::atomic<float> graphicGain_[NUM_BANDS] = {};
    std::atomic<float> frequency_[NUM_BANDS];
    std::atomic<float> q_[NUM_BANDS];
    std::atomic<int> shape_[NUM_BANDS] = {};
    std::atomic<bool> dirty_{true};
    std::atomic<bool> enabled_{true};
};

constexpr float Equalizer::GRAPHIC_FREQUENCIES[Equalizer::NUM_BANDS];

#endif // ALTODFX_EQUALIZER_H
