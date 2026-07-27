#!/usr/bin/env python3
# coverage_check.py — assert EVERY engine UND FUNC import resolves to a bridge, so a
# runtime call can never silently become UNIMPL->0. Run by run_tests.sh; exits 1 if any
# unbridged import remains outside the documented allowlist.
import re, subprocess, sys, os

HERE = os.path.dirname(os.path.abspath(__file__))
SRC  = os.path.join(HERE, "..", "src")
SO   = os.environ.get("ABSHIM_ENGINE_SO",
        os.path.join(HERE, "..", "..", "..", "work803", "libv7", "libAngryBirdsClassic.so"))

def names_in(path):
    try: txt = open(path).read()
    except OSError: return set()
    return set(re.findall(r'\{\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*,', txt))

BR   = names_in(os.path.join(SRC, "dispatch.c"))       # libc BR[] + pthread handlers
LIBC = names_in(os.path.join(SRC, "bridge_libc.c"))    # math/net/string/time/qsort/...
GL   = names_in(os.path.join(SRC, "bridge_gl.c"))      # GLT[] table
FILEB= names_in(os.path.join(SRC, "bridge_file.c"))    # stdio FILE* / fd / mmap / fs
bridged = BR | LIBC | GL | FILEB

# prefix-matched families (asset_try swallows any AAsset*; gl handled by table above)
def prefix_ok(n): return n.startswith("AAsset") or n.startswith("AAssetManager")

# weak-zero whitelist (elf32 resolves these to 0 by design). The exidx finders were
# REMOVED — they are now real bridges (dispatch h_find_exidx) so the C++ unwinder works.
WEAK = {"__google_potentially_blocking_region_begin", "__google_potentially_blocking_region_end"}

# intentionally-unbridged allowlist (documented; each must have a reason). Keep EMPTY as
# the goal; add here only with a justification comment.
ALLOW = set()

und = set()
for ln in subprocess.check_output(["readelf","--dyn-syms","-W",SO]).decode().splitlines():
    p = ln.split()
    if len(p) >= 8 and p[3] == "FUNC" and p[6] == "UND":
        und.add(p[7].split("@")[0])

missing = sorted(f for f in und
                 if f not in bridged and not prefix_ok(f) and f not in WEAK and f not in ALLOW)

print(f"engine UND FUNC: {len(und)} | bridged names: {len(bridged)} (BR={len(BR)} libc={len(LIBC)} gl={len(GL)})")
print(f"UNBRIDGED (would UNIMPL->0 at runtime): {len(missing)}")
if missing:
    for f in missing: print(f"   MISSING  {f}")
    print("=== COVERAGE FAIL ===")
    sys.exit(1)
print("=== COVERAGE OK — every engine import resolves to a bridge ===")
