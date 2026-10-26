/* Parametric stereo (HE-AAC v2), written from ISO/IEC 14496-3 subpart 8, 8.6.4:
 * ps_data() with its IID, ICC and IPD/OPD parameters, the hybrid analysis that
 * splits the lowest QMF bands (10 or 34 parameter-band configurations), the
 * decorrelator (fractional delay and three all-pass links, with transient
 * ducking), the mixing matrices of procedures R_A and R_B interpolated across
 * the envelopes, and the hybrid synthesis. It runs on the mono element's QMF
 * matrix inside SBR (aac_sbr.c), using the low bands' six slots of lookahead so
 * the hybrid filters add no delay. */
#include "aac.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SLOTS 32
#define MAX_ENV 5            /* 4 plus one added to reach the frame's end */
#define MAX_PAR 34
#define MAX_BANDS 91         /* hybrid bands plus the QMF bands above them */
#define MAX_DELAY 14
#define AP_LINKS 3
#define MAX_AP_DELAY 5

typedef struct ps_config {
    int nbands;              /* hybrid bands + QMF bands */
    int nhybrid;             /* hybrid bands */
    int qsplit;              /* QMF bands that are split */
    int npar;                /* parameter bands */
    int allpass_bands, short_delay_band, decay_cutoff, nipd;
    const int8_t* k_to_i;
} ps_config;

/* Constant tables, built once per process by ps_tables_init (same pattern as ac3_init_tables). */
typedef struct ps_tables {
    float phi_fract[2][MAX_BANDS][2];
    float q_fract[2][MAX_BANDS][AP_LINKS][2];
    float f8[8][13][2], f12[12][13][2], f8_34[8][13][2], f4[4][13][2];
    float pd_smooth[512][2];
} ps_tables;

struct fa_aac_ps {
    /* header */
    int header_seen;
    int enable_iid, iid_mode, enable_icc, icc_mode, enable_ext, enable_ipdopd;
    /* frame */
    int num_env;
    int border[MAX_ENV + 1];
    int8_t iid[MAX_ENV][MAX_PAR], icc[MAX_ENV][MAX_PAR], ipd[MAX_ENV][17], opd[MAX_ENV][17];
    int8_t iid_last[MAX_PAR], icc_last[MAX_PAR], ipd_last[17], opd_last[17];
    int nr_iid_last, nr_icc_last, nr_ipd_last;
    /* state */
    int is34, is34_last;
    ps_config cfg[2];
    float in_hist[5][6][2];                  /* the last 6 slots of the split QMF bands */
    float delay[MAX_BANDS][MAX_DELAY][2];    /* the last slots of each band's delay line */
    float ap_delay[MAX_BANDS][AP_LINKS][MAX_AP_DELAY][2];
    float peak_decay[MAX_PAR], power_smooth[MAX_PAR], peak_diff_smooth[MAX_PAR];
    float h_last[4][MAX_PAR][2];             /* mixing matrix at the end of the last envelope (re, im) */
    uint8_t ipd_hist[MAX_PAR], opd_hist[MAX_PAR];
    /* tables */
    const struct ps_tables* t;               /* shared, built once per process */
    /* work */
    float s[MAX_BANDS][SLOTS][2], d[MAX_BANDS][SLOTS][2];
    float hybrid_in[5][6 + 38][2];
    float dl[MAX_DELAY + SLOTS][2];          /* one band's delay line: its history, then this frame */
    float ap[AP_LINKS][MAX_AP_DELAY + SLOTS][2];   /* and its all-pass links' */
};

static const int8_t k_to_i_20[71] = {
    1, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 14, 15, 15, 15, 16, 16, 16, 16, 17, 17, 17, 17, 17,
    18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
    19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19 };

static const int8_t k_to_i_34[91] = {
    0, 1, 2, 3, 4, 5, 6, 6, 7, 2, 1, 0, 10, 10, 4, 5, 6, 7, 8, 9, 10, 11, 12, 9, 14, 11, 12, 13, 14, 15, 16, 13,
    16, 17, 18, 19, 20, 21, 22, 22, 23, 23, 24, 24, 25, 25, 26, 26, 27, 27, 27, 28, 28, 28, 29, 29, 29, 30, 30,
    30, 31, 31, 31, 31, 32, 32, 32, 32, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33,
    33, 33, 33, 33, 33 };

/* Centre frequencies of the hybrid bands, in QMF bands (times 8 and 24). */
static const int8_t f_center_20[10] = { -3, -1, 1, 3, 5, 7, 10, 14, 18, 22 };
static const int8_t f_center_34[32] = { 2, 6, 10, 14, 18, 22, 26, 30, 34, -10, -6, -2, 51, 57, 15, 21,
                                        27, 33, 39, 45, 54, 66, 78, 42, 102, 66, 78, 90, 102, 114, 126, 90 };

static const int num_env_tab[2][4] = { { 0, 1, 2, 4 }, { 1, 2, 3, 4 } };
static const int nr_par_tab[6] = { 10, 20, 34, 10, 20, 34 };
static const int nr_ipd_tab[6] = { 5, 11, 17, 5, 11, 17 };

/* IID steps in dB (default and fine) and ICC correlations. */
static const float iid_default_db[15] = { -25, -18, -14, -10, -7, -4, -2, 0, 2, 4, 7, 10, 14, 18, 25 };
static const float iid_fine_db[31] = { -50, -45, -40, -35, -30, -25, -22, -19, -16, -13, -10, -8, -6, -4, -2, 0,
                                       2, 4, 6, 8, 10, 13, 16, 19, 22, 25, 30, 35, 40, 45, 50 };
static const float icc_rho[8] = { 1.0f, 0.937f, 0.84118f, 0.60092f, 0.36764f, 0.0f, -0.589f, -1.0f };

static void make_filter(float (*f)[13][2], const float* proto, int bands)
{
    int q, n;
    for (q = 0; q < bands; q++)
        for (n = 0; n < 13; n++) {
            float g = proto[n < 7 ? n : 12 - n];
            double theta = 2.0 * FA_PI * (q + 0.5) * (n - 6) / bands;
            f[q][n][0] = (float)(g * cos(theta));
            f[q][n][1] = (float)(g * -sin(theta));
        }
}

static ps_tables g_tables;
static fa_atomic32 g_tables_state;   /* 0 not done, 1 in progress, 2 done */

static void ps_tables_init(void)
{
    static const float q_link[AP_LINKS] = { 0.43f, 0.75f, 0.347f };
    ps_tables* t = &g_tables;
    int c, k, m, i;
    if (fa_load32(&g_tables_state) == 2) return;
    if (!fa_cas32(&g_tables_state, 0, 1)) {
        while (fa_load32(&g_tables_state) != 2) fa_sleep_ms(1);
        return;
    }
    for (c = 0; c < 2; c++) {
        int nbands = c ? 91 : 71, nhybrid = c ? 32 : 10, qsplit = c ? 5 : 3;
        for (k = 0; k < nbands; k++) {
            float f;
            if (k < nhybrid) f = c ? f_center_34[k] / 24.0f : f_center_20[k] / 8.0f;
            else f = (float)(k - nhybrid + qsplit) + 0.5f;
            t->phi_fract[c][k][0] = (float)cos(-FA_PI * 0.39 * f);
            t->phi_fract[c][k][1] = (float)sin(-FA_PI * 0.39 * f);
            for (m = 0; m < AP_LINKS; m++) {
                t->q_fract[c][k][m][0] = (float)cos(-FA_PI * q_link[m] * f);
                t->q_fract[c][k][m][1] = (float)sin(-FA_PI * q_link[m] * f);
            }
        }
    }
    make_filter(t->f8, fa_aac_ps_p8_13_20, 8);
    make_filter(t->f12, fa_aac_ps_p12_13_34, 12);
    make_filter(t->f8_34, fa_aac_ps_p8_13_34, 8);
    make_filter(t->f4, fa_aac_ps_p4_13_34, 4);
    for (i = 0; i < 512; i++) {
        /* Phase smoothing: 0.25, 0.5 and 1 times the last three phases. */
        double a0 = FA_PI / 4 * (i >> 6), a1 = FA_PI / 4 * ((i >> 3) & 7), a2 = FA_PI / 4 * (i & 7);
        double re = 0.25 * cos(a0) + 0.5 * cos(a1) + cos(a2), im = 0.25 * sin(a0) + 0.5 * sin(a1) + sin(a2);
        double mag = sqrt(re * re + im * im);
        t->pd_smooth[i][0] = (float)(mag > 0 ? re / mag : 1.0);
        t->pd_smooth[i][1] = (float)(mag > 0 ? im / mag : 0.0);
    }
    fa_store32(&g_tables_state, 2);
}

fa_aac_ps* fa_aac_ps_create(void)
{
    fa_aac_ps* p = (fa_aac_ps*)calloc(1, sizeof(*p));
    int c;
    if (!p) return NULL;
    ps_tables_init();
    p->t = &g_tables;
    for (c = 0; c < 2; c++) {
        ps_config* cf = &p->cfg[c];
        cf->nbands = c ? 91 : 71;
        cf->nhybrid = c ? 32 : 10;
        cf->qsplit = c ? 5 : 3;
        cf->npar = c ? 34 : 20;
        cf->allpass_bands = c ? 50 : 30;
        cf->short_delay_band = c ? 62 : 42;
        cf->decay_cutoff = c ? 32 : 10;
        cf->nipd = c ? 17 : 11;
        cf->k_to_i = c ? k_to_i_34 : k_to_i_20;
    }
    fa_aac_ps_reset(p);
    return p;
}

void fa_aac_ps_destroy(fa_aac_ps* p)
{
    free(p);
}

void fa_aac_ps_reset(fa_aac_ps* p)
{
    memset(p->in_hist, 0, sizeof(p->in_hist));
    memset(p->delay, 0, sizeof(p->delay));
    memset(p->ap_delay, 0, sizeof(p->ap_delay));
    memset(p->peak_decay, 0, sizeof(p->peak_decay));
    memset(p->power_smooth, 0, sizeof(p->power_smooth));
    memset(p->peak_diff_smooth, 0, sizeof(p->peak_diff_smooth));
    memset(p->h_last, 0, sizeof(p->h_last));
    memset(p->ipd_hist, 0, sizeof(p->ipd_hist));
    memset(p->opd_hist, 0, sizeof(p->opd_hist));
    memset(p->iid_last, 0, sizeof(p->iid_last));
    memset(p->icc_last, 0, sizeof(p->icc_last));
    memset(p->ipd_last, 0, sizeof(p->ipd_last));
    memset(p->opd_last, 0, sizeof(p->opd_last));
    p->num_env = 0;
}

/* ---- ps_data() ------------------------------------------------------------------------ */

static int read_par(fa_aac_bits* b, int8_t* par, const int8_t* prev, int n, int dt, const uint16_t* tf,
                    const uint16_t* tt, int range, int mask)
{
    int i, v = 0;
    for (i = 0; i < n; i++) {
        int d = fa_aac_vlc(b, dt ? tt : tf, FA_AAC_VLC_ROOT);
        if (d < 0) return 0;
        d -= 128;
        if (dt) v = prev[i] + d;
        else v = (i ? v : 0) + d;
        if (mask) v &= mask;
        else if (v < -range || v > range) return 0;
        par[i] = (int8_t)v;
    }
    return 1;
}

int fa_aac_ps_parse(fa_aac_ps* p, fa_aac_bits* b, int bits_left)
{
    size_t start = b->pos;
    int frame_class, e, nr_iid = 0, nr_icc = 0, nr_ipd = 0, fine;
    if (fa_aac_get(b, 1)) {
        p->header_seen = 1;
        p->enable_iid = (int)fa_aac_get(b, 1);
        if (p->enable_iid) {
            p->iid_mode = (int)fa_aac_get(b, 3);
            if (p->iid_mode > 5) return -1;
        }
        p->enable_icc = (int)fa_aac_get(b, 1);
        if (p->enable_icc) {
            p->icc_mode = (int)fa_aac_get(b, 3);
            if (p->icc_mode > 5) return -1;
        }
        p->enable_ext = (int)fa_aac_get(b, 1);
    }
    if (!p->header_seen) return -1;
    frame_class = (int)fa_aac_get(b, 1);
    p->num_env = num_env_tab[frame_class][fa_aac_get(b, 2)];
    p->border[0] = -1;
    if (frame_class) {
        for (e = 1; e <= p->num_env; e++) p->border[e] = (int)fa_aac_get(b, 5);
    } else {
        for (e = 1; e <= p->num_env; e++) p->border[e] = e * SLOTS / p->num_env - 1;
    }
    if (p->enable_iid) {
        nr_iid = nr_par_tab[p->iid_mode];
        fine = p->iid_mode > 2;
        for (e = 0; e < p->num_env; e++) {
            int dt = (int)fa_aac_get(b, 1);
            const int8_t* prev = e ? p->iid[e - 1] : p->iid_last;
            if (!read_par(b, p->iid[e], prev, nr_iid, dt, fine ? fa_aac_ps_huff_iid_fine_f : fa_aac_ps_huff_iid_def_f,
                          fine ? fa_aac_ps_huff_iid_fine_t : fa_aac_ps_huff_iid_def_t, fine ? 15 : 7, 0))
                return -1;
        }
    }
    if (p->enable_icc) {
        nr_icc = nr_par_tab[p->icc_mode];
        for (e = 0; e < p->num_env; e++) {
            int dt = (int)fa_aac_get(b, 1);
            const int8_t* prev = e ? p->icc[e - 1] : p->icc_last;
            if (!read_par(b, p->icc[e], prev, nr_icc, dt, fa_aac_ps_huff_icc_f, fa_aac_ps_huff_icc_t, 7, 0)) return -1;
            for (fine = 0; fine < nr_icc; fine++)
                if (p->icc[e][fine] < 0) return -1;
        }
    }
    p->enable_ipdopd = 0;
    if (p->enable_ext) {
        int cnt = (int)fa_aac_get(b, 4), left;
        if (cnt == 15) cnt += (int)fa_aac_get(b, 8);
        left = cnt * 8;
        while (left > 7) {
            int id = (int)fa_aac_get(b, 2);
            size_t ext_start = b->pos;
            left -= 2;
            if (id == 0) {
                p->enable_ipdopd = (int)fa_aac_get(b, 1);
                if (p->enable_ipdopd) {
                    nr_ipd = nr_ipd_tab[p->iid_mode];
                    for (e = 0; e < p->num_env; e++) {
                        int dt = (int)fa_aac_get(b, 1);
                        if (!read_par(b, p->ipd[e], e ? p->ipd[e - 1] : p->ipd_last, nr_ipd, dt, fa_aac_ps_huff_ipd_f,
                                      fa_aac_ps_huff_ipd_t, 0, 7))
                            return -1;
                        dt = (int)fa_aac_get(b, 1);
                        if (!read_par(b, p->opd[e], e ? p->opd[e - 1] : p->opd_last, nr_ipd, dt, fa_aac_ps_huff_opd_f,
                                      fa_aac_ps_huff_opd_t, 0, 7))
                            return -1;
                    }
                }
                fa_aac_skip(b, 1);                   /* reserved_ps */
            }
            left -= (int)(b->pos - ext_start);
            if (left < 0) return -1;
            if (id != 0) {
                fa_aac_skip(b, (size_t)left);
                left = 0;
            }
        }
        fa_aac_skip(b, (size_t)(left > 0 ? left : 0));
    }
    if ((int)(b->pos - start) > bits_left) return -1;
    /* Without parameters, the last ones hold over the frame. */
    if (p->num_env == 0) {
        p->num_env = 1;
        p->border[1] = SLOTS - 1;
        memcpy(p->iid[0], p->iid_last, sizeof(p->iid_last));
        memcpy(p->icc[0], p->icc_last, sizeof(p->icc_last));
        memcpy(p->ipd[0], p->ipd_last, sizeof(p->ipd_last));
        memcpy(p->opd[0], p->opd_last, sizeof(p->opd_last));
    }
    for (e = 1; e <= p->num_env; e++) {
        if (p->border[e] <= p->border[e - 1]) p->border[e] = p->border[e - 1] + 1;
        if (p->border[e] > SLOTS - 1) p->border[e] = SLOTS - 1;
    }
    /* An envelope that ends before the frame does is held to the end. */
    if (p->border[p->num_env] < SLOTS - 1 && p->num_env < MAX_ENV) {
        int n = p->num_env;
        memcpy(p->iid[n], p->iid[n - 1], sizeof(p->iid[0]));
        memcpy(p->icc[n], p->icc[n - 1], sizeof(p->icc[0]));
        memcpy(p->ipd[n], p->ipd[n - 1], sizeof(p->ipd[0]));
        memcpy(p->opd[n], p->opd[n - 1], sizeof(p->opd[0]));
        p->num_env++;
        p->border[p->num_env] = SLOTS - 1;
    }
    if (!p->enable_iid) memset(p->iid, 0, sizeof(p->iid));
    if (!p->enable_icc) memset(p->icc, 0, sizeof(p->icc));
    if (!p->enable_ipdopd) {
        memset(p->ipd, 0, sizeof(p->ipd));
        memset(p->opd, 0, sizeof(p->opd));
    }
    if (p->enable_iid) {
        memcpy(p->iid_last, p->iid[p->num_env - 1], sizeof(p->iid_last));
        p->nr_iid_last = nr_iid;
    }
    if (p->enable_icc) {
        memcpy(p->icc_last, p->icc[p->num_env - 1], sizeof(p->icc_last));
        p->nr_icc_last = nr_icc;
    }
    if (p->enable_ipdopd) {
        memcpy(p->ipd_last, p->ipd[p->num_env - 1], sizeof(p->ipd_last));
        memcpy(p->opd_last, p->opd[p->num_env - 1], sizeof(p->opd_last));
        p->nr_ipd_last = nr_ipd;
    }
    p->is34 = (p->enable_iid && nr_iid == 34) || (p->enable_icc && nr_icc == 34);
    return (int)(b->pos - start);
}

/* ---- parameter mapping between band resolutions ------------------------------------------ */

static void map_10_to_20(int8_t* out, const int8_t* in, int full)
{
    int b = full ? 9 : 4;
    if (!full) out[10] = 0;
    for (; b >= 0; b--) out[2 * b + 1] = out[2 * b] = in[b];
}

static void map_34_to_20(int8_t* o, const int8_t* p, int full)
{
    o[0] = (int8_t)((2 * p[0] + p[1]) / 3);
    o[1] = (int8_t)((p[1] + 2 * p[2]) / 3);
    o[2] = (int8_t)((2 * p[3] + p[4]) / 3);
    o[3] = (int8_t)((p[4] + 2 * p[5]) / 3);
    o[4] = (int8_t)((p[6] + p[7]) / 2);
    o[5] = (int8_t)((p[8] + p[9]) / 2);
    o[6] = p[10];
    o[7] = p[11];
    o[8] = (int8_t)((p[12] + p[13]) / 2);
    o[9] = (int8_t)((p[14] + p[15]) / 2);
    o[10] = p[16];
    if (full) {
        o[11] = p[17];
        o[12] = p[18];
        o[13] = p[19];
        o[14] = (int8_t)((p[20] + p[21]) / 2);
        o[15] = (int8_t)((p[22] + p[23]) / 2);
        o[16] = (int8_t)((p[24] + p[25]) / 2);
        o[17] = (int8_t)((p[26] + p[27]) / 2);
        o[18] = (int8_t)((p[28] + p[29] + p[30] + p[31]) / 4);
        o[19] = (int8_t)((p[32] + p[33]) / 2);
    }
}

static void map_20_to_34(int8_t* o, const int8_t* p, int full)
{
    static const int8_t src[34] = { 0, -1, 1, 2, -2, 3, 4, 4, 5, 5, 6, 7, 8, 8, 9, 9, 10,
                                    11, 12, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18, 18, 18, 19, 19 };
    int i, n = full ? 34 : 17;
    for (i = 0; i < n; i++) {
        if (src[i] == -1) o[i] = (int8_t)((p[0] + p[1]) / 2);
        else if (src[i] == -2) o[i] = (int8_t)((p[2] + p[3]) / 2);
        else o[i] = p[src[i]];
    }
}

/* The parameters of envelope e in the current band configuration. */
static void mapped(const fa_aac_ps* p, const int8_t* in, int n, int8_t* out, int full)
{
    if (p->is34) {
        if (n == 34 || (!full && n == 17)) memcpy(out, in, (size_t)n);
        else if (n == 20 || n == 11) map_20_to_34(out, in, full);
        else {
            int8_t t[20];
            map_10_to_20(t, in, full);
            map_20_to_34(out, t, full);
        }
    } else {
        if (n == 34 || n == 17) map_34_to_20(out, in, full);
        else if (n == 10 || n == 5) map_10_to_20(out, in, full);
        else memcpy(out, in, (size_t)n);
    }
}

/* ---- hybrid filterbank ------------------------------------------------------------------ */

/* y[n] = sum_j f[j] x[n + j] over 13 taps, for n in [0, 32). */
static void hybrid_complex(const float (*x)[2], const float (*f)[13][2], int bands, float (*out[])[SLOTS][2])
{
    int q, n, j;
    for (q = 0; q < bands; q++) {
        for (n = 0; n < SLOTS; n++) {
            float re = 0.0f, im = 0.0f;
            for (j = 0; j < 13; j++) {
                re += f[q][j][0] * x[n + j][0] - f[q][j][1] * x[n + j][1];
                im += f[q][j][0] * x[n + j][1] + f[q][j][1] * x[n + j][0];
            }
            (*out[q])[n][0] = re;
            (*out[q])[n][1] = im;
        }
    }
}

static void hybrid_analysis(fa_aac_ps* p, float (*l)[64][2])
{
    const ps_config* cf = &p->cfg[p->is34];
    int k, n;
    for (k = 0; k < cf->qsplit; k++) {
        memcpy(p->hybrid_in[k], p->in_hist[k], sizeof(p->in_hist[k]));
        for (n = 0; n < 38; n++) {
            p->hybrid_in[k][6 + n][0] = l[n][k][0];
            p->hybrid_in[k][6 + n][1] = l[n][k][1];
        }
        memcpy(p->in_hist[k], p->hybrid_in[k] + SLOTS, sizeof(p->in_hist[k]));
    }
    if (!p->is34) {
        float tmp[8][SLOTS][2];
        float (*o8[8])[SLOTS][2];
        int q;
        for (q = 0; q < 8; q++) o8[q] = &tmp[q];
        hybrid_complex((const float (*)[2])p->hybrid_in[0], (const float (*)[13][2])p->t->f8, 8, o8);
        for (n = 0; n < SLOTS; n++) {
            int c;
            for (c = 0; c < 2; c++) {
                p->s[0][n][c] = tmp[6][n][c];
                p->s[1][n][c] = tmp[7][n][c];
                p->s[2][n][c] = tmp[0][n][c];
                p->s[3][n][c] = tmp[1][n][c];
                p->s[4][n][c] = tmp[2][n][c] + tmp[5][n][c];
                p->s[5][n][c] = tmp[3][n][c] + tmp[4][n][c];
            }
        }
        /* QMF bands 1 and 2: two real half-band filters (band 1 is spectrally reversed). */
        for (k = 1; k <= 2; k++) {
            const float (*x)[2] = (const float (*)[2])p->hybrid_in[k];
            const float* g = fa_aac_ps_p2_13_20;
            int lo = k == 1 ? 7 : 8, hi = k == 1 ? 6 : 9;
            for (n = 0; n < SLOTS; n++) {
                int c;
                for (c = 0; c < 2; c++) {
                    float mid = g[6] * x[n + 6][c], side = 0.0f;
                    int j;
                    for (j = 1; j < 6; j += 2) side += g[j] * (x[n + j][c] + x[n + 12 - j][c]);
                    p->s[lo][n][c] = mid + side;
                    p->s[hi][n][c] = mid - side;
                }
            }
        }
    } else {
        float (*o[12])[SLOTS][2];
        int q;
        for (q = 0; q < 12; q++) o[q] = &p->s[q];
        hybrid_complex((const float (*)[2])p->hybrid_in[0], (const float (*)[13][2])p->t->f12, 12, o);
        for (q = 0; q < 8; q++) o[q] = &p->s[12 + q];
        hybrid_complex((const float (*)[2])p->hybrid_in[1], (const float (*)[13][2])p->t->f8_34, 8, o);
        for (k = 2; k < 5; k++) {
            for (q = 0; q < 4; q++) o[q] = &p->s[20 + 4 * (k - 2) + q];
            hybrid_complex((const float (*)[2])p->hybrid_in[k], (const float (*)[13][2])p->t->f4, 4, o);
        }
    }
    /* The QMF bands above, as they are. */
    for (k = cf->qsplit; k < 64; k++) {
        int b = cf->nhybrid + k - cf->qsplit;
        for (n = 0; n < SLOTS; n++) {
            p->s[b][n][0] = l[n][k][0];
            p->s[b][n][1] = l[n][k][1];
        }
    }
}

/* Sums the hybrid bands back into QMF bands: out holds 32 slots of 64 bands. */
static void hybrid_synthesis(const fa_aac_ps* p, float (*x)[MAX_BANDS][SLOTS][2], float (*out)[64][2])
{
    const ps_config* cf = &p->cfg[p->is34];
    static const int split20[3] = { 6, 2, 2 }, split34[5] = { 12, 8, 4, 4, 4 };
    const int* split = p->is34 ? split34 : split20;
    int k, n, b = 0, j;
    for (k = 0; k < cf->qsplit; k++) {
        for (n = 0; n < SLOTS; n++) {
            float re = 0.0f, im = 0.0f;
            for (j = 0; j < split[k]; j++) {
                re += (*x)[b + j][n][0];
                im += (*x)[b + j][n][1];
            }
            out[n][k][0] = re;
            out[n][k][1] = im;
        }
        b += split[k];
    }
    for (k = cf->qsplit; k < 64; k++, b++)
        for (n = 0; n < SLOTS; n++) {
            out[n][k][0] = (*x)[b][n][0];
            out[n][k][1] = (*x)[b][n][1];
        }
}

/* ---- decorrelation ---------------------------------------------------------------------- */

static void decorrelate(fa_aac_ps* p, int top)
{
    static const float a_link[AP_LINKS] = { 0.65143905753106f, 0.56471812200776f, 0.48954165955695f };
    static const int d_link[AP_LINKS] = { 3, 4, 5 };
    const ps_config* cf = &p->cfg[p->is34];
    float power[MAX_PAR][SLOTS], gain[MAX_PAR][SLOTS];
    int k, n, i, m;
    /* Transient ducking: from the input power of each parameter band. */
    memset(power, 0, sizeof(power));
    for (k = 0; k < cf->nbands; k++) {
        i = cf->k_to_i[k];
        for (n = 0; n < SLOTS; n++) power[i][n] += p->s[k][n][0] * p->s[k][n][0] + p->s[k][n][1] * p->s[k][n][1];
    }
    for (i = 0; i < cf->npar; i++) {
        for (n = 0; n < SLOTS; n++) {
            float decayed = 0.76592833836465f * p->peak_decay[i], denom;
            p->peak_decay[i] = decayed > power[i][n] ? decayed : power[i][n];
            p->power_smooth[i] += 0.25f * (power[i][n] - p->power_smooth[i]);
            p->peak_diff_smooth[i] += 0.25f * (p->peak_decay[i] - power[i][n] - p->peak_diff_smooth[i]);
            denom = 1.5f * p->peak_diff_smooth[i];
            gain[i][n] = denom > p->power_smooth[i] ? p->power_smooth[i] / denom : 1.0f;
        }
    }
    /* Bands with no signal: their filters are emptied. */
    {
        int top_band = top - cf->qsplit + cf->nhybrid;
        if (top_band < cf->nhybrid) top_band = cf->nhybrid;
        for (k = top_band; k < cf->nbands; k++) {
            memset(p->delay[k], 0, sizeof(p->delay[k]));
            memset(p->ap_delay[k], 0, sizeof(p->ap_delay[k]));
        }
    }
    for (k = 0; k < cf->nbands; k++) {
        float (*dl)[2] = p->dl;
        i = cf->k_to_i[k];
        /* The input goes into the delay line after MAX_DELAY slots of history. */
        memcpy(dl, p->delay[k], sizeof(p->delay[k]));
        memcpy(dl + MAX_DELAY, p->s[k], sizeof(float) * 2 * SLOTS);
        memcpy(p->delay[k], dl + SLOTS, sizeof(p->delay[k]));
        if (k < cf->allpass_bands) {
            float slope = k > cf->decay_cutoff ? 1.0f - 0.05f * (float)(k - cf->decay_cutoff) : 1.0f;
            float ag[AP_LINKS];
            const float* phi = p->t->phi_fract[p->is34][k];
            if (slope < 0.0f) slope = 0.0f;
            for (m = 0; m < AP_LINKS; m++) {
                ag[m] = a_link[m] * slope;
                memcpy(p->ap[m], p->ap_delay[k][m], sizeof(p->ap_delay[k][m]));
            }
            for (n = 0; n < SLOTS; n++) {
                /* z^-2 and the fractional delay */
                const float* x = dl[MAX_DELAY + n - 2];
                float re = x[0] * phi[0] - x[1] * phi[1];
                float im = x[0] * phi[1] + x[1] * phi[0];
                for (m = 0; m < AP_LINKS; m++) {
                    const float* q = p->t->q_fract[p->is34][k][m];
                    const float* w = p->ap[m][MAX_AP_DELAY + n - d_link[m]];
                    float ar = w[0] * q[0] - w[1] * q[1] - ag[m] * re;
                    float ai = w[0] * q[1] + w[1] * q[0] - ag[m] * im;
                    p->ap[m][MAX_AP_DELAY + n][0] = re + ag[m] * ar;
                    p->ap[m][MAX_AP_DELAY + n][1] = im + ag[m] * ai;
                    re = ar;
                    im = ai;
                }
                p->d[k][n][0] = re * gain[i][n];
                p->d[k][n][1] = im * gain[i][n];
            }
            for (m = 0; m < AP_LINKS; m++) memcpy(p->ap_delay[k][m], p->ap[m] + SLOTS, sizeof(p->ap_delay[k][m]));
        } else {
            int delay = k < cf->short_delay_band ? MAX_DELAY : 1;
            for (n = 0; n < SLOTS; n++) {
                p->d[k][n][0] = dl[MAX_DELAY + n - delay][0] * gain[i][n];
                p->d[k][n][1] = dl[MAX_DELAY + n - delay][1] * gain[i][n];
            }
        }
    }
}

/* ---- mixing ------------------------------------------------------------------------------ */

static void mixing_matrix(const fa_aac_ps* p, int iid, int icc, float h[4])
{
    int fine = p->iid_mode > 2;
    float db = fine ? iid_fine_db[iid + 15] : iid_default_db[iid + 7];
    float c = powf(10.0f, db / 20.0f), rho = icc_rho[icc];
    if (p->icc_mode < 3) {
        /* procedure R_A */
        float c1 = 1.41421356f / sqrtf(1.0f + c * c), c2 = c * c1;
        float alpha = 0.5f * acosf(rho), beta = alpha * (c1 - c2) * 0.70710678f;
        h[0] = c2 * cosf(beta + alpha);
        h[1] = c1 * cosf(beta - alpha);
        h[2] = c2 * sinf(beta + alpha);
        h[3] = c1 * sinf(beta - alpha);
    } else {
        /* procedure R_B */
        float r = rho > 0.05f ? rho : 0.05f, alpha, mu, gamma;
        if (fabsf(c * c - 1.0f) < 1e-9f) alpha = (float)(FA_PI / 4);
        else alpha = 0.5f * atanf(2.0f * c * r / (c * c - 1.0f));
        mu = c + 1.0f / c;
        mu = sqrtf(1.0f + (4.0f * r * r - 4.0f) / (mu * mu));
        gamma = atanf(sqrtf((1.0f - mu) / (1.0f + mu)));
        if (alpha < 0.0f) alpha += (float)(FA_PI / 2);
        h[0] = 1.41421356f * cosf(alpha) * cosf(gamma);
        h[1] = 1.41421356f * sinf(alpha) * cosf(gamma);
        h[2] = -1.41421356f * sinf(alpha) * sinf(gamma);
        h[3] = 1.41421356f * cosf(alpha) * sinf(gamma);
    }
}

static void stereo(fa_aac_ps* p)
{
    const ps_config* cf = &p->cfg[p->is34];
    float h[MAX_ENV + 1][4][MAX_PAR][2];
    int e, b, k, n, j, npar = cf->npar;
    int ipd = p->enable_ipdopd;
    for (j = 0; j < 4; j++)
        for (b = 0; b < npar; b++) {
            h[0][j][b][0] = p->h_last[j][b][0];
            h[0][j][b][1] = p->h_last[j][b][1];
        }
    for (e = 0; e < p->num_env; e++) {
        int8_t iid[MAX_PAR], icc[MAX_PAR], ipdp[MAX_PAR], opdp[MAX_PAR];
        mapped(p, p->iid[e], p->nr_iid_last ? p->nr_iid_last : 20, iid, 1);
        mapped(p, p->icc[e], p->nr_icc_last ? p->nr_icc_last : 20, icc, 1);
        if (ipd) {
            mapped(p, p->ipd[e], p->nr_ipd_last, ipdp, 0);
            mapped(p, p->opd[e], p->nr_ipd_last, opdp, 0);
        }
        for (b = 0; b < npar; b++) {
            float m[4];
            mixing_matrix(p, p->enable_iid ? iid[b] : 0, p->enable_icc ? icc[b] : 0, m);
            for (j = 0; j < 4; j++) {
                h[e + 1][j][b][0] = m[j];
                h[e + 1][j][b][1] = 0.0f;
            }
            if (ipd && b < cf->nipd) {
                int oi = p->opd_hist[b] * 8 + (opdp[b] & 7), ii = p->ipd_hist[b] * 8 + (ipdp[b] & 7);
                float o_re = p->t->pd_smooth[oi][0], o_im = p->t->pd_smooth[oi][1];
                float i_re = p->t->pd_smooth[ii][0], i_im = p->t->pd_smooth[ii][1];
                float a_re = o_re * i_re + o_im * i_im, a_im = o_im * i_re - o_re * i_im;   /* OPD - IPD */
                p->opd_hist[b] = (uint8_t)(oi & 0x3F);
                p->ipd_hist[b] = (uint8_t)(ii & 0x3F);
                h[e + 1][0][b][0] = m[0] * o_re;
                h[e + 1][0][b][1] = m[0] * o_im;
                h[e + 1][1][b][0] = m[1] * a_re;
                h[e + 1][1][b][1] = m[1] * a_im;
                h[e + 1][2][b][0] = m[2] * o_re;
                h[e + 1][2][b][1] = m[2] * o_im;
                h[e + 1][3][b][0] = m[3] * a_re;
                h[e + 1][3][b][1] = m[3] * a_im;
            }
        }
    }
    for (j = 0; j < 4; j++)
        for (b = 0; b < npar; b++) {
            p->h_last[j][b][0] = h[p->num_env][j][b][0];
            p->h_last[j][b][1] = h[p->num_env][j][b][1];
        }
    /* Interpolated across each envelope, ending on its own matrix. */
    for (k = 0; k < cf->nbands; k++) {
        int i = cf->k_to_i[k];
        /* Hybrid bands of negative frequency take the conjugate phase along the
         * whole interpolation, as in FAAD2 (FFmpeg conjugates only its start
         * point, and two more bands in 34-band mode). */
        int neg = ipd && (p->is34 ? (k >= 9 && k <= 11) : k <= 1);
        for (e = 0; e < p->num_env; e++) {
            int start = p->border[e], stop = p->border[e + 1];
            float width = 1.0f / (float)(stop - start > 0 ? stop - start : 1);
            float hh[4][2], st[4][2];
            for (j = 0; j < 4; j++) {
                hh[j][0] = h[e][j][i][0];
                hh[j][1] = neg ? -h[e][j][i][1] : h[e][j][i][1];
                st[j][0] = (h[e + 1][j][i][0] - h[e][j][i][0]) * width;
                st[j][1] = ((neg ? -h[e + 1][j][i][1] : h[e + 1][j][i][1]) - hh[j][1]) * width;
            }
            for (n = start + 1; n <= stop; n++) {
                float* s = p->s[k][n];
                float* d = p->d[k][n];
                float sr = s[0], si = s[1], dr = d[0], di = d[1];
                for (j = 0; j < 4; j++) {
                    hh[j][0] += st[j][0];
                    hh[j][1] += st[j][1];
                }
                /* left = h11 s + h21 d, right = h12 s + h22 d */
                s[0] = hh[0][0] * sr - hh[0][1] * si + hh[2][0] * dr - hh[2][1] * di;
                s[1] = hh[0][0] * si + hh[0][1] * sr + hh[2][0] * di + hh[2][1] * dr;
                d[0] = hh[1][0] * sr - hh[1][1] * si + hh[3][0] * dr - hh[3][1] * di;
                d[1] = hh[1][0] * si + hh[1][1] * sr + hh[3][0] * di + hh[3][1] * dr;
            }
        }
    }
}

void fa_aac_ps_apply(fa_aac_ps* p, float (*l)[64][2], float (*r)[64][2], int new_data, int kx)
{
    int top = 64;
    (void)kx;
    if (!new_data) {
        /* No parameters this frame: the last ones over the whole frame. */
        p->num_env = 1;
        p->border[0] = -1;
        p->border[1] = SLOTS - 1;
        memcpy(p->iid[0], p->iid_last, sizeof(p->iid_last));
        memcpy(p->icc[0], p->icc_last, sizeof(p->icc_last));
        memcpy(p->ipd[0], p->ipd_last, sizeof(p->ipd_last));
        memcpy(p->opd[0], p->opd_last, sizeof(p->opd_last));
    }
    if (p->is34 != p->is34_last) {
        memset(p->peak_decay, 0, sizeof(p->peak_decay));
        memset(p->power_smooth, 0, sizeof(p->power_smooth));
        memset(p->peak_diff_smooth, 0, sizeof(p->peak_diff_smooth));
        memset(p->delay, 0, sizeof(p->delay));
        memset(p->ap_delay, 0, sizeof(p->ap_delay));
        p->is34_last = p->is34;
    }
    hybrid_analysis(p, l);
    decorrelate(p, top);
    stereo(p);
    hybrid_synthesis(p, &p->s, l);
    hybrid_synthesis(p, &p->d, r);
}
