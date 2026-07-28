# Validation rig

The scripts that produced every piece of runtime evidence in `reports/shots/`.

These lived only in an ephemeral `/tmp` scratchpad until now. They are committed here so the
evidence base is reproducible — and, more importantly, *falsifiable* — from the repository
alone. Nothing in this directory is needed to build the APK; `port/build_apk.sh` is
self-contained and offline. This is the test apparatus, not the build.

---

## Read this before citing any result from this directory

**No gameplay has ever been validated on arm64.** Every play-validation here — PROOF_2
through PROOF_10, every win screen, every level transition — was produced by the shim
**compiled for x86_64, running on an x86_64 Android emulator**. Every run log records the
engine loading from `.../lib/x86_64/libengine32.so`. There is no `lib/arm64-v8a` in any log.

That is not an oversight. An arm64 AVD is impossible on an x86_64 host: emulator 36.6.11
fails with *"Avd's CPU Architecture 'arm64' is not supported by the QEMU2 emulator on x86_64
host"* — Google removed cross-architecture QEMU. So the arm64 end-to-end run genuinely
requires the physical Galaxy A56.

The argument that the x86 rig proxies for arm64 is: identical shim source, and Unicorn runs the
ARM32 guest faithfully on either host — 125/125 C++ constructors execute clean on both, and the
guest's allocation sequence is identical for 7792 of 7793 requests (see below).

**That argument is weaker than this file used to claim.** It previously said the guest heap was
"verified bit-identical between x86 and arm64 runs". It is not. `test_ctors` after the 125
constructors reports x86 `605096` bytes in use versus arm64 `539536` — a ~64 KB gap, stable on
each host, present both before and after the allocator fix, and not attributable to the Unicorn
build (the pinned commit compiled for x86-linux reproduces the x86 figure exactly), to host page
size (hardcoded 4096), or to nondeterminism (x86 repeats identically).

**Update (2026-07-28): the ~64 KB gap is NO LONGER REPRODUCIBLE — both architectures now
report `605096`.** Measured fresh on the same day: x86 via `run_tests.sh` = `605096`; AArch64 via
the cross-compiled `arm64_cross_test.sh` = `605096`, and again `605096` when rebuilt without
`-DRTLD_DEFAULT=0` (so the flag difference between the two suites, a plausible confound, is ruled
out). The earlier arm64 figure of `539536` cannot be obtained now. The one variable not eliminated
is how the AArch64 binary is produced — the old number came from a build made *inside* an emulated
arm64 container, the new ones from a cross toolchain — and re-running that path costs hours of
emulated compilation. So: treat the guest heap as **measured identical across architectures
today**, while noting the figure has moved before and is worth re-measuring rather than cited.

**Cause identified (2026-07-27).** The divergence is not drift — it is exactly ONE allocation.
Tracing the guest's full allocation sequence on both hosts (`ABSHIM_ALLOC_TRACE`) shows **7792 of
7793 requests are byte-identical** in kind, size and order. The difference is a single doubling
container at `engine+0x938670` — the only site in the whole run that allocates the `2^n + 8`
series `1032, 2056, 4104, 8200, 16392, 32776 …` — which grows one step further on x86 (to 65544)
than on arm64 (stops at 32776). That one block accounts for the entire gap: pre-fix
`req2size(65544)=65552` → payload 65544 = the observed 65544; post-fix `align16(65544)+16=65568`
→ payload 65560 = the observed 65560.

Disassembling that site in **Thumb** mode (`reports/eng.dis` is ARM-mode and therefore garbage
there) shows what it actually is:

```
938668: mov   r4, r0        ; r4 = requested size
93866a: adds  r0, #0x8      ; +8
93866c: blx   <plt>         ; -> malloc
938670: cmp   r0, #0x0      ; <- the captured LR (return from malloc)
938674: strne r4, [r0], #8  ; store the size in a cookie, return ptr+8
```

So `engine+0x938670` is **not the container** — it is inside a sized-allocation wrapper, almost
certainly `operator new[]`, which writes an element-count cookie before the pointer it returns.
That explains the `2^n + 8` shape exactly: the caller requests precisely a power of two and the
wrapper adds the cookie. The doubling container is this wrapper's *caller*, one frame further up,
which the captured LR does not reach.

So this is a **capacity** difference in one container, not divergent execution: both hosts run the
same 7792 allocations and complete all 125 constructors clean. Why that container needs one more
doubling on x86 is unresolved — `engine+0x938670` is stripped static code (nearest symbol is 629 KB
earlier), so naming what it holds needs disassembly rather than symbol lookup.

So: both architectures execute the engine correctly, but they do not reach identical guest state,
and "x86 passed therefore arm64 behaves identically" does not follow. It is not the same as
having run it. **`out/angrybirds-8.0.3-arm64.apk` has never been executed anywhere** —
it has been built, signed, aligned and statically audited only.

arm64 execution is `arm64_cross_test.sh` (and the older, slower `arm64_unicorn_test.sh`), and it
is not the game. Under qemu-user it runs the project's whole test suite on AArch64 — the seven
device tests and all ten mode-agnostic module tests, 19 checks — including the pieces that are
genuinely ABI-sensitive and had **never** run on AArch64 before 2026-07-28:

| test | why the architecture matters |
|---|---|
| `longjmp` | `setjmp`/`longjmp` across `cpu_run` stop/restart — `jmp_buf` layout is per-ABI |
| `sched`   | the green-thread scheduler, built on that same mechanism (300/300 switches, deadlock detection fires) |
| `libc`    | the soft-float / integer ABI bridges — argument passing differs sharply from x86 |
| `file`    | stdio `FILE*` round-trip through guest memory |
| `native_init` | JNI passthrough marshalling up to the JVM boundary (8 env slots exercised) |

Two checkpoints agree with the x86 suite exactly: `605096` bytes in use after the 125 constructors
and `650032` after `nativeInit`.

The suite is negative-tested: deliberately breaking `galloc`'s size-class arithmetic makes the
AArch64 `galloc` module test exit non-zero, so a pass is not vacuous.

There is still no Android, no ART, no GL and no frames here. It validates the ABI and the ARM32
emulation running on it, nothing above that.

Run it with an arm64 container via binfmt (`docker run --privileged --rm tonistiigi/binfmt
--install arm64` once, on the host, to register `qemu-aarch64`):

```bash
docker run --rm --platform linux/arm64 -v "$PWD":/work -v /scratch:/scratch -w /work \
    ubuntu:22.04 bash /work/port/validation/arm64_unicorn_test.sh
```

It caches its Unicorn build in `/scratch/uni-arm64`. **With that cache absent the first run
compiles Unicorn under qemu-user emulation, which takes hours and needs network** — that build is
the only step here that does, and it is why the script keeps the result outside the container.

**Running the real APK on an emulated ARM64 Android is not possible on an x86_64 host.** It was
attempted (`emu_arm64_real_artifact.sh`, and the `ab-emu-arm64` image with its `arm64-v8a` AVD);
the emulator refuses outright:

```
FATAL | Avd's CPU Architecture 'arm64' is not supported by the QEMU2 emulator on x86_64 host.
        System image must match the host architecture.
```

The AVD is creatable but can never boot, which is why nothing had ever used that image. The script
is kept, and now detects that failure in seconds instead of waiting out its boot budget, so the
limitation is recorded by running code rather than by a comment that could drift.

---

## Running the rig

All scripts expect to run **inside** the container with the repo mounted at `/work`. They use
only `/work`-relative paths, so they work unmodified from this location.

```bash
cd /path/to/apk-binary-analysis          # repo root — the dir mounted as /work

docker run --rm --network none \
    --device /dev/kvm --group-add 993 \
    -v "$PWD":/work \
    ab-emu bash /work/port/validation/emu_playthrough.sh
```

Use `ab-emu-34` instead of `ab-emu` for any script whose AVD is `abtest34`. `ab-emu-34` is a
strict superset (it carries both AVDs), so it can run everything.

Build the images first — neither is on Docker Hub:

```bash
docker build -f port/docker/Dockerfile.ab-emu    -t ab-emu    port/docker
docker build -f port/docker/Dockerfile.ab-emu-34 -t ab-emu-34 port/docker
```

`--group-add 993` is this host's `kvm` gid; check yours with `getent group kvm`. Without KVM
the emulator still boots but is far slower.

### Network policy — not optional

Runs are sandboxed on the assumption that the build host may be internet-reachable. Every run
is `--network none`. The Android emulator opens console/adb ports 5554/5555; inside a
`--network none` container those sit in an isolated netns with only a loopback and are
unreachable from the host, let alone the internet.

**Never add `-p`/`--publish`, never `--network host`, never `adb -a` or `ADB_LISTEN_ALL=1`.**
The adb server has no authentication for clients connecting to port 5037 — the device-side
RSA prompt authorizes the *host*, not whoever reaches the socket. Binding it to `0.0.0.0`
would expose unauthenticated `adb shell`/`push`/`pull` to anyone who can route to the machine.

The image builds themselves do need network (Google SDK downloads). That is a separate step
from both the APK build and the test runs, and weakens neither.

### Interactive driving

`emu_interactive.sh` keeps the emulator alive rather than tearing it down, so the game can be
driven by hand from outside:

```bash
docker exec ab-emu-run adb shell input tap 540 960
docker exec ab-emu-run adb shell input swipe 300 700 200 760 400   # slingshot drag
docker exec ab-emu-run adb exec-out screencap -p > /tmp/shot.png
```

That is how PROOF_2/3/4 were produced. It requires launching the container detached with
`--name ab-emu-run` instead of `--rm`.

---

## The proxies the evidence runs on

Until 2026-07-27 these were **not reproducible** — no mtime normalisation, and `KS=/tmp/debug.ks`
was wiped by `--rm` so every build minted a fresh random signing key. Two builds of identical
source produced different APKs, which meant no PROOF could be tied to a specific binary. Both
causes are fixed (`port/build_apk_x86*.sh`); the proxies now rebuild byte-for-byte:

```
1fae5fba747459b75d0a049d6ae135f827777a0a3e5f2bcca1b638218f2fee41  x86shim.apk          (diagnostic)
3bae855100d73915c626c43c231d3690b7952a6c399797e7dfc9cb83548367e5  x86shim-release.apk  (shipping config)
f08f17540e015823a7fc60102363f60a132ccd3081e42dece68d8041e9292c75  x86shim-audio.apk    (audio variant)
```

**PROOF_15** is the first piece of evidence tied to one of these hashes: a full API-34 win
("LEVEL CLEARED", 3 stars, 45790, `h_fatal=0`, frame[1501]) produced by
`emu_modern_playthrough.sh` on `x86shim-release.apk` = `3bae8551…`. Before the reproducibility
fix that sentence could not have been written — every build minted a different signing key, so
no screenshot could be attributed to a recreatable binary.

Re-verify with two consecutive builds and `sha256sum`. If a hash here goes stale, the shim source
changed — regenerate the affected evidence rather than citing screenshots taken against a build
that no longer exists.

## Script → evidence map

PROOF mapping verified by md5 against the source screenshots, not by filename.

| Script | AVD / API | APK under test | Produces |
|---|---|---|---|
| `emu_arm64_real_artifact.sh` | **arm64 / 30 (ARM64!)** | **`angrybirds-8.0.3-arm64.apk` — THE DELIVERABLE** | the only script that runs the artifact users actually install, rather than the x86 proxy. No KVM for an arm64 guest on an x86 host, so everything is QEMU TCG *and* our shim emulates ARM32 inside that — expect it to be very slow. Reports how far it got on a 6-milestone scale (installed → process → shim loaded → engine mapped → ctors ran → frames) instead of pass/fail, because a low milestone means TCG speed, not a broken artifact |
| `emu_layer4_fcm_test.sh` | **abgms / 34 (GMS)** | x86shim-release + x86shim-fbcontrol | **the only tier with Google Play Services.** Differential proof of de-phone-home layer 4: the control build (`ABSHIM_FIREBASE_CONTROL=1`, kill-switch removed) attempts FCM token registration, the shipped build attempts it 0 times. Requires each arm to prove it executed before its measurement counts. Exits non-zero on failure |
| `emu_jni_exception_probe.sh` | abtest34 / 34 | x86shim-release | **the only script that captures UNFILTERED logcat.** Asserts (1) ART reports no pending JNI exception / CheckJNI error / abort, (2) the app's OWN pid does no name resolution or socket work — runtime de-phone-home proof, attributed by pid because the system's NetworkMonitor legitimately resolves names on a different pid, (3) FlurryAgent logs that ACCESS_NETWORK_STATE is missing. Exits non-zero on failure |
| `emu_run.sh` | abtest / 25 | x86shim | first boot-through rig; `emu_screen.png`, `emu_abshim.txt`, `emu_engine.txt` |
| `emu_fatal.sh` | abtest / 25 | x86shim | `emu_fatal_screen.png` → **PROOF_tutorial_render** |
| `emu_fatal_release.sh` | abtest / 25 | x86shim-release | `emu_fatalR_*` — release-config fatal sweep |
| `emu_interactive.sh` | abtest / 25 | x86shim | `interactive_2/3/4.png` → **PROOF_2, PROOF_3, PROOF_4** |
| `emu_playthrough.sh` | abtest / 25 | x86shim | `playthrough_end.png` → **PROOF_6** (level-end survived) |
| `emu_playthrough_release.sh` | abtest / 25 | x86shim-release | `playthroughR_*` — same in shipping config |
| `emu_progress_release.sh` | abtest / 25 | x86shim-release | `progR_2_level2.png` → **PROOF_7** (level-2 progression) |
| `emu_modern_test.sh` | abtest34 / 34 | x86shim-release | `modern34_*` — modern-Android boot; caught the missing `-lm` |
| `emu_modern_playthrough.sh` | abtest34 / 34 | x86shim-release | `modplay_3_end.png` → **PROOF_8** *(original source overwritten)*; re-run 2026-07-27 → **PROOF_11** (win, 44500) |
| `emu_modern_progress.sh` | abtest34 / 34 | x86shim-release | `modprog_2_level2.png` → **PROOF_9** |
| `emu_save_test.sh` | abtest34 / 34 | x86shim-release | `save_*` — save persistence across restart |
| `emu_audio_test.sh` | abtest / 25 | x86shim-audio | `audio_*` — audio-build boot |
| `emu_audio_playthrough.sh` | abtest / 25 | x86shim-audio | `audioplay_end.png` → **PROOF_10** (audio level win) |
| `emu_audio_modern.sh` | abtest34 / 34 | x86shim-audio | `audiomod_*` — audio on API 34 |
| `emu_audio_capture.sh` | abtest / 25 | x86shim-audio | `audiocap_*` — attempts real audio capture |
| `pa_init_test.sh` | abtest / 25 | — | isolates the PulseAudio backend init failure |
| `x86shim_emu_test.sh` | abtest / 25 | x86shim | early x86-shim boot triage |
| `x86shim_gobj.sh` | abtest / 25 | x86shim | game-object write watchpoint triage |
| `capture_stock.sh` | abtest / 25 | *stock 8.0.3* | baseline capture of the unmodified game |
| `stage_pull.sh` | abtest / 25 | offline | pulls app data dir for save inspection |
| `run_render.sh` | — (`ab-render`) | — | host render harness, mesa llvmpipe |
| `run_ctor.sh` | — | — | host constructor-execution harness |
| `arm64_cross_test.sh` | — (cross + qemu-user) | — | **the arm64 validation**: the WHOLE suite on AArch64 — 7 device tests (boot, ctors, longjmp, sched, libc, file, native_init) + all 10 mode-agnostic module tests, 19 checks. CROSS-compiled on x86 in the `ab-arm64x` image so it runs in minutes, fully offline, instead of hours of emulated compilation. Asserts the binaries really are AArch64 |
| `arm64_unicorn_test.sh` | — (qemu-user) | — | the original arm64 validation, built *inside* an emulated arm64 container. Superseded by `arm64_cross_test.sh` for routine use; kept because it is the one path that does **not** depend on a cross toolchain |

### Corrections made 2026-07-27 (read before trusting any older run output)

- **The `levelComplete` metric was measuring nothing, and has been removed.** Several scripts
  reported `levelComplete: N` from `grep -c 'levelComplete'`. Every hit is a
  `data/scripts/particles/levelCompleteStars{1..4}.lua` **asset preload** — four files, two log
  lines each, emitted *before* `frame[1]`. All 14 historical runs report exactly **8**,
  including `emu_fatal` (a crash run). It was constant regardless of outcome and never
  indicated a win. `emu_save_test.sh` reported it as `won:`. Any older `*.txt` output
  containing a `levelComplete` number should be disregarded. **No abshim log marker
  distinguishes a win — the end screenshot is the only authority.**
- **The playthrough tests were flaky.** They waited a fixed `sleep 16` after the last drag.
  Frame rate under SwiftShader varies ~2–16 fps between runs, so 16 s is anywhere from ~29 to
  ~250 frames. The 07-27 09:29 run passed every log metric while screenshotting a level still
  in progress. `emu_modern_playthrough.sh` now waits for **+120 frames** (300 s cap, explicit
  warning if unmet) and its watchdog was raised 1450→2100 s.
- **PROOF_6 is weaker than its filename.** `PROOF_6_levelend_survived.png` shows the game
  alive and rendering — not a results screen. It supports "did not crash"; the `h_fatal=0` log
  carries the "level end survived" claim, not the image.

### Guest-heap inconsistency is API-25-only (open question, but it does not affect modern Android)

`[GALLOC-CORRUPT] galloc_check=-5 → heap chunk/free-list inconsistent` fires during **boot**
(before `frame[1]`), and `-5` is a real dlmalloc invariant violation: `CINUSE(cur) !=
PINUSE(next)`. It is **not** explained by the UAF quarantine fix — `galloc.c:267` states
quarantined blocks deliberately "read as normal in-use chunks to galloc_check". Note the `8`
is a **logging cap** (`dispatch.c:57`, `static int n=0; if(n++<8)`), not an event count, so the
true frequency on API 25 is unknown.

It separates perfectly by Android version:

| tier | runs | GALLOC-CORRUPT lines |
|---|---|---|
| API 25 | 10 / 10 | 8 each (log cap hit) — 80 total |
| API 34 | 0 / 5 | **0** |

Two controls rule out the obvious artifacts: `playthroughR`/`progressR`/`emu_fatalR` are
**release** builds on API 25 and *do* fire it, so it is not gated off in release; and both
tiers log the same 1221 distinct assets from that same boot window, so logd is not dropping
the lines. Different ART/system-service responses between API 25 and 34 plausibly drive a
different guest allocation order. **The A56 is Android 16, i.e. the API-34-like regime, which
shows zero.** Still worth resolving at source — raise the cap in `dispatch.c:57` and re-run an
API-25 script to learn the true count and first-failure point.

### Known gaps in the evidence

- **PROOF_8's source was overwritten** by a later re-run, and could not be regenerated because
  of the flaky fixed-sleep above. **Resolved 2026-07-27:** `PROOF_11_modern_win_reproduced_in_repo.png`
  (+ `.log`) is a fresh API-34 win — "LEVEL CLEARED", 3 stars, **44500**, `h_fatal=0`,
  frame[1501] — produced by the corrected script *from this directory* running on
  `ab-emu-34` built *from the committed Dockerfile*. Its md5 differs from PROOF_8, confirming
  an independent run rather than a copy.

  It is the first PROOF that is **reproducible from the repo**: the script
  (`port/validation/emu_modern_playthrough.sh`), the emulator image recipe
  (`port/docker/Dockerfile.ab-emu-34`) and the APK build (`port/build_apk_x86_release.sh`) are
  all committed, so the run can be repeated rather than merely believed. Note the *log* is
  **not** archived — `.gitignore` tracks only `reports/shots/PROOF_*.png` (`/reports/*` plus a
  global `*.log`), by the project's deliberate "large, regenerable" policy. So the chain is
  reproducible, not archived; `PROOF_11_modern_win_reproduced.log` exists locally but is
  untracked. If you want the log committed, an exception must be added *after* the `*.log`
  line in `.gitignore` — later rules win.
- **PROOF_5 has no surviving log.** `deeper_bird1.png` was captured by ad-hoc `docker exec`
  driving against a live emulator; no script reproduces it and the covering `abshim` log was
  overwritten. The x86_64 host/ABI is certain; whether it used the release or non-release
  x86 build is inferred from a `[jsonparse]` diagnostic line that only the non-release build
  emits.
- **Frame rates here are not predictive.** The emulator renders via SwiftShader (software).
  Measured from the logs, playthroughs ran ~2–16 fps, but when `GL draws=0` the same loop
  held ~16 ms/frame (≈60 fps) — so the cost is software rasterization, not emulation. Real
  GPU behaviour and real frame pacing are among the things only the A56 can settle.

### Not copied from the scratchpad, deliberately

`build_apk_x86_release.sh` and `build_apk_x86_audio.sh` also existed in the scratchpad, but
those copies are **stale**: they lack `-Wl,-z,max-page-size=16384`, the 16 KB-page alignment
fix. The current versions live at `port/build_apk_x86_release.sh` and
`port/build_apk_x86_audio.sh`. Use those; the scratchpad copies were not brought over.
