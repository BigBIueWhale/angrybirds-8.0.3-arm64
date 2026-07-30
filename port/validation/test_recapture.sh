#!/bin/bash
# test_recapture.sh — prove log_recapture_audit.py detects appended `adb logcat` captures.
#
# The defect it guards against is not hypothetical: reports/shots/PROOF_PHONE_abshim.txt is seven
# appended captures, which inflated every count in it 4.33x and put wrong numbers into R50, a commit
# message and the memory record before anything noticed. `adb logcat` re-dumps the whole ring buffer
# unless you pass -T or clear it first.
#
# Two things are tested here, because the tool has two ways to be useless:
#   1. the DETECTOR — synthetic monotonic / appended / same-millisecond / undated logs
#   2. the PIN      — the one known-bad committed log is allowlisted BY JUMP COUNT, and that pin has
#                     to fail if the file is de-duplicated (evidence quietly rewritten) or appended
#                     to again. An allowlist that merely names a file is a check that cannot fail.
#
#   bash port/validation/test_recapture.sh          # no emulator, no device, no network
set +e
cd "$(dirname "$0")" || exit 1
TOOL=./log_recapture_audit.py
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
PASS=0; FAIL=0
ok(){  printf "  [ OK ] %s\n" "$1"; PASS=$((PASS+1)); }   # [ OK ] uppercase: validate_all.sh counts it
bad(){ printf "  [FAIL] %s\n" "$1"; FAIL=$((FAIL+1)); }

echo "== 1. the detector's own selftest =="
if python3 "$TOOL" --selftest; then ok "selftest passed"; else bad "selftest failed"; fi

echo
echo "== 2. the pinned known-bad log is still exactly as R50 describes it =="
REAL=../../reports/shots/PROOF_PHONE_abshim.txt
if [ ! -f "$REAL" ]; then
  bad "the pinned log $REAL is missing — R50 cites it"
else
  OUT=$(python3 "$TOOL" "$REAL" 2>&1)
  printf '%s\n' "$OUT" | grep -q '7 appended captures' \
    && ok "still reports 7 appended captures" \
    || bad "no longer reports 7 appended captures: $(printf '%s' "$OUT" | head -1)"
  # rc must be 0: a PINNED recapture is a known, documented state, not a suite failure.
  python3 "$TOOL" "$REAL" >/dev/null 2>&1 \
    && ok "a pinned recapture exits 0 (known state, not a failure)" \
    || bad "the pinned recapture fails the suite — the pin is not being honoured"
fi

echo
echo "== 3. the pin must FAIL if that evidence file is ever altered =="
# direction A: someone de-duplicates the raw log to make a check pass. R50's caveats would silently
# become wrong, so this must fire.
if [ -f "$REAL" ]; then
  sort -u "$REAL" > "$TMP/PROOF_PHONE_abshim.txt"
  if python3 "$TOOL" "$TMP/PROOF_PHONE_abshim.txt" >"$TMP/a.txt" 2>&1; then
    bad "de-duplicating the pinned log did NOT fail the check"
  else
    grep -q 'now monotonic' "$TMP/a.txt" \
      && ok "de-duplicating the pinned evidence fires the pin" \
      || { bad "it failed, but not for the pin reason: $(head -1 "$TMP/a.txt")"; }
  fi
  # direction B: an eighth capture appended. 6 jumps was pinned, 7 must not be waved through.
  { cat "$REAL"; head -2000 "$REAL"; } > "$TMP/PROOF_PHONE_abshim.txt"
  if python3 "$TOOL" "$TMP/PROOF_PHONE_abshim.txt" >"$TMP/b.txt" 2>&1; then
    bad "appending another capture to the pinned log did NOT fail the check"
  else
    grep -q 'CHANGED since R50' "$TMP/b.txt" \
      && ok "appending another capture fires the pin" \
      || bad "it failed, but not for the pin reason: $(head -1 "$TMP/b.txt")"
  fi
else
  bad "cannot test the pin — the pinned log is missing"
fi

echo
echo "== 4. an UNPINNED appended log must fail outright =="
python3 - "$TMP/fresh.txt" <<'PY'
import sys
b=[f"07-30 02:{s//60:02d}:{s%60:02d}.000 111 111 I abshim  : x\n" for s in range(0,60,3)]
open(sys.argv[1],'w').writelines(b+b)
PY
if python3 "$TOOL" "$TMP/fresh.txt" >/dev/null 2>&1; then
  bad "a new appended log was accepted — only the documented one may be known"
else
  ok "a new appended log fails the check"
fi

echo
echo "== 5. every committed log is accounted for =="
# The tool prints how many it verified. Zero verified would mean the glob matched nothing while still
# exiting 0 — a vacuous pass, which is the failure mode this suite keeps rediscovering.
OUT=$(python3 "$TOOL" ../../reports/shots/*.txt 2>&1)
N=$(printf '%s' "$OUT" | sed -n 's/.*  \([0-9]\+\) log(s) verified monotonic.*/\1/p')
printf '%s\n' "$OUT" | tail -2 | sed 's/^/    /'
if [ "${N:-0}" -ge 40 ]; then ok "$N committed logs verified monotonic"
else bad "only ${N:-0} logs verified — the audit is not seeing the corpus"; fi

echo
echo "PASS=$PASS FAIL=$FAIL"
# Vacuity guard: this file runs 7 assertions on the healthy path. If a refactor drops one, the suite
# must say so rather than report a smaller green run. (This guard caught its own author: 9 was written
# here first, counting the mutually-exclusive missing-file branches as if they also ran. Three of these
# guards have now caught a real miscount, which is the whole argument for having them.)
EXPECT_CHECKS=7
TOT=$((PASS+FAIL))
if [ "$TOT" -ne "$EXPECT_CHECKS" ]; then
  echo "  [FAIL] expected $EXPECT_CHECKS assertions, ran $TOT — update EXPECT_CHECKS deliberately"
  FAIL=$((FAIL+1))
fi
[ "$FAIL" -eq 0 ] && echo "ALL RECAPTURE-DETECTOR CASES BEHAVE" || echo "RECAPTURE SELF-TEST FAILED"
exit "$FAIL"
