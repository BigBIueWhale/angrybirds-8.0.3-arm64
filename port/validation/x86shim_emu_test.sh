#!/bin/bash
# Install the x86_64 SHIM apk in the x86 Android emulator (REAL ART + lifecycle) and watch it
# boot+render. Longer settle (the shim's Unicorn emulation boots slower than the native engine,
# which itself took ~120s to reach a level). Progression screenshots + clean frame[]/Framework logs.
set +e
( sleep 900; adb emu kill 2>/dev/null; pkill -9 -f qemu 2>/dev/null ) &   # watchdog
APK=/work/out/angrybirds-8.0.3-x86shim.apk
LOG=/work/reports/shots/x86shim.txt; : >"$LOG"
mkdir -p /work/reports/shots
echo "APK=$(basename "$APK") $(du -h "$APK"|cut -f1)" >>"$LOG"
emulator -avd abtest -no-window -no-audio -no-boot-anim -no-snapshot -gpu swiftshader_indirect -wipe-data >/tmp/emu.log 2>&1 &
adb wait-for-device
for i in $(seq 1 120); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = 1 ] && break; sleep 2; done
echo "booted ~$((i*2))s" >>"$LOG"
adb shell settings put global airplane_mode_on 1 >/dev/null 2>&1
adb install -r "$APK" >/tmp/inst.log 2>&1; echo "install [$(tail -1 /tmp/inst.log)]" >>"$LOG"
adb logcat -c >/dev/null 2>&1
adb shell monkey -p com.rovio.angrybirds -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
echo "launched; watching the shim boot+render (progression shots every 90s)" >>"$LOG"
for t in 1 2 3 4 5 6; do
  sleep 90
  nb=$(adb exec-out screencap -p 2>/dev/null | wc -c)
  adb exec-out screencap -p > /work/reports/shots/x86shim_t${t}.png 2>/dev/null
  fr=$(adb logcat -d -s abshim 2>/dev/null | grep -oE 'frame\[[0-9]+\] GL draws=[0-9]+' | tail -1)
  echo "  +$((t*90))s: screenshot=${nb}B  pid=$(adb shell pidof com.rovio.angrybirds 2>/dev/null|tr -d '\r')  ${fr}" >>"$LOG"
done
echo "=== boot call[] sequence (first-call log, now un-flooded) ===" >>"$LOG"
adb logcat -d -s abshim 2>/dev/null | grep -E 'call\[|frame\[|ready|abshim ready|engine ' | grep -vE 'call\[[0-9]+\] Java.*nativeUpdate \(Z\)' | tail -50 >>"$LOG"
echo "=== engine (Framework) + any errors ===" >>"$LOG"
adb logcat -d -s Framework AndroidRuntime abshim 2>/dev/null | grep -iE 'Framework|rror|xcept|shader|GL|fatal|effect|scene|level|menu' | tail -30 >>"$LOG"
echo "final_pid: $(adb shell pidof com.rovio.angrybirds 2>/dev/null | tr -d '\r')" >>"$LOG"
echo "native_crash: $(adb logcat -d 2>/dev/null | grep -ciE 'SIGSEGV|SIGABRT|Fatal signal')" >>"$LOG"
echo DONE >>"$LOG"
adb emu kill >/dev/null 2>&1
