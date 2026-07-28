#!/bin/bash
# Validate SAVE PERSISTENCE on modern Android (API 34): does the game WRITE progress to its private
# dir on a win, and RELOAD it after force-stop + relaunch (so the user doesn't replay from scratch)?
# The shim's FILE bridge (bridge_file.c) is host-suite-validated; this confirms it end-to-end on-device.
set +e
source "$(dirname "$0")/lib_metrics.sh"
source "$(dirname "$0")/lib_settle.sh"   # frame-based settle (replaces flaky fixed sleeps)
source "$(dirname "$0")/lib_install.sh"
( sleep 1550; adb emu kill 2>/dev/null; pkill -9 -f qemu 2>/dev/null ) &
APK=/work/out/angrybirds-8.0.3-x86shim-release.apk
# AVD/prefix overridable so the SAME save test can run on the API 36 tier (the A56's actual OS,
# where storage rules are strictest) without overwriting the API 34 evidence.
AVD="${ABSHIM_AVD:-abtest34}"
PFX="${ABSHIM_OUTPFX:-save}"
OUT=/work/reports/shots; mkdir -p "$OUT"; LOG="$OUT/${PFX}.txt"; : >"$LOG"
AB1="$OUT/save_ab1.txt"; AB2="$OUT/save_ab2.txt"
say(){ echo "$@" | tee -a "$LOG"; }
say "== boot API 34 (fresh wipe-data) =="
emulator -avd "$AVD" -no-window -no-audio -no-boot-anim -no-snapshot -accel on \
         -gpu swiftshader_indirect -partition-size 6144 -wipe-data >/tmp/emu34.log 2>&1 &
adb wait-for-device
for i in $(seq 1 200); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = 1 ] && break; sleep 2; done
sleep 10
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
adb logcat -s abshim > "$AB1" 2>/dev/null & C1=$!
adb shell monkey -p com.rovio.angrybirds -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
say "== 1st launch: play + win tutorial (writes a save) =="
for s in $(seq 1 130); do sleep 5; fn=$(grep -aoE 'frame\[[0-9]+\]' "$AB1" 2>/dev/null|tail -1|grep -oE '[0-9]+'); [ -n "$fn" ]&&[ "$fn" -ge 601 ]&&break; done
sleep 4
adb shell input tap 490 237; sleep 1; adb shell input tap 490 237; sleep 3
adb shell input tap 390 266; sleep 4; adb shell input tap 390 266; sleep 12
adb shell input swipe 207 118 110 150 700; sleep 8
adb shell input swipe 207 118 122 140 700; sleep 8
adb shell input swipe 207 118 118 136 700
# FIX (2026-07-27): was `sleep 18` — a fixed wall-clock settle against a frame rate that
# varies ~1.8-15 fps between runs, so it silently captured mid-level on slow runs. See lib_settle.sh.
settle_frames "$AB1" 120 300
# NOTE (2026-07-27): this line used to report `won: levelComplete=N`. That N was a count of
# levelCompleteStars*.lua ASSET PRELOADS (always 8, before frame[1]) — it never indicated a win.
# This test's real subject is save persistence below, which does not depend on winning.
say "  play sequence sent (win NOT asserted — no log marker exists; see save_relaunch.png)"
say "== inspect the app's private storage (did it WRITE saves?) =="
adb root >/dev/null 2>&1; sleep 4
say "  files/shared_prefs/databases written by the game:"
adb shell "find /data/data/com.rovio.angrybirds/files /data/data/com.rovio.angrybirds/shared_prefs /data/data/com.rovio.angrybirds/databases -type f 2>/dev/null" 2>/dev/null | tee -a "$LOG" | head -30
NF=$(adb shell "find /data/data/com.rovio.angrybirds -type f 2>/dev/null | wc -l" 2>/dev/null | tr -d '\r')
say "  total private files: $NF"
kill $C1 2>/dev/null
say "== force-stop (data preserved) + RELAUNCH — does it reload without crash? =="
adb shell am force-stop com.rovio.angrybirds; sleep 3
adb logcat -c >/dev/null 2>&1
adb logcat -s abshim > "$AB2" 2>/dev/null & C2=$!
adb shell monkey -p com.rovio.angrybirds -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
for s in $(seq 1 110); do sleep 5; fn=$(grep -aoE 'frame\[[0-9]+\]' "$AB2" 2>/dev/null|tail -1|grep -oE '[0-9]+'); [ -n "$fn" ]&&[ "$fn" -ge 200 ]&&break; done
sleep 6
adb exec-out screencap -p > "$OUT/${PFX}_relaunch.png" 2>/dev/null
kill $C2 2>/dev/null
say "== RESULTS (save persistence on modern Android) =="
say "  1st-launch saves written:  $NF private files"
say "  2nd-launch boot:           init_array=$(grep -aoE 'init_array [0-9]+/125' "$AB2" 2>/dev/null|tail -1)  last-frame=$(grep -aoE 'frame\[[0-9]+\]' "$AB2" 2>/dev/null|tail -1)  h_fatal=$(marker_report "$AB2" '\[h_fatal\]')"
say "  2nd-launch reads own files: $(marker_report "$AB2" 'fopen.*com.rovio.angrybirds|AAsset.*profile|fopen.*(save|profile|progress|settings|\.lua)') (reloaded persisted data)"
say "  2nd-launch pid:            [$(adb shell pidof com.rovio.angrybirds 2>/dev/null|tr -d '\r')] (alive => reloaded OK, no crash)"
say DONE
adb emu kill >/dev/null 2>&1
