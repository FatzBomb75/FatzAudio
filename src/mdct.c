/* Inverse MDCT of any power-of-two size through a complex FFT of a quarter of the
 * output length (the usual DCT-IV folding). Shared by the built-in transform
 * codecs (ac3.c, aac.c, and libvorbis through fa_vorbis_imdct); written from the
 * transform's definition. */
#include "mdct.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#  include <xmmintrin.h>
#  define FA_FFT_SSE 1
#elif defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(_M_ARM64)
#  include <arm_neon.h>
#  define FA_FFT_NEON 1
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* One set of tables per size 2^bits of m (m up to 2^20). */
typedef struct mdct_tables {
    fa_atomic32 state;     /* 0 not built, 1 being built, 2 built */
    float* twiddle;
    float* post;
    float* fft_tw;
    int* rev;
} mdct_tables;

static mdct_tables shared_tables[21];

static int build_tables(mdct_tables* s, int m)
{
    int q = m / 2, i, bits = 0;
    while ((1 << bits) < q) bits++;
    s->twiddle = (float*)malloc(sizeof(float) * (size_t)q * 2);
    s->post = (float*)malloc(sizeof(float) * (size_t)q * 2);
    /* Radix-4 passes: 12 floats per butterfly (see fft), under 4q in all. */
    s->fft_tw = (float*)malloc(sizeof(float) * (size_t)q * 4);
    s->rev = (int*)malloc(sizeof(int) * (size_t)q);
    if (!s->twiddle || !s->post || !s->fft_tw || !s->rev) {
        free(s->twiddle);
        free(s->post);
        free(s->fft_tw);
        free(s->rev);
        s->twiddle = s->post = s->fft_tw = NULL;
        s->rev = NULL;
        return 0;
    }
    for (i = 0; i < q; i++) {
        double a = M_PI * (4.0 * i + 1.0) / (4.0 * m);
        int r = 0, b;
        s->twiddle[2 * i] = (float)cos(a);
        s->twiddle[2 * i + 1] = (float)-sin(a);
        s->post[2 * i] = (float)cos(M_PI * i / m);
        s->post[2 * i + 1] = (float)-sin(M_PI * i / m);
        for (b = 0; b < bits; b++) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        s->rev[i] = r;
    }
    /* Roots of the radix-4 passes with h >= 2 (the h = 1 pass only multiplies by
     * 1). With W = e^(-2 pi i / 4h), per pair of butterflies k, k + 1 and per
     * root W^rk (r = 1, 2, 3): the real parts as (re_k, re_k, re_k+1, re_k+1),
     * then the imaginary ones as (-im_k, im_k, -im_k+1, im_k+1), the shape a
     * vector complex multiply wants. */
    {
        int h, k, r;
        float* w = s->fft_tw;
        for (h = (bits & 1) ? 2 : 4; 4 * h <= q; h *= 4) {
            for (k = 0; k < h; k += 2) {
                for (r = 1; r <= 3; r++, w += 8) {
                    int j;
                    for (j = 0; j < 2; j++) {
                        double a = -2.0 * M_PI * r * (k + j) / (4.0 * h);
                        w[2 * j] = w[2 * j + 1] = (float)cos(a);
                        w[4 + 2 * j] = (float)-sin(a);
                        w[4 + 2 * j + 1] = (float)sin(a);
                    }
                }
            }
        }
    }
    return 1;
}

fa_result fa_imdct_init(fa_mdct* t, int m)
{
    int bits = 0;
    mdct_tables* s;
    memset(t, 0, sizeof(*t));
    if (m < 4 || (m & (m - 1))) return FA_ERROR_INVALID_ARG;
    while ((1 << bits) < m) bits++;
    if (bits >= (int)(sizeof(shared_tables) / sizeof(shared_tables[0]))) return FA_ERROR_INVALID_ARG;
    s = &shared_tables[bits];
    while (fa_load32(&s->state) != 2) {
        if (fa_cas32(&s->state, 0, 1)) {
            /* Out of memory leaves them unbuilt, for a later try. */
            int ok = build_tables(s, m);
            fa_store32(&s->state, ok ? 2 : 0);
            if (!ok) return FA_ERROR_OUT_OF_MEMORY;
        } else {
            fa_sleep_ms(1);
        }
    }
    t->m = m;
    t->twiddle = s->twiddle;
    t->post = s->post;
    t->fft_tw = s->fft_tw;
    t->rev = s->rev;
    t->work = (float*)malloc(sizeof(float) * (size_t)m);
    if (!t->work) {
        memset(t, 0, sizeof(*t));
        return FA_ERROR_OUT_OF_MEMORY;
    }
    return FA_OK;
}

void fa_imdct_free(fa_mdct* t)
{
    free(t->work);
    memset(t, 0, sizeof(*t));
}

/* In-place FFT (forward, e^-i) of q complex values, input in bit-reversed order:
 * a radix-2 pass when log2(q) is odd, then radix-4 passes, each doing two radix-2
 * stages at once (decimation in time, so the input order stays bit-reversed). A pass
 * joins four transforms of size h into one of 4h: with W = e^(-2 pi i / 4h),
 * X = A + W^2k B, Y = A - W^2k B, U = W^k C + W^3k D, V = W^k C - W^3k D give
 * out[k] = X + U, out[k + h] = Y - iV, out[k + 2h] = X - U, out[k + 3h] = Y + iV.
 * Passes with h >= 2 do two butterflies at once (SSE, NEON, or plain C). */
static void fft(const fa_mdct* t, float* z, int q)
{
    const float* tw = t->fft_tw;
    int h, i, k;
    if ((q & 0x55555555) == 0) {   /* q = 2 * 4^n */
        for (i = 0; i < 2 * q; i += 4) {
            float ar = z[i], ai = z[i + 1], br = z[i + 2], bi = z[i + 3];
            z[i] = ar + br; z[i + 1] = ai + bi;
            z[i + 2] = ar - br; z[i + 3] = ai - bi;
        }
        h = 2;
    } else {
        for (i = 0; i < 2 * q; i += 8) {   /* h = 1: every root is 1 */
            float* a = z + i;
            float xr = a[0] + a[2], xi = a[1] + a[3], yr = a[0] - a[2], yi = a[1] - a[3];
            float ur = a[4] + a[6], ui = a[5] + a[7], vr = a[4] - a[6], vi = a[5] - a[7];
            a[0] = xr + ur; a[1] = xi + ui;
            a[4] = xr - ur; a[5] = xi - ui;
            a[2] = yr + vi; a[3] = yi - vr;
            a[6] = yr - vi; a[7] = yi + vr;
        }
        h = 4;
    }
    for (; 4 * h <= q; h *= 4) {
        for (i = 0; i < q; i += 4 * h) {
            float* a = z + 2 * i;
            float* b = a + 2 * h;
            float* c = b + 2 * h;
            float* d = c + 2 * h;
            const float* w = tw;
            for (k = 0; k < 2 * h; k += 4, w += 24) {
#if defined(FA_FFT_SSE)
                const __m128 neg = _mm_set_ps(-1.0f, 1.0f, -1.0f, 1.0f);   /* (1, -1, 1, -1) */
                __m128 x, y, u, v, p, pb, pc, pd;
                p = _mm_loadu_ps(b + k);
                pb = _mm_add_ps(_mm_mul_ps(p, _mm_loadu_ps(w + 8)),
                                _mm_mul_ps(_mm_shuffle_ps(p, p, _MM_SHUFFLE(2, 3, 0, 1)), _mm_loadu_ps(w + 12)));
                p = _mm_loadu_ps(c + k);
                pc = _mm_add_ps(_mm_mul_ps(p, _mm_loadu_ps(w)),
                                _mm_mul_ps(_mm_shuffle_ps(p, p, _MM_SHUFFLE(2, 3, 0, 1)), _mm_loadu_ps(w + 4)));
                p = _mm_loadu_ps(d + k);
                pd = _mm_add_ps(_mm_mul_ps(p, _mm_loadu_ps(w + 16)),
                                _mm_mul_ps(_mm_shuffle_ps(p, p, _MM_SHUFFLE(2, 3, 0, 1)), _mm_loadu_ps(w + 20)));
                p = _mm_loadu_ps(a + k);
                x = _mm_add_ps(p, pb);
                y = _mm_sub_ps(p, pb);
                u = _mm_add_ps(pc, pd);
                v = _mm_sub_ps(pc, pd);
                v = _mm_mul_ps(_mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 3, 0, 1)), neg);   /* (vi, -vr): -iV */
                _mm_storeu_ps(a + k, _mm_add_ps(x, u));
                _mm_storeu_ps(c + k, _mm_sub_ps(x, u));
                _mm_storeu_ps(b + k, _mm_add_ps(y, v));
                _mm_storeu_ps(d + k, _mm_sub_ps(y, v));
#elif defined(FA_FFT_NEON)
                static const float neg_[4] = { 1.0f, -1.0f, 1.0f, -1.0f };
                float32x4_t neg = vld1q_f32(neg_), x, y, u, v, p, pb, pc, pd;
                p = vld1q_f32(b + k);
                pb = vaddq_f32(vmulq_f32(p, vld1q_f32(w + 8)), vmulq_f32(vrev64q_f32(p), vld1q_f32(w + 12)));
                p = vld1q_f32(c + k);
                pc = vaddq_f32(vmulq_f32(p, vld1q_f32(w)), vmulq_f32(vrev64q_f32(p), vld1q_f32(w + 4)));
                p = vld1q_f32(d + k);
                pd = vaddq_f32(vmulq_f32(p, vld1q_f32(w + 16)), vmulq_f32(vrev64q_f32(p), vld1q_f32(w + 20)));
                p = vld1q_f32(a + k);
                x = vaddq_f32(p, pb);
                y = vsubq_f32(p, pb);
                u = vaddq_f32(pc, pd);
                v = vsubq_f32(pc, pd);
                v = vmulq_f32(vrev64q_f32(v), neg);   /* (vi, -vr): -iV */
                vst1q_f32(a + k, vaddq_f32(x, u));
                vst1q_f32(c + k, vsubq_f32(x, u));
                vst1q_f32(b + k, vaddq_f32(y, v));
                vst1q_f32(d + k, vsubq_f32(y, v));
#else
                int j;
                for (j = 0; j < 4; j += 2) {
                    float w1r = w[j], w1i = w[5 + j], w2r = w[8 + j], w2i = w[13 + j], w3r = w[16 + j], w3i = w[21 + j];
                    int m = k + j;
                    float br = b[m] * w2r - b[m + 1] * w2i, bi = b[m] * w2i + b[m + 1] * w2r;
                    float cr = c[m] * w1r - c[m + 1] * w1i, ci = c[m] * w1i + c[m + 1] * w1r;
                    float dr = d[m] * w3r - d[m + 1] * w3i, di = d[m] * w3i + d[m + 1] * w3r;
                    float xr = a[m] + br, xi = a[m + 1] + bi, yr = a[m] - br, yi = a[m + 1] - bi;
                    float ur = cr + dr, ui = ci + di, vr = cr - dr, vi = ci - di;
                    a[m] = xr + ur; a[m + 1] = xi + ui;
                    c[m] = xr - ur; c[m + 1] = xi - ui;
                    b[m] = yr + vi; b[m + 1] = yi - vr;   /* Y - iV */
                    d[m] = yr - vi; d[m + 1] = yi + vr;   /* Y + iV */
                }
#endif
            }
        }
        tw += 12 * h;
    }
}

void fa_mdct_dct4(const fa_mdct* t, const float* in, float* u)
{
    int m = t->m, q = m / 2, k;
    float* z = t->work;
    for (k = 0; k < q; k++) {
        float re = in[2 * k], im = in[m - 1 - 2 * k];
        float wr = t->twiddle[2 * k], wi = t->twiddle[2 * k + 1];
        int r = t->rev[k];
        z[2 * r] = re * wr - im * wi;
        z[2 * r + 1] = re * wi + im * wr;
    }
    fft(t, z, q);
    for (k = 0; k < q; k++) {
        float re = z[2 * k], im = z[2 * k + 1];
        float wr = t->post[2 * k], wi = t->post[2 * k + 1];
        float yr = re * wr - im * wi, yi = re * wi + im * wr;
        u[2 * k] = yr;
        u[m - 1 - 2 * k] = -yi;
    }
}

/* libvorbis's inverse MDCT goes through this one (see third_party/XIPH.md): n
 * samples from the n/2 coefficients at the start of x, in place. The DCT-IV
 * lands in the second half, u = x + m, and unfolds from there in an order that
 * reads each value of u before it is overwritten. */
void* fa_vorbis_imdct_create(int n)
{
    fa_mdct* t = (fa_mdct*)malloc(sizeof(*t));
    if (t && fa_imdct_init(t, n / 2) != FA_OK) {
        free(t);
        t = NULL;
    }
    return t;
}

void fa_vorbis_imdct(void* p, float* x)
{
    const fa_mdct* t = (const fa_mdct*)p;
    int m = t->m, h = m / 2, i;
    float* u = x + m;
    fa_mdct_dct4(t, x, u);
    for (i = 0; i < h; i++) x[i] = u[i + h];
    for (i = h; i < m; i++) x[i] = -u[m + h - 1 - i];
    for (i = 0; i < h; i++) x[m + h + i] = -u[i];
    for (i = 0; i < h / 2; i++) {
        float a = u[i], b = u[h - 1 - i];
        u[i] = -b;
        u[h - 1 - i] = -a;
    }
}

void fa_vorbis_imdct_free(void* p)
{
    if (!p) return;
    fa_imdct_free((fa_mdct*)p);
    free(p);
}
