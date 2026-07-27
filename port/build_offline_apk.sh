#!/bin/bash
# Reproducible: produce a FULLY-OFFLINE, no-phone-home Angry Birds 8.0.3 APK
# (32-bit original; runs on any 32-bit-capable Android device / emulator).
#   docker run --rm -v "$PWD":/work ab-port bash /work/port/build_offline_apk.sh
# Output: /work/out/angrybirds-8.0.3-offline.apk
set -e
IN=/work/apks/com.rovio.angrybirds@8.0.3.apk
# Prepare untracked inputs from the committed .xz (fresh-clone safe) — see prepare_inputs.sh
. /work/port/prepare_inputs.sh
REPO=/work prepare_inputs || exit 1
OUTDIR=/work/out; mkdir -p "$OUTDIR"
OUT="$OUTDIR/angrybirds-8.0.3-offline.apk"
WORK=/tmp/offwork

command -v apksigner >/dev/null 2>&1 || {
  echo "== installing apksigner + zipalign (apt, outbound only) =="
  apt-get update -qq >/dev/null 2>&1; apt-get install -y -qq apksigner zipalign zip unzip python3 >/dev/null 2>&1
}

echo "== 1/4 unpack =="
rm -rf "$WORK"; mkdir -p "$WORK"; (cd "$WORK" && unzip -q "$IN")

echo "== 2/4 strip network + tracking permissions from AndroidManifest =="
python3 /work/port/depermission.py "$WORK/AndroidManifest.xml" "$WORK/AndroidManifest.xml"

echo "== 3/4 strip signature, repack, align =="
rm -f "$WORK"/META-INF/*.RSA "$WORK"/META-INF/*.SF "$WORK"/META-INF/*.MF 2>/dev/null || true
rm -f /tmp/off.apk /tmp/offa.apk
# Deterministic zip (same as the other builds): fixed mtimes so rebuilds are byte-identical.
find "$WORK" -exec touch -d @315532800 {} +
(cd "$WORK" && TZ=UTC zip -q -r -X /tmp/off.apk .)
zipalign -f -p 4 /tmp/off.apk /tmp/offa.apk

echo "== 4/4 sign (debug key) =="
# Committed keystore, not /tmp (which --rm wipes, minting a fresh random key per build).
KS=/work/port/debug.ks
[ -f "$KS" ] || keytool -genkeypair -keystore "$KS" -storepass android -keypass android \
  -alias d -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=OfflineAngryBirds" >/dev/null 2>&1
apksigner sign --ks "$KS" --ks-pass pass:android --key-pass pass:android --out "$OUT" /tmp/offa.apk

echo "DONE -> $OUT  ($(du -h "$OUT" | cut -f1))"
echo "== verify: any network/tracking permission left? =="
if unzip -p "$OUT" AndroidManifest.xml | strings -el 2>/dev/null | grep -iE 'permission\.(INTERNET|ACCESS_NETWORK_STATE|ACCESS_WIFI_STATE|READ_PHONE_STATE|.*LOCATION)' \
 || unzip -p "$OUT" AndroidManifest.xml | strings 2>/dev/null | grep -iE 'permission\.(INTERNET|ACCESS_NETWORK_STATE|ACCESS_WIFI_STATE|READ_PHONE_STATE|.*LOCATION)'; then
  echo "  ^ still present (check encoding)"
else
  echo "  NONE — fully offline, no phone-home."
fi
