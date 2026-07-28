#!/bin/bash
# emu_gpu_capture.sh — capture the whole GPU-facing surface in one run, reproducibly.
#
# WHY THIS EXISTS
# ---------------
# The GPU is one of only two surfaces no emulator here stands in for (OPEN_FINDINGS R9), so it gets
# the most scrutiny — and until now that scrutiny was applied by hand. The shader screen (R10) was
# reached through three ad-hoc captures across two builds, with the adb commands typed each time.
# That contradicts the project's own falsifiability claim, which says every script that produced the
# evidence is in the repo so the results can be re-run rather than believed. A result nobody else can
# regenerate is an anecdote, however careful the person was who produced it.
#
# It captures BOTH halves of the surface, because they fail differently:
#   * shader SOURCE   (R10) -> what the engine asks the driver to compile. Screened by shader_screen.py.
#   * driver CAPABILITIES   -> what the driver tells the engine about itself. Screened by gl_caps.py.
# The engine branches on the second to decide which textures and geometry paths to use, so a rig and
# a device that answer differently run different code. Both come from the same run here, because
# capturing them separately invites screening a shader set and a capability set that never coexisted.
#
# REQUIRES the capture build (-DABSHIM_RELEASE -DABSHIM_SHADERDUMP: release speed, dumps only):
#   docker run --rm --network none -v "$PWD":/work -w /work ab-port bash port/build_apk_x86_shaders.sh
#
#   docker build -f port/docker/Dockerfile.ab-emu-34 -t ab-emu-34 port/docker
#   docker run --rm --network none --device /dev/kvm --group-add "$(getent group kvm|cut -d: -f3)" \
#       -v "$PWD":/work ab-emu-34 bash /work/port/validation/emu_gpu_capture.sh
#
# ABSHIM_AVD=ab36 runs it on Android 16 (the A56's own OS version) instead of API 34.
set +e
source "$(dirname "$0")/lib_settle.sh"
source "$(dirname "$0")/lib_dialogs.sh"
source "$(dirname "$0")/lib_metrics.sh"
source "$(dirname "$0")/lib_install.sh"
source "$(dirname "$0")/lib_selfhash.sh"

APK=/work/out/angrybirds-8.0.3-x86shim-shaders.apk
AVD="${ABSHIM_AVD:-abtest34}"
PFX="${ABSHIM_OUTPFX:-gpucap}"
OUT=/work/reports/shots; mkdir -p "$OUT"; LOG="$OUT/${PFX}.txt"; : >"$LOG"
ABLOG="$OUT/${PFX}_abshim.txt"; : >"$ABLOG"
say(){ echo "$@" | tee -a "$LOG"; }
selfhash_begin   # see lib_selfhash.sh: detects a mid-run edit of THIS file
FAIL=0

if [ ! -f "$APK" ]; then
    say "[FAIL] $APK is missing — build the capture APK first:"
    say "       docker run --rm --network none -v \"\$PWD\":/work -w /work ab-port bash port/build_apk_x86_shaders.sh"
    exit 1
fi
say "  capturing with: $(basename "$APK")  sha256 $(sha256sum "$APK" | cut -c1-16)…"

# NOT a hardcoded "API 34": $AVD is parameterised, so the version is read FROM THE DEVICE below.
say "== boot $AVD =="
emulator -avd "$AVD" -no-window -no-audio -no-boot-anim -no-snapshot -accel on \
    -gpu swiftshader_indirect >/tmp/emu.log 2>&1 &
adb wait-for-device
for i in $(seq 1 120); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = "1" ] && break; sleep 5; done
say "  android $(adb shell getprop ro.build.version.release 2>/dev/null|tr -d '\r') (API $(adb shell getprop ro.build.version.sdk 2>/dev/null|tr -d '\r'))"
say "  gpu: swiftshader_indirect  <- NOT the A56's Mali/Xclipse; that is the entire point of the comparison"

adb shell settings put global airplane_mode_on 1 >/dev/null 2>&1
# see lib_install.sh: boot_completed=1 does not mean the package service can take a 98 MB APK.
# Status captured directly, NOT through a pipe — through `| tee` $? is tee's, and a failed install
# would sail past the ||.
install_apk "$APK" 4 >/tmp/inst.txt 2>&1; irc=$?
cat /tmp/inst.txt | tee -a "$LOG"
[ "$irc" -eq 0 ] || { say "[FAIL] install"; adb emu kill; exit 1; }
adb logcat -c >/dev/null 2>&1; adb logcat -G 64M >/dev/null 2>&1
adb logcat -s abshim > "$ABLOG" 2>/dev/null &
adb shell monkey -p com.rovio.angrybirds -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1

say "== wait for render, then PLAY — boot alone compiles only a subset =="
for s in $(seq 1 130); do sleep 5
    fn=$(grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null|tail -1|grep -oE '[0-9]+')
    [ -n "$fn" ] && [ "$fn" -ge 601 ] && { say "  rendering at ~$((s*5))s frame[$fn]"; break; }
done
dismiss_system_dialogs
# Drive actual gameplay. The shaders are an uber-shader whose variants are selected by #define
# combination, so a boot-only capture screens an unrepresentative subset — that was one of the three
# partial measurements that preceded R10. Play, win, and advance a level so the level-2 variants and
# the win-screen effects compile too.
adb shell input tap 390 266; sleep 4; adb shell input tap 390 266; sleep 12
for i in 1 2 3 4; do adb shell input swipe 207 118 110 150 700; sleep 8; done
adb shell input tap 390 266; sleep 6
settle_frames "$ABLOG" 600 600

say
say "== RESULTS =="
# The lib_metrics rule: a count of 0 from a log that was never written says nothing at all.
if [ ! -s "$ABLOG" ]; then
    say "  [FAIL] the shim log is empty — nothing was captured, so nothing can be screened."
    adb emu kill; exit 1
fi
say "  h_fatal: $(h_fatal_report "$ABLOG")"
NSH=$(grep -ac '\[shader-src\]' "$ABLOG"); NGL=$(grep -ac '\[gl-str\]' "$ABLOG")
say "  [shader-src] lines: $NSH     [gl-str] lines: $NGL"
if [ "$NSH" -eq 0 ] || [ "$NGL" -eq 0 ]; then
    say "  [FAIL] a dump produced no lines at all. Either the APK is not the -DABSHIM_SHADERDUMP"
    say "         build, or the run never reached the GL path. Screening this would report"
    say "         'nothing risky found', which would mean 'nothing measured'."
    adb emu kill; exit 1
fi

say
say "== DRIVER CAPABILITIES (gl_caps.py) =="
python3 /work/port/validation/gl_caps.py "$ABLOG" /work/work803/libv7/libAngryBirdsClassic.so \
        /work/reports/gl_extensions_rig.txt 2>&1 | tee -a "$LOG"
[ "${PIPESTATUS[0]}" -eq 0 ] || FAIL=1

say
say "== SHADER SCREEN (shader_screen.py) =="
python3 /work/port/validation/shader_screen.py "$ABLOG" 2>&1 | tee -a "$LOG"
[ "${PIPESTATUS[0]}" -eq 0 ] || FAIL=1

say
say "  Reminder: SwiftShader, not Mali/Xclipse. This bounds the GPU risk; it does not remove it."
selfhash_verify
say "DONE (FAIL=$FAIL)"
adb emu kill >/dev/null 2>&1
exit "$FAIL"
