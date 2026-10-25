#!/bin/bash
# Rebuild wx + app for the given ABIs (default both), sign, and put the APKs in dist/.
source "$(dirname "$0")/env.sh"
mkdir -p $R/dist
for abi in ${@:-arm64-v8a x86_64}; do
  (cd $R/build/wx-$abi && nice -n 19 ninja -j$JOBS && ninja install) > $R/build/wx-$abi-rebuild.log 2>&1 || { echo "wx $abi FAILED"; exit 1; }
  $R/build-app.sh $abi android-app/orcaslicer_make_apk > $R/build/app-$abi-rebuild.log 2>&1 || { echo "app $abi FAILED"; grep -E "error:|FAILED" $R/build/app-$abi-rebuild.log | head -5; exit 1; }
  short=${abi/-v8a/}
  $BT/apksigner sign --ks $ORCA_KEYSTORE --ks-pass pass:android --key-pass pass:android \
     --out $R/dist/OrcaSlicer-android-$short-preview.apk $R/build/app-$abi/android-app/android-build/orcaslicer.apk || exit 1
  echo "published $abi"
done
