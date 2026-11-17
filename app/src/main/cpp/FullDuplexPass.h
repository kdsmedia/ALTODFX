/*
 * Copyright 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SAMPLES_FULLDUPLEXPASS_H
#define SAMPLES_FULLDUPLEXPASS_H

#include <dlfcn.h>
#include <cmath>
#include <atomic>
#include "ladspa.h"
#include "FullDuplexStream.h"
#include "Meter.h"
#include "LockFreeQueue.h"

// Front-end conditioning applied to the raw input before the effect chain.
// A guitar plugged into a headset-jack interface (iRIG style) picks up DC
// offset, mains hum and hiss; removing those here keeps the whole chain clean.
class InputConditioner {
public:
    bool enabled = true ;
    float gateThreshold = 0.0035f ; // linear amplitude
    float highpassHz = 80.0f ;
    float sampleRate = 48000.0f ;

    inline void setSampleRate (float sr) {
        if (sr <= 0) return ;
        sampleRate = sr ;
        // 1st order high-pass coefficient (also removes DC offset).
        float rc = 1.0f / (2.0f * (float) M_PI * highpassHz) ;
        float dt = 1.0f / sampleRate ;
        mHighpassAlpha = rc / (rc + dt) ;
        mAttack = std::exp (-1.0f / (0.002f * sampleRate)) ;
        mRelease = std::exp (-1.0f / (0.150f * sampleRate)) ;
        mOpenStep = 1.0f / (0.001f * sampleRate) ;
        mCloseStep = 1.0f / (0.050f * sampleRate) ;
    }

    inline void reset () {
        mX1 = mY1 = 0.0f ;
        mEnv = 0.0f ;
        mGain = 1.0f ;
        mHoldCounter = 0 ;
    }

    inline float process (float x) {
        if (! enabled) return x ;

        // DC blocker + high-pass in one stage.
        float y = mHighpassAlpha * (mY1 + x - mX1) ;
        mX1 = x ;
        mY1 = y ;

        // Envelope follower for the noise gate.
        float rectified = std::fabs (y) ;
        if (rectified > mEnv)
            mEnv = mAttack * mEnv + (1.0f - mAttack) * rectified ;
        else
            mEnv = mRelease * mEnv + (1.0f - mRelease) * rectified ;

        // Hysteresis: open above the threshold, only close once the envelope
        // falls below half of it. Without the second level the gate chatters on
        // a decaying note and chops the tail of every chord.
        const float closeThreshold = gateThreshold * 0.5f ;
        if (mEnv > gateThreshold) {
            mHoldCounter = (int) (0.05f * sampleRate) ; // 50 ms hold
        } else if (mHoldCounter > 0) {
            mHoldCounter -- ;
        }

        bool open = (mEnv > gateThreshold) || (mHoldCounter > 0 && mEnv > closeThreshold) ;
        if (open) {
            mGain += mOpenStep ;           // fast open (~1 ms)
        } else {
            mGain -= mCloseStep ;          // slow close (~50 ms)
        }
        if (mGain > 1.0f) mGain = 1.0f ;
        if (mGain < 0.0f) mGain = 0.0f ;

        return y * mGain ;
    }

private:
    float mHighpassAlpha = 0.99478f ;
    float mAttack = 0.9896f ;
    float mRelease = 0.99986f ;
    float mX1 = 0.0f, mY1 = 0.0f ;
    float mEnv = 0.0f ;
    float mGain = 1.0f ;
    int   mHoldCounter = 0 ;
    // Per-sample ramps chosen for a 48 kHz rate; scaled at setSampleRate time.
    float mOpenStep = 1.0f / (0.001f * 48000.0f) ;
    float mCloseStep = 1.0f / (0.050f * 48000.0f) ;
};

// Final safety stage on the output. The effect chain can easily sum several
// gain stages and clip the DAC hard, which sounds like harsh buzzing; a soft
// peak limiter keeps the level musical instead of crackling.
class OutputLimiter {
public:
    bool enabled = true ;
    float ceiling = 0.95f ;

    inline float process (float x) {
        if (! enabled) return x ;
        const float threshold = ceiling * 0.7f ;
        float a = std::fabs (x) ;
        if (a <= threshold) return x ;
        // Soft knee: smoothly compress everything above the threshold toward the
        // ceiling instead of hard-clipping.
        float over = a - threshold ;
        float range = ceiling - threshold ;
        float compressed = threshold + range * (over / (over + range)) ;
        return (x < 0.0f ? -compressed : compressed) ;
    }
};

// Pitch capture for the tuner. The audio callback appends the conditioned
// input into a ring buffer; the UI thread polls it and runs autocorrelation.
// This is intentionally independent of Meter, whose tuner path has mismatched
// JNI signatures and never worked.
class AudioTuner {
public:
    static const int SIZE = 8192 ;   // power of two
    static const int WINDOW = 2048 ; // samples analysed per reading

    std::atomic<bool> enabled { false } ;
    std::atomic<int>  writeIndex { 0 } ;

    inline void push (float sample) {
        if (! enabled.load (std::memory_order_relaxed)) return ;
        int i = writeIndex.load (std::memory_order_relaxed) ;
        mBuffer [i & (SIZE - 1)] = sample ;
        writeIndex.store (i + 1, std::memory_order_relaxed) ;
    }

    // Copies the most recent `count` samples, oldest first, into `out`.
    void snapshot (float * out, int count) const {
        int w = writeIndex.load (std::memory_order_relaxed) ;
        int start = w - count ;
        for (int i = 0 ; i < count ; i ++)
            out [i] = mBuffer [(start + i) & (SIZE - 1)] ;
    }

private:
    float mBuffer [SIZE] = { 0 } ;
} ;

// Autocorrelation pitch estimate. Returns the frequency in Hz, or 0 when the
// signal is too quiet to be a note. `outRms` receives the signal level and
// `outCents` the deviation from the nearest semitone.
inline float detectPitch (const float * x, int n, float sampleRate,
                          float * outRms, float * outCents) {
    if (outRms) *outRms = 0.0f ;
    if (outCents) *outCents = 0.0f ;

    float mean = 0.0f ;
    for (int i = 0 ; i < n ; i ++) mean += x [i] ;
    mean /= (float) n ;

    float rms = 0.0f ;
    for (int i = 0 ; i < n ; i ++) {
        float v = x [i] - mean ;
        rms += v * v ;
    }
    rms = std::sqrt (rms / (float) n) ;
    if (outRms) *outRms = rms ;
    if (rms < 0.002f) return 0.0f ;

    int minLag = (int) (sampleRate / 1200.0f) ;
    int maxLag = (int) (sampleRate / 60.0f) ;
    if (maxLag > n / 2) maxLag = n / 2 ;
    if (minLag < 1) minLag = 1 ;
    if (maxLag <= minLag) return 0.0f ;

    const int MAX_LAG = 4096 ;
    float nsdf [MAX_LAG] ;
    if (maxLag >= MAX_LAG) maxLag = MAX_LAG - 1 ;

    // Normalised square difference (McLeod): a value near 1 marks a period.
    float globalMax = 0.0f ;
    for (int lag = minLag ; lag <= maxLag ; lag ++) {
        float ac = 0.0f, m0 = 0.0f, ml = 0.0f ;
        for (int i = 0 ; i < n - lag ; i ++) {
            float a = x [i] - mean ;
            float b = x [i + lag] - mean ;
            ac += a * b ;
            m0 += a * a ;
            ml += b * b ;
        }
        float denom = m0 + ml ;
        float v = denom > 1e-12f ? 2.0f * ac / denom : 0.0f ;
        nsdf [lag] = v ;
        if (v > globalMax) globalMax = v ;
    }
    if (globalMax <= 0.0f) return 0.0f ;

    // The fundamental is the first prominent peak, not the global maximum:
    // picking the maximum often selects a harmonic multiple (octave error).
    int bestLag = -1 ;
    float thresh = 0.80f * globalMax ;
    for (int lag = minLag + 1 ; lag < maxLag ; lag ++) {
        if (nsdf [lag] > nsdf [lag - 1] && nsdf [lag] >= nsdf [lag + 1] &&
            nsdf [lag] >= thresh) {
            bestLag = lag ;
            break ;
        }
    }
    if (bestLag < 0) {
        for (int lag = minLag ; lag <= maxLag ; lag ++)
            if (nsdf [lag] >= globalMax) { bestLag = lag ; break ; }
    }
    if (bestLag <= 0) return 0.0f ;

    // Parabolic interpolation around the peak for sub-sample accuracy.
    float lagF = (float) bestLag ;
    if (bestLag > minLag && bestLag < maxLag) {
        float y0 = nsdf [bestLag - 1] ;
        float y1 = nsdf [bestLag] ;
        float y2 = nsdf [bestLag + 1] ;
        float denom = 2.0f * (2.0f * y1 - y0 - y2) ;
        if (std::fabs (denom) > 1e-9f)
            lagF = (float) bestLag + (y2 - y0) / denom ;
    }

    if (lagF <= 0.0f) return 0.0f ;
    float freq = sampleRate / lagF ;
    if (freq < 40.0f || freq > 1500.0f) return 0.0f ;

    if (outCents) {
        float midi = 69.0f + 12.0f * std::log2 (freq / 440.0f) ;
        float nearest = std::round (midi) ;
        *outCents = (midi - nearest) * 100.0f ;
    }
    return freq ;
}

class FullDuplexPass : public FullDuplexStream {
    //| TODO: Limit number of plugins active in free version?
public:
    #define MAX_PLUGINS 32 // room for a full professional effect rack
    InputConditioner inputConditioner ;
    OutputLimiter outputLimiter ;
    AudioTuner tuner ;
    float inputVolume = 1.0 ;
    float outputVolume = 1.0 ;
    bool meterEnabled = true ;
    bool pause = false ;
    
    // Lock Free Queue Manager
    void (queue)(float * data, int frames) ;
    LockFreeQueueManager * lockFreeQueueManager;

    void (*connect_port [MAX_PLUGINS])(LADSPA_Handle Instance,
                         unsigned long Port,
                         LADSPA_Data * DataLocation);
    void (*run [MAX_PLUGINS])(LADSPA_Handle Instance,
                unsigned long SampleCount);
    void (*run_adding [MAX_PLUGINS])(LADSPA_Handle Instance,
                unsigned long SampleCount);
    void (*set_run_adding_gain [MAX_PLUGINS])(LADSPA_Handle Instance,
                                LADSPA_Data   Gain);

    void * handle [MAX_PLUGINS] ;
    LADSPA_Data run_adding_gain [MAX_PLUGINS] ;
    const LADSPA_Descriptor * descriptor [MAX_PLUGINS] ;
    int inputPorts [MAX_PLUGINS] ;
    int inputPorts2 [MAX_PLUGINS] ;
    int outputPorts [MAX_PLUGINS] ;
    int outputPorts2 [MAX_PLUGINS] ;
    bool pushToLockFreeBeforeOutputVolume = false ;
    int activePlugins =  0 ;
    float * raw = NULL; // this will never be greater than this(TM)
    bool bypass = false ;
    float * inSamples = NULL ;

    virtual oboe::DataCallbackResult
    onBothStreamsReady(
            std::shared_ptr<oboe::AudioStream> inputStream,
            const void *inputData,
            int   numInputFrames,
            std::shared_ptr<oboe::AudioStream> outputStream,
            void *outputData,
            int   numOutputFrames) {
        // This code assumes the data format for both streams is Float.
        const float *inputFloats = static_cast<const float *>(inputData);
        float *outputFloats = static_cast<float *>(outputData);

        // It also assumes the channel count for each stream is the same.
        int32_t samplesPerFrame = outputStream->getChannelCount();
        int32_t numInputSamples = numInputFrames * samplesPerFrame;
        int32_t numOutputSamples = numOutputFrames * samplesPerFrame;

        // It is possible that there may be fewer input than output samples.
        int32_t samplesToProcess = std::min(numInputSamples, numOutputSamples);

//        float inSamples [samplesToProcess];
        for (int i = 0 ; i < samplesToProcess ; i ++) {
            raw [i] = inSamples [i] = inputConditioner.process (inputFloats [i]) * inputVolume ;
            tuner.push (inSamples [i]) ;
        }

//        if (meterEnabled) {
//            Meter::process (samplesToProcess, inSamples, true);
//        }
        // this
        // am I devloper yet?
//        memcpy(outputData, inputData, samplesToProcess);
        if (! bypass)
            process(inSamples, numInputSamples);

        /* this is not supposed to be called directly.
         * hence the entire vringbuffer stuff
         */

        if (pushToLockFreeBeforeOutputVolume && ! pause)
            lockFreeQueueManager->process(raw, inSamples, samplesToProcess) ;

        for (int32_t i = 0; i < samplesToProcess; i++) {
            inSamples [i] = inSamples [i]  * outputVolume; // do some arbitrary processing
            inSamples [i] = outputLimiter.process (inSamples [i]) ;
            *outputFloats++ = inSamples [i];
        }

//        if (meterEnabled) {
//            Meter::process (samplesToProcess, inSamples, false);
//        }

        if (! pushToLockFreeBeforeOutputVolume && ! pause)
            lockFreeQueueManager->process(raw, inSamples, samplesToProcess) ;

        //        for (int32_t i = 0; i < samplesToProcess; i++) {
//            *outputFloats++ = *inputFloats++  * outputVolume; // do some arbitrary processing
//        }

        // If there are fewer input samples then clear the rest of the buffer.
        int32_t samplesLeft = numOutputSamples - numInputSamples;
        for (int32_t i = 0; i < samplesLeft; i++) {
            *outputFloats++ = 0.0; // silence
        }

        //        OUT ;
        return oboe::DataCallbackResult::Continue;
    }

    void process (const float * data, int samplesToProcess) {
#define PROFILE_PLUGINS1
#ifdef PROFILE_PLUGINS
        auto start = std::chrono::system_clock::now();
        auto since_last = start ;
#endif
        for (int i = 0 ; i < activePlugins ; i ++) {
            if (inputPorts [i] != -1)
                connect_port [i] (handle [i], inputPorts [i], (LADSPA_Data *) data);
            if (outputPorts [i] != -1)
                connect_port [i] (handle [i], outputPorts [i], (LADSPA_Data *) data);

//            LOGD("I/O Ports %d %d", inputPorts [i], outputPorts [i]);
//            LOGD("[%d] samples", samplesToProcess);

            if (inputPorts2 [i] != -1)
                connect_port [i] (handle [i], inputPorts2 [i], (LADSPA_Data *) data);
            if (outputPorts2 [i] != -1)
                connect_port [i] (handle [i], outputPorts2 [i], (LADSPA_Data *) data);

//            LOGD("I/O 2 Ports %d %d", inputPorts2 [i], outputPorts2 [i]);

            if (run [i] == NULL)
                LOGF ("run %d is null", i);
            else
                run [i] (handle [i], samplesToProcess);
            /*
            if (set_run_adding_gain [i] != NULL)
                set_run_adding_gain [i] (handle [i], run_adding_gain [i]) ;
            if (run_adding [i] != NULL)
                run [i] (handle [i], samplesToProcess);
            */

#ifdef PROFILE_PLUGINS
            auto elapsed = std::chrono::system_clock::now() - since_last ;
            since_last = since_last + elapsed;
            LOGD("[%d: %d] %lld ms", i, samplesToProcess, elapsed.count ()) ;
#endif
        }

#ifdef PROFILE_PLUGINS
        auto elapsed = std::chrono::system_clock::now() - start ;
        LOGD("[%d] %lld ms", samplesToProcess, elapsed.count ()) ;
#endif

    }
};
#endif //SAMPLES_FULLDUPLEXPASS_H
