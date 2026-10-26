/* The FLAC encoder's search kernels: residual sizes of the fixed predictors and of
 * LPC candidates, per Rice partition, without storing the residual.
 *
 * LPC in 32 bits. When no residual and no prediction can leave the 32-bit range
 * (checked from the largest sample and the coefficients, before starting), every
 * sum is taken modulo 2^32 and comes out exact. SSE2 has no 32-bit multiply, so
 * it multiplies 16-bit pairs (pmaddwd): samples that fit 16 bits directly, wider
 * ones (up to 25 bits) as x = hi * 2^11 + lo with both halves in 16 bits; the two
 * sums are joined before the shift, exactly. NEON multiplies 32-bit lanes and
 * splits the same way. Candidates that could overflow take the plain 64-bit code,
 * which checks every sample. */
#include "enc_flac_dsp.h"
#include <string.h>
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#  include <emmintrin.h>
#  define FLAC_SSE2 1
#elif defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(_M_ARM64)
#  include <arm_neon.h>
#  define FLAC_NEON 1
#endif

/* x = hi * 2^SPLIT + lo: hi keeps 14 bits of a 25-bit sample, so with |q| <= 2^14
 * and at most 12 of them the sum of q * hi stays below 2^31. */
#define SPLIT 11

void fa_flac_signal_prepare(flac_signal* s, const int32_t* x, int n, int16_t* x16, int16_t* hi, int16_t* lo)
{
    int32_t mn = 0, mx = 0;
    int i;
    for (i = 0; i < n; i++) {
        if (x[i] < mn) mn = x[i];
        if (x[i] > mx) mx = x[i];
    }
    s->x = x;
    s->n = n;
    s->xmax = -(int64_t)mn > mx ? -(int64_t)mn : mx;
    s->fits16 = mn >= -32768 && mx <= 32767;
    s->x16 = x16;
    s->hi = hi;
    s->lo = lo;
    s->ready = 0;
}

/* ---- fixed predictors -------------------------------------------------------------- */

/* Samples i >= 4: every order's residual from the differences of differences. */
static void fixed_scalar(const int32_t* x, int i, int end, uint64_t s[5], uint32_t b[5])
{
    for (; i < end; i++) {
        int32_t d1 = x[i] - x[i - 1], e1 = x[i - 1] - x[i - 2], f1 = x[i - 2] - x[i - 3], g1 = x[i - 3] - x[i - 4];
        int32_t d2 = d1 - e1, e2 = e1 - f1, f2 = f1 - g1;
        int32_t d3 = d2 - e2, e3 = e2 - f2;
        int32_t r[5];
        int o;
        r[0] = x[i]; r[1] = d1; r[2] = d2; r[3] = d3; r[4] = d3 - e3;
        for (o = 0; o < 5; o++) {
            uint32_t u = fa_flac_zigzag(r[o]);
            s[o] += u;
            b[o] |= u;
        }
    }
}

#if FLAC_SSE2
static __m128i zigzag4(__m128i v) { return _mm_xor_si128(_mm_slli_epi32(v, 1), _mm_srai_epi32(v, 31)); }

/* acc (two 64-bit lanes) += the four 32-bit lanes of u */
static __m128i widen_add(__m128i acc, __m128i u)
{
    const __m128i z = _mm_setzero_si128();
    return _mm_add_epi64(acc, _mm_add_epi64(_mm_unpacklo_epi32(u, z), _mm_unpackhi_epi32(u, z)));
}

/* acc: 64-bit lanes, or (narrow) 32-bit lanes that cannot have overflowed */
static void reduce(__m128i acc, int narrow, __m128i bits, uint64_t* s, uint32_t* b)
{
    uint64_t t[2];
    uint32_t o[4];
    if (narrow) acc = widen_add(_mm_setzero_si128(), acc);
    _mm_storeu_si128((__m128i*)t, acc);
    _mm_storeu_si128((__m128i*)o, bits);
    *s += t[0] + t[1];
    *b |= o[0] | o[1] | o[2] | o[3];
}

static void fixed_vec(const int32_t* x, int i, __m128i r[5])
{
    __m128i x0 = _mm_loadu_si128((const __m128i*)(x + i)), x1 = _mm_loadu_si128((const __m128i*)(x + i - 1));
    __m128i x2 = _mm_loadu_si128((const __m128i*)(x + i - 2)), x3 = _mm_loadu_si128((const __m128i*)(x + i - 3));
    __m128i x4 = _mm_loadu_si128((const __m128i*)(x + i - 4));
    __m128i d1 = _mm_sub_epi32(x0, x1), e1 = _mm_sub_epi32(x1, x2), f1 = _mm_sub_epi32(x2, x3), g1 = _mm_sub_epi32(x3, x4);
    __m128i d2 = _mm_sub_epi32(d1, e1), e2 = _mm_sub_epi32(e1, f1), f2 = _mm_sub_epi32(f1, g1);
    __m128i d3 = _mm_sub_epi32(d2, e2), e3 = _mm_sub_epi32(e2, f2);
    r[0] = zigzag4(x0); r[1] = zigzag4(d1); r[2] = zigzag4(d2); r[3] = zigzag4(d3);
    r[4] = zigzag4(_mm_sub_epi32(d3, e3));
}

static int fixed_simd(const int32_t* x, int i, int end, int narrow, uint64_t s[5], uint32_t b[5])
{
    __m128i acc[5], bits[5], u[5];
    int o;
    for (o = 0; o < 5; o++) acc[o] = bits[o] = _mm_setzero_si128();
    if (narrow) {
        for (; i + 4 <= end; i += 4) {
            fixed_vec(x, i, u);
            for (o = 0; o < 5; o++) { bits[o] = _mm_or_si128(bits[o], u[o]); acc[o] = _mm_add_epi32(acc[o], u[o]); }
        }
    } else {
        for (; i + 4 <= end; i += 4) {
            fixed_vec(x, i, u);
            for (o = 0; o < 5; o++) { bits[o] = _mm_or_si128(bits[o], u[o]); acc[o] = widen_add(acc[o], u[o]); }
        }
    }
    for (o = 0; o < 5; o++) reduce(acc[o], narrow, bits[o], &s[o], &b[o]);
    return i;
}
#elif FLAC_NEON
static uint32x4_t zigzag4(int32x4_t v)
{
    return vreinterpretq_u32_s32(veorq_s32(vshlq_n_s32(v, 1), vshrq_n_s32(v, 31)));
}

static void reduce(uint64x2_t acc, uint32x4_t bits, uint64_t* s, uint32_t* b)
{
    *s += vgetq_lane_u64(acc, 0) + vgetq_lane_u64(acc, 1);
    *b |= vgetq_lane_u32(bits, 0) | vgetq_lane_u32(bits, 1) | vgetq_lane_u32(bits, 2) | vgetq_lane_u32(bits, 3);
}

/* NEON adds pairs into 64-bit lanes in one instruction, so it needs no narrow sums. */
static int fixed_simd(const int32_t* x, int i, int end, int narrow, uint64_t s[5], uint32_t b[5])
{
    uint64x2_t acc[5];
    uint32x4_t bits[5];
    int o;
    (void)narrow;
    for (o = 0; o < 5; o++) { acc[o] = vdupq_n_u64(0); bits[o] = vdupq_n_u32(0); }
    for (; i + 4 <= end; i += 4) {
        int32x4_t x0 = vld1q_s32(x + i), x1 = vld1q_s32(x + i - 1), x2 = vld1q_s32(x + i - 2);
        int32x4_t x3 = vld1q_s32(x + i - 3), x4 = vld1q_s32(x + i - 4);
        int32x4_t d1 = vsubq_s32(x0, x1), e1 = vsubq_s32(x1, x2), f1 = vsubq_s32(x2, x3), g1 = vsubq_s32(x3, x4);
        int32x4_t d2 = vsubq_s32(d1, e1), e2 = vsubq_s32(e1, f1), f2 = vsubq_s32(f1, g1);
        int32x4_t d3 = vsubq_s32(d2, e2), e3 = vsubq_s32(e2, f2);
        int32x4_t r[5];
        r[0] = x0; r[1] = d1; r[2] = d2; r[3] = d3; r[4] = vsubq_s32(d3, e3);
        for (o = 0; o < 5; o++) {
            uint32x4_t u = zigzag4(r[o]);
            bits[o] = vorrq_u32(bits[o], u);
            acc[o] = vpadalq_u32(acc[o], u);
        }
    }
    for (o = 0; o < 5; o++) reduce(acc[o], bits[o], &s[o], &b[o]);
    return i;
}
#endif

void fa_flac_fixed_sums(const int32_t* x, int n, int top, int64_t xmax, flac_sums out[5])
{
    int parts = 1 << top, psize = n >> top, i, j, o;
    /* |order 4 residual| <= 16 xmax: when a lane's share of a partition cannot
     * reach 2^32, the vector sums stay 32 bits wide */
    int narrow = (32 * xmax + 1) * ((psize + 3) / 4) < ((int64_t)1 << 32);
    for (o = 0; o < 5; o++) {
        memset(out[o].sum, 0, (size_t)parts * sizeof(out[o].sum[0]));
        memset(out[o].bits, 0, (size_t)parts * sizeof(out[o].bits[0]));
    }
    /* the first samples, where only the lower orders have a residual */
    for (i = 0; i < n && i < 4; i++) {
        int32_t r[5];
        j = i / psize;
        r[0] = x[i];
        if (i >= 1) r[1] = x[i] - x[i - 1];
        if (i >= 2) r[2] = x[i] - 2 * x[i - 1] + x[i - 2];
        if (i >= 3) r[3] = x[i] - 3 * x[i - 1] + 3 * x[i - 2] - x[i - 3];
        for (o = 0; o <= i; o++) {
            uint32_t u = fa_flac_zigzag(r[o]);
            out[o].sum[j] += u;
            out[o].bits[j] |= u;
        }
    }
    for (j = 0; j < parts; j++) {
        int start = j * psize, end = start + psize;
        uint64_t s[5] = { 0, 0, 0, 0, 0 };
        uint32_t b[5] = { 0, 0, 0, 0, 0 };
        if (start < 4) start = 4;
        if (start >= end) continue;
#if FLAC_SSE2 || FLAC_NEON
        start = fixed_simd(x, start, end, narrow, s, b);
#else
        (void)narrow;
#endif
        fixed_scalar(x, start, end, s, b);
        for (o = 0; o < 5; o++) {
            out[o].sum[j] += s[o];
            out[o].bits[j] |= b[o];
        }
    }
}

/* ---- LPC ------------------------------------------------------------------------------ */

/* Every sample checked, in 64 bits: candidates whose bound allows an overflow. */
static int lpc_checked(const int32_t* x, int order, const int32_t* q, int shift, int count, int stride, int len,
                       flac_sums* out, int32_t* res)
{
    int p, i, j;
    for (p = 0; p < count; p++) {
        int start = p == 0 ? order : p * stride, end = p * stride + len;
        uint64_t s = 0;
        uint32_t b = 0;
        for (i = start; i < end; i++) {
            int64_t sum = 0, v;
            uint32_t u;
            for (j = 0; j < order; j++) sum += (int64_t)q[j] * x[i - j - 1];
            v = (int64_t)x[i] - (sum >> shift);
            if (v > INT32_MAX || v < INT32_MIN) return 0;
            if (res) res[i] = (int32_t)v;
            u = fa_flac_zigzag((int32_t)v);
            s += u;
            b |= u;
        }
        out->sum[p] = s;
        out->bits[p] = b;
    }
    return 1;
}

/* Samples [i, end) of a candidate that cannot overflow, one at a time. */
static void lpc_scalar(const int32_t* x, int i, int end, int order, const int32_t* q, int shift, uint64_t* s, uint32_t* b,
                       int32_t* res)
{
    int j;
    for (; i < end; i++) {
        int64_t sum = 0;
        int32_t v;
        uint32_t u;
        for (j = 0; j < order; j++) sum += (int64_t)q[j] * x[i - j - 1];
        v = x[i] - (int32_t)(sum >> shift);
        if (res) res[i] = v;
        u = fa_flac_zigzag(v);
        *s += u;
        *b |= u;
    }
}

#if FLAC_SSE2
/* Eight samples a step: even and odd ones in their own sums, each pmaddwd taking
 * two coefficients. `direct`: the 16-bit copy, else hi/lo. */
typedef struct lpc_simd_ctx {
    __m128i c[FLAC_MAX_LPC / 2];
    int np, direct, narrow, shift;
} lpc_simd_ctx;

static void lpc_simd_init(lpc_simd_ctx* k, int order, const int32_t* q)
{
    int j;
    k->np = (order + 1) >> 1;
    for (j = 0; j < k->np; j++) {
        /* low half of a lane: the older sample, x[i-2-2j], whose coefficient is q[2j+1] */
        uint32_t newer = (uint16_t)(int16_t)q[2 * j], older = 2 * j + 1 < order ? (uint16_t)(int16_t)q[2 * j + 1] : 0;
        k->c[j] = _mm_set1_epi32((int32_t)(newer << 16 | older));
    }
}

/* The predictions of samples i to i + 7. */
static void lpc_predict(const lpc_simd_ctx* kc, const flac_signal* sg, int i, __m128i pred[2])
{
    const __m128i* c = kc->c;
    const __m128i sh = _mm_cvtsi32_si128(kc->shift);
    int np = kc->np, k, h;
    if (kc->direct) {
        const int16_t* x16 = sg->x16 + i;
        __m128i e = _mm_setzero_si128(), o = _mm_setzero_si128();
        for (k = 0; k < np; k++) {
            e = _mm_add_epi32(e, _mm_madd_epi16(_mm_loadu_si128((const __m128i*)(x16 - 2 - 2 * k)), c[k]));
            o = _mm_add_epi32(o, _mm_madd_epi16(_mm_loadu_si128((const __m128i*)(x16 - 1 - 2 * k)), c[k]));
        }
        pred[0] = _mm_sra_epi32(_mm_unpacklo_epi32(e, o), sh);
        pred[1] = _mm_sra_epi32(_mm_unpackhi_epi32(e, o), sh);
    } else {
        const int16_t *hi = sg->hi + i, *lo = sg->lo + i;
        __m128i eh = _mm_setzero_si128(), oh = eh, el = eh, ol = eh;
        for (k = 0; k < np; k++) {
            eh = _mm_add_epi32(eh, _mm_madd_epi16(_mm_loadu_si128((const __m128i*)(hi - 2 - 2 * k)), c[k]));
            oh = _mm_add_epi32(oh, _mm_madd_epi16(_mm_loadu_si128((const __m128i*)(hi - 1 - 2 * k)), c[k]));
            el = _mm_add_epi32(el, _mm_madd_epi16(_mm_loadu_si128((const __m128i*)(lo - 2 - 2 * k)), c[k]));
            ol = _mm_add_epi32(ol, _mm_madd_epi16(_mm_loadu_si128((const __m128i*)(lo - 1 - 2 * k)), c[k]));
        }
        for (h = 0; h < 2; h++) {
            __m128i A = h ? _mm_unpackhi_epi32(eh, oh) : _mm_unpacklo_epi32(eh, oh);
            __m128i B = h ? _mm_unpackhi_epi32(el, ol) : _mm_unpacklo_epi32(el, ol);
            /* floor((A * 2^11 + B) / 2^shift) */
            if (kc->shift <= SPLIT)
                pred[h] = _mm_add_epi32(_mm_sll_epi32(A, _mm_cvtsi32_si128(SPLIT - kc->shift)), _mm_sra_epi32(B, sh));
            else
                pred[h] = _mm_sra_epi32(_mm_add_epi32(A, _mm_srai_epi32(B, SPLIT)), _mm_cvtsi32_si128(kc->shift - SPLIT));
        }
    }
}

static int lpc_simd(const lpc_simd_ctx* kc, const flac_signal* sg, int i, int end, uint64_t* s, uint32_t* b, int32_t* res)
{
    __m128i acc = _mm_setzero_si128(), bits = _mm_setzero_si128();
    const int32_t* x = sg->x;
    int h;
    for (; i + 8 <= end; i += 8) {
        __m128i pred[2];
        lpc_predict(kc, sg, i, pred);
        for (h = 0; h < 2; h++) {
            __m128i v = _mm_sub_epi32(_mm_loadu_si128((const __m128i*)(x + i + 4 * h)), pred[h]), u = zigzag4(v);
            if (res) _mm_storeu_si128((__m128i*)(res + i + 4 * h), v);
            bits = _mm_or_si128(bits, u);
            acc = kc->narrow ? _mm_add_epi32(acc, u) : widen_add(acc, u);
        }
    }
    reduce(acc, kc->narrow, bits, s, b);
    return i;
}
#elif FLAC_NEON
/* Four samples a step. */
typedef struct lpc_simd_ctx {
    int32x4_t c[FLAC_MAX_LPC];
    int order, direct, narrow, shift;
} lpc_simd_ctx;

static void lpc_simd_init(lpc_simd_ctx* k, int order, const int32_t* q)
{
    int j;
    k->order = order;
    for (j = 0; j < order; j++) k->c[j] = vdupq_n_s32(q[j]);
}

static int lpc_simd(const lpc_simd_ctx* kc, const flac_signal* sg, int i, int end, uint64_t* s, uint32_t* b, int32_t* res)
{
    uint64x2_t acc = vdupq_n_u64(0);
    uint32x4_t bits = vdupq_n_u32(0);
    const int32x4_t sh = vdupq_n_s32(-kc->shift), mask = vdupq_n_s32((1 << SPLIT) - 1);
    const int32x4_t* c = kc->c;
    const int32_t* x = sg->x;
    int order = kc->order, shift = kc->shift, j;
    for (; i + 4 <= end; i += 4) {
        int32x4_t pred, v;
        uint32x4_t u;
        if (kc->direct) {
            int32x4_t sum = vdupq_n_s32(0);
            for (j = 0; j < order; j++) sum = vmlaq_s32(sum, vld1q_s32(x + i - 1 - j), c[j]);
            pred = vshlq_s32(sum, sh);
        } else {
            int32x4_t A = vdupq_n_s32(0), B = A;
            for (j = 0; j < order; j++) {
                int32x4_t xv = vld1q_s32(x + i - 1 - j);
                A = vmlaq_s32(A, vshrq_n_s32(xv, SPLIT), c[j]);
                B = vmlaq_s32(B, vandq_s32(xv, mask), c[j]);
            }
            /* floor((A * 2^11 + B) / 2^shift) */
            if (shift <= SPLIT)
                pred = vaddq_s32(vshlq_s32(A, vdupq_n_s32(SPLIT - shift)), vshlq_s32(B, sh));
            else
                pred = vshlq_s32(vaddq_s32(A, vshrq_n_s32(B, SPLIT)), vdupq_n_s32(SPLIT - shift));
        }
        v = vsubq_s32(vld1q_s32(x + i), pred);
        if (res) vst1q_s32(res + i, v);
        u = zigzag4(v);
        bits = vorrq_u32(bits, u);
        acc = vpadalq_u32(acc, u);
    }
    reduce(acc, bits, s, b);
    return i;
}
#endif

int fa_flac_lpc_sums(flac_signal* sg, int order, const int32_t* q, int shift, int count, int stride, int len,
                     flac_sums* out, int32_t* res)
{
    const int32_t* x = sg->x;
    int p, j;
    int64_t qsum = 0, bound;
#if FLAC_SSE2 || FLAC_NEON
    lpc_simd_ctx kc;
#endif
    for (j = 0; j < order; j++) qsum += q[j] < 0 ? -(int64_t)q[j] : q[j];
    /* |prediction| <= (qsum * xmax >> shift) + 1, and |residual| <= that + xmax */
    if (res)
        for (j = 0; j < order; j++) res[j] = 0;
    bound = sg->xmax + ((qsum * sg->xmax) >> shift) + 1;
    if (bound >= ((int64_t)1 << 31)) return lpc_checked(x, order, q, shift, count, stride, len, out, res);
#if FLAC_SSE2 || FLAC_NEON
    lpc_simd_init(&kc, order, q);
    kc.shift = shift;
    /* the unshifted sum itself fits 32 bits */
    kc.direct = qsum * sg->xmax < ((int64_t)1 << 31);
#  if FLAC_SSE2
    kc.direct = kc.direct && sg->fits16;
    if (kc.direct && !(sg->ready & 1)) {
        for (j = 0; j < sg->n; j++) sg->x16[j] = (int16_t)x[j];
        sg->ready |= 1;
    }
    if (!kc.direct && !(sg->ready & 2)) {
        for (j = 0; j < sg->n; j++) {
            sg->hi[j] = (int16_t)(x[j] >> SPLIT);
            sg->lo[j] = (int16_t)(x[j] & ((1 << SPLIT) - 1));
        }
        sg->ready |= 2;
    }
#  endif
    /* zigzag values are at most 2 * bound; a lane takes a quarter of a range */
    kc.narrow = (2 * bound + 1) * ((len + 3) / 4) < ((int64_t)1 << 32);
#endif
    for (p = 0; p < count; p++) {
        int start = p == 0 ? order : p * stride, end = p * stride + len, i = start;
        uint64_t s = 0;
        uint32_t b = 0;
#if FLAC_SSE2 || FLAC_NEON
        /* the vector loads reach back order + 1 samples */
        if (i < order + 1) {
            lpc_scalar(x, i, order + 1 < end ? order + 1 : end, order, q, shift, &s, &b, res);
            i = order + 1;
        }
        if (i < end) i = lpc_simd(&kc, sg, i, end, &s, &b, res);
#endif
        if (i < end) lpc_scalar(x, i, end, order, q, shift, &s, &b, res);
        out->sum[p] = s;
        out->bits[p] = b;
    }
    return 1;
}
