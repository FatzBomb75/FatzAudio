/* Variable-ratio resampler for FA_PARAM_RATE (varispeed: tempo and pitch move
 * together, like a record or tape played faster). Windowed-sinc interpolation with
 * a Kaiser window, 96 taps, from a 256-phase table interpolated linearly: about
 * -90 dB of aliasing and imaging, flat to 88% of the input's Nyquist frequency.
 * Speeding up (ratio > 1) widens the kernel by the ratio, which lowers the cutoff
 * to the output's Nyquist frequency, so nothing aliases. */
#include "tempo.h"
#include "fa_platform.h"
#include <math.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#  include <emmintrin.h>
#  define VS_SSE 1
#endif

#define VS_HALF FA_VARISPEED_HALF  /* taps on each side at ratio <= 1 */
#define VS_PHASES 256              /* table steps per tap */
#define VS_CUTOFF 0.94             /* fraction of Nyquist */
#define VS_BETA 9.0                /* Kaiser window: ~ -90 dB sidelobes */
#define VS_MAX_RATIO FA_VARISPEED_MAX_RATIO /* widest kernel kept in the history: pitch +36 (x8) at rate 10 */
#define VS_KEEP ((int64_t)(VS_HALF * VS_MAX_RATIO) + 2)

/* Modified Bessel function of the first kind, order 0 (series). */
static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0, q = x * x / 4.0;
    int k;
    for (k = 1; k < 64; k++) {
        term *= q / ((double)k * (double)k);
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

/* The kernel table is the same for every resampler: built once, shared, kept
 * for the life of the process. */
static fa_atomic_ptr shared_table;

static const float* kernel_table(void)
{
    int i, n = VS_HALF * VS_PHASES + 2;
    double i0b;
    float* t = (float*)fa_load_ptr(&shared_table);
    if (t) return t;
    t = (float*)malloc((size_t)n * sizeof(float));
    if (!t) return NULL;
    i0b = bessel_i0(VS_BETA);
    for (i = 0; i < n; i++) {
        double p = (double)i / VS_PHASES, x = VS_CUTOFF * p, sinc, w, r = p / VS_HALF;
        sinc = x == 0.0 ? 1.0 : sin(FA_TEMPO_PI * x) / (FA_TEMPO_PI * x);
        w = r >= 1.0 ? 0.0 : bessel_i0(VS_BETA * sqrt(1.0 - r * r)) / i0b;
        t[i] = (float)(VS_CUTOFF * sinc * w);
    }
    /* Two threads building it at once: the first one in is kept. */
    if (!fa_cas_ptr(&shared_table, NULL, t)) {
        free(t);
        t = (float*)fa_load_ptr(&shared_table);
    }
    return t;
}

fa_result fa_varispeed_init(fa_varispeed* v, uint32_t channels)
{
    memset(v, 0, sizeof(*v));
    v->channels = channels;
    v->keep = VS_KEEP;
    v->table = kernel_table();
    return v->table ? FA_OK : FA_ERROR_OUT_OF_MEMORY;
}

void fa_varispeed_set_keep(fa_varispeed* v, int64_t frames)
{
    v->keep = frames < VS_KEEP ? frames : VS_KEEP;
}

void fa_varispeed_free(fa_varispeed* v)
{
    free(v->buf);
    memset(v, 0, sizeof(*v));
}

void fa_varispeed_reset(fa_varispeed* v)
{
    v->count = 0;
    v->pos = 0.0;
}

static fa_result reserve(fa_varispeed* v, int64_t frames)
{
    float* b;
    int64_t cap;
    if (v->count + frames <= v->cap) return FA_OK;
    /* Drop what no output can reach any more first. */
    if (v->pos > (double)v->keep) {
        int64_t drop = (int64_t)v->pos - v->keep;
        if (drop > v->count) drop = v->count;
        memmove(v->buf, v->buf + (size_t)drop * v->channels, (size_t)(v->count - drop) * v->channels * sizeof(float));
        v->count -= drop;
        v->pos -= (double)drop;
        if (v->count + frames <= v->cap) return FA_OK;
    }
    cap = v->cap ? v->cap : 4096;
    while (cap < v->count + frames) cap *= 2;
    b = (float*)realloc(v->buf, (size_t)cap * v->channels * sizeof(float));
    if (!b) return FA_ERROR_OUT_OF_MEMORY;
    v->buf = b;
    v->cap = cap;
    return FA_OK;
}

fa_result fa_varispeed_prime(fa_varispeed* v, const float* history, int64_t frames)
{
    fa_result r;
    v->count = 0;
    v->pos = 0.0;
    r = reserve(v, frames);
    if (r != FA_OK) return r;
    if (frames > 0) memcpy(v->buf, history, (size_t)frames * v->channels * sizeof(float));
    v->count = frames;
    v->pos = (double)frames;
    return FA_OK;
}

fa_result fa_varispeed_write(fa_varispeed* v, const float* in, int64_t frames)
{
    fa_result r = reserve(v, frames);
    if (r != FA_OK) return r;
    if (in) memcpy(v->buf + (size_t)v->count * v->channels, in, (size_t)frames * v->channels * sizeof(float));
    else memset(v->buf + (size_t)v->count * v->channels, 0, (size_t)frames * v->channels * sizeof(float));
    v->count += frames;
    return FA_OK;
}

#if VS_SSE
/* The first `n` (1 to 4) floats at f, the rest of the vector 0 (unaligned loads). */
static inline __m128 load_upto4(const float* f, uint32_t n)
{
    if (n >= 4) return _mm_loadu_ps(f);
    if (n == 3) return _mm_movelh_ps(_mm_castsi128_ps(_mm_loadl_epi64((const __m128i*)(const void*)f)), _mm_load_ss(f + 2));
    if (n == 2) return _mm_castsi128_ps(_mm_loadl_epi64((const __m128i*)(const void*)f));
    return _mm_load_ss(f);
}

/* Stores the first `n` (0 to 4) floats of v at f. */
static inline void store_upto4(float* f, uint32_t n, __m128 v)
{
    if (n >= 4) { _mm_storeu_ps(f, v); return; }
    if (n >= 2) { _mm_storel_epi64((__m128i*)(void*)f, _mm_castps_si128(v)); f += 2; n -= 2; v = _mm_movehl_ps(v, v); }
    if (n == 1) _mm_store_ss(f, v);
}

/* 3 to 8 channels: acc[c] = sum of x[k * ch + c] * h[k] for k < m, the channels
 * in two vectors (0-3, 4-7). Two frames per step, so the adds overlap. With a
 * constant `ch` (the callers' switch) the loads are straight-line code. */
static inline void dot_frames(const float* x, const float* h, int64_t m, uint32_t ch, float* acc)
{
    const uint32_t lo = ch < 4 ? ch : 4, hi = ch - lo;
    __m128 a0 = _mm_setzero_ps(), a1 = _mm_setzero_ps(), b0 = _mm_setzero_ps(), b1 = _mm_setzero_ps();
    int64_t k;
    for (k = 0; k + 1 < m; k += 2) {
        const float* f = x + k * ch;
        __m128 h0 = _mm_set1_ps(h[k]), h1 = _mm_set1_ps(h[k + 1]);
        a0 = _mm_add_ps(a0, _mm_mul_ps(load_upto4(f, lo), h0));
        b0 = _mm_add_ps(b0, _mm_mul_ps(load_upto4(f + ch, lo), h1));
        if (hi) {
            a1 = _mm_add_ps(a1, _mm_mul_ps(load_upto4(f + 4, hi), h0));
            b1 = _mm_add_ps(b1, _mm_mul_ps(load_upto4(f + ch + 4, hi), h1));
        }
    }
    if (k < m) {
        const float* f = x + k * ch;
        __m128 h0 = _mm_set1_ps(h[k]);
        a0 = _mm_add_ps(a0, _mm_mul_ps(load_upto4(f, lo), h0));
        if (hi) a1 = _mm_add_ps(a1, _mm_mul_ps(load_upto4(f + 4, hi), h0));
    }
    store_upto4(acc, lo, _mm_add_ps(a0, b0));
    store_upto4(acc + 4, hi, _mm_add_ps(a1, b1));
}

/* dot_frames with `ch` a constant in each call, so each is compiled for its count. */
static void dot_frames_ch(const float* x, const float* h, int64_t m, uint32_t ch, float* acc)
{
    switch (ch) {
    case 3: dot_frames(x, h, m, 3, acc); break;
    case 4: dot_frames(x, h, m, 4, acc); break;
    case 5: dot_frames(x, h, m, 5, acc); break;
    case 6: dot_frames(x, h, m, 6, acc); break;
    case 7: dot_frames(x, h, m, 7, acc); break;
    default: dot_frames(x, h, m, 8, acc); break;
    }
}
#endif

/* Adds the taps on one side of the output time: input frames x, x - step, ...
 * (step -1 or +1 frame) at kernel positions t, t + dt, ... until the kernel ends. */
static void taps(const fa_varispeed* v, const float* x, int64_t avail, int dir, float t, float dt, float scale, float* acc)
{
    const float* table = v->table;
    uint32_t ch = v->channels, c;
    const float end = (float)(VS_HALF * VS_PHASES);
    int64_t m;
#if VS_SSE
    if (ch >= 3 && ch <= 8) {
        /* The channels in two vectors (as dot_frames), added to acc at the end. */
        const uint32_t lo = ch < 4 ? ch : 4, hi = ch - lo;
        __m128 a0 = _mm_setzero_ps(), a1 = _mm_setzero_ps();
        for (m = 0; m < avail && t < end; m++, t += dt) {
            int32_t ti = (int32_t)t;
            __m128 h = _mm_set1_ps((table[ti] + (table[ti + 1] - table[ti]) * (t - (float)ti)) * scale);
            const float* f = x + (ptrdiff_t)dir * m * (ptrdiff_t)ch;
            a0 = _mm_add_ps(a0, _mm_mul_ps(load_upto4(f, lo), h));
            if (hi) a1 = _mm_add_ps(a1, _mm_mul_ps(load_upto4(f + 4, hi), h));
        }
        store_upto4(acc, lo, _mm_add_ps(a0, load_upto4(acc, lo)));
        if (hi) store_upto4(acc + 4, hi, _mm_add_ps(a1, load_upto4(acc + 4, hi)));
        return;
    }
#endif
    for (m = 0; m < avail && t < end; m++, t += dt) {
        int32_t ti = (int32_t)t;
        float h = (table[ti] + (table[ti + 1] - table[ti]) * (t - (float)ti)) * scale;
        const float* f = x + (ptrdiff_t)dir * m * (ptrdiff_t)ch;
        for (c = 0; c < ch; c++) acc[c] += f[c] * h;
    }
}

int64_t fa_varispeed_width(double ratio)
{
    double scale = ratio > 1.0 ? 1.0 / ratio : 1.0;
    return (int64_t)ceil((double)VS_HALF / scale);
}

int64_t fa_varispeed_read(fa_varispeed* v, double ratio, float* out, int64_t max_frames)
{
    uint32_t ch = v->channels, c;
    double scale = ratio > 1.0 ? 1.0 / ratio : 1.0;
    int64_t width = fa_varispeed_width(ratio), n = 0;
    float steps = (float)(VS_PHASES * scale);
    while (n < max_frames) {
        int64_t i0 = (int64_t)floor(v->pos);
        float frac = (float)(v->pos - (double)i0), acc[FA_TEMPO_MAX_CHANNELS];
        if (i0 + width >= v->count) break;       /* needs input further ahead */
        for (c = 0; c < ch; c++) acc[c] = 0.0f;
        /* At or before the output time (i0, i0 - 1, ...), then after it (i0 + 1, ...). */
        taps(v, v->buf + (size_t)i0 * ch, i0 + 1, -1, frac * steps, steps, (float)scale, acc);
        taps(v, v->buf + (size_t)(i0 + 1) * ch, v->count - i0 - 1, 1, (1.0f - frac) * steps, steps, (float)scale, acc);
        for (c = 0; c < ch; c++) out[(size_t)n * ch + c] = acc[c];
        n++;
        v->pos += ratio;
    }
    return n;
}

/* ---- fixed ratios (device-rate conversion) ---------------------------------- */

#define VS_MAX_PHASES 1024          /* 44.1 <-> 48 kHz take 160 and 147 */
#define VS_MAX_TABLE (512u * 1024u) /* bytes of coefficients per ratio */

static fa_atomic_ptr phase_tables;  /* fa_vs_phases list, pushed with a CAS */

static uint32_t gcd_u32(uint32_t a, uint32_t b)
{
    while (b) { uint32_t t = a % b; a = b; b = t; }
    return a;
}

/* The kernel at table position t (VS_PHASES steps per tap), as taps() reads it. */
static double kernel_at(const float* table, double t)
{
    int32_t ti;
    if (t >= (double)(VS_HALF * VS_PHASES)) return 0.0;
    ti = (int32_t)t;
    return (double)table[ti] + ((double)table[ti + 1] - (double)table[ti]) * (t - (double)ti);
}

static fa_vs_phases* build_phases(uint32_t num, uint32_t den)
{
    const float* table = kernel_table();
    double ratio = (double)num / (double)den, scale = ratio > 1.0 ? 1.0 / ratio : 1.0;
    double steps = VS_PHASES * scale;
    int64_t w = fa_varispeed_width(ratio), k;
    uint32_t ph;
    fa_vs_phases* p;
    if (!table || (size_t)den * (size_t)(2 * w) * sizeof(float) > VS_MAX_TABLE) return NULL;
    p = (fa_vs_phases*)calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->coef = (float*)malloc((size_t)den * (size_t)(2 * w) * sizeof(float));
    if (!p->coef) { free(p); return NULL; }
    p->num = num;
    p->den = den;
    p->width = w;
    for (ph = 0; ph < den; ph++) {
        double frac = (double)ph / (double)den;
        float* row = p->coef + (size_t)ph * (size_t)(2 * w);
        for (k = 0; k < 2 * w; k++) {
            /* Input i0 - m at or before the output time, or i0 + 1 + m after it. */
            double t = k < w ? (frac + (double)(w - 1 - k)) * steps : (1.0 - frac + (double)(k - w)) * steps;
            row[k] = (float)(kernel_at(table, t) * scale);
        }
    }
    return p;
}

const fa_vs_phases* fa_varispeed_phases(uint32_t rate_in, uint32_t rate_out)
{
    uint32_t g, num, den;
    fa_vs_phases *p, *head;
    if (rate_in == 0 || rate_out == 0) return NULL;
    g = gcd_u32(rate_in, rate_out);
    num = rate_in / g;
    den = rate_out / g;
    if (den > VS_MAX_PHASES || (double)num / (double)den > VS_MAX_RATIO) return NULL;
    for (p = (fa_vs_phases*)fa_load_ptr(&phase_tables); p; p = p->next)
        if (p->num == num && p->den == den) return p;
    p = build_phases(num, den);
    if (!p) return NULL;
    /* Two threads building the same one: both are kept, both are right. */
    do {
        head = (fa_vs_phases*)fa_load_ptr(&phase_tables);
        p->next = head;
    } while (!fa_cas_ptr(&phase_tables, head, p));
    return p;
}

/* One output frame at input time i0 + ph / den into o. */
static void phased_frame(const fa_varispeed* v, const fa_vs_phases* p, uint32_t ph, int64_t i0, float* o)
{
    const uint32_t ch = v->channels;
    const int64_t w = p->width, taps = 2 * w;
    const float* row = p->coef + (size_t)ph * (size_t)taps;
    int64_t first = i0 - w + 1, from = first < 0 ? -first : 0, m = taps - from, k;   /* before the start: nothing */
    const float* x = v->buf + (first + from) * (int64_t)ch;
    uint32_t c;
    row += from;
    if (ch == 2) {
#if VS_SSE
        /* The scalar loop below, its four sums as the lanes l0 r0 l1 r1 of one
         * register: the same products added in the same order. */
        __m128 a = _mm_setzero_ps();
        float t[4];
        for (k = 0; k + 1 < m; k += 2) {
            __m128 h = load_upto4(row + k, 2);
            a = _mm_add_ps(a, _mm_mul_ps(_mm_loadu_ps(x + k * 2), _mm_unpacklo_ps(h, h)));
        }
        _mm_storeu_ps(t, a);
        if (k < m) { t[0] += x[k * 2] * row[k]; t[1] += x[k * 2 + 1] * row[k]; }
        o[0] = t[0] + t[2];
        o[1] = t[1] + t[3];
#else
        /* Two sums per channel, so the adds overlap. */
        float l0 = 0.0f, r0 = 0.0f, l1 = 0.0f, r1 = 0.0f;
        for (k = 0; k + 1 < m; k += 2) {
            l0 += x[k * 2] * row[k];
            r0 += x[k * 2 + 1] * row[k];
            l1 += x[k * 2 + 2] * row[k + 1];
            r1 += x[k * 2 + 3] * row[k + 1];
        }
        if (k < m) { l0 += x[k * 2] * row[k]; r0 += x[k * 2 + 1] * row[k]; }
        o[0] = l0 + l1;
        o[1] = r0 + r1;
#endif
    } else if (ch == 1) {
        float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
        for (k = 0; k + 3 < m; k += 4) {
            a0 += x[k] * row[k];
            a1 += x[k + 1] * row[k + 1];
            a2 += x[k + 2] * row[k + 2];
            a3 += x[k + 3] * row[k + 3];
        }
        for (; k < m; k++) a0 += x[k] * row[k];
        o[0] = (a0 + a1) + (a2 + a3);
#if VS_SSE
    } else if (ch <= 8) {
        dot_frames_ch(x, row, m, ch, o);
#endif
    } else {
        float acc[FA_TEMPO_MAX_CHANNELS];
        for (c = 0; c < ch; c++) acc[c] = 0.0f;
        for (k = 0; k < m; k++) {
            const float h = row[k], *f = x + k * ch;
            for (c = 0; c < ch; c++) acc[c] += f[c] * h;
        }
        for (c = 0; c < ch; c++) o[c] = acc[c];
    }
}

int64_t fa_varispeed_read_phased(fa_varispeed* v, const fa_vs_phases* p, uint32_t* phase, float* out, int64_t max_frames)
{
    int64_t i0 = (int64_t)floor(v->pos), n = 0;
    uint32_t ph = *phase;
    while (n < max_frames && i0 + p->width < v->count) {   /* else it needs input further ahead */
        if (out) phased_frame(v, p, ph, i0, out + (size_t)n * v->channels);
        n++;
        ph += p->num;
        i0 += ph / p->den;
        ph %= p->den;
    }
    v->pos = (double)i0 + (double)ph / (double)p->den;
    *phase = ph;
    return n;
}

double fa_varispeed_pending(const fa_varispeed* v)
{
    return (double)v->count - v->pos;
}

int64_t fa_varispeed_tail(void)
{
    return VS_KEEP;
}
