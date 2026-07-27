/* elf32.c — ELF32/ARM loader parsing + symbol classification. See elf32.h. */
#include "elf32.h"
#include <string.h>

#ifndef R_ARM_ABS32
#define R_ARM_ABS32     2
#endif
#ifndef R_ARM_GLOB_DAT
#define R_ARM_GLOB_DAT  21
#endif
#ifndef R_ARM_JUMP_SLOT
#define R_ARM_JUMP_SLOT 22
#endif
#ifndef R_ARM_RELATIVE
#define R_ARM_RELATIVE  23
#endif

/* Weak-UND symbols that resolve to 0. Only the __google blocking-region hints now:
 * they are pure no-op annotations. The exidx finders were REMOVED from this list — the
 * engine's static C++ unwinder needs a real one (bridged in dispatch); resolving them to
 * 0 silently broke every thrown exception (std::terminate). Everything else weak-UND is
 * bound like a strong-UND (a real bridge). */
static int is_zero_whitelist(const char *n){
    return !strcmp(n,"__google_potentially_blocking_region_begin")
        || !strcmp(n,"__google_potentially_blocking_region_end");
}

static uint32_t rd32(const uint8_t *b, size_t off){ uint32_t v; memcpy(&v,b+off,4); return v; }

int elf32_vaddr_to_off(const elf32_image *img, uint32_t va, uint32_t *off){
    for (int i = 0; i < img->phnum; i++){
        const Elf32_Phdr *p = &img->phdr[i];
        if (p->p_type == PT_LOAD && va >= p->p_vaddr && va < p->p_vaddr + p->p_filesz){
            uint32_t o = p->p_offset + (va - p->p_vaddr);
            if ((size_t)o >= img->len) return -1;
            *off = o; return 0;
        }
    }
    return -1;
}

int elf32_parse(const uint8_t *elf, size_t len, elf32_image *img){
    memset(img, 0, sizeof *img);
    if (len < sizeof(Elf32_Ehdr)) return -1;
    const Elf32_Ehdr *eh = (const Elf32_Ehdr*)elf;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) return -2;
    if (eh->e_ident[EI_CLASS] != ELFCLASS32)        return -3;
    if (eh->e_ident[EI_DATA]  != ELFDATA2LSB)       return -3;
    if (eh->e_machine != EM_ARM)                    return -4;
    img->elf = elf; img->len = len;
    if ((size_t)eh->e_phoff + (size_t)eh->e_phnum * sizeof(Elf32_Phdr) > len) return -5;
    img->phdr = (const Elf32_Phdr*)(elf + eh->e_phoff);
    img->phnum = eh->e_phnum;

    uint32_t maxva = 0; int have_dyn = 0;
    for (int i = 0; i < img->phnum; i++){
        const Elf32_Phdr *p = &img->phdr[i];
        if (p->p_type == PT_LOAD){
            uint32_t e = p->p_vaddr + p->p_memsz; if (e > maxva) maxva = e;
        } else if (p->p_type == PT_DYNAMIC){
            uint32_t doff;
            if (elf32_vaddr_to_off(img, p->p_vaddr, &doff) == 0){
                img->dyn = (const Elf32_Dyn*)(elf + doff);
                img->dyn_count = p->p_filesz / sizeof(Elf32_Dyn);
                have_dyn = 1;
            }
        } else if (p->p_type == 0x70000001u /* PT_ARM_EXIDX */){
            img->exidx_va = p->p_vaddr; img->exidx_sz = p->p_memsz;   /* for the C++ unwinder's exidx finder */
        }
    }
    img->image_size = (maxva + 0xfffu) & ~0xfffu;
    if (!have_dyn) return -6;

    uint32_t seen = 0;
    for (const Elf32_Dyn *d = img->dyn; seen < img->dyn_count && d->d_tag != DT_NULL; d++, seen++){
        uint32_t v = d->d_un.d_val;
        switch (d->d_tag){
        case DT_SYMTAB:       img->symtab_va = v; break;
        case DT_STRTAB:       img->strtab_va = v; break;
        case DT_STRSZ:        img->strsz = v; break;
        case DT_SYMENT:       img->syment = v; break;
        case DT_REL:          img->rel_va = v; break;
        case DT_RELSZ:        img->relsz = v; break;
        case DT_JMPREL:       img->jmprel_va = v; break;
        case DT_PLTRELSZ:     img->pltrelsz = v; break;
        case DT_INIT_ARRAY:   img->init_array_va = v; break;
        case DT_INIT_ARRAYSZ: img->init_arraysz = v; break;
        case DT_HASH:         img->hash_va = v; break;
        default: break;
        }
    }
    /* dynamic-symbol count from the SysV hash table's nchain (2nd word) */
    if (img->hash_va){
        uint32_t ho;
        if (elf32_vaddr_to_off(img, img->hash_va + 4, &ho) == 0) img->symcount = rd32(elf, ho);
    }
    if (!img->symtab_va || !img->strtab_va) return -7;
    return 0;
}

elf32_symres elf32_classify(const elf32_image *img, uint32_t si){
    elf32_symres r; memset(&r, 0, sizeof r); r.name = "";
    uint32_t so;
    if (elf32_vaddr_to_off(img, img->symtab_va + si * sizeof(Elf32_Sym), &so) != 0){ r.cls = SC_UND_FUNC; return r; }
    const Elf32_Sym *s = (const Elf32_Sym*)(img->elf + so);
    uint32_t no;
    if (elf32_vaddr_to_off(img, img->strtab_va + s->st_name, &no) == 0) r.name = (const char*)(img->elf + no);
    int type = ELF32_ST_TYPE(s->st_info), bind = ELF32_ST_BIND(s->st_info);
    if (s->st_shndx != SHN_UNDEF){
        r.cls   = (type == STT_OBJECT) ? SC_GUEST_OBJECT : SC_GUEST_FUNC;
        r.value = s->st_value & ~1u;
        r.thumb = (type == STT_FUNC) && (s->st_value & 1u);
        return r;
    }
    if (bind == STB_WEAK && is_zero_whitelist(r.name)){ r.cls = SC_WEAK_ZERO; return r; }
    r.cls = (type == STT_OBJECT) ? SC_UND_OBJECT : SC_UND_FUNC;
    return r;
}

int elf32_find_symbol(const elf32_image *img, const char *name, uint32_t *out_index){
    for (uint32_t i = 0; i < img->symcount; i++){
        uint32_t so;
        if (elf32_vaddr_to_off(img, img->symtab_va + i * sizeof(Elf32_Sym), &so) != 0) continue;
        const Elf32_Sym *s = (const Elf32_Sym*)(img->elf + so);
        uint32_t no;
        if (elf32_vaddr_to_off(img, img->strtab_va + s->st_name, &no) != 0) continue;
        if (!strcmp((const char*)(img->elf + no), name)){ if (out_index) *out_index = i; return 0; }
    }
    return -1;
}

static void foreach_in(const elf32_image *img, uint32_t va, uint32_t sz, elf32_reloc_cb cb, void *ud){
    if (!va || !sz) return;
    uint32_t n = sz / sizeof(Elf32_Rel);
    for (uint32_t i = 0; i < n; i++){
        uint32_t ro;
        if (elf32_vaddr_to_off(img, va + i * sizeof(Elf32_Rel), &ro) != 0) return;
        const Elf32_Rel *r = (const Elf32_Rel*)(img->elf + ro);
        cb(ud, r->r_offset, ELF32_R_TYPE(r->r_info), ELF32_R_SYM(r->r_info));
    }
}
void elf32_foreach_reloc(const elf32_image *img, elf32_reloc_cb cb, void *ud){
    foreach_in(img, img->rel_va,    img->relsz,    cb, ud);
    foreach_in(img, img->jmprel_va, img->pltrelsz, cb, ud);
}

uint32_t elf32_init_array(const elf32_image *img, uint32_t *out_vaddr){
    if (out_vaddr) *out_vaddr = img->init_array_va;
    return img->init_arraysz / 4u;
}
