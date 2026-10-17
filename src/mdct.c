/* Inverse MDCT of any power-of-two size through a complex FFT of a quarter of the
 * output length (the usual DCT-IV folding). Shared by the built-in transform
 * codecs (ac3.c); written from the transform's definition. */
#include "mdct.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

fa_result fa_imdct_init(fa_mdct* t, int m)
{
    int q = m / 2, i, bits = 0;
    memset(t, 0, sizeof(*t));
    if (m < 4 || (m & (m - 1))) return FA_ERROR_INVALID_ARG;
    while ((1 << bits) < q) bits++;
    t->m = m;
    t->twiddle = (float*)malloc(sizeof(float) * (size_t)q * 2);
    t->post = (float*)malloc(sizeof(float) * (size_t)q * 2);
    t->fft_tw = (float*)malloc(sizeof(float) * (size_t)q);
    t->rev = (int*)malloc(sizeof(int) * (size_t)q);
    t->work = (float*)malloc(sizeof(float) * (size_t)q * 2);
    t->u = (float*)malloc(sizeof(float) * (size_t)m);
    if (!t->twiddle || !t->post || !t->fft_tw || !t->rev || !t->work || !t->u) {
        fa_imdct_free(t);
        return FA_ERROR_OUT_OF_MEMORY;
    }
    for (i = 0; i < q; i++) {
        double a = M_PI * (4.0 * i + 1.0) / (4.0 * m);
        int r = 0, b;
        t->twiddle[2 * i] = (float)cos(a);
        t->twiddle[2 * i + 1] = (float)-sin(a);
        t->post[2 * i] = (float)cos(M_PI * i / m);
        t->post[2 * i + 1] = (float)-sin(M_PI * i / m);
        for (b = 0; b < bits; b++) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        t->rev[i] = r;
    }
    for (i = 0; i < q / 2; i++) {
        double a = -2.0 * M_PI * i / q;
        t->fft_tw[2 * i] = (float)cos(a);
        t->fft_tw[2 * i + 1] = (float)sin(a);
    }
    return FA_OK;
}

void fa_imdct_free(fa_mdct* t)
{
    free(t->twiddle);
    free(t->post);
    free(t->fft_tw);
    free(t->rev);
    free(t->work);
    free(t->u);
    memset(t, 0, sizeof(*t));
}

/* In-place radix-2 FFT (forward, e^-i) of q complex values, input in bit-reversed order. */
static void fft(const fa_mdct* t, float* z, int q)
{
    int len, i, k;
    for (len = 2; len <= q; len <<= 1) {
        int half = len >> 1, step = q / len;
        for (i = 0; i < q; i += len) {
            for (k = 0; k < half; k++) {
                float wr = t->fft_tw[2 * k * step], wi = t->fft_tw[2 * k * step + 1];
                float* a = z + 2 * (i + k);
                float* b = z + 2 * (i + k + half);
                float br = b[0] * wr - b[1] * wi, bi = b[0] * wi + b[1] * wr;
                b[0] = a[0] - br;
                b[1] = a[1] - bi;
                a[0] += br;
                a[1] += bi;
            }
        }
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

void fa_imdct(const fa_mdct* t, const float* in, float* out)
{
    int m = t->m, h = m / 2, n;
    float* u = t->u;
    fa_mdct_dct4(t, in, u);
    /* y[n] = sum_k in[k] cos(pi/m (n + m/2 + 1/2)(k + 1/2)) for n = 0..2m-1, from
     * the DCT-IV's symmetries u[2m-1-j] = -u[j] and u[j+2m] = -u[j]. */
    for (n = 0; n < h; n++) out[n] = u[n + h];
    for (n = h; n < 3 * h; n++) out[n] = -u[3 * h - 1 - n];
    for (n = 3 * h; n < 4 * h; n++) out[n] = -u[n - 3 * h];
}
