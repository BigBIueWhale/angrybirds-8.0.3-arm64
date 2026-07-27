/* cpu.c — Unicorn ARM32 CPU + mem-ops binding. See cpu.h. */
#include "cpu.h"
#include "regions.h"
#include <string.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
static void clog(const char*fmt,...){ static int(*rl)(int,const char*,const char*,...)=0; static int t=0;
    if(!t){t=1; rl=(int(*)(int,const char*,const char*,...))dlsym(RTLD_DEFAULT,"__android_log_print");}
    char b[200]; va_list ap; va_start(ap,fmt); vsnprintf(b,sizeof b,fmt,ap); va_end(ap); if(rl) rl(4,"abshim","%s",b); }

static int reg_id(int i){
    switch (i){
    case 13: return UC_ARM_REG_SP;
    case 14: return UC_ARM_REG_LR;
    case 15: return UC_ARM_REG_PC;
    default: return UC_ARM_REG_R0 + i;         /* r0..r12 */
    }
}
/* ---- GUEST-WRITE WATCHPOINT ON CHUNK HEAD WORDS (non-release diagnostic) --------------------
 * The live shim reports galloc_check == -5 permanently: an in-use chunk whose NEXT chunk has
 * PINUSE cleared. test_galloc_quarantine.c cleared the allocator across every production op mix
 * (200k ops, ASan+UBSan), so the writer is external to galloc — i.e. the guest.
 *
 * This isolates it exactly, thanks to a Unicorn property: galloc writes through gm_wr32 ->
 * uc_mem_write, a HOST API call, which does NOT fire UC_HOOK_MEM_WRITE. That hook fires only on
 * writes executed by guest instructions. So everything recorded here is guest-issued by
 * construction; galloc's own bookkeeping is invisible to it.
 *
 * Cheap filter: galloc_check enforces chunk address ≡ 8 (mod 16) and the head sits at c+4, so
 * every head word is at an address ≡ 12 (mod 16). A guest write is only interesting if its byte
 * range covers such an address — that discards ~15/16 of payload traffic in the hook itself.
 *
 * The address is NOT stable run to run (observed 0x500db338@op12544 and 0x5025ce48@op26368), so a
 * fixed watchpoint cannot work; this records into a ring and the failure site queries it. */
#ifndef ABSHIM_RELEASE
#define HW_RING 1024u
static struct { uint32_t addr, val, pc, lr; } g_hw[HW_RING];
static unsigned long g_hw_n = 0;

static void hw_write_hook(uc_engine *uc, uc_mem_type type, uint64_t addr,
                          int size, int64_t value, void *ud){
    (void)type; (void)ud;
    int hit = 0;
    for (int k = 0; k < size; k++) if ((((uint32_t)addr + k) & 0xFu) == 12u){ hit = 1; break; }
    if (!hit) return;
    uint32_t pc = 0, lr = 0;
    uc_reg_read(uc, UC_ARM_REG_PC, &pc);
    uc_reg_read(uc, UC_ARM_REG_LR, &lr);
    unsigned i = (unsigned)(g_hw_n++ % HW_RING);
    g_hw[i].addr = (uint32_t)addr; g_hw[i].val = (uint32_t)value;
    g_hw[i].pc = pc; g_hw[i].lr = lr;
}

/* Most recent guest write whose byte range covered `want`. Returns 1 on hit. */
int cpu_hw_find(uint32_t want, uint32_t *addr, uint32_t *val, uint32_t *pc, uint32_t *lr){
    unsigned long n = g_hw_n < HW_RING ? g_hw_n : HW_RING;
    for (unsigned long j = 1; j <= n; j++){
        unsigned i = (unsigned)((g_hw_n - j) % HW_RING);
        if (g_hw[i].addr <= want && want < g_hw[i].addr + 8u){
            if(addr)*addr=g_hw[i].addr; if(val)*val=g_hw[i].val;
            if(pc)*pc=g_hw[i].pc; if(lr)*lr=g_hw[i].lr;
            return 1;
        }
    }
    return 0;
}
unsigned long cpu_hw_count(void){ return g_hw_n; }
#endif

static void     m_read (guest_mem *m, void *d, uint32_t a, uint32_t n){ uc_mem_read ((uc_engine*)m->ctx, a, d, n); }
static void     m_write(guest_mem *m, uint32_t a, const void *s, uint32_t n){ uc_mem_write((uc_engine*)m->ctx, a, (void*)s, n); }
static uint32_t m_rg   (guest_mem *m, int i){ uint32_t v=0; uc_reg_read ((uc_engine*)m->ctx, reg_id(i), &v); return v; }
static void     m_sg   (guest_mem *m, int i, uint32_t v){ uc_reg_write((uc_engine*)m->ctx, reg_id(i), &v); }

static void fill_bxlr(uc_engine *uc, uint32_t base, uint32_t sz){
    uint32_t bx = ARM_BX_LR;
    for (uint32_t o = 0; o < sz; o += 4) uc_mem_write(uc, base + o, &bx, 4);
}

int cpu_create(cpu_t *c){
    memset(c, 0, sizeof *c);
    if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &c->uc)) return -1;
    c->mem.read = m_read; c->mem.write = m_write; c->mem.reg_get = m_rg; c->mem.reg_set = m_sg; c->mem.ctx = c->uc;

    /* core regions (engine image is mapped by the loader at its exact size) */
    uc_mem_map(c->uc, RG_STUB,      RG_STUB_SZ,      UC_PROT_READ|UC_PROT_EXEC);
    uc_mem_map(c->uc, RG_JNI,       RG_JNI_SZ,       UC_PROT_READ|UC_PROT_EXEC);
    uc_mem_map(c->uc, RG_GUESTDATA, RG_GUESTDATA_SZ, UC_PROT_READ|UC_PROT_WRITE);
    uc_mem_map(c->uc, RG_TCB,       RG_TCB_SZ,       UC_PROT_READ|UC_PROT_WRITE);  /* per-thread TCB/errno */
    uc_err mr_heap = uc_mem_map(c->uc, RG_HEAP, RG_HEAP_SZ, UC_PROT_READ|UC_PROT_WRITE);
    uc_mem_map(c->uc, RG_ASSET,     RG_ASSET_SZ,     UC_PROT_READ|UC_PROT_WRITE);  /* dedicated AAsset copy arena */
    uc_mem_map(c->uc, RG_STACK,     RG_STACK_SZ,     UC_PROT_READ|UC_PROT_WRITE);
    uc_mem_map(c->uc, RG_RET,       RG_RET_SZ,       UC_PROT_READ|UC_PROT_EXEC);
    uc_mem_map(c->uc, RG_KUSER,     RG_KUSER_SZ,     UC_PROT_READ|UC_PROT_EXEC);

    fill_bxlr(c->uc, RG_STUB,  RG_STUB_SZ);
    fill_bxlr(c->uc, RG_JNI,   RG_JNI_SZ);
    fill_bxlr(c->uc, RG_RET,   RG_RET_SZ);
    fill_bxlr(c->uc, RG_KUSER, RG_KUSER_SZ);
    /* An ARM CLREX (0xf57ff01f) at RG_KUSER (an unused word — the kuser helpers live at
     * 0xffff0f{a,c,e}0, and the engine references none of them). The scheduler runs this one
     * instruction on every green-thread resume to clear the exclusive monitor (real OSes CLREX
     * on context switch), so an LDREX..STREX split by a preemptive time-slice can't corrupt. */
    { uint32_t clrex_insn = 0xf57ff01fu; uc_mem_write(c->uc, RG_KUSER, &clrex_insn, 4); }

    /* enable VFP/NEON (CPACR + FPEXC) — required: the engine uses inline VFP and
       runs statically-linked soft-float helpers (Audit 06 pivot) */
    uint32_t cpacr = 0; uc_reg_read(c->uc, UC_ARM_REG_C1_C0_2, &cpacr);
    cpacr |= (0xfu << 20); uc_reg_write(c->uc, UC_ARM_REG_C1_C0_2, &cpacr);
    uint32_t fpexc = 0x40000000u; uc_reg_write(c->uc, UC_ARM_REG_FPEXC, &fpexc);

    clog("[cpu-init] heap: uc_mem_map=%d(%s) base=0x%x size=0x%x (%uMB) guard@0x%x",
         (int)mr_heap, uc_strerror(mr_heap), RG_HEAP, RG_HEAP_SZ, (unsigned)(RG_HEAP_SZ>>20), RG_HEAPGUARD);
    c->heap = galloc_create(&c->mem, RG_HEAP, RG_HEAP_SZ);
    if (!c->heap) return -1;
    /* UAF shield: quarantine freed blocks on the C++/malloc heap so the engine's prematurely-freed
     * std::string _Rep keeps intact bytes across the window a stale scene/registry reference reads it
     * (kills the cyclic-_Rb_tree nativeInit grind + registry loop + garbage strings at the mechanism). */
    galloc_set_quarantine(c->heap, 131072u);
    clog("[cpu-init] heap UAF-quarantine enabled (depth 131072)");
#ifndef ABSHIM_RELEASE
    {   /* watch GUEST writes that land on chunk head words (see hw_write_hook above) */
        uc_hook hh;
        if (uc_hook_add(c->uc, &hh, UC_HOOK_MEM_WRITE, (void*)hw_write_hook, NULL,
                        RG_HEAP, (uint64_t)RG_HEAP + RG_HEAP_SZ - 1) == UC_ERR_OK)
            clog("[cpu-init] chunk-head guest-write watchpoint armed over 0x%08x+0x%x", RG_HEAP, RG_HEAP_SZ);
        else
            clog("[cpu-init] WARNING: could not arm chunk-head write watchpoint");
    }
#endif
    c->asset_heap = galloc_create(&c->mem, RG_ASSET, RG_ASSET_SZ);
    if (!c->asset_heap) return -1;
    return 0;
}

void cpu_destroy(cpu_t *c){
    if (c->heap) galloc_destroy(c->heap);
    if (c->asset_heap) galloc_destroy(c->asset_heap);
    if (c->uc) uc_close(c->uc);
    c->heap = NULL; c->asset_heap = NULL; c->uc = NULL;
}

/* per-thread pending-longjmp (only one guest thread runs under the BEL at a time,
 * and the flag is set + consumed on that same carrier thread) */
static __thread int      t_lj_pending;
static __thread uint32_t t_lj_target;
void cpu_set_longjmp(uint32_t target){ t_lj_pending = 1; t_lj_target = target; }

uc_err cpu_run(cpu_t *c, uint32_t start, uint64_t budget){
    uint32_t pc = start;
    t_lj_pending = 0;
    for (;;){
        uc_err e = uc_emu_start(c->uc, pc, RG_RET, 0, budget);
        if (e != UC_ERR_OK) return e;
        if (t_lj_pending){ t_lj_pending = 0; pc = t_lj_target; continue; }  /* longjmp: resume at setjmp site */
        return UC_ERR_OK;                                                    /* reached RG_RET */
    }
}

uc_err cpu_call(cpu_t *c, uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint64_t budget){
    uint32_t sp = RG_STACK + RG_STACK_SZ - 0x1000;   /* single-GEC stack top for now */
    uint32_t lr = RG_RET;
    uc_reg_write(c->uc, UC_ARM_REG_SP, &sp);
    uc_reg_write(c->uc, UC_ARM_REG_LR, &lr);
    uc_reg_write(c->uc, UC_ARM_REG_R0, &a0);
    uc_reg_write(c->uc, UC_ARM_REG_R1, &a1);
    uc_reg_write(c->uc, UC_ARM_REG_R2, &a2);
    uc_reg_write(c->uc, UC_ARM_REG_R3, &a3);
    return cpu_run(c, addr, budget);                 /* addr LSB selects ARM/Thumb */
}
