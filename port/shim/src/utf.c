/* utf.c — UTF-8 and modified-UTF-8 codecs. See utf.h. */
#include "utf.h"

int utf8_decode(const uint8_t *s, size_t avail, uint32_t *cp){
    if (avail < 1) return -1;
    uint8_t b0 = s[0];
    if (b0 < 0x80){ *cp = b0; return 1; }
    if ((b0 & 0xE0) == 0xC0){                          /* 2 bytes */
        if (avail < 2 || (s[1] & 0xC0) != 0x80) return -1;
        uint32_t c = ((uint32_t)(b0 & 0x1F) << 6) | (s[1] & 0x3F);
        if (c < 0x80) return -1;                       /* overlong */
        *cp = c; return 2;
    }
    if ((b0 & 0xF0) == 0xE0){                          /* 3 bytes */
        if (avail < 3 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return -1;
        uint32_t c = ((uint32_t)(b0 & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        if (c < 0x800) return -1;                      /* overlong */
        if (c >= 0xD800 && c <= 0xDFFF) return -1;     /* surrogate */
        *cp = c; return 3;
    }
    if ((b0 & 0xF8) == 0xF0){                          /* 4 bytes */
        if (avail < 4 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) return -1;
        uint32_t c = ((uint32_t)(b0 & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12)
                   | ((uint32_t)(s[2] & 0x3F) << 6)  | (s[3] & 0x3F);
        if (c < 0x10000 || c > 0x10FFFF) return -1;    /* overlong or out of range */
        *cp = c; return 4;
    }
    return -1;
}

int utf8_encode(uint32_t cp, uint8_t out[4]){
    if (cp < 0x80){ out[0] = (uint8_t)cp; return 1; }
    if (cp < 0x800){ out[0] = 0xC0 | (cp >> 6); out[1] = 0x80 | (cp & 0x3F); return 2; }
    if (cp >= 0xD800 && cp <= 0xDFFF) return -1;
    if (cp < 0x10000){ out[0] = 0xE0 | (cp >> 12); out[1] = 0x80 | ((cp >> 6) & 0x3F); out[2] = 0x80 | (cp & 0x3F); return 3; }
    if (cp <= 0x10FFFF){ out[0] = 0xF0 | (cp >> 18); out[1] = 0x80 | ((cp >> 12) & 0x3F); out[2] = 0x80 | ((cp >> 6) & 0x3F); out[3] = 0x80 | (cp & 0x3F); return 4; }
    return -1;
}

int mutf8_decode_unit(const uint8_t *s, size_t avail, uint32_t *u16){
    if (avail < 1) return -1;
    uint8_t b0 = s[0];
    if (b0 >= 0x01 && b0 <= 0x7F){ *u16 = b0; return 1; }
    if ((b0 & 0xE0) == 0xC0){                           /* 2 bytes (incl C0 80 -> 0) */
        if (avail < 2 || (s[1] & 0xC0) != 0x80) return -1;
        *u16 = ((uint32_t)(b0 & 0x1F) << 6) | (s[1] & 0x3F); return 2;
    }
    if ((b0 & 0xF0) == 0xE0){                           /* 3 bytes (may be a surrogate half) */
        if (avail < 3 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return -1;
        *u16 = ((uint32_t)(b0 & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F); return 3;
    }
    return -1;                                          /* mUTF-8 has no 4-byte form */
}

int mutf8_encode_unit(uint32_t u16, uint8_t out[3]){
    if (u16 != 0 && u16 <= 0x7F){ out[0] = (uint8_t)u16; return 1; }
    if (u16 == 0 || u16 <= 0x7FF){ out[0] = 0xC0 | (u16 >> 6); out[1] = 0x80 | (u16 & 0x3F); return 2; }  /* 0 -> C0 80 */
    out[0] = 0xE0 | (u16 >> 12); out[1] = 0x80 | ((u16 >> 6) & 0x3F); out[2] = 0x80 | (u16 & 0x3F); return 3;
}

long mutf8_validate(const uint8_t *s, size_t n){
    long units = 0; size_t i = 0;
    for (;;){
        if (n == (size_t)-1){ if (s[i] == 0) break; }
        else if (i >= n) break;
        size_t avail = (n == (size_t)-1) ? 3 : (n - i);   /* bounded read for decode */
        uint32_t u;
        int k = mutf8_decode_unit(s + i, avail, &u);
        if (k < 0) return -1;
        i += k; units++;
    }
    return units;
}
