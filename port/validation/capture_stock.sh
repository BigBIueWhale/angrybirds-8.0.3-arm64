#!/bin/bash
# Boot ab-emu, install the ORIGINAL unmodified ARM APK (runs via houdini), let it render a level
# offline, then EXTRACT the whole app data dir to find the runtime-generated data/script_paths.json.
# --network none => loopback only (assume the host is internet-reachable; never -p / never 0.0.0.0).
set +e
( sleep 700; adb emu kill 2>/dev/null; pkill -9 -f qemu 2>/dev/null ) &
APK=/work/apks/com.rovio.angrybirds@8.0.3.apk
OUT=/work/reports/stock; mkdir -p "$OUT"; LOG="$OUT/capture.txt"; : >"$LOG"
echo "== boot ==" | tee -a "$LOG"
emulator -avd abtest -no-window -no-audio -no-boot-anim -no-snapshot -accel on \
         -gpu swiftshader_indirect -partition-size 6144 -wipe-data >/tmp/emu.log 2>&1 &
adb wait-for-device
for i in $(seq 1 150); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = 1 ] && break; sleep 2; done
sleep 8
adb shell settings put global airplane_mode_on 1 >/dev/null 2>&1
adb shell svc wifi disable >/dev/null 2>&1; adb shell svc data disable >/dev/null 2>&1
echo "== install ORIGINAL apk via push+pm (robust) ==" | tee -a "$LOG"
adb push "$APK" /data/local/tmp/orig.apk >/tmp/push.log 2>&1
INST=fail
for t in 1 2 3 4; do
  R=$(timeout 180 adb shell pm install -r -d /data/local/tmp/orig.apk 2>&1)
  echo "  try$t: [$(echo "$R"|tr -d '\r'|tail -1)]" | tee -a "$LOG"
  echo "$R" | grep -q Success && { INST=ok; break; }; sleep 4
done
[ "$INST" = ok ] || { echo "INSTALL FAILED" | tee -a "$LOG"; adb emu kill >/dev/null 2>&1; exit 0; }
adb shell monkey -p com.rovio.angrybirds -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
echo "launched; staging 150s (renders a level per offline test)" | tee -a "$LOG"; sleep 150
echo "rovio_pid: $(adb shell pidof com.rovio.angrybirds 2>/dev/null|tr -d '\r')" | tee -a "$LOG"
adb exec-out screencap -p > "$OUT/stock_screen.png" 2>/dev/null
echo "== adb root (userdebug emulator) -> direct data-dir access (release app, run-as blocked) ==" | tee -a "$LOG"
adb root 2>&1 | tee -a "$LOG"; sleep 3; adb wait-for-device; sleep 2
echo "root id: $(adb shell id 2>/dev/null|tr -d '\r')" | tee -a "$LOG"
D=/data/data/com.rovio.angrybirds
echo "== hunt script_paths.json (+ all json) under $D and /sdcard + obb ==" | tee -a "$LOG"
adb shell "find $D /storage/emulated/0/Android /data/media/0/Android /mnt 2>/dev/null | grep -iE 'script_paths|angrybirds'" > "$OUT/datadir_find.txt" 2>&1
adb shell "find $D 2>/dev/null" >> "$OUT/datadir_find.txt" 2>&1
grep -iE 'script_paths|\.json$|\.lua$' "$OUT/datadir_find.txt" | head -40 | tee -a "$LOG"
echo "--- dump any script_paths*.json content ---" | tee -a "$LOG"
for p in $(grep -iE 'script_paths' "$OUT/datadir_find.txt" | tr -d '\r'); do
  echo ">>> $p" | tee -a "$LOG"
  adb shell "cat '$p' 2>/dev/null" > "$OUT/GENERATED_script_paths.json"
  head -c 2000 "$OUT/GENERATED_script_paths.json" | tee -a "$LOG"; echo | tee -a "$LOG"
done
echo "== pull the whole data dir ==" | tee -a "$LOG"
adb pull "$D" "$OUT/appdata_stock" >/tmp/pull.log 2>&1; tail -1 /tmp/pull.log | tee -a "$LOG"
find "$OUT/appdata_stock" -iname '*script_paths*' 2>/dev/null | tee -a "$LOG"
echo DONE | tee -a "$LOG"
adb emu kill >/dev/null 2>&1
