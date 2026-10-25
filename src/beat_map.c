/* Tempo maps from tracked beats, for songs whose tempo changes or drifts, and bars.
 *
 * The beat tracker (beat.c) follows a drifting tempo; this turns its beats into a
 * few stretches of one tempo each, joined end to end:
 *
 * 1. Beats are numbered by the local period (the median of the last 8 single-beat
 *    gaps), so a beat the tracker skipped in a break still counts.
 * 2. Single beats far off their neighbours' line (a beat caught on a hat or a
 *    fill) are dropped: off by more than 5 noise deviations and 6% of a beat, and
 *    more than 1.5 times either neighbour (a tempo change moves its corner beat
 *    alone, but by little).
 * 3. The noise (a drummer's timing, the onset estimate's own scatter) is measured
 *    from how far each beat is from the line through its neighbours (median
 *    absolute deviation), with a floor of 1.5 ms.
 * 4. The beats are cut into stretches by optimal segmentation of beat time over
 *    beat number into straight lines (least squares), with a penalty of 3 noise
 *    variances x ln(beats) per stretch (a BIC) and stretches of 8 beats or more,
 *    found with PELT (Killick, Fearnhead and Eckley 2012), which prunes the search
 *    to about linear time. A steady song is one stretch: no map.
 * 5. The stretches are joined: each starts where its own line crosses the previous
 *    one's, when they cross between the two stretches' beats (the tempo changed
 *    there), else halfway between the two lines at its first beat; it runs at the
 *    tempo that reaches the next start, so the map is continuous. The last keeps its
 *    own line's tempo. Beat numbers at a start are then not always whole.
 *
 * Bars: the beat of every 4 (or 3) that carries the most harmony change and low
 * accent, scaled to count the same (chords change and kicks land on the first beat
 * of a bar in most music), when it stands out clearly. */
#include "fa_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAP_MIN_BEATS 8          /* shortest stretch */
#define MAP_NOISE_FLOOR_S 0.0015
#define MAP_PENALTY 3.0          /* noise variances x ln(beats), per stretch */
#define MAP_OUT_SIGMAS 5.0
#define MAP_OUT_PERIODS 0.06

typedef struct sums { double n, x, y, xx, xy, yy; } sums;

static void sums_add(sums* s, const sums* a, double x, double y)
{
    s->n = a->n + 1.0;
    s->x = a->x + x;
    s->y = a->y + y;
    s->xx = a->xx + x * x;
    s->xy = a->xy + x * y;
    s->yy = a->yy + y * y;
}

/* Least-squares line over points i..j-1 from the prefix sums: its squared error,
 * and slope and intercept when asked. */
static double line_fit(const sums* p, int64_t i, int64_t j, double* slope, double* icept)
{
    double n = p[j].n - p[i].n, x = p[j].x - p[i].x, y = p[j].y - p[i].y;
    double cxx = (p[j].xx - p[i].xx) - x * x / n, cxy = (p[j].xy - p[i].xy) - x * y / n, cyy = (p[j].yy - p[i].yy) - y * y / n;
    double b = cxx > 0.0 ? cxy / cxx : 0.0, e = cyy - b * cxy;
    if (slope) *slope = b;
    if (icept) *icept = (y - b * x) / n;
    return e > 0.0 ? e : 0.0;
}

/* How far beat i is from the line through the kept beats around it (0 at the ends). */
static double off_line(const double* t, const double* num, const int* keep, int64_t nb, int64_t i, int64_t* pl, int64_t* pr)
{
    int64_t l = i - 1, r = i + 1;
    while (l >= 0 && !keep[l]) l--;
    while (r < nb && !keep[r]) r++;
    *pl = l;
    *pr = r;
    if (l < 0 || r >= nb) return 0.0;
    return t[i] - (t[l] + (t[r] - t[l]) * (num[i] - num[l]) / (num[r] - num[l]));
}

static double median(double* v, int64_t n)
{
    /* Small arrays only in the hot path (8 gaps); a sort is fine for the rest. */
    int64_t i, j;
    for (i = 1; i < n; i++) {
        double x = v[i];
        for (j = i; j > 0 && v[j - 1] > x; j--) v[j] = v[j - 1];
        v[j] = x;
    }
    return n ? (n & 1 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2])) : 0.0;
}

static int cmp_double(const void* a, const void* b)
{
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

int fa_beat_map(const double* beats, int64_t nb, double period, double fr, fa_beat_seg** out_segs)
{
    double *num = NULL, *t = NULL, *res = NULL, *F = NULL, gaps[8], noise, beta, p0;
    int64_t *last = NULL, *cand = NULL, i, k, n = 0, ng = 0, ncand, nseg = 0, *cut = NULL;
    int* keep = NULL;
    sums* ps = NULL;
    fa_beat_seg* segs = NULL;
    int result = -1;
    *out_segs = NULL;
    if (nb < 2 * MAP_MIN_BEATS || !(period > 0.0)) return 0;
    num = (double*)malloc((size_t)nb * sizeof(double));
    keep = (int*)malloc((size_t)nb * sizeof(int));
    res = (double*)malloc((size_t)nb * sizeof(double));
    if (!num || !keep || !res) goto done;

    /* 1. Numbering by the local period. */
    num[0] = 0.0;
    keep[0] = 1;
    for (i = 1; i < nb; i++) {
        double p = period, d = beats[i] - beats[i - 1], step;
        if (ng > 0) {
            double tmp[8];
            memcpy(tmp, gaps, (size_t)(ng < 8 ? ng : 8) * sizeof(double));
            p = median(tmp, ng < 8 ? ng : 8);
        }
        step = floor(d / p + 0.5);
        if (step < 1.0) step = 1.0;
        num[i] = num[i - 1] + step;
        keep[i] = 1;
        if (step == 1.0) gaps[ng++ % 8] = d;
    }

    /* 3 (first, for 2): the noise, from the beats' distance to their neighbours' line.
     * For timing scatter s that distance has deviation s * sqrt(3/2). */
    for (i = 0, k = 0; i < nb; i++) {
        int64_t l, r;
        double e = off_line(beats, num, keep, nb, i, &l, &r);
        if (l >= 0 && r < nb) res[k++] = fabs(e);
    }
    qsort(res, (size_t)k, sizeof(double), cmp_double);
    noise = 1.4826 * median(res, k) / sqrt(1.5);
    if (noise < MAP_NOISE_FLOOR_S * fr) noise = MAP_NOISE_FLOOR_S * fr;

    /* 2. Lone outliers, worst first. */
    for (k = 0; k < nb / 8; k++) {
        double worst = 0.0;
        int64_t wi = -1;
        for (i = 0; i < nb; i++) {
            int64_t l, r, l2, r2;
            double e;
            if (!keep[i]) continue;
            e = fabs(off_line(beats, num, keep, nb, i, &l, &r));
            if (e <= MAP_OUT_SIGMAS * noise || e <= MAP_OUT_PERIODS * period || e <= worst) continue;
            /* alone: well past both neighbours' own distances */
            if (l >= 0 && 1.5 * fabs(off_line(beats, num, keep, nb, l, &l2, &r2)) >= e) continue;
            if (r < nb && 1.5 * fabs(off_line(beats, num, keep, nb, r, &l2, &r2)) >= e) continue;
            worst = e;
            wi = i;
        }
        if (wi < 0) break;
        keep[wi] = 0;
    }

    /* The kept beats, detrended by the typical period (the fit is the same, the sums
     * stay small). */
    t = (double*)malloc((size_t)nb * sizeof(double));
    ps = (sums*)calloc((size_t)nb + 1, sizeof(sums));
    if (!t || !ps) goto done;
    for (i = 0; i < nb; i++)
        if (keep[i]) {
            num[n] = num[i];
            t[n] = beats[i];
            n++;
        }
    p0 = n > 1 ? (t[n - 1] - t[0]) / (num[n - 1] - num[0]) : period;
    for (i = 0; i < n; i++) sums_add(&ps[i + 1], &ps[i], num[i] - num[0], t[i] - t[0] - (num[i] - num[0]) * p0);
    if (n < 2 * MAP_MIN_BEATS) { result = 0; goto done; }

    /* 4. PELT: F[j] is the best cost of beats 0..j-1; last[j] where its stretch starts. */
    beta = MAP_PENALTY * noise * noise * log((double)n);
    F = (double*)malloc(((size_t)n + 1) * sizeof(double));
    last = (int64_t*)malloc(((size_t)n + 1) * sizeof(int64_t));
    cand = (int64_t*)malloc(((size_t)n + 1) * sizeof(int64_t));
    if (!F || !last || !cand) goto done;
    F[0] = -beta;
    last[0] = 0;
    cand[0] = 0;
    ncand = 1;
    for (i = 1; i <= n; i++) {
        double best = HUGE_VAL;
        int64_t bs = -1, c, w = 0, q;
        for (c = 0; c < ncand; c++) {
            int64_t s = cand[c];
            double v;
            if (i - s < MAP_MIN_BEATS || F[s] == HUGE_VAL) continue;
            v = F[s] + line_fit(ps, s, i, NULL, NULL) + beta;
            if (v < best) { best = v; bs = s; }
        }
        F[i] = best;
        last[i] = bs;
        /* Prune starts that can never win again (costs only grow as a stretch grows).
         * With a shortest stretch, s is beaten only by a start q that can itself start a
         * stretch ending at every later i: q = i - MAP_MIN_BEATS + 1, not i. */
        q = i - MAP_MIN_BEATS + 1;
        for (c = 0; c < ncand; c++) {
            int64_t s = cand[c];
            if (q > s && F[s] != HUGE_VAL && F[q] != HUGE_VAL && F[s] + line_fit(ps, s, q, NULL, NULL) > F[q])
                continue;
            cand[w++] = s;
        }
        ncand = w;
        if (best != HUGE_VAL) cand[ncand++] = i;
    }
    if (F[n] == HUGE_VAL) { result = 0; goto done; }
    for (i = n; i > 0; i = last[i]) nseg++;
    if (nseg <= 1) { result = 0; goto done; }

    /* 5. Join the stretches. */
    cut = (int64_t*)malloc(((size_t)nseg + 1) * sizeof(int64_t));
    segs = (fa_beat_seg*)malloc((size_t)nseg * sizeof(fa_beat_seg));
    if (!cut || !segs) goto done;
    cut[nseg] = n;
    for (i = n, k = nseg; i > 0; i = last[i]) cut[--k] = last[i];
    {
        double ps_slope = 0.0, ps_icept = 0.0;
        for (k = 0; k < nseg; k++) {
            double slope, icept, x0 = num[cut[k]] - num[0], at;
            line_fit(ps, cut[k], cut[k + 1], &slope, &icept);
            at = icept + slope * x0;
            if (k > 0) {
                /* Where the two lines cross, when that is between the stretches' beats
                 * (a tempo change); else halfway between them at the first beat. */
                double xp = num[cut[k] - 1] - num[0], xc = slope != ps_slope ? (ps_icept - icept) / (slope - ps_slope) : -1.0;
                if (xc >= xp && xc <= x0) {
                    x0 = xc;
                    at = icept + slope * x0;
                } else {
                    at = 0.5 * (at + ps_icept + ps_slope * x0);
                }
            }
            segs[k].beat = x0;
            segs[k].t = t[0] + at + x0 * p0;
            segs[k].period = slope + p0;
            ps_slope = slope;
            ps_icept = icept;
        }
    }
    for (k = 0; k + 1 < nseg; k++) {
        double p = (segs[k + 1].t - segs[k].t) / (segs[k + 1].beat - segs[k].beat);
        if (!(p > 0.0)) { result = 0; goto done; }
        segs[k].period = p;
    }
    if (!(segs[nseg - 1].period > 0.0)) { result = 0; goto done; }
    *out_segs = segs;
    segs = NULL;
    result = (int)nseg;
done:
    free(num);
    free(keep);
    free(res);
    free(t);
    free(ps);
    free(F);
    free(last);
    free(cand);
    free(cut);
    free(segs);
    return result;
}

double fa_beat_map_time(const fa_beat_seg* s, int n, double beat)
{
    int lo = 0, hi = n;
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (s[mid].beat <= beat) lo = mid;
        else hi = mid;
    }
    return s[lo].t + (beat - s[lo].beat) * s[lo].period;
}

double fa_beat_map_beat(const fa_beat_seg* s, int n, double t)
{
    int lo = 0, hi = n;
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (s[mid].t <= t) lo = mid;
        else hi = mid;
    }
    return s[lo].beat + (t - s[lo].t) / s[lo].period;
}

/* ---- Bars ---------------------------------------------------------------------------- */

#define BAR_CLEAR 1.4      /* the bar's first beat this much above the others' mean */
#define BAR_THREE 1.2      /* three beats a bar only when that much clearer than four */

/* How clearly one beat of every `m` stands out, and which. */
static double bar_contrast(const float* f, int64_t n, int m, uint32_t* phase)
{
    double s[4] = { 0 }, best = -1.0, rest = 0.0;
    int64_t c[4] = { 0 }, k;
    int p, bp = 0;
    for (k = 0; k < n; k++) {
        s[k % m] += f[k];
        c[k % m]++;
    }
    for (p = 0; p < m; p++) {
        s[p] = c[p] ? s[p] / (double)c[p] : 0.0;
        if (s[p] > best) { best = s[p]; bp = p; }
    }
    for (p = 0; p < m; p++) if (p != bp) rest += s[p];
    rest /= (double)(m - 1);
    *phase = (uint32_t)bp;
    return rest > 0.0 ? best / rest : 0.0;
}

int fa_beat_bars(const float* change, const float* accent, int64_t n, uint32_t* beats_per_bar, uint32_t* offset)
{
    double mc = 0.0, ma = 0.0, c3, c4;
    float* f;
    int64_t k;
    uint32_t p3, p4;
    if (n < 16) return 0;
    for (k = 0; k < n; k++) { mc += change[k]; ma += accent[k]; }
    if (!(mc > 0.0) && !(ma > 0.0)) return 0;
    f = (float*)malloc((size_t)n * sizeof(float));
    if (!f) return 0;
    /* Both count the same: each scaled to a mean of 1. */
    for (k = 0; k < n; k++) f[k] = (float)((mc > 0.0 ? change[k] * n / mc : 0.0) + (ma > 0.0 ? accent[k] * n / ma : 0.0));
    c4 = bar_contrast(f, n, 4, &p4);
    c3 = bar_contrast(f, n, 3, &p3);
    free(f);
    if (c3 > BAR_THREE * c4 && c3 >= BAR_CLEAR) {
        *beats_per_bar = 3;
        *offset = p3;
        return 1;
    }
    if (c4 >= BAR_CLEAR) {
        *beats_per_bar = 4;
        *offset = p4;
        return 1;
    }
    return 0;
}
