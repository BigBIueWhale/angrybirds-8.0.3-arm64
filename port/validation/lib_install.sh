# lib_install.sh — install the APK only once the package service can actually accept it.
#
# WHY THIS EXISTS
# ---------------
# `sys.boot_completed=1` does NOT mean the package manager is ready. Installing a ~98 MB APK
# immediately after that property flips can fail with:
#
#     cmd: Failure calling service package: Broken pipe (32)
#
# which is the system server dropping the call, not a defect in the APK. Observed 2026-07-28 on
# emu_perf_split.sh; the same APK installed fine on the retry. Every playthrough script has the
# same inline `pm install ... | grep -q Success` and so has the same latent flake — it simply had
# not been unlucky yet.
#
# The failure mode this guards against is the dangerous direction: a transient install failure
# reads exactly like "the build is broken", and would send a reader looking for a bug that is not
# there. (The inverse of the project's recurring lesson — here, evidence of absence being mistaken
# for a real absence.)
#
# So: wait for the package service to answer, then retry a few times, and distinguish the two
# outcomes in the output rather than collapsing both to "install FAIL".
#
# USAGE
#   source "$(dirname "$0")/lib_install.sh"
#   install_apk /work/out/foo.apk || { say "[FAIL] install"; exit 1; }
#
# Returns 0 on success. On failure prints why, and whether it ever became ready.

# Wait until `service check package` reports the package service is published.
wait_for_pm() {                       # $1 = max seconds (default 120)
    local cap="${1:-120}" i
    for i in $(seq 1 "$cap"); do
        case "$(adb shell service check package 2>/dev/null | tr -d '\r')" in
            *"not found"*) sleep 1 ;;
            "") sleep 1 ;;
            *) return 0 ;;            # "Service package: [android.content.pm.IPackageManager]"
        esac
    done
    return 1
}

# Push + install with retries. Echoes progress; caller decides what to do on failure.
install_apk() {                       # $1 = host path to apk   $2 = tries (default 3)
    local apk="$1" tries="${2:-3}" t out
    if ! wait_for_pm 120; then
        echo "  install: the package service never became available — the device is not ready,"
        echo "           which is NOT the same as the APK being bad. Nothing was proven here."
        return 1
    fi
    adb push "$apk" /data/local/tmp/ab.apk >/dev/null 2>&1
    for t in $(seq 1 "$tries"); do
        out="$(adb shell pm install -r -d /data/local/tmp/ab.apk 2>&1 | tr -d '\r')"
        case "$out" in
            *Success*) [ "$t" -gt 1 ] && echo "  install: ok (attempt $t)" || echo "  install: ok"
                       return 0 ;;
            *"Broken pipe"*|*"DeadObjectException"*|*"Failure calling service"*)
                       echo "  install: attempt $t hit a transient service error, retrying: $out"
                       sleep 10 ;;
            *)         echo "  install: REJECTED by the package manager: $out"
                       echo "           (a real rejection — signature, ABI or manifest — not a flake)"
                       return 1 ;;
        esac
    done
    echo "  install: still failing after $tries attempts on transient errors"
    return 1
}
