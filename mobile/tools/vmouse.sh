#!/bin/bash
# Test helper for mouse input: drives mobile/tools/vmouse (uinput, needs adb root) through /data/local/tmp/mf.
# Setup: build vmouse.c with the NDK clang (x86_64-linux-android28-clang -O2), push to /data/local/tmp, then
#   adb shell "cd /data/local/tmp; mkfifo mf; (sleep 100000 > mf &); (nohup ./vmouse < mf > vm.log 2>&1 &)"  (this adb call stays open: background it)
# mouse.sh goto X Y | cmd "<vmouse command>"   (emulator-5584, /data/local/tmp/mf fed to vmouse)
A="/home/opc/Android/sdk/platform-tools/adb -s emulator-5584"
send() { timeout 30 $A shell "echo '$1' > /data/local/tmp/mf"; }
pos() { timeout 20 $A shell dumpsys input | grep -o "xCursorPosition=[0-9.-]*, yCursorPosition=[0-9.-]*" | tail -1 | sed 's/xCursorPosition=//; s/, yCursorPosition=/ /'; }
case $1 in
  goto)
    for i in 1 2 3 4; do
      read x y <<< "$(pos)"; dx=$(python3 -c "print(round(($2-$x)/1.05))"); dy=$(python3 -c "print(round(($3-$y)/1.05))")
      [ "$dx" = 0 ] && [ "$dy" = 0 ] && break
      send "m $dx $dy"; sleep $(python3 -c "print(0.5+max(abs($dx),abs($dy))/4*0.009)")
    done; pos;;
  cmd) send "$2";;
esac
