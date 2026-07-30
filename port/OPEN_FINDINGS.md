# Open findings

Things that have been **measured** but not resolved, kept separate from the docs that describe what
works so an unresolved issue is never quietly folded into a success claim. Resolved entries stay
below, with the evidence, so the same question is not re-investigated from scratch.

---

## Open

### 2. Not verifiable from this machine

Not defects — limits of the environment. Stated so they are never implied to be covered.

- **Executing the real arm64 APK.** Not a gap that better effort closes: the Android emulator
  **refuses** to run an arm64 system image on an x86_64 host — `FATAL | Avd's CPU Architecture
  'arm64' is not supported by the QEMU2 emulator on x86_64 host. System image must match the host
  architecture.` The `ab-emu-arm64` image and its `arm64-v8a` AVD exist and were built successfully,
  but the AVD can never boot, which is why nothing had ever used it. `emu_arm64_real_artifact.sh`
  is kept and now reports that in seconds. What *does* cover the arm64 side: `arm64_unicorn_test.sh`
  (qemu-**user**, which translates user-space only and therefore works) runs the engine load and all
  125 C++ constructors on AArch64, and `verify_claims.sh` checks the shipped ELF is AArch64,
  16 KB-page aligned and links libm. None of that is the game running; the shipped APK has been
  built, signed, aligned and statically audited, not executed.
- ~~**The physical Galaxy A56 run.**~~ **DONE 2026-07-30 — see R50.** It installed, played, won 3 stars / 45500 and advanced to level 2 on the real device. The rest of this bullet is the pre-run text:
   Everything is validated on emulators (API 25 and API 34, x86 with
  the shim in real ART). The A56's Exynos 1580 specifically is untested. See `ONDEVICE.md`.
- **Frame pacing under a real GPU.** All rendering evidence is SwiftShader software rendering.
  Narrowed rather than merely restated: R4 measured the rasteriser at 5–7% of frame time, and R4b
  confirmed that independently — 12.3× the pixels costs only 25% more frame time, implying a ~2%
  fill share. So the phone's GPU replaces a *small* slice and **single-thread CPU sets the frame
  rate**. What remains genuinely unknown is that CPU term on Exynos 1580 cores, not the GPU term.
- **Whether Mali/Xclipse accepts what SwiftShader accepted.** The specific residual, now that R10
  and R11 have bounded it, and the most likely way this port fails on the device:
  - the **22 screened shaders** compile here; every float declaration is precision-qualified and no
    `#version`/`#extension`/`dFdx`/`texture2DLod`/`gl_FragDepth` appears, but only the device's
    compiler decides (R10);
  - the **`GL_EXTENSIONS` answer**. The engine branches on three names; the rig advertises two of
    them and not `GL_OES_vertex_buffer_object`. If the A56 advertises that one, the engine takes a
    VBO path nothing here has run. This is now a one-command `diff` on the device against
    `reports/gl_extensions_rig.txt` — see `ONDEVICE.md` and R11.
- **Loading on a 16 KB-page device.** The shim's ELF LOAD segments are 16 KB-aligned
  (`-Wl,-z,max-page-size=16384`) and `verify_claims.sh` asserts that on every build — that is the
  documented requirement for 16 KB-page Android, and a 4 KB-only `.so` fails to `dlopen` there.
  But the alignment is what has been measured; the *loading* has not. No 16 KB-page system image
  is available on this host (API 25/34/36 x86_64 are all 4 KB, and the emulator exposes no
  page-size option), and a 16 KB arm64 image could not boot here anyway. So this is a property
  verified at the mechanism and inferred at the outcome — worth stating because every other
  'installs on modern Android' claim in these docs IS backed by a run.
- **Continuous audio on real audio hardware.** The audio variant plays through and wins on the
  proxy, but sustained playback needs real hardware.
- **Whether the app-rater ever fires.** R13: an `ACTION_VIEW`/`market://` intent is the one route out
  that no de-phone-home layer can block, it is measured at **zero** across a full playthrough, and
  raters typically gate on session count or elapsed days — which one automated run cannot represent.

---

## Resolved

### R50. IT RUNS ON THE PHONE. Every "x86 proxy" caveat in this document is now retired

The Samsung Galaxy A56 (SM-A566B) was connected over USB on 2026-07-30. The shipped artifact —
`out/angrybirds-8.0.3-arm64.apk`, sha256 `27548721a456ea99…`, the exact published bytes — was installed
with the documented `adb install -r` and launched. **It plays, it wins, and it advances.**

**The premise, confirmed on the hardware it was always about:**

```
ro.product.model       SM-A566B
ro.product.cpu.abilist arm64-v8a          <- ONLY arm64. No armeabi-v7a, no x86.
ro.build.version.release 16  (API 36)
ro.board.platform      erd8855            (Exynos 1580)
getconf PAGE_SIZE      4096
```

That `abilist` is the whole reason this project exists: the original 32-bit APK has literally nowhere
to run on this device. And the page size is 4 KB, so the 16 KB alignment work (R18) was insurance
rather than necessity — harmless, and it would have mattered had the answer been 16384.

**Android chose our AArch64 library:** `primaryCpuAbi=arm64-v8a`. The `-r` replace succeeding over a
copy installed on 27 July is also the signer proof, since Android refuses a replace across keys.

**The whole sequence, screenshotted and reviewed by eye:**

| step | evidence |
|---|---|
| splash | `PROOF_22_PHONE_splash.png` — Red, Chuck, Bomb, the pigs, the logo |
| tutorial card | `PROOF_23_PHONE_tutorial_card.png` — slingshot, pig, checkmark |
| level interactive | `PROOF_24_PHONE_level_interactive.png` — tutorial hand + drag arrows |
| **bird launched, structure destroyed** | `PROOF_25_PHONE_score15380.png` — **SCORE 15380**, pig gone |
| **LEVEL CLEARED** | `PROOF_26_PHONE_levelcleared_3stars_45500.png` — **3 stars, 45500** |
| **level 2** | `PROOF_27_PHONE_level2_fresh.png` — fresh puzzle, score 0, two pigs on towers |

**The deepest bug in this project held.** The level-end results transition is where the session-long
`std::string` use-after-free crashed, and it took the WAF-canary targeted leak plus
`neut_s_construct_null` to fix. It survived on the Exynos with `h_fatal=0` — and both fixes are
demonstrably *working*, not merely present: `s-construct-null-guard` fired **15** times and
`empty-json-guard` **29** times during the run.

**Final on-device figures — CORRECTED.** My first write-up of this entry quoted counts from a log I had
built by appending `adb logcat -s abshim` seven times **without `-T`**. Each invocation re-dumps the
whole ring buffer, so the file was **4.33× duplicated**: 35,681 lines, 8,236 unique. `capped_counts.py`
exposed it by reporting counts *above* their caps (`[u16conv]` 98 vs cap 14, `[S2] stash` 280 vs cap
40) — impossible for a per-process `static int`, so the log had to be the problem, not the shim.

Separating what survives from what does not:

| figure | first reported | actual | status |
|---|---|---|---|
| `init_array` | 125/125 | 125/125 | **sound** |
| frame heartbeats | frame[2101] | same | **sound** — distinct monotonic values cannot be inflated (`frame[5101]` is from `PHONE_fps.txt`, a *separate* capture of the same pid 11197 — see R51, not this log) |
| `h_fatal` | 0 | 0 | **sound** — a zero cannot be duplicated upward |
| `uaf-survive` | 0 | 0 | **sound** |
| `THROW St11logic_error` | 0 | 0 | **sound** |
| `nativeUpdate` | 847 | **121 unique** | **CONTAMINATED** — 847/121 ≈ 7 = the number of appends |
| `s-construct-null-guard` | 15× | **≥5** | corrected — see the caveat below, 5 is *also* a floor |
| `empty-json-guard` | 29× | **≥7** | corrected — likewise a floor |

**And de-duplication does not fully recover the guard counts.** My first correction said "actually 5"
and "actually 7", from `sort -u`. That is wrong too, for a reason worth stating: the duplicated lines are
byte-identical *including* timestamp and pid —

```
07-30 02:16:32.897 11197 11197 I abshim  : [s-construct-null-guard] empty std::string _Rep @0x50000060 …
07-30 02:16:32.897 11197 11197 I abshim  : [s-construct-null-guard] empty std::string _Rep @0x50000060 …
```

— so `sort -u` cannot distinguish a re-dumped line from a genuine second firing inside the same
millisecond, and collapses both. What is recoverable: the shim printed between 5 and 8 lines (5 unique,
and the cap is 8), so the guard fired **at least 5 times**. The exact count is not recoverable from this
log, and the cap would have hidden anything past 8 regardless. Report `≥5` and `≥7`.

**One inference is withdrawn entirely.** I wrote that tapping the tutorial card moved `nativeUpdate`
"+121, which proves the tap woke the loop". It proves nothing of the sort — +121 was the re-dump of the
same 121 lines, and it appeared identically after *every* append, which is exactly why a suspiciously
constant delta should have been checked before being used as evidence. What *does* prove the tap worked
is independent and unaffected: the screenshots show the modal card gone and the level interactive, and
the frame heartbeats advanced monotonically past it.

**The headline is unaffected.** "It installs, plays, scores 15380, clears the level with 3 stars /
45500, and advances to level 2" rests on six reviewed screenshots and eight distinct monotonic frame
heartbeats — neither of which duplication can manufacture. `uaf-survive 0` also stands, and remains
worth noting against R41/R42: on real hardware, across a full play-win-advance cycle, **zero** wild
memory accesses needed absorbing.

### R55. A third level cleared on the phone — a harder one, with the star counter as proof

R54 had to caveat its 9-star reading: the app's data predates the session, so the stars might not have
been earned there. This closes that, and pushes device play past the first tutorial levels.

**What was played.** `Tutorial_bomb_niko` — four pigs across three stone towers with a central A-frame,
fought with Bomb birds. Not the one-pig opener behind PROOF_22–27.

| | |
|---|---|
| score | **0 → 21810 → 71750 → 72290** across four bomb launches |
| result | **LEVEL CLEARED! 3 stars, 72290** (PROOF_29) |
| episode stars | **9 before → 12 after** — +3, earned in this session, unambiguously |
| process | pid **11197** throughout, age 1:26 — the same process as PROOF_22–30 |
| faults | `h_fatal 0`, `uaf-survive 0`, `St11logic_error 0`, `FATAL 0`, `signal 11` 0 |
| log | single `adb logcat -d` dump, verified monotonic, **no counter at its cap** — real totals |

PROOF_30 catches the first bomb mid-detonation: left tower rubble, the A-frame collapsing with blocks
still airborne, a pig killed, score 21810. That is the engine's rigid-body simulation running under the
ARM32→ARM64 shim on real hardware, not a static screen.

**The level-end guards did NOT fire this time — and that is informative, not worrying.** R54 measured
`s-construct-null-guard` 4 and `empty-json-guard` 2 at one level-end. At this one both are **0**. So the
guards are not a per-transition necessity: they fire only when the null-string / empty-JSON condition
actually arises (ad-config state, which with no INTERNET may already be cached). What is invariant across
both transitions is `h_fatal 0`. Reporting the 0/0 rather than quietly omitting it, because "the fix
fires every time" would have been the easier and false story.

**Two wasted runs, and the lesson is a coordinate.** The vector recorded in cont.264
(`swipe 860 365 506 473`) launches the bird on the *first* tutorial level and nowhere else:

* a level **opens with the camera on the target area**, not the slingshot, so no bird sits under a fixed
  slingshot coordinate at that moment;
* that vector is a **right-to-left** swipe, which on empty ground is not a slingshot pull at all — it
  **scrolls the camera further right**, away from the slingshot. Each cycle pushed the view further from
  the bird *while the log stayed perfectly healthy*: `h_fatal 0`, heartbeats climbing, `SCORE 0`.

Another instance of the dominant defect class — a healthy log beside a game that is not being played.
`phone_play.sh` now carries the procedure that works: scroll left-to-right **twice** to bring the
slingshot into view, pull back from the bird at **(948,667) → (700,790)**, re-scroll before every
subsequent bird (the camera follows the last one downrange), and tap fast-forward at **(2241,971)** to
settle rather than waiting out the animation.

**Four distinct levels on the device now, and the LOSS path works too.** Running the fixed
`phone_play.sh` loaded a fourth level file — `Tutorial_blues_niko` — and **lost** it: `LEVEL FAILED!`
(PROOF_31), star counter correctly unchanged at 12, `h_fatal 0`. That is worth having rather than
hiding: level-fail is a different code path from level-complete, and it survives too. The run also
showed the camera fix working — **0 of 3 captures repeated a settled screen**, against 4 of 6 before it.

Device totals: **4 distinct level files loaded, 3 cleared with 3 stars, 1 lost**, all in one process.
With camera drift eliminated, the remaining limitation really is **aim** — a single fixed drag vector
cannot aim at arbitrary geometry, which is a harness limit, not a port defect. (`lib_camera.sh`'s header
records that this was once *assumed* to be the cause when the real cause was drift; now that drift is
fixed, it is the honest residual.)

**A stale-capture defect in the same script, found while reading its own output.** `png_sane.py
"$OUT"/c*.png` globs every capture in the directory, so a 3-cycle run validated `c05`/`c06` images left
behind by the previous 6-cycle run and reported on frames it never took. Old evidence presented as
current is exactly the stale-PROOF defect this repo has been bitten by twice. The script now clears
`$OUT/c*.png` before it starts.

**Two of that script's own assertions were unsound and failed a healthy run.** Both are fixed and the
reasoning is recorded in the file: a per-cycle "heartbeats must advance" check is invalid because
`frame[N]` fires every 300 frames (R38) and the device runs at ~6 fps (R51/R54), so one heartbeat takes
~50 s while a cycle takes ~38 s — a cycle containing zero heartbeats is normal. And "consecutive
screenshots must differ" is invalid because a settled results card legitimately produces byte-identical
PNGs. Both are now asserted across the whole run, where the window is long enough to mean something.

### R54. A 68-minute single-process soak on the A56 — a second win, honest counts, and R42's leak question answered

While correcting R50 I checked whether the phone was still attached and found **the app still running as
pid 11197** — the same process that produced PROOF_22–27, alive **68 minutes**, still the focused window.
That is an unplanned but genuine long-duration soak on the target hardware, and this time the log was
captured correctly: **one** `adb logcat -d` dump, no appending.

**The log is provably clean.** 26,142 lines, **26,142 unique**; `log_recapture_audit.py` reports it
monotonic; `capped_counts.py` finds **no counter at its cap**. So unlike `PROOF_PHONE_abshim.txt`, every
count below is a real total rather than a floor — which matters, because R50 had to report `≥5`/`≥7` and
say the true values were unrecoverable. From a good capture they are recoverable:

| in the clean window 02:34:09 → 03:10:11 | count | |
|---|---|---|
| `s-construct-null-guard` | **4** | first honest per-level-end count on real hardware |
| `empty-json-guard` | **2** | ″ |
| `h_fatal` | **0** | |
| `uaf-survive` | **0** | |
| `THROW St11logic_error` | **0** | |
| `FATAL`, `signal 11` | **0** | |

Both guards fired at a single timestamp — **02:34:35** — i.e. one level-end transition. That transition
is the deepest bug's crash site (the session-long `std::string` UAF), and it was traversed on real
silicon with zero faults, for the **second** time.

**A second, distinct win.** `PROOF_28`: `LEVEL CLEARED!` 3 stars, **56010** — PROOF_26 was 45500, so this
is not the same screen re-photographed — with an episode total of **9 stars**, at `frame[22801]`. Two
distinct level files are named in the clean window: **`Tutorial_chuck_niko`** and
**`Tutorial_red2_niko`**. *Honest caveat:* the app's data predates this session (`firstInstallTime`
2026-07-27), so the 9-star total may include earlier progress and I am not claiming all nine were earned
here. What is unambiguous is the distinct score, the two named level files, and the clean traversal.

**Frame rate during this window.** 12,300 frames over 2,139 s = **5.75 fps** average; across 41 windows
of 300 frames, **min 2.71 / median 6.20 / max 6.66**. ⚠️ **This was first written as "unattended, no
input" — that is WRONG (see R56). The user was playing the game on the device during this window.** The
numbers are valid as frame pacing; they are NOT an idle measurement, and the reasoning I attached to the
difference from R51 was built on a false premise.

**R42's leak question, answered by measurement.** R42 corrected "bounded ~64 tiny `_Rep`s" (a log cap) to
a sustained 7–16 blocks/minute, leaving open whether that accumulates. Ten `dumpsys meminfo` samples at
60 s intervals on this process:

```
PSS          spread 466 kB over 10 min,  slope -30.9 kB/min  (declining)
Native Heap  sawtooth: +39.6 then +44.2 kB/min, with a -2,570 kB release between samples 4 and 5
```

A single linear fit over the native-heap series returns −19.7 MB/hour, which is an artifact of that
release step, not a trend — so the segments are reported separately rather than fitted as one line.
R42's predicted leak is ~30 kB/hour, **an order of magnitude below the 466 kB noise band**, and PSS is
flat-to-declining across 68 minutes. **The targeted leak is real but does not accumulate measurably.**
It is a correctness footnote, not an operational risk. Nothing to fix.

The `.txt` proofs are now covered by the sha256 manifest as well as the images (previously `*.png` only),
mutation-tested in both directions: tampering with `PROOF_PHONE_soak_clean.txt` fails the suite, and
restoring it returns 50/50.

### R56. Performance, root-caused with measurement — and R54/R55's framing corrected

The user played the game on the A56 during this session and reported what I had failed to look for:
**~10 s of latency for every interaction**, and performance "an order of magnitude too slow in every
measure". Both are real. I had measured frame rate, called it "playable and honestly so", and never
measured latency at all — the symptom instead of the system.

**Measured latency: 7.61 s** from `input tap` to the first changed pixel (polled screenshots, 0.6 s
resolution). Not a frame-rate artifact — 60 fps → 6 fps turns 16 ms into 160 ms, not 7.6 seconds.

**Where the time goes.**

| measurement | result |
|---|---|
| `GraphicsThread` CPU | **97% of ONE core** (975 jiffies / 10.05 s, CLK_TCK=100), everything else ≤1% |
| cores on the phone | **8** — so the app uses ~1 and leaves 7 idle |
| TCG throughput, real engine code | **42 Minsn/s** on the x86 host (125 ctors = 2.27 Minsn in 0.0542 s) |
| estimated A56 | ~17 Minsn/s chained, ~11 Minsn/s as actually configured |
| predicted frame time | 1–2 Minsn/frame ÷ 11–17 Minsn/s → 60–180 ms → **6–17 fps** |
| observed | **6–8 fps** — the prediction holds |

So the frame rate is *fully explained*: one core, and Unicorn TCG at ~42 Minsn/s. This is architectural —
the GEL means exactly one host thread ever drives the shared engine.

**A concrete 1.56× left on the table, measured not guessed.** `run_loop` preempts green threads by
passing `SCHED_QUANTUM` (200000) as `uc_emu_start`'s **count** argument. Unicorn implements a non-zero
count with an internal per-instruction mechanism that defeats TB chaining. Identical work, only the
count differing:

```
count=0    (chained)  : 0.0545 s   125/125 ctors
count=1e9  (counting) : 0.0849 s   125/125 ctors     <- never exhausted, so same work
=> the counting mechanism alone costs x1.56 (55.7%)
```

The ctors run with `budget=0`, which is why `test_perf2` reports the optimistic 42 Minsn/s — but the
*game* pays the 1.56× on every slice. Preempting some other way (a timer thread calling `uc_emu_stop`,
or ending a slice at a bridge boundary, which already happens thousands of times per second) recovers
it. That is ~6–8 fps → ~9–12 fps and a proportional cut in latency. Not 60 fps; a real 1.5×.

**Wasted work inside that one saturated core: a retry loop my own de-phone-home created.** The clean
36-minute soak log is **95.5% `[fopen]` lines** — 24,976 of 26,141:

```
12449  [fopen] '/dev/urandom' (rb) -> ok
12447  [fopen] '/etc/hosts'   (r)  -> ok
```

That is ~8 name-resolution attempts **per second, forever**, while the game sat on a results screen —
and 61 of them ran during the measured 7.6 s input delay. `dispatch.c`'s own hot-stub comment already
named the mechanism: *"a guest that spins calling a bridged import (observed: libcurl's connect/poll
retry loop after we cut the network)"*. Layer 2 hard-fails the socket, so the bundled networking stack
retries indefinitely. **The rig had already observed this and I had not connected it to performance.**

**What is NOT the bottleneck** — ruled out by measurement, so nobody re-chases it:

* **Bridge crossings.** ~770 ns per bridge call (already measured in this project) × ~7,500 malloc/free
  crossings/s (from the shipping `[hot-stub]` counters: 10.0M `free`, 6.3M `malloc` in 36 min) = **0.6%
  of one core.**
* **The DIAG uc-hooks.** I suspected these and was wrong. The pure-log diagnostics are
  `#ifndef ABSHIM_RELEASE`-gated (four blocks), and the all-block `UC_HOOK_BLOCK` — which would have
  been catastrophic — is behind `ABSHIM_HEAVY_DIAG`. ~27 hooks ship and nearly all are functional
  `neut_*`/`guard_*`; `diag_memcpy` is a guard that clamps a corrupt length, not a probe.
* **Memory mapping.** `uc_mem_map` throughout (real host RAM), not `uc_mmio_map` (which would trap every
  guest access). Correct already.
* **Logging cost.** 12 lines/s of `__android_log_write` ≈ 0.04% of frame time. The uncapped `[fopen]`
  logging is a *release-hygiene* defect, not a speed one.

**No TCG tuning exists at all** — no `uc_ctl` call anywhere in the shim. Unicorn defaults throughout.

**Corrections to R54/R55 forced by the user's disclosure.**

* R54 called its 5.75 fps window "**unattended, no input**". That is **wrong** — the user was playing the
  game during it. It is a frame-pacing measurement, not an idle one.
* R54's second win (56010 at 02:34:35) is most likely **the user's play**, not a side effect of my drag
  vectors. R54 attributed it to the session's automation by implication; withdrawn.
* "Playable, and honestly so" (R51) is withdrawn. 3 stars can be won at this speed, but 7.6 s
  interaction latency is not playable, and calling it so was exactly the verdict-outrunning-evidence
  pattern this file exists to record.

### R57. 3.9% of the phone's screen is unused — the display-cutout letterbox (corrected)

> ⚠️ **This entry first claimed 9.7% and two causes. Measured from the actual pixels, it is 3.9% and
> ONE cause.** `mAppBounds` reserves the navigation bar, but the app *draws under it*, so those 135 px
> are not lost. Reading a window-manager inset as lost pixels was an inference; the screenshots are the
> evidence. Measured across three independent device captures, at the vertical midpoint:
>
> ```
> PHONE_soak_now.png   2340x1080  black-left=92px  black-right=0px  used=2248px (96.1%)
> scroll_left.png      2340x1080  black-left=92px  black-right=0px  used=2248px (96.1%)
> launch1.png          2340x1080  black-left=92px  black-right=0px  used=2248px (96.1%)
> ```
>
> The 92 px is exactly the cutout inset, and it cross-checks against `dumpsys`'s own
> `Requested w=2248` (= 2340 − 92). So the user's description — "the entire game is shifted right such
> that the bezel area is completely black" — is precisely right: a 92 px black band on the left, nothing
> on the right.

The user pointed out the game is shifted right with a dead black band. Measured from `dumpsys window`:

```
panel   mBounds    = Rect(0, 0 - 2340, 1080)     2340 px wide
app     mAppBounds = Rect(92, 0 - 2205, 1080)    2113 px  -> 227 px (9.7%) unused
cutout  insets     = Rect(0, 92 - 0, 0)          punch-hole camera; becomes the LEFT edge at ROTATION_90
right   2340-2205  = 135 px                      = 48dp navigation bar at density 2.8125
```

**One cause: the 92 px cutout letterbox.** The app targets SDK 26 and never declares
`android:layoutInDisplayCutoutMode`, so Android denies it the cutout region in landscape. Raising
targetSdk does **not** fix this — the platform default is still "never into the cutout in landscape".
The fix is to declare `shortEdges`.

**Why that is not a one-line patch here.** It is an AXML attribute **insertion**, not a value rewrite,
and this project's three existing manifest tools (`depermission.py`, `manifest_firebase_off.py`,
`patch_minsdk.py`) all change values at constant length. An insertion must: add the attribute record
(20 bytes) to the `<activity>` element, bump its `attributeCount`, grow the chunk size and the file
header — and, the hard part, add the name string `layoutInDisplayCutoutMode` to the string pool **at an
index inside the XML resource map**, because Android resolves a framework attribute to its resource ID
through `resourceMap[nameIndex]`. A string appended at the end of the pool lands beyond the map and the
platform's `TypedArray`-based manifest parser would never see the attribute. Doing it correctly means
inserting into the pool and renumbering every string reference in the file.

**The verification is objective, which makes the work worth doing**: after installing, `dumpsys window`
must report `mAppBounds=Rect(0, 0 - 2340, 1080)` and a capture must show `black-left=0px`.

Both are window-level properties. A third route avoids AXML insertion entirely: the shim holds the
`JavaVM` from `JNI_OnLoad`, so it could set both from native code via JNI on the UI thread
(`getWindow().getAttributes().layoutInDisplayCutoutMode = SHORT_EDGES`, plus immersive flags). Not yet
implemented — recorded here with the mechanism identified and measured, not fixed.

### R58. Three corrections to R51/R54/R56, and a first-launch cost nobody had measured

Trying to *fix* the resolver loop produced better measurements than finding it did, and they invalidate
several things I wrote earlier today.

**1. `frame[N]` is not a general fps signal.** On the validated build, on an interactive level, the
heartbeat sat at `frame[1]` across a **180-second** window while the game was demonstrably alive — a tap
changed the screen 1.66 s later. `frame[N]` is `if((++rf % 300u)==1u)` and is *not* log-capped, so this
is real: fewer than 300 render frames in three minutes on a static scene.

So every fps number derived from heartbeat deltas — R51's "~7.10 fps post-warmup", R54's "5.75 fps" —
describes **only the intervals in which the counter was advancing**, not the game's general frame rate.
Those intervals were periods with animation (settling physics, the animated results screen). The
measurements are not wrong about those windows; the *generalisation* to "the A56 runs this at ~7 fps" is
not supported. R38 established that `frame[N]` is a heartbeat and not a counter; I then used it as a rate
meter anyway.

**2. Interaction latency is state-dependent, and 7.6 s was not a constant.** Measured twice, same device,
same build family:

| state | latency |
|---|---|
| 2-hour-old process, resolver loop active at ~8 opens/sec | **7.61 s** |
| fresh process, `/etc/hosts` opens still 0 | **1.66 s** |

That is consistent with the resolver loop being the cause, and it is **not proof** — process age, shader
cache and scene differ too. R56 named the loop as the leading explanation; this adds a second data point
in the same direction without closing it.

**3. First launch after a fresh install costs ~10 minutes, and both builds pay it.** Not previously
measured. After `adb install -r` + launch, **10:09 elapsed** before the game reached a playable level, and
`frame[301]` was still not reached. R51 recorded the warm equivalent as 352 s at 0.85 fps and attributed
it to first-run shader compilation; from a genuinely cold install it is roughly twice that. This is
probably a large part of what "abysmal" means in practice, and it is invisible to every existing check.

**The fix attempt, and why its verdict was void.** I added a scheduler-yielding backoff to the resolver
path in `bridge_file.c` (the proven `l_poll` remedy for the same GEL-starvation shape), built it, and
deployed. It showed `frame[1]` after 10:32 and I called it a regression. **That conclusion was invalid:**
I compared a *fresh-install* run against R51's *warm-install* baseline. Re-running the **validated**
build from an equally fresh install gave `frame[1]` at **10:09** — statistically the same. So the
throttle was neither shown to help nor shown to hurt.

Two further defects in that attempt, both mine:

* **The diagnostic I added to observe the fix couldn't observe it.** My log cap counted `/etc/hosts` and
  `/dev/urandom` against one shared counter, so eight early `/dev/urandom` opens exhausted it and every
  subsequent `/etc/hosts` open went unlogged. `grep -c '/etc/hosts'` then returned 0 — which I nearly
  read as "the loop is gone". A counter shared between the thing you are measuring and a much more
  frequent neighbour measures the neighbour.
* Deploying to the phone as the *first* test, when the loop reproduces in every x86 emulator log
  (`progression_abshim.txt`: 26,701 of 59,812 lines) and would have iterated far faster there.

**State:** reverted; the tree rebuilds byte-identical to `27548721a456ea99295469c3` and the phone is back
on the validated build with its save intact (`HIGHSCORE 45500` reloaded). The patch is kept at
`resolver_throttle_attempt.patch` for a retry that (a) fixes the counter, (b) throttles only a genuine
spin rather than any resolver read, (c) bounds the added delay, and (d) is measured on x86 first.

### R60. How NOT to get a framework attr ID offline — four dead ends, and the route they point to

R57's fix needs `android:layoutInDisplayCutoutMode` in the manifest, and an AXML insertion needs that
attribute's **resource ID**, because Android resolves a framework attribute through
`resourceMap[nameIndex]`. Guessing a constant is not acceptable, so I went looking for an authoritative
offline source. All four attempts failed, and the failures are worth recording so nobody repeats them:

1. **The phone's own framework — `framework-res.apk` (API 36, pulled over USB, 38 MB).** Its
   `resources.arsc` is 28.9 MB and contains the string `layoutInDisplayCutoutMode` **zero** times, in
   either UTF-8 or UTF-16. Modern framework-res strips resource *names*; the IDs are compiled into
   callers. A parser found the `android` package (id=1, 19,710 keyStrings) and the name simply is not
   there. **The device cannot tell you an attr name → ID mapping.**
2. **`aapt` / `aapt2` / `apktool` in `ab-port`.** None present — the image has `zipalign`, `apksigner`,
   `javap`, `unzip`, `python3` and nothing else from build-tools.
3. **`android.jar` (which carries `R$attr` constants that `javap` could dump).** Absent from `ab-port`
   and from every `ab-emu*` image — those are system-image-only SDK installs with no `platforms/` tree.
4. **On-device APKs that already declare it.** Scanned 14 (camera, YouTube, Settings, GMS, gallery,
   video, notes, games, launchers): **none** declares `android:layoutInDisplayCutoutMode` in its
   manifest. That is not bad luck — real apps set **`windowLayoutInDisplayCutoutMode` in a theme/style**,
   not the manifest attribute. So manifest-scanning was the wrong source from the start.

**Where that leaves the fix, and it is a better place.** The dead ends all stem from needing the raw
constant — which is only necessary because I was planning to hand-write the AXML insertion. A real
manifest tool (`aapt2`, or `apktool`, a single jar) **resolves the attribute name to its ID itself**, and
also handles the string-pool insertion and resource-map renumbering that make hand-rolling risky. So the
right move is to put such a tool into the toolchain image as a committed input — exactly how Unicorn is
already vendored — rather than to reimplement AXML surgery.

That keeps the build offline (`--network none`) with the tool as a pre-staged input, keeps
reproducibility (same input → same output), and removes the need to know the constant at all. The
trade-off to accept consciously: a re-encoded manifest will not be byte-identical to Rovio's original,
only semantically equivalent — which is a real change to the "payloads are byte-for-byte authentic"
claim's *scope* (it covers the payload `.so`s, not the manifest, which is already rewritten by
`depermission.py` and friends).

Verification stays objective either way: `mAppBounds` must become `Rect(0, 0 - 2340, 1080)` and a
capture must show `black-left=0px`.

### R59. The count fix is worth **11–14×**, not 2.75× — and it is reverted, because it is not yet correct

> ⚠️ **THE HEADLINE NUMBER IN THIS ENTRY WAS WRONG AND TOO SMALL.** I first reported 2.75× by comparing
> raw frame counts between runs of **different durations** — the baseline spends ~9 minutes booting and
> renders few frames in that time, so counting total frames hid the effect. The comparable datum is the
> script's own `card at ~Ns` line, which is the time to reach the tutorial card (`frame[601]`) under
> identical fixed waits:
>
> | run | time to tutorial card |
> |---|---|
> | slice fix v1, run 1 | **40 s** |
> | slice fix v1, run 2 | **40 s** |
> | preempt-before-bridge, run 3 | **50 s** |
> | **baseline, contemporaneous, reverted source** | **565 s** |
>
> **A THIRD, CLEANER DERIVATION — and it refines the number down.** `startup_report()` (new, in
> `lib_metrics.sh`) computes time-to-`frame[601]` from the abshim log's **own timestamps**, so it measures
> only shim-attributable time and excludes Android boot/install:
>
> | | shim-log time to playable |
> |---|---|
> | baseline | **300 s** |
> | fix run 1 | **39 s** → **7.7×** |
> | fix run 3 | **49 s** → **6.1×** |
>
> So the honest figure for what this change actually affects is **~6–8×**; the 11–14× from the script's
> `card at ~Ns` line includes emulator boot and install, which the change does not touch. Both are large,
> and the smaller one is the one to quote.
>
> **11–14× faster to first playable** (script wall-clock). Cross-checked independently against frame rate: baseline 1201
> frames over ~665 s ≈ **1.8 fps**; fix 3301 frames over ~140 s ≈ **23.6 fps** — ~13×, which agrees with
> the boot figure derived a completely different way. It also explains R58's unexplained "fresh install
> takes ~10 minutes to become playable": that *is* the 565 s, and this fix would cut it to under a minute.
>
> Everything below about the defects and the revert still stands. What changes is the stakes: this is
> not a marginal 1.5× worth deferring, it is the difference between a 9-minute cold start and a 40-second
> one, and between ~2 fps and ~20 fps. It should be finished.


R56 found that `run_loop` bounded every slice by passing `SCHED_QUANTUM` as `uc_emu_start`'s **count**,
and measured that mechanism at **×1.56** on identical work. This is what happened when I actually
changed it.

**The speedup is real, larger than predicted, and reproduced three times.** Same script, same image,
same fixed sleeps:

| run | `h_fatal` | `St9bad_alloc` | frames reached | THROWs |
|---|---|---|---|---|
| baseline (pre-change) | 0 | 0 | `frame[1201]` | 15, all `IOException` from `+0x6d8524` |
| 1 — wall-clock slice | **1** | **1** | **`frame[3301]`** | 2 |
| 2 — identical build | **1** | **1** | **`frame[3301]`** | identical → **deterministic, not the known flakiness** |
| 3 — preempt *before* bridge | **1** | 0 | **`frame[3301]`** | 2, now `GraphicsException` |

`frame[3301]` against `frame[1201]` in the same wall time is **~2.75× more frames**, well above the
1.56× the isolated mechanism measurement predicted. That is the largest performance result in this
project and it should not be lost.

**Two Unicorn behaviours, established by standalone test rather than by reasoning.** Both are reusable
facts about the emulator, and one was the bug:

1. **`uc_emu_start` DOES clear a pending stop request.** So an async stop that lands after a slice has
   already ended cannot truncate the next one (`r0=4, pc=0x1010` after a deliberate pending stop). My
   design depended on this and I had assumed it; it holds.
2. **`uc_emu_stop` does NOT return from a hook callback, and the hook RE-FIRES at the same address on
   resume.** Measured: fire #2 at `0x1004`, stop, resume, fire #3 at `0x1004` — while each instruction
   still retires exactly once. Harmless for an ordinary hook. **Fatal here**, because the shim's stub
   callback *is* the bridge implementation: `slice_check` raised the stop and then fell through into the
   bridge, which ran, and ran **again** on resume. `malloc`/`free` are not idempotent, so every
   preempted call leaked one allocation — ending in `THROW St9bad_alloc` and a changed exception pattern
   (1 `IOException` from a *different* site instead of the baseline's 15).

Preempting **before** any bridge side effect is the correct point, and run 3 confirms it: `bad_alloc`
went 1 → 0. Two further defects of mine in that same fix:

* `g_in_stub = 1` at the top of a body with ~100 early-return fast paths would **never be cleared**,
  permanently disabling the timer after the first bridge call. The guard has to live in a wrapper that
  covers every exit, and the release variant had no wrapper at all, so one had to be added.
* The first version relied on bridge boundaries alone, and I wrote the gap off as "a longer slice, not a
  hang". `run_tests.sh`'s **`[T3] preemption of a non-yielding spin-loop` hung outright** — guest code
  that calls no bridge has no boundary to check at. The hard instruction count had been bounding exactly
  that case. A 2 ms timer thread is the second, independent bound.

**Why it is reverted.** Run 3 still leaves one `h_fatal` — `gr::GraphicsException` at `+0x5c4b54` during
`nativeInit` — and the end screen is not a win. Changing preemption points changes thread interleaving,
so a GL-path race is plausible, and that needs a **contemporaneous** baseline control rather than a
comparison against a log recorded hours earlier on a machine whose container set I had since disturbed.

Worse, committing it broke the repo's central invariant: the tree was building `bfe4ea0a` while the docs,
the screenshot index and `verify_claims` all name `27548721a456ea99295469c3`. Restoring the three files
to their state at `54214f0` rebuilds `27548721…` exactly, and `ALL CHECKED CLAIMS HOLD` again.

**THE CONTEMPORANEOUS CONTROL SETTLED IT, AND THE ROOT CAUSE IS IDENTIFIED.** Re-running the reverted
source on the same machine, same APK (`c5911a79`), while the arm64 suite ran alongside (load 3.69 on 24
cores, so not confounded):

```
BASELINE control: all 125 ctors OK, frame[1201], h_fatal 0, St9bad_alloc 0,
                  GraphicsException 0, win check: WIN CONFIRMED from pixels, DONE (FAIL=0)
```

Clean. So the environment is fine and **the fix genuinely introduced the `GraphicsException`** — not
flakiness, not the container disruption. The revert was right.

**Root cause: my `g_in_stub` guard covered only ONE of four callback families.** The proven hazard is
that `uc_emu_stop` makes a `UC_HOOK_CODE` callback re-fire at the same address on resume. I guarded the
stub hook and left exposed:

| callback | what a re-fire would double-execute |
|---|---|
| `jni_hook_cb` (`UC_HOOK_CODE` over `RG_JNI`, `jni_passthrough.c:169`) | **a real JNI call into ART** — which is where GL/surface calls go. A doubled surface call is a very plausible `gr::GraphicsException`. |
| `h_svc` (`UC_HOOK_INTR`, `dispatch.c:735`) | a guest syscall |
| the ~27 `neut_*`/`guard_*` hooks in `jni_entry.c` | a neutralisation or guard action |

The 2 ms timer thread can land inside any of them.

**Design for the next attempt — remove the async stop entirely rather than guard 30 call sites.**
Adaptive preemption: keep `count=0` plus the precise, already-correct bridge-boundary preemption for the
common case (the engine makes tens of thousands of bridge calls per second, so slices end promptly); and
when a slice *overruns* because the thread reached no bridge at all, have the scheduler use a **bounded
count for that thread's next start only**. The pathological case then pays the 1.56× and nothing else
does, and there is no asynchronous stop anywhere — so the re-fire hazard cannot exist. `[T3] non-yielding
spin-loop` is exactly the case the fallback serves, so the test already covers it.

**Preserved for the next attempt** so it starts from the measurements instead of repeating them:
`port/validation/slice_preempt_attempt.patch`, plus the three run logs. What that attempt needs:
a contemporaneous baseline control, the `GraphicsException` root-caused (not assumed to be a race), and
the arm64 cross suite re-run against the fixed code — the run I started covered the *pre*-fix tree.

### R52. Detecting the appended-capture defect structurally, not statistically

R50's numbers were wrong because a log was seven `adb logcat` captures appended together. The obvious
guard — flag files whose total/unique line ratio is high — was **measured across all 61 committed logs
and rejected**:

| log | ratio | verdict |
|---|---|---|
| `PROOF_PHONE_abshim.txt` | 4.33× | genuinely an appended re-dump |
| `dump_api36.txt` | 2.43× | **honest** — a `dumpsys package` dump where 91 activities print identical boilerplate and `reason: assuming delivered` occurs 1052× |
| median of all 61 | 1.01× | |

Structural repetition and re-dump duplication are indistinguishable by ratio, so a ratio gate either
misses real cases or fails honest files. The detector used instead is structural: **a logcat capture is
monotonic in time, and appending a second capture restarts the clock, so timestamps jump backwards.**

```
PROOF_PHONE_abshim.txt      6 backward jumps > 2s (worst 639.8s)   <- 6 jumps == 7 appends
all 49 other timestamped logs   0
```

Clean separation with nothing in between, so no threshold needed tuning. The 6 jumps independently
confirm the 7 appends that `847 / 121 = 7` implied. `port/validation/log_recapture_audit.py` +
`test_recapture.sh` (7 cases), wired into `verify_claims.sh` (now 50 checks) and `validate_all.sh`.

The known-bad log is **kept as captured, not de-duplicated** — rewriting raw evidence to make a check
pass is worse than the defect. It is pinned *by jump count*, and the pin is tested in both directions:
de-duplicating the file fires it, and appending an eighth capture fires it. An allowlist that merely
names a file is a check that cannot fail.

### R53. The tool that exists to catch floors was dropping one of its own

While correcting R50 I ran `saturated_report` on the phone log and it named 4 saturated sites.
`capped_counts.py` on the same log reported 10. The missing one:

```
"[empty-json-guard] empty JSON parse -> '{}' (prevents the level-end ParseError->Lua-panic exit)"
 ^ Python repr() switches to DOUBLE quotes when the string itself contains an apostrophe
```

`saturated_report` scraped the human report with a shell regex for a **single**-quoted marker, so that
one never matched. The consequence is the precise inversion of the tool's purpose: a floor that is not
reported reads as *"this count is a real total"*. On the phone log it hid `empty-json-guard` at 22 hits
against a cap of 8 — one of the two counts R50 had already got wrong.

Two further defects in the same function: the count and the printed list were filtered by **two
different regexes** (`do_call ENTER|do_call RET` when counting, `do_call` when printing), so the number
could contradict the list it introduced; and the list printed whole format strings rather than tags,
which made two distinct sites sharing a tag look like one site listed twice.

Fixed by giving the tool a machine interface — `--tags` emitting `TAG⇥COUNT⇥CAP⇥SITE`, tags extracted
in Python where the format string is known exactly — plus a **parity assertion**: if `--tags` and the
human report disagree on how many floors exist, `saturated_report` refuses to report at all rather than
print the shorter number.

**And the test written to pin this passed vacuously first.** Its fixtures were marker strings copied
out of `--list`, which truncates to 70 characters for display, so they were *prefixes* of the real
format strings and matched nothing. Two cases were negative assertions ("this must not be flagged")
evaluated against a log the tool could not see at all — true of any file, including an empty one.
Fixtures now come from `--markers` (untruncated), and every negative assertion is two-sided: it first
proves the tool *sees* the marker, then proves it is not classified as a floor.
`test_capped.sh`, 9 cases.

### R51. The A56 runs it at ~7 fps — which confirms R4/R4b and refutes the optimism in the record

> ⚠️ **SCOPE CORRECTED BY R58.** These figures are valid for the windows measured, but `frame[N]` does
> **not** advance on a static scene (measured: `frame[1]` unchanged across 180 s on a live, responsive
> level). So this is the frame rate *while the render loop was being driven*, not a general
> "the A56 runs this at ~7 fps". Do not generalise it.

The last device-only unknown. R4/R4b measured the rasteriser at 5–7% of frame time and concluded
**single-thread CPU sets the frame rate**, leaving the Exynos 1580 CPU term as the open question.
The record then carried an optimistic gloss — that emulator frame rates were "an emulator artifact"
and "the A56's real silicon is far faster".

Measured from timestamped 300-frame heartbeats, keeping the FIRST sighting of each distinct frame
number so the R50 duplication cannot affect it. **Every** window in the run is listed, because quoting
only the fast ones is the same defect in a new costume:

| window | frames | seconds | fps | |
|---|---|---|---|---|
| frame[1] → [301] | 300 | 352.6 | **0.85** | first-run shader compilation (one-time) |
| frame[301] → [601] | 300 | 34.6 | **8.68** | steady play |
| frame[601] → [901] | 300 | 36.6 | **8.20** | steady play |
| frame[901] → [1201] | 300 | 90.0 | **3.33** | interactive: screenshots, drags, modal cards |
| frame[1201] → [1501] | 300 | 45.9 | **6.54** | ″ |
| frame[1501] → [1801] | 300 | 62.3 | **4.81** | ″ |
| frame[1801] → [2101] | 300 | 36.6 | **8.19** | steady play |
| frame[2101] → [5101] | 3000 | 369.8 | **8.11** | long unattended window |

**Headline: ~7 fps.** Excluding only the one-time shader-compile window, the whole post-warmup run is
**4800 frames / 676 s = 7.10 fps**. The steady-state ceiling is **~8.1–8.7 fps**, reached in four
separate windows including an unattended 3000-frame one. The interior 3.3–6.5 windows are real and are
not excluded as artifacts — they are what the frame rate does while a human is interacting with it.

The last row spans two capture files (`PHONE_abshim.txt` → `PHONE_fps.txt`). That is sound and was
checked rather than assumed: both lines carry **pid 11197, tid 11275** — the same process and the same
render thread on one device clock, so the 369.8 s interval is a real interval.

So the optimism was wrong: the phone is **not** dramatically faster than the SwiftShader rig (~2–16 fps).
That is precisely what R4/R4b predicted — if the rasteriser is 5–7% of frame time, replacing a software
rasteriser with a real Mali GPU cannot buy much, because the cost is Unicorn interpreting ARM32 on one
thread. The real GPU confirms the diagnosis rather than rescuing the frame rate.

**Playable, and honestly so:** the level was cleared with 3 stars at this frame rate, so ~8 fps is
sufficient for this game's slingshot-and-settle pacing. It is choppy, not broken. Anyone expecting 60 fps
from a 32-bit ARM binary emulated instruction-by-instruction on an AArch64-only phone should recalibrate
against this number.



**Clause 3 on the real device:** `dumpsys package` reports **0** `android.permission.INTERNET`
entries. The permission is genuinely absent on the phone, so the kernel refuses socket creation
outright — the foundation the other three de-phone-home layers sit on.

**Two corrections I made to myself during the run**, both worth keeping:

- I reported the game "stuck at `frame[1]`" after 150 s. It was not. `frame[N]` is a heartbeat every
  300 frames — **my own R38 finding** — and I walked straight into it. The real liveness signal was
  `nativeUpdate` climbing 242 → 363 → 484 → 605 → 847.
- I then read a `nativeUpdate` plateau at 363 as a stall. It was the game **idling for input**: the
  tutorial card is modal. Tapping it moved the counter +121 immediately, which is how the diagnosis
  was confirmed rather than assumed.

Also: `win_detect.py` scored `PROOF_26` as "not a win screen" despite it plainly reading
**LEVEL CLEARED! 3 stars 45500**. Not a game defect — the detector's thresholds were calibrated on
640×320 emulator captures, and the phone's 2340×1080 layout puts the dimmed panel and letterboxing
somewhere else entirely. The detector is emulator-calibrated and should not be pointed at device
captures without recalibration.



### R38. `frame[N]` is a heartbeat every 300 frames, not a frame counter — so two captures cannot be made frame-exact

Re-running `emu_interactive_capture.sh` after wiring assertions into it produced a pass, three real
640x320 frames, a live app and `h_fatal 0` — and the images were **the wrong moments**. The capture
named *bird_launched* shows the bird already impacting (score popup 5000, debris flying, score 8420);
the one named *mid-flight* shows settled wreckage and the tutorial hand (9710). Real gameplay, wrong
instants. The mechanical check passed and the human check did not, which is exactly the division that
script's output asks for.

The cause looked obvious: those two captures still use `sleep 2` and `sleep 3` after the slingshot
release, wall-clock against a frame rate measured at ~2–16 fps — the same defect already fixed twice
in this tree (`sleep 14` → `settle_frames`). The obvious fix was to wait on **frames** instead.

**The obvious fix does not work, and measuring is what showed it.** `emu_launch_timing.sh` captured a
burst at +2/4/6/9/13/18/25 frames after release in one run:

```
  frame at release: 901
  +2 frames -> frame[1201]      +9  frames -> frame[1201]
  +4 frames -> frame[1201]      +13 frames -> frame[1201]
  +6 frames -> frame[1201]      +18 frames -> frame[1201]   +25 frames -> frame[1201]
```

Every offset landed on the same frame, and six of the seven files were byte-identical in size. The
shim explains it:

```c
jni_entry.c:664   if((++rf % 300u)==1u){ LOG("frame[%u] GL draws=%lu ...
```

`frame[N]` is a **heartbeat emitted every 300th frame**. The only values logged all run were
1, 301, 601, 901, 1201 — every gap exactly 300. A frame-based wait therefore cannot resolve anything
shorter than 300 frames, and a bird's flight is far shorter than that: `+2 frames` and `+25 frames`
are the same instruction, *wait for the next heartbeat*.

**Left as-is, deliberately.** Making these captures frame-exact would mean logging every frame in the
shim — a change to shipping code and roughly 300× the log volume, to improve a diagnostic capture.
Not worth it. So the two captures stay wall-clock, stay timing-sensitive, and the script says so in
its own output and requires a human to look before anything is promoted to a `PROOF_` name. Nothing
from this run was promoted; **PROOF_2/3/4 remain the hand-driven originals**, which are separate files.

This does not affect `settle_frames`: it only ever needs "one more heartbeat", which is what a +120
request gets.

### R37. The one verdict that invalidates a whole run was the one verdict that could not fail it

`lib_selfhash.sh` exists because a script edited *while a container is executing it* is a real hazard
here: bash reads a script by byte offset, so an edit mid-run can make it re-enter or skip whole
sections while the output still looks normal. `selfhash_verify` returns **0** unchanged, **1** edited
mid-run, **2** cannot tell, and prints:

```
*** SCRIPT CHANGED WHILE RUNNING — DISCARD THESE RESULTS ***
    the output above may look normal and mean nothing. Re-run.
```

**Seventeen scripts called it and discarded the return.** Fourteen of those already had a `FAIL`
variable and a `DONE (FAIL=n)` line — they asserted carefully on everything else, and then ignored
the single result that says *none of the above counts*. A run that told the reader to discard its
own output still exited 0.

Two of them were worse than ignoring it: `emu_playthrough.sh` and `emu_playthrough_release.sh` called
`selfhash_verify` **after** `echo DONE` and **after** killing the emulator, so the warning appeared
below the line a reader stops at.

Now folded into the failure count everywhere, and proven by construction rather than by inspection —
a script that appends one line to itself mid-run:

```
  unchanged:            script identity: unchanged during the run (a8e06619…)   DONE (FAIL=0)  rc=0
  edited while running: *** SCRIPT CHANGED WHILE RUNNING ***                    DONE (FAIL=1)  rc=1
```

The same sweep found the last four `win_check` callers with the same shape, including two with no
failure variable at all and three where a failed install was an `exit 0` path. Every computed verdict
in the suite now feeds an exit status; the one deliberate exception is `emu_16k_pagesize.sh`, which
discards its `win_check` explicitly (`>/dev/null`, with a comment saying reaching a level is enough
there) rather than by omission.

Verified on hardware-equivalent tiers rather than assumed — `emu_progress_release.sh` on API 25:

```
  [ OK ] rendered to frame[2101] (>= 601)
  win check:  WIN CONFIRMED from pixels  [ WIN ] progR_1_cleared.png gold=0.0572 dark=0.5256 lum=57.9
  [ OK ] the next capture is a fresh level, not the results screen — it advanced
DONE (FAIL=0)
```

Also cross-checked mechanically: all nine screenshot paths handed to an `assert_*` call are paths the
calling script actually captures, so no assertion can be scoring a file that is never written.

### R36. The win verdict was computed correctly, then thrown away — in the three scripts that produce the headline evidence

`lib_wincheck.sh` is careful work. It returns **three** outcomes, never two, because a missing
interpreter once reported `NOT a win screen` for a run that had won, and the whole file exists to
keep "cannot check" from being confused with "did not win".

All three of its callers discarded the answer:

| script | what it did with the verdict |
|---|---|
| `emu_modern_playthrough.sh` | called `win_check`, ignored the return, printed `final pid: [3782] (alive => played through)`, `DONE`, exit 0 |
| `emu_audio_modern.sh` | same |
| `emu_modern_progress.sh` | never called it — printed `win/advance check: SCREENSHOTS ONLY -> …` and left both images unexamined |

So a run that photographed a level still in progress produced the same exit status, and nearly the
same log, as a run that won. These three produce **PROOF_8, PROOF_11 and PROOF_14** — the evidence
for the project's headline claim.

`selfhash_verify` was discarded identically. A script edited while a container was executing it
printed `*** SCRIPT CHANGED WHILE RUNNING — DISCARD THESE RESULTS ***` and then exited 0.

**Fixed** in `lib_playassert.sh`, as functions of plain arguments so they can be exercised without a
device:

- `assert_playthrough` — constructors, a frame floor, `h_fatal`, a live pid, and the win itself.
- `assert_progression` — the same, plus advancement as a **two-sided** claim: the cleared shot must
  be a win *and* the next shot must **not** be one. That second half has an obvious hole — a crashed
  app renders black, which is also "not a win" — so `png_sane.py` runs first and the next-level shot
  must be a real, non-blank frame before its win score means anything.

**"Could not check" is not a pass.** A playthrough test that reached no verdict has not demonstrated
a playthrough, so it fails — while still distinguishing a broken harness from a losing run, because
those call for opposite responses. And a failing win check is *not* automatically a port bug: these
runs are timing-sensitive, so the message points at the screenshot rather than accusing the shim.

`test_playassert.sh` (14 cases) feeds the verdict synthetic logs and **real screenshots** — a genuine
win frame and a genuine mid-flight frame — including the case the old code scored as success: a
mechanically healthy run whose end screen simply is not a win.

Re-run on API 34, asserting rather than narrating:

```
  [ OK ] all 125 constructors ran
  [ OK ] rendered to frame[1801] (>= 601)
  [ OK ] no h_fatal during the playthrough
  [ OK ] the process was still alive at the end (pid 2359)
  win check:  WIN CONFIRMED from pixels
              [ WIN ] modplay_3_end.png  gold=0.0580 dark=0.5294 lum=57.1
  script identity: unchanged during the run — results are from this file
DONE (FAIL=0)
```

`validate_all.sh`'s first stage now **discovers** self-test suites by glob rather than listing them:
the hand-written list was edited twice in one session and its describing comment went stale both
times. Zero matches is a failure, not a quiet pass.

### R35. Three lines of prose were executing as shell commands on every run — and `bash -n` says the file is fine

`emu_layer4_fcm_test.sh` carries the runtime proof for de-phone-home layer 4. Four lines of it
explained why the verdict refuses non-numeric input. The first line had its `#`; **the other three
did not**, so on every invocation the shell ran them:

```
bash: line 1: one: command not found
bash: integer expression expected: No such file or directory
bash: line 3: never: command not found
```

The middle one is bash being asked to execute a file named `integer expression expected`. All three
were harmless **by luck** — the first words happened not to be commands. That is the only reason
this cost nothing, and it is not a property anyone chose.

`bash -n` passes on the file, and always did: the lines are syntactically valid commands. Every
syntax check run on this tree, including mine earlier today, was incapable of seeing it.

The variant worth worrying about starts with a word that **is** a command. A wrapped comment line
beginning `exit ...` ends the script, and every check below it silently never runs — a test file
that passes because it stopped before testing anything.

`prose_as_code.py` now scans for it, with `test_prose.sh` (7 cases) proving it fires on the real
defect, fires on the `exit` variant, and stays quiet on valid shell. **Every shell file in the tree
is now clean** (71 files at the time of writing; 16 non-shell files skipped, and the skip count is
printed, because "0 findings" from a run that scanned nothing is the defect this suite exists to
hunt).

Two things the detector got wrong first, both fixed:

- Its own docstring named `exit` as *the one to worry about*, and its code did not catch it —
  `exit` was whitelisted as a shell keyword, so `exit early is what this would do` read as valid.
  A document claiming more than the code does, in the tool written to catch exactly that.
- The first tree scan produced **163 findings**, nearly all false. 150 were GLSL shader sources
  under `work803/assets/` that end in `.sh` and no shell will ever run; 12 were calls to functions
  defined in the scanned file itself. A checker that flags valid code gets switched off, which is
  the same outcome as not having one.

### R34. The regenerator for the interactive proofs exited 0 when it captured nothing

`emu_interactive_capture.sh` produces the source images for PROOF_2/3/4 — the bird on the slingshot,
the launch, the mid-arc frame. Two defects, both in the "absent symptom reads as success" class:

- **Install failure was an `exit 0` path**: `[ "$INST" = ok ] || { say DONE; adb emu kill; exit 0; }`.
  A run that installed nothing reported the same status as a run that captured three good frames.
- **The previous run's captures were never cleared.** So a run that died before the capture steps
  left `interactive_2/3/4.png` sitting in `reports/shots` — the *old build's* screenshots, beside a
  fresh log saying DONE. Anyone regenerating the proofs against a new build would have compared the
  new build to pictures of the old one.

Fixed: the three files are deleted before anything else happens, install failure exits 1, and the
captures are now checked mechanically by `png_sane.py` — a decodable PNG of the expected geometry
that is not a blank or solid frame — plus assertions that the app was alive and `h_fatal` was 0 when
the shutter fired. A capture of a crashed app is not evidence.

Thresholds were **measured before being written**, across all 27 real captures in `reports/shots`:

| | dominant-colour share | distinct colours |
|---|---|---|
| real captures | 0.072 (mid-flight) – **0.283** (dimmed win screens) | **570** (a menu) – 1947 |
| a solid frame | 1.000 | 1 |

The limit sits mid-gap at 0.650, not at the edge of the observed data. The first draft of that file
asserted "well under 0.5" and "thousands of colours" *before* measuring; both were wrong.

**What this deliberately still cannot do** is judge whether the shutter caught the intended instant.
That needs a person, the script says so, and passing here means only that the frames are real,
non-blank and from a live app.

### R33. A test of a documented promise printed four numbers and asserted none of them

`emu_save_test.sh` covers save persistence — "your progress is still there next time", a user-facing
claim in the shipped documentation. It ended like this:

```
  1st-launch saves written:  44 private files
  2nd-launch boot:  init_array 125/125  last-frame=frame[301]  h_fatal=0
  2nd-launch pid:   [2668] (alive => reloaded OK, no crash)
DONE
```

Four numbers **displayed**, none **checked**, and a bare `DONE` rather than `DONE (FAIL=n)`. Had
saves stopped persisting, it would have printed different numbers, exited 0, and read exactly like a
pass. The parenthetical `(alive => reloaded OK, no crash)` states the conclusion the script never
tested.

The verdict now lives in `lib_saveassert.sh` as a function of four plain arguments, so it can be
exercised **without a device**: `test_saveassert.sh` feeds it synthetic evidence and requires each of
the eight checks to fire on the failure it names — 15 cases, about a second, including a vacuity
guard that counts the `[ OK ]` lines so a silently-empty verdict cannot pass as a clean one. An
assertion that only ever runs at the end of a 20-minute emulator job is an assertion nobody can
prove works.

Re-run on API 34, all eight now assert rather than narrate:

```
  [ OK ] settings.lua survived the relaunch (1552 bytes)
  [ OK ] highscores.lua survived the relaunch (272 bytes)
  [ OK ] all 125 constructors ran on the second launch
  [ OK ] the second launch read its own files back (40 reads)
DONE (FAIL=0)
```

**The way this was nearly botched is the finding.** The first attempt inserted the assertions with a
scripted text replacement anchored on `say DONE` — which occurs **twice**, and the guard was
`assert old in s`, proving only that the string appeared *at all*, not that it was unique. The
assertions landed inside the install-failure branch `[ "$irc" -eq 0 ] || { ... }`. The install
succeeded, that branch never executed, the script ran its original path, printed the old output and
exited 0.

Both verification steps then agreed it was fine: `bash -n` passed, because the splice was still
syntactically valid, and the run exited 0, because the dead branch was never entered. Two checks
that could not fail, used to confirm a fix for a check that could not fail. The corrupted file was
restored from git; edits now anchor on unique multi-line context (which refuses ambiguous matches)
and are verified by reading the resulting `git diff` **hunk positions**, not by asking whether the
file still parses.

`validate_all.sh` gained a first stage that runs these self-tests before anything else, on the host,
in about ten seconds: every stage below it is only as trustworthy as the code that judges it.

### R32. The de-phone-home runtime proof was measured with the radios off

Every script in this rig sets `airplane_mode_on 1` — 36 of them. For most that is right: it keeps
runs deterministic. For **`emu_jni_exception_probe.sh`** it quietly undermined the headline result.

That script carries the strongest runtime evidence for clause 3 of the brief: *the app's own pid
performs no name resolution and opens no sockets*. Asserting that on a device whose radios are off
proves very little — the environment guarantees the answer whether or not the port has anything to do
with it. The check was sound and pid-attributed; the **conditions** made it easy.

Re-run with `ABSHIM_NETWORK=1`, which leaves the guest's network stack up:

```
network: LEFT UP (ABSHIM_NETWORK=1) — the socket/DNS assertions are about the app, not about airplane mode
app pid = 3355
UnknownHostException: 12 in the log, 0 from our pid
SocketException:       0 in the log, 0 from our pid
ConnectException:      0 in the log, 0 from our pid
socket failed:         0 in the log, 0 from our pid
FlurryAgent reports ACCESS_NETWORK_STATE is not declared
Facebook SDK reports no INTERNET permission granted
```

The **12 `UnknownHostException`s are the control**, and they are better than a designed one: with the
stack up, system components genuinely attempted DNS and failed for want of external routing. Name
resolution was demonstrably being attempted on that device, during that run — and none of it was
ours.

**What this still is not.** Every container runs `--network none` by policy, so the emulator's NAT has
nowhere to go: this is a live network *stack*, not live internet. That is adequate for the claim being
made — the app holds no `INTERNET` permission, so the kernel refuses socket creation outright with
`EPERM` regardless of reachability — but it makes this a stronger test rather than a complete one, and
the script says so in its own comments rather than leaving a reader to assume otherwise. **The stronger form is now the default** for both scripts whose subject is the network claim —
`emu_jni_exception_probe.sh` and `emu_doc_verify.sh`. A flag that selects the meaningful test is a
flag someone forgets; `ABSHIM_NETWORK=0` opts back out for a deterministic run, and says so in the
output rather than silently reverting to the easy conditions.

`emu_doc_verify.sh` had the same weakness and the same fix. Its check is *"no ESTABLISHED socket
owned by the app"*, which on a radios-off device is true for reasons unrelated to this build. With
the stack up the result is stronger than the documented claim:

```
network: LEFT UP (default)
[ OK ] and the same command DOES find INTERNET on a package that has it
rows owned by uid 10216: 0   (ESTABLISHED, state 01: 0)
[ OK ] no ESTABLISHED socket owned by the app, as documented
```

**Zero socket rows of any state**, not merely zero established ones — while the positive control
confirms the same command still finds `INTERNET` on a package that holds it.

### R31. Ten capture scripts wrote images with no provenance — two of them produced cited PROOFs

Wiring provenance into this session's three lifecycle tests raised the obvious question: how many
*other* scripts save a screenshot and never record which build made it? Ten:

```
emu_16k_pagesize  emu_a56_screen  emu_audio_test  emu_fatal_release  emu_modern_test
emu_premise       emu_progression emu_run         emu_save_test      emu_soak
```

Most write diagnostics, where the omission costs little. **Two produced cited evidence** —
`emu_premise.sh` → `PROOF_20` (the premise: the original APK refused, ours runs on the same device)
and `emu_a56_screen.sh` → `PROOF_21` (a win at the A56's real 1080x2340 geometry). Two of the
project's load-bearing proofs had no build attached, and the *"captures were taken on builds that
still exist"* check silently did not cover them: it can only police rows that exist, and nothing
failed because nothing was there.

Both now record provenance, and the rows were produced by **re-running** them rather than written by
hand. Both passed again, which re-established their findings in passing:

```
premise     [ OK ] REFUSED for the documented reason: no ABI this device can run (R12)
            [ OK ] the re-hosted game RUNS on the device that refused the original (frame[1201])
a56_screen  [ OK ] renders at the A56's geometry (frame[2401])
            [ OK ] WIN at 1080x2340 — slingshot drags reached the engine through the shim
```

Guarded: the index already names each proof's source script, so the rule is checkable — if the index
says a script produced a proof, that script must call `record_build`. Comments are stripped first, so
a script merely *mentioning* it in prose cannot satisfy the check. Mutation `proof_prov` deletes the
call from a proof source and is caught.

Adding that claim made the gate's own count 31 while the docs still said 30, and the counting check
from R27 caught it immediately — the second time that self-referential check has policed a change I
made in the same commit.

**31 claims, 30 mutation cases, 30/30 detected, 0 skipped**, vacuity audit clean, control passing.

### R30. Rotating the phone rebuilds the Activity under a live process — and the port survives it

Read out of the shipped manifest rather than assumed:

```
activity com.rovio.fusion.App
  screenOrientation = 0      (unspecified — the app is NOT orientation-locked)
  configChanges     = 0x4f0  (uiMode | orientation | navigation | keyboardHidden | keyboard)
```

`orientation` is handled by the activity, so an orientation flip alone would not recreate it — but
**`screenSize` (0x800) is absent**, and for a target above API 13 a rotation that changes the screen
dimensions recreates the activity anyway. So turning the phone destroys and rebuilds the Activity
*underneath a process that keeps running*: the engine stays alive while its window, surface and GL
context are torn down and replaced.

That is strictly harder than R29 (activity merely paused) and harder than R28 (everything restarted
clean), and it had never been tested. `emu_rotate.sh` on **API 36, the A56's OS**:

```
before        pid 3009   frames 1201 -> 1501 over 12s     [ OK ] advancing
rotate 90°    user_rotation now: 1                        [ OK ] really rotated
after         pid 3009   frames 2401 -> 2701 over 15s     [ OK ] +300 frames
rotate back   pid 3009   frames 3601 -> 3901 over 12s     [ OK ] survived the return too
h_fatal: 0  (11 675 shim log lines, so this was measured)
```

Same pid throughout — the activity really was rebuilt under a live process, which is the case worth
testing. `rotate_screen.png` shows the tutorial level drawn normally, so the new surface is being
rendered into and not merely counted.

One measurement detail worth keeping: `wm size` reports **320x640 before and after**, because it
returns the physical panel and does not swap on rotation. Treating that as the rotation check would
have made the test conclude nothing had happened; the applied `user_rotation` setting is read back
instead, and the run refuses to claim survival if it did not stick.

### R29. Home-then-return works — and the first run of the test nearly filed a phantom EGL bug

Backgrounding and resuming is the most common thing a user does to a running app, and no test here
had ever done it. Every play run launches, plays and ends; none leaves and comes back.

It is a real risk for this port. Android destroys the EGL surface when an activity stops and supplies
a new one on resume, and `grep -rn 'surfaceDestroyed\|eglMakeCurrent' port/shim/src/` finds
**nothing** — the shim passes GL through, and the surface lifecycle belongs to the engine's Java
side. The original design notes list "EGL context lifecycle" as a top hazard for precisely this.

`emu_background_resume.sh` measures **three** states rather than two, so it cannot fool itself:
frames must be advancing *before* (or "it resumed" is a claim about a stalled app), must **stop**
*during* (a Home key that did nothing would otherwise look like a flawless resume), and must advance
again *after*.

**The first run failed**, and the failure looked exactly like the hazard:

```
frames while backgrounded: 1501 -> 1501   [ OK ] rendering stopped
pid after resume: 3068    frames 1501 -> 1501 over 15s
[FAIL] frames are NOT advancing after the resume — the renderer did not recover the surface
h_fatal: 0
```

Same process, no crash, renderer permanently stalled after a surface swap. That is a textbook EGL
resume bug, and it would have been reported as one — except the script takes a screenshot, on the
principle that *"frames advance" and "the game is visible" are different claims*.

The screenshot showed **Android's `POST_NOTIFICATIONS` permission dialog** sitting over the game.
The activity is not resumed while a system dialog holds focus, so the renderer was idle for a
completely ordinary reason. Nothing to do with EGL.

`lib_dialogs.sh` now dismisses that prompt — answering **"Don't allow"**, which is also the correct
answer for a build shipping with push neutralised — and the re-run passes:

```
pid after resume: 3127    frames 2401 -> 2701 over 15s
[ OK ] rendering resumed (+300 frames in 15s)
(same process throughout — pid 3127, so this was a real resume, not a relaunch)
h_fatal: 0  (9688 shim log lines, so this was measured)
```

`bg_resume_screen.png` shows level `0-1` drawn with the game's own **pause menu** over it — the
correct behaviour: it paused when backgrounded and offers resume on return.

So the surface swap is handled, the process survives, and the visible result is right. The lesson is
the near-miss: a three-state frame measurement, careful about its own premises, still produced a
confident and wrong diagnosis. Only the image settled it.

### R28. Saves survive a real device reboot, and the game reads them back — tested on the phone's OS

Persistence was proven across an **app** restart only: `emu_save_test.sh` force-stops the process and
relaunches, `emu_update_install.sh` replaces the package. Neither restarts the **device**, and a
reboot is materially different — the process, its ART state and every mapping are gone rather than
force-stopped, `/data` is remounted, credential-encrypted storage unlocks afresh, and the app then
launches on a device where it is *already* installed, with no first-install state and no warm cache.

That matters more here than for a normal app: the shim finds its ARM32 payload by taking `dladdr()`
of one of its own functions and `open()`ing `libengine32.so` beside it. That path exists because the
installer extracted the native libs. Nothing had checked it is still valid, and still points at an
intact file, after a full boot.

`emu_reboot_persist.sh` on **`ab36` — API 36, the A56's own OS**:

```
saves before reboot: 786b666074b0a1396a98f0907be76de7 3beb37249fe64e2f7af961140c5c08e3
boot_id before: 6c173d5d-a08e-4d27-bc7d-885cfef769e1
boot_id after : 71d7cf4f-0f1e-4dfd-bad3-f27423aa0dbe   [ OK ] the device really rebooted
saves after reboot : 786b666074b0a1396a98f0907be76de7 3beb37249fe64e2f7af961140c5c08e3
frames after reboot: 901   h_fatal: 0  (5596 shim log lines, so this was measured)
```

The **boot-id comparison is load-bearing**: an `adb reboot` that silently did nothing would leave
every check below it passing on a device that never restarted. Same for the "saves exist before"
guard — without it the test would happily prove that nothing survives nothing.

And the capture is better evidence than the hashes. `reboot_persist_screen.png` shows the game
running after the reboot with **`HIGHSCORE 43270`** in the HUD: the save files were not merely left
intact, they were **read back and applied** — the score earned before the restart is on screen.

### R27. "This is checked automatically" is itself an unverified claim

Adding a pre-install integrity check to `ONDEVICE.md` produced a lesson worth generalising: the
sentence *"`verify_claims.sh` fails if any SHA-256 printed in these docs stops matching"* was **false
when written**. The hash claim only recognises literal `sha256sum` output (`^<64hex>  <name>.apk`),
and the doc had written the value as a `# expect:` comment — invisible to it. Corrupting the line in
a scratch copy left the gate green.

So every assurance of that shape in the user-facing docs was audited by trying to break it. Most held
— the socket-import claim, the 16 KB alignment claim, the `coverage_check.py` hard gate. One did not:

> `port/validation/verify_claims.sh`  re-checks all **18** documented claims against the shipped bytes

The gate had **29**. That number had fallen behind every claim added since it was written, in the one
sentence that tells a reader how much checking exists at all — the same defect class as a stale
measurement, in a more load-bearing place.

Both are fixed the same way: **make the assurance self-policing rather than corrected.**

- the APK hash is now written in `sha256sum` output form, so the existing check parses it and the
  existing `doc_hash` mutation covers it. Verified by corrupting it: *"docs say 0000000000000000…,
  artifact is 27548721a456ea99…"*.
- a new claim counts the `== CLAIM:` lines in the gate and compares that with the figure the docs
  advertise. Its first run failed with *"the docs advertise 29 … but this file has 30"* — because
  adding the counting claim had changed the count. Mutation `claim_count` rewrites the advertised
  number and is caught.

Two smaller notes from doing it. The count check's first regex anchored on `re.?checks` and matched
nothing, because README.md uses a **non-breaking hyphen** in "re‑checks" — three UTF-8 bytes where
`.` matches one. It **skipped rather than passed**, which is the only reason it was noticed. And the
correction to `ONDEVICE.md` is recorded in the document itself rather than quietly applied, since
"this number is machine-checked" is exactly the kind of assurance a reader cannot verify by looking.

**Then every other number the docs assert was audited the same way**, since a count that drifts is
the same defect wherever it sits:

| stated figure | verdict |
|---|---|
| README's "re-checks all **18** documented claims" | **wrong** — the gate had 29. Fixed, and now self-policing |
| `15/15 mutations detected` (validation README) | **stale** — that suite is at 19/19. Now records both: 15 is history, 19 is current |
| `20 checks` on AArch64 | correct — matches `checks passed on AArch64: 20` from the run |
| `125 constructors` (8 mentions) | correct, and **asserted programmatically**: `arm64_cross_test.sh` greps for `125/125 constructors ran CLEAN` and fails loudly otherwise |

Writing that table tripped the new check: quoting the wrong figure verbatim put a second
`checks all N documented claims` string into the docs, and the claim compares every such figure it
finds. The row now breaks the phrase with markdown emphasis so it reads the same and matches nothing
— the same recursion as the host-path scan flagging the sentence that documented the host-path leak.

Two of four had rotted, and both were counts nobody re-derives. The two that held are the two a
script re-checks on every run — which is the argument for deriving numbers rather than typing them.

**30 claims, 29 mutation cases, 29/29 detected, 0 skipped**, vacuity audit clean, control passing.

### R26. A security-relevant check had never been able to fail — `git grep` was aborting, silently

The gate's header asserts *"Every check in this file has now been deliberately broken at least
once"*, and notes that the sentence "used to be TAKEN ON TRUST". With 29 claims and 21 automated
mutations, it was worth testing again rather than inheriting.

Writing the missing case for **"no tracked file names a build-host path"** exposed that the claim
could not fail at all. It runs

```
git grep -InE '/home/[a-z_][a-z0-9_-]*/|/root/[a-z]|/Users/[A-Za-z]' -- . 2>/dev/null
```

inside a container, as root, against a bind mount owned by the host user — so git aborts with
**"detected dubious ownership in repository at '/work'"**, `2>/dev/null` swallows it, the variable
comes back empty, and the claim reports OK. A deliberately planted `/home/<user>/…` path in a tracked
file was **not detected**.

This matters beyond bookkeeping: the repository is **public**, and this is the check that stops the
build machine's paths being published.

The fix was already written down elsewhere in the same file. The doc-referenced-files claim carries
`-c safe.directory=*` and a comment saying *a check that cannot tell "not tracked" from "cannot ask"
is worse than no check* — this claim is precisely that check. It now passes `-c safe.directory='*'`
and **fails loudly if git errors**, rather than treating an unanswerable question as a clean answer.

Two details worth keeping:

- Writing this very finding up flagged it again: the paragraph above originally spelled the literal
  path while explaining the fix, and the scan caught `OPEN_FINDINGS.md`. Documenting a leak must not
  leak; the placeholder form `/home/<user>/…` is safe because the pattern requires `[a-z_]` after
  `/home/`.
- With the scan working, it immediately flagged **`mutation_test.sh` itself** — the new fixture wrote
  a literal build-host path (`/home/<user>/…`) into a tracked file. The fixture now assembles the path at
  runtime from two halves so neither matches the pattern. A test for a leak must not be a leak.
- The vacuity audit then flagged the new case, correctly: while the real tree was failing that claim,
  its expected text appeared in the "clean" output. Fixing the fixture cleared both.

The artifact-side twin of that claim — *"the artifact names no build-host path"* — had no case
either, and the function written for it was never registered (hand-rolled zip surgery instead of the
`repack_member` helper every other artifact mutation uses). Rewritten and wired up: appending a
build-host path to the shim inside the APK now yields
*"libAngryBirdsClassic.so embeds 1 build-host path(s)"*.

**23/23 mutations detected, 0 skipped**, vacuity audit clean, control passing.

**Closing the rest of the gap.** Four more claims had no automated case, and three are now covered:

| claim | mutation | result |
|---|---|---|
| native libs are EXTRACTED to disk | append `extractNativeLibs` to the manifest — parsers read the chunk header and ignore trailing bytes, so every *other* claim still parses it, while the `strings` scan this claim uses sees it | caught |
| every documented log marker is emitted | add `` `[no-such-marker]` `` to ONDEVICE.md | caught |
| the audio variant ships identical payloads | append a byte to `libengine32.so` **inside the audio APK** — `repack_member` gained an optional 4th argument for this, defaulting to the silent build so existing cases are unchanged | caught |

One earlier entry in this list was wrong: **capture-build currency was already covered** by the
`stale` case, which rewrites a provenance hash. Listing it as a gap was itself an unverified claim.

**26/26 mutations detected, 0 skipped**, vacuity audit clean, control passing.

**JNI thunk completeness is covered too now.** Mutating it needed different surgery: rename one of
the shim's 72 `Java_com_rovio_*` exports in place, at identical length, replacing the last character
with `9` so the scan extracts a well-formed *but different* name. The mutation has to look like a
missing thunk, not a corrupt binary — a missing thunk does not fail at load, it fails the first time
Java calls that method, potentially deep into play on the phone. Caught:
*"shim is MISSING thunks for engine natives"*.

**How much of the gate is actually proven — measured, not asserted.** The header claims every check
has been deliberately broken at least once, and that sentence has been wrong before. A static map
now pairs each claim with the cases whose expected text appears in it (following through to the
helper scripts the claim calls, since several print their own failure lines):

- a first version of that map searched only `verify_claims.sh` and reported **6** uncovered claims.
  Four of those were false: their wording lives in `check_depermission.py` and friends. The map was
  wrong before the file was.
- of the remainder, three were cases that broke *two* claims while asserting only one. `case_run`
  now accepts `A||B` and requires **every** listed failure, so `align16k` asserts both 16 KB claims
  and `libm_gone` asserts both `libm` **and** import-resolvability — the latter previously rested on
  a manual check of mine rather than on the suite.

Both claims the map could not confirm were then settled **empirically**, since a static search cannot
match a message built from shell variables:

- **engine authenticity** — applying the `payload` mutation and reading the gate shows
  `[FAIL] libAngryBirdsClassic.so != libengine32.so` under that very claim. It was covered all along;
  the map simply could not see `bad "$o != $m"`.
- **provenance label lint** — genuinely had no case. Now `prov_label` gives a `$PFX`-parameterised
  capture script a hardcoded provenance label, which is the bug the claim's own comment says *"has
  been made TWICE"*: two runs at different API levels then overwrite each other's row and the ledger
  quietly describes only the last. Caught: *"these take $PFX but record a FIXED provenance label:
  emu_modern_playthrough.sh"*.

Writing that last case produced one more small lesson. Its first version selected the first script
matching `^PFX=`, which turned out never to call `record_build`; the guard refused and the case
**SKIPped** — correct behaviour, but it reads like "this mutation is impossible" rather than "the
harness picked the wrong file". It now selects a script that has both.

**28 cases, 28/28 detected, 0 skipped**, vacuity audit clean, control passing — and every one of the
29 claims is now either matched by the coverage map or confirmed by running its mutation.

### R25. Two of twenty-one mutation cases could not fail — now audited mechanically

Finding one vacuous case raised the obvious question: how many others? Rather than reason about it,
it is decidable — a case is vacuous exactly when the text it expects already appears in the gate's
**clean** output, because then it reports "detected" whether or not its mutation did anything.

Running that comparison over every registered case found a second one immediately:

| case | expected | why it could not fail |
|---|---|---|
| `stale_doc` | `quotes a superseded measurement` | the wording of the claim's **header**, printed every run — and a `mut_stale` name collision meant it ran the *provenance* mutation anyway |
| `sockets` | `network-capable symbol` | the wording of that claim's **OK** line: *"shim imports NO network-capable symbol"* |

`sockets` is the older and worse of the two: it is the check that the shipped shim imports nothing
network-capable — one of the four de-phone-home layers — and its mutation has been reporting success
without ever demonstrating the check fires. The correct expectation is the failure text, *"the
socket-import scan failed"*, and with that it does detect.

**The audit is now permanent.** `mutation_test.sh` runs the gate once on the unmutated tree before
any case and refuses to proceed if any case's expected text appears in that output. Proven able to
fail: reintroducing the old `sockets` wording in a scratch copy produces

```
*** VACUOUS CASE(S): sockets — their expected text appears in the CLEAN gate output,
    so they would report 'detected' with no mutation applied.
```

and a non-zero return. **21/21 mutations detected, 0 skipped**, control passing, audit clean.

The general rule, now enforced rather than remembered: **a mutation must expect the `[FAIL]` text, never
a string the claim also prints when it passes.**

**The other suite was checked too, and is structurally immune.** `port/shim/test/mutation_modules.sh`
decides detection by **exit code** — *"test still exits 0 with the invariant broken"* — not by
matching text, so the trap above cannot occur there, and a mutation that fails to apply leaves the
test passing and is reported NOT DETECTED, which is the safe direction. Run to confirm rather than
assumed: **19/19 module mutations detected, 0 skipped, 0 documented gaps.**

**And the gate skips nothing.** A claim that quietly skips is the neighbour of one that passes
vacuously, so the clean run was checked for `[skip]` lines: there are none, and the summary is a
bare `ALL CHECKED CLAIMS HOLD` with no "but N were not checked" qualifier.

Consolidated state after this session's changes to the validation machinery itself:

| suite | result |
|---|---|
| `validate_all.sh` | **ALL OFFLINE VALIDATION PASSED** (4/4; ctors 7793 and `nativeInit` 8290 allocation records identical across hosts) |
| `verify_claims.sh` | ALL CHECKED CLAIMS HOLD, **0 skipped** |
| `mutation_test.sh` | **21/21**, 0 skipped, vacuity audit clean, control passes |
| `mutation_modules.sh` | **19/19**, 0 skipped, 0 documented gaps |

### R24. The provenance ledger recorded which build a capture came from, but not which Android it ran on

Committing one audio row exposed it: the AVD column was empty. `record_build()` read it from
`${ABSHIM_AVD:-}`, and scripts that pick their AVD internally never export that variable
(`emu_audio_modern.sh` → `abtest34`, `emu_16k_pagesize.sh` → `ab16k`). **Eleven of thirteen rows had
no environment recorded at all.**

That is most of the ledger's value. "Won on API 36" and "won on API 25" are very different claims
about a phone running Android 16, and from the record they were indistinguishable.

**Fixed at the source:** the AVD is now asked of the emulator — `adb emu avd name` — rather than
inferred from a variable the caller may never have set, or may have set to something it did not
launch. Verified against the raw protocol, which returns two lines (`abtest34\r`, `OK\r`), with a
guard for the ordering where the status line lands first. Unknown values record `<not-recorded>`, so
absence looks like absence instead of a formatting artefact.

**Backfilled by re-running, not by inference.** Four rows (`playthrough`, `emu_fatal`, `interactive`,
`modplay`) were missing their environment. Their scripts have known defaults and the README map lists
them, so filling those in from the map was tempting — and would have been inventing provenance, the
exact thing this file says is worse than admitting absence. Each capture was re-run instead, which
refreshed the evidence as well as the row:

```
audiomod  abtest34     playthrough  abtest    emu_fatal  abtest
modplay   abtest34     interactive  abtest    modplay36  ab36     regr36  ab36
```

`modplay`'s re-run reconfirmed its result in passing: WIN from pixels, `h_fatal` 0 across 7370 shim
log lines.

Guarded and mutation-proven: a 5-field row with an empty AVD fails the gate (`prov_env` blanks one),
while legacy 3-field rows stay exempt because they predate the columns. **20/20 mutations detected,
0 skipped.**

### R23. The second deliverable re-verified: the audio variant builds reproducibly and still wins

`ONDEVICE.md` offers an **experimental audio-enabled variant** alongside the silent shipping build,
with a build command the user is told to run and a claim that it plays crash-free. Both were re-run
after this session's changes to `lib_install.sh`, which every emulator script sources.

**The documented build command reproduces byte-identically:**

```
docker run --rm --network none -v "$PWD":/work ab-port \
    env ABSHIM_AUDIO=1 bash /work/port/build_apk.sh
before: 196053244e92ac8c902f1b956ad8b56211266012ec48c6b4…
after : 196053244e92ac8c902f1b956ad8b56211266012ec48c6b4…
cmp: clean
```

So reproducibility is not a property of the default build alone — the `ABSHIM_AUDIO=1` path is
deterministic too, which matters because that is a command handed to the user.

**And it still plays with audio active, on modern Android** (`emu_audio_modern.sh`, API 34):

```
nativeMixData:  8   (>0 ⇒ AudioTrack inited and the mixer ran under modern W^X)
h_fatal:        0   (6431 shim log lines, so this was measured)
win check:      WIN CONFIRMED from pixels
```

Looked at rather than scored: `audiomod_end.png` is **LEVEL CLEARED**, three stars, **43210**.

The known limit is unchanged and still honestly stated in `ONDEVICE.md`: *continuous* playback cannot
be settled here, because the emulator's host audio backend will not initialise in a headless
container, so the guest `AudioTrack` buffer never drains. (**"the mixer blocks after ~8 buffers" is withdrawn — see R43: 8 is the `am++<8` log cap, and the mix rate is not measurable from the logs at all.**) That
is emulator infrastructure, not the shim — `nativeMixData: 8` is exactly that ceiling, and it is why
the number is 8 rather than growing.

### R22. Every symbol the shim imports is now proven resolvable on the phone — the generic form of the `-lm` bug

The worst near-miss in this project was a link flag. The shim used `sin`/`cos`, was linked without
`-lm`, did not declare `libm.so` in `DT_NEEDED`, ran perfectly on API 25 — and would have died on the
A56 at launch with `UnsatisfiedLinkError: cannot locate symbol "sin"`. It was caught by running on a
newer image, which is luck rather than method, and the gate has carried a `libm`-specific check ever
since.

That check covers the instance. The **class** is: any future edit that uses a symbol from a library
the shim does not declare fails identically — on the user's phone, at launch, with some other symbol
name. Nothing detected that.

It is decidable statically. The NDK ships link-time stubs whose exported symbols are exactly what the
platform provides, so every `UND` symbol in the shipped `.so` must be exported by one of the
libraries named in its own `DT_NEEDED`. Anything left over is a symbol the device's linker will not
resolve.

**Measured on the shipped artifacts: all 358 imported symbols resolve, 0 unresolved.** The shim
declares `liblog libandroid libGLESv2 libEGL libm libdl libc` and asks for nothing outside them.

Proven able to fail, and proven to catch the *class* rather than the string: `mutation_test.sh`'s
`libm_gone` case renames `libm.so` to `libq.so` inside the ELF's string table — same length, no
offsets move — which is exactly "linked without `-lm`" as the loader sees it. The general check then
reports **25 unresolved symbols**: `acos`, `acosf`, `asin`, `asinf`, `atan`, `atan2`, … The
`libm`-specific claim fires too, which is the point: the specific check names the known bug, the
general one would have caught it without knowing about it.

**19/19 mutations detected, 0 skipped**, unmutated control still passing.

### R21. Stale measurements in docs are now caught mechanically, not one at a time

Two rounds running, a user-facing document was found quoting a number that a later measurement had
replaced. `RELEASE_NOTES.md` still said the guest heap "differs by about 64 KB after startup" long
after both architectures measured `605096`, and that allocations match "at least the first 4913
requests" after the full traces were compared. `ONDEVICE.md` carried the `4913` figure too. Each was
fixed by hand.

Fixing by hand does not scale, and the cost is not the individual number — it is that a reader who
finds one stale figure stops trusting the rest of the record.

`verify_claims.sh` now checks it. The rule is deliberately **not** "never mention the old value":
the history is worth keeping, and this project's own corrections depend on being able to say what a
number used to be. The rule is that a superseded value must appear **with framing that marks it as
history** — `correction`, `update`, `earlier`, `previously`, `no longer`, `cannot be obtained`,
`superseded`, `at the time`, `has moved`, `old figure`, `history` — within a ±6-line window. An
unframed occurrence is a stale claim and fails the gate.

Tracked so far:

| superseded | current |
|---|---|
| `539536` (arm64 heap after ctors) | `605096`, both architectures, re-measured 2026-07-28 |
| `4913` ("at least the first N allocations match") | all **7793** ctor and **8290** `nativeInit` records identical |

The check found a real instance on its **first** run — and it was in this file. Two mentions of
`4913` in the paragraph above were flagged: prose that is unambiguously historical ("a number that a
later measurement had **replaced**", "one **stale** figure") but that happened to use none of the
original framing keywords. That is a false positive of an over-narrow vocabulary rather than a stale
claim, so `replaced` and `stale` were added to the list. The alternative — rewording documentation
until a regex is satisfied — would have been the checker dictating prose, which is backwards.

**Correction — that "mutation-proven" claim was false when it was written.** The `stale_doc` case
appends an unframed *"the guest heap holds 539536 bytes on arm64"* to `ONDEVICE.md`, padded with
neutral filler so it lands outside any framing window. It reported detected, and it was not testing
anything:

- its function was named `mut_stale`, and so is the pre-existing **provenance**-staleness mutation.
  Bash keeps the last definition, so the case silently ran the wrong mutation;
- it "passed" because the expected substring also appears in the claim's **header** line, which the
  gate prints on every run whether the claim passes or fails.

A vacuous case, inside the suite whose entire purpose is proving checks are not vacuous. It surfaced
only when the expectation was tightened to match the `[FAIL]` text rather than any line — at which
point it immediately went **NOT DETECTED**.

Fixed by renaming to `mut_stale_doc` and matching the failure line. Now genuinely proven:
**21/21 mutations detected, 0 skipped**, unmutated control passing.

### R20. The reproducible build, re-verified end to end today

Clause 2 of the brief is that the conversion be a reproducible automated process inside Docker. That
was established earlier and has not been re-run since a long stretch of harness changes, so it was
re-run from the documented entry point rather than assumed to still hold:

```
bash port/reproduce.sh        →  out/angrybirds-8.0.3-arm64.apk
before: 27548721a456ea99295469c30c247e3f9519878a3d40abb817a148801af04851
after : 27548721a456ea99295469c30c247e3f9519878a3d40abb817a148801af04851
[ OK ] REPRODUCIBLE — byte-identical
```

`cmp` clean against the artifact that existed before the rebuild, and matching the hash documented in
`REPRODUCE.md`. Every step runs `docker run --rm --network none`; there is no `-p`/`--publish` and no
`--network host` anywhere in the build path. (A grep for `-p [0-9]` does hit `zipalign -f -p 4`, which
is zipalign's page-align flag, not a port publish.) The `1980-01-01` timestamps visible on every zip
entry are the normalised mtimes that make the byte-identical result possible.

### R19. Regression check: the install-library changes did not break the phone's actual OS

`lib_install.sh` was changed twice while chasing R18 — installer exceptions reclassified as transient,
and the APK push moved inside the retry loop with verification. **Every emulator script in this rig
uses that library**, so a mistake there would silently degrade all of them.

Re-ran the full playthrough on **API 36 — the A56's OS** — with the modified library:

```
install:           ok
win check:  WIN CONFIRMED from pixels
            [ WIN ] regr36_3_end.png  gold=0.0572 dark=0.5241 lum=57.9
h_fatal:           0  (7925 shim log lines, so this was measured)
```

The capture was looked at, not only scored: it is the **LEVEL CLEARED** screen, three stars, score
**44910**. So the library changes are clean on the platform that matters, and the port still plays and
wins there.

**The de-phone-home assertions were re-run for the same reason** (`emu_jni_exception_probe.sh`,
abtest34, shipping config):

```
ASSERTION 1  no pending exception / JNI DETECTED ERROR / Runtime aborting     [ OK ]
ASSERTION 2  app pid 2349
               UnknownHostException  12 in the log, 0 from our pid            [ OK ]
               SocketException        0 in the log, 0 from our pid            [ OK ]
               ConnectException       0 in the log, 0 from our pid            [ OK ]
               socket failed          0 in the log, 0 from our pid            [ OK ]
ASSERTION 3  FlurryAgent reports ACCESS_NETWORK_STATE is not declared         [ OK ]
             Facebook SDK reports no INTERNET permission granted              [ OK ]
```

**The commands `ONDEVICE.md` tells the user to run were re-executed too**, on `ab36` — the phone's own
OS — via `emu_doc_verify.sh`: the no-phone-home `dumpsys` grep (with its positive control, *"the same
command DOES find INTERNET on a package that has it"*), the "no ESTABLISHED socket while it runs"
check, the save-inspection tip, and the ABI check. `DONE (FAIL=0)`. This matters more than it sounds:
the remaining work is a physical install, so the instructions are the handoff, and instructions that
have never been executed are just prose.

**Layer 4 (the Firebase/FCM kill-switch) was re-run as well**, on the GMS image — the only tier where
it can be exercised at all, since FCM registration is performed by Google Play Services on the app's
behalf:

```
control token-registration attempts: 1
shipped token-registration attempts: 0
[ OK ] control attempted token registration 1x, shipped 0x — layer 4 demonstrably prevents it
```

That it is a **differential** is the whole point. "The shipped build attempted 0 registrations" is
compatible with the kill-switch working and equally compatible with the code path never running at
all; the control build, identical but with the kill-switch removed, is what distinguishes them.

Assertion 2 repays a careful read: **twelve** `UnknownHostException`s do appear in that log, raised by
system components resolving names for their own reasons, and **none of them belong to our pid**. A
bare "0 network errors on the device" would have been both false and unfalsifiable — attributing by
pid is what turns it into evidence. Assertion 3 is the inverse: two bundled tracking SDKs
independently notice, at runtime, that the permissions they need were stripped.

### R18. 16 KB pages are FINE. Android **36.1** is where this app stops being launchable — and it is not our doing

Two things were tested here. One closes an open gap; the other opens a new one that matters more.

**The 16 KB page-size gap is closed at the mechanism, and the artifact installs.** This project linked
the shim and payloads with `-Wl,-z,max-page-size=16384` and could only verify the alignment in the
ELF headers, because no 16 KB image existed on this host. One exists now
(`system-images;android-36.1;google_apis_ps16k;x86_64`), so `port/docker/Dockerfile.ab-emu-16k` +
`port/validation/emu_16k_pagesize.sh` run it for real. The guest is genuinely 16 KB —
`getconf PAGE_SIZE` returns **16384** and the kernel command line carries `page_shift=14` — and the
**APK installs there**.

**That last sentence used to carry an argument that was simply wrong**, and it is worth leaving the
correction visible: it read *"on a 16 KB kernel a 4 KB-aligned library is rejected outright by the
loader, so installing at all already excludes the alignment failure"*. Installing excludes nothing.
Android rejects a misaligned library at **`dlopen`**, not at install time, so an APK full of 4 KB
libraries installs perfectly happily and only dies when something tries to load one. The claim needed
the loader's answer and had been given the package manager's.

**Now measured properly** (`port/validation/emu_dlopen_pagesize.sh`, with
`build_dltest.sh` + `src/dltest.c`). The game cannot supply this evidence on that image because it
never launches there, so a 20-line program asks the loader directly — no Activity, no ART, no app:

| | 16 KB kernel | 4 KB kernel |
|---|---|---|
| **A. 16 KB-aligned tester dlopens THE SHIM** | **DLOPEN-OK** | DLOPEN-OK |
| B. 16 KB-aligned tester dlopens system `libc` | DLOPEN-OK | DLOPEN-OK |
| C. **4 KB-aligned** tester (negative control) | **Segmentation fault** | DLOPEN-OK |

Row **C** is what makes row A mean anything: it shows this kernel genuinely **enforces** alignment,
refusing a 4 KB-aligned binary that runs fine on the 4 KB image. Row **B** shows the tester works at
all. So: the enforcement is real, and **the shim loads under it**.

**And the deliverable itself was audited, not just the x86 proxy.** The probe above loads the *x86*
shim, because that is what runs on an x86_64 emulator. What ships is the arm64 APK, so its libraries
were checked directly:

```
lib/arm64-v8a/libAngryBirdsClassic.so   align=0x4000   AArch64     <- the only one Android dlopens
lib/arm64-v8a/libengine32.so            align=0x1000   ARM (32)
lib/arm64-v8a/libjs32.so                align=0x1000   ARM (32)
lib/arm64-v8a/libadcolony32.so          align=0x1000   ARM (32)
```

Three 4 KB-aligned libraries sit in an arm64 directory, which looks alarming and is not. Nothing
hands them to bionic: `jni_entry.c` takes the payload with `open()` + `fstat()` + `mmap()` and maps it
into Unicorn's own address space, the shim's only real `dlopen` calls target system libraries
(`libandroid.so`, `libGLESv2.so`), and the guest-side `dlopen` bridge returns 0 so emulated ARM32 code
cannot load anything either. That was read out of the source rather than inferred from file names,
and the APK does install on the 16 KB image, so the installer does not object to them either.

**The APK's internal zip alignment is not part of this, and that was checked too.** A 16 KB device
also cares about zip alignment when libraries are `Stored` and mmapped straight out of the APK. Ours
are not: all four are `Defl:N` (compressed), so Android extracts them to the app's lib directory at
install and the loader reads the extracted file from disk. `zipalign -p 4` in `build_apk.sh` is
therefore about page-aligning stored entries that do not exist here — the property that matters is
the ELF `p_align`, which is the one now gated.

Guarded in `verify_claims.sh` — every **AArch64** library in the shipped APKs must have all `LOAD`
segments aligned to at least 16 KB — with the ARM32 payloads exempt for the reason above. The guard
is proven able to fail: `mutation_test.sh`'s `align16k` case rewrites the shim's `p_align` from
`0x4000` to `0x1000` in place and the gate catches it (17/17 mutations detected, 0 skipped).

That control also caught a mistake before it became a finding. The first tester was built without
`-Wl,-z,max-page-size=16384`, so the *tester itself* was 4 KB-aligned and segfaulted on exec — which
looked exactly like "the shim fails to load on 16 KB". Only B, dlopening a system library that must
work, revealed the tester had never started.

**But the app does not become launchable, and that is an Android 36.1 property.** Controlled four
ways rather than assumed:

| what was varied | LAUNCHER resolves? |
|---|---|
| API 36 (Android 16) — the A56's OS | **yes** — `com.rovio.angrybirds/com.rovio.fusion.App` |
| API 36.1, **16 KB** pages | no |
| API 36.1, **4 KB** pages | no |
| API 36.1, our APK carrying the **original untouched Rovio manifest** | no |

So it is neither the page size (4 KB behaves identically) nor our de-phone-home manifest rewrite (the
original manifest behaves identically). The positive control passes on the same device —
`com.android.settings` resolves, and the image has 6 launchable activities — so the query works and
this package genuinely has none.

`dumpsys` shows the activity present with its `VIEW`/`BROWSABLE` deep-link filter, while
`MAIN`/`LAUNCHER` never registers; `am start -n .../com.rovio.fusion.App` answers *"Activity class
does not exist"*, even with `FLAG_INCLUDE_STOPPED_PACKAGES`. Both manifests still contain the `MAIN`
and `LAUNCHER` strings, so nothing was stripped by our tooling.

**Why this matters to the user.** The A56 runs Android 16 / API 36, where the game works and is
proven to. API 36.1 is a QPR-level update the phone may receive, and the most likely mechanism is a
platform policy against this app's very low `minSdk=16` / `targetSdk=26`. That would apply to
Rovio's original build exactly as much as to this port — it is a property of a 2016-era APK meeting a
2025-era platform, not damage done by the conversion.

**That next step was taken, and it does NOT fix it.** Patching `uses-sdk` in place (values only, no
length changes — same principle as `depermission.py`) and re-testing on 36.1:

| variant | outcome on API 36.1 |
|---|---|
| `minSdk=24 targetSdk=34` | install **rejected**: *"Targeting S+ (31 and above) requires an explicit android:exported when intent filters are present"* |
| `minSdk=24 targetSdk=30` | install **rejected**: *"Targeting R+ (30 and above) requires resources.arsc … uncompressed and 4-byte aligned"* |
| `minSdk=24 targetSdk=29` | **installs** (`dumpsys`: `minSdk=24 targetSdk=29`) — and still `No activity found` |

So the SDK levels are not the cause either. Also ruled out, each by measurement rather than argument:
the manifest declares no `<activity-alias>`; and the `android.intent.category.LAUNCHER` /
`android.intent.action.MAIN` pool strings are byte-clean in **both** our manifest and Rovio's
original (a `strings` dump appeared to show leading whitespace on one — an artifact of `strings`, not
of the file, checked with `repr()` on the parsed pool).

**The filter IS registered on 36.1 — resolution is what fails.** A full `dumpsys package` capture from
both images shows the launcher filter present and
byte-for-byte equivalent on each:

```
Non-Data Actions:
    android.intent.action.MAIN:
      … com.rovio.angrybirds/com.rovio.fusion.App filter …
        Action: "android.intent.action.MAIN"
        Category: "android.intent.category.LAUNCHER"
```

Same counts on both (`MAIN` 4, `LAUNCHER` 2, `com.rovio.fusion.App` 10). So the manifest parses
correctly on 36.1 and the component reaches the resolver table; what fails is resolution itself —
`resolve-activity` finds nothing and `am start` on the **explicit component** answers *"Activity class
does not exist"*.

Further eliminations, each measured on 36.1:

- **User/multi-user**: `am get-current-user` = 0, one user only, `--user 0` explicitly on both
  `resolve-activity` and `am start` — no change. `com.android.settings` resolves on the same device
  in the same breath, so the command works.
- **Credential-encrypted storage**: the one concrete difference between the dumps is `ceDataInode`
  (**360573** on API 36, **0** on 36.1), i.e. the app's CE data dir was never created. Waiting for
  `sys.user.0.ce_available=true`, dismissing the keyguard, and installing only then leaves it at 0
  and the resolve still fails — so it is a symptom, not the cause.
- **`query-activities -c LAUNCHER -a MAIN`** returns 6 launchable activities on the device and
  **0** matching our package.

The two raw dumps are **not in the repository** — `.gitignore` excludes `/reports/shots/*` apart from
`PROOF_*.png`, because everything under `reports/` is regenerable. (A commit message here claimed
they were committed; they were not, and `git ls-files --error-unmatch` says so.) Regenerate with the
`dumpsys package com.rovio.angrybirds` step against `ab-emu-36` and `ab-emu-361`; the excerpt above
is the part that carries the finding.

**What this does and does not license saying.** Confirmed: on this `android-36.1` system image the
app's MAIN/LAUNCHER filter never registers, while its VIEW/BROWSABLE filter does, and the same APK is
fine on API 36. Not confirmed: that a shipping Android 16 QPR phone behaves this way. This is an
emulator system image, and an unusually new one; treating it as proof about the A56's future firmware
would be exactly the over-reach this document keeps correcting. The honest status is **an unexplained
incompatibility on one newer image, cause unknown, not attributable to the port** — worth knowing,
not worth alarming the user with.

### R49. The VBO path the phone may take that the rig does not — bridged, and its exercise status is NOT knowable from the logs

R11 measured that the engine issues exactly one `glGetString(GL_EXTENSIONS)` per run, contains three
extension name strings, and that the rig advertises two of them but **not**
`GL_OES_vertex_buffer_object`. It flagged the consequence — if the A56 advertises that one, the engine
may take a path nothing here has run — and left it there. This pins down what that path costs.

**The path exists in the engine.** Its dynamic imports include the whole buffer-object family:
`glGenBuffers`, `glBindBuffer`, `glBufferData`, `glBufferSubData`, `glDeleteBuffers` (69 distinct GL
imports in total). So this is not a hypothetical branch: the code to use VBOs is linked in.

**Every one of them is bridged.** Verified individually in `bridge_gl.c` rather than inferred from the
aggregate coverage number — all five have entries. Combined with the coverage hard-gate (343 engine
imports, 0 unbridged), nothing on that path can fall through to an `UNIMPL→0` stub. That is the part
that actually bounds the risk: a first execution on the phone runs real bridges, not silent no-ops.

**What is NOT determinable, and I nearly claimed otherwise.** Grepping every `*_abshim.txt` for
`glGenBuffers` / `glBindBuffer` / `glBufferData` returns **0** across all runs, and the obvious reading
is "the VBO path is never exercised here". That reading is unsupported: **`bridge_gl.c` contains no
`LOG(` calls at all.** There is no per-call GL logging — the logs carry only aggregates
(`GL draws=`, `clears=1801`, `useProgram=17133`). Absence from a log is evidence only when the log
would have recorded the presence, and here it would not. This is the same absent-symptom error as R41's
`h_fatal`, reached from the opposite direction, and it was one step from being filed as a finding.

Determining it either way needs instrumentation in `bridge_gl.c` — which would change the shipping
binary, the APK hash and every reproducibility claim, to answer a question the phone answers for free
on first launch. Not worth it.

**Static xref analysis was also tried and does not work here**, recorded so it is not repeated: the
three extension strings sit at `0xa2c7b8` / `0xa2ced4` / `0xa2cf04` (file offset == VA, since the first
LOAD maps `0x0 → 0x0`), but searching for those as 4-byte literals finds **zero** occurrences for two of
them and one *data*-segment pointer for ETC1. The engine is position-independent, so string addresses
are formed `PC + offset` at runtime rather than stored absolutely, and `reports/eng.dis` covers only
`.plt` and `.text` — its `ldr` annotations name the pool slot, not the word inside it. So "which code
tests this extension" is not answerable by grepping addresses.

**CORRECTION to the paragraph that stood here.** It said the A56 advertising
`GL_OES_vertex_buffer_object` was "plausible — an ancient and near-universal mobile extension", making
the VBO path a likely device-first-run. That is wrong, and the rig had already said so in its own
output before I wrote it:

```
gl_caps.py:  not automatically a defect — GL_OES_vertex_buffer_object is a GLES1-era name
```

`GL_OES_vertex_buffer_object` is an **OpenGL ES 1.x** extension. Buffer objects are **core** in ES 2.0,
so no ES2 driver advertises the name — not SwiftShader, and not Mali/Xclipse. And this engine is
unambiguously an ES2 client: it imports `glCreateShader`, `glCompileShader`, `glLinkProgram`,
`glUseProgram`, `glVertexAttribPointer`. So the extension check fails identically on the rig and on the
phone, and **rig and device agree on this branch** rather than diverging.

Tested rather than reasoned, too: re-running the capture with `ABSHIM_GPU=guest` (a different GL
backend — the flag is now overridable, default unchanged) returns the **identical** list, 51 extensions
/ 1488 bytes. The backend cannot change it, because the shim dlopens the *guest's* `libGLESv2` either
way.

**Net for the phone, restated honestly:** the VBO family is bridged and remains untested here, but there
is no longer a reason to expect the phone to take that path either. What survives from this entry is
the bounded part — the five bridges exist, the coverage gate guarantees nothing on that path falls
through to an `UNIMPL→0` stub — plus the two method notes below, which are the durable value. The
extension diff in `ONDEVICE.md` is worth running for the *other* 51 entries, not for this one.

**Process note.** `gl_caps.py` already contained the GLES1 analysis and prints it every run. I built a
finding on top of its data without reading its conclusion, then had to correct my own entry. The rig
frequently already knows — read its output to the end before adding to it.

### R48. The 36.1 failure is located: the launcher FILTER is dropped, not the activity — and Android logs no reason

R47 refuted the SDK-level explanation. This narrows *where* the failure happens, on the **shipped**
artifact (targetSdk=26 — what you actually install, not a diagnostic variant), with all three premises
holding: control resolves, `install rc=0`, 27,173 logcat lines captured.

It also resolves a contradiction between the two earlier notes. R18 recorded the activity as *present*
with its deep-link filter; R47's `am start -n` answered *"Activity class does not exist"*. R18 was
right for the shipped APK:

```
Activity Resolver Table:
  Schemes:
      com.rovio.angrybirds-8.0.3:
        com.rovio.fusion.App filter 41f0945
```

The component **is** registered. The resolver contains a `Schemes:` section and **no action table** —
so the declared `MAIN`/`LAUNCHER` filter (its strings are in the manifest: LAUNCHER ×1, MAIN ×2,
BROWSABLE ×3) never enters resolution, while the URI-scheme filter does.

**And Android says nothing about it.** Every log line naming the package during install is routine:

```
LauncherAppsService  onPackageAdded: user=UserHandle{0}, packageName=com.rovio.angrybirds
LauncherApps         onPackageAdded 0,com.rovio.angrybirds
NativePermissionController  PackageState{packageName: com.rovio.angrybirds, targetSdk: 26, …}
```

No `PackageParser` / `ParsingPackageUtils` warning, nothing skipped, ignored, rejected or deprecated.
The launcher is even *notified* of the install. So the filter is discarded without a diagnostic.

**Where this leaves it.** Three things are now established rather than assumed: the install is clean,
the component survives parsing, and the loss is specifically at launcher-filter registration. What
remains unknown is the rule doing it — and answering that needs the 36.1 platform's own resolver
source, not another emulator run. The honest state is *characterised, not explained*.

**Still no impact on the phone.** The A56 runs Android 16 / API 36, where this APK installs and plays
(`PROOF_18`, 3 stars, 42920). 36.1 is a QPR the phone *may* later receive; if it does, R47 rules out a
targetSdk bump as the fix and this rules out a parse error as the cause.

**Four self-inflicted faults in this one experiment, all the same shape: reimplementing what the rig
already solved.**

| what I did | what caught it |
|---|---|
| repacked the APK inside the emulator image → `zip: command not found` | the script failed honestly rather than faking a result |
| reported install failure with `tail -2` → kept the bottom of a Java stack trace and **destroyed the actual `-124` reason** | had to repeat the run to recover it |
| queried the control **once**, 15 s after boot → premise failed on timing, not on the device | the premise check itself, which is why it exists |
| used a raw `adb install` after sourcing `lib_install.sh` → `Failure calling service package: Broken pipe (32)` | `lib_install.sh` exists precisely to absorb that |

The last one is the most embarrassing: I sourced the library written for that exact transient and then
hand-rolled the call anyway. Worth recording as a pattern — before writing a step, check whether
`port/validation/lib_*.sh` already owns it.

### R47. R18's only explanation is REFUTED: targetSdk is not why the app is unlaunchable on Android 36.1

R18 left one hypothesis on record — that 36.1 refuses to register this app's `MAIN`/`LAUNCHER` filter
because of its very low `minSdk=16` / `targetSdk=26`. It was never settled, because the one attempt at
`targetSdk=34` was rejected at install. It is settled now, and the answer is **no**.

**A ladder of install-time blockers appeared on the way**, each one only visible after the previous was
cleared — worth recording because anyone raising `targetSdk` on this APK will hit them in this order:

| `targetSdk` | what Android says |
|---|---|
| 26 *(shipped)* | installs; **launchable on 36, not on 36.1** |
| 30 | `-124: Targeting R+ (version 30 and above) requires the resources.arsc of installed APKs to be stored uncompressed and aligned on a 4-byte boundary` |
| 30 *(with `resources.arsc` Stored + aligned)* | **installs** — `Success` |
| 34 | rejected: *"Targeting S+ (31 and above) requires an explicit `android:exported` when intent filters are present"* |

Ours ships `resources.arsc` as `Defl:N` (643096 → 161837, 75%), which is fine at 26 and fatal at 30.
`port/tools/make_sdk30_variant.sh` removes and re-adds it with `-0`, zipaligns to 4 bytes **before**
signing, and asserts `unzip -v` really reports `Stored` rather than trusting the flag it just passed.

**The result.** With `minSdk=24 targetSdk=30` installed successfully on the 36.1 image:

```
resolve-activity -c LAUNCHER com.rovio.angrybirds  ->  No activity found
am start -n .../com.rovio.fusion.App               ->  Activity class {…} does not exist
com.android.settings                               ->  com.android.settings/.Settings   (control, resolves)
```

The control resolving on that same device is what makes the app's "no" a fact about the app rather than
about the query. So raising the SDK levels — the only mechanism R18 proposed — **does not restore
launchability**, and the cause of the 36.1 behaviour remains unidentified. That is a genuine narrowing:
the leading explanation is gone, and nobody should spend time on it again.

**Practical consequence for the phone:** unchanged and still fine. The A56 runs Android 16 / API 36,
where the shipped APK installs and plays (`PROOF_18`, 3 stars, 42920). 36.1 is a QPR-level update it
*may* receive, and if it ever does, this rules out "just bump targetSdk" as the fix.

**Two self-inflicted process faults in this experiment, both instructive:**

- The first attempt repacked the APK *inside the emulator image* and died on `zip: command not found`.
  Measured afterwards: `ab-port` has `zip unzip apksigner python3`; `ab-emu-361` has only
  `unzip python3 adb`. The rig's existing split — build in `ab-port`, run in an emulator image — was
  right and I had ignored it. It failed honestly (`DONE (FAIL=1)`) rather than inventing a result.
- Worse: the install-failure reporter used `tail -2`, which on a rejected install is the **bottom of a
  Java stack trace**. The actual `-124` reason was discarded *by the line whose only job was to report
  it*, and the run had to be repeated to recover it. The moment a diagnostic matters most is the moment
  it must not be truncated. Now the whole installer output is saved and the meaningful lines grepped.

Also removed from that script: an image-wide "launchable activities" count that printed **0** where R18
counted 6 — the query form differs on this build, so the number measured my `grep`, not the device. A
wrong number beside a correct result invites doubt about the result.

### R46. Fresh clone verified, and every run now names its own floors

Two loose ends from R42–R45, closed.

**The fresh-clone path, executed rather than cited.** `REPRODUCE.md` claims a clone builds the
deliverable bit-for-bit; that was last actually done in an earlier session against a hash three
revisions old, and twelve files were added to the tree since. So: `git clone` into a clean directory,
nothing pre-staged, `bash port/reproduce.sh`. Result — **byte-identical `27548721a456ea99…`**, matching
the working tree. All twelve new files present; `out/` and `work803/` correctly absent (built, not
committed) with `prepare_inputs.sh` unpacking the tracked `.xz`. **42 checks, 0 failed, 1 skip** — and
the skip is legitimate and self-describing (`out/…-audio.apk not built`: a bare clone builds only the
default variant, which is the gate's existing "not built here is NOT stale" distinction working). R45's
fix held there too: `[ OK ] all 179 imported symbol(s) resolve against the shim's own DT_NEEDED
libraries` ran rather than skipping.

**Every playthrough now annotates which of its own numbers are floors.** `capped_counts.py` existed
but nothing invoked it, so the R42/R43 defect — a log cap quoted as a measurement — could simply
recur. `saturated_report` in `lib_metrics.sh` is now called from the playthrough verdict.

Getting that useful took two corrections, both of them the failure modes this session keeps hitting:

- The first version printed *"5 counter(s) saturated"* beside a list of **eight** marker names,
  because it scraped names from every line rather than the `[FLOOR]` lines. Worse, the list included
  `[WAF]` — whose 41-of-64 is precisely the *real count* R42 went to the trouble of establishing. A
  report that contradicts the finding it exists to protect is worse than no report.
- The second version was correct but useless: five sites saturate in **every run ever recorded here**
  — `[audio-isolate]`@3, three `[S2]` sites, `[u16conv]`@14 — because they are early-boot and
  scheduler tracing that fills in the first moments regardless of build variant, API level, or run
  length. Verified identical across a diagnostic playthrough, a release playthrough and a save-test
  relaunch. Reporting them every time is noise, and a mostly-noise report gets ignored, which is how
  a real one gets missed.

So it reports **deviations from that baseline**, which gives exactly the discrimination R42 needed:

```
playthrough_abshim.txt   only the 5 always-saturated tracing sites; every other count is a real total
modplay_abshim.txt       only the 5 always-saturated tracing sites; every other count is a real total
emu_fatal_abshim.txt     1 counter BEYOND the baseline is a floor: [WAF]
```

`emu_fatal`'s 64 is named as a floor; `playthrough`'s 41 is confirmed as a total. Nobody has to read a
guard in the shim source to know which is which.

### R45. `reproduce.sh` — the command the user runs — skipped the one check that catches the `-lm` class of bug

Found by running the full reproducible build after this session's changes, which was itself the point:
prove the deliverable still comes out byte-identical. It does — `27548721a456ea99…`, unchanged. But
the run ended:

```
ALL CHECKED CLAIMS HOLD, but 1 check was NOT checked:
    [skip] import resolvability (no NDK sysroot stubs here - run inside ab-port)
```

`reproduce.sh` built the APK **inside `ab-port`** and then ran `verify_claims.sh` **on the host**. The
import-resolvability check needs the NDK sysroot stubs, which only exist in that image, so it skipped.

That check is R22: every symbol the shim imports, proven resolvable against real bionic stubs. It is
the generic form of the missing `-lm` bug — the one that would have crashed the app on launch on the
A56 with `cannot locate symbol "sin"`, and which API 25 masked. **So the single most safety-critical
check in the gate was the one the user's own build path did not run.**

It was not a false pass. The gate counted the skip and said so, because "a skip is not a pass" was
already built into it. But honest-and-absent is still absent, and a reader who sees
`ALL CHECKED CLAIMS HOLD` on the last line is not guaranteed to read the qualifier above it.

One-line fix: run step 3 in the same container step 2 already uses. Now **49 checks, 0 skipped,
0 failed** (50 since R52 added the appended-capture gate), and nothing about the artifact changed — the rebuild reproduces the same hash either way.

The general shape is worth keeping in mind: this defect could not be found by any check, because it
*was* the check not running. Only executing the user's actual path end to end surfaces that class.

### R44. All three write-after-free sites identified in the engine — the dominant one is COW `std::string` refcount teardown

R42 established that write-after-free fires ~41 times per run from three engine addresses. Those were
recorded as bare offsets. Naming them costs one pass over the existing disassembly
(`reports/eng.dis`) and tells a future maintainer where the actual bug is rather than where the
mitigation catches it.

The logged value is the guest **LR** — the return address *after* the deallocating call — so the call
is the instruction before.

**`engine+0xd4c40` — 33 of 41 events. GCC libstdc++ COW `std::string` `_Rep::_M_dispose`.**
Unmistakable from the shape:

```
d4c10:  sub   r2, r2, #4        ; r2 = &_M_refcount — 4 bytes below the data pointer
d4c14:  dmb   sy
d4c18:  ldrex r3, [r2]          ; atomic decrement of the refcount
d4c1c:  sub   r1, r3, #1
d4c20:  strex r12, r1, [r2]
d4c24:  cmp   r12, #0
d4c28:  bne   0xd4c18           ; strex retry loop
d4c2c:  dmb   sy
d4c30:  cmp   r3, #0
d4c34:  bgt   0xd4be0           ; OLD refcount > 0 -> not the last reference, nothing to free
d4c38:  mov   r1, r8
d4c3c:  blx   0x88d830          ; last reference -> deallocate
d4c40:  b     0xd4be0           ; <-- the LR the WAF log records
d4c44:  ldr   r3, [r2, #-0x4]   ; and here begins the NON-atomic twin (single-threaded path)
```

An `ldrex`/`strex` decrement of the word 4 bytes below the payload, freeing only when the old value
was not greater than zero, is the COW string `_Rep` and nothing else. This confirms from the
instruction stream what R42 inferred from canary deltas — and note the *direction* matters: the
canaries **increase** (`0x7→0x197`), so the offending write is not this decrement. It is a *different*
holder of the same `_Rep` touching it after this site freed it. Premature free, exactly as labelled.

**`engine+0x7c2cb4` (7 events) and `engine+0x7363a8` (1 event) — plain `free()`.** Both return from
`bl 0x374ac`, a PLT stub. Resolving it needed arithmetic rather than pattern-matching, and the
pattern-match would have been wrong:

```
374ac:  add r12, pc, #0xA00000     ; pc = 0x374b4
374b0:  add r12, r12, #0x82000
374b4:  ldr pc, [r12, #0x820]!     ; GOT slot = 0x374b4 + 0xA00000 + 0x82000 + 0x820 = 0xAB9CD4
```

`0xAB9CD4` is **not** among the `free@LIBC` addresses that `objdump -R` lists first
(`0xab98d0`, `0xaba9c8`, `0xabaa38`) — those are data relocations in `.rel.dyn`. Reading the
`R_ARM_JUMP_SLOT` table at the computed address gives `00ab9cd4  R_ARM_JUMP_SLOT  free@LIBC`. So they
are direct `free()` calls, but "the slot isn't in the list I grepped" would have concluded the
opposite.

**41 of 41 events now accounted for**, and no shim change follows. The root cause is a refcount that
is wrong before this code runs — in stripped static engine code, in a COW string implementation whose
copies are shared across the guest. galloc's targeted leak already neutralises it, bounded in absolute
terms (R42), and altering the shim would move the APK hash and every reproducibility claim to chase a
condition that is measurably benign. This is recorded so the next person starts from the answer.

### R43. A documented conclusion about audio rests on a log cap — the same defect as R42, in a load-bearing place

R42 found a documented figure ("bounded, ~64 tiny `_Rep`s") that was really the point where a log
stops printing. That raised an obvious follow-up: **how many other numbers in this project are log
caps?** The shim has thirteen distinct rate limits — `2 3 4 8 12 14 16 20 24 40 64 220 400` — so the
question is mechanical, and one of them landed on a conclusion.

```c
jni_entry.c:560   { static int am=0; if(am++<8) LOG("[audio] nativeMixData ENABLED ..."); }
```

The mixer log is capped at **8**. And the shim's own comment four lines above cites it as evidence:

```
jni_entry.c:543   * audiomod, audio): `nativeMixData ENABLED` appears 8x in each, so the mixer really is running,
```

From there the number became a conclusion, in `ONDEVICE.md` and in R-series notes: *"the guest
`AudioTrack` buffer never drains → the mixer fills ~8 buffers then blocks"*, attributed to the
emulator's audio backend failing headless.

**All three audio logs show exactly 8.** Three identical values across three independent runs on two
different API levels is what a cap looks like; a timing-dependent buffer limit would vary. There is
**no uncapped audio marker anywhere in the shim**, so the true mix rate is not measurable from a log
at all.

**What survives and what is withdrawn:**

- *Survives:* the mixer is running — ≥8 calls is a real lower bound, and `stack_chk_fail` and
  `h_fatal` are 0 in all three runs. The backend failure is independently evidenced by a distinct
  error string, `Could not init 'pa' audio driver`, reproduced even with a working PulseAudio null
  sink — that is an error message, not an inference from a count.
- *Withdrawn:* "the mixer fills ~8 buffers then blocks". Nothing measured supports the *stops* part.
  The mixer may be mixing continuously; 8 is simply where the log goes quiet.

This is mildly **good** news for the device: there is no evidence the mixer stalls, only that the
emulator cannot make it audible. But the honest position is that the rate is unknown, and saying so
is better than a number that came from a `printf` guard.

**Mechanized, and the limits of mechanizing it.** `capped_counts.py` now extracts every `n++<N` site
from the shim (**21** of them, caps 2–400) and reports which counts in which logs have saturated —
a saturated counter is a floor, full stop. Run across every log: 195 saturated counters, almost all
internal tracing (`[S2] do_call` at 24, `[u16conv]` at 14) that nobody quotes. Useful output: it
confirms `[WAF] = 41 of max 64` in the playthrough log is a **real count**, while `emu_fatal`'s 64 is
not.

Automatic cross-referencing against the documentation was attempted **twice and abandoned**, which is
itself worth recording:

- Keying on the bracketed tag made `[audio]` into the keyword "audio", and flagged six documents as
  quoting the cap. All six were false — I checked each one and the claim was not there. R43's fix had
  been complete already.
- Requiring a longer literal plus a word-boundary match on the cap value still produced false hits,
  because `(?<![0-9])8(?![0-9])` matches the 8 in `angrybirds-8.0.3` and `64` matches `64-bit`. These
  documents are full of version strings, sha256 digests and byte counts, so number-proximity carries
  no signal in them.

Both real cases were found by reading code. A third found that way is plausible; found by grepping
numbers, it is not — so the tool does the part that is mechanical and reliable and says why it
declines the rest, rather than shipping a check that cries wolf.

Corrected in `ONDEVICE.md` and in the R-series note. The stale comment at `jni_entry.c:543` is
**deliberately left alone**: no shim source has been touched this session, so every reproducibility
claim stands unchanged. It is recorded here instead. (Comment-only edits would in fact be
binary-neutral — no `__LINE__`/`__FILE__` appears anywhere in the shim sources, checked — but that
would still need a rebuild-and-rehash to *prove* rather than assert, and an unverified binary is a
worse trade than a stale comment.)

### R42. Write-after-free is happening on every run — known, mitigated, bounded, and invisible to every check in the suite

R41 asked what `h_fatal == 0` cannot see and found the zero-page absorber, which turned out to be
dormant (0 events everywhere). The same question asked of galloc's *other* self-healing mechanism has
the opposite answer: **it fires constantly.**

`grep '\[WAF\]'` across every log in the repo — 209 lines, including runs from **today**:

| log | `[WAF]` | build |
|---|---|---|
| `emu_fatal_abshim.txt` | **64** — at the log cap, so a floor | diagnostic |
| `launch_timing_abshim.txt` | 41 | diagnostic |
| `playthrough_abshim.txt` | 41 | diagnostic |
| `shadercap3_abshim.txt` | 36 | diagnostic |
| `emu_interactive_abshim.txt` | 27 | diagnostic |
| every release-build log | 0 | release |

`playthrough_abshim.txt` is the API-25 run from 2026-07-29 that reported `DONE (FAIL=0)` and
`WIN CONFIRMED from pixels`. It contained **41 write-after-free events** and nothing in the suite
looked.

**What it is.** Three distinct engine free-sites — `+0xd4c40` (33 of 41), `+0x7c2cb4` (7),
`+0x7363a8` (1) — and the canary deltas identify it precisely:

```
canary 00000007->00000197      canary 0000005f->000001cf      canary 00000055->000001c5
```

A near-constant increment to the **first word** of an already-freed block: a COW `std::string` `_Rep`
refcount. This is the documented residual UAF, not a new defect.

**Why it is tolerable.** galloc's targeted leak never reclaims the address of a block that was written
while quarantined, so the stale refcount write lands on memory nobody owns. That fix is active in
**release** too. And it is bounded in practice — the 20-minute soak measured
**RSS 613,960 kB → 620,732 kB, 101 % of the first sample, across frame[21601]**.

**Why it is nonetheless invisible, in two independent ways.**

- The `[WAF]` log is `#if defined(__ANDROID__) && !defined(ABSHIM_RELEASE)`. Verified against the
  shipped binaries rather than the source: the deliverable's `.so` contains `uaf-survive` **but not**
  `[WAF]`. So the artifact the user installs can never report this, and most validation here runs the
  *release* proxy.
- The log caps at 64 (`n++<64`). `emu_fatal_abshim.txt`'s exactly-64 is therefore a floor with an
  unknown total; `playthrough_abshim.txt`'s 41 is a real count.

**Deliberately REPORTED, not asserted.** `waf_report` in `lib_metrics.sh`, surfaced as a `[note]` in
the playthrough verdict. Three reasons an assertion would be wrong here, and the first is the
important one:

1. **`waf == 0` would pass on every release log for the wrong reason** — because the diagnostic is
   compiled out, not because nothing happened. That is precisely a check that cannot fail, on the
   majority of runs in this suite. The report says so in its own output instead of hiding it.
2. A non-zero count on a diagnostic build is expected and accepted; failing would paint every
   diagnostic run red for a condition deliberately tolerated.
3. The cap makes ≥64 a floor, so the number cannot carry a threshold.

What the report gives instead is the quantity next to the build that produced it, with the baseline
(41 for an API-25 diagnostic playthrough) stated, so a change in rate becomes visible rather than
being averaged into a green line.

**And measuring it corrected a documented figure that was never a measurement.** `REPRODUCE.md` said
the leak was *"bounded, ~64 tiny `_Rep`s per run"*. It is not bounded, and 64 is not a count — it is
where `galloc.c` stops printing (`n++<64`). The run that produced that number had hit the cap, and
`emu_fatal_abshim.txt` still shows exactly 64 for the same reason. A logging limit was written into
the record as a property of the allocator.

What the numbers actually are, across four independent diagnostic runs:

| run | span | events | first minute | steady rate | distinct blocks |
|---|---|---|---|---|---|
| `playthrough_abshim.txt` | 163 s | 41 | 29 | ~7/min | **41** |
| `launch_timing_abshim.txt` | 133 s | 41 | 24 | ~14/min | **41** |
| `emu_interactive_abshim.txt` | 112 s | 27 | 15 | ~12/min | **27** |
| `shadercap3_abshim.txt` | 126 s | 36 | 18 | ~16/min | **36** |

**Every event is a distinct block — zero repeats in any run — so leaks equal events**, and the rate is
sustained rather than a startup burst: the last event in the playthrough log lands two seconds before
the log ends.

So the correct statement is not "bounded" but "**negligible in absolute terms**": a few hundred small
`_Rep`s per hour, against a measured RSS of 101 % of the first sample over a 20-minute soak. That is
a much weaker claim than the one on record, and it is the one the evidence supports.

### R41. `h_fatal == 0` was being read as a healthy address space — and a whole class of memory faults is neutralised before it can ever be fatal

Every play assertion in this tree checks `h_fatal == 0` and treats it as evidence the run was clean.
Found while chasing a NULL-pointer question in the GL bridges: that reading is not sound.

`jni_entry.c`'s `UC_MEM_READ_UNMAPPED` / `UC_MEM_WRITE_UNMAPPED` handler maps **any** unmapped data
address to a fresh zero page and lets the guest continue:

```c
uint32_t pg = (uint32_t)addr & ~0xFFFu;
if (uc_mem_map(uc, pg, 0x1000u, UC_PROT_READ|UC_PROT_WRITE) == UC_ERR_OK) { scratch_pages++;
    static int nn=0; if (nn++ < 12) LOG("[uaf-survive] wild %s @0x%llx ... continuing");
```

This is deliberate and it is why the game survives the residual `std::string` UAF instead of dying at
level end. Three consequences that were not being accounted for:

- `pg = addr & ~0xFFF` means **address 0 is mapped like any other** — a NULL dereference is absorbed,
  not faulted. (Which answers the original question: the missing NULL guard in `h_glGetShaderiv` /
  `h_glGetProgramiv` does *not* fault loudly. It writes into a zero page and continues.)
- Bounded at 8192 pages / 32 MB, so it is not unbounded — but that is 8192 absorbed faults.
- **Only the first 12 are ever logged** (`nn++ < 12`), and `scratch_pages` is a file-local static that
  is never reported. So after twelve lines, a run that absorbed 8,000 wild accesses produces a log
  indistinguishable from one that absorbed none.

Put together: **`h_fatal == 0` is compatible with sustained memory corruption**, because the faults are
neutralised on the way to becoming fatal. The suite was treating an absent symptom as health — this
project's most repeated defect, sitting inside the assertion library written to stop it.

**Measured baseline: zero — across every log in the repo, including the long ones.** Not just this
session's short runs. Swept all 30-odd `*_abshim.txt` logs:

| log | lines | last frame | `[uaf-survive]` |
|---|---|---|---|
| `progression_abshim.txt` | 59,812 | **frame[26401]** | 0 |
| `soak_abshim.txt` (20-min soak) | 47,958 | **frame[21601]** | 0 |
| `rotate_abshim.txt` | 11,717 | frame[3901] | 0 |
| every other play/lifecycle log | — | — | 0 |
| **`emu_fatalR_abshim.txt`** | 16,880 | frame[6301] | **3** |

The two longest runs in the project — 21,601 and 26,401 frames — absorb nothing at all, so this is not
a short-run artifact.

**The one non-zero log is the useful part, because it proves the check is not vacuous.**
`emu_fatalR_abshim.txt` is dated **2026-07-26**, before the level-end fixes and before the galloc
size-class fix, and it caught the thing the net was built for:

```
[uaf-survive] wild write @0x144dde70 (pc=+0x741f4c) -> mapped zero page 0x144dd000, continuing (residual std::string UAF)
[uaf-survive] wild write @0x144de000 (pc=+0x741f4c) -> mapped zero page 0x144de000, continuing
[uaf-survive] wild write @0x144df004 (pc=+0x7425d8) -> mapped zero page 0x144df000, continuing
```

Wild pointers into the unmapped gap between `RG_GUESTDATA` (0x12000000) and `RG_ENGINE` (0x40000000)
— not NULL — from two engine PCs in the `std::string` code. **That run's `h_fatal` was 0**, which is
precisely the point: the corruption was real, the run reported clean, and only this separate signal
distinguishes them.

So the sequence is a genuine before/after: the mechanism demonstrably fired while the root cause
existed, and has been silent through every run since it was fixed, up to frame[26401]. A regression
that reintroduced it would now be caught rather than absorbed into a green log.

So `assert_playthrough` now checks it separately from `h_fatal`, and says plainly that the number is a
**floor rather than a count** because of the 12-line cap. Proven able to fail: a log with one
synthetic `[uaf-survive]` line exits non-zero.

**The shipping shim is unchanged.** Reporting `scratch_pages` at shutdown would be the better fix and
would also move the APK hash and invalidate every reproducibility claim, to improve reporting on a
condition measured at zero. The check lives outside the artifact instead.

**The first version of this fix was only half a fix, which is the part worth recording.** The check
went into `lib_playassert.sh` — and `h_fatal` was being used as *the* health signal in **eleven**
places. Closing one and leaving ten is a cosmetic fix. There is now one implementation,
`assert_no_absorbed_faults` in `lib_metrics.sh`, wired into all of them: `lib_playassert.sh`,
`lib_saveassert.sh`, `emu_rotate.sh`, `emu_background_resume.sh`, `emu_reboot_persist.sh`,
`emu_progression.sh`, `emu_soak.sh`, `emu_16k_pagesize.sh`, `emu_interactive_capture.sh`,
`emu_launch_timing.sh`, `arm64_real_run.sh`.

Consolidating produced three further defects, each caught by an existing guard rather than by luck:

- **The self-contamination bug came back in a second file.** `test_saveassert.sh` also named its
  abshim log `LOG`, and the shared helper tees into `$LOG` when no `say()` exists — so the verdict
  was written into the log under analysis. Identical to the bug already fixed in
  `test_playassert.sh`; it reappeared the moment a second file started using `$LOG`. Renamed there
  too.
- **`arm64_real_run.sh` sourced nothing at all**, so the newly wired call resolved to nothing and
  would have died with "command not found" on the next arm64 run. `prose_as_code.py` caught it: an
  unsourced helper is a word in command position that resolves to nothing, which is the same class
  of defect as prose. It now sources `lib_metrics.sh`.
- **Both vacuity guards broke**, at 5→6 and 8→9 checks — which is exactly what they are for. Both
  now name `EXPECT_CHECKS` and explain that the hardcoded number is the point.

### R40. Thirteen pthread bridges report success without doing the work — provably single-threaded today, and now pinned

Applying this session's lens ("which verdicts outrun their evidence?") to the shim itself rather than
the harness. The shim-side form of a check that cannot fail is **a bridge that cannot fail**.

`dispatch.c` gates 13 pthread handlers on

```c
#define HAVE_SCH(d) ((d)->sch && sched_current((d)->sch))
```

and `sched_init()` memsets the scheduler, so `S->cur` is NULL until a green thread actually runs.
`dispatch_run_init_array()` calls `cpu_call()` **directly**, not through the scheduler. So for the
entire constructor phase `sched_current()` is NULL and those handlers take their fallback:

| handler | fallback when `sched_current()` is NULL |
|---|---|
| `h_mlock` / `h_mtry` / `h_munlock` | return 0 — *"locked"*, having locked nothing |
| `h_cwait` / `h_ctwait` | return 0 — *"the condition was signalled"* |
| `h_pjoin` | writes 0 to the retval and returns 0 — **without waiting** |
| `h_pself` | returns 1 (a fixed fake thread id) |

Single-threaded, all of that is semantically correct, and the ctor phase **is** single-threaded.
Measured, not assumed — `ABSHIM_LOG=1` makes `h_pcreate` log every creation:

```
  pthread_create calls during the ctor phase : 0
  ctors                                      : 125/125
```

**Why it is worth pinning anyway.** `h_pcreate` is gated differently from the others — on `!d->sch`
alone, *not* on `HAVE_SCH` — so it really does create a green thread whenever the scheduler object
exists, which it does during ctors (`sched_init` runs at `jni_entry.c:1481`, ctors at `1488`). The
safety of the whole arrangement therefore rests on one empirical fact: no constructor spawns a
thread. If a future engine build or shim change ever did, the guest would receive a real thread plus
a `join` that reports it already finished, and mutexes that do not exclude — silently, with no
diagnostic.

So the host suite now asserts it (`port/shim/test/run_tests.sh`, beside the ctors test), and the
assertion is proven able to fail: a doctored log containing one `[pthread_create]` line exits 1, as
does a truncated ctor count.

**Two bugs in that check on the way in**, both this session's own defect classes:

- It first incremented a `FAILED` counter. `run_tests.sh` signals failure through `set -e` and a
  single `ALL MODULE TESTS PASSED` at the end, and **nothing reads `FAILED`** — so it would have
  printed `[FAIL]` and let the suite report success. Now `exit 1`.
- `grep -c` exits 1 when the count is zero, and the file runs under `set -e` — so the first version
  aborted the entire suite precisely on the **healthy** path. A check that breaks when it should pass
  is not a weaker check, it is a different bug.

**Not changed: the shipping shim.** Adding a counter or a warning inside `dispatch.c` would alter the
binary, the APK hash and every downstream reproducibility claim, to guard a latent condition that is
measurably not occurring. The invariant is enforced outside the artifact instead.

### R39. The arm64 wall, located exactly: the kernel boots, Android init runs, and SurfaceFlinger can never get a graphics composer

R17 left the arm64 route "untested, not excluded". It is now tested, and this is how far it goes.

**Five blockers came out.** Each was hidden behind the one before it, so each had to be removed to
see the next:

| # | blocker | how it was removed |
|---|---|---|
| 1 | `-soundhw hda` — Intel HDA is PCI-only | **binary patch**, one 8-byte token → `-pidfile` (`port/tools/patch_emulator_arm64.py`). Not removable from outside: the AVD's `hw.audioInput/Output=no` **are** honoured — the argument is literally `hda:input=off,output=off` — and the device is added regardless |
| 2 | 11 × `virtio_input_multi_touch_pci_N` | AVD `hw.screen=touch`. Deliberately **not** patched: those literals are registered device *type* names and the binary also registers MMIO types of the same name, so renaming collides. Patching the `..._pci_%d` format string was tried and had no effect — the argv is built from the individual literals |
| 3 | `virtio-wifi-pci` | the emulator's own flag, `-feature -VirtioWifi` |
| 4 | SIGSEGV in `setupSubWindow` | `-no-skin -qt-hide-window` + `showDeviceFrame=no`. With every PCI device gone the machine assembled and the crash moved to the host window path: Qt building an `EmulatorQtWindow` with uninitialised geometry, `Negative sizes (-469898510,-939797020)`, despite `-no-window` |
| 5 | camera HAL crash-loop | AVD `hw.camera.back=none` / `front=none` (`vendor.camera-provider-2-4 exited 4 times before boot completed`, retriggering `sys.init.updatable_crashing`) |

**With all five applied, a genuine arm64 machine boots**: `Linux version`, kernel handoff
(`Freeing unused kernel`), Android `init` running, and zygote starting. Not a refusal, not a
configuration error — a booting ARM64 Android on an x86_64 host.

**The sixth blocker is different in kind and was not removed.** SurfaceFlinger cannot obtain a
graphics composer HAL, so it is SIGKILLed and restarted indefinitely, and every dependent service
loops with it:

```
init: Control message: Could not find 'android.hardware.graphics.composer@2.1::IComposer/default'
init: Service 'surfaceflinger' (pid 10675) received signal 9
init: starting service 'surfaceflinger'...
```

Measured across three GPU modes, all of which fail the same way — so this is a property of the
emulated GPU pipe, not a flag left unfound:

| `-gpu` mode | AVD `hw.gpu` | composer complaints | services restarting |
|---|---|---|---|
| `off` | `enabled=no` | 964 | audio-hal 135, zygote 126, netd 126 |
| `swiftshader_indirect` | `enabled=yes, mode=swiftshader_indirect` | 813 | audio-hal 121, zygote 114, netd 114 |
| `guest` | `enabled=yes, mode=guest` | 491 @600s | audio-hal 72, zygote 71, netd 71 |

**Why this is a limit rather than a to-do.** Google dropped arm64-on-x86_64 support; the ranchu arm64
composer HAL in the android-30 image needs a goldfish GL pipe that this x86_64-hosted build does not
wire up. No `-gpu` mode supplies it. The phone does not have this problem — it has a real Mali GPU
and a real composer.

**A trap worth recording: the boot log looks busy the entire time it is failing.** `init` line counts
climb linearly and steadily (~3,300 per five minutes) *because* of the restart loop, so an
init-line counter reads exactly like progress. The first attempt was allowed to run 30 minutes and
20,268 init lines deep on that signal alone. What actually distinguishes the two is whether services
are *repeating* — hence the crash-loop table above rather than a progress number.

**AND THE OBVIOUS NEXT TIER WAS TRIED: API 25, where the composer wall genuinely does not exist — and
zygote segfaults instead.** R17 named "try an older emulator/image" as the obvious next step, so it is
worth closing properly rather than leaving as a maybe.

The reasoning was sound and the prediction held. `IComposer` is a Treble/HIDL service introduced in
Android 8; on API 25 SurfaceFlinger loads `hwcomposer.ranchu.so` in-process, so there is no service
to be missing. `Dockerfile.ab-qemu-arm64-25` installs `system-images;android-25;google_apis;arm64-v8a`
(3.1 GB) — the same API level as this project's primary x86 validation, so a result would have been
directly comparable. Measured:

```
  IComposer complaints : 0          (on android-30: 964 / 813 / 491)
  surfaceflinger       : started ONCE, no loop
  Freeing unused kernel: yes        bootanim: running
  init lines @300s     : 1260       (android-30: 3315-6531 — the difference IS the absent loop)
```

So the R39 wall is genuinely a property of the android-30 image, exactly as predicted. What is behind
it is a different wall:

```
  init: Starting service 'zygote'          163 times
  init: Service 'zygote' (pid 10165) killed by signal 11      <- SIGSEGV
  init: Service 'zygote' (pid 10245) killed by signal 11
```

**ART cannot initialise.** zygote segfaults, init restarts it and the core service group with it
(`netd`, `media`, `audioserver`, `cameraserver` — 163 restarts each), forever.

**Conclusion, and why this route is now closed rather than merely unfinished.** Two independent API
levels fail in two independent ways — a missing HIDL composer on 30, a segfaulting runtime on 25 —
both inside the arm64-on-x86_64 path that Google dropped support for. This is not one bug with one
more flag behind it; the path is broken in several places at once. Further tiers (API 21-24, other
`-cpu` values) would be guessing against an abandoned code path, and the phone settles the same
question in ten minutes with a real Mali GPU and a real runtime.

**Status when written: the deliverable had never been executed. SUPERSEDED by R50 — it now runs on the physical A56.** `arm64_real_run.sh` is committed and would
carry it through install-and-launch the moment a composer exists; it refuses to report an arm64
result unless the APK really contains `lib/arm64-v8a/` **and** the guest's `ro.product.cpu.abi` really
is arm64, so it cannot be mistaken for one of the x86-proxy runs. The remaining route is the physical
A56 — see `ONDEVICE.md`.

### R17. "arm64 cannot be emulated on this host" is a claim about the LAUNCHER — the engine gets to a running machine and dies on an audio device

The deliverable had never been executed anywhere when this was written (SUPERSEDED by R50 — it now runs on the physical A56). The reason on record is that the Android emulator
refuses an arm64 AVD on an x86_64 host:

```
FATAL | Avd's CPU Architecture 'arm64' is not supported by the QEMU2 emulator on x86_64 host.
```

That refusal comes from the `emulator` **launcher**, before it builds anything. The engine it would
drive ships in the same x86_64 distribution:
`/opt/android-sdk/emulator/qemu/linux-x86_64/qemu-system-aarch64` — an ordinary x86_64 ELF that
accepts the full emulator option set, including `-avd` and an explicit `-avd-arch`.

Driven directly (`port/validation/arm64_boot_probe.sh`, image `port/docker/Dockerfile.ab-qemu-arm64`),
it does not refuse. It reports `Target arch = 'arm64'`, assembles a complete ranchu machine —
`-cpu cortex-a57 -machine type=ranchu -smp cores=4 -m 2048`, `kernel-ranchu`, `ramdisk.img`, five
virtio drives, `-android-hw` — **starts the QEMU main loop, and opens the control console on 5554
and ADB on 5555.** Then:

```
qemu-system-aarch64: PCI bus not available for hda
WARNING | QEMU main loop exits abnormally with code 1
```

The argv it generated contains `-soundhw hda`, and the arm64 ranchu machine has no PCI bus to hang
an Intel HDA device on. Same stopping point on three consecutive runs.

**That option cannot be removed from outside**, which was checked rather than assumed. The AVD's
`hw.audioInput`/`hw.audioOutput` are set to `no`, and the emulator's own generated
`hardware-qemu.ini` reads them back as

```
hw.audioInput = false
hw.audioOutput = false
```

— so the configuration propagates correctly and is then **ignored**: the argv still carries
`-soundhw hda`. `-no-audio` and `-audio none` make no difference either. The emulator builds that
argv in-process and calls QEMU directly, so there is no command line to intercept.

**One more route was tried and is unresolved.** The binary documents `-fuchsia` as "bypasses
android-specific setup; args after are treated as standard QEMU args", which allows replaying the
emulator's own generated argv with `-soundhw hda` stripped. Two attempts, neither of which reached a
boot: the first was written inline inside `docker bash -c` and its nested quoting silently failed to
remove the option, so the run failed for a reason that was mine rather than the emulator's; the
second, written as a file with a guard, refused to proceed — `-soundhw hda occurrences: before=1
after=1` — because the substitution still did not match the generated text. The guard is the useful
part: it declined to run a replay that would have failed identically and looked like a result.

So the route is untested, not excluded. Anyone resuming should start by dumping the exact bytes
around `soundhw` in the generated line rather than assuming its spacing.

**Status: not an architecture limitation, and not a boot either.** The blocker is one unconditional
device option in emulator 36.6.11's arm64 argv generation.

**The obvious next step was tried and FAILED.** ARM images on x86 hosts were routine for years, so an
older emulator looked promising. Downloaded 31.3.10 (build 8807927) and pointed it at the same AVD:

```
PANIC: Avd's CPU Architecture 'arm64' is not supported by the QEMU2 emulator on x86_64 host.
```

The launcher check is present there too, so the hypothesis is disproved for that version rather than
left hanging. Worth recording precisely *how* it nearly went the other way: the first look at 31.3.10
showed `Found AVD target architecture: arm64` and no refusal inside the observation window, and I
wrote down that it had "proceeded past the point where 36.6.11 fatals". It had not — the PANIC simply
comes later, after SDK-root resolution, and the run had died on a `Broken AVD system path` error
first (the image has no `platforms/` directory). Absence of the symptom was not absence of the check;
this is the third time in this investigation that reading has cost more than measuring.

What remains untried: an emulator old enough to still ship the pre-QEMU2 "classic" engine, and
upstream QEMU with an arm64 GSI — noting that the ranchu images depend on goldfish devices that
upstream QEMU does not have. Neither is close to free, and neither is needed for the deliverable to
be correct; they only bear on executing it *here* rather than on the phone.

Two traps the probe pre-empts, each of which would have produced a confident wrong answer. The
committed arm64 AVD specifies **`hw.ramSize=96M`**, which cannot boot Android 11 on any
architecture, so a run left as-configured would have failed for a reason with nothing to do with
arm64. And "no `-soundhw` in the argv" also happens when the run dies *before* generating an argv —
observed, and initially misread as progress — so the probe counts the argv line itself rather than
scoring an absent symptom as a solved problem.

### R16. Progression measured across six sequential levels — after two harness bugs that blamed the game

Multi-level progression rested on a single step. `PROOF_9` shows a second level loading after a win,
and everything past that was generalisation. `emu_progression.sh` was written to drive the
win → NEXT cycle deliberately and count **distinct `data/levels/<episode>/<name>` files opened**,
which is progression read from the asset bridge rather than inferred from frames.

Its first run won **once in five cycles** and reached two tutorial levels — no better than blind
input. The conclusion that suggested itself, and that I wrote into the script before examining
anything, was about the game: fixed coordinates cannot aim, so a harness cannot clear arbitrary
levels and progression past level 2 is not demonstrable here.

Both halves of that were wrong, and **every signal a log can carry supported it**: frames advancing,
`h_fatal` 0, the level file open, the process healthy. The screenshot the run had already captured —
`prog_5.png` — is a screenful of empty sky with `SCORE 0` and a live pause button.

**Bug 1: the view drifts.** `input swipe 207 118 110 150 700` appears in seventeen scripts here and
is called "the slingshot drag". It is a drag at a screen position. It launches a bird only when it
starts **on** the bird, and a drag that misses **pans the camera**. Cycle 1 hit the bird because the
tutorial puts one under that point; every later cycle missed and shoved the view further right until
the level was off-screen. Fixed in `lib_camera.sh`, called before every shot.

**Bug 2: the anchor is a constant for something that moves.** With the camera fixed the run still won
once — and `prog_2_pre.png` shows level 2 framed correctly, slingshot and loaded bird at (152,183),
spares behind, pig towers right, while the harness dragged from (207,118): open sky above the
slingshot. `find_bird.py` now locates the bird per shot and applies the proven pull vector relative
to it.

**Measured, API 34, shipping config — three runs, each superseding the last:**

| | 6 cycles | 10 cycles | **18 cycles** |
|---|---|---|---|
| distinct level files | 4 | 5 | **6** |
| wins confirmed from pixels | 3 of 6 | 4 of 10 | 5 of 18 |
| `h_fatal` | 0 (26 085 lines) | 0 (39 057 lines) | **0** (67 507 lines) |
| process | alive | alive | alive |

In order: `Tutorial_red_niko`, `Tutorial_red2_niko`, `Tutorial_chuck_niko`, `Tutorial_blues_niko`,
`Tutorial_bomb_niko`, `Tutorial_6_niko` — four bird types (Red, Chuck, the Blues, Bomb), each level
reached by winning the previous one and tapping NEXT. Every level transition survived: that is the
path where this port's deepest bug lived, a session-long `std::string` UAF that killed the process on
the results screen.

**Correction — "the whole tutorial episode" was wrong, twice.** At four levels this section implied a
ceiling; at five it claimed the episode was complete. The 18-cycle run then opened a sixth,
`Tutorial_6_niko`. The denominator was never measured, so it has now been read straight out of the
shipped APK: **`assets/data/levels/0_Tutorial/` holds 15 `.lua` files**, and the APK carries 20-plus
further episode directories (`1_PoachedEggs`, `10_Birdday5` … `21_PiggyFarm`). So the honest figure is
**6 distinct level files opened, all inside `0_Tutorial`** — the game has never been driven into a
non-tutorial episode here.

Nine of those 15 tutorial files (`birdquake`, `bubbles`, `kingsling`, `powerpotion`, `shockwave`,
`slingscope`, `terence`, `mayhem_niko`, `pucombo_niko`) teach power-ups and later birds and are not
obviously part of the opening sequence, so 6-of-15 is not the same as 40% of a linear episode. What
is certain is the direction: six sequential levels, and a seventh never reached.

**Why the other 27 episodes were never entered: the GAME locks them.** This was assumed to be a
harness limitation until a screenshot settled it. `prog_12_s1.png` is the episode-select screen, and
it shows `TUTORIAL` at **9/45 stars** — 15 levels x 3 stars, matching the 15 `.lua` files exactly —
with a **padlock on every other episode**: Flock Favorites, Danger Above, Short Fuse, The Big Setup,
Ham 'Em High, Mine and Dine, Surf and Turf, Bad Piggies. So "episodes never entered: 27" is the
game's own progression gating, not a defect in the port and not a failure of the driver. It also
confirms the port persists progress (9 stars accumulated across runs) and renders the full episode
carousel correctly.

That screen was reached by accident, which is worth recording. A vertical pan sweep was added on the
sound reasoning that the shot gesture drifts the view UP (`dy +32`) and a horizontal-only reset
cannot undo that — `prog_16_s2.png`, a pre-shot frame, really is a screenful of cloud. Measured, it
made things worse: **4 levels in 14 cycles, against 5 in 10 and 6 in 18** with the horizontal reset
alone, because a long vertical drag does not pan this game's camera — it navigates out of the level.
Reverted. The vertical drift is real and still unhandled.

`emu_progression.sh` now prints the denominator itself, computed from the APK, so the number can no
longer be read without its scale:

```
  episode 0_Tutorial: 6 of 15 level files opened
  episodes never entered: 27
```

A count with no denominator is an invitation to guess one, and the guess was wrong every time it was
made here.

Cycles 12-18 produced no wins and no new levels — the run stalled on `Tutorial_6_niko`, which the
fixed pull vector does not solve. That is a limit of the harness's aim, not evidence about the port.

**What it still does not show.** This is six files inside the tutorial episode, not the whole game
— `1_PoachedEggs` and twenty other episodes are present in the APK and have never been entered — and
the harness loses more often than it wins: 5 of 18. The losses are aiming, which is genuine and uninteresting. The
finder is a red-blob heuristic, and it is honest about that in its own header — on a tutorial
instruction card it will happily report the *illustrated* bird.

**Three corrections the run itself forced**, each visible in the log now: shooting stops once a level
is won (a "bird" at (242,145) was the win banner); an in-game tutorial card is dismissed when no bird
is visible (level 3 sat behind the "tap to launch" card while the harness shot at it); and the
slingshot is latched per cycle (a shot aimed at (236,213) while the slingshot was at (151,185),
having locked onto a spare bird walking up). Picking the largest red **blob** rather than the topmost
red *pixel* fixed a shot aimed at (288,62) — speckle on an animated hint-hand overlay — when the bird
was at (270,203).

Those two numbers came from a `prog_4_s2.png` that a later run has since **overwritten**;
`reports/shots/` is working output, not an archive, and this is the same way PROOF_8's source was
lost. Re-measured on the current capture the two rules still split the same way — topmost-pixel
(212,172) against largest-blob (261,194), and only the second is the bird — so the finding stands,
but the original frame is gone and the figures above cannot be reproduced from disk.

A yellow mask for Chuck was tried and **rejected on measurement**: it finds the bird (98 px) and also
sunlit foliage (2525, 3314 and 5307 px), which outranks it on the very frames red already handles.

Guarded in `verify_claims.sh` (a script that shoots across rounds must reset the camera first) and in
`mutation_test.sh` (deleting the pan must make the gate fail). Both of those checks were themselves
wrong first — see the commit; a guard that fires on correct code, or that quietly stops matching, is
its own defect.

### R15. Sustained operation: 20 minutes, frame[17401], memory flat — within the tutorial levels

Every run in this project was 5-10 minutes and stopped at level 2; the longest ever reached
frame[6301]. Nothing sampled memory at any point — `grep -l meminfo port/validation/*.sh` returned
nothing. So "it plays" was established and "it keeps playing" was not, which is the half a player
actually experiences.

That gap mattered more here than it would for a normal app, because the guest heap is this project's
own implementation (galloc, inside Unicorn's address space) and it has had a real corruption bug:
`galloc_check` once reported -5 permanently, 383 of 384 checks failing. A slow leak or creeping
corruption would have been invisible to every existing test.

Measured by `emu_soak.sh` on the **shipping configuration** (API 34, continuous input, sampled once
a minute):

| | |
|---|---|
| frames | **9001** — half again the previous record, with 0 stalled samples |
| `h_fatal` | **0**, across 22 420 shim log lines |
| resident memory | **611 608 kB → 616 164 kB**, +0.7% over ten minutes |
| process | alive at the end |

The RSS figure is consistent with the 512 MB Unicorn arena plus the app's own allocations, and it is
the shape that matters: essentially flat under active play rather than climbing. The assertion is
deliberately loose (fail above 2× the first steady sample) because no baseline existed before this
run — a threshold invented ahead of the data would either fire on correct behaviour or never fire.
Now that the trend is known, it can be tightened against evidence.

**Repeated at double the duration, which is what makes it a result rather than a data point:**

| soak | frames | RSS start → end | growth |
|---|---|---|---|
| 10 min | 9 001 | 611 608 → 616 164 kB | +0.75% |
| 20 min | **17 401** | 613 696 → 617 824 kB | **+0.67%** |

`h_fatal` 0 in both, across 22 420 and 39 164 shim log lines, 0 stalled samples either time.
**Doubling the session did not increase the growth** — that is the shape of no leak, since a leak
would roughly double it. The threshold has therefore been tightened from 2× to 1.3×, which is
justified by measurement rather than chosen in advance.

**What this does NOT show, checked rather than assumed.** The input is blind taps and swipes on a
timer, so "frames advanced" means the renderer kept running, not that the game kept progressing.
Reading the capture back: the 20-minute soak opened **two** distinct level files, both under
`data/levels/0_Tutorial/` — it did not advance through the game. The
`levelComplete` marker appears 10 times and means nothing here — it counts particle-script
preloads and was removed as a win metric for exactly that reason (it stayed constant even in
crashing runs).

So the claim is sustained **operation**: twenty minutes of continuous rendering and input
handling, 17 401 frames, no fatal, flat memory. Multi-level *progression* is evidenced elsewhere
(`PROOF_9`, `PROOF_21`), by scripts that check for a win screen rather than counting frames.

**Correction, then a re-measurement that corrected the correction.** This section said the run
"replayed the tutorial", which was never measured — `emu_soak.sh` captured no images, so nothing in
it could say what was on screen. `emu_progression.sh` then photographed the same fixed drag and
found empty sky (`prog_5.png`), so this was amended to "the soak was very likely drifting the same
way". That hedge has now been replaced by a run.

**Re-measured, 20 minutes with the camera reset in place:** `soak_start.png` is the tutorial properly
framed — slingshot, loaded bird, three spares, score 0 — and the session opens **two** distinct level
files, the same as before. So "it replayed the tutorial" was right, for a reason nobody had stated:
this script still shoots from the **fixed anchor** (207,118). That wins level 1, whose bird sits
there, and cannot win level 2, whose bird is at (152,183). Within each minute the missed drags pan
the view off the level, and the next minute's pan brings it back — which is why the first end frame
captured straight after a minute's drags was a screenful of cloud. The capture is now taken after a
pan, so it shows where the game is rather than the worst instant of the cycle.

Numbers from that run: **frame[20701], 0 stalled samples, RSS 613 960 → 620 732 kB = 101%,
`h_fatal` 0 across 47 956 shim log lines**, process alive. The measured claims never depended on any
of this — frames, `VmRSS` and `h_fatal` are read from the process, not the screen — but what the
input *achieves* is now measured instead of guessed, twice over.

Still bounded: twenty minutes is not an evening, and this is SwiftShader on x86_64. What it removes
is the possibility that the port only survives the first few minutes — and, now, that it leaks
steadily while it runs.

One transient worth recording: a 20-minute attempt failed with an EMPTY shim log because the app
never launched, and the script sat through its entire 550-second render wait before saying so. The
next attempt, unchanged, ran clean — so it was a launch flake, not a defect. `emu_soak.sh` now
confirms the process exists within 60 seconds and prints the system's own `ActivityManager`/crash
lines if it does not, because "no pid after nine minutes and no log" is a true statement that
explains nothing.

### R14. The project's foundational numbers, re-derived from the binaries

Every other finding quotes these, and none had been re-checked since it was first written. Audited
against the hash-pinned input, after two "checked a sample, generalised to the set" errors turned up
elsewhere in one day:

| claim | measured | verdict |
|---|---|---|
| 72 JNI exports | 72 `Java_*` FUNC symbols | correct |
| 343 UND FUNC imports | 343 | correct |
| 1092 dynamic functions | 749 defined + 343 UND = **1092** | correct |
| 125 `init_array` constructors | `.init_array` is 504 bytes = **126** slots, last one `0x0` | **correct — 125 callable** |
| 3217 asset entries | 3521 assets / 4398 zip entries | **wrong scope — corrected** |

Two are worth keeping written down, because each looks like an error until it is chased:

- **125 vs 126.** `size / 4` gives 126. The final slot is a NULL terminator, so there are exactly 125
  callable constructors. Anyone re-deriving this from the section size alone will get 126 and think
  the docs are off by one.
- **1092 vs 749.** Counting *defined* functions gives 749 and looks like a large discrepancy; the
  figure counts every dynamic FUNC symbol, and 749 + 343 = 1092.

The one real error was `3217`, which is the count under `assets/data/` and was written as "the APK's
3217 asset entries" — language that reads as exhaustive while excluding 304 files, all 287 of
`assets/files/` among them. That number underpins R10's premise that the shaders cannot be screened
statically, so the search was redone across **all 4398 zip entries**: zero files with a shader
extension, and zero of the previously-excluded 304 containing `gl_Position`/`gl_FragColor`/`varying`/
`precision`. The conclusion was right; it had simply been resting on a narrower search than it
claimed, and the wording is now what was actually done.

### R13. The one route out that no de-phone-home layer can block — and it stays shut on the play path

The brief was "remove all phone-homes and annoying internet access of **any type**". The four layers
deliver that for everything the app does *itself*, and every existing test measures that same thing:
`emu_jni_exception_probe.sh` asserts the app's own pid performs zero name resolution and zero socket
work. None of it touches a different route out — the app asking **Android** to open something:

```java
startActivity(new Intent(ACTION_VIEW, Uri.parse("market://details?id=…")))
```

That is not a socket in this process. It is an IPC to the system, which then hands the URL to Play or
a browser. No permission is required, and **no layer here intercepts it**. Worth stating plainly
because the four-layer claim is otherwise easy to read as total.

The capability is definitely present, and by design: `classes.dex` is byte-for-byte Rovio's, and it
contains `market://details?id=`, `android.intent.action.VIEW` and `http://www.rovio.com/eula`. The
game is not merely carrying that code, either — it **loads the rater**: `AppRater.lua` is opened 24
times and `TEXTS_APPRATER.dat` 72 times in a single playthrough. A "rate this app" prompt that sends
the user to the Play Store is precisely the *annoying* half of the brief.

Whether it fires cannot be read statically — the Lua assets are encrypted (magic `e393b813`, no
readable strings; the same encryption behind R1). So it is watched for instead, by
`emu_intent_probe.sh`, which captures **unfiltered** logcat (activity starts are a system tag; the
`-s abshim` capture every other script uses cannot see them at all):

| | |
|---|---|
| activity starts naming this app | 1 — its own `ACTION_MAIN`/`LAUNCHER` |
| outbound `VIEW` / `market://` / `http` intents | **0**, across a full playthrough including a level end |
| `h_fatal` | 0, over 7827 shim log lines |

The positive control is built in and is the reason the zero means anything: the game's own launch is
an activity start, so at least one line naming the app **must** appear. If none did, the probe would
be blind and it fails outright rather than printing a clean result.

**What this does and does not settle.** It bounds the risk to "not on the path a player takes from
launch through a level end", which is where a rater would most plausibly appear. It does **not**
prove the prompt can never fire: raters typically gate on session count or elapsed days, and one
automated playthrough is not a long-term user. The capability could not be removed without editing
`classes.dex`, which would forfeit the byte-for-byte authenticity that is itself a stated property of
this port — so it is recorded rather than patched. If it ever does fire on the phone, the failure
mode is bounded too: the Play app opens a page for a game that is no longer listed, which is an
annoyance, not an exfiltration.

### R12. The premise, reproduced: the original APK genuinely cannot install on 64-bit-only Android

This project exists because Angry Birds Classic 8.0.3 "cannot be installed anymore". That was taken
as given for the whole effort and never actually demonstrated — which makes it the oldest unchecked
assumption in the repo. It fell out of building `emu_signature_clash.sh`, which needed the original
APK installed somewhere:

    # API 34 (x86_64), installing Rovio's untouched com.rovio.angrybirds@8.0.3.apk
    Failure [INSTALL_FAILED_NO_MATCHING_ABIS: Failed to extract native libraries, res=-113]

The original ships `armeabi-v7a` and `x86` and nothing 64-bit. A 64-bit-only Android has no ABI it
can satisfy, so the package manager refuses it before any of the interesting questions — signatures,
targetSdk, Play Protect — are even reached. The A56 is in exactly that position for the ARM side of
that pair, which is the whole reason the shim exists.

It also bounds what the port had to solve. The refusal is `NO_MATCHING_ABIS`, not a crash, not a
missing-symbol failure, not a targetSdk floor: the original is a *complete, working* game whose only
disqualification is the instruction set of its payloads. That is consistent with the approach taken —
keep the engine byte-for-byte and re-host it — and inconsistent with the abandoned one, rebuilding
1092 stripped NEON functions.

### R11. The other half of the GPU surface: the engine's one capability query, and what it decides

R10 screened what the engine asks the driver to **compile**. This screens what the driver **tells the
engine about itself**, because the engine branches on it — and a rig and a device that answer
differently run different code. It was not in R9's table at all, which is the point: the table listed
eight surfaces and this was a ninth, found by asking what an Android game touches that the list did
not mention.

`libAngryBirdsClassic.so` imports `glGetString` and `glCompressedTexImage2D`, and contains exactly
three GL extension name strings — the signature of "read `GL_EXTENSIONS`, look for these, pick a
texture/geometry path":

    GL_OES_compressed_ETC1_RGB8_texture
    GL_OES_texture_npot
    GL_OES_vertex_buffer_object

**Measured** (`emu_gpu_capture.sh` → `gl_caps.py`, API 34, `-gpu swiftshader_indirect`): the engine
issues exactly **one** `glGetString` call in an entire run — `GL_EXTENSIONS` (0x1F03). It never asks
`VENDOR`, `RENDERER` or `VERSION`. The driver returns **51 extensions, 1488 bytes**; `ETC1` and
`texture_npot` are advertised, `vertex_buffer_object` is not. The full list is recorded at
`reports/gl_extensions_rig.txt` so a device capture can be **diffed** against it rather than
eyeballed.

**These asset facts cannot drift, so they are not separately gated.** Every static claim in this
entry — 57 `.pvr` uncompressed, 26 `.zstream` sheets (23 `RGBA4444` + 3 `RGBA8888`), one gameplay
asset tier, the 16 `TEXTS_*.dat` and their language tags — is a property of the **input** APK, and
`prepare_inputs.sh` refuses to build unless that file hashes to
`0580c3d3f79b21b344940bea65b8fadc22e8e5599c89dfe9b5e8a85004846b9a`. A build from a different input
fails before any of this could quietly change. Adding `verify_claims.sh` checks for them would be
machinery duplicating a guarantee that already exists, which is its own kind of defect: a second
mechanism that can disagree with the first. What is *not* covered by that hash — and therefore is
gated — is everything about the shim and the output APK.

**Why grepping the driver binary would have been the wrong test — demonstrated, not asserted.** The
first attempt read the extension names out of the emulator's `libGLESv2.so`. The string the engine
actually receives is the emulator's GLES **translator** list (`ANDROID_EMU_has_shared_slots_host_memory_allocator`,
`GL_EXT_debug_marker`, `ANDROID_EMU_gles_max_version_3_0` …), not SwiftShader's own — so the grep
screens a set the engine never sees. A string present in a binary is what a driver *could* say; only
the returned value is what it *did* say. That is the whole reason the dump exists.

**The compressed-texture branch has no shipped assets to act on.** Every texture in the APK is
uncompressed, so no GPU-specific compressed format ships at all:

| container | count | format |
|---|---|---|
| `.pvr` (PVR v3) | 56 | uncompressed — 32 `rgb`, 24 `rgba` (channel-name pixel format, not a compressed id) |
| `.pvr` (PVR v2 legacy) | 1 | uncompressed RGBA4444 — proven arithmetically: 203 × 86 × 2 = 34 916 = the header's recorded `dataLength` |
| `.7z` → `.zstream` sprite sheets | 26 | uncompressed — **23 tagged `RGBA4444`, 3 tagged `RGBA8888`** (ASCII tag at offset 0x20; the 40-byte header, read **big-endian**, ends exactly where the tag does) |

So `ETC1` being advertised changes nothing for the shipped data, and `glCompressedTexImage2D` — which
*is* bridged, so it would work — has nothing to upload.

**What this does and does not establish.** The rig answers the engine's query with `ETC1` and
`texture_npot` present; a GLES 3.2 device answers with those present too, so the engine reaches the
same capability state and takes the same branch. The one place rig and device could genuinely differ
is `GL_OES_vertex_buffer_object`: it is a GLES1-era name whose functionality is **core** in GLES2, so
no GLES2 driver is expected to advertise it and the rig does not — but if the A56's driver does, the
engine would take a VBO path nothing here has executed. That is the residual.

It is now genuinely one `diff` away from settled, which it was not when this entry was first written.
That sentence originally pointed at `ONDEVICE.md` for a comparison **no artifact could perform**: the
shipped APK is `-DABSHIM_RELEASE` and emits no `[gl-str]` line, so no device capture could produce a
list to diff — the claim described a procedure that did not exist. `ABSHIM_GPUCAP=1 build_apk.sh` now
produces an arm64 diagnostic variant (same signer, so it update-installs over the shipped APK and
back), and `ONDEVICE.md` carries the capture-and-diff steps against `reports/gl_extensions_rig.txt`.
It is a flag on the arm64 pipeline rather than a seventh build script specifically so it cannot drift
from it — most of all on `-lm`, whose omission once produced a binary that would have died on the A56
with `cannot locate symbol "sin"`.

**The largest texture actually uploaded: 2000 × 1991 — and zero compressed uploads.** The assets alone
could not answer this. Of the 1087 textures stored as plain zip entries (`.png`/`.pvr`/`.webp`) the
largest dimension is exactly **2048**, but the 26 `.zstream` sprite sheets are an opaque container:
the header yields a format tag and a 40-byte header size, no dimensions that reconcile with the
payload length, and a byte count bounds **area**, not a single axis. So the shim was made to report
what it actually receives at `glTexImage2D` — a running maximum plus every distinct internal format,
logged only when one of them changes, so the dump stays bounded instead of emitting thousands of
lines during the load phase it is measuring.

Measured over a full run (boot → tutorial → win → level 2), `h_fatal=0`, and **reproduced exactly on
an independent second run** (same 2000 × 1991, same zero compressed uploads, `h_fatal=0` across 8824
shim log lines) — so the figure is a property of the workload, not of one capture:

| | |
|---|---|
| largest upload | **2000 × 1991** — under 2048, which every GLES2 Android GPU supports |
| internal formats | `0x1908` (`GL_RGBA`) and `0x1907` (`GL_RGB`); types `GL_UNSIGNED_BYTE` and `GL_UNSIGNED_SHORT_5_6_5` |
| compressed uploads | **0** — and that zero is worth something only because `glCompressedTexImage2D` logs *unconditionally*, so it means "measured zero", not "never looked at" |

The compressed branch is therefore not merely unused by the shipped assets (which the container
formats already showed) but **observed** not to execute, on a driver that *does* advertise ETC1 — so
the null is not an artifact of the rig lacking the capability. That is the distinction this project
keeps having to make, and here it is settled in the favourable direction.

Scope, stated rather than glossed: 2000 × 1991 is the largest upload *this run touched*, and a run
covers the tutorial and level 2, not every episode's art. Anything larger would have to come from a
`.zstream` sheet, since the zip-stored set is statically bounded at 2048 — and no sheet is anywhere
near large enough to hold one. Bounding that needs the **per-file** format rather than an assumed
one: the biggest *file* is `INGAME_DINOS_DECOS_1` at 4 011 824 bytes, but it is `RGBA8888`, so it
carries only 1 002 946 pixels; the biggest by *pixel count* is `INGAME_BIRDS_1` at 1 114 153. Nothing
exceeds that, so a 4096-wide texture would need its other axis ≤ 272 px, and a 2048 × 2048
(4 194 304 px) cannot fit in any sheet at all — a far tighter bound than the uniform-format estimate
this paragraph previously carried.

**The phone's much larger screen cannot select an asset tier the rig never used, because there is
only one.** This was worth checking rather than assuming: the rig renders at 640 × 320 and the A56 is
1080 × 2340, and games of this vintage routinely pick a texture set by screen size — which would mean
the phone loading art no run has touched, and would undercut the upload bound above. The assets *are*
tiered, so the mechanism exists:

| asset directory | files | exercised in runs? |
|---|---|---|
| `data/images/base` | 129 | yes — 3932 opens |
| `data/images/1024x768` | 213 | yes — 2076 opens (**the only gameplay resolution tier**) |
| `data/images/themes_android` | 183 | yes — 247 opens |
| `data/images/1024x600_splash` | 9 | yes — 230 opens |
| `data/images/1024x768_splash` | 9 | **no — never opened in any run** |

There is exactly one gameplay tier (`1024x768`) and it is the one every run loads, so screen size
cannot route the engine to unexercised gameplay art. The only untested set is the alternate *splash*
tier, and it is nearly the same data: 5 of its 9 files are byte-identical to the exercised tier's,
2 are the `loadlist` manifests, and the tier differs by exactly **one image** —
`SPLASHES_SHEET_1_1024x768_1.png` at **1028 × 772** against the loaded `1024x600` variant's
1028 × 604. Same width, 168 px taller, and less than half the largest dimension already uploaded
successfully. So even if the A56's aspect ratio selects the other splash sheet, it is one image of a
size the pipeline has demonstrably handled.

**Run at the phone's actual screen, the argument above becomes a measurement.** Everything so far was
reasoned from a 640 × 320 rig. The emulator accepts an arbitrary skin, and screen geometry is one of
the very few A56 attributes this host can genuinely reproduce — so `emu_a56_screen.sh` runs at
**1080 × 2340**, confirmed from the device (`Physical size: 1080x2340`) rather than assumed:

| | |
|---|---|
| plays and **wins** | "LEVEL CLEARED", 3 stars, **39830** — frame[2401], `h_fatal=0` over 8928 shim log lines (`PROOF_21`) |
| touch mapping | **validated by that win.** Touch events reach the engine through the shim's JNI path, and nothing had exercised that mapping above 640x320. A slingshot drag that scores at 1080x2340 *is* the test |
| letterboxing | **none.** The capture is 2340 × 1080 — a landscape game on a portrait panel, so the transposition is the correct result — and the art is full-bleed to every edge (`PROOF_21`) |
| largest texture upload | **still 2000 × 1991**, unchanged by a 3.7× larger framebuffer |
| splash tier chosen | **still `1024x600_splash`** (20 opens) |

That last row settles the one loose end above: the alternate `1024x768_splash` set is *still* not
selected at the A56's 2.167 aspect, so the never-loaded tier stays never-loaded on the phone's own
geometry rather than merely on the rig's.

The no-letterboxing result is worth having measured rather than derived. The manifest declares
`resizeableActivity="false"` (Rovio's, unmodified — verified by decoding the binary XML, not by
grepping for the attribute name, which appears either way). Apps targeting below API 26 get a 1.86
max-aspect cap; this APK targets 26 specifically so that cap does not apply. The screenshot is what
turns that from a reading of the platform rules into a fact.

**The density was not cosmetic, and the first version of this run got it wrong.** A skin sets
resolution but not density, so that run left the AVD's 160 — and at 1080 × 2340 a density of 160
makes `smallestWidth` **1080dp**, which Android classifies as an **xlarge tablet**. This APK ships
`res/drawable-large-*` and `res/drawable-xlarge-*` buckets, so the run silently exercised the
**tablet** resource path, not the phone path the A56 takes. "Ran at the A56's geometry" was true of
the pixels and false of the configuration — the exact shape of error this file exists to catch.

Re-run at **420dpi** (the bucket for the A56's ~385ppi), confirmed from the device
(`Override density: 420`, `smallestWidth 411dp` → phone bucket), and every result above survives
unchanged: the win, `h_fatal=0` over 8934 lines, no letterboxing, the same 2000 × 1991 texture
maximum, and the same `1024x600_splash` tier. That last one now means something it did not before —
the splash selection is a **phone-class** result, so the never-loaded `1024x768_splash` stays
unloaded in the configuration the A56 will actually be in. The script now asserts the density and
fails if it is not what the A56 uses.

The remaining limit is the driver: still SwiftShader (R9/R11), so this says nothing about
Mali/Xclipse.

**And the density bucket is a bigger difference than "large vs xlarge".** Enumerating every
configuration qualifier the APK actually ships makes the point concrete:

| qualifier family | buckets present | what the rig selects | what the A56 selects |
|---|---|---|---|
| density | `ldpi` `mdpi` `hdpi` `xhdpi` `xxhdpi` `xxxhdpi` | `drawable-mdpi-v4` at 160 (144 files) | `drawable-xxhdpi-v4` at 420 (64 files) |
| screen size | `large`, `xlarge` (16 files) | *was* xlarge at 160 — the bug above | none (411dp is a normal phone) |
| layout direction | `ldrtl` (15 files) | never — the rig is `en-US` | only under an RTL locale |
| API level | `-v4 -v11 -v16 -v17 -v21 -v22 -v23 -v26` | highest applicable | same, API 36 |

So the default 640 × 320 rig has been exercising the **mdpi** drawables for this project's entire
history, while the phone will use **xxhdpi** — a disjoint set of files. The corrected run is the
first time the phone's actual bucket has been loaded at all.

**One configuration axis remains unexercised: `ldrtl`.** The APK ships 15 right-to-left drawables,
selected only when the device locale is RTL (Arabic, Hebrew, Persian). Every run here is `en-US`. The
blast radius needs stating precisely, and the first version of this paragraph got it wrong. It said
`ldrtl` "can only affect the thin Java-side surface", on the reasoning that the engine draws its own
UI from `assets/`, which ships no RTL variants. The files have no RTL variants; the *content* does.
Measured across the 16 `TEXTS_*.dat`:

- **`ar_AR` is a declared language in 11 of the 16 files** — Arabic is offered, alongside
  `de_DE en_EN es_ES fr_FR it_IT ja_JA/ja_JP pt_BR/pt_PT ru_RU zh_CN zh_TW`;
- but Arabic **glyphs** appear in only **two**: `TEXTS_LANGUAGE_SELECTION.dat` (24 sequences) and
  `TEXTS_MARKETING.dat` (254). Hebrew: **zero**, anywhere.

So nine files declare Arabic and carry no Arabic text — and the core one does not even declare it:
`TEXTS_BASIC.dat`, the 589 800-byte file holding the game's main strings, lists exactly ten languages
and `ar_AR` is not among them. The practical shape of an RTL run is
therefore: the language-selection screen can render Arabic, marketing strings can, and the rest of
the game falls back — while Android independently swaps in the 15 `ldrtl` drawables for the Java
surface. None of that is a *port* concern (the shim marshals bytes; it does not shape text), and the
APK ships the resources for it, so it was built for the case. It has simply never been run in it
here, and saying "no RTL variants at all" overstated how settled that was.

**Locale does not route the engine to different files either.** The same question, asked of the other
setting that varies per device: the rig runs `en-US`, the phone may not. There *are* per-locale
assets — `ML_LEAGUE_ASSETS_LEAGUE_LOCALIZED_<locale>` ships for 11 locales (`de_DE`, `en_EN`,
`es_ES`, `fr_FR`, `it_IT`, `ja_JP`, `ko_KR`, `pt_BR`, `ru_RU`, `zh_CN`, `zh_TW`) and none was opened
in any run. But the game's **text** is not tiered that way: `TEXTS_BASIC.dat` is a single 589 800-byte
file containing all ten language tags, 22 772 UTF-8 Cyrillic sequences and 19 020 CJK sequences, and
it is opened 69 times in every run along with the other 15 `TEXTS_*.dat`. Locale therefore selects a
**record inside a file the rig already reads**, not a path the rig never opens — which is engine-
internal and host-independent. The only locale-dependent *paths* are the Mighty League sheets, and
they belong to a feature a tutorial-to-level-2 playthrough never reaches, not to a device difference.

### R10. The shaders screened: every float declaration is precision-qualified, no extensions

The GPU is one of only two genuinely device-first surfaces (R9), and shader compilation is its most
likely failure: all validation runs on SwiftShader, which is lenient, while a conformant Mali/Xclipse
driver is not. A shader that compiles here could be rejected there, and the symptom would be a black
screen.

**Static screening was impossible.** There is no `.vsh`/`.fsh`/`.glsl` anywhere in the APK (all **4398** zip entries searched, not only the 3217 under `assets/data/`),
and no `gl_Position`/`varying`/`precision` string in the engine or `libjs.so` — the shaders are
**assembled at runtime from a preprocessor-driven uber-shader**, variants selected by `#define`
combinations (`ENABLE_ALPHA_BLENDING`, `ENABLE_PREMULT_ALPHA_BLENDING`, `ENABLE_TWOSIDED`,
`NEEDS_VTEXCOORD0` …). The only point at which the final text exists is the shim's `glShaderSource`
bridge, so a `#ifndef ABSHIM_RELEASE` dump was added there. **The shipped APK is unchanged** —
rebuilt byte-identical, and `strings` confirms `[shader-src]` is present in the diagnostic shim and
absent from the release one.

**The set is CLOSED, verified by playing a level.** The third capture ran a full playthrough on the
release-speed build — boot, menu, level load, dialog dismissal, three slingshot shots, through to
frame 1801, ending on a scored win (`[ WIN ] gold=0.0571 dark=0.5247 lum=58.0`). It compiled
**exactly the same 22 programs**: gameplay adds no variant that loading does not already produce, and
the 4 boot-time shaders are a strict subset. Three captures, two build configurations, one shader set:

| capture | build | shaders |
|---|---|---|
| boot only | full diagnostic | 4 |
| boot + load | full diagnostic | **22** |
| **full playthrough to a win** | release-speed `-DABSHIM_SHADERDUMP` | **22** (identical set) |

**The set is reproducible, which is the strongest completeness evidence available without the
device.** Two independent captures — one from the full diagnostic build, one from a release-speed
build carrying only `-DABSHIM_SHADERDUMP` — produced **the same 22 unique shader programs with
byte-identical hashes** (0 unique to either). So the set is deterministic and build-independent: the
diagnostic build's extra logging does not change which shaders the engine compiles. All 22 screen
clean; 11 vertex and 11 fragment, and genuinely distinct variants (fragment declaration counts differ:
32, 35, 36).

Originally captured on API 34, four shaders, **each verified complete by byte count before being screened**
(9150, 6008, 9132, 5990 — chunks reassembled to exactly the length the bridge reported):

| shader | kind | float-typed declarations | **unqualified** | `#extension` |
|---|---|---|---|---|
| 1 | vertex | 44 | **0** | none |
| 2 | fragment | 35 | **0** | none |
| 4 | vertex | 44 | **0** | none |
| 5 | fragment | 35 | **0** | none |

There is **no global `precision mediump float;` statement** — and that is not a defect. GLES 2.0
§4.5.3 gives fragment shaders no default float precision, so every float-typed declaration must carry
one; here **every one does**, individually (`uniform highp mat4`, `uniform lowp vec4`,
`uniform mediump vec4`, and all 43 locals per fragment shader). That satisfies the requirement the
global statement exists to satisfy.

Also absent across all four: `#version`, `#extension`, GLES3 `in`/`out`, `dFdx`/`dFdy`,
`texture2DLod`, `gl_FragDepth`, `discard`, and dynamic loop bounds — the constructs that most often
separate a lenient implementation from a conformant one. This is conservative GLSL ES 1.00, which is
what one would expect from a title that shipped across thousands of Android GPUs.

**This does not prove the shaders compile on the A56** — only the device can. But the specific,
most-likely failure mode has been checked and is not present, which is a materially better position
than "unknown".

*Two wrong readings preceded this one, both worth recording.* The first screen reported **zero of
every construct** — no `precision`, but also no `gl_FragColor`, no `texture2D` — which reads as "no
risks found" and actually meant "no data": the dump used `%.700s` and the shaders are 6000–9150 bytes,
so only the leading `#define` block was captured. A working renderer cannot contain zero
`gl_FragColor`, and that impossibility was the tell. The second reported **zero declarations at all**,
because the dump flattens newlines to keep each chunk on one logcat line, which leaves `//` comments
unterminated — so stripping comments deleted the entire shader. Both were failures of the measuring
apparatus that would have been quoted as facts about the game.

### R9. Every surface the phone touches, and whether the emulator actually stands in for it

R6–R8 each traced one path. Collected, they answer the question that matters before a device run:
*which of these did the test rig genuinely exercise, and which is the phone the first real instance
of?* Traced from the code and the run logs, not from the design intent.

| surface | how it works | does the emulator stand in for the A56? |
|---|---|---|
| **Assets** | forwarded to the **real `libandroid`** — `dlsym`/`dlopen("libandroid.so")`, `AAssetManager_fromJava` on the real `JNIEnv` | **Yes, by construction.** Android's own asset manager does the reading; our code only marshals |
| **Payload lookup** | `dladdr()` on the shim's own symbol → open `libengine32.so` beside it (R6) | **Yes** — same mechanism, same extracted-libs layout, asserted by `verify_claims.sh` |
| **Saves** | `/data/user/0/com.rovio.angrybirds/files/`, `.tmp`+rename (R7) | **Yes** — app-private internal storage behaves identically; no permission, no scoped storage |
| **JNI** | 72 engine entry points, thunk-for-thunk (asserted) | **Yes** — the same ART on both, and the symbol set is checked |
| **`/proc/cpuinfo`** | passes through to the host's real file (R8) | **No — and the phone is the *favourable* side**: the engine was written for AArch64 `cpuinfo`; x86 is the unusual input |
| **CPU / ABI** | Unicorn emulates ARM32 on the host CPU | **Partially** — allocation sequences are byte-identical on x86 and AArch64, and the whole suite passes on AArch64 under qemu, but the A56's cores are the first real AArch64 host |
| **GPU** | GL calls forwarded to the system driver | **No** — SwiftShader here, Mali/Xclipse there. Shader compilation is the main residual risk |
| **Audio** | mixer no-op in the shipped build; separate audio variant | **No** — the emulator's audio backend cannot init headless |
| **GL capabilities** | the engine reads `GL_EXTENSIONS` **once** and branches on three names (R11) | **Yes for the branch taken** — the rig advertises `ETC1` and `texture_npot`, as a GLES 3.2 device does, and `vertex_buffer_object` is absent on both. All shipped textures are uncompressed, so the ETC1 branch has nothing to act on. Baseline saved for a device diff |

This row was **missing** from the first version of the table. It was not found by re-reading the
table — a table cannot show its own omissions — but by asking what an Android game touches that the
eight rows did not mention.

Two other candidates were raised the same way, and this note first dismissed both as "covered by
rows already here". That was an assertion, and checking it turned out to be worth the effort in
both cases — which is the more useful lesson than the answers themselves:

- **Screen geometry** does not merely fall under an existing row. The assets *are* resolution-tiered,
  so the mechanism for the phone to load unexercised art exists; it is closed only because there is
  exactly **one** gameplay tier and every run loads it (R11).
- **RAM-derived settings** likewise: the engine really does carry `/proc/meminfo` + `MemTotal:`, so
  memory size really could have been a second host-derived input. It is closed only because that
  path is **measured never to execute** (R8).

Neither was covered by an existing row. Both are closed by measurement, which is a different and
stronger statement than the one this note originally made.

So of nine surfaces, **six are host-independent by construction, by assertion, or (R11) by
measurement**, one differs in the phone's favour, and **two — the GPU's driver behaviour and audio —
are genuinely device-first**. That is a narrower
statement than "validated on a proxy", and a more useful one: it says exactly where to look if
something goes wrong, which is why `ONDEVICE.md` now leads with a pid-scoped `logcat` rather than a
tag filter (the driver reports shader errors under the engine's own tag).

### R8. `/proc/cpuinfo` passes through to the host — and the phone is the *favourable* side of that

`fdtable.c` contains a `ROUTE_PROC` facility that would synthesise `/proc/cpuinfo`, `/proc/meminfo`,
`/proc/self/auxv` and `/proc/socinfo`. It is **not wired in**: `ROUTE_PROC` appears only in
`fdtable.c`/`.h` and `fd_route()` has no callers. `bridge_file.c` special-cases nothing but a
`/dev/urandom` diagnostic, so those opens reach the **host's real files** — confirmed in the run
logs, where `/proc/cpuinfo` opens succeed three times per run alongside
`/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq`.

So the engine parses genuinely different bytes on the A56 than in any validation run:

| | validation rig | Galaxy A56 |
|---|---|---|
| `/proc/cpuinfo` | `vendor_id: GenuineIntel`, `model name: Intel(R) Core(TM) Ultra 9 285K`, `cpu family`, `flags` | `Features : fp asimd evtstrm aes pmull`, `CPU implementer : 0x41`, `BogoMIPS` |

**The direction of that difference is favourable, and it is worth being explicit about why.** Angry
Birds 8.0.3 shipped to millions of real ARM phones; its device-info parser was written against, and
extensively exercised by, exactly the AArch64-shaped `cpuinfo` the A56 provides. The **x86 emulator
is the unusual input** here — content Rovio's parser has almost certainly never seen. On this one
path the test rig is the adverse case and the phone is the designed-for case.

That is not a guarantee, and it stays listed as device-only. But it is the opposite of the usual
"validated on a proxy, unknown on the target" caveat, so it should not be read as an outstanding
risk of the same kind as the GPU driver.

**`/proc/meminfo`: the code is in the binary and never runs.** `cpuinfo` is not the only host-derived
input the engine could take. `libAngryBirdsClassic.so` also carries `/proc/meminfo` and `MemTotal:`
as adjacent strings (0xa2cef8 / 0xa2cf08) — the open-and-scan-for-total-RAM shape — which would make
**memory size** a second such input, and an emulator's RAM is not the A56's 8 GB. If the engine
derived texture quality or a heap budget from it, the phone would take a branch nothing here has run.

Measured instead of assumed: across every capture, `/proc/cpuinfo` is opened **95** times (69
`fopen` + 26 `open`) and `/proc/meminfo` **zero** times.

That null is load-bearing, so the ways it could be wrong are enumerated rather than waved off.
`bridge_file.c` logs `fopen` and `open`; it does **not** log `openat`, which would be an invisible
route. But the engine's dynamic imports are `fopen`, `freopen` and `open` — there is no `openat`
among them, and `libjs.so` imports only `open` — so no unlogged path exists for it to have used. The
`MemTotal:` code is present and was not executed in any run: boot, tutorial, win and level 2, across
API 25, 34 and 36. And if the phone ever does take it, `/proc/meminfo` passes through to the real
file exactly as `cpuinfo` does, so the engine would read the A56's true memory rather than an
emulator's — the same favourable direction as the rest of this finding.

**A note on what the module tests actually cover.** It is not only `fd_route` that has no callers:
*every* exported function of `fdtable.c` — `fdt_create`, `fdt_destroy`, `fdt_alloc`, `fdt_get`,
`fdt_free`, `fdt_live`, `fd_route`, `fd_sandbox_resolve` — has **zero** references in
`port/shim/src` outside the module itself, while the live JNI table `handle_table.c` is called
throughout (`ht_new_ref` 14×, `ht_resolve` 10×). The module is nonetheless compiled into the shipped
shim (it is listed in `MODS`), and `test_fdtable.c` exercises it with assertions that pass and are
counted in *ALL MODULE TESTS PASSED* — among them `escape -> contained sandbox`, which reads like a
property of the shim and is not one: production `f_fopen` hands the guest's path to the host `fopen`
verbatim. Nothing misbehaves as a result — the app runs inside Android's own UID sandbox and the
engine is not adversarial input — but a green test for a module the product never calls is assurance
about nothing, and it is recorded here rather than left for someone counting passing checks.

### R7. Where the game's data actually goes — app-private only, so scoped storage never applies

Traced from the run logs rather than assumed, because "old game on modern Android" usually means a
storage-permission problem and this one has none.

Every file the guest opens in a full playthrough resolves under exactly four roots:

| root | opens (one run) | what |
|---|---|---|
| `/data/user/0/com.rovio.angrybirds/files/` | 74 | saves — `settings.lua`, `highscores.lua`, each written `.tmp` then renamed |
| `/proc/cpuinfo`, `/proc/socinfo` | 3 + 3 | CPU probing at startup |
| `/system/cpu`, `/system/soc` | 3 + 3 | ditto |

**No `/sdcard`, no `/storage`, no `/mnt/sdcard` appears in any captured run.** That is why the app
needs no storage permission at all, and why scoped storage — which broke a great many apps of this
vintage on Android 11+ — is simply not in the picture: app-private internal storage was never
restricted by it.

Two consequences worth knowing:

- Saves survive an **update-install** (same signer, same package) — **measured**, not inferred:
  `emu_update_install.sh` plays until saves exist, hashes them, installs the *audio* variant over
  the release build with no uninstall, and finds `settings.lua`/`highscores.lua` byte-identical;
  the round trip back is verified too. They are deleted by an
  **uninstall**. That is the mechanism behind the signer check in `verify_claims.sh` and the
  `INSTALL_FAILED_UPDATE_INCOMPATIBLE` row in `ONDEVICE.md`: a mismatched key forces an uninstall,
  and the uninstall is what costs the saves — not the reinstall.
- The `.tmp`-then-rename pattern means a save interrupted mid-write leaves the previous good file
  intact, so a crash during save cannot corrupt progress.

### R6. The payload lookup depends on `extractNativeLibs` defaulting to true

`load_engine_bytes()` finds the 32-bit engine by calling `dladdr()` on one of its own functions,
taking the directory of the shim's own `.so`, and `open()`ing `libengine32.so` beside it. That is
only valid because Android **extracts** native libraries to `nativeLibraryDir`.

With `android:extractNativeLibs="false"` the libs stay compressed inside the APK and `dli_fname`
becomes a path of the form `/data/app/…/base.apk!/lib/arm64-v8a/libAngryBirdsClassic.so`. `open()`
cannot resolve that, so the shim would load successfully and then fail to find its engine — dying at
`JNI_OnLoad` with no obvious cause on a build that otherwise looks correct.

The attribute is **absent** from this manifest and its default is `true`, which is why it works on
API 25, 34 and 36 alike. It is now asserted by `verify_claims.sh`, because the dangerous version of
this is not a bug someone writes — it is a *future rebuild* adding the attribute, or a targetSdk bump
where build tooling sets it, in which case nothing else in the pipeline would object.

Worth knowing if the loader is ever changed: reading the payload from inside the APK would need an
`AAssetManager`/zip path rather than `open()`, and is the alternative if extraction ever stops being
the default.

### R5. What the shipped APK can actually do — declared vs *granted*, measured on Android 16

Measured from both manifests and from the device, because "no phone-home" is a claim about what is
*absent*, and those are the claims that rot quietly. This entry previously listed **3** mangled
permissions and **2** kept, and asserted "nothing at `dangerous` protection level". All three of
those were wrong, and the last one was wrong in the direction that matters.

**Mangled — 10, not 3.** `depermission.py` flips the first letter, which is length-preserving so the
AXML string pool needs no offset fixups:

| original 8.0.3 | shipped arm64 |
|---|---|
| `android.permission.INTERNET` | `android.permission.XNTERNET` |
| `android.permission.ACCESS_NETWORK_STATE` | `android.permission.XCCESS_NETWORK_STATE` |
| `android.permission.GET_ACCOUNTS` | `android.permission.XET_ACCOUNTS` |
| `com.android.vending.BILLING` | `com.android.vending.XILLING` |
| `com.android.vending.INSTALL_REFERRER` | `com.android.vending.XNSTALL_REFERRER` |
| `com.google.android.c2dm.permission.RECEIVE` | `…XECEIVE` |
| `com.google.android.c2dm.permission.REGISTRATION` | `…XEGISTRATION` |
| `com.google.android.c2dm.permission.SEND` | `…XEND` |
| `com.google.android.finsky.permission.BIND_GET_INSTALL_REFERRER_SERVICE` | `…XIND_GET_INSTALL_REFERRER_SERVICE` |
| `com.rovio.angrybirds.permission.C2D_MESSAGE` | `…X2D_MESSAGE` |

**Kept:** `WAKE_LOCK`, `WRITE_EXTERNAL_STORAGE`, `INSTALL_PACKAGES`, `com.sec.android.airview.HOVER`,
`com.sec.android.airview.enable`.

**`WRITE_EXTERNAL_STORAGE` is `dangerous`, so the old "nothing at dangerous protection level" was
false.** `ONDEVICE.md` had it right and this entry did not. What settles it is not the declaration
but the grant, so that was measured on API 36 — the A56's own OS:

| | |
|---|---|
| granted at install | **exactly two**: `android.permission.WAKE_LOCK`, and `com.rovio.angrybirds.permission.X2D_MESSAGE` — the app's *own* mangled, self-declared permission, which grants it nothing |
| runtime permissions | **all `granted=false`** — `WRITE_EXTERNAL_STORAGE`, `READ_EXTERNAL_STORAGE`, `POST_NOTIFICATIONS`, `ACCESS_MEDIA_LOCATION`, and the four `READ_MEDIA_*` that Android 16 auto-expands a legacy app's storage request into |
| every mangled name | appears under *requested* and in **no** granted list — they resolve to nothing, exactly as intended |

So the app holds **one** real capability, `WAKE_LOCK`. The dangerous permission is declared and **not
held**: targetSdk 26 means runtime permissions must be requested, and the game never asks — which is
consistent with R7's independent measurement that all 48 files it writes are app-private and none
touch external storage.

`verify_claims.sh` asserts the absence of every live network/billing/account/push permission, and
`mutation_test.sh` proves that check fails when Rovio's original manifest is restored.
`emu_doc_verify.sh` now records the on-device grant state so the *declared vs granted* distinction
stays measured rather than argued.

### R0. Bug classes swept across the whole shim

Recorded here because otherwise it takes reading fifteen commits to learn what was actually
audited, and "we looked at X and found nothing" is as useful as "we found a bug in X".

Every one of these was found by **sweeping for a pattern**, not by chasing a symptom. Nothing here
produced a visible failure first — the game played and won throughout.

| class | swept | found |
|---|---|---|
| ignored `uc_mem_read`/`uc_mem_write` results | all modules | `m_read`/`m_write` fed callers stack residue; **12 real failures per run**, measured |
| byte-at-a-time guest string readers | all modules | **6** readers with uninitialised `ch` continuing a scan on stack garbage — the `m_read` fix covered one path into the class, not the class |
| allocations dereferenced without a NULL check | 38 sites | `handle_table` pools ×3, `sched`'s `getobj`, `fdt_create`, `idmap_grow`, `fstr_put` |
| guest-controlled sizes reaching a host allocation or copy | JNI + GL | `Set/Get<T>ArrayRegion` `len*esz` wrapped in 32 bits; `NewString` `len*2`; `glTexImage2D` `w*bpp*h`; `glUniformMatrix4fv` `cnt*64` |
| guest-controlled indices into fixed tables | all modules | **none** — `g_va[]`, fd, and thread tables were all already bounded |
| realloc that corrupts its own state on failure | all modules | `tmp()`, `g_idxbuf`, `g_vabuf`, `idmap_grow` recorded a capacity their buffer no longer had |
| ignored `uc_hook_add` results (a hook that never installs) | 39 sites | **33 ignored, 8 of them load-bearing** — `neut_s_construct_null`, `guard_empty_json`, `neut_gamelua_getter`×3, `neut_vfs_invalid_scheme`, `neut_rcs_login`, `h_svc`. These are *fixes*, so a silent install failure resurrects the bug they fix — including the level-end crash — with no diagnostic. Unreachable short of OOM at init, and empirically covered: see below |
| ignored `uc_reg_read` results (destination left indeterminate) | 118 sites | **1, unreachable, deliberately not changed** — `h_setjmp`'s `v` is written straight into the guest's `jmp_buf`, so residue would have become the saved r4–r11/sp/lr that `longjmp` restores. But `uc_reg_read` only fails on an invalid regid/handle and every id here is a compile-time constant, and `&v` escapes to an opaque call so no compiler can exploit the indeterminacy. Initialising it changes the shipped binary; see below |

Why the `uc_hook_add` finding is recorded rather than fixed, and what covers it meanwhile: the call
allocates, so it can only fail under OOM during init, at which point the app is already failing. More
usefully, the failure mode is **not** silent in practice — each guard logs a marker when it *fires*
(`[s-construct-null-guard]`, `[empty-json-guard]`), the playthrough scripts report those markers, and
a missing guard also shows up as the level-end crash returning and `h_fatal` going non-zero. So a
hook that failed to install would be caught by the existing play validation rather than by an inline
check. One wording defect was found in passing and is worth knowing: the install-time
`LOG("... hooking JSON parse 0x69814c")` is emitted *before* `uc_hook_add` and is conditional on the
guard's malloc, not on the hook succeeding — it reports an action it has not yet performed and never
verifies. It is a log line, not a check, but it should not be read as evidence the hook exists.

Why the one register finding was **not** fixed: the change is a single `= 0`, but it alters the
shipped binary, and this deliverable's trustworthiness rests on three *separately measured* claims —
a `--no-cache` from-scratch toolchain, a genuine fresh clone, and the documented `reproduce.sh` path —
each of which records a hash that a rebuild would invalidate. Paying that for a defect that cannot
occur, in code a compiler cannot miscompile, is the same trade already declined for the hot-stub
counter (R4). It is recorded in the source at the site so no future reader has to re-derive it. If a
deliverable change happens for another reason, this rides along with it.

The register sweep is the companion to the first row: `uc_mem_read`/`uc_mem_write` had been swept, `uc_reg_read` had not, and it is the identical shape. Unlike the memory case it produced no observed failure — register ids here are compile-time constants, so the reads do not fail in practice — which is exactly why it is recorded as *swept, one inconsistency fixed* rather than as a bug found.

Two of these were regressions **introduced during this work**, not pre-existing: changing `tmp()` to
return NULL without updating its 19 callers, and a first version of the GL size guard that folded
"legitimately zero" into "refused", which would have silently dropped 0×0 textures. Both were
caught before shipping — the first by sweeping, the second by asking what a zero-sized call does.
Neither had a test that would have caught it, which is why `test_gl_sizes.c` now exists.

Where a sweep found nothing, that is recorded above rather than omitted. A class that was checked
and came back clean is a different statement from one that was never checked, and only one of them
justifies confidence.

### R4b. R4 confirmed by an independent method: 12.3× the pixels costs 25% more frame time

R4 concluded, against what this project had assumed twice, that the **emulator dominates and the
rasteriser does not** — from instrumented timers inside the shim. A conclusion that important
deserved a check that shares none of that machinery, and the A56-geometry run (R11) supplies one for
free: it renders the same build at a very different resolution, so fill cost is the only large term
that changes.

| run | pixels | median steady-state fps | frame time |
|---|---|---|---|
| default 640 × 320 | 204 800 | 24.2 | ~41.3 ms |
| A56 geometry 1080 × 2340 | 2 527 200 | **19.4** | ~51.5 ms |

**12.3× the pixels, and frame time rises only 25%.** If rasterisation were the dominant cost the
frame rate would have collapsed toward 2 fps. Solving for the fill-rate share *f* at the small
resolution — `51.5/41.3 = (1 − f) + 12.3f` → `1.247 = 1 + 11.3f` → **f ≈ 2.2%** — which sits neatly
inside R4's independently-measured "5–7% outside the shim" (a figure that also includes
`eglSwapBuffers` and vsync, not just rasterisation).

Two different instruments, no shared code path, same answer. That is worth more than either alone,
because R4's own history is three wrong measurements in a row.

It also sharpens the A56 prediction rather than merely repeating it: the phone replaces a slice
measured here at roughly **2%**, so a real GPU cannot rescue the frame rate — single-thread CPU
performance sets it, exactly as R4 said. Caveats kept: SwiftShader need not scale linearly with area,
and the emulator's GL translation layer sits in the path, so treat 2.2% as an order-of-magnitude
agreement with R4's 5–7%, not a refinement of it.

### R4. Where per-frame time goes — measured twice over, and both prior assumptions were wrong

The release notes said the per-frame cost had "only been measured under software rendering, **where
the rasteriser dominates**". That clause was an assumption, and it is false. Correcting it raised a
second question the first measurement could not answer — ~92% of frame time is *inside the shim*,
but is that Unicorn executing ARM32, or this port's own bridge code running on the guest's behalf?
Those have opposite consequences: bridge cost is ours to optimise, emulator cost is close to a hard
ceiling short of replacing Unicorn. So the 92% was split.

Measured with `port/validation/emu_perf_split.sh` on the **release configuration**
(`build_apk_x86_perf.sh`: `-DABSHIM_RELEASE` so no heavy diagnostics, plus `-DABSHIM_PERF` for the
timers alone), API 34, driving actual gameplay rather than a menu:

```
[perf-split] IN-shim=11590ms = emulator=9447ms (75%) + bridges=2015ms (16%, 2617636 calls, GL 112ms) + JNI=127ms (1%: dispatch 1ms + ART-blocking 125ms)
[perf-split] IN-shim=11870ms = emulator=9441ms (73%) + bridges=2301ms (17%, 2408066 calls, GL  98ms) + JNI=127ms (0%: dispatch 1ms + ART-blocking 126ms)
[perf-split] IN-shim=12050ms = emulator=9550ms (74%) + bridges=2370ms (18%, 1843529 calls, GL  73ms) + JNI=129ms (1%: dispatch 1ms + ART-blocking 128ms)
[perf-split] IN-shim=34464ms = emulator=32172ms (89%) + bridges=2159ms (6%, 4482967 calls, GL 178ms) + JNI=133ms (0%: dispatch 1ms + ART-blocking 131ms)   <- level load
```

Percentages are of **wall** time, so they do not sum to 100: the remaining ~5–7% is OUT-shim
(`eglSwapBuffers`, rasterisation, vsync). There are two distinct regimes, and quoting either alone
would mislead:

| | steady gameplay | level load |
|---|---|---|
| emulator (Unicorn ARM32 + hooks + scheduler) | **73–75 %** | 89 % |
| native bridges (GL + asset + libc + file) | **16–18 %** | 6 % |
| — of which the GL bridge | ~0.6–0.9 % | ~0.5 % |
| JNI (dispatch + blocking ART call) | ~1 % | ~0.4 % |
| outside the shim (Java swap / vsync) | 5–7 % | 3 % |

**Consequences.**

- **The emulator proper dominates, and the rasteriser does not** — even under a software rasteriser.
  On the A56 a real GPU replaces only the ~5–7 % OUT-shim slice, so the phone's **CPU** single-thread
  performance sets the frame rate, not its GPU.
- **The bridges are a real 16–18 %**, not the ~0.9 % the GL figure alone suggested. Non-GL (libc)
  bridges are ~20× the GL bridge. Hottest by call count: **`floor`**, then `free`/`malloc`.
- Bridge dispatch costs **~0.5–1.3 µs per call** (weighted mean ~0.7 µs over all sampled windows,
  ~0.97 µs over the gameplay-only windows), at 1.8–4.5 M calls per 300 frames.
- ~24–25 fps on this x86 host is a **data point, not a prediction** for the A56 — different CPU,
  different memory system.

**A quantified optimisation that was deliberately NOT taken.** `h_floor` is one line — read the
double, call the host `floor`, write the result back — so nearly all of its ~0.5–1.3 µs is dispatch
overhead, not work: Unicorn must end the translated block to invoke a `UC_HOOK_CODE` callback.
Eliminating it means giving the guest real ARM32 code to execute instead of a bridged stub, i.e.
hand-written ARM float code that must be bit-exact with libm across NaN, ±inf, ±0 and the
beyond-int32 range. `floor` is ~20–24 % of bridge time ≈ **3–4 % of frame time**. Trading a
possible silent change in game physics for 3–4 % in a build that is bit-reproducible, play-tested
and hash-documented is a bad trade, so it is recorded here rather than done.

**THREE wrong measurements preceded this**, and the pattern is worth more than the numbers: not one
was caught by a test.

1. Timing `uc_emu_start` reported a **constant call count** while frames advanced 300 → 600 → 900.
   After boot the guest's whole render loop runs inside **one long-lived `uc_emu_start`**, bridges
   invoked from hooks during it — so the timer measured the entire run. Caught by the count not
   moving; the measurement had to be inverted.
2. Process-wide accumulators reported **IN 93 % + OUT 65 % = 158 %** of wall time. `shim_call` is
   entered from several ART threads, so summing their durations double-counts. Caught by arithmetic
   exceeding 100 %. All accumulators are now `__thread` — including `g_gl_ns`, which was a global
   compared against one thread's time while summing every thread's.
3. The JNI timer was placed on the RG_JNI hook alone and reported **`JNI=1ms`**. For a
   `Call*Method`, `env_dispatch_real` only *stashes* the call and redirects the guest; the real ART
   call runs later in `run_loop` **with the GEL released**, outside that hook. The true figure is
   ~127 ms/sample, and that time was landing in the residual and being reported as *emulator* — a
   blocking wait, during which the guest is not executing at all, inflating precisely the number the
   "CPU-bound on emulation" conclusion rests on. Caught by reading the code path, not by the output,
   which looked entirely plausible.

Because arithmetic was the only thing that ever caught these, `emu_perf_split.sh` now **asserts** the
impossible cases instead of leaving them for a reader to notice: GL ≤ bridges, bridges + JNI ≤
IN-shim, IN % + OUT % within 90–110, and entries ≥ frames. It also discards sample 1, whose window
contains boot (~140 s against ~12.5 s) — quoting it would be quoting the loading screen.

### R1. The ~2046 unreadable `E Lua` lines are an **engine** bug, faithfully reproduced

**Resolution: not our defect. No change made, and none should be made.**

**Symptom.** With *unfiltered* logcat (only `emu_jni_exception_probe.sh` captures one; every other
validation script filters to `-s abshim`, which is why this went unseen for so long), the app logs
`E Lua : Script paths are:` followed by ~2046 lines of binary rather than text. The injected
`assets/data/script_paths.json` holds 2035 entries, so the engine is dumping that list one line
each.

**What was ruled out first.** A temporary ABI diagnostic in `h_log` — non-release only, and
retargeted to fire only when the formatted result is non-printable — showed:

- `fmtbytes = 25 73 00` — the format string really is `"%s"`, read correctly
- the tag pointer resolves to a correct string
- the `%s` argument is `r3`, exactly what `marshal_pull_word` returns after `prio`/`tag`/`fmt`

So: not a wrong format string, not a misaligned vararg cursor, not a broken tag read.

**What settled it.** Following the `%s` argument one indirection:

```
arg=552ccabc  argb=bc be 16 50 | 7c 2a 65 40 …   argtxt='...P|*e@...'
deref=5016bebc  dereftxt='scripts/data/levels/0_Tu'
```

The `%s` argument points at a **`std::string` object** whose first word is `_M_p`, and the real text
is one dereference away — a script path, exactly matching "Script paths are:".

**Cause.** The ARM C++ ABI passes a class with a non-trivial copy constructor or destructor **by
invisible reference**. `std::string` qualifies. So `__android_log_print(prio, tag, "%s", someString)`
— passing a `std::string` into a varargs slot, which is undefined behaviour in C++ — places a
*pointer to the object* in `r3`. `%s` then prints the object's bytes.

This is the engine's own pre-existing bug. It would print exactly the same garbage on a real ARM32
Android device. Our formatter, our vararg marshalling and our string reads are all correct, and
**faithfully reproducing the engine's behaviour is the correct outcome** — special-casing `%s` to
guess at an indirection would corrupt every genuine `char*` argument.

**Confirmed harmless.** The game boots, plays, scores, wins and progresses with `h_fatal=0`, and the
level scripts these paths refer to demonstrably load.

**The diagnostic that found it** is kept in `dispatch.c` behind `#ifndef ABSHIM_RELEASE`. Proof that
it cannot reach the shipping APK: after adding it, a rebuilt `angrybirds-8.0.3-arm64.apk` is
**byte-identical** to the hash recorded before it existed.

### R2. `__android_log_write` silently dropped every message — fixed

`h_logw` read the message into a buffer and discarded it, with no forward to the real logger, unlike
`h_log` directly above it. Every engine diagnostic sent through the non-printf logging entry point
was invisible in logcat — including anything the engine might report right before giving up. It now
forwards exactly as `h_log` does.

### R3. De-phone-home layer 4 (Firebase/FCM auto-init) — proven on a GMS emulator, and on Android 16

Previously listed as open: every emulator tier ran an AOSP image with **no GMS**, so the one layer
guarding the one path layers 1–3 cannot close was asserted from the manifest and exercised by
nothing. FCM registration is performed by Google Play Services *on the app's behalf*, so it never
needs the app's own INTERNET permission.

`port/docker/Dockerfile.ab-emu-gms` adds an API-34 image with `google_apis` (GMS present, which is
the thing under test; the Play Store app is irrelevant here). `port/validation/emu_layer4_fcm_test.sh`
runs a **differential** test, because "we saw no registration" alone is equally consistent with the
kill-switch working and with nothing ever having tried:

| build | layer 4 | token-registration attempts |
|---|---|---|
| `…-x86shim-fbcontrol.apk` (control, built with `ABSHIM_FIREBASE_CONTROL=1`) | **removed** | **1** |
| `…-x86shim-release.apk` (shipped) | present | **0** |

The control logs `SERVICE_NOT_AVAILABLE. Will retry token retrieval`; the shipped build never
attempts it.

**Two corrections were needed before this result could be believed**, both worth recording:

1. The first run reported a clean `control=21, shipped=0` — and was **worthless**. The shipped app
   had been killed 11 ms after launch (`Killing <pid> (adj -10000): remove task`) by the
   uninstall/reinstall racing the launch, so its zero meant "never ran", not "did not register".
   The test now force-stops, waits, retries the launch, and **requires each arm to prove it executed**
   (a non-zero `abshim` line count) before its measurement counts at all.
2. The verdict compared *total Firebase line counts*. Both arms legitimately log the same benign
   "FirebaseApp failed to initialize" pair, so it was passing on a 3-vs-2 that could as easily have
   been noise. It now asserts on the **token-registration attempt** specifically — the phone-home
   the layer exists to stop — in both directions.

**Re-verified on Android 16 (API 36), the A56's actual OS**, on the `ab-emu-36` google_apis image:
GMS present, control attempts token registration once, shipped attempts it zero times, and both
arms proved they executed (2943 and 2977 `abshim` log lines). So the layer holds on the version the
phone actually runs, not only on API 34.

Caveat kept honest: the emulator runs `--network none`, so no token could be *fetched* either way.
What is measured is whether the app's Firebase messaging component **auto-initialises and tries**,
which is exactly what the kill-switch governs.
