#!/bin/bash
# verify_claims.sh — check the DOCUMENTED claims against the actual artifact.
#
# WHY THIS EXISTS
# ---------------
# On 2026-07-27 four documents were found to assert that the guest heap was "verified
# bit-identical between x86 and arm64". Measurement showed it is not (x86 605096 vs arm64
# 539536 after the 125 constructors). That claim had been repeated for days because nothing
# re-checked it — it was written once and then cited.
#
# The sequel makes the point better than the original: on 2026-07-28 the *correction* stopped being
# true too. Both architectures now measure 605096 (x86 via run_tests.sh, AArch64 via
# arm64_cross_test.sh, and again without -DRTLD_DEFAULT=0). A number is only as good as its last
# measurement — which is the entire reason this script exists.
#
# This script mechanically re-checks the claims that CAN be checked from the artifact, so a
# false one cannot survive silently again. It does not check the claims that need a running
# emulator (those are the emu_*.sh scripts) or the phone (nothing here can).
#
# ---------------------------------------------------------------------------
# THESE CHECKS HAVE BEEN DELIBERATELY BROKEN (2026-07-28)
# ---------------------------------------------------------------------------
# A check nobody has seen fail is not a check. Each of these was made to fail on purpose, against a
# tampered copy of the real artifact, and the pristine APK was restored and re-hashed afterwards:
#
#   engine authenticity      appended a byte to libengine32.so         -> caught
#   16 KB page alignment     relinked the shim without max-page-size   -> caught (LOAD align = 0x1000)
#   libm in DT_NEEDED        relinked the shim without -lm             -> caught
#   no diagnostics in release injected "GALLOC-CORRUPT" into the shim  -> caught
#   signature v1+v2+v3       added a file to the APK after signing     -> caught
#   no live network perm     swapped in the ORIGINAL un-stripped manifest -> caught
#   firebase kill-switch     corrupted the meta-data name             -> caught
#   layer 2 socket hard-fail erased "hard-fail (de-phone-home)"        -> caught
#   provenance staleness     hand-built manifest w/ wrong + missing rows -> caught
#   provenance label lint    reintroduced a fixed record_build label   -> caught
#   proof index consistency  added an unlisted proof / a phantom entry -> caught
#   audio-variant sameness   appended a byte to the AUDIO build's engine  -> caught
#   audio shim size bound    padded the audio shim by 200 KB (>1%)        -> caught
#   documented log markers   added a phantom marker to ONDEVICE.md        -> caught (see below)
#   doc-referenced files     (a) referenced a script that does not exist  -> caught
#                            (b) referenced a file present but UNTRACKED  -> caught  <- the real bug
#   unmeasured input         disabled the extraction the scans read from  -> caught
#                            (previously reported OK, having measured nothing)
#
# TWO of these attempts did not go as expected, and both were instructive:
#
#   - the layer-2 case first reported a MISS that was not one: the test grepped for the SUCCESS
#     message, so when the check correctly failed the pattern matched nothing. Reading the check's
#     source settled it. A failed negative test can mean the test is wrong, not the check.
#   - the log-marker case reported a REAL miss. The check was titled "every log marker ONDEVICE.md
#     tells you to grep for" but never opened ONDEVICE.md — it compared a HARDCODED list against the
#     source, so a newly documented marker was invisible to it, which is exactly the drift it exists
#     to prevent. It now extracts the markers from the doc and keeps the fixed list only as a floor.
#     Trying to break a check is also how you discover it was never doing what it said.
#
# Every check in this file has now been deliberately broken at least once.
#
# TITLES ARE PART OF THE CHECK. Two claims here asserted more than their code established, and both
# were found by asking "could I break this?" rather than by reading the code for correctness:
#   - "every log marker ONDEVICE.md tells you to grep for" never opened ONDEVICE.md (now it does);
#   - "the audio variant is the same build with only the mixer enabled" bounded nothing about how
#     the two shims differ (now it bounds their size delta, and the title says what is actually
#     established rather than what would be nice to establish).
# A check that overstates itself is worse than no check: it produces confidence without coverage.
#
# Usage — runs anywhere; it uses apksigner/zipalign from PATH when present (as inside ab-port,
# which is how REPRODUCE.md step 3 invokes it) and otherwise shells out to the ab-port image.
# Only minSdk/targetSdk needs ab-analyze; everything else, including the whole permission /
# de-phone-home check, is self-contained.
#   bash port/validation/verify_claims.sh
# Exits non-zero if any checked claim is false. Anything it could NOT check is counted and named
# in the final line — a skip must never read as a pass.

set +e
cd "$(dirname "$0")/../.." || exit 1
APK=out/angrybirds-8.0.3-arm64.apk
AUDIO=out/angrybirds-8.0.3-arm64-audio.apk
ORIG=apks/com.rovio.angrybirds@8.0.3.apk
FAIL=0
ok(){ printf "  [ OK ] %s\n" "$1"; }
# Guard for checks that COUNT things. A count of 0 from a missing or unreadable file is
# indistinguishable from a genuine zero, so `strings | grep -c` and `readelf | awk` both report
# success when they have measured nothing at all. That is the inverse - and more dangerous form - of
# the "cannot ask vs bad answer" confusion found three times already: here a failure to measure
# reads as a clean result. Every counting check asserts its input exists first.
have(){ [ -s "$1" ] && return 0; bad "$2: cannot measure - $1 is missing or empty"; return 1; }
bad(){ printf "  [FAIL] %s\n" "$1"; FAIL=1; }
# A skipped check must not read as a passing one: "ALL CHECKED CLAIMS HOLD" printed after two
# silent [skip]s is a false all-clear. Skips are counted and named in the verdict.
SKIPPED=0; SKIPLIST=""
skip(){ printf "  [skip] %s\n" "$1"; SKIPPED=$((SKIPPED+1)); SKIPLIST="$SKIPLIST\n           - $1"; }
[ -f "$APK" ] || { echo "missing $APK — run port/build_apk.sh first"; exit 1; }

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT

# Extract the full arm64 payload set ONCE, unconditionally. This used to happen only inside the
# "original APK present" branch below, so on a fresh clone $T/n held just the shim and the audio
# comparison further down silently compared against missing files and reported DIFFERS - a false
# failure driven by unrelated state.
unzip -o -q "$APK" -d "$T/n" 'lib/arm64-v8a/*' 'classes.dex' 2>/dev/null

# The repo commits only the xz-compressed original. Decompress it here rather than skipping the
# check: a gate that quietly downgrades itself on a fresh clone is worse than one that fails.
if [ ! -f "$ORIG" ] && [ -f "$ORIG.xz" ] && command -v xz >/dev/null 2>&1; then
  echo "  (decompressing $ORIG.xz for the authenticity check)"
  xz -dk "$ORIG.xz" && CLEANUP_ORIG=1
fi

echo "== CLAIM: engine/libjs/libadcolony/classes.dex are byte-for-byte Rovio's original 8.0.3 =="
if [ -f "$ORIG" ]; then
  unzip -o -q "$ORIG" -d "$T/o" 'lib/armeabi-v7a/*' 'classes.dex' 2>/dev/null
  for pair in "libAngryBirdsClassic.so:libengine32.so" "libjs.so:libjs32.so" "libadcolony.so:libadcolony32.so"; do
    o=${pair%%:*}; m=${pair##*:}
    a=$(sha256sum "$T/o/lib/armeabi-v7a/$o" 2>/dev/null | cut -d' ' -f1)
    b=$(sha256sum "$T/n/lib/arm64-v8a/$m"  2>/dev/null | cut -d' ' -f1)
    [ -n "$a" ] && [ "$a" = "$b" ] && ok "$o == $m  ($a)" || bad "$o != $m"
  done
  a=$(sha256sum "$T/o/classes.dex" 2>/dev/null | cut -d' ' -f1)
  b=$(sha256sum "$T/n/classes.dex" 2>/dev/null | cut -d' ' -f1)
  [ -n "$a" ] && [ "$a" = "$b" ] && ok "classes.dex unmodified ($a)" || bad "classes.dex differs"
else
  bad "cannot verify authenticity: neither $ORIG nor a usable $ORIG.xz is present"
fi

echo "== CLAIM: the shim ELF is 16 KB-page aligned (loads on 16 KB-page devices) =="
unzip -o -q "$APK" -d "$T/n" 'lib/arm64-v8a/libAngryBirdsClassic.so'
AL=$(readelf -lW "$T/n/lib/arm64-v8a/libAngryBirdsClassic.so" 2>/dev/null | awk '/LOAD/{print $NF}' | sort -u | tr '\n' ' ')
[ "$(echo "$AL" | tr -d ' ')" = "0x4000" ] && ok "LOAD align = 0x4000 (16 KB)" || bad "LOAD align = $AL (want 0x4000)"

echo "== CLAIM: libm is in DT_NEEDED (without it modern bionic fails to resolve sin/cos) =="
readelf -d "$T/n/lib/arm64-v8a/libAngryBirdsClassic.so" 2>/dev/null | grep -q "libm.so" \
  && ok "libm.so present in DT_NEEDED" || bad "libm.so MISSING — would crash on launch"

# Titled precisely: this greps a KNOWN list of diagnostic markers. It cannot prove the absence of
# ALL scaffolding, only that these do not ship. Keep the list in step with any new diagnostic.
echo "== CLAIM: none of the known heap-diagnostic markers ship in the release build =="
if have "$T/n/lib/arm64-v8a/libAngryBirdsClassic.so" "release diagnostic scan"; then
  N=$(strings -a "$T/n/lib/arm64-v8a/libAngryBirdsClassic.so" | grep -cE 'GALLOC-CORRUPT|GALLOC-STATS|HEAP-PINPOINT|ABSHIM_ALLOC_TRACE')
  [ "$N" = "0" ] && ok "no diagnostic strings in the shipped shim" || bad "$N diagnostic string(s) leaked into release"
fi

echo "== CLAIM: signed v1+v2+v3, and 4-byte zipaligned =="
# Prefer the tools on PATH, fall back to docker. This script's DOCUMENTED invocation is inside
# ab-port (REPRODUCE.md step 3), where apksigner/zipalign exist but `docker` does not - so probing
# only for docker made both of these load-bearing checks silently [skip] in exactly the mode they
# are meant to run in. The skip printed no warning and did not affect the exit code.
SIGN=""; ALIGN=""; PFX=""
if command -v apksigner >/dev/null 2>&1 && command -v zipalign >/dev/null 2>&1; then
  SIGN="apksigner"; ALIGN="zipalign"
elif command -v docker >/dev/null 2>&1 && docker image inspect ab-port >/dev/null 2>&1; then
  SIGN="docker run --rm --network none -v $PWD:/work ab-port apksigner"
  ALIGN="docker run --rm --network none -v $PWD:/work ab-port zipalign"; PFX="/work/"
fi
if [ -n "$SIGN" ]; then
  V=$($SIGN verify -v "$PFX$APK" 2>/dev/null | grep -cE "^Verified using v[123] scheme.*true")
  [ "$V" = "3" ] && ok "v1+v2+v3 all verify" || bad "only $V of 3 signature schemes verify"
  $ALIGN -c 4 "$PFX$APK" >/dev/null 2>&1 && ok "zipalign -c 4 passes" || bad "not 4-byte aligned"
else
  skip "signing/alignment (no apksigner on PATH, no ab-port image)"
fi

echo "== CLAIM: the shipped APK carries NO perf instrumentation =="
# build_apk_x86_perf.sh compiles -DABSHIM_PERF to time the frame split. That build is a measurement
# tool, never a deliverable, and its header used to say "verify_claims.sh checks that" — which was
# simply untrue until this check existed. (The project has hit that exact shape twice before: a
# claim whose TITLE asserted more than its code established.)
#
# Decisive because the perf build's format strings live in the shim's .rodata and cannot be
# optimised out: if "[perf-split]" or "[perf] frames=" appears in the shipped shim, a perf build was
# shipped. have() guards the extraction so a failed unzip cannot pass as "no instrumentation found"
# — absence of evidence is not evidence of absence, which is the single most repeated defect here.
SHIM="$T/shim_arm64.so"
unzip -p "$APK" lib/arm64-v8a/libAngryBirdsClassic.so > "$SHIM" 2>/dev/null
if have "$SHIM" "perf instrumentation"; then
  PERFHIT=""
  for marker in '[perf-split]' '[perf] frames='; do
    strings -a "$SHIM" | grep -qF "$marker" && PERFHIT="$PERFHIT $marker"
  done
  [ -z "$PERFHIT" ] && ok "no ABSHIM_PERF instrumentation in the shipped shim ($(stat -c%s "$SHIM") bytes scanned)" \
                    || bad "the shipped shim contains perf instrumentation:$PERFHIT (a measurement build was released)"
fi

echo "== CLAIM: minSdk 16 / targetSdk 26, and NO live network permission =="
# The PERMISSION half is checked self-contained, always. depermission.py mangles each permission's
# first letter (INTERNET -> XNTERNET) preserving byte length, so the literal string's presence or
# absence in the AXML string pool is decisive - androguard is not needed for it. This half IS the
# de-phone-home guarantee, so it must never depend on an optional image being present.
unzip -p "$APK" AndroidManifest.xml > "$T/am.bin" 2>/dev/null
LIVE=""
for perm in android.permission.INTERNET android.permission.ACCESS_NETWORK_STATE \
            android.permission.ACCESS_WIFI_STATE com.android.vending.BILLING \
            android.permission.GET_ACCOUNTS com.google.android.c2dm.permission.RECEIVE; do
  if strings -a "$T/am.bin" | grep -qxF "$perm" || strings -a -el "$T/am.bin" | grep -qxF "$perm"; then
    LIVE="$LIVE $perm"
  fi
done
[ -z "$LIVE" ] && ok "no live network/billing/account/push permission in the manifest" \
               || bad "LIVE permission present in the manifest:$LIVE"
# The mangled form must also BE there - otherwise "no INTERNET string" could equally mean the
# manifest was never processed, or that we read the wrong file.
if strings -a "$T/am.bin" | grep -qxF android.permission.XNTERNET \
   || strings -a -el "$T/am.bin" | grep -qxF android.permission.XNTERNET; then
  ok "INTERNET present-but-mangled (XNTERNET) - the strip demonstrably ran"
else
  bad "no XNTERNET marker - depermission.py did not run on this manifest"
fi
# minSdk/targetSdk are AXML integers, not pool strings, so they need a parser — but requiring the
# ab-analyze image meant this could only run from a host with docker, and the documented invocation
# is INSIDE ab-port, where there is none. It therefore skipped every time in the one mode that is
# documented. axml_sdk.py reads them with the stdlib alone; it was cross-checked against androguard
# on this artifact (both report 16 / 26) before replacing it.
RES=$(python3 port/validation/axml_sdk.py "$T/am.bin" 2>/dev/null | grep '^RESULT')
if [ -n "$RES" ]; then
  set -- $RES
  [ "$2" = "16" ] && ok "minSdk 16" || bad "minSdk $2 (want 16)"
  [ "$3" = "26" ] && ok "targetSdk 26 (installs on modern Android)" || bad "targetSdk $3 (want 26)"
else
  bad "could not parse minSdk/targetSdk out of the manifest"
fi

echo "== CLAIM: the SDK auto-init kill-switches are injected =="
for k in firebase_messaging_auto_init_enabled com.facebook.sdk.AutoLogAppEventsEnabled; do
  unzip -p "$APK" AndroidManifest.xml 2>/dev/null | strings | grep -q "$k" \
    && ok "$k present" || bad "$k MISSING"
done

echo "== CLAIM: de-phone-home layer 2 — the shim hard-fails the network for the guest =="
strings -a "$T/n/lib/arm64-v8a/libAngryBirdsClassic.so" | grep -q "hard-fail (de-phone-home)" \
  && ok "hard-fail bridge present in the shipped shim" || bad "de-phone-home bridge marker MISSING"
# Stronger than the bridges returning -1: the shim must not even IMPORT socket syscalls, so there
# is no socket capability in the binary for anything to reach.
NETIMP=0
# readelf on a missing file yields no rows, awk prints 0, and every symbol looks absent - i.e. the
# strongest de-phone-home statement here would be produced by not measuring anything.
if ! readelf -sW --dyn-syms "$T/n/lib/arm64-v8a/libAngryBirdsClassic.so" 2>/dev/null | grep -q "Symbol table"; then
  bad "socket-import scan: cannot read the shim's dynamic symbol table - not measured"
  NETIMP=2
fi
for sym in socket connect sendto recvfrom getaddrinfo gethostbyname; do
  [ "$NETIMP" = "2" ] && break
  c=$(readelf -sW --dyn-syms "$T/n/lib/arm64-v8a/libAngryBirdsClassic.so" 2>/dev/null \
      | awk -v s="$sym" '$8==s && $7=="UND"{n++} END{print n+0}')
  [ "$c" != "0" ] && { bad "shim IMPORTS host $sym (x$c) — it could open a real socket"; NETIMP=1; }
done
[ "$NETIMP" = "0" ] && ok "shim imports NO host socket symbols (no socket capability at all)"

# TITLE CORRECTED (2026-07-28). This used to claim "the same build with only the mixer enabled",
# which nothing here verifies - no check bounded how the two shims differ, and `nativeMixData`
# appears in BOTH (the mixer is gated inside the function, not compiled out). Same failure mode as
# the log-marker check: a title asserting more than the code establishes.
echo "== CLAIM: the audio variant ships identical payloads + the same hardening as the silent build =="
if [ -f "$AUDIO" ]; then
  unzip -o -q "$AUDIO" -d "$T/a" 'lib/arm64-v8a/*' 'classes.dex' 2>/dev/null
  for f in libengine32.so libjs32.so libadcolony32.so; do
    x=$(sha256sum "$T/n/lib/arm64-v8a/$f" 2>/dev/null | cut -d" " -f1)
    y=$(sha256sum "$T/a/lib/arm64-v8a/$f" 2>/dev/null | cut -d" " -f1)
    [ -n "$x" ] && [ "$x" = "$y" ] && ok "audio variant: $f identical to the silent build" \
                                   || bad "audio variant: $f DIFFERS from the silent build"
  done
  ad="$T/a/lib/arm64-v8a/libAngryBirdsClassic.so"
  readelf -d "$ad" 2>/dev/null | grep -q libm.so && ok "audio variant: libm in DT_NEEDED" || bad "audio variant: libm MISSING"
  if have "$ad" "audio-variant diagnostic scan"; then
    n=$(strings -a "$ad" | grep -cE "GALLOC-CORRUPT|GALLOC-STATS|HEAP-PINPOINT|ABSHIM_ALLOC_TRACE")
    [ "$n" = "0" ] && ok "audio variant: no diagnostic strings" || bad "audio variant: $n diagnostic string(s) leaked"
  fi
  # Bound the divergence. This cannot prove "only the mixer differs" - that is a claim about
  # behaviour - but it does catch the audio variant silently becoming a DIFFERENT build (other
  # flags, a debug build, a stale binary). Measured delta is ~208 bytes on ~9.1 MB.
  sz_s=$(stat -c%s "$T/n/lib/arm64-v8a/libAngryBirdsClassic.so" 2>/dev/null)
  sz_a=$(stat -c%s "$ad" 2>/dev/null)
  if [ -n "$sz_s" ] && [ -n "$sz_a" ]; then
    d=$(( sz_a > sz_s ? sz_a - sz_s : sz_s - sz_a )); lim=$(( sz_s / 100 ))
    [ "$d" -le "$lim" ] && ok "audio shim differs from the silent one by $d bytes (<=1%: one build, one flag)" \
                        || bad "audio shim differs by $d bytes (>1% of $sz_s) - that is not just a flag"
  fi
else
  echo "  [skip] $AUDIO not built"
fi

echo "== CLAIM: every log marker ONDEVICE.md tells you to grep for is actually emitted =="
# The on-device triage tree is only useful if the strings it names exist. It drifted once and
# nobody noticed: it documented `call[1] nativeInit (VII) @0x...` and `render[N] GL draws=`, neither
# of which the shim emits, and TWO diagnostic rows were keyed on the second one. Someone grepping
# for them with the phone in hand would have found nothing and concluded the shim had stalled.
# This used to check a HARDCODED list against the source, while claiming to check "every marker
# ONDEVICE.md tells you to grep for". It never opened ONDEVICE.md. So a marker newly documented
# there was invisible to it - which is precisely the drift it exists to prevent. Proven by adding a
# phantom marker to the doc: the check passed. Now the doc is the source of truth for WHICH markers
# to verify, and the fixed list below is only a floor of ones that must always be present.
MISSING=""
# (a) every bracketed shim tag the doc shows in backticks, e.g. `[empty-json-guard]`
DOCMARKS=$(grep -oE '`\[[a-z][a-z0-9_-]{2,30}\]`' port/ONDEVICE.md 2>/dev/null | tr -d '`' | sort -u)
for m in $DOCMARKS; do
  grep -rqF "$m" port/shim/src/ 2>/dev/null || MISSING="$MISSING $m"
done
NDOC=$(printf '%s\n' "$DOCMARKS" | grep -c . )
# (b) a floor of markers the triage tree depends on regardless of how the doc phrases them
for m in "empty-json-guard" "s-construct-null-guard" "cpu_create failed" "loader_load failed" \
         "init_array" "shim_call" "h_fatal" "abshim ready" "GL draws="; do
  grep -rqF "$m" port/shim/src/ 2>/dev/null || MISSING="$MISSING [$m]"
done
if [ -z "$MISSING" ]; then ok "all triage markers exist in the shim source (${NDOC:-0} extracted from ONDEVICE.md + the fixed floor)"
else bad "ONDEVICE.md names markers the shim never emits:$MISSING"; fi
# and the two strings that were wrong must not come back
for bogus in "call\[1\] nativeInit" "render\[N\] GL draws" "render\[1\] GL draws"; do
  if grep -nE "$bogus" port/ONDEVICE.md 2>/dev/null | grep -qv "An earlier version\|neither of which\|not on a separate"; then
    bad "ONDEVICE.md again documents a non-existent marker matching /$bogus/"
  fi
done

echo "== CLAIM: no capture script can silently overwrite another's provenance row =="
# This bug has been made TWICE: a script parameterised by ABSHIM_OUTPFX ($PFX) writes its
# screenshots under that prefix but passes a FIXED label to record_build, so running it on a second
# AVD overwrites the first run's row and two different sets of evidence end up sharing one
# provenance entry. Both times it was caught by reading the manifest afterwards, which is luck, not
# process. The coupling is implicit in the source, so check it mechanically instead.
BADLBL=""
for f in port/validation/emu_*.sh; do
  [ -e "$f" ] || continue
  grep -q 'record_build' "$f" || continue
  grep -qE '^PFX=' "$f" || continue                      # not parameterised -> a fixed label is fine
  lbl=$(grep -oE 'record_build "\$APK" "[^"]*"' "$f" | head -1 | sed 's/.*"\(.*\)"$/\1/')
  [ "$lbl" = '$PFX' ] || BADLBL="$BADLBL $(basename "$f")(label=$lbl)"
done
[ -z "$BADLBL" ] && ok "every parameterised capture script labels its provenance with \$PFX" \
                 || bad "these take \$PFX but record a FIXED provenance label:$BADLBL"

echo "== CLAIM: no shim allocation is used without a NULL check =="
# Three separate iterations each found one more instance of the same bug: an allocation whose
# result is dereferenced on the next line (handle_table's pools, sched's getobj, then fdt_create
# and idmap_grow). Finding one per pass is not convergence, so the pattern is checked mechanically
# and the class is closed. Guarded means the variable is tested within three lines of the call.
UNCHECKED=$(python3 - <<'PYEOF'
import re, glob
# Capture the FULL lvalue (t->v, e->data, L->sym_addr), not just the trailing identifier - the
# first version captured "data" from "e->data" and so never matched "if(e->data)". Window is 10
# lines with comment lines skipped, because the guard often sits under an explanatory comment.
bad=[]
ALLOC=re.compile(r'([A-Za-z_][\w.>\[\]-]*)\s*=\s*\(?[\w\s*]*\)?\s*(?:malloc|calloc|realloc|strdup)\s*\(')
for p in sorted(glob.glob('port/shim/src/*.c')):
    lines=open(p,encoding='utf-8').read().split('\n')
    for i,l in enumerate(lines):
        if 'galloc_' in l: continue           # guest address; 0 means failure and the guest checks
        m=ALLOC.search(l)
        if not m: continue
        v=m.group(1)
        code=[x for x in lines[i:i+10] if not x.lstrip().startswith(('*','/*','//'))]
        w='\n'.join(code)
        pat=re.escape(v)
        # no word-boundary after the lvalue: it can end in ']' or '.', where \b never matches
        if re.search(r'!\s*'+pat+r'(?![\w.\[>-])', w): continue
        if re.search(r'if\s*\(\s*'+pat+r'\s*[\)&]', w): continue  # if(v) / if(v && ...)
        if re.search(pat+r'\s*(==|!=)\s*(NULL|0)', w): continue
        bad.append(f"{p.split('/')[-1]}:{i+1}({v})")
print(' '.join(bad))
PYEOF
)
[ -z "$UNCHECKED" ] && ok "every shim allocation is NULL-checked before use" \
                    || bad "shim allocations used without a NULL check:$UNCHECKED"

echo "== CLAIM: every script and Dockerfile the docs reference is actually in the repo =="
# Checked against `git ls-files`, NOT the working tree. The distinction is the whole point: the docs
# once described `apks/…8.0.3.apk` and `work803/…/libAngryBirdsClassic.so` as "committed in this
# repo" when only the .xz is committed. Both files existed HERE, because a build had produced them,
# so any presence check would have passed while a fresh clone hit errors naming files it had never
# seen. Tracked-in-git is what a reader can actually rely on.
DOCREF=$( { grep -rhoE 'port/[a-zA-Z0-9_/.-]+\.(sh|py)' --include='*.md' . 2>/dev/null;
            grep -rhoE 'Dockerfile\.[a-zA-Z0-9_-]+' --include='*.md' . 2>/dev/null | sed 's|^|port/docker/|'; } | sort -u )
# `-c safe.directory=*` because this normally runs INSIDE a container as root while /work is owned
# by the host user, and git then refuses with "detected dubious ownership" — which made EVERY file
# look untracked and produced a confident, completely false FAIL listing files that are plainly in
# the repo. A check that cannot tell "not tracked" from "cannot ask" is worse than no check.
GIT="git -c safe.directory=*"
if ! $GIT rev-parse --git-dir >/dev/null 2>&1; then
  skip "doc-referenced files (no usable git repo here - cannot distinguish untracked from unknowable)"
  DOCREF=""
fi
UNTRACKED=""
for f in $DOCREF; do
  $GIT ls-files --error-unmatch "$f" >/dev/null 2>&1 || UNTRACKED="$UNTRACKED $f"
done
NREF=$(printf '%s\n' "$DOCREF" | grep -c .)
if [ -n "$DOCREF" ]; then
  [ -z "$UNTRACKED" ] && ok "all $NREF doc-referenced scripts/Dockerfiles are tracked in git" \
                      || bad "docs reference files that are NOT in the repo:$UNTRACKED"
fi

echo "== CLAIM: the screenshots were captured on builds that still exist =="
# Screenshots have gone stale silently twice: PROOF_2/3/4 showed a binary that no longer existed,
# and PROOF_10 (the audio variant's only evidence) predated a session of shim changes while a bulk
# docs sweep kept the hash printed beside it looking current. lib_provenance.sh now makes each
# capture record the sha256 of the APK it actually installed, so that question can be ASKED.
#
# Deliberately NOT a hard failure: regenerating every proof after each shim change is hours of
# emulator time, and a gate people learn to ignore is worse than no gate. Instead stale evidence is
# counted and NAMED in the verdict, exactly like a skip - neither may read as a pass.
PROV=reports/shots/provenance.tsv
if [ ! -f "$PROV" ]; then
  skip "screenshot provenance (no $PROV yet - re-run the emu_*.sh captures to populate it)"
else
  STALE=""; CUR=0; NOTBUILT=""
  while IFS="$(printf '\t')" read -r lbl sha apk; do
    [ -n "$lbl" ] || continue
    # "not built here" is NOT "stale". A fresh clone builds only the arm64 deliverable, so every x86
    # proxy is absent and all 12 rows were being reported as evidence from builds "that no longer
    # exist" - which reads as rotten evidence when the truth is simply that the reader has not built
    # those variants. Same failure as the git check: conflating "cannot ask" with "the answer is bad".
    if [ ! -f "out/$apk" ]; then NOTBUILT="$NOTBUILT $lbl"; continue; fi
    now=$(sha256sum "out/$apk" 2>/dev/null | cut -d' ' -f1)
    if [ "$now" = "$sha" ]; then CUR=$((CUR+1))
    else STALE="$STALE\n           - $lbl: captured on ${sha:0:12}…, $apk is now ${now:0:12}…"; fi
  done < "$PROV"
  [ "$CUR" -gt 0 ] && ok "$CUR capture(s) match the current build"
  if [ -n "$NOTBUILT" ]; then
    N=$(printf '%s\n' $NOTBUILT | grep -c .)
    printf "  [note ] %s capture(s) reference variants not built here (normal on a fresh clone):%s\n" "$N" "$NOTBUILT"
  fi
  if [ -n "$STALE" ]; then
    STALECNT=$(printf "%b" "$STALE" | grep -c "^ *-")
    printf "  [STALE] %s capture(s) are from builds that are no longer current:%b\n" "$STALECNT" "$STALE"
    STALEMSG="$STALECNT screenshot capture(s) predate the current build"
  fi
fi

echo "== CLAIM: the screenshot index describes exactly the proofs that exist =="
# PROOF_2/3/4 silently went stale for a day: they showed a binary that no longer existed, and
# nothing noticed because nothing recorded what they were supposed to show. reports/shots/README.md
# now records that — but an index that can drift from the directory is just more stale
# documentation. Check BOTH directions, so neither a new proof nor a removed one can slip past.
IDX=reports/shots/README.md
if [ ! -f "$IDX" ]; then bad "the screenshot index $IDX is missing"
else
  UNLISTED=""; GHOST=""
  for p in reports/shots/PROOF_*.png; do
    [ -e "$p" ] || continue
    grep -qF "$(basename "$p")" "$IDX" || UNLISTED="$UNLISTED $(basename "$p")"
  done
  # every PROOF_*.png named in the index must exist (catches renames like PROOF_4_flight.png)
  for n in $(grep -oE 'PROOF_[A-Za-z0-9_]+\.png' "$IDX" | sort -u); do
    [ -f "reports/shots/$n" ] || GHOST="$GHOST $n"
  done
  [ -z "$UNLISTED" ] && ok "every proof on disk is described in the index" \
                     || bad "proofs exist that the index never describes:$UNLISTED"
  [ -z "$GHOST" ]    && ok "every proof the index names actually exists" \
                     || bad "the index names proofs that do not exist:$GHOST"
fi

[ "${CLEANUP_ORIG:-0}" = "1" ] && rm -f "$ORIG"
echo
if [ "$FAIL" = "0" ]; then
  [ -n "$STALEMSG" ] && echo "NOTE: $STALEMSG — regenerate them, or read them as historical."
  if [ "$SKIPPED" = "0" ]; then echo "ALL CHECKED CLAIMS HOLD"
  else
    [ "$SKIPPED" = "1" ] && W="check was" || W="checks were"
    printf "ALL CHECKED CLAIMS HOLD, but %s %s NOT checked:%b\n" "$SKIPPED" "$W" "$SKIPLIST"
  fi
else echo "SOME CLAIMS ARE FALSE — fix the artifact or the docs"; fi
exit "$FAIL"
