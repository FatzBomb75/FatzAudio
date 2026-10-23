/* The MIDI lyric surface: the timeline's rules (src/lyric_time.c) against the design
 * reference's numbers, and the surface (src/lyric.c) on a song built here, drawn
 * with tests/data/fonts/tt.ttf: layout, colours, changed areas, revisions, frames as
 * a function of the position, styles and errors. */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "fa_internal.h"
#include "lyric_time.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; return; } } while (0)
#define CHECK_EQ(a, b) do { long long va_ = (long long)(a), vb_ = (long long)(b); if (va_ != vb_) { \
    fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_failures++; return; } } while (0)
#define CHECK_NEAR(a, b, tol) do { double va_ = (a), vb_ = (b); if (fabs(va_ - vb_) > (tol)) { \
    fprintf(stderr, "  FAIL %s:%d: %s ~ %s (%.6f vs %.6f)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_failures++; return; } } while (0)
#define RUN(t) do { const char* only_ = getenv("FA_TEST"); int before_ = g_failures; \
    if (only_ && strcmp(only_, #t) != 0) break; \
    printf("%s\n", #t); t(); if (g_failures == before_) printf("  ok\n"); } while (0)

#define MS 1000000LL
#define SEC 1000000000LL
#define RATE 44100
#define PI 3.14159265358979323846

static fa_engine* g_engine;
static fa_soundfont* g_font;

/* ---- the timeline ----------------------------------------------------------------- */

/* The design reference's numbers (fa_lyric_style_init's Classic), lead 0. */
static void params(ly_params* p)
{
    memset(p, 0, sizeof(*p));
    p->paragraph_gap = 2000 * MS;
    p->sentence_gap = 900 * MS;
    p->break_length = 42;
    p->scroll = 1500 * MS;
    p->page = 1000 * MS;
    p->page_hold = 2000 * MS;
    p->page_preview = 1000 * MS;
    p->page_overlap = 1;
    p->end_hold = 5000 * MS;
    p->end_fade = 1000 * MS;
    p->countdown = FA_COUNTDOWN_OFF;
    p->countdown_count = 4;
    p->countdown_time = 5000 * MS;
    p->countdown_gap = 6000 * MS;
    p->wipe_cap = 600 * MS;
    p->instant_cut = 150 * MS;
    p->wipe_share = 0.8f;
    p->lines_above = p->lines_below = 2;
    p->above_opacity[0] = 0.5f;
    p->above_opacity[1] = 0.25f;
    p->next_opacity = 0.65f;
    p->after_next_factor = 0.55f;
    p->near_scale = 0.8f;
    p->far_scale = 0.6f;
    p->page_in_scale = 1.5f;
    p->page_out_scale = 0.75f;
    p->duration = 100 * SEC;
}

static const ly_show* shown(const ly_show* s, uint32_t n, uint32_t line, int outgoing)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        if (s[i].line == line && s[i].outgoing == outgoing) return &s[i];
    return NULL;
}

static void test_lines(void)
{
    ly_params p;
    ly_timeline tl;
    ly_frag f[] = { { "Hel", 1000 * MS, 0 }, { "lo ", 1250 * MS, 0 }, { "world", 1500 * MS, 0 },
                    { "Next", 2000 * MS, FA_MIDI_NEW_LINE } };
    params(&p);
    p.lead = 400 * MS;
    CHECK_EQ(ly_build(&tl, f, 4, &p, NULL, NULL, 0.0f, NULL, NULL), FA_OK);
    CHECK_EQ(tl.n, 2);
    CHECK(strcmp(tl.line[0].text, "Hello world") == 0);
    CHECK_EQ(tl.line[0].nseg, 3);
    CHECK_EQ(tl.line[0].seg[1].start, 3);
    CHECK_EQ(tl.line[0].seg[1].len, 2);
    CHECK_EQ(tl.line[0].seg[2].start, 6);
    CHECK_EQ(tl.line[0].seg[2].len, 5);
    CHECK_EQ(tl.line[0].seg[0].time, 600 * MS);      /* the lead */
    CHECK_EQ(tl.line[0].last, 1100 * MS);
    CHECK(strcmp(tl.line[1].text, "Next") == 0);
    CHECK_EQ(tl.line[1].page, 0);
    ly_free(&tl);
}

static void test_gaps(void)
{
    ly_params p;
    ly_timeline tl;
    ly_frag sentence[] = { { "Hi.", 0, 0 }, { "Yo", 950 * MS, 0 }, { "Ok", 1100 * MS, 0 }, { "So", 3200 * MS, 0 } };
    ly_frag plain[] = { { "Hi", 0, 0 }, { "Yo", 950 * MS, 0 } };
    params(&p);
    /* 0.9 s after a full stop: a new line; 2 s: a new page. */
    CHECK_EQ(ly_build(&tl, sentence, 4, &p, NULL, NULL, 0.0f, NULL, NULL), FA_OK);
    CHECK_EQ(tl.n, 3);
    CHECK(strcmp(tl.line[1].text, "YoOk") == 0);
    CHECK_EQ(tl.line[1].page, 0);
    CHECK_EQ(tl.line[2].page, 1);
    ly_free(&tl);
    /* A long pause after the song's own line mark stays on the page. */
    sentence[3].flags = FA_MIDI_NEW_LINE;
    CHECK_EQ(ly_build(&tl, sentence, 4, &p, NULL, NULL, 0.0f, NULL, NULL), FA_OK);
    CHECK_EQ(tl.n, 3);
    CHECK_EQ(tl.line[2].page, 0);
    ly_free(&tl);
    /* Without the full stop the same gap joins. */
    CHECK_EQ(ly_build(&tl, plain, 2, &p, NULL, NULL, 0.0f, NULL, NULL), FA_OK);
    CHECK_EQ(tl.n, 1);
    CHECK(strcmp(tl.line[0].text, "HiYo") == 0);
    ly_free(&tl);
}

static void test_break_length(void)
{
    ly_params p;
    ly_timeline tl;
    ly_frag f[10];
    int i;
    params(&p);
    for (i = 0; i < 10; i++) {
        f[i].text = "abcde ";
        f[i].time = (fa_time)i * 100 * MS;
        f[i].flags = 0;
    }
    /* 7 syllables are 42 characters: the line breaks before the eighth. */
    CHECK_EQ(ly_build(&tl, f, 10, &p, NULL, NULL, 0.0f, NULL, NULL), FA_OK);
    CHECK_EQ(tl.n, 2);
    CHECK_EQ(tl.line[0].nseg, 7);
    CHECK_EQ(tl.line[1].nseg, 3);
    ly_free(&tl);
}

static float ten_per_byte(void* ctx, const char* text, uint32_t len)
{
    (void)ctx;
    (void)text;
    return 10.0f * (float)len;
}

static void test_split_wide(void)
{
    ly_params p;
    ly_timeline tl;
    ly_frag f[] = { { "aaa ", 0, 0 }, { "bb", 100 * MS, 0 }, { "b ", 200 * MS, 0 }, { "ccc", 300 * MS, 0 },
                    { "aaaaaaaaaa", 5000 * MS, FA_MIDI_NEW_PARAGRAPH } };
    params(&p);
    /* 50 wide: "aaa bbb" (70) does not fit, so each word is a line, "bbb" keeping
     * its two syllables; the single 100-wide word is drawn at half size. */
    CHECK_EQ(ly_build(&tl, f, 5, &p, ten_per_byte, NULL, 50.0f, NULL, NULL), FA_OK);
    CHECK_EQ(tl.n, 4);
    CHECK(strcmp(tl.line[0].text, "aaa") == 0);
    CHECK(strcmp(tl.line[1].text, "bbb") == 0);
    CHECK_EQ(tl.line[1].nseg, 2);
    CHECK_EQ(tl.line[1].seg[1].start, 2);
    CHECK(strcmp(tl.line[2].text, "ccc") == 0);
    CHECK_EQ(tl.line[2].page, 0);
    CHECK_NEAR(tl.line[0].fit, 1.0, 1e-6);
    CHECK_NEAR(tl.line[3].fit, 0.5, 1e-6);
    CHECK_EQ(tl.line[3].page, 1);
    ly_free(&tl);
}

static void test_scroll(void)
{
    ly_params p;
    ly_timeline tl;
    ly_show s[LY_MAX_SHOW];
    const ly_show* a;
    uint32_t n;
    ly_frag f[] = { { "A", 0, 0 }, { "B", 5 * SEC, FA_MIDI_NEW_LINE } };
    params(&p);
    CHECK_EQ(ly_build(&tl, f, 2, &p, NULL, NULL, 0.0f, NULL, NULL), FA_OK);
    /* The scroll ends as the line is due. */
    CHECK_EQ(tl.line[1].t0, 3500 * MS);
    CHECK_EQ(tl.line[1].t1, 5000 * MS);
    /* Halfway (smoothstep 0.5): both half a slot up. */
    n = ly_frame(&tl, &p, 4250 * MS, s);
    a = shown(s, n, 1, 0);
    CHECK(a != NULL);
    CHECK_NEAR(a->slot, 0.5, 1e-4);
    CHECK_NEAR(a->scale, 0.9, 1e-4);
    CHECK_NEAR(a->opacity, (1.0 + 0.65) / 2.0, 1e-4);
    a = shown(s, n, 0, 0);
    CHECK(a != NULL);
    CHECK_NEAR(a->slot, -0.5, 1e-4);
    CHECK_NEAR(a->opacity, 0.75, 1e-4);
    ly_free(&tl);
}

static void test_slots(void)
{
    ly_params p;
    ly_timeline tl;
    ly_show s[LY_MAX_SHOW];
    ly_frag f[6];
    uint32_t n;
    int i;
    static const float op[5] = { 0.25f, 0.5f, 1.0f, 0.65f, 0.65f * 0.55f };
    static const float sc[5] = { 0.6f, 0.8f, 1.0f, 0.8f, 0.6f };
    params(&p);
    for (i = 0; i < 6; i++) {
        f[i].text = "x";
        f[i].time = (fa_time)i * 3 * SEC;
        f[i].flags = i ? FA_MIDI_NEW_LINE : 0;
    }
    CHECK_EQ(ly_build(&tl, f, 6, &p, NULL, NULL, 0.0f, NULL, NULL), FA_OK);
    /* Line 2 active, settled: the design's slot table. */
    n = ly_frame(&tl, &p, 6500 * MS, s);
    for (i = 0; i < 5; i++) {
        const ly_show* a = shown(s, n, (uint32_t)i, 0);
        CHECK(a != NULL);
        CHECK_NEAR(a->slot, i - 2, 1e-4);
        CHECK_NEAR(a->opacity, op[i], 1e-4);
        CHECK_NEAR(a->scale, sc[i], 1e-4);
    }
    CHECK(shown(s, n, 5, 0) == NULL);
    ly_free(&tl);
}

static void test_wipe(void)
{
    ly_params p;
    ly_timeline tl;
    ly_wipe w;
    ly_frag f[] = { { "a", 0, 0 }, { "b", 500 * MS, 0 }, { "c", 600 * MS, 0 } };
    params(&p);
    CHECK_EQ(ly_build(&tl, f, 3, &p, NULL, NULL, 0.0f, NULL, NULL), FA_OK);
    ly_wipe_at(&tl.line[0], &p, -1, &w);
    CHECK_EQ(w.full, 0);
    CHECK_EQ(w.current, -1);
    /* 0.8 of a 500 ms gap = 400 ms; halfway the quintic is at 0.5. */
    ly_wipe_at(&tl.line[0], &p, 200 * MS, &w);
    CHECK_EQ(w.full, 0);
    CHECK_EQ(w.current, 0);
    CHECK_NEAR(w.mix, 0.5, 1e-5);
    ly_wipe_at(&tl.line[0], &p, 450 * MS, &w);
    CHECK_EQ(w.full, 1);
    CHECK_EQ(w.current, -1);
    /* 80 ms is under the 150 ms cut: instant. */
    ly_wipe_at(&tl.line[0], &p, 550 * MS, &w);
    CHECK_EQ(w.full, 2);
    /* The last syllable takes the 600 ms cap. */
    ly_wipe_at(&tl.line[0], &p, 900 * MS, &w);
    CHECK_EQ(w.current, 2);
    CHECK_NEAR(w.mix, ly_ease_in_out_quint(0.5f), 1e-5);
    ly_wipe_at(&tl.line[0], &p, 1200 * MS, &w);
    CHECK_EQ(w.full, 3);
    ly_free(&tl);
}

static void test_countdown(void)
{
    ly_params p;
    ly_timeline tl;
    ly_count c;
    ly_frag f[] = { { "a", 0, 0 }, { "b", 8 * SEC, FA_MIDI_NEW_LINE } };
    params(&p);
    p.countdown = FA_COUNTDOWN_CLOCK;
    CHECK_EQ(ly_build(&tl, f, 2, &p, NULL, NULL, 0.0f, NULL, NULL), FA_OK);
    CHECK(!tl.line[0].countdown);
    CHECK(tl.line[1].countdown);
    CHECK_EQ(tl.line[1].cd_period, SEC);
    /* In place when its countdown starts (5 s before). */
    CHECK_EQ(tl.line[1].t1, 3 * SEC);
    ly_countdown_at(&tl.line[1], &p, 2900 * MS, &c);
    CHECK(!c.shown);
    ly_countdown_at(&tl.line[1], &p, 3500 * MS, &c);
    CHECK(c.shown);
    CHECK_EQ(c.done, 0);
    CHECK_NEAR(c.progress, 0.5, 1e-5);
    CHECK_NEAR(c.opacity, 1.0, 1e-6);
    ly_countdown_at(&tl.line[1], &p, 6200 * MS, &c);
    CHECK_EQ(c.done, 3);
    ly_countdown_at(&tl.line[1], &p, 7500 * MS, &c);
    CHECK_EQ(c.done, 4);
    CHECK_NEAR(c.opacity, 0.5, 1e-5);
    ly_countdown_at(&tl.line[1], &p, 8 * SEC, &c);
    CHECK(!c.shown);
    ly_free(&tl);
}

static void test_pages(void)
{
    ly_params p;
    ly_timeline tl;
    ly_show s[LY_MAX_SHOW];
    const ly_show* a;
    uint32_t n;
    ly_frag f[] = { { "a", 0, 0 }, { "b", 10 * SEC, 0 } };
    params(&p);
    CHECK_EQ(ly_build(&tl, f, 2, &p, NULL, NULL, 0.0f, NULL, NULL), FA_OK);
    CHECK_EQ(tl.line[1].page, 1);
    CHECK(tl.line[1].clears);
    /* Held 2 s, out over 1 s; in place 1 s early after a 1 s fade-in. */
    CHECK_EQ(tl.line[1].out0, 2 * SEC);
    CHECK_EQ(tl.line[1].out1, 3 * SEC);
    CHECK_EQ(tl.line[1].in0, 8 * SEC);
    CHECK_EQ(tl.line[1].in1, 9 * SEC);
    n = ly_frame(&tl, &p, 2500 * MS, s);
    a = shown(s, n, 0, 1);
    CHECK(a != NULL);
    CHECK_NEAR(a->opacity, 0.875, 1e-5);
    CHECK_NEAR(a->scale, 1.0 - 0.25 * 0.125, 1e-5);
    CHECK(shown(s, n, 1, 0) == NULL);
    n = ly_frame(&tl, &p, 8500 * MS, s);
    CHECK(shown(s, n, 0, 1) == NULL);
    a = shown(s, n, 1, 0);
    CHECK(a != NULL);
    CHECK_NEAR(a->opacity, 0.5, 1e-5);
    CHECK_NEAR(a->scale, 1.25, 1e-5);
    /* No overlap: the new page waits for the old one. */
    p.page_overlap = 0;
    p.page_hold = 0;
    p.page_preview = 9 * SEC;
    ly_schedule(&tl, &p, NULL, NULL);
    CHECK(tl.line[1].in0 >= tl.line[1].out1);
    ly_free(&tl);
}

static void test_end(void)
{
    ly_params p;
    ly_timeline tl;
    ly_show s[LY_MAX_SHOW];
    ly_frag f[] = { { "a", 10 * SEC, 0 } };
    params(&p);
    CHECK_EQ(ly_build(&tl, f, 1, &p, NULL, NULL, 0.0f, NULL, NULL), FA_OK);
    CHECK_EQ(tl.end_fade0, 15 * SEC);
    CHECK_EQ(tl.end_fade1, 16 * SEC);
    CHECK_NEAR(ly_frame(&tl, &p, 15500 * MS, s) ? s[0].opacity : -1.0, 0.5, 1e-5);
    CHECK_EQ(ly_frame(&tl, &p, 16 * SEC, s), 0);
    /* A song that ends sooner fades by its end. */
    p.duration = 13 * SEC;
    ly_schedule(&tl, &p, NULL, NULL);
    CHECK_EQ(tl.end_fade1, 13 * SEC);
    CHECK_EQ(tl.end_fade0, 12 * SEC);
    ly_free(&tl);
}

/* ---- a song ------------------------------------------------------------------------ */

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
static void gen(buf* b, unsigned oper, unsigned amount) { put16le(b, oper); put16le(b, amount); }

#define SINE_LEN 4410

/* test_midi.c's soundfont, one preset: a looped 440 Hz sine. */
static buf make_sf2(void)
{
    buf sf = { 0 }, riff = { 0 }, info = { 0 }, sdta = { 0 }, pdta = { 0 }, c = { 0 };
    int i;
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
    putstr(&c, "EOP", 20); put16le(&c, 0); put16le(&c, 0); put16le(&c, 1); put32le(&c, 0); put32le(&c, 0); put32le(&c, 0);
    chunk(&pdta, "phdr", &c);
    c.n = 0; put16le(&c, 0); put16le(&c, 0); put16le(&c, 1); put16le(&c, 0);
    chunk(&pdta, "pbag", &c);
    c.n = 0; for (i = 0; i < 10; i++) put8(&c, 0);
    chunk(&pdta, "pmod", &c);
    c.n = 0; gen(&c, 41, 0); gen(&c, 0, 0);
    chunk(&pdta, "pgen", &c);
    c.n = 0;
    putstr(&c, "SineInst", 20); put16le(&c, 0);
    putstr(&c, "EOI", 20); put16le(&c, 1);
    chunk(&pdta, "inst", &c);
    c.n = 0; put16le(&c, 0); put16le(&c, 0); put16le(&c, 3); put16le(&c, 0);
    chunk(&pdta, "ibag", &c);
    c.n = 0; for (i = 0; i < 10; i++) put8(&c, 0);
    chunk(&pdta, "imod", &c);
    c.n = 0;
    gen(&c, 54, 1);
    gen(&c, 38, (unsigned)(uint16_t)-3600);
    gen(&c, 53, 0);
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
static void ev3(buf* t, uint32_t delta, unsigned status, unsigned a, unsigned b) { varlen(t, delta); put8(t, status); put8(t, a); put8(t, b); }
static void lyric(buf* t, uint32_t delta, const char* text)
{
    size_t n = strlen(text);
    varlen(t, delta); put8(t, 0xFF); put8(t, 0x05); varlen(t, (uint32_t)n); put(t, text, n);
}

/* 480 ticks a beat at 120 bpm: 960 ticks a second. The font has A B O V and space.
 * Page 0: "ABBA VO" (1, 1.5, 2 s) and "OBAV" (3, 3.5 s); page 1 after a 6.5 s gap
 * (a countdown): "BOA VA" (10, 10.5 s). A note holds to 12 s. */
static buf make_song(void)
{
    buf f = { 0 }, t = { 0 };
    put(&f, "MThd", 4); put32be(&f, 6); put8(&f, 0); put8(&f, 0); put8(&f, 0); put8(&f, 1); put8(&f, 480 >> 8); put8(&f, 480 & 255);
    ev3(&t, 0, 0x90, 69, 1);
    lyric(&t, 960, "AB");
    lyric(&t, 480, "BA ");
    lyric(&t, 480, "VO");
    lyric(&t, 960, "\rOB");
    lyric(&t, 480, "AV");
    lyric(&t, 6240, "BOA");
    lyric(&t, 480, " VA");
    ev3(&t, 1440, 0x80, 69, 0);
    varlen(&t, 0); put8(&t, 0xFF); put8(&t, 0x2F); put8(&t, 0);
    put(&f, "MTrk", 4); put32be(&f, (uint32_t)t.n); put(&f, t.d, t.n);
    free(t.d);
    return f;
}

static buf g_song;

static fa_stream* open_song(void)
{
    fa_open_options o;
    fa_stream* s = NULL;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    return fa_stream_open_memory(g_engine, g_song.d, g_song.n, &o, &s) == FA_OK ? s : NULL;
}

static void style(fa_lyric_style* st, fa_lyric_preset preset)
{
    fa_lyric_style_init(st, preset);
    st->font_path = FA_TEST_DATA_DIR "/fonts/tt.ttf";
}

typedef struct target {
    fa_surface_layout lay;
    uint8_t* px;
    int32_t stride;
    uint64_t rev;
} target;

static int target_make(fa_surface* f, target* t)
{
    memset(t, 0, sizeof(*t));
    t->lay.struct_size = (uint32_t)sizeof(t->lay);
    if (fa_surface_get_layout(f, &t->lay) != FA_OK) return 0;
    t->stride = (int32_t)t->lay.width * 4;
    t->px = (uint8_t*)calloc((size_t)t->stride * t->lay.height, 1);
    return t->px != NULL;
}

static fa_result draw_at(fa_surface* f, target* t, fa_time time, fa_surface_frame* fr)
{
    fa_result r;
    memset(fr, 0, sizeof(*fr));
    fr->struct_size = (uint32_t)sizeof(*fr);
    r = fa_surface_update_into(f, time, t->px, t->stride, t->lay.width, t->lay.height, t->rev, fr);
    if (r == FA_OK) t->rev = fr->revision;
    return r;
}

/* Pixels of a colour, premultiplied BGRA, opaque-ish. */
static int count_color(const target* t, int r, int g, int b)
{
    uint32_t x, y;
    int n = 0;
    for (y = 0; y < t->lay.height; y++)
        for (x = 0; x < t->lay.width; x++) {
            const uint8_t* p = t->px + (size_t)y * (size_t)t->stride + x * 4;
            if (p[3] > 240 && abs(p[2] - r) < 16 && abs(p[1] - g) < 16 && abs(p[0] - b) < 16) n++;
        }
    return n;
}

/* ---- the surface ------------------------------------------------------------------- */

static void test_surface(void)
{
    fa_stream* s = open_song();
    fa_lyric_style st;
    fa_surface* f = NULL;
    fa_surface_frame fr;
    target t;
    CHECK(s != NULL);
    style(&st, FA_LYRIC_CLASSIC);
    CHECK_EQ(fa_lyric_surface_create(s, &st, &f), FA_OK);
    CHECK(target_make(f, &t));
    /* 1920x1080: the block is the box's width plus the effects' reach, centred. */
    CHECK_NEAR(t.lay.scale, 1.0, 1e-6);
    CHECK(strcmp(t.lay.font_used, "FA Test") == 0);
    CHECK(t.lay.width > (uint32_t)(0.85 * 1920) && t.lay.width < 1920);
    CHECK(t.lay.height > 200 && t.lay.height < 1080);
    CHECK_NEAR(t.lay.bitmap_x + t.lay.width / 2.0, 960.0, 1.0);
    CHECK_EQ(t.lay.content_width, (uint32_t)ceil(0.85 * 1920));
    /* Line 0 sung to "AB", "BA" half way (the next line scrolls up from 1.35 s):
     * yellow and white text with black edges. */
    CHECK_EQ(draw_at(f, &t, 1300 * MS, &fr), FA_OK);
    CHECK_EQ(fr.base_revision, 0);
    CHECK_EQ(fr.dirty_count, 1);
    CHECK_EQ(fr.dirty_bounds.width, t.lay.width);
    CHECK(count_color(&t, 255, 255, 0) > 500);
    CHECK(count_color(&t, 255, 255, 255) > 500);
    CHECK(count_color(&t, 0, 0, 0) > 100);
    CHECK_EQ(fr.layout_revision, t.lay.layout_revision);
    CHECK_EQ(fr.background_index, -1);
    /* The same position again: nothing changed. */
    {
        uint64_t rev = fr.revision;
        CHECK_EQ(draw_at(f, &t, 1300 * MS, &fr), FA_OK);
        CHECK_EQ(fr.revision, rev);
        CHECK_EQ(fr.dirty_count, 0);
    }
    fa_surface_destroy(f);
    free(t.px);
    fa_stream_close(s);
}

/* While only a syllable is being wiped, only its columns change. */
static void test_wipe_area(void)
{
    fa_stream* s = open_song();
    fa_lyric_style st;
    fa_surface* f = NULL;
    fa_surface_frame fr;
    target t;
    int i;
    CHECK(s != NULL);
    style(&st, FA_LYRIC_MODERN);
    CHECK_EQ(fa_lyric_surface_create(s, &st, &f), FA_OK);
    CHECK(target_make(f, &t));
    CHECK_EQ(draw_at(f, &t, 1150 * MS, &fr), FA_OK);
    for (i = 1; i <= 4; i++) {
        CHECK_EQ(draw_at(f, &t, (1150 + 50 * i) * MS, &fr), FA_OK);
        CHECK_EQ(fr.base_revision, t.rev - 1);
        CHECK(fr.dirty_count >= 1);
        CHECK(fr.dirty_bounds.width < t.lay.width / 3);
        CHECK(fr.dirty_bounds.height < t.lay.height);
    }
    fa_surface_destroy(f);
    free(t.px);
    fa_stream_close(s);
}

/* A frame is a function of its position: one surface walked through the song and
 * back by changed areas equals a new one drawn there at once. */
static void test_stateless(void)
{
    static const int ms[] = { 0, 400, 1150, 1700, 2300, 2900, 3300, 3800, 4300, 6000, 7600, 9900, 10400, 11000,
                              16500, 2600, 9000, 1200, 3600 };
    fa_stream* s = open_song();
    fa_lyric_style st;
    fa_surface *a = NULL, *b = NULL;
    fa_surface_frame fr;
    target ta, tb;
    size_t i;
    CHECK(s != NULL);
    style(&st, FA_LYRIC_CLASSIC);
    st.effects.bevel.on = 1;
    CHECK_EQ(fa_lyric_surface_create(s, &st, &a), FA_OK);
    CHECK_EQ(fa_lyric_surface_create(s, &st, &b), FA_OK);
    CHECK(target_make(a, &ta));
    CHECK(target_make(b, &tb));
    for (i = 0; i < sizeof(ms) / sizeof(ms[0]); i++) {
        CHECK_EQ(draw_at(a, &ta, (fa_time)ms[i] * MS, &fr), FA_OK);
        tb.rev = 0;
        fa_surface_destroy(b);
        CHECK_EQ(fa_lyric_surface_create(s, &st, &b), FA_OK);
        CHECK_EQ(draw_at(b, &tb, (fa_time)ms[i] * MS, &fr), FA_OK);
        if (memcmp(ta.px, tb.px, (size_t)ta.stride * ta.lay.height) != 0) {
            fprintf(stderr, "  differs at %d ms\n", ms[i]);
            CHECK(0);
        }
    }
    /* After the end fade: nothing. */
    CHECK_EQ(count_color(&ta, 255, 255, 255), count_color(&tb, 255, 255, 255));
    fa_surface_destroy(a);
    fa_surface_destroy(b);
    free(ta.px);
    free(tb.px);
    fa_stream_close(s);
}

/* Three buffers in rotation: each catches up from the frame it holds. */
static void test_rotation(void)
{
    fa_stream* s = open_song();
    fa_lyric_style st;
    fa_surface *a = NULL, *b = NULL;
    fa_surface_frame fr;
    target t[3], ref;
    int i;
    CHECK(s != NULL);
    style(&st, FA_LYRIC_NEON);
    CHECK_EQ(fa_lyric_surface_create(s, &st, &a), FA_OK);
    CHECK_EQ(fa_lyric_surface_create(s, &st, &b), FA_OK);
    for (i = 0; i < 3; i++) CHECK(target_make(a, &t[i]));
    CHECK(target_make(b, &ref));
    for (i = 0; i < 300; i++) {
        fa_time at = (fa_time)(500 + 17 * i) * MS;
        target* x = &t[i % 3];
        CHECK_EQ(draw_at(a, x, at, &fr), FA_OK);
        ref.rev = 0;
        CHECK_EQ(draw_at(b, &ref, at, &fr), FA_OK);
        CHECK(memcmp(x->px, ref.px, (size_t)ref.stride * ref.lay.height) == 0);
    }
    /* Finely through the ends of sweeps (the last columns turn as the wipe completes)
     * and the starts of the next syllables. */
    for (i = 0; i < 120; i++) {
        static const int ends[] = { 980, 1480, 1980, 2580, 3080 };
        fa_time at = (fa_time)(ends[i / 24] + 2 * (i % 24)) * MS;
        target* x = &t[i % 3];
        CHECK_EQ(draw_at(a, x, at, &fr), FA_OK);
        ref.rev = 0;
        CHECK_EQ(draw_at(b, &ref, at, &fr), FA_OK);
        CHECK(memcmp(x->px, ref.px, (size_t)ref.stride * ref.lay.height) == 0);
    }
    fa_surface_destroy(a);
    fa_surface_destroy(b);
    for (i = 0; i < 3; i++) free(t[i].px);
    free(ref.px);
    fa_stream_close(s);
}

static void test_countdown_drawn(void)
{
    fa_stream* s = open_song();
    fa_lyric_style st;
    fa_surface* f = NULL;
    fa_surface_frame fr;
    target t;
    CHECK(s != NULL);
    style(&st, FA_LYRIC_CLASSIC);
    st.effects.outline.on = st.effects.shadow.on = 0;
    CHECK_EQ(fa_lyric_surface_create(s, &st, &f), FA_OK);
    CHECK(target_make(f, &t));
    /* Page 1's line is due at 9.6 s with its countdown from 4.6 s: at 5.1 s the first
     * arrow is half way, the others red. */
    CHECK_EQ(draw_at(f, &t, 5100 * MS, &fr), FA_OK);
    CHECK(count_color(&t, 255, 0, 0) > 50);
    CHECK_EQ(count_color(&t, 0, 192, 0), 0);
    CHECK_EQ(draw_at(f, &t, 8500 * MS, &fr), FA_OK);
    CHECK(count_color(&t, 0, 192, 0) > 50);
    CHECK_EQ(count_color(&t, 255, 0, 0), 0);
    /* Off: no arrows. */
    st.countdown = FA_COUNTDOWN_OFF;
    CHECK_EQ(fa_lyric_surface_set_style(f, &st), FA_OK);
    free(t.px);
    CHECK(target_make(f, &t));
    t.rev = 0;
    CHECK_EQ(draw_at(f, &t, 8700 * MS, &fr), FA_OK);
    CHECK_EQ(count_color(&t, 0, 192, 0), 0);
    fa_surface_destroy(f);
    free(t.px);
    fa_stream_close(s);
}

static void test_presets(void)
{
    static const fa_lyric_preset all[] = { FA_LYRIC_CLASSIC, FA_LYRIC_MODERN, FA_LYRIC_NEON, FA_LYRIC_MINIMAL,
                                           FA_LYRIC_SUBTITLE };
    fa_stream* s = open_song();
    size_t i;
    CHECK(s != NULL);
    for (i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        fa_lyric_style st;
        fa_surface* f = NULL;
        fa_surface_frame fr;
        target t;
        style(&st, all[i]);
        CHECK_EQ(fa_lyric_surface_create(s, &st, &f), FA_OK);
        CHECK(target_make(f, &t));
        CHECK_EQ(draw_at(f, &t, 1300 * MS, &fr), FA_OK);
        CHECK(count_color(&t, 255, 255, 255) + count_color(&t, 255, 0xE6, 0xFA) > 100);
        if (all[i] == FA_LYRIC_SUBTITLE) {
            /* A band near the bottom, a panel behind the text. */
            CHECK(t.lay.bitmap_y > 1080 / 2);
            CHECK(t.lay.height < 300);
        }
        fa_surface_destroy(f);
        free(t.px);
    }
    fa_stream_close(s);
}

static void test_sizes(void)
{
    fa_stream* s = open_song();
    fa_lyric_style st;
    fa_surface* f = NULL;
    fa_surface_frame fr;
    fa_surface_layout big;
    target t;
    memset(&big, 0, sizeof(big));
    big.struct_size = (uint32_t)sizeof(big);
    CHECK(s != NULL);
    style(&st, FA_LYRIC_CLASSIC);
    CHECK_EQ(fa_lyric_surface_create(s, &st, &f), FA_OK);
    memset(&fr, 0, sizeof(fr));
    fr.struct_size = (uint32_t)sizeof(fr);
    CHECK_EQ(fa_surface_get_layout(f, &big), FA_OK);
    CHECK_EQ(fa_surface_set_presentation_size(f, 1280, 720), FA_OK);
    CHECK(target_make(f, &t));
    CHECK_NEAR(t.lay.scale, 720.0 / 1080.0, 1e-6);
    CHECK(t.lay.width < big.width);
    CHECK(t.lay.layout_revision != big.layout_revision);
    CHECK_NEAR(t.lay.bitmap_x + t.lay.width / 2.0, 640.0, 1.0);
    CHECK_EQ(fa_surface_update_into(f, 1700 * MS, t.px, t.stride, big.width, big.height, 0, &fr), FA_ERROR_SIZE_MISMATCH);
    CHECK_EQ(fa_surface_update_into(f, 1700 * MS, t.px, t.stride - 4, t.lay.width, t.lay.height, 0, &fr), FA_ERROR_INVALID_ARG);
    CHECK_EQ(draw_at(f, &t, 1300 * MS, &fr), FA_OK);
    CHECK(count_color(&t, 255, 255, 0) > 100);
    /* A bigger layout and back: a buffer of the first one is drawn in full, never
     * with the bigger layout's areas. */
    {
        target big2;
        uint64_t kept = t.rev;
        CHECK_EQ(fa_surface_set_presentation_size(f, 3840, 2160), FA_OK);
        CHECK(target_make(f, &big2));
        CHECK_EQ(draw_at(f, &big2, 1350 * MS, &fr), FA_OK);
        CHECK_EQ(draw_at(f, &big2, 1400 * MS, &fr), FA_OK);
        free(big2.px);
        CHECK_EQ(fa_surface_set_presentation_size(f, 1280, 720), FA_OK);
        t.rev = kept;
        CHECK_EQ(draw_at(f, &t, 1450 * MS, &fr), FA_OK);
        CHECK_EQ(fr.base_revision, 0);
        CHECK_EQ(fr.dirty_bounds.width, t.lay.width);
    }
    fa_surface_destroy(f);
    free(t.px);
    fa_stream_close(s);
}

static void test_styles(void)
{
    fa_stream* s = open_song();
    fa_lyric_style st, got;
    fa_surface_effects e;
    fa_cdg_frame cf;
    fa_cdg_presentation pres;
    fa_surface* f = NULL;
    CHECK(s != NULL);
    CHECK_EQ(fa_lyric_surface_create(NULL, NULL, &f), FA_ERROR_INVALID_ARG);
    style(&st, FA_LYRIC_CLASSIC);
    st.font_size = 1.0f;
    CHECK_EQ(fa_lyric_surface_create(s, &st, &f), FA_ERROR_INVALID_ARG);
    CHECK(f == NULL);
    style(&st, FA_LYRIC_CLASSIC);
    st.lines_above = 3;
    CHECK_EQ(fa_lyric_surface_create(s, &st, &f), FA_ERROR_INVALID_ARG);
    style(&st, FA_LYRIC_CLASSIC);
    st.font_path = FA_TEST_DATA_DIR "/fonts/missing.ttf";
    CHECK_EQ(fa_lyric_surface_create(s, &st, &f), FA_ERROR_NOT_FOUND);
    st.font_path = NULL;
    st.font_data = "not a font";
    st.font_data_size = 10;
    CHECK_EQ(fa_lyric_surface_create(s, &st, &f), FA_ERROR_UNSUPPORTED_FORMAT);
    style(&st, FA_LYRIC_MODERN);
    st.font_size = 60.0f;
    CHECK_EQ(fa_lyric_surface_create(s, &st, &f), FA_OK);
    memset(&got, 0, sizeof(got));
    got.struct_size = (uint32_t)sizeof(got);
    CHECK_EQ(fa_lyric_surface_get_style(f, &got), FA_OK);
    CHECK_NEAR(got.font_size, 60.0, 1e-6);
    CHECK_EQ(got.wipe, FA_WIPE_SWEEP);
    CHECK(got.font_path != st.font_path && strcmp(got.font_path, st.font_path) == 0);
    /* A bad style leaves the old one. */
    st.box_width = 2.0f;
    CHECK_EQ(fa_lyric_surface_set_style(f, &st), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_lyric_surface_get_style(f, &got), FA_OK);
    CHECK_NEAR(got.box_width, 0.85, 1e-6);
    /* The effects are the style's. */
    memset(&e, 0, sizeof(e));
    e.struct_size = (uint32_t)sizeof(e);
    CHECK_EQ(fa_surface_get_effects(f, &e), FA_OK);
    CHECK(e.outline.on && e.outline.color == got.effects.outline.color);
    e.bevel.on = 1;
    CHECK_EQ(fa_surface_set_effects(f, &e), FA_OK);
    CHECK_EQ(fa_lyric_surface_get_style(f, &got), FA_OK);
    CHECK(got.effects.bevel.on);
    /* CD+G's own calls are not for it. */
    CHECK_EQ(fa_cdg_surface_update(f, &cf), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_cdg_surface_update_at(f, 0, &cf), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_cdg_surface_set_presentation(f, NULL), FA_ERROR_INVALID_ARG);
    pres.struct_size = (uint32_t)sizeof(pres);
    CHECK_EQ(fa_cdg_surface_get_presentation(f, &pres), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_lyric_surface_set_style(NULL, &st), FA_ERROR_INVALID_ARG);
    /* It outlives its stream, drawing nothing more. */
    fa_stream_close(s);
    {
        fa_surface_layout lay;
        fa_surface_frame fr;
        uint8_t px[16];
        memset(&lay, 0, sizeof(lay));
        memset(&fr, 0, sizeof(fr));
        lay.struct_size = (uint32_t)sizeof(lay);
        fr.struct_size = (uint32_t)sizeof(fr);
        CHECK_EQ(fa_surface_get_layout(f, &lay), FA_OK);
        CHECK_EQ(fa_surface_update_into(f, 0, px, 4, 1, 1, 0, &fr), FA_ERROR_INVALID_ARG);
    }
    fa_surface_destroy(f);
}

/* A song without lyrics draws nothing; a CD+G surface's calls refuse lyric styles. */
static void test_no_lyrics(void)
{
    buf song = { 0 }, t = { 0 };
    fa_open_options o;
    fa_stream* s = NULL;
    fa_lyric_style st;
    fa_surface* f = NULL;
    fa_surface_frame fr;
    target tg;
    put(&song, "MThd", 4); put32be(&song, 6); put8(&song, 0); put8(&song, 0); put8(&song, 0); put8(&song, 1); put8(&song, 1); put8(&song, 0xE0);
    ev3(&t, 0, 0x90, 69, 1);
    ev3(&t, 960, 0x80, 69, 0);
    varlen(&t, 0); put8(&t, 0xFF); put8(&t, 0x2F); put8(&t, 0);
    put(&song, "MTrk", 4); put32be(&song, (uint32_t)t.n); put(&song, t.d, t.n);
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    CHECK_EQ(fa_stream_open_memory(g_engine, song.d, song.n, &o, &s), FA_OK);
    style(&st, FA_LYRIC_CLASSIC);
    CHECK_EQ(fa_lyric_surface_create(s, &st, &f), FA_OK);
    CHECK(target_make(f, &tg));
    CHECK_EQ(draw_at(f, &tg, 500 * MS, &fr), FA_OK);
    CHECK_EQ(count_color(&tg, 255, 255, 255), 0);
    fa_surface_destroy(f);
    free(tg.px);
    fa_stream_close(s);
    free(song.d);
    free(t.d);
}

/* ---- intro and ending pages -------------------------------------------------------- */

static void test_intro_time(void)
{
    ly_params p;
    ly_timeline tl;
    ly_frag f[] = { { "a", 1 * SEC, 0 }, { "b", 10 * SEC, FA_MIDI_NEW_PARAGRAPH } };
    params(&p);
    p.intro = p.ending = 1;
    p.intro_start = 1000 * MS;
    p.intro_fade = 1000 * MS;
    p.intro_clear = 3000 * MS;
    p.intro_band = 8000 * MS;
    p.ending_min = 3000 * MS;
    p.ending_fade = 750 * MS;
    /* Lyrics from 1 s: bands until 8 s. */
    CHECK_EQ(ly_build(&tl, f, 2, &p, NULL, NULL, 0.0f, NULL, NULL), FA_OK);
    CHECK(tl.intro_bands);
    CHECK_EQ(tl.intro_end, 8 * SEC);
    CHECK_NEAR(ly_intro_at(&tl, &p, 500 * MS), 0.0, 1e-6);
    CHECK_NEAR(ly_intro_at(&tl, &p, 1500 * MS), 0.5, 1e-5);
    CHECK_NEAR(ly_intro_at(&tl, &p, 4 * SEC), 1.0, 1e-6);
    CHECK_NEAR(ly_intro_at(&tl, &p, 7500 * MS), 0.5, 1e-5);
    CHECK_NEAR(ly_intro_at(&tl, &p, 8 * SEC), 0.0, 1e-6);
    ly_free(&tl);
    /* Lyrics from 20 s: a card, gone 3 s before them; with a countdown, 3 s before it. */
    f[0].time = 20 * SEC;
    f[1].time = 30 * SEC;
    CHECK_EQ(ly_build(&tl, f, 2, &p, NULL, NULL, 0.0f, NULL, NULL), FA_OK);
    CHECK(!tl.intro_bands);
    CHECK_EQ(tl.intro_end, 17 * SEC);
    CHECK_NEAR(ly_intro_at(&tl, &p, 16500 * MS), 0.5, 1e-5);
    p.countdown = FA_COUNTDOWN_CLOCK;
    ly_schedule(&tl, &p, NULL, NULL);
    CHECK_EQ(tl.intro_end, 12 * SEC);
    /* The ending card: from the end of the lyrics' fade (36 s) to the song's end. */
    CHECK_NEAR(ly_ending_at(&tl, &p, 35900 * MS), 0.0, 1e-6);
    CHECK_NEAR(ly_ending_at(&tl, &p, 36375 * MS), 0.5, 1e-5);
    CHECK_NEAR(ly_ending_at(&tl, &p, 60 * SEC), 1.0, 1e-6);
    CHECK_NEAR(ly_ending_at(&tl, &p, 99625 * MS), 0.5, 1e-5);
    CHECK_NEAR(ly_ending_at(&tl, &p, 100 * SEC), 0.0, 1e-6);
    /* Not with less than ending_min left; neither page when off. */
    p.duration = 38 * SEC;
    ly_schedule(&tl, &p, NULL, NULL);
    CHECK_NEAR(ly_ending_at(&tl, &p, 37 * SEC), 0.0, 1e-6);
    p.intro = 0;
    CHECK_NEAR(ly_intro_at(&tl, &p, 5 * SEC), 0.0, 1e-6);
    ly_free(&tl);
    /* No room to fade in and out (a card gone by 1 s): no intro at all. */
    p.intro = 1;
    p.countdown = FA_COUNTDOWN_OFF;
    p.intro_band = 0;
    f[0].time = 4 * SEC;
    f[1].time = 10 * SEC;
    CHECK_EQ(ly_build(&tl, f, 2, &p, NULL, NULL, 0.0f, NULL, NULL), FA_OK);
    CHECK(!tl.intro_bands);
    CHECK_NEAR(ly_intro_at(&tl, &p, 1500 * MS), 0.0, 1e-6);
    ly_free(&tl);
}

static void text_event(buf* t, uint32_t delta, const char* text)
{
    size_t n = strlen(text);
    varlen(t, delta); put8(t, 0xFF); put8(t, 0x01); varlen(t, (uint32_t)n); put(t, text, n);
}

/* make_song with "@T" title lines (NULL: none), its lyrics `shift` seconds later, and
 * its note held to `end` seconds. */
static buf make_titled(const char* title, const char* artist, uint32_t shift, uint32_t end)
{
    buf f = { 0 }, t = { 0 };
    put(&f, "MThd", 4); put32be(&f, 6); put8(&f, 0); put8(&f, 0); put8(&f, 0); put8(&f, 1); put8(&f, 480 >> 8); put8(&f, 480 & 255);
    if (title) text_event(&t, 0, title);
    if (artist) text_event(&t, 0, artist);
    ev3(&t, 0, 0x90, 69, 1);
    lyric(&t, 960 + shift * 960, "AB");
    lyric(&t, 480, "BA ");
    lyric(&t, 480, "VO");
    lyric(&t, 960, "\rOB");
    lyric(&t, 480, "AV");
    lyric(&t, 6240, "BOA");
    lyric(&t, 480, " VA");
    ev3(&t, end * 960 - (shift * 960 + 10080), 0x80, 69, 0);
    varlen(&t, 0); put8(&t, 0xFF); put8(&t, 0x2F); put8(&t, 0);
    put(&f, "MTrk", 4); put32be(&f, (uint32_t)t.n); put(&f, t.d, t.n);
    free(t.d);
    return f;
}

static fa_stream* open_buf(const buf* b)
{
    fa_open_options o;
    fa_stream* s = NULL;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    return fa_stream_open_memory(g_engine, b->d, b->n, &o, &s) == FA_OK ? s : NULL;
}

/* The alpha of the bitmap's rows [y0, y1), summed. */
static long alpha_rows(const target* t, uint32_t y0, uint32_t y1)
{
    uint32_t x, y;
    long sum = 0;
    for (y = y0; y < y1 && y < t->lay.height; y++)
        for (x = 0; x < t->lay.width; x++) sum += t->px[(size_t)y * (size_t)t->stride + x * 4 + 3];
    return sum;
}

static const uint8_t* pixel(const target* t, uint32_t x, uint32_t y) { return t->px + (size_t)y * (size_t)t->stride + x * 4; }

/* Lyrics from 1 s: the intro on bands at the top and bottom, the ending card after the
 * lyrics' fade. The bitmap is the whole area. */
static void test_intro_bands(void)
{
    buf song = make_titled("@TBOA", "@TVOB", 0, 40);
    fa_stream* s = open_buf(&song);
    fa_lyric_style st;
    fa_surface* f = NULL;
    fa_surface_frame fr;
    target t;
    CHECK(s != NULL);
    style(&st, FA_LYRIC_CLASSIC);
    CHECK_EQ(fa_lyric_surface_create(s, &st, &f), FA_OK);
    CHECK(target_make(f, &t));
    CHECK_EQ(t.lay.width, 1920);
    CHECK_EQ(t.lay.height, 1080);
    CHECK_EQ(t.lay.bitmap_x, 0);
    CHECK_EQ(t.lay.bitmap_y, 0);
    CHECK_EQ(t.lay.content_width, (uint32_t)ceil(0.85 * 1920));
    CHECK(t.lay.content_y > 150 && t.lay.content_y + t.lay.content_height < 930);
    CHECK_EQ(draw_at(f, &t, 500 * MS, &fr), FA_OK);
    CHECK_EQ(alpha_rows(&t, 0, 150), 0);
    /* At 4 s: glass at the top and the bottom, the title on it. */
    CHECK_EQ(draw_at(f, &t, 4 * SEC, &fr), FA_OK);
    CHECK(pixel(&t, 10, 60)[3] > 150);
    CHECK(pixel(&t, 10, 1070)[3] > 150);
    CHECK_EQ(pixel(&t, 10, 400)[3], 0);
    CHECK(count_color(&t, 255, 244, 220) > 50);
    /* Gone by 8 s. */
    CHECK_EQ(draw_at(f, &t, 8 * SEC, &fr), FA_OK);
    CHECK_EQ(alpha_rows(&t, 0, 150), 0);
    CHECK_EQ(alpha_rows(&t, 950, 1080), 0);
    /* The ending card: in the middle from 16.5 s. */
    CHECK_EQ(draw_at(f, &t, 16 * SEC, &fr), FA_OK);
    CHECK_EQ(count_color(&t, 255, 244, 220), 0);
    CHECK_EQ(draw_at(f, &t, 25 * SEC, &fr), FA_OK);
    CHECK(pixel(&t, 960, 540)[3] > 200);
    CHECK(count_color(&t, 255, 244, 220) > 50);
    CHECK_EQ(alpha_rows(&t, 0, 150), 0);
    fa_surface_destroy(f);
    free(t.px);
    fa_stream_close(s);
    free(song.d);
}

/* Lyrics from 15 s after a countdown from 10 s: the intro on a card, gone by 7 s. */
static void test_intro_card(void)
{
    buf song = make_titled("@TBOA", "@TVOB", 14, 60);
    fa_stream* s = open_buf(&song);
    fa_lyric_style st;
    fa_surface* f = NULL;
    fa_surface_frame fr;
    target t;
    CHECK(s != NULL);
    style(&st, FA_LYRIC_CLASSIC);
    CHECK_EQ(fa_lyric_surface_create(s, &st, &f), FA_OK);
    CHECK(target_make(f, &t));
    CHECK_EQ(draw_at(f, &t, 4 * SEC, &fr), FA_OK);
    CHECK(pixel(&t, 960, 540)[3] > 200);
    CHECK_EQ(alpha_rows(&t, 0, 150), 0);
    CHECK(count_color(&t, 255, 244, 220) > 50);
    CHECK_EQ(draw_at(f, &t, 7500 * MS, &fr), FA_OK);
    CHECK_EQ(alpha_rows(&t, 0, 1080), 0);
    fa_surface_destroy(f);
    free(t.px);
    fa_stream_close(s);
    free(song.d);
}

/* When the pages show: a song naming nothing has them only with fallback_title; off,
 * subtitle mode or an older style's size. */
static void test_intro_when(void)
{
    buf song = make_titled("@TBOA", NULL, 0, 40);
    fa_stream *s = open_buf(&song), *plain = open_song();
    fa_lyric_style st, got;
    fa_surface* f = NULL;
    fa_surface_frame fr;
    target t;
    CHECK(s != NULL && plain != NULL);
    /* An empty fallback title is none. */
    style(&st, FA_LYRIC_CLASSIC);
    st.fallback_title = "";
    CHECK_EQ(fa_lyric_surface_create(plain, &st, &f), FA_OK);
    CHECK(target_make(f, &t));
    CHECK(t.lay.height < 1080);
    fa_surface_destroy(f);
    free(t.px);
    /* A plain song with a fallback title: bands with it. */
    style(&st, FA_LYRIC_CLASSIC);
    st.fallback_title = "VOBA";
    st.labels = "L\nK";
    CHECK_EQ(fa_lyric_surface_create(plain, &st, &f), FA_OK);
    CHECK(target_make(f, &t));
    CHECK_EQ(t.lay.height, 1080);
    CHECK_EQ(draw_at(f, &t, 4 * SEC, &fr), FA_OK);
    CHECK(count_color(&t, 255, 244, 220) > 50);
    memset(&got, 0, sizeof(got));
    got.struct_size = (uint32_t)sizeof(got);
    CHECK_EQ(fa_lyric_surface_get_style(f, &got), FA_OK);
    CHECK(got.fallback_title != st.fallback_title && strcmp(got.fallback_title, "VOBA") == 0);
    CHECK(strcmp(got.labels, "L\nK") == 0);
    fa_surface_destroy(f);
    free(t.px);
    /* Pages off, or subtitle mode: the lyric block. */
    style(&st, FA_LYRIC_CLASSIC);
    st.intro_page = st.ending_page = 0;
    CHECK_EQ(fa_lyric_surface_create(s, &st, &f), FA_OK);
    CHECK(target_make(f, &t));
    CHECK(t.lay.height < 1080);
    fa_surface_destroy(f);
    free(t.px);
    style(&st, FA_LYRIC_SUBTITLE);
    CHECK_EQ(fa_lyric_surface_create(s, &st, &f), FA_OK);
    CHECK(target_make(f, &t));
    CHECK(t.lay.height < 1080);
    fa_surface_destroy(f);
    free(t.px);
    /* A style of the size before the pages: they are on. */
    style(&st, FA_LYRIC_CLASSIC);
    st.struct_size = (uint32_t)offsetof(fa_lyric_style, intro_page);
    CHECK_EQ(fa_lyric_surface_create(s, &st, &f), FA_OK);
    CHECK(target_make(f, &t));
    CHECK_EQ(t.lay.height, 1080);
    CHECK_EQ(fa_lyric_surface_get_style(f, &got), FA_OK);
    CHECK_EQ(got.intro_page, 1);
    CHECK_EQ(got.ending_fade_ms, 750);
    fa_surface_destroy(f);
    free(t.px);
    /* Out of range. */
    style(&st, FA_LYRIC_CLASSIC);
    st.intro_page = 2;
    CHECK_EQ(fa_lyric_surface_create(s, &st, &f), FA_ERROR_INVALID_ARG);
    style(&st, FA_LYRIC_CLASSIC);
    st.ending_fade_ms = -1;
    CHECK_EQ(fa_lyric_surface_create(s, &st, &f), FA_ERROR_INVALID_ARG);
    fa_stream_close(s);
    fa_stream_close(plain);
    free(song.d);
}

/* Pages drawn by changed areas equal pages drawn at once. */
static void test_intro_stateless(void)
{
    static const int ms[] = { 0, 1200, 1500, 4000, 7600, 8200, 12000, 16000, 16700, 17000, 25000, 39900, 3000, 20000, 1200, 7000 };
    buf song = make_titled("@TBOA", "@TVOB", 0, 40);
    fa_stream* s = open_buf(&song);
    fa_lyric_style st;
    fa_surface *a = NULL, *b = NULL;
    fa_surface_frame fr;
    target ta, tb;
    size_t i;
    CHECK(s != NULL);
    style(&st, FA_LYRIC_CLASSIC);
    CHECK_EQ(fa_lyric_surface_create(s, &st, &a), FA_OK);
    CHECK(target_make(a, &ta));
    for (i = 0; i < sizeof(ms) / sizeof(ms[0]); i++) {
        CHECK_EQ(draw_at(a, &ta, (fa_time)ms[i] * MS, &fr), FA_OK);
        CHECK_EQ(fa_lyric_surface_create(s, &st, &b), FA_OK);
        CHECK(target_make(b, &tb));
        CHECK_EQ(draw_at(b, &tb, (fa_time)ms[i] * MS, &fr), FA_OK);
        if (memcmp(ta.px, tb.px, (size_t)ta.stride * ta.lay.height) != 0) {
            fprintf(stderr, "  differs at %d ms\n", ms[i]);
            fa_surface_destroy(b);
            free(tb.px);
            CHECK(0);
        }
        fa_surface_destroy(b);
        free(tb.px);
    }
    /* Small steps (each a tiny move at a scroll's ends) add up to what a fresh draw
     * gives. */
    {
        int k;
        for (k = 0; k <= 6000; k += 3) {
            CHECK_EQ(draw_at(a, &ta, (fa_time)k * MS, &fr), FA_OK);
            if (k % 600) continue;
            CHECK_EQ(fa_lyric_surface_create(s, &st, &b), FA_OK);
            CHECK(target_make(b, &tb));
            CHECK_EQ(draw_at(b, &tb, (fa_time)k * MS, &fr), FA_OK);
            if (memcmp(ta.px, tb.px, (size_t)ta.stride * ta.lay.height) != 0) {
                fprintf(stderr, "  differs at %d ms after small steps\n", k);
                fa_surface_destroy(b);
                free(tb.px);
                CHECK(0);
            }
            fa_surface_destroy(b);
            free(tb.px);
        }
    }
    fa_surface_destroy(a);
    free(ta.px);
    fa_stream_close(s);
    free(song.d);
}

/* Render-ahead: every frame shown is the picture at the time it says, from slots a
 * worker filled or drawn here, through two buffers in turn; paced like a display the
 * worker has the frames ready. */
static int same_at(fa_surface* ref, target* rt, fa_time time, const target* got)
{
    fa_surface_frame fr;
    uint32_t y;
    if (draw_at(ref, rt, time, &fr) != FA_OK) return 0;
    for (y = 0; y < got->lay.height; y++)
        if (memcmp(rt->px + (size_t)y * (size_t)rt->stride, got->px + (size_t)y * (size_t)got->stride, (size_t)got->lay.width * 4))
            return 0;
    return 1;
}

static void test_render_ahead(void)
{
    buf song = make_titled("@TBOA", "@TVOB", 0, 40);
    fa_stream* s = open_buf(&song);
    fa_lyric_style st;
    fa_surface *f = NULL, *ref = NULL;
    fa_surface_frame fr;
    target t[2], rt;
    int k, b, behind = 0, partial = 0, frames = 0;
    CHECK(s != NULL);
    style(&st, FA_LYRIC_CLASSIC);
    CHECK_EQ(fa_lyric_surface_create(s, &st, &f), FA_OK);
    CHECK_EQ(fa_lyric_surface_create(s, &st, &ref), FA_OK);
    CHECK_EQ(fa_surface_set_presentation_size(f, 1280, 720), FA_OK);
    CHECK_EQ(fa_surface_set_presentation_size(ref, 1280, 720), FA_OK);
    CHECK_EQ(fa_surface_set_render_ahead(f, 1), FA_OK);
    CHECK(target_make(f, &t[0]));
    CHECK(target_make(f, &t[1]));
    CHECK(target_make(ref, &rt));
    for (k = 0; k < 2; k++) t[k].stride += 8;
    for (k = 0; k < 2; k++) {
        free(t[k].px);
        t[k].px = (uint8_t*)calloc((size_t)t[k].stride * t[k].lay.height, 1);
        CHECK(t[k].px != NULL);
    }
    for (k = 0; k <= 12000; k += 17) {
        b = (k / 17) % 2;
        CHECK_EQ(draw_at(f, &t[b], (fa_time)k * MS, &fr), FA_OK);
        frames++;
        if (fr.behind) behind++;
        if (fr.base_revision) partial++;
        /* On time: of this time, up to 1/60 s before it or a hair after; late frames say so. */
        CHECK(fr.time <= (fa_time)k * MS + 8333334);
        CHECK(fr.behind || fr.time >= (fa_time)k * MS - 16666667);
        if (!same_at(ref, &rt, fr.time, &t[b])) {
            fprintf(stderr, "  differs at %d ms (frame of %lld ns)\n", k, (long long)fr.time);
            CHECK(0);
        }
        if (k > 6000) fa_sleep_ms(5);
    }
    printf("    %d of %d frames behind, %d partial\n", behind, frames, partial);
    CHECK(partial > 0);
    /* Paused: the frame of that time. A seek back: drawn here. */
    CHECK_EQ(draw_at(f, &t[0], 4000 * MS, &fr), FA_OK);
    CHECK_EQ(draw_at(f, &t[0], 4000 * MS, &fr), FA_OK);
    CHECK_EQ(fr.time, 4000 * MS);
    CHECK(same_at(ref, &rt, fr.time, &t[0]));
    /* A new presentation size: the old buffer no longer fits; a new one does. */
    CHECK_EQ(fa_surface_set_presentation_size(f, 1920, 1080), FA_OK);
    CHECK_EQ(draw_at(f, &t[0], 4100 * MS, &fr), FA_ERROR_SIZE_MISMATCH);
    free(t[0].px);
    CHECK(target_make(f, &t[0]));
    CHECK_EQ(draw_at(f, &t[0], 4100 * MS, &fr), FA_OK);
    CHECK_EQ(fr.layout_revision, t[0].lay.layout_revision);
    /* Off again: drawn on this thread as before. */
    CHECK_EQ(fa_surface_set_render_ahead(f, 0), FA_OK);
    CHECK_EQ(draw_at(f, &t[0], 4200 * MS, &fr), FA_OK);
    CHECK_EQ(fr.time, 4200 * MS);
    fa_surface_destroy(f);
    fa_surface_destroy(ref);
    free(t[0].px);
    free(t[1].px);
    free(rt.px);
    fa_stream_close(s);
    free(song.d);
}

int main(void)
{
    fa_engine_config cfg;
    buf sf = make_sf2();
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    if (fa_engine_create(&cfg, &g_engine) != FA_OK) { fprintf(stderr, "engine create failed\n"); return 1; }
    if (fa_soundfont_open_memory(sf.d, sf.n, &g_font) != FA_OK) { fprintf(stderr, "test soundfont rejected\n"); return 1; }
    free(sf.d);
    fa_engine_set_soundfont(g_engine, g_font);
    g_song = make_song();
    RUN(test_lines);
    RUN(test_gaps);
    RUN(test_break_length);
    RUN(test_split_wide);
    RUN(test_scroll);
    RUN(test_slots);
    RUN(test_wipe);
    RUN(test_countdown);
    RUN(test_pages);
    RUN(test_end);
    RUN(test_surface);
    RUN(test_wipe_area);
    RUN(test_stateless);
    RUN(test_rotation);
    RUN(test_countdown_drawn);
    RUN(test_presets);
    RUN(test_sizes);
    RUN(test_styles);
    RUN(test_no_lyrics);
    RUN(test_intro_time);
    RUN(test_intro_bands);
    RUN(test_intro_card);
    RUN(test_intro_when);
    RUN(test_intro_stateless);
    RUN(test_render_ahead);
    free(g_song.d);
    fa_soundfont_close(g_font);
    fa_engine_destroy(g_engine);
    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("all lyric tests passed\n");
    return 0;
}
