/* elf32.h — ELF32/ARM loader parsing + symbol classification (Audit 03/04).
 *
 * Pure logic over the raw engine `.so` bytes in host memory: parses program
 * headers + the dynamic section, enumerates relocations and init_array, and —
 * the correctness core — classifies each relocated symbol so the device-layer
 * loader binds it to the right target:
 *   - guest-defined  -> base + st_value (Thumb bit preserved)   [keeps the whole
 *                       in-guest C++ EH runtime working]
 *   - UND function   -> a host bridge stub
 *   - UND object     -> a real GUESTDATA mirror     [B1-1: _ctype_/__sF/...]
 *   - weak-UND in the whitelist -> 0                 [only the __google blocking-region
 *                       hints now — see below]
 * The C++ exception FINDER __gnu_Unwind_Find_exidx / dl_unwind_find_exidx are NO LONGER
 * whitelisted-to-0: the engine statically links the unwinder (defines __cxa_throw /
 * _Unwind_RaiseException, 222 RTTI syms) but leaves the exidx finder weak-UND, expecting
 * bionic to provide it on-device. The shim must BRIDGE it to return the engine's
 * PT_ARM_EXIDX table (exidx_va/exidx_sz below) or a thrown exception can't unwind ->
 * std::terminate. (Corrects the old X1 note: the engine defines NO __exidx_start/end, so
 * the claimed static fallback never existed.)
 * All dynamic tables are read via a vaddr->file-offset map (B1-7: never assumes
 * p_offset==p_vaddr). */
#ifndef ABSHIM_ELF32_H
#define ABSHIM_ELF32_H
#include <stdint.h>
#include <stddef.h>
#include <elf.h>

enum elf32_symclass {
    SC_GUEST_FUNC = 0,  /* defined FUNC   -> base+value (thumb bit in `thumb`) */
    SC_GUEST_OBJECT,    /* defined OBJECT -> base+value */
    SC_UND_FUNC,        /* undefined FUNC -> host stub */
    SC_UND_OBJECT,      /* undefined OBJECT -> GUESTDATA mirror (B1-1) */
    SC_WEAK_ZERO        /* weak-UND whitelist -> 0 (X1) */
};

typedef struct {
    int         cls;
    uint32_t    value;     /* st_value with Thumb bit cleared (guest-defined) */
    int         thumb;     /* Thumb bit of a defined FUNC */
    const char *name;      /* into the ELF strtab (host pointer) */
} elf32_symres;

typedef struct {
    const uint8_t *elf; size_t len;
    const Elf32_Phdr *phdr; int phnum;
    const Elf32_Dyn  *dyn;  uint32_t dyn_count;
    uint32_t symtab_va, strtab_va, strsz, syment;
    uint32_t rel_va, relsz, jmprel_va, pltrelsz;
    uint32_t init_array_va, init_arraysz;
    uint32_t hash_va, symcount;
    uint32_t image_size;    /* page-rounded max(vaddr+memsz) over PT_LOAD */
    uint32_t exidx_va, exidx_sz;  /* PT_ARM_EXIDX: .ARM.exidx unwind table (each entry 8B) */
} elf32_image;

/* Parse headers + dynamic section. Returns 0 on success, <0 on malformed. */
int  elf32_parse(const uint8_t *elf, size_t len, elf32_image *img);

/* Map a virtual address to a file offset within a PT_LOAD segment (B1-7). */
int  elf32_vaddr_to_off(const elf32_image *img, uint32_t va, uint32_t *off);

/* Classify the symbol at dynamic-symtab index `si`. */
elf32_symres elf32_classify(const elf32_image *img, uint32_t si);

/* Find a symbol index by name (linear scan; 0/none via return -1). Test/util. */
int  elf32_find_symbol(const elf32_image *img, const char *name, uint32_t *out_index);

/* Relocation iteration: cb receives each REL entry from DT_REL then DT_JMPREL. */
typedef void (*elf32_reloc_cb)(void *ud, uint32_t r_offset, uint32_t r_type, uint32_t sym_index);
void elf32_foreach_reloc(const elf32_image *img, elf32_reloc_cb cb, void *ud);

/* init_array location; returns entry count (each is a function vaddr). */
uint32_t elf32_init_array(const elf32_image *img, uint32_t *out_vaddr);

#endif /* ABSHIM_ELF32_H */
