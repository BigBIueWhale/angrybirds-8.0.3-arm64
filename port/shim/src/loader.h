/* loader.h — engine load + relocate into the CPU (device layer; Audit 03/04). */
#ifndef ABSHIM_LOADER_H
#define ABSHIM_LOADER_H
#include "cpu.h"
#include "elf32.h"

#define LOADER_MAX_STUBS 512

typedef struct loader {
    cpu_t      *cpu;
    elf32_image img;
    uint32_t   *sym_addr;                 /* [symcount] resolved guest addr cache */
    uint8_t    *sym_done;                 /* [symcount] resolved? */
    const char *stub_name[LOADER_MAX_STUBS];  /* symbol name per stub slot (for dispatch) */
    uint32_t    stub_count;
    uint32_t    gd_next;                  /* GUESTDATA bump for object mirrors */
    uint32_t    init_va, init_count;      /* init_array */
    int         err;
} loader_t;

/* Parse, map at RG_ENGINE, apply relocations (guest-defined / stub / GUESTDATA
 * mirror / weak-zero per elf32_classify), and locate init_array. 0 on success. */
int  loader_load(loader_t *L, cpu_t *cpu, const uint8_t *elf, size_t len);
void loader_free(loader_t *L);

/* Map a trapped stub address back to its symbol name (for dispatch). NULL if
 * the address is not a known stub slot. */
const char *loader_stub_name(loader_t *L, uint32_t stub_addr);

#endif /* ABSHIM_LOADER_H */
