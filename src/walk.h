/* The container element walkers shared by the demuxers and the tag reader: EBML
 * (Matroska, WebM) and ISO base media boxes (MP4, MOV, M4A). Each format's header
 * is decoded in one place; the walks in memory clamp an element that runs past its
 * parent to what is there, so a cut file keeps its last element. */
#ifndef FA_WALK_H
#define FA_WALK_H

#include <stddef.h>
#include <stdint.h>

/* ---- EBML (RFC 8794) ------------------------------------------------------------- */

/* Length of a variable-length integer from its first byte: 1..8, 9 when invalid. */
int fa_ebml_vint_len(uint8_t first);

/* An element header in p[0..n): its ID (with the length marker bits, as written)
 * and data size, -1 when unknown (all ones). The header length, 0 when the bytes
 * hold no complete header (IDs are at most 4 bytes). */
size_t fa_ebml_header(const uint8_t* p, size_t n, uint32_t* id, int64_t* size);

typedef struct fa_ebml_el {
    uint32_t id;
    const uint8_t* p;       /* data */
    size_t len;
} fa_ebml_el;

/* The next element in [*cur, end): an unknown size, or one past `end`, runs to `end`. */
int fa_ebml_next(const uint8_t** cur, const uint8_t* end, fa_ebml_el* e);
/* An unsigned integer element (big-endian, at most 8 bytes read). */
uint64_t fa_ebml_uint(const fa_ebml_el* e);

/* ---- ISO base media boxes (ISO/IEC 14496-12 4.2) ----------------------------------- */

/* A box header in h[0..n): its type, size (0: to the end of its parent) and header
 * length (8, or 16 with a 64-bit size). 0 when the bytes hold no complete header
 * or the size is smaller than the header. */
int fa_mp4_box_header(const uint8_t* h, size_t n, uint32_t* type, uint64_t* size, int* hdr);

typedef struct fa_mp4_box {
    uint32_t type;
    const uint8_t* p;       /* body */
    size_t len;
} fa_mp4_box;

/* The next box in [*cur, end): size 0, or one past `end`, runs to `end`. */
int fa_mp4_next_box(const uint8_t** cur, const uint8_t* end, fa_mp4_box* b);

#endif
