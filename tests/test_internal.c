/* White-box tests of the decode-ahead pipeline and the mixer. The device is
 * opened on the null backend but never started, so this test plays the part of
 * the audio thread and calls fa_stream_mix itself: every output sample can be
 * checked exactly. Decode workers and the event thread run for real. */
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
#define RUN(t) do { int before_ = g_failures; printf("%s\n", #t); t(); if (g_failures == before_) printf("  ok\n"); } while (0)

static fa_engine* g_engine;
static fa_device* g_dev;

/* ---- media ------------------------------------------------------------------ */

/* 16-bit mono WAV. kind 0: constant 0.5; kind 1: a sawtooth whose value encodes
 * the frame index (exact in float), so continuity can be checked sample by sample. */
static int16_t sample_of(int kind, uint32_t i) { return kind == 0 ? 16384 : (int16_t)((int)(i % 4096) * 8 - 16384); }
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
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_TEMPO, 1.2f), FA_ERROR_NOT_IMPLEMENTED);
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
    CHECK_EQ(s->voice[FA_PARAM_VOLUME].total, 2400); /* same remaining time at this rate */
    CHECK_EQ(s->voice[FA_PARAM_VOLUME].done, 1200);
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

static void test_forward_only_detach_position(void)
{
    size_t n;
    uint8_t* wav = make_wav(g_dev->rate, g_dev->rate * 2, 1, &n);
    fwd_io io_state = { 0 };
    fa_io_callbacks io = { fwd_read, NULL, NULL, NULL };
    fa_stream* s = NULL;
    fa_frames pos = 0;
    float out[2 * 256];
    uint32_t i;
    io_state.p = wav; io_state.n = n;
    CHECK_EQ(fa_stream_open_io(g_engine, &io, &io_state, NULL, &s), FA_OK);
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    render(s, out, 256);
    fa_sleep_ms(30);                                  /* decoded well past what was heard */
    CHECK_EQ(fa_stream_detach(s), FA_OK);
    CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
    CHECK(pos > 256);                                 /* can't go back: it says where it really is */
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    render(s, out, 256);
    for (i = 0; i < 256; i++) CHECK(out[i * 2] == expected(1, (uint32_t)pos + i));
    fa_stream_close(s);
    free(wav);
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
    RUN(test_curves);
    RUN(test_mix_is_bit_exact);
    RUN(test_seek_while_attached);
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

    fa_device_close(g_dev);
    fa_engine_destroy(g_engine);
    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    printf("PASS\n");
    return 0;
}
