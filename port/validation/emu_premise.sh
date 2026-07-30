#!/bin/bash
# emu_premise.sh — the problem and the fix, on ONE device, in ONE run.
#
# WHY THIS EXISTS
# ---------------
# Everything else here validates a piece of the port. Nothing demonstrated the thing the port is
# FOR. The two halves existed in different runs on different images, which is precisely the shape of
# evidence this project distrusts everywhere else: "it fails there" and "it works here" are not a
# comparison unless the there and the here are the same device.
#
# So: one 64-bit-only Android, two installs, back to back.
#
#   1. Rovio's untouched com.rovio.angrybirds@8.0.3.apk  -> must be REFUSED (armeabi-v7a + x86,
#      nothing 64-bit; the package manager has no ABI it can satisfy).  This is OPEN_FINDINGS R12
#      and it is the reason the project exists.
#   2. the SAME game, re-hosted by the shim                -> must INSTALL and RENDER on that same
#      device.
#
# The x86_64 build is used for step 2 rather than the arm64 deliverable because the host is x86_64:
# an arm64 AVD cannot boot here (Google removed cross-arch QEMU), which is documented throughout this
# repo. So this demonstrates the ARGUMENT — a 32-bit-only payload refused by a 64-bit-only Android,
# and the identical game accepted once re-hosted — on the architecture available. The A56 is the same
# situation with ARM in place of x86, and only the device can close that.
#
#   docker run --rm --network none --device /dev/kvm --group-add "$(getent group kvm|cut -d: -f3)" \
#       -v "$PWD":/work ab-emu-34 bash /work/port/validation/emu_premise.sh
set +e
source "$(dirname "$0")/lib_settle.sh"
# Refuse to run while another evidence run is in flight: 50 scripts share reports/shots and
# two concurrent runs BLEND their logs into numbers that look normal and mean nothing.
source "$(dirname "$0")/lib_runlock.sh"
acquire_run_lock "$(basename "$0")" || exit 1
source "$(dirname "$0")/lib_dialogs.sh"
source "$(dirname "$0")/lib_metrics.sh"
source "$(dirname "$0")/lib_install.sh"
source "$(dirname "$0")/lib_provenance.sh"
source "$(dirname "$0")/lib_selfhash.sh"

ORIG=/work/apks/com.rovio.angrybirds@8.0.3.apk
OURS=/work/out/angrybirds-8.0.3-x86shim-release.apk
OUT=/work/reports/shots; mkdir -p "$OUT"; LOG="$OUT/premise.txt"; : >"$LOG"
ABLOG="$OUT/premise_abshim.txt"; : >"$ABLOG"
say(){ echo "$@" | tee -a "$LOG"; }
selfhash_begin
FAIL=0

[ -f "$ORIG" ] || { [ -f "$ORIG.xz" ] && { say "  decompressing the committed input APK"; xz -dk "$ORIG.xz"; }; }
for f in "$ORIG" "$OURS"; do [ -f "$f" ] || { say "[FAIL] missing $f"; exit 1; }; done
say "  rovio original : $(sha256sum "$ORIG" | cut -c1-16)…  (armeabi-v7a + x86)"
say "  re-hosted      : $(sha256sum "$OURS" | cut -c1-16)…  (x86_64 shim + the SAME 32-bit payloads)"

say "== boot a 64-bit-only Android =="
emulator -avd "${ABSHIM_AVD:-abtest34}" -no-window -no-audio -no-boot-anim -no-snapshot -accel on \
    -gpu swiftshader_indirect >/tmp/emu.log 2>&1 &
adb wait-for-device
for i in $(seq 1 120); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = "1" ] && break; sleep 5; done
say "  android $(adb shell getprop ro.build.version.release 2>/dev/null|tr -d '\r') (API $(adb shell getprop ro.build.version.sdk 2>/dev/null|tr -d '\r'))"
say "  device ABIs: $(adb shell getprop ro.product.cpu.abilist 2>/dev/null|tr -d '\r')"
adb shell settings put global airplane_mode_on 1 >/dev/null 2>&1
adb uninstall com.rovio.angrybirds >/dev/null 2>&1

say
say "== 1. the ORIGINAL game on this device =="
adb push "$ORIG" /data/local/tmp/orig.apk >/dev/null 2>&1
O="$(adb shell pm install -r -d /data/local/tmp/orig.apk 2>&1 | tr -d '\r')"
say "  pm install says: $O"
case "$O" in
    *NO_MATCHING_ABIS*)
        say "  [ OK ] REFUSED for the documented reason: no ABI this device can run (R12)" ;;
    *Success*)
        say "  [FAIL] the original INSTALLED. This device is not 64-bit-only, so it cannot"
        say "         demonstrate the premise — check ro.product.cpu.abilist above."; FAIL=1 ;;
    *)  say "  [FAIL] refused, but for a DIFFERENT reason than R12 documents. The premise rests on"
        say "         NO_MATCHING_ABIS specifically; anything else means a different story."; FAIL=1 ;;
esac
adb uninstall com.rovio.angrybirds >/dev/null 2>&1

say
say "== 2. the SAME game, re-hosted, on the SAME device =="
install_apk "$OURS" 4 2>&1 | tee -a "$LOG"
if ! adb shell pm list packages 2>/dev/null | grep -q 'com.rovio.angrybirds'; then
    say "  [FAIL] the re-hosted build did not install"; FAIL=1
else
    say "  [ OK ] installed"
    adb logcat -c >/dev/null 2>&1; adb logcat -G 32M >/dev/null 2>&1
    adb logcat -s abshim > "$ABLOG" 2>/dev/null &
    adb shell monkey -p com.rovio.angrybirds -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
    say "  waiting for it to render (installing is not running)"
    for s in $(seq 1 90); do sleep 5
        fn=$(grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null|tail -1|grep -oE '[0-9]+')
        [ -n "$fn" ] && [ "$fn" -ge 301 ] && { say "  rendering at ~$((s*5))s frame[$fn]"; break; }
    done
    dismiss_system_dialogs
    settle_frames "$ABLOG" 300 300
    say "  h_fatal: $(h_fatal_report "$ABLOG")"
    FN=$(grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null|tail -1|grep -oE '[0-9]+')
    if [ -n "$FN" ] && [ "$FN" -ge 301 ]; then
        say "  [ OK ] the re-hosted game RUNS on the device that refused the original (frame[$FN])"
# Record which build produced this PROOF. Ten capture scripts were found writing images
# with no provenance row; these two are the ones whose output is cited as evidence.
record_build "$OURS" "premise" 2>&1 | tee -a "$LOG"
        adb shell screencap -p /sdcard/premise.png >/dev/null 2>&1
        adb pull /sdcard/premise.png "$OUT/premise_render.png" >/dev/null 2>&1
        say "  screenshot: $OUT/premise_render.png ($(stat -c%s "$OUT/premise_render.png" 2>/dev/null) bytes) — LOOK AT IT"
    else
        say "  [FAIL] installed but never rendered — installing is not the claim being made"; FAIL=1
    fi
fi

say
say "  What this shows: a 32-bit-only payload is refused by a 64-bit-only Android, and the SAME"
say "  game runs there once re-hosted by the shim. On x86_64, because an arm64 AVD cannot boot on"
say "  an x86_64 host. The A56 is this situation with ARM in place of x86."
# selfhash_verify RETURNS a verdict (0 unchanged / 1 edited mid-run / 2 cannot tell). It was
# called and discarded here, so a run whose script changed underneath it printed
# "*** DISCARD THESE RESULTS ***" and still exited 0 — the one failure that invalidates every
# other line above it.
selfhash_verify; [ $? -eq 0 ] || FAIL=$((FAIL+1))
say "DONE (FAIL=$FAIL)"
adb emu kill >/dev/null 2>&1
exit "$FAIL"
