/* White-box tests of push streams, packet decoding (built-in and FFmpeg) and the
 * presentation clock. As in test_crossfade.c the device is opened on the null
 * backend but never started: the test plays the audio thread, here with
 * fa_device_render_at and a made-up timeline, so the clock can be checked to the
 * microsecond. Decode workers and the event thread run for real. */
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

#define PERIOD 480
#define MAX_REC (1 << 18)

static fa_engine* g_engine;
static fa_device* g_dev;
static int64_t g_now = 1000000000;   /* the made-up monotonic time of the next callback */

/* ---- the audio thread --------------------------------------------------------- */

static float* g_rec;
static int64_t g_rec_frames;

static void rec_reset(void)
{
    g_rec_frames = 0;
    memset(g_rec, 0, (size_t)MAX_REC * 2 * sizeof(float));
}

static fa_time frames_to_ns(int64_t f) { return (fa_time)((double)f * 1e9 / g_dev->rate + 0.5); }

/* Renders `frames` frames in callbacks of PERIOD, one PERIOD of made-up time
 * apart, after giving the workers a moment to fill streams that have data. */
static void render(int64_t frames)
{
    float out[PERIOD * 2];
    while (frames > 0) {
        uint32_t n = frames < PERIOD ? (uint32_t)frames : PERIOD, i;
        fa_voice_list* list = (fa_voice_list*)fa_load_ptr(&g_dev->voices);
        int tries;
        for (tries = 0; list && tries < 500; tries++) {
            int ready = 1;
            for (i = 0; i < list->count; i++) {
                fa_stream* s = list->items[i];
                int waiting = s->push ? fa_push_has_input(s->push) : 1;
                if (waiting && fa_ring_readable(&s->ring) < (int64_t)n && !fa_load32(&s->eof) && !fa_load32(&s->flushing)) ready = 0;
            }
            if (ready) break;
            fa_sleep_ms(1);
        }
        fa_fetch_add64(&g_dev->epoch, 1);
        fa_device_render_at(g_dev, out, n, g_now);
        fa_fetch_add64(&g_dev->epoch, 1);
        for (i = 0; i < n; i++) {
            int64_t at = g_rec_frames + i;
            if (at < MAX_REC) { g_rec[at * 2] = out[i * 2]; g_rec[at * 2 + 1] = out[i * 2 + 1]; }
        }
        g_rec_frames += n;
        g_now += frames_to_ns(n);
        frames -= n;
    }
}

/* Lets the decode workers catch up with what was just pushed (at least `frames`
 * decoded ahead), as they would long before a real device needs it. */
static void wait_decoded(fa_stream* s, int64_t frames)
{
    int tries;
    for (tries = 0; tries < 2000; tries++) {
        if (fa_ring_readable(&s->ring) >= frames || fa_load32(&s->eof)) return;
        fa_sleep_ms(1);
    }
}

static int64_t latency_ns(void)
{
    fa_time l = 0;
    fa_device_get_latency(g_dev, &l);
    return l;
}

/* The clock at a made-up time `t`. */
static int64_t clock_at(fa_stream* s, int64_t t)
{
    int64_t v;
    fa_mutex_lock(&s->api_lock);
    v = fa_stream_clock_at(s, t);
    fa_mutex_unlock(&s->api_lock);
    return v;
}

/* ---- events ----------------------------------------------------------------- */

typedef struct recorder {
    fa_mutex lock;
    int by_type[32];
} recorder;

static void FA_CALL record(const fa_event* ev, void* user)
{
    recorder* r = (recorder*)user;
    fa_mutex_lock(&r->lock);
    if ((int)ev->type >= 0 && (int)ev->type < 32) r->by_type[ev->type]++;
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

/* ---- helpers ------------------------------------------------------------------ */

/* A stereo S16 planar push stream at the device rate. */
static fa_stream* open_push_s16p(void)
{
    fa_audio_format f;
    fa_stream* s = NULL;
    memset(&f, 0, sizeof(f));
    f.sample_format = FA_FORMAT_S16_PLANAR;
    f.sample_rate = g_dev->rate;
    f.channels = 2;
    if (fa_stream_open_push(g_engine, &f, NULL, &s) != FA_OK) return NULL;
    return s;
}

/* Pushes `frames` frames: left = value, right = -value (S16 planar). */
static fa_result push_level(fa_stream* s, fa_frames frames, float value, int64_t pts)
{
    int16_t* l = (int16_t*)malloc((size_t)frames * 2);
    int16_t* r = (int16_t*)malloc((size_t)frames * 2);
    const void* planes[2];
    fa_frames i;
    fa_result res;
    for (i = 0; i < frames; i++) { l[i] = (int16_t)(value * 32768.0f); r[i] = (int16_t)(-value * 32768.0f); }
    planes[0] = l;
    planes[1] = r;
    res = fa_stream_push_pcm(s, planes, frames, pts);
    free(l);
    free(r);
    return res;
}

static uint8_t* load_file(const char* name, size_t* out_size)
{
    char path[1024];
    FILE* f;
    long n;
    uint8_t* b;
    snprintf(path, sizeof(path), "%s/%s", FA_TEST_DATA_DIR, name);
    f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (uint8_t*)malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *out_size = (size_t)n;
    return b;
}

/* Reads a decode-only stream to the end (pushing is already done). */
static float* read_all(fa_stream* s, uint32_t channels, int64_t* out_frames)
{
    int64_t cap = 1 << 16, n = 0;
    float* all = (float*)malloc((size_t)cap * channels * sizeof(float));
    for (;;) {
        fa_frames got = 0;
        fa_result r;
        if (n + 1024 > cap) { cap *= 2; all = (float*)realloc(all, (size_t)cap * channels * sizeof(float)); }
        r = fa_stream_read(s, all + (size_t)n * channels, 1024, &got);
        n += got;
        if (r != FA_OK || got == 0) break;
    }
    *out_frames = n;
    return all;
}

static double rms(const float* x, int64_t from, int64_t to, uint32_t channels)
{
    double acc = 0.0;
    int64_t i;
    for (i = from * channels; i < to * channels; i++) acc += (double)x[i] * x[i];
    return sqrt(acc / (double)((to - from) * channels));
}

/* ---- tests: push streams ----------------------------------------------------------- */

static void test_push_pcm_plays_exactly(void)
{
    fa_stream* s = open_push_s16p();
    fa_stream_info info;
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK_EQ(info.format.sample_rate, g_dev->rate);
    CHECK_EQ(info.format.channels, 2);
    CHECK_EQ(info.seekable, 0);
    CHECK_EQ(push_level(s, 4800, 0.25f, FA_PTS_NONE), FA_OK);
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    rec_reset();
    render(2400);
    /* The limiter delays the output by a fixed few frames: look past them. */
    CHECK_NEAR(g_rec[200 * 2], 0.25, 1e-6);
    CHECK_NEAR(g_rec[200 * 2 + 1], -0.25, 1e-6);
    CHECK_NEAR(g_rec[2000 * 2], 0.25, 1e-6);
    CHECK_EQ(fa_stream_seek(s, 0), FA_ERROR_NOT_SEEKABLE);
    fa_stream_close(s);
}

static void test_push_frame_converts_any_format(void)
{
    fa_stream* s = open_push_s16p();
    fa_audio_frame fr;
    double* mono = (double*)malloc(sizeof(double) * 9600);
    const void* planes[1];
    int64_t i;
    CHECK(s != NULL);
    for (i = 0; i < 9600; i++) mono[i] = 0.5;
    planes[0] = mono;
    memset(&fr, 0, sizeof(fr));
    fr.format.sample_format = FA_FORMAT_F64;    /* mono doubles at twice the rate */
    fr.format.sample_rate = g_dev->rate * 2;
    fr.format.channels = 1;
    fr.data = planes;
    fr.frames = 9600;
    fr.pts = 0;
    fr.time_base.num = 1;
    fr.time_base.den = 1000;
    CHECK_EQ(fa_stream_push_frame(s, &fr), FA_OK);
    free(mono);
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    rec_reset();
    render(2400);
    CHECK_NEAR(g_rec[1000 * 2], 0.5, 1e-3);      /* mono to both sides, resampled */
    CHECK_NEAR(g_rec[1000 * 2 + 1], 0.5, 1e-3);
    CHECK_EQ(fa_stream_push_frame(s, NULL), FA_ERROR_INVALID_ARG);
    fa_stream_close(s);
}

static void test_clock_follows_the_speaker(void)
{
    fa_stream* s = open_push_s16p();
    int64_t lat = latency_ns(), t0, t, paused_at, expect;
    int64_t base = 10000000000LL;               /* the app's first pts: 10 s */
    CHECK(s != NULL);
    CHECK(lat > 0);
    CHECK_EQ(push_level(s, g_dev->rate * 2, 0.25f, base), FA_OK);
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_NEAR((double)clock_at(s, g_now), (double)base, 1);   /* nothing heard yet */
    CHECK_EQ(fa_stream_play(s), FA_OK);
    t0 = g_now;
    rec_reset();
    render(PERIOD * 40);
    /* Between callbacks, interpolated; the latency is subtracted. */
    for (t = t0 + lat + 1000000; t < g_now; t += 3333333) {
        expect = base + (t - t0) - lat;
        CHECK_NEAR((double)clock_at(s, t), (double)expect, 2000.0);
    }
    /* Before the first audio reaches the speaker it holds at the first pts. */
    CHECK_NEAR((double)clock_at(s, t0 + lat / 2), (double)base, 1);

    /* Pause: what is in the device buffer still plays out, then the clock stops. */
    CHECK_EQ(fa_stream_pause(s), FA_OK);
    paused_at = base + (g_now - t0);             /* everything mixed so far */
    render(PERIOD * 20);
    CHECK_NEAR((double)clock_at(s, g_now), (double)paused_at, 2000.0);
    CHECK_NEAR((double)clock_at(s, g_now + 5000000000LL), (double)paused_at, 2000.0);

    /* Resume: it holds until the new audio has gone through the latency. */
    CHECK_EQ(fa_stream_play(s), FA_OK);
    t = g_now;
    render(PERIOD * 20);
    CHECK_NEAR((double)clock_at(s, t + lat / 2), (double)paused_at, 2000.0);
    CHECK_NEAR((double)clock_at(s, t + lat + 2000000), (double)(paused_at + 2000000), 2000.0);
    fa_stream_close(s);
}

static void test_clock_follows_timestamp_jumps(void)
{
    fa_stream* s = open_push_s16p();
    int64_t lat = latency_ns(), t0, half = g_dev->rate / 2;
    CHECK(s != NULL);
    CHECK_EQ(push_level(s, half, 0.25f, 0), FA_OK);
    CHECK_EQ(push_level(s, half, 0.25f, 5000000000LL), FA_OK);              /* a jump of 4.5 s */
    CHECK_EQ(push_level(s, half, 0.25f, 5000000000LL + 500000000 + 3000000), FA_OK); /* 3 ms of jitter: ignored */
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    t0 = g_now;
    rec_reset();
    render(half * 3);
    CHECK_NEAR((double)clock_at(s, t0 + lat + 250000000), 250000000.0, 2000.0);
    CHECK_NEAR((double)clock_at(s, t0 + lat + 750000000), 5250000000.0, 2000.0);
    CHECK_NEAR((double)clock_at(s, t0 + lat + 1250000000), 5750000000.0, 2000.0);
    fa_stream_close(s);
}

/* Detaching keeps the audio decoded ahead (a push stream cannot go back); after
 * attaching again the clock continues exactly where playback was. */
static void test_clock_across_detach(void)
{
    fa_stream* s = open_push_s16p();
    int64_t lat = latency_ns(), base = 10000000000LL, t1, heard_ns;
    fa_frames pos = 0;
    fa_time c = 0;
    CHECK(s != NULL);
    CHECK_EQ(push_level(s, g_dev->rate * 2, 0.25f, base), FA_OK);
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    wait_decoded(s, PERIOD * 4);
    render(PERIOD * 50);
    CHECK_EQ(fa_stream_detach(s), FA_OK);
    CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
    CHECK(pos > 0);
    heard_ns = base + (int64_t)((double)pos * 1e9 / g_dev->rate + 0.5);
    CHECK_EQ(fa_stream_get_clock(s, &c), FA_OK);
    CHECK_NEAR((double)c, (double)heard_ns, 2000.0);   /* detached: what was heard, not what was decoded */
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_NEAR((double)clock_at(s, g_now), (double)heard_ns, 2000.0);   /* holds there */
    CHECK_EQ(fa_stream_play(s), FA_OK);
    wait_decoded(s, PERIOD * 4);
    t1 = g_now;
    render(PERIOD * 50);
    CHECK_NEAR((double)clock_at(s, t1 + lat + 100000000), (double)(heard_ns + 100000000), 2000.0);
    CHECK_NEAR((double)clock_at(s, t1 + lat + 300000000), (double)(heard_ns + 300000000), 2000.0);
    fa_stream_close(s);
}

/* Detaching while the decoded-ahead audio holds a timestamp jump: after attaching
 * again, the jump is still heard where it was. */
static void test_clock_across_detach_with_jump(void)
{
    fa_stream* s = open_push_s16p();
    int64_t lat = latency_ns(), t1, heard_ns, half = g_dev->rate / 4;   /* 250 ms pieces */
    fa_frames pos = 0;
    fa_time c = 0;
    CHECK(s != NULL);
    CHECK_EQ(push_level(s, half, 0.25f, 0), FA_OK);
    CHECK_EQ(push_level(s, half, 0.25f, 10000000000LL), FA_OK);          /* jump to 10 s */
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    wait_decoded(s, half * 2 - PERIOD);
    render(PERIOD * 10);                                                 /* 100 ms: before the jump */
    CHECK_EQ(fa_stream_detach(s), FA_OK);
    CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
    CHECK(pos > 0 && pos < half);
    heard_ns = (int64_t)((double)pos * 1e9 / g_dev->rate + 0.5);
    CHECK_EQ(fa_stream_get_clock(s, &c), FA_OK);
    CHECK_NEAR((double)c, (double)heard_ns, 2000.0);   /* not 10 s: the jump was not heard yet */
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_NEAR((double)clock_at(s, g_now), (double)heard_ns, 2000.0);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    wait_decoded(s, PERIOD * 4);
    t1 = g_now;
    render(half * 2 - pos);
    CHECK_NEAR((double)clock_at(s, t1 + lat + 50000000), (double)(heard_ns + 50000000), 2000.0);
    /* 250 ms - heard after attaching, the 10 s piece is heard. */
    CHECK_NEAR((double)clock_at(s, t1 + lat + (250000000 - heard_ns) + 20000000), 10020000000.0, 2000.0);
    fa_stream_close(s);
}

/* A cue-out set while detached, before the audio kept from the detach: the stream
 * just ends on attach. */
static void test_cue_out_before_kept_audio(void)
{
    fa_stream* s = open_push_s16p();
    fa_stream_state st;
    fa_frames pos = 0;
    CHECK(s != NULL);
    CHECK_EQ(push_level(s, g_dev->rate, 0.25f, 0), FA_OK);
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    wait_decoded(s, PERIOD * 8);
    render(PERIOD * 10);
    CHECK_EQ(fa_stream_detach(s), FA_OK);
    CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
    CHECK(pos > PERIOD);
    CHECK_EQ(fa_stream_set_marker(s, FA_MARKER_CUE_OUT, PERIOD), FA_OK);
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    render(PERIOD * 20);
    CHECK_EQ(fa_stream_get_state(s, &st), FA_OK);
    CHECK_EQ(st, FA_STATE_ENDED);
    fa_stream_close(s);
}

/* Flush drops the audio kept by a detach too, and the clock holds at the last time
 * heard until audio of the new position is heard. */
static void test_flush_after_detach_and_hold(void)
{
    fa_stream* s = open_push_s16p();
    int64_t lat = latency_ns(), t1, held;
    int64_t i, wrong = 0;
    CHECK(s != NULL);
    CHECK_EQ(push_level(s, g_dev->rate, 0.25f, 1000000000LL), FA_OK);
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    wait_decoded(s, PERIOD * 4);
    render(PERIOD * 10);
    CHECK_EQ(fa_stream_pause(s), FA_OK);
    render(PERIOD * 10);                             /* the device buffer plays out */
    held = clock_at(s, g_now);
    CHECK_NEAR((double)held, 1000000000.0 + 100000000.0, 2000.0);
    /* Paused: the app seeks to 30 s. The new audio decodes at once, but the clock
     * holds until it is heard. */
    CHECK_EQ(fa_stream_push_flush(s), FA_OK);
    CHECK_EQ(push_level(s, g_dev->rate / 2, 0.5f, 30000000000LL), FA_OK);
    wait_decoded(s, PERIOD * 4);
    CHECK_NEAR((double)clock_at(s, g_now), (double)held, 2000.0);
    /* Detach, flush again and push another position: nothing kept from before plays. */
    CHECK_EQ(fa_stream_detach(s), FA_OK);
    CHECK_EQ(fa_stream_push_flush(s), FA_OK);
    CHECK_EQ(push_level(s, g_dev->rate / 2, 0.125f, 40000000000LL), FA_OK);
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    wait_decoded(s, PERIOD * 4);
    t1 = g_now;
    rec_reset();
    render(PERIOD * 20);
    for (i = (int64_t)(lat * (double)g_dev->rate / 1e9) + PERIOD; i < g_rec_frames; i++)
        if (fabsf(g_rec[i * 2] - 0.125f) > 0.01f) wrong++;
    CHECK_EQ(wrong, 0);
    CHECK_NEAR((double)clock_at(s, t1 + lat + 20000000), 40020000000.0, 2000.0);
    fa_stream_close(s);
}

/* Buffered counts audio, not timestamps: a jump in the timeline adds nothing. */
static void test_buffered_ignores_timestamp_jumps(void)
{
    fa_stream* s = open_push_s16p();
    fa_time buffered = 0;
    int64_t i, heard = 0;
    CHECK(s != NULL);
    CHECK_EQ(push_level(s, g_dev->rate / 50, 0.25f, 1000000000LL), FA_OK);    /* 20 ms at 1 s */
    CHECK_EQ(push_level(s, g_dev->rate / 50, 0.25f, 10000000000LL), FA_OK);   /* 20 ms at 10 s */
    CHECK_EQ(push_level(s, g_dev->rate / 50, 0.25f, 5000000000LL), FA_OK);    /* 20 ms back at 5 s */
    CHECK_EQ(fa_stream_get_buffered(s, &buffered), FA_OK);
    CHECK_NEAR((double)buffered, 60000000.0, 1e3);
    /* Everything pushed plays, without waiting for more (the last piece read from
     * the queue has nothing behind it). */
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    wait_decoded(s, g_dev->rate * 3 / 50);
    rec_reset();
    render(PERIOD * 30);
    for (i = 0; i < g_rec_frames; i++) if (g_rec[i * 2] > 0.2f) heard++;
    CHECK_NEAR((double)heard, (double)(g_dev->rate * 3 / 50), 2.0);
    CHECK_EQ(fa_stream_get_buffered(s, &buffered), FA_OK);
    CHECK_EQ(buffered, 0);
    fa_stream_close(s);
}

/* A push stream at half the device rate: the resampler's start-up frames come out
 * first, and the clock must not show 0 for them before the app's first pts. */
static void test_clock_starts_at_first_pts_when_resampled(void)
{
    fa_audio_format f;
    fa_stream* s = NULL;
    int64_t lat = latency_ns(), base = 10000000000LL, t0, t, lowest = INT64_MAX;
    memset(&f, 0, sizeof(f));
    f.sample_format = FA_FORMAT_S16_PLANAR;
    f.sample_rate = g_dev->rate / 2;
    f.channels = 2;
    CHECK_EQ(fa_stream_open_push(g_engine, &f, NULL, &s), FA_OK);
    CHECK_EQ(push_level(s, f.sample_rate, 0.25f, base), FA_OK);
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    wait_decoded(s, PERIOD * 4);
    t0 = g_now;
    render(PERIOD * 20);
    for (t = t0; t < g_now; t += 100000) {          /* every 0.1 ms from the start */
        int64_t c = clock_at(s, t);
        if (c < lowest) lowest = c;
    }
    CHECK(lowest > base - 1000000);                /* never far before 10 s */
    /* The resampler's start-up frames take the first pts too: the clock leads the
     * audio by those few frames (well under a millisecond). */
    CHECK_NEAR((double)clock_at(s, t0 + lat + 100000000), (double)(base + 100000000), 200000.0);
    fa_stream_close(s);
}

/* After the last audio is mixed (the stream is buffering again), what the device
 * has not played yet still counts, and drains to zero as it plays. */
static void test_buffered_counts_the_device_while_draining(void)
{
    fa_stream* s = open_push_s16p();
    fa_stream_state st;
    int64_t lat = latency_ns(), mixed_end, b0, b1, b2;
    CHECK(s != NULL);
    CHECK_EQ(push_level(s, g_dev->rate / 10, 0.25f, 0), FA_OK);
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    wait_decoded(s, g_dev->rate / 10);
    render(g_dev->rate / 10 + PERIOD * 2);       /* all of it mixed, then runs dry */
    CHECK_EQ(fa_stream_get_state(s, &st), FA_OK);
    CHECK_EQ(st, FA_STATE_BUFFERING);
    /* When the callback that would play the frame after the last one mixed starts. */
    mixed_end = fa_load64(&g_dev->cb_time) + frames_to_ns(fa_clock_mixed_end(&s->clock) - fa_load64(&g_dev->cb_frame));
    fa_mutex_lock(&s->api_lock);
    b0 = fa_stream_buffered_at(s, mixed_end);
    b1 = fa_stream_buffered_at(s, mixed_end + lat / 2);
    b2 = fa_stream_buffered_at(s, mixed_end + lat + 1000000);
    fa_mutex_unlock(&s->api_lock);
    CHECK_NEAR((double)b0, (double)lat, 1e5);
    CHECK_NEAR((double)b1, (double)(lat / 2), 1e5);
    CHECK_EQ(b2, 0);
    fa_stream_close(s);
}

/* The audio thread extends clock records while other threads read them: a reader
 * never sees a record half-extended (the old length with the new end). */
static fa_clock g_race_clock;
static fa_atomic64 g_race_written;
static fa_atomic32 g_race_stop;

static void race_writer(void* arg)
{
    int64_t k;
    (void)arg;
    for (k = 0; k < 400000 && !fa_load32(&g_race_stop); k++) {
        /* 480 frames, 10 ms each: one rate throughout, so every record extends the last. */
        fa_clock_record(&g_race_clock, k * 480, 480, k * 10000000, (k + 1) * 10000000);
        fa_store64(&g_race_written, (k + 1) * 480);
    }
    fa_store32(&g_race_stop, 1);
}

static void test_clock_records_extend_atomically(void)
{
    fa_thread t;
    int64_t bad = 0, reads = 0;
    fa_clock_reset(&g_race_clock, 0);
    fa_store64(&g_race_written, 0);
    fa_store32(&g_race_stop, 0);
    CHECK_EQ(fa_thread_create(&t, race_writer, NULL), 0);
    while (!fa_load32(&g_race_stop)) {
        int64_t w = fa_load64(&g_race_written), f;
        if (w < 960) continue;
        f = w - 240 - (reads % 200);              /* inside what is written */
        if (f < 0) f = 0;
        if (llabs(fa_clock_at(&g_race_clock, (double)f) - (int64_t)((double)f * 1e9 / 48000.0)) > 1) bad++;
        reads++;
    }
    fa_thread_join(&t);
    CHECK(reads > 1000);
    CHECK_EQ(bad, 0);
}

static void test_file_clock_and_seek(void)
{
    /* 1 s of 16-bit mono WAV at the device rate. */
    uint32_t rate = g_dev->rate, frames = rate, v, i;
    size_t data = (size_t)frames * 2, n = 44 + data;
    uint8_t* b = (uint8_t*)calloc(1, n);
    fa_stream* s = NULL;
    int64_t lat = latency_ns(), t0;
    memcpy(b, "RIFF", 4); v = (uint32_t)(n - 8); memcpy(b + 4, &v, 4); memcpy(b + 8, "WAVEfmt ", 8);
    v = 16; memcpy(b + 16, &v, 4);
    b[20] = 1; b[22] = 1;
    memcpy(b + 24, &rate, 4); v = rate * 2; memcpy(b + 28, &v, 4);
    b[32] = 2; b[34] = 16;
    memcpy(b + 36, "data", 4); v = (uint32_t)data; memcpy(b + 40, &v, 4);
    for (i = 0; i < frames; i++) { int16_t sv = 4096; memcpy(b + 44 + (size_t)i * 2, &sv, 2); }
    CHECK_EQ(fa_stream_open_memory(g_engine, b, n, NULL, &s), FA_OK);
    free(b);
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    t0 = g_now;
    render(PERIOD * 30);
    CHECK_NEAR((double)clock_at(s, t0 + lat + 100000000), 100000000.0, 2000.0);
    /* A seek reports the target at once, and moves on once its audio is heard. */
    CHECK_EQ(fa_stream_seek(s, rate / 2), FA_OK);
    CHECK_NEAR((double)clock_at(s, g_now), 500000000.0, 1);
    t0 = g_now;
    render(PERIOD * 30);
    CHECK_NEAR((double)clock_at(s, t0 + lat / 2), 500000000.0, 1);
    CHECK_NEAR((double)clock_at(s, t0 + lat + 50000000), 550000000.0, 2000.0);
    /* At the end it stops at the duration. */
    render(rate);
    CHECK_NEAR((double)clock_at(s, g_now + 1000000000), 1000000000.0, 2000.0);
    fa_stream_close(s);
}

static void test_buffering_stall_resume_end(void)
{
    fa_stream* s = open_push_s16p();
    recorder rec;
    fa_stream_state st;
    rec_init(&rec);
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_set_event_callback(s, record, &rec), FA_OK);
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    render(PERIOD);
    CHECK_EQ(fa_stream_get_state(s, &st), FA_OK);
    CHECK_EQ(st, FA_STATE_BUFFERING);            /* nothing pushed yet */
    CHECK_EQ(push_level(s, g_dev->rate / 5, 0.25f, FA_PTS_NONE), FA_OK);
    wait_decoded(s, g_dev->rate / 10);
    render(PERIOD);
    CHECK_EQ(fa_stream_get_state(s, &st), FA_OK);
    CHECK_EQ(st, FA_STATE_PLAYING);
    render(g_dev->rate / 2);                     /* runs dry */
    CHECK_EQ(fa_stream_get_state(s, &st), FA_OK);
    CHECK_EQ(st, FA_STATE_BUFFERING);
    CHECK_EQ(count_events(&rec, FA_EVENT_STALLED, 1), 1);
    CHECK_EQ(push_level(s, g_dev->rate / 5, 0.25f, FA_PTS_NONE), FA_OK);
    wait_decoded(s, g_dev->rate / 10);
    render(PERIOD * 2);
    CHECK_EQ(fa_stream_get_state(s, &st), FA_OK);
    CHECK_EQ(st, FA_STATE_PLAYING);
    CHECK_EQ(count_events(&rec, FA_EVENT_RESUMED, 1), 1);
    CHECK_EQ(fa_stream_push_end(s), FA_OK);
    CHECK_EQ(push_level(s, 100, 0.25f, FA_PTS_NONE), FA_ERROR_END_OF_STREAM);
    render(g_dev->rate / 2);
    CHECK_EQ(fa_stream_get_state(s, &st), FA_OK);
    CHECK_EQ(st, FA_STATE_ENDED);
    CHECK_EQ(count_events(&rec, FA_EVENT_ENDED, 1), 1);
    CHECK_EQ(count_events(&rec, FA_EVENT_STALLED, 2), 1);  /* the end is not a stall */
    fa_stream_close(s);
    rec_free(&rec);
}

static void test_flush_and_buffered(void)
{
    fa_stream* s = open_push_s16p();
    fa_time buffered = 0;
    int64_t lat = latency_ns(), held, t0;
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_get_buffered(s, &buffered), FA_OK);
    CHECK_EQ(buffered, 0);
    CHECK_EQ(push_level(s, g_dev->rate, 0.25f, 0), FA_OK);
    CHECK_EQ(fa_stream_get_buffered(s, &buffered), FA_OK);
    CHECK_NEAR((double)buffered, 1e9, 1e3);
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    t0 = g_now;
    render(PERIOD * 20);
    held = clock_at(s, g_now);
    CHECK_NEAR((double)held, (double)(g_now - t0 - lat), 2000.0);
    /* The app seeks: flush, then push from the new position. */
    CHECK_EQ(fa_stream_push_flush(s), FA_OK);
    CHECK_EQ(fa_stream_get_buffered(s, &buffered), FA_OK);
    CHECK_EQ(buffered, 0);
    CHECK_EQ(push_level(s, g_dev->rate, 0.25f, 30000000000LL), FA_OK);
    wait_decoded(s, g_dev->rate / 4);
    t0 = g_now;
    render(PERIOD * 20);
    CHECK_NEAR((double)clock_at(s, t0 + lat + 10000000), 30010000000.0, 2000.0);
    fa_stream_close(s);
}

/* ---- tests: packets ----------------------------------------------------------------- */

static void test_pcm_packets_split_anywhere(void)
{
    fa_codec_params p;
    fa_stream* s = NULL;
    fa_open_options o;
    uint8_t bytes[4000];
    float* out;
    int64_t n, i;
    size_t at = 0;
    for (i = 0; i < 1000; i++) {               /* 1000 stereo frames of big-endian S16 */
        int16_t l = (int16_t)(i * 30), r = (int16_t)(-i * 30);
        bytes[i * 4] = (uint8_t)(l >> 8); bytes[i * 4 + 1] = (uint8_t)l;
        bytes[i * 4 + 2] = (uint8_t)(r >> 8); bytes[i * 4 + 3] = (uint8_t)r;
    }
    fa_codec_params_init(&p);
    p.codec = "pcm_s16be";
    p.sample_rate = 48000;
    p.channels = 2;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    CHECK_EQ(fa_stream_open_packets(g_engine, &p, &o, &s), FA_OK);
    while (at < sizeof(bytes)) {               /* packets of 7 bytes: frames split across them */
        fa_packet pk;
        memset(&pk, 0, sizeof(pk));
        pk.data = bytes + at;
        pk.size = sizeof(bytes) - at < 7 ? sizeof(bytes) - at : 7;
        pk.pts = FA_PTS_NONE;
        CHECK_EQ(fa_stream_push_packet(s, &pk), FA_OK);
        at += pk.size;
    }
    CHECK_EQ(fa_stream_push_end(s), FA_OK);
    out = read_all(s, 2, &n);
    CHECK_EQ(n, 1000);
    for (i = 0; i < 1000; i += 37) {
        CHECK_NEAR(out[i * 2], (double)(int16_t)(i * 30) / 32768.0, 1e-7);
        CHECK_NEAR(out[i * 2 + 1], (double)(int16_t)(-i * 30) / 32768.0, 1e-7);
    }
    free(out);
    fa_stream_close(s);
}

/* Splits `data` into frames with `frame_len` and pushes each one as a packet. */
typedef size_t (*frame_len_fn)(const uint8_t* d, size_t left);

static size_t mp3_len(const uint8_t* h, size_t left)
{
    static const uint16_t kbps[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
    static const uint32_t rates[3] = { 44100, 48000, 32000 };
    if (left < 4 || h[0] != 0xFF || (h[1] & 0xFE) != 0xFA) return 0;   /* MPEG-1 Layer III only */
    return (size_t)(144u * kbps[h[2] >> 4] * 1000u / rates[(h[2] >> 2) & 3] + ((h[2] >> 1) & 1));
}
static size_t adts_len(const uint8_t* h, size_t left)
{
    if (left < 7 || h[0] != 0xFF || (h[1] & 0xF0) != 0xF0) return 0;
    return ((size_t)(h[3] & 3) << 11) | ((size_t)h[4] << 3) | (h[5] >> 5);
}
static size_t ac3_len(const uint8_t* h, size_t left) { return left >= 768 && h[0] == 0x0B && h[1] == 0x77 ? 768 : 0; }
static size_t eac3_len(const uint8_t* h, size_t left)
{
    if (left < 4 || h[0] != 0x0B || h[1] != 0x77) return 0;
    return ((((size_t)(h[2] & 7) << 8) | h[3]) + 1) * 2;
}

/* Decodes a test file packet by packet; checks rate, channels, length and level. */
static void decode_file(const char* file, const char* codec, uint32_t rate, uint32_t channels, frame_len_fn len_of,
                        int64_t expect_frames, int64_t slack)
{
    fa_codec_params p;
    fa_stream* s = NULL;
    fa_open_options o;
    fa_stream_info info;
    size_t size = 0, at = 0;
    uint8_t* data = load_file(file, &size);
    float* out;
    int64_t n, pts = 0;
    fa_result r;
    CHECK(data != NULL);
    fa_codec_params_init(&p);
    p.codec = codec;
    p.sample_rate = rate;
    p.channels = channels;
    p.time_base.num = 1;
    p.time_base.den = (int32_t)rate;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    r = fa_stream_open_packets(g_engine, &p, &o, &s);
    if (r == FA_ERROR_PROVIDER_MISSING) { printf("  (skipped: FFmpeg not found)\n"); free(data); return; }
    CHECK_EQ(r, FA_OK);
    while (at < size) {
        fa_packet pk;
        size_t l = len_of(data + at, size - at);
        if (l == 0 || l > size - at) break;
        memset(&pk, 0, sizeof(pk));
        pk.data = data + at;
        pk.size = l;
        pk.pts = pts;
        CHECK_EQ(fa_stream_push_packet(s, &pk), FA_OK);
        at += l;
        pts += 1;  /* only the first one matters here */
    }
    free(data);
    CHECK(at == size);
    CHECK_EQ(fa_stream_push_end(s), FA_OK);
    out = read_all(s, channels, &n);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK_EQ(info.format.sample_rate, rate);
    CHECK_EQ(info.format.channels, channels);
    CHECK(strcmp(info.codec, codec) == 0);
    CHECK_NEAR((double)n, (double)expect_frames, (double)slack);
    /* A 1 kHz sine of amplitude 0.5: RMS 0.354 in the middle (FFmpeg's own AAC
     * encoder at 64 kbps makes it 0.39, as FFmpeg's decoder agrees). */
    CHECK_NEAR(rms(out, n / 4, n * 3 / 4, channels), 0.3536, 0.05);
    free(out);
    fa_stream_close(s);
}

static void test_mp3_packets(void) { decode_file("tone.mp3", "mp3", 44100, 1, mp3_len, 22050, 2400); }

/* Packets that cut frames anywhere: a frame split across packets waits for the
 * rest (419 bytes = one 417/418-byte frame plus the first bytes of the next). */
static size_t cut_419(const uint8_t* h, size_t left) { (void)h; return left < 419 ? left : 419; }
static size_t cut_1000(const uint8_t* h, size_t left) { (void)h; return left < 1000 ? left : 1000; }
static void test_mp3_packets_cut_anywhere(void)
{
    decode_file("tone.mp3", "mp3", 44100, 1, cut_419, 22050, 2400);
    decode_file("tone.mp3", "mp3", 44100, 1, cut_1000, 22050, 2400);
}

static void test_ffmpeg_aac_ac3_eac3(void)
{
    fa_ffmpeg_info fi;
    if (fa_ffmpeg_load(NULL) != FA_OK) { printf("  (skipped: FFmpeg not found)\n"); return; }
    CHECK_EQ(fa_ffmpeg_get_info(&fi), FA_OK);
    CHECK_EQ(fi.loaded, 1);
    CHECK(fi.avcodec_version >> 16 >= 58);
    printf("  libavcodec %u.%u at %s\n", fi.avcodec_version >> 16, (fi.avcodec_version >> 8) & 255, fi.avcodec_path);
    decode_file("tone.aac", "aac", 48000, 2, adts_len, 24000, 2200);
    decode_file("tone.ac3", "ac3", 48000, 2, ac3_len, 24576, 1600);
    decode_file("tone.eac3", "eac3", 48000, 2, eac3_len, 24576, 1600);
}

static void test_ffmpeg_packets_play_on_device(void)
{
    fa_codec_params p;
    fa_stream* s = NULL;
    size_t size = 0, at = 0;
    uint8_t* data;
    int64_t i;
    double acc = 0.0;
    if (fa_ffmpeg_load(NULL) != FA_OK) { printf("  (skipped: FFmpeg not found)\n"); return; }
    data = load_file("tone.ac3", &size);
    CHECK(data != NULL);
    fa_codec_params_init(&p);
    p.codec = "ac3";
    p.sample_rate = 48000;
    p.channels = 2;
    p.time_base.num = 1;
    p.time_base.den = 90000;                  /* MPEG-TS style */
    CHECK_EQ(fa_stream_open_packets(g_engine, &p, NULL, &s), FA_OK);
    while (at + 768 <= size) {
        fa_packet pk;
        memset(&pk, 0, sizeof(pk));
        pk.data = data + at;
        pk.size = 768;
        pk.pts = 900000 + (int64_t)(at / 768) * 2880;   /* 10 s + 32 ms per frame */
        pk.duration = 2880;
        CHECK_EQ(fa_stream_push_packet(s, &pk), FA_OK);
        at += 768;
    }
    free(data);
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    CHECK_NEAR((double)clock_at(s, g_now), 1e10, 1);
    wait_decoded(s, g_dev->rate / 4);
    rec_reset();
    render(g_dev->rate / 4);
    for (i = g_dev->rate / 20; i < g_dev->rate / 5; i++) acc += (double)g_rec[i * 2] * g_rec[i * 2];
    CHECK_NEAR(sqrt(acc / (double)(g_dev->rate / 5 - g_dev->rate / 20)), 0.3536, 0.04);
    fa_stream_close(s);
}

static void test_bad_arguments(void)
{
    fa_codec_params p;
    fa_stream* s = (fa_stream*)1;
    fa_audio_format f;
    fa_codec_params_init(&p);
    CHECK_EQ(fa_stream_open_packets(g_engine, &p, NULL, &s), FA_ERROR_INVALID_ARG);   /* no codec */
    CHECK(s == NULL);
    p.codec = "pcm_s16le";
    CHECK_EQ(fa_stream_open_packets(g_engine, &p, NULL, &s), FA_ERROR_INVALID_ARG);   /* no rate */
    p.sample_rate = 48000;
    p.channels = 2;
    p.codec = "no-such-codec";
    CHECK(fa_stream_open_packets(g_engine, &p, NULL, &s) != FA_OK);
    memset(&f, 0, sizeof(f));
    f.sample_format = (fa_sample_format)99;
    f.sample_rate = 48000;
    f.channels = 2;
    CHECK_EQ(fa_stream_open_push(g_engine, &f, NULL, &s), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_stream_push_packet(NULL, NULL), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_stream_push_flush(NULL), FA_ERROR_INVALID_ARG);
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
    printf("null device at %u Hz, latency %.1f ms (not started: this test is the audio thread)\n",
           g_dev->rate, (double)latency_ns() / 1e6);

    RUN(test_push_pcm_plays_exactly);
    RUN(test_push_frame_converts_any_format);
    RUN(test_cue_out_before_kept_audio);
    RUN(test_clock_follows_the_speaker);
    RUN(test_clock_follows_timestamp_jumps);
    RUN(test_clock_records_extend_atomically);
    RUN(test_clock_across_detach);
    RUN(test_clock_across_detach_with_jump);
    RUN(test_flush_after_detach_and_hold);
    RUN(test_buffered_ignores_timestamp_jumps);
    RUN(test_buffered_counts_the_device_while_draining);
    RUN(test_clock_starts_at_first_pts_when_resampled);
    RUN(test_file_clock_and_seek);
    RUN(test_buffering_stall_resume_end);
    RUN(test_flush_and_buffered);
    RUN(test_pcm_packets_split_anywhere);
    RUN(test_mp3_packets);
    RUN(test_mp3_packets_cut_anywhere);
    RUN(test_ffmpeg_aac_ac3_eac3);
    RUN(test_ffmpeg_packets_play_on_device);
    RUN(test_bad_arguments);

    fa_device_close(g_dev);
    fa_engine_destroy(g_engine);
    free(g_rec);
    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    printf("PASS\n");
    return 0;
}
