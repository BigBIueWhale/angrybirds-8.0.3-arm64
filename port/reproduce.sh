#!/bin/bash
# reproduce.sh — one command: build the pinned toolchain image, convert the 8.0.3 APK to
# arm64, and verify the result installs. See port/REPRODUCE.md. Outbound-only; no ports.
set -e
cd "$(dirname "$0")/.."                                  # -> apk-binary-analysis

echo "== 0/3 prepare inputs: decompress the sovereign 8.0.3 APK + extract its 32-bit engine =="
APK="apks/com.rovio.angrybirds@8.0.3.apk"
if [ ! -f "$APK" ]; then
  echo "        xz -d the committed input (sha256 verified below)"; xz -dk "$APK.xz"
fi
# sanity: the decompressed input must be the authentic 8.0.3 (sha256 0580c3d3...)
echo "0580c3d3f79b21b344940bea65b8fadc22e8e5599c89dfe9b5e8a85004846b9a  $APK" | sha256sum -c - >/dev/null \
  && echo "        input APK authenticity: OK (0580c3d3...)" || { echo "        FATAL: input APK sha mismatch"; exit 1; }
# build_apk.sh reads the 32-bit engine from work803/libv7/ (js/adcolony it unzips from the APK itself)
mkdir -p work803/libv7
[ -f work803/libv7/libAngryBirdsClassic.so ] || \
  unzip -o -j -q "$APK" lib/armeabi-v7a/libAngryBirdsClassic.so -d work803/libv7/

echo "== 1/3 build ab-port image (NDK r26d + Unicorn 2.1.4; ~15-20 min the first time) =="
echo "        (this is the ONLY step that touches the network — outbound fetches, no ports)"
docker build -t ab-port -f port/docker/Dockerfile.ab-port .

echo "== 2/3 convert: 32-bit 8.0.3 APK -> arm64-v8a (de-phone-homed) — OFFLINE (--network none) =="
docker run --rm --network none -v "$PWD":/work ab-port bash /work/port/build_apk.sh

echo "== 3/3 verify the artifact against every DOCUMENTED claim — OFFLINE =="
# This used to check only the signature and 4-byte alignment. Everything else the docs assert -
# that Rovio's engine/libjs/libadcolony/classes.dex are byte-for-byte unmodified, that the shim is
# 16 KB-page aligned, that libm is linked, that no network permission or socket capability exists,
# that the SDK auto-init kill-switches are injected, that no diagnostic scaffolding shipped - was
# taken on trust. A claim nothing re-checks is how a false one ("the guest heap is bit-identical
# across host architectures") survived for days. verify_claims.sh checks all of them and exits
# non-zero if any is false, so `reproduce.sh` fails loudly rather than producing an APK whose
# description is wrong.
bash port/validation/verify_claims.sh

echo
echo "DONE -> out/angrybirds-8.0.3-arm64.apk"
echo "Install: adb install -r out/angrybirds-8.0.3-arm64.apk"
echo "Debug:   adb logcat -s abshim"
