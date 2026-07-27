# Release notes — PREPARED, NOT PUBLISHED

> **Status: draft, ready to publish.** No GitHub release or tag currently exists for this
> repository. Publish with:
>
> ```bash
> git tag -a v1.0.0 -m "Angry Birds Classic 8.0.3 → arm64 — v1.0.0" && git push origin v1.0.0
> gh release create v1.0.0 --title "Angry Birds Classic 8.0.3 → arm64 — v1.0.0" \
>     --notes-file RELEASE_NOTES.md \
>     out/angrybirds-8.0.3-arm64.apk out/angrybirds-8.0.3-arm64-audio.apk
> ```
>
> Everything below the line is the release body. Re-check the SHA-256 block against
> `sha256sum out/*.apk` before publishing — the deliverable changes whenever the shim does.

---

**Angry Birds Classic 8.0.3, running on an AArch64‑only phone** (target: Samsung Galaxy A56 / Exynos 1580) — the original 32‑bit ARM engine, *unmodified*, inside an embedded ARM32→ARM64 [Unicorn](https://www.unicorn-engine.org/) emulation shim loaded natively by Android's ART. All phone‑homes removed. Reproducibly built, fully offline.

The game plays and **wins levels** — full slingshot/Box2D physics, level‑complete, and multi‑level progression — screenshot‑proven on two Android generations. Please read **Scope of validation** below before installing: it says precisely what has and has not been run.

## Scope of validation — read this first

All runtime validation was performed with the **same shim source compiled for x86_64, running in x86_64 Android emulators** (API 25 and API 34). Every run log records the engine loading from `lib/x86_64/libengine32.so`.

**The arm64 APK in this release has not been executed on hardware.** It has been built, signed, aligned and statically audited. An arm64 AVD is not possible on an x86_64 build host — the emulator refuses with *"Avd's CPU Architecture 'arm64' is not supported by the QEMU2 emulator on x86_64 host"* — so an end‑to‑end arm64 run requires the physical device.

The case that the x86 rig predicts arm64 behaviour is reasonable but not airtight — identical shim source, and Unicorn runs the ARM32 guest faithfully on either host (125/125 C++ constructors execute clean on both, and the guest allocation sequence matches for at least the first 4913 requests). An earlier version of these notes claimed the guest heap was *bit‑identical* across host architectures; that is measurably false — it differs by about 64 KB after startup, deterministically per host, for reasons not yet identified. Both architectures execute the engine correctly, but they do not reach identical state, so “it passed on x86” does not by itself establish “it will behave identically on arm64”. The arm64 ABI itself is validated separately under qemu‑user: the real engine loads and all 125 C++ static constructors execute on AArch64. But that is emulation of the *ABI*, not the game — no Android, no ART, no JNI, no GL, no frames.

Treat this as a well‑engineered candidate build, not a shipped product. If it fails on your device, `port/ONDEVICE.md` maps every `abshim` logcat line to a diagnosis.

## Download & install

- **`angrybirds-8.0.3-arm64.apk`** — the deliverable (silent). Sideload it:
  ```bash
  adb install -r angrybirds-8.0.3-arm64.apk    # or copy to the phone and install via Files
  ```
  On first launch, tap through the one‑time *"built for an older version of Android"* system dialog (the APK targets SDK 26 on purpose, so Unicorn's JIT can run under W^X).
  On Samsung One UI you may need to disable **Auto Blocker** to sideload at all.
- **`angrybirds-8.0.3-arm64-audio.apk`** — experimental audio‑enabled variant (crash‑free, plays+wins with audio; *continuous* playback is pending on‑device confirmation — the emulator's host audio backend cannot init headless, so the buffer never drains there).

Both are signed with the same key, so either update‑installs over the other without losing saves.

**SHA‑256**
```
4fdb3d7cf31bf4d6f5556330c84bd32a36b134033c27aa41158101672d56c2b6  angrybirds-8.0.3-arm64.apk
702009356993bd21cafef7dc9b282058517acd10c0328cd259ddf6b425b20dc0  angrybirds-8.0.3-arm64-audio.apk
```

## What's verified

| | |
|---|---|
| **Plays & wins** | slingshot → correct Box2D arc → pig structure collapses → scores → "LEVEL CLEARED" → advances into the next level *(x86_64 proxy build)* |
| **Two Android generations** | full playthrough + win on Android 7.1 (API 25) *and* Android 14 (API 34); multi‑level + save persistence *(x86_64 emulators)* |
| **Authentic** | game engine, `libjs`, `libadcolony`, `classes.dex` byte‑for‑byte identical to Rovio's original 8.0.3 |
| **Modern Android** | targetSdk 26 installs; `libm` linked; ELF **16 KB‑page aligned** (loads on 16 KB‑page devices too); JIT runs under W^X |
| **No phone‑home** | four independent layers; **zero egress** verified |
| **Reproducible** | version‑pinned toolchain (NDK r26d + Unicorn @ fixed commit); bit‑identical rebuilds; whole image→APK pipeline reproduces the exact deliverable |
| **Falsifiable** | the emulator images and every test script that produced the evidence are in the repo (`port/docker/`, `port/validation/`) — the results can be re‑run, not just believed |

## Changes since the first build

- **Fixed a heap-corruption bug at its root.** The shim's allocator returned 8 fewer usable
  bytes than a real device for a family of small sizes (n = 1, 8, 17, 24, 33 …). Android's
  malloc uses 16‑granular small size classes, so `malloc(17)` yields 32 usable bytes; the shim
  gave 24. The engine legitimately writes past 17 bytes into that block — harmless on hardware,
  but under the shim it landed on the next heap chunk's header. Traced by checking the heap
  before and after every allocator and bridge call: the guest heap went permanently
  inconsistent by ~16 000 allocations into every session (383 of 384 checks failing) and is now
  clean for the whole run. Chunk sizing now matches the device's usable-size semantics.
  Measured memory cost is 6–21 % more heap depending on allocation mix (highest where small
  objects dominate, such as the constructor phase); an earlier 3 % figure came from a synthetic
  mix and understated it. In absolute terms the heap is ~605 KB after startup, against a 512 MB arena.
- **A diagnostic was removed from the release build.** `heap_ck()` ran a full guest‑heap + free‑list walk every 8192 malloc/free operations and was not gated out of release, unlike the sibling diagnostic beside it. Measured on the production path (real Unicorn backend, where every chunk header field is its own `uc_mem_read`), one check costs 0.45 ms at a minimal heap and 7.9 ms at 200k live chunks — against a 16.7 ms frame budget, on phone cores slower than the bench host. It is now release‑gated; the debug build keeps it. This is why the SHA‑256s differ from any earlier build.
- Validation scripts corrected: a `levelComplete` metric that actually counted particle‑script asset preloads (constant at 8 in every run, including crashes) was removed, and a fixed‑duration wait that made playthrough tests flaky was replaced with a frame‑based one.

## Reproduce it

```bash
git clone https://github.com/BigBIueWhale/angrybirds-8.0.3-arm64
cd angrybirds-8.0.3-arm64
bash port/reproduce.sh     # decompresses the input APK, builds the pinned image, converts → arm64 (offline)
```

The authentic 8.0.3 input APK (SHA‑256 `0580c3d3…`), the full shim source, the pinned Dockerfiles, and the fixed signing key are all in the repo — the deliverable regenerates byte‑for‑byte from this repository alone.

## Known limitations

- **Not yet run on real arm64 hardware** — see *Scope of validation*.
- Real‑time frame‑rate under the emulation lock, and the real Mali/Xclipse GPU's shader‑compile path, can only be confirmed on the physical A56 (validation uses software SwiftShader, which is itself the dominant cost in emulator frame timings and so does not predict on‑device rates).
- The engine performs roughly 900–1750 heap operations per frame in steady‑state play (measured),
  but the per‑frame cost of the emulation itself has only been measured under software rendering,
  where the rasteriser dominates. What a real GPU leaves for the emulator is still unknown.

---

*The port is © 2026 Ronen Zyroff, licensed **GPLv2** (the shim statically links Unicorn, GPLv2). Angry Birds Classic is **© Rovio Entertainment**, bundled unmodified; this project is not affiliated with or endorsed by Rovio. See `README.md` / `NOTICE`.*
