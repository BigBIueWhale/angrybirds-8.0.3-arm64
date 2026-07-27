#!/bin/bash
# reproduce.sh — one command: build the pinned toolchain image, convert the 8.0.3 APK to
# arm64, and verify the result installs. See port/REPRODUCE.md. Outbound-only; no ports.
set -e
cd "$(dirname "$0")/.."                                  # -> apk-binary-analysis

echo "== 0/3 prepare inputs: decompress the sovereign 8.0.3 APK + extract its 32-bit engine =="
# Delegated to prepare_inputs.sh rather than repeated here. This step used to be a hand-rolled copy
# of that function, which meant the authentic-input sha256 was hardcoded in TWO places: if the input
# were ever re-pinned, one could be updated and the other left behind, and the build would still
# pass the stale gate. The duplicated extraction also assumed `unzip`, while prepare_inputs falls
# back to python3's zipfile (needed by ab-hosttest, which has no unzip).
. port/prepare_inputs.sh
prepare_inputs || exit 1
echo "        input APK authenticity: OK (sha256-gated in prepare_inputs.sh)"

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
