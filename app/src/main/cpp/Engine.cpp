#include <cassert>

#include "Engine.h"
#include "logging_macros.h"

jmethodID Engine::pushAudio = nullptr;
jclass Engine::mainActivity = nullptr;
JNIEnv * Engine::env = nullptr;
JavaVM * Engine::vm = nullptr;
std::vector<Plugin *> Engine::activePlugins ;
jmethodID Engine::pushSamplesMethod = nullptr;
jfloatArray Engine::pushSamples = nullptr;
bool Engine::lockFreeThreadAttached = false ;
std::string Engine::mainActivityClassName = "";
std::string Engine::pushSamplesMethodName = "";
static jobject gClassLoader;
static jmethodID gFindClassMethod;

Engine::Engine () {
    assert(mOutputChannelCount == mInputChannelCount);
//    queueManager = new LockFreeQueueManager ();
    queueManager.add_function (resetAtomPorts);
    queueManager.add_function (meter->updateMeterOutput);
//    discoverPlugins();
//    loadPlugins();
}

void Engine::setRecordingDeviceId(int32_t deviceId) {
    mRecordingDeviceId = deviceId;
}

void Engine::setPlaybackDeviceId(int32_t deviceId) {
    mPlaybackDeviceId = deviceId;
}

bool Engine::isAAudioRecommended() {
    return oboe::AudioStreamBuilder::isAAudioRecommended();
}

bool Engine::setAudioApi(oboe::AudioApi api) {
    if (mIsEffectOn) return false;
    mAudioApi = api;
    return true;
}

bool Engine::setEffectOn(bool isOn) {
    IN ;
    bool success = true;
    if (isOn != mIsEffectOn) {
        if (isOn) {
            // Reopening and closing the streams must not race with the
            // disconnect recovery path, which also rebuilds them.
            std::lock_guard<std::mutex> guard(mLock);
            success = openStreams() == oboe::Result::OK;
            if (success) {
                /*
                for (int i = 0 ; i < activePlugins.size() ; i ++) {
                    activePlugins.at(i)->print();
                }
                */

//                mFullDuplexPass.queue = queueManager.process ;
                mFullDuplexPass.lockFreeQueueManager = &queueManager ;

                queueManager.init (mPlayStream->getBufferSizeInFrames()/mPlayStream->getChannelCount());

                // The scratch buffers must exist before the streams start, or
                // the first audio callback writes through a null pointer.
                // Note: do not assign mBufferSize here, because
                // FullDuplexStream::start() only allocates its input buffer
                // when the computed size is strictly larger.
                const int32_t scratchSamples = mPlayStream->getBufferCapacityInFrames()
                        * mPlayStream->getChannelCount();

                if (mFullDuplexPass.inSamples != NULL)
                    free (static_cast<void *>(mFullDuplexPass.inSamples));

                if (mFullDuplexPass.raw != NULL)
                    free (static_cast<void *>(mFullDuplexPass.raw));

                // The buffer holds samples, so the byte size must include
                // sizeof(float); allocating one byte per sample overflowed the
                // heap on every audio callback.
                const size_t bufferBytes =
                        static_cast<size_t>(scratchSamples) * sizeof(float);
                mFullDuplexPass.inSamples = static_cast<float *>(malloc(bufferBytes));
                LOGD("[engine] allocated %d bytes to raw samples", static_cast<int>(bufferBytes));
                mFullDuplexPass.raw = static_cast<float *>(malloc(bufferBytes));

                mFullDuplexPass.start();

                meter->jack_samplerate = mSampleRate ;
                meter->lowLatency = lowLatencyMode ;
                meter->enable();
                meter->start();
//                addPluginToRack(0, 0);
                mIsEffectOn = isOn;
            }
        } else {
//            LOGD("stopping meter");
            meter->stop();

            mFullDuplexPass.stop();
            {
                std::lock_guard<std::mutex> guard(mLock);
                closeStreams();
            }
            mIsEffectOn = isOn;
        }
    }

    OUT
    return success;
}


void Engine::closeStreams() {
    IN ;
    /*
    * Note: The order of events is important here.
    * The playback stream must be closed before the recording stream. If the
    * recording stream were to be closed first the playback stream's
    * callback may attempt to read from the recording stream
    * which would cause the app to crash since the recording stream would be
    * null.
    */

    queueManager.quit ();

    closeStream(mPlayStream);
    mFullDuplexPass.setOutputStream(nullptr);

    closeStream(mRecordingStream);
    mFullDuplexPass.setInputStream(nullptr);
}

oboe::Result  Engine::openStreams() {
    IN ;
    // The order of stream creation is important. We open the playback stream
    // first, then reuse its sample rate for the recording stream. Matching the
    // properties keeps both streams on the same low-latency path.
    //
    // Each stream is opened through a layered fallback ladder. Forcing a single
    // combination (Exclusive + LowLatency + AAudio + Unprocessed) makes many
    // devices refuse to open the stream, which shows up as "the app makes no
    // sound at all".
    oboe::AudioStreamBuilder outBuilder;
    if (!mPackageName.empty()) {
        outBuilder.setPackageName(mPackageName);
    }
    oboe::Result result = openOutputStream(outBuilder);
    if (result != oboe::Result::OK) {
        LOGE("Failed to open output stream. Error %s", oboe::convertToText(result));
        mPlayStream.reset();
        mSampleRate = oboe::kUnspecified;
        OUT ;
        return result;
    }
    // The input stream needs to run at the same sample rate as the output.
    mSampleRate = mPlayStream->getSampleRate();
    warnIfNotLowLatency(mPlayStream);

    oboe::AudioStreamBuilder inBuilder;
    if (!mPackageName.empty()) {
        // Required for input streams on Android 12+ (the platform checks the
        // recorder identity when a capture stream is opened).
        inBuilder.setPackageName(mPackageName);
    }
    result = openInputStream(inBuilder, mSampleRate,
                             mPlayStream->getBufferCapacityInFrames());
    if (result != oboe::Result::OK) {
        LOGE("Failed to open input stream. Error %s", oboe::convertToText(result));
        mRecordingStream.reset();
        closeStream(mPlayStream);
        mSampleRate = oboe::kUnspecified;
        OUT ;
        return result;
    }
    warnIfNotLowLatency(mRecordingStream);

    latencyTuner = new oboe::LatencyTuner ( *mRecordingStream);
    latencyTuner->tune();
    latencyTunerOut = new oboe::LatencyTuner ( *mPlayStream);
    latencyTunerOut->tune();

    mRecordingStream->setBufferSizeInFrames(mRecordingStream->getFramesPerBurst() * 2);
    mPlayStream->setBufferSizeInFrames(mPlayStream->getFramesPerBurst() * 2);
    mFullDuplexPass.setInputStream(mRecordingStream);
    mFullDuplexPass.setOutputStream(mPlayStream);

    mFullDuplexPass.inputConditioner.setSampleRate((float) mSampleRate);
    mFullDuplexPass.inputConditioner.reset();

    // Load LADSPA Plugin here

    /*
    LOGV("Checking if plugin state is null ...\n");
    if (mFullDuplexPass.duplexPluginState == NULL) {
        LOGV("Assinging plugin\n");
        mFullDuplexPass .duplexPluginState = loadPlugin();
        pluginState = mFullDuplexPass.duplexPluginState;
    } else {
        LOGV("It isn't ... already loaded %s\n", mFullDuplexPass.duplexPluginState -> descriptor -> Name);
    }

    LOGD("%s loadPlugin [ok]: %s %s\n", __PRETTY_FUNCTION__ , mFullDuplexPass .get_plugin() -> descriptor -> Label, mFullDuplexPass .get_plugin() -> descriptor -> Name);
     */
    OUT ;
    return result;
}

/**
 * Opens the playback stream with a layered fallback. Exclusive + LowLatency is
 * tried first for the best latency; if the device refuses it we drop to Shared,
 * then to PerformanceMode::None, and finally let the device pick its own sample
 * rate. After the first failed attempt the explicit device id is discarded so
 * the remaining attempts use the system default device.
 */
oboe::Result Engine::openOutputStream(oboe::AudioStreamBuilder &builder) {
    int32_t deviceId = mPlaybackDeviceId;
    if (deviceId < 0) {
        deviceId = oboe::kUnspecified;
    }

    struct Attempt {
        oboe::SharingMode sharing;
        oboe::PerformanceMode performance;
        int32_t sampleRate; // 0 = let the device choose (kUnspecified).
    };
    const Attempt attempts[] = {
            {oboe::SharingMode::Exclusive, oboe::PerformanceMode::LowLatency, oboe::kUnspecified},
            {oboe::SharingMode::Shared, oboe::PerformanceMode::LowLatency, oboe::kUnspecified},
            {oboe::SharingMode::Shared, oboe::PerformanceMode::None, oboe::kUnspecified},
    };

    oboe::Result lastResult = oboe::Result::ErrorInternal;
    for (const auto &attempt : attempts) {
        builder.setDirection(oboe::Direction::Output)
                ->setDataCallback(this)
                ->setErrorCallback(this)
                ->setAudioApi(mAudioApi)
                ->setDeviceId(deviceId)
                ->setSharingMode(attempt.sharing)
                ->setPerformanceMode(attempt.performance)
                ->setFormat(mFormat)
                ->setChannelCount(mOutputChannelCount)
                ->setSampleRate(attempt.sampleRate)
                // Allow conversion so devices that only accept I16 / a fixed
                // rate still get a stream instead of failing outright.
                ->setFormatConversionAllowed(true)
                ->setChannelConversionAllowed(true)
                ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium);
        lastResult = builder.openStream(mPlayStream);
        if (lastResult == oboe::Result::OK) {
            return lastResult;
        }
        LOGE("Open output failed (sharing=%d, perf=%d): %s",
             static_cast<int>(attempt.sharing), static_cast<int>(attempt.performance),
             oboe::convertToText(lastResult));
        mPlayStream.reset();
        // The specific device is gone; use the system default from now on.
        deviceId = oboe::kUnspecified;
    }
    return lastResult;
}

/**
 * Opens the recording stream with a layered fallback. For a headset-style input
 * (iRig in the 3.5 mm jack) InputPreset::VoiceRecognition is tried first because
 * on many phones that is the preset that opens the TRRS microphone; the built-in
 * mic and USB inputs keep InputPreset::Unprocessed so the guitar keeps its own
 * character (no AGC or noise suppression).
 */
oboe::Result Engine::openInputStream(oboe::AudioStreamBuilder &builder,
                                     int32_t sampleRate,
                                     int32_t outputCapacity) {
    int32_t deviceId = mRecordingDeviceId;
    if (deviceId < 0) {
        deviceId = oboe::kUnspecified;
    }
    const bool headset = mHeadsetInput;

    struct Attempt {
        oboe::SharingMode sharing;
        oboe::PerformanceMode performance;
        oboe::InputPreset preset;
    };
    const Attempt attempts[] = {
            {oboe::SharingMode::Exclusive, oboe::PerformanceMode::LowLatency,
             headset ? oboe::InputPreset::VoiceRecognition : oboe::InputPreset::Unprocessed},
            {oboe::SharingMode::Exclusive, oboe::PerformanceMode::LowLatency,
             oboe::InputPreset::Generic},
            {oboe::SharingMode::Shared, oboe::PerformanceMode::LowLatency,
             oboe::InputPreset::Generic},
            {oboe::SharingMode::Shared, oboe::PerformanceMode::None,
             oboe::InputPreset::Generic},
    };

    oboe::Result lastResult = oboe::Result::ErrorInternal;
    for (const auto &attempt : attempts) {
        builder.setDirection(oboe::Direction::Input)
                ->setErrorCallback(this)
                ->setAudioApi(mAudioApi)
                ->setDeviceId(deviceId)
                ->setSharingMode(attempt.sharing)
                ->setPerformanceMode(attempt.performance)
                ->setFormat(mFormat)
                ->setChannelCount(mInputChannelCount)
                ->setSampleRate(sampleRate)
                ->setInputPreset(attempt.preset)
                ->setFormatConversionAllowed(true)
                ->setChannelConversionAllowed(true)
                ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium);
        // Give the input twice the output capacity so it does not underrun.
        if (outputCapacity > 0) {
            builder.setBufferCapacityInFrames(outputCapacity * 2);
        }
        lastResult = builder.openStream(mRecordingStream);
        if (lastResult == oboe::Result::OK) {
            return lastResult;
        }
        LOGE("Open input failed (sharing=%d, preset=%d): %s",
             static_cast<int>(attempt.sharing), static_cast<int>(attempt.preset),
             oboe::convertToText(lastResult));
        mRecordingStream.reset();
        // The specific device is gone; use the system default from now on.
        deviceId = oboe::kUnspecified;
    }
    return lastResult;
}

/**
 * Close the stream. AudioStream::close() is a blocking call so
 * the application does not need to add synchronization between
 * onAudioReady() function and the thread calling close().
 * [the closing thread is the UI thread in this sample].
 * @param stream the stream to close
 */
void Engine::closeStream(std::shared_ptr<oboe::AudioStream> &stream) {
    IN ;
    if (stream) {
        oboe::Result result = stream->stop();
        if (result != oboe::Result::OK) {
            LOGW("Error stopping stream: %s", oboe::convertToText(result));
        }
        result = stream->close();
        if (result != oboe::Result::OK) {
            LOGE("Error closing stream: %s", oboe::convertToText(result));
        } else {
            LOGW("Successfully closed streams");
        }
        stream.reset();
    }

    OUT ;
}

/**
 * Warn in logcat if non-low latency stream is created
 * @param stream: newly created stream
 *
 */
void Engine::warnIfNotLowLatency(std::shared_ptr<oboe::AudioStream> &stream) {
    if (stream->getPerformanceMode() != oboe::PerformanceMode::LowLatency) {
        LOGW(
                "Stream is NOT low latency."
                "Check your requested format, sample rate and channel count");
        LOGW("Running in mode: %d\tchannels: %d\tsharing mode: %d\tsample rate: %d\tformat: %d",
             stream->getPerformanceMode(),
             stream->getChannelCount(),
             stream->getSharingMode(),
             stream->getSampleRate(),
             stream->getFormat());
        lowLatencyMode = false ;
    } else {
        lowLatencyMode = true ;
        LOGD ("Congratulations, you have achieved Low Latency!");
        LOGD("Running in Low Latency mode: %d\tchannels: %d\tsharing mode: %d\tsample rate: %d\tformat: %d\tbuffer size: %d",
             stream->getPerformanceMode(),
             stream->getChannelCount(),
             stream->getSharingMode(),
             stream->getSampleRate(),
             stream->getFormat(),
             stream->getBufferSizeInFrames());
    }
}

/**
 * Handles playback stream's audio request. In this sample, we simply block-read
 * from the record stream for the required samples.
 *
 * @param oboeStream: the playback stream that requesting additional samples
 * @param audioData:  the buffer to load audio samples for playback stream
 * @param numFrames:  number of frames to load to audioData buffer
 * @return: DataCallbackResult::Continue.
 */
oboe::DataCallbackResult Engine::onAudioReady(
        oboe::AudioStream *oboeStream, void *audioData, int32_t numFrames) {
    return mFullDuplexPass.onAudioReady(oboeStream, audioData, numFrames);
}

/**
 * Oboe notifies the application for "about to close the stream".
 *
 * @param oboeStream: the stream to close
 * @param error: oboe's reason for closing the stream
 */
void Engine::onErrorBeforeClose(oboe::AudioStream *oboeStream,
                                          oboe::Result error) {
    LOGE("%s stream Error before close: %s",
         oboe::convertToText(oboeStream->getDirection()),
         oboe::convertToText(error));
}

/**
 * Oboe notifies application that "the stream is closed"
 *
 * @param oboeStream
 * @param error
 */
void Engine::onErrorAfterClose(oboe::AudioStream *oboeStream,
                                         oboe::Result error) {
    LOGE("%s stream Error after close: %s",
         oboe::convertToText(oboeStream->getDirection()),
         oboe::convertToText(error));

    // A device was unplugged or reconnected (for example the guitar interface
    // was pulled out). Oboe has already closed the stream, so reopen both
    // streams to keep audio alive instead of leaving the app silent.
    if (error != oboe::Result::ErrorDisconnected || !mIsEffectOn) {
        return;
    }
    LOGI("Audio device disconnected, reopening streams");

    // A second thread (start/stop) may be holding the engine; skip recovery
    // then, because that thread is already rebuilding the streams.
    std::unique_lock<std::mutex> guard(mLock, std::try_to_lock);
    if (!guard.owns_lock()) {
        return;
    }

    closeStreams();
    mIsEffectOn = false;
    if (openStreams() == oboe::Result::OK && mFullDuplexPass.start() == oboe::Result::OK) {
        mIsEffectOn = true;
        LOGI("Audio device reconnected, streams reopened");
    } else {
        LOGE("Failed to reopen streams after device disconnect");
        closeStreams();
    }
}

void Engine::addPluginToRackLazy(char* library, int pluginIndex, SharedLibrary::PluginType _type) {
    SharedLibrary * sharedLibrary = new SharedLibrary (library, _type);
    sharedLibrary -> vm = vm ;
    if (mainActivityClassName != "")
        sharedLibrary->mainActivityClassName = mainActivityClassName ;

    sharedLibrary ->setLibraryPath(LIBRARY_PATH);
    sharedLibrary->load();

    if (sharedLibrary->descriptors.size() == 0) {
        LOGE("Unable to load shared library!") ;
        return;
    }

    LOGD("loaded shared library [ok] ... now trying to load plugin");
    Plugin * plugin = new Plugin (sharedLibrary->descriptors.at(pluginIndex), (long) mSampleRate, _type);
    plugin->sharedLibrary = sharedLibrary;
    if (_type == SharedLibrary::LV2) {
        plugin -> load () ;
    }
    activePlugins .push_back(plugin);
    buildPluginChain();
}

void Engine::loadPlugin(char * filename, SharedLibrary::PluginType type) {
    IN
    LOGD("Loading plugin %s", filename);
    SharedLibrary * sharedLibrary = new SharedLibrary (filename, type);
    libraries.push_back(sharedLibrary);
    OUT
}

void Engine::discoverPlugins () {
    IN

    if (libraries.size() != 0) {
        // we only run this once
        LOGE("tried to re-discover plugins! not allowed") ;
        OUT
        return ;
    }

    SharedLibrary * sharedLibrary = new SharedLibrary ("libamp.so");
    libraries.push_back(sharedLibrary);
    SharedLibrary * sharedLibrary1 = new SharedLibrary ("libdistortionx.so");
    libraries.push_back(sharedLibrary1);
    SharedLibrary * sharedLibrary2 = new SharedLibrary ("libcrybabyx.so");
    libraries.push_back(sharedLibrary2);
    SharedLibrary * sharedLibrary3 = new SharedLibrary ("/data/data/com.shajikhan.ladspa.amprack/lib/libtubex.so");
    libraries.push_back(sharedLibrary3);
    SharedLibrary * sharedLibrary4 = new SharedLibrary ("/data/data/com.shajikhan.ladspa.plugins.tap/lib/libtap_eq.so");
    libraries.push_back(sharedLibrary4);
    OUT
}

void Engine::buildPluginChain () {
    IN
    LOGD("building chain for %d plugins", activePlugins.size());
    mFullDuplexPass.activePlugins = 0;
    for (Plugin *p: activePlugins) {
        if (!p->active)
            continue;
        mFullDuplexPass.inputPorts [mFullDuplexPass.activePlugins] = p->inputPort ;
        mFullDuplexPass.outputPorts [mFullDuplexPass.activePlugins] = p->outputPort ;
        mFullDuplexPass.inputPorts2 [mFullDuplexPass.activePlugins] = p->inputPort2 ;
        mFullDuplexPass.outputPorts2 [mFullDuplexPass.activePlugins] = p->outputPort2 ;
        if (p->type == SharedLibrary::LADSPA) {
            mFullDuplexPass.connect_port [mFullDuplexPass.activePlugins] = p->descriptor->connect_port ;
            mFullDuplexPass.run [mFullDuplexPass.activePlugins] = p->descriptor->run ;
            mFullDuplexPass.run_adding [mFullDuplexPass.activePlugins] = p->descriptor->run_adding ;
            mFullDuplexPass.set_run_adding_gain [mFullDuplexPass.activePlugins] = p->descriptor->set_run_adding_gain ;
            mFullDuplexPass.descriptor [mFullDuplexPass.activePlugins] = p->descriptor;
        } else if (p->type == SharedLibrary::LV2) {
            mFullDuplexPass.connect_port [mFullDuplexPass.activePlugins] = reinterpret_cast<void (*)(
                    LADSPA_Handle, unsigned long, LADSPA_Data *)>(p->lv2Descriptor->connect_port);
            mFullDuplexPass.run [mFullDuplexPass.activePlugins] = reinterpret_cast<void (*)(
                    LADSPA_Handle, unsigned long)>(p->lv2Descriptor->run);
            mFullDuplexPass.descriptor [mFullDuplexPass.activePlugins] = reinterpret_cast<const LADSPA_Descriptor *>(p->lv2Descriptor);
        }

        mFullDuplexPass.run_adding_gain [mFullDuplexPass.activePlugins] = p->run_adding_gain ;
        mFullDuplexPass.handle [mFullDuplexPass.activePlugins] = p->handle ;
        mFullDuplexPass.activePlugins ++ ;
    }

    if (mIsEffectOn) {
        LOGD("adjusting latency ...");

        latencyTuner->tune();
        latencyTunerOut->tune();
    }

    OUT
}

void Engine::addPluginToRack (int libraryIndex, int pluginIndex) {
    IN
    LOGD("Adding plugin %d: %d", libraryIndex, pluginIndex);
    if (libraryIndex > libraries.size()) {
        LOGF ("index %d > libraries size %d", libraryIndex, libraries.size()) ;
        return ;
    }

    Plugin * plugin = new Plugin (libraries.at(libraryIndex)->descriptors.at(pluginIndex), (long) mSampleRate);
    activePlugins .push_back(plugin);
    buildPluginChain();
    OUT
}

int Engine::deletePluginFromRack (int pIndex) {
    IN
    Plugin * p = activePlugins.at(pIndex);
    for (PluginControl * control: p->pluginControls) {
        control->freeMemory();
    }

    if (lazyLoad) {
        p->free();
        p->sharedLibrary->unload();
    }

    delete p ;
    activePlugins.erase( next(begin(activePlugins), pIndex));
    buildPluginChain();
    return activePlugins.size();
    OUT
}

void Engine::loadPlugins () {
    /*
    if (libraries.size()) {
        LOGD("%d pLugins already loaded, not loading again", libraries.size());
        return ;
    }
     */

    for (SharedLibrary *sharedLibrary: libraries) {
        sharedLibrary->setSampleRate(mSampleRate);
        sharedLibrary->load();
    }

    LOGD("[Audio Engine]: Initialized %d plugins!", libraries.size());
}

int Engine :: moveActivePluginDown (int _p) {
    IN
    if (_p == activePlugins.size()) {
        OUT
        return _p ;
    }

    auto it = activePlugins.begin() + _p;
    std::rotate(it, it + 1, activePlugins.end());
    buildPluginChain();
    OUT
    return _p + 1 ;
}

int Engine :: moveActivePluginUp (int _p) {
    IN
    if (_p == 0) {
        OUT
        return _p ;
    }

    auto it = activePlugins.begin() + _p;
    std::rotate(it - 1,  it, activePlugins.end());
    buildPluginChain();
    OUT
    return _p - 1 ;
}

int Engine::addPlugintoRackByName (std::string pluginName) {
    for (int i = 0 ; i < libraries.size() ; i ++) {
        SharedLibrary *sharedLibrary = libraries.at(i);
        for (int j = 0 ; j < sharedLibrary->total_plugins; j ++) {
            if (std::string (sharedLibrary->descriptors [j]->Name) == pluginName) {
                addPluginToRack(i, j);
                return 1 ;
            }
        }
    }

    return  0 ;
}

void Engine::setPluginFilename (std::string filename, int plugin) {
    IN
    mFullDuplexPass.bypass = true;
    activePlugins.at(plugin)->setFileName(filename);
    mFullDuplexPass.bypass = false;
    OUT
}

bool Engine::setPluginBuffer (float * buffer, int buffer_size, int plugin) {
    IN
//    LOGD("plugins in queue %d requested %d", activePlugins.size(), plugin);
    mFullDuplexPass.bypass = true ;
////    activePlugins.at (plugin)->active = false ;
    activePlugins.at (plugin)->setBuffer (buffer, buffer_size);
////    activePlugins.at (plugin)->active = true ;
    mFullDuplexPass.bypass = false ;
    OUT
    return true ;
}

std::string Engine::tuneLatency () {
    IN
    if (! mIsEffectOn)
        return std::string ("Turn on audio engine first") ;

    mRecordingStream->setBufferSizeInFrames(mRecordingStream->getFramesPerBurst() * 2);
    mPlayStream->setBufferSizeInFrames(mPlayStream->getFramesPerBurst() * 2);

    latencyTuner->requestReset();
    oboe::Result result = latencyTuner->tune() ;
    latencyTunerOut->requestReset();
    oboe::Result resultOut = latencyTunerOut->tune() ;
//    mPlayStream->setBufferSizeInFrames(mPlayStream->getFramesPerBurst());
    char tmp [450];
    // hello, old friend
    sprintf (tmp, "Latency: %.0fms, Xruns: %d/%d, Buffer size: %d/%d",
             getLatency(true) + getLatency(false),
             mRecordingStream->getXRunCount(),mPlayStream->getXRunCount(),
             mRecordingStream->getBufferSizeInFrames(), mPlayStream->getBufferSizeInFrames());
    LOGD ("%s",tmp);
    OUT
    return std::string (tmp) ;
}

void Engine::test () {
    float * sampleData ;

    oboe::Result result ;
    while (mIsEffectOn) {
        result = mPlayStream->read(&sampleData, 192, 0) ;
        LOGD("read %d result", result);
    }
}


long Engine::getTimeStamp () {
    const oboe::ResultWithValue<oboe::FrameTimestamp> &frameTimeStamp = mRecordingStream->getTimestamp(
            CLOCK_MONOTONIC);
    return frameTimeStamp.value().timestamp ;
}

void Engine::setSampleRateDisplay (int sampleRate, bool lowLatency) {

}

double Engine::getLatency (bool input) {
    if (mPlayStream->usesAAudio())
        LOGI("using AAudio");

    if (input) {
        const oboe::ResultWithValue<double> &latency = mRecordingStream->calculateLatencyMillis();
        return latency.value();
    } else {
        int frames = mPlayStream->getFramesWritten() - mPlayStream->getFramesRead();
        int bufferLatencyMillis = 1000 * frames / mPlayStream->getSampleRate();

        const oboe::ResultWithValue<double> &latency = mPlayStream->calculateLatencyMillis();
        LOGD("[%f: %d] %d %d %d %d", latency.value(), bufferLatencyMillis,
                mPlayStream->getFramesPerBurst(),
                mPlayStream->getXRunCount().value(),
                mPlayStream->getBufferSizeInFrames(),
                mPlayStream->getBufferCapacityInFrames());
//        mPlayStream->setBufferSizeInFrames(mPlayStream->getFramesPerBurst());
        return latency.value() ;// - bufferLatencyMillis;
    }
}

void Engine::popFunction () {
    queueManager.pop_function();
}

int Engine::getBufferFrameSize (bool input) {
    if (input)
        return mRecordingStream->getBufferSizeInFrames();
    else
        return mPlayStream->getBufferSizeInFrames();
}

void Engine::fixGlitches () {
    IN
    mRecordingStream->setBufferSizeInFrames(mRecordingStream->getFramesPerBurst());
    mPlayStream->setBufferSizeInFrames(mPlayStream->getBufferCapacityInFrames());
    OUT
}

void Engine::minimizeLatency () {
    IN

    mPlayStream->setBufferSizeInFrames(mPlayStream->getFramesPerBurst());
    mRecordingStream->setBufferSizeInFrames(mRecordingStream->getFramesPerBurst());

    OUT
}

void Engine::setBufferSizeFactor (float factor) {
    IN

    mPlayStream->setBufferSizeInFrames(mPlayStream->getFramesPerBurst() * factor);
    mRecordingStream->setBufferSizeInFrames(mRecordingStream->getFramesPerBurst() * factor);

    OUT
}

void Engine::setupPushSamples (std::string methodName) {
    pushSamplesMethodName = methodName ;
    queueManager.add_function(push);
}

JNIEnv* Engine::Env() {
    IN
    if (vm == nullptr) {
        LOGE("gJvm == nullptr");
    }

    JNIEnv *jniEnv;
    int status = vm->GetEnv((void**)&jniEnv, JNI_VERSION_1_6);
    if(status < 0) {
        status = vm->AttachCurrentThread(&jniEnv, NULL);
        if(status < 0) {
            OUT
            LOGE("STATUS < 0");
            return nullptr;
        }
    }

    LOGD("[getenv] attached thread id %d", gettid());
    OUT
    return jniEnv;
}

int Engine::push (AudioBuffer * buffer) {
    IN

    if (! lockFreeThreadAttached) {
        env = Env () ;
        auto randomClass = env->FindClass("java/lang/ClassLoader");
        jclass classClass = env->GetObjectClass(randomClass);
        auto classLoaderClass = env->FindClass("java/lang/ClassLoader");
        auto getClassLoaderMethod = env->GetMethodID(classClass, "getClassLoader",
                                                     "()Ljava/lang/ClassLoader;");
        gClassLoader = (jclass) env->NewGlobalRef(env->CallObjectMethod(randomClass, getClassLoaderMethod));
        gFindClassMethod = env->GetMethodID(classLoaderClass, "findClass",
                                            "(Ljava/lang/String;)Ljava/lang/Class;");
        mainActivity = findClassWithEnv(env, mainActivityClassName.c_str());
        if (mainActivity == nullptr) {
            LOGF ("Cannot find class %s", mainActivityClassName.c_str());
            abort();
        }

        pushSamplesMethod = env->GetStaticMethodID(mainActivity, pushSamplesMethodName.c_str(),"([FI)V");

        pushSamples = env->NewFloatArray(4096);
        lockFreeThreadAttached = true ;
    }

    float * data = buffer->data ;
    int samples = buffer -> pos ;

    env->SetFloatArrayRegion(pushSamples, 0, samples+1,
                                   data);
    env->CallStaticVoidMethod(mainActivity, pushSamplesMethod, pushSamples,
                              samples);

    OUT
    return 0;
}

jclass Engine::findClassWithEnv(JNIEnv *env, const char* name) {
    return static_cast<jclass>(env->CallObjectMethod(gClassLoader, gFindClassMethod, env->NewStringUTF(name)));
}

int Engine::resetAtomPorts (AudioBuffer * buffer) {
    for (int i = 0 ; i < activePlugins.size(); i ++) {
        if (activePlugins.at(i)->notifyPort != nullptr) {
            activePlugins.at(i)->check_notify();
        }
    }

    return 0 ;
}