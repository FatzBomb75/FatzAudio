/* Encoding: WAV, FLAC and MP3 (when LAME is installed), every source (app PCM, a
 * decode-only stream, a device's output, a playing stream), format conversion,
 * tags, forward-only outputs and lifetimes. Files are decoded back with the
 * library's own decoders and compared with what went in.
 *
 * The device is opened on the null backend but never started: the test plays the
 * audio thread by calling fa_device_render. */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "encode.h"
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

#define PI 3.14159265358979323846

static fa_engine* g_engine;
static fa_device* g_dev;

/* ---- signals and helpers ---------------------------------------------------------- */

/* kind 0: a 440 Hz sine on every channel (phase shifted per channel), 1: white noise,
 * 2: silence, 3: a sine with full-scale clipping, 4: a sweep plus noise */
static float* make_signal(int kind, int64_t frames, uint32_t rate, uint32_t channels)
{
    float* x = (float*)malloc((size_t)frames * channels * sizeof(float));
    uint32_t seed = 1234567u;
    int64_t i;
    uint32_t c;
    for (i = 0; i < frames; i++)
        for (c = 0; c < channels; c++) {
            double t = (double)i / rate, v = 0.0;
            seed = seed * 1664525u + 1013904223u;
            switch (kind) {
            case 0: v = 0.5 * sin(2 * PI * 440.0 * t + c * 0.7); break;
            case 1: v = ((double)(seed >> 8) / 16777216.0 - 0.5) * 1.6; break;
            case 2: v = 0.0; break;
            case 3: v = 1.5 * sin(2 * PI * 100.0 * t); break;
            default: v = 0.4 * sin(2 * PI * (100.0 + 2000.0 * t) * t) + ((double)(seed >> 8) / 16777216.0 - 0.5) * 0.01; break;
            }
            x[i * channels + c] = (float)v;
        }
    return x;
}

/* Decodes a whole file with the library's decoders. */
static float* decode_file(const char* path, fa_stream_info* info, int64_t* out_frames)
{
    fa_stream* s = NULL;
    float* all;
    int64_t cap, n = 0;
    *out_frames = 0;
    if (fa_stream_open_path(g_engine, path, NULL, &s) != FA_OK) return NULL;
    fa_stream_get_info(s, info);
    cap = 1 << 16;
    all = (float*)malloc((size_t)cap * info->format.channels * sizeof(float));
    for (;;) {
        fa_frames got = 0;
        fa_result r;
        if (n + 4096 > cap) {
            cap *= 2;
            all = (float*)realloc(all, (size_t)cap * info->format.channels * sizeof(float));
        }
        r = fa_stream_read(s, all + (size_t)n * info->format.channels, 4096, &got);
        n += got;
        if (r != FA_OK || got == 0) break;
    }
    fa_stream_close(s);
    *out_frames = n;
    return all;
}

static int64_t file_size(const char* path)
{
    FILE* f = fopen(path, "rb");
    long n;
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fclose(f);
    return n;
}

static uint8_t* read_file(const char* path, size_t* n)
{
    FILE* f = fopen(path, "rb");
    uint8_t* b;
    long len;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (uint8_t*)malloc((size_t)len + 1);
    *n = fread(b, 1, (size_t)len, f);
    fclose(f);
    return b;
}

static int contains(const uint8_t* b, size_t n, const void* what, size_t wn)
{
    size_t i;
    for (i = 0; i + wn <= n; i++)
        if (memcmp(b + i, what, wn) == 0) return 1;
    return 0;
}

static fa_result encode_pcm(const char* path, const fa_encoder_options* o, const float* x, int64_t frames,
                            uint32_t rate, uint32_t channels)
{
    fa_encoder* e = NULL;
    fa_result r = fa_encoder_create_path(path, o, &e);
    int64_t done = 0;
    if (r != FA_OK) return r;
    /* uneven pieces, as an app would push them */
    while (r == FA_OK && done < frames) {
        int64_t n = 1000 + (done % 3000);
        if (n > frames - done) n = frames - done;
        r = fa_encoder_write(e, x + (size_t)done * channels, n, rate, channels);
        done += n;
    }
    if (r != FA_OK) { fa_encoder_close(e); return r; }
    return fa_encoder_close(e);
}

static double max_error(const float* a, const float* b, int64_t n)
{
    double m = 0.0;
    int64_t i;
    for (i = 0; i < n; i++) {
        double d = fabs((double)a[i] - (double)b[i]);
        if (d > m) m = d;
    }
    return m;
}

/* What the encoder's quantizer makes of `x` (24 bit: plain rounding with clipping). */
static float q24(float x)
{
    double v = floor((double)x * 8388608.0 + 0.5);
    if (v > 8388607.0) v = 8388607.0;
    if (v < -8388608.0) v = -8388608.0;
    return (float)(v / 8388608.0);
}

/* Amplitude and phase of `freq` in channel `c` (a single-bin DFT). */
static double tone_amplitude(const float* x, int64_t frames, uint32_t channels, uint32_t c, double freq, double rate)
{
    double re = 0.0, im = 0.0;
    int64_t i;
    for (i = 0; i < frames; i++) {
        double ph = 2 * PI * freq * (double)i / rate;
        re += x[i * channels + c] * cos(ph);
        im += x[i * channels + c] * sin(ph);
    }
    return 2.0 * sqrt(re * re + im * im) / (double)frames;
}

/* ---- WAV ------------------------------------------------------------------------------- */

static void test_wav_formats(void)
{
    const uint32_t rate = 44100, ch = 2;
    const int64_t n = 44100;
    float* x = make_signal(0, n, rate, ch);
    uint32_t bits[3] = { 16, 24, 32 };
    int k;
    for (k = 0; k < 3; k++) {
        fa_encoder_options o;
        fa_stream_info info;
        int64_t got, i;
        float* y;
        fa_encoder_options_init(&o);
        o.format = FA_ENCODE_WAV;
        o.bits_per_sample = bits[k];
        CHECK_EQ(encode_pcm("enc_test.wav", &o, x, n, rate, ch), FA_OK);
        y = decode_file("enc_test.wav", &info, &got);
        CHECK(y != NULL);
        CHECK_EQ(got, n);
        CHECK_EQ(info.format.sample_rate, rate);
        CHECK_EQ(info.format.channels, ch);
        if (bits[k] == 32) {
            CHECK(memcmp(x, y, (size_t)n * ch * sizeof(float)) == 0);   /* bit exact */
        } else if (bits[k] == 24) {
            for (i = 0; i < n * ch; i++) CHECK_NEAR(y[i], q24(x[i]), 1e-9);
        } else {
            /* TPDF dither: within 1.5 LSB, and the error is not the plain rounding's */
            CHECK(max_error(x, y, n * ch) < 1.5 / 32768.0);
        }
        free(y);
    }
    CHECK_EQ(file_size("enc_test.wav"), 82 + n * ch * 4);   /* RIFF, JUNK, fmt (float), data headers */
    free(x);
    remove("enc_test.wav");
}

static void test_wav_multichannel_and_tags(void)
{
    const uint32_t rate = 48000, ch = 6;
    const int64_t n = 4801;   /* odd: the data chunk is padded */
    float* x = make_signal(0, n, rate, ch);
    fa_encoder_options o;
    fa_stream_info info;
    int64_t got, i;
    float* y;
    size_t fn;
    uint8_t* bytes;
    fa_encoder_options_init(&o);
    o.bits_per_sample = 24;
    o.title = "Night Train";
    o.artist = "Fatz";
    o.track_number = 7;
    CHECK_EQ(encode_pcm("enc_test6.wav", &o, x, n, rate, ch), FA_OK);
    y = decode_file("enc_test6.wav", &info, &got);
    CHECK(y != NULL);
    CHECK_EQ(got, n);
    CHECK_EQ(info.format.channels, 6);
    for (i = 0; i < n * ch; i++) CHECK_NEAR(y[i], q24(x[i]), 1e-9);
    bytes = read_file("enc_test6.wav", &fn);
    CHECK(contains(bytes, fn, "INAMNight Train", 4) && contains(bytes, fn, "Night Train", 11));
    CHECK(contains(bytes, fn, "IART", 4) && contains(bytes, fn, "ITRK", 4));
    CHECK(contains(bytes, fn, "\xFE\xFF", 2));             /* WAVE_FORMAT_EXTENSIBLE */
    CHECK((fn & 1) == 0);
    free(bytes);
    free(y);
    free(x);
    remove("enc_test6.wav");
}

/* ---- FLAC ------------------------------------------------------------------------------ */

static int g_have_flac_cli = -1;

/* The reference decoder checks every CRC and the MD5, when it is installed. */
static int flac_cli_ok(const char* path)
{
    char cmd[512];
    if (g_have_flac_cli < 0) g_have_flac_cli = system("flac --version > /dev/null 2>&1") == 0;
    if (!g_have_flac_cli) return 1;
    snprintf(cmd, sizeof(cmd), "flac -t -s \"%s\" 2> /dev/null", path);
    return system(cmd) == 0;
}

static void test_flac_lossless(void)
{
    static const struct { int kind; uint32_t ch, bits; int level; int64_t frames; } cases[] = {
        { 0, 2, 24, 5, 44100 }, { 1, 2, 24, 5, 10000 }, { 2, 2, 24, 5, 5000 }, { 3, 2, 24, 8, 30000 },
        { 4, 1, 24, 0, 20000 }, { 4, 2, 24, 2, 20001 }, { 4, 2, 24, 3, 20001 }, { 4, 6, 24, 7, 9000 },
        { 0, 8, 24, 5, 4097 }, { 4, 2, 24, 8, 12345 }, { 0, 2, 16, 5, 44100 }, { 1, 1, 16, 8, 8192 }
    };
    size_t k;
    for (k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        const uint32_t rate = 44100, ch = cases[k].ch;
        const int64_t n = cases[k].frames;
        float* x = make_signal(cases[k].kind, n, rate, ch);
        fa_encoder_options o;
        fa_stream_info info;
        int64_t got, i;
        float* y;
        fa_encoder_options_init(&o);
        o.format = FA_ENCODE_FLAC;
        o.bits_per_sample = cases[k].bits;
        o.quality = cases[k].level;
        CHECK_EQ(encode_pcm("enc_test.flac", &o, x, n, rate, ch), FA_OK);
        y = decode_file("enc_test.flac", &info, &got);
        CHECK(y != NULL);
        CHECK_EQ(got, n);
        CHECK_EQ(info.format.channels, ch);
        CHECK_EQ(info.length_frames, n);                        /* STREAMINFO completed */
        if (cases[k].bits == 24) {
            for (i = 0; i < n * ch; i++)
                if (fabs(y[i] - q24(x[i])) > 1e-9) {
                    fprintf(stderr, "  case %u: sample %lld: %.9f vs %.9f\n", (unsigned)k, (long long)i, y[i], q24(x[i]));
                    CHECK(0);
                }
        } else {
            CHECK(max_error(x, y, n * ch) < 1.5 / 32768.0);
        }
        CHECK(flac_cli_ok("enc_test.flac"));
        free(y);
        free(x);
    }
    remove("enc_test.flac");
}

/* A 16-bit source (samples on the 16-bit grid) stays bit exact: no dither is added. */
static void test_16bit_source_is_lossless(void)
{
    const uint32_t rate = 44100, ch = 2;
    const int64_t n = 30000;
    float* x = make_signal(4, n, rate, ch);
    int64_t i;
    int f;
    for (i = 0; i < n * ch; i++) x[i] = (float)(floor((double)x[i] * 32768.0 + 0.5) / 32768.0);
    for (f = 0; f < 2; f++) {
        fa_encoder_options o;
        fa_stream_info info;
        int64_t got;
        float* y;
        fa_encoder_options_init(&o);
        o.format = f ? FA_ENCODE_FLAC : FA_ENCODE_WAV;
        CHECK_EQ(encode_pcm("enc_16.bin", &o, x, n, rate, ch), FA_OK);
        y = decode_file("enc_16.bin", &info, &got);
        CHECK(y != NULL);
        CHECK_EQ(got, n);
        CHECK(memcmp(x, y, (size_t)n * ch * sizeof(float)) == 0);
        free(y);
    }
    free(x);
    remove("enc_16.bin");
}

static void test_flac_compresses(void)
{
    const uint32_t rate = 44100, ch = 2;
    const int64_t n = 44100 * 3;
    float* x = make_signal(0, n, rate, ch);
    fa_encoder_options o;
    int64_t fast, best;
    fa_encoder_options_init(&o);
    o.format = FA_ENCODE_FLAC;
    o.quality = 0;
    CHECK_EQ(encode_pcm("enc_c0.flac", &o, x, n, rate, ch), FA_OK);
    o.quality = 8;
    CHECK_EQ(encode_pcm("enc_c8.flac", &o, x, n, rate, ch), FA_OK);
    fast = file_size("enc_c0.flac");
    best = file_size("enc_c8.flac");
    printf("  sine 16-bit stereo: WAV %lld, FLAC level 0 %lld, level 8 %lld bytes\n", (long long)(n * 4), (long long)fast,
           (long long)best);
    CHECK(best < fast);
    CHECK(best < n * 4 / 3);
    free(x);
    remove("enc_c0.flac");
    remove("enc_c8.flac");
}

/* ---- conversion ------------------------------------------------------------------------- */

static void test_resample_and_mix(void)
{
    const uint32_t in_rate = 48000, out_rate = 44100;
    const int64_t n = 48000;
    float* x = make_signal(0, n, in_rate, 2);
    fa_encoder_options o;
    fa_stream_info info;
    int64_t got, i;
    float* y;
    double amp;
    fa_encoder_options_init(&o);
    o.bits_per_sample = 32;
    o.sample_rate = out_rate;
    o.channels = 1;
    for (i = 0; i < n; i++) x[i * 2 + 1] = x[i * 2];   /* same tone in both: the mono mix keeps it */
    CHECK_EQ(encode_pcm("enc_rs.wav", &o, x, n, in_rate, 2), FA_OK);
    y = decode_file("enc_rs.wav", &info, &got);
    CHECK(y != NULL);
    CHECK_EQ(info.format.sample_rate, out_rate);
    CHECK_EQ(info.format.channels, 1);
    CHECK_EQ(got, 44100);                                   /* exact length */
    amp = tone_amplitude(y + 2000, got - 4000, 1, 0, 440.0, out_rate);
    CHECK_NEAR(amp, 0.5, 0.01);
    free(y);
    free(x);
    remove("enc_rs.wav");
}

/* ---- sources ----------------------------------------------------------------------------- */

static void test_encode_stream_offline(void)
{
    const uint32_t rate = 22050, ch = 2;
    const int64_t n = 22050 * 2 + 17;
    float* x = make_signal(4, n, rate, ch);
    fa_encoder_options o;
    fa_encoder* e = NULL;
    fa_stream* s = NULL;
    fa_encoder_status st;
    fa_stream_info info;
    int64_t got, i;
    float* y;
    fa_encoder_options_init(&o);
    o.bits_per_sample = 32;
    CHECK_EQ(encode_pcm("enc_src.wav", &o, x, n, rate, ch), FA_OK);

    CHECK_EQ(fa_stream_open_path(g_engine, "enc_src.wav", NULL, &s), FA_OK);
    o.format = FA_ENCODE_FLAC;
    o.bits_per_sample = 24;
    CHECK_EQ(fa_encoder_create_path("enc_conv.flac", &o, &e), FA_OK);
    CHECK_EQ(fa_encoder_encode_stream(e, s), FA_OK);
    CHECK_EQ(fa_encoder_encode_stream(e, s), FA_ERROR_BUSY);
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_ERROR_BUSY);   /* the encoder reads it */
    CHECK_EQ(fa_encoder_wait(e, 10000000000ll), FA_OK);
    CHECK_EQ(fa_encoder_get_status(e, &st), FA_OK);
    CHECK_EQ(st.state, FA_ENCODER_FINISHED);
    CHECK_EQ(st.frames, n);
    CHECK_EQ(st.input.sample_rate, rate);
    CHECK_EQ(st.output.sample_format, FA_FORMAT_S24);
    CHECK_EQ(fa_encoder_close(e), FA_OK);
    fa_stream_close(s);

    y = decode_file("enc_conv.flac", &info, &got);
    CHECK(y != NULL);
    CHECK_EQ(got, n);
    for (i = 0; i < n * ch; i++) CHECK_NEAR(y[i], q24(x[i]), 1e-9);
    free(y);
    free(x);
    remove("enc_src.wav");
    remove("enc_conv.flac");
}

/* Plays the audio thread for `frames` frames in callbacks of 512, waiting for the
 * workers to fill each attached stream first. */
static void render(int64_t frames)
{
    float out[512 * 2];
    while (frames > 0) {
        uint32_t n = frames < 512 ? (uint32_t)frames : 512, i;
        fa_voice_list* list = (fa_voice_list*)fa_load_ptr(&g_dev->voices);
        int tries;
        for (tries = 0; list && tries < 2000; tries++) {
            int ready = 1;
            for (i = 0; i < list->count; i++) {
                fa_stream* s = list->items[i];
                if (fa_ring_readable(&s->ring) < (int64_t)n && !fa_load32(&s->eof)) ready = 0;
            }
            if (ready) break;
            fa_sleep_ms(1);
        }
        fa_fetch_add64(&g_dev->epoch, 1);
        fa_device_render(g_dev, out, n);
        fa_fetch_add64(&g_dev->epoch, 1);
        frames -= n;
    }
}

static void test_record_device_and_stream(void)
{
    const uint32_t rate = g_dev->rate;
    const int64_t n = rate;   /* 1 s */
    float* x = make_signal(0, n, rate, 2);
    fa_encoder_options o;
    fa_encoder *dev_enc = NULL, *str_enc = NULL, *str_enc2 = NULL;
    fa_stream* s = NULL;
    fa_encoder_status st;
    fa_stream_info info;
    int64_t got_dev, got_str;
    float *yd, *ys;
    fa_encoder_options_init(&o);
    o.bits_per_sample = 32;
    CHECK_EQ(encode_pcm("enc_play.wav", &o, x, n, rate, 2), FA_OK);
    CHECK_EQ(fa_stream_open_path(g_engine, "enc_play.wav", NULL, &s), FA_OK);
    CHECK_EQ(fa_encoder_create_path("enc_dev.wav", &o, &dev_enc), FA_OK);
    CHECK_EQ(fa_encoder_create_path("enc_str.wav", &o, &str_enc), FA_OK);
    CHECK_EQ(fa_encoder_create_path("enc_str2.wav", &o, &str_enc2), FA_OK);
    CHECK_EQ(fa_encoder_attach_stream(str_enc, s), FA_ERROR_INVALID_ARG);   /* not attached yet */
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_VOLUME, 0.5f), FA_OK);
    CHECK_EQ(fa_encoder_attach_device(dev_enc, g_dev), FA_OK);
    CHECK_EQ(fa_encoder_attach_stream(str_enc, s), FA_OK);
    CHECK_EQ(fa_encoder_attach_stream(str_enc2, s), FA_OK);
    CHECK_EQ(fa_encoder_attach_device(str_enc, g_dev), FA_ERROR_BUSY);
    CHECK_EQ(fa_encoder_encode_stream(dev_enc, s), FA_ERROR_BUSY);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    render(n / 2);
    CHECK_EQ(fa_encoder_set_paused(str_enc2, 1), FA_OK);
    render(n / 2 + 4096);   /* past the end: the stream stops, the device keeps playing silence */
    fa_sleep_ms(50);
    CHECK_EQ(fa_encoder_get_status(str_enc, &st), FA_OK);
    CHECK_EQ(st.state, FA_ENCODER_RUNNING);
    CHECK_EQ(st.dropped_frames, 0);
    CHECK_EQ(fa_encoder_close(str_enc), FA_OK);
    CHECK_EQ(fa_encoder_get_status(str_enc2, &st), FA_OK);
    CHECK(st.paused);
    fa_stream_close(s);                                     /* str_enc2 loses its source */
    CHECK_EQ(fa_encoder_close(str_enc2), FA_OK);
    CHECK_EQ(fa_encoder_close(dev_enc), FA_OK);

    ys = decode_file("enc_str.wav", &info, &got_str);
    CHECK(ys != NULL);
    CHECK_EQ(got_str, n);                                   /* exactly the stream's frames */
    yd = decode_file("enc_dev.wav", &info, &got_dev);
    CHECK(yd != NULL);
    CHECK_EQ(got_dev, n + 4096);                            /* everything the device played */
    /* the only stream playing: its recording is the device's output (which the
     * limiter, even when off, delays by a fixed amount), volume applied */
    CHECK(memcmp(ys, yd + (size_t)g_dev->limiter.delay * 2, (size_t)(n - 4096) * 2 * sizeof(float)) == 0);
    CHECK(tone_amplitude(ys, n, 2, 0, 440.0, rate) < 0.4);
    free(ys);
    free(yd);
    ys = decode_file("enc_str2.wav", &info, &got_str);
    CHECK(ys != NULL);
    CHECK(got_str >= n / 2 - 512 && got_str <= n / 2 + 512);   /* paused half way */
    free(ys);
    free(x);
    remove("enc_play.wav");
    remove("enc_dev.wav");
    remove("enc_str.wav");
    remove("enc_str2.wav");
}

static void test_device_closed_while_recording(void)
{
    fa_device* d = NULL;
    fa_encoder* e = NULL;
    float out[512 * 2];
    fa_encoder_options o;
    fa_encoder_status st;
    fa_encoder_options_init(&o);
    CHECK_EQ(fa_device_open(g_engine, NULL, &d), FA_OK);
    CHECK_EQ(fa_encoder_create_path("enc_gone.wav", &o, &e), FA_OK);
    CHECK_EQ(fa_encoder_attach_device(e, d), FA_OK);
    fa_device_render(d, out, 512);
    fa_device_close(d);
    fa_sleep_ms(20);
    CHECK_EQ(fa_encoder_get_status(e, &st), FA_OK);
    CHECK_EQ(st.frames, 512);
    CHECK_EQ(fa_encoder_close(e), FA_OK);
    remove("enc_gone.wav");
}

/* ---- outputs ------------------------------------------------------------------------------ */

typedef struct mem_out { uint8_t* data; size_t n, cap; int closed; } mem_out;

static fa_result FA_CALL mem_write(void* user, const void* data, size_t size)
{
    mem_out* m = (mem_out*)user;
    if (m->n + size > m->cap) {
        m->cap = (m->n + size) * 2;
        m->data = (uint8_t*)realloc(m->data, m->cap);
    }
    memcpy(m->data + m->n, data, size);
    m->n += size;
    return FA_OK;
}
static void FA_CALL mem_close(void* user) { ((mem_out*)user)->closed++; }
static fa_result FA_CALL fail_write(void* user, const void* data, size_t size)
{
    (void)user; (void)data; (void)size;
    return FA_ERROR_NETWORK;
}

static void test_forward_only_output(void)
{
    const uint32_t rate = 32000, ch = 2;
    const int64_t n = 20000;
    float* x = make_signal(4, n, rate, ch);
    int f;
    for (f = 0; f < 2; f++) {
        fa_encoder_output out = { mem_write, NULL, mem_close };
        mem_out m;
        fa_encoder_options o;
        fa_encoder* e = NULL;
        fa_stream* s = NULL;
        fa_stream_info info;
        float y[4096 * 2];
        int64_t total = 0;
        memset(&m, 0, sizeof(m));
        fa_encoder_options_init(&o);
        o.format = f == 0 ? FA_ENCODE_WAV : FA_ENCODE_FLAC;
        o.bits_per_sample = 24;
        CHECK_EQ(fa_encoder_create_output(&out, &m, &o, &e), FA_OK);
        CHECK_EQ(fa_encoder_write(e, x, n, rate, ch), FA_OK);
        CHECK_EQ(fa_encoder_write(e, x, 10, 44100, ch), FA_ERROR_INVALID_ARG);   /* another format */
        CHECK_EQ(fa_encoder_close(e), FA_OK);
        CHECK_EQ(m.closed, 1);
        /* decodable as it is: "length unknown" headers */
        CHECK_EQ(fa_stream_open_memory(g_engine, m.data, m.n, NULL, &s), FA_OK);
        fa_stream_get_info(s, &info);
        CHECK_EQ(info.format.sample_rate, rate);
        for (;;) {
            fa_frames got = 0;
            fa_result r = fa_stream_read(s, y, 4096, &got);
            total += got;
            if (r != FA_OK || got == 0) break;
        }
        fa_stream_close(s);
        CHECK_EQ(total, n);
        free(m.data);
    }
    free(x);
}

/* A seekable output: every format completes its header and leaves the position at
 * the end, so the app can keep writing after the encoder closes. */
typedef struct seek_out { uint8_t data[1 << 20]; int64_t pos, size; } seek_out;

static fa_result FA_CALL seek_write(void* user, const void* data, size_t size)
{
    seek_out* m = (seek_out*)user;
    if (m->pos + (int64_t)size > (int64_t)sizeof(m->data)) return FA_ERROR_IO;
    memcpy(m->data + m->pos, data, size);
    m->pos += (int64_t)size;
    if (m->pos > m->size) m->size = m->pos;
    return FA_OK;
}
static fa_result FA_CALL seek_seek(void* user, int64_t offset)
{
    ((seek_out*)user)->pos = offset;
    return FA_OK;
}

static void test_seekable_output(void)
{
    const uint32_t rate = 32000, ch = 2;
    const int64_t n = 20001;   /* odd: 24-bit stereo WAV data needs a pad byte */
    float* x = make_signal(5, n, rate, ch);
    seek_out* m = (seek_out*)malloc(sizeof(seek_out));
    int f;
    for (f = 0; f < 2; f++) {
        fa_encoder_output out = { seek_write, seek_seek, NULL };
        fa_encoder_options o;
        fa_encoder* e = NULL;
        fa_stream* s = NULL;
        fa_stream_info info;
        memset(m, 0, sizeof(*m));
        fa_encoder_options_init(&o);
        o.format = f == 0 ? FA_ENCODE_WAV : FA_ENCODE_FLAC;
        o.bits_per_sample = 24;
        o.channels = 1;
        CHECK_EQ(fa_encoder_create_output(&out, m, &o, &e), FA_OK);
        CHECK_EQ(fa_encoder_write(e, x, n, rate, ch), FA_OK);
        CHECK_EQ(fa_encoder_close(e), FA_OK);
        CHECK_EQ(m->pos, m->size);
        CHECK_EQ(fa_stream_open_memory(g_engine, m->data, (size_t)m->size, NULL, &s), FA_OK);
        fa_stream_get_info(s, &info);
        CHECK_EQ(info.length_frames, n);   /* the completed header */
        fa_stream_close(s);
    }
    free(m);
    free(x);
}

static void test_errors(void)
{
    fa_encoder_options o;
    fa_encoder* e = NULL;
    fa_encoder_output bad = { fail_write, NULL, NULL };
    fa_encoder_status st;
    float x[2048 * 2] = { 0 };
    fa_result r;
    int i;
    fa_encoder_options_init(&o);
    o.bits_per_sample = 20;
    CHECK_EQ(fa_encoder_create_path("enc_bad.wav", &o, &e), FA_ERROR_INVALID_ARG);
    o.bits_per_sample = 32;
    o.format = FA_ENCODE_FLAC;
    CHECK_EQ(fa_encoder_create_path("enc_bad.flac", &o, &e), FA_ERROR_INVALID_ARG);
    o.format = (fa_encode_format)99;
    CHECK_EQ(fa_encoder_create_path("enc_bad.x", &o, &e), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_encoder_create_path("no/such/dir/x.wav", NULL, &e), FA_ERROR_IO);

    /* a failing output: reported by write (eventually), status and close */
    CHECK_EQ(fa_encoder_create_output(&bad, NULL, NULL, &e), FA_OK);
    r = FA_OK;
    for (i = 0; i < 200 && r == FA_OK; i++) {
        r = fa_encoder_write(e, x, 2048, 44100, 2);
        fa_sleep_ms(1);
    }
    CHECK_EQ(r, FA_ERROR_NETWORK);
    CHECK_EQ(fa_encoder_get_status(e, &st), FA_OK);
    CHECK_EQ(st.state, FA_ENCODER_FAILED);
    CHECK_EQ(st.error, FA_ERROR_NETWORK);
    CHECK_EQ(fa_encoder_close(e), FA_ERROR_NETWORK);

    /* closed without any audio: a valid, empty file */
    fa_encoder_options_init(&o);
    o.format = FA_ENCODE_FLAC;
    CHECK_EQ(fa_encoder_create_path("enc_empty.flac", &o, &e), FA_OK);
    CHECK_EQ(fa_encoder_get_status(e, &st), FA_OK);
    CHECK_EQ(st.state, FA_ENCODER_IDLE);
    CHECK_EQ(fa_encoder_wait(e, 1000000), FA_ERROR_BUSY);
    CHECK_EQ(fa_encoder_close(e), FA_OK);
    CHECK(flac_cli_ok("enc_empty.flac"));
    remove("enc_empty.flac");
}

/* ---- MP3 ---------------------------------------------------------------------------------- */

static void test_mp3(void)
{
    const uint32_t rate = 44100;
    const int64_t n = 44100 * 2;
    float* x;
    fa_encoder_options o;
    fa_stream_info info;
    fa_lame_info li;
    int64_t got;
    float* y;
    size_t fn;
    uint8_t* bytes;
    int k;
    if (fa_lame_load(NULL) != FA_OK) {
        printf("  LAME not installed: skipped\n");
        return;
    }
    CHECK_EQ(fa_lame_get_info(&li), FA_OK);
    CHECK(li.loaded);
    printf("  LAME %s from %s\n", li.version, li.path);
    x = make_signal(0, n, rate, 2);
    for (k = 0; k < 3; k++) {
        fa_encoder_options_init(&o);
        o.format = FA_ENCODE_MP3;
        o.title = "T\xc3\xabst \xe2\x9c\x93";
        o.artist = "FatzAudio";
        if (k == 1) o.bitrate_kbps = 128;
        if (k == 2) { o.channels = 1; o.sample_rate = 0; }
        CHECK_EQ(encode_pcm("enc_test.mp3", &o, x, n, k == 2 ? 96000 : rate, 2), FA_OK);
        y = decode_file("enc_test.mp3", &info, &got);
        CHECK(y != NULL);
        if (k == 2) {
            /* 96 kHz in: 48 kHz mono out */
            CHECK_EQ(info.format.sample_rate, 48000);
            CHECK_EQ(info.format.channels, 1);
            CHECK(got > n / 2 - 1152 && got < n / 2 + 2304);
        } else {
            CHECK_EQ(info.format.sample_rate, rate);
            CHECK_EQ(info.format.channels, 2);
            /* the LAME header trims the encoder delay and padding */
            CHECK(got >= n - 1152 && got <= n + 1152);
            /* (LAME's 128 kbps CBR loses about 0.4 dB on this phase-shifted stereo tone) */
            CHECK_NEAR(tone_amplitude(y + 4096, got - 8192, 2, 0, 440.0, rate), 0.5, k == 1 ? 0.05 : 0.01);
        }
        free(y);
        bytes = read_file("enc_test.mp3", &fn);
        CHECK(fn > 3 && memcmp(bytes, "ID3", 3) == 0);
        CHECK(contains(bytes, fn, "TIT2", 4) && contains(bytes, fn, "TPE1", 4));
        CHECK(contains(bytes, fn, k == 1 ? "Info" : "Xing", 4));
        free(bytes);
    }
    fa_encoder_options_init(&o);
    o.format = FA_ENCODE_MP3;
    o.sample_rate = 96000;
    {
        fa_encoder* e = NULL;
        CHECK_EQ(fa_encoder_create_path("enc_bad.mp3", &o, &e), FA_ERROR_INVALID_ARG);
    }
    free(x);
    remove("enc_test.mp3");
}

int main(void)
{
    fa_engine_config cfg;
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    cfg.worker_threads = 2;
    if (fa_engine_create(&cfg, &g_engine) != FA_OK) { fprintf(stderr, "engine create failed\n"); return 1; }
    if (fa_device_open(g_engine, NULL, &g_dev) != FA_OK) { fprintf(stderr, "device open failed\n"); return 1; }
    fa_engine_set_limiter(g_engine, FA_LIMITER_OFF, 0.0);

    RUN(test_wav_formats);
    RUN(test_wav_multichannel_and_tags);
    RUN(test_flac_lossless);
    RUN(test_16bit_source_is_lossless);
    RUN(test_flac_compresses);
    RUN(test_resample_and_mix);
    RUN(test_encode_stream_offline);
    RUN(test_record_device_and_stream);
    RUN(test_device_closed_while_recording);
    RUN(test_forward_only_output);
    RUN(test_seekable_output);
    RUN(test_errors);
    RUN(test_mp3);

    fa_device_close(g_dev);
    fa_engine_destroy(g_engine);
    if (g_failures) { fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    printf("all encoding tests passed\n");
    return 0;
}
