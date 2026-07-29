#!/bin/bash
# emu_361_why.sh — WHY does Android 36.1 not register this app's launcher activity?
#
# WHERE THIS PICKS UP
# -------------------
# R18 found that on the 36.1 image the app installs and then has no launchable activity, and that the
# same APK is launchable on 36. R47 refuted the only explanation on record: with `minSdk=24
# targetSdk=30` (and `resources.arsc` re-stored uncompressed so the install is accepted) it is STILL
# unlaunchable, so SDK levels are not the mechanism.
#
# Two observations from R47 cannot both describe the same state, and resolving that is the next step:
#   * R18 recorded `dumpsys` showing the activity PRESENT with its VIEW/BROWSABLE deep-link filter,
#     with only MAIN/LAUNCHER missing.
#   * R47's `am start -n .../com.rovio.fusion.App` answered "Activity class ... does not exist",
#     which says the component is not registered at all.
#
# So this asks the package manager directly, on the SHIPPED artifact (targetSdk=26 — the thing the user
# installs, not a diagnostic variant), and captures the one thing neither earlier run captured: what
# Android logs WHILE PARSING the manifest. PackageManagerService and the manifest parser normally say
# why they drop a component.
#
# PREMISES, because each failure mode here reads like an answer:
#   1. the install succeeds                  — nothing below describes an app that is not installed
#   2. a control app IS launchable           — else the query is broken and "no" means nothing
#   3. logcat was actually captured          — an empty capture and a clean parse look identical
#
#   docker run --rm --network none --device /dev/kvm --group-add "$(getent group kvm|cut -d: -f3)" \
#       -v "$PWD":/work ab-emu-361 bash /work/port/validation/emu_361_why.sh
set +e
source "$(dirname "$0")/lib_install.sh"
PKG=com.rovio.angrybirds
APK=${ABSHIM_APK:-/work/out/angrybirds-8.0.3-x86shim-release.apk}
OUT=/work/reports/shots; mkdir -p "$OUT"
LOG="$OUT/why361.txt"; : >"$LOG"
PARSE="$OUT/why361_parse.txt"; : >"$PARSE"
DUMP="$OUT/why361_dumpsys.txt"; : >"$DUMP"
say(){ echo "$@" | tee -a "$LOG"; }
FAIL=0
[ -f "$APK" ] || { say "  [FAIL] missing $APK"; say "DONE (FAIL=1)"; exit 1; }

say "== boot the 36.1 image =="
emulator -avd "${ABSHIM_AVD:-ab361}" -no-window -no-audio -no-boot-anim -no-snapshot -accel on \
         -gpu swiftshader_indirect -partition-size 6144 -wipe-data >/tmp/e.log 2>&1 &
adb wait-for-device
for i in $(seq 1 240); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = 1 ] && break; sleep 2; done
sleep 15
say "  android $(adb shell getprop ro.build.version.release 2>/dev/null|tr -d '\r') (API $(adb shell getprop ro.build.version.sdk 2>/dev/null|tr -d '\r')) incremental=$(adb shell getprop ro.build.version.incremental 2>/dev/null|tr -d '\r')"
say "  image: $(adb shell getprop ro.build.fingerprint 2>/dev/null|tr -d '\r')"

say
say "== PREMISE 2: a control app IS launchable here =="
# RETRY, do not query once. A single query 15s after boot_completed found nothing on this image and
# failed the premise — while the same query passed in R47's run, which reached it later. The package
# manager is still settling, so "Settings has no launcher" was a statement about timing, not about the
# device. A premise that flakes turns every real result behind it into a coin toss.
CTRL=""
for i in $(seq 1 30); do
    CTRL=$(adb shell cmd package resolve-activity --brief -c android.intent.category.LAUNCHER com.android.settings 2>/dev/null | tr -d '\r' | tail -1)
    case "$CTRL" in *settings*) break ;; esac
    sleep 5
done
say "  com.android.settings -> ${CTRL:-<none>}  (after $((i*5))s of settling)"
case "$CTRL" in *settings*) say "  [ OK ] the LAUNCHER query works on this device" ;;
                *) say "  [FAIL] control does not resolve — nothing below would mean anything"; FAIL=1 ;; esac

say
say "== capture what Android logs WHILE PARSING the manifest =="
adb uninstall "$PKG" >/dev/null 2>&1
adb logcat -c >/dev/null 2>&1; adb logcat -G 32M >/dev/null 2>&1
adb logcat -v brief > "$PARSE" 2>/dev/null &
LPID=$!
sleep 2
# install_apk from lib_install.sh, NOT a raw `adb install`. This script sourced that library and then
# hand-rolled the install anyway, and got exactly what the library exists to absorb:
#   cmd: Failure calling service package: Broken pipe (32)
# which is the package service not being ready yet — indistinguishable, from the outside, from the APK
# being rejected. lib_install.sh waits for the service and retries transient errors while still
# reporting a real rejection as a rejection.
install_apk "$APK" 5 > /tmp/i361.$$ 2>&1; irc=$?
sleep 8
kill $LPID 2>/dev/null
while IFS= read -r _l; do say "  $_l"; done < /tmp/i361.$$; rm -f /tmp/i361.$$
say "  install rc=$irc"
# PREMISE 3: an empty capture and a clean parse are indistinguishable, so require lines.
NL=$(grep -ac . "$PARSE"); say "  logcat lines captured: $NL"
[ "${NL:-0}" -gt 0 ] || { say "  [FAIL] captured nothing — cannot distinguish 'no complaint' from 'no capture'"; FAIL=1; }
adb shell pm list packages 2>/dev/null | grep -q rovio \
  && say "  [ OK ] installed" \
  || { say "  [FAIL] not installed"; say "DONE (FAIL=1)"; kill $LPID 2>/dev/null; adb emu kill; exit 1; }

say
say "== anything the package manager said about our package =="
grep -aiE "packagemanager|packageparser|ParsingPackageUtils|apk|rovio|activity" "$PARSE" \
  | grep -aiE "rovio|skip|ignor|drop|reject|unknown|invalid|malformed|not supported|deprecat|exported|fail" \
  | head -14 | sed 's/^/    /' | tee -a "$LOG"
say "  (full parse log: reports/shots/why361_parse.txt)"

say
say "== what the package manager thinks is registered =="
adb shell dumpsys package "$PKG" > "$DUMP" 2>/dev/null
say "  dumpsys bytes: $(wc -c <"$DUMP")"
say "  --- declared activities ---"
grep -aA3 -iE "^\s*Activity Resolver Table|activities:" "$DUMP" | head -12 | sed 's/^/    /' | tee -a "$LOG"
say "  --- does com.rovio.fusion.App appear at all? ---"
say "    occurrences of 'com.rovio.fusion.App' in dumpsys: $(grep -ac 'com.rovio.fusion.App' "$DUMP")"
say "    occurrences of 'android.intent.category.LAUNCHER': $(grep -ac 'LAUNCHER' "$DUMP")"
say "    occurrences of 'android.intent.action.MAIN':       $(grep -ac 'android.intent.action.MAIN' "$DUMP")"
say "    occurrences of 'BROWSABLE':                        $(grep -ac 'BROWSABLE' "$DUMP")"

say
say "== the two competing descriptions, resolved =="
A=$(grep -ac 'com.rovio.fusion.App' "$DUMP")
M=$(grep -ac 'android.intent.action.MAIN' "$DUMP")
if [ "${A:-0}" -eq 0 ]; then
    say "  the activity is ABSENT from dumpsys entirely — consistent with R47's 'class does not exist',"
    say "  and NOT with R18's note that it was present with a VIEW/BROWSABLE filter. The component is"
    say "  being dropped at parse time, not merely losing its launcher category."
elif [ "${M:-0}" -eq 0 ]; then
    say "  the activity IS present but carries no MAIN filter — consistent with R18: the component"
    say "  survives and only its launcher category is dropped."
else
    say "  both the activity and a MAIN filter are present, yet LAUNCHER does not resolve — the"
    say "  mechanism is in resolution, not registration."
fi
say "  (dumpsys saved: reports/shots/why361_dumpsys.txt)"
say "DONE (FAIL=$FAIL)"
adb emu kill >/dev/null 2>&1
exit "$FAIL"
