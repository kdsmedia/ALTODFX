#include "Normalizer.h"

#include <cstdint>
#include <cmath>

#include "ladspa.h"

// Exposes the Normalizer as a LADSPA plugin so the existing rack host can dlopen
// and run it like any other effect in app/src/main/jniLibs.
//
// Stereo-native: two audio inputs and two audio outputs, so the host runs a
// single instance with the left buffer on the first pair and the right buffer on
// the second (see FullDuplexPass::process). The level is measured across both
// channels together, so the stereo image is preserved.
//
// Port layout (must match the "index" fields of this plugin in assets/voice.json):
//   0 = audio in L, 1 = audio out L, 2 = audio in R, 3 = audio out R,
//   4 = Input (dB), 5 = Target (dB), 6 = Max Gain (dB), 7 = Attack (ms),
//   8 = Release (ms), 9 = Gate (dB), 10 = Gate On, 11 = Out (dB)
//
// The host connects the same buffer to an input and its matching output (see
// FullDuplexPass::process), so run() works in place.

namespace {

const char *const kPortNames[] = {
    "Input L", "Output L", "Input R", "Output R",
    "Input", "Target", "Max Gain", "Attack", "Release", "Gate", "Gate On", "Out",
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
};

const LADSPA_PortRangeHint kPortRangeHints[] = {
    {0, 0.0f, 0.0f},
    {0, 0.0f, 0.0f},
    {0, 0.0f, 0.0f},
    {0, 0.0f, 0.0f},
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MIDDLE, -24.0f, 24.0f}, // Input
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MIDDLE, -36.0f, 0.0f},   // Target
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_HIGH, 0.0f, 30.0f},      // Max Gain
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_LOW, 0.1f, 200.0f},      // Attack
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MIDDLE, 10.0f, 3000.0f}, // Release
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MINIMUM, -100.0f, -20.0f},// Gate
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_INTEGER | LADSPA_HINT_DEFAULT_MINIMUM, 0.0f, 1.0f}, // Gate On
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MIDDLE, -24.0f, 24.0f},  // Out
};

struct NormalizerHandle {
    explicit NormalizerHandle(float sampleRate) : dsp(sampleRate) {}

    Normalizer dsp;
    const LADSPA_Data *inputL = nullptr;
    LADSPA_Data *outputL = nullptr;
    const LADSPA_Data *inputR = nullptr;
    LADSPA_Data *outputR = nullptr;
    const LADSPA_Data *controls[8] = {};
};

float readControl(const LADSPA_Data *port, float fallback) {
    return port != nullptr ? (float) *port : fallback;
}

LADSPA_Handle instantiate(const LADSPA_Descriptor *, unsigned long sampleRate) {
    return new NormalizerHandle((float) (sampleRate > 0 ? sampleRate : 48000));
}

void connectPort(LADSPA_Handle instance, unsigned long port, LADSPA_Data *data) {
    NormalizerHandle *h = static_cast<NormalizerHandle *>(instance);
    switch (port) {
        case 0: h->inputL = data; break;
        case 1: h->outputL = data; break;
        case 2: h->inputR = data; break;
        case 3: h->outputR = data; break;
        default: {
            const int c = (int) port - 4;
            if (c >= 0 && c < 8) h->controls[c] = data;
            break;
        }
    }
}

void activate(LADSPA_Handle instance) {
    static_cast<NormalizerHandle *>(instance)->dsp.reset();
}

void run(LADSPA_Handle instance, unsigned long sampleCount) {
    NormalizerHandle *h = static_cast<NormalizerHandle *>(instance);
    if (h->outputL == nullptr || h->outputR == nullptr || sampleCount == 0) return;

    const float gateOn = readControl(h->controls[6], 0.0f);
    h->dsp.setParameters(readControl(h->controls[0], 0.0f),    // Input
                         readControl(h->controls[1], -12.0f),  // Target
                         readControl(h->controls[2], 18.0f),   // Max Gain
                         readControl(h->controls[3], 10.0f),   // Attack
                         readControl(h->controls[4], 250.0f),  // Release
                         readControl(h->controls[5], -70.0f),  // Gate
                         readControl(h->controls[7], 0.0f),    // Out
                         gateOn >= 0.5f);                      // Gate On
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
    delete static_cast<NormalizerHandle *>(instance);
}

const LADSPA_Descriptor kDescriptor = {
    9014,
    "altodfx_normalizer",
    LADSPA_PROPERTY_HARD_RT_CAPABLE,
    "Normalizer",
    "ALTODFX",
    "None",
    12,
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
