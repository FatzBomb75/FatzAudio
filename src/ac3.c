/* AC-3 (Dolby Digital) and E-AC-3 (Dolby Digital Plus) decoder, written from
 * ATSC A/52 and ETSI TS 102 366 (Annex E).
 *
 * One syncframe holds 1, 2, 3 or 6 audio blocks of 256 samples per channel. Each
 * block carries, per channel, exponents (a coarse spectral envelope) and
 * mantissas whose bit counts follow from a parametric masking model computed
 * identically by encoder and decoder (bit allocation). Above a cut-off the
 * channels may share one "coupling" channel scaled per band, and E-AC-3 may
 * rebuild the top of the spectrum from lower bands ("spectral extension").
 * Coefficients go through a 512-point IMDCT (or two 256-point ones for
 * transients) with a Kaiser-Bessel derived window and overlap-add.
 *
 * E-AC-3 streams can carry several programs (independent substreams); program 0
 * plays, with its dependent substreams adding or replacing channels (7.1 is a
 * 5.1 independent substream plus a dependent one). Atmos (JOC) streams play
 * their 5.1 or 7.1 bed. Every E-AC-3 coding tool is decoded, including the
 * adaptive hybrid transform (a 6-point DCT across the blocks of a frame, with
 * vector and gain-adaptive quantization) and enhanced coupling.
 *
 * Output order is the default layout for the channel count (miniaudio's, which
 * is Microsoft's): 1 C, 2 L R, 3 L R C, 4 L R C S, 5 L R C Ls Rs,
 * 6 L R C LFE Ls Rs, 7 L R C LFE S Ls Rs, 8 L R C LFE Lb Rb Ls Rs. Layouts
 * without a matching count use the next larger one with silent channels (2.1
 * plays as 5.1 with C and the surrounds silent). Dual mono (1+1) plays as stereo. */
#include "ac3.h"
#include "mdct.h"
#include "ac3_aht.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define AC3_BLOCK 256
#define AC3_MAX_BLOCKS 6
#define AC3_MAX_FBW 5
#define AC3_LFE 5          /* channel index of the LFE */
#define AC3_CPL 6          /* channel index of the coupling channel */
#define AC3_NCH 7
#define AC3_MAX_SUBSTREAMS 9   /* the independent one and up to 8 dependent ones */

/* ---- tables (A/52 section 7 and Annex E) ---------------------------------------- */

static const uint32_t ac3_rates[3] = { 48000, 44100, 32000 };
static const uint16_t ac3_kbps[19] = { 32, 40, 48, 56, 64, 80, 96, 112, 128, 160,
                                       192, 224, 256, 320, 384, 448, 512, 576, 640 };
static const uint8_t acmod_nfchans[8] = { 2, 1, 2, 3, 3, 4, 4, 5 };
static const uint8_t eac3_blocks[4] = { 1, 2, 3, 6 };

/* Bit allocation parameters (Table 7.17 - 7.21). */
static const uint8_t slowdec[4] = { 0x0f, 0x11, 0x13, 0x15 };
static const uint8_t fastdec[4] = { 0x3f, 0x53, 0x67, 0x7b };
static const uint16_t slowgain[4] = { 0x540, 0x4d8, 0x478, 0x410 };
static const uint16_t dbpbtab[4] = { 0x000, 0x700, 0x900, 0xb00 };
static const int16_t floortab[8] = { 0x2f0, 0x2b0, 0x270, 0x230, 0x1f0, 0x170, 0x0f0, (int16_t)0xf800 };
static const uint16_t fastgain[8] = { 0x080, 0x100, 0x180, 0x200, 0x280, 0x300, 0x380, 0x400 };

/* First bin of each of the 50 bit allocation bands, and the end of the last. */
static const uint8_t bndtab[51] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
    25, 26, 27, 28, 31, 34, 37, 40, 43, 46, 49, 55, 61, 67, 73, 79, 85, 97, 109, 121, 133,
    157, 181, 205, 229, 253 };

/* Hearing threshold per band (Table 7.23), for 48, 44.1 and 32 kHz. */
static const uint16_t hth[3][50] = {
    { 0x04d0, 0x04d0, 0x0440, 0x0400, 0x03e0, 0x03c0, 0x03b0, 0x03b0, 0x03a0, 0x03a0,
      0x03a0, 0x03a0, 0x03a0, 0x0390, 0x0390, 0x0390, 0x0380, 0x0380, 0x0370, 0x0370,
      0x0360, 0x0360, 0x0350, 0x0350, 0x0340, 0x0340, 0x0330, 0x0320, 0x0310, 0x0300,
      0x02f0, 0x02f0, 0x02f0, 0x02f0, 0x0300, 0x0310, 0x0340, 0x0390, 0x03e0, 0x0420,
      0x0460, 0x0490, 0x04a0, 0x0460, 0x0440, 0x0440, 0x0520, 0x0800, 0x0840, 0x0840 },
    { 0x04f0, 0x04f0, 0x0460, 0x0410, 0x03e0, 0x03d0, 0x03c0, 0x03b0, 0x03b0, 0x03a0,
      0x03a0, 0x03a0, 0x03a0, 0x03a0, 0x0390, 0x0390, 0x0390, 0x0380, 0x0380, 0x0380,
      0x0370, 0x0370, 0x0360, 0x0360, 0x0350, 0x0350, 0x0340, 0x0340, 0x0320, 0x0310,
      0x0300, 0x02f0, 0x02f0, 0x02f0, 0x02f0, 0x0300, 0x0320, 0x0350, 0x0390, 0x03e0,
      0x0420, 0x0450, 0x04a0, 0x0490, 0x0460, 0x0440, 0x0480, 0x0630, 0x0840, 0x0840 },
    { 0x0580, 0x0580, 0x04b0, 0x0450, 0x0420, 0x03f0, 0x03e0, 0x03d0, 0x03c0, 0x03b0,
      0x03b0, 0x03b0, 0x03a0, 0x03a0, 0x03a0, 0x03a0, 0x03a0, 0x03a0, 0x03a0, 0x03a0,
      0x0390, 0x0390, 0x0390, 0x0390, 0x0380, 0x0380, 0x0380, 0x0370, 0x0360, 0x0350,
      0x0340, 0x0330, 0x0320, 0x0310, 0x0300, 0x02f0, 0x02f0, 0x02f0, 0x0300, 0x0310,
      0x0330, 0x0350, 0x03c0, 0x0410, 0x0470, 0x04a0, 0x0460, 0x0440, 0x0450, 0x04e0 } };

/* Bit allocation pointer per masked level (Table 7.16). */
static const uint8_t baptab[64] = {
    0, 1, 1, 1, 1, 1, 2, 2, 3, 3, 3, 4, 4, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8, 8, 9, 9, 9, 9,
    10, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 14, 14, 14,
    14, 15, 15, 15, 15, 15, 15, 15, 15, 15 };

/* Mantissa bits for bap 6..15 (asymmetric quantizers, Table 7.18). */
static const uint8_t qbits[16] = { 0, 0, 0, 0, 0, 0, 5, 6, 7, 8, 9, 10, 11, 12, 14, 16 };

static const uint8_t rematbnd[5] = { 13, 25, 37, 61, 253 };

/* Default band structures for E-AC-3 coupling and spectral extension (subbands
 * merged into the band before them). */
static const uint8_t def_cplbndstrc[18] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 0, 1, 1, 1, 1, 1 };
static const uint8_t def_spxbndstrc[17] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1 };

/* Adaptive hybrid transform (TS 102 366 E.2.4): high-efficiency bap per address
 * (Table E.2.1); VQ index bits for hebap 1..7 and GAQ mantissa bits for 8..19
 * (Table E.2.2); remapping constants a (G = 1, 2, 4) and b for x < 0 (G = 2, 4)
 * as 16-bit fractions (Table E.2.6; b is 1/G for x >= 0, and 0 for G = 1). */
static const uint8_t hebaptab[64] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 8, 8, 8, 9, 9, 9, 10, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 12,
    13, 13, 13, 13, 14, 14, 14, 14, 15, 15, 15, 15, 16, 16, 16, 16, 17, 17, 17, 17, 18, 18, 18,
    18, 18, 18, 18, 18, 19, 19, 19, 19, 19, 19, 19, 19, 19 };
static const uint8_t aht_vqbits[8] = { 0, 2, 3, 4, 5, 7, 8, 9 };
static const uint8_t aht_mbits[12] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 16 };
static const int16_t gaq_a1[12] = { 0x1249, 0x0889, 0x0421, 0x0208, 0x0102, 0x0081, 0x0040, 0x0020, 0x0010,
                                    0x0008, 0x0002, 0x0000 };
static const uint16_t gaq_a[2][9] = {
    { 0xd555, 0xc925, 0xc444, 0xc211, 0xc104, 0xc081, 0xc040, 0xc020, 0xc010 },
    { 0xedb7, 0xe666, 0xe319, 0xe186, 0xe0c2, 0xe060, 0xe030, 0xe018, 0xe00c } };
static const uint16_t gaq_bneg[2][9] = {
    { 0xeaab, 0xd249, 0xc889, 0xc421, 0xc208, 0xc102, 0xc081, 0xc040, 0xc020 },
    { 0xfb6e, 0xeccd, 0xe632, 0xe30c, 0xe183, 0xe0c1, 0xe060, 0xe030, 0xe018 } };

/* Enhanced coupling (E.2.5): first bin of each sub-band (Table E.2.9), default
 * band structure (Table E.1.13) and amplitudes (Table E.2.10). */
static const uint8_t ecplsubbndtab[23] = { 13, 19, 25, 31, 37, 49, 61, 73, 85, 97, 109, 121, 133, 145,
                                           157, 169, 181, 193, 205, 217, 229, 241, 253 };
static const uint8_t def_ecplbndstrc[22] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 1, 1, 0, 1, 1, 1 };

/* Tables computed once (ac3_init_tables). */
static uint8_t latab[256];        /* log-addition (Table 7.15) */
static uint16_t crctab[256];       /* CRC-16 of each byte value */
static float pow2tab[33];          /* 2^-i, exact */
static uint8_t masktab[256];      /* bin -> band */
static float kbd[AC3_BLOCK * 2];  /* Kaiser-Bessel derived window, alpha 5 */
static float b1q[27][3], b2q[125][3], b4q[121][2], b3q[8], b5q[16];
static uint8_t frmexpstr[32][6];  /* E-AC-3 frame exponent strategies (Table E2.10) */
static float spxatten[32][3];
static float aht_idct[6][6];      /* [block][j]: the AHT's inverse DCT */
static float ecplamptab[32];
static fa_atomic32 tables_state;

static int ac3_build_tables(void* ctx)
{
    int i, b, k;
    (void)ctx;
    for (i = 0; i < 256; i++) {
        uint32_t crc = (uint32_t)i << 8;
        for (b = 0; b < 8; b++) crc = (crc & 0x8000) ? ((crc << 1) ^ 0x8005) & 0xFFFF : (crc << 1) & 0xFFFF;
        crctab[i] = (uint16_t)crc;
    }
    for (i = 0; i < 33; i++) pow2tab[i] = ldexpf(1.0f, -i);
    /* Log-addition: the increase (in 1/128ths of 6 dB) when two powers that differ
     * by 2*i units are added. */
    for (i = 0; i < 256; i++) {
        double u = 128.0 / 6.0;
        latab[i] = (uint8_t)floor(u * 10.0 * log10(1.0 + pow(10.0, -2.0 * i / u / 10.0)));
    }
    for (b = 0; b < 50; b++)
        for (k = bndtab[b]; k < bndtab[b + 1]; k++) masktab[k] = (uint8_t)b;
    for (k = 253; k < 256; k++) masktab[k] = 0;
    /* KBD window, alpha 5 */
    fa_kbd_window(kbd, AC3_BLOCK, 5.0);
    for (i = 0; i < AC3_BLOCK; i++) kbd[2 * AC3_BLOCK - 1 - i] = kbd[i];
    /* Symmetric quantizers: level j of L is (2j - (L-1)) / L. */
    for (i = 0; i < 27; i++) {
        b1q[i][0] = (float)(2 * (i / 9) - 2) / 3.0f;
        b1q[i][1] = (float)(2 * ((i / 3) % 3) - 2) / 3.0f;
        b1q[i][2] = (float)(2 * (i % 3) - 2) / 3.0f;
    }
    for (i = 0; i < 125; i++) {
        b2q[i][0] = (float)(2 * (i / 25) - 4) / 5.0f;
        b2q[i][1] = (float)(2 * ((i / 5) % 5) - 4) / 5.0f;
        b2q[i][2] = (float)(2 * (i % 5) - 4) / 5.0f;
    }
    for (i = 0; i < 121; i++) {
        b4q[i][0] = (float)(2 * (i / 11) - 10) / 11.0f;
        b4q[i][1] = (float)(2 * (i % 11) - 10) / 11.0f;
    }
    for (i = 0; i < 8; i++) b3q[i] = i < 7 ? (float)(2 * i - 6) / 7.0f : 0.0f;
    for (i = 0; i < 16; i++) b5q[i] = i < 15 ? (float)(2 * i - 14) / 15.0f : 0.0f;
    /* Frame exponent strategies: bit 4 - n of the index says block n (1..5) gets
     * new exponents; a run of 1 block is coded D45, 2-3 blocks D25, 4-6 D15. */
    for (i = 0; i < 32; i++) {
        int start = 0;
        for (b = 1; b <= 6; b++) {
            if (b == 6 || (i >> (5 - b)) & 1) {
                int run = b - start;
                frmexpstr[i][start] = (uint8_t)(run == 1 ? 3 : run <= 3 ? 2 : 1);
                for (k = start + 1; k < b; k++) frmexpstr[i][k] = 0;
                start = b;
            }
        }
    }
    for (i = 0; i < 32; i++)
        for (k = 0; k < 3; k++) spxatten[i][k] = (float)pow(2.0, -(double)((k + 1) * (i + 1)) / 15.0);
    /* The AHT's inverse DCT (E.2.4.5): C(m) = X(0) + sqrt 2 * the sum over j >= 1
     * of X(j) cos(pi (2m + 1) j / 12), at the scale FFmpeg decodes with
     * (checked on test streams). */
    for (i = 0; i < 6; i++)
        for (k = 0; k < 6; k++)
            aht_idct[i][k] = (float)((k == 0 ? 1.0 : sqrt(2.0)) * cos(FA_PI * (2 * i + 1) * k / 12.0));
    {
        static const uint8_t mant[4] = { 0x1b, 0x17, 0x13, 0x10 };
        ecplamptab[0] = 1.0f;
        for (i = 1; i < 31; i++) ecplamptab[i] = ldexpf((float)mant[(i - 1) % 4] / 32.0f, -((i - 1) / 4));
        ecplamptab[31] = 0.0f;
    }
    return 1;
}

static void ac3_init_tables(void) { fa_once(&tables_state, ac3_build_tables, NULL); }

/* ---- bit reader --------------------------------------------------------------- */

typedef struct bits {
    const uint8_t* p;
    size_t pos, end;   /* in bits */
    int overrun;
} bits;

/* Near the end of the frame, or past it (overrun: 0, and the reader stays at the end). */
#if defined(__GNUC__)
__attribute__((noinline))
#endif
static uint32_t gb_tail(bits* b, int n)
{
    uint32_t v = 0;
    size_t byte = b->pos >> 3;
    if (b->pos + (size_t)n > b->end) {
        b->overrun = 1;
        b->pos = b->end;
        return 0;
    }
    while (n > 0) {
        int off = (int)(b->pos & 7), take = 8 - off;
        if (take > n) take = n;
        v = (v << take) | (((uint32_t)b->p[byte] >> (8 - off - take)) & ((1u << take) - 1));
        b->pos += (size_t)take;
        byte = b->pos >> 3;
        n -= take;
    }
    return v;
}

/* n <= 16. Inline: four whole bytes hold the field (an offset of up to 7 plus 16
 * bits) everywhere but in the last four bytes of the frame. */
static inline uint32_t gb(bits* b, int n)
{
    size_t byte = b->pos >> 3;
    if (n == 0) return 0;
    if (byte + 4 <= b->end >> 3) {
        const uint8_t* q = b->p + byte;
        uint32_t w = (uint32_t)q[0] << 24 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 8 | q[3];
        uint32_t v = (w << (b->pos & 7)) >> (32 - n);
        b->pos += (size_t)n;
        return v;
    }
    return gb_tail(b, n);
}

static int gb1(bits* b) { return (int)gb(b, 1); }
static void skipb(bits* b, size_t n)
{
    if (b->pos + n > b->end) { b->overrun = 1; b->pos = b->end; }
    else b->pos += n;
}

/* ---- syncframe header ----------------------------------------------------------- */

int fa_ac3_parse_header(const uint8_t* p, size_t n, fa_ac3_header* h)
{
    int bsid;
    if (n < 8 || p[0] != 0x0B || p[1] != 0x77) return 0;
    bsid = p[5] >> 3;
    memset(h, 0, sizeof(*h));
    h->bsid = bsid;
    if (bsid <= 10) {
        int fscod = p[4] >> 6, frmsizecod = p[4] & 63, kbps, acmod, bit;
        if (fscod == 3 || frmsizecod >= 38) return 0;
        kbps = ac3_kbps[frmsizecod >> 1];
        if (fscod == 0) h->frame_size = (size_t)kbps * 4;
        else if (fscod == 2) h->frame_size = (size_t)kbps * 6;
        else h->frame_size = ((size_t)kbps * 320 / 147 + (frmsizecod & 1)) * 2;
        /* bsid 9 and 10: half and quarter sample rates (A/52 Annex). */
        h->rate = ac3_rates[fscod] >> (bsid > 8 ? bsid - 8 : 0);
        h->blocks = 6;
        acmod = p[6] >> 5;
        /* lfeon follows cmixlev, surmixlev and dsurmod where present. */
        bit = 51;
        if ((acmod & 1) && acmod != 1) bit += 2;
        if (acmod & 4) bit += 2;
        if (acmod == 2) bit += 2;
        h->acmod = acmod;
        h->lfeon = (p[bit >> 3] >> (7 - (bit & 7))) & 1;
    } else if (bsid <= 16) {
        int fscod = p[4] >> 6, numblkscod = (p[4] >> 4) & 3;
        size_t frmsiz = (((size_t)p[2] & 7) << 8) | p[3];
        h->eac3 = 1;
        h->strmtyp = p[2] >> 6;
        h->substreamid = (p[2] >> 3) & 7;
        if (h->strmtyp == 3) return 0;
        h->frame_size = (frmsiz + 1) * 2;
        if (fscod == 3) {
            if (numblkscod == 3) return 0;
            h->rate = ac3_rates[numblkscod] / 2;
            h->blocks = 6;
        } else {
            h->rate = ac3_rates[fscod];
            h->blocks = eac3_blocks[numblkscod];
        }
        h->acmod = (p[4] >> 1) & 7;
        h->lfeon = p[4] & 1;
        if (h->frame_size < 8) return 0;
    } else {
        return 0;
    }
    h->channels = acmod_nfchans[h->acmod] + h->lfeon;
    return 1;
}

/* CRC-16 (x^16 + x^15 + x^2 + 1), MSB first. Over a whole frame after the
 * syncword it is 0 for an intact frame. */
static uint16_t crc16(const uint8_t* p, size_t n)
{
    uint32_t crc = 0;
    size_t i;
    for (i = 0; i < n; i++) crc = ((crc << 8) ^ crctab[(crc >> 8) ^ p[i]]) & 0xFFFF;
    return (uint16_t)crc;
}

/* ---- decoder state ------------------------------------------------------------- */

/* Speaker locations a substream's channels can go to. */
enum { LOC_L = 0, LOC_C, LOC_R, LOC_LS, LOC_RS, LOC_LC, LOC_RC, LOC_LRS, LOC_RRS, LOC_CS,
       LOC_TS, LOC_LSD, LOC_RSD, LOC_LW, LOC_RW, LOC_VHL, LOC_VHR, LOC_VHC, LOC_LTS, LOC_RTS,
       LOC_LFE2, LOC_LFE, LOC_S, LOC_COUNT };

typedef struct sub_state {
    float delay[6][AC3_BLOCK];   /* overlap per channel (fbw 0..4, LFE 5) */
    int used;
    int seen;                    /* in the access unit being decoded */
    uint32_t layout;             /* acmod, lfeon and chanmap the overlap belongs to */
    uint32_t locs;               /* the locations it wrote last (bit per LOC_) */
} sub_state;

#define BA_KEY 40

/* Everything one syncframe needs while it is decoded. */
typedef struct frame {
    fa_ac3_header h;
    int nfchans, fscod, sr_shift;
    /* bsi */
    int compre[2];
    float compr[2];
    int chanmape;
    uint32_t chanmap;
    /* audfrm (E-AC-3) */
    int ahte, snroffststr, blkswe, dithflage, bamode, frmfgaincode, dbaflde, skipflde;
    int cplstre[AC3_MAX_BLOCKS], cplinu_blk[AC3_MAX_BLOCKS];
    int expstr[AC3_MAX_BLOCKS][AC3_NCH];
    int spxattencod[AC3_MAX_FBW];
    int firstspxcos[AC3_MAX_FBW], firstcplcos[AC3_MAX_FBW], firstcplleak;
    /* per block, kept from block to block */
    int blksw[AC3_MAX_FBW], dithflag[AC3_MAX_FBW];
    float dynrng[2];
    int cplinu, chincpl[AC3_MAX_FBW], phsflginu;
    int cplbegf, ncplbnd;
    uint8_t cplbndstrc[18];
    int cplbndsz[18];
    float cplco[AC3_MAX_FBW][18];
    int phsflg[18];
    int nrematbd, rematflg[4];
    int spxinu, chinspx[AC3_MAX_FBW];
    int spx_copy_start, spx_start, spx_end;   /* bins */
    int nspxbnd, spxbndsz[17];
    uint8_t spxbndstrc[17];
    float spx_nblend[AC3_MAX_FBW][17], spx_sblend[AC3_MAX_FBW][17];
    int ahtinu[AC3_NCH];          /* this frame's mantissas of the channel are AHT coded */
    int ecplinu, ecpl_begin, ecpl_end;   /* sub-bands */
    int necplbnd;
    uint8_t ecplbndstrc[22], ecplbnd[22];   /* sub-band -> band */
    float ecplamp[AC3_MAX_FBW][22];
    int strtmant[AC3_NCH], endmant[AC3_NCH];
    int ngrps[AC3_NCH];
    uint8_t exp[AC3_NCH][256];
    uint8_t bap[AC3_NCH][256];
    int sdecay, fdecay, sgain, dbknee, floorv;
    int snroffset[AC3_NCH], fgain[AC3_NCH];
    int cplfleak, cplsleak;
    int deltbae[AC3_NCH], deltnseg[AC3_NCH];
    uint8_t deltoffst[AC3_NCH][8], deltlen[AC3_NCH][8], deltba[AC3_NCH][8];
    float coef[AC3_NCH][256];
    /* The inputs of each channel's last bit allocation: blocks that reuse the
     * exponents and parameters reuse its bap[] (most blocks after the first). */
    int ba_key[AC3_NCH][BA_KEY];
    uint8_t ba_exp[AC3_NCH][256];
    int ba_ok[AC3_NCH];
    /* AHT channels: every block's mantissas, read in block 0 (allocated with the
     * first frame that uses AHT). */
    float (*aht)[6][256];
} frame;

struct fa_ac3 {
    fa_mdct t256, t128;
    sub_state* sub[AC3_MAX_SUBSTREAMS];   /* allocated when a substream first plays */
    frame f;
    float tmp[512], u[256];
    uint32_t rng;
    /* Output of the substreams of one access unit, per speaker location (allocated
     * when a location is first written; NULL plays as silence). */
    float* loc[LOC_COUNT];
    int loc_used[LOC_COUNT];
    float blk[6][AC3_BLOCK];     /* one block per channel, for units mixed directly */
    int last_used[LOC_COUNT];    /* the locations of the last good unit */
};

fa_ac3* fa_ac3_create(void)
{
    fa_ac3* d;
    ac3_init_tables();
    d = (fa_ac3*)calloc(1, sizeof(*d));
    if (!d) return NULL;
    if (fa_imdct_init(&d->t256, 256) != FA_OK || fa_imdct_init(&d->t128, 128) != FA_OK) {
        fa_ac3_destroy(d);
        return NULL;
    }
    d->rng = 0x12345678u;
    return d;
}

void fa_ac3_destroy(fa_ac3* d)
{
    int i;
    if (!d) return;
    fa_imdct_free(&d->t256);
    fa_imdct_free(&d->t128);
    for (i = 0; i < LOC_COUNT; i++) free(d->loc[i]);
    for (i = 0; i < AC3_MAX_SUBSTREAMS; i++) free(d->sub[i]);
    free(d->f.aht);
    free(d);
}

void fa_ac3_reset(fa_ac3* d)
{
    int i;
    for (i = 0; i < AC3_MAX_SUBSTREAMS; i++) if (d->sub[i]) memset(d->sub[i], 0, sizeof(*d->sub[i]));
}

/* Uniform in [-1, 1). */
static float rnd(fa_ac3* d)
{
    d->rng = d->rng * 1664525u + 1013904223u;
    return (float)(int32_t)d->rng * (1.0f / 2147483648.0f);
}

/* ---- bit allocation (A/52 7.2.2) ---------------------------------------------- */

static int logadd(int a, int b)
{
    int c = a - b, addr = (c < 0 ? -c : c) >> 1;
    if (addr > 255) addr = 255;
    return (c >= 0 ? a : b) + latab[addr];
}

static int calc_lowcomp(int a, int b0, int b1, int bin)
{
    if (bin < 7) {
        if (b0 + 256 == b1) a = 384;
        else if (b0 > b1) a = a > 64 ? a - 64 : 0;
    } else if (bin < 20) {
        if (b0 + 256 == b1) a = 320;
        else if (b0 > b1) a = a > 64 ? a - 64 : 0;
    } else {
        a = a > 128 ? a - 128 : 0;
    }
    return a;
}

static int imax(int a, int b) { return a > b ? a : b; }
static int imin(int a, int b) { return a < b ? a : b; }

/* bap[] of channel ch for bins strtmant..endmant (hebap[] when the channel is
 * AHT coded). 0 on success. */
static int bit_alloc(frame* f, int ch)
{
    const uint8_t* tab = f->ahtinu[ch] ? hebaptab : baptab;
    int start = f->strtmant[ch], end = f->endmant[ch];
    int16_t psd[256], bndpsd[51], mask[50] = { 0 };
    uint8_t* bap = f->bap[ch];
    int excite[50];
    int bin, j, k, lastbin, bndstrt, bndend, begin, lowcomp = 0, fastleak = 0, slowleak = 0;
    int fgain = f->fgain[ch], snroffset = f->snroffset[ch], is_lfe = ch == AC3_LFE;
    int sdecay = f->sdecay, fdecay = f->fdecay, sgain = f->sgain;

    int key[BA_KEY];

    if (end <= start) return 0;
    key[0] = start; key[1] = end; key[2] = snroffset; key[3] = fgain; key[4] = sdecay;
    key[5] = fdecay; key[6] = sgain; key[7] = f->dbknee; key[8] = f->floorv; key[9] = f->cplfleak;
    key[10] = f->cplsleak; key[11] = f->fscod; key[12] = f->sr_shift; key[13] = f->deltbae[ch];
    key[14] = f->deltbae[ch] <= 1 ? f->deltnseg[ch] : 0;
    for (k = 0; k < 8; k++) {
        int on = k < key[14];
        key[15 + k] = on ? f->deltoffst[ch][k] : 0;
        key[23 + k] = on ? f->deltlen[ch][k] : 0;
        key[31 + k] = on ? f->deltba[ch][k] : 0;
    }
    key[39] = f->ahtinu[ch];   /* hebap[] or bap[] */
    if (f->ba_ok[ch] && memcmp(key, f->ba_key[ch], sizeof(key)) == 0 &&
        memcmp(f->exp[ch] + start, f->ba_exp[ch] + start, (size_t)(end - start)) == 0)
        return 0;
    f->ba_ok[ch] = 0;
    if (snroffset == -960) {   /* csnroffst and fsnroffst both 0: no mantissas */
        memset(bap + start, 0, (size_t)(end - start));
        goto done;
    }
    for (bin = start; bin < end; bin++) psd[bin] = (int16_t)(3072 - (f->exp[ch][bin] << 7));
    /* Integration of the PSD over each band. */
    j = start;
    k = masktab[start];
    do {
        lastbin = imin(bndtab[k + 1], end);
        bndpsd[k] = psd[j++];
        for (; j < lastbin; j++) bndpsd[k] = (int16_t)logadd(bndpsd[k], psd[j]);
        k++;
    } while (end > lastbin);
    bndpsd[k] = 0;

    /* Excitation function. */
    bndstrt = masktab[start];
    bndend = masktab[end - 1] + 1;
    if (bndstrt == 0) {
        lowcomp = calc_lowcomp(lowcomp, bndpsd[0], bndpsd[1], 0);
        excite[0] = bndpsd[0] - fgain - lowcomp;
        lowcomp = calc_lowcomp(lowcomp, bndpsd[1], bndpsd[2], 1);
        excite[1] = bndpsd[1] - fgain - lowcomp;
        begin = 7;
        for (bin = 2; bin < 7; bin++) {
            if (!is_lfe || bin != 6) lowcomp = calc_lowcomp(lowcomp, bndpsd[bin], bndpsd[bin + 1], bin);
            fastleak = bndpsd[bin] - fgain;
            slowleak = bndpsd[bin] - sgain;
            excite[bin] = fastleak - lowcomp;
            if ((!is_lfe || bin != 6) && bndpsd[bin] <= bndpsd[bin + 1]) {
                begin = bin + 1;
                break;
            }
        }
        for (bin = begin; bin < imin(bndend, 22); bin++) {
            if (!is_lfe || bin != 6) lowcomp = calc_lowcomp(lowcomp, bndpsd[bin], bndpsd[bin + 1], bin);
            fastleak = imax(fastleak - fdecay, bndpsd[bin] - fgain);
            slowleak = imax(slowleak - sdecay, bndpsd[bin] - sgain);
            excite[bin] = imax(fastleak - lowcomp, slowleak);
        }
        begin = 22;
    } else {
        begin = bndstrt;
        fastleak = (f->cplfleak << 8) + 768;
        slowleak = (f->cplsleak << 8) + 768;
    }
    for (bin = begin; bin < bndend; bin++) {
        fastleak = imax(fastleak - fdecay, bndpsd[bin] - fgain);
        slowleak = imax(slowleak - sdecay, bndpsd[bin] - sgain);
        excite[bin] = imax(fastleak, slowleak);
    }
    /* Masking curve. */
    for (bin = bndstrt; bin < bndend; bin++) {
        if (bndpsd[bin] < f->dbknee) excite[bin] += (f->dbknee - bndpsd[bin]) >> 2;
        mask[bin] = (int16_t)imax(excite[bin], hth[f->fscod][bin >> f->sr_shift]);
    }
    /* Delta bit allocation. */
    if (f->deltbae[ch] == 0 || f->deltbae[ch] == 1) {
        int seg, band = bndstrt;
        for (seg = 0; seg < f->deltnseg[ch]; seg++) {
            int delta = f->deltba[ch][seg] >= 4 ? (f->deltba[ch][seg] - 3) * 128 : (f->deltba[ch][seg] - 4) * 128;
            band += f->deltoffst[ch][seg];
            if (band + f->deltlen[ch][seg] > 50) return -1;
            for (k = 0; k < f->deltlen[ch][seg]; k++) mask[band++] += (int16_t)delta;
        }
    }
    /* Bit allocation pointers. */
    bin = start;
    j = masktab[start];
    do {
        int m = mask[j] - snroffset - f->floorv;
        lastbin = imin(bndtab[j + 1], end);
        if (m < 0) m = 0;
        m = (m & 0x1fe0) + f->floorv;
        for (; bin < lastbin; bin++) {
            int addr = (psd[bin] - m) >> 5;
            bap[bin] = tab[addr < 0 ? 0 : addr > 63 ? 63 : addr];
        }
        j++;
    } while (end > lastbin);
done:
    memcpy(f->ba_key[ch], key, sizeof(key));
    memcpy(f->ba_exp[ch] + start, f->exp[ch] + start, (size_t)(end - start));
    f->ba_ok[ch] = 1;
    return 0;
}

/* ---- syntax helpers ------------------------------------------------------------- */

/* Exponents of channel ch (strategy 1 D15, 2 D25, 3 D45) into exp[first..]. */
static int read_exponents(bits* b, frame* f, int ch, int strategy, int absexp, int first)
{
    int grpsize = strategy == 3 ? 4 : strategy, g, i, prev = absexp, n = first;
    for (g = 0; g < f->ngrps[ch]; g++) {
        int v = (int)gb(b, 7), d[3];
        if (v >= 125) return -1;
        d[0] = v / 25;
        d[1] = (v % 25) / 5;
        d[2] = v % 5;
        for (i = 0; i < 3; i++) {
            int r;
            prev += d[i] - 2;
            if (prev < 0 || prev > 24) return -1;
            for (r = 0; r < grpsize; r++)
                if (n < 256) f->exp[ch][n++] = (uint8_t)prev;
        }
    }
    return 0;
}

/* Band sizes from a band structure (subbands of 12 bins merged into the band
 * before them). Returns the number of bands. */
static int band_sizes(const uint8_t* strc, int first, int last, int* sizes)
{
    int n = 0, s;
    sizes[0] = 12;
    for (s = first + 1; s < last; s++) {
        if (strc[s]) sizes[n] += 12;
        else sizes[++n] = 12;
    }
    return n + 1;
}

static float drc_gain(int v)
{
    int x = v >> 5;                     /* signed 3-bit exponent */
    if (x >= 4) x -= 8;
    return ldexpf((float)(32 + (v & 31)) / 32.0f, x);
}

static float compr_gain(int v)
{
    int x = v >> 4;                     /* signed 4-bit exponent */
    if (x >= 8) x -= 16;
    return ldexpf((float)(16 + (v & 15)) / 16.0f, x);
}

/* ---- bsi and audfrm --------------------------------------------------------------- */

static int parse_bsi(bits* b, frame* f)
{
    const fa_ac3_header* h = &f->h;
    int i, acmod = h->acmod, nprog = acmod == 0 ? 2 : 1;
    f->nfchans = acmod_nfchans[acmod];
    f->compre[0] = f->compre[1] = 0;
    f->chanmape = 0;
    if (!h->eac3) {
        skipb(b, 16 + 16);                        /* syncword, crc1 */
        f->fscod = (int)gb(b, 2);
        skipb(b, 6 + 5 + 3 + 3);                  /* frmsizecod, bsid, bsmod, acmod */
        if ((acmod & 1) && acmod != 1) skipb(b, 2);
        if (acmod & 4) skipb(b, 2);
        if (acmod == 2) skipb(b, 2);
        skipb(b, 1);                              /* lfeon */
        for (i = 0; i < nprog; i++) {
            skipb(b, 5);                          /* dialnorm */
            if ((f->compre[i] = gb1(b)) != 0) f->compr[i] = compr_gain((int)gb(b, 8));
            if (gb1(b)) skipb(b, 8);              /* langcod */
            if (gb1(b)) skipb(b, 5 + 2);          /* mixlevel, roomtyp */
        }
        skipb(b, 2);                              /* copyrightb, origbs */
        if (h->bsid == 6) {                       /* alternate syntax (A/52 Annex D) */
            if (gb1(b)) skipb(b, 2 + 3 + 3 + 3 + 3);
            if (gb1(b)) skipb(b, 2 + 2 + 1 + 8 + 1);
        } else {
            if (gb1(b)) skipb(b, 14);
            if (gb1(b)) skipb(b, 14);
        }
        f->sr_shift = h->bsid > 8 ? h->bsid - 8 : 0;
    } else {
        int numblkscod, fscod;
        skipb(b, 16 + 2 + 3 + 11);                /* syncword, strmtyp, substreamid, frmsiz */
        fscod = (int)gb(b, 2);
        numblkscod = (int)gb(b, 2);
        if (fscod == 3) {
            f->fscod = numblkscod;
            f->sr_shift = 1;
            numblkscod = 3;
        } else {
            f->fscod = fscod;
            f->sr_shift = 0;
        }
        skipb(b, 3 + 1 + 5);                      /* acmod, lfeon, bsid */
        for (i = 0; i < nprog; i++) {
            skipb(b, 5);                          /* dialnorm */
            if ((f->compre[i] = gb1(b)) != 0) f->compr[i] = compr_gain((int)gb(b, 8));
        }
        if (h->strmtyp == 1 && (f->chanmape = gb1(b)) != 0) f->chanmap = gb(b, 16);
        if (gb1(b)) {                             /* mixmdate */
            if (acmod > 2) skipb(b, 2);           /* dmixmod */
            if ((acmod & 1) && acmod > 2) skipb(b, 3 + 3);
            if (acmod & 4) skipb(b, 3 + 3);
            if (h->lfeon && gb1(b)) skipb(b, 5);  /* lfemixlevcod */
            if (h->strmtyp == 0) {
                for (i = 0; i < nprog; i++) if (gb1(b)) skipb(b, 6);   /* pgmscl */
                if (gb1(b)) skipb(b, 6);          /* extpgmscl */
                switch (gb(b, 2)) {               /* mixdef */
                case 1: skipb(b, 5); break;
                case 2: skipb(b, 12); break;
                case 3: skipb(b, ((size_t)gb(b, 5) + 2) * 8); break;
                default: break;
                }
                if (acmod < 2)
                    for (i = 0; i < nprog; i++) if (gb1(b)) skipb(b, 8 + 6);   /* paninfo */
                if (gb1(b)) {                     /* frmmixcfginfoe */
                    int blk;
                    for (blk = 0; blk < h->blocks; blk++)
                        if (h->blocks == 1 || gb1(b)) skipb(b, 5);
                }
            }
        }
        if (gb1(b)) {                             /* infomdate */
            skipb(b, 3 + 1 + 1);                  /* bsmod, copyrightb, origbs */
            if (acmod == 2) skipb(b, 2 + 2);
            if (acmod >= 6) skipb(b, 2);
            for (i = 0; i < nprog; i++) if (gb1(b)) skipb(b, 5 + 2 + 1);
            if (fscod < 3) skipb(b, 1);           /* sourcefscod */
        }
        if (h->strmtyp == 0 && h->blocks != 6) skipb(b, 1);   /* convsync */
        if (h->strmtyp == 2 && (h->blocks == 6 || gb1(b))) skipb(b, 6);   /* blkid: frmsizecod */
    }
    if (gb1(b)) skipb(b, ((size_t)gb(b, 6) + 1) * 8);         /* addbsi */
    return b->overrun ? -1 : 0;
}

static int parse_audfrm(bits* b, frame* f)
{
    const fa_ac3_header* h = &f->h;
    int expstre = 1, transproce, spxattene, blk, ch, i, ncplblks = 0;
    int nfch = f->nfchans;
    f->ahte = 0;
    if (h->blocks == 6) {
        expstre = gb1(b);
        f->ahte = gb1(b);
    }
    f->snroffststr = (int)gb(b, 2);
    transproce = gb1(b);
    f->blkswe = gb1(b);
    f->dithflage = gb1(b);
    f->bamode = gb1(b);
    f->frmfgaincode = gb1(b);
    f->dbaflde = gb1(b);
    f->skipflde = gb1(b);
    spxattene = gb1(b);
    for (blk = 0; blk < h->blocks; blk++) {
        if (h->acmod > 1) {
            f->cplstre[blk] = blk == 0 || gb1(b);
            f->cplinu_blk[blk] = f->cplstre[blk] ? gb1(b) : f->cplinu_blk[blk - 1];
        } else {
            f->cplstre[blk] = blk == 0;
            f->cplinu_blk[blk] = 0;
        }
        ncplblks += f->cplinu_blk[blk];
    }
    if (expstre) {
        for (blk = 0; blk < h->blocks; blk++) {
            f->expstr[blk][AC3_CPL] = f->cplinu_blk[blk] ? (int)gb(b, 2) : 0;
            for (ch = 0; ch < nfch; ch++) f->expstr[blk][ch] = (int)gb(b, 2);
        }
    } else {
        int v;
        if (h->acmod > 1 && ncplblks > 0) {
            v = (int)gb(b, 5);
            for (blk = 0; blk < 6; blk++) f->expstr[blk][AC3_CPL] = frmexpstr[v][blk];
        } else {
            for (blk = 0; blk < 6; blk++) f->expstr[blk][AC3_CPL] = 0;
        }
        for (ch = 0; ch < nfch; ch++) {
            v = (int)gb(b, 5);
            for (blk = 0; blk < 6; blk++) f->expstr[blk][ch] = frmexpstr[v][blk];
        }
    }
    for (blk = 0; blk < h->blocks; blk++) f->expstr[blk][AC3_LFE] = h->lfeon ? gb1(b) : 0;
    if (h->strmtyp == 0 && (h->blocks == 6 || gb1(b))) skipb(b, 5 * (size_t)nfch);   /* convexpstr */
    if (f->ahte) {
        /* AHT is possible for a channel whose exponents (and coupling strategy)
         * are sent once in the frame, in block 0: the coupling channel, the
         * full-bandwidth channels, then the LFE. */
        for (i = 0; i < 2 + nfch; i++) {
            int nregs = 0;
            ch = i == 0 ? AC3_CPL : i <= nfch ? i - 1 : AC3_LFE;
            if (ch == AC3_CPL && ncplblks != 6) continue;
            if (ch == AC3_LFE && !h->lfeon) continue;
            for (blk = 0; blk < 6; blk++)
                if (f->expstr[blk][ch] != 0 || (ch == AC3_CPL && f->cplstre[blk])) nregs++;
            if (nregs == 1) f->ahtinu[ch] = gb1(b);
        }
    }
    if (f->snroffststr == 3) return -1;            /* reserved */
    if (f->snroffststr == 0) {
        int csnr = (int)gb(b, 6), fsnr = (int)gb(b, 4);
        int v = ((csnr - 15) * 16 + fsnr) * 4;
        for (ch = 0; ch < AC3_NCH; ch++) f->snroffset[ch] = v;
    }
    if (transproce)
        for (ch = 0; ch < nfch; ch++) if (gb1(b)) skipb(b, 10 + 8);   /* not applied */
    for (ch = 0; ch < nfch; ch++)
        f->spxattencod[ch] = spxattene && gb1(b) ? (int)gb(b, 5) : -1;
    if (h->blocks > 1 && gb1(b)) {                 /* blkstrtinfo */
        size_t words = h->frame_size / 2, lg = 0;
        while (((size_t)1 << lg) < words) lg++;
        skipb(b, (size_t)(h->blocks - 1) * (4 + lg));
    }
    for (ch = 0; ch < AC3_MAX_FBW; ch++) f->firstspxcos[ch] = f->firstcplcos[ch] = 1;
    f->firstcplleak = 1;
    return b->overrun ? -1 : 0;
}

/* ---- audio blocks ------------------------------------------------------------------ */

typedef struct mant_groups {
    float b1[3], b2[3], b4[2];
    int n1, n2, n4;
} mant_groups;

/* n bits, signed (two's complement). */
static int32_t gsb(bits* b, int n)
{
    uint32_t u = gb(b, n);
    return (int32_t)(u << (32 - n)) >> (32 - n);
}

static float read_mantissa(bits* b, mant_groups* g, int bap)
{
    int v;
    switch (bap) {
    case 1:
        if (g->n1 == 0) {
            v = (int)gb(b, 5);
            if (v > 26) v = 26;
            memcpy(g->b1, b1q[v], sizeof(g->b1));
            g->n1 = 3;
        }
        return g->b1[3 - g->n1--];
    case 2:
        if (g->n2 == 0) {
            v = (int)gb(b, 7);
            if (v > 124) v = 124;
            memcpy(g->b2, b2q[v], sizeof(g->b2));
            g->n2 = 3;
        }
        return g->b2[3 - g->n2--];
    case 3: return b3q[gb(b, 3)];
    case 4:
        if (g->n4 == 0) {
            v = (int)gb(b, 7);
            if (v > 120) v = 120;
            memcpy(g->b4, b4q[v], sizeof(g->b4));
            g->n4 = 2;
        }
        return g->b4[2 - g->n4--];
    case 5: return b5q[gb(b, 4)];
    default: {
        int n = qbits[bap];
        return (float)gsb(b, n) * pow2tab[n - 1];
    }
    }
}

/* Mantissas of channel ch, bins strtmant..endmant, into coef[] (bap 0 bins get
 * dither when `dither`, else 0; the coupling channel's are dithered by each
 * coupled channel's own flag in decouple). */
static void read_mantissas(fa_ac3* d, bits* b, mant_groups* g, int ch, int dither)
{
    frame* f = &d->f;
    int bin;
    for (bin = f->strtmant[ch]; bin < f->endmant[ch]; bin++) {
        int bap = f->bap[ch][bin];
        float m;
        if (bap == 0) {
            m = dither ? rnd(d) * 0.707f : 0.0f;
        } else {
            m = read_mantissa(b, g, bap);
        }
        f->coef[ch][bin] = m * pow2tab[f->exp[ch][bin]];
    }
}

/* One gain-adaptive quantized mantissa (E.2.4.4.2) of a bin with the given hebap
 * (8..19) and gain 2^lg. */
static float gaq_mantissa(bits* b, int hebap, int lg)
{
    int mb = aht_mbits[hebap - 8], n = mb - lg;
    int32_t x = gsb(b, n);
    float y, a, c;
    if (lg == 0) return ldexpf((float)x, 1 - mb) * (1.0f + (float)gaq_a1[hebap - 8] / 32768.0f);
    if (x != -(1 << (n - 1))) return ldexpf((float)x, 1 - n - lg);   /* small: attenuated by the gain */
    /* The tag: a large mantissa follows, from a dead-zone quantizer. */
    n = lg == 1 ? mb - 1 : mb;
    x = gsb(b, n);
    y = ldexpf((float)x, 1 - n);
    a = (float)(int16_t)gaq_a[lg - 1][hebap - 8] / 32768.0f;
    c = x >= 0 ? ldexpf(1.0f, -lg) : (float)(int16_t)gaq_bneg[lg - 1][hebap - 8] / 32768.0f;
    return y + a * y + c;
}

/* The AHT-coded mantissas of channel ch for all six blocks, read in block 0, and
 * their inverse DCT into aht[ch][block][bin] (E.2.4). bap[] holds hebap[]. */
static int read_aht(bits* b, frame* f, int ch)
{
    int start = f->strtmant[ch], end = f->endmant[ch], bin, j, m, k, nactive = 0;
    int gaqmod = (int)gb(b, 2), endbap = gaqmod < 2 ? 12 : 17;
    uint8_t gain[258];   /* log2 of each gain-coded bin's gain, in order */
    memset(gain, 0, sizeof(gain));
    for (bin = start; bin < end; bin++)
        if (f->bap[ch][bin] > 7 && f->bap[ch][bin] < endbap) nactive++;
    if (gaqmod == 1 || gaqmod == 2) {
        for (k = 0; k < nactive; k++) gain[k] = (uint8_t)(gb1(b) ? gaqmod : 0);   /* G = 2 or 4 */
    } else if (gaqmod == 3) {
        for (k = 0; k < nactive; k += 3) {   /* three gains of 1, 2 or 4 per word */
            int v = (int)gb(b, 5);
            if (v > 26) v = 26;   /* reserved: clamped, like grouped mantissa codes */
            gain[k] = (uint8_t)(v / 9);
            gain[k + 1] = (uint8_t)((v % 9) / 3);
            gain[k + 2] = (uint8_t)(v % 3);
        }
    }
    k = 0;
    for (bin = start; bin < end; bin++) {
        int hb = f->bap[ch][bin];
        float x[6];
        if (hb == 0) {   /* no mantissas (dithered later, by dithflag) */
            for (m = 0; m < 6; m++) f->aht[ch][m][bin] = 0.0f;
            continue;
        }
        if (hb < 8) {
            uint32_t v = gb(b, aht_vqbits[hb]);
            const int16_t* q = hb == 1 ? aht_vq1[v] : hb == 2 ? aht_vq2[v] : hb == 3 ? aht_vq3[v] : hb == 4 ? aht_vq4[v]
                             : hb == 5 ? aht_vq5[v] : hb == 6 ? aht_vq6[v] : aht_vq7[v];
            for (j = 0; j < 6; j++) x[j] = (float)q[j] / 32768.0f;
        } else {
            int lg = hb < endbap ? gain[k++] : 0;
            for (j = 0; j < 6; j++) x[j] = gaq_mantissa(b, hb, lg);
        }
        for (m = 0; m < 6; m++) {
            float acc = 0.0f;
            for (j = 0; j < 6; j++) acc += aht_idct[m][j] * x[j];
            f->aht[ch][m][bin] = acc;
        }
    }
    return b->overrun ? -1 : 0;
}

/* Mantissas of channel ch in block blk, AHT-coded or not. */
static int channel_mantissas(fa_ac3* d, bits* b, mant_groups* g, int ch, int blk, int dither)
{
    frame* f = &d->f;
    int bin;
    if (!f->ahtinu[ch]) {
        read_mantissas(d, b, g, ch, dither);
        return 0;
    }
    if (blk == 0 && read_aht(b, f, ch) < 0) return -1;
    for (bin = f->strtmant[ch]; bin < f->endmant[ch]; bin++) {
        float m = f->aht[ch][blk][bin];
        if (f->bap[ch][bin] == 0 && dither) m = rnd(d) * 0.707f;
        f->coef[ch][bin] = m * pow2tab[f->exp[ch][bin]];
    }
    return 0;
}

static void decouple(fa_ac3* d)
{
    frame* f = &d->f;
    int ch, bnd, bin, k;
    for (ch = 0; ch < f->nfchans; ch++) {
        if (!f->chincpl[ch]) continue;
        if (f->ecplinu) {
            /* Enhanced coupling: an amplitude per band (E.2.5.5). */
            int sb;
            for (sb = f->ecpl_begin; sb < f->ecpl_end; sb++) {
                float amp = f->ecplamp[ch][f->ecplbnd[sb]];
                for (bin = ecplsubbndtab[sb]; bin < ecplsubbndtab[sb + 1]; bin++) {
                    float c = f->coef[AC3_CPL][bin];
                    if (f->bap[AC3_CPL][bin] == 0)
                        c = f->dithflag[ch] ? rnd(d) * 0.707f * pow2tab[f->exp[AC3_CPL][bin]] : 0.0f;
                    f->coef[ch][bin] = c * amp;
                }
            }
            continue;
        }
        bin = f->strtmant[AC3_CPL];
        for (bnd = 0; bnd < f->ncplbnd; bnd++) {
            float co = f->cplco[ch][bnd];
            if (f->h.acmod == 2 && ch == 1 && f->phsflg[bnd]) co = -co;
            for (k = 0; k < f->cplbndsz[bnd]; k++, bin++) {
                float c = f->coef[AC3_CPL][bin];
                if (f->bap[AC3_CPL][bin] == 0)   /* dither, scaled like the rest of the band */
                    c = f->dithflag[ch] ? rnd(d) * 0.707f * pow2tab[f->exp[AC3_CPL][bin]] : 0.0f;
                f->coef[ch][bin] = c * co;
            }
        }
    }
}

/* Spectral extension (TS 102 366 E.3.6): the band below spx_start is copied up,
 * wrapping around, notched at the seams, and scaled to the transmitted envelope
 * with noise blended in. */
static void spectral_extension(fa_ac3* d)
{
    frame* f = &d->f;
    int bnd, ch, bin, i, nsect = 0;
    int wrap[17], copy[18];
    float rms[17];
    memset(wrap, 0, sizeof(wrap));
    wrap[0] = 1;
    bin = f->spx_copy_start;
    for (bnd = 0; bnd < f->nspxbnd; bnd++) {
        int size = f->spxbndsz[bnd], step;
        if (bin + size > f->spx_start) {
            copy[nsect++] = bin - f->spx_copy_start;
            bin = f->spx_copy_start;
            wrap[bnd] = 1;
        }
        for (i = 0; i < size; i += step) {
            if (bin == f->spx_start) {
                copy[nsect++] = bin - f->spx_copy_start;
                bin = f->spx_copy_start;
            }
            step = imin(size - i, f->spx_start - bin);
            bin += step;
        }
    }
    copy[nsect++] = bin - f->spx_copy_start;

    for (ch = 0; ch < f->nfchans; ch++) {
        float* c = f->coef[ch];
        if (!f->chinspx[ch]) continue;
        bin = f->spx_start;
        for (i = 0; i < nsect; i++) {
            memmove(&c[bin], &c[f->spx_copy_start], (size_t)copy[i] * sizeof(float));
            bin += copy[i];
        }
        bin = f->spx_start;
        for (bnd = 0; bnd < f->nspxbnd; bnd++) {
            double acc = 0.0;
            for (i = 0; i < f->spxbndsz[bnd]; i++, bin++) acc += (double)c[bin] * c[bin];
            rms[bnd] = (float)sqrt(acc / f->spxbndsz[bnd]);
        }
        if (f->spxattencod[ch] >= 0) {
            const float* at = spxatten[f->spxattencod[ch]];
            bin = f->spx_start - 2;
            for (bnd = 0; bnd < f->nspxbnd; bnd++) {
                if (wrap[bnd]) {
                    c[bin] *= at[0];
                    c[bin + 1] *= at[1];
                    c[bin + 2] *= at[2];
                    c[bin + 3] *= at[1];
                    c[bin + 4] *= at[0];
                }
                bin += f->spxbndsz[bnd];
            }
        }
        bin = f->spx_start;
        for (bnd = 0; bnd < f->nspxbnd; bnd++) {
            float ns = f->spx_nblend[ch][bnd] * rms[bnd], ss = f->spx_sblend[ch][bnd];
            for (i = 0; i < f->spxbndsz[bnd]; i++, bin++) c[bin] = c[bin] * ss + ns * rnd(d);
        }
    }
}

/* The rematrixing bands below where coupling or spectral extension begins
 * (A/52 7.5.2, TS 102 366 E.2.3.2). */
static int rematrix_bands(const frame* f, int cplinu)
{
    int limit = cplinu ? f->strtmant[AC3_CPL] : f->spxinu ? f->spx_start : 253, n = 0;
    while (n < 4 && rematbnd[n] < limit) n++;
    return n;
}

static int parse_block(fa_ac3* d, bits* b, int blk)
{
    frame* f = &d->f;
    const fa_ac3_header* h = &f->h;
    int eac3 = h->eac3, acmod = h->acmod, nfch = f->nfchans, ch, i, bnd;
    int cplinu;
    mant_groups g;

    /* Block switch and dither flags. */
    for (ch = 0; ch < nfch; ch++) f->blksw[ch] = (!eac3 || f->blkswe) ? gb1(b) : 0;
    for (ch = 0; ch < nfch; ch++) {
        if (!eac3 || f->dithflage) f->dithflag[ch] = gb1(b);
        else f->dithflag[ch] = 1;
    }
    /* Dynamic range. */
    for (i = 0; i < (acmod == 0 ? 2 : 1); i++) {
        if (gb1(b)) f->dynrng[i] = drc_gain((int)gb(b, 8));
        else if (blk == 0) f->dynrng[i] = 1.0f;
    }

    /* Spectral extension strategy (E-AC-3). */
    if (eac3 && (blk == 0 || gb1(b))) {
        f->spxinu = gb1(b);
        if (f->spxinu) {
            int strtf, begf, endf, sb_begin, sb_end;
            if (acmod == 1) f->chinspx[0] = 1;
            else for (ch = 0; ch < nfch; ch++) f->chinspx[ch] = gb1(b);
            strtf = (int)gb(b, 2);
            begf = (int)gb(b, 3);
            endf = (int)gb(b, 3);
            sb_begin = begf < 6 ? begf + 2 : begf * 2 - 3;
            sb_end = endf < 3 ? endf + 5 : endf * 2 + 3;
            f->spx_copy_start = strtf * 12 + 25;
            f->spx_start = sb_begin * 12 + 25;
            f->spx_end = sb_end * 12 + 25;
            if (sb_begin >= sb_end || f->spx_copy_start >= f->spx_start) return -1;
            if (blk == 0) memcpy(f->spxbndstrc, def_spxbndstrc, sizeof(f->spxbndstrc));
            if (gb1(b))
                for (bnd = sb_begin + 1; bnd < sb_end; bnd++) f->spxbndstrc[bnd] = (uint8_t)gb1(b);
            f->nspxbnd = band_sizes(f->spxbndstrc, sb_begin, sb_end, f->spxbndsz);
        }
    }
    if (!eac3 || !f->spxinu) {
        f->spxinu = 0;
        for (ch = 0; ch < AC3_MAX_FBW; ch++) { f->chinspx[ch] = 0; f->firstspxcos[ch] = 1; }
    }
    /* Spectral extension coordinates. */
    if (f->spxinu) {
        for (ch = 0; ch < nfch; ch++) {
            if (!f->chinspx[ch]) { f->firstspxcos[ch] = 1; continue; }
            if (f->firstspxcos[ch] || gb1(b)) {
                float blend = (float)gb(b, 5) / 32.0f;
                int mstr = (int)gb(b, 2) * 3, bin = f->spx_start;
                f->firstspxcos[ch] = 0;
                for (bnd = 0; bnd < f->nspxbnd; bnd++) {
                    int size = f->spxbndsz[bnd], e, m;
                    float nratio, co;
                    nratio = (float)(bin + size / 2) / (float)f->spx_end - blend;
                    if (nratio < 0.0f) nratio = 0.0f;
                    if (nratio > 1.0f) nratio = 1.0f;
                    bin += size;
                    e = (int)gb(b, 4);
                    m = (int)gb(b, 2);
                    /* spxco = (m + 4) / 8 (or m / 4 at exponent 15) * 2^-(e + 3 mstr), times 32. */
                    co = e == 15 ? ldexpf((float)m, 3 - e - mstr) : ldexpf((float)(m + 4), 2 - e - mstr);
                    f->spx_nblend[ch][bnd] = sqrtf(3.0f * nratio) * co;   /* the noise has variance 1/3 */
                    f->spx_sblend[ch][bnd] = sqrtf(1.0f - nratio) * co;
                }
            }
        }
    }

    /* Coupling strategy. */
    if (eac3 ? f->cplstre[blk] : gb1(b)) {
        if (!eac3) f->cplinu_blk[blk] = gb1(b);
        f->cplinu = f->cplinu_blk[blk];
        if (f->cplinu) {
            int endsub, was_ecpl = f->ecplinu;
            if (acmod < 2) return -1;
            f->ecplinu = eac3 ? gb1(b) : 0;
            if (eac3 && acmod == 2) f->chincpl[0] = f->chincpl[1] = 1;
            else for (ch = 0; ch < nfch; ch++) f->chincpl[ch] = gb1(b);
            if (f->ecplinu) {
                /* Enhanced coupling (E.1.3.3.16 to E.1.3.3.19): sub-bands of 6 bins
                 * up to bin 37, 12 above, combined into bands above sub-band 8. */
                int begf = (int)gb(b, 4), begin, end, sb, first;
                begin = begf < 3 ? begf * 2 : begf < 13 ? begf + 2 : begf * 2 - 10;
                end = f->spxinu ? (f->spx_start - 25) / 12 + 3 : (int)gb(b, 4) + 7;
                if (begin >= end || end > 22) return -1;
                f->ecpl_begin = begin;
                f->ecpl_end = end;
                if (blk == 0 || !was_ecpl) memcpy(f->ecplbndstrc, def_ecplbndstrc, sizeof(f->ecplbndstrc));
                first = imax(9, begin + 1);
                if (gb1(b))
                    for (sb = first; sb < end; sb++) f->ecplbndstrc[sb] = (uint8_t)gb1(b);
                for (sb = 0; sb < first; sb++) f->ecplbndstrc[sb] = 0;
                f->necplbnd = 0;
                for (sb = begin; sb < end; sb++) {
                    if (sb > begin && !f->ecplbndstrc[sb]) f->necplbnd++;
                    f->ecplbnd[sb] = (uint8_t)f->necplbnd;
                }
                f->necplbnd++;
                f->strtmant[AC3_CPL] = ecplsubbndtab[begin];
                f->endmant[AC3_CPL] = ecplsubbndtab[end];
                f->phsflginu = 0;
                goto cpl_done;
            }
            f->phsflginu = acmod == 2 ? gb1(b) : 0;
            f->cplbegf = (int)gb(b, 4);
            endsub = f->spxinu ? (f->spx_start - 37) / 12 : (int)gb(b, 4) + 3;
            if (f->cplbegf >= endsub) return -1;
            f->strtmant[AC3_CPL] = f->cplbegf * 12 + 37;
            f->endmant[AC3_CPL] = endsub * 12 + 37;
            if (blk == 0) memcpy(f->cplbndstrc, def_cplbndstrc, sizeof(f->cplbndstrc));
            if (!eac3 || gb1(b))
                for (bnd = f->cplbegf + 1; bnd < endsub; bnd++) f->cplbndstrc[bnd] = (uint8_t)gb1(b);
            f->ncplbnd = band_sizes(f->cplbndstrc, f->cplbegf, endsub, f->cplbndsz);
        cpl_done:;
        } else {
            for (ch = 0; ch < AC3_MAX_FBW; ch++) { f->chincpl[ch] = 0; f->firstcplcos[ch] = 1; }
            f->firstcplleak = 1;
            f->phsflginu = 0;
            f->ecplinu = 0;
        }
    } else if (!eac3) {
        if (blk == 0) return -1;
        f->cplinu_blk[blk] = f->cplinu_blk[blk - 1];
    } else {
        f->cplinu = f->cplinu_blk[blk];
    }
    cplinu = f->cplinu = f->cplinu_blk[blk];

    /* Coupling coordinates and phase flags, or enhanced coupling amplitudes
     * (the angle and chaos parameters that may follow them are reserved). */
    if (cplinu && f->ecplinu) {
        int first = -1;
        skipb(b, 1);
        for (ch = 0; ch < nfch; ch++) {
            int param1e, rsvd;
            if (!f->chincpl[ch]) { f->firstcplcos[ch] = 1; continue; }
            if (first < 0) first = ch;
            if (f->firstcplcos[ch]) {
                param1e = 1;
                rsvd = ch > first;
                f->firstcplcos[ch] = 0;
            } else {
                param1e = gb1(b);
                rsvd = ch > first ? gb1(b) : 0;
            }
            if (param1e)
                for (bnd = 0; bnd < f->necplbnd; bnd++) f->ecplamp[ch][bnd] = ecplamptab[gb(b, 5)];
            if (rsvd) skipb(b, 9 * (size_t)(f->necplbnd - 1));
            if (ch > first) skipb(b, 1);
        }
    } else if (cplinu) {
        int any = 0;
        for (ch = 0; ch < nfch; ch++) {
            if (!f->chincpl[ch]) { f->firstcplcos[ch] = 1; continue; }
            if ((eac3 && f->firstcplcos[ch]) || gb1(b)) {
                int mstr = (int)gb(b, 2) * 3;
                f->firstcplcos[ch] = 0;
                any = 1;
                for (bnd = 0; bnd < f->ncplbnd; bnd++) {
                    int e = (int)gb(b, 4), m = (int)gb(b, 4);
                    /* cplco = (m + 16) / 32 (or m / 16 at exponent 15) * 2^-(e + 3 mstr),
                     * times 8 (the coupled channel may be 18 dB above the coupling channel). */
                    f->cplco[ch][bnd] = e == 15 ? ldexpf((float)m, -1 - e - mstr) : ldexpf((float)(m + 16), -2 - e - mstr);
                }
            } else if (blk == 0) {
                return -1;
            }
        }
        if (acmod == 2 && any)
            for (bnd = 0; bnd < f->ncplbnd; bnd++) f->phsflg[bnd] = f->phsflginu ? gb1(b) : 0;
    }

    /* Rematrixing. */
    if (acmod == 2) {
        if ((eac3 && blk == 0) || gb1(b)) {
            f->nrematbd = rematrix_bands(f, cplinu);
            for (bnd = 0; bnd < f->nrematbd; bnd++) f->rematflg[bnd] = gb1(b);
        } else if (blk == 0) {
            f->nrematbd = 0;
        }
    }

    /* Exponent strategies. */
    if (!eac3) {
        if (cplinu) f->expstr[blk][AC3_CPL] = (int)gb(b, 2);
        for (ch = 0; ch < nfch; ch++) f->expstr[blk][ch] = (int)gb(b, 2);
        if (h->lfeon) f->expstr[blk][AC3_LFE] = gb1(b);
    }
    /* Channel bandwidths. */
    for (ch = 0; ch < nfch; ch++) {
        int es = f->expstr[blk][ch];
        f->strtmant[ch] = 0;
        if (es == 0) {
            if (blk == 0) return -1;
            continue;
        }
        if (f->chincpl[ch] && cplinu) f->endmant[ch] = f->strtmant[AC3_CPL];
        else if (f->chinspx[ch]) f->endmant[ch] = f->spx_start;
        else {
            int bw = (int)gb(b, 6);
            if (bw > 60) return -1;
            f->endmant[ch] = bw * 3 + 73;
        }
        f->ngrps[ch] = (f->endmant[ch] + (3 << (es - 1)) - 4) / (3 << (es - 1));
    }
    if (cplinu && f->expstr[blk][AC3_CPL] != 0)
        f->ngrps[AC3_CPL] = (f->endmant[AC3_CPL] - f->strtmant[AC3_CPL]) / (3 << (f->expstr[blk][AC3_CPL] - 1));
    /* Exponents. */
    if (cplinu) {
        int es = f->expstr[blk][AC3_CPL];
        if (es != 0) {
            int absexp = (int)gb(b, 4) << 1;
            if (read_exponents(b, f, AC3_CPL, es, absexp, f->strtmant[AC3_CPL]) < 0) return -1;
        } else if (blk == 0) {
            return -1;
        }
    }
    for (ch = 0; ch < nfch; ch++) {
        int es = f->expstr[blk][ch];
        if (es != 0) {
            int absexp = (int)gb(b, 4);
            f->exp[ch][0] = (uint8_t)absexp;
            if (read_exponents(b, f, ch, es, absexp, 1) < 0) return -1;
            skipb(b, 2);                                      /* gainrng */
        }
    }
    if (h->lfeon) {
        if (f->expstr[blk][AC3_LFE] != 0) {
            int absexp = (int)gb(b, 4);
            f->strtmant[AC3_LFE] = 0;
            f->endmant[AC3_LFE] = 7;
            f->ngrps[AC3_LFE] = 2;
            f->exp[AC3_LFE][0] = (uint8_t)absexp;
            if (read_exponents(b, f, AC3_LFE, 1, absexp, 1) < 0) return -1;
        } else if (blk == 0) {
            return -1;
        }
    }

    /* Bit allocation parameters. */
    if (!eac3 || f->bamode) {
        if (gb1(b)) {
            f->sdecay = slowdec[gb(b, 2)] >> f->sr_shift;
            f->fdecay = fastdec[gb(b, 2)] >> f->sr_shift;
            f->sgain = slowgain[gb(b, 2)];
            f->dbknee = dbpbtab[gb(b, 2)];
            f->floorv = floortab[gb(b, 3)];
        } else if (blk == 0) {
            return -1;
        }
    } else if (blk == 0) {
        f->sdecay = slowdec[2] >> f->sr_shift;
        f->fdecay = fastdec[1] >> f->sr_shift;
        f->sgain = slowgain[1];
        f->dbknee = dbpbtab[2];
        f->floorv = floortab[7];
    }
    /* SNR offsets and fast gains. */
    if (!eac3) {
        if (gb1(b)) {
            int csnr = (int)gb(b, 6);
            if (cplinu) {
                f->snroffset[AC3_CPL] = ((csnr - 15) * 16 + (int)gb(b, 4)) * 4;
                f->fgain[AC3_CPL] = fastgain[gb(b, 3)];
            }
            for (ch = 0; ch < nfch; ch++) {
                f->snroffset[ch] = ((csnr - 15) * 16 + (int)gb(b, 4)) * 4;
                f->fgain[ch] = fastgain[gb(b, 3)];
            }
            if (h->lfeon) {
                f->snroffset[AC3_LFE] = ((csnr - 15) * 16 + (int)gb(b, 4)) * 4;
                f->fgain[AC3_LFE] = fastgain[gb(b, 3)];
            }
        } else if (blk == 0) {
            return -1;
        }
    } else {
        /* Block-level offsets: always in block 0, then when they change. */
        if (f->snroffststr != 0 && (blk == 0 || gb1(b))) {
            int csnr = (int)gb(b, 6), v;
            if (f->snroffststr == 1) {
                v = ((csnr - 15) * 16 + (int)gb(b, 4)) * 4;
                for (ch = 0; ch < AC3_NCH; ch++) f->snroffset[ch] = v;
            } else {
                if (cplinu) f->snroffset[AC3_CPL] = ((csnr - 15) * 16 + (int)gb(b, 4)) * 4;
                for (ch = 0; ch < nfch; ch++) f->snroffset[ch] = ((csnr - 15) * 16 + (int)gb(b, 4)) * 4;
                if (h->lfeon) f->snroffset[AC3_LFE] = ((csnr - 15) * 16 + (int)gb(b, 4)) * 4;
            }
        }
        /* Fast gains: sent, or the default in every block that has none. */
        if (f->frmfgaincode && gb1(b)) {
            if (cplinu) f->fgain[AC3_CPL] = fastgain[gb(b, 3)];
            for (ch = 0; ch < nfch; ch++) f->fgain[ch] = fastgain[gb(b, 3)];
            if (h->lfeon) f->fgain[AC3_LFE] = fastgain[gb(b, 3)];
        } else {
            for (ch = 0; ch < AC3_NCH; ch++) if (ch != AC3_CPL || cplinu) f->fgain[ch] = fastgain[4];
        }
        if (h->strmtyp == 0 && gb1(b)) skipb(b, 10);           /* convsnroffst */
    }
    /* Coupling leak. */
    if (cplinu) {
        if ((eac3 && f->firstcplleak) || gb1(b)) {
            f->cplfleak = (int)gb(b, 3);
            f->cplsleak = (int)gb(b, 3);
        } else if (!eac3 && blk == 0) {
            return -1;
        }
        f->firstcplleak = 0;
    }
    /* Delta bit allocation. */
    if ((!eac3 || f->dbaflde) && gb1(b)) {
        int seg;
        if (cplinu) f->deltbae[AC3_CPL] = (int)gb(b, 2);
        for (ch = 0; ch < nfch; ch++) f->deltbae[ch] = (int)gb(b, 2);
        for (i = 0; i <= nfch; i++) {
            int c = i == 0 ? AC3_CPL : i - 1;
            if (c == AC3_CPL && !cplinu) continue;
            if (f->deltbae[c] == 3) return -1;
            if (f->deltbae[c] != 1) continue;
            f->deltnseg[c] = (int)gb(b, 3) + 1;
            for (seg = 0; seg < f->deltnseg[c]; seg++) {
                f->deltoffst[c][seg] = (uint8_t)gb(b, 5);
                f->deltlen[c][seg] = (uint8_t)gb(b, 4);
                f->deltba[c][seg] = (uint8_t)gb(b, 3);
            }
        }
    } else if (blk == 0) {
        for (ch = 0; ch < AC3_NCH; ch++) f->deltbae[ch] = 2;
    }
    /* Skip field. */
    if ((!eac3 || f->skipflde) && gb1(b)) skipb(b, (size_t)gb(b, 9) * 8);
    if (b->overrun) return -1;

    /* Bit allocation (skipped for a channel whose inputs did not change).
     * AHT channels' (hebap) only in block 0, where all their mantissas are. */
    for (i = 0; i < nfch + 2; i++) {
        ch = i == 0 ? AC3_CPL : i <= nfch ? i - 1 : AC3_LFE;
        if (ch == AC3_CPL ? !cplinu : ch == AC3_LFE && !h->lfeon) continue;
        if (blk > 0 && f->ahtinu[ch]) continue;
        if (bit_alloc(f, ch) < 0) return -1;
    }

    /* Mantissas: each channel's, the coupling channel's right after the first
     * coupled channel, then the LFE. Grouped mantissas carry across channels. */
    memset(&g, 0, sizeof(g));
    {
        int got_cpl = 0;
        for (ch = 0; ch < nfch; ch++) {
            if (channel_mantissas(d, b, &g, ch, blk, f->dithflag[ch]) < 0) return -1;
            if (cplinu && f->chincpl[ch] && !got_cpl) {
                if (channel_mantissas(d, b, &g, AC3_CPL, blk, 0) < 0) return -1;
                got_cpl = 1;
            }
        }
        if (h->lfeon && channel_mantissas(d, b, &g, AC3_LFE, blk, 0) < 0) return -1;
    }
    if (b->overrun) return -1;

    /* Rebuild every channel's full spectrum. */
    for (ch = 0; ch < nfch; ch++) {
        int end = f->chincpl[ch] && cplinu ? f->endmant[AC3_CPL] : f->endmant[ch];
        if (f->chinspx[ch]) end = f->spx_end;
        for (i = end; i < 256; i++) f->coef[ch][i] = 0.0f;
    }
    if (h->lfeon) for (i = 7; i < 256; i++) f->coef[AC3_LFE][i] = 0.0f;
    if (cplinu) decouple(d);
    /* Rematrixing first: the extension copies each channel's own low band. */
    if (acmod == 2) {
        /* Up to the lower channel's end (coupled channels end where coupling
         * starts, extended ones where the extension starts). */
        int end = imin(f->endmant[0], f->endmant[1]);
        for (bnd = 0; bnd < f->nrematbd; bnd++) {
            int k, e = imin(rematbnd[bnd + 1], end);
            if (!f->rematflg[bnd]) continue;
            for (k = rematbnd[bnd]; k < e; k++) {
                float l = f->coef[0][k], r = f->coef[1][k];
                f->coef[0][k] = l + r;
                f->coef[1][k] = l - r;
            }
        }
    }
    if (f->spxinu) spectral_extension(d);
    return 0;
}

/* ---- transform ------------------------------------------------------------------- */

/* Inverse transform of channel ch's block into out[256], with overlap delay[256]. */
static void synth(fa_ac3* d, const float* coef, int short_blocks, float gain, float* delay, float* out)
{
    float* z = d->tmp;
    int n;
    gain *= -2.0f;   /* the inverse of the forward transform's -2/N scaling */
    if (!short_blocks) {
        /* 512-point IMDCT, unfolded from the DCT-IV u[] (see mdct.h) straight into
         * the window: z[n] = u[n + 128], -u[383 - n], -u[n - 384] by quarter. */
        const float* u = d->u;
        fa_mdct_dct4(&d->t256, coef, d->u);
        for (n = 0; n < 128; n++) {
            out[n] = u[n + 128] * kbd[n] * gain + delay[n];
            out[n + 128] = -u[255 - n] * kbd[n + 128] * gain + delay[n + 128];
            delay[n] = -u[127 - n] * kbd[n + 256] * gain;
            delay[n + 128] = -u[n] * kbd[n + 384] * gain;
        }
        return;
    } else {
        /* Two 256-point transforms of the even and odd coefficients. The first has
         * phase n0 = 1/2, so it is odd about its centre like the first half of a
         * long block; the second (n0 = 128 + 1/2) is even like the second half. */
        float in[128];
        float* u = d->u;
        for (n = 0; n < 128; n++) in[n] = coef[2 * n];
        fa_mdct_dct4(&d->t128, in, u);
        for (n = 0; n < 128; n++) { z[n] = u[n]; z[255 - n] = -u[n]; }
        for (n = 0; n < 128; n++) in[n] = coef[2 * n + 1];
        fa_mdct_dct4(&d->t128, in, u);
        for (n = 0; n < 128; n++) { z[256 + 127 - n] = -u[n]; z[384 + n] = -u[n]; }
    }
    for (n = 0; n < AC3_BLOCK; n++) {
        out[n] = z[n] * kbd[n] * gain + delay[n];
        delay[n] = z[AC3_BLOCK + n] * kbd[AC3_BLOCK + n] * gain;
    }
}

/* ---- frames and access units ---------------------------------------------------- */

static const int8_t acmod_loc[8][5] = {
    { LOC_L, LOC_R, -1, -1, -1 },                  /* 1+1: Ch1, Ch2 */
    { LOC_C, -1, -1, -1, -1 },
    { LOC_L, LOC_R, -1, -1, -1 },
    { LOC_L, LOC_C, LOC_R, -1, -1 },
    { LOC_L, LOC_R, LOC_S, -1, -1 },
    { LOC_L, LOC_C, LOC_R, LOC_S, -1 },
    { LOC_L, LOC_R, LOC_LS, LOC_RS, -1 },
    { LOC_L, LOC_C, LOC_R, LOC_LS, LOC_RS },
};

/* chanmap bit (MSB = 0) -> one or two locations (Table E2.5). */
static const int8_t chanmap_loc[16][2] = {
    { LOC_L, -1 }, { LOC_C, -1 }, { LOC_R, -1 }, { LOC_LS, -1 }, { LOC_RS, -1 },
    { LOC_LC, LOC_RC }, { LOC_LRS, LOC_RRS }, { LOC_CS, -1 }, { LOC_TS, -1 },
    { LOC_LSD, LOC_RSD }, { LOC_LW, LOC_RW }, { LOC_VHL, LOC_VHR }, { LOC_VHC, -1 },
    { LOC_LTS, LOC_RTS }, { LOC_LFE2, -1 }, { LOC_LFE, -1 } };

/* Output layouts (see the top of the file): which locations go to which channel,
 * with fold-downs for locations a layout lacks. */
enum { OUT_FL, OUT_FR, OUT_FC, OUT_LFE, OUT_BC, OUT_SL, OUT_SR, OUT_BL, OUT_BR, OUT_N };

typedef struct mix_plan {
    int nch;
    int simple;                                     /* each channel one location at full gain */
    int nroute;
    struct { int ch, loc; float gain; } route[48];  /* one per ADD below at most */
    int src_loc[8];                                 /* simple: each channel's location, or -1 */
} mix_plan;

/* The output channels for the locations in use (u), and which of the locations
 * that have audio (src[loc] not NULL) go to which channel, with what gain. */
static void plan_mix(const int* u, float* const* src, mix_plan* m)
{
    static const int8_t layouts[9][8] = {
        { -1 },
        { OUT_FC, -1 },
        { OUT_FL, OUT_FR, -1 },
        { OUT_FL, OUT_FR, OUT_FC, -1 },
        { OUT_FL, OUT_FR, OUT_FC, OUT_BC, -1 },
        { OUT_FL, OUT_FR, OUT_FC, OUT_BL, OUT_BR, -1 },
        { OUT_FL, OUT_FR, OUT_FC, OUT_LFE, OUT_SL, OUT_SR, -1 },
        { OUT_FL, OUT_FR, OUT_FC, OUT_LFE, OUT_BC, OUT_SL, OUT_SR, -1 },
        { OUT_FL, OUT_FR, OUT_FC, OUT_LFE, OUT_BL, OUT_BR, OUT_SL, OUT_SR } };
    int has_lr = u[LOC_L] || u[LOC_R], has_back = u[LOC_LRS] || u[LOC_RRS];
    int has_surr = u[LOC_LS] || u[LOC_RS] || u[LOC_LSD] || u[LOC_RSD] || u[LOC_LTS] || u[LOC_RTS] || u[LOC_TS];
    int has_bc = u[LOC_S] || u[LOC_CS], has_lfe = u[LOC_LFE] || u[LOC_LFE2];
    int nch, i, c, k, slot[OUT_N], nroute = 0;
    const float h = 0.70710678f;

    if (has_back) nch = 8;
    else if (has_lfe) nch = has_bc ? 7 : 6;
    else if (has_surr) nch = has_bc ? 7 : 5;
    else if (has_bc) nch = 4;
    else if (u[LOC_C] && (has_lr || u[LOC_LC] || u[LOC_RC] || u[LOC_LW] || u[LOC_RW] || u[LOC_VHL] || u[LOC_VHR])) nch = 3;
    else if (u[LOC_C] && !u[LOC_VHC]) nch = 1;
    else nch = 2;
    for (i = 0; i < OUT_N; i++) slot[i] = -1;
    for (c = 0; c < nch; c++) slot[layouts[nch][c]] = c;

#define ADD(out, l, g) do { if (u[l] && slot[out] >= 0 && src[l]) { \
        m->route[nroute].ch = slot[out]; m->route[nroute].loc = (l); m->route[nroute].gain = (g); nroute++; } } while (0)
    if (nch == 1) {
        ADD(OUT_FC, LOC_C, 1.0f);
        ADD(OUT_FC, LOC_VHC, 1.0f);
    } else {
        ADD(OUT_FL, LOC_L, 1.0f);  ADD(OUT_FR, LOC_R, 1.0f);
        ADD(OUT_FL, LOC_LC, 1.0f); ADD(OUT_FR, LOC_RC, 1.0f);
        ADD(OUT_FL, LOC_LW, 1.0f); ADD(OUT_FR, LOC_RW, 1.0f);
        ADD(OUT_FL, LOC_VHL, 1.0f); ADD(OUT_FR, LOC_VHR, 1.0f);
        if (slot[OUT_FC] >= 0) { ADD(OUT_FC, LOC_C, 1.0f); ADD(OUT_FC, LOC_VHC, 1.0f); }
        else { ADD(OUT_FL, LOC_C, h); ADD(OUT_FR, LOC_C, h); ADD(OUT_FL, LOC_VHC, h); ADD(OUT_FR, LOC_VHC, h); }
        ADD(OUT_LFE, LOC_LFE, 1.0f);
        ADD(OUT_LFE, LOC_LFE2, 1.0f);
        /* Surround pair: side channels in 6..8-channel layouts, back ones in 5. */
        if (nch == 5) {
            ADD(OUT_BL, LOC_LS, 1.0f); ADD(OUT_BR, LOC_RS, 1.0f);
            ADD(OUT_BL, LOC_LSD, 1.0f); ADD(OUT_BR, LOC_RSD, 1.0f);
            ADD(OUT_BL, LOC_LTS, 1.0f); ADD(OUT_BR, LOC_RTS, 1.0f);
            ADD(OUT_BL, LOC_TS, h); ADD(OUT_BR, LOC_TS, h);
        } else {
            ADD(OUT_SL, LOC_LS, 1.0f); ADD(OUT_SR, LOC_RS, 1.0f);
            ADD(OUT_SL, LOC_LSD, 1.0f); ADD(OUT_SR, LOC_RSD, 1.0f);
            ADD(OUT_SL, LOC_LTS, 1.0f); ADD(OUT_SR, LOC_RTS, 1.0f);
            ADD(OUT_SL, LOC_TS, h); ADD(OUT_SR, LOC_TS, h);
        }
        ADD(OUT_BL, LOC_LRS, 1.0f); ADD(OUT_BR, LOC_RRS, 1.0f);
        if (slot[OUT_BC] >= 0) { ADD(OUT_BC, LOC_S, 1.0f); ADD(OUT_BC, LOC_CS, 1.0f); }
        else { ADD(OUT_BL, LOC_S, h); ADD(OUT_BR, LOC_S, h); ADD(OUT_BL, LOC_CS, h); ADD(OUT_BR, LOC_CS, h); }
    }
#undef ADD
    m->nch = nch;
    m->nroute = nroute;
    /* Usually each channel has one location at full gain: then it is a plain interleave. */
    m->simple = 1;
    for (c = 0; c < nch; c++) m->src_loc[c] = -1;
    for (k = 0; k < nroute; k++) {
        if (m->route[k].gain != 1.0f || m->src_loc[m->route[k].ch] >= 0) m->simple = 0;
        m->src_loc[m->route[k].ch] = m->route[k].loc;
    }
}

/* Mixes `frames` samples of each location, from src[loc] + off, into dst (interleaved). */
static void render_mix(const mix_plan* m, float* const* src, size_t off, float* dst, int frames)
{
    static const float zeros[AC3_BLOCK * AC3_MAX_BLOCKS];
    int nch = m->nch, i, c, k;
    if (m->simple) {
        const float* s[8];
        for (c = 0; c < nch; c++) s[c] = m->src_loc[c] >= 0 ? src[m->src_loc[c]] + off : zeros;
        for (i = 0; i < frames; i++, dst += nch)
            for (c = 0; c < nch; c++) dst[c] = s[c][i];
    } else {
        memset(dst, 0, (size_t)frames * nch * sizeof(float));
        for (k = 0; k < m->nroute; k++) {
            const float* s_ = src[m->route[k].loc] + off;
            float* o_ = dst + m->route[k].ch;
            float g = m->route[k].gain;
            for (i = 0; i < frames; i++) o_[(size_t)i * nch] += s_[i] * g;
        }
    }
}

static int reserve_out(float** pcm, size_t* cap, size_t need)
{
    if (need > *cap) {
        float* p = (float*)realloc(*pcm, need * sizeof(float));
        if (!p) return -1;
        *pcm = p;
        *cap = need;
    }
    return 0;
}

static int mix_out(fa_ac3* d, int frames, float** pcm, size_t* cap, int* out_channels)
{
    mix_plan m;
    plan_mix(d->loc_used, d->loc, &m);
    if (reserve_out(pcm, cap, (size_t)frames * m.nch) < 0) return -1;
    render_mix(&m, d->loc, 0, *pcm, frames);
    *out_channels = m.nch;
    return 0;
}

/* A unit of one syncframe is mixed block by block straight into the output, without
 * the location buffers (which hold a whole unit, for dependent substreams to add to). */
typedef struct direct_out {
    float** pcm;
    size_t* cap;
    int channels;
} direct_out;

/* Decodes one syncframe into the location buffers at block offset `at`, or with
 * `direct`, into the output. Returns 0, -1 (damaged) or -2 (out of memory). */
static int decode_frame(fa_ac3* d, const uint8_t* p, const fa_ac3_header* h, int state, int at, int drc,
                        int* wrote, direct_out* direct)
{
    frame* f = &d->f;
    sub_state* ss = d->sub[state];
    bits b;
    int r, blk, ch, i;
    int8_t loc[6];
    float scratch[AC3_BLOCK];
    float* src[LOC_COUNT];
    mix_plan plan;

    memset(f, 0, offsetof(frame, exp));
    f->h = *h;
    if (crc16(p + 2, h->frame_size - 2) != 0) return -1;
    b.p = p;
    b.pos = 0;
    b.end = h->frame_size * 8;
    b.overrun = 0;
    if (parse_bsi(&b, f) < 0) return -1;
    if (h->eac3) {
        r = parse_audfrm(&b, f);
        if (r != 0) return r;
        if (f->ahte && !f->aht) {
            f->aht = (float(*)[6][256])malloc(AC3_NCH * sizeof(*f->aht));
            if (!f->aht) return -2;
        }
    }
    /* Where this substream's channels go. */
    for (ch = 0; ch < 6; ch++) loc[ch] = -1;
    if (f->chanmape) {
        int k = 0, bit;
        for (bit = 0; bit < 16 && k < f->nfchans + h->lfeon; bit++) {
            if (!(f->chanmap & (0x8000u >> bit))) continue;
            for (i = 0; i < 2 && chanmap_loc[bit][i] >= 0 && k < f->nfchans + h->lfeon; i++) {
                /* The LFE of the substream is its last channel. */
                if (k < f->nfchans) loc[k] = chanmap_loc[bit][i];
                else loc[AC3_LFE] = chanmap_loc[bit][i];
                k++;
            }
        }
    } else {
        for (ch = 0; ch < f->nfchans; ch++) loc[ch] = acmod_loc[h->acmod][ch];
        if (h->lfeon) loc[AC3_LFE] = LOC_LFE;
    }
    if (direct) {
        int used[LOC_COUNT];
        memset(used, 0, sizeof(used));
        memset(src, 0, sizeof(src));
        for (ch = 0; ch < 6; ch++) if (loc[ch] >= 0) { used[loc[ch]] = 1; src[loc[ch]] = d->blk[ch]; }
        plan_mix(used, src, &plan);
        if (reserve_out(direct->pcm, direct->cap, (size_t)h->blocks * AC3_BLOCK * plan.nch) < 0) return -2;
        direct->channels = plan.nch;
    } else {
        for (ch = 0; ch < 6; ch++) {
            if (loc[ch] < 0 || d->loc[loc[ch]]) continue;
            d->loc[loc[ch]] = (float*)calloc(AC3_BLOCK * AC3_MAX_BLOCKS, sizeof(float));
            if (!d->loc[loc[ch]]) return -2;
        }
    }
    {
        /* A new layout (or a substream that comes back) starts with a clean overlap. */
        uint32_t layout = (uint32_t)h->acmod | (uint32_t)h->lfeon << 3 | (f->chanmape ? f->chanmap << 4 : 0);
        if (!ss->used || ss->layout != layout) memset(ss, 0, sizeof(*ss));
        ss->used = 1;
        ss->seen = 1;
        ss->layout = layout;
    }

    for (blk = 0; blk < h->blocks; blk++) {
        r = parse_block(d, &b, blk);
        if (r != 0) return r;
        for (ch = 0; ch < 6; ch++) {
            float gain, *dst;
            int prog = h->acmod == 0 && ch == 1;
            if (ch < AC3_LFE ? ch >= f->nfchans : !h->lfeon) continue;
            gain = 1.0f;
            if (drc == FA_AC3_DRC_NIGHT && f->compre[prog]) gain = f->compr[prog];
            else if (drc != FA_AC3_DRC_OFF) gain = f->dynrng[prog];
            /* Straight into the location's buffer (a channel without one still
             * keeps its overlap going). */
            if (loc[ch] < 0) {
                dst = scratch;
            } else if (direct) {
                dst = d->blk[ch];
            } else {
                dst = d->loc[loc[ch]] + (size_t)(at + blk) * AC3_BLOCK;
                wrote[loc[ch]] = 1;
            }
            synth(d, f->coef[ch], ch < AC3_LFE && f->blksw[ch], gain, ss->delay[ch], dst);
        }
        if (direct) render_mix(&plan, src, 0, *direct->pcm + (size_t)blk * AC3_BLOCK * plan.nch, AC3_BLOCK);
    }
    /* The blocks must leave room for the error check (crcrsv and crc2): a
     * stream read past that was not read the way it was written. */
    if (b.pos + 17 > b.end) return -1;
    ss->locs = 0;
    for (ch = 0; ch < 6; ch++)
        if (loc[ch] >= 0) { d->loc_used[loc[ch]] = 1; ss->locs |= 1u << loc[ch]; }
    return 0;
}

/* A damaged unit with no channels of its own: the last good layout, so the
 * silence has the stream's shape (stereo before any). */
static void keep_last_layout(fa_ac3* d)
{
    int i, any = 0;
    for (i = 0; i < LOC_COUNT; i++) { d->loc_used[i] = d->last_used[i]; any |= d->last_used[i]; }
    if (!any) d->loc_used[LOC_L] = d->loc_used[LOC_R] = 1;
}

static fa_result decode_single(fa_ac3* d, const uint8_t* data, const fa_ac3_header* h, int drc,
                               float** pcm, size_t* cap, int* frames, int* channels, uint32_t* rate)
{
    direct_out out;
    int wrote[LOC_COUNT], res, i;
    out.pcm = pcm;
    out.cap = cap;
    out.channels = 0;
    memset(wrote, 0, sizeof(wrote));
    if (!d->sub[0]) {
        d->sub[0] = (sub_state*)calloc(1, sizeof(sub_state));
        if (!d->sub[0]) return FA_ERROR_OUT_OF_MEMORY;
    }
    res = decode_frame(d, data, h, 0, 0, drc, wrote, &out);
    if (res == -2) return FA_ERROR_OUT_OF_MEMORY;
    *frames = h->blocks * AC3_BLOCK;
    *rate = h->rate;
    for (i = 1; i < AC3_MAX_SUBSTREAMS; i++) if (d->sub[i]) d->sub[i]->used = 0;
    if (res != 0) {
        /* Silence in the last good layout, and a clean restart of the overlap. */
        float* none[LOC_COUNT] = { 0 };
        mix_plan m;
        memset(d->sub[0]->delay, 0, sizeof(d->sub[0]->delay));
        keep_last_layout(d);
        plan_mix(d->loc_used, none, &m);
        if (reserve_out(pcm, cap, (size_t)*frames * m.nch) < 0) return FA_ERROR_OUT_OF_MEMORY;
        render_mix(&m, none, 0, *pcm, *frames);
        *channels = m.nch;
        return FA_ERROR_GENERIC;
    }
    *channels = out.channels;
    memcpy(d->last_used, d->loc_used, sizeof(d->loc_used));
    return FA_OK;
}

fa_result fa_ac3_decode(fa_ac3* d, const uint8_t* data, size_t size, int drc,
                        float** pcm, size_t* cap, int* frames, int* channels, uint32_t* rate)
{
    size_t pos = 0;
    int blocks = 0, err = 0, i;
    uint32_t r = 0;
    *frames = 0;
    memset(d->loc_used, 0, sizeof(d->loc_used));
    for (i = 0; i < AC3_MAX_SUBSTREAMS; i++) if (d->sub[i]) d->sub[i]->seen = 0;
    {
        /* The usual unit, one AC-3 frame or one E-AC-3 frame of program 0 alone:
         * straight into the output. */
        fa_ac3_header h;
        if (fa_ac3_parse_header(data, size, &h) && h.frame_size == size && fa_ac3_starts_unit(&h))
            return decode_single(d, data, &h, drc, pcm, cap, frames, channels, rate);
    }
    /* Frames in order: an independent frame of program 0 starts a new set of
     * blocks; its dependent frames cover the same blocks. Other programs are skipped. */
    while (pos + 8 <= size) {
        fa_ac3_header h;
        int at, res, state, wrote[LOC_COUNT];
        if (!fa_ac3_parse_header(data + pos, size - pos, &h) || h.frame_size > size - pos) break;
        if (h.eac3 && h.strmtyp != 1 && h.substreamid != 0) { pos += h.frame_size; continue; }
        if (h.eac3 && h.strmtyp == 1 && blocks == 0) { pos += h.frame_size; continue; }   /* no base */
        if (!h.eac3 || h.strmtyp != 1) {
            if (blocks + h.blocks > AC3_MAX_BLOCKS && blocks > 0) break;
            if (blocks > 0 && r != h.rate) break;
            at = blocks;
            blocks += h.blocks;
            r = h.rate;
            state = 0;
        } else {
            at = blocks - h.blocks;
            if (at < 0) { pos += h.frame_size; continue; }
            state = 1 + h.substreamid;
        }
        memset(wrote, 0, sizeof(wrote));
        if (!d->sub[state]) {
            d->sub[state] = (sub_state*)calloc(1, sizeof(sub_state));
            if (!d->sub[state]) return FA_ERROR_OUT_OF_MEMORY;
        }
        res = decode_frame(d, data + pos, &h, state, at, drc, wrote, NULL);
        if (res == -2) return FA_ERROR_OUT_OF_MEMORY;
        if (res != 0) {
            int dependent = h.eac3 && h.strmtyp == 1, any = 0;
            sub_state* ss = d->sub[state];
            err = res;
            /* Silence for the frame, and a clean restart of its overlap. A damaged
             * dependent frame silences only its own channels, keeping the layout:
             * the bed, and what the bed has of the channels it replaces, stay. */
            for (i = 0; i < LOC_COUNT; i++) {
                int stale = dependent && (ss->locs >> i & 1) && !d->loc_used[i];
                if ((!dependent || wrote[i] || stale) && d->loc[i])
                    memset(d->loc[i] + (size_t)at * AC3_BLOCK, 0, (size_t)h.blocks * AC3_BLOCK * sizeof(float));
                if (stale || (dependent && wrote[i])) d->loc_used[i] = 1;
            }
            memset(ss->delay, 0, sizeof(ss->delay));
            if (dependent) { ss->seen = ss->used; pos += h.frame_size; continue; }
            for (i = 0; i < LOC_COUNT; i++) any |= d->loc_used[i];
            if (!any) keep_last_layout(d);
        }
        pos += h.frame_size;
    }
    if (blocks == 0) return FA_ERROR_GENERIC;
    *frames = blocks * AC3_BLOCK;
    if (mix_out(d, *frames, pcm, cap, channels) < 0) return FA_ERROR_OUT_OF_MEMORY;
    *rate = r;
    /* A dependent substream missing from this unit starts clean when it returns. */
    for (i = 1; i < AC3_MAX_SUBSTREAMS; i++) if (d->sub[i] && !d->sub[i]->seen) d->sub[i]->used = 0;
    if (err == 0) memcpy(d->last_used, d->loc_used, sizeof(d->loc_used));
    if (err < 0) return FA_ERROR_GENERIC;
    return FA_OK;
}
