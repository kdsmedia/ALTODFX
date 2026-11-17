# ALTODFX

ALTODFX is a **guitar multi-effects processor** for Android. Plug your guitar
(or an iRIG-style interface) into the phone's 3.5 mm headset jack and play
through a chain of amps, cabinets and stompboxes.

Audio input and output both run through the **single 3.5 mm headset jack** and
are never routed through USB, so interfaces such as iRIG work as intended.

## Effects

ALTODFX ships a full, professional effect chain. Plugins are grouped by role
and are all available in the rack:

- **Noise gate / denoiser** - kills hiss, hum and mains buzz before anything else
- **High-pass filter** - removes rumble and low-frequency mud
- **Compressor** - adjusts the dynamic range of the input
- **Overdrive / distortion / fuzz** - from warm crunch to screaming high gain
- **Amplifier + cabinet** - amp and speaker simulation with many models
- **EQ** - graphic and parametric tone shaping
- **Modulation** - chorus, flanger, phaser, tremolo, vibrato, auto-wah
- **Pitch** - octaver / pitch shift / harmonizer
- **Delay / echo** - from slapback to long ambient repeats
- **Reverb** - spring, hall, room and plate style reverbs
- **Limiter / maximizer** - keeps the output loud without clipping
- **Normalizer** - compressor / limiter for a consistent level

## Presets

ALTODFX includes factory presets covering every genre, plus room to save your
own:

Clean, Blues, Blues Drive, Jazz Clean, Country Twang, Funk Wah, Surf Reverb,
Rock Rhythm, Rock Lead, Classic Rock, Hard Rock, Heavy Metal, Metal Rhythm,
Metal Lead, Punk, Grunge, Progressive, Shred Lead, Ambient, Dreamy, Stadium
Rock, Slapback, Ringing Echo, Acoustic Simulator, Bass, Vox / Voice.

Presets are loaded from `app/src/main/assets/presets/`. User presets are saved
to the app's documents folder and appear in the same list.

## Features

- **3.5 mm headset jack input and output** (iRIG-compatible), USB audio is
  deliberately excluded
- Floating point precision for high-quality audio processing
- Ultra-low latency for a lag-free playing experience
- Built-in noise gate and high-pass stage to keep the signal clean and free of
  hum and hiss
- Low memory footprint, great performance even on low-end devices
- No ads
- No unnecessary permissions required
- Completely free and offline

## Building

```
./gradlew assembleRelease bundleRelease
```

The native audio engine sources are vendored in `app/src/main/cpp` and the
prebuilt static libraries live in `app/src/main/jniLibs/<abi>`, so the project
builds on its own without any external checkout.

Made with love
