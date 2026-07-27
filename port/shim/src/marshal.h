/* marshal.h — the ONE arg-walker + return-writer (Audit 01 backbone).
 *
 * Implements the AAPCS32 base (soft-float) argument-placement state machine over
 * the guest register file + stack, driven by descriptors.h. Two primitives
 * (pull_word / pull_dword) run the NCRN/NSAA machine; read_args walks a full
 * fixed signature; the printf/scanf and JNI-va_list engines resume the SAME
 * cursor to pull the variadic tail (Audit 09: "two variadic specializations of
 * the same walk"). return-writer places r0 / r0:r1 exactly (Audit 01 D2 fix:
 * every 64-bit/double return writes BOTH regs). Guest-import direction only;
 * the AAPCS64->guest thunk entry side (JNI) is a separate walker (Phase D). */
#ifndef ABSHIM_MARSHAL_H
#define ABSHIM_MARSHAL_H
#include "memops.h"
#include "descriptors.h"

/* A marshalled argument: the raw value (32-bit zero-extended, or 64-bit) plus
 * its atom so the bridge knows how to interpret it (pointer resolve, float
 * re-widen, etc.). For A_PTR/A_STR, v is the guest address. */
typedef struct { uint8_t atom; uint64_t v; } garg;

/* AAPCS32 placement cursor: NCRN (next core reg 0..4) + NSAA (next stacked
 * argument guest address). */
typedef struct { int ncrn; uint32_t nsaa; } mcur;

/* Initialise a cursor for a guest-import call: NCRN=0, NSAA=SP (r13). */
void     marshal_cur_init (guest_mem *m, mcur *c);

/* Pull one 32-bit slot (I32/U32/PTR/STR/SIZE/OFF/F32/VA). */
uint32_t marshal_pull_word (guest_mem *m, mcur *c);

/* Pull one 8-byte slot (I64/U64/F64): rounds NCRN up to even, takes an even
 * core-reg pair (r0:r1 or r2:r3) or, once core regs are exhausted, an
 * 8-aligned stack slot. Returns lo | (hi<<32). */
uint64_t marshal_pull_dword(guest_mem *m, mcur *c);

/* Walk a full fixed signature into out[0..d->nargs). */
int      marshal_read_args (guest_mem *m, const mdesc *d, garg out[]);

/* Write a return value of atom `ret` (value bits in v): r0, or r0:r1 for 64-bit
 * (I64/U64/F64); nothing for VOID. Sub-word extension of I32 returns is the
 * handler's responsibility before calling this. */
void     marshal_write_ret (guest_mem *m, uint8_t ret, uint64_t v);

/* Entry-side placement (the inverse of pull): write args into the guest regs/
 * stack per AAPCS32 as the JNI thunks do (Audit 08 Det.4 / Audit 09 flow 3).
 * The cursor's nsaa must point at the guest SP the callee will see. */
void     marshal_place_word (guest_mem *m, mcur *c, uint32_t v);
void     marshal_place_dword(guest_mem *m, mcur *c, uint64_t v);

#endif /* ABSHIM_MARSHAL_H */
