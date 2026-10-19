#!/bin/bash
# usage: build-engine.sh [abi] [targets...]
ABI=${1:-arm64-v8a}; shift
source "$(dirname "$0")/env.sh"
cd $R/build/engine-$ABI && exec nice -n 10 ninja -j$JOBS -k 0 "${@:-libslic3r}"
