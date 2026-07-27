/* descriptors.h — the ONE per-symbol signature descriptor format (Audit 01/09).
 *
 * Every value crossing the guest(ARM32 soft-float, LP32) <-> host(AAPCS64)
 * boundary is described by a `mdesc` and marshalled by the single arg-walker +
 * return-writer in marshal.c. No bridge handler reads guest registers by hand.
 * The SAME format serves libc, GL and JNI (static instances) and dynamically
 * parsed JNI method signatures. This header defines only the data; marshal.c
 * does placement; the bridge layer interprets `dir`/`lensrc` for pointer copies. */
#ifndef ABSHIM_DESCRIPTORS_H
#define ABSHIM_DESCRIPTORS_H
#include <stdint.h>

#define MDESC_MAXARGS 12

/* Argument/return atoms. Only the 32-bit vs 64-bit distinction affects
 * placement (Audit 09: base soft-float variant passes FP in core regs, so F32
 * places like I32 and F64 like I64); the atom identity drives the bridge's
 * value interpretation (pointer resolve, float re-widen to VFP at the host
 * call, sign/zero extension of sub-word returns). */
enum matom {
    A_VOID = 0,
    A_I32,      /* signed 32   -> r0 (return: sign-extended by handler) */
    A_U32,      /* unsigned 32 */
    A_PTR,      /* guest pointer (opaque) */
    A_STR,      /* guest char* (NUL-bounded) */
    A_SIZE,     /* size_t (u32 on LP32) */
    A_OFF,      /* off_t  (i32 on LP32, no LFS) */
    A_I64,      /* signed 64  -> r0:r1, 8-aligned reg-pair or stack */
    A_U64,      /* unsigned 64 */
    A_F32,      /* float  (1 core reg bit-pattern) */
    A_F64,      /* double (r0:r1 bit-pattern, 8-aligned) */
    A_VA,       /* va_list pointer (a word) */
    A_NATOM
};

/* Pointer direction (bridge copy semantics; ignored by the placement walker). */
enum mdir { D_NONE = 0, D_IN, D_OUT, D_INOUT, D_STR, D_OPAQUE };

/* Where a pointer's copy length comes from (bridge layer). */
enum mlensrc { L_NONE = 0, L_CONST, L_ARG, L_COMPUTED };

typedef struct { uint8_t atom, dir, lensrc, lenarg; } marg;

typedef struct {
    uint8_t  ret;        /* enum matom */
    uint8_t  nargs;
    uint8_t  kind;       /* LIBC|GL|AEABI|JNI_ENV|JNI_VM|SPECIAL (bridge dispatch) */
    uint8_t  flags;      /* bit0 = blocking (stop/restart), bit1 = variadic tail */
    uint16_t handler;    /* handler index / gl-symbol index */
    marg     args[MDESC_MAXARGS];
} mdesc;

#define MDF_BLOCKING  0x01u
#define MDF_VARIADIC  0x02u

static inline int matom_is64(uint8_t a){ return a==A_I64 || a==A_U64 || a==A_F64; }

#endif /* ABSHIM_DESCRIPTORS_H */
