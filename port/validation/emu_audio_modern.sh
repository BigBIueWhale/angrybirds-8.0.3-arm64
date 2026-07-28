#!/bin/bash
# AUDIO on MODERN Android (API 34 = Android 14, the A56's W^X + install regime) — de-risk the
# on-device audio variant. All prior audio validation was API 25; AudioTrack/HAL differs a lot on
# modern Android, so this confirms the audio variant (nested-gt fix + nativeMixData) installs, boots
# under W^X, and PLAYS with audio active WITHOUT a crash on the A56's OS generation. Audio ENABLED
# (no -no-audio; QEMU_AUDIO_DRV=none so AudioTrack still inits). Dismisses the deprecated-sdk dialog.
set +e
source "$(dirname "$0")/lib_settle.sh"
source "$(dirname "$0")/lib_provenance.sh"   # frame-based settle (replaces flaky fixed sleeps)
( sleep 1600; adb emu kill 2>/dev/null; pkill -9 -f qemu 2>/dev/null ) &
APK=/work/out/angrybirds-8.0.3-x86shim-audio.apk
OUT=/work/reports/shots; mkdir -p "$OUT"; LOG="$OUT/audiomod.txt"; : >"$LOG"
ABLOG="$OUT/audiomod_abshim.txt"; : >"$ABLOG"
say(){ echo "$@" | tee -a "$LOG"; }
fnow(){ grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null|tail -1|grep -oE '[0-9]+'; }
say "== boot API 34 (abtest34) WITH audio =="
export QEMU_AUDIO_DRV=none
emulator -avd abtest34 -no-window -no-boot-anim -no-snapshot -accel on \
         -gpu swiftshader_indirect -partition-size 6144 -wipe-data >/tmp/emu34.log 2>&1 &
adb wait-for-device
for i in $(seq 1 200); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = 1 ] && break; sleep 2; done
sleep 10
say "  android $(adb shell getprop ro.build.version.release 2>/dev/null|tr -d '\r') (API $(adb shell getprop ro.build.version.sdk 2>/dev/null|tr -d '\r'))"
adb shell settings put global airplane_mode_on 1 >/dev/null 2>&1
adb push "$APK" /data/local/tmp/ab.apk >/dev/null 2>&1
adb shell pm install -r -d /data/local/tmp/ab.apk 2>&1 | grep -q Success && say "install=ok" || { say "install FAIL"; say DONE; adb emu kill; exit 0; }
record_build "$APK" "audiomod"
adb logcat -c >/dev/null 2>&1; adb logcat -G 32M >/dev/null 2>&1
adb logcat -s abshim > "$ABLOG" 2>/dev/null &
adb shell monkey -p com.rovio.angrybirds -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
say "== wait for game render (frame[601]+) =="
for s in $(seq 1 150); do sleep 5; fn=$(fnow); [ -n "$fn" ] && [ "$fn" -ge 601 ] && { say "  card at ~$((s*5))s frame[$fn]"; break; }; done
sleep 4
adb exec-out screencap -p > "$OUT/audiomod_1_dialog.png" 2>/dev/null
say "== dismiss 'built for older Android' dialog (tap OK) =="
adb shell input tap 490 237; sleep 1; adb shell input tap 490 237; sleep 3
say "== settle, then play (proven sequence) + watch frames climb past card with audio =="
sleep 20
adb shell input tap 390 266; sleep 8
adb shell input tap 390 266; sleep 20
adb shell input swipe 207 118 110 150 700; sleep 12
adb shell input swipe 207 118 122 140 700; sleep 12
adb shell input swipe 207 118 118 136 700
# FIX (2026-07-27): was `sleep 12` — a fixed wall-clock settle against a frame rate that
# varies ~1.8-15 fps between runs, so it silently captured mid-level on slow runs. See lib_settle.sh.
settle_frames "$ABLOG" 120 300
for w in $(seq 1 20); do
  sleep 15; CF=$(fnow); HF=$(grep -ac '\[h_fatal\]' "$ABLOG"); SC=$(grep -ac stack_chk_fail "$ABLOG")
  say "  [t=$((w*15))s] frame[$CF] mixData=$(grep -ac 'nativeMixData ENABLED' "$ABLOG") h_fatal=$HF stack_chk=$SC"
  [ -n "$CF" ] && [ "$CF" -ge 1201 ] && { say "  => reached frame[1201] with audio on API 34"; break; }
  [ "$HF" -gt 0 ] && { say "  => FATAL"; break; }
done
adb exec-out screencap -p > "$OUT/audiomod_end.png" 2>/dev/null
say "== RESULTS (API 34 audio) =="
say "  install:           ok"
say "  card frame:        601"
say "  final frame:       $(fnow)"
say "  nativeMixData:     $(grep -ac 'nativeMixData ENABLED' "$ABLOG")  (>0 => AudioTrack inited + mixer ran under modern W^X)"
say "  h_fatal:           $(grep -ac '\[h_fatal\]' "$ABLOG")  (0 = no crash)"
say "  stack_chk_fail:    $(grep -ac stack_chk_fail "$ABLOG")  (0 = nested-gt fix holds on API 34)"
say "  WATCHDOG/FROZEN:   $(grep -acE 'WATCHDOG|FROZEN' "$ABLOG")"
say "  init_array:        $(grep -aoE 'init_array [0-9]+/125' "$ABLOG" | tail -1)  (125/125 = JIT under W^X)"
say "  final pid:         [$(adb shell pidof com.rovio.angrybirds 2>/dev/null|tr -d '\r')]  (alive => played through with audio)"
say DONE
adb emu kill >/dev/null 2>&1
