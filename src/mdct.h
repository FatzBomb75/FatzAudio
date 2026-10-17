/* Inverse MDCT (mdct.c). */
#ifndef FA_MDCT_H
#define FA_MDCT_H

#include "fa_internal.h"

typedef struct fa_mdct {
    int m;           /* coefficients in, 2m samples out */
    float* twiddle;  /* m/2 complex rotations before the FFT */
    float* post;     /* and after it */
    float* fft_tw;   /* m/4 complex FFT roots */
    int* rev;        /* bit reversal of m/2 */
    float* work;     /* m/2 complex */
    float* u;        /* m: the DCT-IV */
} fa_mdct;

fa_result fa_imdct_init(fa_mdct* t, int m);
void fa_imdct_free(fa_mdct* t);
/* out[n] = sum_k in[k] cos(pi/m (n + m/2 + 1/2)(k + 1/2)), n = 0..2m-1 (no scaling,
 * no window). Not reentrant for one fa_mdct (it has a work buffer). */
void fa_imdct(const fa_mdct* t, const float* in, float* out);
/* u[j] = sum_k in[k] cos(pi/m (j + 1/2)(k + 1/2)), j = 0..m-1 (DCT-IV, which the
 * IMDCT unfolds; codecs with other transform phases unfold it themselves). */
void fa_mdct_dct4(const fa_mdct* t, const float* in, float* u);

#endif
