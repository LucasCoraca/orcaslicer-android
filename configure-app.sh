#!/bin/bash
# Configure the packaged Android app (GUI on, Qt toolchain). usage: configure-app.sh [x86_64|arm64-v8a]
ABI=${1:-x86_64}
source "$(dirname "$0")/env.sh"
QT=$(qt_for $ABI)
P=$R/deps-android/$ABI/usr/local
cmake -S $R/orcaslicer-src -B $R/build/app-$ABI -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$QT/lib/cmake/Qt6/qt.toolchain.cmake -DQT_HOST_PATH=$QT_HOST_PATH \
  -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-28 \
  -DCMAKE_PREFIX_PATH="$P;$QT" -DCMAKE_FIND_ROOT_PATH="$P;$QT" \
  -DwxWidgets_DIR=$P/lib/cmake/wxWidgets-3.3 -DBoost_DIR=$P/lib/cmake/Boost-1.84.0 -DEigen3_DIR=$P/share/eigen3/cmake -Dfreetype_DIR=$P/lib/cmake/freetype -DOpenCV_DIR=$P/sdk/native/jni/abi-$ABI \
  -DORCA_DEPS_PREFIX=$P -DSLIC3R_GUI=ON -DSLIC3R_STATIC=ON -DSLIC3R_PCH=ON -DSLIC3R_CAD=ON -DSLIC3R_ENC_CHECK=OFF \
  -DBUILD_TESTS=OFF -DORCA_TOOLS=OFF -DORCA_BUILD_PYTHON_STUBGEN_MODULE=OFF -DORCA_ANDROID_APP=ON \
  "${@:2}"
