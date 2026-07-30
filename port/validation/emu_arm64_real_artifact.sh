#!/bin/bash
# emu_arm64_real_artifact.sh — run THE ACTUAL DELIVERABLE on a real ARM64 Android runtime.
#
# WHY THIS EXISTS
# ---------------
# Every play proof in this repo comes from the **x86 proxy** (`…-x86shim-release.apk`): the same
# shim source, the same 32-bit engine payload, but compiled for x86_64 and running in x86 ART. That
# is a good proxy — it exercises the emulation, the bridges, the allocator and the whole game loop —
# but it is not the artifact anyone installs. The file the user actually gets,
# `angrybirds-8.0.3-arm64.apk`, has never been executed by an ARM64 Android.
#
# What HAS covered the arm64 side so far:
#   - the host test suite runs the shim's core on x86 with no arm64 runtime;
#   - `arm64_unicorn_test.sh` runs engine-load + 125 constructors on the arm64 ABI under qemu-user;
#   - `verify_claims.sh` checks the shipped ELF is AArch64, 16 KB-aligned, links libm, etc.
# None of those is "the real APK, installed, in ART, on ARM64".
#
# This closes that gap as far as it can be closed without the phone. The `ab-emu-arm64` image has an
# `arm64-v8a` AVD (android-30 / google_apis) that was built and then never used by anything.
#
# EXPECT THIS TO BE SLOW, AND POSSIBLY TOO SLOW
# ---------------------------------------------
# There is no KVM acceleration for arm64 guests on an x86 host, so the whole Android system runs
# under QEMU TCG. On top of that, our shim emulates ARM32 *inside* that emulated ARM64 — emulation
# within emulation. Boot alone can take tens of minutes and the game may never reach a playable
# frame rate.
#
# So this script is written to report *how far it got*, not to pass/fail on reaching a win. The
# milestones below are cumulative, and each one that is reached is real evidence about the actual
# deliverable:
#
#   1 installed              — targetSdk 26 accepted by a real ARM64 Android
#   2 process started        — ART loaded the app
#   3 shim loaded            — the AArch64 .so resolved its symbols (this is where a missing -lm
#                              or a 4 KB-aligned ELF would fail, on the real ABI rather than in a check)
#   4 engine mapped          — the 32-bit payload was found and mmap'd
#   5 constructors ran       — Unicorn's JIT executed ARM32 code under ART's W^X on real ARM64
#   6 frames rendered        — the game loop is running
#
#   docker run --rm --network none -v "$PWD":/work ab-emu-arm64 \
#       bash /work/port/validation/emu_arm64_real_artifact.sh
set +e
source "$(dirname "$0")/lib_metrics.sh"
# Refuse to run while another evidence run is in flight: 50 scripts share reports/shots and
# two concurrent runs BLEND their logs into numbers that look normal and mean nothing.
source "$(dirname "$0")/lib_runlock.sh"
acquire_run_lock "$(basename "$0")" || exit 1
BUDGET="${ABSHIM_ARM64_BUDGET:-5400}"          # overall wall-clock budget, default 90 min
( sleep "$BUDGET"; adb emu kill 2>/dev/null; pkill -9 -f qemu 2>/dev/null ) &
APK=/work/out/angrybirds-8.0.3-arm64.apk       # THE REAL DELIVERABLE, not a proxy
OUT=/work/reports/shots; mkdir -p "$OUT"
LOG="$OUT/arm64_real.txt"; : >"$LOG"
ABLOG="$OUT/arm64_real_abshim.txt"; : >"$ABLOG"
say(){ echo "$@" | tee -a "$LOG"; }
[ -f "$APK" ] || { say "FATAL: $APK missing — run port/build_apk.sh first"; exit 1; }
say "artifact: $(sha256sum "$APK" | cut -d' ' -f1)"
say "host arch: $(uname -m)  (arm64 guest => full TCG emulation, no KVM)"

say "== boot arm64 Android (android-30) — expect this to take a long time =="
emulator -avd arm64 -no-window -no-audio -no-boot-anim -no-snapshot \
         -gpu swiftshader_indirect -partition-size 6144 -wipe-data >/tmp/emu64.log 2>&1 &
# Detect an emulator that FAILED TO START before waiting out the whole boot budget. The first run
# of this script sat for 21 minutes waiting for a boot that had been impossible ~3 seconds in:
#   FATAL | Avd's CPU Architecture 'arm64' is not supported by the QEMU2 emulator on x86_64 host.
# Poll the emulator's own log for that and bail immediately with the real reason.
BOOTED=0
for i in $(seq 1 900); do
  if grep -qE "^FATAL|not supported by the QEMU2 emulator" /tmp/emu64.log 2>/dev/null; then
    say "  EMULATOR REFUSED TO START:"
    grep -E "^FATAL|not supported by the QEMU2 emulator" /tmp/emu64.log | head -3 | sed 's/^/    /' | tee -a "$LOG"
    say ""
    say "  This host is x86_64 and the Android emulator requires the system image to match the host"
    say "  architecture, so the REAL arm64 APK cannot be executed here by this route at all."
    say "  What does cover the arm64 side: port/validation/arm64_unicorn_test.sh (qemu-USER, which"
    say "  translates user-space only and therefore does work), plus verify_claims.sh's checks on the"
    say "  shipped ELF. See port/OPEN_FINDINGS.md."
    say "MILESTONE: 0 (emulator cannot run an arm64 image on this host)"
    say DONE; adb emu kill >/dev/null 2>&1; exit 0
  fi
  [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = 1 ] && { BOOTED=1; say "  booted after ~$((i*4))s"; break; }
  sleep 4
done
[ "$BOOTED" = 1 ] || { say "  DID NOT BOOT within the budget — see /tmp/emu64.log"; say "MILESTONE: 0 (no boot)"; say DONE; adb emu kill; exit 0; }
say "  android $(adb shell getprop ro.build.version.release 2>/dev/null|tr -d '\r') / API $(adb shell getprop ro.build.version.sdk 2>/dev/null|tr -d '\r') / abi $(adb shell getprop ro.product.cpu.abi 2>/dev/null|tr -d '\r')"
sleep 15

M=0
say "== install THE REAL arm64 APK =="
adb shell settings put global airplane_mode_on 1 >/dev/null 2>&1
adb push "$APK" /data/local/tmp/ab.apk >/dev/null 2>&1
for t in 1 2 3; do adb shell pm install -r -d /data/local/tmp/ab.apk 2>&1 | tee -a "$LOG" | grep -q Success && { M=1; break; }; sleep 6; done
[ "$M" = 1 ] && say "  [1] INSTALLED on real ARM64 Android" || { say "  install FAILED"; say "MILESTONE: 0"; say DONE; adb emu kill; exit 0; }

adb logcat -c >/dev/null 2>&1; adb logcat -G 64M >/dev/null 2>&1
adb logcat -s abshim AndroidRuntime ActivityManager > "$ABLOG" 2>/dev/null &
adb shell monkey -p com.rovio.angrybirds -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1

say "== watch milestones (polling; TCG is slow) =="
for i in $(seq 1 400); do
  sleep 10
  pid=$(adb shell pidof com.rovio.angrybirds 2>/dev/null | tr -d '\r')
  [ -n "$pid" ] && [ "$M" -lt 2 ] && { M=2; say "  [2] process started (pid $pid)"; }
  grep -aq "JNI_OnLoad\|abshim" "$ABLOG" 2>/dev/null && [ "$M" -lt 3 ] && { M=3; say "  [3] shim loaded and logging (AArch64 .so resolved its symbols)"; }
  grep -aq "engine .* bytes from" "$ABLOG" 2>/dev/null && [ "$M" -lt 4 ] && { M=4; say "  [4] engine mapped: $(grep -a 'engine .* bytes from' "$ABLOG" | head -1 | sed 's/.*abshim *: *//')"; }
  grep -aqE "ctors|init_array|constructors" "$ABLOG" 2>/dev/null && [ "$M" -lt 5 ] && { M=5; say "  [5] constructors executed under ART W^X on real ARM64"; }
  fn=$(grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null | tail -1)
  [ -n "$fn" ] && [ "$M" -lt 6 ] && { M=6; say "  [6] rendering: $fn"; }
  # a crash is a result too - record and stop
  if grep -aqE "FATAL EXCEPTION|UnsatisfiedLinkError|has died" "$ABLOG" 2>/dev/null; then
    say "  CRASH detected:"; grep -aE "FATAL EXCEPTION|UnsatisfiedLinkError|has died|at com.rovio" "$ABLOG" | head -8 | sed 's/^/    /' | tee -a "$LOG"
    break
  fi
  [ "$M" -ge 6 ] && [ $((i % 6)) = 0 ] && say "  ...still running, $fn"
done

say ""
say "== RESULT =="
say "  furthest milestone reached: $M / 6"
say "  1=installed 2=process 3=shim loaded 4=engine mapped 5=ctors ran 6=frames"
say "  final pid:   [$(adb shell pidof com.rovio.angrybirds 2>/dev/null|tr -d '\r')]"
say "  last frame:  $(grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null | tail -1)"
say "  h_fatal:     $(h_fatal_report "$ABLOG")"
say "  NOTE: a low milestone here is about TCG speed, not necessarily about the artifact. Read the"
say "        milestone list: everything up to the one reached is genuine evidence about the real APK."
say DONE
adb emu kill >/dev/null 2>&1
