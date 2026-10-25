#!/bin/bash
# Second tablet AVD "orcatab2" (2560x1600 @320dpi), port 5584 => serial emulator-5584. orcatest/5580 is shared with the PrusaSlicer port.
# Phone layout on the same AVD: adb shell wm size 1080x2400; wm density 420 (reset: wm size reset; wm density reset).
R=/home/lucas/Documents/orcaslicerandroid
export ANDROID_SDK_ROOT=/home/lucas/Android/sdk ANDROID_AVD_HOME=$R/hosttools/avd ANDROID_USER_HOME=$R/hosttools/androiduser
# Capped at 6 GB (systemd scope): with the swangle GPU the host-side qemu once grew to 18 GB and the OOM killer hit the shared machine.
exec systemd-run --user --scope -q -p MemoryMax=9G -p MemorySwapMax=0 nice -n 5 /home/lucas/Android/sdk/emulator/emulator -avd orcatab2 -port 5584 -no-window -no-audio -no-boot-anim -no-snapshot -gpu ${ORCA_GPU:-guest} -memory 3072 -cores 2
