/* MIDI controllers and SoundFont modulators: the mod wheel and pressure vibrato (with
 * RPN 5 and the GS reset), the sound controllers CC71-78 and the GS/XG part NRPNs and
 * SysEx that set them, portamento, sostenuto, the soft pedal, a soundfont's own
 * modulators, resets and seeks. Each test renders a sine soundfont built here and
 * measures what changed: pitch per cycle, level, filter response, envelope times. */
#include "midi_synth_test.h"

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
