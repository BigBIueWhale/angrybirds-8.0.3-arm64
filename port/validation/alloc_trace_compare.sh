#!/bin/bash
# alloc_trace_compare.sh — diff the guest's allocation sequence between x86 and AArch64.
#
# WHY THIS EXISTS
# ---------------
# The strongest available statement about the emulation being host-independent is not "both hosts
# finish the constructors" or even "both report the same heap total" — it is that the guest issues
# the *same allocations, in the same order, with the same sizes, from the same call sites* on both.
#
# That claim had gone stale twice over. It was once "the guest heap is bit-identical across host
# architectures" (false: measured 605096 vs 539536). Then it was corrected to "7792 of 7793
# allocations match" — true at the time, but measured BEFORE the galloc size-class fix and never
# redone, so the README was quoting a number about a binary that no longer existed. Falling back to
# "the totals agree" was honest but weaker.
#
# So: measure it properly, and make it repeatable. Each side runs test_ctors with
# ABSHIM_ALLOC_TRACE set, which records one line per allocation — `m|c|r <size> lr=<engine-rel PC>` —
# then the two traces are compared byte-for-byte.
#
# RESULT (2026-07-28): all 7793 records identical, same sha256 on both sides.
#
# Needs both images:
#   docker build -f port/docker/Dockerfile.ab-hosttest -t ab-hosttest port/docker
#   docker build -f port/docker/Dockerfile.ab-arm64x  -t ab-arm64x   port/docker
#   bash port/validation/alloc_trace_compare.sh
set +e
cd "$(dirname "$0")/../.." || exit 1
OUT="${ABSHIM_TRACE_DIR:-/tmp/abshim-traces}"; mkdir -p "$OUT"
FAIL=0

cat > "$OUT/_x86.sh" <<'EOS'
set -e
cd /work
. /work/port/prepare_inputs.sh; REPO=/work prepare_inputs >/dev/null
export ABSHIM_ENGINE_SO=/work/work803/libv7/libAngryBirdsClassic.so
SRC=/work/port/shim/src; T=/work/port/shim/test; U="${ABSHIM_UNICORN:-/work/port/shim/vendor/unicorn}"
DEV="$SRC/cpu.c $SRC/loader.c $SRC/dispatch.c $SRC/sched.c $SRC/galloc.c $SRC/elf32.c $SRC/ctype_tables.c $SRC/marshal.c $SRC/format.c $SRC/bridge_gl.c $SRC/bridge_asset.c $SRC/bridge_libc.c $SRC/bridge_file.c $SRC/handle_table.c"
# same flags as run_tests.sh - notably NO -D_GNU_SOURCE, which breaks the glibc headers here
cc -w -O2 -DRTLD_DEFAULT=0 -I"$SRC" -I"$U/include" "$T/test_ctors.c" $DEV "$U/lib/libunicorn.a" \
   -lpthread -lm -ldl -o /tmp/tc
ABSHIM_ALLOC_TRACE=/out/trace_x86.txt /tmp/tc >/dev/null 2>&1
EOS

cat > "$OUT/_a64.sh" <<'EOS'
set -e
cd /work
. /work/port/prepare_inputs.sh; REPO=/work prepare_inputs >/dev/null
export ABSHIM_ENGINE_SO=/work/work803/libv7/libAngryBirdsClassic.so
CC=aarch64-linux-gnu-gcc; SRC=/work/port/shim/src; T=/work/port/shim/test
U=/opt/unicorn-b; USRC=/opt/unicorn-src; UL=$(ls $U/libunicorn*.a | tr '\n' ' ')
DEV="$SRC/cpu.c $SRC/loader.c $SRC/dispatch.c $SRC/sched.c $SRC/galloc.c $SRC/elf32.c $SRC/ctype_tables.c $SRC/marshal.c $SRC/format.c $SRC/bridge_gl.c $SRC/bridge_asset.c $SRC/bridge_libc.c $SRC/bridge_file.c $SRC/handle_table.c"
$CC -w -O2 -iquote "$SRC" -I"$USRC/include" -D_GNU_SOURCE -DRTLD_DEFAULT=0 "$T/test_ctors.c" $DEV \
    -Wl,--start-group $UL -Wl,--end-group -lpthread -lm -ldl -o /tmp/tc
ABSHIM_ALLOC_TRACE=/out/trace_a64.txt qemu-aarch64-static -L /usr/aarch64-linux-gnu /tmp/tc >/dev/null 2>&1
EOS

echo "== x86 trace =="
docker run --rm --network none -v "$PWD":/work -v "$OUT":/out -w /work ab-hosttest bash /out/_x86.sh >/dev/null 2>&1
echo "   records: $(wc -l < "$OUT/trace_x86.txt" 2>/dev/null)"
echo "== AArch64 trace (cross-built, run under qemu-user) =="
docker run --rm --network none -v "$PWD":/work -v "$OUT":/out -w /work ab-arm64x bash /out/_a64.sh >/dev/null 2>&1
echo "   records: $(wc -l < "$OUT/trace_a64.txt" 2>/dev/null)"

echo "== compare =="
if [ ! -s "$OUT/trace_x86.txt" ] || [ ! -s "$OUT/trace_a64.txt" ]; then
  echo "  [FAIL] one or both traces are empty - the comparison proves nothing"; FAIL=1
elif cmp -s "$OUT/trace_x86.txt" "$OUT/trace_a64.txt"; then
  echo "  [ OK ] all $(wc -l < "$OUT/trace_x86.txt") allocation records identical (sha256 $(sha256sum "$OUT/trace_x86.txt" | cut -c1-16)…)"
else
  echo "  [DIFF] $(diff "$OUT/trace_x86.txt" "$OUT/trace_a64.txt" | grep -c '^[<>]') differing lines:"
  diff "$OUT/trace_x86.txt" "$OUT/trace_a64.txt" | head -10 | sed 's/^/    /'
  echo "  (a difference is not automatically a bug - it means the guest took a different path on the"
  echo "   two hosts, which is worth understanding before it is either fixed or documented)"
  FAIL=1
fi
exit "$FAIL"
