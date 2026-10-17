#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L /* nanosleep under strict C11 */
#endif
/* CD+G tests: the pack decoder, surfaces (formats, scaling, borders, transparency,
 * dirty rectangles), timing and the audio clock, MP3+G style pairs, ZIP archives
 * and the DEFLATE decoder behind them. Every input is generated here. */
#include "cdg.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; return; } } while (0)
#define CHECK_EQ(a, b) do { long long va_ = (long long)(a), vb_ = (long long)(b); if (va_ != vb_) { \
    fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_failures++; return; } } while (0)
#define RUN(t) do { int before_ = g_failures; printf("%s\n", #t); t(); if (g_failures == before_) printf("  ok\n"); } while (0)

static fa_engine* g_engine;

/* ---- generated media ---------------------------------------------------------- */

typedef struct buf { uint8_t* p; size_t n, cap; } buf;
static void put(buf* b, const void* d, size_t n)
{
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 64; b->p = (uint8_t*)realloc(b->p, b->cap); }
    if (n) memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void put8(buf* b, uint32_t v) { uint8_t c = (uint8_t)v; put(b, &c, 1); }
static void le16(buf* b, uint32_t v) { put8(b, v); put8(b, v >> 8); }
static void le32(buf* b, uint32_t v) { le16(b, v); le16(b, v >> 16); }

static buf make_wav(uint32_t rate, uint32_t frames)
{
    buf b = { 0 };
    uint32_t i, data = frames * 2;
    put(&b, "RIFF", 4); le32(&b, 36 + data); put(&b, "WAVE", 4);
    put(&b, "fmt ", 4); le32(&b, 16); le16(&b, 1); le16(&b, 1);
    le32(&b, rate); le32(&b, rate * 2); le16(&b, 2); le16(&b, 16);
    put(&b, "data", 4); le32(&b, data);
    for (i = 0; i < frames; i++) le16(&b, (uint16_t)(int16_t)(sin(i * 0.05) * 8000.0));
    return b;
}

static void write_file(const char* path, const buf* b)
{
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fwrite(b->p, 1, b->n, f);
    fclose(f);
}

/* One CD+G pack (mode 9, TV graphics). */
static void pack(buf* b, int instruction, const uint8_t data[16])
{
    uint8_t p[FA_CDG_PACK_BYTES] = { 0 };
    p[0] = 9;
    p[1] = (uint8_t)instruction;
    memcpy(p + 4, data, 16);
    put(b, p, sizeof(p));
}
static void empty_packs_to(buf* b, size_t count)
{
    static const uint8_t zero[FA_CDG_PACK_BYTES];
    while (b->n / FA_CDG_PACK_BYTES < count) put(b, zero, sizeof(zero));
}
static void colors(buf* b, int high, const uint8_t rgb4[8][3])
{
    uint8_t d[16];
    int i;
    for (i = 0; i < 8; i++) {
        d[2 * i] = (uint8_t)((rgb4[i][0] << 2) | (rgb4[i][1] >> 2));
        d[2 * i + 1] = (uint8_t)(((rgb4[i][1] & 3) << 4) | rgb4[i][2]);
    }
    pack(b, high ? 31 : 30, d);
}
static void preset(buf* b, int color, int repeat)
{
    uint8_t d[16] = { 0 };
    d[0] = (uint8_t)color; d[1] = (uint8_t)repeat;
    pack(b, 1, d);
}
static void border(buf* b, int color)
{
    uint8_t d[16] = { 0 };
    d[0] = (uint8_t)color;
    pack(b, 2, d);
}
static void tile(buf* b, int xor_mode, int c0, int c1, int row, int col, uint8_t bits)
{
    uint8_t d[16];
    int i;
    d[0] = (uint8_t)c0; d[1] = (uint8_t)c1; d[2] = (uint8_t)row; d[3] = (uint8_t)col;
    for (i = 0; i < 12; i++) d[4 + i] = bits;
    pack(b, xor_mode ? 38 : 6, d);
}
static void scroll(buf* b, int copy, int color, int hcmd, int hoff, int vcmd, int voff)
{
    uint8_t d[16] = { 0 };
    d[0] = (uint8_t)color;
    d[1] = (uint8_t)((hcmd << 4) | hoff);
    d[2] = (uint8_t)((vcmd << 4) | voff);
    pack(b, copy ? 24 : 20, d);
}

/* Palette: 0 black, 1 red, 2 green, 3 blue, 4 white, 5 grey. */
static const uint8_t k_low[8][3] = { { 0, 0, 0 }, { 15, 0, 0 }, { 0, 15, 0 }, { 0, 0, 15 },
                                     { 15, 15, 15 }, { 8, 8, 8 }, { 0, 0, 0 }, { 0, 0, 0 } };

/* Time at which the first n packs are shown. */
static fa_time t_packs(int64_t n) { return n * 10000000 / 3; }

static fa_stream* open_wav_stream(uint32_t seconds, uint32_t flags)
{
    buf w = make_wav(8000, 8000 * seconds);
    fa_open_options o;
    fa_stream* s = NULL;
    fa_open_options_init(&o);
    o.flags = flags;
    fa_stream_open_memory(g_engine, w.p, w.n, &o, &s);
    free(w.p);
    return s;
}

static const uint8_t* px(const fa_cdg_frame* f, uint32_t x, uint32_t y)
{
    uint32_t bpp = f->pixel_format == FA_CDG_PIXELS_INDEXED8 ? 1 : 4;
    return (const uint8_t*)f->pixels + (size_t)y * (size_t)f->stride + x * bpp;
}

/* BGRA pixel equals palette colour (r, g, b) at 0..15 per channel, opaque. */
static int is_bgra(const uint8_t* p, int r, int g, int b)
{
    return p[0] == b * 17 && p[1] == g * 17 && p[2] == r * 17 && p[3] == 255;
}

/* ---- DEFLATE and ZIP ----------------------------------------------------------------- */

static void test_inflate_vectors(void)
{
    /* "Hello CD+G", fixed Huffman codes (zlib, raw deflate) */
    static const uint8_t fixed[] = { 0xf3, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0x70, 0x76, 0xd1, 0x76, 0x07, 0x00 };
    /* 40 lines "Line %d: sing along with the bouncing ball " + "oh " * (i % 5), dynamic codes */
    static const uint8_t dynamic[] = {
        0x9d, 0xd4, 0x3d, 0x0a, 0xc3, 0x30, 0x14, 0x03, 0xe0, 0xbd, 0xa7, 0x78, 0x47, 0xe8, 0x7b, 0xea,
        0xff, 0x19, 0x72, 0x89, 0xa4, 0x84, 0xda, 0x60, 0xec, 0xa1, 0x29, 0xbd, 0x7e, 0x1b, 0x08, 0x99,
        0x2d, 0x2d, 0x1e, 0x8c, 0x34, 0x88, 0x0f, 0x7b, 0xc8, 0x75, 0xb6, 0xe3, 0xc3, 0xde, 0xb9, 0xbe,
        0x6c, 0x2c, 0xed, 0x7f, 0x7e, 0xf3, 0x92, 0x6c, 0x49, 0xb3, 0x4d, 0xed, 0x53, 0x9f, 0xeb, 0xfd,
        0x34, 0x96, 0x62, 0x87, 0x61, 0x8d, 0x7a, 0x4f, 0xb4, 0xa5, 0x2d, 0x1d, 0x9d, 0xe9, 0xbd, 0x80,
        0xfe, 0xc2, 0xde, 0x39, 0x51, 0x9d, 0xbd, 0x76, 0xee, 0x1f, 0x7d, 0xa1, 0x46, 0x5f, 0xd9, 0xd1,
        0x37, 0x61, 0xf4, 0x5d, 0x1b, 0xed, 0x0c, 0x35, 0x67, 0xed, 0x34, 0xb6, 0x2b, 0xda, 0x2e, 0x72,
        0x3b, 0xe1, 0xed, 0x1c, 0xb8, 0xd3, 0xe2, 0xae, 0x90, 0xbb, 0x68, 0x1e, 0x84, 0x79, 0x90, 0xef,
        0x9b, 0x36, 0x0f, 0xc5, 0x3c, 0x44, 0xf3, 0x20, 0xcc, 0x83, 0x33, 0x0f, 0xda, 0x3c, 0x14, 0xf3,
        0x10, 0xcd, 0x41, 0x98, 0x83, 0x33, 0x07, 0xff, 0xa9, 0x2b, 0xe6, 0x10, 0xcd, 0x41, 0x98, 0x83,
        0x33, 0x07, 0x6d, 0x0e, 0xc5, 0x1c, 0xbc, 0xf9, 0x0f };
    uint8_t out[2048];
    buf expect = { 0 };
    char line[128];
    int i, j;
    CHECK_EQ(fa_inflate(fixed, sizeof(fixed), out, 10), FA_OK);
    CHECK(memcmp(out, "Hello CD+G", 10) == 0);
    CHECK_EQ(fa_crc32(out, 10), 0xebd811b2u);
    for (i = 0; i < 40; i++) {
        snprintf(line, sizeof(line), "Line %d: sing along with the bouncing ball ", i);
        put(&expect, line, strlen(line));
        for (j = 0; j < i % 5; j++) put(&expect, "oh ", 3);
        put(&expect, "\n", 1);
    }
    CHECK_EQ(expect.n, 1990);
    CHECK_EQ(fa_inflate(dynamic, sizeof(dynamic), out, expect.n), FA_OK);
    CHECK(memcmp(out, expect.p, expect.n) == 0);
    CHECK_EQ(fa_crc32(out, expect.n), 0x8370438eu);
    /* Damaged or short input fails cleanly; a wrong size is an error too. */
    CHECK(fa_inflate(dynamic, sizeof(dynamic) / 2, out, expect.n) != FA_OK);
    CHECK(fa_inflate(dynamic, sizeof(dynamic), out, expect.n - 1) != FA_OK);
    CHECK(fa_inflate(dynamic, sizeof(dynamic), out, expect.n + 1) != FA_OK);
    free(expect.p);
}

typedef struct zip_file { const char* name; const buf* data; int deflate; uint32_t crc_xor; } zip_file;

/* "Deflate" with stored blocks: a valid method-8 entry without a compressor. */
static buf stored_deflate(const buf* d)
{
    buf z = { 0 };
    size_t pos = 0;
    do {
        size_t n = d->n - pos > 65535 ? 65535 : d->n - pos;
        put8(&z, pos + n == d->n ? 1 : 0);
        le16(&z, (uint32_t)n);
        le16(&z, (uint32_t)~n & 0xFFFF);
        put(&z, d->p + pos, n);
        pos += n;
    } while (pos < d->n);
    return z;
}

static buf make_zip(const zip_file* files, int count)
{
    buf z = { 0 }, cd = { 0 };
    uint32_t offsets[8];
    int i;
    for (i = 0; i < count; i++) {
        buf body = files[i].deflate ? stored_deflate(files[i].data) : *files[i].data;
        uint32_t crc = fa_crc32(files[i].data->p, files[i].data->n) ^ files[i].crc_xor;
        uint32_t name_len = (uint32_t)strlen(files[i].name);
        offsets[i] = (uint32_t)z.n;
        le32(&z, 0x04034b50); le16(&z, 20); le16(&z, 0); le16(&z, files[i].deflate ? 8 : 0);
        le16(&z, 0); le16(&z, 0); le32(&z, crc); le32(&z, (uint32_t)body.n); le32(&z, (uint32_t)files[i].data->n);
        le16(&z, name_len); le16(&z, 0);
        put(&z, files[i].name, name_len);
        put(&z, body.p, body.n);
        le32(&cd, 0x02014b50); le16(&cd, 20); le16(&cd, 20); le16(&cd, 0); le16(&cd, files[i].deflate ? 8 : 0);
        le16(&cd, 0); le16(&cd, 0); le32(&cd, crc); le32(&cd, (uint32_t)body.n); le32(&cd, (uint32_t)files[i].data->n);
        le16(&cd, name_len); le16(&cd, 0); le16(&cd, 0); le16(&cd, 0); le16(&cd, 0); le32(&cd, 0);
        le32(&cd, offsets[i]);
        put(&cd, files[i].name, name_len);
        if (files[i].deflate) free(body.p);
    }
    {
        uint32_t cd_offset = (uint32_t)z.n;
        put(&z, cd.p, cd.n);
        le32(&z, 0x06054b50); le16(&z, 0); le16(&z, 0); le16(&z, (uint32_t)count); le16(&z, (uint32_t)count);
        le32(&z, (uint32_t)cd.n); le32(&z, cd_offset); le16(&z, 0);
    }
    free(cd.p);
    return z;
}

static buf small_cdg(void)
{
    buf c = { 0 };
    colors(&c, 0, k_low);
    preset(&c, 1, 0);
    return c;
}

static void test_zip_pair(void)
{
    buf wav = make_wav(8000, 8000), cdg = small_cdg(), text = { 0 }, zip;
    zip_file files[3];
    fa_stream* s = NULL;
    fa_stream_info info;
    fa_cdg_surface* surf = NULL;
    fa_cdg_frame fr;
    fa_frames got = 0;
    float pcm[256];
    fa_open_options o;
    put(&text, "lyrics by someone", 17);
    files[0].name = "readme.txt"; files[0].data = &text; files[0].deflate = 0; files[0].crc_xor = 0;
    files[1].name = "Song Title.cdg"; files[1].data = &cdg; files[1].deflate = 1; files[1].crc_xor = 0;
    files[2].name = "Song Title.wav"; files[2].data = &wav; files[2].deflate = 1; files[2].crc_xor = 0;
    zip = make_zip(files, 3);
    write_file("cdg_pair.zip", &zip);

    /* From a path */
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    CHECK_EQ(fa_stream_open_path(g_engine, "cdg_pair.zip", &o, &s), FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK_EQ(info.has_cdg, 1);
    CHECK_EQ(info.format.sample_rate, 8000);
    CHECK_EQ(info.length_frames, 8000);
    CHECK_EQ(fa_stream_read(s, pcm, 256, &got), FA_OK);
    CHECK(got > 0);
    CHECK_EQ(fa_cdg_surface_create(s, NULL, &surf), FA_OK);
    CHECK_EQ(fa_cdg_surface_update_at(surf, t_packs(2), &fr), FA_OK);
    CHECK(is_bgra(px(&fr, 150, 100), 15, 0, 0));
    fa_stream_close(s);
    /* The surface outlives its stream until destroyed, but no longer updates */
    CHECK_EQ(fa_cdg_surface_update_at(surf, t_packs(2), &fr), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_cdg_surface_update(surf, &fr), FA_ERROR_INVALID_ARG);
    fa_cdg_surface_destroy(surf);

    /* From memory */
    s = NULL;
    CHECK_EQ(fa_stream_open_memory(g_engine, zip.p, zip.n, &o, &s), FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK_EQ(info.has_cdg, 1);
    CHECK_EQ(info.length_frames, 8000);
    fa_stream_close(s);

    /* FA_OPEN_NO_CDG keeps the audio and drops the graphics */
    o.flags = FA_OPEN_DECODE_ONLY | FA_OPEN_NO_CDG;
    CHECK_EQ(fa_stream_open_memory(g_engine, zip.p, zip.n, &o, &s), FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK_EQ(info.has_cdg, 0);
    fa_stream_close(s);
    free(zip.p);

    /* A damaged .cdg entry (bad CRC) leaves a playable song without graphics */
    files[1].crc_xor = 1;
    zip = make_zip(files, 3);
    o.flags = FA_OPEN_DECODE_ONLY;
    CHECK_EQ(fa_stream_open_memory(g_engine, zip.p, zip.n, &o, &s), FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK_EQ(info.has_cdg, 0);
    CHECK_EQ(info.length_frames, 8000);
    fa_stream_close(s);
    free(zip.p);

    /* No audio inside: unsupported */
    zip = make_zip(files, 2);
    CHECK_EQ(fa_stream_open_memory(g_engine, zip.p, zip.n, &o, &s), FA_ERROR_UNSUPPORTED_FORMAT);
    CHECK(s == NULL);
    free(zip.p);
    free(wav.p); free(cdg.p); free(text.p);
}

static void test_zip_damaged(void)
{
    buf wav = make_wav(8000, 800), zip;
    zip_file f;
    fa_stream* s = NULL;
    size_t cut;
    f.name = "a.wav"; f.data = &wav; f.deflate = 0; f.crc_xor = 0;
    zip = make_zip(&f, 1);
    /* Every truncation fails cleanly (ASan/UBSan watch the parser). */
    for (cut = 4; cut < zip.n; cut += 7) {
        fa_result r = fa_stream_open_memory(g_engine, zip.p, cut, NULL, &s);
        if (r == FA_OK) { fa_stream_close(s); s = NULL; }
    }
    free(zip.p); free(wav.p);
}

static void test_zip_edge_cases(void)
{
    buf wav = make_wav(8000, 800), cdg = small_cdg(), zip;
    zip_file files[3];
    fa_open_options o;
    fa_stream* s = NULL;
    files[0].name = "song.cdg"; files[0].data = &cdg; files[0].deflate = 0; files[0].crc_xor = 0;
    files[1].name = "song.wav"; files[1].data = &wav; files[1].deflate = 0; files[1].crc_xor = 0;
    files[2].name = "other.wav"; files[2].data = &wav; files[2].deflate = 0; files[2].crc_xor = 0;
    zip = make_zip(files, 3);
    /* An audio track the file does not have: an error, and nothing leaks (ASan) */
    fa_open_options_init(&o);
    o.audio_track = 1;
    CHECK_EQ(fa_stream_open_memory(g_engine, zip.p, zip.n, &o, &s), FA_ERROR_NOT_FOUND);
    CHECK(s == NULL);
    free(zip.p);

    /* A deflate entry that claims to unpack to 3 GB is refused before allocating */
    {
        fa_zip_entry e;
        uint8_t* out = NULL;
        buf body = stored_deflate(&wav);
        memset(&e, 0, sizeof(e));
        e.method = 8;
        e.compressed_size = 100;
        e.size = 3000000000u;
        files[0].name = "a.wav"; files[0].data = &wav; files[0].deflate = 1;
        zip = make_zip(files, 1);
        e.header_offset = 0;
        CHECK_EQ(fa_zip_extract(zip.p, zip.n, &e, &out), FA_ERROR_UNSUPPORTED_FORMAT);
        CHECK(out == NULL);
        free(zip.p);
        free(body.p);
    }
    free(wav.p); free(cdg.p);
}

/* ---- the decoder and surfaces ---------------------------------------------------------- */

static buf basic_cdg(void)
{
    buf c = { 0 };
    colors(&c, 0, k_low);      /* pack 0 */
    preset(&c, 1, 0);          /* 1: red screen */
    border(&c, 2);             /* 2: green border */
    tile(&c, 0, 3, 4, 1, 1, 0x20); /* 3: row 1, col 1: leftmost column white, rest blue */
    return c;
}

static void test_decoder_basics(void)
{
    buf c = basic_cdg();
    fa_stream* s = open_wav_stream(1, FA_OPEN_DECODE_ONLY);
    fa_cdg_surface* surf = NULL;
    fa_cdg_frame fr;
    CHECK(s);
    CHECK_EQ(fa_stream_load_cdg_memory(s, c.p, c.n), FA_OK);
    CHECK_EQ(fa_cdg_surface_create(s, NULL, &surf), FA_OK);

    CHECK_EQ(fa_cdg_surface_update_at(surf, t_packs(3), &fr), FA_OK);
    CHECK_EQ(fr.width, 300); CHECK_EQ(fr.height, 216); CHECK_EQ(fr.stride, 1200);
    CHECK_EQ(fr.pixel_format, FA_CDG_PIXELS_BGRA32);
    CHECK_EQ(fr.changed, 1);
    CHECK_EQ(fr.dirty_width, 300); CHECK_EQ(fr.dirty_height, 216);
    CHECK(is_bgra(px(&fr, 0, 0), 0, 15, 0));      /* border */
    CHECK(is_bgra(px(&fr, 299, 215), 0, 15, 0));
    CHECK(is_bgra(px(&fr, 6, 12), 15, 0, 0));     /* picture, before the tile */
    CHECK_EQ(fr.palette[1][0], 255); CHECK_EQ(fr.palette[1][3], 255);

    /* One more pack: only the tile changed */
    CHECK_EQ(fa_cdg_surface_update_at(surf, t_packs(4), &fr), FA_OK);
    CHECK_EQ(fr.changed, 1);
    CHECK_EQ(fr.dirty_x, 6); CHECK_EQ(fr.dirty_y, 12); CHECK_EQ(fr.dirty_width, 6); CHECK_EQ(fr.dirty_height, 12);
    CHECK(is_bgra(px(&fr, 6, 12), 15, 15, 15));
    CHECK(is_bgra(px(&fr, 7, 12), 0, 0, 15));
    CHECK(is_bgra(px(&fr, 11, 23), 0, 0, 15));
    CHECK(is_bgra(px(&fr, 12, 23), 15, 0, 0));
    CHECK_EQ(fr.time, t_packs(4));

    /* Same time again: nothing to upload */
    CHECK_EQ(fa_cdg_surface_update_at(surf, t_packs(4) + 1000, &fr), FA_OK);
    CHECK_EQ(fr.changed, 0);
    CHECK_EQ(fr.dirty_width, 0);

    /* Back to the start: replayed, black (nothing loaded yet at time 0) */
    CHECK_EQ(fa_cdg_surface_update_at(surf, 0, &fr), FA_OK);
    CHECK_EQ(fr.changed, 1);
    CHECK(is_bgra(px(&fr, 6, 12), 0, 0, 0));
    /* And forward again over everything */
    CHECK_EQ(fa_cdg_surface_update_at(surf, 1000000000, &fr), FA_OK);
    CHECK(is_bgra(px(&fr, 6, 12), 15, 15, 15));
    fa_cdg_surface_destroy(surf);
    fa_stream_close(s);
    free(c.p);
}

static void test_xor_and_repeats(void)
{
    buf c = basic_cdg();
    fa_stream* s = open_wav_stream(1, FA_OPEN_DECODE_ONLY);
    fa_cdg_surface* surf = NULL;
    fa_cdg_frame fr;
    fa_cdg_surface_config cfg;
    tile(&c, 1, 0, 1, 1, 1, 0x3F); /* 4: XOR colour 1 over the whole tile: 4^1 = 5, 3^1 = 2 */
    preset(&c, 1, 1);              /* 5: a repeat of a different colour than... */
    fa_cdg_surface_config_init(&cfg);
    cfg.pixel_format = FA_CDG_PIXELS_INDEXED8;
    CHECK_EQ(fa_stream_load_cdg_memory(s, c.p, c.n), FA_OK);
    CHECK_EQ(fa_cdg_surface_create(s, &cfg, &surf), FA_OK);
    CHECK_EQ(fa_cdg_surface_update_at(surf, t_packs(5), &fr), FA_OK);
    CHECK_EQ(fr.stride, 300);
    CHECK_EQ(*px(&fr, 6, 12), 5);
    CHECK_EQ(*px(&fr, 7, 12), 2);
    CHECK_EQ(*px(&fr, 0, 0), 2);
    CHECK_EQ(fr.palette[5][0], 8 * 17);
    /* ...the screen is no longer clean, so the repeated preset clears it again */
    CHECK_EQ(fa_cdg_surface_update_at(surf, t_packs(6), &fr), FA_OK);
    CHECK_EQ(*px(&fr, 6, 12), 1);
    CHECK_EQ(*px(&fr, 0, 0), 1);
    fa_cdg_surface_destroy(surf);
    fa_stream_close(s);
    free(c.p);
}

static void test_scrolling(void)
{
    buf c = { 0 };
    fa_stream* s = open_wav_stream(1, FA_OPEN_DECODE_ONLY);
    fa_cdg_surface* surf = NULL;
    fa_cdg_frame fr;
    fa_cdg_surface_config cfg;
    colors(&c, 0, k_low);
    preset(&c, 1, 0);
    tile(&c, 0, 3, 3, 0, 0, 0);      /* 2: blue tile at the top left corner */
    scroll(&c, 1, 0, 2, 0, 0, 0);    /* 3: copy left: the tile wraps to column 49 */
    scroll(&c, 0, 4, 0, 0, 1, 0);    /* 4: preset down with white: top row of tiles white */
    scroll(&c, 0, 0, 0, 3, 0, 5);    /* 5: no move, smooth offsets 3 / 5 */
    fa_cdg_surface_config_init(&cfg);
    cfg.pixel_format = FA_CDG_PIXELS_INDEXED8;
    CHECK_EQ(fa_stream_load_cdg_memory(s, c.p, c.n), FA_OK);
    CHECK_EQ(fa_cdg_surface_create(s, &cfg, &surf), FA_OK);
    CHECK_EQ(fa_cdg_surface_update_at(surf, t_packs(4), &fr), FA_OK);
    CHECK_EQ(*px(&fr, 0, 0), 1);
    CHECK_EQ(*px(&fr, 294, 0), 3);
    CHECK_EQ(*px(&fr, 299, 11), 3);
    CHECK_EQ(*px(&fr, 293, 0), 1);
    CHECK_EQ(fa_cdg_surface_update_at(surf, t_packs(5), &fr), FA_OK);
    CHECK_EQ(*px(&fr, 100, 11), 4);
    CHECK_EQ(*px(&fr, 294, 12), 3);  /* moved down a tile */
    CHECK_EQ(*px(&fr, 294, 11), 4);
    /* Offsets shift the picture area only: window (291, 12) shows screen (294, 17) */
    CHECK_EQ(fa_cdg_surface_update_at(surf, t_packs(6), &fr), FA_OK);
    CHECK_EQ(fr.changed, 1);
    CHECK_EQ(*px(&fr, 291, 12), 3);
    CHECK_EQ(*px(&fr, 290, 12), 1);
    CHECK_EQ(*px(&fr, 294, 0), 4);   /* the border area is not shifted */
    fa_cdg_surface_destroy(surf);
    fa_stream_close(s);
    free(c.p);
}

static void test_surface_options(void)
{
    buf c = basic_cdg();
    uint8_t d[16] = { 0 };
    fa_stream* s = open_wav_stream(1, FA_OPEN_DECODE_ONLY);
    fa_cdg_surface *nb = NULL, *big = NULL, *smooth = NULL, *tr = NULL, *pm = NULL, *rgba = NULL;
    fa_cdg_surface_config cfg;
    fa_cdg_frame fr;
    const uint8_t* p;
    tile(&c, 0, 5, 5, 2, 2, 0); /* 4: a grey tile at x 12..17, y 24..35 */
    d[5] = 32;                  /* 5: colour 5 (grey) about half transparent */
    pack(&c, 28, d);
    CHECK_EQ(fa_stream_load_cdg_memory(s, c.p, c.n), FA_OK);

    fa_cdg_surface_config_init(&cfg);
    cfg.flags = FA_CDG_NO_BORDER;
    CHECK_EQ(fa_cdg_surface_create(s, &cfg, &nb), FA_OK);
    CHECK_EQ(fa_cdg_surface_update_at(nb, t_packs(4), &fr), FA_OK);
    CHECK_EQ(fr.width, 288); CHECK_EQ(fr.height, 192);
    CHECK(is_bgra(px(&fr, 0, 0), 15, 15, 15));   /* screen (6, 12) */
    CHECK(is_bgra(px(&fr, 287, 191), 15, 0, 0));

    cfg.flags = 0; cfg.width = 600; cfg.height = 432;
    CHECK_EQ(fa_cdg_surface_create(s, &cfg, &big), FA_OK);
    CHECK_EQ(fa_cdg_surface_update_at(big, t_packs(4), &fr), FA_OK);
    CHECK_EQ(fr.stride, 2400);
    CHECK(is_bgra(px(&fr, 12, 24), 15, 15, 15));
    CHECK(is_bgra(px(&fr, 13, 25), 15, 15, 15));
    CHECK(is_bgra(px(&fr, 14, 24), 0, 0, 15));
    /* Dirty rectangles are in output pixels */
    CHECK_EQ(fa_cdg_surface_update_at(big, t_packs(5), &fr), FA_OK);
    CHECK_EQ(fr.dirty_x, 24); CHECK_EQ(fr.dirty_y, 48); CHECK_EQ(fr.dirty_width, 12); CHECK_EQ(fr.dirty_height, 24);

    cfg.flags = FA_CDG_SMOOTH; cfg.width = 450; cfg.height = 324;
    CHECK_EQ(fa_cdg_surface_create(s, &cfg, &smooth), FA_OK);
    CHECK_EQ(fa_cdg_surface_update_at(smooth, t_packs(4), &fr), FA_OK);
    /* Between the white column and the blue tile: a blend, opaque */
    p = px(&fr, 10, 25);
    CHECK(p[3] == 255 && p[0] == 255 && p[2] > 0 && p[2] < 255);
    CHECK(is_bgra(px(&fr, 200, 150), 15, 0, 0));

    cfg.flags = FA_CDG_TRANSPARENT; cfg.width = cfg.height = 0;
    CHECK_EQ(fa_cdg_surface_create(s, &cfg, &tr), FA_OK);
    CHECK_EQ(fa_cdg_surface_update_at(tr, t_packs(6), &fr), FA_OK);
    CHECK_EQ(px(&fr, 100, 100)[3], 0);           /* background (red) */
    CHECK_EQ(px(&fr, 0, 0)[3], 255);             /* border green */
    CHECK_EQ(px(&fr, 12, 24)[3], 255 - (32 * 255 + 31) / 63);
    CHECK_EQ(px(&fr, 12, 24)[2], 8 * 17);        /* straight alpha: colour kept */
    CHECK_EQ(fr.palette[1][3], 0);

    cfg.flags = FA_CDG_TRANSPARENT | FA_CDG_PREMULTIPLIED;
    CHECK_EQ(fa_cdg_surface_create(s, &cfg, &pm), FA_OK);
    CHECK_EQ(fa_cdg_surface_update_at(pm, t_packs(6), &fr), FA_OK);
    p = px(&fr, 12, 24);
    CHECK_EQ(p[2], (8 * 17 * p[3] + 127) / 255);
    CHECK_EQ(px(&fr, 100, 100)[0], 0);

    cfg.flags = 0; cfg.pixel_format = FA_CDG_PIXELS_RGBA32;
    CHECK_EQ(fa_cdg_surface_create(s, &cfg, &rgba), FA_OK);
    CHECK_EQ(fa_cdg_surface_update_at(rgba, t_packs(4), &fr), FA_OK);
    p = px(&fr, 0, 0);
    CHECK(p[0] == 0 && p[1] == 255 && p[2] == 0 && p[3] == 255);
    p = px(&fr, 100, 100);
    CHECK(p[0] == 255 && p[1] == 0 && p[2] == 0);
    fa_cdg_surface_destroy(rgba);

    /* Bad configurations */
    cfg.pixel_format = (fa_cdg_pixel_format)7;
    CHECK_EQ(fa_cdg_surface_create(s, &cfg, &rgba), FA_ERROR_INVALID_ARG);
    cfg.pixel_format = FA_CDG_PIXELS_BGRA32; cfg.width = 9000;
    CHECK_EQ(fa_cdg_surface_create(s, &cfg, &rgba), FA_ERROR_INVALID_ARG);
    cfg.width = 0; cfg.flags = 1u << 9;
    CHECK_EQ(fa_cdg_surface_create(s, &cfg, &rgba), FA_ERROR_INVALID_ARG);
    cfg.flags = 0; cfg.struct_size = 4;
    CHECK_EQ(fa_cdg_surface_create(s, &cfg, &rgba), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_cdg_surface_create(NULL, NULL, &rgba), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_cdg_surface_update(NULL, &fr), FA_ERROR_INVALID_ARG);
    fa_cdg_surface_destroy(nb);
    fa_cdg_surface_destroy(NULL);
    fa_cdg_surface_destroy(big);
    fa_cdg_surface_destroy(smooth);
    fa_stream_close(s);
    /* destroying after the stream is closed is fine too */
    fa_cdg_surface_destroy(tr);
    fa_cdg_surface_destroy(pm);
    free(c.p);
}

static void test_extreme_times(void)
{
    buf c = basic_cdg();
    fa_stream* s = open_wav_stream(1, FA_OPEN_DECODE_ONLY);
    fa_cdg_surface* surf = NULL;
    fa_cdg_frame fr;
    CHECK_EQ(fa_stream_load_cdg_memory(s, c.p, c.n), FA_OK);
    CHECK_EQ(fa_cdg_surface_create(s, NULL, &surf), FA_OK);
    /* No overflow (UBSan) at the ends of the range, with and without offsets */
    CHECK_EQ(fa_cdg_surface_update_at(surf, INT64_MAX / 300, &fr), FA_OK);
    CHECK(is_bgra(px(&fr, 6, 12), 15, 15, 15));
    CHECK_EQ(fa_stream_set_cdg_offset(s, -1000000000), FA_OK);
    CHECK_EQ(fa_cdg_surface_update_at(surf, INT64_MAX, &fr), FA_OK);
    CHECK_EQ(fr.time, INT64_MAX);
    CHECK(is_bgra(px(&fr, 6, 12), 15, 15, 15));
    CHECK_EQ(fa_stream_set_cdg_offset(s, 1000000000), FA_OK);
    CHECK_EQ(fa_cdg_surface_update_at(surf, INT64_MIN, &fr), FA_OK);
    CHECK_EQ(fr.time, INT64_MIN);
    CHECK(is_bgra(px(&fr, 6, 12), 0, 0, 0));
    fa_cdg_surface_destroy(surf);
    fa_stream_close(s);
    free(c.p);
}

/* Indexed pixels do not change with the palette: only palette_changed is reported. */
static void test_palette_changes(void)
{
    static const uint8_t other[8][3] = { { 0, 0, 0 }, { 0, 15, 15 } };
    buf c = small_cdg();
    fa_stream* s = open_wav_stream(1, FA_OPEN_DECODE_ONLY);
    fa_cdg_surface *ix = NULL, *rgb = NULL;
    fa_cdg_surface_config cfg;
    fa_cdg_frame fr;
    colors(&c, 0, other); /* pack 2: colour 1 becomes cyan */
    CHECK_EQ(fa_stream_load_cdg_memory(s, c.p, c.n), FA_OK);
    fa_cdg_surface_config_init(&cfg);
    cfg.pixel_format = FA_CDG_PIXELS_INDEXED8;
    CHECK_EQ(fa_cdg_surface_create(s, &cfg, &ix), FA_OK);
    CHECK_EQ(fa_cdg_surface_create(s, NULL, &rgb), FA_OK);
    CHECK_EQ(fa_cdg_surface_update_at(ix, t_packs(2), &fr), FA_OK);
    CHECK_EQ(fr.palette_changed, 1);
    CHECK_EQ(fa_cdg_surface_update_at(ix, t_packs(3), &fr), FA_OK);
    CHECK_EQ(fr.changed, 1);
    CHECK_EQ(fr.palette_changed, 1);
    CHECK_EQ(fr.dirty_width, 0);
    CHECK_EQ(fr.palette[1][1], 255);
    CHECK_EQ(fa_cdg_surface_update_at(ix, t_packs(3), &fr), FA_OK);
    CHECK_EQ(fr.changed, 0);
    CHECK_EQ(fr.palette_changed, 0);
    /* Colour pixels are redrawn */
    CHECK_EQ(fa_cdg_surface_update_at(rgb, t_packs(2), &fr), FA_OK);
    CHECK_EQ(fa_cdg_surface_update_at(rgb, t_packs(3), &fr), FA_OK);
    CHECK_EQ(fr.palette_changed, 1);
    CHECK_EQ(fr.dirty_width, 300);
    CHECK(is_bgra(px(&fr, 100, 100), 0, 15, 15));
    fa_cdg_surface_destroy(ix);
    fa_cdg_surface_destroy(rgb);
    fa_stream_close(s);
    free(c.p);
}

static void test_offset_and_replace(void)
{
    buf a = small_cdg(), b = { 0 };
    static const uint8_t blue_first[8][3] = { { 0, 0, 15 } };
    fa_stream* s = open_wav_stream(1, FA_OPEN_DECODE_ONLY);
    fa_cdg_surface* surf = NULL;
    fa_cdg_frame fr;
    fa_stream_info info;
    fa_time off = 0;
    colors(&b, 0, blue_first);
    preset(&b, 0, 0);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK_EQ(info.has_cdg, 0);
    CHECK_EQ(fa_cdg_surface_create(s, NULL, &surf), FA_OK);
    CHECK_EQ(fa_cdg_surface_update_at(surf, 500000000, &fr), FA_OK); /* no graphics: black */
    CHECK(is_bgra(px(&fr, 100, 100), 0, 0, 0));

    CHECK_EQ(fa_stream_load_cdg_memory(s, a.p, a.n), FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK_EQ(info.has_cdg, 1);
    CHECK_EQ(fa_cdg_surface_update_at(surf, 500000000, &fr), FA_OK);
    CHECK_EQ(fr.changed, 1);
    CHECK(is_bgra(px(&fr, 100, 100), 15, 0, 0));

    /* A 1 s offset: at 0.5 s nothing is shown yet */
    CHECK_EQ(fa_stream_set_cdg_offset(s, 1000000000), FA_OK);
    CHECK_EQ(fa_stream_get_cdg_offset(s, &off), FA_OK);
    CHECK_EQ(off, 1000000000);
    CHECK_EQ(fa_cdg_surface_update_at(surf, 500000000, &fr), FA_OK);
    CHECK(is_bgra(px(&fr, 100, 100), 0, 0, 0));
    CHECK_EQ(fr.time, -500000000);
    CHECK_EQ(fa_cdg_surface_update_at(surf, 1000000000 + t_packs(2), &fr), FA_OK);
    CHECK(is_bgra(px(&fr, 100, 100), 15, 0, 0));
    fa_stream_set_cdg_offset(s, 0);

    /* Replacing the graphics restarts every surface on the new ones */
    CHECK_EQ(fa_stream_load_cdg_memory(s, b.p, b.n), FA_OK);
    CHECK_EQ(fa_cdg_surface_update_at(surf, 500000000, &fr), FA_OK);
    CHECK_EQ(fr.changed, 1);
    CHECK(is_bgra(px(&fr, 100, 100), 0, 0, 15));

    /* Removing them */
    CHECK_EQ(fa_stream_load_cdg_path(s, NULL), FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK_EQ(info.has_cdg, 0);
    CHECK_EQ(fa_cdg_surface_update_at(surf, 500000000, &fr), FA_OK);
    CHECK(is_bgra(px(&fr, 100, 100), 0, 0, 0));

    CHECK_EQ(fa_stream_load_cdg_memory(s, a.p, 23), FA_ERROR_UNSUPPORTED_FORMAT);
    CHECK_EQ(fa_stream_load_cdg_path(s, "no-such-file.cdg"), FA_ERROR_NOT_FOUND);
    CHECK_EQ(fa_stream_load_cdg_memory(NULL, a.p, a.n), FA_ERROR_INVALID_ARG);
    fa_cdg_surface_destroy(surf);
    fa_stream_close(s);
    free(a.p); free(b.p);
}

static void test_sibling_file(void)
{
    buf wav = make_wav(8000, 8000), cdg = small_cdg();
    fa_stream* s = NULL;
    fa_stream_info info;
    fa_open_options o;
    write_file("cdg_sibling.wav", &wav);
    write_file("cdg_sibling.CDG", &cdg); /* upper case, as on many karaoke discs */
    write_file("cdg_alone.wav", &wav);
    write_file("cdg_mixed.wav", &wav);
    write_file("cdg_mixed.cDg", &cdg); /* any spelling of the extension */
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    CHECK_EQ(fa_stream_open_path(g_engine, "cdg_sibling.wav", &o, &s), FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK_EQ(info.has_cdg, 1);
    fa_stream_close(s);
    o.flags = FA_OPEN_DECODE_ONLY | FA_OPEN_NO_CDG;
    CHECK_EQ(fa_stream_open_path(g_engine, "cdg_sibling.wav", &o, &s), FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK_EQ(info.has_cdg, 0);
    CHECK_EQ(fa_stream_load_cdg_path(s, "cdg_sibling.CDG"), FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK_EQ(info.has_cdg, 1);
    fa_stream_close(s);
    o.flags = FA_OPEN_DECODE_ONLY;
    CHECK_EQ(fa_stream_open_path(g_engine, "cdg_mixed.wav", &o, &s), FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK_EQ(info.has_cdg, 1);
    fa_stream_close(s);
    CHECK_EQ(fa_stream_open_path(g_engine, "cdg_alone.wav", &o, &s), FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK_EQ(info.has_cdg, 0);
    fa_stream_close(s);
    free(wav.p); free(cdg.p);
}

/* A whole song of tiles: going back replays from the first pack, which must stay cheap. */
static void test_replay_speed(void)
{
    buf c = { 0 };
    fa_stream* s = open_wav_stream(1, FA_OPEN_DECODE_ONLY);
    fa_cdg_surface* surf = NULL;
    fa_cdg_frame fr;
    int64_t i, t0, worst = 0;
    int round;
    colors(&c, 0, k_low);
    for (i = 0; i < 300 * 300; i++) {   /* 5 minutes */
        if (i % 3000 == 0) preset(&c, (int)(i / 3000) % 6, 0);
        else tile(&c, (int)(i & 1), 1, 4, (int)(i % 18), (int)(i % 50), (uint8_t)i);
    }
    CHECK_EQ(fa_stream_load_cdg_memory(s, c.p, c.n), FA_OK);
    CHECK_EQ(fa_cdg_surface_create(s, NULL, &surf), FA_OK);
    CHECK_EQ(fa_cdg_surface_update_at(surf, 300LL * 1000000000, &fr), FA_OK);
    for (round = 0; round < 3; round++) {
        /* Back by a few seconds, as a seek or scrub does: replays from a checkpoint */
        t0 = fa_monotonic_ns();
        CHECK_EQ(fa_cdg_surface_update_at(surf, (299LL - round * 7) * 1000000000, &fr), FA_OK);
        if (fa_monotonic_ns() - t0 > worst) worst = fa_monotonic_ns() - t0;
    }
    printf("  seeking back in a 5 minute song: %.2f ms\n", worst / 1e6);
    CHECK(worst < 2000000000); /* generous: sanitizer builds on slow CI machines */
    /* A checkpoint gives the same picture as replaying from the start */
    {
        fa_cdg_surface* fresh = NULL;
        fa_cdg_frame fr2;
        CHECK_EQ(fa_cdg_surface_create(s, NULL, &fresh), FA_OK);
        CHECK_EQ(fa_cdg_surface_update_at(surf, 0, &fr), FA_OK);
        CHECK_EQ(fa_cdg_surface_update_at(surf, 300LL * 1000000000, &fr), FA_OK);
        CHECK_EQ(fa_cdg_surface_update_at(surf, 95LL * 1000000000, &fr), FA_OK);
        CHECK_EQ(fa_cdg_surface_update_at(fresh, 95LL * 1000000000, &fr2), FA_OK);
        CHECK(memcmp(fr.pixels, fr2.pixels, (size_t)fr.stride * fr.height) == 0);
        fa_cdg_surface_destroy(fresh);
    }
    fa_cdg_surface_destroy(surf);
    fa_stream_close(s);
    free(c.p);
}

/* Graphics follow the stream's audio clock during playback, pause and seek. */
static void test_follows_audio_clock(void)
{
    buf c = { 0 };
    fa_stream* s = open_wav_stream(4, 0);
    fa_device* dev = NULL;
    fa_cdg_surface* surf = NULL;
    fa_cdg_frame fr;
    fa_time clock = 0;
    int64_t deadline;
    colors(&c, 0, k_low);
    empty_packs_to(&c, 300);  /* 1 s of nothing */
    preset(&c, 3, 0);         /* blue at 1 s */
    CHECK(s);
    CHECK_EQ(fa_stream_load_cdg_memory(s, c.p, c.n), FA_OK);
    CHECK_EQ(fa_cdg_surface_create(s, NULL, &surf), FA_OK);
    CHECK_EQ(fa_device_open(g_engine, NULL, &dev), FA_OK);
    CHECK_EQ(fa_device_start(dev), FA_OK);
    CHECK_EQ(fa_stream_attach(s, dev, NULL), FA_OK);
    CHECK_EQ(fa_cdg_surface_update(surf, &fr), FA_OK);
    CHECK(is_bgra(px(&fr, 100, 100), 0, 0, 0));
    CHECK_EQ(fa_stream_play(s), FA_OK);
    deadline = fa_monotonic_ns() + 8000000000LL;
    do {
        fa_sleep_ms(20);
        CHECK_EQ(fa_cdg_surface_update(surf, &fr), FA_OK);
    } while (fr.time < 1100000000 && fa_monotonic_ns() < deadline);
    CHECK(fr.time >= 1100000000);
    CHECK(is_bgra(px(&fr, 100, 100), 0, 0, 15));
    CHECK_EQ(fa_stream_pause(s), FA_OK);
    CHECK_EQ(fa_stream_get_clock(s, &clock), FA_OK);
    CHECK_EQ(fa_cdg_surface_update(surf, &fr), FA_OK);
    /* Right after a pause the clock still moves while the device plays out its
     * buffer: the surface reads it a moment later. */
    CHECK(fr.time >= clock && fr.time - clock < 200000000);
    CHECK_EQ(fa_stream_seek(s, 0), FA_OK);
    CHECK_EQ(fa_cdg_surface_update(surf, &fr), FA_OK);
    CHECK(fr.time < 1000000000);
    CHECK(is_bgra(px(&fr, 100, 100), 0, 0, 0));
    fa_cdg_surface_destroy(surf);
    fa_stream_close(s);
    fa_device_close(dev);
    free(c.p);
}

int main(void)
{
    fa_engine_config cfg;
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    if (fa_engine_create(&cfg, &g_engine) != FA_OK) { fprintf(stderr, "engine\n"); return 1; }
    RUN(test_inflate_vectors);
    RUN(test_zip_pair);
    RUN(test_zip_damaged);
    RUN(test_zip_edge_cases);
    RUN(test_decoder_basics);
    RUN(test_xor_and_repeats);
    RUN(test_scrolling);
    RUN(test_surface_options);
    RUN(test_extreme_times);
    RUN(test_palette_changes);
    RUN(test_offset_and_replace);
    RUN(test_sibling_file);
    RUN(test_replay_speed);
    RUN(test_follows_audio_clock);
    fa_engine_destroy(g_engine);
    remove("cdg_pair.zip");
    remove("cdg_sibling.wav");
    remove("cdg_sibling.CDG");
    remove("cdg_alone.wav");
    remove("cdg_mixed.wav");
    remove("cdg_mixed.cDg");
    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    printf("all passed\n");
    return 0;
}
