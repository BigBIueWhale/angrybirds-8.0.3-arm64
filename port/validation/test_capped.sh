#!/bin/bash
# test_capped.sh — regression tests for capped_counts.py and lib_metrics.sh's saturated_report.
#
# WHAT WENT WRONG, AND WHY THIS FILE EXISTS
# -----------------------------------------
# capped_counts.py exists to stop a rate-limited log count being quoted as a measurement. It had no
# tests, and it under-reported: `saturated_report` scraped the human report with a shell regex looking
# for a SINGLE-quoted marker, and the shim's empty-json guard format string contains `'{}'`:
#
#     "[empty-json-guard] empty JSON parse -> '{}' (prevents the level-end ParseError->Lua-panic exit)"
#      ^ python repr() switches to DOUBLE quotes when the string itself holds an apostrophe
#
# so that marker never matched, and a genuine floor (22 hits against a cap of 8, on the phone log) was
# silently missing from the report. An omitted floor reads as "this count is a real total" — the exact
# error the tool was written to prevent, committed by the tool itself.
#
# A SECOND LESSON, FROM WRITING THIS FILE
# ---------------------------------------
# Its first version hardcoded marker strings copied out of `--list`, which truncates to 70 characters
# for display. The copies were therefore PREFIXES of the real format strings and matched nothing, so
# two tests passed VACUOUSLY: they were negative assertions ("this must not be reported") evaluated
# against a log the tool could not see at all. Fixtures now come from `--markers` (untruncated), and
# every negative assertion is two-sided — it first proves the tool SEES the marker, then proves it is
# not classified as a floor. A negative test that never establishes the positive half is decoration.
#
#   bash port/validation/test_capped.sh          # no emulator, no device, no network
set +e
cd "$(dirname "$0")" || exit 1
TOOL=./capped_counts.py
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
PASS=0; FAIL=0
ok(){  printf "  [ OK ] %s\n" "$1"; PASS=$((PASS+1)); }   # uppercase: validate_all.sh stage 1 counts it
bad(){ printf "  [FAIL] %s\n" "$1"; FAIL=$((FAIL+1)); }

# Fixtures FROM THE TOOL: tag<TAB>cap<TAB>site<TAB>full literal.
MK="$TMP/markers.tsv"
python3 "$TOOL" --markers > "$MK" 2>/dev/null
NMK=$(grep -c . "$MK")
if [ "${NMK:-0}" -lt 15 ]; then
  echo "  [FAIL] --markers produced only ${NMK:-0} entries; every test below would be vacuous"
  echo "CAP-AUDIT SELF-TEST FAILED"; exit 1
fi
lit(){ awk -F'\t' -v t="$1" '$1==t {print $4; exit}' "$MK"; }   # full literal for a tag
cap(){ awk -F'\t' -v t="$1" '$1==t {print $2; exit}' "$MK"; }   # its cap

APOS=$(lit '[empty-json-guard]');       APOSC=$(cap '[empty-json-guard]')
PLAIN=$(lit '[s-construct-null-guard]'); PLAINC=$(cap '[s-construct-null-guard]')
BASEL=$(lit '[audio-isolate]');          BASELC=$(cap '[audio-isolate]')
for v in "$APOS" "$PLAIN" "$BASEL"; do
  [ -n "$v" ] || { echo "  [FAIL] a required marker is absent from --markers; the shim source changed"
                   echo "CAP-AUDIT SELF-TEST FAILED"; exit 1; }
done

mklog(){ # $1=out  then pairs of: count marker
  local out="$1"; shift
  : > "$out"
  while [ $# -ge 2 ]; do
    local n="$1" m="$2"; shift 2
    local i=0
    while [ "$i" -lt "$n" ]; do printf '07-30 02:00:00.000 1 1 I abshim  : %s rest\n' "$m" >> "$out"; i=$((i+1)); done
  done
}

echo "== 1. the regression: a marker containing an apostrophe must still be reported =="
mklog "$TMP/apos.txt" $((APOSC + 14)) "$APOS"
if python3 "$TOOL" --tags "$TMP/apos.txt" | grep -q '^\[empty-json-guard\]'; then
  ok "--tags reports [empty-json-guard] despite the '{}' in its format string"
else
  bad "--tags STILL drops the apostrophe marker — the original bug is back"
fi

echo
echo "== 2. --tags and the human report must agree on how many floors there are =="
mklog "$TMP/both.txt" $((APOSC + 14)) "$APOS" $((PLAINC + 1)) "$PLAIN" $((BASELC + 2)) "$BASEL"
T=$(python3 "$TOOL" --tags "$TMP/both.txt" | grep -c .)
H=$(python3 "$TOOL" "$TMP/both.txt" 2>/dev/null | grep -c '\[FLOOR\]')
[ "$T" = "$H" ] && [ "${T:-0}" -gt 0 ] \
  && ok "both interfaces report $T floors" \
  || bad "parity broken: --tags=$T human=$H (a mismatch means one of them is losing markers)"

echo
echo "== 3. a count BELOW its cap is a real total, not a floor =="
mklog "$TMP/under.txt" $((PLAINC - 1)) "$PLAIN"
# Two-sided. First prove the tool SEES it — otherwise "not reported as a floor" is true of any file,
# including an empty one, and the assertion means nothing.
if python3 "$TOOL" "$TMP/under.txt" 2>/dev/null | grep -q "of max ${PLAINC}"; then
  if python3 "$TOOL" --tags "$TMP/under.txt" | grep -q 's-construct'; then
    bad "$((PLAINC - 1)) hits against a cap of $PLAINC was called a floor — that makes every count suspect"
  else
    ok "$((PLAINC - 1)) against a cap of $PLAINC is seen, and left alone as a real count"
  fi
else
  bad "the tool did not even see the under-cap marker, so this case proves nothing"
fi

echo
echo "== 4. a count exactly AT its cap is a floor (the boundary, where off-by-one lives) =="
mklog "$TMP/at.txt" "$PLAINC" "$PLAIN"
python3 "$TOOL" --tags "$TMP/at.txt" | grep -q 's-construct' \
  && ok "exactly-at-cap ($PLAINC) counts as saturated" \
  || bad "a count sitting exactly on its cap was treated as a real total"

echo
echo "== 5. saturated_report separates baseline tracing from real deviations =="
source ./lib_metrics.sh
mklog "$TMP/baseonly.txt" $((BASELC + 2)) "$BASEL"
R=$(saturated_report "$TMP/baseonly.txt")
case "$R" in
  *"always-saturated tracing"*) ok "a log with only baseline saturation reports no deviation" ;;
  *) bad "baseline-only log reported as a deviation: $R" ;;
esac
R=$(saturated_report "$TMP/both.txt")
case "$R" in
  *empty-json-guard*s-construct*|*s-construct*empty-json-guard*)
     ok "both non-baseline tags are named in the report" ;;
  *) bad "the report omitted a non-baseline tag: $R" ;;
esac

echo
echo "== 6. a log with no saturation says so, rather than saying nothing =="
mklog "$TMP/clean.txt" 2 "$PLAIN"
R=$(saturated_report "$TMP/clean.txt")
case "$R" in
  *"real total"*) ok "an unsaturated log is reported as unsaturated" ;;
  *) bad "an unsaturated log produced no clear statement: $R" ;;
esac

echo
echo "== 7. saturated_report refuses to report at all if its two inputs disagree =="
# The parity guard is the thing standing between a future parsing change and a silently short report.
# Force a disagreement by pointing the helper at a stub that emits one interface but not the other.
STUB="$TMP/stub"; mkdir -p "$STUB"
cat > "$STUB/capped_counts.py" <<'STUBEOF'
import sys
if "--tags" in sys.argv:
    print("[fake-tag]\t99\t8\tx.c:1")   # one floor on the machine interface
# and nothing resembling a [FLOOR] line on the human interface -> parity must fail
STUBEOF
cp ./lib_metrics.sh "$STUB/lib_metrics.sh"
R=$( cd "$STUB" && bash -c 'source ./lib_metrics.sh; saturated_report /dev/null' )
case "$R" in
  *"REPORT BROKEN"*) ok "a disagreement between the two interfaces is reported, not papered over" ;;
  *) bad "parity guard did not fire when the interfaces disagreed: $R" ;;
esac

echo
echo "== 8. the cap table itself is not empty =="
# If the GUARD regex ever stops matching the shim source, every log looks clean and every capped count
# silently becomes quotable. That failure is invisible without this check.
[ "${NMK:-0}" -ge 15 ] \
  && ok "$NMK capped log sites found in the shim source" \
  || bad "only ${NMK:-0} capped sites found — the extraction regex has stopped matching the source"

echo
echo "PASS=$PASS FAIL=$FAIL"
# Vacuity guard: 9 assertions on the healthy path.
EXPECT_CHECKS=9
TOT=$((PASS+FAIL))
if [ "$TOT" -ne "$EXPECT_CHECKS" ]; then
  echo "  [FAIL] expected $EXPECT_CHECKS assertions, ran $TOT — update EXPECT_CHECKS deliberately"
  FAIL=$((FAIL+1))
fi
[ "$FAIL" -eq 0 ] && echo "ALL CAP-AUDIT CASES BEHAVE" || echo "CAP-AUDIT SELF-TEST FAILED"
exit "$FAIL"
