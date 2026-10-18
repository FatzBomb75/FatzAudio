/* Tempo, pitch and rate: lengths, pitch accuracy, sound quality measured against
 * reference signals, and how positions, markers, ramps and the clock follow.
 *
 * Quality is measured, not listened to: a stretched or shifted test tone (or
 * chord) is compared, in short windows, with the ideal result, a clean sine at
 * the expected frequency. Whatever is not that sine (phasing, warble, splice
 * noise, aliasing) counts as error; the score is the signal-to-error ratio in dB.
 * The same measurement compares FatzAudio with other stretchers (see
 * tools/tempo-compare).
 *
 * White-box like test_internal.c: the device is the null backend, never started,
 * and the test plays the audio thread for attached streams. */
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

#define PI 3.14159265358979323846
#define RATE 48000

static fa_engine* g_engine;
static fa_device* g_dev;

/* ---- media ------------------------------------------------------------------ */

/* A 32-bit float stereo WAV in memory holding `frames` frames of `gen`. */
typedef float (*gen_fn)(double t, int channel);

static uint8_t* make_wav(uint32_t rate, uint32_t frames, gen_fn gen, size_t* size)
{
    size_t data = (size_t)frames * 8, n = 44 + data;
    uint8_t* b = (uint8_t*)malloc(n);
    uint32_t i, v;
    memcpy(b, "RIFF", 4); v = (uint32_t)(n - 8); memcpy(b + 4, &v, 4); memcpy(b + 8, "WAVEfmt ", 8);
    v = 16; memcpy(b + 16, &v, 4);
    b[20] = 3; b[21] = 0; b[22] = 2; b[23] = 0;               /* IEEE float, stereo */
    memcpy(b + 24, &rate, 4); v = rate * 8; memcpy(b + 28, &v, 4);
    b[32] = 8; b[33] = 0; b[34] = 32; b[35] = 0;
    memcpy(b + 36, "data", 4); v = (uint32_t)data; memcpy(b + 40, &v, 4);
    for (i = 0; i < frames; i++) {
        float l = gen((double)i / rate, 0), r = gen((double)i / rate, 1);
        memcpy(b + 44 + (size_t)i * 8, &l, 4);
        memcpy(b + 48 + (size_t)i * 8, &r, 4);
    }
    *size = n;
    return b;
}

static float sine440(double t, int c) { (void)c; return (float)(0.5 * sin(2 * PI * 440.0 * t)); }
static float sine1k(double t, int c) { (void)c; return (float)(0.5 * sin(2 * PI * 1000.0 * t)); }
static const double g_chord[3] = { 261.63, 329.63, 392.00 };   /* C major */
static float chord(double t, int c)
{
    (void)c;
    return (float)(0.25 * (sin(2 * PI * g_chord[0] * t) + sin(2 * PI * g_chord[1] * t) + sin(2 * PI * g_chord[2] * t)));
}
static float ramp_sig(double t, int c) { (void)c; return (float)(t * 0.01); }  /* value = time: positions */

static fa_stream* open_wav(uint32_t frames, gen_fn gen, uint32_t flags)
{
    size_t n;
    uint8_t* wav = make_wav(RATE, frames, gen, &n);
    fa_open_options o;
    fa_stream* s = NULL;
    fa_open_options_init(&o);
    o.flags = flags;
    if (fa_stream_open_memory(g_engine, wav, n, &o, &s) != FA_OK) s = NULL;
    free(wav);
    return s;
}

/* Reads a decode-only stream to its end; returns the frames (stereo, malloc'd). */
static float* read_all(fa_stream* s, int64_t* out_frames)
{
    int64_t cap = 1 << 16, n = 0;
    float* buf = (float*)malloc((size_t)cap * 2 * sizeof(float));
    for (;;) {
        fa_frames got = 0;
        fa_result r;
        if (n + 4096 > cap) { cap *= 2; buf = (float*)realloc(buf, (size_t)cap * 2 * sizeof(float)); }
        r = fa_stream_read(s, buf + n * 2, 4096, &got);
        n += got;
        if (r == FA_ERROR_END_OF_STREAM || (r == FA_OK && got == 0)) break;
        if (r != FA_OK) { fprintf(stderr, "  read: %s\n", fa_result_string(r)); break; }
    }
    *out_frames = n;
    return buf;
}

/* ---- measurements ----------------------------------------------------------- */

/* Least-squares fit of sines at `freqs` (plus DC) to x[0..n) (channel 0 of stereo);
 * returns the signal-to-residual ratio in dB. Solved by Gauss-Jordan on the normal
 * equations (2 * nf + 1 unknowns). */
static double fit_snr(const float* x, int64_t n, const double* freqs, int nf, double rate)
{
    double a[7][8];
    int k = 2 * nf + 1, i, j, r;
    int64_t t;
    double sig = 0.0, res = 0.0;
    memset(a, 0, sizeof(a));
    for (t = 0; t < n; t++) {
        double basis[7], y = x[t * 2];
        basis[0] = 1.0;
        for (i = 0; i < nf; i++) {
            basis[1 + 2 * i] = sin(2 * PI * freqs[i] * (double)t / rate);
            basis[2 + 2 * i] = cos(2 * PI * freqs[i] * (double)t / rate);
        }
        for (i = 0; i < k; i++) {
            for (j = 0; j < k; j++) a[i][j] += basis[i] * basis[j];
            a[i][k] += basis[i] * y;
        }
    }
    for (i = 0; i < k; i++) {
        double p = a[i][i];
        for (j = i; j <= k; j++) a[i][j] /= p;
        for (r = 0; r < k; r++) {
            double f = a[r][i];
            if (r == i || f == 0.0) continue;
            for (j = i; j <= k; j++) a[r][j] -= f * a[i][j];
        }
    }
    for (t = 0; t < n; t++) {
        double fit = a[0][k], y = x[t * 2];
        for (i = 0; i < nf; i++)
            fit += a[1 + 2 * i][k] * sin(2 * PI * freqs[i] * (double)t / rate) + a[2 + 2 * i][k] * cos(2 * PI * freqs[i] * (double)t / rate);
        sig += fit * fit;
        res += (y - fit) * (y - fit);
    }
    return 10.0 * log10(sig / (res + 1e-30));
}

/* The mean of fit_snr over 4096-frame windows of the steady part (the first and
 * last 0.3 s skipped). Short windows forgive a slow phase drift, not noise. */
static double tone_score(const float* x, int64_t n, const double* freqs, int nf)
{
    int64_t w = 4096, skip = (int64_t)(0.3 * RATE), at;
    double sum = 0.0;
    int count = 0;
    for (at = skip; at + w <= n - skip; at += w) {
        sum += fit_snr(x + at * 2, w, freqs, nf, RATE);
        count++;
    }
    return count ? sum / count : -999.0;
}

/* Frequency of the strongest component near `guess` (Hz), from the phase advance
 * of a single-bin DFT between two windows a second apart. */
static double measure_freq(const float* x, int64_t n, double guess)
{
    int64_t w = 8192, t0 = n / 2 - RATE / 2 - w / 2, t1 = t0 + RATE, t;
    double re0 = 0, im0 = 0, re1 = 0, im1 = 0, dphi;
    for (t = 0; t < w; t++) {
        double win = 0.5 - 0.5 * cos(2 * PI * (double)t / (double)w), ph = 2 * PI * guess * (double)t / RATE;
        re0 += x[(t0 + t) * 2] * win * cos(ph); im0 -= x[(t0 + t) * 2] * win * sin(ph);
        re1 += x[(t1 + t) * 2] * win * cos(ph); im1 -= x[(t1 + t) * 2] * win * sin(ph);
    }
    /* One second apart, a sine at f advances 2 pi f; at `guess` that would be
     * 2 pi guess. The difference, wrapped, is the offset (within +-0.5 Hz). */
    dphi = atan2(im1, re1) - atan2(im0, re0) - 2 * PI * fmod(guess, 1.0);
    while (dphi > PI) dphi -= 2 * PI;
    while (dphi < -PI) dphi += 2 * PI;
    return guess + dphi / (2 * PI);
}

/* ---- decode-only: lengths, pitch, quality ------------------------------------- */

static void render_decoded(gen_fn gen, double seconds, fa_param param, float value, float** out, int64_t* frames)
{
    fa_stream* s = open_wav((uint32_t)(seconds * RATE), gen, FA_OPEN_DECODE_ONLY);
    *out = NULL;
    *frames = 0;
    if (!s) return;
    if (fa_stream_set_param(s, param, value) != FA_OK) { fa_stream_close(s); return; }
    *out = read_all(s, frames);
    fa_stream_close(s);
}

static void test_neutral_leaves_audio_untouched(void)
{
    float *a, *b;
    int64_t na, nb, i;
    fa_stream* s = open_wav(RATE, sine440, FA_OPEN_DECODE_ONLY);
    CHECK(s != NULL);
    a = read_all(s, &na);
    fa_stream_close(s);
    render_decoded(sine440, 1.0, FA_PARAM_TEMPO, 1.0f, &b, &nb);   /* set, but to neutral */
    CHECK(b != NULL);
    CHECK_EQ(na, nb);
    for (i = 0; i < na * 2; i++) if (a[i] != b[i]) break;
    CHECK_EQ(i, na * 2);                                            /* bit-exact: never processed */
    free(a);
    free(b);
}

static void test_tempo_length_and_tone(void)
{
    static const float tempos[] = { 0.5f, 0.8f, 1.25f, 2.0f };
    double f = 440.0;
    size_t k;
    for (k = 0; k < sizeof(tempos) / sizeof(tempos[0]); k++) {
        float* y;
        int64_t n;
        double score, measured;
        render_decoded(sine440, 4.0, FA_PARAM_TEMPO, tempos[k], &y, &n);
        CHECK(y != NULL);
        CHECK_NEAR((double)n, 4.0 * RATE / tempos[k], 2.0);       /* exact length, end included */
        score = tone_score(y, n, &f, 1);
        measured = measure_freq(y, n, 440.0);
        printf("  tempo %.2f: %lld frames, tone %.1f dB, %.3f Hz\n", tempos[k], (long long)n, score, measured);
        CHECK_NEAR(measured, 440.0, 0.05);                          /* pitch kept */
        CHECK(score > 45.0);
        free(y);
    }
}

static void test_pitch_accuracy_and_tone(void)
{
    static const float semis[] = { -12.0f, -5.0f, 3.0f, 7.0f, 12.0f };
    size_t k;
    for (k = 0; k < sizeof(semis) / sizeof(semis[0]); k++) {
        float* y;
        int64_t n;
        double f = 440.0 * pow(2.0, semis[k] / 12.0), score, measured;
        render_decoded(sine440, 4.0, FA_PARAM_PITCH, semis[k], &y, &n);
        CHECK(y != NULL);
        CHECK_NEAR((double)n, 4.0 * RATE, 2.0);                     /* length kept */
        score = tone_score(y, n, &f, 1);
        measured = measure_freq(y, n, f);
        printf("  pitch %+.0f: tone %.1f dB, %.3f Hz (want %.3f)\n", semis[k], score, measured, f);
        CHECK_NEAR(measured, f, 0.05);
        CHECK(score > 45.0);
        free(y);
    }
}

static void test_chord_quality(void)
{
    float* y;
    int64_t n;
    double score, shifted[3];
    int i;
    render_decoded(chord, 4.0, FA_PARAM_TEMPO, 1.15f, &y, &n);
    CHECK(y != NULL);
    score = tone_score(y, n, g_chord, 3);
    printf("  chord at tempo 1.15: %.1f dB\n", score);
    CHECK(score > 40.0);
    free(y);
    render_decoded(chord, 4.0, FA_PARAM_PITCH, -3.0f, &y, &n);
    CHECK(y != NULL);
    for (i = 0; i < 3; i++) shifted[i] = g_chord[i] * pow(2.0, -3.0 / 12.0);
    score = tone_score(y, n, shifted, 3);
    printf("  chord at pitch -3: %.1f dB\n", score);
    CHECK(score > 35.0);
    free(y);
}

static void test_rate_is_clean_varispeed(void)
{
    float* y;
    int64_t n;
    double f = 1500.0, score;
    render_decoded(sine1k, 4.0, FA_PARAM_RATE, 1.5f, &y, &n);
    CHECK(y != NULL);
    CHECK_NEAR((double)n, 4.0 * RATE / 1.5, 2.0);
    score = tone_score(y, n, &f, 1);
    printf("  rate 1.5: tone %.1f dB, %.3f Hz\n", score, measure_freq(y, n, 1500.0));
    CHECK_NEAR(measure_freq(y, n, 1500.0), 1500.0, 0.01);
    CHECK(score > 85.0);                                            /* windowed sinc: no stretcher artifacts */
    free(y);
    f = 700.0;
    render_decoded(sine1k, 4.0, FA_PARAM_RATE, 0.7f, &y, &n);
    CHECK(y != NULL);
    CHECK_NEAR((double)n, 4.0 * RATE / 0.7, 2.0);
    score = tone_score(y, n, &f, 1);
    printf("  rate 0.7: tone %.1f dB\n", score);
    CHECK(score > 85.0);
    free(y);
}

static void test_combined_parameters(void)
{
    fa_stream* s = open_wav(4 * RATE, sine440, FA_OPEN_DECODE_ONLY);
    float* y;
    int64_t n;
    double f = 440.0 * pow(2.0, 2.0 / 12.0) * 1.1, measured;
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_TEMPO, 1.2f), FA_OK);
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_PITCH, 2.0f), FA_OK);
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_RATE, 1.1f), FA_OK);
    y = read_all(s, &n);
    fa_stream_close(s);
    CHECK_NEAR((double)n, 4.0 * RATE / (1.2 * 1.1), 3.0);          /* speed = tempo x rate */
    measured = measure_freq(y, n, f);
    printf("  tempo 1.2 + pitch +2 + rate 1.1: %.3f Hz (want %.3f), tone %.1f dB\n", measured, f, tone_score(y, n, &f, 1));
    CHECK_NEAR(measured, f, 0.05);                                  /* pitch = semitones + rate */
    free(y);
}

/* Decode-only positions follow the source: half way through the output of a
 * tempo-2 stream is half way through the file. */
static void test_decode_position_and_seek(void)
{
    fa_stream* s = open_wav(4 * RATE, ramp_sig, FA_OPEN_DECODE_ONLY);
    float* buf = (float*)malloc((size_t)RATE * 2 * sizeof(float));
    fa_frames got = 0, pos = 0, total = 0;
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_TEMPO, 2.0f), FA_OK);
    while (total < RATE / 2) {
        CHECK_EQ(fa_stream_read(s, buf, RATE / 2 - total, &got), FA_OK);
        total += got;
    }
    CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
    CHECK_NEAR((double)pos, (double)RATE, 4.0);
    CHECK_EQ(fa_stream_seek(s, 3 * RATE), FA_OK);
    CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
    CHECK_EQ(pos, 3 * RATE);
    total = 0;
    while (total < RATE / 4) {
        CHECK_EQ(fa_stream_read(s, buf, RATE / 4 - total, &got), FA_OK);
        total += got;
    }
    CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
    CHECK_NEAR((double)pos, 3.5 * RATE, 4.0);
    fa_stream_close(s);
    /* The audio read is the source at that position (value = time * 0.01). The
     * varispeed alone passes such a slow ramp through exactly; a stretcher works on
     * spectra and does not keep near-DC levels. */
    s = open_wav(4 * RATE, ramp_sig, FA_OPEN_DECODE_ONLY);
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_RATE, 2.0f), FA_OK);
    CHECK_EQ(fa_stream_seek(s, 2 * RATE), FA_OK);
    total = 0;
    while (total < RATE / 4) {
        CHECK_EQ(fa_stream_read(s, buf, RATE / 4 - total, &got), FA_OK);
        total += got;
    }
    CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
    CHECK_NEAR((double)pos, 2.5 * RATE, 2.0);
    CHECK_NEAR(buf[(got - 1) * 2], (double)pos / RATE * 0.01, 0.00002);
    fa_stream_close(s);
    free(buf);
}

/* ---- attached streams: the white-box mixer ---------------------------------- */

static void render(fa_stream* s, float* out, uint32_t frames)
{
    int i;
    for (i = 0; i < 4000; i++) {
        if (fa_ring_readable(&s->ring) >= (int64_t)frames || fa_load32(&s->eof)) break;
        fa_sleep_ms(1);
    }
    memset(out, 0, (size_t)frames * 2 * sizeof(float));
    fa_fetch_add64(&g_dev->epoch, 1);
    fa_stream_mix(s, g_dev, out, frames);
    fa_fetch_add64(&g_dev->epoch, 1);
    fa_store64(&g_dev->clock, fa_load64(&g_dev->clock) + frames);
}

typedef struct recorder {
    fa_mutex lock;
    int ended, ramp_done, markers;
    int64_t last_tag;
    fa_frames ended_at;
} recorder;

static void FA_CALL on_event(const fa_event* ev, void* user)
{
    recorder* r = (recorder*)user;
    fa_mutex_lock(&r->lock);
    if (ev->type == FA_EVENT_ENDED) { r->ended++; r->ended_at = ev->frame; }
    if (ev->type == FA_EVENT_RAMP_DONE) { r->ramp_done++; r->last_tag = ev->value; }
    if (ev->type == FA_EVENT_MARKER) r->markers++;
    fa_mutex_unlock(&r->lock);
}

static int wait_for(recorder* r, int* field, int want)
{
    int i, v = 0;
    for (i = 0; i < 2000; i++) {
        fa_mutex_lock(&r->lock);
        v = *field;
        fa_mutex_unlock(&r->lock);
        if (v >= want) return 1;
        fa_sleep_ms(1);
    }
    return 0;
}

static fa_stream* open_attached(uint32_t frames, gen_fn gen, uint32_t flags)
{
    fa_stream* s = open_wav(frames, gen, flags);
    if (s && fa_stream_attach(s, g_dev, NULL) != FA_OK) { fa_stream_close(s); s = NULL; }
    return s;
}

static void test_attached_position_follows_tempo(void)
{
    fa_stream* s = open_wav(10 * RATE, ramp_sig, 0);
    float* out = (float*)malloc(1024 * 2 * sizeof(float));
    fa_frames pos = 0;
    int i;
    CHECK(s != NULL);
    /* Set before attaching: once playing, a change is heard after what is
     * already decoded ahead. */
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_TEMPO, 1.6f), FA_OK);
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_RATE, 1.25f), FA_OK);    /* 2x in all */
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    for (i = 0; i < 47; i++) render(s, out, 1024);                  /* ~1 s of output */
    CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
    CHECK_NEAR((double)pos, 47.0 * 1024 * 2, 8.0);
    fa_stream_close(s);
    /* What is heard is the source at that position (value = time * 0.01; the
     * varispeed alone keeps such a slow ramp exact). */
    s = open_attached(10 * RATE, ramp_sig, 0);
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_RATE, 2.0f), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    for (i = 0; i < 47; i++) render(s, out, 1024);
    CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
    CHECK_NEAR((double)pos, 47.0 * 1024 * 2, 8.0);
    CHECK_NEAR(out[1023 * 2], (double)pos / RATE * 0.01, 0.00005);
    CHECK_EQ(fa_stream_seek(s, 5 * RATE), FA_OK);
    for (i = 0; i < 10; i++) render(s, out, 1024);
    CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
    CHECK_NEAR((double)pos, 5.0 * RATE + 10.0 * 1024 * 2, 8.0);
    fa_stream_close(s);
    free(out);
}

/* The presentation clock (what video and CD+G graphics follow) runs at the tempo:
 * at 2x, 10 ms heard is 20 ms of the file. Rendered through the device with a
 * made-up timeline, as in test_handoff.c. */
static void test_clock_runs_at_tempo(void)
{
    fa_stream* s = open_wav(10 * RATE, sine440, 0);
    float out[512 * 2];
    int64_t now = 1000000000, lat = 0, t, c0, c1;
    fa_time l = 0;
    fa_frames pos = 0;
    int i;
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_TEMPO, 2.0f), FA_OK);
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    for (i = 0; i < 94; i++) {                                       /* ~1 s */
        int k;
        for (k = 0; k < 2000 && fa_ring_readable(&s->ring) < 512; k++) fa_sleep_ms(1);
        fa_fetch_add64(&g_dev->epoch, 1);
        fa_device_render_at(g_dev, out, 512, now);
        fa_fetch_add64(&g_dev->epoch, 1);
        now += (int64_t)(512 * 1e9 / g_dev->rate + 0.5);
    }
    fa_device_get_latency(g_dev, &l);
    lat = l;
    t = now - 20000000;
    fa_mutex_lock(&s->api_lock);
    c0 = fa_stream_clock_at(s, t);
    c1 = fa_stream_clock_at(s, t + 10000000);
    fa_mutex_unlock(&s->api_lock);
    CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
    printf("  10 ms heard = %.3f ms of the file; clock %.1f ms, position %.1f ms, latency %.1f ms\n",
           (c1 - c0) / 1e6, c1 / 1e6, pos * 1e3 / RATE, lat / 1e6);
    CHECK_NEAR((double)(c1 - c0), 20000000.0, 100000.0);
    /* The clock is the position minus what is still between the mixer and the speaker. */
    CHECK_NEAR((double)c1, (double)pos * 1e9 / RATE - 2.0 * (double)(lat + 10000000), 2000000.0);
    fa_stream_close(s);
}

static void test_cue_out_ends_exactly(void)
{
    fa_stream* s = open_attached(4 * RATE, ramp_sig, 0);
    float* out = (float*)malloc(1024 * 2 * sizeof(float));
    recorder rec;
    fa_frames pos = 0;
    int i;
    CHECK(s != NULL);
    memset(&rec, 0, sizeof(rec));
    fa_mutex_init(&rec.lock);
    fa_stream_set_event_callback(s, on_event, &rec);
    CHECK_EQ(fa_stream_set_marker(s, FA_MARKER_CUE_OUT, 2 * RATE), FA_OK);
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_TEMPO, 1.5f), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    for (i = 0; i < 200 && fa_load32(&s->state) == FA_STATE_PLAYING; i++) render(s, out, 1024);
    CHECK(wait_for(&rec, &rec.ended, 1));
    CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
    CHECK_NEAR((double)pos, 2.0 * RATE, 2.0);
    /* 2 s of source at 1.5x is 1.333 s heard. */
    CHECK_NEAR((double)fa_load64(&s->played), 2.0 * RATE / 1.5, 64.0);
    fa_stream_set_event_callback(s, NULL, NULL);
    fa_stream_close(s);
    fa_mutex_destroy(&rec.lock);
    free(out);
}

static void test_ramp_reaches_target_with_event(void)
{
    fa_stream* s = open_attached(20 * RATE, sine440, 0);
    float* out = (float*)malloc(1024 * 2 * sizeof(float));
    recorder rec;
    fa_ramp ramp;
    float v = 0.0f;
    int i;
    CHECK(s != NULL);
    memset(&rec, 0, sizeof(rec));
    fa_mutex_init(&rec.lock);
    fa_stream_set_event_callback(s, on_event, &rec);
    memset(&ramp, 0, sizeof(ramp));
    ramp.curve = FA_CURVE_SCURVE;
    ramp.tag = 77;
    CHECK_EQ(fa_stream_play(s), FA_OK);
    CHECK_EQ(fa_stream_ramp_param(s, FA_PARAM_TEMPO, 1.5f, 500000000, 0, &ramp), FA_OK);
    for (i = 0; i < 4; i++) render(s, out, 1024);
    CHECK_EQ(fa_stream_get_param(s, FA_PARAM_TEMPO, &v), FA_OK);
    CHECK(v > 1.0f && v < 1.5f);                                    /* mid-ramp */
    for (i = 0; i < 60; i++) render(s, out, 1024);
    CHECK(wait_for(&rec, &rec.ramp_done, 1));
    CHECK_EQ(rec.last_tag, 77);
    CHECK_EQ(fa_stream_get_param(s, FA_PARAM_TEMPO, &v), FA_OK);
    CHECK_NEAR(v, 1.5, 1e-6);
    /* A ramp replaced before it ends still raises its RAMP_DONE, once. */
    ramp.tag = 78;
    CHECK_EQ(fa_stream_ramp_param(s, FA_PARAM_PITCH, 5.0f, 2000000000, 0, &ramp), FA_OK);
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_PITCH, 0.0f), FA_OK);
    CHECK(wait_for(&rec, &rec.ramp_done, 2));
    CHECK_EQ(rec.last_tag, 78);
    fa_stream_set_event_callback(s, NULL, NULL);
    fa_stream_close(s);
    fa_mutex_destroy(&rec.lock);
    free(out);
}

/* A ramp scheduled on the device clock as the first tempo change of a stream that
 * has played a while starts on time (the first change re-renders from what is
 * heard, which restarts the stream's frame count). */
static void test_scheduled_ramp_as_first_change(void)
{
    fa_stream* s = open_attached(20 * RATE, sine440, 0);
    float* out = (float*)malloc(1024 * 2 * sizeof(float));
    recorder rec;
    fa_ramp ramp;
    fa_time now = 0;
    int i;
    CHECK(s != NULL);
    memset(&rec, 0, sizeof(rec));
    fa_mutex_init(&rec.lock);
    fa_stream_set_event_callback(s, on_event, &rec);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    for (i = 0; i < 94; i++) render(s, out, 1024);                   /* 2 s */
    memset(&ramp, 0, sizeof(ramp));
    ramp.tag = 90;
    CHECK_EQ(fa_device_get_time(g_dev, &now), FA_OK);
    CHECK_EQ(fa_stream_ramp_param(s, FA_PARAM_TEMPO, 2.0f, 100000000, now + 300000000, &ramp), FA_OK);
    for (i = 0; i < 3; i++) render(s, out, 1024);
    fa_sleep_ms(50);
    fa_mutex_lock(&rec.lock);
    CHECK_EQ(rec.ramp_done, 0);                                     /* not before its time */
    fa_mutex_unlock(&rec.lock);
    for (i = 0; i < 30; i++) render(s, out, 1024);                   /* 0.64 s more */
    CHECK(wait_for(&rec, &rec.ramp_done, 1));
    CHECK_EQ(rec.last_tag, 90);
    fa_stream_set_event_callback(s, NULL, NULL);
    fa_stream_close(s);
    fa_mutex_destroy(&rec.lock);
    free(out);
}

/* Engaging the stretcher in the middle of playback must not click: the largest
 * step between samples stays that of the sine itself. */
static void test_engaging_mid_play_is_smooth(void)
{
    fa_stream* s = open_attached(10 * RATE, sine440, FA_OPEN_NO_NORMALIZE);
    float* out = (float*)malloc(1024 * 2 * sizeof(float));
    double max_step = 2 * PI * 440.0 / RATE * 0.5, worst = 0.0;
    float prev = 0.0f;
    int i, j, have = 0;
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    for (i = 0; i < 60; i++) {
        if (i == 20) CHECK_EQ(fa_stream_set_param(s, FA_PARAM_PITCH, 0.3f), FA_OK);
        if (i == 40) CHECK_EQ(fa_stream_set_param(s, FA_PARAM_RATE, 1.02f), FA_OK);
        render(s, out, 1024);
        for (j = 0; j < 1024; j++) {
            if (have) { double d = fabs(out[j * 2] - prev); if (d > worst) worst = d; }
            prev = out[j * 2];
            have = 1;
        }
    }
    printf("  largest sample step %.5f (sine alone %.5f)\n", worst, max_step);
    CHECK(worst < max_step * 1.6);
    fa_stream_close(s);
    free(out);
}

static void test_loop_with_tempo_wraps(void)
{
    fa_stream* s = open_attached(RATE, ramp_sig, FA_OPEN_LOOP);
    float* out = (float*)malloc(1024 * 2 * sizeof(float));
    fa_frames pos = 0;
    int i;
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_TEMPO, 1.5f), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    for (i = 0; i < 50; i++) render(s, out, 1024);                  /* 75 k source frames */
    CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
    CHECK_NEAR((double)pos, fmod(50.0 * 1024 * 1.5, RATE), 8.0);
    CHECK_EQ(fa_load32(&s->state), FA_STATE_PLAYING);
    fa_stream_close(s);
    free(out);
}

/* A push stream of `seconds` of 440 Hz stereo float, timestamps from `pts0`. */
static fa_stream* open_pushed_sine(double seconds, int64_t pts0)
{
    fa_audio_format f;
    fa_stream* s = NULL;
    int64_t n = (int64_t)(seconds * RATE), i, chunk = 4800;
    float* buf = (float*)malloc((size_t)chunk * 2 * sizeof(float));
    memset(&f, 0, sizeof(f));
    f.sample_format = FA_FORMAT_F32;
    f.sample_rate = RATE;
    f.channels = 2;
    if (!buf || fa_stream_open_push(g_engine, &f, NULL, &s) != FA_OK) { free(buf); return NULL; }
    for (i = 0; i < n; i += chunk) {
        const void* planes[1];
        int64_t k;
        for (k = 0; k < chunk; k++) buf[k * 2] = buf[k * 2 + 1] = (float)(0.5 * sin(2.0 * PI * 440.0 * (double)(i + k) / RATE));
        planes[0] = buf;
        fa_stream_push_pcm(s, planes, chunk, pts0 + (int64_t)((double)i * 1e9 / RATE + 0.5));
    }
    free(buf);
    return s;
}

/* Renders about `ms` of the device, as the audio thread would, from device time `*now`. */
static void render_for(fa_stream* s, int ms, int64_t* now)
{
    float out[512 * 2];
    int i, blocks = (int)((double)ms * g_dev->rate / 1000.0 / 512.0 + 0.5);
    for (i = 0; i < blocks; i++) {
        int k;
        for (k = 0; k < 2000 && fa_ring_readable(&s->ring) < 512; k++) fa_sleep_ms(1);
        fa_fetch_add64(&g_dev->epoch, 1);
        fa_device_render_at(g_dev, out, 512, *now);
        fa_fetch_add64(&g_dev->epoch, 1);
        *now += (int64_t)(512 * 1e9 / g_dev->rate + 0.5);
    }
}

/* Push streams (an app's FFmpeg audio) take TEMPO and RATE too: the clock still
 * reports the app's timestamps and runs at the speed heard. */
static void test_push_stream_tempo_clock(void)
{
    static const float speeds[2] = { 2.0f, 0.75f };
    int k;
    for (k = 0; k < 2; k++) {
        fa_stream* s = open_pushed_sine(4.0, 5000000000LL);
        int64_t now = 1000000000, t, c0, c1;
        fa_param param = k == 0 ? FA_PARAM_TEMPO : FA_PARAM_RATE;
        CHECK(s != NULL);
        CHECK_EQ(fa_stream_set_param(s, param, speeds[k]), FA_OK);
        CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
        CHECK_EQ(fa_stream_play(s), FA_OK);
        render_for(s, 1000, &now);
        t = now - 20000000;
        fa_mutex_lock(&s->api_lock);
        c0 = fa_stream_clock_at(s, t);
        c1 = fa_stream_clock_at(s, t + 10000000);
        fa_mutex_unlock(&s->api_lock);
        printf("  %s %.2f: 10 ms heard = %.3f ms of the app's timeline; clock %.1f ms\n",
               k == 0 ? "tempo" : "rate", speeds[k], (c1 - c0) / 1e6, c1 / 1e6);
        CHECK_NEAR((double)(c1 - c0), 10000000.0 * speeds[k], 100000.0);
        /* About 1 s of device time played (less the latency): 5 s + that x speed. */
        CHECK(c1 > 5000000000LL + (int64_t)(0.7e9 * speeds[k]) && c1 < 5000000000LL + (int64_t)(1.05e9 * speeds[k]));
        fa_stream_close(s);
    }
}

/* A jump in the app's timestamps is kept while a tempo is applied: the clock
 * moves to the new timeline when its audio is heard, then runs at tempo again. */
static void test_push_stream_tempo_timeline_jump(void)
{
    fa_audio_format f;
    fa_stream* s = NULL;
    float buf[4800 * 2];
    const void* planes[1] = { buf };
    int64_t now = 1000000000, c0, c1;
    int i;
    memset(buf, 0, sizeof(buf));
    for (i = 0; i < 4800; i++) buf[i * 2] = buf[i * 2 + 1] = (float)(0.3 * sin(2.0 * PI * 440.0 * i / RATE));
    memset(&f, 0, sizeof(f));
    f.sample_format = FA_FORMAT_F32;
    f.sample_rate = RATE;
    f.channels = 2;
    CHECK_EQ(fa_stream_open_push(g_engine, &f, NULL, &s), FA_OK);
    for (i = 0; i < 5; i++) fa_stream_push_pcm(s, planes, 4800, (int64_t)i * 4800 * 1000000000LL / RATE);
    for (i = 0; i < 40; i++) fa_stream_push_pcm(s, planes, 4800, 60000000000LL + (int64_t)i * 4800 * 1000000000LL / RATE);
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_TEMPO, 1.5f), FA_OK);
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    render_for(s, 1500, &now);   /* 0.5 s of the first part plays in 0.33 s */
    fa_mutex_lock(&s->api_lock);
    c0 = fa_stream_clock_at(s, now - 30000000);
    c1 = fa_stream_clock_at(s, now - 20000000);
    fa_mutex_unlock(&s->api_lock);
    printf("  after the jump: clock %.1f ms, 10 ms heard = %.3f ms\n", c1 / 1e6, (c1 - c0) / 1e6);
    CHECK(c0 > 60000000000LL && c1 < 62000000000LL);
    CHECK_NEAR((double)(c1 - c0), 15000000.0, 150000.0);
    fa_stream_close(s);
}

static void test_params_and_options_api(void)
{
    fa_stream* s = open_wav(RATE, sine440, FA_OPEN_DECODE_ONLY);
    fa_bus* bus = NULL;
    fa_tempo_options o;
    float v = 0.0f;
    CHECK(s != NULL);
    CHECK_EQ(fa_stream_get_param(s, FA_PARAM_TEMPO, &v), FA_OK); CHECK_NEAR(v, 1.0, 0);
    CHECK_EQ(fa_stream_get_param(s, FA_PARAM_PITCH, &v), FA_OK); CHECK_NEAR(v, 0.0, 0);
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_TEMPO, 50.0f), FA_OK);    /* clamped */
    CHECK_EQ(fa_stream_get_param(s, FA_PARAM_TEMPO, &v), FA_OK); CHECK_NEAR(v, 10.0, 0);
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_RATE, -1.0f), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_PITCH, -100.0f), FA_OK);
    CHECK_EQ(fa_stream_get_param(s, FA_PARAM_PITCH, &v), FA_OK); CHECK_NEAR(v, -36.0, 0);
    CHECK_EQ(fa_stream_ramp_param(s, FA_PARAM_PITCH, 1.0f, 1000000, 5, NULL), FA_ERROR_INVALID_ARG); /* scheduled, not attached */
    CHECK_EQ(fa_stream_get_tempo_options(s, &o), FA_OK);
    CHECK_EQ(o.quality, FA_STRETCH_BEST);
    CHECK_EQ(o.keep_formants, 0);
    CHECK_EQ(o.latency_ms, 150);
    o.quality = FA_STRETCH_FAST;
    o.keep_formants = 1;
    o.latency_ms = 10;                                               /* raised to 50 */
    CHECK_EQ(fa_stream_set_tempo_options(s, &o), FA_OK);
    CHECK_EQ(fa_stream_get_tempo_options(s, &o), FA_OK);
    CHECK_EQ(o.quality, FA_STRETCH_FAST);
    CHECK_EQ(o.keep_formants, 1);
    CHECK_EQ(o.latency_ms, 50);
    o.quality = (fa_stretch_quality)7;
    CHECK_EQ(fa_stream_set_tempo_options(s, &o), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_stream_set_tempo_options(s, NULL), FA_OK);           /* defaults */
    CHECK_EQ(fa_stream_get_tempo_options(s, &o), FA_OK);
    CHECK_EQ(o.quality, FA_STRETCH_BEST);
    fa_stream_close(s);
    CHECK_EQ(fa_bus_create(g_dev, &bus), FA_OK);
    CHECK_EQ(fa_bus_set_param(bus, FA_PARAM_TEMPO, 1.1f), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_bus_set_param(bus, FA_PARAM_VOLUME, 0.5f), FA_OK);
    fa_bus_destroy(bus);
}

/* The fast preset and formant keeping still produce the right pitch. */
static void test_fast_and_formants(void)
{
    fa_stream* s = open_wav(4 * RATE, sine440, FA_OPEN_DECODE_ONLY);
    fa_tempo_options o;
    float* y;
    int64_t n;
    double f = 440.0 * pow(2.0, 4.0 / 12.0);
    CHECK(s != NULL);
    fa_tempo_options_init(&o);
    o.quality = FA_STRETCH_FAST;
    o.keep_formants = 1;
    CHECK_EQ(fa_stream_set_tempo_options(s, &o), FA_OK);
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_PITCH, 4.0f), FA_OK);
    y = read_all(s, &n);
    fa_stream_close(s);
    CHECK_NEAR((double)n, 4.0 * RATE, 2.0);
    printf("  fast + formants, pitch +4: %.3f Hz, tone %.1f dB\n", measure_freq(y, n, f), tone_score(y, n, &f, 1));
    CHECK_NEAR(measure_freq(y, n, f), f, 0.05);
    free(y);
}

int main(void)
{
    fa_engine_config cfg;
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    cfg.worker_threads = 2;
    if (fa_engine_create(&cfg, &g_engine) != FA_OK) { fprintf(stderr, "engine create failed\n"); return 1; }
    if (fa_device_open(g_engine, NULL, &g_dev) != FA_OK) { fprintf(stderr, "device open failed\n"); return 1; }
    if (g_dev->rate != RATE) printf("null device at %u Hz (sources at %u Hz: resampled)\n", g_dev->rate, RATE);

    RUN(test_neutral_leaves_audio_untouched);
    RUN(test_tempo_length_and_tone);
    RUN(test_pitch_accuracy_and_tone);
    RUN(test_chord_quality);
    RUN(test_rate_is_clean_varispeed);
    RUN(test_combined_parameters);
    RUN(test_decode_position_and_seek);
    RUN(test_attached_position_follows_tempo);
    RUN(test_clock_runs_at_tempo);
    RUN(test_cue_out_ends_exactly);
    RUN(test_ramp_reaches_target_with_event);
    RUN(test_scheduled_ramp_as_first_change);
    RUN(test_engaging_mid_play_is_smooth);
    RUN(test_loop_with_tempo_wraps);
    RUN(test_push_stream_tempo_clock);
    RUN(test_push_stream_tempo_timeline_jump);
    RUN(test_params_and_options_api);
    RUN(test_fast_and_formants);

    fa_device_close(g_dev);
    fa_engine_destroy(g_engine);
    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    printf("PASS\n");
    return 0;
}
