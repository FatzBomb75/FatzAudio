/* FLAC outside .flac files, decoded by the built-in FLAC codec (no FFmpeg): Ogg
 * FLAC, FLAC tracks in Matroska and MP4, FLAC packets pushed by the app. Each one
 * must give exactly what the plain .flac of the same stream gives
 * (tests/data/flac, make_flac_data.sh). */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "fa_internal.h"
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

static void data_path(char* out, size_t cap, const char* name)
{
    snprintf(out, cap, "%s/flac/%s", FA_TEST_DATA_DIR, name);
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

static fa_result open_file(const char* name, int32_t track, fa_stream** s, fa_stream_info* info)
{
    char path[512];
    fa_open_options o;
    fa_result r;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    o.audio_track = track;
    data_path(path, sizeof(path), name);
    *s = NULL;
    r = fa_stream_open_path(g_engine, path, &o, s);
    if (r == FA_OK) fa_stream_get_info(*s, info);
    return r;
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

static double rms(const float* p, int64_t from, int64_t to, uint32_t ch, uint32_t c)
{
    double acc = 0;
    int64_t i;
    for (i = from; i < to; i++) acc += (double)p[i * ch + c] * p[i * ch + c];
    return to > from ? sqrt(acc / (double)(to - from)) : 0.0;
}

/* Frequency of channel c from its zero crossings. */
static double freq(const float* p, int64_t from, int64_t to, uint32_t ch, uint32_t c, uint32_t rate)
{
    int64_t i, first = -1, last = -1, crossings = 0;
    for (i = from + 1; i < to; i++)
        if ((p[(i - 1) * ch + c] < 0) != (p[i * ch + c] < 0)) {
            if (first < 0) first = i;
            last = i;
            crossings++;
        }
    if (crossings < 2) return 0;
    return (double)(crossings - 1) / 2.0 / ((double)(last - first) / rate);
}

/* Seeks to each target and compares what follows with the full decode. */
static int seek_matches(fa_stream* s, const float* full, int64_t total, uint32_t ch,
                        const int64_t* targets, int count, double tol, double* worst)
{
    int k;
    *worst = 0;
    for (k = 0; k < count; k++) {
        int64_t at = targets[k], len = 3000, n = 0, i;
        float* p;
        if (at > total) continue;
        if (at + len > total) len = total - at;
        p = (float*)calloc((size_t)len * ch + 1, sizeof(float));
        if (fa_stream_seek(s, at) != FA_OK) { free(p); return 0; }
        while (n < len) {
            fa_frames got = 0;
            fa_frames want = len - n < 777 ? len - n : 777;   /* odd sizes on purpose */
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
        if (*worst > tol) {
            fprintf(stderr, "  seek to %lld: differs by %g\n", (long long)at, *worst);
            return 0;
        }
    }
    return 1;
}

static int ogg_packets(const uint8_t* d, size_t n, const uint8_t** pk, size_t* len, int max)
{
    size_t pos = 0;
    int count = 0;
    static uint8_t joined[1 << 16];
    size_t jl = 0;
    while (pos + 27 <= n && count < max) {
        uint32_t nseg = d[pos + 26], i;
        const uint8_t* body = d + pos + 27 + nseg;
        size_t off = 0;
        for (i = 0; i < nseg; i++) {
            memcpy(joined + jl, body + off, d[pos + 27 + i]);
            jl += d[pos + 27 + i];
            off += d[pos + 27 + i];
            if (d[pos + 27 + i] < 255) {
                uint8_t* copy = (uint8_t*)malloc(jl ? jl : 1);
                memcpy(copy, joined, jl);
                pk[count] = copy;
                len[count++] = jl;
                jl = 0;
                if (count == max) break;
            }
        }
        pos += 27 + nseg + off;
    }
    return count;
}

/* Decodes `name` and the reference .flac and wants the same samples, length,
 * format and exact seeks. */
static void same_as_reference(const char* name, const char* ref, const char* container, uint32_t channels,
                              uint32_t rate)
{
    fa_stream *s, *r;
    fa_stream_info info, rinfo;
    float *a, *b;
    int64_t na, nb, i;
    double worst = 0;
    static const int64_t targets[] = { 0, 1, 1151, 1152, 4000, 20000, 30001 };
    CHECK_EQ(open_file(name, 0, &s, &info), FA_OK);
    CHECK_EQ(open_file(ref, 0, &r, &rinfo), FA_OK);
    CHECK(strcmp(info.codec, "flac") == 0);
    CHECK(strcmp(info.container, container) == 0);
    CHECK_EQ(info.format.channels, channels);
    CHECK_EQ(info.format.sample_rate, rate);
    CHECK(info.seekable);
    CHECK_EQ(info.length_frames, rinfo.length_frames);
    a = read_all(s, channels, &na);
    b = read_all(r, channels, &nb);
    printf("  %s: %lld frames (reference %lld)\n", name, (long long)na, (long long)nb);
    CHECK_EQ(na, nb);
    for (i = 0; i < na * (int64_t)channels; i++)
        if (a[i] != b[i]) { fprintf(stderr, "  sample %lld differs\n", (long long)i); CHECK(0); }
    CHECK(seek_matches(s, b, nb, channels, targets, (int)(sizeof(targets) / sizeof(targets[0])), 0.0, &worst));
    free(a);
    free(b);
    fa_stream_close(s);
    fa_stream_close(r);
}

static void test_ogg_flac(void) { same_as_reference("tone.oga", "tone.flac", "ogg", 2, 44100); }
static void test_flac_in_matroska(void) { same_as_reference("tone_flac.mkv", "tone.flac", "matroska", 2, 44100); }
static void test_flac_in_mp4(void) { same_as_reference("tone_flac.mp4", "tone.flac", "mp4", 2, 44100); }

/* 5.1, 24-bit: the channels in the library's order, in every container. */
static void test_surround(void)
{
    static const char* names[] = { "surround.oga", "surround_flac.mkv" };
    int k;
    same_as_reference("surround.oga", "surround.flac", "ogg", 6, 48000);
    same_as_reference("surround_flac.mkv", "surround.flac", "matroska", 6, 48000);
    for (k = 0; k < 2; k++) {
        fa_stream* s;
        fa_stream_info info;
        float* p;
        int64_t n;
        uint32_t c;
        CHECK_EQ(open_file(names[k], 0, &s, &info), FA_OK);
        p = read_all(s, 6, &n);
        for (c = 0; c < 6; c++) {
            CHECK_NEAR(freq(p, 0, n, 6, c, 48000), 100.0 * (c + 1), 3.0);
            CHECK_NEAR(rms(p, 0, n, 6, c), 0.1 * (c + 1) / sqrt(2.0), 0.01);
        }
        free(p);
        fa_stream_close(s);
    }
}

/* Packets from the app (as from FFmpeg's demuxer): with STREAMINFO as the
 * extradata, and without any (the frame headers win over wrong parameters). */
static void test_packets_pushed(void)
{
    size_t size = 0, len[256];
    const uint8_t* pk[256];
    uint8_t* data = load_file("tone.oga", &size);
    fa_stream* r;
    fa_stream_info rinfo;
    float* ref;
    int64_t nref;
    int count, i, pass;
    CHECK(data != NULL);
    count = ogg_packets(data, size, pk, len, 256);
    free(data);
    CHECK(count > 30);
    CHECK(len[0] == 51);
    CHECK_EQ(open_file("tone.flac", 0, &r, &rinfo), FA_OK);
    ref = read_all(r, 2, &nref);
    fa_stream_close(r);
    for (pass = 0; pass < 3; pass++) {
        fa_codec_params p;
        fa_open_options o;
        fa_stream* s;
        fa_stream_info info;
        float* out;
        int64_t n, j;
        fa_codec_params_init(&p);
        p.codec = "flac";
        p.sample_rate = 44100;
        p.channels = 2;
        p.bits_per_coded_sample = 16;
        if (pass == 0) {
            p.extradata = pk[0] + 17;         /* STREAMINFO */
            p.extradata_size = 34;
        } else if (pass == 1) {
            p.bits_per_coded_sample = 0;      /* the frame headers give it */
        } else {
            p.channels = 1;                   /* wrong: the frames still decode */
        }
        p.time_base.num = 1;
        p.time_base.den = 44100;
        fa_open_options_init(&o);
        o.flags = FA_OPEN_DECODE_ONLY;
        CHECK_EQ(fa_stream_open_packets(g_engine, &p, &o, &s), FA_OK);
        for (i = 1; i < count; i++) {
            fa_packet k;
            if (len[i] < 2 || pk[i][0] != 0xFF) continue;   /* metadata blocks */
            memset(&k, 0, sizeof(k));
            k.data = pk[i];
            k.size = len[i];
            k.pts = FA_PTS_NONE;
            CHECK_EQ(fa_stream_push_packet(s, &k), FA_OK);
        }
        CHECK_EQ(fa_stream_push_end(s), FA_OK);
        CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
        out = read_all(s, info.format.channels, &n);
        CHECK(strcmp(info.codec, "flac") == 0);
        CHECK_EQ(n, nref);
        for (j = 0; j < n * 2 && pass < 2; j++)
            if (out[j] != ref[j]) { fprintf(stderr, "  pass %d: sample %lld differs\n", pass, (long long)j); CHECK(0); }
        free(out);
        fa_stream_close(s);
    }
    free(ref);
    for (i = 0; i < count; i++) free((void*)pk[i]);
}

/* Ogg FLAC's comment block gives the loudness tags. */
static void test_loudness_tags(void)
{
    char path[512];
    fa_source src;
    fa_tag_loudness t;
    data_path(path, sizeof(path), "tone.oga");
    CHECK_EQ(fa_source_open_path(&src, path), FA_OK);
    fa_tags_read_loudness(&src, &t);
    fa_source_close(&src);
    CHECK_NEAR(t.track_lufs, -18.0 + 7.25, 1e-9);
}

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

/* A forward-only source decodes to what the file decodes to (`slack`: frames
 * more it may give without the file's length). */
static void compare_forward_only(const char* name, uint32_t ch, int64_t slack)
{
    size_t size = 0;
    uint8_t* data = load_file(name, &size);
    mem_io m;
    fa_io_callbacks io = { io_read, NULL, NULL, io_close };
    fa_open_options o;
    fa_stream* s = NULL;
    fa_stream_info info;
    float *a, *b;
    int64_t na, nb, i;
    CHECK(data != NULL);
    CHECK_EQ(open_file(name, -1, &s, &info), FA_OK);
    a = read_all(s, ch, &na);
    fa_stream_close(s);
    m.p = data;
    m.n = size;
    m.pos = 0;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    CHECK_EQ(fa_stream_open_io(g_engine, &io, &m, &o, &s), FA_OK);
    fa_stream_get_info(s, &info);
    CHECK(!info.seekable);
    b = read_all(s, ch, &nb);
    /* Matroska keeps its duration up front; Ogg has it only at the end. */
    CHECK(info.length_frames == -1 || info.length_frames == na);
    fa_stream_close(s);
    free(data);
    CHECK(nb >= na && nb <= na + slack);
    for (i = 0; i < na * (int64_t)ch; i++) CHECK_NEAR(b[i], a[i], 0.0);
    free(a);
    free(b);
}

/* From a source that cannot seek: no length, every frame still there. */
static void test_forward_only(void)
{
    compare_forward_only("tone.oga", 2, 0);
    compare_forward_only("tone_flac.mkv", 2, 0);
}

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
    static const char* names[] = { "tone.oga", "tone_flac.mkv", "tone_flac.mp4" };
    int f;
    for (f = 0; f < 3; f++) {
        size_t size = 0, cut;
        uint8_t* data = load_file(names[f], &size);
        uint32_t seed = 12345;
        int k;
        CHECK(data != NULL);
        for (cut = 0; cut < size; cut += size / 23 + 1) exercise_memory(data, cut, 0);
        for (k = 0; k < 40; k++) {
            uint8_t* d = (uint8_t*)malloc(size);
            int j;
            memcpy(d, data, size);
            for (j = 0; j < 8; j++) {
                seed = seed * 1103515245u + 12345u;
                d[(seed >> 8) % size] ^= (uint8_t)(1u << (seed % 8));
            }
            exercise_memory(d, size, 0);
            free(d);
        }
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

    RUN(test_ogg_flac);
    RUN(test_flac_in_matroska);
    RUN(test_flac_in_mp4);
    RUN(test_surround);
    RUN(test_packets_pushed);
    RUN(test_loudness_tags);
    RUN(test_forward_only);
    RUN(test_damaged_files_do_not_crash);

    fa_engine_destroy(g_engine);
    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    printf("PASS\n");
    return 0;
}
