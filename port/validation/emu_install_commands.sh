#!/bin/bash
# emu_install_commands.sh — verify the install commands the DOCS give the user actually work.
#
# WHY THIS EXISTS
# ---------------
# Every other emulator script installs with `pm install -r -d` — an internal form with a reinstall
# flag and a downgrade flag. The documentation tells the user something different: README says the
# APK "installs with plain `pm install`", and ONDEVICE.md points at `adb install`. Those are not the
# same claim, and the one that was tested was not the one that was published.
#
# That matters more than it sounds. Installing is step one; if the documented command fails on
# Android 16, the user is stuck before anything else in this project is relevant to them. Android
# has repeatedly tightened install policy for apps with a low targetSdk (this one targets 26), so
# "it worked on API 34" does not settle it.
#
# Checks three forms on the A56's actual OS version:
#   A  adb install <apk>        what a user actually types
#   B  pm install <apk>         no flags at all — exactly what README claims
#   C  adb install -r <apk>     updating over a previous install, which every rebuild does
#
# RESULT (2026-07-28, API 36): all three succeed.
#
#   docker run --rm --network none --device /dev/kvm --group-add "$(getent group kvm|cut -d: -f3)" \
#       -v "$PWD":/work ab-emu-36 bash /work/port/validation/emu_install_commands.sh
#
# Uses the x86 proxy because the emulator is x86 — this tests the OS's install POLICY and the
# command syntax, which is what the docs claim; it is not an arm64 install test. The arm64 APK
# cannot be installed anywhere on this host (see port/OPEN_FINDINGS.md).
set +e

( sleep 2400; adb emu kill 2>/dev/null; pkill -9 -f qemu 2>/dev/null ) &
APK="${ABSHIM_APK:-/work/out/angrybirds-8.0.3-x86shim-release.apk}"
echo "== boot API 36 =="
emulator -avd ab36 -no-window -no-audio -no-boot-anim -no-snapshot -accel on \
         -gpu swiftshader_indirect -partition-size 6144 -wipe-data >/tmp/e.log 2>&1 &
adb wait-for-device
for i in $(seq 1 240); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = 1 ] && break; sleep 2; done
sleep 15
echo "  android $(adb shell getprop ro.build.version.release|tr -d '\r') / API $(adb shell getprop ro.build.version.sdk|tr -d '\r')"

echo "== A. 'adb install' — the command a user actually types =="
adb uninstall com.rovio.angrybirds >/dev/null 2>&1
out=$(adb install "$APK" 2>&1); echo "$out" | tail -2 | sed 's/^/    /'
echo "$out" | grep -qi "Success" && echo "  [OK] plain 'adb install' works on Android 16" || echo "  [FAIL] plain 'adb install' does NOT work"

echo "== B. 'pm install' with no flags — exactly what the docs claim =="
adb uninstall com.rovio.angrybirds >/dev/null 2>&1
adb push "$APK" /data/local/tmp/ab.apk >/dev/null 2>&1
out=$(adb shell pm install /data/local/tmp/ab.apk 2>&1); echo "$out" | tail -2 | sed 's/^/    /'
echo "$out" | grep -qi "Success" && echo "  [OK] plain 'pm install' works on Android 16" || echo "  [FAIL] plain 'pm install' does NOT work"

echo "== C. reinstall over an existing install (what a rebuild does) =="
out=$(adb install -r "$APK" 2>&1); echo "$out" | tail -1 | sed 's/^/    /'
echo "$out" | grep -qi "Success" && echo "  [OK] 'adb install -r' updates over the previous install" || echo "  [FAIL] reinstall failed"
echo DONE
adb emu kill >/dev/null 2>&1
