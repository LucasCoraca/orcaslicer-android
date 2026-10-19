#!/bin/bash
# usage: build-app.sh [abi] [targets...]   (default target: orcaslicer; the APK via androiddeployqt: orcaslicer_make_apk)
ABI=${1:-x86_64}; shift
source "$(dirname "$0")/env.sh"
cd $R/build/app-$ABI && exec nice -n 10 ninja -j$JOBS -k 0 "${@:-orcaslicer}"
