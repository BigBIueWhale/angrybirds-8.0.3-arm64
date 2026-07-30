#!/bin/bash
# test_runlock.sh — prove lib_runlock.sh actually refuses a concurrent evidence run.
#
# WHY THIS EXISTS
# --------------
# 50 scripts in this directory write into the shared reports/shots namespace and none of them held a
# lock. Two emulator containers ran at once, both wrote reports/shots/playthrough_abshim.txt, and a
# finding (R61) was recorded from numbers that may have been a BLEND of two builds. A blended log has
# the right format, plausible values and monotonic-looking frames -- it is undetectable afterwards.
#
# The guard's FIRST version was also wrong in a way that mattered: it defaulted the lockfile to /tmp,
# and since every run is inside its own container with its own /tmp, two containers would both have
# acquired it. It would not have prevented the thing it was written for. That is why case 5 below
# asserts the shared-mount preference explicitly rather than trusting it.
#
#   bash port/validation/test_runlock.sh          # no emulator, no device, no network
set +e
cd "$(dirname "$0")" || exit 1
LIB=./lib_runlock.sh
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
PASS=0; FAIL=0
ok(){  printf "  [ OK ] %s\n" "$1"; PASS=$((PASS+1)); }   # uppercase: validate_all.sh stage 1 counts it
bad(){ printf "  [FAIL] %s\n" "$1"; FAIL=$((FAIL+1)); }

[ -f "$LIB" ] || { echo "  [FAIL] $LIB missing"; echo "RUNLOCK SELF-TEST FAILED"; exit 1; }

echo "== 1. a single run acquires =="
if ABSHIM_RUNLOCK="$TMP/a.lock" bash -c "source $LIB; acquire_run_lock solo" >/dev/null 2>&1; then
  ok "a lone run acquires the lock"
else
  bad "a lone run was refused — the guard blocks everything"
fi

echo
echo "== 2. a CONCURRENT run is refused =="
ABSHIM_RUNLOCK="$TMP/b.lock" bash -c "source $LIB; acquire_run_lock holder && sleep 6" >/dev/null 2>&1 &
HP=$!
sleep 2
if ABSHIM_RUNLOCK="$TMP/b.lock" bash -c "source $LIB; acquire_run_lock second" >"$TMP/second.out" 2>&1; then
  bad "a concurrent run was ALLOWED — the guard does nothing, which is the whole point"
else
  ok "a concurrent run is refused"
fi
grep -q 'REFUSING TO RUN' "$TMP/second.out" \
  && ok "the refusal says what it is refusing" \
  || bad "refused, but silently — the operator cannot tell why"
grep -qF "$TMP/b.lock" "$TMP/second.out" \
  && ok "the refusal names the lockfile so it can be found" \
  || bad "the refusal does not name the lockfile"
wait $HP 2>/dev/null

echo
echo "== 3. the lock is RELEASED when the holder exits =="
# Without this the first run would poison every later run and the guard would be worse than nothing.
if ABSHIM_RUNLOCK="$TMP/b.lock" bash -c "source $LIB; acquire_run_lock after" >/dev/null 2>&1; then
  ok "a later run acquires once the holder has exited"
else
  bad "the lock outlived its holder — every subsequent run would be blocked"
fi

echo
echo "== 4. the override is honoured =="
# Two DIFFERENT lockfiles must not block each other, or unrelated work would serialise needlessly.
ABSHIM_RUNLOCK="$TMP/c.lock" bash -c "source $LIB; acquire_run_lock c && sleep 5" >/dev/null 2>&1 &
HP2=$!
sleep 2
if ABSHIM_RUNLOCK="$TMP/d.lock" bash -c "source $LIB; acquire_run_lock d" >/dev/null 2>&1; then
  ok "a different lockfile is independent"
else
  bad "an unrelated lockfile was blocked — the override is not honoured"
fi
wait $HP2 2>/dev/null

echo
echo "== 5. the default lock lives on the SHARED mount, not container-local /tmp =="
# This is the bug the first version shipped with: /tmp is per-container, so two containers would both
# acquire and the guard would be theatre. Asserted on the source because /work only exists inside the
# containers; the cross-container behaviour itself was verified by running two ab-port containers, one
# of which was correctly refused.
if grep -q 'if \[ -d /work \]' "$LIB" && grep -q '/work/.abshim-evidence-run.lock' "$LIB"; then
  ok "the default prefers /work (visible across containers)"
else
  bad "the default no longer prefers /work — two containers could both acquire, as they once did"
fi
grep -q '/tmp/abshim-evidence-run.lock' "$LIB" \
  && ok "a /tmp fallback remains for host-side use where /work is absent" \
  || bad "no fallback: host-side callers would have no lockfile at all"

echo
echo "PASS=$PASS FAIL=$FAIL"
# Vacuity guard: 8 assertions on the healthy path.
EXPECT_CHECKS=8
TOT=$((PASS+FAIL))
if [ "$TOT" -ne "$EXPECT_CHECKS" ]; then
  echo "  [FAIL] expected $EXPECT_CHECKS assertions, ran $TOT — update EXPECT_CHECKS deliberately"
  FAIL=$((FAIL+1))
fi
[ "$FAIL" -eq 0 ] && echo "ALL RUNLOCK CASES BEHAVE" || echo "RUNLOCK SELF-TEST FAILED"
exit "$FAIL"
