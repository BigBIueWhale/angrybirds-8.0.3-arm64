#!/bin/bash
# verify_claims.sh — check the DOCUMENTED claims against the actual artifact.
#
# WHY THIS EXISTS
# ---------------
# On 2026-07-27 four documents were found to assert that the guest heap was "verified
# bit-identical between x86 and arm64". Measurement showed it is not (x86 605096 vs arm64
# 539536 after the 125 constructors). That claim had been repeated for days because nothing
# re-checked it — it was written once and then cited.
#
# This script mechanically re-checks the claims that CAN be checked from the artifact, so a
# false one cannot survive silently again. It does not check the claims that need a running
# emulator (those are the emu_*.sh scripts) or the phone (nothing here can).
#
# Usage — needs the ab-port image for apksigner/zipalign, and ab-analyze for the manifest:
#   bash port/validation/verify_claims.sh
# Exits non-zero if any checked claim is false.

set +e
cd "$(dirname "$0")/../.." || exit 1
APK=out/angrybirds-8.0.3-arm64.apk
ORIG=apks/com.rovio.angrybirds@8.0.3.apk
FAIL=0
ok(){ printf "  [ OK ] %s\n" "$1"; }
bad(){ printf "  [FAIL] %s\n" "$1"; FAIL=1; }
[ -f "$APK" ] || { echo "missing $APK — run port/build_apk.sh first"; exit 1; }

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT

echo "== CLAIM: engine/libjs/libadcolony/classes.dex are byte-for-byte Rovio's original 8.0.3 =="
if [ -f "$ORIG" ]; then
  (cd "$T" && mkdir -p o n && cd o && unzip -o -q "$OLDPWD/../$ORIG" 'lib/armeabi-v7a/*' 'classes.dex' 2>/dev/null)
  unzip -o -q "$ORIG" -d "$T/o" 'lib/armeabi-v7a/*' 'classes.dex' 2>/dev/null
  unzip -o -q "$APK"  -d "$T/n" 'lib/arm64-v8a/*'   'classes.dex' 2>/dev/null
  for pair in "libAngryBirdsClassic.so:libengine32.so" "libjs.so:libjs32.so" "libadcolony.so:libadcolony32.so"; do
    o=${pair%%:*}; m=${pair##*:}
    a=$(sha256sum "$T/o/lib/armeabi-v7a/$o" 2>/dev/null | cut -d' ' -f1)
    b=$(sha256sum "$T/n/lib/arm64-v8a/$m"  2>/dev/null | cut -d' ' -f1)
    [ -n "$a" ] && [ "$a" = "$b" ] && ok "$o == $m  ($a)" || bad "$o != $m"
  done
  a=$(sha256sum "$T/o/classes.dex" 2>/dev/null | cut -d' ' -f1)
  b=$(sha256sum "$T/n/classes.dex" 2>/dev/null | cut -d' ' -f1)
  [ -n "$a" ] && [ "$a" = "$b" ] && ok "classes.dex unmodified ($a)" || bad "classes.dex differs"
else
  echo "  [skip] $ORIG not present (it is xz-compressed in git; decompress to check)"
fi

echo "== CLAIM: the shim ELF is 16 KB-page aligned (loads on 16 KB-page devices) =="
unzip -o -q "$APK" -d "$T/n" 'lib/arm64-v8a/libAngryBirdsClassic.so'
AL=$(readelf -lW "$T/n/lib/arm64-v8a/libAngryBirdsClassic.so" 2>/dev/null | awk '/LOAD/{print $NF}' | sort -u | tr '\n' ' ')
[ "$(echo "$AL" | tr -d ' ')" = "0x4000" ] && ok "LOAD align = 0x4000 (16 KB)" || bad "LOAD align = $AL (want 0x4000)"

echo "== CLAIM: libm is in DT_NEEDED (without it modern bionic fails to resolve sin/cos) =="
readelf -d "$T/n/lib/arm64-v8a/libAngryBirdsClassic.so" 2>/dev/null | grep -q "libm.so" \
  && ok "libm.so present in DT_NEEDED" || bad "libm.so MISSING — would crash on launch"

echo "== CLAIM: no heap-diagnostic scaffolding ships in the release build =="
N=$(strings -a "$T/n/lib/arm64-v8a/libAngryBirdsClassic.so" | grep -cE 'GALLOC-CORRUPT|GALLOC-STATS|HEAP-PINPOINT|ABSHIM_ALLOC_TRACE')
[ "$N" = "0" ] && ok "no diagnostic strings in the shipped shim" || bad "$N diagnostic string(s) leaked into release"

echo "== CLAIM: signed v1+v2+v3, and 4-byte zipaligned =="
if command -v docker >/dev/null 2>&1 && docker image inspect ab-port >/dev/null 2>&1; then
  V=$(docker run --rm --network none -v "$PWD":/work ab-port apksigner verify -v "/work/$APK" 2>/dev/null | grep -cE "^Verified using v[123] scheme.*true")
  [ "$V" = "3" ] && ok "v1+v2+v3 all verify" || bad "only $V of 3 signature schemes verify"
  docker run --rm --network none -v "$PWD":/work ab-port zipalign -c 4 "/work/$APK" >/dev/null 2>&1 \
    && ok "zipalign -c 4 passes" || bad "not 4-byte aligned"
else
  echo "  [skip] ab-port image unavailable"
fi

echo "== CLAIM: minSdk 16 / targetSdk 26, and NO live network permission =="
if docker image inspect ab-analyze >/dev/null 2>&1; then
  docker run --rm --network none -v "$PWD":/work ab-analyze python3 -c "
from androguard.core.apk import APK
a=APK('/work/$APK')
net=[p for p in a.get_permissions() if 'INTERNET' in p or 'NETWORK' in p]
print('RESULT', a.get_min_sdk_version(), a.get_target_sdk_version(), ';'.join(net) if net else 'NONE')
" 2>/dev/null | grep '^RESULT' | while read -r _ mn tg net; do
    [ "$mn" = "16" ] && ok "minSdk 16" || bad "minSdk $mn (want 16)"
    [ "$tg" = "26" ] && ok "targetSdk 26" || bad "targetSdk $tg (want 26)"
    case "$net" in
      NONE) ok "no network permission at all" ;;
      *X*)  ok "only mangled network perms present ($net) — kernel denies sockets" ;;
      *)    bad "LIVE network permission present: $net" ;;
    esac
  done
else
  echo "  [skip] ab-analyze image unavailable"
fi

echo "== CLAIM: the SDK auto-init kill-switches are injected =="
for k in firebase_messaging_auto_init_enabled com.facebook.sdk.AutoLogAppEventsEnabled; do
  unzip -p "$APK" AndroidManifest.xml 2>/dev/null | strings | grep -q "$k" \
    && ok "$k present" || bad "$k MISSING"
done

echo
[ "$FAIL" = "0" ] && echo "ALL CHECKED CLAIMS HOLD" || echo "SOME CLAIMS ARE FALSE — fix the artifact or the docs"
exit "$FAIL"
