#!/bin/bash
# lib_playassert.sh — the playthrough verdict, as a function that can be tested without a device.
#
# WHY THIS EXISTS
# ---------------
# Three scripts — emu_modern_playthrough.sh, emu_modern_progress.sh, emu_audio_modern.sh — all ended
# the same way:
#
#     say "  h_fatal:           0"
#     say "  last frame:        frame[1501]"
#     say "  final pid:         [3782]  (alive => played through)"
#     say DONE
#
# Values printed, none checked, no exit status derived from any of them, and a parenthetical stating
# the conclusion the script never tested. `win_check()` was even CALLED — and its carefully designed
# three-way return (0 win / 1 not a win / 2 could not check) was thrown away, so a run that
# photographed a level still in progress reported exactly what a winning run reported.
#
# These are not minor scripts. They produce PROOF_8, PROOF_11 and PROOF_14 — the evidence for "the
# game plays through to a win on modern Android", which is the project's headline claim.
#
# COULD NOT CHECK IS NOT A PASS
# -----------------------------
# rc=2 from win_check means no verdict was reached. A playthrough test that reached no verdict has
# not demonstrated a playthrough, so it fails — while saying plainly which of the two happened, since
# "the detector could not run" and "the game did not win" call for completely different responses.
# That distinction is lib_wincheck.sh's whole reason for existing; this preserves it instead of
# collapsing both into a red line.
#
# A FAILING WIN CHECK IS NOT AUTOMATICALLY A PORT BUG. These runs are timing-sensitive: the settle
# is frame-based (lib_settle.sh) precisely because a wall-clock wait once screenshotted a level still
# in progress on a slow host. So the failure message points at the screenshot rather than accusing
# the shim. But it still FAILS: a run that did not demonstrate a win must not report success.

# Pull in win_check rather than assuming the caller did. Depending on a sibling lib being sourced
# first is a real fragility — a caller that forgets gets "win_check: command not found", rc=127,
# which this reads as "could not check" and fails on. It fails closed, but for the wrong reason and
# with a message that sends the reader hunting the emulator instead of the missing source line.
# (prose_as_code.py flagged the call for exactly this: it could not see where win_check came from.)
source "$(dirname "${BASH_SOURCE[0]}")/lib_wincheck.sh"
source "$(dirname "${BASH_SOURCE[0]}")/lib_metrics.sh"   # absorbed_report / assert_no_absorbed_faults

# Same three-branch fallback as lib_wincheck.sh's _wc_say, and for the same reason: two scripts
# (emu_playthrough.sh, emu_playthrough_release.sh) log with inline `echo ... | tee -a "$LOG"` and
# define no say() at all. With only a say/echo fallback, every assertion line here would print to
# the terminal and never reach the run's log — so the saved evidence would omit the verdict while
# the live output looked complete.
sa_pa_say(){
    if declare -F say >/dev/null 2>&1; then say "$@"
    elif [ -n "$LOG" ]; then echo "$@" | tee -a "$LOG"
    else echo "$@"; fi
}

# assert_playthrough <abshim log> <end screenshot> <final pid> [min frame, default 601]
# Prints one [ OK ] / [FAIL] line per check; returns the number of failures (0 = pass).
assert_playthrough() {
    local ablog="$1" shot="$2" pid="$3" minfr="${4:-601}" fail=0
    local ctor fr hf wc

    # If the caller's run-log IS the log under analysis, every verdict line this function prints
    # gets appended to its own input — and a line reading "[FAIL] h_fatal during the playthrough"
    # then counts as an h_fatal. That is measuring your own output. It happened, in this lib's own
    # self-test, within minutes of the tee fallback being added.
    if [ -n "$LOG" ] && [ -e "$LOG" ] && [ -e "$ablog" ] && [ "$LOG" -ef "$ablog" ]; then
        sa_pa_say "  [FAIL] harness error: the run log and the abshim log are the same file"
        sa_pa_say "         ($ablog) — the assertions would be reading their own output. Fix the caller."
        return 1
    fi

    ctor=$(grep -acE 'init_array 125/125' "$ablog" 2>/dev/null); ctor=${ctor:-0}
    fr=$(grep -aoE 'frame\[[0-9]+\]' "$ablog" 2>/dev/null | tail -1 | grep -oE '[0-9]+')
    hf=$(grep -ac 'h_fatal' "$ablog" 2>/dev/null); hf=${hf:-0}
    # WILD MEMORY ACCESSES THAT NEVER BECOME FATAL. jni_entry.c's UC_MEM_*_UNMAPPED handler maps ANY
    # unmapped data address to a fresh zero page and continues — deliberately, to paper over a
    # residual std::string UAF so the game keeps playing. `pg = addr & ~0xFFF`, so even a NULL
    # dereference is absorbed. That means `h_fatal == 0` is NOT evidence of a healthy address space:
    # a whole class of memory faults is neutralised before it can ever be fatal, and this suite was
    # treating the absence of the symptom as health.
    # Today's baseline is ZERO across every real playthrough log, so any occurrence is a regression.

    if [ "$ctor" -gt 0 ]; then
        sa_pa_say "  [ OK ] all 125 constructors ran"
    else
        sa_pa_say "  [FAIL] never reached init_array 125/125 — the engine did not finish loading"
        fail=$((fail+1))
    fi

    if [ -n "$fr" ] && [ "$fr" -ge "$minfr" ] 2>/dev/null; then
        sa_pa_say "  [ OK ] rendered to frame[$fr] (>= $minfr)"
    else
        sa_pa_say "  [FAIL] only reached frame '${fr:-none}', below the $minfr this test requires"
        fail=$((fail+1))
    fi

    if [ "$hf" -eq 0 ]; then
        sa_pa_say "  [ OK ] no h_fatal during the playthrough"
    else
        sa_pa_say "  [FAIL] h_fatal during the playthrough ($hf occurrence(s))"
        fail=$((fail+1))
    fi

    # ONE implementation, in lib_metrics.sh — this file had its own copy for about an hour,
    # which is how a fix ends up drifting between two places.
    assert_no_absorbed_faults "$ablog" || fail=$((fail+1))
    # REPORTED, not asserted — see waf_report in lib_metrics.sh (R42). A release log always reads 0
    # because the diagnostic is compiled out, so asserting 0 here would be a check that cannot fail.
    sa_pa_say "  [note] write-after-free: $(waf_report "$ablog")"
    # Which of THIS run's numbers are floors rather than totals. R42/R43 were both a log cap quoted
    # as a measurement; stating it per-run means nobody has to go and read the guard in the source.
    sa_pa_say "  [note] saturated counters: $(saturated_report "$ablog")"

    if [ -n "$pid" ]; then
        sa_pa_say "  [ OK ] the process was still alive at the end (pid $pid)"
    else
        sa_pa_say "  [FAIL] the process was gone by the end of the run"
        fail=$((fail+1))
    fi

    # The verdict this whole script exists for. win_check prints its own reasoning.
    win_check "$shot"; wc=$?
    case "$wc" in
        0) sa_pa_say "  [ OK ] the end screen is a win, scored from pixels" ;;
        1) sa_pa_say "  [FAIL] the end screen is NOT a win. Look at $shot before blaming the port:"
           sa_pa_say "         a level still in progress means the settle ended early on a slow host,"
           sa_pa_say "         which is a harness timing miss; a crashed or black frame is not."
           fail=$((fail+1)) ;;
        *) sa_pa_say "  [FAIL] the win check could not run, so this run proves nothing either way."
           sa_pa_say "         That is a broken harness, NOT a statement about the game — fix the"
           sa_pa_say "         check (missing python3, missing/empty screenshot) and re-run."
           fail=$((fail+1)) ;;
    esac

    return "$fail"
}

# assert_progression <abshim log> <cleared shot> <next-level shot> <final pid> [min frame]
# For emu_modern_progress.sh, which has no win_check at all: it printed
#     win/advance check: SCREENSHOTS ONLY -> modprog_1_cleared.png (win) + modprog_2_level2.png
# and left both images entirely unexamined, then said DONE.
#
# Advancing is a TWO-sided claim, so both sides are asserted:
#   the cleared shot IS a win        — the level was actually completed
#   the next shot is NOT a win       — the game left the results screen instead of sitting on it
#
# That second assertion has an obvious hole: a crashed app renders a black frame, which is also
# "not a win", so a crash would satisfy it. png_sane.py closes it — the next-level shot must be a
# real, non-blank frame of the right geometry as well as not-a-win.
assert_progression() {
    local ablog="$1" cleared="$2" nextshot="$3" pid="$4" minfr="${5:-601}" fail=0
    local wc sane
    local sanepy; sanepy="$(dirname "${BASH_SOURCE[0]}")/png_sane.py"

    assert_playthrough "$ablog" "$cleared" "$pid" "$minfr"; fail=$?

    sane=$(python3 "$sanepy" "$nextshot" 2>&1); [ $? -eq 0 ] || {
        sa_pa_say "  [FAIL] the next-level capture is not a picture of anything: $sane"
        sa_pa_say "         (asserted BEFORE 'not a win' below, because a crashed black frame would"
        sa_pa_say "          satisfy 'not a win' perfectly well)"
        fail=$((fail+1)); }

    win_check "$nextshot" >/dev/null 2>&1; wc=$?
    case "$wc" in
        1) sa_pa_say "  [ OK ] the next capture is a fresh level, not the results screen — it advanced" ;;
        0) sa_pa_say "  [FAIL] the next capture is STILL a win screen — the game never left the results"
           sa_pa_say "         screen, so nothing was demonstrated about progression"
           fail=$((fail+1)) ;;
        *) sa_pa_say "  [FAIL] could not score the next-level capture, so advancement is unproven"
           fail=$((fail+1)) ;;
    esac
    return "$fail"
}
