/* memops.h — the ONE guest-memory / regfile interface (Audit 09 "mem-ops").
 *
 * Every module in the mode-agnostic core reaches guest memory and guest
 * registers ONLY through this vtable. In production it is backed by
 * uc_mem_read/write + uc_reg_read/write; in host unit tests by a flat buffer +
 * a register array. This decoupling is what lets the SHIM_TEST scaffolding be
 * DELETED rather than #ifdef'd (Audit 09, "kill the dual-mode"): the allocator,
 * marshaller, formatter, UTF codec and handle table are pure logic over this
 * interface and are validated on x86 with no arm64 runtime.
 *
 * All guest addresses are 32-bit (the guest is LP32). Little-endian both sides. */
#ifndef ABSHIM_MEMOPS_H
#define ABSHIM_MEMOPS_H
#include <stdint.h>
#include <string.h>

typedef struct guest_mem {
    /* copy n bytes guest[gaddr..gaddr+n) -> host dst */
    void     (*read )(struct guest_mem *m, void *dst, uint32_t gaddr, uint32_t n);
    /* copy n bytes host src -> guest[gaddr..gaddr+n) */
    void     (*write)(struct guest_mem *m, uint32_t gaddr, const void *src, uint32_t n);
    /* guest core register i (r0..r15) */
    uint32_t (*reg_get)(struct guest_mem *m, int i);
    void     (*reg_set)(struct guest_mem *m, int i, uint32_t v);
    void     *ctx;   /* backend state (uc_engine* in prod, buffer in tests) */
} guest_mem;

/* Width helpers — the only vocabulary the core needs on top of read/write. */
static inline uint32_t gm_rd32(guest_mem *m, uint32_t a){ uint32_t v; m->read(m,&v,a,4); return v; }
static inline void     gm_wr32(guest_mem *m, uint32_t a, uint32_t v){ m->write(m,a,&v,4); }
static inline uint16_t gm_rd16(guest_mem *m, uint32_t a){ uint16_t v; m->read(m,&v,a,2); return v; }
static inline void     gm_wr16(guest_mem *m, uint32_t a, uint16_t v){ m->write(m,a,&v,2); }
static inline uint8_t  gm_rd8 (guest_mem *m, uint32_t a){ uint8_t  v; m->read(m,&v,a,1); return v; }
static inline void     gm_wr8 (guest_mem *m, uint32_t a, uint8_t  v){ m->write(m,a,&v,1); }

#endif /* ABSHIM_MEMOPS_H */
