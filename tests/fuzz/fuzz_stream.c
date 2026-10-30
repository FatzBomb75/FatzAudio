/* Fuzz target: any audio or container file opened from memory and decoded. Covers the
 * AAC / HE-AAC (ADTS, MP4, CAF, Matroska), MP3, FLAC (native, Ogg, MKV, MP4), MP4,
 * Matroska, CAF, ASF, Ogg, WAV/AIFF, Dolby and tracker/MIDI paths, which all start in the same
 * place. MIDI renders with a tiny built-in soundfont (one looped sine, a melodic and a drum
 * preset), so notes, envelopes and effects run without an OS bank.
 * Fails (abort) on a sanitizer report, a hang, or decoded samples that are not finite. */
#include "fa_internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static fa_engine* g_engine;

/* ---- the built-in soundfont ---------------------------------------------- */

#define SF_LEN 441   /* 10 periods of 1 kHz at 44.1 kHz: loops seamlessly */

typedef struct { uint8_t* d; size_t n, cap; } sfbuf;

static void put(sfbuf* b, const void* p, size_t n)
{
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2;
        b->d = (uint8_t*)realloc(b->d, b->cap);
        if (!b->d) abort();
    }
    memcpy(b->d + b->n, p, n);
    b->n += n;
}
static void put16(sfbuf* b, unsigned v) { uint8_t x[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; put(b, x, 2); }
static void put32(sfbuf* b, uint32_t v) { put16(b, v & 0xFFFF); put16(b, v >> 16); }
static void putname(sfbuf* b, const char* s, size_t n) { char z[20] = { 0 }; memcpy(z, s, strlen(s)); put(b, z, n); }
static void chunk(sfbuf* b, const char* id, const sfbuf* body) { put(b, id, 4); put32(b, (uint32_t)body->n); put(b, body->d, body->n); }

static fa_soundfont* make_soundfont(void)
{
    sfbuf sf = { 0 }, riff = { 0 }, list = { 0 }, c = { 0 };
    fa_soundfont* font = NULL;
    int i;
    put(&riff, "sfbk", 4);
    put(&list, "INFO", 4);
    put16(&c, 2); put16(&c, 1); chunk(&list, "ifil", &c);
    chunk(&riff, "LIST", &list);
    list.n = 0; c.n = 0;
    put(&list, "sdta", 4);
    for (i = 0; i < SF_LEN; i++) put16(&c, (unsigned)(uint16_t)(int16_t)lrint(sin(6.283185307179586 * i / 44.1) * 20000.0));
    for (i = 0; i < 46; i++) put16(&c, 0);
    chunk(&list, "smpl", &c);
    chunk(&riff, "LIST", &list);
    list.n = 0;
    put(&list, "pdta", 4);
    /* presets: 0:0 (melodic) and 128:0 (drums), both on instrument 0 */
    c.n = 0;
    putname(&c, "Sine", 20); put16(&c, 0); put16(&c, 0); put16(&c, 0); put32(&c, 0); put32(&c, 0); put32(&c, 0);
    putname(&c, "Kit", 20); put16(&c, 0); put16(&c, 128); put16(&c, 1); put32(&c, 0); put32(&c, 0); put32(&c, 0);
    putname(&c, "EOP", 20); put16(&c, 0); put16(&c, 0); put16(&c, 2); put32(&c, 0); put32(&c, 0); put32(&c, 0);
    chunk(&list, "phdr", &c);
    c.n = 0; put16(&c, 0); put16(&c, 0); put16(&c, 1); put16(&c, 0); put16(&c, 2); put16(&c, 0);
    chunk(&list, "pbag", &c);
    c.n = 0; put32(&c, 0); put32(&c, 0); put16(&c, 0);
    chunk(&list, "pmod", &c);
    c.n = 0; put16(&c, 41); put16(&c, 0); put16(&c, 41); put16(&c, 0); put16(&c, 0); put16(&c, 0);
    chunk(&list, "pgen", &c);
    c.n = 0; putname(&c, "Inst", 20); put16(&c, 0); putname(&c, "EOI", 20); put16(&c, 1);
    chunk(&list, "inst", &c);
    c.n = 0; put16(&c, 0); put16(&c, 0); put16(&c, 3); put16(&c, 0);
    chunk(&list, "ibag", &c);
    c.n = 0; put32(&c, 0); put32(&c, 0); put16(&c, 0);
    chunk(&list, "imod", &c);
    /* sampleModes loop, releaseVolEnv 2^-3 s, sampleID 0, terminator */
    c.n = 0; put16(&c, 54); put16(&c, 1); put16(&c, 38); put16(&c, (uint16_t)-3600); put16(&c, 53); put16(&c, 0);
    put16(&c, 0); put16(&c, 0);
    chunk(&list, "igen", &c);
    c.n = 0;
    putname(&c, "Sine", 20); put32(&c, 0); put32(&c, SF_LEN); put32(&c, 0); put32(&c, SF_LEN);
    put32(&c, 44100); put(&c, "\x45\x00", 2); put16(&c, 0); put16(&c, 1);
    putname(&c, "EOS", 20); for (i = 0; i < 26; i++) put(&c, "", 1);
    chunk(&list, "shdr", &c);
    chunk(&riff, "LIST", &list);
    chunk(&sf, "RIFF", &riff);
    if (fa_soundfont_open_memory(sf.d, sf.n, &font) != FA_OK) abort();
    free(sf.d); free(riff.d); free(list.d); free(c.d);
    return font;
}

static void exercise(const uint8_t* data, size_t size, int32_t track)
{
    fa_open_options o;
    fa_stream* s = NULL;
    fa_stream_info info;
    static float buf[32768];   /* any channel count up to 32768 */
    int k;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    o.audio_track = track;
    if (fa_stream_open_memory(g_engine, data, size, &o, &s) != FA_OK) return;
    fa_stream_get_info(s, &info);
    for (k = 0; k < 24; k++) {
        fa_frames got = 0, i;
        uint32_t ch = info.format.channels ? info.format.channels : 1;
        if (fa_stream_read(s, buf, ch < 4096 ? 4096 / ch : 1, &got) != FA_OK || got == 0) break;
        for (i = 0; i < got * ch; i++)
            if (!isfinite(buf[i])) { fprintf(stderr, "non-finite sample %zu of block %d\n", (size_t)i, k); abort(); }
    }
    if (info.seekable && info.length_frames > 0) {
        fa_frames got, n = 32768 / (info.format.channels ? info.format.channels : 1);
        if (n > 1000) n = 1000;
        fa_stream_seek(s, info.length_frames / 2);
        fa_stream_read(s, buf, n, &got);
        fa_stream_seek(s, info.length_frames / 7);
        fa_stream_read(s, buf, n, &got);
    }
    fa_stream_close(s);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (!g_engine) {
        fa_engine_config cfg;
        fa_soundfont* font;
        fa_engine_config_init(&cfg);
        if (fa_engine_create(&cfg, &g_engine) != FA_OK) abort();
        font = make_soundfont();
        if (fa_engine_set_soundfont(g_engine, font) != FA_OK) abort();
        fa_soundfont_close(font);
    }
    exercise(data, size, -1);
    exercise(data, size, 1);
    return 0;
}
