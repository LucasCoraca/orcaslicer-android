#!/bin/bash
# Configure the engine-only (libslic3r, SLIC3R_GUI=OFF) Android build. usage: configure-engine.sh [arm64-v8a]
ABI=${1:-arm64-v8a}
source "$(dirname "$0")/env.sh"
P=$R/deps-android/$ABI/usr/local
QT=$(qt_for $ABI)
cmake -S $R/orcaslicer-src -B $R/build/engine-$ABI -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-28 -DANDROID_STL=c++_shared \
  -DCMAKE_PREFIX_PATH="$P;$QT" -DCMAKE_FIND_ROOT_PATH="$P;$QT" -DQT_HOST_PATH=$QT_HOST_PATH \
  -DOpenCV_DIR=$P/sdk/native/jni/abi-$ABI -DORCA_DEPS_PREFIX=$P -DSLIC3R_GUI=${SLIC3R_GUI:-OFF} -DSLIC3R_STATIC=ON -DSLIC3R_PCH=OFF -DSLIC3R_CAD=ON \
  -DSLIC3R_ENC_CHECK=OFF -DBUILD_TESTS=OFF -DORCA_TOOLS=OFF -DORCA_BUILD_PYTHON_STUBGEN_MODULE=OFF \
  "${@:2}"
