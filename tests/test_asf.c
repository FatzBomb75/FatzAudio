/* White-box tests of the ASF / WMA demuxer (demux_asf.c), on the small files in
 * tests/data/asf (make_asf_data.sh). Every audio track is a sine of its own
 * frequency and level. PCM tracks decode without FFmpeg and check the demuxer
 * exactly (lengths, fragments, seeks, forward-only sources, track selection, tags,
 * DRM, damaged input); WMA tracks need FFmpeg on this platform (Media Foundation
 * on Windows) and are skipped without it. */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "fa_internal.h"
#include "demux.h"
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
/* WMA decodes through Media Foundation on Windows; everywhere else it is unsupported. */
#ifdef _WIN32
#define FA_HAVE_WMA 1
#else
#define FA_HAVE_WMA 0
#endif
#define SKIP_NO_WMA(r) do { if (!FA_HAVE_WMA) { CHECK_EQ((r), FA_ERROR_UNSUPPORTED_FORMAT); printf("  (WMA decodes on Windows only)\n"); return; } } while (0)

static fa_engine* g_engine;

/* pcm.asf: 1.25 s at 8 kHz. */
#define PCM_FRAMES 10000

static void data_path(char* out, size_t cap, const char* name)
{
    snprintf(out, cap, "%s/asf/%s", FA_TEST_DATA_DIR, name);
}

static uint8_t* load_file(const char* name, size_t* size)
{
    char path[512];
    FILE* f;
    uint8_t* p;
    long n;
    data_path(path, sizeof(path), name);
    f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    p = (uint8_t*)malloc((size_t)n + 1);
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); p = NULL; }
    fclose(f);
    *size = (size_t)n;
    return p;
}

static fa_result open_file(const char* name, int32_t track, fa_stream** s)
{
    char path[512];
    fa_open_options o;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    o.audio_track = track;
    data_path(path, sizeof(path), name);
    *s = NULL;
    return fa_stream_open_path(g_engine, path, &o, s);
}

static fa_result open_memory(const uint8_t* d, size_t n, int32_t track, fa_stream** s)
{
    fa_open_options o;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    o.audio_track = track;
    *s = NULL;
    return fa_stream_open_memory(g_engine, d, n, &o, s);
}

static float* read_all(fa_stream* s, uint32_t channels, int64_t* out_n)
{
    int64_t cap = 1 << 16, n = 0;
    float* all = (float*)malloc((size_t)cap * channels * sizeof(float));
    for (;;) {
        fa_frames got = 0;
        fa_result r;
        if (n + 1024 > cap) {
            cap *= 2;
            all = (float*)realloc(all, (size_t)cap * channels * sizeof(float));
        }
        r = fa_stream_read(s, all + (size_t)n * channels, 1024, &got);
        n += got;
        if (r != FA_OK || got == 0) break;
    }
    *out_n = n;
    return all;
}

static double rms(const float* p, int64_t from, int64_t to, uint32_t ch)
{
    double acc = 0;
    int64_t i;
    for (i = from; i < to; i++) acc += (double)p[i * ch] * p[i * ch];
    return to > from ? sqrt(acc / (double)(to - from)) : 0.0;
}

/* Frequency of channel 0 from its zero crossings. */
static double freq(const float* p, int64_t from, int64_t to, uint32_t ch, uint32_t rate)
{
    int64_t i, first = -1, last = -1, crossings = 0;
    for (i = from + 1; i < to; i++)
        if ((p[(i - 1) * ch] < 0) != (p[i * ch] < 0)) {
            if (first < 0) first = i;
            last = i;
            crossings++;
        }
    if (crossings < 2) return 0;
    return (double)(crossings - 1) / 2.0 / ((double)(last - first) / rate);
}

/* Seeks to each target and compares the next frames with the full decode; the
 * largest difference goes to *worst. Returns 0 if a seek read too little. */
static int seek_matches(fa_stream* s, const float* full, int64_t total, uint32_t ch,
                        const int64_t* targets, int count, double* worst)
{
    int k;
    *worst = 0;
    for (k = 0; k < count; k++) {
        int64_t at = targets[k], len = 1500, n = 0, i;
        float* p;
        if (at > total) continue;
        if (at + len > total) len = total - at;
        p = (float*)calloc((size_t)len * ch + 1, sizeof(float));
        if (fa_stream_seek(s, at) != FA_OK) { free(p); return 0; }
        while (n < len) {
            fa_frames got = 0;
            fa_frames want = len - n < 333 ? len - n : 333;
            fa_result r = fa_stream_read(s, p + (size_t)n * ch, want, &got);
            n += got;
            if (r != FA_OK || got == 0) break;
        }
        if (n != len) {
            fprintf(stderr, "  seek to %lld: %lld of %lld frames\n", (long long)at, (long long)n, (long long)len);
            free(p);
            return 0;
        }
        for (i = 0; i < len * (int64_t)ch; i++) {
            double d = fabs((double)p[i] - full[(size_t)at * ch + (size_t)i]);
            if (d > *worst) *worst = d;
        }
        free(p);
    }
    return 1;
}

/* ---- forward-only sources --------------------------------------------------------- */

typedef struct mem_io {
    const uint8_t* p;
    size_t n, pos;
} mem_io;

static int64_t FA_CALL io_read(void* user, void* buf, size_t bytes)
{
    mem_io* m = (mem_io*)user;
    size_t k = m->n - m->pos < bytes ? m->n - m->pos : bytes;
    memcpy(buf, m->p + m->pos, k);
    m->pos += k;
    return (int64_t)k;
}

static void FA_CALL io_close(void* user) { (void)user; }

/* Decodes `d` from a forward-only source. */
static float* forward_only(const uint8_t* d, size_t size, int32_t track, uint32_t ch, int64_t* n, fa_result* r, int* seekable)
{
    mem_io m;
    fa_io_callbacks io = { io_read, NULL, NULL, io_close };
    fa_open_options o;
    fa_stream* s = NULL;
    fa_stream_info info;
    float* out;
    m.p = d;
    m.n = size;
    m.pos = 0;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    o.audio_track = track;
    *r = fa_stream_open_io(g_engine, &io, &m, &o, &s);
    if (*r != FA_OK) return NULL;
    fa_stream_get_info(s, &info);
    *seekable = info.seekable;
    out = read_all(s, ch, n);
    fa_stream_close(s);
    return out;
}

/* ---- tests ------------------------------------------------------------------------- */

static void test_registry_and_probe(void)
{
    fa_format_info fi;
    uint32_t i;
    int found = 0;
    for (i = 0; i < fa_format_count(); i++) {
        CHECK_EQ(fa_format_get_info(i, &fi), FA_OK);
        if (strcmp(fi.name, "asf") == 0) {
            found = 1;
            CHECK(strstr(fi.extensions, "wma") != NULL);
        }
    }
    CHECK(found);
    {
        uint8_t junk[64];
        fa_stream* s;
        memset(junk, 0x30, sizeof(junk));
        CHECK(open_memory(junk, sizeof(junk), -1, &s) != FA_OK);
    }
}

/* PCM in ASF: media objects split across 512-byte packets. Exact length, exact
 * audio, exact seeks (bisection over 49 packets). */
static void test_pcm_exact(void)
{
    fa_stream* s;
    fa_stream_info info;
    float* all;
    int64_t n, i;
    double worst;
    static const int64_t targets[] = { 0, 1, 777, 4000, 5123, 9000, 8, 2047, 2048, 9999 - 1500 };
    CHECK_EQ(open_file("pcm.asf", -1, &s), FA_OK);
    fa_stream_get_info(s, &info);
    CHECK_STR(info.container, "asf");
    CHECK_STR(info.codec, "pcm_s16le");
    CHECK_EQ(info.format.sample_rate, 8000);
    CHECK_EQ(info.format.channels, 1);
    CHECK_EQ(info.length_frames, PCM_FRAMES);
    CHECK(info.seekable);
    all = read_all(s, 1, &n);
    CHECK_EQ(n, PCM_FRAMES);
    CHECK_NEAR(freq(all, 0, n, 1, 8000), 440.0, 1.0);
    CHECK_NEAR(rms(all, 0, n, 1), 0.5 / sqrt(2.0), 0.01);
    /* The samples are the generator's: sin(2 pi 440 t) * 0.5 in 16 bits. */
    for (i = 0; i < n; i++) CHECK_NEAR(all[i], 0.5 * sin(2.0 * 3.14159265358979 * 440.0 * (double)i / 8000.0), 2.0 / 32768.0);
    CHECK(seek_matches(s, all, n, 1, targets, (int)(sizeof(targets) / sizeof(targets[0])), &worst));
    CHECK_NEAR(worst, 0.0, 0.0);
    /* Seeks back and forth again, then to the very end. */
    CHECK(seek_matches(s, all, n, 1, targets + 3, 3, &worst));
    CHECK_NEAR(worst, 0.0, 0.0);
    CHECK_EQ(fa_stream_seek(s, PCM_FRAMES), FA_OK);
    {
        float buf[64];
        fa_frames got = 1;
        fa_stream_read(s, buf, 64, &got);
        CHECK_EQ(got, 0);
    }
    free(all);
    fa_stream_close(s);
}

/* A forward-only source plays the same audio, and is not seekable. */
static void test_forward_only(void)
{
    size_t size = 0;
    uint8_t* d = load_file("pcm.asf", &size);
    fa_stream* s;
    float *a, *b;
    int64_t na, nb, i;
    fa_result r;
    int seekable = -1;
    CHECK(d != NULL);
    CHECK_EQ(open_memory(d, size, -1, &s), FA_OK);
    a = read_all(s, 1, &na);
    fa_stream_close(s);
    b = forward_only(d, size, -1, 1, &nb, &r, &seekable);
    CHECK_EQ(r, FA_OK);
    CHECK_EQ(seekable, 0);
    CHECK_EQ(nb, na);
    for (i = 0; i < na; i++) CHECK_NEAR(b[i], a[i], 0.0);
    free(a);
    free(b);
    /* The PCM track of the video file, past video and WMA payloads. */
    free(d);
    d = load_file("av_tracks.asf", &size);
    CHECK(d != NULL);
    CHECK_EQ(open_memory(d, size, 0, &s), FA_OK);
    a = read_all(s, 1, &na);
    fa_stream_close(s);
    b = forward_only(d, size, 0, 1, &nb, &r, &seekable);
    CHECK_EQ(r, FA_OK);
    CHECK_EQ(seekable, 0);
    CHECK_EQ(nb, na);
    for (i = 0; i < na; i++) CHECK_NEAR(b[i], a[i], 0.0);
    free(a);
    free(b);
    free(d);
}

/* Three audio tracks next to a video stream; a Simple Index. */
static void test_tracks(void)
{
    fa_stream* s;
    fa_stream_info info;
    float* all;
    int64_t n;
    fa_result r;
    double worst;
    static const int64_t targets[] = { 0, 12000, 3333, 8000 };
    CHECK_EQ(open_file("av_tracks.asf", -1, &s), FA_OK);
    fa_stream_get_info(s, &info);
    CHECK_EQ(info.audio_track_count, 3);
    CHECK_STR(info.codec, "pcm_s16le");
    fa_stream_close(s);

    CHECK_EQ(open_file("av_tracks.asf", 3, &s), FA_ERROR_NOT_FOUND);

    /* Track 0: PCM, 440 Hz, exact seeks with the index. */
    CHECK_EQ(open_file("av_tracks.asf", 0, &s), FA_OK);
    fa_stream_get_info(s, &info);
    CHECK_EQ(info.format.sample_rate, 8000);
    all = read_all(s, 1, &n);
    /* FFmpeg starts this track 46 ms in (silence before it), so 16368 frames; the
     * length comes from the track's last object, not the file's play duration. */
    CHECK_EQ(n, info.length_frames);
    CHECK_EQ(n, 16368);
    CHECK_NEAR(freq(all, 0, n, 1, 8000), 440.0, 1.0);
    CHECK(seek_matches(s, all, n, 1, targets, 4, &worst));
    CHECK_NEAR(worst, 0.0, 0.0);
    free(all);
    fa_stream_close(s);

    /* Track 1: WMA 1, 1000 Hz stereo. */
    r = open_file("av_tracks.asf", 1, &s);
    SKIP_NO_WMA(r);
    CHECK_EQ(r, FA_OK);
    fa_stream_get_info(s, &info);
    CHECK_STR(info.codec, "wmav1");
    CHECK_EQ(info.format.sample_rate, 22050);
    CHECK_EQ(info.format.channels, 2);
    all = read_all(s, 2, &n);
    printf("  wmav1: %lld frames (length %lld)\n", (long long)n, (long long)info.length_frames);
    CHECK(llabs((long long)n - 44100) < 3000);
    CHECK(llabs((long long)n - (long long)info.length_frames) <= 2048);
    CHECK_NEAR(freq(all, 4000, n - 4000, 2, 22050), 1000.0, 5.0);
    free(all);
    fa_stream_close(s);

    /* Track 2: WMA 2, 2000 Hz mono. */
    CHECK_EQ(open_file("av_tracks.asf", 2, &s), FA_OK);
    fa_stream_get_info(s, &info);
    CHECK_STR(info.codec, "wmav2");
    CHECK_EQ(info.format.sample_rate, 16000);
    CHECK_EQ(info.format.channels, 1);
    all = read_all(s, 1, &n);
    CHECK(llabs((long long)n - 32000) < 3000);
    CHECK_NEAR(freq(all, 4000, n - 4000, 1, 16000), 2000.0, 10.0);
    free(all);
    fa_stream_close(s);
}

/* WMA 2 at 44.1 kHz: length from the File Properties, the tone, seeks. */
static void test_wmav2(void)
{
    fa_stream* s;
    fa_stream_info info;
    float* all;
    int64_t n;
    fa_result r;
    double worst;
    static const int64_t targets[] = { 0, 44100, 2048, 10000, 70000, 20480, 1 };
    r = open_file("tone.wma", -1, &s);
    SKIP_NO_WMA(r);
    CHECK_EQ(r, FA_OK);
    fa_stream_get_info(s, &info);
    CHECK_STR(info.container, "asf");
    CHECK_STR(info.codec, "wmav2");
    CHECK_EQ(info.format.sample_rate, 44100);
    CHECK_EQ(info.format.channels, 2);
    CHECK(info.seekable);
    /* Play duration less preroll: 1.999 s as FFmpeg writes it. */
    CHECK(llabs((long long)info.length_frames - 88200) < 100);
    all = read_all(s, 2, &n);
    printf("  %lld frames decoded, length %lld\n", (long long)n, (long long)info.length_frames);
    CHECK(llabs((long long)n - 88200) < 4096);
    CHECK_NEAR(freq(all, 8192, 80000, 2, 44100), 440.0, 2.0);
    CHECK_NEAR(rms(all, 8192, 80000, 2), 0.5 / sqrt(2.0), 0.03);
    /* Seeks land on the frames a straight decode gives there. */
    CHECK(seek_matches(s, all, n, 2, targets, (int)(sizeof(targets) / sizeof(targets[0])), &worst));
    printf("  largest difference after seeks: %g\n", worst);
    CHECK(worst < 1e-4);
    free(all);
    fa_stream_close(s);
}

static void test_wmav2_forward_only(void)
{
    size_t size = 0;
    uint8_t* d = load_file("tone.wma", &size);
    fa_stream* s;
    float *a, *b;
    int64_t na, nb, i;
    fa_result r;
    int seekable = -1;
    CHECK(d != NULL);
    r = open_memory(d, size, -1, &s);
    if (!FA_HAVE_WMA) { CHECK_EQ(r, FA_ERROR_UNSUPPORTED_FORMAT); printf("  (WMA decodes on Windows only)\n"); free(d); return; }
    CHECK_EQ(r, FA_OK);
    a = read_all(s, 2, &na);
    fa_stream_close(s);
    b = forward_only(d, size, -1, 2, &nb, &r, &seekable);
    CHECK_EQ(r, FA_OK);
    CHECK_EQ(seekable, 0);
    CHECK_EQ(nb, na);
    for (i = 0; i < na * 2; i++) CHECK_NEAR(b[i], a[i], 0.0);
    free(a);
    free(b);
    free(d);
}

/* Title and author, and ReplayGain for loudness normalization. */
static void test_tags(void)
{
    char path[512];
    fa_source src;
    fa_tag_loudness l;
    fa_tags* t = NULL;
    fa_stream_info info;
    data_path(path, sizeof(path), "tone.wma");
    CHECK_EQ(fa_tags_read_path(path, 0, &t), FA_OK);
    CHECK_STR(fa_tags_get(t, FA_TAG_TITLE), "Test Tone");
    CHECK_STR(fa_tags_get(t, FA_TAG_ARTIST), "FatzAudio");
    CHECK_STR(fa_tags_get(t, FA_TAG_ALBUM), "");
    CHECK(fa_tags_get_item_count(t) >= 3);
    /* Properties: only where Media Foundation can open WMA. */
    if (fa_tags_get_info(t, &info) == FA_OK) CHECK(info.bitrate > 0);
    fa_tags_close(t);
    CHECK_EQ(fa_source_open_path(&src, path), FA_OK);
    fa_tags_read_loudness(&src, &l);
    CHECK_NEAR(l.track_lufs, -18.0 + 6.5, 1e-9);
    CHECK_NEAR(l.track_peak_db, 20.0 * log10(0.5), 1e-9);
    CHECK(l.album_lufs != l.album_lufs);     /* NAN: absent */
    fa_source_close(&src);
}

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* Inserts an object (`guid`, body of `body_len` zero bytes) as the first child of
 * the Header Object of `d`. */
static uint8_t* insert_header_object(const uint8_t* d, size_t n, const uint8_t guid[16], size_t body_len, size_t* out_n)
{
    size_t add = 24 + body_len;
    uint8_t* p = (uint8_t*)calloc(n + add, 1);
    memcpy(p, d, 30);
    memcpy(p + 30, guid, 16);
    wr32(p + 46, (uint32_t)add);
    memcpy(p + 30 + add, d + 30, n - 30);
    wr32(p + 16, rd32(d + 16) + (uint32_t)add);    /* header size */
    wr32(p + 24, rd32(d + 24) + 1);                /* object count */
    *out_n = n + add;
    return p;
}

/* DRM-protected files are refused with FA_ERROR_PROTECTED_CONTENT. */
static void test_drm_refused(void)
{
    static const uint8_t content_encryption[16] = { 0xFB, 0xB3, 0x11, 0x22, 0x23, 0xBD, 0xD2, 0x11,
                                                    0xB4, 0xB7, 0x00, 0xA0, 0xC9, 0x55, 0xFC, 0x6E };
    static const uint8_t ext_content_encryption[16] = { 0x14, 0xE6, 0x8A, 0x29, 0x22, 0x26, 0x17, 0x4C,
                                                        0xB9, 0x35, 0xDA, 0xE0, 0x7E, 0xE9, 0x28, 0x9C };
    static const uint8_t unknown[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    size_t size = 0, m, i;
    uint8_t* d = load_file("pcm.asf", &size);
    uint8_t* p;
    fa_stream* s;
    CHECK(d != NULL);
    /* An unknown object is skipped: the edit itself is sound. */
    p = insert_header_object(d, size, unknown, 8, &m);
    CHECK_EQ(open_memory(p, m, -1, &s), FA_OK);
    fa_stream_close(s);
    free(p);
    p = insert_header_object(d, size, content_encryption, 16, &m);
    CHECK_EQ(open_memory(p, m, -1, &s), FA_ERROR_PROTECTED_CONTENT);
    free(p);
    p = insert_header_object(d, size, ext_content_encryption, 4, &m);
    CHECK_EQ(open_memory(p, m, -1, &s), FA_ERROR_PROTECTED_CONTENT);
    free(p);
    /* The Encrypted Content flag of the stream (Stream Properties, flags bit 15). */
    p = (uint8_t*)malloc(size);
    memcpy(p, d, size);
    for (i = 30; i + 24 + 50 < size; i++)
        if (p[i] == 0x91 && p[i + 1] == 0x07 && p[i + 2] == 0xDC && p[i + 3] == 0xB7) {
            p[i + 24 + 48 + 1] |= 0x80;
            break;
        }
    CHECK_EQ(open_memory(p, size, -1, &s), FA_ERROR_PROTECTED_CONTENT);
    free(p);
    free(d);
}

/* Media objects that span packets come out whole and in order: the packet
 * sequence of the demuxer itself. */
static void test_objects_and_seek_positions(void)
{
    char path[512];
    fa_source src;
    fa_decoder dec;
    float buf[256];
    fa_frames got;
    data_path(path, sizeof(path), "pcm.asf");
    CHECK_EQ(fa_source_open_path(&src, path), FA_OK);
    CHECK_EQ(fa_decoder_open(&dec, &src, path, NULL, -1, NULL), FA_OK);
    CHECK_STR(dec.container, "asf");
    CHECK(dec.vt->fast_seek);
    CHECK_EQ(dec.length, PCM_FRAMES);
    /* Positions are frame exact: the sample after a seek is the sine's value there. */
    {
        static const fa_frames at[] = { 4321, 17, 9876, 0, 5000 };
        size_t k;
        for (k = 0; k < sizeof(at) / sizeof(at[0]); k++) {
            CHECK_EQ(fa_decoder_seek(&dec, at[k]), FA_OK);
            CHECK_EQ(fa_decoder_read(&dec, buf, 1, &got), FA_OK);
            CHECK_EQ(got, 1);
            CHECK_NEAR(buf[0], 0.5 * sin(2.0 * 3.14159265358979 * 440.0 * (double)at[k] / 8000.0), 2.0 / 32768.0);
        }
    }
    CHECK_EQ(fa_decoder_seek(&dec, PCM_FRAMES + 1), FA_ERROR_INVALID_ARG);
    fa_decoder_close(&dec);
    fa_source_close(&src);
}

/* ---- a file written here: compressed payloads ---------------------------------------- */

typedef struct wbuf { uint8_t* p; size_t n; } wbuf;

static void put(wbuf* b, const void* d, size_t n) { memcpy(b->p + b->n, d, n); b->n += n; }
static void put8(wbuf* b, uint32_t v) { uint8_t x = (uint8_t)v; put(b, &x, 1); }
static void put16(wbuf* b, uint32_t v) { put8(b, v & 255); put8(b, v >> 8); }
static void put32(wbuf* b, uint32_t v) { put16(b, v & 0xFFFF); put16(b, v >> 16); }
static void put64(wbuf* b, uint64_t v) { put32(b, (uint32_t)v); put32(b, (uint32_t)(v >> 32)); }
static void put_guid(wbuf* b, uint32_t d1, uint16_t d2, uint16_t d3, const char* d4)
{
    int i;
    put32(b, d1);
    put16(b, d2);
    put16(b, d3);
    for (i = 0; i < 8; i++) {
        unsigned v;
        sscanf(d4 + 2 * i, "%2x", &v);
        put8(b, v);
    }
}

static int16_t synth(int64_t k) { return (int16_t)((k * 37) % 20000 - 10000); }

#define CP_PACKETS 20
#define CP_SUBS 5           /* objects per packet */
#define CP_SAMPLES 96       /* per object: 12 ms at 8 kHz */
#define CP_PACKET 1024

/* PCM, 8 kHz mono, every packet one compressed payload of five 12 ms objects. */
static wbuf make_compressed(void)
{
    wbuf b;
    size_t hdr_at, k = 0;
    int i, j, m;
    b.p = (uint8_t*)calloc(4096 + CP_PACKETS * CP_PACKET, 1);
    b.n = 0;
    put_guid(&b, 0x75B22630, 0x668E, 0x11CF, "A6D900AA0062CE6C");   /* Header */
    hdr_at = b.n;
    put64(&b, 0);
    put32(&b, 2);
    put8(&b, 1);
    put8(&b, 2);
    put_guid(&b, 0x8CABDCA1, 0xA947, 0x11CF, "8EE400C00C205365");   /* File Properties */
    put64(&b, 104);
    for (i = 0; i < 16; i++) put8(&b, 0);
    put64(&b, 0);                                                     /* file size */
    put64(&b, 0);
    put64(&b, CP_PACKETS);
    put64(&b, (uint64_t)CP_PACKETS * CP_SUBS * CP_SAMPLES * 10000000 / 8000);   /* play duration */
    put64(&b, (uint64_t)CP_PACKETS * CP_SUBS * CP_SAMPLES * 10000000 / 8000);
    put64(&b, 0);                                                     /* preroll */
    put32(&b, 2);
    put32(&b, CP_PACKET);
    put32(&b, CP_PACKET);
    put32(&b, 128000);
    put_guid(&b, 0xB7DC0791, 0xA9B7, 0x11CF, "8EE600C00C205365");   /* Stream Properties */
    put64(&b, 24 + 54 + 18);
    put_guid(&b, 0xF8699E40, 0x5B4D, 0x11CF, "A8FD00805F5C442B");   /* audio */
    put_guid(&b, 0x20FB5700, 0x5B55, 0x11CF, "A8FD00805F5C442B");   /* no error correction */
    put64(&b, 0);
    put32(&b, 18);
    put32(&b, 0);
    put16(&b, 1);                                                     /* stream 1 */
    put32(&b, 0);
    put16(&b, 1); put16(&b, 1); put32(&b, 8000); put32(&b, 16000); put16(&b, 2); put16(&b, 16); put16(&b, 0);
    wr32(b.p + hdr_at, (uint32_t)b.n);
    put_guid(&b, 0x75B22636, 0x668E, 0x11CF, "A6D900AA0062CE6C");   /* Data */
    put64(&b, 50 + (uint64_t)CP_PACKETS * CP_PACKET);
    for (i = 0; i < 16; i++) put8(&b, 0);
    put64(&b, CP_PACKETS);
    put16(&b, 0x0101);
    for (i = 0; i < CP_PACKETS; i++) {
        size_t start = b.n;
        uint32_t pts = (uint32_t)(i * CP_SUBS * 12);
        size_t used = 3 + 2 + 1 + 6 + 8 + CP_SUBS * (1 + CP_SAMPLES * 2);
        put8(&b, 0x82); put8(&b, 0); put8(&b, 0);
        put8(&b, 0x08);                       /* single payload, padding length as a byte */
        put8(&b, 0x5D);
        put8(&b, (uint32_t)(CP_PACKET - used));
        put32(&b, pts);                       /* send time */
        put16(&b, 60);
        put8(&b, 0x81);                       /* stream 1, key frame */
        put8(&b, (uint32_t)i);                /* media object number */
        put32(&b, pts);                       /* presentation time (compressed payload) */
        put8(&b, 1);                          /* replicated data: 1 byte */
        put8(&b, 12);                         /* time delta */
        for (j = 0; j < CP_SUBS; j++) {
            put8(&b, CP_SAMPLES * 2);
            for (m = 0; m < CP_SAMPLES; m++) put16(&b, (uint16_t)synth((int64_t)k++));
        }
        b.n = start + CP_PACKET;
    }
    return b;
}

static void test_compressed_payloads(void)
{
    wbuf f = make_compressed();
    fa_stream* s;
    fa_stream_info info;
    float* all;
    int64_t n, i, total = CP_PACKETS * CP_SUBS * CP_SAMPLES;
    double worst;
    static const int64_t targets[] = { 5000, 96, 95, 0, 7777, 480, 9000 };
    CHECK_EQ(open_memory(f.p, f.n, -1, &s), FA_OK);
    fa_stream_get_info(s, &info);
    CHECK_STR(info.codec, "pcm_s16le");
    CHECK_EQ(info.length_frames, total);
    CHECK(info.seekable);
    all = read_all(s, 1, &n);
    CHECK_EQ(n, total);
    for (i = 0; i < n; i++) CHECK_NEAR(all[i], synth(i) / 32768.0, 0.0);
    /* Seeks inside a compressed payload restart at the right object. */
    CHECK(seek_matches(s, all, n, 1, targets, (int)(sizeof(targets) / sizeof(targets[0])), &worst));
    CHECK_NEAR(worst, 0.0, 0.0);
    free(all);
    fa_stream_close(s);
    free(f.p);
}

/* ---- damaged files ------------------------------------------------------------------- */

static void exercise_memory(const uint8_t* data, size_t size, int32_t track)
{
    fa_stream* s = NULL;
    fa_stream_info info;
    float buf[4096 * 2];
    int k;
    if (open_memory(data, size, track, &s) != FA_OK) return;
    fa_stream_get_info(s, &info);
    for (k = 0; k < 64; k++) {
        fa_frames got = 0;
        uint32_t ch = info.format.channels ? info.format.channels : 1;
        if (fa_stream_read(s, buf, 4096 / ch, &got) != FA_OK || got == 0) break;
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

static void test_damaged_files_do_not_crash(void)
{
    static const char* names[] = { "pcm.asf", "av_tracks.asf", "tone.wma" };
    size_t f;
    uint32_t seed = 4242;
    for (f = 0; f < sizeof(names) / sizeof(names[0]); f++) {
        size_t size = 0, cut, i;
        uint8_t* data = load_file(names[f], &size);
        uint8_t* bad;
        int pcm_only = strcmp(names[f], "tone.wma") != 0;
        size_t hdr;
        CHECK(data != NULL);
        hdr = rd32(data + 16) + 50;
        bad = (uint8_t*)malloc(size);
        for (cut = 1; cut < size; cut += size / 31 + 1) {
            exercise_memory(data, cut, -1);
            exercise_memory(data, cut, 2);
        }
        /* Changed bytes, where the built-in PCM decoder reads the track (FFmpeg
         * would log every corrupt WMA packet). The header gets its share. */
        for (i = 0; i < 300 && pcm_only; i++) {
            size_t j, flips = 1 + i % 6;
            memcpy(bad, data, size);
            for (j = 0; j < flips; j++) {
                size_t at;
                seed = seed * 1103515245u + 12345u;
                at = (i & 1) ? (seed >> 8) % (size < 1024 ? size : 1024) : (seed >> 8) % size;
                /* The video file: only in its packets, so the PCM track stays track 0
                 * (a damaged header could make it a WMA track, and FFmpeg log). */
                if (f == 1) at = hdr + (seed >> 8) % (size - hdr);
                bad[at] = (uint8_t)(seed >> 3);
            }
            exercise_memory(bad, size, f == 1 ? 0 : -1);
        }
        free(bad);
        free(data);
    }
}

int main(void)
{
    fa_engine_config cfg;
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    cfg.worker_threads = 2;
    if (fa_engine_create(&cfg, &g_engine) != FA_OK) { fprintf(stderr, "engine create failed\n"); return 1; }

    RUN(test_registry_and_probe);
    RUN(test_pcm_exact);
    RUN(test_forward_only);
    RUN(test_objects_and_seek_positions);
    RUN(test_tracks);
    RUN(test_wmav2);
    RUN(test_wmav2_forward_only);
    RUN(test_tags);
    RUN(test_drm_refused);
    RUN(test_compressed_payloads);
    RUN(test_damaged_files_do_not_crash);

    fa_engine_destroy(g_engine);
    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    printf("PASS\n");
    return 0;
}
