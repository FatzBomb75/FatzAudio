/* MIDI effects beyond reverb and chorus: the GS reverb pre-delay, chorus sends to the
 * reverb and delay, the SC-88 delay, the XG chorus's own parameters and the XG
 * variation effect (system and insertion). White-box: the effect blocks (midi_fx.c,
 * midi_var.c) are driven directly and measured from impulse and sine responses; then
 * whole songs through the decoder check the routing (CC94, part sends, insertion),
 * GM files ignoring CC94, seeks and read sizes. */
#include "fa_internal.h"
#include "midi_synth_test.h"

#define BLOCK 64
#define CHECK_EQ(a, b) do { long long va_ = (long long)(a), vb_ = (long long)(b); if (va_ != vb_) { \
    fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_failures++; return; } } while (0)

/* `frames` of `in` (stereo) through the variation in 64-frame blocks; wet out. */
static float* var_run(fa_midi_var* v, const float* in, int frames)
{
    float* out = (float*)calloc((size_t)frames * 2, sizeof(float));
    int off;
    for (off = 0; off < frames; off += BLOCK) {
        int n = frames - off < BLOCK ? frames - off : BLOCK;
        if (!fa_midi_var_process(v, in + off * 2, out + off * 2, n, off)) memset(out + off * 2, 0, (size_t)n * 2 * sizeof(float));
    }
    return out;
}

static float* impulse(int frames, float l, float r)
{
    float* x = (float*)calloc((size_t)frames * 2, sizeof(float));
    x[0] = l;
    x[1] = r;
    return x;
}

static float* sine(int frames, double hz, float amp)
{
    float* x = (float*)malloc((size_t)frames * 2 * sizeof(float));
    int i;
    for (i = 0; i < frames; i++) x[i * 2] = x[i * 2 + 1] = (float)(amp * sin(2.0 * PI * hz * i / RATE));
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

/* ---- tables ---------------------------------------------------------------------- */

static void test_xg_tables(void)
{
    int v;
    CHECK_NEAR(fa_xg_lfo_hz(0), 0.0, 1e-9);
    CHECK_NEAR(fa_xg_lfo_hz(64), 2.69, 1e-9);
    CHECK(fa_xg_lfo_hz(127) <= 39.7 && fa_xg_lfo_hz(127) > 30.0);
    for (v = 1; v < 128; v++) CHECK(fa_xg_lfo_hz(v) > fa_xg_lfo_hz(v - 1) || fa_xg_lfo_hz(v) == 39.7);
    CHECK_NEAR(fa_xg_mod_delay_ms(100), 10.0, 1e-9);
    CHECK_NEAR(fa_xg_feedback(64), 0.0, 1e-9);
    CHECK_NEAR(fa_xg_feedback(96), 0.5, 1e-6);
    CHECK_NEAR(fa_xg_feedback(1), -0.98, 1e-6);
}

/* ---- delays ---------------------------------------------------------------------- */

/* SC-88 Delay 1: the center tap at 340 ms on both sides, repeated at a quarter (feedback
 * +16); Pan Delay 1: left at half the center time, right at the center time. */
static void test_gs_delay(void)
{
    const int n = RATE * 2;
    fa_midi_var* v = fa_midi_var_new(RATE);
    float* in = impulse(n, 1.0f, 1.0f), *out;
    int c = frames_ms(340.0);
    fa_midi_var_gs(v, 0x50, 0);
    CHECK_EQ(fa_midi_var_gs_get(v, 0x58), 64);
    out = var_run(v, in, n);
    CHECK_NEAR(out[c * 2], 1.0, 1e-6);
    CHECK_NEAR(out[c * 2 + 1], 1.0, 1e-6);
    CHECK_NEAR(out[2 * c * 2], 0.25, 1e-6);
    CHECK_NEAR(rms_ch(out, 0, 0, c), 0.0, 0.0);
    CHECK_NEAR(rms_ch(out, 0, c + 1, 2 * c), 0.0, 0.0);
    free(out);
    fa_midi_var_gs(v, 0x50, 4);   /* Pan Delay 1: center 500 ms, left 50% (level 125), right 100% (level 60) */
    out = var_run(v, in, n);
    CHECK_NEAR(out[frames_ms(250.0) * 2], 125.0 / 127.0, 1e-6);
    CHECK_NEAR(out[frames_ms(250.0) * 2 + 1], 0.0, 0.0);
    CHECK_NEAR(out[frames_ms(500.0) * 2 + 1], 60.0 / 127.0, 1e-6);
    free(out);
    /* Feedback 0 (-64 .. +63: 64 is none): one echo only. */
    fa_midi_var_gs(v, 0x50, 0);
    fa_midi_var_gs(v, 0x59, 64);
    out = var_run(v, in, n);
    CHECK_NEAR(out[c * 2], 1.0, 1e-6);
    CHECK_NEAR(rms_ch(out, 0, c + 1, n), 0.0, 0.0);
    free(out);
    free(in);
    fa_midi_var_free(v);
}

/* XG Delay L,C,R, Cross Delay and Karaoke at their default delay times. */
static void test_xg_delays(void)
{
    const int n = RATE * 2;
    fa_midi_var* v = fa_midi_var_new(RATE);
    float* in = impulse(n, 1.0f, 1.0f), *left = impulse(n, 1.0f, 0.0f), *out;
    int l = frames_ms(333.3), r = frames_ms(166.7), c = frames_ms(500.0), k;
    CHECK(fa_midi_var_set_type(v, 0x05, 0));
    out = var_run(v, in, n);
    CHECK_NEAR(out[l * 2], 1.0, 1e-6);
    CHECK_NEAR(out[l * 2 + 1], 0.0, 0.0);
    CHECK_NEAR(out[r * 2 + 1], 1.0, 1e-6);
    CHECK_NEAR(out[c * 2], 100.0 / 127.0, 1e-6);
    CHECK_NEAR(out[c * 2 + 1], 100.0 / 127.0, 1e-6);
    /* Feedback +10 from 500 ms: the left tap again, 333.3 ms after that. */
    CHECK_NEAR(out[(c + l) * 2], 10.0 / 64.0, 1e-6);
    free(out);

    /* Cross delay, left input: right after 170 ms, back to the left 175 ms later at +47. */
    CHECK(fa_midi_var_set_type(v, 0x08, 0));
    fa_midi_var_set_param(v, 3, 2, 0);   /* input L&R */
    out = var_run(v, left, n);
    k = frames_ms(170.0);
    CHECK_NEAR(out[k * 2 + 1], 1.0, 1e-6);
    CHECK_NEAR(out[k * 2], 0.0, 0.0);
    CHECK_NEAR(out[(k + frames_ms(175.0)) * 2], 47.0 / 64.0, 1e-6);
    free(out);

    /* Karaoke 1 without its lowpass: 192 ms echoes, each 33/64 of the last. Parameter 1
     * is two bytes: MSB then LSB. */
    CHECK(fa_midi_var_set_type(v, 0x14, 0));
    fa_midi_var_set_param(v, 3, 0, 0);
    out = var_run(v, in, n);
    k = frames_ms(192.0);
    CHECK_NEAR(out[k * 2], 1.0, 1e-6);
    CHECK_NEAR(out[2 * k * 2 + 1], 33.0 / 64.0, 1e-6);
    free(out);
    fa_midi_var_set_param(v, 0, 0, 1);
    fa_midi_var_set_param(v, 0, 32, 2);   /* 33 * 3 ms */
    out = var_run(v, in, n);
    CHECK_NEAR(out[frames_ms(99.0) * 2], 1.0, 1e-6);
    free(out);
    free(in);
    free(left);
    fa_midi_var_free(v);
}

/* The reflection and gate types: taps after the initial delay, and nothing before. */
static void test_reflections(void)
{
    const int n = RATE;
    static const uint8_t types[3] = { 0x09, 0x0A, 0x0B };
    fa_midi_var* v = fa_midi_var_new(RATE);
    float* in = impulse(n, 1.0f, 1.0f);
    int t;
    for (t = 0; t < 3; t++) {
        float* out;
        int first = -1, last = -1, i;
        CHECK(fa_midi_var_set_type(v, types[t], 0));
        fa_midi_var_set_param(v, 4, 64, 0);   /* no feedback */
        fa_midi_var_set_param(v, 5, 0, 0);    /* no filters */
        fa_midi_var_set_param(v, 6, 60, 0);
        out = var_run(v, in, n);
        for (i = 0; i < n; i++) if (out[i * 2] != 0.0f || out[i * 2 + 1] != 0.0f) { if (first < 0) first = i; last = i; }
        CHECK(first >= frames_ms(1.0) && first < frames_ms(5.0));   /* initial delay 16: 1.6 ms (+1 ms) */
        CHECK(last > first + frames_ms(30.0) && last < frames_ms(500.0));
        if (t == 2) CHECK(fabs(out[last * 2]) + fabs(out[last * 2 + 1]) > fabs(out[first * 2]) + fabs(out[first * 2 + 1]));   /* reverse: grows */
        free(out);
    }
    free(in);
    fa_midi_var_free(v);
}

/* ---- LFO types ------------------------------------------------------------------- */

/* Upward crossings of the middle of channel `ch` between frames a and b. */
static int cycles(const float* x, int ch, int a, int b)
{
    float lo = 1e9f, hi = -1e9f, mid;
    int i, n = 0;
    for (i = a; i < b; i++) { if (x[i * 2 + ch] < lo) lo = x[i * 2 + ch]; if (x[i * 2 + ch] > hi) hi = x[i * 2 + ch]; }
    mid = (lo + hi) / 2;
    for (i = a + 1; i < b; i++) if (x[(i - 1) * 2 + ch] < mid && x[i * 2 + ch] >= mid) n++;
    return n;
}

static void test_tremolo_and_pan(void)
{
    const int n = RATE * 4;
    fa_midi_var* v = fa_midi_var_new(RATE);
    float* dc = (float*)malloc((size_t)n * 2 * sizeof(float)), *out;
    float lo = 1e9f, hi = 0.0f;
    int i;
    for (i = 0; i < n * 2; i++) dc[i] = 0.5f;
    CHECK(fa_midi_var_set_type(v, 0x46, 0));   /* tremolo: LFO 83, depth 56 */
    out = var_run(v, dc, n);
    CHECK(abs(cycles(out, 0, 0, n) - (int)lrint(fa_xg_lfo_hz(83) * 4.0)) <= 1);
    for (i = 0; i < n; i++) { if (out[i * 2] < lo) lo = out[i * 2]; if (out[i * 2] > hi) hi = out[i * 2]; }
    CHECK_NEAR(hi, 0.5, 1e-4);
    CHECK_NEAR(lo, 0.5 * (1.0 - 56.0 / 127.0), 1e-3);
    free(out);
    CHECK(fa_midi_var_set_type(v, 0x47, 0));   /* auto pan: the two sides move against each other */
    fa_midi_var_set_param(v, 1, 0, 1);         /* L/R depth 127 (MSB 0, LSB 127) */
    fa_midi_var_set_param(v, 1, 127, 2);
    out = var_run(v, dc, n);
    for (i = 0; i < n; i += 97) CHECK_NEAR(out[i * 2] + out[i * 2 + 1], 0.5, 1e-5);
    CHECK(abs(cycles(out, 0, 0, n) - (int)lrint(fa_xg_lfo_hz(76) * 4.0)) <= 1);
    free(out);
    free(dc);
    fa_midi_var_free(v);
}

/* The XG chorus's own parameters on the system chorus: LFO 0 makes a plain delay at the
 * offset, and the feedback repeats it. */
static void test_xg_chorus_params(void)
{
    const int n = RATE / 4;
    fa_midi_fx* fx = fa_midi_fx_new(RATE);
    float* in = impulse(n, 1.0f, 1.0f), *out = (float*)calloc((size_t)n * 2, sizeof(float));
    int off, d = frames_ms(10.0);
    fa_midi_fx_xg(fx, 0x22, 0);     /* LFO 0 Hz */
    fa_midi_fx_xg(fx, 0x25, 100);   /* delay offset 10 ms */
    fa_midi_fx_xg(fx, 0x24, 96);    /* feedback +32 */
    for (off = 0; off < n; off += BLOCK) fa_midi_fx_chorus(fx, in + off * 2, out + off * 2, n - off < BLOCK ? n - off : BLOCK, off);
    CHECK_NEAR(out[d * 2], 1.0, 1e-6);
    CHECK_NEAR(out[(2 * d + 1) * 2], 0.5 * 0.9, 1e-6);   /* the feedback takes the last frame out */
    CHECK_NEAR(rms_ch(out, 0, 0, d), 0.0, 0.0);
    /* A new chorus type puts the type's own parameters back. */
    fa_midi_fx_xg(fx, 0x20, 0x41);
    fa_midi_fx_reset(fx);
    memset(out, 0, (size_t)n * 2 * sizeof(float));
    for (off = 0; off < n; off += BLOCK) fa_midi_fx_chorus(fx, in + off * 2, out + off * 2, n - off < BLOCK ? n - off : BLOCK, off);
    CHECK(fabs(out[d * 2]) < 0.5);
    free(in);
    free(out);
    fa_midi_fx_free(fx);
}

/* ---- routing in midi_fx.c ---------------------------------------------------------- */

static float* fx_run(fa_midi_fx* fx, const float* rin, const float* cin, const float* vin, int frames)
{
    float* out = (float*)calloc((size_t)frames * 2, sizeof(float));
    int off;
    for (off = 0; off < frames; off += BLOCK)
        fa_midi_fx_process(fx, rin + off * 2, cin + off * 2, vin ? vin + off * 2 : NULL, out + off * 2,
                           frames - off < BLOCK ? frames - off : BLOCK, off);
    return out;
}

static int first_sound(const float* x, int frames)
{
    int i;
    for (i = 0; i < frames; i++) if (x[i * 2] != 0.0f || x[i * 2 + 1] != 0.0f) return i;
    return -1;
}

static void test_predelay_and_sends(void)
{
    const int n = RATE;
    fa_midi_fx* fx = fa_midi_fx_new(RATE);
    float* imp = impulse(n, 1.0f, 1.0f), *zero = (float*)calloc((size_t)n * 2, sizeof(float)), *a, *b;
    int t0;
    /* Pre-delay 100 ms: the whole reverb 100 ms later, otherwise the same. */
    fa_midi_fx_defaults(fx, FA_MIDI_FX_GS);
    a = fx_run(fx, imp, zero, NULL, n);
    fa_midi_fx_reset(fx);
    fa_midi_fx_gs(fx, 0x37, 100);
    b = fx_run(fx, imp, zero, NULL, n);
    t0 = first_sound(a, n);
    CHECK(t0 > 0);
    CHECK_EQ(first_sound(b, n), t0 + frames_ms(100.0));
    CHECK_NEAR(b[(t0 + frames_ms(100.0)) * 2], a[t0 * 2], 1e-7);
    free(a); free(b);
    /* Chorus to reverb: the chorus alone is over in 30 ms; sent on, a reverb tail follows. */
    fa_midi_fx_defaults(fx, FA_MIDI_FX_GS);
    fa_midi_fx_reset(fx);
    a = fx_run(fx, zero, imp, NULL, n);
    CHECK(first_sound(a, n) >= 0);
    CHECK_NEAR(rms_ch(a, 0, frames_ms(100.0), n), 0.0, 0.0);
    fa_midi_fx_reset(fx);
    fa_midi_fx_gs(fx, 0x3F, 127);
    b = fx_run(fx, zero, imp, NULL, n);
    CHECK(rms_ch(b, 0, frames_ms(100.0), frames_ms(400.0)) > 1e-5);
    free(a); free(b);
    /* Chorus to delay (SC-88): the chorus comes back at the delay time. */
    fa_midi_fx_reset(fx);
    fa_midi_fx_gs(fx, 0x40, 127);
    b = fx_run(fx, zero, imp, zero, n);
    CHECK(rms_ch(b, 0, frames_ms(340.0), frames_ms(380.0)) > 1e-4);
    free(b);
    free(imp);
    free(zero);
    fa_midi_fx_free(fx);
}

static void test_route(void)
{
    fa_midi_fx* fx = fa_midi_fx_new(RATE);
    CHECK_EQ(fa_midi_fx_route(fx), -2);                 /* GM: no third effect */
    fa_midi_fx_defaults(fx, FA_MIDI_FX_GS);
    CHECK_EQ(fa_midi_fx_route(fx), -1);                 /* GS: CC94 to the delay */
    fa_midi_fx_defaults(fx, FA_MIDI_FX_XG);
    CHECK_EQ(fa_midi_fx_route(fx), -2);                 /* XG: insertion, no part */
    fa_midi_fx_xg(fx, 0x5B, 3);
    CHECK_EQ(fa_midi_fx_route(fx), 3);
    fa_midi_fx_xg(fx, 0x5A, 1);
    CHECK_EQ(fa_midi_fx_route(fx), -1);                 /* system */
    fa_midi_fx_xg(fx, 0x40, 0x00);
    CHECK_EQ(fa_midi_fx_route(fx), -2);                 /* no effect */
    fa_midi_fx_xg(fx, 0x40, 0x5E);
    CHECK_EQ(fa_midi_fx_route(fx), -2);                 /* a type we don't play */
    fa_midi_fx_xg(fx, 0x40, 0x41);
    CHECK_EQ(fa_midi_fx_route(fx), -1);
    fa_midi_fx_defaults(fx, FA_MIDI_FX_GM);
    CHECK_EQ(fa_midi_fx_route(fx), -2);
    fa_midi_fx_gs(fx, 0x58, 100);                       /* GS delay SysEx in a GM file */
    CHECK_EQ(fa_midi_fx_route(fx), -1);
    fa_midi_fx_free(fx);
}

/* Insertion: the dry/wet mix (Thru: dry either way, to the bit), and the part's sends. */
static void test_insert_mix(void)
{
    const int n = 1024;
    fa_midi_fx* fx = fa_midi_fx_new(RATE);
    float* x = sine(n, 440.0, 0.3f), *out = (float*)calloc(n * 2, sizeof(float));
    float* rev = (float*)calloc(n * 2, sizeof(float)), *cho = (float*)calloc(n * 2, sizeof(float));
    int i;
    fa_midi_fx_defaults(fx, FA_MIDI_FX_XG);
    fa_midi_fx_xg(fx, 0x40, 0x40);   /* Thru */
    fa_midi_fx_xg(fx, 0x5B, 0);
    fa_midi_fx_insert(fx, x, out, rev, cho, 0.5f, 0.25f, n, 0);
    for (i = 0; i < n * 2; i++) {
        CHECK(out[i] == x[i]);
        CHECK(rev[i] == x[i] * 0.5f);
        CHECK(cho[i] == x[i] * 0.25f);
    }
    /* Distortion, all wet: louder than the clean sine and clipped flat on top. */
    memset(out, 0, n * 2 * sizeof(float));
    fa_midi_fx_xg(fx, 0x40, 0x49);
    fa_midi_fx_insert(fx, x, out, rev, cho, 0.0f, 0.0f, n, 0);
    CHECK(rms_ch(out, 0, 256, n) > 0.0);
    CHECK(fabs(rms_ch(out, 0, 256, n) - rms_ch(x, 0, 256, n)) > 0.01);
    free(x); free(out); free(rev); free(cho);
    fa_midi_fx_free(fx);
}

/* ---- drive and EQ ------------------------------------------------------------------ */

static void test_distortion_curve(void)
{
    const int n = RATE / 2;
    static const uint8_t types[3] = { 0x49, 0x4A, 0x4B };
    int t;
    for (t = 0; t < 3; t++) {
        fa_midi_var* v = fa_midi_var_new(RATE);
        float* quiet = sine(n, 220.0, 0.01f), *loud = sine(n, 220.0, 0.8f), *a, *b;
        CHECK(fa_midi_var_set_type(v, types[t], 0));
        a = var_run(v, quiet, n);
        fa_midi_var_reset(v);
        b = var_run(v, loud, n);
        /* 80 times the input, at most half that out: the clipper squashes it. */
        CHECK(rms_ch(b, 0, n / 2, n) / rms_ch(a, 0, n / 2, n) < 40.0);
        CHECK(rms_ch(b, 0, n / 2, n) / rms_ch(a, 0, n / 2, n) > 1.0);
        free(quiet); free(loud); free(a); free(b);
        fa_midi_var_free(v);
    }
}

/* The gain of a variation at `hz` (dB): a sine through it. */
static double gain_db(fa_midi_var* v, double hz)
{
    const int n = RATE / 2;
    float* x = sine(n, hz, 0.1f), *y;
    double g;
    fa_midi_var_reset(v);
    y = var_run(v, x, n);
    g = 20.0 * log10(rms_ch(y, 0, n / 2, n) / rms_ch(x, 0, n / 2, n));
    free(x);
    free(y);
    return g;
}

static void test_eq(void)
{
    fa_midi_var* v = fa_midi_var_new(RATE);
    CHECK(fa_midi_var_set_type(v, 0x4C, 0));   /* 3-band: low +6 (shelf at 508 Hz), high +6 (4 kHz) */
    CHECK_NEAR(gain_db(v, 40.0), 6.0, 0.3);
    CHECK_NEAR(gain_db(v, 16000.0), 6.0, 0.5);
    fa_midi_var_set_param(v, 0, 64, 2);        /* low and high flat: the mid band alone, -4 dB at 1 kHz */
    fa_midi_var_set_param(v, 4, 64, 2);
    CHECK_NEAR(gain_db(v, 20.0 * pow(2.0, 34.0 / 6.0)), -4.0, 0.2);
    CHECK_NEAR(gain_db(v, 100.0), 0.0, 0.3);
    CHECK(fa_midi_var_set_type(v, 0x4D, 0));   /* 2-band: low +6, high +6 */
    CHECK_NEAR(gain_db(v, 40.0), 6.0, 0.3);
    CHECK_NEAR(gain_db(v, 1000.0 * 1.4), 0.0, 1.5);
    fa_midi_var_free(v);
}

/* Every type at its defaults: bounded on loud noise, and once the input stops the state runs out and the
 * effect goes silent (process returns 0: no cost). */
static void test_every_type_settles(void)
{
    static const uint8_t types[][2] = {
        { 0x01, 0 }, { 0x02, 2 }, { 0x05, 0 }, { 0x06, 0 }, { 0x07, 0 }, { 0x08, 0 }, { 0x09, 1 }, { 0x0A, 0 },
        { 0x0B, 0 }, { 0x13, 0 }, { 0x14, 1 }, { 0x40, 0 }, { 0x41, 8 }, { 0x42, 2 }, { 0x43, 1 }, { 0x44, 0 },
        { 0x45, 0 }, { 0x46, 0 }, { 0x47, 0 }, { 0x48, 1 }, { 0x49, 0 }, { 0x4A, 0 }, { 0x4B, 0 }, { 0x4C, 0 },
        { 0x4D, 0 }, { 0x4E, 0 },
    };
    const int n = RATE, quiet = RATE * 30;
    float* noise = (float*)malloc((size_t)n * 2 * sizeof(float));
    float* zero = (float*)calloc(BLOCK * 2, sizeof(float)), wet[BLOCK * 2];
    uint32_t seed = 1;
    size_t t;
    int i;
    for (i = 0; i < n * 2; i++) { seed = seed * 1664525u + 1013904223u; noise[i] = (float)((int32_t)seed / 2147483648.0 * 0.8); }
    for (t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
        fa_midi_var* v = fa_midi_var_new(RATE);
        float* out;
        double peak = 0;
        int off, settled = -1;
        CHECK(fa_midi_var_set_type(v, types[t][0], types[t][1]));
        out = var_run(v, noise, n);
        for (i = 0; i < n * 2; i++) { CHECK(out[i] == out[i]); if (fabs(out[i]) > peak) peak = fabs(out[i]); }
        if (peak > 8.0) { fprintf(stderr, "  type %02X %d peak %.2f\n", types[t][0], types[t][1], peak); CHECK(peak <= 8.0); }
        for (off = n; off < n + quiet; off += BLOCK)
            if (!fa_midi_var_process(v, zero, wet, BLOCK, off)) { settled = off; break; }
        if (settled < 0) fprintf(stderr, "  type %02X %d never settles\n", types[t][0], types[t][1]);
        CHECK(settled > 0);
        free(out);
        fa_midi_var_free(v);
    }
    free(noise);
    free(zero);
}

/* ---- whole songs ------------------------------------------------------------------- */

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

/* 2 s of `f` with effects on (or off), read `chunk` frames at a time, from `seek`. */
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

/* A short note (0-100 ms) on part 1 with no reverb or chorus send, after `setup`. */
typedef void (*setup_fn)(song* s, int arg);
static buf short_note(setup_fn setup, int arg)
{
    song s = { 0 };
    if (setup) setup(&s, arg);
    cc(&s, 16, 91, 0);
    cc(&s, 16, 93, 0);
    note(&s, 20, 120, 69);
    cc(&s, 1900, 1, 0);   /* the song runs on to 1.9 s */
    return finish(&s);
}

static void setup_gs(song* s, int cc94) { gs(s, 0, 0x40, 0x00, 0x7F, 0x00); if (cc94 >= 0) cc(s, 10, 94, (unsigned)cc94); }
static void setup_gs_part(song* s, int v) { gs(s, 0, 0x40, 0x00, 0x7F, 0x00); gs(s, 5, 0x40, 0x11, 0x2C, (unsigned)v); }
static void setup_gs_level0(song* s, int v) { setup_gs(s, v); gs(s, 15, 0x40, 0x01, 0x58, 0); }
static void setup_gm(song* s, int cc94) { if (cc94 >= 0) cc(s, 10, 94, (unsigned)cc94); }
static void setup_xg_system(song* s, int cc94)
{
    xg(s, 0, 0x00, 0x00, 0x7E, 0x00);
    xg(s, 5, 0x02, 0x01, 0x5A, 1);
    if (cc94 >= 0) cc(s, 10, 94, (unsigned)cc94);
}
static void setup_xg_insert(song* s, int part)
{
    xg(s, 0, 0x00, 0x00, 0x7E, 0x00);
    xg(s, 5, 0x02, 0x01, 0x40, 0x46);   /* tremolo */
    xg(s, 6, 0x02, 0x01, 0x5B, (unsigned)part);
}

static int same(const float* a, const float* b, fa_frames frames)
{
    return memcmp(a, b, (size_t)frames * 2 * sizeof(float)) == 0;
}

/* GS: CC94 (or the part's delay send) to the SC-88 delay, 340 ms later; delay level 0
 * or no send: none. GM files: CC94 does nothing. */
static void test_song_gs_delay(void)
{
    const fa_frames len = (fa_frames)SECONDS * RATE;
    buf none = short_note(setup_gs, -1), send = short_note(setup_gs, 127), part = short_note(setup_gs_part, 127);
    buf level0 = short_note(setup_gs_level0, 127), gm = short_note(setup_gm, -1), gm94 = short_note(setup_gm, 127);
    float* a = render_fx(&none, 1, 0, 1000), *b = render_fx(&send, 1, 0, 1000), *c = render_fx(&part, 1, 0, 1000);
    float* d = render_fx(&level0, 1, 0, 1000), *e = render_fx(&gm, 1, 0, 1000), *g = render_fx(&gm94, 1, 0, 1000);
    CHECK(a && b && c && d && e && g);
    CHECK(rms(a, 0.02, 0.1) > 0.01);
    CHECK(rms(a, 0.4, 0.45) < 1e-6);                /* the dry note is over */
    CHECK(rms(b, 0.4, 0.45) > 1e-3);                /* its echo (at 0.36-0.46 s) */
    CHECK(same(b, c, len));                         /* the part's delay send is CC94 */
    CHECK(same(a, d, len));                         /* delay level 0 */
    CHECK(same(e, g, len));                         /* GM: no delay */
    free(a); free(b); free(c); free(d); free(e); free(g);
    free(none.d); free(send.d); free(part.d); free(level0.d); free(gm.d); free(gm94.d);
}

/* XG: CC94 to the variation in system mode (Delay L,C,R: the left tap at 333 ms); the
 * same with effects off is the dry synth. */
static void test_song_xg_system(void)
{
    const fa_frames len = (fa_frames)SECONDS * RATE;
    buf none = short_note(setup_xg_system, 0), send = short_note(setup_xg_system, 127);
    float* a = render_fx(&none, 1, 0, 1000), *b = render_fx(&send, 1, 0, 1000);
    float* c = render_fx(&send, 0, 0, 1000), *d = render_fx(&none, 0, 0, 1000);
    float* e = render_fx(&send, 1, 0, 777);
    CHECK(a && b && c && d && e);
    CHECK(rms(a, 0.4, 0.45) < 1e-6);
    CHECK(rms(b, 0.4, 0.45) > 1e-3);
    CHECK(same(c, d, len));          /* effects off */
    CHECK(same(b, e, len));          /* any read size */
    /* A seek into the echoes renders the second before it: the same as reading through. */
    free(e);
    e = render_fx(&send, 1, (fa_frames)(0.4 * RATE), 1000);
    CHECK(e && same(b + (size_t)(0.4 * RATE) * 2, e, len - (fa_frames)(0.4 * RATE)));
    free(a); free(b); free(c); free(d); free(e);
    free(none.d); free(send.d);
}

/* XG insertion: the tremolo on part 1 swings the note's level; on part 2 it leaves
 * part 1 alone. */
static void test_song_xg_insert(void)
{
    const fa_frames len = (fa_frames)SECONDS * RATE;
    song s1 = { 0 }, s2 = { 0 }, s0 = { 0 };
    buf on, off, dry;
    float* a, *b, *c;
    double lo = 1e9, hi = 0;
    int k;
    setup_xg_insert(&s1, 0);
    setup_xg_insert(&s2, 1);
    xg(&s0, 0, 0x00, 0x00, 0x7E, 0x00);
    cc(&s1, 10, 91, 0); cc(&s2, 10, 91, 0); cc(&s0, 10, 91, 0);
    note(&s1, 20, 1500, 69); note(&s2, 20, 1500, 69); note(&s0, 20, 1500, 69);
    on = finish(&s1); off = finish(&s2); dry = finish(&s0);
    a = render_fx(&on, 1, 0, 1000);
    b = render_fx(&off, 1, 0, 1000);
    c = render_fx(&dry, 1, 0, 1000);
    CHECK(a && b && c);
    CHECK(same(b, c, len));
    for (k = 0; k < 40; k++) {
        double r = rms(a, 0.3 + k * 0.025, 0.3 + (k + 1) * 0.025);
        if (r < lo) lo = r;
        if (r > hi) hi = r;
    }
    CHECK(hi / lo > 1.15);  /* depth 56 dips the wet half to 0.56: 0.78 of the level */
    free(a); free(b); free(c);
    free(on.d); free(off.d); free(dry.d);
}

/* GS drum map / XG drum setup: a key's delay or variation send. */
static void test_song_drum_send(void)
{
    const fa_frames len = (fa_frames)SECONDS * RATE;
    song s1 = { 0 }, s2 = { 0 };
    buf a, b;
    float* x, *y;
    gs(&s1, 0, 0x40, 0x00, 0x7F, 0x00);
    gs(&s2, 0, 0x40, 0x00, 0x7F, 0x00);
    gs(&s2, 5, 0x41, 0x09, 69, 0);     /* map 1, key 69: delay send 0 */
    ev3(&s1, 10, 0xB9, 94, 127); ev3(&s2, 10, 0xB9, 94, 127);
    ev3(&s1, 10, 0xB9, 91, 0); ev3(&s2, 10, 0xB9, 91, 0);
    ev3(&s1, 20, 0x99, 69, 127); ev3(&s2, 20, 0x99, 69, 127);
    ev3(&s1, 120, 0x89, 69, 0); ev3(&s2, 120, 0x89, 69, 0);
    ev3(&s1, 1900, 0xB0, 1, 0); ev3(&s2, 1900, 0xB0, 1, 0);
    a = finish(&s1);
    b = finish(&s2);
    x = render_fx(&a, 1, 0, 1000);
    y = render_fx(&b, 1, 0, 1000);
    CHECK(x && y);
    CHECK(rms(x, 0.4, 0.45) > 1e-3);
    CHECK(rms(y, 0.4, 0.45) < 1e-6);
    CHECK(!same(x, y, len));
    free(x); free(y); free(a.d); free(b.d);
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
    RUN(test_xg_tables);
    RUN(test_gs_delay);
    RUN(test_xg_delays);
    RUN(test_reflections);
    RUN(test_tremolo_and_pan);
    RUN(test_xg_chorus_params);
    RUN(test_predelay_and_sends);
    RUN(test_route);
    RUN(test_insert_mix);
    RUN(test_distortion_curve);
    RUN(test_eq);
    RUN(test_every_type_settles);
    RUN(test_song_gs_delay);
    RUN(test_song_xg_system);
    RUN(test_song_xg_insert);
    RUN(test_song_drum_send);
    fa_soundfont_close(g_font);
    fa_engine_destroy(g_engine);
    if (g_failures) { fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    printf("all passed\n");
    return 0;
}
