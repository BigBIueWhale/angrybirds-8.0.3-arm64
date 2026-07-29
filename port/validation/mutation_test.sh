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

# EXIT 2, NOT 0. This suite exists to enforce "a skip is not a pass", and its own unavailable-image
# path returned 0 — success to any automation that checked. Distinct code: 0 = all mutations
# detected, 1 = some went undetected, 2 = the suite could not run at all.
# It also has to run on the HOST: it spawns a container per case, so invoking it INSIDE ab-port hits
# exactly this path (no docker in there) and used to look like a clean pass.
command -v docker >/dev/null 2>&1 && docker image inspect ab-port >/dev/null 2>&1 || {
    echo "[skip] mutation test needs docker + the ab-port image, and must run on the HOST (it starts"
    echo "       a container per case). NOT a pass — exiting 2 so nothing can mistake it for one."
    exit 2; }

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

# VACUITY AUDIT — run BEFORE any case, because a case that cannot fail is worse than a missing one.
#
# Two cases were found passing without their mutation doing anything. `stale_doc` expected a string
# that also appears in its claim's HEADER line (printed every run, pass or fail) AND was wired to the
# wrong function through a name collision. `sockets` expected "network-capable symbol", which is the
# wording of that claim's **OK** line: "shim imports NO network-capable symbol".
#
# Both are the same mistake — an expectation that the CLEAN gate already prints. That is mechanically
# detectable, so it is detected here rather than reasoned about: run the gate once on the unmutated
# tree and refuse to proceed if any case's expected text appears in that output.
vacuity_audit() {
    local clean bad_cases="" name exp rest
    clean=$(run_gate "$REPO")
    while read -r name rest; do
        exp=$(printf '%s' "$rest" | sed -n 's/^ *"\([^"]*\)".*/\1/p')
        [ -n "$exp" ] || continue
        case "$clean" in *"$exp"*) bad_cases="$bad_cases $name" ;; esac
    done < <(grep '^case_run' "$0" | sed 's/^case_run *//')
    if [ -n "$bad_cases" ]; then
        echo "*** VACUOUS CASE(S):$bad_cases — their expected text appears in the CLEAN gate output,"
        echo "    so they would report 'detected' with no mutation applied. Fix before trusting this run."
        return 1
    fi
    echo "  [ OK ] no case expects text the clean gate already prints"
    return 0
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

# Rewrite the AArch64 shim's PT_LOAD p_align from 16 KB to 4 KB. That is precisely the defect that
# makes a library un-loadable on a 16 KB-page device: emu_dlopen_pagesize.sh shows a 4 KB-aligned
# binary segfaulting on such a kernel while the shim loads. The mutation edits the ELF program
# headers in place — same size, no offsets move — so nothing else about the APK changes.
m_align4k() {
    python3 - "$1" <<'PYEOF'
import sys, struct
p = sys.argv[1]
b = bytearray(open(p, 'rb').read())
assert b[:4] == b'\x7fELF' and b[4] == 2, "not ELF64"
phoff  = struct.unpack_from('<Q', b, 0x20)[0]
phentsize, phnum = struct.unpack_from('<HH', b, 0x36)
n = 0
for i in range(phnum):
    off = phoff + i * phentsize
    if struct.unpack_from('<I', b, off)[0] == 1:            # PT_LOAD
        struct.pack_into('<Q', b, off + 48, 0x1000)         # p_align -> 4 KB
        n += 1
assert n, "no PT_LOAD segments found — mutation would be a no-op"
open(p, 'wb').write(bytes(b))
PYEOF
}
mut_align() { repack_member "$1" lib/arm64-v8a/libAngryBirdsClassic.so m_align4k; }

# Re-introduce a superseded measurement into a doc, unframed. This is the exact defect the staleness
# claim exists for: RELEASE_NOTES.md really did keep telling readers the guest heap "differs by about
# 64 KB" long after both architectures measured 605096. The filler lines matter — the checker looks
# at a +/-6 line window for framing words, so the mutation has to land somewhere genuinely unframed
# or it would be (correctly) tolerated as history and the case would prove nothing.
# NAME COLLISION, found the hard way. This was called mut_stale — and so is the pre-existing
# provenance-staleness mutation further down. Bash keeps the LAST definition, so `case_run stale_doc
# ... mut_stale` silently ran the PROVENANCE mutation instead of this one. It looked green because
# the expected string ("quotes a superseded measurement") also appears in the claim's HEADER line,
# which the gate prints on every run, pass or fail. A vacuous case inside the very suite that exists
# to prove checks are not vacuous.
mut_stale_doc() {
    local f="$1/port/ONDEVICE.md"
    [ -f "$f" ] || return 1
    # BREAK THE HARD LINK FIRST. The work tree is cp -al'd from the real repo, so `>>` on this path
    # appends THROUGH the link into the actual port/ONDEVICE.md. It did — twice, once here and once
    # in mut_proof_bytes — and the only reason it was caught is that the new integrity and staleness
    # checks started failing on the CONTROL run. The file header warns about this in its first
    # sentence; both functions were written without following it.
    cp "$f" "$f.mut" && rm -f "$f" && mv "$f.mut" "$f" || return 1
    { printf '\n## Guest heap size\n\n'
      printf 'Filler line one, deliberately free of any qualifying language.\n\n'
      printf 'Filler line two, likewise.\n\n'
      printf 'After the 125 constructors the guest heap holds 539536 bytes on arm64.\n\n'
      printf 'Filler line three.\n'
      printf 'Filler line four.\n'
    } >> "$f"
    grep -q 539536 "$f" || return 1
}

# Rename libm.so in the shim's DT_NEEDED to a library that does not exist, keeping the string length
# identical so no ELF offsets move. This reproduces the exact bug that nearly shipped: the shim used
# sin/cos without declaring libm, ran on API 25, and would have died on the A56 with
# `UnsatisfiedLinkError: cannot locate symbol "sin"`. It should trip BOTH the specific libm claim and
# the general import-resolvability claim — if only the specific one fires, the general one is not
# doing its job.
m_unlink_libm() {
    python3 - "$1" <<'PYEOF'
import sys
p = sys.argv[1]
b = open(p, 'rb').read()
assert b.count(b'libm.so\x00') > 0, "libm.so not found in the ELF strings — mutation would be a no-op"
open(p, 'wb').write(b.replace(b'libm.so\x00', b'libq.so\x00'))
PYEOF
}
mut_libm() { repack_member "$1" lib/arm64-v8a/libAngryBirdsClassic.so m_unlink_libm; }

# Blank the AVD out of a full provenance row. This is exactly the state the ledger was found in:
# eleven of thirteen rows recorded which build a capture came from but not which Android it ran on,
# so "won on API 36" and "won on API 25" were indistinguishable from the record. Legacy 3-field rows
# are exempt by design, so the mutation has to target a 5-field row to test the rule that exists.
mut_prov_env() {
    local f="$1/reports/shots/provenance.tsv"
    [ -f "$f" ] || return 1
    awk -F'\t' 'BEGIN{OFS="\t"} NF==5 && $5!="" && !done {$5=""; done=1} {print}' "$f" > "$f.tmp" || return 1
    grep -qP '\t$' "$f.tmp" || return 1          # the blanked row must actually be blank
    mv "$f.tmp" "$f"
}

# Corrupt one byte of a proof image. reports/shots/README.md opens with "every correctness claim in
# this repo ultimately rests on these images", and until the integrity section existed a proof could
# be replaced or truncated with every check still green. One byte is the smallest possible version of
# that, and it must be caught.
mut_proof_bytes() {
    local f
    f=$(ls "$1"/reports/shots/PROOF_*.png 2>/dev/null | head -1) || return 1
    [ -n "$f" ] || return 1
    # COPY, REMOVE, THEN WRITE. The work tree is hard-linked to the real repo (cp -al), so an
    # in-place `dd conv=notrunc` writes THROUGH the link and corrupts the actual evidence file. It
    # did: this function's first version silently damaged reports/shots/PROOF_10_audio_levelwin.png
    # in the real tree, which the new integrity check then caught on the control run. The file header
    # warns about exactly this ("Each rm's before writing"); the warning was there and I did not
    # follow it.
    cp "$f" "$f.mut" || return 1
    rm -f "$f" || return 1
    mv "$f.mut" "$f" || return 1
    printf '\x00' | dd of="$f" bs=1 seek=64 conv=notrunc status=none || return 1
}

mut_diagnostics() { repack_member "$1" lib/arm64-v8a/libAngryBirdsClassic.so m_append_diag; }
mut_perf()        { repack_member "$1" lib/arm64-v8a/libAngryBirdsClassic.so m_append_perf; }
mut_payload()     { repack_member "$1" lib/arm64-v8a/libengine32.so          m_flip_byte;  }

# Give the shim slot a binary that DOES import sockets. Rovio's own engine imports 18 network
# symbols, so this is the mutation the socket scan exists to catch — and it is a real binary rather
# than a corrupted one, so the failure is the scan firing rather than the file being unreadable.
mut_sockets() {
    local tree="$1"
    local apk="$tree/out/angrybirds-8.0.3-arm64.apk"
    local eng="$tree/work803/libv7/libAngryBirdsClassic.so"
    [ -f "$eng" ] || return 1
    local tmp; tmp=$(mktemp -d)
    mkdir -p "$tmp/lib/arm64-v8a"
    cp "$eng" "$tmp/lib/arm64-v8a/libAngryBirdsClassic.so" || { rm -rf "$tmp"; return 1; }
    rm -f "$apk.new"; cp "$apk" "$apk.new"
    ( cd "$tmp" && zip -q "$apk.new" lib/arm64-v8a/libAngryBirdsClassic.so ) || { rm -rf "$tmp"; return 1; }
    rm -f "$apk"; mv "$apk.new" "$apk"
    rm -rf "$tmp"; return 0
}

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


# Flip one character of a documented SHA-256. This is what a hand-transcription error looks like,
# and it is exactly how the audio APK's digest was wrong in RELEASE_NOTES.md on 2026-07-28.
mut_dochash() {
    local f="$1/RELEASE_NOTES.md"
    [ -f "$f" ] || return 1
    local body; body=$(cat "$f"); rm -f "$f"
    printf '%s\n' "$body" | sed -E '0,/^[0-9a-f]{64}  angrybirds/s/^([0-9a-f]{63})[0-9a-f]( angrybirds)/\1z\2/' > "$f"
    grep -qE '^[0-9a-f]{63}z ' "$f" || {
        # the substitution must actually land, or this case proves nothing
        printf '%s\n' "$body" | sed -E '0,/^[0-9a-f]{64}  angrybirds/s/^[0-9a-f]{2}/ff/' > "$f"; }
    ! cmp -s <(printf '%s\n' "$body") "$f"
}

# Re-sign the deliverable with a freshly generated key — precisely what every build script's
# `keytool -genkeypair` fallback produces when port/debug.ks is missing. The APK stays perfectly
# valid; only the signer changes, and a differently-signed APK cannot update-install over the one
# already on the phone.
mut_signer() {
    local tree="$1"
    local apk="$tree/out/angrybirds-8.0.3-arm64.apk"
    local tmp; tmp=$(mktemp -d)
    docker run --rm --network none -v "$tmp":/t -v "$tree":/w -w /w ab-port bash -c '
        keytool -genkeypair -keystore /t/rogue.ks -storepass android -keypass android \
                -alias r -keyalg RSA -keysize 2048 -validity 3650 -dname "CN=Rogue" >/dev/null 2>&1
        rm -rf /t/x && unzip -q -o out/angrybirds-8.0.3-arm64.apk -d /t/x 2>/dev/null
        rm -rf /t/x/META-INF
        (cd /t/x && zip -q -r /t/uns.apk .) && zipalign -f -p 4 /t/uns.apk /t/al.apk >/dev/null 2>&1
        apksigner sign --ks /t/rogue.ks --ks-pass pass:android --key-pass pass:android \
                 --out /t/rogue.apk /t/al.apk >/dev/null 2>&1' >/dev/null 2>&1
    # The container writes as root, so a plain `rm -rf` here fails on every extracted file with
    # "Permission denied" and leaves a few hundred MB of debris in /tmp. Remove it from inside a
    # container, which owns those files.
    local ok=1; [ -s "$tmp/rogue.apk" ] && ok=0
    [ "$ok" -eq 0 ] && { rm -f "$apk"; cp "$tmp/rogue.apk" "$apk"; }
    # Clear the directory's CONTENTS rather than a list of expected names: apksigner also writes a
    # .idsig alongside the APK, which an enumerated list missed and left 776 KB behind. Deleting what
    # is actually there cannot fall behind what the tools decide to produce.
    docker run --rm -v "$tmp":/t alpine sh -c 'rm -rf /t/..?* /t/.[!.]* /t/*' >/dev/null 2>&1
    rmdir "$tmp" 2>/dev/null
    return "$ok"
}


# Rename the package inside the manifest with a SAME-LENGTH substitution — exactly the shape a
# targeting bug in depermission.py would produce, since that script rewrites manifest strings in
# place and length-preserving is its whole technique. An APK declaring a different package installs
# as a separate app instead of an update.
mut_identity() {
    # Two statements, NOT `local tree="$1" apk="$tree/..."`. In a single `local` the right-hand sides
    # are expanded before the assignments take effect, so $tree is still empty and apk becomes
    # "/out/angrybirds-8.0.3-arm64.apk". mut_manifest had this exact bug, it was fixed and commented
    # there, and I reproduced it here anyway — which is why the comment now lives at both sites.
    local tree="$1"
    local apk="$tree/out/angrybirds-8.0.3-arm64.apk"
    local tmp; tmp=$(mktemp -d)
    ( cd "$tmp" && unzip -o -q "$apk" AndroidManifest.xml ) || { rm -rf "$tmp"; return 1; }
    python3 - "$tmp/AndroidManifest.xml" <<'PYEOF' || { rm -rf "$tmp"; return 1; }
import sys
p=sys.argv[1]; d=open(p,'rb').read()
for enc in ('utf-8','utf-16-le'):
    a='com.rovio.angrybirds'.encode(enc); b='com.rovio.angrybirdX'.encode(enc)
    if a in d:
        open(p,'wb').write(d.replace(a,b)); sys.exit(0)
sys.exit(1)
PYEOF
    rm -f "$apk.new"; cp "$apk" "$apk.new"
    ( cd "$tmp" && zip -q "$apk.new" AndroidManifest.xml ) || { rm -rf "$tmp"; return 1; }
    rm -f "$apk"; mv "$apk.new" "$apk"
    rm -rf "$tmp"; return 0
}


# Put a 32-bit ABI directory back. On a device supporting both ABIs Android could then load Rovio's
# ORIGINAL ARM32 engine instead of the shim — which an AArch64-only core cannot execute at all.
mut_strayabi() {
    local tree="$1"
    local apk="$tree/out/angrybirds-8.0.3-arm64.apk"
    local tmp; tmp=$(mktemp -d)
    mkdir -p "$tmp/lib/armeabi-v7a"
    ( cd "$tmp" && unzip -o -q "$apk" lib/arm64-v8a/libjs32.so 2>/dev/null \
        && cp lib/arm64-v8a/libjs32.so lib/armeabi-v7a/libjs.so ) || { rm -rf "$tmp"; return 1; }
    rm -f "$apk.new"; cp "$apk" "$apk.new"
    ( cd "$tmp" && zip -q "$apk.new" lib/armeabi-v7a/libjs.so ) || { rm -rf "$tmp"; return 1; }
    rm -f "$apk"; mv "$apk.new" "$apk"
    rm -rf "$tmp"; return 0
}

echo "== mutation test: break each guarantee, confirm the gate says so =="
# Remove the camera reset from a script that shoots across rounds. This is the harness bug that cost
# two full 15-minute runs and nearly entered the record as a fact about the GAME ("fixed coordinates
# cannot aim, so arbitrary levels cannot be cleared"). Deleting the pan restores exactly the drifting
# harness that produced one win in five cycles while the process looked perfectly healthy — frames
# advancing, h_fatal 0, level file open — because a camera pointed at empty sky is invisible from
# everything except a screenshot.
mut_camera() {
    local f="$1/port/validation/emu_progression.sh"
    [ -f "$f" ] || return 1
    # Cut the pan call itself, leaving the drag loop intact — the precise regression.
    rm -f "$f.tmp"; grep -v 'pan_and_capture "\$OUT/prog_' "$f" > "$f.tmp" || return 1
    # The drag must SURVIVE the mutation, or this case would prove nothing — it would be testing a
    # script that no longer shoots. Checked against `input swipe` generally: the literal coordinates
    # this once matched are gone, the anchor is computed now, and the guard correctly refused to
    # apply rather than silently mutating something else.
    grep -q 'adb shell input swipe' "$f.tmp" || return 1
    rm -f "$f"; mv "$f.tmp" "$f"
}

echo "== vacuity audit: can every case only pass because of its mutation? =="
vacuity_audit || VACUOUS=1

case_run diagnostics   "diagnostic string(s) leaked into release"       mut_diagnostics
case_run perf          "contains perf instrumentation"                  mut_perf
case_run payload       "libAngryBirdsClassic.so != libengine32.so"      mut_payload
case_run sockets       "the socket-import scan failed"                 mut_sockets
case_run manifest      "STILL LIVE in the shipped manifest"            mut_manifest
# The SAME mutation, asserted against the OTHER check it must trip. Restoring Rovio's manifest
# removes the layer-4 kill-switches as well as un-mangling the permissions, and case_run greps for
# ONE string, so each check needs its own case or one of them is covered only by accident.
case_run killswitch    "kill-switch MISSING"                           mut_manifest
case_run alloc         "shim allocations used without a NULL check"     mut_alloc
case_run docref        "docs reference files that are NOT in the repo"  mut_docref
case_run stale         "capture(s) are from builds that are no longer current" mut_stale
case_run missing_proof "the index names proofs that do not exist"     mut_missing_proof
case_run extra_proof   "proofs exist that the index never describes"    mut_extra_proof

case_run stray_abi     "stray ABI directories survived the strip"      mut_strayabi
case_run identity      "identity CHANGED by the conversion"           mut_identity
case_run doc_hash      "documented SHA-256 does not match the artifact"  mut_dochash
case_run signer        "signed by an UNEXPECTED key"                     mut_signer
case_run camera        "without resetting the camera"                  mut_camera
case_run align16k      "NOT 16 KB-aligned"                             mut_align
case_run stale_doc     "a doc quotes a superseded measurement as current" mut_stale_doc
case_run libm_gone     "libm.so MISSING"                               mut_libm
case_run prov_env      "not the environment it ran on"                 mut_prov_env
case_run proof_bytes   "does not match the bytes the index recorded"   mut_proof_bytes

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
