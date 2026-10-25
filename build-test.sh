#!/bin/bash
# Build wx + app for the given ABIs (default x86_64 arm64-v8a) one at a time at JOBS (default 2), sign, and put the
# APKs in dist-test/ (served on port 8013 for testing; dist/ is the published preview). Each heavy step waits until
# the shared machine has memory to spare: it crashed from overload once.
source "$(dirname "$0")/env.sh"
mkdir -p $R/dist-test
waitmem() { while [ "$(awk '/MemAvailable/ {print int($2/1048576)}' /proc/meminfo)" -lt ${MIN_FREE_GB:-6} ]; do sleep 20; done; }
for abi in ${@:-x86_64 arm64-v8a}; do
  waitmem
  (cd $R/build/wx-$abi && nice -n 19 ninja -j$JOBS && ninja install) > $R/build/wx-$abi-rebuild.log 2>&1 || { echo "wx $abi FAILED"; exit 1; }
  waitmem
  $R/build-app.sh $abi android-app/orcaslicer_make_apk > $R/build/app-$abi-rebuild.log 2>&1 || { echo "app $abi FAILED"; grep -E "error:|FAILED" $R/build/app-$abi-rebuild.log | head -5; exit 1; }
  short=${abi/-v8a/}
  $BT/apksigner sign --ks $ORCA_KEYSTORE --ks-pass pass:android --key-pass pass:android \
     --out $R/dist-test/OrcaSlicer-android-$short-test.apk $R/build/app-$abi/android-app/android-build/orcaslicer.apk || exit 1
  rm -f $R/dist-test/*.idsig
  echo "built $abi"
done
