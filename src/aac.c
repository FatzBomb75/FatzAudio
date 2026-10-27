/* AAC decoder: the AAC-LC core of ISO/IEC 14496-3 (subpart 4: the general audio
 * coder's syntax, noiseless decoding, quantization, scale factors, M/S and
 * intensity stereo, perceptual noise substitution, temporal noise shaping, the
 * filterbank and block switching), ADTS framing from 13818-7, and the parts of
 * the AudioSpecificConfig the decoder needs. SBR (HE-AAC) is in aac_sbr.c and
 * parametric stereo (HE-AAC v2) in aac_ps.c. The AAC Main (backward-adaptive
 * prediction) and AAC LTP (long-term prediction) profiles decode too; SSR, error
 * resilient object types and 960-sample frames are not supported. Coupling
 * channel elements are read and left out of the mix (no common encoder writes them).
 *
 * Output order is the default layout for the channel count (as in ac3.c):
 * 1 C, 2 L R, 3 L R C, 4 L R C S, 5 L R C Ls Rs, 6 L R C LFE Ls Rs,
 * 7 L R C LFE S Ls Rs, 8 L R C LFE Lb Rb Ls Rs. The channel configuration or the
 * program config element names each element's place: the first front pair is
 * L R, a front single element C, further front pairs and side pairs the side
 * surrounds, back pairs the back surrounds (or the surrounds of a 5.0 / 5.1
 * layout), a back single element S, the LFE element LFE. Layouts without a
 * matching count use the next larger one with silent channels; elements beyond
 * 7.1 are mixed into their nearest channel. */
#include "aac.h"
#include "mdct.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef FA_AAC_TRACE
#include <stdio.h>
#endif

/* Sampling frequency index -> Hz (shared with the demuxers, demux.h). */
const uint32_t fa_aac_rates[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                    22050, 16000, 12000, 11025, 8000, 7350 };

enum { ID_SCE = 0, ID_CPE, ID_CCE, ID_LFE, ID_DSE, ID_PCE, ID_FIL, ID_END };
enum { AOT_MAIN = 1, AOT_LC = 2, AOT_LTP = 4 };
enum { ONLY_LONG = 0, LONG_START, EIGHT_SHORT, LONG_STOP };
enum { ZERO_HCB = 0, ESC_HCB = 11, NOISE_HCB = 13, INTENSITY_HCB2 = 14, INTENSITY_HCB = 15 };
enum { EXT_FIL = 0, EXT_FILL_DATA = 1, EXT_DATA_ELEMENT = 2, EXT_DYNAMIC_RANGE = 11, EXT_SBR_DATA = 13,
       EXT_SBR_DATA_CRC = 14 };

#define MAX_ELEM 16
#define MAX_GROUPS 8
#define MAX_SFB 64
#define TNS_MAX_ORDER 20

/* Output channels of the layouts by count (as in ac3.c). */
enum { SP_L = 0, SP_R, SP_C, SP_LFE, SP_BL, SP_BR, SP_SL, SP_SR, SP_BC, SP_COUNT };

/* ---- stream configuration ------------------------------------------------------------- */

typedef struct aac_pce_elem {
    int is_cpe, tag;
} aac_pce_elem;

typedef struct aac_pce {
    int valid;
    int n_front, n_side, n_back, n_lfe;
    aac_pce_elem front[15], side[15], back[15];
    int lfe[4];
} aac_pce;

typedef struct aac_ics {
    int window_sequence, window_shape;
    int max_sfb, num_swb, num_windows, num_groups;
    int group_len[MAX_GROUPS];
    const uint16_t* swb;           /* num_swb + 1 offsets */
    uint8_t cb[MAX_GROUPS][MAX_SFB];
    int sf[MAX_GROUPS][MAX_SFB];   /* scale factor, intensity position or noise energy */
    /* AAC Main prediction */
    int pred_present, pred_reset_group;
    uint8_t pred_used[MAX_SFB];
    /* AAC LTP */
    int ltp_present, ltp_lag;
    float ltp_coef;
    uint8_t ltp_used[MAX_SFB];
} aac_ics;

typedef struct aac_tns {
    int present;
    int n_filt[8];
    int length[8][4], order[8][4], direction[8][4];
    float lpc[8][4][TNS_MAX_ORDER + 1];
} aac_tns;

/* One spectral line's backward-adaptive predictor (AAC Main). */
typedef struct aac_pred {
    float r0, r1, cor0, cor1, var0, var1;
} aac_pred;

#define MAX_PRED 672               /* lines the predictors cover at most */
#define MAX_LTP_SFB 40

typedef struct aac_chan {
    aac_ics ics;
    aac_tns tns;
    float spec[1024];
    float overlap[1024];
    int prev_shape;
    float out[2048];               /* the decoded block: 1024 samples, 2048 after SBR */
    aac_pred* pred;                /* AAC Main: MAX_PRED predictors, allocated on first use */
    float* ltp;                    /* AAC LTP: the last two blocks' output and the next one's
                                      overlap (3072 samples), allocated on first use */
} aac_chan;

typedef struct aac_elem {
    int type, tag;                 /* ID_SCE, ID_CPE or ID_LFE */
    int present;                   /* decoded in the current block */
    int nch;
    int ms_mask_present;
    uint8_t ms_used[MAX_GROUPS][MAX_SFB];
    aac_chan* ch[2];
    fa_aac_sbr* sbr;
    int stereo_out;                /* produced two channels (a CPE, or a mono element with PS) */
    int order;                     /* position among the block's elements of its type */
} aac_elem;

/* Which decoded element goes to which speakers: plan_mix sorts them, render_mix
 * mixes them into the output. */
typedef struct mix_layout {
    aac_elem* fronts_c[MAX_ELEM];
    aac_elem* fronts_pair[MAX_ELEM];
    aac_elem* pairs[2 * MAX_ELEM];
    aac_elem* backs_c[MAX_ELEM];
    aac_elem* lfes[MAX_ELEM];
    int nfc, nfp, np, nbc, nlfe, nside, nch;
} mix_layout;

struct fa_aac {
    /* configuration */
    int configured;                /* from an AudioSpecificConfig or an ADTS header */
    int from_asc;                  /* fa_aac_set_config was called: packets are raw access units */
    int object_type;               /* of the core */
    int rate_index;
    uint32_t rate;                 /* core rate */
    int channel_config;
    int sbr_signalled;             /* 1 present, 0 absent, -1 not said */
    int ps_signalled;
    aac_pce pce;
    /* state */
    int sbr_on;
    aac_elem* elem[MAX_ELEM];
    int nelem;
    uint32_t seed;
    fa_mdct mdct_long, mdct_short;
    fa_aac_qmf* qmf;               /* SBR transform tables, made with the first SBR element */
    float buf[2048];               /* windowing */
    int q[1024];                   /* the channel being read: quantized values */
    float* ltp_work;               /* AAC LTP's prediction: 2048 time samples, 1024 spectral
                                      values; allocated on first use */
    uint8_t* pkt;                  /* a padded copy of the packet */
    size_t pkt_cap;
    int out_channels;              /* of the last block decoded, 0 before */
    uint32_t out_rate;
};

static const uint16_t* swb_long(int ri, int* n)
{
    static const struct { const uint16_t* t; int n; } tab[13] = {
        { fa_aac_swb_long_96, 41 }, { fa_aac_swb_long_96, 41 }, { fa_aac_swb_long_64, 47 },
        { fa_aac_swb_long_48, 49 }, { fa_aac_swb_long_48, 49 }, { fa_aac_swb_long_32, 51 },
        { fa_aac_swb_long_24, 47 }, { fa_aac_swb_long_24, 47 }, { fa_aac_swb_long_16, 43 },
        { fa_aac_swb_long_16, 43 }, { fa_aac_swb_long_16, 43 }, { fa_aac_swb_long_8, 40 },
        { fa_aac_swb_long_8, 40 } };
    *n = tab[ri].n;
    return tab[ri].t;
}

static const uint16_t* swb_short(int ri, int* n)
{
    static const struct { const uint16_t* t; int n; } tab[13] = {
        { fa_aac_swb_short_96, 12 }, { fa_aac_swb_short_96, 12 }, { fa_aac_swb_short_64, 12 },
        { fa_aac_swb_short_48, 14 }, { fa_aac_swb_short_48, 14 }, { fa_aac_swb_short_48, 14 },
        { fa_aac_swb_short_24, 15 }, { fa_aac_swb_short_24, 15 }, { fa_aac_swb_short_16, 15 },
        { fa_aac_swb_short_16, 15 }, { fa_aac_swb_short_16, 15 }, { fa_aac_swb_short_8, 15 },
        { fa_aac_swb_short_8, 15 } };
    *n = tab[ri].n;
    return tab[ri].t;
}

/* Highest band TNS may filter, by sampling frequency index (long, short windows). */
static const uint8_t tns_max_bands_long[13] = { 31, 31, 34, 40, 42, 51, 46, 46, 42, 42, 42, 39, 39 };
static const uint8_t tns_max_bands_short[13] = { 9, 9, 10, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14 };

/* ---- windows ------------------------------------------------------------------------------ */

static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0, k;
    for (k = 1.0; k < 50.0; k += 1.0) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

/* The rising half (n samples) of the Kaiser-Bessel derived window of length 2n. */
static void kbd_window(float* w, int n, double alpha)
{
    double k[1024 + 1];                /* n is 1024 or 128 */
    double sum = 0.0, acc = 0.0;
    int i;
    for (i = 0; i <= n; i++) {
        double r = (double)(i - n / 2) / (n / 2);
        k[i] = bessel_i0(FA_PI * alpha * sqrt(1.0 - r * r));
        sum += k[i];
    }
    for (i = 0; i < n; i++) {
        acc += k[i];
        w[i] = (float)sqrt(acc / sum);
    }
}

/* The windows and |q|^(4/3) are the same for every decoder: the first one to open
 * builds them. */
static struct {
    fa_atomic32 state;             /* 0 not built, 1 being built, 2 built */
    float sine_long[1024], sine_short[128], kbd_long[1024], kbd_short[128];
    float iq[256];                 /* |q|^(4/3) */
} shared;

static void build_shared(void)
{
    while (fa_load32(&shared.state) != 2) {
        if (fa_cas32(&shared.state, 0, 1)) {
            int i;
            for (i = 0; i < 1024; i++) shared.sine_long[i] = (float)sin(FA_PI / 2048.0 * (i + 0.5));
            for (i = 0; i < 128; i++) shared.sine_short[i] = (float)sin(FA_PI / 256.0 * (i + 0.5));
            kbd_window(shared.kbd_long, 1024, 4.0);
            kbd_window(shared.kbd_short, 128, 6.0);
            for (i = 0; i < 256; i++) shared.iq[i] = (float)pow((double)i, 4.0 / 3.0);
            fa_store32(&shared.state, 2);
        } else {
            fa_sleep_ms(1);
        }
    }
}

/* ---- configuration ---------------------------------------------------------------------- */

static int read_aot(fa_aac_bits* b)
{
    int aot = (int)fa_aac_get(b, 5);
    if (aot == 31) aot = 32 + (int)fa_aac_get(b, 6);
    return aot;
}

static uint32_t read_rate(fa_aac_bits* b, int* index)
{
    int i = (int)fa_aac_get(b, 4);
    *index = i;
    if (i == 15) return fa_aac_get(b, 24);
    return i < 13 ? fa_aac_rates[i] : 0;
}

static void byte_align(fa_aac_bits* b, size_t start)
{
    size_t r = (b->pos - start) & 7;
    if (r) fa_aac_skip(b, 8 - r);
}

/* program_config_element(), after the element id. `start`: the bit alignment refers to it. */
static int read_pce(fa_aac_bits* b, aac_pce* p, size_t start)
{
    int i, n_assoc, n_cc, comment;
    memset(p, 0, sizeof(*p));
    fa_aac_skip(b, 4 + 2 + 4);            /* element_instance_tag, object_type, sampling_frequency_index */
    p->n_front = (int)fa_aac_get(b, 4);
    p->n_side = (int)fa_aac_get(b, 4);
    p->n_back = (int)fa_aac_get(b, 4);
    p->n_lfe = (int)fa_aac_get(b, 2);
    n_assoc = (int)fa_aac_get(b, 3);
    n_cc = (int)fa_aac_get(b, 4);
    if (fa_aac_get(b, 1)) fa_aac_skip(b, 4);          /* mono_mixdown */
    if (fa_aac_get(b, 1)) fa_aac_skip(b, 4);          /* stereo_mixdown */
    if (fa_aac_get(b, 1)) fa_aac_skip(b, 3);          /* matrix_mixdown_idx, pseudo_surround */
    for (i = 0; i < p->n_front; i++) { p->front[i].is_cpe = (int)fa_aac_get(b, 1); p->front[i].tag = (int)fa_aac_get(b, 4); }
    for (i = 0; i < p->n_side; i++) { p->side[i].is_cpe = (int)fa_aac_get(b, 1); p->side[i].tag = (int)fa_aac_get(b, 4); }
    for (i = 0; i < p->n_back; i++) { p->back[i].is_cpe = (int)fa_aac_get(b, 1); p->back[i].tag = (int)fa_aac_get(b, 4); }
    for (i = 0; i < p->n_lfe; i++) p->lfe[i] = (int)fa_aac_get(b, 4);
    fa_aac_skip(b, (size_t)n_assoc * 4);
    fa_aac_skip(b, (size_t)n_cc * 5);
    byte_align(b, start);
    comment = (int)fa_aac_get(b, 8);
    fa_aac_skip(b, (size_t)comment * 8);
    if (fa_aac_left(b) < 0) return 0;
    p->valid = p->n_front + p->n_side + p->n_back + p->n_lfe > 0;
    return p->valid;
}

static int core_supported(int aot)
{
    return aot == AOT_MAIN || aot == AOT_LC || aot == AOT_LTP;
}

static fa_result read_config(fa_aac* d, fa_aac_bits* b)
{
    int aot, ri, ext_ri = -1, sbr = -1, ps = -1;
    uint32_t rate;
    int config;
    aot = read_aot(b);
    rate = read_rate(b, &ri);
    config = (int)fa_aac_get(b, 4);
    if (aot == 5 || aot == 29) {
        sbr = 1;
        ps = aot == 29;
        read_rate(b, &ext_ri);
        aot = read_aot(b);
        if (aot == 22) fa_aac_skip(b, 4);
    }
    if (!core_supported(aot)) return FA_ERROR_UNSUPPORTED_FORMAT;
    /* GASpecificConfig */
    if (fa_aac_get(b, 1)) return FA_ERROR_UNSUPPORTED_FORMAT;   /* 960-sample frames */
    if (fa_aac_get(b, 1)) fa_aac_skip(b, 14);                  /* core coder delay */
    {
        int ext = (int)fa_aac_get(b, 1);
        memset(&d->pce, 0, sizeof(d->pce));
        if (config == 0 && !read_pce(b, &d->pce, 0)) return FA_ERROR_UNSUPPORTED_FORMAT;
        if (ext) fa_aac_skip(b, 1);                             /* extensionFlag3 */
    }
    /* Backward compatible SBR / PS signalling after the core's config. */
    if (sbr < 0 && fa_aac_left(b) >= 16 && fa_aac_peek(b, 11) == 0x2B7) {
        int ext_aot;
        fa_aac_skip(b, 11);
        ext_aot = read_aot(b);
        if (ext_aot == 5) {
            sbr = (int)fa_aac_get(b, 1);
            if (sbr) {
                read_rate(b, &ext_ri);
                if (fa_aac_left(b) >= 12 && fa_aac_peek(b, 11) == 0x548) {
                    fa_aac_skip(b, 11);
                    ps = (int)fa_aac_get(b, 1);
                }
            }
        }
    }
    if (ri >= 13) {
        /* An explicit rate: the nearest table's bands. */
        static const uint32_t bounds[12] = { 92017, 75132, 55426, 46009, 37566, 27713, 23004, 18783, 13856, 11502, 9391, 0 };
        for (ri = 0; ri < 12 && rate < bounds[ri]; ri++) {}
    }
    if (rate == 0 || ri > 12 || config > 12 || (config > 7 && config < 11)) return FA_ERROR_UNSUPPORTED_FORMAT;
    (void)ext_ri;
    d->configured = 1;
    d->from_asc = 1;
    d->object_type = aot;
    d->rate_index = ri;
    d->rate = rate;
    d->channel_config = config;
    d->sbr_signalled = sbr;
    d->ps_signalled = ps;
    d->sbr_on = sbr == 1;
    return FA_OK;
}

fa_result fa_aac_set_config(fa_aac* d, const uint8_t* asc, size_t n)
{
    uint8_t* tmp;
    fa_aac_bits b;
    fa_result r;
    if (!asc || n < 2) return FA_ERROR_UNSUPPORTED_FORMAT;
    tmp = (uint8_t*)calloc(1, n + 16);   /* the bit reader reads up to 12 bytes past the end */
    if (!tmp) return FA_ERROR_OUT_OF_MEMORY;
    memcpy(tmp, asc, n);
    b.p = tmp;
    b.pos = 0;
    b.end = n * 8;
    r = read_config(d, &b);
    free(tmp);
    return r;
}

/* ---- elements ------------------------------------------------------------------------------ */

static aac_elem* get_elem(fa_aac* d, int type, int tag)
{
    int i, nch = type == ID_CPE ? 2 : 1;
    aac_elem* e;
    for (i = 0; i < d->nelem; i++)
        if (d->elem[i]->type == type && d->elem[i]->tag == tag) return d->elem[i];
    if (d->nelem >= MAX_ELEM) return NULL;
    e = (aac_elem*)calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->type = type;
    e->tag = tag;
    e->nch = nch;
    for (i = 0; i < nch; i++) {
        e->ch[i] = (aac_chan*)calloc(1, sizeof(aac_chan));
        if (!e->ch[i]) {
            if (i) free(e->ch[0]);
            free(e);
            return NULL;
        }
    }
    d->elem[d->nelem++] = e;
    return e;
}

static void free_chan(aac_chan* ch)
{
    if (!ch) return;
    free(ch->pred);
    free(ch->ltp);
    free(ch);
}

static void free_elem(aac_elem* e)
{
    if (!e) return;
    free_chan(e->ch[0]);
    free_chan(e->ch[1]);
    fa_aac_sbr_destroy(e->sbr);
    free(e);
}

/* ---- individual channel stream ------------------------------------------------------------- */

/* Lines predicted in AAC Main, by sampling frequency index (scale factor bands). */
static const uint8_t pred_sfb_max[13] = { 33, 33, 38, 40, 40, 40, 41, 41, 37, 37, 37, 34, 34 };

static const float ltp_coef[8] = { 0.570829f, 0.696616f, 0.813004f, 0.911304f, 0.984900f, 1.067894f, 1.194601f,
                                   1.369533f };

static void read_ltp(fa_aac_bits* b, aac_ics* ics)
{
    int n = ics->max_sfb < MAX_LTP_SFB ? ics->max_sfb : MAX_LTP_SFB, k;
    ics->ltp_lag = (int)fa_aac_get(b, 11);
    ics->ltp_coef = ltp_coef[fa_aac_get(b, 3)];
    for (k = 0; k < n; k++) ics->ltp_used[k] = (uint8_t)fa_aac_get(b, 1);
    for (; k < MAX_SFB; k++) ics->ltp_used[k] = 0;
}

static int read_ics_info(fa_aac* d, fa_aac_bits* b, aac_ics* ics)
{
    int g;
    if (fa_aac_get(b, 1)) return 0;           /* ics_reserved_bit */
    ics->window_sequence = (int)fa_aac_get(b, 2);
    ics->window_shape = (int)fa_aac_get(b, 1);
    if (ics->window_sequence == EIGHT_SHORT) {
        uint32_t grouping;
        ics->max_sfb = (int)fa_aac_get(b, 4);
        grouping = fa_aac_get(b, 7);
        ics->num_windows = 8;
        ics->num_groups = 1;
        ics->group_len[0] = 1;
        for (g = 6; g >= 0; g--) {
            if (grouping & (1u << g)) ics->group_len[ics->num_groups - 1]++;
            else ics->group_len[ics->num_groups++] = 1;
        }
        ics->swb = swb_short(d->rate_index, &ics->num_swb);
    } else {
        ics->max_sfb = (int)fa_aac_get(b, 6);
        ics->num_windows = 1;
        ics->num_groups = 1;
        ics->group_len[0] = 1;
        ics->swb = swb_long(d->rate_index, &ics->num_swb);
        if (ics->max_sfb > ics->num_swb) return 0;
#ifdef FA_AAC_TRACE
        fprintf(stderr, "PRED %zu %d\n", b->pos, ics->max_sfb);   /* for tests/data/aac/ltp_rewrite.py */
#endif
        ics->pred_present = (int)fa_aac_get(b, 1);
        ics->pred_reset_group = 0;
        ics->ltp_present = 0;
        if (ics->pred_present) {
            if (d->object_type == AOT_MAIN) {
                int n = ics->max_sfb < pred_sfb_max[d->rate_index] ? ics->max_sfb : pred_sfb_max[d->rate_index], k;
                if (fa_aac_get(b, 1)) {
                    ics->pred_reset_group = (int)fa_aac_get(b, 5);
                    if (ics->pred_reset_group == 0 || ics->pred_reset_group > 30) return 0;
                }
                for (k = 0; k < n; k++) ics->pred_used[k] = (uint8_t)fa_aac_get(b, 1);
                for (; k < MAX_SFB; k++) ics->pred_used[k] = 0;
            } else if (d->object_type == AOT_LTP) {
                ics->ltp_present = (int)fa_aac_get(b, 1);
                if (ics->ltp_present) read_ltp(b, ics);
            } else {
                return 0;                         /* no prediction in AAC-LC */
            }
        }
        return 1;
    }
    ics->pred_present = 0;
    ics->ltp_present = 0;
    if (ics->max_sfb > ics->num_swb) return 0;
    return 1;
}

static int read_section_data(fa_aac_bits* b, aac_ics* ics)
{
    int g, bits = ics->window_sequence == EIGHT_SHORT ? 3 : 5;
    uint32_t esc = (1u << bits) - 1;
    for (g = 0; g < ics->num_groups; g++) {
        int k = 0;
        while (k < ics->max_sfb) {
            int cb = (int)fa_aac_get(b, 4), len = 0, i;
            uint32_t inc;
            if (cb == 12) return 0;
            do {
                inc = fa_aac_get(b, bits);
                len += (int)inc;
                if (fa_aac_left(b) < 0) return 0;
            } while (inc == esc);
            if (k + len > ics->max_sfb) return 0;
            for (i = 0; i < len; i++) ics->cb[g][k + i] = (uint8_t)cb;
            k += len;
        }
        for (; k < MAX_SFB; k++) ics->cb[g][k] = ZERO_HCB;
    }
    return 1;
}

static int read_scale_factors(fa_aac_bits* b, aac_ics* ics, int global_gain)
{
    int g, sfb, sf = global_gain, is_pos = 0, noise = global_gain - 90, noise_first = 1;
    for (g = 0; g < ics->num_groups; g++) {
        for (sfb = 0; sfb < ics->max_sfb; sfb++) {
            int cb = ics->cb[g][sfb], v;
            if (cb == ZERO_HCB) {
                ics->sf[g][sfb] = 0;
                continue;
            }
            if (cb == NOISE_HCB && noise_first) {
                noise_first = 0;
                noise += (int)fa_aac_get(b, 9) - 256;
                ics->sf[g][sfb] = noise;
                continue;
            }
            v = fa_aac_vlc(b, fa_aac_hcb_sf, FA_AAC_VLC_ROOT);
            if (v < 0) return 0;
            v -= 60;
            if (cb == INTENSITY_HCB || cb == INTENSITY_HCB2) {
                is_pos += v;
                ics->sf[g][sfb] = is_pos;
            } else if (cb == NOISE_HCB) {
                noise += v;
                ics->sf[g][sfb] = noise;
            } else {
                sf += v;
                if (sf < 0 || sf > 255) return 0;
                ics->sf[g][sfb] = sf;
            }
        }
    }
    return 1;
}

static int read_tns(fa_aac_bits* b, const aac_ics* ics, aac_tns* t, int max_order)
{
    int w, f, i, shortw = ics->window_sequence == EIGHT_SHORT;
    for (w = 0; w < ics->num_windows; w++) {
        int coef_res = 0;
        t->n_filt[w] = (int)fa_aac_get(b, shortw ? 1 : 2);
        if (t->n_filt[w]) coef_res = (int)fa_aac_get(b, 1);
        for (f = 0; f < t->n_filt[w]; f++) {
            int order, bits, compress;
            float tmp[TNS_MAX_ORDER], lpc[TNS_MAX_ORDER + 1], next[TNS_MAX_ORDER + 1];
            t->length[w][f] = (int)fa_aac_get(b, shortw ? 4 : 6);
            order = (int)fa_aac_get(b, shortw ? 3 : 5);
            t->order[w][f] = order > max_order ? max_order : order;
            if (!order) continue;
            t->direction[w][f] = (int)fa_aac_get(b, 1);
            compress = (int)fa_aac_get(b, 1);
            bits = coef_res + 3 - compress;
            for (i = 0; i < order; i++) {
                int c = (int)fa_aac_get(b, bits);
                double iqfac;
                if (c & (1 << (bits - 1))) c -= 1 << bits;      /* sign extend */
                iqfac = c >= 0 ? ((1 << (coef_res + 2)) - 0.5) / (FA_PI / 2.0)
                               : ((1 << (coef_res + 2)) + 0.5) / (FA_PI / 2.0);
                if (i < TNS_MAX_ORDER) tmp[i] = (float)sin(c / iqfac);
            }
            /* Reflection coefficients to the direct form. */
            order = t->order[w][f];
            lpc[0] = 1.0f;
            for (i = 1; i <= order; i++) {
                int j;
                for (j = 1; j < i; j++) next[j] = lpc[j] + tmp[i - 1] * lpc[i - j];
                for (j = 1; j < i; j++) lpc[j] = next[j];
                lpc[i] = tmp[i - 1];
            }
            memcpy(t->lpc[w][f], lpc, sizeof(float) * (size_t)(order + 1));
        }
    }
    return fa_aac_left(b) >= 0;
}

static float inv_quant(int q)
{
    int a = q < 0 ? -q : q;
    float v = a < 256 ? shared.iq[a] : (float)a * cbrtf((float)a);
    return q < 0 ? -v : v;
}

/* 2^(0.25 (sf - 100)) times the filterbank's scaling (2/N, and 1/32768 for
 * floats): spectra are kept ready for the inverse transform. */
static float sf_gain(int sf, int shortw)
{
    static const float frac[4] = { 1.0f, 1.18920711500272f, 1.41421356237310f, 1.68179283050743f };
    return ldexpf(frac[sf & 3], (sf >> 2) - 25 - (shortw ? 22 : 25));
}

static void dequantize(aac_chan* ch, const int* q)
{
    const aac_ics* ics = &ch->ics;
    int g, sfb, w0 = 0, shortw = ics->window_sequence == EIGHT_SHORT;
    memset(ch->spec, 0, sizeof(ch->spec));
    for (g = 0; g < ics->num_groups; g++) {
        for (sfb = 0; sfb < ics->max_sfb; sfb++) {
            int cb = ics->cb[g][sfb], w, k;
            float gain;
            if (cb == ZERO_HCB || cb > ESC_HCB) continue;
            gain = sf_gain(ics->sf[g][sfb], shortw);
            for (w = 0; w < ics->group_len[g]; w++) {
                int base = (w0 + w) * 128;
                for (k = ics->swb[sfb]; k < ics->swb[sfb + 1]; k++)
                    ch->spec[base + k] = inv_quant(q[base + k]) * gain;
            }
        }
        w0 += ics->group_len[g];
    }
}

/* Spectral data of one channel into q. */
static int read_spectral(fa_aac_bits* b, const aac_chan* ch, int* qv)
{
    static const uint16_t* const books[11] = { fa_aac_hcb1, fa_aac_hcb2, fa_aac_hcb3, fa_aac_hcb4, fa_aac_hcb5,
                                               fa_aac_hcb6, fa_aac_hcb7, fa_aac_hcb8, fa_aac_hcb9, fa_aac_hcb10,
                                               fa_aac_hcb11 };
    static const int mods[11] = { 3, 3, 3, 3, 9, 9, 8, 8, 13, 13, 17 };
    const aac_ics* ics = &ch->ics;
    int g, sfb, w0 = 0;
    memset(qv, 0, sizeof(int) * 1024);
    for (g = 0; g < ics->num_groups; g++) {
        for (sfb = 0; sfb < ics->max_sfb; sfb++) {
            int cb = ics->cb[g][sfb], w, start = ics->swb[sfb], end = ics->swb[sfb + 1];
            const uint16_t* book;
            int mod;
            if (cb == ZERO_HCB || cb > ESC_HCB) continue;
            book = books[cb - 1];
            mod = mods[cb - 1];
            for (w = 0; w < ics->group_len[g]; w++) {
                int* q = qv + (w0 + w) * 128;
                int k;
                if (cb <= 4) {
                    for (k = start; k < end; k += 4) {
                        int idx = fa_aac_vlc(b, book, FA_AAC_VLC_ROOT), v[4], i;
                        if (idx < 0) return 0;
                        v[0] = idx / 27;
                        v[1] = (idx / 9) % 3;
                        v[2] = (idx / 3) % 3;
                        v[3] = idx % 3;
                        if (cb <= 2) {
                            for (i = 0; i < 4; i++) q[k + i] = v[i] - 1;
                        } else {
                            for (i = 0; i < 4; i++) {
                                q[k + i] = v[i];
                                if (v[i] && fa_aac_get(b, 1)) q[k + i] = -v[i];
                            }
                        }
                    }
                } else {
                    for (k = start; k < end; k += 2) {
                        int idx = fa_aac_vlc(b, book, FA_AAC_VLC_ROOT), y, z;
                        if (idx < 0) return 0;
                        y = idx / mod;
                        z = idx % mod;
                        if (cb <= 6) {
                            y -= 4;
                            z -= 4;
                        } else {
                            int sy = y && fa_aac_get(b, 1), sz;
                            sz = z && fa_aac_get(b, 1);
                            if (cb == ESC_HCB) {
                                int e;
                                for (e = 0; e < 2; e++) {
                                    int* v = e ? &z : &y;
                                    if (*v == 16) {
                                        int n = 4;
                                        while (fa_aac_get(b, 1)) {
                                            if (++n > 12) return 0;
                                        }
                                        *v = (1 << n) + (int)fa_aac_get(b, n);
                                    }
                                }
                            }
                            if (sy) y = -y;
                            if (sz) z = -z;
                        }
                        q[k] = y;
                        q[k + 1] = z;
                    }
                }
            }
        }
        w0 += ics->group_len[g];
    }
    return fa_aac_left(b) >= 0;
}

/* individual_channel_stream(): `common`: the ics_info came with the CPE. */
static int read_ics(fa_aac* d, fa_aac_bits* b, aac_chan* ch, int common, int lfe)
{
    aac_ics* ics = &ch->ics;
    int global_gain = (int)fa_aac_get(b, 8), r;
    if (!common) {
        r = read_ics_info(d, b, ics);
        if (r <= 0) return r;
    }
    if (lfe && ics->window_sequence == EIGHT_SHORT) return 0;
    if (!read_section_data(b, ics)) return 0;
    if (!read_scale_factors(b, ics, global_gain)) return 0;
    {
        int pulse = (int)fa_aac_get(b, 1);
        int pulse_n = 0, pulse_start = 0, pulse_off[4], pulse_amp[4], i;
        if (pulse) {
            if (ics->window_sequence == EIGHT_SHORT) return 0;
            pulse_n = (int)fa_aac_get(b, 2) + 1;
            pulse_start = (int)fa_aac_get(b, 6);
            for (i = 0; i < pulse_n; i++) {
                pulse_off[i] = (int)fa_aac_get(b, 5);
                pulse_amp[i] = (int)fa_aac_get(b, 4);
            }
            if (pulse_start >= ics->num_swb) return 0;
        }
        ch->tns.present = (int)fa_aac_get(b, 1);
        if (ch->tns.present && !read_tns(b, ics, &ch->tns, d->object_type == AOT_MAIN ? 20 : 12)) return 0;
        if (fa_aac_get(b, 1)) return -1;          /* gain_control_data_present: SSR */
        if (!read_spectral(b, ch, d->q)) return 0;
        if (pulse) {
            int k = ics->swb[pulse_start];
            for (i = 0; i < pulse_n; i++) {
                k += pulse_off[i];
                if (k >= 1024) return 0;
                if (d->q[k] >= 0) d->q[k] += pulse_amp[i];
                else d->q[k] -= pulse_amp[i];
            }
        }
    }
    dequantize(ch, d->q);
    return 1;
}

/* ---- spectral reconstruction ------------------------------------------------------------- */

#define PNS_SEED 0x1F2E3D4Cu   /* FFmpeg's, so PNS bands come out as its decoder's */

static float noise_value(fa_aac* d)
{
    d->seed = d->seed * 1664525u + 1013904223u;
    return (float)(int32_t)d->seed;
}

/* Perceptual noise substitution. A right channel's band shares the left channel's
 * noise where both are noise and M/S is on. */
static void apply_pns(fa_aac* d, aac_elem* e, int c)
{
    aac_chan* ch = e->ch[c];
    const aac_ics* ics = &ch->ics;
    int g, sfb, w0 = 0, shortw = ics->window_sequence == EIGHT_SHORT;
    for (g = 0; g < ics->num_groups; g++) {
        for (sfb = 0; sfb < ics->max_sfb; sfb++) {
            int w, k, start = ics->swb[sfb], end = ics->swb[sfb + 1];
            int shared;
            if (ics->cb[g][sfb] != NOISE_HCB) continue;
            shared = c == 1 && e->ms_mask_present && e->ms_used[g][sfb] && e->ch[0]->ics.cb[g][sfb] == NOISE_HCB;
            for (w = 0; w < ics->group_len[g]; w++) {
                float* s = ch->spec + (w0 + w) * 128;
                float energy = 0.0f, scale;
                if (shared) {
                    const float* l = e->ch[0]->spec + (w0 + w) * 128;
                    for (k = start; k < end; k++) {
                        s[k] = l[k];
                        energy += s[k] * s[k];
                    }
                } else {
                    for (k = start; k < end; k++) {
                        s[k] = noise_value(d);
                        energy += s[k] * s[k];
                    }
                }
                if (energy <= 0.0f) continue;
                /* The noise energy has no scale factor offset: the band's RMS is 2^(nrg / 4). */
                scale = sf_gain(ics->sf[g][sfb] + 100, shortw) / sqrtf(energy);
                for (k = start; k < end; k++) s[k] *= scale;
            }
        }
        w0 += ics->group_len[g];
    }
}

static void apply_ms(aac_elem* e)
{
    aac_chan* l = e->ch[0];
    aac_chan* r = e->ch[1];
    const aac_ics* ics = &l->ics;
    int g, sfb, w0 = 0;
    for (g = 0; g < ics->num_groups; g++) {
        for (sfb = 0; sfb < ics->max_sfb; sfb++) {
            int w, k, rcb = r->ics.cb[g][sfb], lcb = l->ics.cb[g][sfb];
            if (!e->ms_used[g][sfb] || rcb >= NOISE_HCB || lcb == NOISE_HCB) continue;
            for (w = 0; w < ics->group_len[g]; w++) {
                float* a = l->spec + (w0 + w) * 128;
                float* bb = r->spec + (w0 + w) * 128;
                for (k = ics->swb[sfb]; k < ics->swb[sfb + 1]; k++) {
                    float m = a[k], s = bb[k];
                    a[k] = m + s;
                    bb[k] = m - s;
                }
            }
        }
        w0 += ics->group_len[g];
    }
}

static void apply_is(aac_elem* e)
{
    aac_chan* l = e->ch[0];
    aac_chan* r = e->ch[1];
    const aac_ics* ics = &r->ics;
    int g, sfb, w0 = 0;
    for (g = 0; g < ics->num_groups; g++) {
        for (sfb = 0; sfb < ics->max_sfb; sfb++) {
            int w, k, cb = ics->cb[g][sfb];
            float scale;
            if (cb != INTENSITY_HCB && cb != INTENSITY_HCB2) continue;
            scale = powf(0.5f, 0.25f * (float)ics->sf[g][sfb]);
            if (cb == INTENSITY_HCB2) scale = -scale;
            if (e->ms_mask_present == 1 && e->ms_used[g][sfb]) scale = -scale;
            for (w = 0; w < ics->group_len[g]; w++) {
                const float* a = l->spec + (w0 + w) * 128;
                float* bb = r->spec + (w0 + w) * 128;
                for (k = ics->swb[sfb]; k < ics->swb[sfb + 1]; k++) bb[k] = a[k] * scale;
            }
        }
        w0 += ics->group_len[g];
    }
}

/* TNS over `spec` (ch->spec, or LTP's prediction of it): the decoder's all-pole
 * filter, or with `analysis` the encoder's inverse (all-zero) filter. */
static void apply_tns(const fa_aac* d, const aac_chan* ch, float* spec, int analysis)
{
    const aac_ics* ics = &ch->ics;
    const aac_tns* t = &ch->tns;
    int shortw = ics->window_sequence == EIGHT_SHORT, w, f;
    int maxb = shortw ? tns_max_bands_short[d->rate_index] : tns_max_bands_long[d->rate_index];
    if (!t->present) return;
    if (maxb > ics->max_sfb) maxb = ics->max_sfb;
    for (w = 0; w < ics->num_windows; w++) {
        float* s = spec + w * 128;
        int bottom = ics->num_swb;
        for (f = 0; f < t->n_filt[w]; f++) {
            int top = bottom, order = t->order[w][f], start, end, size, inc, n, i;
            const float* lpc = t->lpc[w][f];
            float state[TNS_MAX_ORDER];
            bottom = top - t->length[w][f];
            if (bottom < 0) bottom = 0;
            if (!order) continue;
            start = ics->swb[bottom < maxb ? bottom : maxb];
            end = ics->swb[top < maxb ? top : maxb];
            size = end - start;
            if (size <= 0) continue;
            inc = 1;
            if (t->direction[w][f]) {
                inc = -1;
                start = end - 1;
            }
            memset(state, 0, sizeof(state));
            if (analysis) {
                /* All-zero filter y[n] = x[n] + sum lpc[i] x[n - i]; state[0] is x[n-1]. */
                for (n = 0; n < size; n++, start += inc) {
                    float x = s[start], y = x;
                    for (i = 0; i < order; i++) y += lpc[i + 1] * state[i];
                    for (i = order - 1; i > 0; i--) state[i] = state[i - 1];
                    state[0] = x;
                    s[start] = y;
                }
                continue;
            }
            /* All-pole filter y[n] = x[n] - sum lpc[i] y[n - i]; state[0] is y[n-1]. */
            for (n = 0; n < size; n++, start += inc) {
                float y = s[start];
                for (i = 0; i < order; i++) y -= lpc[i + 1] * state[i];
                for (i = order - 1; i > 0; i--) state[i] = state[i - 1];
                state[0] = y;
                s[start] = y;
            }
        }
    }
}

/* ---- AAC Main prediction (4.6.7) -------------------------------------------------------------- */

/* The predictors keep their state at 16-bit mantissa precision, as the standard
 * specifies, rounded the way FFmpeg rounds it (whose output this matches). */
static float flt16_round(float f)
{
    union { float f; uint32_t i; } u;
    u.f = f;
    u.i = (u.i + 0x00008000u) & 0xFFFF0000u;
    return u.f;
}

static float flt16_even(float f)
{
    union { float f; uint32_t i; } u;
    u.f = f;
    u.i = (u.i + 0x00007FFFu + ((u.i & 0x00010000u) >> 16)) & 0xFFFF0000u;
    return u.f;
}

static float flt16_trunc(float f)
{
    union { float f; uint32_t i; } u;
    u.f = f;
    u.i &= 0xFFFF0000u;
    return u.f;
}

/* The predictors work in the standard's spectral scale, 2^25 times ours: their
 * energy threshold and reset variance are 1 there. */
#define PRED_SCALE 33554432.0f

static void reset_pred(aac_pred* p)
{
    p->r0 = p->r1 = p->cor0 = p->cor1 = 0.0f;
    p->var0 = p->var1 = 1.0f;
}

/* State that has decayed to nothing (long digital silence) is set to 0 before
 * it turns denormal, which is slow on some CPUs. It cannot change the output:
 * any coded line is at least 2^-25 here, and the gains need a variance over 1. */
static float flush(float f)
{
    return fabsf(f) < 1e-30f ? 0.0f : f;
}

static void predict(aac_pred* p, float* coef, int use)
{
    const float a = 0.953125f, alpha = 0.90625f;
    float r0 = p->r0, r1 = p->r1, k1, k2, e0, e1;
    k1 = p->var0 > 1.0f ? p->cor0 * flt16_even(a / p->var0) : 0.0f;
    k2 = p->var1 > 1.0f ? p->cor1 * flt16_even(a / p->var1) : 0.0f;
    if (use) *coef += flt16_round(k1 * r0 + k2 * r1);
    e0 = *coef;
    e1 = e0 - k1 * r0;
    p->cor1 = flush(flt16_trunc(alpha * p->cor1 + r1 * e1));
    p->var1 = flush(flt16_trunc(alpha * p->var1 + 0.5f * (r1 * r1 + e1 * e1)));
    p->cor0 = flush(flt16_trunc(alpha * p->cor0 + r0 * e0));
    p->var0 = flush(flt16_trunc(alpha * p->var0 + 0.5f * (r0 * r0 + e0 * e0)));
    p->r1 = flush(flt16_trunc(a * (r0 - k1 * e0)));
    p->r0 = flush(flt16_trunc(a * e0));
}

/* Runs every predictor over this block (each learns from every long block, used
 * or not); short blocks reset them all. 0 when out of memory. */
static int apply_prediction(const fa_aac* d, aac_chan* ch)
{
    const aac_ics* ics = &ch->ics;
    int k, sfb, n = pred_sfb_max[d->rate_index];
    if (!ch->pred) {
        ch->pred = (aac_pred*)malloc(MAX_PRED * sizeof(aac_pred));
        if (!ch->pred) return 0;
        for (k = 0; k < MAX_PRED; k++) reset_pred(&ch->pred[k]);
    }
    if (ics->window_sequence == EIGHT_SHORT) {
        for (k = 0; k < MAX_PRED; k++) reset_pred(&ch->pred[k]);
        return 1;
    }
    for (sfb = 0; sfb < n && sfb < ics->num_swb; sfb++) {
        float* x;
        int use = ics->pred_present && ics->pred_used[sfb];
        for (k = ics->swb[sfb]; k < ics->swb[sfb + 1] && k < MAX_PRED; k++) {
            x = &ch->spec[k];
            *x *= PRED_SCALE;
            predict(&ch->pred[k], x, use);
            *x *= 1.0f / PRED_SCALE;
        }
    }
    if (ics->pred_reset_group)
        for (k = ics->pred_reset_group - 1; k < MAX_PRED; k += 30) reset_pred(&ch->pred[k]);
    return 1;
}

/* ---- AAC LTP (4.6.8) ---------------------------------------------------------------------------- */

/* The forward MDCT of 2048 samples into 1024 lines, scaled so the decoder's
 * filterbank turns them back into the same samples. */
static void mdct_forward(const fa_mdct* t, const float* x, float* out)
{
    float v[1024];
    int j;
    for (j = 0; j < 512; j++) v[j] = -x[1535 - j] - x[1536 + j];
    for (; j < 1024; j++) v[j] = x[j - 512] - x[1535 - j];
    fa_mdct_dct4(t, v, out);
    for (j = 0; j < 1024; j++) out[j] *= 1.0f / 512.0f;
}

/* Adds the prediction from the past output to the bands that use it. */
static void apply_ltp(fa_aac* d, aac_chan* ch)
{
    const aac_ics* ics = &ch->ics;
    const float* lprev = ch->prev_shape ? shared.kbd_long : shared.sine_long;
    const float* lcur = ics->window_shape ? shared.kbd_long : shared.sine_long;
    const float* sprev = ch->prev_shape ? shared.kbd_short : shared.sine_short;
    const float* scur = ics->window_shape ? shared.kbd_short : shared.sine_short;
    float* x = d->ltp_work;
    float* est = d->ltp_work + 2048;
    int i, sfb, n, top;
    if (ics->window_sequence == EIGHT_SHORT || !ics->ltp_present) return;
    n = ics->ltp_lag < 1024 ? ics->ltp_lag + 1024 : 2048;
    for (i = 0; i < n; i++) x[i] = ch->ltp[i + 2048 - ics->ltp_lag] * ics->ltp_coef;
    for (; i < 2048; i++) x[i] = 0.0f;
    /* The current block's window. */
    if (ics->window_sequence == LONG_STOP) {
        for (i = 0; i < 448; i++) x[i] = 0.0f;
        for (i = 0; i < 128; i++) x[448 + i] *= sprev[i];
    } else {
        for (i = 0; i < 1024; i++) x[i] *= lprev[i];
    }
    if (ics->window_sequence == LONG_START) {
        for (i = 0; i < 128; i++) x[1472 + i] *= scur[127 - i];
        for (i = 1600; i < 2048; i++) x[i] = 0.0f;
    } else {
        for (i = 0; i < 1024; i++) x[1024 + i] *= lcur[1023 - i];
    }
    mdct_forward(&d->mdct_long, x, est);
    apply_tns(d, ch, est, 1);
    top = ics->max_sfb < MAX_LTP_SFB ? ics->max_sfb : MAX_LTP_SFB;
    for (sfb = 0; sfb < top; sfb++)
        if (ics->ltp_used[sfb])
            for (i = ics->swb[sfb]; i < ics->swb[sfb + 1]; i++) ch->spec[i] += est[i];
}

/* After the filterbank: the history slides on by this block's output, and the
 * windowed overlap it hands to the next block stands in for that block. */
static void update_ltp(aac_chan* ch)
{
    memmove(ch->ltp, ch->ltp + 1024, 1024 * sizeof(float));
    memcpy(ch->ltp + 1024, ch->out, 1024 * sizeof(float));
    memcpy(ch->ltp + 2048, ch->overlap, 1024 * sizeof(float));
}

/* ---- filterbank ----------------------------------------------------------------------------- */

/* The unwindowed IMDCT of m coefficients into 2m samples, unfolded from the
 * DCT-IV (see mdct.h); u is m floats of scratch. */
static void imdct(const fa_mdct* t, const float* in, float* out, float* u)
{
    int m = t->m, h = m / 2, n;
    fa_mdct_dct4(t, in, u);
    for (n = 0; n < h; n++) out[n] = u[n + h];
    for (; n < 3 * h; n++) out[n] = -u[3 * h - 1 - n];
    for (; n < 2 * m; n++) out[n] = -u[n - 3 * h];
}

static void imdct_block(fa_aac* d, aac_chan* ch)
{
    const aac_ics* ics = &ch->ics;
    const float* lprev = ch->prev_shape ? shared.kbd_long : shared.sine_long;
    const float* lcur = ics->window_shape ? shared.kbd_long : shared.sine_long;
    const float* sprev = ch->prev_shape ? shared.kbd_short : shared.sine_short;
    const float* scur = ics->window_shape ? shared.kbd_short : shared.sine_short;
    float* z = d->buf;
    int n, w;
    if (ics->window_sequence == EIGHT_SHORT) {
        float y[256], u[128];
        memset(z, 0, sizeof(float) * 2048);
        for (w = 0; w < 8; w++) {
            const float* rise = w == 0 ? sprev : scur;
            float* o = z + 448 + 128 * w;
            imdct(&d->mdct_short, ch->spec + 128 * w, y, u);
            for (n = 0; n < 128; n++) {
                o[n] += y[n] * rise[n];
                o[128 + n] += y[128 + n] * scur[127 - n];
            }
        }
    } else {
        float u[1024];
        imdct(&d->mdct_long, ch->spec, z, u);
        if (ics->window_sequence == LONG_STOP) {
            for (n = 0; n < 448; n++) z[n] = 0.0f;
            for (n = 0; n < 128; n++) z[448 + n] *= sprev[n];
        } else {
            for (n = 0; n < 1024; n++) z[n] *= lprev[n];
        }
        if (ics->window_sequence == LONG_START) {
            for (n = 0; n < 128; n++) z[1472 + n] *= scur[127 - n];
            for (n = 1600; n < 2048; n++) z[n] = 0.0f;
        } else {
            for (n = 0; n < 1024; n++) z[1024 + n] *= lcur[1023 - n];
        }
    }
    for (n = 0; n < 1024; n++) ch->out[n] = z[n] + ch->overlap[n];
    memcpy(ch->overlap, z + 1024, sizeof(float) * 1024);
    ch->prev_shape = ics->window_shape;
}

/* ---- raw data block ---------------------------------------------------------------------------- */

static int read_ms_mask(fa_aac_bits* b, aac_elem* e)
{
    const aac_ics* ics = &e->ch[0]->ics;
    int g, sfb;
    e->ms_mask_present = (int)fa_aac_get(b, 2);
    if (e->ms_mask_present == 3) return 0;
    for (g = 0; g < ics->num_groups; g++)
        for (sfb = 0; sfb < ics->max_sfb; sfb++)
            e->ms_used[g][sfb] = e->ms_mask_present == 2 ? 1 : e->ms_mask_present == 1 ? (uint8_t)fa_aac_get(b, 1) : 0;
    return 1;
}

static int read_cpe(fa_aac* d, fa_aac_bits* b, aac_elem* e)
{
    int common = (int)fa_aac_get(b, 1), r;
    if (common) {
#ifdef FA_AAC_TRACE
        fprintf(stderr, "COMMON\n");
#endif
        r = read_ics_info(d, b, &e->ch[0]->ics);
        if (r <= 0) return r;
        e->ch[1]->ics = e->ch[0]->ics;
        if (e->ch[1]->ics.pred_present && d->object_type == AOT_LTP) {
            /* The second channel's own long-term prediction. */
            e->ch[1]->ics.ltp_present = (int)fa_aac_get(b, 1);
            if (e->ch[1]->ics.ltp_present) read_ltp(b, &e->ch[1]->ics);
        }
        if (!read_ms_mask(b, e)) return 0;
    } else {
        e->ms_mask_present = 0;
    }
    r = read_ics(d, b, e->ch[0], common, 0);
    if (r <= 0) return r;
    r = read_ics(d, b, e->ch[1], common, 0);
    if (r <= 0) return r;
    if (!common) {
        /* The two windows must match for M/S, which needs a common window. */
        e->ms_mask_present = 0;
    }
    return 1;
}

/* coupling_channel_element(): read and dropped. */
static int read_cce(fa_aac* d, fa_aac_bits* b, aac_chan* scratch)
{
    int ind_sw, n_coupled, i, lists = 0, r, g, sfb;
    fa_aac_skip(b, 4);
    ind_sw = (int)fa_aac_get(b, 1);
    n_coupled = (int)fa_aac_get(b, 3);
    for (i = 0; i <= n_coupled; i++) {
        int is_cpe = (int)fa_aac_get(b, 1);
        lists++;
        fa_aac_skip(b, 4);
        if (is_cpe) {
            int cl = (int)fa_aac_get(b, 1), cr = (int)fa_aac_get(b, 1);
            if (cl && cr) lists++;
        }
    }
    fa_aac_skip(b, 1 + 1 + 2);                    /* cc_domain, gain_element_sign, gain_element_scale */
    r = read_ics(d, b, scratch, 0, 0);
    if (r <= 0) return r;
    for (i = 1; i < lists; i++) {
        int cge = ind_sw ? 1 : (int)fa_aac_get(b, 1);
        if (cge) {
            if (fa_aac_vlc(b, fa_aac_hcb_sf, FA_AAC_VLC_ROOT) < 0) return 0;
            continue;
        }
        for (g = 0; g < scratch->ics.num_groups; g++)
            for (sfb = 0; sfb < scratch->ics.max_sfb; sfb++)
                if (scratch->ics.cb[g][sfb] != ZERO_HCB && fa_aac_vlc(b, fa_aac_hcb_sf, FA_AAC_VLC_ROOT) < 0) return 0;
    }
    return fa_aac_left(b) >= 0;
}

static fa_aac_sbr* sbr_create(fa_aac* d, int channels)
{
    if (!d->qmf) d->qmf = fa_aac_qmf_create();
    return d->qmf ? fa_aac_sbr_create(d->rate, channels, d->qmf) : NULL;
}

/* fill_element(): SBR data goes to `prev` (the element before it). */
static int read_fil(fa_aac* d, fa_aac_bits* b, aac_elem* prev)
{
    size_t end;
    int count;
#ifdef FA_AAC_TRACE
    fprintf(stderr, "FIL %zu\n", b->pos);   /* for tests/data/aac/ps_rewrite.py */
#endif
    count = (int)fa_aac_get(b, 4);
    if (count == 15) count += (int)fa_aac_get(b, 8) - 1;
    if (count == 0) return 1;
    end = b->pos + (size_t)count * 8;
    if (end > b->end) return 0;
    {
        int type = (int)fa_aac_get(b, 4);
        if ((type == EXT_SBR_DATA || type == EXT_SBR_DATA_CRC) && prev && prev->type != ID_LFE) {
            if (d->sbr_signalled != 0) {
                if (!prev->sbr) prev->sbr = sbr_create(d, prev->nch);
                if (prev->sbr) {
                    fa_aac_sbr_parse(prev->sbr, b, count * 8 - 4, type == EXT_SBR_DATA_CRC);
                    d->sbr_on = 1;
                }
            }
        }
    }
    b->pos = end;
    return 1;
}

static int speakers_layout(int nch, int sp)
{
    /* Output index of speaker `sp` in the layout of nch channels, -1 if absent. */
    static const int8_t idx[9][SP_COUNT] = {
        /*        L   R   C  LFE  BL  BR  SL  SR  BC */
        /* 0 */ { -1, -1, -1, -1, -1, -1, -1, -1, -1 },
        /* 1 */ { -1, -1, 0, -1, -1, -1, -1, -1, -1 },
        /* 2 */ { 0, 1, -1, -1, -1, -1, -1, -1, -1 },
        /* 3 */ { 0, 1, 2, -1, -1, -1, -1, -1, -1 },
        /* 4 */ { 0, 1, 2, -1, -1, -1, -1, -1, 3 },
        /* 5 */ { 0, 1, 2, -1, 3, 4, -1, -1, -1 },
        /* 6 */ { 0, 1, 2, 3, -1, -1, 4, 5, -1 },
        /* 7 */ { 0, 1, 2, 3, -1, -1, 5, 6, 4 },
        /* 8 */ { 0, 1, 2, 3, 4, 5, 6, 7, -1 } };
    return idx[nch][sp];
}

/* Where an element sits: 0 front, 1 side, 2 back, 3 LFE. */
static int element_area(const fa_aac* d, const aac_elem* e, int* known)
{
    int i;
    *known = 1;
    if (e->type == ID_LFE) return 3;
    if (d->pce.valid) {
        for (i = 0; i < d->pce.n_front; i++)
            if (d->pce.front[i].tag == e->tag && d->pce.front[i].is_cpe == (e->type == ID_CPE)) return 0;
        for (i = 0; i < d->pce.n_side; i++)
            if (d->pce.side[i].tag == e->tag && d->pce.side[i].is_cpe == (e->type == ID_CPE)) return 1;
        for (i = 0; i < d->pce.n_back; i++)
            if (d->pce.back[i].tag == e->tag && d->pce.back[i].is_cpe == (e->type == ID_CPE)) return 2;
        *known = 0;
        return 0;
    }
    /* A channel configuration (or none): by order of appearance. */
    switch (d->channel_config) {
    case 4: if (e->type == ID_SCE && e->order == 1) return 2; break;          /* back centre */
    case 5: case 6: if (e->type == ID_CPE && e->order == 1) return 2; break;  /* surrounds */
    case 7: if (e->type == ID_CPE && e->order >= 1) return e->order == 1 ? 1 : 2; break;
    case 11:
        if (e->type == ID_CPE && e->order == 1) return 1;
        if (e->type == ID_SCE && e->order == 1) return 2;
        break;
    case 12: if (e->type == ID_CPE && e->order >= 1) return e->order == 1 ? 1 : 2; break;
    case 0: case 1: case 2: case 3: default:
        if (e->type == ID_CPE && e->order == 1) return 2;
        if (e->type == ID_SCE && e->order >= 1 && d->channel_config == 0) return 2;
        break;
    }
    return 0;
}

/* Sorts the decoded elements by speaker area. Returns the channel count. */
static int plan_mix(fa_aac* d, mix_layout* L)
{
    aac_elem* fronts_c[MAX_ELEM];
    aac_elem* fronts_pair[MAX_ELEM];
    aac_elem* pairs[2 * MAX_ELEM];
    aac_elem* backs_c[MAX_ELEM];
    aac_elem* lfes[MAX_ELEM];
    int nfc = 0, nfp = 0, np = 0, nbc = 0, nlfe = 0, nside = 0, i, nch, pass;
    /* Sort the decoded elements by area; within an area by order of appearance. */
    for (pass = 0; pass < 3; pass++) {
        for (i = 0; i < d->nelem; i++) {
            aac_elem* e = d->elem[i];
            int known, area, stereo;
            if (!e->present) continue;
            area = element_area(d, e, &known);
            stereo = e->stereo_out;
            if (pass == 0) {
                if (area == 3) lfes[nlfe++] = e;
                else if (area == 0 && stereo) fronts_pair[nfp++] = e;
                else if (area == 0) fronts_c[nfc++] = e;
            } else if (pass == 1) {
                if (area == 1 && stereo) { pairs[np++] = e; nside++; }
                else if (area == 1) backs_c[nbc++] = e;
            } else {
                if (area == 2 && stereo) pairs[np++] = e;
                else if (area == 2) backs_c[nbc++] = e;
            }
        }
    }
    /* Front pairs after the first count as side surrounds. */
    if (nfp > 1) {
        memmove(pairs + (nfp - 1), pairs, sizeof(pairs[0]) * (size_t)np);
        for (i = 1; i < nfp; i++) pairs[i - 1] = fronts_pair[i];
        np += nfp - 1;
        nside += nfp - 1;
    }
    /* Two front singles and nothing else in front: a pair (dual mono, or L and R sent apart). */
    if (nfp == 0 && nfc >= 2 && np == 0 && nbc == 0 && nlfe == 0) nch = 2;
    else if (np >= 2) nch = 8;
    else if (np == 1 && nbc) nch = 7;
    else if (np == 1) nch = nlfe ? 6 : 5;
    else if (nbc) nch = nlfe ? 7 : 4;
    else if (nlfe) nch = 6;
    else if (nfp && nfc) nch = 3;
    else if (nfp) nch = 2;
    else nch = 1;
    memcpy(L->fronts_c, fronts_c, sizeof(fronts_c[0]) * (size_t)nfc);
    memcpy(L->fronts_pair, fronts_pair, sizeof(fronts_pair[0]) * (size_t)nfp);
    memcpy(L->pairs, pairs, sizeof(pairs[0]) * (size_t)np);
    memcpy(L->backs_c, backs_c, sizeof(backs_c[0]) * (size_t)nbc);
    memcpy(L->lfes, lfes, sizeof(lfes[0]) * (size_t)nlfe);
    L->nfc = nfc; L->nfp = nfp; L->np = np; L->nbc = nbc; L->nlfe = nlfe; L->nside = nside; L->nch = nch;
    return nch;
}

/* Mixes the elements of a plan into `out` (frames x L->nch, interleaved). */
static void render_mix(const mix_layout* L, float* out, int frames)
{
    aac_elem* const* fronts_c = L->fronts_c;
    aac_elem* const* fronts_pair = L->fronts_pair;
    aac_elem* const* pairs = L->pairs;
    aac_elem* const* backs_c = L->backs_c;
    aac_elem* const* lfes = L->lfes;
    const int nfc = L->nfc, nfp = L->nfp, np = L->np, nbc = L->nbc, nlfe = L->nlfe, nside = L->nside, nch = L->nch;
    int i;
    memset(out, 0, sizeof(float) * (size_t)frames * (size_t)nch);
#define ADD(src, sp, g) do { \
        int o_ = speakers_layout(nch, sp), n_; \
        if (o_ >= 0) for (n_ = 0; n_ < frames; n_++) out[(size_t)n_ * nch + o_] += (src)[n_] * (g); \
    } while (0)
    if (nch == 2 && nfp == 0) {
        ADD(fronts_c[0]->ch[0]->out, SP_L, 1.0f);
        ADD(fronts_c[1]->ch[0]->out, SP_R, 1.0f);
        for (i = 2; i < nfc; i++) {
            ADD(fronts_c[i]->ch[0]->out, SP_L, 0.70710678f);
            ADD(fronts_c[i]->ch[0]->out, SP_R, 0.70710678f);
        }
        return;
    }
    for (i = 0; i < nfc; i++) {
        if (nch == 2 || (nch == 1 && i > 0)) {
            /* A centre in a layout without one. */
            if (nch == 2) {
                ADD(fronts_c[i]->ch[0]->out, SP_L, 0.70710678f);
                ADD(fronts_c[i]->ch[0]->out, SP_R, 0.70710678f);
            } else {
                ADD(fronts_c[i]->ch[0]->out, SP_C, 1.0f);
            }
        } else {
            ADD(fronts_c[i]->ch[0]->out, SP_C, i == 0 ? 1.0f : 0.70710678f);
        }
    }
    if (nfp) {
        ADD(fronts_pair[0]->ch[0]->out, SP_L, 1.0f);
        ADD(fronts_pair[0]->ch[1]->out, SP_R, 1.0f);
    }
    for (i = 0; i < np; i++) {
        aac_elem* e = pairs[i];
        int sl, sr;
        float g = 1.0f;
        if (nch == 8) {
            if (i == 0 && nside > 0) { sl = SP_SL; sr = SP_SR; }
            else if (i == 0 && np >= 2) { sl = SP_SL; sr = SP_SR; }
            else if (i == 1) { sl = SP_BL; sr = SP_BR; }
            else { sl = SP_BL; sr = SP_BR; g = 0.70710678f; }
        } else if (nch == 5) {
            sl = SP_BL;
            sr = SP_BR;
        } else {
            sl = SP_SL;
            sr = SP_SR;
        }
        ADD(e->ch[0]->out, sl, g);
        ADD(e->ch[1]->out, sr, g);
    }
    for (i = 0; i < nbc; i++) {
        const float* s = backs_c[i]->ch[0]->out;
        if (nch == 4 || nch == 7) {
            ADD(s, SP_BC, i == 0 ? 1.0f : 0.70710678f);
        } else if (nch == 8) {
            ADD(s, SP_BL, 0.70710678f);
            ADD(s, SP_BR, 0.70710678f);
        }
    }
    for (i = 0; i < nlfe; i++) ADD(lfes[i]->ch[0]->out, SP_LFE, i == 0 ? 1.0f : 0.70710678f);
#undef ADD
}

/* One raw_data_block(): decodes it into the elements' out buffers and plans their
 * mix (*layout). Returns 1 and sets *frames, *nch; 0 if damaged; -1 if it uses an
 * unsupported feature; -2 out of memory. */
static int decode_block(fa_aac* d, fa_aac_bits* b, int* frames, int* nch, mix_layout* layout)
{
    aac_elem* prev = NULL;
    int counts[4] = { 0, 0, 0, 0 }, i, any = 0, r, n;
    size_t start = b->pos;
    for (i = 0; i < d->nelem; i++) {
        d->elem[i]->present = 0;
    }
    for (;;) {
        int id = (int)fa_aac_get(b, 3);
        if (fa_aac_left(b) < 0) return 0;
        if (id == ID_END) {
#ifdef FA_AAC_TRACE
            fprintf(stderr, "END %zu\n", b->pos);
#endif
            break;
        }
        switch (id) {
        case ID_SCE: case ID_CPE: case ID_LFE: {
            int tag = (int)fa_aac_get(b, 4);
            aac_elem* e = get_elem(d, id, tag);
            if (!e || e->present) return 0;
            e->order = counts[id]++;
            r = id == ID_CPE ? read_cpe(d, b, e) : read_ics(d, b, e->ch[0], 0, id == ID_LFE);
            if (r <= 0) return r;
            e->present = 1;
            prev = e;
            any = 1;
            break;
        }
        case ID_CCE: {
            aac_chan* scratch = (aac_chan*)malloc(sizeof(aac_chan));
            if (!scratch) return 0;
            r = read_cce(d, b, scratch);
            free(scratch);
            if (r <= 0) return r;
            break;
        }
        case ID_DSE: {
            int align, count;
            fa_aac_skip(b, 4);
            align = (int)fa_aac_get(b, 1);
            count = (int)fa_aac_get(b, 8);
            if (count == 255) count += (int)fa_aac_get(b, 8);
            if (align) byte_align(b, start);
            fa_aac_skip(b, (size_t)count * 8);
            break;
        }
        case ID_PCE: {
            aac_pce p;
            if (read_pce(b, &p, start) && d->channel_config == 0) d->pce = p;
            break;
        }
        case ID_FIL:
            if (!read_fil(d, b, prev)) return 0;
            break;
        }
        if (fa_aac_left(b) < 0) return 0;
    }
    byte_align(b, start);
    if (!any) return 0;
    /* Spectral tools and the filterbank. */
    for (i = 0; i < d->nelem; i++) {
        aac_elem* e = d->elem[i];
        int c;
        if (!e->present) continue;
        for (c = 0; c < e->nch; c++) apply_pns(d, e, c);   /* dequantized when read */
        if (e->nch == 2 && e->ms_mask_present) apply_ms(e);
        if (d->object_type == AOT_MAIN)
            for (c = 0; c < e->nch; c++)
                if (!apply_prediction(d, e->ch[c])) return 0;
        if (e->nch == 2) apply_is(e);
        if (d->object_type == AOT_LTP)
            for (c = 0; c < e->nch; c++)
                if (!e->ch[c]->ltp && !(e->ch[c]->ltp = (float*)calloc(3072, sizeof(float)))) return -2;
        if (d->object_type == AOT_LTP && !d->ltp_work && !(d->ltp_work = (float*)malloc(sizeof(float) * 3072))) return -2;
        for (c = 0; c < e->nch; c++) {
            if (d->object_type == AOT_LTP) apply_ltp(d, e->ch[c]);
            apply_tns(d, e->ch[c], e->ch[c]->spec, 0);
            imdct_block(d, e->ch[c]);
            if (d->object_type == AOT_LTP) update_ltp(e->ch[c]);
        }
        e->stereo_out = e->nch == 2;
    }
    n = FA_AAC_FRAME;
    if (d->sbr_on) {
        for (i = 0; i < d->nelem; i++) {
            aac_elem* e = d->elem[i];
            const float* in[2];
            float* o[2];
            float* core = d->buf;      /* free after the filterbank */
            int c, stereo;
            int ps_possible = e->type == ID_SCE && d->nelem == 1 && d->ps_signalled != 0;
            if (!e->present) continue;
            if (!e->sbr) {
                e->sbr = sbr_create(d, e->nch);
                if (!e->sbr) return 0;
            }
            for (c = 0; c < e->nch; c++) memcpy(core + 1024 * c, e->ch[c]->out, sizeof(float) * 1024);
            in[0] = core;
            in[1] = e->nch > 1 ? core + 1024 : NULL;
            o[0] = e->ch[0]->out;
            if (e->nch > 1) {
                o[1] = e->ch[1]->out;
            } else {
                /* A mono stream's element may turn stereo (parametric stereo):
                 * its right channel goes to a second channel allocated on demand. */
                if (!e->ch[1] && ps_possible) e->ch[1] = (aac_chan*)calloc(1, sizeof(aac_chan));
                o[1] = e->ch[1] && ps_possible ? e->ch[1]->out : NULL;
            }
            stereo = fa_aac_sbr_apply(e->sbr, in, o);
            if (o[1] && e->nch == 1) {
                /* Stereo from the first frame, so the layout does not change when
                 * the first PS data arrives (PS needs an SBR header first). */
                if (!stereo) memcpy(o[1], o[0], sizeof(float) * 2 * FA_AAC_FRAME);
                e->stereo_out = 1;
            }
        }
        n = 2 * FA_AAC_FRAME;
    }
    *frames = n;
    *nch = plan_mix(d, layout);
    return 1;
}

/* ---- packets ------------------------------------------------------------------------------- */

typedef struct adts_header {
    size_t len, header;      /* whole frame; header bytes (7, or 9 with a CRC) */
    int profile, rate_index, config, blocks, crc;
} adts_header;

static int parse_adts(const uint8_t* p, size_t n, adts_header* h)
{
    if (n < 7 || p[0] != 0xFF || (p[1] & 0xF6) != 0xF0) return 0;
    h->crc = !(p[1] & 1);
    h->profile = p[2] >> 6;
    h->rate_index = (p[2] >> 2) & 15;
    h->config = ((p[2] & 1) << 2) | (p[3] >> 6);
    h->len = ((size_t)(p[3] & 3) << 11) | ((size_t)p[4] << 3) | (p[5] >> 5);
    h->blocks = (p[6] & 3) + 1;
    h->header = h->crc ? 9 : 7;
    if (h->rate_index >= 13 || h->len < h->header || h->len > n) return 0;
    return 1;
}

fa_aac* fa_aac_create(void)
{
    fa_aac* d = (fa_aac*)calloc(1, sizeof(*d));
    if (!d) return NULL;
    if (fa_imdct_init(&d->mdct_long, 1024) != FA_OK || fa_imdct_init(&d->mdct_short, 128) != FA_OK) {
        fa_aac_destroy(d);
        return NULL;
    }
    build_shared();
    d->seed = PNS_SEED;
    d->sbr_signalled = -1;
    d->ps_signalled = -1;
    return d;
}

void fa_aac_destroy(fa_aac* d)
{
    int i;
    if (!d) return;
    for (i = 0; i < d->nelem; i++) free_elem(d->elem[i]);
    fa_imdct_free(&d->mdct_long);
    fa_imdct_free(&d->mdct_short);
    free(d->ltp_work);
    fa_aac_qmf_destroy(d->qmf);
    free(d->pkt);
    free(d);
}

void fa_aac_reset(fa_aac* d)
{
    int i;
    for (i = 0; i < d->nelem; i++) {
        aac_elem* e = d->elem[i];
        int c;
        for (c = 0; c < 2; c++) {
            if (!e->ch[c]) continue;
            memset(e->ch[c]->overlap, 0, sizeof(e->ch[c]->overlap));
            e->ch[c]->prev_shape = 0;
            /* The predictors start again from nothing. */
            free(e->ch[c]->pred);
            e->ch[c]->pred = NULL;
            if (e->ch[c]->ltp) memset(e->ch[c]->ltp, 0, 3072 * sizeof(float));
        }
        if (e->sbr) fa_aac_sbr_reset(e->sbr);
    }
}

/* A new configuration from an ADTS header: drops the elements when it changes. */
static void adts_configure(fa_aac* d, const adts_header* h)
{
    int i;
    if (d->configured && d->rate_index == h->rate_index && d->channel_config == h->config &&
        d->object_type == h->profile + 1)
        return;
    for (i = 0; i < d->nelem; i++) free_elem(d->elem[i]);
    d->nelem = 0;
    d->configured = 1;
    d->object_type = h->profile + 1;
    d->rate_index = h->rate_index;
    d->rate = fa_aac_rates[h->rate_index];
    d->channel_config = h->config;
    d->sbr_signalled = -1;
    d->ps_signalled = -1;
    d->sbr_on = 0;
    if (h->config != 0) memset(&d->pce, 0, sizeof(d->pce));
}

fa_result fa_aac_decode(fa_aac* d, const uint8_t* data, size_t size, float** pcm, size_t* cap, int* frames,
                        int* channels, uint32_t* rate)
{
    adts_header h;
    fa_aac_bits b;
    int blocks = 1, blk, damaged = 0, unsupported = 0, total = 0, nch = 0;
    size_t payload = 0;
    memset(&h, 0, sizeof(h));
    *frames = 0;
    if (!data || size == 0) return FA_OK;
#ifdef FA_AAC_TRACE
    fprintf(stderr, "FRAME\n");
#endif
    /* ADTS frames, also after an AudioSpecificConfig if one exactly fills the packet
     * (FFmpeg hands MPEG-TS audio over with its headers). */
    if (parse_adts(data, size, &h) && (!d->from_asc || h.len == size)) {
        adts_configure(d, &h);
        if (!core_supported(d->object_type)) return FA_ERROR_UNSUPPORTED_FORMAT;
        blocks = h.blocks;
        payload = h.header;
        size = h.len;
        if (h.crc && blocks > 1) payload += 2 * (size_t)(blocks - 1);   /* raw_data_block_position */
    } else if (!d->configured || !d->from_asc) {
        return FA_ERROR_GENERIC;                  /* not a whole ADTS frame */
    }
    if (size + 16 > d->pkt_cap) {
        uint8_t* p = (uint8_t*)realloc(d->pkt, size + 16);
        if (!p) return FA_ERROR_OUT_OF_MEMORY;
        d->pkt = p;
        d->pkt_cap = size + 16;
    }
    memcpy(d->pkt, data, size);
    memset(d->pkt + size, 0, 16);
    b.p = d->pkt;
    b.pos = payload * 8;
    b.end = size * 8;
    for (blk = 0; blk < blocks; blk++) {
        int n = 0, c = 0, r;
        size_t need;
        float* dst;
        mix_layout layout;
        r = damaged || unsupported ? 0 : decode_block(d, &b, &n, &c, &layout);
        if (r == -2) return FA_ERROR_OUT_OF_MEMORY;
        if (r < 0) unsupported = 1;
        if (r <= 0) {
            /* The rest of the frame can't be found: silence of the block's length,
             * once the format is known. */
            if (r == 0) damaged = 1;
            fa_aac_reset(d);
            if (!d->out_channels) continue;
            n = d->sbr_on ? 2 * FA_AAC_FRAME : FA_AAC_FRAME;
            c = d->out_channels;
        }
        if (nch && c != nch) break;          /* the layout changed inside one frame: keep the first part */
        nch = c;
        need = (size_t)(total + n) * (size_t)c;
        if (need > *cap) {
            /* The first frame sets the size (one frame's audio, exactly); doubling after. */
            size_t nc = *cap ? *cap : need;
            float* p;
            while (nc < need) nc *= 2;
            p = (float*)realloc(*pcm, nc * sizeof(float));
            if (!p) return FA_ERROR_OUT_OF_MEMORY;
            *pcm = p;
            *cap = nc;
        }
        /* Mixed straight into the output. */
        dst = *pcm + (size_t)total * c;
        if (r > 0) render_mix(&layout, dst, n);
        else memset(dst, 0, sizeof(float) * (size_t)n * (size_t)c);
        total += n;
        if (h.crc && blocks > 1) fa_aac_skip(&b, 16);   /* the block's CRC */
        if (r > 0) {
            d->out_channels = c;
            d->out_rate = d->sbr_on ? d->rate * 2 : d->rate;
        }
    }
    *frames = total;
    *channels = nch;
    *rate = d->out_rate;
    if (unsupported) return FA_ERROR_UNSUPPORTED_FORMAT;
    if (damaged) return FA_ERROR_GENERIC;
    return FA_OK;
}

/* ---- the "aac" packet codec ------------------------------------------------------------------ */

static fa_result aacc_decode(fa_codec* c, const uint8_t* data, size_t size)
{
    fa_aac* d = (fa_aac*)c->impl;
    size_t cap = (size_t)c->out_cap;
    int frames = 0, channels = 0;
    uint32_t rate = 0;
    fa_result r;
    c->out_frames = 0;
    if (!data || size == 0) return FA_OK;                /* nothing held back */
    r = fa_aac_decode(d, data, size, &c->out, &cap, &frames, &channels, &rate);
    c->out_cap = (fa_frames)cap;
    if (r == FA_ERROR_OUT_OF_MEMORY) return r;
    if (frames <= 0) return r == FA_OK ? FA_OK : r;
    /* A damaged block came out as silence of its length. */
    c->out_frames = frames;
    c->channels = (uint32_t)channels;
    c->rate = rate;
    return FA_OK;
}

static void aacc_flush(fa_codec* c)
{
    fa_aac* d = (fa_aac*)c->impl;
    fa_aac_reset(d);
    /* The noise generator starts again too, so a seek to the start gives exactly
     * a straight decode's audio. */
    d->seed = PNS_SEED;
}

static void aacc_close(fa_codec* c)
{
    fa_aac_destroy((fa_aac*)c->impl);
    c->impl = NULL;
}

static const fa_codec_vtable aacc_vtable = { "aac", aacc_decode, aacc_flush, aacc_close };

fa_result fa_aac_codec_open(fa_codec* c, const fa_codec_params* p)
{
    fa_aac* d = fa_aac_create();
    if (!d) return FA_ERROR_OUT_OF_MEMORY;
    if (p->extradata_size > 0) {
        fa_result r = fa_aac_set_config(d, (const uint8_t*)p->extradata, p->extradata_size);
        if (r != FA_OK) {
            fa_aac_destroy(d);
            return r;
        }
    }
    c->impl = d;
    c->vt = &aacc_vtable;
    c->rate = p->sample_rate;
    c->channels = p->channels;
    return FA_OK;
}
