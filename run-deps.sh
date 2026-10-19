#!/bin/bash
# Build the dependency superbuild configured by configure-deps.sh. usage: run-deps.sh <arm64|x86_64>
source "$(dirname "$0")/env.sh"
export PATH=$ANDROID_SDK_ROOT/cmake/3.22.1/bin:$PATH CMAKE_BUILD_PARALLEL_LEVEL=$JOBS
cd $R/build/deps-$1 && exec nice -n 10 ninja -j1 -k 0 deps
