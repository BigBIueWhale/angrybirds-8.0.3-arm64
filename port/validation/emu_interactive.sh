#!/bin/bash
# Persistent emulator: boot + install + launch the x86 shim APK, then STAY ALIVE (sleep) so the
# outer session can drive input via `docker exec ab-emu-run adb shell input ...` + screencap, to
# confirm INTERACTIVE play (tap the level-start card -> drag the slingshot -> launch the bird).
set +e
( sleep 1500; adb emu kill 2>/dev/null; pkill -9 -f qemu 2>/dev/null ) &   # hard watchdog 25min
APK=/work/out/angrybirds-8.0.3-x86shim.apk
OUT=/work/reports/shots; mkdir -p "$OUT"; ST="$OUT/emu_interactive.status"; echo BOOTING >"$ST"
emulator -avd abtest -no-window -no-audio -no-boot-anim -no-snapshot -accel on \
         -gpu swiftshader_indirect -partition-size 6144 -wipe-data >/tmp/emu.log 2>&1 &
adb wait-for-device
for i in $(seq 1 150); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = 1 ] && break; sleep 2; done
sleep 8
adb shell settings put global airplane_mode_on 1 >/dev/null 2>&1
adb push "$APK" /data/local/tmp/ab.apk >/tmp/push.log 2>&1
INST=fail
for t in 1 2 3 4; do adb shell pm install -r -d /data/local/tmp/ab.apk 2>&1 | grep -q Success && { INST=ok; break; }; sleep 4; done
echo "install=$INST" >>"$ST"
[ "$INST" = ok ] || { echo INSTALLFAIL >"$ST"; adb emu kill; exit 0; }
adb logcat -c >/dev/null 2>&1; adb logcat -G 32M >/dev/null 2>&1
adb logcat -s abshim > "$OUT/emu_interactive_abshim.txt" 2>/dev/null &
# screen size for coordinate planning
adb shell wm size 2>/dev/null | tee -a "$ST"
adb shell monkey -p com.rovio.angrybirds -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
echo LAUNCHED >>"$ST"
# stay alive for interactive driving
sleep 1400
adb emu kill >/dev/null 2>&1