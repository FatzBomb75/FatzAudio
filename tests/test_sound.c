/* White-box tests of sounds (fa_sound_*): overlapping voices, the voice limit and
 * the steal rules, voice reuse and the shared PCM. The device is opened on the null
 * backend but never started: the test plays the audio thread by calling
 * fa_device_render, as test_crossfade does. */
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

#define PERIOD 512
#define MAX_REC (1 << 16)

static fa_engine* g_engine;
static fa_device* g_dev;
static float* g_rec;
static int64_t g_rec_frames;

/* A mono float sound at the device rate (no resampling, so samples come out exact):
 * every sample `level`. */
static fa_sound* make_sound(uint32_t frames, float level, uint32_t max_voices, int32_t steal)
{
    float* pcm = (float*)malloc(frames * sizeof(float));
    fa_audio_format f;
    fa_sound_options o;
    fa_sound* snd = NULL;
    uint32_t i;
    for (i = 0; i < frames; i++) pcm[i] = level;
    memset(&f, 0, sizeof(f));
    f.sample_format = FA_FORMAT_F32;
    f.sample_rate = g_dev->rate;
    f.channels = 1;
    fa_sound_options_init(&o);
    o.max_voices = max_voices;
    o.steal = steal;
    if (fa_sound_load_pcm(g_engine, &f, pcm, frames, &o, &snd) != FA_OK) snd = NULL;
    free(pcm);
    return snd;
}

/* Renders `frames` frames in callbacks of PERIOD into g_rec (from 0), after letting the
 * workers fill every attached stream, as they would in real time. */
static void render(int64_t frames)
{
    float out[PERIOD * 2];
    int64_t d = g_dev->limiter.delay;
    g_rec_frames = 0;
    memset(g_rec, 0, (size_t)MAX_REC * 2 * sizeof(float));
    frames += d;
    while (frames > 0) {
        uint32_t n = frames < PERIOD ? (uint32_t)frames : PERIOD, i;
        fa_voice_list* list = (fa_voice_list*)fa_load_ptr(&g_dev->voices);
        int64_t deadline = fa_monotonic_ns() + 10000000000LL;   /* a bound for a hang, not a wait */
        while (list && fa_monotonic_ns() < deadline) {
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
            int64_t at = g_rec_frames + i - d;
            if (at >= 0 && at < MAX_REC) { g_rec[at * 2] = out[i * 2]; g_rec[at * 2 + 1] = out[i * 2 + 1]; }
        }
        g_rec_frames += n;
        frames -= n;
    }
}

static float L(int64_t f) { return g_rec[f * 2]; }
static float R(int64_t f) { return g_rec[f * 2 + 1]; }

static int state_of(fa_stream* s)
{
    fa_stream_state st = FA_STATE_STOPPED;
    fa_stream_get_state(s, &st);
    return (int)st;
}

/* The level one copy at volume 1, centre, gives on the left. */
static float unit_level(void)
{
    fa_sound* snd = make_sound(4096, 0.25f, 1, FA_SOUND_STEAL_OLDEST);
    float v;
    if (!snd) return 0.0f;
    fa_sound_play(snd, g_dev, NULL, 1.0f, 0.0f, NULL);
    render(2048);
    v = L(1000);
    fa_sound_close(snd);
    return v;
}

/* ---- tests -------------------------------------------------------------------- */

static void test_overlapping_copies_sum(void)
{
    float unit = unit_level();
    fa_sound* snd = make_sound(8192, 0.25f, 8, FA_SOUND_STEAL_OLDEST);
    fa_stream* a = NULL; fa_stream* b = NULL; fa_stream* c = NULL;
    fa_sound_info info;
    CHECK(unit > 0.05f);
    CHECK(snd != NULL);
    CHECK_EQ(fa_sound_play(snd, g_dev, NULL, 1.0f, 0.0f, &a), FA_OK);
    CHECK_EQ(fa_sound_play(snd, g_dev, NULL, 0.5f, 0.0f, &b), FA_OK);
    CHECK_EQ(fa_sound_play(snd, g_dev, NULL, 0.25f, 0.0f, &c), FA_OK);
    CHECK(a && b && c && a != b && b != c && a != c);
    render(4096);
    /* All three start in the same callback, each at its own level from the first frame. */
    CHECK_NEAR(L(0), unit * 1.75f, 1e-5);
    CHECK_NEAR(L(2000), unit * 1.75f, 1e-5);
    CHECK_NEAR(R(2000), unit * 1.75f, 1e-5);
    CHECK_EQ(fa_sound_get_info(snd, &info), FA_OK);
    CHECK_EQ(info.voices, 3);
    CHECK_EQ(info.playing, 3);
    CHECK_EQ(info.frames, 8192);
    CHECK_EQ(info.memory_bytes, 8192 * 4);
    /* A copy started later adds from its own first frame. */
    CHECK_EQ(fa_sound_stop(snd), FA_OK);
    CHECK_EQ(fa_sound_play(snd, g_dev, NULL, 1.0f, -1.0f, NULL), FA_OK);
    render(1024);
    CHECK(L(500) >= unit * 0.99f);   /* pan hard left: all of it on the left */
    CHECK_NEAR(R(500), 0.0f, 1e-6);
    fa_sound_close(snd);
}

static void test_memory_shared(void)
{
    fa_sound* snd = make_sound(4096, 0.1f, 4, FA_SOUND_STEAL_OLDEST);
    fa_stream* v[4];
    const float* data[4];
    int32_t refs = 0;
    int i;
    CHECK(snd != NULL);
    for (i = 0; i < 4; i++) {
        CHECK_EQ(fa_sound_play(snd, g_dev, NULL, 0.2f, 0.0f, &v[i]), FA_OK);
        data[i] = fa_sound_voice_data(v[i], &refs);
        CHECK(data[i] != NULL);
        CHECK(data[i] == data[0]);
    }
    CHECK_EQ(refs, 5);   /* the sound and its four voices: one copy of the PCM */
    /* A voice the app closes lets go of its reference; the sound forgets it. */
    fa_stream_close(v[3]);
    {
        fa_sound_info info;
        CHECK_EQ(fa_sound_get_info(snd, &info), FA_OK);
        CHECK_EQ(info.voices, 3);
    }
    fa_sound_voice_data(v[0], &refs);
    CHECK_EQ(refs, 4);
    fa_sound_close(snd);
}

static void test_steal_oldest(void)
{
    fa_sound* snd = make_sound(20000, 0.1f, 2, FA_SOUND_STEAL_OLDEST);
    fa_stream* a = NULL; fa_stream* b = NULL; fa_stream* c = NULL; fa_stream* d = NULL;
    fa_stream* list[4];
    uint32_t n = 0;
    CHECK(snd != NULL);
    CHECK_EQ(fa_sound_play(snd, g_dev, NULL, 1.0f, 0.0f, &a), FA_OK);
    render(1024);
    CHECK_EQ(fa_sound_play(snd, g_dev, NULL, 1.0f, 0.0f, &b), FA_OK);
    render(1024);
    CHECK_EQ(fa_sound_play(snd, g_dev, NULL, 1.0f, 0.0f, &c), FA_OK);
    CHECK(c == a);             /* the oldest restarts as the new copy */
    {
        fa_frames pos = -1;
        fa_stream_get_position(c, &pos);
        CHECK(pos < 1024);     /* from the start again */
    }
    render(512);
    CHECK_EQ(fa_sound_play(snd, g_dev, NULL, 1.0f, 0.0f, &d), FA_OK);
    CHECK(d == b);             /* now b is the oldest */
    CHECK_EQ(fa_sound_get_voices(snd, list, 4, &n), FA_OK);
    CHECK_EQ(n, 2);
    CHECK(list[0] == a && list[1] == b);   /* oldest first */
    CHECK_EQ(fa_sound_get_voices(snd, NULL, 0, &n), FA_OK);
    CHECK_EQ(n, 2);
    fa_sound_close(snd);
}

static void test_steal_quietest(void)
{
    fa_sound* snd = make_sound(20000, 0.1f, 3, FA_SOUND_STEAL_QUIETEST);
    fa_stream* a = NULL; fa_stream* b = NULL; fa_stream* c = NULL; fa_stream* d = NULL; fa_stream* e = NULL;
    CHECK(snd != NULL);
    CHECK_EQ(fa_sound_play(snd, g_dev, NULL, 1.0f, 0.0f, &a), FA_OK);
    CHECK_EQ(fa_sound_play(snd, g_dev, NULL, 0.2f, 0.0f, &b), FA_OK);
    CHECK_EQ(fa_sound_play(snd, g_dev, NULL, 0.6f, 0.0f, &c), FA_OK);
    render(512);
    CHECK_EQ(fa_sound_play(snd, g_dev, NULL, 0.9f, 0.0f, &d), FA_OK);
    CHECK(d == b);             /* the quietest (0.2) */
    render(512);
    /* The app turned one down meanwhile: the volume now counts. */
    CHECK_EQ(fa_stream_set_param(a, FA_PARAM_VOLUME, 0.1f), FA_OK);
    CHECK_EQ(fa_sound_play(snd, g_dev, NULL, 0.5f, 0.0f, &e), FA_OK);
    CHECK(e == a);
    fa_sound_close(snd);
}

static void test_steal_none(void)
{
    fa_sound* snd = make_sound(20000, 0.1f, 2, FA_SOUND_STEAL_NONE);
    fa_stream* a = NULL; fa_stream* c = NULL;
    CHECK(snd != NULL);
    CHECK_EQ(fa_sound_play(snd, g_dev, NULL, 1.0f, 0.0f, &a), FA_OK);
    CHECK_EQ(fa_sound_play(snd, g_dev, NULL, 1.0f, 0.0f, NULL), FA_OK);
    CHECK_EQ(fa_sound_play(snd, g_dev, NULL, 1.0f, 0.0f, &c), FA_ERROR_BUSY);
    CHECK(c == NULL);
    /* A paused copy still holds its voice; a stopped one frees it. */
    CHECK_EQ(fa_stream_pause(a), FA_OK);
    CHECK_EQ(fa_sound_play(snd, g_dev, NULL, 1.0f, 0.0f, NULL), FA_ERROR_BUSY);
    CHECK_EQ(fa_stream_stop(a), FA_OK);
    CHECK_EQ(fa_sound_play(snd, g_dev, NULL, 1.0f, 0.0f, &c), FA_OK);
    CHECK(c == a);
    fa_sound_close(snd);
}

static void test_ended_voice_reused(void)
{
    fa_sound* snd = make_sound(1000, 0.3f, 4, FA_SOUND_STEAL_OLDEST);
    fa_stream* a = NULL; fa_stream* b = NULL;
    fa_sound_info info;
    int i;
    CHECK(snd != NULL);
    CHECK_EQ(fa_sound_play(snd, g_dev, NULL, 1.0f, 0.0f, &a), FA_OK);
    CHECK_EQ(fa_stream_set_loop(a, 1), FA_OK);   /* the app looped it: a new copy plays once */
    render(1500);
    CHECK(L(1200) != 0.0f);
    CHECK_EQ(fa_stream_stop(a), FA_OK);
    CHECK_EQ(fa_sound_play(snd, g_dev, NULL, 0.5f, 0.0f, &b), FA_OK);
    CHECK(b == a);
    for (i = 0; i < 20 && state_of(b) != FA_STATE_ENDED; i++) render(1024);
    CHECK_EQ(state_of(b), FA_STATE_ENDED);
    CHECK_EQ(fa_sound_play(snd, g_dev, NULL, 1.0f, 0.0f, &b), FA_OK);
    CHECK(b == a);
    render(1500);
    CHECK(L(500) != 0.0f);
    CHECK_NEAR(L(1100), 0.0f, 1e-6);   /* played once, then silence */
    CHECK_EQ(fa_sound_get_info(snd, &info), FA_OK);
    CHECK_EQ(info.voices, 1);
    fa_sound_close(snd);
}

static void test_lower_limit(void)
{
    fa_sound* snd = make_sound(20000, 0.1f, 4, FA_SOUND_STEAL_OLDEST);
    fa_sound_options o;
    fa_sound_info info;
    fa_stream* v[4];
    fa_stream* list[4];
    fa_frames pos = 0;
    uint32_t n = 0;
    int i;
    CHECK(snd != NULL);
    for (i = 0; i < 4; i++) { CHECK_EQ(fa_sound_play(snd, g_dev, NULL, 1.0f, 0.0f, &v[i]), FA_OK); render(256); }
    CHECK_EQ(fa_stream_stop(v[2]), FA_OK);
    fa_sound_options_init(&o);
    o.max_voices = 2;
    CHECK_EQ(fa_sound_set_options(snd, &o), FA_OK);
    /* The free voice goes first, then the oldest busy one. */
    CHECK_EQ(fa_sound_get_voices(snd, list, 4, &n), FA_OK);
    CHECK_EQ(n, 2);
    CHECK(list[0] == v[1] && list[1] == v[3]);
    CHECK_EQ(fa_sound_get_info(snd, &info), FA_OK);
    CHECK_EQ(info.voices, 2);
    CHECK_EQ(info.max_voices, 2);
    /* The retired ones are silent but still valid. */
    CHECK_EQ(state_of(v[0]), FA_STATE_STOPPED);
    CHECK_EQ(fa_stream_get_position(v[0], &pos), FA_OK);
    render(512);
    CHECK_EQ(fa_sound_play(snd, g_dev, NULL, 1.0f, 0.0f, &list[2]), FA_OK);
    CHECK(list[2] == v[1]);   /* the oldest of the two left */
    o.max_voices = FA_SOUND_MAX_VOICES + 1;
    CHECK_EQ(fa_sound_set_options(snd, &o), FA_ERROR_INVALID_ARG);
    fa_sound_close(snd);
}

static void test_load_formats(void)
{
    /* 16-bit stereo PCM, and the same as a WAV file in memory. */
    int16_t pcm[2 * 100];
    uint8_t wav[44 + sizeof(pcm)];
    fa_audio_format f;
    fa_sound* a = NULL; fa_sound* b = NULL;
    fa_sound_info ia, ib;
    uint32_t i, v;
    for (i = 0; i < 100; i++) { pcm[i * 2] = (int16_t)(i * 100); pcm[i * 2 + 1] = (int16_t)-(int)(i * 100); }
    memset(&f, 0, sizeof(f));
    f.sample_format = FA_FORMAT_S16;
    f.sample_rate = 22050;
    f.channels = 2;
    CHECK_EQ(fa_sound_load_pcm(g_engine, &f, pcm, 100, NULL, &a), FA_OK);
    memcpy(wav, "RIFF", 4); v = (uint32_t)sizeof(wav) - 8; memcpy(wav + 4, &v, 4); memcpy(wav + 8, "WAVEfmt ", 8);
    v = 16; memcpy(wav + 16, &v, 4);
    wav[20] = 1; wav[21] = 0; wav[22] = 2; wav[23] = 0;
    v = 22050; memcpy(wav + 24, &v, 4); v = 22050 * 4; memcpy(wav + 28, &v, 4);
    wav[32] = 4; wav[33] = 0; wav[34] = 16; wav[35] = 0;
    memcpy(wav + 36, "data", 4); v = (uint32_t)sizeof(pcm); memcpy(wav + 40, &v, 4);
    memcpy(wav + 44, pcm, sizeof(pcm));
    CHECK_EQ(fa_sound_load_memory(g_engine, wav, sizeof(wav), NULL, &b), FA_OK);
    CHECK_EQ(fa_sound_get_info(a, &ia), FA_OK);
    CHECK_EQ(fa_sound_get_info(b, &ib), FA_OK);
    CHECK_EQ(ia.sample_rate, 22050);
    CHECK_EQ(ib.sample_rate, 22050);
    CHECK_EQ(ia.channels, 2);
    CHECK_EQ(ib.channels, 2);
    CHECK_EQ(ia.frames, 100);
    CHECK_EQ(ib.frames, 100);
    CHECK_EQ(ia.max_voices, 8);
    CHECK_EQ(ia.steal, FA_SOUND_STEAL_OLDEST);
    {
        fa_stream* va = NULL; fa_stream* vb = NULL;
        const float* da; const float* db;
        CHECK_EQ(fa_sound_play(a, g_dev, NULL, 1.0f, 0.0f, &va), FA_OK);
        CHECK_EQ(fa_sound_play(b, g_dev, NULL, 1.0f, 0.0f, &vb), FA_OK);
        da = fa_sound_voice_data(va, NULL);
        db = fa_sound_voice_data(vb, NULL);
        CHECK(da && db);
        CHECK(memcmp(da, db, 200 * sizeof(float)) == 0);
        CHECK_NEAR(da[2 * 99], 9900.0 / 32768.0, 1e-6);
        CHECK_NEAR(da[2 * 99 + 1], -9900.0 / 32768.0, 1e-6);
    }
    fa_sound_close(a);
    fa_sound_close(b);
    /* Bad arguments. */
    f.sample_format = FA_FORMAT_S16_PLANAR;
    CHECK_EQ(fa_sound_load_pcm(g_engine, &f, pcm, 100, NULL, &a), FA_ERROR_INVALID_ARG);
    CHECK(a == NULL);
    CHECK_EQ(fa_sound_load_memory(g_engine, "nothing", 7, NULL, &a), FA_ERROR_UNSUPPORTED_FORMAT);
    CHECK_EQ(fa_sound_load_path(g_engine, "no-such-file.wav", NULL, &a), FA_ERROR_NOT_FOUND);
}

static void test_bad_play_args(void)
{
    fa_sound* snd = make_sound(1000, 0.1f, 1, FA_SOUND_STEAL_OLDEST);
    CHECK(snd != NULL);
    CHECK_EQ(fa_sound_play(snd, NULL, NULL, 1.0f, 0.0f, NULL), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_sound_play(snd, g_dev, NULL, -1.0f, 0.0f, NULL), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_sound_play(snd, g_dev, NULL, NAN, 0.0f, NULL), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_sound_play(snd, g_dev, NULL, 1.0f, NAN, NULL), FA_ERROR_INVALID_ARG);
    {
        fa_sound_info info;
        CHECK_EQ(fa_sound_get_info(snd, &info), FA_OK);
        CHECK_EQ(info.voices, 0);
    }
    fa_sound_close(snd);
}

/* A voice the app detached mid-play is paused there; reused, it starts from the start. */
static void test_detached_voice_restarts(void)
{
    enum { N = 8000 };
    float* pcm = (float*)malloc(N * sizeof(float));
    fa_audio_format f;
    fa_sound* snd = NULL;
    fa_stream* a = NULL; fa_stream* b = NULL;
    float first;
    int i;
    CHECK(pcm != NULL);
    for (i = 0; i < N; i++) pcm[i] = 0.5f * (float)(i + 1) / (float)N;
    memset(&f, 0, sizeof(f));
    f.sample_format = FA_FORMAT_F32;
    f.sample_rate = g_dev->rate;
    f.channels = 1;
    i = fa_sound_load_pcm(g_engine, &f, pcm, N, NULL, &snd);
    free(pcm);
    CHECK_EQ(i, FA_OK);
    CHECK_EQ(fa_sound_play(snd, g_dev, NULL, 1.0f, 0.0f, &a), FA_OK);
    render(2048);
    first = L(100);
    CHECK(first > 0.0f);
    CHECK_EQ(fa_stream_detach(a), FA_OK);
    CHECK_EQ(fa_sound_play(snd, g_dev, NULL, 1.0f, 0.0f, &b), FA_OK);
    CHECK(b == a);
    render(2048);
    CHECK_NEAR(L(100), first, 1e-4);   /* from the start, not from where it was detached */
    fa_sound_close(snd);
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

    RUN(test_overlapping_copies_sum);
    RUN(test_memory_shared);
    RUN(test_steal_oldest);
    RUN(test_steal_quietest);
    RUN(test_steal_none);
    RUN(test_ended_voice_reused);
    RUN(test_detached_voice_restarts);
    RUN(test_lower_limit);
    RUN(test_load_formats);
    RUN(test_bad_play_args);

    fa_device_close(g_dev);
    fa_engine_destroy(g_engine);
    free(g_rec);
    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    printf("PASS\n");
    return 0;
}
