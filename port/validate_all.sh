#!/bin/bash
# validate_all.sh — run every validation that does NOT need an emulator, in one command.
#
# WHY THIS EXISTS
# ---------------
# The pieces existed; nothing ran them together. `reproduce.sh` builds the APK and checks the
# documented claims about it, but never runs the test suites. So the suites drifted:
# `arm64_cross_test.sh`'s predecessor had its build cache disappear and fell a whole session behind
# the shim changes before anyone noticed, and the only reason that was caught was someone going
# looking. A single entry point makes "did I break anything" a question you can actually answer.
#
# What this covers:
#   1. host test suite      x86, ASan+UBSan, 10 module + 7 device tests + the coverage hard-gate
#   2. arm64 cross suite    the SAME 17 tests on AArch64 — the ABI the phone uses — plus binary
#                           architecture assertions
#   3. verify_claims        every documented claim re-checked against the shipped artifact
#
# What this does NOT cover, and why:
#   - the emulator play/win/progression tests (`emu_*.sh`) need /dev/kvm, take 20-40 minutes each,
#     and several are timing-sensitive enough that their screenshots must be LOOKED AT rather than
#     scored by exit code. They stay separate on purpose. See port/validation/README.md.
#   - anything needing the physical A56. See port/OPEN_FINDINGS.md.
#
#   bash port/validate_all.sh
# Exits non-zero if any stage fails.
set +e
cd "$(dirname "$0")/.." || exit 1
FAIL=0
# Bold only when stdout is a terminal. Redirected to a file the escapes render as literal
# "[1m", which is exactly how these logs get read afterwards.
if [ -t 1 ]; then B=$(printf "\033[1m"); N=$(printf "\033[0m"); else B=""; N=""; fi
stage(){ printf "\n%s== %s ==%s\n" "$B" "$1" "$N"; }
pass(){ printf "  [ PASS ] %s\n" "$1"; }
fail(){ printf "  [ FAIL ] %s\n" "$1"; FAIL=1; }

need_image(){   # $1=image  $2=dockerfile
  docker image inspect "$1" >/dev/null 2>&1 && return 0
  echo "  image $1 missing — building from $2 (needs network, one time)"
  docker build -q -f "$2" -t "$1" port/docker >/dev/null 2>&1 \
    && { echo "  built $1"; return 0; } || { fail "could not build $1"; return 1; }
}

stage "1/3 host test suite (x86, ASan+UBSan, + coverage hard-gate)"
if need_image ab-hosttest port/docker/Dockerfile.ab-hosttest; then
  OUT=$(docker run --rm --network none -v "$PWD":/work -w /work ab-hosttest \
          bash port/shim/test/run_tests.sh 2>&1)
  echo "$OUT" | tail -4 | sed 's/^/    /'
  echo "$OUT" | grep -q "ALL MODULE TESTS PASSED" && pass "host suite" || fail "host suite"
  echo "$OUT" | grep -q "COVERAGE OK" && pass "every engine import is bridged" || fail "coverage gate"
fi

stage "2/3 arm64 cross suite (the same tests on AArch64 — the phone's ABI)"
if need_image ab-arm64x port/docker/Dockerfile.ab-arm64x; then
  OUT=$(docker run --rm --network none -v "$PWD":/work -w /work ab-arm64x \
          bash port/validation/arm64_cross_test.sh 2>&1)
  echo "$OUT" | grep -cE "^  \[ OK \]" | sed 's/^/    checks passed on AArch64: /'
  echo "$OUT" | grep -E "^  \[FAIL\]" | sed 's/^/    /'
  echo "$OUT" | grep -q "ARM64 CROSS TEST PASSED" && pass "arm64 suite" || fail "arm64 suite"
fi

stage "3/3 documented claims vs the shipped artifact"
if [ ! -f out/angrybirds-8.0.3-arm64.apk ]; then
  echo "    no artifact yet — run port/reproduce.sh first"; fail "verify_claims (no artifact)"
elif need_image ab-port port/docker/Dockerfile.ab-port; then
  OUT=$(docker run --rm --network none -v "$PWD":/work -w /work ab-port \
          bash port/validation/verify_claims.sh 2>&1)
  echo "$OUT" | grep -E "^  \[FAIL\]" | sed 's/^/    /'
  echo "$OUT" | tail -2 | sed 's/^/    /'
  echo "$OUT" | grep -q "ALL CHECKED CLAIMS HOLD" && pass "documented claims" || fail "documented claims"
fi

printf "\n"
if [ "$FAIL" = 0 ]; then
  echo "ALL OFFLINE VALIDATION PASSED"
  echo "  Not covered here: the emulator play/win tests (need /dev/kvm, and their screenshots must be"
  echo "  looked at, not scored) and anything needing the physical A56 — see port/OPEN_FINDINGS.md."
else
  echo "VALIDATION FAILED — see [ FAIL ] above"
fi
exit "$FAIL"
