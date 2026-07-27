/* jni_argbuild.c — see jni_argbuild.h. Pure logic over memops/marshal/handle_table. */
#include "jni_argbuild.h"
#include <string.h>

/* Advance `*p` past one JNI type token in a signature, returning a single-char
 * tag: Z B C S I J F D for primitives, or 'L' for ANY reference type (object
 * `L...;` or array `[...`). Returns 0 at ')' or end of string. */
static char nextp(const char **p){
    const char *s = *p;
    if (!*s || *s == ')'){ *p = s; return 0; }
    if (*s == '['){                       /* array: skip all '[' then the element type */
        while (*s == '[') s++;
        if (*s == 'L'){ while (*s && *s != ';') s++; if (*s == ';') s++; }
        else if (*s) s++;
        *p = s; return 'L';
    }
    if (*s == 'L'){ while (*s && *s != ';') s++; if (*s == ';') s++; *p = s; return 'L'; }
    char t = *s; s++; *p = s; return t;    /* primitive */
}
static const char *params_of(const char *sig){ const char *p = strchr(sig, '('); return p ? p + 1 : sig; }

/* va_list cursor readers (guest addresses; dwords 8-aligned as on ARM32). */
static uint32_t va_w(guest_mem *m, uint32_t *va){ uint32_t v = gm_rd32(m, *va); *va += 4; return v; }
static uint64_t va_d(guest_mem *m, uint32_t *va){
    *va = (*va + 7u) & ~7u;
    uint32_t lo = gm_rd32(m, *va), hi = gm_rd32(m, *va + 4); *va += 8;
    return (uint64_t)lo | ((uint64_t)hi << 32);
}

/* Store one already-de-promoted 32-bit word into a cell per its primitive tag
 * (sub-int types narrowed; 'L' resolved through the handle table). Shared by the
 * valist and inline forms, whose scalar words arrive the same way. */
static void put_word(ab_jval *o, char t, uint32_t v, handle_table *ht){
    switch (t){
    case 'Z': o->z = (uint8_t)(v & 1u); break;
    case 'B': o->b = (int8_t)v;   break;
    case 'C': o->c = (uint16_t)v; break;
    case 'S': o->s = (int16_t)v;  break;
    case 'L': o->l = ht_resolve(ht, v); break;
    default:  o->i = (int32_t)v;  break;   /* 'I' */
    }
}

int ab_build_valist(guest_mem *m, handle_table *ht, const char *sig, uint32_t va, ab_jval *out, int maxn){
    const char *p = params_of(sig); int n = 0; char t;
    while ((t = nextp(&p)) && n < maxn){
        if (t == 'J'){ out[n].j = (int64_t)va_d(m, &va); }
        else if (t == 'D'){ uint64_t v = va_d(m, &va); memcpy(&out[n].d, &v, 8); }
        else if (t == 'F'){ uint64_t v = va_d(m, &va); double d; memcpy(&d, &v, 8); out[n].f = (float)d; } /* promoted double -> float */
        else { put_word(&out[n], t, va_w(m, &va), ht); }
        n++;
    }
    return n;
}

int ab_build_inline(guest_mem *m, handle_table *ht, const char *sig, mcur *cur, ab_jval *out, int maxn){
    const char *p = params_of(sig); int n = 0; char t;
    while ((t = nextp(&p)) && n < maxn){
        if (t == 'J'){ out[n].j = (int64_t)marshal_pull_dword(m, cur); }
        else if (t == 'D'){ uint64_t v = marshal_pull_dword(m, cur); memcpy(&out[n].d, &v, 8); }
        else if (t == 'F'){ uint64_t v = marshal_pull_dword(m, cur); double d; memcpy(&d, &v, 8); out[n].f = (float)d; } /* promoted */
        else { put_word(&out[n], t, marshal_pull_word(m, cur), ht); }
        n++;
    }
    return n;
}

int ab_build_jvalarr(guest_mem *m, handle_table *ht, const char *sig, uint32_t ap, ab_jval *out, int maxn){
    const char *p = params_of(sig); int n = 0; char t;
    while ((t = nextp(&p)) && n < maxn){
        uint32_t cell = ap + (uint32_t)n * 8u;   /* jvalue stride = 8 (guest LP32 union carries jlong/jdouble) */
        if (t == 'J' || t == 'D'){
            uint32_t lo = gm_rd32(m, cell), hi = gm_rd32(m, cell + 4);
            uint64_t v = (uint64_t)lo | ((uint64_t)hi << 32);
            if (t == 'J') out[n].j = (int64_t)v; else memcpy(&out[n].d, &v, 8);
        } else if (t == 'F'){                    /* natural float (NOT promoted) in the low word */
            uint32_t v = gm_rd32(m, cell); memcpy(&out[n].f, &v, 4);
        } else {
            put_word(&out[n], t, gm_rd32(m, cell), ht);
        }
        n++;
    }
    return n;
}
