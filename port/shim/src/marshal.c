/* marshal.c — AAPCS32 base (soft-float) arg-walker + return-writer. See marshal.h.
 *
 * Placement rules (AAPCS §5.5, base variant — FP in core registers):
 *   - a 4-byte argument takes the next core register r[NCRN] (NCRN<4), else the
 *     next 4-aligned stack slot;
 *   - an 8-byte argument (long long / double) is 8-byte aligned: NCRN is rounded
 *     up to an even register; it then occupies an even core-reg pair (r0:r1 or
 *     r2:r3) if one remains, otherwise an 8-aligned stack slot (NCRN clamps to 4,
 *     "once on the stack, stay on the stack"). Fundamental 8-byte types never
 *     split across r3/stack.
 * NCRN is never back-filled. */
#include "marshal.h"

#define REG_SP 13

void marshal_cur_init(guest_mem *m, mcur *c){
    c->ncrn = 0;
    c->nsaa = m->reg_get(m, REG_SP);   /* stacked args begin at SP on callee entry */
}

uint32_t marshal_pull_word(guest_mem *m, mcur *c){
    uint32_t v;
    if (c->ncrn < 4){ v = m->reg_get(m, c->ncrn); c->ncrn++; }
    else            { v = gm_rd32(m, c->nsaa);    c->nsaa += 4; }
    return v;
}

uint64_t marshal_pull_dword(guest_mem *m, mcur *c){
    if (c->ncrn & 1) c->ncrn++;                 /* round up to even */
    uint32_t lo, hi;
    if (c->ncrn <= 2){                          /* fits in r0:r1 or r2:r3 */
        lo = m->reg_get(m, c->ncrn);
        hi = m->reg_get(m, c->ncrn + 1);
        c->ncrn += 2;
    } else {                                    /* spill to 8-aligned stack */
        c->ncrn = 4;
        c->nsaa = (c->nsaa + 7u) & ~7u;
        lo = gm_rd32(m, c->nsaa);
        hi = gm_rd32(m, c->nsaa + 4);
        c->nsaa += 8;
    }
    return (uint64_t)lo | ((uint64_t)hi << 32);
}

int marshal_read_args(guest_mem *m, const mdesc *d, garg out[]){
    mcur c; marshal_cur_init(m, &c);
    for (int i = 0; i < d->nargs; i++){
        out[i].atom = d->args[i].atom;
        out[i].v = matom_is64(d->args[i].atom) ? marshal_pull_dword(m, &c)
                                               : marshal_pull_word (m, &c);
    }
    return d->nargs;
}

void marshal_write_ret(guest_mem *m, uint8_t ret, uint64_t v){
    if (ret == A_VOID) return;
    m->reg_set(m, 0, (uint32_t)v);
    if (matom_is64(ret)) m->reg_set(m, 1, (uint32_t)(v >> 32));
}

void marshal_place_word(guest_mem *m, mcur *c, uint32_t v){
    if (c->ncrn < 4){ m->reg_set(m, c->ncrn, v); c->ncrn++; }
    else            { gm_wr32(m, c->nsaa, v);    c->nsaa += 4; }
}

void marshal_place_dword(guest_mem *m, mcur *c, uint64_t v){
    if (c->ncrn & 1) c->ncrn++;                  /* round up to even */
    if (c->ncrn <= 2){
        m->reg_set(m, c->ncrn,     (uint32_t)v);
        m->reg_set(m, c->ncrn + 1, (uint32_t)(v >> 32));
        c->ncrn += 2;
    } else {
        c->ncrn = 4;
        c->nsaa = (c->nsaa + 7u) & ~7u;
        gm_wr32(m, c->nsaa,     (uint32_t)v);
        gm_wr32(m, c->nsaa + 4, (uint32_t)(v >> 32));
        c->nsaa += 8;
    }
}
