/* Soundfonts: stacks of several soundfonts (preset mapping, volume, lifetime), the preset
 * list, DLS connections with controllers and velocity to attack time, DLS 2 conditional
 * chunks and layers, song lengths measured to where the sound dies away, and the OS's
 * own bank when the machine has one (gm.dls on Windows, gs_instruments.dls on macOS). */
#include "midi_synth_test.h"
#include "fatzaudio/fatzaudio.h"
#include <time.h>
#if defined(__APPLE__)
#include <unistd.h>   /* chdir (FA_TEST_CWD) */
#endif

/* ---- helpers ------------------------------------------------------------------------ */

#define CHECK_EQ(a, b) do { long long va_ = (long long)(a), vb_ = (long long)(b); if (va_ != vb_) { \
    fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_failures++; return; } } while (0)

static fa_frames length_of(const buf* f, fa_soundfont* font)
{
    fa_open_options o;
    fa_stream* s = NULL;
    fa_stream_info info;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    o.soundfont = font;
    if (fa_stream_open_memory(g_engine, f->d, f->n, &o, &s) != FA_OK) return -2;
    fa_stream_get_info(s, &info);
    fa_stream_close(s);
    return info.length_frames;
}

/* The pitch of the note (key 69, program `program`) against 440 Hz, in cents. */
static double program_cents(fa_soundfont* font, unsigned program)
{
    song s = { 0 };
    buf f;
    float* x;
    track t;
    ev2(&s, 0, 0xC0, program);
    note(&s, 0, 1500, 69);
    f = finish(&s);
    x = render_from(&f, font, 0);
    free(f.d);
    if (!x) return 1e9;
    t = pitch_track(x, 0.2, 1.2, 440.0);
    free(x);
    return (t.lo + t.hi) / 2;
}

static double program_rms(fa_soundfont* font, unsigned program)
{
    song s = { 0 };
    buf f;
    float* x;
    double r;
    ev2(&s, 0, 0xC0, program);
    note(&s, 0, 1500, 69);
    f = finish(&s);
    x = render_from(&f, font, 0);
    free(f.d);
    if (!x) return 0.0;
    r = rms(x, 0.2, 1.2);
    free(x);
    return r;
}

/* ---- stacks --------------------------------------------------------------------------- */

static fa_soundfont* open_presets(const sfpreset* p, int n)
{
    buf sf = make_sf2_presets(p, n, NULL, 0, NULL, 0);
    fa_soundfont* f = NULL;
    fa_soundfont_open_memory(sf.d, sf.n, &f);
    free(sf.d);
    return f;
}

static void test_stack(void)
{
    /* A: program 0 at 440 Hz. B: program 0 an octave up, program 1 a fifth up. */
    static const sfpreset pa[] = { { 0, 0, 0 } }, pb[] = { { 0, 0, 12 }, { 0, 1, 7 } };
    fa_soundfont *a = open_presets(pa, 1), *b = open_presets(pb, 2), *st = NULL, *st2 = NULL;
    fa_soundfont_stack_entry e[3];
    fa_soundfont_info info;
    fa_soundfont_preset p;
    double level;
    CHECK(a && b);

    /* A over B: program 0 from A, program 1 from B. */
    fa_soundfont_stack_entry_init(&e[0], a);
    fa_soundfont_stack_entry_init(&e[1], b);
    CHECK_EQ(fa_soundfont_open_stack(e, 2, &st), FA_OK);
    CHECK_NEAR(program_cents(st, 0), 0.0, 5.0);
    CHECK_NEAR(program_cents(st, 1), 700.0, 5.0);
    CHECK_EQ(fa_soundfont_get_info(st, &info), FA_OK);
    CHECK_EQ(info.preset_count, 2);
    CHECK_EQ(info.sample_memory, (SINE_LEN + 46) * 2 * 2);   /* both fonts' samples, shared */
    CHECK(strcmp(info.name, "Mod Sine") == 0);
    CHECK_EQ(fa_soundfont_get_preset(st, 1, &p), FA_OK);
    CHECK(p.bank == 0 && p.program == 1 && strcmp(p.name, "Sine") == 0);
    CHECK_EQ(fa_soundfont_get_preset(st, 2, &p), FA_ERROR_INVALID_ARG);
    fa_soundfont_close(st);

    /* B over A: program 0 from B. */
    fa_soundfont_stack_entry_init(&e[0], b);
    fa_soundfont_stack_entry_init(&e[1], a);
    CHECK_EQ(fa_soundfont_open_stack(e, 2, &st), FA_OK);
    CHECK_NEAR(program_cents(st, 0), 1200.0, 5.0);
    fa_soundfont_close(st);

    /* Mapping: B's program 1 as program 5 of bank 0, A for the rest at half volume. A
     * program the stack lacks (1) plays program 0, as with any soundfont. */
    fa_soundfont_stack_entry_init(&e[0], b);
    e[0].bank = 0; e[0].program = 1; e[0].to_program = 5;
    fa_soundfont_stack_entry_init(&e[1], a);
    e[1].volume = 0.5f;
    CHECK_EQ(fa_soundfont_open_stack(e, 2, &st), FA_OK);
    CHECK_EQ(fa_soundfont_get_info(st, &info), FA_OK);
    CHECK_EQ(info.preset_count, 2);
    CHECK_NEAR(program_cents(st, 5), 700.0, 5.0);
    CHECK_NEAR(program_cents(st, 1), 0.0, 5.0);
    level = db(program_rms(st, 0), program_rms(a, 0));
    CHECK_NEAR(level, -6.02, 0.1);
    /* Another bank: B's program 0 as bank 3 program 0; bank select picks it. */
    fa_soundfont_stack_entry_init(&e[2], b);
    e[2].program = 0; e[2].to_bank = 3;
    CHECK_EQ(fa_soundfont_open_stack(e + 2, 1, &st2), FA_OK);
    CHECK_EQ(fa_soundfont_get_preset(st2, 0, &p), FA_OK);
    CHECK(p.bank == 3 && p.program == 0);
    fa_soundfont_close(st2);
    st2 = NULL;

    /* A stack of a stack, after its members were closed: it holds them. */
    fa_soundfont_close(a);
    fa_soundfont_close(b);
    fa_soundfont_stack_entry_init(&e[0], st);
    e[0].volume = 2.0f;
    CHECK_EQ(fa_soundfont_open_stack(e, 1, &st2), FA_OK);
    fa_soundfont_close(st);
    CHECK_NEAR(program_cents(st2, 5), 700.0, 5.0);
    CHECK_NEAR(db(program_rms(st2, 0), program_rms(g_font, 0)), 0.0, 0.1);   /* A: 0.5 * 2 = as is */
    CHECK_NEAR(db(program_rms(st2, 5), program_rms(g_font, 0)), 6.02, 0.1);  /* B: 1 * 2 */

    /* Bad entries; entries that pick nothing. */
    fa_soundfont_stack_entry_init(&e[0], st2);
    CHECK_EQ(fa_soundfont_open_stack(e, 0, &st), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_soundfont_open_stack(NULL, 1, &st), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_soundfont_open_stack(e, 1, NULL), FA_ERROR_INVALID_ARG);
    e[0].font = NULL;
    CHECK_EQ(fa_soundfont_open_stack(e, 1, &st), FA_ERROR_INVALID_ARG);
    fa_soundfont_stack_entry_init(&e[0], st2);
    e[0].program = 128;
    CHECK_EQ(fa_soundfont_open_stack(e, 1, &st), FA_ERROR_INVALID_ARG);
    e[0].program = FA_SOUNDFONT_ALL; e[0].volume = -1.0f;
    CHECK_EQ(fa_soundfont_open_stack(e, 1, &st), FA_ERROR_INVALID_ARG);
    e[0].volume = 1.0f; e[0].bank = 7;
    CHECK_EQ(fa_soundfont_open_stack(e, 1, &st), FA_ERROR_NOT_FOUND);
    CHECK(st == NULL);
    CHECK_EQ(fa_soundfont_get_preset(NULL, 0, &p), FA_ERROR_INVALID_ARG);
    fa_soundfont_close(st2);
}

/* ---- DLS banks --------------------------------------------------------------------------- */

static void list_chunk(buf* b, const char* type, const buf* body)
{
    buf l = { 0 };
    put(&l, type, 4);
    put(&l, body->d, body->n);
    chunk(b, "LIST", &l);
    free(l.d);
}

/* A connection block entry: scale in the destination's units. */
static void conn(buf* a, unsigned src, unsigned ctl, unsigned dst, unsigned trans, double scale)
{
    put16le(a, src); put16le(a, ctl); put16le(a, dst); put16le(a, trans);
    put32le(a, (uint32_t)(int32_t)lrint(scale * 65536.0));
}

/* LIST lar2 { art2 } of `n` connections. */
static void lar2(buf* holder, const buf* conns, int n)
{
    buf a = { 0 }, l = { 0 };
    put32le(&a, 8); put32le(&a, (uint32_t)n);
    put(&a, conns->d, conns->n);
    chunk(&l, "art2", &a);
    list_chunk(holder, "lar2", &l);
    free(a.d); free(l.d);
}

/* 'cdl ' chunks: operators, constants and queries by the first 4 bytes of their GUID. */
enum { OP_GT = 0x0C, OP_NOT = 0x0F, OP_CONST = 0x10, OP_QUERY = 0x11, OP_SUPPORTED = 0x12 };
static const uint8_t TAIL_GM[12] = { 0x64, 0xc3, 0xd1, 0x11, 0xa7, 0x60, 0x00, 0x00, 0xf8, 0x75, 0xac, 0x12 };
static const uint8_t TAIL_DLS2[12] = { 0x89, 0x46, 0xd2, 0x11, 0xaf, 0xa6, 0x00, 0xaa, 0x00, 0x24, 0xd8, 0xb6 };
static void cdl_op(buf* c, unsigned op) { put16le(c, op); }
static void cdl_const(buf* c, uint32_t v) { put16le(c, OP_CONST); put32le(c, v); }
static void cdl_query(buf* c, unsigned op, uint32_t d1, const uint8_t* tail) { put16le(c, op); put32le(c, d1); put(c, tail, 12); }
#define GUID_GM_IN_HARDWARE 0x178f2f24u
#define GUID_DLS2 0xf14599e5u

/* Wave 0: a looped 440 Hz sine; wave 1: 880 Hz. 16-bit, 0.9 of full scale. */
static void dls_wave(buf* pool, uint32_t* offsets, int index, double hz)
{
    buf w = { 0 }, c = { 0 };
    int i;
    put16le(&c, 1); put16le(&c, 1); put32le(&c, RATE); put32le(&c, RATE * 2); put16le(&c, 2); put16le(&c, 16);
    chunk(&w, "fmt ", &c);
    c.n = 0;
    put32le(&c, 20); put16le(&c, 69); put16le(&c, 0); put32le(&c, 0); put32le(&c, 0); put32le(&c, 1);
    put32le(&c, 16); put32le(&c, 0); put32le(&c, 0); put32le(&c, SINE_LEN);
    chunk(&w, "wsmp", &c);
    c.n = 0;
    for (i = 0; i < SINE_LEN; i++) put16le(&c, (unsigned)(uint16_t)(int16_t)lrint(sin(2.0 * PI * hz * i / RATE) * 0.9 * 32767.0));
    chunk(&w, "data", &c);
    offsets[index] = (uint32_t)pool->n;
    list_chunk(pool, "wave", &w);
    free(w.d); free(c.d);
}

/* A DLS 2 region over every key and velocity, on `layer`, playing `cue`; `extra` goes in it. */
static void dls_region(buf* lrgn, unsigned layer, uint32_t cue, const buf* extra)
{
    buf r = { 0 }, c = { 0 };
    put16le(&c, 0); put16le(&c, 127); put16le(&c, 0); put16le(&c, 127); put16le(&c, 0); put16le(&c, 0); put16le(&c, layer);
    chunk(&r, "rgnh", &c);
    if (extra) put(&r, extra->d, extra->n);
    c.n = 0; put16le(&c, 0); put16le(&c, 0); put32le(&c, 1); put32le(&c, cue);
    chunk(&r, "wlnk", &c);
    list_chunk(lrgn, "rgn2", &r);
    free(r.d); free(c.d);
}

/* An instrument: bank 0, `program`, its regions and `extra` (articulation, a condition). */
static void dls_instrument(buf* lins, uint32_t program, const buf* lrgn, int regions, const buf* extra)
{
    buf i = { 0 }, c = { 0 };
    put32le(&c, (uint32_t)regions); put32le(&c, 0); put32le(&c, program);
    chunk(&i, "insh", &c);
    if (extra) put(&i, extra->d, extra->n);
    list_chunk(&i, "lrgn", lrgn);
    list_chunk(lins, "ins ", &i);
    free(i.d); free(c.d);
}

static fa_soundfont* open_dls(const buf* lins, int instruments)
{
    buf dls = { 0 }, riff = { 0 }, pool = { 0 }, ptbl = { 0 }, c = { 0 };
    uint32_t offs[2];
    fa_soundfont* f = NULL;
    int i;
    dls_wave(&pool, offs, 0, 440.0);
    dls_wave(&pool, offs, 1, 880.0);
    put(&riff, "DLS ", 4);
    put32le(&c, (uint32_t)instruments); chunk(&riff, "colh", &c);
    list_chunk(&riff, "lins", lins);
    put32le(&ptbl, 8); put32le(&ptbl, 2);
    for (i = 0; i < 2; i++) put32le(&ptbl, offs[i]);
    chunk(&riff, "ptbl", &ptbl);
    list_chunk(&riff, "wvpl", &pool);
    chunk(&dls, "RIFF", &riff);
    fa_soundfont_open_memory(dls.d, dls.n, &f);
    free(dls.d); free(riff.d); free(pool.d); free(ptbl.d); free(c.d);
    return f;
}

/* DLS sources and destinations. */
enum { SRC_NONE = 0, SRC_LFO = 1, SRC_VELOCITY = 2, SRC_CC1 = 0x81, SRC_CC21 = 0x95,
       DST_GAIN = 1, DST_PITCH = 3, DST_EG1_ATTACK = 0x206, DST_EG1_RELEASE = 0x209, DST_EG1_SUSTAIN = 0x20A };

/* The time the note (velocity `vel`) takes to reach half of its loudest, in seconds. */
static double half_level_time(fa_soundfont* font, unsigned vel)
{
    song s = { 0 };
    buf f;
    float* x;
    double peak = 0, t = -1;
    int w;
    ev3(&s, 0, 0x90, 69, vel);
    ev3(&s, 1500, 0x80, 69, 0);
    f = finish(&s);
    x = render_from(&f, font, 0);
    free(f.d);
    if (!x) return -1;
    for (w = 0; w < 150; w++) { double r = rms(x, w * 0.01, w * 0.01 + 0.01); if (r > peak) peak = r; }
    for (w = 0; w < 150 && t < 0; w++) if (rms(x, w * 0.01, w * 0.01 + 0.01) >= peak / 2) t = w * 0.01;
    free(x);
    return t;
}

/* Velocity to attack time, the mod wheel scaling the LFO's vibrato, a controller to
 * volume: SF2 modulators made from DLS connections. */
static void test_dls_connections(void)
{
    buf lins = { 0 }, lrgn = { 0 }, art = { 0 }, conns = { 0 };
    fa_soundfont* f;
    float* x;
    track t;
    double quiet = 0;
    /* Program 0: a 1 s attack, 6000 timecents shorter at full velocity. */
    conn(&conns, SRC_NONE, SRC_NONE, DST_EG1_ATTACK, 0, 0.0);
    conn(&conns, SRC_VELOCITY, SRC_NONE, DST_EG1_ATTACK, 0, -6000.0);
    conn(&conns, SRC_NONE, SRC_NONE, DST_EG1_SUSTAIN, 0, 1000.0);
    lar2(&art, &conns, 3);
    dls_region(&lrgn, 0, 0, NULL);
    dls_instrument(&lins, 0, &lrgn, 1, &art);
    /* Program 1: LFO x mod wheel -> 200 cents; CC21 -> 20 dB quieter. */
    lrgn.n = art.n = conns.n = 0;
    conn(&conns, SRC_LFO, SRC_CC1, DST_PITCH, 0, 200.0);
    conn(&conns, SRC_CC21, SRC_NONE, DST_GAIN, 0, -200.0);
    lar2(&art, &conns, 2);
    dls_region(&lrgn, 0, 0, NULL);
    dls_instrument(&lins, 1, &lrgn, 1, &art);
    f = open_dls(&lins, 2);
    CHECK(f);

    CHECK(half_level_time(f, 127) < 0.05);
    CHECK(half_level_time(f, 32) > 0.15);

    {
        song s = { 0 };
        buf b;
        ev2(&s, 0, 0xC0, 1);
        cc(&s, 0, 1, 127);
        note(&s, 0, 1500, 69);
        b = finish(&s);
        x = render_from(&b, f, 0);
        free(b.d);
        CHECK(x);
        t = pitch_track(x, 0.3, 1.4, 440.0);
        CHECK(t.hi - t.lo > 300.0 && t.hi - t.lo < 450.0);
        free(x);
    }
    {
        song s = { 0 };
        buf b;
        ev2(&s, 0, 0xC0, 1);
        note(&s, 0, 1500, 69);
        b = finish(&s);
        x = render_from(&b, f, 0);
        free(b.d);
        CHECK(x);
        t = pitch_track(x, 0.3, 1.4, 440.0);
        CHECK(t.hi - t.lo < 5.0);   /* no mod wheel, no vibrato */
        quiet = rms(x, 0.3, 1.4);
        free(x);
    }
    {
        song s = { 0 };
        buf b;
        ev2(&s, 0, 0xC0, 1);
        cc(&s, 0, 21, 127);
        note(&s, 0, 1500, 69);
        b = finish(&s);
        x = render_from(&b, f, 0);
        free(b.d);
        CHECK(x);
        CHECK_NEAR(db(rms(x, 0.3, 1.4), quiet), -20.0 * 127.0 / 128.0, 0.3);
        free(x);
    }
    fa_soundfont_close(f);
    free(lins.d); free(lrgn.d); free(art.d); free(conns.d);
}

/* DLS 2 conditions leave out what this synth doesn't meet; regions on two layers both play. */
static void test_dls_conditions_and_layers(void)
{
    buf lins = { 0 }, lrgn = { 0 }, cdl = { 0 }, extra = { 0 };
    fa_soundfont* f;
    fa_soundfont_info info;
    fa_soundfont_preset p;
    uint32_t i;
    int have[8] = { 0 };
    /* Program 0: a 440 Hz region if GM is in hardware (no), an 880 Hz one if DLS 2 is supported. */
    cdl_query(&cdl, OP_QUERY, GUID_GM_IN_HARDWARE, TAIL_GM);
    chunk(&extra, "cdl ", &cdl);
    dls_region(&lrgn, 0, 0, &extra);
    cdl.n = extra.n = 0;
    cdl_query(&cdl, OP_QUERY, GUID_DLS2, TAIL_DLS2);
    chunk(&extra, "cdl ", &cdl);
    dls_region(&lrgn, 0, 1, &extra);
    dls_instrument(&lins, 0, &lrgn, 2, NULL);
    /* Program 1: only if 2 > 3 (never). */
    lrgn.n = cdl.n = extra.n = 0;
    cdl_const(&cdl, 2); cdl_const(&cdl, 3); cdl_op(&cdl, OP_GT);
    chunk(&extra, "cdl ", &cdl);
    dls_region(&lrgn, 0, 0, NULL);
    dls_instrument(&lins, 1, &lrgn, 1, &extra);
    /* Program 2: only if not GM in hardware and an unknown query is "supported" (no). */
    lrgn.n = cdl.n = extra.n = 0;
    cdl_query(&cdl, OP_SUPPORTED, 0x12345678u, TAIL_GM);
    chunk(&extra, "cdl ", &cdl);
    dls_region(&lrgn, 0, 0, NULL);
    dls_instrument(&lins, 2, &lrgn, 1, &extra);
    /* Program 3: a condition that can't be read (an unknown operator): kept. */
    lrgn.n = cdl.n = extra.n = 0;
    cdl_op(&cdl, 0x99);
    chunk(&extra, "cdl ", &cdl);
    dls_region(&lrgn, 0, 0, NULL);
    dls_instrument(&lins, 3, &lrgn, 1, &extra);
    /* Program 4: two regions of the 440 Hz wave on layers 0 and 1: both play. Program 5: one. */
    lrgn.n = 0;
    dls_region(&lrgn, 0, 0, NULL);
    dls_region(&lrgn, 1, 0, NULL);
    dls_instrument(&lins, 4, &lrgn, 2, NULL);
    lrgn.n = 0;
    dls_region(&lrgn, 0, 0, NULL);
    dls_instrument(&lins, 5, &lrgn, 1, NULL);
    /* Program 6: NOT (GM in hardware): kept. */
    lrgn.n = cdl.n = extra.n = 0;
    cdl_query(&cdl, OP_QUERY, GUID_GM_IN_HARDWARE, TAIL_GM); cdl_op(&cdl, OP_NOT);
    chunk(&extra, "cdl ", &cdl);
    dls_region(&lrgn, 0, 0, NULL);
    dls_instrument(&lins, 6, &lrgn, 1, &extra);
    f = open_dls(&lins, 7);
    CHECK(f);
    CHECK_EQ(fa_soundfont_get_info(f, &info), FA_OK);
    for (i = 0; i < info.preset_count; i++) {
        CHECK_EQ(fa_soundfont_get_preset(f, i, &p), FA_OK);
        if (p.bank == 0 && p.program < 8) have[p.program] = 1;
    }
    CHECK(have[0] && !have[1] && !have[2] && have[3] && have[4] && have[5] && have[6]);
    CHECK_EQ(info.preset_count, 5);
    CHECK_NEAR(program_cents(f, 0), 1200.0, 5.0);   /* the 880 Hz region alone */
    CHECK_NEAR(db(program_rms(f, 4), program_rms(f, 5)), 6.02, 0.1);
    fa_soundfont_close(f);
    free(lins.d); free(lrgn.d); free(cdl.d); free(extra.d);
}

/* ---- song lengths ------------------------------------------------------------------- */

/* The length ends where the sound dies away: after a long release, at the end of the
 * track when the sound ended before it, 10 s after the last event for a note never
 * ended; the reverb's tail counts. */
static void test_length(void)
{
    buf lins = { 0 }, lrgn = { 0 }, art = { 0 }, conns = { 0 }, f;
    fa_soundfont* slow;
    fa_frames len;
    song s = { 0 };
    /* A 2 s release (DLS: 1200 timecents). */
    conn(&conns, SRC_NONE, SRC_NONE, DST_EG1_RELEASE, 0, 1200.0);
    conn(&conns, SRC_NONE, SRC_NONE, DST_EG1_SUSTAIN, 0, 1000.0);
    lar2(&art, &conns, 2);
    dls_region(&lrgn, 0, 0, NULL);
    dls_instrument(&lins, 0, &lrgn, 1, &art);
    slow = open_dls(&lins, 1);
    CHECK(slow);

    /* Dry (CC91 0): the release alone. */
    cc(&s, 0, 91, 0);
    note(&s, 0, 500, 69);
    f = finish(&s);
    len = length_of(&f, slow);
    CHECK(len > (fa_frames)(1.7 * RATE) && len < 3 * RATE);
    {
        float* x = render_from(&f, slow, 0);   /* 2 s: the release still sounds at the end */
        CHECK(x);
        CHECK(rms(x, 1.9, 2.0) > 1e-4);
        free(x);
    }
    /* The sine font's 125 ms release: the song ends soon after the note. */
    len = length_of(&f, g_font);
    CHECK(len > RATE / 2 && len < (fa_frames)(0.65 * RATE));
    free(f.d);
    /* The same, the track ending at 3 s: there. */
    cc(&s, 0, 91, 0);
    note(&s, 0, 500, 69);
    cc(&s, 3000, 7, 100);
    f = finish(&s);
    CHECK_EQ(length_of(&f, g_font), 3 * RATE);
    free(f.d);
    /* A note never ended: 10 s after the last event. */
    cc(&s, 0, 91, 0);
    ev3(&s, 0, 0x90, 69, 127);
    cc(&s, 500, 7, 100);
    f = finish(&s);
    CHECK_EQ(length_of(&f, g_font), RATE / 2 + 10 * RATE);
    free(f.d);
    /* The reverb's tail (CC91 127) rings on after the dry sound. */
    cc(&s, 0, 91, 127);
    note(&s, 0, 500, 69);
    f = finish(&s);
    CHECK(length_of(&f, g_font) > RATE);
    free(f.d);
    fa_soundfont_close(slow);
    free(lins.d); free(lrgn.d); free(art.d); free(conns.d);
}

/* ---- the OS's bank -------------------------------------------------------------------- */

/* gm.dls on the Windows runners, gs_instruments.dls on the macOS ones; skipped where there
 * is none. Loads, lists its presets, plays a piano note and a drum, and stacks under a bank. */
static void test_system_bank(void)
{
    fa_soundfont *sys = NULL, *st = NULL;
    fa_soundfont_info info;
    fa_soundfont_preset p;
    fa_soundfont_stack_entry e[2];
    struct timespec t0, t1;
    song s = { 0 };
    buf f;
    float* x;
    fa_result r;
    uint32_t i, drums = 0;
    timespec_get(&t0, TIME_UTC);
    r = fa_soundfont_open_system(&sys);
    timespec_get(&t1, TIME_UTC);
    if (r == FA_ERROR_NOT_FOUND) { printf("  no system bank here: skipped\n"); return; }
    CHECK_EQ(r, FA_OK);
    CHECK_EQ(fa_soundfont_get_info(sys, &info), FA_OK);
    printf("  \"%s\": %u presets, %.1f MB of samples, loaded in %.0f ms\n", info.name, info.preset_count,
           info.sample_memory / 1048576.0, (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
    CHECK(info.preset_count >= 128);
    CHECK(info.sample_memory > 1000000);
    for (i = 0; i < info.preset_count; i++) {
        CHECK_EQ(fa_soundfont_get_preset(sys, i, &p), FA_OK);
        if (p.bank == 128) drums++;
    }
    CHECK(drums >= 1);
    /* A piano note (program 0), a snare on channel 10. */
    ev2(&s, 0, 0xC0, 0);
    note(&s, 0, 800, 60);
    ev3(&s, 1000, 0x99, 38, 120);
    ev3(&s, 1200, 0x89, 38, 0);
    f = finish(&s);
    x = render_from(&f, sys, 0);
    CHECK(x);
    if (x) {
        CHECK(rms(x, 0.05, 0.6) > 1e-3);
        CHECK(rms(x, 1.0, 1.3) > 1e-3);
        free(x);
    }
    CHECK(length_of(&f, sys) > (fa_frames)(1.2 * RATE));
    /* Under a bank of our own: our program 0 wins, the bank's drums still play. */
    {
        static const sfpreset one[] = { { 0, 0, 0 } };
        fa_soundfont* mine = open_presets(one, 1);
        fa_soundfont_stack_entry_init(&e[0], mine);
        fa_soundfont_stack_entry_init(&e[1], sys);
        CHECK_EQ(fa_soundfont_open_stack(e, 2, &st), FA_OK);
        fa_soundfont_close(mine);
        CHECK_NEAR(program_cents(st, 0), 0.0, 5.0);
        x = render_from(&f, st, 0);
        CHECK(x);
        if (x) { CHECK(rms(x, 1.0, 1.3) > 1e-3); free(x); }
        fa_soundfont_close(st);
    }
    free(f.d);
    fa_soundfont_close(sys);
}

int main(void)
{
    fa_engine_config cfg;
    buf sf = make_sf2(NULL, 0, NULL, 0);
#if defined(__APPLE__)
    /* The iOS simulator starts programs elsewhere: tools/ios-sim/run.sh names the folder. */
    if (getenv("FA_TEST_CWD") && chdir(getenv("FA_TEST_CWD")) != 0) return 1;
#endif
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    cfg.worker_threads = 1;
    if (fa_engine_create(&cfg, &g_engine) != FA_OK) { fprintf(stderr, "engine create failed\n"); return 1; }
    if (fa_soundfont_open_memory(sf.d, sf.n, &g_font) != FA_OK) { fprintf(stderr, "test soundfont rejected\n"); return 1; }
    free(sf.d);
    RUN(test_stack);
    RUN(test_dls_connections);
    RUN(test_dls_conditions_and_layers);
    RUN(test_length);
    RUN(test_system_bank);
    fa_soundfont_close(g_font);
    fa_engine_destroy(g_engine);
    if (g_failures) { fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    printf("all passed\n");
    return 0;
}
