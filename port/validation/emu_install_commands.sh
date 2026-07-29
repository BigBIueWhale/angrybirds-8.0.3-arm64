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
FAIL=0
ok(){  echo "  [ OK ] $1"; }
bad(){ echo "  [FAIL] $1"; FAIL=1; }
die(){ echo "  [FAIL] $1"; echo "DONE (FAIL=1)"; adb emu kill >/dev/null 2>&1; exit 1; }
# A missing APK makes all three checks below fail on a file that was never there, and they would
# report it as Android REJECTING the install — which is exactly what they exist to detect, so the
# wrong conclusion would look like the right one.
[ -f "$APK" ] || { echo "  [FAIL] no APK at $APK — nothing below could be tested"
                   echo "DONE (FAIL=1)"; exit 1; }

echo "== boot API 36 =="
emulator -avd ab36 -no-window -no-audio -no-boot-anim -no-snapshot -accel on \
         -gpu swiftshader_indirect -partition-size 6144 -wipe-data >/tmp/e.log 2>&1 &
adb wait-for-device
for i in $(seq 1 240); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = 1 ] && break; sleep 2; done
sleep 15
REL=$(adb shell getprop ro.build.version.release 2>/dev/null|tr -d '\r')
SDK=$(adb shell getprop ro.build.version.sdk 2>/dev/null|tr -d '\r')
echo "  android ${REL:-<unreadable>} / API ${SDK:-<unreadable>}"
# The three success lines used to say "works on Android 16" whatever had actually booted. If the AVD
# were mis-provisioned — or no device came up at all — every message would still have named the OS
# this test is supposed to be about, which is how a log ends up asserting an OS nobody measured.
# So: assert the platform first, then quote the MEASURED value in each result rather than a literal.
case "$SDK" in
  ''|*[!0-9]*) die "could not read the API level — no device answered, so nothing below is measurable" ;;
esac
[ "$SDK" -ge 36 ] || die "booted API $SDK, but this test's subject is the A56's own OS (API 36+)"
OS="Android $REL (API $SDK)"

echo "== A. 'adb install' — the command a user actually types =="
adb uninstall com.rovio.angrybirds >/dev/null 2>&1
out=$(adb install "$APK" 2>&1); echo "$out" | tail -2 | sed 's/^/    /'
echo "$out" | grep -qi "Success" && ok "plain 'adb install' works on $OS" \
                                 || bad "plain 'adb install' does NOT work on $OS"

echo "== B. 'pm install' with no flags — exactly what the docs claim =="
adb uninstall com.rovio.angrybirds >/dev/null 2>&1
adb push "$APK" /data/local/tmp/ab.apk >/dev/null 2>&1
out=$(adb shell pm install /data/local/tmp/ab.apk 2>&1); echo "$out" | tail -2 | sed 's/^/    /'
echo "$out" | grep -qi "Success" && ok "plain 'pm install' works on $OS" \
                                 || bad "plain 'pm install' does NOT work on $OS"

echo "== C. reinstall over an existing install (what a rebuild does) =="
out=$(adb install -r "$APK" 2>&1); echo "$out" | tail -1 | sed 's/^/    /'
echo "$out" | grep -qi "Success" && ok "'adb install -r' updates over the previous install on $OS" \
                                 || bad "reinstall failed on $OS"
# Until now this printed [FAIL] lines and then exited 0: the script knew all three documented
# install commands had failed and still reported success to whatever ran it. Installing is step one
# of the whole brief — if the published command stops working the user never reaches anything else.
echo "DONE (FAIL=$FAIL)"
adb emu kill >/dev/null 2>&1
exit "$FAIL"
