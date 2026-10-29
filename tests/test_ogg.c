/* Ogg Opus and Ogg Vorbis, decoded by the built-in codecs (no FFmpeg), on the
 * small files in tests/data/ogg (make_ogg_data.sh): lengths from the granule
 * positions, sample-exact seeking, multichannel order, chained and multiplexed
 * files, raw Opus packets pushed by the app, loudness tags, damaged files. */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "fa_internal.h"
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
#define CHECK_NEAR(a, b, tol) do { double va_ = (a), vb_ = (b); if (fabs(va_ - vb_) > (tol)) { \
    fprintf(stderr, "  FAIL %s:%d: %s ~ %s (%.6f vs %.6f)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_failures++; return; } } while (0)
#define RUN(t) do { int before_ = g_failures; printf("%s\n", #t); t(); if (g_failures == before_) printf("  ok\n"); } while (0)

static fa_engine* g_engine;

static void data_path(char* out, size_t cap, const char* name)
{
    snprintf(out, cap, "%s/ogg/%s", FA_TEST_DATA_DIR, name);
}

static uint8_t* load_file(const char* name, size_t* size)
{
    char path[512];
    FILE* f;
    uint8_t* p;
    long n;
    data_path(path, sizeof(path), name);
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

static fa_result open_file(const char* name, int32_t track, fa_stream** s, fa_stream_info* info)
{
    char path[512];
    fa_open_options o;
    fa_result r;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    o.audio_track = track;
    data_path(path, sizeof(path), name);
    *s = NULL;
    r = fa_stream_open_path(g_engine, path, &o, s);
    if (r == FA_OK) fa_stream_get_info(*s, info);
    return r;
}

static float* read_all(fa_stream* s, uint32_t channels, int64_t* out_n)
{
    int64_t cap = 1 << 16, n = 0;
    float* all = (float*)malloc((size_t)cap * channels * sizeof(float));
    for (;;) {
        fa_frames got = 0;
        fa_result r;
        if (n + 1024 > cap) {
            cap *= 2;
            all = (float*)realloc(all, (size_t)cap * channels * sizeof(float));
        }
        r = fa_stream_read(s, all + (size_t)n * channels, 1024, &got);
        n += got;
        if (r != FA_OK || got == 0) break;
    }
    *out_n = n;
    return all;
}

static double rms(const float* p, int64_t from, int64_t to, uint32_t ch, uint32_t c)
{
    double acc = 0;
    int64_t i;
    for (i = from; i < to; i++) acc += (double)p[i * ch + c] * p[i * ch + c];
    return to > from ? sqrt(acc / (double)(to - from)) : 0.0;
}

/* Frequency of channel c from its zero crossings. */
static double freq(const float* p, int64_t from, int64_t to, uint32_t ch, uint32_t c, uint32_t rate)
{
    int64_t i, first = -1, last = -1, crossings = 0;
    for (i = from + 1; i < to; i++)
        if ((p[(i - 1) * ch + c] < 0) != (p[i * ch + c] < 0)) {
            if (first < 0) first = i;
            last = i;
            crossings++;
        }
    if (crossings < 2) return 0;
    return (double)(crossings - 1) / 2.0 / ((double)(last - first) / rate);
}

/* Seeks to each target and compares what follows with the full decode. */
static int seek_matches(fa_stream* s, const float* full, int64_t total, uint32_t ch,
                        const int64_t* targets, int count, double tol, double* worst)
{
    int k;
    *worst = 0;
    for (k = 0; k < count; k++) {
        int64_t at = targets[k], len = 3000, n = 0, i;
        float* p;
        if (at > total) continue;
        if (at + len > total) len = total - at;
        p = (float*)calloc((size_t)len * ch + 1, sizeof(float));
        if (fa_stream_seek(s, at) != FA_OK) { free(p); return 0; }
        while (n < len) {
            fa_frames got = 0;
            fa_frames want = len - n < 777 ? len - n : 777;   /* odd sizes on purpose */
            fa_result r = fa_stream_read(s, p + (size_t)n * ch, want, &got);
            n += got;
            if (r != FA_OK || got == 0) break;
        }
        if (n != len) {
            fprintf(stderr, "  seek to %lld: %lld of %lld frames\n", (long long)at, (long long)n, (long long)len);
            free(p);
            return 0;
        }
        for (i = 0; i < len * (int64_t)ch; i++) {
            double d = fabs((double)p[i] - full[(size_t)at * ch + (size_t)i]);
            if (d > *worst) *worst = d;
        }
        free(p);
        if (*worst > tol) {
            fprintf(stderr, "  seek to %lld: differs by %g\n", (long long)at, *worst);
            return 0;
        }
    }
    return 1;
}

/* ---- single streams ------------------------------------------------------------------- */

static void test_opus_file(void)
{
    static const int64_t targets[] = { 0, 1, 959, 960, 961, 30000, 90000, 48000, 95999, 7 };
    fa_stream* s;
    fa_stream_info info;
    float* full;
    int64_t n;
    double worst;
    CHECK_EQ(open_file("tone.opus", -1, &s, &info), FA_OK);
    CHECK(strcmp(info.codec, "opus") == 0);
    CHECK(strcmp(info.container, "ogg") == 0);
    CHECK_EQ(info.format.sample_rate, 48000);
    CHECK_EQ(info.format.channels, 2);
    CHECK_EQ(info.length_frames, 96000);       /* the last granule less the pre-skip */
    CHECK(info.seekable);
    full = read_all(s, 2, &n);
    CHECK_EQ(n, 96000);
    CHECK_NEAR(freq(full, 9600, 86400, 2, 0, 48000), 1000.0, 2.0);
    CHECK_NEAR(rms(full, 9600, 86400, 2, 1), 0.3536, 0.03);
    /* Opus converges within its pre-roll, not always to the bit. */
    CHECK(seek_matches(s, full, n, 2, targets, (int)(sizeof(targets) / sizeof(targets[0])), 1e-4, &worst));
    printf("  Opus seek vs sequential: max difference %.2g\n", worst);
    free(full);
    fa_stream_close(s);
}

static void test_vorbis_file(void)
{
    static const int64_t targets[] = { 0, 1, 127, 128, 1024, 1025, 30000, 44100, 66149, 66000, 5 };
    fa_stream* s;
    fa_stream_info info;
    float* full;
    int64_t n;
    double worst;
    CHECK_EQ(open_file("tone.ogg", -1, &s, &info), FA_OK);
    CHECK(strcmp(info.codec, "vorbis") == 0);
    CHECK_EQ(info.format.sample_rate, 44100);
    CHECK_EQ(info.format.channels, 1);
    CHECK_EQ(info.length_frames, 66150);
    full = read_all(s, 1, &n);
    CHECK_EQ(n, 66150);
    CHECK_NEAR(freq(full, 4410, 61740, 1, 0, 44100), 440.0, 1.0);
    CHECK_NEAR(rms(full, 4410, 61740, 1, 0), 0.3536, 0.03);
    /* Vorbis decodes the same samples after a seek. */
    CHECK(seek_matches(s, full, n, 1, targets, (int)(sizeof(targets) / sizeof(targets[0])), 1e-6, &worst));
    free(full);
    fa_stream_close(s);
}

/* Short pages: a seek skips the packets well before the target without decoding
 * them, on its first page and on the pages after it. */
static void test_seek_skips_packets(void)
{
    static const int64_t vorbis_at[] = { 0, 4410, 4411, 9000, 22050, 44100, 70000, 88200, 100000, 128000, 3 };
    static const int64_t opus_at[] = { 0, 4800, 4801, 9600, 20000, 48000, 72000, 96001, 120000, 139000, 11 };
    fa_stream* s;
    fa_stream_info info;
    float* full;
    int64_t n;
    double worst;
    CHECK_EQ(open_file("pages.ogg", -1, &s, &info), FA_OK);
    full = read_all(s, 1, &n);
    CHECK_EQ(n, 132300);
    CHECK(seek_matches(s, full, n, 1, vorbis_at, (int)(sizeof(vorbis_at) / sizeof(vorbis_at[0])), 1e-6, &worst));
    free(full);
    fa_stream_close(s);
    CHECK_EQ(open_file("pages.opus", -1, &s, &info), FA_OK);
    full = read_all(s, 2, &n);
    CHECK_EQ(n, 144000);
    /* 300 ms of pre-roll: within about -80 dB of the straight decode. */
    CHECK(seek_matches(s, full, n, 2, opus_at, (int)(sizeof(opus_at) / sizeof(opus_at[0])), 1e-4, &worst));
    printf("  Opus seek with short pages vs sequential: max difference %.2g\n", worst);
    free(full);
    fa_stream_close(s);
}

/* Pages of 4 KB, 58 KB and 31 KB: the page buffer grows past its first size and
 * up to the Ogg maximum, and seeks still match a straight decode. */
static void test_large_pages(void)
{
    static const int64_t at[] = { 0, 1000, 44100, 70000, 100000, 131000, 5 };
    fa_stream* s;
    fa_stream_info info;
    float* full;
    int64_t n;
    double worst;
    CHECK_EQ(open_file("bigpages.ogg", -1, &s, &info), FA_OK);
    full = read_all(s, 1, &n);
    CHECK_EQ(n, 132300);
    CHECK(rms(full, 0, n, 1, 0) > 0.2);
    CHECK(seek_matches(s, full, n, 1, at, (int)(sizeof(at) / sizeof(at[0])), 1e-6, &worst));
    free(full);
    fa_stream_close(s);
}

/* 5.1 comes out in the library's order (L R C LFE SL SR), not Vorbis order. */
static void check_surround(const char* name, int lfe_coded)
{
    fa_stream* s;
    fa_stream_info info;
    float* full;
    int64_t n;
    uint32_t c;
    CHECK_EQ(open_file(name, -1, &s, &info), FA_OK);
    CHECK_EQ(info.format.channels, 6);
    CHECK_EQ(info.length_frames, 48000);
    full = read_all(s, 6, &n);
    CHECK_EQ(n, 48000);
    for (c = 0; c < 6; c++) {
        if (c == 3 && !lfe_coded) continue;    /* the Vorbis encoder low-passes the LFE away */
        CHECK_NEAR(freq(full, 4800, 43200, 6, c, 48000), 100.0 * (c + 1), 3.0);
        CHECK_NEAR(rms(full, 4800, 43200, 6, c) * sqrt(2.0), 0.1 * (c + 1), 0.03);
    }
    free(full);
    fa_stream_close(s);
}

static void test_surround_order(void)
{
    check_surround("surround.opus", 1);
    if (g_failures) return;
    check_surround("surround.ogg", 0);
}

/* ---- several streams ----------------------------------------------------------------- */

static void test_two_audio_streams(void)
{
    fa_stream* s;
    fa_stream_info info;
    float* full;
    int64_t n;
    CHECK_EQ(open_file("twotracks.ogg", -1, &s, &info), FA_OK);
    CHECK(strcmp(info.codec, "opus") == 0);
    CHECK_EQ(info.audio_track_count, 2);
    full = read_all(s, 1, &n);
    CHECK_EQ(n, 48000);
    CHECK_NEAR(freq(full, 4800, 43200, 1, 0, 48000), 440.0, 1.0);
    free(full);
    fa_stream_close(s);
    CHECK_EQ(open_file("twotracks.ogg", 1, &s, &info), FA_OK);
    CHECK(strcmp(info.codec, "vorbis") == 0);
    CHECK_EQ(info.format.sample_rate, 44100);
    CHECK_EQ(info.length_frames, 44100);
    full = read_all(s, 1, &n);
    CHECK_EQ(n, 44100);
    CHECK_NEAR(freq(full, 4410, 39690, 1, 0, 44100), 1000.0, 2.0);
    CHECK_NEAR(rms(full, 4410, 39690, 1, 0), 0.1768, 0.02);
    free(full);
    fa_stream_close(s);
    CHECK_EQ(open_file("twotracks.ogg", 2, &s, &info), FA_ERROR_NOT_FOUND);
}

/* Vorbis 44.1 kHz, Vorbis 22.05 kHz, then Opus stereo: one stream at the first
 * link's format, every link at its place on the timeline. */
static void test_chained_file(void)
{
    static const int64_t targets[] = { 0, 66149, 66150, 66151, 80000, 110249, 110250, 110251, 130000, 154000, 1000 };
    fa_stream* s;
    fa_stream_info info;
    float* full;
    int64_t n;
    double worst;
    CHECK_EQ(open_file("chained.ogg", -1, &s, &info), FA_OK);
    CHECK_EQ(info.format.sample_rate, 44100);
    CHECK_EQ(info.format.channels, 1);
    CHECK_EQ(info.length_frames, 66150 + 44100 + 44100);
    CHECK(info.seekable);
    full = read_all(s, 1, &n);
    CHECK_EQ(n, info.length_frames);
    CHECK_NEAR(freq(full, 4410, 61740, 1, 0, 44100), 440.0, 1.0);
    CHECK_NEAR(freq(full, 70560, 105840, 1, 0, 44100), 2000.0, 4.0);
    CHECK_NEAR(rms(full, 70560, 105840, 1, 0), 0.4 / sqrt(2.0), 0.03);
    CHECK_NEAR(freq(full, 114660, 149940, 1, 0, 44100), 300.0, 1.0);
    CHECK_NEAR(rms(full, 114660, 149940, 1, 0), 0.3 / sqrt(2.0), 0.03);
    CHECK(seek_matches(s, full, n, 1, targets, (int)(sizeof(targets) / sizeof(targets[0])), 0.05, &worst));
    printf("  chained seek vs sequential: max difference %.2g\n", worst);
    free(full);
    fa_stream_close(s);
}

/* A chained file of many links, built from the test files with serial numbers
 * of their own (as RFC 3533 asks): the links are found by bisection, and the
 * length and seeks come out as reading the file through gives them. Three long
 * Opus links (tone.opus's audio pages repeated) make the search jump. Junk
 * between two links is skipped; a link with the serial of the one before is
 * found by reading the file through. */
static uint32_t ogg_crc(const uint8_t* p, size_t n)
{
    uint32_t crc = 0;
    size_t i;
    int b;
    for (i = 0; i < n; i++) {
        crc ^= (uint32_t)p[i] << 24;
        for (b = 0; b < 8; b++) crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
    }
    return crc;
}

static void put_le32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static size_t page_size(const uint8_t* d)
{
    size_t n = 27 + d[26], i;
    for (i = 0; i < d[26]; i++) n += d[27 + i];
    return n;
}

/* Appends `name` to out with serial numbers from `serial` on (one per stream in
 * it), its audio pages (all but the last) `repeat` times; returns its length at
 * its own rate, or -1. */
static int64_t append_link(uint8_t* out, size_t* n, const char* name, uint32_t serial, int repeat, uint32_t* rate)
{
    size_t size = 0, pos, first = 0, last = 0;
    uint8_t* d = load_file(name, &size);
    fa_stream* s;
    fa_stream_info info;
    int64_t span = 0, len;
    uint32_t orig[16], seq[16];
    int r, nser = 0, j;
    if (!d || open_file(name, -1, &s, &info) != FA_OK) { free(d); return -1; }
    fa_stream_close(s);
    *rate = info.format.sample_rate;
    len = info.length_frames;
    for (pos = 0; pos < size; pos += page_size(d + pos)) {
        if (!first && (int64_t)fa_le64(d + pos + 6) > 0) first = pos;   /* the first audio page */
        last = pos;
    }
    if (repeat > 1) {
        /* The repeated pages start and end on whole packets; their audio is `span`. */
        size_t before = last - 1, p;
        for (p = first; p < last; p += page_size(d + p)) before = p;
        if (!first || (d[first + 5] & 1) || d[before + 27 + d[before + 26] - 1] == 255) { free(d); return -1; }
        span = (int64_t)fa_le64(d + before + 6);
        len += span * (repeat - 1);
    }
    for (r = 0; r < repeat; r++) {
        for (pos = 0; pos < size; pos += page_size(d + pos)) {
            uint8_t* pg = out + *n;
            size_t ps = page_size(d + pos);
            int64_t g = (int64_t)fa_le64(d + pos + 6);
            if ((pos < first && r > 0) || (pos >= last && r < repeat - 1)) continue;   /* headers first, the last page last */
            memcpy(pg, d + pos, ps);
            for (j = 0; j < nser && orig[j] != fa_le32(pg + 14); j++) {}
            if (j == nser) {
                if (nser == 16) { free(d); return -1; }
                orig[nser] = fa_le32(pg + 14);
                seq[nser++] = 0;
            }
            if (g > 0) g += span * r;
            put_le32(pg + 6, (uint32_t)g);
            put_le32(pg + 10, (uint32_t)((uint64_t)g >> 32));
            put_le32(pg + 14, serial + (uint32_t)j);
            put_le32(pg + 18, seq[j]++);
            put_le32(pg + 22, 0);
            put_le32(pg + 22, ogg_crc(pg, ps));
            *n += ps;
        }
    }
    free(d);
    return len;
}

typedef enum { CHAIN_PLAIN, CHAIN_JUNK, CHAIN_SAME_SERIAL } chain_kind;

static void check_long_chain(chain_kind kind)
{
    static const char* names[] = { "tone.opus", "tone.opus", "pages.opus", "twotracks.ogg", "tone.opus", "pages.opus",
                                   "tone.opus", "pages.opus", "radio.opus", "pages.opus", "tone.opus", "tone.opus" };
    static const int repeat[] = { 1, 20, 1, 1, 1, 1, 16, 1, 1, 1, 24, 1 };
    size_t cap = 8u << 20, n = 0;
    uint8_t* data = (uint8_t*)malloc(cap);
    int64_t expect = 0, full_n, at[16];
    uint32_t rate0 = 0, serial = 1000;
    size_t i;
    int k = 0;
    fa_open_options o;
    fa_stream* s = NULL;
    fa_stream_info info;
    float* full;
    double worst;
    CHECK(data != NULL);
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        uint32_t rate = 0;
        int64_t len;
        if (kind == CHAIN_JUNK && i == 5) {
            memset(data + n, 0x5A, 3000);             /* between two links: skipped */
            n += 3000;
        }
        /* Against the RFC: link 4 reuses the serial of link 3 (cat of two files
         * that got the same serial). The file is then read through. */
        if (!(kind == CHAIN_SAME_SERIAL && i == 4)) serial += 16;
        len = append_link(data, &n, names[i], serial, repeat[i], &rate);
        CHECK(len > 0 && n < cap / 2);
        if (i == 0) rate0 = rate;
        if (i < sizeof(at) / sizeof(at[0])) at[k++] = expect + 7;
        expect += (int64_t)llround((double)len * rate0 / rate);
    }
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    CHECK_EQ(fa_stream_open_memory(g_engine, data, n, &o, &s), FA_OK);
    fa_stream_get_info(s, &info);
    CHECK(info.seekable);
    CHECK_EQ(info.length_frames, expect);
    if (kind == CHAIN_PLAIN) {
        full = read_all(s, 2, &full_n);
        CHECK_EQ(full_n, expect);
        at[k++] = expect / 3;
        at[k++] = expect - 5000;
        CHECK(seek_matches(s, full, full_n, 2, at, k, 1e-3, &worst));
        printf("  %d links, %.0f s: seek vs sequential: max difference %.2g\n", (int)(sizeof(names) / sizeof(names[0])),
               (double)expect / rate0, worst);
        free(full);
    }
    fa_stream_close(s);
    free(data);
}

static void test_long_chain(void)
{
    check_long_chain(CHAIN_PLAIN);
    if (g_failures) return;
    check_long_chain(CHAIN_JUNK);
    if (g_failures) return;
    check_long_chain(CHAIN_SAME_SERIAL);
}

/* A link whose only audio page is also its last (EOS) page plays its end trim, on
 * its own or chained after other links: playing it through gives what a seek to
 * its start (or to any link's) gives, and the file's length. */
static void test_single_page_links(void)
{
    static const char* singles[] = { "r128.opus", "surround.ogg" };
    static const char* chains[][4] = { { "tone.opus", "r128.opus", "tone.opus", "r128.opus" },
                                       { "surround.ogg", "surround.ogg", "surround.ogg", NULL } };
    size_t f, i;
    for (f = 0; f < sizeof(singles) / sizeof(singles[0]); f++) {
        fa_stream* s;
        fa_stream_info info;
        float* full;
        int64_t n, at[3];
        double worst;
        CHECK_EQ(open_file(singles[f], -1, &s, &info), FA_OK);
        full = read_all(s, info.format.channels, &n);
        CHECK_EQ(n, info.length_frames);
        at[0] = 0; at[1] = 1; at[2] = n / 2;
        CHECK(seek_matches(s, full, n, info.format.channels, at, 3, 1e-3, &worst));
        free(full);
        fa_stream_close(s);
    }
    for (f = 0; f < sizeof(chains) / sizeof(chains[0]); f++) {
        size_t cap = 1u << 20, n = 0;
        uint8_t* data = (uint8_t*)malloc(cap);
        int64_t expect = 0, full_n, at[10];
        uint32_t serial = 500, ch, rate0 = 0;
        int k = 0;
        fa_open_options o;
        fa_stream* s = NULL;
        fa_stream_info info;
        float* full;
        double worst;
        CHECK(data != NULL);
        for (i = 0; i < 4 && chains[f][i]; i++) {
            uint32_t rate = 0;
            int64_t len = append_link(data, &n, chains[f][i], serial += 16, 1, &rate);
            CHECK(len > 0 && n < cap / 2);
            if (!rate0) rate0 = rate;
            CHECK_EQ(rate, rate0);   /* the links of a chain share one rate */
            at[k++] = expect;
            at[k++] = expect + 7;
            expect += len;
        }
        fa_open_options_init(&o);
        o.flags = FA_OPEN_DECODE_ONLY;
        CHECK_EQ(fa_stream_open_memory(g_engine, data, n, &o, &s), FA_OK);
        fa_stream_get_info(s, &info);
        ch = info.format.channels;
        CHECK_EQ(info.length_frames, expect);
        full = read_all(s, ch, &full_n);
        CHECK_EQ(full_n, expect);
        at[k++] = expect / 2;   /* then back from the middle */
        at[k++] = 0;
        CHECK(seek_matches(s, full, full_n, ch, at, k, 1e-3, &worst));
        printf("  %s chained: seek vs sequential: max difference %.2g\n", chains[f][1], worst);
        free(full);
        fa_stream_close(s);
        free(data);
    }
}

/* ---- forward-only sources ------------------------------------------------------------ */

typedef struct mem_io {
    const uint8_t* p;
    size_t n, pos;
} mem_io;

static int64_t FA_CALL io_read(void* user, void* buf, size_t bytes)
{
    mem_io* m = (mem_io*)user;
    size_t k = m->n - m->pos < bytes ? m->n - m->pos : bytes;
    memcpy(buf, m->p + m->pos, k);
    m->pos += k;
    return (int64_t)k;
}

static void FA_CALL io_close(void* user) { (void)user; }

/* A forward-only source decodes to what the file decodes to, chained or not
 * (`slack`: resampled links may end a frame apart without the file's length). */
static void compare_forward_only(const char* name, uint32_t ch, int64_t slack)
{
    size_t size = 0;
    uint8_t* data = load_file(name, &size);
    mem_io m;
    fa_io_callbacks io = { io_read, NULL, NULL, io_close };
    fa_open_options o;
    fa_stream* s = NULL;
    fa_stream_info info;
    float *a, *b;
    int64_t na, nb, i;
    CHECK(data != NULL);
    CHECK_EQ(open_file(name, -1, &s, &info), FA_OK);
    a = read_all(s, ch, &na);
    fa_stream_close(s);
    m.p = data;
    m.n = size;
    m.pos = 0;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    CHECK_EQ(fa_stream_open_io(g_engine, &io, &m, &o, &s), FA_OK);
    fa_stream_get_info(s, &info);
    CHECK(!info.seekable);
    CHECK_EQ(info.length_frames, -1);
    b = read_all(s, ch, &nb);
    fa_stream_close(s);
    free(data);
    CHECK(nb >= na && nb <= na + slack);
    for (i = 0; i < na * (int64_t)ch; i++) CHECK_NEAR(b[i], a[i], 0.0);
    free(a);
    free(b);
}

static void test_forward_only_sources(void)
{
    compare_forward_only("tone.opus", 2, 0);
    compare_forward_only("tone.ogg", 1, 0);
    compare_forward_only("chained.ogg", 1, 1);
}

/* ---- raw Opus packets from the app -------------------------------------------------- */

/* The packets of the first logical stream of an Ogg file (no CRC check: test data). */
static int ogg_packets(const uint8_t* d, size_t n, const uint8_t** pk, size_t* len, int max)
{
    size_t pos = 0;
    int count = 0;
    static uint8_t joined[1 << 16];
    size_t jl = 0;
    while (pos + 27 <= n && count < max) {
        uint32_t nseg = d[pos + 26], i;
        const uint8_t* body = d + pos + 27 + nseg;
        size_t off = 0;
        for (i = 0; i < nseg; i++) {
            memcpy(joined + jl, body + off, d[pos + 27 + i]);
            jl += d[pos + 27 + i];
            off += d[pos + 27 + i];
            if (d[pos + 27 + i] < 255) {
                uint8_t* copy = (uint8_t*)malloc(jl ? jl : 1);
                memcpy(copy, joined, jl);
                pk[count] = copy;
                len[count++] = jl;
                jl = 0;
                if (count == max) break;
            }
        }
        pos += 27 + nseg + off;
    }
    return count;
}

static void test_opus_packets_pushed(void)
{
    size_t size = 0, len[256];
    const uint8_t* pk[256];
    uint8_t* data = load_file("tone.opus", &size);
    fa_codec_params p;
    fa_open_options o;
    fa_stream* s;
    fa_stream_info info;
    float* out;
    int64_t n;
    int count, i;
    CHECK(data != NULL);
    count = ogg_packets(data, size, pk, len, 256);
    free(data);
    CHECK(count > 100);
    fa_codec_params_init(&p);
    p.codec = "opus";
    p.sample_rate = 48000;
    p.channels = 2;
    p.extradata = pk[0];                    /* OpusHead: pre-skip 312 */
    p.extradata_size = len[0];
    p.time_base.num = 1;
    p.time_base.den = 48000;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    CHECK_EQ(fa_stream_open_packets(g_engine, &p, &o, &s), FA_OK);
    for (i = 2; i < count; i++) {           /* after OpusHead and OpusTags */
        fa_packet k;
        memset(&k, 0, sizeof(k));
        k.data = pk[i];
        k.size = len[i];
        k.pts = i == 2 ? -312 : FA_PTS_NONE;
        CHECK_EQ(fa_stream_push_packet(s, &k), FA_OK);
    }
    CHECK_EQ(fa_stream_push_end(s), FA_OK);
    out = read_all(s, 2, &n);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK(strcmp(info.codec, "opus") == 0);
    CHECK_EQ(info.format.sample_rate, 48000);
    /* Every packet's audio, less the pre-skip (no end trimming without a container). */
    CHECK_EQ(n, (count - 2) * 960 - 312);
    CHECK_NEAR(freq(out, 9600, 86400, 2, 0, 48000), 1000.0, 2.0);
    free(out);
    fa_stream_close(s);
    for (i = 0; i < count; i++) free((void*)pk[i]);
}

/* ---- tags --------------------------------------------------------------------------- */

static void test_loudness_tags(void)
{
    char path[512];
    fa_source src;
    fa_tag_loudness t;
    data_path(path, sizeof(path), "r128.opus");
    CHECK_EQ(fa_source_open_path(&src, path), FA_OK);
    fa_tags_read_loudness(&src, &t);
    fa_source_close(&src);
    CHECK_NEAR(t.track_lufs, -23.0 + 5.0, 1e-9);        /* R128_TRACK_GAIN=-1280 is -5 dB */
    data_path(path, sizeof(path), "rg.ogg");
    CHECK_EQ(fa_source_open_path(&src, path), FA_OK);
    fa_tags_read_loudness(&src, &t);
    fa_source_close(&src);
    CHECK_NEAR(t.track_lufs, -18.0 + 6.5, 1e-9);
    CHECK_NEAR(t.track_peak_db, 20.0 * log10(0.5), 1e-9);
}

/* ---- damaged files -------------------------------------------------------------------- */

static void exercise_memory(const uint8_t* data, size_t size, int32_t track)
{
    fa_open_options o;
    fa_stream* s = NULL;
    fa_stream_info info;
    float buf[4096 * 8];
    int k;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    o.audio_track = track;
    if (fa_stream_open_memory(g_engine, data, size, &o, &s) != FA_OK) return;
    fa_stream_get_info(s, &info);
    for (k = 0; k < 64; k++) {
        fa_frames got = 0;
        uint32_t ch = info.format.channels ? info.format.channels : 1;
        if (fa_stream_read(s, buf, 4096 / ch, &got) != FA_OK || got == 0) break;
    }
    if (info.seekable && info.length_frames > 0) {
        fa_frames got;
        fa_stream_seek(s, info.length_frames / 2);
        fa_stream_read(s, buf, 1000, &got);
        fa_stream_seek(s, info.length_frames / 7);
        fa_stream_read(s, buf, 1000, &got);
    }
    fa_stream_close(s);
}

static void test_damaged_files_do_not_crash(void)
{
    static const char* names[] = { "tone.opus", "tone.ogg", "surround.opus", "surround.ogg", "twotracks.ogg", "chained.ogg" };
    size_t f;
    uint32_t seed = 12345;
    for (f = 0; f < sizeof(names) / sizeof(names[0]); f++) {
        size_t size = 0, cut, i;
        uint8_t* data = load_file(names[f], &size);
        uint8_t* bad;
        CHECK(data != NULL);
        bad = (uint8_t*)malloc(size);
        for (cut = 1; cut < size; cut += size / 23 + 1) exercise_memory(data, cut, -1);
        /* Changed bytes: most pages fail their CRC, so some are changed with the CRC
         * left out of the picture by hitting packet data the codecs see. */
        for (i = 0; i < 60; i++) {
            size_t j, flips = 1 + i % 8;
            memcpy(bad, data, size);
            for (j = 0; j < flips; j++) {
                seed = seed * 1103515245u + 12345u;
                bad[(seed >> 8) % size] = (uint8_t)(seed >> 3);
            }
            exercise_memory(bad, size, (int32_t)(i % 3) - 1);
        }
        free(bad);
        free(data);
    }
}

/* Packets the codecs get even with good CRCs: random bytes after valid headers. */
static void test_corrupt_packets_are_skipped(void)
{
    static const char* codecs[] = { "opus", "vorbis" };
    static const char* files[] = { "tone.opus", "tone.ogg" };
    int c;
    uint32_t seed = 777;
    for (c = 0; c < 2; c++) {
        size_t size = 0, len[256], extra_len = 0;
        const uint8_t* pk[256];
        uint8_t* data = load_file(files[c], &size);
        uint8_t extra[8192];
        fa_codec codec;
        fa_codec_params p;
        int count, i, k;
        CHECK(data != NULL);
        count = ogg_packets(data, size, pk, len, 256);
        free(data);
        fa_codec_params_init(&p);
        p.codec = codecs[c];
        if (c == 0) {
            memcpy(extra, pk[0], len[0]);
            extra_len = len[0];
        } else {
            /* The three Vorbis headers, Xiph-laced. */
            size_t v;
            extra[extra_len++] = 2;
            for (i = 0; i < 2; i++) {
                for (v = len[i]; v >= 255; v -= 255) extra[extra_len++] = 255;
                extra[extra_len++] = (uint8_t)v;
            }
            for (i = 0; i < 3; i++) { memcpy(extra + extra_len, pk[i], len[i]); extra_len += len[i]; }
        }
        p.extradata = extra;
        p.extradata_size = extra_len;
        CHECK_EQ(fa_codec_open(&codec, &p), FA_OK);
        CHECK(strcmp(codec.vt->name, codecs[c]) == 0);  /* built in, not FFmpeg */
        for (k = 0; k < 2000; k++) {
            uint8_t junk[1500];
            size_t n = 1 + (size_t)(k * 37) % sizeof(junk), j;
            for (j = 0; j < n; j++) { seed = seed * 1103515245u + 12345u; junk[j] = (uint8_t)(seed >> 16); }
            if (k % 3 == 0) {
                /* A real packet with some bits changed. */
                size_t which = 3 + (size_t)k % (size_t)(count - 3);
                n = len[which] < sizeof(junk) ? len[which] : sizeof(junk);
                memcpy(junk, pk[which], n);
                junk[(seed >> 4) % n] ^= (uint8_t)(1u << (seed % 8));
            }
            codec.vt->decode(&codec, junk, n);
            if (k % 100 == 0) codec.vt->flush(&codec);
        }
        fa_codec_close(&codec);
        for (i = 0; i < count; i++) free((void*)pk[i]);
    }
}

int main(void)
{
    fa_engine_config cfg;
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    cfg.worker_threads = 2;
    if (fa_engine_create(&cfg, &g_engine) != FA_OK) { fprintf(stderr, "engine create failed\n"); return 1; }

    RUN(test_opus_file);
    RUN(test_vorbis_file);
    RUN(test_seek_skips_packets);
    RUN(test_large_pages);
    RUN(test_surround_order);
    RUN(test_two_audio_streams);
    RUN(test_chained_file);
    RUN(test_long_chain);
    RUN(test_single_page_links);
    RUN(test_forward_only_sources);
    RUN(test_opus_packets_pushed);
    RUN(test_loudness_tags);
    RUN(test_damaged_files_do_not_crash);
    RUN(test_corrupt_packets_are_skipped);

    fa_engine_destroy(g_engine);
    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    printf("PASS\n");
    return 0;
}
