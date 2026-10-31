#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L /* nanosleep under strict C11 */
#endif
/* CD+G tests: the pack decoder, surfaces (formats, scaling, borders, transparency,
 * dirty rectangles), timing and the audio clock, MP3+G style pairs, ZIP archives
 * and the DEFLATE decoder behind them. Every input is generated here. */
#include "cdg.h"
#include "cdg_contour.h"
#include "surface.h"
#include "surface_fx.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__)
#  include <unistd.h>
#endif
#include <time.h>
#ifndef _WIN32
#  include <dirent.h>
#  include <utime.h>
#endif

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; return; } } while (0)
#define CHECK_EQ(a, b) do { long long va_ = (long long)(a), vb_ = (long long)(b); if (va_ != vb_) { \
    fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_failures++; return; } } while (0)
/* Sanitizer builds run some expensive checks on a sample only (CI time). */
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#  define FA_TEST_SANITIZED 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#    define FA_TEST_SANITIZED 1
#  endif
#endif
#ifndef FA_TEST_SANITIZED
#  define FA_TEST_SANITIZED 0
#endif
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
        CHECK_EQ(fa_zip_extract(&zs, &e, UINT64_MAX, NULL, &out), FA_ERROR_UNSUPPORTED_FORMAT);
        CHECK(out == NULL);
        /* Past the caller's limit: too large, also before allocating */
        e.size = 2000;
        CHECK_EQ(fa_zip_extract(&zs, &e, 1000, NULL, &out), FA_ERROR_TOO_LARGE);
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
    CHECK_EQ(fa_zip_extract_file(&zs, &e[0], NULL, tmp), FA_OK);
    CHECK_EQ(ftell(tmp), (long)wav.n);
    rewind(tmp);
    out = (uint8_t*)malloc(wav.n);
    CHECK_EQ(fread(out, 1, wav.n, tmp), wav.n);
    CHECK(memcmp(out, wav.p, wav.n) == 0);
    free(out);
    out = NULL;
    /* And into memory from the file source */
    CHECK_EQ(fa_zip_extract(&zs, &e[0], wav.n, NULL, &out), FA_OK);
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
    CHECK(fa_zip_extract(&zs, &e[0], wav.n, NULL, &out) != FA_OK);
    CHECK(out == NULL);
    free(e);
    free(zip.p); free(z.p); free(wav.p);
    remove("cdg_stream.zip");
}

/* App callbacks over a memory block, counting closes. */
typedef struct mem_io {
    const buf* b; int64_t pos; int closes; int seekable;
    int64_t read_bytes;               /* all bytes handed out */
    int64_t cancel_after;             /* sets *flag once this many have been (0 = never) */
    volatile int32_t* flag;
} mem_io;
static int64_t FA_CALL mio_read(void* u, void* dst, size_t n)
{
    mem_io* m = (mem_io*)u;
    size_t left = (size_t)((int64_t)m->b->n - m->pos);
    if (n > left) n = left;
    memcpy(dst, m->b->p + m->pos, n);
    m->pos += (int64_t)n;
    m->read_bytes += (int64_t)n;
    if (m->flag && m->cancel_after && m->read_bytes >= m->cancel_after) *m->flag = 1;
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


/* ---- ZIP leftovers: prefix probe, cancel, temp-file songs, stored-entry CRC -------------- */

/* Lower-case letters from a generator: no format claims them, and stored blocks keep them
 * at their size. */
static buf make_text_junk(size_t n)
{
    buf b = { 0 };
    uint32_t seed = 99;
    size_t i;
    for (i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; put8(&b, 'a' + ((seed >> 16) % 26)); }
    return b;
}

/* A WAV with a title in its LIST chunk. */
static buf make_titled_wav(uint32_t frames, const char* title)
{
    buf b = { 0 };
    uint32_t i, data = frames * 2, tl = (uint32_t)strlen(title) + 1, list = 4 + 8 + ((tl + 1) & ~1u);
    put(&b, "RIFF", 4); le32(&b, 4 + 24 + 8 + list + 8 + data); put(&b, "WAVE", 4);
    put(&b, "fmt ", 4); le32(&b, 16); le16(&b, 1); le16(&b, 1);
    le32(&b, 44100); le32(&b, 88200); le16(&b, 2); le16(&b, 16);
    put(&b, "LIST", 4); le32(&b, list); put(&b, "INFO", 4);
    put(&b, "INAM", 4); le32(&b, tl); put(&b, title, tl);
    if (tl & 1) put8(&b, 0);
    put(&b, "data", 4); le32(&b, data);
    for (i = 0; i < frames; i++) le16(&b, (uint16_t)(int16_t)(sin(i * 6.283185307 / 160.0) * 8000.0));
    return b;
}

static int count_temp_files(void)
{
    int n = 0;
#ifndef _WIN32
    DIR* d = opendir("cdg_tmp/fatzaudio-tmp");
    struct dirent* ent;
    if (!d) return 0;
    while ((ent = readdir(d)) != NULL) if (ent->d_name[0] != '.') n++;
    closedir(d);
#endif
    return n;
}

/* A bundled file that is no audio costs a 64 KB prefix, not its unpacking. */
static void test_zip_prefix_probe(void)
{
    buf junk = make_text_junk(4 << 20), wav = make_wav(8000, 8000), zip;
    zip_file files[2];
    fa_open_options o;
    fa_stream* s = NULL;
    fa_io_callbacks io;
    mem_io m;
    fa_tags* t = NULL;
    files[0].name = "Notes.bin"; files[0].data = &junk; files[0].deflate = 1; files[0].crc_xor = 0;
    files[1].name = "Song.wav"; files[1].data = &wav; files[1].deflate = 1; files[1].crc_xor = 0;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    memset(&io, 0, sizeof(io));
    io.read = mio_read; io.seek = mio_seek; io.length = mio_length; io.close = mio_close;

    /* Only the junk: refused after the prefix (the archive is 4 MB) */
    zip = make_zip(files, 1);
    memset(&m, 0, sizeof(m));
    m.b = &zip; m.seekable = 1;
    CHECK_EQ(fa_stream_open_io(g_engine, &io, &m, &o, &s), FA_ERROR_UNSUPPORTED_FORMAT);
    CHECK(s == NULL);
    CHECK(m.read_bytes < 300000);
    CHECK_EQ(fa_tags_read_memory(zip.p, zip.n, 0, &t), FA_OK);   /* no tags, no unpack of 4 MB */
    fa_tags_close(t);
    free(zip.p);

    /* The junk first, then the song: the song plays, the junk is skipped */
    zip = make_zip(files, 2);
    memset(&m, 0, sizeof(m));
    m.b = &zip; m.seekable = 1;
    CHECK_EQ(fa_stream_open_io(g_engine, &io, &m, &o, &s), FA_OK);
    CHECK_EQ(decode_all(s), 8000);
    CHECK(m.read_bytes < 4000000);   /* the 4 MB entry was not read through */
    fa_stream_close(s);
    free(zip.p); free(junk.p); free(wav.p);
}

/* The cancel flag stops the unpacking of a compressed song, into memory and into a temp file. */
static void test_zip_cancel(void)
{
    buf wav = make_long_wav(1500000), zip;   /* 3 MB, stored blocks: about the same compressed */
    zip_file f;
    fa_open_options o;
    fa_stream* s = NULL;
    fa_io_callbacks io;
    mem_io m;
    fa_engine_config cfg;
    fa_engine* e = NULL;
    volatile int32_t flag = 0;
    int pass;
    f.name = "Big.wav"; f.data = &wav; f.deflate = 1; f.crc_xor = 0;
    zip = make_zip(&f, 1);
    write_file("cdg_cancel.zip", &zip);
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    o.cancel = &flag;
    memset(&io, 0, sizeof(io));
    io.read = mio_read; io.seek = mio_seek; io.length = mio_length; io.close = mio_close;
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    cfg.zip_memory_limit_mb = 1;
    cfg.temp_directory = "cdg_tmp";
    CHECK_EQ(fa_engine_create(&cfg, &e), FA_OK);

    /* Already set: nothing is unpacked */
    flag = 1;
    CHECK_EQ(fa_stream_open_path(e, "cdg_cancel.zip", &o, &s), FA_ERROR_CANCELLED);
    CHECK(s == NULL);
    CHECK_EQ(fa_stream_open_path(g_engine, "cdg_cancel.zip", &o, &s), FA_ERROR_CANCELLED);
    CHECK(s == NULL);

    /* Set by the app's callbacks after 1 MB: into memory (default engine), then a temp file */
    for (pass = 0; pass < 2; pass++) {
        fa_engine* eng = pass ? e : g_engine;
        flag = 0;
        memset(&m, 0, sizeof(m));
        m.b = &zip; m.seekable = 1; m.flag = &flag; m.cancel_after = 1 << 20;
        CHECK_EQ(fa_stream_open_io(eng, &io, &m, &o, &s), FA_ERROR_CANCELLED);
        CHECK(s == NULL);
        CHECK(m.read_bytes < (2 << 20));   /* stopped well before the 3 MB end */
        CHECK_EQ(m.closes, 0);             /* a failed open leaves the callbacks to the app */
        CHECK_EQ(count_temp_files(), 0);
    }

    /* Left clear, the same open plays */
    flag = 0;
    CHECK_EQ(fa_stream_open_path(e, "cdg_cancel.zip", &o, &s), FA_OK);
    CHECK_EQ(decode_all(s), 1500000);
    fa_stream_close(s);
    fa_engine_destroy(e);
    remove("cdg_cancel.zip");
    free(zip.p); free(wav.p);
}

static fa_atomic32 g_crc_events;
static fa_atomic32 g_crc_value;
static void FA_CALL on_crc_event(const fa_event* ev, void* user)
{
    (void)user;
    if (ev->type != FA_EVENT_DECODE_ERROR) return;
    fa_store32(&g_crc_value, (int32_t)ev->value);
    fa_fetch_add32(&g_crc_events, 1);
}

/* A song unpacked into a temp file gets tags, a loudness scan and analysis, like any file. */
static void test_zip_temp_song(void)
{
    buf wav = make_titled_wav(700000, "Temp Title"), zip;
    zip_file f;
    fa_engine_config cfg;
    fa_engine* e = NULL;
    fa_open_options o;
    fa_stream* s = NULL;
    fa_tags* t = NULL;
    fa_loudness l;
    fa_analysis* a = NULL;
    int64_t deadline;
    f.name = "Temp.wav"; f.data = &wav; f.deflate = 1; f.crc_xor = 0;
    zip = make_zip_lz(&f, 1);
    write_file("cdg_temp.zip", &zip);
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    cfg.zip_memory_limit_mb = 1;
    cfg.temp_directory = "cdg_tmp";
    CHECK(wav.n > (1u << 20));
    CHECK_EQ(fa_engine_create(&cfg, &e), FA_OK);
    fa_open_options_init(&o);
    CHECK_EQ(fa_stream_open_path(e, "cdg_temp.zip", &o, &s), FA_OK);
    CHECK_EQ(fa_stream_get_tags(s, 0, &t), FA_OK);
    CHECK(fa_tags_get(t, FA_TAG_TITLE) != NULL);
    CHECK(strcmp(fa_tags_get(t, FA_TAG_TITLE), "Temp Title") == 0);
    fa_tags_close(t);
    deadline = fa_monotonic_ns() + 10000000000LL;
    do {
        fa_sleep_ms(20);
        CHECK_EQ(fa_stream_get_loudness(s, &l), FA_OK);
    } while (!l.is_final && fa_monotonic_ns() < deadline);
    CHECK_EQ(l.is_final, 1);
    CHECK_EQ(l.source, FA_LOUDNESS_SCAN);
    CHECK(l.integrated_lufs < 0.0);
    CHECK_EQ(fa_analyze_stream(s, NULL, &a), FA_OK);
    fa_analysis_free(a);
    CHECK_EQ(decode_all(s), 700000);
    fa_stream_close(s);
    CHECK_EQ(count_temp_files(), 0);
    fa_engine_destroy(e);
    remove("cdg_temp.zip");
    free(zip.p); free(wav.p);
}

/* A stored entry is checked against its CRC while the stream plays; a mismatch is reported once. */
static void test_zip_stored_crc(void)
{
    buf wav = make_wav(44100, 44100), zip;
    zip_file f;
    fa_open_options o;
    fa_stream* s = NULL;
    fa_device* dev = NULL;
    int bad, from_memory;
    f.name = "Song.wav"; f.data = &wav; f.deflate = 0;
    CHECK_EQ(fa_device_open(g_engine, NULL, &dev), FA_OK);
    CHECK_EQ(fa_device_start(dev), FA_OK);
    fa_open_options_init(&o);
    for (bad = 0; bad < 2; bad++) {
        for (from_memory = 0; from_memory < 2; from_memory++) {
            int64_t deadline;
            f.crc_xor = bad ? 0x1234 : 0;
            zip = make_zip(&f, 1);
            write_file("cdg_crc.zip", &zip);
            fa_store32(&g_crc_events, 0);
            if (from_memory) CHECK_EQ(fa_stream_open_memory(g_engine, zip.p, zip.n, &o, &s), FA_OK);
            else CHECK_EQ(fa_stream_open_path(g_engine, "cdg_crc.zip", &o, &s), FA_OK);
            CHECK_EQ(fa_stream_set_event_callback(s, on_crc_event, NULL), FA_OK);
            CHECK_EQ(fa_stream_attach(s, dev, NULL), FA_OK);
            CHECK_EQ(fa_stream_play(s), FA_OK);
            deadline = fa_monotonic_ns() + 5000000000LL;
            while (bad && fa_load32(&g_crc_events) == 0 && fa_monotonic_ns() < deadline) fa_sleep_ms(10);
            if (!bad) fa_sleep_ms(300);   /* long enough for the check to have ended */
            fa_sleep_ms(100);
            CHECK_EQ(fa_load32(&g_crc_events), bad ? 1 : 0);
            if (bad) CHECK_EQ(fa_load32(&g_crc_value), FA_ERROR_UNSUPPORTED_FORMAT);
            fa_stream_close(s);
            free(zip.p);
        }
    }
    fa_device_close(dev);
    remove("cdg_crc.zip");
    free(wav.p);
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
    preset(&c, 1, 1);              /* 5: a copy of the clear at pack 1, after drawing */
    border(&c, 3);                 /* 6 */
    preset(&c, 1, 2);              /* 7: another copy, after a border */
    preset(&c, 1, 1);              /* 8: a count that goes back is a new clear */
    tile(&c, 0, 3, 4, 1, 1, 0x20); /* 9 */
    empty_packs_to(&c, 400);
    preset(&c, 1, 3);              /* 400: over a second after the last clear: a new one */
    preset(&c, 6, 1);              /* 401: another colour is always a new clear */
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
    /* Repeats are copies of one clear (in case one is damaged on a disc): drawing
     * between the copies stays. */
    CHECK_EQ(fa_cdg_surface_update_at(surf, t_packs(6), &fr), FA_OK);
    CHECK_EQ(*px(&fr, 6, 12), 5);
    CHECK_EQ(*px(&fr, 0, 0), 2);
    CHECK_EQ(fa_cdg_surface_update_at(surf, t_packs(8), &fr), FA_OK);
    CHECK_EQ(*px(&fr, 6, 12), 5);
    CHECK_EQ(*px(&fr, 0, 0), 3);
    CHECK_EQ(fa_cdg_surface_update_at(surf, t_packs(9), &fr), FA_OK);
    CHECK_EQ(*px(&fr, 6, 12), 1);
    CHECK_EQ(*px(&fr, 0, 0), 1);
    CHECK_EQ(fa_cdg_surface_update_at(surf, t_packs(400), &fr), FA_OK);
    CHECK_EQ(*px(&fr, 6, 12), 4);
    CHECK_EQ(fa_cdg_surface_update_at(surf, t_packs(401), &fr), FA_OK);
    CHECK_EQ(*px(&fr, 6, 12), 1);
    CHECK_EQ(*px(&fr, 0, 0), 1);
    CHECK_EQ(fa_cdg_surface_update_at(surf, t_packs(402), &fr), FA_OK);
    CHECK_EQ(*px(&fr, 6, 12), 6);
    /* The same after a seek back through the checkpoints' start. */
    CHECK_EQ(fa_cdg_surface_update_at(surf, t_packs(8), &fr), FA_OK);
    CHECK_EQ(*px(&fr, 6, 12), 5);
    CHECK_EQ(*px(&fr, 0, 0), 3);
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

/* A surface updated on one thread while its stream is closed on another: updates
 * stop with FA_ERROR_INVALID_ARG and never touch the closed stream (ASan/TSan). */
typedef struct race_arg {
    fa_cdg_surface* surf;
    fa_atomic32 started;
    int64_t updates;
    fa_result last;              /* checked on the main thread */
} race_arg;

/* A song with something of every kind: tiles far apart (several dirty rectangles),
 * XOR tiles, colour and transparency changes, a border, scrolls and smooth-scroll
 * offsets, and a second screen clear. */
static buf busy_cdg(void)
{
    static const uint8_t other[8][3] = { { 0, 0, 0 }, { 15, 8, 0 }, { 0, 15, 8 }, { 8, 0, 15 },
                                         { 15, 15, 0 }, { 4, 4, 4 }, { 0, 9, 9 }, { 9, 0, 9 } };
    buf c = { 0 };
    int i;
    colors(&c, 0, k_low);
    colors(&c, 1, other);
    preset(&c, 1, 0);
    border(&c, 2);
    for (i = 0; i < 40; i++) tile(&c, i % 5 == 4, i % 16, (i * 7) % 16, (i * 5) % 18, (i * 13) % 50, (uint8_t)(0x15 + i));
    tile(&c, 0, 3, 4, 0, 0, 0x2A);
    tile(&c, 0, 4, 3, 17, 49, 0x15);
    colors(&c, 0, other);
    transparent(&c, 3, 40);
    for (i = 0; i < 10; i++) tile(&c, 1, 5, 6, 3 + i, 10 + 3 * i, 0x3F);
    scroll(&c, 1, 0, 2, 0, 0, 0);
    scroll(&c, 0, 4, 0, 0, 1, 0);
    scroll(&c, 0, 0, 0, 3, 0, 5);
    for (i = 0; i < 6; i++) tile(&c, 0, 7, 8, 8, 20 + i, 0x0F);
    scroll(&c, 0, 0, 0, 0, 0, 0);
    colors(&c, 0, k_low);
    preset(&c, 9, 0);
    for (i = 0; i < 12; i++) tile(&c, 0, i, 15 - i, 2 + i, 40 - i * 3, 0x33);
    return c;
}

/* update_into gives exactly fa_cdg_surface_update's pixels in every configuration,
 * drawing only what changed into buffers used in rotation (and everything into a
 * buffer whose frame is too old). Buffers start as garbage, rows have padding. */
static void test_update_into_matches(void)
{
    static const struct { fa_cdg_pixel_format format; uint32_t w, h, flags; } cfgs[] = {
        { FA_CDG_PIXELS_BGRA32, 0, 0, 0 },
        { FA_CDG_PIXELS_RGBA32, 600, 432, FA_CDG_TRANSPARENT },
        { FA_CDG_PIXELS_INDEXED8, 0, 0, 0 },
        { FA_CDG_PIXELS_INDEXED8, 450, 324, FA_CDG_NO_BORDER },
        { FA_CDG_PIXELS_BGRA32, 450, 324, FA_CDG_SMOOTH | FA_CDG_TRANSPARENT | FA_CDG_PREMULTIPLIED },
        { FA_CDG_PIXELS_BGRA32, 576, 384, FA_CDG_SMOOTH | FA_CDG_NO_BORDER | FA_CDG_TRANSPARENT_BACKGROUND_ONLY },
    };
    buf c = busy_cdg();
    size_t packs = c.n / FA_CDG_PACK_BYTES, k, n;
    unsigned ci;
    for (ci = 0; ci < sizeof(cfgs) / sizeof(cfgs[0]); ci++) {
        fa_stream* s = open_wav_stream(1, FA_OPEN_DECODE_ONLY);
        fa_cdg_surface *ref = NULL, *into = NULL;
        fa_cdg_surface_config cfg;
        fa_surface_layout lay;
        fa_cdg_frame fr;
        fa_surface_frame sf;
        uint8_t* mem[3];
        uint64_t rev[3] = { 0, 0, 0 };
        uint32_t bpp, row, y, partial = 0, nbuf;
        int32_t stride;
        fa_cdg_surface_config_init(&cfg);
        cfg.pixel_format = cfgs[ci].format;
        cfg.width = cfgs[ci].w;
        cfg.height = cfgs[ci].h;
        cfg.flags = cfgs[ci].flags;
        CHECK_EQ(fa_stream_load_cdg_memory(s, c.p, c.n), FA_OK);
        CHECK_EQ(fa_cdg_surface_create(s, &cfg, &ref), FA_OK);
        CHECK_EQ(fa_cdg_surface_create(s, &cfg, &into), FA_OK);
        memset(&lay, 0, sizeof(lay));
        lay.struct_size = sizeof(lay);
        CHECK_EQ(fa_surface_get_layout(into, &lay), FA_OK);
        bpp = cfg.pixel_format == FA_CDG_PIXELS_INDEXED8 ? 1 : 4;
        row = lay.width * bpp;
        stride = (int32_t)row + 16;
        for (nbuf = 0; nbuf < 3; nbuf++) {
            mem[nbuf] = (uint8_t*)malloc((size_t)stride * lay.height);
            memset(mem[nbuf], 0xCD, (size_t)stride * lay.height);
        }
        /* Every pack forward, then jumps back and forward. */
        for (n = 0; n < packs + 12; n++) {
            uint32_t b = (uint32_t)(n % 3), i;
            k = n < packs ? n + 1 : (n * 37) % (packs + 1);
            CHECK_EQ(fa_cdg_surface_update_at(ref, t_packs((int64_t)k), &fr), FA_OK);
            memset(&sf, 0, sizeof(sf));
            sf.struct_size = sizeof(sf);
            CHECK_EQ(fa_surface_update_into(into, t_packs((int64_t)k), mem[b], stride, lay.width, lay.height, rev[b], &sf), FA_OK);
            CHECK(sf.dirty_count <= FA_SURFACE_MAX_DIRTY);
            for (i = 0; i < sf.dirty_count; i++)
                CHECK(sf.dirty[i].x >= 0 && sf.dirty[i].y >= 0 && sf.dirty[i].x + sf.dirty[i].width <= lay.width &&
                      sf.dirty[i].y + sf.dirty[i].height <= lay.height);
            if (sf.base_revision) {
                CHECK_EQ(sf.base_revision, rev[b]);
                partial++;
            }
            rev[b] = sf.revision;
            CHECK_EQ(sf.revision, fr.revision);
            CHECK_EQ(sf.palette_revision, fr.palette_revision);
            CHECK(memcmp(sf.palette, fr.palette, sizeof(sf.palette)) == 0);
            CHECK_EQ(sf.background_index, fr.background_index);
            CHECK_EQ(sf.time, fr.time);
            for (y = 0; y < lay.height; y++)
                if (memcmp(mem[b] + (size_t)y * (size_t)stride, (const uint8_t*)fr.pixels + (size_t)y * (size_t)fr.stride, row) != 0) {
                    CHECK(!"update_into differs from update");
                    printf("    config %u, step %u, pack %u, row %u\n", ci, (unsigned)n, (unsigned)k, y);
                    break;
                }
            for (y = 0; y < lay.height; y++)   /* the padding is never written */
                CHECK_EQ(mem[b][(size_t)y * (size_t)stride + row], 0xCD);
        }
        CHECK(partial > packs / 2);    /* most draws were partial */
        for (nbuf = 0; nbuf < 3; nbuf++) free(mem[nbuf]);
        fa_cdg_surface_destroy(ref);
        fa_surface_destroy(into);
        fa_stream_close(s);
    }
    free(c.p);
}

/* The layout report, revisions handed back, size checks, settings, older structs. */
static void test_update_into_api(void)
{
    buf c = basic_cdg();
    fa_stream* s = open_wav_stream(1, FA_OPEN_DECODE_ONLY);
    fa_cdg_surface *a = NULL, *b = NULL;
    fa_cdg_surface_config cfg;
    fa_surface_layout lay;
    fa_surface_frame fr;
    uint8_t* mem = (uint8_t*)malloc(576 * 384 * 4);
    uint64_t r1;
    int k;
    tile(&c, 0, 5, 6, 4, 4, 0x01);
    CHECK_EQ(fa_stream_load_cdg_memory(s, c.p, c.n), FA_OK);
    CHECK_EQ(fa_cdg_surface_create(s, NULL, &a), FA_OK);
    fa_cdg_surface_config_init(&cfg);
    cfg.width = 576;
    cfg.height = 384;
    cfg.flags = FA_CDG_NO_BORDER;
    CHECK_EQ(fa_cdg_surface_create(s, &cfg, &b), FA_OK);

    memset(&lay, 0, sizeof(lay));
    lay.struct_size = sizeof(lay);
    CHECK_EQ(fa_surface_get_layout(a, &lay), FA_OK);
    CHECK_EQ(lay.width, 300); CHECK_EQ(lay.height, 216);
    CHECK_EQ(lay.crop_x, 0); CHECK_EQ(lay.crop_width, 300); CHECK_EQ(lay.crop_height, 216);
    CHECK_EQ(lay.content_width, 300);
    CHECK_EQ(lay.safety_zone, FA_SAFETY_SHOW);
    CHECK_EQ(lay.safety_zone_resolved, 1);
    CHECK(lay.scale == 1.0f);
    CHECK_EQ(lay.layout_revision, 1);
    CHECK_EQ(lay.font_used[0], 0);
    CHECK_EQ(fa_surface_get_layout(b, &lay), FA_OK);
    CHECK_EQ(lay.width, 576); CHECK_EQ(lay.height, 384);
    CHECK_EQ(lay.crop_x, 6); CHECK_EQ(lay.crop_y, 12); CHECK_EQ(lay.crop_width, 288); CHECK_EQ(lay.crop_height, 192);
    CHECK_EQ(lay.safety_zone, FA_SAFETY_REMOVE);
    /* An older, shorter struct is filled up to its size. */
    memset(&lay, 0x55, sizeof(lay));
    lay.struct_size = (uint32_t)offsetof(fa_surface_layout, bitmap_x);
    CHECK_EQ(fa_surface_get_layout(b, &lay), FA_OK);
    CHECK_EQ(lay.height, 384);
    CHECK_EQ(lay.bitmap_x, 0x55555555);
    lay.struct_size = 0;
    CHECK_EQ(fa_surface_get_layout(b, &lay), FA_ERROR_INVALID_ARG);

    /* The buffer must be the layout's size; the stride at least a row. */
    memset(&fr, 0, sizeof(fr));
    fr.struct_size = sizeof(fr);
    CHECK_EQ(fa_surface_update_into(a, t_packs(4), mem, 300 * 4, 301, 216, 0, &fr), FA_ERROR_SIZE_MISMATCH);
    CHECK_EQ(fa_surface_update_into(a, t_packs(4), mem, 299 * 4, 300, 216, 0, &fr), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_surface_update_into(a, t_packs(4), NULL, 300 * 4, 300, 216, 0, &fr), FA_ERROR_INVALID_ARG);

    /* Unknown revision: a full draw. The same revision again: nothing drawn. */
    CHECK_EQ(fa_surface_update_into(a, t_packs(4), mem, 300 * 4, 300, 216, 0, &fr), FA_OK);
    CHECK_EQ(fr.base_revision, 0);
    CHECK_EQ(fr.dirty_count, 1);
    CHECK_EQ(fr.dirty[0].width, 300); CHECK_EQ(fr.dirty[0].height, 216);
    CHECK_EQ(fr.dirty_bounds.width, 300);
    CHECK_EQ(fr.layout_revision, 1);
    CHECK(fr.draw_ns >= fr.render_ns && fr.render_ns >= 0);
    CHECK_EQ(fr.behind, 0);
    r1 = fr.revision;
    CHECK_EQ(fa_surface_update_into(a, t_packs(4), mem, 300 * 4, 300, 216, r1, &fr), FA_OK);
    CHECK_EQ(fr.dirty_count, 0);
    CHECK_EQ(fr.base_revision, r1);
    CHECK_EQ(fr.revision, r1);
    /* One tile later: only that tile. */
    CHECK_EQ(fa_surface_update_into(a, t_packs(5), mem, 300 * 4, 300, 216, r1, &fr), FA_OK);
    CHECK_EQ(fr.base_revision, r1);
    CHECK_EQ(fr.dirty_count, 1);
    CHECK_EQ(fr.dirty[0].x, 24); CHECK_EQ(fr.dirty[0].y, 48);
    CHECK_EQ(fr.dirty[0].width, 6); CHECK_EQ(fr.dirty[0].height, 12);
    /* More than FA_SURFACE_HISTORY revisions back: a full draw. */
    for (k = 0; k < FA_SURFACE_HISTORY + 1; k++)
        CHECK_EQ(fa_surface_update_into(a, t_packs(k % 2 ? 4 : 5), mem, 300 * 4, 300, 216, 0, &fr), FA_OK);
    CHECK_EQ(fa_surface_update_into(a, t_packs(4), mem, 300 * 4, 300, 216, r1 + 1, &fr), FA_OK);
    CHECK_EQ(fr.base_revision, 0);
    CHECK_EQ(fr.dirty[0].width, 300);
    /* A revision from the future: full. */
    CHECK_EQ(fa_surface_update_into(a, t_packs(4), mem, 300 * 4, 300, 216, fr.revision + 5, &fr), FA_OK);
    CHECK_EQ(fr.base_revision, 0);

    /* Settings. */
    CHECK_EQ(fa_surface_set_presentation_size(a, 1920, 1080), FA_OK);
    CHECK_EQ(fa_surface_set_presentation_size(a, 0, 0), FA_OK);
    CHECK_EQ(fa_surface_set_presentation_size(a, 0, 1080), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_surface_set_presentation_size(a, 7680, 4320), FA_OK);
    CHECK_EQ(fa_surface_set_presentation_size(NULL, 1920, 1080), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_surface_set_render_ahead(a, 1), FA_OK);
    CHECK_EQ(fa_surface_set_render_ahead(a, 2), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_surface_get_layout(a, &lay), FA_ERROR_INVALID_ARG);   /* struct_size still 0 */
    lay.struct_size = sizeof(lay);
    CHECK_EQ(fa_surface_get_layout(a, &lay), FA_OK);
    CHECK_EQ(lay.width, 300);   /* the classic picture keeps its size */

    /* FA_SURFACE_NOW follows the clock (0 before playing: no pack). */
    CHECK_EQ(fa_surface_update_into(a, FA_SURFACE_NOW, mem, 300 * 4, 300, 216, 0, &fr), FA_OK);
    CHECK_EQ(fr.background_index, -1);

    /* Once the stream is closed, updates refuse; destroy still works. */
    fa_stream_close(s);
    CHECK_EQ(fa_surface_update_into(b, t_packs(4), mem, 576 * 4, 576, 384, 0, &fr), FA_ERROR_INVALID_ARG);
    fa_surface_destroy(a);
    fa_surface_destroy(b);
    fa_surface_destroy(NULL);
    free(mem);
    free(c.p);
}

/* The presentation size keeps its shape inside 300x216 .. 3840x2160. */
static void test_presentation_clamp(void)
{
    uint32_t w, h;
    w = 1920; h = 1080; CHECK_EQ(fa_presentation_size_clamp(&w, &h), FA_OK); CHECK_EQ(w, 1920); CHECK_EQ(h, 1080);
    w = 7680; h = 4320; fa_presentation_size_clamp(&w, &h); CHECK_EQ(w, 3840); CHECK_EQ(h, 2160);
    w = 320; h = 180; fa_presentation_size_clamp(&w, &h); CHECK_EQ(w, 384); CHECK_EQ(h, 216);
    w = 5120; h = 1440; fa_presentation_size_clamp(&w, &h); CHECK_EQ(w, 3840); CHECK_EQ(h, 1080);
    w = 100; h = 5000; fa_presentation_size_clamp(&w, &h); CHECK_EQ(h, 2160); CHECK_EQ(w, 300);
    w = 0; h = 0; CHECK_EQ(fa_presentation_size_clamp(&w, &h), FA_OK); CHECK_EQ(w, 0);
    w = 0; h = 1; CHECK_EQ(fa_presentation_size_clamp(&w, &h), FA_ERROR_INVALID_ARG);
}

/* The changed-area sets: merging, the limit, the history. */
static void test_rect_sets(void)
{
    fa_rects r;
    fa_surface_history h;
    int b[4], i;
    fa_rects_clear(&r);
    fa_rects_add(&r, 0, 0, 6, 12);
    fa_rects_add(&r, 6, 0, 12, 12);       /* completes a rectangle: merged */
    CHECK_EQ(r.n, 1); CHECK_EQ(r.r[0][2], 12);
    fa_rects_add(&r, 100, 100, 106, 112); /* far: separate */
    CHECK_EQ(r.n, 2);
    fa_rects_add(&r, 3, 6, 103, 106);     /* overlaps both: one */
    CHECK_EQ(r.n, 1);
    fa_rects_bounds(&r, b);
    CHECK_EQ(b[0], 0); CHECK_EQ(b[3], 112);
    fa_rects_clear(&r);
    for (i = 0; i < 20; i++) fa_rects_add(&r, i * 20, i * 20, i * 20 + 6, i * 20 + 12);
    CHECK_EQ(r.n, FA_SURFACE_MAX_DIRTY);
    fa_rects_bounds(&r, b);
    CHECK_EQ(b[0], 0); CHECK_EQ(b[2], 386); CHECK_EQ(b[3], 392);
    fa_rects_add(&r, 5, 5, 5, 10);        /* empty: ignored */
    CHECK_EQ(r.n, FA_SURFACE_MAX_DIRTY);

    memset(&h, 0, sizeof(h));
    for (i = 1; i <= 10; i++) {
        fa_rects_clear(&r);
        fa_rects_add(&r, i * 10, 0, i * 10 + 5, 5);
        fa_history_push(&h, (uint64_t)i, &r);
    }
    CHECK_EQ(fa_history_since(&h, 10, 10, &r), 1); CHECK_EQ(r.n, 0);
    CHECK_EQ(fa_history_since(&h, 10, 8, &r), 1); CHECK_EQ(r.n, 2);
    CHECK_EQ(fa_history_since(&h, 10, 2, &r), 1); CHECK_EQ(r.n, 8);
    CHECK_EQ(fa_history_since(&h, 10, 1, &r), 0);
    CHECK_EQ(fa_history_since(&h, 10, 0, &r), 0);
    CHECK_EQ(fa_history_since(&h, 10, 11, &r), 0);
}

/* ---- presentation options ---------------------------------------------------------- */

static fa_cdg_presentation pres(int scale, fa_safety_zone zone)
{
    fa_cdg_presentation p;
    fa_cdg_presentation_init(&p);
    p.scale = scale;
    p.safety_zone = zone;
    return p;
}

static fa_surface_layout layout_of(fa_surface* f)
{
    fa_surface_layout l;
    memset(&l, 0, sizeof(l));
    l.struct_size = sizeof(l);
    fa_surface_get_layout(f, &l);
    return l;
}

/* A full buffer (dst_revision 0) at pack k, into a fresh malloc'ed buffer. */
static uint8_t* full_draw(fa_surface* f, int64_t k, uint32_t bpp, fa_surface_layout* l)
{
    fa_surface_frame fr;
    uint8_t* mem;
    *l = layout_of(f);
    mem = (uint8_t*)malloc((size_t)l->width * l->height * bpp);
    memset(&fr, 0, sizeof(fr));
    fr.struct_size = sizeof(fr);
    if (fa_surface_update_into(f, t_packs(k), mem, (int32_t)(l->width * bpp), l->width, l->height, 0, &fr) != FA_OK) {
        free(mem);
        return NULL;
    }
    return mem;
}

/* Scrolls of every kind with tiles between them, also while smooth-scroll offsets
 * are set, drawing in the border, colour changes. */
static buf moves_cdg(void)
{
    buf c = { 0 };
    int i;
    colors(&c, 0, k_low);
    preset(&c, 0, 0);
    border(&c, 5);
    for (i = 0; i < 24; i++) tile(&c, i % 3 == 2, 1 + i % 4, (i * 3) % 7, 1 + (i * 7) % 16, 1 + (i * 11) % 48, (uint8_t)(0x0C + i));
    tile(&c, 0, 0, 4, 0, 10, 0x21);           /* in the border */
    tile(&c, 0, 0, 2, 17, 30, 0x12);
    scroll(&c, 1, 0, 2, 0, 0, 0);             /* copy left */
    tile(&c, 0, 3, 1, 5, 5, 0x1E);
    scroll(&c, 0, 3, 1, 0, 0, 0);             /* preset right, colour 3 */
    scroll(&c, 1, 0, 0, 0, 2, 0);             /* copy up */
    tile(&c, 0, 2, 4, 9, 25, 0x33);
    scroll(&c, 0, 0, 0, 0, 1, 0);             /* preset down */
    scroll(&c, 1, 0, 1, 0, 1, 0);             /* copy right and down at once */
    scroll(&c, 0, 0, 0, 2, 0, 5);             /* offsets only */
    tile(&c, 0, 1, 2, 8, 8, 0x2D);            /* drawn while they are set */
    scroll(&c, 1, 0, 2, 3, 0, 7);             /* copy left, other offsets */
    scroll(&c, 0, 0, 0, 5, 2, 11);
    tile(&c, 1, 0, 3, 16, 47, 0x3F);
    scroll(&c, 0, 0, 0, 0, 0, 0);             /* offsets back to 0 */
    tile(&c, 0, 4, 1, 3, 3, 0x0F);
    transparent(&c, 2, 30);
    colors(&c, 0, k_low);                     /* same colours: no change */
    for (i = 0; i < 6; i++) scroll(&c, 1, 0, 2, 0, 0, 0);
    tile(&c, 0, 2, 3, 12, 40, 0x15);
    for (i = 0; i < 4; i++) scroll(&c, 1, 0, 0, 0, 1, 0);
    preset(&c, 4, 0);
    tile(&c, 0, 1, 2, 6, 6, 0x2A);
    return c;
}

/* Drawing inside the picture area only, moved around by scrolls that fill with the
 * background: SUPER crops it, and the scrolls bring in what was outside the crop. */
static buf inner_moves_cdg(void)
{
    buf c = { 0 };
    int i;
    colors(&c, 0, k_low);
    preset(&c, 0, 0);
    for (i = 0; i < 16; i++) tile(&c, 0, 0, 1 + i % 4, 4 + i % 8, 8 + (i * 5) % 30, (uint8_t)(0x11 + i * 3));
    scroll(&c, 0, 0, 1, 0, 0, 0);
    scroll(&c, 0, 0, 1, 0, 1, 0);
    tile(&c, 0, 2, 3, 9, 20, 0x2C);
    scroll(&c, 0, 0, 2, 0, 0, 0);
    scroll(&c, 0, 0, 2, 0, 2, 0);
    scroll(&c, 0, 0, 2, 0, 0, 0);
    scroll(&c, 0, 0, 1, 0, 0, 0);
    return c;
}

/* A shape near the right edge, moved right by a scroll and then by an offset: it ends
 * next to the screen's edge, beyond the end of a REMOVE crop's master. */
static buf edge_moves_cdg(void)
{
    buf c = { 0 };
    uint8_t d[16];
    int i;
    colors(&c, 0, k_low);
    preset(&c, 0, 0);
    memset(d, 0, sizeof(d));
    d[1] = 1; d[2] = 8; d[3] = 48;
    for (i = 6; i < 12; i++) d[4 + i] = 0x3F;   /* the lower half in colour 1 */
    pack(&c, 6, d);
    scroll(&c, 0, 0, 1, 0, 0, 0);
    scroll(&c, 0, 0, 0, 5, 0, 0);
    scroll(&c, 0, 0, 0, 0, 1, 0);
    scroll(&c, 0, 0, 0, 0, 0, 11);
    return c;
}

/* Without options a surface draws as before; options that change nothing (scale off,
 * SHOW, no padding) draw the same; REMOVE equals FA_CDG_NO_BORDER. */
static void test_presentation_classic(void)
{
    buf c = busy_cdg();
    fa_stream* s = open_wav_stream(1, FA_OPEN_DECODE_ONLY);
    fa_cdg_surface *plain = NULL, *opts = NULL, *noborder = NULL, *removed = NULL;
    fa_cdg_surface_config cfg;
    fa_cdg_presentation p;
    fa_surface_layout la, lb;
    size_t packs = c.n / FA_CDG_PACK_BYTES, k;
    CHECK_EQ(fa_stream_load_cdg_memory(s, c.p, c.n), FA_OK);
    fa_cdg_surface_config_init(&cfg);
    cfg.pixel_format = FA_CDG_PIXELS_INDEXED8;
    CHECK_EQ(fa_cdg_surface_create(s, &cfg, &plain), FA_OK);
    CHECK_EQ(fa_cdg_surface_create(s, &cfg, &opts), FA_OK);
    CHECK_EQ(fa_cdg_surface_create(s, &cfg, &removed), FA_OK);
    cfg.flags = FA_CDG_NO_BORDER;
    CHECK_EQ(fa_cdg_surface_create(s, &cfg, &noborder), FA_OK);
    memset(&p, 0, sizeof(p));
    p.struct_size = sizeof(p);
    CHECK_EQ(fa_cdg_surface_get_presentation(noborder, &p), FA_OK);
    CHECK_EQ(p.safety_zone, FA_SAFETY_REMOVE);
    CHECK_EQ(p.scale, 0);
    p = pres(0, FA_SAFETY_SHOW);
    CHECK_EQ(fa_cdg_surface_set_presentation(opts, &p), FA_OK);
    p = pres(0, FA_SAFETY_REMOVE);
    CHECK_EQ(fa_cdg_surface_set_presentation(removed, &p), FA_OK);
    for (k = 1; k <= packs; k += 3) {
        uint8_t *a = full_draw(plain, (int64_t)k, 1, &la), *b = full_draw(opts, (int64_t)k, 1, &lb);
        CHECK(a && b);
        CHECK_EQ(la.width, lb.width);
        CHECK(memcmp(a, b, (size_t)la.width * la.height) == 0);
        free(a);
        free(b);
        a = full_draw(noborder, (int64_t)k, 1, &la);
        b = full_draw(removed, (int64_t)k, 1, &lb);
        CHECK(a && b);
        CHECK_EQ(la.width, 288); CHECK_EQ(lb.width, 288);
        CHECK(memcmp(a, b, (size_t)la.width * la.height) == 0);
        free(a);
        free(b);
    }
    /* NULL goes back to the config's picture. */
    CHECK_EQ(fa_cdg_surface_set_presentation(removed, NULL), FA_OK);
    la = layout_of(removed);
    CHECK_EQ(la.width, 300);
    CHECK_EQ(la.layout_revision, 3);
    /* After the stream is closed the layout is still reported. */
    fa_stream_close(s);
    s = NULL;
    la = layout_of(plain);
    CHECK_EQ(la.width, 300);
    fa_surface_destroy(plain);
    fa_surface_destroy(opts);
    fa_surface_destroy(noborder);
    fa_surface_destroy(removed);
    free(c.p);
}

/* Scale off: padding around the picture, transparent; a transparent border. */
static void test_presentation_padding(void)
{
    buf c = busy_cdg();
    fa_stream* s = open_wav_stream(1, FA_OPEN_DECODE_ONLY);
    fa_cdg_surface *ref = NULL, *ix = NULL, *col = NULL;
    fa_cdg_surface_config cfg;
    fa_cdg_presentation p;
    fa_surface_layout l, lr;
    uint8_t *a, *b, *r;
    int64_t k = (int64_t)(c.n / FA_CDG_PACK_BYTES);
    uint32_t x, y;
    CHECK_EQ(fa_stream_load_cdg_memory(s, c.p, c.n), FA_OK);
    fa_cdg_surface_config_init(&cfg);
    cfg.pixel_format = FA_CDG_PIXELS_INDEXED8;
    CHECK_EQ(fa_cdg_surface_create(s, &cfg, &ref), FA_OK);
    CHECK_EQ(fa_cdg_surface_create(s, &cfg, &ix), FA_OK);
    cfg.pixel_format = FA_CDG_PIXELS_BGRA32;
    cfg.width = 600;
    cfg.height = 432;
    CHECK_EQ(fa_cdg_surface_create(s, &cfg, &col), FA_OK);
    p = pres(0, FA_SAFETY_SHOW);
    p.padding = 4;
    CHECK_EQ(fa_cdg_surface_set_presentation(ix, &p), FA_OK);
    l = layout_of(ix);
    CHECK_EQ(l.width, 308); CHECK_EQ(l.height, 224);
    CHECK_EQ(l.content_x, 4); CHECK_EQ(l.content_y, 4);
    CHECK_EQ(l.content_width, 300);
    CHECK_EQ(l.padding, 4); CHECK_EQ(l.margin, 0);
    CHECK_EQ(l.layout_revision, 2);
    a = full_draw(ix, k, 1, &l);
    r = full_draw(ref, k, 1, &lr);
    CHECK(a && r);
    for (y = 0; y < l.height; y++)
        for (x = 0; x < l.width; x++) {
            uint8_t v = a[y * l.width + x];
            if (x < 4 || y < 4 || x >= 304 || y >= 220) { CHECK_EQ(v, FA_CDG_INDEX_TRANSPARENT); }
            else CHECK_EQ(v, r[(y - 4) * 300 + x - 4]);
        }
    free(a);
    /* A transparent border: index 16 there, the picture area as it is. */
    p.padding = 0;
    p.flags = FA_CDG_TRANSPARENT_BORDER;
    CHECK_EQ(fa_cdg_surface_set_presentation(ix, &p), FA_OK);
    a = full_draw(ix, k, 1, &l);
    CHECK(a);
    for (y = 0; y < 216; y++)
        for (x = 0; x < 300; x++) {
            int border = x < 6 || x >= 294 || y < 12 || y >= 204;
            CHECK_EQ(a[y * 300 + x], border ? FA_CDG_INDEX_TRANSPARENT : r[y * 300 + x]);
        }
    free(a);
    free(r);
    /* Padding scales with the output size; colour formats get transparent pixels. */
    p.padding = 3;
    CHECK_EQ(fa_cdg_surface_set_presentation(col, &p), FA_OK);
    b = full_draw(col, k, 4, &l);
    CHECK(b);
    CHECK_EQ(l.width, 612); CHECK_EQ(l.height, 444);
    CHECK_EQ(l.content_x, 6);
    for (x = 0; x < 6; x++) CHECK_EQ(b[(100 * l.width + x) * 4 + 3], 0);
    CHECK_EQ(b[(6 * l.width + 6) * 4 + 3], 0);       /* border, transparent */
    CHECK_EQ(b[(100 * l.width + 300) * 4 + 3], 255);  /* picture */
    free(b);
    fa_surface_destroy(ref);
    fa_surface_destroy(ix);
    fa_surface_destroy(col);
    fa_stream_close(s);
    free(c.p);
}

/* Scale on: the multiple, the buffer around the picture, colours, sharp pixels. */
static void test_presentation_scaled(void)
{
    buf c = busy_cdg();
    fa_stream* s = open_wav_stream(1, FA_OPEN_DECODE_ONLY);
    fa_cdg_surface *ref = NULL, *f = NULL, *g = NULL;
    fa_cdg_surface_config cfg;
    fa_cdg_presentation p;
    fa_surface_layout l, lr;
    fa_surface_frame fr;
    fa_cdg_frame cf;
    uint8_t *a, *r;
    int64_t k = 60;
    uint32_t x, y, i;
    CHECK_EQ(fa_stream_load_cdg_memory(s, c.p, c.n), FA_OK);
    fa_cdg_surface_config_init(&cfg);
    cfg.pixel_format = FA_CDG_PIXELS_INDEXED8;
    CHECK_EQ(fa_cdg_surface_create(s, &cfg, &ref), FA_OK);
    CHECK_EQ(fa_cdg_surface_create(s, NULL, &f), FA_OK);
    cfg.pixel_format = FA_CDG_PIXELS_RGBA32;
    CHECK_EQ(fa_cdg_surface_create(s, &cfg, &g), FA_OK);
    p = pres(1, FA_SAFETY_SHOW);
    CHECK_EQ(fa_cdg_surface_set_presentation(f, &p), FA_OK);
    l = layout_of(f);
    CHECK(l.scale == 1.0f);   /* no presentation size: 1x */
    CHECK_EQ(l.width, 302); CHECK_EQ(l.margin, 1);
    CHECK_EQ(fa_surface_set_presentation_size(f, 1920, 1080), FA_OK);
    l = layout_of(f);
    CHECK(l.scale == 5.0f);
    CHECK_EQ(l.width, 1510); CHECK_EQ(l.height, 1090);
    CHECK_EQ(l.content_x, 5); CHECK_EQ(l.content_y, 5);
    CHECK_EQ(l.content_width, 1500); CHECK_EQ(l.content_height, 1080);
    CHECK_EQ(l.margin, 5); CHECK_EQ(l.padding, 0);
    CHECK_EQ(l.bitmap_x, (1920 - 1510) / 2); CHECK_EQ(l.bitmap_y, -5);
    p.safety_zone = FA_SAFETY_REMOVE;
    p.padding = 2;
    CHECK_EQ(fa_cdg_surface_set_presentation(f, &p), FA_OK);
    l = layout_of(f);
    CHECK(l.scale == 6.0f);
    CHECK_EQ(l.content_height, 1152);
    CHECK_EQ(l.width, (288 + 6) * 6); CHECK_EQ(l.padding, 12); CHECK_EQ(l.margin, 6);
    CHECK_EQ(l.content_x, 18);
    /* Sharp pixels: each CD+G pixel an n x n block of its colour, premultiplied, the
     * background transparent; padding and margin transparent. */
    p = pres(1, FA_SAFETY_SHOW);
    CHECK_EQ(fa_cdg_surface_set_presentation(f, &p), FA_OK);
    CHECK_EQ(fa_surface_set_presentation_size(f, 900, 648), FA_OK);
    a = full_draw(f, k, 4, &l);
    r = full_draw(ref, k, 1, &lr);
    CHECK(a && r);
    CHECK(l.scale == 3.0f);
    memset(&fr, 0, sizeof(fr));
    fr.struct_size = sizeof(fr);
    CHECK_EQ(fa_surface_update_into(f, t_packs(k), a, (int32_t)(l.width * 4), l.width, l.height, 0, &fr), FA_OK);
    for (y = 0; y < l.height; y++)
        for (x = 0; x < l.width; x++) {
            const uint8_t* q = a + ((size_t)y * l.width + x) * 4;
            if (x < 3 || y < 3 || x >= l.width - 3 || y >= l.height - 3) {
                CHECK(q[0] == 0 && q[1] == 0 && q[2] == 0 && q[3] == 0);
            } else {
                uint8_t ci = r[((y - 3) / 3) * 300 + (x - 3) / 3];
                const uint8_t* pc = fr.palette[ci];
                uint32_t al = (int)ci == fr.background_index ? 0 : pc[3];
                CHECK_EQ(q[3], al);
                CHECK_EQ(q[2], (pc[0] * al + 127) / 255);   /* BGRA */
                CHECK_EQ(q[0], (pc[2] * al + 127) / 255);
            }
        }
    free(a);
    /* RGBA config: RGBA order. fa_cdg_surface_update gives the same buffer. */
    CHECK_EQ(fa_cdg_surface_set_presentation(g, &p), FA_OK);
    CHECK_EQ(fa_surface_set_presentation_size(g, 900, 648), FA_OK);
    a = full_draw(g, k, 4, &l);
    CHECK(a);
    CHECK_EQ(fa_cdg_surface_update_at(g, t_packs(k), &cf), FA_OK);
    CHECK_EQ(cf.pixel_format, FA_CDG_PIXELS_RGBA32);
    CHECK_EQ(cf.width, l.width); CHECK_EQ(cf.height, l.height);
    for (y = 0; y < l.height; y++) CHECK(memcmp(a + (size_t)y * l.width * 4, (const uint8_t*)cf.pixels + (size_t)y * cf.stride, l.width * 4) == 0);
    for (i = 3; i < l.width * l.height; i += 997) {
        const uint8_t* q = a + (size_t)i * 4;
        CHECK(q[0] <= q[3] && q[1] <= q[3] && q[2] <= q[3]);
    }
    free(a);
    free(r);
    fa_surface_destroy(ref);
    fa_surface_destroy(f);
    fa_surface_destroy(g);
    fa_stream_close(s);
    free(c.p);
}

/* Drawing only what changed (a moved master, offsets, the reach of smoothing and edge
 * blend) gives the same buffer as drawing everything, for every option, into buffers
 * in rotation. */
/* Effects for the tests: 1 outline and a soft shadow, 2 bevel and a hard outline, 3
 * a glow and a hard shadow far off. */
static fa_surface_effects fx_set(int which)
{
    fa_surface_effects e;
    fa_surface_effects_init(&e);
    if (which == 1) {
        e.outline.on = 1;
        e.outline.size = 0.6f;
        e.outline.color = 0xFF102030u;
        e.shadow.on = 1;
        e.shadow.size = 0.7f;
        e.shadow.fade = 0.8f;
        e.shadow.distance = 0.6f;
    } else if (which == 2) {
        e.bevel.on = 1;
        e.bevel.strength = 1.0f;
        e.bevel.size = 1.0f;
        e.outline.on = 1;
        e.outline.size = 0.3f;
    } else {
        e.outline.on = 1;
        e.outline.size = 1.0f;
        e.outline.fade = 1.0f;
        e.outline.color = 0xC0FFE000u;
        e.shadow.on = 1;
        e.shadow.distance = 1.0f;
        e.shadow.angle_degrees = 200.0f;
    }
    return e;
}

/* A pixel of buffer `mem` (BGRA, stride w * 4) at screen pixel (sx, sy) of a SHOW
 * surface drawn at 2x, plus (ox, oy) buffer pixels. */
static const uint8_t* fx_px(const uint8_t* mem, const fa_surface_layout* l, int sx, int sy, int ox, int oy)
{
    return mem + ((size_t)(l->content_y + (uint32_t)sy * 2 + (uint32_t)oy) * l->width + l->content_x + (uint32_t)sx * 2 +
                  (uint32_t)ox) * 4;
}

/* The bevel is kept to half a stroke's width: on a bar 4 pixels wide (half width 2)
 * a bevel 4 pixels deep stops 1 pixel in, so the bar's middle keeps its colour; on a
 * wide bar it goes the whole 4 pixels in. */
static void test_bevel_half_stroke(void)
{
    enum { BW = 96, BH = 64 };
    uint8_t* src = (uint8_t*)calloc((size_t)BW * BH, 4);
    uint8_t* dst = (uint8_t*)calloc((size_t)BW * BH, 4);
    fa_surface_effects fx;
    fa_fx_plan plan;
    void* scratch;
    int x, y;
    CHECK(src && dst);
    for (y = 8; y < 56; y++) {
        for (x = 10; x < 14; x++) memset(src + ((size_t)y * BW + x) * 4, 255, 4);    /* thin */
        for (x = 30; x < 80; x++) memset(src + ((size_t)y * BW + x) * 4, 255, 4);    /* wide */
    }
    fa_surface_effects_init(&fx);
    fx.bevel.on = 1;
    fx.bevel.size = 1.0f;
    fx.bevel.strength = 1.0f;
    fx.bevel.fade = 0.0f;
    fx.bevel.light_angle_degrees = 0.0f;   /* from the right: left edges in shade */
    CHECK(fa_fx_plan_make(&plan, &fx, 1.0f, 1.0f, 1.0f, 4.0f, 0));
    scratch = malloc(fa_fx_scratch_bytes(&plan));
    CHECK(scratch);
    fa_fx_render(&plan, src, (size_t)BW * 4, BW, BH, dst, (size_t)BW * 4, 0, 0, BW, BH, scratch);
    y = 32;
    /* Thin bar: the outer pixel on each side shaded or lit, the middle two untouched. */
    CHECK(memcmp(dst + ((size_t)y * BW + 10) * 4, src + ((size_t)y * BW + 10) * 4, 4) != 0);
    CHECK(memcmp(dst + ((size_t)y * BW + 11) * 4, src + ((size_t)y * BW + 11) * 4, 4) == 0);
    CHECK(memcmp(dst + ((size_t)y * BW + 12) * 4, src + ((size_t)y * BW + 12) * 4, 4) == 0);
    /* Wide bar: the shade reaches 4 pixels in (the fourth at inside distance 3.5), and
     * not the middle. */
    CHECK(memcmp(dst + ((size_t)y * BW + 30) * 4, src + ((size_t)y * BW + 30) * 4, 4) != 0);
    CHECK(memcmp(dst + ((size_t)y * BW + 33) * 4, src + ((size_t)y * BW + 33) * 4, 4) != 0);
    CHECK(memcmp(dst + ((size_t)y * BW + 55) * 4, src + ((size_t)y * BW + 55) * 4, 4) == 0);
    free(scratch);
    free(src);
    free(dst);
}

/* Lyrics coloured in while the colours fade, with a scroll, new transparency and a new
 * background on the way. */
static buf fade_cdg(void)
{
    uint8_t pal[8][3];
    buf c = { 0 };
    int row, col, k, j;
    memcpy(pal, k_low, sizeof(pal));
    colors(&c, 0, k_low);
    colors(&c, 1, k_low);
    preset(&c, 0, 0);
    for (row = 2; row < 16; row += 2)
        for (col = 2; col < 48; col += 3) tile(&c, 0, 0, 1 + (row + col) % 7, row, col, (uint8_t)(0x15 ^ (col * 7 + row)));
    for (k = 0; k < 60; k++) {
        tile(&c, k & 1, 0, 9 + k % 6, 2 + 2 * ((k / 15) % 7), 2 + 3 * (k % 15), (uint8_t)(0x2A ^ k));
        if (k % 3 == 0) {
            for (j = 1; j < 8; j++) pal[j][(k / 3) % 3] = (uint8_t)((pal[j][(k / 3) % 3] + 5) & 15);
            colors(&c, (k / 3) & 1, (const uint8_t(*)[3])pal);
        }
        if (k == 20) scroll(&c, 1, 0, 1, 0, 0, 0);
        if (k == 30) transparent(&c, 4, 30);
        if (k == 45) preset(&c, 5, 0);
    }
    return c;
}

/* With effects, a frame whose colours changed but not its shapes blends the new
 * colours with what the effects kept: the same pixels as drawing them all again. */
static void test_effects_recolor(void)
{
    static const struct { uint32_t w, h; int smooth, blend, rgba, fx; } cfgs[] = {
        { 600, 432, 2, 1, 0, 1 },     /* the 2N pass */
        { 900, 648, 4, 1, 1, 2 },
        { 1200, 864, 4, 0, 0, 4 },    /* everything at maximum */
        { 1200, 864, 10, 0, 0, 3 },
    };
    buf c = fade_cdg();
    size_t packs = c.n / FA_CDG_PACK_BYTES;
    unsigned ci;
    int32_t blends = fa_cdg_test_blends();
    for (ci = 0; ci < sizeof(cfgs) / sizeof(cfgs[0]); ci++) {
        fa_stream* s = open_wav_stream(1, FA_OPEN_DECODE_ONLY);
        fa_cdg_surface *ref = NULL, *f = NULL;
        fa_cdg_surface_config cfg;
        fa_cdg_presentation p;
        fa_surface_effects fx;
        fa_surface_layout l;
        fa_surface_frame fr;
        uint8_t* mem[2];
        uint64_t rev[2] = { 0, 0 };
        size_t n, size;
        uint32_t b, y;
        fa_cdg_surface_config_init(&cfg);
        if (cfgs[ci].rgba) cfg.pixel_format = FA_CDG_PIXELS_RGBA32;
        CHECK_EQ(fa_stream_load_cdg_memory(s, c.p, c.n), FA_OK);
        CHECK_EQ(fa_cdg_surface_create(s, &cfg, &ref), FA_OK);
        CHECK_EQ(fa_cdg_surface_create(s, &cfg, &f), FA_OK);
        p = pres(1, FA_SAFETY_SHOW);
        p.smoothness = cfgs[ci].smooth;
        p.edge_blend = cfgs[ci].blend;
        CHECK_EQ(fa_cdg_surface_set_presentation(f, &p), FA_OK);
        CHECK_EQ(fa_surface_set_presentation_size(f, cfgs[ci].w, cfgs[ci].h), FA_OK);
        CHECK_EQ(fa_surface_set_presentation_size(ref, cfgs[ci].w, cfgs[ci].h), FA_OK);
        if (cfgs[ci].fx == 4) {
            fa_surface_effects_init(&fx);
            fx.outline.on = fx.shadow.on = fx.bevel.on = 1;
            fx.outline.size = fx.shadow.size = fx.shadow.distance = fx.bevel.size = fx.bevel.strength = 1.0f;
            fx.shadow.fade = 1.0f;
        } else {
            fx = fx_set(cfgs[ci].fx);
        }
        CHECK_EQ(fa_surface_set_effects(f, &fx), FA_OK);
        CHECK_EQ(fa_surface_set_effects(ref, &fx), FA_OK);
        l = layout_of(f);
        size = (size_t)l.width * 4 * l.height;
        for (b = 0; b < 2; b++) mem[b] = (uint8_t*)calloc(1, size);
        for (n = 0; n < packs; n++) {
            fa_surface_layout lr;
            uint8_t* want;
            b = (uint32_t)(n % 2);
            memset(&fr, 0, sizeof(fr));
            fr.struct_size = sizeof(fr);
            CHECK_EQ(fa_surface_update_into(f, t_packs((int64_t)n + 1), mem[b], (int32_t)(l.width * 4), l.width, l.height,
                                            rev[b], &fr), FA_OK);
            rev[b] = fr.revision;
            if (n < packs - 70 && n % 5 != 0) continue;   /* the fades: every frame */
            CHECK_EQ(fa_cdg_surface_set_presentation(ref, &p), FA_OK);
            want = full_draw(ref, (int64_t)n + 1, 4, &lr);
            CHECK(want);
            for (y = 0; y < l.height; y++)
                if (memcmp(mem[b] + (size_t)y * l.width * 4, want + (size_t)y * l.width * 4, l.width * 4) != 0) {
                    printf("    config %u, pack %u, row %u\n", ci, (unsigned)n + 1, y);
                    free(want);
                    CHECK(!"blending new colours differs from drawing all");
                }
            free(want);
        }
        for (b = 0; b < 2; b++) free(mem[b]);
        fa_surface_destroy(ref);
        fa_surface_destroy(f);
        fa_stream_close(s);
    }
    CHECK(fa_cdg_test_blends() > blends);
    free(c.p);
}

/* With presentation scaling a colour change redraws only what is drawn in the colours
 * that changed (and what they reach), and one not on the screen redraws nothing; the
 * pixels stay those of a full draw. */
static void test_colour_change_partial(void)
{
    uint8_t pal[8][3];
    buf c = { 0 };
    fa_stream* s = open_wav_stream(1, FA_OPEN_DECODE_ONLY);
    fa_cdg_surface *f = NULL, *ref = NULL;
    fa_cdg_presentation p = pres(1, FA_SAFETY_SHOW);
    fa_surface_layout l, lr;
    fa_surface_frame fr;
    uint8_t *mem, *want;
    int32_t stride;
    int k;
    memcpy(pal, k_low, sizeof(pal));
    colors(&c, 0, k_low);
    preset(&c, 0, 0);
    tile(&c, 0, 0, 1, 4, 5, 0x1E);    /* red, on the left */
    tile(&c, 0, 0, 2, 12, 40, 0x33);  /* green, on the right */
    pal[1][1] = 9;                    /* pack 5: red becomes orange */
    colors(&c, 0, (const uint8_t(*)[3])pal);
    pal[7][2] = 15;                   /* pack 6: a colour nothing is drawn in */
    colors(&c, 0, (const uint8_t(*)[3])pal);
    CHECK_EQ(fa_stream_load_cdg_memory(s, c.p, c.n), FA_OK);
    CHECK_EQ(fa_cdg_surface_create(s, NULL, &f), FA_OK);
    CHECK_EQ(fa_cdg_surface_create(s, NULL, &ref), FA_OK);
    p.smoothness = 4;
    p.edge_blend = 1;
    CHECK_EQ(fa_cdg_surface_set_presentation(f, &p), FA_OK);
    CHECK_EQ(fa_cdg_surface_set_presentation(ref, &p), FA_OK);
    CHECK_EQ(fa_surface_set_presentation_size(f, 600, 432), FA_OK);
    CHECK_EQ(fa_surface_set_presentation_size(ref, 600, 432), FA_OK);
    l = layout_of(f);
    stride = (int32_t)(l.width * 4);
    mem = (uint8_t*)calloc((size_t)stride, l.height);
    CHECK(mem);
    memset(&fr, 0, sizeof(fr));
    fr.struct_size = sizeof(fr);
    CHECK_EQ(fa_surface_update_into(f, t_packs(4), mem, stride, l.width, l.height, 0, &fr), FA_OK);
    for (k = 5; k <= 6; k++) {
        uint64_t rev = fr.revision;
        memset(&fr, 0, sizeof(fr));
        fr.struct_size = sizeof(fr);
        CHECK_EQ(fa_surface_update_into(f, t_packs(k), mem, stride, l.width, l.height, rev, &fr), FA_OK);
        if (k == 5) {
            /* Around the red tile (screen x 30..36, y 48..60, at 2x), not the green one. */
            CHECK(fr.dirty_count > 0);
            CHECK(fr.dirty_bounds.x + fr.dirty_bounds.width <= l.content_x + 2 * 60);
            CHECK(fr.dirty_bounds.y + fr.dirty_bounds.height <= l.content_y + 2 * 80);
        } else {
            CHECK_EQ(fr.dirty_count, 0);
            CHECK_EQ(fr.revision, rev);
        }
        want = full_draw(ref, k, 4, &lr);
        CHECK(want);
        CHECK(memcmp(mem, want, (size_t)stride * l.height) == 0);
        free(want);
    }
    free(mem);
    fa_surface_destroy(ref);
    fa_surface_destroy(f);
    fa_stream_close(s);
    free(c.p);
}

/* At 1x the effects are worked out at 2x and box-filtered down: a bar 2 CD+G pixels
 * wide gets a bevel (at 1x itself half its width is under a pixel and there would be
 * none), a 1-pixel bar keeps its colour, and a 0.45-pixel outline covers about half
 * of the pixel next to the shape. */
static void test_effects_small_sizes(void)
{
    buf c = { 0 };
    fa_stream* s = open_wav_stream(1, FA_OPEN_DECODE_ONLY);
    fa_cdg_surface* f = NULL;
    fa_cdg_presentation p = pres(1, FA_SAFETY_SHOW);
    fa_surface_effects e;
    fa_surface_layout l;
    uint8_t *mem, *base;
    int r;
    colors(&c, 0, k_low);
    preset(&c, 0, 0);
    for (r = 8; r < 10; r++) {
        tile(&c, 0, 0, 1, r, 20, 0x20);   /* x 120 */
        tile(&c, 0, 0, 1, r, 22, 0x30);   /* x 132, 133 */
    }
    CHECK_EQ(fa_stream_load_cdg_memory(s, c.p, c.n), FA_OK);
    CHECK_EQ(fa_cdg_surface_create(s, NULL, &f), FA_OK);
    CHECK_EQ(fa_cdg_surface_set_presentation(f, &p), FA_OK);
    CHECK_EQ(fa_surface_set_presentation_size(f, 300, 216), FA_OK);
    base = full_draw(f, 200, 4, &l);
    CHECK(base);
    CHECK_EQ(l.scale, 1.0f);
#define SMALL_PX(m, x) ((m) + ((size_t)(l.content_y + 100) * l.width + l.content_x + (x)) * 4)
    fa_surface_effects_init(&e);
    e.bevel.on = 1;
    e.bevel.size = 1.0f;
    e.bevel.strength = 1.0f;
    CHECK_EQ(fa_surface_set_effects(f, &e), FA_OK);
    mem = full_draw(f, 200, 4, &l);
    CHECK(mem);
    CHECK(memcmp(SMALL_PX(mem, 120), SMALL_PX(base, 120), 4) == 0);
    CHECK(memcmp(SMALL_PX(mem, 132), SMALL_PX(base, 132), 4) != 0);
    CHECK(memcmp(SMALL_PX(mem, 133), SMALL_PX(base, 133), 4) != 0);
    free(mem);
    fa_surface_effects_init(&e);
    e.outline.on = 1;
    e.outline.size = 0.3f;
    e.outline.color = 0xFF00FF00u;
    CHECK_EQ(fa_surface_set_effects(f, &e), FA_OK);
    mem = full_draw(f, 200, 4, &l);
    CHECK(mem);
    CHECK(SMALL_PX(mem, 119)[3] > 100 && SMALL_PX(mem, 119)[3] < 150 && SMALL_PX(mem, 119)[1] == SMALL_PX(mem, 119)[3]);
    CHECK(SMALL_PX(mem, 121)[3] > 100 && SMALL_PX(mem, 121)[3] < 150);
    CHECK_EQ(SMALL_PX(mem, 122)[3], 0);
#undef SMALL_PX
    free(mem);
    free(base);
    /* The picture under the effects is blended as without them (smoothness 1 at 3x). */
    p.smoothness = 1;
    CHECK_EQ(fa_cdg_surface_set_presentation(f, &p), FA_OK);
    CHECK_EQ(fa_surface_set_presentation_size(f, 900, 648), FA_OK);
    CHECK_EQ(fa_surface_set_effects(f, NULL), FA_OK);
    base = full_draw(f, 200, 4, &l);
    fa_surface_effects_init(&e);
    e.shadow.on = 1;
    e.shadow.opacity = 0.01f;
    CHECK_EQ(fa_surface_set_effects(f, &e), FA_OK);
    mem = full_draw(f, 200, 4, &l);
    CHECK(base && mem);
    if (base && mem) {
        size_t i, most = 0;
        for (i = 0; i < (size_t)l.width * l.height * 4; i++) {
            size_t d = (size_t)abs(mem[i] - base[i]);
            if (d > most) most = d;
        }
        CHECK(most <= 4);
    }
    free(mem);
    free(base);
    fa_surface_destroy(f);
    fa_stream_close(s);
    free(c.p);
}

/* Outline, shadow and bevel where they belong around a red block (screen x 120..138,
 * y 96..120, at 2x); options checked; the classic picture ignores them. */
static void test_effects(void)
{
    buf c = { 0 };
    fa_stream* s = open_wav_stream(1, FA_OPEN_DECODE_ONLY);
    fa_cdg_surface *f = NULL, *plain = NULL, *classic = NULL, *classic_plain = NULL;
    fa_cdg_presentation p = pres(1, FA_SAFETY_SHOW);
    fa_surface_effects e, got;
    fa_surface_layout l;
    uint8_t *mem, *base;
    const uint8_t* q;
    int r, col;
    colors(&c, 0, k_low);
    preset(&c, 0, 0);
    for (r = 8; r < 10; r++)
        for (col = 20; col < 23; col++) tile(&c, 0, 1, 1, r, col, 0x3F);
    CHECK_EQ(fa_stream_load_cdg_memory(s, c.p, c.n), FA_OK);
    CHECK_EQ(fa_cdg_surface_create(s, NULL, &f), FA_OK);
    CHECK_EQ(fa_cdg_surface_create(s, NULL, &plain), FA_OK);
    CHECK_EQ(fa_cdg_surface_set_presentation(f, &p), FA_OK);
    CHECK_EQ(fa_cdg_surface_set_presentation(plain, &p), FA_OK);
    CHECK_EQ(fa_surface_set_presentation_size(f, 600, 432), FA_OK);
    CHECK_EQ(fa_surface_set_presentation_size(plain, 600, 432), FA_OK);
    base = full_draw(plain, 200, 4, &l);
    CHECK(base);
    CHECK_EQ(l.scale, 2.0f);
    CHECK_EQ(fx_px(base, &l, 125, 100, 0, 0)[2], 255);   /* red inside */
    CHECK_EQ(fx_px(base, &l, 138, 100, 0, 0)[3], 0);     /* transparent outside */

    /* Validation, defaults, NULL. */
    memset(&got, 0, sizeof(got));
    got.struct_size = sizeof(got);
    CHECK_EQ(fa_surface_get_effects(f, &got), FA_OK);
    CHECK(!got.outline.on && !got.shadow.on && !got.bevel.on);
    CHECK_EQ(got.shadow.angle_degrees, 315.0f);
    fa_surface_effects_init(&e);
    e.outline.size = 1.5f;
    CHECK_EQ(fa_surface_set_effects(f, &e), FA_ERROR_INVALID_ARG);
    fa_surface_effects_init(&e);
    e.shadow.on = 2;
    CHECK_EQ(fa_surface_set_effects(f, &e), FA_ERROR_INVALID_ARG);
    fa_surface_effects_init(&e);
    e.bevel.light_angle_degrees = NAN;
    CHECK_EQ(fa_surface_set_effects(f, &e), FA_ERROR_INVALID_ARG);

    /* Outline 1.0 = 1.5 CD+G pixels = 3 buffer pixels, hard. */
    fa_surface_effects_init(&e);
    e.outline.on = 1;
    e.outline.size = 1.0f;
    e.outline.color = 0xFF00FF00u;
    CHECK_EQ(fa_surface_set_effects(f, &e), FA_OK);
    CHECK_EQ(fa_surface_get_effects(f, &got), FA_OK);
    CHECK(got.outline.on && got.outline.size == 1.0f);
    mem = full_draw(f, 200, 4, &l);
    CHECK(mem);
    CHECK_EQ(fx_px(mem, &l, 125, 100, 0, 0)[2], 255);    /* the shape over it */
    CHECK_EQ(fx_px(mem, &l, 138, 100, 0, 0)[1], 255);    /* 0.5 px out: outline */
    CHECK_EQ(fx_px(mem, &l, 138, 100, 2, 0)[3], 255);    /* 2.5 px out */
    CHECK_EQ(fx_px(mem, &l, 138, 100, 2, 0)[1], 255);
    CHECK_EQ(fx_px(mem, &l, 138, 100, 4, 0)[3], 0);      /* 4.5 px out: nothing */
    CHECK_EQ(fx_px(mem, &l, 119, 100, 1, 0)[3], 255);    /* left side too */
    CHECK_EQ(fx_px(mem, &l, 125, 120, 0, 1)[1], 255);    /* and below */
    CHECK(fx_px(mem, &l, 138, 120, 3, 3)[3] < 128);      /* the corner is round: 4.2 px */
    free(mem);

    /* A hard shadow 1.0 = 2 CD+G pixels = 4 buffer pixels down right, at 60 %. */
    fa_surface_effects_init(&e);
    e.shadow.on = 1;
    e.shadow.distance = 1.0f;
    CHECK_EQ(fa_surface_set_effects(f, &e), FA_OK);
    mem = full_draw(f, 200, 4, &l);
    CHECK(mem);
    q = fx_px(mem, &l, 138, 120, 1, 1);
    CHECK(q[3] >= 150 && q[3] <= 156 && q[0] == 0 && q[1] == 0 && q[2] == 0);
    CHECK(fx_px(mem, &l, 138, 100, 2, 0)[3] > 128);     /* to the right: 2.8 px */
    CHECK_EQ(fx_px(mem, &l, 138, 100, 3, 0)[3], 0);
    CHECK_EQ(fx_px(mem, &l, 138, 100, 6, 0)[3], 0);      /* 6 px right: past it */
    CHECK_EQ(fx_px(mem, &l, 119, 100, 0, 0)[3], 0);      /* never up left */
    CHECK_EQ(fx_px(mem, &l, 125, 95, 0, 1)[3], 0);
    CHECK(memcmp(fx_px(mem, &l, 125, 100, 0, 0), fx_px(base, &l, 125, 100, 0, 0), 4) == 0);   /* under the shape */
    free(mem);

    /* A bevel lit from the top left: lighter along the top, darker along the bottom. */
    fa_surface_effects_init(&e);
    e.bevel.on = 1;
    e.bevel.strength = 1.0f;
    e.bevel.size = 1.0f;
    CHECK_EQ(fa_surface_set_effects(f, &e), FA_OK);
    mem = full_draw(f, 200, 4, &l);
    CHECK(mem);
    q = fx_px(mem, &l, 128, 96, 0, 0);
    CHECK(q[0] > 60 && q[1] > 60 && q[2] == 255);        /* top: white over red */
    q = fx_px(mem, &l, 128, 119, 0, 1);
    CHECK(q[2] < 200 && q[0] == 0 && q[1] == 0);         /* bottom: black over red */
    CHECK(memcmp(fx_px(mem, &l, 128, 108, 0, 0), fx_px(base, &l, 128, 108, 0, 0), 4) == 0);   /* middle: as is */
    free(mem);

    /* NULL: none again. */
    CHECK_EQ(fa_surface_set_effects(f, NULL), FA_OK);
    mem = full_draw(f, 200, 4, &l);
    CHECK(mem && memcmp(mem, base, (size_t)l.width * l.height * 4) == 0);
    free(mem);

    /* The classic picture ignores them. */
    CHECK_EQ(fa_cdg_surface_create(s, NULL, &classic), FA_OK);
    CHECK_EQ(fa_cdg_surface_create(s, NULL, &classic_plain), FA_OK);
    e = fx_set(1);
    CHECK_EQ(fa_surface_set_effects(classic, &e), FA_OK);
    mem = full_draw(classic, 200, 4, &l);
    CHECK(mem);
    {
        fa_surface_layout l2;
        uint8_t* other = full_draw(classic_plain, 200, 4, &l2);
        CHECK(other && memcmp(mem, other, (size_t)l.width * l.height * 4) == 0);
        free(other);
    }
    free(mem);
    free(base);
    fa_surface_destroy(classic);
    fa_surface_destroy(classic_plain);
    fa_surface_destroy(plain);
    fa_surface_destroy(f);
    fa_stream_close(s);
    free(c.p);

    /* A coloured border outside the crop casts nothing into the picture, on any side. */
    memset(&c, 0, sizeof(c));
    s = open_wav_stream(1, FA_OPEN_DECODE_ONLY);
    colors(&c, 0, k_low);
    preset(&c, 0, 0);
    border(&c, 2);
    CHECK_EQ(fa_stream_load_cdg_memory(s, c.p, c.n), FA_OK);
    CHECK_EQ(fa_cdg_surface_create(s, NULL, &f), FA_OK);
    p = pres(1, FA_SAFETY_REMOVE);
    CHECK_EQ(fa_cdg_surface_set_presentation(f, &p), FA_OK);
    CHECK_EQ(fa_surface_set_presentation_size(f, 576, 384), FA_OK);
    fa_surface_effects_init(&e);
    e.outline.on = 1;
    e.outline.size = 1.0f;
    e.shadow.on = 1;
    e.shadow.distance = 1.0f;
    CHECK_EQ(fa_surface_set_effects(f, &e), FA_OK);
    mem = full_draw(f, 200, 4, &l);
    CHECK(mem);
    CHECK_EQ(fx_px(mem, &l, 0, 100, 0, 0)[3], 0);              /* left edge */
    CHECK_EQ(fx_px(mem, &l, 287, 100, 1, 0)[3], 0);            /* right edge */
    CHECK_EQ(fx_px(mem, &l, 100, 0, 0, 0)[3], 0);              /* top */
    CHECK_EQ(fx_px(mem, &l, 100, 191, 0, 1)[3], 0);            /* bottom */
    free(mem);
    fa_surface_destroy(f);
    fa_stream_close(s);
    free(c.p);
}

static void test_presentation_incremental(void)
{
    static const struct { fa_safety_zone zone; uint32_t w, h; int smooth, blend, tb, padding, rgba, fx; } cfgs[] = {
        { FA_SAFETY_SHOW, 600, 432, 0, 0, 0, 0, 0, 0 },
        { FA_SAFETY_SHOW, 900, 648, 1, 0, 0, 0, 1, 0 },
        { FA_SAFETY_REMOVE, 600, 432, 2, 1, 0, 2, 0, 0 },
        { FA_SAFETY_SHOW, 900, 648, 4, 1, 1, 0, 0, 0 },
        { FA_SAFETY_SHOW, 600, 432, 0, 1, 1, 1, 0, 0 },
        { FA_SAFETY_SUPER, 900, 648, 3, 1, 0, 0, 0, 0 },
        { FA_SAFETY_SHOW, 1200, 864, 9, 1, 0, 0, 0, 0 },
        { FA_SAFETY_SUPER, 900, 648, 0, 0, 0, 0, 0, 0 },
        { FA_SAFETY_SUPER, 900, 648, 1, 0, 1, 1, 0, 0 },
        { FA_SAFETY_REMOVE, 1152, 768, 0, 1, 0, 0, 0, 0 },   /* edge blend near an edge the master does not reach */
        { FA_SAFETY_SHOW, 600, 432, 0, 0, 1, 0, 0, 0 },      /* a transparent border carried in by a scroll */
        { FA_SAFETY_REMOVE, 600, 432, 0, 0, 1, 0, 0, 0 },
        { FA_SAFETY_SHOW, 900, 648, 0, 0, 0, 0, 0, 1 },   /* effects: outline and a soft shadow */
        { FA_SAFETY_REMOVE, 900, 648, 2, 1, 0, 1, 1, 2 }, /* bevel and a hard outline */
        { FA_SAFETY_SHOW, 600, 432, 4, 0, 1, 2, 0, 3 },   /* a glow and a hard shadow far off */
        { FA_SAFETY_SHOW, 900, 648, 5, 1, 0, 0, 0, 0 },   /* contours: edge blend has nothing to add */
        { FA_SAFETY_SUPER, 600, 432, 6, 0, 1, 1, 1, 0 },
        { FA_SAFETY_REMOVE, 900, 648, 7, 0, 0, 2, 0, 0 },
        { FA_SAFETY_SHOW, 1200, 864, 8, 0, 1, 0, 0, 1 },
        { FA_SAFETY_SHOW, 900, 648, 10, 0, 0, 0, 1, 2 },
    };
    buf data[4];
    unsigned ci, di;
    data[0] = moves_cdg();
    data[1] = busy_cdg();
    data[2] = inner_moves_cdg();
    data[3] = edge_moves_cdg();
    for (di = 0; di < 4; di++)
        for (ci = 0; ci < sizeof(cfgs) / sizeof(cfgs[0]); ci++) {
            buf* c = &data[di];
            fa_stream* s = open_wav_stream(1, FA_OPEN_DECODE_ONLY);
            fa_cdg_surface *ref = NULL, *f = NULL;
            fa_cdg_surface_config cfg;
            fa_cdg_presentation p;
            fa_surface_layout l;
            fa_surface_frame fr;
            uint8_t* mem[3];
            uint64_t rev[3] = { 0, 0, 0 };
            size_t packs = c->n / FA_CDG_PACK_BYTES, n, k, size;
            uint32_t partial = 0, b, y;
            int32_t stride;
            fa_cdg_surface_config_init(&cfg);
            if (cfgs[ci].rgba) cfg.pixel_format = FA_CDG_PIXELS_RGBA32;
            CHECK_EQ(fa_stream_load_cdg_memory(s, c->p, c->n), FA_OK);
            CHECK_EQ(fa_cdg_surface_create(s, &cfg, &ref), FA_OK);
            CHECK_EQ(fa_cdg_surface_create(s, &cfg, &f), FA_OK);
            p = pres(1, cfgs[ci].zone);
            p.smoothness = cfgs[ci].smooth;
            p.edge_blend = cfgs[ci].blend;
            p.flags = cfgs[ci].tb ? FA_CDG_TRANSPARENT_BORDER : 0;
            p.padding = (uint32_t)cfgs[ci].padding;
            CHECK_EQ(fa_cdg_surface_set_presentation(f, &p), FA_OK);
            CHECK_EQ(fa_surface_set_presentation_size(f, cfgs[ci].w, cfgs[ci].h), FA_OK);
            CHECK_EQ(fa_surface_set_presentation_size(ref, cfgs[ci].w, cfgs[ci].h), FA_OK);
            if (cfgs[ci].fx) {
                fa_surface_effects fx = fx_set(cfgs[ci].fx);
                CHECK_EQ(fa_surface_set_effects(f, &fx), FA_OK);
                CHECK_EQ(fa_surface_set_effects(ref, &fx), FA_OK);
            }
            l = layout_of(f);
            stride = (int32_t)(l.width * 4 + 12);
            size = (size_t)stride * l.height;
            for (b = 0; b < 3; b++) { mem[b] = (uint8_t*)malloc(size); memset(mem[b], 0xCD, size); }
            for (n = 0; n < packs + 10; n++) {
                fa_surface_layout lr;
                uint8_t* want;
                b = (uint32_t)(n % 3);
                k = n < packs ? n + 1 : (n * 37) % (packs + 1);
                memset(&fr, 0, sizeof(fr));
                fr.struct_size = sizeof(fr);
                CHECK_EQ(fa_surface_update_into(f, t_packs((int64_t)k), mem[b], stride, l.width, l.height, rev[b], &fr), FA_OK);
                if (fr.base_revision) partial++;
                rev[b] = fr.revision;
                /* Compared every fourth step and at the end: a stale pixel stays until
                 * something draws over it, so it is still caught, and the test takes a
                 * quarter of the time (it was 55 s of every CI run). */
                if (n % 4 != 0 && n + 3 < packs + 10) continue;
                /* The reference draws its whole master again every time. */
                CHECK_EQ(fa_cdg_surface_set_presentation(ref, &p), FA_OK);
                want = full_draw(ref, (int64_t)k, 4, &lr);
                CHECK(want);
                CHECK_EQ(lr.width, l.width);
                for (y = 0; y < l.height; y++)
                    if (memcmp(mem[b] + (size_t)y * (size_t)stride, want + (size_t)y * l.width * 4, l.width * 4) != 0) {
                        uint32_t x;
                        for (x = 0; x < l.width * 4 && mem[b][(size_t)y * stride + x] == want[(size_t)y * l.width * 4 + x]; x++) {}
                        printf("    data %u config %u, step %u, pack %u, row %u, byte %u\n", di, ci, (unsigned)n,
                               (unsigned)k, y, x);
                        free(want);
                        CHECK(!"drawing only the changes differs from drawing all");
                    }
                for (y = 0; y < l.height; y++) CHECK_EQ(mem[b][(size_t)y * stride + l.width * 4], 0xCD);
                free(want);
            }
            CHECK(partial > packs / 2);
            for (b = 0; b < 3; b++) free(mem[b]);
            fa_surface_destroy(ref);
            fa_surface_destroy(f);
            fa_stream_close(s);
        }
    free(data[0].p);
    free(data[1].p);
    free(data[2].p);
    free(data[3].p);
}

/* The AUTO and SUPER zones from the pre-pass. */
static void test_safety_zone_prepass(void)
{
    fa_stream* s = open_wav_stream(4, FA_OPEN_DECODE_ONLY);
    fa_cdg_surface* f = NULL;
    fa_cdg_presentation p;
    fa_surface_layout l;
    uint64_t rev;
    buf c = { 0 };
    colors(&c, 0, k_low);
    preset(&c, 0, 0);
    border(&c, 3);                           /* a solid border does not count */
    tile(&c, 0, 0, 1, 3, 10, 0x3F);
    tile(&c, 0, 0, 2, 5, 20, 0x01);
    tile(&c, 0, 1, 1, 4, 15, 0x00);          /* c1 is used where bits are 1: none, all c0 */
    tile(&c, 0, 0, 0, 8, 30, 0x3F);          /* background only: not drawing */

    /* No graphics yet: SHOW, unresolved. */
    CHECK_EQ(fa_cdg_surface_create(s, NULL, &f), FA_OK);
    p = pres(0, FA_SAFETY_AUTO);
    CHECK_EQ(fa_cdg_surface_set_presentation(f, &p), FA_OK);
    l = layout_of(f);
    CHECK_EQ(l.safety_zone, FA_SAFETY_SHOW);
    CHECK_EQ(l.safety_zone_resolved, 0);
    rev = l.layout_revision;
    CHECK_EQ(fa_stream_load_cdg_memory(s, c.p, c.n), FA_OK);
    l = layout_of(f);
    CHECK_EQ(l.safety_zone, FA_SAFETY_REMOVE);
    CHECK_EQ(l.safety_zone_resolved, 1);
    CHECK_EQ(l.width, 288);
    CHECK(l.layout_revision > rev);

    /* SUPER: the tiles drawn, on the tile grid; centred: the same margin both sides. */
    p.safety_zone = FA_SAFETY_SUPER;
    CHECK_EQ(fa_cdg_surface_set_presentation(f, &p), FA_OK);
    l = layout_of(f);
    CHECK_EQ(l.safety_zone, FA_SAFETY_SUPER);
    CHECK_EQ(l.crop_x, 60); CHECK_EQ(l.crop_y, 36);
    CHECK_EQ(l.crop_width, 66); CHECK_EQ(l.crop_height, 36);
    p.super_centered = 1;
    CHECK_EQ(fa_cdg_surface_set_presentation(f, &p), FA_OK);
    l = layout_of(f);
    CHECK_EQ(l.crop_x, 60); CHECK_EQ(l.crop_width, 300 - 120);
    CHECK_EQ(l.crop_y, 36); CHECK_EQ(l.crop_height, 216 - 72);

    /* Drawing in the border at 1 s: AUTO shows it, unless told to ignore that long. */
    empty_packs_to(&c, 300);
    tile(&c, 0, 0, 4, 17, 25, 0x0C);
    CHECK_EQ(fa_stream_load_cdg_memory(s, c.p, c.n), FA_OK);
    p = pres(0, FA_SAFETY_AUTO);
    CHECK_EQ(fa_cdg_surface_set_presentation(f, &p), FA_OK);
    l = layout_of(f);
    CHECK_EQ(l.safety_zone, FA_SAFETY_SHOW);
    CHECK_EQ(l.width, 300);
    p.auto_ignore_seconds = 0.5f;
    CHECK_EQ(fa_cdg_surface_set_presentation(f, &p), FA_OK);
    CHECK_EQ(layout_of(f).safety_zone, FA_SAFETY_SHOW);
    p.auto_ignore_seconds = 2.0f;
    CHECK_EQ(fa_cdg_surface_set_presentation(f, &p), FA_OK);
    CHECK_EQ(layout_of(f).safety_zone, FA_SAFETY_REMOVE);
    /* Scrolled into the border counts too. */
    preset(&c, 0, 0);
    empty_packs_to(&c, 900);
    tile(&c, 0, 0, 2, 1, 25, 0x0C);
    scroll(&c, 0, 0, 0, 0, 2, 0);            /* up: row 1 into the border row 0 */
    CHECK_EQ(fa_stream_load_cdg_memory(s, c.p, c.n), FA_OK);
    CHECK_EQ(layout_of(f).safety_zone, FA_SAFETY_SHOW);
    p.safety_zone = FA_SAFETY_SUPER;
    p.super_centered = 0;
    CHECK_EQ(fa_cdg_surface_set_presentation(f, &p), FA_OK);
    l = layout_of(f);
    CHECK_EQ(l.crop_y, 0);
    CHECK_EQ(l.crop_height, 216);

    /* Settings checked. */
    p = pres(2, FA_SAFETY_SHOW);
    CHECK_EQ(fa_cdg_surface_set_presentation(f, &p), FA_ERROR_INVALID_ARG);
    p = pres(1, (fa_safety_zone)4);
    CHECK_EQ(fa_cdg_surface_set_presentation(f, &p), FA_ERROR_INVALID_ARG);
    p = pres(1, FA_SAFETY_SHOW);
    p.padding = 33;
    CHECK_EQ(fa_cdg_surface_set_presentation(f, &p), FA_ERROR_INVALID_ARG);
    p.padding = 0;
    p.smoothness = 11;
    CHECK_EQ(fa_cdg_surface_set_presentation(f, &p), FA_ERROR_INVALID_ARG);
    p.smoothness = 0;
    p.auto_ignore_seconds = -1.0f;
    CHECK_EQ(fa_cdg_surface_set_presentation(f, &p), FA_ERROR_INVALID_ARG);
    p.auto_ignore_seconds = 0.0f;
    p.flags = 2;
    CHECK_EQ(fa_cdg_surface_set_presentation(f, &p), FA_ERROR_INVALID_ARG);
    p.flags = 0;
    p.struct_size = 0;
    CHECK_EQ(fa_cdg_surface_set_presentation(f, &p), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_cdg_surface_set_presentation(NULL, NULL), FA_ERROR_INVALID_ARG);
    /* An older, shorter struct: the rest takes the defaults. */
    p = pres(1, FA_SAFETY_REMOVE);
    p.smoothness = 3;
    p.struct_size = (uint32_t)offsetof(fa_cdg_presentation, padding);
    CHECK_EQ(fa_cdg_surface_set_presentation(f, &p), FA_OK);
    memset(&p, 0x55, sizeof(p));
    p.struct_size = sizeof(p);
    CHECK_EQ(fa_cdg_surface_get_presentation(f, &p), FA_OK);
    CHECK_EQ(p.scale, 1); CHECK_EQ(p.safety_zone, FA_SAFETY_REMOVE); CHECK_EQ(p.smoothness, 0);
    fa_surface_destroy(f);
    fa_stream_close(s);
    free(c.p);
}

/* Smoothness: every level keeps flat areas as they are and only blends next to a
 * colour change, more pixels the higher the level; edge blend turns a staircase into
 * partly covered pixels and leaves a straight edge alone. All in premultiplied colour
 * between the two colours that meet. */
/* Contour mode (smoothness 5-10) on random screens: with the smoothing off the traced
 * outlines draw exactly the nearest picture; drawing around a change gives what
 * drawing everything gives; a picture moved by whole pixels draws the same pixels
 * (scrolls move the master); and two colours meet without a seam. */
static void cm_screen(uint8_t s[CDG_SCALE_H][CDG_SCALE_W], uint32_t* rs, int shapes)
{
    int i, x, y;
    memset(s, 0, (size_t)CDG_SCALE_H * CDG_SCALE_W);
    for (i = 0; i < shapes; i++) {
        int w, h, x0, y0, c, kind;
        *rs = *rs * 1103515245u + 12345u; w = 1 + (int)((*rs >> 16) % 8);
        *rs = *rs * 1103515245u + 12345u; h = 1 + (int)((*rs >> 16) % 8);
        *rs = *rs * 1103515245u + 12345u; x0 = (int)((*rs >> 16) % CDG_SCALE_W);
        *rs = *rs * 1103515245u + 12345u; y0 = (int)((*rs >> 16) % CDG_SCALE_H);
        *rs = *rs * 1103515245u + 12345u; c = (int)((*rs >> 16) % CDG_SCALE_COLORS);
        *rs = *rs * 1103515245u + 12345u; kind = (int)((*rs >> 16) % 3);
        for (y = y0; y < y0 + h && y < CDG_SCALE_H; y++)
            for (x = x0; x < x0 + w && x < CDG_SCALE_W; x++) {
                if (kind == 1 && (x - x0) * h / w != y - y0) continue;   /* a diagonal */
                if (kind == 2 && (x + y) % 2) continue;                  /* a checkerboard */
                s[y][x] = (uint8_t)c;
            }
    }
}

static void test_contours(void)
{
    static uint8_t a[CDG_SCALE_H][CDG_SCALE_W], b[CDG_SCALE_H][CDG_SCALE_W];
    static const int levels[] = { 0, 5, 6, 7, 8, 9, 10 };
    uint8_t pm[CDG_SCALE_COLORS][4];
    uint32_t n = 3, rs = 7;
    size_t stride = (size_t)CDG_SCALE_W * n * 4, size = stride * CDG_SCALE_H * n;
    uint8_t *o1 = (uint8_t*)malloc(size), *o2 = (uint8_t*)malloc(size);
    int32_t* acc = (int32_t*)malloc(CDG_CONTOUR_ACC_BYTES(n));
    cdg_contours* ct = NULL;
    unsigned li;
    int i, it;
    CHECK(o1 && o2 && acc);
    for (i = 0; i < CDG_SCALE_COLORS; i++) {
        pm[i][0] = (uint8_t)(i * 13);
        pm[i][1] = (uint8_t)(i * 7 + 20);
        pm[i][2] = (uint8_t)(255 - i * 9);
        pm[i][3] = 255;
    }
    memset(pm[16], 0, 4);
    memset(acc, 0x5A, CDG_CONTOUR_ACC_BYTES(n));   /* shared scratch: any contents */
    for (li = 0; li < sizeof(levels) / sizeof(levels[0]); li++) {
        int level = levels[li], reach = cdg_contour_reach(level);
        for (it = 0; it < (FA_TEST_SANITIZED ? 6 : 25); it++) {
            int x0, y0, x1, y1, dx, dy, x, y, m;
            cm_screen(a, &rs, 400);
            CHECK(cdg_contours_build(&ct, (const uint8_t(*)[CDG_SCALE_W])a, level, n));
            cdg_contours_draw(ct, (const uint8_t(*)[CDG_SCALE_W])a, (const uint8_t(*)[4])pm, o1, stride, 0, 0, 0, 0,
                              CDG_SCALE_W, CDG_SCALE_H, acc);
            if (level == 0)
                for (y = 0; y < CDG_SCALE_H * (int)n; y++)
                    for (x = 0; x < CDG_SCALE_W * (int)n; x++)
                        if (memcmp(o1 + (size_t)y * stride + (size_t)x * 4, pm[a[y / (int)n][x / (int)n]], 4) != 0) {
                            printf("    traced edges differ from nearest at %d, %d\n", x, y);
                            CHECK(0);
                            goto next_level;
                        }
            /* A change, drawn around it, against everything drawn. */
            memcpy(b, a, sizeof(a));
            rs = rs * 1103515245u + 12345u; x0 = (int)((rs >> 16) % CDG_SCALE_W);
            rs = rs * 1103515245u + 12345u; y0 = (int)((rs >> 16) % CDG_SCALE_H);
            x1 = x0 + 6 < CDG_SCALE_W ? x0 + 6 : CDG_SCALE_W;
            y1 = y0 + 12 < CDG_SCALE_H ? y0 + 12 : CDG_SCALE_H;
            for (y = y0; y < y1; y++)
                for (x = x0; x < x1; x++) {
                    rs = rs * 1103515245u + 12345u;
                    b[y][x] = (uint8_t)((rs >> 16) % 3);
                }
            CHECK(cdg_contours_build(&ct, (const uint8_t(*)[CDG_SCALE_W])b, level, n));
            cdg_contours_draw(ct, (const uint8_t(*)[CDG_SCALE_W])b, (const uint8_t(*)[4])pm, o1, stride, 0, 0, x0 - reach,
                              y0 - reach, x1 + reach, y1 + reach, acc);
            cdg_contours_draw(ct, (const uint8_t(*)[CDG_SCALE_W])b, (const uint8_t(*)[4])pm, o2, stride, 0, 0, 0, 0,
                              CDG_SCALE_W, CDG_SCALE_H, acc);
            if (memcmp(o1, o2, size) != 0) {
                printf("    level %d: drawing around a change differs from drawing all\n", level);
                CHECK(0);
                goto next_level;
            }
            /* Moved by whole pixels: the same pixels away from the screen's edges. */
            rs = rs * 1103515245u + 12345u; dx = (int)((rs >> 16) % 13) - 6;
            rs = rs * 1103515245u + 12345u; dy = (int)((rs >> 16) % 25) - 12;
            for (y = 0; y < CDG_SCALE_H; y++)
                for (x = 0; x < CDG_SCALE_W; x++) {
                    int sx = x - dx, sy = y - dy;
                    a[y][x] = sx >= 0 && sy >= 0 && sx < CDG_SCALE_W && sy < CDG_SCALE_H ? b[sy][sx] : 0;
                }
            CHECK(cdg_contours_build(&ct, (const uint8_t(*)[CDG_SCALE_W])a, level, n));
            cdg_contours_draw(ct, (const uint8_t(*)[CDG_SCALE_W])a, (const uint8_t(*)[4])pm, o1, stride, 0, 0, 0, 0,
                              CDG_SCALE_W, CDG_SCALE_H, acc);
            m = reach + 14;
            for (y = m * (int)n; y < (CDG_SCALE_H - m) * (int)n; y++)
                if (memcmp(o1 + (size_t)y * stride + (size_t)m * n * 4,
                           o2 + (size_t)(y - dy * (int)n) * stride + (size_t)(m - dx) * n * 4,
                           (size_t)(CDG_SCALE_W - 2 * m) * n * 4) != 0) {
                    printf("    level %d: moved by %d, %d draws differently (row %d)\n", level, dx, dy, y);
                    CHECK(0);
                    goto next_level;
                }
        }
        {
            /* Red against transparent only: R equals A and B, G stay 0 at every pixel
             * (premultiplied blends of the two colours, no seam of anything else). */
            uint8_t two[CDG_SCALE_COLORS][4];
            int x, y;
            memset(two, 0, sizeof(two));
            two[1][2] = two[1][3] = 255;   /* BGRA red */
            cm_screen(a, &rs, 300);
            for (y = 0; y < CDG_SCALE_H; y++)
                for (x = 0; x < CDG_SCALE_W; x++) a[y][x] = a[y][x] ? 1 : 16;
            CHECK(cdg_contours_build(&ct, (const uint8_t(*)[CDG_SCALE_W])a, level, n));
            cdg_contours_draw(ct, (const uint8_t(*)[CDG_SCALE_W])a, (const uint8_t(*)[4])two, o1, stride, 0, 0, 0, 0,
                              CDG_SCALE_W, CDG_SCALE_H, acc);
            for (i = 0; i < CDG_SCALE_W * (int)n * CDG_SCALE_H * (int)n; i++) {
                const uint8_t* q = o1 + (size_t)i * 4;
                if (q[0] || q[1] || q[2] != q[3]) {
                    printf("    level %d: pixel %d is %u %u %u %u\n", level, i, q[0], q[1], q[2], q[3]);
                    CHECK(0);
                    break;
                }
            }
        }
    next_level:;
    }
    {
        /* One pixel added next to a large ellipse: tracing order makes some of its
         * outline run the other way, which must not change anything far from the
         * pixel (it did, by 1, before points and curves were stored the same either
         * way). */
        uint32_t n8 = 8;
        size_t stride8 = (size_t)CDG_SCALE_W * n8 * 4, size8 = stride8 * CDG_SCALE_H * n8;
        uint8_t *e1 = (uint8_t*)malloc(size8), *e2 = (uint8_t*)malloc(size8);
        int32_t* acc8 = (int32_t*)malloc(CDG_CONTOUR_ACC_BYTES(n8));
        int level, x, y, tried = 0;
        CHECK(e1 && e2 && acc8);
        for (y = 0; y < CDG_SCALE_H; y++)
            for (x = 0; x < CDG_SCALE_W; x++) {
                float u = ((float)x + 0.5f - 150.3f) / 60.0f, v = ((float)y + 0.5f - 108.7f) / 45.0f;
                a[y][x] = (uint8_t)(u * u + v * v < 1.0f);
            }
        for (level = 8; level <= 10; level += 2)
            for (y = 1; y < CDG_SCALE_H - 1; y++)
                for (x = 1; x < CDG_SCALE_W - 1; x++) {
                    int reach = cdg_contour_reach(level);
                    if (a[y][x] || !(a[y - 1][x] || a[y + 1][x] || a[y][x - 1] || a[y][x + 1])) continue;
                    if (++tried % (FA_TEST_SANITIZED ? 40 : 9) != 0 && !(x == 210 && y == 101)) continue;
                    CHECK(cdg_contours_build(&ct, (const uint8_t(*)[CDG_SCALE_W])a, level, n8));
                    cdg_contours_draw(ct, (const uint8_t(*)[CDG_SCALE_W])a, (const uint8_t(*)[4])pm, e1, stride8, 0, 0, 0, 0,
                                      CDG_SCALE_W, CDG_SCALE_H, acc8);
                    memcpy(b, a, sizeof(a));
                    b[y][x] = 2;
                    CHECK(cdg_contours_build(&ct, (const uint8_t(*)[CDG_SCALE_W])b, level, n8));
                    cdg_contours_draw(ct, (const uint8_t(*)[CDG_SCALE_W])b, (const uint8_t(*)[4])pm, e1, stride8, 0, 0,
                                      x - reach, y - reach, x + 1 + reach, y + 1 + reach, acc8);
                    cdg_contours_draw(ct, (const uint8_t(*)[CDG_SCALE_W])b, (const uint8_t(*)[4])pm, e2, stride8, 0, 0, 0, 0,
                                      CDG_SCALE_W, CDG_SCALE_H, acc8);
                    if (memcmp(e1, e2, size8) != 0) {
                        printf("    level %d: a pixel at %d, %d changes the ellipse beyond the reach\n", level, x, y);
                        CHECK(0);
                        level = 11;
                        y = CDG_SCALE_H;
                        break;
                    }
                }
        free(e1);
        free(e2);
        free(acc8);
    }
    cdg_contours_free(ct);
    free(acc);
    free(o1);
    free(o2);
}

static void test_smoothness_and_edge_blend(void)
{
    fa_stream* s = open_wav_stream(1, FA_OPEN_DECODE_ONLY);
    fa_cdg_surface* f = NULL;
    fa_cdg_presentation p;
    fa_surface_layout l;
    buf c = { 0 };
    uint8_t* a;
    int level, prev = -1, i;
    colors(&c, 0, k_low);
    preset(&c, 0, 0);
    /* A staircase in red: rows 0..11 of tile (8, 20) hold 1, 2, 3.. pixels; a block
     * with a straight top edge three tiles wide. */
    {
        uint8_t d[16];
        d[0] = 0; d[1] = 1; d[2] = 8; d[3] = 20;
        for (i = 0; i < 12; i++) d[4 + i] = (uint8_t)(0x3F << (5 - (i / 2 < 5 ? i / 2 : 5)) & 0x3F);
        pack(&c, 6, d);
    }
    for (i = 0; i < 3; i++) tile(&c, 0, 1, 1, 12, 20 + i, 0x00);
    CHECK_EQ(fa_stream_load_cdg_memory(s, c.p, c.n), FA_OK);
    CHECK_EQ(fa_cdg_surface_create(s, NULL, &f), FA_OK);
    CHECK_EQ(fa_surface_set_presentation_size(f, 2400, 1728), FA_OK);   /* 8x */
    for (level = 0; level <= 11; level++) {
        int mixed = 0;
        uint32_t x, y;
        p = pres(1, FA_SAFETY_SHOW);
        p.smoothness = level < 5 ? level : level == 5 ? 0 : level - 1;   /* 5: edge blend; 6..11: smoothness 5..10 */
        p.edge_blend = level >= 5;
        CHECK_EQ(fa_cdg_surface_set_presentation(f, &p), FA_OK);
        a = full_draw(f, 20, 4, &l);
        CHECK(a);
        CHECK(l.scale == 8.0f);
        for (y = 0; y < l.height; y++)
            for (x = 0; x < l.width; x++) {
                const uint8_t* q = a + ((size_t)y * l.width + x) * 4;
                /* Red over transparent: B and G stay 0, R equals A. */
                CHECK(q[0] == 0 && q[1] == 0 && q[2] == q[3]);
                if (q[3] != 0 && q[3] != 255) mixed++;
            }
        /* Far from any edge: solid red inside the block, transparent outside. */
        CHECK_EQ(a[(((size_t)(12 * 12 + 6) * 8 + 8) * l.width + (size_t)(21 * 6 + 3) * 8 + 8) * 4 + 3], 255);
        CHECK_EQ(a[(((size_t)100 * 8 + 8) * l.width + (size_t)100 * 8 + 8) * 4 + 3], 0);
        if (level >= 1 && level <= 4) CHECK(mixed >= prev);   /* higher is never sharper */
        if (level == 0) CHECK_EQ(mixed, 0);
        if (level >= 6) {
            /* Edge blend adds nothing to the outlines: the same picture without it. */
            uint8_t* nb;
            p.edge_blend = 0;
            CHECK_EQ(fa_cdg_surface_set_presentation(f, &p), FA_OK);
            nb = full_draw(f, 20, 4, &l);
            CHECK(nb && memcmp(a, nb, (size_t)l.width * l.height * 4) == 0);
            free(nb);
        }
        if (level >= 5) {
            /* The block's straight top edge (row 144, columns well inside it) is not
             * touched; the staircase is. */
            uint32_t top = (12 * 12) * 8 + 8, mid = (21 * 6) * 8 + 8;
            CHECK_EQ(a[((size_t)top * l.width + mid) * 4 + 3], 255);
            CHECK_EQ(a[((size_t)(top - 1) * l.width + mid) * 4 + 3], 0);
            CHECK(mixed > 0);
        }
        prev = level >= 1 && level <= 4 ? mixed : prev;
        free(a);
    }
    fa_surface_destroy(f);
    fa_stream_close(s);
    free(c.p);
}

/* `got` (stride `stride`) against `ref` drawn in full at stream time `t`. */
static int same_as_at(fa_surface* ref, fa_time t, const uint8_t* got, int32_t stride, uint32_t bpp)
{
    fa_surface_layout l = layout_of(ref);
    fa_surface_frame fr;
    uint8_t* want = (uint8_t*)malloc((size_t)l.width * l.height * bpp);
    uint32_t y;
    int same = 1;
    memset(&fr, 0, sizeof(fr));
    fr.struct_size = sizeof(fr);
    if (!want || fa_surface_update_into(ref, t, want, (int32_t)(l.width * bpp), l.width, l.height, 0, &fr) != FA_OK) {
        free(want);
        return 0;
    }
    for (y = 0; y < l.height && same; y++)
        same = memcmp(got + (size_t)y * (size_t)stride, want + (size_t)y * l.width * bpp, l.width * bpp) == 0;
    free(want);
    return same;
}

/* update_into at `t` until the frame is no longer behind (a worker renders it). */
static void settle(fa_surface* f, fa_time t, uint8_t* mem, int32_t stride, const fa_surface_layout* l, uint64_t* rev,
                   fa_surface_frame* fr)
{
    int i;
    for (i = 0; i < 4000; i++) {
        memset(fr, 0, sizeof(*fr));
        fr->struct_size = sizeof(*fr);
        CHECK_EQ(fa_surface_update_into(f, t, mem, stride, l->width, l->height, *rev, fr), FA_OK);
        *rev = fr->revision;
        if (!fr->behind) break;
        fa_sleep_ms(1);
    }
}

/* Render-ahead: every frame shown is the picture of the position it reports, drawn
 * only where it changed; a frame a worker has not finished yet shows as behind and
 * arrives later; options and the classic picture are as without it; surfaces and
 * streams closed while a worker has work for them. */
static void test_render_ahead(void)
{
    buf c = busy_cdg();
    size_t packs = c.n / FA_CDG_PACK_BYTES, n;
    fa_stream* s = open_wav_stream(1, FA_OPEN_DECODE_ONLY);
    fa_cdg_surface *f = NULL, *ref = NULL, *classic = NULL, *classic_ref = NULL;
    fa_cdg_presentation p = pres(1, FA_SAFETY_SHOW);
    fa_surface_layout l;
    fa_surface_frame fr;
    uint8_t* mem[2];
    uint64_t rev[2] = { 0, 0 };
    int32_t stride;
    uint32_t b, behind = 0, partial = 0, i;
    p.smoothness = 2;
    p.edge_blend = 1;
    CHECK_EQ(fa_stream_load_cdg_memory(s, c.p, c.n), FA_OK);
    CHECK_EQ(fa_cdg_surface_create(s, NULL, &f), FA_OK);
    CHECK_EQ(fa_cdg_surface_create(s, NULL, &ref), FA_OK);
    CHECK_EQ(fa_cdg_surface_set_presentation(f, &p), FA_OK);
    CHECK_EQ(fa_cdg_surface_set_presentation(ref, &p), FA_OK);
    CHECK_EQ(fa_surface_set_presentation_size(f, 900, 648), FA_OK);
    CHECK_EQ(fa_surface_set_presentation_size(ref, 900, 648), FA_OK);
    CHECK_EQ(fa_surface_set_render_ahead(f, 1), FA_OK);
    l = layout_of(f);
    stride = (int32_t)(l.width * 4 + 8);
    for (b = 0; b < 2; b++) mem[b] = (uint8_t*)calloc((size_t)stride * l.height, 1);

    /* Two packs a frame through the song, two buffers in turn. */
    for (n = 0; n <= packs + 4; n += 2) {
        b = (uint32_t)(n / 2 % 2);
        memset(&fr, 0, sizeof(fr));
        fr.struct_size = sizeof(fr);
        CHECK_EQ(fa_surface_update_into(f, t_packs((int64_t)n), mem[b], stride, l.width, l.height, rev[b], &fr), FA_OK);
        if (fr.behind) behind++;
        if (fr.base_revision) partial++;
        CHECK(fr.revision >= rev[b] || !fr.base_revision);
        rev[b] = fr.revision;
        if (!same_as_at(ref, fr.time, mem[b], stride, 4)) {
            printf("    step %u: the frame differs from the picture at its time\n", (unsigned)n);
            CHECK(!"render-ahead frame");
        }
    }
    printf("    %u of %u frames behind, %u partial\n", behind, (unsigned)(packs / 2 + 3), partial);
    CHECK(partial > 0);

    /* Paced like a display (timing-dependent, so only reported): the worker has the
     * next frame ready. */
    behind = 0;
    for (n = 0; n <= packs + 4; n += 2) {
        b = (uint32_t)(n / 2 % 2);
        memset(&fr, 0, sizeof(fr));
        fr.struct_size = sizeof(fr);
        CHECK_EQ(fa_surface_update_into(f, t_packs((int64_t)n), mem[b], stride, l.width, l.height, rev[b], &fr), FA_OK);
        if (fr.behind) behind++;
        rev[b] = fr.revision;
        CHECK(same_as_at(ref, fr.time, mem[b], stride, 4));
        fa_sleep_ms(7);
    }
    printf("    paced: %u of %u frames behind\n", behind, (unsigned)(packs / 2 + 3));

    /* Paused somewhere, and after seeks back and forward: the frame arrives. */
    {
        static const size_t at[] = { 30, 5, 70, 69, 0 };
        for (i = 0; i < 5; i++) {
            size_t k = at[i] < packs ? at[i] : packs;
            settle(f, t_packs((int64_t)k), mem[0], stride, &l, &rev[0], &fr);
            CHECK(!fr.behind);
            CHECK(same_as_at(ref, t_packs((int64_t)k), mem[0], stride, 4));
        }
    }

    /* The surface's own pixels with render-ahead on: as always. */
    {
        fa_cdg_frame cf, rf;
        CHECK_EQ(fa_cdg_surface_update_at(f, t_packs(50), &cf), FA_OK);
        CHECK_EQ(fa_cdg_surface_update_at(ref, t_packs(50), &rf), FA_OK);
        CHECK_EQ(cf.width, rf.width);
        CHECK_EQ(cf.height, rf.height);
        CHECK(memcmp(cf.pixels, rf.pixels, (size_t)cf.stride * cf.height) == 0);
    }

    /* Other graphics loaded: the old song's frame is not kept. */
    {
        buf other = moves_cdg();
        CHECK_EQ(fa_stream_load_cdg_memory(s, other.p, other.n), FA_OK);
        settle(f, t_packs(20), mem[0], stride, &l, &rev[0], &fr);
        CHECK(!fr.behind);
        CHECK(same_as_at(ref, t_packs(20), mem[0], stride, 4));
        CHECK_EQ(fa_stream_load_cdg_memory(s, c.p, c.n), FA_OK);
        settle(f, t_packs(20), mem[0], stride, &l, &rev[0], &fr);
        CHECK(!fr.behind);
        CHECK(same_as_at(ref, t_packs(20), mem[0], stride, 4));
        free(other.p);
    }

    /* New options are shown at once, not after a worker's frame. */
    p.smoothness = 0;
    p.edge_blend = 0;
    CHECK_EQ(fa_cdg_surface_set_presentation(f, &p), FA_OK);
    CHECK_EQ(fa_cdg_surface_set_presentation(ref, &p), FA_OK);
    memset(&fr, 0, sizeof(fr));
    fr.struct_size = sizeof(fr);
    CHECK_EQ(fa_surface_update_into(f, t_packs(40), mem[0], stride, l.width, l.height, rev[0], &fr), FA_OK);
    CHECK(!fr.behind);
    rev[0] = fr.revision;
    CHECK(same_as_at(ref, t_packs(40), mem[0], stride, 4));

    /* Off again: drawn on the caller's thread, never behind. */
    CHECK_EQ(fa_surface_set_render_ahead(f, 0), FA_OK);
    for (n = 41; n < 60 && n <= packs; n++) {
        memset(&fr, 0, sizeof(fr));
        fr.struct_size = sizeof(fr);
        CHECK_EQ(fa_surface_update_into(f, t_packs((int64_t)n), mem[0], stride, l.width, l.height, rev[0], &fr), FA_OK);
        CHECK(!fr.behind);
        rev[0] = fr.revision;
        CHECK(same_as_at(ref, t_packs((int64_t)n), mem[0], stride, 4));
    }

    /* The classic picture ignores the setting. */
    CHECK_EQ(fa_cdg_surface_create(s, NULL, &classic), FA_OK);
    CHECK_EQ(fa_cdg_surface_create(s, NULL, &classic_ref), FA_OK);
    CHECK_EQ(fa_surface_set_render_ahead(classic, 1), FA_OK);
    {
        fa_surface_layout lc = layout_of(classic);
        int32_t cs = (int32_t)(lc.width * 4);
        uint8_t* cm = (uint8_t*)calloc((size_t)cs * lc.height, 1);
        uint64_t cr = 0;
        for (n = 0; n <= packs; n += 3) {
            memset(&fr, 0, sizeof(fr));
            fr.struct_size = sizeof(fr);
            CHECK_EQ(fa_surface_update_into(classic, t_packs((int64_t)n), cm, cs, lc.width, lc.height, cr, &fr), FA_OK);
            CHECK(!fr.behind);
            cr = fr.revision;
            CHECK(same_as_at(classic_ref, t_packs((int64_t)n), cm, cs, 4));
        }
        free(cm);
    }
    fa_surface_destroy(classic);
    fa_surface_destroy(classic_ref);
    for (b = 0; b < 2; b++) free(mem[b]);
    fa_surface_destroy(ref);
    fa_surface_destroy(f);
    fa_stream_close(s);

    /* Destroyed, or their stream closed, with work queued or running. */
    for (i = 0; i < 40; i++) {
        fa_stream* s2 = open_wav_stream(1, FA_OPEN_DECODE_ONLY);
        fa_cdg_surface* g = NULL;
        fa_cdg_presentation q = pres(1, FA_SAFETY_SHOW);
        fa_surface_layout lg;
        uint8_t* m;
        uint64_t r = 0;
        q.smoothness = 4;
        q.edge_blend = 1;
        CHECK_EQ(fa_stream_load_cdg_memory(s2, c.p, c.n), FA_OK);
        CHECK_EQ(fa_cdg_surface_create(s2, NULL, &g), FA_OK);
        CHECK_EQ(fa_cdg_surface_set_presentation(g, &q), FA_OK);
        CHECK_EQ(fa_surface_set_presentation_size(g, 1920, 1080), FA_OK);
        CHECK_EQ(fa_surface_set_render_ahead(g, 1), FA_OK);
        lg = layout_of(g);
        m = (uint8_t*)malloc((size_t)lg.width * lg.height * 4);
        for (n = 0; n < 6; n++) {
            memset(&fr, 0, sizeof(fr));
            fr.struct_size = sizeof(fr);
            CHECK_EQ(fa_surface_update_into(g, t_packs((int64_t)(n * 20)), m, (int32_t)(lg.width * 4), lg.width, lg.height, r, &fr), FA_OK);
            r = fr.revision;
        }
        if (i % 2) {
            fa_surface_destroy(g);
            fa_stream_close(s2);
        } else {
            fa_stream_close(s2);
            memset(&fr, 0, sizeof(fr));
            fr.struct_size = sizeof(fr);
            CHECK_EQ(fa_surface_update_into(g, 0, m, (int32_t)(lg.width * 4), lg.width, lg.height, r, &fr), FA_ERROR_INVALID_ARG);
            fa_surface_destroy(g);
        }
        free(m);
    }
    free(c.p);
}

/* Prints what presentation scaling costs, for the table in docs/features/cdg.md: a
 * full redraw of a screen of lyrics, and a typical update while they are coloured in
 * (four tiles), into buffers in rotation. Checks nothing but that it runs. */
static void test_presentation_costs(void)
{
    static const struct { const char* name; int smooth, blend, fx; } levels[] = {
        { "smoothness 0", 0, 0, 0 }, { "smoothness 2", 2, 0, 0 }, { "smoothness 4", 4, 0, 0 },
        { "smoothness 4 + edge blend", 4, 1, 0 }, { "+ outline 0.5, shadow 0.5", 4, 1, 2 },
        { "+ all effects at maximum", 4, 1, 1 }, { "smoothness 5 (contours)", 5, 0, 0 },
        { "smoothness 6", 6, 0, 0 }, { "smoothness 8", 8, 0, 0 }, { "smoothness 10 (curves)", 10, 0, 0 },
        { "10 + outline 0.5, shadow 0.5", 10, 0, 2 },
    };
    static const uint32_t sizes[2][2] = { { 1920, 1080 }, { 3840, 2160 } };
    buf c = { 0 };
    fa_stream* s = open_wav_stream(2, FA_OPEN_DECODE_ONLY);
    fa_cdg_surface* f = NULL;
    int row, col, li, si, k;
    colors(&c, 0, k_low);
    preset(&c, 0, 0);
    /* Six lines of "letters" in white, then 200 tiles coloured in red, four per step. */
    for (row = 3; row < 15; row += 2)
        for (col = 3; col < 47; col++) tile(&c, 0, 0, 4, row, col, (uint8_t)(0x15 ^ (col * 7 + row)));
    for (k = 0; k < 200; k++) tile(&c, 0, 0, 1, 3 + 2 * ((k / 44) % 6), 3 + k % 44, (uint8_t)(0x2A ^ k));
    CHECK_EQ(fa_stream_load_cdg_memory(s, c.p, c.n), FA_OK);
    CHECK_EQ(fa_cdg_surface_create(s, NULL, &f), FA_OK);
    printf("  %-28s %10s %10s %10s %10s\n", "", "1080p full", "typical", "2160p full", "typical");
    for (li = 0; li < (int)(sizeof(levels) / sizeof(levels[0])); li++) {
        double full[2] = { 0, 0 }, typical[2] = { 0, 0 };
        fa_surface_effects fx;
        fa_surface_effects_init(&fx);
        if (levels[li].fx == 1) {
            /* Everything on at its largest. */
            fx.outline.on = fx.shadow.on = fx.bevel.on = 1;
            fx.outline.size = fx.shadow.size = fx.shadow.distance = fx.bevel.size = fx.bevel.strength = 1.0f;
            fx.shadow.fade = 1.0f;
        } else if (levels[li].fx == 2) {
            /* A look like MediaControl's today: an outline and a hard shadow. */
            fx.outline.on = fx.shadow.on = 1;
        }
        CHECK_EQ(fa_surface_set_effects(f, &fx), FA_OK);
        for (si = 0; si < (FA_TEST_SANITIZED ? 1 : 2); si++) {   /* sanitizers: timings mean nothing */
            fa_cdg_presentation p = pres(1, FA_SAFETY_SHOW);
            fa_surface_layout l;
            fa_surface_frame fr;
            uint8_t* mem[3];
            uint64_t rev[3] = { 0, 0, 0 };
            int64_t sum = 0, best = INT64_MAX;
            int32_t stride;
            uint32_t b;
            int64_t base = (int64_t)(c.n / FA_CDG_PACK_BYTES) - 200;
            p.smoothness = levels[li].smooth;
            p.edge_blend = levels[li].blend;
            CHECK_EQ(fa_cdg_surface_set_presentation(f, &p), FA_OK);
            CHECK_EQ(fa_surface_set_presentation_size(f, sizes[si][0], sizes[si][1]), FA_OK);
            l = layout_of(f);
            stride = (int32_t)(l.width * 4);
            for (b = 0; b < 3; b++) mem[b] = (uint8_t*)calloc((size_t)stride, l.height);
            for (k = 0; k < 3; k++) {   /* a full redraw: the options set again */
                CHECK_EQ(fa_cdg_surface_set_presentation(f, &p), FA_OK);
                memset(&fr, 0, sizeof(fr));
                fr.struct_size = sizeof(fr);
                CHECK_EQ(fa_surface_update_into(f, t_packs(base), mem[0], stride, l.width, l.height, 0, &fr), FA_OK);
                if (fr.draw_ns < best) best = fr.draw_ns;
            }
            rev[0] = fr.revision;
            full[si] = (double)best / 1e6;
            for (k = 1; k <= 48; k++) {
                b = (uint32_t)(k % 3);
                memset(&fr, 0, sizeof(fr));
                fr.struct_size = sizeof(fr);
                CHECK_EQ(fa_surface_update_into(f, t_packs(base + 4 * k), mem[b], stride, l.width, l.height, rev[b], &fr), FA_OK);
                rev[b] = fr.revision;
                if (k > 3) sum += fr.draw_ns;
            }
            typical[si] = (double)sum / 45 / 1e6;
            for (b = 0; b < 3; b++) free(mem[b]);
        }
        printf("  %-28s %7.2f ms %7.3f ms %7.2f ms %7.3f ms\n", levels[li].name, full[0], typical[0], full[1], typical[1]);
    }
    fa_surface_destroy(f);
    fa_stream_close(s);
    free(c.p);
}

static void race_updater(void* p)
{
    race_arg* a = (race_arg*)p;
    fa_cdg_frame fr;
    fa_store32(&a->started, 1);
    for (;;) {
        fa_result r = (a->updates & 1) ? fa_cdg_surface_update(a->surf, &fr)
                                       : fa_cdg_surface_update_at(a->surf, t_packs(a->updates % 4), &fr);
        a->updates++;
        a->last = r;
        if (r != FA_OK) break;
    }
}

static void test_close_while_updating(void)
{
    buf c = basic_cdg();
    int round;
    for (round = 0; round < 40; round++) {
        fa_stream* s = open_wav_stream(1, FA_OPEN_DECODE_ONLY);
        race_arg a;
        fa_thread t;
        int64_t deadline;
        memset(&a, 0, sizeof(a));
        CHECK_EQ(fa_stream_load_cdg_memory(s, c.p, c.n), FA_OK);
        CHECK_EQ(fa_cdg_surface_create(s, NULL, &a.surf), FA_OK);
        CHECK_EQ(fa_thread_create(&t, race_updater, &a), 0);
        deadline = fa_monotonic_ns() + 2000000000LL;
        while (!fa_load32(&a.started) && fa_monotonic_ns() < deadline) fa_sleep_ms(0);
        if (round & 1) fa_sleep_ms(1);
        fa_stream_close(s);
        fa_thread_join(&t);
        CHECK(a.updates > 0);
        CHECK_EQ(a.last, FA_ERROR_INVALID_ARG);   /* stopped by the close, nothing else */
        fa_cdg_surface_destroy(a.surf);
    }
    free(c.p);
}

int main(void)
{
    fa_engine_config cfg;
#if defined(__APPLE__)
    /* The iOS simulator starts programs elsewhere: tools/ios-sim/run.sh names the folder. */
    if (getenv("FA_TEST_CWD") && chdir(getenv("FA_TEST_CWD")) != 0) return 1;
#endif
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    cfg.worker_threads = 2;   /* render-ahead needs two (ra_request) */
    if (fa_engine_create(&cfg, &g_engine) != FA_OK) { fprintf(stderr, "engine\n"); return 1; }
    RUN(test_inflate_vectors);
    RUN(test_zip_pair);
    RUN(test_zip_damaged);
    RUN(test_zip_edge_cases);
    RUN(test_inflate_streaming);
    RUN(test_zip_lean);
    RUN(test_zip_over_limit);
    RUN(test_zip_prefix_probe);
    RUN(test_zip_cancel);
    RUN(test_zip_temp_song);
    RUN(test_zip_stored_crc);
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
    RUN(test_close_while_updating);
    RUN(test_update_into_matches);
    RUN(test_update_into_api);
    RUN(test_presentation_clamp);
    RUN(test_rect_sets);
    RUN(test_presentation_classic);
    RUN(test_presentation_padding);
    RUN(test_presentation_scaled);
    RUN(test_safety_zone_prepass);
    RUN(test_smoothness_and_edge_blend);
    RUN(test_contours);
    RUN(test_effects);
    RUN(test_bevel_half_stroke);
    RUN(test_effects_small_sizes);
    RUN(test_effects_recolor);
    RUN(test_colour_change_partial);
    RUN(test_presentation_incremental);
    RUN(test_render_ahead);
    RUN(test_presentation_costs);
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
