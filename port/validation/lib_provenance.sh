# lib_provenance.sh — record WHICH BUILD a screenshot was captured on, at capture time.
#
# WHY THIS EXISTS
# ---------------
# Screenshots go stale silently. It has happened twice:
#   - PROOF_2/3/4 sat for a day showing a binary that no longer existed, because nothing recorded
#     which build produced them and no script could regenerate them.
#   - PROOF_10 (the audio variant's only evidence) predated a session of shim changes. Worse, a
#     bulk docs sweep had updated the hash printed NEXT TO it, so the index looked current while
#     the image was not — the bookkeeping actively hid the staleness.
#
# The lesson from both: provenance written by hand, after the fact, is provenance that drifts. So
# the capture itself records it. Each run appends the sha256 of the APK it actually installed to
# reports/shots/provenance.tsv, and verify_claims.sh compares those against the current builds and
# names any proof whose build no longer exists.
#
# Usage, right after a successful install:
#     source "$(dirname "$0")/lib_provenance.sh"
#     record_build "$APK" "modplay"          # label = the output prefix this script writes
#
# The label ties a row to the screenshots that run produced, so a stale row points at specific
# files rather than at "something, somewhere".
#
# Each row also records THE SCRIPT THAT WROTE IT and the AVD it ran on, because the label identifies
# neither. Chasing a stale `interactive` row cost 13 minutes of emulator time running
# emu_interactive.sh — which never records provenance at all; the row comes from
# emu_interactive_capture.sh.
#
# The AVD is recorded because the script alone is an ACTIVELY MISLEADING instruction. `modplay` and
# `modplay36` are the same script — emu_modern_playthrough.sh — separated only by ABSHIM_AVD and
# ABSHIM_OUTPFX. Re-running it bare to refresh `modplay36` would regenerate `modplay` instead,
# overwriting a good row and leaving the stale one untouched. A hint that sends you to damage a
# different row is worse than no hint, so the row carries what is needed to reproduce it exactly.
# Older 3- and 4-column rows stay readable: consumers treat the extra fields as optional.

# ROW SHAPES IN provenance.tsv. Newer rows carry five tab-separated fields:
#     label <TAB> sha256(apk) <TAB> apk-basename <TAB> capturing-script <TAB> AVD
# Older rows carry only the first three — they predate the script and AVD columns and are NOT
# truncated or corrupt. The gate's provenance checks read fields 1 and 2, so both shapes are valid
# input; the extra columns are context for a human reading the ledger. Legacy rows are deliberately
# left alone rather than backfilled: the script and AVD for a historical capture would have to be
# inferred from the README map, and inventing provenance is worse than admitting it is absent.
record_build() {
    local apk="$1" label="$2"
    local tsv="${OUT:-/work/reports/shots}/provenance.tsv"
    [ -f "$apk" ] || { echo "  [provenance] no APK at $apk - not recorded" >&2; return 1; }
    local sha; sha=$(sha256sum "$apk" 2>/dev/null | cut -d' ' -f1)
    [ -n "$sha" ] || return 1
    mkdir -p "$(dirname "$tsv")"
    # One row per label: replace any previous row so the file reflects the LATEST capture, which is
    # what "is this proof current" asks about. Without this the file would grow a history and the
    # check would have to guess which row is authoritative.
    #
    # UPDATED IN PLACE, not delete-then-append. Appending moved the row to the end of the file on
    # every re-run, so re-capturing a proof produced a git diff of one deleted and one added line
    # with BYTE-IDENTICAL content — pure reordering noise. A ledger whose diffs are mostly noise is
    # one nobody reads, and a real hash change would sit in the middle of that churn looking the
    # same. Now a diff of provenance.tsv means a capture actually changed.
    # The AVD is ASKED OF THE RUNNING EMULATOR, not taken from $ABSHIM_AVD alone. Scripts that pick
    # their AVD internally (emu_audio_modern.sh defaults to abtest34, emu_16k_pagesize.sh to ab16k)
    # never export that variable, so the column came out EMPTY for them — 11 of 13 rows carried no
    # environment at all. Provenance exists so a capture cannot drift from the build AND the
    # environment that produced it; half a row only does half that job. `adb emu avd name` answers
    # from the emulator itself, so it is right even when nobody set the variable, and it stays right
    # if a caller sets the variable to something it did not actually launch.
    local avd="${ABSHIM_AVD:-}"
    if [ -z "$avd" ]; then
        avd=$(adb emu avd name 2>/dev/null | head -1 | tr -d '\r')
        case "$avd" in *OK*|*error*|*unknown*) avd="" ;; esac
    fi
    local row
    row=$(printf '%s\t%s\t%s\t%s\t%s' "$label" "$sha" "$(basename "$apk")" \
          "$(basename "${BASH_SOURCE[1]:-$0}")" "${avd:-<not-recorded>}")
    if [ -f "$tsv" ] && grep -q "^${label}	" "$tsv"; then
        awk -v lbl="$label" -v new="$row" -F'\t' \
            '$1==lbl {print new; next} {print}' "$tsv" > "$tsv.tmp" && mv "$tsv.tmp" "$tsv"
    else
        printf '%s\n' "$row" >> "$tsv"
    fi
    echo "  [provenance] $label <- $(basename "$apk") ${sha:0:12}… (by $(basename "${BASH_SOURCE[1]:-$0}"))"
}
