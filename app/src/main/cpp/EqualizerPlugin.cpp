#include "Equalizer.h"

#include <cstdint>

#include "ladspa.h"

// Exposes the 10-band Equalizer as a LADSPA plugin so the existing rack host can
// dlopen and run it like any other effect in app/src/main/jniLibs.
//
// Stereo-native: two audio inputs and two audio outputs, so the host runs a
// single instance with the left buffer on the first pair and the right buffer on
// the second (see FullDuplexPass::process). The DSP keeps independent filter
// state per channel.
//
// Port layout (must match the "index" fields of this plugin in assets/voice.json):
//   0 = audio in L, 1 = audio out L, 2 = audio in R, 3 = audio out R,
//   4..13 = the ten graphic bands, low to high, each in dB.
//
// The host connects the same buffer to an input and its matching output (see
// FullDuplexPass::process), so run() works in place.

namespace {

const char *const kPortNames[] = {
    "Input L", "Output L", "Input R", "Output R",
    "31 Hz", "62 Hz", "125 Hz", "250 Hz", "500 Hz",
    "1 kHz", "2 kHz", "4 kHz", "8 kHz", "16 kHz",
};

const LADSPA_PortDescriptor kPortDescriptors[] = {
    LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO,
    LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO,
    LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO,
    LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO,
    LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
    LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
    LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
    LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
    LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
    LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
    LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
    LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
    LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
    LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
};

// Bands are cuts/boosts in dB, so the hint is "bounded below and above" with a
// neutral 0 dB default rather than the middle of the range.
#define ALTODFX_BAND_HINT \
    (LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_0)

const LADSPA_PortRangeHint kPortRangeHints[] = {
    {0, 0.0f, 0.0f},
    {0, 0.0f, 0.0f},
    {0, 0.0f, 0.0f},
    {0, 0.0f, 0.0f},
    {ALTODFX_BAND_HINT, -12.0f, 12.0f},
    {ALTODFX_BAND_HINT, -12.0f, 12.0f},
    {ALTODFX_BAND_HINT, -12.0f, 12.0f},
    {ALTODFX_BAND_HINT, -12.0f, 12.0f},
    {ALTODFX_BAND_HINT, -12.0f, 12.0f},
    {ALTODFX_BAND_HINT, -12.0f, 12.0f},
    {ALTODFX_BAND_HINT, -12.0f, 12.0f},
    {ALTODFX_BAND_HINT, -12.0f, 12.0f},
    {ALTODFX_BAND_HINT, -12.0f, 12.0f},
    {ALTODFX_BAND_HINT, -12.0f, 12.0f},
};

constexpr int kFirstBandPort = 4;

struct EqualizerHandle {
    explicit EqualizerHandle(float sampleRate) : dsp(sampleRate) {}

    Equalizer dsp;
    const LADSPA_Data *inputL = nullptr;
    LADSPA_Data *outputL = nullptr;
    const LADSPA_Data *inputR = nullptr;
    LADSPA_Data *outputR = nullptr;
    const LADSPA_Data *bands[Equalizer::NUM_BANDS] = {};
};

LADSPA_Handle instantiate(const LADSPA_Descriptor *, unsigned long sampleRate) {
    return new EqualizerHandle((float) (sampleRate > 0 ? sampleRate : 48000));
}

void connectPort(LADSPA_Handle instance, unsigned long port, LADSPA_Data *data) {
    EqualizerHandle *h = static_cast<EqualizerHandle *>(instance);
    switch (port) {
        case 0: h->inputL = data; break;
        case 1: h->outputL = data; break;
        case 2: h->inputR = data; break;
        case 3: h->outputR = data; break;
        default: {
            const int band = (int) port - kFirstBandPort;
            if (band >= 0 && band < Equalizer::NUM_BANDS) h->bands[band] = data;
            break;
        }
    }
}

void activate(LADSPA_Handle instance) {
    static_cast<EqualizerHandle *>(instance)->dsp.reset();
}

void run(LADSPA_Handle instance, unsigned long sampleCount) {
    EqualizerHandle *h = static_cast<EqualizerHandle *>(instance);
    if (h->outputL == nullptr || h->outputR == nullptr || sampleCount == 0) return;

    h->dsp.setGraphicEQ(
        h->bands[0] ? (float) *h->bands[0] : 0.0f,
        h->bands[1] ? (float) *h->bands[1] : 0.0f,
        h->bands[2] ? (float) *h->bands[2] : 0.0f,
        h->bands[3] ? (float) *h->bands[3] : 0.0f,
        h->bands[4] ? (float) *h->bands[4] : 0.0f,
        h->bands[5] ? (float) *h->bands[5] : 0.0f,
        h->bands[6] ? (float) *h->bands[6] : 0.0f,
        h->bands[7] ? (float) *h->bands[7] : 0.0f,
        h->bands[8] ? (float) *h->bands[8] : 0.0f,
        h->bands[9] ? (float) *h->bands[9] : 0.0f);
    h->dsp.setEnabled(true);

    // The host wires the matching input and output to the same buffer, so the
    // copy below only runs if they differ (kept for safety).
    if (h->inputL != nullptr && h->inputL != h->outputL)
        for (unsigned long i = 0; i < sampleCount; ++i) h->outputL[i] = h->inputL[i];
    if (h->inputR != nullptr && h->inputR != h->outputR)
        for (unsigned long i = 0; i < sampleCount; ++i) h->outputR[i] = h->inputR[i];

    h->dsp.process(h->outputL, h->outputR, (int32_t) sampleCount);
}

void cleanup(LADSPA_Handle instance) {
    delete static_cast<EqualizerHandle *>(instance);
}

const LADSPA_Descriptor kDescriptor = {
    9005,
    "altodfx_equalizer",
    LADSPA_PROPERTY_HARD_RT_CAPABLE,
    "10-Band Equalizer",
    "ALTODFX",
    "None",
    14,
    kPortDescriptors,
    kPortNames,
    kPortRangeHints,
    nullptr,
    instantiate,
    connectPort,
    activate,
    run,
    nullptr,  // run_adding
    nullptr,  // set_run_adding_gain
    nullptr,  // deactivate
    cleanup,
};

}  // namespace

extern "C" const LADSPA_Descriptor *ladspa_descriptor(unsigned long index) {
    return index == 0 ? &kDescriptor : nullptr;
}
