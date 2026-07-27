/* ctype_tables.c — GUESTDATA mirror content. See ctype_tables.h. */
#include "ctype_tables.h"
#include <string.h>

/* bionic __sFILE flag bits we set (buffer fields stay 0 — never read inline). */
#define ABSH_SRD 0x0004
#define ABSH_SWR 0x0008
#define SFILE_SZ   84
#define SFILE_FLAGS 0x0c   /* short */
#define SFILE_FILE  0x0e   /* short */

static int g_built;
static uint8_t  T_ctype[257];        /* char[257] */
static int16_t  T_lower[257];        /* short[257] */
static int16_t  T_upper[257];        /* short[257] */
static uint8_t  T_sF[3*SFILE_SZ];    /* 3 x __sFILE */
static uint8_t  T_guard[4];          /* __stack_chk_guard */

static uint8_t classify(int c){
    uint8_t b = 0;
    if (c >= 'A' && c <= 'Z') b |= _CT_U;
    if (c >= 'a' && c <= 'z') b |= _CT_L;
    if (c >= '0' && c <= '9') b |= _CT_N;
    if (c==' '||c=='\t'||c=='\n'||c=='\v'||c=='\f'||c=='\r') b |= _CT_S;
    if ((c>='0'&&c<='9')||(c>='A'&&c<='F')||(c>='a'&&c<='f')) b |= _CT_X;
    if (c < 0x20 || c == 0x7f) b |= _CT_C;
    if (c == ' ') b |= _CT_B;
    if (c >= 0x21 && c <= 0x7e && !((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9'))) b |= _CT_P;
    return b;
}
static void put16(uint8_t *p, int16_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)((uint16_t)v>>8); }

static void build(void){
    if (g_built) return;
    T_ctype[0] = 0;                                  /* EOF slot */
    T_lower[0] = -1; T_upper[0] = -1;                /* EOF -> EOF */
    for (int c = 0; c <= 255; c++){
        T_ctype[c+1] = classify(c);
        T_lower[c+1] = (int16_t)((c>='A'&&c<='Z') ? c+32 : c);
        T_upper[c+1] = (int16_t)((c>='a'&&c<='z') ? c-32 : c);
    }
    memset(T_sF, 0, sizeof T_sF);
    for (int i = 0; i < 3; i++){
        put16(T_sF + i*SFILE_SZ + SFILE_FILE, (int16_t)i);
        put16(T_sF + i*SFILE_SZ + SFILE_FLAGS, (int16_t)(i==0 ? ABSH_SRD : ABSH_SWR));
    }
    /* fixed canary with a leading NUL byte (bionic-style: stops string overruns) */
    T_guard[0]=0x00; T_guard[1]=0xa5; T_guard[2]=0xdc; T_guard[3]=0xe2;
    g_built = 1;
}

const void *ctype_mirror(const char *name, uint32_t *size, uint32_t *sym_off){
    build();
    uint32_t off = 0, sz = 0; const void *p = NULL;
    if      (!strcmp(name,"_ctype_"))           { p=T_ctype; sz=sizeof T_ctype; off=0; }
    /* off=0 (pointer at &T[0], the base) — SAME convention as _ctype_ above. The engine's std::ctype
     * indexes the table as (tab)[c+1] (BSD/bionic `(tab)[c+1]` with T[0]=EOF slot; verified in the
     * disasm at engine 0x7253b4: `ldrh [ptr + c*2 + 2]` = element c+1). Previously off=sizeof(int16_t)
     * pointed the var at &T[1], so (tab)[c+1] read T[c+2]=toupper(c+1) — a +1 shift (e.g. 'P'->'Q')
     * that corrupted every std::toupper/tolower result (broke the image-format extension match -> draws=0). */
    else if (!strcmp(name,"_tolower_tab_"))      { p=T_lower; sz=sizeof T_lower; off=0; }
    else if (!strcmp(name,"_toupper_tab_"))      { p=T_upper; sz=sizeof T_upper; off=0; }
    else if (!strcmp(name,"__sF"))               { p=T_sF;    sz=sizeof T_sF;    off=0; }
    else if (!strcmp(name,"__stack_chk_guard"))  { p=T_guard; sz=sizeof T_guard; off=0; }
    else return NULL;
    if (size) *size = sz;
    if (sym_off) *sym_off = off;
    return p;
}

uint32_t ctype_write_mirror(guest_mem *m, uint32_t gaddr, const char *name, uint32_t *consumed){
    uint32_t size, off;
    const void *p = ctype_mirror(name, &size, &off);
    if (!p){ if (consumed) *consumed = 0; return 0; }
    m->write(m, gaddr, p, size);                     /* the table / value bytes at gaddr */
    /* _ctype_ / _tolower_tab_ / _toupper_tab_ are POINTER variables in bionic
     * (const char* / const short*): the compiled engine loads the pointer VALUE and
     * then indexes it  (ldr r3,[&sym]; ldrb [r3 + c + 1]).  So the symbol must resolve
     * to a 4-byte pointer WORD holding &table[sym_off] — NOT to the table itself.
     * __sF (a FILE array; symbol = &array[0]) and __stack_chk_guard (a scalar the code
     * dereferences once for the canary value) are accessed directly, no indirection. */
    int ptr_var = !strcmp(name,"_ctype_") || !strcmp(name,"_tolower_tab_") || !strcmp(name,"_toupper_tab_");
    if (!ptr_var){ if (consumed) *consumed = size; return gaddr + off; }
    uint32_t pw = (gaddr + size + 3u) & ~3u;         /* 4-byte-aligned pointer word after the table */
    uint32_t pv = gaddr + off;                       /* &table[sym_off] */
    uint8_t b[4] = { (uint8_t)pv, (uint8_t)(pv>>8), (uint8_t)(pv>>16), (uint8_t)(pv>>24) };
    m->write(m, pw, b, 4);
    if (consumed) *consumed = (pw - gaddr) + 4u;
    return pw;                                        /* symbol resolves to the pointer word */
}
