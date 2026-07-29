#!/bin/bash
# test_playassert.sh — prove the playthrough verdict can fail, and fails for the right reason.
#
# The three scripts this verdict serves carry PROOF_8/11/14, the "plays through to a win on modern
# Android" evidence. Before this, they printed their numbers, discarded win_check's return value and
# exited 0 whatever happened. Assertions added to a 30-minute emulator job are assertions nobody can
# perturb, so each one is exercised here against synthetic logs and REAL screenshots — a genuine win
# frame, a genuine mid-flight frame — in about a second.
#
#   bash port/validation/test_playassert.sh          # no emulator, no device, no network
set +e
cd "$(dirname "$0")" || exit 1
# NOT sourcing lib_wincheck.sh here on purpose: lib_playassert.sh must pull its own dependency,
# and this is what proves it does.
source ./lib_playassert.sh

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
PASS=0; FAIL=0
SHOTS=../../reports/shots
WIN="$SHOTS/PROOF_8_modern_android_win.png"          # scored [ WIN ] by win_detect.py
NOTWIN="$SHOTS/PROOF_3_bird_launched.png"            # mid-flight: a real frame, but not a win

good_log(){
    {   echo "07-29 18:00:00.000  1 1 I abshim  : init_array 125/125"
        echo "07-29 18:00:20.000  1 1 I abshim  : frame[1501]"
    } > "$1"
}
# NOT named LOG: lib_playassert.sh tees its verdict into $LOG when the caller defines no
# say(), so calling this LOG made the assertions write "[FAIL] h_fatal ..." INTO the very
# log they were analysing, and the next check counted their own output. This is an
# abshim log; the name now says so.
ABL="$TMP/ab.txt"; good_log "$ABL"

run_case(){ # <name> <want pass|fail> <expected text> -- <ablog> <shot> <pid> [minframe]
    local name="$1" want="$2" text="$3"; shift 3
    [ "$1" = "--" ] && shift
    local out rc ok=1
    out=$(assert_playthrough "$@" 2>&1); rc=$?
    case "$want" in
        pass) [ "$rc" -eq 0 ] || ok=0 ;;
        fail) [ "$rc" -gt 0 ] || ok=0 ;;
    esac
    [ -n "$text" ] && ! printf '%s\n' "$out" | grep -qF "$text" && ok=0
    if [ "$ok" = 1 ]; then echo "  [ OK ] $name"; PASS=$((PASS+1))
    else echo "  [FAIL] $name (rc=$rc, wanted $want${text:+ + \"$text\"})"
         printf '%s\n' "$out" | sed 's/^/          | /'; FAIL=$((FAIL+1)); fi
}

echo "== control: a healthy winning run =="
run_case "a winning playthrough passes" pass "" -- "$ABL" "$WIN" 3782
NOK=$(assert_playthrough "$ABL" "$WIN" 3782 2>&1 | grep -c '^  \[ OK \]')
# The expected count is deliberately HARDCODED — that is the whole point of a vacuity guard, and it
# is meant to break when a check is added or removed so somebody confirms the change was intended.
# It did exactly that when the wild-memory-access check was added (5 -> 6).
EXPECT_CHECKS=6
if [ "$NOK" -eq "$EXPECT_CHECKS" ]; then
    echo "  [ OK ] the control ran all $EXPECT_CHECKS checks (not vacuously silent)"; PASS=$((PASS+1))
else
    echo "  [FAIL] the control emitted $NOK [ OK ] lines, expected $EXPECT_CHECKS — if a check was"
    echo "         added or removed on purpose, update EXPECT_CHECKS; if not, one went missing"
    FAIL=$((FAIL+1))
fi

echo
echo "== each failure must be caught, and named correctly =="

M="$TMP/noctor.txt"; grep -v init_array "$ABL" > "$M"
run_case "constructors never finished"  fail "init_array 125/125" -- "$M" "$WIN" 3782

M="$TMP/lowfr.txt"; sed 's/frame\[1501\]/frame[42]/' "$ABL" > "$M"
run_case "renderer stalled early"       fail "below the 601"      -- "$M" "$WIN" 3782

M="$TMP/uaf.txt"; cp "$ABL" "$M"
echo "I abshim : [uaf-survive] wild write @0x0 (pc=+0x1234) -> mapped zero page 0x0, continuing" >> "$M"
run_case "a wild write papered over"    fail "were absorbed into fresh zero pages" -- "$M" "$WIN" 3782

M="$TMP/fatal.txt"; cp "$ABL" "$M"; echo "I abshim : [h_fatal] boom" >> "$M"
run_case "h_fatal during the run"       fail "h_fatal during"     -- "$M" "$WIN" 3782

run_case "process died before the end"  fail "process was gone"   -- "$ABL" "$WIN" ""

# The important one: everything mechanical is healthy, and the game simply did not win. This is the
# case the old code reported as success.
run_case "healthy run that did NOT win" fail "is NOT a win"       -- "$ABL" "$NOTWIN" 3782

run_case "missing screenshot"           fail "could not run"      -- "$ABL" "$TMP/nope.png" 3782

M="$TMP/empty.txt"; : > "$M"
run_case "an empty log"                 fail ""                   -- "$M" "$WIN" 3782

echo
echo "== a stricter frame floor must be honoured =="
run_case "frame floor above what ran"   fail "below the 5000"     -- "$ABL" "$WIN" 3782 5000

echo
echo "== the harness must not measure its own output =="
# Set LOG to the very file under analysis, in a SUBSHELL so it cannot leak into the other cases.
CONTAM=$( LOG="$ABL"; assert_playthrough "$ABL" "$WIN" 3782 2>&1 )
if printf '%s\n' "$CONTAM" | grep -qF "harness error"; then
    echo "  [ OK ] run-log == abshim-log is refused, not silently self-contaminated"; PASS=$((PASS+1))
else
    echo "  [FAIL] the self-contamination guard did not fire"; FAIL=$((FAIL+1))
    printf '%s\n' "$CONTAM" | sed 's/^/          | /'
fi

echo
echo "== progression: advancing is a two-sided claim =="
L2="$SHOTS/PROOF_9_modern_android_level2.png"     # a real level-2 frame: sane, and not a win
prog_case(){ # <name> <want> <text> -- <log> <cleared> <next> <pid>
    local name="$1" want="$2" text="$3"; shift 3
    [ "$1" = "--" ] && shift
    local out rc ok=1
    out=$(assert_progression "$@" 2>&1); rc=$?
    case "$want" in pass) [ "$rc" -eq 0 ] || ok=0 ;; fail) [ "$rc" -gt 0 ] || ok=0 ;; esac
    [ -n "$text" ] && ! printf '%s\n' "$out" | grep -qF "$text" && ok=0
    if [ "$ok" = 1 ]; then echo "  [ OK ] $name"; PASS=$((PASS+1))
    else echo "  [FAIL] $name (rc=$rc, wanted $want)"; printf '%s\n' "$out" | sed 's/^/          | /'; FAIL=$((FAIL+1)); fi
}
prog_case "cleared=win + next=fresh level" pass ""                    -- "$ABL" "$WIN" "$L2" 3782
prog_case "never left the results screen"  fail "STILL a win screen"  -- "$ABL" "$WIN" "$WIN" 3782
prog_case "the level was never cleared"    fail "is NOT a win"        -- "$ABL" "$L2"  "$L2" 3782
BLANK=$(mktemp -u)/blank.png; mkdir -p "$(dirname "$BLANK")"
python3 - "$BLANK" <<'PYGEN'
import sys, zlib, struct
w=h=64; rows=bytearray()
for y in range(h):
    rows.append(0); rows += bytes((0,0,0))*w
def ch(t,d):
    c=struct.pack('>I',len(d))+t+d; return c+struct.pack('>I', zlib.crc32(t+d)&0xffffffff)
open(sys.argv[1],'wb').write(b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))
                             +ch(b'IDAT',zlib.compress(bytes(rows)))+ch(b'IEND',b''))
PYGEN
prog_case "crashed to a black frame"       fail "not a picture of anything" -- "$ABL" "$WIN" "$BLANK" 3782
rm -rf "$(dirname "$BLANK")"

echo
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ] && echo "ALL PLAYTHROUGH-ASSERTION CASES BEHAVE" || echo "PLAYTHROUGH-ASSERTION SELF-TEST FAILED"
exit "$FAIL"
