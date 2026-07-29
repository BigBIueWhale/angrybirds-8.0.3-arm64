#!/bin/bash
# emu_update_install.sh — do saves actually survive an update-install?
#
# WHY THIS EXISTS
# ---------------
# "Both are signed with the same key, so either update-installs over the other without losing saves"
# appears in RELEASE_NOTES.md, ONDEVICE.md (twice) and OPEN_FINDINGS.md, where it is stated as a
# finding: "Saves survive an update-install (same signer, same package)". Nothing tested it.
#
# It is also the most likely thing a user does after the first install: rebuild, or switch to the
# audio variant, and reinstall over the top. `emu_save_test.sh` covers force-stop and relaunch —
# same APK — which does not exercise the package manager replacing the app at all.
#
# WHAT IT DOES
#   1. install the release build, play until it writes saves, force-stop
#   2. hash the save files BEFORE anything is reinstalled
#   3. `adb install -r` the AUDIO variant over it — no uninstall, different APK, same signer
#   4. hash again IMMEDIATELY, before relaunching: this isolates "did the update preserve the data"
#      from "does the game rewrite its files on next launch", which it does
#   5. relaunch and confirm the app runs and reads its own files back
#   6. install the release build back over the audio one and hash a third time — ONDEVICE.md
#      promises the round trip ("update-installs straight back over the audio build"), not just one
#      direction
#
#   docker run --rm --network none --device /dev/kvm --group-add "$(getent group kvm|cut -d: -f3)" \
#       -v "$PWD":/work ab-emu-34 bash /work/port/validation/emu_update_install.sh
set +e
source "$(dirname "$0")/lib_settle.sh"
source "$(dirname "$0")/lib_dialogs.sh"
source "$(dirname "$0")/lib_metrics.sh"
source "$(dirname "$0")/lib_install.sh"
source "$(dirname "$0")/lib_selfhash.sh"

A=/work/out/angrybirds-8.0.3-x86shim-release.apk    # "build A" — the shipping configuration
B=/work/out/angrybirds-8.0.3-x86shim-audio.apk      # "build B" — a genuinely different APK, same signer
PKG=com.rovio.angrybirds
DIR=/data/data/$PKG
OUT=/work/reports/shots; mkdir -p "$OUT"; LOG="$OUT/updinstall.txt"; : >"$LOG"
ABLOG="$OUT/updinstall_abshim.txt"; : >"$ABLOG"
say(){ echo "$@" | tee -a "$LOG"; }
selfhash_begin
FAIL=0

for f in "$A" "$B"; do [ -f "$f" ] || { say "[FAIL] missing $f"; exit 1; }; done

# The claim rests on the two APKs sharing a signer; if they did not, this would test nothing.
SA=$(apk_signer "$A")   # see lib_install.sh: apksigner is not in the ab-emu images
SB=$(apk_signer "$B")
if [ -z "$SA" ] || [ "$SA" != "$SB" ]; then
    say "[FAIL] the two builds do not share a signer (A=${SA:-?} B=${SB:-?}) — an update-install"
    say "       could not succeed regardless of save handling, so this would prove nothing."
    exit 1
fi
say "  both builds signed by ${SA:0:24}…  (update-install is possible)"

say "== boot =="
emulator -avd "${ABSHIM_AVD:-abtest34}" -no-window -no-audio -no-boot-anim -no-snapshot -accel on \
    -gpu swiftshader_indirect >/tmp/emu.log 2>&1 &
adb wait-for-device
for i in $(seq 1 120); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = "1" ] && break; sleep 5; done
say "  android $(adb shell getprop ro.build.version.release 2>/dev/null|tr -d '\r') (API $(adb shell getprop ro.build.version.sdk 2>/dev/null|tr -d '\r'))"
adb shell settings put global airplane_mode_on 1 >/dev/null 2>&1
# Reading another app's /data/data requires root. Without this the save hashes come back EMPTY and
# the run aborts with "no save files were written" — which is what happened first time, and would
# have been indistinguishable from the game genuinely not saving. emu_save_test.sh does the same.
adb root >/dev/null 2>&1; sleep 4; adb wait-for-device
say "  adb identity: $(adb shell whoami 2>/dev/null | tr -d '\r')  (root is required to read $DIR)"
adb uninstall "$PKG" >/dev/null 2>&1          # start from no prior install

# Hash the game's own save files. NOT every private file: the analytics SDKs rewrite theirs
# constantly (Flurry/HockeyApp), so hashing all 48 would report a difference that has nothing to do
# with the update. settings.lua and highscores.lua are the user's actual progress.
savehash(){ adb shell "md5sum $DIR/files/settings.lua $DIR/files/highscores.lua 2>/dev/null" \
            | tr -d '\r' | awk '{print $1}' | tr '\n' ' '; }
savecount(){ adb shell "find $DIR -type f 2>/dev/null | wc -l" 2>/dev/null | tr -d '\r'; }

say
say "== 1. install build A and play until it writes saves =="
install_apk "$A" 4 2>&1 | tee -a "$LOG"
adb shell pm list packages 2>/dev/null | grep -q rovio || { say "[FAIL] install A"; adb emu kill; exit 1; }
adb logcat -c >/dev/null 2>&1; adb logcat -s abshim > "$ABLOG" 2>/dev/null &
adb shell monkey -p "$PKG" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
for s in $(seq 1 110); do sleep 5
    fn=$(grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null|tail -1|grep -oE '[0-9]+')
    [ -n "$fn" ] && [ "$fn" -ge 601 ] && { say "  rendering at ~$((s*5))s frame[$fn]"; break; }
done
dismiss_system_dialogs
adb shell input tap 390 266; sleep 4; adb shell input tap 390 266; sleep 10
for i in 1 2 3; do adb shell input swipe 207 118 110 150 700; sleep 8; done
settle_frames "$ABLOG" 300 300
adb shell am force-stop "$PKG"; sleep 5                 # force a flush to disk

H1=$(savehash); N1=$(savecount)
say "  private files: $N1   settings/highscores md5: ${H1:-<none>}"
if [ -z "$H1" ]; then
    say "  [FAIL] no save files were written, so there is nothing whose survival could be tested."
    adb emu kill; exit 1
fi

say
say "== 2. update-install build B OVER it (no uninstall) =="
adb push "$B" /data/local/tmp/b.apk >/dev/null 2>&1
R=$(adb shell pm install -r -d /data/local/tmp/b.apk 2>&1 | tr -d '\r')
say "  pm install -r: $R"
case "$R" in
    *Success*) say "  [ OK ] the update-install itself succeeded" ;;
    *) say "  [FAIL] the documented update-install did not work: $R"; FAIL=1 ;;
esac

H2=$(savehash); N2=$(savecount)
say "  private files: $N2   settings/highscores md5: ${H2:-<none>}"
if [ "$H2" = "$H1" ] && [ -n "$H2" ]; then
    say "  [ OK ] saves are byte-identical across the update — the documented claim holds"
else
    say "  [FAIL] saves changed or vanished across the update: '$H1' -> '$H2'"; FAIL=1
fi

say
say "== 3. the updated build must still run and read its own files back =="
adb logcat -c >/dev/null 2>&1
adb shell monkey -p "$PKG" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
sleep 60
RD=$(adb logcat -d -s abshim 2>/dev/null | grep -acE "fopen.*($DIR|files/(settings|highscores))")
PID=$(adb shell pidof "$PKG" 2>/dev/null | tr -d '\r')
say "  alive pid: ${PID:-<none>}   reads of its own files: $RD"
[ -n "$PID" ] && say "  [ OK ] the updated build runs" || { say "  [FAIL] the updated build did not stay alive"; FAIL=1; }
adb shell am force-stop "$PKG"; sleep 4

say
say "== 4. and back again: install A over B (ONDEVICE.md promises the round trip) =="
adb push "$A" /data/local/tmp/a.apk >/dev/null 2>&1
R2=$(adb shell pm install -r -d /data/local/tmp/a.apk 2>&1 | tr -d '\r')
say "  pm install -r: $R2"
H3=$(savehash)
say "  settings/highscores md5: ${H3:-<none>}"
case "$R2" in
    *Success*) [ -n "$H3" ] && say "  [ OK ] round trip works and the saves are still there" \
                            || { say "  [FAIL] saves vanished on the way back"; FAIL=1; } ;;
    *) say "  [FAIL] could not install A back over B: $R2"; FAIL=1 ;;
esac

say
selfhash_verify
say "DONE (FAIL=$FAIL)"
adb emu kill >/dev/null 2>&1
exit "$FAIL"
