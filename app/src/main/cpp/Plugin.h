#ifndef __PLUGIN_H
#define __PLUGIN_H
#include "ladspa.h"
#include "lv2.h"
#include "lv2/atom/atom.h"

#include <cstddef>
#include <fstream>
#include "logging_macros.h"
#include <vector>

#include "android/asset_manager.h"
#include "android/asset_manager_jni.h"

#include "PluginControl.h"
#include "SharedLibrary.h"
#include "json.hpp"
#include "lv2_ext.h"
#include "atom.h"
#include "symap.h"
//~ #include "lv2/atom/forge.h"

class Plugin {
    LADSPA_Data ** portControls ;
    unsigned long sampleRate ;
public:
    URID urid = URID ();
    LV2_URID_Map * ampMap = nullptr;
    std::vector<const LV2_Feature*> features;
    std::vector<const LV2_Feature*> m_featurePointers;
    LV2_URID_Map lv2UridMap ;
    LV2_Feature featureURID ;
    LV2_Log_Log logLog ;
    Symap * symap = nullptr;
    AmpAtom * ampAtom = nullptr;
    LV2_Feature featureLog ;
    LV2_Feature featureSchedule ;
    LV2_Worker_Schedule lv2WorkerSchedule ;
    LV2_Feature featureState ;
    int filePortSize = 8192 ;
    std::string prefix ;
    LV2_Atom_Sequence * filePort = nullptr;//= static_cast<LV2_Atom_Sequence *>(malloc(sizeof (LV2_Atom_Sequence)));
    LV2_Atom_Sequence * notifyPort = nullptr;//= static_cast<LV2_Atom_Sequence *>(malloc(sizeof (LV2_Atom_Sequence)));
    int filePortIndex = -1 ;
    // Actual LV2 port numbers the first instance connected the atom/file and
    // notify buffers to, so the second (right-channel) instance mirrors them
    // instead of guessing.
    int filePortNum = -1 ;
    int notifyPortNum = -1 ;
//    LV2_Atom_Forge forge;

    const LV2_Feature* const* featurePointers() const
    {
        return m_featurePointers.data();
    }

    void setBuffer (float * buffer, int read_bytes) ;
    bool active = true ;
    SharedLibrary::PluginType type ;
    int ID ;
    std::string lv2_name ;
    LADSPA_Data run_adding_gain = 1 ;
    std::vector <PluginControl *> pluginControls ;
    const LADSPA_Descriptor * descriptor ;
    const LV2_Descriptor * lv2Descriptor;
    SharedLibrary * sharedLibrary;
    int inputPort = -1;
    int inputPort2 = -1;
    int outputPort = -1;
    int outputPort2 = -1;
    LADSPA_Data dummy_output_control_port = 0; // from th pulseaudio ladspa sink module
    LADSPA_Handle *handle ;
    // Second instance of a mono plugin, so a stereo chain keeps independent
    // state for the right channel. Null for stereo-native plugins.
    LADSPA_Handle *handle2 = nullptr ;
    Plugin(const LADSPA_Descriptor * descriptor, unsigned long _sampleRate, SharedLibrary::PluginType _type = SharedLibrary::LADSPA);

    // True when the plugin declares a single audio input and a single audio
    // output, in which case it must be instantiated once per channel.
    bool isMono () const { return inputPort != -1 && inputPort2 == -1
                               && outputPort != -1 && outputPort2 == -1 ; }

    // Creates handle2 for a mono plugin. Control and atom ports are shared with
    // the first instance so both channels follow the same settings; only the
    // audio ports stay unconnected because the host rewires them every block.
    void createSecondHandle () ;
    void print();

    void free();
    std::string getLV2JSON (std::string pluginName);

    void load();

    int addPluginControl(const LV2_Descriptor *_descriptor, nlohmann::json _j);
#ifndef __ANDROID__
    std::string getLV2JSON_PC (std::string pluginName) ;
#endif

    void setFileName(std::string filename);
    std::string loadedFileName ;
    int loadedFileType = -1 ;

    void lv2FeaturesInit();
    void lv2FeaturesURID();

    // have to begin somewhere
    LV2_Worker_Interface * lv2WorkerInterface ;
    LV2_State_Interface  * lv2StateInterface ;

    void lv2ConnectWorkers();

    void setFilePortValue(std::string filename);

    void setFilePortValue1(std::string filename);
    void setAtomPortValue (int control, std::string text) ;

    void check_notify();
};

LV2_Worker_Status lv2ScheduleWork (LV2_Worker_Schedule_Handle  handle, uint32_t size, const void * data);
uint32_t lv2_options_set (LV2_Handle instance, const LV2_Options_Option* options) ;
uint32_t lv2_options_get (LV2_Handle instance, LV2_Options_Option* options) ;

template<class UnaryFunction>
void recursive_iterate(const Plugin &plugin, const nlohmann::json& j, UnaryFunction f)
{
    for(auto it = j.begin(); it != j.end(); ++it)
    {
        if (it->is_structured())
        {
            recursive_iterate(plugin, *it, f);
        }
        else
        {
            f(plugin, it);
        }
    }
}

#endif // __PLUGIN_H
