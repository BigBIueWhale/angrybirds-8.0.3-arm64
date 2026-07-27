#!/bin/bash
# Boot + install the x86 shim, auto-play the tutorial to WIN, and CAPTURE the St11logic_error
# what()+backtrace (logicerr-msg/logicerr-bt) that crashes the level-end ad transition. Fully
# automated: polls for the tutorial card (frame[601]+), then taps the start card + does 3 slingshot
# drags (the sequence proven to reach levelComplete), then dumps the logic_error diagnostics.
set +e
( sleep 1300; adb emu kill 2>/dev/null; pkill -9 -f qemu 2>/dev/null ) &
APK=/work/out/angrybirds-8.0.3-x86shim-release.apk
OUT=/work/reports/shots; mkdir -p "$OUT"; LOG="$OUT/playthroughR.txt"; : >"$LOG"
ABLOG="$OUT/playthroughR_abshim.txt"; : >"$ABLOG"
echo "== boot ==" | tee -a "$LOG"
emulator -avd abtest -no-window -no-audio -no-boot-anim -no-snapshot -accel on \
         -gpu swiftshader_indirect -partition-size 6144 -wipe-data >/tmp/emu.log 2>&1 &
adb wait-for-device
for i in $(seq 1 150); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = 1 ] && break; sleep 2; done
sleep 8
adb shell settings put global airplane_mode_on 1 >/dev/null 2>&1
adb push "$APK" /data/local/tmp/ab.apk >/tmp/push.log 2>&1
INST=fail; for t in 1 2 3 4; do adb shell pm install -r -d /data/local/tmp/ab.apk 2>&1 | grep -q Success && { INST=ok; break; }; sleep 4; done
echo "install=$INST" | tee -a "$LOG"; [ "$INST" = ok ] || { echo DONE|tee -a "$LOG"; adb emu kill; exit 0; }
adb logcat -c >/dev/null 2>&1; adb logcat -G 32M >/dev/null 2>&1
adb logcat -s abshim > "$ABLOG" 2>/dev/null &
adb shell monkey -p com.rovio.angrybirds -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
echo "== waiting for tutorial card (frame[601]+) ==" | tee -a "$LOG"
for s in $(seq 1 130); do sleep 5
  fn=$(grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null | tail -1 | grep -oE '[0-9]+')
  [ -n "$fn" ] && [ "$fn" -ge 601 ] && { echo "  card at ~$((s*5))s (frame[$fn])" | tee -a "$LOG"; break; }
done
sleep 6
echo "== auto-play: start card + 3 slingshot drags ==" | tee -a "$LOG"
adb shell input tap 390 266; sleep 4; adb shell input tap 390 266; sleep 12
adb shell input swipe 207 118 110 150 700; sleep 8
adb shell input swipe 207 118 122 140 700; sleep 8
adb shell input swipe 207 118 118 136 700; sleep 14
adb exec-out screencap -p > "$OUT/playthroughR_end.png" 2>/dev/null
echo "== RESULT: logic_error capture ==" | tee -a "$LOG"
grep -aE 'levelComplete|THROW #1[5-9]|THROW #2[0-9]|logicerr-msg|logicerr-bt|\[h_fatal\]' "$ABLOG" 2>/dev/null | tail -30 | tee -a "$LOG"
echo "guard-fired: $(grep -ac 'empty-json-guard\] empty' "$ABLOG" 2>/dev/null)" | tee -a "$LOG"
echo "final_pid: [$(adb shell pidof com.rovio.angrybirds 2>/dev/null|tr -d '\r')]" | tee -a "$LOG"
echo DONE | tee -a "$LOG"
adb emu kill >/dev/null 2>&1