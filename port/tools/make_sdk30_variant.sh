#!/bin/bash
# make_sdk30_variant.sh — build a targetSdk=30 variant of the x86 proxy, for R18's Android 36.1 test.
#
# WHY THIS IS A SEPARATE SCRIPT FROM THE EMULATOR TEST
# ---------------------------------------------------
# The first attempt did the patch, the repack and the re-sign inside the emulator image and died on
# `zip: command not found`. Measured rather than assumed afterwards:
#
#     ab-port      zip unzip apksigner python3
#     ab-emu-361   unzip python3 adb
#
# The emulator images are Android SDK images; they have no build tooling. That is the same split the
# rest of this rig already uses — build in `ab-port`, run in an emulator image — so this follows it
# instead of fighting it.
#
# WHAT IT PRODUCES AND WHY targetSdk=30 SPECIFICALLY
# -------------------------------------------------
# R18 needs to know whether 36.1's launchability failure is a policy against `minSdk=16`/`targetSdk=26`.
# Testing that at `targetSdk=34` was rejected at install:
#     "Targeting S+ (31 and above) requires an explicit android:exported when intent filters are present"
# 30 is below 31, so that requirement does not apply and the install should be accepted — which makes it
# the cheapest variant that can actually answer the question. Rovio's 2015 manifest has no
# `android:exported`, and adding one is an AXML *insertion* that resizes the element and shifts every
# following offset — a far riskier class of edit than this project's same-length rewrites.
#
#   docker run --rm --network none -v "$PWD":/work ab-port bash /work/port/tools/make_sdk30_variant.sh
set -e
BASE=${ABSHIM_APK:-/work/out/angrybirds-8.0.3-x86shim-release.apk}
DST=${ABSHIM_OUT:-/work/out/angrybirds-8.0.3-x86shim-sdk30.apk}
[ -f "$BASE" ] || { echo "  [FAIL] missing $BASE" >&2; exit 1; }

W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
cp "$BASE" "$W/v.apk"
cd "$W"
unzip -o -q v.apk AndroidManifest.xml resources.arsc
python3 /work/port/tools/patch_minsdk.py AndroidManifest.xml patched.xml 24 30
mv patched.xml AndroidManifest.xml

# resources.arsc MUST be STORED, not deflated, once targetSdk >= 30. Measured, not guessed: the first
# attempt at targetSdk=30 was rejected by the package manager with
#   -124: "Targeting R+ (version 30 and above) requires the resources.arsc of installed APKs to be
#          stored uncompressed and aligned on a 4-byte boundary"
# and `unzip -v` showed ours as `Defl:N  643096 -> 161837  75%`. So the entry is removed and re-added
# with -0 (store), then the whole archive is zipaligned to 4 bytes BEFORE signing — apksigner
# preserves alignment, zipalign after signing would break the signature.
zip -q -d v.apk AndroidManifest.xml resources.arsc
zip -q -X -0 v.apk resources.arsc          # STORED, uncompressed
zip -q -X    v.apk AndroidManifest.xml     # deflate is fine for the manifest
zipalign -p -f 4 v.apk aligned.apk
mv aligned.apk v.apk
# Confirm the archive really has it stored now, rather than trusting the flags above.
unzip -v v.apk resources.arsc | grep -qE '\bStored\b' \
  || { echo "  [FAIL] resources.arsc is still not Stored after the repack" >&2; exit 1; }
echo "  [ OK ] resources.arsc is Stored + 4-byte aligned"
# Any manifest change invalidates v1/v2/v3, so re-sign with the SAME committed debug key the shipped
# builds use — that keeps the variant update-installable over them and keeps the signer digest a known
# published value rather than a fresh random one.
# EXACTLY the form build_apk.sh:158 uses — no --ks-key-alias, because the keystore holds a single
# entry and its alias is `d`, not the conventional `androiddebugkey`. Assuming the convention here
# produced: 'entry "androiddebugkey" does not contain a key'. Read the build script, do not guess.
apksigner sign --ks /work/port/debug.ks --ks-pass pass:android --key-pass pass:android v.apk
cp v.apk "$DST"

# Read the levels back OUT of the finished APK. "The tool printed a change" and "the APK contains the
# change" are different claims, and only the second one matters to the test that consumes this.
unzip -o -q "$DST" AndroidManifest.xml -d "$W/chk"
python3 - "$W/chk/AndroidManifest.xml" <<'PYEOF'
import struct, sys
d = open(sys.argv[1], 'rb').read()
_t, hdr, _ = struct.unpack_from('<HHI', d, 0)


def pool(buf, o):
    _t, h, _s = struct.unpack_from('<HHI', buf, o)
    cnt, _sc, fl, ss, _ = struct.unpack_from('<IIIII', buf, o + 8)
    offs = struct.unpack_from('<%dI' % cnt, buf, o + h)
    utf8 = bool(fl & (1 << 8)); base = o + ss; out = []
    for x in offs:
        p = base + x
        if utf8:
            n = buf[p]; p += 2 if n & 0x80 else 1
            n = buf[p]
            if n & 0x80:
                n = ((n & 0x7F) << 8) | buf[p + 1]; p += 2
            else:
                p += 1
            out.append(buf[p:p + n].decode('utf-8', 'replace'))
        else:
            n = struct.unpack_from('<H', buf, p)[0]; p += 2
            out.append(buf[p:p + n * 2].decode('utf-16-le', 'replace'))
    return out


off, strings, found = hdr, None, {}
while off < len(d):
    typ, hs, size = struct.unpack_from('<HHI', d, off)
    if size == 0:
        break
    if typ == 0x0001 and strings is None:
        strings = pool(d, off)
    elif typ == 0x0102 and strings:
        ni = struct.unpack_from('<I', d, off + hs + 4)[0]
        if ni < len(strings) and strings[ni] == 'uses-sdk':
            st, sz, cn = struct.unpack_from('<HHH', d, off + hs + 8)
            for i in range(cn):
                a = off + hs + st + i * sz
                an = struct.unpack_from('<I', d, a + 4)[0]
                if an < len(strings):
                    found[strings[an]] = struct.unpack_from('<I', d, a + 16)[0]
    off += size
mn, tg = found.get('minSdkVersion'), found.get('targetSdkVersion')
print(f"  minSdkVersion    = {mn}")
print(f"  targetSdkVersion = {tg}")
if (mn, tg) != (24, 30):
    print("  [FAIL] the finished APK does not carry minSdk=24 / targetSdk=30", file=sys.stderr)
    raise SystemExit(1)
print("  [ OK ] the finished APK really carries minSdk=24 / targetSdk=30")
PYEOF
echo "  -> $DST"
