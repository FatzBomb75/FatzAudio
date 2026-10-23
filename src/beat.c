/* Tempo and beat grid from audio (clean room, from published methods).
 *
 * 1. Onset strength: log-magnitude spectral flux, a 1024-point FFT every 256
 *    samples at 44.1/48 kHz (scaled with the rate, so the envelope runs at about
 *    170-190 frames a second), with the bins below 200 Hz weighted up, where kicks
 *    are. Two frames go through one complex FFT (one as the real part, one as the
 *    imaginary part).
 * 2. The envelope minus its local mean (half a second), rectified, scaled to unit
 *    standard deviation.
 * 3. Tempo: the envelope's autocorrelation at 1, 2, 3 and 4 beat periods, weighted
 *    1, 1/2, 1/3, 1/4 (a comb), weighted by a log-normal preference around 120 BPM, searched in the
 *    preferred range in 0.05 BPM steps. A BPM tag picks between half, the same
 *    and double tempo.
 * 4. Beats: dynamic-programming beat tracking (Ellis 2007), each beat refined to
 *    a fraction of a frame on its onset peak.
 * 5. The constant grid: a robust straight-line fit of beat time over beat number
 *    (outliers dropped and fitted again), which gives the tempo far more finely
 *    than the envelope's frames. The confidence is the share of beats on the grid
 *    times how much the grid's onsets stand out.
 *
 * Frames of the envelope are placed where an attack lands (calibrated: the flux
 * of an attack peaks about a quarter window after the attack enters it). */
#include "fa_internal.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define ONSET_LOW_HZ 200.0
#define ONSET_MAX_HZ 11000.0
#define ONSET_LOG_GAIN 100.0f
#define PRIOR_BPM 120.0
#define PRIOR_OCTAVES 1.0
#define DP_TIGHTNESS 100.0
#define MIN_BEATS 8
#define ONSET_HALF_RATIO 0.6     /* every other beat this much weaker: half tempo */
#define ONSET_DELAY 0.18         /* attack to flux peak, in windows (calibrated) */

struct fa_onset {
    uint32_t rate;
    uint32_t n, hop;             /* FFT size and hop */
    const fa_fft_tables* tb;
    float* ring;                 /* the last n samples */
    uint32_t ring_pos;
    int64_t seen;                /* samples fed */
    uint32_t until_hop;          /* samples to the next frame */
    float* pending;              /* a windowed frame waiting for its pair */
    int have_pending;
    float *re, *im, *prev, *cur;
    uint32_t bins, low_bins;
    float* env;                  /* the envelope, one value per frame (all bins) */
    float* low;                  /* the same below 200 Hz */
    int64_t nenv, cap;
    int64_t first_frame_end;     /* sample just after frame 0's window */
    int failed;
};

static uint32_t fft_size_for(uint32_t rate)
{
    if (rate <= 25000) return 512;
    if (rate <= 50000) return 1024;
    if (rate <= 100000) return 2048;
    return 4096;
}

fa_onset* fa_onset_new(uint32_t rate)
{
    fa_onset* o;
    uint32_t n = fft_size_for(rate);
    double binhz;
    if (rate == 0) return NULL;
    o = (fa_onset*)calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->rate = rate;
    o->n = n;
    o->hop = n / 4;
    o->tb = fa_fft_tables_get(n);
    binhz = (double)rate / n;
    o->bins = (uint32_t)(ONSET_MAX_HZ / binhz);
    if (o->bins > n / 2) o->bins = n / 2;
    o->low_bins = (uint32_t)(ONSET_LOW_HZ / binhz) + 1;
    o->ring = (float*)calloc(n, sizeof(float));
    o->pending = (float*)calloc(n, sizeof(float));
    o->re = (float*)malloc((size_t)n * sizeof(float));
    o->im = (float*)malloc((size_t)n * sizeof(float));
    o->prev = (float*)calloc(n / 2 + 1, sizeof(float));
    o->cur = (float*)calloc(n / 2 + 1, sizeof(float));
    o->until_hop = o->hop;
    if (!o->tb || !o->ring || !o->pending || !o->re || !o->im || !o->prev || !o->cur) {
        fa_onset_free(o);
        return NULL;
    }
    return o;
}

void fa_onset_free(fa_onset* o)
{
    if (!o) return;
    free(o->ring);
    free(o->pending);
    free(o->re);
    free(o->im);
    free(o->prev);
    free(o->cur);
    free(o->env);
    free(o->low);
    free(o);
}

static int env_push(fa_onset* o, float all, float low)
{
    if (o->nenv == o->cap) {
        int64_t cap = o->cap ? o->cap * 2 : 4096;
        float* e = (float*)realloc(o->env, (size_t)cap * sizeof(float));
        float* l;
        if (!e) return 0;
        o->env = e;
        l = (float*)realloc(o->low, (size_t)cap * sizeof(float));
        if (!l) return 0;
        o->low = l;
        o->cap = cap;
    }
    o->env[o->nenv] = all;
    o->low[o->nenv++] = low;
    return 1;
}

/* Log magnitudes of one frame's spectrum into o->cur, then its flux: of all bins,
 * and of those below 200 Hz in *low. */
static float flux_of(fa_onset* o, const float* re, const float* im, int which, float* low)
{
    /* Bin k of frame A = (Z[k] + conj Z[n-k]) / 2, of frame B = (Z[k] - conj Z[n-k]) / 2i. */
    uint32_t k, n = o->n;
    float sum = 0.0f, lsum = 0.0f, scale = (float)(2.0 / o->tb->hann_sum);
    float* tmp;
    for (k = 1; k < o->bins; k++) {
        float zr = re[k], zi = im[k], cr = re[n - k], ci = -im[n - k];
        float xr, xi, mag, d;
        if (which == 0) { xr = 0.5f * (zr + cr); xi = 0.5f * (zi + ci); }
        else { xr = 0.5f * (zi - ci); xi = -0.5f * (zr - cr); }
        mag = sqrtf(xr * xr + xi * xi) * scale;
        o->cur[k] = logf(1.0f + ONSET_LOG_GAIN * mag);
        d = o->cur[k] - o->prev[k];
        if (d > 0.0f) {
            sum += d;
            if (k < o->low_bins) lsum += d;
        }
    }
    tmp = o->prev; o->prev = o->cur; o->cur = tmp;
    *low = lsum;
    return sum;
}

/* One windowed frame is ready (the window ends at the newest sample). */
static void frame_ready(fa_onset* o)
{
    uint32_t i, n = o->n;
    float* win = o->have_pending ? o->im : o->pending;
    for (i = 0; i < n; i++) win[i] = o->ring[(o->ring_pos + i) % n] * o->tb->hann[i];
    if (!o->have_pending) {
        o->have_pending = 1;
        return;
    }
    memcpy(o->re, o->pending, (size_t)n * sizeof(float));
    fa_fft(o->re, o->im, o->tb, n);
    for (i = 0; i < 2 && !o->failed; i++) {
        float low, all = flux_of(o, o->re, o->im, (int)i, &low);
        if (!env_push(o, all, low)) o->failed = 1;
    }
    o->have_pending = 0;
}

fa_result fa_onset_process(fa_onset* o, const float* mono, int64_t frames)
{
    int64_t i;
    for (i = 0; i < frames; i++) {
        o->ring[o->ring_pos] = mono[i];
        o->ring_pos = (o->ring_pos + 1) % o->n;
        o->seen++;
        if (--o->until_hop == 0) {
            o->until_hop = o->hop;
            if (o->first_frame_end == 0) o->first_frame_end = o->seen;
            frame_ready(o);
        }
    }
    return o->failed ? FA_ERROR_OUT_OF_MEMORY : FA_OK;
}

/* ---- Envelope to grid ----------------------------------------------------------- */

static float env_at(const float* e, int64_t n, double t)
{
    int64_t i = (int64_t)floor(t);
    double f = t - (double)i;
    float a, b;
    if (i < 0 || i >= n) return 0.0f;
    a = e[i];
    b = i + 1 < n ? e[i + 1] : 0.0f;
    return (float)(a + (b - a) * f);
}

/* The envelope minus its mean over +-half a second, rectified, unit deviation. */
static void normalize_env(float* e, int64_t n, double fr)
{
    int64_t half = (int64_t)(fr * 0.25), i, lo = 0, hi = 0;
    double acc = 0.0, sum2 = 0.0;
    float* out = (float*)malloc((size_t)n * sizeof(float));
    if (!out) return;
    for (i = 0; i < n; i++) {
        int64_t want_hi = i + half + 1 < n ? i + half + 1 : n, want_lo = i - half > 0 ? i - half : 0;
        while (hi < want_hi) acc += e[hi++];
        while (lo < want_lo) acc -= e[lo++];
        out[i] = e[i] - (float)(acc / (double)(hi - lo));
        if (out[i] < 0.0f) out[i] = 0.0f;
        sum2 += (double)out[i] * out[i];
    }
    sum2 = sqrt(sum2 / (double)n);
    for (i = 0; i < n; i++) e[i] = sum2 > 0.0 ? (float)(out[i] / sum2) : 0.0f;
    free(out);
}

/* Autocorrelation for lags 0..maxlag. */
static double* autocorr(const float* e, int64_t n, int64_t maxlag)
{
    double* a = (double*)calloc((size_t)maxlag + 2, sizeof(double));
    int64_t lag, i;
    if (!a) return NULL;
    for (lag = 0; lag <= maxlag + 1 && lag < n; lag++) {
        double s = 0.0;
        for (i = lag; i < n; i++) s += (double)e[i] * e[i - lag];
        a[lag] = s / (double)(n - lag);
    }
    return a;
}

static double ac_at(const double* a, int64_t maxlag, double lag)
{
    int64_t i = (int64_t)floor(lag);
    double f = lag - (double)i;
    if (i < 0 || i + 1 > maxlag + 1) return 0.0;
    return a[i] + (a[i + 1] - a[i]) * f;
}

static double tempo_score(const double* a, int64_t maxlag, double fr, double bpm)
{
    double period = fr * 60.0 / bpm, s = 0.0, lp = log2(bpm / PRIOR_BPM) / PRIOR_OCTAVES;
    int m;
    for (m = 1; m <= 4; m++) s += ac_at(a, maxlag, period * m) / m;
    return s * exp(-0.5 * lp * lp);
}

#define TEMPO_CANDIDATES 3

/* The best tempos in [lo, hi] (the highest peaks of the score, at least half the
 * best, apart by more than 3%), best first; returns how many. */
static int find_tempos(const float* e, int64_t n, double fr, double lo, double hi, double* out)
{
    int64_t maxlag = (int64_t)ceil(4.0 * fr * 60.0 / lo) + 2, steps, i;
    double *a, *sc, ps[TEMPO_CANDIDATES];
    int found = 0, c, d;
    if (maxlag >= n) maxlag = n - 1;
    if (maxlag < 4) return 0;
    steps = (int64_t)floor((hi - lo) / 0.05 + 1e-9) + 1;
    a = autocorr(e, n, maxlag);
    sc = (double*)malloc((size_t)steps * sizeof(double));
    if (!a || !sc) { free(a); free(sc); return 0; }
    for (i = 0; i < steps; i++) sc[i] = tempo_score(a, maxlag, fr, lo + 0.05 * (double)i);
    for (i = 0; i < steps; i++) {
        double bpm = lo + 0.05 * (double)i, v = sc[i];
        int near = 0;
        if ((i > 0 && sc[i - 1] >= v) || (i + 1 < steps && sc[i + 1] > v) || v <= 0.0) continue;   /* not a peak */
        for (c = 0; c < found; c++)
            if (fabs(out[c] - bpm) < 0.03 * bpm) {
                near = 1;
                if (v > ps[c]) { out[c] = bpm; ps[c] = v; }
            }
        if (near) continue;
        for (c = 0; c < found && ps[c] >= v; c++) {}
        if (c >= TEMPO_CANDIDATES) continue;
        if (found < TEMPO_CANDIDATES) found++;
        for (d = found - 1; d > c; d--) { out[d] = out[d - 1]; ps[d] = ps[d - 1]; }
        out[c] = bpm;
        ps[c] = v;
    }
    /* Re-sort after merges, and keep those within half of the best. */
    for (c = 1; c < found; c++)
        for (d = c; d > 0 && ps[d] > ps[d - 1]; d--) {
            double t = ps[d]; ps[d] = ps[d - 1]; ps[d - 1] = t;
            t = out[d]; out[d] = out[d - 1]; out[d - 1] = t;
        }
    while (found > 1 && ps[found - 1] < 0.5 * ps[0]) found--;
    free(a);
    free(sc);
    return found;
}

/* Ellis' dynamic programming beat tracker: beat frames into `beats` (malloc'ed),
 * returns how many. */
static int64_t track_beats(const float* e, int64_t n, double period, double** out)
{
    double* score = (double*)malloc((size_t)n * sizeof(double));
    int64_t* back = (int64_t*)malloc((size_t)n * sizeof(int64_t));
    double* beats = NULL;
    int64_t t, nb = 0, end, i;
    int64_t wlo = (int64_t)floor(period / 2.0), whi = (int64_t)ceil(period * 2.0);
    double best;
    *out = NULL;
    if (!score || !back) { free(score); free(back); return 0; }
    for (t = 0; t < n; t++) {
        int64_t tau, from = t - whi, to = t - wlo;
        double bs = -1e300;
        int64_t bt = -1;
        if (from < 0) from = 0;
        for (tau = from; tau <= to; tau++) {
            double l = log((double)(t - tau) / period);
            double s = score[tau] - DP_TIGHTNESS * l * l;
            if (s > bs) { bs = s; bt = tau; }
        }
        if (bt >= 0) { score[t] = e[t] + bs; back[t] = bt; }
        else { score[t] = e[t]; back[t] = -1; }
    }
    /* End at the best score in the last period. */
    end = n - 1;
    best = -1e300;
    for (t = n - 1; t >= 0 && t >= n - 1 - (int64_t)period; t--)
        if (score[t] > best) { best = score[t]; end = t; }
    for (t = end; t >= 0; t = back[t]) nb++;
    beats = (double*)malloc((size_t)(nb > 0 ? nb : 1) * sizeof(double));
    if (beats) {
        i = nb;
        for (t = end; t >= 0; t = back[t]) beats[--i] = (double)t;
    } else {
        nb = 0;
    }
    free(score);
    free(back);
    *out = beats;
    return nb;
}

/* A beat moved to its onset peak (within a quarter period), to a fraction of a frame. */
static double refine_beat(const float* e, int64_t n, double t, double period)
{
    int64_t c = (int64_t)llround(t), r = (int64_t)(period / 4.0), i, best;
    double a, b, d;
    if (r < 1) r = 1;
    if (c < 0) c = 0;
    if (c > n - 1) c = n - 1;
    best = c;
    for (i = c - r; i <= c + r; i++)
        if (i >= 0 && i < n && e[i] > e[best]) best = i;
    if (best <= 0 || best >= n - 1) return (double)best;
    a = e[best - 1];
    b = e[best + 1];
    d = a - 2.0 * e[best] + b;
    return d < 0.0 ? (double)best + 0.5 * (a - b) / d : (double)best;
}

/* Drops the beats the tracker carried through a quiet intro or ending: leading and
 * trailing beats on onsets weaker than a fifth of the average beat's. */
static int64_t trim_beats(const float* e, int64_t n, double* b, int64_t nb)
{
    double avg = 0.0;
    int64_t i, lo = 0, hi = nb;
    for (i = 0; i < nb; i++) avg += env_at(e, n, b[i]);
    avg /= (double)(nb > 0 ? nb : 1);
    while (lo < hi && env_at(e, n, b[lo]) < 0.2 * avg) lo++;
    while (hi > lo && env_at(e, n, b[hi - 1]) < 0.2 * avg) hi--;
    if (lo) memmove(b, b + lo, (size_t)(hi - lo) * sizeof(double));
    return hi - lo;
}

typedef struct line_fit { double a, p; int64_t inliers; } line_fit;

/* Fits beat = a + idx * p to the beats (index from the period), dropping outliers. */
static line_fit fit_grid(const double* b, int64_t nb, double period, int64_t* idx, int* keep)
{
    line_fit f;
    int64_t i;
    int pass;
    double tol;
    f.a = b[0];
    f.p = period;
    f.inliers = 0;
    idx[0] = 0;
    for (i = 1; i < nb; i++) {
        int64_t step = (int64_t)llround((b[i] - b[i - 1]) / period);
        idx[i] = idx[i - 1] + (step < 1 ? 1 : step);
    }
    for (i = 0; i < nb; i++) keep[i] = 1;
    for (pass = 0; pass < 4; pass++) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0, m = 0, den;
        for (i = 0; i < nb; i++) {
            if (!keep[i]) continue;
            sx += (double)idx[i]; sy += b[i];
            sxx += (double)idx[i] * idx[i]; sxy += (double)idx[i] * b[i];
            m += 1.0;
        }
        den = m * sxx - sx * sx;
        if (m < 2 || den <= 0.0) break;
        f.p = (m * sxy - sx * sy) / den;
        f.a = (sy - f.p * sx) / m;
        /* Re-number from the fit (a skipped or doubled beat), then drop what is off. */
        for (i = 0; i < nb; i++) idx[i] = (int64_t)llround((b[i] - f.a) / f.p);
        tol = pass < 2 ? 0.15 * f.p : 0.08 * f.p;
        for (i = 0; i < nb; i++) keep[i] = fabs(b[i] - (f.a + idx[i] * f.p)) <= tol;
    }
    for (i = 0; i < nb; i++) f.inliers += keep[i];
    return f;
}

/* The best of the envelope's frames around t: beats fall between frames. */
static float peak_near(const float* e, int64_t n, double t)
{
    float v = env_at(e, n, t), v1 = env_at(e, n, t - 0.5), v2 = env_at(e, n, t + 0.5);
    if (v1 > v) v = v1;
    if (v2 > v) v = v2;
    return v;
}

/* Beats tracked at one tempo, fitted to a constant grid. */
typedef struct grid_try {
    double bpm;
    double* beats;
    int64_t nb, first;           /* beats, and the first one on the grid */
    int64_t* idx;
    int* keep;
    line_fit f;
} grid_try;

static void try_free(grid_try* g)
{
    free(g->beats);
    free(g->idx);
    free(g->keep);
    memset(g, 0, sizeof(*g));
}

static int try_tempo(const float* e, int64_t n, double fr, double bpm, grid_try* g)
{
    double period = fr * 60.0 / bpm;
    int64_t i;
    memset(g, 0, sizeof(*g));
    g->bpm = bpm;
    g->first = -1;
    g->nb = track_beats(e, n, period, &g->beats);
    if (g->nb < MIN_BEATS) return 0;
    for (i = 0; i < g->nb; i++) g->beats[i] = refine_beat(e, n, g->beats[i], period);
    g->nb = trim_beats(e, n, g->beats, g->nb);
    if (g->nb < MIN_BEATS) return 0;
    g->idx = (int64_t*)malloc((size_t)g->nb * sizeof(int64_t));
    g->keep = (int*)malloc((size_t)g->nb * sizeof(int));
    if (!g->idx || !g->keep) return 0;
    g->f = fit_grid(g->beats, g->nb, period, g->idx, g->keep);
    for (i = 0; i < g->nb && g->first < 0; i++) if (g->keep[i]) g->first = i;
    return g->f.inliers >= MIN_BEATS && g->f.p > 0.0 && g->first >= 0;
}

/* Mean onset on the grid's beats, times the squared share of tracked beats on it. */
static double grid_strength(const float* e, int64_t n, const grid_try* g)
{
    double k, s = 0.0, share = (double)g->f.inliers / (double)g->nb;
    int64_t c = 0;
    for (k = g->f.a + g->idx[g->first] * g->f.p; k <= g->beats[g->nb - 1] + 0.5; k += g->f.p) {
        s += peak_near(e, n, k);
        c++;
    }
    return c ? s / (double)c * share * share : 0.0;
}

/* Mean onset on the grid's even and odd beats (counting from its first). */
static void parity_strength(const float* e, int64_t n, const grid_try* g, double* even, double* odd)
{
    double k, s[2] = { 0, 0 };
    int64_t c[2] = { 0, 0 }, j = 0;
    for (k = g->f.a + g->idx[g->first] * g->f.p; k <= g->beats[g->nb - 1] + 0.5; k += g->f.p, j++) {
        s[j & 1] += peak_near(e, n, k);
        c[j & 1]++;
    }
    *even = c[0] ? s[0] / (double)c[0] : 0.0;
    *odd = c[1] ? s[1] / (double)c[1] : 0.0;
}

int fa_onset_grid(fa_onset* o, double min_bpm, double max_bpm, double tag_bpm, fa_grid_data* out)
{
    double fr = (double)o->rate / o->hop, bpm, mean = 0.0, on = 0.0, conf, share, contrast, frame0, k, slow = 0.0;
    double cands[TEMPO_CANDIDATES];
    int64_t n = o->nenv, i, cnt = 0, from, to;
    int nc;
    grid_try g;
    fa_frames first_beat;
    memset(out, 0, sizeof(*out));
    if (o->failed || n < (int64_t)(fr * 4.0)) return 0;
    /* Both envelopes count the same: the low one is what makes kicks the beat. */
    normalize_env(o->env, n, fr);
    normalize_env(o->low, n, fr);
    for (i = 0; i < n; i++) o->env[i] += o->low[i];
    /* Among the strongest tempos, the one whose tracked grid lands on the strongest
     * onsets (a tempo 3:2 off the true one lands on every other beat and between). */
    nc = find_tempos(o->env, n, fr, min_bpm, max_bpm, cands);
    if (nc == 0) return 0;
    bpm = cands[0];
    if (nc > 1) {
        double best_c = -1.0;
        int c;
        for (c = 0; c < nc; c++) {
            grid_try t;
            if (try_tempo(o->env, n, fr, cands[c], &t)) {
                double v = grid_strength(o->env, n, &t);
                if (getenv("FA_BEAT_DEBUG")) fprintf(stderr, "candidate %.2f: %.3f\n", cands[c], v);
                if (v > best_c) { best_c = v; bpm = cands[c]; }
            }
            try_free(&t);
        }
    }
    /* Half or double: when both are in range, track the faster; if every other beat
     * of it is much weaker, the slower one is the beat (kicks on the beat, hats in
     * between), else the faster (a backbeat on 2 and 4 is as strong as 1 and 3). */
    if (bpm / 2.0 >= min_bpm) { slow = bpm / 2.0; }
    else if (bpm * 2.0 <= max_bpm) { slow = bpm; bpm *= 2.0; }
    if (!try_tempo(o->env, n, fr, bpm, &g)) {
        try_free(&g);
        if (slow <= 0.0) return 0;
        bpm = slow;
        slow = 0.0;
        if (!try_tempo(o->env, n, fr, bpm, &g)) { try_free(&g); return 0; }
    }
    if (slow > 0.0) {
        double even, odd, weak, strong;
        int use_slow;
        parity_strength(o->env, n, &g, &even, &odd);
        weak = even < odd ? even : odd;
        strong = even < odd ? odd : even;
        use_slow = weak < ONSET_HALF_RATIO * strong;
        if (tag_bpm > 0.0) {
            /* A tag decides between the two (the tempo is measured, the tag only chooses). */
            if (fabs(slow - tag_bpm) < 0.04 * tag_bpm) use_slow = 1;
            else if (fabs(bpm - tag_bpm) < 0.04 * tag_bpm) use_slow = 0;
        }
        if (getenv("FA_BEAT_DEBUG")) fprintf(stderr, "octave %.2f/%.2f: even %.3f odd %.3f -> %s\n", slow, bpm, even, odd, use_slow ? "slow" : "fast");
        if (use_slow) {
            grid_try h;
            if (try_tempo(o->env, n, fr, slow, &h)) { try_free(&g); g = h; }
            else try_free(&h);
        }
    }
    share = (double)g.f.inliers / (double)g.nb;

    /* How much the onsets on the grid stand out from the rest (the envelope has unit
     * deviation per band), over the span the beats cover. */
    from = (int64_t)g.beats[g.first];
    to = (int64_t)g.beats[g.nb - 1];
    for (i = from; i <= to && i < n; i++) mean += o->env[i];
    mean /= (double)(to - from + 1);
    for (k = g.f.a + g.idx[g.first] * g.f.p; k <= g.beats[g.nb - 1] + 0.5; k += g.f.p) {
        on += peak_near(o->env, n, k);
        cnt++;
    }
    contrast = cnt ? (on / (double)cnt - mean) / 2.0 : 0.0;
    conf = share * share * (contrast <= 0.0 ? 0.0 : contrast >= 2.0 ? 1.0 : contrast / 2.0);

    /* Envelope frame t is the window ending at sample first_frame_end - 1 + t * hop;
     * an attack's flux peaks about ONSET_DELAY windows after it enters. */
    frame0 = (double)o->first_frame_end - 1.0 - ONSET_DELAY * o->n;
    first_beat = (fa_frames)llround(frame0 + (g.f.a + g.idx[g.first] * g.f.p) * o->hop);
    while (first_beat < 0) first_beat += (fa_frames)llround(g.f.p * o->hop);

    out->segs = (fa_tempo_segment*)malloc(sizeof(fa_tempo_segment));
    if (!out->segs) { try_free(&g); return 0; }
    out->grid.struct_size = sizeof(fa_beat_grid);
    out->grid.sample_rate = o->rate;
    out->grid.first_beat = first_beat;
    out->grid.beats_per_bar = 4;
    out->grid.bar_offset = 0;
    out->grid.confidence = (float)conf;
    out->grid.flags = FA_GRID_CONSTANT;
    out->grid.segment_count = 1;
    out->segs[0].start = first_beat;
    out->segs[0].bpm = fr * 60.0 / g.f.p;
    out->segs[0].beat = 0.0;
    out->grid.bpm = out->segs[0].bpm;
    try_free(&g);
    return 1;
}
