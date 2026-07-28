#!/bin/bash
# mutation_test.sh — prove the claim gate can actually FAIL, one guarantee at a time.
#
# WHY THIS EXISTS
# ---------------
# verify_claims.sh's header says "Every check in this file has now been deliberately broken at least
# once." On 2026-07-28 that turned out to be false for one of them: the provenance lint had been
# "fixed and negative-tested", but the fix was never applied to the file — the negative test had run
# against a COPY of the intended logic in /tmp, so it proved the idea was right and said nothing
# about the artifact. The check sat there passing vacuously.
#
# A gate nobody has seen fail is a gate nobody knows works. So instead of trusting that sentence,
# this breaks each guarantee for real — in the specific way the check exists to catch — and asserts
# that the check reports it. That is the difference between "we tested it" and "it is tested".
#
# HOW IT WORKS
# ------------
# The repo is copied with `cp -al` (hard links: seconds, near-zero disk for a 2.6 GB tree). A
# mutation NEVER edits in place — it `rm`s the path first, which breaks the link, then writes fresh
# content. Editing a hard-linked file would corrupt the real repo through the shared inode, so the
# rm-then-write order is load-bearing, not stylistic.
#
# Each case then runs the REAL verify_claims.sh against the mutated copy and greps for the specific
# [FAIL] it should produce. Testing the real file is the whole point — see above.
#
# NOTE ON SIGNATURES: any change to APK content invalidates the v2/v3 signatures, so most APK
# mutations also trip the signing check. That is not noise; it is the signing check demonstrating
# it works. Each case asserts only on ITS OWN expected failure.
#
#   bash port/validation/mutation_test.sh              # all cases
#   bash port/validation/mutation_test.sh perf         # one case by name
set +e
cd "$(dirname "$0")/../.." || exit 1
REPO="$PWD"
WORK="${ABSHIM_MUT_DIR:-/tmp/claude-1000/mutation}"
ONLY="$1"
PASS=0; FAILED=0; SKIPPED=0

command -v docker >/dev/null 2>&1 && docker image inspect ab-port >/dev/null 2>&1 || {
    echo "[skip] mutation test needs the ab-port image"; exit 0; }

# Run verify_claims against a tree and echo its output.
run_gate() {
    docker run --rm --network none -v "$1":/work -w /work ab-port \
        bash port/validation/verify_claims.sh 2>&1
}

# case <name> <expected-FAIL-substring> <mutation-fn>
case_run() {
    local name="$1" want="$2" fn="$3"
    [ -n "$ONLY" ] && [ "$ONLY" != "$name" ] && return 0
    printf '  %-22s ' "$name"
    rm -rf "$WORK"; mkdir -p "$(dirname "$WORK")"
    # Hard-link what we can, then backfill the rest. `cp -al` alone is not enough: files created by
    # root inside the containers cannot be hard-linked by this user under fs.protected_hardlinks, so
    # it exits 1 having skipped ~376 of 5407 files. `cp -an` fills those in without clobbering the
    # links. Then VERIFY the tree is complete — a partial copy would make the gate fail for reasons
    # that have nothing to do with the mutation, and that would look exactly like a detection.
    cp -al "$REPO" "$WORK" 2>/dev/null
    cp -an "$REPO/." "$WORK/" 2>/dev/null
    # NOT named `want`: that is this function's second PARAMETER (the expected failure string). The
    # first version declared `local want have` here and clobbered it, so every case compared the
    # gate's output against the file count "5407" and reported NOT DETECTED for checks that had in
    # fact fired correctly. The failure was in the harness, and it read exactly like a broken gate.
    local n_src n_dst; n_src=$(find "$REPO" -type f | wc -l); n_dst=$(find "$WORK" -type f 2>/dev/null | wc -l)
    [ "$n_dst" = "$n_src" ] || { echo "SKIP (copy incomplete: $n_dst/$n_src files)"; SKIPPED=$((SKIPPED+1)); rm -rf "$WORK"; return 0; }
    "$fn" "$WORK" || { echo "SKIP (mutation could not be applied)"; SKIPPED=$((SKIPPED+1)); rm -rf "$WORK"; return 0; }
    local out; out=$(run_gate "$WORK")
    if printf '%s' "$out" | grep -qF "$want"; then
        # It must also still be able to PASS — a check that always fails is equally useless. The
        # unmutated tree is verified once at the end rather than per case.
        echo "OK — gate reported: $(printf '%s' "$out" | grep -F "$want" | head -1 | sed 's/^ *//' | cut -c1-84)"
        PASS=$((PASS+1))
    else
        echo "*** NOT DETECTED *** expected a [FAIL] containing: $want"
        printf '%s' "$out" | grep -E '^\s*\[FAIL\]' | sed 's/^/        also-failed: /' | head -4
        FAILED=$((FAILED+1))
    fi
    rm -rf "$WORK"
}

# --- mutations. Each rm's before writing: the copy is hard-linked to the real repo. -------------

# Repack an APK with one member replaced by a mutated version of itself.
repack_member() {                      # $1=tree $2=member path in apk $3=shell fn applied to the file
    local tree="$1" member="$2" mutate="$3"
    local apk="$tree/out/angrybirds-8.0.3-arm64.apk"
    local tmp; tmp=$(mktemp -d)
    ( cd "$tmp" && unzip -o -q "$apk" "$member" ) || { rm -rf "$tmp"; return 1; }
    "$mutate" "$tmp/$member" || { rm -rf "$tmp"; return 1; }
    rm -f "$apk.new"; cp "$apk" "$apk.new" 2>/dev/null
    ( cd "$tmp" && zip -q "$apk.new" "$member" ) || { rm -rf "$tmp"; return 1; }
    rm -f "$apk"; mv "$apk.new" "$apk"          # rm first: the original is hard-linked to the repo
    rm -rf "$tmp"; return 0
}

m_append_diag()  { printf 'GALLOC-CORRUPT' >> "$1"; }
m_append_perf()  { printf '[perf-split]'   >> "$1"; }
m_flip_byte()    { printf '\x00' | dd of="$1" bs=1 seek=64 conv=notrunc status=none; }

mut_diagnostics() { repack_member "$1" lib/arm64-v8a/libAngryBirdsClassic.so m_append_diag; }
mut_perf()        { repack_member "$1" lib/arm64-v8a/libAngryBirdsClassic.so m_append_perf; }
mut_payload()     { repack_member "$1" lib/arm64-v8a/libengine32.so          m_flip_byte;  }

# Put the ORIGINAL (un-de-phone-homed) manifest back: INTERNET live, no kill-switch meta-data.
mut_manifest() {
    # Two statements, deliberately: in `local a="$1" b="$a/x"` the right-hand sides are expanded
    # before the assignments take effect, so $a is still empty and b becomes "/x". That produced
    # "cannot stat '/out/angrybirds-8.0.3-arm64.apk'" and the case skipped itself.
    local tree="$1"
    local apk="$tree/out/angrybirds-8.0.3-arm64.apk"
    local tmp; tmp=$(mktemp -d)
    ( cd "$tmp" && unzip -o -q "$tree/apks/com.rovio.angrybirds@8.0.3.apk" AndroidManifest.xml ) || { rm -rf "$tmp"; return 1; }
    rm -f "$apk.new"; cp "$apk" "$apk.new"
    ( cd "$tmp" && zip -q "$apk.new" AndroidManifest.xml ) || { rm -rf "$tmp"; return 1; }
    rm -f "$apk"; mv "$apk.new" "$apk"
    rm -rf "$tmp"; return 0
}

# An allocation whose result is dereferenced without a NULL check.
mut_alloc() {
    local f="$1/port/shim/src/galloc.c"
    [ -f "$f" ] || return 1
    local body; body=$(cat "$f")
    rm -f "$f"                                  # break the hard link before writing
    printf '%s\nvoid abshim_mutation_probe(void){ char *p = malloc(32); p[0] = 1; }\n' "$body" > "$f"
}

# A doc that references a script which does not exist.
mut_docref() {
    local f="$1/port/OPEN_FINDINGS.md"
    [ -f "$f" ] || return 1
    local body; body=$(cat "$f")
    rm -f "$f"
    printf '%s\n\nSee `port/validation/this_script_does_not_exist.sh` for details.\n' "$body" > "$f"
}

# A provenance row pointing at a build that is no longer current.
mut_stale() {
    local f="$1/reports/shots/provenance.tsv"
    [ -f "$f" ] || return 1
    local body; body=$(cat "$f")
    rm -f "$f"
    printf '%s\n' "$body" | sed '1s/\t[0-9a-f]\{64\}\t/\tdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef\t/' > "$f"
}

# A proof on disk that the index never mentions (the opposite direction from mut_missing_proof —
# the index check has two branches and only testing one would leave the other unproven).
mut_extra_proof() {
    local d="$1/reports/shots"
    [ -d "$d" ] || return 1
    cp "$(ls "$d"/PROOF_*.png | head -1)" "$d/PROOF_99_mutation_probe.png" 2>/dev/null || return 1
}

# A proof named by the index that is not on disk.
mut_missing_proof() {
    local p; p=$(ls "$1"/reports/shots/PROOF_*.png 2>/dev/null | head -1)
    [ -n "$p" ] || return 1
    rm -f "$p"
}

echo "== mutation test: break each guarantee, confirm the gate says so =="
case_run diagnostics   "diagnostic string(s) leaked into release"       mut_diagnostics
case_run perf          "contains perf instrumentation"                  mut_perf
case_run payload       "libAngryBirdsClassic.so != libengine32.so"      mut_payload
case_run manifest      "LIVE permission present in the manifest"        mut_manifest
case_run alloc         "shim allocations used without a NULL check"     mut_alloc
case_run docref        "docs reference files that are NOT in the repo"  mut_docref
case_run stale         "capture(s) are from builds that are no longer current" mut_stale
case_run missing_proof "the index names proofs that do not exist"     mut_missing_proof
case_run extra_proof   "proofs exist that the index never describes"    mut_extra_proof

echo
echo "== control: the real tree must still PASS =="
if run_gate "$REPO" | grep -q "ALL CHECKED CLAIMS HOLD"; then
    echo "  OK — unmutated tree passes (so the failures above are the mutations, not a broken gate)"
else
    echo "  *** the real tree does NOT pass — every result above is meaningless ***"; FAILED=$((FAILED+1))
fi

echo
echo "  detected: $PASS   NOT detected: $FAILED   skipped: $SKIPPED"
# A SKIP IS NOT A PASS. The first version printed "ALL MUTATIONS DETECTED" on a run where nothing
# ran at all — 0 detected, 9 skipped — because the verdict only looked at $FAILED. That is the exact
# defect this suite exists to hunt (a count of zero from a measurement that never happened), and it
# appeared here, in the harness written to catch it. The verdict now requires positive evidence.
if [ "$FAILED" -ne 0 ]; then
    echo "  SOME MUTATIONS WENT UNDETECTED — those checks pass vacuously"
    exit 1
elif [ "$PASS" -eq 0 ]; then
    echo "  NOTHING WAS TESTED — $SKIPPED case(s) skipped, 0 ran. This is NOT a pass."
    exit 1
elif [ "$SKIPPED" -ne 0 ]; then
    echo "  $PASS detected, but $SKIPPED case(s) could not run — the gate is only proven for what ran."
    exit 1
else
    echo "  ALL $PASS MUTATIONS DETECTED"
    exit 0
fi
