/* White-box tests of the DJ mixer and deck loops. The mixer's device runs on the
 * null backend and is stopped right after it is made, so this test plays the part
 * of the audio thread (fa_device_render_output) and checks the samples. Deck
 * loops are checked sample by sample on a plain device, where the mix is exact
 * (the mixer's isolator is an allpass, so it moves samples). */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "fa_internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__)
#  include <TargetConditionals.h>
#endif

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; return; } } while (0)
#define CHECK_EQ(a, b) do { long long va_ = (long long)(a), vb_ = (long long)(b); if (va_ != vb_) { \
    fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_failures++; return; } } while (0)
#define CHECK_NEAR(a, b, tol) do { double va_ = (a), vb_ = (b); if (fabs(va_ - vb_) > (tol)) { \
    fprintf(stderr, "  FAIL %s:%d: %s ~ %s (%.6f vs %.6f)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_failures++; return; } } while (0)
#define RUN(t) do { int before_ = g_failures; printf("%s\n", #t); t(); if (g_failures == before_) printf("  ok\n"); } while (0)

static fa_engine* g_engine;
static fa_device* g_dev;     /* plain device for the loop tests, never started */
static uint32_t g_rate;

/* ---- media ------------------------------------------------------------------ */

/* 16-bit mono WAV at `rate`. kind 0: a sawtooth whose value encodes the frame index
 * modulo 4096 (exact in float); kind 1: a sine of `hz` at amplitude 0.25. */
static uint8_t* make_wav(uint32_t rate, uint32_t frames, int kind, double hz, size_t* size)
{
    size_t data = (size_t)frames * 2, n = 44 + data;
    uint8_t* b = (uint8_t*)malloc(n);
    uint32_t i, v;
    memcpy(b, "RIFF", 4); v = (uint32_t)(n - 8); memcpy(b + 4, &v, 4); memcpy(b + 8, "WAVEfmt ", 8);
    v = 16; memcpy(b + 16, &v, 4);
    b[20] = 1; b[21] = 0; b[22] = 1; b[23] = 0;
    memcpy(b + 24, &rate, 4); v = rate * 2; memcpy(b + 28, &v, 4);
    b[32] = 2; b[33] = 0; b[34] = 16; b[35] = 0;
    memcpy(b + 36, "data", 4); v = (uint32_t)data; memcpy(b + 40, &v, 4);
    for (i = 0; i < frames; i++) {
        int16_t sv = kind == 0 ? (int16_t)((int)(i % 4096) * 8 - 16384)
                               : (int16_t)lrint(8192.0 * sin(2.0 * 3.14159265358979 * hz * i / rate));
        memcpy(b + 44 + (size_t)i * 2, &sv, 2);
    }
    *size = n;
    return b;
}

static fa_stream* open_wav(uint32_t frames, int kind, double hz)
{
    size_t n;
    uint8_t* wav = make_wav(g_rate, frames, kind, hz, &n);
    fa_stream* s = NULL;
    if (fa_stream_open_memory(g_engine, wav, n, NULL, &s) != FA_OK) s = NULL;
    free(wav);
    return s;
}

/* The sawtooth's frame index (mod 4096) of a sample. */
static int saw_index(float v) { return (int)lrintf((v * 32768.0f + 16384.0f) / 8.0f); }

/* Acts as the audio callback of the plain device for one stream. */
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
    fa_fetch_add64(&g_dev->clock, frames);
    fa_fetch_add64(&g_dev->epoch, 1);
}

/* Renders `frames` and appends the sawtooth indices (left channel) to idx[*n]. */
static void render_saw(fa_stream* s, uint32_t frames, int* idx, int* n)
{
    float out[2 * 512];
    while (frames > 0) {
        uint32_t k = frames < 512 ? frames : 512, i;
        render(s, out, k);
        for (i = 0; i < k; i++) idx[(*n)++] = saw_index(out[i * 2]);
        frames -= k;
    }
}

/* Index of the first place where idx stops counting up by one (mod 4096), from `from`. */
static int first_jump(const int* idx, int n, int from)
{
    int i;
    for (i = from + 1; i < n; i++)
        if (idx[i] != (idx[i - 1] + 1) % 4096) return i;
    return -1;
}

/* ---- events ----------------------------------------------------------------- */

typedef struct recorder {
    fa_mutex lock;
    int looped;
} recorder;

static void FA_CALL record(const fa_event* ev, void* user)
{
    recorder* r = (recorder*)user;
    fa_mutex_lock(&r->lock);
    if (ev->type == FA_EVENT_LOOPED) r->looped++;
    fa_mutex_unlock(&r->lock);
}

static int looped_events(recorder* r, int wait_for)
{
    int n = 0, tries;
    for (tries = 0; tries < 200; tries++) {
        fa_mutex_lock(&r->lock);
        n = r->looped;
        fa_mutex_unlock(&r->lock);
        if (n >= wait_for) break;
        fa_sleep_ms(5);
    }
    return n;
}

/* ---- deck loops (plain device, exact samples) --------------------------------- */

static fa_stream* playing_saw(uint32_t frames)
{
    fa_stream* s = open_wav(frames, 0, 0.0);
    if (!s) return NULL;
    if (fa_stream_attach(s, g_dev, NULL) != FA_OK || fa_stream_play(s) != FA_OK) { fa_stream_close(s); return NULL; }
    return s;
}

/* A loop whose out point the decoder has not reached yet is taken where it decodes. */
static void test_loop_ahead_of_decoder(void)
{
    fa_stream* s = playing_saw(400000);
    int* idx = (int*)malloc(sizeof(int) * 300000), n = 0, j;
    fa_frames pos = 0;
    recorder rec;
    CHECK(s && idx);
    memset(&rec, 0, sizeof(rec));
    fa_mutex_init(&rec.lock);
    fa_stream_set_event_callback(s, record, &rec);
    render_saw(s, 1024, idx, &n);
    CHECK_EQ(fa_stream_set_loop_region(s, 100000, 120000), FA_OK);
    render_saw(s, 140000, idx, &n);
    /* 0..119999 straight, then back to 100000. */
    j = first_jump(idx, n, 0);
    CHECK_EQ(j, 120000);
    CHECK_EQ(idx[j], 100000 % 4096);
    CHECK_EQ(first_jump(idx, n, j), 140000);   /* the second turn */
    CHECK(looped_events(&rec, 2) >= 2);
    fa_stream_get_position(s, &pos);
    CHECK(pos >= 100000 && pos < 120000);
    fa_stream_set_event_callback(s, NULL, NULL);
    fa_stream_close(s);
    fa_mutex_destroy(&rec.lock);
    free(idx);
}

/* An out point inside the decoded-ahead audio but ahead of the mix: joined exactly there. */
static void test_loop_out_inside_ring(void)
{
    fa_stream* s = playing_saw(200000);
    int idx[20000], n = 0, j, i;
    CHECK(s);
    render_saw(s, 2048, idx, &n);
    for (i = 0; i < 2000 && fa_ring_readable(&s->ring) < 8000; i++) fa_sleep_ms(1);
    CHECK(fa_ring_readable(&s->ring) >= 8000);           /* decoded well past the out point */
    CHECK_EQ(fa_stream_set_loop_region(s, 500, 3500), FA_OK);
    render_saw(s, 8000, idx, &n);
    j = first_jump(idx, n, 0);
    CHECK_EQ(j, 3500);
    CHECK_EQ(idx[j], 500);
    CHECK_EQ(first_jump(idx, n, j), 3500 + 3000);
    fa_stream_close(s);
}

/* An out point the mix just passed ("loop out" pressed late): back into the loop at
 * once, at the phase it would have had. */
static void test_loop_out_just_passed(void)
{
    fa_stream* s = playing_saw(200000);
    int idx[20000], n = 0, j, margin = (int)(g_rate * 15 / 1000), want, i;
    CHECK(s);
    render_saw(s, 3000, idx, &n);
    /* The join is a margin ahead of the mix only once the deck has decoded that far. */
    for (i = 0; i < 2000 && fa_ring_readable(&s->ring) < (uint32_t)margin + 512; i++) fa_sleep_ms(1);
    CHECK_EQ(fa_stream_set_loop_region(s, 500, 2800), FA_OK);
    render_saw(s, 6000, idx, &n);
    j = first_jump(idx, n, 0);
    CHECK_EQ(j, 3000 + margin);                       /* the join, a margin ahead of the mix */
    want = 500 + (3000 + margin - 2800) % 2300;
    CHECK_EQ(idx[j], want);
    CHECK_EQ(idx[first_jump(idx, n, j)], 500);         /* and loops at 2800 from then on */
    fa_stream_close(s);
}

/* An out point far behind: to the in point. */
static void test_loop_far_behind_goes_to_in(void)
{
    fa_stream* s = playing_saw(200000);
    int* idx = (int*)malloc(sizeof(int) * 80000), n = 0, j, margin = (int)(g_rate * 15 / 1000), i;
    fa_frames pos = 0;
    CHECK(s && idx);
    render_saw(s, 60000, idx, &n);
    for (i = 0; i < 2000 && fa_ring_readable(&s->ring) < (uint32_t)margin + 512; i++) fa_sleep_ms(1);
    CHECK_EQ(fa_stream_set_loop_region(s, 500, 2500), FA_OK);
    render_saw(s, 4000, idx, &n);
    j = first_jump(idx, n, 0);
    CHECK_EQ(j, 60000 + margin);
    CHECK_EQ(idx[j], 500);
    fa_stream_get_position(s, &pos);
    CHECK(pos >= 500 && pos < 2500);
    fa_stream_close(s);
    free(idx);
}

/* Turning the loop off after it has turned carries on from where it is, past the
 * old out point. */
static void test_loop_off_carries_on(void)
{
    fa_stream* s = playing_saw(200000);
    int idx[30000], n = 0, j, margin = (int)(g_rate * 15 / 1000);
    fa_frames in = -1, out = -1;
    CHECK(s);
    CHECK_EQ(fa_stream_set_loop_region(s, 500, 2500), FA_OK);
    render_saw(s, 3000, idx, &n);                      /* turned once: at 1000 now */
    CHECK_EQ(first_jump(idx, n, 0), 2500);
    CHECK_EQ(fa_stream_set_loop_region(s, 0, 0), FA_OK);
    fa_stream_get_loop_region(s, &in, &out);
    CHECK_EQ(out, 0);
    render_saw(s, 6000, idx, &n);
    /* The join (margin ahead) continues the count, and 2500 passes without a turn. */
    j = first_jump(idx, n, 2600);
    CHECK_EQ(j, -1);
    CHECK_EQ(idx[n - 1], (1000 + 6000 - 1) % 4096);
    (void)margin;
    fa_stream_close(s);
}

/* A seek past the out point leaves the loop. */
static void test_seek_past_loop_leaves_it(void)
{
    fa_stream* s = playing_saw(200000);
    int idx[8000], n = 0;
    fa_frames in = -1, out = -1;
    CHECK(s);
    CHECK_EQ(fa_stream_set_loop_region(s, 500, 2500), FA_OK);
    render_saw(s, 1000, idx, &n);
    CHECK_EQ(fa_stream_seek(s, 50000), FA_OK);
    fa_stream_get_loop_region(s, &in, &out);
    CHECK_EQ(out, 0);
    n = 0;
    render_saw(s, 4000, idx, &n);
    CHECK_EQ(idx[0], 50000 % 4096);
    CHECK_EQ(first_jump(idx, n, 0), -1);
    fa_stream_close(s);
}

/* Loops follow tempo: with the stretcher engaged the position stays in the loop. */
static void test_loop_with_tempo(void)
{
    fa_stream* s = playing_saw(400000);
    float out[2 * 512];
    fa_frames pos = 0;
    int i, inside = 1;
    CHECK(s);
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_TEMPO, 1.25f), FA_OK);
    for (i = 0; i < 40; i++) render(s, out, 512);
    CHECK_EQ(fa_stream_set_loop_region(s, 10000, 30000), FA_OK);
    for (i = 0; i < 400; i++) {
        render(s, out, 512);
        fa_stream_get_position(s, &pos);
        if (i > 100 && (pos < 10000 || pos >= 30000)) inside = 0;
    }
    CHECK(inside);
    fa_stream_close(s);
}

/* ---- the mixer -------------------------------------------------------------- */

typedef struct rig {
    fa_mixer* m;
    fa_device* dev;
    fa_deck* a;
    fa_deck* b;
    uint32_t channels;
} rig;

static int rig_open(rig* r, fa_cue_mode cue)
{
    fa_mixer_config c;
    memset(r, 0, sizeof(*r));
    fa_mixer_config_init(&c);
    c.cue_mode = cue;
    c.latency = FA_LATENCY_NORMAL;
    if (fa_mixer_create(g_engine, &c, &r->m) != FA_OK) return 0;
    fa_mixer_get_device(r->m, &r->dev);
    fa_device_stop(r->dev);   /* this test is the audio thread */
    fa_mixer_get_deck(r->m, 0, &r->a);
    fa_mixer_get_deck(r->m, 1, &r->b);
    fa_mixer_get_output_channels(r->m, &r->channels, NULL);
    return 1;
}

/* Renders `frames` of the mixer's output (all channels) after the decks' workers
 * have that much decoded. */
static void rig_render(rig* r, float* out, uint32_t frames)
{
    int i, d;
    for (i = 0; i < 2000; i++) {
        int ok = 1;
        for (d = 0; d < 2; d++) {
            fa_stream* s = NULL;
            fa_deck_get_stream(d ? r->b : r->a, &s);
            if (s && fa_ring_readable(&s->ring) < (int64_t)frames && !fa_load32(&s->eof)) ok = 0;
        }
        if (ok) break;
        fa_sleep_ms(1);
    }
    fa_fetch_add64(&r->dev->epoch, 1);
    fa_device_render_output(r->dev, out, r->channels, frames, fa_monotonic_ns());
    fa_fetch_add64(&r->dev->epoch, 1);
}

/* RMS of channel `c` over the last `n` frames of `out` (frames long). */
static double rms_of(const float* out, uint32_t channels, uint32_t frames, uint32_t n, uint32_t c)
{
    double sq = 0.0;
    uint32_t i;
    for (i = frames - n; i < frames; i++) sq += (double)out[i * channels + c] * out[i * channels + c];
    return sqrt(sq / n);
}

/* Plays a sine of `hz` on deck A and returns the steady RMS of output channel `c`. */
static double sine_rms(rig* r, uint32_t c, float* buf, uint32_t frames)
{
    uint32_t done;
    for (done = 0; done + 1024 <= frames; done += 1024) rig_render(r, buf + (size_t)done * r->channels, 1024);
    return rms_of(buf, r->channels, frames, frames / 2, c);
}

static fa_stream* load_sine(fa_deck* d, double hz)
{
    fa_stream* s = open_wav(g_rate * 4, 1, hz);
    if (!s) return NULL;
    if (fa_deck_load(d, s, 0) != FA_OK || fa_stream_play(s) != FA_OK) { fa_stream_close(s); return NULL; }
    return s;
}

static const double k_sine_rms = 0.25 / 1.41421356;   /* the test sine's own RMS */

/* At 0 dB the isolator's bands sum flat; the crossfader at A passes deck A whole. */
static void test_isolator_flat(void)
{
    static const double hz[] = { 60.0, 250.0, 1000.0, 2500.0, 8000.0 };
    rig r;
    float* buf = (float*)malloc(sizeof(float) * 2 * 24576);
    int i;
    CHECK(buf && rig_open(&r, FA_CUE_NONE));
    CHECK_EQ(r.channels, 2);
    CHECK_EQ(fa_mixer_set_param(r.m, FA_MIX_CROSSFADER, 0.0f), FA_OK);
    for (i = 0; i < 5; i++) {
        fa_stream* s = load_sine(r.a, hz[i]);
        CHECK(s);
        CHECK_NEAR(sine_rms(&r, 0, buf, 24576) / k_sine_rms, 1.0, 0.01);   /* within 0.1 dB */
        CHECK_EQ(fa_deck_load(r.a, NULL, FA_DECK_LOAD_FORCE), FA_OK);
        fa_stream_close(s);
    }
    fa_mixer_destroy(r.m);
    free(buf);
}

/* Kills: a killed band is gone, the others stay. */
static void test_isolator_kills(void)
{
    rig r;
    float* buf = (float*)malloc(sizeof(float) * 2 * 24576);
    fa_stream* s;
    CHECK(buf && rig_open(&r, FA_CUE_NONE));
    fa_mixer_set_param(r.m, FA_MIX_CROSSFADER, 0.0f);
    CHECK_EQ(fa_deck_set_param(r.a, FA_DECK_EQ_LOW_DB, -100.0f), FA_OK);   /* clamped to the kill */
    s = load_sine(r.a, 60.0);
    CHECK(s);
    CHECK(sine_rms(&r, 0, buf, 24576) / k_sine_rms < 0.01);           /* below -40 dB */
    fa_deck_load(r.a, NULL, FA_DECK_LOAD_FORCE);
    fa_stream_close(s);
    s = load_sine(r.a, 8000.0);                                              /* the highs untouched */
    CHECK(s);
    CHECK_NEAR(sine_rms(&r, 0, buf, 24576) / k_sine_rms, 1.0, 0.02);
    CHECK_EQ(fa_deck_set_param(r.a, FA_DECK_EQ_HIGH_DB, FA_DECK_EQ_KILL_DB), FA_OK);
    CHECK(sine_rms(&r, 0, buf, 24576) / k_sine_rms < 0.01);
    CHECK_EQ(fa_deck_set_param(r.a, FA_DECK_EQ_HIGH_DB, 6.0f), FA_OK);       /* boost: +6 dB */
    CHECK_NEAR(sine_rms(&r, 0, buf, 24576) / k_sine_rms, 1.995, 0.05);
    fa_deck_load(r.a, NULL, FA_DECK_LOAD_FORCE);
    fa_stream_close(s);
    fa_mixer_destroy(r.m);
    free(buf);
}

/* Crossfader curves, sides, trim, fader and master gain. */
static void test_gains(void)
{
    rig r;
    float* buf = (float*)malloc(sizeof(float) * 2 * 24576);
    float v = 0.0f;
    fa_stream* s;
    fa_xfader_curve curve;
    CHECK(buf && rig_open(&r, FA_CUE_NONE));
    s = load_sine(r.a, 1000.0);
    CHECK(s);
    CHECK_NEAR(sine_rms(&r, 0, buf, 24576) / k_sine_rms, 0.7071, 0.01);   /* middle, constant power */
    CHECK_EQ(fa_mixer_set_crossfader_curve(r.m, FA_XFADER_LINEAR), FA_OK);
    CHECK_EQ(fa_mixer_get_crossfader_curve(r.m, &curve), FA_OK);
    CHECK_EQ(curve, FA_XFADER_LINEAR);
    CHECK_NEAR(sine_rms(&r, 0, buf, 24576) / k_sine_rms, 0.5, 0.01);
    /* A curve change glides over 5 ms: the first millisecond stays near linear's 0.5
     * instead of stepping to constant power's 0.707. */
    CHECK_EQ(fa_mixer_set_crossfader_curve(r.m, FA_XFADER_CONSTANT_POWER), FA_OK);
    rig_render(&r, buf, 1024);
    CHECK(rms_of(buf, r.channels, g_rate / 1000, g_rate / 1000, 0) / k_sine_rms < 0.6);
    CHECK_NEAR(rms_of(buf, r.channels, 1024, 512, 0) / k_sine_rms, 0.7071, 0.02);
    CHECK_EQ(fa_mixer_set_crossfader_curve(r.m, FA_XFADER_LINEAR), FA_OK);
    fa_mixer_set_param(r.m, FA_MIX_CROSSFADER, 1.0f);                              /* all the way to B */
    CHECK(sine_rms(&r, 0, buf, 24576) / k_sine_rms < 0.001);
    CHECK_EQ(fa_deck_set_param(r.a, FA_DECK_XFADER_SIDE, 0.0f), FA_OK);             /* thru */
    CHECK_NEAR(sine_rms(&r, 0, buf, 24576) / k_sine_rms, 1.0, 0.01);
    CHECK_EQ(fa_deck_get_param(r.a, FA_DECK_XFADER_SIDE, &v), FA_OK);
    CHECK_EQ((int)v, 0);
    CHECK_EQ(fa_deck_set_param(r.a, FA_DECK_XFADER_SIDE, 1.5f), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_deck_set_param(r.a, FA_DECK_TRIM_DB, 6.0f), FA_OK);
    CHECK_NEAR(sine_rms(&r, 0, buf, 24576) / k_sine_rms, 1.995, 0.03);
    CHECK_EQ(fa_deck_set_param(r.a, FA_DECK_GAIN, 0.5f), FA_OK);
    CHECK_NEAR(sine_rms(&r, 0, buf, 24576) / k_sine_rms, 0.998, 0.02);
    CHECK_EQ(fa_mixer_set_param(r.m, FA_MIX_MASTER_GAIN, 0.5f), FA_OK);
    CHECK_NEAR(sine_rms(&r, 0, buf, 24576) / k_sine_rms, 0.499, 0.01);
    CHECK_EQ(fa_mixer_get_param(r.m, FA_MIX_MASTER_GAIN, &v), FA_OK);
    CHECK_NEAR(v, 0.5, 1e-6);
    CHECK_EQ(fa_deck_ramp_param(r.a, FA_DECK_XFADER_SIDE, 1.0f, 0, 0, NULL), FA_ERROR_INVALID_ARG);
    fa_deck_load(r.a, NULL, FA_DECK_LOAD_FORCE);
    fa_stream_close(s);
    fa_mixer_destroy(r.m);
    free(buf);
}

/* A crossfader ramp runs on the device clock: half way through, the middle. */
static void test_crossfader_ramp(void)
{
    rig r;
    float* buf = (float*)malloc(sizeof(float) * 2 * 48000);
    float v = 0.0f;
    fa_stream* s;
    uint32_t done;
    CHECK(buf && rig_open(&r, FA_CUE_NONE));
    fa_mixer_set_param(r.m, FA_MIX_CROSSFADER, 0.0f);
    s = load_sine(r.a, 1000.0);
    CHECK(s);
    rig_render(&r, buf, 1024);
    CHECK_EQ(fa_mixer_ramp_param(r.m, FA_MIX_CROSSFADER, 1.0f, 1000000000, 0, NULL), FA_OK);
    for (done = 0; done < g_rate / 2; done += 512) rig_render(&r, buf, 512);
    CHECK_EQ(fa_mixer_get_param(r.m, FA_MIX_CROSSFADER, &v), FA_OK);
    CHECK_NEAR(v, 0.5, 0.02);
    for (done = 0; done < g_rate * 6 / 10; done += 512) rig_render(&r, buf, 512);
    CHECK_EQ(fa_mixer_get_param(r.m, FA_MIX_CROSSFADER, &v), FA_OK);
    CHECK_NEAR(v, 1.0, 1e-6);
    fa_deck_load(r.a, NULL, FA_DECK_LOAD_FORCE);
    fa_stream_close(s);
    fa_mixer_destroy(r.m);
    free(buf);
}

/* Channels 3-4: the cue is the pre-fader deck, and CUE_MIX blends in the master. */
static void test_cue_channels(void)
{
    rig r;
    float* buf = (float*)malloc(sizeof(float) * 4 * 24576);
    fa_stream* s;
    int32_t cue_ch = -1;
    CHECK(buf && rig_open(&r, FA_CUE_CHANNELS));
    CHECK_EQ(r.channels, 4);
    fa_mixer_get_output_channels(r.m, NULL, &cue_ch);
    CHECK_EQ(cue_ch, 2);
    s = load_sine(r.a, 1000.0);
    CHECK(s);
    fa_mixer_set_param(r.m, FA_MIX_CROSSFADER, 0.0f);
    fa_deck_set_param(r.a, FA_DECK_GAIN, 0.0f);                 /* fader down: silent in the room */
    CHECK_EQ(fa_deck_set_param(r.a, FA_DECK_CUE, 1.0f), FA_OK);
    sine_rms(&r, 0, buf, 24576);
    CHECK(rms_of(buf, 4, 24576, 12288, 0) < 1e-6);
    CHECK_NEAR(rms_of(buf, 4, 24576, 12288, 2) / k_sine_rms, 1.0, 0.01);   /* but in the headphones */
    CHECK_NEAR(rms_of(buf, 4, 24576, 12288, 3) / k_sine_rms, 1.0, 0.01);
    fa_device_set_mute(r.dev, 1);                               /* the device mute takes the cue too */
    sine_rms(&r, 0, buf, 24576);
    CHECK(rms_of(buf, 4, 24576, 12288, 2) < 1e-6);
    fa_device_set_mute(r.dev, 0);
    sine_rms(&r, 0, buf, 24576);
    CHECK_NEAR(rms_of(buf, 4, 24576, 12288, 2) / k_sine_rms, 1.0, 0.01);
    fa_deck_set_param(r.a, FA_DECK_CUE, 0.0f);
    fa_deck_set_param(r.a, FA_DECK_GAIN, 1.0f);
    fa_mixer_set_param(r.m, FA_MIX_CUE_MIX, 1.0f);              /* the master in the headphones */
    fa_mixer_set_param(r.m, FA_MIX_CUE_GAIN, 0.5f);
    sine_rms(&r, 0, buf, 24576);
    CHECK_NEAR(rms_of(buf, 4, 24576, 12288, 0) / k_sine_rms, 1.0, 0.01);
    CHECK_NEAR(rms_of(buf, 4, 24576, 12288, 2) / k_sine_rms, 0.5, 0.01);
    fa_deck_load(r.a, NULL, FA_DECK_LOAD_FORCE);
    fa_stream_close(s);
    fa_mixer_destroy(r.m);
    free(buf);
}

/* Split cue: the master in mono on the left, the cue in mono on the right. */
static void test_cue_split(void)
{
    rig r;
    float* buf = (float*)malloc(sizeof(float) * 2 * 24576);
    fa_stream* sa;
    fa_stream* sb;
    CHECK(buf && rig_open(&r, FA_CUE_SPLIT));
    CHECK_EQ(r.channels, 2);
    sa = load_sine(r.a, 1000.0);
    sb = load_sine(r.b, 1000.0);
    CHECK(sa && sb);
    fa_mixer_set_param(r.m, FA_MIX_CROSSFADER, 0.0f);   /* A in the room */
    fa_deck_set_param(r.b, FA_DECK_CUE, 1.0f);            /* B in the headphones */
    fa_deck_set_param(r.b, FA_DECK_GAIN, 0.5f);           /* the cue is before the fader */
    sine_rms(&r, 0, buf, 24576);
    CHECK_NEAR(rms_of(buf, 2, 24576, 12288, 0) / k_sine_rms, 1.0, 0.01);
    CHECK_NEAR(rms_of(buf, 2, 24576, 12288, 1) / k_sine_rms, 1.0, 0.01);
    CHECK_EQ(fa_deck_load(r.a, NULL, 0), FA_ERROR_BUSY);  /* A is playing and heard */
    CHECK_EQ(fa_deck_load(r.b, NULL, 0), FA_OK);           /* B is not heard in the room */
    CHECK_EQ(fa_deck_load(r.a, NULL, FA_DECK_LOAD_FORCE), FA_OK);
    fa_stream_close(sa);
    fa_stream_close(sb);
    fa_mixer_destroy(r.m);
    free(buf);
}

/* Deck meters: after the fader, before the crossfader. */
static void test_deck_meter(void)
{
    rig r;
    float* buf = (float*)malloc(sizeof(float) * 2 * 1024);
    float peak = 0.0f, rms = 0.0f;
    fa_stream* s;
    uint32_t done;
    CHECK(buf && rig_open(&r, FA_CUE_NONE));
    s = load_sine(r.a, 1000.0);
    CHECK(s);
    CHECK_EQ(fa_deck_get_levels(r.a, 50000000, &peak, &rms, 1), FA_OK);   /* starts the meter */
    fa_mixer_set_param(r.m, FA_MIX_CROSSFADER, 1.0f);                       /* closed on A: the meter still reads */
    fa_deck_set_param(r.a, FA_DECK_GAIN, 0.5f);
    for (done = 0; done < g_rate; done += 1024) rig_render(&r, buf, 1024);
    CHECK_EQ(fa_deck_get_levels(r.a, 50000000, &peak, &rms, 1), FA_OK);
    CHECK_NEAR(rms / k_sine_rms, 0.5, 0.02);
    CHECK_NEAR(peak, 0.125, 0.01);
    fa_deck_load(r.a, NULL, FA_DECK_LOAD_FORCE);
    fa_stream_close(s);
    fa_mixer_destroy(r.m);
    free(buf);
}

/* Loading moves a stream between decks; destroying the mixer leaves streams open;
 * closing the mixer's device destroys the mixer. Long callbacks run in blocks. */
static void test_decks_and_lifetime(void)
{
    rig r;
    float* buf = (float*)malloc(sizeof(float) * 2 * 5000);
    fa_stream* s = open_wav(g_rate, 1, 1000.0);
    fa_stream* got = NULL;
    fa_frames cue = -1, in = -1, out = -1;
    fa_stream_state st;
    CHECK(buf && s && rig_open(&r, FA_CUE_NONE));
    CHECK_EQ(fa_deck_load(r.a, s, 0), FA_OK);
    CHECK_EQ(fa_deck_load(r.b, s, 0), FA_OK);              /* moves to B */
    CHECK_EQ(fa_deck_get_stream(r.a, &got), FA_OK);
    CHECK(got == NULL);
    CHECK_EQ(fa_deck_get_stream(r.b, &got), FA_OK);
    CHECK(got == s);
    CHECK(fa_load_ptr(&s->bus) != NULL);
    CHECK_EQ(fa_deck_set_cue_point(r.b, 1000), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    rig_render(&r, buf, 5000);                              /* more than a block */
    CHECK_EQ(fa_deck_cue(r.b), FA_OK);
    fa_stream_get_state(s, &st);
    CHECK_EQ(st, FA_STATE_PAUSED);
    CHECK_EQ(fa_deck_get_cue_point(r.b, &cue), FA_OK);
    CHECK_EQ(cue, 1000);
    CHECK_EQ(fa_deck_set_loop(r.b, 2000, 9000), FA_OK);
    CHECK_EQ(fa_deck_get_loop(r.b, &in, &out), FA_OK);
    CHECK_EQ(in, 2000);
    CHECK_EQ(out, 9000);
    CHECK_EQ(fa_deck_set_loop(r.a, 2000, 9000), FA_ERROR_NOT_FOUND);   /* empty deck */
    /* A stream that leaves its deck (moved elsewhere, or closed) leaves it empty. */
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_deck_get_stream(r.b, &got), FA_OK);
    CHECK(got == NULL);
    CHECK_EQ(fa_deck_load(r.b, s, 0), FA_OK);
    {
        fa_stream* s2 = open_wav(g_rate, 1, 500.0);
        CHECK(s2);
        CHECK_EQ(fa_deck_load(r.a, s2, 0), FA_OK);
        {
            fa_stream* s3 = open_wav(g_rate, 1, 500.0);   /* a deck holds one stream */
            CHECK(s3);
            CHECK_EQ(fa_crossfade(s2, s3, 1000000000LL, NULL), FA_ERROR_INVALID_ARG);
            CHECK_EQ(fa_stream_set_next(s2, s3, 0, NULL), FA_ERROR_INVALID_ARG);
            CHECK(s3->device == NULL);
            fa_stream_close(s3);
        }
        fa_stream_close(s2);
        CHECK_EQ(fa_deck_get_stream(r.a, &got), FA_OK);
        CHECK(got == NULL);
    }
    fa_stream_set_loop_region(s, 2000, 9000);
    fa_mixer_destroy(r.m);
    CHECK(s->device == NULL);                                   /* detached, still open */
    fa_stream_get_loop_region(s, &in, &out);
    CHECK_EQ(out, 0);
    CHECK(rig_open(&r, FA_CUE_NONE));
    CHECK_EQ(fa_deck_load(r.a, s, 0), FA_OK);
    fa_device_close(r.dev);                                     /* takes the mixer with it */
    CHECK(s->device == NULL);
    fa_stream_close(s);
    free(buf);
}

static void test_config_errors(void)
{
    fa_mixer_config c;
    fa_mixer* m = NULL;
    fa_mixer_config_init(&c);
    CHECK_EQ(c.latency, FA_LATENCY_LOW);
    c.deck_count = 4;
    CHECK_EQ(fa_mixer_create(g_engine, &c, &m), FA_ERROR_NOT_IMPLEMENTED);
    fa_mixer_config_init(&c);
    c.cue_mode = FA_CUE_DEVICE;
    c.cue_device_id = "zz";      /* not hex */
#if defined(__APPLE__) && TARGET_OS_IPHONE
    CHECK_EQ(fa_mixer_create(g_engine, &c, &m), FA_ERROR_NOT_IMPLEMENTED);   /* no second output on iOS */
#else
    CHECK_EQ(fa_mixer_create(g_engine, &c, &m), FA_ERROR_INVALID_ARG);
#endif
    fa_mixer_config_init(&c);
    c.flags = 2;
    CHECK_EQ(fa_mixer_create(g_engine, &c, &m), FA_ERROR_INVALID_ARG);
    CHECK(m == NULL);
    CHECK(strcmp(fa_result_string(FA_ERROR_NOT_ENOUGH_CHANNELS), "unknown error") != 0);
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

    RUN(test_loop_ahead_of_decoder);
    RUN(test_loop_out_inside_ring);
    RUN(test_loop_out_just_passed);
    RUN(test_loop_far_behind_goes_to_in);
    RUN(test_loop_off_carries_on);
    RUN(test_seek_past_loop_leaves_it);
    RUN(test_loop_with_tempo);
    RUN(test_isolator_flat);
    RUN(test_isolator_kills);
    RUN(test_gains);
    RUN(test_crossfader_ramp);
    RUN(test_cue_channels);
    RUN(test_cue_split);
    RUN(test_deck_meter);
    RUN(test_decks_and_lifetime);
    RUN(test_config_errors);

    fa_device_close(g_dev);
    fa_engine_destroy(g_engine);
    if (g_failures) { fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    printf("all mixer tests passed\n");
    return 0;
}
