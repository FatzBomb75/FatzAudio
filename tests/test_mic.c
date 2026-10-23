/* White-box tests of the mixer's mics. The mixer runs on the null backend with its
 * device stopped, and each mic's input device is stopped too (fa_mic_test_take_over):
 * this test plays both audio threads, feeding the input in its periods and rendering
 * the master in its own, at chosen rates, and checks the samples and the bridge. */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "mixer.h"
#include <math.h>
#include <stddef.h>
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

static const double k_sine_rms = 0.25 / 1.41421356;   /* a sine of amplitude 0.25 */
static fa_engine* g_engine;

#define OUT_BLOCK 256

/* A mixer whose master and inputs this test drives. The input's true rate can differ
 * from the rate it claims (drift); each input channel carries a sine (0 Hz = silence). */
typedef struct sim {
    fa_mixer* m;
    fa_device* dev;
    fa_mic* mic;                 /* the one whose input is driven */
    uint32_t in_rate;            /* what the input claims */
    double in_true;              /* what it delivers per second of master time */
    uint32_t in_ch, in_period;
    double hz[2], amp[2];
    double phase[2];
    double owed;                 /* input frames due but not fed yet */
    float* out;                  /* everything rendered, stereo */
    uint32_t frames, cap;
} sim;

static int sim_open(sim* s)
{
    fa_mixer_config c;
    memset(s, 0, sizeof(*s));
    fa_mixer_config_init(&c);
    c.latency = FA_LATENCY_NORMAL;
    if (fa_mixer_create(g_engine, &c, &s->m) != FA_OK) return 0;
    fa_mixer_get_device(s->m, &s->dev);
    fa_device_stop(s->dev);   /* this test is the audio thread */
    return 1;
}

static int sim_mic(sim* s, int32_t channel, uint32_t in_rate, double in_true, uint32_t in_ch, fa_mic** out)
{
    fa_mic_config mc;
    fa_mic* mic = NULL;
    fa_mic_config_init(&mc);
    mc.channel = channel;
    if (fa_mixer_add_mic(s->m, &mc, &mic) != FA_OK) return 0;
    if (!s->mic) {
        s->mic = mic;
        s->in_rate = in_rate;
        s->in_true = in_true;
        s->in_ch = in_ch;
        s->in_period = in_rate / 100;   /* 10 ms, as a shared-mode input */
        fa_mic_test_take_over(mic, in_rate, in_ch);
    }
    if (out) *out = mic;
    return 1;
}

static void sim_close(sim* s)
{
    fa_mixer_destroy(s->m);
    free(s->out);
}

/* Runs `seconds` of master time: input periods as they fall due, master blocks of OUT_BLOCK. */
static void sim_run(sim* s, double seconds)
{
    uint32_t total = ((uint32_t)(seconds * s->dev->rate) + OUT_BLOCK - 1) / OUT_BLOCK * OUT_BLOCK, done;
    float in[2 * 4800];
    if (s->frames + total > s->cap) {
        s->cap = s->frames + total;
        s->out = (float*)realloc(s->out, (size_t)s->cap * 2 * sizeof(float));
    }
    for (done = 0; done < total; done += OUT_BLOCK) {
        s->owed += (double)OUT_BLOCK * s->in_true / (double)s->dev->rate;
        while (s->owed >= (double)s->in_period) {
            uint32_t i, c;
            for (i = 0; i < s->in_period; i++)
                for (c = 0; c < s->in_ch; c++) {
                    in[i * s->in_ch + c] = (float)(s->amp[c] * sin(s->phase[c]));
                    s->phase[c] += 2.0 * k_pi * s->hz[c] / (double)s->in_rate;
                }
            fa_mic_test_feed(s->mic, in, s->in_period);
            s->owed -= (double)s->in_period;
        }
        fa_fetch_add64(&s->dev->epoch, 1);
        fa_device_render_output(s->dev, s->out + (size_t)s->frames * 2, 2, OUT_BLOCK, fa_monotonic_ns());
        fa_fetch_add64(&s->dev->epoch, 1);
        s->frames += OUT_BLOCK;
    }
}

/* RMS of channel `c` over the last `seconds`. */
static double last_rms(const sim* s, uint32_t c, double seconds)
{
    uint32_t n = (uint32_t)(seconds * s->dev->rate), i;
    double sq = 0.0;
    for (i = s->frames - n; i < s->frames; i++) sq += (double)s->out[i * 2 + c] * s->out[i * 2 + c];
    return sqrt(sq / n);
}

/* Upward zero crossings of channel `c` over the last `seconds`, per second. */
static double last_hz(const sim* s, uint32_t c, double seconds)
{
    uint32_t n = (uint32_t)(seconds * s->dev->rate), i, k = 0;
    for (i = s->frames - n + 1; i < s->frames; i++)
        if (s->out[(i - 1) * 2 + c] < 0.0f && s->out[i * 2 + c] >= 0.0f) k++;
    return (double)k / seconds;
}

/* ---- tests ------------------------------------------------------------------------ */

static void test_passes_the_voice(void)
{
    sim s;
    fa_mixer_latency lat;
    CHECK(sim_open(&s));
    CHECK(sim_mic(&s, -1, s.dev->rate, s.dev->rate, 1, NULL));
    s.hz[0] = 1000.0;
    s.amp[0] = 0.25;
    sim_run(&s, 1.0);
    CHECK_NEAR(last_rms(&s, 0, 0.5) / k_sine_rms, 1.0, 0.01);
    CHECK_NEAR(last_rms(&s, 1, 0.5) / k_sine_rms, 1.0, 0.01);
    CHECK_NEAR(last_hz(&s, 0, 0.5), 1000.0, 4.0);
    memset(&lat, 0, sizeof(lat));
    lat.struct_size = sizeof(lat);
    CHECK_EQ(fa_mixer_get_latency(s.m, &lat), FA_OK);
    CHECK(lat.mic_input[0] > 1000000 && lat.mic_input[0] < 100000000);   /* 1-100 ms */
    CHECK_EQ(lat.mic_monitor[0], lat.mic_input[0] + lat.master);
    CHECK_EQ(lat.mic_input[1], -1);
    CHECK_EQ(lat.cue_alignment, 0);
    sim_close(&s);
}

/* An input clock 500 ppm fast or slow: the bridge's fill stays at its target, with no
 * dropout once it runs. */
static void drift(double ppm)
{
    sim s;
    double target = 0.0, fill = 0.0;
    int32_t underruns = -1;
    CHECK(sim_open(&s));
    CHECK(sim_mic(&s, -1, s.dev->rate, s.dev->rate * (1.0 + ppm * 1e-6), 1, NULL));
    s.hz[0] = 440.0;
    s.amp[0] = 0.25;
    sim_run(&s, 2.0);
    fa_mic_test_stats(s.mic, NULL, NULL, &underruns);
    CHECK_EQ(underruns, 0);
    sim_run(&s, 30.0);
    fa_mic_test_stats(s.mic, &target, &fill, &underruns);
    CHECK_EQ(underruns, 0);
    CHECK_EQ(fa_load32(&s.mic->cap->overruns), 0);
    CHECK(fabs(fill - target) < s.dev->rate * 0.012);   /* within a period of the target */
    CHECK_NEAR(last_rms(&s, 0, 1.0) / k_sine_rms, 1.0, 0.01);
    sim_close(&s);
}

static void test_drift_fast(void) { drift(500.0); }
static void test_drift_slow(void) { drift(-500.0); }

/* A 44.1 kHz input on the master's rate: the same pitch and level. */
static void test_other_rate(void)
{
    sim s;
    int32_t underruns = -1;
    CHECK(sim_open(&s));
    CHECK(sim_mic(&s, -1, 44100, 44100.0, 1, NULL));
    s.hz[0] = 1000.0;
    s.amp[0] = 0.25;
    sim_run(&s, 2.0);
    CHECK_NEAR(last_hz(&s, 0, 1.0), 1000.0, 3.0);
    CHECK_NEAR(last_rms(&s, 0, 1.0) / k_sine_rms, 1.0, 0.01);
    fa_mic_test_stats(s.mic, NULL, NULL, &underruns);
    CHECK_EQ(underruns, 0);
    sim_close(&s);
}

static void test_gate(void)
{
    sim s;
    fa_mic* mic;
    CHECK(sim_open(&s));
    CHECK(sim_mic(&s, -1, s.dev->rate, s.dev->rate, 1, &mic));
    s.hz[0] = 1000.0;
    s.amp[0] = 0.25 * pow(10.0, -60.0 / 20.0);   /* -72 dBFS: room noise */
    sim_run(&s, 1.0);
    CHECK(last_rms(&s, 0, 0.5) < 1e-6);         /* the gate at -50 dB keeps it out */
    CHECK_EQ(fa_mic_set_param(mic, FA_MIC_GATE_DB, -100.0f), FA_OK);
    sim_run(&s, 0.5);
    CHECK_NEAR(last_rms(&s, 0, 0.25) / (k_sine_rms * 1e-3), 1.0, 0.02);
    CHECK_EQ(fa_mic_set_param(mic, FA_MIC_GATE_DB, -50.0f), FA_OK);
    s.amp[0] = 0.25;                              /* singing: opens at once */
    sim_run(&s, 0.5);
    CHECK_NEAR(last_rms(&s, 0, 0.25) / k_sine_rms, 1.0, 0.01);
    sim_close(&s);
}

static void test_gain_mute_pan_monitor(void)
{
    sim s;
    fa_mic* mic;
    float v = 0.0f, peak = 0.0f, rms = 0.0f;
    CHECK(sim_open(&s));
    CHECK(sim_mic(&s, -1, s.dev->rate, s.dev->rate, 1, &mic));
    s.hz[0] = 1000.0;
    s.amp[0] = 0.25;
    CHECK_EQ(fa_mic_set_param(mic, FA_MIC_GAIN_DB, -6.0206f), FA_OK);
    sim_run(&s, 1.0);
    CHECK_NEAR(last_rms(&s, 0, 0.5) / k_sine_rms, 0.5, 0.005);
    CHECK_EQ(fa_mic_set_param(mic, FA_MIC_GAIN_DB, 0.0f), FA_OK);
    CHECK_EQ(fa_mic_set_param(mic, FA_MIC_PAN, -1.0f), FA_OK);
    sim_run(&s, 0.5);
    CHECK_NEAR(last_rms(&s, 0, 0.25) / k_sine_rms, 1.0, 0.01);
    CHECK(last_rms(&s, 1, 0.25) < 1e-6);
    CHECK_EQ(fa_mic_set_param(mic, FA_MIC_PAN, 0.0f), FA_OK);
    CHECK_EQ(fa_mic_set_param(mic, FA_MIC_MUTE, 1.0f), FA_OK);
    sim_run(&s, 0.5);
    CHECK(last_rms(&s, 0, 0.25) < 1e-6);
    CHECK_EQ(fa_mic_set_param(mic, FA_MIC_MUTE, 0.0f), FA_OK);
    CHECK_EQ(fa_mic_set_param(mic, FA_MIC_MONITOR, 0.0f), FA_OK);
    CHECK_EQ(fa_mic_get_levels(mic, 100000000, &peak, &rms), FA_OK);   /* the meter starts with the first ask */
    sim_run(&s, 0.5);
    CHECK(last_rms(&s, 0, 0.25) < 1e-6);         /* not heard */
    CHECK_EQ(fa_mic_get_levels(mic, 100000000, &peak, &rms), FA_OK);
    CHECK_NEAR(peak, 0.25, 0.01);                /* but metered */
    CHECK_NEAR(rms, k_sine_rms, 0.01);
    /* Clamping, switches and the planned sends. */
    CHECK_EQ(fa_mic_set_param(mic, FA_MIC_GAIN_DB, 99.0f), FA_OK);
    CHECK_EQ(fa_mic_get_param(mic, FA_MIC_GAIN_DB, &v), FA_OK);
    CHECK_NEAR(v, 40.0, 1e-6);
    CHECK_EQ(fa_mic_set_param(mic, FA_MIC_GATE_DB, -500.0f), FA_OK);
    CHECK_EQ(fa_mic_get_param(mic, FA_MIC_GATE_DB, &v), FA_OK);
    CHECK_NEAR(v, -100.0, 1e-6);
    CHECK_EQ(fa_mic_set_param(mic, FA_MIC_LOW_CUT, 0.7f), FA_OK);
    CHECK_EQ(fa_mic_get_param(mic, FA_MIC_LOW_CUT, &v), FA_OK);
    CHECK_NEAR(v, 1.0, 1e-6);
    CHECK_EQ(fa_mic_set_param(mic, FA_MIC_REVERB_SEND, 0.3f), FA_ERROR_NOT_IMPLEMENTED);
    CHECK_EQ(fa_mic_set_param(mic, FA_MIC_ECHO_SEND, 0.3f), FA_ERROR_NOT_IMPLEMENTED);
    CHECK_EQ(fa_mic_set_param(mic, (fa_mic_param)99, 0.0f), FA_ERROR_INVALID_ARG);
    sim_close(&s);
}

/* The low cut takes out handling rumble and leaves the voice. */
static void test_low_cut(void)
{
    sim s;
    fa_mic* mic;
    CHECK(sim_open(&s));
    CHECK(sim_mic(&s, -1, s.dev->rate, s.dev->rate, 1, &mic));
    s.hz[0] = 30.0;
    s.amp[0] = 0.25;
    sim_run(&s, 1.0);
    CHECK(last_rms(&s, 0, 0.5) / k_sine_rms < 0.2);   /* 2nd order at 80 Hz: about -17 dB */
    CHECK_EQ(fa_mic_set_param(mic, FA_MIC_LOW_CUT, 0.0f), FA_OK);
    sim_run(&s, 1.0);
    CHECK_NEAR(last_rms(&s, 0, 0.5) / k_sine_rms, 1.0, 0.02);
    sim_close(&s);
}

static void test_eq(void)
{
    sim s;
    fa_mic* mic;
    fa_eq_band band = { FA_EQ_PEAK, 1000.0f, 1.41f, -24.0f }, got[4], preset[FA_EQ_PRESET_BANDS];
    uint32_t n = 0;
    CHECK(sim_open(&s));
    CHECK(sim_mic(&s, -1, s.dev->rate, s.dev->rate, 1, &mic));
    s.hz[0] = 1000.0;
    s.amp[0] = 0.25;
    CHECK_EQ(fa_mic_set_eq(mic, &band, 1), FA_OK);
    CHECK_EQ(fa_mic_get_eq(mic, got, 4, &n), FA_OK);
    CHECK_EQ(n, 1);
    CHECK_NEAR(got[0].gain_db, -24.0, 1e-6);
    sim_run(&s, 1.5);
    CHECK_NEAR(20.0 * log10(last_rms(&s, 0, 0.5) / k_sine_rms), -24.0, 0.5);
    CHECK_EQ(fa_mic_set_eq(mic, NULL, 0), FA_OK);
    sim_run(&s, 1.0);
    CHECK_NEAR(last_rms(&s, 0, 0.5) / k_sine_rms, 1.0, 0.01);
    CHECK_EQ(fa_eq_get_preset(FA_EQ_PRESET_LIVE_VOCAL, preset, &n), FA_OK);
    CHECK_EQ(n, FA_EQ_PRESET_BANDS);
    CHECK(strcmp(fa_eq_preset_name(FA_EQ_PRESET_LIVE_VOCAL), "Live vocal") == 0);
    CHECK_EQ(fa_mic_set_eq(mic, preset, n), FA_OK);
    band.gain_db = NAN;
    CHECK_EQ(fa_mic_set_eq(mic, &band, 1), FA_ERROR_INVALID_ARG);
    sim_close(&s);
}

/* Two mics on one 2-input interface: one input device, a channel each. */
static void test_two_mics_one_input(void)
{
    sim s;
    fa_mic* a;
    fa_mic* b;
    fa_mic* third = NULL;
    fa_mic_config mc;
    uint32_t idx = 9;
    CHECK(sim_open(&s));
    CHECK(sim_mic(&s, 0, s.dev->rate, s.dev->rate, 2, &a));
    fa_mic_config_init(&mc);
    mc.channel = 1;
    if (a->cap->device.capture.channels < 2) {
        /* The null input opened with one channel: channel 1 is not there. */
        printf("  (the input has one channel: only the error is checked)\n");
        CHECK_EQ(fa_mixer_add_mic(s.m, &mc, &b), FA_ERROR_NOT_ENOUGH_CHANNELS);
        sim_close(&s);
        return;
    }
    CHECK_EQ(fa_mixer_add_mic(s.m, &mc, &b), FA_OK);
    CHECK(b->cap == a->cap);
    CHECK(b->lane != a->lane);
    CHECK_EQ(fa_mic_get_index(b, &idx), FA_OK);
    CHECK_EQ(idx, 1);
    CHECK_EQ(fa_mixer_add_mic(s.m, &mc, &third), FA_ERROR_BUSY);
    CHECK(third == NULL);
    fa_mic_set_param(a, FA_MIC_PAN, -1.0f);
    fa_mic_set_param(b, FA_MIC_PAN, 1.0f);
    s.hz[0] = 1000.0; s.amp[0] = 0.25;   /* mic A sings, B is silent */
    sim_run(&s, 1.0);
    CHECK_NEAR(last_rms(&s, 0, 0.5) / k_sine_rms, 1.0, 0.01);
    CHECK(last_rms(&s, 1, 0.5) < 1e-6);
    s.amp[0] = 0.0; s.hz[1] = 700.0; s.amp[1] = 0.25;   /* and the other way round */
    sim_run(&s, 1.0);
    CHECK(last_rms(&s, 0, 0.5) < 1e-6);
    CHECK_NEAR(last_hz(&s, 1, 0.5), 700.0, 4.0);
    /* Removing A keeps the input open for B. */
    fa_mic_remove(a);
    s.mic = b;
    sim_run(&s, 0.5);
    CHECK_NEAR(last_rms(&s, 1, 0.25) / k_sine_rms, 1.0, 0.01);
    sim_close(&s);   /* with B still on it */
}

typedef struct lost_rec {
    fa_mutex lock;
    int count;
    uint32_t index;
    fa_mic* mic;
    int remove;
} lost_rec;

static void FA_CALL on_mixer_event(const fa_mixer_event* ev, void* user)
{
    lost_rec* r = (lost_rec*)user;
    fa_mutex_lock(&r->lock);
    if (ev->type == FA_EVENT_INPUT_LOST) {
        r->count++;
        r->index = ev->index;
        r->mic = ev->mic;
    }
    fa_mutex_unlock(&r->lock);
    if (r->remove) fa_mic_remove(ev->mic);   /* allowed inside the callback */
}

static int wait_lost(lost_rec* r)
{
    int i, n = 0;
    for (i = 0; i < 2000 && n == 0; i++) {
        fa_mutex_lock(&r->lock);
        n = r->count;
        fa_mutex_unlock(&r->lock);
        if (!n) fa_sleep_ms(1);
    }
    return n;
}

static void test_input_lost(void)
{
    sim s;
    fa_mic* mic;
    lost_rec r;
    memset(&r, 0, sizeof(r));
    fa_mutex_init(&r.lock);
    CHECK(sim_open(&s));
    CHECK(sim_mic(&s, -1, s.dev->rate, s.dev->rate, 1, &mic));
    CHECK_EQ(fa_mixer_set_event_callback(s.m, on_mixer_event, &r), FA_OK);
    fa_mic_test_lose(mic);
    CHECK_EQ(wait_lost(&r), 1);
    CHECK_EQ(r.index, 0);
    CHECK(r.mic == mic);
    fa_sleep_ms(20);
    CHECK_EQ(r.count, 1);                         /* once */
    sim_run(&s, 0.2);                             /* the mix carries on, silent */
    CHECK(last_rms(&s, 0, 0.1) < 1e-6);
    /* A callback may remove the mic it is told about. */
    fa_mic_remove(mic);
    CHECK(sim_mic(&s, -1, s.dev->rate, s.dev->rate, 1, &mic));
    r.count = 0;
    r.remove = 1;
    fa_mic_test_lose(mic);
    CHECK_EQ(wait_lost(&r), 1);
    fa_sleep_ms(20);
    CHECK(fa_load_ptr(&s.m->mics[0]) == NULL);
    s.mic = NULL;
    CHECK_EQ(fa_mixer_set_event_callback(s.m, NULL, NULL), FA_OK);
    sim_close(&s);
    fa_mutex_destroy(&r.lock);
}

static void FA_CALL destroy_from_callback(const fa_mixer_event* ev, void* user)
{
    (void)user;
    fa_mixer_destroy(ev->mixer);
}

/* A mixer destroyed by its own event callback. */
static void test_destroy_in_callback(void)
{
    sim s;
    fa_mic* mic;
    int i;
    CHECK(sim_open(&s));
    CHECK(sim_mic(&s, -1, s.dev->rate, s.dev->rate, 1, &mic));
    CHECK_EQ(fa_mixer_set_event_callback(s.m, destroy_from_callback, NULL), FA_OK);
    fa_mic_test_lose(mic);
    for (i = 0; i < 2000; i++) {      /* the device leaves the engine's list when it is closed */
        fa_device* d;
        int found = 0;
        fa_mutex_lock(&g_engine->list_lock);
        for (d = g_engine->devices; d; d = d->next) if (d == s.dev) found = 1;
        fa_mutex_unlock(&g_engine->list_lock);
        if (!found) break;
        fa_sleep_ms(1);
    }
    CHECK(i < 2000);
    fa_sleep_ms(20);                  /* and the service frees it after the callback */
    free(s.out);
}

static void test_config_errors(void)
{
    sim s;
    fa_mic_config mc;
    fa_mic* mic = NULL;
    fa_mixer_latency small;
    CHECK(sim_open(&s));
    fa_mic_config_init(&mc);
    CHECK_EQ(mc.channel, -1);
    CHECK_EQ(mc.monitor, 1);
    mc.flags = 1u << 7;
    CHECK_EQ(fa_mixer_add_mic(s.m, &mc, &mic), FA_ERROR_INVALID_ARG);
    fa_mic_config_init(&mc);
    mc.channel = 99;
    CHECK_EQ(fa_mixer_add_mic(s.m, &mc, &mic), FA_ERROR_NOT_ENOUGH_CHANNELS);
    CHECK(mic == NULL);
    CHECK(s.m->captures == NULL);           /* the input it opened is closed again */
    fa_mic_config_init(&mc);
    mc.device_id = "zz";
    CHECK_EQ(fa_mixer_add_mic(s.m, &mc, &mic), FA_ERROR_INVALID_ARG);
    mc.struct_size = 4;
    CHECK_EQ(fa_mixer_add_mic(s.m, &mc, &mic), FA_ERROR_INVALID_ARG);
    /* A latency struct from an older header is filled up to its size. */
    memset(&small, 0x55, sizeof(small));
    small.struct_size = (uint32_t)offsetof(fa_mixer_latency, cue);
    CHECK_EQ(fa_mixer_get_latency(s.m, &small), FA_OK);
    CHECK_EQ(small.struct_size, offsetof(fa_mixer_latency, cue));
    CHECK(small.cue == (fa_time)0x5555555555555555LL);
    CHECK(strcmp(fa_result_string(FA_ERROR_PERMISSION_DENIED), "unknown result") != 0);
    sim_close(&s);
}

int main(void)
{
    fa_engine_config cfg;
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    cfg.worker_threads = 2;
    if (fa_engine_create(&cfg, &g_engine) != FA_OK) { fprintf(stderr, "engine create failed\n"); return 1; }

    RUN(test_passes_the_voice);
    RUN(test_drift_fast);
    RUN(test_drift_slow);
    RUN(test_other_rate);
    RUN(test_gate);
    RUN(test_gain_mute_pan_monitor);
    RUN(test_low_cut);
    RUN(test_eq);
    RUN(test_two_mics_one_input);
    RUN(test_input_lost);
    RUN(test_destroy_in_callback);
    RUN(test_config_errors);

    fa_engine_destroy(g_engine);
    if (g_failures) { fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    printf("all mic tests passed\n");
    return 0;
}
