#include "Flanger.h"

#include <cstdint>
#include <cmath>

#include "ladspa.h"

// Exposes the Flanger as a LADSPA plugin so the existing rack host can dlopen and
// run it like any other effect in app/src/main/jniLibs.
//
// Stereo-native: two audio inputs and two audio outputs, so the host runs a
// single instance with the left buffer on the first pair and the right buffer on
// the second (see FullDuplexPass::process). The DSP keeps independent delay
// lines per channel.
//
// Port layout (must match the "index" fields of this plugin in assets/voice.json):
//   0 = audio in L, 1 = audio out L, 2 = audio in R, 3 = audio out R,
//   4 = Rate (Hz), 5 = Depth, 6 = Delay (ms), 7 = Feedback, 8 = Mix,
//   9 = Stereo, 10 = Gain, 11 = Wave (0 = sine, 0.5 = triangle, 1 = square)
//
// The host connects the same buffer to an input and its matching output (see
// FullDuplexPass::process), so run() works in place.

namespace {

const char *const kPortNames[] = {
    "Input L", "Output L", "Input R", "Output R",
    "Rate", "Depth", "Delay", "Feedback", "Mix", "Stereo", "Gain", "Wave",
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
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_LOW, 0.01f, 10.0f},    // Rate
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_HIGH, 0.0f, 1.0f},     // Depth
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_LOW, 0.1f, 10.0f},    // Delay
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MIDDLE, -0.95f, 0.95f},// Feedback
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MIDDLE, 0.0f, 1.0f},   // Mix
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MAXIMUM, 0.0f, 1.0f},  // Stereo
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MAXIMUM, 0.0f, 2.0f},  // Gain
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_LOW, 0.0f, 1.0f},      // Wave
};

struct FlangerHandle {
    explicit FlangerHandle(float sampleRate) : dsp(sampleRate) {}

    Flanger dsp;
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
    return new FlangerHandle((float) (sampleRate > 0 ? sampleRate : 48000));
}

void connectPort(LADSPA_Handle instance, unsigned long port, LADSPA_Data *data) {
    FlangerHandle *h = static_cast<FlangerHandle *>(instance);
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
    static_cast<FlangerHandle *>(instance)->dsp.reset();
}

void run(LADSPA_Handle instance, unsigned long sampleCount) {
    FlangerHandle *h = static_cast<FlangerHandle *>(instance);
    if (h->outputL == nullptr || h->outputR == nullptr || sampleCount == 0) return;

    h->dsp.setParameters(readControl(h->controls[0], 0.25f),   // Rate
                         readControl(h->controls[1], 0.75f),   // Depth
                         readControl(h->controls[2], 2.0f),    // Delay
                         readControl(h->controls[3], 0.55f),   // Feedback
                         readControl(h->controls[4], 0.5f),    // Mix
                         readControl(h->controls[5], 1.0f),    // Stereo
                         readControl(h->controls[6], 1.0f));   // Gain
    // The Wave port is 0..1 across the three shapes.
    const float wave = readControl(h->controls[7], 0.0f);
    h->dsp.setWaveform((int) std::lround(wave * 2.0f));
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
    delete static_cast<FlangerHandle *>(instance);
}

const LADSPA_Descriptor kDescriptor = {
    9007,
    "altodfx_flanger",
    LADSPA_PROPERTY_HARD_RT_CAPABLE,
    "Flanger",
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
