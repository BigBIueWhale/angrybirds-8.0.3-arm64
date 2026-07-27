#!/bin/bash
# FULL PLAYTHROUGH on MODERN Android (API 34 = Android 14, the A56's regime) — confirm the game
# PLAYS+WINS (not just boots) on modern Android. Dismisses the DeprecatedTargetSdkVersionDialog
# first (tap OK), then runs the same proven tutorial play sequence (coords transfer: 640x320).
set +e
# watchdog raised 1450 -> 2100s: the frame-based settle below can add up to 300s over the old
# fixed sleep, and boot-to-frame[601] has been observed at ~565s.
( sleep 2100; adb emu kill 2>/dev/null; pkill -9 -f qemu 2>/dev/null ) &
APK=/work/out/angrybirds-8.0.3-x86shim-release.apk
OUT=/work/reports/shots; mkdir -p "$OUT"; LOG="$OUT/modplay.txt"; : >"$LOG"
ABLOG="$OUT/modplay_abshim.txt"; : >"$ABLOG"
say(){ echo "$@" | tee -a "$LOG"; }
say "== boot API 34 (abtest34) =="
emulator -avd abtest34 -no-window -no-audio -no-boot-anim -no-snapshot -accel on \
         -gpu swiftshader_indirect -partition-size 6144 -wipe-data >/tmp/emu34.log 2>&1 &
adb wait-for-device
for i in $(seq 1 200); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = 1 ] && break; sleep 2; done
sleep 10
say "  android $(adb shell getprop ro.build.version.release 2>/dev/null|tr -d '\r') (API $(adb shell getprop ro.build.version.sdk 2>/dev/null|tr -d '\r'))"
adb shell settings put global airplane_mode_on 1 >/dev/null 2>&1
adb push "$APK" /data/local/tmp/ab.apk >/dev/null 2>&1
adb shell pm install -r -d /data/local/tmp/ab.apk 2>&1 | grep -q Success && say "install=ok" || { say "install FAIL"; say DONE; adb emu kill; exit 0; }
adb logcat -c >/dev/null 2>&1; adb logcat -G 32M >/dev/null 2>&1
adb logcat -s abshim > "$ABLOG" 2>/dev/null &
adb shell monkey -p com.rovio.angrybirds -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
say "== wait for game render (frame[601]+) =="
for s in $(seq 1 130); do sleep 5
  fn=$(grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null|tail -1|grep -oE '[0-9]+')
  [ -n "$fn" ] && [ "$fn" -ge 601 ] && { say "  card at ~$((s*5))s frame[$fn]"; break; }
done
sleep 4
adb exec-out screencap -p > "$OUT/modplay_1_dialog.png" 2>/dev/null
say "== dismiss 'built for older Android' dialog (tap OK) =="
adb shell input tap 490 237; sleep 1; adb shell input tap 490 237; sleep 3
adb exec-out screencap -p > "$OUT/modplay_2_afterdialog.png" 2>/dev/null
say "== play: start card + 3 slingshot drags (proven API-25 sequence) =="
adb shell input tap 390 266; sleep 4; adb shell input tap 390 266; sleep 12
adb shell input swipe 207 118 110 150 700; sleep 8
adb shell input swipe 207 118 122 140 700; sleep 8
adb shell input swipe 207 118 118 136 700
# FIX (2026-07-27): this used to be a fixed `sleep 16`. Under SwiftShader the frame rate varies
# a lot between runs (~2-16 fps measured), so 16s was sometimes ~29 frames — not enough for the
# last bird to land and the results screen to animate in. That made the test FLAKY: the run of
# 07-27 09:29 reported success on every log metric yet screenshotted a level still in progress,
# which is why PROOF_8's source could not be regenerated. Wait on FRAMES, not wall-clock, so the
# settle adapts to whatever rate the host happens to deliver.
fnum(){ grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null | tail -1 | grep -oE '[0-9]+'; }
F0=$(fnum); F0=${F0:-0}; say "  last drag sent at frame[$F0]; waiting +120 frames (cap 300s)"
for i in $(seq 1 60); do sleep 5
  FN=$(fnum); FN=${FN:-0}
  [ "$FN" -ge $((F0+120)) ] && { say "  settled at frame[$FN] after ~$((i*5))s"; break; }
done
[ "$(fnum)" -lt $((F0+120)) ] && say "  WARNING: frame target not reached in 300s (rate very low) — screenshot may be early"
adb exec-out screencap -p > "$OUT/modplay_3_end.png" 2>/dev/null
say "== RESULTS (modern-Android full play) =="
say "  install:           ok"
# NOTE (2026-07-27): a 'levelComplete' grep-count was REMOVED from here. It counted
# data/scripts/particles/levelCompleteStars{1..4}.lua ASSET PRELOADS — always exactly 8, two
# log lines per file, emitted BEFORE frame[1] — not level wins. It read "8" identically in
# runs that won and runs that never cleared a pig, so it never measured anything. No abshim
# log marker distinguishes a win; the END SCREENSHOT is the only authority.
say "  win check:         SCREENSHOT ONLY -> modplay_3_end.png (a win shows 'LEVEL CLEARED' + stars + score)"
say "  levelCompleteStars asset preloads (NOT a win signal): $(grep -ac 'levelCompleteStars' "$ABLOG" 2>/dev/null)"
say "  h_fatal:           $(grep -ac '\[h_fatal\]' "$ABLOG" 2>/dev/null)  (0 = no crash)"
say "  s-construct-guard: $(grep -ac 's-construct-null-guard' "$ABLOG" 2>/dev/null)  (level-end fix firing)"
say "  real St11logic:    $(grep -acE 'THROW.*St11logic_error' "$ABLOG" 2>/dev/null)"
say "  last frame:        $(grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null|tail -1)"
say "  final pid:         [$(adb shell pidof com.rovio.angrybirds 2>/dev/null|tr -d '\r')]  (alive => played through)"
say DONE
adb emu kill >/dev/null 2>&1
