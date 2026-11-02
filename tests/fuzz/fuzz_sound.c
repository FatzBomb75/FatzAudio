/* Fuzz target: sounds (fa_sound_*), the short samples played as many voices. The input is a script and a file:
 * byte 0 is the script's length (up to 63), then that many script bytes, then the file. The file is loaded as a
 * sound (any format a stream opens; when it is none, its bytes are taken as 16-bit mono PCM at 22050 Hz). Each script
 * byte is op (low 3 bits) and argument (upper 5):
 *   0 play at volume arg/16, pan (arg - 16) / 16    1 stop all         2 limit arg + 1 voices, steal rule arg % 3
 *   3 list the voices                               4 close the oldest voice (the app closing one)
 *   5 render arg + 1 blocks of the device           6 detach the oldest voice    7 info and a PCM reload
 * The device is the null backend and never started: the target is the audio thread (as tests/test_sound.c is). */
#include "fa_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static fa_engine* g_engine;
static fa_device* g_dev;

static fa_sound* load(const uint8_t* file, size_t n, const fa_sound_options* so)
{
    fa_sound* snd = NULL;
    if (fa_sound_load_memory(g_engine, file, n, so, &snd) == FA_OK) return snd;
    if (n >= 2) {
        fa_audio_format f;
        memset(&f, 0, sizeof(f));
        f.sample_format = FA_FORMAT_S16;
        f.sample_rate = 22050;
        f.channels = 1;
        if (fa_sound_load_pcm(g_engine, &f, file, n / 2, so, &snd) == FA_OK) return snd;
    }
    return NULL;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    size_t sn, i;
    fa_sound_options so;
    fa_sound* snd;
    if (size < 2) return 0;
    if (!g_engine) {
        fa_engine_config cfg;
        fa_engine_config_init(&cfg);
        cfg.flags = FA_ENGINE_NULL_OUTPUT;
        cfg.worker_threads = 2;
        if (fa_engine_create(&cfg, &g_engine) != FA_OK) abort();
        if (fa_device_open(g_engine, NULL, &g_dev) != FA_OK) abort();
    }
    sn = data[0] & 63;
    if (sn > size - 1) sn = size - 1;
    fa_sound_options_init(&so);
    so.max_voices = 4;
    snd = load(data + 1 + sn, size - 1 - sn, &so);
    if (!snd) return 0;
    for (i = 0; i < sn; i++) {
        uint8_t b = data[1 + i];
        int arg = b >> 3;
        fa_stream* v[16];
        uint32_t cnt = 0;
        switch (b & 7) {
        case 0: (void)fa_sound_play(snd, g_dev, NULL, (float)arg / 16.0f, (float)(arg - 16) / 16.0f, NULL); break;
        case 1: (void)fa_sound_stop(snd); break;
        case 2: {
            fa_sound_options o;
            fa_sound_options_init(&o);
            o.max_voices = (uint32_t)arg + 1;
            o.steal = arg % 3;
            (void)fa_sound_set_options(snd, &o);
            break;
        }
        case 3: (void)fa_sound_get_voices(snd, v, 16, &cnt); break;
        case 4:
            if (fa_sound_get_voices(snd, v, 16, &cnt) == FA_OK && cnt > 0) fa_stream_close(v[0]);
            break;
        case 5: {
            float out[512 * 2];
            int k;
            for (k = 0; k <= arg % 8; k++) {
                fa_fetch_add64(&g_dev->epoch, 1);
                fa_device_render(g_dev, out, 512);
                fa_fetch_add64(&g_dev->epoch, 1);
            }
            break;
        }
        case 6:
            if (fa_sound_get_voices(snd, v, 16, &cnt) == FA_OK && cnt > 0) (void)fa_stream_detach(v[0]);
            break;
        default: {
            fa_sound_info info = { .struct_size = sizeof(info) };
            (void)fa_sound_get_info(snd, &info);
            break;
        }
        }
    }
    fa_sound_close(snd);
    return 0;
}
