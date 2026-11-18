# AGENTS.md

Repository notes for AI agents working on ALTODFX.

## What this project is

ALTODFX is an Android guitar multi-effects app, forked from the
open-source "Tonesmith" app by Acoustix Audio. It is a single-module Gradle
project (`app`). The audio engine is native (`System.loadLibrary("amprack")`,
plus LADSPA/LV2 plugin support).

Audio/video recording was removed deliberately: there is no recorder UI, no
`FileWriter`/`Export`/`OpusWorkaround` native code, and no encoder libraries
(libsndfile, libopus, libopusenc, libmp3lame). The `RECORD_AUDIO` permission and
`setRecordingDeviceId()` remain because they belong to the guitar capture path,
not to file recording.

## Repository

- Remote: `https://github.com/kdsmedia/ALTODFX.git`
- **The repository is PUBLIC.** Everything committed here, including the
  signing keystore and its passwords, is world-readable. The owner has
  explicitly accepted this for the signing key (see "Signing" below); do not
  add any *other* secret.
- Default branch: `main`.

## Key facts / conventions

- Application ID and namespace: `com.altomedia.altodfx`
  (renamed from `org.acoustixaudio.tonesmith`).
- App name: `ALTODFX` (`app/src/main/res/values/strings.xml` -> `app_name`,
  `settings.gradle` -> `rootProject.name`).
- App background image: `app/src/main/res/drawable/altomedia.png`
  (referenced from `activity_main.xml` and used for the main screen).
- The launcher icon artwork is `Logo.png` at the repository root (a 1254x1254
  full-bleed image). All icons under `app/src/main/res/mipmap-*` and
  `app/src/main/ic_launcher-playstore.png` are generated from it; regenerate
  them rather than editing the per-density files by hand. Keep the adaptive
  foreground within the 66dp safe circle so the launcher mask never crops it.
- The app is **fully free and offline**: Google Play Billing was removed
  entirely (no `com.android.billingclient` dependency, no `Purchase` activity,
  no premium gating). The `INTERNET` and `BILLING` permissions were removed.
  Do not re-add billing or network access without an explicit request.

## Audio routing (headset jack only, never USB)

- Input and output are routed through the **wired headset jack (3.5 mm)** and
  are deliberately **never** routed through a USB audio device.
- `MainActivity.applySettings()` / `applyInputDevice()` / `applyOutputDevice()`
  resolve the device and call the native
  `AudioEngine.setRecordingDeviceId(int)` / `setPlaybackDeviceId(int)` using
  `AudioDeviceInfo.getId()`.
- `isUsbAudioDevice()` excludes `TYPE_USB_DEVICE`, `TYPE_USB_ACCESSORY`, and
  `TYPE_USB_HEADSET`. `findDevice()` never returns a USB device.
- Preference values for `input` and `output` are `auto`, `headset`, `phone`
  (see `arrays.xml`). `auto` prefers the headset jack and falls back to the
  built-in mic/speaker. `resolveInputDevice()`/`resolveOutputDevice()` return
  `-1` when nothing matches; the engine maps `-1` to the system default device.
  A device id of `0` is a valid device, so never use `0` as a "not found"
  sentinel (an earlier bug did, which silently skipped real devices).
- A `HeadsetReceiver` on `AudioManager.ACTION_HEADSET_PLUG` re-applies routing
  when the jack is plugged or unplugged.
- Note: on many phones the headset jack is output-only (no headset mic), so the
  headset option applies to output; the input follows it only when the phone
  exposes a wired-headset input (TRRS with mic).

## Noise / hum removal (input conditioning)

- The native engine applies a front-end conditioner to the raw input before the
  effect chain: a DC blocker + 1st-order high-pass (default 80 Hz) and a
  soft envelope-follower noise gate. This is what keeps a guitar into a
  headset-jack interface free of hiss and mains hum.
- Implemented in `app/src/main/cpp/FullDuplexPass.h` (`class InputConditioner`,
  used inside `onBothStreamsReady`). Sample rate is set from `Engine::openStreams`.
- Stream opening is **layered and must stay that way** (`Engine::openOutputStream`
  and `Engine::openInputStream`): Exclusive + LowLatency first, then Shared, then
  `PerformanceMode::None`. After the first failed attempt the explicit device id
  is dropped so later attempts use the system default. Forcing one combination
  (Exclusive + LowLatency + AAudio + Unprocessed) makes many devices refuse to
  open the stream, which shows up as "the app makes no sound at all".
- The audio API is `oboe::AudioApi::Unspecified` so Oboe picks AAudio when
  available and falls back to OpenSL ES. Do not force AAudio.
- `AudioEngine.setPackageName()` must be called from `MainActivity` before
  starting: input streams require it on Android 12+.
- Input preset depends on the device. `InputPreset::Unprocessed` is used for the
  built-in mic and USB so the guitar keeps its own character (no AGC or noise
  suppression). For a wired/USB headset-style input (iRig in the 3.5 mm jack)
  `InputPreset::VoiceRecognition` is tried first, because on many phones that is
  what opens the TRRS microphone; `MainActivity.isHeadsetInputDevice()` decides
  this via `AudioEngine.setHeadsetInput()`. Do **not** use `VoicePerformance` /
  `VoiceCommunication`: those enable AGC and noise suppression.
- The duplex scratch buffers (`FullDuplexPass::inSamples` / `raw`) must be
  allocated **before** the streams start, and their byte size must include
  `sizeof(float)`. Getting either wrong corrupts audio or the heap.
- `Engine::onErrorAfterClose` reopens and restarts both streams on
  `ErrorDisconnected` (device unplugged) under `mLock` with `try_lock`.
- Controls: `AudioEngine.setInputConditionerEnabled(boolean)`,
  `setNoiseGateThreshold(float)`, `setInputHighpass(float)`, driven from
  `MainActivity.applyInputConditioner()` using the `input_clean`, `noise_gate`
  and `input_highpass` preferences.

## Output limiter

- The effect chain can sum several gain stages and clip the DAC hard, which is
  heard as harsh buzzing. `class OutputLimiter` in `FullDuplexPass.h` is a
  soft-knee peak limiter applied to the final output in `onBothStreamsReady`
  (ceiling 0.95, knee at 0.7 x ceiling). Keep it last in the output path.
- The noise gate in `InputConditioner` uses hysteresis (opens above
  `gateThreshold`, only closes below half of it) plus a 50 ms hold. A single
  threshold chatters on a decaying note and chops the tail of every chord.

## Tuner

- The tuner captures the conditioned input in `class AudioTuner`
  (`FullDuplexPass.h`): a lock-free ring buffer the audio callback writes and
  the UI thread reads. It is independent of `Meter`, whose tuner path has
  mismatched JNI signatures and never worked.
- `detectPitch()` is an NSDF (McLeod) detector that picks the *first* prominent
  peak, not the global maximum: the global maximum is often a harmonic multiple
  and reads an octave too low. Validated against synthetic tones, harmonic-rich
  guitar-like waveforms and detuned notes.
- JNI: `AudioEngine.getTunerData()` returns `{frequencyHz, rms, cents}`;
  `frequencyHz == 0` means no note. `setTunerEnabled()` also gates the ring
  buffer so capture costs nothing when the tuner screen is closed.

## UI structure

- `activity_main.xml`: toolbar with the app name and the effect ON/OFF
  `ToggleButton` (`@id/preview`, styled by `drawable/power_button_bg.xml`)
  pinned to the **top-right**, a `FrameLayout` holding three screens, and a
  `BottomNavigationView` (`@menu/bottom_nav_menu`) with Home / Tuning / Setting.
- `content_main.xml` is the Home screen (rack + preset spinner). The Tuning
  screen is `fragment_tuner.xml` with the custom `TunerNeedle` gauge. The
  Setting screen hosts `SettingsActivity.SettingsFragment`.
- `MainActivity.showScreen()` switches screens and starts/stops the tuner
  polling loop; the effect engine must be running for the tuner to see input.

## Effect rack and presets

- The active rack is defined by `app/src/main/assets/voice.json` (plugin ID ->
  name + control metadata). The plugin ID encodes the library:
  `library = ID / 100`, `plugin = ID % 100`. Library indices 0..149 map to
  `R.array.ladspa_plugins`; index `150 + i` maps to `R.array.lv2_plugins[i]`.
- The rack currently has 20 slots (compressor, wah, drives, amp, cab, EQ,
  modulation, delay, reverb, normalizer, plus the in-tree Tube Drive).
  `MAX_PLUGINS` in `FullDuplexPass.h` is 32; keep the rack within that.
- `MainActivity.loadPlugins()` loads every slot but calls
  `AudioEngine.togglePlugin(index, false)` so the app starts with all effects
  bypassed. `Plugin::active` defaults to `true`; running all 20 in series stacks
  every gain stage and is heard as a loud buzz. The user enables what they need
  (or loads a preset, which sets `active` per slot).
- **Preset control arrays are positional**, in the same order as the native
  control vector. For LV2 plugins that order is the lexicographic order of the
  JSON port keys (nlohmann `json` uses `std::map`), *not* the file order. For
  LADSPA it is the port index order. Always derive the order from the JSON
  metadata; do not guess.
- Factory presets live in `app/src/main/assets/presets/` (one JSON file per
  preset, each with one entry per rack slot: `active`, `controls`, `name`).
  Every factory preset must have exactly one entry per rack slot with the
  correct control count, or `MainActivity.loadPreset` will mis-set controls.
- Per-effect quick presets live in `app/src/main/assets/effect_presets.json`,
  keyed by the same plugin ID as `voice.json`, with exactly three named presets
  each. Values are keyed by control *name* (not index) and must stay inside the
  `minimum`/`maximum` of that control in `voice.json`. `DataAdapter` renders one
  button per preset under the effect's controls and applies every listed control
  at once; controls missing from a preset are left untouched.
- Bundled effect plugins live in `app/src/main/jniLibs/<abi>/`. When adding an
  effect, copy the `.so` into **all four** ABI folders and make sure the LV2
  bundle metadata (`app/src/main/assets/lv2/<so>/<fragment>.json`, where
  `<fragment>` is the part after `#` in the plugin URI) exists, otherwise the
  plugin fails to load and shifts every preset index after it.

### Stereo engine

- The whole engine is **stereo end to end**. The output stream is opened with
  two channels; the effect chain carries left and right separately; the JNI
  meter and the input conditioner each have a per-channel instance.
- The **capture** stream is left at the device default (`ChannelCount::
  Unspecified`) on purpose. A guitar on the 3.5 mm jack is mono, and forcing a
  stereo capture stream makes many phones fail to open it, which silences the
  whole app. The callback upmixes a mono capture to L = R, so a mono input
  still feeds the stereo chain. Do not set the input channel count to `Stereo`.
- A **mono** rack plugin (one audio in / one audio out) is instantiated twice:
  `Plugin::handle` is the left instance, `Plugin::handle2` the right. Controls
  are shared (both instances point at the same control memory), so a slider
  moves both channels. `Plugin::createSecondHandle()` does this; the host calls
  it from `Engine::buildPluginChain()` for every plugin where `isMono()` is
  true. `setBuffer()` mirrors the impulse-response buffer to both instances.
- A **stereo** rack plugin (two audio in / two audio out, e.g. GxChorus-Stereo,
  GxDelay-Stereo, mda-*, rkrlv2-*) runs as a single instance: `inputPort` /
  `outputPort` are left, `inputPort2` / `outputPort2` are right.
- `FullDuplexPass::process()` decides per plugin: if `handle2` is null it is a
  stereo-native plugin and gets `inputPorts`/`outputPorts`/`inputPorts2`/
  `outputPorts2`; otherwise it is mono and `handle` processes left while
  `handle2` processes right.

### Adding an in-tree effect (built from C++ source)

New effects that ship as source live in `app/src/main/cpp/` and are compiled
into their own LADSPA library by `CMakeLists.txt` (see the `altodfx_tubedrive`
target). Steps:

1. Write the DSP as a header (e.g. `TubeDrive.h`), then a LADSPA wrapper `.cpp`
   that defines `extern "C" const LADSPA_Descriptor *ladspa_descriptor(unsigned
   long index)`. Keep the `LADSPA_Descriptor` initialiser field-for-field: an
   omitted `deactivate` slot silently shifts `cleanup` and crashes on teardown.
2. Add an `add_library(<name> SHARED <wrapper>.cpp)` target. Do **not** use
   `-Ofast` for DSP: fast-math folds `std::isfinite` away, so a NaN/Inf guard
   becomes dead code. Test finiteness via the bit pattern instead (see
   `TubeDrive::isFinite`). `-O3` is fine.
3. Pick a free library index in `R.array.ladspa_plugins`
   (`app/src/main/res/values/arrays.xml`). Index `i` gives plugin IDs
   `i*100 .. i*100+99`. Most of the 150 entries have no real `.so`; an entry
   with no binary in any ABI and no `voice.json`/`all_plugins.json` reference is
   safe to reuse. `libaltodfx_tubedrive.so` occupies index 9 (IDs 900-999),
   `libaltodfx_autowah.so` index 13 (IDs 1300-1399),
   `libaltodfx_distortion.so` index 14 (IDs 1400-1499),
   `libaltodfx_fuzz.so` index 19 (IDs 1900-1999) and
   `libaltodfx_muff.so` index 23 (IDs 2300-2399) and
   `libaltodfx_equalizer.so` index 43 (IDs 4300-4399) and
   `libaltodfx_phaser.so` index 54 (IDs 5400-5499) and
   `libaltodfx_flanger.so` index 58 (IDs 5800-5899) and
   `libaltodfx_chorus.so` index 68 (IDs 6800-6899) and
   `libaltodfx_tremolo.so` index 78 (IDs 7800-7899) and
   `libaltodfx_octaver.so` index 89 (IDs 8900-8999) and
   `libaltodfx_delay.so` index 109 (IDs 10900-10999) and
   `libaltodfx_echo.so` index 116 (IDs 11600-11699).
4. Add the slot to `app/src/main/assets/voice.json`. `index` on each control is
   the **LADSPA port number** (not the control's ordinal): the host matches it
   against `PluginControl::port` in `getActivePluginValueByIndex` /
   `setPluginControlByIndex`. List only the controls the UI should show.
5. Add an entry to `app/src/main/assets/effect_presets.json` (three presets,
   values keyed by control name) so the effect gets the quick-preset bar.
6. The host connects the **same buffer** to the input and output audio ports
   (`FullDuplexPass::process`), so `run()` must process in place. A plugin with
   one audio input and one audio output is treated as **mono**: the host
   instantiates it twice, once per channel, and each instance keeps its own
   state. Do not try to make such a plugin stereo internally; just declare one
   in / one out. A plugin with two audio inputs and two audio outputs is run as
   a single instance on the stereo pair. See "Stereo engine" below.
7. Appending a slot at the end of `voice.json` keeps the existing factory
   presets valid (they are positional but only cover the earlier slots). Do not
   insert in the middle without rewriting every file in `assets/presets/`.

## Version control

- **Every change must be pushed to `origin/main` as soon as it is committed.**
  The owner expects the remote to be current at all times; do not leave
  committed work sitting only in the local clone.
- Remote is `https://github.com/kdsmedia/ALTODFX.git`, branch `main`, public.
- Push credentials live outside the clone, never in it:
  `~/.altodfx_push_token` (chmod 600) holds a fine-grained/classic PAT with
  `repo` scope, and `~/.altodfx_git` is the matching git credential helper.
  `git config credential.helper` in this clone points at that helper, so a plain
  `git push origin main` works.
- The sandbox `GITHUB_TOKEN` is a read-only GitHub App installation token, so it
  can clone and fetch but not write; do not use it to push. If a push still fails
  with `403 Permission to kdsmedia/ALTODFX.git denied`, the PAT has been revoked
  or has expired: report the block instead of assuming the push succeeded, and
  never commit the token value itself (this repository is public).

## Signing (release)

- `app/build.gradle` reads signing values from `keystore.properties`
  (gradle `Properties`), keys: `storeFile`, `storePassword`, `keyAlias`,
  `keyPassword`.
- **The signing keystore `ALTOMEDIA.jks` and `keystore.properties` are
  committed to this repository at the owner's explicit request** (the
  `*.jks` / `keystore.properties` lines were removed from `.gitignore`).
  Because the repository is public, anyone can read the key and its passwords.
  Treat the key as compromised and rotate it before any real distribution.
- Keystore details: alias `kdsmedia`, validity 10000 days,
  DN `CN=ALTOMEDIA, OU=Developer, O=ALTOMEDIA, L=Karawang, ST=Jawa Barat, C=ID`.

## Build

- Standard Android Gradle build (`./gradlew assembleDebug`,
  `./gradlew assembleRelease bundleRelease`).
- The native engine source is vendored in this repository under
  `app/src/main/cpp/`, and `app/build.gradle` points CMake at
  `src/main/cpp/CMakeLists.txt`. (Upstream is
  <https://github.com/djshaji/amp-rack>; the local copy carries the fixes
  needed to build here, so no separate `amp-rack/` clone is required.)
- `AudioEngine.java` (`com.shajikhan.ladspa.amprack`) is vendored into
  `app/src/main/java/com/shajikhan/ladspa/amprack/` so the app module compiles
  without the `amp-rack` Gradle module. Keep it in sync with upstream.
- Build outputs are committed under `app/release/`:
  `app-release.apk` (signed) and `app-release.aab` (signed).
- Toolchain used: JDK 21, Android SDK platform 34, build-tools 34.0.0,
  NDK 26.1.10909125, CMake 3.22.1, AGP 8.3.0.

## Style

- Keep changes minimal and focused.
- Do not commit build outputs, caches, or secrets.

## Known pitfalls (verified)

- `getExternalFilesDir()` can return `null` (no external storage). Never chain
  `.getPath()` / `.listFiles()` on it directly; check for `null` first. All
  user-preset read/write/delete paths in `MainActivity` guard on `presetsDir`.
- `Material Slider` validates its range lazily on the next layout pass, but
  `setValueFrom` / `setValueTo` must be called **before** `setValue`, and the
  value must lie inside `[valueFrom, valueTo]`. Setting a value outside the
  current range throws `IllegalStateException` at layout time.
- The per-effect preset bar is hidden by default and only shown when presets
  were bound successfully, so an effect without an entry shows no empty strip.
- `voice.json` control lists are a **curated subset** of the native control
  vector (BYPASS and some other ports are omitted), so the slider count for an
  effect is smaller than the port count used by `AudioEngine.setPluginControl`.
  Factory preset `controls` arrays are written against the **full** native
  vector (see `presets/*`), which is why their length differs from
  `voice.json`'s control count; that is correct and not a bug.
- `app/src/main/assets/lv2/mda-mda.so/mda.json` is malformed on purpose: it is
  a raw fragment without the enclosing `{ }`. Nothing loads it (mda is not in
  `voice.json` or the LV2 library array), so leave it alone.
