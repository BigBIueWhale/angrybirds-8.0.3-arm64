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
- **The physical Galaxy A56 run.** Everything is validated on emulators (API 25 and API 34, x86 with
  the shim in real ART). The A56's Exynos 1580 specifically is untested. See `ONDEVICE.md`.
- **Frame pacing under a real GPU.** All rendering evidence is SwiftShader software rendering.
- **Continuous audio on real audio hardware.** The audio variant plays through and wins on the
  proxy, but sustained playback needs real hardware.

---

## Resolved

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

Two of these were regressions **introduced during this work**, not pre-existing: changing `tmp()` to
return NULL without updating its 19 callers, and a first version of the GL size guard that folded
"legitimately zero" into "refused", which would have silently dropped 0×0 textures. Both were
caught before shipping — the first by sweeping, the second by asking what a zero-sized call does.
Neither had a test that would have caught it, which is why `test_gl_sizes.c` now exists.

Where a sweep found nothing, that is recorded above rather than omitted. A class that was checked
and came back clean is a different statement from one that was never checked, and only one of them
justifies confidence.

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
