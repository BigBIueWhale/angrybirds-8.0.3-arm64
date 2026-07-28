# prepare_inputs.sh — make the build inputs present, from what git actually tracks.
#
# WHY THIS EXISTS
# ---------------
# The repo commits ONLY the xz-compressed 8.0.3 APK (the raw file is ~100 MB). It does not commit
# the decompressed APK, nor the 32-bit engine extracted from it. Several scripts are documented as
# direct entry points — port/build_apk.sh (REPRODUCE.md x2, ONDEVICE.md for the audio variant),
# the three x86 proxy builds, and port/shim/test/run_tests.sh — and every one of them read those
# untracked files directly. On this machine they exist because reproduce.sh has been run; from a
# fresh clone they do not, and the scripts failed with errors that pointed at the wrong thing
# ("unzip: cannot find or open .../8.0.3.apk", or a raw readelf traceback out of coverage_check).
#
# Factored into ONE implementation rather than pasted into five scripts: five copies of the same
# preparation would drift, which is the same mistake the fixed-duration settles made before
# lib_settle.sh consolidated them.
#
# Usage (from a script whose CWD is the repo root, or with REPO set):
#   . "$(dirname "$0")/prepare_inputs.sh"; prepare_inputs
#
# Sourcing does nothing; call prepare_inputs explicitly so callers control ordering.

prepare_inputs() {
    local repo="${REPO:-$PWD}"
    local apk="$repo/apks/com.rovio.angrybirds@8.0.3.apk"
    local eng="$repo/work803/libv7/libAngryBirdsClassic.so"
    local sha="0580c3d3f79b21b344940bea65b8fadc22e8e5599c89dfe9b5e8a85004846b9a"

    if [ ! -f "$apk" ]; then
        if [ ! -f "$apk.xz" ]; then
            echo "FATAL: no input APK — neither $apk nor $apk.xz exists" >&2; return 1
        fi
        command -v xz >/dev/null 2>&1 || { echo "FATAL: xz needed to decompress $apk.xz" >&2; return 1; }
        echo "   input APK not decompressed yet -> xz -dk $(basename "$apk").xz"
        xz -dk "$apk.xz" || return 1
    fi

    # Authenticity gate: reject a wrong or tampered input before any build work happens.
    echo "$sha  $apk" | sha256sum -c - >/dev/null 2>&1 \
        || { echo "FATAL: input APK sha256 mismatch — not the authentic 8.0.3" >&2; return 1; }

    if [ ! -f "$eng" ]; then
        echo "   engine not extracted yet -> lib/armeabi-v7a/libAngryBirdsClassic.so"
        mkdir -p "$(dirname "$eng")"
        if command -v unzip >/dev/null 2>&1; then
            unzip -o -j -q "$apk" lib/armeabi-v7a/libAngryBirdsClassic.so -d "$(dirname "$eng")" || return 1
        elif command -v python3 >/dev/null 2>&1; then
            # ab-hosttest has python3 but NOT unzip, so keep this fallback.
            python3 - "$apk" "$eng" <<'PYEOF' || return 1
import sys, zipfile, shutil
apk, dest = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(apk) as z, open(dest, "wb") as out:
    with z.open("lib/armeabi-v7a/libAngryBirdsClassic.so") as src:
        shutil.copyfileobj(src, out)
PYEOF
        else
            echo "FATAL: need unzip or python3 to extract the engine" >&2; return 1
        fi
        [ -s "$eng" ] || { echo "FATAL: engine extraction produced nothing" >&2; return 1; }
    fi
    return 0
}

# require_build_tools — assert the image really carries the packaging toolchain.
#
# WHY IT LIVES HERE. build_apk.sh had this check inline and the five x86 proxies did not, even
# though all six sign, align and zip with the same tools. That is the same drift that left the
# proxies with a weak script_paths guard: a safety check added to one copy of six. Every build
# script already sources this file, so putting the check here makes it one implementation reachable
# from all of them without a new file to remember.
#
# The failure it prevents is a stale or wrong ab-port image producing a confusing mid-build error —
# or, worse, a silent fallback to reaching out to apt in a build that is supposed to be offline.
require_build_tools() {
    local missing="" t
    for t in apksigner zipalign zip unzip keytool; do
        command -v "$t" >/dev/null 2>&1 || missing="$missing $t"
    done
    [ -z "$missing" ] && return 0
    echo "FATAL: image is missing build tools:$missing" >&2
    echo "       rebuild it: docker build -t ab-port -f port/docker/Dockerfile.ab-port ." >&2
    return 1
}
