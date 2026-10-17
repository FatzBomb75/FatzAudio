/* fa_tempo_render: renders an audio file through FatzAudio's tempo, pitch and rate
 * into a 32-bit float WAV, for listening tests and tools/tempo-compare/compare.py.
 *
 *   fa_tempo_render in.wav out.wav [--tempo 1.25] [--pitch -3] [--rate 1]
 *                   [--fast] [--formants]
 *
 * Any format FatzAudio opens works as input. */
#include "fatzaudio/fatzaudio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE* f, uint16_t v) { fwrite(&v, 2, 1, f); }

static void wav_header(FILE* f, uint32_t rate, uint32_t channels, uint64_t frames)
{
    uint64_t data = frames * channels * 4;
    if (data > 0xFFFFFFF0u - 36) data = 0xFFFFFFF0u - 36;
    fwrite("RIFF", 1, 4, f); put32(f, (uint32_t)(36 + data)); fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16); put16(f, 3); put16(f, (uint16_t)channels); put32(f, rate);
    put32(f, rate * channels * 4); put16(f, (uint16_t)(channels * 4)); put16(f, 32);
    fwrite("data", 1, 4, f); put32(f, (uint32_t)data);
}

int main(int argc, char** argv)
{
    fa_engine_config cfg;
    fa_engine* engine = NULL;
    fa_open_options opts;
    fa_tempo_options topts;
    fa_stream* s = NULL;
    fa_stream_info info;
    float tempo = 1.0f, pitch = 0.0f, rate = 1.0f, *buf;
    uint64_t total = 0;
    FILE* out;
    fa_result r;
    int i;
    if (argc < 3) {
        fprintf(stderr, "usage: %s in out.wav [--tempo x] [--pitch semitones] [--rate x] [--fast] [--formants]\n", argv[0]);
        return 2;
    }
    fa_tempo_options_init(&topts);
    for (i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--tempo") && i + 1 < argc) tempo = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--pitch") && i + 1 < argc) pitch = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--fast")) topts.quality = FA_STRETCH_FAST;
        else if (!strcmp(argv[i], "--formants")) topts.keep_formants = 1;
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    if ((r = fa_engine_create(&cfg, &engine)) != FA_OK) { fprintf(stderr, "engine: %s\n", fa_result_string(r)); return 1; }
    fa_open_options_init(&opts);
    opts.flags = FA_OPEN_DECODE_ONLY;
    if ((r = fa_stream_open_path(engine, argv[1], &opts, &s)) != FA_OK) {
        fprintf(stderr, "%s: %s\n", argv[1], fa_result_string(r));
        return 1;
    }
    fa_stream_get_info(s, &info);
    fa_stream_set_tempo_options(s, &topts);
    if ((r = fa_stream_set_param(s, FA_PARAM_TEMPO, tempo)) != FA_OK ||
        (r = fa_stream_set_param(s, FA_PARAM_PITCH, pitch)) != FA_OK ||
        (r = fa_stream_set_param(s, FA_PARAM_RATE, rate)) != FA_OK) {
        fprintf(stderr, "params: %s\n", fa_result_string(r));
        return 1;
    }
    out = fopen(argv[2], "wb");
    if (!out) { perror(argv[2]); return 1; }
    wav_header(out, info.format.sample_rate, info.format.channels, 0);
    buf = (float*)malloc((size_t)4096 * info.format.channels * sizeof(float));
    for (;;) {
        fa_frames got = 0;
        r = fa_stream_read(s, buf, 4096, &got);
        if (got > 0) fwrite(buf, sizeof(float) * info.format.channels, (size_t)got, out);
        total += (uint64_t)got;
        if (r != FA_OK || got == 0) break;
    }
    fseek(out, 0, SEEK_SET);
    wav_header(out, info.format.sample_rate, info.format.channels, total);
    fclose(out);
    free(buf);
    fa_stream_close(s);
    fa_engine_destroy(engine);
    if (r != FA_OK && r != FA_ERROR_END_OF_STREAM) { fprintf(stderr, "read: %s\n", fa_result_string(r)); return 1; }
    return 0;
}
