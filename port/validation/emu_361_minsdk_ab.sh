#!/bin/bash
# emu_361_minsdk_ab.sh — is the Android 36.1 launchability failure a minSdk/targetSdk policy?
#
# WHY THIS EXISTS
# ---------------
# R18 established, with a controlled four-way, that on API **36.1** this app's MAIN/LAUNCHER filter
# never registers — while its VIEW/BROWSABLE deep-link filter does, `com.android.settings` resolves,
# and the same APK is launchable on 36. It is not our rewrite: the APK carrying Rovio's ORIGINAL
# untouched manifest behaves identically. 36.1 is a QPR-level update the A56 may receive, so the cause
# is worth knowing.
#
# The leading hypothesis is a platform policy against `minSdk=16` / `targetSdk=26`. Testing it once
# hit a wall:
#
#     minSdk=24 targetSdk=34  ->  install REJECTED
#     "Targeting S+ (31 and above) requires an explicit android:exported when intent filters
#      are present"
#
# Rovio's 2015-era manifest has no `android:exported`, and adding one is an AXML *insertion* — it
# resizes the element and shifts every following offset, which is a different and much riskier class of
# edit than this project's same-length rewrites (`depermission.py`, `manifest_firebase_off.py`,
# `patch_minsdk.py` all change values, never lengths).
#
# SO THIS TESTS THE CHEAPER VARIANT THAT AVOIDS THE WALL ENTIRELY: **targetSdk=30**. 30 is below 31,
# so the `android:exported` requirement does not apply and the install should be accepted. If the app
# then becomes launchable, the hypothesis is confirmed and the threshold is somewhere in 27..30. If it
# stays unlaunchable, the hypothesis is wrong and minSdk/targetSdk is not the mechanism — which is
# equally worth knowing, because it would rule out the only explanation currently on record.
#
# THREE PREMISES ASSERTED, because each failure mode looks like a result:
#   1. the patch actually landed        — the tool prints the before/after integers, and this re-reads
#                                        them out of the rebuilt APK rather than trusting the tool
#   2. the install succeeded            — an unlaunchable app that never installed proves nothing
#   3. a control IS launchable there    — `com.android.settings` must resolve on the same device, or
#                                        the query itself is broken and "no" means nothing
#
#   docker run --rm --network none --device /dev/kvm --group-add "$(getent group kvm|cut -d: -f3)" \
#       -v "$PWD":/work ab-emu-361 bash /work/port/validation/emu_361_minsdk_ab.sh
set +e
source "$(dirname "$0")/lib_install.sh"
# Refuse to run while another evidence run is in flight: 50 scripts share reports/shots and
# two concurrent runs BLEND their logs into numbers that look normal and mean nothing.
source "$(dirname "$0")/lib_runlock.sh"
acquire_run_lock "$(basename "$0")" || exit 1
source "$(dirname "$0")/lib_metrics.sh"
( sleep 2400; adb emu kill 2>/dev/null; pkill -9 -f qemu 2>/dev/null ) &
PKG=com.rovio.angrybirds
OUT=/work/reports/shots; mkdir -p "$OUT"
LOG="$OUT/minsdk_ab_361.txt"; : >"$LOG"
say(){ echo "$@" | tee -a "$LOG"; }
FAIL=0
BASE=${ABSHIM_APK:-/work/out/angrybirds-8.0.3-x86shim-release.apk}
[ -f "$BASE" ] || { say "  [FAIL] missing $BASE"; say "DONE (FAIL=1)"; exit 1; }

# The variant is built by port/tools/make_sdk30_variant.sh in ab-port, NOT here: this image has no
# zip and no apksigner (measured — ab-emu-361 has only unzip/python3/adb). The first version of this
# script tried to repack in-image and died on `zip: command not found`.
V=${ABSHIM_SDK30:-/work/out/angrybirds-8.0.3-x86shim-sdk30.apk}
say "== the variant under test =="
[ -f "$V" ] || { say "  [FAIL] $V missing — build it first:"
                 say "         docker run --rm --network none -v \"\$PWD\":/work ab-port bash /work/port/tools/make_sdk30_variant.sh"
                 say "DONE (FAIL=1)"; exit 1; }
say "  $V"
say "  sha256 $(sha256sum "$V" | cut -c1-32)…"

say "== PREMISE 1: the APK really carries minSdk=24 / targetSdk=30 =="
# Re-read it here too. The builder asserted it, but this script must not take another script's word
# for the one property the whole experiment varies.
unzip -o -q "$V" AndroidManifest.xml -d /tmp/m361 2>/dev/null
python3 - /tmp/m361/AndroidManifest.xml <<'PYEOF' 2>&1 | tee -a "$LOG"
import struct, sys
d=open(sys.argv[1],'rb').read(); _t,hdr,_=struct.unpack_from('<HHI',d,0)
def pool(buf,o):
    _t,h,_s=struct.unpack_from('<HHI',buf,o); cnt,_sc,fl,ss,_=struct.unpack_from('<IIIII',buf,o+8)
    offs=struct.unpack_from('<%dI'%cnt,buf,o+h); utf8=bool(fl&(1<<8)); base=o+ss; out=[]
    for x in offs:
        p=base+x
        if utf8:
            n=buf[p]; p+=2 if n&0x80 else 1
            n=buf[p]
            if n&0x80: n=((n&0x7F)<<8)|buf[p+1]; p+=2
            else: p+=1
            out.append(buf[p:p+n].decode('utf-8','replace'))
        else:
            n=struct.unpack_from('<H',buf,p)[0]; p+=2
            out.append(buf[p:p+n*2].decode('utf-16-le','replace'))
    return out
off,strings,found=hdr,None,{}
while off<len(d):
    typ,hs,size=struct.unpack_from('<HHI',d,off)
    if size==0: break
    if typ==0x0001 and strings is None: strings=pool(d,off)
    elif typ==0x0102 and strings:
        ni=struct.unpack_from('<I',d,off+hs+4)[0]
        if ni<len(strings) and strings[ni]=='uses-sdk':
            st,sz,cn=struct.unpack_from('<HHH',d,off+hs+8)
            for k in range(cn):
                a=off+hs+st+k*sz; an=struct.unpack_from('<I',d,a+4)[0]
                if an<len(strings): found[strings[an]]=struct.unpack_from('<I',d,a+16)[0]
    off+=size
print(f"  minSdk={found.get('minSdkVersion')} targetSdk={found.get('targetSdkVersion')}")
print("  VERDICT:", "patch landed" if (found.get('minSdkVersion'),found.get('targetSdkVersion'))==(24,30) else "PATCH DID NOT LAND")
PYEOF
grep -q "patch landed" "$LOG" || { say "  [FAIL] the variant does not carry targetSdk=30"; say "DONE (FAIL=1)"; exit 1; }

say
say "== boot API 36.1 =="
emulator -avd "${ABSHIM_AVD:-ab361}" -no-window -no-audio -no-boot-anim -no-snapshot -accel on \
         -gpu swiftshader_indirect -partition-size 6144 -wipe-data >/tmp/e.log 2>&1 &
adb wait-for-device
for i in $(seq 1 240); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = 1 ] && break; sleep 2; done
sleep 15
REL=$(adb shell getprop ro.build.version.release 2>/dev/null|tr -d '\r')
SDK=$(adb shell getprop ro.build.version.sdk 2>/dev/null|tr -d '\r')
INC=$(adb shell getprop ro.build.version.incremental 2>/dev/null|tr -d '\r')
say "  android $REL (API $SDK) incremental=$INC"

say
say "== PREMISE 3: a control app IS launchable on this device =="
CTRL=$(adb shell cmd package resolve-activity --brief -c android.intent.category.LAUNCHER com.android.settings 2>/dev/null | tr -d '\r' | tail -1)
say "  com.android.settings LAUNCHER -> ${CTRL:-<none>}"
case "$CTRL" in
  *No*|"") say "  [FAIL] even Settings does not resolve — the query is broken, so any 'no' below is meaningless"; FAIL=1 ;;
  *) say "  [ OK ] the LAUNCHER query works on this device" ;;
esac

say
say "== PREMISE 2 + the actual experiment: install the targetSdk=30 variant =="
adb uninstall "$PKG" >/dev/null 2>&1
OUTI=$(adb install -r "$V" 2>&1)
# Print the WHOLE install output on failure. The first version kept `tail -2`, which on a rejected
# install is the bottom of a Java stack trace — so the actual reason ("Failure [-124: ... requires the
# resources.arsc ...]") was discarded by the very line meant to report it, and the run had to be redone.
# The reason a diagnostic exists is the case where something failed; that is the worst possible moment
# to truncate it.
printf '%s\n' "$OUTI" | grep -aE "Success|Failure|INSTALL_|requires|Exception" | head -6 | sed 's/^/    /' | tee -a "$LOG"
printf '%s\n' "$OUTI" > "$OUT/minsdk_ab_361_install.txt"
say "  (full installer output: reports/shots/minsdk_ab_361_install.txt)"
if ! adb shell pm list packages 2>/dev/null | grep -q rovio; then
    say "  [FAIL] targetSdk=30 was NOT accepted either — the install itself is the blocker, not launchability"
    say "DONE (FAIL=1)"; adb emu kill; exit 1
fi
say "  [ OK ] installed (so 30 avoids the S+ android:exported rejection that 34 hit)"

say
say "== the question: does MAIN/LAUNCHER register now? =="
R=$(adb shell cmd package resolve-activity --brief -c android.intent.category.LAUNCHER "$PKG" 2>/dev/null | tr -d '\r' | tail -1)
say "  resolve-activity -> ${R:-<none>}"
AMOUT=$(adb shell am start -n "$PKG/com.rovio.fusion.App" 2>&1 | tr -d '\r' | tail -2)
say "  am start -n .../com.rovio.fusion.App -> $(printf '%s' "$AMOUT" | tr '\n' ' ')"
# The image-wide launchable count is DELIBERATELY NOT PRINTED here. The first version used
#   cmd package query-activities -c android.intent.category.LAUNCHER | grep -c 'name='
# which reported 0 on an image where R18 counted 6 — the query form or its output shape differs on
# this build, so the number measured my grep rather than the device. A wrong number beside a real
# result is worse than no number: it invites the reader to doubt the result instead of the query.
# What carries the weight is PREMISE 3 above: a control package DOES resolve on this very device, so
# the per-package "No activity found" for ours is a fact about ours.

say
say "== VERDICT =="
case "$R" in
  *rovio*) say "  *** targetSdk=30 IS LAUNCHABLE on API 36.1 — the hypothesis is CONFIRMED and the"
           say "      threshold lies in 27..30. A future A56 QPR update would be survivable by bumping"
           say "      targetSdk (which also needs android:exported once you go >=31)." ;;
  *)       say "  targetSdk=30 is still NOT launchable on API 36.1, so minSdk/targetSdk is NOT the"
           say "  mechanism — this RULES OUT the only explanation currently on record in R18, and the"
           say "  cause remains unidentified. That is a real narrowing, not a failed run." ;;
esac
say "  (either outcome is a result; neither changes the shipped APK, which is launchable on API 36)"
say "DONE (FAIL=$FAIL)"
adb emu kill >/dev/null 2>&1
exit "$FAIL"
