#include "Phaser.h"

#include <cstdint>

#include "ladspa.h"

// Exposes the Phaser as a LADSPA plugin so the existing rack host can dlopen and
// run it like any other effect in app/src/main/jniLibs.
//
// Stereo-native: two audio inputs and two audio outputs, so the host runs a
// single instance with the left buffer on the first pair and the right buffer on
// the second (see FullDuplexPass::process). The DSP keeps independent all-pass
// state per channel.
//
// Port layout (must match the "index" fields of this plugin in assets/voice.json):
//   0 = audio in L, 1 = audio out L, 2 = audio in R, 3 = audio out R,
//   4 = Rate, 5 = Depth, 6 = Feedback, 7 = Mix, 8 = Center,
//   9 = Stereo, 10 = Stages, 11 = Level
//
// The host connects the same buffer to an input and its matching output (see
// FullDuplexPass::process), so run() works in place.

namespace {

const char *const kPortNames[] = {
    "Input L", "Output L", "Input R", "Output R",
    "Rate", "Depth", "Feedback", "Mix", "Center", "Stereo", "Stages", "Level",
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

#define ALTODFX_UNIT_HINT \
    (LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MIDDLE)

const LADSPA_PortRangeHint kPortRangeHints[] = {
    {0, 0.0f, 0.0f},
    {0, 0.0f, 0.0f},
    {0, 0.0f, 0.0f},
    {0, 0.0f, 0.0f},
    {ALTODFX_UNIT_HINT, 0.0f, 1.0f},  // Rate
    {ALTODFX_UNIT_HINT, 0.0f, 1.0f},  // Depth
    {ALTODFX_UNIT_HINT, 0.0f, 1.0f},  // Feedback
    {ALTODFX_UNIT_HINT, 0.0f, 1.0f},  // Mix
    {ALTODFX_UNIT_HINT, 0.0f, 1.0f},  // Center
    {ALTODFX_UNIT_HINT, 0.0f, 1.0f},  // Stereo
    {ALTODFX_UNIT_HINT, 0.0f, 1.0f},  // Stages
    {ALTODFX_UNIT_HINT, 0.0f, 1.0f},  // Level
};

struct PhaserHandle {
    explicit PhaserHandle(float sampleRate) : dsp(sampleRate) {}

    Phaser dsp;
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
    return new PhaserHandle((float) (sampleRate > 0 ? sampleRate : 48000));
}

void connectPort(LADSPA_Handle instance, unsigned long port, LADSPA_Data *data) {
    PhaserHandle *h = static_cast<PhaserHandle *>(instance);
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
    static_cast<PhaserHandle *>(instance)->dsp.reset();
}

void run(LADSPA_Handle instance, unsigned long sampleCount) {
    PhaserHandle *h = static_cast<PhaserHandle *>(instance);
    if (h->outputL == nullptr || h->outputR == nullptr || sampleCount == 0) return;

    h->dsp.setParameters(readControl(h->controls[0], 0.35f),   // Rate
                         readControl(h->controls[1], 0.75f),   // Depth
                         readControl(h->controls[2], 0.55f),   // Feedback
                         readControl(h->controls[3], 0.5f),    // Mix
                         readControl(h->controls[4], 0.45f),   // Center
                         readControl(h->controls[5], 0.5f));   // Stereo
    h->dsp.setStageAmount(readControl(h->controls[6], 2.0f / 3.0f));  // Stages
    h->dsp.setLevel(readControl(h->controls[7], 1.0f));               // Level
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
    delete static_cast<PhaserHandle *>(instance);
}

const LADSPA_Descriptor kDescriptor = {
    9006,
    "altodfx_phaser",
    LADSPA_PROPERTY_HARD_RT_CAPABLE,
    "Phaser",
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
