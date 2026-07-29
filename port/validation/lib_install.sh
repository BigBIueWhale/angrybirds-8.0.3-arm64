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
#
# Matches the PUBLISHED form positively rather than treating "anything that is not 'not found'" as
# ready. The permissive version returned success for any non-empty line, so a transient adb message
# ("error: device offline", "error: closed") read as "the package manager is up" and sent the
# install straight into a service that was not there. Requiring the interface name means only the
# real answer counts — the same rule the rest of this project applies to measurements: a signal that
# is absent must not be scored as a signal that is good.
# 0 = the check output says the service is published, 1 = not yet. Pure function of the string so
# lib_install_test.sh can pin it against outputs MEASURED on a real device, which is the only reason
# it is right: the previous comment here documented the output as
# "Service package: [android.content.pm.IPackageManager]", and API 34 actually prints
# "Service package: found" / "Service package: not found". A first attempt to harden this matched
# the documented form and so never became ready at all — 120 s of waiting, then a false
# "the device is not ready". Both spellings are accepted now, and "not found" is tested BEFORE
# "found" because it contains it.
pm_check_ready() {                    # $1 = output of `service check package`
    case "$1" in
        *"not found"*)              return 1 ;;
        *found*|*IPackageManager*)  return 0 ;;
        *)                          return 1 ;;   # empty, "error: device offline", anything unknown
    esac
}

wait_for_pm() {                       # $1 = max seconds (default 120)
    local cap="${1:-120}" i
    for i in $(seq 1 "$cap"); do
        if pm_check_ready "$(adb shell service check package 2>/dev/null | tr -d '\r')"; then
            # Published is not the same as able to answer: at the moment `service check` first
            # succeeds, `pm` can still return "Can't find service: package". `pm path android` is
            # the cheapest command that proves the service will actually take one, and it is the
            # capability the install needs — so require it rather than inferring it.
            case "$(adb shell pm path android 2>&1 | tr -d '\r')" in
                package:*) return 0 ;;
            esac
        fi
        sleep 1
    done
    return 1
}

# Classify one `pm install` output.  0 = installed, 1 = transient (retry), 2 = real rejection.
#
# A SEPARATE FUNCTION so it can be tested against real strings without an emulator
# (`lib_install_test.sh`). It was inline, and the classification was wrong in the dangerous
# direction: `cmd: Can't find service: package` — the package service not yet published, the very
# condition this file exists to absorb — fell into the catch-all and was reported as
# "a real rejection — signature, ABI or manifest — not a flake". That is exactly the failure this
# library's own header warns about: a transient error reading as "the build is broken", sending a
# reader after a bug that is not there. Observed 2026-07-28, killing a 15-minute capture run.
install_classify() {                  # $1 = the pm install output
    case "$1" in
        *Success*)                                       return 0 ;;
        *"Broken pipe"*|*"DeadObjectException"*|\
        *"Failure calling service"*|\
        *"Can't find service"*|*"Can not find service"*|\
        *"Service not registered"*)                      return 1 ;;
        *)                                               return 2 ;;
    esac
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
        install_classify "$out"
        case $? in
            0) [ "$t" -gt 1 ] && echo "  install: ok (attempt $t)" || echo "  install: ok"
               return 0 ;;
            1) echo "  install: attempt $t hit a transient service error, retrying: $out"
               sleep 10 ;;
            *) echo "  install: REJECTED by the package manager: $out"
               echo "           (a real rejection — signature, ABI or manifest — not a flake)"
               return 1 ;;
        esac
    done
    echo "  install: still failing after $tries attempts on transient errors"
    return 1
}

# apk_signer — a stable per-signer fingerprint for an APK, WITHOUT requiring apksigner.
#
# apksigner lives in ab-port, NOT in the ab-emu* images, and two separate scripts have now been
# written against it there and failed the same way: emu_signature_clash.sh got two EMPTY digests
# (and would have "proved" they differ), and emu_update_install.sh aborted with "the two builds do
# not share a signer (A=? B=?)". Same fix twice is a fix that will drift, so it lives here once.
#
# Falls back to the CERTIFICATE inside the v1 signature block, extracted with openssl.
#
# NOT a hash of META-INF/*.RSA itself. That file is the signature BLOCK, whose bytes depend on the
# APK's content, so two APKs signed with the SAME key hash differently — the first version of this
# fallback did exactly that and reported the release and audio builds as differently signed when
# apksigner says both are d56d5b2e…. It would also have let emu_signature_clash.sh's "the two APKs
# must really differ" premise pass for the wrong reason. Caught only by printing both values and
# noticing they disagreed with the known answer.
#
# The returned fingerprint is comparable only against another value from the SAME method — apksigner
# and openssl hash different encodings. Every caller compares two APKs within one run, so that holds.
apk_signer() {                        # $1 = apk path
    local d
    d=$(apksigner verify --print-certs "$1" 2>/dev/null | grep -m1 'SHA-256 digest' | awk '{print $NF}')
    [ -n "$d" ] && { echo "$d"; return 0; }
    d=$(unzip -p "$1" 'META-INF/*.RSA' 2>/dev/null \
        | openssl pkcs7 -inform DER -print_certs -outform DER 2>/dev/null \
        | sha256sum | cut -d' ' -f1)
    [ -n "$d" ] && [ "$d" != "$(printf '' | sha256sum | cut -d' ' -f1)" ] && { echo "$d"; return 0; }
    return 1                          # could not determine — caller must not treat this as "equal"
}
