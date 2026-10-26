<p align="center">
  <img src="docs/assets/icon.png" width="96" alt="">
</p>

<h1 align="center">OrcaSlicer for Android</h1>

<p align="center">
  The full OrcaSlicer desktop app (same slicing engine, same interface) running natively on Android phones and tablets.
</p>

<p align="center">
  <a href="https://lucascoraca.github.io/orcaslicer-android/"><b>Website</b></a> ·
  <a href="https://github.com/LucasCoraca/orcaslicer-android/releases/latest"><b>Download APK</b></a> ·
  <a href="#building-from-source"><b>Build from source</b></a>
</p>

<p align="center">
  <img src="docs/assets/screens/dark/tablet-prepare.jpg" width="72%" alt="OrcaSlicer on an Android tablet: a 3DBenchy on the build plate">
  &nbsp;
  <img src="docs/assets/screens/light/phone-prepare.jpg" width="20%" alt="OrcaSlicer on an Android phone">
</p>

> **Unofficial community port, preview quality.** This project is not affiliated with or endorsed by the OrcaSlicer team.
> Upstream OrcaSlicer lives at [OrcaSlicer/OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer).

## What this is

This is not a "mobile companion". It is OrcaSlicer's own code, compiled for Android:

- **Same engine.** `libslic3r` is built unchanged from upstream, so you get the same slicing, the same profiles and the same G-code as the desktop version.
- **Same features, native UI.** Since 0.2.0 the interface is a native Android app (Jetpack Compose) on top of `liborcacore`, an engine library that compiles the desktop GUI's own logic from `src/slic3r/GUI` (plates, gizmos, painting, settings tabs, presets, calibration, the G-code viewer, devices) against small stand-ins for the wx headers. Every desktop dialog and gizmo has a touch counterpart: Prepare, Preview, all gizmos (cut, measure, text, SVG, mesh boolean, painting, brim ears, texture displacement), every settings page, the setup wizard, calibration and printer connections. The 0.1.x releases ran the wxWidgets GUI itself through wxWidgets' Qt backend.
- **Made for touch.** Tap, drag and long-press (right click) work on the 3D canvas. Two fingers pinch to zoom, move to pan and twist to rotate. Lists scroll with kinetic finger scrolling, and the Android back button closes dialogs.
- **Phones and tablets.** Tablets get the desktop layout. On phones (narrower than 600 dp) the settings go full screen behind a "3D view" button, the gizmos move to a vertical column, and the top bar and tab bar become compact.
- **Android integration.** Files open and save through the system file picker (Storage Access Framework), so Google Drive and other providers work. Web pages (Home, the setup wizard) use the system WebView, the 3D view renders with OpenGL ES 3, and the light or dark theme follows the system.

Demo videos: [tablet](docs/assets/demo-tablet.mp4) · [phone](docs/assets/demo-phone.mp4)

In a headset: [OrcaSlicer XR](https://github.com/LucasCoraca/orcaslicer-xr) is this app on Meta Quest 3, with Orca's window
in front of you and the plate in the room.

## Install

1. Download `OrcaSlicer-android-arm64-preview.apk` from the [latest release](https://github.com/LucasCoraca/orcaslicer-android/releases/latest). Almost every phone and tablet since 2017 is arm64. The `x86_64` APK is for emulators and Chromebooks.
2. Allow installing apps from your browser or file manager when Android asks.
3. Open **Orca Slicer**. The first start unpacks the profiles and takes a few seconds.

Requires Android 9 (API 28) or newer and OpenGL ES 3.0. A device with 6 GB of RAM or more is recommended for large models.

## Status

Tested feature by feature on a tablet emulator against the desktop app: import (STL, OBJ with textures, 3MF, STEP, AMF, ZIP), plates and plate settings, all gizmos, painting, every setting and preset operation, slicing with cancel and slice all, the Preview legend and sliders, G-code / 3MF / STL export, project save and backup restore, the setup wizard, calibration, printer connections and translations. The arm64 build has been run through the emulator's ARM translation; reports from real devices are welcome.

Known limitations:

- Bambu Lab network printing is not available. It needs Bambu's closed-source network plug-in, which has no Android build.
- Large models are slower than on a desktop. Slicing runs on all CPU cores, but phones have less memory and thermal headroom.

Bug reports with your device model and Android version are very welcome.

## How it works

0.2.0 and later: [`mobile/app`](mobile/app) is the Android app (Kotlin, Jetpack Compose, OpenGL ES 3 view) and
[`mobile/core`](mobile/core) builds `liborcacore.so`: libslic3r plus upstream GUI sources and functions extracted from
them at build time ([`mobile/core/tools`](mobile/core/tools)), with stand-ins for the wx classes they use. The diagram
below is the 0.1.x design, whose patches are still used for the dependency build.

```
 ┌──────────────────────────────────────────────────────────────┐
 │  OrcaSlicer GUI  (src/slic3r/GUI, unmodified apart from a few │
 │  #ifdef __ANDROID__ layout and GLES tweaks)                   │
 ├───────────────────────────────┬──────────────────────────────┤
 │  wxWidgets 3.3 (Orca fork)    │  libslic3r (slicing engine,  │
 │  wxQt backend + Android       │  unchanged)                  │
 │  patches: touch, DPI, SAF,    │                              │
 │  WebView, GL canvases         │                              │
 ├───────────────────────────────┤  Boost, TBB, CGAL, OCCT,     │
 │  Qt 6.8 Widgets for Android   │  OpenVDB … cross-compiled    │
 ├───────────────────────────────┴──────────────────────────────┤
 │  Android: OpenGL ES 3 / EGL, system WebView, Storage Access   │
 │  Framework, NDK 29, API 28+, arm64-v8a and x86_64             │
 └──────────────────────────────────────────────────────────────┘
```

All changes to upstream are kept as two patches:

| Patch | Applies to | Contents |
|---|---|---|
| [`patches/orcaslicer-android.patch`](patches/orcaslicer-android.patch) | [OrcaSlicer](https://github.com/SoftFever/OrcaSlicer) `1ee0b730` | Android dependency superbuild; GLSL ES shaders and a GLES loader; GLES render paths; the compact phone layout; Android startup (resource unpacking, CA certificates) |
| [`patches/wxWidgets-android.patch`](patches/wxWidgets-android.patch) | [Orca-deps-wxWidgets](https://github.com/SoftFever/Orca-deps-wxWidgets) `88f3483` | wxQt fixes for Android: DPI-correct painting; full-screen frames and on-screen dialogs; touch to mouse and gesture events; kinetic scrolling; native file picker with `content://` support; WebView and Keystore bridges in Java |

The Android app wrapper (entry point, resource packing and manifest) is in [`android/app`](android/app).

## Building from source

Host: Linux x86_64 with about 60 GB free disk space. A full build takes a while, so use as many cores as you have.

**Prerequisites**

- Android SDK with NDK `29.0.14206865`, build-tools `34.0.0`, platform `android-34` and CMake `3.22.1`.
- JDK 17.
- Qt 6.8.3 for Android (`android_arm64_v8a` and/or `android_x86_64`) plus the matching host Qt. The Qt online installer provides both.
- CMake ≥ 3.28 and Ninja on `PATH`. The scripts look in `hosttools/pyenv/bin` first; `python3 -m venv hosttools/pyenv && hosttools/pyenv/bin/pip install cmake ninja` sets that up.
- GNU m4, for the GMP/MPFR autotools builds.
- CPython 3.14 built for Android, placed in `deps-android/<abi>/usr/local/libpython`. Orca's plugin system needs it.

Paths are read from the environment; see [`env.sh`](env.sh):

```sh
export ANDROID_SDK_ROOT=~/Android/sdk
export QT_ANDROID=~/Qt/6.8.3/android_arm64_v8a   # or QT_ANDROID_ROOT=<dir with arm64-v8a/ x86_64/ qt-host/>
export QT_HOST_PATH=~/Qt/6.8.3/gcc_64
export JAVA_HOME=~/jdk-17
export ORCA_KEYSTORE=~/.android/debug.keystore    # password "android"
```

**Steps**

```sh
./fetch-sources.sh                 # pinned OrcaSlicer + wxWidgets checkouts, patches applied
./configure-deps.sh arm64-v8a && ./run-deps.sh arm64     # Boost, TBB, CGAL, OCCT, OpenVDB, …
./configure-wx.sh arm64-v8a && ninja -C build/wx-arm64-v8a install
./configure-app.sh arm64-v8a
./build-all.sh arm64-v8a           # -> dist/OrcaSlicer-android-arm64-preview.apk (the 0.1.x wx app)
BUILD_TYPE=release mobile/build-mobile.sh arm64-v8a   # the native app (0.2+): liborcacore + APK
```

`configure-engine.sh` and `build-engine.sh` build only `libslic3r`, without the GUI. That is useful for checking engine changes quickly.

## License

[GNU AGPL-3.0](LICENSE), the same as OrcaSlicer. This port stands on the work of OrcaSlicer, Bambu Studio, PrusaSlicer and Slic3r, and builds on wxWidgets and Qt.
