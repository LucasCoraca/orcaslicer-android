# Shared settings for the build scripts (sourced, not run). Every path can be overridden from the environment.
#   ANDROID_SDK_ROOT  Android SDK (with NDK 29.0.14206865 and build-tools 34.0.0)
#   QT_ANDROID_ROOT   folder holding Qt 6.8.3 for Android per ABI: <root>/arm64-v8a, <root>/x86_64 and the host Qt in <root>/qt-host
#                     (or set QT_ANDROID / QT_HOST_PATH directly, e.g. ~/Qt/6.8.3/android_arm64_v8a and ~/Qt/6.8.3/gcc_64)
#   JAVA_HOME         JDK 17
#   ORCA_KEYSTORE     keystore used to sign the APK (password "android")
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export ANDROID_SDK_ROOT=${ANDROID_SDK_ROOT:-$HOME/Android/sdk}
NDK=${ANDROID_NDK:-$ANDROID_SDK_ROOT/ndk/29.0.14206865}
BT=$ANDROID_SDK_ROOT/build-tools/34.0.0
QT_ANDROID_ROOT=${QT_ANDROID_ROOT:-$HOME/Android/fcdeps}
QT_HOST_PATH=${QT_HOST_PATH:-$QT_ANDROID_ROOT/qt-host}
export JAVA_HOME=${JAVA_HOME:-$HOME/Android/jdk17}
export ORCA_KEYSTORE=${ORCA_KEYSTORE:-$R/hosttools/debug.keystore}
JOBS=${JOBS:-2}   # shared machine: it crashed from overload at higher -j
# private host tools: cmake >= 3.28 and ninja in a Python venv (pip install cmake ninja), GNU m4 for the autotools deps
export PATH=$R/hosttools/pyenv/bin:$R/hosttools/bin:$JAVA_HOME/bin:$PATH

# qt_for <abi>: the Qt for Android prefix of that ABI
qt_for() { echo "${QT_ANDROID:-$QT_ANDROID_ROOT/$1}"; }
