/* Shared by the MIDI synthesizer tests (test_midi_controls.c, test_midi_drums.c): a sine
 * soundfont and MIDI files built in memory, rendering, and pitch, level and filter
 * measurements. Each test program includes it once. */
#ifndef MIDI_SYNTH_TEST_H
#define MIDI_SYNTH_TEST_H
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"   /* not every program uses every helper */
#endif
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

static void mod_records(buf* b, const sfmod* m, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        put16le(b, m[i].src); put16le(b, m[i].dest); put16le(b, (unsigned)(uint16_t)(int16_t)m[i].amount);
        put16le(b, m[i].amt_src); put16le(b, m[i].trans);
    }
}

/* The records and the terminal one. */
static void mods(buf* b, const sfmod* m, int n)
{
    int i;
    mod_records(b, m, n);
    for (i = 0; i < 10; i++) put8(b, 0);
}

/* A preset of the sine instrument: bank, program, and a coarse tune (semitones) of its own. */
typedef struct sfpreset { unsigned bank, program; int coarse; } sfpreset;

/* `presets` of a looped 440 Hz sine, root key 69, instant attack, full sustain, 125 ms
 * release, with `imod` on its instrument zone and `pmod` on each preset zone. */
static buf make_sf2_presets(const sfpreset* presets, int npresets, const sfmod* imod, int ni, const sfmod* pmod, int np)
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
    for (i = 0; i < npresets; i++) {
        putstr(&c, "Sine", 20); put16le(&c, presets[i].program); put16le(&c, presets[i].bank); put16le(&c, (unsigned)i);
        put32le(&c, 0); put32le(&c, 0); put32le(&c, 0);
    }
    putstr(&c, "EOP", 20); put16le(&c, 0); put16le(&c, 0); put16le(&c, (unsigned)npresets); put32le(&c, 0); put32le(&c, 0); put32le(&c, 0);
    chunk(&pdta, "phdr", &c);
    c.n = 0;
    for (i = 0; i <= npresets; i++) { put16le(&c, (unsigned)(i * 2)); put16le(&c, (unsigned)(i * np)); }
    chunk(&pdta, "pbag", &c);
    c.n = 0;
    for (i = 0; i < npresets; i++) mod_records(&c, pmod, np);
    mods(&c, NULL, 0);
    chunk(&pdta, "pmod", &c);
    c.n = 0;
    for (i = 0; i < npresets; i++) { gen(&c, 51, presets[i].coarse); gen(&c, 41, 0); }
    gen(&c, 0, 0);
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

/* Preset 0 bank 0 of the sine. */
static buf make_sf2(const sfmod* imod, int ni, const sfmod* pmod, int np)
{
    static const sfpreset one = { 0, 0, 0 };
    return make_sf2_presets(&one, 1, imod, ni, pmod, np);
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

#endif
