#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L /* nanosleep under strict C11 */
#endif
/* Core engine tests: decoder registry, custom I/O, stream info, markers, devices.
 * Every input file is generated here, so the tests need no fixtures. */
#include "fatzaudio/fatzaudio.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  include <windows.h>
static void sleep_ms(unsigned ms) { Sleep(ms); }
#else
#  include <time.h>
static void sleep_ms(unsigned ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; return; } } while (0)
#define CHECK_EQ(a, b) do { long long va_ = (long long)(a), vb_ = (long long)(b); if (va_ != vb_) { \
    fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_failures++; return; } } while (0)
#define RUN(t) do { int before_ = g_failures; printf("%s\n", #t); t(); if (g_failures == before_) printf("  ok\n"); } while (0)

/* ---- generated test media ------------------------------------------------- */

typedef struct buf { uint8_t* p; size_t n, cap; } buf;
static void put(buf* b, const void* d, size_t n)
{
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2; b->p = (uint8_t*)realloc(b->p, b->cap); }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void put8(buf* b, uint32_t v) { uint8_t c = (uint8_t)v; put(b, &c, 1); }
static void le16(buf* b, uint32_t v) { put8(b, v); put8(b, v >> 8); }
static void le32(buf* b, uint32_t v) { le16(b, v); le16(b, v >> 16); }
static void be16(buf* b, uint32_t v) { put8(b, v >> 8); put8(b, v); }
static void be32(buf* b, uint32_t v) { be16(b, v >> 16); be16(b, v); }

static int16_t tone(uint32_t i, uint32_t rate, uint32_t ch)
{
    return (int16_t)(sin(2.0 * 3.14159265358979 * (440.0 + 110.0 * ch) * i / rate) * 12000.0);
}

static buf make_wav(uint32_t rate, uint32_t channels, uint32_t frames)
{
    buf b = { 0 };
    uint32_t i, c, data = frames * channels * 2;
    put(&b, "RIFF", 4); le32(&b, 36 + data); put(&b, "WAVE", 4);
    put(&b, "fmt ", 4); le32(&b, 16); le16(&b, 1); le16(&b, channels);
    le32(&b, rate); le32(&b, rate * channels * 2); le16(&b, channels * 2); le16(&b, 16);
    put(&b, "data", 4); le32(&b, data);
    for (i = 0; i < frames; i++) for (c = 0; c < channels; c++) le16(&b, (uint16_t)tone(i, rate, c));
    return b;
}

static buf make_aiff(uint32_t rate, uint32_t channels, uint32_t frames)
{
    buf b = { 0 };
    uint32_t i, c, data = frames * channels * 2;
    uint8_t ext[10] = { 0 };
    /* 80-bit IEEE extended sample rate: exponent 16383 + 15 for rates in [32768, 65536). */
    uint32_t e = 16383 + 15, m = rate << 16;
    ext[0] = (uint8_t)(e >> 8); ext[1] = (uint8_t)e;
    ext[2] = (uint8_t)(m >> 24); ext[3] = (uint8_t)(m >> 16); ext[4] = (uint8_t)(m >> 8); ext[5] = (uint8_t)m;
    put(&b, "FORM", 4); be32(&b, 4 + 26 + 16 + data); put(&b, "AIFF", 4);
    put(&b, "COMM", 4); be32(&b, 18); be16(&b, channels); be32(&b, frames); be16(&b, 16); put(&b, ext, 10);
    put(&b, "SSND", 4); be32(&b, 8 + data); be32(&b, 0); be32(&b, 0);
    for (i = 0; i < frames; i++) for (c = 0; c < channels; c++) be16(&b, (uint16_t)tone(i, rate, c));
    return b;
}

/* FLAC with VERBATIM subframes: valid, uncompressed. See the FLAC format spec. */
static uint8_t crc8(const uint8_t* d, size_t n)
{
    uint8_t c = 0; size_t i; int k;
    for (i = 0; i < n; i++) { c ^= d[i]; for (k = 0; k < 8; k++) c = (uint8_t)((c & 0x80) ? (c << 1) ^ 0x07 : c << 1); }
    return c;
}
static uint16_t crc16(const uint8_t* d, size_t n)
{
    uint16_t c = 0; size_t i; int k;
    for (i = 0; i < n; i++) { c ^= (uint16_t)(d[i] << 8); for (k = 0; k < 8; k++) c = (uint16_t)((c & 0x8000) ? (c << 1) ^ 0x8005 : c << 1); }
    return c;
}

static buf make_flac(uint32_t rate, uint32_t channels, uint32_t frames, int with_id3)
{
    const uint32_t block = 4096;
    buf b = { 0 };
    uint32_t start, f, i, c;
    if (with_id3) {
        uint8_t id3[10] = { 'I', 'D', '3', 4, 0, 0, 0, 0, 0, 20 };
        uint8_t pad[20] = { 0 };
        put(&b, id3, 10); put(&b, pad, 20);
    }
    put(&b, "fLaC", 4);
    put8(&b, 0x80); put8(&b, 0); put8(&b, 0); put8(&b, 34);       /* last block: STREAMINFO */
    be16(&b, block); be16(&b, block); put8(&b, 0); put8(&b, 0); put8(&b, 0); put8(&b, 0); put8(&b, 0); put8(&b, 0);
    /* rate(20) channels-1(3) bps-1(5) total(36) */
    put8(&b, rate >> 12); put8(&b, rate >> 4);
    put8(&b, ((rate & 15) << 4) | ((channels - 1) << 1) | (15 >> 4));
    put8(&b, ((15 & 15) << 4) | 0); be32(&b, frames);
    for (i = 0; i < 16; i++) put8(&b, 0); /* MD5 unknown */

    for (start = 0, f = 0; start < frames; start += block, f++) {
        uint32_t n = frames - start < block ? frames - start : block;
        size_t h = b.n;
        uint8_t hdr[8];
        put8(&b, 0xFF); put8(&b, 0xF8);                             /* sync, fixed blocksize */
        put8(&b, (7 << 4) | 0);                                      /* blocksize: 16-bit n-1 follows; rate from STREAMINFO */
        put8(&b, ((channels - 1) << 4) | (4 << 1));                  /* independent channels, 16 bps */
        if (f < 128) put8(&b, f); else { put8(&b, 0xC0 | (f >> 6)); put8(&b, 0x80 | (f & 63)); } /* UTF-8 frame number */
        be16(&b, n - 1);
        memcpy(hdr, b.p + h, b.n - h);
        put8(&b, crc8(hdr, b.n - h));
        for (c = 0; c < channels; c++) {
            put8(&b, 0x02);                                          /* VERBATIM, no wasted bits */
            for (i = 0; i < n; i++) be16(&b, (uint16_t)tone(start + i, rate, c));
        }
        be16(&b, crc16(b.p + h, b.n - h));
    }
    return b;
}

/* MP3: MPEG-1 Layer III 128 kbps 44.1 kHz frames with zeroed side info (decode to silence). */
static buf make_mp3(uint32_t nframes, int with_id3)
{
    buf b = { 0 };
    uint32_t f, i;
    if (with_id3) {
        uint8_t id3[10] = { 'I', 'D', '3', 3, 0, 0, 0, 0, 1, 0 }; /* 128-byte tag body */
        put(&b, id3, 10);
        for (i = 0; i < 128; i++) put8(&b, 0);
    }
    for (f = 0; f < nframes; f++) {
        put8(&b, 0xFF); put8(&b, 0xFB); put8(&b, 0x90); put8(&b, 0x44); /* joint stereo */
        for (i = 4; i < 417; i++) put8(&b, 0);
    }
    return b;
}

/* ---- custom I/O ------------------------------------------------------------ */

typedef struct mem_io { const uint8_t* p; size_t n, pos; int closes; } mem_io;
static int64_t FA_CALL io_read(void* u, void* dst, size_t bytes)
{
    mem_io* m = (mem_io*)u;
    size_t k = m->n - m->pos;
    if (bytes > 1000) bytes = 1000; /* force short reads */
    if (k > bytes) k = bytes;
    memcpy(dst, m->p + m->pos, k);
    m->pos += k;
    return (int64_t)k;
}
static fa_result FA_CALL io_seek(void* u, int64_t off)
{
    mem_io* m = (mem_io*)u;
    if (off < 0 || (size_t)off > m->n) return FA_ERROR_INVALID_ARG;
    m->pos = (size_t)off;
    return FA_OK;
}
static int64_t FA_CALL io_length(void* u) { return (int64_t)((mem_io*)u)->n; }
static void FA_CALL io_close(void* u) { ((mem_io*)u)->closes++; }

/* ---- tests ----------------------------------------------------------------- */

static fa_engine* g_engine;

static fa_frames read_all(fa_stream* s, uint32_t channels, int* nonzero)
{
    float* tmp = (float*)malloc(1000 * channels * sizeof(float));
    fa_frames total = 0, got = 0;
    fa_result r;
    *nonzero = 0;
    while ((r = fa_stream_read(s, tmp, 1000, &got)) == FA_OK) {
        fa_frames i;
        if (got <= 0 || got > 1000) { total = -1; break; }
        for (i = 0; i < got * (fa_frames)channels; i++) if (tmp[i] != 0.0f) { *nonzero = 1; break; }
        total += got;
    }
    if (r != FA_ERROR_END_OF_STREAM || got != 0) total = -2;
    free(tmp);
    return total;
}

static fa_open_options decode_only(void)
{
    fa_open_options o;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    return o;
}

static void test_format_list(void)
{
    uint32_t i, n = fa_format_count();
    int wav = 0, flac = 0, mp3 = 0;
    fa_format_info fi;
    CHECK(n >= 3);
    for (i = 0; i < n; i++) {
        CHECK_EQ(fa_format_get_info(i, &fi), FA_OK);
        CHECK(fi.description[0] && fi.extensions[0]);
        wav |= strcmp(fi.name, "wav") == 0;
        flac |= strcmp(fi.name, "flac") == 0;
        mp3 |= strcmp(fi.name, "mp3") == 0;
    }
    CHECK(wav && flac && mp3);
    CHECK_EQ(fa_format_get_info(n, &fi), FA_ERROR_NOT_FOUND);
}

static void check_decodes(buf media, const char* codec, const char* container, uint32_t rate, uint32_t channels,
                          fa_frames expect_frames, int expect_sound)
{
    fa_open_options o = decode_only();
    fa_stream* s = NULL;
    fa_stream_info info;
    int nonzero = 0;
    fa_result r = fa_stream_open_memory(g_engine, media.p, media.n, &o, &s);
    free(media.p);
    CHECK_EQ(r, FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK(strcmp(info.codec, codec) == 0);
    CHECK(strcmp(info.container, container) == 0);
    CHECK_EQ(info.format.sample_format, FA_FORMAT_F32);
    CHECK_EQ(info.format.sample_rate, rate);
    CHECK_EQ(info.format.channels, channels);
    if (expect_frames > 0) {
        CHECK_EQ(info.length_frames, expect_frames);
        CHECK_EQ(info.duration, (fa_time)((double)expect_frames * 1e9 / rate + 0.5));
        CHECK_EQ(read_all(s, channels, &nonzero), expect_frames);
    } else {
        CHECK(read_all(s, channels, &nonzero) > 0);
    }
    CHECK_EQ(nonzero, expect_sound);
    fa_stream_close(s);
}

static void test_detect_wav(void) { check_decodes(make_wav(48000, 2, 12345), "pcm", "wav", 48000, 2, 12345, 1); }
static void test_detect_aiff(void) { check_decodes(make_aiff(44100, 1, 5000), "pcm", "aiff", 44100, 1, 5000, 1); }
static void test_detect_flac(void) { check_decodes(make_flac(44100, 2, 10000, 0), "flac", "flac", 44100, 2, 10000, 1); }
static void test_detect_flac_with_id3(void) { check_decodes(make_flac(32000, 1, 5000, 1), "flac", "flac", 32000, 1, 5000, 1); }
static void test_detect_mp3(void) { check_decodes(make_mp3(40, 0), "mp3", "mp3", 44100, 2, 0, 0); }
static void test_detect_mp3_with_id3(void) { check_decodes(make_mp3(40, 1), "mp3", "mp3", 44100, 2, 0, 0); }

static void test_unsupported(void)
{
    uint8_t junk[3000];
    fa_stream* s = (fa_stream*)1;
    size_t i;
    for (i = 0; i < sizeof(junk); i++) junk[i] = (uint8_t)(i * 7 + 3);
    CHECK_EQ(fa_stream_open_memory(g_engine, junk, sizeof(junk), NULL, &s), FA_ERROR_UNSUPPORTED_FORMAT);
    CHECK(s == NULL);
}

static void test_missing_file(void)
{
    fa_stream* s = NULL;
    CHECK_EQ(fa_stream_open_path(g_engine, "definitely-missing-file.wav", NULL, &s), FA_ERROR_NOT_FOUND);
}

static void test_open_path_utf8(void)
{
    const char* name = "fa-test-\xC3\xA9\xE2\x99\xAA.flac"; /* "fa-test-é♪.flac" */
    buf media = make_flac(44100, 1, 3000, 0);
    fa_stream* s = NULL;
    fa_stream_info info;
    FILE* f;
#ifdef _WIN32
    wchar_t wname[64];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 64);
    f = _wfopen(wname, L"wb");
#else
    f = fopen(name, "wb");
#endif
    CHECK(f != NULL);
    fwrite(media.p, 1, media.n, f);
    fclose(f);
    free(media.p);
    CHECK_EQ(fa_stream_open_path(g_engine, name, NULL, &s), FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK_EQ(info.length_frames, 3000);
    fa_stream_close(s);
#ifdef _WIN32
    _wremove(wname);
#else
    remove(name);
#endif
}

static void test_open_io_seekable(void)
{
    buf media = make_flac(44100, 2, 9000, 0);
    mem_io m = { 0 };
    fa_io_callbacks io = { io_read, io_seek, io_length, io_close };
    fa_open_options o = decode_only();
    fa_stream* s = NULL;
    int nonzero;
    m.p = media.p; m.n = media.n;
    CHECK_EQ(fa_stream_open_io(g_engine, &io, &m, &o, &s), FA_OK);
    CHECK_EQ(fa_stream_seek(s, 4000), FA_OK);
    CHECK_EQ(read_all(s, 2, &nonzero), 5000);
    CHECK_EQ(m.closes, 0);
    fa_stream_close(s);
    CHECK_EQ(m.closes, 1); /* close is called exactly once, by fa_stream_close */
    free(media.p);
}

static void test_open_io_forward_only(void)
{
    buf media = make_wav(22050, 1, 7000);
    mem_io m = { 0 };
    fa_io_callbacks io = { io_read, NULL, NULL, io_close };
    fa_open_options o = decode_only();
    fa_stream* s = NULL;
    fa_stream_info info;
    int nonzero;
    m.p = media.p; m.n = media.n;
    CHECK_EQ(fa_stream_open_io(g_engine, &io, &m, &o, &s), FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK_EQ(info.seekable, 0);
    CHECK_EQ(read_all(s, 1, &nonzero), 7000);
    CHECK(nonzero);
    CHECK_EQ(fa_stream_seek(s, 0), FA_ERROR_NOT_SEEKABLE);
    fa_stream_close(s);
    CHECK_EQ(m.closes, 1);
    free(media.p);
}

static void test_open_io_failure_keeps_ownership(void)
{
    uint8_t junk[512] = { 1, 2, 3 };
    mem_io m = { 0 };
    fa_io_callbacks io = { io_read, io_seek, io_length, io_close };
    fa_stream* s = NULL;
    m.p = junk; m.n = sizeof(junk);
    CHECK_EQ(fa_stream_open_io(g_engine, &io, &m, NULL, &s), FA_ERROR_UNSUPPORTED_FORMAT);
    CHECK(s == NULL);
    CHECK_EQ(m.closes, 0); /* the caller still owns `user` */
    io.read = NULL;
    CHECK_EQ(fa_stream_open_io(g_engine, &io, &m, NULL, &s), FA_ERROR_INVALID_ARG);
}

static void test_read_end_of_stream(void)
{
    buf media = make_wav(8000, 1, 100);
    fa_open_options o = decode_only();
    fa_stream* s = NULL;
    float tmp[256];
    fa_frames got = -1;
    CHECK_EQ(fa_stream_open_memory(g_engine, media.p, media.n, &o, &s), FA_OK);
    free(media.p);
    CHECK_EQ(fa_stream_read(s, tmp, 256, &got), FA_OK);
    CHECK_EQ(got, 100);
    CHECK_EQ(fa_stream_read(s, tmp, 256, &got), FA_ERROR_END_OF_STREAM);
    CHECK_EQ(got, 0);
    CHECK_EQ(fa_stream_read(s, tmp, 256, &got), FA_ERROR_END_OF_STREAM); /* stays at the end */
    CHECK_EQ(fa_stream_read(s, tmp, 0, &got), FA_OK);
    CHECK_EQ(got, 0);
    fa_stream_close(s);
}

static void test_markers(void)
{
    buf media = make_wav(8000, 1, 8000);
    fa_stream* s = NULL;
    fa_frames f = 0;
    CHECK_EQ(fa_stream_open_memory(g_engine, media.p, media.n, NULL, &s), FA_OK);
    free(media.p);
    CHECK_EQ(fa_stream_get_marker(s, FA_MARKER_MIX_POINT, &f), FA_OK);
    CHECK_EQ(f, FA_FRAMES_UNSET);
    CHECK_EQ(fa_stream_set_marker(s, FA_MARKER_MIX_POINT, 4000), FA_OK);
    CHECK_EQ(fa_stream_get_marker(s, FA_MARKER_MIX_POINT, &f), FA_OK);
    CHECK_EQ(f, 4000);
    CHECK_EQ(fa_stream_set_marker(s, FA_MARKER_MIX_POINT, FA_FRAMES_UNSET), FA_OK);
    CHECK_EQ(fa_stream_get_marker(s, FA_MARKER_MIX_POINT, &f), FA_OK);
    CHECK_EQ(f, FA_FRAMES_UNSET);
    CHECK_EQ(fa_stream_set_marker(s, FA_MARKER_CUE_IN, -5), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_stream_set_marker(s, (fa_marker)99, 0), FA_ERROR_INVALID_ARG);
    fa_stream_close(s);
}

static void test_device_start_stop_idempotent(void)
{
    fa_device* d = NULL;
    int32_t started = -1;
    CHECK_EQ(fa_device_open(g_engine, NULL, &d), FA_OK);
    CHECK_EQ(fa_device_is_started(d, &started), FA_OK);
    CHECK_EQ(started, 0);
    CHECK_EQ(fa_device_stop(d), FA_OK);
    CHECK_EQ(fa_device_start(d), FA_OK);
    CHECK_EQ(fa_device_start(d), FA_OK);
    CHECK_EQ(fa_device_is_started(d, &started), FA_OK);
    CHECK_EQ(started, 1);
    CHECK_EQ(fa_device_stop(d), FA_OK);
    CHECK_EQ(fa_device_stop(d), FA_OK);
    CHECK_EQ(fa_device_is_started(d, &started), FA_OK);
    CHECK_EQ(started, 0);
    fa_device_close(d);
}

static fa_frames wait_for_position(fa_stream* s, fa_frames at_least)
{
    fa_frames pos = 0;
    int i;
    for (i = 0; i < 300; i++) {
        fa_stream_get_position(s, &pos);
        if (pos >= at_least) break;
        sleep_ms(10);
    }
    return pos;
}

static void test_attach_keeps_position_and_format(void)
{
    buf media = make_wav(22050, 1, 22050 * 10);
    fa_device *d1 = NULL, *d2 = NULL;
    fa_stream* s = NULL;
    fa_stream_info info;
    fa_stream_state st;
    fa_frames pos = 0, pos2 = 0;
    CHECK_EQ(fa_device_open(g_engine, NULL, &d1), FA_OK);
    CHECK_EQ(fa_device_open(g_engine, NULL, &d2), FA_OK);
    CHECK_EQ(fa_stream_open_memory(g_engine, media.p, media.n, NULL, &s), FA_OK);
    free(media.p);

    CHECK_EQ(fa_stream_seek(s, 1000), FA_OK);
    CHECK_EQ(fa_stream_attach(s, d1, NULL), FA_OK);
    CHECK_EQ(fa_stream_attach(s, d1, NULL), FA_OK);           /* same device: no-op */
    CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
    CHECK_EQ(pos, 1000);                                       /* attach does not rewind */
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK_EQ(info.format.sample_rate, 22050);                  /* source format, not the device's */
    CHECK_EQ(info.format.channels, 1);

    CHECK_EQ(fa_device_start(d1), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    pos = wait_for_position(s, 3000);
    CHECK(pos >= 3000);
    CHECK_EQ(fa_stream_pause(s), FA_OK);
    CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);

    CHECK_EQ(fa_stream_attach(s, d2, NULL), FA_OK);           /* move to another device */
    CHECK_EQ(fa_stream_get_position(s, &pos2), FA_OK);
    CHECK_EQ(pos2, pos);
    CHECK_EQ(fa_stream_get_state(s, &st), FA_OK);
    CHECK_EQ(st, FA_STATE_PAUSED);

    CHECK_EQ(fa_stream_detach(s), FA_OK);
    CHECK_EQ(fa_stream_detach(s), FA_OK);
    CHECK_EQ(fa_stream_get_position(s, &pos2), FA_OK);
    CHECK_EQ(pos2, pos);

    fa_stream_close(s);
    fa_device_close(d1);
    fa_device_close(d2);
}

static void test_decode_only_cannot_attach(void)
{
    buf media = make_wav(8000, 1, 800);
    fa_open_options o = decode_only();
    fa_device* d = NULL;
    fa_stream* s = NULL;
    CHECK_EQ(fa_device_open(g_engine, NULL, &d), FA_OK);
    CHECK_EQ(fa_stream_open_memory(g_engine, media.p, media.n, &o, &s), FA_OK);
    free(media.p);
    CHECK_EQ(fa_stream_attach(s, d, NULL), FA_ERROR_INVALID_ARG);
    fa_stream_close(s);
    fa_device_close(d);
}

static void test_play_to_end(void)
{
    buf media = make_wav(44100, 2, 4410); /* 100 ms */
    fa_device* d = NULL;
    fa_stream* s = NULL;
    fa_stream_state st = FA_STATE_PLAYING;
    int i;
    CHECK_EQ(fa_device_open(g_engine, NULL, &d), FA_OK);
    CHECK_EQ(fa_stream_open_memory(g_engine, media.p, media.n, NULL, &s), FA_OK);
    free(media.p);
    CHECK_EQ(fa_stream_attach(s, d, NULL), FA_OK);
    CHECK_EQ(fa_device_start(d), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    for (i = 0; i < 300 && st != FA_STATE_ENDED; i++) { sleep_ms(10); fa_stream_get_state(s, &st); }
    CHECK_EQ(st, FA_STATE_ENDED);
    fa_stream_close(s);   /* closing an attached stream detaches it first */
    fa_device_close(d);
}

int main(void)
{
    fa_engine_config cfg;
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT; /* real-time silent output: works on CI without audio hardware */
    if (fa_engine_create(&cfg, &g_engine) != FA_OK) { fprintf(stderr, "engine create failed\n"); return 1; }

    RUN(test_format_list);
    RUN(test_detect_wav);
    RUN(test_detect_aiff);
    RUN(test_detect_flac);
    RUN(test_detect_flac_with_id3);
    RUN(test_detect_mp3);
    RUN(test_detect_mp3_with_id3);
    RUN(test_unsupported);
    RUN(test_missing_file);
    RUN(test_open_path_utf8);
    RUN(test_open_io_seekable);
    RUN(test_open_io_forward_only);
    RUN(test_open_io_failure_keeps_ownership);
    RUN(test_read_end_of_stream);
    RUN(test_markers);
    RUN(test_device_start_stop_idempotent);
    RUN(test_attach_keeps_position_and_format);
    RUN(test_decode_only_cannot_attach);
    RUN(test_play_to_end);

    fa_engine_destroy(g_engine);
    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    printf("PASS\n");
    return 0;
}
