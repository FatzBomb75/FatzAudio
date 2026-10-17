/* FLAC output, written from the format specification (RFC 9639).
 *
 * Fixed-size blocks. For each block and channel the encoder tries a constant,
 * verbatim, the fixed predictors (order 0 to 4) and LPC (Tukey-windowed
 * autocorrelation, Levinson-Durbin, every order up to the level's maximum), and for
 * stereo the four channel decorrelations (left/right, left/side, side/right,
 * mid/side); the residual's Rice partitions are chosen per order with escapes for
 * noise. Everything is costed exactly (a Rice estimate from partition sums is an
 * upper bound of the real size), and the cheapest wins.
 *
 * STREAMINFO is completed at the end (length, frame sizes, MD5 of the audio) when
 * the output can seek; a forward-only output keeps "unknown" there, which the
 * format allows. */
#include "encode.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FLAC_MAX_LPC 12
#define FLAC_MAX_PART 8
#define FLAC_PADDING 4096

/* ---- MD5 (RFC 1321) ---------------------------------------------------------------- */

typedef struct md5_ctx {
    uint32_t a, b, c, d;
    uint64_t len;
    uint8_t buf[64];
    size_t fill;
} md5_ctx;

static void md5_init(md5_ctx* m)
{
    m->a = 0x67452301u; m->b = 0xefcdab89u; m->c = 0x98badcfeu; m->d = 0x10325476u;
    m->len = 0;
    m->fill = 0;
}

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void md5_block(md5_ctx* m, const uint8_t* p)
{
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
    };
    static const uint8_t R[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
    };
    uint32_t w[16], a = m->a, b = m->b, c = m->c, d = m->d;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] | ((uint32_t)p[i * 4 + 1] << 8) | ((uint32_t)p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);
    for (i = 0; i < 64; i++) {
        uint32_t f, t;
        int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
        else { f = c ^ (b | ~d); g = (7 * i) & 15; }
        t = d;
        d = c;
        c = b;
        b = b + ROL(a + f + K[i] + w[g], R[i]);
        a = t;
    }
    m->a += a; m->b += b; m->c += c; m->d += d;
}

static void md5_update(md5_ctx* m, const uint8_t* p, size_t n)
{
    m->len += n;
    while (n > 0) {
        size_t k = 64 - m->fill;
        if (k > n) k = n;
        memcpy(m->buf + m->fill, p, k);
        m->fill += k;
        p += k;
        n -= k;
        if (m->fill == 64) { md5_block(m, m->buf); m->fill = 0; }
    }
}

static void md5_final(md5_ctx* m, uint8_t out[16])
{
    uint64_t bits = m->len * 8;
    uint8_t pad[72] = { 0x80 };
    size_t n = (m->fill < 56) ? 56 - m->fill : 120 - m->fill;
    int i;
    for (i = 0; i < 8; i++) pad[n + i] = (uint8_t)(bits >> (8 * i));
    md5_update(m, pad, n + 8);
    for (i = 0; i < 4; i++) {
        out[i] = (uint8_t)(m->a >> (8 * i));
        out[4 + i] = (uint8_t)(m->b >> (8 * i));
        out[8 + i] = (uint8_t)(m->c >> (8 * i));
        out[12 + i] = (uint8_t)(m->d >> (8 * i));
    }
}

/* ---- bit writer ---------------------------------------------------------------------- */

typedef struct bitw {
    uint8_t* buf;
    size_t len;
    uint64_t acc;
    int nbits;
} bitw;

static void put(bitw* w, uint32_t v, int n)
{
    if (n <= 0) return;
    if (n < 32) v &= (1u << n) - 1;
    w->acc = (w->acc << n) | v;
    w->nbits += n;
    while (w->nbits >= 8) {
        w->nbits -= 8;
        w->buf[w->len++] = (uint8_t)(w->acc >> w->nbits);
    }
}

static void put_signed(bitw* w, int64_t v, int n) { put(w, (uint32_t)(uint64_t)v, n); }

static void put_unary(bitw* w, uint64_t q)
{
    while (q >= 32) { put(w, 0, 32); q -= 32; }
    put(w, 1, (int)q + 1);
}

static void align_byte(bitw* w)
{
    if (w->nbits) put(w, 0, 8 - w->nbits);
}

static uint8_t crc8(const uint8_t* p, size_t n)
{
    uint8_t c = 0;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++) c = (uint8_t)((c & 0x80) ? (c << 1) ^ 0x07 : c << 1);
    }
    return c;
}

static uint16_t crc16(const uint8_t* p, size_t n)
{
    uint16_t c = 0;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= (uint16_t)(p[i] << 8);
        for (k = 0; k < 8; k++) c = (uint16_t)((c & 0x8000) ? (c << 1) ^ 0x8005 : c << 1);
    }
    return c;
}

/* ---- residual coding --------------------------------------------------------------------- */

typedef struct rice_plan {
    int method;                  /* 0: 4-bit parameters (max 14), 1: 5-bit (max 30) */
    int order;                   /* partition order */
    uint8_t param[1 << FLAC_MAX_PART];
    uint8_t escape_bits[1 << FLAC_MAX_PART];   /* 0 = Rice coded */
    uint64_t bits;
} rice_plan;

static int bitlen64(uint64_t u)
{
    int n = 0;
    while (u) { n++; u >>= 1; }
    return n;
}

static uint64_t zigzag(int64_t r) { return ((uint64_t)r << 1) ^ (uint64_t)(r >> 63); }

/* Cheapest coding of one partition of `n` samples whose zigzag values sum to `sum`
 * (largest `umax`): Rice parameter, or an escape with raw bits. */
static uint64_t part_cost(uint64_t n, uint64_t sum, uint64_t umax, int method, uint8_t* out_k, uint8_t* out_esc)
{
    int kmax = method ? 30 : 14, pbits = method ? 5 : 4, k, k0, best_k = 0;
    uint64_t best = UINT64_MAX, esc;
    int raw;
    if (n == 0) { *out_k = 0; *out_esc = 0; return (uint64_t)pbits; }
    k0 = sum > n ? bitlen64(sum / n) - 1 : 0;
    for (k = k0 - 1; k <= k0 + 1; k++) {
        uint64_t c;
        if (k < 0 || k > kmax) continue;
        /* sum(u >> k) <= sum >> k: an upper bound of the coded size */
        c = (uint64_t)pbits + n * (uint64_t)(k + 1) + (sum >> k);
        if (c < best) { best = c; best_k = k; }
    }
    if (best == UINT64_MAX) { best_k = kmax; best = (uint64_t)pbits + n * (uint64_t)(kmax + 1) + (sum >> kmax); }
    raw = bitlen64(umax);
    if (raw < 1) raw = 1;
    esc = (uint64_t)pbits + 5 + n * (uint64_t)raw;
    if (raw <= 31 && esc < best) { *out_k = 0; *out_esc = (uint8_t)raw; return esc; }
    *out_k = (uint8_t)best_k;
    *out_esc = 0;
    return best;
}

/* Plans the residual r[pred_order .. n) (with r stored from index 0 for the whole
 * block; the first pred_order entries are warm-up and ignored). */
static void plan_residual(const int64_t* r, int n, int pred_order, int max_part, rice_plan* best)
{
    uint64_t sums[1 << FLAC_MAX_PART], maxs[1 << FLAC_MAX_PART];
    int top = 0, p, i, method;
    while (top < max_part && (n % (2 << top)) == 0 && (n >> (top + 1)) > pred_order) top++;
    /* finest partitions */
    {
        int parts = 1 << top, psize = n >> top, j;
        for (j = 0; j < parts; j++) {
            int start = j == 0 ? pred_order : j * psize, end = (j + 1) * psize;
            uint64_t s = 0, m = 0;
            for (i = start; i < end; i++) {
                uint64_t u = zigzag(r[i]);
                s += u;
                if (u > m) m = u;
            }
            sums[j] = s;
            maxs[j] = m;
        }
    }
    best->bits = UINT64_MAX;
    for (p = top; p >= 0; p--) {
        int parts = 1 << p, psize = n >> p, j;
        if (p < top) {
            for (j = 0; j < parts; j++) {
                sums[j] = sums[2 * j] + sums[2 * j + 1];
                maxs[j] = maxs[2 * j] > maxs[2 * j + 1] ? maxs[2 * j] : maxs[2 * j + 1];
            }
        }
        for (method = 0; method < 2; method++) {
            rice_plan cand;
            cand.method = method;
            cand.order = p;
            cand.bits = 6;   /* method + partition order */
            for (j = 0; j < parts; j++) {
                uint64_t cnt = (uint64_t)(j == 0 ? psize - pred_order : psize);
                cand.bits += part_cost(cnt, sums[j], maxs[j], method, &cand.param[j], &cand.escape_bits[j]);
            }
            if (cand.bits < best->bits) *best = cand;
        }
    }
}

static void write_residual(bitw* w, const int64_t* r, int n, int pred_order, const rice_plan* pl)
{
    int parts = 1 << pl->order, psize = n >> pl->order, j, i, pbits = pl->method ? 5 : 4;
    put(w, (uint32_t)pl->method, 2);
    put(w, (uint32_t)pl->order, 4);
    for (j = 0; j < parts; j++) {
        int start = j == 0 ? pred_order : j * psize, end = (j + 1) * psize;
        if (pl->escape_bits[j]) {
            int b = pl->escape_bits[j];
            put(w, (1u << pbits) - 1, pbits);
            put(w, (uint32_t)b, 5);
            for (i = start; i < end; i++) put_signed(w, r[i], b);
        } else {
            int k = pl->param[j];
            put(w, (uint32_t)k, pbits);
            for (i = start; i < end; i++) {
                uint64_t u = zigzag(r[i]);
                put_unary(w, u >> k);
                if (k) put(w, (uint32_t)(u & ((1ull << k) - 1)), k);
            }
        }
    }
}

/* ---- subframes ------------------------------------------------------------------------- */

enum { SUB_CONSTANT, SUB_VERBATIM, SUB_FIXED, SUB_LPC };

typedef struct subframe {
    int type, order;
    int precision, shift;
    int32_t coef[FLAC_MAX_LPC];
    rice_plan plan;
    uint64_t bits;
} subframe;

typedef struct flac_state {
    fa_enc_format f;
    uint32_t bits;               /* sample size: 16 or 24 */
    int level, blocksize, max_lpc, max_part, stereo_search, precision_search;
    int64_t frames, frame_number;
    uint32_t min_frame, max_frame;
    int64_t streaminfo_pos;
    uint32_t seed;
    md5_ctx md5;
    int32_t* pcm;                /* blocksize x channels, interleaved: the block being filled */
    int fill;
    int32_t* sig[4];             /* per signal: L, R, M, S (or one per channel) */
    int64_t* res;                /* residual scratch */
    int64_t* best_res[4];        /* residual of the chosen subframe per signal */
    double* win;                 /* Tukey window */
    double* wbuf;
    subframe sub[8];
    uint8_t* frame;              /* one encoded frame */
    size_t frame_cap;
    uint8_t* md5buf;
} flac_state;

static void residual_fixed(const int32_t* x, int n, int order, int64_t* r)
{
    int i;
    for (i = 0; i < order && i < n; i++) r[i] = 0;
    switch (order) {
    case 0: for (i = 0; i < n; i++) r[i] = x[i]; break;
    case 1: for (i = 1; i < n; i++) r[i] = (int64_t)x[i] - x[i - 1]; break;
    case 2: for (i = 2; i < n; i++) r[i] = (int64_t)x[i] - 2 * (int64_t)x[i - 1] + x[i - 2]; break;
    case 3: for (i = 3; i < n; i++) r[i] = (int64_t)x[i] - 3 * (int64_t)x[i - 1] + 3 * (int64_t)x[i - 2] - x[i - 3]; break;
    default:
        for (i = 4; i < n; i++)
            r[i] = (int64_t)x[i] - 4 * (int64_t)x[i - 1] + 6 * (int64_t)x[i - 2] - 4 * (int64_t)x[i - 3] + x[i - 4];
        break;
    }
}

/* 1 when the residual fits the 32-bit range decoders use. */
static int residual_lpc(const int32_t* x, int n, int order, const int32_t* q, int shift, int64_t* r)
{
    int i, j;
    for (i = 0; i < order; i++) r[i] = 0;
    for (i = order; i < n; i++) {
        int64_t sum = 0, v;
        for (j = 0; j < order; j++) sum += (int64_t)q[j] * x[i - j - 1];
        v = (int64_t)x[i] - (sum >> shift);
        if (v > INT32_MAX || v < INT32_MIN) return 0;
        r[i] = v;
    }
    return 1;
}

/* Quantizes LPC coefficients to `precision` bits; 0 if they cannot be. */
static int quantize_lpc(const double* lpc, int order, int precision, int32_t* q, int* out_shift)
{
    double cmax = 0.0, err = 0.0;
    int i, shift, log2c;
    int32_t qmax = (1 << (precision - 1)) - 1, qmin = -(1 << (precision - 1));
    for (i = 0; i < order; i++)
        if (fabs(lpc[i]) > cmax) cmax = fabs(lpc[i]);
    if (cmax <= 0.0) return 0;
    frexp(cmax, &log2c);
    shift = precision - 1 - log2c;
    if (shift > 15) shift = 15;
    if (shift < 0) return 0;   /* coefficients too large for the precision */
    for (i = 0; i < order; i++) {
        double v = lpc[i] * (double)(1 << shift) + err;
        long qi = lround(v);
        if (qi > qmax) qi = qmax;
        if (qi < qmin) qi = qmin;
        err = v - (double)qi;
        q[i] = (int32_t)qi;
    }
    *out_shift = shift;
    return 1;
}

/* Best subframe for signal x (bps bits per sample); its residual goes to `keep`. */
static void choose_subframe(flac_state* st, const int32_t* x, int n, int bps, subframe* best, int64_t* keep)
{
    int i, order;
    int64_t* r = st->res;
    subframe cand;
    int all_same = 1;
    for (i = 1; i < n; i++)
        if (x[i] != x[0]) { all_same = 0; break; }
    memset(best, 0, sizeof(*best));
    if (all_same) {
        best->type = SUB_CONSTANT;
        best->bits = 8 + (uint64_t)bps;
        return;
    }
    best->type = SUB_VERBATIM;
    best->bits = 8 + (uint64_t)n * bps;

    for (order = 0; order <= 4 && order < n; order++) {
        memset(&cand, 0, sizeof(cand));
        cand.type = SUB_FIXED;
        cand.order = order;
        residual_fixed(x, n, order, r);
        plan_residual(r, n, order, st->max_part, &cand.plan);
        cand.bits = 8 + (uint64_t)order * bps + cand.plan.bits;
        if (cand.bits < best->bits) {
            *best = cand;
            memcpy(keep, r, (size_t)n * sizeof(int64_t));
        }
    }

    if (st->max_lpc > 0 && n > st->max_lpc * 2) {
        double ac[FLAC_MAX_LPC + 1], lpc[FLAC_MAX_LPC][FLAC_MAX_LPC], a[FLAC_MAX_LPC], tmp[FLAC_MAX_LPC], e;
        int maxo = st->max_lpc, lag, k, p;
        int pmin = bps <= 16 ? 12 : 15, pmax = st->precision_search ? 15 : pmin;
        double* w = st->wbuf;
        const double* win = st->win;
        int whole = n == st->blocksize;
        for (i = 0; i < n; i++) w[i] = (double)x[i] * (whole ? win[i] : 1.0);
        for (lag = 0; lag <= maxo; lag++) {
            double s = 0.0;
            for (i = lag; i < n; i++) s += w[i] * w[i - lag];
            ac[lag] = s;
        }
        if (ac[0] <= 0.0) return;
        ac[0] *= 1.0 + 1e-9;   /* a little white noise: keeps the recursion stable */
        /* Levinson-Durbin: the coefficients of every order */
        e = ac[0];
        for (k = 0; k < maxo; k++) {
            double acc = ac[k + 1], refl;
            for (i = 0; i < k; i++) acc -= a[i] * ac[k - i];
            refl = acc / e;
            for (i = 0; i < k; i++) tmp[i] = a[i] - refl * a[k - 1 - i];
            for (i = 0; i < k; i++) a[i] = tmp[i];
            a[k] = refl;
            e *= 1.0 - refl * refl;
            for (i = 0; i <= k; i++) lpc[k][i] = a[i];
            if (e <= 0.0) { maxo = k + 1; break; }
        }
        for (order = 1; order <= maxo; order++) {
            for (p = pmin; p <= pmax; p++) {
                memset(&cand, 0, sizeof(cand));
                cand.type = SUB_LPC;
                cand.order = order;
                cand.precision = p;
                if (!quantize_lpc(lpc[order - 1], order, p, cand.coef, &cand.shift)) continue;
                if (!residual_lpc(x, n, order, cand.coef, cand.shift, r)) continue;
                plan_residual(r, n, order, st->max_part, &cand.plan);
                cand.bits = 8 + (uint64_t)order * bps + 4 + 5 + (uint64_t)order * p + cand.plan.bits;
                if (cand.bits < best->bits) {
                    *best = cand;
                    memcpy(keep, r, (size_t)n * sizeof(int64_t));
                }
            }
        }
    }
}

static void write_subframe(bitw* w, const int32_t* x, int n, int bps, const subframe* sf, const int64_t* res)
{
    int i;
    switch (sf->type) {
    case SUB_CONSTANT:
        put(w, 0x00, 8);
        put_signed(w, x[0], bps);
        break;
    case SUB_VERBATIM:
        put(w, 0x02, 8);
        for (i = 0; i < n; i++) put_signed(w, x[i], bps);
        break;
    case SUB_FIXED:
        put(w, (uint32_t)((0x08 | sf->order) << 1), 8);
        for (i = 0; i < sf->order; i++) put_signed(w, x[i], bps);
        write_residual(w, res, n, sf->order, &sf->plan);
        break;
    default:
        put(w, (uint32_t)((0x20 | (sf->order - 1)) << 1), 8);
        for (i = 0; i < sf->order; i++) put_signed(w, x[i], bps);
        put(w, (uint32_t)(sf->precision - 1), 4);
        put(w, (uint32_t)sf->shift, 5);
        for (i = 0; i < sf->order; i++) put_signed(w, sf->coef[i], sf->precision);
        write_residual(w, res, n, sf->order, &sf->plan);
        break;
    }
}

/* ---- frames ---------------------------------------------------------------------------- */

static void put_utf8_number(bitw* w, uint64_t v)
{
    if (v < 0x80) { put(w, (uint32_t)v, 8); return; }
    {
        int bytes = 2, i;
        while (bytes < 7 && v >= (1ull << (5 * bytes + 1))) bytes++;
        put(w, (uint32_t)((0xFF00u >> bytes) & 0xFF) | (uint32_t)(v >> (6 * (bytes - 1))), 8);
        for (i = bytes - 2; i >= 0; i--) put(w, 0x80 | (uint32_t)((v >> (6 * i)) & 0x3F), 8);
    }
}

static int rate_code(uint32_t rate, int* extra_bits, uint32_t* extra)
{
    static const uint32_t rates[12] = { 0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000 };
    int i;
    *extra_bits = 0;
    for (i = 1; i < 12; i++)
        if (rates[i] == rate) return i;
    if (rate % 1000 == 0 && rate / 1000 < 256) { *extra_bits = 8; *extra = rate / 1000; return 12; }
    if (rate < 65536) { *extra_bits = 16; *extra = rate; return 13; }
    if (rate % 10 == 0 && rate / 10 < 65536) { *extra_bits = 16; *extra = rate / 10; return 14; }
    return 0;   /* from STREAMINFO */
}

static fa_result encode_frame(flac_state* st, fa_enc_sink* s)
{
    int n = st->fill, ch = (int)st->f.channels, c, i, mode = 0, rbits, bs_code, bs_bits = 0;
    int bps = (int)st->bits;
    uint32_t rextra = 0;
    bitw w;
    size_t hdr_end;
    uint16_t crc;
    fa_result r;
    int rcode = rate_code(st->f.rate, &rbits, &rextra);
    subframe* sub = st->sub;

    /* MD5 of the samples as little-endian signed integers */
    {
        int64_t k, total = (int64_t)n * ch, b = 0;
        int bytes = bps / 8;
        for (k = 0; k < total; k++) {
            int32_t v = st->pcm[k];
            st->md5buf[b++] = (uint8_t)v;
            st->md5buf[b++] = (uint8_t)(v >> 8);
            if (bytes == 3) st->md5buf[b++] = (uint8_t)(v >> 16);
        }
        md5_update(&st->md5, st->md5buf, (size_t)b);
    }

    if (ch == 2 && st->stereo_search) {
        int32_t *L = st->sig[0], *R = st->sig[1], *M = st->sig[2], *S = st->sig[3];
        uint64_t cost[4], best;
        for (i = 0; i < n; i++) {
            int32_t l = st->pcm[2 * i], rr = st->pcm[2 * i + 1];
            L[i] = l;
            R[i] = rr;
            M[i] = (int32_t)(((int64_t)l + rr) >> 1);
            S[i] = l - rr;
        }
        for (c = 0; c < 4; c++) {
            choose_subframe(st, st->sig[c], n, c == 3 ? bps + 1 : bps, &sub[c], st->best_res[c]);
            cost[c] = sub[c].bits;
        }
        /* 0: L/R, 8: L/S, 9: S/R, 10: M/S */
        best = cost[0] + cost[1]; mode = 0;
        if (cost[0] + cost[3] < best) { best = cost[0] + cost[3]; mode = 8; }
        if (cost[3] + cost[1] < best) { best = cost[3] + cost[1]; mode = 9; }
        if (cost[2] + cost[3] < best) { mode = 10; }
    } else {
        /* Channels 0-3 are planned here; any further ones reuse signal 0's buffers
         * once channel 0 is written (below). */
        for (c = 0; c < ch && c < 4; c++) {
            for (i = 0; i < n; i++) st->sig[c][i] = st->pcm[i * ch + c];
            choose_subframe(st, st->sig[c], n, bps, &sub[c], st->best_res[c]);
        }
    }

    w.buf = st->frame;
    w.len = 0;
    w.acc = 0;
    w.nbits = 0;
    put(&w, 0xFFF8 >> 2, 14);                         /* sync, then reserved 0 */
    put(&w, 0, 1);
    put(&w, 0, 1);                                    /* fixed block size */
    if (n == 4096) bs_code = 12;
    else if (n == 1152) bs_code = 3;
    else if (n <= 256) { bs_code = 6; bs_bits = 8; }
    else { bs_code = 7; bs_bits = 16; }
    put(&w, (uint32_t)bs_code, 4);
    put(&w, (uint32_t)rcode, 4);
    put(&w, (uint32_t)(mode ? mode : ch - 1), 4);
    put(&w, bps == 16 ? 4 : 6, 3);
    put(&w, 0, 1);
    put_utf8_number(&w, (uint64_t)st->frame_number);
    if (bs_bits) put(&w, (uint32_t)(n - 1), bs_bits);
    if (rbits) put(&w, rextra, rbits);
    hdr_end = w.len;
    put(&w, crc8(w.buf, hdr_end), 8);

    if (ch == 2 && st->stereo_search) {
        static const int pick[11][2] = { { 0, 1 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 },
                                         { 0, 3 }, { 3, 1 }, { 2, 3 } };
        for (c = 0; c < 2; c++) {
            int sidx = pick[mode][c];
            write_subframe(&w, st->sig[sidx], n, sidx == 3 ? bps + 1 : bps, &sub[sidx], st->best_res[sidx]);
        }
    } else {
        for (c = 0; c < ch; c++) {
            if (c >= 4) {
                for (i = 0; i < n; i++) st->sig[0][i] = st->pcm[i * ch + c];
                choose_subframe(st, st->sig[0], n, bps, &sub[0], st->best_res[0]);
                write_subframe(&w, st->sig[0], n, bps, &sub[0], st->best_res[0]);
            } else {
                write_subframe(&w, st->sig[c], n, bps, &sub[c], st->best_res[c]);
            }
        }
    }
    align_byte(&w);
    crc = crc16(w.buf, w.len);
    put(&w, crc >> 8, 8);
    put(&w, crc & 0xFF, 8);

    r = fa_sink_write(s, w.buf, w.len);
    if (r != FA_OK) return r;
    if (st->min_frame == 0 || w.len < st->min_frame) st->min_frame = (uint32_t)w.len;
    if (w.len > st->max_frame) st->max_frame = (uint32_t)w.len;
    st->frames += n;
    st->frame_number++;
    st->fill = 0;
    return FA_OK;
}

/* ---- metadata ------------------------------------------------------------------------- */

static fa_result write_streaminfo(flac_state* st, fa_enc_sink* s, const uint8_t md5[16])
{
    uint8_t b[34];
    uint64_t total = (uint64_t)st->frames;
    uint32_t rate = st->f.rate, ch = st->f.channels - 1, bps = st->bits - 1;
    b[0] = (uint8_t)(st->blocksize >> 8); b[1] = (uint8_t)st->blocksize;
    b[2] = (uint8_t)(st->blocksize >> 8); b[3] = (uint8_t)st->blocksize;
    b[4] = (uint8_t)(st->min_frame >> 16); b[5] = (uint8_t)(st->min_frame >> 8); b[6] = (uint8_t)st->min_frame;
    b[7] = (uint8_t)(st->max_frame >> 16); b[8] = (uint8_t)(st->max_frame >> 8); b[9] = (uint8_t)st->max_frame;
    b[10] = (uint8_t)(rate >> 12);
    b[11] = (uint8_t)(rate >> 4);
    b[12] = (uint8_t)(((rate & 0xF) << 4) | (ch << 1) | (bps >> 4));
    b[13] = (uint8_t)(((bps & 0xF) << 4) | (uint32_t)((total >> 32) & 0xF));
    b[14] = (uint8_t)(total >> 24); b[15] = (uint8_t)(total >> 16); b[16] = (uint8_t)(total >> 8); b[17] = (uint8_t)total;
    memcpy(b + 18, md5, 16);
    return fa_sink_write(s, b, 34);
}

static fa_result block_header(fa_enc_sink* s, int last, int type, uint32_t size)
{
    uint8_t h[4] = { (uint8_t)((last ? 0x80 : 0) | type), (uint8_t)(size >> 16), (uint8_t)(size >> 8), (uint8_t)size };
    return fa_sink_write(s, h, 4);
}

static uint32_t comment_size(const char* key, const char* v) { return v ? (uint32_t)(4 + strlen(key) + 1 + strlen(v)) : 0; }

static fa_result write_comment(fa_enc_sink* s, const char* key, const char* v)
{
    fa_result r;
    if (!v) return FA_OK;
    r = fa_sink_write_le32(s, (uint32_t)(strlen(key) + 1 + strlen(v)));
    if (r == FA_OK) r = fa_sink_write(s, key, strlen(key));
    if (r == FA_OK) r = fa_sink_write(s, "=", 1);
    if (r == FA_OK) r = fa_sink_write(s, v, strlen(v));
    return r;
}

/* ---- the format ------------------------------------------------------------------------ */

static fa_result flac_check(const fa_enc_format* f)
{
    if (f->bits != 16 && f->bits != 24) return FA_ERROR_INVALID_ARG;
    if (f->quality > 8) return FA_ERROR_INVALID_ARG;
    if (f->channels > 8) return FA_ERROR_INVALID_ARG;
    if (f->rate > 655350) return FA_ERROR_INVALID_ARG;
    return FA_OK;
}

static void flac_choose(fa_enc_format* f)
{
    if (f->channels > 8) f->channels = 8;   /* the format's maximum */
}

static fa_result flac_begin(void** impl, const fa_enc_format* f, const fa_enc_tags* t, fa_enc_sink* s)
{
    static const struct { int lpc, part, stereo, block, prec; } levels[9] = {
        { 0, 3, 0, 1152, 0 }, { 0, 3, 1, 1152, 0 }, { 0, 4, 1, 1152, 0 },
        { 6, 4, 0, 4096, 0 }, { 8, 4, 1, 4096, 0 }, { 8, 5, 1, 4096, 0 },
        { 8, 6, 1, 4096, 0 }, { 12, 6, 1, 4096, 0 }, { 12, 8, 1, 4096, 1 }
    };
    flac_state* st = (flac_state*)calloc(1, sizeof(*st));
    char track[16];
    const char* trk = NULL;
    const char* vendor = "FatzAudio";
    uint32_t vc;
    int i, lv, bs;
    fa_result r;
    if (!st) return FA_ERROR_OUT_OF_MEMORY;
    *impl = st;
    st->f = *f;
    st->bits = f->bits;
    lv = f->quality < 0 ? 5 : f->quality;
    st->level = lv;
    st->blocksize = bs = levels[lv].block;
    st->max_lpc = levels[lv].lpc;
    st->max_part = levels[lv].part;
    st->stereo_search = levels[lv].stereo && f->channels == 2;
    st->precision_search = levels[lv].prec;
    st->seed = 0x2545F491u;
    md5_init(&st->md5);
    st->pcm = (int32_t*)malloc((size_t)bs * f->channels * sizeof(int32_t));
    for (i = 0; i < 4; i++) {
        st->sig[i] = (int32_t*)malloc((size_t)bs * sizeof(int32_t));
        st->best_res[i] = (int64_t*)malloc((size_t)bs * sizeof(int64_t));
        if (!st->sig[i] || !st->best_res[i]) return FA_ERROR_OUT_OF_MEMORY;
    }
    st->res = (int64_t*)malloc((size_t)bs * sizeof(int64_t));
    st->win = (double*)malloc((size_t)bs * sizeof(double));
    st->wbuf = (double*)malloc((size_t)bs * sizeof(double));
    /* the worst case is verbatim (+1 bit for side) plus headers */
    st->frame_cap = (size_t)bs * f->channels * 4 + 64 * f->channels + 64;
    st->frame = (uint8_t*)malloc(st->frame_cap);
    st->md5buf = (uint8_t*)malloc((size_t)bs * f->channels * 3);
    if (!st->pcm || !st->res || !st->win || !st->wbuf || !st->frame || !st->md5buf) return FA_ERROR_OUT_OF_MEMORY;
    /* Tukey window, half of it tapered */
    for (i = 0; i < bs; i++) {
        double x = (double)i / (double)(bs - 1), a = 0.5, v = 1.0;
        if (x < a / 2) v = 0.5 * (1 - cos(2 * 3.14159265358979323846 * x / a));
        else if (x > 1 - a / 2) v = 0.5 * (1 - cos(2 * 3.14159265358979323846 * (1 - x) / a));
        st->win[i] = v;
    }

    if (t->track) { snprintf(track, sizeof(track), "%u", t->track); trk = track; }
    vc = 4 + (uint32_t)strlen(vendor) + 4 + comment_size("TITLE", t->title) + comment_size("ARTIST", t->artist) +
         comment_size("ALBUM", t->album) + comment_size("GENRE", t->genre) + comment_size("DATE", t->date) +
         comment_size("COMMENT", t->comment) + comment_size("TRACKNUMBER", trk);
    {
        uint32_t count = (t->title != NULL) + (t->artist != NULL) + (t->album != NULL) + (t->genre != NULL) +
                         (t->date != NULL) + (t->comment != NULL) + (trk != NULL);
        uint8_t zero_md5[16] = { 0 };
        r = fa_sink_write(s, "fLaC", 4);
        if (r == FA_OK) r = block_header(s, 0, 0, 34);
        st->streaminfo_pos = s->pos;
        if (r == FA_OK) r = write_streaminfo(st, s, zero_md5);
        if (r == FA_OK) r = block_header(s, 0, 4, vc);
        if (r == FA_OK) r = fa_sink_write_le32(s, (uint32_t)strlen(vendor));
        if (r == FA_OK) r = fa_sink_write(s, vendor, strlen(vendor));
        if (r == FA_OK) r = fa_sink_write_le32(s, count);
        if (r == FA_OK) r = write_comment(s, "TITLE", t->title);
        if (r == FA_OK) r = write_comment(s, "ARTIST", t->artist);
        if (r == FA_OK) r = write_comment(s, "ALBUM", t->album);
        if (r == FA_OK) r = write_comment(s, "GENRE", t->genre);
        if (r == FA_OK) r = write_comment(s, "DATE", t->date);
        if (r == FA_OK) r = write_comment(s, "COMMENT", t->comment);
        if (r == FA_OK) r = write_comment(s, "TRACKNUMBER", trk);
        /* padding, so tag editors can add to the tags without rewriting the file */
        if (r == FA_OK) r = block_header(s, 1, 1, FLAC_PADDING);
        for (i = 0; r == FA_OK && i < FLAC_PADDING / 512; i++) {
            static const uint8_t z[512] = { 0 };
            r = fa_sink_write(s, z, sizeof(z));
        }
    }
    return r;
}

static fa_result flac_write(void* impl, const float* frames, int64_t count, fa_enc_sink* s)
{
    flac_state* st = (flac_state*)impl;
    uint32_t ch = st->f.channels;
    while (count > 0) {
        int64_t n = st->blocksize - st->fill;
        if (n > count) n = count;
        fa_enc_quantize(frames, st->pcm + (size_t)st->fill * ch, n * ch, st->bits, &st->seed);
        st->fill += (int)n;
        frames += n * ch;
        count -= n;
        if (st->fill == st->blocksize) {
            fa_result r = encode_frame(st, s);
            if (r != FA_OK) return r;
        }
    }
    return FA_OK;
}

static fa_result flac_finish(void* impl, fa_enc_sink* s)
{
    flac_state* st = (flac_state*)impl;
    uint8_t digest[16];
    fa_result r = FA_OK;
    int64_t end;
    if (st->fill > 0) r = encode_frame(st, s);
    if (r != FA_OK || !s->seekable) return r;
    md5_final(&st->md5, digest);
    end = s->pos;
    r = fa_sink_seek(s, st->streaminfo_pos);
    if (r == FA_OK) r = write_streaminfo(st, s, digest);
    if (r == FA_OK) r = fa_sink_seek(s, end);
    return r;
}

static void flac_free(void* impl)
{
    flac_state* st = (flac_state*)impl;
    int i;
    for (i = 0; i < 4; i++) { free(st->sig[i]); free(st->best_res[i]); }
    free(st->pcm);
    free(st->res);
    free(st->win);
    free(st->wbuf);
    free(st->frame);
    free(st->md5buf);
    free(st);
}

const fa_enc_backend fa_enc_flac = { flac_check, flac_choose, flac_begin, flac_write, flac_finish, flac_free };
