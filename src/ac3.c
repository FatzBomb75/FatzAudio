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
 * their 5.1 or 7.1 bed. Not decoded here: the adaptive hybrid transform and
 * enhanced coupling (fa_ac3_decode returns FA_ERROR_UNSUPPORTED_FORMAT, and the
 * codec hands such streams to FFmpeg when the app has it).
 *
 * Output order is the default layout for the channel count (miniaudio's, which
 * is Microsoft's): 1 C, 2 L R, 3 L R C, 4 L R C S, 5 L R C Ls Rs,
 * 6 L R C LFE Ls Rs, 7 L R C LFE S Ls Rs, 8 L R C LFE Lb Rb Ls Rs. Layouts
 * without a matching count use the next larger one with silent channels (2.1
 * plays as 5.1 with C and the surrounds silent). Dual mono (1+1) plays as stereo. */
#include "ac3.h"
#include "mdct.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

/* Tables computed once (ac3_init_tables). */
static uint8_t latab[256];        /* log-addition (Table 7.15) */
static uint8_t masktab[256];      /* bin -> band */
static float kbd[AC3_BLOCK * 2];  /* Kaiser-Bessel derived window, alpha 5 */
static float b1q[27][3], b2q[125][3], b4q[121][2], b3q[8], b5q[16];
static uint8_t frmexpstr[32][6];  /* E-AC-3 frame exponent strategies (Table E2.10) */
static float spxatten[32][3];
static fa_atomic32 tables_state;  /* 0 not done, 1 in progress, 2 done */

static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0;
    int k;
    for (k = 1; k < 64; k++) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

static void ac3_init_tables(void)
{
    double w[AC3_BLOCK + 1], total = 0.0, acc = 0.0;
    int i, b, k;
    if (fa_load32(&tables_state) == 2) return;
    if (!fa_cas32(&tables_state, 0, 1)) {
        while (fa_load32(&tables_state) != 2) fa_sleep_ms(1);
        return;
    }
    /* Log-addition: the increase (in 1/128ths of 6 dB) when two powers that differ
     * by 2*i units are added. */
    for (i = 0; i < 256; i++) {
        double u = 128.0 / 6.0;
        latab[i] = (uint8_t)floor(u * 10.0 * log10(1.0 + pow(10.0, -2.0 * i / u / 10.0)));
    }
    for (b = 0; b < 50; b++)
        for (k = bndtab[b]; k < bndtab[b + 1]; k++) masktab[k] = (uint8_t)b;
    for (k = 253; k < 256; k++) masktab[k] = 0;
    /* KBD window: cumulative Kaiser kernel of length N/2 + 1. */
    for (i = 0; i <= AC3_BLOCK; i++) {
        double r = 2.0 * i / AC3_BLOCK - 1.0;
        w[i] = bessel_i0(M_PI * 5.0 * sqrt(1.0 - r * r));
        total += w[i];
    }
    for (i = 0; i < AC3_BLOCK; i++) {
        acc += w[i];
        kbd[i] = (float)sqrt(acc / total);
        kbd[2 * AC3_BLOCK - 1 - i] = kbd[i];
    }
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
    fa_store32(&tables_state, 2);
}

/* ---- bit reader --------------------------------------------------------------- */

typedef struct bits {
    const uint8_t* p;
    size_t pos, end;   /* in bits */
    int overrun;
} bits;

static uint32_t gb(bits* b, int n)
{
    uint32_t v = 0;
    if (n == 0) return 0;
    if (b->pos + (size_t)n > b->end) {
        b->overrun = 1;
        b->pos = b->end;
        return 0;
    }
    while (n > 0) {
        size_t byte = b->pos >> 3;
        int off = (int)(b->pos & 7), take = 8 - off;
        if (take > n) take = n;
        v = (v << take) | (((uint32_t)b->p[byte] >> (8 - off - take)) & ((1u << take) - 1));
        b->pos += (size_t)take;
        n -= take;
    }
    return v;
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
    int b;
    for (i = 0; i < n; i++) {
        crc ^= (uint32_t)p[i] << 8;
        for (b = 0; b < 8; b++) crc = (crc & 0x8000) ? ((crc << 1) ^ 0x8005) & 0xFFFF : (crc << 1) & 0xFFFF;
    }
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
    int cplbegf, cplendf, ncplbnd;
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
    int strtmant[AC3_NCH], endmant[AC3_NCH];
    int ngrps[AC3_NCH];
    uint8_t exp[AC3_NCH][256];
    uint8_t bap[AC3_NCH][256];
    int16_t psd[AC3_NCH][256], bndpsd[AC3_NCH][51], mask[AC3_NCH][50];
    int sdecay, fdecay, sgain, dbknee, floorv;
    int snroffset[AC3_NCH], fgain[AC3_NCH];
    int cplfleak, cplsleak;
    int deltbae[AC3_NCH], deltnseg[AC3_NCH];
    uint8_t deltoffst[AC3_NCH][8], deltlen[AC3_NCH][8], deltba[AC3_NCH][8];
    float coef[AC3_NCH][256];
} frame;

struct fa_ac3 {
    fa_mdct t256, t128;
    sub_state sub[AC3_MAX_SUBSTREAMS];
    frame f;
    float tmp[512], u[256];
    uint32_t rng;
    /* Output of the substreams of one access unit, per speaker location. */
    float* loc[LOC_COUNT];
    int loc_used[LOC_COUNT];
    int last_used[LOC_COUNT];    /* the locations of the last good unit */
};

fa_ac3* fa_ac3_create(void)
{
    fa_ac3* d;
    int i;
    ac3_init_tables();
    d = (fa_ac3*)calloc(1, sizeof(*d));
    if (!d) return NULL;
    if (fa_imdct_init(&d->t256, 256) != FA_OK || fa_imdct_init(&d->t128, 128) != FA_OK) {
        fa_ac3_destroy(d);
        return NULL;
    }
    for (i = 0; i < LOC_COUNT; i++) {
        d->loc[i] = (float*)calloc(AC3_BLOCK * AC3_MAX_BLOCKS, sizeof(float));
        if (!d->loc[i]) { fa_ac3_destroy(d); return NULL; }
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
    free(d);
}

void fa_ac3_reset(fa_ac3* d)
{
    memset(d->sub, 0, sizeof(d->sub));
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

/* bap[] of channel ch for bins strtmant..endmant. 0 on success. */
static int bit_alloc(frame* f, int ch)
{
    int start = f->strtmant[ch], end = f->endmant[ch];
    int16_t* psd = f->psd[ch];
    int16_t* bndpsd = f->bndpsd[ch];
    int16_t* mask = f->mask[ch];
    uint8_t* bap = f->bap[ch];
    int excite[50];
    int bin, j, k, lastbin, bndstrt, bndend, begin, lowcomp = 0, fastleak = 0, slowleak = 0;
    int fgain = f->fgain[ch], snroffset = f->snroffset[ch], is_lfe = ch == AC3_LFE;
    int sdecay = f->sdecay, fdecay = f->fdecay, sgain = f->sgain;

    if (end <= start) return 0;
    if (snroffset == -960) {   /* csnroffst and fsnroffst both 0: no mantissas */
        memset(bap + start, 0, (size_t)(end - start));
        return 0;
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
            bap[bin] = baptab[addr < 0 ? 0 : addr > 63 ? 63 : addr];
        }
        j++;
    } while (end > lastbin);
    return 0;
}

#ifdef FA_AC3_STATS
/* Test builds: which coding tools the decoded streams used. */
long fa_ac3_stats[16];
#define STAT(i) (fa_ac3_stats[i]++)
#else
#define STAT(i) ((void)0)
#endif
enum { ST_BLOCKS, ST_CPL, ST_SHORT, ST_REMAT, ST_DBA, ST_SPX, ST_SPXATTEN, ST_PHSFLG, ST_DEP, ST_DYNRNG,
       ST_EXPREUSE, ST_D25, ST_D45, ST_SNRBLK, ST_FGAIN, ST_FRMEXP };

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
    int expstre = 1, transproce, spxattene, blk, ch, ncplblks = 0;
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
        /* AHT is possible only for channels that reuse block 0's exponents in every
         * other block; each such channel then says whether it uses it. */
        int any = 0;
        for (ch = 0; ch < AC3_NCH; ch++) {
            int possible = 1;
            if (ch < AC3_LFE && ch >= nfch) continue;
            if (ch == AC3_LFE && !h->lfeon) continue;
            if (ch == AC3_CPL && ncplblks != 6) continue;
            for (blk = 1; blk < 6; blk++)
                if (f->expstr[blk][ch] != 0 || (ch == AC3_CPL && f->cplstre[blk])) possible = 0;
            if (possible && gb1(b)) any = 1;
        }
        if (any) return 1;                         /* uses AHT: not decoded here */
        f->ahte = 0;
    }
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
        uint32_t u = gb(b, n);
        int32_t s = (int32_t)(u << (32 - n)) >> (32 - n);
        return ldexpf((float)s, 1 - n);
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
        f->coef[ch][bin] = ldexpf(m, -(int)f->exp[ch][bin]);
    }
}

static void decouple(fa_ac3* d)
{
    frame* f = &d->f;
    int ch, bnd, bin, k;
    for (ch = 0; ch < f->nfchans; ch++) {
        if (!f->chincpl[ch]) continue;
        bin = f->strtmant[AC3_CPL];
        for (bnd = 0; bnd < f->ncplbnd; bnd++) {
            float co = f->cplco[ch][bnd];
            if (f->h.acmod == 2 && ch == 1 && f->phsflg[bnd]) co = -co;
            for (k = 0; k < f->cplbndsz[bnd]; k++, bin++) {
                float c = f->coef[AC3_CPL][bin];
                if (f->bap[AC3_CPL][bin] == 0)   /* dither, scaled like the rest of the band */
                    c = f->dithflag[ch] ? ldexpf(rnd(d) * 0.707f, -(int)f->exp[AC3_CPL][bin]) : 0.0f;
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
            int endsub;
            if (acmod < 2) return -1;
            if (eac3 && gb1(b)) return 1;                       /* enhanced coupling */
            if (eac3 && acmod == 2) f->chincpl[0] = f->chincpl[1] = 1;
            else for (ch = 0; ch < nfch; ch++) f->chincpl[ch] = gb1(b);
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
        } else {
            for (ch = 0; ch < AC3_MAX_FBW; ch++) { f->chincpl[ch] = 0; f->firstcplcos[ch] = 1; }
            f->firstcplleak = 1;
            f->phsflginu = 0;
        }
    } else if (!eac3) {
        if (blk == 0) return -1;
        f->cplinu_blk[blk] = f->cplinu_blk[blk - 1];
    } else {
        f->cplinu = f->cplinu_blk[blk];
    }
    cplinu = f->cplinu = f->cplinu_blk[blk];

    /* Coupling coordinates and phase flags. */
    if (cplinu) {
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
            f->nrematbd = 4;
            if (cplinu && f->strtmant[AC3_CPL] <= 61) f->nrematbd -= 1 + (f->strtmant[AC3_CPL] == 37);
            else if (f->spxinu && f->spx_start <= 61) f->nrematbd--;
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
        /* Block-level offsets are sent in block 0 only, behind a flag (this is
         * what deployed decoders read; checked against FFmpeg). */
        if (f->snroffststr != 0 && blk == 0 && gb1(b)) {
            int csnr = (int)gb(b, 6), v;
            if (f->snroffststr == 1) {
                v = ((csnr - 15) * 16 + (int)gb(b, 4)) * 4;
                for (ch = 0; ch < AC3_NCH; ch++) if (ch != AC3_CPL || cplinu) f->snroffset[ch] = v;
            } else {
                if (cplinu) f->snroffset[AC3_CPL] = ((csnr - 15) * 16 + (int)gb(b, 4)) * 4;
                for (ch = 0; ch < nfch; ch++) f->snroffset[ch] = ((csnr - 15) * 16 + (int)gb(b, 4)) * 4;
                if (h->lfeon) f->snroffset[AC3_LFE] = ((csnr - 15) * 16 + (int)gb(b, 4)) * 4;
            }
        }
        if (f->frmfgaincode && gb1(b)) {
            if (cplinu) f->fgain[AC3_CPL] = fastgain[gb(b, 3)];
            for (ch = 0; ch < nfch; ch++) f->fgain[ch] = fastgain[gb(b, 3)];
            if (h->lfeon) f->fgain[AC3_LFE] = fastgain[gb(b, 3)];
        } else if (blk == 0) {
            /* The default; the coupling channel's only when coupling is on in
             * block 0 (it otherwise keeps its last value, as FFmpeg does). */
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

    STAT(ST_BLOCKS);
    if (cplinu) STAT(ST_CPL);
    for (ch = 0; ch < nfch; ch++) {
        if (f->blksw[ch]) STAT(ST_SHORT);
        if (f->expstr[blk][ch] == 0) STAT(ST_EXPREUSE);
        if (f->expstr[blk][ch] == 2) STAT(ST_D25);
        if (f->expstr[blk][ch] == 3) STAT(ST_D45);
    }
    for (bnd = 0; bnd < f->nrematbd; bnd++) if (f->rematflg[bnd]) { STAT(ST_REMAT); break; }
    for (ch = 0; ch < AC3_NCH; ch++) if (f->deltbae[ch] <= 1 && f->deltnseg[ch]) { STAT(ST_DBA); break; }
    if (f->spxinu) STAT(ST_SPX);
    if (f->spxinu) for (ch = 0; ch < nfch; ch++) if (f->chinspx[ch] && f->spxattencod[ch] >= 0) { STAT(ST_SPXATTEN); break; }
    if (cplinu && f->phsflginu) STAT(ST_PHSFLG);
    if (f->dynrng[0] != 1.0f) STAT(ST_DYNRNG);

    /* Bit allocation (redone every block: it is cheap next to the transform). */
    if (cplinu && bit_alloc(f, AC3_CPL) < 0) return -1;
    for (ch = 0; ch < nfch; ch++) if (bit_alloc(f, ch) < 0) return -1;
    if (h->lfeon && bit_alloc(f, AC3_LFE) < 0) return -1;

    /* Mantissas: each channel's, the coupling channel's right after the first
     * coupled channel, then the LFE. Grouped mantissas carry across channels. */
    memset(&g, 0, sizeof(g));
    {
        int got_cpl = 0;
        for (ch = 0; ch < nfch; ch++) {
            read_mantissas(d, b, &g, ch, f->dithflag[ch]);
            if (cplinu && f->chincpl[ch] && !got_cpl) {
                read_mantissas(d, b, &g, AC3_CPL, 0);
                got_cpl = 1;
            }
        }
        if (h->lfeon) read_mantissas(d, b, &g, AC3_LFE, 0);
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
    if (!short_blocks) {
        /* 512-point IMDCT. */
        fa_imdct(&d->t256, coef, z);
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
    gain *= -2.0f;   /* the inverse of the forward transform's -2/N scaling */
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

/* Decodes one syncframe into the location buffers at block offset `at`.
 * Returns 0, 1 (unsupported feature) or -1 (damaged). */
static int decode_frame(fa_ac3* d, const uint8_t* p, const fa_ac3_header* h, int state, int at, int drc,
                        int* wrote)
{
    frame* f = &d->f;
    sub_state* ss = &d->sub[state];
    bits b;
    int r, blk, ch, i;
    int8_t loc[6];
    float pcm[6][AC3_BLOCK];

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
            float gain;
            int prog = h->acmod == 0 && ch == 1;
            if (ch < AC3_LFE ? ch >= f->nfchans : !h->lfeon) continue;
            gain = 1.0f;
            if (drc == FA_AC3_DRC_NIGHT && f->compre[prog]) gain = f->compr[prog];
            else if (drc != FA_AC3_DRC_OFF) gain = f->dynrng[prog];
            synth(d, f->coef[ch], ch < AC3_LFE && f->blksw[ch], gain, ss->delay[ch], pcm[ch]);
        }
        for (ch = 0; ch < 6; ch++) {
            float* dst;
            if (loc[ch] < 0) continue;
            dst = d->loc[loc[ch]] + (size_t)(at + blk) * AC3_BLOCK;
            memcpy(dst, pcm[ch], sizeof(pcm[ch]));
            wrote[loc[ch]] = 1;
        }
    }
    ss->locs = 0;
    for (ch = 0; ch < 6; ch++)
        if (loc[ch] >= 0) { d->loc_used[loc[ch]] = 1; ss->locs |= 1u << loc[ch]; }
    return 0;
}

/* Output layouts (see the top of the file): which locations go to which channel,
 * with fold-downs for locations a layout lacks. */
enum { OUT_FL, OUT_FR, OUT_FC, OUT_LFE, OUT_BC, OUT_SL, OUT_SR, OUT_BL, OUT_BR, OUT_N };

static int mix_out(fa_ac3* d, int frames, float** pcm, size_t* cap, int* out_channels)
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
    const int* u = d->loc_used;
    int has_lr = u[LOC_L] || u[LOC_R], has_back = u[LOC_LRS] || u[LOC_RRS];
    int has_surr = u[LOC_LS] || u[LOC_RS] || u[LOC_LSD] || u[LOC_RSD] || u[LOC_LTS] || u[LOC_RTS] || u[LOC_TS];
    int has_bc = u[LOC_S] || u[LOC_CS], has_lfe = u[LOC_LFE] || u[LOC_LFE2];
    int nch, i, c, slot[OUT_N];
    float* dst;
    const float h = 0.70710678f;
    size_t need;

    if (has_back) nch = 8;
    else if (has_lfe) nch = has_bc ? 7 : 6;
    else if (has_surr) nch = has_bc ? 7 : 5;
    else if (has_bc) nch = 4;
    else if (u[LOC_C] && (has_lr || u[LOC_LC] || u[LOC_RC] || u[LOC_LW] || u[LOC_RW] || u[LOC_VHL] || u[LOC_VHR])) nch = 3;
    else if (u[LOC_C] && !u[LOC_VHC]) nch = 1;
    else nch = 2;
    for (i = 0; i < OUT_N; i++) slot[i] = -1;
    for (c = 0; c < nch; c++) slot[layouts[nch][c]] = c;

    need = (size_t)frames * nch;
    if (need > *cap) {
        float* p = (float*)realloc(*pcm, need * sizeof(float));
        if (!p) return -1;
        *pcm = p;
        *cap = need;
    }
    dst = *pcm;
    memset(dst, 0, need * sizeof(float));

#define ADD(out, l, g) do { if (u[l] && slot[out] >= 0) { const float* s_ = d->loc[l]; float* o_ = dst + slot[out]; \
        for (i = 0; i < frames; i++) o_[(size_t)i * nch] += s_[i] * (g); } } while (0)
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
    *out_channels = nch;
    return 0;
}

fa_result fa_ac3_decode(fa_ac3* d, const uint8_t* data, size_t size, int drc,
                        float** pcm, size_t* cap, int* frames, int* channels, uint32_t* rate)
{
    size_t pos = 0;
    int blocks = 0, err = 0, i;
    uint32_t r = 0;
    *frames = 0;
    memset(d->loc_used, 0, sizeof(d->loc_used));
    for (i = 0; i < AC3_MAX_SUBSTREAMS; i++) d->sub[i].seen = 0;
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
            STAT(ST_DEP);
        }
        memset(wrote, 0, sizeof(wrote));
        res = decode_frame(d, data + pos, &h, state, at, drc, wrote);
        if (res != 0) {
            int dependent = h.eac3 && h.strmtyp == 1, any = 0;
            sub_state* ss = &d->sub[state];
            err = res;
            /* Silence for the frame, and a clean restart of its overlap. A damaged
             * dependent frame silences only its own channels, keeping the layout:
             * the bed, and what the bed has of the channels it replaces, stay. */
            for (i = 0; i < LOC_COUNT; i++) {
                int stale = dependent && (ss->locs >> i & 1) && !d->loc_used[i];
                if (!dependent || wrote[i] || stale)
                    memset(d->loc[i] + (size_t)at * AC3_BLOCK, 0, (size_t)h.blocks * AC3_BLOCK * sizeof(float));
                if (stale || (dependent && wrote[i])) d->loc_used[i] = 1;
            }
            memset(ss->delay, 0, sizeof(ss->delay));
            if (dependent) { ss->seen = ss->used; pos += h.frame_size; continue; }
            for (i = 0; i < LOC_COUNT; i++) any |= d->loc_used[i];
            if (!any) {
                /* Keep the last good layout so the silence has the stream's shape. */
                for (i = 0; i < LOC_COUNT; i++) { d->loc_used[i] = d->last_used[i]; any |= d->last_used[i]; }
                if (!any) d->loc_used[LOC_L] = d->loc_used[LOC_R] = 1;
            }
        }
        pos += h.frame_size;
    }
    if (blocks == 0) return FA_ERROR_GENERIC;
    *frames = blocks * AC3_BLOCK;
    if (mix_out(d, *frames, pcm, cap, channels) < 0) return FA_ERROR_OUT_OF_MEMORY;
    *rate = r;
    /* A dependent substream missing from this unit starts clean when it returns. */
    for (i = 1; i < AC3_MAX_SUBSTREAMS; i++) if (!d->sub[i].seen) d->sub[i].used = 0;
    if (err == 0) memcpy(d->last_used, d->loc_used, sizeof(d->loc_used));
    if (err > 0) return FA_ERROR_UNSUPPORTED_FORMAT;
    if (err < 0) return FA_ERROR_GENERIC;
    return FA_OK;
}
