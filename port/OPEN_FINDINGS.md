# Open findings

Things that have been **measured** but not resolved. Kept separate from the docs that describe what
works, so an unresolved issue is never quietly folded into a success claim.

Each entry states what was observed, what was ruled out, and what would settle it.

---

## 1. The engine emits ~2046 unreadable `E Lua` log lines during the script-paths dump

**Status:** open. Not known to affect gameplay — the game boots, plays, scores, wins and progresses
with `h_fatal=0`, and the level scripts those paths refer to demonstrably load.

**Observed.** With *unfiltered* logcat (`emu_jni_exception_probe.sh` — every other validation script
captures only `-s abshim`, which is why this went unseen), the app logs:

```
E Lua : Script paths are:
E Lua : <8 bytes of binary>          x ~2046
```

The injected `assets/data/script_paths.json` holds 2035 entries, so the engine is dumping that list
one line per entry. Decoding a line: `0C B9 08 50 | 7C 2A 65 40` = `0x5008B90C` (inside `RG_HEAP`,
0x50000000) followed by `0x40652A7C` (inside `RG_ENGINE`, 0x40000000) — i.e. **two guest pointers
printed as if they were characters**, not text.

**Measured, and therefore ruled out.** A temporary ABI diagnostic in `h_log` (non-release only,
fires only when the formatted result is non-printable) captured the actual call shape:

```
r0=00000006 r1=51b8a9bc r2=40a19c38 r3=51b8a9ec fmt=40a19c38
fmtbytes=25 73 00 ...   tag='LocalNotifications'
```

- `fmtbytes = 25 73 00` is `"%s"` — the format string is read **correctly**.
- The tag pointer (`r1`) resolves to a correct string.
- The `%s` argument is `r3`, consistently `r1 + 0x30` — the same heap block as the tag.
- So argument marshalling is right: AAPCS puts the first vararg in `r3`, which is what
  `marshal_pull_word` returns after `prio`/`tag`/`fmt` (`ncrn` 0,1,2).

That eliminates the obvious suspects: it is **not** a wrong format string, **not** a misaligned
vararg cursor, and **not** a broken tag read. The pointer the engine hands to `%s` simply addresses
memory that does not contain text, while the tag 0x30 bytes earlier in the same block does.

**Still to determine.** Whether the engine passes the address of a `std::string` object (whose first
word is the data pointer) where the character data was intended, or whether the engine formatted the
message into that buffer through a bridged function of ours that produced non-text. The next step is
to dereference the first word at the `%s` address and see whether the real path string is one
indirection away — that distinguishes "engine passed the object" from "our formatting wrote garbage".

**Why it is not merely cosmetic.** Whatever the cause, engine diagnostics in this path are
unreadable, so if the engine ever reports something important here we cannot read it.

---

## 2. Not verifiable from this machine

Not defects — limits of the environment. Stated so they are never implied to be covered.

- **The physical Galaxy A56 run.** Everything is validated on emulators (API 25 and API 34, x86 with
  the shim in real ART). The A56's Exynos 1580 specifically is untested. See `ONDEVICE.md`.
- **Frame pacing under a real GPU.** All rendering evidence is SwiftShader software rendering.
- **Continuous audio on real audio hardware.** The audio variant plays through and wins on the
  proxy, but sustained playback needs real hardware.
