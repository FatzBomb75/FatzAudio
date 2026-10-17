/* The DCT-IV behind the inverse MDCT (mdct.c). */
#ifndef FA_MDCT_H
#define FA_MDCT_H

#include "fa_internal.h"

typedef struct fa_mdct {
    int m;           /* coefficients in, 2m samples out */
    float* twiddle;  /* m/2 complex rotations before the FFT */
    float* post;     /* and after it */
    float* fft_tw;   /* FFT roots, per radix-4 pass */
    int* rev;        /* bit reversal of m/2 */
    float* work;     /* m/2 complex */
} fa_mdct;

fa_result fa_imdct_init(fa_mdct* t, int m);
void fa_imdct_free(fa_mdct* t);
/* u[j] = sum_k in[k] cos(pi/m (j + 1/2)(k + 1/2)), j = 0..m-1 (DCT-IV, no scaling).
 * The IMDCT out[n] = sum_k in[k] cos(pi/m (n + m/2 + 1/2)(k + 1/2)), n = 0..2m-1,
 * unfolds from it by its symmetries u[2m-1-j] = -u[j] and u[j+2m] = -u[j]:
 * out[n] = u[n + m/2], -u[3m/2 - 1 - n], -u[n - 3m/2] for n below m/2, 3m/2, 2m.
 * Codecs unfold it into their window themselves. Not reentrant for one fa_mdct
 * (it has a work buffer). */
void fa_mdct_dct4(const fa_mdct* t, const float* in, float* u);

#endif
