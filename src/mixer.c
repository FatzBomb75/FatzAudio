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
#include "mixer.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#  include <emmintrin.h>
#  define FA_ISO_SSE 1
#endif


/* ---- the isolator ------------------------------------------------------------- */

/* RBJ cookbook sections, Q = 1/sqrt(2): two Butterworth sections are one LR4 filter,
 * and LR4 low + high is exactly the allpass at the same frequency (all bilinear with
 * the same prewarp), so low (allpassed at 2.5 kHz) + mid + high is flat. */
biquad fa_mixer_design(int type, double hz, double rate)
{
    double w = 2.0 * FA_PI * hz / rate, c = cos(w), alpha = sin(w) / (2.0 * 0.70710678118654752);
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


#ifndef FA_ISO_SSE
/* One sample of one channel: the three bands, weighted. */
static inline float isolate(const struct fa_mixer* m, float (*z)[2][2], int c, float x, float gl, float gm, float gh)
{
    float lo = fa_biquad_run(&m->lp1, z[1][c], fa_biquad_run(&m->lp1, z[0][c], x));
    float up = fa_biquad_run(&m->hp1, z[3][c], fa_biquad_run(&m->hp1, z[2][c], x));
    float mid = fa_biquad_run(&m->lp2, z[5][c], fa_biquad_run(&m->lp2, z[4][c], up));
    float hi = fa_biquad_run(&m->hp2, z[7][c], fa_biquad_run(&m->hp2, z[6][c], up));
    lo = fa_biquad_run(&m->ap2, z[8][c], lo);
    return gl * lo + gm * mid + gh * hi;
}
#endif

#ifdef FA_ISO_SSE
/* The isolator four sections at a time: both channels of two sections per vector,
 * so its 9 sections x 2 channels take 5 vector steps a frame instead of 18. Each lane
 * does the scalar section's arithmetic in the same order, so the output is the
 * same to the bit. Stages (lanes L, R, L, R):
 *   0: lp1 (section 0), hp1 (2) on the input     1: lp1 (1), hp1 (3) -> lo, up
 *   2: lp2 (4), hp2 (6) on up                    3: lp2 (5), hp2 (7) -> mid, hi
 *   4: ap2 (8) on lo (lanes 2 and 3 unused, kept at 0) */
typedef struct iso_sse {
    __m128 b0[5], b1[5], b2[5], a1[5], a2[5], z0[5], z1[5];
} iso_sse;

static const int8_t iso_lane_sec[5][4] = { { 0, 0, 2, 2 }, { 1, 1, 3, 3 }, { 4, 4, 6, 6 }, { 5, 5, 7, 7 }, { 8, 8, -1, -1 } };

static void iso_sse_load(iso_sse* v, const struct fa_mixer* m, float (*z)[2][2])
{
    const biquad* q[9] = { &m->lp1, &m->lp1, &m->hp1, &m->hp1, &m->lp2, &m->lp2, &m->hp2, &m->hp2, &m->ap2 };
    int s, l;
    for (s = 0; s < 5; s++) {
        float b0[4], b1[4], b2[4], a1[4], a2[4], z0[4], z1[4];
        for (l = 0; l < 4; l++) {
            int sec = iso_lane_sec[s][l];
            if (sec < 0) { b0[l] = b1[l] = b2[l] = a1[l] = a2[l] = z0[l] = z1[l] = 0.0f; continue; }
            b0[l] = q[sec]->b0; b1[l] = q[sec]->b1; b2[l] = q[sec]->b2; a1[l] = q[sec]->a1; a2[l] = q[sec]->a2;
            z0[l] = z[sec][l & 1][0];
            z1[l] = z[sec][l & 1][1];
        }
        v->b0[s] = _mm_loadu_ps(b0); v->b1[s] = _mm_loadu_ps(b1); v->b2[s] = _mm_loadu_ps(b2);
        v->a1[s] = _mm_loadu_ps(a1); v->a2[s] = _mm_loadu_ps(a2);
        v->z0[s] = _mm_loadu_ps(z0); v->z1[s] = _mm_loadu_ps(z1);
    }
}

static void iso_sse_store(const iso_sse* v, float (*z)[2][2])
{
    int s, l;
    for (s = 0; s < 5; s++) {
        float z0[4], z1[4];
        _mm_storeu_ps(z0, v->z0[s]);
        _mm_storeu_ps(z1, v->z1[s]);
        for (l = 0; l < 4; l++) {
            int sec = iso_lane_sec[s][l];
            if (sec < 0) continue;
            z[sec][l & 1][0] = z0[l];
            z[sec][l & 1][1] = z1[l];
        }
    }
}

static inline __m128 iso_sse_step(iso_sse* v, int s, __m128 x)
{
    __m128 y = _mm_add_ps(_mm_mul_ps(v->b0[s], x), v->z0[s]);
    v->z0[s] = _mm_add_ps(_mm_sub_ps(_mm_mul_ps(v->b1[s], x), _mm_mul_ps(v->a1[s], y)), v->z1[s]);
    v->z1[s] = _mm_sub_ps(_mm_mul_ps(v->b2[s], x), _mm_mul_ps(v->a2[s], y));
    return y;
}

/* One frame (left, right) through the isolator, as isolate() on each channel. */
static inline void iso_sse_frame(iso_sse* v, float xl, float xr, float gl, float gm, float gh, float* y)
{
    float o[4];
    __m128 in = _mm_setr_ps(xl, xr, xl, xr);
    __m128 lu = iso_sse_step(v, 1, iso_sse_step(v, 0, in));                       /* lo, lo, up, up */
    __m128 mh = iso_sse_step(v, 3, iso_sse_step(v, 2, _mm_movehl_ps(lu, lu)));    /* mid, mid, hi, hi */
    __m128 lo = iso_sse_step(v, 4, _mm_movelh_ps(lu, _mm_setzero_ps()));
    __m128 r = _mm_add_ps(_mm_add_ps(_mm_mul_ps(_mm_set1_ps(gl), lo), _mm_mul_ps(_mm_set1_ps(gm), mh)),
                          _mm_mul_ps(_mm_set1_ps(gh), _mm_movehl_ps(mh, mh)));
    _mm_storeu_ps(o, r);
    y[0] = o[0];
    y[1] = o[1];
}
#endif

/* dB to linear for trim and EQ: the kill setting is silence. */
static float band_gain(float db)
{
    return db <= FA_DECK_EQ_KILL_DB + 0.001f ? 0.0f : fa_db_to_lin(db);
}

/* ---- the crossfader ----------------------------------------------------------- */

/* The two sides' gains at crossfader position `x`; `lin` blends from constant power (0)
 * to linear (1), so a curve change glides instead of stepping by up to 3 dB. */
static void xfader_gains(float lin, float x, float* ga, float* gb)
{
    float a = 1.0f - x, b = x;
    if (lin < 1.0f) {
        float ca = cosf(x * (float)(FA_PI / 2.0)), cb = sinf(x * (float)(FA_PI / 2.0));
        a = ca + (a - ca) * lin;
        b = cb + (b - cb) * lin;
    }
    *ga = a;
    *gb = b;
}

static float curve_lin(struct fa_mixer* m) { return fa_load32(&m->curve) == FA_XFADER_LINEAR ? 1.0f : 0.0f; }

/* ---- audio thread ------------------------------------------------------------- */


/* Mixer lane values at every MIXER_SUB edge of the block (index k = frame k x MIXER_SUB,
 * the last one at the block's end): sub-block k runs from edge[k] to edge[k + 1]. */
#define MIXER_EDGES (FA_MIXER_BLOCK / MIXER_SUB + 1)
typedef struct mix_edges {
    float v[MIX_LANES][MIXER_EDGES];     /* the value from that edge on */
    float e[MIX_LANES][MIXER_EDGES];     /* the value up to that edge (differs where a jump starts) */
    float lin[MIXER_EDGES];              /* the crossfader curve blend (fa_mixer.curve_w) */
    float ga[MIXER_EDGES], gb[MIXER_EDGES];     /* the crossfader's side gains from v[ML_XF] */
    float gae[MIXER_EDGES], gbe[MIXER_EDGES];   /* and from e[ML_XF] */
} mix_edges;

/* Bit-equal floats (a function of them gives the same result). */
static int same_bits(float a, float b) { return memcmp(&a, &b, sizeof(a)) == 0; }

/* xfader_gains, kept from the last call while its inputs stay the same: outside a
 * crossfader move or a curve glide, every sub-block edge. */
static void xfader_gains_memo(struct fa_mixer* m, float lin, float x, float* ga, float* gb)
{
    if (!same_bits(lin, m->xf_in[0]) || !same_bits(x, m->xf_in[1])) {
        m->xf_in[0] = lin;
        m->xf_in[1] = x;
        xfader_gains(lin, x, &m->xf_out[0], &m->xf_out[1]);
    }
    *ga = m->xf_out[0];
    *gb = m->xf_out[1];
}

/* A deck lane's dB as a linear gain (trim; the EQ bands, where the kill is silence),
 * kept from the last time while the lane holds the same value: a sub-block's start
 * is the last one's end except where a ramp starts. */
static float lane_gain(struct fa_deck* d, int lane, float db)
{
    if (!same_bits(db, d->gain_in[lane])) {
        d->gain_in[lane] = db;
        d->gain_out[lane] = lane == DL_TRIM ? fa_db_to_lin(db) : band_gain(db);
    }
    return d->gain_out[lane];
}

/* One deck channel over the block: trim, isolator, cue send, fader, meter, crossfader. */
static void deck_process(struct fa_mixer* m, struct fa_deck* d, const mix_edges* me, int64_t clock, uint32_t rate,
                         float* out, uint32_t frames)
{
    float* buf = d->bus->buf;
    int side = fa_load32(&d->side), cue_on = m->cue_mode != FA_CUE_NONE;
    uint32_t off, k;
    fa_tap* tap;
#ifdef FA_ISO_SSE
    iso_sse iv;
#endif
    for (off = 0, k = 0; off < frames; off += MIXER_SUB, k++) {
        uint32_t n = frames - off < MIXER_SUB ? frames - off : MIXER_SUB, i, j;
        float v0[DECK_LANES], v1[DECK_LANES], w0[3], w1[3];
        float t0, t1, l0, l1, mi0, mi1, h0, h1, g0, g1, c0, c1, x0, x1, inv = 1.0f / (float)n;
        lane_span(d->lanes, DECK_LANES, clock, clock + off, n, v0, v1);
        for (j = 0; j < 3; j++) {
            w0[j] = d->side_w[j];
            d->side_w[j] = fa_glide(d->side_w[j], (int)j == side ? 1.0f : 0.0f, n, rate);
            w1[j] = d->side_w[j];
        }
        t0 = lane_gain(d, DL_TRIM, v0[DL_TRIM]); t1 = lane_gain(d, DL_TRIM, v1[DL_TRIM]);
        l0 = lane_gain(d, DL_LOW, v0[DL_LOW]); l1 = lane_gain(d, DL_LOW, v1[DL_LOW]);
        mi0 = lane_gain(d, DL_MID, v0[DL_MID]); mi1 = lane_gain(d, DL_MID, v1[DL_MID]);
        h0 = lane_gain(d, DL_HIGH, v0[DL_HIGH]); h1 = lane_gain(d, DL_HIGH, v1[DL_HIGH]);
        g0 = v0[DL_GAIN]; g1 = v1[DL_GAIN];
        c0 = v0[DL_CUE]; c1 = v1[DL_CUE];
        /* This deck's crossfader gain (its side mix) at both ends of the sub-block. */
        x0 = w0[0] + w0[1] * me->ga[k] + w0[2] * me->gb[k];
        x1 = w1[0] + w1[1] * me->gae[k + 1] + w1[2] * me->gbe[k + 1];
#ifdef FA_ISO_SSE
        iso_sse_load(&iv, m, d->z);
#endif
        for (i = 0; i < n; i++) {
            float f = (float)i * inv;
            float tr = t0 + (t1 - t0) * f, gl = l0 + (l1 - l0) * f, gm = mi0 + (mi1 - mi0) * f, gh = h0 + (h1 - h0) * f;
            float fd = g0 + (g1 - g0) * f, cs = c0 + (c1 - c0) * f, xf = x0 + (x1 - x0) * f;
            float* p = buf + (size_t)(off + i) * 2;
            float* o = out + (size_t)(off + i) * 2;
            int c;
#ifdef FA_ISO_SSE
            float ys[2];
            iso_sse_frame(&iv, p[0] * tr, p[1] * tr, gl, gm, gh, ys);
#endif
            for (c = 0; c < 2; c++) {
#ifdef FA_ISO_SSE
                float y = ys[c];
#else
                float y = isolate(m, d->z, c, p[c] * tr, gl, gm, gh);
#endif
                if (cue_on) m->cue[(size_t)(off + i) * 2 + c] += y * cs;
                y *= fd;
                p[c] = y;          /* for the meter */
                o[c] += y * xf;
            }
        }
#ifdef FA_ISO_SSE
        iso_sse_store(&iv, d->z);
#endif
        /* Flush denormals so a deck left silent stays cheap: every sub-block, since the
         * upper sections decay from 1e-20 into the denormal range in about 180 frames. */
        {
            float* z = &d->z[0][0][0];
            for (i = 0; i < ISO_SECTIONS * 4; i++) if (fabsf(z[i]) < 1e-20f) z[i] = 0.0f;
        }
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
    float lin = curve_lin(m);
    mix_edges me;
    m->check_now = (struct fa_mic_check*)fa_load_ptr(&m->check);   /* the same for the mics and the render */
    fa_lanes_take(m->lanes, MIX_LANES, dev->rate);
    for (i = 0; i < m->deck_count; i++) fa_lanes_take(m->decks[i].lanes, DECK_LANES, dev->rate);
    for (off = 0, k = 0;; off += MIXER_SUB, k++) {
        uint32_t at = off < frames ? off : frames;
        for (p = 0; p < MIX_LANES; p++) {
            me.v[p][k] = fa_lane_value_at_frame(&m->lanes[p].v, clock, clock + at, 1);
            me.e[p][k] = fa_lane_value_at_frame(&m->lanes[p].v, clock, clock + at, 0);
        }
        me.lin[k] = m->curve_w;
        xfader_gains_memo(m, me.lin[k], me.e[ML_XF][k], &me.gae[k], &me.gbe[k]);
        xfader_gains_memo(m, me.lin[k], me.v[ML_XF][k], &me.ga[k], &me.gb[k]);
        if (off >= frames) break;
        m->curve_w = fa_glide(m->curve_w, lin, frames - off < MIXER_SUB ? frames - off : MIXER_SUB, dev->rate);
    }
    if (m->cue_mode != FA_CUE_NONE) memset(m->cue, 0, (size_t)frames * FA_DEVICE_CHANNELS * sizeof(float));
    for (i = 0; i < m->deck_count; i++) deck_process(m, &m->decks[i], &me, clock, dev->rate, out, frames);
    fa_mixer_mics_process(m, dev, out, frames);   /* the mics join the music before the master gain */
    m->block++;
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
    fa_lanes_end(m->lanes, MIX_LANES, clock, frames);
    for (i = 0; i < m->deck_count; i++) fa_lanes_end(m->decks[i].lanes, DECK_LANES, clock, frames);
}


/* The master (after the limiter and the device mute) and the cue onto the device's channels.
 * The device mute silences the cue sends too, gliding as the master did over the block
 * (from mute0 to mute1). */
static void lay_out(struct fa_mixer* m, const float* master, float* out, uint32_t channels, uint32_t frames,
                    float mute0, float mute1, uint32_t rate)
{
    uint32_t i, c, cue_at = m->cue_mode == FA_CUE_CHANNELS && channels >= m->cue_channel + 2 ? m->cue_channel : 0;
    float mute = mute0;
    int cue_here = m->cue_mode == FA_CUE_CHANNELS || m->cue_mode == FA_CUE_SPLIT;   /* not a second device */
    if (channels == 2 && m->cue_mode != FA_CUE_SPLIT) {
        memcpy(out, master, (size_t)frames * 2 * sizeof(float));
        return;
    }
    for (i = 0; i < frames; i++) {
        const float* mm = master + (size_t)i * 2;
        float* o = out + (size_t)i * channels;
        float cl = 0.0f, cr = 0.0f;
        if (cue_here) {
            const float* cs = m->cue + (size_t)i * 2;
            float audible;
            if (mute != mute1) mute = fa_glide(mute, mute1, 1, rate);
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
        int64_t clock = fa_load64(&dev->clock);
        n = frames - done < FA_MIXER_BLOCK ? frames - done : FA_MIXER_BLOCK;
        fa_device_render_at(dev, m->master, n, now_ns + (int64_t)((double)done * 1e9 / dev->rate));
        if (fa_load_ptr(&m->encoders)) fa_encode_mixer_push(m, FA_MIXER_TAP_MASTER, m->master, n, clock);
        if (m->check_now) {   /* the setup check: its test sound instead of the music */
            fa_mic_check_render(m->check_now, m->master, n, clock);
            m->check_now = NULL;
        }
        if (m->cue_dev) fa_mixer_cue_render(m, dev, m->master, n, mute0, dev->mute_level);   /* the headphones */
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
    for (i = 0; i < m->deck_count; i++) {
        fa_tap_free((fa_tap*)fa_load_ptr(&m->decks[i].tap));
        free(m->decks[i].segs);
    }
    while (fa_load32(&m->refs)) fa_sleep_ms(0);   /* fa_mixer_stream_leaving finds the deck empty */
    fa_mixer_mics_free_removed(m);
    fa_mic_bus_free((fa_mic_bus*)fa_load_ptr(&m->bus));
    fa_mutex_destroy(&m->cb_lock);
    fa_mutex_destroy(&m->lock);
    free(m->cue);
    free(m->cue_master);
    free(m);
}

/* Whether this thread is the engine's event thread (a mixer callback may be running on it). */
static int on_event_thread(struct fa_mixer* m)
{
    return (uintptr_t)fa_load64(&m->engine->event_thread_id) == fa_thread_current_id();
}

void fa_mixer_wait_callbacks(struct fa_mixer* m)
{
    if (on_event_thread(m)) return;
    while (fa_load32(&m->busy)) fa_sleep_ms(1);
}

struct fa_mixer* fa_mixer_hold(fa_device* dev)
{
    struct fa_mixer* m = (struct fa_mixer*)fa_load_ptr(&dev->mixer);
    if (m) fa_fetch_add32(&m->busy, 1);   /* fa_mixer_destroy waits for it after the close */
    return m;
}

void fa_mixer_release(struct fa_mixer* m)
{
    /* Read first: once `busy` is 0, a destroying thread may free the mixer. Only this
     * thread (the event thread, inside its own callback) sets `doomed`. */
    int doomed = fa_load32(&m->doomed);
    fa_fetch_add32(&m->busy, -1);
    if (doomed) free_mixer(m);   /* destroyed by its own callback */
}

FA_API fa_result FA_CALL fa_mixer_create(fa_engine* engine, const fa_mixer_config* config, fa_mixer** out_mixer)
{
    fa_mixer_config c;
    struct fa_mixer* m;
    fa_device* dev = NULL;
    fa_result r;
    uint32_t i, channels;
    int l;
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
    if ((uint32_t)c.cue_mode > FA_CUE_SPLIT || (uint32_t)c.latency > FA_LATENCY_EXCLUSIVE ||
        (c.flags & ~FA_MIXER_CUE_LOWEST_LATENCY))
        return FA_ERROR_INVALID_ARG;
    if (c.cue_channel == 0) c.cue_channel = 2;
    if (c.cue_channel < 2 || c.cue_channel > 30) return FA_ERROR_INVALID_ARG;
    /* Channels 3-4: the device opens with all its channels (the null output, which has
     * as many as asked, with just enough). */
    channels = FA_DEVICE_CHANNELS;
    if (c.cue_mode == FA_CUE_CHANNELS) channels = (engine->config.flags & FA_ENGINE_NULL_OUTPUT) ? c.cue_channel + 2 : 0;
    r = fa_device_open_with(engine, c.device_id, channels, c.latency, &dev);
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
    if (fa_mutex_init(&m->cb_lock) != 0) {
        fa_mutex_destroy(&m->lock);
        free(m);
        fa_device_close(dev);
        return FA_ERROR_GENERIC;
    }
    m->engine = engine;
    m->device = dev;
    m->deck_count = c.deck_count;
    m->cue_mode = c.cue_mode;
    m->cue_channel = c.cue_channel;
    m->flags = c.flags;
    if (c.cue_mode != FA_CUE_NONE) {
        m->cue = (float*)calloc((size_t)FA_MIXER_BLOCK * FA_DEVICE_CHANNELS, sizeof(float));
        m->cue_master = (float*)calloc(FA_MIXER_BLOCK, sizeof(float));
        if (!m->cue || !m->cue_master) {
            fa_device_close(dev);
            free_mixer(m);
            return FA_ERROR_OUT_OF_MEMORY;
        }
    }
    fa_store32(&m->curve, FA_XFADER_CONSTANT_POWER);
    xfader_gains(0.0f, 0.0f, &m->xf_out[0], &m->xf_out[1]);   /* the memos start at 0 (xf_in, gain_in) */
    fa_lane_init(&m->lanes[ML_XF], 0.5f);
    fa_lane_init(&m->lanes[ML_MASTER], 1.0f);
    fa_lane_init(&m->lanes[ML_CUE_MIX], 0.0f);
    fa_lane_init(&m->lanes[ML_CUE_GAIN], 1.0f);
    fa_mixer_fx_defaults(m);
    m->lp1 = fa_mixer_design(0, ISO_LOW_HZ, dev->rate);
    m->hp1 = fa_mixer_design(1, ISO_LOW_HZ, dev->rate);
    m->lp2 = fa_mixer_design(0, ISO_HIGH_HZ, dev->rate);
    m->hp2 = fa_mixer_design(1, ISO_HIGH_HZ, dev->rate);
    m->ap2 = fa_mixer_design(2, ISO_HIGH_HZ, dev->rate);
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
        fa_store_float(&d->sync_range, 0.16f);
        d->side_w[side] = 1.0f;
        for (l = 0; l < DECK_LANES; l++) d->gain_out[l] = l == DL_TRIM ? fa_db_to_lin(0.0f) : band_gain(0.0f);
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
    if (c.cue_mode == FA_CUE_DEVICE && (r = fa_mixer_cue_create(m, c.cue_device_id)) != FA_OK) {
        fa_device_close(dev);
        free_mixer(m);
        return r;
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
    mixer->closing = 1;   /* a service waiting for the lock leaves the device alone */
    for (i = 0; i < mixer->deck_count; i++) {
        fa_stream* s = (fa_stream*)fa_exchange_ptr(&mixer->decks[i].stream, NULL);
        if (!s) continue;
        fa_stream_detach_internal(s);
        fa_stream_set_loop_region(s, 0, 0);   /* detached first: nothing to join (see fa_deck_load) */
    }
    fa_mutex_unlock(&mixer->lock);
    dev = mixer->device;
    fa_encode_mixer_closing(mixer);   /* recordings of its taps end here */
    /* Stop it first: a callback without the mixer would lay stereo out on all the
     * device's channels (a 4-channel cue device), a burst of noise until the close. */
    fa_device_stop(dev);
    fa_exchange_ptr(&dev->mixer, NULL);
    fa_device_wait_quiescent(dev);   /* no callback is in the mixer any more */
    fa_mutex_lock(&mixer->lock);
    fa_mixer_cue_free(mixer);        /* closes the headphones */
    fa_mutex_unlock(&mixer->lock);
    fa_mixer_mics_free(mixer);       /* closes the inputs */
    fa_device_close(dev);
    /* After the close the event thread cannot pick the mixer up again; wait out a
     * service that has it, unless that is us (destroyed from its own callback): then
     * the service frees it when the callback returns. */
    if (on_event_thread(mixer) && fa_load32(&mixer->busy)) {
        fa_store32(&mixer->doomed, 1);
        return;
    }
    fa_mixer_wait_callbacks(mixer);
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

/* While the master is closed: the inputs and the headphones follow it to the new mode. */
static void reopen_inputs(void* user, fa_latency_mode mode)
{
    fa_mixer_captures_reopen((struct fa_mixer*)user, mode);
    fa_mixer_cue_reopen((struct fa_mixer*)user, mode);
}

/* Caller holds the mixer lock. */
static fa_result switch_mode_locked(struct fa_mixer* mixer, fa_latency_mode mode)
{
    fa_result r = FA_OK;
    if (mode != mixer->device->open_mode) {
        fa_capture* c;
        r = fa_device_reopen_mode(mixer->device, mode, reopen_inputs, mixer);
        for (c = mixer->captures; c; c = c->next) c->mode_lost = 0;   /* lost for good now */
    }
    return r;
}

fa_result fa_mixer_switch_latency_mode(struct fa_mixer* mixer, fa_latency_mode mode)
{
    fa_result r;
    fa_mutex_lock(&mixer->lock);
    r = switch_mode_locked(mixer, mode);
    fa_mutex_unlock(&mixer->lock);
    return r;
}

FA_API fa_result FA_CALL fa_mixer_set_latency_mode(fa_mixer* mixer, fa_latency_mode mode)
{
    fa_result r;
    if (!mixer || (uint32_t)mode > FA_LATENCY_EXCLUSIVE) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&mixer->lock);
    /* A setup check switches modes itself and times the devices: not under its feet.
     * Tested under the lock, which the check takes to start. */
    r = fa_load32(&mixer->checking) ? FA_ERROR_BUSY : switch_mode_locked(mixer, mode);
    fa_mutex_unlock(&mixer->lock);
    return r;
}

FA_API fa_result FA_CALL fa_mixer_get_latency_mode(fa_mixer* mixer, fa_latency_mode* out_mode)
{
    if (!mixer || !out_mode) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&mixer->lock);
    *out_mode = mixer->device->open_mode;
    fa_mutex_unlock(&mixer->lock);
    return FA_OK;
}

static fa_result mix_range(fa_mix_param p, float* v)
{
    if (*v != *v) return FA_ERROR_INVALID_ARG;
    switch (p) {
    case FA_MIX_CROSSFADER: case FA_MIX_CUE_MIX: *v = clampf(*v, 0.0f, 1.0f); return FA_OK;
    case FA_MIX_MASTER_GAIN: case FA_MIX_CUE_GAIN: *v = clampf(*v, 0.0f, 4.0f); return FA_OK;
    case FA_MIX_CUE_TRIM_MS: *v = clampf(*v, -250.0f, 250.0f); return FA_OK;
    default: return FA_ERROR_INVALID_ARG;
    }
}

FA_API fa_result FA_CALL fa_mixer_set_param(fa_mixer* mixer, fa_mix_param param, float value)
{
    fa_result r;
    if (!mixer) return FA_ERROR_INVALID_ARG;
    if ((r = mix_range(param, &value)) != FA_OK) return r;
    if (param == FA_MIX_CUE_TRIM_MS) {   /* no glide: the audio thread lines the outputs up anew */
        if (value != value) return FA_ERROR_INVALID_ARG;   /* NaN: clampf lets it through */
        fa_store_float(&mixer->cue_trim, value);
        return FA_OK;
    }
    fa_mutex_lock(&mixer->lock);
    fa_lane_post(&mixer->lanes[param], value, MIXER_DEZIPPER_NS, NULL, 0, 0, -1, 0, NULL);
    fa_mutex_unlock(&mixer->lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_mixer_get_param(fa_mixer* mixer, fa_mix_param param, float* out_value)
{
    float v = 0.0f;
    if (!mixer || !out_value || mix_range(param, &v) != FA_OK) return FA_ERROR_INVALID_ARG;
    if (param == FA_MIX_CUE_TRIM_MS) {
        *out_value = fa_load_float(&mixer->cue_trim);
        return FA_OK;
    }
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
    if (!mixer || duration < 0 || device_time < 0 || param == FA_MIX_CUE_TRIM_MS) return FA_ERROR_INVALID_ARG;
    if ((r = fa_shape_from_ramp(ramp, FA_CURVE_LINEAR, &shape)) != FA_OK) return r;
    if ((r = mix_range(param, &target)) != FA_OK) return r;
    fa_mutex_lock(&mixer->lock);
    fa_lane_post(&mixer->lanes[param], target, duration, &shape, 0, 1, fa_ramp_frame(mixer->device->rate, device_time), 0, NULL);
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

int fa_mixer_bus_slips(fa_bus* bus) { return bus && bus->deck && fa_load32(&bus->deck->slip); }

void fa_mixer_stream_leaving(struct fa_stream* s, fa_bus* to)
{
    struct fa_deck* d = NULL;
    struct fa_mixer* m;
    fa_bus* bus;
    /* The bus and its mixer are read under api_lock, which fa_mixer_destroy's detach
     * takes too, and `refs` keeps the mixer until we are done with it. */
    fa_mutex_lock(&s->api_lock);
    bus = (fa_bus*)fa_load_ptr(&s->bus);
    if (bus && bus != to) d = bus->deck;
    if (d) fa_fetch_add32(&d->mixer->refs, 1);
    fa_mutex_unlock(&s->api_lock);
    if (!d) return;
    m = d->mixer;
    /* Off the deck under the mixer lock: deck calls and the event thread's sync and
     * auto-mix use a deck's stream under it, and find the deck empty from here on. */
    fa_mutex_lock(&m->lock);
    if (fa_cas_ptr(&d->stream, s, NULL)) fa_deck_sync_loaded(d);
    fa_mutex_unlock(&m->lock);
    fa_fetch_add32(&m->refs, -1);
}

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
    xfader_gains(curve_lin(m), fa_lane_get(&m->lanes[ML_XF]), &ga, &gb);
    g = fa_lane_get(&d->lanes[DL_GAIN]) * (side == 1 ? ga : side == 2 ? gb : 1.0f);
    return g > 0.001f;   /* -60 dB */
}

/* The deck of a mixer other than `m` that `s` is on, its mixer kept by `refs` as in
 * fa_mixer_stream_leaving; NULL when there is none. */
static struct fa_deck* foreign_deck(struct fa_stream* s, struct fa_mixer* m)
{
    struct fa_deck* d = NULL;
    fa_bus* bus;
    fa_mutex_lock(&s->api_lock);
    bus = (fa_bus*)fa_load_ptr(&s->bus);
    if (bus && bus->deck && bus->deck->mixer != m) d = bus->deck;
    if (d) fa_fetch_add32(&d->mixer->refs, 1);
    fa_mutex_unlock(&s->api_lock);
    return d;
}

/* The locks of `m` and of `o` (another mixer, or NULL), in address order so that two
 * mixers loading each other's streams cannot deadlock. */
static void lock_mixers(struct fa_mixer* m, struct fa_mixer* o)
{
    if (o && o < m) fa_mutex_lock(&o->lock);
    fa_mutex_lock(&m->lock);
    if (o && o > m) fa_mutex_lock(&o->lock);
}

static void unlock_foreign(struct fa_deck* od)
{
    if (!od) return;
    fa_mutex_unlock(&od->mixer->lock);
    fa_fetch_add32(&od->mixer->refs, -1);
}

FA_API fa_result FA_CALL fa_deck_load(fa_deck* deck, fa_stream* stream, uint32_t flags)
{
    struct fa_mixer* m;
    struct fa_deck* od;
    fa_stream* old;
    fa_result r = FA_OK;
    int od_had = 0;
    if (!deck || (flags & ~FA_DECK_LOAD_FORCE)) return FA_ERROR_INVALID_ARG;
    m = deck->mixer;
    if (stream && stream->engine != m->engine) return FA_ERROR_INVALID_ARG;
    /* A stream on a deck of another mixer leaves it under that mixer's lock too, held
     * with ours, so it stays there if the load fails; looked up again under the locks
     * in case it moved meanwhile. */
    for (;;) {
        struct fa_deck* now;
        od = stream ? foreign_deck(stream, m) : NULL;
        lock_mixers(m, od ? od->mixer : NULL);
        now = stream ? foreign_deck(stream, m) : NULL;
        if (now) fa_fetch_add32(&now->mixer->refs, -1);   /* when it is `od`, od's own ref keeps it */
        if (now == od) break;
        fa_mutex_unlock(&m->lock);
        unlock_foreign(od);
    }
    if (od) od_had = deck_stream(od) == stream;
    old = deck_stream(deck);
    if (stream == old) {
        fa_mutex_unlock(&m->lock);
        unlock_foreign(od);
        return FA_OK;
    }
    if (old && !(flags & FA_DECK_LOAD_FORCE) && deck_heard(deck)) {
        fa_mutex_unlock(&m->lock);
        unlock_foreign(od);
        return FA_ERROR_BUSY;
    }
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
        r = fa_stream_attach_internal(stream, m->device, deck->bus);
        if (r != FA_OK) {
            if (shortened) {   /* not loaded: the stream keeps its own decode-ahead */
                o.latency_ms = old_latency;
                fa_stream_set_tempo_options(stream, &o);
            }
            fa_mutex_unlock(&m->lock);
            unlock_foreign(od);
            return r;
        }
        if (od_had) fa_deck_sync_loaded(od);   /* the other mixer's deck is empty now */
        unlock_foreign(od);
    }
    fa_exchange_ptr(&deck->stream, stream);
    if (stream) fa_stream_beat_grid_now(stream, NULL, NULL, 0, NULL);   /* a module's grid starts rendering now */
    deck->cue_point = FA_FRAMES_UNSET;
    fa_deck_sync_loaded(deck);
    fa_automix_loaded(m, deck, stream);
    if (old) {
        /* Detached first: clearing the loop while attached would join it in the
         * ring (device wait, decoder seek, prefill) only to throw that away. */
        fa_stream_detach_internal(old);
        fa_stream_set_loop_region(old, 0, 0);
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
    case FA_DECK_SYNC_RANGE: *v = clampf(*v, 0.01f, 1.0f); return FA_OK;
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
    if (param == FA_DECK_SYNC_RANGE) {    /* read by sync, not the audio thread */
        fa_store_float(&deck->sync_range, value);
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
    if (param == FA_DECK_SYNC_RANGE) { *out_value = fa_load_float(&deck->sync_range); return FA_OK; }
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
    if (!deck || duration < 0 || device_time < 0 || param == FA_DECK_XFADER_SIDE || param == FA_DECK_CUE ||
        param == FA_DECK_SYNC_RANGE)
        return FA_ERROR_INVALID_ARG;
    if ((r = fa_shape_from_ramp(ramp, FA_CURVE_LINEAR, &shape)) != FA_OK) return r;
    if ((r = deck_range(param, &target)) != FA_OK) return r;
    fa_mutex_lock(&deck->mixer->lock);
    fa_lane_post(deck_lane(deck, param), target, duration, &shape, 0, 1, fa_ramp_frame(deck->mixer->device->rate, device_time), 0, NULL);
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

fa_frames fa_deck_cue_point_now(struct fa_deck* d) { return cue_point_of(d); }

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

FA_API fa_result FA_CALL fa_deck_set_slip(fa_deck* deck, int32_t on)
{
    fa_stream* s;
    if (!deck) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&deck->mixer->lock);
    fa_store32(&deck->slip, on ? 1 : 0);
    if (!on && (s = deck_stream(deck))) fa_stream_slip_cancel(s);   /* playback carries on from where it is */
    fa_mutex_unlock(&deck->mixer->lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_deck_get_slip(fa_deck* deck, int32_t* out_on)
{
    if (!deck || !out_on) return FA_ERROR_INVALID_ARG;
    *out_on = fa_load32(&deck->slip);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_deck_get_slip_position(fa_deck* deck, fa_frames* out_frame)
{
    fa_stream* s;
    if (!deck || !out_frame) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&deck->mixer->lock);
    s = deck_stream(deck);
    *out_frame = s ? fa_stream_slip_position(s) : FA_FRAMES_UNSET;
    fa_mutex_unlock(&deck->mixer->lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_deck_slip_return(fa_deck* deck)
{
    fa_stream* s;
    fa_result r = FA_OK;
    if (!deck) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&deck->mixer->lock);
    if ((s = deck_stream(deck))) r = fa_stream_slip_return(s);
    fa_mutex_unlock(&deck->mixer->lock);
    return r;
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
