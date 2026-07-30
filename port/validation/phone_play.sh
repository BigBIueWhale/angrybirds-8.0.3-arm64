#!/bin/bash
# phone_play.sh — drive real multi-level play on the physically connected A56 and capture it CLEANLY.
#
# WHY THIS EXISTS
# ---------------
# The x86 proxy has played six sequential levels (cont.212-213). The phone had one win plus an advance
# into level 2 (R50) and a second win found during the soak (R54). This closes that gap on the real
# hardware, and it exists as a script rather than ad-hoc adb commands because everything else in this
# rig does: an experiment that lives only in a shell history is an experiment nobody can repeat.
#
# THE CAPTURE DISCIPLINE THAT R50 PAID FOR
# ----------------------------------------
# `adb logcat` re-dumps the entire ring buffer unless you clear it or pass -T. R50's evidence was seven
# appended captures, 4.33x duplicated, and its counts were wrong in the record before anything caught
# it. So this clears once and runs ONE continuous capture, then verifies its own log with
# log_recapture_audit.py before reporting any count from it. A run whose log is not monotonic reports
# nothing.
#
# WHAT IS ASSERTED MECHANICALLY, AND WHAT IS NOT
# ---------------------------------------------
# Asserted: the process stays alive and keeps the same pid; h_fatal and the other fault markers stay 0;
# every screenshot passes png_sane.py; the log is a single capture; capped counters are named as floors;
# and, ACROSS THE WHOLE RUN, that frame heartbeats advanced and the surface changed.
#
# Deliberately NOT asserted per cycle, because both versions of that failed a healthy run:
#   * heartbeat advance — `frame[N]` fires every 300 frames (R38) and this device runs at ~6 fps
#     (R51/R54), so one heartbeat takes ~50s while a cycle takes ~38s. A cycle containing zero
#     heartbeats is normal; asserting otherwise failed on arithmetic, not on evidence.
#   * screenshots differing — a settled results card or paused level legitimately yields byte-identical
#     PNGs. Only EVERY capture being identical means the surface never changed.
#
# NOT asserted: that a level was won. `win_detect.py`'s thresholds are calibrated for 640x320 emulator
# captures and do NOT transfer to 2340x1080 (cont.264 — it scored an unmistakable LEVEL CLEARED screen
# as "not a win"). Pointing it at device screenshots would produce a confident wrong verdict, which is
# worse than no verdict. Wins are established by EYE from the numbered captures, which is how PROOF_22-28
# were established. This script says so rather than implying otherwise.
#
#   bash port/validation/phone_play.sh [cycles]      # default 6; USB only, no network
#
# SECURITY: adb must be listening on 127.0.0.1 only (this box has a public IPv4 on all interfaces).
# Asserted below; the script refuses to run otherwise.
set +e
# Refuse to run while another evidence run is in flight: 50 scripts share reports/shots and
# two concurrent runs BLEND their logs into numbers that look normal and mean nothing.
source "$(dirname "$0")/lib_runlock.sh"
acquire_run_lock "$(basename "$0")" || exit 1
cd "$(dirname "$0")/../.." || exit 1
source port/validation/lib_metrics.sh
# lib_camera.sh owns the camera reset. This script first reimplemented it with its own hardcoded
# device coordinates, and verify_claims.sh caught that with a check written for exactly this defect
# ("a script that shoots across rounds resets the camera first"). The library documents the whole
# mechanism - a drag that misses the bird PANS instead, so repeating it walks the view off the level -
# and it even records that a vertical sweep was tried, measured worse, and reverted. Standing rule 1.
source port/validation/lib_camera.sh
CYCLES=${1:-6}
PKG=com.rovio.angrybirds
OUT=reports/shots/phone_play; mkdir -p "$OUT"
S=${ABSHIM_TMP:-/tmp}
LOGF="$OUT/phone_play_abshim.txt"
RUN="$OUT/phone_play_run.txt"; : > "$RUN"
say(){ echo "$@" | tee -a "$RUN"; }
FAIL=0
ok(){  say "  [ OK ] $1"; }
bad(){ say "  [FAIL] $1"; FAIL=1; }

# The run log and the analysed log must never be the same file — the lib guards against it, and the
# reason is that a verdict line saying "h_fatal" would then count as an h_fatal.
[ "$RUN" != "$LOGF" ] || { echo "harness error: run log == abshim log"; exit 1; }

# ABSHIM_ADB, or whatever is on PATH. Deliberately NOT a hardcoded absolute path: verify_claims.sh
# rejects a build-host path in a tracked file, and rightly - this repo is public and the path named
# this box's home directory.
if ! command -v adb >/dev/null 2>&1; then
  for cand in "${ABSHIM_ADB:-}" "$HOME/Android/Sdk/platform-tools/adb" "$HOME/android-sdk/platform-tools/adb"; do
    [ -n "$cand" ] && [ -x "$cand" ] && { PATH="$(dirname "$cand"):$PATH"; export PATH; break; }
  done
fi
command -v adb >/dev/null 2>&1 || { say "FATAL: no adb on PATH"; exit 1; }

say "== SECURITY PRECONDITION =="
LISTEN=$(ss -ltnp 2>/dev/null | grep -a 5037)
say "  $(printf '%s' "$LISTEN" | tr -s ' ' | cut -c1-72)"
if printf '%s' "$LISTEN" | grep -qE '0\.0\.0\.0:5037|\[::\]:5037'; then
  bad "adb is listening on a wildcard address — refusing to run (this box has a public IPv4)"
  say "DONE (FAIL=1)"; exit 1
fi
ok "adb is bound to loopback only (USB transport)"

say
say "== device =="
N=$(adb devices | grep -acE '\sdevice$')
[ "${N:-0}" -eq 1 ] || { bad "expected exactly 1 device, found ${N:-0}"; say "DONE (FAIL=1)"; exit 1; }
MODEL=$(adb shell getprop ro.product.model 2>/dev/null | tr -d '\r')
SDK=$(adb shell getprop ro.build.version.sdk 2>/dev/null | tr -d '\r')
ABIS=$(adb shell getprop ro.product.cpu.abilist 2>/dev/null | tr -d '\r')
say "  $MODEL  API $SDK  abilist=$ABIS"
case "$ABIS" in *armeabi*) bad "this device has 32-bit ARM support, so it is NOT the A56 premise" ;; esac

PID0=$(adb shell pidof "$PKG" 2>/dev/null | tr -d '\r')
if [ -z "$PID0" ]; then
  say "  app not running — launching"
  adb shell monkey -p "$PKG" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
  sleep 25
  source port/validation/lib_dialogs.sh 2>/dev/null && dismiss_system_dialogs "$OUT/00_afterdialogs.png" >/dev/null 2>&1
  sleep 5
  PID0=$(adb shell pidof "$PKG" 2>/dev/null | tr -d '\r')
fi
[ -n "$PID0" ] || { bad "the app is not running and would not start"; say "DONE (FAIL=1)"; exit 1; }
AGE0=$(adb shell ps -o ETIME -p "$PID0" 2>/dev/null | tail -1 | tr -d ' \r')
say "  running as pid $PID0 (age $AGE0)"

say
say "== ONE clean capture (clear first; see R50) =="
adb logcat -c >/dev/null 2>&1
adb logcat -G 64M >/dev/null 2>&1
adb logcat -s abshim > "$LOGF" 2>/dev/null &
LPID=$!
sleep 3
say "  capturing to $LOGF"

# DEVICE-SPACE COORDINATES, all measured by reading real captures at 2340x1080 (landscape).
#
# The emulator-scaled vector recorded in cont.264 ("860 365 506 473 700") launches the bird on the
# FIRST tutorial level and nowhere else, and the reason is worth writing down because it wasted two
# runs:
#
#   * A level OPENS with the camera on the TARGET area, not on the slingshot. There is no bird under
#     a fixed slingshot coordinate at that moment.
#   * That vector is a RIGHT-TO-LEFT swipe. On empty ground it is not a slingshot pull at all — it
#     SCROLLS THE CAMERA further right, toward the pigs and away from the slingshot. So each cycle
#     pushed the view further from the bird while the log stayed perfectly healthy: h_fatal 0,
#     heartbeats climbing, SCORE 0. Another "healthy log, nothing happening" trap.
#
# What actually works, verified on Tutorial_bomb_niko (4 pigs, 3 towers) with the result captured in
# PROOF_29/PROOF_30:
#   1. pan_to_slingshot from lib_camera.sh - a rightward finger sweep moves the VIEW left toward the
#      launch point, and the camera clamps at the wall so over-panning is free. Now resolution-scaled:
#      the library's emulator-space (80,150)->(600,150) becomes (292,506)->(2193,506) on this panel,
#      which is within a few percent of the vector found empirically here.
#   2. pull BACK from the bird itself at (948,667) to (700,790) - down-and-left, which launches
#      up-and-right
#   3. tap the fast-forward button to settle the level instead of waiting out the animation
NEXT_X=1419; NEXT_Y=863          # NEXT on the results screen
BIRD_DRAG="948 667 700 790 700"  # pull back from the loaded bird -> launches up and right
FF_X=2241; FF_Y=971              # fast-forward, on the settling/results screen

# Clear PREVIOUS captures. Without this, `png_sane.py "$OUT"/c*.png` and any by-eye review at the end
# mix this run's images with the last run's: a 3-cycle run was validating c05/c06 files left by a
# 6-cycle run, so the report described frames the run never took. Old evidence presented as current is
# the same defect as a stale PROOF, which this project has already been bitten by twice.
rm -f "$OUT"/c*.png
shot(){ adb exec-out screencap -p > "$OUT/$1.png" 2>/dev/null; }
frame_now(){ grep -aoE 'frame\[[0-9]+\]' "$LOGF" 2>/dev/null | tail -1 | grep -oE '[0-9]+'; }

say
say "== $CYCLES play cycles =="
PREV=""; IDENT=0; FRAME_FIRST=""; FRAME_LAST=""
for c in $(seq 1 "$CYCLES"); do
  F0=$(frame_now)
  # From a results screen NEXT advances; mid-level the same tap lands on background and is harmless.
  adb shell input tap "$NEXT_X" "$NEXT_Y" >/dev/null 2>&1
  sleep 6
  # A level OPENS with the camera on the target area, not on the slingshot, so a fixed drag vector
  # lands on empty sky. The first run of this script proved it: Tutorial_bomb_niko loaded (4 pigs,
  # 3 towers) and ended with SCORE 0 and every structure intact - the drags never touched a bird.
  # A tap during the intro pan skips it and settles the camera on the slingshot.
  adb shell input tap 1170 540 >/dev/null 2>&1
  sleep 4
  shot "$(printf 'c%02d_a_level' "$c")"
  # Up to 4 birds per level. Re-scroll to the slingshot BEFORE each shot: the camera follows the bird
  # downrange after every launch, so a vector that worked for bird 1 misses bird 2 entirely.
  for b in 1 2 3 4; do
    # Reset the camera BEFORE each shot: it follows the previous bird downrange, so a vector that
    # worked for bird 1 misses bird 2 entirely. pan_and_capture also records what we are about to
    # shoot at, because "the drag missed the bird" and "the view was on empty sky" produce identical
    # logs and are one glance apart with the image.
    pan_and_capture "$OUT/$(printf 'c%02d_pre%d' "$c" "$b").png"
    adb shell input swipe $BIRD_DRAG >/dev/null 2>&1
    sleep 12
  done
  # Settle the level rather than waiting out the animation; harmless if not present.
  adb shell input tap "$FF_X" "$FF_Y" >/dev/null 2>&1
  sleep 12
  shot "$(printf 'c%02d_b_after' "$c")"
  F1=$(frame_now)
  P=$(adb shell pidof "$PKG" 2>/dev/null | tr -d '\r')
  say "  cycle $c: frame $F0 -> $F1   pid $P"
  [ -z "$FRAME_FIRST" ] && FRAME_FIRST="$F0"
  [ -n "$F1" ] && FRAME_LAST="$F1"
  [ "$P" = "$PID0" ] || bad "cycle $c: pid changed $PID0 -> $P (the process restarted, i.e. it crashed)"
  # NO per-cycle heartbeat assertion. `frame[N]` is emitted every 300 frames (R38), and this device
  # runs at ~6 fps (R51/R54), so one heartbeat takes ~50s while a cycle takes ~38s. A cycle spanning
  # zero heartbeats is therefore NORMAL, and asserting otherwise failed a healthy run on arithmetic
  # alone. Advance is asserted ACROSS THE RUN instead, where the window is long enough to be valid.
  if [ -n "$F0" ] && [ -n "$F1" ] && [ "$F1" -lt "$F0" ]; then
    bad "cycle $c: heartbeat went BACKWARDS ($F0 -> $F1), which cannot happen in one process"
  fi
  # Byte-identical captures. A settled screen (results card, paused level) legitimately produces
  # identical PNGs, so this is NOT a per-cycle failure either; it is only alarming if EVERY capture
  # is identical, i.e. the surface never changes at all. Counted here, judged after the loop.
  CUR=$(sha256sum "$OUT/$(printf 'c%02d_b_after' "$c").png" 2>/dev/null | cut -d' ' -f1)
  if [ -n "$PREV" ] && [ "$CUR" = "$PREV" ]; then
    IDENT=$((IDENT+1))
    say "         (capture identical to the previous cycle — settled screen, not itself a fault)"
  fi
  PREV="$CUR"
done

sleep 3
kill $LPID 2>/dev/null
wait $LPID 2>/dev/null

say
say "== whole-run assertions (windows long enough to be valid) =="
if [ -n "$FRAME_FIRST" ] && [ -n "$FRAME_LAST" ] && [ "$FRAME_LAST" -gt "$FRAME_FIRST" ]; then
  ok "heartbeats advanced across the run: frame[$FRAME_FIRST] -> frame[$FRAME_LAST]"
else
  bad "heartbeats did not advance across the WHOLE run (${FRAME_FIRST:-?} -> ${FRAME_LAST:-?}) — the renderer really is stalled"
fi
if [ "$IDENT" -ge "$CYCLES" ]; then
  bad "every capture was byte-identical — the surface never changed, so nothing was playing"
else
  ok "the surface changed during the run ($IDENT of $CYCLES captures repeated a settled screen)"
fi

say
say "== the log must be ONE capture before any count from it is quoted =="
if python3 port/validation/log_recapture_audit.py "$LOGF" | tee -a "$RUN" | grep -q 'RECAPTURE'; then
  bad "the capture is not monotonic — refusing to report counts from it"
  say "DONE (FAIL=1)"; exit 1
fi
TOT=$(grep -ac . "$LOGF"); UQ=$(sort -u "$LOGF" | grep -ac .)
say "  lines $TOT, unique $UQ"
[ "${TOT:-0}" -gt 200 ] || bad "only ${TOT:-0} log lines — the app was barely executing, so the zeros below prove little"

say
say "== faults (real totals only if nothing is at its cap — checked next) =="
for m in h_fatal uaf-survive St11logic_error UNIMPL; do
  n=$(grep -ac "$m" "$LOGF"); printf "  %-18s %s\n" "$m" "$n" | tee -a "$RUN"
  [ "${n:-0}" -eq 0 ] || bad "$m fired $n time(s)"
done
say "  floors: $(saturated_report "$LOGF")"

say
say "== level files loaded during this run =="
grep -aoE 'data/levels/[A-Za-z0-9_]+/[A-Za-z0-9_]+' "$LOGF" 2>/dev/null | sort | uniq -c | tee -a "$RUN" | sed 's/^/  /'
NL=$(grep -aoE 'data/levels/[A-Za-z0-9_]+/[A-Za-z0-9_]+' "$LOGF" 2>/dev/null | sort -u | grep -ac .)
say "  distinct level files: ${NL:-0}"

say
say "== every capture must be a real frame =="
python3 port/validation/png_sane.py "$OUT"/c*.png 2>&1 | tail -3 | tee -a "$RUN" | sed 's/^/  /'
python3 port/validation/png_sane.py "$OUT"/c*.png >/dev/null 2>&1 || bad "a capture failed png sanity"

say
say "== NOT ASSERTED HERE =="
say "  Whether a level was WON. win_detect.py is calibrated for 640x320 emulator captures and scored a"
say "  real LEVEL CLEARED screen as 'not a win' at device resolution (cont.264). Wins are read by eye"
say "  from $OUT/c*_b_after.png, which is how PROOF_22-28 were established."
say "DONE (FAIL=$FAIL)"
exit "$FAIL"
