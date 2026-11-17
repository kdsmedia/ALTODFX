#include "TubeDrive.h"

#include <cstdlib>
#include <cstring>

#include "ladspa.h"

// Exposes TubeDrive as a LADSPA plugin so the existing rack host can dlopen and
// run it like any other effect in app/src/main/jniLibs.
//
// Port layout (must match the "index" fields of this plugin in assets/voice.json):
//   0 = audio in, 1 = audio out, 2 = Drive, 3 = Tone, 4 = Level
//
// The host connects the same buffer to the input and output audio ports
// (see FullDuplexPass::process), so run() must work in place.

namespace {

const char *const kPortNames[] = {"Input", "Output", "Drive", "Tone", "Level"};

const LADSPA_PortDescriptor kPortDescriptors[] = {
    LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO,
    LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO,
    LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
    LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
    LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
};

const LADSPA_PortRangeHint kPortRangeHints[] = {
    {0, 0.0f, 0.0f},
    {0, 0.0f, 0.0f},
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MIDDLE, 0.0f, 1.0f},
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MIDDLE, 0.0f, 1.0f},
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_HIGH, 0.0f, 1.0f},
};

struct TubeDriveHandle {
    explicit TubeDriveHandle(float sampleRate) : dsp(sampleRate) {}

    TubeDrive dsp;
    const LADSPA_Data *input = nullptr;
    LADSPA_Data *output = nullptr;
    const LADSPA_Data *drive = nullptr;
    const LADSPA_Data *tone = nullptr;
    const LADSPA_Data *level = nullptr;
};

LADSPA_Handle instantiate(const LADSPA_Descriptor *, unsigned long sampleRate) {
    return new TubeDriveHandle((float) (sampleRate > 0 ? sampleRate : 48000));
}

void connectPort(LADSPA_Handle instance, unsigned long port, LADSPA_Data *data) {
    TubeDriveHandle *h = static_cast<TubeDriveHandle *>(instance);
    switch (port) {
        case 0: h->input = data; break;
        case 1: h->output = data; break;
        case 2: h->drive = data; break;
        case 3: h->tone = data; break;
        case 4: h->level = data; break;
        default: break;
    }
}

void activate(LADSPA_Handle instance) {
    static_cast<TubeDriveHandle *>(instance)->dsp.reset();
}

void run(LADSPA_Handle instance, unsigned long sampleCount) {
    TubeDriveHandle *h = static_cast<TubeDriveHandle *>(instance);
    if (h->output == nullptr || sampleCount == 0) return;

    h->dsp.setParameters(h->drive ? *h->drive : 0.5f,
                         h->tone ? *h->tone : 0.5f,
                         h->level ? *h->level : 0.75f);
    h->dsp.setEnabled(true);

    if (h->input != nullptr && h->input != h->output)
        std::memcpy(h->output, h->input, sampleCount * sizeof(LADSPA_Data));

    h->dsp.process(h->output, (int32_t) sampleCount);
}

void cleanup(LADSPA_Handle instance) {
    delete static_cast<TubeDriveHandle *>(instance);
}

const LADSPA_Descriptor kDescriptor = {
    9000,
    "altodfx_tubedrive",
    LADSPA_PROPERTY_HARD_RT_CAPABLE,
    "Tube Drive",
    "ALTODFX",
    "None",
    5,
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
