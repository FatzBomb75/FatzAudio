/* Tracker modules (libopenmpt): detection, info and texts, rendering, exact
 * seeking by frame and by order/row, live settings and channel mute, subsongs,
 * the order/row heard and FA_EVENT_TRACKER_ROW on a device, damaged files. On
 * MOD files built here; white-box only to play the audio thread itself. */
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
#define CHECK_NEAR(a, b, tol) do { double va_ = (a), vb_ = (b); if (fabs(va_ - vb_) > (tol)) { \
    fprintf(stderr, "  FAIL %s:%d: %s ~ %s (%.6f vs %.6f)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_failures++; return; } } while (0)
#define RUN(t) do { const char* only_ = getenv("FA_TEST"); int before_ = g_failures; \
    if (only_ && strcmp(only_, #t) != 0) break; \
    printf("%s\n", #t); t(); if (g_failures == before_) printf("  ok\n"); } while (0)

#define RATE 44100
#define FA_TRACKER_STEP_TEST 64      /* dec_tracker.c notes row starts to within this */
#define PI 3.14159265358979323846
#define ROW_S 0.06                 /* speed 3 at tempo 125: 3 ticks of 20 ms */
#define LOOP_ROWS 40               /* rows a looping play_rows raises at least (2.5 s) */

static fa_engine* g_engine;

typedef struct buf { uint8_t* d; size_t n; } buf;

static void put(buf* b, const void* p, size_t n)
{
    b->d = (uint8_t*)realloc(b->d, b->n + n);
    memcpy(b->d + b->n, p, n);
    b->n += n;
}
static void put8(buf* b, unsigned v) { uint8_t x = (uint8_t)v; put(b, &x, 1); }
static void put16be(buf* b, unsigned v) { put8(b, (v >> 8) & 255); put8(b, v & 255); }
static void putstr(buf* b, const char* s, size_t n) { size_t k = strlen(s); put(b, s, k < n ? k : n); while (k++ < n) put8(b, 0); }

/* One cell: sample (1-based, 0 = none), Amiga period (0 = none), effect, parameter. */
static void cell(uint8_t* p, unsigned sample, unsigned period, unsigned fx, unsigned param)
{
    p[0] = (uint8_t)((sample & 0xF0) | (period >> 8));
    p[1] = (uint8_t)(period & 0xFF);
    p[2] = (uint8_t)(((sample & 0x0F) << 4) | fx);
    p[3] = (uint8_t)param;
}

#define PERIOD_C2 428              /* 8287 Hz on a PAL Amiga */
#define SINE_LEN 32                /* one period: 8287 / 32 = 259 Hz */

/* "Test Module", 4 channels (M.K.), sample 1 "Sine" (a looped sine), two orders:
 * pattern 0 plays the sine on channel 0 (left), pattern 1 on channel 1 (right);
 * each note is cut at row 8 and each pattern breaks at row 15, at speed 3, so
 * each order lasts 16 rows of 60 ms. */
static buf make_mod(void)
{
    buf m = { 0 };
    uint8_t pat[2][64 * 4 * 4];
    int i, p;
    putstr(&m, "Test Module", 20);
    for (i = 0; i < 31; i++) {
        putstr(&m, i == 0 ? "Sine" : "", 22);
        put16be(&m, i == 0 ? SINE_LEN / 2 : 0);   /* length in words */
        put8(&m, 0);                              /* finetune */
        put8(&m, i == 0 ? 64 : 0);                /* volume */
        put16be(&m, 0);                           /* loop start */
        put16be(&m, i == 0 ? SINE_LEN / 2 : 1);   /* loop length */
    }
    put8(&m, 2);     /* song length */
    put8(&m, 127);
    put8(&m, 0);
    put8(&m, 1);
    for (i = 2; i < 128; i++) put8(&m, 0);
    put(&m, "M.K.", 4);
    memset(pat, 0, sizeof(pat));
    for (p = 0; p < 2; p++) {
        cell(pat[p] + (0 * 4 + p) * 4, 1, PERIOD_C2, 0, 0);       /* the note */
        cell(pat[p] + (0 * 4 + 3) * 4, 0, 0, 0xF, 3);              /* speed 3 */
        cell(pat[p] + (8 * 4 + p) * 4, 0, 0, 0xC, 0);              /* volume 0 */
        cell(pat[p] + (15 * 4 + 2) * 4, 0, 0, 0xD, 0);             /* next order */
    }
    put(&m, pat, sizeof(pat));
    for (i = 0; i < SINE_LEN; i++) put8(&m, (unsigned)(uint8_t)(int8_t)lrint(sin(2.0 * PI * i / SINE_LEN) * 120.0));
    return m;
}

static fa_stream* open_mod(const buf* b, fa_result* out_r)
{
    fa_open_options o;
    fa_stream* s = NULL;
    fa_result r;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    r = fa_stream_open_memory(g_engine, b->d, b->n, &o, &s);
    if (out_r) *out_r = r;
    return r == FA_OK ? s : NULL;
}

static fa_frames read_all(fa_stream* s, float* out, fa_frames frames, fa_frames chunk)
{
    fa_frames done = 0;
    while (done < frames) {
        fa_frames got = 0, want = frames - done < chunk ? frames - done : chunk;
        fa_result r = fa_stream_read(s, out + done * 2, want, &got);
        if (r != FA_OK || got == 0) break;
        done += got;
    }
    return done;
}

/* RMS of one channel (0 left, 1 right) over [from, to). */
static double rms_ch(const float* x, fa_frames from, fa_frames to, int ch)
{
    double acc = 0;
    fa_frames i;
    for (i = from; i < to; i++) acc += (double)x[i * 2 + ch] * x[i * 2 + ch];
    return to > from ? sqrt(acc / (double)(to - from)) : 0.0;
}

static fa_frames at(double seconds) { return (fa_frames)(seconds * RATE); }

static void test_detect_and_info(void)
{
    buf mod = make_mod();
    fa_stream* s = open_mod(&mod, NULL);
    fa_stream_info info;
    fa_tracker_info ti;
    char text[64];
    size_t len = 0;
    CHECK(s);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK(strcmp(info.codec, "mod") == 0);
    CHECK_EQ(info.format.channels, 2);
    CHECK_EQ(info.format.sample_rate, RATE);
    CHECK_NEAR(info.duration / 1e9, 32 * ROW_S, 0.01);
    memset(&ti, 0, sizeof(ti));
    ti.struct_size = sizeof(ti);
    CHECK_EQ(fa_stream_get_tracker_info(s, &ti), FA_OK);
    CHECK(strcmp(ti.type, "mod") == 0);
    CHECK(strcmp(ti.title, "Test Module") == 0);
    CHECK(ti.type_name[0] != 0);
    CHECK_EQ(ti.channels, 4);
    CHECK_EQ(ti.orders, 2);
    CHECK_EQ(ti.patterns, 2);
    CHECK_EQ(ti.restart_order, 0);
    CHECK_EQ(ti.restart_row, 0);
    CHECK_EQ(ti.samples, 31);
    CHECK_NEAR(ti.duration, info.duration / 1e9, 0.001);
    /* Texts: by index, cut at the capacity, sizes. */
    CHECK_EQ(fa_stream_get_tracker_text(s, FA_TRACKER_TEXT_SAMPLE, 0, text, sizeof(text), &len), FA_OK);
    CHECK(strcmp(text, "Sine") == 0);
    CHECK_EQ(len, 4);
    CHECK_EQ(fa_stream_get_tracker_text(s, FA_TRACKER_TEXT_SAMPLE, 0, text, 3, &len), FA_OK);
    CHECK(strcmp(text, "Si") == 0);
    CHECK_EQ(len, 4);
    CHECK_EQ(fa_stream_get_tracker_text(s, FA_TRACKER_TEXT_SAMPLE, 0, NULL, 0, &len), FA_OK);
    CHECK_EQ(len, 4);
    CHECK_EQ(fa_stream_get_tracker_text(s, FA_TRACKER_TEXT_SAMPLE, 31, text, sizeof(text), NULL), FA_ERROR_NOT_FOUND);
    CHECK_EQ(fa_stream_get_tracker_text(s, FA_TRACKER_TEXT_MESSAGE, 0, text, sizeof(text), NULL), FA_OK);
    CHECK_EQ(fa_stream_get_tracker_text(s, FA_TRACKER_TEXT_MESSAGE, 1, text, sizeof(text), NULL), FA_ERROR_NOT_FOUND);
    /* The stream's tags carry the title. */
    {
        fa_tags* tags = NULL;
        CHECK_EQ(fa_stream_get_tags(s, FA_TAGS_NO_INFO, &tags), FA_OK);
        CHECK(strcmp(fa_tags_get(tags, FA_TAG_TITLE), "Test Module") == 0);
        fa_tags_close(tags);
    }
    /* A struct from an older header (without `subsong`) is filled as far as it
     * goes; one smaller than the first version is refused. */
    {
        fa_tracker_info small;
        memset(&small, 0x55, sizeof(small));
        small.struct_size = (uint32_t)(offsetof(fa_tracker_info, duration) + sizeof(double));
        CHECK_EQ(fa_stream_get_tracker_info(s, &small), FA_OK);
        CHECK_EQ(small.struct_size, offsetof(fa_tracker_info, duration) + sizeof(double));
        CHECK_EQ(small.subsong, 0x55555555);
        CHECK_NEAR(small.duration, ti.duration, 1e-9);
        small.struct_size = 4;
        CHECK_EQ(fa_stream_get_tracker_info(s, &small), FA_ERROR_INVALID_ARG);
    }
    fa_stream_close(s);
    free(mod.d);
}

static void test_render_and_seek(void)
{
    buf mod = make_mod();
    fa_stream* s = open_mod(&mod, NULL);
    fa_stream_info info;
    fa_frames len, got, k;
    float *all, *other;
    double t = -1;
    CHECK(s);
    fa_stream_get_info(s, &info);
    len = info.length_frames;
    CHECK_NEAR((double)len / RATE, 32 * ROW_S, 0.01);
    all = (float*)calloc((size_t)len * 2 + 64, sizeof(float));
    other = (float*)calloc((size_t)len * 2 + 64, sizeof(float));
    /* Exactly the length, then the end. */
    got = read_all(s, all, len + 32, 1000);
    CHECK_EQ(got, len);
    /* Order 0 on the left, order 1 on the right (libopenmpt narrows MOD stereo a
     * little, as heard on an Amiga with headphones), silence after each cut. */
    CHECK(rms_ch(all, at(0.05), at(0.4), 0) > 0.05);
    CHECK(rms_ch(all, at(0.05), at(0.4), 0) > 2.0 * rms_ch(all, at(0.05), at(0.4), 1));
    CHECK(rms_ch(all, at(0.6), at(0.9), 0) < 1e-3);
    CHECK(rms_ch(all, at(1.0), at(1.4), 1) > 0.05);
    CHECK(rms_ch(all, at(1.0), at(1.4), 1) > 2.0 * rms_ch(all, at(1.0), at(1.4), 0));
    for (k = 0; k < len * 2; k++) CHECK(all[k] == all[k] && fabsf(all[k]) <= 1.0f);
    /* The read size changes nothing. */
    CHECK_EQ(fa_stream_seek(s, 0), FA_OK);
    CHECK_EQ(read_all(s, other, len, 777), len);
    CHECK(memcmp(all, other, (size_t)len * 2 * sizeof(float)) == 0);
    /* Order/row times; a row never played is not found. */
    CHECK_EQ(fa_stream_get_tracker_time(s, 1, 0, &t), FA_OK);
    CHECK_NEAR(t, 16 * ROW_S, 0.002);
    CHECK_EQ(fa_stream_get_tracker_time(s, 0, 4, &t), FA_OK);
    CHECK_NEAR(t, 4 * ROW_S, 0.002);
    CHECK_EQ(fa_stream_get_tracker_time(s, 0, 20, &t), FA_ERROR_NOT_FOUND);   /* past the break */
    CHECK_EQ(fa_stream_get_tracker_time(s, 2, 0, &t), FA_ERROR_NOT_FOUND);
    /* Seeking by order/row lands where playing through gets there. */
    CHECK_EQ(fa_stream_seek_tracker(s, 1, 0), FA_OK);
    {
        fa_frames pos = -1, from = at(16 * ROW_S), n;
        double diff = 0;
        fa_stream_get_position(s, &pos);
        CHECK_NEAR((double)pos, (double)from, 2.0);
        n = read_all(s, other, len - pos, 1000);
        CHECK_EQ(n, len - pos);
        for (k = 0; k < n * 2; k++) diff = fmax(diff, fabs((double)other[k] - all[pos * 2 + k]));
        CHECK(diff < 0.01);
    }
    /* A seek to any frame lands on it exactly: the frames after it match. */
    {
        static const double targets[] = { 0.0, 0.1234, 0.5, 1.21, 1.9 };
        size_t i;
        for (i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
            fa_frames to = at(targets[i]), pos = -1, n;
            double diff = 0;
            CHECK_EQ(fa_stream_seek(s, to), FA_OK);
            fa_stream_get_position(s, &pos);
            CHECK_EQ(pos, to);
            n = read_all(s, other, 4096, 1000);
            CHECK(n > 0);
            for (k = 0; k < n * 2; k++) diff = fmax(diff, fabs((double)other[k] - all[to * 2 + k]));
            if (diff >= 0.01) fprintf(stderr, "  seek to %.4f s: off by %.4f\n", targets[i], diff);
            CHECK(diff < 0.01);
        }
    }
    free(all);
    free(other);
    fa_stream_close(s);
    free(mod.d);
}

static void test_live_settings(void)
{
    buf mod = make_mod();
    fa_stream* s = open_mod(&mod, NULL);
    fa_frames len = at(32 * ROW_S);
    float* x = (float*)calloc((size_t)len * 2, sizeof(float));
    int32_t v = -5;
    CHECK(s);
    /* Defaults and range checks. */
    CHECK_EQ(fa_stream_get_tracker_param(s, FA_TRACKER_INTERPOLATION, &v), FA_OK);
    CHECK_EQ(v, 0);
    CHECK_EQ(fa_stream_get_tracker_param(s, FA_TRACKER_STEREO_SEPARATION, &v), FA_OK);
    CHECK_EQ(v, 100);
    CHECK_EQ(fa_stream_get_tracker_param(s, FA_TRACKER_VOLUME_RAMPING, &v), FA_OK);
    CHECK_EQ(v, -1);
    CHECK_EQ(fa_stream_get_tracker_param(s, FA_TRACKER_AMIGA_RESAMPLER, &v), FA_OK);
    CHECK_EQ(v, 0);
    CHECK_EQ(fa_stream_set_tracker_param(s, FA_TRACKER_INTERPOLATION, 3), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_stream_set_tracker_param(s, FA_TRACKER_STEREO_SEPARATION, 201), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_stream_set_tracker_param(s, FA_TRACKER_VOLUME_RAMPING, 11), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_stream_set_tracker_param(s, (fa_tracker_param)99, 0), FA_ERROR_INVALID_ARG);
    /* Mono: both channels the same. */
    CHECK_EQ(fa_stream_set_tracker_param(s, FA_TRACKER_STEREO_SEPARATION, 0), FA_OK);
    CHECK_EQ(fa_stream_set_tracker_param(s, FA_TRACKER_INTERPOLATION, 2), FA_OK);
    CHECK_EQ(fa_stream_get_tracker_param(s, FA_TRACKER_INTERPOLATION, &v), FA_OK);
    CHECK_EQ(v, 2);
    CHECK_EQ(read_all(s, x, len, 1000), len);
    CHECK(rms_ch(x, at(0.05), at(0.4), 0) > 0.02);
    CHECK_NEAR(rms_ch(x, at(0.05), at(0.4), 0), rms_ch(x, at(0.05), at(0.4), 1), 1e-4);
    /* Channel mute: channel 1 (the second order's note) is silent. */
    CHECK_EQ(fa_stream_set_tracker_param(s, FA_TRACKER_STEREO_SEPARATION, 100), FA_OK);
    CHECK_EQ(fa_stream_set_tracker_channel_mute(s, 4, 1), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_stream_set_tracker_channel_mute(s, 1, 1), FA_OK);
    CHECK_EQ(fa_stream_get_tracker_channel_mute(s, 1, &v), FA_OK);
    CHECK_EQ(v, 1);
    CHECK_EQ(fa_stream_get_tracker_channel_mute(s, 0, &v), FA_OK);
    CHECK_EQ(v, 0);
    CHECK_EQ(fa_stream_seek(s, 0), FA_OK);
    CHECK_EQ(read_all(s, x, len, 1000), len);
    CHECK(rms_ch(x, at(0.05), at(0.4), 0) > 0.05);
    CHECK(rms_ch(x, at(1.0), at(1.4), 0) + rms_ch(x, at(1.0), at(1.4), 1) < 1e-4);
    free(x);
    fa_stream_close(s);
    free(mod.d);
}

/* Sample data that looks like MPEG audio (chained MP3 frame headers) must not
 * make the module play as an MP3: the module's signature wins. */
static void test_mp3_like_samples(void)
{
    buf mod = make_mod();
    fa_stream* s;
    fa_stream_info info;
    size_t i, extra = 4000;
    uint8_t* d;
    /* Sample 2: 4000 bytes holding MPEG-1 layer III 128 kbit/s 44.1 kHz headers every 417 bytes. */
    mod.d[20 + 30 + 22] = (uint8_t)((extra / 2) >> 8);
    mod.d[20 + 30 + 23] = (uint8_t)((extra / 2) & 255);
    mod.d[20 + 30 + 25] = 64;
    mod.d[20 + 30 + 29] = 1;
    d = (uint8_t*)calloc(1, extra);
    for (i = 0; i + 4 <= extra; i += 417) { d[i] = 0xFF; d[i + 1] = 0xFB; d[i + 2] = 0x90; d[i + 3] = 0x00; }
    put(&mod, d, extra);
    free(d);
    s = open_mod(&mod, NULL);
    CHECK(s);
    if (s) {
        CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
        CHECK(strcmp(info.codec, "mod") == 0);
        fa_stream_close(s);
    }
    free(mod.d);
}

/* Two subsongs: order 0 plays pattern 2, which jumps back to order 0 at row 15
 * (so the first song is that one pattern), and orders 1 and 2 (patterns 0 and 1)
 * are never reached from it: the second subsong. */
static buf make_two_songs(void)
{
    buf one = make_mod(), m = { 0 };
    uint8_t pat[64 * 4 * 4];
    size_t patterns = 1084;
    one.d[950] = 3;                    /* song length */
    one.d[952] = 2;
    one.d[953] = 0;
    one.d[954] = 1;
    put(&m, one.d, patterns + 2 * sizeof(pat));
    memcpy(pat, one.d + patterns, sizeof(pat));
    cell(pat + (15 * 4 + 2) * 4, 0, 0, 0xB, 0);   /* back to order 0: the song ends */
    put(&m, pat, sizeof(pat));
    put(&m, one.d + patterns + 2 * sizeof(pat), one.n - patterns - 2 * sizeof(pat));
    free(one.d);
    return m;
}

static fa_stream* open_track(const buf* b, int32_t track, fa_result* out_r)
{
    fa_open_options o;
    fa_stream* s = NULL;
    fa_result r;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    o.audio_track = track;
    r = fa_stream_open_memory(g_engine, b->d, b->n, &o, &s);
    if (out_r) *out_r = r;
    return r == FA_OK ? s : NULL;
}

static void test_subsongs(void)
{
    buf mod = make_two_songs();
    fa_stream* s;
    fa_stream_info info;
    fa_tracker_info ti;
    fa_result r;
    double t;
    s = open_track(&mod, -1, NULL);
    CHECK(s);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK_EQ(info.audio_track_count, 2);
    CHECK_NEAR(info.duration / 1e9, 16 * ROW_S, 0.01);
    ti.struct_size = sizeof(ti);
    CHECK_EQ(fa_stream_get_tracker_info(s, &ti), FA_OK);
    CHECK_EQ(ti.subsongs, 2);
    CHECK_EQ(ti.subsong, 0);
    /* Order 1 belongs to the other subsong. */
    CHECK_EQ(fa_stream_get_tracker_time(s, 1, 0, &t), FA_ERROR_NOT_FOUND);
    CHECK_EQ(fa_stream_get_tracker_time(s, 2, 3, &t), FA_ERROR_NOT_FOUND);
    CHECK_EQ(fa_stream_seek_tracker(s, 1, 0), FA_ERROR_NOT_FOUND);
    fa_stream_close(s);

    s = open_track(&mod, 1, NULL);
    CHECK(s);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK_NEAR(info.duration / 1e9, 32 * ROW_S, 0.01);
    ti.struct_size = sizeof(ti);
    CHECK_EQ(fa_stream_get_tracker_info(s, &ti), FA_OK);
    CHECK_EQ(ti.subsong, 1);
    CHECK_NEAR(ti.duration, 32 * ROW_S, 0.01);
    CHECK_EQ(fa_stream_get_tracker_time(s, 0, 5, &t), FA_ERROR_NOT_FOUND);
    CHECK_EQ(fa_stream_get_tracker_time(s, 1, 0, &t), FA_OK);
    CHECK_NEAR(t, 0, 1e-6);
    CHECK_EQ(fa_stream_get_tracker_time(s, 2, 0, &t), FA_OK);
    CHECK_NEAR(t, 16 * ROW_S, 1e-6);
    fa_stream_close(s);

    CHECK(open_track(&mod, 2, &r) == NULL);
    CHECK_EQ(r, FA_ERROR_NOT_FOUND);
    free(mod.d);
}

/* The order and row heard: decode-only streams hear what was read. */
static void test_position(void)
{
    buf mod = make_mod();
    fa_stream* s = open_mod(&mod, NULL);
    float* x = (float*)malloc(sizeof(float) * 2 * RATE);
    uint32_t order = 9, row = 9;
    CHECK(s && x);
    /* Rows of 60 ms: 0.5 s is order 0 row 8, 1.0 s order 1 row 0. */
    read_all(s, x, RATE / 2, 1000);
    CHECK_EQ(fa_stream_get_tracker_position(s, &order, &row), FA_OK);
    CHECK_EQ(order, 0);
    CHECK_EQ(row, 8);
    read_all(s, x, RATE / 2, 777);
    CHECK_EQ(fa_stream_get_tracker_position(s, &order, &row), FA_OK);
    CHECK_EQ(order, 1);
    CHECK_EQ(row, 0);
    /* After a seek, at once: 1.5 s is order 1 row 9; then back to the start. */
    CHECK_EQ(fa_stream_seek(s, RATE * 3 / 2), FA_OK);
    CHECK_EQ(fa_stream_get_tracker_position(s, &order, &row), FA_OK);
    CHECK_EQ(order, 1);
    CHECK_EQ(row, 9);
    CHECK_EQ(fa_stream_seek(s, 0), FA_OK);
    CHECK_EQ(fa_stream_get_tracker_position(s, &order, &row), FA_OK);
    CHECK_EQ(order, 0);
    CHECK_EQ(row, 0);
    /* A row start, exactly: frame 0.06 s (row 1) and the frame before (row 0). */
    {
        fa_frames r1 = (fa_frames)llround(ROW_S * RATE);
        CHECK_EQ(fa_stream_seek(s, r1 + FA_TRACKER_STEP_TEST), FA_OK);
        CHECK_EQ(fa_stream_get_tracker_position(s, &order, &row), FA_OK);
        CHECK_EQ(row, 1);
        CHECK_EQ(fa_stream_seek(s, r1 - 1), FA_OK);
        CHECK_EQ(fa_stream_get_tracker_position(s, &order, &row), FA_OK);
        CHECK_EQ(row, 0);
    }
    CHECK_EQ(fa_stream_get_tracker_position(s, NULL, &row), FA_ERROR_INVALID_ARG);
    fa_stream_close(s);
    free(x);
    free(mod.d);
}

/* ---- on a device: FA_EVENT_TRACKER_ROW ------------------------------------------ */

typedef struct row_rec {
    fa_mutex lock;
    int count;
    int64_t values[64];
    fa_frames frames[64];
    int ended;
} row_rec;

static void FA_CALL on_event(const fa_event* ev, void* user)
{
    row_rec* r = (row_rec*)user;
    fa_mutex_lock(&r->lock);
    if (ev->type == FA_EVENT_TRACKER_ROW && r->count < 64) { r->values[r->count] = ev->value; r->frames[r->count] = ev->frame; r->count++; }
    if (ev->type == FA_EVENT_ENDED) r->ended = 1;
    fa_mutex_unlock(&r->lock);
}

/* Plays the song through the device (rendered here, not by a device thread)
 * for 2.5 s, from `seek` (looping, if asked), and collects the row events. */
static void play_rows(const buf* mod, int32_t mode, int loop, fa_frames seek, row_rec* rec)
{
    fa_device* dev = NULL;
    fa_stream* s = NULL;
    fa_open_options o;
    float out[512 * 2];
    int64_t rendered = 0, total, deadline;
    int tries;
    memset(rec, 0, sizeof(*rec));
    fa_mutex_init(&rec->lock);
    if (fa_device_open(g_engine, NULL, &dev) != FA_OK) return;
    fa_open_options_init(&o);
    if (fa_stream_open_memory(g_engine, mod->d, mod->n, &o, &s) != FA_OK) { fa_device_close(dev); return; }
    fa_stream_set_tracker_param(s, FA_TRACKER_ROW_EVENTS, mode);
    fa_stream_set_loop(s, loop);
    if (seek > 0) fa_stream_seek(s, seek);
    fa_stream_set_event_callback(s, on_event, rec);
    fa_stream_attach(s, dev, NULL);
    fa_stream_play(s);
    total = (int64_t)dev->rate * 5 / 2;
    while (rendered < total) {
        for (tries = 0; tries < 2000; tries++) {
            if (fa_ring_readable(&s->ring) >= 512 || fa_load32(&s->eof)) break;
            fa_sleep_ms(1);
        }
        fa_fetch_add64(&dev->epoch, 1);
        fa_device_render(dev, out, 512);
        fa_fetch_add64(&dev->epoch, 1);
        rendered += 512;
    }
    /* Events reach the callback on the event thread, after the render: wait for
     * them (the end, or a looping song's rows) by the clock, not by sleeps. */
    for (deadline = fa_monotonic_ns() + 2000000000LL; fa_monotonic_ns() < deadline;) {
        int done;
        fa_mutex_lock(&rec->lock);
        done = loop ? rec->count >= LOOP_ROWS : rec->ended;
        fa_mutex_unlock(&rec->lock);
        if (done) break;
        fa_sleep_ms(5);
    }
    fa_stream_close(s);
    fa_device_close(dev);
}

static void test_row_events_on_device(void)
{
    buf mod = make_mod();
    row_rec rec;
    int i;
    fa_frames row = (fa_frames)llround(ROW_S * RATE);
    /* Every row, once, in order, within a step of its start. */
    play_rows(&mod, 1, 0, 0, &rec);
    CHECK(rec.ended);
    CHECK_EQ(rec.count, 32);
    for (i = 0; i < rec.count; i++) {
        CHECK_EQ(rec.values[i] >> 16, i / 16);
        CHECK_EQ(rec.values[i] & 0xFFFF, i % 16);
        CHECK(rec.frames[i] >= i * row && rec.frames[i] <= i * row + FA_TRACKER_STEP_TEST);
    }
    fa_mutex_destroy(&rec.lock);
    /* Orders only. */
    play_rows(&mod, 2, 0, 0, &rec);
    CHECK(rec.ended);
    CHECK_EQ(rec.count, 2);
    CHECK_EQ(rec.values[0], 0);
    CHECK_EQ(rec.values[1], 1 << 16);
    fa_mutex_destroy(&rec.lock);
    /* Off (the default): none. */
    play_rows(&mod, 0, 0, 0, &rec);
    CHECK(rec.ended);
    CHECK_EQ(rec.count, 0);
    fa_mutex_destroy(&rec.lock);
    /* From a seek into row 8 (0.5 s): the rows after it. */
    play_rows(&mod, 1, 0, RATE / 2, &rec);
    CHECK(rec.ended);
    CHECK_EQ(rec.count, 32 - 9);
    CHECK_EQ(rec.values[0], 9);
    CHECK(rec.frames[0] >= 9 * row && rec.frames[0] <= 9 * row + FA_TRACKER_STEP_TEST);
    fa_mutex_destroy(&rec.lock);
    /* Looping: the song's rows, then the first ones again from frame 0. */
    play_rows(&mod, 1, 1, 0, &rec);
    CHECK(rec.count >= LOOP_ROWS);
    for (i = 0; i < rec.count; i++) {
        CHECK_EQ(rec.values[i] >> 16, (i % 32) / 16);
        CHECK_EQ(rec.values[i] & 0xFFFF, i % 16);
        CHECK_EQ(rec.frames[i], rec.frames[i % 32]);
    }
    fa_mutex_destroy(&rec.lock);
    free(mod.d);
}

static void test_not_a_module(void)
{
    fa_audio_format fmt;
    fa_stream* s = NULL;
    fa_tracker_info ti;
    int32_t v;
    double t;
    char text[8];
    memset(&fmt, 0, sizeof(fmt));
    fmt.sample_format = FA_FORMAT_F32;
    fmt.sample_rate = RATE;
    fmt.channels = 2;
    CHECK_EQ(fa_stream_open_push(g_engine, &fmt, NULL, &s), FA_OK);
    ti.struct_size = sizeof(ti);
    CHECK_EQ(fa_stream_get_tracker_info(s, &ti), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_stream_get_tracker_text(s, FA_TRACKER_TEXT_MESSAGE, 0, text, sizeof(text), NULL), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_stream_get_tracker_time(s, 0, 0, &t), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_stream_seek_tracker(s, 0, 0), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_stream_set_tracker_param(s, FA_TRACKER_INTERPOLATION, 0), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_stream_get_tracker_param(s, FA_TRACKER_INTERPOLATION, &v), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_stream_set_tracker_channel_mute(s, 0, 1), FA_ERROR_INVALID_ARG);
    {
        uint32_t o_, r_;
        CHECK_EQ(fa_stream_get_tracker_position(s, &o_, &r_), FA_ERROR_INVALID_ARG);
    }
    fa_stream_close(s);
    CHECK_EQ(fa_stream_get_tracker_info(NULL, &ti), FA_ERROR_INVALID_ARG);
}

/* Cut short or overwritten anywhere: an error or a stream, never a crash. */
static void test_damaged(void)
{
    buf mod = make_mod();
    size_t cut;
    unsigned seed = 1;
    float x[2048];
    for (cut = 0; cut < mod.n; cut += mod.n / 41 + 1) {
        fa_stream* s = NULL;
        fa_open_options o;
        fa_open_options_init(&o);
        o.flags = FA_OPEN_DECODE_ONLY;
        if (fa_stream_open_memory(g_engine, mod.d, cut, &o, &s) == FA_OK) {
            read_all(s, x, 1024, 512);
            fa_stream_close(s);
        }
    }
    for (cut = 0; cut < 200; cut++) {
        buf copy = { 0 };
        fa_stream* s;
        int k;
        put(&copy, mod.d, mod.n);
        for (k = 0; k < 8; k++) {
            seed = seed * 1103515245u + 12345u;
            copy.d[(seed >> 8) % copy.n] = (uint8_t)(seed >> 16);
        }
        s = open_mod(&copy, NULL);
        if (s) {
            read_all(s, x, 1024, 512);
            fa_stream_seek(s, 5000);
            read_all(s, x, 1024, 512);
            fa_stream_close(s);
        }
        free(copy.d);
    }
    free(mod.d);
}

int main(void)
{
    fa_engine_config cfg;
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    cfg.worker_threads = 2;
    if (fa_engine_create(&cfg, &g_engine) != FA_OK) { fprintf(stderr, "engine create failed\n"); return 1; }
    RUN(test_detect_and_info);
    RUN(test_render_and_seek);
    RUN(test_live_settings);
    RUN(test_mp3_like_samples);
    RUN(test_subsongs);
    RUN(test_position);
    RUN(test_row_events_on_device);
    RUN(test_not_a_module);
    RUN(test_damaged);
    fa_engine_destroy(g_engine);
    printf(g_failures ? "%d failure(s)\n" : "PASS\n", g_failures);
    return g_failures ? 1 : 0;
}
