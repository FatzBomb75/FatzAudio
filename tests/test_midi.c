/* MIDI: detection, length, sample-exact seeking, lyrics and other texts, live
 * controls (transpose, channel mute), soundfont sharing, DLS banks, the OS's bank
 * as the fallback, the missing-soundfont error, FA_EVENT_MIDI_TEXT on a device, loudness scans, and malformed MIDI and
 * SF2 data. The soundfont (a sine wave, a "drum" with the same sample) and every
 * MIDI file are built here, so no third-party data is needed. */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "fa_internal.h"
#include <math.h>
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

#define RATE 44100
#define PI 3.14159265358979323846
#define FA_MIDI_TEST_DRUMS 9

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
static void put32be(buf* b, uint32_t v) { put8(b, v >> 24); put8(b, (v >> 16) & 255); put8(b, (v >> 8) & 255); put8(b, v & 255); }
static void putstr(buf* b, const char* s, size_t n) { size_t k = strlen(s); put(b, s, k < n ? k : n); while (k++ < n) put8(b, 0); }

/* A RIFF chunk: id, size, body. */
static void chunk(buf* b, const char* id, const buf* body)
{
    put(b, id, 4);
    put32le(b, (uint32_t)body->n);
    put(b, body->d, body->n);
    if (body->n & 1) put8(b, 0);
}

/* ---- the soundfont ---------------------------------------------------------- */

#define SINE_LEN 4410   /* 100 periods of 440 Hz at 44.1 kHz: loops seamlessly */

static void gen(buf* b, unsigned oper, unsigned amount) { put16le(b, oper); put16le(b, amount); }

/* Preset 0 bank 0 ("Sine") and preset 0 bank 128 ("Kit"): the same looped 440 Hz
 * sine, root key 69, 125 ms release. `atten` (centibels) is the Sine zone's
 * initialAttenuation; 0 leaves it out. */
static buf make_sf2_ex3(unsigned atten, int kit_coarse, unsigned reverb)
{
    buf sf = { 0 }, riff = { 0 }, info = { 0 }, sdta = { 0 }, pdta = { 0 }, c = { 0 };
    int i;
    put(&riff, "sfbk", 4);
    /* INFO */
    put(&info, "INFO", 4);
    c.n = 0; put16le(&c, 2); put16le(&c, 1); chunk(&info, "ifil", &c);
    c.n = 0; putstr(&c, "Test Sine", 10); chunk(&info, "INAM", &c);
    chunk(&riff, "LIST", &info);
    /* samples: the sine, then 46 zero samples as the format wants */
    put(&sdta, "sdta", 4);
    c.n = 0;
    for (i = 0; i < SINE_LEN; i++) put16le(&c, (unsigned)(uint16_t)(int16_t)lrint(sin(2.0 * PI * 440.0 * i / RATE) * 32000.0));
    for (i = 0; i < 46; i++) put16le(&c, 0);
    chunk(&sdta, "smpl", &c);
    chunk(&riff, "LIST", &sdta);
    /* hydra */
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
    {
        unsigned first = 3 + (atten != 0) + (reverb != 0);
        c.n = 0; put16le(&c, 0); put16le(&c, 0); put16le(&c, first); put16le(&c, 0); put16le(&c, first + 3 + (kit_coarse != 0)); put16le(&c, 0);
    }
    chunk(&pdta, "ibag", &c);
    c.n = 0; for (i = 0; i < 10; i++) put8(&c, 0);
    chunk(&pdta, "imod", &c);
    c.n = 0;
    gen(&c, 54, 1);                          /* sampleModes: loop */
    gen(&c, 38, (unsigned)(uint16_t)-3600);  /* releaseVolEnv: 2^-3 s */
    if (atten) gen(&c, 48, atten);           /* initialAttenuation */
    if (reverb) gen(&c, 16, reverb);         /* reverbEffectsSend, 0.1% */
    gen(&c, 53, 0);                          /* sampleID */
    gen(&c, 54, 1); gen(&c, 38, (unsigned)(uint16_t)-3600);
    if (kit_coarse) gen(&c, 51, (unsigned)(uint16_t)(int16_t)kit_coarse);   /* coarseTune */
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

static buf make_sf2_ex(unsigned atten, int kit_coarse) { return make_sf2_ex3(atten, kit_coarse, 0); }
static buf make_sf2_atten(unsigned atten) { return make_sf2_ex(atten, 0); }
static buf make_sf2(void) { return make_sf2_ex(0, 0); }

/* ---- MIDI files -------------------------------------------------------------------- */

static void varlen(buf* b, uint32_t v)
{
    uint8_t tmp[5];
    int n = 0;
    tmp[n++] = v & 0x7F;
    while (v >>= 7) tmp[n++] = (uint8_t)(0x80 | (v & 0x7F));
    while (n--) put8(b, tmp[n]);
}
static void ev3(buf* t, uint32_t delta, unsigned status, unsigned a, unsigned b) { varlen(t, delta); put8(t, status); put8(t, a); put8(t, b); }
static void meta(buf* t, uint32_t delta, unsigned type, const char* text)
{
    size_t n = strlen(text);
    varlen(t, delta); put8(t, 0xFF); put8(t, type); varlen(t, (uint32_t)n); put(t, text, n);
}
static void tempo(buf* t, uint32_t delta, uint32_t us)
{
    varlen(t, delta); put8(t, 0xFF); put8(t, 0x51); put8(t, 3); put8(t, us >> 16); put8(t, (us >> 8) & 255); put8(t, us & 255);
}
static void eot(buf* t, uint32_t delta) { varlen(t, delta); put8(t, 0xFF); put8(t, 0x2F); put8(t, 0); }

static void mthd(buf* f, int format, int tracks, int division)
{
    put(f, "MThd", 4); put32be(f, 6); put8(f, 0); put8(f, format); put8(f, 0); put8(f, tracks);
    put8(f, division >> 8); put8(f, division & 255);
}
static void mtrk(buf* f, buf* t) { put(f, "MTrk", 4); put32be(f, (uint32_t)t->n); put(f, t->d, t->n); t->n = 0; }

/* Format 0, 480 ticks per beat at 120 bpm (1 tick = 1/960 s). Note 69 from 0 to
 * 0.5 s, silence, note 69 from 1.5 s to 2 s. Lyrics "Hel" "lo" (0, 0.25 s) and
 * "world" after a CR (1.5 s); title and copyright. */
static buf make_song(void)
{
    buf f = { 0 }, t = { 0 };
    mthd(&f, 0, 1, 480);
    meta(&t, 0, 0x03, "Test Song");
    meta(&t, 0, 0x02, "(c) Nobody");
    ev3(&t, 0, 0xC0, 0, 0);  /* the C0 has two data bytes in ev3; fixed below */
    t.n -= 1;
    ev3(&t, 0, 0x90, 69, 127);
    meta(&t, 0, 0x05, "Hel");
    meta(&t, 240, 0x05, "lo ");
    ev3(&t, 240, 0x80, 69, 0);
    meta(&t, 960, 0x05, "\rworld");
    ev3(&t, 0, 0x90, 69, 100);
    ev3(&t, 480, 0x80, 69, 0);
    eot(&t, 0);
    mtrk(&f, &t);
    free(t.d);
    return f;
}

/* One note (key 69, channel `ch`) from 0 to 1 s. */
static buf make_note(int ch)
{
    buf f = { 0 }, t = { 0 };
    mthd(&f, 0, 1, 480);
    ev3(&t, 0, 0x90 | ch, 69, 127);
    ev3(&t, 960, 0x80 | ch, 69, 0);
    eot(&t, 0);
    mtrk(&f, &t);
    free(t.d);
    return f;
}

static fa_stream* open_bytes(const buf* b, uint32_t flags, fa_soundfont* font, fa_result* out_r)
{
    fa_open_options o;
    fa_stream* s = NULL;
    fa_result r;
    fa_open_options_init(&o);
    o.flags = flags;
    o.soundfont = font;
    r = fa_stream_open_memory(g_engine, b->d, b->n, &o, &s);
    if (out_r) *out_r = r;
    return r == FA_OK ? s : NULL;
}

static fa_stream* open_bytes_cp(const buf* b, int32_t code_page)
{
    fa_open_options o;
    fa_stream* s = NULL;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    o.text_code_page = code_page;
    return fa_stream_open_memory(g_engine, b->d, b->n, &o, &s) == FA_OK ? s : NULL;
}

/* The lyric syllables of a stream, "|" between them, with "/" before a new line
 * and "\" before a new paragraph. */
static void lyric_string(fa_stream* s, char* out, size_t cap)
{
    fa_midi_info info;
    fa_midi_text t;
    uint32_t i;
    out[0] = '\0';
    if (fa_stream_get_midi_info(s, &info) != FA_OK) return;
    for (i = 0; i < info.text_count; i++) {
        size_t n = strlen(out);
        if (fa_stream_get_midi_text(s, i, &t) != FA_OK || t.type != FA_MIDI_LYRIC) continue;
        snprintf(out + n, cap - n, "%s%s%s", n ? "|" : "",
                 (t.flags & FA_MIDI_NEW_PARAGRAPH) ? "\\" : (t.flags & FA_MIDI_NEW_LINE) ? "/" : "", t.text);
    }
}

/* Shift-JIS lyrics are found and read as such (whatever their trail bytes look
 * like: "\x83\x5c" is a katakana, not a "\" mark); Windows-1252 accents that pair
 * up as Shift-JIS are not; the app can force either; a UTF-8 byte order mark goes. */
static void test_text_encoding(void)
{
    buf f = { 0 }, t = { 0 };
    fa_stream* s;
    fa_midi_credits c;
    char got[512];
    mthd(&f, 0, 1, 480);
    meta(&t, 0, 0x05, "\x82\xb3\x82\xad\x82\xe7");      /* さくら */
    meta(&t, 240, 0x05, " \x8d\xe7\x82\xad");           /* " 咲く" */
    meta(&t, 240, 0x05, "/\x83\x5c\x95\x5c");           /* new line, "ソ表" */
    meta(&t, 240, 0x05, "\xf0\x40\x82\xa0");             /* a user-defined character, "あ" */
    ev3(&t, 0, 0x90, 69, 100);
    ev3(&t, 480, 0x80, 69, 0);
    eot(&t, 0);
    mtrk(&f, &t);
    s = open_bytes_cp(&f, 0);
    CHECK(s);
    lyric_string(s, got, sizeof(got));
    CHECK(strcmp(got, "\xe3\x81\x95\xe3\x81\x8f\xe3\x82\x89| \xe5\x92\xb2\xe3\x81\x8f|/\xe3\x82\xbd\xe8\xa1\xa8|\xef\xbf\xbd\xe3\x81\x82") == 0);
    memset(&c, 0, sizeof(c));
    c.struct_size = sizeof(c);
    CHECK_EQ(fa_stream_get_midi_credits(s, &c), FA_OK);
    CHECK_EQ(c.code_page, FA_CODE_PAGE_SHIFT_JIS);
    fa_stream_close(s);
    /* Forced Windows-1252: the same bytes as accented Latin. */
    s = open_bytes_cp(&f, FA_CODE_PAGE_WINDOWS_1252);
    CHECK(s);
    lyric_string(s, got, sizeof(got));
    CHECK(strncmp(got, "\xe2\x80\x9a\xc2\xb3", 5) == 0);   /* 0x82 = U+201A, 0xB3 = U+00B3 */
    CHECK_EQ(fa_stream_get_midi_credits(s, &c), FA_OK);
    CHECK_EQ(c.code_page, FA_CODE_PAGE_WINDOWS_1252);
    fa_stream_close(s);
    free(f.d);
    memset(&f, 0, sizeof(f));

    /* Accented Windows-1252 ("é" + a letter is a valid Shift-JIS kanji pair). */
    mthd(&f, 0, 1, 480);
    meta(&t, 0, 0x05, "Caf\xe9s ");
    meta(&t, 240, 0x05, "\xe0 ");
    meta(&t, 240, 0x05, "No\xebl");
    meta(&t, 240, 0x05, "\xef\xbb\xbfoui");                 /* a byte order mark */
    eot(&t, 0);
    mtrk(&f, &t);
    s = open_bytes_cp(&f, 0);
    CHECK(s);
    lyric_string(s, got, sizeof(got));
    CHECK(strcmp(got, "Caf\xc3\xa9s |\xc3\xa0 |No\xc3\xabl|oui") == 0);
    CHECK_EQ(fa_stream_get_midi_credits(s, &c), FA_OK);
    CHECK_EQ(c.code_page, FA_CODE_PAGE_WINDOWS_1252);
    fa_stream_close(s);
    /* Forced Shift-JIS reads it as such; an unknown code page is detection. */
    s = open_bytes_cp(&f, FA_CODE_PAGE_SHIFT_JIS);
    CHECK(s);
    CHECK_EQ(fa_stream_get_midi_credits(s, &c), FA_OK);
    CHECK_EQ(c.code_page, FA_CODE_PAGE_SHIFT_JIS);
    fa_stream_close(s);
    s = open_bytes_cp(&f, 12345);
    CHECK(s);
    CHECK_EQ(fa_stream_get_midi_credits(s, &c), FA_OK);
    CHECK_EQ(c.code_page, FA_CODE_PAGE_WINDOWS_1252);
    fa_stream_close(s);
    free(f.d);
    memset(&f, 0, sizeof(f));

    /* All UTF-8. */
    mthd(&f, 0, 1, 480);
    meta(&t, 0, 0x05, "Gr\xc3\xbc\xc3\x9f");
    eot(&t, 0);
    mtrk(&f, &t);
    s = open_bytes_cp(&f, 0);
    CHECK(s);
    lyric_string(s, got, sizeof(got));
    CHECK(strcmp(got, "Gr\xc3\xbc\xc3\x9f") == 0);
    CHECK_EQ(fa_stream_get_midi_credits(s, &c), FA_OK);
    CHECK_EQ(c.code_page, FA_CODE_PAGE_UTF8);
    fa_stream_close(s);
    free(f.d); free(t.d);
}

/* Karaoke files: "/" and "\" anywhere in a syllable, a Solton "<" line start,
 * chord lines kept out of the lyrics. */
static void test_lyric_marks(void)
{
    buf f = { 0 }, t = { 0 };
    fa_stream* s;
    fa_midi_info info;
    fa_midi_text x;
    char got[512];
    uint32_t i, chords = 0;
    mthd(&f, 1, 2, 480);
    meta(&t, 0, 0x01, "@KMIDI KARAOKE FILE");
    eot(&t, 0);
    mtrk(&f, &t);
    meta(&t, 0, 0x03, "Words");
    meta(&t, 0, 0x01, "Hel");
    meta(&t, 120, 0x01, "lo/");           /* the mark at the end: the next syllable starts a line */
    meta(&t, 120, 0x01, "world/next ");   /* in the middle: two syllables at one time */
    meta(&t, 120, 0x01, "%Am");           /* Tune1000 chord */
    meta(&t, 120, 0x01, "<C G Am>");      /* Solton chord line */
    meta(&t, 120, 0x01, "<New line");     /* Solton line start */
    meta(&t, 120, 0x01, "\\end\r");
    meta(&t, 120, 0x01, "last");
    eot(&t, 0);
    mtrk(&f, &t);
    s = open_bytes(&f, FA_OPEN_DECODE_ONLY, NULL, NULL);
    CHECK(s);
    lyric_string(s, got, sizeof(got));
    CHECK(strcmp(got, "Hel|lo|/world|/next |\\New line|\\end|/last") == 0);
    CHECK_EQ(fa_stream_get_midi_info(s, &info), FA_OK);
    CHECK_EQ(info.lyric_count, 7);
    for (i = 0; i < info.text_count; i++) {
        CHECK_EQ(fa_stream_get_midi_text(s, i, &x), FA_OK);
        if (strcmp(x.text, "world") == 0) CHECK_EQ(x.frame, RATE / 4);
        if (strcmp(x.text, "next ") == 0) CHECK_EQ(x.frame, RATE / 4);
        if (x.text[0] == '%' || x.text[0] == '<') { CHECK_EQ(x.type, FA_MIDI_TEXT); chords++; }
    }
    CHECK_EQ(chords, 2);
    fa_stream_close(s);
    free(f.d);
    memset(&f, 0, sizeof(f));

    /* Outside karaoke files a "/" inside a lyric is text ("AC/DC"). */
    mthd(&f, 0, 1, 480);
    meta(&t, 0, 0x05, "/AC/DC");
    eot(&t, 0);
    mtrk(&f, &t);
    s = open_bytes(&f, FA_OPEN_DECODE_ONLY, NULL, NULL);
    CHECK(s);
    lyric_string(s, got, sizeof(got));
    CHECK(strcmp(got, "/AC/DC") == 0);
    fa_stream_close(s);
    free(f.d); free(t.d);
}

/* Words in text events without karaoke headers or a "Words" track: the track with
 * plenty of syllable-like text events, even when a few stray lyric events exist;
 * too few is not lyrics. */
static void test_text_fallback(void)
{
    buf f = { 0 }, t = { 0 };
    fa_stream* s;
    fa_midi_info info;
    fa_midi_text x;
    int pass, k;
    for (pass = 0; pass < 2; pass++) {
        int syllables = pass == 0 ? 20 : 10;
        mthd(&f, 1, 2, 480);
        meta(&t, 0, 0x01, "Sequenced by somebody with a long credit line here");
        meta(&t, 0, 0x05, "stray");
        eot(&t, 0);
        mtrk(&f, &t);
        for (k = 0; k < syllables; k++) meta(&t, k ? 120 : 0, 0x01, k % 2 ? "la " : "la");
        eot(&t, 0);
        mtrk(&f, &t);
        s = open_bytes(&f, FA_OPEN_DECODE_ONLY, NULL, NULL);
        CHECK(s);
        CHECK_EQ(fa_stream_get_midi_info(s, &info), FA_OK);
        if (pass == 0) {
            CHECK_EQ(info.lyric_count, 20);
            CHECK_EQ(info.karaoke, 1);
            CHECK_EQ(fa_stream_get_midi_text(s, 1, &x), FA_OK);   /* the stray lyric event */
            CHECK(strcmp(x.text, "stray") == 0);
            CHECK_EQ(x.type, FA_MIDI_TEXT);
        } else {
            CHECK_EQ(info.lyric_count, 1);                       /* only the lyric event */
            CHECK_EQ(info.karaoke, 0);
        }
        fa_stream_close(s);
        free(f.d);
        memset(&f, 0, sizeof(f));
    }
    free(t.d);
}

/* "@T" and "@I" lines and the file's own events as song credits. */
static void test_credits(void)
{
    buf f = { 0 }, t = { 0 };
    fa_stream* s;
    fa_midi_credits c;
    mthd(&f, 1, 2, 480);
    tempo(&t, 0, 500000);
    varlen(&t, 0); put8(&t, 0xFF); put8(&t, 0x58); put8(&t, 4); put8(&t, 3); put8(&t, 2); put8(&t, 24); put8(&t, 8);
    varlen(&t, 0); put8(&t, 0xFF); put8(&t, 0x59); put8(&t, 2); put8(&t, 1); put8(&t, 1);   /* E minor */
    meta(&t, 0, 0x01, "@KMIDI KARAOKE FILE");
    eot(&t, 0);
    mtrk(&f, &t);
    meta(&t, 0, 0x03, "Words");
    meta(&t, 0, 0x01, "@TThe Title");
    meta(&t, 0, 0x01, "@TThe Artist");
    meta(&t, 0, 0x01, "@T(c) 1999 Somebody Music");
    meta(&t, 0, 0x01, "@IWritten by: Ann Writer");
    meta(&t, 0, 0x01, "@ILyrics by - Bo Lyric");
    meta(&t, 0, 0x01, "@I1987");
    meta(&t, 0, 0x01, "@IKey: G");               /* the key signature event wins */
    meta(&t, 0, 0x01, "@IKeyboards: Cy");        /* not a key */
    meta(&t, 0, 0x01, "@IArranged for karaoke");
    meta(&t, 0, 0x01, "@Iarranged FOR KARAOKE");  /* a repeat */
    meta(&t, 0, 0x01, "@IWritten");               /* a label with nothing after it */
    meta(&t, 0, 0x01, "la");
    eot(&t, 0);
    mtrk(&f, &t);
    s = open_bytes(&f, FA_OPEN_DECODE_ONLY, NULL, NULL);
    CHECK(s);
    memset(&c, 0, sizeof(c));
    c.struct_size = sizeof(c);
    CHECK_EQ(fa_stream_get_midi_credits(s, &c), FA_OK);
    CHECK(strcmp(c.title, "The Title") == 0);
    CHECK(strcmp(c.artist, "The Artist") == 0);
    CHECK(strcmp(c.copyright, "(c) 1999 Somebody Music") == 0);
    CHECK(strcmp(c.writer, "Ann Writer") == 0);
    CHECK(strcmp(c.lyricist, "Bo Lyric") == 0);
    CHECK(strcmp(c.year, "1987") == 0);
    CHECK(strcmp(c.key, "E minor") == 0);
    CHECK(strcmp(c.tempo, "120 bpm") == 0);
    CHECK(strcmp(c.time_signature, "3/4") == 0);
    CHECK(strcmp(c.other, "Keyboards: Cy\nArranged for karaoke\nWritten") == 0);
    /* An older, shorter struct is filled up to its size. */
    memset(&c, 0x55, sizeof(c));
    c.struct_size = (uint32_t)offsetof(fa_midi_credits, artist);
    CHECK_EQ(fa_stream_get_midi_credits(s, &c), FA_OK);
    CHECK(strcmp(c.title, "The Title") == 0);
    CHECK_EQ((uint8_t)c.artist[0], 0x55);
    c.struct_size = 0;
    CHECK_EQ(fa_stream_get_midi_credits(s, &c), FA_ERROR_INVALID_ARG);
    fa_stream_close(s);
    free(f.d);
    memset(&f, 0, sizeof(f));

    /* One credit for both, and "Lyrics and ..." is not a lyricist label. */
    mthd(&f, 0, 1, 480);
    meta(&t, 0, 0x01, "@IWords and Music by Joe Both");
    meta(&t, 0, 0x01, "@ILyrics and arrangement: Kim");
    eot(&t, 0);
    mtrk(&f, &t);
    s = open_bytes(&f, FA_OPEN_DECODE_ONLY, NULL, NULL);
    CHECK(s);
    memset(&c, 0, sizeof(c));
    c.struct_size = sizeof(c);
    CHECK_EQ(fa_stream_get_midi_credits(s, &c), FA_OK);
    CHECK(strcmp(c.writer, "Joe Both") == 0);
    CHECK(strcmp(c.lyricist, "Joe Both") == 0);
    CHECK(strcmp(c.other, "Lyrics and arrangement: Kim") == 0);
    fa_stream_close(s);
    free(f.d); free(t.d);
}

/* Reads up to `frames` stereo frames in reads of `chunk`; returns how many. */
static fa_frames read_all(fa_stream* s, float* out, fa_frames frames, fa_frames chunk)
{
    fa_frames done = 0;
    while (done < frames) {
        fa_frames got = 0, want = frames - done < chunk ? frames - done : chunk;
        fa_result r = fa_stream_read(s, out + done * 2, want, &got);
        if (r != FA_OK || got == 0) break;
        done += got;
    }
    return done;
}

static double rms(const float* x, fa_frames from, fa_frames to)
{
    double acc = 0;
    fa_frames i;
    for (i = from; i < to; i++) acc += (double)x[i * 2] * x[i * 2] + (double)x[i * 2 + 1] * x[i * 2 + 1];
    return to > from ? sqrt(acc / (double)(2 * (to - from))) : 0.0;
}

/* Rising zero crossings of the left channel per second. */
static double frequency(const float* x, fa_frames from, fa_frames to)
{
    fa_frames i;
    int n = 0;
    for (i = from + 1; i < to; i++) if (x[(i - 1) * 2] < 0 && x[i * 2] >= 0) n++;
    return (double)n * RATE / (double)(to - from);
}

/* ---- tests ------------------------------------------------------------------------ */

static void test_detect_and_length(void)
{
    buf song = make_song();
    fa_stream* s = open_bytes(&song, FA_OPEN_DECODE_ONLY, NULL, NULL);
    fa_stream_info info;
    uint32_t i;
    int listed = 0;
    CHECK(s);
    fa_stream_get_info(s, &info);
    CHECK(strcmp(info.codec, "midi") == 0);
    CHECK(strcmp(info.container, "smf") == 0);
    CHECK_EQ(info.format.sample_rate, RATE);
    CHECK_EQ(info.format.channels, 2);
    CHECK(info.seekable);
    /* The last event is at 2 s; one second of tail after it. */
    CHECK_EQ(info.length_frames, 2 * RATE + RATE);
    fa_stream_close(s);
    for (i = 0; i < fa_format_count(); i++) {
        fa_format_info f;
        fa_format_get_info(i, &f);
        if (strcmp(f.name, "midi") == 0 && strstr(f.extensions, "kar")) listed = 1;
    }
    CHECK(listed);
    free(song.d);
}

static void test_rmid(void)
{
    buf song = make_song(), riff = { 0 }, rmid = { 0 };
    fa_stream* s;
    fa_stream_info info;
    put(&riff, "RMID", 4);
    chunk(&riff, "data", &song);
    chunk(&rmid, "RIFF", &riff);
    s = open_bytes(&rmid, FA_OPEN_DECODE_ONLY, NULL, NULL);
    CHECK(s);
    fa_stream_get_info(s, &info);
    CHECK(strcmp(info.container, "rmid") == 0);
    CHECK_EQ(info.length_frames, 3 * RATE);
    fa_stream_close(s);
    free(song.d); free(riff.d); free(rmid.d);
}

static void test_no_soundfont(void)
{
    buf song = make_song();
    fa_result r = FA_OK;
    fa_stream* s;
    fa_engine_set_soundfont(g_engine, NULL);
    /* As on a machine without an OS bank (white box): see test_system_soundfont. */
    g_engine->system_soundfont_tried = 1;
    s = open_bytes(&song, FA_OPEN_DECODE_ONLY, NULL, &r);
    CHECK(!s);
    CHECK_EQ(r, FA_ERROR_NO_SOUNDFONT);
    CHECK(strcmp(fa_result_string(r), "unknown result") != 0);
    /* A stream's own soundfont needs no engine default. */
    s = open_bytes(&song, FA_OPEN_DECODE_ONLY, g_font, &r);
    CHECK(s);
    fa_stream_close(s);
    fa_engine_set_soundfont(g_engine, g_font);
    free(song.d);
}

static void test_render_and_seek_exact(void)
{
    buf song = make_song();
    fa_stream* s = open_bytes(&song, FA_OPEN_DECODE_ONLY, NULL, NULL);
    fa_frames len = 3 * RATE, got, k;
    float* all = (float*)calloc((size_t)len * 2, sizeof(float));
    float* other = (float*)calloc((size_t)len * 2, sizeof(float));
    /* Seek targets where no note is held (a held note is not retriggered by a
     * seek, by design): in the gap, on a note-on, in the tail. */
    static const fa_frames targets[] = { RATE, RATE + 17, 0, 66150, 30001, 2 * RATE + RATE / 2 + 3 };
    size_t t;
    CHECK(s);
    /* Dry: a seek clears the reverb tail (test_effects seeks with effects on). */
    fa_stream_set_midi_effects(s, 0);
    got = read_all(s, all, len, 1000);
    CHECK_EQ(got, len);
    /* Sound where the notes are, silence in the gap and the tail. */
    CHECK(rms(all, 1000, 20000) > 0.1);
    CHECK(rms(all, 30000, 65000) < 1e-6);
    CHECK(rms(all, 67000, 85000) > 0.05);
    CHECK(rms(all, 2 * RATE + 8000, len) < 1e-6);
    CHECK_NEAR(frequency(all, 1000, 20000), 440.0, 3.0);
    for (k = 0; k < len * 2; k++) CHECK(all[k] == all[k] && fabsf(all[k]) <= 1.0f);
    /* The read size changes nothing. */
    CHECK_EQ(fa_stream_seek(s, 0), FA_OK);
    got = read_all(s, other, len, 4093);
    CHECK_EQ(got, len);
    CHECK(memcmp(all, other, (size_t)len * 2 * sizeof(float)) == 0);
    /* A seek lands on the exact frame: what follows equals reading through. */
    for (t = 0; t < sizeof(targets) / sizeof(targets[0]); t++) {
        fa_frames at = targets[t], n = len - at;
        CHECK_EQ(fa_stream_seek(s, at), FA_OK);
        memset(other, 0, (size_t)len * 2 * sizeof(float));
        got = read_all(s, other, n, 777);
        CHECK_EQ(got, n);
        if (memcmp(all + at * 2, other, (size_t)n * 2 * sizeof(float)) != 0) {
            fprintf(stderr, "  seek to %lld differs\n", (long long)at);
            CHECK(0);
        }
    }
    /* At the end: nothing more. */
    CHECK_EQ(fa_stream_seek(s, len), FA_OK);
    {
        float x[8];
        fa_frames g = 5;
        CHECK_EQ(fa_stream_read(s, x, 4, &g), FA_ERROR_END_OF_STREAM);
        CHECK_EQ(g, 0);
    }
    fa_stream_close(s);
    free(all); free(other); free(song.d);
}

static void test_texts(void)
{
    buf song = make_song();
    fa_stream* s = open_bytes(&song, FA_OPEN_DECODE_ONLY, NULL, NULL);
    fa_midi_info info;
    fa_midi_text t;
    uint32_t i, lyrics = 0;
    CHECK(s);
    CHECK_EQ(fa_stream_get_midi_info(s, &info), FA_OK);
    CHECK(strcmp(info.title, "Test Song") == 0);
    CHECK(strcmp(info.copyright, "(c) Nobody") == 0);
    CHECK_EQ(info.format, 0);
    CHECK_EQ(info.tracks, 1);
    CHECK_EQ(info.karaoke, 0);
    CHECK_EQ(info.lyric_count, 3);
    CHECK_EQ(info.text_count, 5);
    for (i = 0; i < info.text_count; i++) {
        CHECK_EQ(fa_stream_get_midi_text(s, i, &t), FA_OK);
        if (t.type != FA_MIDI_LYRIC) continue;
        if (lyrics == 0) { CHECK(strcmp(t.text, "Hel") == 0); CHECK_EQ(t.frame, 0); CHECK_EQ(t.flags, 0); }
        if (lyrics == 1) { CHECK(strcmp(t.text, "lo ") == 0); CHECK_EQ(t.frame, RATE / 4); CHECK_EQ(t.time, 250000000); }
        if (lyrics == 2) { CHECK(strcmp(t.text, "world") == 0); CHECK_EQ(t.frame, 66150); CHECK_EQ(t.flags, FA_MIDI_NEW_LINE); }
        lyrics++;
    }
    CHECK_EQ(lyrics, 3);
    CHECK_EQ(fa_stream_get_midi_text(s, info.text_count, &t), FA_ERROR_NOT_FOUND);
    fa_stream_close(s);
    free(song.d);
}

/* A .kar file: a header track with "@" lines and a "Words" track whose text
 * events are the lyrics; a format 1 tempo map. */
static void test_karaoke(void)
{
    buf f = { 0 }, t = { 0 };
    fa_stream* s;
    fa_midi_info info;
    fa_midi_text x;
    uint32_t i, n = 0;
    const char* want[] = { "Hel", "lo", "Caf\xc3\xa9", "again" };
    const uint32_t want_flags[] = { FA_MIDI_NEW_LINE | FA_MIDI_NEW_PARAGRAPH, 0, FA_MIDI_NEW_LINE, FA_MIDI_NEW_LINE | FA_MIDI_NEW_PARAGRAPH };
    const fa_frames want_frame[] = { 0, RATE / 2, RATE, 2 * RATE };
    mthd(&f, 1, 3, 480);
    /* Track 0: 120 bpm, then 60 bpm from beat 2 (1 s). */
    tempo(&t, 0, 500000);
    meta(&t, 0, 0x01, "@KMIDI KARAOKE FILE");
    tempo(&t, 960, 1000000);
    eot(&t, 0);
    mtrk(&f, &t);
    meta(&t, 0, 0x03, "Words");
    meta(&t, 0, 0x01, "@TMy Song");
    meta(&t, 0, 0x01, "@TThe Singer");
    meta(&t, 0, 0x01, "\\Hel");
    meta(&t, 480, 0x01, "lo");
    meta(&t, 480, 0x01, "/Caf\xe9");    /* Latin-1 */
    meta(&t, 480, 0x01, "\\again");     /* 1 s + 480 ticks at 60 bpm = 2 s */
    eot(&t, 0);
    mtrk(&f, &t);
    ev3(&t, 0, 0x90, 69, 100);
    ev3(&t, 1440, 0x80, 69, 0);
    eot(&t, 0);
    mtrk(&f, &t);
    s = open_bytes(&f, FA_OPEN_DECODE_ONLY, NULL, NULL);
    CHECK(s);
    CHECK_EQ(fa_stream_get_midi_info(s, &info), FA_OK);
    CHECK_EQ(info.karaoke, 1);
    CHECK_EQ(info.format, 1);
    CHECK_EQ(info.tracks, 3);
    CHECK(strcmp(info.title, "My Song") == 0);
    CHECK(strcmp(info.artist, "The Singer") == 0);
    CHECK_EQ(info.lyric_count, 4);
    {
        /* The same through the tag reader. */
        fa_tags* tags = NULL;
        CHECK_EQ(fa_stream_get_tags(s, FA_TAGS_NO_PICTURES, &tags), FA_OK);
        CHECK(strcmp(fa_tags_get(tags, FA_TAG_TITLE), "My Song") == 0);
        CHECK(strcmp(fa_tags_get(tags, FA_TAG_ARTIST), "The Singer") == 0);
        fa_tags_close(tags);
    }
    for (i = 0; i < info.text_count; i++) {
        fa_stream_get_midi_text(s, i, &x);
        if (x.type != FA_MIDI_LYRIC) { CHECK(x.text[0] == '@' || x.type == FA_MIDI_TRACK_NAME); continue; }
        CHECK(n < 4);
        CHECK(strcmp(x.text, want[n]) == 0);
        CHECK_EQ(x.flags, want_flags[n]);
        CHECK_EQ(x.frame, want_frame[n]);
        CHECK_EQ(x.track, 1);
        n++;
    }
    CHECK_EQ(n, 4);
    fa_stream_close(s);
    free(f.d); free(t.d);
}

static void test_transpose_and_drums(void)
{
    buf melody = make_note(0), drums = make_note(9);
    float* x = (float*)calloc((size_t)RATE * 2, sizeof(float));
    fa_stream* s = open_bytes(&melody, FA_OPEN_DECODE_ONLY, NULL, NULL);
    int32_t v = 0;
    CHECK(s);
    CHECK_EQ(fa_stream_set_midi_transpose(s, 12), FA_OK);
    CHECK_EQ(fa_stream_get_midi_transpose(s, &v), FA_OK);
    CHECK_EQ(v, 12);
    CHECK_EQ(fa_stream_set_midi_transpose(s, 49), FA_ERROR_INVALID_ARG);
    CHECK_EQ(read_all(s, x, RATE, 512), RATE);
    CHECK_NEAR(frequency(x, 2000, RATE - 2000), 880.0, 5.0);
    /* Back to 0 and from the start: the original pitch. */
    fa_stream_set_midi_transpose(s, -12);
    fa_stream_seek(s, 0);
    CHECK_EQ(read_all(s, x, RATE, 512), RATE);
    CHECK_NEAR(frequency(x, 2000, RATE - 2000), 220.0, 3.0);
    fa_stream_close(s);
    /* Channel 10 is drums: never transposed. */
    s = open_bytes(&drums, FA_OPEN_DECODE_ONLY, NULL, NULL);
    CHECK(s);
    fa_stream_set_midi_transpose(s, 12);
    CHECK_EQ(read_all(s, x, RATE, 512), RATE);
    CHECK(rms(x, 2000, RATE - 2000) > 0.1);
    CHECK_NEAR(frequency(x, 2000, RATE - 2000), 440.0, 3.0);
    fa_stream_close(s);
    free(x); free(melody.d); free(drums.d);
}

static void test_channel_mute(void)
{
    buf melody = make_note(0);
    float* x = (float*)calloc((size_t)RATE * 2, sizeof(float));
    fa_stream* s = open_bytes(&melody, FA_OPEN_DECODE_ONLY, NULL, NULL);
    int32_t m = 0;
    CHECK(s);
    fa_stream_set_midi_effects(s, 0);   /* the reverb of what was heard rings on after a mute */
    CHECK_EQ(fa_stream_set_midi_channel_mute(s, 16, 1), FA_ERROR_INVALID_ARG);
    /* Muted while playing: silent within one block and the 10 ms release. */
    CHECK_EQ(read_all(s, x, RATE / 4, 512), RATE / 4);
    CHECK(rms(x, 1000, RATE / 4) > 0.1);
    CHECK_EQ(fa_stream_set_midi_channel_mute(s, 0, 1), FA_OK);
    CHECK_EQ(fa_stream_get_midi_channel_mute(s, 0, &m), FA_OK);
    CHECK_EQ(m, 1);
    CHECK_EQ(fa_stream_get_midi_channel_mute(s, 1, &m), FA_OK);
    CHECK_EQ(m, 0);
    CHECK_EQ(read_all(s, x, RATE / 4, 512), RATE / 4);
    CHECK(rms(x, RATE / 50, RATE / 4) < 1e-6);
    /* Unmuted: the held note does not come back, the next one plays. */
    fa_stream_set_midi_channel_mute(s, 0, 0);
    fa_stream_seek(s, 0);
    CHECK_EQ(read_all(s, x, RATE / 4, 512), RATE / 4);
    CHECK(rms(x, 1000, RATE / 4) > 0.1);
    fa_stream_close(s);
    free(x); free(melody.d);
}

/* ---- SysEx, held notes, tempo and signature marks ------------------------------ */

static void sysex(buf* t, uint32_t delta, const uint8_t* d, size_t n)
{
    varlen(t, delta); put8(t, 0xF0); varlen(t, (uint32_t)n); put(t, d, n);
}

/* A file of `pre` SysEx messages (all at 0), then a note on `ch` from 0 to 1 s, a
 * bank select `bank` (>= 0) and program 0 before it. */
static buf make_sysex_note(const uint8_t* const* msgs, const size_t* lens, int count, int ch, int bank)
{
    buf f = { 0 }, t = { 0 };
    int i;
    mthd(&f, 0, 1, 480);
    for (i = 0; i < count; i++) sysex(&t, 0, msgs[i], lens[i]);
    if (bank >= 0) ev3(&t, 0, 0xB0 | ch, 0, (unsigned)bank);
    ev3(&t, 0, 0xC0 | ch, 0, 0);
    t.n -= 1;
    ev3(&t, 0, 0x90 | ch, 69, 127);
    ev3(&t, 960, 0x80 | ch, 69, 0);
    eot(&t, 0);
    mtrk(&f, &t);
    free(t.d);
    return f;
}

/* The pitch heard in the first second (the test kit is an octave up: 880 Hz). */
static double note_frequency(fa_soundfont* font, const uint8_t* const* msgs, const size_t* lens, int count, int ch, int bank,
                             double* out_rms)
{
    buf f = make_sysex_note(msgs, lens, count, ch, bank);
    float* x = (float*)calloc((size_t)RATE * 2, sizeof(float));
    fa_stream* s = open_bytes(&f, FA_OPEN_DECODE_ONLY, font, NULL);
    double hz = -1;
    if (s && read_all(s, x, RATE, 512) == RATE) {
        hz = frequency(x, 2000, RATE - 2000);
        if (out_rms) *out_rms = rms(x, 2000, RATE - 2000);
    }
    fa_stream_close(s);
    free(x); free(f.d);
    return hz;
}

static void test_sysex(void)
{
    static const uint8_t gm_on[] = { 0x7E, 0x7F, 0x09, 0x01, 0xF7 };
    static const uint8_t gm2_on[] = { 0x7E, 0x7F, 0x09, 0x03, 0xF7 };
    static const uint8_t gs_reset[] = { 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7 };
    static const uint8_t gs_part2_drums[] = { 0x41, 0x10, 0x42, 0x12, 0x40, 0x12, 0x15, 0x01, 0x18, 0xF7 };   /* part 2 = channel 1 */
    static const uint8_t gs_part10_normal[] = { 0x41, 0x10, 0x42, 0x12, 0x40, 0x10, 0x15, 0x00, 0x1B, 0xF7 };
    static const uint8_t xg_on[] = { 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7 };
    static const uint8_t xg_part3_drums[] = { 0x43, 0x10, 0x4C, 0x08, 0x02, 0x07, 0x01, 0xF7 };
    static const uint8_t xg_part10_normal[] = { 0x43, 0x10, 0x4C, 0x08, 0x09, 0x07, 0x00, 0xF7 };
    static const uint8_t master_half[] = { 0x7F, 0x7F, 0x04, 0x01, 0x7F, 0x3F, 0xF7 };   /* 8191 / 16383 */
    static const uint8_t gs_master_half[] = { 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x04, 0x40, 0x7C, 0xF7 };
    static const uint8_t coarse_up[] = { 0x7F, 0x7F, 0x04, 0x04, 0x00, 0x4C, 0xF7 };       /* +12 semitones */
    static const uint8_t fine_down[] = { 0x7F, 0x7F, 0x04, 0x03, 0x00, 0x20, 0xF7 };       /* 4096: -0.5 semitone */
    buf sf = make_sf2_ex(0, 12);
    fa_soundfont* font = NULL;
    const uint8_t* m[2];
    size_t n[2];
    double full = 0, half = 0;
    CHECK_EQ(fa_soundfont_open_memory(sf.d, sf.n, &font), FA_OK);
    /* Without SysEx: channel 10 is the kit, the others melodic. */
    CHECK_NEAR(note_frequency(font, NULL, NULL, 0, 9, -1, &full), 880.0, 5.0);
    CHECK_NEAR(note_frequency(font, NULL, NULL, 0, 1, -1, NULL), 440.0, 3.0);
    /* GS: part 2 made a rhythm part, part 10 a normal one. */
    m[0] = gs_reset; n[0] = sizeof(gs_reset); m[1] = gs_part2_drums; n[1] = sizeof(gs_part2_drums);
    CHECK_NEAR(note_frequency(font, m, n, 2, 1, -1, NULL), 880.0, 5.0);
    m[1] = gs_part10_normal; n[1] = sizeof(gs_part10_normal);
    CHECK_NEAR(note_frequency(font, m, n, 2, 9, -1, NULL), 440.0, 3.0);
    /* A GM reset undoes it. */
    m[0] = gs_part10_normal; n[0] = sizeof(gs_part10_normal); m[1] = gm_on; n[1] = sizeof(gm_on);
    CHECK_NEAR(note_frequency(font, m, n, 2, 9, -1, NULL), 880.0, 5.0);
    /* XG: bank 127 is drums on any channel, bank 0 melodic on channel 10; part mode. */
    m[0] = xg_on; n[0] = sizeof(xg_on);
    CHECK_NEAR(note_frequency(font, m, n, 1, 0, 127, NULL), 880.0, 5.0);
    CHECK_NEAR(note_frequency(font, m, n, 1, 9, 0, NULL), 440.0, 3.0);
    CHECK_NEAR(note_frequency(font, m, n, 1, 9, -1, NULL), 880.0, 5.0);
    m[1] = xg_part3_drums; n[1] = sizeof(xg_part3_drums);
    CHECK_NEAR(note_frequency(font, m, n, 2, 2, -1, NULL), 880.0, 5.0);
    m[1] = xg_part10_normal; n[1] = sizeof(xg_part10_normal);
    CHECK_NEAR(note_frequency(font, m, n, 2, 9, -1, NULL), 440.0, 3.0);
    /* Without an XG reset, bank 127 is a melodic bank (GS: the MT-32 set). */
    CHECK_NEAR(note_frequency(font, NULL, NULL, 0, 0, 127, NULL), 440.0, 3.0);
    /* GM2: bank 120 is drums, channel 10 starts on it. */
    m[0] = gm2_on; n[0] = sizeof(gm2_on);
    CHECK_NEAR(note_frequency(font, m, n, 1, 3, 120, NULL), 880.0, 5.0);
    CHECK_NEAR(note_frequency(font, m, n, 1, 9, -1, NULL), 880.0, 5.0);
    CHECK_NEAR(note_frequency(font, m, n, 1, 3, -1, NULL), 440.0, 3.0);
    /* Master volume halves the level (universal and GS); master tuning. */
    m[0] = master_half; n[0] = sizeof(master_half);
    CHECK_NEAR(note_frequency(font, m, n, 1, 9, -1, &half), 880.0, 5.0);
    CHECK_NEAR(half / full, 0.5, 0.01);
    m[0] = gs_master_half; n[0] = sizeof(gs_master_half);
    CHECK_NEAR(note_frequency(font, m, n, 1, 9, -1, &half), 880.0, 5.0);
    CHECK_NEAR(half / full, 64.0 / 127.0, 0.01);
    m[0] = coarse_up; n[0] = sizeof(coarse_up);
    CHECK_NEAR(note_frequency(font, m, n, 1, 0, -1, NULL), 880.0, 5.0);
    m[1] = fine_down; n[1] = sizeof(fine_down);
    CHECK_NEAR(note_frequency(font, m, n, 2, 0, -1, NULL), 880.0 * pow(2.0, -0.5 / 12.0), 5.0);
    fa_soundfont_close(font);
    free(sf.d);
}

/* Notes held across a seek target start again: by a key, by the pedal; drums don't.
 * A master volume set before the target is kept. */
static void test_seek_restarts_held_notes(void)
{
    static const uint8_t master_half[] = { 0x7F, 0x7F, 0x04, 0x01, 0x7F, 0x3F, 0xF7 };
    buf melody = make_note(0), drums = make_note(9), pedal = { 0 }, twice = { 0 }, t = { 0 }, vol;
    float* x = (float*)calloc((size_t)RATE * 2, sizeof(float));
    fa_stream* s;
    double level;
    /* Pedal down at 0, note 0..0.25 s, pedal up at 1.5 s. */
    mthd(&pedal, 0, 1, 480);
    ev3(&t, 0, 0xB0, 64, 127);
    ev3(&t, 0, 0x90, 69, 127);
    ev3(&t, 240, 0x80, 69, 0);
    ev3(&t, 1200, 0xB0, 64, 0);
    eot(&t, 0);
    mtrk(&pedal, &t);
    /* The same key struck twice (0 and 0.5 s), ended at 1 s and 2 s. */
    mthd(&twice, 0, 1, 480);
    ev3(&t, 0, 0x90, 69, 127);
    ev3(&t, 480, 0x90, 69, 127);
    ev3(&t, 480, 0x80, 69, 0);
    ev3(&t, 960, 0x80, 69, 0);
    eot(&t, 0);
    mtrk(&twice, &t);
    {
        const uint8_t* m[1] = { master_half };
        size_t n[1] = { sizeof(master_half) };
        vol = make_sysex_note(m, n, 1, 0, -1);
    }

    /* Effects off: with them on, a seek renders the time before the target
     * (test_seek_keeps_tails). */
    s = open_bytes(&melody, FA_OPEN_DECODE_ONLY, NULL, NULL);
    CHECK(s);
    fa_stream_set_midi_effects(s, 0);
    CHECK_EQ(read_all(s, x, RATE / 2, 512), RATE / 2);
    level = rms(x, 2000, RATE / 2);
    CHECK_EQ(fa_stream_seek(s, RATE / 2 + 7), FA_OK);
    CHECK_EQ(read_all(s, x, RATE / 4, 512), RATE / 4);
    CHECK(rms(x, 1000, RATE / 4) > level * 0.8);
    CHECK_NEAR(frequency(x, 1000, RATE / 4), 440.0, 5.0);
    /* Transposed now: the restarted note takes the transpose. */
    CHECK_EQ(fa_stream_set_midi_transpose(s, 12), FA_OK);
    CHECK_EQ(fa_stream_seek(s, RATE / 2), FA_OK);
    CHECK_EQ(read_all(s, x, RATE / 4, 512), RATE / 4);
    CHECK_NEAR(frequency(x, 1000, RATE / 4), 880.0, 5.0);
    /* After its note-off: nothing restarts. */
    fa_stream_set_midi_transpose(s, 0);
    CHECK_EQ(fa_stream_seek(s, RATE + RATE / 2), FA_OK);
    CHECK_EQ(read_all(s, x, RATE / 4, 512), RATE / 4);
    CHECK(rms(x, 0, RATE / 4) < 1e-6);
    fa_stream_close(s);

    s = open_bytes(&pedal, FA_OPEN_DECODE_ONLY, NULL, NULL);
    CHECK(s);
    fa_stream_set_midi_effects(s, 0);
    CHECK_EQ(fa_stream_seek(s, RATE), FA_OK);
    CHECK_EQ(read_all(s, x, RATE / 4, 512), RATE / 4);
    CHECK(rms(x, 1000, RATE / 4) > level * 0.8);   /* the pedal holds it */
    CHECK_EQ(fa_stream_seek(s, 2 * RATE), FA_OK);
    CHECK_EQ(read_all(s, x, RATE / 4, 512), RATE / 4);
    CHECK(rms(x, 0, RATE / 4) < 1e-6);             /* released by then */
    fa_stream_close(s);

    /* Both strikes are held at 0.8 s: the first note-off ends one, the other rings on. */
    s = open_bytes(&twice, FA_OPEN_DECODE_ONLY, NULL, NULL);
    CHECK(s);
    fa_stream_set_midi_effects(s, 0);
    CHECK_EQ(fa_stream_seek(s, RATE * 8 / 10), FA_OK);
    CHECK_EQ(read_all(s, x, RATE, 512), RATE);
    CHECK(rms(x, RATE / 2, RATE * 9 / 10) > level * 0.5);
    fa_stream_close(s);

    s = open_bytes(&drums, FA_OPEN_DECODE_ONLY, NULL, NULL);
    CHECK(s);
    fa_stream_set_midi_effects(s, 0);
    CHECK_EQ(fa_stream_seek(s, RATE / 2), FA_OK);
    CHECK_EQ(read_all(s, x, RATE / 4, 512), RATE / 4);
    CHECK(rms(x, 0, RATE / 4) < 1e-6);
    fa_stream_close(s);

    s = open_bytes(&vol, FA_OPEN_DECODE_ONLY, NULL, NULL);
    CHECK(s);
    fa_stream_set_midi_effects(s, 0);
    CHECK_EQ(fa_stream_seek(s, RATE / 2), FA_OK);
    CHECK_EQ(read_all(s, x, RATE / 4, 512), RATE / 4);
    CHECK_NEAR(rms(x, 1000, RATE / 4) / level, 0.5, 0.02);
    fa_stream_close(s);
    free(x); free(melody.d); free(drums.d); free(pedal.d); free(twice.d); free(t.d); free(vol.d);
}

static void test_tempo_and_signature_marks(void)
{
    buf f = { 0 }, t = { 0 };
    fa_stream* s;
    fa_midi_info info;
    fa_midi_mark x[8];
    uint32_t n = 0;
    mthd(&f, 0, 1, 480);
    tempo(&t, 0, 500000);
    varlen(&t, 0); put8(&t, 0xFF); put8(&t, 0x58); put8(&t, 4); put8(&t, 3); put8(&t, 2); put8(&t, 24); put8(&t, 8);
    varlen(&t, 0); put8(&t, 0xFF); put8(&t, 0x59); put8(&t, 2); put8(&t, 0xFF); put8(&t, 1);   /* 1 flat, minor */
    ev3(&t, 0, 0x90, 69, 100);
    tempo(&t, 960, 416667);                    /* at 1 s: 144 bpm */
    varlen(&t, 0); put8(&t, 0xFF); put8(&t, 0x59); put8(&t, 2); put8(&t, 2); put8(&t, 0);      /* D major */
    varlen(&t, 0); put8(&t, 0xFF); put8(&t, 0x59); put8(&t, 2); put8(&t, 9); put8(&t, 0);      /* malformed: skipped */
    ev3(&t, 480, 0x80, 69, 0);
    eot(&t, 0);
    mtrk(&f, &t);
    s = open_bytes(&f, FA_OPEN_DECODE_ONLY, NULL, NULL);
    CHECK(s);
    CHECK_EQ(fa_stream_get_midi_info(s, &info), FA_OK);
    CHECK_EQ(info.text_count, 0);                    /* marks are not texts */
    CHECK_EQ(fa_stream_get_midi_marks(s, NULL, 0, &n), FA_OK);
    CHECK_EQ(n, 5);
    CHECK_EQ(fa_stream_get_midi_marks(s, x, 2, &n), FA_OK);   /* fewer than there are */
    CHECK_EQ(n, 5);
    CHECK_EQ(fa_stream_get_midi_marks(s, x, 8, &n), FA_OK);
    CHECK_EQ(n, 5);
    CHECK_EQ(x[0].type, FA_MIDI_MARK_TEMPO); CHECK_EQ(x[0].value, 500000); CHECK(strcmp(x[0].text, "120 bpm") == 0);
    CHECK_EQ(x[1].type, FA_MIDI_MARK_TIME_SIGNATURE); CHECK_EQ(x[1].value, 3); CHECK_EQ(x[1].value2, 4);
    CHECK(strcmp(x[1].text, "3/4") == 0);
    CHECK_EQ(x[2].type, FA_MIDI_MARK_KEY_SIGNATURE); CHECK_EQ(x[2].value, -1); CHECK_EQ(x[2].value2, 1);
    CHECK(strcmp(x[2].text, "D minor") == 0);
    CHECK_EQ(x[3].type, FA_MIDI_MARK_TEMPO); CHECK_EQ(x[3].frame, RATE); CHECK_EQ(x[3].time, 1000000000);
    CHECK(strcmp(x[3].text, "144 bpm") == 0);
    CHECK_EQ(x[4].type, FA_MIDI_MARK_KEY_SIGNATURE); CHECK_EQ(x[4].value, 2); CHECK_EQ(x[4].value2, 0);
    CHECK(strcmp(x[4].text, "D major") == 0);
    CHECK_EQ(fa_stream_get_midi_marks(s, NULL, 1, &n), FA_ERROR_INVALID_ARG);
    fa_stream_close(s);
    free(f.d); free(t.d);
}

/* ---- SF3 ------------------------------------------------------------------------ */

static buf read_file(const char* path)
{
    buf b = { 0 };
    FILE* f = fopen(path, "rb");
    uint8_t tmp[4096];
    size_t n;
    if (!f) return b;
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) put(&b, tmp, n);
    fclose(f);
    return b;
}

/* An SF3 bank: preset 0 plays tests/data/ogg/tone.ogg (440 Hz, 1.5 s, Vorbis) at
 * root key 69, no loop; a second sample is plain PCM (the sine) for bank 128. */
static buf make_sf3(const buf* ogg)
{
    buf sf = { 0 }, riff = { 0 }, info = { 0 }, sdta = { 0 }, pdta = { 0 }, c = { 0 };
    uint32_t pcm_at;
    int i;
    put(&riff, "sfbk", 4);
    put(&info, "INFO", 4);
    c.n = 0; put16le(&c, 3); put16le(&c, 1); chunk(&info, "ifil", &c);
    c.n = 0; putstr(&c, "Test SF3", 10); chunk(&info, "INAM", &c);
    chunk(&riff, "LIST", &info);
    put(&sdta, "sdta", 4);
    c.n = 0;
    put(&c, ogg->d, ogg->n);
    if (c.n & 1) put8(&c, 0);
    pcm_at = (uint32_t)(c.n / 2);   /* the PCM sample, in samples */
    for (i = 0; i < SINE_LEN; i++) put16le(&c, (unsigned)(uint16_t)(int16_t)lrint(sin(2.0 * PI * 440.0 * i / RATE) * 32000.0));
    for (i = 0; i < 46; i++) put16le(&c, 0);
    chunk(&sdta, "smpl", &c);
    chunk(&riff, "LIST", &sdta);
    put(&pdta, "pdta", 4);
    c.n = 0;
    putstr(&c, "Tone", 20); put16le(&c, 0); put16le(&c, 0); put16le(&c, 0); put32le(&c, 0); put32le(&c, 0); put32le(&c, 0);
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
    putstr(&c, "ToneInst", 20); put16le(&c, 0);
    putstr(&c, "KitInst", 20); put16le(&c, 1);
    putstr(&c, "EOI", 20); put16le(&c, 2);
    chunk(&pdta, "inst", &c);
    c.n = 0; put16le(&c, 0); put16le(&c, 0); put16le(&c, 2); put16le(&c, 0); put16le(&c, 5); put16le(&c, 0);
    chunk(&pdta, "ibag", &c);
    c.n = 0; for (i = 0; i < 10; i++) put8(&c, 0);
    chunk(&pdta, "imod", &c);
    c.n = 0;
    gen(&c, 38, (unsigned)(uint16_t)-3600); gen(&c, 53, 0);                  /* tone: no loop */
    gen(&c, 54, 1); gen(&c, 38, (unsigned)(uint16_t)-3600); gen(&c, 53, 1);  /* kit: the looped PCM sine */
    gen(&c, 0, 0);
    chunk(&pdta, "igen", &c);
    c.n = 0;
    /* SF3: byte offsets of the Ogg stream, loops relative to it, type 1 | Vorbis. */
    putstr(&c, "Tone", 20); put32le(&c, 0); put32le(&c, (uint32_t)ogg->n); put32le(&c, 0); put32le(&c, 1000);
    put32le(&c, RATE); put8(&c, 69); put8(&c, 0); put16le(&c, 0); put16le(&c, 1 | 0x10);
    putstr(&c, "Sine", 20); put32le(&c, pcm_at); put32le(&c, pcm_at + SINE_LEN); put32le(&c, pcm_at); put32le(&c, pcm_at + SINE_LEN);
    put32le(&c, RATE); put8(&c, 69); put8(&c, 0); put16le(&c, 0); put16le(&c, 1);
    putstr(&c, "EOS", 20); for (i = 0; i < 26; i++) put8(&c, 0);
    chunk(&pdta, "shdr", &c);
    chunk(&riff, "LIST", &pdta);
    chunk(&sf, "RIFF", &riff);
    free(riff.d); free(info.d); free(sdta.d); free(pdta.d); free(c.d);
    return sf;
}

static void test_sf3(void)
{
    buf ogg = read_file(FA_TEST_DATA_DIR "/ogg/tone.ogg"), sf3, melody = make_note(0), drums = make_note(9);
    fa_soundfont* font = NULL;
    fa_soundfont_info fi;
    fa_stream* s;
    float* x = (float*)calloc((size_t)RATE * 2, sizeof(float));
    CHECK(ogg.n > 1000);
    sf3 = make_sf3(&ogg);
    CHECK_EQ(fa_soundfont_open_memory(sf3.d, sf3.n, &font), FA_OK);
    CHECK_EQ(fa_soundfont_get_info(font, &fi), FA_OK);
    CHECK(strcmp(fi.name, "Test SF3") == 0);
    CHECK_EQ(fi.preset_count, 2);
    /* 1.5 s of decoded tone plus the PCM sine, each padded, as floats. */
    CHECK(fi.sample_memory >= (uint64_t)(RATE * 3 / 2 + SINE_LEN) * sizeof(float));
    /* The Vorbis sample plays at its pitch. */
    s = open_bytes(&melody, FA_OPEN_DECODE_ONLY, font, NULL);
    CHECK(s);
    CHECK_EQ(read_all(s, x, RATE, 512), RATE);
    CHECK(rms(x, 2000, RATE - 2000) > 0.1);
    CHECK_NEAR(frequency(x, 2000, RATE - 2000), 440.0, 3.0);
    fa_stream_close(s);
    /* The PCM sample next to it still plays. */
    s = open_bytes(&drums, FA_OPEN_DECODE_ONLY, font, NULL);
    CHECK(s);
    CHECK_EQ(read_all(s, x, RATE, 512), RATE);
    CHECK(rms(x, 2000, RATE - 2000) > 0.1);
    CHECK_NEAR(frequency(x, 2000, RATE - 2000), 440.0, 3.0);
    fa_stream_close(s);
    fa_soundfont_close(font);
    /* A damaged Ogg stream: the bank still loads, that sample is silent. */
    {
        size_t k;
        for (k = 0; k + 4 < sf3.n; k++) if (memcmp(sf3.d + k, "OggS", 4) == 0) { memset(sf3.d + k, 0x55, ogg.n); break; }
    }
    font = NULL;
    CHECK_EQ(fa_soundfont_open_memory(sf3.d, sf3.n, &font), FA_OK);
    s = open_bytes(&melody, FA_OPEN_DECODE_ONLY, font, NULL);
    CHECK(s);
    CHECK_EQ(read_all(s, x, RATE, 512), RATE);
    CHECK(rms(x, 0, RATE) < 1e-6);
    fa_stream_close(s);
    fa_soundfont_close(font);
    /* Truncated anywhere: never a crash. */
    {
        size_t cut;
        for (cut = 12; cut < sf3.n; cut += sf3.n / 37 + 1) {
            font = NULL;
            if (fa_soundfont_open_memory(sf3.d, cut, &font) == FA_OK) fa_soundfont_close(font);
        }
    }
    /* A codec SF3 doesn't define (sampleType 0x20): not a bank we can play. */
    {
        size_t k;
        for (k = 0; k + 8 + 46 < sf3.n; k++)
            if (memcmp(sf3.d + k, "shdr", 4) == 0) { sf3.d[k + 8 + 44] = 0x21; break; }
        font = NULL;
        CHECK_EQ(fa_soundfont_open_memory(sf3.d, sf3.n, &font), FA_ERROR_UNSUPPORTED_FORMAT);
    }
    free(x); free(ogg.d); free(sf3.d); free(melody.d); free(drums.d);
}

/* Format 2: the tracks are patterns played one after another. Also: a .kar
 * header with lyric events is still karaoke, and invalid UTF-8 (an overlong
 * form) is read as Windows-1252. */
/* One note (key 69, channel 0) from 0 to 0.5 s with CC91 / CC93 set first (-1:
 * left at the default), and a second one at `second` seconds (0: none). */
static buf make_fx_song(int cc91, int cc93, int second)
{
    buf f = { 0 }, t = { 0 };
    mthd(&f, 0, 1, 480);
    if (cc91 >= 0) ev3(&t, 0, 0xB0, 91, (unsigned)cc91);
    if (cc93 >= 0) ev3(&t, 0, 0xB0, 93, (unsigned)cc93);
    ev3(&t, 0, 0x90, 69, 127);
    ev3(&t, 480, 0x80, 69, 0);
    if (second) {
        ev3(&t, (uint32_t)(second * 960 - 480), 0x90, 69, 127);
        ev3(&t, 480, 0x80, 69, 0);
    }
    eot(&t, 0);
    mtrk(&f, &t);
    free(t.d);
    return f;
}

/* Renders `len` frames of `song` with effects on or off. */
static fa_frames render_fx(const buf* song, fa_soundfont* font, int effects, float* out, fa_frames len, fa_frames chunk)
{
    fa_stream* s = open_bytes(song, FA_OPEN_DECODE_ONLY, font, NULL);
    fa_frames got;
    int32_t on = -1;
    if (!s) return 0;
    /* On by default. */
    if (fa_stream_get_midi_effects(s, &on) != FA_OK || on != 1) { fa_stream_close(s); return 0; }
    if (!effects) fa_stream_set_midi_effects(s, 0);
    got = read_all(s, out, len, chunk);
    fa_stream_close(s);
    return got;
}

/* Reverb and chorus follow CC91 / CC93 and the soundfont's sends, are the same
 * whatever the read size, and a seek past the tail equals reading through. */
static void test_effects(void)
{
    const fa_frames len = (fa_frames)(1.5 * RATE), note_end = RATE / 2, dry_end = RATE / 2 + RATE / 4;
    size_t bytes = (size_t)len * 2 * sizeof(float);
    float* dry = (float*)calloc((size_t)len * 2, sizeof(float));
    float* wet = (float*)calloc((size_t)len * 2, sizeof(float));
    float* other = (float*)calloc((size_t)len * 2, sizeof(float));
    buf def = make_fx_song(-1, -1, 0), none = make_fx_song(0, 0, 0), chorus = make_fx_song(0, 127, 0);
    buf loud = make_fx_song(127, -1, 0);
    fa_soundfont* sendfont = NULL;
    fa_stream* s;
    fa_result r;
    int32_t on;

    /* Off: the dry synth, silent once the note's release is over. */
    CHECK_EQ(render_fx(&def, g_font, 0, dry, len, 1000), len);
    CHECK(rms(dry, 1000, note_end) > 0.1);
    CHECK(rms(dry, dry_end, len) == 0.0);
    /* Default (CC91 40): a reverb tail after the dry sound has ended. */
    CHECK_EQ(render_fx(&def, g_font, 1, wet, len, 1000), len);
    CHECK(rms(wet, dry_end, dry_end + RATE / 10) > 1e-4);
    CHECK(rms(wet, dry_end, dry_end + RATE / 10) < 0.05);
    /* More send, more reverb. */
    CHECK_EQ(render_fx(&loud, g_font, 1, other, len, 1000), len);
    CHECK(rms(other, dry_end, dry_end + RATE / 10) > 2.0 * rms(wet, dry_end, dry_end + RATE / 10));
    /* Read size changes nothing. */
    CHECK_EQ(render_fx(&def, g_font, 1, other, len, 4093), len);
    CHECK(memcmp(wet, other, bytes) == 0);
    /* CC91 and CC93 0: exactly the dry sound. */
    CHECK_EQ(render_fx(&none, g_font, 1, other, len, 777), len);
    CHECK(memcmp(dry, other, bytes) == 0);
    /* Chorus: changes the sound while the note plays, gone 20 ms after the dry. */
    CHECK_EQ(render_fx(&chorus, g_font, 1, other, len, 1000), len);
    {
        double diff = 0;
        fa_frames i;
        for (i = 2000; i < note_end; i++) diff += fabs((double)other[i * 2] - dry[i * 2]);
        CHECK(diff / (double)(note_end - 2000) > 1e-3);
    }
    CHECK(rms(other, dry_end + RATE / 50, len) == 0.0);
    /* The soundfont's own reverb send (50%) with CC91 0. */
    {
        buf sf = make_sf2_ex3(0, 0, 500);
        CHECK_EQ(fa_soundfont_open_memory(sf.d, sf.n, &sendfont), FA_OK);
        free(sf.d);
        CHECK_EQ(render_fx(&none, sendfont, 1, other, len, 1000), len);
        CHECK(rms(other, dry_end, dry_end + RATE / 10) > 1e-4);
    }
    /* A sound much shorter than the reverb's first echo still leaves a tail:
     * muted after 512 frames (it ends within 10 ms). */
    s = open_bytes(&loud, FA_OPEN_DECODE_ONLY, g_font, &r);
    CHECK(s);
    if (s) {
        CHECK_EQ(read_all(s, other, 512, 512), 512);
        CHECK_EQ(fa_stream_set_midi_channel_mute(s, 0, 1), FA_OK);
        CHECK_EQ(read_all(s, other, RATE / 2, 512), RATE / 2);
        CHECK(rms(other, RATE / 20, RATE / 5) > 1e-5);
        fa_stream_close(s);
    }
    /* Off while the tail rings: it stops at once (by the next block). */
    s = open_bytes(&def, FA_OPEN_DECODE_ONLY, g_font, &r);
    CHECK(s);
    if (s) {
        CHECK_EQ(read_all(s, other, dry_end, 1000), dry_end);
        CHECK_EQ(fa_stream_set_midi_effects(s, 0), FA_OK);
        CHECK_EQ(fa_stream_get_midi_effects(s, &on), FA_OK);
        CHECK_EQ(on, 0);
        CHECK_EQ(read_all(s, other, len - dry_end, 1000), len - dry_end);
        CHECK(rms(other, 64, len - dry_end) == 0.0);
        fa_stream_close(s);
    }
    free(dry); free(wet); free(other);
    /* A seek past the tail (the second note at 5 s) equals reading through. */
    {
        buf two = make_fx_song(127, 127, 5);
        fa_frames total = (fa_frames)(6.5 * RATE), at = 4 * RATE + RATE / 2 + 4321;
        float* all = (float*)calloc((size_t)total * 2, sizeof(float));
        float* part = (float*)calloc((size_t)total * 2, sizeof(float));
        s = open_bytes(&two, FA_OPEN_DECODE_ONLY, g_font, &r);
        CHECK(s);
        if (s) {
            CHECK_EQ(read_all(s, all, total, 1000), total);
            CHECK(rms(all, 4 * RATE, 5 * RATE) == 0.0);   /* the tail has died away */
            CHECK(rms(all, 5 * RATE + 1000, 5 * RATE + RATE / 2) > 0.1);
            CHECK_EQ(fa_stream_seek(s, at), FA_OK);
            CHECK_EQ(read_all(s, part, total - at, 999), total - at);
            CHECK(memcmp(all + at * 2, part, (size_t)(total - at) * 2 * sizeof(float)) == 0);
            fa_stream_close(s);
        }
        /* Not a MIDI stream, or no stream. */
        CHECK_EQ(fa_stream_set_midi_effects(NULL, 1), FA_ERROR_INVALID_ARG);
        CHECK_EQ(fa_stream_get_midi_effects(NULL, &on), FA_ERROR_INVALID_ARG);
        free(all); free(part); free(two.d);
    }
    fa_soundfont_close(sendfont);
    free(def.d); free(none.d); free(chorus.d); free(loud.d);
}

/* GS / XG effect SysEx. */
#define GS(a, v) { 0x41, 0x10, 0x42, 0x12, 0x40, 0x01, (a), (v), 0x00, 0xF7 }
#define GS_PART1(a, v) { 0x41, 0x10, 0x42, 0x12, 0x40, 0x11, (a), (v), 0x00, 0xF7 }
#define XG(a, v) { 0x43, 0x10, 0x4C, 0x02, 0x01, (a), (v), 0xF7 }

/* SysEx, then one note (channel 0, key 69, 0..1 s), and the song going on to 4.5 s
 * (a controller at 3.5 s; the length is a second after the last event), so the
 * reverb rings out. */
static buf make_fx_sysex_song(const uint8_t* const* msgs, const size_t* lens, int count)
{
    buf f = { 0 }, t = { 0 };
    int i;
    mthd(&f, 0, 1, 480);
    for (i = 0; i < count; i++) sysex(&t, 0, msgs[i], lens[i]);
    ev3(&t, 0, 0x90, 69, 127);
    ev3(&t, 960, 0x80, 69, 0);
    ev3(&t, 2400, 0xBF, 1, 0);
    eot(&t, 0);
    mtrk(&f, &t);
    free(t.d);
    return f;
}

/* The note of make_fx_sysex_song after up to three SysEx. */
static fa_frames render_sysex(const uint8_t* a, size_t na, const uint8_t* b, size_t nb, const uint8_t* c, size_t nc,
                              int effects, float* out, fa_frames len, fa_frames chunk)
{
    const uint8_t* m[3];
    size_t n[3];
    int k = 0;
    buf song;
    fa_frames got;
    if (a) { m[k] = a; n[k++] = na; }
    if (b) { m[k] = b; n[k++] = nb; }
    if (c) { m[k] = c; n[k++] = nc; }
    song = make_fx_sysex_song(m, n, k);
    got = render_fx(&song, g_font, effects, out, len, chunk);
    free(song.d);
    return got;
}

/* Last frame that is not silent, +1. */
static fa_frames sound_end(const float* x, fa_frames len)
{
    while (len > 0 && x[len * 2 - 1] == 0.0f && x[len * 2 - 2] == 0.0f) len--;
    return len;
}

/* Reverb and chorus type, level and time from GS and XG SysEx; part sends. */
static void test_gs_xg_effects(void)
{
    static const uint8_t gs_reset[] = { 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7 };
    static const uint8_t xg_on[] = { 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7 };
    static const uint8_t hall2[] = GS(0x30, 4), room1[] = GS(0x30, 0), long_time[] = GS(0x34, 127);
    static const uint8_t level0[] = GS(0x33, 0), delay[] = GS(0x30, 6), pan_delay[] = GS(0x30, 7);
    static const uint8_t delay_long[] = GS(0x34, 127), fb0[] = GS(0x35, 0), fb127[] = GS(0x35, 127);
    static const uint8_t send0[] = GS_PART1(0x22, 0), chorus_send[] = GS_PART1(0x21, 127);
    static const uint8_t chorus_level0[] = GS(0x3A, 0), flanger[] = GS(0x38, 5);
    /* Several data bytes: reverb level 0 then time 127 (0x33, 0x34). */
    static const uint8_t level_time[] = { 0x41, 0x10, 0x42, 0x12, 0x40, 0x01, 0x33, 0x00, 0x7F, 0x00, 0xF7 };
    static const uint8_t xg_room1[] = { 0x43, 0x10, 0x4C, 0x02, 0x01, 0x00, 0x02, 0x00, 0xF7 };   /* type MSB and LSB */
    static const uint8_t xg_time_max[] = XG(0x02, 69), xg_off[] = XG(0x00, 0), xg_return0[] = XG(0x0C, 0);
    static const uint8_t xg_chorus_off[] = XG(0x20, 0);
    static const uint8_t xg_chorus_send[] = { 0x43, 0x10, 0x4C, 0x08, 0x00, 0x12, 0x7F, 0xF7 };
    static const uint8_t xg_send0[] = { 0x43, 0x10, 0x4C, 0x08, 0x00, 0x13, 0x00, 0xF7 };
    const fa_frames len = 3 * RATE;
    size_t bytes = (size_t)len * 2 * sizeof(float);
    float* dry = (float*)calloc((size_t)len * 2, sizeof(float));
    float* def = (float*)calloc((size_t)len * 2, sizeof(float));
    float* x = (float*)calloc((size_t)len * 2, sizeof(float));
    float* y = (float*)calloc((size_t)len * 2, sizeof(float));
    fa_frames end, late0, late1, i, d;
    double tail;

    CHECK_EQ(render_sysex(NULL, 0, NULL, 0, NULL, 0, 0, dry, len, 1000), len);
    CHECK_EQ(render_sysex(NULL, 0, NULL, 0, NULL, 0, 1, def, len, 1000), len);
    end = sound_end(dry, len);
    CHECK(end > RATE && end < 2 * RATE);
    late0 = end + RATE / 2;
    late1 = end + RATE;
    tail = rms(def, late0, late1);
    CHECK(tail > 1e-6);
    /* GS reset and Hall 2 at its defaults: the reverb as without SysEx. */
    CHECK_EQ(render_sysex(gs_reset, sizeof(gs_reset), hall2, sizeof(hall2), NULL, 0, 1, x, len, 1000), len);
    CHECK(memcmp(x, def, bytes) == 0);
    /* Room 1: a shorter tail; reverb time 127: a much longer one. */
    CHECK_EQ(render_sysex(gs_reset, sizeof(gs_reset), room1, sizeof(room1), NULL, 0, 1, x, len, 1000), len);
    CHECK(rms(x, late0, late1) < tail * 0.1);
    CHECK(rms(x, end, end + RATE / 10) > 1e-5);
    CHECK_EQ(render_sysex(gs_reset, sizeof(gs_reset), long_time, sizeof(long_time), NULL, 0, 1, x, len, 1000), len);
    CHECK(rms(x, late0, late1) > tail * 5.0);
    /* Level 0, or the part's reverb send 0: exactly the dry sound (CC93 is 0). */
    CHECK_EQ(render_sysex(gs_reset, sizeof(gs_reset), level0, sizeof(level0), NULL, 0, 1, x, len, 1000), len);
    CHECK(memcmp(x, dry, bytes) == 0);
    CHECK_EQ(render_sysex(gs_reset, sizeof(gs_reset), send0, sizeof(send0), NULL, 0, 1, x, len, 1000), len);
    CHECK(memcmp(x, dry, bytes) == 0);
    CHECK_EQ(render_sysex(gs_reset, sizeof(gs_reset), level_time, sizeof(level_time), NULL, 0, 1, x, len, 1000), len);
    CHECK(memcmp(x, dry, bytes) == 0);
    /* A GS reset after them puts the default reverb back. */
    CHECK_EQ(render_sysex(level0, sizeof(level0), room1, sizeof(room1), gs_reset, sizeof(gs_reset), 1, x, len, 1000), len);
    CHECK(memcmp(x, def, bytes) == 0);

    /* Delay (time 127: (127 + 1) * 3.3 ms), no feedback: one echo of the send, and
     * after it silence. Panning delay: the first echo on the left only. */
    d = (fa_frames)((127 + 1) * 3.3 * RATE / 1000.0);
    CHECK_EQ(render_sysex(delay, sizeof(delay), delay_long, sizeof(delay_long), fb0, sizeof(fb0), 1, x, len, 1000), len);
    CHECK(rms(x, end, end + d / 2) > 1e-4);
    for (i = end + d + 2; i < len && x[i * 2] == 0.0f && x[i * 2 + 1] == 0.0f; i++) {}
    CHECK_EQ(i, len);
    CHECK_EQ(render_sysex(pan_delay, sizeof(pan_delay), delay_long, sizeof(delay_long), fb0, sizeof(fb0), 1, x, len, 1000), len);
    {
        double right = 0.0, left = 0.0;
        for (i = 0; i < len; i++) {
            left += fabs((double)x[i * 2] - dry[i * 2]);
            right += fabs((double)x[i * 2 + 1] - dry[i * 2 + 1]);
        }
        CHECK(left > 1.0);
        CHECK(right == 0.0);
    }
    /* With feedback the echoes go on past the first. */
    CHECK_EQ(render_sysex(delay, sizeof(delay), delay_long, sizeof(delay_long), fb127, sizeof(fb127), 1, x, len, 1000), len);
    CHECK(rms(x, end + d + 2, end + 2 * d) > 1e-5);
    /* Read size changes nothing, with feedback too. */
    CHECK_EQ(render_sysex(delay, sizeof(delay), delay_long, sizeof(delay_long), fb127, sizeof(fb127), 1, y, len, 4093), len);
    CHECK(memcmp(x, y, bytes) == 0);

    /* Chorus: the part's chorus send; level 0 takes it away again; the flanger
     * differs from the default chorus and is the same at any read size. */
    CHECK_EQ(render_sysex(gs_reset, sizeof(gs_reset), send0, sizeof(send0), chorus_send, sizeof(chorus_send), 1, x, len, 1000), len);
    CHECK(memcmp(x, dry, bytes) != 0);
    CHECK_EQ(render_sysex(send0, sizeof(send0), chorus_send, sizeof(chorus_send), chorus_level0, sizeof(chorus_level0), 1, y, len, 1000), len);
    CHECK(memcmp(y, dry, bytes) == 0);
    CHECK_EQ(render_sysex(send0, sizeof(send0), chorus_send, sizeof(chorus_send), flanger, sizeof(flanger), 1, y, len, 1000), len);
    CHECK(memcmp(x, y, bytes) != 0);
    CHECK(rms(y, 2000, RATE / 2) > 0.05);
    CHECK_EQ(render_sysex(send0, sizeof(send0), chorus_send, sizeof(chorus_send), flanger, sizeof(flanger), 1, x, len, 777), len);
    CHECK(memcmp(x, y, bytes) == 0);

    /* XG: Room 1 (type 02 00), time 69 (30 s), no effect, return 0, part sends. */
    CHECK_EQ(render_sysex(xg_on, sizeof(xg_on), xg_room1, sizeof(xg_room1), NULL, 0, 1, x, len, 1000), len);
    CHECK(rms(x, late0, late1) < tail * 0.1);
    CHECK_EQ(render_sysex(xg_on, sizeof(xg_on), xg_time_max, sizeof(xg_time_max), NULL, 0, 1, x, len, 1000), len);
    CHECK(rms(x, late0, late1) > tail * 5.0);
    CHECK_EQ(render_sysex(xg_on, sizeof(xg_on), xg_off, sizeof(xg_off), NULL, 0, 1, x, len, 1000), len);
    CHECK(memcmp(x, dry, bytes) == 0);
    CHECK_EQ(render_sysex(xg_on, sizeof(xg_on), xg_return0, sizeof(xg_return0), NULL, 0, 1, x, len, 1000), len);
    CHECK(memcmp(x, dry, bytes) == 0);
    CHECK_EQ(render_sysex(xg_on, sizeof(xg_on), xg_send0, sizeof(xg_send0), NULL, 0, 1, x, len, 1000), len);
    CHECK(memcmp(x, dry, bytes) == 0);
    CHECK_EQ(render_sysex(xg_send0, sizeof(xg_send0), xg_chorus_send, sizeof(xg_chorus_send), NULL, 0, 1, x, len, 1000), len);
    CHECK(memcmp(x, dry, bytes) != 0);
    CHECK_EQ(render_sysex(xg_send0, sizeof(xg_send0), xg_chorus_send, sizeof(xg_chorus_send), xg_chorus_off, sizeof(xg_chorus_off), 1, x, len, 1000), len);
    CHECK(memcmp(x, dry, bytes) == 0);
    free(dry); free(def); free(x); free(y);
}

/* With effects on, a seek renders the time before the target, so the reverb of
 * what played just before rings on after it: within FA_MIDI_PREROLL_S (1 s) of
 * the start it is exactly reading through. The effect settings at the target
 * are kept. */
static void test_seek_keeps_tails(void)
{
    static const uint8_t long_time[] = GS(0x34, 127);
    const uint8_t* m[1] = { long_time };
    size_t n[1] = { sizeof(long_time) };
    buf song = make_fx_sysex_song(m, n, 1);
    const fa_frames len = 4 * RATE, at = RATE * 9 / 10 + 33;
    fa_frames far;
    float* all = (float*)calloc((size_t)len * 2, sizeof(float));
    float* part = (float*)calloc((size_t)len * 2, sizeof(float));
    fa_stream* s = open_bytes(&song, FA_OPEN_DECODE_ONLY, g_font, NULL);
    CHECK(s);
    if (s) {
        CHECK_EQ(read_all(s, all, len, 1000), len);
        CHECK(rms(all, at, at + RATE / 10) > 1e-4);
        CHECK_EQ(fa_stream_seek(s, at), FA_OK);
        CHECK_EQ(read_all(s, part, len - at, 999), len - at);
        CHECK(memcmp(all + at * 2, part, (size_t)(len - at) * 2 * sizeof(float)) == 0);
        /* Half a second after the sound ends: the reverb rings on, at about the level
         * of reading through (the reverb time set by the SysEx is kept). Not the
         * same: the note held 1 s before the target restarted there, and the reverb
         * of what came before is missing. */
        CHECK_EQ(render_fx(&song, g_font, 0, part, len, 1000), len);
        far = sound_end(part, len);
        CHECK(far > RATE && far < 2 * RATE);
        far += RATE / 2 + 11;
        CHECK_EQ(fa_stream_seek(s, far), FA_OK);
        CHECK_EQ(read_all(s, part, RATE / 2, 999), RATE / 2);
        CHECK(rms(part, 0, RATE / 4) > 0.5 * rms(all + far * 2, 0, RATE / 4));
        CHECK(rms(part, 0, RATE / 4) < 2.0 * rms(all + far * 2, 0, RATE / 4));
        fa_stream_close(s);
    }
    free(all); free(part); free(song.d);
}

static void test_format2(void)
{
    buf f = { 0 }, t = { 0 };
    fa_stream* s;
    fa_stream_info info;
    fa_midi_info mi;
    fa_midi_text x;
    uint32_t i, seen = 0;
    mthd(&f, 2, 2, 480);
    meta(&t, 0, 0x01, "@KMIDI KARAOKE FILE");
    meta(&t, 0, 0x05, "one");
    ev3(&t, 0, 0x90, 69, 100);
    ev3(&t, 480, 0x80, 69, 0);
    eot(&t, 0);
    mtrk(&f, &t);
    meta(&t, 0, 0x05, "\xE0\x80\xAF");
    ev3(&t, 0, 0x90, 69, 100);
    ev3(&t, 480, 0x80, 69, 0);
    eot(&t, 0);
    mtrk(&f, &t);
    s = open_bytes(&f, FA_OPEN_DECODE_ONLY, NULL, NULL);
    CHECK(s);
    if (s) {
        fa_stream_get_info(s, &info);
        CHECK_EQ(info.length_frames, 2 * RATE);       /* 0.5 s + 0.5 s + the 1 s tail */
        CHECK_EQ(fa_stream_get_midi_info(s, &mi), FA_OK);
        CHECK_EQ(mi.format, 2);
        CHECK_EQ(mi.karaoke, 1);
        CHECK_EQ(mi.lyric_count, 2);
        for (i = 0; i < mi.text_count; i++) {
            CHECK_EQ(fa_stream_get_midi_text(s, i, &x), FA_OK);
            if (x.type != FA_MIDI_LYRIC) continue;
            if (seen == 0) { CHECK(strcmp(x.text, "one") == 0); CHECK_EQ(x.frame, 0); }
            if (seen == 1) { CHECK(strcmp(x.text, "\xC3\xA0\xE2\x82\xAC\xC2\xAF") == 0); CHECK_EQ(x.frame, RATE / 2); CHECK_EQ(x.track, 1); }
            seen++;
        }
        CHECK_EQ(seen, 2);
        fa_stream_close(s);
    }
    free(f.d); free(t.d);
}

static void test_not_midi(void)
{
    /* A WAV stream has no MIDI state. */
    uint8_t wav[48] = { 'R', 'I', 'F', 'F', 40, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,
                        0x44, 0xAC, 0, 0, 0x88, 0x58, 1, 0, 2, 0, 16, 0, 'd', 'a', 't', 'a', 4, 0, 0, 0, 0, 0, 0, 0 };
    fa_stream* s = NULL;
    fa_open_options o;
    fa_midi_info info;
    int32_t v;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    CHECK_EQ(fa_stream_open_memory(g_engine, wav, sizeof(wav), &o, &s), FA_OK);
    CHECK_EQ(fa_stream_get_midi_info(s, &info), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_stream_set_midi_transpose(s, 1), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_stream_get_midi_channel_mute(s, 0, &v), FA_ERROR_INVALID_ARG);
    fa_stream_close(s);
}

static void test_soundfont_lifetime(void)
{
    buf sf = make_sf2(), song = make_song();
    fa_soundfont* font = NULL;
    fa_soundfont_info info;
    fa_stream* s;
    float x[2048];
    CHECK_EQ(fa_soundfont_open_memory(sf.d, sf.n, &font), FA_OK);
    CHECK_EQ(fa_soundfont_get_info(font, &info), FA_OK);
    CHECK(strcmp(info.name, "Test Sine") == 0);
    CHECK_EQ(info.preset_count, 2);
    CHECK_EQ(info.sample_memory, (SINE_LEN + 46) * 4);
    s = open_bytes(&song, FA_OPEN_DECODE_ONLY, font, NULL);
    CHECK(s);
    /* The stream holds its own reference: closing ours changes nothing. */
    fa_soundfont_close(font);
    CHECK_EQ(read_all(s, x, 1024, 1024), 1024);
    CHECK(rms(x, 0, 1024) > 0.1);
    fa_stream_close(s);
    CHECK_EQ(fa_soundfont_open_path("no-such-file.sf2", &font), FA_ERROR_NOT_FOUND);
    CHECK_EQ(fa_soundfont_open_memory(song.d, song.n, &font), FA_ERROR_UNSUPPORTED_FORMAT);
    CHECK(!font);
    free(sf.d); free(song.d);
}

/* Several streams render with one font at the same time, each on its own thread. */
typedef struct render_job { fa_stream* s; float* out; fa_frames got; } render_job;

static void render_thread(void* arg)
{
    render_job* j = (render_job*)arg;
    j->got = read_all(j->s, j->out, 3 * RATE, 1500);
}

static void test_shared_font_threads(void)
{
    buf song = make_song();
    render_job jobs[4];
    fa_thread th[4];
    int i;
    for (i = 0; i < 4; i++) {
        jobs[i].s = open_bytes(&song, FA_OPEN_DECODE_ONLY, NULL, NULL);
        jobs[i].out = (float*)calloc((size_t)3 * RATE * 2, sizeof(float));
        CHECK(jobs[i].s);
    }
    for (i = 0; i < 4; i++) fa_thread_create(&th[i], render_thread, &jobs[i]);
    for (i = 0; i < 4; i++) fa_thread_join(&th[i]);
    for (i = 0; i < 4; i++) {
        CHECK_EQ(jobs[i].got, 3 * RATE);
        CHECK(memcmp(jobs[0].out, jobs[i].out, (size_t)3 * RATE * 2 * sizeof(float)) == 0);
    }
    for (i = 0; i < 4; i++) { fa_stream_close(jobs[i].s); free(jobs[i].out); }
    free(song.d);
}

/* ---- on a device: FA_EVENT_MIDI_TEXT -------------------------------------------- */

typedef struct text_rec {
    fa_mutex lock;
    int count;
    int64_t values[16];
    fa_frames frames[16];
    int ended;
} text_rec;

static void FA_CALL on_event(const fa_event* ev, void* user)
{
    text_rec* r = (text_rec*)user;
    fa_mutex_lock(&r->lock);
    if (ev->type == FA_EVENT_MIDI_TEXT && r->count < 16) { r->values[r->count] = ev->value; r->frames[r->count] = ev->frame; r->count++; }
    if (ev->type == FA_EVENT_ENDED) r->ended = 1;
    fa_mutex_unlock(&r->lock);
}

static void test_text_events_on_device(void)
{
    buf song = make_song();
    fa_device* dev = NULL;
    fa_stream* s;
    text_rec rec;
    float out[512 * 2];
    int64_t rendered = 0, total;
    int tries, i;
    memset(&rec, 0, sizeof(rec));
    fa_mutex_init(&rec.lock);
    CHECK_EQ(fa_device_open(g_engine, NULL, &dev), FA_OK);
    s = open_bytes(&song, 0, NULL, NULL);
    CHECK(s);
    fa_stream_set_event_callback(s, on_event, &rec);
    CHECK_EQ(fa_stream_attach(s, dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    /* This test plays the audio thread: the device was never started. */
    total = (int64_t)dev->rate * 7 / 2;
    while (rendered < total) {
        for (tries = 0; tries < 2000; tries++) {
            if (fa_ring_readable(&s->ring) >= 512 || fa_load32(&s->eof)) break;
            fa_sleep_ms(1);
        }
        fa_fetch_add64(&dev->epoch, 1);
        fa_device_render(dev, out, 512);
        fa_fetch_add64(&dev->epoch, 1);
        rendered += 512;
    }
    for (tries = 0; tries < 400; tries++) {
        int done;
        fa_mutex_lock(&rec.lock);
        done = rec.ended;
        fa_mutex_unlock(&rec.lock);
        if (done) break;
        fa_sleep_ms(5);
    }
    fa_stream_close(s);
    fa_device_close(dev);
    /* Every text once, in order, at its frame. */
    CHECK(rec.ended);
    CHECK_EQ(rec.count, 5);
    for (i = 0; i < rec.count; i++) {
        CHECK_EQ(rec.values[i], i);
        if (i > 0) CHECK(rec.frames[i] >= rec.frames[i - 1]);
    }
    CHECK_EQ(rec.frames[4], 66150);
    fa_mutex_destroy(&rec.lock);
    free(song.d);
}

/* ---- loudness scans -------------------------------------------------------------- */

static void test_loudness_scan(void)
{
    buf song = make_song();
    const char* path = "midi_scan_test.mid";
    FILE* f = fopen(path, "wb");
    fa_loudness l;
    CHECK(f);
    fwrite(song.d, 1, song.n, f);
    fclose(f);
    CHECK_EQ(fa_scan_loudness_path(g_engine, path, &l), FA_OK);
    CHECK(l.integrated_lufs > -40.0 && l.integrated_lufs < 0.0);
    remove(path);
    free(song.d);
}

/* ---- malformed data ------------------------------------------------------------- */

static uint32_t g_seed = 12345;
static uint32_t rnd(void) { g_seed = g_seed * 1664525u + 1013904223u; return g_seed >> 8; }

/* Opens, reads a little, seeks and reads again; anything but a crash is fine. */
static void try_midi(const uint8_t* d, size_t n)
{
    fa_open_options o;
    fa_stream* s = NULL;
    float x[4096 * 2];
    fa_frames got;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    if (n == 0 || fa_stream_open_memory(g_engine, d, n, &o, &s) != FA_OK) return;
    fa_stream_read(s, x, 4096, &got);
    fa_stream_seek(s, 1000);
    fa_stream_read(s, x, 4096, &got);
    {
        fa_midi_info info;
        fa_midi_text t;
        uint32_t i;
        fa_stream_get_midi_info(s, &info);
        for (i = 0; i < info.text_count && i < 64; i++) fa_stream_get_midi_text(s, i, &t);
    }
    fa_stream_close(s);
}

static void try_font(const uint8_t* d, size_t n, const buf* song)
{
    fa_soundfont* font = NULL;
    fa_stream* s;
    float x[2048 * 2];
    fa_frames got;
    if (fa_soundfont_open_memory(d, n, &font) != FA_OK) return;
    s = open_bytes(song, FA_OPEN_DECODE_ONLY, font, NULL);
    fa_soundfont_close(font);
    if (!s) return;
    while (fa_stream_read(s, x, 2048, &got) == FA_OK) {}
    fa_stream_close(s);
}

static void test_malformed_midi(void)
{
    buf song = make_song(), f = { 0 }, t = { 0 };
    uint8_t* copy = (uint8_t*)malloc(song.n + 64);
    size_t n;
    int i;
    /* Every truncation. */
    for (n = 0; n <= song.n; n++) try_midi(song.d, n);
    /* Random damage. */
    for (i = 0; i < 3000; i++) {
        int k, flips = 1 + (int)(rnd() % 6);
        memcpy(copy, song.d, song.n);
        for (k = 0; k < flips; k++) copy[14 + rnd() % (song.n - 14)] = (uint8_t)rnd();
        try_midi(copy, song.n);
    }
    /* Hand-made nasties: huge lengths, many tracks, no division, SMPTE timing,
     * a status-less start, sysex and meta lengths past the end. */
    mthd(&f, 1, 255, 480); put(&f, "MTrk", 4); put32be(&f, 0x7FFFFFFF); try_midi(f.d, f.n); f.n = 0;
    mthd(&f, 0, 1, 0); ev3(&t, 0, 0x90, 60, 100); eot(&t, 10); mtrk(&f, &t); try_midi(f.d, f.n); f.n = 0;
    mthd(&f, 0, 1, 0xE728); ev3(&t, 0, 0x90, 60, 100); ev3(&t, 1000, 0x80, 60, 0); mtrk(&f, &t); try_midi(f.d, f.n); f.n = 0;
    mthd(&f, 0, 1, 480); put8(&t, 0); put8(&t, 60); put8(&t, 100); mtrk(&f, &t); try_midi(f.d, f.n); f.n = 0;
    mthd(&f, 0, 1, 480); put8(&t, 0); put8(&t, 0xF0); put8(&t, 0x8F); put8(&t, 0xFF); put8(&t, 0xFF); put8(&t, 0x7F); mtrk(&f, &t); try_midi(f.d, f.n); f.n = 0;
    mthd(&f, 0, 1, 480); put8(&t, 0); put8(&t, 0xFF); put8(&t, 0x05); put8(&t, 0x8F); put8(&t, 0xFF); put8(&t, 0x7F); mtrk(&f, &t); try_midi(f.d, f.n); f.n = 0;
    mthd(&f, 0, 1, 480); varlen(&t, 0x0FFFFFFF); put8(&t, 0x90); put8(&t, 60); put8(&t, 1); varlen(&t, 0x0FFFFFFF); put8(&t, 0x80); put8(&t, 60); put8(&t, 0);
    mtrk(&f, &t); try_midi(f.d, f.n); f.n = 0;
    /* RIFF RMID with a lying data size. */
    put(&f, "RIFF", 4); put32le(&f, 100); put(&f, "RMIDdata", 8); put32le(&f, 0xFFFFFFF0); put(&f, song.d, song.n);
    try_midi(f.d, f.n); f.n = 0;
    free(copy); free(song.d); free(f.d); free(t.d);
}

static void test_malformed_soundfont(void)
{
    buf sf = make_sf2(), song = make_note(0);
    uint8_t* copy = (uint8_t*)malloc(sf.n);
    size_t n;
    int i;
    for (n = 0; n <= sf.n; n += (n < 200 || n > sf.n - 2000 ? 1 : 97)) try_font(sf.d, n, &song);
    for (i = 0; i < 1500; i++) {
        int k, flips = 1 + (int)(rnd() % 4);
        memcpy(copy, sf.d, sf.n);
        for (k = 0; k < flips; k++) {
            /* Mostly the hydra and the chunk headers, where the indices are. */
            size_t at = rnd() % 4 == 0 ? rnd() % sf.n : sf.n - 1 - rnd() % 700;
            copy[at] = (uint8_t)rnd();
        }
        try_font(copy, sf.n, &song);
    }
    free(copy); free(sf.d); free(song.d);
}

/* ---- DLS banks ------------------------------------------------------------------- */

static void list_chunk(buf* b, const char* type, const buf* body)
{
    buf l = { 0 };
    put(&l, type, 4);
    put(&l, body->d, body->n);
    chunk(b, "LIST", &l);
    free(l.d);
}

/* wsmp: unity note, gain (dB), and a forward loop over [start, start + len) if len. */
static void dls_wsmp(buf* b, unsigned unity, double gain_db, uint32_t start, uint32_t len)
{
    buf c = { 0 };
    put32le(&c, 20); put16le(&c, unity); put16le(&c, 0);
    put32le(&c, (uint32_t)(int32_t)lrint(gain_db * 655360.0)); put32le(&c, 0); put32le(&c, len ? 1 : 0);
    if (len) { put32le(&c, 16); put32le(&c, 0); put32le(&c, start); put32le(&c, len); }
    chunk(b, "wsmp", &c);
    free(c.d);
}

static void dls_wave(buf* pool, uint32_t* offsets, int index, int channels, int bits, uint32_t rate, uint32_t frames,
                     double hz_left, double hz_right, int with_wsmp)
{
    buf w = { 0 }, c = { 0 };
    uint32_t i;
    int ch;
    put16le(&c, 1); put16le(&c, (unsigned)channels); put32le(&c, rate);
    put32le(&c, rate * (uint32_t)channels * (uint32_t)bits / 8); put16le(&c, (unsigned)(channels * bits / 8)); put16le(&c, (unsigned)bits);
    chunk(&w, "fmt ", &c);
    if (with_wsmp) dls_wsmp(&w, 69, 0.0, 0, frames);
    c.n = 0;
    for (i = 0; i < frames; i++)
        for (ch = 0; ch < channels; ch++) {
            double v = sin(2.0 * PI * (ch ? hz_right : hz_left) * i / rate) * 0.9;
            if (bits == 8) put8(&c, (unsigned)lrint(128.0 + v * 127.0));
            else put16le(&c, (unsigned)(uint16_t)(int16_t)lrint(v * 32767.0));
        }
    chunk(&w, "data", &c);
    offsets[index] = (uint32_t)pool->n;
    list_chunk(pool, "wave", &w);
    free(w.d); free(c.d);
}

static void dls_region(buf* lrgn, unsigned klo, unsigned khi, uint32_t cue, uint32_t channels, const buf* wsmp)
{
    buf r = { 0 }, c = { 0 };
    put16le(&c, klo); put16le(&c, khi); put16le(&c, 0); put16le(&c, 127); put16le(&c, 0); put16le(&c, 0);
    chunk(&r, "rgnh", &c);
    if (wsmp) put(&r, wsmp->d, wsmp->n);
    c.n = 0; put16le(&c, 0); put16le(&c, 0); put32le(&c, channels); put32le(&c, cue);
    chunk(&r, "wlnk", &c);
    list_chunk(lrgn, "rgn ", &r);
    free(r.d); free(c.d);
}

/* EG1: 2^-3 s release, full sustain. */
static void dls_art(buf* ins)
{
    buf a = { 0 }, l = { 0 };
    put32le(&a, 8); put32le(&a, 2);
    put16le(&a, 0); put16le(&a, 0); put16le(&a, 0x209); put16le(&a, 0); put32le(&a, (uint32_t)(-3600 * 65536));
    put16le(&a, 0); put16le(&a, 0); put16le(&a, 0x20A); put16le(&a, 0); put32le(&a, 1000u * 65536u);
    chunk(&l, "art1", &a);
    list_chunk(ins, "lart", &l);
    free(a.d); free(l.d);
}

static void dls_instrument(buf* lins, uint32_t bank, uint32_t program, const char* name, const buf* lrgn, int regions, int art)
{
    buf i = { 0 }, c = { 0 }, info = { 0 };
    put32le(&c, (uint32_t)regions); put32le(&c, bank); put32le(&c, program);
    chunk(&i, "insh", &c);
    list_chunk(&i, "lrgn", lrgn);
    if (art) dls_art(&i);
    c.n = 0; put(&c, name, strlen(name) + 1);
    chunk(&info, "INAM", &c);
    list_chunk(&i, "INFO", &info);
    list_chunk(lins, "ins ", &i);
    free(i.d); free(c.d); free(info.d);
}

/* Program 0: a looped 440 Hz sine (16-bit, its own wsmp). Program 1: keys 0..63 an
 * 8-bit 441 Hz sine at 22050 Hz, keys 64..127 the right channel (220.5 Hz) of a
 * stereo wave. Drum kit: key 36 plays the 440 Hz sine once, 6 dB down. The pool
 * table counts offsets from the pool's data, or (`with_type`) from its 'wvpl'. */
static buf make_dls(int with_type)
{
    buf dls = { 0 }, riff = { 0 }, lins = { 0 }, lrgn = { 0 }, pool = { 0 }, ptbl = { 0 }, ws = { 0 }, c = { 0 }, info = { 0 };
    uint32_t offs[3];
    int i;
    dls_wave(&pool, offs, 0, 1, 16, RATE, SINE_LEN, 440.0, 0, 1);
    dls_wave(&pool, offs, 1, 1, 8, 22050, 2200, 441.0, 0, 1);
    dls_wave(&pool, offs, 2, 2, 16, RATE, 4400, 440.0, 220.5, 0);
    dls_region(&lrgn, 0, 127, 0, 1, NULL);
    dls_instrument(&lins, 0, 0, "DLS Sine", &lrgn, 1, 1);
    lrgn.n = 0;
    dls_region(&lrgn, 0, 63, 1, 1, NULL);
    dls_wsmp(&ws, 69, 0.0, 0, 4400);
    dls_region(&lrgn, 64, 127, 2, 2, &ws);
    dls_instrument(&lins, 0, 1, "DLS Two", &lrgn, 2, 1);
    lrgn.n = 0; ws.n = 0;
    dls_wsmp(&ws, 36, -6.0, 0, 0);
    dls_region(&lrgn, 36, 36, 0, 1, &ws);
    dls_instrument(&lins, 0x80000000u, 0, "DLS Kit", &lrgn, 1, 0);
    put(&riff, "DLS ", 4);
    put32le(&c, 3); chunk(&riff, "colh", &c);
    list_chunk(&riff, "lins", &lins);
    put32le(&ptbl, 8); put32le(&ptbl, 3);
    for (i = 0; i < 3; i++) put32le(&ptbl, offs[i] + (with_type ? 4 : 0));
    chunk(&riff, "ptbl", &ptbl);
    list_chunk(&riff, "wvpl", &pool);
    c.n = 0; put(&c, "Test DLS", 9); chunk(&info, "INAM", &c);
    list_chunk(&riff, "INFO", &info);
    chunk(&dls, "RIFF", &riff);
    free(riff.d); free(lins.d); free(lrgn.d); free(pool.d); free(ptbl.d); free(ws.d); free(c.d); free(info.d);
    return dls;
}

/* Program `prog` on channel `ch`, then key `key` from 0 to 1 s. */
static buf make_prog_note(int ch, int prog, int key)
{
    buf f = { 0 }, t = { 0 };
    mthd(&f, 0, 1, 480);
    varlen(&t, 0); put8(&t, 0xC0 | ch); put8(&t, prog);
    ev3(&t, 0, 0x90 | ch, key, 127);
    ev3(&t, 960, 0x80 | ch, key, 0);
    eot(&t, 0);
    mtrk(&f, &t);
    free(t.d);
    return f;
}

/* Renders a note with `font`: the first 2 s. */
static fa_frames render_note(fa_soundfont* font, int ch, int prog, int key, float* x)
{
    buf song = make_prog_note(ch, prog, key);
    fa_stream* s = open_bytes(&song, FA_OPEN_DECODE_ONLY, font, NULL);
    fa_frames got = 0;
    if (s) {
        fa_stream_set_midi_effects(s, 0);   /* the instrument alone */
        got = read_all(s, x, 2 * RATE, 4096);
        fa_stream_close(s);
    }
    free(song.d);
    return got;
}

/* initialAttenuation is in centibels: 200 is 20 dB down, as the SF2 spec has it. */
static void test_sf2_attenuation(void)
{
    float* x = (float*)malloc(sizeof(float) * 2 * 2 * RATE);
    float* ref = (float*)malloc(sizeof(float) * 2 * 2 * RATE);
    buf sf = make_sf2_atten(200);
    fa_soundfont* f = NULL;
    CHECK_EQ(fa_soundfont_open_memory(sf.d, sf.n, &f), FA_OK);
    free(sf.d);
    CHECK_EQ(render_note(g_font, 0, 0, 69, ref), 2 * RATE);
    CHECK_EQ(render_note(f, 0, 0, 69, x), 2 * RATE);
    CHECK_NEAR(20.0 * log10(rms(x, RATE / 2, RATE * 9 / 10) / rms(ref, RATE / 2, RATE * 9 / 10)), -20.0, 0.5);
    fa_soundfont_release(f);
    free(x);
    free(ref);
}

/* Velocity follows the SF2 default curve (concave, 96 dB): 40*log10(v/127), so
 * velocity 64 is 11.9 dB down, as in FluidSynth. */
static void test_velocity_curve(void)
{
    float* x = (float*)malloc(sizeof(float) * 2 * 2 * RATE);
    buf f = { 0 }, t = { 0 };
    fa_stream* s;
    double loud, soft;
    mthd(&f, 0, 1, 480);
    ev3(&t, 0, 0x90, 69, 127);
    ev3(&t, 480, 0x80, 69, 0);
    ev3(&t, 480, 0x90, 69, 64);
    ev3(&t, 480, 0x80, 69, 0);
    eot(&t, 0);
    mtrk(&f, &t);
    s = open_bytes(&f, FA_OPEN_DECODE_ONLY, NULL, NULL);
    CHECK(s);
    fa_stream_set_midi_effects(s, 0);
    CHECK_EQ(read_all(s, x, 2 * RATE, 4096), 2 * RATE);
    loud = rms(x, RATE / 10, RATE * 4 / 10);
    soft = rms(x, RATE + RATE / 10, RATE + RATE * 4 / 10);
    CHECK_NEAR(20.0 * log10(soft / loud), 40.0 * log10(64.0 / 127.0), 0.3);
    fa_stream_close(s);
    free(f.d); free(t.d); free(x);
}

/* A program the soundfont lacks plays program 0, not the channel's previous one. */
static void test_missing_program(void)
{
    float* x = (float*)malloc(sizeof(float) * 2 * 2 * RATE);
    buf dls = make_dls(1), f = { 0 }, t = { 0 };
    fa_soundfont* font = NULL;
    fa_stream* s;
    CHECK_EQ(fa_soundfont_open_memory(dls.d, dls.n, &font), FA_OK);
    free(dls.d);
    mthd(&f, 0, 1, 480);
    varlen(&t, 0); put8(&t, 0xC0); put8(&t, 1);    /* the 220.5 Hz wave at key 69 */
    ev3(&t, 0, 0x90, 69, 127);
    ev3(&t, 480, 0x80, 69, 0);
    varlen(&t, 480); put8(&t, 0xC0); put8(&t, 50); /* not in the bank */
    ev3(&t, 0, 0x90, 69, 127);
    ev3(&t, 480, 0x80, 69, 0);
    eot(&t, 0);
    mtrk(&f, &t);
    s = open_bytes(&f, FA_OPEN_DECODE_ONLY, font, NULL);
    CHECK(s);
    fa_stream_set_midi_effects(s, 0);
    CHECK_EQ(read_all(s, x, 2 * RATE, 4096), 2 * RATE);
    CHECK_NEAR(frequency(x, RATE / 10, RATE * 4 / 10), 220.5, 2.0);
    CHECK_NEAR(frequency(x, RATE + RATE / 10, RATE + RATE * 4 / 10), 440.0, 3.0);
    fa_stream_close(s);
    fa_soundfont_release(font);
    free(f.d); free(t.d); free(x);
}

static void test_dls_bank(void)
{
    int with_type;
    float* x = (float*)malloc(sizeof(float) * 2 * 2 * RATE);
    float* ref = (float*)malloc(sizeof(float) * 2 * 2 * RATE);
    CHECK(render_note(g_font, 0, 0, 69, ref) == 2 * RATE);
    for (with_type = 0; with_type < 2; with_type++) {
        buf dls = make_dls(with_type);
        fa_soundfont* f = NULL;
        fa_soundfont_info info;
        CHECK_EQ(fa_soundfont_open_memory(dls.d, dls.n, &f), FA_OK);
        free(dls.d);
        CHECK_EQ(fa_soundfont_get_info(f, &info), FA_OK);
        CHECK(strcmp(info.name, "Test DLS") == 0);
        CHECK_EQ(info.preset_count, 3);
        /* The looped 16-bit sine: 440 Hz, held for the note, then a 125 ms release,
         * at the level of the same sine in the SF2 test bank. */
        CHECK_EQ(render_note(f, 0, 0, 69, x), 2 * RATE);
        CHECK_NEAR(frequency(x, RATE / 10, RATE * 9 / 10), 440.0, 3.0);
        CHECK(rms(x, RATE / 2, RATE * 9 / 10) > 0.05);
        CHECK_NEAR(rms(x, RATE / 2, RATE * 9 / 10) / rms(ref, RATE / 2, RATE * 9 / 10), 0.9, 0.05);
        CHECK(rms(x, RATE * 3 / 2, 2 * RATE) < 1e-3);
        /* 8-bit at 22050 Hz, played 29 semitones down; the right channel of a stereo wave. */
        CHECK_EQ(render_note(f, 0, 1, 40, x), 2 * RATE);
        CHECK_NEAR(frequency(x, RATE / 10, RATE * 9 / 10), 441.0 * pow(2.0, -29.0 / 12.0), 2.0);
        CHECK_EQ(render_note(f, 0, 1, 69, x), 2 * RATE);
        CHECK_NEAR(frequency(x, RATE / 10, RATE * 9 / 10), 220.5, 2.0);
        /* The drum: 440 Hz once (100 ms, no loop), 6 dB down. */
        CHECK_EQ(render_note(f, FA_MIDI_TEST_DRUMS, 0, 36, x), 2 * RATE);
        CHECK_NEAR(frequency(x, RATE / 100, RATE / 12), 440.0, 15.0);
        CHECK_NEAR(rms(x, RATE / 100, RATE / 12) / rms(ref, RATE / 100, RATE / 12), 0.9 * 0.5012, 0.05);
        CHECK(rms(x, RATE / 4, RATE / 2) < 1e-3);
        fa_soundfont_close(f);
    }
    free(x);
    free(ref);
}

static void test_malformed_dls(void)
{
    buf dls = make_dls(0), song = make_note(0);
    uint8_t* copy = (uint8_t*)malloc(dls.n);
    size_t n;
    int i;
    for (n = 0; n <= dls.n; n += (n < 300 ? 1 : 61)) try_font(dls.d, n, &song);
    for (i = 0; i < 1500; i++) {
        int k, flips = 1 + (int)(rnd() % 4);
        memcpy(copy, dls.d, dls.n);
        /* The instruments, regions and pool table come first; the samples after them. */
        for (k = 0; k < flips; k++) copy[rnd() % 4 == 0 ? rnd() % dls.n : rnd() % 1200] = (uint8_t)rnd();
        try_font(copy, dls.n, &song);
    }
    free(copy); free(dls.d); free(song.d);
}

/* MIDI files opened without a soundfont play with the OS's bank, loaded once per
 * engine, unless FA_ENGINE_NO_SYSTEM_SOUNDFONT. */
static void test_system_soundfont(void)
{
    buf song = make_song();
    fa_soundfont* sys = NULL;
    fa_engine* e = NULL;
    fa_engine_config cfg;
    fa_stream* s = NULL;
    fa_open_options o;
    fa_result r = fa_soundfont_open_system(&sys);
    /* Whatever this machine has (CI images usually have none). */
    CHECK(r == FA_OK || r == FA_ERROR_NOT_FOUND);
    CHECK((r == FA_OK) == (sys != NULL));
    fa_soundfont_close(sys);
    CHECK_EQ(fa_soundfont_open_system(NULL), FA_ERROR_INVALID_ARG);

    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    CHECK_EQ(fa_engine_create(&cfg, &e), FA_OK);
    /* Stand in for the OS's bank (white box): the test bank. */
    e->system_soundfont = fa_soundfont_retain(g_font);
    e->system_soundfont_tried = 1;
    CHECK_EQ(fa_stream_open_memory(e, song.d, song.n, &o, &s), FA_OK);
    fa_stream_close(s);
    s = NULL;
    /* An app's soundfont still wins, and other formats never load the bank. */
    CHECK(fa_engine_system_soundfont(e) == g_font);
    fa_soundfont_release(g_font);
    fa_engine_destroy(e);

    cfg.flags = FA_ENGINE_NULL_OUTPUT | FA_ENGINE_NO_SYSTEM_SOUNDFONT;
    CHECK_EQ(fa_engine_create(&cfg, &e), FA_OK);
    e->system_soundfont = fa_soundfont_retain(g_font);
    e->system_soundfont_tried = 1;
    CHECK_EQ(fa_stream_open_memory(e, song.d, song.n, &o, &s), FA_ERROR_NO_SOUNDFONT);
    CHECK(fa_engine_system_soundfont(e) == NULL);
    fa_engine_destroy(e);
    free(song.d);
}

int main(void)
{
    fa_engine_config cfg;
    buf sf = make_sf2();
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    cfg.worker_threads = 2;
    if (fa_engine_create(&cfg, &g_engine) != FA_OK) { fprintf(stderr, "engine create failed\n"); return 1; }
    if (fa_soundfont_open_memory(sf.d, sf.n, &g_font) != FA_OK) { fprintf(stderr, "test soundfont rejected\n"); return 1; }
    free(sf.d);
    fa_engine_set_soundfont(g_engine, g_font);

    RUN(test_detect_and_length);
    RUN(test_rmid);
    RUN(test_no_soundfont);
    RUN(test_render_and_seek_exact);
    RUN(test_texts);
    RUN(test_karaoke);
    RUN(test_text_encoding);
    RUN(test_lyric_marks);
    RUN(test_text_fallback);
    RUN(test_credits);
    RUN(test_transpose_and_drums);
    RUN(test_channel_mute);
    RUN(test_sysex);
    RUN(test_seek_restarts_held_notes);
    RUN(test_tempo_and_signature_marks);
    RUN(test_sf3);
    RUN(test_effects);
    RUN(test_gs_xg_effects);
    RUN(test_seek_keeps_tails);
    RUN(test_format2);
    RUN(test_not_midi);
    RUN(test_soundfont_lifetime);
    RUN(test_shared_font_threads);
    RUN(test_text_events_on_device);
    RUN(test_loudness_scan);
    RUN(test_malformed_midi);
    RUN(test_malformed_soundfont);
    RUN(test_sf2_attenuation);
    RUN(test_dls_bank);
    RUN(test_velocity_curve);
    RUN(test_missing_program);
    RUN(test_malformed_dls);
    RUN(test_system_soundfont);

    fa_soundfont_close(g_font);
    fa_engine_destroy(g_engine);
    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    printf("PASS\n");
    return 0;
}
