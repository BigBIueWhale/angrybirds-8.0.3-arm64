/* galloc.h — the ONE guest-heap allocator (Audit 05).
 *
 * A real coalescing boundary-tag allocator managing the fixed guest HEAP arena,
 * with inline chunk headers stored IN guest memory. Replaces the bump `balloc`
 * + no-op `free` of the legacy shim, which — since the game heap is bionic's
 * behind our malloc bridge (Audit 05 pivot: even C++ new/delete tail-call the
 * UND malloc/free) — leaked every freed/deleted block to OOM (M1/M11).
 *
 * Guarantees (Audit 05 invariants M8/M9/M2/M10/M3):
 *   - payloads are >= 16-byte aligned;
 *   - free() actually frees and coalesces (O(1) via boundary tags);
 *   - realloc copies min(old,new) — never over-reads the old block (M2) — and
 *     grows in place into an adjacent free chunk when possible;
 *   - calloc checks nmemb*size overflow (M10);
 *   - malloc(0) returns a unique non-NULL 16-byte chunk;
 *   - malloc/calloc/realloc return 0 (NULL) on arena exhaustion (M9) — never
 *     bump past the arena — so the guest's operator-new / bad_alloc / 512-byte
 *     emergency-exception pool engage gracefully instead of faulting.
 * Runs under the Big-Emulator-Lock (Audit 02) so it needs no internal lock. */
#ifndef ABSHIM_GALLOC_H
#define ABSHIM_GALLOC_H
#include "memops.h"

typedef struct galloc galloc;

/* Create an allocator over the guest region [base, base+size).
 * `base` must be 16-aligned; `size` must be a multiple of 16 and >= 64.
 * Returns a host-side handle (allocated with the C library — NOT guest mem). */
galloc  *galloc_create(guest_mem *m, uint32_t base, uint32_t size);
void     galloc_destroy(galloc *a);

uint32_t galloc_malloc (galloc *a, uint32_t n);              /* 0 on exhaustion */
void     galloc_free   (galloc *a, uint32_t p);             /* p==0 => no-op */
void     galloc_note_free_lr(uint32_t lr);                  /* WAF-detector: engine-rel free-site (guest LR) for the next galloc_free */
uint32_t galloc_realloc(galloc *a, uint32_t p, uint32_t n); /* see header note */
uint32_t galloc_calloc (galloc *a, uint32_t nmemb, uint32_t size);
uint32_t galloc_usable (galloc *a, uint32_t p);            /* payload capacity */

/* Use-after-free quarantine (OFF by default). Deferring the real reclamation of freed blocks by `n`
 * operations keeps a prematurely-freed block's bytes intact across the window a stale reference reads it,
 * neutralizing the engine's std::string _Rep use-after-free. The production shim enables it; galloc_flush
 * drains all held blocks (restores immediate-reclamation semantics for leak/coalesce assertions). */
void     galloc_set_quarantine(galloc *a, uint32_t n);
void     galloc_flush  (galloc *a);

/* Debug/verification: full heap walk + free-list check.
 * Returns 0 if the heap is well-formed, else a negative error code (see .c).
 * Used by the host torture test after every operation. */
int      galloc_check  (galloc *a);
/* Same walk, but stores the offending chunk address in *bad (0 if the failure is not
 * chunk-local). Used to aim a write-watchpoint at whatever is clobbering chunk headers. */
int      galloc_check_where(galloc *a, uint32_t *bad);

/* Test introspection: bytes currently handed out (sum of in-use payloads). */
uint32_t galloc_inuse_bytes(galloc *a);

#ifdef GALLOC_DEBUG
int galloc_dbg_rpath(void);   /* last realloc path id (debug builds only) */
#endif

#endif /* ABSHIM_GALLOC_H */
