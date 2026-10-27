/* The DJ mixer's headphones on a second output device (FA_CUE_DEVICE).
 *
 * The master callback lays the cue out (the cue sends and the master's share, as on
 * channels 3-4) and writes it into a bridge (bridge.c); the headphones' callback reads
 * it at their own rate through the varispeed, nudged by at most 0.05% (about 1 cent),
 * so the drift between the two crystals never builds up and their rates may differ.
 *
 * The two outputs are then lined up from their reported latencies: the master's (its
 * buffer and the limiter's look-ahead) against the headphones' (their buffer and the
 * bridge), plus the app's trim. A faster cue is delayed in the bridge itself (it holds
 * that much more); a faster master goes through a delay line after the limiter, whose
 * length is part of the device's latency, so every heard position follows it. While a
 * mic is monitored the master is never delayed (the singer would hear themselves late).
 * The master callback decides the alignment each block from atomics; nothing here locks
 * or allocates on an audio thread. */
#include "mixer.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__)
#  include <TargetConditionals.h>
#endif

#define CUE_MAX_ADJUST 0.0005    /* the most the ratio is nudged: 0.05%, about 1 cent */
#define CUE_RING_SECONDS 1.0     /* the bridge, plus the most alignment */
#define ALIGN_MAX_MS 500         /* the most either output is delayed */
#define ALIGN_STEP_MS 1.0        /* the outputs count as lined up within this; smaller changes wait */
#define DELAY_FADE_MS 5.0        /* the master's delay changes through a fade out and in */

/* ---- the headphones' callback ----------------------------------------------------- */

static void cue_read(fa_cue_dev* q, float* out, uint32_t frames)
{
    fa_device* dev = q->mixer->device;
    uint32_t done, n, rate = (uint32_t)fa_load32(&q->rate);
    double in_period = (double)fa_load32(&dev->period_frames), out_period = (double)fa_load32(&q->period);
    for (done = 0; done < frames; done += n) {   /* the bridge reads at most a mixer block */
        n = frames - done < FA_MIXER_BLOCK ? frames - done : FA_MIXER_BLOCK;
        fa_bridge_pull(&q->br, out + (size_t)done * FA_DEVICE_CHANNELS, n, rate, in_period, out_period, 0);
    }
}

static void cue_callback(ma_device* d, void* out, const void* in, ma_uint32 frames)
{
    fa_cue_dev* q = (fa_cue_dev*)d->pUserData;
    (void)in;
    if (q) cue_read(q, (float*)out, frames);
    else memset(out, 0, (size_t)frames * FA_DEVICE_CHANNELS * sizeof(float));
}

static void cue_flag_lost(fa_cue_dev* q)
{
    fa_device* dev = q->mixer->device;
    fa_store32(&q->lost, 1);
    fa_store32(&dev->mixer_events, 1);
    fa_cond_broadcast(&dev->engine->event_cond);
}

/* The backend's thread: headphones that stop without being asked to have gone away.
 * Default-output headphones the backend moved to the new default (after stopping them)
 * play on: not lost any more, and the event thread forgets the report (take_loss). */
static void cue_notification(const ma_device_notification* n)
{
    fa_cue_dev* q = (fa_cue_dev*)n->pDevice->pUserData;
    if (!q || fa_load32(&q->stopping)) return;
    if (n->type == ma_device_notification_type_stopped) {
        cue_flag_lost(q);
    } else if (n->type == ma_device_notification_type_rerouted) {
        fa_device* dev = q->mixer->device;
        fa_store32(&q->lost, 0);
        fa_store32(&dev->mixer_events, 1);
        fa_cond_broadcast(&dev->engine->event_cond);
    }
}

/* ---- opening and closing (mixer lock, or the mixer not published yet) -------------- */

static void cue_close(fa_cue_dev* q)
{
    fa_store32(&q->running, 0);
    fa_store32(&q->stopping, 1);
    if (q->device_ok) ma_device_uninit(&q->device);   /* waits for its callback */
    q->device_ok = 0;
    q->test_owned = 0;
}

/* Opens and starts q->id in the period of `mode` (exclusive mode keeps the headphones
 * shared, with the low-latency period: they are not what the singer hears). */
static fa_result cue_open(fa_cue_dev* q, fa_latency_mode mode)
{
    struct fa_mixer* m = q->mixer;
    fa_engine* e = m->engine;
    ma_device_config cfg;
    ma_device_id did;
    uint32_t rate, period, irate;
    int has_id;
    fa_result r = fa_device_parse_id(e, FA_DEVICE_PLAYBACK, q->id, &did, &has_id);
    if (r != FA_OK) return r;
    cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.pDeviceID = has_id ? &did : NULL;
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = FA_DEVICE_CHANNELS;
    cfg.sampleRate = 0;                  /* their own rate: the bridge converts, not the OS */
    cfg.periodSizeInMilliseconds = mode == FA_LATENCY_NORMAL ? e->config.period_ms : 5;
    if (e->context.backend == ma_backend_wasapi) {   /* as the master (device.c) */
        cfg.periods = 2;
        if (mode != FA_LATENCY_NORMAL) {
            cfg.periodSizeInMilliseconds = 0;
            cfg.periodSizeInFrames = 16;
            cfg.wasapi.noAutoConvertSRC = MA_TRUE;
        }
    }
    cfg.dataCallback = cue_callback;
    cfg.notificationCallback = cue_notification;
    cfg.pUserData = q;
    cfg.noPreSilencedOutputBuffer = MA_TRUE;   /* the callback fills it all */
    cfg.wasapi.usage = ma_wasapi_usage_pro_audio;
    if (ma_device_init(&e->context, &cfg, &q->device) != MA_SUCCESS) return FA_ERROR_DEVICE;
    q->device_ok = 1;
    rate = q->device.sampleRate;
    irate = q->device.playback.internalSampleRate ? q->device.playback.internalSampleRate : rate;
    period = (uint32_t)((double)q->device.playback.internalPeriodSizeInFrames * rate / irate + 0.5);
    fa_store32(&q->rate, (int32_t)rate);
    fa_store32(&q->period, (int32_t)period);
    fa_store64(&q->os_ns, (int64_t)((double)q->device.playback.internalPeriodSizeInFrames *
                                    (double)q->device.playback.internalPeriods * 1e9 / (double)irate));
    /* No reader runs yet; the master may be writing (positions are its business): the
     * first read drops what is there and fills up anew. */
    r = fa_bridge_reserve(&q->br, rate);
    fa_store32(&q->br.reset, 1);
    fa_store64(&q->br.held_ns, 0);   /* known again once they read: the alignment waits */
    fa_store32(&q->stopping, 0);
    fa_store32(&q->lost, 0);
    q->lost_reported = 0;
    if (r == FA_OK && ma_device_start(&q->device) != MA_SUCCESS) r = FA_ERROR_DEVICE;
    if (r != FA_OK) {
        cue_close(q);
        return r;
    }
    fa_store32(&q->running, 1);
    return FA_OK;
}

static fa_result set_id(fa_cue_dev* q, const char* id)
{
    size_t len = id ? strlen(id) : 0;
    char* c = (char*)malloc(len + 1);
    if (!c) return FA_ERROR_OUT_OF_MEMORY;
    memcpy(c, id ? id : "", len + 1);
    free(q->id);
    q->id = c;
    return FA_OK;
}

/* The mode the headphones open in: the master's, but never exclusive. */
static fa_latency_mode cue_mode_for(fa_latency_mode master)
{
    return master == FA_LATENCY_EXCLUSIVE ? FA_LATENCY_LOW : master;
}

fa_result fa_mixer_cue_create(struct fa_mixer* m, const char* id)
{
    fa_device* dev = m->device;
    fa_cue_dev* q;
    fa_result r;
    uint32_t len;
#if defined(__APPLE__) && TARGET_OS_IPHONE
    (void)dev; (void)q; (void)r; (void)len; (void)id;
    return FA_ERROR_NOT_IMPLEMENTED;   /* two outputs at once need the MultiRoute session category */
#else
    q = (fa_cue_dev*)calloc(1, sizeof(*q));
    if (!q) return FA_ERROR_OUT_OF_MEMORY;
    q->mixer = m;
    r = fa_bridge_init(&q->br, CUE_MAX_ADJUST);
    if (r == FA_OK) r = set_id(q, id);
    if (r == FA_OK) r = fa_bridge_size(&q->br, dev->rate, dev->rate, CUE_RING_SECONDS);
    /* The master's delay line: the most alignment, and a block. None for the lowest
     * latency, where the master is never delayed. */
    if (!(m->flags & FA_MIXER_CUE_LOWEST_LATENCY)) {
        dev->align_max = (uint32_t)((uint64_t)dev->rate * ALIGN_MAX_MS / 1000);
        for (len = 1024; len < dev->align_max + FA_MIXER_BLOCK; len *= 2) {}
        m->mdelay.len = len;
        m->mdelay.gain = 1.0f;
        m->mdelay.buf = r == FA_OK ? (float*)calloc((size_t)len * FA_DEVICE_CHANNELS, sizeof(float)) : NULL;
        if (r == FA_OK && !m->mdelay.buf) r = FA_ERROR_OUT_OF_MEMORY;
    }
    if (r == FA_OK) r = cue_open(q, cue_mode_for(dev->open_mode));
    if (r != FA_OK) {
        fa_bridge_free(&q->br);
        free(q->id);
        free(q);
        free(m->mdelay.buf);
        m->mdelay.buf = NULL;
        dev->align_max = 0;
        return r;
    }
    if (m->flags & FA_MIXER_CUE_LOWEST_LATENCY) fa_store32(&m->align_kind, FA_CUE_ALIGN_OFF);
    m->cue_dev = q;
    return FA_OK;
#endif
}

void fa_mixer_cue_free(struct fa_mixer* m)
{
    fa_cue_dev* q = m->cue_dev;
    if (!q) return;
    cue_close(q);
    fa_bridge_free(&q->br);
    free(q->id);
    free(q);
    m->cue_dev = NULL;
    free(m->mdelay.buf);
    m->mdelay.buf = NULL;
}

void fa_mixer_cue_reopen(struct fa_mixer* m, fa_latency_mode mode)
{
    fa_cue_dev* q = m->cue_dev;
    int was_lost;
    if (!q || q->test_owned) return;
    was_lost = fa_load32(&q->lost) && q->lost_reported;
    cue_close(q);
    if (cue_open(q, cue_mode_for(mode)) != FA_OK) {
        if (was_lost) {             /* still gone: not reported again */
            fa_store32(&q->lost, 1);
            q->lost_reported = 1;
        } else {
            cue_flag_lost(q);
        }
    }
}

FA_API fa_result FA_CALL fa_mixer_set_cue_device(fa_mixer* mixer, const char* device_id)
{
    fa_cue_dev* q;
    fa_result r;
    ma_device_id did;
    int has_id;
    if (!mixer) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&mixer->lock);
    q = mixer->cue_dev;
    /* A malformed id leaves the headphones playing as they are. */
    r = q ? fa_device_parse_id(mixer->engine, FA_DEVICE_PLAYBACK, device_id, &did, &has_id) : FA_ERROR_INVALID_ARG;
    if (r != FA_OK) {
        fa_mutex_unlock(&mixer->lock);
        return r;
    }
    cue_close(q);
    r = set_id(q, device_id);
    if (r == FA_OK) r = cue_open(q, cue_mode_for(mixer->device->open_mode));
    fa_mutex_unlock(&mixer->lock);
    return r;
}

int fa_mixer_cue_take_loss(struct fa_mixer* m)
{
    fa_cue_dev* q = m->cue_dev;
    if (!q) return 0;
    if (!fa_load32(&q->lost)) {   /* back (rerouted): a later loss is reported again */
        q->lost_reported = 0;
        return 0;
    }
    if (q->lost_reported) return 0;
    q->lost_reported = 1;
    return 1;
}

void fa_mixer_cue_latency(struct fa_mixer* m, fa_time* cue, fa_time* alignment, int32_t* kind)
{
    fa_cue_dev* q = m->cue_dev;
    *alignment = 0;
    *kind = FA_CUE_ALIGN_NONE;
    if (!q) return;
    *alignment = fa_load64(&m->align_ns);
    *kind = fa_load32(&m->align_kind);
    *cue = fa_load64(&q->os_ns) + fa_load64(&q->br.held_ns) + (*kind == FA_CUE_ALIGN_CUE ? *alignment : 0);
}

/* ---- the master callback ------------------------------------------------------------ */

/* Whether any mic is heard through the master (or about to be). */
static int mics_monitored(struct fa_mixer* m)
{
    int i;
    for (i = 0; i < FA_MIC_MAX; i++) {
        struct fa_mic* mic = (struct fa_mic*)fa_load_ptr(&m->mics[i]);
        const fa_param_voice* v;
        if (!mic) continue;
        v = &mic->lanes[MIC_MONITOR].v;
        if (v->value > 0.0f || (v->active && v->target > 0.0f)) return 1;
    }
    return 0;
}

static void publish_align(struct fa_mixer* m, int64_t frames, int kind, uint32_t rate)
{
    m->align_now = kind == FA_CUE_ALIGN_MASTER ? -frames : kind == FA_CUE_ALIGN_CUE ? frames : 0;
    fa_store64(&m->align_ns, (int64_t)((double)frames * 1e9 / (double)rate));
    fa_store32(&m->align_kind, kind);
}

/* Decides which output is delayed, and by how much. */
static void align(struct fa_mixer* m, fa_device* dev)
{
    fa_cue_dev* q = m->cue_dev;
    uint32_t rate = dev->rate;
    int64_t step = (int64_t)((double)rate * ALIGN_STEP_MS / 1000.0), most = (int64_t)dev->align_max;
    int64_t held = fa_load64(&q->br.held_ns), diff_ns, v;
    int kind, monitored = mics_monitored(m);
    if (monitored && m->mdelay.want > 0) {
        /* A mic is heard: the master's delay goes at once, whatever else happens. */
        m->mdelay.want = 0;
        publish_align(m, 0, FA_CUE_ALIGN_SKIPPED, rate);
    }
    if (m->flags & FA_MIXER_CUE_LOWEST_LATENCY) {
        if (fa_load32(&m->align_kind) != FA_CUE_ALIGN_OFF) publish_align(m, 0, FA_CUE_ALIGN_OFF, rate);
        return;
    }
    /* Headphones not open, gone, or not read yet (the bridge's size is known once they
     * have been): the alignment stays as it is. */
    if (!fa_load32(&q->running) || fa_load32(&q->lost) || held == 0) return;
    diff_ns = fa_load64(&dev->latency_ns) + (int64_t)((double)dev->limiter.delay * 1e9 / (double)rate) +
              (int64_t)(fa_load_float(&m->cue_trim) * 1e6f) - (fa_load64(&q->os_ns) + held);
    v = (int64_t)((double)diff_ns * (double)rate / 1e9 + (diff_ns < 0 ? -0.5 : 0.5));
    if (v > most) v = most;
    if (v < -most) v = -most;
    if (v > -step && v < step) {
        v = 0;
        kind = FA_CUE_ALIGN_NONE;
    } else if (v > 0) {
        kind = FA_CUE_ALIGN_CUE;
    } else if (monitored) {
        v = 0;
        kind = FA_CUE_ALIGN_SKIPPED;
    } else {
        kind = FA_CUE_ALIGN_MASTER;
    }
    if (kind == fa_load32(&m->align_kind) && llabs(v - m->align_now) < step) return;
    fa_store32(&q->br.extra, (int32_t)(v > 0 ? v : 0));
    m->mdelay.want = v < 0 ? -v : 0;
    publish_align(m, v < 0 ? -v : v, kind, rate);
}

/* The master through its delay line, in place. */
static void delay_run(fa_master_delay* d, fa_device* dev, float* x, uint32_t frames)
{
    uint32_t i, mask = d->len - 1;
    float step = 1.0f / ((float)DELAY_FADE_MS * 0.001f * (float)dev->rate);
    if (d->cur == 0 && d->want == 0 && d->gain == 1.0f) {
        /* Nothing delayed: only the history is kept, for a delay that may come. */
        for (i = 0; i < frames;) {
            uint32_t at = (d->pos + i) & mask, run = d->len - at;
            if (run > frames - i) run = frames - i;
            memcpy(d->buf + (size_t)at * 2, x + (size_t)i * 2, (size_t)run * 2 * sizeof(float));
            i += run;
        }
        d->pos += frames;
        return;
    }
    for (i = 0; i < frames; i++, d->pos++) {
        float* w = d->buf + (size_t)(d->pos & mask) * 2;
        const float* y;
        w[0] = x[i * 2];
        w[1] = x[i * 2 + 1];
        if (d->cur != d->want) {
            d->gain -= step;
            if (d->gain <= 0.0f) {   /* silent: jump to the new delay, and fade in there */
                d->gain = 0.0f;
                d->cur = d->want;
                fa_store32(&dev->align_frames, (int32_t)d->cur);
            }
        } else if (d->gain < 1.0f) {
            d->gain = d->gain + step < 1.0f ? d->gain + step : 1.0f;
        }
        y = d->buf + (size_t)((d->pos - (uint32_t)d->cur) & mask) * 2;
        x[i * 2] = y[0] * d->gain;
        x[i * 2 + 1] = y[1] * d->gain;
    }
}

void fa_mixer_cue_render(struct fa_mixer* m, fa_device* dev, float* master, uint32_t frames, float mute0, float mute1)
{
    fa_cue_dev* q = m->cue_dev;
    float mute = mute0;
    uint32_t i;
    /* The cue as channels 3-4 would carry it: the sends (silenced with the device's mute,
     * gliding as the master did) and the master's share, before the master's delay. */
    for (i = 0; i < frames; i++) {
        const float* cs = m->cue + (size_t)i * 2;
        const float* mm = master + (size_t)i * 2;
        float audible;
        if (mute != mute1) mute = fa_glide(mute, mute1, 1, dev->rate);
        audible = 1.0f - mute;
        q->cue[i * 2] = clip1(cs[0] * audible + m->cue_master[i] * mm[0]);
        q->cue[i * 2 + 1] = clip1(cs[1] * audible + m->cue_master[i] * mm[1]);
    }
    fa_bridge_write(&q->br, q->cue, frames);
    align(m, dev);
    if (m->mdelay.buf) delay_run(&m->mdelay, dev, master, frames);
}

/* ---- tests -------------------------------------------------------------------------- */

void fa_mixer_test_cue_take_over(fa_mixer* m, uint32_t rate, uint32_t period, fa_time os_ns)
{
    fa_cue_dev* q = m->cue_dev;
    fa_mutex_lock(&m->lock);
    fa_store32(&q->stopping, 1);
    if (q->device_ok) ma_device_stop(&q->device);
    q->test_owned = 1;   /* a latency mode switch leaves it alone */
    fa_store32(&q->rate, (int32_t)rate);
    fa_store32(&q->period, (int32_t)period);
    fa_store64(&q->os_ns, os_ns);
    fa_bridge_reserve(&q->br, rate);
    fa_store32(&q->br.reset, 1);
    fa_store64(&q->br.held_ns, 0);
    fa_store32(&q->br.underruns, 0);   /* what the real devices did before is not the test's */
    fa_store32(&q->br.overruns, 0);
    fa_store32(&q->lost, 0);
    q->lost_reported = 0;
    fa_store32(&q->running, 1);
    fa_mutex_unlock(&m->lock);
}

void fa_mixer_test_cue_pull(fa_mixer* m, float* out, uint32_t frames)
{
    cue_read(m->cue_dev, out, frames);
}

void fa_mixer_test_cue_lose(fa_mixer* m)
{
    cue_flag_lost(m->cue_dev);
}

void fa_mixer_test_cue_stats(fa_mixer* m, double* out_target, double* out_fill, int32_t* out_underruns,
                             int32_t* out_overruns)
{
    fa_bridge* b = &m->cue_dev->br;
    if (out_target) *out_target = b->target;
    if (out_fill) *out_fill = fa_bridge_fill(b);
    if (out_underruns) *out_underruns = fa_load32(&b->underruns);
    if (out_overruns) *out_overruns = fa_load32(&b->overruns);
}
