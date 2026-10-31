/* Dolby Digital (AC-3) and Dolby Digital Plus (E-AC-3) decoding, built in.
 *
 * tests/data/dolby holds E-AC-3 streams written by eac3_writer.c with the coding
 * tools real encoders seldom use (spectral extension, coupling that changes
 * within a frame, delta bit allocation, block-level SNR offsets, dependent
 * substreams, every channel layout), and what the ffmpeg CLI decodes them to
 * (make_dolby_data.sh). The decoder must match FFmpeg's to within float
 * rounding: FFmpeg works in 24-bit fixed point before the transform, so very
 * quiet blocks agree to about 45 dB and everything else to 80 dB or more. */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "fa_internal.h"
#include "ac3.h"
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
static fa_device* g_dev;

static uint8_t* load_file(const char* name, size_t* size)
{
    char path[512];
    FILE* f;
    uint8_t* p;
    long n;
    snprintf(path, sizeof(path), "%s/%s", FA_TEST_DATA_DIR, name);
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

static fa_result open_file(const char* name, int32_t dynamic_range, fa_stream** s)
{
    char path[512];
    fa_open_options o;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    o.dynamic_range = dynamic_range;
    snprintf(path, sizeof(path), "%s/%s", FA_TEST_DATA_DIR, name);
    *s = NULL;
    return fa_stream_open_path(g_engine, path, &o, s);
}

static float* read_all(fa_stream* s, uint32_t channels, int64_t* out_n)
{
    int64_t cap = 1 << 14, n = 0;
    float* all = (float*)malloc((size_t)cap * channels * sizeof(float));
    for (;;) {
        fa_frames got = 0;
        fa_result r;
        if (n + 1000 > cap) {
            cap *= 2;
            all = (float*)realloc(all, (size_t)cap * channels * sizeof(float));
        }
        r = fa_stream_read(s, all + (size_t)n * channels, 1000, &got);
        n += got;
        if (r != FA_OK || got == 0) break;
    }
    *out_n = n;
    return all;
}

/* A whole file decoded through the public API. */
typedef struct decoded {
    float* pcm;
    int64_t frames;
    fa_stream_info info;
} decoded;

static int decode(const char* name, int32_t dynamic_range, decoded* d)
{
    fa_stream* s = NULL;
    memset(d, 0, sizeof(*d));
    if (open_file(name, dynamic_range, &s) != FA_OK) { fprintf(stderr, "  cannot open %s\n", name); return 0; }
    fa_stream_get_info(s, &d->info);
    d->pcm = read_all(s, d->info.format.channels, &d->frames);
    fa_stream_close(s);
    return 1;
}

/* SNR in dB of channel `mc` of `m` against channel `rc` of `r`, over frames
 * [from, to); 999 when identical or silent. */
static double snr(const decoded* m, int mc, const decoded* r, int rc, int64_t from, int64_t to)
{
    double e = 0, s = 0;
    uint32_t mn = m->info.format.channels, rn = r->info.format.channels;
    int64_t i;
    for (i = from; i < to; i++) {
        double x = r->pcm[i * rn + rc], d = m->pcm[i * mn + mc] - x;
        e += d * d;
        s += x * x;
    }
    if (e == 0) return 999;
    return 10 * log10((s + 1e-20) / e);
}

/* Compares a decoded stream with FFmpeg's decode of it. map[k]: the channel of
 * ours that FFmpeg's channel k is (its order is L R C LFE, back or centre
 * surround, side pair; ours is the layout of fa_stream_info's channel count). */
static void compare(const char* name, const char* ref_name, int32_t dynamic_range, const int* map)
{
    decoded m, r;
    uint32_t c;
    int64_t b;
    double worst_block = 999, worst = 999;
    CHECK(decode(name, dynamic_range, &m));
    CHECK(decode(ref_name, 0, &r));
    CHECK(strcmp(m.info.codec, "eac3") == 0);
    CHECK(strcmp(m.info.container, "ec3") == 0);
    CHECK_EQ(m.info.format.sample_rate, 48000);
    CHECK_EQ(m.frames, r.frames);
    CHECK_EQ(m.info.length_frames, m.frames);
    for (c = 0; c < r.info.format.channels; c++) {
        double v = snr(&m, map[c], &r, (int)c, 0, m.frames);
        if (v < worst) worst = v;
        for (b = 0; b + 256 <= m.frames; b += 256) {
            v = snr(&m, map[c], &r, (int)c, b, b + 256);
            if (v < worst_block) worst_block = v;
        }
    }
    printf("  %s: %u channels, %lld frames, %.0f dB (worst block %.0f dB)\n", name, m.info.format.channels,
           (long long)m.frames, worst, worst_block);
    CHECK(worst > 75);
    CHECK(worst_block > 40);
    free(m.pcm);
    free(r.pcm);
}

static const int k_same[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };

static void test_matches_ffmpeg(void)
{
    /* FFmpeg's channels -> ours, where the layouts differ. */
    static const int mono_lfe[2] = { 2, 3 };          /* C LFE -> L R C LFE SL SR */
    static const int stereo_lfe[3] = { 0, 1, 3 };     /* L R LFE -> 6 channels */
    static const int lcr_lfe[4] = { 0, 1, 2, 3 };     /* L R C LFE -> 6 channels */
    static const int lrs_lfe[4] = { 0, 1, 3, 4 };     /* L R LFE S -> L R C LFE BC SL SR */
    static const int lcrs_lfe[5] = { 0, 1, 2, 3, 4 }; /* L R C LFE S -> 7 channels */
    static const int quad[4] = { 0, 1, 3, 4 };        /* L R Ls Rs -> L R C BL BR */
    compare("dolby/mono.ec3", "dolby/mono.wav", FA_DYNAMIC_RANGE_NORMAL, k_same);
    compare("dolby/stereo.ec3", "dolby/stereo.wav", FA_DYNAMIC_RANGE_NORMAL, k_same);
    compare("dolby/51.ec3", "dolby/51.wav", FA_DYNAMIC_RANGE_NORMAL, k_same);
    compare("dolby/71.ec3", "dolby/71.wav", FA_DYNAMIC_RANGE_NORMAL, k_same);
    compare("dolby/1_0_lfe.ec3", "dolby/1_0_lfe.wav", FA_DYNAMIC_RANGE_NORMAL, mono_lfe);
    compare("dolby/2_0_lfe.ec3", "dolby/2_0_lfe.wav", FA_DYNAMIC_RANGE_NORMAL, stereo_lfe);
    compare("dolby/3_0_lfe.ec3", "dolby/3_0_lfe.wav", FA_DYNAMIC_RANGE_NORMAL, lcr_lfe);
    compare("dolby/2_1_lfe.ec3", "dolby/2_1_lfe.wav", FA_DYNAMIC_RANGE_NORMAL, lrs_lfe);
    compare("dolby/3_1_lfe.ec3", "dolby/3_1_lfe.wav", FA_DYNAMIC_RANGE_NORMAL, lcrs_lfe);
    compare("dolby/2_2.ec3", "dolby/2_2.wav", FA_DYNAMIC_RANGE_NORMAL, quad);
    /* The adaptive hybrid transform (vector and gain-adaptive quantization). */
    compare("dolby/aht_mono.ec3", "dolby/aht_mono.wav", FA_DYNAMIC_RANGE_NORMAL, k_same);
    compare("dolby/aht_stereo.ec3", "dolby/aht_stereo.wav", FA_DYNAMIC_RANGE_NORMAL, k_same);
    compare("dolby/aht_51.ec3", "dolby/aht_51.wav", FA_DYNAMIC_RANGE_NORMAL, k_same);
    compare("dolby/aht_71.ec3", "dolby/aht_71.wav", FA_DYNAMIC_RANGE_NORMAL, k_same);
}

/* Decodes a raw stream access unit by access unit with ac3.c, counting the units
 * that fail and the decoded samples that are not 0. */
static void decode_units(const char* name, int* units, int* errors, long* nonzero)
{
    size_t size, pos = 0;
    uint8_t* data = load_file(name, &size);
    fa_ac3* d = fa_ac3_create();
    float* pcm = NULL;
    size_t cap = 0;
    *units = *errors = 0;
    *nonzero = 0;
    while (data && d && pos + 8 <= size) {
        fa_ac3_header h, n;
        size_t end;
        int frames = 0, channels = 0, i;
        uint32_t rate = 0;
        if (!fa_ac3_parse_header(data + pos, size - pos, &h)) { pos++; continue; }
        end = pos + h.frame_size;
        while (end + 8 <= size && fa_ac3_parse_header(data + end, size - end, &n) && n.eac3 && n.strmtyp == 1)
            end += n.frame_size;
        if (end > size) break;
        if (fa_ac3_decode(d, data + pos, end - pos, FA_AC3_DRC_OFF, &pcm, &cap, &frames, &channels, &rate) != FA_OK)
            (*errors)++;
        for (i = 0; i < frames * channels; i++) *nonzero += pcm[i] != 0.0f;
        (*units)++;
        pos = end;
    }
    fa_ac3_destroy(d);
    free(pcm);
    free(data);
}

/* Two streams that code the same audio two ways decode the same. */
static void same_audio(const char* a, const char* b)
{
    decoded m, r;
    uint32_t c;
    int units, errors;
    long nonzero;
    double worst = 999;
    decode_units(a, &units, &errors, &nonzero);
    CHECK(units > 0);
    CHECK_EQ(errors, 0);
    CHECK(nonzero > 0);
    decode_units(b, &units, &errors, &nonzero);
    CHECK_EQ(errors, 0);
    CHECK(decode(a, FA_DYNAMIC_RANGE_FULL, &m));
    CHECK(decode(b, FA_DYNAMIC_RANGE_FULL, &r));
    CHECK_EQ(m.frames, r.frames);
    CHECK_EQ(m.info.format.channels, r.info.format.channels);
    for (c = 0; c < r.info.format.channels; c++) {
        double v = snr(&m, (int)c, &r, (int)c, 0, m.frames);
        if (v < worst) worst = v;
    }
    printf("  %s = %s: %d units, %u channels, %.0f dB\n", a, b, units, m.info.format.channels, worst);
    CHECK(worst > 200);
    free(m.pcm);
    free(r.pcm);
}

static void test_tools_ffmpeg_lacks(void)
{
    int units, errors;
    long nonzero;
    /* Enhanced coupling against the same bands as standard coupling (sub-band 4
     * and up, amplitudes standard coordinates can express). */
    same_audio("dolby/ecpl_stereo.ec3", "dolby/ecpl_stereo_std.ec3");
    same_audio("dolby/ecpl_51.ec3", "dolby/ecpl_51_std.ec3");
    /* Enhanced coupling from sub-band 0, with any amplitude and the reserved
     * fields: every frame must parse to its end. */
    decode_units("dolby/ecpl_any.ec3", &units, &errors, &nonzero);
    printf("  dolby/ecpl_any.ec3: %d units, %d errors\n", units, errors);
    CHECK(units == 6);
    CHECK_EQ(errors, 0);
    CHECK(nonzero > 0);
    /* Block-level SNR offsets and default fast gains (TS 102 366 E.1.2.4), against
     * frame-level offsets and fast gains sent. */
    same_audio("dolby/snr_51.ec3", "dolby/snr_51_frame.ec3");
    same_audio("dolby/snr_any.ec3", "dolby/snr_any_frame.ec3");
}

static void test_dynamic_range_modes(void)
{
    fa_stream* s = NULL;
    fa_dynamic_range mode = FA_DYNAMIC_RANGE_NIGHT;
    decoded full, normal;
    float buf[2 * 1536];
    fa_frames got = 0;
    int64_t i;
    double diff = 0;
    compare("dolby/stereo.ec3", "dolby/stereo_full.wav", FA_DYNAMIC_RANGE_FULL, k_same);
    compare("dolby/stereo.ec3", "dolby/stereo_night.wav", FA_DYNAMIC_RANGE_NIGHT, k_same);
    /* FULL is the default, and the mode can change on an open stream. */
    CHECK(decode("dolby/stereo.ec3", -1, &full));   /* out of range: the default */
    CHECK(decode("dolby/stereo.ec3", FA_DYNAMIC_RANGE_NORMAL, &normal));
    CHECK(open_file("dolby/stereo.ec3", FA_DYNAMIC_RANGE_FULL, &s) == FA_OK);
    CHECK_EQ(fa_stream_get_dynamic_range(s, &mode), FA_OK);
    CHECK_EQ(mode, FA_DYNAMIC_RANGE_FULL);
    CHECK_EQ(fa_stream_set_dynamic_range(s, FA_DYNAMIC_RANGE_NORMAL), FA_OK);
    CHECK_EQ(fa_stream_get_dynamic_range(s, &mode), FA_OK);
    CHECK_EQ(mode, FA_DYNAMIC_RANGE_NORMAL);
    CHECK_EQ(fa_stream_set_dynamic_range(s, (fa_dynamic_range)7), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_stream_seek(s, 1536), FA_OK);
    CHECK_EQ(fa_stream_read(s, buf, 1536, &got), FA_OK);
    CHECK_EQ(got, 1536);
    for (i = 0; i < 1536 * 2; i++) {
        CHECK_NEAR(buf[i], normal.pcm[1536 * 2 + i], 1e-4);
        diff += fabs(normal.pcm[1536 * 2 + i] - full.pcm[1536 * 2 + i]);
    }
    CHECK(diff > 1.0);   /* the stream does carry dynamic range gains */
    fa_stream_close(s);
    free(full.pcm);
    free(normal.pcm);
}

/* A seek lands on the exact frame and decodes what a straight decode does there
 * (these streams have no dither, so the output is deterministic). */
static void test_seek_is_exact(void)
{
    static const int64_t targets[] = { 3000, 0, 1536, 2047, 777, 2560 };
    decoded d;
    fa_stream* s = NULL;
    size_t k;
    CHECK(decode("dolby/51.ec3", FA_DYNAMIC_RANGE_NORMAL, &d));
    CHECK(d.info.seekable);
    CHECK(open_file("dolby/51.ec3", FA_DYNAMIC_RANGE_NORMAL, &s) == FA_OK);
    for (k = 0; k < sizeof(targets) / sizeof(targets[0]); k++) {
        float buf[6 * 400];
        fa_frames got = 0, pos = -1;
        int64_t i, want = d.frames - targets[k] < 400 ? d.frames - targets[k] : 400;
        CHECK_EQ(fa_stream_seek(s, targets[k]), FA_OK);
        CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
        CHECK_EQ(pos, targets[k]);
        CHECK_EQ(fa_stream_read(s, buf, (fa_frames)want, &got), FA_OK);
        CHECK_EQ(got, want);
        for (i = 0; i < want * 6; i++) CHECK_NEAR(buf[i], d.pcm[targets[k] * 6 + i], 1e-5);
    }
    fa_stream_close(s);
    free(d.pcm);
}

/* RMS of channel 0 over the middle half. */
static double mid_rms(const decoded* d)
{
    double acc = 0;
    int64_t i, from = d->frames / 4, to = d->frames * 3 / 4;
    for (i = from; i < to; i++) acc += (double)d->pcm[i * d->info.format.channels] * d->pcm[i * d->info.format.channels];
    return sqrt(acc / (double)(to - from));
}

/* FFmpeg-encoded tones (1 kHz, amplitude 0.5) as raw files, in MP4 and in Matroska. */
static void test_tones_in_containers(void)
{
    static const struct { const char* file; const char* codec; const char* container; } files[] = {
        { "tone.ac3", "ac3", "ac3" }, { "tone.eac3", "eac3", "ec3" },
        { "dolby/tone_ac3.mp4", "ac3", "mp4" }, { "dolby/tone_eac3.mkv", "eac3", "matroska" } };
    size_t k;
    for (k = 0; k < sizeof(files) / sizeof(files[0]); k++) {
        decoded d;
        CHECK(decode(files[k].file, FA_DYNAMIC_RANGE_FULL, &d));
        printf("  %s: %s in %s, %u Hz, %u channels, %lld frames\n", files[k].file, d.info.codec, d.info.container,
               d.info.format.sample_rate, d.info.format.channels, (long long)d.frames);
        CHECK(strcmp(d.info.codec, files[k].codec) == 0);
        CHECK(strncmp(d.info.container, files[k].container, strlen(files[k].container)) == 0);
        CHECK_EQ(d.info.format.sample_rate, 48000);
        CHECK_EQ(d.info.format.channels, 2);
        CHECK(d.frames >= 24000 && d.frames <= 24576 + 1536);
        CHECK_EQ(d.info.length_frames, d.frames);
        CHECK_NEAR(mid_rms(&d), 0.3536, 0.01);
        free(d.pcm);
    }
}

/* A raw constant-bit-rate file opens without an index (unit k is at k frames);
 * the same data with junk in the middle is indexed instead. Both have the exact
 * length, and seeks land on the frame (FFmpeg dithers, so only near a straight
 * decode). */
static void test_raw_cbr_and_index(void)
{
    static const int64_t targets[] = { 5000, 0, 1536, 9000, 777, 23000 };
    size_t size = 0, k;
    uint8_t* data = load_file("tone.ac3", &size);
    uint8_t* junk;
    fa_open_options o;
    fa_stream* s = NULL;
    fa_stream_info info;
    decoded d;
    float* all;
    int64_t n, i;
    CHECK(data != NULL && size == 16 * 768);
    CHECK(decode("tone.ac3", FA_DYNAMIC_RANGE_FULL, &d));
    CHECK_EQ(d.frames, 16 * 1536);
    CHECK_EQ(d.info.length_frames, d.frames);
    CHECK(open_file("tone.ac3", FA_DYNAMIC_RANGE_FULL, &s) == FA_OK);
    for (k = 0; k < sizeof(targets) / sizeof(targets[0]); k++) {
        float buf[2 * 400];
        fa_frames got = 0, pos = -1;
        int64_t want = d.frames - targets[k] < 400 ? d.frames - targets[k] : 400;
        CHECK_EQ(fa_stream_seek(s, targets[k]), FA_OK);
        CHECK_EQ(fa_stream_get_position(s, &pos), FA_OK);
        CHECK_EQ(pos, targets[k]);
        CHECK_EQ(fa_stream_read(s, buf, (fa_frames)want, &got), FA_OK);
        CHECK_EQ(got, want);
        for (i = 0; i < want * 2; i++) CHECK_NEAR(buf[i], d.pcm[targets[k] * 2 + i], 1e-3);
    }
    fa_stream_close(s);

    /* 100 bytes of junk after the fifth frame: still 16 frames, same audio. */
    junk = (uint8_t*)malloc(size + 100);
    memcpy(junk, data, 5 * 768);
    memset(junk + 5 * 768, 0x55, 100);
    memcpy(junk + 5 * 768 + 100, data + 5 * 768, size - 5 * 768);
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    o.dynamic_range = FA_DYNAMIC_RANGE_FULL;
    CHECK_EQ(fa_stream_open_memory(g_engine, junk, size + 100, &o, &s), FA_OK);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK_EQ(info.length_frames, d.frames);
    all = read_all(s, 2, &n);
    CHECK_EQ(n, d.frames);
    for (i = 0; i < n * 2 && i < d.frames * 2; i++) CHECK_NEAR(all[i], d.pcm[i], 0.0);
    fa_stream_close(s);
    free(all);
    free(junk);
    free(data);
    free(d.pcm);
}

/* A damaged frame decodes as silence of its own length; the frames after it are
 * intact once the overlap has settled (one block), and a cut-off last frame is
 * dropped. */
static void test_damaged_frames(void)
{
    size_t size = 0, frame;
    uint8_t* data = load_file("dolby/stereo.ec3", &size);
    fa_ac3* dec = fa_ac3_create();
    fa_ac3_header h;
    float* pcm = NULL;
    size_t cap = 0, at = 0;
    int frames = 0, channels = 0, k = 0;
    uint32_t rate = 0;
    decoded ref;
    CHECK(data != NULL && dec != NULL);
    CHECK(decode("dolby/stereo.ec3", FA_DYNAMIC_RANGE_FULL, &ref));
    CHECK(fa_ac3_parse_header(data, size, &h));
    CHECK(h.eac3 && h.blocks == 6 && h.acmod == 2);
    frame = h.frame_size;
    data[frame + frame / 2] ^= 0x5A;              /* the second frame fails its CRC */
    while (at < size) {
        fa_result r;
        int i;
        CHECK(fa_ac3_parse_header(data + at, size - at, &h));
        r = fa_ac3_decode(dec, data + at, h.frame_size, FA_AC3_DRC_OFF, &pcm, &cap, &frames, &channels, &rate);
        CHECK_EQ(frames, 1536);
        CHECK_EQ(channels, 2);
        if (k == 1) {
            CHECK_EQ(r, FA_ERROR_GENERIC);
            for (i = 0; i < frames * 2; i++) CHECK(pcm[i] == 0.0f);
        } else {
            CHECK_EQ(r, FA_OK);
            for (i = k == 2 ? 256 * 2 : 0; i < frames * 2; i++) CHECK_NEAR(pcm[i], ref.pcm[(size_t)k * 1536 * 2 + i], 1e-5);
        }
        at += h.frame_size;
        k++;
    }
    CHECK_EQ(k, 3);
    /* Half a frame: nothing to decode. */
    CHECK_EQ(fa_ac3_decode(dec, data, frame / 2, FA_AC3_DRC_OFF, &pcm, &cap, &frames, &channels, &rate), FA_ERROR_GENERIC);
    CHECK_EQ(frames, 0);
    free(pcm);
    fa_ac3_destroy(dec);
    free(data);
    free(ref.pcm);
}

/* A damaged dependent frame (7.1) silences only its own channels: the 5.1 bed
 * plays on and the layout stays 8 channels. */
static void test_damaged_dependent_frame(void)
{
    size_t size = 0, at = 0, units[8], nunits = 0, u;
    uint8_t* data = load_file("dolby/71.ec3", &size);
    uint8_t* clean;
    fa_ac3* dec = fa_ac3_create();
    fa_ac3* ref = fa_ac3_create();
    float *pcm = NULL, *rpcm = NULL;
    size_t cap = 0, rcap = 0;
    int frames = 0, channels = 0, rframes = 0, rchannels = 0, i, c;
    uint32_t rate = 0;
    CHECK(data != NULL && dec != NULL && ref != NULL);
    if (!data) return;
    /* Units: an independent frame and the dependent one after it. */
    while (at < size && nunits < 8) {
        fa_ac3_header h;
        CHECK(fa_ac3_parse_header(data + at, size - at, &h));
        if (h.strmtyp != 1) units[nunits++] = at;
        at += h.frame_size;
    }
    CHECK_EQ(nunits, 2);
    clean = (uint8_t*)malloc(size);
    memcpy(clean, data, size);
    {
        fa_ac3_header h;
        fa_ac3_parse_header(data + units[1], size - units[1], &h);
        data[units[1] + h.frame_size + 40] ^= 0x5A;   /* the second unit's dependent frame fails its CRC */
    }
    for (u = 0; u < nunits; u++) {
        size_t end = u + 1 < nunits ? units[u + 1] : size;
        fa_result r = fa_ac3_decode(dec, data + units[u], end - units[u], FA_AC3_DRC_OFF, &pcm, &cap, &frames, &channels, &rate);
        CHECK_EQ(fa_ac3_decode(ref, clean + units[u], end - units[u], FA_AC3_DRC_OFF, &rpcm, &rcap, &rframes, &rchannels, &rate), FA_OK);
        CHECK_EQ(channels, 8);
        CHECK_EQ(frames, rframes);
        CHECK_EQ(r, u == 1 ? FA_ERROR_GENERIC : FA_OK);
        for (i = 0; i < frames; i++) {
            for (c = 0; c < 8; c++) {
                float got = pcm[(size_t)i * 8 + c], want = rpcm[(size_t)i * 8 + c];
                if (u == 1 && (c == 4 || c == 5)) CHECK(got == 0.0f);          /* BL BR: only in the dependent frame */
                else if (u == 1 && c >= 6) continue;                          /* SL SR: the bed's surrounds */
                else CHECK_NEAR(got, want, 1e-5);
            }
        }
    }
    free(pcm);
    free(rpcm);
    free(clean);
    fa_ac3_destroy(dec);
    fa_ac3_destroy(ref);
    free(data);
}

/* Packets that end between an independent frame and its dependent one decode
 * as 7.1, matching the file. Only the first unit, before the stream is known to
 * have dependent frames, plays without them. */
static void test_dependent_frames_in_separate_packets(void)
{
    size_t size = 0, at = 0;
    uint8_t* data = load_file("dolby/71.ec3", &size);
    fa_codec_params p;
    fa_open_options o;
    fa_stream* s = NULL;
    fa_stream_info info;
    decoded ref;
    float* out;
    int64_t n = 0, i;
    double err = 0;
    CHECK(data != NULL);
    CHECK(decode("dolby/71.ec3", FA_DYNAMIC_RANGE_FULL, &ref));
    fa_codec_params_init(&p);
    p.codec = "eac3";
    p.sample_rate = 48000;
    p.channels = 8;
    p.time_base.num = 1;
    p.time_base.den = 48000;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    CHECK_EQ(fa_stream_open_packets(g_engine, &p, &o, &s), FA_OK);
    while (at < size) {
        fa_ac3_header h;
        fa_packet pk;
        CHECK(fa_ac3_parse_header(data + at, size - at, &h));
        memset(&pk, 0, sizeof(pk));
        pk.data = data + at;
        pk.size = h.frame_size;
        pk.pts = at == 0 ? 0 : FA_PTS_NONE;
        CHECK_EQ(fa_stream_push_packet(s, &pk), FA_OK);
        at += h.frame_size;
    }
    CHECK_EQ(fa_stream_push_end(s), FA_OK);
    out = read_all(s, 8, &n);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK_EQ(info.format.channels, 8);
    CHECK_EQ(n, ref.frames);
    for (i = (1536 + 256) * 8; i < n * 8 && i < ref.frames * 8; i++) err += fabs(out[i] - ref.pcm[i]);
    CHECK(err < 1e-3);
    free(out);
    free(ref.pcm);
    fa_stream_close(s);
    free(data);
}

/* One packet holding a stereo stream and then a 5.1 one: the stereo audio comes
 * out first, the 5.1 audio on the next calls (nothing lost or decoded twice). */
static void test_layout_change_inside_a_packet(void)
{
    size_t na = 0, nb = 0;
    uint8_t* a = load_file("dolby/stereo.ec3", &na);
    uint8_t* b = load_file("dolby/51.ec3", &nb);
    uint8_t* both;
    fa_codec c;
    fa_codec_params p;
    decoded ra, rb;
    int64_t got6 = 0, i;
    double err = 0;
    int calls = 0;
    CHECK(a != NULL && b != NULL);
    CHECK(decode("dolby/stereo.ec3", FA_DYNAMIC_RANGE_FULL, &ra));
    CHECK(decode("dolby/51.ec3", FA_DYNAMIC_RANGE_FULL, &rb));
    both = (uint8_t*)malloc(na + nb);
    memcpy(both, a, na);
    memcpy(both + na, b, nb);
    fa_codec_params_init(&p);
    p.codec = "eac3";
    p.sample_rate = 48000;
    p.channels = 2;
    memset(&c, 0, sizeof(c));
    CHECK_EQ(fa_codec_open(&c, &p), FA_OK);
    CHECK_EQ(c.vt->decode(&c, both, na + nb), FA_OK);
    CHECK_EQ(c.channels, 2);
    CHECK_EQ(c.out_frames, ra.frames);
    for (;;) {
        CHECK_EQ(c.vt->decode(&c, NULL, 0), FA_OK);
        if (c.out_frames == 0 || ++calls > 10) break;
        CHECK_EQ(c.channels, 6);
        for (i = 0; i < c.out_frames * 6 && got6 * 6 + i < rb.frames * 6; i++) err += fabs(c.out[i] - rb.pcm[got6 * 6 + i]);
        got6 += c.out_frames;
    }
    CHECK_EQ(got6, rb.frames);
    CHECK(err < 1e-3);
    fa_codec_close(&c);
    free(both);
    free(a);
    free(b);
    free(ra.pcm);
    free(rb.pcm);
}

/* Garbage and truncated files do not crash the reader or the decoder. */
static void test_garbage_does_not_crash(void)
{
    size_t size = 0, n, i;
    uint8_t* data = load_file("dolby/51.ec3", &size);
    uint32_t seed = 12345;
    CHECK(data != NULL);
    for (n = 0; n < 40; n++) {
        uint8_t* copy = (uint8_t*)malloc(size);
        fa_stream* s = NULL;
        memcpy(copy, data, size);
        for (i = 0; i < 1 + n % 8; i++) {
            seed = seed * 1103515245u + 12345u;
            copy[(seed >> 8) % size] = (uint8_t)(seed >> 3);
        }
        if (fa_stream_open_memory(g_engine, copy, n % 5 == 4 ? size / 3 : size, NULL, &s) == FA_OK) {
            int64_t frames = 0;
            fa_stream_info info;
            fa_stream_get_info(s, &info);
            free(read_all(s, info.format.channels, &frames));
            fa_stream_close(s);
        }
        free(copy);
    }
    free(data);
}

/* ---- downmix ------------------------------------------------------------------ */

static const float* weights_of(const fa_ac3_mix* m, int channels, int ltrt, float* w)
{
    return fa_ac3_downmix(m, channels, ltrt, w, NULL) ? w : NULL;
}

/* used: the channels with audio, a bit each. */
static fa_ac3_mix mix_codes(int acmod, int c, int s, uint32_t used)
{
    fa_ac3_mix m;
    memset(&m, 0, sizeof(m));
    m.acmod = acmod;
    m.lfeon = 1;
    m.ltrtcmix = m.lorocmix = c;
    m.ltrtsurmix = m.lorosurmix = s;
    m.lfemix = -1;
    m.used = used;
    return m;
}

/* The weights against TS 102 366 6.8.2, Tables 6.31 and 6.32 (scaled, worst case). */
static void test_downmix_coefficients(void)
{
    float w[16], scale = 0;
    fa_ac3_mix m = mix_codes(7, 4, 4, 0x3f);   /* 3/2, clev = slev = 0.707 */
    /* Lo/Ro: 1 / 2.414 = 0.414 and 0.707 / 2.414 = 0.293. Order L R C LFE SL SR. */
    CHECK(fa_ac3_downmix(&m, 6, 0, w, &scale));
    CHECK_NEAR(scale, 0.4142, 1e-4);
    CHECK_NEAR(w[0 * 2 + 0], 0.4142, 1e-4); CHECK_NEAR(w[0 * 2 + 1], 0.0, 0);
    CHECK_NEAR(w[1 * 2 + 1], 0.4142, 1e-4); CHECK_NEAR(w[1 * 2 + 0], 0.0, 0);
    CHECK_NEAR(w[2 * 2 + 0], 0.2929, 1e-4); CHECK_NEAR(w[2 * 2 + 1], 0.2929, 1e-4);
    CHECK_NEAR(w[3 * 2 + 0], 0.0, 0);                     /* no LFE in an AC-3 downmix */
    CHECK_NEAR(w[4 * 2 + 0], 0.2929, 1e-4); CHECK_NEAR(w[4 * 2 + 1], 0.0, 0);
    CHECK_NEAR(w[5 * 2 + 1], 0.2929, 1e-4); CHECK_NEAR(w[5 * 2 + 0], 0.0, 0);
    /* Lt/Rt: 1 / 3.121 = 0.3204 and 0.707 / 3.121 = 0.2265; both surrounds go to
     * the left output inverted and to the right as they are. */
    CHECK(weights_of(&m, 6, 1, w));
    CHECK_NEAR(w[0 * 2 + 0], 0.3204, 1e-4);
    CHECK_NEAR(w[2 * 2 + 0], 0.2265, 1e-4); CHECK_NEAR(w[2 * 2 + 1], 0.2265, 1e-4);
    CHECK_NEAR(w[4 * 2 + 0], -0.2265, 1e-4); CHECK_NEAR(w[5 * 2 + 0], -0.2265, 1e-4);
    CHECK_NEAR(w[4 * 2 + 1], 0.2265, 1e-4); CHECK_NEAR(w[5 * 2 + 1], 0.2265, 1e-4);
    /* Other levels: clev 0.5, slev 0 (cmixlev 10, surmixlev 10) scale by 1 / 1.5. */
    m = mix_codes(7, 6, 7, 0x3f);
    CHECK(weights_of(&m, 6, 0, w));
    CHECK_NEAR(w[0], 1.0 / 1.5, 1e-6); CHECK_NEAR(w[2 * 2 + 0], 0.5 / 1.5, 1e-6); CHECK_NEAR(w[4 * 2 + 0], 0.0, 0);
    /* 3/1: the single surround at 0.7 x slev in both outputs (L R C S). */
    m = mix_codes(5, 4, 6, 0xf);
    CHECK(weights_of(&m, 4, 0, w));
    {
        double sum = 1.0 + 0.70710678 + 0.7 * 0.5;
        CHECK_NEAR(w[3 * 2 + 0], 0.35 / sum, 1e-5); CHECK_NEAR(w[3 * 2 + 1], 0.35 / sum, 1e-5);
    }
    /* Lt/Rt mono surround: -slev left, +slev right. */
    CHECK(weights_of(&m, 4, 1, w));
    CHECK(w[3 * 2 + 0] < 0 && w[3 * 2 + 1] > 0);
    /* The reserved surround codes read as 0.841 (D.1.3.4); 2/2 has no centre. */
    m = mix_codes(6, 4, 0, 0x1b);
    CHECK(weights_of(&m, 5, 0, w));                     /* L R C BL BR */
    CHECK_NEAR(w[2 * 2 + 0], 0.0, 0);
    CHECK_NEAR(w[3 * 2 + 0] / w[0], 0.8408964, 1e-5);
    /* Silent channels take no headroom: stereo plus LFE plays L and R at full scale. */
    m = mix_codes(2, 4, 4, 0x0b);
    CHECK(weights_of(&m, 6, 0, w));
    CHECK_NEAR(w[0], 1.0, 0); CHECK_NEAR(w[1 * 2 + 1], 1.0, 0); CHECK_NEAR(w[2 * 2 + 0], 0.0, 0);
    /* A 3/1 bed whose dependent substream adds a surround pair: the pair is mixed in. */
    m = mix_codes(5, 4, 4, 0x37);
    CHECK(weights_of(&m, 6, 0, w));
    CHECK(w[4 * 2 + 0] > 0.2f && w[5 * 2 + 1] > 0.2f);
    /* E-AC-3's LFE mix level: 10 - lfemixlevcod dB, mixed in 4.5 dB lower (E.2.9). */
    m = mix_codes(7, 2, 7, 0x3f);
    m.lfemix = 10;
    CHECK(weights_of(&m, 6, 0, w));
    CHECK_NEAR(w[3 * 2 + 0] / w[0], pow(10.0, -4.5 / 20), 1e-5);
    /* A mono programme with LFE (C and LFE have audio): the centre at -3 dB in both. */
    m = mix_codes(1, 4, 4, 0x0c);
    CHECK(weights_of(&m, 6, 0, w));
    CHECK_NEAR(w[2 * 2 + 0], 0.70710678, 1e-6);
    CHECK_NEAR(w[2 * 2 + 1], 0.70710678, 1e-6);
    CHECK_NEAR(w[0], 0.0, 0);
    /* 7.1 (L R C LFE BL BR SL SR): back and side pairs both at slev. */
    m = mix_codes(7, 4, 4, 0xff);
    CHECK(weights_of(&m, 8, 0, w));
    CHECK_NEAR(w[4 * 2 + 0], w[6 * 2 + 0], 1e-7); CHECK(w[4 * 2 + 0] > 0); CHECK_NEAR(w[5 * 2 + 0], 0, 0);
    CHECK_NEAR(w[5 * 2 + 1], w[7 * 2 + 1], 1e-7);
    /* Mono and stereo need no downmix. */
    CHECK(!weights_of(&m, 2, 0, w));
    CHECK(!weights_of(&m, 1, 0, w));
}

/* The levels as the streams carry them: AC-3 (cmixlev / surmixlev), AC-3's
 * alternate syntax (Annex D) and E-AC-3's mixing metadata, written by FFmpeg. */
static void test_downmix_levels_in_streams(void)
{
    static const struct { const char* name; int dmixmod, ltrtc, ltrts, loroc, loros, dialnorm; } k[] = {
        { "dolby/mix_std.ac3", 0, 4, 4, 6, 7, 24 },    /* clev 0.5, slev 0 */
        { "dolby/mix_alt.ac3", 1, 0, 3, 2, 6, 20 },    /* Lt/Rt: 1.414, 0.841; Lo/Ro: 1.0, 0.5 */
        { "dolby/mix.ec3", 2, 5, 6, 3, 4, 27 },        /* Lt/Rt: 0.595, 0.5; Lo/Ro: 0.841, 0.707 */
    };
    size_t i;
    for (i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        fa_stream* s = NULL;
        fa_ac3_mix m;
        float buf[1536 * 6];
        fa_frames got = 0;
        CHECK_EQ(open_file(k[i].name, 0, &s), FA_OK);
        CHECK_EQ(fa_stream_read(s, buf, 1536, &got), FA_OK);
        CHECK(fa_ac3_mix_unpack((uint64_t)fa_load64(&s->dolby_mix), &m));
        CHECK_EQ(m.acmod, 7);
        CHECK_EQ(m.lfeon, 1);
        CHECK_EQ(m.dmixmod, k[i].dmixmod);
        CHECK_EQ(m.ltrtcmix, k[i].ltrtc);
        CHECK_EQ(m.ltrtsurmix, k[i].ltrts);
        CHECK_EQ(m.lorocmix, k[i].loroc);
        CHECK_EQ(m.lorosurmix, k[i].loros);
        CHECK_EQ(m.dialnorm, k[i].dialnorm);
        CHECK_EQ(m.used, 0x3f);                        /* every channel of 5.1 has audio */
        fa_stream_close(s);
    }
}

/* Plays a file on the (not started) null device, this thread being the audio
 * thread: `frames` stereo frames into out. */
static fa_result play_stereo(const char* name, int32_t mode, int32_t switch_to, float* out, int64_t frames)
{
    char path[512];
    fa_open_options o;
    fa_stream* s = NULL;
    int64_t done = 0, d;
    fa_result r;
    float* tmp;
    fa_open_options_init(&o);
    o.dolby_downmix = mode;
    snprintf(path, sizeof(path), "%s/%s", FA_TEST_DATA_DIR, name);
    if ((r = fa_stream_open_path(g_engine, path, &o, &s)) != FA_OK) return r;
    if ((r = fa_stream_attach(s, g_dev, NULL)) != FA_OK || (r = fa_stream_play(s)) != FA_OK) { fa_stream_close(s); return r; }
    if (switch_to >= 0 && (r = fa_stream_set_dolby_downmix(s, (fa_dolby_downmix)switch_to)) != FA_OK) {
        fa_stream_close(s);
        return r;
    }
    d = g_dev->limiter.delay;
    tmp = (float*)malloc((size_t)(frames + d) * 2 * sizeof(float));
    while (done < frames + d) {
        uint32_t n = frames + d - done < 512 ? (uint32_t)(frames + d - done) : 512;
        int64_t deadline = fa_monotonic_ns() + 10000000000LL;   /* a bound for a hang, not a wait */
        while (fa_ring_readable(&s->ring) < (int64_t)n && !fa_load32(&s->eof) && fa_monotonic_ns() < deadline)
            fa_sleep_ms(1);
        fa_fetch_add64(&g_dev->epoch, 1);
        fa_device_render(g_dev, tmp + done * 2, n);
        fa_fetch_add64(&g_dev->epoch, 1);
        done += n;
    }
    memcpy(out, tmp + d * 2, (size_t)frames * 2 * sizeof(float));
    free(tmp);
    fa_stream_close(s);
    return FA_OK;
}

/* Complex amplitude of a `hz` tone in channel c of interleaved pcm (Hann window). */
static void tone(const float* pcm, int channels, int c, int64_t from, int64_t n, double rate, double hz,
                 double* re, double* im)
{
    int64_t i;
    double a = 0, b = 0;
    for (i = 0; i < n; i++) {
        double win = 0.5 - 0.5 * cos(2 * 3.141592653589793 * (double)i / (double)n), t = (double)(from + i) / rate;
        double x = pcm[(from + i) * channels + c] * win;
        a += x * cos(2 * 3.141592653589793 * hz * t);
        b += x * sin(2 * 3.141592653589793 * hz * t);
    }
    *re = a * 4 / (double)n;
    *im = b * 4 / (double)n;
}

static double tone_amp(const float* pcm, int channels, int c, int64_t from, int64_t n, double rate, double hz)
{
    double re, im;
    tone(pcm, channels, c, from, n, rate, hz, &re, &im);
    return sqrt(re * re + im * im);
}

/* What reaches the speakers: each channel's tone (L 500 Hz, R 700, C 900, LFE 60,
 * SL 1100, SR 1300 Hz) at the weight of the stream's levels, against the decoded
 * channels; Lt/Rt has the surrounds out of phase. */
static void test_downmix_on_the_device(void)
{
    static const double hz[6] = { 500, 700, 900, 60, 1100, 1300 };
    enum { N = 9000 };
    float* out = (float*)malloc((size_t)N * 2 * sizeof(float));
    double rate = (double)g_dev->rate, src[6];
    decoded dec;
    fa_ac3_mix m;
    float w[12];
    int c, k;
    struct { const char* name; int32_t mode, switch_to; int ltrt; } runs[] = {
        { "dolby/mix_std.ac3", FA_DOLBY_DOWNMIX_AUTO, -1, 0 },
        { "dolby/mix_alt.ac3", FA_DOLBY_DOWNMIX_AUTO, -1, 1 },     /* prefers Lt/Rt */
        { "dolby/mix_alt.ac3", FA_DOLBY_DOWNMIX_LORO, -1, 0 },
        { "dolby/mix.ec3", FA_DOLBY_DOWNMIX_LTRT, -1, 1 },
        { "dolby/mix.ec3", FA_DOLBY_DOWNMIX_ENGINE, FA_DOLBY_DOWNMIX_AUTO, 0 },   /* rebuilt while attached */
    };
    CHECK(out != NULL);
    for (k = 0; k < (int)(sizeof(runs) / sizeof(runs[0])); k++) {
        CHECK(decode(runs[k].name, 0, &dec));
        CHECK_EQ(dec.info.format.channels, 6);
        for (c = 0; c < 6; c++) src[c] = tone_amp(dec.pcm, 6, c, 3000, 6000, 48000, hz[c]);
        free(dec.pcm);
        {
            fa_stream* s = NULL;
            float buf[1536 * 6];
            fa_frames got = 0;
            CHECK_EQ(open_file(runs[k].name, 0, &s), FA_OK);
            fa_stream_read(s, buf, 1536, &got);
            CHECK(fa_ac3_mix_unpack((uint64_t)fa_load64(&s->dolby_mix), &m));
            fa_stream_close(s);
        }
        CHECK(fa_ac3_downmix(&m, 6, runs[k].ltrt, w, NULL));
        CHECK_EQ(play_stereo(runs[k].name, runs[k].mode, runs[k].switch_to, out, N), FA_OK);
        for (c = 0; c < 6; c++) {
            int64_t from = (int64_t)(3000 * rate / 48000);
            double l = tone_amp(out, 2, 0, from, (int64_t)(6000 * rate / 48000), rate, hz[c]);
            double r = tone_amp(out, 2, 1, from, (int64_t)(6000 * rate / 48000), rate, hz[c]);
            CHECK_NEAR(l, fabs(w[c * 2 + 0]) * src[c], 0.01 * src[c] + 1e-3);
            CHECK_NEAR(r, fabs(w[c * 2 + 1]) * src[c], 0.01 * src[c] + 1e-3);
        }
        if (runs[k].ltrt) {
            /* The surrounds' tone in Lt is the negative of the one in Rt. */
            double lre, lim, rre, rim;
            int64_t from = (int64_t)(3000 * rate / 48000), n = (int64_t)(6000 * rate / 48000);
            tone(out, 2, 0, from, n, rate, 1100, &lre, &lim);
            tone(out, 2, 1, from, n, rate, 1100, &rre, &rim);
            CHECK(lre * rre + lim * rim < 0);
        }
        printf("  %s mode %d: weights C %.3f, SL %.3f / %.3f\n", runs[k].name, runs[k].mode, w[4], w[8], w[9]);
    }
    /* ENGINE keeps the engine's downmix, which ignores the stream's levels: the
     * surrounds that mix_std.ac3 leaves out (slev 0) are heard. */
    CHECK_EQ(play_stereo("dolby/mix_std.ac3", FA_DOLBY_DOWNMIX_ENGINE, -1, out, N), FA_OK);
    CHECK(tone_amp(out, 2, 0, (int64_t)(3000 * rate / 48000), (int64_t)(6000 * rate / 48000), rate, 1100) > 0.05);
    free(out);
}

/* Normalization gives back what the downmix took off to stay below full scale. */
static void test_downmix_normalization_makeup(void)
{
    char path[512];
    int pass;
    snprintf(path, sizeof(path), "%s/dolby/mix_std.ac3", FA_TEST_DATA_DIR);   /* L 1, C 0.5: scaled 1 / 1.5 */
    CHECK_EQ(fa_engine_set_normalization(g_engine, FA_NORMALIZE_TRACK, -16), FA_OK);
    for (pass = 0; pass < 2; pass++) {
        fa_open_options o;
        fa_stream* s = NULL;
        fa_loudness l;
        int64_t deadline = fa_monotonic_ns() + 10000000000LL;
        fa_open_options_init(&o);
        o.dolby_downmix = pass == 0 ? FA_DOLBY_DOWNMIX_AUTO : FA_DOLBY_DOWNMIX_ENGINE;
        CHECK_EQ(fa_stream_open_path(g_engine, path, &o, &s), FA_OK);
        memset(&l, 0, sizeof(l));
        l.integrated_lufs = -20;
        l.range_lu = l.true_peak_dbtp = NAN;
        CHECK_EQ(fa_stream_set_loudness(s, &l), FA_OK);
        CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
        while (fa_ring_readable(&s->ring) == 0 && fa_monotonic_ns() < deadline) fa_sleep_ms(1);
        CHECK_EQ(fa_stream_get_loudness(s, &l), FA_OK);
        if (pass == 0) CHECK_NEAR(l.gain_db, 4.0 + 20 * log10(1.5), 1e-3);
        else CHECK_NEAR(l.gain_db, 4.0, 1e-6);
        fa_stream_close(s);
    }
    fa_engine_set_normalization(g_engine, FA_NORMALIZE_OFF, -16);
}

/* fa_engine_set_dialnorm: the dialogue level stands in for a scan. */
static void test_dialnorm_loudness(void)
{
    char path[512];
    fa_stream* s = NULL;
    fa_loudness l;
    int64_t deadline;
    int pass;
    for (pass = 0; pass < 2; pass++) {
        CHECK_EQ(fa_engine_set_dialnorm(g_engine, pass == 0), FA_OK);
        snprintf(path, sizeof(path), "%s/dolby/mix_alt.ac3", FA_TEST_DATA_DIR);
        CHECK_EQ(fa_stream_open_path(g_engine, path, NULL, &s), FA_OK);
        deadline = fa_monotonic_ns() + 20000000000LL;
        do {
            CHECK_EQ(fa_stream_get_loudness(s, &l), FA_OK);
            if (!l.is_final) fa_sleep_ms(2);
        } while (!l.is_final && fa_monotonic_ns() < deadline);
        fa_stream_close(s);
        CHECK(l.is_final);
        if (pass == 0) {
            CHECK_EQ(l.source, FA_LOUDNESS_DIALNORM);
            CHECK_NEAR(l.integrated_lufs, -20.0, 0);
        } else {
            CHECK_EQ(l.source, FA_LOUDNESS_SCAN);    /* off: measured */
        }
    }
}

static void test_registry_lists_dolby(void)
{
    uint32_t i, n = fa_format_count();
    int found = 0;
    for (i = 0; i < n; i++) {
        fa_format_info fi;
        CHECK_EQ(fa_format_get_info(i, &fi), FA_OK);
        if (strcmp(fi.name, "ac3") == 0) {
            found = 1;
            CHECK(strstr(fi.extensions, "ec3") != NULL);
        }
    }
    CHECK(found);
}

int main(void)
{
    fa_engine_config cfg;
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    cfg.worker_threads = 2;
    if (fa_engine_create(&cfg, &g_engine) != FA_OK) { fprintf(stderr, "engine create failed\n"); return 1; }
    if (fa_device_open(g_engine, NULL, &g_dev) != FA_OK) { fprintf(stderr, "device open failed\n"); return 1; }

    RUN(test_registry_lists_dolby);
    RUN(test_matches_ffmpeg);
    RUN(test_tools_ffmpeg_lacks);
    RUN(test_dynamic_range_modes);
    RUN(test_seek_is_exact);
    RUN(test_tones_in_containers);
    RUN(test_raw_cbr_and_index);
    RUN(test_damaged_frames);
    RUN(test_damaged_dependent_frame);
    RUN(test_dependent_frames_in_separate_packets);
    RUN(test_layout_change_inside_a_packet);
    RUN(test_garbage_does_not_crash);
    RUN(test_downmix_coefficients);
    RUN(test_downmix_levels_in_streams);
    RUN(test_downmix_on_the_device);
    RUN(test_downmix_normalization_makeup);
    RUN(test_dialnorm_loudness);

    fa_engine_destroy(g_engine);
    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    printf("PASS\n");
    return 0;
}
