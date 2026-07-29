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
