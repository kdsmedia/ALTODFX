#include "Tremolo.h"

#include <cstdint>
#include <cmath>

#include "ladspa.h"

// Exposes the Tremolo as a LADSPA plugin so the existing rack host can dlopen
// and run it like any other effect in app/src/main/jniLibs.
//
// Stereo-native: two audio inputs and two audio outputs, so the host runs a
// single instance with the left buffer on the first pair and the right buffer on
// the second (see FullDuplexPass::process). The DSP keeps independent LFO state
// per channel.
//
// Port layout (must match the "index" fields of this plugin in assets/voice.json):
//   0 = audio in L, 1 = audio out L, 2 = audio in R, 3 = audio out R,
//   4 = Rate (Hz), 5 = Depth, 6 = Bias, 7 = Stereo, 8 = Gain,
//   9 = Wave (0..1 across sine/triangle/square/saw up/saw down/sample-hold)
//
// The host connects the same buffer to an input and its matching output (see
// FullDuplexPass::process), so run() works in place.

namespace {

const char *const kPortNames[] = {
    "Input L", "Output L", "Input R", "Output R",
    "Rate", "Depth", "Bias", "Stereo", "Gain", "Wave",
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

const LADSPA_PortRangeHint kPortRangeHints[] = {
    {0, 0.0f, 0.0f},
    {0, 0.0f, 0.0f},
    {0, 0.0f, 0.0f},
    {0, 0.0f, 0.0f},
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MIDDLE, 0.01f, 20.0f},  // Rate
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_HIGH, 0.0f, 1.0f},      // Depth
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MIDDLE, -1.0f, 1.0f},   // Bias
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_LOW, 0.0f, 1.0f},       // Stereo
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MAXIMUM, 0.0f, 2.0f},   // Gain
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_LOW, 0.0f, 1.0f},       // Wave
};

struct TremoloHandle {
    explicit TremoloHandle(float sampleRate) : dsp(sampleRate) {}

    Tremolo dsp;
    const LADSPA_Data *inputL = nullptr;
    LADSPA_Data *outputL = nullptr;
    const LADSPA_Data *inputR = nullptr;
    LADSPA_Data *outputR = nullptr;
    const LADSPA_Data *controls[6] = {};
};

float readControl(const LADSPA_Data *port, float fallback) {
    return port != nullptr ? (float) *port : fallback;
}

LADSPA_Handle instantiate(const LADSPA_Descriptor *, unsigned long sampleRate) {
    return new TremoloHandle((float) (sampleRate > 0 ? sampleRate : 48000));
}

void connectPort(LADSPA_Handle instance, unsigned long port, LADSPA_Data *data) {
    TremoloHandle *h = static_cast<TremoloHandle *>(instance);
    switch (port) {
        case 0: h->inputL = data; break;
        case 1: h->outputL = data; break;
        case 2: h->inputR = data; break;
        case 3: h->outputR = data; break;
        default: {
            const int c = (int) port - 4;
            if (c >= 0 && c < 6) h->controls[c] = data;
            break;
        }
    }
}

void activate(LADSPA_Handle instance) {
    static_cast<TremoloHandle *>(instance)->dsp.reset();
}

void run(LADSPA_Handle instance, unsigned long sampleCount) {
    TremoloHandle *h = static_cast<TremoloHandle *>(instance);
    if (h->outputL == nullptr || h->outputR == nullptr || sampleCount == 0) return;

    h->dsp.setParameters(readControl(h->controls[0], 4.0f),    // Rate
                         readControl(h->controls[1], 0.75f),   // Depth
                         readControl(h->controls[2], 0.0f),    // Bias
                         readControl(h->controls[3], 0.0f),    // Stereo
                         readControl(h->controls[4], 1.0f));   // Gain
    // The Wave port is 0..1 across the six shapes.
    const float wave = readControl(h->controls[5], 0.0f);
    h->dsp.setWaveform((int) std::lround(wave * 5.0f));
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
    delete static_cast<TremoloHandle *>(instance);
}

const LADSPA_Descriptor kDescriptor = {
    9009,
    "altodfx_tremolo",
    LADSPA_PROPERTY_HARD_RT_CAPABLE,
    "Tremolo",
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
