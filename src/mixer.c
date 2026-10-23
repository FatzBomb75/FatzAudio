/* The DJ mixer: decks on one output device, each a mix bus with a buffer of its
 * own, so a deck can carry a trim, an isolator EQ, a fader, a meter and a cue send
 * before the crossfader sums it into the master.
 *
 * The audio thread mixes each deck's streams into the deck's buffer (render_block in
 * device.c), then fa_mixer_process runs the deck channels and the crossfader into
 * the master, before the device's limiter (always on here). fa_mixer_render, the
 * device callback of a mixer device, then lays the master and the cue out on the
 * device's channels. Parameters are lanes on the device clock, as buses use: sets
 * glide over 5 ms, ramps start on their device frame. Nothing here locks or
 * allocates on the audio thread. */
#include "fa_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MIXER_MAX_DECKS 2
#define MIXER_DEZIPPER_NS 5000000
#define MIXER_SUB 16             /* gains are exact every 16 frames, linear in between */
#define ISO_LOW_HZ 250.0
#define ISO_HIGH_HZ 2500.0
#define ISO_SECTIONS 9
#define DECK_TEMPO_LATENCY_MS 60

static const double k_pi = 3.14159265358979323846;

enum { DL_GAIN = 0, DL_TRIM, DL_LOW, DL_MID, DL_HIGH, DL_CUE, DECK_LANES };
enum { ML_XF = 0, ML_MASTER, ML_CUE_MIX, ML_CUE_GAIN, MIX_LANES };

typedef struct biquad {
    float b0, b1, b2, a1, a2;
} biquad;

struct fa_deck {
    struct fa_mixer* mixer;
    uint32_t index;
    fa_bus* bus;                 /* its streams mix into bus->buf */
    fa_atomic_ptr stream;        /* fa_stream*; written under the mixer lock, or cleared when it leaves */
    fa_frames cue_point;         /* mixer lock; FA_FRAMES_UNSET = the stream's CUE_IN */
    fa_lane lanes[DECK_LANES];
    fa_atomic32 side;            /* FA_DECK_XFADER_SIDE */
    fa_atomic_ptr tap;           /* fa_tap*, the channel meter (post fader) */
    /* audio thread */
    float side_w[3];             /* thru, A, B: glide between sides over 5 ms */
    float z[ISO_SECTIONS][2][2]; /* isolator filter memory, per section and channel */
};

struct fa_mixer {
    fa_engine* engine;
    fa_device* device;
    fa_mutex lock;               /* API calls */
    uint32_t deck_count;
    struct fa_deck decks[MIXER_MAX_DECKS];
    fa_lane lanes[MIX_LANES];
    fa_atomic32 curve;           /* fa_xfader_curve */
    fa_cue_mode cue_mode;
    uint32_t cue_channel;
    /* The isolator: LR4 low/high at 250 Hz (2 + 2 sections), LR4 low/high at 2.5 kHz on
     * the upper band (2 + 2), and an allpass at 2.5 kHz on the low band so the three
     * sum flat. */
    biquad lp1, hp1, lp2, hp2, ap2;
    /* audio thread */
    float master[FA_MIXER_BLOCK * FA_DEVICE_CHANNELS];
    float cue[FA_MIXER_BLOCK * FA_DEVICE_CHANNELS];   /* the cue sends, scaled by (1 - mix) x gain */
    float cue_master[FA_MIXER_BLOCK];                 /* mix x gain: the master's share of the cue */
};

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---- the isolator ------------------------------------------------------------- */

/* RBJ cookbook sections, Q = 1/sqrt(2): two Butterworth sections are one LR4 filter,
 * and LR4 low + high is exactly the allpass at the same frequency (all bilinear with
 * the same prewarp), so low (allpassed at 2.5 kHz) + mid + high is flat. */
static biquad design(int type, double hz, double rate)
{
    double w = 2.0 * k_pi * hz / rate, c = cos(w), alpha = sin(w) / (2.0 * 0.70710678118654752);
    double a0 = 1.0 + alpha, b0, b1, b2;
    biquad q;
    if (type == 0) { b0 = (1.0 - c) / 2.0; b1 = 1.0 - c; b2 = b0; }             /* low-pass */
    else if (type == 1) { b0 = (1.0 + c) / 2.0; b1 = -(1.0 + c); b2 = b0; }     /* high-pass */
    else { b0 = 1.0 - alpha; b1 = -2.0 * c; b2 = 1.0 + alpha; }                 /* allpass */
    q.b0 = (float)(b0 / a0);
    q.b1 = (float)(b1 / a0);
    q.b2 = (float)(b2 / a0);
    q.a1 = (float)(-2.0 * c / a0);
    q.a2 = (float)((1.0 - alpha) / a0);
    return q;
}

static inline float run(const biquad* q, float* z, float x)
{
    float y = q->b0 * x + z[0];
    z[0] = q->b1 * x - q->a1 * y + z[1];
    z[1] = q->b2 * x - q->a2 * y;
    return y;
}

/* One sample of one channel: the three bands, weighted. */
static inline float isolate(const struct fa_mixer* m, float (*z)[2][2], int c, float x, float gl, float gm, float gh)
{
    float lo = run(&m->lp1, z[1][c], run(&m->lp1, z[0][c], x));
    float up = run(&m->hp1, z[3][c], run(&m->hp1, z[2][c], x));
    float mid = run(&m->lp2, z[5][c], run(&m->lp2, z[4][c], up));
    float hi = run(&m->hp2, z[7][c], run(&m->hp2, z[6][c], up));
    lo = run(&m->ap2, z[8][c], lo);
    return gl * lo + gm * mid + gh * hi;
}

/* dB to linear for trim and EQ: the kill setting is silence. */
static float band_gain(float db)
{
    return db <= FA_DECK_EQ_KILL_DB + 0.001f ? 0.0f : powf(10.0f, db / 20.0f);
}

/* ---- the crossfader ----------------------------------------------------------- */

static void xfader_gains(int curve, float x, float* ga, float* gb)
{
    if (curve == FA_XFADER_LINEAR) {
        *ga = 1.0f - x;
        *gb = x;
    } else {
        *ga = cosf(x * (float)(k_pi / 2.0));
        *gb = sinf(x * (float)(k_pi / 2.0));
    }
}

/* ---- audio thread ------------------------------------------------------------- */

static void take_lanes(fa_lane* lanes, int n, uint32_t rate)
{
    int i;
    for (i = 0; i < n; i++) fa_lane_take(&lanes[i], 1, rate, NULL);
}

static void end_lanes(fa_lane* lanes, int n, int64_t clock, int64_t frames)
{
    int i;
    for (i = 0; i < n; i++) {
        fa_lane_advance(&lanes[i].v, clock, frames);
        fa_store_float(&lanes[i].current, lanes[i].v.value);
    }
}

/* Values of `lanes` at the start and the end of [at, at + n). */
static void lane_span(const fa_lane* lanes, int n, int64_t clock, int64_t at, int64_t len, float* v0, float* v1)
{
    int i;
    for (i = 0; i < n; i++) {
        v0[i] = fa_lane_value_at_frame(&lanes[i].v, clock, at, 1);
        v1[i] = fa_lane_value_at_frame(&lanes[i].v, clock, at + len, 0);
    }
}

static float side_glide(float w, float target, int64_t n, uint32_t rate)
{
    float step = (float)n / ((float)rate * 0.005f);
    if (w < target) return w + step < target ? w + step : target;
    return w - step > target ? w - step : target;
}

/* Mixer lane values at every MIXER_SUB edge of the block (index k = frame k x MIXER_SUB,
 * the last one at the block's end): sub-block k runs from edge[k] to edge[k + 1]. */
#define MIXER_EDGES (FA_MIXER_BLOCK / MIXER_SUB + 1)
typedef struct mix_edges {
    float v[MIX_LANES][MIXER_EDGES];     /* the value from that edge on */
    float e[MIX_LANES][MIXER_EDGES];     /* the value up to that edge (differs where a jump starts) */
} mix_edges;

/* One deck channel over the block: trim, isolator, cue send, fader, meter, crossfader. */
static void deck_process(struct fa_mixer* m, struct fa_deck* d, const mix_edges* me, int64_t clock, uint32_t rate,
                         float* out, uint32_t frames)
{
    float* buf = d->bus->buf;
    int curve = fa_load32(&m->curve), side = fa_load32(&d->side), cue_on = m->cue_mode != FA_CUE_NONE;
    uint32_t off, k;
    fa_tap* tap;
    for (off = 0, k = 0; off < frames; off += MIXER_SUB, k++) {
        uint32_t n = frames - off < MIXER_SUB ? frames - off : MIXER_SUB, i, j;
        float v0[DECK_LANES], v1[DECK_LANES], w0[3], w1[3];
        float t0, t1, l0, l1, mi0, mi1, h0, h1, g0, g1, c0, c1, x0, x1, ga0, gb0, ga1, gb1, inv = 1.0f / (float)n;
        lane_span(d->lanes, DECK_LANES, clock, clock + off, n, v0, v1);
        for (j = 0; j < 3; j++) {
            w0[j] = d->side_w[j];
            d->side_w[j] = side_glide(d->side_w[j], (int)j == side ? 1.0f : 0.0f, n, rate);
            w1[j] = d->side_w[j];
        }
        t0 = powf(10.0f, v0[DL_TRIM] / 20.0f); t1 = powf(10.0f, v1[DL_TRIM] / 20.0f);
        l0 = band_gain(v0[DL_LOW]); l1 = band_gain(v1[DL_LOW]);
        mi0 = band_gain(v0[DL_MID]); mi1 = band_gain(v1[DL_MID]);
        h0 = band_gain(v0[DL_HIGH]); h1 = band_gain(v1[DL_HIGH]);
        g0 = v0[DL_GAIN]; g1 = v1[DL_GAIN];
        c0 = v0[DL_CUE]; c1 = v1[DL_CUE];
        /* This deck's crossfader gain (its side mix) at both ends of the sub-block. */
        xfader_gains(curve, me->v[ML_XF][k], &ga0, &gb0);
        xfader_gains(curve, me->e[ML_XF][k + 1], &ga1, &gb1);
        x0 = w0[0] + w0[1] * ga0 + w0[2] * gb0;
        x1 = w1[0] + w1[1] * ga1 + w1[2] * gb1;
        for (i = 0; i < n; i++) {
            float f = (float)i * inv;
            float tr = t0 + (t1 - t0) * f, gl = l0 + (l1 - l0) * f, gm = mi0 + (mi1 - mi0) * f, gh = h0 + (h1 - h0) * f;
            float fd = g0 + (g1 - g0) * f, cs = c0 + (c1 - c0) * f, xf = x0 + (x1 - x0) * f;
            float* p = buf + (size_t)(off + i) * 2;
            float* o = out + (size_t)(off + i) * 2;
            int c;
            for (c = 0; c < 2; c++) {
                float y = isolate(m, d->z, c, p[c] * tr, gl, gm, gh);
                if (cue_on) m->cue[(size_t)(off + i) * 2 + c] += y * cs;
                y *= fd;
                p[c] = y;          /* for the meter */
                o[c] += y * xf;
            }
        }
    }
    /* Flush denormals so a deck left silent stays cheap. */
    {
        float* z = &d->z[0][0][0];
        int i;
        for (i = 0; i < ISO_SECTIONS * 4; i++) if (fabsf(z[i]) < 1e-20f) z[i] = 0.0f;
    }
    tap = (fa_tap*)fa_load_ptr(&d->tap);
    if (tap) fa_tap_write(tap, clock, buf, frames);
    memset(buf, 0, (size_t)frames * FA_DEVICE_CHANNELS * sizeof(float));   /* ready for the next block */
}

void fa_mixer_process(struct fa_mixer* m, fa_device* dev, float* out, uint32_t frames)
{
    int64_t clock = fa_load64(&dev->clock);
    uint32_t i, off, k;
    int p;
    mix_edges me;
    take_lanes(m->lanes, MIX_LANES, dev->rate);
    for (i = 0; i < m->deck_count; i++) take_lanes(m->decks[i].lanes, DECK_LANES, dev->rate);
    for (off = 0, k = 0;; off += MIXER_SUB, k++) {
        uint32_t at = off < frames ? off : frames;
        for (p = 0; p < MIX_LANES; p++) {
            me.v[p][k] = fa_lane_value_at_frame(&m->lanes[p].v, clock, clock + at, 1);
            me.e[p][k] = fa_lane_value_at_frame(&m->lanes[p].v, clock, clock + at, 0);
        }
        if (off >= frames) break;
    }
    if (m->cue_mode != FA_CUE_NONE) memset(m->cue, 0, (size_t)frames * FA_DEVICE_CHANNELS * sizeof(float));
    for (i = 0; i < m->deck_count; i++) deck_process(m, &m->decks[i], &me, clock, dev->rate, out, frames);
    /* Master gain on the sum; the cue's blend factors for fa_mixer_render. */
    for (off = 0, k = 0; off < frames; off += MIXER_SUB, k++) {
        uint32_t n = frames - off < MIXER_SUB ? frames - off : MIXER_SUB, j;
        float g0 = me.v[ML_MASTER][k], g1 = me.e[ML_MASTER][k + 1], inv = 1.0f / (float)n;
        float s0 = (1.0f - me.v[ML_CUE_MIX][k]) * me.v[ML_CUE_GAIN][k];
        float s1 = (1.0f - me.e[ML_CUE_MIX][k + 1]) * me.e[ML_CUE_GAIN][k + 1];
        float q0 = me.v[ML_CUE_MIX][k] * me.v[ML_CUE_GAIN][k], q1 = me.e[ML_CUE_MIX][k + 1] * me.e[ML_CUE_GAIN][k + 1];
        for (j = 0; j < n; j++) {
            float f = (float)j * inv, g = g0 + (g1 - g0) * f;
            float* o = out + (size_t)(off + j) * 2;
            o[0] *= g;
            o[1] *= g;
            if (m->cue_mode != FA_CUE_NONE) {
                float* c = m->cue + (size_t)(off + j) * 2;
                float sc = s0 + (s1 - s0) * f;
                c[0] *= sc;
                c[1] *= sc;
                m->cue_master[off + j] = q0 + (q1 - q0) * f;
            }
        }
    }
    end_lanes(m->lanes, MIX_LANES, clock, frames);
    for (i = 0; i < m->deck_count; i++) end_lanes(m->decks[i].lanes, DECK_LANES, clock, frames);
}

static float clip1(float v) { return v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v; }

/* The master (after the limiter and the device mute) and the cue onto the device's channels.
 * The device mute silences the cue sends too, gliding as the master did over the block
 * (from mute0 to mute1). */
static void lay_out(struct fa_mixer* m, const float* master, float* out, uint32_t channels, uint32_t frames,
                    float mute0, float mute1, uint32_t rate)
{
    uint32_t i, c, cue_at = m->cue_mode == FA_CUE_CHANNELS && channels >= m->cue_channel + 2 ? m->cue_channel : 0;
    float mute = mute0;
    if (channels == 2 && m->cue_mode != FA_CUE_SPLIT) {
        memcpy(out, master, (size_t)frames * 2 * sizeof(float));
        return;
    }
    for (i = 0; i < frames; i++) {
        const float* mm = master + (size_t)i * 2;
        float* o = out + (size_t)i * channels;
        float cl = 0.0f, cr = 0.0f;
        if (m->cue_mode != FA_CUE_NONE) {
            const float* cs = m->cue + (size_t)i * 2;
            float audible;
            if (mute != mute1) mute = fa_mute_glide(mute, mute1, 1, rate);
            audible = 1.0f - mute;
            cl = clip1(cs[0] * audible + m->cue_master[i] * mm[0]);
            cr = clip1(cs[1] * audible + m->cue_master[i] * mm[1]);
        }
        for (c = 0; c < channels; c++) o[c] = 0.0f;
        if (channels == 1) {
            o[0] = 0.5f * (mm[0] + mm[1]);
        } else if (m->cue_mode == FA_CUE_SPLIT) {
            o[0] = 0.5f * (mm[0] + mm[1]);
            o[1] = 0.5f * (cl + cr);
        } else {
            o[0] = mm[0];
            o[1] = mm[1];
            if (cue_at) { o[cue_at] = cl; o[cue_at + 1] = cr; }
        }
    }
}

void fa_mixer_render(struct fa_mixer* m, fa_device* dev, float* out, uint32_t channels, uint32_t frames, int64_t now_ns)
{
    uint32_t done, n;
    for (done = 0; done < frames; done += n) {
        float mute0 = dev->mute_level;   /* audio thread only, as render_block uses it */
        n = frames - done < FA_MIXER_BLOCK ? frames - done : FA_MIXER_BLOCK;
        fa_device_render_at(dev, m->master, n, now_ns + (int64_t)((double)done * 1e9 / dev->rate));
        lay_out(m, m->master, out + (size_t)done * channels, channels, n, mute0, dev->mute_level, dev->rate);
    }
}

/* ---- mixer API ---------------------------------------------------------------- */

FA_API void FA_CALL fa_mixer_config_init(fa_mixer_config* config)
{
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->struct_size = (uint32_t)sizeof(*config);
    config->deck_count = 2;
    config->cue_mode = FA_CUE_NONE;
    config->latency = FA_LATENCY_LOW;
}

static void free_mixer(struct fa_mixer* m)
{
    uint32_t i;
    for (i = 0; i < m->deck_count; i++) fa_tap_free((fa_tap*)fa_load_ptr(&m->decks[i].tap));
    fa_mutex_destroy(&m->lock);
    free(m);
}

FA_API fa_result FA_CALL fa_mixer_create(fa_engine* engine, const fa_mixer_config* config, fa_mixer** out_mixer)
{
    fa_mixer_config c;
    struct fa_mixer* m;
    fa_device* dev = NULL;
    fa_result r;
    uint32_t i, channels;
    if (!engine || !out_mixer) return FA_ERROR_INVALID_ARG;
    *out_mixer = NULL;
    fa_mixer_config_init(&c);
    if (config) {
        if (config->struct_size < sizeof(fa_mixer_config)) return FA_ERROR_INVALID_ARG;
        c = *config;
    }
    if (c.deck_count == 0) c.deck_count = 2;
    if (c.deck_count > 2) return FA_ERROR_NOT_IMPLEMENTED;   /* 4 decks are planned */
    if (c.deck_count < 2) return FA_ERROR_INVALID_ARG;
    if (c.cue_mode == FA_CUE_DEVICE || c.latency == FA_LATENCY_EXCLUSIVE) return FA_ERROR_NOT_IMPLEMENTED;
    if ((uint32_t)c.cue_mode > FA_CUE_SPLIT || (uint32_t)c.latency > FA_LATENCY_EXCLUSIVE || c.flags != 0)
        return FA_ERROR_INVALID_ARG;
    if (c.cue_channel == 0) c.cue_channel = 2;
    if (c.cue_channel < 2 || c.cue_channel > 30) return FA_ERROR_INVALID_ARG;
    /* Channels 3-4: the device opens with all its channels (the null output, which has
     * as many as asked, with just enough). */
    channels = FA_DEVICE_CHANNELS;
    if (c.cue_mode == FA_CUE_CHANNELS) channels = (engine->config.flags & FA_ENGINE_NULL_OUTPUT) ? c.cue_channel + 2 : 0;
    r = fa_device_open_with(engine, c.device_id, channels, c.latency == FA_LATENCY_LOW, &dev);
    if (r != FA_OK) return r;
    if (c.cue_mode == FA_CUE_CHANNELS && dev->device.playback.channels < c.cue_channel + 2) {
        fa_device_close(dev);
        return FA_ERROR_NOT_ENOUGH_CHANNELS;
    }
    m = (struct fa_mixer*)calloc(1, sizeof(*m));
    if (!m || fa_mutex_init(&m->lock) != 0) {
        free(m);
        fa_device_close(dev);
        return m ? FA_ERROR_GENERIC : FA_ERROR_OUT_OF_MEMORY;
    }
    m->engine = engine;
    m->device = dev;
    m->deck_count = c.deck_count;
    m->cue_mode = c.cue_mode;
    m->cue_channel = c.cue_channel;
    fa_store32(&m->curve, FA_XFADER_CONSTANT_POWER);
    fa_lane_init(&m->lanes[ML_XF], 0.5f);
    fa_lane_init(&m->lanes[ML_MASTER], 1.0f);
    fa_lane_init(&m->lanes[ML_CUE_MIX], 0.0f);
    fa_lane_init(&m->lanes[ML_CUE_GAIN], 1.0f);
    m->lp1 = design(0, ISO_LOW_HZ, dev->rate);
    m->hp1 = design(1, ISO_LOW_HZ, dev->rate);
    m->lp2 = design(0, ISO_HIGH_HZ, dev->rate);
    m->hp2 = design(1, ISO_HIGH_HZ, dev->rate);
    m->ap2 = design(2, ISO_HIGH_HZ, dev->rate);
    for (i = 0; i < m->deck_count; i++) {
        struct fa_deck* d = &m->decks[i];
        int side = i % 2 ? 2 : 1;
        d->mixer = m;
        d->index = i;
        d->cue_point = FA_FRAMES_UNSET;
        fa_lane_init(&d->lanes[DL_GAIN], 1.0f);
        fa_lane_init(&d->lanes[DL_TRIM], 0.0f);
        fa_lane_init(&d->lanes[DL_LOW], 0.0f);
        fa_lane_init(&d->lanes[DL_MID], 0.0f);
        fa_lane_init(&d->lanes[DL_HIGH], 0.0f);
        fa_lane_init(&d->lanes[DL_CUE], 0.0f);
        fa_store32(&d->side, side);
        d->side_w[side] = 1.0f;
        r = fa_bus_create(dev, &d->bus);
        if (r == FA_OK) {
            d->bus->deck = d;
            d->bus->buf = (float*)calloc((size_t)FA_MIXER_BLOCK * FA_DEVICE_CHANNELS, sizeof(float));
            if (!d->bus->buf) r = FA_ERROR_OUT_OF_MEMORY;
        }
        if (r != FA_OK) {
            fa_device_close(dev);   /* frees the buses */
            free_mixer(m);
            return r;
        }
    }
    fa_exchange_ptr(&dev->mixer, m);
    r = fa_device_start(dev);
    if (r != FA_OK) {
        fa_mixer_destroy(m);
        return r;
    }
    *out_mixer = m;
    return FA_OK;
}

FA_API void FA_CALL fa_mixer_destroy(fa_mixer* mixer)
{
    fa_device* dev;
    uint32_t i;
    if (!mixer) return;
    fa_mutex_lock(&mixer->lock);
    for (i = 0; i < mixer->deck_count; i++) {
        fa_stream* s = (fa_stream*)fa_exchange_ptr(&mixer->decks[i].stream, NULL);
        if (!s) continue;
        fa_stream_set_loop_region(s, 0, 0);
        fa_stream_detach(s);
    }
    fa_mutex_unlock(&mixer->lock);
    dev = mixer->device;
    /* Stop it first: a callback without the mixer would lay stereo out on all the
     * device's channels (a 4-channel cue device), a burst of noise until the close. */
    fa_device_stop(dev);
    fa_exchange_ptr(&dev->mixer, NULL);
    fa_device_wait_quiescent(dev);   /* no callback is in the mixer any more */
    fa_device_close(dev);
    free_mixer(mixer);
}

void fa_mixer_destroy_from_device(struct fa_mixer* m) { fa_mixer_destroy(m); }

FA_API fa_result FA_CALL fa_mixer_get_device(fa_mixer* mixer, fa_device** out_device)
{
    if (!mixer || !out_device) return FA_ERROR_INVALID_ARG;
    *out_device = mixer->device;
    return FA_OK;
}

FA_API fa_result FA_CALL fa_mixer_get_deck(fa_mixer* mixer, uint32_t index, fa_deck** out_deck)
{
    if (!mixer || !out_deck) return FA_ERROR_INVALID_ARG;
    *out_deck = NULL;
    if (index >= mixer->deck_count) return FA_ERROR_INVALID_ARG;
    *out_deck = &mixer->decks[index];
    return FA_OK;
}

FA_API fa_result FA_CALL fa_mixer_get_output_channels(fa_mixer* mixer, uint32_t* out_channels, int32_t* out_cue_channel)
{
    uint32_t ch;
    fa_device* dev;
    if (!mixer) return FA_ERROR_INVALID_ARG;
    dev = mixer->device;
    fa_mutex_lock(&dev->life_lock);   /* the device may be reopened on the default */
    ch = dev->device_ok ? dev->device.playback.channels : 0;
    fa_mutex_unlock(&dev->life_lock);
    if (out_channels) *out_channels = ch;
    if (out_cue_channel) {
        if (mixer->cue_mode == FA_CUE_CHANNELS && ch >= mixer->cue_channel + 2) *out_cue_channel = (int32_t)mixer->cue_channel;
        else if (mixer->cue_mode == FA_CUE_SPLIT && ch >= 2) *out_cue_channel = 1;
        else *out_cue_channel = -1;
    }
    return FA_OK;
}

static fa_result mix_range(fa_mix_param p, float* v)
{
    if (*v != *v) return FA_ERROR_INVALID_ARG;
    switch (p) {
    case FA_MIX_CROSSFADER: case FA_MIX_CUE_MIX: *v = clampf(*v, 0.0f, 1.0f); return FA_OK;
    case FA_MIX_MASTER_GAIN: case FA_MIX_CUE_GAIN: *v = clampf(*v, 0.0f, 4.0f); return FA_OK;
    default: return FA_ERROR_INVALID_ARG;
    }
}

/* Device frame of `device_time` (0 = at once) for a lane ramp. */
static int64_t ramp_at(fa_device* dev, fa_time device_time)
{
    return device_time ? (int64_t)((double)device_time * (double)dev->rate / 1e9 + 0.5) : -1;
}

FA_API fa_result FA_CALL fa_mixer_set_param(fa_mixer* mixer, fa_mix_param param, float value)
{
    fa_result r;
    if (!mixer) return FA_ERROR_INVALID_ARG;
    if ((r = mix_range(param, &value)) != FA_OK) return r;
    fa_mutex_lock(&mixer->lock);
    fa_lane_post(&mixer->lanes[param], value, MIXER_DEZIPPER_NS, NULL, 0, 0, -1, 0, NULL);
    fa_mutex_unlock(&mixer->lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_mixer_get_param(fa_mixer* mixer, fa_mix_param param, float* out_value)
{
    float v = 0.0f;
    if (!mixer || !out_value || mix_range(param, &v) != FA_OK) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&mixer->lock);
    *out_value = fa_lane_get(&mixer->lanes[param]);
    fa_mutex_unlock(&mixer->lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_mixer_ramp_param(fa_mixer* mixer, fa_mix_param param, float target, fa_time duration,
                                             fa_time device_time, const fa_ramp* ramp)
{
    fa_shape shape;
    fa_result r;
    if (!mixer || duration < 0 || device_time < 0) return FA_ERROR_INVALID_ARG;
    if ((r = fa_shape_from_ramp(ramp, FA_CURVE_LINEAR, &shape)) != FA_OK) return r;
    if ((r = mix_range(param, &target)) != FA_OK) return r;
    fa_mutex_lock(&mixer->lock);
    fa_lane_post(&mixer->lanes[param], target, duration, &shape, 0, 1, ramp_at(mixer->device, device_time), 0, NULL);
    fa_mutex_unlock(&mixer->lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_mixer_set_crossfader_curve(fa_mixer* mixer, fa_xfader_curve curve)
{
    if (!mixer || (curve != FA_XFADER_CONSTANT_POWER && curve != FA_XFADER_LINEAR)) return FA_ERROR_INVALID_ARG;
    fa_store32(&mixer->curve, (int32_t)curve);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_mixer_get_crossfader_curve(fa_mixer* mixer, fa_xfader_curve* out_curve)
{
    if (!mixer || !out_curve) return FA_ERROR_INVALID_ARG;
    *out_curve = (fa_xfader_curve)fa_load32(&mixer->curve);
    return FA_OK;
}

/* ---- deck API ----------------------------------------------------------------- */

static fa_stream* deck_stream(struct fa_deck* d) { return (fa_stream*)fa_load_ptr(&d->stream); }

void fa_mixer_bus_left(fa_bus* bus, struct fa_stream* s) { fa_cas_ptr(&bus->deck->stream, s, NULL); }

/* Whether the deck is heard now: playing, fader up, and the crossfader open on its
 * side. Caller holds the mixer lock. */
static int deck_heard(struct fa_deck* d)
{
    struct fa_mixer* m = d->mixer;
    fa_stream_state st = FA_STATE_STOPPED;
    float ga, gb, g;
    int side = fa_load32(&d->side);
    fa_stream* s = deck_stream(d);
    if (!s || fa_stream_get_state(s, &st) != FA_OK || st != FA_STATE_PLAYING) return 0;
    xfader_gains(fa_load32(&m->curve), fa_lane_get(&m->lanes[ML_XF]), &ga, &gb);
    g = fa_lane_get(&d->lanes[DL_GAIN]) * (side == 1 ? ga : side == 2 ? gb : 1.0f);
    return g > 0.001f;   /* -60 dB */
}

FA_API fa_result FA_CALL fa_deck_load(fa_deck* deck, fa_stream* stream, uint32_t flags)
{
    struct fa_mixer* m;
    fa_stream* old;
    fa_result r = FA_OK;
    if (!deck || (flags & ~FA_DECK_LOAD_FORCE)) return FA_ERROR_INVALID_ARG;
    m = deck->mixer;
    if (stream && stream->engine != m->engine) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&m->lock);
    old = deck_stream(deck);
    if (stream == old) { fa_mutex_unlock(&m->lock); return FA_OK; }
    if (old && !(flags & FA_DECK_LOAD_FORCE) && deck_heard(deck)) { fa_mutex_unlock(&m->lock); return FA_ERROR_BUSY; }
    if (stream) {
        fa_stream_state st = FA_STATE_STOPPED;
        fa_tempo_options o;
        uint32_t old_latency = 0;
        int shortened = 0;
        fa_stream_get_state(stream, &st);
        if (st != FA_STATE_PLAYING) {
            fa_tempo_options_init(&o);
            if (fa_stream_get_tempo_options(stream, &o) == FA_OK && (o.latency_ms == 0 || o.latency_ms > DECK_TEMPO_LATENCY_MS)) {
                old_latency = o.latency_ms;
                o.latency_ms = DECK_TEMPO_LATENCY_MS;
                shortened = fa_stream_set_tempo_options(stream, &o) == FA_OK;
            }
        }
        /* From another deck too: that deck is empty then (fa_mixer_bus_left). */
        r = fa_stream_attach(stream, m->device, deck->bus);
        if (r != FA_OK) {
            if (shortened) {   /* not loaded: the stream keeps its own decode-ahead */
                o.latency_ms = old_latency;
                fa_stream_set_tempo_options(stream, &o);
            }
            fa_mutex_unlock(&m->lock);
            return r;
        }
    }
    fa_exchange_ptr(&deck->stream, stream);
    deck->cue_point = FA_FRAMES_UNSET;
    if (old) {
        fa_stream_set_loop_region(old, 0, 0);
        fa_stream_detach(old);
    }
    fa_mutex_unlock(&m->lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_deck_get_stream(fa_deck* deck, fa_stream** out_stream)
{
    if (!deck || !out_stream) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&deck->mixer->lock);
    *out_stream = deck_stream(deck);
    fa_mutex_unlock(&deck->mixer->lock);
    return FA_OK;
}

static fa_result deck_range(fa_deck_param p, float* v)
{
    if (*v != *v) return FA_ERROR_INVALID_ARG;
    switch (p) {
    case FA_DECK_GAIN: *v = clampf(*v, 0.0f, 1.0f); return FA_OK;
    case FA_DECK_TRIM_DB: *v = clampf(*v, -12.0f, 12.0f); return FA_OK;
    case FA_DECK_EQ_LOW_DB: case FA_DECK_EQ_MID_DB: case FA_DECK_EQ_HIGH_DB:
        *v = clampf(*v, FA_DECK_EQ_KILL_DB, 6.0f);
        return FA_OK;
    case FA_DECK_XFADER_SIDE:
        if (*v != 0.0f && *v != 1.0f && *v != 2.0f) return FA_ERROR_INVALID_ARG;
        return FA_OK;
    case FA_DECK_CUE: *v = *v != 0.0f ? 1.0f : 0.0f; return FA_OK;
    default: return FA_ERROR_INVALID_ARG;
    }
}

/* The lane behind a rampable deck parameter. */
static fa_lane* deck_lane(struct fa_deck* d, fa_deck_param p)
{
    switch (p) {
    case FA_DECK_GAIN: return &d->lanes[DL_GAIN];
    case FA_DECK_TRIM_DB: return &d->lanes[DL_TRIM];
    case FA_DECK_EQ_LOW_DB: return &d->lanes[DL_LOW];
    case FA_DECK_EQ_MID_DB: return &d->lanes[DL_MID];
    case FA_DECK_EQ_HIGH_DB: return &d->lanes[DL_HIGH];
    case FA_DECK_CUE: return &d->lanes[DL_CUE];
    default: return NULL;
    }
}

FA_API fa_result FA_CALL fa_deck_set_param(fa_deck* deck, fa_deck_param param, float value)
{
    fa_result r;
    if (!deck) return FA_ERROR_INVALID_ARG;
    if ((r = deck_range(param, &value)) != FA_OK) return r;
    if (param == FA_DECK_XFADER_SIDE) {   /* glides between the sides over 5 ms */
        fa_store32(&deck->side, (int32_t)value);
        return FA_OK;
    }
    fa_mutex_lock(&deck->mixer->lock);
    fa_lane_post(deck_lane(deck, param), value, MIXER_DEZIPPER_NS, NULL, 0, 0, -1, 0, NULL);
    fa_mutex_unlock(&deck->mixer->lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_deck_get_param(fa_deck* deck, fa_deck_param param, float* out_value)
{
    float v = 0.0f;
    if (!deck || !out_value || deck_range(param, &v) != FA_OK) return FA_ERROR_INVALID_ARG;
    if (param == FA_DECK_XFADER_SIDE) { *out_value = (float)fa_load32(&deck->side); return FA_OK; }
    fa_mutex_lock(&deck->mixer->lock);
    *out_value = fa_lane_get(deck_lane(deck, param));
    fa_mutex_unlock(&deck->mixer->lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_deck_ramp_param(fa_deck* deck, fa_deck_param param, float target, fa_time duration,
                                            fa_time device_time, const fa_ramp* ramp)
{
    fa_shape shape;
    fa_result r;
    if (!deck || duration < 0 || device_time < 0 || param == FA_DECK_XFADER_SIDE || param == FA_DECK_CUE)
        return FA_ERROR_INVALID_ARG;
    if ((r = fa_shape_from_ramp(ramp, FA_CURVE_LINEAR, &shape)) != FA_OK) return r;
    if ((r = deck_range(param, &target)) != FA_OK) return r;
    fa_mutex_lock(&deck->mixer->lock);
    fa_lane_post(deck_lane(deck, param), target, duration, &shape, 0, 1, ramp_at(deck->mixer->device, device_time), 0, NULL);
    fa_mutex_unlock(&deck->mixer->lock);
    return FA_OK;
}

/* The cue point in force. Caller holds the mixer lock and the deck has a stream. */
static fa_frames cue_point_of(struct fa_deck* d)
{
    fa_frames f = d->cue_point;
    if (f == FA_FRAMES_UNSET && (fa_stream_get_marker(deck_stream(d), FA_MARKER_CUE_IN, &f) != FA_OK || f == FA_FRAMES_UNSET)) f = 0;
    return f;
}

FA_API fa_result FA_CALL fa_deck_set_cue_point(fa_deck* deck, fa_frames source_frame)
{
    if (!deck || (source_frame < 0 && source_frame != FA_FRAMES_UNSET)) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&deck->mixer->lock);
    deck->cue_point = source_frame;
    fa_mutex_unlock(&deck->mixer->lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_deck_get_cue_point(fa_deck* deck, fa_frames* out_source_frame)
{
    if (!deck || !out_source_frame) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&deck->mixer->lock);
    *out_source_frame = deck_stream(deck) ? cue_point_of(deck) : (deck->cue_point == FA_FRAMES_UNSET ? 0 : deck->cue_point);
    fa_mutex_unlock(&deck->mixer->lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_deck_cue(fa_deck* deck)
{
    fa_result r;
    if (!deck) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&deck->mixer->lock);
    if (!deck_stream(deck)) { fa_mutex_unlock(&deck->mixer->lock); return FA_ERROR_NOT_FOUND; }
    r = fa_stream_pause(deck_stream(deck));
    if (r == FA_OK) r = fa_stream_seek(deck_stream(deck), cue_point_of(deck));
    fa_mutex_unlock(&deck->mixer->lock);
    return r;
}

FA_API fa_result FA_CALL fa_deck_set_loop(fa_deck* deck, fa_frames loop_in, fa_frames loop_out)
{
    fa_result r;
    if (!deck) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&deck->mixer->lock);
    r = deck_stream(deck) ? fa_stream_set_loop_region(deck_stream(deck), loop_in, loop_out) : FA_ERROR_NOT_FOUND;
    fa_mutex_unlock(&deck->mixer->lock);
    return r;
}

FA_API fa_result FA_CALL fa_deck_get_loop(fa_deck* deck, fa_frames* out_loop_in, fa_frames* out_loop_out)
{
    fa_frames in = 0, out = 0;
    if (!deck || !out_loop_in || !out_loop_out) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&deck->mixer->lock);
    if (deck_stream(deck)) fa_stream_get_loop_region(deck_stream(deck), &in, &out);
    fa_mutex_unlock(&deck->mixer->lock);
    *out_loop_in = in;
    *out_loop_out = out;
    return FA_OK;
}

FA_API fa_result FA_CALL fa_deck_get_levels(fa_deck* deck, fa_time window, float* out_peak, float* out_rms, uint32_t channels)
{
    fa_result r;
    if (!deck) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&deck->mixer->lock);   /* serializes fa_tap_fit with other readers */
    r = fa_tap_levels(&deck->tap, deck->mixer->device, window, out_peak, out_rms, channels);
    fa_mutex_unlock(&deck->mixer->lock);
    return r;
}
