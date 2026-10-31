/* The third MIDI effect: the SC-88's delay (GS, CC94 sends to it) and the XG
 * variation effect (CC94 in system mode, one part's whole sound in insertion
 * mode), written for FatzAudio. midi_fx.c routes it; this file is the effect.
 *
 * The XG types are built from a few shared blocks:
 *  - delays (Delay L,C,R / L,R, Echo, Cross Delay, Karaoke 1-3, the GS delay):
 *    two delay lines, up to four taps out of them and a feedback path into each,
 *    with a lowpass in the feedback ("high damp") and filters on the way in;
 *  - early reflections and gate reverbs: the same lines with up to 16 taps in a
 *    fixed pattern the room size stretches;
 *  - the hall, room, stage and plate types: a reverb of their own, and the
 *    chorus, celeste, flanger and symphonic types a chorus of their own (a nested
 *    midi_fx.c, so they sound as the system effects of the same type do);
 *  - LFO types: tremolo, auto pan, rotary speaker (doppler and level), phaser
 *    (a chain of swept first-order allpasses);
 *  - distortion, overdrive and amp simulator (a drive into a soft/hard clipper,
 *    a lowpass and EQ), 3-band and 2-band EQ (eq.c's RBJ sections), auto wah (a
 *    swept state-variable bandpass).
 * Parameters are taken as the XG tables number them (the two-byte ones as one
 * value). The defaults and the curves (LFO frequency, delay offset, room size,
 * drive) are our reading of Yamaha's published tables, not copied from a module.
 * Types newer than the MU80's list (pitch change, exciter, compressor, ...) are
 * not built: the variation is then off (insertion: the part plays dry).
 *
 * Deterministic as midi_fx.c: the output depends only on the input and settings
 * since the last reset, and the LFOs only on the song frame. Once the input has
 * been silent and nothing at -140 dB or more is left in the state, it is cleared
 * and processing stops, so silence is exact and costs nothing. */
#include "fa_internal.h"
#include "mixer.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define VAR_LINE_S 1.0          /* the longest delay (GS: 1 s; XG: 715 ms) */
#define VAR_TAPS 16
#define VAR_STAGES 8            /* phaser allpasses */
#define VAR_QUIET 1e-7f         /* -140 dB */
#define VAR_FILTER_TAIL 8192    /* frames a filter or LFO type is run on after its input stops */
#define VAR_FB_MAX 0.98f
#define VAR_SWEEP_STEP 16       /* frames between new coefficients of the swept filters */

enum {
    FAM_OFF, FAM_THRU, FAM_REVERB, FAM_CHORUS, FAM_GS_DELAY, FAM_LCR, FAM_LR, FAM_ECHO, FAM_CROSS, FAM_KARAOKE,
    FAM_ER, FAM_GATE, FAM_RGATE, FAM_ROTARY, FAM_TREMOLO, FAM_AUTOPAN, FAM_PHASER, FAM_DIST, FAM_OD, FAM_AMP,
    FAM_EQ3, FAM_EQ2, FAM_WAH
};
enum { IN_STEREO, IN_MONO, IN_LEFT, IN_RIGHT };

/* XG variation types (MSB, LSB) and their default parameters 1-16. An LSB not
 * listed plays as LSB 0 of its MSB, as on the modules. */
typedef struct var_type { uint8_t msb, lsb, family; int16_t p[16]; } var_type;
static const var_type var_types[] = {
    { 0x01, 0, FAM_REVERB, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 64 } },       /* Hall 1, 2 */
    { 0x01, 1, FAM_REVERB, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 64 } },
    { 0x02, 0, FAM_REVERB, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 64 } },       /* Room 1-3 */
    { 0x02, 1, FAM_REVERB, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 64 } },
    { 0x02, 2, FAM_REVERB, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 64 } },
    { 0x03, 0, FAM_REVERB, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 64 } },       /* Stage 1, 2 */
    { 0x03, 1, FAM_REVERB, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 64 } },
    { 0x04, 0, FAM_REVERB, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 64 } },       /* Plate */
    { 0x05, 0, FAM_LCR, { 3333, 1667, 5000, 5000, 74, 100, 10, 0, 0, 64 } },
    { 0x06, 0, FAM_LR, { 2500, 3750, 2500, 3750, 87, 10, 0, 0, 0, 64 } },
    { 0x07, 0, FAM_ECHO, { 1700, 80, 1780, 80, 10, 1700, 1780, 0, 0, 64 } },
    { 0x08, 0, FAM_CROSS, { 1700, 1750, 111, 2, 10, 0, 0, 0, 0, 64 } },
    { 0x09, 0, FAM_ER, { 0, 19, 5, 16, 64, 0, 46, 0, 0, 64 } },       /* ER 1 (S-Hall), ER 2 (L-Hall) */
    { 0x09, 1, FAM_ER, { 1, 25, 7, 16, 64, 0, 46, 0, 0, 64 } },
    { 0x0A, 0, FAM_GATE, { 0, 20, 10, 0, 64, 0, 46, 0, 0, 64 } },     /* Gate Reverb */
    { 0x0B, 0, FAM_RGATE, { 1, 19, 10, 0, 64, 0, 46, 0, 0, 64 } },    /* Reverse Gate */
    { 0x10, 0, FAM_REVERB, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 64 } },       /* White Room, Tunnel, Canyon, Basement */
    { 0x11, 0, FAM_REVERB, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 64 } },
    { 0x12, 0, FAM_REVERB, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 64 } },
    { 0x13, 0, FAM_REVERB, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 64 } },
    { 0x14, 0, FAM_KARAOKE, { 63, 97, 0, 46, 0, 0, 0, 0, 0, 64 } },   /* Karaoke 1-3 */
    { 0x14, 1, FAM_KARAOKE, { 55, 105, 0, 42, 0, 0, 0, 0, 0, 64 } },
    { 0x14, 2, FAM_KARAOKE, { 43, 110, 14, 38, 0, 0, 0, 0, 0, 64 } },
    { 0x40, 0, FAM_THRU, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 64 } },
    { 0x41, 0, FAM_CHORUS, { 6, 54, 77, 106, 0, 0, 0, 0, 0, 64 } },   /* Chorus 1-4 */
    { 0x41, 1, FAM_CHORUS, { 8, 63, 64, 30, 0, 0, 0, 0, 0, 64 } },
    { 0x41, 2, FAM_CHORUS, { 4, 44, 64, 110, 0, 0, 0, 0, 0, 64 } },
    { 0x41, 8, FAM_CHORUS, { 9, 32, 69, 104, 0, 0, 0, 0, 0, 64 } },
    { 0x42, 0, FAM_CHORUS, { 12, 32, 64, 0, 0, 0, 0, 0, 0, 64 } },    /* Celeste 1-3 */
    { 0x42, 1, FAM_CHORUS, { 28, 18, 90, 2, 0, 0, 0, 0, 0, 64 } },
    { 0x42, 2, FAM_CHORUS, { 4, 63, 44, 2, 0, 0, 0, 0, 0, 64 } },
    { 0x43, 0, FAM_CHORUS, { 14, 14, 104, 2, 0, 0, 0, 0, 0, 64 } },   /* Flanger 1-3 */
    { 0x43, 1, FAM_CHORUS, { 32, 17, 26, 2, 0, 0, 0, 0, 0, 64 } },
    { 0x43, 8, FAM_CHORUS, { 4, 109, 109, 2, 0, 0, 0, 0, 0, 64 } },
    { 0x44, 0, FAM_CHORUS, { 12, 25, 16, 0, 0, 0, 0, 0, 0, 64 } },    /* Symphonic: no feedback, p3 = delay offset */
    { 0x45, 0, FAM_ROTARY, { 81, 35, 0, 0, 0, 0, 0, 0, 0, 64 } },
    { 0x46, 0, FAM_TREMOLO, { 83, 56, 0, 0, 0, 0, 0, 0, 0, 64 } },
    { 0x47, 0, FAM_AUTOPAN, { 76, 80, 32, 5, 0, 0, 0, 0, 0, 64 } },
    { 0x48, 0, FAM_PHASER, { 8, 111, 74, 104, 0, 0, 0, 0, 0, 64 } },  /* Phaser 1, 2 */
    { 0x48, 1, FAM_PHASER, { 8, 111, 74, 108, 0, 0, 0, 0, 0, 64 } },
    { 0x49, 0, FAM_DIST, { 40, 20, 72, 53, 48, 0, 43, 74, 10, 127, 120 } },
    { 0x4A, 0, FAM_OD, { 29, 24, 68, 45, 55, 0, 41, 72, 10, 127, 104 } },
    { 0x4B, 0, FAM_AMP, { 39, 1, 48, 55, 0, 0, 0, 0, 0, 127, 112 } },
    { 0x4C, 0, FAM_EQ3, { 70, 34, 60, 10, 70, 28, 46, 0, 0, 64 } },
    { 0x4D, 0, FAM_EQ2, { 28, 70, 46, 70, 0, 0, 0, 0, 0, 64 } },
    { 0x4E, 0, FAM_WAH, { 70, 56, 39, 25, 0, 0, 0, 0, 0, 127 } },
};

/* GS delay macros (40 01 50): pre-LPF, time C, ratio L, ratio R, level C, L, R,
 * level, feedback, send to reverb, as the parameters 51-5A take them. */
static const uint8_t gs_delay_macros[10][10] = {
    { 0, 97, 1, 1, 127, 0, 0, 64, 80, 0 },        /* Delay 1 */
    { 0, 106, 1, 1, 127, 0, 0, 64, 80, 0 },       /* Delay 2 */
    { 0, 115, 1, 1, 127, 0, 0, 64, 72, 0 },       /* Delay 3 */
    { 0, 83, 1, 1, 127, 0, 0, 64, 72, 0 },        /* Delay 4 */
    { 0, 105, 12, 24, 0, 125, 60, 64, 74, 0 },    /* Pan Delay 1 */
    { 0, 109, 12, 24, 0, 125, 60, 64, 71, 0 },    /* Pan Delay 2 */
    { 0, 115, 12, 24, 0, 120, 64, 64, 73, 0 },    /* Pan Delay 3 */
    { 0, 93, 12, 24, 0, 120, 64, 64, 72, 0 },     /* Pan Delay 4 */
    { 0, 105, 1, 1, 127, 0, 0, 64, 70, 64 },      /* Delay to Reverb */
    { 0, 110, 12, 24, 97, 127, 67, 64, 46, 0 },   /* Pan Repeat */
};

typedef struct var_tap { uint8_t line; int time; float gl, gr; } var_tap;

struct fa_midi_var {
    uint32_t rate;
    int family, msb, lsb;
    int p[16];                  /* XG parameters 1-16 */
    uint8_t gs[11];             /* GS delay 40 01 50-5A */
    /* Delays, reflections, rotary: the lines (stereo, made when first needed). */
    float* line[2];
    int cap, pos, nlines, in_mode, ntaps;
    var_tap tap[VAR_TAPS];
    int fb_from[2], fb_time[2];
    float fb_gain[2], damp, pre_a, hp_a;
    float damp_z[2], pre_z[2], hp_z[2];
    int tail;                   /* frames the state lasts after the input stops */
    fa_frames soft;             /* frames since something at -140 dB or more went in or came out */
    int idle;
    fa_midi_fx* sub;            /* the reverb and chorus types */
    /* LFO, drive and filter types */
    double hz;
    float depth, depth2, base, fb, level, drive, edge;
    int stages;
    float ap[2][VAR_STAGES], last[2], svf[2][2];
    float sweep[2];             /* the swept filters' coefficient per side (phaser), or tuning (wah) */
    biquad sec[4];
    float z[4][2][2];
    int nsec;
};

static float lowpass1(float x, float a, float* z) { return a > 0.0f ? (*z = x * (1.0f - a) + *z * a) : x; }

/* ---- XG and GS tables --------------------------------------------------------- */

double fa_xg_lfo_hz(int v)
{
    /* 0.042 Hz a step up to 64 (2.69 Hz), then the step doubles every 12 values. */
    double step = 2.69 / 64.0, hz;
    int i;
    v = v < 0 ? 0 : v > 127 ? 127 : v;
    if (v <= 64) return v * step;
    hz = 2.69;
    step *= 2.0;
    for (i = 64; i < v; i++) {
        hz += step;
        if ((i - 64) % 12 == 11) step *= 2.0;
    }
    return hz > 39.7 ? 39.7 : hz;
}

double fa_xg_mod_delay_ms(int v) { return (v < 0 ? 0 : v > 127 ? 127 : v) * 0.1; }

double fa_xg_mod_depth_ms(int v) { return (v < 0 ? 0 : v > 127 ? 127 : v) * (5.0 / 127.0); }

float fa_xg_feedback(int v)
{
    float g = (float)(v - 64) / 64.0f;
    return g < -VAR_FB_MAX ? -VAR_FB_MAX : g > VAR_FB_MAX ? VAR_FB_MAX : g;
}

/* EQ frequency (table #3): 20 Hz to 20 kHz in sixth-octave steps (0..60). */
static double eq_hz(int v) { return 20.0 * pow(2.0, (v < 0 ? 0 : v > 60 ? 60 : v) / 6.0); }
static double eq_db(int v) { return v < 52 ? -12.0 : v > 76 ? 12.0 : v - 64; }

/* GS delay time center (40 01 52): 0.1 to 1000 ms, in steps that grow with the time. */
static double gs_delay_ms(int v)
{
    static const struct { int from; double ms, step; } seg[] = {
        { 0x01, 0.1, 0.1 }, { 0x14, 2.0, 0.2 }, { 0x23, 5.0, 0.5 }, { 0x2D, 10.0, 1.0 }, { 0x37, 20.0, 2.0 },
        { 0x46, 50.0, 5.0 }, { 0x50, 100.0, 10.0 }, { 0x5A, 200.0, 20.0 }, { 0x69, 500.0, 50.0 },
    };
    int i = 8;
    if (v < 1) v = 1;
    if (v > 0x73) v = 0x73;
    while (v < seg[i].from) i--;
    return seg[i].ms + (v - seg[i].from) * seg[i].step;
}

/* ---- set up ------------------------------------------------------------------- */

fa_midi_var* fa_midi_var_new(uint32_t rate)
{
    fa_midi_var* v = (fa_midi_var*)calloc(1, sizeof(*v));
    if (!v) return NULL;
    v->rate = rate;
    v->cap = (int)(VAR_LINE_S * rate) + 2;
    v->idle = 1;
    return v;
}

void fa_midi_var_free(fa_midi_var* v)
{
    if (!v) return;
    free(v->line[0]);
    fa_midi_fx_free(v->sub);
    free(v);
}

void fa_midi_var_reset(fa_midi_var* v)
{
    if (!v) return;
    if (v->line[0]) memset(v->line[0], 0, 2 * (size_t)v->cap * sizeof(float));
    v->pos = 0;
    memset(v->damp_z, 0, sizeof(v->damp_z));
    memset(v->pre_z, 0, sizeof(v->pre_z));
    memset(v->hp_z, 0, sizeof(v->hp_z));
    memset(v->ap, 0, sizeof(v->ap));
    memset(v->last, 0, sizeof(v->last));
    memset(v->svf, 0, sizeof(v->svf));
    memset(v->z, 0, sizeof(v->z));
    v->soft = 0;
    v->idle = 1;
    if (v->sub) fa_midi_fx_reset(v->sub);
}

/* The lines, made the first time a type that uses them has something to play. */
static int make_lines(fa_midi_var* v)
{
    if (v->line[0]) return 1;
    v->line[0] = (float*)calloc(2 * (size_t)v->cap, sizeof(float));
    if (!v->line[0]) return 0;
    v->line[1] = v->line[0] + v->cap;
    return 1;
}

static int frames_of(const fa_midi_var* v, double ms)
{
    int n = (int)(ms * v->rate / 1000.0 + 0.5);
    return n < 1 ? 1 : n >= v->cap ? v->cap - 1 : n;
}

static void add_tap(fa_midi_var* v, int line, double ms, float gl, float gr)
{
    var_tap* t;
    if (v->ntaps == VAR_TAPS || (gl == 0.0f && gr == 0.0f)) return;
    t = &v->tap[v->ntaps++];
    t->line = (uint8_t)line;
    t->time = frames_of(v, ms);
    t->gl = gl;
    t->gr = gr;
}

static void set_fb(fa_midi_var* v, int k, int from, double ms, float gain)
{
    v->fb_from[k] = from;
    v->fb_time[k] = frames_of(v, ms);
    v->fb_gain[k] = gain;
}

/* High damp 1-10 (0.1 to 1.0: how much of the highs a repeat keeps) as the lowpass in the feedback. */
static float high_damp(int v) { return (1.0f - (float)(v < 1 ? 1 : v > 10 ? 10 : v) / 10.0f) * 0.85f; }

/* A one-pole coefficient for a cutoff (0: none). */
static float pole(const fa_midi_var* v, double hz)
{
    return hz <= 0.0 || hz >= 0.45 * v->rate ? 0.0f : (float)exp(-2.0 * FA_PI * hz / v->rate);
}

static biquad section(const fa_midi_var* v, int type, double hz, double q, double db)
{
    fa_eq_bandstate b;
    biquad s;
    memset(&b, 0, sizeof(b));
    b.type = type;
    b.f = (float)hz;
    b.q = (float)q;
    b.g = (float)db;
    fa_eq_design(&b, v->rate);
    s.b0 = b.b0; s.b1 = b.b1; s.b2 = b.b2; s.a1 = b.a1; s.a2 = b.a2;
    return s;
}

/* Early reflection patterns (time 0..1 of the span, level): S-Hall, L-Hall, Random,
 * Reverse, Plate, Spring. */
static const float er_time[6][12] = {
    { 0.00f, 0.07f, 0.13f, 0.21f, 0.28f, 0.36f, 0.45f, 0.53f, 0.64f, 0.75f, 0.87f, 1.00f },
    { 0.00f, 0.09f, 0.16f, 0.25f, 0.33f, 0.41f, 0.50f, 0.58f, 0.69f, 0.79f, 0.89f, 1.00f },
    { 0.00f, 0.11f, 0.17f, 0.31f, 0.38f, 0.52f, 0.56f, 0.69f, 0.77f, 0.83f, 0.94f, 1.00f },
    { 0.00f, 0.07f, 0.13f, 0.21f, 0.28f, 0.36f, 0.45f, 0.53f, 0.64f, 0.75f, 0.87f, 1.00f },
    { 0.00f, 0.03f, 0.06f, 0.10f, 0.14f, 0.19f, 0.25f, 0.32f, 0.40f, 0.50f, 0.62f, 0.75f },
    { 0.00f, 0.09f, 0.18f, 0.27f, 0.36f, 0.45f, 0.55f, 0.64f, 0.73f, 0.82f, 0.91f, 1.00f },
};

/* The reflection and gate types: taps over a span the room size (0.1-7.0) sets. */
static void taps_update(fa_midi_var* v)
{
    const int* p = v->p;
    double size = 0.1 + (p[1] < 0 ? 0 : p[1] > 44 ? 44 : p[1]) * (6.9 / 44.0);
    double span = size * (v->family == FAM_ER ? 25.0 : 40.0), pre = fa_xg_mod_delay_ms(p[3]) + 1.0;
    int kind = p[0] < 0 || p[0] > 5 ? 0 : p[0], n = v->family == FAM_ER ? 12 : 16, i;
    float norm = 0.0f, g[VAR_TAPS];
    for (i = 0; i < n; i++) {
        if (v->family == FAM_ER) g[i] = kind == 3 ? 0.3f + 0.7f * i / 11.0f : 1.0f - 0.7f * i / 11.0f;
        else if (v->family == FAM_RGATE) g[i] = 0.2f + 0.8f * i / 15.0f;
        else g[i] = kind == 1 && (i < 3 || i > 12) ? 0.6f : 1.0f;   /* Type B: softer ends */
        norm += g[i] * g[i];
    }
    norm = 0.8f / sqrtf(norm);
    for (i = 0; i < n; i++) {
        double t = pre + span * (v->family == FAM_ER ? er_time[kind][i] * (kind == 1 ? 1.6 : 1.0) : i / 15.0);
        float a = g[i] * norm;
        add_tap(v, 0, t, i % 2 ? 0.4f * a : a, i % 2 ? a : 0.4f * a);
    }
    set_fb(v, 0, 0, pre + span, fa_xg_feedback(p[4]) * 0.5f);
    v->pre_a = pole(v, p[6] >= 34 && p[6] < 60 ? eq_hz(p[6]) : 0.0);
    v->hp_a = pole(v, p[5] > 0 && p[5] <= 52 ? eq_hz(p[5]) : 0.0);
}

/* Turns the parameters into the block's settings. */
static void update(fa_midi_var* v)
{
    const int* p = v->p;
    int i;
    v->ntaps = 0;
    v->nlines = 1;
    v->in_mode = IN_MONO;
    v->fb_gain[0] = v->fb_gain[1] = 0.0f;
    v->fb_time[0] = v->fb_time[1] = 1;
    v->fb_from[0] = 0;
    v->fb_from[1] = 1;
    v->damp = v->pre_a = v->hp_a = 0.0f;
    v->tail = VAR_FILTER_TAIL;
    v->nsec = 0;
    switch (v->family) {
    case FAM_REVERB:
        if (!v->sub && !(v->sub = fa_midi_fx_new(v->rate))) { v->family = FAM_OFF; return; }
        fa_midi_fx_xg(v->sub, 0x20, 0);           /* the reverb alone */
        fa_midi_fx_xg(v->sub, 0x00, v->msb);
        fa_midi_fx_xg(v->sub, 0x01, v->lsb);
        if (p[0] > 0) fa_midi_fx_xg(v->sub, 0x02, p[0]);
        return;
    case FAM_CHORUS:
        if (!v->sub && !(v->sub = fa_midi_fx_new(v->rate))) { v->family = FAM_OFF; return; }
        fa_midi_fx_xg(v->sub, 0x00, 0);           /* the chorus alone */
        fa_midi_fx_xg(v->sub, 0x20, v->msb == 0x44 ? 0x41 : v->msb);
        fa_midi_fx_xg(v->sub, 0x21, v->lsb);
        fa_midi_fx_xg(v->sub, 0x22, p[0]);
        fa_midi_fx_xg(v->sub, 0x23, p[1]);
        fa_midi_fx_xg(v->sub, 0x24, v->msb == 0x44 ? 64 : p[2]);
        fa_midi_fx_xg(v->sub, 0x25, v->msb == 0x44 ? p[2] : p[3]);
        return;
    case FAM_ROTARY: case FAM_TREMOLO: case FAM_AUTOPAN: case FAM_PHASER: case FAM_WAH:
        v->hz = fa_xg_lfo_hz(p[0]);
        v->depth = (float)p[1] / 127.0f;
        if (v->family == FAM_PHASER) {
            v->base = (float)(100.0 * pow(2.0, p[2] * 4.0 / 127.0));
            v->fb = fa_xg_feedback(p[3]) * 0.9f;
            v->stages = v->lsb == 1 ? 8 : 6;
        }
        if (v->family == FAM_WAH) {
            v->base = (float)(200.0 * pow(2.0, p[2] * 4.0 / 127.0));
            v->fb = 1.0f / (p[3] < 10 ? 1.0f : p[3] > 120 ? 12.0f : (float)p[3] / 10.0f);   /* 1 / resonance */
        }
        return;
    case FAM_DIST: case FAM_OD: case FAM_AMP: {
        int amp = v->family == FAM_AMP;
        v->drive = (float)pow(10.0, p[0] / 127.0 * 2.5);
        v->edge = (float)p[10] / 127.0f * (v->family == FAM_OD ? 0.5f : 1.0f);
        v->level = (float)(amp ? p[3] : p[4]) / 127.0f * 0.5f;
        v->sec[v->nsec++] = fa_mixer_design(0, eq_hz(amp ? p[2] : p[3]), v->rate);
        if (amp) {
            static const double cab[4] = { 0.0, 3500.0, 2800.0, 2200.0 };   /* off, stack, combo, tube */
            if (p[1] > 0 && p[1] < 4) v->sec[v->nsec++] = fa_mixer_design(0, cab[p[1]], v->rate);
        } else {
            v->sec[v->nsec++] = section(v, FA_EQ_LOW_SHELF, eq_hz(p[1]), 0.7071, eq_db(p[2]));
            v->sec[v->nsec++] = section(v, FA_EQ_PEAK, eq_hz(p[6]), p[8] < 10 ? 1.0 : p[8] / 10.0, eq_db(p[7]));
        }
        return;
    }
    case FAM_EQ3:
        v->sec[v->nsec++] = section(v, FA_EQ_LOW_SHELF, eq_hz(p[5]), 0.7071, eq_db(p[0]));
        v->sec[v->nsec++] = section(v, FA_EQ_PEAK, eq_hz(p[1]), p[3] < 10 ? 1.0 : p[3] / 10.0, eq_db(p[2]));
        v->sec[v->nsec++] = section(v, FA_EQ_HIGH_SHELF, eq_hz(p[6]), 0.7071, eq_db(p[4]));
        return;
    case FAM_EQ2:
        v->sec[v->nsec++] = section(v, FA_EQ_LOW_SHELF, eq_hz(p[0]), 0.7071, eq_db(p[1]));
        v->sec[v->nsec++] = section(v, FA_EQ_HIGH_SHELF, eq_hz(p[2]), 0.7071, eq_db(p[3]));
        return;
    case FAM_OFF: case FAM_THRU:
        return;
    default:
        break;
    }
    /* The line types. */
    switch (v->family) {
    case FAM_GS_DELAY: {
        const uint8_t* g = v->gs;
        double c = gs_delay_ms(g[2]);
        float lc = g[5] / 127.0f;
        if (c > VAR_LINE_S * 1000.0) c = VAR_LINE_S * 1000.0;
        add_tap(v, 0, c, lc, lc);
        add_tap(v, 0, c * (g[3] ? g[3] : 1) / 24.0, g[6] / 127.0f, 0.0f);
        add_tap(v, 0, c * (g[4] ? g[4] : 1) / 24.0, 0.0f, g[7] / 127.0f);
        set_fb(v, 0, 0, c, fa_xg_feedback(g[9]));
        v->pre_a = (float)(g[1] > 7 ? 7 : g[1]) * 0.1f;
        break;
    }
    case FAM_LCR:
        add_tap(v, 0, p[0] / 10.0, 1.0f, 0.0f);
        add_tap(v, 0, p[1] / 10.0, 0.0f, 1.0f);
        add_tap(v, 0, p[2] / 10.0, p[5] / 127.0f, p[5] / 127.0f);
        set_fb(v, 0, 0, p[3] / 10.0, fa_xg_feedback(p[4]));
        v->damp = high_damp(p[6]);
        break;
    case FAM_LR:
        v->in_mode = IN_STEREO;
        v->nlines = 2;
        add_tap(v, 0, p[0] / 10.0, 1.0f, 0.0f);
        add_tap(v, 1, p[1] / 10.0, 0.0f, 1.0f);
        set_fb(v, 0, 0, p[2] / 10.0, fa_xg_feedback(p[4]));
        set_fb(v, 1, 1, p[3] / 10.0, fa_xg_feedback(p[4]));
        v->damp = high_damp(p[5]);
        break;
    case FAM_ECHO:
        v->in_mode = IN_STEREO;
        v->nlines = 2;
        add_tap(v, 0, p[0] / 10.0, 1.0f, 0.0f);
        add_tap(v, 1, p[2] / 10.0, 0.0f, 1.0f);
        add_tap(v, 0, p[5] / 10.0, p[7] / 127.0f, 0.0f);
        add_tap(v, 1, p[6] / 10.0, 0.0f, p[7] / 127.0f);
        set_fb(v, 0, 0, p[0] / 10.0, fa_xg_feedback(p[1]));
        set_fb(v, 1, 1, p[2] / 10.0, fa_xg_feedback(p[3]));
        v->damp = high_damp(p[4]);
        break;
    case FAM_CROSS:
        /* Line 0 carries the left input to the right, line 1 the right to the left; each
         * repeat crosses back. Input select: left, right, both. */
        v->in_mode = p[3] == 0 ? IN_LEFT : p[3] == 1 ? IN_RIGHT : IN_STEREO;
        v->nlines = 2;
        add_tap(v, 0, p[0] / 10.0, 0.0f, 1.0f);
        add_tap(v, 1, p[1] / 10.0, 1.0f, 0.0f);
        set_fb(v, 0, 1, p[1] / 10.0, fa_xg_feedback(p[2]));
        set_fb(v, 1, 0, p[0] / 10.0, fa_xg_feedback(p[2]));
        v->damp = high_damp(p[4]);
        break;
    case FAM_KARAOKE: {
        double t = (p[0] + 1) * 3.0;   /* about 3 ms a step */
        add_tap(v, 0, t, 1.0f, 1.0f);
        set_fb(v, 0, 0, t, fa_xg_feedback(p[1]));
        v->hp_a = pole(v, p[2] > 0 && p[2] <= 52 ? eq_hz(p[2]) : 0.0);
        v->pre_a = pole(v, p[3] >= 34 && p[3] < 60 ? eq_hz(p[3]) : 0.0);
        break;
    }
    case FAM_ER: case FAM_GATE: case FAM_RGATE:
        taps_update(v);
        break;
    default:
        break;
    }
    v->tail = 1;
    for (i = 0; i < v->ntaps; i++) if (v->tap[i].time > v->tail) v->tail = v->tap[i].time;
    for (i = 0; i < v->nlines; i++) if (v->fb_time[i] > v->tail) v->tail = v->fb_time[i];
    if (v->pos >= v->cap) v->pos = 0;
}

int fa_midi_var_set_type(fa_midi_var* v, int msb, int lsb)
{
    const var_type* t = NULL;
    size_t i;
    if (!v) return 0;
    for (i = 0; i < sizeof(var_types) / sizeof(var_types[0]); i++) {
        if (var_types[i].msb != msb) continue;
        if (var_types[i].lsb == lsb) { t = &var_types[i]; break; }
        if (var_types[i].lsb == 0 && !t) t = &var_types[i];
    }
    fa_midi_var_reset(v);
    v->msb = msb;
    v->lsb = t ? t->lsb : 0;
    v->family = t ? t->family : FAM_OFF;
    for (i = 0; i < 16; i++) v->p[i] = t ? t->p[i] : 0;
    update(v);
    return v->family != FAM_OFF;
}

void fa_midi_var_set_param(fa_midi_var* v, int index, int value, int part)
{
    if (!v || index < 0 || index >= 16 || v->family == FAM_GS_DELAY) return;
    if (part == 1) v->p[index] = (value & 0x7F) << 7 | (v->p[index] & 0x7F);
    else if (part == 2) v->p[index] = (v->p[index] & ~0x7F) | (value & 0x7F);
    else v->p[index] = value & 0x7F;
    update(v);
}

int fa_midi_var_dry_wet(const fa_midi_var* v)
{
    return v && v->family != FAM_GS_DELAY ? v->p[9] : 64;
}

void fa_midi_var_gs(fa_midi_var* v, int addr, int value)
{
    int i;
    if (!v || addr < 0x50 || addr > 0x5A) return;
    value &= 0x7F;
    if (v->family != FAM_GS_DELAY) {
        fa_midi_var_reset(v);
        v->family = FAM_GS_DELAY;
        if (addr != 0x50) fa_midi_var_gs(v, 0x50, 0);
    }
    if (addr == 0x50) {
        if (value > 9) return;
        v->gs[0] = (uint8_t)value;
        for (i = 0; i < 10; i++) v->gs[1 + i] = gs_delay_macros[value][i];
    } else {
        v->gs[addr - 0x50] = (uint8_t)value;
    }
    update(v);
}

int fa_midi_var_gs_get(const fa_midi_var* v, int addr)
{
    return v && v->family == FAM_GS_DELAY && addr >= 0x50 && addr <= 0x5A ? v->gs[addr - 0x50] : 0;
}

/* ---- processing --------------------------------------------------------------- */

static float peak_of(const float* x, int n)
{
    float p = 0.0f;
    int i;
    for (i = 0; i < n; i++) if (fabsf(x[i]) > p) p = fabsf(x[i]);
    return p;
}

static float rd(const fa_midi_var* v, int line, int time)
{
    int i = v->pos - time;
    return v->line[line][i < 0 ? i + v->cap : i];
}

static float rd_frac(const fa_midi_var* v, int line, double time)
{
    double r = v->pos - time;
    int i0, i1;
    float fr;
    while (r < 0) r += v->cap;
    i0 = (int)r;
    fr = (float)(r - i0);
    if (i0 >= v->cap) i0 -= v->cap;
    i1 = i0 + 1 == v->cap ? 0 : i0 + 1;
    return v->line[line][i0] + (v->line[line][i1] - v->line[line][i0]) * fr;
}

/* The line types: returns the loudest value written into the lines. */
static float run_lines(fa_midi_var* v, const float* in, float* wet, int frames)
{
    float loud = 0.0f;
    int f, k, t;
    for (f = 0; f < frames; f++) {
        float x[2], ol = 0.0f, orr = 0.0f, fbv[2];
        float l = in[f * 2], r = in[f * 2 + 1];
        switch (v->in_mode) {
        case IN_STEREO: x[0] = l; x[1] = r; break;
        case IN_LEFT: x[0] = l; x[1] = 0.0f; break;
        case IN_RIGHT: x[0] = 0.0f; x[1] = r; break;
        default: x[0] = x[1] = (l + r) * 0.5f; break;
        }
        for (t = 0; t < v->ntaps; t++) {
            float s = rd(v, v->tap[t].line, v->tap[t].time);
            ol += s * v->tap[t].gl;
            orr += s * v->tap[t].gr;
        }
        for (k = 0; k < v->nlines; k++) fbv[k] = v->fb_gain[k] != 0.0f ? rd(v, v->fb_from[k], v->fb_time[k]) : 0.0f;
        for (k = 0; k < v->nlines; k++) {
            float w = lowpass1(x[k], v->pre_a, &v->pre_z[k]);
            if (v->hp_a > 0.0f) w -= lowpass1(w, v->hp_a, &v->hp_z[k]);
            if (v->fb_gain[k] != 0.0f) w += lowpass1(fbv[k], v->damp, &v->damp_z[k]) * v->fb_gain[k];
            v->line[k][v->pos] = w;
            if (fabsf(w) > loud) loud = fabsf(w);
        }
        if (++v->pos == v->cap) v->pos = 0;
        wet[f * 2] = ol;
        wet[f * 2 + 1] = orr;
    }
    return loud;
}

static void run_lfo(fa_midi_var* v, const float* in, float* wet, int frames, fa_frames song_frame)
{
    double inc = v->hz / v->rate, ph = fmod((double)song_frame * inc, 1.0);
    int f, c, s;
    for (f = 0; f < frames; f++, ph += inc) {
        float l = in[f * 2], r = in[f * 2 + 1];
        float sn = (float)sin(2.0 * FA_PI * ph);
        if (ph >= 1.0) ph -= 1.0;
        switch (v->family) {
        case FAM_TREMOLO: {
            float g = 1.0f - v->depth * (0.5f - 0.5f * sn);
            wet[f * 2] = l * g;
            wet[f * 2 + 1] = r * g;
            break;
        }
        case FAM_AUTOPAN:
            wet[f * 2] = l * (1.0f - v->depth * (0.5f + 0.5f * sn));
            wet[f * 2 + 1] = r * (1.0f - v->depth * (0.5f - 0.5f * sn));
            break;
        case FAM_ROTARY: {
            /* The horn turning: its distance (a short delay, doppler) and level swing,
             * opposite on the two sides. */
            double base = 2.0 * v->rate / 1000.0, d = v->depth * 1.0 * v->rate / 1000.0;
            v->line[0][v->pos] = (l + r) * 0.5f;
            wet[f * 2] = rd_frac(v, 0, base + d * sn) * (1.0f + 0.5f * v->depth * sn);
            wet[f * 2 + 1] = rd_frac(v, 0, base - d * sn) * (1.0f - 0.5f * v->depth * sn);
            if (++v->pos == v->cap) v->pos = 0;
            break;
        }
        case FAM_PHASER:
            for (c = 0; c < 2; c++) {
                float a, x = (c ? r : l), y;
                if (f == 0 || (song_frame + f) % VAR_SWEEP_STEP == 0) {
                    /* Left and right a quarter turn apart; the allpasses swept over 4 octaves at full depth. */
                    float lfo = c ? (float)cos(2.0 * FA_PI * ph) : sn;
                    double fc = v->base * pow(2.0, 4.0 * v->depth * (0.5 + 0.5 * lfo)), tn;
                    if (fc > 0.45 * v->rate) fc = 0.45 * v->rate;
                    tn = tan(FA_PI * fc / v->rate);
                    v->sweep[c] = (float)((tn - 1.0) / (tn + 1.0));
                }
                a = v->sweep[c];
                y = x + v->last[c] * v->fb;
                for (s = 0; s < v->stages; s++) {
                    float o = a * y + v->ap[c][s];
                    v->ap[c][s] = y - a * o;
                    y = o;
                }
                v->last[c] = y;
                wet[f * 2 + c] = 0.5f * (x + y);
            }
            break;
        default: {   /* FAM_WAH */
            float fq;
            if (f == 0 || (song_frame + f) % VAR_SWEEP_STEP == 0) {
                double fc = v->base * pow(2.0, 2.0 * v->depth * sn);
                if (fc > v->rate / 7.0) fc = v->rate / 7.0;
                v->sweep[0] = (float)(2.0 * sin(FA_PI * fc / v->rate));
            }
            fq = v->sweep[0];
            for (c = 0; c < 2; c++) {
                float* st = v->svf[c], x = c ? r : l, hp;
                st[0] += fq * st[1];
                hp = x - st[0] - v->fb * st[1];
                st[1] += fq * hp;
                wet[f * 2 + c] = st[1] * v->fb * 2.0f;
            }
            break;
        }
        }
    }
}

static void run_filters(fa_midi_var* v, const float* in, float* wet, int frames)
{
    int f, c, s, shaped = v->family == FAM_DIST || v->family == FAM_OD || v->family == FAM_AMP;
    for (f = 0; f < frames; f++) {
        for (c = 0; c < 2; c++) {
            float x = in[f * 2 + c];
            if (shaped) {
                float d = x * v->drive, soft = tanhf(d), hard = d > 1.0f ? 1.0f : d < -1.0f ? -1.0f : d;
                x = soft + v->edge * (hard - soft);
            }
            for (s = 0; s < v->nsec; s++) x = fa_biquad_run(&v->sec[s], v->z[s][c], x);
            wet[f * 2 + c] = shaped ? x * v->level : x;
        }
    }
}

int fa_midi_var_process(fa_midi_var* v, const float* in, float* wet, int frames, fa_frames song_frame)
{
    float loud;
    int quiet;
    if (!v || frames <= 0 || v->family == FAM_OFF) return 0;
    if (v->family == FAM_REVERB || v->family == FAM_CHORUS) {
        /* The nested effect keeps its own silence. */
        memset(wet, 0, (size_t)frames * 2 * sizeof(float));
        if (v->family == FAM_REVERB) fa_midi_fx_reverb(v->sub, in, wet, frames);
        else fa_midi_fx_chorus(v->sub, in, wet, frames, song_frame);
        return !fa_all_zero(wet, (uint32_t)frames * 2);
    }
    quiet = fa_all_zero(in, (uint32_t)frames * 2);
    if (quiet && v->idle) return 0;
    if ((v->family >= FAM_GS_DELAY && v->family <= FAM_ROTARY) && !make_lines(v)) return 0;   /* no memory: silent */
    v->idle = 0;
    loud = quiet ? 0.0f : peak_of(in, frames * 2);
    switch (v->family) {
    case FAM_THRU:
        memcpy(wet, in, (size_t)frames * 2 * sizeof(float));
        break;
    case FAM_ROTARY: case FAM_TREMOLO: case FAM_AUTOPAN: case FAM_PHASER: case FAM_WAH:
        run_lfo(v, in, wet, frames, song_frame);
        break;
    case FAM_DIST: case FAM_OD: case FAM_AMP: case FAM_EQ3: case FAM_EQ2:
        run_filters(v, in, wet, frames);
        break;
    default: {
        float w = run_lines(v, in, wet, frames);
        if (w > loud) loud = w;
        break;
    }
    }
    {
        float out = peak_of(wet, frames * 2);
        if (out > loud) loud = out;
    }
    v->soft = loud >= VAR_QUIET ? 0 : v->soft + frames;
    if (v->soft > v->tail) fa_midi_var_reset(v);
    return 1;
}
