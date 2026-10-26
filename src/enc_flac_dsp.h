/* The FLAC encoder's kernels (enc_flac_dsp.c): what each predictor's residual
 * would cost, per Rice partition, without storing it; and the LPC residual of
 * the predictor chosen. */
#ifndef FA_ENC_FLAC_DSP_H
#define FA_ENC_FLAC_DSP_H

#include <stdint.h>

#define FLAC_MAX_LPC 12
#define FLAC_MAX_PART 8

/* The Rice coding's unsigned value of a residual (0, -1, 1, -2, ... to 0, 1, 2, 3, ...). */
static inline uint32_t fa_flac_zigzag(int32_t r) { return ((uint32_t)r << 1) ^ (uint32_t)(r >> 31); }

/* Per Rice partition of a residual: the sum of its zigzag values and their OR
 * (whose bit length is the largest value's). */
typedef struct flac_sums {
    uint64_t sum[1 << FLAC_MAX_PART];
    uint32_t bits[1 << FLAC_MAX_PART];
} flac_sums;

/* One channel signal, readied once for every candidate. */
typedef struct flac_signal {
    const int32_t* x;
    int n;
    int64_t xmax;                /* largest |x| */
    int fits16;                  /* every sample fits int16 */
    int16_t* x16;                /* SSE2, filled when first needed: x, when fits16 */
    int16_t* hi;                 /* x >> 11 */
    int16_t* lo;                 /* x & 2047 */
    int ready;                   /* 1: x16 filled, 2: hi and lo */
} flac_signal;

/* Fills xmax and fits16; the three buffers hold n samples each. */
void fa_flac_signal_prepare(flac_signal* s, const int32_t* x, int n, int16_t* x16, int16_t* hi, int16_t* lo);

/* The fixed predictors of orders 0 to 4 at once, in partitions of n >> top
 * samples (order o leaves out its first o samples). xmax: the largest |x|. */
void fa_flac_fixed_sums(const int32_t* x, int n, int top, int64_t xmax, flac_sums out[5]);

/* The LPC residual x[i] - (sum q[j] x[i-1-j] >> shift), summed over `count`
 * ranges: range j is samples [j * stride, j * stride + len), the first from
 * `order` on (partitions: stride = len = n >> partition order). Samples of at
 * most 25 bits, orders up to 12, |q| at most 2^14. With res, the residual is
 * stored there too (res[0 .. order) = 0). 0 (out and res undefined) when a
 * residual leaves the 32-bit range decoders use. */
int fa_flac_lpc_sums(flac_signal* s, int order, const int32_t* q, int shift, int count, int stride, int len,
                     flac_sums* out, int32_t* res);

#endif
