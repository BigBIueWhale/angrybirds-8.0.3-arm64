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
| `.7z` → `.zstream` sprite sheets | 26 | uncompressed, ASCII tag `RGBA4444` at offset 0x20 (the 40-byte header ends exactly where the tag does) |

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
`.zstream` sheet, since the zip-stored set is statically bounded at 2048 — and the largest sheet is
4 011 824 bytes at 2 bytes/px, so it holds at most 2 005 892 pixels and could exceed 2048 on one axis
only by being an extreme strip (4096 × 489 would fit that budget). Not excluded; not observed.

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

**Static screening was impossible.** There is no `.vsh`/`.fsh`/`.glsl` among the 3217 asset entries,
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

- Saves survive an **update-install** (same signer, same package) and are deleted by an
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

### R5. What the shipped APK can actually do — the full permission set, before and after

Measured from both manifests rather than described, because "no phone-home" is a claim about what is
*absent* and those are the claims that rot quietly.

| original 8.0.3 | shipped arm64 | effect |
|---|---|---|
| `android.permission.INTERNET` | `android.permission.XNTERNET` | unresolvable → **no socket may be opened by the process at all** |
| `android.permission.GET_ACCOUNTS` | `android.permission.XET_ACCOUNTS` | unresolvable → no account enumeration |
| `com.android.vending.BILLING` | `com.android.vending.XILLING` | unresolvable → no in-app purchase path |
| `android.permission.WAKE_LOCK` | `android.permission.WAKE_LOCK` | **kept** — normal protection level, auto-granted, keeps the screen awake while playing |
| `com.sec.android.airview.HOVER` | `com.sec.android.airview.HOVER` | **kept** — a Samsung hover-UI hint, not a capability |

So the installed app holds exactly one real capability, `WAKE_LOCK`, and **nothing at
`dangerous` protection level** — installing prompts for no runtime permission at all.

The mangling is length-preserving (first letter flipped) so the AXML string pool needs no offset
fixups; `depermission.py` explains why. `verify_claims.sh` asserts the absence of every live
network/billing/account/push permission, and `mutation_test.sh` proves that check fails when Rovio's
original manifest is restored.

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
