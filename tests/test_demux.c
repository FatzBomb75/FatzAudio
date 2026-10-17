/* White-box tests of the MP4 / MOV and Matroska / WebM demuxers, on the small
 * files in tests/data (make_demux_data.sh). Every audio track is a sine of its own
 * frequency and level, so the tests can tell which track came out. Tracks that
 * need FFmpeg (AAC, Opus) are skipped when it is not installed; MP3 and PCM
 * tracks always run. */
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
#define RUN(t) do { int before_ = g_failures; printf("%s\n", #t); t(); if (g_failures == before_) printf("  ok\n"); } while (0)
#define SKIP_NO_FFMPEG(r) do { if ((r) == FA_ERROR_PROVIDER_MISSING) { printf("  (skipped: FFmpeg not found)\n"); return; } } while (0)

static fa_engine* g_engine;

static void data_path(char* out, size_t cap, const char* name)
{
    snprintf(out, cap, "%s/%s", FA_TEST_DATA_DIR, name);
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

/* Decodes `frames` frames from `at` after a seek. */
static float* read_at(fa_stream* s, uint32_t ch, int64_t at, int64_t frames, int64_t* got_total)
{
    float* p = (float*)calloc((size_t)frames * ch + 1, sizeof(float));
    int64_t n = 0;
    *got_total = 0;
    if (fa_stream_seek(s, at) != FA_OK) return p;
    while (n < frames) {
        fa_frames got = 0;
        fa_frames want = frames - n < 777 ? frames - n : 777;   /* odd sizes on purpose */
        fa_result r = fa_stream_read(s, p + (size_t)n * ch, want, &got);
        n += got;
        if (r != FA_OK || got == 0) break;
    }
    *got_total = n;
    return p;
}

/* Seeks to each target and compares what follows with the full decode: exactly
 * (tol 0) or within `tol`. */
static int seek_matches(fa_stream* s, const float* full, int64_t total, uint32_t ch,
                        const int64_t* targets, int count, double tol, double* worst)
{
    int k;
    *worst = 0;
    for (k = 0; k < count; k++) {
        int64_t at = targets[k], len = 3000, got, i;
        float* p;
        if (at > total) continue;
        if (at + len > total) len = total - at;
        p = read_at(s, ch, at, len, &got);
        if (got != len) {
            fprintf(stderr, "  seek to %lld: %lld of %lld frames\n", (long long)at, (long long)got, (long long)len);
            free(p);
            return 0;
        }
        for (i = 0; i < len * (int64_t)ch; i++) {
            double d = fabs((double)p[i] - full[(size_t)at * ch + (size_t)i]);
            if (d > *worst) *worst = d;
        }
        free(p);
        if (*worst > tol) {
            fprintf(stderr, "  seek to %lld: differs by %g\n", (long long)at, *worst);
            return 0;
        }
    }
    return 1;
}

/* ---- MP4 ------------------------------------------------------------------------- */

static void test_mp4_info_and_tracks(void)
{
    fa_stream* s;
    fa_stream_info info;
    float* all;
    int64_t n;
    fa_result r;

    CHECK_EQ(open_file("av_tracks.mp4", -1, &s), FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK_EQ(info.format.sample_rate, 22050);
    CHECK_EQ(info.format.channels, 1);
    CHECK(strcmp(info.codec, "mp3") == 0);
    CHECK(strcmp(info.container, "mp4") == 0);
    CHECK_EQ(info.audio_track_count, 3);
    CHECK_EQ(info.seekable, 1);
    CHECK_NEAR((double)info.length_frames, 44100.0, 30.0);
    all = read_all(s, 1, &n);
    CHECK_EQ(n, info.length_frames);
    CHECK_NEAR(freq(all, n / 4, n * 3 / 4, 1, 22050), 440.0, 2.0);
    CHECK_NEAR(rms(all, n / 4, n * 3 / 4, 1), 0.3536, 0.03);
    free(all);
    fa_stream_close(s);

    CHECK_EQ(open_file("av_tracks.mp4", 0, &s), FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK_EQ(info.format.sample_rate, 22050);
    fa_stream_close(s);

    CHECK_EQ(open_file("av_tracks.mp4", 1, &s), FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK_EQ(info.format.sample_rate, 16000);
    CHECK_EQ(info.format.channels, 2);
    CHECK(strcmp(info.codec, "mp3") == 0);
    all = read_all(s, 2, &n);
    CHECK_NEAR((double)n, 32000.0, 30.0);
    CHECK_NEAR(freq(all, n / 4, n * 3 / 4, 2, 16000), 1000.0, 3.0);
    CHECK_NEAR(rms(all, n / 4, n * 3 / 4, 2), 0.1768, 0.02);
    free(all);
    fa_stream_close(s);

    CHECK_EQ(open_file("av_tracks.mp4", 5, &s), FA_ERROR_NOT_FOUND);
    CHECK(s == NULL);

    r = open_file("av_tracks.mp4", 2, &s);
    SKIP_NO_FFMPEG(r);
    CHECK_EQ(r, FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK(strcmp(info.codec, "aac") == 0);
    CHECK_EQ(info.format.sample_rate, 24000);
    CHECK_EQ(info.format.channels, 1);
    all = read_all(s, 1, &n);
    CHECK_EQ(n, info.length_frames);
    CHECK_NEAR((double)n, 48000.0, 30.0);
    CHECK_NEAR(freq(all, n / 4, n * 3 / 4, 1, 24000), 2000.0, 5.0);
    CHECK_NEAR(rms(all, n / 4, n * 3 / 4, 1), 0.2828, 0.05);
    free(all);
    fa_stream_close(s);
}

static void test_mp4_mp3_seek_is_exact(void)
{
    static const int64_t targets[] = { 0, 1, 575, 576, 577, 1151, 1152, 12345, 30000, 44000, 2000, 0 };
    fa_stream* s;
    float* full;
    int64_t n;
    double worst;
    int t;
    for (t = 0; t < 2; t++) {
        uint32_t ch = t == 0 ? 1 : 2;
        CHECK_EQ(open_file("av_tracks.mp4", t, &s), FA_OK);
        full = read_all(s, ch, &n);
        CHECK(seek_matches(s, full, n, ch, targets, (int)(sizeof(targets) / sizeof(targets[0])), 0.0, &worst));
        /* Seeking to the very end leaves nothing to read. */
        {
            fa_frames got = 1;
            float tmp[16];
            CHECK_EQ(fa_stream_seek(s, n), FA_OK);
            CHECK_EQ(fa_stream_read(s, tmp, 4, &got), FA_ERROR_END_OF_STREAM);
            CHECK_EQ(got, 0);
        }
        free(full);
        fa_stream_close(s);
    }
}

static void test_m4a_aac(void)
{
    static const int64_t targets[] = { 0, 1023, 1024, 1025, 20000, 47000, 5 };
    fa_stream* s;
    fa_stream_info info;
    float* full;
    int64_t n;
    double worst;
    fa_result r = open_file("aac.m4a", -1, &s);
    SKIP_NO_FFMPEG(r);
    CHECK_EQ(r, FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK(strcmp(info.codec, "aac") == 0);
    CHECK(strcmp(info.container, "mp4") == 0);
    CHECK_EQ(info.format.sample_rate, 48000);
    CHECK_EQ(info.format.channels, 2);
    CHECK_EQ(info.length_frames, 48000);       /* the edit list drops the 1024 priming frames */
    CHECK_EQ(info.audio_track_count, 1);
    full = read_all(s, 2, &n);
    CHECK_EQ(n, 48000);
    CHECK_NEAR(freq(full, 4800, 43200, 2, 48000), 1000.0, 3.0);
    CHECK_NEAR(rms(full, 4800, 43200, 2), 0.3536, 0.05);
    /* The first frames are the start of the sine, not priming silence. */
    CHECK(fabs(full[2 * 12]) > 0.05);
    CHECK(seek_matches(s, full, n, 2, targets, (int)(sizeof(targets) / sizeof(targets[0])), 1e-3, &worst));
    printf("  AAC seek vs sequential: max difference %.2g\n", worst);
    free(full);
    fa_stream_close(s);
}

static void test_mov_pcm(void)
{
    static const int64_t targets[] = { 0, 1, 4095, 4096, 4097, 5999, 3000 };
    fa_stream* s;
    fa_stream_info info;
    float* full;
    int64_t n, i;
    double worst = 0;
    CHECK_EQ(open_file("pcm.mov", -1, &s), FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK(strcmp(info.codec, "pcm_s24le") == 0);
    CHECK(strcmp(info.container, "mov") == 0);
    CHECK_EQ(info.format.sample_rate, 8000);
    CHECK_EQ(info.format.channels, 1);
    CHECK_EQ(info.length_frames, 6000);
    full = read_all(s, 1, &n);
    CHECK_EQ(n, 6000);
    for (i = 0; i < n; i++) {
        double want = 0.5 * sin(2 * 3.14159265358979 * 440.0 * (double)i / 8000.0);
        double d = fabs(full[i] - want);
        if (d > worst) worst = d;
    }
    CHECK(worst < 1e-5);
    CHECK(seek_matches(s, full, n, 1, targets, (int)(sizeof(targets) / sizeof(targets[0])), 0.0, &worst));
    free(full);
    fa_stream_close(s);
}

static void test_fragmented_mp4(void)
{
    static const int64_t targets[] = { 0, 11024, 11025, 22050, 33333, 1 };
    fa_stream* s;
    fa_stream_info info;
    float* full;
    int64_t n;
    double worst;
    CHECK_EQ(open_file("frag.mp4", -1, &s), FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK(strcmp(info.codec, "mp3") == 0);
    CHECK_EQ(info.format.sample_rate, 22050);
    CHECK_EQ(info.seekable, 1);
    full = read_all(s, 1, &n);
    CHECK_NEAR((double)n, 44100.0, 1200.0);
    CHECK_NEAR(freq(full, n / 4, n * 3 / 4, 1, 22050), 440.0, 2.0);
    CHECK_NEAR(rms(full, n / 4, n * 3 / 4, 1), 0.3536, 0.03);
    CHECK(seek_matches(s, full, n, 1, targets, (int)(sizeof(targets) / sizeof(targets[0])), 0.0, &worst));
    free(full);
    fa_stream_close(s);
}

/* ---- Matroska --------------------------------------------------------------------- */

static void test_mkv_tracks_and_seek(void)
{
    static const int64_t pcm_targets[] = { 0, 1, 1023, 1024, 1025, 5000, 9999, 7777, 3 };
    static const int64_t mp3_targets[] = { 0, 1, 1151, 1152, 20000, 31000, 10000, 2 };
    fa_stream* s;
    fa_stream_info info;
    float* full;
    int64_t n, i;
    double worst = 0;

    CHECK_EQ(open_file("av_tracks.mkv", -1, &s), FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK(strcmp(info.codec, "pcm_s16le") == 0);
    CHECK(strcmp(info.container, "matroska") == 0);
    CHECK_EQ(info.format.sample_rate, 8000);
    CHECK_EQ(info.format.channels, 1);
    CHECK_EQ(info.audio_track_count, 2);
    CHECK_EQ(info.seekable, 1);
    /* From the last block (the segment's Duration covers the 2 s video too). */
    CHECK_EQ(info.length_frames, 10000);
    full = read_all(s, 1, &n);
    CHECK_EQ(n, 10000);
    for (i = 0; i < n; i++) {
        double d = fabs(full[i] - 0.5 * sin(2 * 3.14159265358979 * 440.0 * (double)i / 8000.0));
        if (d > worst) worst = d;
    }
    CHECK(worst < 1.0 / 16384);
    CHECK(seek_matches(s, full, n, 1, pcm_targets, (int)(sizeof(pcm_targets) / sizeof(pcm_targets[0])), 0.0, &worst));
    free(full);
    fa_stream_close(s);

    CHECK_EQ(open_file("av_tracks.mkv", 1, &s), FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK(strcmp(info.codec, "mp3") == 0);
    CHECK_EQ(info.format.sample_rate, 16000);
    CHECK_EQ(info.format.channels, 2);
    CHECK_EQ(info.length_frames, 32000);
    full = read_all(s, 2, &n);
    CHECK_EQ(n, 32000);                    /* CodecDelay and DiscardPadding removed */
    CHECK_NEAR(freq(full, n / 4, n * 3 / 4, 2, 16000), 1000.0, 3.0);
    CHECK_NEAR(rms(full, n / 4, n * 3 / 4, 2), 0.1768, 0.02);
    CHECK(seek_matches(s, full, n, 2, mp3_targets, (int)(sizeof(mp3_targets) / sizeof(mp3_targets[0])), 0.0, &worst));
    free(full);
    fa_stream_close(s);

    CHECK_EQ(open_file("av_tracks.mkv", 2, &s), FA_ERROR_NOT_FOUND);
    CHECK_EQ(open_file("av_tracks.mkv", 0, &s), FA_OK);
    fa_stream_close(s);
}

static void test_webm_opus(void)
{
    static const int64_t targets[] = { 0, 959, 960, 961, 30000, 90000, 48000, 7 };
    fa_stream* s;
    fa_stream_info info;
    float* full;
    int64_t n;
    double worst;
    fa_result r = open_file("opus.webm", -1, &s);
    SKIP_NO_FFMPEG(r);
    CHECK_EQ(r, FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK(strcmp(info.codec, "opus") == 0);
    CHECK(strcmp(info.container, "webm") == 0);
    CHECK_EQ(info.format.sample_rate, 48000);
    CHECK_EQ(info.format.channels, 2);
    CHECK_EQ(info.length_frames, 96000);
    full = read_all(s, 2, &n);
    CHECK_EQ(n, 96000);                    /* pre-skip and end padding removed */
    CHECK_NEAR(freq(full, 9600, 86400, 2, 48000), 1000.0, 3.0);
    CHECK_NEAR(rms(full, 9600, 86400, 2), 0.3536, 0.06);
    CHECK(seek_matches(s, full, n, 2, targets, (int)(sizeof(targets) / sizeof(targets[0])), 0.05, &worst));
    printf("  Opus seek vs sequential: max difference %.2g\n", worst);
    free(full);
    fa_stream_close(s);
}

/* ---- forward-only sources ---------------------------------------------------------- */

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

/* Decodes `name` from a forward-only source; NULL if it cannot be opened (*r). */
static float* forward_only(const char* name, int32_t track, uint32_t ch, int64_t* n, fa_result* r, int* seekable)
{
    size_t size = 0;
    uint8_t* data = load_file(name, &size);
    mem_io m;
    fa_io_callbacks io = { io_read, NULL, NULL, io_close };
    fa_open_options o;
    fa_stream* s = NULL;
    fa_stream_info info;
    float* out;
    m.p = data;
    m.n = size;
    m.pos = 0;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    o.audio_track = track;
    *r = fa_stream_open_io(g_engine, &io, &m, &o, &s);
    if (*r != FA_OK) { free(data); return NULL; }
    fa_stream_get_info(s, &info);
    *seekable = info.seekable;
    out = read_all(s, ch, n);
    if (fa_stream_seek(s, 0) != FA_ERROR_NOT_SEEKABLE) *seekable = 2;
    fa_stream_close(s);
    free(data);
    return out;
}

static void compare_forward_only(const char* name, int32_t track, uint32_t ch)
{
    fa_stream* s;
    float *a, *b;
    int64_t na, nb, i;
    fa_result r;
    int seekable = -1;
    CHECK_EQ(open_file(name, track, &s), FA_OK);
    a = read_all(s, ch, &na);
    fa_stream_close(s);
    b = forward_only(name, track, ch, &nb, &r, &seekable);
    CHECK_EQ(r, FA_OK);
    CHECK_EQ(seekable, 0);
    CHECK_EQ(nb, na);
    for (i = 0; i < na * (int64_t)ch; i++) CHECK_NEAR(b[i], a[i], 0.0);
    free(a);
    free(b);
}

static void test_forward_only_sources(void)
{
    int64_t n;
    fa_result r;
    int seekable;
    float* p;
    compare_forward_only("av_tracks.mp4", 1, 2);   /* moov in front (+faststart) */
    compare_forward_only("frag.mp4", -1, 1);       /* fragments read as they come */
    compare_forward_only("av_tracks.mkv", 0, 1);
    compare_forward_only("av_tracks.mkv", 1, 2);
    if (g_failures) return;
    /* The movie box after the media data cannot be reached without seeking. */
    p = forward_only("aac.m4a", -1, 2, &n, &r, &seekable);
    CHECK(p == NULL);
    CHECK_EQ(r, FA_ERROR_NOT_SEEKABLE);
}

/* ---- AAC in ADTS frames ------------------------------------------------------------- */

/* net/tone.aac: 3 s of a 440 Hz sine, 44.1 kHz stereo, 131 frames. RMS 0.0625 per
 * channel (amplitude 1/8, less 3 dB from FFmpeg's mono to stereo upmix). */
#define ADTS_FRAMES 131
static void test_adts_file(void)
{
    static const int64_t targets[] = { 1023, 1024, 1025, 50000, 120000, 7 };
    static const int64_t start[] = { 0 };
    fa_stream* s;
    fa_stream_info info;
    float* full;
    int64_t n;
    double worst;
    fa_result r = open_file("net/tone.aac", -1, &s);
    SKIP_NO_FFMPEG(r);
    CHECK_EQ(r, FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK(strcmp(info.codec, "aac") == 0);
    CHECK(strcmp(info.container, "adts") == 0);
    CHECK_EQ(info.format.sample_rate, 44100);
    CHECK_EQ(info.format.channels, 2);
    CHECK_EQ(info.length_frames, ADTS_FRAMES * 1024);   /* every frame, from the index */
    CHECK_EQ(info.seekable, 1);
    full = read_all(s, 2, &n);
    CHECK_EQ(n, ADTS_FRAMES * 1024);
    CHECK_NEAR(freq(full, 4410, 128000, 2, 44100), 440.0, 2.0);
    CHECK_NEAR(rms(full, 4410, 128000, 2), 0.0625, 0.01);
    CHECK(seek_matches(s, full, n, 2, targets, (int)(sizeof(targets) / sizeof(targets[0])), 1e-3, &worst));
    printf("  ADTS seek vs sequential: max difference %.2g\n", worst);
    /* Back to the very start there is no earlier frame to decode first, and FFmpeg's
     * flush keeps the last frame's window shape: the first frame differs a little
     * (an MP4 edit list hides this in the priming frames). */
    CHECK(seek_matches(s, full, n, 2, start, 1, 5e-3, &worst));
    free(full);
    fa_stream_close(s);
    compare_forward_only("net/tone.aac", -1, 2);
}

/* Builds net/tone.aac with an ID3 tag in front, half a frame of junk (a stream
 * joined mid-frame), and an ID3 tag between frames 40 and 41. */
static uint8_t* adts_with_junk(size_t* out_n)
{
    static const uint8_t tag[20] = { 'I', 'D', '3', 4, 0, 0, 0, 0, 0, 10, 'T', 'I', 'T', '2', 0, 0, 0, 0, 0, 0 };
    size_t size = 0, i = 0, n = 0, f = 0, len;
    uint8_t* data = load_file("net/tone.aac", &size);
    uint8_t* out;
    if (!data) return NULL;
    out = (uint8_t*)malloc(size + 2 * sizeof(tag) + 200);
    memcpy(out, tag, sizeof(tag)); n = sizeof(tag);
    len = ((size_t)(data[3] & 3) << 11) | ((size_t)data[4] << 3) | (data[5] >> 5);
    memcpy(out + n, data + len / 2, len - len / 2); n += len - len / 2;   /* the end of frame 0 */
    for (i = 0; i < size; i += len, f++) {
        len = ((size_t)(data[i + 3] & 3) << 11) | ((size_t)data[i + 4] << 3) | (data[i + 5] >> 5);
        if (f == 41) { memcpy(out + n, tag, sizeof(tag)); n += sizeof(tag); }
        memcpy(out + n, data + i, len);
        n += len;
    }
    free(data);
    *out_n = n;
    return out;
}

static void test_adts_junk_and_tags(void)
{
    size_t size = 0;
    uint8_t* data = adts_with_junk(&size);
    fa_open_options o;
    fa_stream* s = NULL;
    fa_stream_info info;
    float *a, *b;
    int64_t na, nb;
    mem_io m;
    fa_io_callbacks io = { io_read, NULL, NULL, io_close };
    fa_result r;
    CHECK(data != NULL);
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    r = fa_stream_open_memory(g_engine, data, size, &o, &s);
    SKIP_NO_FFMPEG(r);
    CHECK_EQ(r, FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK(strcmp(info.container, "adts") == 0);
    CHECK_EQ(info.length_frames, ADTS_FRAMES * 1024);   /* the tag between frames is skipped */
    a = read_all(s, 2, &na);
    CHECK_EQ(na, ADTS_FRAMES * 1024);
    fa_stream_close(s);
    /* The same from a forward-only source: found by sliding to the next header. */
    m.p = data;
    m.n = size;
    m.pos = 0;
    CHECK_EQ(fa_stream_open_io(g_engine, &io, &m, &o, &s), FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK_EQ(info.seekable, 0);
    b = read_all(s, 2, &nb);
    CHECK_EQ(nb, na);
    CHECK(nb == na && memcmp(a, b, (size_t)na * 2 * sizeof(float)) == 0);
    fa_stream_close(s);
    free(a);
    free(b);
    free(data);
}

/* Junk between frames of a local file, then a change of configuration (net/tone.aac
 * at 44.1 kHz followed by tone.aac at 48 kHz, as at an HLS discontinuity): nothing
 * is lost, and the 48 kHz part is converted to the stream's rate. */
static void test_adts_junk_and_format_change(void)
{
    size_t na_ = 0, nb_ = 0, n = 0, i = 0, f = 0, len;
    uint8_t* a = load_file("net/tone.aac", &na_);
    uint8_t* b = load_file("tone.aac", &nb_);
    uint8_t* buf;
    uint32_t seed = 777;
    fa_open_options o;
    fa_stream* s = NULL;
    fa_stream_info info;
    float *x, *y;
    int64_t nx, ny, expect = ADTS_FRAMES * 1024 + 25 * 940;   /* 1024 frames at 48 kHz last 940 at 44.1 */
    mem_io m;
    fa_io_callbacks io = { io_read, NULL, NULL, io_close };
    fa_result r;
    CHECK(a && b);
    if (!a || !b) { free(a); free(b); return; }
    buf = (uint8_t*)malloc(na_ + nb_ + 300);
    for (i = 0; i < na_; i += len, f++) {
        len = ((size_t)(a[i + 3] & 3) << 11) | ((size_t)a[i + 4] << 3) | (a[i + 5] >> 5);
        if (f == 60) {
            size_t k;
            for (k = 0; k < 300; k++) { seed = seed * 1103515245u + 12345u; buf[n++] = (uint8_t)((seed >> 16) % 255); }
        }
        memcpy(buf + n, a + i, len);
        n += len;
    }
    memcpy(buf + n, b, nb_);
    n += nb_;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    r = fa_stream_open_memory(g_engine, buf, n, &o, &s);
    if (r == FA_ERROR_PROVIDER_MISSING) { printf("  (skipped: FFmpeg not found)\n"); free(a); free(b); free(buf); return; }
    CHECK_EQ(r, FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK_EQ(info.format.sample_rate, 44100);
    CHECK_EQ(info.length_frames, expect);
    x = read_all(s, 2, &nx);
    printf("  %lld frames decoded, length %lld\n", (long long)nx, (long long)info.length_frames);
    CHECK(llabs((long long)(nx - expect)) < 64);
    /* The 48 kHz part is the 1 kHz tone of tone.aac. */
    CHECK_NEAR(freq(x, expect - 15000, expect - 2000, 2, 44100), 1000.0, 5.0);
    fa_stream_close(s);
    m.p = buf;
    m.n = n;
    m.pos = 0;
    CHECK_EQ(fa_stream_open_io(g_engine, &io, &m, &o, &s), FA_OK);
    y = read_all(s, 2, &ny);
    CHECK(llabs((long long)(ny - nx)) < 64);
    fa_stream_close(s);
    free(x);
    free(y);
    free(a);
    free(b);
    free(buf);
}

/* ---- a hand-made Matroska file: lacing, unknown sizes, block groups ----------------- */

typedef struct ebuf { uint8_t* p; size_t n, cap; } ebuf;

static void put(ebuf* b, const void* d, size_t n)
{
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 256;
        b->p = (uint8_t*)realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}

static void put_id(ebuf* b, uint32_t id)
{
    uint8_t t[4];
    int n = id > 0xFFFFFF ? 4 : id > 0xFFFF ? 3 : id > 0xFF ? 2 : 1, i;
    for (i = 0; i < n; i++) t[i] = (uint8_t)(id >> (8 * (n - 1 - i)));
    put(b, t, (size_t)n);
}

/* An element with an 8-byte size (or unknown size, for size < 0). */
static void put_el(ebuf* b, uint32_t id, const void* d, int64_t size)
{
    uint8_t t[8];
    int i;
    put_id(b, id);
    t[0] = 0x01;
    for (i = 1; i < 8; i++) t[i] = size < 0 ? 0xFF : (uint8_t)((uint64_t)size >> (8 * (7 - i)));
    put(b, t, 8);
    if (d && size > 0) put(b, d, (size_t)size);
}

static void put_uint(ebuf* b, uint32_t id, uint64_t v)
{
    uint8_t t[8];
    int i;
    for (i = 0; i < 8; i++) t[i] = (uint8_t)(v >> (8 * (7 - i)));
    put_el(b, id, t, 8);
}

static int16_t synth(int64_t k) { return (int16_t)((k * 37) % 20000 - 10000); }

/* A block of track `track` at `ts` ms in its cluster, holding the given frames of
 * PCM (`counts` samples each) with `lacing` (0 none, 1 Xiph, 2 fixed, 3 EBML). */
static void put_block(ebuf* out, uint32_t id, int track, int ts, int lacing, const int* counts, int frames, int64_t* k)
{
    ebuf b = { 0 };
    uint8_t h[4];
    int i;
    h[0] = (uint8_t)(0x80 | track);
    h[1] = (uint8_t)(ts >> 8);
    h[2] = (uint8_t)ts;
    h[3] = (uint8_t)(0x80 | (lacing << 1));
    put(&b, h, 4);
    if (lacing) {
        uint8_t c = (uint8_t)(frames - 1);
        put(&b, &c, 1);
        for (i = 0; i < frames - 1; i++) {
            size_t sz = (size_t)counts[i] * 2;
            if (lacing == 1) {
                while (sz >= 255) { uint8_t f = 255; put(&b, &f, 1); sz -= 255; }
                c = (uint8_t)sz;
                put(&b, &c, 1);
            } else if (lacing == 3) {
                /* Two-byte EBML sizes: the first plain, the rest as signed differences. */
                int64_t v = i == 0 ? (int64_t)sz : (int64_t)sz - (int64_t)counts[i - 1] * 2 + 8191;
                uint8_t e[2];
                e[0] = (uint8_t)(0x40 | (v >> 8));
                e[1] = (uint8_t)v;
                put(&b, e, 2);
            }
        }
    }
    for (i = 0; i < frames; i++) {
        int j;
        for (j = 0; j < counts[i]; j++) {
            int16_t v = synth((*k)++);
            uint8_t le[2];
            le[0] = (uint8_t)v;
            le[1] = (uint8_t)((uint16_t)v >> 8);
            put(&b, le, 2);
        }
    }
    put_el(out, id, b.p, (int64_t)b.n);
    free(b.p);
}

/* 8 kHz mono PCM; every block starts on a whole millisecond (8 samples). */
static ebuf make_laced_mkv(int64_t* total)
{
    static const int one[1] = { 96 }, xiph[3] = { 160, 80, 40 }, ebml[3] = { 40, 120, 64 }, fixed[2] = { 80, 80 };
    static const int video[1] = { 25 };
    ebuf f = { 0 }, hdr = { 0 }, info = { 0 }, tracks = { 0 }, te = { 0 }, audio = { 0 };
    int64_t k = 0, junk = 0;
    int c, ts;
    union { double d; uint64_t u; } rate;
    put_el(&hdr, 0x4282, "matroska", 8);
    put_el(&f, 0x1A45DFA3, hdr.p, (int64_t)hdr.n);
    put_el(&f, 0x18538067, NULL, -1);                         /* live: unknown-size segment */
    put_uint(&info, 0x2AD7B1, 1000000);
    put_el(&f, 0x1549A966, info.p, (int64_t)info.n);
    rate.d = 8000.0;
    put_uint(&audio, 0xB5, rate.u);
    put_uint(&audio, 0x9F, 1);
    put_uint(&audio, 0x6264, 16);
    put_uint(&te, 0xD7, 2);                                   /* a video track first, numbered 2 */
    put_uint(&te, 0x83, 1);
    put_el(&te, 0x86, "V_TEST", 6);
    put_el(&tracks, 0xAE, te.p, (int64_t)te.n);
    te.n = 0;
    put_uint(&te, 0xD7, 1);
    put_uint(&te, 0x83, 2);
    put_el(&te, 0x86, "A_PCM/INT/LIT", 13);
    put_el(&te, 0xE1, audio.p, (int64_t)audio.n);
    put_el(&tracks, 0xAE, te.p, (int64_t)te.n);
    put_el(&f, 0x1654AE6B, tracks.p, (int64_t)tracks.n);
    for (c = 0, ts = 0; c < 3; c++) {
        int64_t start = k;
        ebuf bg = { 0 };
        put_el(&f, 0x1F43B675, NULL, -1);                     /* unknown-size cluster */
        put_uint(&f, 0xE7, (uint64_t)(start / 8));
        ts = 0;
        put_block(&f, 0xA3, 1, ts, 0, one, 1, &k);
        put_block(&f, 0xA3, 2, ts, 0, video, 1, &junk);       /* skipped */
        ts = (int)((k - start) / 8);
        put_block(&f, 0xA3, 1, ts, 1, xiph, 3, &k);
        ts = (int)((k - start) / 8);
        put_block(&f, 0xA3, 1, ts, 3, ebml, 3, &k);
        ts = (int)((k - start) / 8);
        put_block(&f, 0xA3, 1, ts, 2, fixed, 2, &k);
        ts = (int)((k - start) / 8);
        put_block(&bg, 0xA1, 1, ts, 0, one, 1, &k);           /* in a BlockGroup */
        put_uint(&bg, 0x9B, 12);
        put_el(&f, 0xA0, bg.p, (int64_t)bg.n);
        free(bg.p);
    }
    free(hdr.p); free(info.p); free(tracks.p); free(te.p); free(audio.p);
    *total = k;
    return f;
}

static void test_mkv_lacing_and_live_layout(void)
{
    static const int64_t targets[] = { 0, 96, 100, 256, 600, 1300, 2000, 5 };
    int64_t total, n, i;
    ebuf f = make_laced_mkv(&total);
    fa_open_options o;
    fa_stream* s = NULL;
    fa_stream_info info;
    float* full;
    double worst;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    CHECK_EQ(fa_stream_open_memory(g_engine, f.p, f.n, &o, &s), FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK(strcmp(info.codec, "pcm_s16le") == 0);
    CHECK_EQ(info.audio_track_count, 1);
    CHECK_EQ(info.format.sample_rate, 8000);
    full = read_all(s, 1, &n);
    CHECK_EQ(n, total);
    for (i = 0; i < n; i++) CHECK_NEAR(full[i], synth(i) / 32768.0, 1e-7);
    /* No Cues: seeks use the clusters seen so far, or scan for them. */
    CHECK(seek_matches(s, full, n, 1, targets, (int)(sizeof(targets) / sizeof(targets[0])), 0.0, &worst));
    fa_stream_close(s);
    /* Forward-only: the same frames. */
    {
        mem_io m;
        fa_io_callbacks io = { io_read, NULL, NULL, io_close };
        float* b;
        int64_t nb;
        m.p = f.p;
        m.n = f.n;
        m.pos = 0;
        CHECK_EQ(fa_stream_open_io(g_engine, &io, &m, &o, &s), FA_OK);
        b = read_all(s, 1, &nb);
        fa_stream_close(s);
        CHECK_EQ(nb, n);
        CHECK(memcmp(b, full, (size_t)n * sizeof(float)) == 0);
        free(b);
    }
    free(full);
    free(f.p);
}

/* ---- damaged files ------------------------------------------------------------------- */

static void exercise_memory(const uint8_t* data, size_t size, int32_t track)
{
    fa_open_options o;
    fa_stream* s = NULL;
    fa_stream_info info;
    float buf[4096 * 8];
    int k;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    o.audio_track = track;
    if (fa_stream_open_memory(g_engine, data, size, &o, &s) != FA_OK) return;
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
    static const char* names[] = { "av_tracks.mp4", "aac.m4a", "pcm.mov", "frag.mp4", "av_tracks.mkv", "opus.webm", "net/tone.aac" };
    size_t f;
    uint32_t seed = 12345;
    for (f = 0; f < sizeof(names) / sizeof(names[0]); f++) {
        size_t size = 0, cut, i;
        uint8_t* data = load_file(names[f], &size);
        uint8_t* bad;
        CHECK(data != NULL);
        bad = (uint8_t*)malloc(size);
        /* Cut short anywhere. */
        for (cut = 1; cut < size; cut += size / 23 + 1) {
            exercise_memory(data, cut, -1);
            exercise_memory(data, cut, 1);
        }
        /* Random bytes changed (only where the built-in codecs decode: FFmpeg would
         * log every corrupt packet). */
        for (i = 0; i < 40 && strcmp(names[f], "aac.m4a") != 0 && strcmp(names[f], "opus.webm") != 0 &&
                         strcmp(names[f], "net/tone.aac") != 0; i++) {
            size_t j, flips = 1 + i % 8;
            memcpy(bad, data, size);
            for (j = 0; j < flips; j++) {
                seed = seed * 1103515245u + 12345u;
                bad[(seed >> 8) % size] = (uint8_t)(seed >> 3);
            }
            exercise_memory(bad, size, (int32_t)(i % 3) - 1);
        }
        free(bad);
        free(data);
    }
}

/* ---- loudness scan of a video file ------------------------------------------------- */

static void test_scan_video_file(void)
{
    char path[512];
    fa_loudness l;
    data_path(path, sizeof(path), "av_tracks.mp4");
    CHECK_EQ(fa_scan_loudness_path(g_engine, path, &l), FA_OK);
    /* A 0.5 sine at 440 Hz, mono: about -9 LUFS. */
    printf("  av_tracks.mp4: %.2f LUFS, peak %.2f dBTP\n", l.integrated_lufs, l.true_peak_dbtp);
    CHECK(l.integrated_lufs > -14.0 && l.integrated_lufs < -4.0);
    CHECK(l.true_peak_dbtp > -9.0 && l.true_peak_dbtp < 0.5);
    data_path(path, sizeof(path), "av_tracks.mkv");
    CHECK_EQ(fa_scan_loudness_path(g_engine, path, &l), FA_OK);
    printf("  av_tracks.mkv: %.2f LUFS\n", l.integrated_lufs);
    CHECK(l.integrated_lufs > -14.0 && l.integrated_lufs < -4.0);
}

static void test_registry_lists_containers(void)
{
    fa_format_info fi;
    uint32_t i, found = 0;
    for (i = 0; i < fa_format_count(); i++) {
        CHECK_EQ(fa_format_get_info(i, &fi), FA_OK);
        if (strcmp(fi.name, "mp4") == 0 || strcmp(fi.name, "matroska") == 0 || strcmp(fi.name, "aac") == 0) found++;
    }
    CHECK_EQ(found, 3);
}

/* AAC configs with explicit (non-table) rates, which a Matroska track without
 * CodecPrivate can ask for: 73 bits with SBR, no write past the buffer. */
static void test_aac_config_explicit_rates(void)
{
    uint8_t asc[FA_AAC_ASC_MAX + 4];
    memset(asc, 0xAB, sizeof(asc));
    CHECK(fa_aac_build_asc(asc, 2, 12345, 2, 24690) == 10);
    CHECK(asc[FA_AAC_ASC_MAX] == 0xAB);                 /* nothing written past the limit */
    CHECK((asc[0] >> 3) == 5);                          /* explicit SBR */
    CHECK(fa_aac_build_asc(asc, 2, 12345, 2, 0) == 5);  /* 5 + 28 + 4 + 3 = 40 bits */
    CHECK(fa_aac_build_asc(asc, 2, 44100, 2, 0) == 2);
    /* channelConfiguration (bits 9-12): 7.1 is 7, counts without one are 0, never stereo. */
    fa_aac_build_asc(asc, 2, 48000, 6, 0);
    CHECK(((asc[1] >> 3) & 15) == 6);
    fa_aac_build_asc(asc, 2, 48000, 8, 0);
    CHECK(((asc[1] >> 3) & 15) == 7);
    fa_aac_build_asc(asc, 2, 48000, 7, 0);
    CHECK(((asc[1] >> 3) & 15) == 0);
}

int main(void)
{
    fa_engine_config cfg;
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    cfg.worker_threads = 2;
    if (fa_engine_create(&cfg, &g_engine) != FA_OK) { fprintf(stderr, "engine create failed\n"); return 1; }
    printf("FFmpeg %s\n", fa_ffmpeg_load(NULL) == FA_OK ? "found" : "not found: AAC and Opus tracks are skipped");

    RUN(test_aac_config_explicit_rates);
    RUN(test_registry_lists_containers);
    RUN(test_mp4_info_and_tracks);
    RUN(test_mp4_mp3_seek_is_exact);
    RUN(test_m4a_aac);
    RUN(test_mov_pcm);
    RUN(test_fragmented_mp4);
    RUN(test_mkv_tracks_and_seek);
    RUN(test_webm_opus);
    RUN(test_forward_only_sources);
    RUN(test_adts_file);
    RUN(test_adts_junk_and_tags);
    RUN(test_adts_junk_and_format_change);
    RUN(test_mkv_lacing_and_live_layout);
    RUN(test_damaged_files_do_not_crash);
    RUN(test_scan_video_file);

    fa_engine_destroy(g_engine);
    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    printf("PASS\n");
    return 0;
}
