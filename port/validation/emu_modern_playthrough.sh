#!/bin/bash
# FULL PLAYTHROUGH on MODERN Android (API 34 = Android 14, the A56's regime) — confirm the game
# PLAYS+WINS (not just boots) on modern Android. Dismisses the DeprecatedTargetSdkVersionDialog
# first (tap OK), then runs the same proven tutorial play sequence (coords transfer: 640x320).
set +e
source "$(dirname "$0")/lib_settle.sh"
source "$(dirname "$0")/lib_provenance.sh"
source "$(dirname "$0")/lib_metrics.sh"
source "$(dirname "$0")/lib_dialogs.sh"   # frame-based settle (replaces flaky fixed sleeps)
# watchdog raised 1450 -> 2100s: the frame-based settle below can add up to 300s over the old
# fixed sleep, and boot-to-frame[601] has been observed at ~565s.
( sleep 2100; adb emu kill 2>/dev/null; pkill -9 -f qemu 2>/dev/null ) &
APK=/work/out/angrybirds-8.0.3-x86shim-release.apk
# AVD and output prefix are overridable so the SAME playthrough can be run on the GMS tier
# (ABSHIM_AVD=abgms ABSHIM_OUTPFX=modplaygms) without overwriting the AOSP-tier evidence. Defaults
# reproduce the original behaviour exactly.
AVD="${ABSHIM_AVD:-abtest34}"
PFX="${ABSHIM_OUTPFX:-modplay}"
OUT=/work/reports/shots; mkdir -p "$OUT"; LOG="$OUT/${PFX}.txt"; : >"$LOG"
ABLOG="$OUT/${PFX}_abshim.txt"; : >"$ABLOG"
say(){ echo "$@" | tee -a "$LOG"; }
say "== boot API 34 ($AVD) =="
emulator -avd "$AVD" -no-window -no-audio -no-boot-anim -no-snapshot -accel on \
         -gpu swiftshader_indirect -partition-size 6144 -wipe-data >/tmp/emu34.log 2>&1 &
adb wait-for-device
for i in $(seq 1 200); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = 1 ] && break; sleep 2; done
sleep 10
say "  android $(adb shell getprop ro.build.version.release 2>/dev/null|tr -d '\r') (API $(adb shell getprop ro.build.version.sdk 2>/dev/null|tr -d '\r'))"
adb shell settings put global airplane_mode_on 1 >/dev/null 2>&1
adb push "$APK" /data/local/tmp/ab.apk >/dev/null 2>&1
adb shell pm install -r -d /data/local/tmp/ab.apk 2>&1 | grep -q Success && say "install=ok" || { say "install FAIL"; say DONE; adb emu kill; exit 0; }
record_build "$APK" "$PFX"   # follows ABSHIM_OUTPFX, so an abgms run records "modplaygms" and does NOT overwrite the AOSP row
adb logcat -c >/dev/null 2>&1; adb logcat -G 32M >/dev/null 2>&1
adb logcat -s abshim > "$ABLOG" 2>/dev/null &
adb shell monkey -p com.rovio.angrybirds -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
say "== wait for game render (frame[601]+) =="
for s in $(seq 1 130); do sleep 5
  fn=$(grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null|tail -1|grep -oE '[0-9]+')
  [ -n "$fn" ] && [ "$fn" -ge 601 ] && { say "  card at ~$((s*5))s frame[$fn]"; break; }
done
sleep 4
adb exec-out screencap -p > "$OUT/${PFX}_1_dialog.png" 2>/dev/null
say "== dismiss stacked system dialogs (see lib_dialogs.sh) =="
dismiss_system_dialogs "$OUT/${PFX}_1b_afterdialogs.png"
adb exec-out screencap -p > "$OUT/${PFX}_2_afterdialog.png" 2>/dev/null
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
# (2026-07-27, later: this was originally implemented inline here. It is now lib_settle.sh so
#  the other seven playthrough scripts share ONE implementation instead of eight copies that
#  can drift apart. Behaviour is unchanged: +120 frames, 300s cap, loud warning if unmet.)
settle_frames "$ABLOG" 120 300
adb exec-out screencap -p > "$OUT/${PFX}_3_end.png" 2>/dev/null
say "== RESULTS (modern-Android full play) =="
say "  install:           ok"
# NOTE (2026-07-27): a 'levelComplete' grep-count was REMOVED from here. It counted
# data/scripts/particles/levelCompleteStars{1..4}.lua ASSET PRELOADS — always exactly 8, two
# log lines per file, emitted BEFORE frame[1] — not level wins. It read "8" identically in
# runs that won and runs that never cleared a pig, so it never measured anything. No abshim
# log marker distinguishes a win; the END SCREENSHOT is the only authority.
say "  win check:         SCREENSHOT ONLY -> ${PFX}_3_end.png (a win shows 'LEVEL CLEARED' + stars + score)"
say "  levelCompleteStars asset preloads (NOT a win signal): $(marker_report "$ABLOG" 'levelCompleteStars')"
say "  h_fatal:           $(h_fatal_report "$ABLOG")"
say "  s-construct-guard: $(marker_report "$ABLOG" 's-construct-null-guard')  (level-end fix firing)"
say "  real St11logic:    $(marker_report "$ABLOG" 'THROW.*St11logic_error')"
say "  last frame:        $(grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null|tail -1)"
say "  final pid:         [$(adb shell pidof com.rovio.angrybirds 2>/dev/null|tr -d '\r')]  (alive => played through)"
say DONE
adb emu kill >/dev/null 2>&1
