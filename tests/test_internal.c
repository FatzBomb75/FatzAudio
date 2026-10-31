/* White-box tests of the decode-ahead pipeline and the mixer. The device is
 * opened on the null backend but never started, so this test plays the part of
 * the audio thread and calls fa_stream_mix itself: every output sample can be
 * checked exactly. Decode workers and the event thread run for real. */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "fa_internal.h"
#include "mdct.h"
#include "pkt_index.h"
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
#define RUN(t) do { int before_ = g_failures; printf("%s\n", #t); t(); if (g_failures == before_) printf("  ok\n"); } while (0)

static fa_engine* g_engine;
static fa_device* g_dev;

/* ---- media ------------------------------------------------------------------ */

/* 16-bit mono WAV. kind 0: constant 0.5; kind 1: a sawtooth whose value encodes
 * the frame index (exact in float), so continuity can be checked sample by sample. */
/* kind 2 / 3: a 1 kHz / 100 Hz sine at a quarter of full scale. */
static int16_t sample_of(int kind, uint32_t i)
{
    if (kind >= 2)
        return (int16_t)lrint(8192.0 * sin(2.0 * 3.14159265358979 * (kind == 2 ? 1000.0 : 100.0) * i / g_dev->rate));
    return kind == 0 ? 16384 : (int16_t)((int)(i % 4096) * 8 - 16384);
}
static float expected(int kind, uint32_t i) { return (float)sample_of(kind, i) / 32768.0f; }

static uint8_t* make_wav(uint32_t rate, uint32_t frames, int kind, size_t* size)
{
    size_t data = (size_t)frames * 2, n = 44 + data;
    uint8_t* b = (uint8_t*)malloc(n);
    uint32_t i, v;
    memcpy(b, "RIFF", 4); v = (uint32_t)(n - 8); memcpy(b + 4, &v, 4); memcpy(b + 8, "WAVEfmt ", 8);
    v = 16; memcpy(b + 16, &v, 4);
    b[20] = 1; b[21] = 0; b[22] = 1; b[23] = 0;               /* PCM, mono */
    memcpy(b + 24, &rate, 4); v = rate * 2; memcpy(b + 28, &v, 4);
    b[32] = 2; b[33] = 0; b[34] = 16; b[35] = 0;
    memcpy(b + 36, "data", 4); v = (uint32_t)data; memcpy(b + 40, &v, 4);
    for (i = 0; i < frames; i++) { int16_t sv = sample_of(kind, i); memcpy(b + 44 + (size_t)i * 2, &sv, 2); }
    *size = n;
    return b;
}

static fa_stream* open_attached(uint32_t frames, int kind, uint32_t flags)
{
    size_t n;
    uint8_t* wav = make_wav(g_dev->rate, frames, kind, &n);
    fa_open_options o;
    fa_stream* s = NULL;
    fa_open_options_init(&o);
    o.flags = flags;
    if (fa_stream_open_memory(g_engine, wav, n, &o, &s) != FA_OK) s = NULL;
    free(wav);
    if (s && fa_stream_attach(s, g_dev, NULL) != FA_OK) { fa_stream_close(s); s = NULL; }
    return s;
}

/* Act as the audio callback for `frames` frames, after giving the workers time
 * to have that much decoded (as they would in real time). */
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

/* ---- events ----------------------------------------------------------------- */

typedef struct recorder {
    fa_mutex lock;
    int count;
    int by_type[32];
    fa_event ev[64];
} recorder;

static void FA_CALL record(const fa_event* ev, void* user)
{
    recorder* r = (recorder*)user;
    fa_mutex_lock(&r->lock);
    if (r->count < 64) r->ev[r->count] = *ev;
    if ((int)ev->type >= 0 && (int)ev->type < 32) r->by_type[ev->type]++;
    r->count++;
    fa_mutex_unlock(&r->lock);
}

static int count_events(recorder* r, fa_event_type type, int wait_for)
{
    int n = 0, tries;
    for (tries = 0; tries < 200; tries++) {
        fa_mutex_lock(&r->lock);
        n = r->by_type[type];
        fa_mutex_unlock(&r->lock);
        if (n >= wait_for) break;
        fa_sleep_ms(5);
    }
    return n;
}

static void rec_init(recorder* r) { memset(r, 0, sizeof(*r)); fa_mutex_init(&r->lock); }
static void rec_free(recorder* r) { fa_mutex_destroy(&r->lock); }

/* ---- unit tests -------------------------------------------------------------- */

/* The compact packet list gives back exactly what was added: offsets and times
 * across group edges, breaks (gaps, jumps in time), joined PCM packets and
 * durations past 16 bits; and finds packets by time as a binary search would. */
static void test_pkt_index(void)
{
    enum { N = 5000 };
    static fa_pkt ref[N];
    fa_pkt_index x;
    fa_pkt p;
    uint32_t seed = 12345;
    int64_t off = 100, pts = 0;
    size_t i, n = 0;
    memset(&x, 0, sizeof(x));
    for (i = 0; i < N + 600; i++) {
        uint32_t r, size, dur;
        seed = seed * 1103515245u + 12345u;
        r = seed >> 8;
        size = 1 + r % 900;
        dur = (r % 97 == 0) ? 70000 + r % 1000 : 1024;
        if (r % 13 == 0) off += 5000 + r % 70000;      /* a gap (interleaved video) */
        if (r % 211 == 0) pts += 33;                   /* a jump in time */
        if (n > 0 && r % 7 == 0) {                     /* joined to the last packet */
            CHECK_EQ(fa_pkt_extend_last(&x, size, dur), FA_OK);
            ref[n - 1].size += size;
            ref[n - 1].dur += dur;
        } else {
            if (n == N) break;
            CHECK_EQ(fa_pkt_add(&x, off, size, pts, dur), FA_OK);
            ref[n].off = off; ref[n].pts = pts; ref[n].size = size; ref[n].dur = dur;
            n++;
        }
        off += size;
        pts += dur;
    }
    fa_pkt_trim(&x);
    CHECK_EQ(x.n, n);
    for (i = 0; i < n; i++) {
        fa_pkt_get(&x, i, &p);
        if (p.off != ref[i].off || p.pts != ref[i].pts || p.size != ref[i].size || p.dur != ref[i].dur) { CHECK_EQ((long long)i, -1); }
    }
    for (i = 0; i < 2000; i++) {
        int64_t t = (int64_t)((uint64_t)i * (uint64_t)(ref[n - 1].pts + 5000) / 2000) - 100;
        size_t lo = 0, hi = n;
        while (hi - lo > 1) { size_t mid = lo + (hi - lo) / 2; if (ref[mid].pts <= t) lo = mid; else hi = mid; }
        if (fa_pkt_find(&x, t) != lo) { CHECK_EQ((long long)t, -1); }
    }
    fa_pkt_free(&x);
}

/* The one UTF-8 check that tags, stream titles, HTTP headers and MIDI text share:
 * well-formed RFC 3629 only. */
static void test_utf8_valid(void)
{
    static const struct { const char* s; int ok; } cases[] = {
        { "", 1 }, { "plain", 1 }, { "Caf\xc3\xa9", 1 }, { "\xe2\x82\xac", 1 }, { "\xf0\x9f\x8e\xa4", 1 },
        { "\xed\x9f\xbf", 1 }, { "\xee\x80\x80", 1 }, { "\xf4\x8f\xbf\xbf", 1 },
        { "Caf\xe9", 0 },                         /* Latin-1 */
        { "\xc0\xaf", 0 }, { "\xc1\xbf", 0 },    /* overlong two-byte */
        { "\xe0\x80\xaf", 0 }, { "\xe0\x9f\xbf", 0 }, { "\xf0\x8f\xbf\xbf", 0 },   /* overlong */
        { "\xed\xa0\x80", 0 }, { "\xed\xbf\xbf", 0 },                                 /* surrogates */
        { "\xf4\x90\x80\x80", 0 }, { "\xf5\x80\x80\x80", 0 }, { "\xff", 0 },        /* above U+10FFFF */
        { "\xe2\x82", 0 }, { "\x80", 0 }, { "\xc3\x28", 0 },                               /* cut off, stray */
    };
    size_t i;
    char out[4];
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (fa_utf8_valid(cases[i].s, strlen(cases[i].s)) != cases[i].ok) { CHECK_EQ((int)i, -1); }
    }
    CHECK(!fa_utf8_valid("\xe2\x82\xac", 2));                  /* the length counts, not a NUL */
    CHECK_EQ(fa_put_utf8(out, 0x1F3A4), 4);
    CHECK(memcmp(out, "\xf0\x9f\x8e\xa4", 4) == 0);
    CHECK_EQ(fa_put_utf8(out, 0x20AC), 3);
    CHECK(memcmp(out, "\xe2\x82\xac", 3) == 0);
}

static void test_ring_wraps(void)
{
    fa_ring r;
    float in[20], out[20];
    const float *p1, *p2;
    int64_t n1, n2, i, round;
    CHECK_EQ(fa_ring_init(&r, 2, 7), FA_OK);
    for (round = 0; round < 10; round++) {
        for (i = 0; i < 10; i++) in[i] = (float)(round * 100 + i);
        CHECK_EQ(fa_ring_writable(&r), 7);
        fa_ring_write(&r, in, 5);                     /* 5 frames = 10 floats */
        CHECK_EQ(fa_ring_readable(&r), 5);
        fa_ring_regions(&r, 5, &p1, &n1, &p2, &n2);
        CHECK_EQ(n1 + n2, 5);
        memcpy(out, p1, (size_t)n1 * 2 * sizeof(float));
        memcpy(out + n1 * 2, p2, (size_t)n2 * 2 * sizeof(float));
        fa_ring_advance_read(&r, 5);
        for (i = 0; i < 10; i++) CHECK(out[i] == in[i]);
    }
    fa_ring_free(&r);
}

static void test_curves(void)
{
    static const float none[4] = { 0, 0, 0, 0 };
    static const float linear_bez[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    static const float ease[4] = { 0.25f, 0.1f, 0.25f, 1.0f };
    int c;
    float t;
    for (c = FA_CURVE_LINEAR; c <= FA_CURVE_BEZIER; c++) {
        const float* b = c == FA_CURVE_BEZIER ? ease : none;
        float prev = 0.0f;
        CHECK_NEAR(fa_curve_eval(c, b, 0.0f, 1), 0.0, 1e-6);
        CHECK_NEAR(fa_curve_eval(c, b, 1.0f, 1), 1.0, 1e-6);
        for (t = 0.05f; t < 1.0f; t += 0.05f) {          /* monotonic */
            float v = fa_curve_eval(c, b, t, 1);
            CHECK(v >= prev - 1e-6f);
            prev = v;
        }
    }
    /* Equal power: a fade-out and a fade-in at the same progress keep out^2 + in^2 = 1. */
    for (t = 0.0f; t <= 1.0f; t += 0.1f) {
        float out = 1.0f - fa_curve_eval(FA_CURVE_EQUAL_POWER, none, t, 0);
        float in = fa_curve_eval(FA_CURVE_EQUAL_POWER, none, t, 1);
        CHECK_NEAR(out * out + in * in, 1.0, 1e-5);
    }
    for (t = 0.0f; t <= 1.0f; t += 0.1f) CHECK_NEAR(fa_curve_eval(FA_CURVE_BEZIER, linear_bez, t, 1), t, 1e-4);
    CHECK_NEAR(fa_curve_eval(FA_CURVE_BEZIER, ease, 0.5f, 1), 0.8024, 2e-3); /* CSS "ease" at 50% */
    CHECK_NEAR(fa_curve_eval(FA_CURVE_SCURVE, none, 0.5f, 1), 0.5, 1e-6);
}

/* ---- pipeline tests ----------------------------------------------------------- */

static void test_mix_is_bit_exact(void)
{
    fa_stream* s = open_attached(g_dev->rate * 2, 1, 0);
    float out[2 * 4800];
    uint32_t done = 0, i;
    fa_frames pos;
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    while (done < g_dev->rate) {                     /* one second in callback-sized pieces */
        render(s, out, 480);
        for (i = 0; i < 480; i++) {
            CHECK(out[i * 2] == expected(1, done + i));
            CHECK(out[i * 2 + 1] == expected(1, done + i));
        }
        done += 480;
    }
    CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
    CHECK_EQ(pos, g_dev->rate);
    CHECK_EQ(fa_load32(&s->underruns), 0);
    fa_stream_close(s);
}

static void test_seek_while_attached(void)
{
    fa_stream* s = open_attached(g_dev->rate * 3, 1, 0);
    float out[2 * 512];
    uint32_t i;
    fa_frames pos;
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    render(s, out, 512);
    CHECK_EQ(fa_stream_seek(s, 100000), FA_OK);
    CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
    CHECK_EQ(pos, 100000);
    CHECK(fa_ring_readable(&s->ring) >= (int64_t)g_dev->rate / 20);  /* prefilled before returning */
    render(s, out, 512);
    for (i = 0; i < 512; i++) CHECK(out[i * 2] == expected(1, 100000 + i));
    fa_stream_close(s);
}

/* The system moved the default device's stream by itself (WASAPI, Core Audio):
 * every stream on it gets FA_EVENT_DEVICE_CHANGED (value 0) with its next mix. */
static void test_device_rerouted_event(void)
{
    fa_stream* s = open_attached(g_dev->rate * 2, 0, 0);
    recorder rec;
    float out[2 * 512];
    rec_init(&rec);
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_set_event_callback(s, record, &rec), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    fa_device_simulate(g_dev, 0);
    fa_fetch_add64(&g_dev->epoch, 1);
    fa_device_render_at(g_dev, out, 512, fa_monotonic_ns());
    fa_fetch_add64(&g_dev->epoch, 1);
    CHECK_EQ(count_events(&rec, FA_EVENT_DEVICE_CHANGED, 1), 1);
    CHECK_EQ(rec.ev[0].value, 0);
    fa_fetch_add64(&g_dev->epoch, 1);
    fa_device_render_at(g_dev, out, 512, fa_monotonic_ns());   /* only once */
    fa_fetch_add64(&g_dev->epoch, 1);
    fa_sleep_ms(30);
    CHECK_EQ(count_events(&rec, FA_EVENT_DEVICE_CHANGED, 1), 1);
    fa_stream_close(s);
    rec_free(&rec);
}

/* A started device that stops by itself (unplugged) is reopened on the default at
 * the same rate: its streams stay attached, keep their position and play on. */
static void test_device_lost_reopens_default(void)
{
    fa_device* d = NULL;
    fa_stream* s = NULL;
    recorder rec;
    size_t n;
    uint8_t* wav = make_wav(g_dev->rate, g_dev->rate * 20, 0, &n);
    fa_frames before = 0, after = 0;
    uint32_t rate;
    int i;
    rec_init(&rec);
    CHECK_EQ(fa_device_open(g_engine, NULL, &d), FA_OK);
    rate = d->rate;
    CHECK_EQ(fa_stream_open_memory(g_engine, wav, n, NULL, &s), FA_OK);
    free(wav);
    CHECK_EQ(fa_stream_set_event_callback(s, record, &rec), FA_OK);
    CHECK_EQ(fa_stream_attach(s, d, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    CHECK_EQ(fa_device_start(d), FA_OK);
    CHECK_EQ(fa_device_set_volume(d, 0.25f), FA_OK);
    for (i = 0; i < 200 && before == 0; i++) { fa_sleep_ms(5); fa_stream_get_position(s, &before); }
    CHECK(before > 0);
    fa_device_simulate(d, 1);
    CHECK_EQ(count_events(&rec, FA_EVENT_DEVICE_CHANGED, 1), 1);
    CHECK_EQ(rec.ev[0].value, 1);
    CHECK_EQ(fa_load32(&d->lost), 0);
    CHECK_EQ(d->rate, rate);
    CHECK(d->device_ok);
    {
        float v = 0;
        CHECK_EQ(fa_device_get_volume(d, &v), FA_OK);
        CHECK_NEAR(v, 0.25, 1e-6);
        CHECK_EQ(ma_device_get_master_volume(&d->device, &v), MA_SUCCESS);
        CHECK_NEAR(v, 0.25, 1e-6);                   /* set again on the reopened device */
    }
    for (i = 0; i < 200; i++) {
        fa_stream_get_position(s, &after);
        if (after > before + (fa_frames)rate / 20) break;
        fa_sleep_ms(5);
    }
    CHECK(after > before + (fa_frames)rate / 20);   /* plays on, from where it was */
    {
        int32_t started = 0;
        CHECK_EQ(fa_device_is_started(d, &started), FA_OK);
        CHECK_EQ(started, 1);
    }
    /* Stopped, then rerouted by the backend itself (WASAPI when the default goes):
     * no loss, so no reopen of our own. */
    fa_device_simulate(d, 1);
    fa_device_simulate(d, 0);
    fa_sleep_ms(500);
    CHECK_EQ(fa_load32(&d->lost), 0);
    CHECK_EQ(count_events(&rec, FA_EVENT_DEVICE_CHANGED, 2), 1);
    fa_stream_close(s);
    fa_device_close(d);
    rec_free(&rec);
}

/* Waits for a worker to carry out a pending fa_stream_seek_async. */
static int wait_seek_done(fa_stream* s)
{
    int i;
    for (i = 0; i < 2000; i++) {
        if (fa_load64(&s->seek_pending) < 0 && !fa_load32(&s->flushing)) return 1;
        fa_sleep_ms(1);
    }
    return 0;
}

static void test_seek_async(void)
{
    fa_stream* s = open_attached(g_dev->rate * 4, 1, 0);
    fa_stream* d = NULL;
    float out[2 * 512];
    uint32_t i;
    fa_frames pos = 0;
    fa_time clock = 0;
    int k;
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    render(s, out, 512);
    /* A drag: only the last target matters, and the call never waits. */
    for (k = 1; k <= 10; k++) CHECK_EQ(fa_stream_seek_async(s, k * 15000), FA_OK);
    CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
    CHECK_EQ(pos, 150000);
    CHECK_EQ(fa_stream_get_clock(s, &clock), FA_OK);
    CHECK_NEAR((double)clock, 150000.0 * 1e9 / g_dev->rate, 1000.0);
    CHECK(wait_seek_done(s));
    render(s, out, 512);
    for (i = 0; i < 512; i++) CHECK(out[i * 2] == expected(1, 150000 + i));
    /* While one is pending, a newer target never waits for the API lock (a worker
     * holds it while it seeks a web stream). */
    fa_mutex_lock(&s->api_lock);       /* as a worker seeking holds it: pending 10000 */
    fa_store64(&s->seek_max, INT64_MAX);
    fa_store32(&s->flushing, 1);
    fa_store64(&s->seek_pending, 10000);
    CHECK_EQ(fa_stream_seek_async(s, 20000), FA_OK);
    CHECK_EQ(fa_load64(&s->seek_pending), 20000);
    CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);   /* nor does reading the position */
    CHECK_EQ(pos, 20000);
    fa_mutex_unlock(&s->api_lock);
    CHECK(wait_seek_done(s));
    render(s, out, 512);
    for (i = 0; i < 512; i++) CHECK(out[i * 2] == expected(1, 20000 + i));
    /* A blocking seek replaces one not carried out yet. */
    CHECK_EQ(fa_stream_seek_async(s, 50000), FA_OK);
    CHECK_EQ(fa_stream_seek(s, 80000), FA_OK);
    CHECK_EQ(fa_load64(&s->seek_pending), -1);
    CHECK(wait_seek_done(s));
    render(s, out, 512);
    for (i = 0; i < 512; i++) CHECK(out[i * 2] == expected(1, 80000 + i));
    /* Out of range and not seekable are refused at once. */
    CHECK_EQ(fa_stream_seek_async(s, (fa_frames)g_dev->rate * 5), FA_ERROR_INVALID_ARG);
    fa_stream_close(s);
    /* Not attached: seeks at once. */
    {
        size_t n;
        uint8_t* wav = make_wav(g_dev->rate, g_dev->rate, 1, &n);
        CHECK_EQ(fa_stream_open_memory(g_engine, wav, n, NULL, &d), FA_OK);
        free(wav);
        CHECK_EQ(fa_stream_seek_async(d, 1234), FA_OK);
        CHECK_EQ(fa_stream_get_position(d, &pos), FA_OK);
        CHECK_EQ(pos, 1234);
        CHECK_EQ(fa_load64(&d->seek_pending), -1);
        fa_stream_close(d);
    }
}

/* Detaching with a seek pending continues from its target. */
static void test_seek_async_then_detach(void)
{
    fa_stream* s = open_attached(g_dev->rate * 4, 1, 0);
    fa_frames pos = 0;
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    fa_mutex_lock(&s->decode_lock);    /* no worker can carry it out */
    fa_store32(&s->flushing, 1);
    fa_store64(&s->seek_pending, 70000);
    fa_mutex_unlock(&s->decode_lock);
    CHECK_EQ(fa_stream_detach(s), FA_OK);
    CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
    CHECK_EQ(pos, 70000);
    CHECK_EQ(fa_load64(&s->seek_pending), -1);
    fa_stream_close(s);
}

static void test_detach_resumes_at_heard_position(void)
{
    fa_stream* s = open_attached(g_dev->rate * 3, 1, 0);
    float out[2 * 1000];
    uint32_t i;
    fa_frames pos;
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    render(s, out, 1000);
    fa_sleep_ms(30);                                  /* workers decode well ahead */
    CHECK(fa_ring_readable(&s->ring) > 1000);
    CHECK_EQ(fa_stream_detach(s), FA_OK);
    CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
    CHECK_EQ(pos, 1000);
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    render(s, out, 1000);
    for (i = 0; i < 1000; i++) CHECK(out[i * 2] == expected(1, 1000 + i));
    fa_stream_close(s);
}

static void test_volume_and_pan(void)
{
    fa_stream* s = open_attached(g_dev->rate, 0, 0);
    float out[2 * 2048], v = 0.0f;
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_VOLUME, 0.5f), FA_OK);
    CHECK_EQ(fa_stream_get_param(s, FA_PARAM_VOLUME, &v), FA_OK);
    CHECK_NEAR(v, 0.5, 1e-6);                         /* reads back before the mixer ran */
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_PAN, 1.0f), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    render(s, out, 16);                               /* paused->playing: params applied directly */
    CHECK_NEAR(out[0], 0.0, 1e-6);                    /* pan right: left silent */
    CHECK_NEAR(out[1], 0.25, 1e-6);                   /* 0.5 signal * 0.5 volume */
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_PAN, 0.0f), FA_OK);
    render(s, out, 2048);                             /* glides over 5 ms, then steady */
    CHECK(out[1] < 0.25f + 1e-6f && out[0] < out[2046 * 2]);
    CHECK_NEAR(out[2047 * 2], 0.25, 1e-6);
    CHECK_NEAR(out[2047 * 2 + 1], 0.25, 1e-6);
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_VOLUME, -1.0f), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_TEMPO, 0.0f), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_PITCH, NAN), FA_ERROR_INVALID_ARG);
    fa_stream_close(s);
}

static void test_ramp_shape_and_done_event(void)
{
    fa_stream* s = open_attached(g_dev->rate, 0, 0);
    recorder rec;
    fa_ramp ramp;
    float out[2 * 4800];
    uint32_t i, len = 4800;
    rec_init(&rec);
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_set_event_callback(s, record, &rec), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    memset(&ramp, 0, sizeof(ramp));
    ramp.curve = FA_CURVE_EQUAL_POWER;
    ramp.tag = 42;
    /* Fade out over exactly 4800 frames. */
    CHECK_EQ(fa_stream_ramp_param(s, FA_PARAM_VOLUME, 0.0f, (fa_time)((double)len * 1e9 / g_dev->rate), 0, &ramp), FA_OK);
    render(s, out, len);
    for (i = 0; i < len; i += 160) {                  /* equal-power fade-out: cos(t * pi/2) */
        double want = 0.5 * cos((double)i / len * 3.14159265358979 / 2);
        CHECK_NEAR(out[i * 2], want, 2e-3);
    }
    render(s, out, 64);
    CHECK_NEAR(out[0], 0.0, 1e-6);
    CHECK_EQ(count_events(&rec, FA_EVENT_RAMP_DONE, 1), 1);
    CHECK_EQ(rec.ev[0].value, 42);
    CHECK_EQ(rec.ev[0].stream, s);
    fa_stream_close(s);
    rec_free(&rec);
}

static void test_ramp_done_exactly_once(void)
{
    fa_stream* s = open_attached(g_dev->rate, 0, 0);
    recorder rec;
    fa_ramp a, b, c;
    float out[2 * 512];
    int i, seen[4] = { 0, 0, 0, 0 };
    rec_init(&rec);
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_set_event_callback(s, record, &rec), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    memset(&a, 0, sizeof(a)); a.tag = 1;
    memset(&b, 0, sizeof(b)); b.tag = 2;
    memset(&c, 0, sizeof(c)); c.tag = 3;
    CHECK_EQ(fa_stream_ramp_param(s, FA_PARAM_VOLUME, 0.0f, 1000000000, 0, &a), FA_OK);
    CHECK_EQ(fa_stream_ramp_param(s, FA_PARAM_VOLUME, 0.2f, 1000000000, 0, &b), FA_OK); /* replaces a before it started */
    render(s, out, 512);                                 /* b starts */
    CHECK_EQ(fa_stream_ramp_param(s, FA_PARAM_VOLUME, 1.0f, 0, 0, &c), FA_OK);        /* replaces running b */
    render(s, out, 512);                                 /* c (zero length) finishes */
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_VOLUME, 0.7f), FA_OK);                   /* no ramp running: no event */
    render(s, out, 512);
    CHECK_EQ(count_events(&rec, FA_EVENT_RAMP_DONE, 3), 3);
    fa_sleep_ms(30);
    CHECK_EQ(rec.count, 3);
    for (i = 0; i < 3; i++) if (rec.ev[i].value >= 1 && rec.ev[i].value <= 3) seen[rec.ev[i].value]++;
    CHECK(seen[1] == 1 && seen[2] == 1 && seen[3] == 1);
    fa_stream_close(s);
    rec_free(&rec);
}

static void test_set_then_ramp_before_mixing(void)
{
    fa_stream* s = open_attached(g_dev->rate, 0, 0);
    float out[2 * 4800], v = -1.0f;
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_VOLUME, 0.0f), FA_OK);
    CHECK_EQ(fa_stream_ramp_param(s, FA_PARAM_VOLUME, 1.0f, (fa_time)(4800.0 * 1e9 / g_dev->rate), 0, NULL), FA_OK);
    CHECK_EQ(fa_stream_get_param(s, FA_PARAM_VOLUME, &v), FA_OK);
    CHECK_NEAR(v, 0.0, 1e-6);                       /* the fade has not started: still 0 */
    CHECK_EQ(fa_stream_play(s), FA_OK);
    render(s, out, 4800);
    CHECK_NEAR(out[0], 0.0, 1e-6);                  /* fades in from 0, not from 1 */
    CHECK_NEAR(out[2400 * 2], 0.25, 2e-3);          /* linear: half way = 0.5 * 0.5 */
    fa_stream_close(s);
}

static void test_ended_event_and_state(void)
{
    fa_stream* s = open_attached(3000, 0, 0);
    recorder rec;
    float out[2 * 1024];
    fa_stream_state st;
    int i;
    rec_init(&rec);
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_set_event_callback(s, record, &rec), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    for (i = 0; i < 4; i++) render(s, out, 1024);
    CHECK_EQ(fa_stream_get_state(s, &st), FA_OK);
    CHECK_EQ(st, FA_STATE_ENDED);
    CHECK_NEAR(out[2 * 1023], 0.0, 1e-9);           /* silence after the end */
    CHECK_EQ(count_events(&rec, FA_EVENT_ENDED, 1), 1);
    CHECK_EQ(rec.ev[0].frame, 3000);
    CHECK_EQ(fa_stream_play(s), FA_OK);              /* play after the end restarts */
    render(s, out, 16);
    CHECK_NEAR(out[0], 0.5, 1e-6);
    fa_stream_close(s);
    rec_free(&rec);
}

static void test_loop_is_seamless(void)
{
    uint32_t len = 5000, i, done = 0;
    fa_stream* s = open_attached(len, 1, FA_OPEN_LOOP);
    float out[2 * 1000];
    fa_frames pos;
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    while (done < 3 * len) {
        render(s, out, 1000);
        for (i = 0; i < 1000; i++) CHECK(out[i * 2] == expected(1, (done + i) % len));
        done += 1000;
    }
    CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
    CHECK_EQ(pos, (3 * len) % len);
    fa_stream_close(s);
}

static void FA_CALL close_from_callback(const fa_event* ev, void* user)
{
    (void)user;
    if (ev->type == FA_EVENT_ENDED) fa_stream_close(ev->stream);
}

static void test_close_inside_callback(void)
{
    fa_stream* s = open_attached(500, 0, 0);
    float out[2 * 1024];
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_set_event_callback(s, close_from_callback, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    render(s, out, 1024);                            /* ends -> the callback closes it */
    fa_sleep_ms(50);                                 /* no crash, no double free (ASan) */
}

static void test_zero_length_ramp_jumps(void)
{
    fa_stream* s = open_attached(g_dev->rate, 0, 0);
    recorder rec;
    fa_ramp r;
    float out[2 * 64];
    rec_init(&rec);
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_set_event_callback(s, record, &rec), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    render(s, out, 64);
    CHECK_NEAR(out[0], 0.5, 1e-6);
    memset(&r, 0, sizeof(r)); r.tag = 7;
    CHECK_EQ(fa_stream_ramp_param(s, FA_PARAM_VOLUME, 0.0f, 0, 0, &r), FA_OK);
    render(s, out, 64);
    CHECK_NEAR(out[0], 0.0, 1e-9);                   /* the very first sample is silent */
    CHECK_EQ(count_events(&rec, FA_EVENT_RAMP_DONE, 1), 1);
    CHECK_EQ(rec.ev[0].frame, 64);                   /* finished before this callback mixed */
    fa_stream_close(s);
    rec_free(&rec);
}

static fa_atomic32 g_hold;

/* Records like `record`, but a STALLED event parks the event thread until released. */
static void FA_CALL record_and_hold(const fa_event* ev, void* user)
{
    record(ev, user);
    if (ev->type == FA_EVENT_STALLED) while (fa_load32(&g_hold)) fa_sleep_ms(1);
}

static void test_events_arrive_in_order(void)
{
    fa_stream* s = open_attached(g_dev->rate, 0, 0);
    recorder rec;
    fa_ramp a, b;
    fa_event park;
    float out[2 * 512];
    rec_init(&rec);
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_set_event_callback(s, record_and_hold, &rec), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    /* Park the event thread so the next two events wait in different queues. */
    fa_store32(&g_hold, 1);
    memset(&park, 0, sizeof(park));
    park.type = FA_EVENT_STALLED;
    park.stream = s;
    fa_engine_post_event(g_engine, &park);
    CHECK_EQ(count_events(&rec, FA_EVENT_STALLED, 1), 1);
    memset(&a, 0, sizeof(a)); a.tag = 1;
    memset(&b, 0, sizeof(b)); b.tag = 2;
    CHECK_EQ(fa_stream_ramp_param(s, FA_PARAM_VOLUME, 0.5f, 0, 0, &a), FA_OK);
    render(s, out, 512);                             /* a ends: audio thread -> device queue */
    CHECK_EQ(fa_stream_ramp_param(s, FA_PARAM_VOLUME, 0.2f, 1000000000, 0, &b), FA_OK);
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_VOLUME, 1.0f), FA_OK); /* replaces unstarted b: API thread posts */
    fa_store32(&g_hold, 0);
    CHECK_EQ(count_events(&rec, FA_EVENT_RAMP_DONE, 2), 2);
    CHECK_EQ(rec.ev[1].value, 1);
    CHECK_EQ(rec.ev[2].value, 2);
    fa_stream_close(s);
    rec_free(&rec);
}

static void test_full_queue_keeps_ramp_done(void)
{
    fa_stream* s = open_attached(g_dev->rate, 0, 0);
    recorder rec;
    fa_ramp r;
    fa_qevent filler;
    float out[2 * 64];
    int i;
    rec_init(&rec);
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_set_event_callback(s, record, &rec), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    memset(&r, 0, sizeof(r)); r.tag = 99;
    CHECK_EQ(fa_stream_ramp_param(s, FA_PARAM_VOLUME, 0.0f, 0, 0, &r), FA_OK);
    memset(&filler, 0, sizeof(filler));
    filler.ev.type = FA_EVENT_STALLED;
    filler.ev.stream = s;
    fa_mutex_lock(&g_engine->list_lock);             /* the event thread cannot drain meanwhile */
    for (i = 0; i < FA_EVENT_QUEUE_SIZE; i++) {
        filler.seq = fa_fetch_add32(&g_engine->event_seq, 1);
        CHECK(fa_event_queue_push(&g_dev->events, &filler));
    }
    render(s, out, 64);                              /* RAMP_DONE finds the queue full */
    fa_mutex_unlock(&g_engine->list_lock);
    CHECK_EQ(count_events(&rec, FA_EVENT_RAMP_DONE, 1), 1);
    CHECK_EQ(count_events(&rec, FA_EVENT_STALLED, FA_EVENT_QUEUE_SIZE), FA_EVENT_QUEUE_SIZE);
    fa_stream_close(s);
    rec_free(&rec);
}

static void test_ramp_keeps_time_across_rates(void)
{
    fa_stream* s = open_attached(g_dev->rate, 0, 0);
    recorder rec;
    float out[2 * 4800];
    rec_init(&rec);
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_set_event_callback(s, record, &rec), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    CHECK_EQ(fa_stream_ramp_param(s, FA_PARAM_VOLUME, 0.0f, (fa_time)(4800.0 * 1e9 / g_dev->rate), 0, NULL), FA_OK);
    render(s, out, 2400);                            /* half way */
    CHECK_EQ(fa_stream_detach(s), FA_OK);
    s->voice_rate = g_dev->rate * 2;                 /* pretend it ran on a device twice as fast */
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(s->lanes[FA_PARAM_VOLUME].v.total, 2400); /* same remaining time at this rate */
    CHECK_EQ(s->lanes[FA_PARAM_VOLUME].v.done, 1200);
    fa_stream_close(s);
    rec_free(&rec);
}

static void test_attach_rejects_other_engine(void)
{
    fa_engine_config cfg;
    fa_engine* other;
    fa_device* dev;
    fa_stream* s = open_attached(1000, 0, 0);
    CHECK(s != NULL);
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    CHECK_EQ(fa_engine_create(&cfg, &other), FA_OK);
    CHECK_EQ(fa_device_open(other, NULL, &dev), FA_OK);
    CHECK_EQ(fa_stream_attach(s, dev, NULL), FA_ERROR_INVALID_ARG);
    CHECK(s->device == g_dev);                       /* left where it was */
    fa_stream_close(s);
    fa_device_close(dev);
    fa_engine_destroy(other);
}

static fa_atomic32 g_destroyed;

static void FA_CALL destroy_from_callback(const fa_event* ev, void* user)
{
    if (ev->type != FA_EVENT_ENDED) return;
    fa_stream_close(ev->stream);
    fa_engine_destroy((fa_engine*)user);
    fa_store32(&g_destroyed, 1);
}

static void test_destroy_engine_inside_callback(void)
{
    fa_engine_config cfg;
    fa_engine* e;
    fa_device* dev;
    fa_stream* s = NULL;
    fa_open_options o;
    size_t n;
    uint8_t* wav;
    float out[2 * 1024];
    int i;
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    CHECK_EQ(fa_engine_create(&cfg, &e), FA_OK);
    CHECK_EQ(fa_device_open(e, NULL, &dev), FA_OK);
    wav = make_wav(dev->rate, 300, 0, &n);
    fa_open_options_init(&o);
    CHECK_EQ(fa_stream_open_memory(e, wav, n, &o, &s), FA_OK);
    free(wav);
    CHECK_EQ(fa_stream_attach(s, dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_set_event_callback(s, destroy_from_callback, e), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    for (i = 0; i < 2000 && !fa_load32(&s->eof); i++) fa_sleep_ms(1);
    memset(out, 0, sizeof(out));
    fa_fetch_add64(&dev->epoch, 1);
    fa_stream_mix(s, dev, out, 1024);                /* ends -> callback closes and destroys */
    fa_fetch_add64(&dev->epoch, 1);
    for (i = 0; i < 400 && !fa_load32(&g_destroyed); i++) fa_sleep_ms(5);
    CHECK(fa_load32(&g_destroyed));                  /* returned, no deadlock; ASan checks the rest */
    fa_sleep_ms(50);
}

static void test_move_keeps_playing(void)
{
    fa_stream* s = open_attached(g_dev->rate, 1, 0);
    fa_device* other = NULL;
    fa_stream_state st;
    float out[2 * 256];
    CHECK(s != NULL);
    CHECK_EQ(fa_device_open(g_engine, NULL, &other), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    render(s, out, 256);
    CHECK_EQ(fa_stream_attach(s, other, NULL), FA_OK);   /* move while playing */
    CHECK_EQ(fa_stream_get_state(s, &st), FA_OK);
    CHECK_EQ(st, FA_STATE_PLAYING);
    CHECK_EQ(fa_stream_detach(s), FA_OK);                /* an explicit detach still pauses */
    CHECK_EQ(fa_stream_get_state(s, &st), FA_OK);
    CHECK_EQ(st, FA_STATE_PAUSED);
    fa_stream_close(s);
    fa_device_close(other);
}

/* At the end of a resampled stream the converter's last frames still come out. */
static void test_resampled_end_is_complete(void)
{
    uint32_t src_rate = g_dev->rate / 3, frames = 1600, total = 0, i, guard;
    size_t n;
    uint8_t* wav = make_wav(src_rate, frames, 0, &n);
    fa_stream* s = NULL;
    fa_stream_state st = FA_STATE_PLAYING;
    float out[2 * 512];
    CHECK_EQ(fa_stream_open_memory(g_engine, wav, n, NULL, &s), FA_OK);
    free(wav);
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    for (guard = 0; guard < 100 && st == FA_STATE_PLAYING; guard++) {
        render(s, out, 512);
        for (i = 0; i < 512; i++) if (out[i * 2] > 0.25f) total++;
        fa_stream_get_state(s, &st);
    }
    CHECK_EQ(st, FA_STATE_ENDED);
    CHECK(total >= frames * 3 - 1);                  /* every input frame came out, x3 */
    fa_stream_close(s);
}

typedef struct fwd_io { const uint8_t* p; size_t n, pos; } fwd_io;
static int64_t FA_CALL fwd_read(void* u, void* dst, size_t bytes)
{
    fwd_io* m = (fwd_io*)u;
    size_t k = m->n - m->pos;
    if (k > bytes) k = bytes;
    memcpy(dst, m->p + m->pos, k);
    m->pos += k;
    return (int64_t)k;
}

/* A forward-only stream can't seek back, so detaching keeps the audio it decoded
 * ahead and plays it first after the next attach. The second attach uses a smaller
 * buffer, so part of the kept audio is still waiting when it is detached again. */
static void test_forward_only_detach_position(void)
{
    size_t n;
    uint8_t* wav = make_wav(g_dev->rate, g_dev->rate * 2, 1, &n);
    fwd_io io_state = { 0 };
    fa_io_callbacks io = { fwd_read, NULL, NULL, NULL };
    fa_stream* s = NULL;
    fa_frames pos = 0, waiting;
    float out[2 * 256];
    uint32_t i, heard = 0, round, default_ms;
    io_state.p = wav; io_state.n = n;
    CHECK_EQ(fa_stream_open_io(g_engine, &io, &io_state, NULL, &s), FA_OK);
    default_ms = s->buffer_ms;
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    for (round = 0; round < 3; round++) {
        uint32_t frames = round == 1 ? 100 : 256;
        render(s, out, frames);
        for (i = 0; i < frames; i++) CHECK(out[i * 2] == expected(1, heard + i));
        heard += frames;
        fa_sleep_ms(50);                              /* decoded well past what was heard */
        if (round == 1) {
            fa_mutex_lock(&s->decode_lock);
            waiting = s->carry_count;                 /* kept audio not yet back in the ring */
            fa_mutex_unlock(&s->decode_lock);
            CHECK(waiting > 0);
        }
        CHECK_EQ(fa_stream_detach(s), FA_OK);
        CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
        CHECK_EQ(pos, heard);                         /* where it was heard up to */
        s->buffer_ms = round == 0 ? 50 : default_ms;
        CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
        CHECK_EQ(fa_stream_play(s), FA_OK);
    }
    render(s, out, 256);
    for (i = 0; i < 256; i++) CHECK(out[i * 2] == expected(1, heard + i));
    fa_stream_close(s);
    free(wav);
}

/* A forward-only stream's history grows when the tempo chain is made, after it
 * has wrapped: the audio kept at a detach is still the audio not yet heard. */
static void test_forward_only_history_grows_with_chain(void)
{
    size_t n;
    uint8_t* wav = make_wav(g_dev->rate, g_dev->rate * 6, 1, &n);
    fwd_io io_state = { 0 };
    fa_io_callbacks io = { fwd_read, NULL, NULL, NULL };
    fa_stream* s = NULL;
    fa_frames pos = 0, cap0, cap1 = 0, decoded;
    float out[2 * 256];
    uint32_t i, k;
    int made;
    io_state.p = wav; io_state.n = n;
    CHECK_EQ(fa_stream_open_io(g_engine, &io, &io_state, NULL, &s), FA_OK);
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    for (k = 0; k < g_dev->rate / 256; k++) render(s, out, 256);   /* 1 s */
    fa_mutex_lock(&s->decode_lock);
    cap0 = s->hist_cap;
    decoded = s->decoded;
    fa_mutex_unlock(&s->decode_lock);
    CHECK(decoded > cap0);                            /* the history wrapped */
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_TEMPO, 1.25f), FA_OK);
    for (k = 0, made = 0; k < 400 && !made; k++) {  /* until a worker made the chain */
        render(s, out, 256);
        fa_mutex_lock(&s->decode_lock);
        made = s->tempo != NULL;
        cap1 = s->hist_cap;
        fa_mutex_unlock(&s->decode_lock);
    }
    CHECK(made);
    CHECK_EQ(cap1, cap0 + g_dev->rate / 2);           /* what the chain holds */
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_TEMPO, 1.0f), FA_OK);
    for (k = 0; k < 20; k++) render(s, out, 256);
    fa_sleep_ms(50);
    CHECK_EQ(fa_stream_detach(s), FA_OK);
    CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);   /* no chain now: plays unchanged */
    CHECK_EQ(fa_stream_play(s), FA_OK);
    render(s, out, 256);
    for (i = 0; i < 256; i++) CHECK(out[i * 2] == expected(1, (uint32_t)pos + i));
    fa_stream_close(s);
    free(wav);
}

/* Plays a forward-only stream at another rate than the device's to the end, 16
 * frames per callback, optionally detaching and reattaching after `detach_at`.
 * Returns how many frames it produced. */
static int64_t forward_only_resampled_length(int64_t detach_at)
{
    size_t n;
    uint32_t rate = g_dev->rate * 3 / 4;
    uint8_t* wav = make_wav(rate, rate, 0, &n);
    fwd_io io_state = { 0 };
    fa_io_callbacks io = { fwd_read, NULL, NULL, NULL };
    fa_stream* s = NULL;
    float out[2 * 16];
    int64_t produced = 0;
    io_state.p = wav; io_state.n = n;
    if (fa_stream_open_io(g_engine, &io, &io_state, NULL, &s) != FA_OK) { free(wav); return -1; }
    fa_stream_attach(s, g_dev, NULL);
    fa_stream_play(s);
    while (fa_load32(&s->state) == FA_STATE_PLAYING && produced < (int64_t)g_dev->rate * 4) {
        if (produced == detach_at) {
            fa_sleep_ms(50);
            fa_stream_detach(s);
            fa_stream_attach(s, g_dev, NULL);
            fa_stream_play(s);
        }
        render(s, out, 16);
        produced += 16;
    }
    fa_stream_close(s);
    free(wav);
    return produced;
}

/* Across a detach at another rate, only the resampler's few frames of delay may
 * differ; before, everything decoded ahead was lost. */
static void test_forward_only_detach_resampled(void)
{
    int64_t straight = forward_only_resampled_length(-1);
    int64_t moved = forward_only_resampled_length(16 * 600);
    CHECK(straight > 0);
    CHECK_NEAR((double)moved, (double)straight, 32.0);
}

/* FA_OPEN_LOOP can't loop a forward-only source: it ends, and its position stays at
 * the end instead of wrapping to 0 (which would replay heard audio on reattach). */
static void test_forward_only_loop_flag_ends(void)
{
    size_t n;
    uint8_t* wav = make_wav(g_dev->rate, 3000, 1, &n);
    fwd_io io_state = { 0 };
    fa_io_callbacks io = { fwd_read, NULL, NULL, NULL };
    fa_open_options o;
    fa_stream* s = NULL;
    fa_frames pos = 0, waiting;
    float out[2 * 1024];
    int i;
    io_state.p = wav; io_state.n = n;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_LOOP;
    CHECK_EQ(fa_stream_open_io(g_engine, &io, &io_state, &o, &s), FA_OK);
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    for (i = 0; i < 4; i++) render(s, out, 1024);
    CHECK_EQ(fa_load32(&s->state), FA_STATE_ENDED);
    CHECK_EQ(fa_stream_detach(s), FA_OK);
    CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
    CHECK_EQ(pos, 3000);
    fa_mutex_lock(&s->decode_lock);
    waiting = s->carry_count;
    fa_mutex_unlock(&s->decode_lock);
    CHECK_EQ(waiting, 0);                             /* nothing heard comes back */
    fa_stream_close(s);
    free(wav);
}

static void test_consecutive_sets_glide_from_heard(void)
{
    fa_stream* s = open_attached(g_dev->rate, 0, 0);
    float out[2 * 64];
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    render(s, out, 64);                              /* audible at volume 1 */
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_VOLUME, 0.0f), FA_OK);
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_VOLUME, 0.5f), FA_OK); /* replaces it before mixing */
    render(s, out, 64);
    CHECK_NEAR(out[0], 0.5, 1e-3);                   /* glides from 1, no jump to silence */
    fa_stream_close(s);
}

static void test_slow_callback_loses_nothing(void)
{
    fa_stream* s = open_attached(g_dev->rate * 4, 0, FA_OPEN_LOOP);
    recorder rec;
    fa_ramp r;
    fa_event park;
    float out[2 * 16];
    int i, total = FA_EVENT_QUEUE_SIZE + 200;
    rec_init(&rec);
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_set_event_callback(s, record_and_hold, &rec), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    fa_store32(&g_hold, 1);                          /* the app's callback blocks */
    memset(&park, 0, sizeof(park));
    park.type = FA_EVENT_STALLED;
    park.stream = s;
    fa_engine_post_event(g_engine, &park);
    CHECK_EQ(count_events(&rec, FA_EVENT_STALLED, 1), 1);
    memset(&r, 0, sizeof(r));
    for (i = 0; i < total; i++) {
        r.tag = i;
        CHECK_EQ(fa_stream_ramp_param(s, FA_PARAM_VOLUME, (float)(i & 1), 0, 0, &r), FA_OK);
        render(s, out, 16);
        if (i % 100 == 99) fa_sleep_ms(10);          /* audio callbacks are spaced out in time */
    }
    fa_store32(&g_hold, 0);
    CHECK_EQ(count_events(&rec, FA_EVENT_RAMP_DONE, total), total);
    fa_stream_close(s);
    rec_free(&rec);
}

static void test_publishing_audio_event_holds_later_ones(void)
{
    fa_stream* s = open_attached(g_dev->rate, 0, 0);
    recorder rec;
    fa_qevent early;
    fa_event later;
    rec_init(&rec);
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_set_event_callback(s, record, &rec), FA_OK);
    /* An audio thread took a sequence number and was preempted before publishing. */
    fa_fetch_add32(&g_dev->publishing, 1);
    memset(&early, 0, sizeof(early));
    early.ev.type = FA_EVENT_STALLED;
    early.ev.stream = s;
    early.seq = fa_fetch_add32(&g_engine->event_seq, 1);
    memset(&later, 0, sizeof(later));
    later.type = FA_EVENT_RESUMED;
    later.stream = s;
    fa_engine_post_event(g_engine, &later);          /* a later API event */
    fa_sleep_ms(30);
    fa_mutex_lock(&rec.lock);
    CHECK_EQ(rec.count, 0);                          /* held back */
    fa_mutex_unlock(&rec.lock);
    CHECK(fa_event_queue_push(&g_dev->events, &early));
    fa_fetch_add32(&g_dev->publishing, -1);
    CHECK_EQ(count_events(&rec, FA_EVENT_RESUMED, 1), 1);
    CHECK_EQ(rec.ev[0].type, FA_EVENT_STALLED);      /* in sequence order */
    CHECK_EQ(rec.ev[1].type, FA_EVENT_RESUMED);
    fa_stream_close(s);
    rec_free(&rec);
}

/* Fills the device queue; the caller holds list_lock, so nothing collects it until
 * it lets go. Returns how many fillers it added. */
static int fill_queue(fa_stream* s)
{
    fa_qevent filler;
    int n = 0;
    memset(&filler, 0, sizeof(filler));
    filler.ev.type = FA_EVENT_STALLED;
    filler.ev.stream = s;
    for (;;) {
        filler.seq = fa_fetch_add32(&g_engine->event_seq, 1);
        if (!fa_event_queue_push(&g_dev->events, &filler)) return n;
        n++;
    }
}

/* The workers decode under list_lock's condition variable, so wait for the audio
 * before taking the lock to render it. */
static void wait_decoded(fa_stream* s, int64_t frames)
{
    int i;
    for (i = 0; i < 2000; i++) {
        if (fa_ring_readable(&s->ring) >= frames || fa_load32(&s->eof)) return;
        fa_sleep_ms(1);
    }
}

static int spill_used(fa_stream* s)
{
    int i, n = 0;
    for (i = 0; i < FA_EVENT_SPILL; i++) n += fa_load32(&s->spill_full[i]) != 0;
    return n;
}

#define DONE_TAGS 64
static int g_done_tags[DONE_TAGS];
static fa_atomic32 g_done_count;
static void FA_CALL record_ramp_tags(const fa_event* ev, void* user)
{
    if (ev->type == FA_EVENT_RAMP_DONE) {
        int i = fa_fetch_add32(&g_done_count, 1);
        if (i < DONE_TAGS) g_done_tags[i] = (int)ev->value;
    }
    record(ev, user);
}

/* The device queue stays full and nothing but the API collects: ramp after ramp
 * (some replaced before they start) and an end still lose no event, the spill
 * slots never overflow, and completions arrive in the order they happened. */
static void test_full_queue_never_drops(void)
{
    fa_stream* s = open_attached(g_dev->rate / 10, 0, 0);
    recorder rec;
    fa_ramp r;
    float out[2 * 1024];
    int i, fillers = 0, worst = 0, used, ended;
    rec_init(&rec);
    fa_store32(&g_done_count, 0);
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_set_event_callback(s, record_ramp_tags, &rec), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    memset(&r, 0, sizeof(r));
    for (i = 0; i < 40; i++) {
        r.tag = i;
        CHECK_EQ(fa_stream_ramp_param(s, i & 2 ? FA_PARAM_PAN : FA_PARAM_VOLUME, 0.5f, 0, 0, &r), FA_OK);
        /* A set replacing a ramp the mixer has not started: its RAMP_DONE comes from
         * here, after the earlier ramp's, which the mixer raised. */
        if (i % 4 == 3) CHECK_EQ(fa_stream_set_param(s, FA_PARAM_PAN, 0.0f), FA_OK);
        wait_decoded(s, 32);
        fa_mutex_lock(&g_engine->list_lock);         /* checks wait until it is released */
        fillers += fill_queue(s);
        render(s, out, 16);
        render(s, out, 16);
        used = spill_used(s);
        fa_mutex_unlock(&g_engine->list_lock);
        if (used > worst) worst = used;
    }
    for (i = 0, ended = 0; i < 50 && !ended; i++) { /* runs out with the queue full */
        wait_decoded(s, 1024);
        fa_mutex_lock(&g_engine->list_lock);
        fillers += fill_queue(s);
        render(s, out, 1024);
        ended = fa_load32(&s->state) == FA_STATE_ENDED;
        fa_mutex_unlock(&g_engine->list_lock);
    }
    CHECK(worst <= FA_EVENT_SPILL);
    CHECK(ended);
    CHECK_EQ(count_events(&rec, FA_EVENT_RAMP_DONE, 40), 40);
    CHECK_EQ(count_events(&rec, FA_EVENT_ENDED, 1), 1);
    CHECK_EQ(count_events(&rec, FA_EVENT_STALLED, fillers), fillers);
    for (i = 0; i < 40; i++) CHECK_EQ(g_done_tags[i], i);
    fa_stream_close(s);
    rec_free(&rec);
}

static void test_counters_wrap(void)
{
    fa_stream* s = open_attached(g_dev->rate, 0, 0);
    recorder rec;
    fa_ramp r;
    float out[2 * 16];
    int i;
    rec_init(&rec);
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_set_event_callback(s, record, &rec), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    fa_mutex_lock(&g_engine->list_lock);             /* queue is empty and nobody is draining */
    fa_store32(&g_dev->events.head, 0x7FFFFFFE);
    fa_store32(&g_dev->events.tail, 0x7FFFFFFE);
    fa_store32(&g_engine->event_seq, 0x7FFFFFFE);
    fa_mutex_unlock(&g_engine->list_lock);
    memset(&r, 0, sizeof(r));
    for (i = 0; i < 6; i++) {
        r.tag = i;
        CHECK_EQ(fa_stream_ramp_param(s, FA_PARAM_VOLUME, 0.5f, 0, 0, &r), FA_OK);
        render(s, out, 16);
    }
    CHECK_EQ(count_events(&rec, FA_EVENT_RAMP_DONE, 6), 6);
    for (i = 0; i < 6; i++) CHECK_EQ(rec.ev[i].value, i);
    fa_stream_close(s);
    rec_free(&rec);
}

/* A file's first bytes are kept only while the format is found: the open stream
 * lets them go, and a later look reads them again without moving the reader.
 * App callbacks keep them (they may not seek back to replay them). */
static void test_source_prefix_dropped(void)
{
    const char* path = "fa_test_source_prefix.wav";
    size_t n, k;
    uint8_t* wav = make_wav(44100, 9000, 1, &n);
    const uint8_t* head;
    uint8_t got[100];
    fa_source src;
    fa_stream* s = NULL;
    FILE* f = fopen(path, "wb");
    CHECK(f != NULL);
    CHECK_EQ(fwrite(wav, 1, n, f), n);
    fclose(f);

    CHECK_EQ(fa_source_open_path(&src, path), FA_OK);
    CHECK_EQ(fa_source_peek(&src, &head), FA_PROBE_BYTES);
    CHECK(memcmp(head, wav, FA_PROBE_BYTES) == 0);
    CHECK_EQ(fa_source_seek(&src, 5000), FA_OK);
    fa_source_drop_prefix(&src);
    CHECK_EQ(fa_source_peek_held(&src, &head), 0);
    CHECK_EQ(fa_source_peek(&src, &head), FA_PROBE_BYTES);
    CHECK(memcmp(head, wav, FA_PROBE_BYTES) == 0);
    CHECK_EQ(fa_source_peek_held(&src, &head), FA_PROBE_BYTES);
    CHECK_EQ(fa_source_read(&src, got, sizeof(got)), sizeof(got));
    CHECK(memcmp(got, wav + 5000, sizeof(got)) == 0);
    fa_source_close(&src);

    /* A memory block is looked at in place. */
    CHECK_EQ(fa_source_open_memory_ref(&src, wav, n), FA_OK);
    CHECK_EQ(fa_source_peek(&src, &head), FA_PROBE_BYTES);
    CHECK(head == wav);
    fa_source_close(&src);

    CHECK_EQ(fa_stream_open_path(g_engine, path, NULL, &s), FA_OK);
    CHECK(s->source.prefix_buf == NULL);
    CHECK_EQ(fa_source_peek_held(&s->source, &head), 0);
    for (k = 0; k < 3; k++) {
        float out[64];
        fa_frames frames = 0;
        uint32_t i;
        CHECK_EQ(fa_stream_seek(s, k * 3000), FA_OK);
        CHECK_EQ(fa_stream_read(s, out, 64, &frames), FA_OK);
        CHECK_EQ(frames, 64);
        for (i = 0; i < 64; i++) CHECK(out[i] == expected(1, (uint32_t)(k * 3000 + i)));     /* mono */
    }
    fa_stream_close(s);
    remove(path);
    free(wav);
}

/* libvorbis's inverse MDCT through FatzAudio's (third_party/XIPH.md), in place:
 * x[0..n/2) coefficients in, x[0..n) = sum_k c[k] cos(2 pi/n (j + 1/2 + n/4)(k + 1/2))
 * out, for every Vorbis block size; two transforms of a size share their tables. */
static void test_vorbis_imdct(void)
{
    int n;
    for (n = 64; n <= 8192; n *= 2) {
        int m = n / 2, j, k, t;
        float* x = (float*)malloc(sizeof(float) * (size_t)n * 2);
        double* c = (double*)malloc(sizeof(double) * (size_t)m);
        void* tr[2];
        double err = 0, pow_ = 0;
        tr[0] = fa_vorbis_imdct_create(n);
        tr[1] = fa_vorbis_imdct_create(n);
        CHECK(x && c && tr[0] && tr[1]);
        for (t = 0; t < 2; t++) {
            for (k = 0; k < m; k++) c[k] = sin(k * 0.731 + t) * (1.0 + (k % 5)) / (1.0 + k * 0.01);
            for (k = 0; k < m; k++) x[k] = (float)c[k];
            for (k = m; k < 2 * n; k++) x[k] = 12345.0f;     /* overwritten below n, left past it */
            fa_vorbis_imdct(tr[t], x);
            for (j = 0; j < n; j++) {
                double want = 0;
                for (k = 0; k < m; k++) want += c[k] * cos(3.14159265358979323846 / m * (j + 0.5 + m / 2.0) * (k + 0.5));
                err += (x[j] - want) * (x[j] - want);
                pow_ += want * want;
            }
            for (k = n; k < 2 * n; k++) CHECK(x[k] == 12345.0f);
        }
        if (10 * log10(err / pow_) > -100) {
            fprintf(stderr, "  n %d: error %.1f dB\n", n, 10 * log10(err / pow_));
            CHECK(0);
        }
        /* One set of tables per size. */
        CHECK(((fa_mdct*)tr[0])->twiddle == ((fa_mdct*)tr[1])->twiddle && ((fa_mdct*)tr[0])->work != ((fa_mdct*)tr[1])->work);
        fa_vorbis_imdct_free(tr[0]);
        fa_vorbis_imdct_free(tr[1]);
        free(x);
        free(c);
    }
}

/* Looping turned on while the decoder already reached the end (a restart at the
 * position heard), then off again after it jumped back: playback stays continuous,
 * LOOPED is raised for each jump heard, and the stream then ends at its length. */
static void test_loop_toggle_while_playing(void)
{
    uint32_t len = 5000, i, done = 0;
    fa_stream* s = open_attached(len, 1, 0);
    float out[2 * 1000];
    recorder rec;
    int32_t on = -1;
    rec_init(&rec);
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_set_event_callback(s, record, &rec), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    render(s, out, 1000);
    done = 1000;
    for (i = 0; i < 200 && !fa_load32(&s->eof); i++) fa_sleep_ms(5);
    CHECK(fa_load32(&s->eof));                                         /* decoded to the end */
    CHECK_EQ(fa_stream_set_loop(s, 1), FA_OK);
    CHECK_EQ(fa_stream_get_loop(s, &on), FA_OK);
    CHECK_EQ(on, 1);
    while (done < 2 * len + 2000) {
        render(s, out, 1000);
        for (i = 0; i < 1000; i++) CHECK(out[i * 2] == expected(1, (done + i) % len));
        done += 1000;
    }
    CHECK_EQ(count_events(&rec, FA_EVENT_LOOPED, 2), 2);
    fa_mutex_lock(&rec.lock);
    for (i = 0; i < (uint32_t)rec.count && i < 64; i++)
        if (rec.ev[i].type == FA_EVENT_LOOPED) { CHECK_EQ(rec.ev[i].frame, len); CHECK_EQ(rec.ev[i].value, 0); }
    fa_mutex_unlock(&rec.lock);
    for (i = 0; i < 200; i++) {
        int64_t head;
        fa_mutex_lock(&s->decode_lock);
        head = fa_load64(&s->seek_base) + s->decoded;
        fa_mutex_unlock(&s->decode_lock);
        if (head > (int64_t)(3 * len)) break;
        fa_sleep_ms(5);
    }
    CHECK_EQ(fa_stream_set_loop(s, 0), FA_OK);                         /* decoded into the next turn */
    while (done < 3 * len) {
        render(s, out, 1000);
        for (i = 0; i < 1000; i++) CHECK(out[i * 2] == expected(1, (done + i) % len));
        done += 1000;
    }
    render(s, out, 1000);
    for (i = 0; i < 1000; i++) CHECK(out[i * 2] == 0.0f);               /* ended at the length */
    CHECK_EQ(count_events(&rec, FA_EVENT_ENDED, 1), 1);
    CHECK_EQ(count_events(&rec, FA_EVENT_LOOPED, 2), 2);
    fa_stream_close(s);
    rec_free(&rec);
}

/* Reversed and looping (CUE_IN..CUE_OUT): the audio counts down from CUE_OUT and
 * wraps from CUE_IN back up to it without a gap. Markers are raised as the position
 * counts down past them (CUE_OUT where playback starts and each time it wraps to it),
 * and LOOPED at each wrap, with the loop start it left and the end it went to. */
static void test_reverse_markers_and_loop(void)
{
    uint32_t i, done = 0, len = 20000, in = 2000, out = 12000, turn = 12000 - 2000;
    fa_stream* s = open_attached(len, 1, FA_OPEN_LOOP);
    float buf[2 * 1000];
    recorder rec;
    int k, markers[FA_MARKER_COUNT] = { 0 }, looped = 0;
    rec_init(&rec);
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_set_marker(s, FA_MARKER_CUE_IN, in), FA_OK);
    CHECK_EQ(fa_stream_set_marker(s, FA_MARKER_CUE_OUT, out), FA_OK);
    CHECK_EQ(fa_stream_set_marker(s, FA_MARKER_MIX_POINT, 7000), FA_OK);
    CHECK_EQ(fa_stream_set_event_callback(s, record, &rec), FA_OK);
    CHECK_EQ(fa_stream_set_reverse(s, 1), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);                /* from CUE_OUT */
    while (done < 2 * turn + 3000) {
        render(s, buf, 1000);
        for (i = 0; i < 1000; i++) CHECK(buf[i * 2] == expected(1, out - 1 - (done + i) % turn));
        done += 1000;
    }
    CHECK_EQ(count_events(&rec, FA_EVENT_LOOPED, 2), 2);
    CHECK_EQ(count_events(&rec, FA_EVENT_MARKER, 7), 7);
    fa_sleep_ms(20);
    fa_mutex_lock(&rec.lock);
    CHECK_EQ(rec.count, 9);
    for (k = 0; k < rec.count; k++) {
        if (rec.ev[k].type == FA_EVENT_MARKER) markers[rec.ev[k].value]++;
        if (rec.ev[k].type == FA_EVENT_LOOPED) {
            looped++;
            CHECK_EQ(rec.ev[k].frame, in);
            CHECK_EQ(rec.ev[k].value, out);
        }
    }
    fa_mutex_unlock(&rec.lock);
    CHECK_EQ(looped, 2);
    CHECK_EQ(markers[FA_MARKER_CUE_OUT], 3);           /* the start and two wraps to it */
    CHECK_EQ(markers[FA_MARKER_MIX_POINT], 2);
    CHECK_EQ(markers[FA_MARKER_CUE_IN], 2);
    /* The first ones in the order heard: CUE_OUT, the mix point, CUE_IN, the wrap. */
    CHECK_EQ(rec.ev[0].value, FA_MARKER_CUE_OUT);
    CHECK_EQ(rec.ev[1].value, FA_MARKER_MIX_POINT);
    CHECK_EQ(rec.ev[2].value, FA_MARKER_CUE_IN);
    fa_stream_close(s);
    rec_free(&rec);
}

/* A loop shorter than a callback jumps back several times per callback: one
 * LOOPED for each jump. */
static void test_short_loop_counts_every_jump(void)
{
    fa_stream* s = open_attached(5000, 1, FA_OPEN_LOOP);
    float out[2 * 1000];
    recorder rec;
    uint32_t i;
    rec_init(&rec);
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_set_marker(s, FA_MARKER_CUE_IN, 1000), FA_OK);
    CHECK_EQ(fa_stream_set_marker(s, FA_MARKER_CUE_OUT, 1200), FA_OK);
    CHECK_EQ(fa_stream_set_event_callback(s, record, &rec), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);           /* starts at the cue-in */
    render(s, out, 1000);                         /* 1000..1200 five times */
    for (i = 0; i < 1000; i++) CHECK(out[i * 2] == expected(1, 1000 + i % 200));
    CHECK_EQ(count_events(&rec, FA_EVENT_LOOPED, 5), 5);
    fa_sleep_ms(50);
    CHECK_EQ(count_events(&rec, FA_EVENT_LOOPED, 5), 5);
    fa_stream_close(s);
    rec_free(&rec);
}

typedef struct mem_out { uint8_t* p; size_t n, cap; } mem_out;

static fa_result FA_CALL mem_write(void* user, const void* data, size_t size)
{
    mem_out* m = (mem_out*)user;
    if (m->n + size > m->cap) {
        size_t cap = (m->n + size) * 2;
        uint8_t* p = (uint8_t*)realloc(m->p, cap);
        if (!p) return FA_ERROR_OUT_OF_MEMORY;
        m->p = p;
        m->cap = cap;
    }
    memcpy(m->p + m->n, data, size);
    m->n += size;
    return FA_OK;
}

/* A looping source without a length (a FLAC written forward only, which leaves its
 * length unknown): the end is found when the decoder first reaches it, and from
 * then on the position wraps and LOOPED is raised. */
static void test_loop_without_length(void)
{
    uint32_t len = 5000, i, done = 0;
    mem_out flac = { NULL, 0, 0 };
    fa_encoder_output eo = { mem_write, NULL, NULL };
    fa_encoder_options eopt;
    fa_encoder* e = NULL;
    fa_open_options o;
    fa_stream* s = NULL;
    fa_stream_info info;
    fa_frames pos = -1;
    float out[2 * 1000], pcm[5000];
    recorder rec;
    for (i = 0; i < len; i++) pcm[i] = expected(1, i);   /* on the 16-bit grid: lossless */
    fa_encoder_options_init(&eopt);
    eopt.format = FA_ENCODE_FLAC;
    CHECK_EQ(fa_encoder_create_output(&eo, &flac, &eopt, &e), FA_OK);
    CHECK_EQ(fa_encoder_write(e, pcm, len, g_dev->rate, 1), FA_OK);
    CHECK_EQ(fa_encoder_close(e), FA_OK);
    rec_init(&rec);
    fa_open_options_init(&o);
    o.flags = FA_OPEN_LOOP;
    CHECK_EQ(fa_stream_open_memory(g_engine, flac.p, flac.n, &o, &s), FA_OK);
    if (!s) { free(flac.p); rec_free(&rec); return; }
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK_EQ(info.length_frames, -1);
    CHECK(info.seekable);
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_set_event_callback(s, record, &rec), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    while (done < 2 * len + 2000) {
        render(s, out, 1000);
        for (i = 0; i < 1000; i++) CHECK(out[i * 2] == expected(1, (done + i) % len));
        done += 1000;
    }
    CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
    CHECK_EQ(pos, (fa_frames)(done % len));
    CHECK_EQ(count_events(&rec, FA_EVENT_LOOPED, 2), 2);
    fa_mutex_lock(&rec.lock);
    for (i = 0; i < (uint32_t)rec.count && i < 64; i++)
        if (rec.ev[i].type == FA_EVENT_LOOPED) CHECK_EQ(rec.ev[i].frame, len);
    fa_mutex_unlock(&rec.lock);
    fa_stream_close(s);
    free(flac.p);
    rec_free(&rec);
}

static void test_loop_needs_seekable(void)
{
    fa_stream* s = NULL;
    fa_audio_format f;
    memset(&f, 0, sizeof(f));
    f.sample_format = FA_FORMAT_F32; f.sample_rate = 48000; f.channels = 2;
    CHECK_EQ(fa_stream_open_push(g_engine, &f, NULL, &s), FA_OK);
    CHECK_EQ(fa_stream_set_loop(s, 1), FA_ERROR_NOT_SEEKABLE);
    CHECK_EQ(fa_stream_set_loop(s, 0), FA_OK);
    fa_stream_close(s);
}

/* render() and then the device clock moves on, as after a real callback. */
static void render_on(fa_stream* s, float* out, uint32_t frames)
{
    render(s, out, frames);
    fa_fetch_add64(&g_dev->clock, frames);
}

/* Mute glides to silence in 5 ms and back, keeping the volume; muted before the
 * start, the very first sample is silent. */
static void test_mute_glides_and_keeps_volume(void)
{
    uint32_t glide = g_dev->rate / 200, i;
    fa_stream* s = open_attached(g_dev->rate, 0, 0);
    float out[2 * 4096], vol = 0.0f;
    int32_t m = -1;
    CHECK(s != NULL);
    CHECK(glide + 64 <= 4096);
    CHECK_EQ(fa_stream_set_mute(s, 1), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    render_on(s, out, 64);
    for (i = 0; i < 64; i++) CHECK(out[i * 2] == 0.0f);
    CHECK_EQ(fa_stream_set_mute(s, 0), FA_OK);
    render_on(s, out, glide + 64);
    CHECK(out[0] < 0.01f);                                   /* glides up from silence */
    CHECK(out[glide / 2 * 2] > 0.1f && out[glide / 2 * 2] < 0.4f);
    for (i = glide + 1; i < glide + 64; i++) CHECK_NEAR(out[i * 2], 0.5, 1e-6);
    CHECK_EQ(fa_stream_set_mute(s, 1), FA_OK);
    CHECK_EQ(fa_stream_get_mute(s, &m), FA_OK);
    CHECK_EQ(m, 1);
    render_on(s, out, glide + 64);
    CHECK(out[0] > 0.49f);                                   /* no jump: glides down */
    for (i = glide + 1; i < glide + 64; i++) CHECK(out[i * 2] == 0.0f);
    CHECK_EQ(fa_stream_get_param(s, FA_PARAM_VOLUME, &vol), FA_OK);
    CHECK_NEAR(vol, 1.0, 1e-9);
    fa_stream_close(s);
}

static void test_device_mute(void)
{
    uint32_t glide = g_dev->rate / 200, i;
    fa_stream* s = open_attached(g_dev->rate, 0, 0);
    float out[2 * 4096];
    int32_t m = -1;
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    render(s, out, 64);
    CHECK_EQ(fa_device_set_mute(g_dev, 1), FA_OK);
    CHECK_EQ(fa_device_get_mute(g_dev, &m), FA_OK);
    CHECK_EQ(m, 1);
    for (i = 0; i < 2000 && fa_ring_readable(&s->ring) < (int64_t)(glide + 64); i++) fa_sleep_ms(1);
    fa_fetch_add64(&g_dev->epoch, 1);
    fa_device_render(g_dev, out, glide + 64);
    fa_fetch_add64(&g_dev->epoch, 1);
    for (i = glide + 1; i < glide + 64; i++) CHECK(out[i * 2] == 0.0f && out[i * 2 + 1] == 0.0f);
    CHECK_EQ(fa_device_set_mute(g_dev, 0), FA_OK);
    for (i = 0; i < 2000 && fa_ring_readable(&s->ring) < (int64_t)(glide + 64); i++) fa_sleep_ms(1);
    fa_fetch_add64(&g_dev->epoch, 1);
    fa_device_render(g_dev, out, glide + 64);
    fa_fetch_add64(&g_dev->epoch, 1);
    CHECK(out[(glide + 63) * 2] > 0.1f);
    fa_stream_close(s);
}

typedef struct clock_probe { fa_stream* s; fa_atomic32 done; fa_atomic32 stop; fa_time t; } clock_probe;

static void clock_hammer(void* arg)
{
    clock_probe* p = (clock_probe*)arg;
    while (!fa_load32(&p->stop)) fa_stream_get_clock(p->s, &p->t);
    fa_store32(&p->done, 1);
}

/* Lock-free reads while the stream moves on and off a device (TSan/ASan). */
static void test_clock_during_attach_detach(void)
{
    fa_stream* s = open_attached(g_dev->rate, 0, 0);
    clock_probe p;
    fa_thread t;
    int i;
    CHECK(s != NULL);
    memset(&p, 0, sizeof(p));
    p.s = s;
    CHECK(fa_thread_create(&t, clock_hammer, &p) == 0);
    for (i = 0; i < 50; i++) {
        CHECK_EQ(fa_stream_detach(s), FA_OK);
        CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    }
    fa_store32(&p.stop, 1);
    fa_thread_join(&t);
    fa_stream_close(s);
}

static void clock_probe_run(void* arg)
{
    clock_probe* p = (clock_probe*)arg;
    fa_stream_get_clock(p->s, &p->t);
    fa_store32(&p->done, 1);
}

/* A render loop's clock read must not wait behind a seek, which holds the API lock
 * while it refills: this one holds it the whole time. */
static void test_clock_never_waits_for_api_lock(void)
{
    fa_stream* s = open_attached(g_dev->rate, 0, 0);
    clock_probe p;
    fa_thread t;
    int64_t deadline;
    int done;
    CHECK(s != NULL);
    memset(&p, 0, sizeof(p));
    p.s = s;
    fa_mutex_lock(&s->api_lock);
    CHECK(fa_thread_create(&t, clock_probe_run, &p) == 0);
    deadline = fa_monotonic_ns() + 2000000000LL;
    while (!fa_load32(&p.done) && fa_monotonic_ns() < deadline) fa_sleep_ms(1);
    done = fa_load32(&p.done);
    fa_mutex_unlock(&s->api_lock);
    fa_thread_join(&t);
    CHECK(done);
    fa_stream_close(s);
}

/* Peak amplitude of the left channel over `n` frames. */
static float peak_left(const float* out, uint32_t n)
{
    float p = 0.0f;
    uint32_t i;
    for (i = 0; i < n; i++) if (fabsf(out[i * 2]) > p) p = fabsf(out[i * 2]);
    return p;
}

/* Renders 200 ms (the glide settles) and returns the peak of the next 20 ms. */
static float settled_peak(fa_stream* s)
{
    float out[2 * 960];
    uint32_t i;
    for (i = 0; i < 10; i++) render_on(s, out, g_dev->rate / 50);
    render_on(s, out, g_dev->rate / 50);
    return peak_left(out, g_dev->rate / 50);
}

static void test_eq_peak_band_and_bypass(void)
{
    fa_stream* s = open_attached(g_dev->rate * 3, 2, 0);
    fa_stream* low = open_attached(g_dev->rate * 3, 3, 0);
    fa_eq_band b = { FA_EQ_PEAK, 1000.0f, 1.41f, 12.0f }, got[4];
    uint32_t n = 0, i;
    int32_t on = 0;
    float out[2 * 960], p;
    CHECK(s != NULL && low != NULL);
    CHECK(g_dev->rate / 50 <= 960);
    CHECK_EQ(fa_stream_set_eq(s, &b, 1), FA_OK);
    CHECK_EQ(fa_stream_set_eq(low, &b, 1), FA_OK);
    CHECK_EQ(fa_stream_get_eq(s, got, 4, &n), FA_OK);
    CHECK_EQ(n, 1);
    CHECK_NEAR(got[0].gain_db, 12.0, 1e-6);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    CHECK_EQ(fa_stream_play(low), FA_OK);
    p = settled_peak(s);
    CHECK_NEAR(20.0 * log10(p / 0.25), 12.0, 0.3);      /* the 1 kHz sine is boosted 12 dB */
    p = settled_peak(low);
    CHECK_NEAR(20.0 * log10(p / 0.25), 0.0, 1.0);       /* 100 Hz is 3.3 octaves away */
    CHECK_EQ(fa_stream_set_eq_gain(s, 0, -6.0f), FA_OK);
    CHECK_EQ(fa_stream_set_eq_gain(s, 1, 3.0f), FA_ERROR_INVALID_ARG);
    p = settled_peak(s);
    CHECK_NEAR(20.0 * log10(p / 0.25), -6.0, 0.3);
    /* Bypassed: glides to flat, then the output is the input, bit for bit. */
    CHECK_EQ(fa_stream_set_eq_enabled(s, 0), FA_OK);
    CHECK_EQ(fa_stream_get_eq_enabled(s, &on), FA_OK);
    CHECK_EQ(on, 0);
    settled_peak(s);
    CHECK_EQ(fa_stream_get_eq(s, got, 4, &n), FA_OK);
    CHECK_EQ(n, 1);                                       /* the bands are kept */
    {
        fa_frames pos;
        CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
        render_on(s, out, 480);
        for (i = 0; i < 480; i++) CHECK(out[i * 2] == expected(2, (uint32_t)pos + i));
    }
    fa_stream_close(s);
    fa_stream_close(low);
}

/* A gain change while playing glides: no step between samples bigger than the
 * sine itself can make (2 pi f / rate of its amplitude), plus a little. */
static void test_eq_change_is_smooth(void)
{
    fa_stream* s = open_attached(g_dev->rate * 3, 2, 0);
    fa_eq_band b = { FA_EQ_PEAK, 1000.0f, 1.41f, 0.0f };
    float out[2 * 960], prev = 0.0f, max_step = 0.0f;
    uint32_t i, k;
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_set_eq(s, &b, 1), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    render_on(s, out, 480);
    prev = out[479 * 2];
    CHECK_EQ(fa_stream_set_eq_gain(s, 0, 18.0f), FA_OK);
    for (k = 0; k < 10; k++) {
        render_on(s, out, 480);
        for (i = 0; i < 480; i++) {
            float d = fabsf(out[i * 2] - prev);
            if (d > max_step) max_step = d;
            prev = out[i * 2];
        }
    }
    /* At +18 dB the sine is 0.25 * 7.94 = 1.99; its biggest step is 2 pi 1000/rate of that. */
    CHECK(max_step < 1.99f * 6.2832f * 1000.0f / (float)g_dev->rate * 1.2f);
    fa_stream_close(s);
}

/* After a seek the filters start from rest, so no ringing of the old audio: the
 * output equals a stream that starts fresh at that position. */
static void test_eq_memory_cleared_on_seek(void)
{
    fa_stream* a = open_attached(g_dev->rate * 3, 2, 0);
    fa_stream* b = open_attached(g_dev->rate * 3, 2, 0);
    fa_eq_band band = { FA_EQ_PEAK, 1000.0f, 8.0f, 18.0f };
    float oa[2 * 480], ob[2 * 480];
    uint32_t i;
    CHECK(a != NULL && b != NULL);
    CHECK_EQ(fa_stream_set_eq(a, &band, 1), FA_OK);
    CHECK_EQ(fa_stream_set_eq(b, &band, 1), FA_OK);
    CHECK_EQ(fa_stream_play(a), FA_OK);
    for (i = 0; i < 20; i++) render_on(a, oa, 480);   /* settled, ringing at 1 kHz */
    CHECK_EQ(fa_stream_seek(a, 12345), FA_OK);
    CHECK_EQ(fa_stream_seek(b, 12345), FA_OK);
    {
        fa_stream_eq* ea = (fa_stream_eq*)fa_load_ptr(&a->eq);
        fa_stream_eq* eb = (fa_stream_eq*)fa_load_ptr(&b->eq);
        eb->st = ea->st;                               /* same glide state, filters at rest */
        for (i = 0; i < FA_EQ_MAX_BANDS; i++) memset(eb->st.band[i].z, 0, sizeof(eb->st.band[i].z));
    }
    CHECK_EQ(fa_stream_play(b), FA_OK);
    render_on(a, oa, 480);
    b->mute_next = a->mute_next - 480;                    /* b continues "its" last mix too */
    fa_store32(&b->eq_restart, 0);
    {
        int64_t c = fa_load64(&g_dev->clock);
        fa_store64(&g_dev->clock, c - 480);
        render_on(b, ob, 480);
    }
    for (i = 0; i < 480; i++) CHECK_NEAR(oa[i * 2], ob[i * 2], 1e-6);
    fa_stream_close(a);
    fa_stream_close(b);
}

static void test_eq_presets(void)
{
    fa_eq_band b[FA_EQ_PRESET_BANDS];
    uint32_t n = 0, i;
    int p;
    for (p = 0; p < FA_EQ_PRESET_COUNT; p++) {
        CHECK_EQ(fa_eq_get_preset((fa_eq_preset)p, b, &n), FA_OK);
        CHECK_EQ(n, FA_EQ_PRESET_BANDS);
        CHECK(fa_eq_preset_name((fa_eq_preset)p) != NULL);
        for (i = 0; i < n; i++) CHECK(b[i].type == FA_EQ_PEAK && b[i].gain_db >= -12.0f && b[i].gain_db <= 12.0f);
    }
    CHECK_EQ(fa_eq_get_preset(FA_EQ_PRESET_FLAT, b, &n), FA_OK);
    for (i = 0; i < n; i++) CHECK(b[i].gain_db == 0.0f);
    CHECK(strcmp(fa_eq_preset_name(FA_EQ_PRESET_ROCK), "Rock") == 0);
    CHECK_EQ(fa_eq_get_preset(FA_EQ_PRESET_COUNT, b, &n), FA_ERROR_INVALID_ARG);
    CHECK(fa_eq_preset_name(FA_EQ_PRESET_COUNT) == NULL);
}

/* A flat preset on a playing stream changes nothing: bit-exact. */
static void test_eq_flat_is_free(void)
{
    fa_stream* s = open_attached(g_dev->rate, 1, 0);
    fa_eq_band b[FA_EQ_PRESET_BANDS];
    uint32_t n = 0, i;
    float out[2 * 480];
    CHECK(s != NULL);
    CHECK_EQ(fa_eq_get_preset(FA_EQ_PRESET_FLAT, b, &n), FA_OK);
    CHECK_EQ(fa_stream_set_eq(s, b, n), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    render_on(s, out, 480);
    for (i = 0; i < 480; i++) CHECK(out[i * 2] == expected(1, i));
    CHECK(!((fa_stream_eq*)fa_load_ptr(&s->eq))->st.active);
    fa_stream_close(s);
}

/* Clearing the bands or enabling an EQ that was never set makes none. */
static void test_eq_noop_makes_none(void)
{
    fa_stream* s = open_attached(g_dev->rate, 1, 0);
    int32_t on = 0;
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_set_eq(s, NULL, 0), FA_OK);
    CHECK_EQ(fa_stream_set_eq_enabled(s, 1), FA_OK);
    CHECK(fa_load_ptr(&s->eq) == NULL);
    CHECK_EQ(fa_stream_get_eq_enabled(s, &on), FA_OK);
    CHECK_EQ(on, 1);
    CHECK_EQ(fa_stream_set_eq_enabled(s, 0), FA_OK);   /* a real change makes one */
    CHECK(fa_load_ptr(&s->eq) != NULL);
    fa_stream_close(s);
}

/* A levels tap holds a 1 s window on a device with big callbacks and Bluetooth-like
 * latency (4096-frame periods, 200 ms), made bigger at the attach to it, and stays
 * small on an ordinary device. */
static void test_levels_tap_fits_latency(void)
{
    int32_t period = fa_load32(&g_dev->period_frames);
    int64_t latency = fa_load64(&g_dev->latency_ns);
    int64_t need = (int64_t)g_dev->rate + (int64_t)g_dev->rate / 5 + 3 * 4096 + 1024;
    float peak[2], rms[2];
    fa_stream* s = open_attached(g_dev->rate, 1, 0);
    fa_tap* t;
    fa_result r;
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_get_levels(s, 1000000000, peak, rms, 2), FA_OK);
    t = (fa_tap*)fa_load_ptr(&s->tap);
    CHECK(t != NULL && t->frames <= 65536u * (g_dev->rate > 52000 ? 2u : 1u));   /* this null device */
    CHECK_EQ(fa_stream_detach(s), FA_OK);
    fa_store32(&g_dev->period_frames, 4096);
    fa_store64(&g_dev->latency_ns, 200000000);
    r = fa_stream_attach(s, g_dev, NULL);
    t = (fa_tap*)fa_load_ptr(&s->tap);
    fa_store32(&g_dev->period_frames, period);     /* restored before any check returns */
    fa_store64(&g_dev->latency_ns, latency);
    CHECK_EQ(r, FA_OK);
    CHECK((int64_t)t->frames >= need);
    fa_stream_close(s);
}

static void FA_CALL dsp_double(float* f, fa_frames n, uint32_t ch, uint32_t rate, void* user)
{
    fa_frames i;
    (void)rate; (void)user;
    for (i = 0; i < n * (fa_frames)ch; i++) f[i] *= 2.0f;
}

static void FA_CALL dsp_add(float* f, fa_frames n, uint32_t ch, uint32_t rate, void* user)
{
    fa_frames i;
    float* seen_rate = (float*)user;
    *seen_rate = (float)rate;
    for (i = 0; i < n * (fa_frames)ch; i++) f[i] += 0.125f;
}

/* Higher priority first: add (priority 5) then double (0) = (x + 0.125) * 2. */
static void test_dsp_order_and_remove(void)
{
    fa_stream* s = open_attached(g_dev->rate * 2, 0, 0);
    uint32_t id_double = 0, id_add = 0, i;
    float out[2 * 256], rate_seen = 0.0f;
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_add_dsp(s, dsp_double, NULL, 0, &id_double), FA_OK);
    CHECK_EQ(fa_stream_add_dsp(s, dsp_add, &rate_seen, 5, &id_add), FA_OK);
    CHECK(id_double != id_add);
    CHECK_EQ(fa_stream_seek(s, 0), FA_OK);              /* drop what was decoded before */
    CHECK_EQ(fa_stream_play(s), FA_OK);
    render(s, out, 256);
    for (i = 0; i < 256; i++) CHECK_NEAR(out[i * 2], (0.5 + 0.125) * 2.0, 1e-6);
    CHECK_NEAR(rate_seen, (double)g_dev->rate, 0.5);
    CHECK_EQ(fa_stream_remove_dsp(s, id_add), FA_OK);
    CHECK_EQ(fa_stream_remove_dsp(s, id_add), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_stream_remove_dsp(s, id_double), FA_OK);
    CHECK_EQ(fa_stream_seek(s, 256), FA_OK);
    render(s, out, 256);
    for (i = 0; i < 256; i++) CHECK_NEAR(out[i * 2], 0.5, 1e-6);
    CHECK_EQ(fa_stream_add_dsp(s, NULL, NULL, 0, NULL), FA_ERROR_INVALID_ARG);
    fa_stream_close(s);
}

static void test_dsp_on_decode_only(void)
{
    size_t sz;
    uint8_t* wav = make_wav(22050, 1000, 0, &sz);
    fa_open_options o;
    fa_stream* s = NULL;
    float buf[1000], rate_seen = 0.0f;
    fa_frames got = 0;
    uint32_t id = 0;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    CHECK(fa_stream_open_memory(g_engine, wav, sz, &o, &s) == FA_OK);
    free(wav);
    CHECK_EQ(fa_stream_add_dsp(s, dsp_add, &rate_seen, 0, &id), FA_OK);
    CHECK_EQ(fa_stream_read(s, buf, 1000, &got), FA_OK);
    CHECK(got > 0);
    CHECK_NEAR(buf[0], 0.5 + 0.125, 1e-6);
    CHECK_NEAR(rate_seen, 22050.0, 0.5);
    fa_stream_close(s);
}


/* ---- wake-ups ---------------------------------------------------------------- */

typedef struct { fa_wake* w; fa_atomic32 done; } wake_waiter;
static void wake_waiter_main(void* arg)
{
    wake_waiter* ww = (wake_waiter*)arg;
    fa_wake_wait_ms(ww->w, UINT32_MAX);
    fa_store32(&ww->done, 1);
}

/* Waits until the thread waiting on `w` is asleep in it (the state says so). */
static int wait_asleep(fa_wake* w)
{
    int64_t until = fa_monotonic_ns() + 5000000000LL;
    while (fa_load32(&w->state) != 2)
        if (fa_monotonic_ns() > until) return 0;
        else fa_sleep_ms(1);
    return 1;
}

/* A signal given before the wait is kept, a wait without one times out, and a
 * sleeper with no timeout wakes at once when signalled. */
static void test_wake(void)
{
    fa_wake w;
    wake_waiter ww;
    fa_thread t;
    int64_t t0, until;
    CHECK_EQ(fa_wake_init(&w), 0);
    fa_wake_signal(&w);
    t0 = fa_monotonic_ns();
    fa_wake_wait_ms(&w, 10000);
    CHECK(fa_monotonic_ns() - t0 < 2000000000LL);
    t0 = fa_monotonic_ns();
    fa_wake_wait_ms(&w, 30);
    CHECK(fa_monotonic_ns() - t0 >= 20000000LL);
    CHECK_EQ(fa_load32(&w.state), 0);

    ww.w = &w;
    fa_store32(&ww.done, 0);
    CHECK_EQ(fa_thread_create(&t, wake_waiter_main, &ww), 0);
    CHECK(wait_asleep(&w));
    fa_sleep_ms(20);
    CHECK_EQ(fa_load32(&ww.done), 0);   /* no timeout: it sleeps until signalled */
    fa_wake_signal(&w);
    until = fa_monotonic_ns() + 2000000000LL;
    while (!fa_load32(&ww.done) && fa_monotonic_ns() < until) fa_sleep_ms(1);
    if (!fa_load32(&ww.done)) fa_wake_signal(&w);   /* let the join return before failing */
    fa_thread_join(&t);
    fa_wake_destroy(&w);
    CHECK(fa_monotonic_ns() < until);
}

/* The idle event thread sleeps (up to a second) instead of polling, and an event
 * the audio thread posts wakes it at once. */
static void test_event_thread_wakes(void)
{
    fa_stream* s = open_attached(g_dev->rate, 0, 0);
    recorder rec;
    int k;
    rec_init(&rec);
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_set_event_callback(s, record, &rec), FA_OK);
    for (k = 0; k < 3; k++) {
        int64_t t0;
        int n = 0;
        CHECK(wait_asleep(&g_engine->event_wake));
        t0 = fa_monotonic_ns();
        fa_audio_post_event(g_dev, s, FA_EVENT_MARKER, 0, k);
        while (n <= k && fa_monotonic_ns() - t0 < 2000000000LL) {
            fa_mutex_lock(&rec.lock);
            n = rec.by_type[FA_EVENT_MARKER];
            fa_mutex_unlock(&rec.lock);
            if (n <= k) fa_sleep_ms(1);
        }
        CHECK_EQ(n, k + 1);
        CHECK(fa_monotonic_ns() - t0 < 500000000LL);   /* not the one-second safety net */
    }
    fa_stream_close(s);
    rec_free(&rec);
}

/* An engine whose event thread sleeps shuts down at once. */
static void test_engine_destroy_wakes(void)
{
    fa_engine_config cfg;
    fa_engine* e = NULL;
    int64_t t0;
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    cfg.worker_threads = 1;
    CHECK_EQ(fa_engine_create(&cfg, &e), FA_OK);
    if (!wait_asleep(&e->event_wake)) { fa_engine_destroy(e); CHECK(0); }
    t0 = fa_monotonic_ns();
    fa_engine_destroy(e);
    CHECK(fa_monotonic_ns() - t0 < 500000000LL);
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

    RUN(test_ring_wraps);
    RUN(test_wake);
    RUN(test_event_thread_wakes);
    RUN(test_engine_destroy_wakes);
    RUN(test_curves);
    RUN(test_mix_is_bit_exact);
    RUN(test_seek_while_attached);
    RUN(test_device_rerouted_event);
    RUN(test_device_lost_reopens_default);
    RUN(test_seek_async);
    RUN(test_seek_async_then_detach);
    RUN(test_detach_resumes_at_heard_position);
    RUN(test_volume_and_pan);
    RUN(test_ramp_shape_and_done_event);
    RUN(test_ramp_done_exactly_once);
    RUN(test_set_then_ramp_before_mixing);
    RUN(test_ended_event_and_state);
    RUN(test_loop_is_seamless);
    RUN(test_close_inside_callback);
    RUN(test_zero_length_ramp_jumps);
    RUN(test_events_arrive_in_order);
    RUN(test_full_queue_keeps_ramp_done);
    RUN(test_ramp_keeps_time_across_rates);
    RUN(test_attach_rejects_other_engine);
    RUN(test_destroy_engine_inside_callback);
    RUN(test_move_keeps_playing);
    RUN(test_resampled_end_is_complete);
    RUN(test_forward_only_detach_position);
    RUN(test_forward_only_history_grows_with_chain);
    RUN(test_forward_only_detach_resampled);
    RUN(test_forward_only_loop_flag_ends);
    RUN(test_consecutive_sets_glide_from_heard);
    RUN(test_slow_callback_loses_nothing);
    RUN(test_publishing_audio_event_holds_later_ones);
    RUN(test_counters_wrap);
    RUN(test_full_queue_never_drops);
    RUN(test_vorbis_imdct);
    RUN(test_source_prefix_dropped);
    RUN(test_loop_toggle_while_playing);
    RUN(test_short_loop_counts_every_jump);
    RUN(test_reverse_markers_and_loop);
    RUN(test_loop_without_length);
    RUN(test_loop_needs_seekable);
    RUN(test_mute_glides_and_keeps_volume);
    RUN(test_device_mute);
    RUN(test_clock_never_waits_for_api_lock);
    RUN(test_clock_during_attach_detach);
    RUN(test_eq_peak_band_and_bypass);
    RUN(test_eq_change_is_smooth);
    RUN(test_eq_memory_cleared_on_seek);
    RUN(test_eq_presets);
    RUN(test_eq_flat_is_free);
    RUN(test_eq_noop_makes_none);
    RUN(test_levels_tap_fits_latency);
    RUN(test_dsp_order_and_remove);
    RUN(test_dsp_on_decode_only);
    RUN(test_pkt_index);
    RUN(test_utf8_valid);

    fa_device_close(g_dev);
    fa_engine_destroy(g_engine);
    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    printf("PASS\n");
    return 0;
}
