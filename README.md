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

This is not a rewrite or a "mobile companion". It is OrcaSlicer itself, compiled for Android:

- **Same engine.** `libslic3r` is built unchanged from upstream, so you get the same slicing, the same profiles and the same G-code as the desktop version.
- **Same interface.** Orca's own wxWidgets GUI (`src/slic3r/GUI`, about 400k lines) runs on Android through wxWidgets' Qt backend, plus a set of Android patches. You get Prepare, Preview, the 3D gizmos, every settings tab, the printer and filament presets, and calibration.
- **Made for touch.** Tap, drag and long-press (right click) work on the 3D canvas. Two fingers pinch to zoom, move to pan and twist to rotate. Lists scroll with kinetic finger scrolling, and the Android back button closes dialogs.
- **Phones and tablets.** Tablets get the desktop layout. On phones (narrower than 600 dp) the settings go full screen behind a "3D view" button, the gizmos move to a vertical column, and the top bar and tab bar become compact.
- **Android integration.** Files open and save through the system file picker (Storage Access Framework), so Google Drive and other providers work. Web pages (Home, the setup wizard) use the system WebView, the 3D view renders with OpenGL ES 3, and the light or dark theme follows the system.

Demo videos: [tablet](docs/assets/demo-tablet.mp4) · [phone](docs/assets/demo-phone.mp4)

## Install

1. Download `OrcaSlicer-android-arm64-preview.apk` from the [latest release](https://github.com/LucasCoraca/orcaslicer-android/releases/latest). Almost every phone and tablet since 2017 is arm64. The `x86_64` APK is for emulators and Chromebooks.
2. Allow installing apps from your browser or file manager when Android asks.
3. Open **Orca Slicer**. The first start unpacks the profiles and takes a few seconds.

Requires Android 9 (API 28) or newer and OpenGL ES 3.0. A device with 6 GB of RAM or more is recommended for large models.

## Status

Working, and tested on a phone and a tablet emulator:

- Importing STL files through the system file picker.
- Handy models and primitives.
- Selecting, and the move, rotate and scale gizmos.
- The long-press context menus.
- Every print, filament and printer setting, and presets.
- Slicing, with the legend and layer sliders.
- The setup wizard and the Home page.
- Light and dark themes.

Saving and exporting G-code goes through the same file picker, and is still being tested.

Known limitations:

- Bambu Lab cloud printing and the network plugin are not available. The plugin is a closed-source desktop binary.
- The printer camera view is not available yet.
- On emulators with a software GPU (SwiftShader/ANGLE), the Preview toolpaths render black once the G-code has loaded. Reports from real devices are welcome.
- A few web-based dialogs, such as printer selection, are not yet fully adapted to narrow phone screens.
- Large models are slower than on a desktop. Slicing runs on all CPU cores, but phones have less memory and thermal headroom.

Bug reports with your device model and Android version are very welcome.

## How it works

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
./build-all.sh arm64-v8a           # -> dist/OrcaSlicer-android-arm64-preview.apk
```

`configure-engine.sh` and `build-engine.sh` build only `libslic3r`, without the GUI. That is useful for checking engine changes quickly.

## License

[GNU AGPL-3.0](LICENSE), the same as OrcaSlicer. This port stands on the work of OrcaSlicer, Bambu Studio, PrusaSlicer and Slic3r, and builds on wxWidgets and Qt.
