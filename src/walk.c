/* Container element walkers (walk.h), written from RFC 8794 (EBML) and
 * ISO/IEC 14496-12 (boxes). */
#include "walk.h"
#include "fa_util.h"

int fa_ebml_vint_len(uint8_t first)
{
    int n = 1;
    while (n <= 8 && !(first & (0x80 >> (n - 1)))) n++;
    return n;
}

size_t fa_ebml_header(const uint8_t* p, size_t n, uint32_t* id, int64_t* size)
{
    size_t il, sl, i;
    uint64_t s;
    int unknown = 1;
    if (n < 2) return 0;
    il = (size_t)fa_ebml_vint_len(p[0]);
    if (il > 4 || il >= n) return 0;
    *id = 0;
    for (i = 0; i < il; i++) *id = (*id << 8) | p[i];
    sl = (size_t)fa_ebml_vint_len(p[il]);
    if (sl > 8 || il + sl > n) return 0;
    s = p[il] & (0xFFu >> sl);
    if (s != (0xFFu >> sl)) unknown = 0;
    for (i = 1; i < sl; i++) {
        if (p[il + i] != 0xFF) unknown = 0;
        s = (s << 8) | p[il + i];
    }
    *size = unknown ? -1 : (int64_t)s;
    return il + sl;
}

int fa_ebml_next(const uint8_t** cur, const uint8_t* end, fa_ebml_el* e)
{
    const uint8_t* p = *cur;
    size_t n = (size_t)(end - p), hl;
    int64_t size;
    hl = fa_ebml_header(p, n, &e->id, &size);
    if (hl == 0) return 0;
    n -= hl;
    if (size < 0 || (uint64_t)size > n) size = (int64_t)n;   /* unknown or cut short: to the end of the parent */
    e->p = p + hl;
    e->len = (size_t)size;
    *cur = e->p + size;
    return 1;
}

uint64_t fa_ebml_uint(const fa_ebml_el* e)
{
    uint64_t v = 0;
    size_t i;
    for (i = 0; i < e->len && i < 8; i++) v = (v << 8) | e->p[i];
    return v;
}

int fa_mp4_box_header(const uint8_t* h, size_t n, uint32_t* type, uint64_t* size, int* hdr)
{
    uint64_t s;
    if (n < 8) return 0;
    s = fa_be32(h);
    *type = fa_be32(h + 4);
    *hdr = 8;
    if (s == 1) {
        if (n < 16) return 0;
        s = fa_be64(h + 8);
        *hdr = 16;
        if (s < 16) return 0;   /* "to the end" is only said by a 32-bit 0 */
    }
    if (s != 0 && s < (uint64_t)*hdr) return 0;
    *size = s;
    return 1;
}

int fa_mp4_next_box(const uint8_t** cur, const uint8_t* end, fa_mp4_box* b)
{
    size_t n = (size_t)(end - *cur);
    uint64_t size;
    int hdr;
    if (!fa_mp4_box_header(*cur, n, &b->type, &size, &hdr)) return 0;
    if (size == 0 || size > n) size = n;   /* to the end, or cut short: keep what is there */
    b->p = *cur + hdr;
    b->len = (size_t)size - (size_t)hdr;
    *cur += size;
    return 1;
}
