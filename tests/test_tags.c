/* Tests of the tag reader (tags.c, tags_read.c) and the file properties of
 * fa_stream_info. Tags are built byte by byte here for the details FFmpeg's
 * muxers never write (unsynchronisation, ID3v2.2, ID3v1, APEv2 lists, binary
 * covers, album-level Matroska tags, ZIPs), and read from the files of
 * tests/data/tags (make_tags_data.sh) as real muxers write them. */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "fa_internal.h"
#include "cdg.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FA_TEST_DATA_DIR
#  define FA_TEST_DATA_DIR "data"
#endif

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; return; } } while (0)
#define CHECK_EQ(a, b) do { long long va_ = (long long)(a), vb_ = (long long)(b); if (va_ != vb_) { \
    fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_failures++; return; } } while (0)
#define CHECK_NEAR(a, b, tol) do { double va_ = (a), vb_ = (b); if (fabs(va_ - vb_) > (tol)) { \
    fprintf(stderr, "  FAIL %s:%d: %s ~ %s (%.6f vs %.6f)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_failures++; return; } } while (0)
#define CHECK_STR(a, b) do { const char* va_ = (a); const char* vb_ = (b); if (strcmp(va_, vb_) != 0) { \
    fprintf(stderr, "  FAIL %s:%d: %s == %s (\"%s\" vs \"%s\")\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_failures++; return; } } while (0)
#define RUN(t) do { int before_ = g_failures; printf("%s\n", #t); t(); if (g_failures == before_) printf("  ok\n"); } while (0)

static fa_engine* g_engine;

/* ---- byte builder -------------------------------------------------------------- */

typedef struct buf {
    uint8_t* p;
    size_t n, cap;
} buf;

static void put(buf* b, const void* d, size_t n)
{
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 64;
        b->p = (uint8_t*)realloc(b->p, b->cap);
    }
    if (n) memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void put_str(buf* b, const char* s) { put(b, s, strlen(s)); }
static void put_strz(buf* b, const char* s) { put(b, s, strlen(s) + 1); }
static void put_u8(buf* b, unsigned v) { uint8_t x = (uint8_t)v; put(b, &x, 1); }
static void put_be16(buf* b, unsigned v) { put_u8(b, v >> 8); put_u8(b, v); }
static void put_be32(buf* b, uint32_t v) { put_u8(b, v >> 24); put_u8(b, v >> 16); put_u8(b, v >> 8); put_u8(b, v); }
static void put_le16(buf* b, unsigned v) { put_u8(b, v); put_u8(b, v >> 8); }
static void put_le32(buf* b, uint32_t v) { put_u8(b, v); put_u8(b, v >> 8); put_u8(b, v >> 16); put_u8(b, v >> 24); }
static void put_buf(buf* b, const buf* c) { put(b, c->p, c->n); }
static void set_be32(buf* b, size_t at, uint32_t v) { b->p[at] = (uint8_t)(v >> 24); b->p[at + 1] = (uint8_t)(v >> 16); b->p[at + 2] = (uint8_t)(v >> 8); b->p[at + 3] = (uint8_t)v; }
static void put_synchsafe(buf* b, uint32_t v) { put_u8(b, (v >> 21) & 127); put_u8(b, (v >> 14) & 127); put_u8(b, (v >> 7) & 127); put_u8(b, v & 127); }
static void buf_free(buf* b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

/* A tiny JPEG-looking image (only the magic matters). */
static const uint8_t g_jpeg[] = { 0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00, 0xFF, 0x00, 0xFF, 0xD9 };
static const uint8_t g_png[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R' };

static fa_tags* read_mem(const buf* b, uint32_t flags)
{
    fa_tags* t = NULL;
    if (fa_tags_read_memory(b->p, b->n, flags | FA_TAGS_NO_INFO, &t) != FA_OK) return NULL;
    return t;
}

static const char* item_value(const fa_tags* t, const char* key)
{
    uint32_t i;
    fa_tag_item it;
    for (i = 0; fa_tags_get_item(t, i, &it) == FA_OK; i++)
        if (strcmp(it.key, key) == 0) return it.value;
    return "(missing)";
}

/* ---- ID3v2 ------------------------------------------------------------------------ */

static void id3_frame23(buf* b, const char* id, const buf* body, uint8_t flags2)
{
    put_str(b, id);
    put_be32(b, (uint32_t)body->n);
    put_u8(b, 0);
    put_u8(b, flags2);
    put_buf(b, body);
}

static void id3_header(buf* b, int ver, int flags, uint32_t size)
{
    put_str(b, "ID3");
    put_u8(b, ver);
    put_u8(b, 0);
    put_u8(b, flags);
    put_synchsafe(b, size);
}

/* Text frame body: encoding byte + text. UTF-16 with a BOM for enc 1. */
static void text_body(buf* f, int enc, const char* s)
{
    put_u8(f, enc);
    if (enc == 1) {
        put_u8(f, 0xFF); put_u8(f, 0xFE);
        for (; *s; s++) put_le16(f, (uint8_t)*s);
    } else {
        put_str(f, s);
    }
}

static void test_id3v23_all_frames(void)
{
    buf frames = { 0 }, f = { 0 }, file = { 0 };
    fa_tags* t;
    fa_tag_picture pic;
    text_body(&f, 1, "Title16"); id3_frame23(&frames, "TIT2", &f, 0); buf_free(&f);
    text_body(&f, 0, "Caf\xe9"); id3_frame23(&frames, "TPE1", &f, 0); buf_free(&f);   /* Latin-1 */
    text_body(&f, 0, "(17)"); id3_frame23(&frames, "TCON", &f, 0); buf_free(&f);
    text_body(&f, 0, "5/9"); id3_frame23(&frames, "TRCK", &f, 0); buf_free(&f);
    text_body(&f, 0, "1999"); id3_frame23(&frames, "TYER", &f, 0); buf_free(&f);
    /* COMM: encoding, language, empty description, text. */
    put_u8(&f, 0); put_str(&f, "eng"); put_u8(&f, 0); put_str(&f, "A comment"); id3_frame23(&frames, "COMM", &f, 0); buf_free(&f);
    put_u8(&f, 0); put_str(&f, "eng"); put_strz(&f, "iTunNORM"); put_str(&f, "000 111"); id3_frame23(&frames, "COMM", &f, 0); buf_free(&f);
    put_u8(&f, 3); put_str(&f, "eng"); put_u8(&f, 0); put_str(&f, "La la la\nLa"); id3_frame23(&frames, "USLT", &f, 0); buf_free(&f);
    put_u8(&f, 0); put_strz(&f, "REPLAYGAIN_TRACK_GAIN"); put_str(&f, "-3.00 dB"); id3_frame23(&frames, "TXXX", &f, 0); buf_free(&f);
    put_strz(&f, "https://example.com"); id3_frame23(&frames, "WOAR", &f, 0); buf_free(&f);
    /* APIC: encoding, MIME, type, description, data. */
    put_u8(&f, 0); put_strz(&f, "image/jpeg"); put_u8(&f, 3); put_strz(&f, "front"); put(&f, g_jpeg, sizeof(g_jpeg));
    id3_frame23(&frames, "APIC", &f, 0); buf_free(&f);
    /* A compressed frame is skipped. */
    text_body(&f, 0, "nope"); id3_frame23(&frames, "TALB", &f, 0x80); buf_free(&f);
    id3_header(&file, 3, 0, (uint32_t)frames.n + 16);
    put_buf(&file, &frames);
    { uint8_t pad[16] = { 0 }; put(&file, pad, 16); }
    put_str(&file, "\xFF\xFB\x90\x00 not really audio");

    t = read_mem(&file, 0);
    CHECK(t != NULL);
    CHECK_STR(fa_tags_get(t, FA_TAG_TITLE), "Title16");
    CHECK_STR(fa_tags_get(t, FA_TAG_ARTIST), "Caf\xc3\xa9");
    CHECK_STR(fa_tags_get(t, FA_TAG_GENRE), "Rock");
    CHECK_STR(fa_tags_get(t, FA_TAG_TRACK), "5");
    CHECK_STR(fa_tags_get(t, FA_TAG_TRACK_TOTAL), "9");
    CHECK_STR(fa_tags_get(t, FA_TAG_DATE), "1999");
    CHECK_STR(fa_tags_get(t, FA_TAG_COMMENT), "A comment");
    CHECK_STR(fa_tags_get(t, FA_TAG_LYRICS), "La la la\nLa");
    CHECK_STR(fa_tags_get(t, FA_TAG_ALBUM), "");
    CHECK_STR(item_value(t, "TCON"), "(17)");                 /* items stay as stored */
    CHECK_STR(item_value(t, "COMM:iTunNORM"), "000 111");
    CHECK_STR(item_value(t, "REPLAYGAIN_TRACK_GAIN"), "-3.00 dB");
    CHECK_STR(item_value(t, "WOAR"), "https://example.com");
    CHECK_EQ(fa_tags_get_picture_count(t), 1);
    CHECK_EQ(fa_tags_get_cover(t, &pic), FA_OK);
    CHECK_EQ(pic.type, FA_PICTURE_FRONT_COVER);
    CHECK_STR(pic.mime, "image/jpeg");
    CHECK_STR(pic.description, "front");
    CHECK_EQ(pic.size, sizeof(g_jpeg));
    CHECK(memcmp(pic.data, g_jpeg, sizeof(g_jpeg)) == 0);
    CHECK_EQ(pic.format, FA_TAG_FORMAT_ID3V2);
    fa_tags_close(t);

    /* Without pictures. */
    t = read_mem(&file, FA_TAGS_NO_PICTURES);
    CHECK(t != NULL);
    CHECK_EQ(fa_tags_get_picture_count(t), 0);
    CHECK_EQ(fa_tags_get_cover(t, &pic), FA_ERROR_NOT_FOUND);
    CHECK_STR(fa_tags_get(t, FA_TAG_TITLE), "Title16");
    fa_tags_close(t);

    /* The loudness reader takes the same tag. */
    {
        fa_source src;
        fa_tag_loudness l;
        CHECK_EQ(fa_source_open_memory_ref(&src, file.p, file.n), FA_OK);
        fa_tags_read_loudness(&src, &l);
        CHECK_NEAR(l.track_lufs, -15.0, 1e-9);
        fa_source_close(&src);
    }
    buf_free(&frames);
    buf_free(&file);
}

/* Whole-tag unsynchronisation (ID3v2.3): every 0xFF is followed by an inserted 0x00. */
static void test_id3v23_unsync(void)
{
    buf frames = { 0 }, f = { 0 }, file = { 0 }, un = { 0 };
    fa_tags* t;
    fa_tag_picture pic;
    size_t i;
    text_body(&f, 0, "Unsync"); id3_frame23(&frames, "TIT2", &f, 0); buf_free(&f);
    put_u8(&f, 0); put_strz(&f, "image/jpeg"); put_u8(&f, 3); put_u8(&f, 0); put(&f, g_jpeg, sizeof(g_jpeg));
    id3_frame23(&frames, "APIC", &f, 0); buf_free(&f);
    for (i = 0; i < frames.n; i++) {
        put_u8(&un, frames.p[i]);
        if (frames.p[i] == 0xFF) put_u8(&un, 0);
    }
    id3_header(&file, 3, 0x80, (uint32_t)un.n);
    put_buf(&file, &un);
    t = read_mem(&file, 0);
    CHECK(t != NULL);
    CHECK_STR(fa_tags_get(t, FA_TAG_TITLE), "Unsync");
    CHECK_EQ(fa_tags_get_cover(t, &pic), FA_OK);
    CHECK_EQ(pic.size, sizeof(g_jpeg));
    CHECK(memcmp(pic.data, g_jpeg, sizeof(g_jpeg)) == 0);
    fa_tags_close(t);
    buf_free(&frames);
    buf_free(&un);
    buf_free(&file);
}

/* ID3v2.4: synchsafe frame sizes, UTF-8 lists, per-frame unsynchronisation with a
 * data length indicator, and the plain sizes some writers use instead. */
static void test_id3v24(void)
{
    buf file = { 0 }, frames = { 0 }, body = { 0 };
    fa_tags* t;
    /* TPE1 "A" NUL "B" in UTF-8: a list. */
    put_u8(&body, 3); put_str(&body, "A"); put_u8(&body, 0); put_str(&body, "B");
    put_str(&frames, "TPE1"); put_synchsafe(&frames, (uint32_t)body.n); put_be16(&frames, 0); put_buf(&frames, &body);
    buf_free(&body);
    /* TIT2 "\xC3\xBF" contains no 0xFF, but mark it unsynchronised with a data length
     * indicator anyway, and give a value with 0xFF 0x00 to undo: "x\xFF\x00y" -> "x\xFFy"
     * is not UTF-8, so it reads as Latin-1: "x" U+00FF "y". */
    put_synchsafe(&body, 4);
    put_u8(&body, 0); put_str(&body, "x"); put_u8(&body, 0xFF); put_u8(&body, 0x00); put_str(&body, "y");
    put_str(&frames, "TIT2"); put_synchsafe(&frames, (uint32_t)body.n); put_u8(&frames, 0); put_u8(&frames, 0x03); put_buf(&frames, &body);
    buf_free(&body);
    /* TALB of 200 bytes with a plain (non-synchsafe) size: 0x000000C8. */
    put_u8(&body, 3);
    while (body.n < 200) put_str(&body, "z");
    put_str(&frames, "TALB"); put_be32(&frames, (uint32_t)body.n); put_be16(&frames, 0); put_buf(&frames, &body);
    buf_free(&body);
    /* TPE2 in "UTF-8" holding a UTF-16 surrogate (ED A0 80): not well-formed, so
     * read as Latin-1, as other text that is not UTF-8. */
    put_u8(&body, 3); put_u8(&body, 0xED); put_u8(&body, 0xA0); put_u8(&body, 0x80);
    put_str(&frames, "TPE2"); put_synchsafe(&frames, (uint32_t)body.n); put_be16(&frames, 0); put_buf(&frames, &body);
    buf_free(&body);
    put_u8(&body, 3); put_str(&body, "Next");
    put_str(&frames, "TCOM"); put_synchsafe(&frames, (uint32_t)body.n); put_be16(&frames, 0); put_buf(&frames, &body);
    buf_free(&body);
    id3_header(&file, 4, 0, (uint32_t)frames.n);
    put_buf(&file, &frames);
    t = read_mem(&file, 0);
    CHECK(t != NULL);
    CHECK_STR(fa_tags_get(t, FA_TAG_ARTIST), "A; B");
    CHECK_STR(fa_tags_get(t, FA_TAG_TITLE), "x\xc3\xbfy");
    CHECK_EQ(strlen(fa_tags_get(t, FA_TAG_ALBUM)), 199);
    CHECK_STR(fa_tags_get(t, FA_TAG_COMPOSER), "Next");
    CHECK_STR(fa_tags_get(t, FA_TAG_ALBUM_ARTIST), "\xc3\xad\xc2\xa0\xc2\x80");
    fa_tags_close(t);
    buf_free(&frames);
    buf_free(&file);
}

static void test_id3v22(void)
{
    buf file = { 0 }, frames = { 0 }, body = { 0 };
    fa_tags* t;
    fa_tag_picture pic;
    put_u8(&body, 0); put_str(&body, "Old Title");
    put_str(&frames, "TT2"); put_u8(&frames, 0); put_be16(&frames, (unsigned)body.n); put_buf(&frames, &body);
    buf_free(&body);
    put_u8(&body, 0); put_str(&body, "PNG"); put_u8(&body, 4); put_u8(&body, 0); put(&body, g_png, sizeof(g_png));
    put_str(&frames, "PIC"); put_u8(&frames, 0); put_be16(&frames, (unsigned)body.n); put_buf(&frames, &body);
    buf_free(&body);
    id3_header(&file, 2, 0, (uint32_t)frames.n);
    put_buf(&file, &frames);
    t = read_mem(&file, 0);
    CHECK(t != NULL);
    CHECK_STR(fa_tags_get(t, FA_TAG_TITLE), "Old Title");
    CHECK_STR(item_value(t, "TIT2"), "Old Title");          /* v2.2 IDs read as v2.3 ones */
    CHECK_EQ(fa_tags_get_picture(t, 0, &pic), FA_OK);
    CHECK_EQ(pic.type, FA_PICTURE_BACK_COVER);
    CHECK_STR(pic.mime, "image/png");
    CHECK_EQ(fa_tags_get_cover(t, &pic), FA_OK);            /* no front cover: the first one */
    CHECK_EQ(pic.type, FA_PICTURE_BACK_COVER);
    fa_tags_close(t);
    buf_free(&frames);
    buf_free(&file);
}

/* ---- ID3v1 and APEv2 at the end, and their priority ----------------------------------- */

static void put_v1(buf* b, const char* title, int track, int genre)
{
    uint8_t t[128];
    memset(t, 0, sizeof(t));
    memcpy(t, "TAG", 3);
    memcpy(t + 3, title, strlen(title));
    memcpy(t + 33, "V1 Artist", 9);
    memcpy(t + 63, "V1 Album  ", 10);                   /* trailing spaces are trimmed */
    memcpy(t + 93, "1988", 4);
    memcpy(t + 97, "c", 1);
    t[126] = (uint8_t)track;
    t[127] = (uint8_t)genre;
    put(b, t, 128);
}

static void put_ape(buf* b, int with_cover)
{
    buf items = { 0 };
    uint32_t count = 0;
    /* Artist: a list of two values. */
    put_le32(&items, 3); put_le32(&items, 0); put_strz(&items, "Artist"); put(&items, "X\0Y", 3); count++;
    put_le32(&items, 4); put_le32(&items, 0); put_strz(&items, "Track"); put_str(&items, "7/10"); count++;
    if (with_cover) {
        /* Binary: file name, NUL, image. */
        put_le32(&items, (uint32_t)(6 + sizeof(g_jpeg))); put_le32(&items, 1u << 1); put_strz(&items, "Cover Art (Front)");
        put_strz(&items, "c.jpg"); put(&items, g_jpeg, sizeof(g_jpeg));
        count++;
    }
    put_buf(b, &items);
    put_str(b, "APETAGEX");
    put_le32(b, 2000);
    put_le32(b, (uint32_t)items.n + 32);
    put_le32(b, count);
    put_le32(b, 0);
    { uint8_t z[8] = { 0 }; put(b, z, 8); }
    buf_free(&items);
}

static void test_v1_ape_priority(void)
{
    buf file = { 0 }, frames = { 0 }, body = { 0 };
    fa_tags* t;
    fa_tag_picture pic;
    /* ID3v1 alone. */
    put_str(&file, "\xFF\xFB\x90\x00 audio bytes here");
    put_v1(&file, "V1 Title", 4, 9);
    t = read_mem(&file, 0);
    CHECK(t != NULL);
    CHECK_STR(fa_tags_get(t, FA_TAG_TITLE), "V1 Title");
    CHECK_STR(fa_tags_get(t, FA_TAG_ALBUM), "V1 Album");
    CHECK_STR(fa_tags_get(t, FA_TAG_DATE), "1988");
    CHECK_STR(fa_tags_get(t, FA_TAG_TRACK), "4");
    CHECK_STR(fa_tags_get(t, FA_TAG_GENRE), "Metal");
    CHECK_STR(fa_tags_get(t, FA_TAG_COMMENT), "c");
    fa_tags_close(t);
    buf_free(&file);

    /* ID3v2 + audio + APEv2 + ID3v1: v2 wins, then APE, then v1 for what is left. */
    put_u8(&body, 0); put_str(&body, "V2 Title");
    put_str(&frames, "TIT2"); put_be32(&frames, (uint32_t)body.n); put_be16(&frames, 0); put_buf(&frames, &body);
    id3_header(&file, 3, 0, (uint32_t)frames.n);
    put_buf(&file, &frames);
    put_str(&file, "\xFF\xFB\x90\x00 audio bytes here");
    put_ape(&file, 1);
    put_v1(&file, "V1 Title", 4, 9);
    t = read_mem(&file, 0);
    CHECK(t != NULL);
    CHECK_STR(fa_tags_get(t, FA_TAG_TITLE), "V2 Title");
    CHECK_STR(fa_tags_get(t, FA_TAG_ARTIST), "X; Y");        /* APE, not "V1 Artist" */
    CHECK_STR(fa_tags_get(t, FA_TAG_TRACK), "7");
    CHECK_STR(fa_tags_get(t, FA_TAG_TRACK_TOTAL), "10");
    CHECK_STR(fa_tags_get(t, FA_TAG_ALBUM), "V1 Album");      /* only ID3v1 has it */
    CHECK_EQ(fa_tags_get_cover(t, &pic), FA_OK);
    CHECK_EQ(pic.format, FA_TAG_FORMAT_APE);
    CHECK_STR(pic.mime, "image/jpeg");
    CHECK_EQ(pic.size, sizeof(g_jpeg));
    fa_tags_close(t);
    {
        /* The audio lies between the tags. */
        fa_source src;
        int64_t start, end;
        uint32_t bits;
        CHECK_EQ(fa_source_open_memory_ref(&src, file.p, file.n), FA_OK);
        fa_tags_audio_span(&src, &start, &end, &bits);
        CHECK_EQ(start, 10 + (int64_t)frames.n);
        CHECK_EQ(end - start, (int64_t)strlen("\xFF\xFB\x90\x00 audio bytes here"));
        CHECK_EQ(bits, 0);
        fa_source_close(&src);
    }
    buf_free(&body);
    buf_free(&frames);
    buf_free(&file);
}


/* ---- ID3v2 compressed frames, SYLT ------------------------------------------------------ */

/* zlib streams (RFC 1950) made with Python's zlib: "\0Compressed Title", "\3Zlib Artist \xC3\xA9"
 * and "\3" + "abcdefghij" x 50. */
static const uint8_t g_zlib_title[] = { 0x78, 0xDA, 0x63, 0x70, 0xCE, 0xCF, 0x2D, 0x28, 0x4A, 0x2D, 0x2E, 0x4E, 0x4D, 0x51, 0x08, 0xC9, 0x2C, 0xC9, 0x49, 0x05, 0x00, 0x35, 0x25, 0x06, 0x38 };
static const uint8_t g_zlib_artist[] = { 0x78, 0xDA, 0x63, 0x8E, 0xCA, 0xC9, 0x4C, 0x52, 0x70, 0x2C, 0x2A, 0xC9, 0x2C, 0x2E, 0x51, 0x38, 0xBC, 0x12, 0x00, 0x27, 0x1F, 0x05, 0xB8 };
static const uint8_t g_zlib_long[] = { 0x78, 0xDA, 0x63, 0x4E, 0x4C, 0x4A, 0x4E, 0x49, 0x4D, 0x4B, 0xCF, 0xC8, 0xCC, 0x1A, 0x65, 0x8D, 0x14, 0x16, 0x00, 0xFE, 0xB1, 0xC6, 0x42 };

static void test_id3_compressed(void)
{
    buf file = { 0 }, frames = { 0 }, body = { 0 };
    fa_tags* t;
    /* ID3v2.3: TIT2 with the compression flag (0x80): decompressed size, then the zlib stream. */
    put_be32(&body, 17); put(&body, g_zlib_title, sizeof(g_zlib_title));
    put_str(&frames, "TIT2"); put_be32(&frames, (uint32_t)body.n); put_be16(&frames, 0x0080); put_buf(&frames, &body);
    buf_free(&body);
    /* The same with a group ID byte too (0xA0), after the size. */
    put_be32(&body, 501); put_u8(&body, 7); put(&body, g_zlib_long, sizeof(g_zlib_long));
    put_str(&frames, "TCOM"); put_be32(&frames, (uint32_t)body.n); put_be16(&frames, 0x00A0); put_buf(&frames, &body);
    buf_free(&body);
    /* A wrong decompressed size, and an encrypted frame: both skipped. */
    put_be32(&body, 99); put(&body, g_zlib_title, sizeof(g_zlib_title));
    put_str(&frames, "TALB"); put_be32(&frames, (uint32_t)body.n); put_be16(&frames, 0x0080); put_buf(&frames, &body);
    buf_free(&body);
    put_be32(&body, 17); put_u8(&body, 1); put(&body, g_zlib_title, sizeof(g_zlib_title));
    put_str(&frames, "TCOP"); put_be32(&frames, (uint32_t)body.n); put_be16(&frames, 0x00C0); put_buf(&frames, &body);
    buf_free(&body);
    put_u8(&body, 0); put_str(&body, "Plain");
    put_str(&frames, "TPE1"); put_be32(&frames, (uint32_t)body.n); put_be16(&frames, 0); put_buf(&frames, &body);
    buf_free(&body);
    id3_header(&file, 3, 0, (uint32_t)frames.n);
    put_buf(&file, &frames);
    t = read_mem(&file, 0);
    CHECK(t != NULL);
    CHECK_STR(fa_tags_get(t, FA_TAG_TITLE), "Compressed Title");
    CHECK_EQ(strlen(fa_tags_get(t, FA_TAG_COMPOSER)), 500);
    CHECK_STR(fa_tags_get(t, FA_TAG_ALBUM), "");
    CHECK_STR(fa_tags_get(t, FA_TAG_COPYRIGHT), "");
    CHECK_STR(fa_tags_get(t, FA_TAG_ARTIST), "Plain");       /* the frames after the bad ones are read */
    fa_tags_close(t);
    buf_free(&frames);
    buf_free(&file);

    /* ID3v2.4: compression (0x08) needs the data length indicator (0x01, synchsafe), and
     * unsynchronisation (0x02) is undone before inflating. */
    put_synchsafe(&body, 15); put(&body, g_zlib_artist, sizeof(g_zlib_artist));
    put_str(&frames, "TPE1"); put_synchsafe(&frames, (uint32_t)body.n); put_be16(&frames, 0x0009); put_buf(&frames, &body);
    buf_free(&body);
    put(&body, g_zlib_artist, sizeof(g_zlib_artist));      /* no data length: skipped */
    put_str(&frames, "TALB"); put_synchsafe(&frames, (uint32_t)body.n); put_be16(&frames, 0x0008); put_buf(&frames, &body);
    buf_free(&body);
    put_synchsafe(&body, 4); put(&body, g_zlib_artist, sizeof(g_zlib_artist));    /* encrypted (0x04): skipped */
    put_str(&frames, "TCOM"); put_synchsafe(&frames, (uint32_t)body.n); put_be16(&frames, 0x000D); put_buf(&frames, &body);
    buf_free(&body);
    id3_header(&file, 4, 0, (uint32_t)frames.n);
    put_buf(&file, &frames);
    t = read_mem(&file, 0);
    CHECK(t != NULL);
    CHECK_STR(fa_tags_get(t, FA_TAG_ARTIST), "Zlib Artist \xc3\xa9");
    CHECK_STR(fa_tags_get(t, FA_TAG_ALBUM), "");
    CHECK_STR(fa_tags_get(t, FA_TAG_COMPOSER), "");
    fa_tags_close(t);
    buf_free(&frames);
    buf_free(&file);
}

static void put_sylt(buf* frames, int ver, int enc, int format, const char* desc, int count, const char* const* text, const uint32_t* time)
{
    buf body = { 0 };
    int i;
    put_u8(&body, enc); put_str(&body, "eng"); put_u8(&body, format); put_u8(&body, 1);
    if (enc == 1) { put_u8(&body, 0xFF); put_u8(&body, 0xFE); }
    put_strz(&body, desc);
    for (i = 0; i < count; i++) { put_strz(&body, text[i]); put_be32(&body, time[i]); }
    put_str(frames, "SYLT");
    if (ver == 4) put_synchsafe(frames, (uint32_t)body.n); else put_be32(frames, (uint32_t)body.n);
    put_be16(frames, 0);
    put_buf(frames, &body);
    buf_free(&body);
}

static void test_sylt(void)
{
    buf file = { 0 }, frames = { 0 };
    {
        fa_tags* t = NULL;
        char path[512];
        snprintf(path, sizeof(path), "%s/tags/sylt.mp3", FA_TEST_DATA_DIR);
        CHECK_EQ(fa_tags_read_path(path, FA_TAGS_NO_INFO, &t), FA_OK);
        /* mutagen's SYLT (ID3v2.4, UTF-8, milliseconds): a line per entry; a line break at the start of
         * an entry's text is an empty line. */
        CHECK_STR(item_value(t, "SYLT:Karaoke"), "[00:01.000]Hello\n[00:01.500] world\n\n[01:02.340]Second line");
        CHECK_STR(fa_tags_get(t, FA_TAG_TITLE), "Sung");
        fa_tags_close(t);
    }
    {
        /* Built here: no description, UTF-16, a trailing line break, and times in MPEG frames (not read). */
        static const char* const text[] = { "A", "B\n" };
        static const uint32_t time[] = { 90, 3723004 };
        fa_tags* t;
        put_sylt(&frames, 3, 0, 2, "", 2, text, time);
        put_sylt(&frames, 3, 0, 1, "frames", 2, text, time);
        id3_header(&file, 3, 0, (uint32_t)frames.n);
        put_buf(&file, &frames);
        t = read_mem(&file, 0);
        CHECK(t != NULL);
        CHECK_STR(item_value(t, "SYLT"), "[00:00.090]A\n[62:03.004]B");
        CHECK_STR(item_value(t, "SYLT:frames"), "(missing)");
        fa_tags_close(t);
    }
    buf_free(&frames);
    buf_free(&file);
}

/* ---- Lyrics3 ---------------------------------------------------------------------------- */

static void put_l3_field(buf* b, const char* id, const char* data)
{
    char size[8];
    snprintf(size, sizeof(size), "%05d", (int)strlen(data));
    put_str(b, id); put_str(b, size); put_str(b, data);
}

/* "LYRICSBEGIN", fields, a 6-digit size and "LYRICS200". `counts_begin`: the size includes "LYRICSBEGIN". */
static void put_lyrics3_v2(buf* b, int counts_begin)
{
    buf f = { 0 };
    char size[8];
    put_l3_field(&f, "IND", "10");
    put_l3_field(&f, "LYR", "Line one\r\nLine two");
    put_l3_field(&f, "ETT", "Lyric Title");
    put_l3_field(&f, "EAR", "Lyric Artist");
    put_l3_field(&f, "EAL", "Lyric Album");
    put_l3_field(&f, "AUT", "Writer");
    put_str(b, "LYRICSBEGIN");
    put_buf(b, &f);
    snprintf(size, sizeof(size), "%06d", (int)f.n + (counts_begin ? 11 : 0));
    put_str(b, size);
    put_str(b, "LYRICS200");
    buf_free(&f);
}

static void test_lyrics3(void)
{
    static const char audio[] = "\xFF\xFB\x90\x00 audio bytes here";
    buf file = { 0 };
    {
        fa_tags* t;
        fa_source src;
        int64_t start, end;
        uint32_t bits;
        /* v2.00 before an ID3v1 tag: v1 wins the fields it has, Lyrics3 fills the rest. */
        put_str(&file, audio);
        put_lyrics3_v2(&file, 1);
        put_v1(&file, "V1 Title", 4, 9);
        t = read_mem(&file, 0);
        CHECK(t != NULL);
        CHECK_STR(fa_tags_get(t, FA_TAG_TITLE), "V1 Title");
        CHECK_STR(fa_tags_get(t, FA_TAG_LYRICS), "Line one\r\nLine two");
        CHECK_STR(item_value(t, "LYR"), "Line one\r\nLine two");
        CHECK_STR(item_value(t, "ETT"), "Lyric Title");
        CHECK_STR(item_value(t, "AUT"), "Writer");
        CHECK_STR(item_value(t, "IND"), "10");
        fa_tags_close(t);
        CHECK_EQ(fa_source_open_memory_ref(&src, file.p, file.n), FA_OK);
        fa_tags_audio_span(&src, &start, &end, &bits);
        CHECK_EQ(start, 0);
        CHECK_EQ(end, (int64_t)strlen(audio));
        fa_source_close(&src);
        buf_free(&file);

        /* Without an ID3v1 tag, and with a size that leaves out "LYRICSBEGIN". */
        put_str(&file, audio);
        put_lyrics3_v2(&file, 0);
        t = read_mem(&file, 0);
        CHECK(t != NULL);
        CHECK_STR(fa_tags_get(t, FA_TAG_TITLE), "Lyric Title");
        CHECK_STR(fa_tags_get(t, FA_TAG_ARTIST), "Lyric Artist");
        CHECK_STR(fa_tags_get(t, FA_TAG_ALBUM), "Lyric Album");
        fa_tags_close(t);
        buf_free(&file);

        /* v1.00: the text between LYRICSBEGIN and LYRICSEND. */
        put_str(&file, audio);
        put_str(&file, "LYRICSBEGIN" "Just the words");
        put_str(&file, "LYRICSEND");
        put_v1(&file, "V1 Title", 4, 9);
        t = read_mem(&file, 0);
        CHECK(t != NULL);
        CHECK_STR(fa_tags_get(t, FA_TAG_LYRICS), "Just the words");
        fa_tags_close(t);
        CHECK_EQ(fa_source_open_memory_ref(&src, file.p, file.n), FA_OK);
        fa_tags_audio_span(&src, &start, &end, &bits);
        CHECK_EQ(end, (int64_t)strlen(audio));
        fa_source_close(&src);
        buf_free(&file);

        /* APEv2 after the Lyrics3 tag, and before it. */
        put_str(&file, audio);
        put_lyrics3_v2(&file, 1);
        put_ape(&file, 0);
        put_v1(&file, "V1 Title", 4, 9);
        t = read_mem(&file, 0);
        CHECK(t != NULL);
        CHECK_STR(fa_tags_get(t, FA_TAG_ARTIST), "X; Y");
        CHECK_STR(fa_tags_get(t, FA_TAG_LYRICS), "Line one\r\nLine two");
        fa_tags_close(t);
        CHECK_EQ(fa_source_open_memory_ref(&src, file.p, file.n), FA_OK);
        fa_tags_audio_span(&src, &start, &end, &bits);
        CHECK_EQ(end, (int64_t)strlen(audio));
        fa_source_close(&src);
        buf_free(&file);
        put_str(&file, audio);
        put_ape(&file, 0);
        put_lyrics3_v2(&file, 1);
        put_v1(&file, "V1 Title", 4, 9);
        t = read_mem(&file, 0);
        CHECK(t != NULL);
        CHECK_STR(fa_tags_get(t, FA_TAG_ARTIST), "X; Y");
        CHECK_STR(fa_tags_get(t, FA_TAG_LYRICS), "Line one\r\nLine two");
        fa_tags_close(t);
        CHECK_EQ(fa_source_open_memory_ref(&src, file.p, file.n), FA_OK);
        fa_tags_audio_span(&src, &start, &end, &bits);
        CHECK_EQ(end, (int64_t)strlen(audio));
        fa_source_close(&src);
        buf_free(&file);

        /* A damaged tag (a size that leads nowhere) gives nothing. */
        put_str(&file, audio);
        put_str(&file, "LYRICSBEGIN" "IND0000210" "999999" "LYRICS200");
        t = read_mem(&file, 0);
        CHECK(t != NULL);
        CHECK_STR(fa_tags_get(t, FA_TAG_LYRICS), "");
        fa_tags_close(t);
        buf_free(&file);
    }
}

/* ---- FLAC: Vorbis comments and pictures -------------------------------------------- */

static void vc_item(buf* b, const char* s) { put_le32(b, (uint32_t)strlen(s)); put_str(b, s); }

static void put_flac_picture(buf* b, uint32_t type, const char* mime, const uint8_t* d, size_t n)
{
    put_be32(b, type);
    put_be32(b, (uint32_t)strlen(mime)); put_str(b, mime);
    put_be32(b, 4); put_str(b, "desc");
    put_be32(b, 1); put_be32(b, 1); put_be32(b, 24); put_be32(b, 0);
    put_be32(b, (uint32_t)n); put(b, d, n);
}

static void b64(buf* out, const uint8_t* d, size_t n)
{
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i;
    for (i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        put_u8(out, tbl[(v >> 18) & 63]);
        put_u8(out, tbl[(v >> 12) & 63]);
        put_u8(out, i + 1 < n ? tbl[(v >> 6) & 63] : '=');
        put_u8(out, i + 2 < n ? tbl[v & 63] : '=');
    }
}

static void test_flac_blocks(void)
{
    buf file = { 0 }, vc = { 0 }, pic = { 0 }, item = { 0 };
    fa_tags* t;
    fa_tag_picture p;
    uint8_t si[34];
    memset(si, 0, sizeof(si));
    si[12] = 0x01; si[13] = 0x70;              /* bits per sample 24: (1 << 4 | 7) + 1 */
    put_str(&file, "fLaC");
    put_u8(&file, 0); put_u8(&file, 0); put_u8(&file, 0); put_u8(&file, 34); put(&file, si, 34);
    /* Vorbis comments: two ARTIST values, TRACKNUMBER + TRACKTOTAL, and a picture as base64. */
    put_le32(&vc, 6); put_str(&vc, "vendor");
    put_flac_picture(&pic, 3, "image/png", g_png, sizeof(g_png));
    put_str(&item, "METADATA_BLOCK_PICTURE=");
    b64(&item, pic.p, pic.n);
    put_le32(&vc, 7);
    vc_item(&vc, "REPLAYGAIN_TRACK_GAIN=+2.00 dB");
    vc_item(&vc, "ARTIST=One");
    vc_item(&vc, "artist=Two");
    vc_item(&vc, "TRACKNUMBER=2");
    vc_item(&vc, "TRACKTOTAL=11");
    vc_item(&vc, "GENRE=Jazz");
    put_le32(&vc, (uint32_t)item.n); put_buf(&vc, &item);
    put_u8(&file, 4); put_u8(&file, (unsigned)(vc.n >> 16)); put_be16(&file, (unsigned)vc.n & 0xFFFF); put_buf(&file, &vc);
    buf_free(&pic);
    /* A PICTURE block too (the last block). */
    put_flac_picture(&pic, 0, "", g_jpeg, sizeof(g_jpeg));
    put_u8(&file, 0x86); put_u8(&file, 0); put_be16(&file, (unsigned)pic.n); put_buf(&file, &pic);
    put_str(&file, "\xFF\xF8 frames");
    t = read_mem(&file, 0);
    CHECK(t != NULL);
    CHECK_STR(fa_tags_get(t, FA_TAG_ARTIST), "One; Two");
    CHECK_STR(fa_tags_get(t, FA_TAG_TRACK), "2");
    CHECK_STR(fa_tags_get(t, FA_TAG_TRACK_TOTAL), "11");
    CHECK_STR(fa_tags_get(t, FA_TAG_GENRE), "Jazz");
    CHECK_EQ(fa_tags_get_picture_count(t), 2);
    CHECK_EQ(fa_tags_get_picture(t, 0, &p), FA_OK);
    CHECK_EQ(p.type, FA_PICTURE_FRONT_COVER);
    CHECK_STR(p.mime, "image/png");
    CHECK_STR(p.description, "desc");
    CHECK_EQ(p.size, sizeof(g_png));
    CHECK(memcmp(p.data, g_png, sizeof(g_png)) == 0);
    CHECK_EQ(fa_tags_get_picture(t, 1, &p), FA_OK);
    CHECK_STR(p.mime, "image/jpeg");                        /* not stated: from the data */
    fa_tags_close(t);
    {
        fa_source src;
        int64_t start, end;
        uint32_t bits;
        CHECK_EQ(fa_source_open_memory_ref(&src, file.p, file.n), FA_OK);
        fa_tags_audio_span(&src, &start, &end, &bits);
        CHECK_EQ(end - start, 9);                 /* "\xFF\xF8 frames" */
        CHECK_EQ(bits, 24);
        fa_source_close(&src);
    }
    {
        /* An ID3v2 tag in front: FLAC's own comments still win, for the fields and
         * for loudness. */
        buf tagged = { 0 }, frames = { 0 }, f = { 0 };
        fa_source src;
        fa_tag_loudness l;
        put_u8(&f, 0); put_strz(&f, "REPLAYGAIN_TRACK_GAIN"); put_str(&f, "-6.00 dB"); id3_frame23(&frames, "TXXX", &f, 0); buf_free(&f);
        text_body(&f, 0, "Id3 Artist"); id3_frame23(&frames, "TPE1", &f, 0); buf_free(&f);
        text_body(&f, 0, "Id3 Title"); id3_frame23(&frames, "TIT2", &f, 0); buf_free(&f);
        id3_header(&tagged, 3, 0, (uint32_t)frames.n);
        put_buf(&tagged, &frames);
        put_buf(&tagged, &file);
        t = read_mem(&tagged, FA_TAGS_NO_PICTURES);
        CHECK(t != NULL);
        CHECK_STR(fa_tags_get(t, FA_TAG_ARTIST), "One; Two");
        CHECK_STR(fa_tags_get(t, FA_TAG_TITLE), "Id3 Title");      /* not in the comments */
        fa_tags_close(t);
        CHECK_EQ(fa_source_open_memory_ref(&src, tagged.p, tagged.n), FA_OK);
        fa_tags_read_loudness(&src, &l);
        CHECK_NEAR(l.track_lufs, -20.0, 1e-9);
        fa_source_close(&src);
        buf_free(&tagged);
        buf_free(&frames);
    }
    buf_free(&file);
    buf_free(&vc);
    buf_free(&pic);
    buf_free(&item);
}

/* ---- MP4 ---------------------------------------------------------------------------- */

static void mp4_box(buf* out, const char* type, const buf* body)
{
    put_be32(out, (uint32_t)(8 + body->n));
    put_str(out, type);
    put_buf(out, body);
}

static void mp4_data(buf* item, uint32_t type, const void* d, size_t n)
{
    put_be32(item, (uint32_t)(16 + n));
    put_str(item, "data");
    put_be32(item, type);
    put_be32(item, 0);
    put(item, d, n);
}

static void test_mp4_ilst(void)
{
    buf ilst = { 0 }, it = { 0 }, meta = { 0 }, udta = { 0 }, moov = { 0 }, file = { 0 }, b = { 0 };
    fa_tags* t;
    fa_tag_picture p;
    static const uint8_t trkn[8] = { 0, 0, 0, 4, 0, 15, 0, 0 };
    static const uint8_t gnre[2] = { 0, 18 };       /* ID3 17 + 1: Rock */
    static const uint8_t tmpo[2] = { 0, 120 };
    mp4_data(&it, 1, "Name", 4);
    { buf x = { 0 }; mp4_box(&x, "\xA9nam", &it); put_buf(&ilst, &x); buf_free(&x); } buf_free(&it);
    mp4_data(&it, 0, trkn, 8); { buf x = { 0 }; mp4_box(&x, "trkn", &it); put_buf(&ilst, &x); buf_free(&x); } buf_free(&it);
    mp4_data(&it, 0, gnre, 2); { buf x = { 0 }; mp4_box(&x, "gnre", &it); put_buf(&ilst, &x); buf_free(&x); } buf_free(&it);
    mp4_data(&it, 21, tmpo, 2); { buf x = { 0 }; mp4_box(&x, "tmpo", &it); put_buf(&ilst, &x); buf_free(&x); } buf_free(&it);
    {
        /* Signed integers, 1 and 8 bytes wide: both -2. */
        static const uint8_t m2a[1] = { 0xFE }, m2b[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE };
        mp4_data(&it, 21, m2a, 1); { buf x = { 0 }; mp4_box(&x, "rtng", &it); put_buf(&ilst, &x); buf_free(&x); } buf_free(&it);
        mp4_data(&it, 21, m2b, 8); { buf x = { 0 }; mp4_box(&x, "plID", &it); put_buf(&ilst, &x); buf_free(&x); } buf_free(&it);
    }
    mp4_data(&it, 13, g_jpeg, sizeof(g_jpeg)); { buf x = { 0 }; mp4_box(&x, "covr", &it); put_buf(&ilst, &x); buf_free(&x); } buf_free(&it);
    /* Freeform: ----, mean, name, data. */
    put_be32(&it, 8 + 4 + 16); put_str(&it, "mean"); put_be32(&it, 0); put_str(&it, "com.apple.iTunes");
    put_be32(&it, 8 + 4 + 21); put_str(&it, "name"); put_be32(&it, 0); put_str(&it, "replaygain_track_gain");
    mp4_data(&it, 1, "+2.00 dB", 8);
    { buf x = { 0 }; mp4_box(&x, "----", &it); put_buf(&ilst, &x); buf_free(&x); } buf_free(&it);
    /* meta (full box) = version/flags + hdlr + ilst. */
    put_be32(&meta, 0);
    put_be32(&b, 0); put_be32(&b, 0); put_str(&b, "mdirappl"); put_be32(&b, 0); put_be32(&b, 0); put_u8(&b, 0);
    { buf x = { 0 }; mp4_box(&x, "hdlr", &b); put_buf(&meta, &x); buf_free(&x); } buf_free(&b);
    { buf x = { 0 }; mp4_box(&x, "ilst", &ilst); put_buf(&meta, &x); buf_free(&x); }
    mp4_box(&udta, "meta", &meta);
    { buf x = { 0 }; mp4_box(&x, "udta", &udta); put_buf(&moov, &x); buf_free(&x); }
    put_be32(&b, 0); put_str(&b, "M4A "); put_be32(&b, 0);
    mp4_box(&file, "ftyp", &b);
    buf_free(&b);
    mp4_box(&file, "moov", &moov);
    t = read_mem(&file, 0);
    CHECK(t != NULL);
    CHECK_STR(fa_tags_get(t, FA_TAG_TITLE), "Name");
    CHECK_STR(item_value(t, "\xC2\xA9nam"), "Name");
    CHECK_STR(fa_tags_get(t, FA_TAG_TRACK), "4");
    CHECK_STR(fa_tags_get(t, FA_TAG_TRACK_TOTAL), "15");
    CHECK_STR(fa_tags_get(t, FA_TAG_GENRE), "Rock");
    CHECK_STR(fa_tags_get(t, FA_TAG_BPM), "120");
    CHECK_STR(item_value(t, "rtng"), "-2");
    CHECK_STR(item_value(t, "plID"), "-2");
    CHECK_STR(item_value(t, "replaygain_track_gain"), "+2.00 dB");
    CHECK_EQ(fa_tags_get_cover(t, &p), FA_OK);
    CHECK_EQ(p.type, FA_PICTURE_FRONT_COVER);
    CHECK_STR(p.mime, "image/jpeg");
    fa_tags_close(t);
    {
        /* ReplayGain in MP4 freeform atoms feeds normalization too. */
        fa_source src;
        fa_tag_loudness l;
        CHECK_EQ(fa_source_open_memory_ref(&src, file.p, file.n), FA_OK);
        fa_tags_read_loudness(&src, &l);
        CHECK_NEAR(l.track_lufs, -20.0, 1e-9);
        fa_source_close(&src);
    }
    buf_free(&ilst);
    buf_free(&meta);
    buf_free(&udta);
    buf_free(&moov);
    buf_free(&file);
}

/* ---- WAV and AIFF ------------------------------------------------------------------- */

static void riff_chunk(buf* out, const char* id, const void* d, size_t n)
{
    put_str(out, id);
    put_le32(out, (uint32_t)n);
    put(out, d, n);
    if (n & 1) put_u8(out, 0);
}

static buf make_wav(int with_id3, int with_info)
{
    buf file = { 0 }, list = { 0 }, fmt = { 0 }, id3 = { 0 }, body = { 0 };
    int16_t samples[200];
    memset(samples, 0, sizeof(samples));
    put_le16(&fmt, 1); put_le16(&fmt, 1); put_le32(&fmt, 8000); put_le32(&fmt, 16000); put_le16(&fmt, 2); put_le16(&fmt, 16);
    put_str(&file, "RIFF"); put_le32(&file, 0); put_str(&file, "WAVE");
    riff_chunk(&file, "fmt ", fmt.p, fmt.n);
    riff_chunk(&file, "data", samples, sizeof(samples));
    if (with_info) {
        put_str(&list, "INFO");
        riff_chunk(&list, "INAM", "Info Title\0", 11);
        riff_chunk(&list, "IART", "Info Artist", 11);
        riff_chunk(&list, "ICRD", "2001", 4);
        riff_chunk(&file, "LIST", list.p, list.n);
    }
    if (with_id3) {
        put_u8(&body, 0); put_str(&body, "ID3 Title");
        put_str(&id3, "ID3\x03"); put_u8(&id3, 0); put_u8(&id3, 0); put_synchsafe(&id3, (uint32_t)body.n + 10);
        put_str(&id3, "TIT2"); put_be32(&id3, (uint32_t)body.n); put_be16(&id3, 0); put_buf(&id3, &body);
        riff_chunk(&file, "id3 ", id3.p, id3.n);
    }
    file.p[4] = (uint8_t)(file.n - 8); file.p[5] = (uint8_t)((file.n - 8) >> 8);
    buf_free(&list);
    buf_free(&fmt);
    buf_free(&id3);
    buf_free(&body);
    return file;
}

static void test_wav_aiff(void)
{
    buf wav = make_wav(1, 1), aiff = { 0 }, comm = { 0 };
    fa_tags* t = NULL;
    fa_stream_info info;
    /* ID3 chunk wins over INFO; INFO fills the rest. Properties: exact. */
    CHECK_EQ(fa_tags_read_memory(wav.p, wav.n, 0, &t), FA_OK);
    CHECK_STR(fa_tags_get(t, FA_TAG_TITLE), "ID3 Title");
    CHECK_STR(fa_tags_get(t, FA_TAG_ARTIST), "Info Artist");
    CHECK_STR(fa_tags_get(t, FA_TAG_DATE), "2001");
    CHECK_STR(item_value(t, "INAM"), "Info Title");
    CHECK_EQ(fa_tags_get_info(t, &info), FA_OK);
    CHECK_STR(info.codec, "pcm");
    CHECK_EQ(info.format.sample_rate, 8000);
    CHECK_EQ(info.format.channels, 1);
    CHECK_EQ(info.length_frames, 200);
    CHECK_EQ(info.audio_bytes, 400);
    CHECK_EQ(info.bits_per_sample, 16);
    CHECK_EQ(info.bitrate, 128000);
    CHECK_EQ(info.file_size, (int64_t)wav.n);
    CHECK_EQ(info.has_cdg, 0);
    fa_tags_close(t);

    /* AIFF: NAME / AUTH / ANNO. */
    put_be16(&comm, 1); put_be32(&comm, 100); put_be16(&comm, 16);
    put_be16(&comm, 0x400B); put_be32(&comm, 0xFA000000u); put_be32(&comm, 0);   /* 8000 as 80-bit float */
    put_str(&aiff, "FORM"); put_be32(&aiff, 0); put_str(&aiff, "AIFF");
    put_str(&aiff, "COMM"); put_be32(&aiff, (uint32_t)comm.n); put_buf(&aiff, &comm);
    put_str(&aiff, "NAME"); put_be32(&aiff, 5); put_str(&aiff, "Aname"); put_u8(&aiff, 0);
    put_str(&aiff, "AUTH"); put_be32(&aiff, 4); put_str(&aiff, "Auth");
    put_str(&aiff, "ANNO"); put_be32(&aiff, 3); put_str(&aiff, "hey"); put_u8(&aiff, 0);
    put_str(&aiff, "SSND"); put_be32(&aiff, 8 + 200); put_be32(&aiff, 0); put_be32(&aiff, 0);
    { uint8_t z[200] = { 0 }; put(&aiff, z, 200); }
    set_be32(&aiff, 4, (uint32_t)aiff.n - 8);
    t = read_mem(&aiff, 0);
    CHECK(t != NULL);
    CHECK_STR(fa_tags_get(t, FA_TAG_TITLE), "Aname");
    CHECK_STR(fa_tags_get(t, FA_TAG_ARTIST), "Auth");
    CHECK_STR(fa_tags_get(t, FA_TAG_COMMENT), "hey");
    fa_tags_close(t);
    buf_free(&wav);
    buf_free(&aiff);
    buf_free(&comm);
}

/* ---- Matroska ------------------------------------------------------------------------ */

static void ebml_id(buf* b, uint32_t id)
{
    if (id >= 0x1000000) put_u8(b, id >> 24);
    if (id >= 0x10000) put_u8(b, id >> 16);
    if (id >= 0x100) put_u8(b, id >> 8);
    put_u8(b, id);
}

static void ebml_el(buf* out, uint32_t id, const void* d, size_t n)
{
    ebml_id(out, id);
    put_u8(out, 0x01);                              /* an 8-byte size */
    put_u8(out, 0); put_u8(out, 0); put_u8(out, 0);
    put_be32(out, (uint32_t)n);
    put(out, d, n);
}

static void ebml_buf(buf* out, uint32_t id, const buf* b) { ebml_el(out, id, b->p, b->n); }

static void simple_tag(buf* out, const char* name, const char* value)
{
    buf st = { 0 };
    ebml_el(&st, 0x45A3, name, strlen(name));
    ebml_el(&st, 0x4487, value, strlen(value));
    ebml_buf(out, 0x67C8, &st);
    buf_free(&st);
}

static void tag(buf* tags, int ttv, uint32_t track_uid, const char* name, const char* value)
{
    buf tg = { 0 }, targets = { 0 };
    uint8_t v = (uint8_t)ttv;
    ebml_el(&targets, 0x68CA, &v, 1);
    if (track_uid) { uint8_t u = (uint8_t)track_uid; ebml_el(&targets, 0x63C5, &u, 1); }
    ebml_buf(&tg, 0x63C0, &targets);
    simple_tag(&tg, name, value);
    ebml_buf(tags, 0x7373, &tg);
    buf_free(&tg);
    buf_free(&targets);
}

static void test_matroska_tags(void)
{
    buf file = { 0 }, seg = { 0 }, tags = { 0 }, att = { 0 }, af = { 0 }, info = { 0 }, hdr = { 0 };
    fa_tags* t;
    fa_tag_picture p;
    ebml_el(&hdr, 0x4282, "matroska", 8);
    ebml_buf(&file, 0x1A45DFA3, &hdr);
    ebml_el(&info, 0x7BA9, "Segment Title", 13);
    ebml_buf(&seg, 0x1549A966, &info);
    tag(&tags, 30, 0, "TITLE", "Track Title");
    tag(&tags, 50, 0, "TITLE", "The Album");
    tag(&tags, 50, 0, "ARTIST", "Album Artist");
    tag(&tags, 30, 1, "BPS", "123456");                  /* one track's statistics: ignored */
    ebml_buf(&seg, 0x1254C367, &tags);
    ebml_el(&af, 0x466E, "cover.jpg", 9);
    ebml_el(&af, 0x4660, "image/jpeg", 10);
    ebml_el(&af, 0x465C, g_jpeg, sizeof(g_jpeg));
    ebml_buf(&att, 0x61A7, &af);
    buf_free(&af);
    ebml_el(&af, 0x466E, "font.ttf", 8);
    ebml_el(&af, 0x4660, "font/ttf", 8);
    ebml_el(&af, 0x465C, "FONTDATA", 8);
    ebml_buf(&att, 0x61A7, &af);
    ebml_buf(&seg, 0x1941A469, &att);
    ebml_buf(&file, 0x18538067, &seg);
    t = read_mem(&file, 0);
    CHECK(t != NULL);
    CHECK_STR(fa_tags_get(t, FA_TAG_TITLE), "Track Title");    /* tags win over the segment title */
    CHECK_STR(fa_tags_get(t, FA_TAG_ALBUM), "The Album");
    CHECK_STR(fa_tags_get(t, FA_TAG_ALBUM_ARTIST), "Album Artist");
    CHECK_STR(fa_tags_get(t, FA_TAG_ARTIST), "");
    CHECK_STR(item_value(t, "BPS"), "(missing)");
    CHECK_STR(item_value(t, "INFO/TITLE"), "Segment Title");
    CHECK_EQ(fa_tags_get_picture_count(t), 1);                  /* the font is not a picture */
    CHECK_EQ(fa_tags_get_cover(t, &p), FA_OK);
    CHECK_EQ(p.type, FA_PICTURE_FRONT_COVER);
    CHECK_STR(p.description, "cover.jpg");
    fa_tags_close(t);
    buf_free(&file);
    buf_free(&seg);
    buf_free(&tags);
    buf_free(&att);
    buf_free(&af);
    buf_free(&info);
    buf_free(&hdr);
}


static void test_matroska_targeted_tags(void)
{
    buf file = { 0 }, seg = { 0 }, tags = { 0 }, hdr = { 0 };
    fa_tags* t;
    ebml_el(&hdr, 0x4282, "matroska", 8);
    ebml_buf(&file, 0x1A45DFA3, &hdr);
    tag(&tags, 30, 0, "TITLE", "Global");
    tag(&tags, 30, 77, "BPS", "123456");
    tag(&tags, 30, 77, "TITLE", "Track Title");             /* aimed at a track: never the common field */
    {
        /* A chapter's tag, and a tag aimed at both a track and an edition (named after the track). */
        buf st = { 0 }, tg = { 0 }, targets = { 0 }, one = { 0 };
        ebml_el(&st, 0x45A3, "TITLE", 5);
        ebml_el(&st, 0x4487, "Verse", 5);
        ebml_buf(&one, 0x67C8, &st);
        ebml_el(&targets, 0x68CA, "\x1E", 1);
        ebml_el(&targets, 0x63C4, "\x05", 1);
        ebml_buf(&tg, 0x63C0, &targets);
        put_buf(&tg, &one);
        ebml_buf(&tags, 0x7373, &tg);
        buf_free(&st); buf_free(&tg); buf_free(&targets); buf_free(&one);
        ebml_el(&st, 0x45A3, "ARTIST", 6);
        ebml_el(&st, 0x4487, "Singer", 6);
        ebml_buf(&one, 0x67C8, &st);
        ebml_el(&targets, 0x63C9, "\x09", 1);
        ebml_el(&targets, 0x63C5, "\x2A", 1);
        ebml_buf(&tg, 0x63C0, &targets);
        put_buf(&tg, &one);
        ebml_buf(&tags, 0x7373, &tg);
        buf_free(&st); buf_free(&tg); buf_free(&targets); buf_free(&one);
    }
    ebml_buf(&seg, 0x1254C367, &tags);
    ebml_buf(&file, 0x18538067, &seg);
    t = read_mem(&file, 0);
    CHECK(t != NULL);
    CHECK_STR(fa_tags_get(t, FA_TAG_TITLE), "Global");
    CHECK_STR(fa_tags_get(t, FA_TAG_ARTIST), "");
    CHECK_STR(item_value(t, "TRACK:77/BPS"), "123456");
    CHECK_STR(item_value(t, "TRACK:77/TITLE"), "Track Title");
    CHECK_STR(item_value(t, "CHAPTER:5/TITLE"), "Verse");
    CHECK_STR(item_value(t, "TRACK:42/ARTIST"), "Singer");
    fa_tags_close(t);
    buf_free(&file);
    buf_free(&seg);
    buf_free(&tags);
    buf_free(&hdr);
}

static void test_flac_cuesheet_and_vendor(void)
{
    fa_tags* t = NULL;
    char path[512];
    snprintf(path, sizeof(path), "%s/tags/cue.flac", FA_TEST_DATA_DIR);
    CHECK_EQ(fa_tags_read_path(path, FA_TAGS_NO_INFO, &t), FA_OK);
    /* mutagen's cue sheet: two tracks and the lead-out (170, left out). */
    CHECK_STR(item_value(t, "CUESHEET/CATALOG"), "0123456789012");
    CHECK_STR(item_value(t, "CUESHEET/TRACK01"), "00:00.000");
    CHECK_STR(item_value(t, "CUESHEET/TRACK01/ISRC"), "USRC17607839");
    CHECK_STR(item_value(t, "CUESHEET/TRACK02"), "00:00.510");           /* at index point 1: (22050 + 441) / 44100 s */
    CHECK_STR(item_value(t, "CUESHEET/TRACK02/ISRC"), "(missing)");
    CHECK_STR(item_value(t, "CUESHEET/TRACK170"), "(missing)");
    CHECK_STR(item_value(t, "VENDOR"), "ffmpeg");                          /* the vendor string (-fflags +bitexact) */
    fa_tags_close(t);

    snprintf(path, sizeof(path), "%s/tags/tags.opus", FA_TEST_DATA_DIR);
    CHECK_EQ(fa_tags_read_path(path, FA_TAGS_NO_INFO, &t), FA_OK);
    CHECK_STR(item_value(t, "VENDOR"), "ffmpeg");                          /* OpusTags carries one too */
    fa_tags_close(t);
}

/* A Standard MIDI File of one track: a track name, a copyright and one note. */
static void put_smf(buf* b)
{
    buf trk = { 0 };
    put_u8(&trk, 0); put_u8(&trk, 0xFF); put_u8(&trk, 0x03); put_u8(&trk, 8); put_str(&trk, "Song One");
    put_u8(&trk, 0); put_u8(&trk, 0xFF); put_u8(&trk, 0x02); put_u8(&trk, 9); put_str(&trk, "(c) Mine!");
    put_u8(&trk, 0); put_u8(&trk, 0x90); put_u8(&trk, 60); put_u8(&trk, 100);
    put_u8(&trk, 0x83); put_u8(&trk, 0x60); put_u8(&trk, 0x80); put_u8(&trk, 60); put_u8(&trk, 0);   /* 480 ticks: 0.5 s at 120 bpm / 480 */
    put_u8(&trk, 0); put_u8(&trk, 0xFF); put_u8(&trk, 0x2F); put_u8(&trk, 0);
    put_str(b, "MThd"); put_be32(b, 6); put_be16(b, 0); put_be16(b, 1); put_be16(b, 480);
    put_str(b, "MTrk"); put_be32(b, (uint32_t)trk.n); put_buf(b, &trk);
    buf_free(&trk);
}

static void test_midi_without_soundfont(void)
{
    buf file = { 0 };
    fa_tags* t;
    fa_stream_info info;
    put_smf(&file);
    /* No decoder can open it (no soundfont), but the title, artist and length can be read. */
    t = NULL;
    CHECK_EQ(fa_tags_read_memory(file.p, file.n, 0, &t), FA_OK);
    CHECK_STR(fa_tags_get(t, FA_TAG_TITLE), "Song One");
    CHECK_STR(fa_tags_get(t, FA_TAG_COPYRIGHT), "(c) Mine!");
    CHECK_EQ(fa_tags_get_info(t, &info), FA_OK);
    CHECK_STR(info.codec, "midi");
    CHECK_STR(info.container, "smf");
    CHECK_EQ(info.format.sample_rate, 44100);
    CHECK_EQ(info.format.channels, 2);
    CHECK(info.duration > 400000000LL && info.duration < 4000000000LL);
    CHECK_EQ(info.file_size, (int64_t)file.n);
    fa_tags_close(t);
    /* With NO_INFO nothing is parsed. */
    t = read_mem(&file, 0);
    CHECK(t != NULL);
    CHECK_EQ(fa_tags_get_info(t, &info), FA_ERROR_NOT_FOUND);
    fa_tags_close(t);
    buf_free(&file);
}

/* ---- ZIP (MP3+G) -------------------------------------------------------------------- */

static void zip_add(buf* z, buf* cd, const char* name, const buf* data, uint32_t* count)
{
    uint32_t crc = fa_crc32(data->p, data->n), off = (uint32_t)z->n;
    put_le32(z, 0x04034b50u); put_le16(z, 10); put_le16(z, 0); put_le16(z, 0); put_le16(z, 0); put_le16(z, 0);
    put_le32(z, crc); put_le32(z, (uint32_t)data->n); put_le32(z, (uint32_t)data->n);
    put_le16(z, (unsigned)strlen(name)); put_le16(z, 0); put_str(z, name); put_buf(z, data);
    put_le32(cd, 0x02014b50u); put_le16(cd, 20); put_le16(cd, 10); put_le16(cd, 0); put_le16(cd, 0); put_le16(cd, 0); put_le16(cd, 0);
    put_le32(cd, crc); put_le32(cd, (uint32_t)data->n); put_le32(cd, (uint32_t)data->n);
    put_le16(cd, (unsigned)strlen(name)); put_le16(cd, 0); put_le16(cd, 0); put_le16(cd, 0); put_le16(cd, 0);
    put_le32(cd, 0); put_le32(cd, off); put_str(cd, name);
    (*count)++;
}

static void test_zip(void)
{
    buf wav = make_wav(1, 0), zip = { 0 }, cd = { 0 }, txt = { 0 }, cdg = { 0 };
    uint32_t count = 0;
    fa_tags* t = NULL;
    fa_stream_info info;
    uint8_t pack[24 * 4];
    memset(pack, 0, sizeof(pack));
    put_str(&txt, "readme");
    put(&cdg, pack, sizeof(pack));
    zip_add(&zip, &cd, "readme.txt", &txt, &count);
    zip_add(&zip, &cd, "track01.wav", &wav, &count);     /* not named like the .cdg */
    zip_add(&zip, &cd, "song.cdg", &cdg, &count);
    {
        uint32_t cd_off = (uint32_t)zip.n;
        put_buf(&zip, &cd);
        put_le32(&zip, 0x06054b50u); put_le16(&zip, 0); put_le16(&zip, 0); put_le16(&zip, count); put_le16(&zip, count);
        put_le32(&zip, (uint32_t)cd.n); put_le32(&zip, cd_off); put_le16(&zip, 0);
    }
    CHECK_EQ(fa_tags_read_memory(zip.p, zip.n, 0, &t), FA_OK);
    CHECK_STR(fa_tags_get(t, FA_TAG_TITLE), "ID3 Title");
    CHECK_EQ(fa_tags_get_info(t, &info), FA_OK);
    CHECK_EQ(info.has_cdg, 1);
    CHECK_EQ(info.audio_bytes, 400);
    CHECK_EQ(info.file_size, (int64_t)zip.n);
    fa_tags_close(t);
    {
        /* A stream opened from the ZIP: the same tags, and the ZIP's size. */
        fa_stream* s = NULL;
        fa_open_options o;
        fa_open_options_init(&o);
        o.flags = FA_OPEN_DECODE_ONLY;
        CHECK_EQ(fa_stream_open_memory(g_engine, zip.p, zip.n, &o, &s), FA_OK);
        CHECK_EQ(fa_stream_get_tags(s, 0, &t), FA_OK);
        CHECK_STR(fa_tags_get(t, FA_TAG_TITLE), "ID3 Title");
        CHECK_EQ(fa_tags_get_info(t, &info), FA_OK);
        CHECK_EQ(info.has_cdg, 1);
        CHECK_EQ(info.file_size, (int64_t)zip.n);
        fa_tags_close(t);
        fa_stream_close(s);
    }
    {
        /* The same from a path. */
        fa_stream* s = NULL;
        fa_open_options o;
        const char* path = "fa_test_tags.zip";
        FILE* f = fopen(path, "wb");
        CHECK(f != NULL);
        if (f) { fwrite(zip.p, 1, zip.n, f); fclose(f); }
        fa_open_options_init(&o);
        o.flags = FA_OPEN_DECODE_ONLY;
        CHECK_EQ(fa_stream_open_path(g_engine, path, &o, &s), FA_OK);
        CHECK_EQ(fa_stream_get_tags(s, 0, &t), FA_OK);
        CHECK_STR(fa_tags_get(t, FA_TAG_TITLE), "ID3 Title");
        CHECK_EQ(fa_tags_get_info(t, &info), FA_OK);
        CHECK_EQ(info.file_size, (int64_t)zip.n);
        fa_tags_close(t);
        fa_stream_close(s);
        remove(path);
    }
    buf_free(&wav);
    buf_free(&zip);
    buf_free(&cd);
    buf_free(&txt);
    buf_free(&cdg);
}

/* ---- files written by FFmpeg -------------------------------------------------------- */

static void data_path(char* out, size_t cap, const char* name) { snprintf(out, cap, "%s/tags/%s", FA_TEST_DATA_DIR, name); }

static void check_common(fa_tags* t, int has_disc)
{
    CHECK_STR(fa_tags_get(t, FA_TAG_ARTIST), "The Artist");
    CHECK_STR(fa_tags_get(t, FA_TAG_ALBUM), "Great Album");
    CHECK_STR(fa_tags_get(t, FA_TAG_GENRE), "Rock");
    CHECK_STR(fa_tags_get(t, FA_TAG_DATE), "2024");
    CHECK_STR(fa_tags_get(t, FA_TAG_TRACK), "3");
    CHECK_STR(fa_tags_get(t, FA_TAG_TRACK_TOTAL), "12");
    CHECK_STR(fa_tags_get(t, FA_TAG_COMPOSER), "Composer Name");
    CHECK_STR(fa_tags_get(t, FA_TAG_COPYRIGHT), "(c) Me");
    if (has_disc) {
        CHECK_STR(fa_tags_get(t, FA_TAG_ALBUM_ARTIST), "Various");
        CHECK_STR(fa_tags_get(t, FA_TAG_DISC), "1");
        CHECK_STR(fa_tags_get(t, FA_TAG_DISC_TOTAL), "2");
    }
}

static void test_ffmpeg_files(void)
{
    static const struct { const char* name; const char* title; int pictures; int has_disc; const char* codec; } files[] = {
        { "id3v23.mp3", "S\xC3\xB3ng T\xC3\xAFtle", 1, 1, "mp3" },
        { "id3v24.mp3", "Song Title", 0, 1, "mp3" },
        { "tags.flac", "Song Title", 1, 1, "flac" },
        { "tags.m4a", "Song Title", 1, 1, "aac" },
        { "tags.opus", "Song Title", 0, 1, "opus" },
        { "tags.aiff", "Song Title", 0, 1, "pcm" },
        { "tags.mka", "Song Title", 1, 0, "opus" }
    };
    size_t i;
    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        char path[512];
        fa_tags* t = NULL;
        fa_stream_info info;
        fa_tag_picture p;
        data_path(path, sizeof(path), files[i].name);
        CHECK_EQ(fa_tags_read_path(path, 0, &t), FA_OK);
        CHECK_STR(fa_tags_get(t, FA_TAG_TITLE), files[i].title);
        check_common(t, files[i].has_disc);
        CHECK_EQ(fa_tags_get_picture_count(t), files[i].pictures);
        if (files[i].pictures) {
            CHECK_EQ(fa_tags_get_cover(t, &p), FA_OK);
            CHECK(strcmp(p.mime, "image/jpeg") == 0 || strcmp(p.mime, "image/png") == 0);
            CHECK(p.size > 50);
        }
        CHECK_EQ(fa_tags_get_info(t, &info), FA_OK);
        CHECK_STR(info.codec, files[i].codec);
        CHECK_NEAR((double)info.duration / 1e9, 0.5, 0.03);
        CHECK(info.audio_bytes > 0 && info.audio_bytes < info.file_size);
        CHECK(info.bitrate > 0);
        fa_tags_close(t);
    }
    {
        /* Known values: the MP3 (128 kbps CBR) and the AIFF. The MP3's 9406 bytes are a
         * 212-byte ID3v2 tag, a 417-byte LAME Info frame (not audio) and 8777 of audio. */
        char path[512];
        fa_tags* t = NULL;
        fa_stream_info info;
        data_path(path, sizeof(path), "id3v24.mp3");
        CHECK_EQ(fa_tags_read_path(path, FA_TAGS_NO_PICTURES, &t), FA_OK);
        CHECK_EQ(fa_tags_get_info(t, &info), FA_OK);
        CHECK_NEAR(info.bitrate, 128000, 128000 * 0.2);
        CHECK_EQ(info.audio_bytes, 8777);
        CHECK_EQ(info.file_size, 9406);
        CHECK_EQ(info.bits_per_sample, 0);
        fa_tags_close(t);
        data_path(path, sizeof(path), "tags.aiff");
        CHECK_EQ(fa_tags_read_path(path, FA_TAGS_NO_PICTURES, &t), FA_OK);
        CHECK_EQ(fa_tags_get_info(t, &info), FA_OK);
        CHECK_EQ(info.audio_bytes, 8000);
        CHECK_EQ(info.bitrate, 128000);
        CHECK_EQ(info.bits_per_sample, 16);
        fa_tags_close(t);
        /* Tags only: no properties. */
        CHECK_EQ(fa_tags_read_path(path, FA_TAGS_NO_INFO, &t), FA_OK);
        CHECK_EQ(fa_tags_get_info(t, &info), FA_ERROR_NOT_FOUND);
        CHECK_STR(fa_tags_get(t, FA_TAG_TITLE), "Song Title");
        fa_tags_close(t);
    }
    {
        /* A stream of the file gives the same tags, and its info the same properties. */
        char path[512];
        fa_stream* s = NULL;
        fa_tags* t = NULL;
        fa_stream_info a, b;
        fa_open_options o;
        fa_open_options_init(&o);
        o.flags = FA_OPEN_DECODE_ONLY;
        data_path(path, sizeof(path), "tags.m4a");
        CHECK_EQ(fa_stream_open_path(g_engine, path, &o, &s), FA_OK);
        CHECK_EQ(fa_stream_get_tags(s, 0, &t), FA_OK);
        CHECK_STR(fa_tags_get(t, FA_TAG_TITLE), "Song Title");
        CHECK_EQ(fa_tags_get_picture_count(t), 1);
        CHECK_EQ(fa_tags_get_info(t, &a), FA_OK);
        CHECK_EQ(fa_stream_get_info(s, &b), FA_OK);
        CHECK_EQ(a.bitrate, b.bitrate);
        CHECK_EQ(a.audio_bytes, b.audio_bytes);
        CHECK_EQ(a.file_size, b.file_size);
        fa_tags_close(t);
        fa_stream_close(s);
    }
}

static void test_errors_and_empty(void)
{
    fa_tags* t = (fa_tags*)1;
    fa_tag_item it;
    fa_tag_picture p;
    char path[512];
    uint8_t junk[64];
    data_path(path, sizeof(path), "missing.mp3");
    CHECK_EQ(fa_tags_read_path(path, 0, &t), FA_ERROR_NOT_FOUND);
    CHECK(t == NULL);
    CHECK_EQ(fa_tags_read_path(NULL, 0, &t), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_tags_read_memory(NULL, 4, 0, &t), FA_ERROR_INVALID_ARG);
    CHECK_STR(fa_tags_get(NULL, FA_TAG_TITLE), "");
    CHECK_EQ(fa_tags_get_item_count(NULL), 0);
    fa_tags_close(NULL);
    /* No tags: an empty object. */
    memset(junk, 0x55, sizeof(junk));
    CHECK_EQ(fa_tags_read_memory(junk, sizeof(junk), 0, &t), FA_OK);
    CHECK_EQ(fa_tags_get_item_count(t), 0);
    CHECK_STR(fa_tags_get(t, FA_TAG_TITLE), "");
    CHECK_STR(fa_tags_get(t, FA_TAG_FIELD_COUNT), "");
    CHECK_EQ(fa_tags_get_item(t, 0, &it), FA_ERROR_NOT_FOUND);
    CHECK_EQ(fa_tags_get_picture(t, 0, &p), FA_ERROR_NOT_FOUND);
    fa_tags_close(t);
}

/* Hostile tags: a Vorbis comment block with 70,000 ARTIST values keeps 65,536
 * items and a joined field of at most 64 KB, and reads in moments (joining them
 * all took minutes). */
static void test_hostile_tag_counts(void)
{
    buf file = { 0 }, vc = { 0 };
    fa_tags* t;
    uint8_t si[34];
    uint32_t i, n = 70000;
    memset(si, 0, sizeof(si));
    si[12] = 0x01; si[13] = 0x70;
    put_str(&file, "fLaC");
    put_u8(&file, 0); put_u8(&file, 0); put_u8(&file, 0); put_u8(&file, 34); put(&file, si, 34);
    put_le32(&vc, 6); put_str(&vc, "vendor");
    put_le32(&vc, n);
    for (i = 0; i < n; i++) {
        char item[32];
        snprintf(item, sizeof(item), "ARTIST=Artist %u", (unsigned)i);
        vc_item(&vc, item);
    }
    put_u8(&file, 0x84); put_u8(&file, (unsigned)(vc.n >> 16)); put_be16(&file, (unsigned)vc.n & 0xFFFF); put_buf(&file, &vc);
    put_str(&file, "\xFF\xF8 frames");
    t = read_mem(&file, 0);
    CHECK(t != NULL);
    if (t) {
        const char* artist = fa_tags_get(t, FA_TAG_ARTIST);
        CHECK_EQ(fa_tags_get_item_count(t), 65536);
        CHECK(strlen(artist) <= 65536 && strlen(artist) > 60000);
        CHECK(strncmp(artist, "Artist 0; Artist 1; ", 20) == 0);
        fa_tags_close(t);
    }
    buf_free(&vc);
    buf_free(&file);
}

/* An empty ID3v2 frame (some writers make them) is skipped: the frames after it
 * are still read. */
static void test_id3_empty_frame(void)
{
    buf file = { 0 }, frames = { 0 }, body = { 0 };
    fa_tags* t;
    id3_frame23(&frames, "TPE1", &body, 0);
    text_body(&body, 0, "After");
    id3_frame23(&frames, "TIT2", &body, 0);
    id3_header(&file, 3, 0, (uint32_t)frames.n);
    put_buf(&file, &frames);
    put_str(&file, "\xFF\xFB\x90\x00");
    t = read_mem(&file, 0);
    CHECK(t != NULL);
    if (t) {
        CHECK_STR(fa_tags_get(t, FA_TAG_TITLE), "After");
        fa_tags_close(t);
    }
    buf_free(&body);
    buf_free(&frames);
    buf_free(&file);
}

/* Damaged tags never read outside the data: prefixes of each tagged file, and
 * every byte flipped in the first and the last 400 (where the tags are). */
static void test_damaged(void)
{
    static const char* names[] = { "id3v23.mp3", "id3v24.mp3", "tags.flac", "tags.m4a", "tags.mka", "tags.aiff", "tags.opus" };
    size_t k;
    for (k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
        char path[512];
        FILE* f;
        uint8_t* d;
        long n;
        size_t i;
        data_path(path, sizeof(path), names[k]);
        f = fopen(path, "rb");
        CHECK(f != NULL);
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        d = (uint8_t*)malloc((size_t)n);
        CHECK(fread(d, 1, (size_t)n, f) == (size_t)n);
        fclose(f);
        for (i = 1; i < (size_t)n; i += 1 + i / 16) {
            fa_tags* t = NULL;
            if (fa_tags_read_memory(d, i, FA_TAGS_NO_INFO, &t) == FA_OK) fa_tags_close(t);
        }
        for (i = 0; i < (size_t)n; i++) {
            fa_tags* t = NULL;
            if (i == 400 && n > 800) i = (size_t)n - 400;
            uint8_t save = d[i];
            d[i] ^= 0xA5;
            if (fa_tags_read_memory(d, (size_t)n, FA_TAGS_NO_INFO, &t) == FA_OK) fa_tags_close(t);
            d[i] = save;
        }
        free(d);
    }
}

int main(void)
{
    fa_engine_config cfg;
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    cfg.worker_threads = 2;
    if (fa_engine_create(&cfg, &g_engine) != FA_OK) { fprintf(stderr, "engine create failed\n"); return 1; }

    RUN(test_id3v23_all_frames);
    RUN(test_id3v23_unsync);
    RUN(test_id3v24);
    RUN(test_id3v22);
    RUN(test_v1_ape_priority);
    RUN(test_flac_blocks);
    RUN(test_mp4_ilst);
    RUN(test_wav_aiff);
    RUN(test_matroska_tags);
    RUN(test_matroska_targeted_tags);
    RUN(test_flac_cuesheet_and_vendor);
    RUN(test_midi_without_soundfont);
    RUN(test_id3_compressed);
    RUN(test_sylt);
    RUN(test_lyrics3);
    RUN(test_zip);
    RUN(test_ffmpeg_files);
    RUN(test_errors_and_empty);
    RUN(test_damaged);
    RUN(test_hostile_tag_counts);
    RUN(test_id3_empty_frame);

    fa_engine_destroy(g_engine);
    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    printf("PASS\n");
    return 0;
}
