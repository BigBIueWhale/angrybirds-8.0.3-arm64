# Open findings

Things that have been **measured** but not resolved, kept separate from the docs that describe what
works so an unresolved issue is never quietly folded into a success claim. Resolved entries stay
below, with the evidence, so the same question is not re-investigated from scratch.

---

## Open

### 1. Not verifiable from this machine

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

### R3. De-phone-home layer 4 (Firebase/FCM auto-init) — now proven on a GMS emulator

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

Caveat kept honest: the emulator runs `--network none`, so no token could be *fetched* either way.
What is measured is whether the app's Firebase messaging component **auto-initialises and tries**,
which is exactly what the kill-switch governs.
