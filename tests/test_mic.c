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
    fa_stream* music;            /* on deck A, when a test plays music */
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
    fa_mic_set_param(mic, FA_MIC_REVERB_SEND, 0.0f);   /* dry: the strip alone (the effects have their own tests) */
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
    if (s->music) fa_stream_close(s->music);
    free(s->out);
}

/* A 16-bit mono WAV of a sine at amplitude 0.25. */
static uint8_t* sine_wav(uint32_t rate, uint32_t frames, double hz, size_t* size)
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
        int16_t sv = (int16_t)lrint(8192.0 * sin(2.0 * k_pi * hz * i / rate));
        memcpy(b + 44 + (size_t)i * 2, &sv, 2);
    }
    *size = n;
    return b;
}

/* Music on deck A: a sine of `hz`, `seconds` long, playing (crossfader in the middle:
 * heard at -3 dB). */
static int sim_music(sim* s, double hz, double seconds)
{
    size_t n;
    fa_deck* a;
    uint8_t* wav = sine_wav(s->dev->rate, (uint32_t)(seconds * s->dev->rate), hz, &n);
    int ok = fa_stream_open_memory(g_engine, wav, n, NULL, &s->music) == FA_OK;
    free(wav);
    fa_mixer_get_deck(s->m, 0, &a);
    return ok && fa_deck_load(a, s->music, 0) == FA_OK && fa_stream_play(s->music) == FA_OK;
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
        int w;
        for (w = 0; s->music && w < 2000; w++) {   /* the deck's worker has the block decoded */
            if (fa_ring_readable(&s->music->ring) >= OUT_BLOCK || fa_load32(&s->music->eof)) break;
            fa_sleep_ms(1);
        }
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
    /* Clamping, switches and the sends. */
    CHECK_EQ(fa_mic_set_param(mic, FA_MIC_GAIN_DB, 99.0f), FA_OK);
    CHECK_EQ(fa_mic_get_param(mic, FA_MIC_GAIN_DB, &v), FA_OK);
    CHECK_NEAR(v, 40.0, 1e-6);
    CHECK_EQ(fa_mic_set_param(mic, FA_MIC_GATE_DB, -500.0f), FA_OK);
    CHECK_EQ(fa_mic_get_param(mic, FA_MIC_GATE_DB, &v), FA_OK);
    CHECK_NEAR(v, -100.0, 1e-6);
    CHECK_EQ(fa_mic_set_param(mic, FA_MIC_LOW_CUT, 0.7f), FA_OK);
    CHECK_EQ(fa_mic_get_param(mic, FA_MIC_LOW_CUT, &v), FA_OK);
    CHECK_NEAR(v, 1.0, 1e-6);
    CHECK_EQ(fa_mic_set_param(mic, FA_MIC_REVERB_SEND, 3.0f), FA_OK);
    CHECK_EQ(fa_mic_get_param(mic, FA_MIC_REVERB_SEND, &v), FA_OK);
    CHECK_NEAR(v, 1.0, 1e-6);
    CHECK_EQ(fa_mic_set_param(mic, FA_MIC_ECHO_SEND, -1.0f), FA_OK);
    CHECK_EQ(fa_mic_get_param(mic, FA_MIC_ECHO_SEND, &v), FA_OK);
    CHECK_NEAR(v, 0.0, 1e-6);
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
    fa_mic_set_param(b, FA_MIC_REVERB_SEND, 0.0f);
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

/* ---- the mic bus: effects, ducking, recording ---------------------------------- */

static double rms_at(const sim* s, uint32_t c, uint32_t from, uint32_t n)
{
    double sq = 0.0;
    uint32_t i;
    for (i = from; i < from + n && i < s->frames; i++) sq += (double)s->out[i * 2 + c] * s->out[i * 2 + c];
    return sqrt(sq / n);
}

/* The first frame from `from` on where channel 0 goes above `level`. */
static uint32_t onset(const sim* s, uint32_t from, float level)
{
    uint32_t i;
    for (i = from; i < s->frames; i++)
        if (fabsf(s->out[i * 2]) > level) return i;
    return s->frames;
}

static void test_effects_settings(void)
{
    sim s;
    fa_mic_effects e;
    fa_ducking d;
    fa_mic_config mc;
    fa_mic* mic;
    float v = -1.0f;
    CHECK(sim_open(&s));
    fa_mic_config_init(&mc);
    CHECK_EQ(fa_mixer_add_mic(s.m, &mc, &mic), FA_OK);
    CHECK_EQ(fa_mic_get_param(mic, FA_MIC_REVERB_SEND, &v), FA_OK);
    CHECK_NEAR(v, 0.2, 1e-6);                         /* karaoke: a little reverb from the start */
    CHECK_EQ(fa_mic_get_param(mic, FA_MIC_ECHO_SEND, &v), FA_OK);
    CHECK_NEAR(v, 0.0, 1e-6);
    fa_mic_effects_init(&e);
    CHECK_EQ(fa_mixer_get_mic_effects(s.m, &e), FA_OK);
    CHECK_NEAR(e.reverb_size, 0.5, 1e-6);
    CHECK_NEAR(e.echo_time_ms, 250.0, 1e-6);
    CHECK_NEAR(e.echo_feedback, 0.35, 1e-6);
    e.reverb_size = 3.0f;
    e.echo_time_ms = 5.0f;
    e.echo_feedback = 2.0f;
    e.echo_tone = -1.0f;
    CHECK_EQ(fa_mixer_set_mic_effects(s.m, &e), FA_OK);
    CHECK_EQ(fa_mixer_get_mic_effects(s.m, &e), FA_OK);
    CHECK_NEAR(e.reverb_size, 1.0, 1e-6);
    CHECK_NEAR(e.echo_time_ms, 20.0, 1e-6);
    CHECK_NEAR(e.echo_feedback, 0.9, 1e-6);
    CHECK_NEAR(e.echo_tone, 0.0, 1e-6);
    e.echo_tone = NAN;
    CHECK_EQ(fa_mixer_set_mic_effects(s.m, &e), FA_ERROR_INVALID_ARG);
    e.struct_size = 4;
    CHECK_EQ(fa_mixer_set_mic_effects(s.m, &e), FA_ERROR_INVALID_ARG);
    fa_ducking_init(&d);
    CHECK_EQ(fa_mixer_get_ducking(s.m, &d), FA_OK);
    CHECK_NEAR(d.depth_db, -12.0, 1e-6);              /* on from the start */
    CHECK_EQ(d.hold, 300000000);
    d.depth_db = -100.0f;
    d.attack = 0;
    CHECK_EQ(fa_mixer_set_ducking(s.m, &d), FA_OK);
    CHECK_EQ(fa_mixer_get_ducking(s.m, &d), FA_OK);
    CHECK_NEAR(d.depth_db, -40.0, 1e-6);
    CHECK_EQ(d.attack, 1000000);
    CHECK_EQ(fa_mixer_set_ducking(s.m, NULL), FA_OK);
    CHECK_EQ(fa_mixer_get_ducking(s.m, &d), FA_OK);
    CHECK_NEAR(d.depth_db, 0.0, 1e-6);
    sim_close(&s);
}

/* The reverb: a tail after the voice stops, longer for a bigger room; none without a send. */
static double reverb_tail(float size, float send)
{
    sim s;
    fa_mic* mic;
    fa_mic_effects e;
    uint32_t stop;
    double tail;
    if (!sim_open(&s)) return -1.0;
    if (!sim_mic(&s, -1, s.dev->rate, s.dev->rate, 1, &mic)) { sim_close(&s); return -1.0; }
    fa_mic_effects_init(&e);
    e.reverb_size = size;
    fa_mixer_set_mic_effects(s.m, &e);
    fa_mic_set_param(mic, FA_MIC_REVERB_SEND, send);
    s.hz[0] = 700.0;
    s.amp[0] = 0.25;
    sim_run(&s, 1.0);
    s.amp[0] = 0.0;
    stop = s.frames;
    sim_run(&s, 2.0);
    /* 0.5 .. 1 s after the voice stopped: only the tail is left (the gate closed long ago). */
    tail = rms_at(&s, 0, stop + s.dev->rate / 2, s.dev->rate / 2);
    sim_close(&s);
    return tail;
}

static void test_reverb(void)
{
    double none = reverb_tail(0.5f, 0.0f), room = reverb_tail(0.0f, 1.0f), hall = reverb_tail(1.0f, 1.0f);
    CHECK(none >= 0.0 && none < 1e-7);
    CHECK(room > 0.0 && hall > 0.0);
    CHECK(hall > room * 10.0);          /* a hall rings on long after a small room */
    CHECK(hall < k_sine_rms);           /* and stays below the voice */
}

/* The echo: repeats at the set time, each `feedback` of the one before. */
static void test_echo(void)
{
    sim s;
    fa_mic* mic;
    fa_mic_effects e;
    uint32_t t0, d, burst;
    double first, r1, r2;
    CHECK(sim_open(&s));
    CHECK(sim_mic(&s, -1, s.dev->rate, s.dev->rate, 1, &mic));
    fa_mic_effects_init(&e);
    e.echo_time_ms = 100.0f;
    e.echo_feedback = 0.5f;
    e.echo_tone = 1.0f;
    CHECK_EQ(fa_mixer_set_mic_effects(s.m, &e), FA_OK);
    CHECK_EQ(fa_mic_set_param(mic, FA_MIC_ECHO_SEND, 1.0f), FA_OK);
    CHECK_EQ(fa_mic_set_param(mic, FA_MIC_GATE_DB, -100.0f), FA_OK);
    s.hz[0] = 1000.0;
    sim_run(&s, 0.5);                   /* the input runs, silent */
    s.amp[0] = 0.25;
    sim_run(&s, 0.03);
    s.amp[0] = 0.0;
    sim_run(&s, 1.0);
    t0 = onset(&s, 0, 0.05f);
    CHECK(t0 < s.frames);
    d = s.dev->rate / 10;
    burst = s.dev->rate / 100;          /* the middle 10 ms of each */
    first = rms_at(&s, 0, t0 + burst / 2, burst);
    r1 = rms_at(&s, 0, t0 + d + burst / 2, burst);
    r2 = rms_at(&s, 0, t0 + 2 * d + burst / 2, burst);
    CHECK_NEAR(first / k_sine_rms, 1.0, 0.05);
    CHECK_NEAR(r1 / first, 1.0, 0.05);  /* the first repeat is the send */
    CHECK_NEAR(r2 / r1, 0.5, 0.05);     /* then the feedback */
    CHECK(rms_at(&s, 0, t0 + d / 2, burst) < 0.01 * first);   /* quiet between repeats */
    sim_close(&s);
}

/* A host's voice ducks the music; a singer's does not; NULL turns it off. */
static void test_ducking(void)
{
    sim s;
    fa_mic* host;
    fa_mic* singer;
    fa_mic_config mc;
    double music, ducked;
    CHECK(sim_open(&s));
    CHECK(sim_music(&s, 200.0, 12.0));
    CHECK(sim_mic(&s, 0, s.dev->rate, s.dev->rate, 2, &host));
    fa_mic_config_init(&mc);
    mc.channel = 1;
    CHECK_EQ(fa_mixer_add_mic(s.m, &mc, &singer), FA_OK);
    fa_mic_set_param(singer, FA_MIC_REVERB_SEND, 0.0f);
    fa_mic_remove(host);                /* again, now as a host */
    mc.channel = 0;
    mc.flags = FA_MIC_DUCKS_MUSIC;
    CHECK_EQ(fa_mixer_add_mic(s.m, &mc, &host), FA_OK);
    s.mic = singer;                     /* drives the shared input */
    /* Neither heard, so the output is the music alone. */
    CHECK_EQ(fa_mic_set_param(host, FA_MIC_MONITOR, 0.0f), FA_OK);
    CHECK_EQ(fa_mic_set_param(singer, FA_MIC_MONITOR, 0.0f), FA_OK);
    s.hz[0] = 1000.0;
    s.hz[1] = 1500.0;
    sim_run(&s, 1.0);
    music = last_rms(&s, 0, 0.5);
    CHECK_NEAR(music / (k_sine_rms * 0.70710678), 1.0, 0.02);
    s.amp[0] = 0.25;                    /* the host speaks */
    sim_run(&s, 1.0);
    ducked = last_rms(&s, 0, 0.5);
    CHECK_NEAR(20.0 * log10(ducked / music), -12.0, 0.3);
    s.amp[0] = 0.0;
    sim_run(&s, 0.2);                   /* held */
    CHECK_NEAR(20.0 * log10(last_rms(&s, 0, 0.1) / music), -12.0, 0.3);
    sim_run(&s, 1.2);                   /* released */
    CHECK_NEAR(last_rms(&s, 0, 0.3) / music, 1.0, 0.02);
    s.amp[1] = 0.25;                    /* a singer never ducks */
    sim_run(&s, 1.0);
    CHECK_NEAR(last_rms(&s, 0, 0.5) / music, 1.0, 0.02);
    s.amp[1] = 0.0;
    CHECK_EQ(fa_mic_set_param(host, FA_MIC_MUTE, 1.0f), FA_OK);
    s.amp[0] = 0.25;                    /* a muted host does not either */
    sim_run(&s, 1.0);
    CHECK_NEAR(last_rms(&s, 0, 0.5) / music, 1.0, 0.02);
    CHECK_EQ(fa_mic_set_param(host, FA_MIC_MUTE, 0.0f), FA_OK);
    CHECK_EQ(fa_mixer_set_ducking(s.m, NULL), FA_OK);
    sim_run(&s, 1.0);                   /* off */
    CHECK_NEAR(last_rms(&s, 0, 0.5) / music, 1.0, 0.02);
    sim_close(&s);
}

/* Recording: a WAV in memory, forward only. */
typedef struct membuf { uint8_t* data; size_t size, cap; } membuf;

static fa_result FA_CALL mem_write(void* user, const void* data, size_t size)
{
    membuf* b = (membuf*)user;
    if (b->size + size > b->cap) {
        b->cap = (b->size + size) * 2;
        b->data = (uint8_t*)realloc(b->data, b->cap);
    }
    memcpy(b->data + b->size, data, size);
    b->size += size;
    return FA_OK;
}

static fa_encoder* mem_encoder(membuf* b)
{
    fa_encoder_options o;
    fa_encoder_output out = { mem_write, NULL, NULL };
    fa_encoder* e = NULL;
    memset(b, 0, sizeof(*b));
    fa_encoder_options_init(&o);
    o.bits_per_sample = 32;             /* float: the samples exactly */
    if (fa_encoder_create_output(&out, b, &o, &e) != FA_OK) return NULL;
    return e;
}

/* The float frames of a recorded WAV, copied out (the data chunk need not be aligned). */
static float* wav_frames(const membuf* b, uint32_t* frames)
{
    size_t i;
    for (i = 12; i + 8 <= b->size; i++)
        if (memcmp(b->data + i, "data", 4) == 0) {
            float* f;
            *frames = (uint32_t)((b->size - i - 8) / 8);
            f = (float*)malloc((size_t)*frames * 8 + 8);
            if (f) memcpy(f, b->data + i + 8, (size_t)*frames * 8);
            return f;
        }
    *frames = 0;
    return NULL;
}

static void recording(uint32_t flags)
{
    sim s;
    fa_mic* mic;
    membuf mb, vb;
    fa_encoder* enc[2];
    float* mf;
    float* vf;
    uint32_t mn = 0, vn = 0, i, bad = 0, from;
    int64_t shift;
    float other;
    fa_mixer_latency lat;
    CHECK(sim_open(&s));
    CHECK(sim_mic(&s, -1, s.dev->rate, s.dev->rate, 1, &mic));
    s.hz[0] = 440.0;
    s.amp[0] = 0.25;
    sim_run(&s, 0.5);
    enc[0] = mem_encoder(&mb);
    enc[1] = mem_encoder(&vb);
    CHECK(enc[0] && enc[1]);
    CHECK_EQ(fa_encoder_attach_mixer(enc[0], s.m, FA_MIXER_TAP_MASTER), FA_OK);
    CHECK_EQ(fa_encoder_attach_mixer(enc[1], s.m, FA_MIXER_TAP_VOCALS), FA_OK);
    sim_run(&s, 0.1);                   /* attached, not started: nothing yet */
    CHECK_EQ(fa_mixer_start_recording(s.m, enc, 2, 0, flags), FA_OK);
    CHECK_EQ(fa_mixer_start_recording(s.m, enc, 1, 0, flags), FA_ERROR_BUSY);
    from = s.frames + FA_MIXER_BLOCK;   /* the earliest start: one block ahead */
    sim_run(&s, 1.0);
    lat.struct_size = sizeof(lat);
    CHECK_EQ(fa_mixer_get_latency(s.m, &lat), FA_OK);
    /* Both start with what the mixer made on the start frame (the master's limiter
     * look-ahead allowed for); aligned, the vocals then start later by the round trip
     * the singer hears: the look-ahead, the master's OS latency and the mic's input. */
    shift = flags & FA_MIXER_REC_RAW_TIMING ? 0 : fa_mixer_vocal_shift(s.m);
    CHECK(flags & FA_MIXER_REC_RAW_TIMING ||
          llabs(shift - (int64_t)((double)(lat.mic_input[0] + fa_load64(&s.dev->latency_ns)) * 1e-9 * s.dev->rate) -
                s.dev->limiter.delay) <= 1);
    CHECK_EQ(fa_encoder_close(enc[0]), FA_OK);
    CHECK_EQ(fa_encoder_close(enc[1]), FA_OK);
    mf = wav_frames(&mb, &mn);
    vf = wav_frames(&vb, &vn);
    CHECK(mf && vf);
    CHECK_EQ(mn, s.frames - from - s.dev->limiter.delay);   /* exactly from the start frame */
    CHECK_EQ((int64_t)vn, (int64_t)(s.frames - from) - shift);
    /* Only the voice plays, at unity through the master: the vocals file is the master
     * file, `shift` frames on. */
    for (i = 0; i < vn && i + shift < mn; i++) {
        other = mf[(i + shift) * 2];
        if (fabsf(vf[i * 2] - other) > 1e-6f) bad++;
    }
    free(mf);
    free(vf);
    CHECK_EQ(bad, 0);
    free(mb.data);
    free(vb.data);
    sim_close(&s);
}

static void test_recording_aligned(void) { recording(0); }
static void test_recording_raw(void) { recording(FA_MIXER_REC_RAW_TIMING); }

static void test_recording_errors(void)
{
    sim s, t;
    membuf b;
    fa_encoder* e;
    CHECK(sim_open(&s));
    CHECK(sim_open(&t));
    e = mem_encoder(&b);
    CHECK(e != NULL);
    CHECK_EQ(fa_encoder_attach_mixer(e, s.m, (fa_mixer_tap)7), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_mixer_start_recording(s.m, &e, 1, 0, 0), FA_ERROR_INVALID_ARG);   /* not attached */
    CHECK_EQ(fa_encoder_attach_mixer(e, s.m, FA_MIXER_TAP_MASTER), FA_OK);
    CHECK_EQ(fa_encoder_attach_mixer(e, s.m, FA_MIXER_TAP_MASTER), FA_ERROR_BUSY);
    CHECK_EQ(fa_mixer_start_recording(t.m, &e, 1, 0, 0), FA_ERROR_INVALID_ARG);   /* another mixer */
    CHECK_EQ(fa_mixer_start_recording(s.m, &e, 1, 0, 1u << 5), FA_ERROR_INVALID_ARG);
    sim_close(&s);                      /* the mixer goes first: the encoder loses its source */
    CHECK_EQ(fa_encoder_close(e), FA_OK);
    free(b.data);
    sim_close(&t);
}

/* ---- the setup check ---------------------------------------------------------------
 * A simulated room: the mic hears the master `delay` frames later, through a gain, a
 * resonance (a peaking filter at 1 kHz) and some noise, while fa_mixer_measure_latency
 * runs on another thread. */
typedef struct room {
    int64_t delay;
    double gain, peak_db, noise_db;
    int64_t reflection;      /* a reflection this many frames after the direct sound (0 = none) */
    double reflection_gain;
} room;

typedef struct check_job {
    fa_mixer* m;
    fa_mic* mic;
    fa_latency_check_config cfg;
    fa_latency_check res;
    fa_result r;
    fa_atomic32 done;
} check_job;

static void check_thread(void* arg)
{
    check_job* j = (check_job*)arg;
    j->r = fa_mixer_measure_latency(j->m, j->mic, &j->cfg, &j->res);
    fa_store32(&j->done, 1);
}

static int room_check(sim* s, const room* rm, const fa_latency_check_config* cfg, fa_latency_check* out, fa_result* out_r)
{
    check_job job;
    fa_thread t;
    float* hist = NULL;
    uint32_t cap = 0, rendered = 0, fed = 0, rate = s->dev->rate, i;
    int64_t deadline;
    double A = pow(10.0, rm->peak_db / 40.0), w0 = 2.0 * k_pi * 1000.0 / rate, alpha = sin(w0) / 16.0;
    double a0 = 1.0 + alpha / A, b0 = (1.0 + alpha * A) / a0, b1 = -2.0 * cos(w0) / a0, b2 = (1.0 - alpha * A) / a0;
    double a1 = -2.0 * cos(w0) / a0, a2 = (1.0 - alpha / A) / a0, z1 = 0.0, z2 = 0.0;
    double noise = pow(10.0, rm->noise_db / 20.0) * 1.7320508;   /* uniform: RMS = amplitude / sqrt(3) */
    uint32_t seed = 12345u;
    float in[4800], blk[OUT_BLOCK * 2];
    double owed = 0.0;
    if (rm->delay < OUT_BLOCK) return 0;   /* the input runs up to a block ahead of the render */
    sim_run(s, 3.0);   /* the bridge fills and settles */
    memset(&job, 0, sizeof(job));
    job.m = s->m;
    job.mic = s->mic;
    job.res.struct_size = (uint32_t)sizeof(job.res);
    if (cfg) job.cfg = *cfg; else fa_latency_check_config_init(&job.cfg);
    if (fa_thread_create(&t, check_thread, &job) != 0) return 0;
    /* Wall time, not blocks: on a loaded machine the check thread can fall far behind. */
    deadline = fa_monotonic_ns() + 180 * (int64_t)1000000000;
    while (!fa_load32(&job.done) && fa_monotonic_ns() < deadline) {
        if (rendered + OUT_BLOCK > cap) {
            cap = cap ? cap * 2 : rate * 8;
            hist = (float*)realloc(hist, (size_t)cap * sizeof(float));
        }
        /* The input first, as sim_run: what the mic hears is `delay` behind, so it is
         * already rendered. */
        owed += OUT_BLOCK;
        while (owed >= (double)s->in_period) {
            for (i = 0; i < s->in_period; i++, fed++) {
                double x = fed >= rm->delay ? hist[fed - rm->delay] : 0.0, y;
                if (rm->reflection && fed >= rm->delay + rm->reflection)
                    x += rm->reflection_gain * hist[fed - rm->delay - rm->reflection];
                y = b0 * x + z1;
                z1 = b1 * x - a1 * y + z2;
                z2 = b2 * x - a2 * y;
                seed = seed * 1664525u + 1013904223u;
                in[i] = (float)(rm->gain * y + noise * ((double)seed / 2147483648.0 - 1.0));
            }
            fa_mic_test_feed(s->mic, in, s->in_period);
            owed -= (double)s->in_period;
        }
        fa_fetch_add64(&s->dev->epoch, 1);
        fa_device_render_output(s->dev, blk, 2, OUT_BLOCK, fa_monotonic_ns());
        fa_fetch_add64(&s->dev->epoch, 1);
        for (i = 0; i < OUT_BLOCK; i++) hist[rendered + i] = blk[i * 2];
        rendered += OUT_BLOCK;
        /* Between passes (settling, analysing) the check thread needs the time; during a
         * pass the blocks go as fast as they render. */
        if (!fa_load_ptr(&s->m->check)) fa_sleep_ms(1);
    }
    fa_thread_join(&t);
    free(hist);
    *out = job.res;
    *out_r = job.r;
    return 1;
}

static int check_sim(sim* s)
{
    if (!sim_open(s) || !sim_mic(s, 0, 48000, 48000.0, 1, NULL)) return 0;
    return s->dev->rate == 48000;
}

static double ms(fa_time t) { return (double)t / 1e6; }

static void test_check_delay_and_gain(void)
{
    sim s, t;
    room rm = { 480, 0.1, 12.0, -70.0, 0, 0.0 };
    fa_latency_check_config cfg;
    fa_latency_check a, b;
    fa_mixer_latency l;
    fa_result r;
    fa_latency_check_config_init(&cfg);
    cfg.max_monitor = 200000000;
    CHECK(check_sim(&s));
    CHECK(room_check(&s, &rm, &cfg, &a, &r));
    CHECK_EQ(r, FA_OK);
    CHECK_EQ(a.count, 1);
    CHECK_EQ(a.modes[0].mode, FA_LATENCY_NORMAL);
    CHECK_EQ(a.modes[0].result, FA_OK);
    CHECK_EQ(a.advice, FA_CHECK_MONITOR);
    CHECK_EQ(a.advised_mode, FA_LATENCY_NORMAL);
    CHECK(a.modes[0].level_db <= -30.0f);
    CHECK(ms(a.modes[0].spread) <= 1.0);
    /* 20 dB down, a 12 dB resonance and 6 dB to spare: +2 dB. */
    CHECK_NEAR(a.modes[0].max_gain_db, 2.0, 1.5);
    /* Kept: the latency report now gives the measured time. */
    l.struct_size = (uint32_t)sizeof(l);
    CHECK_EQ(fa_mixer_get_latency(s.m, &l), FA_OK);
    CHECK_EQ(l.mic_monitor[0], a.modes[0].mic_to_speaker);
    /* 40 ms more in the room is 40 ms more measured, to the frame. */
    rm.delay = 480 + 1920;
    CHECK(check_sim(&t));
    CHECK(room_check(&t, &rm, &cfg, &b, &r));
    CHECK_EQ(b.modes[0].result, FA_OK);
    CHECK_NEAR(ms(b.modes[0].mic_to_speaker - a.modes[0].mic_to_speaker), 40.0, 0.25);
    /* The rest is the bridge and the limiter's look-ahead (the simulated output has none). */
    CHECK(ms(a.modes[0].mic_to_speaker) > 10.0 && ms(a.modes[0].mic_to_speaker) < 40.0);
    sim_close(&s);
    sim_close(&t);
}

static void test_check_level_ramp(void)
{
    sim s;
    room rm = { 480, 0.1, 0.0, -45.0, 0, 0.0 };   /* noisy: the first probes drown */
    fa_latency_check a;
    fa_result r;
    CHECK(check_sim(&s));
    CHECK(room_check(&s, &rm, NULL, &a, &r));
    CHECK_EQ(r, FA_OK);
    CHECK_EQ(a.modes[0].result, FA_OK);
    CHECK(a.modes[0].level_db >= -30.0f && a.modes[0].level_db <= -12.0f);
    CHECK_NEAR(a.modes[0].max_gain_db, 14.0, 2.0);
    sim_close(&s);
}

static void test_check_reflection(void)
{
    sim s, t;
    room rm = { 480, 0.1, 0.0, -80.0, 48, 0.5 };   /* a hard wall right behind the mic: never "clear" */
    room plain = { 480, 0.1, 0.0, -80.0, 0, 0.0 };
    fa_latency_check a, b;
    fa_result r;
    CHECK(check_sim(&s));
    CHECK(room_check(&s, &rm, NULL, &a, &r));
    CHECK_EQ(r, FA_OK);
    CHECK_EQ(a.modes[0].result, FA_OK);
    CHECK(a.modes[0].level_db < -12.0f);   /* louder didn't help: the quieter level is kept */
    CHECK(check_sim(&t));
    CHECK(room_check(&t, &plain, NULL, &b, &r));
    CHECK_EQ(b.modes[0].result, FA_OK);
    /* The direct sound, not the reflection 1 ms later. */
    CHECK_NEAR(ms(a.modes[0].mic_to_speaker), ms(b.modes[0].mic_to_speaker), 0.25);
    sim_close(&s);
    sim_close(&t);
}

static void test_check_not_heard(void)
{
    sim s;
    room rm = { 480, 0.0, 0.0, -60.0, 0, 0.0 };
    fa_latency_check a;
    fa_result r;
    CHECK(check_sim(&s));
    CHECK(room_check(&s, &rm, NULL, &a, &r));
    CHECK_EQ(r, FA_OK);
    CHECK_EQ(a.modes[0].result, FA_ERROR_NOT_FOUND);
    CHECK_EQ(a.advice, FA_CHECK_NOT_HEARD);
    sim_close(&s);
}

static void test_check_advice(void)
{
    sim s, t;
    room loud = { 480, 1.0, 12.0, -70.0, 0, 0.0 }, far = { 2400, 0.1, 0.0, -70.0, 0, 0.0 };
    fa_latency_check_config cfg;
    fa_latency_check a;
    fa_result r;
    fa_latency_check_config_init(&cfg);
    cfg.max_monitor = 200000000;
    CHECK(check_sim(&s));
    CHECK(room_check(&s, &loud, &cfg, &a, &r));
    CHECK_EQ(a.modes[0].result, FA_OK);
    CHECK(a.modes[0].max_gain_db < 0.0f);
    CHECK_EQ(a.advice, FA_CHECK_FEEDBACK);
    CHECK(check_sim(&t));
    CHECK(room_check(&t, &far, NULL, &a, &r));   /* over the default 25 ms */
    CHECK_EQ(a.modes[0].result, FA_OK);
    CHECK(ms(a.modes[0].mic_to_speaker) > 50.0);
    CHECK_EQ(a.advice, FA_CHECK_SLOW);
    CHECK_EQ(a.advised_mode, FA_LATENCY_NORMAL);
    sim_close(&s);
    sim_close(&t);
}

static void test_check_all_modes(void)
{
    sim s;
    room rm = { 480, 0.1, 0.0, -70.0, 0, 0.0 };
    fa_latency_check_config cfg;
    fa_latency_check a;
    fa_latency_mode mode;
    fa_result r;
    fa_latency_check_config_init(&cfg);
    cfg.flags = FA_CHECK_ALL_MODES;
    cfg.max_monitor = 200000000;
    CHECK(check_sim(&s));
    CHECK(room_check(&s, &rm, &cfg, &a, &r));
    CHECK_EQ(r, FA_OK);
    CHECK_EQ(a.count, 3);
    CHECK_EQ(a.modes[0].mode, FA_LATENCY_NORMAL);
    CHECK_EQ(a.modes[1].mode, FA_LATENCY_LOW);
    CHECK_EQ(a.modes[2].mode, FA_LATENCY_EXCLUSIVE);
    CHECK_EQ(a.modes[0].result, FA_OK);
    CHECK_EQ(a.modes[1].result, FA_OK);
    CHECK_EQ(a.modes[2].result, FA_ERROR_NOT_IMPLEMENTED);   /* not Windows */
    CHECK_EQ(a.advice, FA_CHECK_MONITOR);
    CHECK_EQ(a.advised_mode, FA_LATENCY_LOW);   /* shared and fast enough: the cheapest */
    CHECK_EQ(fa_mixer_get_latency_mode(s.m, &mode), FA_OK);
    CHECK_EQ(mode, FA_LATENCY_NORMAL);          /* back where it was */
    sim_close(&s);
}

static void test_check_errors(void)
{
    sim s, t;
    fa_latency_check a;
    fa_latency_check_config cfg;
    CHECK(check_sim(&s));
    CHECK(check_sim(&t));
    a.struct_size = (uint32_t)sizeof(a);
    CHECK_EQ(fa_mixer_measure_latency(s.m, t.mic, NULL, &a), FA_ERROR_INVALID_ARG);   /* another mixer's mic */
    CHECK_EQ(fa_mixer_measure_latency(s.m, NULL, NULL, &a), FA_ERROR_INVALID_ARG);
    fa_latency_check_config_init(&cfg);
    cfg.flags = 1u << 7;
    CHECK_EQ(fa_mixer_measure_latency(s.m, s.mic, &cfg, &a), FA_ERROR_INVALID_ARG);
    /* The output doesn't run (nobody renders it): the device error, soon. */
    fa_latency_check_config_init(&cfg);
    CHECK_EQ(fa_mixer_measure_latency(s.m, s.mic, &cfg, &a), FA_ERROR_DEVICE);
    sim_close(&s);
    sim_close(&t);
}

static void test_latency_mode(void)
{
    fa_mixer_config c;
    fa_mixer* m;
    fa_mic* mic;
    fa_latency_mode mode;
    fa_mixer_config_init(&c);
    c.latency = FA_LATENCY_EXCLUSIVE;
    CHECK_EQ(fa_mixer_create(g_engine, &c, &m), FA_ERROR_NOT_IMPLEMENTED);   /* not Windows */
    c.latency = FA_LATENCY_NORMAL;
    CHECK_EQ(fa_mixer_create(g_engine, &c, &m), FA_OK);
    CHECK_EQ(fa_mixer_add_mic(m, NULL, &mic), FA_OK);
    CHECK_EQ(fa_mixer_get_latency_mode(m, &mode), FA_OK);
    CHECK_EQ(mode, FA_LATENCY_NORMAL);
    CHECK_EQ(fa_mixer_set_latency_mode(m, FA_LATENCY_LOW), FA_OK);
    CHECK_EQ(fa_mixer_get_latency_mode(m, &mode), FA_OK);
    CHECK_EQ(mode, FA_LATENCY_LOW);
    CHECK_EQ(fa_mixer_set_latency_mode(m, FA_LATENCY_EXCLUSIVE), FA_ERROR_NOT_IMPLEMENTED);
    CHECK_EQ(fa_mixer_set_latency_mode(m, (fa_latency_mode)7), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_mixer_get_latency_mode(m, &mode), FA_OK);
    CHECK_EQ(mode, FA_LATENCY_LOW);
    CHECK_EQ(fa_load32(&mic->cap->lost), 0);    /* the input opened again */
    CHECK(mic->cap->device_ok);
    CHECK_EQ(fa_mixer_set_latency_mode(m, FA_LATENCY_NORMAL), FA_OK);
    fa_mixer_destroy(m);
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
    RUN(test_effects_settings);
    RUN(test_reverb);
    RUN(test_echo);
    RUN(test_ducking);
    RUN(test_recording_aligned);
    RUN(test_recording_raw);
    RUN(test_recording_errors);
    RUN(test_check_delay_and_gain);
    RUN(test_check_level_ramp);
    RUN(test_check_reflection);
    RUN(test_check_not_heard);
    RUN(test_check_advice);
    RUN(test_check_all_modes);
    RUN(test_check_errors);
    RUN(test_latency_mode);

    fa_engine_destroy(g_engine);
    if (g_failures) { fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    printf("all mic tests passed\n");
    return 0;
}
