#!/bin/bash
# emu_reboot_persist.sh — do saves survive a DEVICE reboot, and does the app still start afterwards?
#
# WHY THIS EXISTS
# ---------------
# Persistence here is proven across an APP restart only: emu_save_test.sh force-stops the process and
# relaunches it, and emu_update_install.sh replaces the package. Neither restarts the DEVICE. A user
# reboots their phone, and a reboot is a materially different event:
#
#   - the app's process, its ART state and every mapping it held are gone, not just force-stopped;
#   - /data is remounted, and credential-encrypted storage is unlocked afresh;
#   - the app launches on a device where it is ALREADY installed — no first-install state, no
#     freshly-extracted native libs, no warm page cache.
#
# The shim is unusual enough for this to be worth asserting rather than assuming: it locates its
# ARM32 payload by taking dladdr() of its own function and open()ing libengine32.so beside it. That
# path is created by the installer's native-lib extraction; nothing here had checked it is still
# valid, and still points at an intact file, after the device has been through a full boot.
#
# WHAT IT ASSERTS, in order, each refusing to score an unanswerable question as a pass:
#   1. saves exist before the reboot          — else there is nothing whose survival could be tested
#   2. the device actually rebooted           — boot id changes; a no-op reboot must not read as a pass
#   3. the save bytes are identical after     — the actual persistence claim
#   4. the app starts and renders again       — frames advance, h_fatal stays 0
#
#   docker run --rm --network none --device /dev/kvm --group-add "$(getent group kvm|cut -d: -f3)" \
#       -v "$PWD":/work -e ABSHIM_AVD=ab36 ab-emu-36 bash /work/port/validation/emu_reboot_persist.sh
set +e
source "$(dirname "$0")/lib_settle.sh"
source "$(dirname "$0")/lib_dialogs.sh"
source "$(dirname "$0")/lib_metrics.sh"
source "$(dirname "$0")/lib_install.sh"
source "$(dirname "$0")/lib_camera.sh"
source "$(dirname "$0")/lib_selfhash.sh"
source "$(dirname "$0")/lib_provenance.sh"

APK=/work/out/angrybirds-8.0.3-x86shim-release.apk    # the SHIPPING configuration
PKG=com.rovio.angrybirds
DIR=/data/data/$PKG
OUT=/work/reports/shots; mkdir -p "$OUT"; LOG="$OUT/reboot_persist.txt"; : >"$LOG"
ABLOG="$OUT/reboot_persist_abshim.txt"; : >"$ABLOG"
say(){ echo "$@" | tee -a "$LOG"; }
selfhash_begin
FAIL=0
[ -f "$APK" ] || { say "[FAIL] missing $APK"; exit 1; }

say "== boot =="
emulator -avd "${ABSHIM_AVD:-ab36}" -no-window -no-audio -no-boot-anim -no-snapshot -accel on \
    -feature -ModemSimulator -gpu swiftshader_indirect >/tmp/emu.log 2>&1 &
adb wait-for-device
for i in $(seq 1 150); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = "1" ] && break; sleep 5; done
say "  android $(adb shell getprop ro.build.version.release 2>/dev/null|tr -d '\r') (API $(adb shell getprop ro.build.version.sdk 2>/dev/null|tr -d '\r'))"
adb shell settings put global airplane_mode_on 1 >/dev/null 2>&1
adb root >/dev/null 2>&1; sleep 4; adb wait-for-device      # reading /data/data needs root

install_apk "$APK" 4 2>&1 | tee -a "$LOG"
adb shell pm list packages 2>/dev/null | grep -q rovio || { say "[FAIL] install"; adb emu kill; exit 1; }
# Tie this run's captures to the build that produced them. Written without this at first: three new
# scripts each saved a screenshot with no record of which APK made it, invisible to the gate's
# "captures were taken on builds that still exist" check — the exact gap provenance.tsv exists to
# close, reintroduced by the scripts that were meant to strengthen the evidence.
record_build "$APK" "reboot_persist" 2>&1 | tee -a "$LOG"

say
say "== 1. play until it writes saves =="
adb logcat -c >/dev/null 2>&1; adb logcat -T 1 -s abshim > "$ABLOG" 2>/dev/null &
adb shell monkey -p "$PKG" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
for s in $(seq 1 130); do sleep 5
    fn=$(grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null|tail -1|grep -oE '[0-9]+')
    [ -n "$fn" ] && [ "$fn" -ge 601 ] && { say "  rendering at ~$((s*5))s frame[$fn]"; break; }
done
dismiss_system_dialogs
adb shell input tap 390 266; sleep 4; adb shell input tap 390 266; sleep 8
pan_to_slingshot 3 1
for k in 1 2 3; do adb shell input swipe 207 118 110 150 700 >/dev/null 2>&1; sleep 6; done
settle_frames "$ABLOG" 200 200
adb shell am force-stop "$PKG"; sleep 5        # flush to disk

savehash(){ adb shell "md5sum $DIR/files/settings.lua $DIR/files/highscores.lua 2>/dev/null" \
            | tr -d '\r' | awk '{print $1}' | tr '\n' ' '; }
H1=$(savehash)
say "  saves before reboot: ${H1:-<none>}"
if [ -z "$H1" ]; then
    say "  [FAIL] no save files were written, so there is nothing whose survival a reboot could test."
    adb emu kill; exit 1
fi

say
say "== 2. reboot the DEVICE (not just the app) =="
# The boot id changes on every kernel boot. Without comparing it, an `adb reboot` that silently did
# nothing would leave every check below passing on a device that never restarted.
BOOT1=$(adb shell cat /proc/sys/kernel/random/boot_id 2>/dev/null | tr -d '\r')
say "  boot_id before: ${BOOT1:-<unreadable>}"
adb reboot >/dev/null 2>&1
sleep 10
adb wait-for-device
for i in $(seq 1 150); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = "1" ] && break; sleep 5; done
adb root >/dev/null 2>&1; sleep 4; adb wait-for-device
BOOT2=$(adb shell cat /proc/sys/kernel/random/boot_id 2>/dev/null | tr -d '\r')
say "  boot_id after : ${BOOT2:-<unreadable>}"
if [ -z "$BOOT1" ] || [ -z "$BOOT2" ]; then
    say "  [FAIL] could not read the boot id — cannot prove the device restarted, so nothing below counts"
    adb emu kill; exit 1
elif [ "$BOOT1" = "$BOOT2" ]; then
    say "  [FAIL] the boot id did not change — the device did NOT reboot; the rest would be meaningless"
    adb emu kill; exit 1
fi
say "  [ OK ] the device really rebooted"

say
say "== 3. are the saves still there, byte for byte =="
H2=$(savehash)
say "  saves after reboot : ${H2:-<none>}"
if [ -n "$H2" ] && [ "$H2" = "$H1" ]; then
    say "  [ OK ] save files are byte-identical across a device reboot"
else
    say "  [FAIL] saves changed or vanished across the reboot: '$H1' -> '$H2'"; FAIL=1
fi

say
say "== 4. does it still start and render on the rebooted device =="
: >"$ABLOG"
adb logcat -c >/dev/null 2>&1; adb logcat -T 1 -s abshim > "$ABLOG" 2>/dev/null &
adb shell monkey -p "$PKG" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
for i in $(seq 1 12); do P=$(adb shell pidof "$PKG" 2>/dev/null | tr -d '\r'); [ -n "$P" ] && break; sleep 5; done
if [ -z "$P" ]; then
    say "  [FAIL] the app did not start within 60s after the reboot"
    adb logcat -d '*:E' 2>/dev/null | grep -aiE "rovio|dlopen|UnsatisfiedLink" | tail -6 | sed 's/^/    /' | tee -a "$LOG"
    FAIL=1
else
    say "  started as pid $P"
    for s in $(seq 1 130); do sleep 5
        fn=$(grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null|tail -1|grep -oE '[0-9]+')
        [ -n "$fn" ] && [ "$fn" -ge 601 ] && break
    done
    settle_frames "$ABLOG" 150 150
    adb exec-out screencap -p > "$OUT/reboot_persist_screen.png" 2>/dev/null
    FR=$(grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null|tail -1|grep -oE '[0-9]+')
    say "  frames after reboot: ${FR:-<none>}   h_fatal: $(h_fatal_report "$ABLOG")"
    [ -n "$FR" ] && [ "$FR" -ge 601 ] && say "  [ OK ] the engine renders again on a rebooted device" \
        || { say "  [FAIL] never reached frame[601] after the reboot"; FAIL=1; }
    HF=$(grep -ac 'h_fatal' "$ABLOG"); [ "${HF:-0}" -eq 0 ] || { say "  [FAIL] h_fatal after the reboot"; FAIL=1; }
fi

say
selfhash_verify
say "DONE (FAIL=$FAIL)"
adb emu kill >/dev/null 2>&1
exit "$FAIL"
