#!/bin/bash
# Run one of our test AVDs in a container (the dev server's user is not in the kvm group, but docker can pass /dev/kvm).
# usage: ./start-docker-emulator.sh [avd] [port]     default: orcadock (2560x1600 tablet, 2.5 GB) on 5584 => emulator-5584
# The server is itself a KVM guest: the nested boot takes ~6 min and stalls the vCPUs at first; without
# ANDROID_EMU_ENABLE_CRASH_REPORTING=0 the emulator's hang detector kills it (exit 139).
#   ORCA_GPU=swiftshader_indirect (default)   ORCA_EMU_MEM=5g container memory cap (the host has 9 GB)   ORCA_EMU_RAM=2560 guest MB   ORCA_EMU_SWAP= memory+swap cap
# Image avm-emulator:latest = ubuntu 24.04 + the emulator's runtime libraries (libpulse0 libgl1 libnss3 ...).
# Stop it with: docker stop orca-emu-<avd>
R="$(cd "$(dirname "$0")" && pwd)"
AVD=${1:-orcadock}
PORT=${2:-5584}
SDK=${ANDROID_SDK_ROOT:-$HOME/Android/sdk}
KVM_GID=$(stat -c %g /dev/kvm)
exec docker run --rm --name orca-emu-$AVD --network host --device /dev/kvm --group-add $KVM_GID \
  --user $(id -u):$(id -g) --memory ${ORCA_EMU_MEM:-5g} --memory-swap ${ORCA_EMU_SWAP:-${ORCA_EMU_MEM:-5g}} ${ORCA_EMU_CPUS:+--cpus $ORCA_EMU_CPUS} \
  -e HOME=/tmp -e ANDROID_EMU_ENABLE_CRASH_REPORTING=0 -e ANDROID_SDK_ROOT=$SDK -e ANDROID_AVD_HOME=$R/hosttools/avd -e ANDROID_USER_HOME=$R/hosttools/androiduser \
  -v $SDK:$SDK -v $R/hosttools:$R/hosttools \
  avm-emulator:latest \
  $SDK/emulator/emulator -avd $AVD -port $PORT -no-window -no-audio -no-boot-anim -no-snapshot -no-metrics \
    -gpu ${ORCA_GPU:-swiftshader_indirect} -memory ${ORCA_EMU_RAM:-2560} -cores 2
