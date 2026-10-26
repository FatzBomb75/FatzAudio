/* AAC decoding (aac.c, aac_sbr.c, aac_ps.c), written from ISO/IEC 14496-3
 * (the AAC-LC core, SBR and parametric stereo) and 13818-7 (ADTS). */
#ifndef FA_AAC_H
#define FA_AAC_H

#include "fa_internal.h"

#define FA_AAC_FRAME 1024          /* core samples per raw data block */

typedef struct fa_aac fa_aac;

fa_aac* fa_aac_create(void);
void fa_aac_destroy(fa_aac* d);
/* Forgets the overlap and SBR history (after a seek). */
void fa_aac_reset(fa_aac* d);
/* Sets the stream up from an AudioSpecificConfig (MP4 'esds', Matroska
 * CodecPrivate). Without one, each packet must be an ADTS frame.
 * FA_ERROR_UNSUPPORTED_FORMAT: an object type this decoder does not have. */
fa_result fa_aac_set_config(fa_aac* d, const uint8_t* asc, size_t n);
/* Decodes one packet: an ADTS frame (header included, any number of raw data
 * blocks), or a raw access unit after fa_aac_set_config. Writes interleaved
 * samples to *pcm (grown as needed; *cap counts floats) in the default speaker
 * order for *channels (see aac.c): 1024 frames per raw data block, 2048 with SBR.
 * FA_ERROR_UNSUPPORTED_FORMAT: a feature this decoder does not have; FA_ERROR_GENERIC:
 * damaged data. Either way *frames is 0 if nothing was decoded yet, else the
 * packet's length of silence. */
fa_result fa_aac_decode(fa_aac* d, const uint8_t* data, size_t size, float** pcm, size_t* cap, int* frames,
                        int* channels, uint32_t* rate);

/* ---- shared by the decoder's parts ---------------------------------------------- */

/* MSB-first bit reader over a buffer with at least 12 readable bytes past `end`
 * (aac.c pads its copy of each packet), so peeks never check bounds. */
typedef struct fa_aac_bits {
    const uint8_t* p;
    size_t pos;           /* bits read */
    size_t end;           /* bits in the data */
} fa_aac_bits;

static inline uint32_t fa_aac_peek(const fa_aac_bits* b, int n)   /* n <= 25 */
{
    const uint8_t* q = b->p + (b->pos >> 3);
    uint32_t v = fa_be32(q);
    return n ? (v << (b->pos & 7)) >> (32 - n) : 0;
}

static inline void fa_aac_skip(fa_aac_bits* b, size_t n)
{
    b->pos += n;
    if (b->pos > b->end + 64) b->pos = b->end + 64;   /* stays inside the padding */
}

static inline uint32_t fa_aac_get(fa_aac_bits* b, int n)          /* n <= 25 */
{
    uint32_t v = fa_aac_peek(b, n);
    fa_aac_skip(b, (size_t)n);
    return v;
}

static inline int fa_aac_left(const fa_aac_bits* b)
{
    return b->pos <= b->end ? (int)(b->end - b->pos) : -1;
}

/* One Huffman symbol from a lookup table of aac_tables.c: an entry is a leaf,
 * (length << 9) | symbol with the length counted from the start of its level,
 * or a link to the next level, 0x8000 | (offset << 3) | (bits - 1). The root
 * level has `root` bits. Returns -1 on a code outside the table. */
static inline int fa_aac_vlc(fa_aac_bits* b, const uint16_t* t, int root)
{
    uint32_t e = t[fa_aac_peek(b, root)];
    while (e & 0x8000u) {
        int bits = (int)(e & 7) + 1;
        fa_aac_skip(b, (size_t)root);
        root = bits;
        e = t[((e >> 3) & 0xFFF) + fa_aac_peek(b, bits)];
    }
    if ((e >> 9) == 0) return -1;
    fa_aac_skip(b, e >> 9);
    return (int)(e & 0x1FF);
}

#define FA_AAC_VLC_ROOT 7

/* aac_tables.c */
extern const uint16_t fa_aac_hcb1[];
extern const uint16_t fa_aac_hcb2[];
extern const uint16_t fa_aac_hcb3[];
extern const uint16_t fa_aac_hcb4[];
extern const uint16_t fa_aac_hcb5[];
extern const uint16_t fa_aac_hcb6[];
extern const uint16_t fa_aac_hcb7[];
extern const uint16_t fa_aac_hcb8[];
extern const uint16_t fa_aac_hcb9[];
extern const uint16_t fa_aac_hcb10[];
extern const uint16_t fa_aac_hcb11[];
extern const uint16_t fa_aac_hcb_sf[];
extern const uint16_t fa_aac_sbr_t_huffman_env_1_5db[];
extern const uint16_t fa_aac_sbr_f_huffman_env_1_5db[];
extern const uint16_t fa_aac_sbr_t_huffman_env_bal_1_5db[];
extern const uint16_t fa_aac_sbr_f_huffman_env_bal_1_5db[];
extern const uint16_t fa_aac_sbr_t_huffman_env_3_0db[];
extern const uint16_t fa_aac_sbr_f_huffman_env_3_0db[];
extern const uint16_t fa_aac_sbr_t_huffman_env_bal_3_0db[];
extern const uint16_t fa_aac_sbr_f_huffman_env_bal_3_0db[];
extern const uint16_t fa_aac_sbr_t_huffman_noise_3_0db[];
extern const uint16_t fa_aac_sbr_t_huffman_noise_bal_3_0db[];
extern const uint16_t fa_aac_ps_huff_iid_def_f[];
extern const uint16_t fa_aac_ps_huff_iid_def_t[];
extern const uint16_t fa_aac_ps_huff_iid_fine_f[];
extern const uint16_t fa_aac_ps_huff_iid_fine_t[];
extern const uint16_t fa_aac_ps_huff_icc_f[];
extern const uint16_t fa_aac_ps_huff_icc_t[];
extern const uint16_t fa_aac_ps_huff_ipd_f[];
extern const uint16_t fa_aac_ps_huff_ipd_t[];
extern const uint16_t fa_aac_ps_huff_opd_f[];
extern const uint16_t fa_aac_ps_huff_opd_t[];
extern const uint16_t fa_aac_swb_long_96[42];
extern const uint16_t fa_aac_swb_long_64[48];
extern const uint16_t fa_aac_swb_long_48[50];
extern const uint16_t fa_aac_swb_long_32[52];
extern const uint16_t fa_aac_swb_long_24[48];
extern const uint16_t fa_aac_swb_long_16[44];
extern const uint16_t fa_aac_swb_long_8[41];
extern const uint16_t fa_aac_swb_short_96[13];
extern const uint16_t fa_aac_swb_short_64[13];
extern const uint16_t fa_aac_swb_short_48[15];
extern const uint16_t fa_aac_swb_short_24[16];
extern const uint16_t fa_aac_swb_short_16[16];
extern const uint16_t fa_aac_swb_short_8[16];
extern const float fa_aac_sbr_qmf_window[640];
extern const float fa_aac_sbr_noise[1024];
extern const float fa_aac_ps_p8_13_20[7];
extern const float fa_aac_ps_p2_13_20[7];
extern const float fa_aac_ps_p12_13_34[7];
extern const float fa_aac_ps_p8_13_34[7];
extern const float fa_aac_ps_p4_13_34[7];

extern const uint32_t fa_aac_rates[13];        /* sampling frequency index -> Hz (aac.c) */

/* ---- SBR (aac_sbr.c) --------------------------------------------------------------- */

typedef struct fa_aac_sbr fa_aac_sbr;
typedef struct fa_aac_qmf fa_aac_qmf;

/* The QMF banks' transform tables (80 KB) and the SBR scratch, shared by a decoder's
 * elements (decoded one after another, never at the same time). */
fa_aac_qmf* fa_aac_qmf_create(void);
void fa_aac_qmf_destroy(fa_aac_qmf* q);
/* One element's SBR: `rate` is the core's sampling rate (the output runs at
 * twice it). `qmf` must outlive it. */
fa_aac_sbr* fa_aac_sbr_create(uint32_t rate, int channels, fa_aac_qmf* qmf);
void fa_aac_sbr_destroy(fa_aac_sbr* s);
void fa_aac_sbr_reset(fa_aac_sbr* s);
/* Reads an SBR extension payload (EXT_SBR_DATA or EXT_SBR_DATA_CRC) of `bits`
 * bits for an SCE (channels 1) or CPE (2). Leaves the reader at the payload's end.
 * Damaged data makes the next fa_aac_sbr_apply upsample without new data. */
void fa_aac_sbr_parse(fa_aac_sbr* s, fa_aac_bits* b, int bits, int crc);
/* Turns 1024 core samples per channel into 2048 at twice the rate: in[ch] holds
 * the core output, out[ch] gets 2048 samples. Without data for this frame the
 * high band is left empty (plain upsampling). With parametric stereo in a mono
 * element, out[1] gets the second channel and 1 is returned. */
int fa_aac_sbr_apply(fa_aac_sbr* s, const float* const in[2], float* out[2]);

/* ---- parametric stereo (aac_ps.c) --------------------------------------------------- */

typedef struct fa_aac_ps fa_aac_ps;

fa_aac_ps* fa_aac_ps_create(void);
void fa_aac_ps_destroy(fa_aac_ps* p);
void fa_aac_ps_reset(fa_aac_ps* p);
/* Reads ps_data() (after the extension id); returns the bits read, or -1 if damaged. */
int fa_aac_ps_parse(fa_aac_ps* p, fa_aac_bits* b, int bits_left);
/* Makes stereo from a mono element's QMF matrix: l holds 38 slots of 64 bands
 * (re, im): the frame's 32 and 6 more of the bands below kx, which the hybrid
 * filters look ahead into. Writes the left channel's 32 slots back to l and the
 * right channel's to r. `new_data`: this frame carried PS data (else the last
 * parameters hold). */
void fa_aac_ps_apply(fa_aac_ps* p, float (*l)[64][2], float (*r)[64][2], int new_data, int kx);

#endif
