#!/bin/bash
# emu_perf_split.sh — where does a frame's time actually go? Reproducibly.
#
# WHY THIS EXISTS
# ---------------
# The frame-time split is the one number in this project that says what performance on the A56 will
# be limited BY, and it was measured twice wrongly before it was measured right:
#
#   1. A timer around uc_emu_start reported a CONSTANT call count while frames advanced 300 -> 600
#      -> 900. After boot the guest's whole render loop runs inside ONE long-lived uc_emu_start
#      with bridges invoked from hooks during it, so the timer measured the entire run.
#   2. Process-wide accumulators reported IN 93% + OUT 65% = 158% of wall time. shim_call is
#      entered from several ART threads, so summing their durations double-counts.
#
# Neither was caught by a test. Both were caught because the arithmetic was impossible. That is a
# bad way to have to find out, and it is the reason this script exists rather than another ad-hoc
# run: the measurement is now repeatable by anyone, and the impossible cases are ASSERTED instead
# of being left for a reader to spot.
#
# WHAT IT REPORTS
#   IN-shim   = emulator (Unicorn running ARM32 + hook dispatch + scheduler)
#             + bridges  (GL + asset + libc + file + the bent table, all via stub_cb)
#             + JNI      (guest->JVM passthrough, which arrives on its own hook at RG_JNI)
#   OUT-shim  = eglSwapBuffers + rasterisation + vsync, i.e. everything on the Java side.
#
# The emulator/bridges split is the actionable part: bridge cost is ours to optimise, emulator cost
# is close to a hard ceiling short of replacing Unicorn.
#
# MEASURED ON THE RELEASE CONFIGURATION. build_apk_x86_perf.sh is -DABSHIM_RELEASE plus -DABSHIM_PERF
# (timers only, no heavy diagnostics). Measuring on the diagnostic build overstates emulation's
# share — it runs ~17 fps against release's ~24 — so a number from it would not describe what a user
# gets. ABSHIM_PERF is never defined for the shipped APK; verify_claims.sh checks that.
#
# NOT A PREDICTION FOR THE PHONE. This is an x86_64 host under SwiftShader. The A56 has a different
# CPU, a different memory system and a real GPU. The SHAPE of the split is the transferable part.
#
#   docker build -f port/docker/Dockerfile.ab-emu-34 -t ab-emu-34 port/docker
#   docker run --rm --network none --device /dev/kvm --group-add "$(getent group kvm|cut -d: -f3)" \
#       -v "$PWD":/work ab-emu-34 bash /work/port/validation/emu_perf_split.sh
set +e
source "$(dirname "$0")/lib_settle.sh"
source "$(dirname "$0")/lib_dialogs.sh"
source "$(dirname "$0")/lib_metrics.sh"
source "$(dirname "$0")/lib_install.sh"
source "$(dirname "$0")/lib_selfhash.sh"

APK=/work/out/angrybirds-8.0.3-x86shim-perf.apk
AVD="${ABSHIM_AVD:-abtest34}"
PFX="${ABSHIM_OUTPFX:-perfsplit}"
OUT=/work/reports/shots; mkdir -p "$OUT"; LOG="$OUT/${PFX}.txt"; : >"$LOG"
ABLOG="$OUT/${PFX}_abshim.txt"; : >"$ABLOG"
say(){ echo "$@" | tee -a "$LOG"; }
selfhash_begin   # see lib_selfhash.sh: detects a mid-run edit of THIS file
FAIL=0

if [ ! -f "$APK" ]; then
    say "[FAIL] $APK is missing — build it first:"
    say "       docker run --rm --network none -v \"\$PWD\":/work -w /work ab-port bash port/build_apk_x86_perf.sh"
    exit 1
fi

# Tie the numbers to a binary. Deliberately NOT via record_build/provenance.tsv: that ledger exists
# to pin SCREENSHOTS to deliverables, and this APK is a measurement tool that gets rebuilt often —
# adding it there would generate perpetual STALE rows about a build nobody ships. A hash in the log
# is enough to answer "which binary produced these numbers".
say "  measuring: $(basename "$APK")  sha256 $(sha256sum "$APK" | cut -c1-16)…"

# NOT "API 34": $AVD is parameterised (ABSHIM_AVD=ab36 runs Android 16, the A56's OS), so a
# hardcoded version here prints "boot API 34 (ab36)" on an Android 16 run — a log line that
# misstates what was under test, in a file kept as evidence. The very next line reports the
# version read FROM THE DEVICE, so state nothing here that is not measured.
say "== boot $AVD =="
emulator -avd "$AVD" -no-window -no-audio -no-boot-anim -no-snapshot -accel on \
    -gpu swiftshader_indirect >/tmp/emu.log 2>&1 &
adb wait-for-device
for i in $(seq 1 120); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = "1" ] && break; sleep 5; done
say "  android $(adb shell getprop ro.build.version.release 2>/dev/null|tr -d '\r') (API $(adb shell getprop ro.build.version.sdk 2>/dev/null|tr -d '\r'))"

adb shell settings put global airplane_mode_on 1 >/dev/null 2>&1
# see lib_install.sh: boot_completed=1 does not mean the package service can take a 98 MB APK
# NOT `install_apk ... | tee ... || ...` — through a pipe $? is tee's, so a failed install would
# sail straight past the ||. That is the same defect that once made verify_claims.sh print [FAIL]
# and exit 0. Capture the status directly.
install_apk "$APK" 4 >/tmp/inst.txt 2>&1; irc=$?
cat /tmp/inst.txt | tee -a "$LOG"
[ "$irc" -eq 0 ] || { say "[FAIL] install"; adb emu kill; exit 1; }
adb logcat -c >/dev/null 2>&1; adb logcat -G 32M >/dev/null 2>&1
adb logcat -s abshim > "$ABLOG" 2>/dev/null &
adb shell monkey -p com.rovio.angrybirds -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1

say "== wait for render, then drive a real workload =="
for s in $(seq 1 130); do sleep 5
    fn=$(grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null|tail -1|grep -oE '[0-9]+')
    [ -n "$fn" ] && [ "$fn" -ge 601 ] && { say "  rendering at ~$((s*5))s frame[$fn]"; break; }
done
dismiss_system_dialogs
# Measure a PLAYING game, not a menu. A static screen has little geometry and few libc bridges, so
# a split measured there would flatter the emulator and misdescribe what the phone will do.
adb shell input tap 390 266; sleep 4; adb shell input tap 390 266; sleep 12
adb shell input swipe 207 118 110 150 700; sleep 8
adb shell input swipe 207 118 122 140 700; sleep 8
adb shell input swipe 207 118 118 136 700
settle_frames "$ABLOG" 600 600

say
say "== RESULTS =="
# Refuse to report on an empty log — the lib_metrics rule: a count of 0 from a log that was never
# written says nothing at all, and this project has produced that exact false-clean four times.
if [ ! -s "$ABLOG" ]; then
    say "  [FAIL] the shim log is empty — nothing was measured, so no split can be reported"
    adb emu kill; exit 1
fi
say "  h_fatal: $(h_fatal_report "$ABLOG")"

SPLITS=$(grep -a '\[perf-split\]' "$ABLOG")
NS=$(printf '%s' "$SPLITS" | grep -c . )
say "  [perf-split] samples: $NS"
if [ "$NS" -lt 2 ]; then
    say "  [FAIL] fewer than 2 steady-state samples — the run did not reach a measurable state."
    say "         (A single sample includes boot, which is not what this measures.)"
    adb emu kill; exit 1
fi

# Drop sample 1. Its window runs from the first frame native to frame 300, so it contains boot and
# first-level load: measured wall=139693ms against ~12500ms for a steady-state window. Averaging it
# in would not describe the game running, and quoting it as "98% emulation" would be quoting the
# loading screen. The invariant checks below therefore also skip it.
say "  (sample 1 excluded from steady state: its window contains boot — wall was ~140s vs ~12.5s)"
say
grep -a '\[perf\]' "$ABLOG" | tail -n +2 | tail -6 | sed 's/^/    /' | tee -a "$LOG"
say
printf '%s\n' "$SPLITS" | tail -n +2 | tail -6 | sed 's/^/    /' | tee -a "$LOG"

say
say "== INVARIANTS =="
# The shim itself flags accounting violations inline; a single occurrence invalidates the numbers.
if printf '%s\n' "$SPLITS" | tail -n +2 | grep -q 'BAD: accounting violated'; then
    say "  [FAIL] the shim reported an accounting violation (GL > bridges, or bridges+JNI > IN-shim)."
    say "         The split is not trustworthy; fix the accounting before quoting any number."
    FAIL=1
else
    say "  [ OK ] no accounting violation: GL <= bridges, and bridges+JNI <= IN-shim, in every sample"
fi
# IN + OUT must be ~100% of wall. This is the check that would have caught the 158% error.
BADSUM=$(grep -a '\[perf\]' "$ABLOG" | tail -n +2 | tail -6 | sed -n 's/.*IN-shim=[0-9]*ms (\([0-9]*\)%).*OUT-shim[^(]*(\([0-9]*\)%).*/\1 \2/p' \
         | awk '{t=$1+$2; if(t<90||t>110) print t}' | wc -l)
if [ "$BADSUM" -gt 0 ]; then
    say "  [FAIL] $BADSUM sample(s) where IN%+OUT% is outside 90-110 — durations are being"
    say "         double-counted or lost. This is exactly the 158%-of-wall-time failure."
    FAIL=1
else
    say "  [ OK ] IN% + OUT% within 90-110 in every sample (no double-counting across threads)"
fi
# entries vs frames. The real invariant is entries >= 300, NOT == 300: every frame is one shim_call,
# but shim_call also carries input, lifecycle and level-load natives, so an excess is expected and
# healthy (measured 300, 302, 310, 435 — the 435 is a level load). FEWER than 300 would mean frames
# were counted that no shim_call produced, i.e. the two counters are measuring different things and
# the window is not a frame window. The first version of this check asserted ==300 and so reported a
# warning on a perfectly good run; a check that cries wolf on correct behaviour trains you to ignore
# it, which is worse than not having it.
ENTBAD=$(grep -a '\[perf\]' "$ABLOG" | tail -n +2 | sed -n 's/.*entries=\([0-9]*\).*/\1/p' | awk '$1<300' | wc -l)
if [ "$ENTBAD" -gt 0 ]; then
    say "  [FAIL] $ENTBAD sample(s) with entries < 300 — impossible if one frame is one shim_call."
    say "         The frame counter and the shim_call counter are not measuring the same window."
    FAIL=1
else
    say "  [ OK ] entries >= 300 in every sample (excess = input/lifecycle/level-load natives)"
fi

say
say "  Reminder: x86_64 host under SwiftShader. The SHAPE of the split transfers to the A56;"
say "  the absolute frame rate does not."
# selfhash_verify RETURNS a verdict (0 unchanged / 1 edited mid-run / 2 cannot tell). It was
# called and discarded here, so a run whose script changed underneath it printed
# "*** DISCARD THESE RESULTS ***" and still exited 0 — the one failure that invalidates every
# other line above it.
selfhash_verify; [ $? -eq 0 ] || FAIL=$((FAIL+1))
say DONE
adb emu kill >/dev/null 2>&1
exit "$FAIL"
