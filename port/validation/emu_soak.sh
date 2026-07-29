#!/bin/bash
# emu_soak.sh — does it stay healthy over a LONG session, and does memory grow without bound?
#
# WHY THIS EXISTS
# ---------------
# Every run in this project is 5-10 minutes and stops at level 2. The longest any capture reached is
# frame[6301]. Nothing samples memory over time at all — `grep -l meminfo port/validation/*.sh`
# returns nothing. So "it plays" is established and "it keeps playing" is not, which is the half a
# user actually experiences.
#
# It matters more here than for a normal app because the guest heap is this project's own
# implementation (galloc, inside Unicorn's address space), and it has had a real corruption bug:
# galloc_check reported -5 permanently, 383 of 384 checks failing, traced to the allocator returning
# 8 fewer usable bytes than Android for a family of small sizes. A leak or slow corruption that only
# shows after twenty minutes would be invisible to every existing test.
#
# WHAT IT MEASURES, per sample
#   frame[N]   — must keep advancing, or the game has stalled even if the process is alive
#   VmRSS      — the process's resident memory, straight from /proc/<pid>/status
#   h_fatal    — must stay 0 for the whole session, not just at the end
#
# The RSS assertion is deliberately loose (end < 2x the first steady sample) because no baseline
# existed before this script: the point of the first run is to ESTABLISH the trend, and a threshold
# invented ahead of the data would either fire on correct behaviour or never fire at all. The raw
# samples are printed so the shape can be read rather than only the verdict.
#
#   docker run --rm --network none --device /dev/kvm --group-add "$(getent group kvm|cut -d: -f3)" \
#       -v "$PWD":/work ab-emu-34 bash /work/port/validation/emu_soak.sh
#
# SOAK_MINUTES=20 makes it longer. Default 10 keeps a full run near 15 minutes.
set +e
source "$(dirname "$0")/lib_settle.sh"
source "$(dirname "$0")/lib_dialogs.sh"
source "$(dirname "$0")/lib_metrics.sh"
source "$(dirname "$0")/lib_install.sh"
source "$(dirname "$0")/lib_selfhash.sh"

APK=/work/out/angrybirds-8.0.3-x86shim-release.apk   # the SHIPPING configuration, not a diagnostic build
PKG=com.rovio.angrybirds
MINUTES="${SOAK_MINUTES:-10}"
OUT=/work/reports/shots; mkdir -p "$OUT"; LOG="$OUT/soak.txt"; : >"$LOG"
ABLOG="$OUT/soak_abshim.txt"; : >"$ABLOG"
say(){ echo "$@" | tee -a "$LOG"; }
selfhash_begin
FAIL=0

[ -f "$APK" ] || { say "[FAIL] missing $APK"; exit 1; }
say "  soaking $(basename "$APK") for ${MINUTES} minutes of play"

say "== boot =="
emulator -avd "${ABSHIM_AVD:-abtest34}" -no-window -no-audio -no-boot-anim -no-snapshot -accel on \
    -gpu swiftshader_indirect >/tmp/emu.log 2>&1 &
adb wait-for-device
for i in $(seq 1 120); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = "1" ] && break; sleep 5; done
say "  android $(adb shell getprop ro.build.version.release 2>/dev/null|tr -d '\r') (API $(adb shell getprop ro.build.version.sdk 2>/dev/null|tr -d '\r'))"
adb shell settings put global airplane_mode_on 1 >/dev/null 2>&1

install_apk "$APK" 4 2>&1 | tee -a "$LOG"
adb shell pm list packages 2>/dev/null | grep -q rovio || { say "[FAIL] install"; adb emu kill; exit 1; }
adb logcat -c >/dev/null 2>&1; adb logcat -G 64M >/dev/null 2>&1
adb logcat -s abshim > "$ABLOG" 2>/dev/null &
adb shell monkey -p "$PKG" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1

say "== reach steady play before sampling =="
for s in $(seq 1 110); do sleep 5
    fn=$(grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null|tail -1|grep -oE '[0-9]+')
    [ -n "$fn" ] && [ "$fn" -ge 601 ] && { say "  rendering at ~$((s*5))s frame[$fn]"; break; }
done
dismiss_system_dialogs
adb shell input tap 390 266; sleep 4; adb shell input tap 390 266; sleep 8

PID=$(adb shell pidof "$PKG" 2>/dev/null | tr -d '\r')
[ -n "$PID" ] || { say "[FAIL] no pid — the app is not running, nothing to soak"; adb emu kill; exit 1; }
say "  pid $PID"

say
say "== soak: play and sample once a minute =="
say "  min  frame      VmRSS(kB)  h_fatal"
PREVF=0; FIRSTRSS=""; LASTRSS=""; STALLS=0
for m in $(seq 1 "$MINUTES"); do
    # keep it playing: a slingshot drag, then advance past whatever card appears
    for k in 1 2 3; do adb shell input swipe 207 118 110 150 700 >/dev/null 2>&1; sleep 6; done
    adb shell input tap 390 266 >/dev/null 2>&1; sleep 4
    adb shell input tap 500 300 >/dev/null 2>&1; sleep 8

    F=$(grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null|tail -1|grep -oE '[0-9]+')
    R=$(adb shell "cat /proc/$PID/status 2>/dev/null | grep VmRSS" 2>/dev/null | tr -d '\r' | awk '{print $2}')
    HF=$(grep -ac 'h_fatal' "$ABLOG" 2>/dev/null)
    printf "  %3s  %-9s  %-9s  %s\n" "$m" "${F:-?}" "${R:-?}" "${HF:-?}" | tee -a "$LOG"

    [ -n "$R" ] && { [ -z "$FIRSTRSS" ] && FIRSTRSS="$R"; LASTRSS="$R"; }
    if [ -n "$F" ] && [ "$F" -le "$PREVF" ]; then STALLS=$((STALLS+1)); fi
    [ -n "$F" ] && PREVF="$F"
    if [ "${HF:-0}" -ne 0 ]; then say "  [FAIL] h_fatal appeared at minute $m — the shim crashed mid-session"; FAIL=1; break; fi
done

say
say "== RESULTS =="
say "  h_fatal over the whole session: $(h_fatal_report "$ABLOG")"
ALIVE=$(adb shell pidof "$PKG" 2>/dev/null | tr -d '\r')
say "  still alive at the end: ${ALIVE:-<no>}"
[ -n "$ALIVE" ] || { say "  [FAIL] the process died during the soak"; FAIL=1; }

if [ "$STALLS" -gt 1 ]; then
    say "  [FAIL] frames failed to advance in $STALLS sample(s) — the game stalled while alive,"
    say "         which no existing test would have caught (they all stop within ~10 minutes)"
    FAIL=1
else
    say "  [ OK ] frames advanced in every sample but $STALLS (a single flat sample is a level load)"
fi

if [ -n "$FIRSTRSS" ] && [ -n "$LASTRSS" ] && [ "$FIRSTRSS" -gt 0 ]; then
    PCT=$(( LASTRSS * 100 / FIRSTRSS ))
    say "  RSS ${FIRSTRSS}kB -> ${LASTRSS}kB over ${MINUTES} min = ${PCT}% of the first sample"
    if [ "$PCT" -gt 200 ]; then
        say "  [FAIL] resident memory more than DOUBLED during steady play — that is a leak shape"
        FAIL=1
    else
        say "  [ OK ] resident memory stayed under 2x the first steady sample"
    fi
else
    say "  [FAIL] could not read VmRSS — memory growth was NOT measured, so this run says nothing"
    say "         about it (a missing measurement must not read as a clean one)"
    FAIL=1
fi

say
selfhash_verify
say "DONE (FAIL=$FAIL)"
adb emu kill >/dev/null 2>&1
exit "$FAIL"
