/* MIDI controllers and SoundFont modulators: the mod wheel and pressure vibrato (with
 * RPN 5 and the GS reset), the sound controllers CC71-78 and the GS/XG part NRPNs and
 * SysEx that set them, portamento, sostenuto, the soft pedal, a soundfont's own
 * modulators, resets and seeks. Each test renders a sine soundfont built here and
 * measures what changed: pitch per cycle, level, filter response, envelope times. */
#include "fatzaudio/fatzaudio.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; return; } } while (0)
#define CHECK_NEAR(a, b, tol) do { double va_ = (a), vb_ = (b); if (fabs(va_ - vb_) > (tol)) { \
    fprintf(stderr, "  FAIL %s:%d: %s ~ %s (%.6f vs %.6f)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_failures++; return; } } while (0)
#define RUN(t) do { const char* only_ = getenv("FA_TEST"); int before_ = g_failures; \
    if (only_ && strcmp(only_, #t) != 0) break; \
    printf("%s\n", #t); t(); if (g_failures == before_) printf("  ok\n"); } while (0)

#define RATE 44100
#define PI 3.14159265358979323846
#define SINE_LEN 4410   /* 100 periods of 440 Hz: loops seamlessly */
#define SECONDS 2

static fa_engine* g_engine;
static fa_soundfont* g_font;

/* ---- byte buffers, soundfont and MIDI files ---------------------------------------- */

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
static void put32be(buf* b, uint32_t v) { put8(b, v >> 24); put8(b, (v >> 16) & 255); put8(b, (v >> 8) & 255); put8(b, v & 255); }
static void putstr(buf* b, const char* s, size_t n) { size_t k = strlen(s); put(b, s, k < n ? k : n); while (k++ < n) put8(b, 0); }
static void chunk(buf* b, const char* id, const buf* body)
{
    put(b, id, 4);
    put32le(b, (uint32_t)body->n);
    put(b, body->d, body->n);
    if (body->n & 1) put8(b, 0);
}
static void gen(buf* b, unsigned oper, int amount) { put16le(b, oper); put16le(b, (unsigned)(uint16_t)(int16_t)amount); }

/* A modulator record: source, destination, amount, amount source, transform. */
typedef struct sfmod { unsigned src, dest; int amount; unsigned amt_src, trans; } sfmod;

static void mods(buf* b, const sfmod* m, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        put16le(b, m[i].src); put16le(b, m[i].dest); put16le(b, (unsigned)(uint16_t)(int16_t)m[i].amount);
        put16le(b, m[i].amt_src); put16le(b, m[i].trans);
    }
    for (i = 0; i < 10; i++) put8(b, 0);
}

/* Preset 0 bank 0: a looped 440 Hz sine, root key 69, instant attack, full sustain,
 * 125 ms release, with `imod` on its instrument zone and `pmod` on its preset zone. */
static buf make_sf2(const sfmod* imod, int ni, const sfmod* pmod, int np)
{
    buf sf = { 0 }, riff = { 0 }, info = { 0 }, sdta = { 0 }, pdta = { 0 }, c = { 0 };
    int i;
    put(&riff, "sfbk", 4);
    put(&info, "INFO", 4);
    c.n = 0; put16le(&c, 2); put16le(&c, 4); chunk(&info, "ifil", &c);
    c.n = 0; putstr(&c, "Mod Sine", 10); chunk(&info, "INAM", &c);
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
    putstr(&c, "EOP", 20); put16le(&c, 0); put16le(&c, 0); put16le(&c, 1); put32le(&c, 0); put32le(&c, 0); put32le(&c, 0);
    chunk(&pdta, "phdr", &c);
    c.n = 0; put16le(&c, 0); put16le(&c, 0); put16le(&c, 1); put16le(&c, (unsigned)np);
    chunk(&pdta, "pbag", &c);
    c.n = 0; mods(&c, pmod, np);
    chunk(&pdta, "pmod", &c);
    c.n = 0; gen(&c, 41, 0); gen(&c, 0, 0);
    chunk(&pdta, "pgen", &c);
    c.n = 0; putstr(&c, "SineInst", 20); put16le(&c, 0); putstr(&c, "EOI", 20); put16le(&c, 1);
    chunk(&pdta, "inst", &c);
    c.n = 0; put16le(&c, 0); put16le(&c, 0); put16le(&c, 3); put16le(&c, (unsigned)ni);
    chunk(&pdta, "ibag", &c);
    c.n = 0; mods(&c, imod, ni);
    chunk(&pdta, "imod", &c);
    c.n = 0; gen(&c, 54, 1); gen(&c, 38, -3600); gen(&c, 53, 0); gen(&c, 0, 0);
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

/* A format 0 file at 960 ticks per second (480 per beat, 120 bpm). Events are added
 * at absolute times in milliseconds, in order. */
typedef struct song { buf t; uint32_t last; } song;

static void at(song* s, uint32_t ms)
{
    uint32_t tick = ms * 960 / 1000;
    if (tick < s->last) { fprintf(stderr, "events out of order at %u ms\n", ms); abort(); }
    varlen(&s->t, tick - s->last);
    s->last = tick;
}
static void ev3(song* s, uint32_t ms, unsigned status, unsigned a, unsigned b) { at(s, ms); put8(&s->t, status); put8(&s->t, a); put8(&s->t, b); }
static void ev2(song* s, uint32_t ms, unsigned status, unsigned a) { at(s, ms); put8(&s->t, status); put8(&s->t, a); }
static void cc(song* s, uint32_t ms, unsigned controller, unsigned value) { ev3(s, ms, 0xB0, controller, value); }
static void sysex(song* s, uint32_t ms, const uint8_t* d, size_t n) { at(s, ms); put8(&s->t, 0xF0); varlen(&s->t, (uint32_t)n); put(&s->t, d, n); }
static void note(song* s, uint32_t on_ms, uint32_t off_ms, unsigned key)
{
    ev3(s, on_ms, 0x90, key, 127);
    ev3(s, off_ms, 0x80, key, 0);
}

static buf finish(song* s)
{
    buf f = { 0 };
    put8(&s->t, 0); put8(&s->t, 0xFF); put8(&s->t, 0x2F); put8(&s->t, 0);
    put(&f, "MThd", 4); put32be(&f, 6); put8(&f, 0); put8(&f, 0); put8(&f, 0); put8(&f, 1); put8(&f, 480 >> 8); put8(&f, 480 & 255);
    put(&f, "MTrk", 4); put32be(&f, (uint32_t)s->t.n); put(&f, s->t.d, s->t.n);
    free(s->t.d);
    memset(s, 0, sizeof(*s));
    return f;
}

/* ---- rendering and measuring ------------------------------------------------------ */

/* Renders SECONDS of `f` dry (effects off) with `font`, from `seek` (frames) on. */
static float* render_from(const buf* f, fa_soundfont* font, fa_frames seek)
{
    fa_open_options o;
    fa_stream* s = NULL;
    float* x = (float*)calloc((size_t)SECONDS * RATE * 2, sizeof(float));
    fa_frames done = 0, got;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    o.soundfont = font;
    if (fa_stream_open_memory(g_engine, f->d, f->n, &o, &s) != FA_OK) { free(x); return NULL; }
    fa_stream_set_midi_effects(s, 0);
    if (seek && fa_stream_seek(s, seek) != FA_OK) { fa_stream_close(s); free(x); return NULL; }
    while (done < (fa_frames)SECONDS * RATE) {
        fa_frames want = (fa_frames)SECONDS * RATE - done;
        if (want > 1024) want = 1024;
        if (fa_stream_read(s, x + done * 2, want, &got) != FA_OK || !got) break;
        done += got;
    }
    fa_stream_close(s);
    return x;
}

static float* render(const buf* f) { return render_from(f, g_font, 0); }

static double rms(const float* x, double from_s, double to_s)
{
    fa_frames i, a = (fa_frames)(from_s * RATE), b = (fa_frames)(to_s * RATE);
    double acc = 0;
    for (i = a; i < b; i++) acc += (double)x[i * 2] * x[i * 2];
    return b > a ? sqrt(acc / (double)(b - a)) : 0.0;
}

static double db(double a, double b) { return 20.0 * log10(a / b); }

/* The pitch of each cycle between from_s and to_s, in cents against `ref_hz`: lowest,
 * highest, and how often it crosses its middle going up (the vibrato's cycles). */
typedef struct track { double lo, hi; int cycles; } track;

static track pitch_track(const float* x, double from_s, double to_s, double ref_hz)
{
    track t = { 1e9, -1e9, 0 };
    fa_frames i, a = (fa_frames)(from_s * RATE), b = (fa_frames)(to_s * RATE);
    double prev = -1, mid, *cents = (double*)malloc(sizeof(double) * (size_t)(b - a));
    int n = 0, k, above = -1;
    for (i = a + 1; i < b; i++) {
        if (x[(i - 1) * 2] < 0 && x[i * 2] >= 0) {
            double when = (double)(i - 1) + x[(i - 1) * 2] / (x[(i - 1) * 2] - x[i * 2]);
            if (prev >= 0) cents[n++] = 1200.0 * log2(RATE / (when - prev) / ref_hz);
            prev = when;
        }
    }
    for (k = 0; k < n; k++) { if (cents[k] < t.lo) t.lo = cents[k]; if (cents[k] > t.hi) t.hi = cents[k]; }
    mid = (t.lo + t.hi) / 2;
    for (k = 0; k < n; k++) {
        int now = cents[k] > mid;
        if (above == 0 && now) t.cycles++;
        above = now;
    }
    free(cents);
    return t;
}

/* The gain of TinySoundFont's low-pass biquad (cutoff in cents, resonance in
 * centibels) at `hz`, in dB. */
static double lowpass_db(double cutoff_cents, double q_cb, double hz)
{
    double fc = 8.176 * pow(2.0, cutoff_cents / 1200.0) / RATE, qinv = 1.0 / pow(10.0, q_cb / 10.0 / 20.0);
    double k = tan(PI * fc), kk = k * k, norm = 1 / (1 + k * qinv + kk);
    double a0 = kk * norm, a1 = 2 * a0, b1 = 2 * (kk - 1) * norm, b2 = (1 - k * qinv + kk) * norm, w = 2 * PI * hz / RATE;
    double nr = a0 + a1 * cos(w) + a0 * cos(2 * w), ni = -a1 * sin(w) - a0 * sin(2 * w);
    double dr = 1 + b1 * cos(w) + b2 * cos(2 * w), di = -b1 * sin(w) - b2 * sin(2 * w);
    return 10.0 * log10((nr * nr + ni * ni) / (dr * dr + di * di));
}

/* One note (`key`, 0 to 1.5 s) after `controller` = `value` (none if controller < 0). */
static buf note_after(int controller, int value, unsigned key)
{
    song s = { 0 };
    if (controller >= 0) cc(&s, 0, (unsigned)controller, (unsigned)value);
    note(&s, 0, 1500, key);
    return finish(&s);
}

/* ---- tests ------------------------------------------------------------------------ */

/* CC1 adds the SF2 default vibrato: 50 cents at full, scaled by RPN 5. */
static void test_mod_wheel(void)
{
    buf off = note_after(1, 0, 69), full = note_after(1, 127, 69), range;
    song s = { 0 };
    float *a, *b, *c;
    track t;
    cc(&s, 0, 101, 0); cc(&s, 0, 100, 5); cc(&s, 0, 6, 1); cc(&s, 0, 38, 0);   /* RPN 5: one semitone */
    cc(&s, 0, 1, 127);
    note(&s, 0, 1500, 69);
    range = finish(&s);
    a = render(&off); b = render(&full); c = render(&range);
    CHECK(a && b && c);
    t = pitch_track(a, 0.1, 1.4, 440.0);
    CHECK(t.hi - t.lo < 1.0);
    t = pitch_track(b, 0.1, 1.4, 440.0);
    CHECK_NEAR(t.hi, 50.0 * 127 / 128, 3.0);
    CHECK_NEAR(t.lo, -50.0 * 127 / 128, 3.0);
    CHECK_NEAR(t.cycles / 1.3, 8.176, 1.0);   /* the SF2 default vibrato LFO, 8.176 Hz */
    t = pitch_track(c, 0.1, 1.4, 440.0);
    CHECK_NEAR(t.hi, 100.0 * 127 / 128, 4.0);
    CHECK_NEAR(t.lo, -100.0 * 127 / 128, 4.0);
    free(a); free(b); free(c); free(off.d); free(full.d); free(range.d);
}

/* Channel pressure: the same vibrato, but none after a GS reset (as on GS devices).
 * Poly pressure has no default effect. */
static void test_pressure(void)
{
    static const uint8_t gs_reset[] = { 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7 };
    song s = { 0 };
    buf gm, gs, poly;
    float *a, *b, *c;
    track t;
    ev2(&s, 0, 0xD0, 127);
    note(&s, 0, 1500, 69);
    gm = finish(&s);
    sysex(&s, 0, gs_reset, sizeof(gs_reset));
    ev2(&s, 0, 0xD0, 127);
    note(&s, 0, 1500, 69);
    gs = finish(&s);
    ev3(&s, 0, 0x90, 69, 127);
    ev3(&s, 0, 0xA0, 69, 127);
    ev3(&s, 1500, 0x80, 69, 0);
    poly = finish(&s);
    a = render(&gm); b = render(&gs); c = render(&poly);
    CHECK(a && b && c);
    t = pitch_track(a, 0.1, 1.4, 440.0);
    CHECK_NEAR(t.hi, 50.0 * 127 / 128, 3.0);
    CHECK_NEAR(t.lo, -50.0 * 127 / 128, 3.0);
    t = pitch_track(b, 0.1, 1.4, 440.0);
    CHECK(t.hi - t.lo < 1.0);
    t = pitch_track(c, 0.1, 1.4, 440.0);
    CHECK(t.hi - t.lo < 1.0);
    free(a); free(b); free(c); free(gm.d); free(gs.d); free(poly.d);
}

/* CC74 closes the filter by 100 cents a step, CC71 adds 3 cB of resonance a step; the
 * GS/XG NRPNs and part SysEx set the same values. */
static void test_filter(void)
{
    static const uint8_t gs_cutoff[] = { 0x41, 0x10, 0x42, 0x12, 0x40, 0x11, 0x32, 0x00, 0x00, 0xF7 };   /* part 1 */
    static const uint8_t xg_cutoff[] = { 0x43, 0x10, 0x4C, 0x08, 0x00, 0x18, 0x00, 0xF7 };
    buf ref = note_after(-1, 0, 81), dark = note_after(74, 0, 81), bright = note_after(74, 127, 81), nrpn, gs, xg, res, res_ref;
    song s = { 0 };
    float *a, *b, *c, *d, *e, *g, *h, *k;
    double cutoff = 13500 - 64 * 100, base;
    fa_frames i;
    cc(&s, 0, 99, 1); cc(&s, 0, 98, 0x20); cc(&s, 0, 6, 0);
    note(&s, 0, 1500, 81);
    nrpn = finish(&s);
    sysex(&s, 0, gs_cutoff, sizeof(gs_cutoff));
    note(&s, 0, 1500, 81);
    gs = finish(&s);
    sysex(&s, 0, xg_cutoff, sizeof(xg_cutoff));
    note(&s, 0, 1500, 81);
    xg = finish(&s);
    cc(&s, 0, 74, 0); cc(&s, 0, 71, 127);
    note(&s, 0, 1500, 71);
    res = finish(&s);
    cc(&s, 0, 74, 0);
    note(&s, 0, 1500, 71);
    res_ref = finish(&s);
    a = render(&ref); b = render(&dark); c = render(&bright); d = render(&nrpn); e = render(&gs); g = render(&xg);
    h = render(&res); k = render(&res_ref);
    CHECK(a && b && c && d && e && g && h && k);
    base = rms(a, 0.2, 1.4);
    /* The filter at its open default (13500 cents) is already in the path: compare against it. */
    CHECK_NEAR(db(rms(b, 0.2, 1.4), base), lowpass_db(cutoff, 0, 880.0) - lowpass_db(13500, 0, 880.0), 0.3);
    CHECK_NEAR(db(rms(c, 0.2, 1.4), base), 0.0, 0.05);
    for (i = 0; i < (fa_frames)SECONDS * RATE * 2; i++) {
        CHECK(d[i] == b[i]);
        CHECK(e[i] == b[i]);
        CHECK(g[i] == b[i]);
    }
    CHECK_NEAR(db(rms(h, 0.2, 1.4), rms(k, 0.2, 1.4)),
               lowpass_db(cutoff, 63 * 3, 493.883) - lowpass_db(cutoff, 0, 493.883), 0.5);
    free(a); free(b); free(c); free(d); free(e); free(g); free(h); free(k);
    free(ref.d); free(dark.d); free(bright.d); free(nrpn.d); free(gs.d); free(xg.d); free(res.d); free(res_ref.d);
}

/* CC73 lengthens an instant attack to about 0.6 s (linear rise), CC72 the release
 * (exponential, -80 dB at its time) to 4.8 s or shortens it to 3 ms, CC78 delays the
 * vibrato. */
static void test_envelope(void)
{
    buf ref = note_after(-1, 0, 69), slow = note_after(73, 127, 69), long_rel, short_rel, delayed;
    song s = { 0 };
    float *a, *b, *c, *d, *e;
    double steady, half = 0;
    int w;
    track t;
    cc(&s, 0, 72, 127); note(&s, 0, 500, 69);
    long_rel = finish(&s);
    cc(&s, 0, 72, 0); note(&s, 0, 500, 69);
    short_rel = finish(&s);
    cc(&s, 0, 78, 127); cc(&s, 0, 1, 127); note(&s, 0, 1500, 69);
    delayed = finish(&s);
    a = render(&ref); b = render(&slow); c = render(&long_rel); d = render(&short_rel); e = render(&delayed);
    CHECK(a && b && c && d && e);
    steady = rms(a, 0.2, 0.5);
    for (w = 0; w < 100; w++)
        if (rms(b, w * 0.01, w * 0.01 + 0.01) >= 0.5 * steady) { half = w * 0.01 + 0.005; break; }
    CHECK_NEAR(half, 0.5 * 0.0156 * pow(2.0, 63 * 100 / 1200.0), 0.03);
    CHECK_NEAR(db(rms(c, 1.0, 1.05), steady), -80.1 * 0.525 / (0.125 * pow(2.0, 63 * 100 / 1200.0)), 1.0);
    CHECK(rms(d, 0.52, 0.55) < steady * 1e-4);
    CHECK(rms(a, 0.52, 0.55) > steady * 1e-3);   /* the default release still rings there */
    t = pitch_track(e, 0.05, 0.5, 440.0);
    CHECK(t.hi - t.lo < 1.0);
    t = pitch_track(e, 0.8, 1.4, 440.0);
    CHECK(t.hi > 40.0);
    free(a); free(b); free(c); free(d); free(e);
    free(ref.d); free(slow.d); free(long_rel.d); free(short_rel.d); free(delayed.d);
}

/* Portamento (CC65 on, CC5 = 64: 0.18 s) glides from the last key; CC84 names the key
 * the next note glides from, also with CC65 off. */
static void test_portamento(void)
{
    buf on, off, from;
    song s = { 0 };
    float *a, *b, *c;
    double glide = 0.005 * pow(2.0, 64 * 10.2 / 127);
    track t;
    cc(&s, 0, 65, 127); cc(&s, 0, 5, 64);
    note(&s, 0, 300, 57); note(&s, 600, 1800, 69);
    on = finish(&s);
    note(&s, 0, 300, 57); note(&s, 600, 1800, 69);
    off = finish(&s);
    cc(&s, 0, 5, 64); cc(&s, 500, 84, 57); note(&s, 600, 1800, 69);
    from = finish(&s);
    a = render(&on); b = render(&off); c = render(&from);
    CHECK(a && b && c);
    t = pitch_track(a, 0.6 + glide / 2 - 0.004, 0.6 + glide / 2 + 0.004, 440.0);
    CHECK_NEAR((t.lo + t.hi) / 2, -600.0, 60.0);
    t = pitch_track(a, 0.6 + glide + 0.02, 1.5, 440.0);
    CHECK(t.lo > -2.0 && t.hi < 2.0);
    t = pitch_track(a, 0.605, 0.62, 440.0);
    CHECK(t.lo < -1000.0);
    t = pitch_track(b, 0.605, 1.5, 440.0);
    CHECK(t.lo > -2.0 && t.hi < 2.0);
    t = pitch_track(c, 0.6 + glide / 2 - 0.004, 0.6 + glide / 2 + 0.004, 440.0);
    CHECK_NEAR((t.lo + t.hi) / 2, -600.0, 60.0);
    free(a); free(b); free(c); free(on.d); free(off.d); free(from.d);
}

/* Sostenuto holds the note whose key was down when it was pressed, not a later one;
 * a seek into the hold restarts that note, and the release still ends it. */
static void test_sostenuto(void)
{
    buf f;
    song s = { 0 };
    float *a, *b;
    double steady;
    ev3(&s, 0, 0x90, 69, 127);
    cc(&s, 100, 66, 127);
    ev3(&s, 200, 0x90, 81, 127);
    ev3(&s, 300, 0x80, 69, 0);
    ev3(&s, 400, 0x80, 81, 0);
    cc(&s, 1000, 66, 0);
    f = finish(&s);
    a = render(&f);
    b = render_from(&f, g_font, RATE * 7 / 10);
    CHECK(a && b);
    steady = rms(a, 0.05, 0.1);
    CHECK_NEAR(db(rms(a, 0.6, 0.95), steady), 0.0, 0.2);   /* 440 Hz alone: 81 has ended */
    CHECK_NEAR(pitch_track(a, 0.6, 0.95, 440.0).hi, 0.0, 1.0);
    CHECK(rms(a, 1.3, 1.5) < steady * 1e-4);
    CHECK_NEAR(db(rms(b, 0.05, 0.25), steady), 0.0, 0.2);  /* from 0.7 s */
    CHECK(rms(b, 0.6, 0.8) < steady * 1e-4);
    free(a); free(b); free(f.d);
}

/* Notes struck with the soft pedal down are 3 dB softer; one struck before is not. */
static void test_soft_pedal(void)
{
    buf ref = note_after(-1, 0, 69), soft = note_after(67, 127, 69), before;
    song s = { 0 };
    float *a, *b, *c;
    ev3(&s, 0, 0x90, 69, 127); cc(&s, 100, 67, 127); ev3(&s, 1500, 0x80, 69, 0);
    before = finish(&s);
    a = render(&ref); b = render(&soft); c = render(&before);
    CHECK(a && b && c);
    CHECK_NEAR(db(rms(b, 0.2, 1.4), rms(a, 0.2, 1.4)), -3.0 + lowpass_db(12300, 0, 440) - lowpass_db(13500, 0, 440), 0.05);
    CHECK_NEAR(db(rms(c, 0.2, 1.4), rms(a, 0.2, 1.4)), 0.0, 0.01);
    free(a); free(b); free(c); free(ref.d); free(soft.d); free(before.d);
}

/* A soundfont's own modulators: CC2 to attenuation (instrument, 20 dB, plus 10 dB
 * from the preset, which adds), CC4 to fine tune, and one that replaces the default
 * mod wheel vibrato with nothing. */
static void test_soundfont_modulators(void)
{
    static const sfmod imod[] = {
        { 0x0082, 48, 200, 0, 0 },   /* CC2, linear, to initialAttenuation */
        { 0x0084, 52, 100, 0, 0 },   /* CC4 to fineTune */
        { 0x0081, 6, 0, 0, 0 },      /* CC1 to vibLfoToPitch: none */
        { 0x0502, 48, 960, 0, 0 },   /* the default velocity curve: built in, so not counted twice */
    };
    static const sfmod pmod[] = { { 0x0082, 48, 100, 0, 0 } };
    buf sf = make_sf2(imod, 4, pmod, 1), plain = make_sf2(NULL, 0, NULL, 0), quiet = note_after(2, 127, 69),
        sharp = note_after(4, 127, 69), wheel = note_after(1, 127, 69), ref = note_after(-1, 0, 69);
    fa_soundfont *f = NULL, *p = NULL;
    float *a, *b, *c, *d, *e;
    track t;
    CHECK(fa_soundfont_open_memory(sf.d, sf.n, &f) == FA_OK);
    CHECK(fa_soundfont_open_memory(plain.d, plain.n, &p) == FA_OK);
    a = render_from(&ref, f, 0); b = render_from(&quiet, f, 0); c = render_from(&sharp, f, 0);
    d = render_from(&wheel, f, 0); e = render_from(&ref, p, 0);
    CHECK(a && b && c && d && e);
    CHECK_NEAR(db(rms(a, 0.2, 1.4), rms(e, 0.2, 1.4)), 0.0, 0.01);
    CHECK_NEAR(db(rms(b, 0.2, 1.4), rms(a, 0.2, 1.4)), -30.0 * 127 / 128, 0.1);
    t = pitch_track(c, 0.2, 1.4, 440.0);
    CHECK_NEAR((t.lo + t.hi) / 2, 100.0 * 127 / 128, 1.0);
    t = pitch_track(d, 0.2, 1.4, 440.0);
    CHECK(t.hi - t.lo < 1.0);
    free(a); free(b); free(c); free(d); free(e);
    fa_soundfont_close(f);
    fa_soundfont_close(p);
    free(sf.d); free(plain.d); free(quiet.d); free(sharp.d); free(wheel.d); free(ref.d);
}

/* CC121 ends the mod wheel vibrato; a GS reset puts the sound controllers back. */
static void test_resets(void)
{
    static const uint8_t gs_reset[] = { 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7 };
    buf ref = note_after(-1, 0, 81), wheel, dark;
    song s = { 0 };
    float *a, *b, *c;
    track t;
    cc(&s, 0, 1, 127); ev3(&s, 0, 0x90, 69, 127); cc(&s, 500, 121, 0); ev3(&s, 1500, 0x80, 69, 0);
    wheel = finish(&s);
    cc(&s, 0, 74, 0); sysex(&s, 0, gs_reset, sizeof(gs_reset)); note(&s, 0, 1500, 81);
    dark = finish(&s);
    a = render(&wheel); b = render(&dark); c = render(&ref);
    CHECK(a && b && c);
    t = pitch_track(a, 0.1, 0.45, 440.0);
    CHECK(t.hi > 40.0);
    t = pitch_track(a, 0.55, 1.4, 440.0);
    CHECK(t.hi - t.lo < 1.0);
    CHECK_NEAR(db(rms(b, 0.2, 1.4), rms(c, 0.2, 1.4)), 0.0, 0.01);
    free(a); free(b); free(c); free(ref.d); free(wheel.d); free(dark.d);
}

/* A seek replays the controllers: the vibrato and the darker filter are there after it,
 * and a note starting after the target sounds as when playing through. */
static void test_seek(void)
{
    buf f;
    song s = { 0 };
    float *a, *b;
    fa_frames i, from = RATE / 2;
    cc(&s, 0, 1, 127); cc(&s, 0, 74, 20); ev2(&s, 0, 0xD0, 90);
    note(&s, 1000, 1800, 69);
    f = finish(&s);
    a = render(&f);
    b = render_from(&f, g_font, from);
    CHECK(a && b);
    CHECK(rms(a, 1.2, 1.7) > 0.01);
    for (i = 0; i < (fa_frames)(SECONDS * RATE - from) * 2; i++) CHECK(b[i] == a[from * 2 + i]);
    free(a); free(b); free(f.d);
}

int main(void)
{
    fa_engine_config cfg;
    buf sf = make_sf2(NULL, 0, NULL, 0);
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    cfg.worker_threads = 1;
    if (fa_engine_create(&cfg, &g_engine) != FA_OK) { fprintf(stderr, "engine create failed\n"); return 1; }
    if (fa_soundfont_open_memory(sf.d, sf.n, &g_font) != FA_OK) { fprintf(stderr, "test soundfont rejected\n"); return 1; }
    free(sf.d);
    RUN(test_mod_wheel);
    RUN(test_pressure);
    RUN(test_filter);
    RUN(test_envelope);
    RUN(test_portamento);
    RUN(test_sostenuto);
    RUN(test_soft_pedal);
    RUN(test_soundfont_modulators);
    RUN(test_resets);
    RUN(test_seek);
    fa_soundfont_close(g_font);
    fa_engine_destroy(g_engine);
    if (g_failures) { fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    printf("all passed\n");
    return 0;
}
