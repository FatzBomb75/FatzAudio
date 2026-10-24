/* Mixer mics: capture devices bridged into the master callback, and each mic's
 * channel strip (low cut, gain, gate, EQ, meter, mute, pan, sends) into the mic bus
 * (mic_fx.c).
 *
 * An input device runs on its own clock. Its callback copies the channels the mics
 * read into a bridge (bridge.c, capture_write); the master callback takes them out
 * through it, at the ratio of the two rates nudged by up to 0.2%, so drift and a
 * different rate (a 44.1 kHz mic on a 48 kHz master) are absorbed without a click.
 *
 * Mics live in two slots the master callback reads; adding publishes a slot,
 * removing clears it and waits for the callback to be past it. Input loss is
 * flagged by the backend's notification and reported by the event thread
 * (fa_mixer_service). */
#include "mixer.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MIC_LOW_CUT_HZ 80.0
#define MIC_WARM_MS 50           /* silence after adding, while the lane fills with its channel */
#define MIC_FADE_MS 10
#define GATE_ATTACK_MS 1.0
#define GATE_HOLD_MS 100.0
#define GATE_RELEASE_MS 150.0
#define GATE_FOLLOW_MS 10.0      /* the level follower's decay */
#define GATE_OFF_DB (-99.9f)
#define BRIDGE_MAX_ADJUST 0.002  /* the most the ratio is nudged: 0.2%, about 3.5 cents */
#define RING_SECONDS 0.5

/* ---- the input side (capture callback) ------------------------------------------ */

static float pick(const float* frame, uint32_t ch, int32_t src)
{
    uint32_t c;
    float s = 0.0f;
    if (src >= 0) return (uint32_t)src < ch ? frame[src] : 0.0f;
    if (src != CAPTURE_LANE_ALL) return 0.0f;
    for (c = 0; c < ch; c++) s += frame[c];
    return s;
}

/* The capture callback's work: the lanes' channels into the bridge. A full ring drops
 * the newest frames (the reader catches up by dropping what it holds over target). */
static void capture_write(fa_capture* c, const float* in, uint32_t n, uint32_t ch)
{
    int32_t s0 = fa_load32(&c->lane_src[0]), s1 = fa_load32(&c->lane_src[1]);
    uint32_t w, i;
    n = fa_bridge_space(&c->br, n, &w);
    for (i = 0; i < n; i++) {
        float* f = fa_bridge_frame(&c->br, w + i);
        const float* x = in + (size_t)i * ch;
        f[0] = pick(x, ch, s0);
        f[1] = pick(x, ch, s1);
    }
    fa_bridge_commit(&c->br, w, n);
}

static void capture_callback(ma_device* d, void* out, const void* in, ma_uint32 frames)
{
    fa_capture* c = (fa_capture*)d->pUserData;
    (void)out;
    if (c && in) capture_write(c, (const float*)in, frames, c->channels);
}

static void flag_lost(fa_capture* c)
{
    fa_device* dev = c->mixer->device;
    fa_store32(&c->lost, 1);
    fa_store32(&dev->mixer_events, 1);
    fa_cond_broadcast(&dev->engine->event_cond);
}

/* The backend's thread: an input that stops without being asked to has gone away. */
static void capture_notification(const ma_device_notification* n)
{
    fa_capture* c = (fa_capture*)n->pDevice->pUserData;
    if (!c || fa_load32(&c->stopping)) return;
    if (n->type == ma_device_notification_type_stopped) flag_lost(c);
}

/* `frames` frames at the master rate into c->out, both lanes. `hold`: a setup check is
 * timing the input, so the ratio stays at the nominal one (the drift of two crystals over
 * its 3 s is a fraction of a ms). */
static void capture_pull(fa_capture* c, fa_device* dev, uint32_t frames, int hold)
{
    double in_period = c->device_ok ? (double)c->device.capture.internalPeriodSizeInFrames : 0.0;
    fa_bridge_pull(&c->br, c->out, frames, dev->rate, in_period, (double)fa_load32(&dev->period_frames), hold);
}

/* ---- the channel strip (master callback) ---------------------------------------- */

static float db_to_lin(float db) { return powf(10.0f, db / 20.0f); }

/* Per sub-block edge (start, end): the gains after the strip. */
typedef struct strip_out {
    float audible[2], monitor[2], left[2], right[2], reverb[2], echo[2];
} strip_out;

static void mic_process(struct fa_mic* mic, fa_mic_bus* b, fa_device* dev, int64_t clock, uint32_t frames)
{
    fa_capture* c = mic->cap;
    uint32_t rate = dev->rate, off, i;
    float thr = fa_load_float(&mic->gate_db);
    int gate_on = thr > GATE_OFF_DB;
    float open_lin = db_to_lin(thr), close_lin = open_lin * 0.5f;
    float follow = expf(-1.0f / ((float)GATE_FOLLOW_MS * 0.001f * (float)rate));
    float att = 1.0f / ((float)GATE_ATTACK_MS * 0.001f * (float)rate);
    float rel = 1.0f / ((float)GATE_RELEASE_MS * 0.001f * (float)rate);
    float fade_step = 1.0f / ((float)MIC_FADE_MS * 0.001f * (float)rate);
    int64_t hold_frames = (int64_t)(GATE_HOLD_MS * 0.001 * (double)rate);
    strip_out so[FA_MIXER_BLOCK / MIXER_SUB + 1];
    int host = (mic->flags & FA_MIC_DUCKS_MUSIC) != 0;
    fa_tap* tap;
    uint32_t k;
    take_lanes(mic->lanes, MIC_LANES, rate);
    /* Low cut, gain and gate into buf (the mono voice on both channels, for the EQ and
     * the meter); the output gains (mute, monitor, pan, sends) kept per sub-block. */
    for (off = 0, k = 0; off < frames; off += MIXER_SUB, k++) {
        uint32_t n = frames - off < MIXER_SUB ? frames - off : MIXER_SUB;
        float v0[MIC_LANES], v1[MIC_LANES], g0, g1, inv = 1.0f / (float)n;
        int e;
        lane_span(mic->lanes, MIC_LANES, clock, clock + off, n, v0, v1);
        g0 = db_to_lin(v0[MIC_GAIN]);
        g1 = db_to_lin(v1[MIC_GAIN]);
        for (i = 0; i < n; i++) {
            float f = (float)i * inv, x = c->out[(size_t)(off + i) * CAPTURE_LANES + mic->lane];
            float lc = v0[MIC_LOW_CUT] + (v1[MIC_LOW_CUT] - v0[MIC_LOW_CUT]) * f, h;
            if (mic->warm > 0) {
                mic->warm--;
                x = 0.0f;
            } else if (mic->fade < 1.0f) {
                mic->fade = fminf(1.0f, mic->fade + fade_step);
                x *= mic->fade;
            }
            h = fa_biquad_run(&mic->hp, mic->hp_z, x);
            x += (h - x) * lc;
            x *= g0 + (g1 - g0) * f;
            if (gate_on) {
                float a = fabsf(x);
                mic->env = a > mic->env ? a : mic->env * follow;
                if (mic->env >= open_lin) mic->gate_hold = hold_frames + 1;   /* open, or stays open */
                else if (mic->env >= close_lin && mic->gate_hold > 0) mic->gate_hold = hold_frames + 1;
                if (mic->gate_hold > 0) {
                    mic->gate_hold--;
                    mic->gate = fminf(1.0f, mic->gate + att);
                } else {
                    mic->gate = fmaxf(0.0f, mic->gate - rel);
                }
                x *= mic->gate;
            } else {
                mic->gate = 1.0f;
            }
            mic->buf[(size_t)(off + i) * 2] = x;
            mic->buf[(size_t)(off + i) * 2 + 1] = x;
        }
        for (e = 0; e < 2; e++) {
            const float* v = e ? v1 : v0;
            so[k].audible[e] = 1.0f - v[MIC_MUTE];
            so[k].monitor[e] = v[MIC_MONITOR];
            so[k].left[e] = v[MIC_PAN] > 0.0f ? 1.0f - v[MIC_PAN] : 1.0f;
            so[k].right[e] = v[MIC_PAN] < 0.0f ? 1.0f + v[MIC_PAN] : 1.0f;
            so[k].reverb[e] = v[MIC_REVERB];
            so[k].echo[e] = v[MIC_ECHO];
        }
    }
    if (fabsf(mic->hp_z[0]) < 1e-20f) mic->hp_z[0] = 0.0f;
    if (fabsf(mic->hp_z[1]) < 1e-20f) mic->hp_z[1] = 0.0f;
    if (mic->env < 1e-20f) mic->env = 0.0f;
    if (fa_eq_begin(&mic->eq_st, &mic->eq, rate, 0)) fa_eq_process(&mic->eq_st, mic->buf, frames);
    tap = (fa_tap*)fa_load_ptr(&mic->tap);
    if (tap) fa_tap_write(tap, clock, mic->buf, frames);
    /* Into the bus: heard (vox and the sends) or not (dry, for the vocals tap), and the
     * host's level for the ducker. */
    if (host) b->hosts++;
    for (off = 0, k = 0; off < frames; off += MIXER_SUB, k++) {
        const strip_out* q = &so[k];
        uint32_t n = frames - off < MIXER_SUB ? frames - off : MIXER_SUB;
        float inv = 1.0f / (float)n;
        if (q->audible[0] == 0.0f && q->audible[1] == 0.0f) continue;
        for (i = 0; i < n; i++) {
            float f = (float)i * inv;
            size_t at = off + i;
            float y = mic->buf[at * 2] * (q->audible[0] + (q->audible[1] - q->audible[0]) * f);
            float mo = q->monitor[0] + (q->monitor[1] - q->monitor[0]) * f, heard = y * mo, quiet = y - heard;
            float l = q->left[0] + (q->left[1] - q->left[0]) * f, r = q->right[0] + (q->right[1] - q->right[0]) * f;
            float rs = q->reverb[0] + (q->reverb[1] - q->reverb[0]) * f, es = q->echo[0] + (q->echo[1] - q->echo[0]) * f;
            b->vox[at * 2] += heard * l;
            b->vox[at * 2 + 1] += heard * r;
            b->dry[at * 2] += quiet * l;
            b->dry[at * 2 + 1] += quiet * r;
            b->rsend[at * 2] += heard * rs * 0.5f;   /* the reverb takes left + right */
            b->rsend[at * 2 + 1] += heard * rs * 0.5f;
            b->esend[at] += heard * es;
            if (host && fabsf(y) > b->duck[at]) b->duck[at] = fabsf(y);
        }
    }
    end_lanes(mic->lanes, MIC_LANES, clock, frames);
}

void fa_mixer_mics_process(struct fa_mixer* m, fa_device* dev, float* out, uint32_t frames)
{
    int64_t clock = fa_load64(&dev->clock);
    fa_mic_bus* b = (fa_mic_bus*)fa_load_ptr(&m->bus);
    uint32_t i;
    size_t stereo = (size_t)frames * FA_DEVICE_CHANNELS * sizeof(float);
    if (!b) {
        /* No mic yet: the vocals tap records silence, so a mic added later lines up. */
        static const float silence[FA_MIXER_BLOCK * FA_DEVICE_CHANNELS];
        if (fa_load_ptr(&m->encoders)) fa_encode_mixer_push(m, FA_MIXER_TAP_VOCALS, silence, frames, clock);
        return;
    }
    memset(b->vox, 0, stereo);
    memset(b->dry, 0, stereo);
    memset(b->rsend, 0, stereo);
    memset(b->esend, 0, (size_t)frames * sizeof(float));
    memset(b->duck, 0, (size_t)frames * sizeof(float));
    b->hosts = 0;
    for (i = 0; i < FA_MIC_MAX; i++) {
        struct fa_mic* mic = (struct fa_mic*)fa_load_ptr(&m->mics[i]);
        if (!mic) continue;
        if (mic->cap->pulled != m->block + 1) {   /* two mics on one input pull it once */
            capture_pull(mic->cap, dev, frames, m->check_now != NULL);
            mic->cap->pulled = m->block + 1;
        }
        if (m->check_now && m->check_now->mic == mic) fa_mic_check_record(m->check_now, mic->cap->out, mic->lane, frames, clock);
        mic_process(mic, b, dev, clock, frames);
    }
    fa_mic_bus_process(m, b, clock, out, frames);
}

/* ---- inputs (API threads, mixer lock) ------------------------------------------- */

static void capture_free(fa_capture* c)
{
    if (c->device_ok) {
        fa_store32(&c->stopping, 1);
        ma_device_uninit(&c->device);   /* waits for its callback */
    }
    fa_bridge_free(&c->br);
    free(c->id);
    free(c);
}

/* Opens c's input device in `mode` and sizes the ring and the resampler for its rate. No
 * master callback reads `c` meanwhile (it isn't published yet, or the master is closed). */
static fa_result capture_start(fa_capture* c, fa_latency_mode mode)
{
    struct fa_mixer* m = c->mixer;
    fa_engine* e = m->engine;
    ma_device_config cfg;
    ma_device_id did;
    ma_result mr;
    fa_result r;
    int has_id;
    r = fa_device_parse_id(e, FA_DEVICE_CAPTURE, c->id, &did, &has_id);
    if (r != FA_OK) return r;
    cfg = ma_device_config_init(ma_device_type_capture);
    cfg.capture.pDeviceID = has_id ? &did : NULL;
    cfg.capture.format = ma_format_f32;
    cfg.capture.channels = 0;            /* the device's own */
    cfg.sampleRate = 0;                  /* and its own rate: the bridge converts, not the OS */
    cfg.periodSizeInMilliseconds = mode == FA_LATENCY_LOW ? 5 : e->config.period_ms;
    if (mode == FA_LATENCY_EXCLUSIVE) {  /* as the master (device.c) */
        cfg.capture.shareMode = ma_share_mode_exclusive;
        cfg.periodSizeInMilliseconds = 3;
    }
    cfg.dataCallback = capture_callback;
    cfg.notificationCallback = capture_notification;
    cfg.pUserData = c;
    cfg.wasapi.usage = ma_wasapi_usage_pro_audio;
    if (!c->os_processing) {
        cfg.aaudio.inputPreset = ma_aaudio_input_preset_unprocessed;
        cfg.opensl.recordingPreset = ma_opensl_recording_preset_voice_unprocessed;
        cfg.wasapi.rawCapture = MA_TRUE;   /* miniaudio patch (PATCHES.md) */
    }
    mr = ma_device_init(&e->context, &cfg, &c->device);
    if (mr != MA_SUCCESS) return mr == MA_ACCESS_DENIED ? FA_ERROR_PERMISSION_DENIED : FA_ERROR_DEVICE;
    c->device_ok = 1;
    c->channels = c->device.capture.channels;
    c->rate = c->device.sampleRate;
    fa_store64(&c->input_ns, c->device.capture.internalSampleRate == 0 ? 0 :
               (int64_t)((double)c->device.capture.internalPeriodSizeInFrames *
                         (double)c->device.capture.internalPeriods * 1e9 /
                         (double)c->device.capture.internalSampleRate));
    /* The master callback never allocates: the bridge is sized for it now. */
    return fa_bridge_size(&c->br, c->rate, m->device->rate, RING_SECONDS);
}

static fa_result capture_open(struct fa_mixer* m, const char* id, uint32_t mic_flags, fa_capture** out)
{
    fa_capture* c;
    fa_result r;
    int i;
    size_t len = id ? strlen(id) : 0;
    *out = NULL;
    c = (fa_capture*)calloc(1, sizeof(*c));
    if (!c) return FA_ERROR_OUT_OF_MEMORY;
    c->mixer = m;
    c->os_processing = (mic_flags & FA_MIC_OS_PROCESSING) != 0;
    c->id = (char*)malloc(len + 1);
    if (!c->id || fa_bridge_init(&c->br, BRIDGE_MAX_ADJUST) != FA_OK) {
        free(c->id);
        free(c);
        return FA_ERROR_OUT_OF_MEMORY;
    }
    memcpy(c->id, id ? id : "", len + 1);
    for (i = 0; i < CAPTURE_LANES; i++) fa_store32(&c->lane_src[i], CAPTURE_LANE_FREE);
    r = capture_start(c, m->device->open_mode);
    if (r != FA_OK) {
        capture_free(c);
        return r;
    }
    *out = c;
    return FA_OK;
}

/* fa_device_reopen_mode, while the master is closed (mixer lock held): every input opens
 * again in `mode`. One that can't is lost (FA_EVENT_INPUT_LOST), as if unplugged, unless
 * the master falls back to the old mode and it opens again there (the event thread can't
 * report it meanwhile: it waits for the mixer lock). */
void fa_mixer_captures_reopen(struct fa_mixer* m, fa_latency_mode mode)
{
    fa_capture* c;
    for (c = m->captures; c; c = c->next) {
        if ((fa_load32(&c->lost) && !c->mode_lost) || c->test_owned) continue;
        fa_store32(&c->stopping, 1);
        if (c->device_ok) {
            ma_device_uninit(&c->device);   /* waits for its callback */
            c->device_ok = 0;
        }
        if (capture_start(c, mode) != FA_OK || ma_device_start(&c->device) != MA_SUCCESS) {
            if (c->device_ok) ma_device_uninit(&c->device);
            c->device_ok = 0;
            c->mode_lost = 1;
            flag_lost(c);
            continue;
        }
        if (c->mode_lost) {
            c->mode_lost = 0;
            fa_store32(&c->lost, 0);
        }
        fa_store32(&c->stopping, 0);
    }
}

static fa_capture* capture_find(struct fa_mixer* m, const char* id)
{
    fa_capture* c;
    for (c = m->captures; c; c = c->next)   /* a lost one stays silent: a new mic opens the device again */
        if (!fa_load32(&c->lost) && strcmp(c->id, id ? id : "") == 0) return c;
    return NULL;
}

/* A lane of `c` carrying `channel`: shared with a mic that reads the same one, else a free one. */
static int lane_for(fa_capture* c, int32_t channel)
{
    int i;
    for (i = 0; i < CAPTURE_LANES; i++)
        if (c->lane_users[i] > 0 && fa_load32(&c->lane_src[i]) == channel) return i;
    for (i = 0; i < CAPTURE_LANES; i++)
        if (c->lane_users[i] == 0) return i;
    return -1;
}

static void capture_unlink(struct fa_mixer* m, fa_capture* c)
{
    fa_capture** p;
    for (p = &m->captures; *p; p = &(*p)->next)
        if (*p == c) { *p = c->next; return; }
}

/* ---- mic API ---------------------------------------------------------------------- */

FA_API void FA_CALL fa_mic_config_init(fa_mic_config* config)
{
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->struct_size = (uint32_t)sizeof(*config);
    config->channel = -1;
    config->monitor = 1;
}

FA_API fa_result FA_CALL fa_mixer_add_mic(fa_mixer* mixer, const fa_mic_config* config, fa_mic** out_mic)
{
    fa_mic_config c;
    struct fa_mic* mic;
    fa_capture* cap;
    fa_result r = FA_OK;
    int slot = -1, fresh = 0, i, lane;
    if (!mixer || !out_mic) return FA_ERROR_INVALID_ARG;
    *out_mic = NULL;
    fa_mic_config_init(&c);
    if (config) {
        if (config->struct_size < sizeof(fa_mic_config)) return FA_ERROR_INVALID_ARG;
        c = *config;
    }
    if (c.channel < -1 || (c.flags & ~(FA_MIC_DUCKS_MUSIC | FA_MIC_OS_PROCESSING))) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&mixer->lock);
    for (i = 0; i < FA_MIC_MAX; i++)
        if (!fa_load_ptr(&mixer->mics[i])) { slot = i; break; }
    if (slot < 0) {
        fa_mutex_unlock(&mixer->lock);
        return FA_ERROR_BUSY;
    }
    r = fa_mic_bus_make(mixer);
    if (r != FA_OK) {
        fa_mutex_unlock(&mixer->lock);
        return r;
    }
    cap = capture_find(mixer, c.device_id);
    if (!cap) {
        r = capture_open(mixer, c.device_id, c.flags, &cap);
        fresh = 1;
    }
    if (r == FA_OK && c.channel >= 0 && (uint32_t)c.channel >= cap->channels) r = FA_ERROR_NOT_ENOUGH_CHANNELS;
    mic = r == FA_OK ? (struct fa_mic*)calloc(1, sizeof(*mic)) : NULL;
    if (r == FA_OK && !mic) r = FA_ERROR_OUT_OF_MEMORY;
    if (r != FA_OK) {
        if (fresh && cap) capture_free(cap);
        fa_mutex_unlock(&mixer->lock);
        return r;
    }
    lane = lane_for(cap, c.channel);   /* two mics per mixer, two lanes per input: always one */
    mic->mixer = mixer;
    mic->index = (uint32_t)slot;
    mic->cap = cap;
    mic->lane = lane;
    mic->flags = c.flags;
    fa_lane_init(&mic->lanes[MIC_GAIN], 0.0f);
    fa_lane_init(&mic->lanes[MIC_MUTE], 0.0f);
    fa_lane_init(&mic->lanes[MIC_PAN], 0.0f);
    fa_lane_init(&mic->lanes[MIC_MONITOR], c.monitor ? 1.0f : 0.0f);
    fa_lane_init(&mic->lanes[MIC_LOW_CUT], 1.0f);
    fa_lane_init(&mic->lanes[MIC_REVERB], 0.2f);
    fa_lane_init(&mic->lanes[MIC_ECHO], 0.0f);
    fa_store_float(&mic->gate_db, -50.0f);
    fa_eq_shared_init(&mic->eq);
    fa_eq_reset(&mic->eq_st);
    mic->hp = fa_mixer_design(1, MIC_LOW_CUT_HZ, mixer->device->rate);
    mic->warm = (int64_t)mixer->device->rate * MIC_WARM_MS / 1000;
    if (cap->lane_users[lane]++ == 0) fa_store32(&cap->lane_src[lane], c.channel);
    cap->users++;
    if (fresh) {
        cap->next = mixer->captures;
        mixer->captures = cap;
        if (ma_device_start(&cap->device) != MA_SUCCESS) {
            mixer->captures = cap->next;
            capture_free(cap);
            free(mic);
            fa_mutex_unlock(&mixer->lock);
            return FA_ERROR_DEVICE;
        }
    }
    fa_exchange_ptr(&mixer->mics[slot], mic);
    fa_mutex_unlock(&mixer->lock);
    *out_mic = mic;
    return FA_OK;
}

/* Takes the mic out of its slot and closes its input if it was the last on it.
 * Caller holds the mixer lock. Returns 0 when it was out already (fa_mixer_destroy and
 * a callback's fa_mic_remove raced): whoever took it out frees it. */
static int mic_detach_locked(struct fa_mixer* m, struct fa_mic* mic)
{
    fa_capture* c = mic->cap;
    if (!fa_cas_ptr(&m->mics[mic->index], mic, NULL)) return 0;
    fa_device_wait_quiescent(m->device);   /* the master callback is past it */
    if (--c->lane_users[mic->lane] == 0) fa_store32(&c->lane_src[mic->lane], CAPTURE_LANE_FREE);
    if (--c->users == 0) {
        capture_unlink(m, c);
        capture_free(c);
    }
    return 1;
}

static void mic_free(struct fa_mic* mic)
{
    fa_tap_free((fa_tap*)fa_load_ptr(&mic->tap));
    free(mic);
}

FA_API void FA_CALL fa_mic_remove(fa_mic* mic)
{
    struct fa_mixer* m;
    int mine;
    if (!mic) return;
    m = mic->mixer;
    fa_mutex_lock(&m->lock);
    mine = mic_detach_locked(m, mic);
    fa_mutex_unlock(&m->lock);
    if (!mine) return;            /* fa_mixer_destroy took it out and frees it */
    fa_mixer_wait_callbacks(m);   /* no event callback still has it */
    mic_free(mic);
}

void fa_mixer_mics_free(struct fa_mixer* m)
{
    uint32_t i;
    fa_mutex_lock(&m->lock);
    for (i = 0; i < FA_MIC_MAX; i++) {
        struct fa_mic* mic = (struct fa_mic*)fa_load_ptr(&m->mics[i]);
        /* Freed with the mixer, after the event thread is done with it. */
        if (mic && mic_detach_locked(m, mic)) m->removed[i] = mic;
    }
    fa_mutex_unlock(&m->lock);
}

void fa_mixer_mics_free_removed(struct fa_mixer* m)
{
    uint32_t i;
    for (i = 0; i < FA_MIC_MAX; i++) {
        if (m->removed[i]) mic_free(m->removed[i]);
        m->removed[i] = NULL;
    }
}

FA_API fa_result FA_CALL fa_mic_get_index(fa_mic* mic, uint32_t* out_index)
{
    if (!mic || !out_index) return FA_ERROR_INVALID_ARG;
    *out_index = mic->index;
    return FA_OK;
}

static int lane_of(fa_mic_param p)
{
    switch (p) {
    case FA_MIC_GAIN_DB: return MIC_GAIN;
    case FA_MIC_MUTE: return MIC_MUTE;
    case FA_MIC_PAN: return MIC_PAN;
    case FA_MIC_MONITOR: return MIC_MONITOR;
    case FA_MIC_LOW_CUT: return MIC_LOW_CUT;
    case FA_MIC_REVERB_SEND: return MIC_REVERB;
    case FA_MIC_ECHO_SEND: return MIC_ECHO;
    default: return -1;
    }
}

FA_API fa_result FA_CALL fa_mic_set_param(fa_mic* mic, fa_mic_param param, float value)
{
    int lane;
    if (!mic || value != value) return FA_ERROR_INVALID_ARG;
    if (param == FA_MIC_GATE_DB) {
        fa_store_float(&mic->gate_db, clampf(value, -100.0f, 0.0f));
        return FA_OK;
    }
    lane = lane_of(param);
    if (lane < 0) return FA_ERROR_INVALID_ARG;
    switch (param) {
    case FA_MIC_GAIN_DB: value = clampf(value, -20.0f, 40.0f); break;
    case FA_MIC_PAN: value = clampf(value, -1.0f, 1.0f); break;
    case FA_MIC_REVERB_SEND:
    case FA_MIC_ECHO_SEND: value = clampf(value, 0.0f, 1.0f); break;
    default: value = value >= 0.5f ? 1.0f : 0.0f; break;   /* switches */
    }
    fa_mutex_lock(&mic->mixer->lock);
    fa_lane_post(&mic->lanes[lane], value, MIXER_DEZIPPER_NS, NULL, 0, 0, -1, 0, NULL);
    fa_mutex_unlock(&mic->mixer->lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_mic_get_param(fa_mic* mic, fa_mic_param param, float* out_value)
{
    int lane;
    if (!mic || !out_value) return FA_ERROR_INVALID_ARG;
    if (param == FA_MIC_GATE_DB) {
        *out_value = fa_load_float(&mic->gate_db);
        return FA_OK;
    }
    lane = lane_of(param);
    if (lane < 0) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&mic->mixer->lock);
    *out_value = fa_lane_get(&mic->lanes[lane]);
    fa_mutex_unlock(&mic->mixer->lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_mic_set_eq(fa_mic* mic, const fa_eq_band* bands, uint32_t count)
{
    if (!mic || fa_eq_check_bands(bands, count) != FA_OK) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&mic->mixer->lock);
    fa_eq_shared_set(&mic->eq, bands, count);
    fa_mutex_unlock(&mic->mixer->lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_mic_get_eq(fa_mic* mic, fa_eq_band* out_bands, uint32_t capacity, uint32_t* out_count)
{
    if (!mic || !out_count || (capacity > 0 && !out_bands)) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&mic->mixer->lock);
    *out_count = fa_eq_shared_get(&mic->eq, out_bands, capacity);
    fa_mutex_unlock(&mic->mixer->lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_mic_get_levels(fa_mic* mic, fa_time window, float* out_peak, float* out_rms)
{
    fa_result r;
    if (!mic) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&mic->mixer->lock);   /* serializes fa_tap_fit with other readers */
    r = fa_tap_levels(&mic->tap, mic->mixer->device, window, out_peak, out_rms, 1);
    fa_mutex_unlock(&mic->mixer->lock);
    return r;
}

FA_API fa_result FA_CALL fa_mixer_get_latency(fa_mixer* mixer, fa_mixer_latency* out_latency)
{
    fa_mixer_latency l;
    uint32_t i, size;
    if (!mixer || !out_latency || out_latency->struct_size < sizeof(uint32_t)) return FA_ERROR_INVALID_ARG;
    size = out_latency->struct_size < sizeof(l) ? out_latency->struct_size : (uint32_t)sizeof(l);
    memset(&l, 0, sizeof(l));
    l.struct_size = size;
    /* The OS's buffer and the limiter's look-ahead (always on for a mixer). */
    /* The OS's buffer, the limiter's look-ahead (always on for a mixer) and a cue alignment delay. */
    l.master = fa_load64(&mixer->device->latency_ns) + fa_device_align_ns(mixer->device) +
               (fa_time)((double)mixer->device->limiter.delay * 1e9 / (double)mixer->device->rate);
    l.cue = fa_load64(&mixer->device->latency_ns);   /* the cue is not limited: no look-ahead */
    fa_mixer_cue_latency(mixer, &l.cue, &l.cue_alignment, &l.cue_align);
    fa_mutex_lock(&mixer->lock);
    for (i = 0; i < FA_MIC_MAX; i++) {
        struct fa_mic* mic = (struct fa_mic*)fa_load_ptr(&mixer->mics[i]);
        if (!mic) {
            l.mic_input[i] = -1;
            l.mic_monitor[i] = -1;
            continue;
        }
        l.mic_input[i] = fa_load64(&mic->cap->input_ns) + fa_load64(&mic->cap->br.held_ns);
        /* The setup check's measurement in this mode, when there is one. */
        l.mic_monitor[i] = fa_load64(&mic->measured[mixer->device->open_mode]);
        if (l.mic_monitor[i] <= 0) l.mic_monitor[i] = l.mic_input[i] + l.master;
        else l.mic_monitor[i] += fa_device_align_ns(mixer->device);   /* measured without it */
    }
    fa_mutex_unlock(&mixer->lock);
    memcpy(out_latency, &l, size);
    return FA_OK;
}

/* ---- events ----------------------------------------------------------------------- */

FA_API fa_result FA_CALL fa_mixer_set_event_callback(fa_mixer* mixer, fa_mixer_event_callback callback, void* user)
{
    if (!mixer) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&mixer->cb_lock);
    mixer->callback = callback;
    mixer->callback_user = user;
    fa_mutex_unlock(&mixer->cb_lock);
    fa_mixer_wait_callbacks(mixer);   /* the old callback is done */
    return FA_OK;
}

void fa_mixer_service(struct fa_mixer* m)
{
    fa_mixer_event ev[FA_MIC_MAX + 1];
    uint32_t n = 0, i;
    fa_mutex_lock(&m->lock);
    if (fa_mixer_cue_take_loss(m)) {
        memset(&ev[n], 0, sizeof(ev[n]));
        ev[n].type = FA_EVENT_CUE_LOST;
        ev[n].mixer = m;
        n++;
    }
    for (i = 0; i < FA_MIC_MAX; i++) {
        struct fa_mic* mic = (struct fa_mic*)fa_load_ptr(&m->mics[i]);
        if (!mic || fa_load32(&mic->cap->lost) == 0 || mic->lost_reported) continue;
        mic->lost_reported = 1;
        memset(&ev[n], 0, sizeof(ev[n]));
        ev[n].type = FA_EVENT_INPUT_LOST;
        ev[n].mixer = m;
        ev[n].index = i;
        ev[n].mic = mic;
        n++;
    }
    fa_mutex_unlock(&m->lock);
    for (i = 0; i < n && !fa_load32(&m->doomed); i++) {
        fa_mixer_event_callback cb;
        void* user;
        int still;
        fa_mutex_lock(&m->lock);   /* a callback before may have removed it */
        /* The same address may be a new mic (removed and added by a callback): only the
         * one that took the loss has lost_reported. */
        still = !ev[i].mic || (fa_load_ptr(&m->mics[ev[i].index]) == ev[i].mic && ev[i].mic->lost_reported);
        fa_mutex_unlock(&m->lock);
        if (!still) continue;
        fa_mutex_lock(&m->cb_lock);
        cb = m->callback;
        user = m->callback_user;
        fa_mutex_unlock(&m->cb_lock);
        if (cb) cb(&ev[i], user);
    }
    fa_mixer_release(m);
}

/* ---- tests ------------------------------------------------------------------------ */

void fa_mic_test_take_over(fa_mic* mic, uint32_t rate, uint32_t channels)
{
    fa_capture* c = mic->cap;
    fa_store32(&c->stopping, 1);
    if (c->device_ok) ma_device_stop(&c->device);
    c->test_owned = 1;   /* a latency mode switch leaves it alone */
    c->rate = rate;
    c->channels = channels;
    c->br.in_rate = rate;
    fa_store32(&c->br.wpos, 0);
    fa_store32(&c->br.rpos, 0);
    c->br.out_rate = 0;   /* set up again on the next pull */
}

void fa_mic_test_feed(fa_mic* mic, const float* frames, uint32_t n)
{
    capture_write(mic->cap, frames, n, mic->cap->channels);
}

void fa_mic_test_lose(fa_mic* mic)
{
    flag_lost(mic->cap);
}

void fa_mic_test_stats(fa_mic* mic, double* out_target, double* out_fill, int32_t* out_underruns)
{
    fa_capture* c = mic->cap;
    if (out_target) *out_target = c->br.target;
    if (out_fill) *out_fill = fa_bridge_fill(&c->br);
    if (out_underruns) *out_underruns = fa_load32(&c->br.underruns);
}
