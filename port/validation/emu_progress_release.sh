#!/bin/bash
# DEEPER PROGRESSION test on the EXACT shipping RELEASE config (x86 proxy):
#   boot -> install -> play tutorial to WIN (LEVEL CLEARED) -> tap the orange NEXT button
#   -> confirm a SECOND level loads and RENDERS -> slingshot-drag -> confirm a bird LAUNCHES
#   in level 2. Proves multi-level progression, not just surviving one level-end.
# Screencap space == input space == 640x320 (verified). Orange NEXT button ~ (378,256).
set +e
source "$(dirname "$0")/lib_settle.sh"
source "$(dirname "$0")/lib_provenance.sh"
source "$(dirname "$0")/lib_metrics.sh"   # frame-based settle (replaces flaky fixed sleeps)
( sleep 1650; adb emu kill 2>/dev/null; pkill -9 -f qemu 2>/dev/null ) &
APK=/work/out/angrybirds-8.0.3-x86shim-release.apk
OUT=/work/reports/shots; mkdir -p "$OUT"; LOG="$OUT/progressR.txt"; : >"$LOG"
ABLOG="$OUT/progressR_abshim.txt"; : >"$ABLOG"
say(){ echo "$@" | tee -a "$LOG"; }
fnow(){ grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null | tail -1 | grep -oE '[0-9]+'; }
# Guarded: a bare count returns 0 from an empty log, which reads as "no crash". See lib_metrics.sh.
fatal(){ if [ ! -s "$ABLOG" ]; then printf 'NOT MEASURED (empty log)'; else grep -ac '\[h_fatal\]' "$ABLOG" 2>/dev/null; fi; }

say "== boot =="
emulator -avd abtest -no-window -no-audio -no-boot-anim -no-snapshot -accel on \
         -gpu swiftshader_indirect -partition-size 6144 -wipe-data >/tmp/emu.log 2>&1 &
adb wait-for-device
for i in $(seq 1 150); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = 1 ] && break; sleep 2; done
sleep 8
adb shell settings put global airplane_mode_on 1 >/dev/null 2>&1
adb push "$APK" /data/local/tmp/ab.apk >/tmp/push.log 2>&1
INST=fail; for t in 1 2 3 4; do adb shell pm install -r -d /data/local/tmp/ab.apk 2>&1 | grep -q Success && { INST=ok; break; }; sleep 4; done
say "install=$INST"; [ "$INST" = ok ] || { say DONE; adb emu kill; exit 0; }
record_build "$APK" "progR"
adb logcat -c >/dev/null 2>&1; adb logcat -G 32M >/dev/null 2>&1
adb logcat -s abshim > "$ABLOG" 2>/dev/null &
adb shell monkey -p com.rovio.angrybirds -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1

say "== wait for tutorial card (frame[601]+) =="
for s in $(seq 1 130); do sleep 5
  fn=$(fnow); [ -n "$fn" ] && [ "$fn" -ge 601 ] && { say "  card at ~$((s*5))s (frame[$fn])"; break; }
done
sleep 6

say "== play tutorial to WIN: start card + 3 slingshot drags =="
adb shell input tap 390 266; sleep 4; adb shell input tap 390 266; sleep 12
adb shell input swipe 207 118 110 150 700; sleep 8
adb shell input swipe 207 118 122 140 700; sleep 8
adb shell input swipe 207 118 118 136 700
# FIX (2026-07-27): was `sleep 16` — a fixed wall-clock settle against a frame rate that
# varies ~1.8-15 fps between runs, so it silently captured mid-level on slow runs. See lib_settle.sh.
settle_frames "$ABLOG" 120 300
adb exec-out screencap -p > "$OUT/progR_1_cleared.png" 2>/dev/null
# NOTE (2026-07-27): `levelComplete` grep-count removed — it counted levelCompleteStars*.lua
# asset preloads (always 8, emitted before frame[1]), identical in winning and losing runs.
# Win confirmation is progR_1_cleared.png / progR_2_level2.png only.
LC=$(marker_report "$ABLOG" 'levelCompleteStars')
say "  starsAssetPreloads=$LC (NOT a win signal)  frame=$(fnow)  h_fatal=$(fatal)"

say "== tap orange NEXT-level button (378,256) =="
F_BEFORE=$(fnow)
adb shell input tap 378 256; sleep 3
# fallbacks in case the button sits a few px off
adb shell input tap 378 256
# FIX (2026-07-27): was a fixed `sleep 14` gating the level-2 screenshot — i.e. gating PROOF_7,
# the multi-level-progression evidence. On a slow run that is ~25 frames, which can capture
# before level 2 has finished loading and make a working build look broken. Wait on frames.
settle_frames "$ABLOG" 120 300
adb exec-out screencap -p > "$OUT/progR_2_level2.png" 2>/dev/null
F_AFTER=$(fnow)
say "  frame before next-tap=$F_BEFORE  after=$F_AFTER  h_fatal=$(fatal)"

say "== confirm level-2 is INTERACTIVE: slingshot drag =="
adb shell input swipe 207 118 118 140 700; sleep 4
adb exec-out screencap -p > "$OUT/progR_3_level2_launch.png" 2>/dev/null
sleep 6
adb exec-out screencap -p > "$OUT/progR_4_level2_flight.png" 2>/dev/null
F_END=$(fnow)

say "== RESULTS =="
say "  frames: cleared=$F_BEFORE  level2-loaded=$F_AFTER  end=$F_END"
say "  h_fatal total=$(fatal)   (0 = no fatal at any point)"
say "  s-construct-guard: $(marker_report "$ABLOG" 's-construct')"
say "  empty-json-guard:  $(marker_report "$ABLOG" 'empty-json-guard\] empty')"
say "  WAF-diag lines:    $(marker_report "$ABLOG" 'WAF')"
say "  logic_error:       $(marker_report "$ABLOG" 'logic_error\|logicerr')"
say "  lua_longjmp:       $(marker_report "$ABLOG" 'lua_longjmp\|longjmp')"
say "  --- last level markers ---"
grep -aE 'levelComplete|levelCleared|loadLevel|startLevel|\[h_fatal\]' "$ABLOG" 2>/dev/null | tail -20 | tee -a "$LOG"
say "  final_pid: [$(adb shell pidof com.rovio.angrybirds 2>/dev/null|tr -d '\r')]"
say DONE
adb emu kill >/dev/null 2>&1
