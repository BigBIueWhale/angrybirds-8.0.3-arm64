/* handle_table.h — the JNI 32<->64 handle table (Audit 08 Determination 3 / J1).
 *
 * The guest is LP32; every jobject/jclass/jstring/jarray/jthrowable/jmethodID/
 * jfieldID is a 64-bit host pointer. A real ref/ID can never be handed to the
 * guest — it crosses ONLY as a 32-bit kind-tagged token:
 *     token = (kind << 28) | index ,  token 0 <=> real NULL.
 * Two entry classes: object refs (local/global/weak — a fresh token per real ref;
 * NOT deduped, since JNI does not guarantee ref pointer-identity — IsSameObject
 * forwards to the real env) and IDs (methodID/fieldID — permanent + deduped by
 * real pointer, carrying a parsed signature descriptor).
 *
 * Lifetime is kept in lockstep with the real ref by the JNI bridge (Phase D):
 * locals are activation-scoped via a per-GEC frame stack (freed at thunk return
 * + on explicit DeleteLocalRef); globals persist until DeleteGlobalRef. This
 * module is pure host bookkeeping — it never makes real JNI calls; it maps
 * tokens<->real pointers and manages per-kind index allocation with reuse. */
#ifndef ABSHIM_HANDLE_TABLE_H
#define ABSHIM_HANDLE_TABLE_H
#include <stdint.h>

typedef struct handle_table handle_table;

#define HT_NULL 0u
enum { HK_NULL=0, HK_LOCAL=1, HK_GLOBAL=2, HK_WEAK=3, HK_ID=4 };
static inline uint32_t ht_kind (uint32_t tok){ return tok >> 28; }
static inline uint32_t ht_index(uint32_t tok){ return tok & 0x0FFFFFFFu; }

handle_table *ht_create (void);
void          ht_destroy(handle_table *t);

/* Object refs. kind in {HK_LOCAL,HK_GLOBAL,HK_WEAK}. real==NULL -> HT_NULL.
 * A fresh token each call (no dedup). Locals are also pushed on the frame stack. */
uint32_t ht_new_ref   (handle_table *t, int kind, void *real);
/* token -> real pointer; NULL for HT_NULL or a stale/invalid token. */
void    *ht_resolve   (handle_table *t, uint32_t tok);
/* Free a ref's slot (DeleteLocalRef/DeleteGlobalRef/DeleteWeakGlobalRef).
 * Returns 1 if a live slot was freed, 0 otherwise. The caller makes the matching
 * real JNI call. */
int      ht_delete_ref(handle_table *t, uint32_t tok);

/* IDs — deduped by real pointer: the same real ID always returns the same token.
 * `desc` (opaque, e.g. the parsed method signature) is stored on first intern. */
uint32_t     ht_intern_id(handle_table *t, void *real_id, const void *desc);
const void  *ht_id_desc  (handle_table *t, uint32_t tok);   /* NULL if not an id token */

/* Local-ref frame (per activation; the JNI bridge holds one per GEC). */
uint32_t ht_frame_mark(handle_table *t);                    /* current local depth */
void     ht_frame_pop (handle_table *t, uint32_t mark);     /* free locals created since mark */

/* Test introspection: number of live slots of a kind. */
uint32_t ht_live(handle_table *t, int kind);

#endif /* ABSHIM_HANDLE_TABLE_H */
