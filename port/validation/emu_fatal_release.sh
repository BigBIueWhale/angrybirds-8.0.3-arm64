#!/bin/bash
# Boot ab-emu, install the CURRENT x86 shim APK, launch, and watch the game AUTO-navigate
# splash->menu->level-load. Capture the FULL abshim stream (no early exit) and, when the first
# [h_fatal] appears, dump its LR-capture + the last level-load ops before it. This tells us the
# CURRENT fatal (post-glGetIntegerv-fix), resolving the stale 14:43(stack-smash)/14:56(IOException) logs.
set +e
# Refuse to run while another evidence run is in flight: 50 scripts share reports/shots and
# two concurrent runs BLEND their logs into numbers that look normal and mean nothing.
source "$(dirname "$0")/lib_runlock.sh"
acquire_run_lock "$(basename "$0")" || exit 1
( sleep 900; adb emu kill 2>/dev/null; pkill -9 -f qemu 2>/dev/null ) &
APK=/work/out/angrybirds-8.0.3-x86shim-release.apk
OUT=/work/reports/shots; mkdir -p "$OUT"; LOG="$OUT/emu_fatalR.txt"; : >"$LOG"
ABLOG="$OUT/emu_fatalR_abshim.txt"; : >"$ABLOG"
echo "== boot ==" | tee -a "$LOG"
emulator -avd abtest -no-window -no-audio -no-boot-anim -no-snapshot -accel on \
         -gpu swiftshader_indirect -partition-size 6144 -wipe-data >/tmp/emu.log 2>&1 &
adb wait-for-device
for i in $(seq 1 150); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = 1 ] && break; sleep 2; done
sleep 8
echo "booted ~$((i*2))s" | tee -a "$LOG"
adb shell settings put global airplane_mode_on 1 >/dev/null 2>&1
adb push "$APK" /data/local/tmp/ab.apk >/tmp/push.log 2>&1
INST=fail
for t in 1 2 3 4; do
  R=$(adb shell pm install -r -d /data/local/tmp/ab.apk 2>&1)
  echo "$R" | grep -q Success && { INST=ok; break; }; sleep 4
done
echo "install=$INST" | tee -a "$LOG"
[ "$INST" = ok ] || { adb emu kill >/dev/null 2>&1; echo DONE|tee -a "$LOG"; exit 0; }
adb logcat -c >/dev/null 2>&1; adb logcat -G 32M >/dev/null 2>&1
adb logcat -s abshim > "$ABLOG" 2>/dev/null &
CATPID=$!
adb shell monkey -p com.rovio.angrybirds -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
echo "== launched; watching up to 660s for auto-level-load + [h_fatal] ==" | tee -a "$LOG"
FATAL=0
for s in $(seq 1 132); do
  sleep 5
  if grep -qa '\[h_fatal\]' "$ABLOG" 2>/dev/null; then
    echo "  >>> [h_fatal] seen at ~$((s*5))s after launch" | tee -a "$LOG"; FATAL=1; sleep 6; break; fi
done
kill $CATPID 2>/dev/null
PID=$(adb shell pidof com.rovio.angrybirds 2>/dev/null|tr -d '\r')
echo "== RESULT ==" | tee -a "$LOG"
echo "final_pid: [$PID]  fatal_seen=$FATAL  draws=[$(grep -aoE 'draws=[0-9]+' "$ABLOG"|tail -1)]" | tee -a "$LOG"
echo "################ [h_fatal] LR-capture (THE FAILING FUNCTION) ################" | tee -a "$LOG"
grep -a '\[h_fatal\]' "$ABLOG" | head -6 | tee -a "$LOG"
echo "--- last 14 file/asset/throw ops before the first h_fatal ---" | tee -a "$LOG"
awk '/\[h_fatal\]/{found=1} {buf[NR]=$0} END{ for(i=1;i<=NR;i++) if(i>=fl-14 && i<=fl) print buf[i] } /\[h_fatal\]/ && !fl{fl=NR}' "$ABLOG" 2>/dev/null | tail -16 | grep -aE '\[fopen\]|\[open\]|\[AAsset|THROW|\[throw\]|h_fatal|IOException|errno' | tail -16 | tee -a "$LOG"
echo "--- distinct THROWs across the run ---" | tee -a "$LOG"
grep -aoE 'THROW #[0-9]+ [A-Za-z0-9_:]+|\[throw\][^,]*' "$ABLOG" | sed -E 's/#[0-9]+//' | sort | uniq -c | sort -rn | head | tee -a "$LOG"
echo "--- all fopen/open FAILs (device-info etc) ---" | tee -a "$LOG"
grep -aE 'FAIL\(->IOException\)' "$ABLOG" | sed -E 's/.*\[(fopen|open)\]/[\1]/' | sort | uniq -c | sort -rn | head -25 | tee -a "$LOG"
echo "== screenshot ==" | tee -a "$LOG"
adb exec-out screencap -p > "$OUT/emu_fatalR_screen.png" 2>/dev/null
echo "screenshot: $(wc -c < "$OUT/emu_fatalR_screen.png" 2>/dev/null)B" | tee -a "$LOG"
echo DONE | tee -a "$LOG"
adb emu kill >/dev/null 2>&1