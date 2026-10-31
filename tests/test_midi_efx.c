/* MIDI insertion effects: the SC-88 Pro EFX and the MU100's XG insertion types
 * (src/midi_efx.c, src/midi_efx_types.c). White-box: every type is driven directly
 * (bounded, settles, silence stays silent), a few blocks are measured (EQ response,
 * delay taps, pitch shift, compressor, gate, tremolo, a control); then whole songs
 * through the decoder check XG insertion 1 and 2, the GS EFX part switch and send
 * levels, control sources, unknown types (dry), read sizes and seeks. */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "fa_internal.h"
#include "midi_synth_test.h"
#if defined(__APPLE__)
#  include <unistd.h>
#endif

#define BLOCK 64
#define CHECK_EQ(a, b) do { long long va_ = (long long)(a), vb_ = (long long)(b); if (va_ != vb_) { \
    fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_failures++; return; } } while (0)

/* `frames` of `in` (stereo) through the effect in 64-frame blocks, from song frame
 * `start`; silence where process says there is none. */
static float* efx_run_from(fa_midi_efx* e, const float* in, int frames, int start)
{
    float* out = (float*)calloc((size_t)frames * 2, sizeof(float));
    int off;
    for (off = 0; off < frames; off += BLOCK) {
        int n = frames - off < BLOCK ? frames - off : BLOCK;
        if (!fa_midi_efx_process(e, in + off * 2, out + off * 2, n, start + off)) memset(out + off * 2, 0, (size_t)n * 2 * sizeof(float));
    }
    return out;
}
static float* efx_run(fa_midi_efx* e, const float* in, int frames) { return efx_run_from(e, in, frames, 0); }

static float* sine(int frames, double hz, float amp)
{
    float* x = (float*)malloc((size_t)frames * 2 * sizeof(float));
    int i;
    for (i = 0; i < frames; i++) x[i * 2] = x[i * 2 + 1] = (float)(amp * sin(2.0 * PI * hz * i / RATE));
    return x;
}

static float* noise(int frames, float amp, uint32_t seed)
{
    float* x = (float*)malloc((size_t)frames * 2 * sizeof(float));
    int i;
    for (i = 0; i < frames * 2; i++) { seed = seed * 1664525u + 1013904223u; x[i] = (float)((int32_t)seed / 2147483648.0 * amp); }
    return x;
}

static double rms_ch(const float* x, int ch, int from, int to)
{
    double acc = 0;
    int i;
    for (i = from; i < to; i++) acc += (double)x[i * 2 + ch] * x[i * 2 + ch];
    return to > from ? sqrt(acc / (to - from)) : 0.0;
}

static int frames_ms(double ms) { return (int)(ms * RATE / 1000.0 + 0.5); }

/* GS parameters by their SysEx address (40 03 03-16), XG by number (1-16). */
static void gsp(fa_midi_efx* e, int addr, int v) { fa_midi_efx_set_param(e, addr - 3, v); }
static void xgp(fa_midi_efx* e, int n, int v) { fa_midi_efx_set_param(e, n - 1, v); }

/* ---- every type ------------------------------------------------------------------- */

static int each_type(int gs, int k, int* msb, int* lsb)
{
    int m, l, n = 0;
    for (m = 0; m < 128; m++)
        for (l = 0; l < 128; l++)
            if (fa_midi_efx_known(gs, m, l, 1) && n++ == k) { *msb = m; *lsb = l; return 1; }
    return 0;
}

static void test_counts(void)
{
    int gs = 0, xg = 0, m, l;
    while (each_type(1, gs, &m, &l)) gs++;
    while (each_type(0, xg, &m, &l)) xg++;
    CHECK_EQ(gs, fa_midi_efx_gs_count());
    CHECK_EQ(xg, fa_midi_efx_xg_count());
    CHECK_EQ(gs, 65);                       /* Thru and the SC-88 Pro's 64 types */
    CHECK_EQ(xg, 24);                       /* the MU100's insertion-only types and variants */
    CHECK(fa_midi_efx_known(1, 0x01, 0x22, 1));     /* Rotary */
    CHECK(!fa_midi_efx_known(1, 0x01, 0x7F, 0));    /* GS: an unknown LSB is not a type */
    CHECK(fa_midi_efx_known(0, 0x5E, 0x05, 0));     /* XG: an unknown LSB plays LSB 0 */
    CHECK(!fa_midi_efx_known(0, 0x5E, 0x05, 1));
}

/* Each type at its defaults, and at all parameters 0 and all 127: bounded and finite on
 * loud noise; with its defaults, silence in from the start gives nothing (process
 * returns 0) and once the input stops the state runs out (back to 0: no cost). */
static void test_every_type(void)
{
    const int n = RATE, quiet = RATE * 30;
    float* loud = noise(n, 0.8f, 1);
    float* zero = (float*)calloc(BLOCK * 2, sizeof(float)), out[BLOCK * 2];
    int gs, k, msb, lsb;
    for (gs = 0; gs < 2; gs++) {
        for (k = 0; each_type(gs, k, &msb, &lsb); k++) {
            int pass;
            for (pass = 0; pass < 3; pass++) {
                fa_midi_efx* e = fa_midi_efx_new(RATE);
                float* y;
                double peak = 0;
                int i, off, settled = -1;
                CHECK(fa_midi_efx_set_type(e, gs, msb, lsb));
                if (pass) for (i = 0; i < 20; i++) fa_midi_efx_set_param(e, i, pass == 1 ? 0 : 127);
                if (!pass) CHECK(!fa_midi_efx_process(e, zero, out, BLOCK, 0));
                y = efx_run(e, loud, pass ? n / 4 : n);
                for (i = 0; i < (pass ? n / 4 : n) * 2; i++) { CHECK(y[i] == y[i]); if (fabs(y[i]) > peak) peak = fabs(y[i]); }
                /* All parameters at 127 stack every gain at +12 dB and feedback at +0.98: loud, but it stays bounded. */
                if (peak > (pass ? 256.0 : 8.0)) fprintf(stderr, "  %s %02X %02X pass %d peak %.2f\n", gs ? "GS" : "XG", msb, lsb, pass, peak);
                CHECK(peak <= (pass ? 256.0 : 8.0));
                free(y);
                if (!pass) {
                    for (off = n; off < n + quiet; off += BLOCK)
                        if (!fa_midi_efx_process(e, zero, out, BLOCK, off)) { settled = off; break; }
                    if (settled < 0) fprintf(stderr, "  %s %02X %02X never settles\n", gs ? "GS" : "XG", msb, lsb);
                    CHECK(settled > 0);
                }
                fa_midi_efx_free(e);
            }
        }
    }
    free(loud);
    free(zero);
}

/* The same input from the same song frame gives the same output (LFOs follow the song). */
static void test_deterministic(void)
{
    const int n = RATE / 2;
    float* x = noise(n, 0.5f, 7), *a, *b;
    fa_midi_efx* e = fa_midi_efx_new(RATE), *f = fa_midi_efx_new(RATE);
    CHECK(fa_midi_efx_set_type(e, 1, 0x11, 0x07) && fa_midi_efx_set_type(f, 1, 0x11, 0x07));   /* Phaser / Rotary */
    a = efx_run_from(e, x, n, 12345);
    b = efx_run_from(f, x, n, 12345);
    CHECK(memcmp(a, b, (size_t)n * 2 * sizeof(float)) == 0);
    free(a); free(b); free(x);
    fa_midi_efx_free(e);
    fa_midi_efx_free(f);
}

/* ---- blocks ------------------------------------------------------------------------- */

static double gain_db(fa_midi_efx* e, double hz)
{
    const int n = RATE / 2;
    float* x = sine(n, hz, 0.1f), *y;
    double g;
    fa_midi_efx_reset(e);
    y = efx_run(e, x, n);
    g = 20.0 * log10(rms_ch(y, 0, n / 2, n) / rms_ch(x, 0, n / 2, n));
    free(x); free(y);
    return g;
}

/* Stereo-EQ: low shelf +12 dB at 200 Hz, high shelf -12 dB at 4 kHz, the mids flat. */
static void test_gs_eq(void)
{
    fa_midi_efx* e = fa_midi_efx_new(RATE);
    CHECK(fa_midi_efx_set_type(e, 1, 0x01, 0x00));
    gsp(e, 0x03, 0); gsp(e, 0x04, 0x4C);
    gsp(e, 0x05, 0); gsp(e, 0x06, 0x34);
    gsp(e, 0x09, 0x40); gsp(e, 0x0C, 0x40);
    CHECK_NEAR(gain_db(e, 40.0), 12.0, 1.0);
    CHECK_NEAR(gain_db(e, 1000.0), 0.0, 1.5);
    CHECK_NEAR(gain_db(e, 16000.0), -12.0, 1.0);
    gsp(e, 0x16, 0);                       /* level 0 */
    CHECK(gain_db(e, 1000.0) < -100.0);
    fa_midi_efx_free(e);
}

static int first_peak(const float* x, int ch, int frames)
{
    int i, at = -1;
    float best = 0;
    for (i = 0; i < frames; i++) if (fabsf(x[i * 2 + ch]) > best) { best = fabsf(x[i * 2 + ch]); at = i; }
    return at;
}

/* Stereo Delay: left 150 ms, right 300 ms (the defaults), wet only, no feedback. */
static void test_gs_delay_taps(void)
{
    const int n = RATE;
    float* x = (float*)calloc((size_t)n * 2, sizeof(float)), *y;
    fa_midi_efx* e = fa_midi_efx_new(RATE);
    CHECK(fa_midi_efx_set_type(e, 1, 0x01, 0x50));
    gsp(e, 0x05, 64);
    gsp(e, 0x0A, 0x7F);                    /* no HF damp */
    gsp(e, 0x12, 127);
    x[0] = x[1] = 1.0f;
    y = efx_run(e, x, n);
    CHECK(abs(first_peak(y, 0, n) - frames_ms(150)) <= 2);
    CHECK(abs(first_peak(y, 1, n) - frames_ms(300)) <= 2);
    CHECK(rms_ch(y, 0, 0, frames_ms(140)) == 0.0);
    free(x); free(y);
    fa_midi_efx_free(e);
}

static double frequency(const float* x, int from, int to)
{
    int i, first = -1, last = -1, n = 0;
    for (i = from + 1; i < to; i++) {
        float a = x[(i - 1) * 2] + x[(i - 1) * 2 + 1], b = x[i * 2] + x[i * 2 + 1];
        if (a < 0 && b >= 0) { if (first < 0) first = i; last = i; n++; }
    }
    return n > 1 ? (double)(n - 1) * RATE / (last - first) : 0.0;
}

/* 2Voice Pitch Shifter: voice 1 an octave up, alone, wet only: 440 Hz comes out at 880. */
static void test_gs_pitch(void)
{
    const int n = RATE;
    float* x = sine(n, 440.0, 0.3f), *y;
    fa_midi_efx* e = fa_midi_efx_new(RATE);
    CHECK(fa_midi_efx_set_type(e, 1, 0x01, 0x60));
    gsp(e, 0x03, 64 + 12); gsp(e, 0x04, 64); gsp(e, 0x05, 0); gsp(e, 0x06, 64);
    gsp(e, 0x0C, 0);
    gsp(e, 0x12, 127);
    y = efx_run(e, x, n);
    CHECK_NEAR(frequency(y, RATE / 4, n), 880.0, 880.0 * 0.03);
    free(x); free(y);
    /* XG Pitch Change: -12 semitones, both voices, wet only: 220 Hz. */
    e = (fa_midi_efx_free(e), fa_midi_efx_new(RATE));
    CHECK(fa_midi_efx_set_type(e, 0, 0x50, 0x00));
    xgp(e, 1, 64 - 12); xgp(e, 2, 0); xgp(e, 3, 64); xgp(e, 4, 64); xgp(e, 5, 64);
    x = sine(n, 440.0, 0.3f);
    y = efx_run(e, x, n);
    CHECK_NEAR(frequency(y, RATE / 4, n), 220.0, 220.0 * 0.03);
    free(x); free(y);
    fa_midi_efx_free(e);
}

/* XG Compressor (threshold -27 dB, 3:1): quiet input passes, loud input is pressed down. */
static void test_xg_compressor(void)
{
    const int n = RATE;
    float* q = sine(n, 500.0, 0.01f), *l = sine(n, 500.0, 0.9f), *y;
    fa_midi_efx* e = fa_midi_efx_new(RATE);
    double gq, gl;
    CHECK(fa_midi_efx_set_type(e, 0, 0x53, 0x00));
    y = efx_run(e, q, n);
    gq = 20.0 * log10(rms_ch(y, 0, n / 2, n) / rms_ch(q, 0, n / 2, n));
    free(y);
    fa_midi_efx_reset(e);
    y = efx_run(e, l, n);
    gl = 20.0 * log10(rms_ch(y, 0, n / 2, n) / rms_ch(l, 0, n / 2, n));
    free(y);
    CHECK_NEAR(gq, 0.0, 0.5);
    if (gl > -12.0 || gl < -22.0) fprintf(stderr, "  compressor gain %.2f dB\n", gl);
    CHECK(gl < -12.0 && gl > -22.0);
    free(q); free(l);
    fa_midi_efx_free(e);
}

/* XG Noise Gate (threshold -57 dB): -66 dB is shut out, -20 dB passes. */
static void test_xg_gate(void)
{
    const int n = RATE;
    float* q = sine(n, 500.0, 0.0005f), *l = sine(n, 500.0, 0.1f), *y;
    fa_midi_efx* e = fa_midi_efx_new(RATE);
    CHECK(fa_midi_efx_set_type(e, 0, 0x54, 0x00));
    y = efx_run(e, q, n);
    CHECK(rms_ch(y, 0, n / 2, n) < rms_ch(q, 0, n / 2, n) * 0.05);
    free(y);
    fa_midi_efx_reset(e);
    y = efx_run(e, l, n);
    CHECK_NEAR(rms_ch(y, 0, n / 2, n) / rms_ch(l, 0, n / 2, n), 1.0, 0.1);
    free(y); free(q); free(l);
    fa_midi_efx_free(e);
}

static int level_cycles(const float* x, int from, int to)
{
    const int w = frames_ms(5);
    double mean = rms_ch(x, 0, from, to), prev = -1;
    int i, n = 0;
    for (i = from; i + w <= to; i += w) {
        double r = rms_ch(x, 0, i, i + w);
        if (prev >= 0 && prev < mean && r >= mean) n++;
        prev = r;
    }
    return n;
}

/* Tremolo: sine wave at 3.05 Hz (rate 0x3C): about 6 swings in 2 s; control 1 (on the
 * rate) at +1 takes it to the fastest rate. */
static void test_gs_tremolo_and_control(void)
{
    const int n = RATE * 2;
    float* x = sine(n, 1000.0, 0.3f), *y;
    fa_midi_efx* e = fa_midi_efx_new(RATE);
    int c;
    CHECK(fa_midi_efx_set_type(e, 1, 0x01, 0x25));
    gsp(e, 0x03, 2);
    gsp(e, 0x05, 127);
    y = efx_run(e, x, n);
    c = level_cycles(y, 0, n);
    if (c < 5 || c > 7) fprintf(stderr, "  tremolo cycles %d\n", c);
    CHECK(c >= 5 && c <= 7);
    free(y);
    fa_midi_efx_control(e, 0, 1.0f);
    y = efx_run(e, x, n);
    c = level_cycles(y, 0, n);
    if (c < 16) fprintf(stderr, "  tremolo cycles at control +1: %d\n", c);
    CHECK(c >= 16);    /* 10 Hz, at 5 ms windows the swings blur but stay many more */
    free(y); free(x);
    fa_midi_efx_free(e);
}

/* ---- whole songs ---------------------------------------------------------------------- */

static void gs(song* s, uint32_t ms, unsigned a1, unsigned a2, unsigned a3, unsigned v)
{
    uint8_t d[10] = { 0x41, 0x10, 0x42, 0x12, (uint8_t)a1, (uint8_t)a2, (uint8_t)a3, (uint8_t)v, 0, 0xF7 };
    d[8] = (uint8_t)((128 - (a1 + a2 + a3 + v) % 128) % 128);
    sysex(s, ms, d, sizeof(d));
}

static void xg(song* s, uint32_t ms, unsigned a1, unsigned a2, unsigned a3, unsigned v)
{
    uint8_t d[8] = { 0x43, 0x10, 0x4C, (uint8_t)a1, (uint8_t)a2, (uint8_t)a3, (uint8_t)v, 0xF7 };
    sysex(s, ms, d, sizeof(d));
}

static float* render_fx(const buf* f, int effects, fa_frames seek, fa_frames chunk)
{
    fa_open_options o;
    fa_stream* s = NULL;
    float* x = (float*)calloc((size_t)SECONDS * RATE * 2, sizeof(float));
    fa_frames done = 0, got;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    o.soundfont = g_font;
    if (fa_stream_open_memory(g_engine, f->d, f->n, &o, &s) != FA_OK) { free(x); return NULL; }
    fa_stream_set_midi_effects(s, effects);
    if (seek && fa_stream_seek(s, seek) != FA_OK) { fa_stream_close(s); free(x); return NULL; }
    while (done < (fa_frames)SECONDS * RATE - seek) {
        fa_frames want = (fa_frames)SECONDS * RATE - seek - done;
        if (want > chunk) want = chunk;
        if (fa_stream_read(s, x + done * 2, want, &got) != FA_OK || !got) break;
        done += got;
    }
    fa_stream_close(s);
    return x;
}

static int same(const float* a, const float* b, fa_frames frames)
{
    return memcmp(a, b, (size_t)frames * 2 * sizeof(float)) == 0;
}

static float* render_on(const buf* f) { return render_fx(f, 1, 0, 1000); }

/* A note (20-`off_ms`) on channel `ch` with no chorus send and a reverb send of g_cc91
 * (0), after `setup` (events up to 15 ms), running on to 1.9 s. */
typedef void (*setup_fn)(song* s, int arg);
static unsigned g_cc91 = 0;
static buf song_with(setup_fn setup, int arg, unsigned ch, uint32_t off_ms)
{
    song s = { 0 };
    if (setup) setup(&s, arg);
    ev3(&s, 16, 0xB0 | ch, 91, g_cc91);
    ev3(&s, 16, 0xB0 | ch, 93, 0);
    ev3(&s, 20, 0x90 | ch, 69, 127);
    ev3(&s, off_ms, 0x80 | ch, 69, 0);
    cc(&s, 1900, 2, 0);
    return finish(&s);
}

static void xg_reset(song* s) { xg(s, 0, 0x00, 0x00, 0x7E, 0x00); }
static void gs_reset(song* s) { gs(s, 0, 0x40, 0x00, 0x7F, 0x00); }
static void setup_xg(song* s, int arg) { (void)arg; xg_reset(s); }
static void setup_gs(song* s, int arg) { (void)arg; gs_reset(s); }

/* XG insertion 1 (Tremolo) on part `arg` + 1; with insertion 2 (Voice Cancel) on it too. */
static void setup_xg_ins1(song* s, int part)
{
    xg_reset(s);
    xg(s, 5, 0x03, 0x00, 0x00, 0x46);      /* insertion 1: Tremolo */
    xg(s, 5, 0x03, 0x00, 0x01, 0x00);
    xg(s, 6, 0x03, 0x00, 0x0C, (unsigned)part);
}
static void setup_xg_ins12(song* s, int part)
{
    setup_xg_ins1(s, part);
    xg(s, 7, 0x03, 0x01, 0x00, 0x55);      /* insertion 2: Voice Cancel */
    xg(s, 7, 0x03, 0x01, 0x01, 0x00);
    xg(s, 8, 0x03, 0x01, 0x0C, (unsigned)part);
}
/* insertion 2 alone on part 1 with an unknown type: dry */
static void setup_xg_unknown(song* s, int arg)
{
    (void)arg;
    xg_reset(s);
    xg(s, 5, 0x03, 0x01, 0x00, 0x7D);
    xg(s, 6, 0x03, 0x01, 0x0C, 0);
}
/* GS EFX Stereo-EQ (high shelf +12 dB) on part 1 (arg 1), on part 2 (arg 2) or on nothing. */
static void setup_gs_efx(song* s, int arg)
{
    gs_reset(s);
    gs(s, 5, 0x40, 0x03, 0x00, 0x01);
    gs(s, 5, 0x40, 0x03, 0x01, 0x00);
    gs(s, 5, 0x40, 0x03, 0x06, 0x4C);      /* high gain +12 dB */
    gs(s, 5, 0x40, 0x03, 0x05, 0x00);      /* at 4 kHz */
    gs(s, 5, 0x40, 0x03, 0x17, 0);         /* EFX send to reverb 0 */
    if (arg) gs(s, 6, 0x40, (unsigned)(0x40 + arg), 0x22, 1);
}
/* An unknown GS EFX type on part 1: dry (the part plays as without the EFX). */
static void setup_gs_unknown(song* s, int arg)
{
    (void)arg;
    gs_reset(s);
    gs(s, 5, 0x40, 0x03, 0x00, 0x01);
    gs(s, 5, 0x40, 0x03, 0x01, 0x7E);
    gs(s, 6, 0x40, 0x41, 0x22, 1);
}

/* XG insertion: on part 1 it changes part 1; on part 2 part 1 is bit-identical to no
 * insertion; insertion 2 after 1 changes it again; unknown types are dry; effects off
 * is the dry synth. */
static void test_song_xg_insertion(void)
{
    const fa_frames len = (fa_frames)SECONDS * RATE;
    buf dry = song_with(setup_xg, 0, 0, 1500), on = song_with(setup_xg_ins1, 0, 0, 1500);
    buf other = song_with(setup_xg_ins1, 1, 0, 1500), both = song_with(setup_xg_ins12, 0, 0, 1500);
    buf unk = song_with(setup_xg_unknown, 0, 0, 1500);
    float* a = render_on(&dry), *b = render_on(&on), *c = render_on(&other), *d = render_on(&both), *e = render_on(&unk);
    float* g = render_fx(&on, 0, 0, 1000), *h = render_fx(&dry, 0, 0, 1000);
    CHECK(a && b && c && d && e && g && h);
    CHECK(rms(a, 0.3, 1.4) > 0.01);
    CHECK(!same(a, b, len));
    CHECK(same(a, c, len));
    CHECK(!same(b, d, len));
    CHECK(rms(d, 0.3, 1.4) < rms(b, 0.3, 1.4) * 0.7);   /* Voice Cancel takes out the centre's mids */
    CHECK(same(a, e, len));
    CHECK(same(g, h, len));
    free(a); free(b); free(c); free(d); free(e); free(g); free(h);
    free(dry.d); free(on.d); free(other.d); free(both.d); free(unk.d);
}

/* GS EFX: the part switch puts part 1 through it (and its send levels in place of the
 * part's); on part 2 part 1 is bit-identical; an unknown type is dry. */
static void test_song_gs_efx(void)
{
    const fa_frames len = (fa_frames)SECONDS * RATE;
    buf dry = song_with(setup_gs, 0, 0, 120), on = song_with(setup_gs_efx, 1, 0, 120);
    buf other = song_with(setup_gs_efx, 2, 0, 120), none = song_with(setup_gs_efx, 0, 0, 120);
    buf unk = song_with(setup_gs_unknown, 0, 0, 120);
    float* a = render_on(&dry), *b = render_on(&on), *c = render_on(&other), *d = render_on(&none), *e = render_on(&unk);
    CHECK(a && b && c && d && e);
    CHECK(!same(a, b, len));
    CHECK(same(a, c, len));
    CHECK(same(a, d, len));
    CHECK(same(a, e, len));
    free(a); free(b); free(c); free(d); free(e);
    free(dry.d); free(on.d); free(other.d); free(none.d); free(unk.d);
}

/* The GS EFX's own reverb send: the part's CC91 127 is ignored while the part is on the
 * EFX (send 0: no tail), and the EFX send 127 gives one. */
static void setup_gs_send(song* s, int send)
{
    gs_reset(s);
    gs(s, 5, 0x40, 0x03, 0x17, (unsigned)send);
    gs(s, 6, 0x40, 0x41, 0x22, 1);
}
static void test_song_gs_efx_sends(void)
{
    buf s0, s127, part;
    float* a, *b, *c;
    g_cc91 = 127;
    s0 = song_with(setup_gs_send, 0, 0, 120);
    s127 = song_with(setup_gs_send, 127, 0, 120);
    part = song_with(setup_gs, 0, 0, 120);
    g_cc91 = 0;
    a = render_on(&s0);
    b = render_on(&s127);
    c = render_on(&part);
    CHECK(a && b && c);
    CHECK(rms(a, 0.02, 0.1) > 0.01);
    CHECK(rms(a, 0.5, 1.0) < 1e-6);
    CHECK(rms(b, 0.5, 1.0) > 1e-4);
    CHECK(rms(c, 0.5, 1.0) > 1e-4);      /* off the EFX, the part's own CC91 */
    free(a); free(b); free(c);
    free(s0.d); free(s127.d); free(part.d);
}

/* Control sources. GS: control 1 from CC16 at full depth on the Tremolo's rate; XG: the
 * insertion's AC1 (CC16) depth on its controlled parameter. CC16 moves the sound only
 * when a depth asks for it (CC1 would not do: the mod wheel is also the synth's vibrato). */
static void setup_gs_ctl(song* s, int arg)
{
    gs_reset(s);
    gs(s, 5, 0x40, 0x03, 0x00, 0x01);
    gs(s, 5, 0x40, 0x03, 0x01, 0x25);      /* Tremolo */
    gs(s, 5, 0x40, 0x03, 0x05, 127);
    gs(s, 5, 0x40, 0x03, 0x1B, 16);        /* control 1: CC16 */
    gs(s, 5, 0x40, 0x03, 0x1C, (unsigned)(arg & 1 ? 127 : 64));
    gs(s, 6, 0x40, 0x41, 0x22, 1);
    cc(s, 15, 16, arg & 2 ? 127 : 0);
}
static void setup_xg_ctl(song* s, int arg)
{
    xg_reset(s);
    xg(s, 5, 0x03, 0x00, 0x00, 0x56);      /* insertion 1: 2-way Rotary Speaker */
    xg(s, 5, 0x03, 0x00, 0x01, 0x00);
    xg(s, 5, 0x03, 0x00, 0x10, (unsigned)(arg & 1 ? 127 : 64));   /* AC1 depth */
    xg(s, 6, 0x03, 0x00, 0x0C, 0);
    cc(s, 15, 16, arg & 2 ? 127 : 0);
}
static void test_song_controls(void)
{
    const fa_frames len = (fa_frames)SECONDS * RATE;
    setup_fn fns[2] = { setup_gs_ctl, setup_xg_ctl };
    int k;
    for (k = 0; k < 2; k++) {
        buf off0 = song_with(fns[k], 0, 0, 1500), off1 = song_with(fns[k], 2, 0, 1500);
        buf on0 = song_with(fns[k], 1, 0, 1500), on1 = song_with(fns[k], 3, 0, 1500);
        float* a = render_on(&off0), *b = render_on(&off1), *c = render_on(&on0), *d = render_on(&on1);
        CHECK(a && b && c && d);
        CHECK(same(a, b, len));            /* depth 64 (none): CC16 does nothing */
        CHECK(same(a, c, len));            /* CC16 at 0 */
        CHECK(!same(c, d, len));
        free(a); free(b); free(c); free(d);
        free(off0.d); free(off1.d); free(on0.d); free(on1.d);
    }
}

/* Two parts on one EFX (Stereo Delay), any read size, and a seek into the echoes: the
 * same as reading through. */
static void test_song_reads_and_seek(void)
{
    const fa_frames len = (fa_frames)SECONDS * RATE, at = (fa_frames)(0.4 * RATE);
    song s = { 0 };
    buf f;
    float* a, *b, *c;
    gs_reset(&s);
    gs(&s, 5, 0x40, 0x03, 0x00, 0x01);
    gs(&s, 5, 0x40, 0x03, 0x01, 0x50);
    gs(&s, 6, 0x40, 0x41, 0x22, 1);
    gs(&s, 6, 0x40, 0x42, 0x22, 1);
    cc(&s, 10, 91, 0);
    ev3(&s, 10, 0xB1, 91, 0);
    ev3(&s, 20, 0x90, 69, 127);
    ev3(&s, 30, 0x91, 76, 100);
    ev3(&s, 120, 0x80, 69, 0);
    ev3(&s, 140, 0x81, 76, 0);
    cc(&s, 1900, 2, 0);
    f = finish(&s);
    a = render_on(&f);
    b = render_fx(&f, 1, 0, 777);
    c = render_fx(&f, 1, at, 1000);
    CHECK(a && b && c);
    CHECK(rms(a, 0.4, 0.5) > 1e-3);       /* the echoes (150 and 300 ms, repeating) */
    CHECK(same(a, b, len));
    CHECK(same(a + (size_t)at * 2, c, len - at));
    free(a); free(b); free(c); free(f.d);
}

/* A file with the GS EFX and an XG insertion-mode variation on one of the EFX's parts: the
 * EFX (Stereo Delay, wet only, 150 ms on the left) runs once a block on the sum, so a note
 * on the other EFX part still echoes 150 ms later, not sooner. */
static void test_song_var_and_efx(void)
{
    song s = { 0 };
    buf f;
    float* a;
    gs_reset(&s);
    gs(&s, 5, 0x40, 0x03, 0x00, 0x01);
    gs(&s, 5, 0x40, 0x03, 0x01, 0x50);
    gs(&s, 5, 0x40, 0x03, 0x05, 64);       /* no feedback */
    gs(&s, 5, 0x40, 0x03, 0x12, 127);      /* wet only */
    gs(&s, 6, 0x40, 0x41, 0x22, 1);
    gs(&s, 6, 0x40, 0x42, 0x22, 1);
    xg(&s, 7, 0x02, 0x01, 0x40, 0x40);     /* variation: Thru, inserted on part 1 */
    xg(&s, 7, 0x02, 0x01, 0x41, 0x00);
    xg(&s, 7, 0x02, 0x01, 0x5A, 0);
    xg(&s, 7, 0x02, 0x01, 0x5B, 0);
    ev3(&s, 10, 0xB1, 91, 0);
    ev3(&s, 20, 0x91, 69, 127);
    ev3(&s, 60, 0x81, 69, 0);
    cc(&s, 1900, 2, 0);
    f = finish(&s);
    a = render_on(&f);
    CHECK(a);
    CHECK(rms_ch(a, 0, frames_ms(25), frames_ms(55)) < 1e-6);    /* wet only: nothing yet */
    CHECK(rms_ch(a, 0, frames_ms(100), frames_ms(160)) < 1e-6);
    CHECK(rms_ch(a, 0, frames_ms(175), frames_ms(205)) > 1e-3);  /* the echo of 20-60 ms */
    free(a); free(f.d);
}

int main(void)
{
    fa_engine_config cfg;
    buf sf = make_sf2(NULL, 0, NULL, 0);
#if defined(__APPLE__)
    if (getenv("FA_TEST_CWD") && chdir(getenv("FA_TEST_CWD")) != 0) return 1;
#endif
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    cfg.worker_threads = 1;
    if (fa_engine_create(&cfg, &g_engine) != FA_OK) { fprintf(stderr, "engine create failed\n"); return 1; }
    if (fa_soundfont_open_memory(sf.d, sf.n, &g_font) != FA_OK) { fprintf(stderr, "test soundfont rejected\n"); return 1; }
    free(sf.d);
    RUN(test_counts);
    RUN(test_every_type);
    RUN(test_deterministic);
    RUN(test_gs_eq);
    RUN(test_gs_delay_taps);
    RUN(test_gs_pitch);
    RUN(test_xg_compressor);
    RUN(test_xg_gate);
    RUN(test_gs_tremolo_and_control);
    RUN(test_song_xg_insertion);
    RUN(test_song_gs_efx);
    RUN(test_song_gs_efx_sends);
    RUN(test_song_controls);
    RUN(test_song_reads_and_seek);
    RUN(test_song_var_and_efx);
    fa_soundfont_close(g_font);
    fa_engine_destroy(g_engine);
    if (g_failures) { fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    printf("all passed\n");
    return 0;
}
