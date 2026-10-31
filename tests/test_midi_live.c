/* MIDI events (extras step 15): the real-time MIDI stream, events sent to MIDI
 * streams, streams made from event lists, a file's events read back, channel values
 * and track volume. White-box for the device test (it plays the audio thread and
 * looks at the decode-ahead); the rest goes through the public API with the sine
 * soundfont of midi_synth_test.h. */
#include "fa_internal.h"
#include "midi_synth_test.h"

/* A real-time stream, decode-only, effects off: what the tests send is all it plays. */
static fa_stream* open_live(void)
{
    fa_open_options o;
    fa_midi_stream_options mo;
    fa_stream* s = NULL;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    o.soundfont = g_font;
    fa_midi_stream_options_init(&mo);
    mo.sample_rate = RATE;
    if (fa_stream_open_midi(g_engine, &mo, &o, &s) != FA_OK) return NULL;
    fa_stream_set_midi_effects(s, 0);
    return s;
}

/* Reads `frames` (stereo) in pieces of `piece` frames. */
static float* read_frames(fa_stream* s, fa_frames frames, fa_frames piece)
{
    float* x = (float*)calloc((size_t)frames * 2, sizeof(float));
    fa_frames done = 0, got;
    while (x && done < frames) {
        fa_frames want = frames - done < piece ? frames - done : piece;
        if (fa_stream_read(s, x + done * 2, want, &got) != FA_OK || !got) break;
        done += got;
    }
    return x;
}

static fa_frames length_of(fa_stream* s)
{
    fa_stream_info info;
    if (fa_stream_get_info(s, &info) != FA_OK) return -2;
    return info.length_frames;
}

static fa_frames first_sound(const float* x, fa_frames from, fa_frames to)
{
    fa_frames i;
    for (i = from; i < to; i++) if (fabsf(x[i * 2]) > 1e-6f) return i;
    return -1;
}

static fa_midi_event ev(fa_frames frame, int type, int channel, int param, int value)
{
    fa_midi_event e;
    memset(&e, 0, sizeof(e));
    e.frame = frame;
    e.type = type;
    e.channel = channel;
    e.param = param;
    e.value = value;
    return e;
}

/* Notes queued at frame offsets start at those frames, to the sample, however the
 * stream is read. */
static void test_onsets(void)
{
    fa_midi_event e[4];
    fa_stream *s = open_live(), *t = open_live();
    float *a, *b;
    fa_frames i;
    CHECK(s && t);
    e[0] = ev(1000, FA_MIDI_EVENT_NOTE_ON, 0, 69, 127);
    e[1] = ev(3000, FA_MIDI_EVENT_NOTE_OFF, 0, 69, 0);
    e[2] = ev(30001, FA_MIDI_EVENT_NOTE_ON, 3, 69, 100);
    e[3] = ev(40000, FA_MIDI_EVENT_NOTE_ON, 3, 69, 0);   /* velocity 0: a note off */
    CHECK(fa_stream_send_midi_events(s, e, 4) == FA_OK);
    CHECK(fa_stream_send_midi_events(t, e, 4) == FA_OK);
    a = read_frames(s, 60000, 4096);
    b = read_frames(t, 60000, 77);
    CHECK(a && b);
    /* The sine starts at phase 0: its first sample is 0, the next is not. */
    CHECK(first_sound(a, 0, 60000) == 1001);
    CHECK(fabsf(a[3000 * 2]) > 0.01f || fabsf(a[3001 * 2]) > 0.01f);   /* releasing */
    CHECK(first_sound(a, 20000, 60000) == 30002);
    CHECK(rms(a, 50000.0 / RATE, 60000.0 / RATE) < 1e-6);            /* released by then */
    for (i = 0; i < 60000 * 2; i++) CHECK(a[i] == b[i]);
    free(a); free(b);
    fa_stream_close(s); fa_stream_close(t);
}

/* FA_MIDI_NOW (and frames already rendered) play at the next frames rendered, in the
 * order they were sent. */
static void test_now(void)
{
    fa_stream* s = open_live();
    fa_midi_event e[2];
    float* a;
    float* b;
    fa_frames at;
    CHECK(s);
    a = read_frames(s, 5000, 5000);
    CHECK(a && first_sound(a, 0, 5000) < 0);
    e[0] = ev(10, FA_MIDI_EVENT_NOTE_ON, 0, 69, 127);           /* long past */
    e[1] = ev(FA_MIDI_NOW, FA_MIDI_EVENT_NOTE_OFF, 0, 69, 0);  /* ... and its end, at once */
    CHECK(fa_stream_send_midi_events(s, e, 1) == FA_OK);
    b = read_frames(s, 5000, 5000);
    CHECK(b);
    at = first_sound(b, 0, 5000);
    CHECK(at >= 0 && at < 64 + 2);   /* the next block after the one read into */
    CHECK(fa_stream_send_midi_events(s, e + 1, 1) == FA_OK);
    free(a);
    a = read_frames(s, 20000, 1000);
    CHECK(a && rms(a, 15000.0 / RATE, 20000.0 / RATE) < 1e-6);   /* the note off took effect */
    free(a); free(b);
    /* Sent together, a late note and a NOW note off: the off comes second, so no note hangs. */
    e[0].frame = 0;
    CHECK(fa_stream_send_midi_events(s, e, 2) == FA_OK);
    a = read_frames(s, 20000, 1000);
    CHECK(a && rms(a, 15000.0 / RATE, 20000.0 / RATE) < 1e-6);
    free(a);
    fa_stream_close(s);
}

/* Raw bytes with running status and SysEx give what the same events give. */
static void test_raw(void)
{
    static const uint8_t bytes[] = {
        0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7,   /* GM on */
        0xF8,                                  /* clock: skipped */
        0xC1, 0x00,
        0xB1, 0x07, 0x50, 0x0A, 0x20,          /* CC7 = 80, CC10 = 32 (running status) */
        0x91, 0x45, 0x7F, 0x48, 0x40,          /* two notes */
        0x55                                   /* half a message: dropped */
    };
    fa_midi_event e[6];
    static const uint8_t gm[] = { 0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7 };
    fa_stream *s = open_live(), *t = open_live();
    float *a, *b;
    fa_frames i;
    int32_t v = -1;
    CHECK(s && t);
    CHECK(fa_stream_send_midi(s, 500, bytes, sizeof(bytes)) == FA_OK);
    memset(e, 0, sizeof(e));
    e[0].frame = 500; e[0].type = FA_MIDI_EVENT_SYSEX; e[0].data = gm; e[0].size = sizeof(gm);
    e[1] = ev(500, FA_MIDI_EVENT_PROGRAM, 1, 0, 0);
    e[2] = ev(500, FA_MIDI_EVENT_CONTROL, 1, 7, 80);
    e[3] = ev(500, FA_MIDI_EVENT_CONTROL, 1, 10, 32);
    e[4] = ev(500, FA_MIDI_EVENT_NOTE_ON, 1, 69, 127);
    e[5] = ev(500, FA_MIDI_EVENT_NOTE_ON, 1, 72, 64);
    CHECK(fa_stream_send_midi_events(t, e, 6) == FA_OK);
    a = read_frames(s, 20000, 1024);
    b = read_frames(t, 20000, 1024);
    CHECK(a && b);
    CHECK(first_sound(a, 0, 20000) == 501);
    for (i = 0; i < 20000 * 2; i++) CHECK(a[i] == b[i]);
    CHECK(fa_stream_get_midi_value(s, 1, FA_MIDI_EVENT_CONTROL, 10, &v) == FA_OK && v == 32);
    free(a); free(b);
    fa_stream_close(s); fa_stream_close(t);
}

/* The values the synth has: controllers, program, bend, pressure. */
static void test_values(void)
{
    fa_stream* s = open_live();
    fa_midi_event e[4];
    int32_t v = -1;
    float* a;
    CHECK(s);
    CHECK(fa_stream_get_midi_value(s, 2, FA_MIDI_EVENT_CONTROL, 7, &v) == FA_OK && v == 100);   /* GM default */
    CHECK(fa_stream_get_midi_value(s, 2, FA_MIDI_EVENT_PITCH_BEND, 0, &v) == FA_OK && v == 8192);
    e[0] = ev(FA_MIDI_NOW, FA_MIDI_EVENT_CONTROL, 2, 7, 50);
    e[1] = ev(FA_MIDI_NOW, FA_MIDI_EVENT_PROGRAM, 2, 0, 5);
    e[2] = ev(FA_MIDI_NOW, FA_MIDI_EVENT_PITCH_BEND, 2, 0, 1000);
    e[3] = ev(FA_MIDI_NOW, FA_MIDI_EVENT_CHANNEL_PRESSURE, 2, 0, 33);
    CHECK(fa_stream_send_midi_events(s, e, 4) == FA_OK);
    a = read_frames(s, 256, 256);
    free(a);
    CHECK(fa_stream_get_midi_value(s, 2, FA_MIDI_EVENT_CONTROL, 7, &v) == FA_OK && v == 50);
    CHECK(fa_stream_get_midi_value(s, 2, FA_MIDI_EVENT_PROGRAM, 0, &v) == FA_OK && v == 5);
    CHECK(fa_stream_get_midi_value(s, 2, FA_MIDI_EVENT_PITCH_BEND, 0, &v) == FA_OK && v == 1000);
    CHECK(fa_stream_get_midi_value(s, 2, FA_MIDI_EVENT_CHANNEL_PRESSURE, 0, &v) == FA_OK && v == 33);
    CHECK(fa_stream_get_midi_value(s, 3, FA_MIDI_EVENT_CONTROL, 7, &v) == FA_OK && v == 100);   /* other channels untouched */
    CHECK(fa_stream_get_midi_value(s, 16, FA_MIDI_EVENT_CONTROL, 7, &v) == FA_ERROR_INVALID_ARG);
    CHECK(fa_stream_get_midi_value(s, 0, FA_MIDI_EVENT_NOTE_ON, 7, &v) == FA_ERROR_INVALID_ARG);
    fa_stream_close(s);
}

/* The queue: all or nothing, FA_ERROR_BUSY when full, free again once taken. */
static void test_queue(void)
{
    fa_stream* s = open_live();
    fa_midi_event* e = (fa_midi_event*)calloc(FA_MIDI_QUEUE + 1, sizeof(fa_midi_event));
    fa_midi_event bad;
    uint32_t i;
    float* a;
    CHECK(s && e);
    for (i = 0; i <= FA_MIDI_QUEUE; i++) e[i] = ev(100000, FA_MIDI_EVENT_CONTROL, 0, 1, (int)(i & 127));
    CHECK(fa_stream_send_midi_events(s, e, FA_MIDI_QUEUE + 1) == FA_ERROR_INVALID_ARG);
    CHECK(fa_stream_send_midi_events(s, e, FA_MIDI_QUEUE - 1) == FA_OK);
    CHECK(fa_stream_send_midi_events(s, e, 2) == FA_ERROR_BUSY);
    CHECK(fa_stream_send_midi_events(s, e, 1) == FA_OK);
    CHECK(fa_stream_send_midi_events(s, e, 1) == FA_ERROR_BUSY);
    a = read_frames(s, 64, 64);   /* the synth takes them (they wait in its own list) */
    free(a);
    CHECK(fa_stream_send_midi_events(s, e, FA_MIDI_QUEUE) == FA_OK);
    bad = ev(0, FA_MIDI_EVENT_NOTE_ON, 16, 60, 100);
    CHECK(fa_stream_send_midi_events(s, &bad, 1) == FA_ERROR_INVALID_ARG);
    bad = ev(-5, FA_MIDI_EVENT_NOTE_ON, 0, 60, 100);
    CHECK(fa_stream_send_midi_events(s, &bad, 1) == FA_ERROR_INVALID_ARG);
    bad = ev(0, FA_MIDI_EVENT_PITCH_BEND, 0, 0, 16384);
    CHECK(fa_stream_send_midi_events(s, &bad, 1) == FA_ERROR_INVALID_ARG);
    bad = ev(0, FA_MIDI_EVENT_SYSEX, 0, 0, 0);
    CHECK(fa_stream_send_midi_events(s, &bad, 1) == FA_ERROR_INVALID_ARG);
    free(e);
    fa_stream_close(s);   /* frees what is still queued */
}

/* A two-track format 1 file: track 0 plays key 69 from 100 to 600 ms, track 1 key 81
 * from 800 to 1300 ms, both on channel 1. */
static buf two_tracks(void)
{
    song a = { 0 }, b = { 0 };
    buf f = { 0 };
    note(&a, 100, 600, 69);
    ev3(&b, 800, 0x90, 81, 127);
    ev3(&b, 1300, 0x80, 81, 0);
    put8(&a.t, 0); put8(&a.t, 0xFF); put8(&a.t, 0x2F); put8(&a.t, 0);
    put8(&b.t, 0); put8(&b.t, 0xFF); put8(&b.t, 0x2F); put8(&b.t, 0);
    put(&f, "MThd", 4); put32be(&f, 6); put8(&f, 0); put8(&f, 1); put8(&f, 0); put8(&f, 2); put8(&f, 480 >> 8); put8(&f, 480 & 255);
    put(&f, "MTrk", 4); put32be(&f, (uint32_t)a.t.n); put(&f, a.t.d, a.t.n);
    put(&f, "MTrk", 4); put32be(&f, (uint32_t)b.t.n); put(&f, b.t.d, b.t.n);
    free(a.t.d); free(b.t.d);
    return f;
}

static fa_stream* open_file(const buf* f)
{
    fa_open_options o;
    fa_stream* s = NULL;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    o.soundfont = g_font;
    if (fa_stream_open_memory(g_engine, f->d, f->n, &o, &s) != FA_OK) return NULL;
    fa_stream_set_midi_effects(s, 0);
    return s;
}

/* Track volume scales the level of its own notes only, from the next note on. */
static void test_track_volume(void)
{
    buf f = two_tracks();
    fa_stream *s = open_file(&f), *t = open_file(&f);
    float *a, *b, v = 0;
    fa_frames i;
    CHECK(s && t);
    CHECK(fa_stream_get_midi_track_volume(t, 1, &v) == FA_OK && v == 1.0f);
    CHECK(fa_stream_set_midi_track_volume(t, 1, 0.5f) == FA_OK);
    CHECK(fa_stream_get_midi_track_volume(t, 1, &v) == FA_OK && v == 0.5f);
    CHECK(fa_stream_set_midi_track_volume(t, 2, 0.5f) == FA_ERROR_INVALID_ARG);
    CHECK(fa_stream_set_midi_track_volume(t, 0, 5.0f) == FA_ERROR_INVALID_ARG);
    a = read_frames(s, 2 * RATE, 1024);
    b = read_frames(t, 2 * RATE, 1024);
    CHECK(a && b);
    for (i = 0; i < (fa_frames)(0.75 * RATE) * 2; i++) CHECK(a[i] == b[i]);   /* track 0 as it was */
    CHECK_NEAR(db(rms(b, 0.9, 1.2), rms(a, 0.9, 1.2)), -6.0206, 0.01);
    free(a); free(b);
    /* Silent: no voice at all. A seek into the note restarts it at the track's volume. */
    CHECK(fa_stream_set_midi_track_volume(t, 1, 0.0f) == FA_OK);
    CHECK(fa_stream_set_midi_track_volume(s, 1, 0.25f) == FA_OK);
    CHECK(fa_stream_seek(t, (fa_frames)(RATE * 0.9)) == FA_OK);
    CHECK(fa_stream_seek(s, (fa_frames)(RATE * 0.9)) == FA_OK);
    a = read_frames(s, RATE / 4, 1024);
    b = read_frames(t, RATE / 4, 1024);
    CHECK(a && b);
    CHECK(rms(b, 0.0, 0.25) == 0.0);
    CHECK(rms(a, 0.05, 0.25) > 0.01);
    free(a); free(b);
    fa_stream_close(s); fa_stream_close(t);
    free(f.d);
}

/* A file's events, all or by track and type; texts and tempo are not among them. */
static void test_file_events(void)
{
    buf f = two_tracks();
    fa_stream* s = open_file(&f);
    fa_midi_event e[8];
    uint32_t n = 0;
    CHECK(s);
    CHECK(fa_stream_get_midi_events(s, -1, -1, NULL, 0, &n) == FA_OK && n == 4);
    CHECK(fa_stream_get_midi_events(s, -1, -1, e, 8, &n) == FA_OK && n == 4);
    CHECK(e[0].type == FA_MIDI_EVENT_NOTE_ON && e[0].param == 69 && e[0].value == 127 && e[0].track == 0 && e[0].channel == 0);
    CHECK(e[0].frame == (fa_frames)(0.1 * RATE + 0.5) && e[0].time == 100000000);
    CHECK(e[1].type == FA_MIDI_EVENT_NOTE_OFF && e[1].param == 69 && e[1].frame == (fa_frames)(0.6 * RATE + 0.5));
    CHECK(e[2].type == FA_MIDI_EVENT_NOTE_ON && e[2].param == 81 && e[2].track == 1);
    CHECK(e[3].type == FA_MIDI_EVENT_NOTE_OFF && e[3].track == 1);
    CHECK(fa_stream_get_midi_events(s, 1, -1, e, 8, &n) == FA_OK && n == 2 && e[0].param == 81);
    CHECK(fa_stream_get_midi_events(s, -1, FA_MIDI_EVENT_NOTE_OFF, e, 1, &n) == FA_OK && n == 2 && e[0].param == 69);
    CHECK(fa_stream_get_midi_events(s, 5, -1, e, 8, &n) == FA_OK && n == 0);
    CHECK(fa_stream_get_midi_events(s, -1, 99, e, 8, &n) == FA_ERROR_INVALID_ARG);
    fa_stream_close(s);
    free(f.d);
}

/* A list of events plays as the same events in a file, seeks and lists its events. */
static void test_event_list(void)
{
    static const uint8_t gs[] = { 0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7 };
    song so = { 0 };
    buf f;
    fa_midi_event e[8], got[8];
    fa_midi_stream_options mo;
    fa_open_options o;
    fa_stream *s = NULL, *t;
    float *a, *b, *c, *d;
    fa_frames i, len = -1;
    uint32_t n = 0;
    sysex(&so, 0, gs + 1, sizeof(gs) - 1);
    cc(&so, 50, 7, 90);
    ev3(&so, 100, 0x90, 69, 127);
    ev3(&so, 300, 0xE0, 0, 0x50);
    ev3(&so, 600, 0x80, 69, 0);
    note(&so, 650, 900, 72);
    f = finish(&so);
    memset(e, 0, sizeof(e));
    /* Given out of order: sorted, equal frames kept as given. */
    e[0] = ev((fa_frames)(0.65 * RATE + 0.5), FA_MIDI_EVENT_NOTE_ON, 0, 72, 127);
    e[1] = ev((fa_frames)(0.9 * RATE + 0.5), FA_MIDI_EVENT_NOTE_OFF, 0, 72, 0);
    e[2].frame = 0; e[2].type = FA_MIDI_EVENT_SYSEX; e[2].data = gs; e[2].size = sizeof(gs);
    e[3] = ev((fa_frames)(0.05 * RATE + 0.5), FA_MIDI_EVENT_CONTROL, 0, 7, 90);
    e[4] = ev((fa_frames)(0.1 * RATE + 0.5), FA_MIDI_EVENT_NOTE_ON, 0, 69, 127);
    e[5] = ev((fa_frames)(0.3 * RATE + 0.5), FA_MIDI_EVENT_PITCH_BEND, 0, 0, 0x50 << 7);
    e[6] = ev((fa_frames)(0.6 * RATE + 0.5), FA_MIDI_EVENT_NOTE_OFF, 0, 69, 0);
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    o.soundfont = g_font;
    fa_midi_stream_options_init(&mo);
    mo.sample_rate = RATE;
    CHECK(fa_stream_open_midi_events(g_engine, e, 7, &mo, &o, &s) == FA_OK);
    fa_stream_set_midi_effects(s, 0);
    t = open_file(&f);
    CHECK(t);
    len = length_of(s);
    CHECK(len > (fa_frames)(0.9 * RATE + 0.5) && len == length_of(t));   /* the same tail as the file's */
    a = read_frames(s, 2 * RATE, 999);
    b = read_frames(t, 2 * RATE, 1024);
    CHECK(a && b);
    CHECK(rms(a, 0.2, 0.5) > 0.01);
    for (i = 0; i < (fa_frames)(1.9 * RATE) * 2; i++) CHECK(a[i] == b[i]);
    CHECK(fa_stream_seek(s, RATE / 5) == FA_OK);
    CHECK(fa_stream_seek(t, RATE / 5) == FA_OK);
    c = read_frames(s, RATE, 1024);
    d = read_frames(t, RATE, 1024);
    CHECK(c && d);
    for (i = 0; i < RATE * 2; i++) CHECK(c[i] == d[i]);
    CHECK(fa_stream_get_midi_events(s, -1, -1, got, 8, &n) == FA_OK && n == 7);
    CHECK(got[0].type == FA_MIDI_EVENT_SYSEX && got[0].size == sizeof(gs) && memcmp(got[0].data, gs, sizeof(gs)) == 0);
    CHECK(got[1].type == FA_MIDI_EVENT_CONTROL && got[3].type == FA_MIDI_EVENT_PITCH_BEND && got[3].value == 0x50 << 7);
    CHECK(fa_stream_get_midi_events(t, -1, FA_MIDI_EVENT_SYSEX, got, 8, &n) == FA_OK && n == 1);
    CHECK(got[0].size == sizeof(gs) && memcmp(got[0].data, gs, sizeof(gs)) == 0);
    free(a); free(b); free(c); free(d);
    fa_stream_close(s); fa_stream_close(t);
    free(f.d);
}

/* Events sent to a file stream play on top of the file, at their frames. */
static void test_send_to_file(void)
{
    buf f = two_tracks();
    fa_stream *s = open_file(&f), *t = open_file(&f);
    fa_midi_event e[2];
    float *a, *b;
    fa_frames i;
    CHECK(s && t);
    e[0] = ev(RATE * 3 / 2, FA_MIDI_EVENT_NOTE_ON, 5, 69, 127);
    e[1] = ev(RATE * 7 / 4, FA_MIDI_EVENT_NOTE_OFF, 5, 69, 0);
    CHECK(fa_stream_send_midi_events(s, e, 2) == FA_OK);
    a = read_frames(s, 2 * RATE, 1024);
    b = read_frames(t, 2 * RATE, 1024);
    CHECK(a && b);
    for (i = 0; i < (fa_frames)(1.5 * RATE) * 2; i++) CHECK(a[i] == b[i]);
    CHECK(first_sound(a, RATE * 3 / 2 - 10, 2 * RATE) == RATE * 3 / 2 + 1);
    free(a); free(b);
    fa_stream_close(s); fa_stream_close(t);
    free(f.d);
}

/* Sent events queued before a seek play at its target, not in the unheard pre-roll
 * rendered before it (effects on). */
static void test_send_then_seek(void)
{
    fa_open_options o;
    fa_midi_stream_options mo;
    fa_midi_event list[1], e;
    fa_stream* s = NULL;
    float* a;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    o.soundfont = g_font;
    fa_midi_stream_options_init(&mo);
    mo.sample_rate = RATE;
    list[0] = ev(RATE * 4, FA_MIDI_EVENT_CONTROL, 0, 7, 100);
    CHECK(fa_stream_open_midi_events(g_engine, list, 1, &mo, &o, &s) == FA_OK && s);
    if (!s) return;
    e = ev(FA_MIDI_NOW, FA_MIDI_EVENT_NOTE_ON, 0, 69, 127);
    CHECK(fa_stream_send_midi_events(s, &e, 1) == FA_OK);
    CHECK(fa_stream_seek(s, RATE * 2 + 10) == FA_OK);
    a = read_frames(s, 1000, 1000);
    CHECK(a && first_sound(a, 0, 1000) == 1);   /* starts at the target (phase 0, then sound) */
    free(a);
    fa_stream_close(s);
}

static void test_open_args(void)
{
    fa_midi_stream_options mo;
    fa_open_options o;
    fa_stream* s = (fa_stream*)1;
    fa_midi_event e = ev(0, FA_MIDI_EVENT_NOTE_ON, 0, 60, 100);
    fa_midi_info info;
    uint32_t n = 9;
    fa_frames len = 0;
    fa_open_options_init(&o);
    o.soundfont = g_font;
    o.flags = FA_OPEN_LOOP;
    CHECK(fa_stream_open_midi(g_engine, NULL, &o, &s) == FA_ERROR_INVALID_ARG && s == NULL);
    o.flags = FA_OPEN_DECODE_ONLY;
    fa_midi_stream_options_init(&mo);
    mo.latency_ms = 5;
    CHECK(fa_stream_open_midi(g_engine, &mo, &o, &s) == FA_ERROR_INVALID_ARG);
    mo.latency_ms = 0;
    mo.sample_rate = 4000;
    CHECK(fa_stream_open_midi(g_engine, &mo, &o, &s) == FA_ERROR_INVALID_ARG);
    e.track = 65535;   /* one past the last track there can be */
    CHECK(fa_stream_open_midi_events(g_engine, &e, 1, NULL, &o, &s) == FA_ERROR_INVALID_ARG);
    CHECK(fa_stream_open_midi(g_engine, NULL, &o, &s) == FA_OK);
    len = length_of(s);
    CHECK(len == -1);
    CHECK(fa_stream_seek(s, 1000) != FA_OK);
    CHECK(fa_stream_get_midi_info(s, &info) == FA_OK && info.tracks == 0 && info.text_count == 0);
    CHECK(fa_stream_get_midi_events(s, -1, -1, NULL, 0, &n) == FA_OK && n == 0);
    CHECK(fa_stream_set_midi_track_volume(s, 0, 0.5f) == FA_ERROR_INVALID_ARG);
    CHECK(fa_stream_send_midi(s, FA_MIDI_NOW, "\x90\x3C\x40", 3) == FA_OK);
    fa_stream_close(s);
}

/* On a device it renders about latency_ms ahead, so a note sent now is heard that soon. */
static void test_device_latency(void)
{
    fa_open_options o;
    fa_midi_stream_options mo;
    fa_device* dev = NULL;
    fa_stream* s = NULL;
    float out[256 * 2];
    int64_t rendered = 0, sent_at = -1, heard_at = -1, most = 0, limit;
    fa_midi_event e = ev(FA_MIDI_NOW, FA_MIDI_EVENT_NOTE_ON, 0, 69, 127);
    int tries;
    uint32_t rate;
    CHECK(fa_device_open(g_engine, NULL, &dev) == FA_OK);
    fa_open_options_init(&o);
    o.soundfont = g_font;
    fa_midi_stream_options_init(&mo);
    mo.latency_ms = 20;
    CHECK(fa_stream_open_midi(g_engine, &mo, &o, &s) == FA_OK);
    fa_stream_set_midi_effects(s, 0);
    fa_stream_attach(s, dev, NULL);
    fa_stream_play(s);
    limit = (int64_t)dev->rate * 20 / 1000;
    /* This test plays the audio thread: the device was never started. */
    while (rendered < (int64_t)dev->rate && heard_at < 0) {
        int64_t k;
        for (tries = 0; tries < 2000; tries++) {
            if (fa_ring_readable(&s->ring) >= 256) break;
            fa_sleep_ms(1);
        }
        if (fa_ring_readable(&s->ring) > most) most = fa_ring_readable(&s->ring);
        if (rendered >= (int64_t)dev->rate / 4 && sent_at < 0) {
            CHECK(fa_stream_send_midi_events(s, &e, 1) == FA_OK);
            sent_at = rendered;
        }
        fa_fetch_add64(&dev->epoch, 1);
        fa_device_render(dev, out, 256);
        fa_fetch_add64(&dev->epoch, 1);
        for (k = 0; k < 256 && sent_at >= 0; k++) if (fabsf(out[k * 2]) > 1e-4f) { heard_at = rendered + k; break; }
        rendered += 256;
    }
    rate = dev->rate;
    fa_stream_close(s);
    fa_device_close(dev);
    /* The decode-ahead stays near 20 ms (a piece over it at most). */
    CHECK(most <= limit + 256);
    CHECK(sent_at >= 0 && heard_at >= sent_at);
    printf("  heard %.1f ms after sending (decode-ahead at most %.1f ms)\n",
           (double)(heard_at - sent_at) * 1000.0 / rate, (double)most * 1000.0 / rate);
    CHECK(heard_at - sent_at <= limit + (int64_t)rate * 10 / 1000);
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
    RUN(test_onsets);
    RUN(test_now);
    RUN(test_raw);
    RUN(test_values);
    RUN(test_queue);
    RUN(test_track_volume);
    RUN(test_file_events);
    RUN(test_event_list);
    RUN(test_send_to_file);
    RUN(test_send_then_seek);
    RUN(test_open_args);
    RUN(test_device_latency);
    fa_soundfont_close(g_font);
    fa_engine_destroy(g_engine);
    if (g_failures) { fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    printf("all passed\n");
    return 0;
}
