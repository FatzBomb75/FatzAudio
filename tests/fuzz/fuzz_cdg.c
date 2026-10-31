/* Fuzz target: CD+G. The first 4 bytes pick the surface options, the rest are raw
 * 24-byte packs: loaded as a stream's graphics (fa_stream_load_cdg_memory) and drawn
 * at a few times, forward and back, through fa_cdg_surface_update_at and
 * fa_surface_update_into, with presentation scaling, the traced-outline smoothness
 * levels and the surface effects as the options say. The same bytes then go in as a
 * disc's raw subcode (disc_cdg.c: layouts, interleave, parity) on a second stream. */
#include "fa_internal.h"
#include "disc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECONDS 30
#define RATE 8000

static fa_engine* g_engine;
static uint8_t* g_wav;
static size_t g_wav_size;

static void le32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* 30 s of silence, 8 kHz mono 16-bit: the audio the graphics are timed against. */
static void make_wav(void)
{
    uint32_t data = SECONDS * RATE * 2;
    g_wav_size = 44 + data;
    g_wav = (uint8_t*)calloc(1, g_wav_size);
    if (!g_wav) abort();
    memcpy(g_wav, "RIFF", 4); le32(g_wav + 4, (uint32_t)g_wav_size - 8); memcpy(g_wav + 8, "WAVEfmt ", 8);
    le32(g_wav + 16, 16); g_wav[20] = 1; g_wav[22] = 1; le32(g_wav + 24, RATE); le32(g_wav + 28, RATE * 2);
    g_wav[32] = 2; g_wav[34] = 16; memcpy(g_wav + 36, "data", 4); le32(g_wav + 40, data);
}

static fa_stream* open_stream(void)
{
    fa_open_options o;
    fa_stream* s = NULL;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    if (fa_stream_open_memory(g_engine, g_wav, g_wav_size, &o, &s) != FA_OK) abort();
    return s;
}

static void draw(fa_surface* f, const uint8_t* h, fa_time last, size_t draws)
{
    static const fa_time when[] = { 3333334, 1000000000, -1, 400000000, -2 };
    static uint8_t buf[4 << 20];
    fa_surface_layout lay;
    fa_surface_frame fr;
    fa_cdg_frame cf;
    uint64_t rev = 0;
    size_t i;
    for (i = 0; i < draws && i < sizeof(when) / sizeof(when[0]); i++) {
        fa_time t = when[i] == -1 ? last : when[i] == -2 ? last / 2 : when[i];
        if (h[1] & 0x80) {
            memset(&lay, 0, sizeof(lay));
            lay.struct_size = sizeof(lay);
            if (fa_surface_get_layout(f, &lay) != FA_OK) return;
            if ((size_t)lay.width * 4 * lay.height > sizeof(buf)) return;
            memset(&fr, 0, sizeof(fr));
            fr.struct_size = sizeof(fr);
            if (fa_surface_update_into(f, t, buf, (int32_t)(lay.width * 4), lay.width, lay.height, rev, &fr) == FA_OK)
                rev = fr.revision;
        } else {
            (void)fa_cdg_surface_update_at(f, t, &cf);
        }
    }
}

static void surfaces(fa_stream* s, const uint8_t* h, fa_time last)
{
    fa_cdg_surface_config cfg;
    fa_cdg_presentation p;
    fa_surface_effects fx;
    fa_surface* f = NULL;
    fa_cdg_surface_config_init(&cfg);
    cfg.flags = h[0];
    cfg.pixel_format = (h[1] & 3) == 3 ? FA_CDG_PIXELS_BGRA32 : (fa_cdg_pixel_format)(h[1] & 3);
    if (h[1] & 0x40) { cfg.width = 96u + h[3] * 3u; cfg.height = 64u + h[2] * 2u; }
    cfg.background_color = 0x80204060u;
    cfg.background_opacity = h[3] % 101u;
    if (fa_cdg_surface_create(s, &cfg, &f) != FA_OK) return;
    if (h[1] & 4) {
        fa_cdg_presentation_init(&p);
        p.scale = 1;
        p.safety_zone = (fa_safety_zone)((h[1] >> 3) & 3);
        p.smoothness = h[2] % 11;
        p.edge_blend = (h[2] >> 4) & 1;
        p.padding = h[3] % 33u;
        p.flags = (h[2] >> 5) & 1;
        p.auto_ignore_seconds = 0.5f;
        if (fa_cdg_surface_set_presentation(f, &p) != FA_OK) abort();
        fa_surface_set_presentation_size(f, 300u + h[3] * 2u, 216u + h[2]);   /* 1x to 2x: speed */
    }
    if (h[1] & 0x20) {
        fa_surface_effects_init(&fx);
        fx.outline.on = 1;
        fx.shadow.on = (h[2] >> 6) & 1;
        fx.bevel.on = h[2] >> 7;
        fx.outline.size = (float)(h[3] & 15) / 15.0f;
        fx.shadow.size = (float)(h[3] >> 4) / 15.0f;
        fx.shadow.angle_degrees = (float)h[2] * 1.4f;
        if (fa_surface_set_effects(f, &fx) != FA_OK) abort();
    }
    draw(f, h, last, (h[1] & 0x20) ? 2 : 5);   /* effects are slow under the sanitizers */
    fa_surface_destroy(f);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    fa_stream* s;
    fa_disc_cdg* c;
    uint8_t h[4] = { 0 };
    size_t n = size;
    if (!g_engine) {
        fa_engine_config cfg;
        fa_engine_config_init(&cfg);
        if (fa_engine_create(&cfg, &g_engine) != FA_OK) abort();
        make_wav();
    }
    memcpy(h, data, size < 4 ? size : 4);
    data += size < 4 ? size : 4;
    n = size < 4 ? 0 : size - 4;
    s = open_stream();
    if (fa_stream_load_cdg_memory(s, data, n) == FA_OK) surfaces(s, h, (fa_time)(n / 24) * 10000000 / 3);
    fa_stream_close(s);

    /* As a disc track's subcode: 96 bytes a sector, fed as a drive read returns them. */
    s = open_stream();
    c = fa_disc_cdg_create(s, 150, SECONDS * 75);
    if (c) {
        int64_t lba = 150;
        size_t at;
        for (at = 0; at + FA_CD_SUB_BYTES <= n; ) {
            uint32_t count = (uint32_t)((n - at) / FA_CD_SUB_BYTES);
            if (count > 8) count = 8;
            fa_disc_cdg_feed(c, lba, count, data + at);
            lba += count;
            at += (size_t)count * FA_CD_SUB_BYTES;
        }
        if (fa_disc_cdg_wanted(c)) {
            uint8_t plain[4] = { 0, (uint8_t)(h[1] & 0x83), 0, 0 };   /* the decoding is what is new here */
            surfaces(s, plain, (lba - 150) * 4 * 10000000 / 3);
        }
        fa_disc_cdg_free(c);
    }
    fa_stream_close(s);
    return 0;
}
