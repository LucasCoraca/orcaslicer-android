#!/bin/bash
# Configure Orca's dependency superbuild for Android. usage: configure-deps.sh [arm64-v8a|x86_64]
# Trees: build/deps-<arm64|x86_64>, prefix deps-android/<abi>/usr/local, downloads in dl-cache/.
ABI=${1:-arm64-v8a}
source "$(dirname "$0")/env.sh"
SHORT=${ABI/arm64-v8a/arm64}
export PATH=$ANDROID_SDK_ROOT/cmake/3.22.1/bin:$PATH
cmake -S $R/orcaslicer-src/deps -B $R/build/deps-$SHORT -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-28 -DANDROID_STL=c++_shared \
  -DDESTDIR=$R/deps-android/$ABI -DDEP_DOWNLOAD_DIR=$R/dl-cache
