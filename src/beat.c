/* Tempo and beat grid from audio (clean room, from published methods).
 *
 * 1. Onset strength: log-magnitude spectral flux, a 1024-point FFT every 256
 *    samples at 44.1/48 kHz (scaled with the rate, so the envelope runs at about
 *    170-190 frames a second). Two envelopes are kept, one over all bins up to
 *    11 kHz and one below 200 Hz (where kicks are); each is scaled to unit
 *    standard deviation and the two are summed. Two frames go through one complex
 *    FFT (one as the real part, one as the imaginary part).
 * 2. The envelope minus its local mean (half a second), rectified.
 * 3. Tempo candidates: the envelope's autocorrelation at 1, 2, 3 and 4 beat
 *    periods, weighted 1, 1/2, 1/3, 1/4 (a comb), times a log-normal preference
 *    around 120 BPM, searched in the preferred range in 0.05 BPM steps. The three
 *    best peaks (at least half the best, more than 3% apart) are each tracked and
 *    the one whose grid lands on the strongest onsets wins.
 * 4. Half or double: when both a tempo and its double are in range, the faster is
 *    tracked, and the slower kept when every other beat is clearly weaker (below
 *    0.6 of the others). A BPM tag within 4% picks between half, the same and
 *    double.
 * 5. Beats: dynamic-programming beat tracking (Ellis 2007), each beat refined to
 *    a fraction of a frame on its onset peak; weak beats at the ends are dropped.
 * 6. The constant grid: a robust straight-line fit of beat time over beat number
 *    (outliers dropped and fitted again), which gives the tempo far more finely
 *    than the envelope's frames. The confidence is the share of beats on the grid,
 *    squared, times how much the grid's onsets stand out.
 *
 * Frames of the envelope are placed where an attack lands (calibrated on
 * synthetic kicks and General MIDI renders: the flux of an attack peaks about
 * 0.42 of a window after the attack enters it). */
#include "fa_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ONSET_LOW_HZ 200.0
#define ONSET_MAX_HZ 11000.0
#define ONSET_LOG_GAIN 100.0f
#define PRIOR_BPM 120.0
#define PRIOR_OCTAVES 1.0
#define DP_TIGHTNESS 100.0
#define MIN_BEATS 8
#define ONSET_HALF_RATIO 0.6     /* every other beat this much weaker: half tempo */
#define ONSET_DELAY 0.42         /* attack to flux peak, in windows (calibrated) */
#define CHROMA_LO_HZ 130.0       /* pitch classes for bars: C3 .. */
#define CHROMA_HI_HZ 4000.0      /* ... about B7 */
#define CHROMA_SPAN 12           /* most pitch classes one bin is spread over */
#define CHROMA_STEP 4            /* envelope frames per chroma frame */
#define BAR_MIN_BEATS 32

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
    uint8_t* bin_pc;             /* first pitch class of each bin, 255 = not counted */
    uint8_t* bin_cnt;            /* pitch classes it covers (they follow each other) */
    float* bin_w;                /* their shares of the bin, CHROMA_SPAN per bin, summing to 1 */
    float acc[12];               /* the chroma frame being summed */
    int acc_n;
    float* chroma;               /* 12 per CHROMA_STEP envelope frames */
    int64_t nchroma, chroma_cap;
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
    o->bin_pc = (uint8_t*)malloc(o->bins + 1);
    o->bin_cnt = (uint8_t*)calloc(o->bins + 1, 1);
    o->bin_w = (float*)calloc((size_t)(o->bins + 1) * CHROMA_SPAN, sizeof(float));
    if (o->bin_pc && o->bin_cnt && o->bin_w) {
        uint32_t k;
        for (k = 0; k <= o->bins; k++) {
            double f = k * binhz, lo, hi, tot = 0.0;
            long j0, j1, j;
            o->bin_pc[k] = 255;
            if (f < CHROMA_LO_HZ || f > CHROMA_HI_HZ) continue;
            /* The bin covers f +- half its width; low bins are wider than a semitone,
             * so a chord's low notes would fall on whichever class the bin's centre
             * is nearest. Each semitone cell [j - 0.5, j + 0.5] (in semitones from A440)
             * takes the share of the bin that overlaps it. */
            lo = 12.0 * log2((f - binhz / 2.0 > 1.0 ? f - binhz / 2.0 : 1.0) / 440.0);
            hi = 12.0 * log2((f + binhz / 2.0) / 440.0);
            j0 = (long)floor(lo + 0.5);
            j1 = (long)floor(hi + 0.5);
            if (j1 - j0 >= CHROMA_SPAN) j1 = j0 + CHROMA_SPAN - 1;
            for (j = j0; j <= j1; j++) {
                double a = lo > j - 0.5 ? lo : j - 0.5, b = hi < j + 0.5 ? hi : j + 0.5;
                double w = b > a ? b - a : 0.0;
                o->bin_w[(size_t)k * CHROMA_SPAN + (size_t)(j - j0)] = (float)w;
                tot += w;
            }
            if (tot <= 0.0) { o->bin_w[(size_t)k * CHROMA_SPAN] = 1.0f; tot = 1.0; j1 = j0; }
            for (j = j0; j <= j1; j++) o->bin_w[(size_t)k * CHROMA_SPAN + (size_t)(j - j0)] /= (float)tot;
            o->bin_cnt[k] = (uint8_t)(j1 - j0 + 1);
            o->bin_pc[k] = (uint8_t)(((j0 % 12) + 21) % 12);
        }
    }
    if (!o->tb || !o->ring || !o->pending || !o->re || !o->im || !o->prev || !o->cur || !o->bin_pc || !o->bin_cnt || !o->bin_w) {
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
    free(o->bin_pc);
    free(o->bin_cnt);
    free(o->bin_w);
    free(o->chroma);
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

/* The summed chroma frame onto o->chroma. */
static int chroma_push(fa_onset* o)
{
    if (o->nchroma == o->chroma_cap) {
        int64_t cap = o->chroma_cap ? o->chroma_cap * 2 : 1024;
        float* c = (float*)realloc(o->chroma, (size_t)cap * 12 * sizeof(float));
        if (!c) return 0;
        o->chroma = c;
        o->chroma_cap = cap;
    }
    memcpy(o->chroma + o->nchroma * 12, o->acc, sizeof(o->acc));
    o->nchroma++;
    memset(o->acc, 0, sizeof(o->acc));
    o->acc_n = 0;
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
        if (o->bin_pc[k] < 12) {
            const float* w = o->bin_w + (size_t)k * CHROMA_SPAN;
            unsigned c, pc = o->bin_pc[k];
            for (c = 0; c < o->bin_cnt[k]; c++) o->acc[(pc + c) % 12] += mag * w[c];
        }
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
        if (++o->acc_n == CHROMA_STEP && !o->failed && !chroma_push(o)) o->failed = 1;
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

/* The best tempos in [lo, hi]: the highest peaks of the score, at least half the
 * best, more than 3% apart, best first; returns how many. */
static int find_tempos(const float* e, int64_t n, double fr, double lo, double hi, double* out)
{
    int64_t maxlag = (int64_t)ceil(4.0 * fr * 60.0 / lo) + 2, steps, i;
    double *a, *sc, ps[TEMPO_CANDIDATES];
    int found = 0, c;
    if (maxlag >= n) maxlag = n - 1;
    if (maxlag < 4) return 0;
    steps = (int64_t)floor((hi - lo) / 0.05 + 1e-9) + 1;
    a = autocorr(e, n, maxlag);
    sc = (double*)malloc((size_t)steps * sizeof(double));
    if (!a || !sc) { free(a); free(sc); return 0; }
    for (i = 0; i < steps; i++) sc[i] = tempo_score(a, maxlag, fr, lo + 0.05 * (double)i);
    /* Highest peak first, each time leaving out those near a chosen one. */
    while (found < TEMPO_CANDIDATES) {
        int64_t best = -1;
        for (i = 0; i < steps; i++) {
            double bpm = lo + 0.05 * (double)i;
            int near = 0;
            if (sc[i] <= 0.0 || (i > 0 && sc[i - 1] >= sc[i]) || (i + 1 < steps && sc[i + 1] > sc[i])) continue;
            for (c = 0; c < found && !near; c++) near = fabs(out[c] - bpm) < 0.03 * bpm;
            if (!near && (best < 0 || sc[i] > sc[best])) best = i;
        }
        if (best < 0 || (found > 0 && sc[best] < 0.5 * ps[0])) break;
        out[found] = lo + 0.05 * (double)best;
        ps[found++] = sc[best];
    }
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
    double *beats = NULL, *pen;
    int64_t t, nb = 0, end, i, d;
    int64_t wlo = (int64_t)floor(period / 2.0), whi = (int64_t)ceil(period * 2.0);
    double best;
    *out = NULL;
    if (wlo < 1) wlo = 1;
    pen = (double*)malloc((size_t)(whi + 1) * sizeof(double));
    if (!score || !back || !pen) { free(score); free(back); free(pen); return 0; }
    /* The penalty for a beat d frames after the last, by d. */
    for (d = wlo; d <= whi; d++) {
        double l = log((double)d / period);
        pen[d] = DP_TIGHTNESS * l * l;
    }
    for (t = 0; t < n; t++) {
        int64_t tau, from = t - whi, to = t - wlo;
        double bs = -1e300;
        int64_t bt = -1;
        if (from < 0) from = 0;
        for (tau = from; tau <= to; tau++) {
            double s = score[tau] - pen[t - tau];
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
    free(pen);
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

/* Beats tracked at one tempo, and the grid they give: a constant one fitted to them,
 * or a tempo map when the tempo changes or drifts. Beat 0 of either is the first
 * beat on it. */
typedef struct grid_try {
    double bpm;
    double* beats;
    int64_t nb, first;           /* beats, and the first one on the constant grid */
    int64_t* idx;
    int* keep;
    line_fit f;
    fa_beat_seg* map;            /* NULL: the constant grid */
    int nmap;
} grid_try;

static void try_free(grid_try* g)
{
    free(g->beats);
    free(g->idx);
    free(g->keep);
    free(g->map);
    memset(g, 0, sizeof(*g));
}

/* Envelope frame of a beat of the grid, and the beat of a frame. */
static double try_time(const grid_try* g, double beat)
{
    if (g->map) return fa_beat_map_time(g->map, g->nmap, beat);
    return g->f.a + ((double)g->idx[g->first] + beat) * g->f.p;
}

static double try_beat(const grid_try* g, double t)
{
    if (g->map) return fa_beat_map_beat(g->map, g->nmap, t);
    return (t - g->f.a) / g->f.p - (double)g->idx[g->first];
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
    g->nmap = fa_beat_map(g->beats, g->nb, g->f.p > 0.0 ? g->f.p : period, fr, &g->map);
    if (g->nmap < 0) return 0;   /* out of memory, as for idx and keep */
    if (g->nmap <= 1) { g->map = NULL; g->nmap = 0; }
    if (g->map) return 1;
    return g->f.inliers >= MIN_BEATS && g->f.p > 0.0 && g->first >= 0;
}

/* The tracked beats' agreement with the grid: the share on it (within 8% of a
 * beat), the mean onset on its beats, and the mean envelope, over the span the
 * beats cover. */
static void try_measure(const float* e, int64_t n, const grid_try* g, double* share, double* on, double* mean)
{
    double k, kend, s = 0.0, m = 0.0;
    int64_t i, cnt = 0, hits = 0, from = (int64_t)g->beats[0], to = (int64_t)g->beats[g->nb - 1];
    for (i = 0; i < g->nb; i++) {
        double j = floor(try_beat(g, g->beats[i]) + 0.5);
        double p = try_time(g, j + 0.5) - try_time(g, j - 0.5);
        hits += fabs(g->beats[i] - try_time(g, j)) <= 0.08 * p;
    }
    kend = try_beat(g, g->beats[g->nb - 1] + 0.5);
    for (k = ceil(try_beat(g, g->beats[0] - 0.5)); k <= kend; k += 1.0) {
        s += peak_near(e, n, try_time(g, k));
        cnt++;
    }
    for (i = from; i <= to && i < n; i++) m += e[i];
    *share = (double)hits / (double)g->nb;
    *on = cnt ? s / (double)cnt : 0.0;
    *mean = m / (double)(to - from + 1);
}

/* Mean onset on the grid's beats, times the squared share of tracked beats on it. */
static double grid_strength(const float* e, int64_t n, const grid_try* g)
{
    double share, on, mean;
    try_measure(e, n, g, &share, &on, &mean);
    return on * share * share;
}

/* Mean onset on the grid's even and odd beats. */
static void parity_strength(const float* e, int64_t n, const grid_try* g, double* even, double* odd)
{
    double k, kend = try_beat(g, g->beats[g->nb - 1] + 0.5), s[2] = { 0, 0 };
    int64_t c[2] = { 0, 0 };
    for (k = ceil(try_beat(g, g->beats[0] - 0.5)); k <= kend; k += 1.0) {
        int j = (int)((int64_t)k & 1);
        s[j] += peak_near(e, n, try_time(g, k));
        c[j]++;
    }
    *even = c[0] ? s[0] / (double)c[0] : 0.0;
    *odd = c[1] ? s[1] / (double)c[1] : 0.0;
}

/* Bars: per beat of the grid, how much the harmony changes there (one minus the
 * cosine of the chroma of the two beats before and the two from it: a melody moves
 * within a chord, the chord changes at a bar) and the low-band onset on it;
 * fa_beat_bars picks the meter and the beat that starts a bar. `shift`: grid beat
 * that is beat 0 of the result. */
static void find_bars(const fa_onset* o, const grid_try* g, double shift, fa_beat_grid* grid)
{
    int64_t k0 = (int64_t)ceil(try_beat(g, g->beats[0] - 0.5)), k1 = (int64_t)floor(try_beat(g, g->beats[g->nb - 1] + 0.5));
    int64_t nk = k1 - k0, k, i;
    float *change, *accent;
    double* c;
    uint32_t bpb = 0, off = 0;
    if (nk < BAR_MIN_BEATS || o->nchroma == 0) return;
    change = (float*)malloc((size_t)nk * sizeof(float));
    accent = (float*)malloc((size_t)nk * sizeof(float));
    c = (double*)calloc((size_t)nk * 12, sizeof(double));
    if (!change || !accent || !c) { free(change); free(accent); free(c); return; }
    for (k = 0; k < nk; k++) {
        double t0 = try_time(g, (double)(k0 + k)), t1 = try_time(g, (double)(k0 + k + 1));
        int64_t a = (int64_t)floor(t0 / CHROMA_STEP), b = (int64_t)floor(t1 / CHROMA_STEP);
        int p;
        if (a < 0) a = 0;
        if (b > o->nchroma) b = o->nchroma;
        for (i = a; i < b; i++)
            for (p = 0; p < 12; p++) c[k * 12 + p] += o->chroma[i * 12 + p];
        accent[k] = peak_near(o->low, o->nenv, t0);
    }
    for (k = 0; k < nk; k++) {
        double x = 0.0, y = 0.0, xy = 0.0;
        int p;
        change[k] = 0.0f;
        if (k < 2 || k + 2 > nk) continue;
        for (p = 0; p < 12; p++) {
            double u = c[(k - 2) * 12 + p] + c[(k - 1) * 12 + p], v = c[k * 12 + p] + c[(k + 1) * 12 + p];
            x += u * u;
            y += v * v;
            xy += u * v;
        }
        if (x > 0.0 && y > 0.0) change[k] = (float)(1.0 - xy / sqrt(x * y));
    }
    /* The first two and the last beat have no change measured: left out. */
    if (fa_beat_bars(change + 2, accent + 2, nk - 3, &bpb, &off)) {
        int64_t first = k0 + 2 + (int64_t)off - (int64_t)llround(shift);
        grid->beats_per_bar = bpb;
        grid->bar_offset = (uint32_t)(((first % (int64_t)bpb) + (int64_t)bpb) % (int64_t)bpb);
        grid->flags |= FA_GRID_BARS;
    }
    free(change);
    free(accent);
    free(c);
}

/* The tempo map into `out` (sample frames; frame0 = the sample of envelope frame 0).
 * Beat 0 is the first one in the song: the map may start a beat or two before it,
 * where the tracker ran on from the intro. */
static int map_out(const fa_onset* o, const grid_try* g, double frame0, double conf, fa_grid_data* out)
{
    double fr = (double)o->rate / o->hop, j = 0.0;
    const fa_beat_seg* map = g->map;
    int k, first = 0, nmap = g->nmap;
    while (frame0 + fa_beat_map_time(map, nmap, j) * o->hop < -(double)(o->rate / 200)) j += 1.0;
    while (first + 1 < nmap && map[first + 1].beat <= j) first++;
    /* The output starts at beat j, inside stretch `first`; g->map stays as it is (it
     * is still read by find_bars). */
    map += first;
    nmap -= first;
    out->segs = (fa_tempo_segment*)malloc((size_t)nmap * sizeof(fa_tempo_segment));
    if (!out->segs) return 0;
    for (k = 0; k < nmap; k++) {
        double t = k == 0 ? map[0].t + (j - map[0].beat) * map[0].period : map[k].t;
        fa_frames at = (fa_frames)llround(frame0 + t * o->hop);
        out->segs[k].start = k == 0 && at < 0 ? 0 : at;
        out->segs[k].bpm = fr * 60.0 / map[k].period;
        out->segs[k].beat = k == 0 ? 0.0 : map[k].beat - j;
    }
    out->grid.struct_size = sizeof(fa_beat_grid);
    out->grid.sample_rate = o->rate;
    out->grid.first_beat = out->segs[0].start;
    out->grid.beats_per_bar = 4;
    out->grid.bar_offset = 0;
    out->grid.confidence = (float)conf;
    out->grid.segment_count = (uint32_t)nmap;
    fa_grid_data_finish(out, o->seen);
    find_bars(o, g, j, &out->grid);
    return 1;
}

int fa_onset_grid(fa_onset* o, double min_bpm, double max_bpm, double tag_bpm, fa_grid_data* out)
{
    double fr = (double)o->rate / o->hop, bpm, on, mean, conf, share, contrast, frame0, slow = 0.0, shift = 0.0;
    double cands[TEMPO_CANDIDATES];
    int64_t n = o->nenv, i;
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
        if (use_slow) {
            grid_try h;
            if (try_tempo(o->env, n, fr, slow, &h)) { try_free(&g); g = h; }
            else try_free(&h);
        }
    }

    /* How much the onsets on the grid stand out from the rest (the envelope has unit
     * deviation per band). */
    try_measure(o->env, n, &g, &share, &on, &mean);
    contrast = (on - mean) / 2.0;
    conf = share * share * (contrast <= 0.0 ? 0.0 : contrast >= 2.0 ? 1.0 : contrast / 2.0);

    /* Envelope frame t is the window ending at sample first_frame_end - 1 + t * hop;
     * an attack's flux peaks about ONSET_DELAY windows after it enters. */
    frame0 = (double)o->first_frame_end - 1.0 - ONSET_DELAY * o->n;
    if (g.map) {
        int ok = map_out(o, &g, frame0, conf, out);
        try_free(&g);
        return ok;
    }
    first_beat = (fa_frames)llround(frame0 + try_time(&g, 0.0) * o->hop);
    /* A beat a few ms before the start is the song's first (the estimate's own scatter). */
    if (first_beat < 0 && first_beat > -(fa_frames)(o->rate / 200)) first_beat = 0;
    while (first_beat < 0) {
        first_beat += (fa_frames)llround(g.f.p * o->hop);
        shift += 1.0;
    }

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
    find_bars(o, &g, shift, &out->grid);
    try_free(&g);
    return 1;
}
