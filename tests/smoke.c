/* Smoke test: engine, device enumeration, in-memory WAV decode. */
#include "fatzaudio/fatzaudio.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_RATE 44100u
#define SECONDS 1u
#define NUM_FRAMES (SAMPLE_RATE * SECONDS)

#define CHECK(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); return 1; } } while (0)

static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t* p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }

/* 16-bit mono PCM WAV, 440 Hz sine. */
static uint8_t* make_wav(size_t* out_size)
{
    size_t data_bytes = (size_t)NUM_FRAMES * 2;
    size_t total = 44 + data_bytes;
    uint8_t* b = (uint8_t*)malloc(total);
    uint32_t i;
    if (!b) return NULL;
    memcpy(b, "RIFF", 4); put32(b + 4, (uint32_t)(total - 8)); memcpy(b + 8, "WAVE", 4);
    memcpy(b + 12, "fmt ", 4); put32(b + 16, 16); put16(b + 20, 1); put16(b + 22, 1);
    put32(b + 24, SAMPLE_RATE); put32(b + 28, SAMPLE_RATE * 2); put16(b + 32, 2); put16(b + 34, 16);
    memcpy(b + 36, "data", 4); put32(b + 40, (uint32_t)data_bytes);
    for (i = 0; i < NUM_FRAMES; i++) {
        double v = sin(2.0 * 3.14159265358979323846 * 440.0 * (double)i / (double)SAMPLE_RATE);
        put16(b + 44 + (size_t)i * 2, (uint16_t)(int16_t)(v * 0.5 * 32767.0));
    }
    *out_size = total;
    return b;
}

int main(void)
{
    fa_engine* engine = NULL;
    fa_stream* stream = NULL;
    fa_open_options opts;
    fa_stream_info info;
    fa_result r;
    uint32_t count = 0, i;
    size_t wav_size = 0;
    uint8_t* wav;
    float* buf;
    fa_frames total = 0, got = 0;
    size_t nonzero = 0, n;

    printf("fatzaudio version 0x%06x\n", fa_version());
    CHECK(fa_version() == ((FA_VERSION_MAJOR << 16) | (FA_VERSION_MINOR << 8) | FA_VERSION_PATCH), "version");

    r = fa_engine_create(NULL, &engine);
    CHECK(r == FA_OK && engine, "engine create");

    r = fa_device_count(engine, FA_DEVICE_PLAYBACK, &count);
    printf("playback devices: %u (%s)\n", count, fa_result_string(r));
    for (i = 0; r == FA_OK && i < count; i++) {
        fa_device_info di;
        if (fa_device_get_info(engine, FA_DEVICE_PLAYBACK, i, &di) == FA_OK)
            printf("  [%u] %s%s\n", i, di.name, di.is_default ? " (default)" : "");
    }

    wav = make_wav(&wav_size);
    CHECK(wav, "make wav");

    fa_open_options_init(&opts);
    opts.flags = FA_OPEN_DECODE_ONLY;
    r = fa_stream_open_memory(engine, wav, wav_size, &opts, &stream);
    free(wav);
    CHECK(r == FA_OK && stream, "open memory");

    r = fa_stream_get_info(stream, &info);
    CHECK(r == FA_OK, "get info");
    printf("info: %u Hz, %u ch, length %lld frames\n", info.format.sample_rate, info.format.channels, (long long)info.length_frames);
    CHECK(info.format.sample_rate == SAMPLE_RATE, "sample rate");
    CHECK(info.format.channels == 1, "channels");
    CHECK(info.length_frames == (fa_frames)NUM_FRAMES, "length");

    buf = (float*)malloc(4096 * info.format.channels * sizeof(float));
    CHECK(buf, "alloc");
    for (;;) {
        r = fa_stream_read(stream, buf, 4096, &got);
        if (got == 0) break;
        CHECK(r == FA_OK, "read");
        for (n = 0; n < (size_t)got * info.format.channels; n++) if (buf[n] != 0.0f) nonzero++;
        total += got;
    }
    free(buf);
    printf("read %lld frames, %zu nonzero samples\n", (long long)total, nonzero);
    CHECK(total == (fa_frames)NUM_FRAMES, "frame count == 44100");
    CHECK(nonzero > 0, "nonzero samples");

    fa_stream_close(stream);
    fa_engine_destroy(engine);
    printf("PASS\n");
    return 0;
}
