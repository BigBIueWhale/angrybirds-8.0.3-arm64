# abshim — Correctness Design (opinionated, defensively-correct, single-path)

Goal (user directive 2026-07-24): make the shim **truly, defensively correct**, ONE canonical
path / mode of operation, opinionated by construction, accounting for edge cases we haven't
thought of. Correctness is the acceptance criterion — NOT "it runs." Hardened via sequential
Opus-4.8 correctness audits; each audit's canonical spec is folded in here.

Root architectural principle established in Audit 01: **every value crossing the guest(ARM32
soft-float EABI, LP32, little-endian) ↔ host(AAPCS64) boundary goes through ONE arg-walker +
ONE return-writer, driven by a per-symbol signature descriptor.** No handler may read
`Rg(0..2)` by hand or write a bare `S0()`. This single rule structurally fixes most defects.

---

## Audit 01 — ABI marshalling boundary  (agent a8394b0eac96d5a1b)

> **Read "current" as "as of 2026-07-24".** These audits were performed against
> `port/shim/abshim.c`, the Stage-3 single-file skeleton, and that file is **superseded and no longer
> built** — `grep -c abshim.c port/build_apk.sh` is 0; the shipping shim is the 20-module `MODS` list
> under `port/shim/src/`. The defect list below is the *input* that produced the architecture stated
> at the top of this file (one arg-walker, one return-writer, per-symbol signature descriptors), not
> a list of things wrong with what ships.
>
> Concretely for **D1**, the Critical one: `src/marshal.c` implements the full AAPCS walk — r0–r3
> *and* stack spill, 8-byte alignment, NCRN clamping, "once on the stack, stay on the stack", and
> fundamental 8-byte types never split across r3/stack. The "reads only r0/r1/r2" defect is gone by
> construction.
>
> Each other finding should be re-checked against `src/` before being quoted as live; several are
> annotated as fixed further down, and the architectural rule above is what closes most of them.

### Confirmed defects in `abshim.c` as audited on 2026-07-24 (all silent wrong-value, none fault)
- **D1 (Critical)** `dispatch()` reads only r0/r1/r2 — cannot read r3 or stack args. Every ≥4-arg
  import (glTexImage2D/9, glVertexAttribPointer/6, glDrawElements/4, pthread_create/4, qsort/4…)
  is under-served. Masked today only because GL is a no-op catch-all.
- **D2 (Critical)** 64-bit returns write only r0; r1 left stale. Breaks `strtoll/strtoull` (i64),
  `strtod` + all double libm except ceil/floor (`sin cos tan sqrt pow atan2 exp log fmod ldexp
  frexp modf …`), `AAsset_getLength64`. Garbage NaN/Inf into physics/trig/timing.
- **D3 (High)** No single-precision float path. `sinf cosf tanf sqrtf ceilf floorf …` all → 0.
- **D4 (High)** `d_in/d_out` hardwired to r0:r1 — cannot express 2nd double operand (r2:r3) of
  pow/atan2/fmod, nor stack-spilled doubles, nor ldexp/frexp/modf companion args.
- **D5 (Critical)** `sprintf/snprintf/…` copy the format verbatim. Python reference marshaller has
  5 ABI bugs to NOT carry over — chiefly **no 8-byte alignment of 64-bit varargs** (a `%f` after
  a `%d` lands 8-aligned → on stack, r3 = padding), plus ignored snprintf size, v-variant va_list,
  length modifiers, %n.
- **D7 (High)** UNIMPL catch-all `S0(0)` silently zeroes pointer/64-bit/float returns → the
  silent-corruption amplifier. Pointer-returning imports return NULL → guest deref.
- **D8 (Critical, Stage 5)** `emu_call()` sets only r0–r3, returns only r0 — cannot pass a 5th arg,
  a 64-bit arg, a float/double, nor return 64-bit/float. Blocks the 72 JNI thunks.
- **D9 (High, latent)** `setjmp`→0 always, `longjmp`→no-op. Must snapshot/restore guest r4–r11/sp
  + resume PC. No-op longjmp makes error-recovery fall through.
- **D10 (High)** Host→guest callbacks (qsort/bsearch comparator, pthread_create routine,
  sigaction handler, atexit/key destructors) point at guest code — need trampolines re-entering
  emulation. Today qsort → unsorted; pthreads → never start.
- **D11 (Medium)** `memmove` routed through forward-only `em_copy` → overlap corruption.
- calloc `a0*a1` 32-bit overflow (E8); `mmap` must return `MAP_FAILED=(void*)-1` not 0 (E6).

### Canonical spec (THE single correct path)
Guest ABI = AAPCS32 base/soft-float, LP32 (long/size_t/off_t = 32-bit; long long/off64_t/double
= 64-bit/8-aligned), little-endian, **all FP in core regs** (never d/s regs).

**Two-counter arg machine** (`NCRN`=next core reg 0..3, `NSAA`=guest stack ptr at trap):
1. 8-byte types → round `NCRN` up to even first (0→0,1→2,3→4).
2. w = words (1 int/ptr/float, 2 i64/double). If w ≤ 4−NCRN → regs r[NCRN..]; NCRN+=w.
3. Else spill whole to stack: 8-align NSAA for 64-bit, place, NSAA+=w*4, NCRN=4. **8-byte scalars
   never split r3/stack** (alignment guarantees it). low word first (lower reg/addr).
- float = 32-bit IEEE pattern in ONE core slot, NOT promoted (named), NOT in a d-reg.
- Variadic tail: apply default promotions first (float→double, char/short/bool→int), THEN the
  same machine INCLUDING the 8-byte alignment step. Length modifiers pick width.

**Return writer:** void→nothing; i32/ptr→r0 (sub-word sign/zero-extended to 32 by signedness);
i64/off64/→ r0=low,r1=high (ALWAYS write both); float→r0 bits; double→r0:r1 bits; aggregate>4B→
hidden result ptr in r0 (unused here but spec'd).

**Pointer-direction policy (per signature):** `in`(copy len guest→host bounce), `out`(host buf →
copy back len to guest after call), `inout`, `str`(NUL-bounded), `opaque`(host handle table keyed
by guest token, as the AAsset code does). Length = const | other-arg | derived. Replaces all
ad-hoc em_str/em_copy.

**Two primitives, both directions:** `g_next_{u32,u64,f32,f64,ptr}` (read guest→typed) and a
write-walker (typed→guest r0–r3+8-aligned stack, returns final NSAA). `d_in/d_out` bit-logic is
CORRECT (LE r0=low) — keep it, just generalize to arbitrary aligned slot + single precision.

**Host call (arm64):** once typed, marshal into x0–x7 / v0–v7 / stack via **libffi** (one cif per
signature) OR a small typed-thunk table (only ~2 dozen distinct signature shapes across 351
imports). Read host result from x0 / v0 and convert float/double from host d0 back into guest
CORE reg(s) — the soft-float re-conversion lives here.

**`emu_call_v(guest_addr, GArg[], n, RetClass)`** replaces emu_call: write-walker places args
(8-aligned stack), SP 8-aligned, LR=RET, `addr` LSB selects ARM/Thumb (preserve symbol Thumb
bit), capture return by class. Guarantees guest r4–r11/sp preserved; r0–r3/r12/NZCV clobbered.

**72 JNI thunks = ONE generic thunk + a name→descriptor table.** Read incoming AAPCS64 (ints/
ptr/jlong from x0–x7, jfloat/jdouble from v0–v7 — two independent sequences), feed in Java order
into the guest write-walker (merges to single soft-float sequence, jlong/jdouble 8-aligned,
jfloat bitcast to core u32), emu_call_v the entry, convert guest return → AAPCS64 (jlong r0:r1→x0,
jfloat r0→v0, jdouble r0:r1→d0).

**JNIEnv/JavaVM passthrough:** guest vtable slots forward to the real env via read-walker+host
caller. Specials: variadic Call<Type>Method* (walk guest varargs by the method's JNI descriptor;
prefer the …A jvalue-array forms — jvalue is 8-byte 8-aligned); 64-bit returns (GetLongField,
CallLongMethod*) → r0:r1; float/double returns → soft-float; 64-bit args (SetLongField/
SetDoubleField) alignment.

**setjmp/longjmp, host→guest callback trampolines, SVC syscalls (r7=nr, args r0–r6, ret r0[:r1];
restore Python's clock_gettime/gettimeofday timespec-zeroing — else frame dt is garbage), and the
state-preservation contract (preserve r4–r11/sp, honor lr Thumb bit, write EXACTLY the return
regs of the class).**

Worked placements to validate any impl: glTexImage2D(9) → r0–r3 + SP+0..16; lseek64 → fd=r0,
off=r2:r3 (r1 skipped), whence=SP+0, ret r0:r1; pow → x=r0:r1,y=r2:r3,ret r0:r1; sprintf("%d %f")
→ %d=r2, %f 8-aligned → SP+0:7, r3=padding.

---

## Audit 02 — Concurrency / threading / reentrancy  (agent acf3b8aa74c278524)

Engine is genuinely multithreaded (imports pthread_create/join/detach, full mutex/cond/rwlock,
once, keys, self, cond_timedwait_monotonic; NO sem/__atomic/__sync/futex — atomics are inline
ldrex/strex + __kuser_cmpxchg). Android drives natives from GLThread (nativeRender/RenderThread) +
UI (init/pause/resume/input) + audio (nativeMixData) concurrently.

### Defects (all latent until thunks land, but hardwired now)
- **C1 (Crit)** single `uc` touched with ZERO synchronization → concurrent uc_emu_start/reg/mem/TCG
  races → arbitrary corruption. **C2 (Crit)** emu_call uses FIXED SP + saves NO context → two
  threads share one guest stack; reentrancy obliterates the suspended frame. **C3 (Crit)**
  __kuser_get_tls + __errno are ONE fixed address for ALL threads → cross-thread TLS/errno clobber.
  **C4 (Crit)** whole pthread_* no-op'd → threads never spawn, mutex no exclusion, cond returns
  immediately, once never runs init, getspecific always NULL. **C5 (High)** balloc non-atomic
  global bump → overlapping allocations. **C6 (High)** constant tid (1234) / pthread_self=0 → breaks
  owner checks, pthread_equal, tid maps. **C7 (Crit)** uc_emu_start not reentrant; no uc_context
  save/restore → native→Java→native clobbers.
- Hazards: lock-free guest SPSC/spinlocks (audio ring) → a cooperative "yield only at blocking
  bridges" scheme DEADLOCKS → forces PREEMPTIVE time-slicing; GL is host-thread-affine (EGL
  context); audio real-time vs global lock → priority inversion; clock_gettime out-param unfilled →
  garbage dt; guest audio time must track consumed-sample count not wall clock.

### Canonical design — Big Emulator Lock (BEL) + per-thread GEC (rejected: per-thread uc = unsound/pointless; dedicated emu-thread = breaks affinity)
- **`uc` touched ONLY under BEL** (one PI-mutex) — kills every C1 race.
- **GEC (Guest Execution Context) per guest thread**: `{ uc_context* full regfile; private stack
  region; private tls_base + errno_slot + tls-key map; unique guest_tid; state; depth }`. Each host
  thread that calls in lazily attaches a GEC (like bionic attaching a foreign thread); each guest
  `pthread_create` spawns a REAL host carrier thread that mounts a fresh GEC and runs the start
  routine via emu_call. **Render GEC carried by the GLThread** so guest GL hits the current EGL ctx.
- **Preemptive time-slicing**: `uc_emu_start(…, count=SLICE)`; on slice end uc_context_save →
  release BEL → reschedule. Bounded hold ⇒ progress even under lock-free guest spins. Bridges/hooks
  run within one guest-instruction handling ⇒ atomic vs scheduler ⇒ balloc/cmpxchg safe w/o extra
  lock (fixes C5).
- **Reentrancy (C7 fix)**: every emu_call does uc_context save on entry / restore on exit. Top-level
  entry sets SP=GEC.stack_top; NESTED entry (depth>0) keeps CURRENT guest SP (deeper frame). Nested
  clobber erased by outer restore; JNI return value written into restored outer ctx.
- **Blocking/callout discipline**: RELEASE BEL around any blocking guest wait AND any reentrant/
  blocking host callout (real JNIEnv->Call*Method runs arbitrary Java): uc_context_save → mark
  BLOCKED → release BEL → do it → reacquire → restore → set retval. (Kills block-while-holding-BEL
  deadlock.) Quick host calls (GL/libc) keep BEL.
- **Real guest sync** (BEL does NOT subsume them — time-slicing can preempt mid-section): shadow
  tables keyed by guest addr for mutex(owner+count, block-release-remount)/cond(atomic release+
  reacquire)/once(NOT_RUN→RUNNING→DONE runs guest init)/rwlock/join(block till ZOMBIE, return
  captured retval)/keys(+exit destructors). Atomics = guest instructions under BEL (single-owner ⇒
  correct; cleared exclusive monitor across slice = legal spurious strex fail, guest retries).
  pthread_self/gettid = GEC.guest_tid.
- **Affinity**: GL only on render GEC/GLThread; audio GEC gets priority + small SLICE; run each
  native on its REAL calling host thread so Java's GLSurfaceView pause/resume happens-before is
  inherited (never marshal natives onto a foreign thread). Thread-exit runs key destructors → ZOMBIE
  → wake joiners.
- **Invariants** (race-free): single-owner-uc; GEC isolation; bridge atomicity; real guest exclusion;
  serialized exec ⇒ sequential consistency (⊃ ARM weak model, safe-side). (Deadlock-free): bounded
  BEL hold; wait-without-BEL (no circular wait through BEL); reentrancy terminates.

---

## Audit 03 — Loader / dynamic-linker completeness  (agent a21e0e3413a5ce7ed)

Empirical (readelf-grounded): all 3 libs ELF32/DYN/ARM, REL (8-byte, no addend), SysV hash, no
GNU_HASH, no PT_TLS, no TEXTREL, PT_GNU_RELRO present, BIND_NOW. Engine NEEDED = 7 system libs
only (NOT js/adcolony). **libadcolony** NEEDED `libjs.so`+system; **libjs** NEEDED system only.
Load mechanism: engine = `System.loadLibrary("AngryBirdsClassic")` (→ loads OUR shim, same soname);
**adcolony = conditional runtime load with built-in "not found → Disabling AdColony" fallback**; js
= transitive dep of adcolony only. Reloc types across all 3 = exactly the 4 the loader handles
(NO TLS relocs, NO ifunc) → type-coverage adequate. Only INIT_ARRAY (no DT_INIT/PREINIT). Degenerate
versioning (LIBC node, no-op). **Engine UND ∩ js/adcolony DEFINED = ∅ → binding every engine UND to
a host stub mis-targets ZERO engine calls (engine is self-contained).**

### OPINIONATED DECISION: single-lib load. Do NOT load js/adcolony.
adcolony/js exist only for the AdColony **ad** SDK. The user directive is "remove all phone-homes."
So intentionally let adcolony fail to load → the engine's own fallback disables AdColony → no ad
phone-home AND the loader stays single-lib. dlopen("libadcolony.so")/System.loadLibrary("adcolony")
→ return failure by design. (The full recursive multi-lib linker below is SPEC'd for completeness/
defense but is intentionally not on the active path.)

### Confirmed loader defects (bite the engine even alone)
- **B1-1 (real bug)** the 5 STT_OBJECT UND data imports (`_ctype_`,`__sF`,`_tolower_tab_`,
  `_toupper_tab_`,`__stack_chk_guard`) are GLOB_DAT'd to the `bx lr` STUB page → guest reads
  instruction bytes as DATA → garbage (inlined ctype, `stderr=&__sF[2]`). MUST point these GOT slots
  at REAL guest data pages populated to mirror the host tables (ctype/`__sF`/canary).
- **B1-4** weak-UND must resolve to **0** *only when we deliberately do not provide it and 0 is the
  correct absent-behavior*. **CORRECTED by Audit 04/X8** (disasm-verified): the real weak-UND set is
  7 `pthread_*`+`getauxval`+2 `__google_*`+`__gnu_Unwind_Find_exidx` (the personalities
  `__aeabi_unwind_cpp_pr0/1/2` are DEFINED weak ndx 12, NOT UND). A blanket "→0" is a **bug**:
  `__cxa_guard_acquire` address-tests `&pthread_create` as its multithread sentinel, so `pthread_*`
  and `getauxval` MUST get real bridges; only `{__gnu_Unwind_Find_exidx, dl_unwind_find_exidx,
  __google_*}` → 0. See Audit 04 "Refined weak-UND rule."
- **B1-7** `load_engine` reads SYMTAB/STRTAB/REL/JMPREL as `g_elf+d_val` (vaddr AS file offset) —
  correct ONLY because first PT_LOAD has p_offset==p_vaddr==0. Read dynamic tables from the MAPPED
  image (`base+d_val`), robust to p_offset≠p_vaddr.
- **B1-2** dlopen/dlsym/dlclose/dlerror → host stub 0. Engine uses `dlopen(NULL)`+dlsym SELF-lookup
  → must implement: dlopen(NULL)=global-scope handle; dlsym resolves in engine+host scope; named
  guest-lib dlopen → fail by design (see decision). **B1-5** no RELRO/W^X (all UC_PROT_ALL). **B1-6**
  no FINI path. **B1-3** C++ EH imports stubbed → see Audit 04.

### Canonical spec (data-symbol fix is mandatory; multi-lib spec dormant)
- Map engine at BASE=0x40000000; read all DT_* tables from mapped image.
- Reloc: RELATIVE=B+A, GLOB_DAT/JUMP_SLOT=S, ABS32=S+A, **Thumb bit in st_value preserved**. Symbol
  class split: FUNC→code/trampoline addr; **OBJECT host datum → real guest data page mirroring host
  table** (fixes B1-1). Unknown reloc type → hard error, never silent skip.
- Symbol resolution rule (for any future guest lib): **guest-defined wins over host bridge; host
  bridge only for names no guest lib defines**; DF_SYMBOLIC → self first; weak-UND → resolve
  identically to strong-UND EXCEPT the small "→0" whitelist (Audit 04/X8 refined rule); SysV-hash
  lookup; degenerate versions are no-ops here.
- Per-segment perms from p_flags after relocation; RELRO span → RO; bss zeroed (uc_mem_map zero-fill
  ✓). Init: dependency order, DT_INIT then INIT_ARRAY ascending via emu_call; fini reverse on dlclose.
- Address arena for any future guest lib: **0x60000000–0x6FFFFFFF** (page-aligned bump), per-thread
  stacks from a separate sub-arena; each lib uses its OWN load_bias for every reloc/sym/init.
- soname→path remap `libFOO.so`→`libFOO32.so` in the shim's own dir (only needed if multi-lib ever
  activated; currently dormant per the no-ad decision).

---

## Audit 04 — C++ exceptions / unwinding / setjmp-longjmp / fatal paths  (agent a88eaa6f3c033c2b9)

Empirical (readelf + capstone disasm of the engine, all offsets file==vaddr in LOAD-1; GOT in
LOAD-2 so file=vaddr−0x1000). The engine STATICALLY links its **entire** ARM EHABI + libc++abi
runtime into `.text` §12 (`__cxa_*`, `_Unwind_*`, `__gnu_Unwind_*`, `_ZSt9terminatev`,
personalities `__aeabi_unwind_cpp_pr0/1/2` are **DEFINED weak, ndx 12** — they resolve in-guest,
NOT to 0). `.ARM.exidx` = **18269** 8-byte entries (0x23ae8/8; the earlier "18,301" is wrong),
covering all code; **6.3 % (1151) are `EXIDX_CANTUNWIND`**. PT_ARM_EXIDX @ vaddr 0x9bf3e4. The
**only** EH symbol that is UND is `__gnu_Unwind_Find_exidx` — WEAK, with **two** relocs:
`R_ARM_GLOB_DAT`@0xab98bc (address-test slot) **and** `R_ARM_JUMP_SLOT`@0xab9f1c (PLT@0x37b84).
`setjmp`/`longjmp` are UND@LIBC. **Decisive**: the engine's static `get_eit_entry`@0x89ca50 does
the textbook EHABI dance — `ldr r3,[GOT 0xab98bc]; cmp r3,#0; beq .fallback`; the **fallback**
@0x89ca90 loads `__exidx_end`(GOT 0xab98c0, `R_ARM_RELATIVE` addend 0x9e2ecc) and
`__exidx_start`(GOT 0xab98c4, `R_ARM_RELATIVE` addend 0x9bf3e4), computes `nrec=(end−start)>>3`,
and binary-searches (`search_EIT_table`@0x89c95c, bounded: nrec==0 or no-match ⇒ NULL). So after
`+BASE` the fallback yields the exact live exidx `[0x409bf3e4,0x409e2ecc)`, entirely in-guest.

### Confirmed defects (IDs Xn; map to the audit brief's Dn)
- **X1 = D1 (Critical, live)** `apply_rel` binds weak-UND `__gnu_Unwind_Find_exidx` to a **nonzero**
  stub (both slots). ⇒ `get_eit_entry`'s `cmp r3,#0` is false ⇒ it `bl 0x37b84` the PLT ⇒ stub
  `bx lr`, `dispatch()`(line 193) returns **r0=0 / never writes `*r1`(nrec)** ⇒ `get_eit_entry`
  reads NULL eitp ⇒ `mov r0,#9`(`_URC_FAILURE`). Failure is a *deterministic clean FAILURE*, not
  garbage-exidx corruption (the NULL check precedes the nrec read). Net: `__cxa_throw`@0x85a1d0 →
  `_Unwind_RaiseException`@0x89d90c returns fail → `bl 0x85a11c`=`std::terminate` → `abort`.
  **Every C++ throw ⇒ abort.** (The engine *does* throw: STL `bad_alloc/length_error/out_of_range`,
  `std::call_once`, and `__cxa_guard_acquire` throws `__gnu_cxx::recursive_init_error` on re-entrant
  init.) Fragile-clean: any refactor leaving r0=stale-pc instead of 0 flips this to real memory
  corruption (non-NULL bogus eitp → OOB table walk).
- **X2 = D2 (Critical, latent)** `setjmp`/`longjmp` UND@LIBC, unhandled ⇒ fall to line 213
  (S0(0)+`bx lr`). `setjmp` "returns 0" by accident; `longjmp(buf,val)` returns to **its own caller**
  instead of the `setjmp` site. Any guest error path that longjmps (libpng `error_fn`, zlib, custom
  parsers) resumes at the wrong PC with r0=0 ⇒ control-flow corruption / infinite retry.
- **X3 = D3 (Medium, landmine)** dispatch arms for `__cxa_begin_catch`/`__cxa_end_catch`(193),
  `__cxa_guard_acquire`(172, S0(1)), `__cxa_guard_release`(171) are **dead** — all four are
  guest-DEFINED (§12) so `apply_rel` resolves them to BASE+value, never the stub. Also **wrong if
  reached**: `__cxa_begin_catch`@0x8595c8 returns the *adjusted object pointer* (S0(0)⇒catch derefs
  NULL); guard_acquire→1 ⇒ every guard "wins" ⇒ initializers re-run. MUST delete these arms.
- **X4 = D4 (Critical)** `abort`/`__stack_chk_fail`(194) do `uc_emu_stop`+**S0(0)** ⇒ `uc_emu_start`
  returns `UC_ERR_OK` ⇒ `emu_call` returns r0=0 ⇒ JNI thunk reports "native returned 0". A guest
  `abort`/`terminate`/stack-smash is silently laundered into a bogus success; no fatal channel; guest
  CPU not reset ⇒ a partial-abort poisons the next `emu_call`.
- **X5 = D5 (High — resolved by X1 fix)** Uncaught exception at `emu_call` boundary (LR=RET=
  0xdead0000): unwinder sets PC=0xdead0000, `get_eit_entry(0xdeadfffe)`; exidx correct+bounded ⇒
  0xdeadfffe ∉ any range ⇒ NULL ⇒ `_URC_FAILURE` ⇒ phase-2 `bl abort` / phase-1 fail ⇒ terminate.
  Clean terminate/abort — **no OOB read, no mis-attribution** — provided X1 is fixed. 0xdead0000 is
  provably outside `[0x409bf3e4,0x409e2ecc)`.
- **X6 = D6 (Decision, OK)** `__cxa_atexit`/`__cxa_finalize` no-ops are **correct as an intentional
  lifecycle rule**: never `dlclose`, never clean-shutdown; process death is teardown (consistent with
  Audit 03 "no FINI path"). Static dtors legitimately never run; return 0 = "registered OK". Keep as
  a **documented** decision.
- **X7 = D7 (Low)** VFP unwind exists (`_Unwind_VRS_Pop`@0x89d4b8, `__gnu_Unwind_Save/Restore_VFP`
  use `vldm/vstm`). Requires CPACR+FPEXC (shim sets both before any emulation ✓). Smoke-test a throw
  across a frame that saves d8–d15.
- **X8 = NEW (Critical — refutes blanket weak-UND→0)** 11 weak-UND = 7 `pthread_*` (create,
  cond_signal, join, detach, key_delete, equal, mutex_trylock), 2 `__google_potentially_blocking_
  region_*`, `__gnu_Unwind_Find_exidx`, `getauxval`. **`__cxa_guard_acquire`@0x85a350 tests the
  address of weak `pthread_create` (GOT 0xab8eb0) as `__gthread_active_p`**: nonzero ⇒ MT path
  (ldrex/strex CAS + futex); **zero ⇒ single-thread non-atomic byte-flag path**. Applying Audit-03
  B1-4's blanket "→0" ⇒ `&pthread_create==0` ⇒ every function-local-static guard uses the ST path ⇒
  concurrent init (render/audio/UI threads) **double-initializes / uses-before-init**. Weak-UND is
  NOT a "→0" class.
- **X9 = NEW (Critical — couples Audit 02)** `__cxa_get_globals`@0x8597ec keeps per-thread exception
  state (uncaught count + caught-handler chain) in a **pthread TLS key** (`pthread_getspecific`@GOT
  0xab9d6c / `malloc` / `pthread_setspecific`@0xab9d64). If getspecific/setspecific no-op while the
  key exists, every call mallocs a **fresh** globals ⇒ in-flight exception state lost between
  `__cxa_throw` and `__cxa_begin_catch` ⇒ EH broken even after X1. MUST back with real **per-GEC**
  TLS (Audit 02 keys).
- **X10 = NEW (High — couples Audit 02)** `__cxa_guard_acquire`'s contended wait is a **raw
  `syscall(__NR_futex=0xf0,…)`** (`syscall`@GOT 0xab9e5c, PLT 0x37944), not pthread. `dispatch()` has
  **no `syscall` arm** ⇒ UNIMPL(213)→0 ⇒ the futex "wait" returns immediately ⇒ a thread waiting on
  an in-progress static init **busy-spins** (bounded only by Audit-02 preemptive slicing; a pure
  yield-at-bridge scheme would livelock). MUST route FUTEX_WAIT/WAKE on the guard word into real
  guest blocking. (Refines Audit 02 "no futex": data atomics are ldrex/strex, but the libc++abi
  guard blocks via a futex syscall.)

### Canonical spec (single path; rules are MUST)

**EXIDX / X1.** Resolve weak-UND `__gnu_Unwind_Find_exidx` (**both** GLOB_DAT and JUMP_SLOT slots)
to **0** and rely on the engine's PROVEN static `__exidx_start/__exidx_end` fallback. Chosen over a
real `Find_exidx` bridge because it adds **zero new failure surface**: the exidx bounds arrive
through the same `R_ARM_RELATIVE` path that already services ~17 k relocs (if broken, nothing runs),
and unwind stays in the guest's own verified `get_eit_entry`/`search_EIT_table` — no host round-trip,
no `nrec` out-param mis-marshalling. Safe because `get_eit_entry` address-tests before the PLT call
(`cmp r3,#0; beq fallback`), so a 0 JUMP_SLOT is never invoked. (A real bridge is needed ONLY if a
future engine build lacks the fallback — detectable as a `get_eit_entry` with no `beq .fallback`;
then bridge `Find_exidx(pc,*nrec)`: r0=PT_ARM_EXIDX base, `*(u32*)r1`=entry count from the phdrs.
Dormant.)

**Refined weak-UND rule (supersedes Audit 03 B1-4 blanket).** A weak-UND is an *optional* import the
engine address-tests (GLOB_DAT) and may call (JUMP_SLOT); weakness only makes **0 a legal
resolution**. Resolve it **identically to strong-UND** (guest-defined wins; else the real host
bridge); resolve to **0 ONLY for symbols we deliberately do not provide and whose absent-behavior is
correct** — exactly `{__gnu_Unwind_Find_exidx, dl_unwind_find_exidx}` (static exidx fallback) and the
two `__google_potentially_blocking_region_*` profiling no-ops. **All 7 weak `pthread_*` and
`getauxval` MUST resolve to their real bridges** (nonzero) — `pthread_create` especially, being
`__cxa_guard_acquire`'s multithread sentinel. `apply_rel` MUST branch on `STB_WEAK && SHN_UNDEF`
against this small whitelist, never a blanket 0.

**setjmp/longjmp (X2).** Special bridges, real guest-state transfer. bionic ARM32 `jmp_buf` =
`long[_JBLEN=64]` = **256 bytes** (guest-allocated); our snapshot uses our own self-consistent layout
fitting within it. `setjmp(buf)`: snapshot **r4–r11, r13(SP), r14(LR=resume PC), callee-saved
d8–d15** into `[r0]`, set r0=0, return via `bx lr`. Ignore the signal mask (shim delivers no async
signals to the guest). `longjmp(buf,val)`: restore r4–r11 and SP, set r0=`val?val:1`, **resume the
guest at the saved LR**, preserving its Thumb bit (`newpc=lr&~1`; `CPSR.T=lr&1`). **Unicorn resume
mechanism (MUST verify on target unicorn 2.1.x):** canonical, version-independent transfer is
*pending-request + stop/restart* — the longjmp hook stashes {regfile,newpc} and calls `uc_emu_stop`;
the `emu_call`/Audit-02 scheduler loop (already re-issuing `uc_emu_start` per slice) restarts
`uc_emu_start(uc,newpc,RET,0,0)`. Composes with time-slicing; avoids relying on mid-hook
`UC_ARM_REG_PC` writes. (The box86/box64 mid-hook PC-write trick MAY be used as an optimization *iff*
a standalone test confirms unicorn 2.1.x honors it: hook@A writes PC=B, assert next insn executes at
B not A. If in doubt, stop/restart.)

**Fatal channel (X4).** ONE sink for `abort`, `__stack_chk_fail`, `std::terminate`,
`__cxa_call_unexpected`, and any uncaught-at-boundary `_URC_FAILURE`: set `g_fatal` (+reason) and
`uc_emu_stop`. `emu_call` MUST check `g_fatal` on return and, instead of a bogus value, escalate to
the JNI thunk which calls **`(*env)->FatalError(env,msg)`** (process-level, clean, message-carrying)
— a C++ `terminate`/`abort`/smashed canary means engine state is unrecoverable; there is no "return 0
and keep playing." One mode; no soft-recover. Because we `FatalError`, no inter-call CPU reset is
needed; the general Audit-02 contract (per-GEC `uc_context` restore on `emu_call` exit) already
prevents a non-fatal abort from poisoning the next call. **Couples to Audit 03 B1-1**: until
`__stack_chk_guard` is a real, consistent guest datum, `__stack_chk_fail` can fire *spuriously* on
any stack-protected return — B1-1 is a **prerequisite** for the fatal channel not to misfire.
0xdead0000 needs no exidx entry (∉ covered range ⇒ deterministic NULL).

**Static-init threading (X8–X10).** `__cxa_guard_acquire` (guest-defined 0x85a329) runs in-guest
under Audit-02: its ldrex/strex CAS on the guard word executes as guest instructions under the BEL
(single-owner ⇒ correct); its `syscall(__NR_futex)` wait/wake, its `pthread_create` sentinel, and
`__cxa_get_globals`' `pthread_getspecific/setspecific` MUST be the **real per-GEC** bridges, never the
dead dispatch no-ops. Concurrent function-local-static init then correct (no double-init, no deadlock:
the futex wait blocks-and-releases-BEL per Audit 02, or busy-spins bounded by the slice).

**RTTI / dynamic_cast.** Exception type-matching (`__cxa_type_match`@0x8592d9, personalities) and
`dynamic_cast` rely on `type_info` **pointer identity within the single loaded image**; static-linker
vague-linkage/COMDAT merge guarantees one address per type. No cross-DSO `type_info` unification
needed. The bump allocator (Audit 05) MUST NOT relocate/duplicate `type_info` (they are static
`.rodata`/`.data.rel.ro` ✓). `__dynamic_cast`/`__cxa_bad_cast`/`__cxa_bad_typeid` all guest-defined.

**Not-thought-of.** (a) **Emergency exception pool**: `__cxa_allocate_exception`@0x859048 calls
`malloc(size+0x80)`; on NULL it uses a fixed slotted **emergency buffer** (512-byte slots;
`size>0x200`⇒`terminate`). Our `balloc` never returns 0 (it bumps past `HEAP_SZ` → the exception
write faults instead of engaging the pool). **Couples to Audit 05**: `balloc` MUST return 0 on arena
exhaustion so the guest's emergency pool engages, and/or the arena be sized so a throw never OOMs.
(b) **Throw during init_array / `__cxa_guard`**: a ctor that throws unwinds through the `run_ctors`
`emu_call` boundary → fatal channel (correct). (c) **`EXIDX_CANTUNWIND`** (1151): an exception into
such a frame ⇒ personality → `terminate` (normal; handled by the fatal channel — not a bug).
(d) **`_Unwind_ForcedUnwind`/pthread cancellation** (`__gnu_Unwind_ForcedUnwind`@0x89ce14 exists):
unused by normal play; if ever bridged it MUST honor the fatal channel at the boundary. (e) Exception
object lifetime (header at obj−0x80, refcount) is owned by guest `__cxa_throw`/`begin_catch`/
`end_catch`; the allocator MUST keep it stable for the exception's lifetime (never move/free under it).

---

## Audit 05 — Memory model: guest heap / address space / stacks / TLS / errno / mmap  (agent a9ebcaacafa98d613)

Empirical (readelf + capstone disasm; dynamic counts from an instrumented `poc_load.py` over
init_array + JNI_OnLoad + nativeInit). Image = 0xAF0000 (10.9 MiB), 2 PT_LOAD (R-X flags=5 + RW
flags=6), RW half at vaddr 0xaa2490.

### THE PIVOT — the game heap is **BIONIC's**, reached through our bridge.
`malloc`(715)/`free`(712)/`realloc`(823)/`calloc`(824)/`__errno`(267)/`mmap`(766)/`munmap`(765) are
all **UND @LIBC**. `memalign/posix_memalign/aligned_alloc/valloc/pvalloc/malloc_usable_size/mallinfo/
mallopt/reallocarray/mremap/brk/sbrk/mprotect/madvise` are **ABSENT** from `.dynsym`. C++
`operator new/new[]/delete/delete[]` (`_Znwj`@0x85a610, `_Znaj`@0x85a6a5, `_ZdlPv`@0x858ebc,
`_ZdaPv`@0x858ec5) are DEFINED §12 but are libc++ shims that **call the UND malloc/free** (disasm:
`_Znwj` `new(0)→malloc(1)`, `blx 0x374c4`=malloc PLT, on NULL→new_handler loop then
`__cxa_allocate_exception(4)`; `_ZdlPv`→veneer 0x965000→free PLT 0x374ac). ⇒ **our `balloc`/no-op
`free`/`realloc` bridge IS the game's live C++ heap** — every new/delete/STL/std::string flows
through it. Correct design = a **REAL allocator behind the malloc bridge**, not a bigger arena.
PLT anchors: malloc 0x374c4, free 0x374ac, realloc 0x37770, calloc 0x3777c, mmap 0x37608,
munmap 0x375fc, __errno 0x36fe4.

### Measured (boot only; LOWER BOUND — PoC's fake JNIEnv/no-GL/no-assets ⇒ shallow nativeInit)
init_array+JNI_OnLoad+nativeInit: **7860 malloc (358 KB), 425 realloc (grows: 16→32→64→128), 136
free (~10 KB), 453 KiB high-water. calloc/mmap/memalign/valloc = ZERO.** `free` and growing realloc
ARE exercised ⇒ engine assumes a reclaiming allocator with per-chunk sizes. 453 KiB at boot is
trivial; the danger is *gameplay* churn (per-frame new/delete + level load/unload) where no-op free
makes growth monotonic.

### Defects (M-series)
- **M1 (leak → OOM/fault) CONFIRMED Critical.** no-op free + bump balloc ⇒ every freed/deleted/
  realloc'd block leaks; `g_heap_ptr` passes arena end 0x58000000 → `UC_ERR_WRITE_UNMAPPED`. A
  long-session / many-level problem, not boot. Fix: real allocator with working free.
- **M2 (realloc over-copy) CONFIRMED High.** bridge copies `a1`(NEW size) out of the smaller OLD
  block ⇒ reads past it (measured 3 live cases at boot). No per-chunk size to copy min(old,new).
- **M3 (alignment) REFUTED as bug → promoted to invariant.** valloc/memalign/posix_memalign/
  aligned_alloc NOT imported ⇒ the buggy `valloc→balloc` + unhandled `memalign→0` are dead code.
  `balloc` already returns 16-aligned (satisfies double=8, NEON=16); SCTLR.A=0 under Unicorn so
  unaligned scalar loads don't fault. Rule: allocator MUST return ≥16-aligned.
- **M4 (single shared stack) CONFIRMED Critical.** one STACK (2 MiB) for all guest threads; sole
  `pthread_create`@0x88e790 passes **attr=NULL** and `pthread_attr_setstacksize` NOT imported ⇒
  engine relies on bionic defaults (main 8 MiB / worker 1 MiB) with deep Box2D/C++ recursion. Current
  2 MiB is both shared (concurrent GECs corrupt frames) AND undersized. Fix: per-GEC guard-framed
  stacks.
- **M5 (shared TLS) CONFIRMED but SCOPED — get_tls is near-dead.** engine **never reads a raw thread
  pointer** (0 valid `MRC p15…c13`, no `__aeabi_read_tp`, no `__emutls`, no PT_TLS). ALL per-thread
  state is via pthread keys (chiefly `__cxa_get_globals`, Audit 04 X9). Fix = per-GEC pthread-key
  storage, NOT a per-thread `__kuser_get_tls` (back the get_tls hook per-GEC defensively, but it is
  not the live path).
- **M6 (shared errno) CONFIRMED Critical.** `__errno` UND, read by engine; one fixed slot races
  across GECs. Fix: per-GEC errno slot.
- **M7 (arena collision) SPLIT.** errno/TLS slots carved from the TOP of the balloc arena = CONFIRMED
  wrong (structurally bad for N threads); region overlap (HEAP<lib<STACK) = REFUTED (non-overlap
  verified). Fix: move errno/TLS into per-GEC TCB pages.
- **M8 (mmap) CONFIRMED wrong; scope REFUTED.** mmap/munmap used ONLY by a file-mapping helper
  (@0x6db1d0): `fopen→ftell(len)→mmap(NULL,len,PROT_READ,MAP_SHARED,fd,0)→fclose`; dtors `munmap`.
  **File-backed read-only, never anonymous**; no mprotect/brk/mremap. UNIMPL→0 is wrong (0≠MAP_FAILED
  =-1, and returns no bytes). A gameplay/asset-or-savefile path (0 at boot). Fix: real file-backed
  guest mapping; failure→-1. **NOTE→Audit 06/07:** there is a direct `fopen`/`open`+`mmap` FS path
  distinct from AAssetManager — the current `open→3`/`read→0` stubs break it.
- **M9 (emergency pool, couples Audit 04) CONFIRMED High.** operator new + `__cxa_allocate_exception`
  depend on malloc returning NULL to engage new_handler / the 512-B-slot emergency buffer; balloc
  never returns 0 (it faults). `bad_alloc` payload 0x84 < 0x200 fits the pool ⇒ NULL-on-exhaustion is
  clean, no over-sizing. Fix: allocator returns 0 on exhaustion.
- **M10 (calloc overflow) CONFIRMED (couples Audit 01).** `calloc(a,b)=balloc(a*b)` 32-bit product
  overflows → under-alloc → heap overflow. Real calloc MUST check `b && tot/b!=a` → NULL.
- **M11 (delete→no-op-free) CONFIRMED.** every C++ delete tail-calls the no-op free — the dominant
  leak channel in a C++ engine (not direct free). `malloc_usable_size/mallinfo/mremap` NOT imported
  ⇒ allocator need not implement introspection or in-place remap.

### Canonical spec (single path; every rule MUST)
**Allocator (M1,M2,M9,M10,M11).** ONE real dlmalloc/TLSF-class allocator in the shim (host C) over
the fixed guest HEAP arena, with **inline boundary-tag chunk headers in guest memory** before each
payload (O(1) free/realloc, coalescing, queryable size). ≥16-aligned; `malloc(0)`→unique non-NULL
16-B chunk; `free` coalesces; `realloc` copies `min(old,n)` via the header + grows in place when the
next chunk is free (`realloc(NULL,n)`≡malloc, `realloc(p,0)`≡free); `calloc` overflow-checks then
zeroes; **returns 0 on exhaustion (never bump past the arena)**. Runs under the Audit-02 BEL ⇒ no
internal lock. valloc/memalign/posix_memalign/aligned_alloc kept as asserting dead stubs (never route
to balloc).

**Per-GEC control (M5,M6,M7).** Each GEC owns a **4 KiB TCB page** in a TCB arena: `+0x00 errno`;
`+0x40` 256-B TLS block (defensive backing for get_tls); `+0x100` 128-entry pthread-key table.
`__errno`→`GEC.tcb+0`; `__kuser_get_tls`(0xffff0fe0)→`GEC.tcb+0x40`; getspecific/setspecific index
the GEC key table. **errno & TLS MUST NOT live in the heap.**

**Per-GEC stacks (M4).** private stack per GEC in the STACK arena, framed by **unmapped guard pages**
(overflow ⇒ deterministic fault): **main = 8 MiB, worker = 2 MiB**. `emu_call` top-level sets
`SP=GEC.stack_top` (16-aligned); nested entry keeps current SP (Audit 02).

**mmap/munmap (M8).** bridge `mmap(NULL,len,PROT_READ,MAP_SHARED,fd,0)` → allocate `len` guest bytes,
fill from the fd's backing (same FILE*/AAsset table as open/read), return the guest pointer;
`munmap`→free. **Failure→`(void*)-1`, never 0.** No anonymous/mprotect/brk/mremap. Assets SHOULD stay
on AAssetManager; this path is for real FS files (save/cache).

**Canonical guest address-space map (non-overlap verified; heap grows UP w/ NULL at top, stacks DOWN
w/ guard pages; perms per PT_LOAD after reloc — .text R-X, RELRO R, data/heap/stack/TCB RW; guard
gaps unmapped):**

| region | base | size | perms | notes |
|---|---|---|---|---|
| STUB (import bx-lr stubs) | 0x10000000 | 0x20000 | R-X | 512 slots, `stub_hook` |
| JNISTUB (host-test only) | 0x11000000 | 0x10000 | R-X | shipping = real JNIEnv, not mapped |
| GUESTDATA (Audit 03 B1-1) | 0x12000000 | 0x10000 | RW | real `_ctype_`/`__sF`/`__stack_chk_guard` |
| ENGINE IMAGE | 0x40000000 | 0xAF0000 | per-seg | reserve to 0x47FFFFFF |
| TCB ARENA (per-GEC) | 0x48000000 | 0x01000000 | RW | 4 KiB/GEC: errno + tls + key table |
| HEAP (real allocator) | 0x50000000 | 0x08000000 | RW | allocator-managed; **NULL at 0x58000000** |
| — heap fault-guard | 0x58000000 | 0x08000000 | unmapped | overrun → fault |
| GUEST-LIB ARENA (dormant) | 0x60000000 | 0x08000000 | per-seg | single-lib ⇒ unused (lower half) |
| ASSET ARENA (Audit 07) | 0x68000000 | 0x08000000 | RW | `AAsset_getBuffer` guest copies; carved from the dormant guest-lib upper half |
| STACK ARENA (per-GEC) | 0x70000000 | 0x10000000 | RW+guards | main 8 MiB, workers 2 MiB, guard-framed |
| RET sentinel | 0xdead0000 | 0x1000 | R-X | emu_call return trap (∉ any exidx range) |
| KUSER helper page | 0xffff0000 | 0x1000 | R-X | cmpxchg/barrier(/get_tls); `kuser_hook` |

**Invariants (MUST).** (1) every malloc/calloc/realloc/new result is a live ≥16-aligned in-arena
chunk with a valid header; free/delete returns it. (2) heap never grows past 0x58000000; exhaustion ⇒
malloc 0 (⇒ bad_alloc/emergency pool, never a fault). (3) errno & TLS per-GEC, never in heap. (4)
each guest thread has its own guard-framed stack; no sharing. (5) all regions mutually non-overlapping;
stack/heap overruns hit an unmapped guard and FAULT deterministically. (6) `type_info` & static
`.rodata`/`.data.rel.ro` never relocated/duplicated by the allocator. (7) mmap returns a real
file-backed guest mapping or `(void*)-1`, never 0.

---

## Audit 06 — libc / runtime bridge exact-semantics  (agent a9ada42b — libc/runtime layer)

Empirical basis: `readelf --dyn-syms/-r -W` over the engine; capstone-5 disasm (host objdump has
no ARM target); `p_offset==p_vaddr` in LOAD-1 (code/rodata), `=vaddr−0x1000` in LOAD-2 (got/data),
matching Audit 03/04. ELF `Flags 0x5000200` = **Version5 EABI, soft-float ABI** (FP args/returns in
**core registers**) — the root premise of every descriptor below. `_FILE_OFFSET_BITS` NOT set:
`open/lseek/fopen/fstat/stat/ftell` are imported but **no** `*64` variants → off_t = 32-bit (LP32),
files <2 GiB. Formatter family is exactly 8 symbols (no `asprintf/dprintf/scanf/fscanf/vsscanf/
vfprintf`). RNG is the 48-bit family only (`lrand48/srand48`; no `rand/srand/random/arc4random/
getentropy`). No `fileno` (the guest reads the fd inline — see FILE model). No `usleep/sleep`
(only `nanosleep`), no `getpagesize/gettid/getuid/setenv/realpath/lstat/tzset/localeconv/__assert*`.

---

### THE PIVOT — the compiler soft-float / integer-helper runtime is **STATICALLY LINKED (in-guest); it does NOT cross the bridge.** Only `__aeabi_mem*` is imported.

Audit brief hypothesized the soft-float helpers might be UND@LIBC. **Refuted, decisively.** All of
`__aeabi_dadd/dsub/dmul/ddiv/drsub` (0x89c0a8/0x89de60/0x89e0cc…), `__aeabi_dcmpeq/lt/le/ge/gt` +
the `cdcmp*/cdrcmple` compare-and-set variants, `__aeabi_fadd/fsub/fcmp*`, `__aeabi_i2d/ui2d/l2d/
ul2d/f2d/i2f/ui2f/l2f/ul2f/d2lz/d2uiz/d2ulz/f2lz/f2ulz` (all confirmed DEFINED §12 in the readelf
dump), `__aeabi_idiv/uidiv/idivmod/uidivmod/
ldivmod/uldivmod/idiv0/ldiv0`, `__aeabi_llsl/llsr` are **DEFINED FUNC, ndx 12** (`.text`). `apply_rel`
binds them to `BASE+st_value`; they execute **inside Unicorn**. Disasm of `__aeabi_dmul`@0x89de60 is
pure-integer soft-float (`umull/umlal/lsr/orr/teq`), i.e. the engine is `-mfloat-abi=soft` for the
libgcc helpers — **and** VFP is used inline elsewhere (the file-mmap helper @0x6db2f0 does
`vldr/vcvt.f64.s32/vstr`). Both live **in-guest**; neither crosses the boundary → the shim's
CPACR+FPEXC enable (abshim.c 112-114) is **required** (contra a "soft ⇒ no VFP" reading) and Audit
01's "FP arithmetic uses VFP internally" is not wrong, merely incomplete: *both* VFP-inline and
statically-linked integer soft-float coexist in-guest.

**The ONLY `__aeabi_*` that is UND (→ we bridge, at high volume) is the block-memory set:**
`__aeabi_memcpy/memcpy4/memcpy8`, `__aeabi_memmove`, `__aeabi_memset/memset4/memset8`,
`__aeabi_memclr/memclr4/memclr8` (10 JUMP_SLOTs @ 0xab9f20-0xab9fe4). Consequence: **the only FP
that crosses the bridge is UND libm + printf/scanf `%f`**, always as core-register soft-float — the
`d_in/d_out` (r0=low,r1=high) shape is correct; it just needs generalizing to r2:r3 and single
precision per Audit 01 D3/D4. The full FP-helper table stays entirely off Audit 06's surface.

**`__aeabi_memset(dest, size_t n, int c)` — args are (dest, n, c), REVERSED vs `memset(dest,c,n)`
(ARM RTABI §4.3.4; bionic `__aeabi_memset(d,n,c){memset(d,c,n);}`).** abshim.c:162 and
poc_load.py:79 both treat a1=value,a2=size → **wrong byte, wrong length** (defect L1). The `4/8`
suffixes are alignment *hints* only (identical semantics). `__aeabi_memclr(dest,n)` (dest,n) — the
shim's line 163 handles memclr correctly; only memset is inverted. All `__aeabi_mem*` return **void**
(unlike `memcpy` which returns dest — writing r0=dest is harmless, not a bug).

---

### Enumeration — 351 unique UND → 124 owned by other audits, **227 = Audit 06 surface, 0 unclassified**

Subtracted (verified by partition): GL `gl*` (68), assets `AAsset*` (5), `__android_log_*` (2 →
GL/asset-adjacent), Audit 02 `pthread_*`(31)+`syscall`(1), Audit 05 `malloc/free/calloc/realloc/
mmap/munmap/__errno`(7), Audit 04 `__cxa_atexit/__cxa_finalize/setjmp/longjmp/abort/__stack_chk_fail/
__stack_chk_guard/__gnu_Unwind_Find_exidx/__google_potentially_blocking_region_*`(10). JNIEnv is the
env vtable, not `dispatch()`. Remainder = 227, categorized below.

**Descriptor key** (feeds Audit 01's single arg-walker/return-writer; no handler touches `Rg()`):
arg atoms `i`=int32 `u`=uint32 `p`=ptr `s`=cstr-ptr `z`=size_t(u32) `off`=off_t(i32) `L`=int64
(reg-pair, 8-align) `d`=double(reg-pair,8-align,bitcast) `f`=float32(1 core reg) `…`=variadic tail
`va`=va_list ptr; pointer dir suffix `‹in/out/io/op›`. ret atoms `-`void `i/u/p`→r0 `L/U`→r0:r1
`d`→r0:r1(bits) `f`→r0(bits). "cur" = current abshim.c status.

#### file-I/O + fd/FILE model (54) — see canonical FILE model below
| sym | desc | contract / cur | fix |
|---|---|---|---|
| `open`,`openat` | `(s,i[,u])→i` | O_* flags; ret fd or −1+errno. cur:→3 (line179) **broken** | fd table; route path; /dev/urandom,/proc/* providers |
| `close` | `(i)→i` | ret 0/−1. cur:→0 | close table entry |
| `read` | `(i,pout,z)→i` (=off) | ret bytes(**may be short**)/0EOF/−1. cur:→0 **broken** | real read; short-count honored |
| `write`,`writev` | `(i,pin,z)→i` | ret bytes/−1. cur:UNIMPL→0 | real write to sandbox/log |
| `lseek` | `(i,off,i)→off` | SEEK_SET/CUR/END; ret pos/−1. cur:→0 | real seek |
| `fcntl` | `(i,i,…)→i` | F_GETFL/SETFL/DUPFD; ret per-cmd. cur:UNIMPL | minimal (dup/flags) |
| `dup` | `(i)→i` | new fd. cur:UNIMPL | dup table entry |
| `pipe` | `(pout)→i` | self-pipe(fd[2]). cur:UNIMPL | host pipe or emulated |
| `poll` | `(p,u,i)→i` | fds ready; net-coupled. cur:UNIMPL | 0 (timeout) on dead-net fds |
| `ioctl` | `(i,u,…)→i` | rare; ret −1+ENOTTY unless known | −1/ENOTTY |
| `fsync`,`ftruncate` | `(i[,off])→i` | ret 0/−1. cur:UNIMPL | real on sandbox |
| `fopen`,`freopen`,`fdopen` | `(s,s[,i])→p‹FILE›` | ret guest `__sFILE*`/NULL+errno. cur:UNIMPL **broken** | **alloc bionic FILE, _file@0xe=fd** |
| `fclose` | `(p)→i` | flush+close; ret 0/EOF. cur:UNIMPL | close via _file@0xe |
| `fread`,`fwrite` | `(p,z,z,p)→z` | ret **item** count. cur:UNIMPL | via _file@0xe |
| `fseek`,`fseeko` | `(p,off,i)→i` | ret 0/−1. cur:UNIMPL | via table |
| `ftell`,`ftello` | `(p)→off` | ret pos/−1. cur:UNIMPL | via table |
| `fflush` | `(p)→i` | NULL⇒all. cur:UNIMPL | flush |
| `fgets` | `(pout,i,p)→p` | line, ret buf/NULL. cur:UNIMPL | |
| `fputs`,`puts` | `(s,p)/(s)→i` | ret ≥0/EOF. cur:UNIMPL | |
| `fputc`,`putc`,`getc`,`ungetc` | `(i,p)/(p)/(i,p)→i` | **functions, not inline** (so FILE buffer fields never read inline) | via table + 1-byte pushback |
| `feof`,`ferror` | `(p)→i` | **functions** ⇒ our FILE flags need not be inline-read | table EOF/err bit |
| `setvbuf` | `(p,p,i,z)→i` | ret 0. cur:UNIMPL | accept, ignore (host-buffered) |
| `perror` | `(s)→-` | "s: strerror(errno)\n"→stderr | via errno+strerror |
| `tmpfile`,`mkstemp` | `()→p / (pio)→i` | temp in sandbox. cur:UNIMPL | sandbox temp |
| `stat`,`fstat`,`statfs` | `(s/i,pout)→i` | fill guest `struct stat`/`statfs` (bionic layout — verify st_size/st_mtime/st_mode offsets vs target bionic). cur:fstat→zero buf (poc) | fill size/mode/mtime; statfs⇒plausible free space |
| `access` | `(s,i)→i` | 0 exists / −1+ENOENT. cur:UNIMPL(poc→−1) | route+test |
| `chmod`,`utime` | `(s,…)→i` | ret 0 on sandbox | best-effort/0 |
| `mkdir`,`rmdir`,`unlink`,`remove`,`rename` | `(s[,s/u])→i` | sandbox mutations. cur:UNIMPL | real on sandbox |
| `opendir`,`readdir_r`,`closedir` | `(s)→p /(p,pout,pout)→i /(p)→i` | dir enum (sandbox+assets). cur:UNIMPL | DIR table |
| `getcwd` | `(pout,z)→p` | ret buf. cur:→0(NULL)**risky** | write sandbox cwd, ret buf |
| `pathconf` | `(s,i)→off` | _PC_*; ret plausible (e.g. NAME_MAX 255) | canned |
| `uname` | `(pout)→i` | fill `utsname`(6×65B); ret 0 | canned "Linux/aarch64" facade |

#### stdio-format (8) — the varargs engine (canonical algorithm below)
`sprintf`(`p,s,…→i`), `snprintf`(`p,z,s,…→i`), `vsprintf`(`p,s,va→i`), `vsnprintf`(`p,z,s,va→i`),
`printf`(`s,…→i`), `fprintf`(`p,s,…→i`), `vprintf`(`s,va→i`), `sscanf`(`s,s,…→i`). cur: sprintf/
snprintf **copy the format verbatim** (line 201-202, ignores size, returns fmt-len), printf logs raw
fmt (203), the rest UNIMPL→0. **All wrong** (L4).

#### string / mem (47)
| sym | desc | contract / cur | fix |
|---|---|---|---|
| `memcpy` | `(p,p,z)→p` | dest. cur ok (line161) | keep |
| `memmove`,`__aeabi_memmove` | `(p,p,z)→p/-` | **overlap-correct**. cur:forward `em_copy` **corrupts** (L2=D11) | memmove-order copy |
| `__aeabi_memcpy/4/8` | `(p,p,z)→-` | =memcpy. cur ok | keep (ret void) |
| `memset` | `(p,i,z)→p` | cur ok(162) | keep |
| `__aeabi_memset/4/8` | `(p,z,i)→-` | **(dest,n,c) reversed**. cur **inverted**(162)(L1) | c=a2&0xff,n=a1 |
| `__aeabi_memclr/4/8` | `(p,z)→-` | zero n. cur ok(163) | keep |
| `memcmp` | `(p,p,z)→i` | unsigned-byte sign. cur ok(164) | keep (but see L11 buffers) |
| `memchr`,`memrchr`,`memmem` | `(p,i/p,z[,z])→p` | cur:memchr ok(190); rchr/mem UNIMPL | add |
| `strlen`,`strcmp`,`strncmp`,`strchr`,`strrchr`,`strstr`,`strcpy`,`stpcpy`,`strncpy`,`strcat`,`strncat`,`strdup`,`strcasecmp`,`strncasecmp` | std | cur: handled 165-189 but via **fixed 2048/8192 host bounce → truncates long data** (L11); strncpy pad ok(185); strcmp unsigned via host strcmp ok | operate on guest mem, length-bounded |
| `strspn`,`strcspn`,`strpbrk`,`strtok_r` | std | cur:UNIMPL | add |
| `strcoll`,`strxfrm` | C-locale | =strcmp/=strcpy | trivial |
| `strerror`,`strerror_r` | `(i[,p,z])→p/i` | static err table | provide |
| `basename` | `(s)→p` | last component (bionic: static buf) | provide |
| `fnmatch` | `(s,s,i)→i` | 0/FNM_NOMATCH | real fnmatch |
| `atoi`,`atol` | `(s)→i` | 32-bit. cur:UNIMPL(poc regex) | via strtol |
| `strtol`,`strtoul` | `(s,pout‹endptr›,i)→i/u` | base-detect, endptr, ERANGE→errno | provide |
| `strtoll`,`strtoull` | `(s,pout,i)→L/U` | **r0:r1** (L3=D2). cur:UNIMPL→0 | 64-bit return-writer |
| `strtod` | `(s,pout)→d` | **r0:r1** soft-float; ERANGE | provide |

#### ctype / locale (29) — tables in GUESTDATA (see below)
| sym | desc | contract / cur | fix |
|---|---|---|---|
| `_ctype_` **(OBJECT)** | GOT@0xab92b4 | inline `is*` do `_ctype_[c+1]&mask`; consumers @0x7bed5c,0x840dec | real bionic 257-B table |
| `_tolower_tab_`,`_toupper_tab_` **(OBJECT)** | GOT@0xab911c,0xab92ac | inline `_tolower/_toupper[c]`; consumers @0x3cec68,0x655f90 | real bionic short tables |
| `isdigit`,`isspace`,`isupper`,`isxdigit` | `(i)→i` | **also called as fns** (isdigit GLOB_DAT@0xab99bc, isspace JUMP_SLOT). cur:UNIMPL | table-consistent bridges |
| `tolower` | `(i)→i` | cur ASCII-only(192) | via _tolower_tab_ |
| `towlower`,`towupper`,`iswctype`,`wctype` | `(u[,u])→u` | wide. cur:wctype→1, iswctype UNIMPL | ASCII/Latin-1 + type mask |
| `btowc`,`wctob` | `(i)→u/i` | cur ok(181-182) | keep |
| `mbrtowc`,`wcrtomb` | `(p,s,z,p)→z` | **UTF-8** (font names are zh/ja) | UTF-8 codec |
| `wcslen`,`wcscoll`,`wcsxfrm` | wide str | C-locale | provide |
| `wmemchr/cmp/cpy/move/set` | `(p,…,z)→p/i` | wchar=4B | provide |
| `getwc`,`putwc`,`ungetwc` | `(p)/(u,p)→u` | wide stdio via FILE | via table |
| `setlocale` | `(i,s)→p` | cur:→0(NULL) **deref risk** (L12) | ret guest static "C" |

#### libm (35) — soft-float; args r0:r1[/r2:r3], ret r0:r1 (double) or r0 (float/long)
| sym | desc | note |
|---|---|---|
| `sin`,`cos`,`tan`,`asin`,`acos`,`atan`,`exp`,`log`,`log10`,`sqrt`,`ceil`,`floor`,`sinh`,`cosh`,`tanh`,`rint` | `(d)→d` | cur: only ceil/floor(174-175); rest UNIMPL→0 (L3) |
| `sinf`,`cosf`,`tanf`,`asinf`,`acosf`,`sqrtf`,`ceilf`,`floorf` | `(f)→f` | float in 1 core reg (D3) |
| `pow`,`atan2`,`fmod` | `(d,d)→d` | 2nd double **r2:r3** (D4) |
| `atan2f`,`fmodf` | `(f,f)→f` | |
| `ldexp` | `(d,i)→d` | int in r2 |
| `frexp` | `(d,pout‹int*›)→d` | writes `*exp` (guest ptr r2) |
| `modf` | `(d,pout‹double*›)→d` | writes int part (guest ptr r2) |
| `modff` | `(f,pout‹float*›)→f` | |
| `lrint`,`lrintf` | `(d)/(f)→i` | ret long(32) r0 |
| `difftime` | `(L?,L?)→d` | time_t=32-bit ⇒ `(i,i)→d` |

#### time / clock (12)
`gettimeofday`(`pout,p→i`), `clock_gettime`(`i,pout→i`), `clock`(`()→i`), `time`(`pout→i`),
`nanosleep`(`pin,pout→i` **blocking**), `gmtime`/`localtime`(`pin→p‹static tm›`), `gmtime_r`/
`localtime_r`(`pin,pout→p`), `mktime`(`pio→i`), `strftime`(`pout,z,s,pin→z`). cur: no time bridge;
`intr_hook`→1234 for nr20/224 only. **clock_gettime/gettimeofday leave the out-param timespec
unwritten ⇒ garbage frame dt / monotonic=0** (L9).

#### rng (2)
`srand48`(`i→-`), `lrand48`(`()→i`). cur:UNIMPL→0 ⇒ **lrand48 always 0, no gameplay randomness** (L10).

#### process / env / ids / signal (13)
`getpid`(`()→i`), `geteuid`(`()→i`), `getpwuid`(`u→p`), `getauxval`(`u→u`), `sysconf`(`i→i`),
`__system_property_get`(`s,pout→i`), `getenv`(`s→p`), `exit`/`_exit`(`i→-`), `raise`(`i→i`),
`sigaction`/`sigprocmask`(`i,pin,pout→i`), `sched_yield`(`()→i`). cur: sysconf/getpagesize→4096(176),
getauxval(16)→0x1000(177) **incoherent HWCAP** (L19), `__system_property_get` **leaves out-buf
uninit** (L20), exit/raise UNIMPL→0 **laundered** (L18).

#### sort / search (2)
`qsort`(`p,z,z,p‹cmp›→-`), `bsearch`(`p,p,z,z,p‹cmp›→p`). cur:UNIMPL→0 ⇒ **unsorted / not-found**
(L17=D10). Comparator is guest code ⇒ host→guest re-entry trampoline.

#### network (20) — **NEUTRALIZE (hard-fail facade, never a real socket)**
`socket connect bind send recv recvfrom getaddrinfo freeaddrinfo gai_strerror getpeername getsockname
getsockopt setsockopt inet_addr inet_ntop inet_pton if_indextoname if_nametoindex gethostname
getservbyport`. cur:UNIMPL→0 — **`socket→0` = "fd 0", a valid descriptor** (L16).

#### dl (4) — Audit 03 owns
`dlopen dlsym dlclose dlerror` — B1-2: `dlopen(NULL)`+self-`dlsym` must work; named guest-lib dlopen
fails by design. Listed for completeness; spec lives in Audit 03.

#### stdio data (1)
`__sF` **(OBJECT)** GOT@0xab9290 — array of 3 bionic `__sFILE`; `stderr=&__sF[2]`. See FILE model.

---

### CANONICAL FILE / FILE* MODEL — **forced to bionic `__sFILE`; the struct CANNOT be opaque.**

**Decisive evidence** (the file-mmap helper `@0x6db1d0`, the Audit-05 path): `fopen([r8],mode)` →
`fseek(f,0,SEEK_END)` → `ftell`→len → `fseek(f,0,SEEK_SET)` → if len≠0 **`ldrsh r3,[f,#0xe]`**
(read `FILE._file`, the fd, as a signed short) → `mmap(NULL,len,PROT_READ=1,MAP_SHARED=1,fd,0)`
(r0=0,r1=len,r2=1,r3=1,sp[0]=fd,sp[4]=0) → store ptr. On fopen-fail it `__cxa_allocate_exception(0xb8)`
and throws `strerror(errno)`. Because the guest reads `_file` **inline** at offset 0xe *and* indexes
`__sF[2]` with the concrete 84-byte stride, **no interception can keep FILE opaque** — the layout is
load-bearing. Offset 0xe = `_file` matches bionic's BSD `struct __sFILE` exactly (`_p`0x0 `_r`0x4
`_w`0x8 `_flags`0xc **`_file`0xe** `_bf`0x10 … total **0x54=84 B**).

**Design (single path):**
1. **Guest FILE = a real 84-B bionic `__sFILE`** allocated in guest memory by `fopen/fdopen/freopen/
   tmpfile`. We populate `_file`(0xe)=our guest fd and `_flags`(0xc)=bionic read/write bits; the
   buffer fields (`_p/_r/_w/_bf`) stay 0 — **safe because the inline-stdio helpers `__srget/__swbuf/
   __sfvwrite/__srefill` are absent from BOTH the UND imports AND the guest §12 defs**, i.e. the guest
   never uses bionic's inline `getc/putc` buffering macros (which would touch `_p/_r/_w`); every
   `getc/putc/fgets/fputc/feof/ferror` is the imported *function* form. So `_file`(0xe) is the **only**
   FILE field read in-guest. `__sF` = a fixed 3×84-B array in **GUESTDATA (0x12000000, Audit 03)**; its GLOB_DAT slot
   → `&__sF[0]`; entries 0/1/2 carry `_file`=0/1/2 (stdin/out/err).
2. **One shim-side descriptor table** keyed by the small guest fd stored at `_file`. Entry =
   {host FILE*/fd, resolved sandbox path, mode, role}. Every stdio call reads `_file@0xe` from its
   FILE* arg, looks up the entry, operates on the host handle. `open/read/write/lseek/close/dup/
   fcntl/fstat` use the same table with a raw fd. **`mmap` looks up the fd from the table** and reads
   `len` bytes into a fresh guest mapping (Audit 05 M8); `munmap` frees; failure→`(void*)-1`.
3. **Path router (ONE sandbox).** (a) writable app-data (saves/highscores/prefs/cache/"persisted
   events") → a real host **sandbox dir** (from the app files-dir handed to `nativeInit`, else a fixed
   shim dir); full read/write; atomic-save `tmp+rename` supported; `..`/absolute escapes → mapped-in
   or ENOENT (**writes never leave the sandbox — security**). (b) bundled read-only assets
   (`.lua/.json/fonts/levels`) belong to the **asset audit**; if a `fopen` resolves under the asset
   tree, serve read-only from the asset store. (c) **special files**: `/dev/urandom`→CSPRNG bytes
   (return the requested count, never 0); `/proc/cpuinfo|meminfo|self/auxv|socinfo`→**canned** content
   consistent with the emulated CPU (never the host's real, x86, `/proc` — leak + wrong arch).
4. **errno per-GEC** (Audit 05 M6 TCB slot): every failing call sets it; `__errno`→`&GEC.errno`.
5. **Short reads**: `read()`/`fread()` may return < requested; the guest's own loops handle it — the
   bridge must **not** pad. `off_t`=32-bit throughout (no LFS imported).

---

### CANONICAL VARARGS ENGINE (printf/scanf) — soft-float AAPCS, one formatter core, two arg sources.

**Format string is the first non-buffer fixed arg**: printf fmt=r0 (varargs from r1); fprintf
stream=r0,fmt=r1 (from r2); sprintf buf=r0,fmt=r1 (from r2); snprintf buf=r0,size=r1,fmt=r2 (from r3).

**Arg source A — non-`v` variants** (the bridge *is* the variadic fn): Audit 01's two-counter machine
starting at the first-vararg register, over trap-time r0-r3 + guest stack@SP. **Arg source B — `v`
variants** (`vprintf/vsprintf/vsnprintf`): walk the guest `va_list` pointer through guest memory. Both
obey the **same** promotion+alignment rules and feed the same core.

**Per-conversion pull (variadic tail, MUST 8-align 64-bit):**
- `%d/i/u/x/X/o/c/p` and `hh/h` mods → 1 word (int-promoted; `%c` is int).
- `%ld/lu/lx` (long=32 LP32), `%zd/zu` (size_t=32), `%td` (32) → 1 word.
- `%lld/llu/llx`, `%jd` (intmax=64) → **round NCRN up to even, pull 2 words** (or 8-align the
  va_list/stack cursor).
- `%f/F/e/E/g/G/a` **and** `%Lf` → **default promotion float→double ⇒ pull a DOUBLE = 2 words,
  8-aligned**, low word first, bitcast `(lo|hi<<32)`→f64. *This is the soft-float crux — the double
  comes from the CORE-reg/stack integer sequence, never a d-reg.*
- `%s`→ptr (1 word, NUL-bounded, precision-capped); `%n`→ptr.
- 8-byte scalars never split reg/stack (alignment guarantees it); once NCRN=4, everything from the
  8-aligned stack. `*` width/precision pulls one int before the value.

**Output/return:** sprintf⇒all+NUL, ret len; **snprintf⇒`min(len,size−1)`+NUL if size>0, ret the FULL
would-be len** (C99; the shim's "return fmt-len" is L4); printf/vprintf⇒stdout(fd1); fprintf⇒stream's
fd (stderr→log). Worked check (matches Audit 01): `sprintf(buf,"%d %f",…)`: %d=r2; %f 8-aligns
NCRN 3→4 ⇒ regs full ⇒ SP+0:7, r3=padding. ✔

**`%n`**: implement faithfully (write the running count to the guest int*), because the engine's
format strings are static/internal and correctness is the criterion — but it *is* an arbitrary-write
primitive; gate behind a compile flag and log, since no legitimate `%n` use is expected offline.

**`sscanf`**: parse guest `str` per fmt; each conversion consumes a **pointer** arg (1 word each) and
writes the value at the **length-mod width** (scanf `%f`⇒`float*`, `%lf`⇒`double*`(8 B), `%ld`⇒
`long*`(4), `%lld`⇒`long long*`(8) — unlike printf, the length modifier is mandatory for width).
Support width, `*` (suppress, no ptr consumed), `%[...]`, whitespace skip; ret # assignments (EOF if
input dry first).

---

### ctype / locale tables (GUESTDATA, consistent with the inline macros)

Populate three real guest tables + keep the function bridges byte-identical to them:
- **`_ctype_`** → a 257-byte array; inline `is*` compute `_ctype_[c+1] & MASK`, index range `c∈[−1,255]`
  ⇒ index `[0,256]` (index 0 = the EOF slot). Masks are the classic BSD/bionic bits
  `_U=0x01 _L=0x02 _N=0x04 _S=0x08 _P=0x10 _C=0x20 _X=0x40 _B=0x80`. `isdigit/isspace/isupper/
  isxdigit` (the imported fns) and `isalpha/isalnum/islower/…` (inline-only) MUST read the **same**
  bytes.
- **`_tolower_tab_`,`_toupper_tab_`** → `const short` arrays; inline `_tolower/_toupper[c]` with the
  bionic pointer offset so `c∈[EOF,255]` is valid (the "384/257-entry" backing per the target
  bionic — take the exact bytes from the KitKat-era bionic this engine linked, since the macros were
  frozen at its compile time; consistency is guaranteed only by mirroring bionic's exact table). `tolower`/`towlower`/`towupper` route through these.
- `setlocale`→ a guest static `"C\0"` (never NULL — L12); `localeconv` NOT imported (no `lconv`
  needed). MB/WC codec = **UTF-8** (font asset names are zh/ja). `__ctype_get_mb_cur_max` NOT imported.

---

### time / rng / misc (single opinionated forms)

- **time**: `clock_gettime(CLOCK_MONOTONIC=1)` = real host monotonic → the frame clock (fill the guest
  `timespec` `{tv_sec:i32, tv_nsec:i32}`); `CLOCK_REALTIME=0`=wall. `gettimeofday`→`{tv_sec,tv_usec}`.
  `time`→wall seconds (+ `*t`). `clock`→CPU ticks (CLOCKS_PER_SEC=1e6). **MUST fill the out-param**
  (L9). No `tzset` imported ⇒ `localtime≡gmtime` (UTC); save timestamps are UTC (acceptable offline).
  `nanosleep` **blocks ⇒ release the BEL and sleep** (Audit 02), not spin; `sched_yield`→scheduler
  yield. These are the two Audit-02 couplings on this surface.
- **rng**: exact 48-bit LCG `X←(0x5DEECE66D·X+0xB) mod 2⁴⁸`. `srand48(s)`: `X=(s<<16)|0x330E`.
  `lrand48()`: advance, ret `(X>>17)&0x7FFFFFFF` (r0). Unseeded default `X=0x1234ABCD330E`. **Separate**
  from `/dev/urandom` (a seeded host CSPRNG returning the exact byte count). Deterministic, reclaims
  gameplay randomness (L10).
- **process/env**: `getpid`→fixed (e.g. 12345); `geteuid`→app uid (e.g. 10123); `getpwuid`→minimal
  guest static `passwd` or NULL (note deref risk if it reads `pw_dir`); `getenv`→NULL except a
  controlled allowlist (`TMPDIR`→sandbox); `sysconf`: `_SC_PAGESIZE`4096, `_SC_NPROCESSORS_ONLN`→small
  (e.g. 4), `_SC_CLK_TCK`100; `__system_property_get`→**NUL-terminate the out-buf** and return 0 (or a
  canned allowlist e.g. `ro.build.version.sdk`="24") — never leave it uninit (L20); `exit/_exit/raise
  (SIGABRT)`→**fatal/shutdown channel** (Audit 04), not →0 (L18); `sigaction/sigprocmask`→record-but-
  never-fire success (the shim delivers no async signals — Audit 04).
- **getauxval / CPU-feature coherence (NEW):** the engine has a `CPU_Features` detector
  (`CPU_Feature_NEON/VFP/VFPv3/VFPv3d16/VFPv4`, + NEON AES from the bundled OpenSSL of the dead TLS
  stack). It gates fast paths on `getauxval(AT_HWCAP=16)` **and/or** the `/proc/cpuinfo` `Features:`
  line. `getauxval(16)→0x1000` (L19) advertises **HWCAP_NEON only**, incoherent with VFP. Fix: return
  **one coherent HWCAP** (VFP|VFPv3|NEON = 0x40|0x2000|0x1000) matching (a) what Unicorn actually
  emulates and (b) the synthetic `/proc/cpuinfo` — all three MUST agree or the two detectors diverge.
- **qsort/bsearch**: implement in host C over guest memory; invoke the guest comparator via Audit-01
  `emu_call_v` (two element **guest addresses** in, int out), nested under BEL with per-GEC context
  save/restore (Audit 02 C7). Elements stay in guest memory (as real qsort). Not stable — fine.
- **network**: hard-fail facade. `socket`→**−1**+EAFNOSUPPORT (never 0 = a real fd, never a real host
  socket — the host is on public IPv4); `connect`→−1+ENETUNREACH; `send/recv`→−1+ENOTCONN;
  `getaddrinfo`→EAI_FAIL; `inet_pton`→0; `gethostname`→"localhost"+0. The engine's offline fallbacks
  (and Audit 03's AdColony-disable) then engage. **No real sockets, ever** (L16 + security).

---

### Defects (L-series)

- **L1 (Crit, CONFIRMED)** `__aeabi_memset(dest,n,c)` arg order reversed. abshim.c:162 / poc:79 use
  a1=value,a2=size. *Scenario:* a `memset(p,0,64)` struct-init the compiler lowered to
  `__aeabi_memset(p,64,0)` fills `0` bytes of value `64` → struct left uninitialized → later reads
  garbage. *Fix:* for `__aeabi_memset*`, `c=a2&0xff, n=a1`; leave `memclr` (dest,n) as-is.
- **L2 (Med, CONFIRMED = D11)** `memmove`/`__aeabi_memmove` via forward-only `em_copy`. *Scenario:*
  `memmove(p+1,p,n)` (overlapping down-shift, common in string insert / ring buffers) smears the first
  byte. *Fix:* real overlap-correct copy.
- **L3 (Crit, CONFIRMED = D2)** 64-bit/double returns drop r1: `strtoll/strtoull/strtod` + all double
  libm except ceil/floor. *Scenario:* `strtod("9.81")` returns r0=low32,r1=stale ⇒ NaN gravity. *Fix:*
  the Audit-01 return-writer (always both regs).
- **L4 (Crit, CONFIRMED = D5)** No varargs formatter — sprintf/snprintf copy the format; snprintf
  ignores size + returns fmt-len; fprintf/vprintf/vsprintf/vsnprintf/sscanf →0. *Scenario:* any score/
  path/`"%s/%d"` string is emitted as the literal format → wrong filenames, wrong UI, buffer misuse.
  *Fix:* the canonical engine above.
- **L5 (Crit, CONFIRMED = A05 M8 handoff)** File I/O stubbed (open→3, read/close/lseek→0; fopen-family
  UNIMPL). *Scenario:* the mmap-helper's `fread`/`ftell` return 0 ⇒ len 0 ⇒ no data / the mmap path
  never runs; `/dev/urandom` read→0. *Fix:* the fd/FILE table.
- **L6 (Crit, CONFIRMED NEW)** `FILE._file@0xe` never populated — the guest reads the fd inline and
  hands it to `mmap`. *Scenario:* with no fopen bridge there is no FILE struct; the `ldrsh[f,#0xe]`
  reads unmapped/garbage ⇒ bogus fd ⇒ mmap fails or maps wrong. *Fix:* bionic `__sFILE` with
  `_file`=guest fd; mmap resolves fd via the table.
- **L7 (Crit, CONFIRMED = A03 B1-1 content)** `__sF` GLOB_DAT points at the bx-lr STUB page ⇒
  `stderr=&__sF[2]` is instruction bytes ⇒ any `fprintf(stderr)`/`perror` corrupts. *Fix:* 3×84-B
  `__sFILE` array in GUESTDATA.
- **L8 (High, CONFIRMED = A03 B1-1 content)** `_ctype_/_tolower_tab_/_toupper_tab_` GLOB_DAT→STUB ⇒
  inline `is*/to*` read `bx lr` bytes as classification ⇒ wrong tokenizing of every parsed
  `.lua/.json`. *Fix:* real bionic tables (above).
- **L9 (Crit, CONFIRMED = A02 hazard)** `clock_gettime/gettimeofday` out-param unfilled + no monotonic
  bridge. *Scenario:* frame `dt = now−prev` with both 0 ⇒ 0 (physics frozen) or huge ⇒ explosion;
  monotonic=0 across frames ⇒ div-by-zero in fps. *Fix:* real monotonic filling the timespec.
- **L10 (High, CONFIRMED)** `lrand48/srand48`→0 and `/dev/urandom`→0 bytes ⇒ no randomness/entropy.
  *Fix:* exact LCG + urandom provider.
- **L11 (High, CONFIRMED)** str/mem bridges bounce through fixed 2048/8192 host buffers ⇒ silent
  truncation / wrong compare for long guest data (level JSON paths, base64). *Fix:* operate directly
  on guest memory, bounded by the actual length arg.
- **L12 (Med, CONFIRMED)** `setlocale`→NULL; if the engine `strcmp`s the result vs "C" it derefs NULL.
  *Fix:* guest static "C".
- **L16 (Crit, CONFIRMED — security)** network UNIMPL→0: `socket→0` is a *valid fd* ⇒ the engine
  believes it has a socket; and any future real wiring phones home from a public-IPv4 host. *Fix:*
  hard-fail facade, no real sockets.
- **L17 (High, CONFIRMED = D10)** `qsort/bsearch`→0 ⇒ unsorted / never-found (leaderboards, draw-order,
  spatial sort). *Fix:* host sort + guest-comparator trampoline.
- **L18 (Med, CONFIRMED, couples A04)** `exit/_exit/raise`→0 ⇒ the engine's request to terminate is
  silently ignored and it runs on in an undefined post-exit state. *Fix:* fatal/shutdown channel.
- **L19 (Med, CONFIRMED NEW)** `getauxval(AT_HWCAP)→0x1000` = NEON-only, incoherent with VFP and with
  `/proc/cpuinfo`. *Scenario:* the detector enables NEON kernels but takes the "no-VFP" scalar path
  elsewhere → mismatched math / init assert. *Fix:* one coherent HWCAP across getauxval ⟺ /proc ⟺
  Unicorn.
- **L20 (Med, CONFIRMED NEW)** `__system_property_get`→0 without writing the out-buffer ⇒ the engine
  reads uninitialized stack/heap as a property value. *Fix:* always NUL-terminate the buffer, ret 0.
- **L13/L14/L15 (REFUTED as bugs)** toupper/tolower ASCII path (191-192) is correct for C-locale
  (route through tables only for consistency/wide); `memcmp` sign (164) is unsigned-correct; the
  `__aeabi_mem*` "missing return" is a non-issue (they return void).

---

### Invariants (MUST)
1. FP crosses the boundary **only** for UND libm + printf/scanf `%f`, always core-register soft-float
   (r0:r1 double, r0 float); all `__aeabi_*` arithmetic/convert/divide stays in-guest.
2. `__aeabi_memset` is (dest, **n, c**); `__aeabi_memclr` is (dest, n); `memmove`/`__aeabi_memmove`
   are overlap-correct.
3. Every FILE* is a real bionic-`__sFILE`-layout struct with a valid fd at `_file` (0xe); `__sF[0..2]`
   real in GUESTDATA; all stdio routes via `_file`→fd table; `mmap(...,fd,0)` resolves that fd.
4. All file writes confined to the shim sandbox; assets read-only; `/dev/urandom` & `/proc/*` synthetic
   & CPU-coherent; **no real network socket is ever created**.
5. Every 64-bit/double/float return uses the Audit-01 return-writer (both regs / bit-exact); every
   varargs pull applies float→double promotion and 8-byte alignment for 64-bit.
6. `_ctype_`/`_tolower_tab_`/`_toupper_tab_` bytes and the `is*/to*` function bridges are mutually
   identical and mirror the target bionic; errno per-GEC; blocking calls (`nanosleep`, futex-less waits)
   release the BEL (Audit 02); `exit/abort/raise`→fatal channel (Audit 04).

---

## Audit 07 — GL / EGL / AAsset graphics + asset bridges  (agent a9ada42b — graphics/asset layer)

Empirical basis: `readelf --dyn-syms/-lW/-SW` over the engine + capstone-5 disasm (host `objdump`
has no ARM target; PLT→symbol map built from `.rel.plt` ordering and **cross-verified 0/344
mismatch** against the decoded GOT slot of each PLT stub, and against Audit 05/06's cited
`malloc 0x374c4 / free 0x374ac / __errno 0x36fe4` — all match). File offset == vaddr in LOAD-1
(`.plt 0x36dfc`, `.text 0x37e40`, `.rel.plt 0x3633c`, `.rodata`), = vaddr−0x1000 in LOAD-2
(GOT/data), consistent with Audits 03–06. The engine is **mixed ARM/Thumb**: the JNI render thunks
and the **entire graphics module `[0x5af000,0x5cf000)`** are ARM (verified by clean linear disasm of
the draw functions); the sole `pthread_create` caller `@0x88e790` is Thumb. 210 real ARM
`gl*/AAsset*/__android_log_print` call sites, all clustered in the graphics module + 3 log sites on
the init path — GL is a **self-contained renderer module**, not sprinkled through the engine.

---

### THE PIVOT (double) — (1) **zero EGL ⇒ GL is single-threaded on the Java render thread, PROVABLY.** (2) **VBO-only ⇒ draws are pure scalar passthrough, no per-draw client copy.**

**No EGL, no surface, no context — verified exhaustively.** `readelf` UND set contains **zero**
`egl*`, **zero** `eglGetProcAddress`, **zero** `ANativeWindow_*`, **zero** `*Surface*`,
`*MakeCurrent*`, `*CreateContext*`. ⇒ The engine neither creates, shares, nor makes-current any EGL
context, and owns no window/surface. Java (a `GLSurfaceView`/Fusion render thread) owns EGL; the
engine only issues GL on whatever context is already current on the calling thread. **There is no
EGL bridge to write.** The design question reduces entirely to *which host thread runs the guest GL*.

That question is then settled **by construction, not by tracing**: GLES2 permits GL only on the one
thread the context is current on; a second GL-issuing thread would require `eglCreateContext`
(share) + `eglMakeCurrent` on that thread — **both absent**. A commercial title (AB 8.0.3, tens of
millions of installs) does not ship a "GL from a thread with no current context" bug. Therefore
**every one of the 68 `gl*` is issued from the single thread Java made current** — the render/GL
thread. The engine's ONE guest thread-spawn (`pthread_create@0x88e790`, `attr=NULL`, start routine
`@0x88e679` = a generic C++ `blx vtable[2]` thread trampoline whose body is virtual/polymorphic and
so *statically* unknowable) provably does **not** issue GL, because it has no way to make the context
current. Worker threads do CPU work (image decode, streaming) and hand results to the render thread,
which does the actual `glTexImage2D`. This is a correctness-by-construction lock: **no disasm of the
polymorphic thread body is needed — the absence of EGL is the proof.**

Premise re-verified rather than trusted: `libAngryBirdsClassic.so` has **0** EGL imports and **68**
`gl*` imports; `libjs.so` has 0 of each. "Both absent" and "the 68" are exactly right.

**But that argument is about the ENGINE, and the shim interposes green threads on carrier pthreads.**
`ctx_switch_in` restores any runnable gthread onto whichever carrier holds the GEL, so guest
execution is not inherently pinned to the host thread a gthread was created on — which is the one way
this by-construction lock could be broken from below. If a render gthread ever yielded mid-frame and
resumed on another carrier, its `gl*` calls would land on a host thread with no current context: a
black screen on a strict driver, and plausibly tolerated by SwiftShader, i.e. exactly the shape of
bug that would survive every test here and appear first on the A56.

Measured, because reasoning about a scheduler is how one over-claims. logcat records the host tid,
and across two independent runs on two different builds (`gpucap` = release+dumps, `playthrough` =
debug), the shim is active on **four** host threads — yet **every** `frame[…]` line and **every**
GL-bridge dump (`[shader-src]`, `[gl-str]`, `[tex-dim]`) is emitted from exactly **one** of them:

| run | shim host tids | tids issuing GL |
|---|---|---|
| `gpucap` | 2237, 2275, 2288, 2391 | **2288 only** |
| `playthrough` | 2661, 2684, 2767, 2842 | **2767 only** |

So the scheduler does not migrate GL work off the render carrier in practice. This is a measurement
over two runs rather than a proof — but unlike the GPU facts around it, the mechanism is
**host-independent**: the same scheduler, with the same yield points, runs on the A56.

**Render entry model — disasm-decisive, and not what the brief's option (a)/(b) assumed.** The
render JNI thunks are near-empty:
- `nativeRender@0x1da8f8` = **`mov r0,#1; bx lr`** — a **pure no-op returning JNI_TRUE**. It does NOT
  render a frame and takes no GL path.
- `nativeRenderThread@0x1da900` = `ldr r3,[pc,#8]; add r3,pc,r3; ldrb r0,[r3,#0x5c]; bx lr` — **returns
  a boolean flag byte** from a global. A *query* ("does the engine want a dedicated render thread"),
  NOT a blocking render loop.
- `nativeFrameClear@0x1da8b0` = **`bx lr`**; `nativeInterruptRender@0x1da8b4` = **`bx lr`** — both
  **pure no-ops**.
- `nativeResize@0x1da8b8`: loads a global renderer object `*(g+0x7c)`, and if non-NULL calls
  `vtable[0xdc](obj, width=r2, height=r3)` — resize handled inside the engine (glViewport is issued
  deeper, 2 sites in the graphics module), returns 1.
- `nativeInit@0x1de5ec` = `mov r0,r2; mov r1,r3; b 0x1de254` (tail-branch, drops env/thiz);
  `nativePause@0x1df5ac`/`nativeResume@0x1df5b4` = `mov r0,#0/#1; b 0x1dee08` (shared handler, bool).

⇒ Per-frame rendering is driven by **`nativeUpdate@0x1dd36c`** (the only substantial per-frame entry,
1580 B, ARM, real prologue `push{r4-fp,lr}; vpush{d8}`), which runs simulation **and** render
(0 *direct* GL sites in its body — it reaches the graphics module through the engine's C++ virtual
dispatch, `ldr r3,[obj]; ldr r3,[r3,#N]; blx r3`). `nativeRender` is a **vestigial no-op** in this
build; **Java calls `eglSwapBuffers` itself** after `nativeUpdate` returns (it must — the engine
cannot). **Canonical model: RENDERMODE-continuous — Java's GLThread calls `nativeUpdate` once/frame
(context current), the engine simulates+draws synchronously, Java swaps.**

**VBO-only rendering — disasm-decisive (determination #2).** Every `glBindBuffer` (11 ARM sites) uses
target **`0x8892`=GL_ARRAY_BUFFER** or **`0x8893`=GL_ELEMENT_ARRAY_BUFFER**, gated by a
redundant-bind state cache at `[g+0x314]`(array) / `[g+0x318]`(element). All 3 `glBufferData` use
**usage `0x88e4`=GL_STATIC_DRAW**. The `glBindBuffer(target,0)` (unbind) calls occur **only** in the
upload path immediately after `glBufferData`, never at draw time. At every draw a nonzero VBO is bound
first, and:
- `glDrawElements@0x5cb220`: `r2=0x1403`=GL_UNSIGNED_SHORT, and **`indices = 0 + start*2`** (r3 set to
  `#0`@0x5cb1f8, then `add r3,r3,r1,lsl#1`) — a **byte offset into the bound element buffer**, i.e. a
  small integer, NOT a client pointer.
- `glVertexAttribPointer@0x5cb198` / `@0x5cd0bc`: the 6th arg (`pointer`) is a stack-slot value = a
  **VBO byte offset** into the bound array buffer.
- `glDrawArrays@0x5cb358`,`@0x5cd1dc`: `(mode, first, count)`, count=`end-first`.

⇒ **The vertex/index "pointers" are `GLintptr` offsets into GPU buffer objects. Draws are pure scalar
passthrough — NO per-draw guest→host vertex/index copy.** The only vertex-data copy is at
`glBufferData` (a normal IN-pointer copy of `size` bytes, once, GL_STATIC_DRAW). `glMapBuffer*`,
`glBufferSubData`, `glVertexAttrib{1..4}f*` are **all absent** ⇒ no buffer mapping/streaming/constant-
attrib machinery. **Client-side vertex arrays are NEVER used** (spec a defensive guard that asserts,
below — it must never fire).

---

### Determinations (evidence-anchored)

1. **EGL/threading.** No EGL/surface/context imports ⇒ no EGL bridge; Java owns EGL and calls
   `eglSwapBuffers`. GL is single-threaded on the Java GLThread that calls `nativeUpdate`/`nativeInit`/
   `nativeResize` (the context-current thread). `nativeRender`/`FrameClear`/`InterruptRender` are
   no-ops; `nativeUpdate` is the renderer. The one guest `pthread_create` does not issue GL (no EGL to
   make-current a worker). **Canonical rule G-INV1 below.**
2. **Client-vs-VBO.** VBO-only (GL_ARRAY_BUFFER + GL_ELEMENT_ARRAY_BUFFER, GL_STATIC_DRAW, redundant-
   bind cache, draws with buffer offsets). Draws = passthrough; only `glBufferData` copies. No client
   arrays. Defensive client-array guard spec'd but inert.
3. **Marshalling table** — all 68, below.
4. **AAsset path** — real libandroid, host-ptr→guest-copy, 64-bit length, below.
5. **Not-thought-of / residuals** — below.

---

### Defects (G-series). Current abshim.c GL/AAsset handling is a **host-test façade**; for shipping it is wholesale wrong.

The GL catch-all (abshim.c **195–200**) fabricates IDs and returns 0 for everything else — it **never
calls a real GL driver**. The AAsset handlers (**205–211**) are inside `#ifdef SHIM_TEST`, so the
**shipping** build has *none* of them and every AAsset call hits the UNIMPL fallthrough (213)→0.

- **G1 (Critical, CONFIRMED) — no real GL forwarding.** Catch-all (195–200) forwards nothing; it
  invents `++gid` handles and returns 0. Shipping draws **nothing** and mistracks every object.
  *Fix:* resolve the 68 `gl*` once via `dlsym(RTLD_DEFAULT,...)` against the process's already-loaded
  real arm64 `libGLESv2`, and forward each through the Audit-01 descriptor marshaller (table below).
- **G2 (Critical, CONFIRMED) — `glGetString`→0 (NULL).** Not special-cased ⇒ `S0(0)`. The engine
  queries `GL_VENDOR(0x1f00)`/RENDERER/VERSION/EXTENSIONS (7 sites, e.g. `@0x5afd9c`) and derefs the
  result for GPU/driver detection (`strcmp/strstr`). NULL ⇒ deref crash / wrong caps path.
  *Fix:* call real `glGetString`, **copy the host C-string into a cached guest buffer, return the
  guest pointer** (one cached copy per `name`; strings are immutable).
- **G3 (Critical, CONFIRMED) — fabricated IDs collide with the real driver namespace.** Catch-all
  returns `++gid` for `glCreateProgram/Shader`, `glGetUniform/AttribLocation`, and writes `++gid` for
  `glGen*`. Once GL is real (G1) the driver assigns its **own** names; any mix ⇒ every subsequent
  bind/draw/uniform targets a wrong/nonexistent object. *Fix:* **IDs come only from the real driver** —
  return the real `glCreate*` result; write back the real `glGen*` output. Never fabricate.
- **G4 (Critical, CONFIRMED) — AAsset\* unimplemented in shipping.** Handlers are SHIM_TEST-only ⇒
  `AAssetManager_open/getBuffer/getLength64/close/fromJava` all →0 in shipping. **No bundled asset ever
  loads** (levels, atlases, shaders, fonts, audio) ⇒ total failure. *Fix:* the real libandroid path
  below.
- **G5 (Critical, CONFIRMED — couples A01 D2 / A06 L3) — `AAsset_getLength64` drops r1.** Even the
  SHIM_TEST handler (208) does `S0((u32)L)` — r1 left stale. Disasm confirms the engine consumes a
  **64-bit** length (`@0x6d0634` result used as a size). Stale r1 ⇒ astronomically large/negative
  length ⇒ the engine reads/copies a bogus size ⇒ fault/corruption. *Fix:* return-writer sets
  **r0=low, r1=high** (assets <4 GiB ⇒ high=0, but MUST be *written* 0).
- **G6 (Critical, CONFIRMED) — `AAsset_getBuffer` must return a GUEST pointer.** The real call returns
  a **host** mmap pointer; the engine derefs it in guest address space (`@0x6d0628`→`r5`, then reads).
  *Fix:* **copy `getLength64` bytes into a guest buffer and return the guest pointer**, cached
  per-`AAsset*` (getBuffer is idempotent — same asset ⇒ same guest ptr); freed at `AAsset_close`.
- **G7 (High, CONFIRMED — couples A01 D1) — ≥4-arg + pointer GL calls unserved.** `dispatch()` reads
  only r0–r2; `glTexImage2D(9)`, `glTexSubImage2D(9)`, `glGetActiveUniform(7)`, `glReadPixels(7)`,
  `glVertexAttribPointer(6)`, `glShaderSource(4, string\*\*+length\*)`, `glUniform4f(5)`,
  `glDrawElements(4)`, `glFramebufferTexture2D(5)`, `glCompressedTexImage2D(8)` need r3/stack + pointer
  copies. Catch-all ignores all of it. *Fix:* the per-symbol descriptors below (Audit-01 arg-walker).
- **G8 (High, CONFIRMED) — shader compile/link status + info-log faked.** Handler (198) writes a
  hardcoded `1` for `glGetShaderiv/glGetProgramiv`. Disasm confirms the compile pipeline
  `glCreateShader→glShaderSource→glCompileShader→glGetShaderiv(COMPILE_STATUS)→glGetShaderInfoLog`
  (`@0x5c4de4…`) and `glLinkProgram→glGetProgramiv(LINK_STATUS)`. Faking success while the **real**
  driver may reject the guest GLSL ⇒ the engine binds a dead program ⇒ black screen with no
  diagnostic. *Fix:* real `glGetShaderiv/Programiv`; **round-trip the real info log** (OUT length +
  string).
- **G9 (Medium, CONFIRMED — couples A06 varargs) — `__android_log_print` varargs.** Handler (170)
  copies only the format (`tag=a1, fmt=a2, varargs from a3`); a `%s/%d` diagnostic is emitted as the
  literal format. *Fix:* route through the Audit-06 formatter with first-vararg at the log-specific
  position (`__android_log_write(prio,tag,text)` = plain text, no format).
- **G-INV1 (Critical invariant, CONFIRMED-by-construction) — GL only on the render GEC.** Not a
  present code bug (threading not yet built) but a MUST for this layer: every `gl*` forward executes on
  the host thread that owns the EGL context (the Java GLThread carrying the render GEC, Audit 02). The
  scheduler must **never** resume the render GEC on a pool/foreign host thread, and a `gl*` trapping on
  any non-render GEC ⇒ **fatal** (Audit 04) — provably impossible here (no EGL to arm a second GL
  thread), so it is an assert that guards future builds.

**REFUTED / non-issues:** *no EGL bridge* (zero egl imports); *no client-array per-draw copy* (VBO-
only); *no `glMapBuffer` staging* (unimported); *no `glGetError` round-trip* (unimported — see
residual R1); *no `glGenerateMipmap`/`glBufferSubData`/`glGetStringi`/`glVertexAttrib*f`/`glGetFloatv`/
`glGetBooleanv`/`glGetActiveAttrib`/stencil* handling (all unimported — the GL surface is exactly the
68).

---

### Canonical GL marshalling table — ALL 68 (Audit-01 descriptor + pointer direction)

Descriptor atoms (A06 key): `i`int32 `u`uint32 `f`float32(soft-float, **1 core reg — MUST bit-move
core→VFP for the hardfloat arm64 host call**) `z`size_t `p`ptr; dir `‹in:len›` copy len guest→host
before the call, `‹out:len›` copy len host→guest after, `‹str›` NUL-bounded, `‹off›` **integer VBO
offset — passthrough, NEVER copy**. All are forwarded to the dlsym'd real `gl*`; float-in-reg args are
re-widened core→VFP at the host boundary (A01). GL executes **synchronously** in the bridge ⇒ engine
order is preserved (never batch/reorder). State the shim must shadow: current `GL_UNPACK_ALIGNMENT`/
`GL_PACK_ALIGNMENT` (for copy sizes); a uniform-type map (for `glGetUniformfv` count).

**A. Scalar passthrough (no pointer; forward via descriptor).** `glActiveTexture(u)`,
`glAttachShader(u,u)`, `glBindBuffer(u,u)`, `glBindFramebuffer(u,u)` *(FBO id 0 = default fb — pass
verbatim, never remap)*, `glBindRenderbuffer(u,u)`, `glBindTexture(u,u)`, `glBlendEquation(u)`,
`glBlendFunc(u,u)`, `glClear(u)`, **`glClearColor(f,f,f,f)`**, `glCompileShader(u)`,
`glCopyTexImage2D(u,i,u,i,i,i,i,i)`[8], `glCreateProgram()→u`, `glCreateShader(u)→u`, `glCullFace(u)`,
`glDeleteProgram(u)`, `glDeleteShader(u)`, `glDepthFunc(u)`, `glDepthMask(u)`, `glDetachShader(u,u)`,
`glDisable(u)`, `glDisableVertexAttribArray(u)`, **`glDrawArrays(u,i,i)`**, `glEnable(u)`,
`glEnableVertexAttribArray(u)`, `glFinish()`, `glFlush()`, `glFramebufferRenderbuffer(u,u,u,u)`,
`glFramebufferTexture2D(u,u,u,u,i)`[5], `glFrontFace(u)`, `glLinkProgram(u)`, **`glPixelStorei(u,i)`
— stateful: record GL_UNPACK_ALIGNMENT(0x0cf5, engine sets =1 @0x5c9d0c) / GL_PACK_ALIGNMENT**,
`glRenderbufferStorage(u,u,i,i)`, `glScissor(i,i,i,i)`, `glTexParameteri(u,u,i)`,
**`glUniform1f(i,f)`**, `glUniform1i(i,i)`, **`glUniform4f(i,f,f,f,f)`**[5], `glUseProgram(u)`,
`glValidateProgram(u)`, `glViewport(i,i,i,i)`. *(`f`-marked: bit-move core→VFP.)*

**B. VBO offset passthrough (the "pointers" are integer offsets — determination #2).**
`glVertexAttribPointer(u,i,u,u‹normalized›,i‹stride›,u‹off›)`[6] — last arg forwarded as an integer.
`glDrawElements(u‹mode›,i‹count›,u‹type=0x1403›,u‹off›)`[4] — `indices` forwarded as an integer.
**Defensive guard (inert):** shadow per-attrib {enabled,size,type,stride,ptr} and the bound
ARRAY_BUFFER; if at a draw an *enabled* attrib has ARRAY_BUFFER==0 (client array), copy the referenced
guest range (glDrawArrays: `(first+count)` verts × stride; glDrawElements: read indices, `max_index+1`
verts × stride) to a host staging VBO and repoint. **This branch must never execute** (engine always
binds a VBO); if it does → log+assert (a build-drift tripwire).

**C. IN pointer, explicit size.**
`glBufferData(u,z‹size›,p‹in:size›,u‹usage=0x88e4›)` — copy `size` bytes from `data`; **`data==0` ⇒
allocate-only, no copy**.
`glCompressedTexImage2D(u,i,u,i,i,i,z‹imageSize›,p‹in:imageSize›)`[8] — copy exactly `imageSize`
bytes (compressed; no format math). `data==0`⇒none.
`glShaderSource(u,i‹count›,p‹in›,p‹in›)` — read `count`(=1 @0x5c4df0) guest `char*` from arg2; read
`count` ints from arg3 **iff arg3≠0** (present here @0x5c4dfc) else treat each string NUL-terminated;
copy each source (length[i] or strlen) to host; build host `char*[]`+`GLint[]`; call real.

**D. IN pointer, computed size = rows × align(width×bpp(format,type), UNPACK_ALIGNMENT).**
`glTexImage2D(u,i,i,i‹w›,i‹h›,i,u‹format›,u‹type›,p‹in:computed›)`[9] — **`pixels==0` ⇒ no copy**
(FBO color-attachment alloc — engine has FBOs). `glTexSubImage2D(u,i,i,i,i‹w›,i‹h›,u‹format›,u‹type›,
p‹in:computed›)`[9]. `bpp`: RGBA/UNSIGNED_BYTE=4, RGB=3, LUMINANCE/ALPHA=1, LUMINANCE_ALPHA=2,
RGB565/RGBA4444/RGBA5551=2.

**E. IN pointer, count × components.** `glUniformMatrix4fv(i,i‹count=1›,u‹transpose=0›,p‹in:count*64›)`
— copy `count*16` floats (64 B/matrix). *(Only Matrix4fv of the fv family is imported.)*

**F. OUT array of GL names.** `glGenBuffers/glGenFramebuffers/glGenRenderbuffers/glGenTextures
(i‹n›,p‹out:n*4›)` — call real into a host `GLuint[n]`, copy `n*4` back (**real ids, G3**).
IN array (read, no writeback): `glDeleteBuffers/glDeleteFramebuffers/glDeleteRenderbuffers/
glDeleteTextures(i‹n›,p‹in:n*4›)` — copy `n*4` guest→host, forward.

**G. OUT scalar/vector.** `glGetIntegerv(u‹pname›,p‹out:N*4›)` — **N by pname** (default 1;
VIEWPORT/SCISSOR_BOX=4, MAX_VIEWPORT_DIMS/ALIASED_*_RANGE=2, COMPRESSED_TEXTURE_FORMATS=NUM_… ) — a
pname→N table. `glGetShaderiv(u,u,p‹out:4›)`, `glGetProgramiv(u,u,p‹out:4›)` — 1 int, **real status
(G8)**. `glGetUniformfv(u,i,p‹out›)` — count = the uniform's component count (1..16); track type via
`glGetActiveUniform` bookkeeping; **never over-copy** (would leak host stack past the driver write) —
conservative fallback 4 (vec4) with a logged flag.

**H. OUT string/length.** `glGetShaderInfoLog/glGetProgramInfoLog(u,i‹bufSize›,p‹out:4,len›,
p‹out:≤bufSize›)` — real into host `char[bufSize]`; copy the written string + the `length` int back.
`glGetActiveUniform(u,u,i‹bufSize›,p‹out:4›,p‹out:4›,p‹out:4›,p‹out:name›)`[7] — copy length,size,type
ints + name string. `glGetAttribLocation/glGetUniformLocation(u,s‹str,in›)→i` — copy guest name to
host, forward, return int.

**I. OUT const char\* (host→guest string copy).** `glGetString(u‹name›)→p` — real returns a host
`const char*`; **copy the NUL-terminated string into a cached guest buffer, return the guest pointer**
(G2). Cache keyed by `name` (immutable per context).

**J. OUT pixel buffer, computed size.** `glReadPixels(i,i,i‹w›,i‹h›,u‹format=0x1908›,u‹type=0x1401›,
p‹out:computed›)`[7] — after the real call copy `rows(h) × align(w×bpp, PACK_ALIGNMENT)` back
(RGBA/UBYTE ⇒ 4 bpp @0x5b0a44). Implicit GPU finish ⇒ BEL-release (below).

**BEL/affinity (couples Audit 02):** GL forwards keep the BEL (quick host calls) EXCEPT **`glFinish`
and `glReadPixels`** (GPU drain) which **release the BEL** while blocking, so the audio GEC isn't
stalled — safe because no other GEC ever issues GL. `glFlush` keeps it. Correctness holds either way
(GL is single-thread single-context); the release is an anti-priority-inversion refinement.

---

### AAsset real path (determination #4) — real libandroid; host-ptr → guest-copy; disjoint from Audit 06's FS path

Disasm-confirmed usage (loader `@0x6d05d4…`): `fromJava` → cached `AAssetManager*` at `[g+4]`
(lazy, once-guarded `ldrb;dmb;tst#1`); `open(mgr,path,mode=2=STREAMING)`; a single-slot `AAsset*`
cache at `[obj+8]` (closes the previous before opening the next); `getBuffer`→ptr; `getLength64`→i64.

**Single path:**
1. `AAssetManager_fromJava(env, jobject)` — resolve the guest JNIEnv → the **real** per-thread
   `JNIEnv*` (Audit 01 env-passthrough map) and the guest `jobject` → the **real** `AssetManager`
   jref (Audit 08 handle map); call the **real** libandroid `AAssetManager_fromJava(realEnv,realObj)`
   → real `AAssetManager*`; store in a shim table, return an **opaque guest token** (the engine only
   passes it back to `open`). Idempotent (engine caches it).
2. `AAssetManager_open(token, path‹str,in›, mode)` — copy the guest path to host, call real
   `AAssetManager_open(realMgr, path, mode)` → real `AAsset*`; return an **opaque guest handle**
   (table entry `{AAsset*, guest_copy=0, len}`). NULL AAsset → return 0.
3. `AAsset_getBuffer(handle)` — call real `AAsset_getBuffer` (host ptr) + `AAsset_getLength64` (len);
   **copy `len` bytes into a guest buffer, return the guest pointer** (G6). Cache the guest copy in the
   handle entry — repeat calls return the same guest ptr (idempotent, matches the real API).
4. `AAsset_getLength64(handle)` — return the real length in **r0:r1** (G5).
5. `AAsset_close(handle)` — free the guest copy, real `AAsset_close`, invalidate the handle.

**Where the copy lives (memory cost).** The copy is **mandatory** — Unicorn cannot alias a host mmap
into the guest address space; the bytes must physically live in guest memory. Put them in a
**dedicated ASSET arena, NOT the C++ malloc heap** (Audit 05): asset buffers have AAsset-scoped
(open→getBuffer→close) lifetimes unlike `new`/`delete` objects, and large blocks (texture atlases MBs,
audio banks tens of MBs) would fragment the C++ heap and could spuriously trip the Audit-05 bad_alloc/
emergency-pool path. A separate arena gives clean accounting and graceful failure (arena-full ⇒
`getBuffer`→0, an asset-load error the engine handles) instead of a heap fault. **Carve it from the
dormant GUEST-LIB ARENA (0x60000000, unused per Audit 03's single-lib decision) — e.g.
`0x68000000–0x6FFFFFFF` (128 MiB)** — sized for the peak concurrent open-asset working set (current
level bundle + atlases + the active audio bank); assets are opened/buffered/closed serially, so the
peak is bounded, not cumulative. (Alternative if profiling shows headroom: the Audit-05 allocator with
`free` at `AAsset_close` — but the dedicated arena is the opinionated choice.)

**Router (reconcile Audit 06):** AAsset handles (mine) and FILE\*/fd (Audit 06) are **disjoint handle
spaces on disjoint engine code paths** — bundled RO assets go through `AAssetManager_*` (confirmed
`@0x6d05d4`), saves/cache through `fopen`/`open`+`mmap` (Audit 05/06 `@0x6db1d0`). No token ambiguity.
The **only** overlap is Audit 06's documented fallback: a `fopen` that resolves under the asset tree is
served read-only from the asset store. Assets are **read-only** here (`AAsset_read`/`_seek` not
imported ⇒ whole-buffer access only; no writes).

---

### Residual on-device-only risks (correctness-by-construction cannot eliminate these on an x86 host with no arm64 GPU)

- **R1 — no `glGetError`.** The engine never polls GL errors (unimported). Real-driver errors
  (unsupported format/state) are therefore **silent** to the engine — it proceeds on a mis-rendered or
  empty framebuffer. Matches device behavior, but means our bridge cannot rely on error propagation to
  detect a mismatch; only visual/on-device testing will.
- **R2 — real driver semantics (Samsung Xclipse 540 / RDNA-derived on the A56).** Exact ETC1
  (`GL_ETC1_RGB8_OES`) support for `glCompressedTexImage2D`, NPOT+wrap+filter constraints (no
  `glGenerateMipmap` imported ⇒ textures are non-mipmapped or upload explicit levels; NPOT on ES2
  needs CLAMP_TO_EDGE + non-mip filtering via `glTexParameteri`), default-precision and extension
  strings from `glGetString` — all driver-specific. A path the engine selects from the vendor string
  may not match what the A56 driver actually accepts.
- **R3 — GLSL compilation on a newer, stricter ES2 validator.** Shaders are guest GLSL text compiled
  by the real driver (correct to forward). Source frozen for a 2013-era Adreno may warn/reject on the
  A56 driver ⇒ `COMPILE_STATUS`/`LINK_STATUS` fail ⇒ black screen. G8's real status+log round-trip is
  the *diagnostic* channel, but the fix (if any) is content-side, on-device.
- **R4 — pixel-exactness.** Blend/depth/precision/dithering differences from the original device are
  visual, not correctness-by-construction; unverifiable here.
- **R5 — context-current-thread assumption (Java side).** The proof that GL is single-threaded rests
  on Java making the EGL context current on the exact thread it calls `nativeUpdate`/`nativeInit`/
  `nativeResize` on. That is *forced* by the absence of EGL, but it is a Fusion-framework fact not
  encoded in the `.so`. G-INV1's assert (GL on a non-render GEC ⇒ fatal) is the tripwire.
- **R6 — context loss on pause/resume.** `nativeInterruptRender` is a no-op and the engine issues no
  EGL; whether GL objects survive `onPause` depends on Java's `setPreserveEGLContextOnPause`. If the
  context is lost, object re-creation is the **engine's** job via its normal load path (all Gen/Create
  calls forward normally) — the shim adds nothing, but a build that relied on preservation could show a
  blank surface after resume. On-device only.
- **R7 — asset-copy memory doubling.** Every `AAsset_getBuffer` holds the host mmap **and** a guest
  copy simultaneously; large atlases/audio banks stress the ASSET arena. Sized above; a pathological
  level bundle is a device-only OOM risk.

---

### Invariants (MUST)
1. **No EGL bridge; Java owns context + `eglSwapBuffers`.** The shim issues no EGL and creates no
   surface/context.
2. **G-INV1:** all 68 `gl*` execute **only** on the render GEC, on the context-current Java GLThread;
   the scheduler never migrates the render GEC to a foreign host thread; a `gl*` on any other GEC ⇒
   fatal (A04).
3. **All GL object IDs originate from the real driver** (return real `glCreate*`; write back real
   `glGen*`); the shim never fabricates a name. FBO 0 is never remapped.
4. **Draws are scalar passthrough** — `glVertexAttribPointer`/`glDrawElements` pointer args are
   `GLintptr` VBO offsets, forwarded as integers, **never copied**; the client-array guard is inert and
   asserts if ever hit.
5. **Every IN pointer copied guest→host with the exact computed size** (buffer size arg / imageSize
   arg / rows×align(w×bpp,UNPACK_ALIGNMENT)); **NULL data ⇒ no copy**. Every OUT pointer copied
   host→guest after the real call with the exact written size. `glGetString` returns a **cached guest**
   pointer, never a host pointer.
6. **`AAsset_getBuffer` returns a guest copy** (in the dedicated ASSET arena), idempotent per asset,
   freed at close; **`AAsset_getLength64` writes r0:r1**; asset handles are opaque tokens in a shim
   table, disjoint from Audit 06's fd/FILE space; assets are read-only.
7. **GL float register args are re-widened core→VFP** at the arm64 host-call boundary
   (`glClearColor`, `glUniform1f`, `glUniform4f`); matrix/vector floats travel in memory (byte-copied).
8. **The bridge executes GL synchronously and in order** — never batched or reordered relative to
   emulation; `glFinish`/`glReadPixels` release the BEL during the GPU drain (A02), all other GL keeps
   it.

---

## Audit 08 — JNI passthrough full contract  (agent a9ada42b — JNI bidirectional bridge)

Empirical basis: capstone-5 disasm of the engine (host `objdump` has no ARM target); file
offset==vaddr in LOAD-1, =vaddr−0x1000 in LOAD-2 (Audits 03–07). Method: (a) **env-provenance
dataflow** — `get_env()@0x708880` is the engine's canonical per-thread `JNIEnv*` accessor, reached
by a raw-encoding scan finding **962 ARM `bl` callers, 0 Thumb**; I taint every result and record
`ldr vt,[env]; ldr fn,[vt,#off]; blx fn` (slot=off/4); (b) **contiguous-region enumeration** of the
Fusion JNI C++ wrapper layer `[0x706000,0x70a800)` (per-word ARM decode, resync-robust — a single
linear sweep desyncs on literal pools); (c) **direct disasm of one representative site per slot
family** — every offset/4 matches its canonical `JNINativeInterface` name by BOTH index AND call
semantics (FindClass takes a name; GetMethodID takes class+name+sig; the string triad; the audio
get/release). `x86_64` host + no qemu ⇒ no dynamic boot-trace, but the three static methods
corroborate each other exactly. **The PoC/brief slot guesses are mostly right; the one PoC error is
`171`, which is `GetArrayLength`, not `GetStringUTFLength` (that is `168`).**

---

### THE PIVOTS

1. **The engine is a disciplined C++ JNI client with a real wrapper layer; there is no cached
   `JNIEnv*`.** `JNI_OnLoad@0x1da914` does `bl set_vm@0x70885c` (stores the `JavaVM*` into ONE global;
   **exactly 1 caller**) then returns `0x10006` = `JNI_VERSION_1_6`. Every JNI-using site instead
   calls `get_env()@0x708880` (962×), which reads the cached VM and does **`GetEnv(vm,&env,0x10006)`
   [VM slot 6 = off 0x18]**, and on `JNI_EDETACHED` **`AttachCurrentThread(vm,&env,NULL)` [VM slot 4 =
   off 0x10]**. ⇒ **per-thread env model, hard-coupled to Audit 02**: the shim must resolve the REAL
   env of the *current carrier thread*, never a single shared env. `GetJavaVM(219)` is **never called**
   — the VM is cached at load, not re-derived.
2. **The Java-method call form is EXCLUSIVELY the `va_list` (`…V`) form.** The full family lives as
   contiguous variadic wrappers at `0x708040–0x7087c0`: `Call<T>MethodV` (slots 35,38,41,44,47,50,53,
   56,59,62) + `CallStatic<T>MethodV` (115,118,121,124,127,130,133,136,139,142) + `NewObjectV(29)`.
   The wrapper `push {r2,r3}`es the variadic tail to build a stack `va_list`, then
   `ldr ip,[vt,#0x8c]; blx ip` = `CallObjectMethodV(env,obj,mid,va_list)`. **No varargs form (34), no
   jvalue-array `…A` form (36), no `CallNonvirtual*`.** Marshalling is therefore a **guest-`va_list`
   walk driven by the method signature captured at `GetMethodID` time** (the `sig` char\* is `r3` at
   `GetMethodID(env,clazz,name,sig)@0x67a74`).
3. **`AudioOutput_nativeMixData@0x642324` uses a primitive `byte[]`, NOT a direct `ByteBuffer`.**
   Disasm: `GetByteArrayElements(env,array,NULL)` [184=0x2e0] → mix routine `0x646390` writes into the
   returned pointer → tail `ReleaseByteArrayElements(env,array,elems,mode=0)` [192=0x300]. `mode=0` =
   **commit + free**. **`GetDirectBufferAddress/NewDirectByteBuffer/GetDirectBufferCapacity` (230/229/
   231) are absent everywhere** ⇒ no direct buffers in the whole engine.
4. **`RegisterNatives(215)` is never called** (0 double-deref sites at off 0x35c, excluding the
   `ldr r3,[sp,#0x35c]` stack-slot false-positives). The 72 natives are resolved by the JVM via
   `Java_*` **name mangling**. Strings are **pure modified-UTF-8**; `NewString/GetStringChars/
   ReleaseStringChars/GetStringRegion` (163/165/166/220) are absent (only `GetStringLength(164)` of the
   UTF-16 family is used, as the char-count arg to `GetStringUTFRegion`).

---

### Determination 1 — the used-slot enumeration (JNIEnv + JavaVM)

**JavaVM `JNIInvokeInterface` (from `get_env`):** `AttachCurrentThread` (4=0x10), `GetEnv` (6=0x18).
`DestroyJavaVM(3)/DetachCurrentThread(5)/AttachCurrentThreadAsDaemon(7)` **not invoked by the engine**
(the shim still provides them on the guest VM; detach is driven by the shim at carrier-thread exit,
Audit 02, not by a guest call).

**JNIEnv `JNINativeInterface` — 46 live slots** (env-confirmed via `get_env` provenance = ●;
Fusion `…V` wrapper family = ○; core/audio direct = ◆). A dense JNI table entry has ≥233 slots (max
used = 228); the guest env vtable the shim builds needs 233 trampolines.

| off | slot | name | ev | where / note |
|---|---|---|---|---|
|0x018|6|FindClass|●|wrapper `0x7077d0`: FindClass→ExceptionCheck→tolerate NULL/pending |
|0x038|14|ThrowNew|●|`0x706d84` |
|0x03c|15|ExceptionOccurred|●|`0x70a1b4` (→ jthrowable local token) |
|0x044|17|ExceptionClear|●|`0x707824` |
|0x054|21|NewGlobalRef|●|`0x7079b8` GlobalRef holder ctor (caches jclass/jobject) |
|0x058|22|DeleteGlobalRef|●|`0x707b00` GlobalRef holder dtor |
|0x05c|23|DeleteLocalRef|●|`0x707904/0x707934` **RAII LocalRef holder dtor** (prompt delete) |
|0x074|29|NewObjectV|○|`0x401908` (RCS) |
|0x07c|31|GetObjectClass|●|`0x1d9408` (+ core) |
|0x084|33|GetMethodID|●|`0x67a74` **captures `sig`=r3** ×106 |
|0x08c–0x0f8|35,38,41,44,47,50,53,56,59,62|Call{Object,Boolean,Byte,Char,Short,Int,Long,Float,Double,Void}**MethodV**|○|`0x708040–0x7083a0` |
|0x178|94|GetFieldID|●|`0x686ac` ×2 |
|0x1c4|113|GetStaticMethodID|●|`0x1d9cd0` ×63 |
|0x1cc–0x238|115,118,121,124,127,130,133,136,139,142|CallStatic{…}**MethodV**|○|`0x708400–0x708790` |
|0x240|144|GetStaticFieldID|●|`0x67c34` ×7 |
|0x290|164|GetStringLength|●|**UTF-16 char count** (→ len arg of GetStringUTFRegion) ×57 |
|0x29c|167|NewStringUTF|●|guest→Java strings (call args) ×82 |
|0x2a0|168|GetStringUTFLength|●|**modified-UTF-8 byte count** (sizes dest buf) ×65 |
|0x2a4|169|GetStringUTFChars|◆|`0x1da7f8,0x1de614,0x1e33c0` (core) ×3 |
|0x2a8|170|ReleaseStringUTFChars|◆|`0x1da824` ×1 |
|0x2ac|171|GetArrayLength|●|`0x3fc614` (RCS String[]) |
|0x2b0|172|NewObjectArray|●|`0x3fef38` |
|0x2b4|173|GetObjectArrayElement|●|`0x3fc64c` |
|0x2b8|174|SetObjectArrayElement|●|`0x3ff054` |
|0x2e0|184|GetByteArrayElements|◆|**audio** `0x642348` |
|0x300|192|ReleaseByteArrayElements|◆|**audio** `0x642378` mode=0 |
|0x374|221|GetStringUTFRegion|●|`0x1da0b4` core string-read triad ×57 |
|0x390|228|ExceptionCheck|●|**checked after nearly every call** ×255 |

**ABSENT (verified 0 double-deref sites) — the surface is bounded, and these need only defensive
stubs:** `RegisterNatives(215)`, `UnregisterNatives(216)`, `MonitorEnter/Exit(217/218)`,
`GetJavaVM(219)`, `GetStringRegion(220)`, all `GetPrimitiveArrayCritical/GetStringCritical(222–225)`,
`NewWeakGlobalRef/DeleteWeakGlobalRef(226/227)`, `NewDirectByteBuffer/GetDirectBufferAddress/Capacity
(229–231)`, `GetObjectRefType(232)`, **all `Get/Set<T>Field(95–112)` and static `(145–162)`** (the
engine takes field *IDs* but the reads/writes are ≤9 and did not surface — support them generically,
but they are minor), all primitive `New<T>Array/Get<T>ArrayRegion/Set<T>ArrayRegion`. Dead-RCS-only,
ambiguous vs C++ vtable noise (do NOT rely on): `PushLocalFrame/PopLocalFrame(19/20)`,
`NewLocalRef(25)`, `EnsureLocalCapacity(26)` — spec them (they may fire in the phone-home layer) but
they are not on the live gameplay path.

---

### Determination 2 — the Call form and the `va_list`→host-`jvalue[]` marshalling

**Form = `Call<Type>MethodV` / `CallStatic<Type>MethodV` / `NewObjectV`.** When the guest invokes
slot 35 it passes `(guest_env, obj_token, mid_token, guest_va_list_ptr)`, where `guest_va_list_ptr`
points into **guest** stack memory holding the variadic args in **AAPCS32 soft-float** order (32-bit
scalar = 1 word; `jlong/jdouble` = 2 words, 8-byte-aligned; `jfloat` = 1 word bit-pattern — never a
d-reg; Audit 01). The passthrough (one generic `Call*MethodV` handler, descriptor-driven):
1. Resolve `real_env` = **current GEC carrier's** env (Audit 02); `real_obj` = `obj_token→real`;
   `(real_mid, descriptor)` = `mid_token→id-table entry` (descriptor captured at GetMethodID).
2. Walk the guest `va_list` per `descriptor.args`: read each arg from guest memory and build a host
   `jvalue`: `jboolean/jbyte/jchar/jshort/jint`→`.i` (4 B; **canonicalize jboolean→0/1**),
   `jfloat`→`.f` (4 B bitcast, core→v-reg happens in the host call), `jlong`→`.j` (8 B),
   `jdouble`→`.d` (8 B), `jobject/jstring/jarray`→**`token→real`**, `.l`. NULL token 0 ⇒ real NULL.
3. **Call the REAL `Call<Type>MethodA(real_env, real_obj, real_mid, jvalue[])`** — the `…A` form is
   the clean host target (`jvalue` is 8-aligned; no host va_list synthesis). Static variants take
   `real_clazz`; `NewObjectV`→`NewObjectA`.
4. Convert the return into the guest per class: `jobject`→**fresh local token**; `jint`→r0
   (sub-word sign/zero-extended, Audit 01); `jlong`→r0:r1; `jfloat`→r0 bits; `jdouble`→r0:r1 bits;
   `jboolean`→canonical r0; `void`→nothing.
5. The real Call runs arbitrary Java (may block / re-enter native → guest): **release the BEL around
   it** (Audit 02 blocking discipline) — `uc_context` save, mark BLOCKED, release, real call,
   reacquire, marshal return. Token alloc/free brackets the call *under* the BEL.
6. Do **not** touch the pending-exception state; the engine's own `ExceptionCheck(228)` (which follows
   almost every call) reads the real state.

**Descriptor capture (the enabler):** at `GetMethodID(env,clazz,name,sig)` [33] and
`GetStaticMethodID` [113] the shim reads the guest `sig` char\* (modified-UTF-8 JNI descriptor, e.g.
`"(Ljava/lang/String;IF)V"`), parses it into `(arg-atom list, ret-atom)`, calls the real GetMethodID,
and stores `{real jmethodID, descriptor}` in the id-table under the returned token. **This is the ONLY
place the arg types exist** — the `va_list` is untyped.

---

### Determination 3 — THE HANDLE TABLE (the crux)

The guest is LP32; every `jobject/jclass/jstring/jarray/jthrowable/jweak/jmethodID/jfieldID` is a
64-bit host pointer. A real ref/ID can NEVER be handed to the guest — it crosses only as a **32-bit
token**. ONE bidirectional table, defined first; everything else uses it.

**Token shape.** `token = (kind<<28) | index`, `kind ∈ {0 NULL, 1 local, 2 global, 3 weak, 4 id}`,
`index` = dense free-list slot per kind (28 bits ⇒ 256 M — never a real limit). **Token 0 ⇔ real
NULL ⇔ null jobject** is a hard invariant (kind 0, index 0). Small dense ints, not tagged pointers —
the guest treats a token as an opaque `jobject`; it must never be dereferenced guest-side and never
collide with a real address (28-bit kind-tagged ints can't).

**Two entry classes.**
- **Object refs** (`kind 1/2/3`): entry = `{real jobject, kind, frame_depth (local only)}`. Reverse
  lookup is **not** used for identity (see IsSameObject); a fresh token per real ref.
- **IDs** (`kind 4`): entry = `{real jmethodID/jfieldID, parsed descriptor (methods only)}`. IDs are
  **stable for the class's lifetime** (and the class is global-ref-cached) ⇒ id tokens are permanent,
  **deduped** by real ID via a `real→token` host hashmap (the engine re-looks-up/caches IDs; same real
  ID must return the same token). Never freed during a session.

**Ref-kind + lifetime — kept in LOCKSTEP with the real ref (delete the real ref exactly when the
token is freed, so neither the JVM tables NOR the shim table leak over a long session):**
- **Local** (FindClass, GetObjectClass, NewObjectV, `CallObjectMethodV`→Object, GetObjectArrayElement,
  NewStringUTF, ExceptionOccurred, entry-thunk `thiz`/`clazz`/jobject-args): valid only within ONE
  native activation. **Model (A), chosen:** rely on the JVM's automatic per-native local frame to free
  the REAL locals when the thunk returns; the shim frees its own local **tokens** at thunk return
  (frame-scoped, per-GEC frame stack for re-entrancy). Additionally the shim **honors every explicit
  guest `DeleteLocalRef(token)`** [23] → real DeleteLocalRef + free token immediately. The engine wraps
  locals in **RAII holders that call DeleteLocalRef promptly** (`0x707904/0x707934`), so the live real
  local count stays far below the 512 default — no shim auto-`EnsureLocalCapacity` needed (a per-
  activation token-count tripwire logs build-drift). **The returned jobject is handed back as its real
  local ref** (the JVM caller frame takes ownership) — the shim must NOT delete it in the frame pop.
- **Global** (NewGlobalRef `0x7079b8`, ×2 — cached jclass + a jobject): `NewGlobalRef(token)`→ real
  NewGlobalRef → **new global token**, persists across activations; not in any local frame ⇒ not
  auto-freed. `DeleteGlobalRef(token)`→ real DeleteGlobalRef + free token. **Cached jclass/jmethodID
  MUST ride real globals** or they dangle after the caching thunk returns (the engine correctly
  promotes them — the shim must actually create the real global, not alias the local token).
- **Weak** (absent here; spec'd): `NewWeakGlobalRef/DeleteWeakGlobalRef`; resolving a weak token may
  yield real NULL after GC; `IsSameObject(weak, NULL)` is the liveness test.

**Identity / NULL.** `IsSameObject(a,b)` [24] forwards to the **real** IsSameObject on the resolved
pointers (JNI does NOT guarantee ref pointer-equality ⇒ never compare tokens); `IsSameObject(0,0)`=
`JNI_TRUE`; `IsSameObject(tok,0)` resolves to `real vs NULL`. No local-ref dedup (each real ref → a
distinct token — JNI-correct; the engine uses IsSameObject, not `==`).

**Thread-safety (Audit 02 BEL).** The global-ref table + id table are process-wide; local-ref frames
are per-GEC. All alloc/free runs inside the passthrough **under the BEL** (bridge atomicity) — the
alloc/free brackets the BEL-released real call-out, so the table is consistent without an extra lock.

---

### Determination 4 — the 72 entry thunks (entry side)

72 exported ARM `Java_*` at fixed engine addresses (`0x1da724…0x642324`; core gameplay at `0x1da–
0x1e3` + `0x642324`; the rest are the inert phone-home subsystems, Audit 03/06). **ONE generic
descriptor-driven thunk** (Audit 01's thunk = D8 fix). The real JVM calls each arm64 thunk per
AAPCS64 `(JNIEnv* x0, jobject/jclass x1, <java args in x2–x7 / v0–v7 by type>)`. Marshal **real→guest**
into the guest soft-float core-register ABI:
- `env`→the **single shared guest `JNIEnv*` sentinel** (a guest `JNIEnv**` whose 233-entry vtable are
  trap trampolines; the passthrough resolves the REAL env from the *current carrier's* GEC, so one
  sentinel is correct — `nativeMixData@0x642334` derefs `env`=r0 directly, so the sentinel MUST be a
  valid guest double-pointer). `thiz`/`clazz`→**fresh local token**.
- `jint/jboolean/jbyte/jchar/jshort`→zero/sign-extended into one guest core reg (jboolean 0/1);
  `jlong/jdouble`→core-reg pair (8-aligned, soft-float); `jfloat`→one core reg (bitcast from v-reg —
  the guest reads it via `vmov s,rN`, e.g. `nativeInput@0x1e0528 vmov s14,r3`); `jstring/jarray/
  jobject`→fresh local token.
Then **`emu_call_v`** (Audit 01) into `BASE+Java_*addr` (ARM). Marshal **guest return→real** by the
native's return class: token→real jobject (a real local ref the JVM caller frame owns); core→`jint`
(sub-word extended); r0:r1→`jlong`; r0→`jfloat`(v0); r0:r1→`jdouble`(d0); r0→canonical `jboolean`.
Pop the activation's local-token frame (freeing all except the returned ref). **Signatures** come from
the Java native declarations in the APK `classes.dex` (authoritative; the APK was unavailable here) —
partially inferable from the engine (float args via `vmov s,rN`). CORE signatures that matter most:
`nativeInit(env,thiz,…)`, `nativeConfig(env,thiz,jobject/jstring)`, `nativeUpdate` (per-frame),
`nativeResize(env,thiz,jint w,jint h)`, `nativeInput(env,thiz,jint,jint,jfloat,jfloat,…)`,
`nativeKeyInput/nativeInputAxis` (int/float), `nativeMixData(env,thiz,…,jbyteArray,jint,jint)`,
`nativeRender→jboolean`, `nativeRenderThread→jboolean` (Audit 07: near-empty). The **dead subsystem
thunks use the identical generic mechanism**; their Java side never fires (the SDKs never init), so
they are exported-and-safe but never entered.

---

### Determination 5 — RegisterNatives vs name discovery

**Name discovery. `RegisterNatives(215)` is NOT called** (verified: 0 double-deref sites at off
0x35c). The 72 `Java_*` are exported with canonical JNI name mangling ⇒ the JVM resolves them by name
at first call. The passthrough needs no RegisterNatives on the live path. **Defensive spec (dormant):**
if a future build calls `RegisterNatives(env,clazz,JNINativeMethod[],n)`, the array holds **guest**
function pointers; the shim must, per entry, allocate a REAL arm64 thunk (the same generic thunk,
parameterised by the guest fn address + the `signature` string in the `JNINativeMethod`) and call the
real `RegisterNatives` with those real thunks. Not needed here.

---

### Determination 6 — strings / arrays / fields / direct buffers

- **Strings — modified-UTF-8 only.** Create: `NewStringUTF(guest_char*)` [167] — copy the guest
  NUL-bounded bytes to a host temp, real NewStringUTF, return a **local token**. Read (the core triad,
  `0x1da020`, NativeApplication — live): `GetStringLength`[164]→UTF-16 char count `L`;
  `GetStringUTFLength`[168]→modified-UTF-8 byte count (sizes the guest dest, +1 NUL);
  `GetStringUTFRegion(jstr, 0, L, guest_buf)`[221] — real call into a host temp, then **byte-copy the
  written modified-UTF-8 into `guest_buf`** (guest ptr at `sp[0]`). `GetStringUTFChars/Release`[169/170]
  (core ×3/×1): real GetStringUTFChars→host bytes→copy into a guest buffer→return guest ptr; Release
  frees the guest copy (real ReleaseStringUTFChars on the host ptr). All byte-copy (Audit 06).
- **Arrays — object arrays (RCS) + the one audio primitive array.** `GetArrayLength`[171],
  `NewObjectArray`[172], `GetObjectArrayElement`[173]→local token, `SetObjectArrayElement`[174] (token→
  real) — pure token marshalling, no bulk copy. **Audio (determination):** `nativeMixData` →
  `GetByteArrayElements(array,NULL)`[184] returns a **host** pointer the guest can't alias ⇒ the shim
  **allocates a guest buffer of `GetArrayLength(array)` bytes, copies host→guest, returns the guest
  ptr**, and remembers `{guest_ptr, host_ptr, array_token, len}`; the guest mix routine writes into the
  guest buffer; `ReleaseByteArrayElements(array, guest_ptr, mode=0)`[192] — **copy guest→host, then
  real Release(host_ptr, 0)** (mode 0 = commit+free; honor 1=JNI_COMMIT keep, 2=JNI_ABORT discard).
  The copy-back at Release is the audio-correctness sync point.
- **Fields.** `GetFieldID`[94]×2 / `GetStaticFieldID`[144]×7 → id token (fieldID). The `Get/Set<T>
  Field` reads are ≤9 and did not surface on the live path — supported generically (token/ID resolve;
  64-bit `GetLongField/GetDoubleField`→r0:r1; `SetObjectField` value = token→real).
- **Direct buffers — NONE.** `GetDirectBufferAddress/NewDirectByteBuffer/GetDirectBufferCapacity` are
  absent engine-wide. No shared-memory/host-alias path is needed. Audio is fully covered by the
  byte[] get/release copy-back above. `GetPrimitiveArrayCritical/GetStringCritical` also absent (no
  pinning path — a plain copy suffices; nothing to note for perf).

---

### Determination 7 — exceptions + JavaVM

- **Exceptions forward verbatim; never swallowed.** `ExceptionCheck`[228] (×255 — after nearly every
  call), `ExceptionOccurred`[15] (→ jthrowable local token), `ExceptionClear`[17], `ThrowNew`[14],
  and defensively `Throw(13)/ExceptionDescribe(16)/FatalError(18)` forward to the real env. A **Java
  pending exception when a thunk returns is left pending** — the JVM handles it; the shim must not
  clear it. **Java pending exceptions (state on the real env) are SEPARATE from guest C++ exceptions
  (in-guest, Audit 04) — never conflate.** The FindClass wrapper `0x7077d0` (FindClass→ExceptionCheck→
  branch) converts a Java pending exception into the engine's own C++ error handling ⇒ the shim's
  `ExceptionCheck` MUST return the TRUE real state (a fabricated 0 makes the engine march past a real
  pending exception → the next real JNI call aborts). `FatalError`[18] is also the Audit-04 fatal-
  channel sink (process abort with message).
- **JavaVM.** The guest gets a **guest `JavaVM*` sentinel** whose `JNIInvokeInterface` traps: `GetEnv`
  (6)/`AttachCurrentThread`(4)/`AttachCurrentThreadAsDaemon`(7)/`DetachCurrentThread`(5)/`GetVersion`
  forward to the **real** `JavaVM` (already `g_realvm` in abshim.c). Guest `JNI_OnLoad(guestVM)`
  (emu_call'd) returns `JNI_VERSION_1_6`; the shim's own real `JNI_OnLoad` returns `JNI_VERSION_1_6`
  to the JVM. `GetEnv/AttachCurrentThread` hand back the guest env **sentinel**; the shim binds the
  carrier's REAL env in the GEC. **Guest-spawned threads** (the one `pthread_create`, Audit 02) that
  call Java: the carrier host thread calls the real `AttachCurrentThread` → real per-thread env in its
  GEC, and **`DetachCurrentThread` at carrier-thread ZOMBIE** (else a JVM thread leak).

---

### Determination 8 — not-thought-of

- **Local-ref overflow:** not a risk here — the engine RAII-deletes locals promptly (`DeleteLocalRef`
  holders) ⇒ live real locals ≪ 512; the shim relies on JVM auto-pop + honors explicit deletes; a
  per-activation token-count tripwire logs future build drift.
- **jclass/jmethodID caching across calls:** cached as **real globals** (NewGlobalRef) — the shim must
  create real globals, not alias local tokens, or they dangle (the engine already promotes correctly).
- **static vs instance `arg1`:** static natives get `clazz`, instance get `thiz` — both a fresh local
  token; the generic thunk is agnostic.
- **jboolean canonicalization:** `JNI_TRUE`=1 both directions (`nativeRender`/`nativeRenderThread`
  return booleans — canonicalize the returned byte).
- **NULL name/sig defensive:** a NULL `sig` at GetMethodID forwards NULL (real env returns NULL +
  pending exception) — no descriptor stored; the engine's ExceptionCheck catches it.
- **Re-entrancy (native→Java→native→guest):** per-GEC **local-frame stack**; the inner activation's
  tokens free on its return, the outer's persist (mirrors JVM frame nesting; couples Audit 02 C7
  `uc_context` save/restore).
- **Absent classes in a stripped/offline APK:** `FindClass`→NULL+pending exception; the wrapper's
  `ExceptionCheck` branch tolerates it (the dead subsystems then no-op) — the shim forwards faithfully;
  the JVM never calls those thunks anyway (SDKs never init, Audit 03/06).
- **Guest env sentinel identity:** one shared sentinel is safe (natives use env on their own thread;
  cross-thread env is re-derived via `get_env`→GetEnv). Ultra-defensive option: a per-GEC sentinel so
  cross-thread guest env-pointer comparisons differ — not required.

---

### Defects (J-series). Current abshim.c has **no** real JNI passthrough (the `#ifndef SHIM_TEST` ship path stops at the line-303 TODO; the SHIM_TEST `jni_hook/jni_setup` `0x11000000` tables are host-test fakes with fabricated handles).

- **J1 (Critical, CONFIRMED) — no handle table.** There is no 32↔64 token map. SHIM_TEST `jni_hook`
  (228–238) returns `hc+=8` fakes with no reverse map, no ref-kind, no lifetime; the ship path has
  nothing. A real 64-bit `jobject/jclass/jstring/jmethodID/jfieldID` cannot be handed to the LP32
  guest at all. *Fix:* the Determination-3 table (kind-tagged tokens, per-kind lifetime, token 0⇔NULL).
- **J2 (Critical, CONFIRMED = A01 D8) — `emu_call` can't drive the thunks.** `emu_call` (248) sets
  only r0–r3 and returns only r0 ⇒ cannot pass a 5th arg / `jlong`·`jdouble` pair / `jfloat`, nor
  return 64-bit/float, nor place the `va_list` beyond r3. *Fix:* `emu_call_v` (Audit 01).
- **J3 (Critical, CONFIRMED) — Call-form is `va_list`, not varargs/jvalue.** Any passthrough that read
  trap-time registers (varargs) or expected a guest `jvalue[]` (`…A`) would mis-marshal every Java
  call. The engine passes `CallXMethodV(env,obj,mid, guest_va_list)`. *Fix:* walk the **guest
  `va_list`** per the captured signature → host `jvalue[]` → real `CallXMethodA` (Determination 2).
- **J4 (Critical, CONFIRMED) — signature not captured at GetMethodID.** The PoC `jni_cb` (350) returns
  a fake handle for GetMethodID and **discards `sig`** ⇒ at `CallXMethodV` there is no arg-type info ⇒
  the untyped `va_list` cannot be walked (wrong widths/alignment, `jobject` args not resolved) ⇒
  corruption/crash. *Fix:* parse+store the `sig` (r3) descriptor in the id-table entry.
- **J5 (Critical, CONFIRMED = A02 coupling) — single shared env.** `JNIEnv*` is thread-local; the
  engine re-derives it per thread via `GetEnv`/`AttachCurrentThread`. The PoC uses one `g_ENV` (236)
  for all threads; a real env used off its own thread is a JNI crash. *Fix:* resolve the REAL env from
  the current GEC carrier; one guest env **sentinel** whose passthrough dereferences the carrier's env.
- **J6 (Critical, CONFIRMED) — local-ref lifetime unmanaged.** Without per-activation token frames +
  honoring the engine's `DeleteLocalRef`, either the JVM's 512-slot local table overflows (leak → the
  next real JNI call aborts) or freed refs dangle. The PoC never frees. *Fix:* Model (A) — free local
  **tokens** at thunk return (per-GEC frame stack), honor explicit `DeleteLocalRef`, rely on JVM
  auto-pop for real locals, keep the returned ref.
- **J7 (High, CONFIRMED) — global-ref lifetime.** Cached jclass/jobject MUST be real globals; aliasing
  them as local tokens leaves the engine's cached class **dangling** on the next call (use-after-frame).
  *Fix:* `NewGlobalRef`→real global + persistent token; `DeleteGlobalRef`→real + free.
- **J8 (Critical, CONFIRMED) — audio byte[] copy-back missing.** `GetByteArrayElements` returns a HOST
  pointer; handed to the guest it either aliases nothing (silence) or faults on deref. No audio array
  path exists (SHIM_TEST-absent; ship-absent). *Fix:* guest-copy on Get, **write-back on
  `ReleaseByteArrayElements(mode=0)`** (commit+free) — the audio sync point.
- **J9 (High, CONFIRMED) — `GetStringUTFRegion` write-back missing.** The core string-read triad
  writes modified-UTF-8 into a **guest** buffer (`sp[0]`); the real env writes a host temp. Without the
  copy-back every Java string the engine reads (config, file paths, locale) is empty/garbage. *Fix:*
  real GetStringUTFRegion into a host temp sized by GetStringUTFLength, byte-copy into the guest buf.
- **J10 (High, CONFIRMED) — `ExceptionCheck` fabricated.** The PoC returns 0 for slot 228 (350). The
  engine branches on ExceptionCheck after ~every call (×255) — a fake 0 makes it march past a real
  pending Java exception ⇒ the next real JNI call aborts (JNI forbids most calls with an exception
  pending). *Fix:* forward to the real `ExceptionCheck`; never fabricate.
- **J11 (Medium, CONFIRMED) — VM-slot fakes.** SHIM_TEST `jni_hook` writes the fake `g_VM/g_ENV` for
  GetEnv/AttachCurrentThread (232–236) and returns `0x10006` inline; ship must forward to the real
  `JavaVM` (`GetEnv`=6/`AttachCurrentThread`=4) and bind the carrier's real env. *Fix:* Determination 7.
- **J12 (Medium, CONFIRMED = A01 return-writer) — sub-word/boolean returns.** The entry thunk must
  canonicalize `jboolean`→0/1 and sign/zero-extend `jbyte/jchar/jshort` returns per AAPCS64;
  `nativeRender`/`nativeRenderThread` return a boolean/flag byte whose high bits must be clean. *Fix:*
  the Audit-01 return-writer at the thunk boundary.
- **J13 (Low, REFUTED as a live bug — assert only) — RegisterNatives / direct buffers / weak refs /
  monitors / Critical / UTF-16 chars / GetJavaVM are all UNUSED.** Provide safe defensive stubs; a hit
  on `GetDirectBufferAddress`/`RegisterNatives`/`MonitorEnter` ⇒ **log + assert** (build-drift
  tripwire), provably never taken in this build.

---

### Residual on-device-only risks

- **R-J1 — native signatures come from `classes.dex`.** The exact arg/return JNI types of the 72
  thunks are declared Java-side; the APK was unavailable here (only partly inferable from the engine,
  e.g. `nativeInput`'s floats via `vmov s,rN`). A mis-declared thunk descriptor (float vs int, missing
  a `jlong`'s 8-alignment) mis-marshals — the DEX is authoritative and must be read on-device.
- **R-J2 — modified-UTF-8 edge cases.** Supplementary chars (CESU-8 surrogate pairs) and embedded
  NULs: the shim byte-copies and the guest sizes via `GetStringUTFLength`, so it is correct by
  construction, but only device strings exercise the surrogate path.
- **R-J3 — JVM local-ref capacity under regressed engine hygiene.** Bounded here by the RAII
  `DeleteLocalRef` discipline; a future build that leaks locals could overflow the 512 default — the
  tripwire is the only warning.
- **R-J4 — thread attach/detach accounting.** A guest worker that attaches must detach at exit;
  couples Audit 02 thread-exit. Mis-accounting is a JVM thread/ref leak visible only under a long
  device session.
- **R-J5 — `va_list` ABI at the Unicorn boundary.** The guest `va_list` is AAPCS32 soft-float in guest
  memory; the shim must honor 8-byte alignment for `jlong/jdouble` exactly (Audit 01). Correct by
  construction; only device workloads with mixed-width Java args stress it.

---

### Invariants (MUST)
1. **Every ref/ID crosses only as a kind-tagged 32-bit token**; token 0 ⇔ real NULL; the guest never
   sees or derefs a real 64-bit pointer. Real ref freed **exactly** when its token is freed.
2. **`JNIEnv*` is per-carrier-thread**, resolved in the passthrough from the current GEC (Audit 02);
   one shared guest env/VM sentinel; the `JavaVM` is cached once at `JNI_OnLoad` (`set_vm`), never
   re-derived (`GetJavaVM` unused).
3. **Java calls are `Call<T>MethodV`** — walk the **guest `va_list`** per the **GetMethodID-captured
   signature**, resolve `jobject` args token→real, build a host `jvalue[]`, call the real `…A`; convert
   the return by class (jlong/jdouble→r0:r1, jfloat→r0, jobject→local token). Release the BEL around the
   real call.
4. **Local tokens are activation-scoped** (per-GEC frame stack; freed at thunk return except the
   returned ref; explicit `DeleteLocalRef` honored); **globals persist until `DeleteGlobalRef`**; cached
   jclass/jmethodID ride **real** globals.
5. **`GetByteArrayElements`/`GetStringUTFChars`/`GetStringUTFRegion` return GUEST copies**; the audio
   `byte[]` is **written back at `ReleaseByteArrayElements(mode=0)`** (commit); modified-UTF-8 is
   byte-copy both directions. No direct buffers exist.
6. **Exceptions forward verbatim and are never swallowed**; `ExceptionCheck` returns the TRUE real
   state; a pending exception at thunk return is left for the JVM; Java pending exceptions are disjoint
   from guest C++ exceptions (Audit 04); `FatalError` is the shared fatal sink.
7. **The 72 natives are name-resolved** (no `RegisterNatives`); each is the ONE generic
   descriptor-driven arm64 thunk (real AAPCS64 → guest soft-float via `emu_call_v` → real return).
8. Unused slots (RegisterNatives, GetJavaVM, monitors, weak, direct buffers, Critical, UTF-16 chars,
   Get/Set field families beyond the ≤9 ID lookups) get safe stubs that **assert on use** — provable
   build-drift tripwires.

---

## Audit 09 — Synthesis: single-path architecture, seams, and invariants  (agent a9ada42b — synthesis/final)

Role: NOT new binary facts — the interlock check. Audits 01–08 each hardened one layer; this audit
fuses them into ONE opinionated path and resolves the SEAMS where two layers' designs must mesh but
could conflict. Every claim below is grounded in the folded specs (line refs to this doc) and the
current `abshim.c`. Criterion: correctness, ONE mode, opinionated by construction. Verdict up front:
the eight audits interlock cleanly except for **11 seams (S1–S11)**, of which **S1/S2/S3/S6 are
correctness-critical** — one stale map cell, one unsound-if-read-literally concurrency step, one
unverified emulator-reentrancy dependency, and one missing fault sink. All resolved below.

---

### S-series — the newly-identified cross-layer seams/defects (each with a decisive resolution)

- **S1 (Critical) — the JNI sentinel has no home in shipping; A05's map says it isn't mapped.**
  A05's map (line 464): `JNISTUB (host-test only) 0x11000000 … shipping = real JNIEnv, not mapped`.
  A08 (Det.4/7, lines 1420-1423, 1496) *requires in shipping* a **guest** `JNIEnv*` sentinel whose
  233-slot vtable is trap trampolines and a guest `JavaVM*` sentinel — the real 64-bit env/VM can
  **never** be handed to the LP32 guest (`nativeMixData@0x642334` derefs `env`=r0 directly). The map
  cell is stale (predates A08). **Resolution:** the 0x11000000 region is **mapped in shipping** and
  renamed the **JNI trampoline arena** (233 env-slot + 8 VM-slot `bx lr` trampolines). The sentinel
  **P-cells + vtables** and the `glGetString` cache go in **GUESTDATA** (0x12000000, <4 KiB used) —
  **not** balloc'd from the game heap as the SHIM_TEST `jni_setup` does (abshim.c:242-243); permanent
  shim structures must not mix with the A05 coalescing heap.

- **S2 (Critical) — "release BEL around a call-out" is UNSOUND if the block happens inside the hook.**
  A02 (lines 137-140) prescribes `uc_context_save → mark BLOCKED → release BEL → do it → reacquire`.
  Read literally (block *inside* the `UC_HOOK_CODE` while `uc_emu_start` is still on the carrier's
  stack), another carrier then acquires the BEL and calls `uc_*` on the **same `uc`** — reintroducing
  the very **C1** race A02 exists to kill (one `uc`, single-threaded). **Resolution:** the blocking/
  reentrant discipline MUST be **stop/restart**, exactly A04's longjmp mechanism (line 319): the hook
  does `uc_context_save` → sets a **pending-request** (op + return-PC=LR + result-class) → `uc_emu_stop`
  → the outer `uc_emu_start` **returns**; the **carrier loop** (not the hook) then releases the BEL,
  performs the blocking op with `uc` on **no** thread's stack, reacquires, writes the result into the
  saved context, restores, and re-issues `uc_emu_start` at LR. A04's stop/restart, A02's BEL-release,
  and A08's "release BEL around the real Call" are **the same one mechanism**. Inline block-in-hook is
  forbidden.

- **S3 (Critical, verification) — nested guest execution rests on unverified `uc_emu_start` reentrancy.**
  A06 (line 795) runs the qsort/bsearch comparator "nested under BEL with per-GEC context save/restore
  (A02 C7)"; A02 C7 (lines 116, 134) fixes reentrancy with `uc_context` save-on-entry/restore-on-exit —
  i.e. a **host-recursive nested `uc_emu_start`** on one `uc`. Whether Unicorn 2.1.x tolerates a nested
  `uc_emu_start` from within a hook is **unproven** (no arm64 test host). **Resolution / opinionated
  split:** confine nested `uc_emu_start` to **provably-non-blocking leaf callbacks only** — qsort/bsearch
  comparators (pure compares; no JNI, no blocking, no further callback; asserted). **Everything that can
  block or re-enter** — `pthread_once` init, JNI `Call*Method`, all guest waits — uses **S2 stop/restart**
  (which flattens re-entry: an outer call unwound by `uc_emu_stop` lets the re-entrant native run as a
  fresh top-level, never nested on the host stack). MUST-verify on target Unicorn 2.1.x: (a) nested
  `uc_emu_start` composes with `uc_context` save/restore; (b) `uc_emu_stop`-from-hook then re-issue
  resumes at an arbitrary PC. If (a) fails, hand-write a yielding qsort/bsearch state machine (the only
  two bulk-callback bridges) driven by the carrier loop. This is the single deepest device dependency.

- **S4 (High) — the state-preservation contract omits callee-saved VFP.** A01's `emu_call_v` guarantees
  only "r4–r11/sp preserved" (line 76); A04's `setjmp` snapshots **d8–d15** (line 314); A06 uses inline
  VFP in-guest. A nested call or a preemptive slice mid-`vldr/vcvt` would corrupt an outer computation's
  d8–d15/FPSCR unless preserved. **Resolution:** the preservation contract is the **full `uc_context`**
  (A02) — it MUST include d0–d31 + FPSCR; the A01 "r4–r11/sp" list is the *integer* subset only. Every
  per-GEC save (slice, reentry, blocking) captures full VFP state. (CPACR/FPEXC are set once, identical
  across GECs — no per-GEC save needed.)

- **S5 (High) — syscall handling is split and incomplete; the futex path is the `syscall` libc import.**
  A04 X10 (line 282) shows `__cxa_guard_acquire`'s contended wait is `syscall(__NR_futex=0xf0,…)` via the
  **`syscall` libc function** (PLT 0x37944), *not* an SVC; A01 also cites SVC syscalls. Current
  `intr_hook` (abshim.c:222-224) handles only nr 20/224→1234 and has no futex. **Resolution:** ONE
  `do_syscall(nr,args)` table serves BOTH the `syscall` libc bridge (STUB) AND the SVC `intr_hook`;
  `__NR_futex` → the real **BEL-releasing** guest-block (S2 stop/restart on the guard word, keyed in the
  A02 sync shadow tables), `__NR_gettid`→GEC.guest_tid, `__NR_clock_gettime/gettimeofday`→the A06 time
  bridge (fill the timespec — L9), `__NR_getpid`→fixed pid, `__NR_sched_yield`→yield; unknown → −ENOSYS.
  The `syscall` libc bridge is **mandatory** (not optional) — without it the C++ static-init futex wait
  busy-spins (X10).

- **S6 (High) — a Unicorn fault (incl. the A05 guard pages) is laundered into success.** `emu_call`
  (abshim.c:255) logs a `UC_ERR` and returns stale r0 — the same launder class as A04 X4. A05's heap/
  stack guard pages surface as `UC_ERR_*_UNMAPPED`. **Resolution:** the carrier loop classifies every
  `uc_emu_start` return: `PC==RET`→done; slice-count→resume; pending-request→S2; `g_fatal`→FatalError;
  **any other `UC_ERR_*` (bad guest access, including deliberate guard-page overruns)→the fatal channel**
  with faulting PC/addr. Guest faults arrive as `UC_ERR` (softmmu checks in software, not host SIGSEGV),
  so no host signal handler is required for guest faults; a host-side `SIGSEGV`/`SIGABRT` handler is an
  optional *shim-bug* diagnostic only.

- **S7 (High) — host LP64 ↔ guest LP32 struct/width marshalling is unowned across A06/A08.** The host is
  arm64 bionic (LP64: `time_t/off_t/size_t/pointer`=64, and arm64 `struct stat/tm/statfs/dirent` layouts
  differ from arm32); the guest is LP32. A06 only flags "verify st_size/st_mtime offsets" (line 573).
  **Resolution (cross-cutting invariant):** every struct crossing the boundary (`stat`, `timespec`, `tm`,
  `statfs`, `utsname`, `passwd`, `dirent`) is translated **field-by-field into the guest arm32-bionic
  layout — never a `memcpy` of the host struct**; every `time_t/off_t/clock_t` **narrows 64→32 with a
  clamp** at the boundary (files <2 GiB, dates <2038 assumed — A06 confirms no LFS imported). `jlong`
  stays 64-bit (reg-pair). This is a marshaller responsibility (a "struct descriptor" per type), not a
  per-call afterthought.

- **S8 (Med) — the sandbox root has no defined source; A06 defers to an unknown `nativeInit` arg.** A06
  (line 700) roots the write-sandbox in "the app files-dir handed to `nativeInit`, else a fixed shim
  dir", but the `nativeInit` signature is DEX-declared and unknown (R-J1). **Resolution:** the shim
  resolves the app's writable dir **itself via JNI** (Context `getFilesDir`/`getCacheDir`) at
  `JNI_OnLoad`/first-thunk and sets the sandbox root **before any fs call** — independent of the engine's
  `nativeInit` args. No file op may run before the root is set.

- **S9 (Med, device) — Activity restart without process death re-drives `nativeInit` over a live guest.**
  Android may call `onDestroy→onCreate` in the **same** process; `init_array` ran once at `JNI_OnLoad`
  (A04 X6: no re-init, no `dlclose`) and the guest globals/heap are non-pristine. **Resolution:** keep
  `uc` **process-scoped** (never re-open per Activity); rely on the engine's own Android re-init handling
  (it ships for exactly this). GL context loss on pause/resume (A07 R6) → the engine re-creates GL
  objects via its normal Gen/Create path (the shim forwards them). Device-only risk.

- **S10 (Med) — BEL held across host bulk-callback bridges can starve the audio GEC.** qsort/bsearch hold
  the BEL for the whole sort (N nested comparator calls, S3), an **unbounded-in-array-size** hold that
  violates A02's "bounded BEL hold" and can underrun audio. Bounded in practice (game sorts are small:
  draw-order, leaderboards). **Resolution:** accept for shipping; expose a documented tuning knob — yield
  the BEL between comparisons if on-device audio underruns during large sorts (safe: the array isn't
  concurrently mutated by design).

- **S11 (Low) — one shared guest env sentinel vs per-GEC.** A08 (line 1526) uses ONE shared env sentinel
  for all carriers (the passthrough resolves the carrier's real env); `nativeMixData` derefs it directly,
  so it MUST be a valid guest double-pointer (`env→vtable`). Confirmed sufficient (natives use env on
  their own thread; cross-thread env is re-derived via `get_env→GetEnv`). Ultra-defensive per-GEC
  sentinel optional, not adopted.

---

### THE ONE combined guest address map (non-overlapping; supersedes A05's table with S1's corrections)

| region | base | size | perms | contents / owning audit |
|---|---|---|---|---|
| STUB import trampoline arena | `0x10000000` | `0x20000` | R-X | ≤512 `bx lr` slots for the 351 libc/gl/aeabi **FUNC** imports; `stub_hook` (01/06/07) |
| **JNI trampoline arena** *(S1: mapped in shipping)* | `0x11000000` | `0x10000` | R-X | 233 env-slot + 8 VM-slot `bx lr` trampolines; `jni_hook` (08) |
| GUESTDATA | `0x12000000` | `0x10000` | RW | `_ctype_`(257B)/`_tolower_/_toupper_tab_`, `__sF[0..2]`(3×84B, `_file`=0/1/2), `__stack_chk_guard`(random), **env+VM sentinel P-cells + vtables**, **glGetString cache** (03 B1-1 / 06 / 07 / 08 / S1) |
| ENGINE IMAGE | `0x40000000` | `0xAF0000` (reserve→`0x47FFFFFF`) | per-seg (.text R-X, rodata+RELRO R, data/bss RW) | relocated 32-bit engine (03/04) |
| TCB ARENA (per-GEC) | `0x48000000` | `0x01000000` | RW | 4KiB/GEC: `+0` errno, `+0x40` 256-B TLS, `+0x100` 128-entry key table (05) |
| HEAP (real allocator) | `0x50000000` | `0x08000000` | RW | coalescing allocator w/ inline boundary tags; the live C++ new/delete heap; fopen'd bionic `FILE`s; NULL at exhaustion (05) |
| heap fault-guard | `0x58000000` | `0x08000000` | **unmapped** | overrun → `UC_ERR` → fatal (05 / S6) |
| GUEST-LIB ARENA (dormant) | `0x60000000` | `0x08000000` | — | single-lib decision → unused (03) |
| ASSET ARENA | `0x68000000` | `0x08000000` | RW | `AAsset_getBuffer` guest copies + per-audio-GEC `byte[]` staging (host→guest copy home) (07/08) |
| STACK ARENA (per-GEC) | `0x70000000` | `0x10000000` | RW + unmapped guard frames | main 8 MiB, workers 2 MiB, guard-framed; overrun → fatal (05 M4) |
| RET sentinel | `0xdead0000` | `0x1000` | R-X | `emu_call_v` return trap (∉ exidx `[0x409bf3e4,0x409e2ecc)`) (01/04) |
| KUSER helper page | `0xffff0000` | `0x1000` | R-X | `__kuser_cmpxchg/memory_barrier/get_tls`; `kuser_hook` (02/05) |

**Host-side (NOT guest memory):** the BEL + per-GEC structs; the **slot-descriptor dispatch table**; the
**32↔64 handle table** + `real→token` dedup map; the **fd/FILE descriptor table** + sandbox router; the
AAsset shim table; the sync **shadow tables** (mutex/cond/rwlock/once/join/keys/futex-uaddr); the
libffi cifs / typed-thunk table; GL-string-cache bookkeeping; the client-array **staging VBO** (a real
host GL buffer, inert). **Guest-owned (not a shim region):** every `jmp_buf` (256 B, in the guest stack/
struct). Every region every audit reserves now has exactly one home; no overlap, none unbudgeted.

---

### THE ONE dispatch architecture (unifies 01/06/07's STUB path and 08's JNI-vtable path)

There is **one boundary-crossing mechanism**: the guest `blx`es into an executable arena of `bx lr`
slots; a `UC_HOOK_CODE` fires; a host handler runs; the slot's `bx lr` (or an S2 stop/restart) returns
control. libc/gl (GOT→STUB slot, single-deref) and JNI (`env`/`VM`→vtable slot→arena, double-deref) are
the **same** trap. **One dispatch function** is hooked on `{STUB, JNI-arena, KUSER}` (+ `UC_HOOK_INTR`
for SVC); it classifies by address:

- **Trampoline arena** → index a **slot-descriptor table** `slot[i] = {kind, desc*, handler, aux}`
  resolved **once at load time** (name→descriptor), replacing the per-call `strcmp`/`PRE()` chain
  (abshim.c:155-214) — that chain is both O(n) per import (a per-frame perf tax, couples S-R1) and
  fragile (prefix matching). `kind ∈ {LIBC, GL, AEABI-MEM, JNI-ENV, JNI-VM, SPECIAL}`.
- **KUSER page** → direct architectural emulation (cmpxchg = read-compare-write, atomic under the BEL;
  `get_tls`→current-GEC.tcb+0x40; barrier→no-op). Not descriptor-driven.
- **SVC / `syscall` libc bridge** → the ONE `do_syscall` (S5).

**Two handler execution modes**, chosen by the descriptor's blocking bit:
1. **Non-blocking** (most libc/gl/libm, kuser, marshal-only JNI slots): marshal → real host call → write
   result regs → return; the slot `bx lr` resumes. BEL held throughout; `uc` untouched by others.
2. **Blocking / reentrant** (JNI `Call*Method`, `nanosleep`, futex, mutex/cond/join waits): the S2
   stop/restart continuation — hook sets pending-request + return-PC=LR, `uc_context_save`, `uc_emu_stop`;
   carrier loop releases BEL, performs it, reacquires, writes result, restores, re-issues at LR.
Non-blocking **leaf** guest callbacks (qsort/bsearch comparator) are the sole users of **nested
`emu_call_v`** (S3), under the held BEL.

---

### THE ONE descriptor format (shared static tables + dynamic JNI-sig instances)

```
struct arg { u8 atom; u8 dir; u8 lensrc; u8 lenarg; };   // 4 bytes
struct desc {
  u8  ret;            // VOID|I32|U32|PTR|I64|F32|F64  (>4B AGG hidden-ptr reserved)
  u8  nargs;
  u8  kind;           // LIBC|GL|AEABI|JNI_ENV|JNI_VM|SPECIAL
  u8  flags;          // bit0 = blocking (mode-2), bit1 = variadic tail present
  u16 handler;        // handler index / gl-symbol index
  struct arg args[MAXARGS];
};
```
- **atom** = the A06 key superset: `I32 U32 PTR STR SIZE OFF I64 F32 F64 VA VARIADIC`. A07's GL "pointer"
  offsets are atom **U32** (`‹off›` = integer, never copied); jstring/jarray/jobject in JNI are **PTR**
  resolved token↔real.
- **dir** ∈ `{NONE, IN, OUT, INOUT, STR, OPAQUE}`; **lensrc** ∈ `{NONE, CONST(lenarg), ARG(lenarg),
  COMPUTED(id)}` where COMPUTED ids are the few GL size formulas (`LEN_TEXIMAGE = rows×align(w×bpp,
  UNPACK_ALIGNMENT)`, `LEN_READPIXELS`, `LEN_COMPRESSED`, …) kept in one place. `S7` adds struct-typed
  atoms (`STAT, TIMESPEC, TM, …`) whose marshaller translates arm64→arm32 bionic layout field-by-field.
- **ret**: `VOID`→none; `I32/U32/PTR`→r0 (sub-word sign/zero-extended); `I64`→r0:r1; `F32`→r0 bits;
  `F64`→r0:r1 bits (A01 return-writer — always writes both regs).
Static instances: libc(227), gl(68), aeabi-mem(10), jni-env(233 slots), jni-vm(8). **Dynamic** instances:
each JNI method `sig` parsed at `GetMethodID`/`GetStaticMethodID` into the *same* atom/ret vocabulary,
stored in the handle-table id-entry (A08 Det.2) — the only place JNI arg types exist.

### THE ONE marshaller (four directions, two variadic specializations, two host emitters)

Two guest primitives over a mode-agnostic **guest-mem/regfile ops** interface (`read(dst,gaddr,n)`,
`write(gaddr,src,n)`, `reg_get/set`): a **read-walker** (guest regs/stack/va_list → typed `GArg[]`) and a
**write-walker** (typed `GArg[]` → guest r0–r3 + 8-aligned stack). The A01 two-counter machine (NCRN/NSAA,
8-align 64-bit, float=1 core slot, low-word-first) is the single algorithm; `d_in/d_out` (r0=low) is the
correct nucleus, generalized to arbitrary aligned slots + f32. Four flows, all one descriptor + these
primitives:
1. guest import → host: read-walk → **host-AAPCS64 emitter** (libffi cif / typed-thunk) → real call.
2. host result → guest: read host x0/v0 → **return-writer**.
3. real JVM → guest thunk (72): AAPCS64 x0–x7/v0–v7 (two sequences, Java order) → write-walk (soft-float
   merge, jfloat bitcast to core, jlong/jdouble 8-aligned) → `emu_call_v`.
4. guest thunk return → real: read-walk r0[:r1] → AAPCS64 return.
**Two variadic specializations of the SAME walk** (default promotions float→double, sub-int→int, then
8-align 64-bit): **printf/scanf** (source A = trap-time regs+stack, or source B = guest `va_list`) →
sink; **JNI `Call*MethodV`** (source = guest `va_list`) → **jvalue[] emitter** (the *second* host emitter;
`…A` form is 8-aligned, no host va_list synthesis). One walk, two emitters (AAPCS64 regs vs `jvalue[]`).

---

### THE ONE float / soft-float story (all audits agree)

In-guest, FP is computed by a **mix** of inline VFP (`vldr/vcvt/vmov/vldm`) **and** statically-linked
integer soft-float `__aeabi_*` helpers (§12) — **both live in-guest**; the only `__aeabi_*` that crosses
is the block-memory set (A06). Therefore **CPACR + FPEXC MUST be enabled** (abshim.c:112-114) — "soft
ABI" ≠ "no VFP". At the boundary, FP crosses **only** as **core-register soft-float**: `float` = 1 core
reg bit-pattern (never a d/s reg), `double` = reg-pair (r0:r1 or r2:r3, 8-aligned) — for the UND libm
set, printf/scanf `%f`, GL float args (`glClearColor/glUniform*f`), and JNI `jfloat/jdouble`. The
core→VFP re-widening for the **hardfloat AAPCS64 host call's v-registers** happens **only** at the real
host-call boundary (emitter); the reverse (host d0→guest core) at the return. **S4:** the callee-saved
VFP (d8–d15) + FPSCR ride the full `uc_context` across every save/restore.

---

### THE ONE threading / execution model (BEL hand-off among fixed carriers; no central scheduler)

- **One `uc`, touched only under the BEL** (one non-recursive **PI-mutex** — priority-inheritance boosts
  whoever holds it for the real-time audio GEC). Kills C1.
- **GECs are pinned to carrier threads, never migrated.** Each guest thread = one host carrier: the
  render GEC on the Java **GLThread**, the audio GEC on the audio thread, workers on their
  `pthread_create` carriers, init/UI on their calling threads. "Time-slicing" (A02) is **BEL fairness**,
  not GEC migration: each carrier loops `acquire BEL → uc_emu_start(GEC, count=SLICE) → uc_context_save →
  release BEL`. This reconciles A02 (slicing) with A07/A08 (per-carrier affinity) — the apparent tension
  is resolved: there is no work-stealing pool.
- **One stop/restart continuation engine (S2)** dispatched by the carrier loop on the `uc_emu_start`
  stop-reason: RET→done · slice→resume · pending-block→(release BEL, do op, reacquire, resume@LR) ·
  pending-longjmp→resume@target · `g_fatal`→FatalError · other `UC_ERR`→fatal (S6). The **same engine**
  serves slicing (A02), longjmp (A04), blocking waits (A02/06), and JNI Call BEL-release (A08).
- **Non-blocking leaf callbacks** (qsort/bsearch comparator) = nested `emu_call_v` under held BEL (S3).
  **`pthread_once` init and all blocking/reentrant call-outs** = stop/restart (never nested).
- **Render GEC never blocks while holding the GL context:** GL forwards keep the BEL (quick) except
  `glFinish`/`glReadPixels` which release it during the GPU drain (A07); a guest JNI Call inside
  `nativeUpdate` uses stop/restart (releases BEL, unwinds `uc_emu_start`), so re-entrant Java-on-GLThread
  runs a fresh top-level — **no self-deadlock** (the BEL is released before every call-out; a
  non-recursive mutex suffices).
- **Real guest sync** (BEL doesn't subsume it — slices preempt mid-section): shadow tables keyed by guest
  addr (mutex owner+count / cond / rwlock / once / join→ZOMBIE-retval / keys+destructors / futex-uaddr).
  Atomics = ldrex/strex + `__kuser_cmpxchg` as guest instructions under the BEL (single-owner ⇒ correct;
  cleared monitor across a slice = legal spurious strex fail). `pthread_self/gettid` = GEC.guest_tid.
- **Thread exit** runs key destructors → ZOMBIE → wakes joiners; a guest worker that attached to the JVM
  (`AttachCurrentThread`) **`DetachCurrentThread` at ZOMBIE** (A08 — else a JVM thread leak).

---

### THE ONE fatal policy (single sink; fatal vs guest-recoverable is explicit)

**FATAL** → `g_fatal`(+reason) → `uc_emu_stop` → propagate up every `emu_call_v` return (no bogus value)
→ top-level thunk calls **`(*env)->FatalError(env,reason)`** (process death). Members: guest `abort` /
`__stack_chk_fail` / `std::terminate` / `__cxa_call_unexpected` / `__cxa_pure_virtual` (04); an uncaught
guest C++ exception reaching the `emu_call_v` boundary → `_URC_FAILURE` (04 X5); guest `exit/_exit/
raise(SIGABRT|SEGV|ILL|FPE|BUS|KILL)` (06 L18); JNI `FatalError(18)` (08); **any `UC_ERR_*` fault
including the A05 guard-page overruns (S6)**; a `gl*` on a non-render GEC (07 G-INV1); and every
**build-drift tripwire** — `RegisterNatives`/`GetDirectBufferAddress`/`MonitorEnter`/weak-ref/Critical/
UTF-16-chars slot (08 J13), the GL client-array guard (07), an unknown reloc type (03), a per-activation
local-token overflow (08). A tripwire firing means our model is wrong → fail loud, never limp.

**GUEST-RECOVERABLE** (return the error the engine expects; **never** fatal, **never** a fault):
`malloc/new`→**NULL on exhaustion** (→ new_handler / `bad_alloc` / 512-B emergency pool — A05 M9; balloc
MUST return 0, not bump); network facade →`−1`+errno (06 L16); file-not-found →`−1`/NULL+errno, `fopen`
fail→NULL (the engine throws+catches its own `strerror` exception); AAsset arena full →`getBuffer`→0;
`FindClass`/`GetMethodID`/`GetFieldID`→NULL + real Java pending exception (engine's `ExceptionCheck`
handles it). **Distinguishing rule:** if the guest's own invariants remain coherent and it has a defined
recovery path → recoverable; if they are already violated (smashed canary, terminate, uncaught throw,
bad access, "can't-happen" slot) → fatal. The current `abort→S0(0)` (194), `exit→0`, and `uc-fault→
return r0` (255) are all the launder bug this policy forbids. **Prerequisite (A04):** `__stack_chk_guard`
must be a real, consistent guest datum (03 B1-1) **before** any stack-protected guest code, or
`__stack_chk_fail` fires spuriously.

---

### THE ONE canonical bring-up order (folded from all audits; dependency-checked)

1. `uc_open(UC_ARCH_ARM, UC_MODE_ARM)`.
2. Map **all** regions (combined map above); fixed-perm regions get final perms now.
3. **Populate GUESTDATA**: ctype/tolower/toupper tables, `__sF[0..2]` (`_file`=0/1/2), a **random
   `__stack_chk_guard`** — *before* any guest code (04/06; prereq for the fatal channel not to misfire).
4. **Load engine**: read DT_* from the **mapped image** (03 B1-7, robust to `p_offset≠p_vaddr`); apply
   relocs preserving Thumb bits — FUNC-UND→stub slot, **OBJECT-UND→GUESTDATA mirror** (03 B1-1),
   **weak-UND→0 only for `{__gnu_Unwind_Find_exidx, dl_unwind_find_exidx, __google_potentially_blocking_
   region_*}`** else identical to strong-UND (04 X8); unknown reloc type → hard error.
5. **Perms/RELRO**: `.text` R-X, RELRO span R, data/bss RW — *after* relocs (relocs write the GOT).
6. **Enable VFP** (CPACR + FPEXC) — *before* any emulation (init_array uses VFP).
7. **Install hooks + build the slot-descriptor table** (resolve every import name→descriptor now, O(1)
   dispatch): `stub_hook`, `jni_hook`, `kuser_hook`, `intr_hook`.
8. **Initialize the real allocator** over the HEAP arena — *before* init_array (ctors malloc: A05
   measured 7860 mallocs across init).
9. **BEL + main GEC** (TCB page, errno, TLS, key table, 8-MiB guard-framed stack; current-carrier→main).
10. **Build the guest env/VM sentinels** (S1): 233+8 trampolines in the JNI arena; P-cells+vtables in
    GUESTDATA; wire JNI slot descriptors.
11. **Arm the fatal channel** (`g_fatal`=0; FatalError path via the carrier's real env) — *before* the
    first `emu_call`.
12. **Run `init_array`** ascending via `emu_call_v` under BEL (allocator live, EH's static exidx fallback
    working since relocs done, fatal armed). Then **`emu_call` the guest `JNI_OnLoad(guest_VM_sentinel)`**
    (its `set_vm` caches the guest VM — A08 Det.1/7), then the real `JNI_OnLoad` returns
    `JNI_VERSION_1_6`. (Order matches native: ctors before the guest's own `JNI_OnLoad`.)

Every audit's ordering assumption is satisfied: allocator<init_array (05), GUESTDATA ctype/canary<any use
(06/04), fatal<first emu_call (04), BEL/GEC<init_array (02), VM sentinel<guest JNI_OnLoad (08), RELRO after
reloc / DT_* from mapped image (03).

---

### Weak-UND rule — confirmed internally consistent (post-X8)

B1-4 (lines 181-187) is explicitly "CORRECTED by Audit 04/X8"; A03's canonical rule (lines 202-203) defers
to "the small `→0` whitelist (Audit 04/X8 refined rule)"; A04's refined rule (lines 302-310) is the single
authority. No lingering blanket "weak→0" remains in the doc. **The `→0` set is exactly 4 symbols**
(`__gnu_Unwind_Find_exidx`, `dl_unwind_find_exidx`, the two `__google_potentially_blocking_region_*`); the
7 weak `pthread_*` + `getauxval` get **real bridges** (nonzero) — `pthread_create` is
`__cxa_guard_acquire`'s `__gthread_active_p` sentinel. **Code gap (not a spec gap):** `apply_rel`
(abshim.c:76-84) still sends *all* UND to nonzero stubs — it MUST branch on `STB_WEAK && SHN_UNDEF` against
this 4-symbol whitelist → 0 (the live X1 bug: `__gnu_Unwind_Find_exidx`→stub defeats the static exidx
fallback → every C++ throw aborts).

---

### KILL the dual-mode — ONE production path

The shipping **arm64** build is the ONLY production path; **all `#ifdef SHIM_TEST` is removed from the
production translation units** (no arm64 Android runtime exists on the x86 dev host — project-established;
the SHIM_TEST fakes — `jni_hook`/`jni_setup` `0x11000000` fabricated handles, printf, `fopen`-from-
`/work/work803/assets`, the AAsset `fopen` fakes, `main()` — neither match the device nor belong on the
ship path). Host-side unit testing of **pure logic** is retained in **separate test TUs** that link a
**mode-agnostic core**, never as `#ifdef`s inside it:

- **core (host C, no uc/JNI/GL/Android — unit-testable):** `alloc.c` (A05 allocator over a flat arena via
  the mem-ops interface), `marshal.c` (A01 walkers + the mem/regfile ops), `descriptors.c` (the static
  tables), `format.c` (A06 printf/scanf), `utf.c` (modified-UTF-8 + UTF-8 MB/WC codecs), `handle_table.c`
  (A08 tokens), `fdtable.c` (A06 fd/FILE + sandbox router policy), `elf32.c` (A03 reloc/DT_*/init
  enumeration), `ctype_tables.c` (bionic table bytes). The marshaller/allocator/formatter take **block
  read/write + reg accessor callbacks** — in production backed by `uc_mem_*`/`uc_reg_*`, in test by a flat
  buffer + a regfile array. This decouple is what lets the SHIM_TEST scaffolding be deleted rather than
  ported (it replaces `em_str/em_copy/em_set/Rg/S0/d_in/d_out`).
- **test TUs (host `make test`, never shipped):** differential printf vs system libc; allocator torture;
  UTF-8 round-trip; marshaller placement checks (the A01 worked cases: glTexImage2D(9), lseek64, pow,
  `sprintf("%d %f")`); handle-table lifetime.
- **device layer (host C, links uc+JNI+GLESv2+Android — the ONLY arm64/Android TU, zero test code):**
  `cpu.c`, `sched.c` (BEL/GEC/stop-restart/sync/futex), `dispatch.c` (the one hook + kuser + `do_syscall`),
  `bridge_libc.c`, `bridge_gl.c`, `bridge_asset.c`, `jni_passthrough.c` (env/VM sentinels + slots + audio
  copy-back), `jni_entry.c` (the generic thunk + 72 exports + `JNI_OnLoad` bring-up), `eh.c` (setjmp/
  longjmp + fatal channel + canary).

### Module → audit → guest/host

| module | audit(s) | guest code/mem vs host C |
|---|---|---|
| `regions.h`/`cpu.c` | 02/03/05 | maps guest regions; host C |
| `alloc.c` | 05 | metadata **in** guest heap (boundary tags); allocator **code** host |
| `marshal.c`+`descriptors.c` | 01/06/07/08 | host C over guest mem |
| `format.c`/`utf.c`/`ctype_tables.c` | 06/08 | host C; tables copied into GUESTDATA |
| `elf32.c` | 03 | host C; writes the guest image/relocs |
| `sched.c` | 02 (04/06/08) | host C; GECs/stacks/TCB are guest mem |
| `dispatch.c` | 01/02/04/06/07/08 | trampoline arena = **guest** `bx lr`; hook = host |
| `bridge_libc.c`/`fdtable.c` | 06 | host C; guest `FILE`/`__sF` in guest mem |
| `bridge_gl.c`/`bridge_asset.c` | 07 | host C; glGetString cache + AAsset copies = guest mem |
| `jni_passthrough.c` | 08 | sentinels/vtables = **guest** mem; handle table = host |
| `jni_entry.c` | 01/08/04 | 72 `Java_*` = **guest** engine addrs; thunk = host arm64 |
| `eh.c` | 04 | `jmp_buf` = guest mem; g_fatal/canary logic = host |

---

### Master invariant list (deduplicated across 01–08 + S-series)

**Marshalling/FP.** (M1) Every value crossing the boundary goes through the one read/write-walker driven
by a descriptor; no handler reads `Rg()` or writes a bare `S0()`. (M2) FP crosses **only** as
core-register soft-float; all `__aeabi_*` arithmetic and inline VFP stay in-guest; CPACR+FPEXC enabled.
(M3) Every 64-bit/double/float return writes the exact reg(s) (i64/off64→r0:r1 always both; f32→r0 bits;
f64→r0:r1 bits). (M4) Variadic pulls apply float→double + sub-int→int promotion then 8-align 64-bit;
printf source-A/B and JNI `va_list` are the same walk. (M5, S4) The state-preservation contract is the
full `uc_context` incl. d8–d15/FPSCR. (M6, S7) Structs translate arm64→arm32-bionic field-by-field;
time_t/off_t narrow 64→32 with clamp. (M7) `__aeabi_memset` is `(dest,n,c)`; `memmove` overlap-correct.

**Memory.** (M8) Every malloc/new result is a live ≥16-aligned in-arena chunk with a valid header;
free/delete reclaims it; realloc copies `min(old,n)`; calloc overflow-checks. (M9) Heap never bumps past
`0x58000000`; exhaustion → NULL (engages emergency pool), never a fault. (M10) errno & TLS per-GEC (TCB
page), never in the heap. (M11) Each guest thread owns a guard-framed stack; overruns fault
deterministically → fatal. (M12) `type_info`/static `.rodata`/`.data.rel.ro` never relocated by the
allocator. (M13) mmap returns a real file-backed guest mapping or `(void*)-1`, never 0.

**Dispatch/threading.** (M14) One `uc`, touched only under the BEL. (M15, S2/S6) Blocking/reentrant
call-outs use stop/restart (never block inside a hook); every `uc_emu_start` return is classified, and
any unexpected `UC_ERR`→fatal. (M16) GECs pinned to carriers; `gl*` only on the render GEC/GLThread (else
fatal); audio GEC = priority + small slice. (M17) Bridge atomicity: a hook runs within one guest-instr
handling; the allocator/handle/fd/sync tables mutate under the BEL, no inner lock. (M18) Blocking guest
waits and `nanosleep`/`glFinish`/`glReadPixels`/JNI Call release the BEL; quick host calls keep it.

**Loader/EH.** (M19) Read DT_* from the mapped image; unknown reloc type → hard error; RELRO after reloc;
Thumb bits preserved. (M20) OBJECT-UND → GUESTDATA mirror; weak-UND→0 only for the 4-symbol whitelist,
else strong-UND rules. (M21) EH stays in-guest via the proven `__exidx_start/end` fallback
(`__gnu_Unwind_Find_exidx`→0); `__cxa_get_globals` backed by real per-GEC TLS; the guard futex routes to a
real block. (M22) One fatal channel; the fatal-vs-recoverable table above is authoritative; no launder.

**JNI/GL/asset.** (M23) Every ref/ID crosses only as a kind-tagged 32-bit token; token 0⇔NULL; real ref
freed exactly when its token is freed. (M24) `JNIEnv*` per-carrier; one shared guest env/VM sentinel; VM
cached once at load. (M25) Java calls are `Call*MethodV` → walk guest `va_list` per the GetMethodID-captured
sig → `jvalue[]` → real `…A`; return by class. (M26) Local tokens activation-scoped (per-GEC frame stack);
globals persist; cached jclass/jmethodID ride real globals. (M27) Exceptions forward verbatim;
`ExceptionCheck` returns the TRUE state; Java pending exceptions ≠ guest C++ exceptions. (M28) All GL IDs
originate from the real driver; draws are VBO-offset passthrough (never copied); every IN/OUT pointer
copied at the exact computed size, NULL data ⇒ no copy; `glGetString`/`AAsset_getBuffer`/`GetStringUTFChars`/
audio `byte[]` return **guest** copies; `AAsset_getLength64` writes r0:r1; audio writes back at
`ReleaseByteArrayElements(mode=0)`. (M29) No EGL bridge; no real network socket ever; assets read-only.

---

### Residual on-device-only risks (the honest "only the phone validates this" list — read BEFORE install)

From A07/A08 + synthesis. **Ordered by how likely they are to break first play:**

- **SR1 (existential) — whole-pipeline per-frame performance.** Every `nativeUpdate` runs the entire
  Box2D sim + render as **TCG-interpreted ARM32** under one BEL on the A56, then real GL. 60 fps ⇒ ~16 ms
  of emulated ARM/frame. Emulation throughput is un-boundable by construction; jank/slow-motion is the
  most probable first symptom. Single-threaded emulation (one `uc`) serializes all guest threads.
  **MEASURED (2026-07-28) — this prediction was right.** The frame-time split
  (`port/validation/emu_perf_split.sh`, release configuration, real gameplay) puts **73–75 % of frame
  time in the emulator**, 16–18 % in the native bridges, and only 5–7 % outside the shim. Other docs
  had assumed software rasterisation was the dominant cost; it is not, and a real GPU on the A56
  replaces only that 5–7 % slice. SR1 is therefore not merely still open — it is the *confirmed*
  determinant of on-device frame rate, bounded by the phone's CPU single-thread performance rather
  than by anything this port can optimise away. See `port/OPEN_FINDINGS.md` R4, which also records
  the one quantified bridge optimisation (un-bridging `floor`, ~3–4 %) declined as a physics risk.
- **SR2 — ANR / watchdog.** A `nativeUpdate` or first `nativeInit`/asset-load exceeding ~5 s under
  emulation, or a main-thread thunk starving on the BEL, trips Android's ANR killer.
- **SR3 — audio latency/underrun under BEL scheduling (S10).** `nativeMixData` under the BEL; a long game
  frame/sort starves the audio GEC. PI-mutex + small audio slice mitigate but are unvalidated.
- **SR4 — the emulator-semantics dependency (S2/S3/S4).** Nested `uc_emu_start` (comparators) and
  `uc_emu_stop`-from-hook then re-issue-at-PC (slicing/longjmp/blocking) rest on Unicorn 2.1.x behavior no
  x86 test fully exercises. The one hard "must smoke-test on device / arm64 Unicorn first" item.
- **SR5 — DEX-declared native signatures (R-J1) and the sandbox-dir source (S8).** The 72 thunk descriptors
  and method/field sigs are authoritative in `classes.dex` (unread here); a mis-typed float/`jlong` mis-
  marshals. The writable-dir must be JNI-resolved (S8).
- **SR6 — GL driver/GLSL on Samsung Xclipse 540 (R2/R3/R1).** ETC1/NPOT/precision acceptance, GLSL frozen
  for 2013-era Adreno on a stricter ES2 validator (→ black screen), and no `glGetError` polling ⇒ driver
  errors are silent. Content-side, device-only.
- **SR7 — pixel-exactness, context loss on pause/resume, Activity restart (R4/R6/S9).** Blend/depth/dither
  deltas; GL object survival across `onPause` (Java `setPreserveEGLContextOnPause`); same-process
  `onDestroy→onCreate` re-driving `nativeInit`.
- **SR8 — memory/JVM accounting under a long session (R7/R-J3/R-J4).** Asset-copy doubling (host mmap +
  guest copy) in the ASSET arena; JVM local-ref capacity if engine hygiene regresses; thread attach/detach
  balance. Bounded by design; device-only under long play.
- **SR9 — modified-UTF-8 supplementary/surrogate + embedded-NUL edges (R-J2).** Correct by construction
  (byte-copy + guest sizing) **and directly unit-tested**: `test_utf.c` (30 cases) rejects encoding a
  surrogate as a scalar value and anything above U+10FFFF, decodes U+10348, and checks that mUTF-8
  encodes each surrogate half separately as 3 bytes.
  This line used to end "only device zh/ja strings exercise the surrogate path", which names the
  wrong trigger: ordinary Chinese and Japanese text is BMP — 3-byte UTF-8 — and needs no surrogate
  pair at all. Surrogate pairs arise only for U+10000 and above. Measured against the shipped data:
  the 16 `assets/data/localization/TEXTS_*.dat` files contain **38 921** three-byte sequences and
  **zero** four-byte ones, so no supplementary-plane character exists in the game's own text and the
  surrogate path is not reachable from assets on **any** locale. It is covered by the unit tests
  rather than by a playthrough, which is the honest place to rest that claim.

---

### Cross-layer gaps the eight audits did not own (now assigned)

- **G-A (owned → S6):** a Unicorn `UC_ERR` fault (including the A05 guard pages) had no sink — now the
  fatal channel. Guest faults are `UC_ERR`, not host signals; a host signal handler is optional
  shim-bug diagnostics only.
- **G-B (owned → S5):** the `syscall` libc function + SVC futex/gettid/clock_gettime had no unified
  handler — now one `do_syscall`.
- **G-C (owned → S7):** LP64↔LP32 struct/time_t/off_t translation had no owner — now a marshaller struct
  descriptor + narrowing rule.
- **G-D (owned → S8):** the writable sandbox root source — JNI-resolved, not from the unknown `nativeInit`
  arg.
- **G-E (owned → S9):** app lifecycle / Activity restart / GL context loss — `uc` process-scoped, engine
  re-init relied upon.
- **G-F (noted):** static-init vs JNI_OnLoad ordering is safe — `set_vm` has exactly one caller
  (`JNI_OnLoad`, A08), so the VM is not cached in a static ctor; init_array (step 12) runs after all
  bridges (step 7) are installed.
- **G-G (noted):** the `0xdead0000` RET sentinel is a latent alias hazard only if the guest ever branches
  to that exact address as non-return (astronomically unlikely; it is deliberately outside all mapped
  engine code and every exidx range).

### One-line verdict

The eight layers compose into one coherent single-path design once **S1** (map the JNI arena in shipping),
**S2** (blocking = stop/restart, never block in a hook), **S3** (confine nested `uc_emu_start` to leaf
comparators; verify on device), and **S6** (fault→fatal) are adopted; the remaining seams are width/
placement/tuning. Correctness is achievable; the residual risks are all **throughput and driver/DEX
facts** that only the A56 can settle — none are architectural.
