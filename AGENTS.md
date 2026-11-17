# AGENTS.md

Repository notes for AI agents working on ALTODFX.

## What this project is

ALTODFX is an Android guitar multi-effects / voice-recorder app, forked from the
open-source "Tonesmith" app by Acoustix Audio. It is a single-module Gradle
project (`app`). The audio engine is native (`System.loadLibrary("amprack")`,
plus LADSPA/LV2 plugin support).

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
- The native engine source lives outside this repository: `app/build.gradle`
  points CMake at `../../amp-rack/app/src/main/cpp/CMakeLists.txt`
  (upstream <https://github.com/djshaji/amp-rack>). To build, clone that repo
  next to this one as `amp-rack/`, then apply two local fixes to the clone:
  add `include_directories(${CMAKE_CURRENT_SOURCE_DIR})` after
  `project("amprack")`, and add `#include <vector>` + `#include <algorithm>`
  to `app/src/main/cpp/lv2_ext.cpp`.
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
