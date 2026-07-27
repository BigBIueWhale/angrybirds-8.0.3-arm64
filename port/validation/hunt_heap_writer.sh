#!/bin/bash
# hunt_heap_writer.sh — find what clears PINUSE on a chunk head word.
#
# Runs the NON-RELEASE x86 shim (which carries the chunk-head guest-write watchpoint from
# cpu.c) and waits, patiently, for [GALLOC-CORRUPT] to appear — then dumps the verdict.
#
# WHY A DEDICATED SCRIPT: arming UC_HOOK_MEM_WRITE over the 512MB heap makes Unicorn
# instrument every guest write and defeats TCG block chaining, so the emulation runs FAR
# slower than normal. emu_run.sh's ~650s window ended after init_array with fewer than ~16k
# allocator ops — short of the op 12544-26368 range where the corruption has been observed.
# This script does nothing but wait for the marker, for as long as it takes.
#
# Usage (from the repo root, mounted at /work):
#   docker run --rm --network none --device /dev/kvm --group-add 993 \
#       -v "$PWD":/work ab-emu bash /work/port/validation/hunt_heap_writer.sh
set +e
( sleep 5400; adb emu kill 2>/dev/null; pkill -9 -f qemu 2>/dev/null ) &
APK=/work/out/angrybirds-8.0.3-x86shim.apk          # non-release: has the watchpoint
OUT=/work/reports/shots; mkdir -p "$OUT"
LOG="$OUT/hunt_heap.txt"; : >"$LOG"
ABLOG="$OUT/hunt_heap_abshim.txt"; : >"$ABLOG"
say(){ echo "$@" | tee -a "$LOG"; }

say "== boot (API 25) =="
emulator -avd abtest -no-window -no-audio -no-boot-anim -no-snapshot -accel on \
         -gpu swiftshader_indirect -partition-size 6144 -wipe-data >/tmp/emu.log 2>&1 &
adb wait-for-device
for i in $(seq 1 150); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = 1 ] && break; sleep 2; done
sleep 8
adb shell settings put global airplane_mode_on 1 >/dev/null 2>&1
adb push "$APK" /data/local/tmp/ab.apk >/dev/null 2>&1
INST=fail
for t in 1 2 3 4; do adb shell pm install -r -d /data/local/tmp/ab.apk 2>&1 | grep -q Success && { INST=ok; break; }; sleep 4; done
say "install=$INST"; [ "$INST" = ok ] || { say DONE; adb emu kill; exit 0; }

adb logcat -c >/dev/null 2>&1; adb logcat -G 64M >/dev/null 2>&1
adb logcat -s abshim > "$ABLOG" 2>/dev/null &
adb shell monkey -p com.rovio.angrybirds -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1

say "== waiting for [GALLOC-CORRUPT] (the watchpoint makes emulation very slow) =="
FOUND=0
for s in $(seq 1 420); do          # up to ~70 min
    sleep 10
    if grep -qa "GALLOC-CORRUPT" "$ABLOG" 2>/dev/null; then
        say "  corruption detected after ~$((s*10))s"; FOUND=1; sleep 15; break
    fi
    if [ $((s % 30)) -eq 0 ]; then
        say "  ...${s}0s elapsed; last marker: $(grep -aoE 'init_array [0-9]+/125|abshim ready|frame\[[0-9]+\]|GALLOC-STATS\] checks=[0-9]+' "$ABLOG" 2>/dev/null | tail -1)"
        adb shell pidof com.rovio.angrybirds >/dev/null 2>&1 || { say "  process GONE — aborting"; break; }
    fi
done

say "== VERDICT =="
if [ "$FOUND" = 1 ]; then
    grep -a "GALLOC-CORRUPT" "$ABLOG" | head -8 | tee -a "$LOG"
else
    say "  no corruption seen in the window"
    say "  last progress: $(grep -aoE 'init_array [0-9]+/125|abshim ready|frame\[[0-9]+\]|checks=[0-9]+' "$ABLOG" 2>/dev/null | tail -1)"
fi
say "  watchpoint armed: $(grep -ac 'watchpoint armed' "$ABLOG")"
say "  head-writes seen: $(grep -aoE 'head-writes seen' "$ABLOG" | wc -l)"
say "  final pid: [$(adb shell pidof com.rovio.angrybirds 2>/dev/null|tr -d '\r')]"
say DONE
adb emu kill >/dev/null 2>&1
