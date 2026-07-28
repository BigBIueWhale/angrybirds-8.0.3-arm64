/* test_gl_sizes.c — the GL scratch-size contract.
 *
 * WHY THIS EXISTS
 * ---------------
 * bridge_gl computes the size of a host scratch buffer from guest-supplied values, and hands the
 * SAME guest values to the GL driver. If the product overflows 32 bits the buffer is small while
 * the driver still receives the real width/height/count and reads past the end of it. gl_gsize3
 * exists to refuse those calls.
 *
 * The first version of gl_gsize3 folded "overflow" and "the product is legitimately zero" into one
 * return value, which would have silently dropped 0-width and 0-height textures - legal GL that
 * previously reached the driver. That was caught by reading the code, not by a test, because
 * nothing tested the GL bridge at all. This is that test.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

int gl_gsize3(uint64_t a, uint64_t b, uint64_t c, uint32_t *out);

static int fails = 0;
static void ck(const char *what, int got, int want){
    if (got != want){ printf("  FAIL %-46s got %d want %d\n", what, got, want); fails++; }
    else printf("  ok   %s\n", what);
}
static void cksz(const char *what, uint32_t got, uint32_t want){
    if (got != want){ printf("  FAIL %-46s got %u want %u\n", what, got, want); fails++; }
    else printf("  ok   %s\n", what);
}

int main(void){
    uint32_t n;
    printf("=== GL scratch-size contract ===\n");

    n = 0xdead;
    ck  ("ordinary texture 256x256x4 accepted",   gl_gsize3(256, 4, 256, &n), 1);
    cksz("  ...and sized correctly",              n, 256u*4u*256u);

    /* The regression that was nearly shipped: a zero-sized call is LEGAL and must be accepted,
     * distinguishably from a refusal. */
    n = 0xdead;
    ck  ("zero width accepted (legal GL)",        gl_gsize3(0, 4, 256, &n), 1);
    cksz("  ...with size 0",                      n, 0u);
    n = 0xdead;
    ck  ("zero height accepted (legal GL)",       gl_gsize3(256, 4, 0, &n), 1);
    cksz("  ...with size 0",                      n, 0u);

    /* Products that would overflow 32 bits must be refused, not truncated. */
    n = 0xdead;
    ck  ("2^16 * 2^16 * 4 refused (overflows)",   gl_gsize3(65536, 65536, 4, &n), 0);
    ck  ("0xffffffff * 2 * 2 refused",            gl_gsize3(0xffffffffu, 2, 2, &n), 0);

    /* Exactly at, and just past, the cap. */
    ck  ("64 MB exactly is accepted",             gl_gsize3(64u*1024u*1024u, 1, 1, &n), 1);
    ck  ("64 MB + 1 refused",                     gl_gsize3(64u*1024u*1024u + 1u, 1, 1, &n), 0);

    /* A uniform-matrix count that overflows count*16*4. */
    ck  ("uniform count 2^28 refused (x64)",      gl_gsize3(1u<<28, 16, 4, &n), 0);
    ck  ("uniform count 64 accepted",             gl_gsize3(64, 16, 4, &n), 1);
    cksz("  ...sized 4096",                       n, 64u*16u*4u);

    printf(fails ? "\n=== %d FAILURES ===\n" : "\n=== ALL PASS ===\n", fails);
    return fails ? 1 : 0;
}
