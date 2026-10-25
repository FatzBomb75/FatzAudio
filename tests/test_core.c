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
#if defined(__APPLE__)
#  include <unistd.h>
#endif
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

/* With `fake_syncs`, every 256th sample of channel 0 starts a run of three that reads
 * as a frame header for 24-bit audio (FF F8 C0 1C 00 00), which a search must skip:
 * a false sync, as real audio has now and then, that fails before its CRC-8 is read. */
static int16_t flac_sample(uint32_t i, uint32_t rate, uint32_t ch, int fake_syncs)
{
    static const int16_t fake[3] = { (int16_t)0xFFF8, (int16_t)0xC01C, 0 };
    if (fake_syncs && ch == 0 && i % 256 < 3) return fake[i % 256];
    return tone(i, rate, ch);
}

/* Blocks that start before frame `quiet` are digital silence (CONSTANT subframes,
 * a few bytes each), so the file's bytes per frame change across it. With
 * `seek_every`, a SEEKTABLE has a point every that many blocks. */
static buf make_flac_ex(uint32_t rate, uint32_t channels, uint32_t frames, int with_id3, uint32_t quiet, uint32_t seek_every, int fake_syncs)
{
    const uint32_t block = 4096;
    buf b = { 0 }, fr = { 0 };
    uint32_t start, f, i, c, points = seek_every ? (frames + block * seek_every - 1) / (block * seek_every) : 0;
    uint64_t* offset = (uint64_t*)calloc(points + 1, sizeof(uint64_t));
    for (start = 0, f = 0; start < frames; start += block, f++) {
        uint32_t n = frames - start < block ? frames - start : block;
        size_t h = fr.n;
        uint8_t hdr[8];
        if (seek_every && f % seek_every == 0) offset[f / seek_every] = fr.n;
        put8(&fr, 0xFF); put8(&fr, 0xF8);                           /* sync, fixed blocksize */
        put8(&fr, (7 << 4) | 0);                                     /* blocksize: 16-bit n-1 follows; rate from STREAMINFO */
        put8(&fr, ((channels - 1) << 4) | (4 << 1));                 /* independent channels, 16 bps */
        if (f < 128) put8(&fr, f); else { put8(&fr, 0xC0 | (f >> 6)); put8(&fr, 0x80 | (f & 63)); } /* UTF-8 frame number */
        be16(&fr, n - 1);
        memcpy(hdr, fr.p + h, fr.n - h);
        put8(&fr, crc8(hdr, fr.n - h));
        for (c = 0; c < channels; c++) {
            if (start < quiet) { put8(&fr, 0x00); be16(&fr, 0); continue; } /* CONSTANT 0 */
            put8(&fr, 0x02);                                         /* VERBATIM, no wasted bits */
            for (i = 0; i < n; i++) be16(&fr, (uint16_t)flac_sample(start + i, rate, c, fake_syncs));
        }
        be16(&fr, crc16(fr.p + h, fr.n - h));
    }

    if (with_id3) {
        uint8_t id3[10] = { 'I', 'D', '3', 4, 0, 0, 0, 0, 0, 20 };
        uint8_t pad[20] = { 0 };
        put(&b, id3, 10); put(&b, pad, 20);
    }
    put(&b, "fLaC", 4);
    put8(&b, points ? 0x00 : 0x80); put8(&b, 0); put8(&b, 0); put8(&b, 34); /* STREAMINFO */
    be16(&b, block); be16(&b, block); put8(&b, 0); put8(&b, 0); put8(&b, 0); put8(&b, 0); put8(&b, 0); put8(&b, 0);
    /* rate(20) channels-1(3) bps-1(5) total(36) */
    put8(&b, rate >> 12); put8(&b, rate >> 4);
    put8(&b, ((rate & 15) << 4) | ((channels - 1) << 1) | (15 >> 4));
    put8(&b, ((15 & 15) << 4) | 0); be32(&b, frames);
    for (i = 0; i < 16; i++) put8(&b, 0); /* MD5 unknown */
    if (points) {
        put8(&b, 0x83); put8(&b, 0); be16(&b, points * 18);          /* last block: SEEKTABLE */
        for (i = 0; i < points; i++) {
            uint32_t first = i * seek_every * block;
            be32(&b, 0); be32(&b, first);
            be32(&b, (uint32_t)(offset[i] >> 32)); be32(&b, (uint32_t)offset[i]);
            be16(&b, frames - first < block ? frames - first : block);
        }
    }
    put(&b, fr.p, fr.n);
    free(fr.p);
    free(offset);
    return b;
}

static buf make_flac(uint32_t rate, uint32_t channels, uint32_t frames, int with_id3)
{
    return make_flac_ex(rate, channels, frames, with_id3, 0, 0, 0);
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

/* fail_at: reads at or past this byte fail (0 = never); seek_limit: seeks past it
 * are refused (0 = no limit). */
typedef struct mem_io { const uint8_t* p; size_t n, pos; int closes; size_t fail_at, seek_limit; int seeks; size_t bytes; } mem_io;
static int64_t FA_CALL io_read(void* u, void* dst, size_t bytes)
{
    mem_io* m = (mem_io*)u;
    size_t k = m->n - m->pos;
    if (m->fail_at) {
        if (m->pos >= m->fail_at) return FA_ERROR_IO;
        if (k > m->fail_at - m->pos) k = m->fail_at - m->pos; /* partial, then the error */
    }
    if (bytes > 1000) bytes = 1000; /* force short reads */
    if (k > bytes) k = bytes;
    memcpy(dst, m->p + m->pos, k);
    m->pos += k;
    m->bytes += k;
    return (int64_t)k;
}
static fa_result FA_CALL io_seek(void* u, int64_t off)
{
    mem_io* m = (mem_io*)u;
    if (off < 0 || (size_t)off > m->n) return FA_ERROR_INVALID_ARG;
    if (m->seek_limit && (size_t)off > m->seek_limit) return FA_ERROR_IO;
    m->pos = (size_t)off;
    m->seeks++;
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

/* Sample value i of channel c in the tests below: walks every byte pattern. */
static int32_t sample_value(uint32_t i, uint32_t c, int bits)
{
    uint32_t x = (i * 2654435761u) ^ (c * 0x9E3779B9u);
    int32_t top = (int32_t)((1u << (bits - 1)) - 1);
    if (i == 0) return c ? -top - 1 : top;                                   /* the extremes */
    return (int32_t)(x << (32 - bits)) >> (32 - bits);
}

/* PCM WAV of 8, 16, 24, 32-bit integers or 32-bit floats, read back as
 * x / 2^(bits-1) (8-bit is unsigned). RIFX is the big-endian variant. */
static buf make_wav_bits(uint32_t channels, uint32_t frames, int bits, int is_float, int rifx)
{
    buf b = { 0 };
    uint32_t i, c, bps = (uint32_t)bits / 8, data = frames * channels * bps;
    void (*w16)(buf*, uint32_t) = rifx ? be16 : le16;
    void (*w32)(buf*, uint32_t) = rifx ? be32 : le32;
    put(&b, rifx ? "RIFX" : "RIFF", 4); w32(&b, 36 + data); put(&b, "WAVE", 4);
    put(&b, "fmt ", 4); w32(&b, 16); w16(&b, is_float ? 3 : 1); w16(&b, channels);
    w32(&b, 48000); w32(&b, 48000 * channels * bps); w16(&b, channels * bps); w16(&b, (uint32_t)bits);
    put(&b, "data", 4); w32(&b, data);
    for (i = 0; i < frames; i++)
        for (c = 0; c < channels; c++) {
            int32_t v = sample_value(i, c, bits);
            uint32_t k;
            if (is_float) { float f = (float)v / 2147483648.0f; uint32_t u; memcpy(&u, &f, 4); w32(&b, u); }
            else if (bits == 8) put8(&b, (uint32_t)(v + 128));
            else for (k = 0; k < bps; k++) put8(&b, (uint32_t)v >> (8 * (rifx ? bps - 1 - k : k)));
        }
    return b;
}

static void test_wav_sample_formats(void)
{
    /* 16 to 32-bit RIFF reads straight into the output; 8-bit and RIFX (k >= 5) go
     * through dr_wav's own conversion. */
    static const int bits[] = { 8, 16, 24, 32, 32, 16, 24 };
    int k;
    for (k = 0; k < 7; k++) {
        const uint32_t ch = 3, frames = 5000;
        int is_float = k == 4;
        buf media = make_wav_bits(ch, frames, bits[k], is_float, k >= 5);
        fa_open_options o = decode_only();
        fa_stream* s = NULL;
        float* out = (float*)malloc(sizeof(float) * frames * ch);
        fa_frames n = 0;
        uint32_t i;
        CHECK_EQ(fa_stream_open_memory(g_engine, media.p, media.n, &o, &s), FA_OK);
        while (n < (fa_frames)frames) {
            fa_frames got = 0, want = (n / 7) % 3 == 0 ? 7 : 1000;    /* odd sizes too */
            if (want > (fa_frames)frames - n) want = (fa_frames)frames - n;
            if (fa_stream_read(s, out + n * ch, want, &got) != FA_OK || got == 0) break;
            n += got;
        }
        CHECK_EQ(n, frames);
        /* And after a seek, in the middle of a read block. */
        CHECK_EQ(fa_stream_seek(s, 1234), FA_OK);
        CHECK_EQ(fa_stream_read(s, out + 1234 * ch, 3000, &n), FA_OK);
        CHECK_EQ(n, 3000);
        for (i = 0; i < frames * ch; i++) {
            int32_t v = sample_value(i / ch, i % ch, bits[k]);
            float want = is_float ? (float)v / 2147483648.0f
                       : (float)((double)v / (double)(1u << (bits[k] - 1)));
            /* 8-bit goes through dr_wav's own mapping (255 is 1.0); the rest is exact. */
            if (bits[k] == 8 ? fabs(out[i] - want) > 1.0 / 127 : out[i] != want) {
                fprintf(stderr, "  %d-bit%s sample %u: %.9g, expected %.9g\n", bits[k], is_float ? " float" : "", i, out[i], want);
                CHECK(0);
            }
        }
        fa_stream_close(s);
        free(out);
        free(media.p);
    }
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

/* A .flac without a SEEKTABLE (FatzAudio's and FFmpeg's encoders write none) seeks
 * by searching the file for the frame, and one with a coarse SEEKTABLE searches
 * between its points: every seek lands on exactly the right sample, in a few
 * probes (one file seek each), also where silence makes the bytes per frame jump. */
static void test_flac_seek_search(void)
{
    /* Probes read a frame header, not the frame: bytes read per seek, on average. */
    static const struct { uint32_t quiet, seek_every; int fake_syncs; uint32_t max_seeks, max_bytes; } cases[] = {
        { 0, 0, 0, 4, 56000 },            /* upstream dr_flac: up to 11; whole-frame probes: 65000 bytes */
        { 73 * 4096, 0, 0, 17, 120000 },  /* silence, then the tone: 17 just where it starts (upstream: up to 11; whole-frame probes: 174000 bytes) */
        { 0, 40, 0, 6, 58000 },           /* a point every 40 blocks, 3.7 s (upstream: up to 8) */
        { 0, 0, 1, 4, 56000 },            /* false syncs (before: up to 18 file seeks and 113000 bytes) */
    };
    const uint32_t frames = 600000;
    size_t q;
    for (q = 0; q < sizeof(cases) / sizeof(cases[0]); q++) {
        buf media = make_flac_ex(44100, 2, frames, 0, cases[q].quiet, cases[q].seek_every, cases[q].fake_syncs);
        mem_io m = { 0 };
        fa_io_callbacks io = { io_read, io_seek, io_length, io_close };
        fa_open_options o = decode_only();
        fa_stream* s = NULL;
        float out[2 * 64];
        uint32_t k, n = cases[q].quiet ? 43 : 40, worst = 0;
        size_t bytes;
        m.p = media.p; m.n = media.n;
        CHECK_EQ(fa_stream_open_io(g_engine, &io, &m, &o, &s), FA_OK);
        bytes = m.bytes;
        for (k = 0; k < n; k++) {
            /* With silence, also just either side of where the tone starts: the search's upper
             * point lands right past the target there, again and again. */
            uint32_t at = k < 40 ? (uint32_t)(((uint64_t)k * 7919u * 4099u) % (frames - 64)) : cases[q].quiet - 4008 + (k - 40) * 4200, i, c;
            fa_frames got = 0;
            int before = m.seeks;
            CHECK_EQ(fa_stream_seek(s, at), FA_OK);
            CHECK_EQ(fa_stream_read(s, out, 64, &got), FA_OK);
            CHECK_EQ(got, 64);
            for (i = 0; i < 64; i++) for (c = 0; c < 2; c++) {
                float want = at + i < cases[q].quiet ? 0.0f : (float)flac_sample(at + i, 44100, c, cases[q].fake_syncs) / 32768.0f;
                if (out[i * 2 + c] != want) {
                    fprintf(stderr, "  case %u, seek to %u: frame %u is %.9g, expected %.9g\n", (unsigned)q, at, at + i, out[i * 2 + c], want);
                    CHECK(0);
                }
            }
            if ((uint32_t)(m.seeks - before) > worst) worst = (uint32_t)(m.seeks - before);
        }
        bytes = (m.bytes - bytes) / n;
        printf("  case %u: at most %u file seeks per seek, %u bytes read per seek\n", (unsigned)q, worst, (unsigned)bytes);
        CHECK(worst <= cases[q].max_seeks);
        CHECK(bytes <= cases[q].max_bytes);
        fa_stream_close(s);
        free(media.p);
    }
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

/* A forward-only source that still reports its length: no seek to the end. */
static void test_forward_only_with_length(void)
{
    buf media = make_wav(22050, 1, 30000);
    mem_io m = { 0 };
    fa_io_callbacks io = { io_read, NULL, io_length, io_close };
    fa_open_options o = decode_only();
    fa_stream* s = NULL;
    int nonzero;
    m.p = media.p; m.n = media.n;
    CHECK_EQ(fa_stream_open_io(g_engine, &io, &m, &o, &s), FA_OK);
    CHECK_EQ(read_all(s, 1, &nonzero), 30000);
    fa_stream_close(s);
    free(media.p);
}

/* AIFF whose sound data runs past the 8 KB prefix, from a forward-only source. */
static void test_forward_only_aiff(void)
{
    buf media = make_aiff(44100, 2, 20000);
    mem_io m = { 0 };
    fa_io_callbacks io = { io_read, NULL, NULL, io_close };
    fa_open_options o = decode_only();
    fa_stream* s = NULL;
    fa_stream_info info;
    int nonzero;
    m.p = media.p; m.n = media.n;
    CHECK_EQ(fa_stream_open_io(g_engine, &io, &m, &o, &s), FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK(strcmp(info.container, "aiff") == 0);
    CHECK_EQ(read_all(s, 2, &nonzero), 20000);
    CHECK(nonzero);
    fa_stream_close(s);
    free(media.p);
}

/* Cover art makes the ID3v2 tag bigger than the probe window. */
static buf with_big_id3(buf inner, size_t tag)
{
    buf b = { 0 };
    uint8_t h[10] = { 'I', 'D', '3', 4, 0, 0, 0, 0, 0, 0 };
    size_t i, body = tag - 10;
    h[6] = (uint8_t)((body >> 21) & 0x7F); h[7] = (uint8_t)((body >> 14) & 0x7F);
    h[8] = (uint8_t)((body >> 7) & 0x7F);  h[9] = (uint8_t)(body & 0x7F);
    put(&b, h, 10);
    for (i = 0; i < body; i++) put8(&b, 0);
    put(&b, inner.p, inner.n);
    free(inner.p);
    return b;
}

static void test_flac_after_big_id3(void)
{
    check_decodes(with_big_id3(make_flac(44100, 2, 6000, 0), 20000), "flac", "flac", 44100, 2, 6000, 1);
}

static void test_flac_after_big_id3_forward_only(void)
{
    buf media = with_big_id3(make_flac(44100, 1, 6000, 0), 20000);
    mem_io m = { 0 };
    fa_io_callbacks io = { io_read, NULL, NULL, io_close };
    fa_open_options o = decode_only();
    fa_stream* s = NULL;
    fa_stream_info info;
    int nonzero;
    m.p = media.p; m.n = media.n;
    o.mime_hint = "audio/flac";           /* nothing past the tag can be looked at */
    CHECK_EQ(fa_stream_open_io(g_engine, &io, &m, &o, &s), FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK(strcmp(info.codec, "flac") == 0);
    CHECK_EQ(read_all(s, 1, &nonzero), 6000);
    fa_stream_close(s);
    free(media.p);
}

static void test_io_read_error_is_reported(void)
{
    buf media = make_wav(22050, 1, 30000);    /* 60 KB */
    mem_io m = { 0 };
    fa_io_callbacks io = { io_read, NULL, NULL, io_close };
    fa_open_options o = decode_only();
    fa_stream* s = NULL;
    float tmp[1000];
    fa_frames got = 0, total = 0;
    fa_result r;
    m.p = media.p; m.n = media.n;
    m.fail_at = 20500;                       /* past the prefix, mid-frame */
    CHECK_EQ(fa_stream_open_io(g_engine, &io, &m, &o, &s), FA_OK);
    while ((r = fa_stream_read(s, tmp, 1000, &got)) == FA_OK) total += got;
    CHECK_EQ(r, FA_ERROR_IO);                /* not a quiet end of stream */
    CHECK_EQ(total, (20500 - 44) / 2);       /* everything before the failure was delivered */
    fa_stream_close(s);
    free(media.p);
}

static void test_io_seek_refused(void)
{
    buf media = make_wav(22050, 1, 30000);
    mem_io m = { 0 };
    fa_io_callbacks io = { io_read, io_seek, io_length, io_close };
    fa_open_options o = decode_only();
    fa_stream* s = NULL;
    fa_frames pos = -1;
    m.p = media.p; m.n = media.n;
    m.seek_limit = 30000;
    CHECK_EQ(fa_stream_open_io(g_engine, &io, &m, &o, &s), FA_OK);
    CHECK(fa_stream_seek(s, 25000) != FA_OK); /* byte 50044: the app refuses */
    CHECK_EQ(fa_stream_seek(s, 10000), FA_OK);
    CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
    CHECK_EQ(pos, 10000);
    fa_stream_close(s);
    free(media.p);
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
#if defined(__APPLE__)
    /* The iOS simulator starts programs elsewhere: tools/ios-sim/run.sh names the folder. */
    if (getenv("FA_TEST_CWD") && chdir(getenv("FA_TEST_CWD")) != 0) return 1;
#endif
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT; /* real-time silent output: works on CI without audio hardware */
    if (fa_engine_create(&cfg, &g_engine) != FA_OK) { fprintf(stderr, "engine create failed\n"); return 1; }

    RUN(test_format_list);
    RUN(test_detect_wav);
    RUN(test_wav_sample_formats);
    RUN(test_detect_aiff);
    RUN(test_detect_flac);
    RUN(test_detect_flac_with_id3);
    RUN(test_detect_mp3);
    RUN(test_detect_mp3_with_id3);
    RUN(test_unsupported);
    RUN(test_missing_file);
    RUN(test_open_path_utf8);
    RUN(test_open_io_seekable);
    RUN(test_flac_seek_search);
    RUN(test_open_io_forward_only);
    RUN(test_open_io_failure_keeps_ownership);
    RUN(test_forward_only_with_length);
    RUN(test_forward_only_aiff);
    RUN(test_flac_after_big_id3);
    RUN(test_flac_after_big_id3_forward_only);
    RUN(test_io_read_error_is_reported);
    RUN(test_io_seek_refused);
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
