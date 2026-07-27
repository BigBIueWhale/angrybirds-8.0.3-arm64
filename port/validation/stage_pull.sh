#!/bin/bash
# Boot x86 emu offline, run the de-phone-homed APK so real Android Java staging runs, then
# root + tar the app's private data (script_paths.json + any runtime-staged files) to sdcard
# and pull it. Robust: bounded adb root, no wait-for-device hang, full file listing.
set +e
( sleep 560; adb emu kill 2>/dev/null; pkill -9 -f qemu 2>/dev/null ) &   # watchdog
APK=/work/out/angrybirds-8.0.3-offline.apk
LOG=/work/out/staged_pull.log; : >"$LOG"
mkdir -p /work/out/staged
emulator -avd abtest -no-window -no-audio -no-boot-anim -no-snapshot -gpu swiftshader_indirect -wipe-data >/tmp/emu.log 2>&1 &
adb wait-for-device
for i in $(seq 1 120); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = 1 ] && break; sleep 2; done
echo "booted ~$((i*2))s" >>"$LOG"
adb install -r "$APK" >/tmp/inst.log 2>&1; echo "install [$(tail -1 /tmp/inst.log)]" >>"$LOG"
adb shell monkey -p com.rovio.angrybirds -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
echo "launched; staging+running 130s" >>"$LOG"; sleep 130
# root (bounded, no wait-for-device hang)
timeout 40 adb root >>"$LOG" 2>&1; sleep 6
echo "id after root: $(adb shell id 2>/dev/null | tr -d '\r')" >>"$LOG"
echo "=== full app-data file tree ===" >>"$LOG"
adb shell "find /data/data/com.rovio.angrybirds -type f -exec ls -la {} \; 2>/dev/null" >>"$LOG" 2>&1
echo "=== script_paths.json locations (device-wide) ===" >>"$LOG"
adb shell "find /data /sdcard /storage -iname 'script_paths*' 2>/dev/null" >>"$LOG" 2>&1
# extract everything via tar->sdcard->pull (robust vs per-file perms)
adb shell "tar czf /sdcard/ab_appdata.tgz -C /data/data/com.rovio.angrybirds . 2>/dev/null" 2>>"$LOG"
adb pull /sdcard/ab_appdata.tgz /work/out/staged/ >>"$LOG" 2>&1
echo "pulled: $(ls -la /work/out/staged/ab_appdata.tgz 2>/dev/null | awk '{print $5}') bytes" >>"$LOG"
echo DONE >>"$LOG"
adb emu kill >/dev/null 2>&1
