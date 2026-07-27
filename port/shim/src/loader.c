/* loader.c — engine load + relocate. See loader.h. */
#include "loader.h"
#include "regions.h"
#include "ctype_tables.h"
#include <stdlib.h>
#include <string.h>

static void gd_zero(loader_t *L, uint32_t addr, uint32_t n){
    uint8_t z[256]; memset(z, 0, sizeof z);
    while (n){ uint32_t k = n < sizeof z ? n : sizeof z; L->cpu->mem.write(&L->cpu->mem, addr, z, k); addr += k; n -= k; }
}

static uint32_t resolve_sym(loader_t *L, uint32_t si){
    if (si < L->img.symcount && L->sym_done[si]) return L->sym_addr[si];
    elf32_symres r = elf32_classify(&L->img, si);
    uint32_t addr = 0;
    switch (r.cls){
    case SC_GUEST_FUNC:   addr = (RG_ENGINE + r.value) | (r.thumb ? 1u : 0u); break;  /* thumb bit for blx */
    case SC_GUEST_OBJECT: addr = RG_ENGINE + r.value; break;
    case SC_WEAK_ZERO:    addr = 0; break;                                            /* X1 */
    case SC_UND_OBJECT: {
        uint32_t base = RG_GUESTDATA + L->gd_next;
        uint32_t consumed = 0;
        uint32_t sym  = ctype_write_mirror(&L->cpu->mem, base, r.name, &consumed);     /* B1-1 */
        if (sym){ L->gd_next += (consumed + 15u) & ~15u; addr = sym; }                 /* table [+ ptr word] */
        else { gd_zero(L, base, 256); L->gd_next += 256; addr = base; }                /* unknown obj: zeroed */
        break; }
    case SC_UND_FUNC:
    default:
        if (L->stub_count < LOADER_MAX_STUBS){ addr = RG_STUB + L->stub_count * 4u; L->stub_name[L->stub_count] = r.name; L->stub_count++; }
        else { L->err = 1; addr = RG_STUB; }
        break;
    }
    if (si < L->img.symcount){ L->sym_addr[si] = addr; L->sym_done[si] = 1; }
    return addr;
}

static void reloc_cb(void *ud, uint32_t r_off, uint32_t r_type, uint32_t si){
    loader_t *L = (loader_t*)ud; guest_mem *m = &L->cpu->mem;
    uint32_t off = RG_ENGINE + r_off, cur = gm_rd32(m, off);
    switch (r_type){
    case 23: gm_wr32(m, off, cur + RG_ENGINE); break;                 /* R_ARM_RELATIVE */
    case 21: /* GLOB_DAT */
    case 22: gm_wr32(m, off, resolve_sym(L, si)); break;              /* JUMP_SLOT */
    case 2:  gm_wr32(m, off, resolve_sym(L, si) + cur); break;        /* ABS32 (addend) */
    default: L->err = 1; break;                                       /* unhandled type -> hard error */
    }
}

int loader_load(loader_t *L, cpu_t *cpu, const uint8_t *elf, size_t len){
    memset(L, 0, sizeof *L); L->cpu = cpu;
    if (elf32_parse(elf, len, &L->img)) return -1;
    L->sym_addr = (uint32_t*)calloc(L->img.symcount ? L->img.symcount : 1, sizeof(uint32_t));
    L->sym_done = (uint8_t*) calloc(L->img.symcount ? L->img.symcount : 1, 1);
    if (!L->sym_addr || !L->sym_done) return -1;

    /* map engine image RWX for load+reloc (perm-tightening / RELRO is a later
       hardening pass; functionally the boot needs the image mapped) */
    if (uc_mem_map(cpu->uc, RG_ENGINE, L->img.image_size, UC_PROT_ALL)) return -2;
    for (int i = 0; i < L->img.phnum; i++){
        const Elf32_Phdr *p = &L->img.phdr[i];
        if (p->p_type == PT_LOAD)
            uc_mem_write(cpu->uc, RG_ENGINE + p->p_vaddr, elf + p->p_offset, p->p_filesz);
    }
    elf32_foreach_reloc(&L->img, reloc_cb, L);
    if (L->err) return -3;

    L->init_count = elf32_init_array(&L->img, &L->init_va);
    return 0;
}

void loader_free(loader_t *L){
    if (!L) return;
    free(L->sym_addr); free(L->sym_done);
    L->sym_addr = NULL; L->sym_done = NULL;
}

const char *loader_stub_name(loader_t *L, uint32_t stub_addr){
    if (stub_addr < RG_STUB || stub_addr >= RG_STUB + RG_STUB_SZ) return NULL;
    uint32_t idx = (stub_addr - RG_STUB) / 4u;
    return idx < L->stub_count ? L->stub_name[idx] : NULL;
}
