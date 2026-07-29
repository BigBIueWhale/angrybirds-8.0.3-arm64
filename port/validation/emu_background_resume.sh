#!/bin/bash
# emu_background_resume.sh — does it survive being backgrounded and brought back?
#
# WHY THIS EXISTS
# ---------------
# Pressing Home mid-game and returning is the single most common thing a user does to a running app,
# and nothing here had ever tested it. Every play run in this rig launches, plays and ends; none
# leaves and comes back.
#
# It is a real risk for THIS port specifically. Android tears the EGL surface down when an activity
# stops and hands back a NEW one on resume, and `grep -rn 'surfaceDestroyed\|eglMakeCurrent'
# port/shim/src/` finds nothing: the shim passes GL through and the surface lifecycle belongs to the
# engine's Java side. Whether the emulated ARM32 renderer copes with a surface swap — rather than
# drawing into a dead one and showing black, or aborting — is a question the shim's own source cannot
# answer. The original design notes list "EGL context lifecycle" as a top hazard for exactly this.
#
# HOW IT AVOIDS FOOLING ITSELF. Three states are measured, not two:
#   before   frames must be ADVANCING, or "it resumed" would be a claim about a stalled app
#   during   frames must STOP (or very nearly), which is what proves it actually went to background —
#            a Home key that did nothing would otherwise look like a perfect resume
#   after    frames must advance AGAIN, from a higher number, with h_fatal still 0
#
# A screenshot is taken after the resume because "frames advance" and "the game is visible" are
# different claims: a renderer drawing into a destroyed surface can keep counting frames happily
# while the user sees black.
#
#   docker run --rm --network none --device /dev/kvm --group-add "$(getent group kvm|cut -d: -f3)" \
#       -v "$PWD":/work -e ABSHIM_AVD=ab36 ab-emu-36 bash /work/port/validation/emu_background_resume.sh
set +e
source "$(dirname "$0")/lib_settle.sh"
source "$(dirname "$0")/lib_dialogs.sh"
source "$(dirname "$0")/lib_metrics.sh"
source "$(dirname "$0")/lib_install.sh"
source "$(dirname "$0")/lib_camera.sh"
source "$(dirname "$0")/lib_wincheck.sh"
source "$(dirname "$0")/lib_selfhash.sh"

APK=/work/out/angrybirds-8.0.3-x86shim-release.apk
PKG=com.rovio.angrybirds
OUT=/work/reports/shots; mkdir -p "$OUT"; LOG="$OUT/bg_resume.txt"; : >"$LOG"
ABLOG="$OUT/bg_resume_abshim.txt"; : >"$ABLOG"
say(){ echo "$@" | tee -a "$LOG"; }
frames(){ grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null | tail -1 | grep -oE '[0-9]+'; }
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

install_apk "$APK" 4 2>&1 | tee -a "$LOG"
adb shell pm list packages 2>/dev/null | grep -q rovio || { say "[FAIL] install"; adb emu kill; exit 1; }

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
F_A=$(frames); sleep 12; F_B=$(frames)
say "  pid $PID1   frames $F_A -> $F_B over 12s"
if [ -z "$F_A" ] || [ -z "$F_B" ] || [ "$F_B" -le "$F_A" ]; then
    say "  [FAIL] frames were not advancing BEFORE backgrounding — 'it resumed' would mean nothing"
    adb emu kill; exit 1
fi
say "  [ OK ] advancing before the test"

say
say "== 2. send it to the background (HOME) =="
adb shell input keyevent 3 >/dev/null 2>&1        # KEYCODE_HOME
sleep 15
F_C=$(frames); sleep 12; F_D=$(frames)
PID2=$(adb shell pidof "$PKG" 2>/dev/null | tr -d '\r')
say "  frames while backgrounded: $F_C -> $F_D over 12s   pid now: ${PID2:-<gone>}"
# If frames keep climbing at full rate the app never actually stopped, and everything after this
# would be measuring an app that was in the foreground the whole time.
DELTA=$(( ${F_D:-0} - ${F_C:-0} ))
if [ "$DELTA" -gt 60 ]; then
    say "  [FAIL] frames advanced by $DELTA while supposedly backgrounded — HOME did not take effect,"
    say "         so a 'successful resume' below would be an app that never left the foreground"
    FAIL=1
else
    say "  [ OK ] rendering stopped/idled while backgrounded (+$DELTA frames)"
fi

say
say "== 3. bring it back =="
adb shell monkey -p "$PKG" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
sleep 10
# Dismiss anything the system put up ON RESUME. The first run of this script stalled here and the
# capture showed why: Android's POST_NOTIFICATIONS prompt appears when the app returns to the
# foreground, holds focus, and stops the activity resuming — indistinguishable from a dead renderer
# if you only read frame counters.
dismiss_system_dialogs
sleep 10
PID3=$(adb shell pidof "$PKG" 2>/dev/null | tr -d '\r')
F_E=$(frames); sleep 15; F_F=$(frames)
say "  pid after resume: ${PID3:-<none>}   frames $F_E -> $F_F over 15s"
if [ -z "$PID3" ]; then
    say "  [FAIL] the app is not running after being brought back"
    adb logcat -d '*:E' 2>/dev/null | grep -aiE "rovio|EGL|surface|abort" | tail -6 | sed 's/^/    /' | tee -a "$LOG"
    FAIL=1
elif [ -z "$F_F" ] || [ "$F_F" -le "$F_E" ]; then
    say "  [FAIL] frames are NOT advancing after the resume — the renderer did not recover the surface"
    FAIL=1
else
    say "  [ OK ] rendering resumed (+$(( F_F - F_E )) frames in 15s)"
fi
[ -n "$PID1" ] && [ "$PID1" = "$PID3" ] && say "  (same process throughout — pid $PID1, so this was a real resume, not a relaunch)" \
                                        || say "  (process was recreated: $PID1 -> ${PID3:-none} — Android restarted it rather than resuming)"

adb exec-out screencap -p > "$OUT/bg_resume_screen.png" 2>/dev/null
say "  h_fatal: $(h_fatal_report "$ABLOG")"
HF=$(grep -ac 'h_fatal' "$ABLOG"); [ "${HF:-0}" -eq 0 ] || { say "  [FAIL] h_fatal across the background/resume cycle"; FAIL=1; }
say "  capture: bg_resume_screen.png — LOOK at it; counting frames is not the same as being visible"

say
selfhash_verify
say "DONE (FAIL=$FAIL)"
adb emu kill >/dev/null 2>&1
exit "$FAIL"
