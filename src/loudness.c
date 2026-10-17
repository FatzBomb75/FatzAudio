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
 * True peak uses a polyphase 4x oversampler (2x at 88.2/96 kHz, none above).
 * Its 128-tap filter is flat to within 0.1 dB up to 0.83x Nyquist (20 kHz at
 * 48 kHz) and never reads high (passband ripple below 0.001 dB). */
#include "fa_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#  include <xmmintrin.h>
#  define FA_TP_SSE 1
#elif defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(_M_ARM64)
#  include <arm_neon.h>
#  define FA_TP_NEON 1
#endif

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

    /* Kaiser-windowed sinc low-pass (beta 9) at 0.97x the original Nyquist, split
     * into phases. Each phase is normalised to unity DC gain. */
    n = tp->phases * FA_TP_TAPS;
    for (p = 0; p < tp->phases; p++) {
        double sum = 0.0, abs_sum = 0.0;
        double h[FA_TP_TAPS];
        for (t = 0; t < FA_TP_TAPS; t++) {
            int i = t * tp->phases + p;
            double x = (double)i - (double)(n - 1) / 2.0;
            double fc = 0.97 / (2.0 * tp->phases);       /* cycles per oversampled sample */
            double sinc = x == 0.0 ? 2.0 * fc : sin(2.0 * FA_PI * fc * x) / (FA_PI * x);
            double r = 2.0 * (double)i / (double)(n - 1) - 1.0;
            double w = bessel_i0(9.0 * sqrt(1.0 - r * r)) / bessel_i0(9.0);
            h[t] = sinc * w;
            sum += h[t];
        }
        for (t = 0; t < FA_TP_TAPS; t++) {
            h[t] /= sum;
            abs_sum += fabs(h[t]);
            /* Reversed so the dot product runs over the history oldest-first. */
            tp->coef[FA_TP_TAPS - 1 - t][p] = (float)h[t];
        }
        if ((float)abs_sum > tp->bound) tp->bound = (float)abs_sum;
    }
#if defined(FA_TP_SSE) || defined(FA_TP_NEON)
    tp->wide = (float*)malloc(sizeof(float) * FA_TP_TAPS * FA_TP_PHASES * 4);
    if (!tp->wide) { fa_truepeak_free(tp); return FA_ERROR_OUT_OF_MEMORY; }
    for (t = 0; t < FA_TP_TAPS; t++)
        for (p = 0; p < FA_TP_PHASES; p++)
            for (n = 0; n < 4; n++) tp->wide[(t * FA_TP_PHASES + p) * 4 + n] = tp->coef[t][p];
#endif
    return FA_OK;
}

void fa_truepeak_free(fa_truepeak* tp)
{
    free(tp->hist);
    free(tp->wide);
    tp->hist = NULL;
    tp->wide = NULL;
}

/* The four phases' interpolated values at once: out[p] = sum over t of
 * coef[t][p] * w[t], each phase summed in tap order as the plain loop does (one
 * SIMD lane per phase). */
static void tp_phases(const float (*coef)[FA_TP_PHASES], const float* w, float* out)
{
    int t;
#if defined(FA_TP_SSE)
    __m128 acc = _mm_setzero_ps();
    for (t = 0; t < FA_TP_TAPS; t++) acc = _mm_add_ps(acc, _mm_mul_ps(_mm_loadu_ps(coef[t]), _mm_set1_ps(w[t])));
    _mm_storeu_ps(out, acc);
#elif defined(FA_TP_NEON)
    float32x4_t acc = vdupq_n_f32(0.0f);
    for (t = 0; t < FA_TP_TAPS; t++) acc = vaddq_f32(acc, vmulq_f32(vld1q_f32(coef[t]), vdupq_n_f32(w[t])));
    vst1q_f32(out, acc);
#else
    float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
    for (t = 0; t < FA_TP_TAPS; t++) {
        a0 += coef[t][0] * w[t];
        a1 += coef[t][1] * w[t];
        a2 += coef[t][2] * w[t];
        a3 += coef[t][3] * w[t];
    }
    out[0] = a0; out[1] = a1; out[2] = a2; out[3] = a3;
#endif
}

/* Appends one sample per channel and returns the largest |value| among the
 * sample itself and the interpolated points it completes. History per channel is stored twice
 * (positions pos and pos + TAPS) so the newest TAPS samples are contiguous. */
static float tp_push(fa_truepeak* tp, const float* frame)
{
    uint32_t c;
    int p;
    float m = 0.0f, v[FA_TP_PHASES];
    tp->pos = (tp->pos + 1) % FA_TP_TAPS;
    for (c = 0; c < tp->channels; c++) {
        float* h = tp->hist + (size_t)c * FA_TP_TAPS * 2;
        float a = fabsf(frame[c]);
        h[tp->pos] = h[tp->pos + FA_TP_TAPS] = frame[c];
        if (a > m) m = a;
        if (tp->phases == 1) continue;
        tp_phases((const float (*)[FA_TP_PHASES])tp->coef, h + tp->pos + 1, v);
        for (p = 0; p < tp->phases; p++) {
            a = fabsf(v[p]);
            if (a > m) m = a;
        }
    }
    return m;
}

float fa_truepeak_push(fa_truepeak* tp, const float* frame) { return tp_push(tp, frame); }

/* Pushes TAPS frames of silence so the interpolated points between the last
 * samples are measured too (end of the file). */
void fa_truepeak_flush(fa_truepeak* tp)
{
    float* zero = (float*)calloc(tp->channels, sizeof(float));
    int i;
    if (!zero) return;
    for (i = 0; i < FA_TP_TAPS; i++) {
        float m = tp_push(tp, zero);
        if (m > tp->peak) tp->peak = m;
    }
    free(zero);
}

/* The largest |value| among the samples lin[TAPS - 1 .. TAPS - 1 + n) of one
 * channel and the points interpolated up to each: lin holds the TAPS - 1
 * samples before them first. Four samples at a time, each SIMD lane one sample,
 * each sum in tap order as tp_push forms it, so the result is the same. */
static float tp_block(const fa_truepeak* tp, const float* lin, int n)
{
    int j, p, phases = tp->phases;
    float m = 0.0f, v[4];
    for (j = 0; j < n; j++) { float a = fabsf(lin[FA_TP_TAPS - 1 + j]); if (a > m) m = a; }
    if (phases == 1) return m;
    j = 0;
#if defined(FA_TP_SSE)
    {
        const float* wide = tp->wide;
        int t;
        const __m128 sign = _mm_set1_ps(-0.0f);
        __m128 mx = _mm_setzero_ps();
        for (; j + 4 <= n; j += 4) {
            __m128 a0 = _mm_setzero_ps(), a1 = a0, a2 = a0, a3 = a0;
            for (t = 0; t < FA_TP_TAPS; t++) {
                __m128 x = _mm_loadu_ps(lin + j + t);
                const float* c = wide + t * FA_TP_PHASES * 4;
                a0 = _mm_add_ps(a0, _mm_mul_ps(_mm_loadu_ps(c), x));
                a1 = _mm_add_ps(a1, _mm_mul_ps(_mm_loadu_ps(c + 4), x));
                a2 = _mm_add_ps(a2, _mm_mul_ps(_mm_loadu_ps(c + 8), x));
                a3 = _mm_add_ps(a3, _mm_mul_ps(_mm_loadu_ps(c + 12), x));
            }
            mx = _mm_max_ps(mx, _mm_max_ps(_mm_max_ps(_mm_andnot_ps(sign, a0), _mm_andnot_ps(sign, a1)),
                                           _mm_max_ps(_mm_andnot_ps(sign, a2), _mm_andnot_ps(sign, a3))));
        }
        _mm_storeu_ps(v, mx);
        for (p = 0; p < 4; p++) if (v[p] > m) m = v[p];
    }
#elif defined(FA_TP_NEON)
    {
        const float* wide = tp->wide;
        int t;
        float32x4_t mx = vdupq_n_f32(0.0f);
        for (; j + 4 <= n; j += 4) {
            float32x4_t a0 = vdupq_n_f32(0.0f), a1 = a0, a2 = a0, a3 = a0;
            for (t = 0; t < FA_TP_TAPS; t++) {
                float32x4_t x = vld1q_f32(lin + j + t);
                const float* c = wide + t * FA_TP_PHASES * 4;
                a0 = vaddq_f32(a0, vmulq_f32(vld1q_f32(c), x));
                a1 = vaddq_f32(a1, vmulq_f32(vld1q_f32(c + 4), x));
                a2 = vaddq_f32(a2, vmulq_f32(vld1q_f32(c + 8), x));
                a3 = vaddq_f32(a3, vmulq_f32(vld1q_f32(c + 12), x));
            }
            mx = vmaxq_f32(mx, vmaxq_f32(vmaxq_f32(vabsq_f32(a0), vabsq_f32(a1)), vmaxq_f32(vabsq_f32(a2), vabsq_f32(a3))));
        }
        vst1q_f32(v, mx);
        for (p = 0; p < 4; p++) if (v[p] > m) m = v[p];
    }
#endif
    /* The rest (and every sample without SIMD). */
    for (; j < n; j++) {
        tp_phases((const float (*)[FA_TP_PHASES])tp->coef, lin + j, v);
        for (p = 0; p < phases; p++) { float a = fabsf(v[p]); if (a > m) m = a; }
    }
    return m;
}

/* Measures `frames` interleaved frames. With `measure` 0 the samples only fill
 * the history (warm-up). Blocks of FA_TP_BLOCK frames that cannot beat the peak
 * found so far are skipped: no interpolated point can exceed `bound` x the
 * largest input sample it is computed from (the block and the TAPS frames before
 * it, which are the history), so the skip never changes the result. */
#define FA_TP_BLOCK 256
static void tp_process(fa_truepeak* tp, const float* x, int64_t frames, int measure)
{
    uint32_t ch = tp->channels;
    int64_t at = 0;
    while (at < frames) {
        int64_t n = frames - at < FA_TP_BLOCK ? frames - at : FA_TP_BLOCK, i;
        const float* b = x + (size_t)at * ch;
        int skip = !measure;
        uint32_t c;
        if (measure) {
            size_t k;
            float in_max = 0.0f;
            for (k = 0; k < (size_t)n * ch; k++) { float a = fabsf(b[k]); if (a > in_max) in_max = a; }
            for (c = 0; c < ch; c++) {
                const float* h = tp->hist + (size_t)c * FA_TP_TAPS * 2 + tp->pos + 1;   /* the last TAPS */
                for (k = 0; k < FA_TP_TAPS; k++) { float a = fabsf(h[k]); if (a > in_max) in_max = a; }
            }
            skip = in_max * tp->bound <= tp->peak;
        }
        if (!skip) {
            float lin[FA_TP_TAPS - 1 + FA_TP_BLOCK];
            for (c = 0; c < ch; c++) {
                float m;
                memcpy(lin, tp->hist + (size_t)c * FA_TP_TAPS * 2 + tp->pos + 2, sizeof(float) * (FA_TP_TAPS - 1));
                for (i = 0; i < n; i++) lin[FA_TP_TAPS - 1 + i] = b[(size_t)i * ch + c];
                m = tp_block(tp, lin, (int)n);
                if (m > tp->peak) tp->peak = m;
            }
        }
        /* Only the last TAPS frames matter for what comes next. */
        for (i = n > FA_TP_TAPS ? n - FA_TP_TAPS : 0; i < n; i++) {
            tp->pos = (tp->pos + 1) % FA_TP_TAPS;
            for (c = 0; c < ch; c++) {
                float* h = tp->hist + (size_t)c * FA_TP_TAPS * 2;
                h[tp->pos] = h[tp->pos + FA_TP_TAPS] = b[(size_t)i * ch + c];
            }
        }
        at += n;
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
 * excluded. Layouts follow the usual WAVE/FLAC/Vorbis orders: quad L R Ls Rs,
 * 5.0, 5.1, 6.1 (L R C LFE Cs Ls Rs) and 7.1. */
static double channel_weight(uint32_t channels, uint32_t c)
{
    if (channels == 4) { static const double w[4] = { 1, 1, 1.41, 1.41 }; return w[c]; }
    if (channels == 6) { static const double w[6] = { 1, 1, 1, 0, 1.41, 1.41 }; return w[c]; }
    if (channels == 5) { static const double w[5] = { 1, 1, 1, 1.41, 1.41 }; return w[c]; }
    if (channels == 7) { static const double w[7] = { 1, 1, 1, 0, 1.41, 1.41, 1.41 }; return w[c]; }
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
