#include "Fuzz.h"

#include <cstdint>

#include "ladspa.h"

// Exposes Fuzz as a LADSPA plugin so the existing rack host can dlopen and run
// it like any other effect in app/src/main/jniLibs.
//
// Stereo-native: two audio inputs and two audio outputs, so the host runs a
// single instance with the left buffer on the first pair and the right buffer
// on the second (see FullDuplexPass::process). The DSP keeps independent filter
// state per channel.
//
// Port layout (must match the "index" fields of this plugin in assets/voice.json):
//   0 = audio in L, 1 = audio out L, 2 = audio in R, 3 = audio out R,
//   4 = Fuzz, 5 = Tone, 6 = Sustain, 7 = Bias, 8 = Level, 9 = Character
//
// The host connects the same buffer to an input and its matching output (see
// FullDuplexPass::process), so run() works in place.

namespace {

const char *const kPortNames[] = {
    "Input L", "Output L", "Input R", "Output R",
    "Fuzz", "Tone", "Sustain", "Bias", "Level", "Character",
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
};

#define ALTODFX_CONTROL_HINT \
    (LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MIDDLE)

const LADSPA_PortRangeHint kPortRangeHints[] = {
    {0, 0.0f, 0.0f},
    {0, 0.0f, 0.0f},
    {0, 0.0f, 0.0f},
    {0, 0.0f, 0.0f},
    {ALTODFX_CONTROL_HINT, 0.0f, 1.0f}, // Fuzz
    {ALTODFX_CONTROL_HINT, 0.0f, 1.0f}, // Tone
    {ALTODFX_CONTROL_HINT, 0.0f, 1.0f}, // Sustain
    {ALTODFX_CONTROL_HINT, 0.0f, 1.0f}, // Bias
    {ALTODFX_CONTROL_HINT, 0.0f, 1.0f}, // Level
    {ALTODFX_CONTROL_HINT, 0.0f, 1.0f}, // Character
};

struct FuzzHandle {
    explicit FuzzHandle(float sampleRate) : dsp(sampleRate) {}

    Fuzz dsp;
    const LADSPA_Data *inputL = nullptr;
    LADSPA_Data *outputL = nullptr;
    const LADSPA_Data *inputR = nullptr;
    LADSPA_Data *outputR = nullptr;
    const LADSPA_Data *fuzz = nullptr;
    const LADSPA_Data *tone = nullptr;
    const LADSPA_Data *sustain = nullptr;
    const LADSPA_Data *bias = nullptr;
    const LADSPA_Data *level = nullptr;
    const LADSPA_Data *character = nullptr;
};

float readControl(const LADSPA_Data *port, float fallback) {
    return port != nullptr ? (float) *port : fallback;
}

LADSPA_Handle instantiate(const LADSPA_Descriptor *, unsigned long sampleRate) {
    return new FuzzHandle((float) (sampleRate > 0 ? sampleRate : 48000));
}

void connectPort(LADSPA_Handle instance, unsigned long port, LADSPA_Data *data) {
    FuzzHandle *h = static_cast<FuzzHandle *>(instance);
    switch (port) {
        case 0: h->inputL = data; break;
        case 1: h->outputL = data; break;
        case 2: h->inputR = data; break;
        case 3: h->outputR = data; break;
        case 4: h->fuzz = data; break;
        case 5: h->tone = data; break;
        case 6: h->sustain = data; break;
        case 7: h->bias = data; break;
        case 8: h->level = data; break;
        case 9: h->character = data; break;
        default: break;
    }
}

void activate(LADSPA_Handle instance) {
    static_cast<FuzzHandle *>(instance)->dsp.reset();
}

void run(LADSPA_Handle instance, unsigned long sampleCount) {
    FuzzHandle *h = static_cast<FuzzHandle *>(instance);
    if (h->outputL == nullptr || h->outputR == nullptr || sampleCount == 0) return;

    h->dsp.setParameters(readControl(h->fuzz, 0.75f),
                         readControl(h->tone, 0.55f),
                         readControl(h->sustain, 0.7f),
                         readControl(h->bias, 0.5f),
                         readControl(h->level, 0.65f),
                         readControl(h->character, 0.25f));
    h->dsp.setEnabled(true);

    // The host wires the matching input and output to the same buffer, so the
    // left output is the left input when the two differ (kept for safety).
    if (h->inputL != nullptr && h->inputL != h->outputL)
        for (unsigned long i = 0; i < sampleCount; ++i) h->outputL[i] = h->inputL[i];
    if (h->inputR != nullptr && h->inputR != h->outputR)
        for (unsigned long i = 0; i < sampleCount; ++i) h->outputR[i] = h->inputR[i];

    h->dsp.process(h->outputL, h->outputR, (int32_t) sampleCount);
}

void cleanup(LADSPA_Handle instance) {
    delete static_cast<FuzzHandle *>(instance);
}

const LADSPA_Descriptor kDescriptor = {
    9003,
    "altodfx_fuzz",
    LADSPA_PROPERTY_HARD_RT_CAPABLE,
    "Fuzz",
    "ALTODFX",
    "None",
    10,
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
