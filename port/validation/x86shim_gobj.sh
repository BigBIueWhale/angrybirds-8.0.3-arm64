#!/bin/bash
# Run the x86 diagnostic shim (with the [g+0x80] watchpoint) in the emulator and capture whether
# the game-ready globals get set with a REAL app object + real JNI.
set +e
( sleep 420; adb emu kill 2>/dev/null; pkill -9 -f qemu 2>/dev/null ) &
APK=/work/out/angrybirds-8.0.3-x86shim.apk
LOG=/work/reports/shots/x86shim_gobj.txt; : >"$LOG"; mkdir -p /work/reports/shots
emulator -avd abtest -no-window -no-audio -no-boot-anim -no-snapshot -gpu swiftshader_indirect -wipe-data >/tmp/emu.log 2>&1 &
adb wait-for-device
for i in $(seq 1 120); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = 1 ] && break; sleep 2; done
echo "booted ~$((i*2))s" >>"$LOG"
adb shell settings put global airplane_mode_on 1 >/dev/null 2>&1
adb install -r "$APK" >/tmp/inst.log 2>&1; echo "install [$(tail -1 /tmp/inst.log)]" >>"$LOG"
adb logcat -c >/dev/null 2>&1
adb shell monkey -p com.rovio.angrybirds -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
sleep 150
echo "=== the [g+0x80]/[g+0x7c] watchpoint writes (does the ready flag get set with REAL JNI?) ===" >>"$LOG"
adb logcat -d -s abshim 2>/dev/null | grep -E 'gobj WRITE' >>"$LOG"
echo "=== boot call[] sequence (de-duped, un-flooded) + frame[] draws ===" >>"$LOG"
adb logcat -d -s abshim 2>/dev/null | grep -E 'call\[|frame\[' | grep -vE 'nativeUpdate \(Z\)' | tail -40 >>"$LOG"
echo "=== engine Framework/errors ===" >>"$LOG"
adb logcat -d -s Framework AndroidRuntime 2>/dev/null | grep -iE 'rror|xcept|fatal|effect|scene|Framework' | tail -12 >>"$LOG"
adb exec-out screencap -p > /work/reports/shots/x86shim_gobj.png 2>/dev/null
echo "final_pid: $(adb shell pidof com.rovio.angrybirds 2>/dev/null|tr -d '\r')  crash: $(adb logcat -d 2>/dev/null|grep -ciE 'SIGSEGV|SIGABRT|Fatal signal')" >>"$LOG"
echo DONE >>"$LOG"
adb emu kill >/dev/null 2>&1
