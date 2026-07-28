#!/bin/bash
# emu_signature_clash.sh — prove the install-triage row for INSTALL_FAILED_UPDATE_INCOMPATIBLE is
# right, by actually causing it.
#
# WHY THIS EXISTS
# ---------------
# ONDEVICE.md's triage table tells a user what a refused install means and what to do about it. The
# likeliest cause on a real phone is that ROVIO's own Angry Birds is still installed: this APK keeps
# the original package name `com.rovio.angrybirds`, so a copy from Play — or restored by a backup —
# already owns that name under a signature no key of ours can update.
#
# That row was written from reasoning. Reasoning is how the same table previously claimed a cause
# that was real but secondary, so the row is now produced rather than argued: the ORIGINAL Rovio APK
# is genuinely signed by Rovio and genuinely ships x86 libs, which means the exact collision a user
# would hit can be reproduced here rather than described.
#
# It checks the whole row, not just the error:
#   1. the documented failure actually occurs, and the message matches what the table prints
#   2. the two documented "check before you uninstall anything" commands actually report the clash
#   3. the documented remedy actually works — uninstall, then our APK installs
#   4. the two signatures really are different (otherwise the test proves nothing about signing)
#
#   docker run --rm --network none --device /dev/kvm --group-add "$(getent group kvm|cut -d: -f3)" \
#       -v "$PWD":/work ab-emu bash /work/port/validation/emu_signature_clash.sh
#
# ab-emu (API 25), not ab-emu-34 — see the boot section: the API 34 x86_64 image cannot install
# Rovio's original at all, which is the very problem this project solves.
set +e
source "$(dirname "$0")/lib_install.sh"
source "$(dirname "$0")/lib_selfhash.sh"

ORIG=/work/apks/com.rovio.angrybirds@8.0.3.apk
OURS=/work/out/angrybirds-8.0.3-x86shim-release.apk
OUT=/work/reports/shots; mkdir -p "$OUT"; LOG="$OUT/sigclash.txt"; : >"$LOG"
say(){ echo "$@" | tee -a "$LOG"; }
selfhash_begin
FAIL=0

# The plain APK is gitignored (only the .xz is committed), so decompress on demand rather than
# assuming a previous session left it lying around.
if [ ! -f "$ORIG" ]; then
    if [ -f "$ORIG.xz" ]; then say "  decompressing the committed input APK"; xz -dk "$ORIG.xz"; fi
fi
for f in "$ORIG" "$OURS"; do
    [ -f "$f" ] || { say "[FAIL] missing $f"; exit 1; }
done
say "  rovio original : $(sha256sum "$ORIG" | cut -c1-16)…"
say "  our build      : $(sha256sum "$OURS" | cut -c1-16)…"

say "== boot =="
# API 25 (ab-emu), NOT API 34 — and the reason is itself a result. Rovio's original 8.0.3 ships
# armeabi-v7a + x86 and nothing 64-bit, and the API 34 x86_64 image REFUSES it outright:
#     Failure [INSTALL_FAILED_NO_MATCHING_ABIS: Failed to extract native libraries, res=-113]
# because modern 64-bit-only Android carries no 32-bit ABI. That is exactly the condition this whole
# project exists to work around, so the clash has to be staged on an image where the original can
# still install at all. Run with ABSHIM_AVD=abtest34 to reproduce the refusal instead.
emulator -avd "${ABSHIM_AVD:-abtest}" -no-window -no-audio -no-boot-anim -no-snapshot -accel on \
    -gpu swiftshader_indirect >/tmp/emu.log 2>&1 &
adb wait-for-device
for i in $(seq 1 120); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = "1" ] && break; sleep 5; done
say "  android $(adb shell getprop ro.build.version.release 2>/dev/null|tr -d '\r') (API $(adb shell getprop ro.build.version.sdk 2>/dev/null|tr -d '\r'))"
adb shell settings put global airplane_mode_on 1 >/dev/null 2>&1
adb uninstall com.rovio.angrybirds >/dev/null 2>&1   # start from a known-clean device

say
say "== 0. the two APKs must really be signed by different keys =="
# If they were not, everything below would pass for the wrong reason.
# apksigner is NOT present in the ab-emu images (it lives in ab-port), and the first version of this
# script assumed it was: both digests came back empty. The check failed loudly rather than passing on
# empty strings, which is the only reason that was obvious — but a premise check that cannot run is
# useless, so fall back to hashing the v1 certificate block, which needs only unzip + sha256sum.
sigof(){                              # $1 = apk -> a stable per-signer fingerprint
    local d
    d=$(apksigner verify --print-certs "$1" 2>/dev/null | grep -m1 'SHA-256 digest' | awk '{print $NF}')
    [ -n "$d" ] && { echo "$d"; return; }
    unzip -p "$1" 'META-INF/*.RSA' 2>/dev/null | sha256sum | cut -d' ' -f1
}
S1=$(sigof "$ORIG"); S2=$(sigof "$OURS")
say "  rovio signer : ${S1:0:32}…"
say "  our signer   : ${S2:0:32}…"
if [ -z "$S1" ] || [ -z "$S2" ]; then
    say "  [FAIL] could not read a signer from one of the APKs — the premise is unverified"; FAIL=1
elif [ "$S1" = "$S2" ]; then
    say "  [FAIL] identical signers — this test cannot demonstrate a signature clash"; FAIL=1
else
    say "  [ OK ] different signers, so a clash is genuinely what is being tested"
fi

say
say "== 1. install ROVIO's original (the situation a user is actually in) =="
install_apk "$ORIG" 4 2>&1 | tee -a "$LOG"
if ! adb shell pm list packages 2>/dev/null | grep -q 'com.rovio.angrybirds'; then
    say "  [FAIL] Rovio's APK did not install — cannot set up the clash. (It ships armeabi-v7a + x86;"
    say "         on an x86_64 image the x86 libs should satisfy it.)"
    adb emu kill; exit 1
fi
say "  [ OK ] Rovio's build is installed and owns com.rovio.angrybirds"

say
say "== 2. now install OURS on top — the documented failure =="
adb push "$OURS" /data/local/tmp/ours.apk >/dev/null 2>&1
OUTP="$(adb shell pm install -r -d /data/local/tmp/ours.apk 2>&1 | tr -d '\r')"
say "  pm install says: $OUTP"
case "$OUTP" in
    *INSTALL_FAILED_UPDATE_INCOMPATIBLE*|*"signatures do not match"*|*INSTALL_FAILED_SHARED_USER_INCOMPATIBLE*)
        say "  [ OK ] the documented error is what actually happens" ;;
    *Success*)
        say "  [FAIL] it INSTALLED over a differently-signed package. That should be impossible;"
        say "         if Android ever allows this the triage row is wrong."; FAIL=1 ;;
    *)  say "  [FAIL] a DIFFERENT refusal than the table documents — the row would send a reader"
        say "         down the wrong path. Update ONDEVICE.md with the message above."; FAIL=1 ;;
esac

say
say "== 3. the documented 'check before you uninstall anything' commands =="
P="$(adb shell pm list packages 2>/dev/null | grep rovio | tr -d '\r')"
say "  pm list packages | grep rovio -> ${P:-<nothing>}"
[ -n "$P" ] && say "  [ OK ] the first documented check finds the squatting package" \
            || { say "  [FAIL] the documented check reports nothing while the package is installed"; FAIL=1; }
SG="$(adb shell dumpsys package com.rovio.angrybirds 2>/dev/null | grep -A1 -i 'signatures' | head -2 | tr -d '\r')"
say "  dumpsys ... | grep -A1 signatures -> ${SG:-<nothing>}"
[ -n "$SG" ] && say "  [ OK ] the second documented check returns signature info" \
             || say "  [WARN] dumpsys printed no signature line on this API level; the first check is the load-bearing one"

say
say "== 4. the documented remedy =="
adb uninstall com.rovio.angrybirds >/dev/null 2>&1
install_apk "$OURS" 4 2>&1 | tee -a "$LOG"
if adb shell pm list packages 2>/dev/null | grep -q 'com.rovio.angrybirds'; then
    say "  [ OK ] after the documented uninstall, our APK installs — the remedy works as written"
else
    say "  [FAIL] our APK still does not install after the uninstall"; FAIL=1
fi

say
selfhash_verify
say "DONE (FAIL=$FAIL)"
adb emu kill >/dev/null 2>&1
exit "$FAIL"
