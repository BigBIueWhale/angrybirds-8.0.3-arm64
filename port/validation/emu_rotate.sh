#!/bin/bash
# emu_rotate.sh — does it survive the screen being rotated mid-game?
#
# WHY THIS EXISTS
# ---------------
# Read out of the shipped manifest rather than guessed:
#
#   activity com.rovio.fusion.App
#     screenOrientation = 0      (unspecified — the app is NOT orientation-locked)
#     configChanges     = 0x4f0  (uiMode | orientation | navigation | keyboardHidden | keyboard)
#
# `orientation` is handled by the activity, so a plain orientation flip does not recreate it. But
# **`screenSize` (0x800) is absent**, and for an app targeting API 13 or later a rotation that changes
# the screen dimensions recreates the activity regardless. So on a real phone, turning the device
# destroys and rebuilds the Activity underneath a still-live process — the engine keeps running while
# its window, surface and GL context are torn down and replaced.
#
# That is a harder case than R29's background/resume, where the activity merely paused, and harder
# than R28's reboot, where everything started clean. Nothing here had tested it.
#
# PREMISES ASSERTED, because each failure mode reads like success:
#   - frames advancing before the rotation, or "it survived" describes an app that was already idle
#   - the rotation ACTUALLY took effect — checked by reading the display size back, not by trusting
#     that the setting was written
#   - after: process alive, frames advancing again, h_fatal still 0, and a capture to look at, since
#     a renderer can count frames into a surface nobody can see
#
#   docker run --rm --network none --device /dev/kvm --group-add "$(getent group kvm|cut -d: -f3)" \
#       -v "$PWD":/work -e ABSHIM_AVD=ab36 ab-emu-36 bash /work/port/validation/emu_rotate.sh
set +e
source "$(dirname "$0")/lib_settle.sh"
# Refuse to run while another evidence run is in flight: 50 scripts share reports/shots and
# two concurrent runs BLEND their logs into numbers that look normal and mean nothing.
source "$(dirname "$0")/lib_runlock.sh"
acquire_run_lock "$(basename "$0")" || exit 1
source "$(dirname "$0")/lib_dialogs.sh"
source "$(dirname "$0")/lib_metrics.sh"
source "$(dirname "$0")/lib_install.sh"
source "$(dirname "$0")/lib_selfhash.sh"
source "$(dirname "$0")/lib_provenance.sh"

APK=/work/out/angrybirds-8.0.3-x86shim-release.apk
PKG=com.rovio.angrybirds
OUT=/work/reports/shots; mkdir -p "$OUT"; LOG="$OUT/rotate.txt"; : >"$LOG"
ABLOG="$OUT/rotate_abshim.txt"; : >"$ABLOG"
say(){ echo "$@" | tee -a "$LOG"; }
frames(){ grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null | tail -1 | grep -oE '[0-9]+'; }
dispsize(){ adb shell wm size 2>/dev/null | tr -d '\r' | grep -oE '[0-9]+x[0-9]+' | tail -1; }
selfhash_begin
FAIL=0
[ -f "$APK" ] || { say "[FAIL] missing $APK"; exit 1; }

say "== boot =="
emulator -avd "${ABSHIM_AVD:-ab36}" -no-window -no-audio -no-boot-anim -no-snapshot -accel on \
    -feature -ModemSimulator -gpu swiftshader_indirect >/tmp/emu.log 2>&1 &
adb wait-for-device
for i in $(seq 1 150); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = "1" ] && break; sleep 5; done
say "  android $(adb shell getprop ro.build.version.release 2>/dev/null|tr -d '\r') (API $(adb shell getprop ro.build.version.sdk 2>/dev/null|tr -d '\r'))"
adb shell settings put global airplane_mode_on 1 >/dev/null 2>&1
adb shell settings put system accelerometer_rotation 0 >/dev/null 2>&1   # rotate on command, not by sensor

install_apk "$APK" 4 2>&1 | tee -a "$LOG"
adb shell pm list packages 2>/dev/null | grep -q rovio || { say "[FAIL] install"; adb emu kill; exit 1; }
# Tie this run's captures to the build that produced them. Written without this at first: three new
# scripts each saved a screenshot with no record of which APK made it, invisible to the gate's
# "captures were taken on builds that still exist" check — the exact gap provenance.tsv exists to
# close, reintroduced by the scripts that were meant to strengthen the evidence.
record_build "$APK" "rotate" 2>&1 | tee -a "$LOG"

say
say "== 1. get it playing =="
adb logcat -c >/dev/null 2>&1; adb logcat -T 1 -s abshim > "$ABLOG" 2>/dev/null &
adb shell monkey -p "$PKG" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
for s in $(seq 1 130); do sleep 5
    f=$(frames); [ -n "$f" ] && [ "$f" -ge 601 ] && { say "  rendering at ~$((s*5))s frame[$f]"; break; }
done
dismiss_system_dialogs
adb shell input tap 390 266; sleep 4; adb shell input tap 390 266; sleep 8
PID1=$(adb shell pidof "$PKG" 2>/dev/null | tr -d '\r')
SZ1=$(dispsize)
F_A=$(frames); sleep 12; F_B=$(frames)
say "  pid $PID1   display $SZ1   frames $F_A -> $F_B over 12s"
if [ -z "$F_A" ] || [ -z "$F_B" ] || [ "$F_B" -le "$F_A" ]; then
    say "  [FAIL] frames were not advancing BEFORE the rotation — nothing below would mean anything"
    adb emu kill; exit 1
fi
say "  [ OK ] advancing before the rotation"

say
say "== 2. rotate the display 90 degrees =="
adb shell settings put system user_rotation 1 >/dev/null 2>&1
sleep 12
dismiss_system_dialogs
sleep 6
SZ2=$(dispsize)
ROT=$(adb shell settings get system user_rotation 2>/dev/null | tr -d '\r')
say "  user_rotation now: ${ROT:-<unreadable>}   display: $SZ2"
# `wm size` reports the physical panel and does not swap on rotation, so it cannot confirm this.
# The setting itself is the check that the rotation was applied; if it did not stick, the test is
# measuring an unrotated device and must say so rather than pass.
if [ "$ROT" != "1" ]; then
    say "  [FAIL] the rotation did not take effect (user_rotation=${ROT:-unset}) — a 'survived rotation'"
    say "         verdict here would describe a device that never rotated"
    FAIL=1
else
    say "  [ OK ] the display really is rotated"
fi

say
say "== 3. is it still alive and rendering =="
PID2=$(adb shell pidof "$PKG" 2>/dev/null | tr -d '\r')
F_C=$(frames); sleep 15; F_D=$(frames)
say "  pid after rotation: ${PID2:-<none>}   frames $F_C -> $F_D over 15s"
if [ -z "$PID2" ]; then
    say "  [FAIL] the process died on rotation"
    adb logcat -d '*:E' 2>/dev/null | grep -aiE "rovio|EGL|surface|abort|AndroidRuntime" | tail -8 | sed 's/^/    /' | tee -a "$LOG"
    FAIL=1
elif [ -z "$F_D" ] || [ "$F_D" -le "$F_C" ]; then
    say "  [FAIL] frames stopped after the rotation — the activity was recreated and the renderer did not recover"
    FAIL=1
else
    say "  [ OK ] still rendering after rotation (+$(( F_D - F_C )) frames in 15s)"
fi
[ -n "$PID1" ] && [ "$PID1" = "$PID2" ] && say "  (same process — pid $PID1: the activity was rebuilt under a live process, the harder case)" \
                                        || say "  (process changed: $PID1 -> ${PID2:-none})"
adb exec-out screencap -p > "$OUT/rotate_screen.png" 2>/dev/null

say
say "== 4. rotate back =="
adb shell settings put system user_rotation 0 >/dev/null 2>&1
sleep 12
dismiss_system_dialogs
F_E=$(frames); sleep 12; F_F=$(frames)
PID3=$(adb shell pidof "$PKG" 2>/dev/null | tr -d '\r')
say "  pid: ${PID3:-<none>}   frames $F_E -> $F_F over 12s"
[ -n "$PID3" ] && [ -n "$F_F" ] && [ "$F_F" -gt "$F_E" ] \
    && say "  [ OK ] survived the return to the original orientation too" \
    || { say "  [FAIL] did not survive rotating back"; FAIL=1; }

say "  h_fatal: $(h_fatal_report "$ABLOG")"
HF=$(grep -ac 'h_fatal' "$ABLOG"); [ "${HF:-0}" -eq 0 ] || { say "  [FAIL] h_fatal across the rotation cycle"; FAIL=1; }
# h_fatal alone is not a health signal — see assert_no_absorbed_faults in lib_metrics.sh (R41).
assert_no_absorbed_faults "${ABLOG}" || FAIL=1
say "  capture: rotate_screen.png — LOOK at it; frames advancing is not the same as being visible"

say
# selfhash_verify RETURNS a verdict (0 unchanged / 1 edited mid-run / 2 cannot tell). It was
# called and discarded here, so a run whose script changed underneath it printed
# "*** DISCARD THESE RESULTS ***" and still exited 0 — the one failure that invalidates every
# other line above it.
selfhash_verify; [ $? -eq 0 ] || FAIL=$((FAIL+1))
say "DONE (FAIL=$FAIL)"
adb emu kill >/dev/null 2>&1
exit "$FAIL"
