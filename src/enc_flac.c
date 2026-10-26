/* FLAC output, written from the format specification (RFC 9639).
 *
 * Fixed-size blocks. For each block and channel the encoder tries a constant,
 * verbatim, the fixed predictors (order 0 to 4) and LPC (Tukey-windowed
 * autocorrelation, Levinson-Durbin, every order up to the level's maximum), and for
 * stereo the four channel decorrelations (left/right, left/side, side/right,
 * mid/side); the residual's Rice partitions are chosen per order with escapes for
 * noise. A predictor is costed from per-partition sums of its residual
 * (enc_flac_dsp.c) without storing it, and the cost of a Rice partition is exact
 * (closed-form parameter, no search). At levels 3 and up the LPC candidates are
 * first costed on 8 short pieces of the block and only the best few in full; the
 * residual is computed once more for the predictor written.
 *
 * STREAMINFO is completed at the end (length, frame sizes, MD5 of the audio) when
 * the output can seek; a forward-only output keeps "unknown" there, which the
 * format allows. */
#include "encode.h"
#include "enc_flac_dsp.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef _MSC_VER
#include <intrin.h>
#endif

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

/* The four rounds written out, so every rotation and word is a constant. */
#define MD5_F(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define MD5_G(x, y, z) (((x) & (z)) + ((y) & ~(z)))
#define MD5_H(x, y, z) ((x) ^ (y) ^ (z))
#define MD5_I(x, y, z) ((y) ^ ((x) | ~(z)))
#define MD5_STEP(f, a, b, c, d, x, s, k) (a) += (x) + (k); (a) += f((b), (c), (d)); (a) = ROL((a), (s)) + (b);

static void md5_block(md5_ctx* m, const uint8_t* p)
{
    uint32_t w[16], a = m->a, b = m->b, c = m->c, d = m->d;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] | ((uint32_t)p[i * 4 + 1] << 8) | ((uint32_t)p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);
    MD5_STEP(MD5_F, a, b, c, d, w[0], 7, 0xd76aa478u)
    MD5_STEP(MD5_F, d, a, b, c, w[1], 12, 0xe8c7b756u)
    MD5_STEP(MD5_F, c, d, a, b, w[2], 17, 0x242070dbu)
    MD5_STEP(MD5_F, b, c, d, a, w[3], 22, 0xc1bdceeeu)
    MD5_STEP(MD5_F, a, b, c, d, w[4], 7, 0xf57c0fafu)
    MD5_STEP(MD5_F, d, a, b, c, w[5], 12, 0x4787c62au)
    MD5_STEP(MD5_F, c, d, a, b, w[6], 17, 0xa8304613u)
    MD5_STEP(MD5_F, b, c, d, a, w[7], 22, 0xfd469501u)
    MD5_STEP(MD5_F, a, b, c, d, w[8], 7, 0x698098d8u)
    MD5_STEP(MD5_F, d, a, b, c, w[9], 12, 0x8b44f7afu)
    MD5_STEP(MD5_F, c, d, a, b, w[10], 17, 0xffff5bb1u)
    MD5_STEP(MD5_F, b, c, d, a, w[11], 22, 0x895cd7beu)
    MD5_STEP(MD5_F, a, b, c, d, w[12], 7, 0x6b901122u)
    MD5_STEP(MD5_F, d, a, b, c, w[13], 12, 0xfd987193u)
    MD5_STEP(MD5_F, c, d, a, b, w[14], 17, 0xa679438eu)
    MD5_STEP(MD5_F, b, c, d, a, w[15], 22, 0x49b40821u)
    MD5_STEP(MD5_G, a, b, c, d, w[1], 5, 0xf61e2562u)
    MD5_STEP(MD5_G, d, a, b, c, w[6], 9, 0xc040b340u)
    MD5_STEP(MD5_G, c, d, a, b, w[11], 14, 0x265e5a51u)
    MD5_STEP(MD5_G, b, c, d, a, w[0], 20, 0xe9b6c7aau)
    MD5_STEP(MD5_G, a, b, c, d, w[5], 5, 0xd62f105du)
    MD5_STEP(MD5_G, d, a, b, c, w[10], 9, 0x02441453u)
    MD5_STEP(MD5_G, c, d, a, b, w[15], 14, 0xd8a1e681u)
    MD5_STEP(MD5_G, b, c, d, a, w[4], 20, 0xe7d3fbc8u)
    MD5_STEP(MD5_G, a, b, c, d, w[9], 5, 0x21e1cde6u)
    MD5_STEP(MD5_G, d, a, b, c, w[14], 9, 0xc33707d6u)
    MD5_STEP(MD5_G, c, d, a, b, w[3], 14, 0xf4d50d87u)
    MD5_STEP(MD5_G, b, c, d, a, w[8], 20, 0x455a14edu)
    MD5_STEP(MD5_G, a, b, c, d, w[13], 5, 0xa9e3e905u)
    MD5_STEP(MD5_G, d, a, b, c, w[2], 9, 0xfcefa3f8u)
    MD5_STEP(MD5_G, c, d, a, b, w[7], 14, 0x676f02d9u)
    MD5_STEP(MD5_G, b, c, d, a, w[12], 20, 0x8d2a4c8au)
    MD5_STEP(MD5_H, a, b, c, d, w[5], 4, 0xfffa3942u)
    MD5_STEP(MD5_H, d, a, b, c, w[8], 11, 0x8771f681u)
    MD5_STEP(MD5_H, c, d, a, b, w[11], 16, 0x6d9d6122u)
    MD5_STEP(MD5_H, b, c, d, a, w[14], 23, 0xfde5380cu)
    MD5_STEP(MD5_H, a, b, c, d, w[1], 4, 0xa4beea44u)
    MD5_STEP(MD5_H, d, a, b, c, w[4], 11, 0x4bdecfa9u)
    MD5_STEP(MD5_H, c, d, a, b, w[7], 16, 0xf6bb4b60u)
    MD5_STEP(MD5_H, b, c, d, a, w[10], 23, 0xbebfbc70u)
    MD5_STEP(MD5_H, a, b, c, d, w[13], 4, 0x289b7ec6u)
    MD5_STEP(MD5_H, d, a, b, c, w[0], 11, 0xeaa127fau)
    MD5_STEP(MD5_H, c, d, a, b, w[3], 16, 0xd4ef3085u)
    MD5_STEP(MD5_H, b, c, d, a, w[6], 23, 0x04881d05u)
    MD5_STEP(MD5_H, a, b, c, d, w[9], 4, 0xd9d4d039u)
    MD5_STEP(MD5_H, d, a, b, c, w[12], 11, 0xe6db99e5u)
    MD5_STEP(MD5_H, c, d, a, b, w[15], 16, 0x1fa27cf8u)
    MD5_STEP(MD5_H, b, c, d, a, w[2], 23, 0xc4ac5665u)
    MD5_STEP(MD5_I, a, b, c, d, w[0], 6, 0xf4292244u)
    MD5_STEP(MD5_I, d, a, b, c, w[7], 10, 0x432aff97u)
    MD5_STEP(MD5_I, c, d, a, b, w[14], 15, 0xab9423a7u)
    MD5_STEP(MD5_I, b, c, d, a, w[5], 21, 0xfc93a039u)
    MD5_STEP(MD5_I, a, b, c, d, w[12], 6, 0x655b59c3u)
    MD5_STEP(MD5_I, d, a, b, c, w[3], 10, 0x8f0ccc92u)
    MD5_STEP(MD5_I, c, d, a, b, w[10], 15, 0xffeff47du)
    MD5_STEP(MD5_I, b, c, d, a, w[1], 21, 0x85845dd1u)
    MD5_STEP(MD5_I, a, b, c, d, w[8], 6, 0x6fa87e4fu)
    MD5_STEP(MD5_I, d, a, b, c, w[15], 10, 0xfe2ce6e0u)
    MD5_STEP(MD5_I, c, d, a, b, w[6], 15, 0xa3014314u)
    MD5_STEP(MD5_I, b, c, d, a, w[13], 21, 0x4e0811a1u)
    MD5_STEP(MD5_I, a, b, c, d, w[4], 6, 0xf7537e82u)
    MD5_STEP(MD5_I, d, a, b, c, w[11], 10, 0xbd3af235u)
    MD5_STEP(MD5_I, c, d, a, b, w[2], 15, 0x2ad7d2bbu)
    MD5_STEP(MD5_I, b, c, d, a, w[9], 21, 0xeb86d391u)
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

/* Up to 32 bits; whole 32-bit words go out to buf, the rest waits in acc
 * (flush_bytes writes its whole bytes). */
static void put(bitw* w, uint32_t v, int n)
{
    if (n <= 0) return;
    if (n < 32) v &= (1u << n) - 1;
    w->acc = (w->acc << n) | v;
    w->nbits += n;
    if (w->nbits >= 32) {
        uint32_t o;
        w->nbits -= 32;
        o = (uint32_t)(w->acc >> w->nbits);
        w->buf[w->len] = (uint8_t)(o >> 24);
        w->buf[w->len + 1] = (uint8_t)(o >> 16);
        w->buf[w->len + 2] = (uint8_t)(o >> 8);
        w->buf[w->len + 3] = (uint8_t)o;
        w->len += 4;
    }
}

static void flush_bytes(bitw* w)
{
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
    if (w->nbits & 7) put(w, 0, 8 - (w->nbits & 7));
    flush_bytes(w);
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

/* A byte at a time, through the CRC of each byte value (polynomial 0x8005, MSB
 * first). */
static const uint16_t crc16_table[256] = {
    0x0000, 0x8005, 0x800f, 0x000a, 0x801b, 0x001e, 0x0014, 0x8011, 0x8033, 0x0036, 0x003c, 0x8039,
    0x0028, 0x802d, 0x8027, 0x0022, 0x8063, 0x0066, 0x006c, 0x8069, 0x0078, 0x807d, 0x8077, 0x0072,
    0x0050, 0x8055, 0x805f, 0x005a, 0x804b, 0x004e, 0x0044, 0x8041, 0x80c3, 0x00c6, 0x00cc, 0x80c9,
    0x00d8, 0x80dd, 0x80d7, 0x00d2, 0x00f0, 0x80f5, 0x80ff, 0x00fa, 0x80eb, 0x00ee, 0x00e4, 0x80e1,
    0x00a0, 0x80a5, 0x80af, 0x00aa, 0x80bb, 0x00be, 0x00b4, 0x80b1, 0x8093, 0x0096, 0x009c, 0x8099,
    0x0088, 0x808d, 0x8087, 0x0082, 0x8183, 0x0186, 0x018c, 0x8189, 0x0198, 0x819d, 0x8197, 0x0192,
    0x01b0, 0x81b5, 0x81bf, 0x01ba, 0x81ab, 0x01ae, 0x01a4, 0x81a1, 0x01e0, 0x81e5, 0x81ef, 0x01ea,
    0x81fb, 0x01fe, 0x01f4, 0x81f1, 0x81d3, 0x01d6, 0x01dc, 0x81d9, 0x01c8, 0x81cd, 0x81c7, 0x01c2,
    0x0140, 0x8145, 0x814f, 0x014a, 0x815b, 0x015e, 0x0154, 0x8151, 0x8173, 0x0176, 0x017c, 0x8179,
    0x0168, 0x816d, 0x8167, 0x0162, 0x8123, 0x0126, 0x012c, 0x8129, 0x0138, 0x813d, 0x8137, 0x0132,
    0x0110, 0x8115, 0x811f, 0x011a, 0x810b, 0x010e, 0x0104, 0x8101, 0x8303, 0x0306, 0x030c, 0x8309,
    0x0318, 0x831d, 0x8317, 0x0312, 0x0330, 0x8335, 0x833f, 0x033a, 0x832b, 0x032e, 0x0324, 0x8321,
    0x0360, 0x8365, 0x836f, 0x036a, 0x837b, 0x037e, 0x0374, 0x8371, 0x8353, 0x0356, 0x035c, 0x8359,
    0x0348, 0x834d, 0x8347, 0x0342, 0x03c0, 0x83c5, 0x83cf, 0x03ca, 0x83db, 0x03de, 0x03d4, 0x83d1,
    0x83f3, 0x03f6, 0x03fc, 0x83f9, 0x03e8, 0x83ed, 0x83e7, 0x03e2, 0x83a3, 0x03a6, 0x03ac, 0x83a9,
    0x03b8, 0x83bd, 0x83b7, 0x03b2, 0x0390, 0x8395, 0x839f, 0x039a, 0x838b, 0x038e, 0x0384, 0x8381,
    0x0280, 0x8285, 0x828f, 0x028a, 0x829b, 0x029e, 0x0294, 0x8291, 0x82b3, 0x02b6, 0x02bc, 0x82b9,
    0x02a8, 0x82ad, 0x82a7, 0x02a2, 0x82e3, 0x02e6, 0x02ec, 0x82e9, 0x02f8, 0x82fd, 0x82f7, 0x02f2,
    0x02d0, 0x82d5, 0x82df, 0x02da, 0x82cb, 0x02ce, 0x02c4, 0x82c1, 0x8243, 0x0246, 0x024c, 0x8249,
    0x0258, 0x825d, 0x8257, 0x0252, 0x0270, 0x8275, 0x827f, 0x027a, 0x826b, 0x026e, 0x0264, 0x8261,
    0x0220, 0x8225, 0x822f, 0x022a, 0x823b, 0x023e, 0x0234, 0x8231, 0x8213, 0x0216, 0x021c, 0x8219,
    0x0208, 0x820d, 0x8207, 0x0202
};

static uint16_t crc16(const uint8_t* p, size_t n)
{
    uint16_t c = 0;
    size_t i;
    for (i = 0; i < n; i++) c = (uint16_t)((c << 8) ^ crc16_table[(c >> 8) ^ p[i]]);
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

/* Bits needed for u (0 for 0), in one instruction where the compiler has one. */
static int bitlen64(uint64_t u)
{
#if defined(__GNUC__) || defined(__clang__)
    return u ? 64 - __builtin_clzll(u) : 0;
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
    unsigned long i;
    return _BitScanReverse64(&i, u) ? (int)i + 1 : 0;
#else
    int n = 0;
    while (u) { n++; u >>= 1; }
    return n;
#endif
}

/* Cheapest coding of one partition of `n` samples whose zigzag values sum to `sum`
 * (OR of them `uor`; `avg` is sum / n): Rice parameter (at most kmax, written in
 * pbits), or an escape with raw bits. *wide is set when a parameter above 14 was
 * in reach (only then can the 5-bit method win). */
static uint64_t part_cost(uint64_t n, uint64_t sum, uint64_t avg, uint32_t uor, int kmax, int pbits, uint8_t* out_k,
                          uint8_t* out_esc, int* wide)
{
    int k0, best_k, raw;
    uint64_t best, esc;
    if (n == 0) { *out_k = 0; *out_esc = 0; return (uint64_t)pbits; }
    k0 = sum > n ? bitlen64(avg) - 1 : 0;
    if (k0 + 1 > 14) *wide = 1;
    /* Size bound: n * (k + 1) + (sum >> k), since sum(u >> k) <= sum >> k. With
     * 2^k0 <= sum / n < 2^(k0 + 1), k0 + 1 never costs less than k0, and k0 - 1
     * costs the same only when sum >> (k0 - 1) == 2n (taken then, as the lower). */
    best_k = k0 > kmax ? kmax : k0 > 0 && (sum >> (k0 - 1)) == 2 * n ? k0 - 1 : k0;
    best = n * (uint64_t)(best_k + 1) + (sum >> best_k);
    best += (uint64_t)pbits;
    raw = bitlen64(uor);
    if (raw < 1) raw = 1;
    esc = (uint64_t)pbits + 5 + n * (uint64_t)raw;
    if (raw <= 31 && esc < best) { *out_k = 0; *out_esc = (uint8_t)raw; return esc; }
    *out_k = (uint8_t)best_k;
    *out_esc = 0;
    return best;
}

/* The highest partition order for n samples, pred_order of them warm-up. */
static int part_top(int n, int pred_order, int max_part)
{
    int top = 0;
    while (top < max_part && (n % (2 << top)) == 0 && (n >> (top + 1)) > pred_order) top++;
    return top;
}

/* Plans a residual from its partition sums at partition order `level` (at least
 * the residual's own highest); merges in place, so `ps` is used up. */
static void plan_sums(flac_sums* ps, int level, int n, int pred_order, int max_part, rice_plan* best)
{
    int top = part_top(n, pred_order, max_part), p, j, method;
    best->bits = UINT64_MAX;
    for (p = level; p >= 0; p--) {
        int parts = 1 << p, psize = n >> p, wide = 0;
        if (p < level) {
            for (j = 0; j < parts; j++) {
                ps->sum[j] = ps->sum[2 * j] + ps->sum[2 * j + 1];
                ps->bits[j] = ps->bits[2 * j] | ps->bits[2 * j + 1];
            }
        }
        if (p > top) continue;
        /* Where every partition's parameters stay within 14, the 5-bit method picks
         * the same ones at a bit more each, so it cannot win. */
        for (method = 0; method < 2 && (method == 0 || wide); method++) {
            int kmax = method ? 30 : 14, pbits = method ? 5 : 4, lg = bitlen64((uint64_t)psize) - 1;
            int pow2 = (psize & (psize - 1)) == 0;
            uint8_t param[1 << FLAC_MAX_PART], esc[1 << FLAC_MAX_PART];
            uint64_t bits = 6;   /* method + partition order */
            {
                uint64_t cnt = (uint64_t)(psize - pred_order), sum = ps->sum[0];
                bits += part_cost(cnt, sum, cnt ? sum / cnt : 0, ps->bits[0], kmax, pbits, &param[0], &esc[0], &wide);
            }
            for (j = 1; j < parts; j++) {
                uint64_t sum = ps->sum[j];
                bits += part_cost((uint64_t)psize, sum, pow2 ? sum >> lg : sum / (uint64_t)psize, ps->bits[j], kmax, pbits,
                                  &param[j], &esc[j], &wide);
            }
            if (bits < best->bits) {
                best->bits = bits;
                best->method = method;
                best->order = p;
                memcpy(best->param, param, (size_t)parts);
                memcpy(best->escape_bits, esc, (size_t)parts);
            }
        }
    }
}

static void write_residual(bitw* w, const int32_t* r, int n, int pred_order, const rice_plan* pl)
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
            uint32_t mask = (1u << k) - 1;
            uint64_t acc;
            int nbits;
            uint8_t* o;
            put(w, (uint32_t)k, pbits);
            /* put(), with the writer's state kept in registers */
            acc = w->acc;
            nbits = w->nbits;
            o = w->buf + w->len;
            for (i = start; i < end; i++) {
                uint32_t u = fa_flac_zigzag(r[i]), q = u >> k;
                if (q + 1 + (uint32_t)k <= 32) {
                    /* q zeros, a one, then the k low bits: one write */
                    int len = (int)q + 1 + k;
                    acc = (acc << len) | ((1u << k) | (u & mask));
                    nbits += len;
                    if (nbits >= 32) {
                        uint32_t v;
                        nbits -= 32;
                        v = (uint32_t)(acc >> nbits);
                        o[0] = (uint8_t)(v >> 24); o[1] = (uint8_t)(v >> 16); o[2] = (uint8_t)(v >> 8); o[3] = (uint8_t)v;
                        o += 4;
                    }
                } else {
                    w->acc = acc; w->nbits = nbits; w->len = (size_t)(o - w->buf);
                    put_unary(w, q);
                    if (k) put(w, u & mask, k);
                    acc = w->acc; nbits = w->nbits; o = w->buf + w->len;
                }
            }
            w->acc = acc;
            w->nbits = nbits;
            w->len = (size_t)(o - w->buf);
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

/* An LPC candidate of the search */
typedef struct lpc_cand {
    int order, precision, shift, use;
    int32_t coef[FLAC_MAX_LPC];
    uint64_t est;                /* bits estimated from the probe */
} lpc_cand;

typedef struct flac_state {
    fa_enc_format f;
    uint32_t bits;               /* sample size: 16 or 24 */
    int level, blocksize, max_lpc, max_part, stereo_search, precision_search;
    int probe;                   /* LPC candidates costed in full after a probe, 0 = all */
    int64_t frames, frame_number;
    uint32_t min_frame, max_frame;
    int64_t streaminfo_pos;
    uint32_t seed;
    md5_ctx md5;
    int32_t* pcm;                /* blocksize x channels, interleaved: the block being filled */
    int fill;
    int32_t* sig[4];             /* per signal: L, R, M, S (or one per channel) */
    int32_t* best_res[4];        /* residual of the chosen subframe per signal */
    flac_signal sg[4];           /* the kernels' view of each signal */
    int16_t* sgbuf;              /* their buffers */
    flac_sums fixed[5], sums;
    double* win;                 /* Tukey window */
    double* wbuf;
    subframe sub[8];
    uint8_t* frame;              /* one encoded frame */
    size_t frame_cap;
    uint8_t* md5buf;
} flac_state;

/* The residuals fit 32 bits: samples have at most 25 (24-bit side), and the
 * order 4 predictor's coefficients add up to 16 in size (choose_subframe checks
 * the sample size). */
static void residual_fixed(const int32_t* x, int n, int order, int32_t* r)
{
    int i;
    for (i = 0; i < order && i < n; i++) r[i] = 0;
    switch (order) {
    case 0: for (i = 0; i < n; i++) r[i] = x[i]; break;
    case 1: for (i = 1; i < n; i++) r[i] = x[i] - x[i - 1]; break;
    case 2: for (i = 2; i < n; i++) r[i] = x[i] - 2 * x[i - 1] + x[i - 2]; break;
    case 3: for (i = 3; i < n; i++) r[i] = x[i] - 3 * x[i - 1] + 3 * x[i - 2] - x[i - 3]; break;
    default:
        for (i = 4; i < n; i++) r[i] = x[i] - 4 * x[i - 1] + 6 * x[i - 2] - 4 * x[i - 3] + x[i - 4];
        break;
    }
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

/* Best subframe for signal x (bps bits per sample), costed from the candidates'
 * partition sums; finish_subframe computes its residual. */
static void choose_subframe(flac_state* st, const int32_t* x, int n, int bps, subframe* best, flac_signal* sg)
{
    int i, order;
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
    /* The 32-bit residuals below need at most 25 bits a sample (24-bit side);
     * flac_check takes only 16 and 24 bits. */
    if (bps > 25) return;
    fa_flac_signal_prepare(sg, x, n, sg->x16, sg->hi, sg->lo);

    {
        int top = part_top(n, 0, st->max_part);
        fa_flac_fixed_sums(x, n, top, sg->xmax, st->fixed);
        for (order = 0; order <= 4 && order < n; order++) {
            memset(&cand, 0, sizeof(cand));
            cand.type = SUB_FIXED;
            cand.order = order;
            plan_sums(&st->fixed[order], top, n, order, st->max_part, &cand.plan);
            cand.bits = 8 + (uint64_t)order * bps + cand.plan.bits;
            if (cand.bits < best->bits) *best = cand;
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
        /* Every lag at once: each sum still adds its terms in order, but the
         * sums no longer wait for one another. */
        for (lag = 0; lag <= maxo; lag++) ac[lag] = 0.0;
        for (i = 0; i < maxo && i < n; i++)
            for (lag = 0; lag <= i; lag++) ac[lag] += w[i] * w[i - lag];
        for (; i < n; i++)
            for (lag = 0; lag <= maxo; lag++) ac[lag] += w[i] * w[i - lag];
        if (ac[0] <= 0.0) return;
        {
            lpc_cand list[FLAC_MAX_LPC * 4 * 2];
            int nc = 0, c, keep = st->probe, pass;
            /* Levinson-Durbin as measured, and again with a little white noise
             * (ac[0] * (1 + 1e-9)) when the first reached a very high prediction
             * gain. The noise keeps the recursion well-conditioned on pure tones
             * (up to 10% smaller) but caps the gain near 90 dB, which costs 24-bit
             * music up to 15%; the candidates of both go on. */
            for (pass = 0; pass < 2; pass++) {
                int mo = maxo;
                if (pass && e > ac[0] * 1e-7) break;   /* under 70 dB: the noise changes nothing */
                e = pass ? ac[0] * (1.0 + 1e-9) : ac[0];
                for (k = 0; k < mo; k++) {
                    double acc = ac[k + 1], refl;
                    for (i = 0; i < k; i++) acc -= a[i] * ac[k - i];
                    refl = acc / e;
                    for (i = 0; i < k; i++) tmp[i] = a[i] - refl * a[k - 1 - i];
                    for (i = 0; i < k; i++) a[i] = tmp[i];
                    a[k] = refl;
                    e *= 1.0 - refl * refl;
                    for (i = 0; i <= k; i++) lpc[k][i] = a[i];
                    if (e <= 0.0) { mo = k + 1; break; }
                }
                for (order = 1; order <= mo; order++)
                    for (p = pmin; p <= pmax; p++) {
                        lpc_cand* lc = &list[nc];
                        if (!quantize_lpc(lpc[order - 1], order, p, lc->coef, &lc->shift)) continue;
                        for (c = 0; c < nc; c++)   /* the same predictor twice */
                            if (list[c].order == order && list[c].precision == p && list[c].shift == lc->shift &&
                                !memcmp(list[c].coef, lc->coef, sizeof(int32_t) * (size_t)order)) break;
                        if (c < nc) continue;
                        lc->order = order;
                        lc->precision = p;
                        lc->use = 1;
                        nc++;
                    }
            }
            if (keep > 0 && nc > keep && n >= 512) {
                /* Probe every candidate on a quarter of the block (8 spread pieces),
                 * then cost only the `keep` most promising in full. */
                int pieces = 8, stride = n / pieces, len = stride / 4, j, t;
                for (c = 0; c < nc; c++) {
                    lpc_cand* lc = &list[c];
                    uint64_t est = 0;
                    int wide = 0;
                    lc->use = 0;
                    lc->est = UINT64_MAX;
                    if (!fa_flac_lpc_sums(sg, lc->order, lc->coef, lc->shift, pieces, stride, len, &st->sums, NULL)) continue;
                    for (j = 0; j < pieces; j++) {
                        uint64_t m = (uint64_t)(j == 0 ? len - lc->order : len), sum = st->sums.sum[j];
                        uint8_t pk, pe;
                        est += part_cost(m, sum, m ? sum / m : 0, st->sums.bits[j], 30, 5, &pk, &pe, &wide);
                    }
                    lc->est = (uint64_t)lc->order * (uint64_t)(bps + lc->precision) +
                              est * (uint64_t)(n - lc->order) / (uint64_t)(pieces * len - lc->order);
                }
                for (t = 0; t < keep; t++) {
                    int bc = -1;
                    for (c = 0; c < nc; c++)
                        if (!list[c].use && list[c].est != UINT64_MAX && (bc < 0 || list[c].est < list[bc].est)) bc = c;
                    if (bc >= 0) list[bc].use = 1;
                }
            }
            for (c = 0; c < nc; c++) {
                const lpc_cand* lc = &list[c];
                int top;
                if (!lc->use) continue;
                order = lc->order;
                top = part_top(n, order, st->max_part);
                memset(&cand, 0, sizeof(cand));
                cand.type = SUB_LPC;
                cand.order = order;
                cand.precision = lc->precision;
                cand.shift = lc->shift;
                memcpy(cand.coef, lc->coef, sizeof(cand.coef));
                if (!fa_flac_lpc_sums(sg, order, cand.coef, cand.shift, 1 << top, n >> top, n >> top, &st->sums, NULL)) continue;
                plan_sums(&st->sums, top, n, order, st->max_part, &cand.plan);
                cand.bits = 8 + (uint64_t)order * bps + 4 + 5 + (uint64_t)order * cand.precision + cand.plan.bits;
                if (cand.bits < best->bits) *best = cand;
            }
        }
    }
}

/* The chosen subframe's residual (the plan made from its sums stays). */
static void finish_subframe(flac_state* st, int n, int bps, subframe* sf, int32_t* res, flac_signal* sg)
{
    if (sf->type == SUB_FIXED) {
        residual_fixed(sg->x, n, sf->order, res);
    } else if (sf->type == SUB_LPC) {
        int top = part_top(n, sf->order, st->max_part);
        if (!fa_flac_lpc_sums(sg, sf->order, sf->coef, sf->shift, 1 << top, n >> top, n >> top, &st->sums, res)) {
            /* not reached: the search computed the same residual */
            memset(sf, 0, sizeof(*sf));
            sf->type = SUB_VERBATIM;
            sf->bits = 8 + (uint64_t)n * bps;
        }
    }
}

static void write_subframe(bitw* w, const int32_t* x, int n, int bps, const subframe* sf, const int32_t* res)
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
        uint8_t* o = st->md5buf;
        if (bps == 16) {
            for (k = 0; k < total; k++, b += 2) {
                o[b] = (uint8_t)st->pcm[k];
                o[b + 1] = (uint8_t)(st->pcm[k] >> 8);
            }
        } else {
            for (k = 0; k < total; k++, b += 3) {
                o[b] = (uint8_t)st->pcm[k];
                o[b + 1] = (uint8_t)(st->pcm[k] >> 8);
                o[b + 2] = (uint8_t)(st->pcm[k] >> 16);
            }
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
            choose_subframe(st, st->sig[c], n, c == 3 ? bps + 1 : bps, &sub[c], &st->sg[c]);
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
            choose_subframe(st, st->sig[c], n, bps, &sub[c], &st->sg[c]);
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
    flush_bytes(&w);
    hdr_end = w.len;
    put(&w, crc8(w.buf, hdr_end), 8);

    if (ch == 2 && st->stereo_search) {
        static const int pick[11][2] = { { 0, 1 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 },
                                         { 0, 3 }, { 3, 1 }, { 2, 3 } };
        for (c = 0; c < 2; c++) {
            int sidx = pick[mode][c], sbps = sidx == 3 ? bps + 1 : bps;
            finish_subframe(st, n, sbps, &sub[sidx], st->best_res[sidx], &st->sg[sidx]);
            write_subframe(&w, st->sig[sidx], n, sbps, &sub[sidx], st->best_res[sidx]);
        }
    } else {
        for (c = 0; c < ch; c++) {
            if (c >= 4) {
                for (i = 0; i < n; i++) st->sig[0][i] = st->pcm[i * ch + c];
                choose_subframe(st, st->sig[0], n, bps, &sub[0], &st->sg[0]);
                finish_subframe(st, n, bps, &sub[0], st->best_res[0], &st->sg[0]);
                write_subframe(&w, st->sig[0], n, bps, &sub[0], st->best_res[0]);
            } else {
                finish_subframe(st, n, bps, &sub[c], st->best_res[c], &st->sg[c]);
                write_subframe(&w, st->sig[c], n, bps, &sub[c], st->best_res[c]);
            }
        }
    }
    align_byte(&w);
    crc = crc16(w.buf, w.len);
    put(&w, crc >> 8, 8);
    put(&w, crc & 0xFF, 8);
    flush_bytes(&w);

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
    static const struct { int lpc, part, stereo, block, prec, probe; } levels[9] = {
        { 0, 3, 0, 1152, 0, 0 }, { 0, 3, 1, 1152, 0, 0 }, { 0, 4, 1, 1152, 0, 0 },
        { 6, 4, 0, 4096, 0, 2 }, { 8, 4, 1, 4096, 0, 2 }, { 8, 5, 1, 4096, 0, 2 },
        { 8, 6, 1, 4096, 0, 2 }, { 12, 6, 1, 4096, 0, 2 }, { 12, 8, 1, 4096, 1, 4 }
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
    st->probe = levels[lv].probe;
    st->seed = 0x2545F491u;
    md5_init(&st->md5);
    st->pcm = (int32_t*)malloc((size_t)bs * f->channels * sizeof(int32_t));
    for (i = 0; i < 4; i++) {
        st->sig[i] = (int32_t*)malloc((size_t)bs * sizeof(int32_t));
        st->best_res[i] = (int32_t*)malloc((size_t)bs * sizeof(int32_t));
        if (!st->sig[i] || !st->best_res[i]) return FA_ERROR_OUT_OF_MEMORY;
    }
    st->sgbuf = (int16_t*)malloc((size_t)bs * 12 * sizeof(int16_t));
    for (i = 0; st->sgbuf && i < 4; i++) {
        st->sg[i].x16 = st->sgbuf + (size_t)bs * (3 * i);
        st->sg[i].hi = st->sgbuf + (size_t)bs * (3 * i + 1);
        st->sg[i].lo = st->sgbuf + (size_t)bs * (3 * i + 2);
    }
    st->win = (double*)malloc((size_t)bs * sizeof(double));
    st->wbuf = (double*)malloc((size_t)bs * sizeof(double));
    /* the worst case is verbatim (+1 bit for side) plus headers */
    st->frame_cap = (size_t)bs * f->channels * 4 + 64 * f->channels + 64;
    st->frame = (uint8_t*)malloc(st->frame_cap);
    st->md5buf = (uint8_t*)malloc((size_t)bs * f->channels * 3);
    if (!st->pcm || !st->sgbuf || !st->win || !st->wbuf || !st->frame || !st->md5buf) return FA_ERROR_OUT_OF_MEMORY;
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
    free(st->sgbuf);
    free(st->win);
    free(st->wbuf);
    free(st->frame);
    free(st->md5buf);
    free(st);
}

const fa_enc_backend fa_enc_flac = { flac_check, flac_choose, flac_begin, flac_write, flac_finish, flac_free };
