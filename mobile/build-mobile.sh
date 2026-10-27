#!/bin/bash
# Build the native mobile app: liborcacore.so (engine build dir with ORCA_MOBILE_CORE=ON), the generated assets, the APK.
# usage: mobile/build-mobile.sh [abi...]      (default x86_64; BUILD_TYPE=release for a release APK)
#   SKIP_CORE=1   reuse the last liborcacore.so      SKIP_ASSETS=1   reuse the staged assets
#   GRADLE_OFFLINE=1   no network for Gradle (its cache must hold the dependencies)
set -e
M="$(cd "$(dirname "$0")" && pwd)"
source "$M/../env.sh"
ABIS=${@:-x86_64}
BUILD_TYPE=${BUILD_TYPE:-debug}
ORCA=$R/orcaslicer-src
STAGE=$M/app/app/build/staging
mkdir -p $STAGE/assets $R/build/logs

for ABI in $ABIS; do
  B=$R/build/engine-$ABI
  if [ -z "$SKIP_CORE" ]; then
    if ! grep -q "ORCA_MOBILE_CORE:BOOL=ON" $B/CMakeCache.txt 2>/dev/null; then
      $R/configure-engine.sh $ABI -DORCA_MOBILE_CORE=ON
    fi
    (cd $B && nice -n 19 ninja -j$JOBS orcacore python3_exe)
  fi
  mkdir -p $STAGE/jni/$ABI
  TRIPLE=$([ $ABI = arm64-v8a ] && echo aarch64-linux-android || echo x86_64-linux-android)
  # stripped copy: the unstripped library stays in the build dir for ndk-stack
  $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip --strip-unneeded -o $STAGE/jni/$ABI/liborcacore.so $B/mobile-core/liborcacore.so
  cp $NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/$TRIPLE/libc++_shared.so $STAGE/jni/$ABI/
  # the plugin terminal's python (an executable named as a library: Android installs it runnable)
  $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip --strip-unneeded -o $STAGE/jni/$ABI/libpython3_exe.so $B/mobile-core/libpython3_exe.so
  # CPython for the plugin system: the interpreter and the libraries its modules use as native libraries, the
  # extension modules (lib-dynload) as an asset of the ABI's APK, unpacked next to the stdlib at the first start
  PY=$R/deps-android/$ABI/usr/local/libpython/lib
  cp $PY/libpython3.14.so $PY/libcrypto_python.so $PY/libssl_python.so $PY/libsqlite3_python.so $STAGE/jni/$ABI/
  mkdir -p $STAGE/assets-$ABI && rm -f $STAGE/assets-$ABI/python-dynload.zip
  (cd $PY/python3.14 && zip -q -r $STAGE/assets-$ABI/python-dynload.zip lib-dynload)
done

if [ -z "$SKIP_ASSETS" ]; then
  # Orca's resources plus the translation catalogs (read at run time by mobile/core/src/I18n.cpp)
  RES=$R/build/mobile-resources
  rm -rf $RES && mkdir -p $RES && cp -a $ORCA/resources/. $RES/
  for d in $ORCA/localization/i18n/*/; do
    l=$(basename $d); [ -f $d/OrcaSlicer_$l.po ] && mkdir -p $RES/i18n/$l && cp $d/OrcaSlicer_$l.po $RES/i18n/$l/
  done
  # the Python stdlib of the plugin system (PythonInterpreter: <resources>/python/lib/python3.14), without the tests,
  # the Tk GUI and pip; lib/python3.12/encodings marks the home as upstream's find_bundled_python_home looks for it
  PYSTD=$R/deps-android/x86_64/usr/local/libpython/lib/python3.14
  mkdir -p $RES/python/lib/python3.14 $RES/python/lib/python3.12/encodings
  (cd $PYSTD && tar cf - --exclude=./test --exclude=./idlelib --exclude=./tkinter --exclude=./turtledemo --exclude=./ensurepip \
      --exclude='./config-3.14*' --exclude=./lib-dynload --exclude=__pycache__ .) | (cd $RES/python/lib/python3.14 && tar xf -)
  echo "the stdlib is in ../python3.14" > $RES/python/lib/python3.12/encodings/README
  python3 $R/android/app/make_resources_zip.py $RES $STAGE/assets/resources.zip
  python3 $M/tools/gen_settings_layout.py $ORCA $STAGE/assets/settings_layout.json $M/layout/layout_overrides.json
fi

FLAVORS=""
for ABI in $ABIS; do FLAVORS="$FLAVORS $([ $ABI = arm64-v8a ] && echo arm64 || echo x86)"; done
cd $M/app
for F in $FLAVORS; do
  T=assemble${F^}${BUILD_TYPE^}
  nice -n 19 ./gradlew ${GRADLE_OFFLINE:+--offline} -q $T
done
find $M/app/app/build/outputs/apk -name "*.apk" -newer $STAGE/assets/settings_layout.json -printf "%p %s\n"
