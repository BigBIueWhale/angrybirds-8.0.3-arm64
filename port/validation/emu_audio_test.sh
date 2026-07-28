#!/bin/bash
# AUDIO TEST: run the -DABSHIM_AUDIO proxy WITH emulator audio ENABLED (no -no-audio; QEMU null
# driver so AudioTrack still inits in the emulated HAL). Two questions:
#  (1) does nativeMixData get CALLED (i.e., AudioTrack.startOutput succeeds + the audio HandlerThread
#      runs the mixer)?  (2) does the game keep PLAYING with no freeze — i.e., the audio thread's
#      mixer does NOT starve render on the BEL (the cont.24 hazard)?  Headless => can't hear it, but
#      "pipeline runs + no freeze + no crash" is the decisive safety result (blocking AudioTrack.write
#      is Java-side, not under the BEL, per AudioOutput.java).
set +e
source "$(dirname "$0")/lib_settle.sh"
source "$(dirname "$0")/lib_metrics.sh"   # frame-based settle (replaces flaky fixed sleeps)
source "$(dirname "$0")/lib_install.sh"
( sleep 1400; adb emu kill 2>/dev/null; pkill -9 -f qemu 2>/dev/null ) &
APK=/work/out/angrybirds-8.0.3-x86shim-audio.apk
OUT=/work/reports/shots; mkdir -p "$OUT"; LOG="$OUT/audio.txt"; : >"$LOG"
ABLOG="$OUT/audio_abshim.txt"; : >"$ABLOG"
say(){ echo "$@" | tee -a "$LOG"; }
fnow(){ grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null|tail -1|grep -oE '[0-9]+'; }
say "== boot emulator WITH audio (no -no-audio; QEMU_AUDIO_DRV=none) =="
export QEMU_AUDIO_DRV=none
emulator -avd abtest -no-window -no-boot-anim -no-snapshot -accel on \
         -gpu swiftshader_indirect -partition-size 6144 -wipe-data >/tmp/emu.log 2>&1 &
adb wait-for-device
for i in $(seq 1 150); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = 1 ] && break; sleep 2; done
sleep 8
adb shell settings put global airplane_mode_on 1 >/dev/null 2>&1
# see lib_install.sh: boot_completed=1 does not mean the package service can accept a ~98MB APK.
# Observed "Failure calling service package: Broken pipe (32)", which reads exactly like a broken
# build. install_apk waits for the service, retries transient errors, and reports a real rejection
# differently. NOT read through a pipe — $? would be tee's, the defect that once made a check print
# [FAIL] and exit 0.
install_apk "$APK" 4 > /tmp/inst.$$ 2>&1; irc=$?
while IFS= read -r _l; do say "$_l"; done < /tmp/inst.$$; rm -f /tmp/inst.$$
[ "$irc" -eq 0 ] || { say "install FAIL"; say DONE; adb emu kill; exit 1; }
adb logcat -c >/dev/null 2>&1; adb logcat -G 32M >/dev/null 2>&1
adb logcat -s abshim > "$ABLOG" 2>/dev/null &
adb shell monkey -p com.rovio.angrybirds -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
say "== wait for tutorial card (frame[601]+), then PLAY (audio active) =="
for s in $(seq 1 130); do sleep 5; fn=$(fnow); [ -n "$fn" ]&&[ "$fn" -ge 601 ]&&break; done
FB=$(fnow); MB=$(marker_report "$ABLOG" 'nativeMixData ENABLED'); say "  card at frame[$FB]  (nativeMixData calls so far: $MB)"
adb shell input tap 390 266; sleep 4; adb shell input tap 390 266; sleep 12
adb shell input swipe 207 118 110 150 700; sleep 8
adb shell input swipe 207 118 122 140 700; sleep 8
adb shell input swipe 207 118 118 136 700
# FIX (2026-07-27): was `sleep 16` — a fixed wall-clock settle against a frame rate that
# varies ~1.8-15 fps between runs, so it silently captured mid-level on slow runs. See lib_settle.sh.
settle_frames "$ABLOG" 120 300
FE=$(fnow)
adb exec-out screencap -p > "$OUT/audio_end.png" 2>/dev/null
say "== RESULTS (audio) =="
say "  install:                 ok"
say "  nativeMixData CALLED:    $(marker_report "$ABLOG" 'nativeMixData ENABLED')  (>0 => AudioTrack inited + mixer running)"
say "  frames: card=$FB after-play=$FE  => $([ -n "$FE" ] && [ -n "$FB" ] && [ "$FE" -gt "$FB" ] && echo 'ADVANCED (no audio freeze/deadlock)' || echo 'CHECK (did frames stall?)')"
# NOTE (2026-07-27): `levelComplete` grep-count removed — it counted levelCompleteStars*.lua
# asset preloads (always 8, before frame[1]), not wins. This test's subject is the audio mixer.
say "  starsAssetPreloads:      $(marker_report "$ABLOG" levelCompleteStars)  (NOT a win signal)"
say "  h_fatal:                 $(h_fatal_report "$ABLOG")"
say "  crash-sig:               $(adb logcat -d 2>/dev/null|grep -ciE 'SIGSEGV|Fatal signal|com.rovio.*died')"
say "  final pid:               [$(adb shell pidof com.rovio.angrybirds 2>/dev/null|tr -d '\r')]"
say "  --- AudioTrack/AudioOutput status in full logcat ---"
adb logcat -d 2>/dev/null | grep -aiE 'AudioOutput|AudioTrack.*(error|fail|init|obtainBuffer)|startOutput' | head -6 | tee -a "$LOG"
say DONE
adb emu kill >/dev/null 2>&1
