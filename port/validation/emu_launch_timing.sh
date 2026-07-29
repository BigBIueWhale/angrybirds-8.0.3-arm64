#!/bin/bash
# emu_launch_timing.sh — DIAGNOSTIC: which frame offset after release actually shows the launch?
#
# WHY
# ---
# emu_interactive_capture.sh regenerates the source images for PROOF_2/3/4 and times two of its
# captures with `sleep 2` and `sleep 3` after the slingshot release. That is wall-clock against a
# frame rate measured at ~2-16 fps under SwiftShader — the same defect that was already fixed twice
# in this tree (`sleep 14` -> settle_frames), still present in the capture whose whole point is to
# catch a specific instant.
#
# Re-running it confirmed the consequence rather than predicting it: the frame named
# "bird_launched" showed the bird ALREADY IMPACTING (score popup 5000, debris flying), and the
# "mid-flight" frame showed settled wreckage. Both are real gameplay; neither is the moment named.
#
# The hypothesis was that the unit should be FRAMES, because physics advance per frame: "+N frames
# after release" is the same game moment on a fast host and a slow one, while "+2 seconds" is not.
# So this captured a burst at +2/4/6/9/13/18/25 frames in one run, to pick N by looking.
#
# RESULT (2026-07-29): THE HYPOTHESIS IS WRONG, and the experiment is what showed it. Every one of
# the seven captures reported the SAME frame[1201], and six of the seven files came out byte-identical
# in size. The reason is in the shim:
#
#     jni_entry.c:664   if((++rf % 300u)==1u){ LOG("frame[%u] GL draws=%lu ...
#
# `frame[N]` is a **heartbeat emitted every 300th frame**, not a per-frame counter. Measured on the
# run: the only values logged were 1, 301, 601, 901, 1201 — every gap exactly 300. So a frame-based
# wait cannot resolve anything shorter than 300 frames, and a bird's flight is far shorter than that.
# "+2 frames" and "+25 frames" are the same instruction: wait for the next heartbeat.
#
# CONSEQUENCE, recorded so this is not attempted again: emu_interactive_capture.sh's `sleep 2` /
# `sleep 3` CANNOT simply be swapped for frame counts. Making it frame-exact would mean logging every
# frame in the shim — a change to shipping code, ~300x the log volume, for a diagnostic capture — which
# is not worth it. The captures stay wall-clock, they stay timing-sensitive, and the script says so in
# its own output and requires a human to look before anything is promoted to a PROOF_ name.
# (settle_frames' +120 is unaffected: it only ever needs "one more heartbeat", which is what it gets.)
#
#   docker run --rm --network none --device /dev/kvm --group-add "$(getent group kvm|cut -d: -f3)" \
#       -v "$PWD":/work ab-emu bash /work/port/validation/emu_launch_timing.sh
set +e
source "$(dirname "$0")/lib_settle.sh"
source "$(dirname "$0")/lib_metrics.sh"
source "$(dirname "$0")/lib_selfhash.sh"
( sleep 2100; adb emu kill 2>/dev/null; pkill -9 -f qemu 2>/dev/null ) &
APK=/work/out/angrybirds-8.0.3-x86shim.apk
OUT=/work/reports/shots; mkdir -p "$OUT"
LOG="$OUT/launch_timing.txt"; : >"$LOG"
ABLOG="$OUT/launch_timing_abshim.txt"; : >"$ABLOG"
say(){ echo "$@" | tee -a "$LOG"; }
selfhash_begin
FAIL=0
rm -f "$OUT"/launchts_*.png          # a stale burst must not be mistaken for this run's

say "== boot (API 25) =="
emulator -avd abtest -no-window -no-audio -no-boot-anim -no-snapshot -accel on \
         -gpu swiftshader_indirect -partition-size 6144 -wipe-data >/tmp/emu.log 2>&1 &
adb wait-for-device
for i in $(seq 1 150); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = 1 ] && break; sleep 2; done
sleep 8
adb shell settings put global airplane_mode_on 1 >/dev/null 2>&1
adb push "$APK" /data/local/tmp/ab.apk >/dev/null 2>&1
INST=fail
for t in 1 2 3 4; do adb shell pm install -r -d /data/local/tmp/ab.apk 2>&1 | grep -q Success && { INST=ok; break; }; sleep 4; done
say "install=$INST"
[ "$INST" = ok ] || { say "  [FAIL] install failed"; say "DONE (FAIL=1)"; adb emu kill; exit 1; }

adb logcat -c >/dev/null 2>&1; adb logcat -G 32M >/dev/null 2>&1
adb logcat -s abshim > "$ABLOG" 2>/dev/null &
adb shell monkey -p com.rovio.angrybirds -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
say "== wait for the tutorial card =="
for s in $(seq 1 130); do sleep 5
  fn=$(grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null|tail -1|grep -oE '[0-9]+')
  [ -n "$fn" ] && [ "$fn" -ge 601 ] && { say "  card at ~$((s*5))s frame[$fn]"; break; }
done
sleep 4
adb shell input tap 390 266; sleep 4; adb shell input tap 390 266
settle_frames "$ABLOG" 60 180

fnow(){ grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null | tail -1 | grep -oE '[0-9]+'; }

# Wait until the frame counter has advanced by $1 from $2, capped at $3 seconds. Deliberately NOT
# settle_frames: that waits for a QUIET period (no growth), which is the opposite of what a burst
# through a moving scene needs.
wait_delta(){   # $1=frames to advance  $2=baseline frame  $3=cap secs
    local want="$1" base="$2" cap="${3:-40}" t=0 n
    while [ "$t" -lt "$cap" ]; do
        n=$(fnow); [ -n "$n" ] && [ $((n - base)) -ge "$want" ] && { echo "$n"; return 0; }
        sleep 1; t=$((t+1))
    done
    echo "${n:-0}"; return 1
}

say
say "== release, then capture at several FRAME offsets =="
BASE=$(fnow)
say "  frame at release: ${BASE:-none}"
[ -n "$BASE" ] || { say "  [FAIL] no frame counter — cannot measure offsets"; say "DONE (FAIL=1)"; adb emu kill; exit 1; }
adb shell input swipe 207 118 110 150 700
for d in 2 4 6 9 13 18 25; do
    got=$(wait_delta "$d" "$BASE" 60)
    adb exec-out screencap -p > "$OUT/launchts_+${d}f.png" 2>/dev/null
    say "  +${d} frames -> frame[$got]  $(wc -c < "$OUT/launchts_+${d}f.png" 2>/dev/null) bytes"
done

say
say "== are they all real frames? =="
python3 "$(dirname "$0")/png_sane.py" --expect 640x320 "$OUT"/launchts_*.png > /tmp/s.$$ 2>&1
SANE=$?
while IFS= read -r _l; do say "$_l"; done < /tmp/s.$$; rm -f /tmp/s.$$
[ "$SANE" -eq 0 ] || FAIL=$((FAIL+1))
PID=$(adb shell pidof com.rovio.angrybirds 2>/dev/null|tr -d '\r')
[ -n "$PID" ] && say "  [ OK ] app alive (pid $PID)" || { say "  [FAIL] app died during the burst"; FAIL=$((FAIL+1)); }
HF=$(grep -ac 'h_fatal' "$ABLOG"); [ "${HF:-0}" -eq 0 ] && say "  [ OK ] no h_fatal" \
    || { say "  [FAIL] h_fatal during the burst"; FAIL=$((FAIL+1)); }
say
say "  NOW LOOK AT THEM. This script cannot tell which offset shows a bird in flight —"
say "  that is the entire question, and it is a question for a person."
selfhash_verify; [ $? -eq 0 ] || FAIL=$((FAIL+1))
say "DONE (FAIL=$FAIL)"
adb emu kill >/dev/null 2>&1
exit "$FAIL"
