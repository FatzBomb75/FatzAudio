#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L /* nanosleep under strict C11 */
#endif
/* Engine and stream parity with BASS: engine stats (BASS_GetCPU), callback streams
 * (STREAMPROC), loudness meters (BASSloud) and capture streams (BASS_RecordStart),
 * all on the silent null output and input. Cancelling a web stream's open is in
 * test_net.c, next to its server. */
#include "fatzaudio/fatzaudio.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__)
#  include <unistd.h>
#endif
#ifdef _WIN32
#  include <windows.h>
static void sleep_ms(unsigned ms) { Sleep(ms); }
#else
#  include <time.h>
static void sleep_ms(unsigned ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; return; } } while (0)
#define CHECK_EQ(a, b) do { long long va_ = (long long)(a), vb_ = (long long)(b); if (va_ != vb_) { \
    fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_failures++; return; } } while (0)
#define RUN(t) do { int before_ = g_failures; printf("%s\n", #t); t(); if (g_failures == before_) printf("  ok\n"); } while (0)

#define PI 3.14159265358979323846

static fa_engine* g_engine;

/* Waits until the device has played `ms` more (its clock, not the wall clock: sleeps
 * on a loaded runner overshoot), giving up after 10x as long. */
static void play_for(fa_device* d, unsigned ms)
{
    fa_time t0 = 0, t = 0;
    unsigned waited = 0;
    fa_device_get_time(d, &t0);
    do {
        sleep_ms(10);
        waited += 10;
        fa_device_get_time(d, &t);
    } while (t - t0 < (fa_time)ms * 1000000 && waited < ms * 10);
}

/* ---- callback source ------------------------------------------------------ */

/* Stereo S16: left counts up from 0, right counts down; `total` frames, then the end.
 * Every other call with `stutter` gives nothing (0), as a source that is not ready. */
typedef struct ramp {
    int64_t next, total;
    int stutter, calls;
} ramp;

static int64_t FA_CALL ramp_proc(void* buffer, fa_frames frames, void* user)
{
    ramp* r = (ramp*)user;
    int16_t* p = (int16_t*)buffer;
    int64_t i, n;
    r->calls++;
    if (r->next >= r->total) return FA_STREAM_PROC_END;
    if (r->stutter && (r->calls & 1)) return 0;
    n = r->total - r->next < frames ? r->total - r->next : frames;
    if (n > 100) n = 100;   /* short writes are fine */
    for (i = 0; i < n; i++) {
        p[i * 2] = (int16_t)((r->next + i) % 30000);
        p[i * 2 + 1] = (int16_t)-((r->next + i) % 30000);
    }
    r->next += n;
    return n;
}

static void test_callback_decode(void)
{
    fa_audio_format f = { FA_FORMAT_S16, 22050, 2, 0 };
    fa_open_options o;
    ramp r = { 0, 1000, 1, 0 };
    fa_stream* s = NULL;
    fa_stream_info info;
    float buf[2 * 256];
    int64_t at = 0;
    int empty = 0, guard = 0;
    fa_result res = FA_OK;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    CHECK_EQ(fa_stream_open_callback(g_engine, &f, ramp_proc, &r, &o, &s), FA_OK);
    memset(&info, 0, sizeof(info));
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK_EQ(info.format.sample_rate, 22050);
    CHECK_EQ(info.format.channels, 2);
    while (guard++ < 1000) {
        fa_frames got = 0, i;
        res = fa_stream_read(s, buf, 256, &got);
        if (res == FA_ERROR_END_OF_STREAM) break;
        if (res != FA_OK) break;
        if (got == 0) { empty++; continue; }   /* the source had nothing this time */
        for (i = 0; i < got; i++, at++) {
            if (buf[i * 2] != (float)(at % 30000) / 32768.0f || buf[i * 2 + 1] != -(float)(at % 30000) / 32768.0f) {
                fprintf(stderr, "  frame %lld: %f %f\n", (long long)at, buf[i * 2], buf[i * 2 + 1]);
                CHECK(0);
            }
        }
    }
    CHECK_EQ(res, FA_ERROR_END_OF_STREAM);
    CHECK_EQ(at, 1000);
    CHECK(empty > 0);
    CHECK_EQ(fa_stream_seek(s, 0), FA_ERROR_NOT_SEEKABLE);
    fa_stream_close(s);

    /* Planar formats, loops and missing arguments are refused. */
    f.sample_format = FA_FORMAT_F32_PLANAR;
    CHECK_EQ(fa_stream_open_callback(g_engine, &f, ramp_proc, &r, NULL, &s), FA_ERROR_INVALID_ARG);
    f.sample_format = FA_FORMAT_S16;
    o.flags = FA_OPEN_LOOP;
    CHECK_EQ(fa_stream_open_callback(g_engine, &f, ramp_proc, &r, &o, &s), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_stream_open_callback(g_engine, &f, NULL, &r, NULL, &s), FA_ERROR_INVALID_ARG);
}

/* ---- loudness meters on a callback tone ---------------------------------- */

/* A stereo 997 Hz sine of amplitude `amp` in F32, without end (EBU Tech 3341 case 1:
 * -23 dBFS reads -23 LUFS momentary and short-term). */
typedef struct sine { double phase, amp; uint32_t rate; int calls; } sine;

static int64_t FA_CALL sine_proc(void* buffer, fa_frames frames, void* user)
{
    sine* t = (sine*)user;
    float* p = (float*)buffer;
    fa_frames i;
    t->calls++;
    for (i = 0; i < frames; i++) {
        float v = (float)(t->amp * sin(t->phase));
        p[i * 2] = p[i * 2 + 1] = v;
        t->phase += 2.0 * PI * 997.0 / t->rate;
        if (t->phase > 2.0 * PI) t->phase -= 2.0 * PI;
    }
    return frames;
}

static void test_stats_and_loudness(void)
{
    fa_audio_format f = { FA_FORMAT_F32, 48000, 2, 0 };
    /* Static: a failed CHECK returns with the stream still calling sine_proc. */
    static sine t;
    fa_device* d = NULL;
    fa_stream* s = NULL;
    fa_engine_stats st;
    double m = 0.0, sh = 0.0, dm = 0.0, dsh = 0.0;
    memset(&t, 0, sizeof(t));
    t.rate = 48000;
    t.amp = pow(10.0, -23.0 / 20.0);
    CHECK_EQ(fa_device_open(g_engine, NULL, &d), FA_OK);
    CHECK_EQ(fa_stream_open_callback(g_engine, &f, sine_proc, &t, NULL, &s), FA_OK);
    /* Before anything plays: silence. */
    CHECK_EQ(fa_stream_get_loudness_meter(s, &m, &sh), FA_OK);
    CHECK(isinf(m) && m < 0 && isinf(sh) && sh < 0);
    CHECK_EQ(fa_device_get_loudness_meter(d, &dm, NULL), FA_OK);
    CHECK(isinf(dm) && dm < 0);
    CHECK_EQ(fa_stream_get_loudness_meter(s, NULL, NULL), FA_ERROR_INVALID_ARG);

    CHECK_EQ(fa_stream_attach(s, d, NULL), FA_OK);
    CHECK_EQ(fa_device_start(d), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    play_for(d, 3500);   /* the short-term window, and the meters' start */
    CHECK_EQ(fa_stream_get_loudness_meter(s, &m, &sh), FA_OK);
    CHECK_EQ(fa_device_get_loudness_meter(d, &dm, &dsh), FA_OK);
    printf("  stream: momentary %.2f, short-term %.2f LUFS; device: %.2f, %.2f\n", m, sh, dm, dsh);
    CHECK(fabs(m + 23.0) < 0.2 && fabs(sh + 23.0) < 0.2);
    CHECK(fabs(dm + 23.0) < 0.2 && fabs(dsh + 23.0) < 0.2);

    /* Quieter by 10 dB: momentary follows within 400 ms, short-term still holds most of
     * the louder part. */
    t.amp = pow(10.0, -33.0 / 20.0);
    play_for(d, 1000);
    CHECK_EQ(fa_stream_get_loudness_meter(s, &m, &sh), FA_OK);
    printf("  after the step: momentary %.2f, short-term %.2f LUFS\n", m, sh);
    CHECK(fabs(m + 33.0) < 0.5);
    CHECK(sh > -30.0 && sh < -23.5);

    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_EQ(fa_engine_get_stats(g_engine, &st), FA_OK);
    printf("  audio load %.2f%% (peak %.2f%%), workers %.2f%% of a core, %u workers, %u devices, %u streams\n",
           st.audio_load, st.audio_load_peak, st.worker_load, st.workers, st.devices, st.attached_streams);
    CHECK_EQ(st.struct_size, sizeof(st));
    CHECK_EQ(st.devices, 1);
    CHECK_EQ(st.attached_streams, 1);
    CHECK(st.workers >= 1);
    CHECK(st.audio_load > 0.0f && st.audio_load < 100.0f);
    CHECK(st.audio_load_peak >= st.audio_load * 0.5f && st.audio_load_peak < 1000.0f);
    CHECK(st.worker_load > 0.0f && st.worker_load < 100.0f * st.workers);
    /* An older, shorter struct gets its part only. */
    {
        fa_engine_stats older;
        memset(&older, 0xAB, sizeof(older));
        older.struct_size = (uint32_t)(sizeof(uint32_t) + sizeof(float));
        CHECK_EQ(fa_engine_get_stats(g_engine, &older), FA_OK);
        CHECK(older.audio_load >= 0.0f);
        CHECK_EQ(older.workers, 0xABABABABu);
    }

    /* Stopped: silence again. */
    CHECK_EQ(fa_stream_stop(s), FA_OK);
    CHECK_EQ(fa_stream_get_loudness_meter(s, &m, NULL), FA_OK);
    CHECK(isinf(m) && m < 0);
    fa_stream_close(s);
    fa_device_close(d);
    CHECK_EQ(fa_engine_get_stats(g_engine, &st), FA_OK);
    CHECK_EQ(st.devices, 0);
    CHECK_EQ(st.attached_streams, 0);
}

/* A callback stream that ends plays out and reports ENDED. */
static void test_callback_plays_to_end(void)
{
    fa_audio_format f = { FA_FORMAT_S16, 44100, 2, 0 };
    static ramp r;   /* static: a failed CHECK returns with the stream still calling ramp_proc */
    fa_device* d = NULL;
    fa_stream* s = NULL;
    fa_stream_state state = FA_STATE_STOPPED;
    int i;
    memset(&r, 0, sizeof(r));
    r.total = 44100 / 4;
    CHECK_EQ(fa_device_open(g_engine, NULL, &d), FA_OK);
    CHECK_EQ(fa_stream_open_callback(g_engine, &f, ramp_proc, &r, NULL, &s), FA_OK);
    CHECK_EQ(fa_stream_attach(s, d, NULL), FA_OK);
    CHECK_EQ(fa_device_start(d), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    for (i = 0; i < 300 && state != FA_STATE_ENDED; i++) {
        sleep_ms(10);
        fa_stream_get_state(s, &state);
    }
    CHECK_EQ(state, FA_STATE_ENDED);
    CHECK_EQ(r.next, r.total);
    fa_stream_close(s);
    fa_device_close(d);
}

/* ---- capture streams ------------------------------------------------------- */

static void test_capture_read(void)
{
    fa_capture_options c;
    fa_open_options o;
    fa_stream* s = NULL;
    fa_stream_info info;
    float buf[2 * 512];
    fa_frames total = 0;
    int i;
    fa_result r;
    fa_capture_options_init(&c);
    c.channels = 2;
    c.sample_rate = 48000;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    r = fa_stream_open_capture(g_engine, &c, &o, &s);
    CHECK_EQ(r, FA_OK);
    memset(&info, 0, sizeof(info));
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK_EQ(info.format.channels, 2);
    CHECK_EQ(info.format.sample_rate, 48000);
    /* The null input gives silence in real time; 0 frames while nothing new is there. */
    for (i = 0; i < 300 && total < 4800; i++) {
        fa_frames got = 0, k;
        CHECK_EQ(fa_stream_read(s, buf, 512, &got), FA_OK);
        for (k = 0; k < got * 2; k++) CHECK(buf[k] == 0.0f);
        total += got;
        if (got == 0) sleep_ms(10);
    }
    printf("  read %lld frames\n", (long long)total);
    CHECK(total >= 4800);
    CHECK_EQ(fa_stream_seek(s, 0), FA_ERROR_NOT_SEEKABLE);
    fa_stream_close(s);

    c.flags = 1u << 7;   /* unknown flag */
    CHECK_EQ(fa_stream_open_capture(g_engine, &c, &o, &s), FA_ERROR_INVALID_ARG);
    CHECK(s == NULL);
}

/* Recording an input to a file: a decode-only capture stream read by an encoder. */
static void test_capture_record(void)
{
    const char* path = "parity_capture.wav";
    fa_open_options o;
    fa_stream* s = NULL;
    fa_encoder* e = NULL;
    fa_encoder_status st;
    int i;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    CHECK_EQ(fa_stream_open_capture(g_engine, NULL, &o, &s), FA_OK);
    CHECK_EQ(fa_encoder_create_path(path, NULL, &e), FA_OK);
    CHECK_EQ(fa_encoder_encode_stream(e, s), FA_OK);
    memset(&st, 0, sizeof(st));
    for (i = 0; i < 300; i++) {
        sleep_ms(10);
        CHECK_EQ(fa_encoder_get_status(e, &st), FA_OK);
        if (st.duration >= 300000000) break;
    }
    printf("  recorded %.0f ms\n", (double)st.duration / 1e6);
    CHECK_EQ(st.state, FA_ENCODER_RUNNING);
    CHECK(st.duration >= 300000000);
    CHECK_EQ(fa_encoder_close(e), FA_OK);
    fa_stream_close(s);
    remove(path);
}

/* Played on an output like any other stream. */
static void test_capture_plays(void)
{
    fa_device* d = NULL;
    fa_stream* s = NULL;
    fa_frames pos = 0;
    CHECK_EQ(fa_device_open(g_engine, NULL, &d), FA_OK);
    CHECK_EQ(fa_stream_open_capture(g_engine, NULL, NULL, &s), FA_OK);
    CHECK_EQ(fa_stream_attach(s, d, NULL), FA_OK);
    CHECK_EQ(fa_device_start(d), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    play_for(d, 500);
    CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
    printf("  position %lld frames\n", (long long)pos);
    CHECK(pos > 0);
    fa_stream_close(s);
    fa_device_close(d);
}

int main(void)
{
    fa_engine_config cfg;
#if defined(__APPLE__)
    /* The iOS simulator starts programs elsewhere: tools/ios-sim/run.sh names the folder. */
    if (getenv("FA_TEST_CWD") && chdir(getenv("FA_TEST_CWD")) != 0) return 1;
#endif
    setvbuf(stdout, NULL, _IONBF, 0);   /* the readings show even if a test crashes */
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    if (fa_engine_create(&cfg, &g_engine) != FA_OK) {
        fprintf(stderr, "engine create failed\n");
        return 1;
    }
    RUN(test_callback_decode);
    RUN(test_callback_plays_to_end);
    RUN(test_stats_and_loudness);
    RUN(test_capture_read);
    RUN(test_capture_record);
    RUN(test_capture_plays);
    fa_engine_destroy(g_engine);
    printf(g_failures ? "%d failure(s)\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
