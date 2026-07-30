#!/bin/bash
# bump_deliverable_hash.sh — retire one documented deliverable hash in favour of another, safely.
#
# WHY THIS IS A SCRIPT AND NOT A SED ONE-LINER
# -------------------------------------------
# The current hash appears 19 times across 6 files, and they are NOT all the same kind of claim:
#
#   CURRENT-STATE  "the build reproduces X"            -> must change
#   HISTORICAL     "reverted; the tree rebuilds to X"  -> must NOT change; rewriting it makes the
#                                                         record of what happened false
#
# This repo's convention is that superseded claims are ANNOTATED, not deleted (that is why
# OPEN_FINDINGS.md still contains 9da84326, 4fdb3d7c, 6fd9c57d, 8ff45e25, 68bf6f4a, f279169a and
# 83be89e5 — every previous deliverable hash, each in its own narrative). A blanket replace would
# silently rewrite that history into a lie, which is worse than leaving the docs stale.
#
# So: only the four files that assert the CURRENT artifact are touched. OPEN_FINDINGS.md is left
# entirely alone and is expected to keep naming the old hash — that is the point of it.
#
#   bash port/tools/bump_deliverable_hash.sh <old-full-sha256> <new-full-sha256>
#
# Then, in the SAME commit: regenerate port/validation/shim_sources.sha256 (this script does it) and
# re-run verify_claims.sh, whose source-drift tripwire exists precisely to catch a half-finished bump.
set -e
OLD="${1:-}"; NEW="${2:-}"
case "$OLD$NEW" in *[!0-9a-f]*|"") echo "usage: $0 <old-sha256> <new-sha256>   (64 hex chars each)" >&2; exit 2;; esac
[ "${#OLD}" = 64 ] && [ "${#NEW}" = 64 ] || { echo "both hashes must be 64 hex chars" >&2; exit 2; }
[ "$OLD" != "$NEW" ] || { echo "old and new are identical — nothing to do" >&2; exit 2; }

cd "$(dirname "$0")/../.."
APK=out/angrybirds-8.0.3-arm64.apk
[ -f "$APK" ] || { echo "  [FAIL] $APK missing — build it before bumping the documented hash" >&2; exit 1; }
ACTUAL=$(sha256sum "$APK" | cut -d' ' -f1)
if [ "$ACTUAL" != "$NEW" ]; then
    echo "  [FAIL] refusing: $APK is $ACTUAL, not the new hash you passed ($NEW)." >&2
    echo "         Bumping the docs to a hash the artifact does not have is the exact inversion of" >&2
    echo "         what these documents are for." >&2
    exit 1
fi
echo "  artifact verified: $APK really is $NEW"

# The four files that assert the CURRENT artifact. OPEN_FINDINGS.md is deliberately absent.
CUR="RELEASE_NOTES.md README.md port/REPRODUCE.md port/ONDEVICE.md"
SHORT_OLD="${OLD:0:16}"
n=0
for f in $CUR; do
    [ -f "$f" ] || continue
    before=$(grep -c "$OLD\|$SHORT_OLD" "$f" 2>/dev/null || true)
    [ "${before:-0}" -eq 0 ] && continue
    python3 - "$f" "$OLD" "$NEW" <<'PYEOF'
import sys
p, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(p, encoding='utf-8').read()
# full form first, then any truncated prefix used for readability in prose
s = s.replace(old, new)
for k in (24, 16, 12, 8):
    s = s.replace(old[:k], new[:k])
open(p, 'w', encoding='utf-8').write(s)
PYEOF
    after=$(grep -c "$OLD\|$SHORT_OLD" "$f" 2>/dev/null || true)
    echo "  $f: $before reference(s) -> ${after:-0} remaining"
    n=$((n+1))
done
echo "  updated $n current-state file(s)"

# The source manifest must move in the SAME commit, or verify_claims.sh's drift tripwire will (rightly)
# report that the source no longer matches the documented artifact.
( cd port/shim/src && sha256sum *.c *.h | grep -v ' jni_thunks.gen.c$' | LC_ALL=C sort -k2 ) \
    > port/validation/shim_sources.sha256
echo "  regenerated port/validation/shim_sources.sha256 ($(grep -c . port/validation/shim_sources.sha256) files)"

echo
echo "  OPEN_FINDINGS.md intentionally untouched — it still names $SHORT_OLD… and every earlier"
echo "  deliverable hash, because those entries describe what was true when written."
echo "  Now run: docker run --rm --network none -v \"\$PWD\":/work -w /work ab-port bash port/validation/verify_claims.sh"
