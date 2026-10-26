/* White-box tests of the mixer's mics. The mixer runs on the null backend with its
 * device stopped, and each mic's input device is stopped too (fa_mic_test_take_over):
 * this test plays both audio threads, feeding the input in its periods and rendering
 * the master in its own, at chosen rates, and checks the samples and the bridge. */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "mixer.h"
#include "cdg.h"
#include <math.h>
#include <stddef.h>
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
    fa_stream* music2;           /* on deck B */
    /* The headphones of FA_CUE_DEVICE, when the test drives them: read in their periods at
     * their true rate, everything they played kept in cue_out. */
    int cue;
    uint32_t cue_rate, cue_period;
    double cue_true, cue_owed;
    float* cue_out;
    uint32_t cue_frames, cue_cap;
    double* cue_t;               /* per period read: the master time it was read at (s) */
    int32_t max_align;           /* the most dev->align_frames after a block */
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

/* A mixer with its headphones on a second device that this test reads: at `rate` (really
 * `true_rate` per second of master time), in periods of `period`, reporting `os_ns`. */
static int sim_open_cue(sim* s, uint32_t flags, uint32_t rate, double true_rate, uint32_t period, fa_time os_ns)
{
    fa_mixer_config c;
    memset(s, 0, sizeof(*s));
    fa_mixer_config_init(&c);
    c.latency = FA_LATENCY_NORMAL;
    c.cue_mode = FA_CUE_DEVICE;
    c.flags = flags;
    if (fa_mixer_create(g_engine, &c, &s->m) != FA_OK) return 0;
    fa_mixer_get_device(s->m, &s->dev);
    fa_device_stop(s->dev);
    fa_mixer_test_cue_take_over(s->m, rate, period, os_ns);
    s->cue = 1;
    s->cue_rate = rate;
    s->cue_true = true_rate;
    s->cue_period = period;
    return 1;
}

static void sim_run(sim* s, double seconds);

/* sim_open_cue, then a moment for the alignment the real devices made before the test
 * took over (the master may have been delayed) to give way to the test's; what was
 * played meanwhile is forgotten. */
static int sim_open_cue_settled(sim* s, uint32_t flags, uint32_t rate, double true_rate, uint32_t period, fa_time os_ns)
{
    if (!sim_open_cue(s, flags, rate, true_rate, period, os_ns)) return 0;
    sim_run(s, 0.1);
    s->frames = 0;
    s->cue_frames = 0;
    s->max_align = 0;
    return 1;
}

static void sim_close(sim* s)
{
    fa_mixer_destroy(s->m);
    if (s->music) fa_stream_close(s->music);
    if (s->music2) fa_stream_close(s->music2);
    free(s->out);
    free(s->cue_out);
    free(s->cue_t);
}

/* A 16-bit mono WAV: `start` frames of silence, then a sine at amplitude 0.25. */
static uint8_t* tone_wav(uint32_t rate, uint32_t frames, uint32_t start, double hz, size_t* size)
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
        int16_t sv = i < start ? 0 : (int16_t)lrint(8192.0 * sin(2.0 * FA_PI * hz * (i - start) / rate));
        memcpy(b + 44 + (size_t)i * 2, &sv, 2);
    }
    *size = n;
    return b;
}

static uint8_t* sine_wav(uint32_t rate, uint32_t frames, double hz, size_t* size)
{
    return tone_wav(rate, frames, 0, hz, size);
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

/* Until the decks' workers have the next block decoded (or 2 s). */
static void sim_wait_decoded(sim* s)
{
    int w;
    for (w = 0; s->music && w < 2000; w++) {
        if ((fa_ring_readable(&s->music->ring) >= OUT_BLOCK || fa_load32(&s->music->eof)) &&
            (!s->music2 || fa_ring_readable(&s->music2->ring) >= OUT_BLOCK || fa_load32(&s->music2->eof)))
            break;
        fa_sleep_ms(1);
    }
}

/* One master block of OUT_BLOCK, rendered at `now_ns`, and the input periods and the
 * headphones' periods that fell due. */
static void sim_step(sim* s, int64_t now_ns)
{
    float in[2 * 4800];
    if (s->frames + OUT_BLOCK > s->cap) {
        s->cap = (s->frames + OUT_BLOCK) * 2;
        s->out = (float*)realloc(s->out, (size_t)s->cap * 2 * sizeof(float));
    }
    {
        sim_wait_decoded(s);
        s->owed += (double)OUT_BLOCK * s->in_true / (double)s->dev->rate;
        while (s->mic && s->owed >= (double)s->in_period) {
            uint32_t i, c;
            for (i = 0; i < s->in_period; i++)
                for (c = 0; c < s->in_ch; c++) {
                    in[i * s->in_ch + c] = (float)(s->amp[c] * sin(s->phase[c]));
                    s->phase[c] += 2.0 * FA_PI * s->hz[c] / (double)s->in_rate;
                }
            fa_mic_test_feed(s->mic, in, s->in_period);
            s->owed -= (double)s->in_period;
        }
        fa_fetch_add64(&s->dev->epoch, 1);
        fa_device_render_output(s->dev, s->out + (size_t)s->frames * 2, 2, OUT_BLOCK, now_ns);
        fa_fetch_add64(&s->dev->epoch, 1);
        s->frames += OUT_BLOCK;
        if (fa_load32(&s->dev->align_frames) > s->max_align) s->max_align = fa_load32(&s->dev->align_frames);
        /* The headphones read what fell due meanwhile, in their periods. */
        s->cue_owed += s->cue ? (double)OUT_BLOCK * s->cue_true / (double)s->dev->rate : 0.0;
        while (s->cue && s->cue_owed >= (double)s->cue_period) {
            if (s->cue_frames + s->cue_period > s->cue_cap) {
                s->cue_cap = (s->cue_frames + s->cue_period) * 2;
                s->cue_out = (float*)realloc(s->cue_out, (size_t)s->cue_cap * 2 * sizeof(float));
            }
            fa_mixer_test_cue_pull(s->m, s->cue_out + (size_t)s->cue_frames * 2, s->cue_period);
            s->cue_t = (double*)realloc(s->cue_t, (size_t)(s->cue_cap / s->cue_period + 1) * sizeof(double));
            s->cue_t[s->cue_frames / s->cue_period] = (double)s->frames / s->dev->rate;
            s->cue_frames += s->cue_period;
            s->cue_owed -= (double)s->cue_period;
        }
    }
}

/* Runs `seconds` of master time: input periods as they fall due, master blocks of OUT_BLOCK. */
static void sim_run(sim* s, double seconds)
{
    uint32_t total = ((uint32_t)(seconds * s->dev->rate) + OUT_BLOCK - 1) / OUT_BLOCK * OUT_BLOCK, done;
    for (done = 0; done < total; done += OUT_BLOCK) sim_step(s, fa_monotonic_ns());
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
    CHECK_EQ(fa_load32(&s.mic->cap->br.overruns), 0);
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
    /* Stopped and moved to the new default by the backend: not reported. */
    fa_mic_test_lose(mic);
    fa_sleep_ms(50);
    fa_mic_test_back(mic);
    fa_sleep_ms(400);
    CHECK_EQ(r.count, 0);
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
    double A = pow(10.0, rm->peak_db / 40.0), w0 = 2.0 * FA_PI * 1000.0 / rate, alpha = sin(w0) / 16.0;
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
    /* The direct sound, not the reflection 1 ms later. Two checks can differ by a few
     * frames (the block the check thread starts on), so the line is halfway. */
    CHECK_NEAR(ms(a.modes[0].mic_to_speaker), ms(b.modes[0].mic_to_speaker), 0.5);
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

/* The mic removed while a check measures it (an INPUT_LOST handler on the event thread,
 * say): the check keeps it until it returns, then frees it. */
static void test_check_mic_removed(void)
{
    sim s;
    check_job job;
    fa_thread t;
    CHECK(check_sim(&s));
    memset(&job, 0, sizeof(job));
    job.m = s.m;
    job.mic = s.mic;
    job.res.struct_size = (uint32_t)sizeof(job.res);
    fa_latency_check_config_init(&job.cfg);
    CHECK(fa_thread_create(&t, check_thread, &job) == 0);
    while (!fa_load32(&s.m->checking) && !fa_load32(&job.done)) fa_sleep_ms(1);
    fa_mic_remove(s.mic);   /* nobody renders: the check is waiting for the output */
    s.mic = NULL;
    CHECK(fa_load_ptr(&s.m->mics[0]) == NULL);
    fa_thread_join(&t);
    CHECK(job.r != FA_OK);
    CHECK_EQ(fa_load32(&s.m->checking), 0);
    CHECK(s.m->check_mic == NULL);
    sim_close(&s);
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

/* ---- the second-device cue -------------------------------------------------------- */

/* Music on deck A, sent to the headphones (cue only): `start` s of silence, then a sine. */
static int sim_cue_music(sim* s, double hz, double start, double seconds)
{
    size_t n;
    fa_deck* a;
    uint8_t* wav = tone_wav(s->dev->rate, (uint32_t)(seconds * s->dev->rate), (uint32_t)(start * s->dev->rate), hz, &n);
    int ok = fa_stream_open_memory(g_engine, wav, n, NULL, &s->music) == FA_OK;
    free(wav);
    fa_mixer_get_deck(s->m, 0, &a);
    return ok && fa_deck_load(a, s->music, 0) == FA_OK && fa_deck_set_param(a, FA_DECK_CUE, 1.0f) == FA_OK &&
           fa_stream_play(s->music) == FA_OK;
}

/* RMS and upward zero crossings per second (at the rate they claim) of the headphones'
 * channel `c` over the last `seconds`. */
static double cue_rms(const sim* s, uint32_t c, double seconds)
{
    uint32_t n = (uint32_t)(seconds * s->cue_rate), i;
    double sq = 0.0;
    for (i = s->cue_frames - n; i < s->cue_frames; i++) sq += (double)s->cue_out[i * 2 + c] * s->cue_out[i * 2 + c];
    return sqrt(sq / n);
}

static double cue_hz(const sim* s, uint32_t c, double seconds)
{
    uint32_t n = (uint32_t)(seconds * s->cue_rate), i, k = 0;
    for (i = s->cue_frames - n + 1; i < s->cue_frames; i++)
        if (s->cue_out[(i - 1) * 2 + c] < 0.0f && s->cue_out[i * 2 + c] >= 0.0f) k++;
    return (double)k / seconds;
}

/* The first frame of channel 0 over a quarter of the peak (stereo frames), or -1. */
static int64_t first_over(const float* x, uint32_t frames)
{
    uint32_t i;
    float peak = 0.0f;
    for (i = 0; i < frames; i++) peak = fabsf(x[i * 2]) > peak ? fabsf(x[i * 2]) : peak;
    if (peak < 0.01f) return -1;
    for (i = 0; i < frames; i++)
        if (fabsf(x[i * 2]) > 0.25f * peak) return i;
    return -1;
}

/* How much later the headphones play the music's start than the speakers, in ms. Each
 * output plays its frames one after the other at its own rate, starting its reported
 * latency after it was handed them: the master was handed frame i at i / rate; the
 * headphones' reads fall on master blocks, so their frames' times are fitted to the
 * reads (on average a read hands its first frame over when it happens). The limiter's
 * look-ahead is already in the master's samples. */
static double cue_lag_ms(const sim* s, fa_time cue_os)
{
    int64_t tm = first_over(s->out, s->frames), tc = first_over(s->cue_out, s->cue_frames);
    uint32_t k, reads = s->cue_frames / s->cue_period;
    double hm, hc, offset = 0.0;
    if (tm < 0 || tc < 0 || reads == 0) return 1e9;
    for (k = 0; k < reads; k++) offset += s->cue_t[k] - (double)k * s->cue_period / s->cue_true;
    offset /= reads;
    hm = (double)tm / s->dev->rate + (double)fa_load64(&s->dev->latency_ns) / 1e9;
    hc = (double)tc / s->cue_true + offset + (double)cue_os / 1e9;
    return (hc - hm) * 1000.0;
}

static fa_mixer_latency latency_of(sim* s)
{
    fa_mixer_latency l;
    memset(&l, 0, sizeof(l));
    l.struct_size = sizeof(l);
    fa_mixer_get_latency(s->m, &l);
    return l;
}

/* The headphones' clock `ppm` fast or slow: the bridge holds its target, no dropout, and
 * the tone comes out at its level. */
static void cue_drift(double ppm)
{
    sim s;
    double target = 0.0, fill = 0.0;
    int32_t underruns = -1, overruns = -1;
    uint32_t rate;
    CHECK(sim_open(&s));   /* for the device's rate */
    rate = s.dev->rate;
    sim_close(&s);
    CHECK(sim_open_cue_settled(&s, 0, rate, rate * (1.0 + ppm * 1e-6), rate / 100, 5000000));
    CHECK(sim_cue_music(&s, 1000.0, 0.0, 40.0));
    sim_run(&s, 2.0);
    fa_mixer_test_cue_stats(s.m, NULL, NULL, &underruns, NULL);
    CHECK_EQ(underruns, 0);
    sim_run(&s, 30.0);
    fa_mixer_test_cue_stats(s.m, &target, &fill, &underruns, &overruns);
    CHECK_EQ(underruns, 0);
    CHECK_EQ(overruns, 0);
    CHECK(fabs(fill - target) < rate * 0.012);   /* within a period of the target */
    CHECK_NEAR(cue_rms(&s, 0, 1.0) / k_sine_rms, 1.0, 0.01);
    CHECK_NEAR(cue_hz(&s, 1, 1.0), 1000.0, 3.0);
    sim_close(&s);
}

static void test_cue_drift_fast(void) { cue_drift(400.0); }
static void test_cue_drift_slow(void) { cue_drift(-400.0); }

/* 44.1 kHz headphones on the master's rate: the same pitch and level. */
static void test_cue_other_rate(void)
{
    sim s;
    int32_t underruns = -1;
    CHECK(sim_open_cue_settled(&s, 0, 44100, 44100.0, 441, 5000000));
    CHECK(sim_cue_music(&s, 1000.0, 0.0, 4.0));
    sim_run(&s, 3.0);
    CHECK_NEAR(cue_hz(&s, 0, 1.0), 1000.0, 3.0);
    CHECK_NEAR(cue_rms(&s, 0, 1.0) / k_sine_rms, 1.0, 0.01);
    fa_mixer_test_cue_stats(s.m, NULL, NULL, &underruns, NULL);
    CHECK_EQ(underruns, 0);
    sim_close(&s);
}

/* Headphones faster than the speakers: they are delayed, and the music's start reaches
 * both at the same time. */
static void test_cue_aligns_cue(void)
{
    sim s;
    fa_mixer_latency l;
    fa_time lat = 0;
    CHECK(sim_open_cue_settled(&s, 0, 48000, 48000.0, 480, 1000000));
    CHECK(sim_cue_music(&s, 1000.0, 0.5, 1.5));
    sim_run(&s, 1.0);
    l = latency_of(&s);
    CHECK_EQ(l.cue_align, FA_CUE_ALIGN_CUE);
    CHECK(l.cue_alignment > 1000000);
    CHECK_NEAR((double)l.cue / 1e6, (double)l.master / 1e6, 0.1);   /* lined up, as reported */
    CHECK_EQ(s.max_align, 0);                                          /* the master is not delayed */
    CHECK_EQ(fa_device_get_latency(s.dev, &lat), FA_OK);
    CHECK_EQ(lat, l.master);
    printf("  cue delayed %.2f ms, lag %.3f ms\n", l.cue_alignment / 1e6, cue_lag_ms(&s, 1000000));
    CHECK_NEAR(cue_lag_ms(&s, 1000000), 0.0, 1.0);
    sim_close(&s);
}

/* Headphones slower than the speakers: the master is delayed, which every heard time
 * includes, and the music's start reaches both at the same time. */
static void test_cue_aligns_master(void)
{
    sim s;
    fa_mixer_latency l;
    fa_time lat = 0, os;
    CHECK(sim_open_cue_settled(&s, 0, 48000, 48000.0, 480, 120000000));
    CHECK(sim_cue_music(&s, 1000.0, 0.5, 1.5));
    sim_run(&s, 1.0);
    l = latency_of(&s);
    os = fa_load64(&s.dev->latency_ns);
    CHECK_EQ(l.cue_align, FA_CUE_ALIGN_MASTER);
    CHECK(l.cue_alignment > 50000000);
    CHECK_NEAR((double)l.cue / 1e6, (double)l.master / 1e6, 0.1);
    CHECK_NEAR((double)fa_load32(&s.dev->align_frames) / s.dev->rate * 1e3, (double)l.cue_alignment / 1e6, 0.03);
    CHECK_EQ(fa_device_get_latency(s.dev, &lat), FA_OK);
    CHECK_EQ(lat, l.master);
    CHECK_NEAR((double)(lat - os) / 1e6, (double)l.cue_alignment / 1e6 + s.dev->limiter.delay * 1e3 / s.dev->rate, 0.03);
    printf("  master delayed %.2f ms, lag %.3f ms\n", l.cue_alignment / 1e6, cue_lag_ms(&s, 120000000));
    CHECK_NEAR(cue_lag_ms(&s, 120000000), 0.0, 1.0);
    sim_close(&s);
}

/* The trim moves the line-up by ear; FA_MIXER_CUE_LOWEST_LATENCY turns it off. */
static void test_cue_trim_and_off(void)
{
    sim s;
    fa_mixer_latency l0, l1;
    double t0 = 0.0, t1 = 0.0;
    float v = 0.0f;
    CHECK(sim_open_cue_settled(&s, 0, 48000, 48000.0, 480, 1000000));
    CHECK(sim_cue_music(&s, 1000.0, 0.0, 3.0));
    sim_run(&s, 0.5);
    l0 = latency_of(&s);
    fa_mixer_test_cue_stats(s.m, &t0, NULL, NULL, NULL);
    CHECK_EQ(fa_mixer_set_param(s.m, FA_MIX_CUE_TRIM_MS, 20.0f), FA_OK);
    sim_run(&s, 0.5);
    l1 = latency_of(&s);
    fa_mixer_test_cue_stats(s.m, &t1, NULL, NULL, NULL);
    CHECK_EQ(l1.cue_align, FA_CUE_ALIGN_CUE);
    CHECK_NEAR((double)(l1.cue_alignment - l0.cue_alignment) / 1e6, 20.0, 0.05);
    CHECK_NEAR((t1 - t0) / s.dev->rate * 1e3, 20.0, 0.05);   /* the bridge holds that much more */
    /* Trimmed the other way past zero: the master is delayed instead. */
    CHECK_EQ(fa_mixer_set_param(s.m, FA_MIX_CUE_TRIM_MS, (float)(-(double)l0.cue_alignment / 1e6 - 10.0)), FA_OK);
    sim_run(&s, 0.5);
    l1 = latency_of(&s);
    CHECK_EQ(l1.cue_align, FA_CUE_ALIGN_MASTER);
    CHECK_NEAR((double)l1.cue_alignment / 1e6, 10.0, 0.05);
    CHECK(s.max_align > 0);
    CHECK_EQ(fa_mixer_set_param(s.m, FA_MIX_CUE_TRIM_MS, 900.0f), FA_OK);
    CHECK_EQ(fa_mixer_get_param(s.m, FA_MIX_CUE_TRIM_MS, &v), FA_OK);
    CHECK_NEAR(v, 250.0, 1e-6);
    CHECK_EQ(fa_mixer_ramp_param(s.m, FA_MIX_CUE_TRIM_MS, 0.0f, 1000000, 0, NULL), FA_ERROR_INVALID_ARG);
    sim_close(&s);
    /* Off: nothing is delayed, whatever the latencies. */
    CHECK(sim_open_cue_settled(&s, FA_MIXER_CUE_LOWEST_LATENCY, 48000, 48000.0, 480, 120000000));
    CHECK(sim_cue_music(&s, 1000.0, 0.0, 2.0));
    sim_run(&s, 1.0);
    l1 = latency_of(&s);
    CHECK_EQ(l1.cue_align, FA_CUE_ALIGN_OFF);
    CHECK_EQ(l1.cue_alignment, 0);
    CHECK_EQ(s.max_align, 0);
    CHECK_EQ(fa_load32(&s.m->cue_dev->br.extra), 0);
    CHECK_NEAR(cue_rms(&s, 0, 0.5) / k_sine_rms, 1.0, 0.01);
    sim_close(&s);
}

/* MediaControl's rule: while a mic is monitored the master is never delayed. */
static void test_cue_never_delays_a_monitored_mic(void)
{
    sim s;
    fa_mic* mic;
    fa_mixer_latency l;
    fa_time lat = 0;
    /* A mic that is not monitored: the slower headphones get the master delayed, and the
     * mic's way to the speaker includes it. */
    CHECK(sim_open_cue_settled(&s, 0, 48000, 48000.0, 480, 120000000));
    CHECK(sim_mic(&s, -1, s.dev->rate, s.dev->rate, 1, &mic));
    CHECK_EQ(fa_mic_set_param(mic, FA_MIC_MONITOR, 0.0f), FA_OK);
    CHECK(sim_cue_music(&s, 1000.0, 0.0, 4.0));
    sim_run(&s, 1.0);
    l = latency_of(&s);
    CHECK_EQ(l.cue_align, FA_CUE_ALIGN_MASTER);
    CHECK(fa_load32(&s.dev->align_frames) > 0);
    CHECK_EQ(l.mic_monitor[0], l.mic_input[0] + l.master);
    CHECK(l.master > fa_load64(&s.dev->latency_ns) + l.cue_alignment);
    /* Monitored now: the delay goes at once (one fade), and alignment is skipped. */
    CHECK_EQ(fa_mic_set_param(mic, FA_MIC_MONITOR, 1.0f), FA_OK);
    sim_run(&s, 0.05);
    l = latency_of(&s);
    CHECK_EQ(fa_load32(&s.dev->align_frames), 0);
    CHECK_EQ(l.cue_align, FA_CUE_ALIGN_SKIPPED);
    CHECK_EQ(l.cue_alignment, 0);
    CHECK_EQ(fa_device_get_latency(s.dev, &lat), FA_OK);
    CHECK_EQ(lat, fa_load64(&s.dev->latency_ns) + (fa_time)((double)s.dev->limiter.delay * 1e9 / s.dev->rate));
    CHECK(l.cue - l.master > 50000000);   /* how late the headphones are */
    s.max_align = 0;
    sim_run(&s, 1.0);
    CHECK_EQ(s.max_align, 0);
    sim_close(&s);
    /* Monitored from the start: never delayed, not for one block. */
    CHECK(sim_open_cue_settled(&s, 0, 48000, 48000.0, 480, 120000000));
    CHECK(sim_mic(&s, -1, s.dev->rate, s.dev->rate, 1, &mic));
    CHECK(sim_cue_music(&s, 1000.0, 0.0, 3.0));
    sim_run(&s, 2.0);
    CHECK_EQ(s.max_align, 0);
    l = latency_of(&s);
    CHECK_EQ(l.cue_align, FA_CUE_ALIGN_SKIPPED);
    /* Faster headphones are still lined up: delaying them never touches the singer. */
    fa_mixer_test_cue_take_over(s.m, 48000, 480, 1000000);
    sim_run(&s, 0.5);
    l = latency_of(&s);
    CHECK_EQ(l.cue_align, FA_CUE_ALIGN_CUE);
    CHECK_EQ(s.max_align, 0);
    sim_close(&s);
}

static void FA_CALL on_cue_event(const fa_mixer_event* ev, void* user)
{
    lost_rec* r = (lost_rec*)user;
    fa_mutex_lock(&r->lock);
    if (ev->type == FA_EVENT_CUE_LOST && ev->mic == NULL && ev->index == 0) r->count++;
    fa_mutex_unlock(&r->lock);
}

/* Headphones that go away: one event, silence there, the master untouched; other
 * headphones can be opened. */
static void test_cue_lost(void)
{
    sim s;
    lost_rec r;
    fa_mixer_config c;
    fa_mixer* m;
    int32_t cue_ch = 5;
    uint32_t ch = 0;
    memset(&r, 0, sizeof(r));
    fa_mutex_init(&r.lock);
    CHECK(sim_open_cue_settled(&s, 0, 48000, 48000.0, 480, 1000000));
    CHECK(sim_cue_music(&s, 1000.0, 0.0, 4.0));
    CHECK_EQ(fa_mixer_set_event_callback(s.m, on_cue_event, &r), FA_OK);
    CHECK_EQ(fa_mixer_get_output_channels(s.m, &ch, &cue_ch), FA_OK);
    CHECK_EQ(cue_ch, -1);
    sim_run(&s, 1.0);
    fa_mixer_test_cue_lose(s.m);
    CHECK_EQ(wait_lost(&r), 1);
    fa_sleep_ms(20);
    CHECK_EQ(r.count, 1);
    s.cue = 0;   /* the headphones read no more */
    sim_run(&s, 0.5);
    CHECK_NEAR(last_rms(&s, 0, 0.25) / (k_sine_rms * 0.7071), 1.0, 0.02);   /* the master plays on */
    /* Open again (the null backend's default output). */
    CHECK_EQ(fa_mixer_set_cue_device(s.m, NULL), FA_OK);
    CHECK(s.m->cue_dev->device_ok);
    CHECK_EQ(fa_load32(&s.m->cue_dev->lost), 0);
    CHECK_EQ(fa_mixer_set_cue_device(s.m, "zz"), FA_ERROR_INVALID_ARG);   /* not an id: nothing changes */
    CHECK(s.m->cue_dev->device_ok);
    CHECK_EQ(fa_load32(&s.m->cue_dev->running), 1);
    CHECK_EQ(fa_mixer_set_event_callback(s.m, NULL, NULL), FA_OK);
    sim_close(&s);
    fa_mutex_destroy(&r.lock);
    /* Only for FA_CUE_DEVICE. */
    fa_mixer_config_init(&c);
    c.latency = FA_LATENCY_NORMAL;
    CHECK_EQ(fa_mixer_create(g_engine, &c, &m), FA_OK);
    CHECK_EQ(fa_mixer_set_cue_device(m, NULL), FA_ERROR_INVALID_ARG);
    fa_mixer_destroy(m);
}

/* The headphones follow a latency mode switch. */
static void test_cue_latency_mode(void)
{
    fa_mixer_config c;
    fa_mixer* m;
    fa_mixer_config_init(&c);
    c.latency = FA_LATENCY_NORMAL;
    c.cue_mode = FA_CUE_DEVICE;
    CHECK_EQ(fa_mixer_create(g_engine, &c, &m), FA_OK);
    CHECK(m->cue_dev->device_ok);
    CHECK_EQ(fa_mixer_set_latency_mode(m, FA_LATENCY_LOW), FA_OK);
    CHECK(m->cue_dev->device_ok);
    CHECK_EQ(fa_load32(&m->cue_dev->lost), 0);
    CHECK_EQ(fa_load32(&m->cue_dev->running), 1);
    fa_mixer_destroy(m);
}

/* ---- heard positions with a delayed master ---------------------------------------- */

typedef struct tbuf { uint8_t* d; size_t n, cap; } tbuf;

static void tput(tbuf* b, const void* p, size_t n)
{
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 64;
        b->d = (uint8_t*)realloc(b->d, b->cap);
    }
    memcpy(b->d + b->n, p, n);
    b->n += n;
}
static void tput8(tbuf* b, unsigned v) { uint8_t x = (uint8_t)v; tput(b, &x, 1); }
static void tput16(tbuf* b, unsigned v) { tput8(b, v & 255); tput8(b, v >> 8); }
static void tput32(tbuf* b, uint32_t v) { tput16(b, v & 0xFFFF); tput16(b, v >> 16); }
static void tput32be(tbuf* b, uint32_t v) { tput8(b, v >> 24); tput8(b, (v >> 16) & 255); tput8(b, (v >> 8) & 255); tput8(b, v & 255); }
static void tstr(tbuf* b, const char* s, size_t n) { size_t k = strlen(s); tput(b, s, k < n ? k : n); while (k++ < n) tput8(b, 0); }
static void tchunk(tbuf* b, const char* id, const tbuf* body)
{
    tput(b, id, 4);
    tput32(b, (uint32_t)body->n);
    tput(b, body->d, body->n);
    if (body->n & 1) tput8(b, 0);
}
static void tgen(tbuf* b, unsigned oper, unsigned amount) { tput16(b, oper); tput16(b, amount); }

/* test_lyric.c's soundfont: one preset, a looped 440 Hz sine at 44.1 kHz. */
static tbuf make_sf2(void)
{
    tbuf sf = { 0 }, riff = { 0 }, info = { 0 }, sdta = { 0 }, pdta = { 0 }, c = { 0 };
    int i;
    tput(&riff, "sfbk", 4);
    tput(&info, "INFO", 4);
    c.n = 0; tput16(&c, 2); tput16(&c, 1); tchunk(&info, "ifil", &c);
    c.n = 0; tstr(&c, "Test Sine", 10); tchunk(&info, "INAM", &c);
    tchunk(&riff, "LIST", &info);
    tput(&sdta, "sdta", 4);
    c.n = 0;
    for (i = 0; i < 4410; i++) tput16(&c, (unsigned)(uint16_t)(int16_t)lrint(sin(2.0 * FA_PI * 440.0 * i / 44100) * 32000.0));
    for (i = 0; i < 46; i++) tput16(&c, 0);
    tchunk(&sdta, "smpl", &c);
    tchunk(&riff, "LIST", &sdta);
    tput(&pdta, "pdta", 4);
    c.n = 0;
    tstr(&c, "Sine", 20); tput16(&c, 0); tput16(&c, 0); tput16(&c, 0); tput32(&c, 0); tput32(&c, 0); tput32(&c, 0);
    tstr(&c, "EOP", 20); tput16(&c, 0); tput16(&c, 0); tput16(&c, 1); tput32(&c, 0); tput32(&c, 0); tput32(&c, 0);
    tchunk(&pdta, "phdr", &c);
    c.n = 0; tput16(&c, 0); tput16(&c, 0); tput16(&c, 1); tput16(&c, 0);
    tchunk(&pdta, "pbag", &c);
    c.n = 0; for (i = 0; i < 10; i++) tput8(&c, 0);
    tchunk(&pdta, "pmod", &c);
    c.n = 0; tgen(&c, 41, 0); tgen(&c, 0, 0);
    tchunk(&pdta, "pgen", &c);
    c.n = 0;
    tstr(&c, "SineInst", 20); tput16(&c, 0);
    tstr(&c, "EOI", 20); tput16(&c, 1);
    tchunk(&pdta, "inst", &c);
    c.n = 0; tput16(&c, 0); tput16(&c, 0); tput16(&c, 3); tput16(&c, 0);
    tchunk(&pdta, "ibag", &c);
    c.n = 0; for (i = 0; i < 10; i++) tput8(&c, 0);
    tchunk(&pdta, "imod", &c);
    c.n = 0;
    tgen(&c, 54, 1);
    tgen(&c, 38, (unsigned)(uint16_t)-3600);
    tgen(&c, 53, 0);
    tgen(&c, 0, 0);
    tchunk(&pdta, "igen", &c);
    c.n = 0;
    tstr(&c, "Sine", 20); tput32(&c, 0); tput32(&c, 4410); tput32(&c, 0); tput32(&c, 4410);
    tput32(&c, 44100); tput8(&c, 69); tput8(&c, 0); tput16(&c, 0); tput16(&c, 1);
    tstr(&c, "EOS", 20); for (i = 0; i < 26; i++) tput8(&c, 0);
    tchunk(&pdta, "shdr", &c);
    tchunk(&riff, "LIST", &pdta);
    tchunk(&sf, "RIFF", &riff);
    free(riff.d); free(info.d); free(sdta.d); free(pdta.d); free(c.d);
    return sf;
}

static void tvarlen(tbuf* b, uint32_t v)
{
    uint8_t tmp[5];
    int n = 0;
    tmp[n++] = v & 0x7F;
    while (v >>= 7) tmp[n++] = (uint8_t)(0x80 | (v & 0x7F));
    while (n--) tput8(b, tmp[n]);
}

/* A MIDI song at 960 ticks a second: a note held 4 s, the words "AB" (1 s) "BA" (1.5 s)
 * "VO" (2 s) (the test font has A B O V and space). */
static tbuf make_song(void)
{
    tbuf f = { 0 }, t = { 0 };
    static const char* words[3] = { "AB", "BA ", "VO" };
    static const uint32_t at[3] = { 960, 480, 480 };
    int i;
    tput(&f, "MThd", 4); tput32be(&f, 6); tput8(&f, 0); tput8(&f, 0); tput8(&f, 0); tput8(&f, 1);
    tput8(&f, 480 >> 8); tput8(&f, 480 & 255);
    tvarlen(&t, 0); tput8(&t, 0x90); tput8(&t, 69); tput8(&t, 1);
    for (i = 0; i < 3; i++) {
        tvarlen(&t, at[i]); tput8(&t, 0xFF); tput8(&t, 0x05); tvarlen(&t, (uint32_t)strlen(words[i]));
        tput(&t, words[i], strlen(words[i]));
    }
    tvarlen(&t, 1920); tput8(&t, 0x80); tput8(&t, 69); tput8(&t, 0);
    tvarlen(&t, 0); tput8(&t, 0xFF); tput8(&t, 0x2F); tput8(&t, 0);
    tput(&f, "MTrk", 4); tput32be(&f, (uint32_t)t.n); tput(&f, t.d, t.n);
    free(t.d);
    return f;
}

static void cdg_pack(tbuf* b, int instruction, const uint8_t data[16])
{
    uint8_t p[FA_CDG_PACK_BYTES] = { 0 };
    p[0] = 9;
    p[1] = (uint8_t)instruction;
    memcpy(p + 4, data, 16);
    tput(b, p, sizeof(p));
}

/* CD+G: a black screen from the start, white from 1 s (pack 300) on. */
static tbuf make_cdg(void)
{
    tbuf c = { 0 };
    uint8_t d[16] = { 0 };
    int i;
    for (i = 0; i < 8; i++) {   /* colour i: black, then 4 white */
        int v = i == 4 ? 15 : 0;
        d[2 * i] = (uint8_t)((v << 2) | (v >> 2));
        d[2 * i + 1] = (uint8_t)(((v & 3) << 4) | v);
    }
    cdg_pack(&c, 30, d);
    memset(d, 0, sizeof(d));
    cdg_pack(&c, 1, d);          /* memory preset: colour 0 */
    while (c.n / FA_CDG_PACK_BYTES < 300) { static const uint8_t z[16]; cdg_pack(&c, 0, z); c.d[c.n - FA_CDG_PACK_BYTES] = 0; }
    d[0] = 4;
    cdg_pack(&c, 1, d);          /* memory preset: colour 4 */
    while (c.n / FA_CDG_PACK_BYTES < 900) { static const uint8_t z[16]; cdg_pack(&c, 0, z); c.d[c.n - FA_CDG_PACK_BYTES] = 0; }
    return c;
}

static tbuf g_sf2_song;

/* Deck A: a WAV with CD+G, deck B: the MIDI song; both playing. */
static int heard_decks(sim* s, const tbuf* cdg)
{
    size_t n;
    fa_deck* a;
    fa_deck* b;
    uint8_t* wav = tone_wav(s->dev->rate, 3 * s->dev->rate, 0, 1000.0, &n);
    int ok = fa_stream_open_memory(g_engine, wav, n, NULL, &s->music) == FA_OK &&
             fa_stream_open_memory(g_engine, g_sf2_song.d, g_sf2_song.n, NULL, &s->music2) == FA_OK;
    free(wav);
    fa_mixer_get_deck(s->m, 0, &a);
    fa_mixer_get_deck(s->m, 1, &b);
    return ok && fa_stream_load_cdg_memory(s->music, cdg->d, cdg->n) == FA_OK && fa_deck_load(a, s->music, 0) == FA_OK &&
           fa_deck_load(b, s->music2, 0) == FA_OK && fa_stream_play(s->music) == FA_OK && fa_stream_play(s->music2) == FA_OK;
}

typedef struct lyric_view {
    fa_surface* f;
    fa_surface_layout lay;
    uint8_t* px;
    uint64_t rev;
} lyric_view;

static int lyric_view_make(lyric_view* v, fa_stream* s)
{
    fa_lyric_style st;
    memset(v, 0, sizeof(*v));
    fa_lyric_style_init(&st, FA_LYRIC_CLASSIC);
    st.font_path = FA_TEST_DATA_DIR "/fonts/tt.ttf";
    v->lay.struct_size = (uint32_t)sizeof(v->lay);
    if (fa_lyric_surface_create(s, &st, &v->f) != FA_OK || fa_surface_get_layout(v->f, &v->lay) != FA_OK) return 0;
    v->px = (uint8_t*)calloc((size_t)v->lay.width * 4 * v->lay.height, 1);
    return v->px != NULL;
}

/* The lyric picture for the audio heard now: the position it shows. */
static fa_time lyric_view_now(lyric_view* v)
{
    fa_surface_frame fr;
    memset(&fr, 0, sizeof(fr));
    fr.struct_size = (uint32_t)sizeof(fr);
    if (fa_surface_update_into(v->f, FA_SURFACE_NOW, v->px, (int32_t)v->lay.width * 4, v->lay.width, v->lay.height,
                               v->rev, &fr) != FA_OK)
        return -1;
    v->rev = fr.revision;
    return fr.time;
}

/* MediaControl's second rule: a deck's heard position includes a delay on the master.
 * Two mixers play the same decks in step, one with its master delayed to line up slow
 * headphones, one without: the delayed one's clock, CD+G picture and lyric picture are
 * the other's, exactly that much later. */
static void test_cue_heard_position(void)
{
    sim a, b;
    tbuf cdg = make_cdg();
    fa_cdg_surface* ga = NULL;
    fa_cdg_surface* gb = NULL;
    lyric_view la, lb;
    fa_mixer_latency l;
    fa_time da = 0, db = 0;
    double delay_ms = 0.0, worst_clock = 0.0, worst_lyric = 0.0, worst_cdg = 0.0;
    int64_t turn_a = -1, turn_b = -1;
    uint32_t blocks = 0, compared = 0;
    memset(&la, 0, sizeof(la));
    memset(&lb, 0, sizeof(lb));
    CHECK(sim_open_cue_settled(&a, 0, 48000, 48000.0, 480, 120000000));
    CHECK(sim_open_cue_settled(&b, FA_MIXER_CUE_LOWEST_LATENCY, 48000, 48000.0, 480, 120000000));
    CHECK(heard_decks(&a, &cdg));
    CHECK(heard_decks(&b, &cdg));
    CHECK_EQ(fa_cdg_surface_create(a.music, NULL, &ga), FA_OK);
    CHECK_EQ(fa_cdg_surface_create(b.music, NULL, &gb), FA_OK);
    CHECK(lyric_view_make(&la, a.music2));
    CHECK(lyric_view_make(&lb, b.music2));
    l = latency_of(&a);
    CHECK_EQ(l.cue_align, FA_CUE_ALIGN_MASTER);
    delay_ms = (double)l.cue_alignment / 1e6;
    CHECK(delay_ms > 50.0);
    CHECK_EQ(fa_device_get_latency(a.dev, &da), FA_OK);
    CHECK_EQ(fa_device_get_latency(b.dev, &db), FA_OK);
    CHECK_NEAR((double)(da - db) / 1e6, delay_ms, 0.001);
    /* The presentation clock smooths the callback times it is given; both start afresh
     * here, so with the same times both carry the same callback time from now on. */
    a.dev->cb_prev_time = 0;
    b.dev->cb_prev_time = 0;
    for (blocks = 0; blocks < 2.5 * a.dev->rate / OUT_BLOCK; blocks++) {
        int64_t now, t[7];
        fa_time ca = 0, cb = 0, ya, yb;
        fa_cdg_frame fa_, fb_;
        sim_wait_decoded(&a);   /* no waiting (a 15 ms sleep on Windows) between `now` and the reads */
        sim_wait_decoded(&b);
        now = fa_monotonic_ns();
        sim_step(&a, now);
        sim_step(&b, now);
        /* Each pair is asked one after the other: the clocks move on with real time
         * meanwhile, by t[i + 1] - t[i], which is taken off. */
        t[0] = fa_monotonic_ns();
        CHECK_EQ(fa_stream_get_clock(a.music, &ca), FA_OK);
        t[1] = fa_monotonic_ns();
        CHECK_EQ(fa_stream_get_clock(b.music, &cb), FA_OK);
        t[2] = fa_monotonic_ns();
        CHECK_EQ(fa_cdg_surface_update(ga, &fa_), FA_OK);
        t[3] = fa_monotonic_ns();
        CHECK_EQ(fa_cdg_surface_update(gb, &fb_), FA_OK);
        t[4] = fa_monotonic_ns();
        ya = lyric_view_now(&la);
        t[5] = fa_monotonic_ns();
        yb = lyric_view_now(&lb);
        t[6] = fa_monotonic_ns();
        if (cb < 300000000 || ca <= 0) continue;   /* both heard and well under way */
        /* At the same moment the delayed mixer is exactly the delay behind. Each read
         * happened somewhere between the stamps around it, so the gap between the two
         * reads is known to within those stamps: only an error beyond that counts. A
         * clock holds at the end of what was rendered, so reads later than the undelayed
         * mixer's latency after `now` (a busy machine) are left out. */
        if (t[6] - now < db) {
            double dc = fabs((double)(cb - ca - (t[1] - t[0])) / 1e6 - delay_ms) - (double)(t[2] - t[0]) / 1e6;
            double dg = fabs((double)(fb_.time - fa_.time - (t[3] - t[2])) / 1e6 - delay_ms) - (double)(t[4] - t[2]) / 1e6;
            double dy = fabs((double)(yb - ya - (t[5] - t[4])) / 1e6 - delay_ms) - (double)(t[6] - t[4]) / 1e6;
            if (dc > worst_clock) worst_clock = dc;
            if (dg > worst_cdg) worst_cdg = dg;
            if (dy > worst_lyric) worst_lyric = dy;
            compared++;
        }
        /* The picture turns white at 1 s on both, so on the delayed one that much later:
         * it is still black when the other turns. */
        CHECK_EQ(fa_.background_index, fa_.time > 1000000000 ? 4 : 0);
        CHECK_EQ(fb_.background_index, fb_.time > 1000000000 ? 4 : 0);
        if (turn_b < 0 && fb_.background_index == 4) {
            turn_b = (int64_t)a.frames;
            CHECK_EQ(fa_.background_index, 0);
        }
        if (turn_a < 0 && fa_.background_index == 4) turn_a = (int64_t)a.frames;
    }
    /* (How many frames apart the two turns are depends on how fast this test renders
     * against real time; the clocks above compare them at the same moment.) */
    CHECK(compared > 100);
    CHECK(worst_clock <= 0.001);   /* a microsecond: rounding */
    CHECK(worst_cdg <= 0.001);
    CHECK(worst_lyric <= 0.001);
    CHECK(turn_b > 0 && turn_a > turn_b);
    fa_cdg_surface_destroy(ga);
    fa_cdg_surface_destroy(gb);
    fa_surface_destroy(la.f);
    fa_surface_destroy(lb.f);
    free(la.px);
    free(lb.px);
    sim_close(&a);
    sim_close(&b);
    free(cdg.d);
}

#if defined(__APPLE__) && TARGET_OS_IPHONE
static void test_cue_device_not_on_ios(void)
{
    fa_mixer_config c;
    fa_mixer* m = NULL;
    fa_mixer_config_init(&c);
    c.cue_mode = FA_CUE_DEVICE;
    CHECK_EQ(fa_mixer_create(g_engine, &c, &m), FA_ERROR_NOT_IMPLEMENTED);
    CHECK(m == NULL);
}
#endif

int main(void)
{
    fa_engine_config cfg;
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    cfg.worker_threads = 2;
    if (fa_engine_create(&cfg, &g_engine) != FA_OK) { fprintf(stderr, "engine create failed\n"); return 1; }
    {
        /* The heard-position test plays a MIDI song with lyrics. */
        fa_soundfont* font = NULL;
        tbuf sf = make_sf2();
        if (fa_soundfont_open_memory(sf.d, sf.n, &font) != FA_OK) { fprintf(stderr, "test soundfont rejected\n"); return 1; }
        free(sf.d);
        fa_engine_set_soundfont(g_engine, font);
        fa_soundfont_close(font);   /* the engine keeps its own reference */
        g_sf2_song = make_song();
    }

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
    RUN(test_check_mic_removed);
    RUN(test_latency_mode);
#if defined(__APPLE__) && TARGET_OS_IPHONE
    /* Headphones on a second device are not on iOS (FA_ERROR_NOT_IMPLEMENTED). */
    RUN(test_cue_device_not_on_ios);
#else
    RUN(test_cue_drift_fast);
    RUN(test_cue_drift_slow);
    RUN(test_cue_other_rate);
    RUN(test_cue_aligns_cue);
    RUN(test_cue_aligns_master);
    RUN(test_cue_trim_and_off);
    RUN(test_cue_never_delays_a_monitored_mic);
    RUN(test_cue_lost);
    RUN(test_cue_latency_mode);
    RUN(test_cue_heard_position);
#endif

    fa_engine_destroy(g_engine);
    free(g_sf2_song.d);
    if (g_failures) { fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    printf("all mic tests passed\n");
    return 0;
}
