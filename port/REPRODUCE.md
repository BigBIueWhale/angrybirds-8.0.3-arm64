# Reproducible build: Angry Birds Classic 8.0.3 → installable arm64 APK

A fully automated, from-scratch pipeline that converts the original 32-bit (armeabi-v7a)
Angry Birds Classic **8.0.3** APK into an **arm64-v8a** APK that installs on a modern
AArch64-only phone (target: Samsung Galaxy A56 / SM-A566B, Android 16), with **all
phone-homes / internet access removed**. The 32-bit game engine is not recompiled — it
is run *unmodified* inside an embedded ARM32→ARM64 emulation shim (Unicorn).

Everything happens inside Docker. Two commands (or one, via `reproduce.sh`).


## Reproducibility: verified from scratch (2026-07-27)

Not just "the build script is deterministic on this machine" — the **toolchain image itself** was
rebuilt with `docker build --no-cache`, forcing a real NDK download and a fresh clone + compile of
Unicorn from the pinned commit, and the resulting image produced the **byte-identical APK**:

```
from-scratch toolchain : b143c6dd7f1933598ab38d6e313e4d94bdce7a2ebdf67fccc7c4e09467b4419d
cached toolchain       : b143c6dd7f1933598ab38d6e313e4d94bdce7a2ebdf67fccc7c4e09467b4419d
```

**Verified from a genuine fresh clone.** On 2026-07-28 the repo was cloned to a scratch directory —
committed content only, so no decompressed input, no extracted engine, no `out/` — and
`bash port/reproduce.sh` was run there with nothing pre-staged. It decompressed the input, gated
its sha256, extracted the engine, built the image, converted offline and verified the result,
producing `b143c6dd7f1933598ab38d6e313e4d94bdce7a2ebdf67fccc7c4e09467b4419d` — byte-identical to
the artifact built here. That is the claim that actually matters to a reader, and it is the one
this repo previously got wrong: several entry points read files that exist only after a build.

**And the documented path itself is run, not assumed.** On 2026-07-28 `bash port/reproduce.sh`
was executed exactly as published — inputs prepared and sha256-gated, image built, conversion run
offline, artifact verified — and produced
`b143c6dd7f1933598ab38d6e313e4d94bdce7a2ebdf67fccc7c4e09467b4419d`, the same hash recorded here,
with all 13 claim sections holding. Worth doing periodically rather than trusting: the entry point
the docs lead with is the one most likely to rot unnoticed, because everyday work calls the inner
scripts directly. That is exactly how its step 0 came to reference files that a fresh clone does
not have.

**The toolchain image itself is verified, not assumed.** On 2026-07-28 the committed
`Dockerfile.ab-port` was rebuilt from scratch with `--no-cache` into `ab-port:verify`, and the
deliverable rebuilt with that image came out **byte-identical** to the one built by the
long-lived `ab-port:latest`. So reproducibility runs all the way down: committed Dockerfile →
toolchain image → `build_apk.sh` → the exact APK that ships, with no step taken on trust.

The toolchain resolved independently to the same versions — apksigner 0.9 (deb 31.0.2-1),
cmake 3.22.1, NDK 26.3.11579264, Unicorn 7c5db941 — so the one genuine drift risk (apksigner and
zipalign come from `apt`, and a newer apksigner would emit a different signature block and change
the hash with no code change) did not materialise: Ubuntu 22.04's archive holds them stable.

Worth stating because the obvious check is misleading. Re-running `docker build` **without**
`--no-cache` completes in under a second by reusing the layer cache and re-tagging it — which
proves only that a cache exists on this machine, i.e. exactly the thing the claim is meant to
exclude. Any future re-verification must use `--no-cache`.

## Inputs

**Committed:**
- `apks/com.rovio.angrybirds@8.0.3.apk.xz` — the original 32-bit APK (Rovio-signed), xz-compressed
  because the raw file is ~100 MB. sha256 of the *decompressed* APK is `0580c3d3…`, and every build
  script checks it before doing any work, so a wrong or tampered input is rejected up front.

**Generated on first build (not in git — do not expect them after a clone):**
- `apks/com.rovio.angrybirds@8.0.3.apk` — decompressed from the `.xz` above.
- `work803/libv7/libAngryBirdsClassic.so` — the 32-bit engine, extracted from that APK.

`port/prepare_inputs.sh` creates both, and every entry point calls it — `build_apk.sh`, the three
x86 proxy builds, `build_offline_apk.sh`, and `port/shim/test/run_tests.sh`. This section previously
listed all of these as "committed in this repo", which was wrong and actively misleading: those
scripts read the files directly and every one of them failed from a fresh clone with an error
naming a file the clone never had.
- `port/shim/src/` — the emulation shim (C; built into `lib/arm64-v8a/libAngryBirdsClassic.so`).
- `port/docker/Dockerfile.ab-port` — pins the whole toolchain (NDK r26d + Unicorn 2.1.4).
- `port/debug.ks` — the fixed throwaway debug keystore (password `android`) the output is signed
  with, so every rebuild shares one signer identity → the APK is bit-reproducible *and* a rebuild
  update-installs over a prior install. (Delete it and the build mints a fresh random key instead.)

## Reproduce

```bash
cd apk-binary-analysis

# 1) Build the toolchain image from scratch (NDK r26d + Unicorn 2.1.4 for arm64/android).
#    This is the ONLY step that touches the network (outbound fetches; never binds a port).
docker build -t ab-port -f port/docker/Dockerfile.ab-port .

# 2) Convert the APK (reads apks/…8.0.3….apk, writes out/…arm64.apk) — fully OFFLINE:
#    every build tool is baked into the image, so the conversion needs no network at all.
docker run --rm --network none -v "$PWD":/work ab-port bash /work/port/build_apk.sh

# -> out/angrybirds-8.0.3-arm64.apk        (the silent, fully-validated default)

# 2b) OPTIONAL experimental AUDIO variant (separate APK; default above is untouched + byte-identical):
docker run --rm --network none -v "$PWD":/work ab-port env ABSHIM_AUDIO=1 bash /work/port/build_apk.sh
# -> out/angrybirds-8.0.3-arm64-audio.apk  (also bit-reproducible; crash-free + plays/wins with audio
#    off-device, but continuous playback only verifiable on real audio HW — see port/ONDEVICE.md)
```

Or just: `bash port/reproduce.sh`  (does both, then prints the signature/alignment check).

## Test the shim (optional, reproducible)

```bash
docker build -t ab-hosttest -f port/docker/Dockerfile.ab-hosttest .          # one-time (network)
docker run --rm --network none -v "$PWD":/work ab-hosttest \
       bash /work/port/shim/test/run_tests.sh                                 # offline; exits 0 on green
```
Compiles the shim **natively** and runs the Phase-A logic tortures (galloc/marshal/format/utf/
ctype/handle_table/fdtable/jni_arg/elf32), the nativeInit boot-drive (real engine under the
repo-vendored Unicorn), and the **coverage hard gate** (`coverage_check.py`: every one of the
engine's 343 UND FUNC imports must resolve to a bridge — 0 unbridged).

## Install + run (on the phone)

```bash
adb install -r out/angrybirds-8.0.3-arm64.apk     # or sideload via the file manager
adb logcat -s abshim                              # watch the shim boot
```

The shim logs its whole boot to logcat under the tag **`abshim`**:
```
abshim: engine 11248680 bytes …          abshim: guest JNI_OnLoad -> 0x10006 (OK)
abshim: init_array 125/125 (unimpl=0)    abshim: abshim ready (host pagesize=4096)
abshim: call[1] nativeInit (VII) @…      abshim: call[2] nativeResize (ZII) @…  …
```
If anything faults, the last `call[N]` line + a `shim_call … emu=… fatal=… pc=0x…`
line pinpoint exactly where.

## What `build_apk.sh` does (5 steps, deterministic)
1. Build the arm64 shim `.so` from `port/shim/src/` (regenerating the 72 `Java_*` thunks
   from the APK's `classes.dex` + the engine exports), linked against the Unicorn arm64
   static libs.
2. Unpack the original APK; strip network/tracking permissions in the binary manifest
   (`depermission.py`: INTERNET, network state, billing, accounts, all c2dm/push,
   install-referrer — mangled to invalid same-length names so Android grants none), then
   `manifest_firebase_off.py` injects `firebase_messaging_auto_init_enabled=false` to stop the
   GMS-mediated Firebase Cloud Messaging device-registration phone-home.
3. Replace `lib/` with `lib/arm64-v8a/` = the shim (as `libAngryBirdsClassic.so`) + the
   original 32-bit engine as the `libengine32.so` payload the shim emulates.
4. Repack + `zipalign -p 4`. All entry mtimes are normalised to the zip epoch (1980-01-01Z) so
   the archive is deterministic.
5. Debug-sign with `apksigner` (v1+v2+v3) using the repo's fixed `port/debug.ks` → installable,
   self-signed ("unsigned"-by-Play).

**Bit-reproducible:** with the pinned `ab-port` image, two independent `build_apk.sh` runs produce
a **byte-identical** APK (same whole-file SHA-256) — verified. Determinism comes from the fixed
signing key + normalised zip mtimes + the already-deterministic shim compile; `apksigner`'s
RSA/PKCS#1 signature over identical input is itself deterministic.

## No phone-home — four independent layers
1. **OS level:** no INTERNET permission → the kernel denies every `socket()` the app opens. This
   blocks *all* app-originated network: analytics upload, ad fetch, crash reports, Facebook, cloud
   save, install-referrer, connectivity checks. (Primary.)
2. **libc level:** the shim hard-fails `socket/connect/send/recv/getaddrinfo/…` (→ −1) for the
   emulated engine code (defense-in-depth for the guest).
3. **Manifest permissions:** billing / accounts / push (c2dm) / install-referrer declarations
   neutralised (renamed to invalid same-length names so Android grants none).
4. **Auto-collecting SDKs (source-disabled):** `manifest_firebase_off.py` injects `<meta-data>`
   flags each SDK's own code reads: `firebase_messaging_auto_init_enabled=false` (stops Firebase
   Cloud Messaging auto-registering a device token *via Google Play Services* — the one phone-home
   layer 1 can't stop, since GMS does that network for the app) and
   `com.facebook.sdk.AutoLogAppEventsEnabled=false` (stops the Facebook SDK's local event logging —
   verified: `AppEventsLogger.persistedevents` + `attributionTracking.xml` are no longer written).
   Each SDK still initialises normally — no stability impact; validated install+boot+play+save.
   *Residual:* the native Flurry / Rovio-BI analytics (`libengine32`) and HockeyApp still gather
   data **locally**, but it is **sendless** — layer 1 denies every socket, so nothing it collects
   can ever leave the device.

## Correctness status (what's proven on x86 vs. needs the phone)
Proven here (all green, gated): APK installs (signed/aligned/page-safe), engine loads,
125/125 C++ constructors run, the guest-thread scheduler is adversarially audited, and
**every one of the 343 libc/GL/file/network functions the engine imports resolves to a
real bridge** (enforced by `port/shim/test/coverage_check.py` as a hard gate in the test
suite).

**The game RENDERS AND PLAYS.** The *same* shim source — compiled for x86_64 instead of
arm64 (only the outer ABI differs; the emulated ARM32 core runs faithfully on either host —
though not to bit-identical guest state, see port/validation/README.md) — was run in a real Android x86 emulator (real ART + full app lifecycle). It boots
through `nativeInit → nativeResume → nativeResize → nativeUpdate`, auto-loads the tutorial
level and renders it; driving touch input then confirms **full interactive play** — a tap
advances into the level, and a slingshot drag **launches the bird along a correct parabolic
Box2D trajectory while the camera tracks it, **hits and collapses the pig structure, and
scores** — full rigid-body destruction physics (screenshots in `reports/shots/PROOF_*.png`);
the level then **completes** and the win-screen stars load. 300k+ GL draws.

**Level progression — fixed and screenshot-proven.** An earlier build crashed at the
level-*complete results transition* (a residual `std::string` `_Rep` use-after-free resurfacing
under the results-screen asset load → uncaught `io::IOException` → Lua panic). **Both level-end
crash paths are now eliminated at the source:** (1) the UAF is pinned by a **write-after-free
canary** in the guest allocator — a freed block that gets *written while held in quarantine* (the
tell-tale of a live stale pointer) is never reclaimed, so its address is never reused and the
stale write lands harmlessly (bounded, ~64 tiny `_Rep`s per run); (2) an ad-config *null*
`std::string` construction is caught by hooking `std::string::_S_construct` (NULL begin → a shared
empty string instead of `__throw_logic_error`). With both fixes the game **wins the level, renders
the “LEVEL CLEARED” results screen, and — on the next-level tap — loads and renders the following
level** (a fresh puzzle, score reset), all with `h_fatal=0` (screenshots
`reports/shots/PROOF_5…7_*.png`). Getting here also required, on top of the bridges: a
`glGetIntegerv` bridge over-write fix (was smashing a stack canary during level-load);
**de-phone-home** neutralisation of the blocking Google-Ad-ID / Play-referrer calls (they
otherwise hang the single emulation loop); and the S2 blocking-JNI GEL-release +
CLREX-on-context-switch. Because Unicorn's ARM32 emulation is deterministic across host
architecture, an x86 shim that plays in real ART ⇒ the arm64 shim plays on the A56.

This is validated on **modern Android** too: the targetSdk=26 APK installs and runs under
Android 14/API 34's W^X + install regime (Unicorn's JIT executes all 125 C++ ctors under W^X),
with the shim linked `-lm` (modern bionic needs the explicit libm dependency) and its ELF LOAD
segments **16 KB-page-aligned** (`-Wl,-z,max-page-size=16384`) so it `dlopen`s on 16 KB-page
devices (newer arm64 SoCs like the A56's Exynos 1580) as well as 4 KB ones.

What only the physical device now settles: on-device install + real-time performance under
the emulation lock — the play *capability* is proven above; the phone just confirms it runs
at a good frame-rate on the A56's own silicon.

**Audio.** The default APK is **silent** (mixer no-op) — the fully-validated shipped build. The
audio crash was since root-caused (a same-pthread JNI re-entrancy that aliased the guest stack)
and **fixed**; an optional `ABSHIM_AUDIO=1` variant now **plays and wins a level with audio active**
(validated on API 25 + 34, `reports/shots/PROOF_10_audio_levelwin.png`). What can't be settled
off-device is *continuous* playback: the emulator's own host audio backend won't init headless
(`Could not init 'pa'`) so the AudioTrack buffer never drains — an emulator limitation, not a shim
bug; the A56's real audio HW drains it. See `port/ONDEVICE.md` to build/try the audio variant.
Send the `abshim` logcat from the A56 to iterate.

## Security
Every container is outbound-only. Run with neither `-p` nor `--network host`; nothing here
ever listens on a socket (a hard requirement of the host environment).
