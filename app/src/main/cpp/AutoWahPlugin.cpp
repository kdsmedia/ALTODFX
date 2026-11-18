#include "AutoWah.h"

#include <cstdint>
#include <cstring>

#include "ladspa.h"

// Exposes AutoWah as a LADSPA plugin so the existing rack host can dlopen and
// run it like any other effect in app/src/main/jniLibs.
//
// Port layout (must match the "index" fields of this plugin in assets/voice.json):
//   0 = audio in, 1 = audio out,
//   2 = Sensitivity, 3 = Resonance, 4 = Depth,
//   5 = Range, 6 = Attack, 7 = Release
//
// The plugin is deliberately mono: the host runs a stereo chain and
// instantiates a mono plugin twice (once per channel, see
// FullDuplexPass::process), so each channel keeps its own envelope and filter
// state. run() works in place because the host connects the same buffer to
// both audio ports.

namespace {

const char *const kPortNames[] = {
    "Input", "Output", "Sensitivity", "Resonance",
    "Depth", "Range", "Attack", "Release",
};

const LADSPA_PortDescriptor kPortDescriptors[] = {
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
    {ALTODFX_CONTROL_HINT, 0.0f, 1.0f}, // Sensitivity
    {ALTODFX_CONTROL_HINT, 0.0f, 1.0f}, // Resonance
    {ALTODFX_CONTROL_HINT, 0.0f, 1.0f}, // Depth
    {ALTODFX_CONTROL_HINT, 0.0f, 1.0f}, // Range
    {ALTODFX_CONTROL_HINT, 0.0f, 1.0f}, // Attack
    {ALTODFX_CONTROL_HINT, 0.0f, 1.0f}, // Release
};

struct AutoWahHandle {
    explicit AutoWahHandle(float sampleRate) : dsp(sampleRate) {}

    AutoWah dsp;
    const LADSPA_Data *input = nullptr;
    LADSPA_Data *output = nullptr;
    const LADSPA_Data *sensitivity = nullptr;
    const LADSPA_Data *resonance = nullptr;
    const LADSPA_Data *depth = nullptr;
    const LADSPA_Data *range = nullptr;
    const LADSPA_Data *attack = nullptr;
    const LADSPA_Data *release = nullptr;
};

float readControl(const LADSPA_Data *port, float fallback) {
    return port != nullptr ? (float) *port : fallback;
}

LADSPA_Handle instantiate(const LADSPA_Descriptor *, unsigned long sampleRate) {
    return new AutoWahHandle((float) (sampleRate > 0 ? sampleRate : 48000));
}

void connectPort(LADSPA_Handle instance, unsigned long port, LADSPA_Data *data) {
    AutoWahHandle *h = static_cast<AutoWahHandle *>(instance);
    switch (port) {
        case 0: h->input = data; break;
        case 1: h->output = data; break;
        case 2: h->sensitivity = data; break;
        case 3: h->resonance = data; break;
        case 4: h->depth = data; break;
        case 5: h->range = data; break;
        case 6: h->attack = data; break;
        case 7: h->release = data; break;
        default: break;
    }
}

void activate(LADSPA_Handle instance) {
    static_cast<AutoWahHandle *>(instance)->dsp.reset();
}

void run(LADSPA_Handle instance, unsigned long sampleCount) {
    AutoWahHandle *h = static_cast<AutoWahHandle *>(instance);
    if (h->output == nullptr || sampleCount == 0) return;

    h->dsp.setParameters(readControl(h->sensitivity, 0.5f),
                         readControl(h->resonance, 0.55f),
                         readControl(h->depth, 0.85f),
                         readControl(h->range, 0.5f),
                         readControl(h->attack, 0.2f),
                         readControl(h->release, 0.4f));
    h->dsp.setEnabled(true);

    if (h->input != nullptr && h->input != h->output)
        std::memcpy(h->output, h->input, sampleCount * sizeof(LADSPA_Data));

    h->dsp.process(h->output, (int32_t) sampleCount);
}

void cleanup(LADSPA_Handle instance) {
    delete static_cast<AutoWahHandle *>(instance);
}

const LADSPA_Descriptor kDescriptor = {
    9001,
    "altodfx_autowah",
    LADSPA_PROPERTY_HARD_RT_CAPABLE,
    "AutoWah",
    "ALTODFX",
    "None",
    8,
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
