/* White-box tests of scheduling, markers, crossfades, queues, buses and the
 * channel matrix. The device is opened on the null backend but never started:
 * the test plays the audio thread by calling fa_device_render, so every output
 * frame can be checked against the device clock exactly. Decode workers and the
 * event thread run for real. */
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
/* FA_TEST=name runs one test only. */
#define RUN(t) do { const char* only_ = getenv("FA_TEST"); int before_ = g_failures; \
    if (only_ && strcmp(only_, #t) != 0) break; \
    printf("%s\n", #t); t(); if (g_failures == before_) printf("  ok\n"); } while (0)

#define PERIOD 512
#define MAX_REC (1 << 17)

static fa_engine* g_engine;
static fa_device* g_dev;

/* ---- media ------------------------------------------------------------------ */

/* 16-bit mono WAV at the device rate (no resampling, so samples come out exact).
 * kind 0: constant 0.5; kind 1: a sawtooth whose value encodes the frame index;
 * kind 2: constant 0.25; kind 3: a 0.5 sine of 32 frames per period, so bin 64
 * of a 2048-point spectrum. */
static int16_t sample_of(int kind, uint32_t i)
{
    if (kind == 0) return 16384;
    if (kind == 2) return 8192;
    if (kind == 3) return (int16_t)lrint(16384.0 * sin(6.283185307179586 * (i % 32) / 32.0));
    return (int16_t)((int)(i % 4096) * 8 - 16384);
}
static float expected(int kind, uint32_t i) { return (float)sample_of(kind, i) / 32768.0f; }

/* A WAV file in memory; free() it. */
static uint8_t* make_wav(uint32_t frames, int kind, size_t* out_n)
{
    size_t data = (size_t)frames * 2, n = 44 + data;
    uint8_t* b = (uint8_t*)malloc(n);
    uint32_t i, v, rate = g_dev->rate;
    memcpy(b, "RIFF", 4); v = (uint32_t)(n - 8); memcpy(b + 4, &v, 4); memcpy(b + 8, "WAVEfmt ", 8);
    v = 16; memcpy(b + 16, &v, 4);
    b[20] = 1; b[21] = 0; b[22] = 1; b[23] = 0;
    memcpy(b + 24, &rate, 4); v = rate * 2; memcpy(b + 28, &v, 4);
    b[32] = 2; b[33] = 0; b[34] = 16; b[35] = 0;
    memcpy(b + 36, "data", 4); v = (uint32_t)data; memcpy(b + 40, &v, 4);
    for (i = 0; i < frames; i++) { int16_t sv = sample_of(kind, i); memcpy(b + 44 + (size_t)i * 2, &sv, 2); }
    *out_n = n;
    return b;
}

static fa_stream* open_wav(uint32_t frames, int kind, uint32_t flags, int attach)
{
    size_t n;
    uint8_t* b = make_wav(frames, kind, &n);
    fa_open_options o;
    fa_stream* s = NULL;
    fa_open_options_init(&o);
    o.flags = flags;
    if (fa_stream_open_memory(g_engine, b, n, &o, &s) != FA_OK) s = NULL;
    free(b);
    if (s && attach && fa_stream_attach(s, g_dev, NULL) != FA_OK) { fa_stream_close(s); s = NULL; }
    return s;
}

/* A forward-only source (no seek callback), like a pipe. */
typedef struct mem_reader { uint8_t* data; size_t n, pos; } mem_reader;

static int64_t FA_CALL mem_read(void* user, void* buffer, size_t bytes)
{
    mem_reader* m = (mem_reader*)user;
    size_t k = m->n - m->pos < bytes ? m->n - m->pos : bytes;
    memcpy(buffer, m->data + m->pos, k);
    m->pos += k;
    return (int64_t)k;
}
static int64_t FA_CALL mem_length(void* user) { return (int64_t)((mem_reader*)user)->n; }
static void FA_CALL mem_close(void* user) { free(((mem_reader*)user)->data); free(user); }

static fa_stream* open_forward_only(uint32_t frames, int kind)
{
    fa_io_callbacks io = { mem_read, NULL, mem_length, mem_close };
    mem_reader* m = (mem_reader*)calloc(1, sizeof(*m));
    fa_stream* s = NULL;
    m->data = make_wav(frames, kind, &m->n);
    if (fa_stream_open_io(g_engine, &io, m, NULL, &s) != FA_OK) { mem_close(m); return NULL; }
    if (fa_stream_attach(s, g_dev, NULL) != FA_OK) { fa_stream_close(s); return NULL; }
    return s;
}

/* ---- the audio thread --------------------------------------------------------- */

/* Everything rendered since rec_reset, by mix frame (the limiter's fixed delay removed). */
static float* g_rec;
static int64_t g_rec_frames;
static int64_t g_rec_base;   /* device clock at rec_reset */

static void rec_reset(void)
{
    g_rec_frames = 0;
    g_rec_base = fa_load64(&g_dev->clock);
    memset(g_rec, 0, (size_t)MAX_REC * 2 * sizeof(float));
}

/* Renders `frames` frames (plus the limiter delay, so all of them reach the
 * recording) in callbacks of PERIOD, after giving the workers time to fill every
 * attached stream (as they would in real time). */
static void render(int64_t frames)
{
    float out[PERIOD * 2];
    int64_t d = g_dev->limiter.delay;
    frames += d;
    while (frames > 0) {
        uint32_t n = frames < PERIOD ? (uint32_t)frames : PERIOD, i;
        fa_voice_list* list = (fa_voice_list*)fa_load_ptr(&g_dev->voices);
        int tries;
        for (tries = 0; list && tries < 2000; tries++) {
            int ready = 1;
            for (i = 0; i < list->count; i++) {
                fa_stream* s = list->items[i];
                if (fa_ring_readable(&s->ring) < (int64_t)n && !fa_load32(&s->eof) && !fa_load32(&s->flushing)) ready = 0;
            }
            if (ready) break;
            fa_sleep_ms(1);
        }
        fa_fetch_add64(&g_dev->epoch, 1);
        fa_device_render(g_dev, out, n);
        fa_fetch_add64(&g_dev->epoch, 1);
        for (i = 0; i < n; i++) {
            int64_t at = g_rec_frames + i - d;  /* the limiter delays everything by d frames */
            if (at >= 0 && at < MAX_REC) { g_rec[at * 2] = out[i * 2]; g_rec[at * 2 + 1] = out[i * 2 + 1]; }
        }
        g_rec_frames += n;
        frames -= n;
    }
}

static float L(int64_t f) { return g_rec[f * 2]; }
static float R(int64_t f) { return g_rec[f * 2 + 1]; }
static fa_time frames_to_ns(int64_t f) { return (fa_time)((double)f * 1e9 / g_dev->rate + 0.5); }

/* ---- events ----------------------------------------------------------------- */

typedef struct recorder {
    fa_mutex lock;
    int by_type[32];
    int markers[FA_MARKER_COUNT];
    fa_frames marker_frame[FA_MARKER_COUNT];
    int64_t last_value[32];
    int ready;
} recorder;

static void FA_CALL record(const fa_event* ev, void* user)
{
    recorder* r = (recorder*)user;
    fa_mutex_lock(&r->lock);
    if ((int)ev->type >= 0 && (int)ev->type < 32) { r->by_type[ev->type]++; r->last_value[ev->type] = ev->value; }
    if (ev->type == FA_EVENT_MARKER && ev->value >= 0 && ev->value < FA_MARKER_COUNT) {
        r->markers[ev->value]++;
        r->marker_frame[ev->value] = ev->frame;
    }
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

/* Waits until marker `which` was reported `wait_for` times; returns the count. */
static int count_markers(recorder* r, fa_marker which, int wait_for)
{
    int n = 0, tries;
    for (tries = 0; tries < 200; tries++) {
        fa_mutex_lock(&r->lock);
        n = r->markers[which];
        fa_mutex_unlock(&r->lock);
        if (n >= wait_for) break;
        fa_sleep_ms(5);
    }
    return n;
}

/* Recorders are static and never torn down: a test that fails and returns early
 * leaves its streams open, and their callbacks may still write to them. */
static void rec_init(recorder* r)
{
    if (!r->ready) { fa_mutex_init(&r->lock); r->ready = 1; }
    fa_mutex_lock(&r->lock);
    memset(r->by_type, 0, sizeof(r->by_type));
    memset(r->markers, 0, sizeof(r->markers));
    memset(r->marker_frame, 0, sizeof(r->marker_frame));
    memset(r->last_value, 0, sizeof(r->last_value));
    fa_mutex_unlock(&r->lock);
}
static void rec_free(recorder* r) { (void)r; }

/* ---- tests ------------------------------------------------------------------ */

static void test_device_clock(void)
{
    fa_time t0, t1;
    CHECK_EQ(fa_device_get_time(g_dev, &t0), FA_OK);
    rec_reset();
    render(g_dev->rate / 10);                 /* 100 ms */
    CHECK_EQ(fa_device_get_time(g_dev, &t1), FA_OK);
    CHECK_NEAR((double)(t1 - t0), (double)frames_to_ns(g_dev->rate / 10 + g_dev->limiter.delay), 1e3);
    CHECK_EQ(fa_device_get_time(NULL, &t0), FA_ERROR_INVALID_ARG);
}

static void test_play_at_is_sample_accurate(void)
{
    fa_stream* s = open_wav(20000, 0, 0, 1);
    static recorder rec;
    fa_stream_state st;
    fa_time now;
    int64_t start;
    rec_init(&rec);
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_set_event_callback(s, record, &rec), FA_OK);
    rec_reset();
    fa_device_get_time(g_dev, &now);
    start = 1234;                              /* frames after the recording starts */
    CHECK_EQ(fa_stream_play_at(s, now + frames_to_ns(start)), FA_OK);
    CHECK_EQ(fa_stream_get_state(s, &st), FA_OK);
    CHECK_EQ(st, FA_STATE_SCHEDULED);
    render(4096);
    CHECK_EQ(L(start - 1), 0);
    CHECK_NEAR(L(start), 0.5, 1e-6);
    CHECK_NEAR(R(start + 100), 0.5, 1e-6);
    CHECK_EQ(fa_stream_get_state(s, &st), FA_OK);
    CHECK_EQ(st, FA_STATE_PLAYING);
    CHECK_EQ(count_events(&rec, FA_EVENT_STARTED, 1), 1);
    fa_stream_close(s);
    rec_free(&rec);
}

static void test_scheduled_ramp(void)
{
    fa_stream* s = open_wav(20000, 0, 0, 1);
    fa_time now;
    int64_t at = 1000, len = 2000;
    CHECK(s != NULL);
    rec_reset();
    fa_device_get_time(g_dev, &now);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    CHECK_EQ(fa_stream_ramp_param(s, FA_PARAM_VOLUME, 0.0f, frames_to_ns(len), now + frames_to_ns(at), NULL), FA_OK);
    render(4096);
    CHECK_NEAR(L(at - 1), 0.5, 1e-6);                   /* holds until the start */
    CHECK_NEAR(L(at), 0.5, 1e-6);                       /* the ramp's first frame */
    CHECK_NEAR(L(at + len / 2), 0.25, 1e-3);            /* linear half way */
    CHECK_NEAR(L(at + len), 0.0, 1e-6);
    CHECK_EQ(fa_stream_ramp_param(s, FA_PARAM_VOLUME, 1.0f, 1000, -5, NULL), FA_ERROR_INVALID_ARG);
    fa_stream_close(s);
}

static void test_cue_in_and_cue_out(void)
{
    fa_stream* s = open_wav(20000, 1, 0, 1);
    static recorder rec;
    fa_stream_state st;
    fa_frames pos, i;
    rec_init(&rec);
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_set_event_callback(s, record, &rec), FA_OK);
    CHECK_EQ(fa_stream_set_marker(s, FA_MARKER_CUE_IN, 1000), FA_OK);
    CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
    CHECK_EQ(pos, 1000);                                /* stopped = at the cue-in */
    CHECK_EQ(fa_stream_set_marker(s, FA_MARKER_CUE_OUT, 3000), FA_OK);
    rec_reset();
    CHECK_EQ(fa_stream_play(s), FA_OK);
    render(4096);
    for (i = 0; i < 2000; i++)
        if (L(i) != expected(1, (uint32_t)(1000 + i))) { CHECK_EQ(i, -1); }
    CHECK_EQ(L(2000), 0);                               /* exactly at the cue-out */
    CHECK_EQ(fa_stream_get_state(s, &st), FA_OK);
    CHECK_EQ(st, FA_STATE_ENDED);
    CHECK_EQ(count_events(&rec, FA_EVENT_ENDED, 1), 1);
    CHECK_EQ(count_events(&rec, FA_EVENT_MARKER, 2), 2);
    CHECK_EQ(rec.markers[FA_MARKER_CUE_IN], 1);
    CHECK_EQ(rec.markers[FA_MARKER_CUE_OUT], 1);
    CHECK_EQ(rec.marker_frame[FA_MARKER_CUE_OUT], 3000);
    CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
    CHECK_EQ(pos, 3000);

    /* Play again from ended: back at the cue-in. */
    rec_reset();
    CHECK_EQ(fa_stream_play(s), FA_OK);
    render(512);
    CHECK_EQ(L(0), expected(1, 1000));
    /* Stop rewinds to the cue-in; a seek leaves "stopped". */
    CHECK_EQ(fa_stream_stop(s), FA_OK);
    CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
    CHECK_EQ(pos, 1000);
    CHECK_EQ(fa_stream_seek(s, 1500), FA_OK);
    CHECK_EQ(fa_stream_get_state(s, &st), FA_OK);
    CHECK_EQ(st, FA_STATE_PAUSED);
    fa_stream_close(s);
    rec_free(&rec);
}

static void test_loop_between_cues(void)
{
    fa_stream* s = open_wav(20000, 1, FA_OPEN_LOOP, 1);
    fa_frames i;
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_set_marker(s, FA_MARKER_CUE_IN, 500), FA_OK);
    CHECK_EQ(fa_stream_set_marker(s, FA_MARKER_CUE_OUT, 1500), FA_OK);
    rec_reset();
    CHECK_EQ(fa_stream_play(s), FA_OK);
    render(3500);
    for (i = 0; i < 3500; i++)
        if (L(i) != expected(1, (uint32_t)(500 + i % 1000))) { CHECK_EQ(i, -1); }
    fa_stream_close(s);
}

static void test_marker_fades(void)
{
    fa_stream* s = open_wav(20000, 0, 0, 1);
    fa_ramp lin;
    CHECK(s != NULL);
    memset(&lin, 0, sizeof(lin));
    CHECK_EQ(fa_stream_set_marker(s, FA_MARKER_FADE_IN_END, 2000), FA_OK);   /* equal power, from 0 */
    CHECK_EQ(fa_stream_set_marker(s, FA_MARKER_FADE_OUT_START, 16000), FA_OK);
    CHECK_EQ(fa_stream_set_fade_curve(s, FA_FADE_OUT, &lin), FA_OK);          /* linear to the end */
    rec_reset();
    CHECK_EQ(fa_stream_play(s), FA_OK);
    render(20000 + PERIOD);
    CHECK_NEAR(L(0), 0.0, 1e-6);
    CHECK_NEAR(L(1000), 0.5 * sin(0.25 * 3.14159265358979), 2e-3);
    CHECK_NEAR(L(2000), 0.5, 1e-6);
    CHECK_NEAR(L(10000), 0.5, 1e-6);
    CHECK_NEAR(L(18000), 0.25, 1e-3);
    CHECK_NEAR(L(19999), 0.0, 1e-3);
    /* Seeking into a fade plays it at the right level at once. */
    CHECK_EQ(fa_stream_seek(s, 17000), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    rec_reset();
    render(512);
    CHECK_NEAR(L(0), 0.5 * 0.75, 1e-3);
    {
        fa_ramp got;
        CHECK_EQ(fa_stream_get_fade_curve(s, FA_FADE_IN, &got), FA_OK);
        CHECK_EQ(got.curve, FA_CURVE_EQUAL_POWER);
        CHECK_EQ(fa_stream_get_fade_curve(s, FA_FADE_OUT, &got), FA_OK);
        CHECK_EQ(got.curve, FA_CURVE_LINEAR);
    }
    fa_stream_close(s);
}

static void test_crossfade_equal_power(void)
{
    fa_stream* a = open_wav(48000, 0, 0, 1);
    fa_stream* b = open_wav(48000, 0, 0, 0);       /* fa_crossfade attaches it */
    static recorder ra, rb;
    fa_stream_state st;
    fa_ramp eq;
    int64_t start = -1, len = 4000, i;
    rec_init(&ra); rec_init(&rb);
    CHECK(a != NULL && b != NULL);
    CHECK_EQ(fa_stream_set_event_callback(a, record, &ra), FA_OK);
    CHECK_EQ(fa_stream_set_event_callback(b, record, &rb), FA_OK);
    CHECK_EQ(fa_stream_set_param(a, FA_PARAM_PAN, -1.0f), FA_OK);  /* a on the left */
    CHECK_EQ(fa_stream_set_param(b, FA_PARAM_PAN, 1.0f), FA_OK);   /* b on the right */
    CHECK_EQ(fa_stream_play(a), FA_OK);
    rec_reset();
    render(1024);
    memset(&eq, 0, sizeof(eq));
    eq.curve = FA_CURVE_EQUAL_POWER;
    eq.tag = 77;
    CHECK_EQ(fa_crossfade(a, b, frames_to_ns(len), &eq), FA_OK);
    CHECK_EQ(fa_stream_get_state(b, &st), FA_OK);
    CHECK_EQ(st, FA_STATE_SCHEDULED);
    render(len + 4096);
    for (i = 0; i < g_rec_frames; i++) if (R(i) != 0.0f) { start = i; break; }
    CHECK_EQ(start, 1024 + g_dev->limiter.delay + 1);        /* the next callback (b is silent on its first frame) */
    CHECK_NEAR(L(start - 1), 0.5, 1e-6);                     /* a untouched before */
    for (i = 0; i <= len; i += 500) {
        double t = (double)i / (double)len;
        double gl = L(start + i) / 0.5, gr = R(start + i) / 0.5;
        CHECK_NEAR(gl, cos(t * 1.5707963267949), 2e-3);      /* a: shape played backwards */
        CHECK_NEAR(gr, sin(t * 1.5707963267949), 2e-3);      /* b: the shape */
        CHECK_NEAR(gl * gl + gr * gr, 1.0, 4e-3);            /* constant power */
    }
    CHECK_EQ(L(start + len + 10), 0);
    CHECK_EQ(fa_stream_get_state(a, &st), FA_OK);
    CHECK_EQ(st, FA_STATE_ENDED);
    CHECK_EQ(count_events(&ra, FA_EVENT_ENDED, 1), 1);
    CHECK_EQ(count_events(&ra, FA_EVENT_RAMP_DONE, 1), 1);
    CHECK_EQ(ra.last_value[FA_EVENT_RAMP_DONE], 77);
    CHECK_EQ(count_events(&rb, FA_EVENT_RAMP_DONE, 1), 1);
    CHECK_EQ(count_events(&rb, FA_EVENT_STARTED, 1), 1);
    {
        float v;
        CHECK_EQ(fa_stream_get_param(a, FA_PARAM_VOLUME, &v), FA_OK);
        CHECK_NEAR(v, 1.0, 0);                                /* the user's volume is untouched */
    }
    /* a plays again at full level from its start. */
    rec_reset();
    CHECK_EQ(fa_stream_play(a), FA_OK);
    render(1024);
    CHECK_NEAR(L(0), 0.5, 1e-6);
    fa_stream_close(a);
    fa_stream_close(b);
    rec_free(&ra); rec_free(&rb);
}

/* A callback that starts while fa_crossfade is posting (it holds the device's
 * xf_lock) leaves every start and command due by its first frame for the next
 * callback, which takes them together; a start later in the callback is still
 * exact. */
static void test_crossfade_posting_defers(void)
{
    fa_stream* a = open_wav(48000, 0, 0, 1);
    fa_stream* b = open_wav(48000, 0, 0, 1);
    fa_stream* c = open_wav(48000, 0, 0, 1);
    int64_t clock, d = g_dev->limiter.delay, k0 = 1024 + d, k1 = k0 + PERIOD + d;
    CHECK(a != NULL && b != NULL && c != NULL);
    CHECK_EQ(fa_stream_set_param(a, FA_PARAM_PAN, -1.0f), FA_OK);
    CHECK_EQ(fa_stream_set_param(b, FA_PARAM_PAN, 1.0f), FA_OK);
    CHECK_EQ(fa_stream_set_param(c, FA_PARAM_PAN, 1.0f), FA_OK);
    CHECK_EQ(fa_stream_play(a), FA_OK);
    rec_reset();
    render(1024);
    /* What fa_crossfade does, stopped halfway: b scheduled for the next callback
     * and a's fade posted, the lock still held. c is scheduled 100 frames later. */
    fa_mutex_lock(&g_dev->xf_lock);
    clock = fa_load64(&g_dev->clock);
    CHECK_EQ(clock, g_rec_base + k0);
    fa_store64(&b->start_frame, clock);
    fa_store32(&b->state, FA_STATE_SCHEDULED);
    CHECK_EQ(fa_stream_set_param(a, FA_PARAM_VOLUME, 0.0f), FA_OK);
    fa_store64(&c->start_frame, clock + 100);
    fa_store32(&c->state, FA_STATE_SCHEDULED);
    render(PERIOD);                                /* mix frames k0 to k1 (callbacks of PERIOD and d) */
    fa_mutex_unlock(&g_dev->xf_lock);
    CHECK_EQ(fa_stream_stop(c), FA_OK);
    render(4096);
    CHECK(R(k0 + 99) == 0.0f && R(k0 + 100) > 0.0f);  /* c on its frame */
    CHECK_NEAR(L(k1 - 1), 0.5, 1e-6);              /* a untouched */
    CHECK_NEAR(R(k1 - 1), 0.5, 1e-6);              /* c only, no b */
    CHECK_NEAR(R(k1), 0.5, 1e-6);                  /* b from the first frame of the next callback */
    CHECK_NEAR(L(k1 + 4000), 0.0, 1e-6);           /* and a's command taken then too */
    fa_stream_close(a);
    fa_stream_close(b);
    fa_stream_close(c);
}

static void test_queue_is_gapless(void)
{
    fa_stream* a = open_wav(3000, 1, 0, 1);
    fa_stream* b = open_wav(20000, 1, 0, 0);
    fa_stream* got = NULL;
    static recorder rb;
    int64_t i;
    rec_init(&rb);
    CHECK(a != NULL && b != NULL);
    CHECK_EQ(fa_stream_set_event_callback(b, record, &rb), FA_OK);
    CHECK_EQ(fa_stream_set_next(a, b, 0, NULL), FA_OK);
    CHECK_EQ(fa_stream_get_next(a, &got), FA_OK);
    CHECK(got == b);
    rec_reset();
    CHECK_EQ(fa_stream_play(a), FA_OK);
    render(8192);
    for (i = 0; i < 3000; i++) if (L(i) != expected(1, (uint32_t)i)) { CHECK_EQ(i, -1); }
    for (i = 0; i < 5000; i++) if (L(3000 + i) != expected(1, (uint32_t)i)) { CHECK_EQ(3000 + i, -1); }
    CHECK_EQ(count_events(&rb, FA_EVENT_STARTED, 1), 1);
    CHECK_EQ(fa_stream_get_next(a, &got), FA_OK);
    CHECK(got == NULL);
    fa_stream_close(a);
    fa_stream_close(b);
    rec_free(&rb);
}

static void test_queue_crossfade_and_mix_point(void)
{
    fa_stream* a = open_wav(10000, 0, 0, 1);
    fa_stream* b = open_wav(20000, 0, 0, 1);
    fa_stream* c = open_wav(20000, 0, 0, 1);
    fa_stream_state st;
    int64_t i, start = -1, xf = 2000;
    CHECK(a != NULL && b != NULL && c != NULL);
    CHECK_EQ(fa_stream_set_param(a, FA_PARAM_PAN, -1.0f), FA_OK);
    CHECK_EQ(fa_stream_set_param(b, FA_PARAM_PAN, 1.0f), FA_OK);
    CHECK_EQ(fa_stream_set_next(a, b, frames_to_ns(xf), NULL), FA_OK);
    CHECK_EQ(fa_stream_get_state(b, &st), FA_OK);
    CHECK_EQ(st, FA_STATE_SCHEDULED);
    rec_reset();
    CHECK_EQ(fa_stream_play(a), FA_OK);
    render(16384);
    for (i = 0; i < g_rec_frames; i++) if (R(i) != 0.0f) { start = i; break; }
    CHECK_EQ(start, 8000 + 1);                  /* the mix point; b's first sample has gain sin(0) = 0 */
    CHECK_NEAR(L(8000 + xf / 2) / 0.5, cos(0.25 * 3.14159265358979), 2e-3);
    CHECK_NEAR(R(8000 + xf / 2) / 0.5, sin(0.25 * 3.14159265358979), 2e-3);
    CHECK_EQ(L(10000), 0);
    CHECK_NEAR(R(12000), 0.5, 1e-6);
    CHECK_EQ(fa_stream_get_state(a, &st), FA_OK);
    CHECK_EQ(st, FA_STATE_ENDED);

    /* An explicit mix point wins; next is removed again with NULL. */
    CHECK_EQ(fa_stream_set_marker(b, FA_MARKER_MIX_POINT, 19000), FA_OK);
    CHECK_EQ(fa_stream_set_next(b, c, 0, NULL), FA_OK);
    CHECK_EQ(fa_stream_set_next(b, NULL, 0, NULL), FA_OK);
    CHECK_EQ(fa_stream_get_state(c, &st), FA_OK);
    CHECK_EQ(st, FA_STATE_STOPPED);
    /* Closing a queued stream unlinks it. */
    CHECK_EQ(fa_stream_set_next(b, c, 0, NULL), FA_OK);
    fa_stream_close(c);
    {
        fa_stream* got = (fa_stream*)1;
        CHECK_EQ(fa_stream_get_next(b, &got), FA_OK);
        CHECK(got == NULL);
    }
    render(8192);   /* b passes its mix point with nothing queued */
    fa_stream_close(a);
    fa_stream_close(b);
}

static void test_queue_rejects_playing_next(void)
{
    fa_stream* a = open_wav(10000, 0, 0, 1);
    fa_stream* b = open_wav(10000, 0, 0, 1);
    CHECK(a != NULL && b != NULL);
    CHECK_EQ(fa_stream_play(b), FA_OK);
    CHECK_EQ(fa_stream_set_next(a, b, 0, NULL), FA_ERROR_BUSY);
    CHECK_EQ(fa_stream_set_next(a, a, 0, NULL), FA_ERROR_INVALID_ARG);
    fa_stream_close(a);
    fa_stream_close(b);
}

static void test_buses(void)
{
    fa_stream* s = open_wav(48000, 0, 0, 1);
    fa_bus* bus = NULL;
    fa_time now;
    float v;
    CHECK(s != NULL);
    CHECK_EQ(fa_bus_create(g_dev, &bus), FA_OK);
    CHECK_EQ(fa_bus_set_param(bus, FA_PARAM_VOLUME, 0.5f), FA_OK);
    CHECK_EQ(fa_bus_get_param(bus, FA_PARAM_VOLUME, &v), FA_OK);
    CHECK_NEAR(v, 0.5, 0);
    CHECK_EQ(fa_stream_attach(s, g_dev, bus), FA_OK);     /* same device: only the bus changes */
    CHECK_EQ(fa_stream_play(s), FA_OK);
    rec_reset();
    render(2048);
    CHECK_NEAR(L(1500), 0.25, 1e-6);
    /* A scheduled bus ramp, and pan. */
    fa_device_get_time(g_dev, &now);
    CHECK_EQ(fa_bus_ramp_param(bus, FA_PARAM_VOLUME, 1.0f, frames_to_ns(1000), now + frames_to_ns(1000), NULL), FA_OK);
    CHECK_EQ(fa_bus_set_param(bus, FA_PARAM_PAN, 1.0f), FA_OK);
    rec_reset();
    render(4096);
    CHECK_NEAR(R(999), 0.25, 1e-6);
    CHECK_NEAR(R(1500), 0.375, 1e-3);
    CHECK_NEAR(R(2500), 0.5, 1e-6);
    CHECK_NEAR(L(2500), 0.0, 1e-6);
    /* Destroying the bus moves the stream to the master mix. */
    fa_bus_destroy(bus);
    rec_reset();
    render(1024);
    CHECK_NEAR(L(500), 0.5, 1e-6);
    CHECK_NEAR(R(500), 0.5, 1e-6);
    /* A bus of another device is rejected. */
    {
        fa_device* other = NULL;
        fa_bus* ob = NULL;
        CHECK_EQ(fa_device_open(g_engine, NULL, &other), FA_OK);
        CHECK_EQ(fa_bus_create(other, &ob), FA_OK);
        CHECK_EQ(fa_stream_attach(s, g_dev, ob), FA_ERROR_INVALID_ARG);
        fa_device_close(other);                       /* frees its buses */
    }
    fa_stream_close(s);
}

static void test_matrix(void)
{
    fa_stream* s = open_wav(48000, 0, 0, 1);
    float w[2] = { 0.25f, 1.0f };                         /* mono -> L 0.25, R 1 */
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_set_matrix(s, w, 2, 2), FA_ERROR_INVALID_ARG);   /* the source is mono */
    CHECK_EQ(fa_stream_set_matrix(s, w, 1, 2), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    rec_reset();
    render(2048);
    CHECK_NEAR(L(1000), 0.125, 1e-6);
    CHECK_NEAR(R(1000), 0.5, 1e-6);
    CHECK_EQ(fa_stream_set_matrix(s, NULL, 0, 0), FA_OK);
    rec_reset();
    render(2048);
    CHECK_NEAR(L(1000), 0.5, 1e-6);
    CHECK_NEAR(R(1000), 0.5, 1e-6);
    fa_stream_close(s);
}

static void test_point_curve(void)
{
    fa_stream* s = open_wav(48000, 0, 0, 1);
    float pts[4] = { 0.0f, 1.0f, 0.5f, 1.0f };
    fa_ramp r;
    CHECK(s != NULL);
    memset(&r, 0, sizeof(r));
    r.curve = FA_CURVE_POINTS;
    r.points = pts;
    r.point_count = 1;
    CHECK_EQ(fa_stream_ramp_param(s, FA_PARAM_VOLUME, 0.0f, 0, 0, &r), FA_ERROR_INVALID_ARG);
    r.point_count = 4;
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_VOLUME, 0.0f), FA_OK);
    CHECK_EQ(fa_stream_ramp_param(s, FA_PARAM_VOLUME, 1.0f, frames_to_ns(3000), 0, &r), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    rec_reset();
    render(4096);
    CHECK_NEAR(L(0), 0.0, 1e-6);
    CHECK_NEAR(L(1000), 0.5, 5e-3);   /* the second point (a corner: gains are exact every 16 frames) */
    CHECK_NEAR(L(2000), 0.25, 1e-3);  /* the third */
    CHECK_NEAR(L(3500), 0.5, 1e-6);
    fa_stream_close(s);
}

/* Queueing behind a stream that has already ended starts it at once, with STARTED. */
static void test_queue_after_end_starts(void)
{
    fa_stream* a = open_wav(1000, 0, 0, 1);
    fa_stream* b = open_wav(20000, 2, 0, 1);
    static recorder rb;
    fa_stream_state st;
    rec_init(&rb);
    CHECK(a != NULL && b != NULL);
    CHECK_EQ(fa_stream_set_event_callback(b, record, &rb), FA_OK);
    CHECK_EQ(fa_stream_play(a), FA_OK);
    render(2048);
    CHECK_EQ(fa_stream_get_state(a, &st), FA_OK);
    CHECK_EQ(st, FA_STATE_ENDED);
    CHECK_EQ(fa_stream_set_next(a, b, 0, NULL), FA_OK);
    rec_reset();
    render(1024);
    CHECK_NEAR(L(100), 0.25, 1e-6);
    CHECK_EQ(count_events(&rb, FA_EVENT_STARTED, 1), 1);
    fa_stream_close(a);
    fa_stream_close(b);
    rec_free(&rb);
}

/* A crossfade-out longer than what is left of the stream still completes. */
static void test_crossfade_out_ends_early(void)
{
    fa_stream* a = open_wav(3000, 0, 0, 1);
    fa_stream* b = open_wav(48000, 0, 0, 1);
    static recorder ra;
    fa_ramp r;
    rec_init(&ra);
    CHECK(a != NULL && b != NULL);
    CHECK_EQ(fa_stream_set_event_callback(a, record, &ra), FA_OK);
    CHECK_EQ(fa_stream_play(a), FA_OK);
    render(512);
    memset(&r, 0, sizeof(r));
    r.tag = 5;
    CHECK_EQ(fa_crossfade(a, b, frames_to_ns(20000), &r), FA_OK);
    render(8192);
    CHECK_EQ(count_events(&ra, FA_EVENT_ENDED, 1), 1);
    CHECK_EQ(count_events(&ra, FA_EVENT_RAMP_DONE, 1), 1);
    CHECK_EQ(ra.last_value[FA_EVENT_RAMP_DONE], 5);
    fa_stream_close(a);
    fa_stream_close(b);
    rec_free(&ra);
}

/* A cue-out moved behind the playback position ends the stream at once. */
static void test_cue_out_behind_position(void)
{
    fa_stream* s = open_wav(48000, 0, 0, 1);
    fa_stream* f = open_forward_only(48000, 0);
    fa_stream_state st;
    CHECK(s != NULL && f != NULL);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    rec_reset();
    render(4096);
    CHECK_EQ(fa_stream_set_marker(s, FA_MARKER_CUE_OUT, 1000), FA_OK);
    render(1024);
    CHECK_EQ(fa_stream_get_state(s, &st), FA_OK);
    CHECK_EQ(st, FA_STATE_ENDED);
    CHECK_EQ(L(4096 + 100), 0);
    fa_stream_close(s);

    /* Forward-only: the audio already buffered past the new cue-out is not played. */
    CHECK_EQ(fa_stream_play(f), FA_OK);
    rec_reset();
    render(1024);
    CHECK_EQ(fa_stream_set_marker(f, FA_MARKER_CUE_OUT, 2000), FA_OK);
    render(4096);
    CHECK_NEAR(L(1500), 0.5, 1e-6);
    CHECK_EQ(L(2000), 0);
    CHECK_EQ(fa_stream_get_state(f, &st), FA_OK);
    CHECK_EQ(st, FA_STATE_ENDED);
    /* A matrix change would need a seek: refused, and playback is untouched. */
    {
        float w[2] = { 1.0f, 1.0f };
        CHECK_EQ(fa_stream_set_matrix(f, w, 1, 2), FA_ERROR_NOT_SEEKABLE);
    }
    fa_stream_close(f);
}

/* Looping: CUE_OUT is reported, the fade at the loop end is not bent toward the
 * next turn, and a queue with a crossfade does not end the loop without a MIX_POINT. */
static void test_loop_markers_and_queue(void)
{
    fa_stream* s = open_wav(20000, 0, FA_OPEN_LOOP, 1);
    fa_stream* n = open_wav(20000, 2, 0, 1);
    static recorder rec;
    fa_stream_state st, nst;
    int cue_outs;
    rec_init(&rec);
    CHECK(s != NULL && n != NULL);
    CHECK_EQ(fa_stream_set_event_callback(s, record, &rec), FA_OK);
    CHECK_EQ(fa_stream_set_marker(s, FA_MARKER_CUE_OUT, 1000), FA_OK);
    CHECK_EQ(fa_stream_set_marker(s, FA_MARKER_FADE_IN_END, 200), FA_OK);
    CHECK_EQ(fa_stream_set_next(s, n, frames_to_ns(300), NULL), FA_OK);
    rec_reset();
    CHECK_EQ(fa_stream_play(s), FA_OK);
    render(3500);
    CHECK_NEAR(L(995), 0.5, 1e-6);           /* full level up to the loop end */
    CHECK_NEAR(L(999), 0.5, 1e-6);
    CHECK_NEAR(L(1000), 0.0, 1e-6);          /* the fade-in again */
    cue_outs = count_markers(&rec, FA_MARKER_CUE_OUT, 3);   /* wrapped at 1000, 2000 and 3000 */
    fa_stream_get_state(s, &st);
    fa_stream_get_state(n, &nst);
    fa_stream_close(s);
    fa_stream_close(n);
    rec_free(&rec);
    CHECK_EQ(cue_outs, 3);
    CHECK_EQ(st, FA_STATE_PLAYING);          /* no MIX_POINT: the loop goes on */
    CHECK_EQ(nst, FA_STATE_SCHEDULED);
}

/* A zero-length ramp is a jump on its exact frame, for streams and buses. */
static void test_zero_length_scheduled_ramp(void)
{
    fa_stream* s = open_wav(48000, 0, 0, 1);
    fa_bus* bus = NULL;
    fa_time now;
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    render(1024);
    rec_reset();
    fa_device_get_time(g_dev, &now);
    CHECK_EQ(fa_stream_ramp_param(s, FA_PARAM_VOLUME, 0.5f, 0, now + frames_to_ns(1003), NULL), FA_OK);
    render(2048);
    CHECK_NEAR(L(1002), 0.5, 1e-6);
    CHECK_NEAR(L(1003), 0.25, 1e-6);
    CHECK_EQ(fa_bus_create(g_dev, &bus), FA_OK);
    CHECK_EQ(fa_stream_attach(s, g_dev, bus), FA_OK);
    rec_reset();
    fa_device_get_time(g_dev, &now);
    CHECK_EQ(fa_bus_ramp_param(bus, FA_PARAM_VOLUME, 0.5f, 0, now + frames_to_ns(1003), NULL), FA_OK);
    render(2048);
    CHECK_NEAR(L(1002), 0.25, 1e-6);
    CHECK_NEAR(L(1003), 0.125, 1e-6);
    fa_bus_destroy(bus);
    fa_stream_close(s);
}

/* A mix point in the very first callback after play still crossfades, and a chain
 * of streams shorter than a callback hands over to the end of the chain. */
static void test_queue_in_first_callback(void)
{
    fa_stream* a = open_wav(1000, 0, 0, 1);
    fa_stream* b = open_wav(20000, 0, 0, 1);
    fa_stream* c = open_wav(200, 1, 0, 1);
    fa_stream* d = open_wav(200, 1, 0, 1);
    fa_stream* e = open_wav(20000, 2, 0, 1);
    int64_t i;
    CHECK(a && b && c && d && e);
    CHECK_EQ(fa_stream_set_param(a, FA_PARAM_PAN, -1.0f), FA_OK);
    CHECK_EQ(fa_stream_set_param(b, FA_PARAM_PAN, 1.0f), FA_OK);
    CHECK_EQ(fa_stream_set_next(a, b, frames_to_ns(800), NULL), FA_OK);  /* mix point at 200 */
    rec_reset();
    CHECK_EQ(fa_stream_play(a), FA_OK);
    render(2048);
    CHECK_NEAR(L(100), 0.5, 1e-6);
    CHECK_NEAR(L(600) / 0.5, cos(0.25 * 3.14159265358979), 2e-3);
    CHECK_NEAR(R(600) / 0.5, sin(0.25 * 3.14159265358979), 2e-3);
    fa_stream_close(a);
    fa_stream_close(b);

    /* e is queued behind d, which is queued behind c: all three within one callback. */
    CHECK_EQ(fa_stream_set_next(d, e, 0, NULL), FA_OK);
    CHECK_EQ(fa_stream_set_next(c, d, 0, NULL), FA_OK);
    rec_reset();
    CHECK_EQ(fa_stream_play(c), FA_OK);
    render(1024);
    for (i = 0; i < 400; i++) if (L(i) != expected(1, (uint32_t)(i % 200))) { CHECK_EQ(i, -1); }
    CHECK_NEAR(L(400), 0.25, 1e-6);
    CHECK_NEAR(L(900), 0.25, 1e-6);
    fa_stream_close(c);
    fa_stream_close(d);
    fa_stream_close(e);
}

/* Levels and spectrum: a stream's own audio (before its volume) and the device's
 * output, read where it is heard; zeros when nothing plays. */
static void test_levels_and_spectrum(void)
{
    fa_stream* s = open_wav(96000, 3, 0, 1);
    float peak[2], rms[2], bins[1024], hz = -1.0f, top = 0.0f;
    uint32_t i, at = 0;
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_get_levels(s, 50000000, peak, rms, 3), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_stream_get_spectrum(s, 1000, 0, bins, NULL), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_stream_get_spectrum(s, 2048, FA_SPECTRUM_LEFT | FA_SPECTRUM_RIGHT, bins, NULL), FA_ERROR_INVALID_ARG);
    /* Not playing: zeros (this first call also starts recording). */
    peak[0] = rms[0] = 1.0f;
    CHECK_EQ(fa_stream_get_levels(s, 50000000, peak, rms, 1), FA_OK);
    CHECK(peak[0] == 0.0f && rms[0] == 0.0f);
    CHECK_EQ(fa_stream_get_spectrum(s, 2048, 0, bins, &hz), FA_OK);
    CHECK(hz == 0.0f && bins[64] == 0.0f);
    CHECK_EQ(fa_device_get_levels(g_dev, 50000000, peak, NULL, 1), FA_OK);   /* records from here */
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_VOLUME, 0.25f), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    rec_reset();
    render(24000);
    /* The stream: its 0.5 sine whatever its volume. */
    CHECK_EQ(fa_stream_get_levels(s, 100000000, peak, rms, 2), FA_OK);
    CHECK_NEAR(peak[0], 0.5, 0.01);
    CHECK_NEAR(peak[1], 0.5, 0.01);
    CHECK_NEAR(rms[0], 0.5 / sqrt(2.0), 0.01);
    CHECK_EQ(fa_stream_get_spectrum(s, 2048, 0, bins, &hz), FA_OK);
    CHECK_NEAR(hz, g_dev->rate / 2048.0, 1e-3);
    for (i = 0; i < 1024; i++) if (bins[i] > top) { top = bins[i]; at = i; }
    CHECK_EQ(at, 64);
    CHECK_NEAR(top, 0.5, 0.01);
    CHECK(bins[10] < 0.001f && bins[300] < 0.001f);
    CHECK_EQ(fa_stream_get_spectrum(s, 2048, FA_SPECTRUM_RIGHT | FA_SPECTRUM_NO_WINDOW, bins, NULL), FA_OK);
    CHECK_NEAR(bins[64], 0.5, 0.01);
    /* The device: what is heard, after the volume. */
    CHECK_EQ(fa_device_get_levels(g_dev, 100000000, peak, NULL, 1), FA_OK);
    CHECK_NEAR(peak[0], 0.125, 0.005);
    CHECK_EQ(fa_device_get_spectrum(g_dev, 2048, FA_SPECTRUM_LEFT, bins, &hz), FA_OK);
    CHECK_NEAR(bins[64], 0.125, 0.005);
    /* Paused: zeros at once. */
    CHECK_EQ(fa_stream_pause(s), FA_OK);
    CHECK_EQ(fa_stream_get_levels(s, 50000000, peak, rms, 2), FA_OK);
    CHECK(peak[0] == 0.0f && peak[1] == 0.0f && rms[1] == 0.0f);
    /* A device with no callbacks for a while (stopped) is silent too. */
    fa_sleep_ms(300);
    CHECK_EQ(fa_device_get_levels(g_dev, 50000000, peak, rms, 1), FA_OK);
    CHECK(peak[0] == 0.0f);
    fa_stream_close(s);
    /* Decode-only streams have nothing playing. */
    s = open_wav(4800, 3, FA_OPEN_DECODE_ONLY, 0);
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_get_levels(s, 50000000, peak, rms, 1), FA_ERROR_INVALID_ARG);
    fa_stream_close(s);
}

/* Cue output: a stream previewed on a second device is not in the first one's
 * mix, and moving it to the first keeps its position. */
static void test_cue_on_second_device(void)
{
    fa_device* cue = NULL;
    fa_stream* room = open_wav(48000, 0, 0, 1);
    fa_stream* preview = open_wav(48000, 1, 0, 0);
    float out[PERIOD * 2];
    fa_frames pos = 0;
    int i, k;
    CHECK(room != NULL && preview != NULL);
    CHECK_EQ(fa_device_open(g_engine, NULL, &cue), FA_OK);
    CHECK_EQ(fa_stream_attach(preview, cue, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(room), FA_OK);
    CHECK_EQ(fa_stream_play(preview), FA_OK);
    rec_reset();
    render(2048);
    CHECK_NEAR(L(1000), 0.5, 1e-6);                  /* the room hears only its song */
    /* The cue device mixes the preview (its own callbacks; here called by hand). */
    for (k = 0; k < 2000 && fa_ring_readable(&preview->ring) < PERIOD; k++) fa_sleep_ms(1);
    fa_fetch_add64(&cue->epoch, 1);
    fa_device_render(cue, out, PERIOD);
    fa_fetch_add64(&cue->epoch, 1);
    for (i = (int)cue->limiter.delay; i < PERIOD; i++)
        if (fabsf(out[i * 2] - expected(1, (uint32_t)(i - (int)cue->limiter.delay))) > 1e-6f) { CHECK_EQ(i, -1); }
    /* On air: the preview moves to the room from where it was. */
    CHECK_EQ(fa_stream_attach(preview, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_get_position(preview, &pos), FA_OK);
    CHECK(pos > 0 && pos <= PERIOD);
    fa_stream_close(preview);
    fa_stream_close(room);
    fa_device_close(cue);
}

int main(void)
{
    fa_engine_config cfg;
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    cfg.worker_threads = 2;
    if (fa_engine_create(&cfg, &g_engine) != FA_OK) { fprintf(stderr, "engine create failed\n"); return 1; }
    if (fa_device_open(g_engine, NULL, &g_dev) != FA_OK) { fprintf(stderr, "device open failed\n"); return 1; }
    g_rec = (float*)malloc((size_t)MAX_REC * 2 * sizeof(float));
    printf("null device at %u Hz (not started: this test is the audio thread)\n", g_dev->rate);

    RUN(test_device_clock);
    RUN(test_play_at_is_sample_accurate);
    RUN(test_scheduled_ramp);
    RUN(test_cue_in_and_cue_out);
    RUN(test_loop_between_cues);
    RUN(test_marker_fades);
    RUN(test_crossfade_equal_power);
    RUN(test_crossfade_posting_defers);
    RUN(test_queue_is_gapless);
    RUN(test_queue_crossfade_and_mix_point);
    RUN(test_queue_rejects_playing_next);
    RUN(test_buses);
    RUN(test_matrix);
    RUN(test_point_curve);
    RUN(test_queue_after_end_starts);
    RUN(test_crossfade_out_ends_early);
    RUN(test_cue_out_behind_position);
    RUN(test_loop_markers_and_queue);
    RUN(test_zero_length_scheduled_ramp);
    RUN(test_queue_in_first_callback);
    RUN(test_levels_and_spectrum);
    RUN(test_cue_on_second_device);

    fa_device_close(g_dev);
    fa_engine_destroy(g_engine);
    free(g_rec);
    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    printf("PASS\n");
    return 0;
}
