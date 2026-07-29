#!/bin/bash
# lib_saveassert.sh — the save-persistence verdict, as a function that can be tested without a device.
#
# WHY THIS IS A LIB AND NOT FIVE LINES INSIDE emu_save_test.sh
# ------------------------------------------------------------
# emu_save_test.sh used to end like this:
#
#     say "  1st-launch saves written:  $NF private files"
#     say "  2nd-launch boot: init_array=... last-frame=... h_fatal=..."
#     say "  2nd-launch pid:  [...] (alive => reloaded OK, no crash)"
#     say DONE
#
# Four numbers displayed, none checked, and a bare DONE rather than DONE (FAIL=n). If saves had
# stopped persisting the script would have printed different numbers, exited 0, and read as a pass.
# Save persistence is a documented user-facing promise ("your progress is still there next time"),
# so its test has to be capable of failing.
#
# It lives here, apart from the emulator driver, for a second reason: an assertion that only ever
# runs inside a 20-minute emulator job is an assertion nobody can prove works. Everything below
# derives its verdict from four plain arguments, so test_saveassert.sh can feed it synthetic
# evidence offline and prove each check fires on the failure it names — in about a second.
#
# WHAT IS DELIBERATELY *NOT* ASSERTED
#   - a win. The driver's play sequence cannot confirm one (no log marker exists for it), so
#     highscores.lua is asserted to EXIST, not to be non-empty: a no-win run may legitimately
#     leave it empty, and an assertion that flakes on legitimate behaviour teaches people to
#     ignore the suite.
#   - byte-identity of the saves across the relaunch. The game reloads AND rewrites them, so
#     identical bytes are not expected here. emu_reboot_persist.sh asserts byte-identity, which
#     is valid there because the app is not relaunched between the two hashes.

# Prefer the caller's say() (it tees into the run log) and fall back to echo when tested offline.
sa_say(){ if type say >/dev/null 2>&1; then say "$@"; else echo "$@"; fi; }

# Size of one file out of `wc -c` output. index() is a literal substring test, not a regex, so a
# dot in the filename cannot match some other character, and wc's trailing "total" line cannot
# be mistaken for a file.
sa_size(){ printf '%s\n' "$2" | awk -v f="$1" 'index($NF, f) {print $1; exit}'; }

# assert_save_persistence <n_private_files> <2nd-launch abshim log> <2nd-launch pid> <wc -c output>
# Prints one [ OK ] or [FAIL] line per check; returns the number of failures (0 = pass).
assert_save_persistence() {
    local nf="$1" ab2="$2" pid2="$3" saves="$4" fail=0
    local hf fr ctor rd s_set s_high

    hf=$(grep -ac 'h_fatal' "$ab2" 2>/dev/null); hf=${hf:-0}
    fr=$(grep -aoE 'frame\[[0-9]+\]' "$ab2" 2>/dev/null | tail -1 | grep -oE '[0-9]+')
    ctor=$(grep -acE 'init_array 125/125' "$ab2" 2>/dev/null); ctor=${ctor:-0}
    rd=$(grep -acE 'fopen.*com\.rovio\.angrybirds|AAsset.*profile|fopen.*(save|profile|progress|settings|\.lua)' \
         "$ab2" 2>/dev/null); rd=${rd:-0}
    s_set=$(sa_size 'settings.lua' "$saves")
    s_high=$(sa_size 'highscores.lua' "$saves")

    # 1. Something was written at all. Without this the rest would be asserting the persistence of
    #    nothing, which passes trivially and means nothing.
    if [ "${nf:-0}" -gt 0 ] 2>/dev/null; then
        sa_say "  [ OK ] the first launch wrote $nf private file(s)"
    else
        sa_say "  [FAIL] no private files were written, so there is nothing whose persistence could be tested"
        fail=$((fail+1))
    fi

    # 2/3. The two files the documentation names by name, after the relaunch.
    if [ -n "$s_set" ] && [ "$s_set" -gt 0 ] 2>/dev/null; then
        sa_say "  [ OK ] settings.lua survived the relaunch ($s_set bytes)"
    else
        sa_say "  [FAIL] settings.lua is missing or empty after the relaunch (size='${s_set:-absent}')"
        fail=$((fail+1))
    fi
    if [ -n "$s_high" ]; then
        sa_say "  [ OK ] highscores.lua survived the relaunch ($s_high bytes)"
    else
        sa_say "  [FAIL] highscores.lua is absent after the relaunch"
        fail=$((fail+1))
    fi

    # 4. The relaunched process exists. A dead process reloads nothing.
    if [ -n "$pid2" ]; then
        sa_say "  [ OK ] the relaunched process is alive (pid $pid2)"
    else
        sa_say "  [FAIL] the app is not running after the relaunch"
        fail=$((fail+1))
    fi

    # 5. It got all the way through its constructors on the SECOND launch, with saved state on disk.
    if [ "$ctor" -gt 0 ]; then
        sa_say "  [ OK ] all 125 constructors ran on the second launch"
    else
        sa_say "  [FAIL] the second launch never reached init_array 125/125"
        fail=$((fail+1))
    fi

    # 6. It rendered. 200 is the driver's own wait threshold; asserting more than the driver waits
    #    for would fail on a slow run rather than on a broken one.
    if [ -n "$fr" ] && [ "$fr" -ge 200 ] 2>/dev/null; then
        sa_say "  [ OK ] the second launch rendered to frame[$fr]"
    else
        sa_say "  [FAIL] the second launch did not render (last frame '${fr:-none}')"
        fail=$((fail+1))
    fi

    # 7. It read its own files back — the difference between "the files are still on disk" and
    #    "the game picked them up".
    if [ "$rd" -gt 0 ]; then
        sa_say "  [ OK ] the second launch read its own files back ($rd reads)"
    else
        sa_say "  [FAIL] the second launch read none of its own files — it did not reload the saves"
        fail=$((fail+1))
    fi

    # 8. No fatal in the shim across the reload path.
    if [ "$hf" -eq 0 ]; then
        sa_say "  [ OK ] no h_fatal on the second launch"
    else
        sa_say "  [FAIL] h_fatal on the second launch ($hf occurrence(s))"
        fail=$((fail+1))
    fi

    return "$fail"
}
