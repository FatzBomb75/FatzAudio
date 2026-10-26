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

#endif
