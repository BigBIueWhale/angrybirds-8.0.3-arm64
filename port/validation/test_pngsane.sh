#!/bin/bash
# test_pngsane.sh — prove png_sane.py rejects the frames a broken run actually produces.
#
# The failure modes below are not hypothetical. Each one has occurred in this tree or is one
# `adb exec-out` away from occurring:
#   - a solid black frame        the app died, or the surface was never drawn into
#   - a two-tone frame           the renderer produced geometry but no scene
#   - a file of ASCII text       screencap wrote an error message; the shell redirect made the file
#   - the wrong geometry         a capture from a different device than the one being claimed
#
#   bash port/validation/test_pngsane.sh          # no emulator, no device, no network
set +e
cd "$(dirname "$0")" || exit 1
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
PASS=0; FAIL=0

# Synthetic PNGs, written with zlib+struct only — the same dependency floor as win_detect.py, so
# this runs anywhere that one does (the emulator image's python has no PIL, and no shutil either).
gen(){ # gen <out> <w> <h> <mode>
python3 - "$@" <<'PY'
import sys, zlib, struct
out, w, h, mode = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
rows = bytearray()
for y in range(h):
    rows.append(0)                       # filter: none
    for x in range(w):
        if   mode == 'black':  px = (0, 0, 0)
        elif mode == 'white':  px = (255, 255, 255)
        elif mode == 'checker':px = (0, 0, 0) if (x // 8 + y // 8) % 2 else (255, 255, 255)
        else:                  px = ((x * 7) % 256, (y * 5) % 256, (x * y) % 256)   # 'busy'
        rows += bytes(px)
def chunk(t, d):
    c = struct.pack('>I', len(d)) + t + d
    return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = (b'\x89PNG\r\n\x1a\n'
       + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
       + chunk(b'IDAT', zlib.compress(bytes(rows)))
       + chunk(b'IEND', b''))
open(out, 'wb').write(png)
PY
}

check(){ # check <name> <want pass|fail> <expected text> <args...>
    local name="$1" want="$2" text="$3"; shift 3
    local out rc ok=1
    out=$(python3 png_sane.py "$@" 2>&1); rc=$?
    case "$want" in
        pass) [ "$rc" -eq 0 ] || ok=0 ;;
        fail) [ "$rc" -ne 0 ] || ok=0 ;;
    esac
    [ -n "$text" ] && ! printf '%s\n' "$out" | grep -qF "$text" && ok=0
    if [ "$ok" = 1 ]; then echo "  [ OK ] $name"; PASS=$((PASS+1))
    else echo "  [FAIL] $name (rc=$rc, wanted $want${text:+ + \"$text\"})"
         printf '%s\n' "$out" | sed 's/^/          | /'; FAIL=$((FAIL+1)); fi
}

gen "$TMP/black.png"   640 320 black
gen "$TMP/white.png"   640 320 white
gen "$TMP/checker.png" 640 320 checker
gen "$TMP/busy.png"    640 320 busy
printf 'Killed\nerror: device offline\n' > "$TMP/ascii.png"      # 33 bytes of text named .png

echo "== frames a broken run produces must be rejected =="
check "a solid black frame"          fail "blank/solid frame"     "$TMP/black.png"
check "a solid white frame"          fail "blank/solid frame"     "$TMP/white.png"
check "a two-tone frame"             fail "distinct colours"      "$TMP/checker.png"
check "an error message named .png"  fail "cannot decode"         "$TMP/ascii.png"
check "a file that is not there"     fail "cannot decode"         "$TMP/nope.png"

echo
echo "== real frames must be accepted =="
check "a synthetic busy frame"       pass ""                      "$TMP/busy.png"
check "a real gameplay capture"      pass ""                      ../../reports/shots/PROOF_3_bird_launched.png
check "a real win capture (dimmest)" pass ""                      ../../reports/shots/PROOF_6_levelend_survived.png
check "the sparsest real capture"    pass ""                      ../../reports/shots/PROOF_20_premise_same_device.png
check "every proof at once"          pass ""                      ../../reports/shots/PROOF_2_interactive_level.png \
                                                                   ../../reports/shots/PROOF_4_debris_settle.png \
                                                                   ../../reports/shots/PROOF_7_level2_progression.png

echo
echo "== geometry =="
check "wrong geometry is caught"     fail "expected 999x999"      --expect 999x999 "$TMP/busy.png"
check "right geometry passes"        pass ""                      --expect 640x320 "$TMP/busy.png"
# --expect with nothing after it used to raise StopIteration and print a traceback. It exited 1, so
# it failed closed, but the message told the reader nothing.
check "--expect with no value errors" fail "needs a value"          "$TMP/busy.png" --expect

echo
echo "== one bad frame among good ones must still fail the batch =="
check "a batch containing a blank"   fail "blank/solid frame"     "$TMP/busy.png" "$TMP/black.png"

echo
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ] && echo "ALL PNG-SANITY CASES BEHAVE" || echo "PNG-SANITY SELF-TEST FAILED"
exit "$FAIL"
