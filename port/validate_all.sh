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
#   3. allocation compare   the guest's allocation sequence, x86 vs AArch64, at two depths
#   4. verify_claims        every documented claim re-checked against the shipped artifact
#
# What this does NOT cover, and why:
#   - the emulator play/win/progression tests (`emu_*.sh`) need /dev/kvm, take 20-40 minutes each,
#     and several are timing-sensitive enough that their screenshots must be LOOKED AT rather than
#     scored by exit code. They stay separate on purpose. See port/validation/README.md.
#   - anything needing the physical A56. See port/OPEN_FINDINGS.md.
#
# All three images this depends on are verified to rebuild from their committed Dockerfiles:
# ab-hosttest and ab-port were rebuilt --no-cache and re-checked (ab-port's rebuild produces a
# BYTE-IDENTICAL deliverable), and ab-arm64x was built from its Dockerfile when it was created.
# See the "VERIFIED BY REBUILD" notes in port/docker/.
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

stage "1/4 host test suite (x86, ASan+UBSan, + coverage hard-gate)"
if need_image ab-hosttest port/docker/Dockerfile.ab-hosttest; then
  OUT=$(docker run --rm --network none -v "$PWD":/work -w /work ab-hosttest \
          bash port/shim/test/run_tests.sh 2>&1)
  echo "$OUT" | tail -4 | sed 's/^/    /'
  echo "$OUT" | grep -q "ALL MODULE TESTS PASSED" && pass "host suite" || fail "host suite"
  echo "$OUT" | grep -q "COVERAGE OK" && pass "every engine import is bridged" || fail "coverage gate"
fi

stage "2/4 arm64 cross suite (the same tests on AArch64 — the phone's ABI)"
if need_image ab-arm64x port/docker/Dockerfile.ab-arm64x; then
  OUT=$(docker run --rm --network none -v "$PWD":/work -w /work ab-arm64x \
          bash port/validation/arm64_cross_test.sh 2>&1)
  echo "$OUT" | grep -cE "^  \[ OK \]" | sed 's/^/    checks passed on AArch64: /'
  echo "$OUT" | grep -E "^  \[FAIL\]" | sed 's/^/    /'
  echo "$OUT" | grep -q "ARM64 CROSS TEST PASSED" && pass "arm64 suite" || fail "arm64 suite"
fi

stage "3/4 guest allocation sequence: x86 vs AArch64"
# Wired in because anything not run here goes stale - which is how the claim this checks became
# wrong twice (first "bit-identical across hosts", false; then a true-but-unrepeatable
# "7792 of 7793"). It needs both images, so it is skipped rather than failed if either is absent.
if docker image inspect ab-hosttest >/dev/null 2>&1 && docker image inspect ab-arm64x >/dev/null 2>&1; then
  OUT=$(bash port/validation/alloc_trace_compare.sh 2>&1)
  echo "$OUT" | grep -E "^  \[ OK \]|^  \[FAIL\]|^  \[DIFF\]" | sed 's/^/  /'
  # POSITIVE EVIDENCE REQUIRED. This used to pass whenever [FAIL]/[DIFF] were ABSENT from the
  # output — so an empty run passed. If docker failed to start, the image was wrong, or the script
  # died before comparing anything, nothing matched and this printed
  # "[ PASS ] allocation sequence identical on both hosts" having compared nothing at all. Every
  # other stage here greps for a SUCCESS marker and so fails closed; this one was inverted and
  # failed open. It is the project's most repeated defect — a zero from a measurement that never
  # happened — sitting in the top-level validator.
  #
  # alloc_trace_compare.sh runs two phases (ctors, native_init) and prints one "[ OK ]" per phase,
  # so require BOTH, and still reject any [FAIL]/[DIFF].
  NOK=$(printf '%s' "$OUT" | grep -c '\[ OK \]')
  if printf '%s' "$OUT" | grep -q "\[FAIL\]\|\[DIFF\]"; then
    fail "allocation sequence differs between hosts"
  elif [ "$NOK" -lt 2 ]; then
    fail "allocation comparison produced $NOK/2 phase results — it did not run, so nothing was compared"
  else
    pass "allocation sequence identical on both hosts ($NOK/2 phases compared)"
  fi
else
  echo "    needs both ab-hosttest and ab-arm64x"; fail "allocation comparison (image missing)"
fi

stage "4/4 documented claims vs the shipped artifact"
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
