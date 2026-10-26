/* The DJ mixer's mic bus: what both mics share after their channel strips (mic.c).
 *
 *   mics (heard) -+-> vox ------------------------------------+-> + music (ducked) -> master
 *                 +-> reverb send -> reverb (midi_fx.c) ------+
 *                 '-> echo send ---> echo --------------------'
 *   mics (monitor off) -> dry;  vox + dry -> the vocals tap (recording)
 *   host mics (FA_MIC_DUCKS_MUSIC) -> level -> ducker -> gain on the music
 *
 * The reverb is the MIDI synthesizer's (Freeverb-style), with Freeverb's size and
 * damping ranges. The echo is one delay line with feedback through a one-pole
 * low-pass, its time gliding so a change bends the repeats instead of clicking. The
 * ducker follows the hosts' gated voice: above the threshold the music goes down by
 * the depth at the attack rate, and after the hold comes back at the release rate,
 * linear in dB. All of it is per-sample work on the audio thread, no locks, no
 * allocation: the bus is made with the first mic, and the settings are atomics read
 * at each block. */
#include "mixer.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define REVERB_IN_GAIN 0.015f    /* Freeverb's fixed input gain */
#define REVERB_WET 1.0f
#define ECHO_GLIDE_MS 50.0       /* the delay time glides over about this long */
#define ECHO_MIN_MS 20.0f
#define ECHO_TONE_LOW_HZ 1000.0
#define ECHO_TONE_SPAN 16.0      /* the tone's range: 1 kHz x 16 = 16 kHz */
#define DUCK_FOLLOW_MS 10.0
#define QUIET 1e-7f              /* -140 dB: an echo tail below this is cleared */

FA_API void FA_CALL fa_mic_effects_init(fa_mic_effects* effects)
{
    if (!effects) return;
    memset(effects, 0, sizeof(*effects));
    effects->struct_size = (uint32_t)sizeof(*effects);
    effects->reverb_size = 0.5f;
    effects->reverb_damping = 0.5f;
    effects->echo_time_ms = 250.0f;
    effects->echo_feedback = 0.35f;
    effects->echo_tone = 0.6f;
}

FA_API void FA_CALL fa_ducking_init(fa_ducking* ducking)
{
    if (!ducking) return;
    memset(ducking, 0, sizeof(*ducking));
    ducking->struct_size = (uint32_t)sizeof(*ducking);
    ducking->depth_db = -12.0f;
    ducking->attack = 50000000;
    ducking->hold = 300000000;
    ducking->release = 600000000;
    ducking->threshold_db = -40.0f;
}

static fa_time clamp_time(fa_time t, fa_time lo, fa_time hi) { return t < lo ? lo : t > hi ? hi : t; }

static int bad(float v) { return v != v; }

static void store_effects(struct fa_mixer* m, const fa_mic_effects* e)
{
    fa_store_float(&m->fx[MFX_REVERB_SIZE], clampf(e->reverb_size, 0.0f, 1.0f));
    fa_store_float(&m->fx[MFX_REVERB_DAMPING], clampf(e->reverb_damping, 0.0f, 1.0f));
    fa_store_float(&m->fx[MFX_ECHO_MS], clampf(e->echo_time_ms, ECHO_MIN_MS, (float)ECHO_MAX_MS));
    fa_store_float(&m->fx[MFX_ECHO_FEEDBACK], clampf(e->echo_feedback, 0.0f, 0.9f));
    fa_store_float(&m->fx[MFX_ECHO_TONE], clampf(e->echo_tone, 0.0f, 1.0f));
}

static void store_ducking(struct fa_mixer* m, const fa_ducking* d)
{
    fa_store_float(&m->duck_depth, clampf(d->depth_db, -40.0f, 0.0f));
    fa_store_float(&m->duck_threshold, clampf(d->threshold_db, -60.0f, 0.0f));
    fa_store64(&m->duck_attack, clamp_time(d->attack, 1000000, 1000000000));
    fa_store64(&m->duck_hold, clamp_time(d->hold, 0, 5000000000LL));
    fa_store64(&m->duck_release, clamp_time(d->release, 10000000, 5000000000LL));
}

void fa_mixer_fx_defaults(struct fa_mixer* m)
{
    fa_mic_effects e;
    fa_ducking d;
    fa_mic_effects_init(&e);
    fa_ducking_init(&d);
    store_effects(m, &e);
    store_ducking(m, &d);
}

FA_API fa_result FA_CALL fa_mixer_set_mic_effects(fa_mixer* mixer, const fa_mic_effects* effects)
{
    const fa_mic_effects* e = effects;
    if (!mixer || !e || e->struct_size < sizeof(fa_mic_effects)) return FA_ERROR_INVALID_ARG;
    if (bad(e->reverb_size) || bad(e->reverb_damping) || bad(e->echo_time_ms) || bad(e->echo_feedback) || bad(e->echo_tone))
        return FA_ERROR_INVALID_ARG;
    store_effects(mixer, e);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_mixer_get_mic_effects(fa_mixer* mixer, fa_mic_effects* out_effects)
{
    if (!mixer || !out_effects || out_effects->struct_size < sizeof(fa_mic_effects)) return FA_ERROR_INVALID_ARG;
    out_effects->reverb_size = fa_load_float(&mixer->fx[MFX_REVERB_SIZE]);
    out_effects->reverb_damping = fa_load_float(&mixer->fx[MFX_REVERB_DAMPING]);
    out_effects->echo_time_ms = fa_load_float(&mixer->fx[MFX_ECHO_MS]);
    out_effects->echo_feedback = fa_load_float(&mixer->fx[MFX_ECHO_FEEDBACK]);
    out_effects->echo_tone = fa_load_float(&mixer->fx[MFX_ECHO_TONE]);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_mixer_set_ducking(fa_mixer* mixer, const fa_ducking* ducking)
{
    fa_ducking off;
    if (!mixer) return FA_ERROR_INVALID_ARG;
    if (!ducking) {
        fa_ducking_init(&off);
        off.depth_db = 0.0f;
        ducking = &off;
    }
    if (ducking->struct_size < sizeof(fa_ducking) || bad(ducking->depth_db) || bad(ducking->threshold_db))
        return FA_ERROR_INVALID_ARG;
    store_ducking(mixer, ducking);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_mixer_get_ducking(fa_mixer* mixer, fa_ducking* out_ducking)
{
    if (!mixer || !out_ducking || out_ducking->struct_size < sizeof(fa_ducking)) return FA_ERROR_INVALID_ARG;
    out_ducking->depth_db = fa_load_float(&mixer->duck_depth);
    out_ducking->threshold_db = fa_load_float(&mixer->duck_threshold);
    out_ducking->attack = fa_load64(&mixer->duck_attack);
    out_ducking->hold = fa_load64(&mixer->duck_hold);
    out_ducking->release = fa_load64(&mixer->duck_release);
    return FA_OK;
}

/* ---- the bus (made under the mixer lock) ---------------------------------------- */

void fa_mic_bus_free(fa_mic_bus* b)
{
    if (!b) return;
    fa_midi_fx_free(b->reverb);
    free(b->echo);
    free(b);
}

fa_result fa_mic_bus_make(struct fa_mixer* m)
{
    fa_mic_bus* b;
    uint32_t rate = m->device->rate;
    if (fa_load_ptr(&m->bus)) return FA_OK;
    b = (fa_mic_bus*)calloc(1, sizeof(*b));
    if (!b) return FA_ERROR_OUT_OF_MEMORY;
    b->rate = rate;
    b->reverb = fa_midi_fx_new(rate);
    b->echo_len = (uint32_t)ceil(ECHO_MAX_MS * 0.001 * rate) + 4;
    b->echo = (float*)calloc(b->echo_len, sizeof(float));
    if (!b->reverb || !b->echo) {
        fa_mic_bus_free(b);
        return FA_ERROR_OUT_OF_MEMORY;
    }
    b->echo_delay = fa_load_float(&m->fx[MFX_ECHO_MS]) * 0.001 * rate;
    b->echo_fb = fa_load_float(&m->fx[MFX_ECHO_FEEDBACK]);
    b->echo_idle = 1;
    fa_exchange_ptr(&m->bus, b);
    return FA_OK;
}

/* ---- audio thread ---------------------------------------------------------------- */

/* The echo: the send through the delay line into vox (both sides alike). */
static void echo_process(struct fa_mixer* m, fa_mic_bus* b, uint32_t frames)
{
    double target = fa_load_float(&m->fx[MFX_ECHO_MS]) * 0.001 * b->rate;
    float fb_to = fa_load_float(&m->fx[MFX_ECHO_FEEDBACK]);
    double glide, hz, most = (double)b->echo_len - 3.0;
    float a, fb_step;
    int64_t since;
    uint32_t i;
    if (b->echo_idle && fa_all_zero(b->esend, frames)) {
        b->echo_delay = target;   /* nothing to bend: take the new time at once */
        b->echo_fb = fb_to;
        return;
    }
    glide = 1.0 - exp(-1.0 / (ECHO_GLIDE_MS * 0.001 * b->rate));
    hz = fmin(ECHO_TONE_LOW_HZ * pow(ECHO_TONE_SPAN, fa_load_float(&m->fx[MFX_ECHO_TONE])), 0.45 * b->rate);
    a = (float)exp(-2.0 * FA_PI * hz / b->rate);
    fb_step = (fb_to - b->echo_fb) / (float)frames;
    b->echo_idle = 0;
    since = b->echo_quiet + frames;
    if (target > most) target = most;
    for (i = 0; i < frames; i++) {
        double rp;
        uint32_t i0, i1;
        float fr, y;
        b->echo_delay += (target - b->echo_delay) * glide;
        rp = (double)b->echo_pos - b->echo_delay;
        if (rp < 0.0) rp += b->echo_len;
        i0 = (uint32_t)rp;
        fr = (float)(rp - i0);
        if (i0 >= b->echo_len) i0 -= b->echo_len;
        i1 = i0 + 1 == b->echo_len ? 0 : i0 + 1;
        y = b->echo[i0] + (b->echo[i1] - b->echo[i0]) * fr;
        b->echo_lp = y + (b->echo_lp - y) * a;
        if (fabsf(b->echo_lp) < 1e-20f) b->echo_lp = 0.0f;   /* no denormals as a repeat dies away */
        b->echo_fb += fb_step;
        b->echo[b->echo_pos] = b->esend[i] + b->echo_lp * b->echo_fb;
        if (++b->echo_pos == b->echo_len) b->echo_pos = 0;
        b->vox[i * 2] += y;
        b->vox[i * 2 + 1] += y;
        /* The line only holds what went in or came out within its length: a repeat
         * comes back until a whole line's worth of both has been quiet. */
        if (b->esend[i] != 0.0f || fabsf(y) >= QUIET) since = (int64_t)(frames - 1 - i);
    }
    b->echo_quiet = since;
    b->echo_fb = fb_to;
    /* Cleared once the last input has had time to come back and the repeats are gone:
     * not on a quiet block between two repeats, which a short phrase leaves. */
    if (b->echo_quiet > (int64_t)b->echo_len) {
        memset(b->echo, 0, (size_t)b->echo_len * sizeof(float));
        b->echo_lp = 0.0f;
        b->echo_idle = 1;
    }
}

/* The ducker: the hosts' level (b->duck) becomes the music's gain in dB, applied to
 * `out` in MIXER_SUB steps. */
static void duck_process(struct fa_mixer* m, fa_mic_bus* b, float* out, uint32_t frames)
{
    float depth, thr, follow, att, rel, down, up, g_prev;
    int64_t hold;
    uint32_t i, off;
    if (!b->hosts && b->duck_db == 0.0f && b->duck_env == 0.0f) return;
    depth = fa_load_float(&m->duck_depth);
    thr = fa_db_to_lin(fa_load_float(&m->duck_threshold));
    follow = expf(-1.0f / ((float)DUCK_FOLLOW_MS * 0.001f * (float)b->rate));
    att = (float)fa_load64(&m->duck_attack) * 1e-9f * (float)b->rate;
    rel = (float)fa_load64(&m->duck_release) * 1e-9f * (float)b->rate;
    /* dB per frame; turned off while ducked, the music comes back 12 dB per release. */
    down = -depth / att;
    up = (depth < 0.0f ? -depth : 12.0f) / rel;
    hold = (int64_t)((double)fa_load64(&m->duck_hold) * 1e-9 * b->rate);
    g_prev = fa_db_to_lin(b->duck_db);
    for (i = 0; i < frames; i++) {
        float x = b->hosts ? b->duck[i] : 0.0f;
        b->duck_env = x > b->duck_env ? x : b->duck_env * follow;
        if (depth < 0.0f && b->duck_env >= thr) b->duck_hold = hold + 1;
        if (b->duck_hold > 0) {
            b->duck_hold--;
            /* towards the depth, also from below when it was made shallower */
            b->duck_db = b->duck_db > depth ? fmaxf(depth, b->duck_db - down) : fminf(depth, b->duck_db + up);
        } else {
            b->duck_db = fminf(0.0f, b->duck_db + up);
        }
        b->duck[i] = b->duck_db;
    }
    if (b->duck_env < 1e-20f) b->duck_env = 0.0f;
    for (off = 0; off < frames; off += MIXER_SUB) {
        uint32_t n = frames - off < MIXER_SUB ? frames - off : MIXER_SUB, j;
        float g1 = fa_db_to_lin(b->duck[off + n - 1]), inv = 1.0f / (float)n;
        if (g_prev == 1.0f && g1 == 1.0f) continue;
        for (j = 0; j < n; j++) {
            float g = g_prev + (g1 - g_prev) * (float)(j + 1) * inv;
            out[(size_t)(off + j) * 2] *= g;
            out[(size_t)(off + j) * 2 + 1] *= g;
        }
        g_prev = g1;
    }
}

void fa_mic_bus_process(struct fa_mixer* m, fa_mic_bus* b, int64_t clock, float* out, uint32_t frames)
{
    float size = fa_load_float(&m->fx[MFX_REVERB_SIZE]), damping = fa_load_float(&m->fx[MFX_REVERB_DAMPING]);
    uint32_t i, n = frames * FA_DEVICE_CHANNELS;
    /* Freeverb's ranges: room 0.7 .. 0.98, damping 0 .. 0.4. */
    fa_midi_fx_set_reverb(b->reverb, 0.7f + 0.28f * size, 0.4f * damping, REVERB_IN_GAIN, REVERB_WET);
    fa_midi_fx_reverb(b->reverb, b->rsend, b->vox, (int)frames);
    echo_process(m, b, frames);
    duck_process(m, b, out, frames);
    for (i = 0; i < n; i++) out[i] += b->vox[i];
    if (fa_load_ptr(&m->encoders)) {
        for (i = 0; i < n; i++) b->vox[i] += b->dry[i];
        fa_encode_mixer_push(m, FA_MIXER_TAP_VOCALS, b->vox, frames, clock);
    }
}

int64_t fa_mixer_vocal_shift(struct fa_mixer* m)
{
    fa_mixer_latency l;
    int64_t most = 0;
    int i;
    l.struct_size = (uint32_t)sizeof(l);
    if (fa_mixer_get_latency(m, &l) != FA_OK) return 0;
    for (i = 0; i < FA_MIC_MAX; i++)
        if (l.mic_monitor[i] > most) most = l.mic_monitor[i];
    if (most == 0) most = l.master;   /* no mic: the master's own */
    return (int64_t)((double)most * 1e-9 * m->device->rate + 0.5);
}
