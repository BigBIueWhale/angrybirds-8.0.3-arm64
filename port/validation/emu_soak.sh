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
# WHAT IT DOES NOT SHOW. The input is blind — taps and swipes on a timer — so this measures that
# the renderer and input path keep working, NOT that the game progresses. A 20-minute run opened
# two distinct level files, both in data/levels/0_Tutorial/. Progression is proven by the
# playthrough scripts, which check for a win screen instead of counting frames.
#
# That used to read "it replayed the tutorial", which was a GUESS — this script captured no images,
# so nothing here could say what was on screen. emu_progression.sh later shot the same repeated drag
# and photographed the result: empty sky, SCORE 0, level still loaded. A drag on empty space PANS
# THE CAMERA, so repeating one fixed drag walks the view off the level and every later swipe lands
# on nothing. This soak was almost certainly doing that too, which makes its input weaker than
# "replaying the tutorial" implied.
#
# Two changes follow. The loop pans back to the slingshot before each drag, and it saves a frame at
# the start and end, so a future reader can SEE what was soaked instead of trusting a sentence.
#
# MEASURED with both in place (20 min, API 34): soak_start.png is the tutorial properly framed —
# slingshot, loaded bird, three spares, score 0 — and the run opens TWO distinct level files. So the
# original "it replayed the tutorial" was right after all, for a reason that was never stated: this
# script still shoots from the FIXED anchor (207,118). That wins level 1, whose bird sits there, and
# then cannot win level 2, whose bird is at (152,183). Within each minute the missed drags pan the
# view off the level; the next minute's pan brings it back.
#
# So the input here lands sometimes and not others, by design — this is a stability test, not a
# progression test, and emu_progression.sh is the one that actually plays (it locates the bird per
# shot, reaches four levels). What the soak asserts is unaffected either way: frames, VmRSS and
# h_fatal are read from the process, not from the screen.
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
source "$(dirname "$0")/lib_camera.sh"

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
MONKEY=$(adb shell monkey -p "$PKG" -c android.intent.category.LAUNCHER 1 2>&1 | tr -d '\r')
# Confirm it actually STARTED before waiting ten minutes for frames. A 20-minute soak once sat
# through the entire 550 s render wait and only then reported "no pid", with an empty shim log —
# true, but it took nine minutes to say so and carried no clue why. Check within 60 s and show
# the system's own reason if the process is not there.
for i in $(seq 1 12); do
    P0=$(adb shell pidof "$PKG" 2>/dev/null | tr -d '\r'); [ -n "$P0" ] && break; sleep 5
done
if [ -z "$P0" ]; then
    say "[FAIL] the app did not start within 60s of launch — nothing to soak."
    say "  monkey said: ${MONKEY:-<nothing>}"
    say "  last activity/crash lines:"
    adb logcat -d 2>/dev/null | grep -aiE "rovio|ActivityManager.*(START|died)|FATAL|AndroidRuntime" \
        | tail -8 | sed 's/^/    /' | tee -a "$LOG"
    adb emu kill; exit 1
fi
say "  started as pid $P0"

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
adb exec-out screencap -p > "$OUT/soak_start.png" 2>/dev/null   # what is actually being soaked

say
say "== soak: play and sample once a minute =="
say "  min  frame      VmRSS(kB)  h_fatal"
PREVF=0; FIRSTRSS=""; LASTRSS=""; STALLS=0
for m in $(seq 1 "$MINUTES"); do
    # keep it playing: pan back to the slingshot (see header — otherwise the view drifts off the
    # level and the drags hit nothing), a slingshot drag, then advance past whatever card appears
    pan_to_slingshot 3 1        # lib_camera.sh — one implementation, not a copy per script
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

# Pan BEFORE the end capture. Without this the frame is taken immediately after the minute's three
# drags — which, on any level whose bird is not under the fixed anchor, miss and pan the view into
# the sky. The first end frame captured that way was a screenful of cloud, which reads as "the soak
# was doing nothing" when the next minute's pan would have recovered it. Show where the game IS.
pan_to_slingshot 4 2
adb exec-out screencap -p > "$OUT/soak_end.png" 2>/dev/null

say
say "== RESULTS =="
say "  frames captured to soak_start.png / soak_end.png — LOOK at them before describing this run"
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
    # 130%, tightened from the 200% the first run shipped with. Two measured runs on the shipping
    # config: 10 min 611608->616164 kB (+0.75%) and 20 min 613696->617824 kB (+0.67%). DOUBLING the
    # duration did not increase the growth, which is the shape of no leak — a leak would roughly
    # double it. So the real figure is ~1%, and 130% leaves thirty points of headroom for a longer
    # soak or a heavier level while still catching anything that actually climbs.
    if [ "$PCT" -gt 130 ]; then
        say "  [FAIL] resident memory grew to ${PCT}% of the first steady sample — measured runs sit"
        say "         at ~101%, so this is a leak shape rather than normal variation"
        FAIL=1
    else
        say "  [ OK ] resident memory at ${PCT}% of the first steady sample (measured baseline: ~101%)"
    fi
else
    say "  [FAIL] could not read VmRSS — memory growth was NOT measured, so this run says nothing"
    say "         about it (a missing measurement must not read as a clean one)"
    FAIL=1
fi

say
# selfhash_verify RETURNS a verdict (0 unchanged / 1 edited mid-run / 2 cannot tell). It was
# called and discarded here, so a run whose script changed underneath it printed
# "*** DISCARD THESE RESULTS ***" and still exited 0 — the one failure that invalidates every
# other line above it.
selfhash_verify; [ $? -eq 0 ] || FAIL=$((FAIL+1))
say "DONE (FAIL=$FAIL)"
adb emu kill >/dev/null 2>&1
exit "$FAIL"
