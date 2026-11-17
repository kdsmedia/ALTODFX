# AGENTS.md

Repository notes for AI agents working on ALTODFX.

## What this project is

ALTODFX is an Android guitar multi-effects / voice-recorder app, forked from the
open-source "Tonesmith" app by Acoustix Audio. It is a single-module Gradle
project (`app`). The audio engine is native (`System.loadLibrary("amprack")`,
plus LADSPA/LV2 plugin support).

## Repository

- Remote: `https://github.com/kdsmedia/ALTODFX.git`
- **The repository is PUBLIC.** Never commit secrets.
- Default branch: `main`.

## Key facts / conventions

- Application ID and namespace: `com.altomedia.altodfx`
  (renamed from `org.acoustixaudio.tonesmith`).
- App name: `ALTODFX` (`app/src/main/res/values/strings.xml` -> `app_name`,
  `settings.gradle` -> `rootProject.name`).
- App background image: `app/src/main/res/drawable/altomedia.png`
  (referenced from `activity_main.xml` and used for the main screen).
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
  built-in mic/speaker. A device id of `0` means "leave the engine default".
- A `HeadsetReceiver` on `AudioManager.ACTION_HEADSET_PLUG` re-applies routing
  when the jack is plugged or unplugged.
- Note: on many phones the headset jack is output-only (no headset mic), so the
  headset option applies to output; the input follows it only when the phone
  exposes a wired-headset input (TRRS with mic).

## Signing (release)

- `app/build.gradle` reads signing values from `keystore.properties`
  (gradle `Properties`), keys: `storeFile`, `storePassword`, `keyAlias`,
  `keyPassword`.
- `keystore.properties` and `*.jks` are git-ignored. The keystore file
  `ALTOMEDIA.jks` must stay out of version control.
- The keystore file is kept only in the local workspace (not in git). If a
  fresh clone needs to build a release, the keystore and
  `keystore.properties` must be supplied out of band.

## Build

- Standard Android Gradle build (`./gradlew assembleDebug`).
- Note: a full build may fail because the native/CMake dependency `amp-rack`
  is referenced from outside the repository and is not present here.

## Style

- Keep changes minimal and focused.
- Do not commit build outputs, caches, or secrets.
