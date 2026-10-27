/* AAC decoding, built in: AAC-LC, HE-AAC (SBR), HE-AAC v2 (parametric stereo),
 * AAC Main and AAC-LTP.
 *
 * tests/data/aac holds short streams from fdkaac and FFmpeg's encoder, plus two
 * whose PS data was rewritten with modes encoders seldom use (34 bands, fine
 * IID, mixing procedure R_B, IPD/OPD), one with long-term prediction added
 * (FFmpeg's encoder never predicts), and what the ffmpeg CLI decodes each of
 * them to (make_aac_data.sh). The decoder must match FFmpeg's to within the
 * references' 16-bit rounding. For IPD/OPD FFmpeg conjugates the phase of the
 * negative-frequency hybrid bands only at the start of each interpolation (see
 * aac_ps.c), so that stream is compared above the lowest QMF band. */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "fa_internal.h"
#include "aac.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FA_TEST_DATA_DIR
#  define FA_TEST_DATA_DIR "data"
#endif

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; return; } } while (0)
#define CHECK_EQ(a, b) do { long long va_ = (long long)(a), vb_ = (long long)(b); if (va_ != vb_) { \
    fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_failures++; return; } } while (0)
#define RUN(t) do { int before_ = g_failures; printf("%s\n", #t); t(); if (g_failures == before_) printf("  ok\n"); } while (0)

static fa_engine* g_engine;

static uint8_t* load_file(const char* name, size_t* size)
{
    char path[512];
    FILE* f;
    uint8_t* p;
    long n;
    snprintf(path, sizeof(path), "%s/%s", FA_TEST_DATA_DIR, name);
    f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    p = (uint8_t*)malloc((size_t)n + 1);
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); p = NULL; }
    fclose(f);
    *size = (size_t)n;
    return p;
}

/* ---- through the public API ---------------------------------------------------------- */

typedef struct decoded {
    float* pcm;
    int64_t frames;
    fa_stream_info info;
} decoded;

static int decode(const char* name, decoded* d)
{
    char path[512];
    fa_open_options o;
    fa_stream* s = NULL;
    int64_t cap = 1 << 14;
    uint32_t ch;
    memset(d, 0, sizeof(*d));
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    snprintf(path, sizeof(path), "%s/aac/%s", FA_TEST_DATA_DIR, name);
    if (fa_stream_open_path(g_engine, path, &o, &s) != FA_OK) { fprintf(stderr, "  cannot open %s\n", name); return 0; }
    fa_stream_get_info(s, &d->info);
    ch = d->info.format.channels;
    d->pcm = (float*)malloc((size_t)cap * ch * sizeof(float));
    for (;;) {
        fa_frames got = 0;
        fa_result r;
        if (d->frames + 1000 > cap) {
            cap *= 2;
            d->pcm = (float*)realloc(d->pcm, (size_t)cap * ch * sizeof(float));
        }
        r = fa_stream_read(s, d->pcm + (size_t)d->frames * ch, 1000, &got);
        d->frames += got;
        if (r != FA_OK || got == 0) break;
    }
    fa_stream_close(s);
    return 1;
}

/* Two second-order Butterworth high-pass sections (24 dB/octave) at `hz`. */
static void highpass(float* x, int64_t n, int stride, double hz, double rate)
{
    double w = 2 * 3.14159265358979 * hz / rate, al = sin(w) / (2 * sqrt(0.5)), c = cos(w);
    double b0 = (1 + c) / 2, b1 = -(1 + c), b2 = (1 + c) / 2, a0 = 1 + al, a1 = -2 * c, a2 = 1 - al;
    int pass;
    for (pass = 0; pass < 2; pass++) {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        int64_t i;
        for (i = 0; i < n; i++) {
            double v = x[i * stride], y = (b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2) / a0;
            x2 = x1;
            x1 = v;
            y2 = y1;
            y1 = y;
            x[i * stride] = (float)y;
        }
    }
}

typedef struct ref_case {
    const char* name;
    uint32_t rate, channels;
    int lead;                  /* frames FFmpeg's decode has before ours (the ADTS priming) */
    int map[8];                /* our channel for FFmpeg's channel k */
    double highpass;           /* compare above this frequency (0: everything) */
} ref_case;

static void compare(const ref_case* t)
{
    char name[64];
    decoded m, r;
    uint32_t c, rc;
    int64_t n, i, b;
    double worst = 999, worst_block = 999;
    snprintf(name, sizeof(name), "%s.aac", t->name);
    CHECK(decode(name, &m));
    snprintf(name, sizeof(name), "%s.ref.flac", t->name);
    CHECK(decode(name, &r));
    CHECK(strcmp(m.info.codec, "aac") == 0);
    CHECK(strcmp(m.info.container, "adts") == 0);
    CHECK_EQ(m.info.format.sample_rate, t->rate);
    CHECK_EQ(m.info.format.channels, t->channels);
    rc = r.info.format.channels;
    n = r.frames - t->lead;
    CHECK(m.frames >= n - 2048 && m.frames <= n);
    n = m.frames;
    for (c = 0; c < rc; c++) {
        float* e = (float*)malloc((size_t)n * sizeof(float));
        float* x = (float*)malloc((size_t)n * sizeof(float));
        double se = 0, sx = 0, be = 0, bx = 0;
        for (i = 0; i < n; i++) {
            x[i] = r.pcm[(size_t)(i + t->lead) * rc + c];
            e[i] = m.pcm[(size_t)i * t->channels + t->map[c]] - x[i];
        }
        if (t->highpass > 0) {
            highpass(e, n, 1, t->highpass, t->rate);
            highpass(x, n, 1, t->highpass, t->rate);
        }
        for (b = 0; b < n; b++) {
            se += (double)e[b] * e[b];
            sx += (double)x[b] * x[b];
            be += (double)e[b] * e[b];
            bx += (double)x[b] * x[b];
            if ((b + 1) % 2048 == 0 || b + 1 == n) {
                /* Blocks with something in them (the priming is near silence). */
                if (bx > 1e-3 && be > 0) {
                    double v = 10 * log10(bx / be);
                    if (v < worst_block) worst_block = v;
                }
                be = bx = 0;
            }
        }
        if (se > 0 && 10 * log10(sx / se) < worst) worst = 10 * log10(sx / se);
        free(e);
        free(x);
    }
    for (i = 0; i < n * (int64_t)t->channels; i++) CHECK(isfinite(m.pcm[i]));
    printf("  %s: %u channels at %u Hz, %lld frames, %.0f dB (worst block %.0f dB)\n", t->name, t->channels, t->rate,
           (long long)n, worst, worst_block);
    CHECK(worst > 80);
    CHECK(worst_block > 60);
    free(m.pcm);
    free(r.pcm);
}

static void test_matches_ffmpeg(void)
{
    static const ref_case cases[] = {
        { "he_stereo", 44100, 2, 2048, { 0, 1 }, 0 },
        { "he_v2", 44100, 2, 2048, { 0, 1 }, 0 },
        { "he_mono", 48000, 2, 2048, { 0, 1 }, 0 },                  /* mono HE-AAC plays as stereo */
        { "he_51", 48000, 6, 2048, { 0, 1, 2, 3, 4, 5 }, 0 },
        { "lc_pce_quad", 48000, 5, 1024, { 0, 1, 3, 4 }, 0 },       /* quad: 5 channels, centre silent */
        { "ps34", 44100, 2, 2048, { 0, 1 }, 0 },
        { "ps_ipd", 44100, 2, 2048, { 0, 1 }, 1500 },
        { "main", 44100, 2, 1024, { 0, 1 }, 0 },                     /* AAC Main prediction */
        { "ltp", 44100, 2, 1024, { 0, 1 }, 0 },                      /* AAC-LTP */
    };
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) compare(&cases[i]);
}

/* Seeks give a straight decode's samples: the SBR and PS state is settled by
 * the packets decoded before the target, and a seek to the start restarts the
 * PNS noise too. The noise generators run through the whole stream, so further
 * in they can differ: tone.aac (PNS) is checked only where the seek decodes
 * from its first packet; in the short HE-AAC streams they line up. */
static void test_seek_matches_straight_decode(void)
{
    static const char* files[] = { "aac/he_stereo.aac", "aac/he_v2.aac", "aac/he_mono.aac", "aac/he_51.aac",
                                   "aac/ps34.aac", "aac/ps_ipd.aac", "caf/aac_he.caf", "caf/aac_hev2.caf",
                                   "tone.aac" };
    static const int64_t targets[] = { 0, 1, 1023, 1024, 2047, 5000, 9000, 10000, 12523, 13000 };
    size_t f, k;
    for (f = 0; f < sizeof(files) / sizeof(files[0]); f++) {
        char path[512];
        fa_open_options o;
        fa_stream* s = NULL;
        fa_stream_info info;
        float* full;
        float* p;
        int64_t n = 0, cap = 1 << 15, i;
        uint32_t ch;
        double worst = 0;
        fa_open_options_init(&o);
        o.flags = FA_OPEN_DECODE_ONLY;
        snprintf(path, sizeof(path), "%s/%s", FA_TEST_DATA_DIR, files[f]);
        CHECK_EQ(fa_stream_open_path(g_engine, path, &o, &s), FA_OK);
        fa_stream_get_info(s, &info);
        ch = info.format.channels;
        full = (float*)malloc((size_t)cap * ch * sizeof(float));
        for (;;) {
            fa_frames got = 0;
            fa_result r;
            if (n + 1000 > cap) {
                cap *= 2;
                full = (float*)realloc(full, (size_t)cap * ch * sizeof(float));
            }
            r = fa_stream_read(s, full + (size_t)n * ch, 1000, &got);
            n += got;
            if (r != FA_OK || got == 0) break;
        }
        p = (float*)malloc((size_t)3000 * ch * sizeof(float));
        for (k = 0; k < sizeof(targets) / sizeof(targets[0]); k++) {
            int64_t at = targets[k], len = n - at < 3000 ? n - at : 3000, m = 0;
            if (len <= 0 || (strstr(files[f], "tone") && at > 10000)) continue;
            CHECK_EQ(fa_stream_seek(s, at), FA_OK);
            while (m < len) {
                fa_frames got = 0;
                fa_frames want = len - m < 777 ? len - m : 777;
                if (fa_stream_read(s, p + (size_t)m * ch, want, &got) != FA_OK || got == 0) break;
                m += got;
            }
            CHECK_EQ(m, len);
            for (i = 0; i < len * (int64_t)ch; i++)
                if (fabs((double)p[i] - full[(size_t)at * ch + (size_t)i]) > worst)
                    worst = fabs((double)p[i] - full[(size_t)at * ch + (size_t)i]);
        }
        printf("  %s: %lld frames, seek vs straight decode %g\n", files[f], (long long)n, worst);
        CHECK(worst == 0.0);
        free(p);
        free(full);
        fa_stream_close(s);
    }
}

/* ---- the decoder itself ------------------------------------------------------------------ */

typedef struct adts_frame {
    const uint8_t* p;
    size_t len, header;
} adts_frame;

static size_t split_adts(const uint8_t* d, size_t n, adts_frame* out, size_t max)
{
    size_t pos = 0, k = 0;
    while (pos + 7 <= n && k < max) {
        size_t len = ((size_t)(d[pos + 3] & 3) << 11) | ((size_t)d[pos + 4] << 3) | (d[pos + 5] >> 5);
        if (len < 7 || pos + len > n) break;
        out[k].p = d + pos;
        out[k].len = len;
        out[k].header = (d[pos + 1] & 1) ? 7 : 9;
        k++;
        pos += len;
    }
    return k;
}

typedef struct pcm_buf {
    float* p;
    size_t n, cap;            /* floats */
    int channels;
    uint32_t rate;
} pcm_buf;

static void append(pcm_buf* o, const float* p, int frames, int channels)
{
    size_t need = o->n + (size_t)frames * channels;
    if (need > o->cap) {
        o->cap = need * 2;
        o->p = (float*)realloc(o->p, o->cap * sizeof(float));
    }
    memcpy(o->p + o->n, p, (size_t)frames * channels * sizeof(float));
    o->n = need;
}

/* Decodes packets with a fresh decoder (`asc` set up first if given) into *o. */
static int decode_packets(const uint8_t* const* pk, const size_t* len, size_t count, const uint8_t* asc, size_t asc_n,
                          pcm_buf* o)
{
    fa_aac* d = fa_aac_create();
    float* pcm = NULL;
    size_t cap = 0, i;
    int ok = d != NULL;
    memset(o, 0, sizeof(*o));
    if (ok && asc) ok = fa_aac_set_config(d, asc, asc_n) == FA_OK;
    for (i = 0; ok && i < count; i++) {
        int frames = 0, channels = 0;
        uint32_t rate = 0;
        if (fa_aac_decode(d, pk[i], len[i], &pcm, &cap, &frames, &channels, &rate) != FA_OK) ok = 0;
        if (frames > 0) {
            append(o, pcm, frames, channels);
            o->channels = channels;
            o->rate = rate;
        }
    }
    free(pcm);
    fa_aac_destroy(d);
    return ok;
}

static int same_pcm(const pcm_buf* a, const pcm_buf* b)
{
    return a->n == b->n && a->channels == b->channels && a->rate == b->rate && a->n > 0 &&
           memcmp(a->p, b->p, a->n * sizeof(float)) == 0;
}

#define MAX_FRAMES 256

/* An AudioSpecificConfig, MSB first. */
typedef struct bitw {
    uint8_t b[16];
    int pos;
} bitw;

static void put(bitw* w, uint32_t v, int n)
{
    while (n-- > 0) {
        if ((v >> n) & 1) w->b[w->pos >> 3] |= (uint8_t)(0x80 >> (w->pos & 7));
        w->pos++;
    }
}

/* MP4 and Matroska carry the configuration once and raw access units: the same
 * audio as the ADTS frames, whether the config names HE-AAC (explicit) or leaves
 * SBR and PS to be found in the data (implicit). */
static void test_raw_access_units(void)
{
    static const struct { const char* name; int aot; int ext_rate_index; } cases[] = {
        { "aac/he_stereo.aac", 2, -1 },      /* implicit */
        { "aac/he_stereo.aac", 5, 4 },       /* explicit SBR, 44.1 kHz after it */
        { "aac/he_v2.aac", 2, -1 },
        { "aac/he_v2.aac", 29, 4 },          /* explicit PS */
        { "aac/he_51.aac", 5, 3 },
    };
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t size, n, k;
        uint8_t* data = load_file(cases[i].name, &size);
        adts_frame fr[MAX_FRAMES];
        const uint8_t* pk[MAX_FRAMES];
        size_t len[MAX_FRAMES];
        pcm_buf ref, raw;
        bitw w;
        int ri, cfg;
        CHECK(data != NULL);
        n = split_adts(data, size, fr, MAX_FRAMES);
        CHECK(n > 4);
        for (k = 0; k < n; k++) {
            pk[k] = fr[k].p;
            len[k] = fr[k].len;
        }
        CHECK(decode_packets(pk, len, n, NULL, 0, &ref));
        ri = (fr[0].p[2] >> 2) & 15;
        cfg = ((fr[0].p[2] & 1) << 2) | (fr[0].p[3] >> 6);
        memset(&w, 0, sizeof(w));
        put(&w, (uint32_t)cases[i].aot, 5);
        put(&w, (uint32_t)ri, 4);
        put(&w, (uint32_t)cfg, 4);
        if (cases[i].ext_rate_index >= 0) {
            put(&w, (uint32_t)cases[i].ext_rate_index, 4);
            put(&w, 2, 5);                   /* AAC-LC core */
        }
        put(&w, 0, 3);                       /* GASpecificConfig: 1024 samples, no core coder delay, no extension */
        for (k = 0; k < n; k++) {
            pk[k] = fr[k].p + fr[k].header;
            len[k] = fr[k].len - fr[k].header;
        }
        CHECK(decode_packets(pk, len, n, w.b, (size_t)(w.pos + 7) / 8, &raw));
        CHECK(same_pcm(&ref, &raw));
        free(ref.p);
        free(raw.p);
        free(data);
    }
}

/* ADTS frames holding several raw data blocks, with and without CRCs (the
 * position table and a CRC after each block), decode as the separate frames do. */
static void repack(const adts_frame* fr, size_t n, int blocks, int crc, uint8_t* out, size_t* out_n, const uint8_t** pk,
                   size_t* len, size_t* count)
{
    size_t i, o = 0;
    *count = 0;
    for (i = 0; i < n; i += (size_t)blocks) {
        size_t m = n - i < (size_t)blocks ? n - i : (size_t)blocks, j, start = o, total;
        uint8_t* h = out + o;
        memcpy(h, fr[i].p, 7);
        h[1] = (uint8_t)((h[1] & 0xFE) | (crc ? 0 : 1));
        o += 7;
        if (crc) {
            size_t at = 0;
            for (j = 0; j + 1 < m; j++) {      /* raw_data_block_position: end of each block, from the first's start */
                at += fr[i + j].len - fr[i + j].header + 2;
                if (m > 1) {
                    out[o++] = (uint8_t)(at >> 8);
                    out[o++] = (uint8_t)at;
                }
            }
            out[o++] = 0;                      /* the header's CRC (not checked) */
            out[o++] = 0;
        }
        for (j = 0; j < m; j++) {
            memcpy(out + o, fr[i + j].p + fr[i + j].header, fr[i + j].len - fr[i + j].header);
            o += fr[i + j].len - fr[i + j].header;
            if (crc && m > 1) {
                out[o++] = 0;
                out[o++] = 0;
            }
        }
        total = o - start;
        h[3] = (uint8_t)((h[3] & 0xFC) | (total >> 11));
        h[4] = (uint8_t)(total >> 3);
        h[5] = (uint8_t)((h[5] & 0x1F) | ((total & 7) << 5));
        h[6] = (uint8_t)((h[6] & 0xFC) | (m - 1));
        pk[*count] = h;
        len[*count] = total;
        (*count)++;
    }
    *out_n = o;
}

static void test_several_blocks_and_crc(void)
{
    static const char* files[] = { "aac/he_stereo.aac", "aac/he_v2.aac", "aac/lc_pce_quad.aac", "aac/he_51.aac",
                                   "aac/main.aac", "aac/ltp.aac" };
    size_t f;
    for (f = 0; f < sizeof(files) / sizeof(files[0]); f++) {
        size_t size, n, k, outn, count;
        uint8_t* data = load_file(files[f], &size);
        uint8_t* buf;
        adts_frame fr[MAX_FRAMES];
        const uint8_t* pk[MAX_FRAMES];
        size_t len[MAX_FRAMES];
        pcm_buf ref;
        int blocks, crc;
        CHECK(data != NULL);
        n = split_adts(data, size, fr, MAX_FRAMES);
        for (k = 0; k < n; k++) {
            pk[k] = fr[k].p;
            len[k] = fr[k].len;
        }
        CHECK(decode_packets(pk, len, n, NULL, 0, &ref));
        buf = (uint8_t*)malloc(size + n * 8 + 64);
        for (blocks = 1; blocks <= 4; blocks++) {
            for (crc = 0; crc < 2; crc++) {
                pcm_buf got;
                repack(fr, n, blocks, crc, buf, &outn, pk, len, &count);
                CHECK(decode_packets(pk, len, count, NULL, 0, &got));
                CHECK(same_pcm(&ref, &got));
                free(got.p);
            }
        }
        free(buf);
        free(ref.p);
        free(data);
    }
}

/* Damaged and truncated frames: no crash (the sanitizer build checks reads),
 * finite output, never more than a frame's samples, and the stream carries on
 * at its normal level once the damage is past. */
static void test_damaged_frames(void)
{
    static const char* files[] = { "aac/he_stereo.aac", "aac/he_v2.aac", "aac/he_51.aac", "aac/ps_ipd.aac",
                                   "aac/ps34.aac", "aac/lc_pce_quad.aac", "aac/he_mono.aac", "aac/main.aac",
                                   "aac/ltp.aac" };
    uint32_t seed = 12345;
    size_t f;
    for (f = 0; f < sizeof(files) / sizeof(files[0]); f++) {
        size_t size, n, k;
        uint8_t* data = load_file(files[f], &size);
        adts_frame fr[MAX_FRAMES];
        int round, predicts = strstr(files[f], "main") || strstr(files[f], "ltp");
        CHECK(data != NULL);
        n = split_adts(data, size, fr, MAX_FRAMES);
        for (round = 0; round < 40; round++) {
            fa_aac* d = fa_aac_create();
            float* pcm = NULL;
            size_t cap = 0;
            int decoded_after = 0;
            CHECK(d != NULL);
            for (k = 0; k < n; k++) {
                uint8_t pkt[8192];
                size_t len = fr[k].len, i;
                int frames = 0, channels = 0, flips;
                uint32_t rate = 0;
                if (len > sizeof(pkt)) len = sizeof(pkt);
                memcpy(pkt, fr[k].p, len);
                /* Every other frame of the middle third damaged, past the header. */
                if (k >= n / 3 && k < 2 * n / 3 && (k & 1)) {
                    for (flips = 0; flips < 1 + round % 8; flips++) {
                        seed = seed * 1103515245u + 12345u;
                        i = 7 + (seed >> 8) % (len - 7);
                        pkt[i] ^= (uint8_t)(1u << ((seed >> 4) & 7));
                    }
                    if (round % 5 == 4) len = 7 + (seed >> 12) % (len - 7);   /* truncated */
                }
                fa_aac_decode(d, pkt, len, &pcm, &cap, &frames, &channels, &rate);
                CHECK(frames >= 0 && frames <= 4 * 2048);
                for (i = 0; i < (size_t)frames * (size_t)channels; i++) {
                    CHECK(isfinite(pcm[i]));
                    /* Back to normal levels once the damage is past (the signal peaks at 0.8),
                     * except where prediction carries what the damage left over into later frames. */
                    CHECK(k < 2 * n / 3 + 3 || predicts || fabsf(pcm[i]) < 2.0f);
                }
                if (k >= 2 * n / 3 + 2 && frames > 0) decoded_after++;
            }
            CHECK(decoded_after > 0);
            free(pcm);
            fa_aac_destroy(d);
        }
        free(data);
    }
    /* Garbage, and nothing. */
    {
        fa_aac* d = fa_aac_create();
        uint8_t junk[2048];
        float* pcm = NULL;
        size_t cap = 0, i;
        int frames, channels, round;
        uint32_t rate;
        CHECK(d != NULL);
        for (round = 0; round < 200; round++) {
            for (i = 0; i < sizeof(junk); i++) {
                seed = seed * 1103515245u + 12345u;
                junk[i] = (uint8_t)(seed >> 16);
            }
            /* A valid ADTS header in front, so the payload gets parsed. */
            junk[0] = 0xFF;
            junk[1] = 0xF1;
            junk[2] = (uint8_t)((1 << 6) | ((3 + round % 9) << 2) | ((round % 8) >> 2));
            junk[3] = (uint8_t)(((round % 8) & 3) << 6);
            junk[3] |= (uint8_t)(sizeof(junk) >> 11);
            junk[4] = (uint8_t)(sizeof(junk) >> 3);
            junk[5] = (uint8_t)((sizeof(junk) & 7) << 5);
            fa_aac_decode(d, junk, sizeof(junk), &pcm, &cap, &frames, &channels, &rate);
            for (i = 0; i < (size_t)frames * (size_t)channels; i++) CHECK(isfinite(pcm[i]));
        }
        CHECK_EQ(fa_aac_decode(d, NULL, 0, &pcm, &cap, &frames, &channels, &rate), FA_OK);
        CHECK_EQ(frames, 0);
        free(pcm);
        fa_aac_destroy(d);
    }
}

/* SBR on its own: a payload of an SBR header (then whatever follows) for a mono
 * element. Bits go MSB first; `buf` must have 12 bytes of padding past `bits`. */
typedef struct sbr_bits {
    uint8_t buf[256 + 12];
    size_t n;
} sbr_bits;

static void sbr_put(sbr_bits* w, uint32_t v, int n)
{
    while (n-- > 0) {
        if ((v >> n) & 1) w->buf[w->n >> 3] |= (uint8_t)(0x80 >> (w->n & 7));
        w->n++;
    }
}

static void sbr_feed(fa_aac_sbr* s, const sbr_bits* w)
{
    fa_aac_bits b;
    b.p = w->buf;
    b.pos = 0;
    b.end = w->n;
    fa_aac_sbr_parse(s, &b, (int)w->n, 0);
}

/* The standard has SBR only up to a 48 kHz core, but a stream can claim it with a
 * faster one. Building the patches for some headers there used to loop forever
 * (the test then hangs): every header value at 64, 88.2 and 96 kHz. */
static void test_sbr_fast_core(void)
{
    static const uint32_t rates[3] = { 64000, 88200, 96000 };
    fa_aac_qmf* q = fa_aac_qmf_create();
    int r;
    CHECK(q != NULL);
    for (r = 0; r < 3 && q; r++) {
        fa_aac_sbr* s = fa_aac_sbr_create(rates[r], 1, q);
        int start, stop, xover, scale, alter, noise;
        CHECK(s != NULL);
        if (!s) continue;
        for (start = 0; start < 16; start++)
            for (stop = 0; stop < 16; stop++)
                for (xover = 0; xover < 8; xover++)
                    for (scale = 0; scale < 4; scale++)
                        for (alter = 0; alter < 2; alter++)
                            for (noise = 0; noise < 4; noise++) {
                                sbr_bits w;
                                memset(&w, 0, sizeof(w));
                                sbr_put(&w, 1, 1);                  /* header follows */
                                sbr_put(&w, 0, 1);                  /* amp_res */
                                sbr_put(&w, (uint32_t)start, 4);
                                sbr_put(&w, (uint32_t)stop, 4);
                                sbr_put(&w, (uint32_t)xover, 3);
                                sbr_put(&w, 0, 2);
                                sbr_put(&w, 1, 1);                  /* extra 1 */
                                sbr_put(&w, 0, 1);
                                sbr_put(&w, (uint32_t)scale, 2);
                                sbr_put(&w, (uint32_t)alter, 1);
                                sbr_put(&w, (uint32_t)noise, 2);
                                w.n += 64;                          /* zero data */
                                sbr_feed(s, &w);
                            }
        fa_aac_sbr_destroy(s);
    }
    fa_aac_qmf_destroy(q);
}

/* The shortest code of `table` for the value `v`: its bits in *code, its length
 * returned (0 if there is none up to 20 bits). */
static int sbr_code_for(const uint16_t* table, int v, uint32_t* code)
{
    int len;
    for (len = 1; len <= 20; len++) {
        uint32_t c;
        for (c = 0; c < (1u << len); c++) {
            uint8_t buf[16] = { 0 };
            fa_aac_bits b;
            uint32_t x = c << (32 - len);
            buf[0] = (uint8_t)(x >> 24);
            buf[1] = (uint8_t)(x >> 16);
            buf[2] = (uint8_t)(x >> 8);
            buf[3] = (uint8_t)x;
            b.p = buf;
            b.pos = 0;
            b.end = 32;
            if (fa_aac_vlc(&b, table, FA_AAC_VLC_ROOT) == v + 128 && b.pos == (size_t)len) {
                *code = c;
                return len;
            }
        }
    }
    return 0;
}

/* Damaged SBR data whose delta-coded envelope levels climb past the standard's
 * range (here by the largest step every band) used to give inf, then NaN, in the
 * output. Now the frame counts as damaged: plain upsampling, finite output. The
 * band and noise band counts are tried rather than worked out, so one of the
 * payloads lines up with the tables. */
static void test_sbr_levels_out_of_range(void)
{
    static float in[1024], out0[2048];
    uint32_t up = 0, zero = 0;
    int up_len = 0, zero_len, step, n, nq, i;
    fa_aac_qmf* q = fa_aac_qmf_create();
    CHECK(q != NULL);
    if (!q) return;
    for (step = 60; step > 0 && !up_len; step--) up_len = sbr_code_for(fa_aac_sbr_f_huffman_env_1_5db, step, &up);
    zero_len = sbr_code_for(fa_aac_sbr_f_huffman_env_3_0db, 0, &zero);
    CHECK(up_len > 0 && zero_len > 0);
    for (i = 0; i < 1024; i++) in[i] = 0.5f * sinf((float)i * 0.3f);
    for (n = 2; n <= 24; n++)
        for (nq = 1; nq <= 5; nq++) {
            fa_aac_sbr* s = fa_aac_sbr_create(22050, 1, q);
            const float* ins[2] = { in, NULL };
            float* outs[2] = { out0, NULL };
            sbr_bits w;
            int k, frame, bad = 0;
            CHECK(s != NULL);
            if (!s) continue;
            memset(&w, 0, sizeof(w));
            sbr_put(&w, 1, 1);
            sbr_put(&w, 0, 1);       /* amp_res */
            sbr_put(&w, 5, 4);       /* start */
            sbr_put(&w, 9, 4);       /* stop */
            sbr_put(&w, 0, 3);       /* xover */
            sbr_put(&w, 0, 2);
            sbr_put(&w, 0, 2);       /* default extras */
            sbr_put(&w, 0, 1);       /* no data extra */
            sbr_put(&w, 0, 2);       /* FIXFIX */
            sbr_put(&w, 0, 2);       /* one envelope */
            sbr_put(&w, 1, 1);       /* high resolution */
            sbr_put(&w, 0, 2);       /* frequency-coded envelope and noise */
            sbr_put(&w, 0, 2 * nq);  /* invf */
            sbr_put(&w, 127, 7);     /* the top of the range, then up each band */
            for (k = 1; k < n; k++) sbr_put(&w, up, up_len);
            sbr_put(&w, 0, 5);
            for (k = 1; k < nq; k++) sbr_put(&w, zero, zero_len);
            sbr_put(&w, 0, 2);       /* no harmonics, no extensions */
            for (frame = 0; frame < 3; frame++) {
                sbr_feed(s, &w);
                fa_aac_sbr_apply(s, ins, outs);
                for (i = 0; i < 2048; i++)
                    if (!isfinite(out0[i])) bad = 1;
            }
            CHECK(!bad);
            fa_aac_sbr_destroy(s);
        }
    fa_aac_qmf_destroy(q);
}

/* A parametric stereo header with a reserved IID mode is refused, and must not
 * leave that mode for the next frame (which then read past the band tables). */
static void test_ps_bad_header_not_kept(void)
{
    fa_aac_ps* p = fa_aac_ps_create();
    sbr_bits w;
    fa_aac_bits b;
    CHECK(p != NULL);
    if (!p) return;
    memset(&w, 0, sizeof(w));
    sbr_put(&w, 1, 1);           /* header */
    sbr_put(&w, 1, 1);           /* IID on */
    sbr_put(&w, 6, 3);           /* mode 6: reserved */
    w.n += 64;
    b.p = w.buf; b.pos = 0; b.end = w.n;
    CHECK_EQ(fa_aac_ps_parse(p, &b, (int)w.n), -1);
    memset(&w, 0, sizeof(w));
    sbr_put(&w, 0, 1);           /* no header: the last one's modes */
    w.n += 64;
    b.p = w.buf; b.pos = 0; b.end = w.n;
    CHECK_EQ(fa_aac_ps_parse(p, &b, (int)w.n), -1);
    fa_aac_ps_destroy(p);
}

/* What the built-in decoder turns down goes on to the system's decoders. */
static void test_unsupported_configs(void)
{
    static const uint8_t xhe[] = { 0xF8, 0xF0, 0x21, 0x2C, 0x00 };   /* object type 42 (USAC) */
    static const uint8_t frame960[] = { 0x12, 0x14 };               /* AAC-LC with 960-sample frames */
    static const uint8_t lc[] = { 0x12, 0x10 };
    fa_codec c;
    fa_codec_params p;
    fa_aac* d = fa_aac_create();
    CHECK(d != NULL);
    CHECK_EQ(fa_aac_set_config(d, xhe, sizeof(xhe)), FA_ERROR_UNSUPPORTED_FORMAT);
    CHECK_EQ(fa_aac_set_config(d, frame960, sizeof(frame960)), FA_ERROR_UNSUPPORTED_FORMAT);
    CHECK_EQ(fa_aac_set_config(d, lc, sizeof(lc)), FA_OK);
    fa_aac_destroy(d);
    fa_codec_params_init(&p);
    p.codec = "aac";
    p.sample_rate = 44100;
    p.channels = 2;
    memset(&c, 0, sizeof(c));
    p.extradata = xhe;
    p.extradata_size = sizeof(xhe);
    CHECK_EQ(fa_aac_codec_open(&c, &p), FA_ERROR_UNSUPPORTED_FORMAT);
#if defined(__linux__) && !defined(__ANDROID__)
    /* No system decoder here, and AAC never goes to FFmpeg, even when it is loaded. */
    CHECK_EQ(fa_codec_open(&c, &p), FA_ERROR_UNSUPPORTED_FORMAT);
#endif
    p.extradata = lc;
    p.extradata_size = sizeof(lc);
    CHECK_EQ(fa_codec_open(&c, &p), FA_OK);
    CHECK(strcmp(c.vt->name, "aac") == 0);
    fa_codec_close(&c);
    {
        /* An .aac file of AAC SSR (ADTS profile 2) is refused at open. */
        size_t size, pos;
        uint8_t* data = load_file("aac/lc_pce_quad.aac", &size);
        fa_stream* s = NULL;
        CHECK(data != NULL);
        for (pos = 0; pos + 7 <= size;) {
            size_t len = ((size_t)(data[pos + 3] & 3) << 11) | ((size_t)data[pos + 4] << 3) | (data[pos + 5] >> 5);
            data[pos + 2] = (uint8_t)((data[pos + 2] & 0x3F) | (2 << 6));
            pos += len;
        }
        CHECK_EQ(fa_stream_open_memory(g_engine, data, size, NULL, &s), FA_ERROR_UNSUPPORTED_FORMAT);
        free(data);
    }
}

#if defined(_WIN32) || defined(__APPLE__) || defined(__ANDROID__)
#  if defined(_WIN32)
#    define OS_AAC_OPEN fa_mf_codec_open
#  elif defined(__APPLE__)
#    define OS_AAC_OPEN fa_at_codec_open
#  else
#    define OS_AAC_OPEN fa_mc_codec_open
#  endif
/* Two second-order Butterworth low-pass sections (24 dB/octave) at `hz`. */
static void lowpass(float* x, int64_t n, int stride, double hz, double rate)
{
    double w = 2 * 3.14159265358979 * hz / rate, al = sin(w) / (2 * sqrt(0.5)), c = cos(w);
    double b0 = (1 - c) / 2, b1 = 1 - c, b2 = (1 - c) / 2, a0 = 1 + al, a1 = -2 * c, a2 = 1 - al;
    int pass;
    for (pass = 0; pass < 2; pass++) {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        int64_t i;
        for (i = 0; i < n; i++) {
            double v = x[i * stride], y = (b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2) / a0;
            x2 = x1;
            x1 = v;
            y2 = y1;
            y1 = y;
            x[i * stride] = (float)y;
        }
    }
}

/* The system's own AAC decoder (Media Foundation, AudioToolbox, MediaCodec), which
 * the library uses for xHE-AAC: given HE-AAC here, it must give the built-in
 * decoder's audio (to within the two decoders' differences, after lining them up).
 * Where none can be reached (a Windows without Media Foundation) it skips, unless
 * FA_TEST_REAL_SYSTEM=1 says the system has one; on Android it needs that. */
static void test_os_decoder(void)
{
    const char* real = getenv("FA_TEST_REAL_SYSTEM");
    size_t size, n, k;
    uint8_t* data = load_file("aac/he_stereo.aac", &size);
    adts_frame fr[MAX_FRAMES];
    const uint8_t* pk[MAX_FRAMES];
    size_t len[MAX_FRAMES];
    pcm_buf ref, os;
    bitw w;
    fa_codec_params p;
    fa_codec c;
    fa_result r;
    int ri, cfg, lag, best_lag = 0;
    double best = -1, se = 0, sx = 0, snr;
    size_t frames, i, from = 2048, span = 8192;   /* the file is 16384 frames */
    CHECK(data != NULL);
#if defined(__ANDROID__)
    /* Outside a booted system (tools/android-test/run.sh's chroot, no binder)
     * MediaCodec waits for its service forever. */
    if (!(real && strcmp(real, "1") == 0)) {
        printf("  skipped (MediaCodec needs a booted Android: FA_TEST_REAL_SYSTEM=1)\n");
        free(data);
        return;
    }
#endif
    n = split_adts(data, size, fr, MAX_FRAMES);
    CHECK(n > 4);
    for (k = 0; k < n; k++) {
        pk[k] = fr[k].p;
        len[k] = fr[k].len;
    }
    CHECK(decode_packets(pk, len, n, NULL, 0, &ref));
    /* HE-AAC said explicitly: 44.1 kHz SBR on a 22.05 kHz LC core. */
    ri = (fr[0].p[2] >> 2) & 15;
    cfg = ((fr[0].p[2] & 1) << 2) | (fr[0].p[3] >> 6);
    memset(&w, 0, sizeof(w));
    put(&w, 5, 5);
    put(&w, (uint32_t)ri, 4);
    put(&w, (uint32_t)cfg, 4);
    put(&w, 4, 4);
    put(&w, 2, 5);
    put(&w, 0, 3);
    fa_codec_params_init(&p);
    p.codec = "aac";
    p.sample_rate = 44100;
    p.channels = 2;
    p.extradata = w.b;
    p.extradata_size = (size_t)(w.pos + 7) / 8;
    memset(&c, 0, sizeof(c));
    r = OS_AAC_OPEN(&c, &p);
    if (r != FA_OK && !(real && strcmp(real, "1") == 0)) {
        printf("  skipped (no system AAC decoder here: %s)\n", fa_result_string(r));
        free(ref.p);
        free(data);
        return;
    }
    CHECK_EQ(r, FA_OK);
    memset(&os, 0, sizeof(os));
    /* MediaCodec's output trails its input by a few packets, so the file goes in
     * twice: what is compared all comes out of the first pass. */
    for (k = 0; k < 2 * n; k++) {
        const adts_frame* f = &fr[k % n];
        CHECK_EQ(c.vt->decode(&c, f->p + f->header, f->len - f->header), FA_OK);
        if (c.out_frames > 0) append(&os, c.out, (int)c.out_frames, (int)c.channels);
    }
    printf("  %s: %u Hz, %u channels, %zu frames from the file twice (built-in %zu from it once)\n", c.vt->name,
           c.rate, c.channels, os.n / (c.channels ? c.channels : 1), ref.n / 2);
    CHECK_EQ(c.rate, 44100);
    CHECK_EQ(c.channels, 2);
    fa_codec_close(&c);
    /* SBR rebuilds the high band from noise and envelopes, differently in every
     * decoder: only the LC core under it must match, so compare below 4 kHz. */
    frames = os.n / 2 < ref.n / 2 ? os.n / 2 : ref.n / 2;
    for (k = 0; k < 2; k++) {
        lowpass(ref.p + k, (int64_t)(ref.n / 2), 2, 4000, 44100);
        lowpass(os.p + k, (int64_t)(os.n / 2), 2, 4000, 44100);
    }
    /* The decoders' delays differ: line the left channels up over a stretch of music. */
    CHECK(frames > from + span + 2048);
    for (lag = -2048; lag <= 2048; lag++) {
        double dot = 0;
        for (i = from; i < from + span; i++) dot += (double)ref.p[i * 2] * os.p[(size_t)((long)i + lag) * 2];
        if (dot > best) { best = dot; best_lag = lag; }
    }
    for (i = from; i < frames - 2048; i++) {
        size_t j = (size_t)((long)i + best_lag);
        double e0 = ref.p[i * 2] - os.p[j * 2], e1 = ref.p[i * 2 + 1] - os.p[j * 2 + 1];
        se += e0 * e0 + e1 * e1;
        sx += (double)ref.p[i * 2] * ref.p[i * 2] + (double)ref.p[i * 2 + 1] * ref.p[i * 2 + 1];
    }
    snr = se > 0 ? 10 * log10(sx / se) : 999;
    printf("  lag %d frames, %.1f dB against the built-in decoder below 4 kHz\n", best_lag, snr);
    CHECK(snr > 20);
    free(os.p);
    free(ref.p);
    free(data);
}
#endif

int main(void)
{
    fa_engine_config cfg;
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    cfg.worker_threads = 2;
    if (fa_engine_create(&cfg, &g_engine) != FA_OK) { fprintf(stderr, "engine create failed\n"); return 1; }

    RUN(test_matches_ffmpeg);
    RUN(test_seek_matches_straight_decode);
    RUN(test_raw_access_units);
    RUN(test_several_blocks_and_crc);
    RUN(test_damaged_frames);
    RUN(test_unsupported_configs);
    RUN(test_sbr_fast_core);
    RUN(test_sbr_levels_out_of_range);
    RUN(test_ps_bad_header_not_kept);
#if defined(_WIN32) || defined(__APPLE__) || defined(__ANDROID__)
    RUN(test_os_decoder);
#endif

    fa_engine_destroy(g_engine);
    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    printf("PASS\n");
    return 0;
}
