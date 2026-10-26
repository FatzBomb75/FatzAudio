/* Small helpers shared across the library, with no dependencies of their own (the
 * surface code uses them without the rest of fa_internal.h). */
#ifndef FA_UTIL_H
#define FA_UTIL_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Byte-order readers. */
static inline uint16_t fa_be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t fa_be24(const uint8_t* p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }
static inline uint32_t fa_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline uint64_t fa_be64(const uint8_t* p) { return ((uint64_t)fa_be32(p) << 32) | fa_be32(p + 4); }
static inline uint16_t fa_le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t fa_le32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t fa_le64(const uint8_t* p) { return (uint64_t)fa_le32(p) | ((uint64_t)fa_le32(p + 4) << 32); }
#define FA_PI 3.14159265358979323846
#define FA_FOURCC(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/* malloc'd copy of the first `n` bytes of `s`, plus a terminator. */
static inline char* fa_strndup(const char* s, size_t n)
{
    char* d = (char*)malloc(n + 1);
    if (d) { memcpy(d, s, n); d[n] = '\0'; }
    return d;
}
/* malloc'd copy of all of `s`; NULL for NULL. */
static inline char* fa_strdup(const char* s) { return s ? fa_strndup(s, strlen(s)) : NULL; }

/* 1 if the `n` bytes at `s` are well-formed UTF-8 (RFC 3629): no overlong forms,
 * UTF-16 surrogates, code points above U+10FFFF or cut-off characters. Text that
 * fails is read in a legacy code page by the caller. */
static inline int fa_utf8_valid(const void* s, size_t n)
{
    const uint8_t* p = (const uint8_t*)s;
    size_t i = 0, k, len;
    while (i < n) {
        uint8_t c = p[i];
        if (c < 0x80) { i++; continue; }
        if (c >= 0xC2 && c <= 0xDF) len = 2;
        else if (c >= 0xE0 && c <= 0xEF) len = 3;
        else if (c >= 0xF0 && c <= 0xF4) len = 4;
        else return 0;
        if (n - i < len) return 0;
        for (k = 1; k < len; k++) if ((p[i + k] & 0xC0) != 0x80) return 0;
        if ((c == 0xE0 && p[i + 1] < 0xA0) || (c == 0xED && p[i + 1] > 0x9F) ||
            (c == 0xF0 && p[i + 1] < 0x90) || (c == 0xF4 && p[i + 1] > 0x8F)) return 0;
        i += len;
    }
    return 1;
}

/* Writes code point `u` (up to U+10FFFF) as UTF-8; returns the bytes written (1-4). */
static inline size_t fa_put_utf8(char* out, uint32_t u)
{
    if (u < 0x80) { out[0] = (char)u; return 1; }
    if (u < 0x800) { out[0] = (char)(0xC0 | (u >> 6)); out[1] = (char)(0x80 | (u & 63)); return 2; }
    if (u < 0x10000) {
        out[0] = (char)(0xE0 | (u >> 12)); out[1] = (char)(0x80 | ((u >> 6) & 63)); out[2] = (char)(0x80 | (u & 63));
        return 3;
    }
    out[0] = (char)(0xF0 | (u >> 18)); out[1] = (char)(0x80 | ((u >> 12) & 63));
    out[2] = (char)(0x80 | ((u >> 6) & 63)); out[3] = (char)(0x80 | (u & 63));
    return 4;
}

#endif
