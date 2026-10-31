/* MIDI reverb and chorus: the effects the synthesizer's send buses (CC91, CC93
 * and the soundfont's effect-send generators) feed, written for FatzAudio.
 *
 * Reverb: the classic Schroeder-Moorer design, eight parallel comb filters with
 * a one-pole lowpass in their feedback (high frequencies die first, as in a
 * room) into four series allpass filters, per channel; the right channel's delay
 * lines are a little longer so the two are uncorrelated (a wide tail). Delay
 * lengths are the widely published Freeverb tunings, scaled to the rate.
 *
 * Chorus: two modulated delay taps per channel (10-16 ms, sine LFOs at 0.4 Hz in
 * quadrature between the channels), averaged, no feedback.
 *
 * The reverb's room (comb feedback), damping, input gain and wet level default to
 * the MIDI tuning below; the DJ mixer's mic reverb sets its own (fa_midi_fx_set_reverb)
 * and runs the reverb alone (fa_midi_fx_reverb).
 *
 * GS and XG effect SysEx (fa_midi_fx_gs, fa_midi_fx_xg) change the type, level and
 * time of both (and the GS reverb's pre-delay, the XG chorus's LFO, feedback and
 * delay offset): a reverb type is a decay time and damping for the same reverb (the
 * comb feedback is set so the tail falls 60 dB in that time), or, for GS Delay and
 * Panning Delay, a feedback echo instead; a chorus type sets the delay, depth, rate
 * and feedback of the same chorus (a flanger is a short delay with feedback). The
 * hardware's exact curves are not published, so the times and ranges are close
 * approximations. The defaults (and the state after any reset) are the tuning
 * above, which is GS Hall 2 and Chorus 3 at their default parameters here.
 *
 * The third effect (midi_var.c) is the SC-88's delay after a GS reset (or any GS
 * delay SysEx) and the XG variation after an XG reset (or any XG variation SysEx);
 * CC94 sends to it (fa_midi_fx_route: the GS delay, the XG variation in system
 * mode), or, in XG insertion mode, one part's sound goes through it whole
 * (fa_midi_fx_insert). Chorus can send on to the reverb and the delay, the delay
 * and the variation to the reverb (and the variation to the chorus), as the
 * hardware's effect blocks are wired. Without that SysEx, or after a GM reset,
 * there is no third effect and CC94 does nothing.
 *
 * Both are deterministic: the output depends only on the input and the settings
 * since the last reset, and the chorus LFO only on the song frame, so reading in
 * any block size gives the same samples. When the input has been silent long
 * enough for the tail to fall below -140 dB the state is cleared and processing
 * stops, so a silent stretch is exactly silent (and costs nothing), as it is
 * without effects. */
#include "fa_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FX_COMBS 8
#define FX_ALLPASSES 4
typedef char fx_combs_in_fours[FX_COMBS % 4 == 0 ? 1 : -1];   /* reverb_process runs them four at a time */
#define FX_SPREAD 23                    /* extra delay of the right channel, frames at 44.1 kHz */
#define FX_INPUT_GAIN 0.015f
#define FX_ROOM 0.84f                   /* comb feedback: about 1.3 s to -60 dB */
#define FX_ROOM_TIME 1.3f               /* that time, seconds */
#define FX_ROOM_MAX 0.985f              /* about 25 s */
#define FX_DAMP 0.25f
#define FX_ALLPASS_FEEDBACK 0.5f
#define FX_REVERB_WET 1.5f
#define FX_QUIET 1e-7f                  /* -140 dB: the tail is cleared below this */

#define FX_ECHO_MAX_S 0.45              /* GS delay time 127 */
#define FX_ECHO_MS_PER_STEP 3.3         /* GS delay time step */
#define FX_ECHO_WET 0.5f
#define FX_FEEDBACK_MAX 0.9f            /* feedback 127 */

#define FX_CHORUS_DELAY_MS 13.0         /* at GS chorus delay 80 */
#define FX_CHORUS_DEPTH_MS 3.0          /* at GS chorus depth 19 */
#define FX_CHORUS_RATE_HZ 0.4           /* at GS chorus rate 3 */
#define FX_CHORUS_MAX_DELAY_MS 51.0     /* GS chorus delay 127 */
#define FX_CHORUS_MAX_DEPTH_MS 20.0     /* GS chorus depth 127 */
#define FX_CHORUS_MAX_RATE_HZ 10.0
#define FX_CHORUS_TAPS 2
#define FX_LFO_SEED 256                 /* frames between exact values of the chorus LFO */
#define FX_PREDELAY_MAX_MS 127          /* GS reverb pre-delay 127 */
#define FX_CHUNK 64                     /* frames processed at a time where effects feed each other */

static const int comb_tuning[FX_COMBS] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
static const int allpass_tuning[FX_ALLPASSES] = { 556, 441, 341, 225 };

enum { KIND_ROOM, KIND_DELAY, KIND_PAN_DELAY, KIND_OFF };
enum { VAR_NONE, VAR_GS, VAR_XG };     /* the third effect: none, the GS delay, the XG variation */

/* Reverb types: time to -60 dB (at GS time 64; XG: the type's default time) and
 * the damping in the comb feedback. GS characters 0-7, then XG types. */
typedef struct fx_reverb_type { float time, damp; int kind; } fx_reverb_type;
static const fx_reverb_type reverb_types[] = {
    { 0.5f, 0.35f, KIND_ROOM },        /* GS Room 1 */
    { 0.8f, 0.30f, KIND_ROOM },        /* GS Room 2 */
    { 1.0f, 0.30f, KIND_ROOM },        /* GS Room 3 */
    { 1.8f, 0.20f, KIND_ROOM },        /* GS Hall 1 */
    { FX_ROOM_TIME, FX_DAMP, KIND_ROOM },  /* GS Hall 2: the default */
    { 1.2f, 0.05f, KIND_ROOM },        /* GS Plate */
    { 0.0f, 0.0f, KIND_DELAY },        /* GS Delay */
    { 0.0f, 0.0f, KIND_PAN_DELAY },    /* GS Panning Delay */
    { 2.4f, 0.20f, KIND_ROOM },        /* XG Hall 1 */
    { 1.9f, 0.25f, KIND_ROOM },        /* XG Hall 2 */
    { 0.6f, 0.35f, KIND_ROOM },        /* XG Room 1 */
    { 1.0f, 0.30f, KIND_ROOM },        /* XG Room 2 */
    { 1.4f, 0.30f, KIND_ROOM },        /* XG Room 3 */
    { 1.7f, 0.20f, KIND_ROOM },        /* XG Stage 1 */
    { 1.4f, 0.20f, KIND_ROOM },        /* XG Stage 2 */
    { 1.6f, 0.05f, KIND_ROOM },        /* XG Plate */
    { 0.4f, 0.30f, KIND_ROOM },        /* XG White Room */
    { 2.8f, 0.15f, KIND_ROOM },        /* XG Tunnel */
    { 3.5f, 0.15f, KIND_ROOM },        /* XG Canyon */
    { 1.0f, 0.40f, KIND_ROOM },        /* XG Basement */
};
enum { GS_HALL2 = 4, GS_DELAY = 6, XG_HALL1 = 8 };

/* GS chorus macros: feedback, delay, rate, depth, as the parameters below take them. */
static const uint8_t chorus_macros[8][4] = {
    { 0, 112, 3, 5 },       /* Chorus 1 */
    { 5, 80, 9, 19 },       /* Chorus 2 */
    { 8, 80, 3, 19 },       /* Chorus 3 */
    { 16, 64, 9, 16 },      /* Chorus 4 */
    { 64, 127, 2, 24 },     /* Feedback Chorus */
    { 112, 127, 1, 5 },     /* Flanger */
    { 0, 127, 0, 127 },     /* Short Delay */
    { 80, 127, 0, 127 },    /* Short Delay (FB) */
};

typedef struct fx_line { float* buf; int len, pos; float lp; } fx_line;

struct fa_midi_fx {
    uint32_t rate;
    float room, damp, in_gain, wet;     /* the reverb's tuning */
    int kind;                           /* KIND_* */
    float pre_a, pre_z;                 /* the input's lowpass (GS pre-LPF; 0: none) */
    float* pre;                         /* the pre-delay line (GS), made when first needed */
    int pre_cap, pre_len, pre_pos;
    fx_line comb[2][FX_COMBS];
    fx_line allpass[2][FX_ALLPASSES];
    int reverb_idle;
    fa_frames reverb_quiet;             /* frames since the reverb input was last non-zero */
    fa_frames reverb_span;              /* the longest path through the delay lines */
    float* echo;                        /* the delay types' line, stereo, made when first needed */
    int echo_cap, echo_len, echo_pos;
    float echo_fb;
    fa_frames echo_soft;                /* frames since a sample of -140 dB or more went into it */
    float* chorus_buf[2];
    int chorus_len, chorus_pos, chorus_idle;
    fa_frames chorus_soft;              /* frames since a sample of -140 dB or more went into the line */
    double delay_ms, depth_ms, rate_hz; /* the chorus's tuning */
    float chorus_fb, chorus_wet, chorus_pre_a;
    float chorus_z[2], chorus_last[2];  /* its input lowpass and its last output, per channel */
    double lfo_cos, lfo_sin;            /* the chorus LFO's turn in one frame */
    double lfo_s, lfo_c;                /* the LFO's sine and cosine at frame lfo_next */
    fa_frames lfo_next;                 /* -1: not running */
    fa_frames chorus_quiet;             /* frames since the chorus input was last non-zero */
    float* mem;                         /* every delay line, one allocation */

    /* GS and XG settings, as the SysEx gave them. */
    int rev_type;                       /* index in reverb_types */
    float rev_time;                     /* seconds to -60 dB */
    int rev_level, rev_pre, gs_time, gs_feedback;
    int cho_on, cho_level, cho_pre, cho_feedback, cho_delay, cho_rate, cho_depth;
    int xg_rev_msb, xg_cho_msb;
    int rev_predelay;                   /* GS 37, ms */
    int cho_to_rev, cho_to_del;         /* GS 3F and 40, XG 2E */
    int xg_cho[4];                      /* XG chorus LFO frequency, depth, feedback, delay offset (22-25); -1: the type's */

    /* The third effect. */
    fa_midi_var* var;
    int var_kind;                       /* VAR_* */
    int var_on;                         /* XG: a type midi_var.c plays */
    int var_msb, var_system, var_part, var_return, var_pan, var_to_rev, var_to_cho;
    int var_depth[4];                   /* XG 5C-5F: MW, bend, aftertouch, AC1 on the variation (insertion mode) */

    /* Insertion effects: XG insertion 1 and 2 (03 0n xx), the GS EFX (40 03 xx). */
    int ins_mode;                       /* FA_MIDI_FX_GM: none; _GS: the EFX; _XG: insertion 1 and 2 */
    fa_midi_var* ins[2];
    int ins_on[2], ins_msb[2], ins_part[2], ins_depth[2][5];
    fa_midi_var* efx;
    int efx_on, efx_send[3], efx_src[2], efx_depth[2];
    unsigned efx_parts;                 /* the parts switched through it (40 4x 22), one bit each */
    float sc_x[FX_CHUNK * 2];
    float sc_rin[FX_CHUNK * 2], sc_cin[FX_CHUNK * 2], sc_din[FX_CHUNK * 2];   /* inputs with what other effects send */
    float sc_cwet[FX_CHUNK * 2], sc_vwet[FX_CHUNK * 2];                      /* chorus and third effect outputs */
};

static int scaled(int frames, uint32_t rate)
{
    int n = (int)((double)frames * rate / 44100.0 + 0.5);
    return n < 1 ? 1 : n;
}

static void chorus_update(fa_midi_fx* fx);

fa_midi_fx* fa_midi_fx_new(uint32_t rate)
{
    fa_midi_fx* fx = (fa_midi_fx*)calloc(1, sizeof(*fx));
    size_t total = 0;
    float* p;
    int ch, i;
    if (!fx) return NULL;
    fx->rate = rate;
    fx->room = FX_ROOM;
    fx->damp = FX_DAMP;
    fx->in_gain = FX_INPUT_GAIN;
    fx->wet = FX_REVERB_WET;
    for (ch = 0; ch < 2; ch++) {
        for (i = 0; i < FX_COMBS; i++) total += (size_t)(fx->comb[ch][i].len = scaled(comb_tuning[i] + ch * FX_SPREAD, rate));
        for (i = 0; i < FX_ALLPASSES; i++) total += (size_t)(fx->allpass[ch][i].len = scaled(allpass_tuning[i] + ch * FX_SPREAD, rate));
    }
    for (ch = 0; ch < 2; ch++) {
        fa_frames span = 0;
        for (i = 0; i < FX_COMBS; i++) if (fx->comb[ch][i].len > span) span = fx->comb[ch][i].len;
        for (i = 0; i < FX_ALLPASSES; i++) span += fx->allpass[ch][i].len;
        if (span > fx->reverb_span) fx->reverb_span = span;
    }
    fx->chorus_len = (int)ceil((FX_CHORUS_MAX_DELAY_MS + FX_CHORUS_MAX_DEPTH_MS) * rate / 1000.0) + 2;
    total += 2 * (size_t)fx->chorus_len;
    fx->mem = p = (float*)calloc(total, sizeof(float));
    if (!p) { free(fx); return NULL; }
    for (ch = 0; ch < 2; ch++) {
        for (i = 0; i < FX_COMBS; i++) { fx->comb[ch][i].buf = p; p += fx->comb[ch][i].len; }
        for (i = 0; i < FX_ALLPASSES; i++) { fx->allpass[ch][i].buf = p; p += fx->allpass[ch][i].len; }
    }
    fx->chorus_buf[0] = p;
    fx->chorus_buf[1] = p + fx->chorus_len;
    fa_midi_fx_defaults(fx, FA_MIDI_FX_GM);
    fa_midi_fx_reset(fx);
    return fx;
}

void fa_midi_fx_free(fa_midi_fx* fx)
{
    if (!fx) return;
    fa_midi_var_free(fx->var);
    fa_midi_var_free(fx->ins[0]);
    fa_midi_var_free(fx->ins[1]);
    fa_midi_var_free(fx->efx);
    free(fx->pre);
    free(fx->echo);
    free(fx->mem);
    free(fx);
}

static void reverb_clear(fa_midi_fx* fx)
{
    int ch, i;
    for (ch = 0; ch < 2; ch++) {
        for (i = 0; i < FX_COMBS; i++) {
            memset(fx->comb[ch][i].buf, 0, (size_t)fx->comb[ch][i].len * sizeof(float));
            fx->comb[ch][i].pos = 0;
            fx->comb[ch][i].lp = 0.0f;
        }
        for (i = 0; i < FX_ALLPASSES; i++) {
            memset(fx->allpass[ch][i].buf, 0, (size_t)fx->allpass[ch][i].len * sizeof(float));
            fx->allpass[ch][i].pos = 0;
        }
    }
    if (fx->echo) memset(fx->echo, 0, 2 * (size_t)fx->echo_cap * sizeof(float));
    if (fx->pre) memset(fx->pre, 0, (size_t)fx->pre_cap * sizeof(float));
    fx->pre_pos = 0;
    fx->echo_pos = 0;
    fx->echo_soft = 0;
    fx->pre_z = 0.0f;
    fx->reverb_idle = 1;
    fx->reverb_quiet = 0;
}

static void chorus_clear(fa_midi_fx* fx)
{
    memset(fx->chorus_buf[0], 0, 2 * (size_t)fx->chorus_len * sizeof(float));
    fx->chorus_pos = 0;
    fx->chorus_z[0] = fx->chorus_z[1] = 0.0f;
    fx->chorus_last[0] = fx->chorus_last[1] = 0.0f;
    fx->chorus_idle = 1;
    fx->chorus_quiet = 0;
    fx->chorus_soft = 0;
}

void fa_midi_fx_reset(fa_midi_fx* fx)
{
    if (!fx) return;
    reverb_clear(fx);
    chorus_clear(fx);
    fx->lfo_next = -1;
    fa_midi_var_reset(fx->var);
    fa_midi_var_reset(fx->ins[0]);
    fa_midi_var_reset(fx->ins[1]);
    fa_midi_var_reset(fx->efx);
}

/* The one-pole lowpass of the GS pre-LPF (a: 0 is none). */
static float lowpass(float x, float a, float* z)
{
    return a > 0.0f ? (*z = x * (1.0f - a) + *z * a) : x;
}

/* The GS Delay and Panning Delay types: one echo, repeated by the feedback; the
 * panning one bounces between the channels. echo_soft counts the frames since
 * the line was last given a sample that is not below -140 dB: past its length,
 * the whole line (and so every repeat still to come) is below that. */
static void echo_process(fa_midi_fx* fx, const float* in, float* out, int frames)
{
    float wet = fx->wet * (FX_ECHO_WET / FX_REVERB_WET);
    int f, pan = fx->kind == KIND_PAN_DELAY;
    for (f = 0; f < frames; f++) {
        float* d = fx->echo + (size_t)fx->echo_pos * 2;
        float x = (in[f * 2] + in[f * 2 + 1]) * 0.5f, l = d[0], r = d[1];
        x = lowpass(x, fx->pre_a, &fx->pre_z);
        d[0] = x + (pan ? r : l) * fx->echo_fb;
        d[1] = pan ? l * fx->echo_fb : x + r * fx->echo_fb;
        fx->echo_soft = fabsf(d[0]) >= FX_QUIET || fabsf(d[1]) >= FX_QUIET ? 0 : fx->echo_soft + 1;
        if (++fx->echo_pos >= fx->echo_len) fx->echo_pos = 0;
        out[f * 2] += l * wet;
        out[f * 2 + 1] += r * wet;
    }
}

/* Four comb lines over `n` frames, adding what each gives to y in line order. Four at once so their feedback chains overlap; the
 * frames are cut where a line wraps, so the inner loop has no wrap tests. */
static void combs4(fx_line* c, const float* x, float* y, int n, float damp, float room)
{
    const float keep = 1.0f - damp;
    float l0 = c[0].lp, l1 = c[1].lp, l2 = c[2].lp, l3 = c[3].lp;
    int k = 0, j;
    while (k < n) {
        float* b0 = c[0].buf + c[0].pos; float* b1 = c[1].buf + c[1].pos;
        float* b2 = c[2].buf + c[2].pos; float* b3 = c[3].buf + c[3].pos;
        int m = n - k;
        for (j = 0; j < 4; j++) if (c[j].len - c[j].pos < m) m = c[j].len - c[j].pos;
        for (j = 0; j < m; j++) {
            float d0 = b0[j], d1 = b1[j], d2 = b2[j], d3 = b3[j], v = x[k + j];
            l0 = d0 * keep + l0 * damp;
            l1 = d1 * keep + l1 * damp;
            l2 = d2 * keep + l2 * damp;
            l3 = d3 * keep + l3 * damp;
            b0[j] = v + l0 * room;
            b1[j] = v + l1 * room;
            b2[j] = v + l2 * room;
            b3[j] = v + l3 * room;
            y[k + j] = y[k + j] + d0 + d1 + d2 + d3;
        }
        for (j = 0; j < 4; j++) if ((c[j].pos += m) == c[j].len) c[j].pos = 0;
        k += m;
    }
    c[0].lp = l0; c[1].lp = l1; c[2].lp = l2; c[3].lp = l3;
}

/* An allpass line over `n` frames, y in and out. */
static void allpass(fx_line* a, float* y, int n)
{
    int k = 0, j;
    while (k < n) {
        float* b = a->buf + a->pos;
        int m = a->len - a->pos < n - k ? a->len - a->pos : n - k;
        for (j = 0; j < m; j++) {
            float d = b[j];
            b[j] = y[k + j] + d * FX_ALLPASS_FEEDBACK;
            y[k + j] = d - y[k + j];
        }
        if ((a->pos += m) == a->len) a->pos = 0;
        k += m;
    }
}

static void reverb_process(fa_midi_fx* fx, const float* in, float* out, int frames)
{
    float peak = 0.0f;
    int quiet_in, f, ch, i;
    if (fx->kind == KIND_OFF) return;
    quiet_in = fa_all_zero(in, (uint32_t)frames * 2);
    if (quiet_in && fx->reverb_idle) return;
    fx->reverb_idle = 0;
    fx->reverb_quiet = quiet_in ? fx->reverb_quiet + frames : 0;
    if (fx->kind != KIND_ROOM) {
        echo_process(fx, in, out, frames);
        if (fx->reverb_quiet > fx->echo_len && fx->echo_soft > fx->echo_len) reverb_clear(fx);
        return;
    }
    /* A chunk at a time, each line on its own: every line still steps through the
     * frames in order and the lines' sums keep their order, so the float result is
     * the same as frame by frame, faster. */
    for (f = 0; f < frames; f += FX_CHUNK) {
        float x[FX_CHUNK], y[FX_CHUNK];
        int n = frames - f < FX_CHUNK ? frames - f : FX_CHUNK, k;
        for (k = 0; k < n; k++) {
            float v = (in[(f + k) * 2] + in[(f + k) * 2 + 1]) * fx->in_gain;
            v = lowpass(v, fx->pre_a, &fx->pre_z);
            if (fx->pre_len) {
                float d = fx->pre[fx->pre_pos];
                fx->pre[fx->pre_pos] = v;
                if (++fx->pre_pos >= fx->pre_len) fx->pre_pos = 0;
                v = d;
            }
            x[k] = v;
        }
        for (ch = 0; ch < 2; ch++) {
            for (k = 0; k < n; k++) y[k] = 0.0f;
            for (i = 0; i < FX_COMBS; i += 4) combs4(&fx->comb[ch][i], x, y, n, fx->damp, fx->room);
            for (i = 0; i < FX_ALLPASSES; i++) allpass(&fx->allpass[ch][i], y, n);
            for (k = 0; k < n; k++) {
                float v = y[k] * fx->wet;
                out[(f + k) * 2 + ch] += v;
                if (fabsf(v) > peak) peak = fabsf(v);
            }
        }
    }
    /* Cleared only once the last input has gone all the way through the delay
     * lines (the output is still silent before the first comb echo comes back). */
    if (fx->reverb_quiet > fx->reverb_span + fx->pre_len && peak < FX_QUIET) reverb_clear(fx);
}

static void chorus_process(fa_midi_fx* fx, const float* in, float* out, int frames, fa_frames song_frame)
{
    const double base = fx->delay_ms * fx->rate / 1000.0, depth = fx->depth_ms * fx->rate / 1000.0;
    const double step = fx->rate_hz / fx->rate;
    const float gain = fx->chorus_wet * (1.0f / FX_CHORUS_TAPS);
    double s, c;
    int f, ch, t, quiet_in;
    if (!fx->cho_on) return;
    quiet_in = fa_all_zero(in, (uint32_t)frames * 2);
    if (quiet_in && fx->chorus_idle) return;
    fx->chorus_idle = 0;
    fx->chorus_quiet = quiet_in ? fx->chorus_quiet + frames : 0;
    /* The LFO is turned on frame by frame from exact values at every multiple of
     * FX_LFO_SEED song frames, so each frame's value depends only on the frame
     * (not on how the song is cut into blocks), and the rounding that adds up in
     * between stays far below a float's. Away from where it left off (a seek,
     * a silent stretch, a new rate) it starts again from the last such frame. */
    if (fx->lfo_next != song_frame) {
        fa_frames g = song_frame - song_frame % FX_LFO_SEED;
        double ph = 2.0 * FA_PI * fmod((double)g * step, 1.0);
        s = sin(ph);
        c = cos(ph);
        for (; g < song_frame; g++) {
            double s1 = s * fx->lfo_cos + c * fx->lfo_sin;
            c = c * fx->lfo_cos - s * fx->lfo_sin;
            s = s1;
        }
    } else {
        s = fx->lfo_s;
        c = fx->lfo_c;
    }
    for (f = 0; f < frames; f++) {
        int w = fx->chorus_pos;
        fa_frames g = song_frame + f;
        double lfo[2], s1;
        int loud = 0;
        if (g % FX_LFO_SEED == 0) {
            double ph = 2.0 * FA_PI * fmod((double)g * step, 1.0);
            s = sin(ph);
            c = cos(ph);
        }
        /* Taps half a cycle apart (the sine's sign flips); the right channel a
         * quarter cycle on (the cosine). */
        lfo[0] = s;
        lfo[1] = c;
        s1 = s * fx->lfo_cos + c * fx->lfo_sin;
        c = c * fx->lfo_cos - s * fx->lfo_sin;
        s = s1;
        for (ch = 0; ch < 2; ch++) {
            float x = lowpass(in[f * 2 + ch], fx->chorus_pre_a, &fx->chorus_z[ch]);
            if (fx->chorus_fb != 0.0f) x += fx->chorus_last[ch] * fx->chorus_fb;
            fx->chorus_buf[ch][w] = x;
            if (fabsf(x) >= FX_QUIET) loud = 1;
        }
        fx->chorus_soft = loud ? 0 : fx->chorus_soft + 1;
        for (ch = 0; ch < 2; ch++) {
            float y = 0.0f;
            for (t = 0; t < FX_CHORUS_TAPS; t++) {
                double delay = base + depth * (t ? -lfo[ch] : lfo[ch]);
                double rp = w - delay;
                int i0, i1;
                float fr;
                while (rp < 0) rp += fx->chorus_len;
                i0 = (int)rp;
                fr = (float)(rp - i0);
                if (i0 >= fx->chorus_len) i0 -= fx->chorus_len;
                i1 = i0 + 1 == fx->chorus_len ? 0 : i0 + 1;
                y += fx->chorus_buf[ch][i0] + (fx->chorus_buf[ch][i1] - fx->chorus_buf[ch][i0]) * fr;
            }
            fx->chorus_last[ch] = y * (1.0f / FX_CHORUS_TAPS);
            out[f * 2 + ch] += y * gain;
        }
        if (++fx->chorus_pos == fx->chorus_len) fx->chorus_pos = 0;
    }
    fx->lfo_s = s;
    fx->lfo_c = c;
    fx->lfo_next = song_frame + frames;
    /* As the echo: once nothing above -140 dB has gone into the line for its whole
     * length (with feedback, its repeats have died away too). */
    if (fx->chorus_quiet > fx->chorus_len && fx->chorus_soft > fx->chorus_len) chorus_clear(fx);
}

void fa_midi_fx_set_reverb(fa_midi_fx* fx, float room, float damp, float in_gain, float wet)
{
    fx->room = room;
    fx->damp = damp;
    fx->in_gain = in_gain;
    fx->wet = wet;
}

void fa_midi_fx_reverb(fa_midi_fx* fx, const float* in, float* out, int frames)
{
    if (fx && frames > 0) reverb_process(fx, in, out, frames);
}

void fa_midi_fx_chorus(fa_midi_fx* fx, const float* in, float* out, int frames, fa_frames song_frame)
{
    if (fx && frames > 0) chorus_process(fx, in, out, frames, song_frame);
}

int fa_midi_fx_route(const fa_midi_fx* fx)
{
    if (!fx || !fx->var || fx->var_kind == VAR_NONE) return -2;
    if (fx->var_kind == VAR_GS) return -1;
    if (!fx->var_on) return -2;
    if (fx->var_system) return -1;
    return fx->var_part < 16 ? fx->var_part : -2;
}

/* `dst` = `src` (unless it is already a copy) + `add` * `g`; returns dst. */
static const float* send_into(float* dst, const float* src, const float* add, float g, int n)
{
    int i;
    if (src != dst) memcpy(dst, src, (size_t)n * sizeof(float));
    for (i = 0; i < n; i++) dst[i] += add[i] * g;
    return dst;
}

/* At most FX_CHUNK frames: the third effect and the chorus first when they send on,
 * then the reverb. The outputs are added reverb, chorus, third effect, so without
 * the sends the sum is made as it always was. */
static void process_chunk(fa_midi_fx* fx, const float* rin, const float* cin, const float* vin, float* out, int n, fa_frames frame)
{
    int route = vin ? fa_midi_fx_route(fx) : -2, i, vout = 0, cwet = 0;
    if (route == -1 && fx->var_kind == VAR_XG) {
        vout = fa_midi_var_process(fx->var, vin, fx->sc_vwet, n, frame);
        if (vout) {
            /* Return and pan (64: the middle, full level both sides). */
            float g = (float)fx->var_return / 64.0f, p = (float)(fx->var_pan < 1 ? 1 : fx->var_pan);
            float gl = g * (p <= 64.0f ? 1.0f : (127.0f - p) / 63.0f), gr = g * (p >= 64.0f ? 1.0f : (p - 1.0f) / 63.0f);
            for (i = 0; i < n; i++) { fx->sc_vwet[i * 2] *= gl; fx->sc_vwet[i * 2 + 1] *= gr; }
            if (fx->var_to_cho) cin = send_into(fx->sc_cin, cin, fx->sc_vwet, (float)fx->var_to_cho / 127.0f, n * 2);
            if (fx->var_to_rev) rin = send_into(fx->sc_rin, rin, fx->sc_vwet, (float)fx->var_to_rev / 127.0f, n * 2);
        }
    }
    if (fx->cho_on && (fx->cho_to_rev || (route == -1 && fx->var_kind == VAR_GS && fx->cho_to_del))) {
        memset(fx->sc_cwet, 0, (size_t)n * 2 * sizeof(float));
        chorus_process(fx, cin, fx->sc_cwet, n, frame);
        cwet = 1;
        if (fx->cho_to_rev) rin = send_into(fx->sc_rin, rin, fx->sc_cwet, (float)fx->cho_to_rev / 127.0f, n * 2);
    }
    if (route == -1 && fx->var_kind == VAR_GS) {
        const float* din = vin;
        if (cwet && fx->cho_to_del) din = send_into(fx->sc_din, vin, fx->sc_cwet, (float)fx->cho_to_del / 127.0f, n * 2);
        vout = fa_midi_var_process(fx->var, din, fx->sc_vwet, n, frame);
        if (vout) {
            float g = (float)fa_midi_var_gs_get(fx->var, 0x58) / 64.0f;
            int to_rev = fa_midi_var_gs_get(fx->var, 0x5A);
            for (i = 0; i < n * 2; i++) fx->sc_vwet[i] *= g;
            if (to_rev) rin = send_into(fx->sc_rin, rin, fx->sc_vwet, (float)to_rev / 127.0f, n * 2);
        }
    }
    reverb_process(fx, rin, out, n);
    if (cwet) for (i = 0; i < n * 2; i++) out[i] += fx->sc_cwet[i];
    else chorus_process(fx, cin, out, n, frame);
    if (vout) for (i = 0; i < n * 2; i++) out[i] += fx->sc_vwet[i];
}

void fa_midi_fx_process(fa_midi_fx* fx, const float* reverb_in, const float* chorus_in, const float* var_in, float* out, int frames, fa_frames song_frame)
{
    int off, n;
    if (!fx || frames <= 0) return;
    for (off = 0; off < frames; off += n) {
        n = frames - off < FX_CHUNK ? frames - off : FX_CHUNK;
        process_chunk(fx, reverb_in + off * 2, chorus_in + off * 2, var_in ? var_in + off * 2 : NULL, out + off * 2, n, song_frame + off);
    }
}

/* Whether block `bit` of a chain (insertion 1, 2, the variation, the EFX) plays now, and
 * its effect: a part rendered apart while the routing changed (SysEx in the block) goes
 * through it dry. */
static fa_midi_var* chain_block(fa_midi_fx* fx, int bit, float* w)
{
    fa_midi_var* v = NULL;
    int on = 0;
    switch (bit) {
    case FA_MIDI_CHAIN_INS1: case FA_MIDI_CHAIN_INS2: {
        int k = bit == FA_MIDI_CHAIN_INS2;
        v = fx->ins[k];
        on = fx->ins_mode == FA_MIDI_FX_XG && fx->ins_on[k] && fx->ins_part[k] < 16;
        break;
    }
    case FA_MIDI_CHAIN_VAR: v = fx->var; on = fa_midi_fx_route(fx) >= 0; break;
    default: v = fx->efx; on = fx->ins_mode == FA_MIDI_FX_GS && fx->efx_on; break;
    }
    *w = on && v ? (float)(fa_midi_var_dry_wet(v) - 1) / 126.0f : 0.0f;
    *w = *w < 0.0f ? 0.0f : *w > 1.0f ? 1.0f : *w;
    return v;
}

void fa_midi_fx_insert(fa_midi_fx* fx, int chain, const float* in, float* out, float* reverb, float* chorus, float* var,
                       float rs, float cs, float vs, int frames, fa_frames song_frame)
{
    static const int order[4] = { FA_MIDI_CHAIN_INS1, FA_MIDI_CHAIN_INS2, FA_MIDI_CHAIN_VAR, FA_MIDI_CHAIN_EFX };
    fa_midi_var* blk[4];
    float w[4], dry[4];
    int off, n, i, k;
    if (!fx || frames <= 0 || !chain) return;
    /* Dry/wet 1..127: 64 is half each; 127 (the GS EFX, types without a dry/wet) the effect alone. */
    for (k = 0; k < 4; k++) {
        w[k] = 0.0f;
        blk[k] = (chain & order[k]) ? chain_block(fx, order[k], &w[k]) : NULL;
        dry[k] = 1.0f - w[k];
    }
    for (off = 0; off < frames; off += n) {
        const float* x = in + off * 2;
        n = frames - off < FX_CHUNK ? frames - off : FX_CHUNK;
        for (k = 0; k < 4; k++) {
            int vout;
            if (!(chain & order[k])) continue;
            vout = w[k] > 0.0f ? fa_midi_var_process(blk[k], x, fx->sc_vwet, n, song_frame + off) : 0;
            for (i = 0; i < n * 2; i++) fx->sc_x[i] = x[i] * dry[k] + (vout ? fx->sc_vwet[i] * w[k] : 0.0f);
            x = fx->sc_x;
        }
        for (i = 0; i < n * 2; i++) {
            float y = x[i];
            out[off * 2 + i] += y;
            reverb[off * 2 + i] += y * rs;
            chorus[off * 2 + i] += y * cs;
        }
        if (var && vs != 0.0f) for (i = 0; i < n * 2; i++) var[off * 2 + i] += x[i] * vs;
    }
}

int fa_midi_fx_chain(const fa_midi_fx* fx, int ch)
{
    int m = 0, k;
    if (!fx || ch < 0 || ch > 15) return 0;
    if (fx->ins_mode == FA_MIDI_FX_XG)
        for (k = 0; k < 2; k++) if (fx->ins[k] && fx->ins_on[k] && fx->ins_part[k] == ch) m |= k ? FA_MIDI_CHAIN_INS2 : FA_MIDI_CHAIN_INS1;
    if (fa_midi_fx_route(fx) == ch) m |= FA_MIDI_CHAIN_VAR;
    if (fx->ins_mode == FA_MIDI_FX_GS && fx->efx && fx->efx_on && (fx->efx_parts >> ch & 1u)) m |= FA_MIDI_CHAIN_EFX;
    return m;
}

void fa_midi_fx_insert_sends(const fa_midi_fx* fx, int chain, int cc91, int cc93, int cc94, float* rs, float* cs, float* vs)
{
    int route = fa_midi_fx_route(fx);
    if (chain & FA_MIDI_CHAIN_EFX) {
        /* The EFX's own send levels in place of the parts' (SC-88 Pro). */
        *rs = (float)fx->efx_send[0] * (0.2f / 127.0f);
        *cs = (float)fx->efx_send[1] * (0.2f / 127.0f);
        *vs = route == -1 ? (float)fx->efx_send[2] * (0.2f / 127.0f) : 0.0f;
        return;
    }
    *rs = (float)cc91 * (0.2f / 127.0f);
    *cs = (float)cc93 * (0.2f / 127.0f);
    *vs = route == -1 && !(chain & FA_MIDI_CHAIN_VAR) ? (float)cc94 * (0.2f / 127.0f) : 0.0f;
}

/* The controllers' sum on a block: depths 0-127 (64: none) times the sources. */
static float xg_amount(const int* depth, int n, int ch, fa_midi_ctl_fn get, void* user)
{
    static const int src[5] = { 1, FA_MIDI_CTL_BEND, FA_MIDI_CTL_PRESSURE, 16, 17 };   /* MW, bend, aftertouch, AC1, AC2 */
    float a = 0.0f;
    int j;
    for (j = 0; j < n; j++) if (depth[j] != 64) a += (float)(depth[j] - 64) / 64.0f * get(user, ch, src[j]);
    return a;
}

void fa_midi_fx_controls(fa_midi_fx* fx, fa_midi_ctl_fn get, void* user)
{
    int k, route;
    if (!fx || !get) return;
    if (fx->ins_mode == FA_MIDI_FX_XG)
        for (k = 0; k < 2; k++)
            if (fx->ins[k] && fx->ins_on[k] && fx->ins_part[k] < 16)
                fa_midi_var_control(fx->ins[k], 2, xg_amount(fx->ins_depth[k], 5, fx->ins_part[k], get, user));
    route = fa_midi_fx_route(fx);
    if (route >= 0) fa_midi_var_control(fx->var, 2, xg_amount(fx->var_depth, 4, route, get, user));
    if (fx->ins_mode == FA_MIDI_FX_GS && fx->efx && fx->efx_on && fx->efx_parts) {
        int ch = 0;
        while (!(fx->efx_parts >> ch & 1u)) ch++;   /* the lowest part switched through it */
        for (k = 0; k < 2; k++) {
            int s = fx->efx_src[k], code = s >= 1 && s <= 95 ? s : s == 96 ? FA_MIDI_CTL_PRESSURE : s == 97 ? FA_MIDI_CTL_BEND : -1;
            float depth = (float)(fx->efx_depth[k] - 64) / 50.0f;
            fa_midi_var_control(fx->efx, k, code < 0 || depth == 0.0f ? 0.0f : depth * get(user, ch, code));
        }
    }
}

/* ---- GS and XG settings ------------------------------------------------------ */

/* The tuning from the settings. A change of kind (room, echo, off) starts from silence. */
static void reverb_update(fa_midi_fx* fx)
{
    const fx_reverb_type* t = &reverb_types[fx->rev_type < 0 ? GS_HALL2 : fx->rev_type];
    int kind = fx->rev_type < 0 ? KIND_OFF : t->kind;
    float time = fx->rev_time;
    if ((kind == KIND_DELAY || kind == KIND_PAN_DELAY) && !fx->echo) {
        fx->echo_cap = (int)(FX_ECHO_MAX_S * fx->rate) + 1;
        fx->echo = (float*)calloc(2 * (size_t)fx->echo_cap, sizeof(float));
        if (!fx->echo) { kind = KIND_ROOM; t = &reverb_types[GS_HALL2]; time = t->time; }   /* no memory: the default reverb */
    }
    if (kind != fx->kind) {
        fx->kind = kind;
        reverb_clear(fx);
    }
    /* g^n = 0.001 after n trips round a comb: log g goes as 1 / time. */
    fx->room = powf(FX_ROOM, FX_ROOM_TIME / (time > 0.1f ? time : 0.1f));
    if (fx->room > FX_ROOM_MAX) fx->room = FX_ROOM_MAX;
    fx->damp = t->damp;
    fx->in_gain = FX_INPUT_GAIN;
    fx->wet = FX_REVERB_WET * ((float)fx->rev_level / 64.0f);
    fx->pre_a = (float)fx->rev_pre * 0.1f;
    if (fx->rev_predelay > 0 && !fx->pre) {
        fx->pre_cap = (int)(FX_PREDELAY_MAX_MS * fx->rate / 1000.0) + 2;
        fx->pre = (float*)calloc((size_t)fx->pre_cap, sizeof(float));
    }
    {
        int len = fx->pre ? (int)(fx->rev_predelay * fx->rate / 1000.0 + 0.5) : 0;
        if (fx->pre && len >= fx->pre_cap) len = fx->pre_cap - 1;
        /* A longer line: what is past the old end was written long ago, so it starts silent. */
        if (len > fx->pre_len) memset(fx->pre + fx->pre_len, 0, (size_t)(len - fx->pre_len) * sizeof(float));
        fx->pre_len = len;
    }
    if (fx->pre_pos >= fx->pre_len) fx->pre_pos = 0;
    if (fx->echo) {
        int len = (int)((fx->gs_time + 1) * FX_ECHO_MS_PER_STEP * fx->rate / 1000.0);
        fx->echo_len = len < 1 ? 1 : len > fx->echo_cap ? fx->echo_cap : len;
        if (fx->echo_pos >= fx->echo_len) fx->echo_pos = 0;
    }
    fx->echo_fb = (float)fx->gs_feedback / 127.0f * FX_FEEDBACK_MAX;
}

static void chorus_update(fa_midi_fx* fx)
{
    double rate = FX_CHORUS_RATE_HZ * (fx->cho_rate / 3.0);
    if (rate > FX_CHORUS_MAX_RATE_HZ) rate = FX_CHORUS_MAX_RATE_HZ;
    if (fx->xg_cho[0] >= 0) rate = fa_xg_lfo_hz(fx->xg_cho[0]);
    fx->delay_ms = FX_CHORUS_DELAY_MS * pow(2.0, (fx->cho_delay - 80) / 24.0);
    /* Without an LFO the taps stand still: a plain delay. */
    fx->depth_ms = rate > 0.0 ? FX_CHORUS_DEPTH_MS * (fx->cho_depth / 19.0) : 0.0;
    if (fx->xg_cho[1] >= 0) fx->depth_ms = rate > 0.0 ? fa_xg_mod_depth_ms(fx->xg_cho[1]) : 0.0;
    /* XG's delay offset is the shortest delay the LFO reaches. */
    if (fx->xg_cho[3] >= 0) fx->delay_ms = fa_xg_mod_delay_ms(fx->xg_cho[3]) + fx->depth_ms;
    if (fx->depth_ms > 0.9 * fx->delay_ms) fx->depth_ms = 0.9 * fx->delay_ms;
    if (rate != fx->rate_hz || fx->lfo_cos == 0.0) {
        fx->rate_hz = rate;
        fx->lfo_cos = cos(2.0 * FA_PI * rate / fx->rate);
        fx->lfo_sin = sin(2.0 * FA_PI * rate / fx->rate);
        fx->lfo_next = -1;
    }
    fx->chorus_fb = fx->xg_cho[2] >= 0 ? fa_xg_feedback(fx->xg_cho[2]) * FX_FEEDBACK_MAX : (float)fx->cho_feedback / 127.0f * FX_FEEDBACK_MAX;
    fx->chorus_wet = (float)fx->cho_level / 64.0f;
    fx->chorus_pre_a = (float)fx->cho_pre * 0.1f;
    if (!fx->cho_on) chorus_clear(fx);
}

static void gs_reverb_time(fa_midi_fx* fx)
{
    fx->rev_time = reverb_types[fx->rev_type].time * exp2f((float)(fx->gs_time - 64) / 32.0f);
}

static void chorus_macro(fa_midi_fx* fx, int macro)
{
    fx->cho_feedback = chorus_macros[macro][0];
    fx->cho_delay = chorus_macros[macro][1];
    fx->cho_rate = chorus_macros[macro][2];
    fx->cho_depth = chorus_macros[macro][3];
    fx->cho_pre = 0;
    fx->xg_cho[0] = fx->xg_cho[1] = fx->xg_cho[2] = fx->xg_cho[3] = -1;
}

/* The third effect as a GS delay or an XG variation, at its reset state when it
 * becomes one. 0: no memory. */
static int var_make(fa_midi_fx* fx, int kind)
{
    if (!fx->var && !(fx->var = fa_midi_var_new(fx->rate))) return 0;
    if (fx->var_kind == kind) return 1;
    fx->var_kind = kind;
    if (kind == VAR_GS) {
        fa_midi_var_gs(fx->var, 0x50, 0);   /* Delay 1 */
    } else {
        fx->var_msb = 0x05;                 /* Delay L,C,R, insertion, no part, as after an XG reset */
        fx->var_on = fa_midi_var_set_type(fx->var, 0x05, 0);
        fx->var_system = 0;
        fx->var_part = 127;
        fx->var_return = fx->var_pan = 64;
        fx->var_to_rev = fx->var_to_cho = 0;
    }
    return 1;
}

void fa_midi_fx_defaults(fa_midi_fx* fx, int mode)
{
    int k, j;
    if (!fx) return;
    fx->rev_type = GS_HALL2;
    fx->rev_time = FX_ROOM_TIME;
    fx->rev_level = 64;
    fx->rev_pre = 0;
    fx->gs_time = 64;
    fx->gs_feedback = 0;
    fx->xg_rev_msb = 1;
    fx->xg_cho_msb = 0x41;
    fx->rev_predelay = 0;
    fx->cho_to_rev = fx->cho_to_del = 0;
    fx->cho_on = 1;
    fx->cho_level = 64;
    chorus_macro(fx, 2);    /* Chorus 3 without its feedback: the chorus above */
    fx->cho_feedback = 0;
    reverb_update(fx);
    chorus_update(fx);
    /* The third effect: the SC-88 delay, the XG variation, or none (GM, GM2). */
    fx->var_kind = VAR_NONE;   /* back to the reset state */
    if (mode != FA_MIDI_FX_GM) var_make(fx, mode == FA_MIDI_FX_GS ? VAR_GS : VAR_XG);
    /* Insertion: the GS EFX (Thru, no part) or XG insertion 1 and 2 (no part), or none. */
    fx->ins_mode = mode;
    fx->efx_parts = 0;
    fx->efx_send[0] = 40;
    fx->efx_send[1] = fx->efx_send[2] = 0;
    fx->efx_src[0] = fx->efx_src[1] = 0;
    fx->efx_depth[0] = fx->efx_depth[1] = 64;
    if (fx->efx) fx->efx_on = fa_midi_var_gs_efx(fx->efx, 0x00, 0);
    for (k = 0; k < 2; k++) {
        fx->ins_part[k] = 127;
        fx->ins_msb[k] = 0x49;
        for (j = 0; j < 5; j++) fx->ins_depth[k][j] = 64;
        if (fx->ins[k]) fx->ins_on[k] = fa_midi_var_set_type(fx->ins[k], 0x49, 0);
    }
    for (j = 0; j < 4; j++) fx->var_depth[j] = 64;
}

/* XG insertion block `k` (Distortion, no part, after an XG reset) or the GS EFX (Thru),
 * made when first needed. 0: no memory. */
static int ins_make(fa_midi_fx* fx, int k)
{
    if (fx->ins[k]) return 1;
    if (!(fx->ins[k] = fa_midi_var_new(fx->rate))) return 0;
    fx->ins_on[k] = fa_midi_var_set_type(fx->ins[k], fx->ins_msb[k], 0);
    return 1;
}

static int efx_make(fa_midi_fx* fx)
{
    if (fx->efx) return 1;
    if (!(fx->efx = fa_midi_var_new(fx->rate))) return 0;
    fx->efx_on = fa_midi_var_gs_efx(fx->efx, 0x00, 0);
    return 1;
}

void fa_midi_fx_xg_insertion(fa_midi_fx* fx, int k, int addr, int v)
{
    fa_midi_var* b;
    if (!fx || k < 0 || k > 1) return;
    v &= 0x7F;
    fx->ins_mode = FA_MIDI_FX_XG;
    if (!ins_make(fx, k)) return;
    b = fx->ins[k];
    if (addr == 0x00 || addr == 0x01) {
        if (addr == 0x00) fx->ins_msb[k] = v;
        fx->ins_on[k] = fa_midi_var_set_type(b, fx->ins_msb[k], addr == 0x01 ? v : 0);
    } else if (addr >= 0x02 && addr <= 0x0B) {
        fa_midi_var_set_param(b, addr - 0x02, v, 0);
    } else if (addr == 0x0C) {
        if (v != fx->ins_part[k]) fa_midi_var_reset(b);   /* another part: from silence */
        fx->ins_part[k] = v;
    } else if (addr >= 0x0D && addr <= 0x11) {
        fx->ins_depth[k][addr - 0x0D] = v;
    } else if (addr >= 0x20 && addr <= 0x25) {
        fa_midi_var_set_param(b, 10 + addr - 0x20, v, 0);
    } else if (addr >= 0x30 && addr <= 0x43) {
        fa_midi_var_set_param(b, (addr - 0x30) / 2, v, (addr - 0x30) % 2 ? 2 : 1);
    }
}

void fa_midi_fx_gs_efx(fa_midi_fx* fx, int addr, int v)
{
    if (!fx) return;
    v &= 0x7F;
    fx->ins_mode = FA_MIDI_FX_GS;
    if (!efx_make(fx)) return;
    if (addr <= 0x16 && addr != 0x02) fx->efx_on = fa_midi_var_gs_efx(fx->efx, addr, v);
    else if (addr >= 0x17 && addr <= 0x19) fx->efx_send[addr - 0x17] = v;
    else if (addr == 0x1B || addr == 0x1D) fx->efx_src[addr == 0x1D] = v;
    else if (addr == 0x1C || addr == 0x1E) fx->efx_depth[addr == 0x1E] = v;
}

void fa_midi_fx_gs_part_efx(fa_midi_fx* fx, int ch, int on)
{
    if (!fx || ch < 0 || ch > 15) return;
    if (on) {
        fx->ins_mode = FA_MIDI_FX_GS;
        if (!efx_make(fx)) return;
        fx->efx_parts |= 1u << ch;
    } else if (fx->efx_parts & 1u << ch) {
        fx->efx_parts &= ~(1u << ch);
        if (!fx->efx_parts) fa_midi_var_reset(fx->efx);   /* no part left: from silence when one comes back */
    }
}

/* GS: 40 01 30 .. 40 01 5A (reverb 30-37, chorus 38-40, delay 50-5A). */
void fa_midi_fx_gs(fa_midi_fx* fx, int addr, int v)
{
    if (!fx) return;
    v &= 0x7F;
    if (addr >= 0x50 && addr <= 0x5A) {
        if (var_make(fx, VAR_GS)) fa_midi_var_gs(fx->var, addr, v);
        return;
    }
    switch (addr) {
    case 0x30:                  /* reverb macro */
        if (v > 7) return;
        fx->rev_type = v;
        fx->rev_pre = 0;
        fx->rev_predelay = 0;
        fx->gs_time = 64;
        fx->gs_feedback = v >= GS_DELAY ? 32 : 0;
        gs_reverb_time(fx);
        break;
    case 0x31:                  /* reverb character */
        if (v > 7) return;
        fx->rev_type = v;
        gs_reverb_time(fx);
        break;
    case 0x32: fx->rev_pre = v > 7 ? 7 : v; break;
    case 0x33: fx->rev_level = v; break;
    case 0x34:
        fx->gs_time = v;
        if (fx->rev_type >= 0 && fx->rev_type < XG_HALL1) gs_reverb_time(fx);
        break;
    case 0x35: fx->gs_feedback = v; break;
    case 0x37: fx->rev_predelay = v; break;   /* pre-delay, ms */
    case 0x38:                  /* chorus macro */
        if (v > 7) return;
        chorus_macro(fx, v);
        fx->cho_to_rev = fx->cho_to_del = 0;
        fx->cho_on = 1;
        break;
    case 0x39: fx->cho_pre = v > 7 ? 7 : v; break;
    case 0x3A: fx->cho_level = v; break;
    case 0x3B: fx->cho_feedback = v; break;
    case 0x3C: fx->cho_delay = v; break;
    case 0x3D: fx->cho_rate = v; break;
    case 0x3E: fx->cho_depth = v; break;
    case 0x3F: fx->cho_to_rev = v; return;
    case 0x40: fx->cho_to_del = v; return;
    default: return;
    }
    if (addr < 0x38) reverb_update(fx);
    else chorus_update(fx);
}

/* XG reverb time (parameter 1 of the hall, room, stage and plate types), seconds. */
static float xg_reverb_time(int v)
{
    static const float tail[12] = { 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 25, 30 };
    if (v < 48) return 0.3f + 0.1f * (float)v;
    if (v < 58) return 5.5f + 0.5f * (float)(v - 48);
    return tail[v < 69 ? v - 58 : 11];
}

static int xg_reverb_type(int msb, int lsb)
{
    switch (msb) {
    case 0x00: return -1;   /* no effect */
    case 0x01: return lsb == 1 ? XG_HALL1 + 1 : XG_HALL1;
    case 0x02: return XG_HALL1 + 2 + (lsb <= 2 ? lsb : 0);
    case 0x03: return XG_HALL1 + 5 + (lsb == 1);
    case 0x04: return XG_HALL1 + 7;
    case 0x10: case 0x11: case 0x12: case 0x13: return XG_HALL1 + 8 + (msb - 0x10);
    default: return XG_HALL1;   /* a type made later than XG's: the default hall */
    }
}

/* XG chorus types as GS macros: chorus and celeste (and the later ensemble types)
 * as the choruses, flanger as the flanger. */
static int xg_chorus_macro(int msb, int lsb)
{
    if (msb == 0x43) return 5;
    if (msb == 0x42) return 1 + (lsb == 2 ? 2 : 0);
    if (msb == 0x41) return lsb == 1 ? 1 : lsb == 2 ? 3 : 2;
    return 2;
}

/* XG variation: type 40-41, parameters 1-10 (42-55, two bytes each) and 11-16
 * (70-75), return, pan, sends to reverb and chorus, connection, part (56-5B). */
static void xg_variation(fa_midi_fx* fx, int addr, int v)
{
    if (!var_make(fx, VAR_XG)) return;
    if (addr == 0x40 || addr == 0x41) {
        if (addr == 0x40) fx->var_msb = v;
        fx->var_on = fa_midi_var_set_type(fx->var, fx->var_msb, addr == 0x41 ? v : 0);
    } else if (addr <= 0x55) {
        fa_midi_var_set_param(fx->var, (addr - 0x42) / 2, v, (addr - 0x42) % 2 ? 2 : 1);
    } else if (addr >= 0x70) {
        fa_midi_var_set_param(fx->var, 10 + addr - 0x70, v, 0);
    } else {
        switch (addr) {
        case 0x56: fx->var_return = v; break;
        case 0x57: fx->var_pan = v; break;
        case 0x58: fx->var_to_rev = v; break;
        case 0x59: fx->var_to_cho = v; break;
        case 0x5A:
            if ((v & 1) != fx->var_system) {   /* rewired: from silence, and the part's controllers let go */
                fa_midi_var_reset(fx->var);
                fa_midi_var_control(fx->var, 2, 0.0f);
            }
            fx->var_system = v & 1;
            break;
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:   /* MW, bend, aftertouch, AC1 control depth */
            fx->var_depth[addr - 0x5C] = v;
            break;
        default:                            /* 5B: 0-15, 127 none */
            if (v != fx->var_part) {   /* another part: from silence, its controllers taken next block */
                fa_midi_var_reset(fx->var);
                fa_midi_var_control(fx->var, 2, 0.0f);
            }
            fx->var_part = v;
            break;
        }
    }
}

/* XG: 02 01 00 .. 02 01 75 (reverb 00-0C, chorus 20-2E, variation 40-5B, 70-75). */
void fa_midi_fx_xg(fa_midi_fx* fx, int addr, int v)
{
    if (!fx) return;
    v &= 0x7F;
    switch (addr) {
    case 0x00:                  /* reverb type MSB (the LSB follows or is 0) */
    case 0x01:                  /* reverb type LSB */
        if (addr == 0x00) fx->xg_rev_msb = v;
        fx->rev_type = xg_reverb_type(fx->xg_rev_msb, addr == 0x01 ? v : 0);
        fx->rev_pre = 0;
        if (fx->rev_type >= 0) fx->rev_time = reverb_types[fx->rev_type].time;
        break;
    case 0x02:                  /* reverb time */
        if (fx->rev_type >= 0 && reverb_types[fx->rev_type].kind == KIND_ROOM) fx->rev_time = xg_reverb_time(v);
        break;
    case 0x0C: fx->rev_level = v; break;   /* reverb return */
    case 0x20:
    case 0x21:
        if (addr == 0x20) fx->xg_cho_msb = v;
        fx->cho_on = fx->xg_cho_msb != 0;
        if (fx->cho_on) chorus_macro(fx, xg_chorus_macro(fx->xg_cho_msb, addr == 0x21 ? v : 0));
        break;
    case 0x22: case 0x23: case 0x24: case 0x25:   /* LFO frequency, depth, feedback, delay offset */
        fx->xg_cho[addr - 0x22] = v;
        break;
    case 0x2C: fx->cho_level = v; break;   /* chorus return */
    case 0x2E: fx->cho_to_rev = v; return;
    default:
        if ((addr >= 0x40 && addr <= 0x5F) || (addr >= 0x70 && addr <= 0x75)) xg_variation(fx, addr, v);
        return;
    }
    if (addr < 0x20) reverb_update(fx);
    else chorus_update(fx);
}
