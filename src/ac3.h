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

/* What a stream says about its own stereo downmix and dialogue level (TS 102 366
 * 4.4.2, Annex D.1.3, E.1.3.1), from its last independent frame of program 0.
 * The four levels are codes of Tables D.1.2 to D.1.5 (0 = 1.414 ... 7 = 0); the
 * AC-3 cmixlev and surmixlev fields and missing ones are translated to them. */
typedef struct fa_ac3_mix {
    int acmod, lfeon;
    int dmixmod;                     /* 0 not indicated, 1 Lt/Rt preferred, 2 Lo/Ro */
    int ltrtcmix, ltrtsurmix;        /* Lt/Rt centre and surround levels */
    int lorocmix, lorosurmix;        /* Lo/Ro centre and surround levels */
    int lfemix;                      /* E-AC-3 lfemixlevcod (LFE at 10 - code dB), -1 = none */
    int dialnorm;                    /* dialogue level -1..-31 dBFS as 1..31; 0 = reserved */
    uint32_t used;                   /* the decoded channels that have audio, a bit per
                                        channel (a dependent substream may add some) */
} fa_ac3_mix;

/* The levels a stream gets when it gives none (6.8.2): Lt/Rt -3 dB, Lo/Ro the
 * intermediate values Tables 4.4 and 4.5 name for reserved codes; no LFE mix, no
 * preference. Sets acmod and lfeon too. */
void fa_ac3_mix_defaults(fa_ac3_mix* m, int acmod, int lfeon);
/* Is `codec` (fa_decoder.codec) AC-3 or E-AC-3? */
static inline int fa_ac3_is_codec(const char* codec)
{
    return strcmp(codec, "ac3") == 0 || strcmp(codec, "eac3") == 0;
}

/* One atomic word for the codec to hand to the stream (0 = nothing decoded yet).
 * Bits 29 and 30 are always 0. */
uint64_t fa_ac3_mix_pack(const fa_ac3_mix* m);
int fa_ac3_mix_unpack(uint64_t packed, fa_ac3_mix* out);   /* 0 if packed is 0 */
#define FA_AC3_MIX_DIALNORM_BITS ((uint64_t)31 << 24)
/* The mix information of the last unit decoded (0 before the first). */
uint64_t fa_ac3_mix_info(const fa_ac3* d);

/* Stereo downmix weights (TS 102 366 6.8.2, D.2.1, E.2.9) for `channels` decoded
 * channels in the speaker order of fa_ac3_decode: weights[in * 2 + out]. ltrt 1 =
 * Lt/Rt, 0 = Lo/Ro. Only the channels in m->used get a weight. All are scaled by
 * *out_scale (NULL = not wanted, <= 1) so neither output's add up to more than 1.
 * Returns 0 (weights untouched) for 1 or 2 channels, which need no downmix. */
int fa_ac3_downmix(const fa_ac3_mix* m, int channels, int ltrt, float* weights, float* out_scale);

/* Decodes one access unit: an AC-3 syncframe, or an E-AC-3 independent frame and
 * the dependent frames that follow it (data may hold several of them; frames of
 * other programs, substreamid > 0, are skipped). Writes interleaved samples to
 * *pcm (grown as needed; *cap counts floats) in the default speaker order for
 * *channels (see ac3.c). FA_ERROR_GENERIC: damaged data (*frames still holds the
 * access unit's length, of silence). */
fa_result fa_ac3_decode(fa_ac3* d, const uint8_t* data, size_t size, int drc,
                        float** pcm, size_t* cap, int* frames, int* channels, uint32_t* rate);

#endif
