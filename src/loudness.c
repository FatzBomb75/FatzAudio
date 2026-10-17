/* Loudness measurement to ITU-R BS.1770-4 / EBU R128, written from the
 * published recommendations (no third-party meter code).
 *
 * The meter K-weights every channel and stores one mean-square energy per
 * 100 ms "sub-block". Everything else is derived from that list:
 * - 400 ms gating blocks (75 % overlap) are the mean of 4 consecutive sub-blocks,
 * - 3 s short-term windows (for loudness range) the mean of 30.
 * Because the list is just concatenated, a file can be measured in chunks on
 * several threads and the chunks' lists joined: the result is the same as a
 * single pass (each chunk warms its filters up on the audio before its start).
 *
 * True peak uses a polyphase 4x oversampler (2x at 88.2/96 kHz, none above). */
#include "fa_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FA_PI 3.14159265358979323846

/* ---- True peak ------------------------------------------------------------- */

static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0, k;
    for (k = 1.0; k < 50.0; k += 1.0) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < sum * 1e-12) break;
    }
    return sum;
}

fa_result fa_truepeak_init(fa_truepeak* tp, uint32_t rate, uint32_t channels)
{
    int p, t, n;
    memset(tp, 0, sizeof(*tp));
    tp->channels = channels;
    tp->phases = rate >= 176400 ? 1 : rate >= 88200 ? 2 : 4;
    tp->hist = (float*)calloc((size_t)channels * FA_TP_TAPS * 2, sizeof(float));
    if (!tp->hist) return FA_ERROR_OUT_OF_MEMORY;
    if (tp->phases == 1) { tp->bound = 1.0f; return FA_OK; }

    /* Kaiser-windowed sinc low-pass at 0.9x the original Nyquist, split into phases.
     * Each phase is normalised to unity DC gain. */
    n = tp->phases * FA_TP_TAPS;
    for (p = 0; p < tp->phases; p++) {
        double sum = 0.0, abs_sum = 0.0;
        double h[FA_TP_TAPS];
        for (t = 0; t < FA_TP_TAPS; t++) {
            int i = t * tp->phases + p;
            double x = (double)i - (double)(n - 1) / 2.0;
            double fc = 0.9 / (2.0 * tp->phases);       /* cycles per oversampled sample */
            double sinc = x == 0.0 ? 2.0 * fc : sin(2.0 * FA_PI * fc * x) / (FA_PI * x);
            double r = 2.0 * (double)i / (double)(n - 1) - 1.0;
            double w = bessel_i0(6.0 * sqrt(1.0 - r * r)) / bessel_i0(6.0);
            h[t] = sinc * w;
            sum += h[t];
        }
        for (t = 0; t < FA_TP_TAPS; t++) {
            h[t] /= sum;
            abs_sum += fabs(h[t]);
            /* Reversed so the dot product runs over the history oldest-first. */
            tp->coef[p][FA_TP_TAPS - 1 - t] = (float)h[t];
        }
        if ((float)abs_sum > tp->bound) tp->bound = (float)abs_sum;
    }
    return FA_OK;
}

void fa_truepeak_free(fa_truepeak* tp)
{
    free(tp->hist);
    tp->hist = NULL;
}

/* Appends one sample per channel and returns the largest |value| among the
 * interpolated points it completes. History per channel is stored twice
 * (positions pos and pos + TAPS) so the newest TAPS samples are contiguous. */
static float tp_push(fa_truepeak* tp, const float* frame)
{
    uint32_t c;
    int p, t;
    float m = 0.0f;
    tp->pos = (tp->pos + 1) % FA_TP_TAPS;
    for (c = 0; c < tp->channels; c++) {
        float* h = tp->hist + (size_t)c * FA_TP_TAPS * 2;
        const float* w;
        h[tp->pos] = h[tp->pos + FA_TP_TAPS] = frame[c];
        w = h + tp->pos + 1;
        if (tp->phases == 1) {
            float a = fabsf(frame[c]);
            if (a > m) m = a;
            continue;
        }
        for (p = 0; p < tp->phases; p++) {
            float acc = 0.0f;
            for (t = 0; t < FA_TP_TAPS; t++) acc += tp->coef[p][t] * w[t];
            acc = fabsf(acc);
            if (acc > m) m = acc;
        }
    }
    return m;
}

float fa_truepeak_push(fa_truepeak* tp, const float* frame) { return tp_push(tp, frame); }

/* Measures `frames` interleaved frames. With `measure` 0 the samples only fill
 * the history (warm-up). A block that cannot beat the peak found so far is
 * skipped: no interpolated point can exceed `bound` x the largest input sample
 * it is computed from, so the skip never changes the result. */
static void tp_process(fa_truepeak* tp, const float* x, int64_t frames, int measure)
{
    uint32_t ch = tp->channels;
    int64_t i;
    float in_max = 0.0f;
    if (measure) {
        size_t k, n = (size_t)frames * ch;
        for (k = 0; k < n; k++) { float a = fabsf(x[k]); if (a > in_max) in_max = a; }
        for (k = 0; k < (size_t)ch * FA_TP_TAPS * 2; k++) { float a = fabsf(tp->hist[k]); if (a > in_max) in_max = a; }
    }
    if (!measure || in_max * tp->bound <= tp->peak) {
        /* Only the last TAPS frames matter for what comes next. */
        i = frames > FA_TP_TAPS ? frames - FA_TP_TAPS : 0;
        for (; i < frames; i++) {
            uint32_t c;
            tp->pos = (tp->pos + 1) % FA_TP_TAPS;
            for (c = 0; c < ch; c++) {
                float* h = tp->hist + (size_t)c * FA_TP_TAPS * 2;
                h[tp->pos] = h[tp->pos + FA_TP_TAPS] = x[(size_t)i * ch + c];
            }
        }
        return;
    }
    for (i = 0; i < frames; i++) {
        float m = tp_push(tp, x + (size_t)i * ch);
        if (m > tp->peak) tp->peak = m;
    }
}

/* ---- K-weighting ----------------------------------------------------------- */

/* The two BS.1770 pre-filter stages (high shelf, then RLB high-pass), derived
 * for any sample rate from their analogue prototypes via the bilinear transform. */
static void k_weighting(double rate, double b[2][3], double a[2][3])
{
    double f0 = 1681.974450955533, g = 3.999843853973347, q = 0.7071752369554196;
    double k = tan(FA_PI * f0 / rate);
    double vh = pow(10.0, g / 20.0);
    double vb = pow(vh, 0.4996667741545416);
    double a0 = 1.0 + k / q + k * k;
    b[0][0] = (vh + vb * k / q + k * k) / a0;
    b[0][1] = 2.0 * (k * k - vh) / a0;
    b[0][2] = (vh - vb * k / q + k * k) / a0;
    a[0][0] = 1.0;
    a[0][1] = 2.0 * (k * k - 1.0) / a0;
    a[0][2] = (1.0 - k / q + k * k) / a0;

    f0 = 38.13547087602444; q = 0.5003270373238773;
    k = tan(FA_PI * f0 / rate);
    a0 = 1.0 + k / q + k * k;
    b[1][0] = 1.0; b[1][1] = -2.0; b[1][2] = 1.0;
    a[1][0] = 1.0;
    a[1][1] = 2.0 * (k * k - 1.0) / a0;
    a[1][2] = (1.0 - k / q + k * k) / a0;
}

/* BS.1770 channel weights: 1.0 for front channels, 1.41 for surrounds, LFE
 * excluded. Layouts follow the usual WAVE/FLAC/Vorbis orders. */
static double channel_weight(uint32_t channels, uint32_t c)
{
    if (channels == 6) { static const double w[6] = { 1, 1, 1, 0, 1.41, 1.41 }; return w[c]; }
    if (channels == 5) { static const double w[5] = { 1, 1, 1, 1.41, 1.41 }; return w[c]; }
    if (channels == 8) { static const double w[8] = { 1, 1, 1, 0, 1.41, 1.41, 1.41, 1.41 }; return w[c]; }
    return 1.0;
}

fa_result fa_meter_init(fa_meter* m, uint32_t rate, uint32_t channels, int with_peak, int64_t ring_subs)
{
    uint32_t c;
    memset(m, 0, sizeof(*m));
    if (rate == 0 || channels == 0) return FA_ERROR_INVALID_ARG;
    m->rate = rate;
    m->channels = channels;
    m->sublen = (rate + 5) / 10;
    k_weighting((double)rate, m->b, m->a);
    m->z = (double*)calloc((size_t)channels * 4, sizeof(double));
    m->weight = (double*)calloc(channels, sizeof(double));
    m->ring = ring_subs > 0;
    m->cap = m->ring ? ring_subs : 256;
    m->subs = (double*)malloc((size_t)m->cap * sizeof(double));
    if (!m->z || !m->weight || !m->subs) { fa_meter_free(m); return FA_ERROR_OUT_OF_MEMORY; }
    for (c = 0; c < channels; c++) m->weight[c] = channel_weight(channels, c);
    if (with_peak) {
        if (fa_truepeak_init(&m->tp, rate, channels) != FA_OK) { fa_meter_free(m); return FA_ERROR_OUT_OF_MEMORY; }
        m->with_peak = 1;
    }
    return FA_OK;
}

void fa_meter_free(fa_meter* m)
{
    free(m->z);
    free(m->weight);
    free(m->subs);
    if (m->with_peak) fa_truepeak_free(&m->tp);
    memset(m, 0, sizeof(*m));
}

void fa_meter_reset(fa_meter* m)
{
    memset(m->z, 0, (size_t)m->channels * 4 * sizeof(double));
    m->subfill = 0;
    m->subacc = 0.0;
    m->nsubs = 0;
}

static int push_sub(fa_meter* m, double energy)
{
    if (m->ring) {
        m->subs[m->nsubs % m->cap] = energy;
        m->nsubs++;
        return 1;
    }
    if (m->nsubs == m->cap) {
        double* p = (double*)realloc(m->subs, (size_t)m->cap * 2 * sizeof(double));
        if (!p) return 0;
        m->subs = p;
        m->cap *= 2;
    }
    m->subs[m->nsubs++] = energy;
    return 1;
}

/* Runs both filter stages over n frames of channel c (stride = channels) and
 * returns the sum of squares of the output (direct form II transposed). */
static double filter_channel(fa_meter* m, const float* x, int64_t n, uint32_t c)
{
    double* z = m->z + (size_t)c * 4;
    double z0 = z[0], z1 = z[1], z2 = z[2], z3 = z[3], acc = 0.0;
    const double b00 = m->b[0][0], b01 = m->b[0][1], b02 = m->b[0][2], a01 = m->a[0][1], a02 = m->a[0][2];
    const double b10 = m->b[1][0], b11 = m->b[1][1], b12 = m->b[1][2], a11 = m->a[1][1], a12 = m->a[1][2];
    uint32_t stride = m->channels;
    int64_t i;
    for (i = 0; i < n; i++) {
        double in = (double)x[(size_t)i * stride + c];
        double y1 = b00 * in + z0;
        double y2;
        z0 = b01 * in - a01 * y1 + z1;
        z1 = b02 * in - a02 * y1;
        y2 = b10 * y1 + z2;
        z2 = b11 * y1 - a11 * y2 + z3;
        z3 = b12 * y1 - a12 * y2;
        acc += y2 * y2;
    }
    /* Flush denormals so silence after a loud passage stays fast. */
    if (fabs(z0) < 1e-30) z0 = 0.0;
    if (fabs(z1) < 1e-30) z1 = 0.0;
    if (fabs(z2) < 1e-30) z2 = 0.0;
    if (fabs(z3) < 1e-30) z3 = 0.0;
    z[0] = z0; z[1] = z1; z[2] = z2; z[3] = z3;
    return acc;
}

fa_result fa_meter_process(fa_meter* m, const float* x, int64_t frames)
{
    uint32_t c;
    while (frames > 0) {
        int64_t n = frames;
        if (m->skip > 0) {
            if (n > m->skip) n = m->skip;
            for (c = 0; c < m->channels; c++) filter_channel(m, x, n, c);
            if (m->with_peak) tp_process(&m->tp, x, n, 0);
            m->skip -= n;
        } else {
            if (n > m->sublen - m->subfill) n = m->sublen - m->subfill;
            for (c = 0; c < m->channels; c++) {
                double e = filter_channel(m, x, n, c);
                if (m->weight[c] != 0.0) m->subacc += m->weight[c] * e;
            }
            if (m->with_peak) tp_process(&m->tp, x, n, 1);
            m->subfill += n;
            if (m->subfill == m->sublen) {
                if (!push_sub(m, m->subacc / (double)m->sublen)) return FA_ERROR_OUT_OF_MEMORY;
                m->subfill = 0;
                m->subacc = 0.0;
            }
        }
        x += (size_t)n * m->channels;
        frames -= n;
    }
    return FA_OK;
}

/* ---- Gating ---------------------------------------------------------------- */

static double energy_to_lufs(double e) { return e > 0.0 ? -0.691 + 10.0 * log10(e) : -HUGE_VAL; }
static double lufs_to_energy(double l) { return pow(10.0, (l + 0.691) / 10.0); }

/* Integrated loudness (LUFS) of a list of 100 ms sub-block energies: 400 ms blocks,
 * absolute gate -70 LUFS, relative gate -10 LU. -HUGE_VAL when nothing passes. */
double fa_loudness_integrated(const double* subs, int64_t n)
{
    double abs_gate = lufs_to_energy(-70.0), sum = 0.0, run = 0.0, rel_gate, sum2 = 0.0;
    int64_t i, count = 0, count2 = 0;
    if (n < 4) return -HUGE_VAL;
    for (i = 0; i < 3; i++) run += subs[i];
    for (i = 3; i < n; i++) {
        double e;
        run += subs[i];
        e = run / 4.0;
        if (e > abs_gate) { sum += e; count++; }
        run -= subs[i - 3];
    }
    if (count == 0) return -HUGE_VAL;
    rel_gate = (sum / (double)count) * 0.1;   /* -10 LU */
    run = 0.0;
    for (i = 0; i < 3; i++) run += subs[i];
    for (i = 3; i < n; i++) {
        double e;
        run += subs[i];
        e = run / 4.0;
        if (e > abs_gate && e > rel_gate) { sum2 += e; count2++; }
        run -= subs[i - 3];
    }
    return count2 ? energy_to_lufs(sum2 / (double)count2) : -HUGE_VAL;
}

static int cmp_double(const void* a, const void* b)
{
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* Loudness range (LU) per EBU Tech 3342: 3 s short-term windows every 100 ms,
 * absolute gate -70 LUFS, relative gate -20 LU, then P95 - P10. */
double fa_loudness_range(const double* subs, int64_t n)
{
    double abs_gate = lufs_to_energy(-70.0), sum = 0.0, run = 0.0, rel_gate;
    double* vals;
    int64_t i, count = 0, k = 0, lo, hi;
    double lra;
    if (n < 30) return 0.0;
    vals = (double*)malloc((size_t)(n - 29) * sizeof(double));
    if (!vals) return 0.0;
    for (i = 0; i < 29; i++) run += subs[i];
    for (i = 29; i < n; i++) {
        double e;
        run += subs[i];
        e = run / 30.0;
        if (e > abs_gate) { sum += e; count++; vals[k++] = e; }
        run -= subs[i - 29];
    }
    if (count == 0) { free(vals); return 0.0; }
    rel_gate = (sum / (double)count) * 0.01;  /* -20 LU */
    count = 0;
    for (i = 0; i < k; i++) if (vals[i] > rel_gate) vals[count++] = energy_to_lufs(vals[i]);
    if (count < 2) { free(vals); return 0.0; }
    qsort(vals, (size_t)count, sizeof(double), cmp_double);
    lo = (int64_t)floor(0.10 * (double)(count - 1) + 0.5);
    hi = (int64_t)floor(0.95 * (double)(count - 1) + 0.5);
    lra = vals[hi] - vals[lo];
    free(vals);
    return lra;
}

/* Integrated loudness of the newest sub-blocks of a ring-mode meter (the
 * running estimate). NAN until one 400 ms block exists. */
double fa_meter_running_lufs(const fa_meter* m)
{
    double tmp[FA_RUNNING_SUBS];
    int64_t n = m->nsubs < m->cap ? m->nsubs : m->cap, i, first = m->nsubs - n;
    if (n < 4 || n > FA_RUNNING_SUBS) return NAN;
    for (i = 0; i < n; i++) tmp[i] = m->subs[(first + i) % m->cap];
    return fa_loudness_integrated(tmp, n);
}

double fa_meter_peak_dbtp(const fa_meter* m)
{
    if (!m->with_peak) return NAN;
    return m->tp.peak > 0.0f ? 20.0 * log10((double)m->tp.peak) : -HUGE_VAL;
}
