#!/bin/bash
# test_prose.sh — prove prose_as_code.py catches executable prose and does not cry wolf.
#
# The detector exists because emu_layer4_fcm_test.sh carried three lines of explanation that had
# lost their `#` and ran as commands on every invocation, invisibly to `bash -n`. A detector for
# that is only useful if it (a) fires on the real thing and (b) stays silent on 71 files of real,
# heavily-commented shell — a checker that flags valid code gets switched off within a week, which
# is the same outcome as not having it.
#
#   bash port/validation/test_prose.sh          # no emulator, no device, no network
set +e
cd "$(dirname "$0")" || exit 1
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
PASS=0; FAIL=0

check(){ # check <name> <want pass|fail> <expected text> <args...>
    local name="$1" want="$2" text="$3"; shift 3
    local out rc ok=1
    out=$(python3 prose_as_code.py "$@" 2>&1); rc=$?
    case "$want" in
        pass) [ "$rc" -eq 0 ] || ok=0 ;;
        fail) [ "$rc" -ne 0 ] || ok=0 ;;
    esac
    [ -n "$text" ] && ! printf '%s\n' "$out" | grep -qF "$text" && ok=0
    if [ "$ok" = 1 ]; then echo "  [ OK ] $name"; PASS=$((PASS+1))
    else echo "  [FAIL] $name (rc=$rc, wanted $want${text:+ + \"$text\"})"
         printf '%s\n' "$out" | sed 's/^/          | /'; FAIL=$((FAIL+1)); fi
}

# --- the defect, exactly as it was found in the tree -------------------------------------------
cat > "$TMP/real.sh" <<'EOF'
#!/bin/bash
# Refuse anything that is not an integer. Every failure path emits -1, but if a future
one forgets, CN/SN would hold a message string and the comparisons below would be decided by a
bash "integer expression expected" error — printing a confident verdict.
echo verdict
EOF

# --- the variant that would silently truncate a test file ---------------------------------------
cat > "$TMP/exit.sh" <<'EOF'
#!/bin/bash
# this explanation wraps onto a second line and says the script would
exit early, so every check below it would never run at all
echo "this check would never be reached"
EOF

# --- clean shell that uses the idioms which fooled earlier versions -----------------------------
cat > "$TMP/clean.sh" <<'EOF'
#!/bin/bash
# A perfectly ordinary script with a long comment that wraps across
# several lines and mentions words like exit, return and local.
set -e
command -v docker >/dev/null 2>&1 || echo "no docker"
export ABSHIM_ENGINE_SO="$(cd "$HOME/x" && pwd)/lib.so"
say(){ echo "$@"; }
say hello
local_looking_var=3
exit "$local_looking_var"
EOF

# --- prose inside data, which must NOT be flagged -----------------------------------------------
cat > "$TMP/data.sh" <<'OUTER'
#!/bin/bash
# a comment directly above a heredoc whose body is English
cat <<'INNER'
one forgets that this is data, not code
exit early is also fine in here
INNER
# a comment directly above a multi-line quoted string
echo "this string spans lines and
one forgets it is still a string"
OUTER

# --- a file that is not shell at all (a shader asset that ends in .sh) ---------------------------
printf 'varying highp vec2 V_TexCoord0;\nvarying lowp float V_FogLevel;\n' > "$TMP/shader.sh"

echo "== executable prose must be caught =="
check "the defect as found in the tree" fail "one forgets"   "$TMP/real.sh"
check "a stray line starting with exit" fail "exit early"    "$TMP/exit.sh"

echo
echo "== valid shell must NOT be flagged =="
check "ordinary script, long comments"  pass ""              "$TMP/clean.sh"
check "prose inside heredocs/strings"   pass ""              "$TMP/data.sh"

echo
echo "== non-shell files =="
check "a shader asset is skipped"       pass ""              "$TMP/clean.sh" "$TMP/shader.sh"
# Scanning ONLY non-shell files means nothing was examined. Reporting that as a pass is the exact
# defect class this suite hunts, so it must be an error.
check "scanning nothing is not a pass"  fail "nothing was scanned" "$TMP/shader.sh"

echo
echo "== the real tree is the control: 71 files of heavily-commented shell =="
# If this ever fails, either the tree really did grow executable prose (fix it) or the detector
# became noisy (fix that) — both are worth stopping for.
check "the whole repo is clean"         pass "no prose in command position" \
      $(cd ../.. && find . -name '*.sh' -not -path './.git/*' | sed 's|^\./|../../|' | sort)

echo
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ] && echo "ALL PROSE-DETECTOR CASES BEHAVE" || echo "PROSE-DETECTOR SELF-TEST FAILED"
exit "$FAIL"
