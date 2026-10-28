/* Fuzz target: any audio or container file opened from memory and decoded. Covers the
 * AAC / HE-AAC (ADTS, MP4, CAF, Matroska), MP3, FLAC (native, Ogg, MKV, MP4), MP4,
 * Matroska, CAF, ASF, Ogg and tracker/MIDI paths, which all start in the same place.
 * Fails (abort) on a sanitizer report, a hang, or decoded samples that are not finite. */
#include "fa_internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static fa_engine* g_engine;

static void exercise(const uint8_t* data, size_t size, int32_t track)
{
    fa_open_options o;
    fa_stream* s = NULL;
    fa_stream_info info;
    static float buf[4096 * 8];
    int k;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    o.audio_track = track;
    if (fa_stream_open_memory(g_engine, data, size, &o, &s) != FA_OK) return;
    fa_stream_get_info(s, &info);
    for (k = 0; k < 24; k++) {
        fa_frames got = 0, i;
        uint32_t ch = info.format.channels ? info.format.channels : 1;
        if (ch > 8) ch = 8;
        if (fa_stream_read(s, buf, 4096 / ch, &got) != FA_OK || got == 0) break;
        for (i = 0; i < got * ch; i++)
            if (!isfinite(buf[i])) { fprintf(stderr, "non-finite sample %zu of block %d\n", (size_t)i, k); abort(); }
    }
    if (info.seekable && info.length_frames > 0) {
        fa_frames got;
        fa_stream_seek(s, info.length_frames / 2);
        fa_stream_read(s, buf, 1000, &got);
        fa_stream_seek(s, info.length_frames / 7);
        fa_stream_read(s, buf, 1000, &got);
    }
    fa_stream_close(s);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (!g_engine) {
        fa_engine_config cfg;
        fa_engine_config_init(&cfg);
        if (fa_engine_create(&cfg, &g_engine) != FA_OK) abort();
    }
    exercise(data, size, -1);
    exercise(data, size, 1);
    return 0;
}
