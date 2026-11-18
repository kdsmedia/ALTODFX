#include "DistortionPlus.h"

#include <cstdint>

#include "ladspa.h"

// Exposes DistortionPlus as a LADSPA plugin so the existing rack host can dlopen
// and run it like any other effect in app/src/main/jniLibs.
//
// Stereo-native: two audio inputs and two audio outputs, so the host runs a single
// instance with the left buffer on the first pair and the right buffer on the
// second (see FullDuplexPass::process).
//
// Port layout (must match the "index" fields of this plugin in assets/voice.json):
//   0 = audio in L, 1 = audio out L, 2 = audio in R, 3 = audio out R,
//   4 = Input Gain (dB), 5 = Drive, 6 = Bass (dB), 7 = Mid (dB), 8 = Treble (dB),
//   9 = Presence, 10 = Hard Clip (0 soft, 1 hard), 11 = Output (dB)
//
// The host connects the same buffer to an input and its matching output (see
// FullDuplexPass::process), so run() works in place.

namespace {

const char *const kPortNames[] = {
    "Input L", "Output L", "Input R", "Output R",
    "Input Gain", "Drive", "Bass", "Mid", "Treble", "Presence", "Hard Clip", "Output",
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
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MIDDLE, -24.0f, 24.0f},  // Input Gain
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MIDDLE, 0.0f, 1.0f},      // Drive
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MIDDLE, -12.0f, 12.0f},  // Bass
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MIDDLE, -12.0f, 12.0f},  // Mid
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MIDDLE, -12.0f, 12.0f},  // Treble
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MIDDLE, 0.0f, 1.0f},     // Presence
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MINIMUM | LADSPA_HINT_INTEGER, 0.0f, 1.0f},  // Hard Clip
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_LOW, -24.0f, 12.0f},     // Output
};

struct DistortionPlusHandle {
    explicit DistortionPlusHandle(float sampleRate) : dsp(sampleRate) {}

    DistortionPlus dsp;
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
    return new DistortionPlusHandle((float) (sampleRate > 0 ? sampleRate : 48000));
}

void connectPort(LADSPA_Handle instance, unsigned long port, LADSPA_Data *data) {
    DistortionPlusHandle *h = static_cast<DistortionPlusHandle *>(instance);
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
    static_cast<DistortionPlusHandle *>(instance)->dsp.reset();
}

void run(LADSPA_Handle instance, unsigned long sampleCount) {
    DistortionPlusHandle *h = static_cast<DistortionPlusHandle *>(instance);
    if (h->outputL == nullptr || h->outputR == nullptr || sampleCount == 0) return;

    h->dsp.setParameters(readControl(h->controls[0], 0.0f),      // Input Gain
                         readControl(h->controls[1], 0.65f),     // Drive
                         readControl(h->controls[2], 0.0f),      // Bass
                         readControl(h->controls[3], 0.0f),      // Mid
                         readControl(h->controls[4], 0.0f),      // Treble
                         readControl(h->controls[5], 0.5f),      // Presence
                         readControl(h->controls[6], 0.0f),      // Hard Clip
                         readControl(h->controls[7], -6.0f));    // Output
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
    delete static_cast<DistortionPlusHandle *>(instance);
}

const LADSPA_Descriptor kDescriptor = {
    9016,
    "altodfx_distortionplus",
    LADSPA_PROPERTY_HARD_RT_CAPABLE,
    "Distortion+",
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
