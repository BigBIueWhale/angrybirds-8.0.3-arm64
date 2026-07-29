#!/bin/bash
# test_saveassert.sh — prove the save-persistence assertions can actually fail.
#
# Adding assertions to a test is only half the job. Assertions that live inside a 20-minute emulator
# run get written once, pass once, and are never perturbed again; several checks in this tree turned
# out to be incapable of failing and nobody noticed, because a green line looks the same either way.
#
# So each check gets fed synthetic evidence here, offline, in about a second:
#   - the CONTROL case supplies healthy evidence and must produce zero failures and 8 [ OK ] lines
#     (counted: if the function silently stopped emitting checks, "no failures" would still be true)
#   - each MUTATION breaks exactly one input and must produce a failure whose text names that input.
#     A mutation that fails for the wrong reason is not evidence, so the expected text is matched,
#     not just the exit status.
#
#   bash port/validation/test_saveassert.sh          # no emulator, no device, no network
set +e
cd "$(dirname "$0")" || exit 1
source ./lib_saveassert.sh

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
PASS=0; FAIL=0

# --- healthy evidence -------------------------------------------------------------------------
good_log(){
    {   echo "07-29 18:00:00.000  1234  1234 I abshim  : init_array 125/125"
        for i in $(seq 1 40); do
            echo "07-29 18:00:0$((i%10)).000  1234  1234 I abshim  : fopen /data/data/com.rovio.angrybirds/files/settings.lua"
        done
        echo "07-29 18:00:20.000  1234  1234 I abshim  : frame[301]"
    } > "$1"
}
GOOD_SAVES='     412 /data/data/com.rovio.angrybirds/files/settings.lua
      88 /data/data/com.rovio.angrybirds/files/highscores.lua
     500 total'
GOOD_NF=46
GOOD_PID=2659

LOG="$TMP/ab2.txt"; good_log "$LOG"

# expect <name> <expected-failure-count-relation> <expected-text> -- <nf> <log> <pid> <saves>
run_case(){
    local name="$1" want="$2" text="$3"; shift 3
    [ "$1" = "--" ] && shift
    local out rc
    out=$(assert_save_persistence "$1" "$2" "$3" "$4" 2>&1); rc=$?
    local ok=1
    case "$want" in
        pass) [ "$rc" -eq 0 ] || ok=0 ;;
        fail) [ "$rc" -gt 0 ] || ok=0 ;;
    esac
    if [ -n "$text" ] && ! printf '%s\n' "$out" | grep -qF "$text"; then ok=0; fi
    if [ "$ok" = 1 ]; then
        echo "  [ OK ] $name"; PASS=$((PASS+1))
    else
        echo "  [FAIL] $name  (rc=$rc, wanted $want${text:+ + text \"$text\"})"
        printf '%s\n' "$out" | sed 's/^/          | /'
        FAIL=$((FAIL+1))
    fi
}

echo "== control: healthy evidence must pass =="
run_case "control passes" pass "" -- "$GOOD_NF" "$LOG" "$GOOD_PID" "$GOOD_SAVES"
# Vacuity guard: "zero failures" is also what an empty function returns. Count the OK lines.
NOK=$(assert_save_persistence "$GOOD_NF" "$LOG" "$GOOD_PID" "$GOOD_SAVES" 2>&1 | grep -c '\[ OK \]')
if [ "$NOK" -eq 8 ]; then
    echo "  [ OK ] the control ran all 8 checks (not vacuously silent)"; PASS=$((PASS+1))
else
    echo "  [FAIL] the control emitted $NOK [ OK ] lines, expected 8 — checks went missing"; FAIL=$((FAIL+1))
fi

echo
echo "== mutations: each must be caught, by name =="

run_case "no private files written" fail "nothing whose persistence could be tested" \
    -- 0 "$LOG" "$GOOD_PID" "$GOOD_SAVES"

run_case "settings.lua vanished" fail "settings.lua is missing or empty" \
    -- "$GOOD_NF" "$LOG" "$GOOD_PID" '      88 /data/data/com.rovio.angrybirds/files/highscores.lua'

run_case "settings.lua truncated to 0 bytes" fail "settings.lua is missing or empty" \
    -- "$GOOD_NF" "$LOG" "$GOOD_PID" '       0 /data/data/com.rovio.angrybirds/files/settings.lua
      88 /data/data/com.rovio.angrybirds/files/highscores.lua'

run_case "highscores.lua vanished" fail "highscores.lua is absent" \
    -- "$GOOD_NF" "$LOG" "$GOOD_PID" '     412 /data/data/com.rovio.angrybirds/files/settings.lua'

run_case "the app died on relaunch" fail "not running after the relaunch" \
    -- "$GOOD_NF" "$LOG" "" "$GOOD_SAVES"

M="$TMP/no_ctor.txt"; grep -v 'init_array' "$LOG" > "$M"
run_case "constructors did not complete" fail "never reached init_array 125/125" \
    -- "$GOOD_NF" "$M" "$GOOD_PID" "$GOOD_SAVES"

M="$TMP/partial_ctor.txt"; sed 's|init_array 125/125|init_array 124/125|' "$LOG" > "$M"
run_case "constructors stopped at 124/125" fail "never reached init_array 125/125" \
    -- "$GOOD_NF" "$M" "$GOOD_PID" "$GOOD_SAVES"

M="$TMP/stalled.txt"; sed 's|frame\[301\]|frame[7]|' "$LOG" > "$M"
run_case "renderer stalled at frame 7" fail "did not render" \
    -- "$GOOD_NF" "$M" "$GOOD_PID" "$GOOD_SAVES"

M="$TMP/noframe.txt"; grep -v 'frame\[' "$LOG" > "$M"
run_case "no frames at all" fail "did not render" \
    -- "$GOOD_NF" "$M" "$GOOD_PID" "$GOOD_SAVES"

M="$TMP/noreads.txt"; grep -v 'fopen' "$LOG" > "$M"
run_case "saves on disk but never read back" fail "read none of its own files" \
    -- "$GOOD_NF" "$M" "$GOOD_PID" "$GOOD_SAVES"

M="$TMP/fatal.txt"; cp "$LOG" "$M"; echo "07-29 18:00:21.000 I abshim  : [h_fatal] unhandled" >> "$M"
run_case "h_fatal during the reload" fail "h_fatal on the second launch" \
    -- "$GOOD_NF" "$M" "$GOOD_PID" "$GOOD_SAVES"

M="$TMP/empty.txt"; : > "$M"
run_case "an empty log (the emulator produced nothing)" fail "" \
    -- "$GOOD_NF" "$M" "$GOOD_PID" "$GOOD_SAVES"

M="$TMP/missing.txt"      # deliberately never created
run_case "a log file that does not exist" fail "" \
    -- "$GOOD_NF" "$M" "$GOOD_PID" "$GOOD_SAVES"

echo
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ] && echo "ALL SAVE-ASSERTION CASES BEHAVE" || echo "SAVE-ASSERTION SELF-TEST FAILED"
exit "$FAIL"
