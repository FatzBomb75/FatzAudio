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
 * sine, root key 69, 125 ms release. */
static buf make_sf2(void)
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
    c.n = 0; put16le(&c, 0); put16le(&c, 0); put16le(&c, 3); put16le(&c, 0); put16le(&c, 6); put16le(&c, 0);
    chunk(&pdta, "ibag", &c);
    c.n = 0; for (i = 0; i < 10; i++) put8(&c, 0);
    chunk(&pdta, "imod", &c);
    c.n = 0;
    gen(&c, 54, 1);                          /* sampleModes: loop */
    gen(&c, 38, (unsigned)(uint16_t)-3600);  /* releaseVolEnv: 2^-3 s */
    gen(&c, 53, 0);                          /* sampleID */
    gen(&c, 54, 1); gen(&c, 38, (unsigned)(uint16_t)-3600); gen(&c, 53, 0);
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

/* Format 2: the tracks are patterns played one after another. Also: a .kar
 * header with lyric events is still karaoke, and invalid UTF-8 (an overlong
 * form) is read as Windows-1252. */
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
    if (s) { got = read_all(s, x, 2 * RATE, 4096); fa_stream_close(s); }
    free(song.d);
    return got;
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
    RUN(test_transpose_and_drums);
    RUN(test_channel_mute);
    RUN(test_format2);
    RUN(test_not_midi);
    RUN(test_soundfont_lifetime);
    RUN(test_shared_font_threads);
    RUN(test_text_events_on_device);
    RUN(test_loudness_scan);
    RUN(test_malformed_midi);
    RUN(test_malformed_soundfont);
    RUN(test_dls_bank);
    RUN(test_malformed_dls);
    RUN(test_system_soundfont);

    fa_soundfont_close(g_font);
    fa_engine_destroy(g_engine);
    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    printf("PASS\n");
    return 0;
}
