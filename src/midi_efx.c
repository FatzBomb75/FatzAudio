/* MIDI insertion effects: the GS EFX of the SC-88 Pro (one effect any parts can be
 * switched through, 40 03 xx) and the XG types the MU80 variation list lacks (the
 * MU100's insertion and variation additions: pitch change, aural exciter, touch wah,
 * compressor, noise gate, voice cancel, lo-fi, ...), written for FatzAudio.
 *
 * A type is a short chain of processing stages (midi_efx_types.c builds it from the
 * type's parameters): EQ sections, a drive (overdrive/distortion with an amp's tone),
 * an enhancer, formant filters (humanizer, talking modulator), a phaser, a wah
 * (envelope and LFO swept), a two-rotor rotary speaker, modulated delay voices (the
 * chorus, flanger, hexa chorus, space D family, with a level LFO for tremolo chorus
 * and a stepped LFO for the step flanger), tremolo and auto pan, dynamics
 * (compressor, limiter, noise gate), multi-tap delays with feedback (stereo, cross,
 * modulated, gliding time, gate reverb patterns), a reverb (midi_fx.c's), a pitch
 * shifter (two crossfaded taps), a 3D panner, lo-fi (sample and hold, word length,
 * noises), a ring modulator and a voice canceller. Stages run one after the other, or
 * in two chains side by side (the SC-88 Pro's parallel types). GS types end with the
 * EFX's low and high shelves, pan and level.
 *
 * Deterministic as midi_fx.c and midi_var.c: the output depends only on the input and
 * the settings since the last reset, the LFOs (and the lo-fi noises) only on the song
 * frame, except the rotary's rotors, whose speed glides. Once the input has been
 * silent and nothing at -140 dB or more is left, the state is cleared and processing
 * stops, so silence is exact and costs nothing. */
#include "midi_efx.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define EFX_QUIET 1e-7f          /* -140 dB */
#define EFX_FILTER_TAIL 8192     /* frames a filter or LFO type runs on after its input stops */
#define EFX_SWEEP_STEP 16        /* frames between new coefficients of the swept filters */

/* ---- helpers ------------------------------------------------------------------ */

float efx_pole(uint32_t rate, double hz)
{
    return hz <= 0.0 || hz >= 0.45 * rate ? 0.0f : (float)exp(-2.0 * FA_PI * hz / rate);
}

static float lowpass1(float x, float a, float* z) { return a > 0.0f ? (*z = x * (1.0f - a) + *z * a) : x; }

/* RBJ cookbook sections with any Q (fa_mixer_design is fixed at 1/sqrt(2), fa_eq_design
 * has no low-pass or band-pass). */
biquad efx_biquad(int type, double hz, double q, double db, uint32_t rate)
{
    double w, c, s, alpha, A = pow(10.0, db / 40.0), b0, b1, b2, a0, a1, a2, sq;
    biquad r;
    if (hz > 0.45 * rate) hz = 0.45 * rate;
    if (hz < 10.0) hz = 10.0;
    if (q < 0.1) q = 0.1;
    w = 2.0 * FA_PI * hz / rate;
    c = cos(w);
    s = sin(w);
    alpha = s / (2.0 * q);
    sq = 2.0 * sqrt(A) * alpha;
    switch (type) {
    case BQ_LP: b0 = (1 - c) / 2; b1 = 1 - c; b2 = b0; a0 = 1 + alpha; a1 = -2 * c; a2 = 1 - alpha; break;
    case BQ_HP: b0 = (1 + c) / 2; b1 = -(1 + c); b2 = b0; a0 = 1 + alpha; a1 = -2 * c; a2 = 1 - alpha; break;
    case BQ_BP: b0 = alpha; b1 = 0; b2 = -alpha; a0 = 1 + alpha; a1 = -2 * c; a2 = 1 - alpha; break;
    case BQ_PEAK:
        b0 = 1 + alpha * A; b1 = -2 * c; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * c; a2 = 1 - alpha / A;
        break;
    case BQ_LOSHELF:
        b0 = A * ((A + 1) - (A - 1) * c + sq); b1 = 2 * A * ((A - 1) - (A + 1) * c); b2 = A * ((A + 1) - (A - 1) * c - sq);
        a0 = (A + 1) + (A - 1) * c + sq; a1 = -2 * ((A - 1) + (A + 1) * c); a2 = (A + 1) + (A - 1) * c - sq;
        break;
    default:   /* BQ_HISHELF */
        b0 = A * ((A + 1) + (A - 1) * c + sq); b1 = -2 * A * ((A - 1) + (A + 1) * c); b2 = A * ((A + 1) + (A - 1) * c - sq);
        a0 = (A + 1) - (A - 1) * c + sq; a1 = 2 * ((A - 1) - (A + 1) * c); a2 = (A + 1) - (A - 1) * c - sq;
        break;
    }
    r.b0 = (float)(b0 / a0); r.b1 = (float)(b1 / a0); r.b2 = (float)(b2 / a0);
    r.a1 = (float)(a1 / a0); r.a2 = (float)(a2 / a0);
    return r;
}

static float wave_at(int wave, double ph)
{
    switch (wave) {
    case WAVE_TRI: return (float)(1.0 - 4.0 * fabs(ph - 0.5));
    case WAVE_SQR: { float s = (float)sin(2.0 * FA_PI * ph) * 6.0f; return s > 1.0f ? 1.0f : s < -1.0f ? -1.0f : s; }
    case WAVE_SAW_UP: return (float)(2.0 * ph - 1.0);
    case WAVE_SAW_DOWN: return (float)(1.0 - 2.0 * ph);
    default: return (float)sin(2.0 * FA_PI * ph);
    }
}

/* The LFO's phase (0..1) at a song frame. */
static double phase_at(double hz, uint32_t rate, fa_frames frame, float offset)
{
    double ph = fmod((double)frame * hz / rate + offset, 1.0);
    return ph < 0.0 ? ph + 1.0 : ph;
}

/* Deterministic noise in -1..1 from a song frame and a stream number. */
static float noise_at(fa_frames frame, uint32_t salt)
{
    uint32_t x = (uint32_t)frame * 2654435761u ^ (uint32_t)((uint64_t)frame >> 32) ^ salt * 2246822519u;
    x ^= x >> 15; x *= 2246822519u; x ^= x >> 13; x *= 3266489917u; x ^= x >> 16;
    return (float)x / 2147483648.0f - 1.0f;
}

static float rd(const efx_line* l, int ch, float t)
{
    float r = (float)l->pos - t, fr;
    int i0, i1;
    while (r < 0.0f) r += (float)l->cap;
    i0 = (int)r;
    fr = r - (float)i0;
    if (i0 >= l->cap) i0 -= l->cap;
    i1 = i0 + 1 == l->cap ? 0 : i0 + 1;
    return l->d[i0 * 2 + ch] + (l->d[i1 * 2 + ch] - l->d[i0 * 2 + ch]) * fr;
}

static void wr(efx_line* l, float a, float b)
{
    l->d[l->pos * 2] = a;
    l->d[l->pos * 2 + 1] = b;
    if (++l->pos == l->cap) l->pos = 0;
}

/* ---- stages ------------------------------------------------------------------- */

void efx_stage_clear(efx_stage* s)
{
    if (s->line.d) memset(s->line.d, 0, 2 * (size_t)s->line.cap * sizeof(float));
    s->line.pos = 0;
    memset(s->z, 0, sizeof(s->z));
    memset(s->fz, 0, sizeof(s->fz));
    memset(s->svf, 0, sizeof(s->svf));
    memset(s->ap, 0, sizeof(s->ap));
    memset(s->last, 0, sizeof(s->last));
    memset(s->damp_z, 0, sizeof(s->damp_z));
    memset(s->pre_z, 0, sizeof(s->pre_z));
    memset(s->xz, 0, sizeof(s->xz));
    memset(s->hold, 0, sizeof(s->hold));
    memset(s->noise_z, 0, sizeof(s->noise_z));
    memset(s->v_ph, 0, sizeof(s->v_ph));
    memset(s->held, 0, sizeof(s->held));
    memset(s->rot_ph, 0, sizeof(s->rot_ph));
    s->hold_i = 0;
    s->env = 0.0f;
    s->gate_g = 0.0f;
    s->tap_now = s->tap_t[0];
    s->f1 = s->f1_to;
    s->f2 = s->f2_to;
    s->rot_hz[0] = (float)(s->fast_on ? s->fast[0] : s->slow[0]);
    s->rot_hz[1] = (float)(s->fast_on ? s->fast[1] : s->slow[1]);
    if (s->sub) fa_midi_fx_reset(s->sub);
}

void efx_stage_free(efx_stage* s)
{
    free(s->line.d);
    fa_midi_fx_free(s->sub);
    memset(s, 0, sizeof(*s));
}

/* The stage's line, made (or made longer, from silence) when it has something to play. */
int efx_stage_ready(fa_midi_efx* e, efx_stage* s)
{
    float* d;
    int cap;
    (void)e;
    if (s->need <= 0 || (s->line.d && s->line.cap >= s->need + 2)) return 1;
    cap = s->need + 2;
    d = (float*)calloc(2 * (size_t)cap, sizeof(float));
    if (!d) return 0;
    free(s->line.d);
    s->line.d = d;
    s->line.cap = cap;
    s->line.pos = 0;
    return 1;
}

static void run_drive(efx_stage* s, const float* in, float* w, int n)
{
    int i;
    for (i = 0; i < n * 2; i++) {
        float d = in[i] * s->drive, soft = tanhf(d);
        float hard = d > 1.0f ? 1.0f : d < -1.0f ? -1.0f : d;
        w[i] = (soft + s->edge * (hard - soft)) * s->level;
    }
}

static void run_enhance(efx_stage* s, const float* in, float* w, int n)
{
    int i;
    for (i = 0; i < n * 2; i++) {
        float h = in[i] - lowpass1(in[i], s->pre_a, &s->pre_z[i & 1]);
        w[i] = in[i] + tanhf(h * s->drive) * s->level;
    }
}

static void formant_design(fa_midi_efx* e, efx_stage* s)
{
    s->fmt[0] = efx_biquad(BQ_BP, s->f1, 6.0, 0.0, e->rate);
    s->fmt[1] = efx_biquad(BQ_BP, s->f2, 8.0, 0.0, e->rate);
}

static void run_formant(fa_midi_efx* e, efx_stage* s, const float* in, float* w, int n, fa_frames frame)
{
    int i, c;
    for (i = 0; i < n; i++) {
        if (i == 0 || (frame + i) % EFX_SWEEP_STEP == 0) {
            if (s->f1 != s->f1_to || s->f2 != s->f2_to || i == 0) {
                s->f1 += (s->f1_to - s->f1) * s->glide;
                s->f2 += (s->f2_to - s->f2) * s->glide;
                if (fabsf(s->f1 - s->f1_to) < 0.5f) s->f1 = s->f1_to;
                if (fabsf(s->f2 - s->f2_to) < 0.5f) s->f2 = s->f2_to;
                formant_design(e, s);
            }
        }
        for (c = 0; c < 2; c++) {
            float x = in[i * 2 + c];
            if (s->drive > 1.0f) x = tanhf(x * s->drive) / s->drive * 2.0f;
            w[i * 2 + c] = (fa_biquad_run(&s->fmt[0], s->fz[0][c], x) + 0.6f * fa_biquad_run(&s->fmt[1], s->fz[1][c], x)) * 2.5f;
        }
    }
}

static void run_phaser(fa_midi_efx* e, efx_stage* s, const float* in, float* w, int n, fa_frames frame)
{
    int i, c, k;
    for (i = 0; i < n; i++) {
        for (c = 0; c < 2; c++) {
            float a, y;
            if (i == 0 || (frame + i) % EFX_SWEEP_STEP == 0) {
                double ph = phase_at(s->hz, e->rate, frame + i, c ? s->phase : 0.0f);
                double fc = s->base * pow(2.0, s->range * s->depth * (0.5 + 0.5 * sin(2.0 * FA_PI * ph))), tn;
                if (fc > 0.45 * e->rate) fc = 0.45 * e->rate;
                tn = tan(FA_PI * fc / e->rate);
                s->coef[c] = (float)((tn - 1.0) / (tn + 1.0));
            }
            a = s->coef[c];
            y = in[i * 2 + c] + s->last[c] * s->fb;
            for (k = 0; k < s->npole; k++) {
                float o = a * y + s->ap[c][k];
                s->ap[c][k] = y - a * o;
                y = o;
            }
            s->last[c] = y;
            w[i * 2 + c] = y;
        }
    }
}

static void run_wah(fa_midi_efx* e, efx_stage* s, const float* in, float* w, int n, fa_frames frame)
{
    int i, c;
    float q = 1.0f / (s->reso < 0.5f ? 0.5f : s->reso);
    for (i = 0; i < n; i++) {
        float lvl = fabsf(in[i * 2]) > fabsf(in[i * 2 + 1]) ? fabsf(in[i * 2]) : fabsf(in[i * 2 + 1]);
        s->env += (lvl - s->env) * (lvl > s->env ? s->att : s->rel);
        if (i == 0 || (frame + i) % EFX_SWEEP_STEP == 0) {
            float env = s->env * s->sens * 8.0f, lfo = 0.0f;
            double oct, fc;
            if (env > 1.0f) env = 1.0f;
            if (s->hz > 0.0 && s->depth > 0.0f) lfo = (float)sin(2.0 * FA_PI * phase_at(s->hz, e->rate, frame + i, 0.0f)) * s->depth;
            oct = (env * s->range + lfo * s->range * 0.5) * (s->down ? -1.0 : 1.0);
            fc = s->base * pow(2.0, oct);
            if (fc > e->rate / 7.0) fc = e->rate / 7.0;
            if (fc < 40.0) fc = 40.0;
            s->coef[0] = (float)(2.0 * sin(FA_PI * fc / e->rate));
        }
        for (c = 0; c < 2; c++) {
            float* st = s->svf[c], hp;
            st[0] += s->coef[0] * st[1];
            hp = in[i * 2 + c] - st[0] - q * st[1];
            st[1] += s->coef[0] * hp;
            w[i * 2 + c] = s->ftype == FILT_LP ? st[0] : st[1] * (q < 1.0f ? 1.0f + q : 2.0f) * 0.7f;
        }
    }
}

static void run_rotary(fa_midi_efx* e, efx_stage* s, const float* in, float* w, int n)
{
    double base = 1.2 * e->rate / 1000.0, swing = 0.6 * e->rate / 1000.0;
    int i, k;
    for (i = 0; i < n; i++) {
        float m = (in[i * 2] + in[i * 2 + 1]) * 0.5f, lo = lowpass1(m, s->xover_a, &s->xz[0]), hi = m - lo;
        float sl, sh;
        for (k = 0; k < 2; k++) {
            float to = (float)(s->fast_on ? s->fast[k] : s->slow[k]);
            s->rot_hz[k] += (to - s->rot_hz[k]) * s->rot_acc[k];
            s->rot_ph[k] += s->rot_hz[k] / (float)e->rate;
            if (s->rot_ph[k] >= 1.0f) s->rot_ph[k] -= 1.0f;
        }
        sl = sinf(2.0f * (float)FA_PI * s->rot_ph[0]);
        sh = sinf(2.0f * (float)FA_PI * s->rot_ph[1]);
        wr(&s->line, hi, 0.0f);
        {
            float hl = rd(&s->line, 0, (float)(base + swing * sh * s->sep)), hr = rd(&s->line, 0, (float)(base - swing * sh * s->sep));
            float al = 1.0f + 0.35f * sh * s->sep, ar = 1.0f - 0.35f * sh * s->sep;
            float ll = 1.0f + 0.25f * sl * s->sep, lr = 1.0f - 0.25f * sl * s->sep;
            w[i * 2] = hl * al * s->rot_lev[1] + lo * ll * s->rot_lev[0];
            w[i * 2 + 1] = hr * ar * s->rot_lev[1] + lo * lr * s->rot_lev[0];
        }
    }
}

static void run_mod(fa_midi_efx* e, efx_stage* s, const float* in, float* w, int n, fa_frames frame)
{
    double inc[EFX_VOICES], ph[EFX_VOICES];
    float fbv[2] = { s->last[0], s->last[1] };
    int i, v;
    for (v = 0; v < s->voices; v++) {
        inc[v] = s->hz / e->rate;
        ph[v] = phase_at(s->hz, e->rate, frame, s->v_phase[v]);
    }
    for (i = 0; i < n; i++) {
        float l = in[i * 2], r = in[i * 2 + 1], ol = 0.0f, orr = 0.0f, first[2] = { 0.0f, 0.0f };
        int got[2] = { 0, 0 };
        if (s->pre_a > 0.0f) {
            float fl = lowpass1(l, s->pre_a, &s->pre_z[0]), fr = lowpass1(r, s->pre_a, &s->pre_z[1]);
            l = s->pre_hp ? l - fl : fl;
            r = s->pre_hp ? r - fr : fr;
        }
        wr(&s->line, l + fbv[0] * s->fb_gain, r + fbv[1] * s->fb_gain);
        for (v = 0; v < s->voices; v++) {
            float lfo, x;
            if (s->step > 0) {
                if ((frame + i) % s->step == 0 || (i == 0 && s->held[v] == 0.0f)) s->held[v] = 1.0f + wave_at(s->wave, ph[v]);
                lfo = s->held[v] - 1.0f;
            } else {
                lfo = wave_at(s->wave, ph[v]);
            }
            x = rd(&s->line, s->v_ch[v], s->v_pre[v] + s->v_depth[v] * (0.5f + 0.5f * lfo));
            ol += x * s->v_gl[v];
            orr += x * s->v_gr[v];
            if (!got[s->v_ch[v]]) { first[s->v_ch[v]] = x; got[s->v_ch[v]] = 1; }
            ph[v] += inc[v];
            if (ph[v] >= 1.0) ph[v] -= 1.0;
        }
        fbv[0] = first[0];
        fbv[1] = got[1] ? first[1] : first[0];
        if (s->trem_depth > 0.0f) {
            double tp = phase_at(s->trem_hz, e->rate, frame + i, 0.0f);
            ol *= 1.0f - s->trem_depth * (0.5f + 0.5f * (float)sin(2.0 * FA_PI * tp));
            orr *= 1.0f - s->trem_depth * (0.5f + 0.5f * (float)sin(2.0 * FA_PI * (tp + s->trem_phase)));
        }
        w[i * 2] = ol;
        w[i * 2 + 1] = orr;
    }
    s->last[0] = fbv[0];
    s->last[1] = fbv[1];
}

static void run_tremolo(fa_midi_efx* e, efx_stage* s, const float* in, float* w, int n, fa_frames frame)
{
    double inc = s->hz / e->rate, ph = phase_at(s->hz, e->rate, frame, 0.0f);
    int i;
    for (i = 0; i < n; i++, ph += inc) {
        double pr = ph + s->phase;
        if (ph >= 1.0) ph -= 1.0;
        if (pr >= 1.0) pr -= 1.0;
        w[i * 2] = in[i * 2] * (1.0f - s->depth * (0.5f + 0.5f * wave_at(s->wave, ph)));
        w[i * 2 + 1] = in[i * 2 + 1] * (1.0f - s->depth * (0.5f + 0.5f * wave_at(s->wave, pr)));
    }
}

static void run_dynamics(efx_stage* s, const float* in, float* w, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        float lvl = fabsf(in[i * 2]) > fabsf(in[i * 2 + 1]) ? fabsf(in[i * 2]) : fabsf(in[i * 2 + 1]), g;
        float db;
        s->env += (lvl - s->env) * (lvl > s->env ? s->att : s->rel);
        db = 20.0f * log10f(s->env + 1e-9f);
        if (s->dyn == DYN_GATE) {
            float to = db > s->thresh ? 1.0f : 0.0f;
            s->gate_g += (to - s->gate_g) * (to > s->gate_g ? s->att : s->rel);
            g = s->gate_g;
        } else {
            float over = db - s->thresh;
            g = over > 0.0f ? powf(10.0f, -over * (1.0f - 1.0f / s->ratio) / 20.0f) : 1.0f;
        }
        w[i * 2] = in[i * 2] * g * s->makeup;
        w[i * 2 + 1] = in[i * 2 + 1] * g * s->makeup;
    }
}

static void run_delay(fa_midi_efx* e, efx_stage* s, const float* in, float* w, int n, fa_frames frame)
{
    double inc = s->hz / e->rate, ph = s->depth > 0.0f ? phase_at(s->hz, e->rate, frame, 0.0f) : 0.0;
    int i, k, c;
    for (i = 0; i < n; i++) {
        float x[2] = { in[i * 2], in[i * 2 + 1] }, ol = 0.0f, orr = 0.0f, fbv[2], mod[2] = { 0.0f, 0.0f };
        if (s->depth > 0.0f) {
            double pr = ph + s->phase;
            mod[0] = s->depth * (0.5f + 0.5f * (float)sin(2.0 * FA_PI * ph));
            mod[1] = s->depth * (0.5f + 0.5f * (float)sin(2.0 * FA_PI * pr));
            ph += inc;
            if (ph >= 1.0) ph -= 1.0;
        }
        if (s->glide > 0.0f) s->tap_now += (s->tap_t[0] - s->tap_now) * s->glide;
        for (k = 0; k < s->ntaps; k++) {
            float t = (k == 0 && s->glide > 0.0f ? s->tap_now : s->tap_t[k]) + mod[s->tap_ch[k]];
            float v = rd(&s->line, s->tap_ch[k], t);
            ol += v * s->tap_gl[k];
            orr += v * s->tap_gr[k];
        }
        for (c = 0; c < 2; c++) {
            int from = s->fb_cross ? 1 - c : c;
            float t = s->fb_t[from];
            if (s->glide > 0.0f && t > 0.0f) t = s->tap_now;
            fbv[c] = t > 0.0f && s->fb_gain != 0.0f ? lowpass1(rd(&s->line, from, t + mod[from]), s->damp_a, &s->damp_z[c]) * s->fb_gain : 0.0f;
            if (s->pre_a > 0.0f) {
                float f = lowpass1(x[c], s->pre_a, &s->pre_z[c]);
                x[c] = s->pre_hp ? x[c] - f : f;
            }
        }
        wr(&s->line, x[0] + fbv[0], x[1] + fbv[1]);
        w[i * 2] = ol;
        w[i * 2 + 1] = orr;
    }
}

static void run_pitch(efx_stage* s, const float* in, float* w, int n)
{
    int i, v;
    for (i = 0; i < n; i++) {
        float m = (in[i * 2] + in[i * 2 + 1]) * 0.5f, ol = 0.0f, orr = 0.0f, fb = 0.0f;
        wr(&s->line, m + s->last[0] * s->fb_gain, 0.0f);
        for (v = 0; v < s->voices; v++) {
            float p = s->v_ph[v], q = p + 0.5f, g1, x;
            if (q >= 1.0f) q -= 1.0f;
            g1 = 1.0f - fabsf(2.0f * p - 1.0f);
            x = rd(&s->line, 0, s->v_pre[v] + 1.0f + p * s->window) * g1 +
                rd(&s->line, 0, s->v_pre[v] + 1.0f + q * s->window) * (1.0f - g1);
            ol += x * s->v_gl[v];
            orr += x * s->v_gr[v];
            if (v == 0) fb = x;
            p += (1.0f - s->v_ratio[v]) / s->window;
            while (p >= 1.0f) p -= 1.0f;
            while (p < 0.0f) p += 1.0f;
            s->v_ph[v] = p;
        }
        s->last[0] = fb;
        w[i * 2] = ol;
        w[i * 2 + 1] = orr;
    }
}

static void run_pan3d(fa_midi_efx* e, efx_stage* s, const float* in, float* w, int n, fa_frames frame)
{
    float itd = 0.6f * (float)e->rate / 1000.0f;
    int i;
    for (i = 0; i < n; i++) {
        float az = s->az, sn, gl, gr;
        if (s->az_speed > 0.0f) {
            double ph = phase_at(s->az_speed, e->rate, frame + i, 0.0f);
            az = s->az_turn ? s->az_from + 360.0f * (float)ph * (s->hz < 0.0 ? -1.0f : 1.0f)
                            : s->az_from * (float)sin(2.0 * FA_PI * ph);
        }
        sn = sinf(az * (float)FA_PI / 180.0f);
        gl = sqrtf(0.5f * (1.0f - sn));
        gr = sqrtf(0.5f * (1.0f + sn));
        if (cosf(az * (float)FA_PI / 180.0f) < 0.0f) { gl *= 0.8f; gr *= 0.8f; }   /* behind: a little quieter */
        wr(&s->line, (in[i * 2] + in[i * 2 + 1]) * 0.5f, 0.0f);
        w[i * 2] = rd(&s->line, 0, 1.0f + (sn > 0.0f ? itd * sn : 0.0f)) * gl;
        w[i * 2 + 1] = rd(&s->line, 0, 1.0f + (sn < 0.0f ? -itd * sn : 0.0f)) * gr;
    }
}

static void run_lofi(fa_midi_efx* e, efx_stage* s, const float* in, float* w, int n, fa_frames frame)
{
    int i, c, k;
    for (i = 0; i < n; i++) {
        float lvl = fabsf(in[i * 2]) + fabsf(in[i * 2 + 1]), nz[2] = { 0.0f, 0.0f };
        s->env += (lvl - s->env) * (lvl > s->env ? 0.01f : 0.0002f);
        if (s->hold_i == 0)
            for (c = 0; c < 2; c++) s->hold[c] = lowpass1(in[i * 2 + c], s->pre_a, &s->pre_z[c]);
        if (++s->hold_i >= s->hold_n) s->hold_i = 0;
        /* Noises ride on the sound (they follow the input's level, so silence stays silent). */
        for (k = 0; k < 4; k++) {
            float v;
            if (s->noise[k] <= 0.0f) continue;
            if (k == 3) {
                double ph = phase_at(s->hum_hz, e->rate, frame + i, 0.0f);
                v = (float)(sin(2.0 * FA_PI * ph) + 0.5 * sin(6.0 * FA_PI * ph));
            } else if (k == 2) {
                float r = noise_at(frame + i, 7u);
                v = r > 1.0f - 0.0004f * (float)(s->crackle + 1) ? noise_at(frame + i, 11u) * 4.0f : 0.0f;
            } else {
                v = noise_at(frame + i, (uint32_t)k + 1u);
            }
            for (c = 0; c < 2; c++) {
                float x = lowpass1(c ? noise_at(frame + i, (uint32_t)k + 5u) * (k < 2) + v * (k >= 2) : v, s->noise_lp[k], &s->noise_z[k][c]);
                nz[c] += x * s->noise[k];
            }
        }
        for (c = 0; c < 2; c++) {
            float x = s->hold[c];
            if (s->levels > 0.0f) x = floorf(x * s->levels + 0.5f) / s->levels;
            w[i * 2 + c] = x + nz[c] * (s->env > 1.0f ? 1.0f : s->env);
        }
        if (s->mono_out) w[i * 2] = w[i * 2 + 1] = (w[i * 2] + w[i * 2 + 1]) * 0.5f;
    }
}

static void run_ring(fa_midi_efx* e, efx_stage* s, const float* in, float* w, int n, fa_frames frame)
{
    double inc = s->ring_hz / e->rate, ph = phase_at(s->ring_hz, e->rate, frame, 0.0f);
    int i;
    for (i = 0; i < n; i++, ph += inc) {
        float m = (float)sin(2.0 * FA_PI * ph);
        w[i * 2] = in[i * 2] * m;
        w[i * 2 + 1] = in[i * 2 + 1] * m;
    }
}

static void run_cancel(efx_stage* s, const float* in, float* w, int n)
{
    int i, c;
    for (i = 0; i < n; i++) {
        float lo[2], hi[2], mid[2];
        for (c = 0; c < 2; c++) {
            float x = in[i * 2 + c];
            lo[c] = lowpass1(x, s->pre_a, &s->pre_z[c]);
            hi[c] = x - lowpass1(x, s->damp_a, &s->damp_z[c]);
            mid[c] = x - lo[c] - hi[c];
        }
        w[i * 2] = lo[0] + hi[0] + (mid[0] - mid[1]);
        w[i * 2 + 1] = lo[1] + hi[1] + (mid[1] - mid[0]);
    }
}

void efx_stage_run(fa_midi_efx* e, efx_stage* s, float* x, int n, fa_frames frame)
{
    float m[EFX_CHUNK * 2], *w = e->c;
    const float* in = x;
    int i, k;
    if (s->kind == ST_NONE) return;
    if (s->mono_in) {
        for (i = 0; i < n; i++) m[i * 2] = m[i * 2 + 1] = (x[i * 2] + x[i * 2 + 1]) * 0.5f;
        in = m;
    }
    switch (s->kind) {
    case ST_DRIVE: run_drive(s, in, w, n); break;
    case ST_ENHANCE: run_enhance(s, in, w, n); break;
    case ST_FORMANT: run_formant(e, s, in, w, n, frame); break;
    case ST_PHASER: run_phaser(e, s, in, w, n, frame); break;
    case ST_WAH: run_wah(e, s, in, w, n, frame); break;
    case ST_ROTARY: run_rotary(e, s, in, w, n); break;
    case ST_MOD: run_mod(e, s, in, w, n, frame); break;
    case ST_TREMOLO: run_tremolo(e, s, in, w, n, frame); break;
    case ST_DYNAMICS: run_dynamics(s, in, w, n); break;
    case ST_DELAY: run_delay(e, s, in, w, n, frame); break;
    case ST_REVERB:
        memset(w, 0, (size_t)n * 2 * sizeof(float));
        if (s->sub) fa_midi_fx_reverb(s->sub, in, w, n);
        break;
    case ST_PITCH: run_pitch(s, in, w, n); break;
    case ST_PAN3D: run_pan3d(e, s, in, w, n, frame); break;
    case ST_LOFI: run_lofi(e, s, in, w, n, frame); break;
    case ST_RING: run_ring(e, s, in, w, n, frame); break;
    case ST_CANCEL: run_cancel(s, in, w, n); break;
    default: memcpy(w, in, (size_t)n * 2 * sizeof(float)); break;   /* ST_EQ: the sections below */
    }
    for (k = 0; k < s->nsec; k++)
        for (i = 0; i < n; i++) {
            w[i * 2] = fa_biquad_run(&s->sec[k], s->z[k][0], w[i * 2]);
            w[i * 2 + 1] = fa_biquad_run(&s->sec[k], s->z[k][1], w[i * 2 + 1]);
        }
    if (s->pan >= 0.0f) {
        float gl = s->pan <= 0.5f ? 1.0f : (1.0f - s->pan) * 2.0f, gr = s->pan >= 0.5f ? 1.0f : s->pan * 2.0f;
        for (i = 0; i < n; i++) {
            float v = (w[i * 2] + w[i * 2 + 1]) * 0.5f;
            w[i * 2] = v * gl;
            w[i * 2 + 1] = v * gr;
        }
    }
    for (i = 0; i < n * 2; i++) x[i] = x[i] * s->dry + w[i] * s->wet;
}

/* ---- the effect ----------------------------------------------------------------- */

fa_midi_efx* fa_midi_efx_new(uint32_t rate)
{
    fa_midi_efx* e = (fa_midi_efx*)calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->rate = rate;
    e->idle = 1;
    return e;
}

void fa_midi_efx_free(fa_midi_efx* e)
{
    int i;
    if (!e) return;
    for (i = 0; i < EFX_STAGES; i++) efx_stage_free(&e->st[i]);
    free(e);
}

void fa_midi_efx_reset(fa_midi_efx* e)
{
    int i;
    if (!e) return;
    for (i = 0; i < EFX_STAGES; i++) efx_stage_clear(&e->st[i]);
    memset(e->lz, 0, sizeof(e->lz));
    memset(e->hz2, 0, sizeof(e->hz2));
    e->soft = 0;
    e->idle = 1;
}

/* Rebuilds the chain from the parameters (and controls) without clearing its state. */
static void rebuild(fa_midi_efx* e)
{
    int i;
    if (!e->t) return;
    efx_build(e);
    e->tail = EFX_FILTER_TAIL;
    for (i = 0; i < e->nst; i++) if (e->st[i].need + EFX_FILTER_TAIL > e->tail) e->tail = e->st[i].need + EFX_FILTER_TAIL;
}

int fa_midi_efx_known(int gs, int msb, int lsb, int exact)
{
    const efx_type* t = gs ? efx_find_gs(msb, lsb) : efx_find_xg(msb, lsb);
    return t && (!exact || efx_type_lsb(t) == lsb);
}

int fa_midi_efx_set_type(fa_midi_efx* e, int gs, int msb, int lsb)
{
    int i;
    if (!e) return 0;
    for (i = 0; i < EFX_STAGES; i++) efx_stage_free(&e->st[i]);
    e->nst = e->par = 0;
    e->ctl[0] = e->ctl[1] = e->xg_ctl = 0.0f;
    e->t = gs ? efx_find_gs(msb, lsb) : efx_find_xg(msb, lsb);
    memset(e->p, 0, sizeof(e->p));
    fa_midi_efx_reset(e);
    if (!e->t) return 0;
    efx_load_defaults(e);
    rebuild(e);
    for (i = 0; i < EFX_STAGES; i++) efx_stage_clear(&e->st[i]);   /* the glides start where the settings are */
    return 1;
}

void fa_midi_efx_set_param(fa_midi_efx* e, int index, int value)
{
    if (!e || !e->t || index < 0 || index >= 20 || e->p[index] == value) return;
    e->p[index] = value;
    rebuild(e);
}

int fa_midi_efx_param(const fa_midi_efx* e, int index)
{
    return e && e->t && index >= 0 && index < 20 ? e->p[index] : 0;
}

int fa_midi_efx_dry_wet(const fa_midi_efx* e)
{
    int k;
    if (!e || !e->t || efx_type_is_gs(e->t)) return 127;
    k = efx_type_dry_wet(e->t);
    if (k < 0) return 127;
    /* The controllers move it when it is the type's controlled parameter (as X() does). */
    if (efx_type_ctl(e->t, 0) == k && e->xg_ctl != 0.0f) {
        int v = (int)lrintf((float)e->p[k] + e->xg_ctl * 127.0f);
        return v < 0 ? 0 : v > 127 ? 127 : v;
    }
    return e->p[k];
}

void fa_midi_efx_control(fa_midi_efx* e, int which, float amount)
{
    float* c;
    if (!e || !e->t || which < 0 || which > 2) return;
    amount = amount < -1.0f ? -1.0f : amount > 1.0f ? 1.0f : amount;
    c = which == 2 ? &e->xg_ctl : &e->ctl[which];
    if (*c == amount) return;
    *c = amount;
    rebuild(e);
}

static float peak_of(const float* x, int n)
{
    float p = 0.0f;
    int i;
    for (i = 0; i < n; i++) if (fabsf(x[i]) > p) p = fabsf(x[i]);
    return p;
}

int fa_midi_efx_process(fa_midi_efx* e, const float* in, float* out, int frames, fa_frames song_frame)
{
    float loud;
    int off, n, i, k;
    if (!e || !e->t || frames <= 0) return 0;
    if (fa_all_zero(in, (uint32_t)frames * 2) && e->idle) return 0;
    for (k = 0; k < e->nst; k++) if (!efx_stage_ready(e, &e->st[k])) return 0;   /* no memory: silent */
    e->idle = 0;
    loud = peak_of(in, frames * 2);
    for (off = 0; off < frames; off += n) {
        float* a = out + off * 2;
        fa_frames f = song_frame + off;
        n = frames - off < EFX_CHUNK ? frames - off : EFX_CHUNK;
        memcpy(a, in + off * 2, (size_t)n * 2 * sizeof(float));
        if (e->par > 0) {
            memcpy(e->b, a, (size_t)n * 2 * sizeof(float));
            for (k = 0; k < e->par; k++) efx_stage_run(e, &e->st[k], a, n, f);
            for (k = e->par; k < e->nst; k++) efx_stage_run(e, &e->st[k], e->b, n, f);
            for (i = 0; i < n; i++) {
                float al = a[i * 2], ar = a[i * 2 + 1];
                float bl = e->b[i * 2], br = e->b[i * 2 + 1];
                a[i * 2] = (al + ar) * 0.5f * e->par_g[0][0] + (bl + br) * 0.5f * e->par_g[1][0];
                a[i * 2 + 1] = (al + ar) * 0.5f * e->par_g[0][1] + (bl + br) * 0.5f * e->par_g[1][1];
            }
        } else {
            for (k = 0; k < e->nst; k++) efx_stage_run(e, &e->st[k], a, n, f);
        }
        if (e->post) {
            for (i = 0; i < n; i++) {
                float l = fa_biquad_run(&e->hi, e->hz2[0], fa_biquad_run(&e->lo, e->lz[0], a[i * 2]));
                float r = fa_biquad_run(&e->hi, e->hz2[1], fa_biquad_run(&e->lo, e->lz[1], a[i * 2 + 1]));
                if (e->out_mono) l = r = (l + r) * 0.5f;
                a[i * 2] = l * e->out_gl;
                a[i * 2 + 1] = r * e->out_gr;
            }
        }
    }
    {
        float o = peak_of(out, frames * 2);
        if (o > loud) loud = o;
    }
    e->soft = loud >= EFX_QUIET ? 0 : e->soft + frames;
    if (e->soft > e->tail) fa_midi_efx_reset(e);
    return 1;
}
