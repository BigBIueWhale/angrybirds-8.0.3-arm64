# Open findings

Things that have been **measured** but not resolved, kept separate from the docs that describe what
works so an unresolved issue is never quietly folded into a success claim. Resolved entries stay
below, with the evidence, so the same question is not re-investigated from scratch.

---

## Open

### 1. De-phone-home layer 4 (Firebase/FCM auto-init) is not exercised by any test

**Status:** open, and it is a real gap rather than an environment limit — worth stating plainly
because every other de-phone-home layer *is* verified.

The four layers are: (1) no INTERNET permission, so the kernel refuses the app's sockets;
(2) the shim's libc hard-fails socket calls; (3) manifest billing/accounts/push neutralised;
(4) `manifest_firebase_off.py` injects `firebase_messaging_auto_init_enabled=false` so Firebase
Cloud Messaging cannot auto-register a device token.

Layer 4 exists **specifically for the phone-home that layers 1–3 cannot stop**: FCM registration is
performed by Google Play Services *on the app's behalf*, so it does not need the app's own INTERNET
permission. That is the one path where the app can reach the network without owning a socket.

**The emulator has no GMS.** Unfiltered logcat shows `GooglePlayServicesUtil: Google Play Store is
missing.` and `Default FirebaseApp failed to initialize because no default options were found.` —
so Firebase never initialises there, and the kill-switch is never put to the test. **The A56 does
have GMS**, so this is exactly the environment where layer 4 matters and exactly the one we cannot
reproduce here.

What *is* verified statically: the injected `<meta-data>` is present in the shipped manifest
(`verify_claims.sh` asserts it), the Firebase config resources are byte-identical to Rovio's
original, and `FirebaseInitProvider` is left intact — so the surgery is minimal and targeted rather
than a blunt removal.

What would settle it: install on a GMS-equipped device and confirm no FCM registration token is
obtained. `emu_jni_exception_probe.sh` now prints an explicit `[SCOPE]` line when it detects GMS is
absent, so a green run cannot be mistaken for coverage of this layer.

### 2. Not verifiable from this machine

Not defects — limits of the environment. Stated so they are never implied to be covered.

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
