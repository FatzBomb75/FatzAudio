/* The system's real audio output and input, where a machine has them: the CI jobs
 * on the Android emulator, the iOS simulator and macOS set FA_TEST_REAL_SYSTEM=1.
 * Every other test plays into the silent null output, so this is the one that runs
 * AAudio / OpenSL ES, Core Audio and the rest for real. Without the variable it
 * skips (a developer's PC may have no output, or one that would play the tone).
 *
 * The output must be a real backend (not miniaudio's null one), open, start, and
 * pull audio: a stream's position moves at about the device's rate. A mic, when the
 * system has an input, must open and run; a refused microphone permission is
 * reported, not failed (a command-line program has no usage string to ask with);
 * on iOS, where that ends the program, the input is not tried. */
#include "fa_internal.h"
#include "mixer.h"
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
#define RUN(t) do { int before_ = g_failures; printf("%s\n", #t); t(); if (g_failures == before_) printf("  ok\n"); } while (0)

static fa_engine* g_engine;

static void put16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t* p, uint32_t v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }

/* Mono 16-bit WAV: a quiet 440 Hz tone. */
static uint8_t* make_wav(uint32_t rate, uint32_t frames, size_t* size)
{
    uint8_t* b = (uint8_t*)malloc(44 + (size_t)frames * 2);
    uint32_t i;
    if (!b) return NULL;
    memcpy(b, "RIFF", 4); put32(b + 4, 36 + frames * 2); memcpy(b + 8, "WAVEfmt ", 8);
    put32(b + 16, 16); put16(b + 20, 1); put16(b + 22, 1); put32(b + 24, rate); put32(b + 28, rate * 2);
    put16(b + 32, 2); put16(b + 34, 16); memcpy(b + 36, "data", 4); put32(b + 40, frames * 2);
    for (i = 0; i < frames; i++) put16(b + 44 + i * 2, (uint16_t)(int16_t)(3000 * sin(2 * 3.14159265358979 * 440 * i / rate)));
    *size = 44 + (size_t)frames * 2;
    return b;
}

static void test_real_output(void)
{
    uint32_t count = 0, i;
    fa_device* d = NULL;
    fa_stream* s = NULL;
    size_t size = 0;
    uint8_t* wav = make_wav(44100, 44100 * 5, &size);
    fa_frames pos = 0;
    int64_t t0, ms;
    printf("  backend: %s\n", ma_get_backend_name(g_engine->context.backend));
    CHECK(g_engine->context.backend != ma_backend_null);
    CHECK_EQ(fa_device_count(g_engine, FA_DEVICE_PLAYBACK, &count), FA_OK);
    for (i = 0; i < count; i++) {
        fa_device_info di;
        if (fa_device_get_info(g_engine, FA_DEVICE_PLAYBACK, i, &di) == FA_OK)
            printf("  output %u: %s, %u Hz, %u channels%s\n", i, di.name, di.native_sample_rate, di.native_channels,
                   di.is_default ? " (default)" : "");
    }
    CHECK(count > 0);
    CHECK(wav != NULL);
    CHECK_EQ(fa_device_open(g_engine, NULL, &d), FA_OK);
    printf("  opened at %u Hz\n", d->rate);
    CHECK_EQ(fa_stream_open_memory(g_engine, wav, size, NULL, &s), FA_OK);
    CHECK_EQ(fa_stream_attach(s, d, NULL), FA_OK);
    CHECK_EQ(fa_device_start(d), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    /* A second of audio, which the device pulls in real time. */
    t0 = fa_monotonic_ns();
    while (pos < 44100 && fa_monotonic_ns() - t0 < 5000000000LL) {
        fa_sleep_ms(20);
        fa_stream_get_position(s, &pos);
    }
    ms = (fa_monotonic_ns() - t0) / 1000000;
    printf("  1 s of audio played in %lld ms\n", (long long)ms);
    CHECK(pos >= 44100);
    CHECK(ms >= 700);           /* real time, not as fast as it decodes */
    fa_stream_close(s);
    fa_device_close(d);
    free(wav);
}

static void test_real_input(void)
{
    uint32_t count = 0;
    fa_mixer_config mc;
    fa_mixer* m = NULL;
    fa_mic_config c;
    fa_mic* mic = NULL;
    fa_result r;
#if defined(__APPLE__) && TARGET_OS_IPHONE
    /* iOS ends a program that opens the microphone without an app's usage string. */
    (void)count; (void)mc; (void)m; (void)c; (void)mic; (void)r;
    printf("  skipped (needs an app with NSMicrophoneUsageDescription)\n");
    return;
#endif
    CHECK_EQ(fa_device_count(g_engine, FA_DEVICE_CAPTURE, &count), FA_OK);
    printf("  %u inputs\n", count);
    if (count == 0) { printf("  skipped (no input on this system)\n"); return; }
    fa_mixer_config_init(&mc);
    CHECK_EQ(fa_mixer_create(g_engine, &mc, &m), FA_OK);
    fa_mic_config_init(&c);
    r = fa_mixer_add_mic(m, &c, &mic);
    if (r == FA_ERROR_PERMISSION_DENIED) {
        printf("  microphone permission refused: not checked\n");
        fa_mixer_destroy(m);
        return;
    }
    CHECK_EQ(r, FA_OK);
    fa_sleep_ms(500);
    printf("  input at %u Hz, %u channels\n", mic->cap->device.sampleRate, mic->cap->device.capture.channels);
    CHECK_EQ(ma_device_get_state(&mic->cap->device), ma_device_state_started);
    fa_mixer_destroy(m);
}

int main(void)
{
    fa_engine_config cfg;
    const char* real = getenv("FA_TEST_REAL_SYSTEM");
    if (!real || strcmp(real, "1") != 0) {
        printf("skipped (set FA_TEST_REAL_SYSTEM=1 on a machine with audio output)\n");
        return 0;
    }
    fa_engine_config_init(&cfg);
    cfg.worker_threads = 2;
    if (fa_engine_create(&cfg, &g_engine) != FA_OK) { fprintf(stderr, "engine create failed\n"); return 1; }
    RUN(test_real_output);
    RUN(test_real_input);
    fa_engine_destroy(g_engine);
    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    printf("all device tests passed\n");
    return 0;
}
