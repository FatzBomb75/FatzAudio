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

    RUN(test_registry_lists_dolby);
    RUN(test_matches_ffmpeg);
    RUN(test_dynamic_range_modes);
    RUN(test_seek_is_exact);
    RUN(test_tones_in_containers);
    RUN(test_damaged_frames);
    RUN(test_damaged_dependent_frame);
    RUN(test_dependent_frames_in_separate_packets);
    RUN(test_layout_change_inside_a_packet);
    RUN(test_garbage_does_not_crash);

    fa_engine_destroy(g_engine);
    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    printf("PASS\n");
    return 0;
}
