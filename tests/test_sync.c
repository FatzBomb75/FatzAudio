/* White-box tests of deck sync, beat loops and auto-mix. The mixer's device runs on
 * the null backend and is stopped right after it is made: this test is the audio
 * thread (fa_device_render_output) on a simulated clock, and runs the event thread's
 * sync and auto-mix ticks by hand at the same simulated time (fa_mixer_test_tick), so
 * every position is exact and nothing depends on how fast the machine is. Phase is
 * checked from the decks' positions (frames handed to the device), independently of
 * the clocks sync itself reads. */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "mixer.h"
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
static fa_device* g_dev;     /* plain device for the jump test, never started */
static uint32_t g_rate;

/* ---- media ------------------------------------------------------------------ */

/* 16-bit mono WAV. kind 0: a sawtooth whose value encodes the frame index modulo
 * 4096; kind 1: a quiet sine. */
static fa_stream* open_wav(uint32_t frames, int kind)
{
    size_t data = (size_t)frames * 2, n = 44 + data;
    uint8_t* b = (uint8_t*)malloc(n);
    uint32_t i, v, rate = g_rate;
    fa_stream* s = NULL;
    if (!b) return NULL;
    memcpy(b, "RIFF", 4); v = (uint32_t)(n - 8); memcpy(b + 4, &v, 4); memcpy(b + 8, "WAVEfmt ", 8);
    v = 16; memcpy(b + 16, &v, 4);
    b[20] = 1; b[21] = 0; b[22] = 1; b[23] = 0;
    memcpy(b + 24, &rate, 4); v = rate * 2; memcpy(b + 28, &v, 4);
    b[32] = 2; b[33] = 0; b[34] = 16; b[35] = 0;
    memcpy(b + 36, "data", 4); v = (uint32_t)data; memcpy(b + 40, &v, 4);
    for (i = 0; i < frames; i++) {
        int16_t sv = kind == 0 ? (int16_t)((int)(i % 4096) * 8 - 16384)
                               : (int16_t)lrint(2000.0 * sin(2.0 * 3.14159265358979 * 440.0 * i / rate));
        memcpy(b + 44 + (size_t)i * 2, &sv, 2);
    }
    if (fa_stream_open_memory(g_engine, b, n, NULL, &s) != FA_OK) s = NULL;
    free(b);
    return s;
}

static int saw_index(float v) { return (int)lrintf((v * 32768.0f + 16384.0f) / 8.0f); }

static fa_result set_grid(fa_stream* s, double bpm, fa_frames first_beat, float confidence)
{
    fa_beat_grid g;
    memset(&g, 0, sizeof(g));
    g.struct_size = sizeof(g);
    g.bpm = bpm;
    g.first_beat = first_beat;
    g.sample_rate = g_rate;
    g.beats_per_bar = 4;
    g.confidence = confidence;
    return fa_stream_set_beat_grid(s, &g, NULL, 0);
}

/* ---- the rig: a mixer on a simulated clock ------------------------------------- */

typedef struct rig {
    fa_mixer* m;
    fa_device* dev;
    fa_deck* a;
    fa_deck* b;
    uint32_t channels;
    int64_t t;                   /* simulated monotonic time of the next callback */
    int64_t last_tick;
    int ev[32];                  /* mixer events: type << 8 | index */
    int nev;
} rig;

static void FA_CALL on_event(const fa_mixer_event* e, void* user)
{
    rig* r = (rig*)user;
    if (r->nev < 32) r->ev[r->nev++] = (int)e->type << 8 | (int)e->index;
}

static int rig_open(rig* r)
{
    fa_mixer_config c;
    memset(r, 0, sizeof(*r));
    fa_mixer_config_init(&c);
    c.latency = FA_LATENCY_NORMAL;
    if (fa_mixer_create(g_engine, &c, &r->m) != FA_OK) return 0;
    fa_mixer_get_device(r->m, &r->dev);
    fa_device_stop(r->dev);   /* this test is the audio thread */
    fa_mixer_get_deck(r->m, 0, &r->a);
    fa_mixer_get_deck(r->m, 1, &r->b);
    fa_mixer_get_output_channels(r->m, &r->channels, NULL);
    fa_mixer_set_event_callback(r->m, on_event, r);
    r->m->test_ticks = 1;     /* ticks by hand, at the simulated time */
    r->dev->cb_prev_time = 0; /* no smoothing toward the real callbacks before the stop */
    r->t = 1000000000000LL;
    r->m->test_now = r->t;
    return 1;
}

static void rig_close(rig* r)
{
    fa_stream* s = NULL;
    int d;
    for (d = 0; d < 2; d++) {
        fa_deck_get_stream(d ? r->b : r->a, &s);
        fa_deck_load(d ? r->b : r->a, NULL, 0);
        if (s) fa_stream_close(s);
    }
    fa_mixer_destroy(r->m);
}

/* Renders `frames` in 480-frame callbacks once the decks have that much decoded,
 * ticking every 50 ms of simulated time. */
static void rig_run(rig* r, uint32_t frames)
{
    static float out[480 * 8];
    while (frames > 0) {
        uint32_t n = frames < 480 ? frames : 480;
        int i, d;
        for (i = 0; i < 4000; i++) {
            int ok = 1;
            for (d = 0; d < 2; d++) {
                fa_stream* s = NULL;
                fa_deck_get_stream(d ? r->b : r->a, &s);
                if (s && s->ring.data && fa_ring_readable(&s->ring) < (int64_t)n && !fa_load32(&s->eof) &&
                    !fa_load32(&s->flushing))
                    ok = 0;
            }
            if (ok) break;
            fa_sleep_ms(1);
        }
        fa_fetch_add64(&r->dev->epoch, 1);
        fa_device_render_output(r->dev, out, r->channels, n, r->t);
        fa_fetch_add64(&r->dev->epoch, 1);
        r->t += (int64_t)((double)n * 1e9 / g_rate + 0.5);
        r->m->test_now = r->t;
        if (r->t - r->last_tick >= MIXER_TICK_NS) {
            r->last_tick = r->t;
            fa_mixer_test_tick(r->m, r->t);
        }
        frames -= n;
    }
}

static double seconds(double s) { return s * g_rate; }

static fa_stream* load(fa_deck* d, uint32_t frames, double bpm, fa_frames first_beat)
{
    fa_stream* s = open_wav(frames, 1);
    if (!s) return NULL;
    if ((bpm > 0.0 && set_grid(s, bpm, first_beat, 1.0f) != FA_OK) || fa_deck_load(d, s, 0) != FA_OK) {
        fa_stream_close(s);
        return NULL;
    }
    return s;
}

/* Device frame `ahead` after the clock, as a fa_stream_play_at time. */
static fa_time at_frame(rig* r, int64_t ahead)
{
    return (fa_time)((double)(fa_load64(&r->dev->clock) + ahead) * 1e9 / g_rate + 0.5);
}

static fa_frames pos(fa_stream* s)
{
    fa_frames p = 0;
    fa_stream_get_position(s, &p);
    return p;
}

/* Beat phase difference of two decks of the same tempo, from their positions, in frames
 * (wrapped to the nearest beat). */
static double frame_phase(fa_stream* a, fa_frames fa, fa_stream* b, fa_frames fb, double beat)
{
    double x = (double)(pos(b) - fb) - (double)(pos(a) - fa);
    return x - beat * floor(x / beat + 0.5);
}

static float tempo_of(fa_stream* s)
{
    float t = 0.0f;
    fa_stream_get_param(s, FA_PARAM_TEMPO, &t);
    return t;
}

/* ---- grid math and tempo choice (pure logic) ------------------------------------- */

static void test_grid_math(void)
{
    fa_tempo_segment segs[2] = { { 1000, 120.0, 0.0 }, { 49000, 60.0, 2.0 } };
    fa_grid_view v;
    double f = 0.0;
    memset(&v, 0, sizeof(v));
    v.g.sample_rate = 48000;
    v.segs = segs;
    v.n = 2;
    v.scale = 1.0;
    CHECK_NEAR(fa_grid_beat_at(&v, 1000.0), 0.0, 1e-9);
    CHECK_NEAR(fa_grid_beat_at(&v, 25000.0), 1.0, 1e-9);     /* 24000 frames a beat at 120 */
    CHECK_NEAR(fa_grid_beat_at(&v, 97000.0), 3.0, 1e-9);     /* 48000 a beat after the change */
    CHECK_NEAR(fa_grid_beat_at(&v, -23000.0), -1.0, 1e-9);   /* before the first beat: its tempo */
    CHECK_NEAR(fa_grid_frame_at(&v, 2.5), 73000.0, 1e-6);
    CHECK_NEAR(fa_grid_frame_at(&v, 1.5), 37000.0, 1e-6);
    CHECK_NEAR(fa_sync_tempo_for(120.0, 125.0, 1.0, 0.16, &f), 0.96, 1e-12);
    CHECK_NEAR(f, 1.0, 0.0);
    CHECK_NEAR(fa_sync_tempo_for(170.0, 85.0, 1.0, 0.16, &f), 1.0, 1e-12);   /* half time */
    CHECK_NEAR(f, 2.0, 0.0);
    CHECK_NEAR(fa_sync_tempo_for(80.0, 160.0, 1.0, 0.16, &f), 1.0, 1e-12);   /* double time */
    CHECK_NEAR(f, 0.5, 0.0);
    CHECK_NEAR(fa_sync_tempo_for(120.0, 100.0, 1.25, 0.16, &f), 0.96, 1e-12); /* RATE counts */
    CHECK_NEAR(fa_sync_tempo_for(120.0, 150.0, 1.0, 0.16, &f), 0.0, 0.0);    /* out of reach */
}

/* ---- sync -------------------------------------------------------------------- */

static void test_sync_tempo(void)
{
    rig r;
    fa_stream *a, *b;
    fa_deck_beat_info bi;
    CHECK(rig_open(&r));
    a = load(r.a, (uint32_t)seconds(10), 120.0, 0);
    b = load(r.b, (uint32_t)seconds(10), 125.0, 0);
    CHECK(a && b);
    CHECK_EQ(fa_stream_play(a), FA_OK);
    rig_run(&r, (uint32_t)seconds(0.2));
    CHECK_EQ(fa_deck_sync(r.b, r.a, FA_SYNC_TEMPO), FA_OK);
    CHECK_NEAR(tempo_of(b), 0.96, 1e-6);
    memset(&bi, 0, sizeof(bi));
    bi.struct_size = sizeof(bi);
    CHECK_EQ(fa_deck_get_beat_info(r.b, &bi), FA_OK);
    CHECK_NEAR(bi.bpm, 120.0, 1e-3);
    CHECK_EQ(bi.synced, 1);
    CHECK_EQ(bi.locked, 0);
    /* Out of reach: nothing changes. */
    CHECK_EQ(set_grid(b, 150.0, 0, 1.0f), FA_OK);
    CHECK_EQ(fa_deck_set_param(r.b, FA_DECK_SYNC_RANGE, 0.1f), FA_OK);
    CHECK_EQ(fa_deck_sync(r.b, r.a, FA_SYNC_TEMPO), FA_ERROR_OUT_OF_RANGE);
    CHECK_NEAR(tempo_of(b), 0.96, 1e-6);
    /* Errors. */
    CHECK_EQ(fa_deck_sync(r.b, r.b, FA_SYNC_TEMPO), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_deck_sync(r.b, r.a, 1u << 7), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_stream_set_beat_grid(a, NULL, NULL, 0), FA_OK);
    CHECK_EQ(fa_deck_sync(r.b, r.a, FA_SYNC_TEMPO), FA_ERROR_NOT_FOUND);   /* the leader has no grid */
    CHECK_EQ(fa_deck_sync(r.b, NULL, 0), FA_OK);                            /* ends it */
    CHECK_EQ(fa_deck_get_beat_info(r.b, &bi), FA_OK);
    CHECK_EQ(bi.synced, 0);
    CHECK_EQ(fa_deck_get_beat_info(r.a, &bi), FA_ERROR_NOT_FOUND);
    rig_close(&r);
}

/* A stopped deck started in phase: its beat lands on the leader's beat, to the frame. */
static void test_play_synced_beat(void)
{
    rig r;
    fa_stream *a, *b;
    fa_stream_state st;
    fa_deck_beat_info bi;
    double beat = seconds(0.5);
    CHECK(rig_open(&r));
    a = load(r.a, (uint32_t)seconds(10), 120.0, 1000);
    b = load(r.b, (uint32_t)seconds(10), 120.0, 5000);
    CHECK(a && b);
    CHECK_EQ(fa_deck_set_cue_point(r.b, 300), FA_OK);   /* starts 4700 frames before its first beat */
    CHECK_EQ(fa_stream_play_at(a, at_frame(&r, 480)), FA_OK);
    rig_run(&r, (uint32_t)seconds(0.73));
    CHECK_EQ(fa_deck_play_synced(r.b, r.a, FA_SYNC_BEAT), FA_OK);
    fa_stream_get_state(b, &st);
    CHECK_EQ(st, FA_STATE_SCHEDULED);
    rig_run(&r, (uint32_t)seconds(1.0));
    fa_stream_get_state(b, &st);
    CHECK_EQ(st, FA_STATE_PLAYING);
    CHECK_NEAR(frame_phase(a, 1000, b, 5000, beat), 0.0, 1.0);
    memset(&bi, 0, sizeof(bi));
    bi.struct_size = sizeof(bi);
    CHECK_EQ(fa_deck_get_beat_info(r.b, &bi), FA_OK);
    CHECK_NEAR(bi.phase_error_ms, 0.0, 0.05);
    rig_close(&r);
}

/* BAR: the deck's downbeat lands on one of the leader's downbeats. */
static void test_play_synced_bar(void)
{
    rig r;
    fa_stream *a, *b;
    double beat = seconds(0.5), x;
    CHECK(rig_open(&r));
    a = load(r.a, (uint32_t)seconds(10), 120.0, 0);
    b = load(r.b, (uint32_t)seconds(10), 120.0, 2000);
    CHECK(a && b);
    CHECK_EQ(fa_stream_play_at(a, at_frame(&r, 480)), FA_OK);
    rig_run(&r, (uint32_t)seconds(0.6));   /* the leader is past beat 1 of its first bar */
    CHECK_EQ(fa_deck_play_synced(r.b, r.a, FA_SYNC_BAR), FA_OK);
    rig_run(&r, (uint32_t)seconds(2.5));
    x = (double)(pos(b) - 2000) - (double)pos(a);
    x -= 4.0 * beat * floor(x / (4.0 * beat) + 0.5);
    CHECK_NEAR(x, 0.0, 1.0);
    rig_close(&r);
}

/* With TEMPO the deck is stretched to the leader's tempo first, then lands in phase. */
static void test_play_synced_tempo(void)
{
    rig r;
    fa_stream *a, *b;
    double la, lb;
    CHECK(rig_open(&r));
    a = load(r.a, (uint32_t)seconds(10), 120.0, 1000);
    b = load(r.b, (uint32_t)seconds(10), 100.0, 3000);
    CHECK(a && b);
    CHECK_EQ(fa_deck_set_param(r.b, FA_DECK_SYNC_RANGE, 0.3f), FA_OK);
    CHECK_EQ(fa_stream_play_at(a, at_frame(&r, 480)), FA_OK);
    rig_run(&r, (uint32_t)seconds(0.3));
    CHECK_EQ(fa_deck_play_synced(r.b, r.a, FA_SYNC_TEMPO | FA_SYNC_BEAT), FA_OK);
    CHECK_NEAR(tempo_of(b), 1.2, 1e-6);
    rig_run(&r, (uint32_t)seconds(2.0));
    /* Beats from positions: 24000 frames a beat on A, 28800 on B (as source frames). */
    la = (double)(pos(a) - 1000) / seconds(0.5);
    lb = (double)(pos(b) - 3000) / seconds(0.6);
    la -= floor(la);
    lb -= floor(lb);
    if (la - lb > 0.5) lb += 1.0;
    if (lb - la > 0.5) la += 1.0;
    CHECK_NEAR(la, lb, 0.004);   /* 2 ms at 120 BPM */
    rig_close(&r);
}

/* Both playing, 10 ms apart: pulled in by a bend, then back to the matched tempo. */
static void test_pull_small_error(void)
{
    rig r;
    fa_stream *a, *b;
    double beat = seconds(0.5);
    CHECK(rig_open(&r));
    a = load(r.a, (uint32_t)seconds(20), 120.0, 0);
    b = load(r.b, (uint32_t)seconds(20), 120.0, 0);
    CHECK(a && b);
    CHECK_EQ(fa_stream_play_at(a, at_frame(&r, 480)), FA_OK);
    CHECK_EQ(fa_stream_play_at(b, at_frame(&r, 480 + (int64_t)seconds(0.010))), FA_OK);   /* 10 ms late */
    rig_run(&r, (uint32_t)seconds(0.3));
    CHECK_NEAR(frame_phase(a, 0, b, 0, beat), -seconds(0.010), 1.0);
    CHECK_EQ(fa_deck_sync(r.b, r.a, FA_SYNC_BEAT), FA_OK);
    rig_run(&r, (uint32_t)seconds(6.0));
    CHECK_NEAR(frame_phase(a, 0, b, 0, beat), 0.0, seconds(0.001));
    CHECK_NEAR(tempo_of(b), 1.0, 1e-6);   /* settled: the bend is off */
    rig_close(&r);
}

/* 150 ms apart: jumped on the deck's next beat, no tempo change. */
static void test_jump_large_error(void)
{
    rig r;
    fa_stream *a, *b;
    double beat = seconds(0.5);
    CHECK(rig_open(&r));
    a = load(r.a, (uint32_t)seconds(20), 120.0, 0);
    b = load(r.b, (uint32_t)seconds(20), 120.0, 0);
    CHECK(a && b);
    CHECK_EQ(fa_stream_play_at(a, at_frame(&r, 480)), FA_OK);
    CHECK_EQ(fa_stream_play_at(b, at_frame(&r, 480 + (int64_t)seconds(0.150))), FA_OK);
    rig_run(&r, (uint32_t)seconds(0.5));
    CHECK_EQ(fa_deck_sync(r.b, r.a, FA_SYNC_BEAT), FA_OK);
    rig_run(&r, (uint32_t)seconds(2.0));
    CHECK_NEAR(frame_phase(a, 0, b, 0, beat), 0.0, seconds(0.001));
    CHECK_NEAR(tempo_of(b), 1.0, 1e-6);
    rig_close(&r);
}

/* LOCK follows the leader's tempo moves and keeps the phase. */
static void test_lock_follows(void)
{
    rig r;
    fa_stream *a, *b;
    fa_deck_beat_info bi;
    CHECK(rig_open(&r));
    a = load(r.a, (uint32_t)seconds(20), 120.0, 0);
    b = load(r.b, (uint32_t)seconds(20), 120.0, 700);
    CHECK(a && b);
    CHECK_EQ(fa_stream_play_at(a, at_frame(&r, 480)), FA_OK);
    rig_run(&r, (uint32_t)seconds(0.2));
    CHECK_EQ(fa_deck_play_synced(r.b, r.a, FA_SYNC_TEMPO | FA_SYNC_BEAT | FA_SYNC_LOCK), FA_OK);
    rig_run(&r, (uint32_t)seconds(1.0));
    CHECK_EQ(fa_stream_set_param(a, FA_PARAM_TEMPO, 1.05f), FA_OK);
    rig_run(&r, (uint32_t)seconds(5.0));
    CHECK_NEAR(tempo_of(b), 1.05, 0.002);
    memset(&bi, 0, sizeof(bi));
    bi.struct_size = sizeof(bi);
    CHECK_EQ(fa_deck_get_beat_info(r.b, &bi), FA_OK);
    CHECK_EQ(bi.locked, 1);
    CHECK_NEAR(bi.phase_error_ms, 0.0, 1.0);
    CHECK_NEAR(bi.bpm, 126.0, 0.3);
    {   /* and from the positions: both decks' beats, as source frames, line up */
        double la = (double)pos(a) / seconds(0.5), lb = (double)(pos(b) - 700) / seconds(0.5);
        la -= floor(la);
        lb -= floor(lb);
        if (la - lb > 0.5) lb += 1.0;
        if (lb - la > 0.5) la += 1.0;
        CHECK_NEAR(la, lb, 0.004);
    }
    /* A new stream on the leader ends the lock. */
    {
        fa_stream* c = open_wav((uint32_t)seconds(2), 1);
        CHECK(c);
        CHECK_EQ(fa_deck_load(r.a, c, FA_DECK_LOAD_FORCE), FA_OK);
        fa_stream_close(a);
        CHECK_EQ(fa_deck_get_beat_info(r.b, &bi), FA_OK);
        CHECK_EQ(bi.synced, 0);
    }
    rig_close(&r);
}

/* fa_stream_jump on a plain device: joins exactly where the stream reaches `at`. */
static void test_stream_jump(void)
{
    fa_stream* s = open_wav(400000, 0);
    static float out[2 * 512];
    int* idx = (int*)malloc(sizeof(int) * 60000);
    int n = 0, i, j = -1;
    fa_result r = FA_ERROR_BUSY;
    CHECK(s && idx);
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    while (n < 50000) {
        uint32_t k;
        for (i = 0; i < 2000 && fa_ring_readable(&s->ring) < 512 && !fa_load32(&s->eof); i++) fa_sleep_ms(1);
        memset(out, 0, sizeof(out));
        fa_fetch_add64(&g_dev->epoch, 1);
        fa_stream_mix(s, g_dev, out, 512);
        fa_fetch_add64(&g_dev->clock, 512);
        fa_fetch_add64(&g_dev->epoch, 1);
        for (k = 0; k < 512; k++) idx[n++] = saw_index(out[k * 2]);
        if (r == FA_ERROR_BUSY && n >= 10240) r = fa_stream_jump(s, 20000, 5000);
    }
    CHECK_EQ(r, FA_OK);
    for (i = 1; i < n; i++)
        if (idx[i] != (idx[i - 1] + 1) % 4096) { j = i; break; }
    CHECK_EQ(j, 20000);
    CHECK_EQ(idx[j], 25000 % 4096);
    CHECK_EQ(pos(s), n + 5000);
    CHECK_EQ(fa_stream_jump(s, -1, -30000), FA_OK);   /* at once (a margin ahead), backwards */
    fa_stream_close(s);
    free(idx);
}

/* ---- beat loops ----------------------------------------------------------------- */

static void test_loop_beats(void)
{
    rig r;
    fa_stream *a, *b;
    fa_frames in = -1, out = -1;
    CHECK(rig_open(&r));
    a = load(r.a, (uint32_t)seconds(10), 120.0, 1000);
    b = load(r.b, (uint32_t)seconds(10), 0.0, 0);   /* no grid */
    CHECK(a && b);
    CHECK_EQ(fa_stream_play_at(a, at_frame(&r, 0)), FA_OK);
    rig_run(&r, 36000);   /* heard a little before 36000: beat 1 (25000..49000) */
    CHECK_EQ(fa_deck_set_loop_beats(r.a, 2.0), FA_OK);
    fa_deck_get_loop(r.a, &in, &out);
    CHECK_EQ(in, 25000);
    CHECK_EQ(out, 73000);
    CHECK_EQ(fa_deck_set_loop_beats(r.a, 1.0), FA_OK);   /* halved: the same start */
    fa_deck_get_loop(r.a, &in, &out);
    CHECK_EQ(in, 25000);
    CHECK_EQ(out, 49000);
    CHECK_EQ(fa_deck_set_loop_beats(r.a, 0.0), FA_OK);
    fa_deck_get_loop(r.a, &in, &out);
    CHECK_EQ(out, 0);
    CHECK_EQ(fa_deck_set_loop_beats(r.a, 0.25), FA_OK);   /* a quarter beat: the quarter heard now */
    fa_deck_get_loop(r.a, &in, &out);
    CHECK(in >= 25000 && in < 37000 && (in - 25000) % 6000 == 0);
    CHECK_EQ(out - in, 6000);
    CHECK_EQ(fa_deck_set_loop_beats(r.a, 64.0), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_deck_set_loop_beats(r.b, 4.0), FA_ERROR_NOT_FOUND);
    rig_close(&r);
}

/* ---- auto-mix ---------------------------------------------------------------- */

static int find_event(rig* r, fa_event_type type, int index)
{
    int i;
    for (i = 0; i < r->nev; i++)
        if (r->ev[i] == ((int)type << 8 | index)) return i;
    return -1;
}

/* No grids: a plain crossfade over `length`, starting `length` before the end. */
static void test_automix_plain(void)
{
    rig r;
    fa_stream *a, *b;
    fa_automix_options o;
    int64_t a_start;
    float xf = 0.0f;
    fa_stream_state st;
    CHECK(rig_open(&r));
    a = load(r.a, (uint32_t)seconds(4), 0.0, 0);
    b = load(r.b, (uint32_t)seconds(4), 0.0, 0);
    CHECK(a && b);
    CHECK_EQ(fa_deck_set_cue_point(r.b, 1234), FA_OK);
    CHECK_EQ(fa_mixer_set_param(r.m, FA_MIX_CROSSFADER, 0.0f), FA_OK);
    fa_automix_options_init(&o);
    o.length = 1000000000LL;
    CHECK_EQ(fa_mixer_set_automix(r.m, &o), FA_OK);
    a_start = fa_load64(&r.dev->clock) + 480;
    CHECK_EQ(fa_stream_play_at(a, at_frame(&r, 480)), FA_OK);
    rig_run(&r, (uint32_t)seconds(2.5));
    /* Scheduled at the device frame A reaches 3 s, B from its cue point. */
    fa_stream_get_state(b, &st);
    CHECK_EQ(st, FA_STATE_SCHEDULED);
    CHECK_NEAR((double)fa_load64(&b->start_frame), (double)a_start + seconds(3.0), 2.0);
    CHECK_EQ(pos(b), 1234);
    rig_run(&r, (uint32_t)seconds(1.2));
    CHECK(find_event(&r, FA_EVENT_AUTOMIX_STARTED, 1) >= 0);
    CHECK_EQ(find_event(&r, FA_EVENT_AUTOMIX_DONE, 1), -1);
    rig_run(&r, (uint32_t)seconds(0.5));
    CHECK(find_event(&r, FA_EVENT_AUTOMIX_DONE, 1) > find_event(&r, FA_EVENT_AUTOMIX_STARTED, 1));
    CHECK(find_event(&r, FA_EVENT_DECK_EMPTY, 0) > find_event(&r, FA_EVENT_AUTOMIX_DONE, 1));
    fa_mixer_get_param(r.m, FA_MIX_CROSSFADER, &xf);
    CHECK_NEAR(xf, 1.0, 1e-6);   /* across to B */
    fa_stream_get_state(a, &st);
    CHECK(st == FA_STATE_STOPPED || st == FA_STATE_ENDED);
    CHECK_EQ(fa_mixer_automix_now(r.m), FA_ERROR_NOT_FOUND);   /* deck A still holds the old song */
    rig_close(&r);
}

/* Both grids good: on the outgoing deck's downbeat nearest the mix point, the incoming
 * deck from its first downbeat, whole bars, at the outgoing tempo. */
static void test_automix_aligned(void)
{
    rig r;
    fa_stream *a, *b;
    fa_automix_options o;
    int64_t a_start;
    CHECK(rig_open(&r));
    a = load(r.a, (uint32_t)seconds(10), 120.0, 0);
    b = load(r.b, (uint32_t)seconds(10), 123.0, 3000);
    CHECK(a && b);
    CHECK_EQ(fa_stream_set_marker(a, FA_MARKER_MIX_POINT, (fa_frames)seconds(3.1)), FA_OK);
    CHECK_EQ(fa_deck_set_cue_point(r.b, 2000), FA_OK);
    fa_automix_options_init(&o);
    o.length_beats = 30.0f;   /* rounds to 8 bars */
    CHECK_EQ(fa_mixer_set_automix(r.m, &o), FA_OK);
    a_start = fa_load64(&r.dev->clock) + 480;
    CHECK_EQ(fa_stream_play_at(a, at_frame(&r, 480)), FA_OK);
    rig_run(&r, (uint32_t)seconds(2.5));
    CHECK_NEAR((double)fa_load64(&b->start_frame), (double)a_start + seconds(4.0), 2.0);   /* bar 2 */
    CHECK_EQ(pos(b), 3000);
    CHECK_NEAR(tempo_of(b), 120.0 / 123.0, 1e-6);
    CHECK_NEAR((double)(r.m->am.end - r.m->am.start), seconds(16.0), 1.0);
    CHECK(r.b->leader == r.a);   /* locked meanwhile */
    rig_run(&r, (uint32_t)seconds(3.0));
    {
        double la = (double)pos(a) / seconds(0.5), lb = (double)(pos(b) - 3000) / (seconds(60.0 / 123.0));
        la -= floor(la);
        lb -= floor(lb);
        if (la - lb > 0.5) lb += 1.0;
        if (lb - la > 0.5) la += 1.0;
        CHECK_NEAR(la, lb, 0.004);
    }
    rig_close(&r);
}

/* DECK_EMPTY 30 s before the mix while the idle deck is empty; skip once it is loaded. */
static void test_automix_empty_and_skip(void)
{
    rig r;
    fa_stream *a, *b;
    fa_automix_options o;
    fa_stream_state st;
    CHECK(rig_open(&r));
    a = load(r.a, (uint32_t)seconds(40), 0.0, 0);
    CHECK(a);
    fa_automix_options_init(&o);
    CHECK_EQ(fa_mixer_set_automix(r.m, &o), FA_OK);
    CHECK_EQ(fa_stream_play_at(a, at_frame(&r, 480)), FA_OK);
    CHECK_EQ(fa_mixer_automix_now(r.m), FA_ERROR_NOT_FOUND);   /* nothing plays yet */
    rig_run(&r, (uint32_t)seconds(0.2));
    CHECK_EQ(find_event(&r, FA_EVENT_DECK_EMPTY, 1), -1);      /* the mix is 32 s away */
    rig_run(&r, (uint32_t)seconds(0.2));
    CHECK_EQ(fa_mixer_automix_now(r.m), FA_ERROR_NOT_FOUND);   /* deck B is empty */
    b = load(r.b, (uint32_t)seconds(10), 0.0, 0);
    CHECK(b);
    CHECK_EQ(fa_mixer_automix_now(r.m), FA_OK);
    rig_run(&r, (uint32_t)seconds(0.1));
    fa_stream_get_state(b, &st);
    CHECK(st == FA_STATE_SCHEDULED || st == FA_STATE_PLAYING);
    CHECK_EQ(fa_mixer_automix_now(r.m), FA_ERROR_BUSY);
    CHECK_EQ(fa_mixer_set_automix(r.m, NULL), FA_OK);
    rig_run(&r, (uint32_t)seconds(8.5));                      /* finishes, without events */
    CHECK_EQ(find_event(&r, FA_EVENT_AUTOMIX_DONE, 1), -1);
    fa_stream_get_state(a, &st);
    CHECK(st == FA_STATE_STOPPED);
    CHECK_EQ(fa_mixer_automix_now(r.m), FA_ERROR_NOT_FOUND);   /* off */
    o.length = 100;
    CHECK_EQ(fa_mixer_set_automix(r.m, &o), FA_ERROR_INVALID_ARG);
    fa_automix_options_init(&o);
    o.flags |= FA_AUTOMIX_BASS_SWAP;
    CHECK_EQ(fa_mixer_set_automix(r.m, &o), FA_ERROR_INVALID_ARG);
    rig_close(&r);
}

static void test_automix_empty_warning(void)
{
    rig r;
    fa_stream* a;
    fa_automix_options o;
    CHECK(rig_open(&r));
    a = load(r.a, (uint32_t)seconds(30), 0.0, 0);
    CHECK(a);
    fa_automix_options_init(&o);
    CHECK_EQ(fa_mixer_set_automix(r.m, &o), FA_OK);
    CHECK_EQ(fa_stream_play_at(a, at_frame(&r, 480)), FA_OK);
    rig_run(&r, (uint32_t)seconds(0.3));
    CHECK(find_event(&r, FA_EVENT_DECK_EMPTY, 1) >= 0);   /* 22 s to the mix */
    CHECK_EQ(r.nev, 1);
    rig_run(&r, (uint32_t)seconds(0.3));
    CHECK_EQ(r.nev, 1);                                    /* once per song */
    rig_close(&r);
}

int main(void)
{
    fa_engine_config cfg;
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    cfg.worker_threads = 2;
    if (fa_engine_create(&cfg, &g_engine) != FA_OK) { fprintf(stderr, "engine create failed\n"); return 1; }
    if (fa_device_open(g_engine, NULL, &g_dev) != FA_OK) { fprintf(stderr, "device open failed\n"); return 1; }
    g_rate = g_dev->rate;
    printf("null device at %u Hz\n", g_rate);

    RUN(test_grid_math);
    RUN(test_sync_tempo);
    RUN(test_play_synced_beat);
    RUN(test_play_synced_bar);
    RUN(test_play_synced_tempo);
    RUN(test_pull_small_error);
    RUN(test_jump_large_error);
    RUN(test_lock_follows);
    RUN(test_stream_jump);
    RUN(test_loop_beats);
    RUN(test_automix_plain);
    RUN(test_automix_aligned);
    RUN(test_automix_empty_and_skip);
    RUN(test_automix_empty_warning);

    fa_device_close(g_dev);
    fa_engine_destroy(g_engine);
    if (g_failures) { fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    printf("all sync tests passed\n");
    return 0;
}
