#!/bin/bash
# mutation_modules.sh — prove the MODULE suite can fail, one module at a time.
#
# WHY THIS EXISTS
# ---------------
# `run_tests.sh` ends with "ALL MODULE TESTS PASSED", and that line is quoted as evidence in
# validate_all.sh, the README and the release notes. Until now exactly ONE module had been shown to
# be capable of failing: the validation README notes that breaking galloc's size-class arithmetic
# makes the AArch64 galloc test exit non-zero. For the other seventeen, "passed" rested on the
# assumption that the test would notice.
#
# That assumption is worth checking rather than trusting — the same lesson that produced
# port/validation/mutation_test.sh for the claim gate, after one of its checks turned out to have
# been passing vacuously for days.
#
# Two things were verified statically first, and both are recorded because a clean sweep is a
# result:
#   - Every test binary the suite RUNS has a path that exits non-zero. (A first pass wrongly flagged
#     four, including test_galloc_quarantine, because it looked at the LAST `return` in main and so
#     missed an earlier conditional `return 1`. The tool was wrong, not the tests.)
#   - The only two tests that genuinely cannot fail — test_boot_render.c and test_perf2.c — are
#     diagnostic probes and are NOT executed by run_tests.sh, so they cannot inflate its verdict.
#
# WHAT THIS DOES
# --------------
# Breaks one real invariant in one module's SOURCE, rebuilds, and asserts that module's test exits
# non-zero. Mutations are chosen to be semantically meaningful — a wrong size class, a broken UTF-8
# continuation check, an ELF magic that accepts anything — not cosmetic edits that a compiler would
# fold away.
#
# Runs against a COPY of the tree (rm-then-write, never in-place: see mutation_test.sh for why).
#
#   docker run --rm --network none -v "$PWD":/work -w /work ab-hosttest \
#       bash port/shim/test/mutation_modules.sh
set +e
cd "$(dirname "$0")/../../.." || exit 1
REPO="$PWD"
WORK="${ABSHIM_MODMUT_DIR:-/tmp/modmut}"
ONLY="$1"
PASS=0; MISSED=0; SKIPPED=0

# Build+run a single module test out of a mutated tree. Mirrors run_tests.sh's compile lines.
build_run() {                          # $1=tree $2=module $3=extra srcs
    local tree="$1" mod="$2" extra="$3"
    local S="$tree/port/shim/src" H="$tree/port/shim/test" O; O=$(mktemp -d)
    # Modules that touch cpu.c/bridge_gl.c need Unicorn, exactly as run_tests.sh links them. The
    # vendored tree is used from THIS copy so a mutation to it would be picked up too.
    local UNI="$tree/port/shim/vendor/unicorn" ULIB="" UINC=""
    if [ -f "$UNI/lib/libunicorn.a" ]; then ULIB="$UNI/lib/libunicorn.a"; UINC="-I$UNI/include"; fi
    cc -w -O2 -DRTLD_DEFAULT=0 -I"$S" $UINC "$H/test_$mod.c" $extra $ULIB -lpthread -lm -ldl -o "$O/t" 2>"$O/cc.log" || {
        echo "BUILDFAIL"; rm -rf "$O"; return 9; }
    # Point tests that need the engine at THIS tree's copy. Without it test_elf32 falls back to a
    # hardcoded host path, does not find it, prints "SKIP: engine .so not found" and RETURNS 0 — so
    # the mutated build looked like a pass and the case reported NOT DETECTED. The harness was
    # skipping the test, not the test missing the bug.
    local out; out=$(ABSHIM_ENGINE_SO="$tree/work803/libv7/libAngryBirdsClassic.so" "$O/t" 2>&1); local rc=$?
    # A test that SKIPPED proves nothing either way — never let that count as a run.
    if printf '%s' "$out" | grep -qiE '^\s*SKIP:'; then rm -rf "$O"; return 8; fi
    rm -rf "$O"; return $rc
}

# case <name> <module> <extra-srcs-relative-to-src> <sed-expr-file> <sed-expr>
case_run() {
    local name="$1" mod="$2" srcs="$3" file="$4" expr="$5"
    [ -n "$ONLY" ] && [ "$ONLY" != "$name" ] && return 0
    printf '  %-16s ' "$name"
    rm -rf "$WORK"
    cp -al "$REPO" "$WORK" 2>/dev/null; cp -an "$REPO/." "$WORK/" 2>/dev/null
    local n_src n_dst; n_src=$(find "$REPO" -type f | wc -l); n_dst=$(find "$WORK" -type f 2>/dev/null | wc -l)
    [ "$n_dst" = "$n_src" ] || { echo "SKIP (copy incomplete $n_dst/$n_src)"; SKIPPED=$((SKIPPED+1)); rm -rf "$WORK"; return 0; }

    # expand $srcs against the COPY's src dir
    local ES="" f
    for f in $srcs; do ES="$ES $WORK/port/shim/src/$f"; done

    # control: unmutated must PASS, else the case proves nothing
    build_run "$WORK" "$mod" "$ES"; local base=$?
    if [ "$base" -eq 9 ]; then echo "SKIP (control build failed)"; SKIPPED=$((SKIPPED+1)); rm -rf "$WORK"; return 0; fi
    if [ "$base" -eq 8 ]; then echo "SKIP (test skipped itself — missing input)"; SKIPPED=$((SKIPPED+1)); rm -rf "$WORK"; return 0; fi
    if [ "$base" -ne 0 ]; then echo "SKIP (control test already fails, rc=$base)"; SKIPPED=$((SKIPPED+1)); rm -rf "$WORK"; return 0; fi

    # mutate: rm first — the copy is hard-linked to the real repo
    local tgt="$WORK/port/shim/src/$file"
    local body; body=$(cat "$tgt") || { echo "SKIP (no $file)"; SKIPPED=$((SKIPPED+1)); rm -rf "$WORK"; return 0; }
    rm -f "$tgt"; printf '%s\n' "$body" | sed "$expr" > "$tgt"
    if cmp -s <(printf '%s\n' "$body") "$tgt"; then
        echo "SKIP (mutation matched nothing — the sed expr is stale)"; SKIPPED=$((SKIPPED+1)); rm -rf "$WORK"; return 0; fi

    build_run "$WORK" "$mod" "$ES"; local rc=$?
    if [ "$rc" -eq 8 ]; then
        echo "SKIP (mutated test skipped itself — proves nothing)"; SKIPPED=$((SKIPPED+1))
    elif [ "$rc" -eq 9 ]; then
        echo "OK (mutation refused to compile — still a detection)"; PASS=$((PASS+1))
    elif [ "$rc" -ne 0 ]; then
        echo "OK (test exits $rc)"; PASS=$((PASS+1))
    else
        echo "*** NOT DETECTED *** test still exits 0 with the invariant broken"; MISSED=$((MISSED+1))
    fi
    rm -rf "$WORK"
}

GAPS=0
# A case for a mutation the suite is KNOWN not to catch. Reported as an open gap rather than a
# failure — the fact is documented, and a red run every time would train people to ignore this.
# Its detection status is still checked: if a future test closes the gap, this says so.
case_run_gap() {
    local name="$1"
    local out; out=$(case_run "$@" 2>&1)
    if printf '%s' "$out" | grep -q "NOT DETECTED"; then
        printf '  %-16s OPEN GAP (documented) — no test in the suite catches this\n' "$name"
        # NO MISSED adjustment: case_run ran inside $( ), a subshell, so its increments never
        # reached this shell in the first place. Decrementing here drove the count to -2 and made
        # the verdict claim undetected mutations that did not exist.
        GAPS=$((GAPS+1))
    else
        printf '%s\n' "$out"
        printf '  %-16s (gap now CLOSED — update the comment above this case)\n' "$name"
    fi
}

echo "== module mutation test: break one invariant, confirm that module's test fails =="

# galloc: the size-class formula. chunk must be align16(n)+16 so payload >= align16(n); dropping the
# +16 hands back short blocks — the exact defect the current formula was written to fix.
case_run galloc     galloc     "galloc.c" galloc.c \
  's/uint32_t s = want + ALIGN;/uint32_t s = want;/'

# utf: the UTF-8 continuation-byte check. Accepting any second byte makes malformed sequences decode.
case_run utf        utf        "utf.c" utf.c \
  's/(s\[1\] & 0xC0) != 0x80/0/'

# elf32: the magic check. Returning success for a non-ELF blob is the loader trusting anything.
case_run elf32      elf32      "elf32.c" elf32.c \
  's/if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) return -2;//'

# handle_table: exhaustion must be reported. Returning slot 0 instead of POOL_FULL silently aliases
# every caller onto one handle.
case_run handle     handle_table "handle_table.c" handle_table.c \
  's/void \*ht_resolve(handle_table \*t, uint32_t tok){/void *ht_resolve(handle_table *t, uint32_t tok){ return NULL;/'

# format: %x must format hex. Folding it into the decimal case is a wrong-output bug that any
# formatting assertion should see.
case_run format     format     "format.c marshal.c" format.c \
  "s/case 'u': case 'o': case 'x': case 'X':/case 'u': case 'o':/"

# ctype_tables: the ASCII classification tables the guest's libc leans on.
case_run ctype      ctype_tables "ctype_tables.c" ctype_tables.c \
  's/T_ctype\[c+1\] = classify(c);/T_ctype[c+1] = 0;/'

# marshal: the AAPCS 8-byte alignment rule. An i64/double must start on an EVEN core register;
# dropping the round-up hands every bridge the wrong half of a 64-bit argument. This is the module
# every other bridge sits on, so a silent defect here is the worst case in the shim.
case_run marshal    marshal    "marshal.c" marshal.c \
  's/if (c->ncrn & 1) c->ncrn++;//'

# jni_arg: JNI argument assembly on top of marshal + the handle table.
case_run jni_arg    jni_arg    "jni_argbuild.c marshal.c handle_table.c" jni_argbuild.c \
  's/uint32_t cell = ap + (uint32_t)n \* 8u;/uint32_t cell = ap + (uint32_t)n * 4u;/'

# fdtable: fds 0,1,2 are reserved; allocating from 0 hands out stdin/stdout as ordinary files.
case_run fdtable    fdtable    "fdtable.c" fdtable.c \
  's/for (int i = 3; i < t->n; i++)/for (int i = 0; i < t->n; i++)/'

# gl_sizes: the overflow guard that stops a guest-supplied w*h*count from exceeding the scratch
# buffer the driver is then told to read. Written after a near-miss found by reading code, not by a
# test — so it had better be able to fail.
case_run gl_sizes   gl_sizes   "bridge_gl.c cpu.c galloc.c marshal.c format.c" bridge_gl.c \
  's/if(v > GL_SCRATCH_MAX) return 0;//'

# galloc_quarantine: the held-block window IS the use-after-free protection — the fix for the
# session-long std::string UAF. Collapsing it to zero means a freed block is reusable immediately,
# which is exactly the pre-fix behaviour.
case_run quarantine galloc_quarantine "galloc.c" galloc.c \
  's/#define QUARANTINE_N 131072u/#define QUARANTINE_N 0u/'

# KNOWN GAP (2026-07-28), kept as a standing reminder rather than deleted. Verified against the FULL
# suite, not just this module: removing the GT_BLOCKED transition is caught by NOTHING in
# run_tests.sh. test_sched drives pthread_create/join and a cond_wait handoff, and both still pass —
# so the blocked-state bookkeeping that the deadlock detector depends on is unasserted. Closing it
# needs a test that parks a thread with no runnable peer and expects the detector to fire; that is a
# real test to design, not a one-liner, so it is recorded here instead of faked.
case_run_gap sched      sched      "cpu.c loader.c dispatch.c sched.c galloc.c elf32.c ctype_tables.c marshal.c format.c bridge_gl.c bridge_asset.c bridge_libc.c bridge_file.c handle_table.c" sched.c \
  's/else                      r->state=GT_BLOCKED;//'

# KNOWN GAP (2026-07-28), same treatment. Zeroing the SP saved into the guest's jmp_buf is caught by
# NOTHING in the suite: test_longjmp's guest returns almost immediately after the longjmp, so a
# bogus restored SP is never dereferenced before the test ends. A test that touches the stack after
# longjmp would close it.
case_run_gap longjmp    longjmp    "cpu.c loader.c dispatch.c sched.c galloc.c elf32.c ctype_tables.c marshal.c format.c bridge_gl.c bridge_asset.c bridge_libc.c bridge_file.c handle_table.c" dispatch.c \
  's/uc_reg_read(cc->uc,UC_ARM_REG_SP,&v); gm_wr32(&cc->mem,jb+32,v);/gm_wr32(\&cc->mem,jb+32,0u);/'

# The BR-table bridges (memcpy/malloc/free/str*) live in dispatch.c, NOT in bridge_libc.c, so
# test_libc — which drives libc_try — never sees them. Running the FULL suite against this mutation
# showed it IS caught (by the boot/ctors device tests, which run the real engine), so the coverage
# exists; it simply is not this module's. Attributed to ctors, where the detection actually happens.
case_run memcpy_br  ctors      "cpu.c loader.c dispatch.c sched.c galloc.c elf32.c ctype_tables.c marshal.c format.c bridge_gl.c bridge_asset.c bridge_libc.c bridge_file.c handle_table.c" dispatch.c \
  's/static uint64_t h_memcpy (dispatch_t\*d,mcur\*c){ heap_pin(d,"before","memcpy");/static uint64_t h_memcpy (dispatch_t*d,mcur*c){ return 0;/'

echo
echo "  detected: $PASS   NOT detected: $MISSED   skipped: $SKIPPED   documented gaps: $GAPS"
# A skip is not a pass — same rule as mutation_test.sh, which once printed success on a run where
# nothing executed.
[ "$GAPS" -gt 0 ] && echo "  ($GAPS documented gap(s): a mutation no test in the suite catches — see the comments above those cases)"
if [ "$MISSED" -ne 0 ]; then
    echo "  SOME MODULE MUTATIONS WENT UNDETECTED — those tests pass vacuously"; exit 1
elif [ "$PASS" -eq 0 ]; then
    echo "  NOTHING WAS TESTED — $SKIPPED skipped, 0 ran. NOT a pass."; exit 1
elif [ "$SKIPPED" -ne 0 ]; then
    echo "  $PASS detected, but $SKIPPED case(s) could not run — proven only for what ran."; exit 1
else
    echo "  ALL $PASS MODULE MUTATIONS DETECTED"; exit 0
fi
