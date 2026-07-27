/* format.h — the ONE printf/scanf varargs engine (Audit 06 L4).
 *
 * A single formatter core with two argument sources (Audit 09):
 *   A) non-`v` variants — the bridge IS the variadic function, so the variadic
 *      tail is pulled by resuming the marshal cursor (regs then 8-aligned stack);
 *   B) `v`-variants — walk a guest `va_list` pointer through guest memory.
 * Both obey the same soft-float promotion rules: `%f/%e/%g` pull a *double* (2
 * words, 8-aligned) out of the CORE-register/stack integer sequence — never a
 * VFP register (the crux of the soft-float ABI); `%lld/%llx/%j` pull 2 words.
 *
 * Per-conversion formatting is delegated to the host libc (the formatter runs as
 * host C over guest memory), which is correct-by-construction and lets the host
 * `snprintf` double as the test oracle. The guest length modifiers are NOT copied
 * verbatim (guest `l` = 32-bit LP32 vs host `l` = 64-bit LP64); the host length
 * is reconstructed from the pulled width. */
#ifndef ABSHIM_FORMAT_H
#define ABSHIM_FORMAT_H
#include "marshal.h"

/* Source A -> guest buffer. `cap` is the snprintf size (bytes incl NUL); use
 * 0xFFFFFFFF for sprintf. `cur` must be positioned AFTER the fixed args
 * (e.g. after buf/fmt for sprintf, buf/size/fmt for snprintf). Returns the C
 * return value: the number of chars that WOULD have been written (excl NUL). */
uint32_t fmt_to_guest(guest_mem *m, uint32_t out, uint32_t cap, uint32_t fmt, mcur cur);

/* Source B (v-variants) -> guest buffer, walking guest va_list `va`. */
uint32_t fmt_to_guest_va(guest_mem *m, uint32_t out, uint32_t cap, uint32_t fmt, uint32_t va);

/* printf/fprintf: format into a host buffer (the bridge logs/writes it).
 * `*outlen` receives the NUL-terminated host byte count. Returns the C value. */
uint32_t fmt_to_host(guest_mem *m, char *hbuf, uint32_t hcap, uint32_t fmt, mcur cur, uint32_t *outlen);

#endif /* ABSHIM_FORMAT_H */
