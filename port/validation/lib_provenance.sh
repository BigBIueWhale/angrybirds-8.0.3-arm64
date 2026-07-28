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
    if [ -f "$tsv" ]; then
        grep -v "^${label}	" "$tsv" > "$tsv.tmp" 2>/dev/null || true
        mv "$tsv.tmp" "$tsv"
    fi
    printf '%s\t%s\t%s\n' "$label" "$sha" "$(basename "$apk")" >> "$tsv"
    echo "  [provenance] $label <- $(basename "$apk") ${sha:0:12}…"
}
