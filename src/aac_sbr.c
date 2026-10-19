/* Spectral band replication (HE-AAC), written from ISO/IEC 14496-3 subpart 4,
 * 4.6.18: the bitstream (sbr_header, sbr_data and its grid, envelope and noise
 * floor data), the frequency band tables, the QMF analysis (32 bands) and
 * synthesis (64 bands) banks, HF generation by patching with inverse filtering,
 * and HF adjustment (envelope gains, limiter, noise floor and added sinusoids).
 * Parametric stereo (aac_ps.c) runs on this file's QMF matrix. The core decoder
 * calls fa_aac_sbr_apply on every block of an element once SBR is on, also when a
 * block carries no SBR data (the high band is then left empty). */
#include "aac.h"
#include "mdct.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef FA_AAC_TRACE
#include <stdio.h>
#endif

#define SLOTS 32           /* QMF slots per frame (numTimeSlots 16 * RATE 2) */
#define HFGEN 8            /* t_HFGen: slots of low band history */
#define HFADJ 2            /* t_HFAdj */
#define XSLOTS 38          /* slots of the QMF matrix handed on: 32 plus 6 of low band lookahead */
#define MAX_ENV 5
#define MAX_NQ 5
#define MAX_PATCH 6
#define MAX_LIM 32

enum { FIXFIX = 0, FIXVAR, VARFIX, VARVAR };

typedef struct sbr_header {
    int amp_res, start_freq, stop_freq, xover_band;
    int freq_scale, alter_scale, noise_bands;
    int limiter_bands, limiter_gains, interpol_freq, smoothing_mode;
} sbr_header;

typedef struct sbr_chan {
    /* this frame's data */
    int frame_class, L_E, L_Q, pointer, l_A, amp_res;
    int t_E[MAX_ENV + 1], t_Q[3];
    int freq_res[MAX_ENV];
    int df_env[MAX_ENV], df_noise[2];
    int invf[MAX_NQ];
    int E[MAX_ENV][64];
    int Q[2][MAX_NQ];
    uint8_t add_harmonic[64];
    float E_orig[MAX_ENV][64];          /* per band of the envelope's resolution */
    float Q_orig[2][MAX_NQ];
    /* from the previous frame */
    int E_last[64], freq_res_last, Q_last[MAX_NQ];
    int L_E_last, l_A_last, t_last;     /* t_last: t_E[L_E] of the previous frame */
    int invf_last[MAX_NQ];
    float bw_last[MAX_NQ];
    uint8_t harmonic_last[64];          /* S_IndexMapped of the last envelope, per subband */
    int have_last;
    /* signal */
    float ana[320];                     /* analysis delay line, newest first */
    float syn[1280];                    /* synthesis delay line v */
    float xlow[SLOTS + HFGEN][32][2];   /* low band: 8 slots of history, then this frame */
    float y[2][SLOTS + 8][64][2];       /* high band, this frame [1] and the previous [0] */
    float g_hist[5][64], q_hist[5][64]; /* smoothing history: last 4 slots plus current */
    int hist_valid;
    int index_noise, index_sine;
} sbr_chan;

struct fa_aac_sbr {
    uint32_t rate;                      /* SBR (output) rate */
    int nch;
    int header_valid, data_valid, coupling;
    sbr_header h;
    /* frequency tables */
    int tables_ok;
    int k0, k2, kx, M, N_master, N_high, N_low, N_Q, N_L;
    int f_master[65], f_high[65], f_low[65], f_noise[MAX_NQ + 1], f_lim[MAX_LIM + 1];
    int num_patches, patch_n[MAX_PATCH], patch_start[MAX_PATCH];
    int kx_last, M_last;
    sbr_chan ch[2];
    float x[2][XSLOTS][64][2];          /* the QMF matrix of each output channel */
    float xhigh[SLOTS + 8][64][2];      /* HF generator output, one channel at a time */
    const fa_aac_qmf* qmf;              /* transform tables, shared by the decoder's elements */
    fa_aac_ps* ps;
    int ps_seen;
    int ps_data;                        /* PS data came with this frame */
};

/* ---- setup ------------------------------------------------------------------------------- */

struct fa_aac_qmf {
    fa_mdct dct4;                       /* 64 points, for the analysis and the synthesis */
};

fa_aac_qmf* fa_aac_qmf_create(void)
{
    fa_aac_qmf* q = (fa_aac_qmf*)malloc(sizeof(*q));
    if (!q) return NULL;
    if (fa_imdct_init(&q->dct4, 64) != FA_OK) {
        free(q);
        return NULL;
    }
    return q;
}

void fa_aac_qmf_destroy(fa_aac_qmf* q)
{
    if (!q) return;
    fa_imdct_free(&q->dct4);
    free(q);
}

fa_aac_sbr* fa_aac_sbr_create(uint32_t rate, int channels, const fa_aac_qmf* qmf)
{
    fa_aac_sbr* s = (fa_aac_sbr*)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->rate = rate * 2;
    s->nch = channels;
    s->qmf = qmf;
    fa_aac_sbr_reset(s);
    return s;
}

void fa_aac_sbr_destroy(fa_aac_sbr* s)
{
    if (!s) return;
    fa_aac_ps_destroy(s->ps);
    free(s);
}

void fa_aac_sbr_reset(fa_aac_sbr* s)
{
    int c;
    for (c = 0; c < 2; c++) {
        sbr_chan* ch = &s->ch[c];
        memset(ch->ana, 0, sizeof(ch->ana));
        memset(ch->syn, 0, sizeof(ch->syn));
        memset(ch->xlow, 0, sizeof(ch->xlow));
        memset(ch->y, 0, sizeof(ch->y));
        ch->have_last = 0;
        ch->hist_valid = 0;
        ch->t_last = 16;
        ch->L_E_last = 0;
        ch->l_A_last = -1;
        memset(ch->bw_last, 0, sizeof(ch->bw_last));
        memset(ch->invf_last, 0, sizeof(ch->invf_last));
        memset(ch->harmonic_last, 0, sizeof(ch->harmonic_last));
        ch->index_noise = 0;
        ch->index_sine = 0;
    }
    s->data_valid = 0;
    s->kx_last = s->tables_ok ? s->kx : 32;    /* no SBR data yet: the whole low band passes */
    s->M_last = 0;
    if (s->ps) fa_aac_ps_reset(s->ps);
}

int fa_aac_sbr_has_ps(const fa_aac_sbr* s)
{
    return s && s->ps_seen;
}

/* ---- frequency band tables (4.6.18.3) ---------------------------------------------------- */

static int cmp_int(const void* a, const void* b)
{
    return *(const int*)a - *(const int*)b;
}

static int nint(double x)
{
    return (int)floor(x + 0.5);
}

/* Band widths of a geometric split of [a, b) into n bands, sorted. */
static void geometric(int a, int b, int n, int* dk)
{
    int k;
    for (k = 0; k < n; k++)
        dk[k] = nint(a * pow((double)b / a, (k + 1.0) / n)) - nint(a * pow((double)b / a, (double)k / n));
    qsort(dk, (size_t)n, sizeof(int), cmp_int);
}

static int build_master(fa_aac_sbr* s)
{
    static const int8_t offsets[7][16] = {
        { -8, -7, -6, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 6, 7 },
        { -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 6, 7, 9, 11, 13 },
        { -5, -3, -2, -1, 0, 1, 2, 3, 4, 5, 6, 7, 9, 11, 13, 16 },
        { -6, -4, -2, -1, 0, 1, 2, 3, 4, 5, 6, 7, 9, 11, 13, 16 },
        { -4, -2, -1, 0, 1, 2, 3, 4, 5, 6, 7, 9, 11, 13, 16, 20 },
        { -2, -1, 0, 1, 2, 3, 4, 5, 6, 7, 9, 11, 13, 16, 20, 24 } };
    const sbr_header* h = &s->h;
    uint32_t fs = s->rate;
    int start_min, stop_min, row, k, max_bands;
    int dk[64];
    if (fs < 32000) {
        start_min = nint(3000.0 * 128.0 / fs);
        stop_min = nint(6000.0 * 128.0 / fs);
    } else if (fs < 64000) {
        start_min = nint(4000.0 * 128.0 / fs);
        stop_min = nint(8000.0 * 128.0 / fs);
    } else {
        start_min = nint(5000.0 * 128.0 / fs);
        stop_min = nint(10000.0 * 128.0 / fs);
    }
    row = fs <= 16000 ? 0 : fs <= 22050 ? 1 : fs <= 24000 ? 2 : fs <= 32000 ? 3 : fs <= 64000 ? 4 : 5;
    s->k0 = start_min + offsets[row][h->start_freq];
    if (h->stop_freq == 15) {
        s->k2 = 3 * s->k0;
    } else if (h->stop_freq == 14) {
        s->k2 = 2 * s->k0;
    } else {
        int sum = 0;
        geometric(stop_min, 64, 13, dk);
        for (k = 0; k < h->stop_freq; k++) sum += dk[k];
        s->k2 = stop_min + sum;
    }
    if (s->k2 > 64) s->k2 = 64;
    max_bands = fs <= 32000 ? 48 : fs == 44100 ? 35 : 32;
    if (s->k0 >= s->k2 || s->k2 - s->k0 > max_bands || s->k0 < 1) return 0;

    if (h->freq_scale == 0) {
        /* 2 * INT((k2 - k0) / 2) bands of 1, or 2 * NINT((k2 - k0) / 4) bands of 2 */
        int dkk = h->alter_scale ? 2 : 1, diff, incr;
        int n = h->alter_scale ? (((s->k2 - s->k0) >> 1) + 1) & ~1 : 2 * ((s->k2 - s->k0) / 2);
        if (n <= 0) return 0;
        diff = (s->k2 - s->k0) - n * dkk;
        for (k = 0; k < n; k++) dk[k] = dkk;
        if (diff < 0) {
            incr = 1;
            k = 0;
        } else {
            incr = -1;
            k = n - 1;
        }
        while (diff != 0 && k >= 0 && k < n) {
            dk[k] -= incr;
            k += incr;
            diff += incr;
        }
        s->f_master[0] = s->k0;
        for (k = 1; k <= n; k++) s->f_master[k] = s->f_master[k - 1] + dk[k - 1];
        s->N_master = n;
    } else {
        static const int bands_tab[3] = { 12, 10, 8 };
        int bands = bands_tab[h->freq_scale - 1], k1, two, n0, n1 = 0, dk1[64];
        double warp = h->alter_scale ? 1.3 : 1.0;
        if ((double)s->k2 / s->k0 > 2.2449) {
            two = 1;
            k1 = 2 * s->k0;
        } else {
            two = 0;
            k1 = s->k2;
        }
        n0 = 2 * nint(bands * log((double)k1 / s->k0) / (2.0 * log(2.0)));
        if (n0 <= 0 || n0 > 63) return 0;
        geometric(s->k0, k1, n0, dk);
        s->f_master[0] = s->k0;
        for (k = 1; k <= n0; k++) s->f_master[k] = s->f_master[k - 1] + dk[k - 1];
        if (two) {
            n1 = 2 * nint(bands * log((double)s->k2 / k1) / (2.0 * log(2.0) * warp));
            if (n1 <= 0 || n0 + n1 > 64) return 0;
            geometric(k1, s->k2, n1, dk1);
            if (dk1[0] < dk[n0 - 1]) {
                int change = dk[n0 - 1] - dk1[0];
                dk1[0] += change;
                dk1[n1 - 1] -= change;
                qsort(dk1, (size_t)n1, sizeof(int), cmp_int);
            }
            for (k = 1; k <= n1; k++) s->f_master[n0 + k] = s->f_master[n0 + k - 1] + dk1[k - 1];
        }
        s->N_master = n0 + n1;
    }
    for (k = 0; k < s->N_master; k++)
        if (s->f_master[k + 1] <= s->f_master[k]) return 0;
    return 1;
}

static int is_patch_border(const fa_aac_sbr* s, int v)
{
    int i, b = s->kx;
    for (i = 0; i <= s->num_patches; i++) {
        if (b == v) return 1;
        if (i < s->num_patches) b += s->patch_n[i];
    }
    return 0;
}

static int build_patches(fa_aac_sbr* s)
{
    int msb = s->k0, usb = s->kx, goal = nint(2.048e6 / s->rate), k, i, sb = 0;
    s->num_patches = 0;
    if (goal < s->kx + s->M) {
        for (i = 0, k = 0; s->f_master[i] < goal; i++) k = i + 1;
    } else {
        k = s->N_master;
    }
    do {
        int j = k + 1, odd;
        do {
            j--;
            sb = s->f_master[j];
            odd = (sb - 2 + s->k0) % 2;
        } while (sb > s->k0 - 1 + msb - odd);
        if (s->num_patches >= MAX_PATCH) return 0;
        s->patch_n[s->num_patches] = sb - usb > 0 ? sb - usb : 0;
        s->patch_start[s->num_patches] = s->k0 - odd - s->patch_n[s->num_patches];
        if (s->patch_n[s->num_patches] > 0) {
            usb = sb;
            msb = sb;
            s->num_patches++;
        } else {
            msb = s->kx;
        }
        if (s->f_master[k] - sb < 3) k = s->N_master;
    } while (sb != s->kx + s->M);
    if (s->num_patches > 1 && s->patch_n[s->num_patches - 1] < 3) s->num_patches--;
    if (s->num_patches > 5 || s->num_patches == 0) return 0;
    for (i = 0; i < s->num_patches; i++)
        if (s->patch_start[i] < 1 || s->patch_start[i] + s->patch_n[i] > s->kx) return 0;
    return 1;
}

static int build_tables(fa_aac_sbr* s)
{
    const sbr_header* h = &s->h;
    int k;
    s->tables_ok = 0;
    if (!build_master(s)) return 0;
    if (h->xover_band >= s->N_master) return 0;
    s->N_high = s->N_master - h->xover_band;
    s->N_low = (s->N_high >> 1) + (s->N_high & 1);
    for (k = 0; k <= s->N_high; k++) s->f_high[k] = s->f_master[k + h->xover_band];
    s->f_low[0] = s->f_high[0];
    for (k = 1; k <= s->N_low; k++) s->f_low[k] = s->f_high[2 * k - (s->N_high & 1)];
    s->kx = s->f_high[0];
    s->M = s->f_high[s->N_high] - s->kx;
    if (s->kx > 32 || s->kx + s->M > 64 || s->M <= 0) return 0;
    if (h->noise_bands == 0) {
        s->N_Q = 1;
    } else {
        s->N_Q = nint(h->noise_bands * log((double)s->k2 / s->kx) / log(2.0));
        if (s->N_Q < 1) s->N_Q = 1;
    }
    if (s->N_Q > MAX_NQ) return 0;
    {
        int ik = 0;
        s->f_noise[0] = s->f_low[0];
        for (k = 1; k <= s->N_Q; k++) {
            ik += (s->N_low - ik) / (s->N_Q + 1 - k);
            s->f_noise[k] = s->f_low[ik];
        }
    }
    if (!build_patches(s)) return 0;
    /* limiter bands */
    if (h->limiter_bands == 0) {
        s->f_lim[0] = s->f_low[0];
        s->f_lim[1] = s->f_low[s->N_low];
        s->N_L = 1;
    } else {
        static const double lim_bands[3] = { 1.2, 2.0, 3.0 };
        double lb = lim_bands[h->limiter_bands - 1];
        int tab[64 + MAX_PATCH], n = 0, border = s->kx, nr;
        for (k = 0; k <= s->N_low; k++) tab[n++] = s->f_low[k];
        for (k = 0; k < s->num_patches - 1; k++) {
            border += s->patch_n[k];
            tab[n++] = border;
        }
        qsort(tab, (size_t)n, sizeof(int), cmp_int);
        nr = n - 1;
        k = 1;
        while (k <= nr) {
            double oct = tab[k - 1] > 0 ? log((double)tab[k] / tab[k - 1]) / log(2.0) : 0.0;
            if (oct * lb < 0.49) {
                int rm;
                if (tab[k] == tab[k - 1]) {
                    rm = k;
                } else if (is_patch_border(s, tab[k])) {
                    if (is_patch_border(s, tab[k - 1])) {
                        k++;
                        continue;
                    }
                    rm = k - 1;
                } else {
                    rm = k;
                }
                memmove(tab + rm, tab + rm + 1, sizeof(int) * (size_t)(nr - rm));
                nr--;
                continue;
            }
            k++;
        }
        if (nr > MAX_LIM) return 0;
        for (k = 0; k <= nr; k++) s->f_lim[k] = tab[k];
        s->N_L = nr;
    }
    s->tables_ok = 1;
    return 1;
}

/* ---- bitstream (4.5.2.8) ----------------------------------------------------------------- */

/* 2: new frequency tables (the envelopes start over), 1: a new limiter table only. */
static int read_header(fa_aac_sbr* s, fa_aac_bits* b)
{
    sbr_header h = s->h;
    int extra1, extra2, reset;
    h.amp_res = (int)fa_aac_get(b, 1);
    h.start_freq = (int)fa_aac_get(b, 4);
    h.stop_freq = (int)fa_aac_get(b, 4);
    h.xover_band = (int)fa_aac_get(b, 3);
    fa_aac_skip(b, 2);
    extra1 = (int)fa_aac_get(b, 1);
    extra2 = (int)fa_aac_get(b, 1);
    if (extra1) {
        h.freq_scale = (int)fa_aac_get(b, 2);
        h.alter_scale = (int)fa_aac_get(b, 1);
        h.noise_bands = (int)fa_aac_get(b, 2);
    } else {
        h.freq_scale = 2;
        h.alter_scale = 1;
        h.noise_bands = 2;
    }
    if (extra2) {
        h.limiter_bands = (int)fa_aac_get(b, 2);
        h.limiter_gains = (int)fa_aac_get(b, 2);
        h.interpol_freq = (int)fa_aac_get(b, 1);
        h.smoothing_mode = (int)fa_aac_get(b, 1);
    } else {
        h.limiter_bands = 2;
        h.limiter_gains = 2;
        h.interpol_freq = 1;
        h.smoothing_mode = 1;
    }
    reset = !s->header_valid || h.start_freq != s->h.start_freq || h.stop_freq != s->h.stop_freq ||
            h.xover_band != s->h.xover_band || h.freq_scale != s->h.freq_scale ||
            h.alter_scale != s->h.alter_scale || h.noise_bands != s->h.noise_bands;
    if (!reset && h.limiter_bands != s->h.limiter_bands) reset = 1;   /* new limiter table only */
    else if (reset) reset = 2;
    s->h = h;
    s->header_valid = 1;
    return reset;
}

static int ceil_log2(int x)
{
    int n = 0;
    while ((1 << n) < x) n++;
    return n;
}

static int read_grid(fa_aac_sbr* s, fa_aac_bits* b, sbr_chan* ch)
{
    int i, rel0[4], rel1[4], n0 = 0, n1 = 0, lead = 0, trail = 16, L;
    ch->frame_class = (int)fa_aac_get(b, 2);
    switch (ch->frame_class) {
    case FIXFIX:
        L = 1 << fa_aac_get(b, 2);
        if (L > 4) return 0;
        ch->freq_res[0] = (int)fa_aac_get(b, 1);
        for (i = 1; i < L; i++) ch->freq_res[i] = ch->freq_res[0];
        ch->pointer = 0;
        break;
    case FIXVAR:
        trail = 16 + (int)fa_aac_get(b, 2);
        n1 = (int)fa_aac_get(b, 2);
        for (i = 0; i < n1; i++) rel1[i] = 2 * (int)fa_aac_get(b, 2) + 2;
        L = n1 + 1;
        ch->pointer = (int)fa_aac_get(b, ceil_log2(n1 + 2));
        for (i = 0; i < L; i++) ch->freq_res[L - 1 - i] = (int)fa_aac_get(b, 1);
        break;
    case VARFIX:
        lead = (int)fa_aac_get(b, 2);
        n0 = (int)fa_aac_get(b, 2);
        for (i = 0; i < n0; i++) rel0[i] = 2 * (int)fa_aac_get(b, 2) + 2;
        L = n0 + 1;
        ch->pointer = (int)fa_aac_get(b, ceil_log2(n0 + 2));
        for (i = 0; i < L; i++) ch->freq_res[i] = (int)fa_aac_get(b, 1);
        break;
    default:
        lead = (int)fa_aac_get(b, 2);
        trail = 16 + (int)fa_aac_get(b, 2);
        n0 = (int)fa_aac_get(b, 2);
        n1 = (int)fa_aac_get(b, 2);
        for (i = 0; i < n0; i++) rel0[i] = 2 * (int)fa_aac_get(b, 2) + 2;
        for (i = 0; i < n1; i++) rel1[i] = 2 * (int)fa_aac_get(b, 2) + 2;
        L = n0 + n1 + 1;
        if (L > MAX_ENV) return 0;
        ch->pointer = (int)fa_aac_get(b, ceil_log2(n0 + n1 + 2));
        for (i = 0; i < L; i++) ch->freq_res[i] = (int)fa_aac_get(b, 1);
        break;
    }
    if (L > MAX_ENV || ch->pointer > L + 1) return 0;
    ch->L_E = L;
    ch->L_Q = L > 1 ? 2 : 1;
    /* envelope borders */
    ch->t_E[0] = lead;
    ch->t_E[L] = trail;
    if (ch->frame_class == FIXFIX) {
        for (i = 1; i < L; i++) ch->t_E[i] = ch->t_E[i - 1] + 16 / L;
    } else {
        for (i = 1; i <= n0; i++) ch->t_E[i] = ch->t_E[i - 1] + rel0[i - 1];
        for (i = 1; i <= n1; i++) ch->t_E[L - i] = ch->t_E[L - i + 1] - rel1[i - 1];
    }
    for (i = 0; i < L; i++)
        if (ch->t_E[i + 1] <= ch->t_E[i]) return 0;
    /* the transient envelope and the noise floor borders */
    switch (ch->frame_class) {
    case FIXFIX: ch->l_A = -1; break;
    case VARFIX: ch->l_A = ch->pointer > 1 ? ch->pointer - 1 : -1; break;
    default: ch->l_A = ch->pointer == 0 ? -1 : L + 1 - ch->pointer; break;
    }
    ch->t_Q[0] = ch->t_E[0];
    if (L == 1) {
        ch->t_Q[1] = ch->t_E[1];
    } else {
        int mid;
        if (ch->frame_class == FIXFIX) mid = L / 2;
        else if (ch->frame_class == VARFIX) mid = ch->pointer == 0 ? 1 : ch->pointer == 1 ? L - 1 : ch->pointer - 1;
        else mid = ch->pointer > 1 ? L + 1 - ch->pointer : L - 1;
        if (mid <= 0 || mid >= L) return 0;
        ch->t_Q[1] = ch->t_E[mid];
        ch->t_Q[2] = ch->t_E[L];
    }
    ch->amp_res = s->h.amp_res;
    if (ch->frame_class == FIXFIX && L == 1) ch->amp_res = 0;
    return fa_aac_left(b) >= 0;
}

static void copy_grid(sbr_chan* dst, const sbr_chan* src)
{
    dst->frame_class = src->frame_class;
    dst->L_E = src->L_E;
    dst->L_Q = src->L_Q;
    dst->pointer = src->pointer;
    dst->l_A = src->l_A;
    dst->amp_res = src->amp_res;
    memcpy(dst->t_E, src->t_E, sizeof(dst->t_E));
    memcpy(dst->t_Q, src->t_Q, sizeof(dst->t_Q));
    memcpy(dst->freq_res, src->freq_res, sizeof(dst->freq_res));
}

static void read_dtdf(fa_aac_bits* b, sbr_chan* ch)
{
    int i;
    for (i = 0; i < ch->L_E; i++) ch->df_env[i] = (int)fa_aac_get(b, 1);
    for (i = 0; i < ch->L_Q; i++) ch->df_noise[i] = (int)fa_aac_get(b, 1);
}

static void read_invf(const fa_aac_sbr* s, fa_aac_bits* b, sbr_chan* ch)
{
    int i;
    for (i = 0; i < s->N_Q; i++) ch->invf[i] = (int)fa_aac_get(b, 2);
}

static int sym(fa_aac_bits* b, const uint16_t* t)
{
    int v = fa_aac_vlc(b, t, FA_AAC_VLC_ROOT);
    return v < 0 ? 1000 : v - 128;
}

static int read_envelope(const fa_aac_sbr* s, fa_aac_bits* b, sbr_chan* ch, int bal)
{
    const uint16_t *t, *f;
    int l, k, bits, delta = bal ? 2 : 1;
    if (ch->amp_res) {
        t = bal ? fa_aac_sbr_t_huffman_env_bal_3_0db : fa_aac_sbr_t_huffman_env_3_0db;
        f = bal ? fa_aac_sbr_f_huffman_env_bal_3_0db : fa_aac_sbr_f_huffman_env_3_0db;
        bits = bal ? 5 : 6;
    } else {
        t = bal ? fa_aac_sbr_t_huffman_env_bal_1_5db : fa_aac_sbr_t_huffman_env_1_5db;
        f = bal ? fa_aac_sbr_f_huffman_env_bal_1_5db : fa_aac_sbr_f_huffman_env_1_5db;
        bits = bal ? 6 : 7;
    }
    for (l = 0; l < ch->L_E; l++) {
        int n = ch->freq_res[l] ? s->N_high : s->N_low;
        if (ch->df_env[l] == 0) {
            ch->E[l][0] = delta * (int)fa_aac_get(b, bits);
            for (k = 1; k < n; k++) {
                int v = sym(b, f);
                if (v == 1000) return 0;
                ch->E[l][k] = ch->E[l][k - 1] + delta * v;
            }
        } else {
            /* Time direction: from the previous envelope, mapped between resolutions. */
            const int* prev = l ? ch->E[l - 1] : ch->E_last;
            int rprev = l ? ch->freq_res[l - 1] : ch->freq_res_last, r = ch->freq_res[l];
            if (!l && !ch->have_last) return 0;
            for (k = 0; k < n; k++) {
                int v = sym(b, t), base;
                if (v == 1000) return 0;
                if (r == rprev) {
                    base = prev[k];
                } else if (r == 0) {
                    /* low from high: the high band starting where this one does */
                    int i;
                    base = prev[0];
                    for (i = 0; i <= s->N_high; i++)
                        if (s->f_high[i] == s->f_low[k]) { base = prev[i]; break; }
                } else {
                    /* high from low: the low band containing this one */
                    int i = 0;
                    while (i + 1 < s->N_low && s->f_low[i + 1] <= s->f_high[k]) i++;
                    base = prev[i];
                }
                ch->E[l][k] = base + delta * v;
            }
        }
    }
    return fa_aac_left(b) >= 0;
}

static int read_noise(const fa_aac_sbr* s, fa_aac_bits* b, sbr_chan* ch, int bal)
{
    const uint16_t* t = bal ? fa_aac_sbr_t_huffman_noise_bal_3_0db : fa_aac_sbr_t_huffman_noise_3_0db;
    const uint16_t* f = bal ? fa_aac_sbr_f_huffman_env_bal_3_0db : fa_aac_sbr_f_huffman_env_3_0db;
    int l, k, delta = bal ? 2 : 1;
    for (l = 0; l < ch->L_Q; l++) {
        if (ch->df_noise[l] == 0) {
            ch->Q[l][0] = delta * (int)fa_aac_get(b, 5);
            for (k = 1; k < s->N_Q; k++) {
                int v = sym(b, f);
                if (v == 1000) return 0;
                ch->Q[l][k] = ch->Q[l][k - 1] + delta * v;
            }
        } else {
            const int* prev = l ? ch->Q[l - 1] : ch->Q_last;
            if (!l && !ch->have_last) return 0;
            for (k = 0; k < s->N_Q; k++) {
                int v = sym(b, t);
                if (v == 1000) return 0;
                ch->Q[l][k] = prev[k] + delta * v;
            }
        }
    }
    return fa_aac_left(b) >= 0;
}

static void read_harmonics(const fa_aac_sbr* s, fa_aac_bits* b, sbr_chan* ch)
{
    int k;
    memset(ch->add_harmonic, 0, sizeof(ch->add_harmonic));
    if (!fa_aac_get(b, 1)) return;
    for (k = 0; k < s->N_high; k++) ch->add_harmonic[k] = (uint8_t)fa_aac_get(b, 1);
}

static void read_extensions(fa_aac_sbr* s, fa_aac_bits* b)
{
    int cnt, left;
#ifdef FA_AAC_TRACE
    fprintf(stderr, "EXT %zu\n", b->pos);   /* for tests/data/aac/ps_rewrite.py */
#endif
    if (!fa_aac_get(b, 1)) return;
    cnt = (int)fa_aac_get(b, 4);
    if (cnt == 15) cnt += (int)fa_aac_get(b, 8);
    left = cnt * 8;
    while (left > 7) {
        int id = (int)fa_aac_get(b, 2);
        size_t start = b->pos;
        left -= 2;
        if (id == 2 && s->nch == 1 && left > 0) {
            int used;
            if (!s->ps) s->ps = fa_aac_ps_create();
            used = s->ps ? fa_aac_ps_parse(s->ps, b, left) : -1;
            if (used >= 0) {
                s->ps_seen = 1;
                s->ps_data = 1;
            }
        }
        /* Any other extension, or what PS left: skipped to the end of the data. */
        b->pos = start + (size_t)left;
        left = 0;
    }
    fa_aac_skip(b, (size_t)(left > 0 ? left : 0));
}

/* Dequantizes the envelope and noise data (4.6.18.3.5). */
static void dequantize(fa_aac_sbr* s)
{
    int c, l, k;
    if (s->nch == 2 && s->coupling) {
        sbr_chan* a = &s->ch[0];
        sbr_chan* b = &s->ch[1];
        /* The balance is centred on 12 steps of 3 dB or 24 of 1.5 dB. */
        float pan_a = a->amp_res ? 1.0f : 0.5f, centre = a->amp_res ? 12.0f : 24.0f;
        for (l = 0; l < a->L_E; l++) {
            int n = a->freq_res[l] ? s->N_high : s->N_low;
            for (k = 0; k < n; k++) {
                float level = 64.0f * exp2f((float)a->E[l][k] * pan_a + 1.0f);
                float pan = ((float)b->E[l][k] - centre) * pan_a;
                a->E_orig[l][k] = level / (1.0f + exp2f(-pan));
                b->E_orig[l][k] = level / (1.0f + exp2f(pan));
            }
        }
        for (l = 0; l < a->L_Q; l++)
            for (k = 0; k < s->N_Q; k++) {
                float level = exp2f(6.0f - (float)a->Q[l][k] + 1.0f);
                float pan = (float)b->Q[l][k];
                a->Q_orig[l][k] = level / (1.0f + exp2f(12.0f - pan));
                b->Q_orig[l][k] = level / (1.0f + exp2f(pan - 12.0f));
            }
        return;
    }
    for (c = 0; c < s->nch; c++) {
        sbr_chan* ch = &s->ch[c];
        float a = ch->amp_res ? 1.0f : 0.5f;
        for (l = 0; l < ch->L_E; l++) {
            int n = ch->freq_res[l] ? s->N_high : s->N_low;
            for (k = 0; k < n; k++) ch->E_orig[l][k] = 64.0f * exp2f((float)ch->E[l][k] * a);
        }
        for (l = 0; l < ch->L_Q; l++)
            for (k = 0; k < s->N_Q; k++) ch->Q_orig[l][k] = exp2f(6.0f - (float)ch->Q[l][k]);
    }
}

void fa_aac_sbr_parse(fa_aac_sbr* s, fa_aac_bits* b, int bits, int crc)
{
    size_t end = b->pos + (size_t)(bits > 0 ? bits : 0);
    int ok = 1, c;
    s->data_valid = 0;
    s->ps_data = 0;
    if (bits <= 0) return;
    if (crc) fa_aac_skip(b, 10);
    if (fa_aac_get(b, 1)) {
        int r = read_header(s, b);
        if (r) {
            if (!build_tables(s)) {
                s->header_valid = 0;
                b->pos = end;
                return;
            }
            if (r == 2)
                for (c = 0; c < 2; c++) s->ch[c].have_last = 0;
        }
    }
    if (!s->header_valid || !s->tables_ok) {
        b->pos = end;
        return;
    }
    if (s->nch == 1) {
        sbr_chan* ch = &s->ch[0];
        if (fa_aac_get(b, 1)) fa_aac_skip(b, 4);
        ok = read_grid(s, b, ch);
        if (ok) {
            read_dtdf(b, ch);
            read_invf(s, b, ch);
            ok = read_envelope(s, b, ch, 0) && read_noise(s, b, ch, 0);
        }
        if (ok) read_harmonics(s, b, ch);
    } else {
        sbr_chan* a = &s->ch[0];
        sbr_chan* bb = &s->ch[1];
        if (fa_aac_get(b, 1)) fa_aac_skip(b, 8);
        s->coupling = (int)fa_aac_get(b, 1);
        if (s->coupling) {
            ok = read_grid(s, b, a);
            if (ok) {
                copy_grid(bb, a);
                read_dtdf(b, a);
                read_dtdf(b, bb);
                read_invf(s, b, a);
                memcpy(bb->invf, a->invf, sizeof(a->invf));
                ok = read_envelope(s, b, a, 0) && read_noise(s, b, a, 0) && read_envelope(s, b, bb, 1) &&
                     read_noise(s, b, bb, 1);
            }
        } else {
            ok = read_grid(s, b, a) && read_grid(s, b, bb);
            if (ok) {
                read_dtdf(b, a);
                read_dtdf(b, bb);
                read_invf(s, b, a);
                read_invf(s, b, bb);
                ok = read_envelope(s, b, a, 0) && read_envelope(s, b, bb, 0) && read_noise(s, b, a, 0) &&
                     read_noise(s, b, bb, 0);
            }
        }
        if (ok) {
            read_harmonics(s, b, a);
            read_harmonics(s, b, bb);
        }
    }
    if (ok) read_extensions(s, b);
    if (!ok || b->pos > end) {
        for (c = 0; c < 2; c++) s->ch[c].have_last = 0;
        s->ps_data = 0;
        b->pos = end;
        return;
    }
    b->pos = end;
    dequantize(s);
    s->data_valid = 1;
}

/* ---- QMF banks (4.6.18.4) ------------------------------------------------------------------ */

/* 32 new samples into 32 complex subbands. The matrixing
 *   X[k] = 2 sum_n u[n] exp(i pi/64 (k + 1/2)(2n - 1/2)), k = 0..31, n = 0..63,
 * is one 64-point DCT-IV C of u folded: the angle is pi/256 (2k + 1) q with
 * q = 4n - 1, and q's odd values fold onto the DCT's 2m + 1 = |q| below 128 and
 * 256 - q above (with a sign flip there, cos(pi (2k+1) - a) = -cos a). So
 * Re X[k] = C[k], and as 2(63 - k) + 1 = 128 - (2k + 1) and q is odd,
 * Im X[k] = -C[63 - k]. The QMF domain works on the standard's 16-bit sample
 * scale (its energies and the "1 +" terms of the gain formulas assume it), which
 * goes into the fold. */
static void qmf_analysis(const fa_aac_sbr* s, sbr_chan* ch, const float* in, float (*out)[2])
{
    const float scale = 2.0f * 32768.0f;
    float u[64], v[64], c[64];
    int n, k;
    memmove(ch->ana + 32, ch->ana, sizeof(float) * 288);
    for (n = 0; n < 32; n++) ch->ana[n] = in[31 - n];
    for (n = 0; n < 64; n++) {
        float sum = 0.0f;
        int j;
        for (j = 0; j < 5; j++) sum += ch->ana[n + 64 * j] * fa_aac_sbr_qmf_window[2 * (n + 64 * j)];
        u[n] = sum * scale;
    }
    v[0] = u[0];                                  /* q = -1 */
    for (n = 1; n <= 32; n++) v[2 * n - 1] = u[n];  /* q = 3 .. 127 */
    for (n = 33; n < 64; n++) v[128 - 2 * n] = -u[n];   /* q = 131 .. 251 */
    fa_mdct_dct4(&s->qmf->dct4, v, c);
    for (k = 0; k < 32; k++) {
        out[k][0] = c[k];
        out[k][1] = -c[63 - k];
    }
}

/* 64 complex subbands into 64 samples. The matrixing
 *   v[n] = 1/64 sum_k Re(X[k] exp(i pi/128 (k + 1/2)(2n - 255))), n = 0..127,
 * is, as (2n - 255)/2 = (n + 1/2) - 128, a DCT-IV C of the real parts and a
 * DST-IV S of the imaginary ones: v[n] = (S[n] - C[n]) / 64 below 64 and
 * (C[127-n] + S[127-n]) / 64 above. S[j] = (-1)^j DCT-IV(imaginary parts reversed)[j]. */
static void qmf_synthesis(const fa_aac_sbr* s, sbr_chan* ch, float (*in)[2], float* out)
{
    float* v = ch->syn;
    float re[64], im[64], c[64], d[64];
    const float scale = 1.0f / (64.0f * 32768.0f);
    int n, k;
    memmove(v + 128, v, sizeof(float) * 1152);
    for (k = 0; k < 64; k++) {
        re[k] = in[k][0] * scale;
        im[63 - k] = in[k][1] * scale;
    }
    fa_mdct_dct4(&s->qmf->dct4, re, c);
    fa_mdct_dct4(&s->qmf->dct4, im, d);
    for (n = 1; n < 64; n += 2) d[n] = -d[n];
    for (n = 0; n < 64; n++) {
        v[n] = d[n] - c[n];
        v[127 - n] = c[n] + d[n];
    }
    for (k = 0; k < 64; k++) {
        float sum = 0.0f;
        for (n = 0; n < 5; n++) {
            sum += v[256 * n + k] * fa_aac_sbr_qmf_window[128 * n + k];
            sum += v[256 * n + 192 + k] * fa_aac_sbr_qmf_window[128 * n + 64 + k];
        }
        out[k] = sum;
    }
}

/* ---- HF generation (4.6.18.6) ---------------------------------------------------------------- */

static float new_bw(int mode, int prev)
{
    switch (mode) {
    case 0: return prev == 1 ? 0.6f : 0.0f;
    case 1: return prev == 0 ? 0.6f : 0.75f;
    case 2: return 0.9f;
    default: return 0.98f;
    }
}

/* Linear prediction coefficients of each low subband (covariance method). */
static void lpc(const sbr_chan* ch, int k, float a0[2], float a1[2])
{
    double phi[3][3][2];
    int i, j, n;
    double d, re, im;
    memset(phi, 0, sizeof(phi));
    for (i = 0; i < 3; i++)
        for (j = i; j < 3; j++) {
            double sr = 0.0, si = 0.0;
            for (n = 0; n < SLOTS + 6; n++) {
                const float* x = ch->xlow[n - i + HFADJ][k];
                const float* y = ch->xlow[n - j + HFADJ][k];
                /* x * conj(y) */
                sr += (double)x[0] * y[0] + (double)x[1] * y[1];
                si += (double)x[1] * y[0] - (double)x[0] * y[1];
            }
            phi[i][j][0] = sr;
            phi[i][j][1] = si;
        }
    d = phi[2][2][0] * phi[1][1][0] - (phi[1][2][0] * phi[1][2][0] + phi[1][2][1] * phi[1][2][1]) / (1.0 + 1e-6);
    a1[0] = a1[1] = 0.0f;
    if (d != 0.0) {
        /* (phi01 * phi12 - phi02 * phi11) / d */
        re = phi[0][1][0] * phi[1][2][0] - phi[0][1][1] * phi[1][2][1] - phi[0][2][0] * phi[1][1][0];
        im = phi[0][1][0] * phi[1][2][1] + phi[0][1][1] * phi[1][2][0] - phi[0][2][1] * phi[1][1][0];
        a1[0] = (float)(re / d);
        a1[1] = (float)(im / d);
    }
    a0[0] = a0[1] = 0.0f;
    if (phi[1][1][0] != 0.0) {
        /* -(phi01 + a1 * conj(phi12)) / phi11 */
        re = phi[0][1][0] + a1[0] * phi[1][2][0] + a1[1] * phi[1][2][1];
        im = phi[0][1][1] + a1[1] * phi[1][2][0] - a1[0] * phi[1][2][1];
        a0[0] = (float)(-re / phi[1][1][0]);
        a0[1] = (float)(-im / phi[1][1][0]);
    }
    if (a0[0] * a0[0] + a0[1] * a0[1] >= 16.0f || a1[0] * a1[0] + a1[1] * a1[1] >= 16.0f) {
        a0[0] = a0[1] = a1[0] = a1[1] = 0.0f;
    }
}

static void hf_generate(fa_aac_sbr* s, sbr_chan* ch, float (*xhigh)[64][2])
{
    float bw[MAX_NQ], a0[32][2], a1[32][2];
    int i, x, k, p, l, g, lo = 2 * ch->t_E[0], hi = 2 * ch->t_E[ch->L_E];
    int done[32];
    for (i = 0; i < s->N_Q; i++) {
        float t = new_bw(ch->invf[i], ch->invf_last[i]);
        float v = t < ch->bw_last[i] ? 0.75f * t + 0.25f * ch->bw_last[i] : 0.90625f * t + 0.09375f * ch->bw_last[i];
        if (v < 0.015625f) v = 0.0f;
        if (v >= 0.99609375f) v = 0.99609375f;
        bw[i] = v;
    }
    memset(done, 0, sizeof(done));
    k = s->kx;
    for (i = 0; i < s->num_patches; i++) {
        for (x = 0; x < s->patch_n[i]; x++, k++) {
            float b, b2;
            p = s->patch_start[i] + x;
            if (!done[p]) {
                lpc(ch, p, a0[p], a1[p]);
                done[p] = 1;
            }
            for (g = 0; g < s->N_Q - 1 && s->f_noise[g + 1] <= k; g++) {}
            b = bw[g];
            b2 = b * b;
            for (l = lo; l < hi; l++) {
                const float* x0 = ch->xlow[l + HFADJ][p];
                const float* x1 = ch->xlow[l - 1 + HFADJ][p];
                const float* x2 = ch->xlow[l - 2 + HFADJ][p];
                float* o = xhigh[l + HFADJ][k];
                o[0] = x0[0] + b * (a0[p][0] * x1[0] - a0[p][1] * x1[1]) + b2 * (a1[p][0] * x2[0] - a1[p][1] * x2[1]);
                o[1] = x0[1] + b * (a0[p][0] * x1[1] + a0[p][1] * x1[0]) + b2 * (a1[p][0] * x2[1] + a1[p][1] * x2[0]);
            }
        }
    }
    for (i = 0; i < s->N_Q; i++) {
        ch->bw_last[i] = bw[i];
        ch->invf_last[i] = ch->invf[i];
    }
}

/* ---- HF adjustment (4.6.18.7) ------------------------------------------------------------------ */

static const float h_smooth[5] = { 0.33333333333333f, 0.30150283239582f, 0.21816949906249f, 0.11516383427084f,
                                   0.03183050093751f };

static void hf_adjust(fa_aac_sbr* s, sbr_chan* ch, float (*xhigh)[64][2], float (*y)[64][2])
{
    static const float lim_gain[4] = { 0.70794578438414f, 1.0f, 1.41253754462275f, 1e10f };
    static const float phi_re[4] = { 1.0f, 0.0f, -1.0f, 0.0f };
    static const float phi_im[4] = { 0.0f, 1.0f, 0.0f, -1.0f };
    int M = s->M, kx = s->kx, L = ch->L_E, l, m, i;
    int l_A_prev_end = ch->l_A_last == ch->L_E_last ? 0 : -1;   /* a transient at the end of the last frame */
    uint8_t s_index[MAX_ENV][64];
    int h_sl = s->h.smoothing_mode ? 0 : 4;
    /* Sinusoids: placed in the middle subband of each high-resolution band. */
    for (l = 0; l < L; l++) {
        memset(s_index[l], 0, 64);
        for (i = 0; i < s->N_high; i++) {
            int mm = ((s->f_high[i + 1] + s->f_high[i]) >> 1) - kx;
            if (ch->add_harmonic[i] && (l >= ch->l_A || ch->harmonic_last[mm])) s_index[l][mm] = 1;
        }
    }
    for (l = 0; l < L; l++) {
        float e_orig[64], q_orig[64], e_curr[64], g[64], qm[64], sm[64];
        uint8_t s_mapped[64];
        int r = ch->freq_res[l], nb = r ? s->N_high : s->N_low, q;
        const int* ft = r ? s->f_high : s->f_low;
        int lo = 2 * ch->t_E[l], hi = 2 * ch->t_E[l + 1];
        int delta = !(l == ch->l_A || l == l_A_prev_end);
        for (q = 0; q < ch->L_Q - 1 && ch->t_Q[q + 1] <= ch->t_E[l]; q++) {}
        /* Mapped envelope, noise floor and sinusoids per subband. */
        for (i = 0; i < nb; i++) {
            int any = 0;
            for (m = ft[i] - kx; m < ft[i + 1] - kx; m++) {
                e_orig[m] = ch->E_orig[l][i];
                if (s_index[l][m]) any = 1;
            }
            for (m = ft[i] - kx; m < ft[i + 1] - kx; m++) s_mapped[m] = (uint8_t)any;
        }
        for (i = 0; i < s->N_Q; i++)
            for (m = s->f_noise[i] - kx; m < s->f_noise[i + 1] - kx; m++) q_orig[m] = ch->Q_orig[q][i];
        /* Energies of the generated high band. */
        if (s->h.interpol_freq) {
            for (m = 0; m < M; m++) {
                float sum = 0.0f;
                for (i = lo; i < hi; i++) {
                    const float* v = xhigh[i + HFADJ][m + kx];
                    sum += v[0] * v[0] + v[1] * v[1];
                }
                e_curr[m] = sum / (float)(hi - lo);
            }
        } else {
            for (i = 0; i < nb; i++) {
                float sum = 0.0f;
                int k;
                for (k = ft[i]; k < ft[i + 1]; k++)
                    for (m = lo; m < hi; m++) {
                        const float* v = xhigh[m + HFADJ][k];
                        sum += v[0] * v[0] + v[1] * v[1];
                    }
                sum /= (float)((hi - lo) * (ft[i + 1] - ft[i]));
                for (k = ft[i]; k < ft[i + 1]; k++) e_curr[k - kx] = sum;
            }
        }
        /* Gains. */
        for (m = 0; m < M; m++) {
            float tmp = e_orig[m] / (1.0f + q_orig[m]);
            qm[m] = sqrtf(tmp * q_orig[m]);
            sm[m] = sqrtf(tmp * (float)s_index[l][m]);
            if (!s_mapped[m]) g[m] = sqrtf(e_orig[m] / ((1.0f + e_curr[m]) * (1.0f + (delta ? q_orig[m] : 0.0f))));
            else g[m] = sqrtf(e_orig[m] * q_orig[m] / ((1.0f + e_curr[m]) * (1.0f + q_orig[m])));
        }
        /* Limiter. */
        for (i = 0; i < s->N_L; i++) {
            float so = 0.0f, sc = 0.0f, gmax, den = 0.0f, boost;
            int a = s->f_lim[i] - kx, b = s->f_lim[i + 1] - kx;
            for (m = a; m < b; m++) {
                so += e_orig[m];
                sc += e_curr[m];
            }
            gmax = lim_gain[s->h.limiter_gains] * sqrtf((1e-12f + so) / (1e-12f + sc));
            if (gmax > 1e5f) gmax = 1e5f;
            for (m = a; m < b; m++) {
                if (g[m] > gmax) {
                    qm[m] *= gmax / g[m];
                    g[m] = gmax;
                }
                den += e_curr[m] * g[m] * g[m] + sm[m] * sm[m];
                if (sm[m] == 0.0f && delta) den += qm[m] * qm[m];
            }
            boost = sqrtf((1e-12f + so) / (1e-12f + den));
            if (boost > 1.584893192f) boost = 1.584893192f;
            for (m = a; m < b; m++) {
                g[m] *= boost;
                qm[m] *= boost;
                sm[m] *= boost;
            }
        }
        /* Assembly, slot by slot. */
        for (i = lo; i < hi; i++) {
            int smooth = h_sl && l != ch->l_A && l != l_A_prev_end;
            int j;
            memmove(ch->g_hist[1], ch->g_hist[0], sizeof(float) * 64 * 4);
            memmove(ch->q_hist[1], ch->q_hist[0], sizeof(float) * 64 * 4);
            memcpy(ch->g_hist[0], g, sizeof(float) * (size_t)M);
            memcpy(ch->q_hist[0], qm, sizeof(float) * (size_t)M);
            if (!ch->hist_valid) {
                for (j = 1; j < 5; j++) {
                    memcpy(ch->g_hist[j], g, sizeof(float) * (size_t)M);
                    memcpy(ch->q_hist[j], qm, sizeof(float) * (size_t)M);
                }
                ch->hist_valid = 1;
            }
            for (m = 0; m < M; m++) {
                float gf, qf;
                const float* xv = xhigh[i + HFADJ][m + kx];
                float* o = y[i + HFADJ][m + kx];
                if (smooth) {
                    gf = 0.0f;
                    qf = 0.0f;
                    for (j = 0; j <= 4; j++) {
                        gf += ch->g_hist[j][m] * h_smooth[j];
                        qf += ch->q_hist[j][m] * h_smooth[j];
                    }
                } else {
                    gf = g[m];
                    qf = qm[m];
                }
                if (sm[m] != 0.0f || !delta) qf = 0.0f;
                ch->index_noise = (ch->index_noise + 1) & 511;
                o[0] = xv[0] * gf + qf * fa_aac_sbr_noise[2 * ch->index_noise];
                o[1] = xv[1] * gf + qf * fa_aac_sbr_noise[2 * ch->index_noise + 1];
                if (sm[m] != 0.0f) {
                    float sign = ((m + kx) & 1) ? -1.0f : 1.0f;
                    o[0] += sm[m] * phi_re[ch->index_sine];
                    o[1] += sm[m] * sign * phi_im[ch->index_sine];
                }
            }
            ch->index_sine = (ch->index_sine + 1) & 3;
        }
    }
    /* What the next frame needs. */
    memset(ch->harmonic_last, 0, sizeof(ch->harmonic_last));
    for (m = 0; m < 64; m++) ch->harmonic_last[m] = s_index[L - 1][m];
}

/* ---- the frame ------------------------------------------------------------------------------- */

static void save_last(sbr_chan* ch)
{
    int n = 64;
    memcpy(ch->E_last, ch->E[ch->L_E - 1], sizeof(int) * (size_t)n);
    ch->freq_res_last = ch->freq_res[ch->L_E - 1];
    memcpy(ch->Q_last, ch->Q[ch->L_Q - 1], sizeof(ch->Q_last));
    ch->have_last = 1;
}

int fa_aac_sbr_apply(fa_aac_sbr* s, const float* const in[2], float* out[2])
{
    int c, l, k, stereo_ps = 0, nout;
    int data = s->data_valid && s->tables_ok;
    for (c = 0; c < s->nch; c++) {
        sbr_chan* ch = &s->ch[c];
        float (*x)[64][2] = s->x[c];
        float (*ycur)[64][2] = ch->y[1];
        float (*yold)[64][2] = ch->y[0];
        int kx = data ? s->kx : s->kx_last, M = data ? s->M : 0;
        int t_old = 2 * ch->t_last - SLOTS;                   /* slots the last frame's envelopes reach into this one */
        if (t_old < 0) t_old = 0;
        /* QMF analysis: 32 new slots after 8 of history. */
        memmove(ch->xlow[0], ch->xlow[SLOTS], sizeof(float) * HFGEN * 32 * 2);
        for (l = 0; l < SLOTS; l++) qmf_analysis(s, ch, in[c] + 32 * l, ch->xlow[HFGEN + l]);
        /* This frame's high band. */
        memset(ycur, 0, sizeof(ch->y[1]));
        if (data) {
            hf_generate(s, ch, s->xhigh);
            hf_adjust(s, ch, s->xhigh, ycur);
        }
        /* The QMF matrix: low band delayed by t_HFAdj, the high band from the
         * envelopes covering each slot (the last frame's for its overhang). */
        memset(x, 0, sizeof(s->x[c]));
        for (l = 0; l < XSLOTS; l++) {
            int kxl = l < t_old ? s->kx_last : kx;
            for (k = 0; k < kxl && k < 32; k++) {
                x[l][k][0] = ch->xlow[l + HFADJ][k][0];
                x[l][k][1] = ch->xlow[l + HFADJ][k][1];
            }
            if (l >= SLOTS) continue;
            if (l < t_old) {
                for (k = s->kx_last; k < s->kx_last + s->M_last; k++) {
                    x[l][k][0] = yold[l + SLOTS + HFADJ][k][0];
                    x[l][k][1] = yold[l + SLOTS + HFADJ][k][1];
                }
            } else {
                for (k = kx; k < kx + M; k++) {
                    x[l][k][0] = ycur[l + HFADJ][k][0];
                    x[l][k][1] = ycur[l + HFADJ][k][1];
                }
            }
        }
        /* The overhang into the next frame stays in y[0]. */
        memcpy(yold, ycur, sizeof(ch->y[1]));
        if (data) {
            ch->t_last = ch->t_E[ch->L_E];
            ch->L_E_last = ch->L_E;
            ch->l_A_last = ch->l_A;
            save_last(ch);
        } else {
            ch->t_last = 16;
            ch->L_E_last = 0;
            ch->l_A_last = -1;
        }
    }
    if (data) {
        s->kx_last = s->kx;
        s->M_last = s->M;
    } else {
        s->M_last = 0;
    }
    /* Parametric stereo turns the mono matrix into two. */
    nout = s->nch;
    if (s->nch == 1 && s->ps_seen && s->ps && out[1]) {
        fa_aac_ps_apply(s->ps, s->x[0], s->x[1], s->ps_data, data ? s->kx : s->kx_last);
        stereo_ps = 1;
        nout = 2;
    }
    s->data_valid = 0;
    s->ps_data = 0;
    for (c = 0; c < nout; c++) {
        sbr_chan* ch = &s->ch[c];
        for (l = 0; l < SLOTS; l++) qmf_synthesis(s, ch, s->x[c][l], out[c] + 64 * l);
    }
    return stereo_ps;
}
