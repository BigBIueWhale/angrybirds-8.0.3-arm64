#!/bin/bash
# AUDIO PLAYTHROUGH (longer window): the cont.121 nested-gt fix eliminated the __stack_chk_fail
# crash and the app boots+renders to frame[601] with the mixer running — but the short test window
# ended while the app was still menu-loading, so "temporary-slow" vs "render-stall" was unresolved.
# DECISIVE METRIC here: with audio active, do frames climb PAST 601 (i.e., is it rendering/playing,
# just slower under the audio overhead)? Give it a long settle + play + a multi-minute frame-watch.
set +e
( sleep 2200; adb emu kill 2>/dev/null; pkill -9 -f qemu 2>/dev/null ) &
APK=/work/out/angrybirds-8.0.3-x86shim-audio.apk
OUT=/work/reports/shots; mkdir -p "$OUT"; LOG="$OUT/audioplay.txt"; : >"$LOG"
ABLOG="$OUT/audioplay_abshim.txt"; : >"$ABLOG"
say(){ echo "$@" | tee -a "$LOG"; }
fnow(){ grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null|tail -1|grep -oE '[0-9]+'; }
say "== boot emulator WITH audio =="
export QEMU_AUDIO_DRV=none
emulator -avd abtest -no-window -no-boot-anim -no-snapshot -accel on \
         -gpu swiftshader_indirect -partition-size 6144 -wipe-data >/tmp/emu.log 2>&1 &
adb wait-for-device
for i in $(seq 1 150); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = 1 ] && break; sleep 2; done
sleep 8
adb shell settings put global airplane_mode_on 1 >/dev/null 2>&1
adb push "$APK" /data/local/tmp/ab.apk >/dev/null 2>&1
adb shell pm install -r -d /data/local/tmp/ab.apk 2>&1 | grep -q Success && say "install=ok" || { say "install FAIL"; say DONE; adb emu kill; exit 0; }
adb logcat -c >/dev/null 2>&1; adb logcat -G 32M >/dev/null 2>&1
adb logcat -s abshim > "$ABLOG" 2>/dev/null &
adb shell monkey -p com.rovio.angrybirds -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
say "== wait for tutorial card frame[601]+ =="
for s in $(seq 1 160); do sleep 5; fn=$(fnow); [ -n "$fn" ]&&[ "$fn" -ge 601 ]&&break; done
FB=$(fnow); say "  card at frame[$FB]"
say "== settle 25s, then drive the proven play sequence with generous timing =="
sleep 25
adb shell input tap 390 266; sleep 8
adb shell input tap 390 266; sleep 20
adb shell input swipe 207 118 110 150 700; sleep 12
adb shell input swipe 207 118 122 140 700; sleep 12
adb shell input swipe 207 118 118 136 700; sleep 12
say "== KEY: watch frame progression for up to ~6 min (does it climb past the card with audio?) =="
LASTF=$FB; STUCK=0
for w in $(seq 1 24); do
  sleep 15
  CF=$(fnow)
  MB=$(grep -ac 'nativeMixData ENABLED' "$ABLOG")
  HF=$(grep -ac '\[h_fatal\]' "$ABLOG")
  SC=$(grep -ac stack_chk_fail "$ABLOG")
  say "  [t=$((w*15))s] frame[$CF] mixData=$MB h_fatal=$HF stack_chk=$SC"
  [ -n "$CF" ] && [ -n "$LASTF" ] && [ "$CF" -gt "$LASTF" ] && { STUCK=0; LASTF=$CF; } || STUCK=$((STUCK+1))
  # decisive early-exit: clearly progressed well past the card
  [ -n "$CF" ] && [ "$CF" -ge 1201 ] && { say "  => PROGRESSED past frame[1201] with audio"; break; }
  [ "$HF" -gt 0 ] && { say "  => FATAL appeared"; break; }
done
FE=$(fnow)
adb exec-out screencap -p > "$OUT/audioplay_end.png" 2>/dev/null
say "== RESULTS (audio playthrough) =="
say "  install:            ok"
say "  card frame:         $FB"
say "  final frame:        $FE   => $([ -n "$FE" ] && [ -n "$FB" ] && [ "$FE" -gt "$FB" ] && echo 'FRAMES CLIMBED PAST CARD (renders/plays with audio)' || echo 'frames did NOT climb past card')"
say "  nativeMixData:      $(grep -ac 'nativeMixData ENABLED' "$ABLOG")"
say "  h_fatal:            $(grep -ac '\[h_fatal\]' "$ABLOG")"
say "  stack_chk_fail:     $(grep -ac stack_chk_fail "$ABLOG")"
say "  WATCHDOG/FROZEN:    $(grep -acE 'WATCHDOG|FROZEN' "$ABLOG")"
say "  final pid:          [$(adb shell pidof com.rovio.angrybirds 2>/dev/null|tr -d '\r')]"
say DONE
adb emu kill >/dev/null 2>&1
