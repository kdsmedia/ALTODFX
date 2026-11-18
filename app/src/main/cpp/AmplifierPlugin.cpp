#include "Amplifier.h"

#include <cstdint>

#include "ladspa.h"

// Exposes GuitarAmplifier as a LADSPA plugin so the existing rack host can dlopen
// and run it like any other effect in app/src/main/jniLibs.
//
// Stereo-native: two audio inputs and two audio outputs, so the host runs a single
// instance with the left buffer on the first pair and the right buffer on the
// second (see FullDuplexPass::process).
//
// Port layout (must match the "index" fields of this plugin in assets/voice.json):
//   0 = audio in L, 1 = audio out L, 2 = audio in R, 3 = audio out R,
//   4 = Preamp Gain, 5 = Master, 6 = Bass, 7 = Middle, 8 = Treble, 9 = Presence,
//   10 = Resonance, 11 = Output, 12 = Mode (0 clean, 1 crunch, 2 lead)
//
// The host connects the same buffer to an input and its matching output (see
// FullDuplexPass::process), so run() works in place.

namespace {

const char *const kPortNames[] = {
    "Input L", "Output L", "Input R", "Output R",
    "Preamp", "Master", "Bass", "Middle", "Treble", "Presence", "Resonance", "Output", "Mode",
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
};

const LADSPA_PortRangeHint kPortRangeHints[] = {
    {0, 0.0f, 0.0f},
    {0, 0.0f, 0.0f},
    {0, 0.0f, 0.0f},
    {0, 0.0f, 0.0f},
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_LOW, 0.0f, 1.0f},      // Preamp
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_HIGH, 0.0f, 1.0f},     // Master
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MIDDLE, 0.0f, 1.0f},   // Bass
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MIDDLE, 0.0f, 1.0f},   // Middle
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MIDDLE, 0.0f, 1.0f},   // Treble
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MIDDLE, 0.0f, 1.0f},   // Presence
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_LOW, 0.0f, 1.0f},      // Resonance
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_HIGH, 0.0f, 1.0f},     // Output
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MINIMUM | LADSPA_HINT_INTEGER, 0.0f, 2.0f},  // Mode
};

struct AmplifierHandle {
    explicit AmplifierHandle(float sampleRate) : dsp(sampleRate) {}

    GuitarAmplifier dsp;
    const LADSPA_Data *inputL = nullptr;
    LADSPA_Data *outputL = nullptr;
    const LADSPA_Data *inputR = nullptr;
    LADSPA_Data *outputR = nullptr;
    const LADSPA_Data *controls[9] = {};
};

float readControl(const LADSPA_Data *port, float fallback) {
    return port != nullptr ? (float) *port : fallback;
}

LADSPA_Handle instantiate(const LADSPA_Descriptor *, unsigned long sampleRate) {
    return new AmplifierHandle((float) (sampleRate > 0 ? sampleRate : 48000));
}

void connectPort(LADSPA_Handle instance, unsigned long port, LADSPA_Data *data) {
    AmplifierHandle *h = static_cast<AmplifierHandle *>(instance);
    switch (port) {
        case 0: h->inputL = data; break;
        case 1: h->outputL = data; break;
        case 2: h->inputR = data; break;
        case 3: h->outputR = data; break;
        default: {
            const int c = (int) port - 4;
            if (c >= 0 && c < 9) h->controls[c] = data;
            break;
        }
    }
}

void activate(LADSPA_Handle instance) {
    static_cast<AmplifierHandle *>(instance)->dsp.reset();
}

void run(LADSPA_Handle instance, unsigned long sampleCount) {
    AmplifierHandle *h = static_cast<AmplifierHandle *>(instance);
    if (h->outputL == nullptr || h->outputR == nullptr || sampleCount == 0) return;

    h->dsp.setParameters(readControl(h->controls[0], 0.35f),   // Preamp
                         readControl(h->controls[1], 0.75f),   // Master
                         readControl(h->controls[2], 0.5f),    // Bass
                         readControl(h->controls[3], 0.5f),    // Middle
                         readControl(h->controls[4], 0.5f),    // Treble
                         readControl(h->controls[5], 0.5f),    // Presence
                         readControl(h->controls[6], 0.35f),   // Resonance
                         readControl(h->controls[7], 0.8f),    // Output
                         (int) readControl(h->controls[8], 0.0f));  // Mode
    h->dsp.setEnabled(true);

    // The host wires the matching input and output to the same buffer, so the copy
    // below only runs if they differ (kept for safety).
    if (h->inputL != nullptr && h->inputL != h->outputL)
        for (unsigned long i = 0; i < sampleCount; ++i) h->outputL[i] = h->inputL[i];
    if (h->inputR != nullptr && h->inputR != h->outputR)
        for (unsigned long i = 0; i < sampleCount; ++i) h->outputR[i] = h->inputR[i];

    h->dsp.process(h->outputL, h->outputR, (int32_t) sampleCount);
}

void cleanup(LADSPA_Handle instance) {
    delete static_cast<AmplifierHandle *>(instance);
}

const LADSPA_Descriptor kDescriptor = {
    9017,
    "altodfx_amplifier",
    LADSPA_PROPERTY_HARD_RT_CAPABLE,
    "Amplifier",
    "ALTODFX",
    "None",
    13,
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
