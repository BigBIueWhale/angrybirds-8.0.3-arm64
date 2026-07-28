#!/bin/bash
# lib_install_test.sh — pin install_classify() against REAL `pm install` outputs. No emulator needed.
#
# WHY THIS EXISTS
# ---------------
# On 2026-07-28 a 15-minute GPU capture died at install with:
#
#     install: REJECTED by the package manager: cmd: Can't find service: package
#              (a real rejection — signature, ABI or manifest — not a flake)
#
# Both lines were wrong. Nothing was rejected: the package service had not been published yet, which
# is the exact condition lib_install.sh was written to absorb — it already retried the sibling
# "Failure calling service package: Broken pipe". One unlisted string fell through to the catch-all
# and was reported as a signature/ABI/manifest problem, which is the precise failure lib_install.sh's
# own header warns about: a transient error reading as "the build is broken", sending a reader after
# a bug that does not exist.
#
# The classification is the whole value of that library, and it was untested because it was inline in
# a function that needs a device. It is now a pure function of a string, so it can be pinned here.
#
# The table below matters in BOTH directions. Listing more strings as transient is not automatically
# safer: if a genuine rejection (wrong ABI, bad signature) were classified transient, the script
# would retry it three times and then report "still failing on transient errors" — hiding a real
# defect behind a flake. So real rejections are asserted to stay rejections.
#
#     bash port/validation/lib_install_test.sh
set +e
source "$(dirname "$0")/lib_install.sh"

PASS=0; FAIL=0
ck() {                                # $1 = expected code, $2 = label, $3 = output string
    install_classify "$3"; local got=$?
    if [ "$got" -eq "$1" ]; then
        PASS=$((PASS+1)); printf "  [ OK ] %-46s -> %s\n" "$2" "$got"
    else
        FAIL=$((FAIL+1)); printf "  [FAIL] %-46s -> %s, expected %s\n" "$2" "$got" "$1"
        printf "         string: %s\n" "$3"
    fi
}

echo "== installed (0) =="
ck 0 "plain Success"                        "Success"
ck 0 "Success with trailing noise"          "Success"$'\n'

echo "== transient, must RETRY (1) =="
# The two observed in this project, verbatim.
ck 1 "package service not yet published"    "cmd: Can't find service: package"
ck 1 "system server dropped the call"       "cmd: Failure calling service package: Broken pipe (32)"
ck 1 "American-spelling variant"            "cmd: Can not find service: package"
ck 1 "binder death"                         "java.lang.IllegalStateException: DeadObjectException"
ck 1 "service dereg mid-call"               "Error: Service not registered: package"

echo "== real rejection, must NOT be retried (2) =="
# If any of these were classified transient, a genuine defect would be retried and then reported as
# a flake — the dangerous direction.
ck 2 "wrong ABI"                            "Failure [INSTALL_FAILED_NO_MATCHING_ABIS]"
ck 2 "unsigned / bad certificate"           "Failure [INSTALL_PARSE_FAILED_NO_CERTIFICATES]"
ck 2 "signature mismatch on update"         "Failure [INSTALL_FAILED_UPDATE_INCOMPATIBLE: Package com.rovio.angrybirds signatures do not match previously installed version]"
ck 2 "out of space"                         "Failure [INSTALL_FAILED_INSUFFICIENT_STORAGE]"
ck 2 "manifest rejected"                    "Failure [INSTALL_PARSE_FAILED_MANIFEST_MALFORMED]"
ck 2 "older SDK than device requires"       "Failure [INSTALL_FAILED_OLDER_SDK]"
ck 2 "empty output (device gone silent)"    ""

echo "== package-service readiness (0 = published, 1 = not yet) =="
# MEASURED on abtest34 (Android 14) on 2026-07-28, not recalled: at boot_completed=1 the check says
# "not found" and `pm path android` fails; seconds later both succeed. The older interface-name form
# is accepted too, since the two Android generations print differently and this library runs on both.
ckr() {                               # $1 = expected, $2 = label, $3 = string
    pm_check_ready "$3"; local got=$?
    if [ "$got" -eq "$1" ]; then PASS=$((PASS+1)); printf "  [ OK ] %-46s -> %s\n" "$2" "$got"
    else FAIL=$((FAIL+1)); printf "  [FAIL] %-46s -> %s, expected %s\n" "$2" "$got" "$1"; fi
}
ckr 1 "API 34 before the service is up"     "Service package: not found"
ckr 0 "API 34 once it is up"                "Service package: found"
ckr 0 "older-Android interface-name form"   "Service package: [android.content.pm.IPackageManager]"
ckr 1 "empty (adb not answering)"           ""
ckr 1 "adb error, not a service answer"     "error: device offline"

echo
if [ "$FAIL" -eq 0 ]; then
    echo "  ALL $PASS INSTALL-CLASSIFICATION CASES PASSED"
    exit 0
fi
echo "  $FAIL of $((PASS+FAIL)) CASES FAILED — install outcomes would be misreported"
exit 1
