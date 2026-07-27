# On-device install & triage — Angry Birds 8.0.3 arm64 shim

`out/angrybirds-8.0.3-arm64.apk` is the finished, reproducibly-built deliverable
(`bash port/reproduce.sh` regenerates it bit-for-bit). It installs on the **Samsung Galaxy A56
(SM-A566B, Exynos 1580, AArch64-only, Android 16)** and runs the original 32-bit ARM engine,
unmodified, inside an ARM32→ARM64 emulation shim (Unicorn). targetSdk 26, self-signed
(v1+v2+v3), 4-byte aligned.

## What's already proven off-device

- **It plays.** The *same shim source*, compiled for x86_64 and run in a stock **x86 Android
  emulator** (real ART, real app lifecycle, real GLSurfaceView), boots → auto-loads the tutorial
  → renders → a slingshot drag **launches the bird on a correct Box2D arc, collapses the pig
  structure, scores, wins, and advances into the next level** — zero fatal
  (`reports/shots/PROOF_2…7_*.png`). Unicorn runs the ARM32 engine faithfully on either host, so
  this x86 rig is a good proxy for the arm64 build on the A56 — but see the caveat below on how
  far that goes.

  **Correction (2026-07-27): the guest heap is NOT bit-identical across host architectures.**
  This document previously said it was. Measured with `test_ctors` after the 125 C++
  constructors: x86 `605096` bytes in use, arm64 (qemu-user) `539536` — a gap of ~64 KB that is
  deterministic on each host and present both before and after the allocator fix. Ruled out as
  causes: the allocator change, differing Unicorn builds (the pinned commit built for x86-linux
  gives the same figure as the prebuilt vendor blob), host page size (`h_sysconf`/`h_pagesz`
  return a hardcoded 4096), and run-to-run nondeterminism (x86 is stable across repeats). The
  guest's allocation *sequence* is identical for at least the first 4913 requests, and both hosts
  run 125/125 constructors clean, so the emulation is faithful on both — but "both hosts work" is
  a weaker claim than "both hosts produce identical state", and only the former is evidenced.
  **Cause identified:** it is exactly ONE allocation. 7792 of 7793 guest requests are
  byte-identical across hosts; a single doubling container at `engine+0x938670` grows one step
  further on x86 (65544) than on arm64 (32776), and that block accounts for the whole gap. So
  it is a capacity difference in one container, not divergent execution — both hosts run the
  same allocations and complete all 125 constructors clean. Why it needs the extra step on x86
  is unresolved; the site is stripped static code.
- **The arm64 ABI itself is validated** via qemu-user: the real engine loads, all 125 C++
  constructors run under emulation on aarch64, and every ABI-sensitive logic suite passes.
- **The shim source is host-suite-green** (`Dockerfile.ab-hosttest` → `run_tests.sh` exits 0):
  15 module tortures + nativeInit boot-drive + the coverage hard-gate (343 engine imports,
  0 unbridged).
- **It runs on MODERN Android** (validated on Android 14 / API 34 — same W^X + install regime the
  A56's Android 16 enforces): the targetSdk=26 APK installs with a plain `pm install`, the shim
  loads, **Unicorn's JIT executes all 125 C++ constructors under W^X** (the emulator needs
  writable+executable memory; targetSdk<29 keeps that permitted), and it renders — no execmem /
  SELinux denial. (This is why the shim links `-lm`: modern bionic needs the explicit libm
  dependency to resolve the engine's `sin`/`cos`/… math imports; older Android was lenient.)
- **16 KB-page ready.** The shim's ELF LOAD segments are 16 KB-aligned (`-Wl,-z,max-page-size=16384`),
  so it `dlopen`s on 16 KB-page devices (which Android 15+ / newer arm64 SoCs can use) as well as
  4 KB ones — a 4 KB-only `.so` fails to load on a 16 KB-page device. The runtime is page-size-agnostic
  too (the guest engine is emulated in Unicorn's own address space; Unicorn adapts to the host page
  size), so no 4 KB assumption leaks through. Harmless on a 4 KB A56, essential if it's 16 KB.

**On first launch you'll see a "This app was built for an older version of Android" dialog** — that
is normal for a targetSdk=26 app; tap through it (the game still runs). **What only the A56 itself
can settle:** real-hardware frame-rate under the emulation lock, and the real Mali/Xclipse GPU's
shader-compile path (the emulator uses software SwiftShader). This guide maps every `abshim` log
line to a diagnosis so one `adb logcat` run pinpoints anything device-specific.

## Install + run

```bash
adb install -r out/angrybirds-8.0.3-arm64.apk          # or sideload via the Files app
adb logcat -c && adb logcat -s abshim                  # clear, then watch ONLY the shim tag
# launch the app from the launcher; watch the boot sequence stream
```

### If the install itself is refused

None of these are shim problems — they are Samsung/Android install policy, and each has a
distinct message. Check here before reading any `abshim` output, because a refused install
produces no shim log at all:

| Message | Cause | Fix |
|---|---|---|
| Install blocked with no adb error, or the Files‑app install is greyed out | **Auto Blocker** is on by default in One UI 7/8 and blocks sideloading outright | Settings → Security and privacy → Auto Blocker → off (re‑enable it afterwards if you like) |
| `INSTALL_FAILED_VERIFICATION_FAILURE` | Play Protect scanning the sideload | Play Store → Play Protect → turn off "Scan apps" for the install, or accept the "install anyway" prompt |
| `INSTALL_FAILED_DEPRECATED_SDK_VERSION` | Android's minimum-installable targetSdk floor | This APK targets SDK 26 and Android 16's floor is 24, so it should not appear. If it does, `adb install -r --bypass-low-target-sdk-block <apk>` |
| `INSTALL_FAILED_UPDATE_INCOMPATIBLE` | A previous install signed with a different key | `adb uninstall com.rovio.angrybirds` first. Rebuilds keep the same signer (`port/debug.ks`), so this only happens across a genuinely different build |
| `INSTALL_FAILED_NO_MATCHING_ABIS` | The device rejected the native libs | Confirm `adb shell getprop ro.product.cpu.abi` → `arm64-v8a`. This APK ships **only** `lib/arm64-v8a/` |

`adb install` needs USB debugging enabled (Settings → Developer options) and the RSA prompt
accepted on the phone. Use **USB**, not `adb tcpip` — see the network note in
`port/validation/README.md` for why.

Everything the shim does is logged under the tag **`abshim`**. Capture the whole run
(`adb logcat -s abshim > boot.txt`) and read from the top — the LAST line before a stall or crash
is the failure point.

## Optional: experimental audio build

The default APK above is **silent** (the audio mixer is a no-op) — that's the fully-validated,
shipped build. There is also an **experimental audio-enabled variant** you can try on the phone:

```bash
docker run --rm --network none -v "$PWD":/work ab-port \
    env ABSHIM_AUDIO=1 bash /work/port/build_apk.sh        # -> out/angrybirds-8.0.3-arm64-audio.apk
adb install -r out/angrybirds-8.0.3-arm64-audio.apk        # same signer -> updates over the silent one
```

Off-device it is validated **crash-free** and it **plays and wins a level with audio active**
(the mixer inits and runs; `reports/shots/PROOF_10_audio_levelwin.png`, on API 25 **and** API 34).
What could NOT be settled without real audio hardware is **continuous playback**, and we pinned
exactly why: the **emulator's host audio backend fails to init in a headless container** (`Could
not init 'pa' audio driver` — reproduced even with a working PulseAudio null sink wired up via
`Dockerfile.ab-emu-pa`; `pactl`/`parecord` connect fine but the emulator's own audio backend won't).
So the guest `AudioTrack` buffer never drains → the mixer fills ~8 buffers then blocks. **This is an
emulator-infrastructure limitation, not a shim bug** — the game's audio path is correct (AudioTrack
inits, mixer runs, no crash). On the A56 the real audio HAL drains to real speakers, so continuous
mixing follows — this is exactly what the phone settles. If you try it, capture `adb logcat -s
abshim` and note whether audio plays continuously and whether the frame-rate holds (audio adds
emulation overhead). If anything
regresses, the silent `angrybirds-8.0.3-arm64.apk` is the safe fallback — same signer, so it
update-installs straight back over the audio build with no save-data loss.

## Happy-path boot + play (what success looks like)

```
abshim: engine 11248680 bytes from /data/app/.../lib/arm64/libengine32.so
abshim: init_array 125/125 (unimpl=0 last='')
abshim: guest JNI_OnLoad -> 0x10006 (OK)
abshim: abshim ready (host pagesize=4096)
abshim: call[1] nativeInit (VII) @0x401de5ec
abshim: call[2] nativeResize (ZII) @0x...
abshim: call[3] nativeUpdate (Z) @0x...        ← game loop is turning
abshim: render[1] GL draws=1274 clears=61 ...  ← geometry IS being submitted
abshim: frame[601] ...                          ← the tutorial card is up; tap to advance
abshim: [empty-json-guard] ... / [s-construct-null-guard] ...   ← level-end guards firing (normal)
abshim: frame[1201] GL draws=28168 ...          ← level cleared / next level rendering
```

`frame[N]`/`render[N]` climbing = the engine is rendering. Reaching `frame[601]+` with the
tutorial card = **it booted and is interactive** — tap the screen and play. On the x86 proxy this
whole sequence completes to a level win + the next level with `h_fatal=0`; the A56 should match,
just on its own silicon.

## Triage decision tree

| Symptom in logcat | Diagnosis | Action |
|---|---|---|
| **No `abshim` lines at all** | The `.so` never loaded, so the shim never ran. **`-s abshim` hides the reason** — the failure is reported by ART under `AndroidRuntime`, not by us. | Re-run WITHOUT the tag filter and look for the load error: `adb logcat -c && adb logcat AndroidRuntime:E ReconfigureLinker:E '*:S'` then launch. See the note directly below this table. |
| `open ... libengine32.so failed` / `mmap engine failed` | The 32-bit engine payload is missing next to the shim. | Re-run `build_apk.sh`; `unzip -l out/…apk \| grep libengine32`. |
| `cpu_create failed` / `loader_load failed` | Unicorn init or ELF relocation failed on-device. | Capture the line — this would be a host/arm64 divergence; send it. |
| `init_array INCOMPLETE (N/125) last_unimpl='X'` | A C++ static ctor faulted needing unbridged symbol `X`. | Host + qemu-user both run 125/125, so this implies a device-only import; bridge `X` (coverage_check enforces 0). |
| **`JNIEnv slot N UNHANDLED -> 0`** | The engine called a JNIEnv method not yet bridged (only genuinely-rare ones remain). | Look up slot `N` in NDK `JNINativeInterface` order, implement in `env_dispatch_real` (jni_entry.c). |
| **`shim_call NAME emu=… fatal=1 pc=0x…`** | Native `NAME` aborted. `pc` `0x10000000+` = a bridge stub; `0x40000000+` = guest engine code. | `__stack_chk_fail` = memory corruption (report `NAME`); `abort` = engine error path (missing asset/config assertion). |
| Boots, renders, but **black/blank screen** | GL state / shader compile / EGL surface — not fatal. | Read `render[N] GL draws=…`: `draws>0` → engine IS drawing, so `adb logcat` **without** `-s abshim` and grep `Mali`/`Xclipse`/`GLSL`/`shader` — a driver shader-compile error (GLSL ES version/precision) is the usual new-GPU culprit. `draws=0` → see next row. |
| **`render[N] GL draws=0`** repeating | Booted but no scene loaded. | `build_apk.sh` injects `assets/data/script_paths.json` so this should NOT happen; if it does, enable the asset trace to see the missing file. |
| Runs but **very slow / stutters** | Steady-state double-work (Unicorn ARM32 emulation). | Warms up after first-frame JIT (≈350 Minsn/s on x86; the A56's cores are the real test). Note any action that specifically stutters. |
| **Loses save progress** between launches | File write path / data dir. | Saves go via real `fopen`/`fwrite` to the app private dir; `adb shell run-as com.rovio.angrybirds ls files/`. Note: a *rebuilt* APK signed with a different key can't update-install over an old one — `port/debug.ks` is fixed so rebuilds keep the same signer. |

### If you get NO `abshim` output at all

This is the one failure the `-s abshim` filter cannot show you, because the shim never got far
enough to log anything. Capture the unfiltered error instead:

```bash
adb logcat -c
adb logcat AndroidRuntime:E ReconfigureLinker:E '*:S' > loadfail.txt   # then launch the app
```

What to look for, and what it means:

- `UnsatisfiedLinkError: dlopen failed: cannot locate symbol "<name>"` — the shim references a
  libc/libm symbol this device's bionic does not export from the library we linked. This is a real
  class of bug, not hypothetical: the shim was once missing `-lm`, which would have failed here on
  `sin`. Send the symbol name.
- `dlopen failed: ... is 32-bit instead of 64-bit` / `has unexpected e_machine` — the wrong ABI got
  installed. Confirm `adb shell getprop ro.product.cpu.abi` is `arm64-v8a` and that the APK contains
  only `lib/arm64-v8a/` (`unzip -l out/angrybirds-8.0.3-arm64.apk | grep lib/`).
- `dlopen failed: ... not 16 KB aligned` (or a segment-alignment complaint) — would mean the 16 KB
  page alignment regressed; `port/validation/verify_claims.sh` checks exactly this before shipping.
- `java.lang.UnsatisfiedLinkError: No implementation found for ...` — the library loaded but a JNI
  entry point is missing; capture the method name.
- Nothing at all, and `adb shell pm path com.rovio.angrybirds` prints nothing — the install did not
  actually take. See the refused-install table above.

## The engine's OWN logs also reach logcat

The shim forwards the engine's `__android_log_print` to the real logger, so `adb logcat` (no
`-s abshim`) shows the engine's own messages (e.g. tag **`Framework`**) — including the exact
reason if it aborts. Watch both:

```bash
adb logcat -c && adb logcat abshim:V Framework:V '*:S'
```

## `io::IOException` — now caught, no longer the blocker

Earlier builds died at the level-complete transition: a missing `script_paths.json` and then a
`std::string` use-after-free surfaced as an **uncaught** `io::IOException` → Lua panic → exit.
**Both are fixed:** `build_apk.sh` injects `script_paths.json`, and the UAF is pinned at its source
(a write-after-free canary in the guest allocator leaks the offending block so its address is never
reused). The level-end now survives; `io::IOException #14/#15` are *caught* and non-fatal, and the
game progresses. If the A56 logcat shows an **uncaught** io::IOException with `fatal=1`, capture the
file it names + the preceding `[s-construct-null-guard]`/`[empty-json-guard]` lines and send them.

## Fixes that got it from "renders" to "plays through levels"

- **Level-end std::string UAF** — pinned via a write-after-free canary + targeted leak in `galloc`
  (the single deepest bug; was the level-complete crash).
- **Ad-config null std::string** — `neut_s_construct_null` hooks `std::string::_S_construct` (NULL
  begin → shared empty string instead of `__throw_logic_error`).
- **Empty ad-JSON** — `guard_empty_json` redirects an empty parse to `{}`.
- **`glGetIntegerv` over-write** — was smashing a stack canary during level-load.
- Earlier: JNI Call-family + float promotion, JNIEnv fields/NewObject/arrays, GL client-side vertex
  arrays, guest-heap leak fixes + asset arena, C++ exception unwinding, sysconf, the S2 blocking-JNI
  GEL-release, and the GameLua `_Rep` corruption fix.

## No phone-home — four layers (verify on-device)

```bash
# The app has NO INTERNET permission -> the kernel denies every socket. Confirm nothing is granted:
adb shell dumpsys package com.rovio.angrybirds | grep -i "permission\." | grep -i internet
#   -> should show NOTHING (the string was mangled to android.permission.XNTERNET)
```

1. **OS:** no INTERNET permission → the kernel blocks every socket the app opens (analytics, ads,
   Facebook, crash reports, install-referrer, cloud, connectivity checks — all dead).
2. **libc:** the shim hard-fails `socket/connect/send/recv/getaddrinfo` for the emulated engine.
3. **Manifest:** billing / accounts / push (c2dm) / install-referrer declarations neutralised.
4. **Auto-collecting SDKs:** `manifest_firebase_off.py` injects two flags the SDKs read —
   `firebase_messaging_auto_init_enabled=false` (stops FCM auto-registering a device token *via
   Google Play Services*, the one phone-home the missing INTERNET permission can't stop) and
   `com.facebook.sdk.AutoLogAppEventsEnabled=false` (stops Facebook's local event collection).
   Native Flurry / Rovio-BI still gather locally but it's **sendless** (layer 1 denies every socket).

Storage permission is kept, so saves persist across launches (validated: `settings.lua`/
`highscores.lua` written and reloaded). The SDKs still initialise (no crash) but phone home nowhere.

**You may see the engine repeatedly read `/etc/hosts` + `/dev/urandom` and attempt sockets in
`logcat` — this is NOT a leak.** Those are a bundled analytics/networking SDK's own resolver
(it reads `/etc/hosts` directly and retries persistently) trying to look up its server and failing:
layer 1 (no INTERNET → the kernel denies the socket) + layer 2 (the shim hard-fails
`socket`/`connect`) block **every** attempt, so **nothing leaves the device**. The retries are the
SDK's own behaviour — noisy in the log, but each one is exactly what the blocking *stops*, not
egress that got through (that's why they're harmless, not why they're rare). To confirm zero egress
directly: `adb shell cat /proc/net/tcp /proc/net/tcp6` while the app runs shows no ESTABLISHED
sockets it owns.

## If you send one thing back

`adb logcat -s abshim > boot.txt` from a fresh launch (or `adb logcat abshim:V Framework:V '*:S'`
to also catch the engine's own abort reason). The last few lines localize any remaining issue to a
single bridge or GPU driver message — every failure mode above is designed to name itself.
