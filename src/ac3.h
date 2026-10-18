/* AC-3 and E-AC-3 (Dolby Digital, Dolby Digital Plus) decoding (ac3.c), written
 * from ATSC A/52 and ETSI TS 102 366. */
#ifndef FA_AC3_H
#define FA_AC3_H

#include "fa_internal.h"

/* Dynamic range control applied by the decoder (fa_dynamic_range). */
enum { FA_AC3_DRC_OFF = 0, FA_AC3_DRC_NORMAL, FA_AC3_DRC_NIGHT };

/* The fixed part of a syncframe's header. */
typedef struct fa_ac3_header {
    int eac3;              /* bsid 11..16 */
    int bsid;
    size_t frame_size;     /* bytes */
    uint32_t rate;
    int blocks;            /* 256 samples each: 1, 2, 3 or 6 */
    int strmtyp;           /* E-AC-3: 0 independent, 1 dependent, 2 converted from AC-3 */
    int substreamid;
    int acmod, lfeon;
    int channels;          /* full-bandwidth channels plus LFE */
} fa_ac3_header;

/* Parses the header at p (n bytes, 8 are enough). 0 if it is not a syncframe. */
int fa_ac3_parse_header(const uint8_t* p, size_t n, fa_ac3_header* out);

/* An access unit starts at an AC-3 frame or an E-AC-3 independent frame of program 0. */
static inline int fa_ac3_starts_unit(const fa_ac3_header* h)
{
    return !h->eac3 || (h->strmtyp != 1 && h->substreamid == 0);
}

typedef struct fa_ac3 fa_ac3;

fa_ac3* fa_ac3_create(void);
void fa_ac3_destroy(fa_ac3* d);
/* Forgets the overlap of the previous frames (after a seek). */
void fa_ac3_reset(fa_ac3* d);

/* Decodes one access unit: an AC-3 syncframe, or an E-AC-3 independent frame and
 * the dependent frames that follow it (data may hold several of them; frames of
 * other programs, substreamid > 0, are skipped). Writes interleaved samples to
 * *pcm (grown as needed; *cap counts floats) in the default speaker order for
 * *channels (see ac3.c). FA_ERROR_GENERIC: damaged data (*frames still holds the
 * access unit's length, of silence). */
fa_result fa_ac3_decode(fa_ac3* d, const uint8_t* data, size_t size, int drc,
                        float** pcm, size_t* cap, int* frames, int* channels, uint32_t* rate);

#endif
