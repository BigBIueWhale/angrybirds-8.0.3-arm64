# lib_metrics.sh — report emulator metrics without letting "nothing was measured" read as "clean".
#
# WHY THIS EXISTS
# ---------------
# `h_fatal` is the metric this project cites more than any other: it counts `[h_fatal]` lines in the
# shim's log, and 0 means the shim never hit a fatal path. Every script reported it as
#
#     h_fatal:  0   (0 = no crash)
#
# computed with `grep -ac '\[h_fatal\]' "$ABLOG"`. If the app never started, or logcat did not
# attach, or the tag filter matched nothing, that grep also returns 0 — and the line then claims a
# clean run on the strength of an empty file. The same shape has now been found four times in this
# project (a check printing [FAIL] and exiting 0; a layer-4 test passing while the app had been
# killed 11 ms in; three counting checks passing on unreadable inputs; and this).
#
# So the log's own size is checked first, and the metric refuses to report a number it cannot back:
#
#     h_fatal:  0   (0 = no crash; 41207 shim log lines, so this was measured)
#     h_fatal:  NOT MEASURED — the shim log is empty, so 0 here would mean nothing
#
# Usage:
#     source "$(dirname "$0")/lib_metrics.sh"
#     say "  h_fatal:  $(h_fatal_report "$ABLOG")"

# Print an h_fatal count, or an explicit refusal if the log is empty.
h_fatal_report() {
    local log="$1"
    if [ ! -s "$log" ]; then
        printf 'NOT MEASURED — the shim log is empty, so 0 here would mean nothing'
        return 0
    fi
    printf '%s  (0 = no crash; %s shim log lines, so this was measured)' \
        "$(grep -ac '\[h_fatal\]' "$log" 2>/dev/null)" "$(wc -l < "$log")"
}

# Same idea for any other counted marker: a zero is only meaningful if the log exists.
# Uses -E so both plain and extended patterns work through one helper.
marker_report() {
    local log="$1" pat="$2"
    if [ ! -s "$log" ]; then printf 'NOT MEASURED (empty log)'; return 0; fi
    printf '%s' "$(grep -acE "$pat" "$log" 2>/dev/null)"
}

# ---------------------------------------------------------------------------------------------
# absorbed_report / assert_no_absorbed_faults — the companion to h_fatal, and the reason h_fatal
# alone is not a health signal (R41 in port/OPEN_FINDINGS.md).
#
# jni_entry.c's UC_MEM_*_UNMAPPED handler maps ANY unmapped data address to a fresh zero page and
# lets the guest continue. That is deliberate — it is why the game survives the residual std::string
# UAF instead of dying at level end — but it means a whole class of memory faults is neutralised
# BEFORE it can become fatal. So `h_fatal == 0` is compatible with sustained corruption, and eleven
# scripts here were treating it as proof of a clean run.
#
# Two limits the caller must not misread, both of them in the shim:
#   * only the FIRST 12 occurrences are logged (`if(nn++<12)`), and the internal page counter is
#     never reported — so the number below is a FLOOR, not a count.
#   * address 0 is mapped like any other (`pg = addr & ~0xFFFu`), so even a NULL dereference lands
#     here rather than faulting.
#
# Baseline is 0 across every log in this repo, including the 20-minute soak (frame[21601]) and the
# deep progression run (frame[26401]). It was NOT always 0: emu_fatalR_abshim.txt from 2026-07-26,
# before the level-end and galloc fixes, has three — with h_fatal 0 on the same run. So any
# occurrence is a regression worth stopping for.
absorbed_report() {            # $1 = abshim log
    local n; n=$(grep -ac 'uaf-survive' "$1" 2>/dev/null); echo "${n:-0}"
}

# Prints one [ OK ] / [FAIL] line; returns 0 when clean, 1 otherwise, so callers can do
#   assert_no_absorbed_faults "$ABLOG" || FAIL=1
assert_no_absorbed_faults() {  # $1 = abshim log
    local n; n=$(absorbed_report "$1")
    local out
    if [ "$n" -eq 0 ]; then
        out="  [ OK ] no wild memory access was papered over (so h_fatal=0 means something here)"
    else
        out="  [FAIL] $n wild memory access(es) absorbed into fresh zero pages — the run SURVIVES
         these by design and h_fatal stays 0, which is exactly why this is separate. The shim
         logs only the first 12, so $n is a FLOOR, not the count. Baseline everywhere is 0."
    fi
    if declare -F say >/dev/null 2>&1; then say "$out"; elif [ -n "$LOG" ]; then echo "$out" | tee -a "$LOG"; else echo "$out"; fi
    [ "$n" -eq 0 ]
}

# ---------------------------------------------------------------------------------------------
# waf_report — write-after-free events, REPORTED and never asserted to be zero. See R42.
#
# The engine genuinely writes to blocks it has already freed: 41 events in the 2026-07-29 API-25
# diagnostic playthrough, from three distinct engine free-sites (+0xd4c40 x33, +0x7c2cb4 x7,
# +0x7363a8 x1). The canary deltas (0x7->0x197, 0x5f->0x1cf, 0x55->0x1c5) are a near-constant
# increment to the FIRST word of a freed block — a COW std::string _Rep refcount, i.e. the documented
# residual UAF, not a new defect.
#
# galloc mitigates it with a targeted leak: a block written while quarantined never has its address
# reclaimed, so the stale refcount write lands on memory nobody owns. That FIX is always active,
# including in release. It is bounded in practice — the 20-minute soak grew RSS 613960kB -> 620732kB,
# 101% of the first sample, across frame[21601].
#
# WHY THIS IS NOT ASSERTED == 0, which would be the obvious thing to write:
#   * On a RELEASE build the count is ALWAYS 0 because the [WAF] log is compiled out
#     (`#if defined(__ANDROID__) && !defined(ABSHIM_RELEASE)` in galloc.c) — verified by grepping the
#     shipped .so: it contains "uaf-survive" but no "[WAF]". So `waf == 0` on a release log means
#     "the diagnostic does not exist here", not "no write-after-free happened". Asserting it would be
#     a check that cannot fail, on the majority of runs in this suite.
#   * On a DIAGNOSTIC build a non-zero count is EXPECTED and accepted. Failing on it would paint
#     every diagnostic run red for a condition that is deliberately tolerated and bounded.
#   * The log itself caps at 64 (`n++<64`), so any count of exactly 64 is a FLOOR, not a total.
#     emu_fatal_abshim.txt hit that cap; playthrough_abshim.txt's 41 did not, so 41 is real.
#
# What IS useful is the number, next to the build that produced it, so a change in rate is visible.
waf_report() {                 # $1 = abshim log
    local n; n=$(grep -ac '\[WAF\]' "$1" 2>/dev/null); n=${n:-0}
    if   [ "$n" -eq 0 ];  then echo "0 (either a release build, where the [WAF] diagnostic is compiled out, or none occurred — these are indistinguishable from a log)"
    elif [ "$n" -ge 64 ]; then echo "$n — AT THE LOG CAP (n++<64), so this is a FLOOR, not a count"
    else echo "$n write-after-free event(s), absorbed by galloc's targeted leak (expected on a diagnostic build; baseline 41 for an API-25 playthrough)"
    fi
}

# ---------------------------------------------------------------------------------------------
# saturated_report — name this run's own saturated counters, so no number in its log can later be
# quoted as a total by mistake. This is the standing fix for R42/R43: twice a log cap was written
# into the record as a measurement ("bounded ~64 tiny _Reps", "the mixer fills ~8 buffers"), and both
# times the only way to tell was to go and read the guard in the shim source. Now every run says it.
#
# Report-only. Saturation is not a fault — [S2] do_call saturating at 24 is normal tracing — it just
# means that particular number is a FLOOR. Printing which ones they are costs nothing and removes the
# need for anyone to remember.
saturated_report() {           # $1 = abshim log
    local py; py="$(dirname "${BASH_SOURCE[0]}")/capped_counts.py"
    [ -f "$py" ] && command -v python3 >/dev/null 2>&1 || { echo "not checked (capped_counts.py or python3 unavailable)"; return 0; }
    local out floors extra
    out=$(python3 "$py" "$1" 2>/dev/null)
    floors=$(printf '%s\n' "$out" | grep '\[FLOOR\]' | grep -oE "'\[[a-zA-Z0-9_ -]+\][^']*'" | tr -d "'" | sort -u)
    # BASELINE. These five sites saturate in EVERY run ever recorded here — diagnostic and release,
    # API 25 through 36, a 2-minute capture and a 20-minute soak — because they are early-boot and
    # scheduler tracing that fills up in the first moments regardless of what the run then does.
    # Verified identical across playthrough_abshim.txt, modplay_abshim.txt and save_ab2.txt.
    # Reporting them every time would be noise, and a report that is mostly noise gets ignored, which
    # is how a real one gets missed. So only DEVIATIONS are called out.
    extra=$(printf '%s\n' "$floors" | grep -vE '^\[audio-isolate\]|^\[S2\] do_call ENTER|^\[S2\] do_call RET|^\[S2\] stash|^\[u16conv\] src')
    extra=$(printf '%s' "$extra" | grep -c .)
    if [ "${extra:-0}" -eq 0 ]; then
        echo "only the 5 always-saturated tracing sites (early-boot/scheduler); every other count in this log is a real total"
    else
        echo "$extra counter(s) BEYOND the always-saturated baseline are floors: $(printf '%s\n' "$floors" | grep -vE '^\[audio-isolate\]|^\[S2\] do_call|^\[S2\] stash|^\[u16conv\] src' | tr '\n' ' ')"
    fi
}
