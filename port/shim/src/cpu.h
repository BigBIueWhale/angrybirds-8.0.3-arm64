/* cpu.h — the Unicorn ARM32 CPU + the mem-ops binding (device layer, Audit 02/05).
 * Maps the combined address space, enables VFP, fills the bx-lr trampoline arenas
 * and the kuser page, and creates the real allocator over the heap arena. The
 * `guest_mem` here is backed by the uc_mem and uc_reg calls -- this is what lets
 * the whole host-tested core run over real emulated guest memory. */
#ifndef ABSHIM_CPU_H
#define ABSHIM_CPU_H
#include <unicorn/unicorn.h>
#include "memops.h"
#include "galloc.h"

typedef struct cpu {
    uc_engine *uc;
    guest_mem  mem;      /* backed by uc */
    galloc    *heap;       /* over RG_HEAP  — the game's C++/malloc heap */
    galloc    *asset_heap;  /* over RG_ASSET — AAsset_getBuffer copies, isolated so large/churning
                              asset buffers don't fragment or exhaust the game heap */
} cpu_t;

int  cpu_create(cpu_t *c);
void cpu_destroy(cpu_t *c);

/* Convenience: run a guest function to completion (until it returns to RG_RET),
 * with args in r0..r3, a fresh SP, LR=RG_RET. Returns uc_err. addr LSB selects
 * ARM/Thumb (unicorn 2.x). */
/* Chunk-head guest-write watchpoint (non-release diagnostic; see cpu.c). cpu_hw_find returns 1
 * if some guest instruction wrote a range covering `want`, with the writing PC/LR. */
#ifndef ABSHIM_RELEASE
int           cpu_hw_find(uint32_t want, uint32_t *addr, uint32_t *val, uint32_t *pc, uint32_t *lr);
unsigned long cpu_hw_count(void);
#endif

uc_err cpu_call(cpu_t *c, uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint64_t insn_budget);

/* Run the guest from `start` until it returns to RG_RET, honouring a pending
 * longjmp (Audit 09 S2 stop/restart: a bridge sets the target + calls uc_emu_stop;
 * this loop restarts emulation at the target). Used by cpu_call and shim_call. */
uc_err cpu_run(cpu_t *c, uint32_t start, uint64_t insn_budget);

/* A setjmp/longjmp (or blocking) bridge calls this to request that cpu_run
 * resume at `target` (the guest regs have already been restored). Per-thread. */
void   cpu_set_longjmp(uint32_t target);

#endif /* ABSHIM_CPU_H */
