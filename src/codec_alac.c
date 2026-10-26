/* The built-in ALAC (Apple Lossless) packet codec: one ALAC frame per packet, as
 * MP4/M4A and Matroska carry them, decoded to the bit on every platform.
 *
 * Written from Apple's description of the format (the ALAC specification that
 * came with Apple's 2011 Apache-licensed release). A frame is a run of elements,
 * each 3 bits of type first: single channels (SCE, and LFE the same way) and
 * channel pairs (CPE) carry audio; fill and data elements are skipped; END closes
 * the frame. An audio element is either verbatim samples or, compressed:
 *  - an adaptive Golomb code of the prediction residual per channel, with runs of
 *    zeros coded on their own when the running average is small;
 *  - an adaptive FIR predictor per channel (its coefficients nudged by the sign of
 *    every residual), optionally after a first-order pass;
 *  - for pairs, a mid/side-like mix (mixBits, mixRes) undone afterwards;
 *  - for 20-bit and wider audio, the low bytes stored apart, uncompressed.
 * Elements come in the order of ALAC's channel layouts (centre first) and are
 * put into the library's (WAVE's) order. */
#include "fa_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _MSC_VER
#include <intrin.h>
#endif

#define ALAC_MAX_FRAME 65536
#define ALAC_MAX_COEFS 32

enum { EL_SCE = 0, EL_CPE = 1, EL_CCE = 2, EL_LFE = 3, EL_DSE = 4, EL_PCE = 5, EL_FIL = 6, EL_END = 7 };

typedef struct alac_state {
    uint32_t frame_length, bit_depth, channels, rate;
    uint32_t pb, mb, kb;            /* Golomb tuning: history multiplier, initial history, k limit */
    int32_t* buf[2];                /* one element's channels */
    uint32_t* low[2];               /* their low bytes, for shifted (wide) audio only */
} alac_state;

typedef struct bits {
    const uint8_t* p;
    size_t n, pos;                  /* in bits */
} bits;

static uint32_t peek(const bits* b, int k)
{
    uint64_t v = 0;
    size_t byte = b->pos >> 3;
    int i, have;
    if (k == 0) return 0;
    if (byte + 8 <= (b->n >> 3)) {
        /* Away from the end: eight bytes in one go. */
        const uint8_t* q = b->p + byte;
        v = ((uint64_t)q[0] << 56) | ((uint64_t)q[1] << 48) | ((uint64_t)q[2] << 40) | ((uint64_t)q[3] << 32) |
            ((uint64_t)q[4] << 24) | ((uint64_t)q[5] << 16) | ((uint64_t)q[6] << 8) | (uint64_t)q[7];
        have = 64 - (int)(b->pos & 7);
    } else {
        for (i = 0; i < 5; i++) v = (v << 8) | (byte + i < (b->n >> 3) ? b->p[byte + i] : 0);
        have = 40 - (int)(b->pos & 7);
    }
    return (uint32_t)((v >> (have - k)) & ((k == 32) ? 0xFFFFFFFFu : ((1u << k) - 1)));
}

static uint32_t get(bits* b, int k)
{
    uint32_t v = peek(b, k);
    b->pos += (size_t)k;
    return v;
}

static int left(const bits* b) { return b->pos <= b->n; }

static int32_t sext(uint32_t v, int k)
{
    if (k >= 32) return (int32_t)v;
    v &= (1u << k) - 1;
    return (int32_t)(v ^ (1u << (k - 1))) - (int32_t)(1u << (k - 1));
}

/* The index of the highest set bit of a nonzero value (floor of log2), in one
 * instruction where the compiler has one. */
static int top_bit(uint32_t v)
{
#if defined(__GNUC__) || defined(__clang__)
    return 31 - __builtin_clz(v);
#elif defined(_MSC_VER)
    unsigned long i;
    _BitScanReverse(&i, v);
    return (int)i;
#else
    int n = 0;
    while (v >>= 1) n++;
    return n;
#endif
}

/* One Golomb-coded value: a unary prefix of up to 9 ones, then k bits or, past
 * that, the value verbatim in `size` bits. */
static uint32_t golomb(bits* b, int k, int size)
{
    uint32_t x, extra, zeros = ~peek(b, 9) & 0x1FF;
    /* The prefix: the ones before the first zero (which ends it and is read too). */
    x = zeros ? 8 - (uint32_t)top_bit(zeros) : 9;
    b->pos += x + (x < 9);
    if (x > 8) return get(b, size);
    if (k == 1) return x;
    x = (x << k) - x;
    extra = peek(b, k);
    if (extra > 1) {
        x += extra - 1;
        b->pos += (size_t)k;
    } else {
        b->pos += (size_t)(k - 1);
    }
    return x;
}

/* The residuals of one channel. */
static int residuals(bits* b, int32_t* out, uint32_t n, int size, uint32_t mult, const alac_state* st)
{
    uint32_t history = st->mb, i;
    int zero_run_after = 0;
    for (i = 0; i < n; i++) {
        uint32_t x;
        int k = top_bit((history >> 9) + 3);
        if (k > (int)st->kb) k = (int)st->kb;
        if (!left(b)) return 0;
        x = golomb(b, k, size) + (uint32_t)zero_run_after;
        zero_run_after = 0;
        out[i] = (int32_t)(x >> 1) ^ -(int32_t)(x & 1);
        if (x > 0xFFFF) history = 0xFFFF;
        else history += x * mult - ((history * mult) >> 9);
        /* A small average: a run of zeros may follow, coded as its length. */
        if (history < 128 && i + 1 < n) {
            uint32_t run;
            /* 8 for a history of 0, as the reference's leading-zero count makes it. */
            k = (history ? 7 - top_bit(history) : 8) + (int)((history + 16) >> 6);
            if (k > (int)st->kb) k = (int)st->kb;
            run = golomb(b, k, 16);
            if (run > 0) {
                if (run > n - i - 1) run = n - i - 1;
                memset(out + i + 1, 0, (size_t)run * sizeof(*out));
                i += run;
            }
            if (run <= 0xFFFF) zero_run_after = 1;
            history = 0;
        }
    }
    return left(b);
}

/* The coefficients move against the residual's sign, oldest sample first, until
 * the residual is used up. p points at the most recent of the `order` samples,
 * base at the one before them. */
static __inline void adapt(int16_t* coefs, const int32_t* p, int32_t base, int32_t err, int order, int shift)
{
    int sign = err > 0 ? 1 : -1, j;
    if (err == 0) return;
    for (j = order - 1; j >= 0 && (sign > 0 ? err > 0 : err < 0); j--) {
        int64_t d = (int64_t)base - p[-j];
        int step = ((d > 0) - (d < 0)) * sign;
        coefs[j] = (int16_t)(coefs[j] - step);
        err = (int32_t)((uint32_t)err - (uint32_t)(int32_t)((d * step) >> shift) * (uint32_t)(order - j));
    }
}

/* The predictor from sample i on with the sum in 32 bits, as the reference
 * decoder does it: wrapped, the sum still has the low 32 bits of the exact one,
 * and only `size` bits of it shifted are used, its bits shift .. shift + size -
 * 1, so where those are within 32 the result is the same as predict's. Inline,
 * so the calls with the usual orders 4 and 8 get their own unrolled copies. */
static __inline void predict32(int32_t* s, uint32_t i, uint32_t n, int16_t* coefs, int order, int shift, int size)
{
    int j;
    for (; i < n; i++) {
        const int32_t* p = s + i - 1;
        int32_t base = s[i - order - 1], err = s[i];
        uint32_t acc = 0;
        for (j = 0; j < order; j++) acc += ((uint32_t)p[-j] - (uint32_t)base) * (uint32_t)(int32_t)coefs[j];
        if (shift > 0) acc += 1u << (shift - 1);
        s[i] = sext(((uint32_t)((int32_t)acc >> shift)) + (uint32_t)base + (uint32_t)err, size);
        adapt(coefs, p, base, err, order, shift);
    }
}

/* The adaptive FIR predictor, in place: residuals in, samples out. `coefs`
 * applies to the `order` previous samples, the most recent first; it adapts as it
 * goes. Order 31 is the plain first-order predictor. */
static void predict(int32_t* s, uint32_t n, int16_t* coefs, int order, int shift, int size)
{
    uint32_t i;
    int j;
    if (n <= 1 || order == 0) return;
    if (order == 31) {
        for (i = 1; i < n; i++) s[i] = sext((uint32_t)s[i - 1] + (uint32_t)s[i], size);
        return;
    }
    for (i = 1; i <= (uint32_t)order && i < n; i++) s[i] = sext((uint32_t)s[i - 1] + (uint32_t)s[i], size);
    if (size + shift <= 32 && size < 32) {
        if (order == 4) predict32(s, i, n, coefs, 4, shift, size);
        else if (order == 8) predict32(s, i, n, coefs, 8, shift, size);
        else predict32(s, i, n, coefs, order, shift, size);
        return;
    }
    for (; i < n; i++) {
        const int32_t* p = s + i - 1;
        int32_t base = s[i - order - 1], err = s[i];
        int64_t acc = 0;
        for (j = 0; j < order; j++) acc += ((int64_t)p[-j] - base) * coefs[j];
        if (shift > 0) acc = (acc + ((int64_t)1 << (shift - 1))) >> shift;
        s[i] = sext((uint32_t)(int32_t)acc + (uint32_t)base + (uint32_t)err, size);
        adapt(coefs, p, base, err, order, shift);
    }
}

typedef struct chan_params {
    int mode, shift, pb_factor, order;
    int16_t coefs[ALAC_MAX_COEFS];
} chan_params;

/* One SCE/LFE (nch 1) or CPE (nch 2) element into st->buf; *n is the frame's
 * sample count (a partial frame sets it). */
static fa_result element(bits* b, alac_state* st, uint32_t nch, uint32_t* n)
{
    uint32_t partial, shifted, verbatim, i, c;
    int size;
    b->pos += 4 + 12;                               /* element instance tag, unused */
    partial = get(b, 1);
    shifted = get(b, 2);
    verbatim = get(b, 1);
    if (partial) {
        *n = get(b, 32);
        if (*n == 0 || *n > st->frame_length) return FA_ERROR_UNSUPPORTED_FORMAT;
    }
    if (shifted * 8 >= st->bit_depth) return FA_ERROR_UNSUPPORTED_FORMAT;
    if (!verbatim) {
        chan_params cp[2];
        uint32_t mix_bits, mult;
        int32_t mix_res;
        size = (int)(st->bit_depth - shifted * 8 + nch - 1);
        if (size > 32) return FA_ERROR_UNSUPPORTED_FORMAT;
        mix_bits = get(b, 8);
        mix_res = (int8_t)get(b, 8);
        if (mix_bits > 31) return FA_ERROR_UNSUPPORTED_FORMAT;
        for (c = 0; c < nch; c++) {
            int k;
            cp[c].mode = (int)get(b, 4);
            cp[c].shift = (int)get(b, 4);
            cp[c].pb_factor = (int)get(b, 3);
            cp[c].order = (int)get(b, 5);
            /* The first stored applies to the most recent sample. */
            for (k = 0; k < cp[c].order; k++) cp[c].coefs[k] = (int16_t)get(b, 16);
        }
        /* The low bytes, stored after the parameters and before the residuals
         * (wide audio: their buffers are made at open, or here for a file that
         * has them at 16 bits). */
        if (shifted) {
            for (c = 0; c < nch; c++)
                if (!st->low[c] && !(st->low[c] = (uint32_t*)malloc((size_t)st->frame_length * sizeof(uint32_t))))
                    return FA_ERROR_OUT_OF_MEMORY;
            for (i = 0; i < *n; i++)
                for (c = 0; c < nch; c++) st->low[c][i] = get(b, (int)shifted * 8);
        }
        for (c = 0; c < nch; c++) {
            mult = st->pb * (uint32_t)cp[c].pb_factor / 4;
            if (!residuals(b, st->buf[c], *n, size, mult, st)) return FA_ERROR_UNSUPPORTED_FORMAT;
            if (cp[c].mode != 0) predict(st->buf[c], *n, NULL, 31, 0, size);
            if (cp[c].order > 0 && cp[c].order < 31) predict(st->buf[c], *n, cp[c].coefs, cp[c].order, cp[c].shift, size);
            else if (cp[c].order == 31) predict(st->buf[c], *n, NULL, 31, 0, size);
        }
        if (nch == 2 && mix_res != 0) {
            for (i = 0; i < *n; i++) {
                int32_t u = st->buf[0][i], v = st->buf[1][i];
                int32_t l = (int32_t)((uint32_t)u + (uint32_t)v - (uint32_t)(int32_t)(((int64_t)mix_res * v) >> mix_bits));
                st->buf[0][i] = l;
                st->buf[1][i] = (int32_t)((uint32_t)l - (uint32_t)v);
            }
        }
    } else {
        size = (int)st->bit_depth;
        shifted = 0;
        for (i = 0; i < *n; i++)
            for (c = 0; c < nch; c++) st->buf[c][i] = sext(get(b, size), size);
    }
    if (!left(b)) return FA_ERROR_UNSUPPORTED_FORMAT;
    if (shifted) {
        for (c = 0; c < nch; c++)
            for (i = 0; i < *n; i++)
                st->buf[c][i] = (int32_t)(((uint32_t)st->buf[c][i] << (shifted * 8)) | st->low[c][i]);
    }
    return FA_OK;
}

/* Where each element's channels go: ALAC's layouts (centre first) by channel
 * count, as positions in the library's WAVE order. */
static const uint8_t wave_pos[8][8] = {
    { 0 },
    { 0, 1 },
    { 2, 0, 1 },                    /* C L R */
    { 2, 0, 1, 3 },                 /* C L R Cs */
    { 2, 0, 1, 3, 4 },              /* C L R Ls Rs */
    { 2, 0, 1, 4, 5, 3 },           /* C L R Ls Rs LFE */
    { 2, 0, 1, 4, 5, 6, 3 },        /* C L R Ls Rs Cs LFE */
    { 2, 6, 7, 0, 1, 4, 5, 3 },     /* C Lc Rc L R Ls Rs LFE */
};

static fa_result alac_decode(fa_codec* c, const uint8_t* data, size_t size)
{
    alac_state* st = (alac_state*)c->impl;
    bits b;
    uint32_t got = 0, n = st->frame_length, i, ch;
    float scale = 1.0f / (float)((uint64_t)1 << (st->bit_depth - 1));
    fa_result r;
    c->out_frames = 0;
    if (!data || size == 0) return FA_OK;           /* nothing held back */
    b.p = data;
    b.n = size * 8;
    b.pos = 0;
    r = fa_codec_reserve(c, st->frame_length, st->channels);
    if (r != FA_OK) return r;
    for (;;) {
        uint32_t type, nch, frames = n;
        if (b.pos + 3 > b.n) return FA_ERROR_UNSUPPORTED_FORMAT;
        type = get(&b, 3);
        if (type == EL_END) break;
        if (type == EL_FIL) {
            uint32_t count = get(&b, 4);
            if (count == 15) count += get(&b, 8) - 1;
            b.pos += (size_t)count * 8;
            continue;
        }
        if (type == EL_DSE) {
            uint32_t align, count;
            b.pos += 4;                             /* element instance tag */
            align = get(&b, 1);
            count = get(&b, 8);
            if (count == 255) count += get(&b, 8);
            if (align) b.pos = (b.pos + 7) & ~(size_t)7;
            b.pos += (size_t)count * 8;
            continue;
        }
        if (type != EL_SCE && type != EL_CPE && type != EL_LFE) return FA_ERROR_UNSUPPORTED_FORMAT;
        nch = type == EL_CPE ? 2 : 1;
        if (got + nch > st->channels) return FA_ERROR_UNSUPPORTED_FORMAT;
        r = element(&b, st, nch, &frames);
        if (r != FA_OK) return r;
        if (got == 0) n = frames;
        else if (frames != n) return FA_ERROR_UNSUPPORTED_FORMAT;
        for (ch = 0; ch < nch; ch++) {
            float* o = c->out + wave_pos[st->channels - 1][got + ch];
            const int32_t* s = st->buf[ch];
            for (i = 0; i < n; i++) o[(size_t)i * st->channels] = (float)s[i] * scale;
        }
        got += nch;
        if (got == st->channels) {
            /* All channels are in; what may follow is an END. */
            if (b.pos + 3 <= b.n && peek(&b, 3) == EL_END) break;
        }
    }
    if (got != st->channels) return FA_ERROR_UNSUPPORTED_FORMAT;
    c->out_frames = (fa_frames)n;
    return FA_OK;
}

static void alac_flush(fa_codec* c)
{
    (void)c;                                        /* every frame stands alone */
}

static void alac_close(fa_codec* c)
{
    alac_state* st = (alac_state*)c->impl;
    if (st) {
        free(st->buf[0]);
        free(st->buf[1]);
        free(st->low[0]);
        free(st->low[1]);
        free(st);
    }
    c->impl = NULL;
}

static const fa_codec_vtable alac_vtable = { "alac", alac_decode, alac_flush, alac_close };

/* The 24-byte ALACSpecificConfig in the containers' extradata: the bare config,
 * the 'alac' box (MP4, and Matroska's A_ALAC as this library passes it), or a
 * Core Audio cookie with a 'frma' box in front. */
int fa_alac_parse_config(const uint8_t* p, size_t n, fa_alac_info* out)
{
    const uint8_t* c = NULL;
    memset(out, 0, sizeof(*out));
    if (!p) return 0;
    if (n == 24) c = p;
    else if (n >= 36 && memcmp(p + 4, "alac", 4) == 0) c = p + 12;
    else if (n >= 48 && memcmp(p + 4, "frma", 4) == 0 && memcmp(p + 16, "alac", 4) == 0) c = p + 24;
    if (!c) return 0;
    out->config = c;
    out->frame_length = fa_be32(c);
    out->bit_depth = c[5];
    out->channels = c[9];
    out->rate = fa_be32(c + 20);
    if (c[4] != 0 || out->frame_length == 0 || out->frame_length > ALAC_MAX_FRAME || out->channels == 0 ||
        out->channels > 8 || out->rate == 0 || out->rate > 768000 ||
        (out->bit_depth != 16 && out->bit_depth != 20 && out->bit_depth != 24 && out->bit_depth != 32))
        return 0;
    return 1;
}

fa_result fa_alac_codec_open(fa_codec* c, const fa_codec_params* p)
{
    fa_alac_info a;
    alac_state* st;
    if (!fa_alac_parse_config((const uint8_t*)p->extradata, p->extradata_size, &a)) return FA_ERROR_UNSUPPORTED_FORMAT;
    st = (alac_state*)calloc(1, sizeof(*st));
    if (!st) return FA_ERROR_OUT_OF_MEMORY;
    st->frame_length = a.frame_length;
    st->bit_depth = a.bit_depth;
    st->channels = a.channels;
    st->rate = a.rate;
    st->pb = a.config[6];
    st->mb = a.config[7];
    st->kb = a.config[8];
    if (st->kb == 0 || st->kb > 31) st->kb = 14;
    st->buf[0] = (int32_t*)malloc((size_t)st->frame_length * sizeof(int32_t));
    st->buf[1] = (int32_t*)malloc((size_t)st->frame_length * sizeof(int32_t));
    if (st->bit_depth > 16) {
        st->low[0] = (uint32_t*)malloc((size_t)st->frame_length * sizeof(uint32_t));
        st->low[1] = (uint32_t*)malloc((size_t)st->frame_length * sizeof(uint32_t));
    }
    c->impl = st;
    c->vt = &alac_vtable;
    if (!st->buf[0] || !st->buf[1] || (st->bit_depth > 16 && (!st->low[0] || !st->low[1]))) {
        alac_close(c);
        c->vt = NULL;
        return FA_ERROR_OUT_OF_MEMORY;
    }
    c->rate = st->rate;
    c->channels = st->channels;
    snprintf(c->name, sizeof(c->name), "alac");
    return FA_OK;
}
