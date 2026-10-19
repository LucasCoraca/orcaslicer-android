#!/bin/bash
# Check out the pinned upstream sources and apply the Android patches:
#   orcaslicer-src/          SoftFever/OrcaSlicer
#   third_party/wxWidgets/   SoftFever/Orca-deps-wxWidgets (Orca's wxWidgets 3.3 fork)
set -e
cd "$(dirname "$0")"
ORCA_COMMIT=1ee0b730a1e6484e55a631dd567b41c986fc1d6a
WX_COMMIT=88f3483ca546fbf4ad732e1acd94cc930935077a
checkout() {  # <url> <dir> <commit> <patch>
  if [ ! -d "$2/.git" ]; then
    git init -q "$2"; git -C "$2" remote add origin "$1"
  fi
  git -C "$2" fetch -q --depth 1 origin "$3"
  git -C "$2" checkout -q --force "$3"
  git -C "$2" submodule update -q --init --recursive --depth 1 || true
  git -C "$2" apply --index "$PWD/$4"
  echo "$2: $(git -C "$2" log -1 --format='%h %s') + $(basename "$4")"
}
checkout https://github.com/SoftFever/OrcaSlicer.git orcaslicer-src $ORCA_COMMIT patches/orcaslicer-android.patch
checkout https://github.com/SoftFever/Orca-deps-wxWidgets.git third_party/wxWidgets $WX_COMMIT patches/wxWidgets-android.patch
