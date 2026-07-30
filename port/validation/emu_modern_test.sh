#!/bin/bash
# Validate the deliverable-config (x86 proxy) on MODERN Android (API 34 = Android 14, the first
# release with the W^X + low-target-sdk-install regime the A56's Android 16 also enforces). Two
# critical questions ALL prior validation (API 25 / Android 7.1) could NOT answer:
#   (1) does a targetSdk=26 app INSTALL past the low-target-sdk block?
#   (2) does the shim's Unicorn JIT get EXECUTABLE memory under W^X (targetSdk<29 exemption), i.e.
#       does init_array run + the game render, with NO execmem/SELinux denials?
set +e
source "$(dirname "$0")/lib_metrics.sh"
# Refuse to run while another evidence run is in flight: 50 scripts share reports/shots and
# two concurrent runs BLEND their logs into numbers that look normal and mean nothing.
source "$(dirname "$0")/lib_runlock.sh"
acquire_run_lock "$(basename "$0")" || exit 1
( sleep 1500; adb emu kill 2>/dev/null; pkill -9 -f qemu 2>/dev/null ) &
APK=/work/out/angrybirds-8.0.3-x86shim-release.apk
OUT=/work/reports/shots; mkdir -p "$OUT"; LOG="$OUT/modern34.txt"; : >"$LOG"
ABLOG="$OUT/modern34_abshim.txt"; : >"$ABLOG"; ERRLOG="$OUT/modern34_errors.txt"; : >"$ERRLOG"
say(){ echo "$@" | tee -a "$LOG"; }

say "== boot API 34 emulator (abtest34) =="
emulator -avd abtest34 -no-window -no-audio -no-boot-anim -no-snapshot -accel on \
         -gpu swiftshader_indirect -partition-size 6144 -wipe-data >/tmp/emu34.log 2>&1 &
adb wait-for-device
for i in $(seq 1 200); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = 1 ] && break; sleep 2; done
sleep 10
say "  android: $(adb shell getprop ro.build.version.release 2>/dev/null|tr -d '\r') (API $(adb shell getprop ro.build.version.sdk 2>/dev/null|tr -d '\r'))  abi=$(adb shell getprop ro.product.cpu.abi 2>/dev/null|tr -d '\r')"
adb shell settings put global airplane_mode_on 1 >/dev/null 2>&1

say "== Q1: install a targetSdk=26 app on API 34 (low-target-sdk block?) =="
adb push "$APK" /data/local/tmp/ab.apk >/tmp/push.log 2>&1
R=$(adb shell pm install -r -d /data/local/tmp/ab.apk 2>&1 | tr -d '\r')
say "  pm install: [$R]"
if echo "$R" | grep -q Success; then INST=ok; say "  => INSTALL=OK (plain install; targetSdk=26 not blocked)"
else
  R2=$(adb shell pm install -r -d --bypass-low-target-sdk-block /data/local/tmp/ab.apk 2>&1 | tr -d '\r')
  say "  retry --bypass-low-target-sdk-block: [$R2]"
  echo "$R2" | grep -q Success && { INST=bypass; say "  => needs bypass flag (adb-only; note for the A56)"; } || INST=fail
fi
# exit 1, not 0. This line ANNOUNCES a failure and used to return success, so any caller checking
# the status saw a pass — the same "a skip is not a pass" defect found in mutation_test.sh's
# unavailable-image path. The rest of this script is deliberately informational (it reports Q1/Q2
# for a human to read, like the proof screenshots that must be looked at rather than scored), but
# a declared failure has to be reflected in the exit code.
[ "$INST" = fail ] && { say "  => INSTALL FAILED on API 34"; say DONE; adb emu kill; exit 1; }

adb logcat -c >/dev/null 2>&1; adb logcat -G 64M >/dev/null 2>&1
adb logcat -s abshim > "$ABLOG" 2>/dev/null &
adb logcat '*:W' > "$ERRLOG" 2>/dev/null &                         # catch avc/execmem/SIGSEGV
adb shell monkey -p com.rovio.angrybirds -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1

say "== Q2: does Unicorn JIT + init_array + render under modern W^X? =="
for s in $(seq 1 150); do sleep 5
  fn=$(grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null | tail -1 | grep -oE '[0-9]+')
  [ -n "$fn" ] && [ "$fn" -ge 60 ] && { say "  RENDERING on API 34: frame[$fn] at ~$((s*5))s"; break; }
done
sleep 6
adb exec-out screencap -p > "$OUT/modern34_screen.png" 2>/dev/null

say "== RESULTS =="
say "  Q1 install:   $INST"
say "  init_array:   $(grep -aoE 'init_array [0-9]+/125' "$ABLOG" 2>/dev/null | tail -1)  (125/125 => the ARM32 engine's C++ ctors ran = Unicorn JIT works)"
say "  JNI_OnLoad:   $(marker_report "$ABLOG" 'JNI_OnLoad')   last frame: $(grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null|tail -1)"
say "  h_fatal:      $(h_fatal_report "$ABLOG")"
say "  final pid:    [$(adb shell pidof com.rovio.angrybirds 2>/dev/null|tr -d '\r')]  (alive => running)"
say "  crash-sig:    $(adb logcat -d 2>/dev/null | grep -ciE 'SIGSEGV|Fatal signal|com.rovio.*died')"
say "  --- W^X / execmem / SELinux denials touching the app (JIT-blocking? MUST be none) ---"
DEN=$(grep -aiE 'avc: *denied.*(execmem|execmod|execute)|execmem|not executable|mprotect.*EACCES' "$ERRLOG" 2>/dev/null | grep -aiE 'rovio|angrybird|untrusted_app|execmem|execmod' | head -6)
[ -n "$DEN" ] && say "$DEN" || say "  NONE — no execmem/execmod/W^X denial (Unicorn JIT got executable memory)"
say DONE
adb emu kill >/dev/null 2>&1
