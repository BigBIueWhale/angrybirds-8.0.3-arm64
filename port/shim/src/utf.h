/* utf.h — UTF-8 and JNI modified-UTF-8 codecs (Audit 06 mb/wc, Audit 08 R-J2).
 *
 * Standard UTF-8 (utf8_*) backs the libc mb/wc bridges (mbrtowc/wcrtomb/…): it
 * rejects overlong forms, surrogate code points (U+D800..U+DFFF) and values
 * above U+10FFFF. JNI modified-UTF-8 (mutf8_*) is the CESU-8-style encoding the
 * JVM uses: U+0000 is encoded as C0 80 (so strings never contain a real NUL),
 * there is no 4-byte form (supplementary chars are a surrogate PAIR, each a
 * 3-byte unit). Pure host byte-buffer logic. */
#ifndef ABSHIM_UTF_H
#define ABSHIM_UTF_H
#include <stdint.h>
#include <stddef.h>

/* Decode one standard-UTF-8 sequence at s[0..avail). On success writes the code
 * point to *cp and returns the byte length (1..4); returns -1 on malformed or
 * incomplete input (overlong, surrogate, > U+10FFFF, bad continuation). */
int utf8_decode(const uint8_t *s, size_t avail, uint32_t *cp);

/* Encode code point cp as standard UTF-8 into out[0..4). Returns the byte length
 * (1..4), or -1 if cp is a surrogate or > U+10FFFF. */
int utf8_encode(uint32_t cp, uint8_t out[4]);

/* Decode one modified-UTF-8 unit -> a UTF-16 code unit (0..0xFFFF, possibly a
 * surrogate half). Returns byte length (1..3) or -1. C0 80 decodes to 0. */
int mutf8_decode_unit(const uint8_t *s, size_t avail, uint32_t *u16);

/* Encode a UTF-16 code unit (0..0xFFFF) as modified-UTF-8 into out[0..3).
 * Returns byte length (1..3): 0 -> C0 80. */
int mutf8_encode_unit(uint32_t u16, uint8_t out[3]);

/* Validate a NUL-terminated-or-length-bounded modified-UTF-8 string. Returns the
 * number of UTF-16 units on success, or -1 if malformed. If n==(size_t)-1 the
 * string is read until a real 0 byte (which mUTF-8 never contains internally). */
long mutf8_validate(const uint8_t *s, size_t n);

#endif /* ABSHIM_UTF_H */
