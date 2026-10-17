/* Loudness normalization tests: the BS.1770 / R128 meter against the EBU
 * compliance cases (Tech 3341 / 3342), true peak, chunked scans versus one pass,
 * the cache, gain tags, the mixer's normalization gain and glide, and the
 * output limiter. Like test_internal, the device is never started: the test
 * calls the mixer itself so every output sample can be checked. */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "fa_internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; return; } } while (0)
#define CHECK_EQ(a, b) do { long long va_ = (long long)(a), vb_ = (long long)(b); if (va_ != vb_) { \
    fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_failures++; return; } } while (0)
#define CHECK_NEAR(a, b, tol) do { double va_ = (a), vb_ = (b); if (!(fabs(va_ - vb_) <= (tol))) { \
    fprintf(stderr, "  FAIL %s:%d: %s ~ %s (%.4f vs %.4f)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_failures++; return; } } while (0)
#define RUN(t) do { int before_ = g_failures; printf("%s\n", #t); t(); if (g_failures == before_) printf("  ok\n"); } while (0)

#define PI 3.14159265358979323846

static fa_engine* g_engine;
static fa_device* g_dev;

/* ---- signals ------------------------------------------------------------------ */

/* A piecewise signal: segments of a sine (or noise) at a level in dBFS
 * (peak amplitude of the sine), the same in every channel. */
typedef struct segment { double seconds, dbfs; int noise; } segment;

static float* make_signal(uint32_t rate, uint32_t ch, const segment* seg, int nseg, double freq, int64_t* out_frames)
{
    int64_t total = 0, at = 0, i;
    int s;
    uint32_t c, rng = 12345;
    float* x;
    for (s = 0; s < nseg; s++) total += (int64_t)(seg[s].seconds * rate + 0.5);
    x = (float*)malloc((size_t)total * ch * sizeof(float));
    for (s = 0; s < nseg; s++) {
        int64_t n = (int64_t)(seg[s].seconds * rate + 0.5);
        double a = pow(10.0, seg[s].dbfs / 20.0);
        for (i = 0; i < n; i++, at++) {
            float v;
            if (seg[s].noise) {
                rng = rng * 1664525u + 1013904223u;
                v = (float)(a * ((double)(rng >> 8) / 8388608.0 - 1.0));
            } else {
                v = (float)(a * sin(2.0 * PI * freq * (double)at / rate));
            }
            for (c = 0; c < ch; c++) x[at * ch + c] = v;
        }
    }
    *out_frames = total;
    return x;
}

/* 32-bit float WAV, optionally followed by an APEv2 tag. */
static uint8_t* make_wav(uint32_t rate, uint32_t ch, const float* x, int64_t frames, const uint8_t* tail, size_t tail_n, size_t* size)
{
    size_t data = (size_t)frames * ch * 4, n = 44 + data + tail_n;
    uint8_t* b = (uint8_t*)malloc(n);
    uint32_t v;
    uint16_t w;
    memcpy(b, "RIFF", 4); v = (uint32_t)(36 + data); memcpy(b + 4, &v, 4); memcpy(b + 8, "WAVEfmt ", 8);
    v = 16; memcpy(b + 16, &v, 4);
    w = 3; memcpy(b + 20, &w, 2);                               /* IEEE float */
    w = (uint16_t)ch; memcpy(b + 22, &w, 2);
    memcpy(b + 24, &rate, 4); v = rate * ch * 4; memcpy(b + 28, &v, 4);
    w = (uint16_t)(ch * 4); memcpy(b + 32, &w, 2); w = 32; memcpy(b + 34, &w, 2);
    memcpy(b + 36, "data", 4); v = (uint32_t)data; memcpy(b + 40, &v, 4);
    memcpy(b + 44, x, data);
    if (tail_n) memcpy(b + 44 + data, tail, tail_n);
    *size = n;
    return b;
}

static int write_file(const char* path, const uint8_t* b, size_t n)
{
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    fwrite(b, 1, n, f);
    fclose(f);
    return 1;
}

/* Integrated loudness, range and true peak of a buffer, in one pass. */
static void measure(const float* x, int64_t frames, uint32_t rate, uint32_t ch, double* i_lufs, double* lra, double* tp)
{
    fa_meter m;
    fa_meter_init(&m, rate, ch, 1, 0);
    fa_meter_process(&m, x, frames);
    *i_lufs = fa_loudness_integrated(m.subs, m.nsubs);
    if (lra) *lra = fa_loudness_range(m.subs, m.nsubs);
    if (tp) *tp = fa_meter_peak_dbtp(&m);
    fa_meter_free(&m);
}

/* ---- meter: EBU Tech 3341 / 3342 ---------------------------------------------------- */

static void check_case(const segment* seg, int nseg, double expect, uint32_t rate)
{
    int64_t n;
    double i;
    float* x = make_signal(rate, 2, seg, nseg, 1000.0, &n);
    measure(x, n, rate, 2, &i, NULL, NULL);
    free(x);
    CHECK_NEAR(i, expect, 0.1);
}

static void test_ebu_3341_integrated(void)
{
    static const segment c1[] = { { 20, -23, 0 } };
    static const segment c2[] = { { 20, -33, 0 } };
    static const segment c3[] = { { 10, -36, 0 }, { 60, -23, 0 }, { 10, -36, 0 } };
    static const segment c4[] = { { 10, -72, 0 }, { 10, -36, 0 }, { 60, -23, 0 }, { 10, -36, 0 }, { 10, -72, 0 } };
    uint32_t rates[2] = { 48000, 44100 };
    int r;
    for (r = 0; r < 2; r++) {
        int before = g_failures;
        check_case(c1, 1, -23.0, rates[r]); if (g_failures != before) return;
        check_case(c2, 1, -33.0, rates[r]); if (g_failures != before) return;
        check_case(c3, 3, -23.0, rates[r]); if (g_failures != before) return;
        check_case(c4, 5, -23.0, rates[r]); if (g_failures != before) return;
    }
}

static void test_ebu_3342_range(void)
{
    static const segment c1[] = { { 20, -20, 0 }, { 20, -30, 0 } };
    static const segment c2[] = { { 20, -20, 0 }, { 20, -15, 0 } };
    static const segment c3[] = { { 20, -40, 0 }, { 20, -20, 0 } };
    int64_t n;
    double i, lra;
    float* x;
    x = make_signal(48000, 2, c1, 2, 1000.0, &n); measure(x, n, 48000, 2, &i, &lra, NULL); free(x);
    CHECK_NEAR(lra, 10.0, 1.0);
    x = make_signal(48000, 2, c2, 2, 1000.0, &n); measure(x, n, 48000, 2, &i, &lra, NULL); free(x);
    CHECK_NEAR(lra, 5.0, 1.0);
    x = make_signal(48000, 2, c3, 2, 1000.0, &n); measure(x, n, 48000, 2, &i, &lra, NULL); free(x);
    CHECK_NEAR(lra, 20.0, 1.0);
}

static void test_true_peak(void)
{
    /* A sine at a quarter of the sample rate, 45 degrees out of phase with the
     * samples: every sample is at 0.707 of the real peak (-3 dB). */
    uint32_t rate = 48000;
    int64_t n = rate * 2, k;
    float* x = (float*)malloc((size_t)n * 2 * sizeof(float));
    double i, tp, sample_peak = 0.0;
    for (k = 0; k < n; k++) {
        float v = (float)(0.5 * sin(2.0 * PI * (double)k / 4.0 + PI / 4.0));
        x[k * 2] = x[k * 2 + 1] = v;
        if (fabs(v) > sample_peak) sample_peak = fabs(v);
    }
    measure(x, n, rate, 2, &i, NULL, &tp);
    free(x);
    CHECK_NEAR(20.0 * log10(sample_peak), -9.03, 0.05);
    CHECK_NEAR(tp, -6.02, 0.3);             /* BS.1770 tolerance is +0.2 / -0.4 dB */
}

static void test_surround_weights(void)
{
    /* Same sine in L only vs in Ls only: the surround channel weighs +1.5 dB. */
    int64_t n = 48000 * 10, k;
    float* x = (float*)calloc((size_t)n * 6, sizeof(float));
    double front, surround;
    for (k = 0; k < n; k++) x[k * 6 + 0] = (float)(0.1 * sin(2.0 * PI * 1000.0 * k / 48000.0));
    measure(x, n, 48000, 6, &front, NULL, NULL);
    memset(x, 0, (size_t)n * 6 * sizeof(float));
    for (k = 0; k < n; k++) x[k * 6 + 4] = (float)(0.1 * sin(2.0 * PI * 1000.0 * k / 48000.0));
    measure(x, n, 48000, 6, &surround, NULL, NULL);
    for (k = 0; k < n; k++) { x[k * 6 + 4] = 0.0f; x[k * 6 + 3] = (float)(0.5 * sin(2.0 * PI * 50.0 * k / 48000.0)); }
    {
        double lfe;
        measure(x, n, 48000, 6, &lfe, NULL, NULL);
        CHECK(lfe < -1000.0);                /* LFE is not counted */
    }
    free(x);
    CHECK_NEAR(surround - front, 10.0 * log10(1.41), 0.01);
}

/* ---- scans ------------------------------------------------------------------------- */

/* Three minutes of noise and tones at changing levels. */
static const segment g_song[] = {
    { 25, -30, 1 }, { 40, -14, 1 }, { 20, -26, 0 }, { 50, -18, 1 }, { 30, -40, 1 }, { 15, -12, 0 }
};

static void test_chunked_scan_matches_one_pass(void)
{
    uint32_t rate = 44100;
    int64_t n;
    size_t size;
    float* x = make_signal(rate, 2, g_song, 6, 440.0, &n);
    uint8_t* wav = make_wav(rate, 2, x, n, NULL, 0, &size);
    double i, lra, tp;
    fa_loudness l;
    int64_t t0;
    CHECK(write_file("loudness-song.wav", wav, size));
    measure(x, n, rate, 2, &i, &lra, &tp);
    free(x);
    free(wav);

    t0 = fa_monotonic_ns();
    CHECK_EQ(fa_scan_loudness_path(g_engine, "loudness-song.wav", &l), FA_OK);
    printf("  scan of %.0f s of audio: %.1f ms (%u scan threads + caller)\n", (double)n / rate,
           (double)(fa_monotonic_ns() - t0) / 1e6, g_engine->scan_thread_count);
    CHECK_EQ(l.is_final, 1);
    CHECK_EQ(l.source, FA_LOUDNESS_SCAN);
    CHECK_NEAR(l.integrated_lufs, i, 0.01);
    CHECK_NEAR(l.range_lu, lra, 0.05);
    CHECK_NEAR(l.true_peak_dbtp, tp, 0.001);
    CHECK_EQ(l.gain_db, 0);                  /* normalization is off */
}

static void test_cache(void)
{
    fa_engine_config cfg;
    fa_engine* e;
    fa_loudness a, b;
    char* dir = (char*)malloc(32);
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    strcpy(dir, "loudness-cache");
    cfg.cache_directory = dir;
    CHECK_EQ(fa_engine_create(&cfg, &e), FA_OK);
    memset(dir, 'x', 31);                     /* the engine keeps its own copy */
    free(dir);
    /* Start clean: a previous run may have left this entry. */
    {
        fa_loudness first;
        fa_scan_loudness_path(e, "loudness-song.wav", &first);
    }
    CHECK_EQ(fa_scan_loudness_path(e, "loudness-song.wav", &a), FA_OK);
    CHECK_EQ(a.source, FA_LOUDNESS_CACHE);
    fa_engine_destroy(e);
    CHECK_EQ(fa_scan_loudness_path(g_engine, "loudness-song.wav", &b), FA_OK);  /* no cache here */
    CHECK_EQ(b.source, FA_LOUDNESS_SCAN);
    CHECK_NEAR(a.integrated_lufs, b.integrated_lufs, 0.001);
    CHECK_NEAR(a.range_lu, b.range_lu, 0.001);
    CHECK_NEAR(a.true_peak_dbtp, b.true_peak_dbtp, 0.001);
}

static void test_scan_errors(void)
{
    fa_loudness l;
    CHECK_EQ(fa_scan_loudness_path(g_engine, "does-not-exist.wav", &l), FA_ERROR_NOT_FOUND);
    CHECK(write_file("loudness-junk.bin", (const uint8_t*)"this is not audio at all", 24));
    CHECK_EQ(fa_scan_loudness_path(g_engine, "loudness-junk.bin", &l), FA_ERROR_UNSUPPORTED_FORMAT);
    CHECK_EQ(fa_scan_loudness_path(NULL, "x", &l), FA_ERROR_INVALID_ARG);
}

/* ---- tags --------------------------------------------------------------------------- */

static size_t put_be32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; return 4; }
static size_t put_le32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); return 4; }

/* ID3v2.3 with a big picture frame first, then two TXXX frames (one UTF-16). */
static size_t make_id3(uint8_t* b)
{
    size_t p = 10, start, tag;
    static const char latin[] = "\0REPLAYGAIN_TRACK_GAIN\0-6.50 dB";
    static const char key16[] = "REPLAYGAIN_ALBUM_GAIN";
    static const char val16[] = "+1.25 dB";
    size_t k;
    memcpy(b, "ID3\x03\x00\x00", 6);
    memcpy(b + p, "APIC", 4); put_be32(b + p + 4, 3000); b[p + 8] = b[p + 9] = 0;
    memset(b + p + 10, 0xAB, 3000);
    p += 10 + 3000;
    memcpy(b + p, "TXXX", 4); put_be32(b + p + 4, (uint32_t)(sizeof(latin) - 1)); b[p + 8] = b[p + 9] = 0;
    memcpy(b + p + 10, latin, sizeof(latin) - 1);
    p += 10 + sizeof(latin) - 1;
    start = p;
    memcpy(b + p, "TXXX", 4); b[p + 8] = b[p + 9] = 0;
    p += 10;
    b[p++] = 1;                                   /* UTF-16 with BOM */
    b[p++] = 0xFF; b[p++] = 0xFE;
    for (k = 0; key16[k]; k++) { b[p++] = (uint8_t)key16[k]; b[p++] = 0; }
    b[p++] = 0; b[p++] = 0;
    b[p++] = 0xFF; b[p++] = 0xFE;
    for (k = 0; val16[k]; k++) { b[p++] = (uint8_t)val16[k]; b[p++] = 0; }
    put_be32(b + start + 4, (uint32_t)(p - start - 10));
    memset(b + p, 0, 64);                         /* padding */
    p += 64;
    tag = p - 10;
    b[6] = (uint8_t)((tag >> 21) & 0x7f); b[7] = (uint8_t)((tag >> 14) & 0x7f);
    b[8] = (uint8_t)((tag >> 7) & 0x7f); b[9] = (uint8_t)(tag & 0x7f);
    return p;
}

/* APEv2 footer-only tag with one item. */
static size_t make_ape(uint8_t* b, const char* key, const char* value)
{
    size_t p = 0, vlen = strlen(value), items;
    p += put_le32(b + p, (uint32_t)vlen);
    p += put_le32(b + p, 0);
    memcpy(b + p, key, strlen(key) + 1); p += strlen(key) + 1;
    memcpy(b + p, value, vlen); p += vlen;
    items = p;
    memcpy(b + p, "APETAGEX", 8);
    put_le32(b + p + 8, 2000);
    put_le32(b + p + 12, (uint32_t)(items + 32));
    put_le32(b + p + 16, 1);
    put_le32(b + p + 20, 0);
    memset(b + p + 24, 0, 8);
    return p + 32;
}

static void test_tag_parsers(void)
{
    uint8_t buf[8192];
    fa_source src;
    fa_tag_loudness t;
    size_t n;

    n = make_id3(buf);
    fa_source_open_memory_ref(&src, buf, n);
    fa_tags_read_loudness(&src, &t);
    CHECK_NEAR(t.track_lufs, -18.0 + 6.5, 1e-9);
    CHECK_NEAR(t.album_lufs, -18.0 - 1.25, 1e-9);

    n = make_ape(buf, "R128_TRACK_GAIN", "-1280");   /* -5 dB relative to -23 LUFS */
    fa_source_open_memory_ref(&src, buf, n);
    fa_tags_read_loudness(&src, &t);
    CHECK_NEAR(t.track_lufs, -18.0, 1e-9);
    CHECK(t.album_lufs != t.album_lufs);

    {
        /* FLAC: fLaC + STREAMINFO (34 bytes) + VORBIS_COMMENT (last). */
        static const char* items[2] = { "replaygain_track_gain=-3.00 dB", "REPLAYGAIN_TRACK_PEAK=0.5" };
        size_t p = 0, vc, k;
        memcpy(buf, "fLaC", 4); p = 4;
        buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = 34;
        memset(buf + p, 0, 34); p += 34;
        vc = p;
        p += 4;
        p += put_le32(buf + p, 3); memcpy(buf + p, "abc", 3); p += 3;
        p += put_le32(buf + p, 2);
        for (k = 0; k < 2; k++) { p += put_le32(buf + p, (uint32_t)strlen(items[k])); memcpy(buf + p, items[k], strlen(items[k])); p += strlen(items[k]); }
        buf[vc] = 0x84;
        buf[vc + 1] = (uint8_t)((p - vc - 4) >> 16); buf[vc + 2] = (uint8_t)((p - vc - 4) >> 8); buf[vc + 3] = (uint8_t)(p - vc - 4);
        fa_source_open_memory_ref(&src, buf, p);
        fa_tags_read_loudness(&src, &t);
        CHECK_NEAR(t.track_lufs, -15.0, 1e-9);
        CHECK_NEAR(t.track_peak_db, -6.0206, 1e-3);
    }
}

/* ---- playback ------------------------------------------------------------------------ */

static int wait_final(fa_stream* s, fa_loudness* l)
{
    int i;
    for (i = 0; i < 1000; i++) {
        fa_stream_get_loudness(s, l);
        if (l->is_final) return 1;
        fa_sleep_ms(5);
    }
    return 0;
}

static void render(fa_stream* s, float* out, uint32_t frames)
{
    int i;
    for (i = 0; i < 2000; i++) {
        if (fa_ring_readable(&s->ring) >= (int64_t)frames || fa_load32(&s->eof)) break;
        fa_sleep_ms(1);
    }
    memset(out, 0, (size_t)frames * 2 * sizeof(float));
    fa_fetch_add64(&g_dev->epoch, 1);
    fa_stream_mix(s, g_dev, out, frames);
    fa_fetch_add64(&g_dev->epoch, 1);
}

static double peak_of(const float* x, size_t n)
{
    double p = 0.0;
    size_t i;
    for (i = 0; i < n; i++) if (fabs(x[i]) > p) p = fabs(x[i]);
    return p;
}

/* A mono sine of `seconds` at `dbfs` peak, at the device rate. */
static fa_stream* open_sine(double seconds, double dbfs, uint32_t flags, const uint8_t* tail, size_t tail_n)
{
    segment seg = { 0, 0, 0 };
    int64_t n;
    size_t size;
    float* x;
    uint8_t* wav;
    fa_open_options o;
    fa_stream* s = NULL;
    seg.seconds = seconds;
    seg.dbfs = dbfs;
    x = make_signal(g_dev->rate, 1, &seg, 1, 1000.0, &n);
    wav = make_wav(g_dev->rate, 1, x, n, tail, tail_n, &size);
    fa_open_options_init(&o);
    o.flags = flags;
    if (fa_stream_open_memory(g_engine, wav, size, &o, &s) != FA_OK) s = NULL;
    free(x);
    free(wav);
    return s;
}

static void test_stream_gain_from_scan(void)
{
    /* Mono 1 kHz sine at -10 dBFS peak measures -13.0 LUFS; target -23 -> -10 dB. */
    fa_stream* s;
    fa_loudness l;
    float out[2 * 4800];
    CHECK_EQ(fa_engine_set_normalization(g_engine, FA_NORMALIZE_TRACK, -23.0), FA_OK);
    s = open_sine(30.0, -10.0, 0, NULL, 0);
    CHECK(s != NULL);
    CHECK(wait_final(s, &l));
    CHECK_EQ(l.source, FA_LOUDNESS_SCAN);
    CHECK_NEAR(l.integrated_lufs, -13.0, 0.1);
    CHECK_NEAR(l.gain_db, -23.0 - l.integrated_lufs, 1e-4);
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    render(s, out, 4800);
    /* Exact gain from the first sample: the result was known before playback. */
    CHECK_NEAR(peak_of(out, 2 * 4800), pow(10.0, (-10.0 + l.gain_db) / 20.0), 2e-3);
    fa_stream_close(s);
}

static void test_glide_to_new_loudness(void)
{
    /* While playing, the loudness changes by 12 dB: the gain glides at 12 dB/s down. */
    fa_stream* s;
    fa_loudness l, app;
    float out[2 * 480];
    int i;
    double first_db, after_half, end_db;
    s = open_sine(10.0, -10.0, 0, NULL, 0);
    CHECK(s != NULL);
    CHECK(wait_final(s, &l));
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    render(s, out, 480);
    first_db = 20.0 * log10(peak_of(out, 960));
    memset(&app, 0, sizeof(app));
    app.integrated_lufs = l.integrated_lufs + 12.0;
    app.range_lu = app.true_peak_dbtp = NAN;
    CHECK_EQ(fa_stream_set_loudness(s, &app), FA_OK);
    CHECK_EQ(fa_stream_get_loudness(s, &l), FA_OK);
    CHECK_EQ(l.source, FA_LOUDNESS_APP);
    for (i = 0; i < 50; i++) render(s, out, 480);   /* 0.5 s at 48 kHz */
    after_half = 20.0 * log10(peak_of(out, 960));
    CHECK_NEAR(first_db - after_half, 12.0 * 0.5 * 48000.0 / g_dev->rate, 0.4);
    for (i = 0; i < 100; i++) render(s, out, 480);
    end_db = 20.0 * log10(peak_of(out, 960));
    CHECK_NEAR(first_db - end_db, 12.0, 0.05);       /* arrived and stays */
    fa_stream_close(s);
}

static void test_tags_win_and_album_mode(void)
{
    uint8_t tail[256];
    size_t n = 0;
    fa_stream* s;
    fa_loudness l;
    /* Both an R128 track gain (-> -18 LUFS) and a ReplayGain album gain (-> -20 LUFS). */
    {
        size_t a = make_ape(tail, "R128_TRACK_GAIN", "-1280");
        /* Two items: rebuild by hand with both. */
        static const char* k2 = "REPLAYGAIN_ALBUM_GAIN";
        static const char* v2 = "2.00 dB";
        uint8_t items[256];
        size_t p = a - 32;                               /* first item's bytes */
        memcpy(items, tail, p);
        p += put_le32(items + p, (uint32_t)strlen(v2));
        p += put_le32(items + p, 0);
        memcpy(items + p, k2, strlen(k2) + 1); p += strlen(k2) + 1;
        memcpy(items + p, v2, strlen(v2)); p += strlen(v2);
        memcpy(tail, items, p);
        memcpy(tail + p, "APETAGEX", 8);
        put_le32(tail + p + 8, 2000);
        put_le32(tail + p + 12, (uint32_t)(p + 32));
        put_le32(tail + p + 16, 2);
        put_le32(tail + p + 20, 0);
        memset(tail + p + 24, 0, 8);
        n = p + 32;
    }
    CHECK_EQ(fa_engine_set_normalization(g_engine, FA_NORMALIZE_TRACK, -16.0), FA_OK);
    s = open_sine(5.0, -10.0, 0, tail, n);
    CHECK(s != NULL);
    CHECK(wait_final(s, &l));
    CHECK_EQ(l.source, FA_LOUDNESS_TAGS);
    CHECK_NEAR(l.integrated_lufs, -18.0, 1e-6);
    CHECK_NEAR(l.gain_db, 2.0, 1e-4);
    CHECK_EQ(fa_engine_set_normalization(g_engine, FA_NORMALIZE_ALBUM, -16.0), FA_OK);
    CHECK_EQ(fa_stream_get_loudness(s, &l), FA_OK);
    CHECK_NEAR(l.gain_db, 4.0, 1e-4);                     /* album: -20 LUFS */
    fa_stream_close(s);
    CHECK_EQ(fa_engine_set_normalization(g_engine, FA_NORMALIZE_TRACK, -23.0), FA_OK);
}

static void test_opt_out_and_off(void)
{
    fa_stream* s;
    fa_loudness l;
    float out[2 * 480];
    s = open_sine(5.0, -10.0, FA_OPEN_NO_NORMALIZE, NULL, 0);
    CHECK(s != NULL);
    CHECK(wait_final(s, &l));                     /* measurable on request */
    CHECK_EQ(l.gain_db, 0);
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    render(s, out, 480);
    CHECK_NEAR(peak_of(out, 960), pow(10.0, -0.5), 2e-3);
    fa_stream_close(s);

    CHECK_EQ(fa_engine_set_normalization(g_engine, FA_NORMALIZE_OFF, 0.0), FA_OK);
    s = open_sine(5.0, -10.0, 0, NULL, 0);
    CHECK(s != NULL);
    CHECK(wait_final(s, &l));
    CHECK_EQ(l.gain_db, 0);
    fa_stream_close(s);
    CHECK_EQ(fa_engine_set_normalization(g_engine, FA_NORMALIZE_TRACK, -23.0), FA_OK);
    CHECK_EQ(fa_engine_set_normalization(g_engine, FA_NORMALIZE_TRACK, 5.0), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_engine_set_normalization(g_engine, (fa_normalize_mode)7, -16.0), FA_ERROR_INVALID_ARG);
}

/* App I/O cannot be read twice, so it is never scanned: the running estimate
 * from the decoded audio is what normalization uses. */
typedef struct mem_reader { const uint8_t* p; size_t n, pos; } mem_reader;
static int64_t FA_CALL mr_read(void* u, void* buf, size_t bytes)
{
    mem_reader* m = (mem_reader*)u;
    size_t k = m->n - m->pos < bytes ? m->n - m->pos : bytes;
    memcpy(buf, m->p + m->pos, k);
    m->pos += k;
    return (int64_t)k;
}

static void test_running_estimate_for_io(void)
{
    segment seg = { 10.0, -10.0, 0 };
    int64_t n;
    size_t size;
    float* x = make_signal(g_dev->rate, 1, &seg, 1, 1000.0, &n);
    uint8_t* wav = make_wav(g_dev->rate, 1, x, n, NULL, 0, &size);
    mem_reader mr;
    fa_io_callbacks io;
    fa_stream* s = NULL;
    fa_loudness l;
    float out[2 * 4800];
    int i;
    free(x);
    mr.p = wav; mr.n = size; mr.pos = 0;
    memset(&io, 0, sizeof(io));
    io.read = mr_read;
    CHECK_EQ(fa_stream_open_io(g_engine, &io, &mr, NULL, &s), FA_OK);
    CHECK_EQ(fa_stream_get_loudness(s, &l), FA_OK);
    CHECK_EQ(l.source, FA_LOUDNESS_NONE);
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    for (i = 0; i < 10; i++) render(s, out, 4800);     /* 1 s */
    CHECK_EQ(fa_stream_get_loudness(s, &l), FA_OK);
    CHECK_EQ(l.source, FA_LOUDNESS_ESTIMATE);
    CHECK_EQ(l.is_final, 0);
    CHECK_NEAR(l.integrated_lufs, -13.0, 0.1);
    CHECK_NEAR(l.gain_db, -10.0, 0.1);
    fa_stream_close(s);
    free(wav);
}

static void test_close_during_scan(void)
{
    /* Closing streams whose scans are queued or running must not wait for the scans. */
    fa_stream* s[6];
    int i;
    int64_t t0;
    for (i = 0; i < 6; i++) { s[i] = open_sine(120.0, -20.0, 0, NULL, 0); CHECK(s[i] != NULL); }
    t0 = fa_monotonic_ns();
    for (i = 5; i >= 0; i--) fa_stream_close(s[i]);
    CHECK((fa_monotonic_ns() - t0) / 1000000 < 2000);
}

/* ---- limiter ------------------------------------------------------------------------- */

static void test_limiter(void)
{
    fa_limiter lim;
    fa_truepeak tp;
    uint32_t rate = 48000, i, n = rate;
    float* x = (float*)malloc((size_t)n * 2 * sizeof(float));
    float* orig = (float*)malloc((size_t)n * 2 * sizeof(float));
    float ceiling = (float)pow(10.0, -1.0 / 20.0);
    double out_tp;
    CHECK_EQ(fa_limiter_init(&lim, rate, 2), FA_OK);

    /* Below the ceiling: passes unchanged, only delayed. */
    for (i = 0; i < n; i++) orig[i * 2] = orig[i * 2 + 1] = x[i * 2] = x[i * 2 + 1] = (float)(0.5 * sin(2.0 * PI * 997.0 * i / rate));
    fa_limiter_process(&lim, x, n, ceiling);
    for (i = (uint32_t)lim.delay; i < n; i++) CHECK(x[i * 2] == orig[(i - (uint32_t)lim.delay) * 2]);

    /* 6 dB over: true peak of the output stays at the ceiling. A burst on
     * silence checks that the look-ahead catches the very first peak. */
    for (i = 0; i < n; i++) {
        float v = i < rate / 4 ? 0.0f : (float)(2.0 * sin(2.0 * PI * 3001.0 * i / rate + 0.3));
        x[i * 2] = x[i * 2 + 1] = v;
    }
    fa_limiter_process(&lim, x, n, ceiling);
    fa_truepeak_init(&tp, rate, 2);
    for (i = 0; i < n; i++) { float m = fa_truepeak_push(&tp, x + i * 2); if (m > tp.peak) tp.peak = m; }
    out_tp = 20.0 * log10(tp.peak);
    fa_truepeak_free(&tp);
    CHECK(out_tp <= -1.0 + 0.1);
    CHECK(out_tp > -1.6);                          /* limits, does not crush */

    /* Turned off: the reduction releases smoothly (no click) and then the audio
     * passes unchanged, delayed. */
    for (i = 0; i < n; i++) x[i * 2] = x[i * 2 + 1] = orig[i * 2] = (float)(1.5 * sin(2.0 * PI * 440.0 * i / rate));
    fa_limiter_process(&lim, x, n, 0.0f);
    for (i = (uint32_t)lim.delay + 1; i < rate / 100; i++) CHECK(fabs(x[i * 2] - x[(i - 1) * 2]) < 0.1);
    fa_limiter_free(&lim);
    CHECK_EQ(fa_limiter_init(&lim, rate, 2), FA_OK);
    for (i = 0; i < n; i++) x[i * 2] = x[i * 2 + 1] = orig[i * 2];
    fa_limiter_process(&lim, x, n, 0.0f);
    for (i = (uint32_t)lim.delay; i < n; i++) CHECK(x[i * 2] == orig[(i - (uint32_t)lim.delay) * 2]);
    fa_limiter_free(&lim);
    free(x);
    free(orig);

    CHECK_EQ(fa_engine_set_limiter(g_engine, FA_LIMITER_ON, -1.0), FA_OK);
    CHECK_EQ(fa_engine_set_limiter(g_engine, FA_LIMITER_AUTO, 1.0), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_engine_set_limiter(g_engine, FA_LIMITER_AUTO, -1.0), FA_OK);
}

static void test_latency_includes_limiter(void)
{
    fa_time t;
    CHECK_EQ(fa_device_get_latency(g_dev, &t), FA_OK);
    CHECK(t >= (fa_time)((double)g_dev->limiter.delay * 1e9 / g_dev->rate));
}

int main(void)
{
    fa_engine_config cfg;
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    cfg.worker_threads = 2;
    if (fa_engine_create(&cfg, &g_engine) != FA_OK) { fprintf(stderr, "engine create failed\n"); return 1; }
    if (fa_device_open(g_engine, NULL, &g_dev) != FA_OK) { fprintf(stderr, "device open failed\n"); return 1; }
    printf("null device at %u Hz (not started: this test is the audio thread)\n", g_dev->rate);

    RUN(test_ebu_3341_integrated);
    RUN(test_ebu_3342_range);
    RUN(test_true_peak);
    RUN(test_surround_weights);
    RUN(test_chunked_scan_matches_one_pass);
    RUN(test_cache);
    RUN(test_scan_errors);
    RUN(test_tag_parsers);
    RUN(test_stream_gain_from_scan);
    RUN(test_glide_to_new_loudness);
    RUN(test_tags_win_and_album_mode);
    RUN(test_opt_out_and_off);
    RUN(test_running_estimate_for_io);
    RUN(test_close_during_scan);
    RUN(test_limiter);
    RUN(test_latency_includes_limiter);

    fa_device_close(g_dev);
    fa_engine_destroy(g_engine);
    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    printf("all passed\n");
    return 0;
}
