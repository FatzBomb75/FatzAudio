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
 * Both are deterministic: the output depends only on the input since the last
 * reset, and the chorus LFO only on the song frame, so reading in any block size
 * gives the same samples. When the input has been silent long enough for the
 * tail to fall below -140 dB the state is cleared and processing stops, so a
 * silent stretch is exactly silent (and costs nothing), as it is without effects. */
#include "fa_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FX_COMBS 8
#define FX_ALLPASSES 4
#define FX_SPREAD 23                    /* extra delay of the right channel, frames at 44.1 kHz */
#define FX_INPUT_GAIN 0.015f
#define FX_ROOM 0.84f                   /* comb feedback: about 1.3 s to -60 dB */
#define FX_DAMP 0.25f
#define FX_ALLPASS_FEEDBACK 0.5f
#define FX_REVERB_WET 1.5f
#define FX_QUIET 1e-7f                  /* -140 dB: the tail is cleared below this */

#define FX_CHORUS_DELAY_MS 13.0
#define FX_CHORUS_DEPTH_MS 3.0
#define FX_CHORUS_RATE_HZ 0.4
#define FX_CHORUS_TAPS 2
#define FX_LFO_SEED 256                 /* frames between exact values of the chorus LFO */

static const int comb_tuning[FX_COMBS] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
static const int allpass_tuning[FX_ALLPASSES] = { 556, 441, 341, 225 };

typedef struct fx_line { float* buf; int len, pos; float lp; } fx_line;

struct fa_midi_fx {
    uint32_t rate;
    float room, damp, in_gain, wet;     /* the reverb's tuning */
    fx_line comb[2][FX_COMBS];
    fx_line allpass[2][FX_ALLPASSES];
    int reverb_idle;
    fa_frames reverb_quiet;             /* frames since the reverb input was last non-zero */
    fa_frames reverb_span;              /* the longest path through the delay lines */
    float* chorus_buf[2];
    int chorus_len, chorus_pos;
    double lfo_cos, lfo_sin;            /* the chorus LFO's turn in one frame */
    double lfo_s, lfo_c;                /* the LFO's sine and cosine at frame lfo_next */
    fa_frames lfo_next;                 /* -1: not running */
    fa_frames chorus_quiet;             /* frames since the chorus input was last non-zero */
    float* mem;                         /* every delay line, one allocation */
};

static int scaled(int frames, uint32_t rate)
{
    int n = (int)((double)frames * rate / 44100.0 + 0.5);
    return n < 1 ? 1 : n;
}

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
    fx->chorus_len = (int)ceil((FX_CHORUS_DELAY_MS + FX_CHORUS_DEPTH_MS) * rate / 1000.0) + 2;
    fx->lfo_cos = cos(2.0 * 3.14159265358979323846 * FX_CHORUS_RATE_HZ / rate);
    fx->lfo_sin = sin(2.0 * 3.14159265358979323846 * FX_CHORUS_RATE_HZ / rate);
    total += 2 * (size_t)fx->chorus_len;
    fx->mem = p = (float*)calloc(total, sizeof(float));
    if (!p) { free(fx); return NULL; }
    for (ch = 0; ch < 2; ch++) {
        for (i = 0; i < FX_COMBS; i++) { fx->comb[ch][i].buf = p; p += fx->comb[ch][i].len; }
        for (i = 0; i < FX_ALLPASSES; i++) { fx->allpass[ch][i].buf = p; p += fx->allpass[ch][i].len; }
    }
    fx->chorus_buf[0] = p;
    fx->chorus_buf[1] = p + fx->chorus_len;
    fa_midi_fx_reset(fx);
    return fx;
}

void fa_midi_fx_free(fa_midi_fx* fx)
{
    if (!fx) return;
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
    fx->reverb_idle = 1;
    fx->reverb_quiet = 0;
}

void fa_midi_fx_reset(fa_midi_fx* fx)
{
    if (!fx) return;
    reverb_clear(fx);
    memset(fx->chorus_buf[0], 0, 2 * (size_t)fx->chorus_len * sizeof(float));
    fx->chorus_pos = 0;
    fx->chorus_quiet = fx->chorus_len;
    fx->lfo_next = -1;
}

static void reverb_process(fa_midi_fx* fx, const float* in, float* out, int frames)
{
    float peak = 0.0f;
    int quiet_in = fa_all_zero(in, (uint32_t)frames * 2), f, ch, i;
    if (quiet_in && fx->reverb_idle) return;
    fx->reverb_idle = 0;
    fx->reverb_quiet = quiet_in ? fx->reverb_quiet + frames : 0;
    for (f = 0; f < frames; f++) {
        float x = (in[f * 2] + in[f * 2 + 1]) * fx->in_gain;
        for (ch = 0; ch < 2; ch++) {
            float y = 0.0f;
            for (i = 0; i < FX_COMBS; i++) {
                fx_line* c = &fx->comb[ch][i];
                float d = c->buf[c->pos];
                c->lp = d * (1.0f - fx->damp) + c->lp * fx->damp;
                c->buf[c->pos] = x + c->lp * fx->room;
                if (++c->pos == c->len) c->pos = 0;
                y += d;
            }
            for (i = 0; i < FX_ALLPASSES; i++) {
                fx_line* a = &fx->allpass[ch][i];
                float d = a->buf[a->pos];
                a->buf[a->pos] = y + d * FX_ALLPASS_FEEDBACK;
                if (++a->pos == a->len) a->pos = 0;
                y = d - y;
            }
            y *= fx->wet;
            out[f * 2 + ch] += y;
            if (fabsf(y) > peak) peak = fabsf(y);
        }
    }
    /* Cleared only once the last input has gone all the way through the delay
     * lines (the output is still silent before the first comb echo comes back). */
    if (fx->reverb_quiet > fx->reverb_span && peak < FX_QUIET) reverb_clear(fx);
}

static void chorus_process(fa_midi_fx* fx, const float* in, float* out, int frames, fa_frames song_frame)
{
    const double base = FX_CHORUS_DELAY_MS * fx->rate / 1000.0, depth = FX_CHORUS_DEPTH_MS * fx->rate / 1000.0;
    const double step = FX_CHORUS_RATE_HZ / fx->rate;
    double s, c;
    int f, ch, t;
    if (fa_all_zero(in, (uint32_t)frames * 2)) {
        if (fx->chorus_quiet >= fx->chorus_len) return;   /* the lines hold only zeros */
        fx->chorus_quiet += frames;
    } else {
        fx->chorus_quiet = 0;
    }
    /* The LFO is turned on frame by frame from exact values at every multiple of
     * FX_LFO_SEED song frames, so each frame's value depends only on the frame
     * (not on how the song is cut into blocks), and the rounding that adds up in
     * between stays far below a float's. Away from where it left off (a seek,
     * a silent stretch) it starts again from the last such frame. */
    if (fx->lfo_next != song_frame) {
        fa_frames g = song_frame - song_frame % FX_LFO_SEED;
        double ph = 2.0 * 3.14159265358979323846 * fmod((double)g * step, 1.0);
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
        if (g % FX_LFO_SEED == 0) {
            double ph = 2.0 * 3.14159265358979323846 * fmod((double)g * step, 1.0);
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
        fx->chorus_buf[0][w] = in[f * 2];
        fx->chorus_buf[1][w] = in[f * 2 + 1];
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
            out[f * 2 + ch] += y * (1.0f / FX_CHORUS_TAPS);
        }
        if (++fx->chorus_pos == fx->chorus_len) fx->chorus_pos = 0;
    }
    fx->lfo_s = s;
    fx->lfo_c = c;
    fx->lfo_next = song_frame + frames;
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

void fa_midi_fx_process(fa_midi_fx* fx, const float* reverb_in, const float* chorus_in, float* out, int frames, fa_frames song_frame)
{
    if (!fx || frames <= 0) return;
    reverb_process(fx, reverb_in, out, frames);
    chorus_process(fx, chorus_in, out, frames, song_frame);
}
