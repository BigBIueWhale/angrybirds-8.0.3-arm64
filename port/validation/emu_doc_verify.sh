#!/bin/bash
# emu_doc_verify.sh — the VERIFICATION commands the docs hand the user: do they run, and do they
# give the documented answer?
#
# WHY THIS EXISTS
# ---------------
# `emu_install_commands.sh` already checks the commands a user types to INSTALL. Nothing checked the
# commands they type afterwards to CONVINCE THEMSELVES the thing works — the no-phone-home proof and
# the save-inspection tip in `port/ONDEVICE.md`. Those matter more than most internal claims: they
# are what a reader uses to decide whether to trust any of this, and a command that fails or prints
# the wrong thing reads as "the port is broken".
#
# This was written after finding, in one pass over the user's path, that the README pointed at a
# release that does not exist and gave a build command that cannot run outside the container. The
# verification commands had never been executed as written either.
#
#   docker run --rm --network none --device /dev/kvm --group-add "$(getent group kvm|cut -d: -f3)" \
#       -v "$PWD":/work ab-emu-36 bash /work/port/validation/emu_doc_verify.sh
set +e
source "$(dirname "$0")/lib_install.sh"
# Refuse to run while another evidence run is in flight: 50 scripts share reports/shots and
# two concurrent runs BLEND their logs into numbers that look normal and mean nothing.
source "$(dirname "$0")/lib_runlock.sh"
acquire_run_lock "$(basename "$0")" || exit 1
source "$(dirname "$0")/lib_selfhash.sh"

APK=/work/out/angrybirds-8.0.3-x86shim-release.apk   # deliverable manifest/signer, runnable ABI here
OUT=/work/reports/shots; mkdir -p "$OUT"; LOG="$OUT/docverify.txt"; : >"$LOG"
say(){ echo "$@" | tee -a "$LOG"; }
selfhash_begin
FAIL=0

[ -f "$APK" ] || { say "[FAIL] missing $APK"; exit 1; }
say "== boot =="
emulator -avd "${ABSHIM_AVD:-ab36}" -no-window -no-audio -no-boot-anim -no-snapshot -accel on \
    -gpu swiftshader_indirect >/tmp/emu.log 2>&1 &
adb wait-for-device
for i in $(seq 1 120); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = "1" ] && break; sleep 5; done
say "  android $(adb shell getprop ro.build.version.release 2>/dev/null|tr -d '\r') (API $(adb shell getprop ro.build.version.sdk 2>/dev/null|tr -d '\r'))"
# NETWORK STATE — same reasoning as emu_jni_exception_probe.sh (R32). One of the checks below is
# "no ESTABLISHED socket owned by the app", and on a device with the radios off that is true for
# reasons having nothing to do with this build. ABSHIM_NETWORK=1 leaves the stack up so the check is
# about the app. It is still not live internet — every container runs `--network none` — but with no
# INTERNET permission the kernel refuses socket creation outright, so the claim does not depend on
# anything being reachable.
if [ "${ABSHIM_NETWORK:-1}" = "1" ]; then
    adb shell settings put global airplane_mode_on 0 >/dev/null 2>&1
    adb shell svc wifi enable >/dev/null 2>&1
    adb shell svc data enable >/dev/null 2>&1
    say "  network: LEFT UP (default) — the socket check is about the app, not about airplane mode"
else
    adb shell settings put global airplane_mode_on 1 >/dev/null 2>&1
    say "  network: airplane mode ON (ABSHIM_NETWORK=0) — the weaker form; the network claim is then guaranteed by the environment"
fi
install_apk "$APK" 4 2>&1 | tee -a "$LOG"
adb shell pm list packages 2>/dev/null | grep -q rovio || { say "[FAIL] install"; adb emu kill; exit 1; }
adb shell monkey -p com.rovio.angrybirds -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
sleep 45   # let it get past boot so /proc/net has something to say about a running app

say
say "== ONDEVICE.md: the no-phone-home check =="
say "   adb shell dumpsys package com.rovio.angrybirds | grep -i 'permission\\.' | grep -i internet"
P=$(adb shell dumpsys package com.rovio.angrybirds 2>/dev/null | grep -i "permission\." | grep -i internet | tr -d '\r')
if [ -z "$P" ]; then
    say "   -> (no output), exactly as documented"
    # POSITIVE CONTROL: the check must be capable of finding a granted INTERNET permission at all,
    # otherwise "no output" would prove nothing. A system app that certainly has it stands in.
    C=$(adb shell dumpsys package com.android.shell 2>/dev/null | grep -i "permission\." | grep -i internet | tr -d '\r' | head -1)
    if [ -n "$C" ]; then
        say "   [ OK ] and the same command DOES find INTERNET on a package that has it:"
        say "          $(printf '%s' "$C" | sed 's/^ *//')"
    else
        say "   [FAIL] the control package shows no INTERNET either — the command cannot detect the"
        say "          permission at all, so its silence on our package means nothing."
        FAIL=1
    fi
else
    say "   [FAIL] the command printed something; the docs say it must print nothing:"
    say "          $P"; FAIL=1
fi

say
say "== ONDEVICE.md: the save-inspection tip =="
say "   adb shell run-as com.rovio.angrybirds ls files/"
R=$(adb shell run-as com.rovio.angrybirds ls files/ 2>&1 | tr -d '\r' | head -2)
say "   -> ${R:-<no output>}"
case "$R" in
    *"not debuggable"*|*"unknown package"*|*"Package "*not*)
        say "   NOTE: this FAILS by design — the shipped manifest is debuggable=\"false\" (Rovio's own"
        say "         value; the port only neutralises permissions). A debug SIGNATURE does not make an"
        say "         app debuggable, and run-as requires the latter. The docs must not present this as"
        say "         a way to read saves on the shipped build." ;;
    "") say "   [FAIL] no output at all — inconclusive, so nothing is established"; FAIL=1 ;;
    *)  say "   run-as SUCCEEDED — then the shipped build is debuggable, which would itself be a finding"
        say "   worth chasing, since the manifest says otherwise." ;;
esac

say
say "== ONDEVICE.md: no established connections while it runs =="
say "   adb shell cat /proc/net/tcp /proc/net/tcp6"
# `pm list packages -U` prints "package:<name> uid:<n>" directly. The first version scraped
# userId= out of dumpsys, which returned nothing on API 36 and left this check inconclusive —
# a reminder that a parser is a claim about output format and needs the same scepticism as any
# other claim. dumpsys stays as a fallback for older images.
UID_=$(adb shell pm list packages -U 2>/dev/null | tr -d '\r' | sed -n 's/^package:com\.rovio\.angrybirds uid:\([0-9]*\)$/\1/p' | head -1)
[ -n "$UID_" ] || UID_=$(adb shell dumpsys package com.rovio.angrybirds 2>/dev/null | sed -n 's/.*userId=\([0-9]*\).*/\1/p' | head -1 | tr -d '\r')
say "   app uid: ${UID_:-<unknown>}"
if [ -z "$UID_" ]; then
    say "   [FAIL] could not read the app's uid, so per-uid rows cannot be attributed"; FAIL=1
else
    ROWS=$(adb shell cat /proc/net/tcp /proc/net/tcp6 2>/dev/null | tr -d '\r' | awk -v u="$UID_" '$8==u')
    N=$(printf '%s' "$ROWS" | grep -c .)
    EST=$(printf '%s' "$ROWS" | awk '$4=="01"' | grep -c .)
    say "   rows owned by uid $UID_: $N   (ESTABLISHED, state 01: $EST)"
    if [ "$EST" -eq 0 ]; then say "   [ OK ] no ESTABLISHED socket owned by the app, as documented"
    else say "   [FAIL] the app owns $EST established connection(s)"; FAIL=1; fi
fi

say
say "== R5: what the OS actually grants this app =="
# R5 claimed "nothing at dangerous protection level". The shipped manifest declares
# WRITE_EXTERNAL_STORAGE, which IS dangerous — so the claim needed measuring, not restating.
# `dumpsys package` splits requested / install / runtime permissions; print all three.
adb shell dumpsys package com.rovio.angrybirds 2>/dev/null | tr -d '\r' \
  | awk '/requested permissions:/{r=1;next} /install permissions:|runtime permissions:|User 0:/{r=0} r&&NF{print "   requested: "$0}' \
  | head -20 | tee -a "$LOG"
adb shell dumpsys package com.rovio.angrybirds 2>/dev/null | tr -d '\r' \
  | awk '/install permissions:/{i=1;next} /runtime permissions:|User 0:|declared permissions:/{i=0} i&&NF{print "   granted(install): "$0}' \
  | head -12 | tee -a "$LOG"
RT=$(adb shell dumpsys package com.rovio.angrybirds 2>/dev/null | tr -d '\r' \
  | awk '/runtime permissions:/{i=1;next} /^\s*$|User [1-9]/{i=0} i&&NF{print}' | head -12)
say "   runtime permissions: ${RT:-<none>}"
STOR=$(printf '%s' "$RT" | grep -ci 'EXTERNAL_STORAGE')
if [ "${STOR:-0}" -eq 0 ]; then
    say "   [ OK ] no EXTERNAL_STORAGE runtime grant — the declared dangerous permission is not held"
else
    say "   [note] EXTERNAL_STORAGE appears in the runtime set: $(printf '%s' "$RT" | grep -i EXTERNAL_STORAGE)"
fi

say
say "== ONDEVICE.md: the ABI check it tells the user to run =="
A=$(adb shell getprop ro.product.cpu.abi 2>/dev/null | tr -d '\r')
say "   adb shell getprop ro.product.cpu.abi -> $A"
say "   (the command works; the VALUE here is the emulator's. On the A56 it reads arm64-v8a, which is"
say "    what makes the arm64 deliverable installable there and this x86 proxy not.)"

say
# selfhash_verify RETURNS a verdict (0 unchanged / 1 edited mid-run / 2 cannot tell). It was
# called and discarded here, so a run whose script changed underneath it printed
# "*** DISCARD THESE RESULTS ***" and still exited 0 — the one failure that invalidates every
# other line above it.
selfhash_verify; [ $? -eq 0 ] || FAIL=$((FAIL+1))
say "DONE (FAIL=$FAIL)"
adb emu kill >/dev/null 2>&1
exit "$FAIL"
