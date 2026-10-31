/* MIDI scale tuning and drum setup: GS and XG scale tuning, GS drum map and XG drum setup
 * edits and the drum NRPNs (pitch, level, pan, filter, exclusive groups, note-off and
 * note-on reception), GS drum map 2, resets, seeks and the XG SFX kit fallback. Each test
 * renders the sine soundfont (drum channels play it too: it has no drum kit) and
 * measures pitch, level and pan. */
#include "midi_synth_test.h"

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

static void gs_reset(song* s) { gs(s, 0, 0x40, 0x00, 0x7F, 0x00); }
static void xg_reset(song* s) { xg(s, 0, 0x00, 0x00, 0x7E, 0x00); }

/* A note on channel `ch` (0-based). */
static void note_on_ch(song* s, uint32_t on_ms, uint32_t off_ms, unsigned ch, unsigned key)
{
    ev3(s, on_ms, 0x90 | ch, key, 127);
    ev3(s, off_ms, 0x80 | ch, key, 0);
}

static void nrpn(song* s, uint32_t ms, unsigned ch, unsigned msb, unsigned lsb, unsigned v)
{
    ev3(s, ms, 0xB0 | ch, 99, msb);
    ev3(s, ms, 0xB0 | ch, 98, lsb);
    ev3(s, ms, 0xB0 | ch, 6, v);
}

/* The pitch of the note between from_s and to_s, in cents against 440 Hz (its middle). */
static double cents(const float* x, double from_s, double to_s)
{
    track t = pitch_track(x, from_s, to_s, 440.0);
    return (t.lo + t.hi) / 2;
}

/* The RMS of the right channel. */
static double rms_right(const float* x, double from_s, double to_s) { return rms(x + 1, from_s, to_s); }

/* The power of both channels together, as RMS (the sqrt pan law keeps it whatever the pan). */
static double rms_both(const float* x, double from_s, double to_s)
{
    double l = rms(x, from_s, to_s), r = rms_right(x, from_s, to_s);
    return sqrt(l * l + r * r);
}

/* Drum keys 69 (0 to 1.5 s) and 71 (0.5 to 1.5 s). */
static void two_drums(song* s)
{
    ev3(s, 0, 0x99, 69, 127);
    ev3(s, 500, 0x99, 71, 127);
    ev3(s, 1500, 0x89, 69, 0);
    ev3(s, 1500, 0x89, 71, 0);
}

/* ---- tests ------------------------------------------------------------------------ */

/* GS (40 1x 40-4B) and XG (08 nn 41-4C) scale tuning move each note of the octave by
 * -64..+63 cents; other notes stay; a change retunes the notes sounding. */
static void test_scale_tuning(void)
{
    song s = { 0 };
    buf gs_a, gs_b, xg_a, live;
    float *a, *b, *c, *d;
    track t;
    gs_reset(&s); gs(&s, 0, 0x40, 0x11, 0x49, 64 + 30);   /* part 1, A */
    note(&s, 0, 1500, 81);
    gs_a = finish(&s);
    gs_reset(&s); gs(&s, 0, 0x40, 0x11, 0x49, 64 + 30);
    note(&s, 0, 1500, 71);
    gs_b = finish(&s);
    xg_reset(&s); xg(&s, 0, 0x08, 0x00, 0x4A, 64 - 20);   /* part 1, A */
    note(&s, 0, 1500, 69);
    xg_a = finish(&s);
    gs_reset(&s);
    ev3(&s, 0, 0x90, 69, 127);
    gs(&s, 500, 0x40, 0x11, 0x49, 64 + 40);
    ev3(&s, 1500, 0x80, 69, 0);
    live = finish(&s);
    a = render(&gs_a); b = render(&gs_b); c = render(&xg_a); d = render(&live);
    CHECK(a && b && c && d);
    CHECK_NEAR(cents(a, 0.1, 1.4), 1200.0 + 30.0, 1.0);
    t = pitch_track(b, 0.1, 1.4, 493.883);
    CHECK_NEAR((t.lo + t.hi) / 2, 0.0, 1.0);
    CHECK_NEAR(cents(c, 0.1, 1.4), -20.0, 1.0);
    CHECK_NEAR(cents(d, 0.1, 0.45), 0.0, 1.0);
    CHECK_NEAR(cents(d, 0.55, 1.4), 40.0, 1.0);
    free(a); free(b); free(c); free(d); free(gs_a.d); free(gs_b.d); free(xg_a.d); free(live.d);
}

/* GS drum map (41 0p rr), XG drum setup (30 rr pp) and the drum NRPNs set a drum key's
 * pitch, level, pan and filter; a melodic channel ignores the NRPNs. */
static void test_drum_edits(void)
{
    song s = { 0 };
    buf plain, gs_edit, xg_pitch, nrpn_edit, xg_ref, xg_filter, melodic;
    float *a, *b, *c, *d, *e, *g, *h;
    double level = 40.0 * log10(64.0 / 127.0), cutoff = 13500 - 64 * 100;
    gs_reset(&s);
    note_on_ch(&s, 0, 1500, 9, 69);
    plain = finish(&s);
    gs_reset(&s);
    gs(&s, 0, 0x41, 0x01, 69, 71);   /* play note number: two semitones up */
    gs(&s, 0, 0x41, 0x02, 69, 64);   /* level */
    gs(&s, 0, 0x41, 0x04, 69, 1);    /* pan: left */
    note_on_ch(&s, 0, 1500, 9, 69);
    gs_edit = finish(&s);
    xg_reset(&s);
    xg(&s, 0, 0x30, 69, 0x00, 65);   /* pitch coarse +1 */
    xg(&s, 0, 0x30, 69, 0x01, 74);   /* pitch fine +10 */
    note_on_ch(&s, 0, 1500, 9, 69);
    xg_pitch = finish(&s);
    gs_reset(&s);
    nrpn(&s, 0, 9, 0x18, 69, 66);   /* pitch coarse +2 */
    nrpn(&s, 0, 9, 0x1A, 69, 64);   /* level */
    note_on_ch(&s, 0, 1500, 9, 69);
    nrpn_edit = finish(&s);
    xg_reset(&s);
    note_on_ch(&s, 0, 1500, 9, 81);
    xg_ref = finish(&s);
    xg_reset(&s);
    xg(&s, 0, 0x30, 81, 0x0B, 0);   /* filter cutoff, as CC74 = 0 */
    note_on_ch(&s, 0, 1500, 9, 81);
    xg_filter = finish(&s);
    gs_reset(&s);
    nrpn(&s, 0, 0, 0x18, 69, 66);
    note(&s, 0, 1500, 69);
    melodic = finish(&s);
    a = render(&plain); b = render(&gs_edit); c = render(&xg_pitch); d = render(&nrpn_edit);
    e = render(&xg_ref); g = render(&xg_filter); h = render(&melodic);
    CHECK(a && b && c && d && e && g && h);
    CHECK_NEAR(cents(a, 0.1, 1.4), 0.0, 1.0);
    CHECK_NEAR(cents(b, 0.1, 1.4), 200.0, 1.0);
    CHECK_NEAR(db(rms_both(b, 0.2, 1.4), rms_both(a, 0.2, 1.4)), level, 0.1);
    CHECK(rms_right(b, 0.2, 1.4) < rms(b, 0.2, 1.4) * 0.01);
    CHECK_NEAR(cents(c, 0.1, 1.4), 110.0, 1.0);
    CHECK_NEAR(cents(d, 0.1, 1.4), 200.0, 1.0);
    CHECK_NEAR(db(rms_both(d, 0.2, 1.4), rms_both(a, 0.2, 1.4)), level, 0.1);
    CHECK_NEAR(db(rms(g, 0.2, 1.4), rms(e, 0.2, 1.4)), lowpass_db(cutoff, 0, 880.0) - lowpass_db(13500, 0, 880.0), 0.3);
    CHECK_NEAR(cents(h, 0.1, 1.4), 0.0, 1.0);
    free(a); free(b); free(c); free(d); free(e); free(g); free(h);
    free(plain.d); free(gs_edit.d); free(xg_pitch.d); free(nrpn_edit.d); free(xg_ref.d); free(xg_filter.d); free(melodic.d);
}

/* A part made a drum part with GS drum map 2 (40 1x 15 = 2) plays map 2's edits; channel
 * 10 keeps map 1's. */
static void test_drum_map_2(void)
{
    song s = { 0 };
    buf f;
    float* a;
    gs_reset(&s);
    gs(&s, 0, 0x40, 0x11, 0x15, 2);
    gs(&s, 0, 0x41, 0x01, 69, 71);   /* map 1: +2 semitones */
    gs(&s, 0, 0x41, 0x11, 69, 74);   /* map 2: +5 */
    note(&s, 0, 600, 69);
    note_on_ch(&s, 800, 1500, 9, 69);
    f = finish(&s);
    a = render(&f);
    CHECK(a);
    CHECK_NEAR(cents(a, 0.1, 0.55), 500.0, 1.0);
    CHECK_NEAR(cents(a, 0.9, 1.4), 200.0, 1.0);
    free(a); free(f.d);
}

/* Keys put in one exclusive group (GS assign group, XG alternate group) cut each other;
 * a key set not to receive note-offs rings on (not a note already sounding when it is
 * set), one not to receive note-ons is silent. */
static void test_groups_and_reception(void)
{
    song s = { 0 };
    buf grouped, xg_grouped, alone, apart, no_off, with_off, no_on, late;
    float *a, *b, *c, *d, *e, *g, *h, *k;
    double steady;
    gs_reset(&s);
    gs(&s, 0, 0x41, 0x03, 69, 1); gs(&s, 0, 0x41, 0x03, 71, 1);
    two_drums(&s);
    grouped = finish(&s);
    xg_reset(&s);
    xg(&s, 0, 0x30, 69, 0x03, 5); xg(&s, 0, 0x30, 71, 0x03, 5);
    two_drums(&s);
    xg_grouped = finish(&s);
    gs_reset(&s);
    note_on_ch(&s, 500, 1500, 9, 71);
    alone = finish(&s);
    gs_reset(&s);
    two_drums(&s);
    apart = finish(&s);
    gs_reset(&s);
    gs(&s, 0, 0x41, 0x07, 69, 0);
    note_on_ch(&s, 0, 300, 9, 69);
    no_off = finish(&s);
    gs_reset(&s);
    note_on_ch(&s, 0, 300, 9, 69);
    with_off = finish(&s);
    gs_reset(&s);
    gs(&s, 0, 0x41, 0x08, 69, 0);
    note_on_ch(&s, 0, 1500, 9, 69);
    no_on = finish(&s);
    gs_reset(&s);
    ev3(&s, 0, 0x99, 69, 127);
    gs(&s, 100, 0x41, 0x07, 69, 0);   /* too late for the note sounding */
    ev3(&s, 300, 0x89, 69, 0);
    late = finish(&s);
    a = render(&grouped); b = render(&xg_grouped); c = render(&alone); d = render(&apart);
    e = render(&no_off); g = render(&with_off); h = render(&no_on); k = render(&late);
    CHECK(a && b && c && d && e && g && h && k);
    steady = rms(c, 0.6, 1.4);
    CHECK(steady > 0.01);
    CHECK_NEAR(db(rms(a, 0.6, 1.4), steady), 0.0, 0.01);   /* 69 was cut: 71 alone */
    CHECK_NEAR(db(rms(b, 0.6, 1.4), steady), 0.0, 0.01);
    CHECK(fabs(db(rms(d, 0.6, 1.4), steady)) > 1.0);        /* without the group, both sound */
    CHECK_NEAR(db(rms(e, 0.6, 1.2), rms(e, 0.1, 0.25)), 0.0, 0.01);   /* the song ends 1 s after the note-off */
    CHECK(rms(g, 0.6, 1.2) < rms(g, 0.1, 0.25) * 1e-4);
    CHECK(rms(h, 0.0, 1.5) == 0.0);
    CHECK(rms(k, 0.6, 1.2) < rms(k, 0.1, 0.25) * 1e-4);
    free(a); free(b); free(c); free(d); free(e); free(g); free(h); free(k);
    free(grouped.d); free(xg_grouped.d); free(alone.d); free(apart.d); free(no_off.d); free(with_off.d); free(no_on.d); free(late.d);
}

/* GS and XG resets clear scale tuning and drum edits; the XG drum setup reset (00 00 7D)
 * clears one setup. */
static void test_resets(void)
{
    song s = { 0 };
    buf gs_f, xg_f, setup_f;
    float *a, *b, *c;
    gs_reset(&s);
    gs(&s, 0, 0x40, 0x11, 0x49, 64 + 30);
    gs(&s, 0, 0x41, 0x01, 69, 71);
    gs_reset(&s);
    note(&s, 0, 600, 69);
    note_on_ch(&s, 800, 1500, 9, 69);
    gs_f = finish(&s);
    xg_reset(&s);
    xg(&s, 0, 0x08, 0x00, 0x4A, 64 - 20);
    xg(&s, 0, 0x30, 69, 0x00, 66);
    xg_reset(&s);
    note(&s, 0, 600, 69);
    note_on_ch(&s, 800, 1500, 9, 69);
    xg_f = finish(&s);
    xg_reset(&s);
    xg(&s, 0, 0x30, 69, 0x00, 66);
    xg(&s, 0, 0x00, 0x00, 0x7D, 0);
    note_on_ch(&s, 0, 1500, 9, 69);
    setup_f = finish(&s);
    a = render(&gs_f); b = render(&xg_f); c = render(&setup_f);
    CHECK(a && b && c);
    CHECK_NEAR(cents(a, 0.1, 0.55), 0.0, 1.0);
    CHECK_NEAR(cents(a, 0.9, 1.4), 0.0, 1.0);
    CHECK_NEAR(cents(b, 0.1, 0.55), 0.0, 1.0);
    CHECK_NEAR(cents(b, 0.9, 1.4), 0.0, 1.0);
    CHECK_NEAR(cents(c, 0.1, 1.4), 0.0, 1.0);
    free(a); free(b); free(c); free(gs_f.d); free(xg_f.d); free(setup_f.d);
}

/* A seek replays scale tuning and drum edits: what follows is what playing through gives. */
static void test_seek(void)
{
    song s = { 0 };
    buf f;
    float *a, *b;
    fa_frames i, from = RATE / 2;
    gs_reset(&s);
    gs(&s, 0, 0x40, 0x11, 0x49, 64 + 30);
    gs(&s, 0, 0x41, 0x01, 69, 71);
    nrpn(&s, 0, 9, 0x1A, 69, 90);
    ev3(&s, 1000, 0x90, 69, 127);
    ev3(&s, 1000, 0x99, 69, 127);
    ev3(&s, 1800, 0x80, 69, 0);
    ev3(&s, 1800, 0x89, 69, 0);
    f = finish(&s);
    a = render(&f);
    b = render_from(&f, g_font, from);
    CHECK(a && b);
    CHECK(rms(a, 1.2, 1.7) > 0.01);
    for (i = 0; i < (fa_frames)(SECONDS * RATE - from) * 2; i++) CHECK(b[i] == a[from * 2 + i]);
    free(a); free(b); free(f.d);
}

/* XG SFX kits (bank 126) come from the soundfont's bank 126, else the GS SFX kit (drum
 * kit 56), else the standard kit. Coarse tunes tell the presets apart. */
static void test_sfx_kit(void)
{
    static const sfpreset all[] = { { 128, 0, 0 }, { 128, 56, 7 }, { 126, 0, 12 } };
    buf f1 = make_sf2_presets(all, 3, NULL, 0, NULL, 0), f2 = make_sf2_presets(all, 2, NULL, 0, NULL, 0);
    buf f3 = make_sf2_presets(all, 1, NULL, 0, NULL, 0), song_f;
    fa_soundfont *s1 = NULL, *s2 = NULL, *s3 = NULL;
    song s = { 0 };
    float *a, *b, *c;
    xg_reset(&s);
    cc(&s, 0, 0, 126);
    ev2(&s, 0, 0xC0, 0);
    note(&s, 0, 1500, 69);
    song_f = finish(&s);
    CHECK(fa_soundfont_open_memory(f1.d, f1.n, &s1) == FA_OK);
    CHECK(fa_soundfont_open_memory(f2.d, f2.n, &s2) == FA_OK);
    CHECK(fa_soundfont_open_memory(f3.d, f3.n, &s3) == FA_OK);
    a = render_from(&song_f, s1, 0); b = render_from(&song_f, s2, 0); c = render_from(&song_f, s3, 0);
    CHECK(a && b && c);
    CHECK_NEAR(cents(a, 0.1, 1.4), 1200.0, 1.0);
    CHECK_NEAR(cents(b, 0.1, 1.4), 700.0, 1.0);
    CHECK_NEAR(cents(c, 0.1, 1.4), 0.0, 1.0);
    free(a); free(b); free(c);
    fa_soundfont_close(s1); fa_soundfont_close(s2); fa_soundfont_close(s3);
    free(f1.d); free(f2.d); free(f3.d); free(song_f.d);
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
    RUN(test_scale_tuning);
    RUN(test_drum_edits);
    RUN(test_drum_map_2);
    RUN(test_groups_and_reception);
    RUN(test_resets);
    RUN(test_seek);
    RUN(test_sfx_kit);
    fa_soundfont_close(g_font);
    fa_engine_destroy(g_engine);
    if (g_failures) { fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    printf("all passed\n");
    return 0;
}
