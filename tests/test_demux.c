/* White-box tests of the MP4 / MOV and Matroska / WebM demuxers, on the small
 * files in tests/data (make_demux_data.sh). Every audio track is a sine of its own
 * frequency and level, so the tests can tell which track came out. Every codec
 * here is built in, so none of them needs FFmpeg. */
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

static fa_engine* g_engine;

/* AAC goes to the system's decoders on Windows, macOS, iOS and Android
 * (codec_mf.c, codec_at.c, codec_mediacodec.c), to FFmpeg elsewhere. Checks of
 * AAC audio hold for any decoder (tones, levels, lengths, positions); a seek
 * matches the full decode to the bit only where FFmpeg is known to be the one. */
#if defined(_WIN32) || defined(__APPLE__) || defined(__ANDROID__)
#  define AAC_SEEK_TOL 1e-2
#  define AAC_START_TOL 5e-2
#else
#  define AAC_SEEK_TOL 1e-3
#  define AAC_START_TOL 5e-3
#endif

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
    CHECK(seek_matches(s, full, n, 2, targets, (int)(sizeof(targets) / sizeof(targets[0])), AAC_SEEK_TOL, &worst));
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
    CHECK_EQ(r, FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK(strcmp(info.codec, "aac") == 0);
    CHECK(strcmp(info.container, "adts") == 0);
    CHECK_EQ(info.format.sample_rate, 44100);
    CHECK_EQ(info.format.channels, 2);
    CHECK_EQ(info.length_frames, (ADTS_FRAMES - 1) * 1024);   /* every frame but the priming, from the index */
    CHECK_EQ(info.seekable, 1);
    full = read_all(s, 2, &n);
    CHECK_EQ(n, (ADTS_FRAMES - 1) * 1024);
    CHECK_NEAR(freq(full, 4410, 128000, 2, 44100), 440.0, 2.0);
    CHECK_NEAR(rms(full, 4410, 128000, 2), 0.0625, 0.01);
    CHECK(seek_matches(s, full, n, 2, targets, (int)(sizeof(targets) / sizeof(targets[0])), AAC_SEEK_TOL, &worst));
    printf("  ADTS seek vs sequential: max difference %.2g\n", worst);
    /* Back to the very start there is no earlier frame to decode first, and FFmpeg's
     * flush keeps the last frame's window shape: the first frame differs a little
     * (an MP4 edit list hides this in the priming frames). */
    CHECK(seek_matches(s, full, n, 2, start, 1, AAC_START_TOL, &worst));
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
    CHECK_EQ(r, FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK(strcmp(info.container, "adts") == 0);
    CHECK_EQ(info.length_frames, (ADTS_FRAMES - 1) * 1024);   /* the tag between frames is skipped */
    a = read_all(s, 2, &na);
    CHECK_EQ(na, (ADTS_FRAMES - 1) * 1024);
    {
        /* The index keeps every 16th frame and the frame after the tag (frame 41);
         * seeks hop from header to header, and land exactly on either side of it. */
        static const int64_t targets[] = { 39 * 1024 + 5, 40 * 1024 - 1, 40 * 1024, 40 * 1024 + 700, 46 * 1024 + 3, 47 * 1024 + 1000, 100, 20000 };
        double worst;
        CHECK(seek_matches(s, a, na, 2, targets, (int)(sizeof(targets) / sizeof(targets[0])), AAC_SEEK_TOL, &worst));
    }
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
    int64_t nx, ny, expect = (ADTS_FRAMES - 1) * 1024 + 25 * 940;   /* 1024 frames at 48 kHz last 940 at 44.1;
                                                                     the priming is dropped once, at the start */
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

/* Encrypted tracks are refused as protected content, not as an unknown format:
 * MP4 Common Encryption ('enca'), iTunes FairPlay ('drms') and Matroska
 * ContentEncryption. */
static void test_protected_content(void)
{
    static const char* fourccs[] = { "enca", "drms" };
    size_t size = 0, i, j;
    uint8_t* d = load_file("aac.m4a", &size);
    fa_open_options o;
    fa_stream* s = NULL;
    CHECK(d != NULL);
    fa_open_options_init(&o);
    for (i = 0; i < 2; i++) {
        uint8_t* p = (uint8_t*)malloc(size);
        int done = 0;
        memcpy(p, d, size);
        for (j = 4; j + 12 < size && !done; j++)
            if (memcmp(p + j, "stsd", 4) == 0 && memcmp(p + j + 16, "mp4a", 4) == 0) {
                memcpy(p + j + 16, fourccs[i], 4);
                done = 1;
            }
        CHECK(done);
        CHECK_EQ(fa_stream_open_memory(g_engine, p, size, &o, &s), FA_ERROR_PROTECTED_CONTENT);
        free(p);
    }
    free(d);
    {
        ebuf f = { 0 }, hdr = { 0 }, info = { 0 }, tracks = { 0 }, te = { 0 }, audio = { 0 };
        ebuf encs = { 0 }, enc = { 0 }, crypt = { 0 };
        union { double d; uint64_t u; } rate;
        static const int one[1] = { 96 };
        int64_t k = 0;
        put_el(&hdr, 0x4282, "matroska", 8);
        put_el(&f, 0x1A45DFA3, hdr.p, (int64_t)hdr.n);
        put_el(&f, 0x18538067, NULL, -1);
        put_uint(&info, 0x2AD7B1, 1000000);
        put_el(&f, 0x1549A966, info.p, (int64_t)info.n);
        rate.d = 8000.0;
        put_uint(&audio, 0xB5, rate.u);
        put_uint(&audio, 0x9F, 1);
        put_uint(&audio, 0x6264, 16);
        put_uint(&crypt, 0x47E1, 5);                          /* ContentEncAlgo: AES */
        put_uint(&enc, 0x5033, 1);                            /* ContentEncodingType: encryption */
        put_el(&enc, 0x5035, crypt.p, (int64_t)crypt.n);
        put_el(&encs, 0x6240, enc.p, (int64_t)enc.n);
        put_uint(&te, 0xD7, 1);
        put_uint(&te, 0x83, 2);
        put_el(&te, 0x86, "A_PCM/INT/LIT", 13);
        put_el(&te, 0xE1, audio.p, (int64_t)audio.n);
        put_el(&te, 0x6D80, encs.p, (int64_t)encs.n);
        put_el(&tracks, 0xAE, te.p, (int64_t)te.n);
        put_el(&f, 0x1654AE6B, tracks.p, (int64_t)tracks.n);
        put_el(&f, 0x1F43B675, NULL, -1);
        put_uint(&f, 0xE7, 0);
        put_block(&f, 0xA3, 1, 0, 0, one, 1, &k);
        CHECK_EQ(fa_stream_open_memory(g_engine, f.p, f.n, &o, &s), FA_ERROR_PROTECTED_CONTENT);
        free(f.p); free(hdr.p); free(info.p); free(tracks.p); free(te.p); free(audio.p);
        free(encs.p); free(enc.p); free(crypt.p);
    }
    CHECK(strcmp(fa_result_string(FA_ERROR_PROTECTED_CONTENT), "protected content") == 0);
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
    static const char* names[] = { "av_tracks.mp4", "aac.m4a", "pcm.mov", "frag.mp4", "av_tracks.mkv", "opus.webm", "net/tone.aac",
                                   "alac/alac16.mkv", "alac/alac51_24.m4a" };
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

/* ---- what the systems' AAC and ALAC decoders are given (codec_os.c) --------------- */

static void test_os_codec_helpers(void)
{
    static const uint8_t lc[2] = { 0x12, 0x10 };   /* LC, 44.1 kHz, stereo */
    /* LC 22.05 kHz stereo, then the sync extension: SBR at 44.1 kHz and PS. */
    static const uint8_t ext[7] = { 0x13, 0x90, 0x56, 0xE5, 0xA5, 0x48, 0x80 };
    static const uint8_t ext_off[5] = { 0x13, 0x90, 0x56, 0xE5, 0x00 };   /* SBR signalled absent */
    uint8_t asc[FA_AAC_ASC_MAX + 8], esds[64], adts[64], alac[48];
    fa_aac_asc_info info;
    fa_codec_params p;
    fa_alac_info a;
    const uint8_t* d;
    size_t n, m;
    uint32_t map[8];

    CHECK(fa_aac_parse_asc(lc, 2, &info));
    CHECK_EQ(info.object_type, 2);
    CHECK_EQ(info.rate, 44100);
    CHECK_EQ(info.channel_config, 2);
    CHECK_EQ(info.sbr, -1);
    CHECK_EQ(info.ext_rate, 0);
    n = fa_aac_build_asc(asc, 2, 22050, 2, 44100);
    CHECK(fa_aac_parse_asc(asc, n, &info));
    CHECK_EQ(info.object_type, 2);
    CHECK_EQ(info.rate, 22050);
    CHECK_EQ(info.ext_rate, 44100);
    CHECK_EQ(info.sbr, 1);
    CHECK_EQ(info.ps, 0);
    CHECK(fa_aac_parse_asc(ext, sizeof(ext), &info));
    CHECK_EQ(info.object_type, 2);
    CHECK_EQ(info.rate, 22050);
    CHECK_EQ(info.sbr, 1);
    CHECK_EQ(info.ext_rate, 44100);
    CHECK_EQ(info.ps, 1);
    CHECK(fa_aac_parse_asc(ext_off, sizeof(ext_off), &info));
    CHECK_EQ(info.sbr, 0);
    CHECK(!fa_aac_parse_asc(lc, 1, &info));
    asc[0] = 0x16; asc[1] = 0x90;                    /* rate index 13: reserved */
    CHECK(!fa_aac_parse_asc(asc, 2, &info));
    CHECK_EQ(fa_aac_config_channels(6), 6);
    CHECK_EQ(fa_aac_config_channels(7), 8);

    /* ADTS frames (no extradata): LC from the header's rate and channels, or at low
     * rates HE-AAC (HE-AAC v2 for mono) for decoders that do not find SBR alone. */
    fa_codec_params_init(&p);
    p.codec = "aac";
    p.sample_rate = 44100;
    p.channels = 2;
    n = fa_aac_os_asc(&p, 1, asc, sizeof(asc));
    CHECK(n == 2 && memcmp(asc, lc, 2) == 0);
    p.sample_rate = 24000;
    p.channels = 1;
    n = fa_aac_os_asc(&p, 1, asc, sizeof(asc));
    CHECK(n > 0 && fa_aac_parse_asc(asc, n, &info));
    CHECK_EQ(info.sbr, 1);
    CHECK_EQ(info.ps, 1);
    CHECK_EQ(info.rate, 24000);
    CHECK_EQ(info.ext_rate, 48000);
    CHECK_EQ(info.channel_config, 1);
    n = fa_aac_os_asc(&p, 0, asc, sizeof(asc));
    CHECK(n > 0 && fa_aac_parse_asc(asc, n, &info));
    CHECK_EQ(info.sbr, -1);
    p.extradata = ext;                               /* the stream's own config wins */
    p.extradata_size = sizeof(ext);
    n = fa_aac_os_asc(&p, 1, asc, sizeof(asc));
    CHECK(n == sizeof(ext) && memcmp(asc, ext, n) == 0);
    {
        /* LC at 22.05 kHz stereo, SBR left implicit: signalled when guessing. */
        static const uint8_t lc22[2] = { 0x13, 0x90 };
        p.extradata = lc22;
        p.extradata_size = 2;
        n = fa_aac_os_asc(&p, 1, asc, sizeof(asc));
        CHECK(n > 0 && fa_aac_parse_asc(asc, n, &info));
        CHECK_EQ(info.sbr, 1);
        CHECK_EQ(info.rate, 22050);
        CHECK_EQ(info.ext_rate, 44100);
        CHECK_EQ(info.channel_config, 2);
        n = fa_aac_os_asc(&p, 0, asc, sizeof(asc));
        CHECK(n == 2 && memcmp(asc, lc22, 2) == 0);
    }
    p.extradata = lc;
    p.extradata_size = 1;                            /* broken */
    CHECK_EQ(fa_aac_os_asc(&p, 1, asc, sizeof(asc)), 0);

    /* The ES descriptor around it: tags 3, 4 (MPEG-4 audio), 5 (the config), 6. */
    m = fa_aac_esds(lc, 2, esds, sizeof(esds));
    CHECK_EQ(m, 39);
    CHECK(esds[0] == 0x03 && esds[4] == 34);
    CHECK(esds[8] == 0x04 && esds[12] == 20 && esds[13] == 0x40 && esds[14] == 0x15);
    CHECK(esds[26] == 0x05 && esds[30] == 2 && esds[31] == 0x12 && esds[32] == 0x10);
    CHECK(esds[33] == 0x06 && esds[37] == 1 && esds[38] == 0x02);
    CHECK_EQ(fa_aac_esds(lc, 2, esds, 38), 0);

    /* ADTS headers: 7 bytes, 9 with a CRC; several raw blocks are refused. */
    memset(adts, 0x55, sizeof(adts));
    adts[0] = 0xFF; adts[1] = 0xF1; adts[2] = 0x50; adts[3] = 0x80;
    adts[4] = (uint8_t)(20 >> 3); adts[5] = (uint8_t)((20 & 7) << 5) | 0x1F; adts[6] = 0xFC;
    d = adts; m = 20;
    CHECK_EQ(fa_adts_strip(&d, &m), 1);
    CHECK(d == adts + 7 && m == 13);
    adts[1] = 0xF0;
    d = adts; m = 20;
    CHECK_EQ(fa_adts_strip(&d, &m), 1);
    CHECK(d == adts + 9 && m == 11);
    adts[1] = 0xF1;
    adts[6] = 0xFD;
    d = adts; m = 20;
    CHECK_EQ(fa_adts_strip(&d, &m), -1);
    adts[6] = 0xFC;
    d = adts; m = 21;                                /* the length does not match */
    CHECK_EQ(fa_adts_strip(&d, &m), 0);
    CHECK(d == adts && m == 21);
    d = lc; m = 2;
    CHECK_EQ(fa_adts_strip(&d, &m), 0);

    /* ALAC: the bare config, the 'alac' box, and Core Audio's cookie with 'frma'. */
    memset(alac, 0, sizeof(alac));
    alac[24 + 2] = 0x10;                             /* frameLength 4096 */
    alac[24 + 5] = 16;                               /* bitDepth */
    alac[24 + 9] = 2;                                /* numChannels */
    alac[24 + 22] = 0xAC; alac[24 + 23] = 0x44;      /* sampleRate 44100 */
    CHECK(fa_alac_parse_config(alac + 24, 24, &a));
    CHECK(a.config == alac + 24 && a.frame_length == 4096 && a.bit_depth == 16 && a.channels == 2 && a.rate == 44100);
    alac[12 + 3] = 36;
    memcpy(alac + 12 + 4, "alac", 4);
    CHECK(fa_alac_parse_config(alac + 12, 36, &a));
    CHECK(a.config == alac + 24);
    alac[3] = 12;
    memcpy(alac + 4, "frma", 4);
    memcpy(alac + 8, "alac", 4);
    CHECK(fa_alac_parse_config(alac, 48, &a));
    CHECK(a.config == alac + 24);
    alac[24 + 5] = 15;
    CHECK(!fa_alac_parse_config(alac + 24, 24, &a));
    CHECK(!fa_alac_parse_config(alac + 24, 23, &a));

    /* Core Audio's channel labels to the library's order (WAVE's). */
    {
        static const uint32_t aac51[6] = { 3, 1, 2, 5, 6, 4 };            /* C L R Ls Rs LFE */
        static const uint32_t aac71[8] = { 3, 7, 8, 1, 2, 5, 6, 4 };      /* C Lc Rc L R Ls Rs LFE */
        static const uint32_t mpeg71[8] = { 1, 2, 3, 4, 5, 6, 33, 34 };   /* L R C LFE Ls Rs Rls Rrs */
        static const uint32_t aac61[7] = { 3, 1, 2, 5, 6, 9, 4 };         /* C L R Ls Rs Cs LFE */
        static const uint32_t wave51[6] = { 1, 2, 3, 4, 5, 6 };
        static const uint32_t dup[3] = { 1, 1, 2 };
        static const uint32_t want51[6] = { 1, 2, 0, 5, 3, 4 };
        static const uint32_t want71[8] = { 3, 4, 0, 7, 5, 6, 1, 2 };
        static const uint32_t want_m71[8] = { 0, 1, 2, 3, 6, 7, 4, 5 };
        static const uint32_t want61[7] = { 1, 2, 0, 6, 5, 3, 4 };
        CHECK(fa_os_channel_map(aac51, 6, map) == 1 && memcmp(map, want51, sizeof(want51)) == 0);
        CHECK(fa_os_channel_map(aac71, 8, map) == 1 && memcmp(map, want71, sizeof(want71)) == 0);
        CHECK(fa_os_channel_map(mpeg71, 8, map) == 1 && memcmp(map, want_m71, sizeof(want_m71)) == 0);
        CHECK(fa_os_channel_map(aac61, 7, map) == 1 && memcmp(map, want61, sizeof(want61)) == 0);
        CHECK_EQ(fa_os_channel_map(wave51, 6, map), 0);
        CHECK_EQ(fa_os_channel_map(aac51, 2, map), 0);
        CHECK_EQ(fa_os_channel_map(dup, 3, map), 0);
        CHECK(map[0] == 0 && map[1] == 1 && map[2] == 2);
    }
}

/* ALAC is lossless: the 16-bit sines come back exactly. */
static void test_m4a_alac(void)
{
    static const int64_t targets[] = { 0, 1, 4095, 4096, 4097, 9000, 3 };
    fa_stream* s;
    fa_stream_info info;
    float* full;
    int64_t n, i;
    double worst = 0;
    fa_result r = open_file("alac.m4a", -1, &s);
    CHECK_EQ(r, FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK(strcmp(info.codec, "alac") == 0);
    CHECK_EQ(info.format.sample_rate, 44100);
    CHECK_EQ(info.format.channels, 2);
    CHECK_EQ(info.length_frames, 11025);
    full = read_all(s, 2, &n);
    CHECK_EQ(n, 11025);
    for (i = 0; i < n; i++) {
        double l = 0.5 * sin(2 * 3.14159265358979 * 440.0 * (double)i / 44100.0);
        double rr = 0.25 * sin(2 * 3.14159265358979 * 660.0 * (double)i / 44100.0);
        double d = fabs(full[2 * i] - l);
        if (fabs(full[2 * i + 1] - rr) > d) d = fabs(full[2 * i + 1] - rr);
        if (d > worst) worst = d;
    }
    printf("  largest difference from the sines: %.3g (a 16-bit step is %.3g)\n", worst, 1.0 / 32768);
    CHECK(worst <= 1.01 / 32768);
    CHECK(seek_matches(s, full, n, 2, targets, (int)(sizeof(targets) / sizeof(targets[0])), 0.0, &worst));
    free(full);
    fa_stream_close(s);
}

/* The built-in ALAC decoder against FLAC copies of the same samples (lossless
 * both, so equal to the bit): 5.1 at 24 bits (ALAC's centre-first order put in
 * the library's, noise, silence, a sweep) in MP4, and 16-bit stereo in Matroska;
 * the last frame of each is a partial one. */
static void check_alac(const char* alac, const char* flac, uint32_t channels, uint32_t rate)
{
    static const int64_t targets[] = { 0, 1, 4095, 4096, 4097, 9000, 3, 15000 };
    fa_stream *s, *f;
    fa_stream_info info;
    float *a, *b;
    int64_t na, nb, i;
    double worst = 0;
    CHECK_EQ(open_file(alac, -1, &s), FA_OK);
    CHECK_EQ(open_file(flac, -1, &f), FA_OK);
    if (!s || !f) { fa_stream_close(s); fa_stream_close(f); return; }
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK(strcmp(info.codec, "alac") == 0);
    CHECK_EQ(info.format.sample_rate, rate);
    CHECK_EQ(info.format.channels, channels);
    a = read_all(s, channels, &na);
    b = read_all(f, channels, &nb);
    CHECK_EQ(na, nb);
    CHECK_EQ(info.length_frames, nb);
    for (i = 0; i < na * (int64_t)channels && i < nb * (int64_t)channels; i++) {
        double d = fabs((double)a[i] - (double)b[i]);
        if (d > worst) worst = d;
    }
    printf("  %s: %lld frames, largest difference from FLAC %.3g\n", alac, (long long)na, worst);
    CHECK(worst == 0.0);
    CHECK(seek_matches(s, a, na, channels, targets, (int)(sizeof(targets) / sizeof(targets[0])), 0.0, &worst));
    free(a);
    free(b);
    fa_stream_close(s);
    fa_stream_close(f);
}

static void test_alac_against_flac(void)
{
    check_alac("alac/alac51_24.m4a", "alac/alac51_24.flac", 6, 48000);
    check_alac("alac/alac16.mkv", "alac/alac16.flac", 2, 44100);
}

/* AAC 5.1: each channel's own frequency where the library's order puts it
 * (L R C LFE Ls Rs), whichever decoder and whatever order it decodes in. */
static void test_aac_51_channel_order(void)
{
    static const double hz[6] = { 500, 700, 900, 80, 1300, 1700 };
    fa_stream* s;
    fa_stream_info info;
    float* full;
    int64_t n;
    uint32_t k;
    fa_result r = open_file("aac51.m4a", -1, &s);
    CHECK_EQ(r, FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK(strcmp(info.codec, "aac") == 0);
    CHECK_EQ(info.format.sample_rate, 48000);
    CHECK_EQ(info.format.channels, 6);
    full = read_all(s, 6, &n);
    CHECK_EQ(n, 24000);
    for (k = 0; k < 6; k++) {
        double f = freq(full + k, 4800, 19200, 6, 48000);
        printf("  channel %u: %.1f Hz (want %.0f), RMS %.3f\n", k, f, hz[k], rms(full + k, 4800, 19200, 6));
        CHECK_NEAR(f, hz[k], hz[k] * 0.02);
        CHECK_NEAR(rms(full + k, 4800, 19200, 6), 0.2121, 0.05);
    }
    free(full);
    fa_stream_close(s);
}

/* ---- MP3 files (dec_mp3.c) ---------------------------------------------------------- */

/* Where a 440 Hz sine of phase 0 at sample 0 sits in channel 0 of p: its lag
 * in samples, modulo one period (0 .. rate / 440). */
static double sine_lag(const float* p, int64_t from, int64_t to, uint32_t ch, uint32_t rate)
{
    double a = 0, b = 0, w = 2 * 3.14159265358979 * 440.0 / rate, period = rate / 440.0, lag;
    int64_t i;
    for (i = from; i < to; i++) {
        a += p[i * ch] * sin(w * (double)i);
        b += p[i * ch] * cos(w * (double)i);
    }
    /* p[i] ~ sin(w (i - lag)) = sin(w i) cos(w lag) - cos(w i) sin(w lag) */
    lag = atan2(-b, a) / w;
    while (lag < 0) lag += period;
    while (lag >= period) lag -= period;
    return lag;
}

/* net/tone.aac starts on the first real sample: FFmpeg's encoder primes with 1024
 * samples, which the ADTS reader drops, so the sine has no lag (bass_aac agrees). */
static void test_adts_start(void)
{
    fa_stream* s;
    float* full;
    int64_t n;
    double lag, period = 44100 / 440.0;
    fa_result r = open_file("net/tone.aac", -1, &s);
    CHECK_EQ(r, FA_OK);
    full = read_all(s, 2, &n);
    lag = sine_lag(full, 4410, 44100, 2, 44100);
    printf("  ADTS start lag: %.2f frames\n", lag);
    CHECK(lag < 1.0 || lag > period - 1.0);
    free(full);
    fa_stream_close(s);
}

static fa_result open_mp3(const char* name, uint32_t flags, fa_stream** s)
{
    char path[512];
    fa_open_options o;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY | flags;
    snprintf(path, sizeof(path), "%s/mp3/%s", FA_TEST_DATA_DIR, name);
    return fa_stream_open_path(g_engine, path, &o, s);
}

/* Length, start and exact seeks of one file. `lag`: where the audio starts
 * against the source (the encoder delay the file does not record). */
static void check_mp3(const char* name, uint32_t rate, uint32_t channels, int64_t length, double lag)
{
    static const int64_t targets[] = { 0, 1, 1151, 1152, 1153, 100000, 2000, 2577, 60000, 61000, 62500,
                                       175000, 170000, 30000, 5, 88199, 44100, 44101 };
    int prescan;
    for (prescan = 0; prescan < 2; prescan++) {
        fa_stream* s;
        fa_stream_info info;
        float* full;
        int64_t n;
        double worst, period = rate / 440.0, d;
        CHECK_EQ(open_mp3(name, prescan ? FA_OPEN_PRESCAN : 0, &s), FA_OK);
        CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
        CHECK(strcmp(info.codec, "mp3") == 0);
        CHECK_EQ(info.format.sample_rate, rate);
        CHECK_EQ(info.format.channels, channels);
        CHECK_EQ(info.seekable, 1);
        if (length > 0) CHECK_EQ(info.length_frames, length);
        full = read_all(s, channels, &n);
        CHECK_EQ(n, info.length_frames);
        d = fabs(sine_lag(full, n / 4, n * 3 / 4, channels, rate) - lag);
        if (d > period / 2) d = period - d;
        CHECK(d < 0.5);
        /* Every seek gives exactly the samples of the straight decode. */
        CHECK(seek_matches(s, full, n, channels, targets, (int)(sizeof(targets) / sizeof(targets[0])), 0.0, &worst));
        free(full);
        fa_stream_close(s);
    }
}

/* LAME header: encoder delay and padding trimmed, the exact source length. */
static void test_mp3_cbr(void) { check_mp3("cbr.mp3", 44100, 2, 176400, 0.0); }
static void test_mp3_vbr(void) { check_mp3("vbr.mp3", 44100, 2, 176400, 0.0); }
/* One granule per frame; the reservoir spans several small frames. */
static void test_mp3_mpeg2(void) { check_mp3("mpeg2.mp3", 22050, 1, 88200, 0.0); }

/* No header: only the decoder's 529-sample delay is known, so the audio starts
 * after LAME's 576-sample encoder delay (as in BASS), and the length is counted
 * at open: the frames' samples less those 529. */
static void test_mp3_no_header(void)
{
    double period = 44100 / 440.0;
    check_mp3("noxing.mp3", 44100, 2, 155 * 1152 - 529, fmod(576.0, period));
}

/* Junk between frames is skipped the same way when playing and when seeking. */
static void test_mp3_junk(void)
{
    static const int64_t targets[] = { 0, 90000, 50000, 120000, 1 };
    size_t size, i;
    uint8_t* data = load_file("mp3/cbr.mp3", &size);
    uint8_t* bad;
    fa_open_options o;
    fa_stream* s;
    fa_stream_info info;
    float* full;
    int64_t n;
    double worst;
    CHECK(data != NULL);
    bad = (uint8_t*)malloc(size + 3000);
    memcpy(bad, data, size / 2);
    for (i = 0; i < 3000; i++) bad[size / 2 + i] = (uint8_t)(i * 37 + (i >> 3));
    memcpy(bad + size / 2 + 3000, data + size / 2, size - size / 2);
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    CHECK_EQ(fa_stream_open_memory(g_engine, bad, size + 3000, &o, &s), FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    full = read_all(s, 2, &n);
    CHECK(n > 170000 && n <= 176400);
    CHECK(seek_matches(s, full, n, 2, targets, (int)(sizeof(targets) / sizeof(targets[0])), 0.0, &worst));
    free(full);
    fa_stream_close(s);
    /* Cut in the middle of a frame: plays up to the last whole frame. */
    CHECK_EQ(fa_stream_open_memory(g_engine, data, size / 3 + 77, &o, &s), FA_OK);
    full = read_all(s, 2, &n);
    CHECK(n > 40000 && n < 70000);
    CHECK(seek_matches(s, full, n, 2, targets, (int)(sizeof(targets) / sizeof(targets[0])), 0.0, &worst));
    CHECK_EQ(fa_stream_seek(s, 150000), FA_OK);
    {
        float buf[64];
        fa_frames got = 1;
        fa_stream_read(s, buf, 32, &got);
        CHECK_EQ(got, 0);
    }
    free(full);
    fa_stream_close(s);
    free(bad);
    free(data);
}

int main(void)
{
    fa_engine_config cfg;
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    cfg.worker_threads = 2;
    if (fa_engine_create(&cfg, &g_engine) != FA_OK) { fprintf(stderr, "engine create failed\n"); return 1; }

    RUN(test_aac_config_explicit_rates);
    RUN(test_os_codec_helpers);
    RUN(test_registry_lists_containers);
    RUN(test_mp4_info_and_tracks);
    RUN(test_mp4_mp3_seek_is_exact);
    RUN(test_m4a_aac);
    RUN(test_m4a_alac);
    RUN(test_alac_against_flac);
    RUN(test_aac_51_channel_order);
    RUN(test_mov_pcm);
    RUN(test_fragmented_mp4);
    RUN(test_mkv_tracks_and_seek);
    RUN(test_webm_opus);
    RUN(test_forward_only_sources);
    RUN(test_adts_file);
    RUN(test_adts_start);
    RUN(test_adts_junk_and_tags);
    RUN(test_adts_junk_and_format_change);
    RUN(test_mkv_lacing_and_live_layout);
    RUN(test_protected_content);
    RUN(test_damaged_files_do_not_crash);
    RUN(test_scan_video_file);
    RUN(test_mp3_cbr);
    RUN(test_mp3_vbr);
    RUN(test_mp3_mpeg2);
    RUN(test_mp3_no_header);
    RUN(test_mp3_junk);

    fa_engine_destroy(g_engine);
    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    printf("PASS\n");
    return 0;
}
