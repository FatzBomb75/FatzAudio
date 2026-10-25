/* Track analysis: beat grids, keys, waveform overview, loudness, the analysis
 * cache, exact grids of MIDI files and tracker modules, and a stream's grid.
 *
 * Ground truth comes from audio made here: drum patterns synthesized sample by
 * sample, and MIDI songs (known tempo and key) rendered by our own synthesizer
 * with a soundfont built here. With FA_ANALYSIS_REPORT_SF2 set to a General MIDI
 * soundfont, `fa_test_analysis` instead renders a set of songs with it and prints
 * the accuracy report (docs/features/analysis.md). */
#include "fatzaudio/fatzaudio.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#  include <direct.h>
#  include <io.h>
#else
#  include <dirent.h>
#  include <unistd.h>
#endif

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; return; } } while (0)
#define CHECK_EQ(a, b) do { long long va_ = (long long)(a), vb_ = (long long)(b); if (va_ != vb_) { \
    fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_failures++; return; } } while (0)
#define CHECK_NEAR(a, b, tol) do { double va_ = (a), vb_ = (b); if (fabs(va_ - vb_) > (tol)) { \
    fprintf(stderr, "  FAIL %s:%d: %s ~ %s (%.6f vs %.6f)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_failures++; return; } } while (0)
#define CHECK_STR(a, b) do { if (strcmp((a), (b)) != 0) { \
    fprintf(stderr, "  FAIL %s:%d: %s == \"%s\" (got \"%s\")\n", __FILE__, __LINE__, #a, (b), (a)); g_failures++; return; } } while (0)
#define RUN(t) do { const char* only_ = getenv("FA_TEST"); int before_ = g_failures; \
    if (only_ && strcmp(only_, #t) != 0) break; \
    printf("%s\n", #t); t(); if (g_failures == before_) printf("  ok\n"); } while (0)

#define RATE 44100
#define PI 3.14159265358979323846

static fa_engine* g_engine;
static fa_soundfont* g_font;

/* ---- byte buffers ------------------------------------------------------------- */

typedef struct buf { uint8_t* d; size_t n, cap; } buf;

static void put(buf* b, const void* p, size_t n)
{
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 64;
        b->d = (uint8_t*)realloc(b->d, b->cap);
    }
    memcpy(b->d + b->n, p, n);
    b->n += n;
}
static void put8(buf* b, unsigned v) { uint8_t x = (uint8_t)v; put(b, &x, 1); }
static void put16le(buf* b, unsigned v) { put8(b, v & 255); put8(b, v >> 8); }
static void put32le(buf* b, uint32_t v) { put16le(b, v & 0xFFFF); put16le(b, v >> 16); }
static void put16be(buf* b, unsigned v) { put8(b, (v >> 8) & 255); put8(b, v & 255); }
static void put32be(buf* b, uint32_t v) { put8(b, v >> 24); put8(b, (v >> 16) & 255); put8(b, (v >> 8) & 255); put8(b, v & 255); }
static void putstr(buf* b, const char* s, size_t n) { size_t k = strlen(s); put(b, s, k < n ? k : n); while (k++ < n) put8(b, 0); }

static void chunk(buf* b, const char* id, const buf* body)
{
    put(b, id, 4);
    put32le(b, (uint32_t)body->n);
    put(b, body->d, body->n);
    if (body->n & 1) put8(b, 0);
}

/* ---- WAV in memory, and analysing it ---------------------------------------------- */

/* 32-bit float WAV of `frames` frames of `ch` channels. */
static buf make_wav(const float* x, int64_t frames, int ch)
{
    buf w = { 0 };
    uint32_t data = (uint32_t)(frames * ch * 4);
    put(&w, "RIFF", 4); put32le(&w, 36 + data); put(&w, "WAVE", 4);
    put(&w, "fmt ", 4); put32le(&w, 16); put16le(&w, 3); put16le(&w, (unsigned)ch);
    put32le(&w, RATE); put32le(&w, (uint32_t)(RATE * ch * 4)); put16le(&w, (unsigned)(ch * 4)); put16le(&w, 32);
    put(&w, "data", 4); put32le(&w, data);
    put(&w, x, (size_t)data);
    return w;
}

static fa_stream* open_mem(const buf* b, fa_soundfont* font)
{
    fa_open_options o;
    fa_stream* s = NULL;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    o.soundfont = font;
    if (fa_stream_open_memory(g_engine, b->d, b->n, &o, &s) != FA_OK) return NULL;
    return s;
}

static fa_analysis* analyze_ex(const buf* b, uint32_t kinds, float min_bpm, float max_bpm, uint32_t flags, fa_result* out_r)
{
    fa_analysis_options o;
    fa_analysis* a = NULL;
    fa_stream* s = open_mem(b, g_font);
    fa_result r;
    fa_analysis_options_init(&o);
    o.kinds = kinds;
    if (min_bpm > 0) { o.min_bpm = min_bpm; o.max_bpm = max_bpm; }
    o.flags = flags | FA_ANALYZE_NO_CACHE;
    r = s ? fa_analyze_stream(s, &o, &a) : FA_ERROR_GENERIC;
    if (out_r) *out_r = r;
    fa_stream_close(s);
    return a;
}

static fa_analysis* analyze(const buf* b, uint32_t kinds) { return analyze_ex(b, kinds, 0, 0, 0, NULL); }

static int get_grid(const fa_analysis* a, fa_beat_grid* g)
{
    memset(g, 0, sizeof(*g));
    g->struct_size = sizeof(*g);
    return fa_analysis_get_beat_grid(a, g) == FA_OK;
}

static int get_key(const fa_analysis* a, fa_key_info* k)
{
    memset(k, 0, sizeof(*k));
    k->struct_size = sizeof(*k);
    return fa_analysis_get_key(a, k) == FA_OK;
}

/* How far (ms) the grid's beat lines are from the true ones (period 60/bpm s, a
 * beat at `phase` s), at the grid's first beat and `at` seconds later. */
static double beat_error_ms(const fa_beat_grid* g, double true_bpm, double phase, double at)
{
    double p = 60.0 / true_bpm, t = (double)g->first_beat / g->sample_rate, e1, e2, t2;
    e1 = fmod(t - phase, p);
    if (e1 < 0) e1 += p;
    if (e1 > p / 2) e1 -= p;
    t2 = t + floor(at / (60.0 / g->bpm) + 0.5) * (60.0 / g->bpm);
    e2 = fmod(t2 - phase, p);
    if (e2 < 0) e2 += p;
    if (e2 > p / 2) e2 -= p;
    return (fabs(e1) > fabs(e2) ? fabs(e1) : fabs(e2)) * 1000.0;
}

/* ---- synthesized drums --------------------------------------------------------- */

static uint32_t g_rand = 12345;
static double frand(void) { g_rand = g_rand * 1664525u + 1013904223u; return (double)(g_rand >> 8) / 16777216.0; }

/* A kick (a falling sine with a fast decay) at time t. */
static void add_kick(float* x, int64_t n, double t, float gain)
{
    int64_t s = (int64_t)llround(t * RATE), i;
    double ph = 0.0;
    for (i = 0; i < RATE / 4 && s + i < n; i++) {
        double tt = (double)i / RATE, f = 50.0 + 100.0 * exp(-tt * 30.0);
        if (s + i < 0) continue;
        ph += 2.0 * PI * f / RATE;
        x[s + i] += gain * (float)(sin(ph) * exp(-tt * 12.0));
    }
}

/* A closed hi-hat: a short noise burst. */
static void add_hat(float* x, int64_t n, double t, float gain)
{
    int64_t s = (int64_t)llround(t * RATE), i;
    double prev = 0.0;
    for (i = 0; i < RATE / 20 && s + i < n; i++) {
        double v = frand() * 2.0 - 1.0, hp = v - prev;
        prev = v;
        if (s + i < 0) continue;
        x[s + i] += gain * (float)(hp * 0.5 * exp(-(double)i / RATE * 80.0));
    }
}

/* Kicks on every beat, hats on the off-beats, from `phase` s, mono. */
static float* drums(double bpm, double phase, double seconds, int64_t* out_n)
{
    int64_t n = (int64_t)(seconds * RATE);
    float* x = (float*)calloc((size_t)n, sizeof(float));
    double p = 60.0 / bpm, t;
    for (t = phase; t < seconds; t += p) {
        add_kick(x, n, t, 0.6f);
        add_hat(x, n, t + p / 2, 0.25f);
    }
    *out_n = n;
    return x;
}

/* ---- soundfont and MIDI ----------------------------------------------------------- */

#define SINE_LEN 4410   /* 100 periods of 440 Hz at 44.1 kHz: loops seamlessly */

static void gen(buf* b, unsigned oper, unsigned amount) { put16le(b, oper); put16le(b, amount); }

/* Preset 0 bank 0 and preset 0 bank 128: a looped 440 Hz sine, root key 69, a 2 ms
 * attack, decaying to -20 dB over a second (so notes have onsets), 125 ms release. */
static buf make_sf2(void)
{
    buf sf = { 0 }, riff = { 0 }, info = { 0 }, sdta = { 0 }, pdta = { 0 }, c = { 0 };
    int i, z;
    put(&riff, "sfbk", 4);
    put(&info, "INFO", 4);
    c.n = 0; put16le(&c, 2); put16le(&c, 1); chunk(&info, "ifil", &c);
    c.n = 0; putstr(&c, "Test Sine", 10); chunk(&info, "INAM", &c);
    chunk(&riff, "LIST", &info);
    put(&sdta, "sdta", 4);
    c.n = 0;
    for (i = 0; i < SINE_LEN; i++) put16le(&c, (unsigned)(uint16_t)(int16_t)lrint(sin(2.0 * PI * 440.0 * i / RATE) * 32000.0));
    for (i = 0; i < 46; i++) put16le(&c, 0);
    chunk(&sdta, "smpl", &c);
    chunk(&riff, "LIST", &sdta);
    put(&pdta, "pdta", 4);
    c.n = 0;
    putstr(&c, "Sine", 20); put16le(&c, 0); put16le(&c, 0); put16le(&c, 0); put32le(&c, 0); put32le(&c, 0); put32le(&c, 0);
    putstr(&c, "Kit", 20); put16le(&c, 0); put16le(&c, 128); put16le(&c, 1); put32le(&c, 0); put32le(&c, 0); put32le(&c, 0);
    putstr(&c, "EOP", 20); put16le(&c, 0); put16le(&c, 0); put16le(&c, 2); put32le(&c, 0); put32le(&c, 0); put32le(&c, 0);
    chunk(&pdta, "phdr", &c);
    c.n = 0; put16le(&c, 0); put16le(&c, 0); put16le(&c, 1); put16le(&c, 0); put16le(&c, 2); put16le(&c, 0);
    chunk(&pdta, "pbag", &c);
    c.n = 0; for (i = 0; i < 10; i++) put8(&c, 0);
    chunk(&pdta, "pmod", &c);
    c.n = 0; gen(&c, 41, 0); gen(&c, 41, 1); gen(&c, 0, 0);
    chunk(&pdta, "pgen", &c);
    c.n = 0;
    putstr(&c, "SineInst", 20); put16le(&c, 0);
    putstr(&c, "KitInst", 20); put16le(&c, 1);
    putstr(&c, "EOI", 20); put16le(&c, 2);
    chunk(&pdta, "inst", &c);
    c.n = 0; put16le(&c, 0); put16le(&c, 0); put16le(&c, 6); put16le(&c, 0); put16le(&c, 12); put16le(&c, 0);
    chunk(&pdta, "ibag", &c);
    c.n = 0; for (i = 0; i < 10; i++) put8(&c, 0);
    chunk(&pdta, "imod", &c);
    c.n = 0;
    for (z = 0; z < 2; z++) {
        gen(&c, 54, 1);                          /* sampleModes: loop */
        gen(&c, 34, (unsigned)(uint16_t)-10800); /* attackVolEnv: about 2 ms */
        gen(&c, 36, 0);                          /* decayVolEnv: 1 s */
        gen(&c, 37, 200);                        /* sustainVolEnv: 20 dB down */
        gen(&c, 38, (unsigned)(uint16_t)-3600);  /* releaseVolEnv: 2^-3 s */
        gen(&c, 53, 0);                          /* sampleID */
    }
    gen(&c, 0, 0);
    chunk(&pdta, "igen", &c);
    c.n = 0;
    putstr(&c, "Sine", 20); put32le(&c, 0); put32le(&c, SINE_LEN); put32le(&c, 0); put32le(&c, SINE_LEN);
    put32le(&c, RATE); put8(&c, 69); put8(&c, 0); put16le(&c, 0); put16le(&c, 1);
    putstr(&c, "EOS", 20); for (i = 0; i < 26; i++) put8(&c, 0);
    chunk(&pdta, "shdr", &c);
    chunk(&riff, "LIST", &pdta);
    chunk(&sf, "RIFF", &riff);
    free(riff.d); free(info.d); free(sdta.d); free(pdta.d); free(c.d);
    return sf;
}

static void varlen(buf* b, uint32_t v)
{
    uint8_t tmp[5];
    int n = 0;
    tmp[n++] = v & 0x7F;
    while (v >>= 7) tmp[n++] = (uint8_t)(0x80 | (v & 0x7F));
    while (n--) put8(b, tmp[n]);
}
static void tempo_ev(buf* t, uint32_t delta, uint32_t us)
{
    varlen(t, delta); put8(t, 0xFF); put8(t, 0x51); put8(t, 3); put8(t, us >> 16); put8(t, (us >> 8) & 255); put8(t, us & 255);
}
static void timesig_ev(buf* t, uint32_t delta, unsigned num, unsigned den_log2)
{
    varlen(t, delta); put8(t, 0xFF); put8(t, 0x58); put8(t, 4); put8(t, num); put8(t, den_log2); put8(t, 24); put8(t, 8);
}
static void eot(buf* t, uint32_t delta) { varlen(t, delta); put8(t, 0xFF); put8(t, 0x2F); put8(t, 0); }

/* MIDI events gathered at absolute ticks, then written as one format-0 track. */
typedef struct mev { uint32_t tick; uint8_t d[3]; int n; } mev;
typedef struct song { mev* e; int n, cap; } song;

static void add_ev(song* s, uint32_t tick, int n, unsigned a, unsigned b, unsigned c)
{
    if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 256; s->e = (mev*)realloc(s->e, (size_t)s->cap * sizeof(mev)); }
    s->e[s->n].tick = tick;
    s->e[s->n].d[0] = (uint8_t)a; s->e[s->n].d[1] = (uint8_t)b; s->e[s->n].d[2] = (uint8_t)c;
    s->e[s->n].n = n;
    s->n++;
}
static void note(song* s, uint32_t tick, uint32_t len, int ch, int key, int vel)
{
    add_ev(s, tick, 3, 0x90 | ch, key, vel);
    add_ev(s, tick + len, 3, 0x80 | ch, key, 0);
}

static int cmp_ev(const void* a, const void* b)
{
    const mev* x = (const mev*)a;
    const mev* y = (const mev*)b;
    if (x->tick != y->tick) return x->tick < y->tick ? -1 : 1;
    /* note offs first */
    return (int)((x->d[0] & 0xF0) == 0x90) - (int)((y->d[0] & 0xF0) == 0x90);
}

/* 480 ticks per beat, tempo `bpm`; the events of `s`. */
static buf write_song(song* s, double bpm, const uint32_t* tempo_ticks, const double* tempos, int ntempo, int num, int den_log2)
{
    buf f = { 0 }, t = { 0 };
    uint32_t last = 0;
    int i, k = 0;
    qsort(s->e, (size_t)s->n, sizeof(mev), cmp_ev);
    if (num) timesig_ev(&t, 0, (unsigned)num, (unsigned)den_log2);
    tempo_ev(&t, 0, (uint32_t)llround(60000000.0 / bpm));
    for (i = 0; i < s->n; i++) {
        while (k < ntempo && tempo_ticks[k] <= s->e[i].tick) {
            tempo_ev(&t, tempo_ticks[k] - last, (uint32_t)llround(60000000.0 / tempos[k]));
            last = tempo_ticks[k++];
        }
        varlen(&t, s->e[i].tick - last);
        put(&t, s->e[i].d, (size_t)s->e[i].n);
        last = s->e[i].tick;
    }
    eot(&t, 480);
    put(&f, "MThd", 4); put32be(&f, 6); put16be(&f, 0); put16be(&f, 1); put16be(&f, 480);
    put(&f, "MTrk", 4); put32be(&f, (uint32_t)t.n); put(&f, t.d, t.n);
    free(t.d);
    return f;
}

/* Renders a MIDI file through FatzAudio's synthesizer into a stereo WAV. */
static buf render(const buf* midi, fa_soundfont* font, double* out_seconds)
{
    fa_stream* s = open_mem(midi, font);
    float* x = NULL;
    int64_t n = 0, cap = 0;
    buf w = { 0 };
    if (!s) return w;
    for (;;) {
        fa_frames got = 0;
        if (n + 4096 > cap) { cap = (n + 4096) * 2; x = (float*)realloc(x, (size_t)cap * 2 * sizeof(float)); }
        if (fa_stream_read(s, x + n * 2, 4096, &got) != FA_OK || got == 0) break;
        n += got;
    }
    fa_stream_close(s);
    w = make_wav(x, n, 2);
    if (out_seconds) *out_seconds = (double)n / RATE;
    free(x);
    return w;
}

/* Chords (I IV V I, or i iv V i in harmonic minor), one per bar, in the octave
 * from C4; a root bass note on every beat; with `kick`, drum notes (channel 10)
 * on every beat. */
static void chords(song* s, int tonic, int minor, int bars, int kick)
{
    static const int major_prog[4][3] = { { 0, 4, 7 }, { 5, 9, 12 }, { 7, 11, 14 }, { 0, 4, 7 } };
    static const int minor_prog[4][3] = { { 0, 3, 7 }, { 5, 8, 12 }, { 7, 11, 14 }, { 0, 3, 7 } };
    int bar, b, i;
    for (bar = 0; bar < bars; bar++) {
        const int* c = minor ? minor_prog[bar % 4] : major_prog[bar % 4];
        for (i = 0; i < 3; i++) note(s, (uint32_t)bar * 1920, 1900, 0, 60 + tonic + c[i], 90);
        for (b = 0; b < 4; b++) {
            note(s, (uint32_t)(bar * 1920 + b * 480), 200, 1, 36 + tonic + c[0], 100);
            if (kick) note(s, (uint32_t)(bar * 1920 + b * 480), 60, 9, 36, 127);
        }
    }
}

/* ---- tests ------------------------------------------------------------------------ */

static void test_key_names(void)
{
    static const char* const names[24] = { "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B",
                                           "Cm", "C#m", "Dm", "Ebm", "Em", "Fm", "F#m", "Gm", "G#m", "Am", "Bbm", "Bm" };
    static const char* const camelot[24] = { "8B", "3B", "10B", "5B", "12B", "7B", "2B", "9B", "4B", "11B", "6B", "1B",
                                             "5A", "12A", "7A", "2A", "9A", "4A", "11A", "6A", "1A", "8A", "3A", "10A" };
    fa_key_info k, t;
    int i;
    for (i = 0; i < 24; i++) {
        k.struct_size = sizeof(k);
        CHECK(fa_key_make(i % 12, i / 12, &k) == FA_OK);
        CHECK_STR(k.name, names[i]);
        CHECK_STR(k.camelot, camelot[i]);
        CHECK_NEAR(k.confidence, 1.0, 0.0);
    }
    k.struct_size = sizeof(k);
    fa_key_make(9, 1, &k);                     /* A minor */
    k.confidence = 0.5f;
    t.struct_size = sizeof(t);
    CHECK(fa_key_transpose(&k, -2, &t) == FA_OK);
    CHECK_STR(t.name, "Gm");
    CHECK_STR(t.camelot, "6A");
    CHECK_NEAR(t.confidence, 0.5, 1e-6);
    CHECK(fa_key_transpose(&k, 27, &t) == FA_OK);   /* up 3: C minor */
    CHECK_STR(t.name, "Cm");
    CHECK(fa_key_transpose(&k, -25, &t) == FA_OK);  /* down 1: G# minor */
    CHECK_STR(t.name, "G#m");
    CHECK(fa_key_transpose(&k, 5, &k) == FA_OK);    /* in place: D minor */
    CHECK_STR(k.name, "Dm");
    CHECK(fa_key_make(12, 0, &k) == FA_ERROR_INVALID_ARG);
    CHECK(fa_key_make(0, 2, &k) == FA_ERROR_INVALID_ARG);
    CHECK(fa_key_transpose(NULL, 1, &k) == FA_ERROR_INVALID_ARG);
}

/* A steady drum pattern at several tempos: tempo within 0.05 BPM, beats within
 * 10 ms at the start and at the end of a minute. */
static void test_drum_grids(void)
{
    static const double tempos[] = { 75.0, 98.5, 123.4, 128.0, 140.0, 174.0 };
    size_t i;
    for (i = 0; i < sizeof(tempos) / sizeof(tempos[0]); i++) {
        int64_t n;
        float* x = drums(tempos[i], 0.25, 60.0, &n);
        buf w = make_wav(x, n, 1);
        fa_analysis* a = analyze(&w, FA_ANALYZE_BEATS);
        fa_beat_grid g;
        fa_tempo_segment seg[2];
        uint32_t count = 0;
        free(x);
        free(w.d);
        CHECK(a != NULL);
        CHECK(get_grid(a, &g));
        printf("  %.2f BPM: %.4f, first beat %.4f s, confidence %.2f, error %.1f ms\n", tempos[i], g.bpm,
               (double)g.first_beat / RATE, g.confidence, beat_error_ms(&g, tempos[i], 0.25, 55.0));
        CHECK_NEAR(g.bpm, tempos[i], 0.05);
        CHECK(beat_error_ms(&g, tempos[i], 0.25, 55.0) < 10.0);
        CHECK(g.confidence > 0.7f);
        CHECK(g.flags & FA_GRID_CONSTANT);
        CHECK(!(g.flags & FA_GRID_EXACT));
        CHECK_EQ(g.segment_count, 1);
        CHECK_EQ(g.sample_rate, RATE);
        CHECK(fa_analysis_get_tempo_map(a, seg, 2, &count) == FA_OK);
        CHECK_EQ(count, 1);
        CHECK_EQ(seg[0].start, g.first_beat);
        CHECK_NEAR(seg[0].bpm, g.bpm, 0.0);
        fa_analysis_free(a);
    }
}

/* A tempo outside the preferred range is folded into it; a range that holds it keeps it. */
static void test_tempo_range(void)
{
    int64_t n;
    float* x = drums(200.0, 0.1, 40.0, &n);
    buf w = make_wav(x, n, 1);
    fa_analysis* a = analyze(&w, FA_ANALYZE_BEATS);
    fa_beat_grid g;
    fa_result r;
    free(x);
    CHECK(a && get_grid(a, &g));
    CHECK_NEAR(g.bpm, 100.0, 0.05);
    fa_analysis_free(a);
    a = analyze_ex(&w, FA_ANALYZE_BEATS, 100.0f, 220.0f, 0, NULL);
    CHECK(a && get_grid(a, &g));
    CHECK_NEAR(g.bpm, 200.0, 0.05);
    fa_analysis_free(a);
    a = analyze_ex(&w, FA_ANALYZE_BEATS, 100.0f, 150.0f, 0, &r);   /* narrower than an octave */
    CHECK(a == NULL && r == FA_ERROR_INVALID_ARG);
    free(w.d);
}

/* MIDI renders: the tempo the file says, beats on its quarter notes. */
static void test_midi_render_grid(void)
{
    static const double tempos[] = { 92.0, 117.0, 150.0 };
    size_t i;
    for (i = 0; i < sizeof(tempos) / sizeof(tempos[0]); i++) {
        song s = { 0 };
        buf m, w;
        fa_analysis* a;
        fa_beat_grid g;
        double secs;
        chords(&s, 9, 1, 24, 1);
        m = write_song(&s, tempos[i], NULL, NULL, 0, 0, 0);
        w = render(&m, g_font, &secs);
        a = analyze(&w, FA_ANALYZE_BEATS);
        free(s.e); free(m.d); free(w.d);
        CHECK(a && get_grid(a, &g));
        printf("  %.0f BPM: %.4f, first beat %.4f s, confidence %.2f, error %.1f ms\n", tempos[i], g.bpm,
               (double)g.first_beat / RATE, g.confidence, beat_error_ms(&g, tempos[i], 0.0, secs - 2.0));
        CHECK_NEAR(g.bpm, tempos[i], 0.05);
        CHECK(beat_error_ms(&g, tempos[i], 0.0, secs - 2.0) < 10.0);
        fa_analysis_free(a);
    }
}

/* MIDI renders in several keys, major and minor. */
static void test_midi_render_key(void)
{
    static const int keys[][2] = { { 0, 0 }, { 9, 1 }, { 6, 1 }, { 3, 0 }, { 7, 0 }, { 4, 1 }, { 10, 0 }, { 1, 1 } };
    size_t i;
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        song s = { 0 };
        buf m, w;
        fa_analysis* a;
        fa_key_info k, want;
        chords(&s, keys[i][0], keys[i][1], 8, 0);
        m = write_song(&s, 100.0, NULL, NULL, 0, 0, 0);
        w = render(&m, g_font, NULL);
        a = analyze(&w, FA_ANALYZE_KEY);
        free(s.e); free(m.d); free(w.d);
        want.struct_size = sizeof(want);
        fa_key_make(keys[i][0], keys[i][1], &want);
        CHECK(a && get_key(a, &k));
        printf("  %s: %s (%s), confidence %.2f\n", want.name, k.name, k.camelot, k.confidence);
        CHECK_STR(k.name, want.name);
        CHECK(k.confidence > 0.3f);
        fa_analysis_free(a);
    }
}

/* Exact grids of MIDI files: tempo changes as segments, bars from the signature. */
static void test_midi_exact_grid(void)
{
    static const uint32_t ticks[] = { 480 * 8 };
    static const double tempos[] = { 90.0 };
    song s = { 0 };
    buf m, m2;
    fa_analysis* a;
    fa_beat_grid g;
    fa_tempo_segment seg[4];
    uint32_t count = 0;
    fa_stream* st;
    chords(&s, 0, 0, 8, 1);
    m = write_song(&s, 120.0, ticks, tempos, 1, 3, 2);
    a = analyze(&m, FA_ANALYZE_BEATS | FA_ANALYZE_KEY);
    CHECK(a && get_grid(a, &g));
    CHECK(g.flags & FA_GRID_EXACT);
    CHECK(g.flags & FA_GRID_BARS);
    CHECK(!(g.flags & FA_GRID_CONSTANT));
    CHECK_EQ(g.beats_per_bar, 3);
    CHECK_EQ(g.first_beat, 0);
    CHECK_NEAR(g.confidence, 1.0, 0.0);
    CHECK_EQ(g.segment_count, 2);
    CHECK(fa_analysis_get_tempo_map(a, seg, 4, &count) == FA_OK);
    CHECK_EQ(count, 2);
    CHECK_EQ(seg[0].start, 0);
    CHECK_NEAR(seg[0].bpm, 120.0, 1e-9);
    CHECK_EQ(seg[1].start, 4 * RATE);          /* 8 beats at 120 */
    CHECK_NEAR(seg[1].bpm, 60e6 / 666667.0, 1e-9);   /* the file's microseconds per beat */
    CHECK_NEAR(seg[1].beat, 8.0, 1e-9);
    CHECK_NEAR(g.bpm, seg[1].bpm, 0.0);        /* the tempo that lasts longest */
    fa_analysis_free(a);

    /* The stream has it too, without an analysis. */
    st = open_mem(&m, g_font);
    CHECK(st != NULL);
    memset(&g, 0, sizeof(g));
    g.struct_size = sizeof(g);
    CHECK(fa_stream_get_beat_grid(st, &g, seg, 4, &count) == FA_OK);
    CHECK_EQ(count, 2);
    CHECK(g.flags & FA_GRID_EXACT);
    CHECK_EQ(seg[1].start, 4 * RATE);
    fa_stream_close(st);
    free(m.d);

    /* No signature: 4/4, bars known; one tempo: constant. */
    m2 = write_song(&s, 100.0, NULL, NULL, 0, 0, 0);
    a = analyze(&m2, FA_ANALYZE_BEATS);
    CHECK(a && get_grid(a, &g));
    CHECK(g.flags & FA_GRID_CONSTANT);
    CHECK(g.flags & FA_GRID_BARS);
    CHECK_EQ(g.beats_per_bar, 4);
    CHECK_NEAR(g.bpm, 100.0, 1e-9);
    fa_analysis_free(a);

    /* SMPTE timing (25 fps, 40 ticks a frame) has no tempo: no exact grid, the
     * render is listened to instead. */
    m2.d[12] = 0xE7;
    m2.d[13] = 40;
    a = analyze(&m2, FA_ANALYZE_BEATS);
    CHECK(a != NULL);
    if (a && get_grid(a, &g)) CHECK(!(g.flags & FA_GRID_EXACT));
    fa_analysis_free(a);
    st = open_mem(&m2, g_font);
    CHECK(st != NULL);
    CHECK(fa_stream_get_beat_grid(st, &g, NULL, 0, &count) == FA_ERROR_NOT_FOUND);
    fa_stream_close(st);
    free(m2.d);
    free(s.e);
}

/* ---- tracker modules ---- */

static void cell(uint8_t* p, unsigned sample, unsigned period, unsigned fx, unsigned param)
{
    p[0] = (uint8_t)((sample & 0xF0) | (period >> 8));
    p[1] = (uint8_t)(period & 0xFF);
    p[2] = (uint8_t)(((sample & 0x0F) << 4) | fx);
    p[3] = (uint8_t)param;
}

/* Two 64-row patterns: speed 6 at tempo 125 (125 BPM at 4 rows a beat), then
 * tempo 150; a note on every beat. MOD files change the tempo on a row's second
 * tick, as ProTracker did, so the row that sets it has one tick at the old tempo:
 * the new tempo holds from row 65 (beat 16.25), 882 + 5 x 735 frames after row 64. */
static buf make_mod(void)
{
    buf m = { 0 };
    static uint8_t pat[2][64 * 4 * 4];
    int i, p, row;
    putstr(&m, "Grid Module", 20);
    for (i = 0; i < 31; i++) {
        putstr(&m, i == 0 ? "Click" : "", 22);
        put16be(&m, i == 0 ? 256 : 0);
        put8(&m, 0);
        put8(&m, i == 0 ? 64 : 0);
        put16be(&m, 0);
        put16be(&m, 1);
    }
    put8(&m, 2);
    put8(&m, 127);
    put8(&m, 0);
    put8(&m, 1);
    for (i = 2; i < 128; i++) put8(&m, 0);
    put(&m, "M.K.", 4);
    memset(pat, 0, sizeof(pat));
    for (p = 0; p < 2; p++) {
        for (row = 0; row < 64; row += 4) cell(pat[p] + (row * 4 + 0) * 4, 1, 214, 0, 0);
        cell(pat[p] + (0 * 4 + 3) * 4, 0, 0, 0xF, p == 0 ? 6 : 150);
    }
    put(&m, pat, sizeof(pat));
    for (i = 0; i < 512; i++) put8(&m, (unsigned)(uint8_t)(int8_t)lrint(sin(2.0 * PI * i / 16) * 120.0 * exp(-i / 100.0)));
    return m;
}

static void test_tracker_exact_grid(void)
{
    buf m = make_mod();
    fa_analysis* a = analyze(&m, FA_ANALYZE_BEATS);
    fa_beat_grid g;
    fa_tempo_segment seg[4];
    uint32_t count = 0;
    fa_stream* st;
    CHECK(a && get_grid(a, &g));
    CHECK(g.flags & FA_GRID_EXACT);
    CHECK_EQ(g.segment_count, 2);
    CHECK(fa_analysis_get_tempo_map(a, seg, 4, &count) == FA_OK);
    CHECK_EQ(seg[0].start, 0);
    CHECK_NEAR(seg[0].bpm, 125.0, 1e-6);
    CHECK_NEAR(seg[1].start, 64 * 5292 + 882 + 5 * 735, 2);   /* 64 rows of 6 ticks of 882 frames, then row 64 */
    CHECK_NEAR(seg[1].bpm, 150.0, 1e-6);
    CHECK_NEAR(seg[1].beat, 16.25, 1e-9);
    fa_analysis_free(a);

    st = open_mem(&m, NULL);
    CHECK(st != NULL);
    memset(&g, 0, sizeof(g));
    g.struct_size = sizeof(g);
    CHECK(fa_stream_get_beat_grid(st, &g, seg, 4, &count) == FA_OK);
    CHECK_EQ(count, 2);
    CHECK_NEAR(seg[1].start, 64 * 5292 + 882 + 5 * 735, 2);
    fa_stream_close(st);
    free(m.d);
}

/* Silence has neither beat nor key; the overview is there, all zero. */
static void test_silence(void)
{
    int64_t n = 20 * RATE;
    float* x = (float*)calloc((size_t)n, sizeof(float));
    buf w = make_wav(x, n, 1);
    fa_analysis* a = analyze(&w, FA_ANALYZE_BEATS | FA_ANALYZE_KEY | FA_ANALYZE_WAVEFORM);
    fa_beat_grid g;
    fa_key_info k;
    fa_analysis_info info;
    fa_waveform_point p[4];
    uint32_t count = 0;
    free(x);
    free(w.d);
    CHECK(a != NULL);
    CHECK(!get_grid(a, &g));
    CHECK(!get_key(a, &k));
    CHECK(fa_analysis_get_waveform(a, p, 4, &count) == FA_OK);
    CHECK_EQ(count, 2000);
    CHECK_NEAR(p[0].peak + p[3].low + p[2].high, 0.0, 0.0);
    CHECK(fa_analysis_get_loudness(a, NULL) == FA_ERROR_INVALID_ARG);
    info.struct_size = sizeof(info);
    CHECK(fa_analysis_get_info(a, &info) == FA_OK);
    CHECK_EQ(info.kinds, FA_ANALYZE_BEATS | FA_ANALYZE_KEY | FA_ANALYZE_WAVEFORM);
    CHECK_EQ(info.length, n);
    CHECK_EQ(info.sample_rate, RATE);
    CHECK(!(info.flags & FA_ANALYSIS_FROM_CACHE));
    fa_analysis_free(a);
}

/* The overview: a sine in each band, one second each. */
static void test_waveform(void)
{
    static const double hz[3] = { 80.0, 1000.0, 8000.0 };
    int64_t n = 3 * RATE, i;
    float* x = (float*)malloc((size_t)n * 2 * sizeof(float));
    buf w;
    fa_analysis* a;
    fa_waveform_point* p;
    uint32_t count = 0;
    int b;
    for (i = 0; i < n; i++) x[i * 2] = x[i * 2 + 1] = (float)(0.5 * sin(2.0 * PI * hz[i / RATE] * (double)i / RATE));
    x[RATE / 2 * 2 + 1] = 0.9f;                /* a peak in one channel */
    w = make_wav(x, n, 2);
    free(x);
    a = analyze(&w, FA_ANALYZE_WAVEFORM);
    free(w.d);
    CHECK(a != NULL);
    CHECK(fa_analysis_get_waveform(a, NULL, 0, &count) == FA_OK);
    CHECK_EQ(count, 300);
    p = (fa_waveform_point*)malloc(count * sizeof(*p));
    CHECK(fa_analysis_get_waveform(a, p, count, &count) == FA_OK);
    CHECK_NEAR(p[50].peak, 0.9, 1e-4);
    for (b = 0; b < 3; b++) {
        const fa_waveform_point* q = &p[b * 100 + 70];
        float band[3];
        band[0] = q->low; band[1] = q->mid; band[2] = q->high;
        printf("  %.0f Hz: peak %.3f low %.3f mid %.3f high %.3f\n", hz[b], q->peak, q->low, q->mid, q->high);
        CHECK_NEAR(q->peak, 0.5, 0.01);
        CHECK(band[b] > 0.4f && band[b] < 0.55f);
        CHECK(band[(b + 1) % 3] < 0.15f && band[(b + 2) % 3] < 0.15f);
    }
    free(p);
    fa_analysis_free(a);
}

/* Loudness in the same pass: a 1 kHz sine at -20 dBFS in both channels is -20 LUFS. */
static void test_loudness(void)
{
    int64_t n = 10 * RATE, i;
    float* x = (float*)malloc((size_t)n * 2 * sizeof(float));
    buf w;
    fa_analysis* a;
    fa_loudness l;
    for (i = 0; i < n; i++) x[i * 2] = x[i * 2 + 1] = (float)(0.1 * sin(2.0 * PI * 997.0 * (double)i / RATE));
    w = make_wav(x, n, 2);
    free(x);
    a = analyze(&w, FA_ANALYZE_LOUDNESS);
    free(w.d);
    CHECK(a != NULL);
    CHECK(fa_analysis_get_loudness(a, &l) == FA_OK);
    CHECK_NEAR(l.integrated_lufs, -20.0, 0.1);
    CHECK_NEAR(l.true_peak_dbtp, -20.0, 0.1);
    CHECK(l.is_final);
    CHECK_EQ(l.source, FA_LOUDNESS_SCAN);
    fa_beat_grid g;
    CHECK(!get_grid(a, &g));                   /* not asked for */
    fa_analysis_free(a);
}

static int same_results(const fa_analysis* a, const fa_analysis* b)
{
    fa_beat_grid ga, gb;
    fa_key_info ka, kb;
    fa_waveform_point pa[64], pb[64];
    uint32_t na = 0, nb = 0;
    memset(pa, 0, sizeof(pa));
    memset(pb, 0, sizeof(pb));
    if (get_grid(a, &ga) != get_grid(b, &gb) || get_key(a, &ka) != get_key(b, &kb)) return 0;
    if (memcmp(&ga, &gb, sizeof(ga)) != 0) return 0;
    if (strcmp(ka.name, kb.name) != 0 || ka.confidence != kb.confidence) return 0;
    fa_analysis_get_waveform(a, pa, 64, &na);
    fa_analysis_get_waveform(b, pb, 64, &nb);
    return na == nb && memcmp(pa, pb, sizeof(pa)) == 0;
}

static fa_analysis* analyze_cached(fa_engine* e, const buf* b, uint32_t kinds, float min_bpm, uint32_t flags)
{
    fa_analysis_options o;
    fa_analysis* a = NULL;
    fa_stream* s = NULL;
    fa_open_options oo;
    fa_open_options_init(&oo);
    oo.flags = FA_OPEN_DECODE_ONLY;
    if (fa_stream_open_memory(e, b->d, b->n, &oo, &s) != FA_OK) return NULL;
    fa_analysis_options_init(&o);
    o.kinds = kinds;
    o.flags = flags;
    if (min_bpm > 0) o.min_bpm = min_bpm;
    fa_analyze_stream(s, &o, &a);
    fa_stream_close(s);
    return a;
}

static uint32_t info_flags(const fa_analysis* a)
{
    fa_analysis_info info;
    info.struct_size = sizeof(info);
    fa_analysis_get_info(a, &info);
    return info.flags;
}

static uint32_t info_kinds(const fa_analysis* a)
{
    fa_analysis_info info;
    info.struct_size = sizeof(info);
    fa_analysis_get_info(a, &info);
    return info.kinds;
}

/* Deletes the files in `dir` (no subfolders), then the folder. */
static void remove_dir(const char* dir)
{
    char path[1024 + 300];   /* the folder and a name */
#if defined(_WIN32)
    struct _finddata_t fd;
    intptr_t h;
    snprintf(path, sizeof(path), "%s/*", dir);
    h = _findfirst(path, &fd);
    if (h != -1) {
        do {
            if (fd.attrib & _A_SUBDIR) continue;
            snprintf(path, sizeof(path), "%s/%s", dir, fd.name);
            remove(path);
        } while (_findnext(h, &fd) == 0);
        _findclose(h);
    }
    _rmdir(dir);
#else
    DIR* d = opendir(dir);
    struct dirent* e;
    if (d) {
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
            remove(path);
        }
        closedir(d);
    }
    rmdir(dir);
#endif
}

/* Results are cached by content; asking for more measures the union. */
static void cache_checks(const char* dir)
{
    fa_engine_config cfg;
    fa_engine* e = NULL;
    song s = { 0 };
    buf m, w;
    fa_analysis *a, *b;
    int64_t n;
    float* x;
    fa_engine_config_init(&cfg);
    cfg.flags |= FA_ENGINE_NULL_OUTPUT;
    cfg.cache_directory = dir;
    CHECK(fa_engine_create(&cfg, &e) == FA_OK);
    x = drums(111.0, 0.1, 20.0, &n);
    (void)s; (void)m;
    x[0] = (float)(time(NULL) % 100000) * 1e-9f;   /* content this run's own: no file from an earlier run */
    w = make_wav(x, n, 1);
    free(x);
    a = analyze_cached(e, &w, FA_ANALYZE_BEATS, 0, 0);
    b = analyze_cached(e, &w, FA_ANALYZE_BEATS, 0, 0);
    CHECK(a && b);
    CHECK(info_flags(b) & FA_ANALYSIS_FROM_CACHE);
    CHECK(same_results(a, b));
    fa_analysis_free(b);
    /* More kinds: measured again, with what was there. */
    b = analyze_cached(e, &w, FA_ANALYZE_WAVEFORM, 0, 0);
    CHECK(b && !(info_flags(b) & FA_ANALYSIS_FROM_CACHE));
    CHECK_EQ(info_kinds(b), FA_ANALYZE_BEATS | FA_ANALYZE_WAVEFORM);
    fa_analysis_free(b);
    b = analyze_cached(e, &w, FA_ANALYZE_BEATS | FA_ANALYZE_WAVEFORM, 0, 0);
    CHECK(b && (info_flags(b) & FA_ANALYSIS_FROM_CACHE));
    {
        fa_beat_grid ga, gb;
        fa_waveform_point p1[8], p2[8];
        uint32_t c1 = 0, c2 = 0;
        fa_analysis* fresh = analyze_cached(e, &w, FA_ANALYZE_WAVEFORM, 0, FA_ANALYZE_NO_CACHE);
        CHECK(get_grid(a, &ga) && get_grid(b, &gb));
        CHECK(memcmp(&ga, &gb, sizeof(ga)) == 0);
        CHECK(fresh && !(info_flags(fresh) & FA_ANALYSIS_FROM_CACHE));
        fa_analysis_get_waveform(fresh, p1, 8, &c1);
        fa_analysis_get_waveform(b, p2, 8, &c2);
        CHECK_EQ(c1, c2);
        CHECK(memcmp(p1, p2, sizeof(p1)) == 0);   /* 16-bit values: the cache gives them back exactly */
        fa_analysis_free(fresh);
    }
    fa_analysis_free(b);
    /* Another tempo range: the grid is measured again. */
    b = analyze_cached(e, &w, FA_ANALYZE_BEATS, 60.0f, 0);
    CHECK(b && !(info_flags(b) & FA_ANALYSIS_FROM_CACHE));
    fa_analysis_free(b);
    fa_analysis_free(a);
    free(w.d);
    fa_engine_destroy(e);
}

/* In a folder of the system's temp directory, deleted after. */
static void test_cache(void)
{
    char dir[1024];
    const char* tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = getenv("TEMP");
    if (!tmp || !*tmp) tmp = getenv("TMP");
#if defined(_WIN32)
    if (!tmp || !*tmp) tmp = ".";
#elif defined(__ANDROID__)
    if (!tmp || !*tmp) tmp = "/data/local/tmp";   /* Android has no /tmp */
#else
    if (!tmp || !*tmp) tmp = "/tmp";
#endif
    snprintf(dir, sizeof(dir), "%s/fa_analysis_cache_%ld_%ld", tmp, (long)time(NULL), (long)clock());
    cache_checks(dir);
    remove_dir(dir);
}

static void test_stream_grid(void)
{
    int64_t n = RATE;
    float* x = (float*)calloc((size_t)n, sizeof(float));
    buf w = make_wav(x, n, 1);
    fa_stream* s = open_mem(&w, NULL);
    fa_beat_grid g, out;
    fa_tempo_segment seg[3], got[4];
    uint32_t count = 7;
    song so = { 0 };
    buf m;
    free(x);
    CHECK(s != NULL);
    memset(&out, 0, sizeof(out));
    out.struct_size = sizeof(out);
    CHECK(fa_stream_get_beat_grid(s, &out, NULL, 0, &count) == FA_ERROR_NOT_FOUND);
    CHECK_EQ(count, 0);
    memset(&g, 0, sizeof(g));
    g.struct_size = sizeof(g);
    g.bpm = 128.0;
    g.first_beat = 1000;
    g.sample_rate = RATE;
    g.beats_per_bar = 4;
    g.confidence = 1.0f;
    g.flags = FA_GRID_CONSTANT | FA_GRID_EDITED;
    CHECK(fa_stream_set_beat_grid(s, &g, NULL, 0) == FA_OK);
    CHECK(fa_stream_get_beat_grid(s, &out, got, 4, &count) == FA_OK);
    CHECK_EQ(count, 1);
    CHECK_EQ(out.segment_count, 1);
    CHECK_NEAR(out.bpm, 128.0, 0.0);
    CHECK_EQ(out.flags, FA_GRID_CONSTANT | FA_GRID_EDITED);
    CHECK_EQ(got[0].start, 1000);
    CHECK_NEAR(got[0].bpm, 128.0, 0.0);
    /* A tempo map */
    seg[0].start = 1000; seg[0].bpm = 128.0; seg[0].beat = 0.0;
    seg[1].start = 50000; seg[1].bpm = 130.0; seg[1].beat = 4.2;
    seg[2].start = 90000; seg[2].bpm = 132.0; seg[2].beat = 8.0;
    g.flags = FA_GRID_EDITED;
    CHECK(fa_stream_set_beat_grid(s, &g, seg, 3) == FA_OK);
    CHECK(fa_stream_get_beat_grid(s, &out, got, 2, &count) == FA_OK);
    CHECK_EQ(count, 3);
    CHECK_EQ(out.segment_count, 3);
    CHECK_EQ(out.flags, FA_GRID_EDITED);
    CHECK_EQ(got[1].start, 50000);
    CHECK(fa_stream_get_beat_grid(s, NULL, NULL, 0, &count) == FA_OK);
    CHECK_EQ(count, 3);
    /* Bad grids */
    seg[1].start = 1000;
    CHECK(fa_stream_set_beat_grid(s, &g, seg, 3) == FA_ERROR_INVALID_ARG);
    seg[1].start = 50000;
    g.bpm = 0.05;
    CHECK(fa_stream_set_beat_grid(s, &g, NULL, 0) == FA_ERROR_INVALID_ARG);
    g.bpm = 128.0;
    g.flags = 1u << 20;
    CHECK(fa_stream_set_beat_grid(s, &g, NULL, 0) == FA_ERROR_INVALID_ARG);
    g.flags = 0;
    g.bar_offset = 4;
    CHECK(fa_stream_set_beat_grid(s, &g, NULL, 0) == FA_ERROR_INVALID_ARG);
    g.bar_offset = 0;
    CHECK(fa_stream_set_beat_grid(s, &g, NULL, 2) == FA_ERROR_INVALID_ARG);
    CHECK(fa_stream_get_beat_grid(s, &out, NULL, 1, &count) == FA_ERROR_INVALID_ARG);
    /* Still the last good one; NULL removes it. */
    CHECK(fa_stream_get_beat_grid(s, &out, NULL, 0, &count) == FA_OK);
    CHECK_EQ(count, 3);
    CHECK(fa_stream_set_beat_grid(s, NULL, NULL, 0) == FA_OK);
    CHECK(fa_stream_get_beat_grid(s, &out, NULL, 0, &count) == FA_ERROR_NOT_FOUND);
    /* Exact grids of modules can be fast (speed 1, tempo 255); they can be set back. */
    g.bpm = 1531.25;
    CHECK(fa_stream_set_beat_grid(s, &g, NULL, 0) == FA_OK);
    g.bpm = 128.0;
    fa_stream_close(s);
    free(w.d);

    /* A MIDI stream: the app's grid wins over the file's, which comes back after. */
    chords(&so, 0, 0, 4, 0);
    m = write_song(&so, 100.0, NULL, NULL, 0, 0, 0);
    s = open_mem(&m, g_font);
    CHECK(s != NULL);
    g.flags = FA_GRID_EDITED;
    CHECK(fa_stream_set_beat_grid(s, &g, NULL, 0) == FA_OK);
    CHECK(fa_stream_get_beat_grid(s, &out, NULL, 0, &count) == FA_OK);
    CHECK_NEAR(out.bpm, 128.0, 0.0);
    CHECK_EQ(out.flags, FA_GRID_CONSTANT | FA_GRID_EDITED);
    CHECK(fa_stream_set_beat_grid(s, NULL, NULL, 0) == FA_OK);
    CHECK(fa_stream_get_beat_grid(s, &out, NULL, 0, &count) == FA_OK);
    CHECK_NEAR(out.bpm, 100.0, 1e-9);
    CHECK(out.flags & FA_GRID_EXACT);
    fa_stream_close(s);
    free(m.d);
    free(so.e);
}

static void test_args(void)
{
    fa_analysis_options o;
    fa_analysis* a = (fa_analysis*)1;
    fa_stream* s;
    fa_beat_grid g;
    int64_t n = RATE;
    float* x = (float*)calloc((size_t)n, sizeof(float));
    buf w = make_wav(x, n, 1);
    free(x);
    fa_analysis_options_init(&o);
    CHECK_EQ(o.kinds, FA_ANALYZE_BEATS | FA_ANALYZE_KEY);
    CHECK_NEAR(o.min_bpm, 70.0, 0.0);
    CHECK_NEAR(o.max_bpm, 180.0, 0.0);
    CHECK(fa_analyze_path(NULL, "x", NULL, &a) == FA_ERROR_INVALID_ARG && a == NULL);
    CHECK(fa_analyze_path(g_engine, "no-such-file.wav", NULL, &a) != FA_OK && a == NULL);
    s = open_mem(&w, NULL);
    o.kinds = 0;
    CHECK(fa_analyze_stream(s, &o, &a) == FA_ERROR_INVALID_ARG);
    o.kinds = 1u << 9;
    CHECK(fa_analyze_stream(s, &o, &a) == FA_ERROR_INVALID_ARG);
    o.kinds = FA_ANALYZE_BEATS;
    o.flags = 1u << 7;
    CHECK(fa_analyze_stream(s, &o, &a) == FA_ERROR_INVALID_ARG);
    o.flags = 0;
    o.struct_size = 4;
    CHECK(fa_analyze_stream(s, &o, &a) == FA_ERROR_INVALID_ARG);
    CHECK(fa_analyze_stream(s, NULL, &a) == FA_OK);   /* defaults */
    g.struct_size = 0;
    CHECK(fa_analysis_get_beat_grid(a, &g) == FA_ERROR_INVALID_ARG);
    fa_analysis_free(a);
    fa_analysis_free(NULL);
    fa_stream_close(s);
    free(w.d);
}

/* ---- the accuracy report ------------------------------------------------------------- */

#define REPORT_SONGS 48

typedef struct report_song { double bpm; int tonic, minor, style; } report_song;

/* General MIDI drums: kick, snare, closed hat. Styles: 0 four on the floor, 1 rock
 * backbeat with eighth hats, 2 half-time feel, 3 humanized (+-12 ms) with the
 * tempo drifting by 1% over the song. *out_mean = the song's mean tempo (beats over
 * time). */
static buf report_midi(const report_song* r, double* out_mean)
{
    song s = { 0 };
    int bars = 48, bar, b, i, h;
    static const int major_prog[4][3] = { { 0, 4, 7 }, { 7, 11, 14 }, { 9, 12, 16 }, { 5, 9, 12 } };   /* I V vi IV */
    static const int minor_prog[4][3] = { { 0, 3, 7 }, { 8, 12, 15 }, { 5, 8, 12 }, { 7, 11, 14 } };   /* i VI iv V */
    uint32_t tticks[64];
    double tvals[64];
    int nt = 0;
    buf m;
    add_ev(&s, 0, 2, 0xC0, 0, 0);             /* piano */
    add_ev(&s, 0, 2, 0xC1, 33, 0);            /* fingered bass */
    add_ev(&s, 0, 2, 0xC2, 48, 0);            /* strings */
    for (bar = 0; bar < bars; bar++) {
        const int* c = r->minor ? minor_prog[bar % 4] : major_prog[bar % 4];
        uint32_t t0 = (uint32_t)bar * 1920;
        for (i = 0; i < 3; i++) note(&s, t0, 1900, 2, 60 + r->tonic + c[i], 60);
        for (b = 0; b < 4; b++) {
            uint32_t t = t0 + (uint32_t)b * 480;
            int jitter = r->style == 3 ? (int)((frand() - 0.5) * 2.0 * 12.0 / 60000.0 * r->bpm * 480.0) : 0;
            uint32_t tj = (uint32_t)((int)t + jitter > 0 ? (int)t + jitter : 0);
            note(&s, tj, 240, 1, 36 + r->tonic + c[0] + (b == 2 ? 12 : 0), 100);
            note(&s, t + 240, 200, 0, 60 + r->tonic + c[(b + 1) % 3] + 12, 70);
            if (r->style == 0 || r->style == 3) note(&s, tj, 60, 9, 36, 120);
            else if (r->style == 1) { if (b == 0 || b == 2) note(&s, tj, 60, 9, 36, 120); }
            else if (b == 0) note(&s, tj, 60, 9, 36, 120);
            if ((r->style == 1 || r->style == 3) && (b == 1 || b == 3)) note(&s, tj, 60, 9, 38, 110);
            if (r->style == 2 && b == 2) note(&s, tj, 60, 9, 38, 110);
            for (h = 0; h < 2; h++) note(&s, t + (uint32_t)h * 240, 30, 9, 42, h ? 60 : 90);
        }
        if (r->style == 3 && bar % 4 == 3 && nt < 64) {
            tticks[nt] = (uint32_t)(bar + 1) * 1920;
            tvals[nt] = r->bpm * (1.0 + 0.01 * (double)(bar + 1) / bars);
            nt++;
        }
    }
    {
        double secs = 0.0, cur = r->bpm;
        int k2 = 0;
        for (bar = 0; bar < bars; bar++) {
            if (k2 < nt && tticks[k2] == (uint32_t)bar * 1920) cur = tvals[k2++];
            secs += 4.0 * 60.0 / cur;
        }
        *out_mean = bars * 4 / (secs / 60.0);
    }
    m = write_song(&s, r->bpm, tticks, tvals, nt, 0, 0);
    free(s.e);
    return m;
}

/* Processor time: the analysis runs on this one thread (and timespec_get is
 * missing from older Android). */
static double now_s(void)
{
    return (double)clock() / CLOCKS_PER_SEC;
}

static const char* key_relation(const fa_key_info* got, int tonic, int minor)
{
    int d = ((got->tonic - tonic) % 12 + 12) % 12;
    if (got->minor == minor && d == 0) return "exact";
    if (got->minor != minor && ((minor && d == 3) || (!minor && d == 9))) return "relative";
    if (got->minor == minor && (d == 7 || d == 5)) return "fifth";
    if (got->minor != minor && d == 0) return "parallel";
    return "other";
}

static void run_report(const char* sf2)
{
    static const char* const styles[4] = { "four on the floor", "rock", "half-time", "humanized, drifting" };
    fa_soundfont* font = NULL;
    report_song songs[REPORT_SONGS];
    int i, exact = 0, rel = 0, fifth = 0, par = 0, other = 0, tempo_ok[4] = { 0 }, count[4] = { 0 }, octave[4] = { 0 };
    double secs_total = 0.0, wall_total = 0.0, worst_bpm[4] = { 0 }, worst_ms[4] = { 0 }, conf_sum[4] = { 0 };
    if (fa_soundfont_open_path(sf2, &font) != FA_OK) { fprintf(stderr, "cannot open %s\n", sf2); g_failures++; return; }
    for (i = 0; i < REPORT_SONGS; i++) {
        songs[i].bpm = floor((70.0 + frand() * 110.0) * 10.0 + 0.5) / 10.0;
        songs[i].tonic = i % 12;
        songs[i].minor = (i / 12) % 2;
        songs[i].style = i % 4;
    }
    printf("| # | Style | BPM (file) | Found | Error (BPM) | Beat error (ms) | Confidence | Key | Found | |\n");
    printf("|---|---|---|---|---|---|---|---|---|---|\n");
    for (i = 0; i < REPORT_SONGS; i++) {
        double truth, secs, ms, t0, t1, err;
        buf m, w;
        fa_stream* s;
        fa_analysis* a = NULL;
        fa_analysis_options o;
        fa_beat_grid g;
        fa_key_info k, want;
        const char* rel_s;
        int st = songs[i].style;
        if (getenv("FA_REPORT_ONLY") && atoi(getenv("FA_REPORT_ONLY")) != i + 1) continue;
        m = report_midi(&songs[i], &truth);
        w = render(&m, font, &secs);
        fa_analysis_options_init(&o);
        o.kinds = FA_ANALYZE_BEATS | FA_ANALYZE_KEY | FA_ANALYZE_WAVEFORM;
        o.flags = FA_ANALYZE_NO_CACHE;
        s = open_mem(&w, NULL);
        t0 = now_s();
        fa_analyze_stream(s, &o, &a);
        t1 = now_s();
        fa_stream_close(s);
        secs_total += secs;
        wall_total += t1 - t0;
        want.struct_size = sizeof(want);
        fa_key_make(songs[i].tonic, songs[i].minor, &want);
        if (!a || !get_grid(a, &g)) {
            printf("| %d | %s | %.2f | none | | | | %s | | |\n", i + 1, styles[st], truth, want.name);
            fa_analysis_free(a); free(m.d); free(w.d);
            continue;
        }
        count[st]++;
        conf_sum[st] += g.confidence;
        /* Half or double tempo is counted apart, its error measured at that tempo. */
        if (fabs(g.bpm - truth / 2.0) < 0.01 * truth) { octave[st]++; truth /= 2.0; }
        else if (fabs(g.bpm - truth * 2.0) < 0.02 * truth) { octave[st]++; truth *= 2.0; }
        err = g.bpm - truth;
        ms = beat_error_ms(&g, truth, 0.0, st == 3 ? 4.0 : secs - 4.0);
        if (fabs(err) <= 0.05) tempo_ok[st]++;
        if (fabs(err) > worst_bpm[st]) worst_bpm[st] = fabs(err);
        if (ms > worst_ms[st]) worst_ms[st] = ms;
        if (get_key(a, &k)) rel_s = key_relation(&k, songs[i].tonic, songs[i].minor);
        else { rel_s = "none"; strcpy(k.name, "-"); }
        if (!strcmp(rel_s, "exact")) exact++;
        else if (!strcmp(rel_s, "relative")) rel++;
        else if (!strcmp(rel_s, "fifth")) fifth++;
        else if (!strcmp(rel_s, "parallel")) par++;
        else other++;
        printf("| %d | %s | %.2f | %.2f | %.3f | %.1f | %.2f | %s | %s | %s |\n", i + 1, styles[st], truth, g.bpm, err, ms,
               g.confidence, want.name, k.name, rel_s);
        fa_analysis_free(a);
        free(m.d);
        free(w.d);
    }
    printf("\n| Style | Songs | Tempo within 0.05 BPM | Of those, half or double | Worst tempo error | Worst beat error (ms) | Mean confidence |\n|---|---|---|---|---|---|---|\n");
    for (i = 0; i < 4; i++)
        printf("| %s | %d | %d | %d | %.3f | %.1f | %.2f |\n", styles[i], count[i], tempo_ok[i], octave[i], worst_bpm[i],
               worst_ms[i], count[i] ? conf_sum[i] / count[i] : 0.0);
    printf("\nKeys: %d exact, %d relative, %d a fifth away, %d parallel, %d other (of %d)\n", exact, rel, fifth, par, other,
           REPORT_SONGS);
    printf("Speed: %.0f s of audio in %.2f s (%.0fx real time, one core, beats + key + waveform)\n", secs_total, wall_total,
           secs_total / wall_total);
    fa_soundfont_close(font);
}

int main(void)
{
    fa_engine_config cfg;
    buf sf2;
    const char* report = getenv("FA_ANALYSIS_REPORT_SF2");
    fa_engine_config_init(&cfg);
    cfg.flags |= FA_ENGINE_NULL_OUTPUT;
    if (fa_engine_create(&cfg, &g_engine) != FA_OK) { fprintf(stderr, "engine\n"); return 1; }
    sf2 = make_sf2();
    if (fa_soundfont_open_memory(sf2.d, sf2.n, &g_font) != FA_OK) { fprintf(stderr, "soundfont\n"); return 1; }
    if (report && *report) {
        run_report(report);
    } else {
        RUN(test_key_names);
        RUN(test_drum_grids);
        RUN(test_tempo_range);
        RUN(test_midi_render_grid);
        RUN(test_midi_render_key);
        RUN(test_midi_exact_grid);
        RUN(test_tracker_exact_grid);
        RUN(test_silence);
        RUN(test_waveform);
        RUN(test_loudness);
        RUN(test_cache);
        RUN(test_stream_grid);
        RUN(test_args);
    }
    fa_soundfont_close(g_font);
    free(sf2.d);
    fa_engine_destroy(g_engine);
    if (g_failures) { fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    printf("all passed\n");
    return 0;
}
