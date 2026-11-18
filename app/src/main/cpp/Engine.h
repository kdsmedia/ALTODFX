#ifndef __ENGINE__H
#define __ENGINE__H
#include <jni.h>
#include <oboe/Oboe.h>
#include <string>
#include <thread>
#include <mutex>
#include "FullDuplexPass.h"
#include "SharedLibrary.h"
#include "Plugin.h"
#include "Meter.h"
#include "LockFreeQueue.h"

class Engine : public oboe::AudioStreamCallback {
public:
    LockFreeQueueManager queueManager ;
    static oboe::DataCallbackResult outputCapture (
            std::shared_ptr<oboe::AudioStream> inputStream,
            const void *inputData,
            int   numOutputFrames) {
    }

    std::string LIBRARY_PATH ;
    Engine() ;
    std::string tuneLatency();
    static JavaVM * vm ;
    static JNIEnv * env ;
    static bool lockFreeThreadAttached ;
    static jclass mainActivity;
    static jmethodID pushSamplesMethod;
    static jfloatArray pushSamples ;
    static std::string mainActivityClassName ;
    bool              mIsEffectOn = false;
    bool setPluginBuffer (float * buffer, int buffer_size, int plugin) ;

    int deletePluginFromRack(int pIndex);
    void setRecordingDeviceId(int32_t deviceId);

    void setPlaybackDeviceId(int32_t deviceId);

    // Package name is required for input streams on Android 12+ (the platform
    // validates the recorder identity when opening a capture stream).
    void setPackageName(const std::string &packageName) { mPackageName = packageName; }

    // True when the selected input is a wired/USB headset-style interface
    // (for example an iRig plugged into the 3.5 mm jack). Such devices need
    // InputPreset::VoiceRecognition on many phones to open the TRRS mic.
    void setHeadsetInput(bool headset) { mHeadsetInput = headset; }

    bool setEffectOn(bool isOn);
    /*
     * oboe::AudioStreamDataCallback interface implementation
     */
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream *oboeStream,
                                          void *audioData, int32_t numFrames) override;

    /*
     * oboe::AudioStreamErrorCallback interface implementation
     */
    void onErrorBeforeClose(oboe::AudioStream *oboeStream, oboe::Result error) override;
    void onErrorAfterClose(oboe::AudioStream *oboeStream, oboe::Result error) override;

    bool setAudioApi(oboe::AudioApi);
    bool isAAudioRecommended(void);
    void addPluginToRack(int libraryIndex, int pluginIndex);

    int32_t           mSampleRate = oboe::kUnspecified;
    void * handle ;

    FullDuplexPass    mFullDuplexPass;
    bool bootComplete = false ;
    bool lowLatencyMode = false ;
    bool lazyLoad = false ;

    Meter * meter ;

    std::vector <SharedLibrary *> libraries ;
    static std::vector<Plugin *> activePlugins ;

    void loadPlugin(char *filename, SharedLibrary::PluginType type = SharedLibrary::LADSPA );
    void loadPlugins();
    int moveActivePluginDown(int _p);
    int moveActivePluginUp(int _p);
    void buildPluginChain();
    void addPluginToRackLazy(char * library, int pluginIndex, SharedLibrary::PluginType _type = SharedLibrary::LADSPA);
    int addPlugintoRackByName(std::string);
    int lowLatency = 12 ;
    // The rack is stereo end to end: the effect chain and the output stream
    // both carry two channels, so stereo effects (chorus, delay, reverb) keep
    // their width.
    //
    // The capture side is left at the device default instead of being forced
    // to stereo. A guitar on the 3.5 mm jack is a mono source, and requesting
    // a stereo capture stream makes many phones fail to open it, which would
    // silence the whole app. The callback upmixes a mono input to L = R, so a
    // mono capture still feeds a fully stereo chain.
    const int32_t     mInputChannelCount = oboe::ChannelCount::Unspecified;
    const int32_t     mOutputChannelCount = oboe::ChannelCount::Stereo;
    const oboe::AudioFormat mFormat = oboe::AudioFormat::Float; // for easier processing

    std::shared_ptr<oboe::AudioStream> mRecordingStream;
    std::shared_ptr<oboe::AudioStream> mPlayStream;

    static jmethodID pushAudio ;

    void test();
    long getTimeStamp();
    oboe::LatencyTuner *latencyTuner, *latencyTunerOut;

private:
    int32_t           mRecordingDeviceId = oboe::kUnspecified;
    int32_t           mPlaybackDeviceId = oboe::kUnspecified;
    // Unspecified lets Oboe pick AAudio when available and fall back to
    // OpenSL ES automatically; forcing AAudio made many devices fail to open.
    oboe::AudioApi    mAudioApi = oboe::AudioApi::Unspecified;
    std::string       mPackageName;
    bool              mHeadsetInput = false;
    // Guards openStreams/closeStreams against concurrent start/stop and the
    // disconnect recovery path in onErrorAfterClose.
    std::mutex        mLock;
    oboe::Result openStreams();

    void closeStreams();

    void closeStream(std::shared_ptr<oboe::AudioStream> &stream);

    oboe::Result openOutputStream(oboe::AudioStreamBuilder &builder);

    oboe::Result openInputStream(oboe::AudioStreamBuilder &builder,
                                 int32_t sampleRate,
                                 int32_t outputCapacity);

    void warnIfNotLowLatency(std::shared_ptr<oboe::AudioStream> &stream);


    void discoverPlugins();


    void setSampleRateDisplay(int sampleRate, bool lowLatency);

public:
    double getLatency(bool input);

    void setPluginFilename(std::string filename, int plugin);

    void popFunction();

    int getBufferFrameSize(bool input);

    void fixGlitches();

    void minimizeLatency();

    void setupPushSamples(std::string methodName);

    static JNIEnv *Env();

    static int push(AudioBuffer *buffer);

    static std::string pushSamplesMethodName;

    static jclass findClassWithEnv(JNIEnv *env, const char *name);

    void setBufferSizeFactor(float factor);

    static int resetAtomPorts(AudioBuffer *buffer);
} ;

#endif // __ENGINE__H