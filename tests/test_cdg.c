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
#include <time.h>
#ifndef _WIN32
#  include <dirent.h>
#  include <utime.h>
#endif

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

static void transparent(buf* b, int color, int value)
{
    uint8_t d[16] = { 0 };
    d[color] = (uint8_t)value;
    pack(b, 28, d);
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
        fa_source zs;
        uint8_t* out = NULL;
        buf body = stored_deflate(&wav);
        memset(&e, 0, sizeof(e));
        e.method = 8;
        e.compressed_size = 100;
        e.size = 3000000000u;
        files[0].name = "a.wav"; files[0].data = &wav; files[0].deflate = 1;
        zip = make_zip(files, 1);
        e.header_offset = 0;
        fa_source_open_memory_ref(&zs, zip.p, zip.n);
        CHECK_EQ(fa_zip_extract(&zs, &e, UINT64_MAX, &out), FA_ERROR_UNSUPPORTED_FORMAT);
        CHECK(out == NULL);
        /* Past the caller's limit: too large, also before allocating */
        e.size = 2000;
        CHECK_EQ(fa_zip_extract(&zs, &e, 1000, &out), FA_ERROR_TOO_LARGE);
        CHECK(out == NULL);
        free(zip.p);
        free(body.p);
    }
    free(wav.p); free(cdg.p);
}

/* ---- Lean archive reading -------------------------------------------------------------- */

/* A real compressor for the tests: greedy LZ77 with fixed Huffman codes, so the
 * inflater meets matches and literals across its 1 MB output chunks. */
typedef struct bitw { buf b; uint32_t acc; int n; } bitw;
static void bw_put(bitw* w, uint32_t v, int bits)
{
    w->acc |= v << w->n;
    w->n += bits;
    while (w->n >= 8) { put8(&w->b, w->acc & 0xFF); w->acc >>= 8; w->n -= 8; }
}
static void bw_code(bitw* w, uint32_t code, int bits) /* Huffman codes go most significant bit first */
{
    uint32_t r = 0;
    int i;
    for (i = 0; i < bits; i++) r |= ((code >> i) & 1u) << (bits - 1 - i);
    bw_put(w, r, bits);
}
static void bw_sym(bitw* w, uint32_t v)
{
    if (v < 144) bw_code(w, 0x30 + v, 8);
    else if (v < 256) bw_code(w, 0x190 + v - 144, 9);
    else if (v < 280) bw_code(w, v - 256, 7);
    else bw_code(w, 0xC0 + v - 280, 8);
}
static buf lz_deflate(const buf* d)
{
    static const uint16_t lb[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                     35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const uint8_t le[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    static const uint16_t db[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                     1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
    static const uint8_t de[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
    bitw w;
    int64_t* head = (int64_t*)malloc(sizeof(int64_t) << 15);
    size_t pos = 0, i;
    memset(&w, 0, sizeof(w));
    for (i = 0; i < ((size_t)1 << 15); i++) head[i] = -1;
    bw_put(&w, 1, 1);  /* final block */
    bw_put(&w, 1, 2);  /* fixed codes */
    while (pos < d->n) {
        size_t len = 0, dist = 0;
        if (d->n - pos >= 3) {
            uint32_t h = ((uint32_t)d->p[pos] * 2654435761u ^ (uint32_t)d->p[pos + 1] * 40503u ^ d->p[pos + 2]) & 0x7FFF;
            int64_t c = head[h];
            head[h] = (int64_t)pos;
            if (c >= 0 && pos - (size_t)c <= 32768) {
                while (len < 258 && pos + len < d->n && d->p[(size_t)c + len] == d->p[pos + len]) len++;
                dist = pos - (size_t)c;
            }
        }
        if (len >= 3) {
            int k = 28, j = 30;
            while (lb[k] > len) k--;
            bw_sym(&w, 257 + (uint32_t)k);
            bw_put(&w, (uint32_t)(len - lb[k]), le[k]);
            while (db[j - 1] > dist) j--;
            bw_code(&w, (uint32_t)(j - 1), 5);
            bw_put(&w, (uint32_t)(dist - db[j - 1]), de[j - 1]);
            pos += len;
        } else {
            bw_sym(&w, d->p[pos++]);
        }
    }
    bw_sym(&w, 256);
    if (w.n) put8(&w.b, w.acc & 0xFF);
    free(head);
    return w.b;
}

/* A mono 16-bit WAV of `frames`: a periodic tone (long matches) with a stretch of
 * noise in the middle (literals). */
static buf make_long_wav(uint32_t frames)
{
    buf b = { 0 };
    uint32_t i, data = frames * 2, seed = 12345;
    put(&b, "RIFF", 4); le32(&b, 36 + data); put(&b, "WAVE", 4);
    put(&b, "fmt ", 4); le32(&b, 16); le16(&b, 1); le16(&b, 1);
    le32(&b, 44100); le32(&b, 88200); le16(&b, 2); le16(&b, 16);
    put(&b, "data", 4); le32(&b, data);
    for (i = 0; i < frames; i++) {
        int v = (int)(sin(i * 6.283185307 / 160.0) * 8000.0);
        if (i > frames / 3 && i < frames / 3 + 40000) { seed = seed * 1103515245u + 12345u; v = (int)(seed >> 16) - 32768; }
        le16(&b, (uint16_t)(int16_t)v);
    }
    return b;
}

/* make_zip with a real compressor for the deflate entries. */
static buf make_zip_lz(const zip_file* files, int count)
{
    buf out = { 0 }, cd = { 0 };
    uint32_t offsets[8];
    int i;
    for (i = 0; i < count; i++) {
        buf body = files[i].deflate ? lz_deflate(files[i].data) : *files[i].data;
        uint32_t crc = fa_crc32(files[i].data->p, files[i].data->n) ^ files[i].crc_xor;
        uint32_t name_len = (uint32_t)strlen(files[i].name);
        offsets[i] = (uint32_t)out.n;
        le32(&out, 0x04034b50); le16(&out, 20); le16(&out, 0); le16(&out, files[i].deflate ? 8 : 0);
        le16(&out, 0); le16(&out, 0); le32(&out, crc); le32(&out, (uint32_t)body.n); le32(&out, (uint32_t)files[i].data->n);
        le16(&out, name_len); le16(&out, 0);
        put(&out, files[i].name, name_len);
        put(&out, body.p, body.n);
        le32(&cd, 0x02014b50); le16(&cd, 20); le16(&cd, 20); le16(&cd, 0); le16(&cd, files[i].deflate ? 8 : 0);
        le16(&cd, 0); le16(&cd, 0); le32(&cd, crc); le32(&cd, (uint32_t)body.n); le32(&cd, (uint32_t)files[i].data->n);
        le16(&cd, name_len); le16(&cd, 0); le16(&cd, 0); le16(&cd, 0); le16(&cd, 0); le32(&cd, 0);
        le32(&cd, offsets[i]);
        put(&cd, files[i].name, name_len);
        if (files[i].deflate) free(body.p);
    }
    {
        uint32_t cd_offset = (uint32_t)out.n;
        put(&out, cd.p, cd.n);
        le32(&out, 0x06054b50); le16(&out, 0); le16(&out, 0); le16(&out, (uint32_t)count); le16(&out, (uint32_t)count);
        le32(&out, (uint32_t)cd.n); le32(&out, cd_offset); le16(&out, 0);
    }
    free(cd.p);
    return out;
}

/* Decodes the whole stream: frames, or -1 on an error. */
static int64_t decode_all(fa_stream* s)
{
    float pcm[4096];
    int64_t total = 0;
    for (;;) {
        fa_frames got = 0;
        fa_result r = fa_stream_read(s, pcm, 2048, &got);
        if (r == FA_ERROR_END_OF_STREAM || (r == FA_OK && got == 0)) return total;
        if (r != FA_OK) return -1;
        total += got;
    }
}

static void test_inflate_streaming(void)
{
    buf wav = make_long_wav(1500000), z = lz_deflate(&wav), zip;   /* 3 MB, 3 output chunks */
    zip_file f;
    fa_source zs;
    fa_zip_entry* e = NULL;
    uint32_t count = 0;
    uint8_t* out = NULL;
    FILE* tmp;
    CHECK(z.n < wav.n / 4);
    /* Into memory, one buffer */
    out = (uint8_t*)malloc(wav.n);
    CHECK_EQ(fa_inflate(z.p, z.n, out, wav.n), FA_OK);
    CHECK(memcmp(out, wav.p, wav.n) == 0);
    free(out);
    /* Into a file, through the 1 MB window, input pulled from a file source */
    f.name = "big.wav"; f.data = &wav; f.deflate = 1; f.crc_xor = 0;
    zip = make_zip_lz(&f, 1);
    write_file("cdg_stream.zip", &zip);
    CHECK_EQ(fa_source_open_path(&zs, "cdg_stream.zip"), FA_OK);
    CHECK_EQ(fa_zip_list(&zs, &e, &count), FA_OK);
    CHECK_EQ(count, 1);
    tmp = tmpfile();
    CHECK(tmp != NULL);
    CHECK_EQ(fa_zip_extract_file(&zs, &e[0], tmp), FA_OK);
    CHECK_EQ(ftell(tmp), (long)wav.n);
    rewind(tmp);
    out = (uint8_t*)malloc(wav.n);
    CHECK_EQ(fread(out, 1, wav.n, tmp), wav.n);
    CHECK(memcmp(out, wav.p, wav.n) == 0);
    free(out);
    out = NULL;
    /* And into memory from the file source */
    CHECK_EQ(fa_zip_extract(&zs, &e[0], wav.n, &out), FA_OK);
    CHECK(memcmp(out, wav.p, wav.n) == 0);
    free(out);
    fclose(tmp);
    fa_source_close(&zs);
    free(e);
    /* A flipped byte in the middle: an error, never a crash (ASan) */
    zip.p[zip.n / 2] ^= 0x5A;
    fa_source_open_memory_ref(&zs, zip.p, zip.n);
    CHECK_EQ(fa_zip_list(&zs, &e, &count), FA_OK);
    out = NULL;
    CHECK(fa_zip_extract(&zs, &e[0], wav.n, &out) != FA_OK);
    CHECK(out == NULL);
    free(e);
    free(zip.p); free(z.p); free(wav.p);
    remove("cdg_stream.zip");
}

/* App callbacks over a memory block, counting closes. */
typedef struct mem_io { const buf* b; int64_t pos; int closes; int seekable; } mem_io;
static int64_t FA_CALL mio_read(void* u, void* dst, size_t n)
{
    mem_io* m = (mem_io*)u;
    size_t left = (size_t)((int64_t)m->b->n - m->pos);
    if (n > left) n = left;
    memcpy(dst, m->b->p + m->pos, n);
    m->pos += (int64_t)n;
    return (int64_t)n;
}
static fa_result FA_CALL mio_seek(void* u, int64_t off)
{
    mem_io* m = (mem_io*)u;
    if (!m->seekable || off < 0 || off > (int64_t)m->b->n) return FA_ERROR_IO;
    m->pos = off;
    return FA_OK;
}
static int64_t FA_CALL mio_length(void* u) { return (int64_t)((mem_io*)u)->b->n; }
static void FA_CALL mio_close(void* u) { ((mem_io*)u)->closes++; }

static void test_zip_lean(void)
{
    buf wav = make_wav(8000, 8000), cdg = small_cdg(), zip;
    zip_file files[2];
    fa_open_options o;
    fa_stream* s = NULL;
    fa_stream_info info;
    fa_io_callbacks io;
    mem_io m;
    int deflate;
    files[0].name = "Song.cdg"; files[0].data = &cdg; files[0].deflate = 0; files[0].crc_xor = 0;
    files[1].name = "Song.wav"; files[1].data = &wav; files[1].crc_xor = 0;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    memset(&io, 0, sizeof(io));
    io.read = mio_read; io.seek = mio_seek; io.length = mio_length; io.close = mio_close;
    for (deflate = 0; deflate < 2; deflate++) {
        files[1].deflate = deflate;
        zip = make_zip_lz(files, 2);
        write_file("cdg_lean.zip", &zip);
        /* A path: stored audio plays in place, deflated from memory */
        CHECK_EQ(fa_stream_open_path(g_engine, "cdg_lean.zip", &o, &s), FA_OK);
        CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
        CHECK_EQ(info.has_cdg, 1);
        CHECK_EQ(info.length_frames, 8000);
        CHECK_EQ(info.file_size, (int64_t)zip.n);
        CHECK_EQ(decode_all(s), 8000);
        CHECK_EQ(fa_stream_seek(s, 4000), FA_OK);
        CHECK_EQ(decode_all(s), 4000);
        {
            fa_tags* t = NULL;  /* the tags reader finds the entry again */
            CHECK_EQ(fa_stream_get_tags(s, 0, &t), FA_OK);
            fa_tags_close(t);
        }
        fa_stream_close(s);
        /* Memory */
        CHECK_EQ(fa_stream_open_memory(g_engine, zip.p, zip.n, &o, &s), FA_OK);
        CHECK_EQ(decode_all(s), 8000);
        fa_stream_close(s);
        /* App callbacks: closed once, with the stream */
        memset(&m, 0, sizeof(m));
        m.b = &zip; m.seekable = 1;
        CHECK_EQ(fa_stream_open_io(g_engine, &io, &m, &o, &s), FA_OK);
        CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
        CHECK_EQ(info.has_cdg, 1);
        CHECK_EQ(decode_all(s), 8000);
        CHECK_EQ(m.closes, 0);
        fa_stream_close(s);
        CHECK_EQ(m.closes, 1);
        /* Callbacks that cannot seek: the directory is at the end */
        memset(&m, 0, sizeof(m));
        m.b = &zip;
        io.seek = NULL;
        CHECK_EQ(fa_stream_open_io(g_engine, &io, &m, &o, &s), FA_ERROR_NOT_SEEKABLE);
        CHECK(s == NULL);
        CHECK_EQ(m.closes, 0);   /* a failed open leaves them to the app */
        io.seek = mio_seek;
        /* Cut archives fail cleanly from a path and from callbacks */
        {
            size_t cut;
            for (cut = 4; cut < zip.n; cut += zip.n / 13 + 1) {
                buf part = zip;
                part.n = cut;
                write_file("cdg_lean.zip", &part);
                if (fa_stream_open_path(g_engine, "cdg_lean.zip", &o, &s) == FA_OK) { fa_stream_close(s); s = NULL; }
                memset(&m, 0, sizeof(m));
                m.b = &part; m.seekable = 1;
                if (fa_stream_open_io(g_engine, &io, &m, &o, &s) == FA_OK) { fa_stream_close(s); s = NULL; CHECK_EQ(m.closes, 1); }
                else CHECK_EQ(m.closes, 0);
            }
        }
        free(zip.p);
    }
    remove("cdg_lean.zip");
    free(wav.p); free(cdg.p);
}

/* ---- ZIP64 ---------------------------------------------------------------------- */

static void le64(buf* b, uint64_t v) { le32(b, (uint32_t)v); le32(b, (uint32_t)(v >> 32)); }

typedef struct z64_file { const char* name; const buf* data; int deflate; uint64_t gap; } z64_file;
typedef struct z64_layout { size_t cd_at, first_extra, z64_end, locator, end; } z64_layout;

/* A ZIP64 archive. `n_empty` empty entries come first (to pass 65,535 entries);
 * `wide` writes every file entry with 0xFFFFFFFF fields and ZIP64 extra fields, else
 * only the end records are ZIP64. A file's `gap` is a stretch of zeros that is not in
 * the returned bytes (the entry's data is `gap` bytes long): offsets after it count
 * it, so the archive can be bigger than 4 GB without being written. */
static buf make_zip64(const z64_file* files, int count, int n_empty, int wide, uint64_t* gap_at, z64_layout* lay)
{
    buf z = { 0 }, cd = { 0 };
    uint64_t skipped = 0, cd_offset;
    int i;
    char name[32];
    memset(lay, 0, sizeof(*lay));
    for (i = -n_empty; i < count; i++) {
        const z64_file* f = i >= 0 ? &files[i] : NULL;
        buf body = { 0 };
        const char* nm = name;
        uint64_t size = 0, csize = 0, offset = (uint64_t)z.n + skipped;
        uint32_t crc = 0, name_len;
        if (f) {
            nm = f->name;
            if (f->gap) { size = csize = f->gap; }
            else {
                body = f->deflate ? stored_deflate(f->data) : *f->data;
                size = f->data->n; csize = body.n;
                crc = fa_crc32(f->data->p, f->data->n);
            }
        } else {
            snprintf(name, sizeof(name), "e%d.txt", i + n_empty);
        }
        name_len = (uint32_t)strlen(nm);
        le32(&z, 0x04034b50); le16(&z, 45); le16(&z, 0); le16(&z, f && f->deflate ? 8 : 0);
        le16(&z, 0); le16(&z, 0); le32(&z, crc);
        le32(&z, f && wide ? 0xFFFFFFFFu : (uint32_t)csize); le32(&z, f && wide ? 0xFFFFFFFFu : (uint32_t)size);
        le16(&z, name_len); le16(&z, f && wide ? 20 : 0);
        put(&z, nm, name_len);
        if (f && wide) { le16(&z, 1); le16(&z, 16); le64(&z, size); le64(&z, csize); }
        if (f && f->gap) { if (gap_at) *gap_at = z.n; skipped = f->gap; }
        else put(&z, body.p, body.n);
        if (f && f->deflate) free(body.p);
        le32(&cd, 0x02014b50); le16(&cd, 45); le16(&cd, 45); le16(&cd, 0); le16(&cd, f && f->deflate ? 8 : 0);
        le16(&cd, 0); le16(&cd, 0); le32(&cd, crc);
        le32(&cd, f && wide ? 0xFFFFFFFFu : (uint32_t)csize); le32(&cd, f && wide ? 0xFFFFFFFFu : (uint32_t)size);
        le16(&cd, name_len); le16(&cd, f && wide ? 28 : (offset > 0xFFFFFFFEu ? 12 : 0)); le16(&cd, 0); le16(&cd, 0);
        le16(&cd, 0); le32(&cd, 0);
        le32(&cd, f && (wide || offset > 0xFFFFFFFEu) ? 0xFFFFFFFFu : (uint32_t)offset);
        put(&cd, nm, name_len);
        if (f && wide) {
            if (!lay->first_extra) lay->first_extra = cd.n;   /* in the directory */
            le16(&cd, 1); le16(&cd, 24); le64(&cd, size); le64(&cd, csize); le64(&cd, offset);
        } else if (f && offset > 0xFFFFFFFEu) {
            le16(&cd, 1); le16(&cd, 8); le64(&cd, offset);
        }
    }
    cd_offset = (uint64_t)z.n + skipped;
    lay->cd_at = z.n;
    put(&z, cd.p, cd.n);
    lay->z64_end = z.n;
    le32(&z, 0x06064b50); le64(&z, 44); le16(&z, 45); le16(&z, 45); le32(&z, 0); le32(&z, 0);
    le64(&z, (uint64_t)(count + n_empty)); le64(&z, (uint64_t)(count + n_empty));
    le64(&z, cd.n); le64(&z, cd_offset);
    lay->locator = z.n;
    le32(&z, 0x07064b50); le32(&z, 0); le64(&z, cd_offset + cd.n); le32(&z, 1);
    lay->end = z.n;
    le32(&z, 0x06054b50); le16(&z, 0); le16(&z, 0); le16(&z, 0xFFFF); le16(&z, 0xFFFF);
    le32(&z, 0xFFFFFFFFu); le32(&z, 0xFFFFFFFFu); le16(&z, 0);
    free(cd.p);
    return z;
}

static void open_and_decode(const buf* zip, int64_t frames)
{
    fa_open_options o;
    fa_stream* s = NULL;
    fa_stream_info info;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    CHECK_EQ(fa_stream_open_memory(g_engine, zip->p, zip->n, &o, &s), FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK_EQ(info.has_cdg, 1);
    CHECK_EQ(decode_all(s), frames);
    fa_stream_close(s);
}

static void test_zip64_basic(void)
{
    buf wav = make_wav(8000, 4000), cdg = small_cdg(), zip;
    z64_file files[2];
    z64_layout lay;
    fa_source zs;
    fa_zip_entry* e = NULL;
    uint32_t count = 0;
    int wide, deflate;
    files[0].name = "Song.cdg"; files[0].data = &cdg; files[0].deflate = 0; files[0].gap = 0;
    files[1].name = "Song.wav"; files[1].data = &wav; files[1].gap = 0;
    for (wide = 0; wide < 2; wide++) {
        for (deflate = 0; deflate < 2; deflate++) {
            buf sd = stored_deflate(&wav);
            files[1].deflate = deflate;
            zip = make_zip64(files, 2, 0, wide, NULL, &lay);
            open_and_decode(&zip, 4000);
            fa_source_open_memory_ref(&zs, zip.p, zip.n);
            CHECK_EQ(fa_zip_list(&zs, &e, &count), FA_OK);
            CHECK_EQ(count, 2);
            CHECK_EQ(e[1].size, wav.n);
            CHECK_EQ(e[1].compressed_size, deflate ? sd.n : wav.n);
            free(e); e = NULL;
            free(sd.p);
            free(zip.p);
        }
    }
    free(wav.p); free(cdg.p);
}

static void test_zip64_many_entries(void)
{
    buf wav = make_wav(8000, 2000), cdg = small_cdg(), zip;
    z64_file files[2];
    z64_layout lay;
    fa_source zs;
    fa_zip_entry* e = NULL;
    uint32_t count = 0;
    files[0].name = "Song.cdg"; files[0].data = &cdg; files[0].deflate = 0; files[0].gap = 0;
    files[1].name = "Song.wav"; files[1].data = &wav; files[1].deflate = 0; files[1].gap = 0;
    zip = make_zip64(files, 2, 70000, 0, NULL, &lay);   /* the end record's own count stops at 65,535 */
    fa_source_open_memory_ref(&zs, zip.p, zip.n);
    CHECK_EQ(fa_zip_list(&zs, &e, &count), FA_OK);
    CHECK_EQ(count, 70002);
    CHECK(strcmp(e[69999].name, "e69999.txt") == 0);
    CHECK(strcmp(e[70001].name, "Song.wav") == 0);
    free(e);
    open_and_decode(&zip, 2000);
    free(zip.p); free(wav.p); free(cdg.p);
}

/* An archive of 4.5 GB that is mostly one stored entry of zeros: a virtual file made
 * of the generated bytes around the gap. The song after it starts past 4 GB. */
typedef struct big_io { const buf* b; uint64_t gap_at, gap; int64_t pos; } big_io;
static int64_t FA_CALL big_read(void* u, void* dst, size_t n)
{
    big_io* m = (big_io*)u;
    int64_t total = (int64_t)(m->b->n + m->gap), done = 0;
    uint8_t* out = (uint8_t*)dst;
    while ((size_t)done < n && m->pos < total) {
        uint64_t pos = (uint64_t)m->pos, chunk;
        if (pos < m->gap_at) {
            chunk = m->gap_at - pos;
            if (chunk > n - (size_t)done) chunk = n - (size_t)done;
            memcpy(out + done, m->b->p + pos, (size_t)chunk);
        } else if (pos < m->gap_at + m->gap) {
            chunk = m->gap_at + m->gap - pos;
            if (chunk > n - (size_t)done) chunk = n - (size_t)done;
            memset(out + done, 0, (size_t)chunk);
        } else {
            chunk = (uint64_t)total - pos;
            if (chunk > n - (size_t)done) chunk = n - (size_t)done;
            memcpy(out + done, m->b->p + (pos - m->gap), (size_t)chunk);
        }
        done += (int64_t)chunk;
        m->pos += (int64_t)chunk;
    }
    return done;
}
static fa_result FA_CALL big_seek(void* u, int64_t off)
{
    big_io* m = (big_io*)u;
    if (off < 0 || off > (int64_t)(m->b->n + m->gap)) return FA_ERROR_IO;
    m->pos = off;
    return FA_OK;
}
static int64_t FA_CALL big_length(void* u) { big_io* m = (big_io*)u; return (int64_t)(m->b->n + m->gap); }
static void FA_CALL big_close(void* u) { (void)u; }

static void test_zip64_over_4gb(void)
{
    buf wav = make_wav(8000, 6000), cdg = small_cdg(), zip;
    z64_file files[3];
    z64_layout lay;
    uint64_t gap_at = 0;
    big_io m;
    fa_io_callbacks io;
    fa_open_options o;
    fa_stream* s = NULL;
    fa_stream_info info;
    fa_tags* t = NULL;
    int wide;
    files[0].name = "pad.bin"; files[0].data = NULL; files[0].deflate = 0; files[0].gap = 4500000000ull;
    files[1].name = "Song.cdg"; files[1].data = &cdg; files[1].deflate = 0; files[1].gap = 0;
    files[2].name = "Song.wav"; files[2].data = &wav; files[2].deflate = 0; files[2].gap = 0;
    memset(&io, 0, sizeof(io));
    io.read = big_read; io.seek = big_seek; io.length = big_length; io.close = big_close;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    for (wide = 0; wide < 2; wide++) {
        zip = make_zip64(files, 3, 0, wide, &gap_at, &lay);
        memset(&m, 0, sizeof(m));
        m.b = &zip; m.gap_at = gap_at; m.gap = files[0].gap;
        CHECK_EQ(fa_stream_open_io(g_engine, &io, &m, &o, &s), FA_OK);
        CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
        CHECK_EQ(info.has_cdg, 1);
        CHECK_EQ(info.length_frames, 6000);
        CHECK_EQ(info.file_size, (int64_t)(zip.n + files[0].gap));
        CHECK_EQ(decode_all(s), 6000);
        CHECK_EQ(fa_stream_seek(s, 3000), FA_OK);
        CHECK_EQ(decode_all(s), 3000);
        CHECK_EQ(fa_stream_get_tags(s, 0, &t), FA_OK);
        fa_tags_close(t);
        fa_stream_close(s);
        free(zip.p);
    }
    free(wav.p); free(cdg.p);
}

/* Damaged ZIP64 records are refused (never read out of bounds: run under ASan). */
static void test_zip64_damaged(void)
{
    buf wav = make_wav(8000, 400), cdg = small_cdg(), zip;
    z64_file files[2];
    z64_layout lay;
    fa_source zs;
    fa_zip_entry* e = NULL;
    uint32_t count = 0;
    int k;
    files[0].name = "Song.cdg"; files[0].data = &cdg; files[0].deflate = 0; files[0].gap = 0;
    files[1].name = "Song.wav"; files[1].data = &wav; files[1].deflate = 0; files[1].gap = 0;
    for (k = 0; k < 9; k++) {
        zip = make_zip64(files, 2, 0, 1, NULL, &lay);
        lay.first_extra += lay.cd_at;
        switch (k) {
        case 0: zip.p[lay.locator] ^= 0xFF; break;                       /* locator signature */
        case 1: zip.p[lay.z64_end] ^= 0xFF; break;                       /* ZIP64 end signature */
        case 2: zip.p[lay.locator + 8] += 1; break;                      /* locator points elsewhere */
        case 3: zip.p[lay.locator + 15] = 0x7F; break;                   /* ...past the end */
        case 4: zip.p[lay.z64_end + 24] = 3; break;                      /* entries on this disk differ */
        case 5: zip.p[lay.first_extra] = 2; break;                       /* no ZIP64 field in the extra */
        case 6: zip.p[lay.first_extra + 2] = 0x40; break;                /* field longer than the extra */
        case 7: zip.p[lay.first_extra + 2] = 8; break;                   /* field too short for its sizes */
        default: zip.p[lay.z64_end + 16] = 1; break;                     /* ZIP64 end on another disk */
        }
        fa_source_open_memory_ref(&zs, zip.p, zip.n);
        CHECK_EQ(fa_zip_list(&zs, &e, &count), FA_ERROR_UNSUPPORTED_FORMAT);
        CHECK(e == NULL);
        free(zip.p);
    }
    /* Cut anywhere: an error, never a crash */
    zip = make_zip64(files, 2, 0, 1, NULL, &lay);
    for (k = 1; k < (int)zip.n; k += 3) {
        fa_source_open_memory_ref(&zs, zip.p, (size_t)k);
        if (fa_zip_list(&zs, &e, &count) == FA_OK) free(e);
        e = NULL;
    }
    free(zip.p); free(wav.p); free(cdg.p);
}

/* Compressed audio past the memory limit: an error without a temp directory, a
 * temp file (gone when the stream closes) with one. */
static void test_zip_over_limit(void)
{
    buf wav = make_long_wav(1500000), cdg = small_cdg(), zip;
    zip_file files[2];
    fa_engine_config cfg;
    fa_engine* e = NULL;
    fa_open_options o;
    fa_stream* s = NULL;
    fa_stream_info info;
    files[0].name = "Big.cdg"; files[0].data = &cdg; files[0].deflate = 1; files[0].crc_xor = 0;
    files[1].name = "Big.wav"; files[1].data = &wav; files[1].deflate = 1; files[1].crc_xor = 0;
    zip = make_zip_lz(files, 2);
    write_file("cdg_big.zip", &zip);
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;

    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    cfg.zip_memory_limit_mb = 1;
    CHECK_EQ(fa_engine_create(&cfg, &e), FA_OK);
    CHECK_EQ(fa_stream_open_path(e, "cdg_big.zip", &o, &s), FA_ERROR_TOO_LARGE);
    CHECK(s == NULL);
    CHECK_EQ(fa_stream_open_memory(e, zip.p, zip.n, &o, &s), FA_ERROR_TOO_LARGE);
    fa_engine_destroy(e);
    e = NULL;

#ifndef _WIN32
    {
        /* A leftover from a crash, old enough to be nobody's */
        FILE* stale;
        struct utimbuf old;
        fa_mkdir_utf8("cdg_tmp");
        fa_mkdir_utf8("cdg_tmp/fatzaudio-tmp");
        stale = fopen("cdg_tmp/fatzaudio-tmp/fa-abc123", "wb");
        CHECK(stale != NULL);
        fclose(stale);
        old.actime = old.modtime = time(NULL) - 3600;
        utime("cdg_tmp/fatzaudio-tmp/fa-abc123", &old);
    }
#endif
    cfg.temp_directory = "cdg_tmp";
    CHECK_EQ(fa_engine_create(&cfg, &e), FA_OK);
#ifndef _WIN32
    {
        FILE* gone = fopen("cdg_tmp/fatzaudio-tmp/fa-abc123", "rb");
        CHECK(gone == NULL);   /* swept at engine start */
    }
#endif
    CHECK_EQ(fa_stream_open_path(e, "cdg_big.zip", &o, &s), FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK_EQ(info.has_cdg, 1);
    CHECK_EQ(info.length_frames, 1500000);
    CHECK_EQ(info.file_size, (int64_t)zip.n);
    CHECK_EQ(decode_all(s), 1500000);
    CHECK_EQ(fa_stream_seek(s, 1000000), FA_OK);
    CHECK_EQ(decode_all(s), 500000);
#ifndef _WIN32
    {
        DIR* d = opendir("cdg_tmp/fatzaudio-tmp");
        struct dirent* ent;
        int files_left = 0;
        CHECK(d != NULL);
        while ((ent = readdir(d)) != NULL) if (ent->d_name[0] != '.') files_left++;
        closedir(d);
        CHECK_EQ(files_left, 0);   /* the name went right after it was created */
    }
#endif
    fa_stream_close(s);
    fa_engine_destroy(e);
    remove("cdg_big.zip");
    free(zip.p); free(wav.p); free(cdg.p);
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

/* The smooth scaler, worked out pixel by pixel from the window's colour indices
 * (a native indexed surface) and the frame's palette: bilinear on premultiplied
 * colour in 8-bit fixed point, pixel centres on pixel centres. */
static void smooth_reference(const fa_cdg_frame* ix, const fa_cdg_frame* fr, uint32_t flags, uint8_t* out)
{
    uint32_t pm[16][4], ox, oy, i, k, w = ix->width, h = ix->height;
    for (i = 0; i < 16; i++) {
        pm[i][3] = fr->palette[i][3];
        for (k = 0; k < 3; k++) pm[i][k] = (fr->palette[i][k] * pm[i][3] + 127) / 255;
    }
    for (oy = 0; oy < fr->height; oy++) {
        int64_t fy = ((int64_t)(2 * oy + 1) * h * 256) / (2 * fr->height) - 128;
        uint32_t y0, y1, ty;
        if (fy < 0) fy = 0;
        y0 = (uint32_t)(fy >> 8); ty = (uint32_t)(fy & 255);
        if (y0 >= h - 1) { y0 = h - 1; ty = 0; }
        y1 = y0 + 1 < h ? y0 + 1 : y0;
        for (ox = 0; ox < fr->width; ox++) {
            int64_t fx = ((int64_t)(2 * ox + 1) * w * 256) / (2 * fr->width) - 128;
            uint32_t x0, x1, tx, acc[4];
            uint8_t* p = out + ((size_t)oy * fr->width + ox) * 4;
            if (fx < 0) fx = 0;
            x0 = (uint32_t)(fx >> 8); tx = (uint32_t)(fx & 255);
            if (x0 >= w - 1) { x0 = w - 1; tx = 0; }
            x1 = x0 + 1 < w ? x0 + 1 : x0;
            for (k = 0; k < 4; k++) {
                uint32_t top = pm[*px(ix, x0, y0)][k] * (256 - tx) + pm[*px(ix, x1, y0)][k] * tx;
                uint32_t bottom = pm[*px(ix, x0, y1)][k] * (256 - tx) + pm[*px(ix, x1, y1)][k] * tx;
                acc[k] = (top * (256 - ty) + bottom * ty + 32768) >> 16;
            }
            if (!(flags & FA_CDG_PREMULTIPLIED)) {
                uint32_t a = acc[3];
                for (k = 0; k < 3; k++) {
                    if (a == 0) acc[k] = 0;
                    else if (a < 255) acc[k] = (acc[k] * 255 + a / 2) / a;
                    if (acc[k] > 255) acc[k] = 255;
                }
            }
            if (fr->pixel_format == FA_CDG_PIXELS_BGRA32) { p[0] = (uint8_t)acc[2]; p[1] = (uint8_t)acc[1]; p[2] = (uint8_t)acc[0]; }
            else { p[0] = (uint8_t)acc[0]; p[1] = (uint8_t)acc[1]; p[2] = (uint8_t)acc[2]; }
            p[3] = (uint8_t)acc[3];
        }
    }
}

static void test_smooth_matches_reference(void)
{
    static const uint32_t sizes[][2] = { { 1920, 1080 }, { 450, 324 }, { 301, 217 }, { 640, 200 }, { 37, 29 } };
    static const uint32_t flag_sets[] = { 0, FA_CDG_NO_BORDER, FA_CDG_TRANSPARENT, FA_CDG_TRANSPARENT | FA_CDG_PREMULTIPLIED,
                                          FA_CDG_BACKGROUND_COLOR | FA_CDG_BACKGROUND_OPACITY };
    buf c = { 0 };
    fa_stream* s = open_wav_stream(1, FA_OPEN_DECODE_ONLY);
    uint8_t* want = (uint8_t*)malloc((size_t)1920 * 1080 * 4);
    uint32_t seed = 7, i, f, z, step;
    int r, col;
    CHECK(s && want);
    colors(&c, 0, k_low);
    preset(&c, 1, 0);
    border(&c, 2);
    for (r = 0; r < 18; r++)        /* text-like rows and noise, so flat areas and edges */
        for (col = 0; col < 50; col++) {
            seed = seed * 1103515245u + 12345u;
            if (r % 3 == 0 && (seed >> 20) % 4) continue;
            tile(&c, (int)(seed >> 9) & 1, (int)(seed >> 12) & 7, (int)(seed >> 16) & 7, r, col, (uint8_t)((seed >> 22) & 0x3f));
        }
    transparent(&c, 5, 32);
    transparent(&c, 3, 50);
    step = (uint32_t)(c.n / FA_CDG_PACK_BYTES);   /* shown at t_packs(step) */
    scroll(&c, 0, 0, 0, 3, 0, 7);                 /* then smooth offsets 3 / 7 */
    CHECK_EQ(fa_stream_load_cdg_memory(s, c.p, c.n), FA_OK);
    for (z = 0; z < 2; z++)
        for (f = 0; f < sizeof(flag_sets) / sizeof(flag_sets[0]); f++)
            for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
                fa_cdg_surface *ix = NULL, *sm = NULL;
                fa_cdg_surface_config cfg;
                fa_cdg_frame fi, fs;
                uint32_t y;
                fa_cdg_surface_config_init(&cfg);
                cfg.flags = flag_sets[f];
                cfg.background_color = 0x80336699u;
                cfg.background_opacity = 60;
                cfg.pixel_format = FA_CDG_PIXELS_INDEXED8;
                CHECK_EQ(fa_cdg_surface_create(s, &cfg, &ix), FA_OK);
                cfg.flags |= FA_CDG_SMOOTH;
                cfg.width = sizes[i][0];
                cfg.height = sizes[i][1];
                cfg.pixel_format = (i & 1) ? FA_CDG_PIXELS_RGBA32 : FA_CDG_PIXELS_BGRA32;
                CHECK_EQ(fa_cdg_surface_create(s, &cfg, &sm), FA_OK);
                CHECK_EQ(fa_cdg_surface_update_at(ix, t_packs(step + z), &fi), FA_OK);
                CHECK_EQ(fa_cdg_surface_update_at(sm, t_packs(step + z), &fs), FA_OK);
                smooth_reference(&fi, &fs, cfg.flags, want);
                for (y = 0; y < fs.height; y++)
                    if (memcmp(px(&fs, 0, y), want + (size_t)y * fs.width * 4, (size_t)fs.width * 4) != 0) {
                        fprintf(stderr, "  %ux%u flags 0x%x offsets %u: row %u differs\n", fs.width, fs.height, cfg.flags, z, y);
                        CHECK(0);
                    }
                fa_cdg_surface_destroy(ix);
                fa_cdg_surface_destroy(sm);
            }
    fa_stream_close(s);
    free(want);
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
    t0 = fa_monotonic_ns();
    CHECK_EQ(fa_stream_load_cdg_memory(s, c.p, c.n), FA_OK);
    CHECK_EQ(fa_cdg_surface_create(s, NULL, &surf), FA_OK);
    printf("  loading a 5 minute song, checkpoints included: %.2f ms\n", (fa_monotonic_ns() - t0) / 1e6);
    t0 = fa_monotonic_ns();
    CHECK_EQ(fa_cdg_surface_update_at(surf, 300LL * 1000000000, &fr), FA_OK);
    printf("  first jump to the end: %.2f ms\n", (fa_monotonic_ns() - t0) / 1e6);
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


/* The decoder's own state in the frame, and the transparency options built on it. */
static void test_frame_state(void)
{
    buf c = { 0 };
    fa_stream* s = open_wav_stream(1, FA_OPEN_DECODE_ONLY);
    fa_cdg_surface *ix = NULL, *tr = NULL, *bg_only = NULL, *hide = NULL, *opts = NULL;
    fa_cdg_surface_config cfg;
    fa_cdg_frame fr;
    int i;
    colors(&c, 0, k_low);          /* pack 0 */
    transparent(&c, 1, 63);        /* 1: red invisible */
    border(&c, 3);                 /* 2: blue border */
    preset(&c, 2, 0);              /* 3: green screen */
    tile(&c, 0, 2, 1, 5, 5, 0x3F); /* 4: a red tile */
    CHECK_EQ(fa_stream_load_cdg_memory(s, c.p, c.n), FA_OK);
    fa_cdg_surface_config_init(&cfg);
    cfg.pixel_format = FA_CDG_PIXELS_INDEXED8;
    CHECK_EQ(fa_cdg_surface_create(s, &cfg, &ix), FA_OK);
    cfg.pixel_format = FA_CDG_PIXELS_BGRA32;
    cfg.flags = FA_CDG_TRANSPARENT;
    CHECK_EQ(fa_cdg_surface_create(s, &cfg, &tr), FA_OK);
    cfg.flags = FA_CDG_TRANSPARENT_BACKGROUND_ONLY;
    CHECK_EQ(fa_cdg_surface_create(s, &cfg, &bg_only), FA_OK);
    cfg.flags = FA_CDG_HIDE_UNTIL_PRESET;
    CHECK_EQ(fa_cdg_surface_create(s, &cfg, &hide), FA_OK);
    cfg.flags = FA_CDG_BACKGROUND_COLOR | FA_CDG_BACKGROUND_OPACITY;
    cfg.background_color = 0x80112233u;
    cfg.background_opacity = 50;
    CHECK_EQ(fa_cdg_surface_create(s, &cfg, &opts), FA_OK);

    /* Before any preset: no background, no border */
    CHECK_EQ(fa_cdg_surface_update_at(ix, t_packs(2), &fr), FA_OK);
    CHECK_EQ(fr.background_index, -1);
    CHECK_EQ(fr.border_index, -1);
    CHECK_EQ(fr.transparency[1], 63);
    CHECK_EQ(fr.transparency[0], 0);
    for (i = 0; i < 4; i++) CHECK_EQ(fr.background[i], 0);
    for (i = 0; i < 16; i++) CHECK_EQ(fr.palette[i][3], 255);  /* no flag: alpha untouched */
    CHECK_EQ(fa_cdg_surface_update_at(hide, t_packs(3), &fr), FA_OK);
    CHECK_EQ(fr.border_index, 3);
    CHECK_EQ(px(&fr, 0, 0)[3], 0);                 /* hidden, border included */
    CHECK_EQ(px(&fr, 150, 100)[3], 0);
    CHECK_EQ(fa_cdg_surface_update_at(opts, t_packs(3), &fr), FA_OK);
    for (i = 0; i < 4; i++) CHECK_EQ(fr.background[i], 0);   /* no background yet */

    /* After the preset */
    CHECK_EQ(fa_cdg_surface_update_at(ix, t_packs(5), &fr), FA_OK);
    CHECK_EQ(fr.background_index, 2);
    CHECK_EQ(fr.border_index, 3);
    CHECK(fr.background[0] == 0 && fr.background[1] == 255 && fr.background[2] == 0 && fr.background[3] == 255);
    for (i = 0; i < 16; i++) CHECK_EQ(fr.palette[i][3], 255);
    CHECK_EQ(fa_cdg_surface_update_at(tr, t_packs(5), &fr), FA_OK);
    CHECK_EQ(fr.palette[2][3], 0);                 /* background */
    CHECK_EQ(fr.palette[1][3], 0);                 /* transparent-colour pack */
    CHECK_EQ(fr.palette[1][0], 255);               /* RGB without alpha folded in */
    CHECK_EQ(fr.background[3], 0);
    CHECK_EQ(fa_cdg_surface_update_at(bg_only, t_packs(5), &fr), FA_OK);
    CHECK_EQ(fr.palette[2][3], 0);
    CHECK_EQ(fr.palette[1][3], 255);               /* the pack is ignored */
    CHECK_EQ(fr.transparency[1], 63);              /* but still reported */
    CHECK_EQ(px(&fr, 5 * 6 + 2, 5 * 12 + 2)[3], 255);  /* the red tile shows */
    CHECK_EQ(px(&fr, 150, 150)[3], 0);
    CHECK_EQ(fa_cdg_surface_update_at(hide, t_packs(5), &fr), FA_OK);
    CHECK(is_bgra(px(&fr, 150, 150), 0, 15, 0));   /* shown from the preset on */
    CHECK(is_bgra(px(&fr, 0, 0), 0, 15, 0));      /* the preset covers the border too */
    CHECK_EQ(fa_cdg_surface_update_at(opts, t_packs(5), &fr), FA_OK);
    CHECK(fr.palette[2][0] == 0x11 && fr.palette[2][1] == 0x22 && fr.palette[2][2] == 0x33 && fr.palette[2][3] == 64);
    CHECK(memcmp(fr.background, fr.palette[2], 4) == 0);
    CHECK_EQ(px(&fr, 150, 150)[3], 64);
    CHECK_EQ(px(&fr, 150, 150)[2], 0x11);          /* straight alpha: R in byte 2 */
    /* Seeking back before the preset brings -1 back */
    CHECK_EQ(fa_cdg_surface_update_at(ix, t_packs(3), &fr), FA_OK);
    CHECK_EQ(fr.background_index, -1);

    /* Bad or old configurations */
    {
        fa_cdg_surface* bad = NULL;
        cfg.background_opacity = 101;
        CHECK_EQ(fa_cdg_surface_create(s, &cfg, &bad), FA_ERROR_INVALID_ARG);
        cfg.flags = FA_CDG_TRANSPARENT;
        CHECK_EQ(fa_cdg_surface_create(s, &cfg, &bad), FA_OK);   /* ignored without its flag */
        fa_cdg_surface_destroy(bad);
        cfg.struct_size = 20;                                    /* built before the new fields */
        cfg.flags = FA_CDG_BACKGROUND_OPACITY;
        CHECK_EQ(fa_cdg_surface_create(s, &cfg, &bad), FA_OK);
        CHECK_EQ(fa_cdg_surface_update_at(bad, t_packs(5), &fr), FA_OK);
        CHECK_EQ(fr.palette[2][3], 0);                           /* opacity read as 0, not 101 */
        fa_cdg_surface_destroy(bad);
    }
    fa_cdg_surface_destroy(ix);
    fa_cdg_surface_destroy(tr);
    fa_cdg_surface_destroy(bg_only);
    fa_cdg_surface_destroy(hide);
    fa_cdg_surface_destroy(opts);
    /* Without graphics: -1 */
    CHECK_EQ(fa_stream_load_cdg_path(s, NULL), FA_OK);
    CHECK_EQ(fa_cdg_surface_create(s, NULL, &ix), FA_OK);
    CHECK_EQ(fa_cdg_surface_update_at(ix, t_packs(5), &fr), FA_OK);
    CHECK_EQ(fr.background_index, -1);
    fa_cdg_surface_destroy(ix);
    fa_stream_close(s);
    free(c.p);
}

/* Revisions: monotonic, pixels apart from colours, unchanged by updates that change nothing. */
static void test_revisions(void)
{
    buf c = { 0 };
    fa_stream* s = open_wav_stream(1, FA_OPEN_DECODE_ONLY);
    fa_cdg_surface* ix = NULL;
    fa_cdg_surface_config cfg;
    fa_cdg_frame fr;
    uint64_t rev, prev;
    static const uint8_t other[8][3] = { { 1, 1, 1 } };
    colors(&c, 0, k_low);          /* 0 */
    preset(&c, 0, 0);              /* 1 */
    tile(&c, 0, 0, 1, 2, 2, 0x3F); /* 2 */
    colors(&c, 0, other);          /* 3 */
    transparent(&c, 4, 10);        /* 4 */
    CHECK_EQ(fa_stream_load_cdg_memory(s, c.p, c.n), FA_OK);
    fa_cdg_surface_config_init(&cfg);
    cfg.pixel_format = FA_CDG_PIXELS_INDEXED8;
    CHECK_EQ(fa_cdg_surface_create(s, &cfg, &ix), FA_OK);
    CHECK_EQ(fa_cdg_surface_update_at(ix, t_packs(2), &fr), FA_OK);
    CHECK_EQ(fr.revision, 1);
    CHECK_EQ(fr.palette_revision, 1);
    CHECK_EQ(fa_cdg_surface_update_at(ix, t_packs(2), &fr), FA_OK);   /* nothing new */
    CHECK_EQ(fr.revision, 1);
    CHECK_EQ(fr.palette_revision, 1);
    CHECK_EQ(fr.changed, 0);
    CHECK_EQ(fa_cdg_surface_update_at(ix, t_packs(3), &fr), FA_OK);   /* a tile */
    CHECK_EQ(fr.revision, 2);
    CHECK_EQ(fr.palette_revision, 1);
    CHECK_EQ(fa_cdg_surface_update_at(ix, t_packs(4), &fr), FA_OK);   /* colours only */
    CHECK_EQ(fr.revision, 2);
    CHECK_EQ(fr.palette_revision, 2);
    CHECK_EQ(fa_cdg_surface_update_at(ix, t_packs(5), &fr), FA_OK);   /* transparency only */
    CHECK_EQ(fr.revision, 2);
    CHECK_EQ(fr.palette_revision, 3);
    /* A seek back never lowers them */
    prev = fr.palette_revision;
    CHECK_EQ(fa_cdg_surface_update_at(ix, t_packs(2), &fr), FA_OK);
    CHECK_EQ(fr.revision, 3);                     /* the tile went away */
    CHECK(fr.palette_revision > prev);
    rev = fr.revision;
    CHECK_EQ(fa_cdg_surface_update_at(ix, t_packs(1), &fr), FA_OK);   /* colours, no preset yet */
    CHECK_EQ(fr.revision, rev);                    /* screen still all 0 */
    CHECK_EQ(fr.background_index, -1);
    fa_cdg_surface_destroy(ix);
    fa_stream_close(s);
    free(c.p);
}

/* Song-length graphics: checkpoints at load, so the first far jump replays at most
 * 10 s, and the picture equals decoding from the start. */
static void test_checkpoints(void)
{
    buf c = { 0 };
    fa_stream* s = open_wav_stream(1, FA_OPEN_DECODE_ONLY);
    fa_cdg_surface *jump = NULL, *walk = NULL;
    fa_cdg_info ci;
    fa_cdg_frame fr, fr2;
    int64_t i, t;
    CHECK_EQ(fa_stream_get_cdg_info(s, &ci), FA_OK);
    CHECK_EQ(ci.source, FA_CDG_SOURCE_NONE);
    CHECK_EQ(ci.disc_parity, -1);
    colors(&c, 0, k_low);
    for (i = 1; i < 300 * 120; i++) {   /* 2 minutes */
        if (i % 2000 == 0) preset(&c, (int)(i / 2000) % 6, 0);
        else if (i % 7 == 0) scroll(&c, (int)(i / 7) & 1, 2, (int)(i % 3), (int)(i % 6), (int)(i % 3), (int)(i % 12));
        else tile(&c, (int)(i & 1), 1, 4, (int)(i % 18), (int)(i % 50), (uint8_t)i);
    }
    CHECK_EQ(fa_stream_load_cdg_memory(s, c.p, c.n), FA_OK);
    CHECK_EQ(fa_stream_get_cdg_info(s, &ci), FA_OK);
    CHECK_EQ(ci.source, FA_CDG_SOURCE_FILE);
    CHECK_EQ(ci.pack_count, 300 * 120);
    CHECK_EQ(ci.checkpoint_interval, 10000000000LL);
    CHECK_EQ(ci.checkpoint_count, 0);           /* built for the first surface */
    CHECK_EQ(fa_cdg_surface_create(s, NULL, &jump), FA_OK);
    CHECK_EQ(fa_stream_get_cdg_info(s, &ci), FA_OK);
    CHECK_EQ(ci.checkpoint_count, 12);
    CHECK_EQ(fa_cdg_surface_create(s, NULL, &walk), FA_OK);
    /* Jumps all over, compared with a surface that only walks forward from the start */
    for (t = 0; t < 12; t++) {
        fa_time at = (fa_time)((t * 7919) % 119 + 1) * 1000000000 + t_packs(t);
        fa_time step;
        CHECK_EQ(fa_cdg_surface_update_at(jump, at, &fr), FA_OK);
        CHECK_EQ(fa_cdg_surface_update_at(walk, 0, &fr2), FA_OK);
        for (step = 0; step < at; step += 3000000000LL)
            CHECK_EQ(fa_cdg_surface_update_at(walk, step, &fr2), FA_OK);
        CHECK_EQ(fa_cdg_surface_update_at(walk, at, &fr2), FA_OK);
        CHECK(memcmp(fr.pixels, fr2.pixels, (size_t)fr.stride * fr.height) == 0);
        CHECK(memcmp(fr.palette, fr2.palette, sizeof(fr.palette)) == 0);
    }
    fa_cdg_surface_destroy(jump);
    fa_cdg_surface_destroy(walk);
    free(c.p);

    /* Graphics over 30 minutes space their checkpoints further apart */
    c.p = NULL; c.n = c.cap = 0;
    empty_packs_to(&c, (size_t)300 * 60 * 31);
    CHECK_EQ(fa_cdg_surface_create(s, NULL, &jump), FA_OK);
    CHECK_EQ(fa_stream_load_cdg_memory(s, c.p, c.n), FA_OK);
    CHECK_EQ(fa_cdg_surface_update_at(jump, 0, &fr), FA_OK);   /* a load under a surface */
    CHECK_EQ(fa_stream_get_cdg_info(s, &ci), FA_OK);
    CHECK_EQ(ci.checkpoint_interval, 20000000000LL);
    CHECK_EQ(ci.checkpoint_count, 93);
    fa_cdg_surface_destroy(jump);
    fa_stream_close(s);
    free(c.p);
}

/* Disc-style graphics written while they play: checkpoints follow the writes and are
 * redone when a part already covered is rewritten with different packs. */
static void test_checkpoints_streamed(void)
{
    buf c = { 0 };
    fa_stream* s = open_wav_stream(1, FA_OPEN_DECODE_ONLY);
    fa_cdg_surface *surf = NULL, *ref = NULL;
    fa_cdg_data* blank = NULL;
    fa_cdg_info ci;
    fa_cdg_frame fr, fr2;
    uint32_t token = 0;
    size_t i, n = 300 * 40;
    fa_stream* r = open_wav_stream(1, FA_OPEN_DECODE_ONLY);
    colors(&c, 0, k_low);
    for (i = 1; i < n; i++) {
        if (i % 2500 == 0) preset(&c, (int)(i / 2500) % 6, 0);
        else tile(&c, 0, 1, 4, (int)(i % 18), (int)(i % 50), (uint8_t)i);
    }
    CHECK_EQ(fa_cdg_data_blank(n, &blank), FA_OK);
    blank->from_disc = 1;
    CHECK(fa_stream_cdg_attach_if_none(s, blank, &token));
    CHECK_EQ(fa_cdg_surface_create(s, NULL, &surf), FA_OK);
    for (i = 0; i < n / 2; i += 64)      /* the first 20 s arrive */
        CHECK(fa_stream_cdg_write(s, token, i, c.p + i * FA_CDG_PACK_BYTES, i + 64 <= n ? 64 : n - i));
    CHECK_EQ(fa_stream_get_cdg_info(s, &ci), FA_OK);
    CHECK_EQ(ci.source, FA_CDG_SOURCE_DISC);
    CHECK_EQ(ci.checkpoint_count, 2);
    CHECK_EQ(ci.disc_parity, 0);
    /* A seek: 30..40 s arrive, then the gap is filled in */
    for (i = 300 * 30; i < n; i += 64)
        CHECK(fa_stream_cdg_write(s, token, i, c.p + i * FA_CDG_PACK_BYTES, i + 64 <= n ? 64 : n - i));
    CHECK_EQ(fa_stream_get_cdg_info(s, &ci), FA_OK);
    CHECK_EQ(ci.checkpoint_count, 4);           /* taken over the blank gap for now */
    CHECK_EQ(fa_cdg_surface_update_at(surf, 35000000000LL, &fr), FA_OK);
    for (i = n / 2; i < 300 * 30; i += 64)
        CHECK(fa_stream_cdg_write(s, token, i, c.p + i * FA_CDG_PACK_BYTES, 64));
    CHECK_EQ(fa_stream_get_cdg_info(s, &ci), FA_OK);
    CHECK_EQ(ci.checkpoint_count, 4);           /* redone with the real packs */
    /* Re-reading identical packs changes nothing */
    CHECK(fa_stream_cdg_write(s, token, 0, c.p, 64));
    CHECK_EQ(fa_stream_get_cdg_info(s, &ci), FA_OK);
    CHECK_EQ(ci.checkpoint_count, 4);
    /* Same pictures as the whole file loaded at once */
    CHECK_EQ(fa_stream_load_cdg_memory(r, c.p, n * FA_CDG_PACK_BYTES), FA_OK);
    CHECK_EQ(fa_cdg_surface_create(r, NULL, &ref), FA_OK);
    for (i = 0; i < 5; i++) {
        fa_time at = (fa_time)(39 - i * 8) * 1000000000 + 12345;
        CHECK_EQ(fa_cdg_surface_update_at(surf, at, &fr), FA_OK);
        CHECK_EQ(fa_cdg_surface_update_at(ref, at, &fr2), FA_OK);
        CHECK(memcmp(fr.pixels, fr2.pixels, (size_t)fr.stride * fr.height) == 0);
    }
    fa_cdg_surface_destroy(surf);
    fa_cdg_surface_destroy(ref);
    fa_stream_close(s);
    fa_stream_close(r);
    free(c.p);
}

/* The documented timing rule: packs [0, ceil(t * 300)) at t. */
static void test_timing_rule(void)
{
    buf c = { 0 };
    fa_stream* s = open_wav_stream(2, FA_OPEN_DECODE_ONLY);
    fa_cdg_surface* ix = NULL;
    fa_cdg_surface_config cfg;
    fa_cdg_frame fr;
    colors(&c, 0, k_low);
    empty_packs_to(&c, 299);
    preset(&c, 1, 0);              /* pack 299 */
    preset(&c, 2, 0);              /* pack 300 */
    CHECK_EQ(fa_stream_load_cdg_memory(s, c.p, c.n), FA_OK);
    fa_cdg_surface_config_init(&cfg);
    cfg.pixel_format = FA_CDG_PIXELS_INDEXED8;
    CHECK_EQ(fa_cdg_surface_create(s, &cfg, &ix), FA_OK);
    CHECK_EQ(fa_cdg_surface_update_at(ix, 1000000000LL - 3333334, &fr), FA_OK);
    CHECK_EQ(fr.background_index, -1);             /* pack 299 starts at 0.99666.. s */
    CHECK_EQ(fa_cdg_surface_update_at(ix, 1000000000LL - 3333333, &fr), FA_OK);
    CHECK_EQ(fr.background_index, 1);
    CHECK_EQ(fa_cdg_surface_update_at(ix, 1000000000LL, &fr), FA_OK);
    CHECK_EQ(fr.background_index, 1);              /* 1 s: packs 0..299 */
    CHECK_EQ(fa_cdg_surface_update_at(ix, 1000000001LL, &fr), FA_OK);
    CHECK_EQ(fr.background_index, 2);              /* 1 s + 1 ns: pack 300 too */
    /* The offset shifts the timeline */
    CHECK_EQ(fa_stream_set_cdg_offset(s, 500000000), FA_OK);
    CHECK_EQ(fa_cdg_surface_update_at(ix, 1500000000LL, &fr), FA_OK);
    CHECK_EQ(fr.background_index, 1);
    CHECK_EQ(fr.time, 1000000000LL);
    fa_cdg_surface_destroy(ix);
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

/* Push model: FA_EVENT_CDG_CHANGED follows the clock and says what changed. */
static fa_event g_cdg_events[64];
static fa_atomic32 g_cdg_event_count;

static void FA_CALL on_cdg_event(const fa_event* ev, void* user)
{
    int32_t n = fa_load32(&g_cdg_event_count);
    (void)user;
    if (ev->type != FA_EVENT_CDG_CHANGED || n >= 64) return;
    g_cdg_events[n] = *ev;
    fa_store32(&g_cdg_event_count, n + 1);
}

/* Waits for event number `n` (0-based); NULL after 3 s. */
static const fa_event* cdg_event(int32_t n)
{
    int64_t deadline = fa_monotonic_ns() + 3000000000LL;
    while (fa_load32(&g_cdg_event_count) <= n && fa_monotonic_ns() < deadline) fa_sleep_ms(2);
    return fa_load32(&g_cdg_event_count) > n ? &g_cdg_events[n] : NULL;
}

static void read_to(fa_stream* s, fa_frames frame)
{
    float pcm[2 * 1024];
    fa_frames pos = 0;
    while (fa_stream_get_position(s, &pos) == FA_OK && pos < frame) {
        fa_frames got = 0, want = frame - pos < 1024 ? frame - pos : 1024;
        if (fa_stream_read(s, pcm, want, &got) != FA_OK || got == 0) break;
    }
}

static void test_change_events(void)
{
    buf c = { 0 };
    fa_stream* s = open_wav_stream(3, FA_OPEN_DECODE_ONLY);
    const fa_event* ev;
    int64_t v;
    static const uint8_t other[8][3] = { { 1, 2, 3 } };
    colors(&c, 0, k_low);          /* pack 0 */
    preset(&c, 2, 0);              /* 1 */
    empty_packs_to(&c, 300);
    tile(&c, 0, 2, 1, 5, 5, 0x3F); /* 300: at 1 s */
    empty_packs_to(&c, 600);
    colors(&c, 0, other);          /* 600: at 2 s */
    empty_packs_to(&c, 900);
    CHECK(s);
    CHECK_EQ(fa_stream_load_cdg_memory(s, c.p, c.n), FA_OK);
    fa_store32(&g_cdg_event_count, 0);
    CHECK_EQ(fa_stream_set_event_callback(s, on_cdg_event, NULL), FA_OK);
    CHECK_EQ(fa_stream_set_cdg_events(s, 1), FA_OK);
    /* The first event asks for the whole picture */
    CHECK((ev = cdg_event(0)) != NULL);
    v = ev->value;
    CHECK(FA_CDG_CHANGE_X(v) == 0 && FA_CDG_CHANGE_Y(v) == 0);
    CHECK(FA_CDG_CHANGE_WIDTH(v) == 300 && FA_CDG_CHANGE_HEIGHT(v) == 216);
    CHECK_EQ(FA_CDG_CHANGE_PALETTE(v), 1);
    CHECK_EQ(ev->stream, s);
    /* Colours and the clear */
    read_to(s, 4000);
    CHECK((ev = cdg_event(1)) != NULL);
    CHECK_EQ(ev->frame, 150);
    CHECK_EQ(FA_CDG_CHANGE_WIDTH(ev->value), 300);
    CHECK_EQ(FA_CDG_CHANGE_PALETTE(ev->value), 1);
    /* One tile: just its area */
    read_to(s, 8100);
    CHECK((ev = cdg_event(2)) != NULL);
    v = ev->value;
    CHECK_EQ(FA_CDG_CHANGE_X(v), 30);
    CHECK_EQ(FA_CDG_CHANGE_Y(v), 60);
    CHECK_EQ(FA_CDG_CHANGE_WIDTH(v), 6);
    CHECK_EQ(FA_CDG_CHANGE_HEIGHT(v), 12);
    CHECK_EQ(FA_CDG_CHANGE_PALETTE(v), 0);
    /* Nothing new while the clock stands still */
    fa_sleep_ms(30);
    CHECK_EQ(fa_load32(&g_cdg_event_count), 3);
    /* A colour change only */
    read_to(s, 16100);
    CHECK((ev = cdg_event(3)) != NULL);
    CHECK_EQ(FA_CDG_CHANGE_WIDTH(ev->value), 0);
    CHECK_EQ(FA_CDG_CHANGE_PALETTE(ev->value), 1);
    /* A seek back: the whole screen */
    CHECK_EQ(fa_stream_seek(s, 0), FA_OK);
    CHECK((ev = cdg_event(4)) != NULL);
    CHECK_EQ(FA_CDG_CHANGE_WIDTH(ev->value), 300);
    CHECK_EQ(ev->frame, 0);
    /* Off: no more */
    CHECK_EQ(fa_stream_set_cdg_events(s, 0), FA_OK);
    read_to(s, 8100);
    fa_sleep_ms(30);
    CHECK_EQ(fa_load32(&g_cdg_event_count), 5);
    CHECK_EQ(fa_stream_set_cdg_events(NULL, 1), FA_ERROR_INVALID_ARG);
    /* Closing a stream with events on takes it off the watch list */
    CHECK_EQ(fa_stream_set_cdg_events(s, 1), FA_OK);
    CHECK(cdg_event(5) != NULL);
    fa_stream_close(s);
    fa_sleep_ms(30);
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
    RUN(test_inflate_streaming);
    RUN(test_zip_lean);
    RUN(test_zip_over_limit);
    RUN(test_zip64_basic);
    RUN(test_zip64_many_entries);
    RUN(test_zip64_over_4gb);
    RUN(test_zip64_damaged);
    RUN(test_decoder_basics);
    RUN(test_xor_and_repeats);
    RUN(test_scrolling);
    RUN(test_surface_options);
    RUN(test_smooth_matches_reference);
    RUN(test_extreme_times);
    RUN(test_palette_changes);
    RUN(test_offset_and_replace);
    RUN(test_sibling_file);
    RUN(test_replay_speed);
    RUN(test_follows_audio_clock);
    RUN(test_frame_state);
    RUN(test_revisions);
    RUN(test_checkpoints);
    RUN(test_checkpoints_streamed);
    RUN(test_timing_rule);
    RUN(test_change_events);
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
