#!/bin/bash
# emu_intent_probe.sh — the one kind of "internet access" the four de-phone-home layers cannot block.
#
# WHY THIS EXISTS
# ---------------
# The brief was "remove all phone-homes and annoying internet access of any type". The four layers
# deliver that for anything the app does ITSELF: no INTERNET permission (the kernel refuses every
# socket), the shim imports no socket symbols, billing/accounts/push are neutralised in the manifest,
# and the auto-collecting SDKs are switched off at their source. Every existing test measures that
# same thing — `emu_jni_exception_probe.sh` asserts the app's own pid performs zero name resolution
# and zero socket work.
#
# None of that touches a different route out: the app asking ANDROID to open something.
#
#     startActivity(new Intent(ACTION_VIEW, Uri.parse("market://details?id=...")))
#
# That is not a socket in this process — it is an IPC to the system, which then hands the URL to
# Play or a browser. No permission is required and no layer here intercepts it. And the capability is
# demonstrably present in the shipped APK, because `classes.dex` is byte-for-byte Rovio's: the dex
# contains `market://details?id=`, `android.intent.action.VIEW`, `http://www.rovio.com/eula`, and the
# game loads `AppRater.lua` 24 times and `TEXTS_APPRATER.dat` 72 times in a single playthrough. An
# app-rater prompt that sends the user to the Play Store is exactly the "annoying" half of the brief.
#
# The Lua is encrypted (magic e393b813, no readable strings), so whether that path FIRES cannot be
# read statically. It can be watched for, which is what this does.
#
# WHAT IT REFUSES TO DO
# ---------------------
# Report "no intents were launched" from a log that never contained any. The game's own launch is an
# activity start, so at least one `START` attributable to this app MUST appear; if none does, the
# probe measured nothing and says so rather than printing a clean result. That is the positive
# control, built in — the recurring rule that a null result needs proof the detector fires.
#
#   docker run --rm --network none --device /dev/kvm --group-add "$(getent group kvm|cut -d: -f3)" \
#       -v "$PWD":/work ab-emu-34 bash /work/port/validation/emu_intent_probe.sh
set +e
source "$(dirname "$0")/lib_settle.sh"
source "$(dirname "$0")/lib_dialogs.sh"
source "$(dirname "$0")/lib_metrics.sh"
source "$(dirname "$0")/lib_install.sh"
source "$(dirname "$0")/lib_selfhash.sh"

APK=/work/out/angrybirds-8.0.3-x86shim-release.apk
OUT=/work/reports/shots; mkdir -p "$OUT"; LOG="$OUT/intents.txt"; : >"$LOG"
ABLOG="$OUT/intents_abshim.txt"; : >"$ABLOG"
FULL="$OUT/intents_full.txt"; : >"$FULL"
say(){ echo "$@" | tee -a "$LOG"; }
selfhash_begin
FAIL=0

[ -f "$APK" ] || { say "[FAIL] missing $APK"; exit 1; }
say "  measuring: $(basename "$APK")  sha256 $(sha256sum "$APK" | cut -c1-16)…"

say "== boot =="
emulator -avd "${ABSHIM_AVD:-abtest34}" -no-window -no-audio -no-boot-anim -no-snapshot -accel on \
    -gpu swiftshader_indirect >/tmp/emu.log 2>&1 &
adb wait-for-device
for i in $(seq 1 120); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = "1" ] && break; sleep 5; done
say "  android $(adb shell getprop ro.build.version.release 2>/dev/null|tr -d '\r') (API $(adb shell getprop ro.build.version.sdk 2>/dev/null|tr -d '\r'))"
adb shell settings put global airplane_mode_on 1 >/dev/null 2>&1

install_apk "$APK" 4 2>&1 | tee -a "$LOG"
adb shell pm list packages 2>/dev/null | grep -q rovio || { say "[FAIL] install"; adb emu kill; exit 1; }

# UNFILTERED logcat — ActivityManager/ActivityTaskManager log activity starts, and they are a system
# tag, not ours. A `-s abshim` capture (what every other script uses) cannot see them at all.
adb logcat -c >/dev/null 2>&1; adb logcat -G 64M >/dev/null 2>&1
adb logcat > "$FULL" 2>/dev/null &
adb logcat -s abshim > "$ABLOG" 2>/dev/null &
adb shell monkey -p com.rovio.angrybirds -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1

say "== play — the rater is the likeliest trigger, so reach a level end =="
for s in $(seq 1 110); do sleep 5
    fn=$(grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null|tail -1|grep -oE '[0-9]+')
    [ -n "$fn" ] && [ "$fn" -ge 601 ] && { say "  rendering at ~$((s*5))s frame[$fn]"; break; }
done
dismiss_system_dialogs
adb shell input tap 390 266; sleep 4; adb shell input tap 390 266; sleep 12
for i in 1 2 3 4 5; do adb shell input swipe 207 118 110 150 700; sleep 8; done
adb shell input tap 390 266; sleep 6          # advance past the level-end card
settle_frames "$ABLOG" 300 300

say
say "== RESULTS =="
say "  h_fatal: $(h_fatal_report "$ABLOG")"

# Every activity start on the device, then the ones attributable to this app.
ALLSTART=$(grep -aE 'ActivityManager|ActivityTaskManager' "$FULL" | grep -acE 'START u[0-9]+')
say "  activity starts seen on the device: $ALLSTART"

# POSITIVE CONTROL. The game's own launch is an activity start naming our package, so at least one
# line MUST mention it. Zero means the probe is blind and any "clean" verdict below is worthless.
OURS=$(grep -aE 'START u[0-9]+' "$FULL" | grep -c 'com.rovio.angrybirds')
say "  of those, naming com.rovio.angrybirds: $OURS"
if [ "$OURS" -eq 0 ]; then
    say "  [FAIL] the probe saw NO activity start for this app at all — not even its own launch."
    say "         The detector is blind, so 'no outbound intents' below would mean nothing."
    adb emu kill; exit 1
fi
say "  [ OK ] positive control: the detector does see this app's activity starts"

say
say "== outbound intents (the thing no de-phone-home layer can block) =="
# A start whose DATA is a URL/market link, or whose action is VIEW, launched with our package as the
# caller — that is the app sending the user out to Play or a browser.
OUTB=$(grep -aE 'START u[0-9]+' "$FULL" | grep -E 'act=android.intent.action.VIEW|dat=market://|dat=http' | grep -v 'cmp=com.rovio.angrybirds/' )
NOUT=$(printf '%s' "$OUTB" | grep -c . )
if [ "$NOUT" -eq 0 ]; then
    say "  [ OK ] ZERO outbound VIEW/market/http intents during a full playthrough incl. a level end"
    say "         (capability present in the untouched dex; not exercised on this path)"
else
    say "  [FAIL] $NOUT outbound intent(s) — the app tried to send the user to a browser/Play:"
    printf '%s\n' "$OUTB" | head -8 | sed 's/^/      /' | tee -a "$LOG"
    FAIL=1
fi

say
say "  Also recorded for the file: every start naming this app"
grep -aE 'START u[0-9]+' "$FULL" | grep 'com.rovio.angrybirds' | head -6 \
    | sed 's/.*START/    START/' | cut -c1-150 | tee -a "$LOG"

say
selfhash_verify
say "DONE (FAIL=$FAIL)"
adb emu kill >/dev/null 2>&1
exit "$FAIL"
