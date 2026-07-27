#!/bin/bash
# Boot ab-emu, install the (fixed) x86 shim APK, launch the game, capture boot behavior:
# does it get PAST the 'wgt' VFS scheme / render / crash? (real ART + asset-mgr + init).
set +e
( sleep 900; adb emu kill 2>/dev/null; pkill -9 -f qemu 2>/dev/null ) &
APK=/work/out/angrybirds-8.0.3-x86shim.apk
OUT=/work/reports/shots; mkdir -p "$OUT"; LOG="$OUT/emu_run.txt"; : >"$LOG"
echo "== boot emulator (KVM) ==" | tee -a "$LOG"
emulator -avd abtest -no-window -no-audio -no-boot-anim -no-snapshot -accel on \
         -gpu swiftshader_indirect -partition-size 6144 -wipe-data >/tmp/emu.log 2>&1 &
adb wait-for-device
for i in $(seq 1 150); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = 1 ] && break; sleep 2; done
for i in $(seq 1 90); do adb shell pm list packages >/dev/null 2>&1 && \
    [ "$(adb shell getprop dev.bootcomplete 2>/dev/null|tr -d '\r')" = 1 ] && break; sleep 2; done
sleep 8
echo "settled; /data: $(adb shell df /data 2>/dev/null|tail -1)" | tee -a "$LOG"
adb shell settings put global airplane_mode_on 1 >/dev/null 2>&1
echo "== install ==" | tee -a "$LOG"
adb push "$APK" /data/local/tmp/ab.apk >/tmp/push.log 2>&1
INST=fail
for t in 1 2 3 4; do
  R=$(adb shell pm install -r -d /data/local/tmp/ab.apk 2>&1)
  echo "  try$t: [$(echo "$R"|tr -d '\r'|tail -1)]" | tee -a "$LOG"
  echo "$R" | grep -q Success && { INST=ok; break; }; sleep 4
done
[ "$INST" = ok ] || { echo "INSTALL FAILED" | tee -a "$LOG"; adb emu kill >/dev/null 2>&1; exit 0; }
adb logcat -c >/dev/null 2>&1; adb logcat -G 32M >/dev/null 2>&1
echo "== launch (capturing abshim continuously) ==" | tee -a "$LOG"
ABLOG=/work/reports/shots/emu_abshim.txt
adb logcat -s abshim > "$ABLOG" 2>/dev/null &
CATPID=$!
# ALSO capture the ENGINE's OWN logs (h_log forwards __android_log_print under tags Framework/fusion/
# Lua/etc.) — these carry the real "why the scene didn't load" errors that -s abshim misses.
ENGLOG=/work/reports/shots/emu_engine.txt
adb logcat -s Framework fusion Fusion Lua AngryBirds Rovio Renderer luaAssetRenderer '*:E' > "$ENGLOG" 2>/dev/null &
CATPID2=$!
adb shell monkey -p com.rovio.angrybirds -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
# Poll for scene-set / first render (or nativeInit progress) — exit early when seen. ART is slow to
# call nativeInit on the headless emulator (~5 min surface setup), so wait long but stop on success.
for s in $(seq 1 130); do
  sleep 5
  if grep -qaE 'draws=[1-9]|frame\[[3-9][0-9]' "$ABLOG" 2>/dev/null; then
    echo "  >>> scene/render marker seen at ~$((s*5))s after launch" | tee -a "$LOG"; sleep 10; break; fi
done
kill $CATPID $CATPID2 2>/dev/null
PID=$(adb shell pidof com.rovio.angrybirds 2>/dev/null|tr -d '\r')
echo "== RESULT ==" | tee -a "$LOG"
echo "final_pid: [$PID]  (alive => didn't abort/crash)" | tee -a "$LOG"
echo "crash-signals: $(adb logcat -d 2>/dev/null|grep -ciE 'SIGSEGV|SIGABRT|Fatal signal')" | tee -a "$LOG"
echo "--- gobj WRITE ([g+0x7c]=0xabb124 Context, [g+0x80]=0xabb128 scene) ---" | tee -a "$LOG"
grep -aE 'gobj WRITE' "$ABLOG" | sort -u | tee -a "$LOG"
echo "--- scene-ctor path F@/factory@ (does F run? ctor return? [g+0x80] store?) ---" | tee -a "$LOG"
grep -aoE 'F@0x[0-9a-f]+ [A-Za-z<>\[\] +0x=-]*|factory@0x[0-9a-f]+ [A-Za-z ]*' "$ABLOG" | sort | uniq -c | tee -a "$LOG"
echo "--- distinct THROWs ---" | tee -a "$LOG"
grep -aoE 'THROW #[0-9]+ [A-Za-z0-9_:]+|\[throw\] [A-Za-z0-9_:]+' "$ABLOG" | sed -E 's/#[0-9]+//' | sort | uniq -c | head | tee -a "$LOG"
echo "--- frames / draws ---" | tee -a "$LOG"
grep -aE 'frame\[' "$ABLOG" | tail -3 | tee -a "$LOG"
echo "== screenshot ==" | tee -a "$LOG"
adb exec-out screencap -p > "$OUT/emu_screen.png" 2>/dev/null
python3 - "$OUT/emu_screen.png" <<'PY' 2>&1 | tee -a "$LOG"
import sys,struct,zlib
try:
    d=open(sys.argv[1],'rb').read(); assert d[:8]==b'\x89PNG\r\n\x1a\n'
    # find IHDR
    w,h=struct.unpack('>II',d[16:24]); print(f"screenshot {w}x{h}, {len(d)} bytes")
    # crude non-black check: decompress IDAT, sample
    idat=b''; i=8
    while i<len(d):
        ln=struct.unpack('>I',d[i:i+4])[0]; typ=d[i+4:i+8]
        if typ==b'IDAT': idat+=d[i+8:i+8+ln]
        i+=12+ln
    raw=zlib.decompress(idat); nz=sum(1 for b in raw[:200000] if b>16)
    print(f"non-trivial bytes in first 200KB of pixels: {nz} ({'RENDERS something' if nz>5000 else 'mostly black/empty'})")
except Exception as e: print("screenshot check failed:",e)
PY
echo DONE | tee -a "$LOG"
adb emu kill >/dev/null 2>&1
