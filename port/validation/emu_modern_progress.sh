#!/bin/bash
# FINAL modern-Android completeness: confirm MULTI-LEVEL PROGRESSION on API 34 (Android 14) — the
# game not only wins a level but ADVANCES into the next one (the level-2 load path) on the A56's
# OS regime. Extends emu_modern_playthrough.sh with the orange NEXT tap + a level-2 confirmation.
set +e
# AVD and output prefix overridable so this also runs on the API 36 tier (the A56's actual OS).
# Defaults reproduce the original behaviour exactly.
AVD="${ABSHIM_AVD:-abtest34}"
PFX="${ABSHIM_OUTPFX:-modprog}"
source "$(dirname "$0")/lib_settle.sh"
source "$(dirname "$0")/lib_provenance.sh"
source "$(dirname "$0")/lib_metrics.sh"
source "$(dirname "$0")/lib_dialogs.sh"   # frame-based settle (replaces flaky fixed sleeps)
source "$(dirname "$0")/lib_install.sh"
source "$(dirname "$0")/lib_selfhash.sh"
( sleep 1550; adb emu kill 2>/dev/null; pkill -9 -f qemu 2>/dev/null ) &
APK=/work/out/angrybirds-8.0.3-x86shim-release.apk
OUT=/work/reports/shots; mkdir -p "$OUT"; LOG="$OUT/${PFX}.txt"; : >"$LOG"
ABLOG="$OUT/${PFX}_abshim.txt"; : >"$ABLOG"
say(){ echo "$@" | tee -a "$LOG"; }
selfhash_begin   # see lib_selfhash.sh: detects a mid-run edit of THIS file
fnow(){ grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null|tail -1|grep -oE '[0-9]+'; }
# NOT "API 34": ABSHIM_AVD is parameterised, so running this against ab36 (Android 16) printed a
# header claiming API 34 — a log kept as evidence stating the wrong OS under test. The version is
# reported from the device a few lines below; assert nothing here that was not measured.
say "== boot $AVD =="
emulator -avd "$AVD" -no-window -no-audio -no-boot-anim -no-snapshot -accel on \
         -gpu swiftshader_indirect -partition-size 6144 -wipe-data >/tmp/emu34.log 2>&1 &
adb wait-for-device
for i in $(seq 1 200); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = 1 ] && break; sleep 2; done
sleep 10
say "  android $(adb shell getprop ro.build.version.release 2>/dev/null|tr -d '\r') (API $(adb shell getprop ro.build.version.sdk 2>/dev/null|tr -d '\r'))"
adb shell settings put global airplane_mode_on 1 >/dev/null 2>&1
# see lib_install.sh: boot_completed=1 does not mean the package service can accept a ~98MB APK.
# Observed "Failure calling service package: Broken pipe (32)", which reads exactly like a broken
# build. install_apk waits for the service, retries transient errors, and reports a real rejection
# differently. NOT read through a pipe — $? would be tee's, the defect that once made a check print
# [FAIL] and exit 0.
install_apk "$APK" 4 > /tmp/inst.$$ 2>&1; irc=$?
while IFS= read -r _l; do say "$_l"; done < /tmp/inst.$$; rm -f /tmp/inst.$$
[ "$irc" -eq 0 ] || { say "install FAIL"; say DONE; adb emu kill; exit 1; }
record_build "$APK" "$PFX"   # MUST follow $PFX: a fixed label lets an ab36 run overwrite the API-34 row
adb logcat -c >/dev/null 2>&1; adb logcat -G 32M >/dev/null 2>&1
adb logcat -s abshim > "$ABLOG" 2>/dev/null &
adb shell monkey -p com.rovio.angrybirds -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
say "== wait render (frame[601]+), dismiss dialog, play to win =="
for s in $(seq 1 130); do sleep 5; fn=$(fnow); [ -n "$fn" ] && [ "$fn" -ge 601 ] && { say "  card frame[$fn]"; break; }; done
sleep 4
# Was a single tap at the API-25/34 "OK" position. On API 36 that is not enough: Android 16 draws
# its "Viewing full screen" notice ON TOP, swallowing every touch, so this script climbed to
# frame[1801] with h_fatal=0 and never advanced - looking like a shim failure that was not one.
dismiss_system_dialogs "$OUT/${PFX}_1b_afterdialogs.png"
adb shell input tap 390 266; sleep 4; adb shell input tap 390 266; sleep 12
adb shell input swipe 207 118 110 150 700; sleep 8
adb shell input swipe 207 118 122 140 700; sleep 8
adb shell input swipe 207 118 118 136 700
# FIX (2026-07-27): was `sleep 16` — a fixed wall-clock settle against a frame rate that
# varies ~1.8-15 fps between runs, so it silently captured mid-level on slow runs. See lib_settle.sh.
settle_frames "$ABLOG" 120 300
adb exec-out screencap -p > "$OUT/${PFX}_1_cleared.png" 2>/dev/null
# NOTE (2026-07-27): `levelComplete` grep-count removed — it counted levelCompleteStars*.lua
# asset preloads (always 8, before frame[1]), identical in winning and non-winning runs.
LC=$(marker_report "$ABLOG" 'levelCompleteStars'); say "  starsAssetPreloads=$LC (NOT a win signal) frame=$(fnow) h_fatal=$(h_fatal_report "$ABLOG")"
say "== tap orange NEXT (378,256) -> does level 2 load on modern Android? =="
adb shell input tap 378 256; sleep 3; adb shell input tap 378 256
# FIX (2026-07-27): was a fixed `sleep 16` gating the level-2 screenshot — i.e. gating PROOF_9,
# the multi-level-progression evidence. Same flakiness as the last-drag settle: on a slow run
# 16s is ~29 frames, so this could capture BEFORE level 2 finishes loading and the resulting
# image would understate a working build. Wait on frames instead.
settle_frames "$ABLOG" 120 300
adb exec-out screencap -p > "$OUT/${PFX}_2_level2.png" 2>/dev/null
say "== RESULTS (modern-Android multi-level progression) =="
say "  install:           ok"
say "  win/advance check: SCREENSHOTS ONLY -> ${PFX}_1_cleared.png (win) + ${PFX}_2_level2.png (level 2)"
say "  starsAssetPreloads:$LC  (NOT a win signal — see note above)"
say "  h_fatal:           $(h_fatal_report "$ABLOG")  (0 = no crash across win + advance)"
say "  s-construct-guard: $(marker_report "$ABLOG" 's-construct-null-guard')"
say "  real St11logic:    $(marker_report "$ABLOG" 'THROW.*St11logic_error')"
say "  last frame:        $(fnow)"
say "  final pid:         [$(adb shell pidof com.rovio.angrybirds 2>/dev/null|tr -d '\r')]  (alive => advanced OK)"
selfhash_verify
say DONE
adb emu kill >/dev/null 2>&1
