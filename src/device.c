#include "encode.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Device ids: hex of the backend id bytes (trailing zeros trimmed). If that
 * does not fit the 256-byte field, fall back to "#<index>" (not stable). */
static void encode_id(const ma_device_id* id, uint32_t index, char* out, size_t cap)
{
    const unsigned char* b = (const unsigned char*)id;
    size_t n = sizeof(ma_device_id);
    size_t i;
    while (n > 0 && b[n - 1] == 0) n--;
    if (n * 2 + 1 > cap) { snprintf(out, cap, "#%u", (unsigned)index); return; }
    for (i = 0; i < n; i++) snprintf(out + i * 2, 3, "%02x", b[i]);
    out[n * 2] = '\0';
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Lists devices into pointers valid until the next enumeration; caller holds engine->lock. */
static ma_result list_devices(fa_engine* e, fa_device_type type, ma_device_info** infos, ma_uint32* count)
{
    ma_device_info* pb; ma_uint32 pbn; ma_device_info* cp; ma_uint32 cpn;
    ma_result r = ma_context_get_devices(&e->context, &pb, &pbn, &cp, &cpn);
    if (r != MA_SUCCESS) return r;
    if (type == FA_DEVICE_CAPTURE) { *infos = cp; *count = cpn; } else { *infos = pb; *count = pbn; }
    return MA_SUCCESS;
}

FA_API fa_result FA_CALL fa_device_count(fa_engine* engine, fa_device_type type, uint32_t* out_count)
{
    ma_device_info* infos; ma_uint32 n; ma_result r;
    if (!engine || !out_count || (type != FA_DEVICE_PLAYBACK && type != FA_DEVICE_CAPTURE)) return FA_ERROR_INVALID_ARG;
    ma_mutex_lock(&engine->lock);
    r = list_devices(engine, type, &infos, &n);
    ma_mutex_unlock(&engine->lock);
    if (r != MA_SUCCESS) return FA_ERROR_DEVICE;
    *out_count = n;
    return FA_OK;
}

FA_API fa_result FA_CALL fa_device_get_info(fa_engine* engine, fa_device_type type, uint32_t index, fa_device_info* out_info)
{
    ma_device_info* infos; ma_uint32 n; ma_result r;
    ma_device_info detail;
    if (!engine || !out_info || (type != FA_DEVICE_PLAYBACK && type != FA_DEVICE_CAPTURE)) return FA_ERROR_INVALID_ARG;
    memset(out_info, 0, sizeof(*out_info));
    ma_mutex_lock(&engine->lock);
    r = list_devices(engine, type, &infos, &n);
    if (r != MA_SUCCESS) { ma_mutex_unlock(&engine->lock); return FA_ERROR_DEVICE; }
    if (index >= n) { ma_mutex_unlock(&engine->lock); return FA_ERROR_NOT_FOUND; }
    encode_id(&infos[index].id, index, out_info->id, sizeof(out_info->id));
    snprintf(out_info->name, sizeof(out_info->name), "%s", infos[index].name);
    out_info->type = type;
    out_info->is_default = infos[index].isDefault ? 1 : 0;
    /* Native format needs a detailed query; best effort. */
    if (ma_context_get_device_info(&engine->context,
                                   type == FA_DEVICE_CAPTURE ? ma_device_type_capture : ma_device_type_playback,
                                   &infos[index].id, &detail) == MA_SUCCESS && detail.nativeDataFormatCount > 0) {
        out_info->native_sample_rate = detail.nativeDataFormats[0].sampleRate;
        out_info->native_channels = detail.nativeDataFormats[0].channels;
    }
    ma_mutex_unlock(&engine->lock);
    return FA_OK;
}

/* ---- The audio callback ------------------------------------------------------
 * Lock-free: it reads an immutable voice list published through an atomic
 * pointer and mixes each stream's pre-decoded ring buffer. `epoch` is odd while
 * it runs, which lets API threads wait until it no longer sees an old list
 * (fa_device_wait_quiescent) before freeing anything it might touch. */
static int limiter_on(fa_engine* e)
{
    int mode = fa_load32(&e->limiter_mode);
    return mode == FA_LIMITER_ON || (mode == FA_LIMITER_AUTO && fa_load32(&e->norm_mode) != FA_NORMALIZE_OFF);
}

/* Publishes when this callback started, for the presentation clock. Callbacks
 * arrive with a few ms of scheduling jitter while the device consumes frames at an
 * exact rate, so the time is smoothed: predicted from the previous callback and
 * pulled 1/16 of the way toward the measured time (a jump over 20 ms resets it). */
static void publish_callback_time(fa_device* dev, int64_t frame, int64_t now_ns)
{
    int64_t t = now_ns;
    if (dev->cb_prev_time && dev->rate) {
        int64_t expected = dev->cb_prev_time + (int64_t)((double)dev->cb_prev_frames * 1e9 / (double)dev->rate);
        int64_t err = now_ns - expected;
        if (err > -20000000 && err < 20000000) t = expected + err / 16;
    }
    fa_fetch_add64(&dev->cb_seq, 1);
    fa_store64(&dev->cb_frame, frame);
    fa_store64(&dev->cb_time, t);
    fa_fetch_add64(&dev->cb_seq, 1);
    dev->cb_prev_time = t;
}

void fa_device_render(fa_device* dev, float* out, uint32_t frames)
{
    fa_device_render_at(dev, out, frames, fa_monotonic_ns());
}

static void render_block(fa_device* dev, float* out, uint32_t frames, int64_t now_ns)
{
    fa_voice_list* list;
    struct fa_mixer* mixer = (struct fa_mixer*)fa_load_ptr(&dev->mixer);
    uint32_t i;
    publish_callback_time(dev, fa_load64(&dev->clock), now_ns);
    dev->cb_prev_frames = frames;
    dev->xf_busy = !fa_mutex_trylock(&dev->xf_lock);   /* never waits */
    memset(out, 0, (size_t)frames * FA_DEVICE_CHANNELS * sizeof(float));
    fa_buses_begin(dev);
    list = (fa_voice_list*)fa_load_ptr(&dev->voices);
    if (list) {
        /* Queue transitions first, so a stream that starts this callback starts on
         * its exact frame whatever its place in the list. */
        uint32_t pass;
        int changed = 1;
        /* Repeated while transitions are found: a stream started by one may reach
         * its own mix point in this callback (a chain of very short streams). */
        for (pass = 0; changed && pass <= list->count; pass++) {
            changed = 0;
            for (i = 0; i < list->count; i++) changed |= fa_stream_check_queue(list->items[i], dev, frames);
        }
        for (i = 0; i < list->count; i++) {
            /* A mixer deck's streams go to the deck's buffer, the rest to the master. */
            fa_bus* bus = mixer ? (fa_bus*)fa_load_ptr(&list->items[i]->bus) : NULL;
            fa_stream_mix(list->items[i], dev, bus && bus->buf ? bus->buf : out, frames);
        }
    }
    if (mixer) fa_mixer_process(mixer, dev, out, frames);
    {
        /* The output moved (fa_devices_service, or the system rerouted it): every
         * stream on it hears about it, as its audio is mixed. */
        int32_t changed = fa_load32(&dev->changed) ? fa_exchange32(&dev->changed, 0) : 0;
        if (changed && list)
            for (i = 0; i < list->count; i++)
                fa_audio_post_event(dev, list->items[i], FA_EVENT_DEVICE_CHANGED,
                                    fa_stream_position_now(list->items[i]), changed - 1);
    }
    if (!dev->xf_busy) fa_mutex_unlock(&dev->xf_lock);   /* commands taken */
    fa_buses_end(dev, frames);
    fa_limiter_process(&dev->limiter, out, frames, fa_db_to_lin(fa_load_float(&dev->engine->limiter_ceiling)),
                       mixer || limiter_on(dev->engine));   /* a DJ mixer's master is always limited */
    {
        /* Device mute, after the limiter: glides sample by sample, costs nothing unmuted. */
        float level = dev->mute_level, target = (float)fa_load32(&dev->muted);
        if (level != target) {
            for (i = 0; i < frames; i++) {
                level = fa_glide(level, target, 1, dev->rate);
                out[i * FA_DEVICE_CHANNELS] *= 1.0f - level;
                out[i * FA_DEVICE_CHANNELS + 1] *= 1.0f - level;
            }
            dev->mute_level = level;
        } else if (level == 1.0f) {
            memset(out, 0, (size_t)frames * FA_DEVICE_CHANNELS * sizeof(float));
        }
    }
    {
        fa_enc_list* encs = (fa_enc_list*)fa_load_ptr(&dev->encoders);
        if (encs)
            for (i = 0; i < encs->count; i++) fa_encode_push(encs->items[i], out, frames, dev->rate);
    }
    {
        fa_tap* tap = (fa_tap*)fa_load_ptr(&dev->tap);
        if (tap) fa_tap_write(tap, fa_load64(&dev->clock), out, frames);
    }
    fa_fetch_add64(&dev->clock, frames);
}

void fa_device_render_at(fa_device* dev, float* out, uint32_t frames, int64_t now_ns)
{
    uint32_t done, n;
    if (!fa_load_ptr(&dev->mixer) || frames <= FA_MIXER_BLOCK) { render_block(dev, out, frames, now_ns); return; }
    /* A mixer's buffers hold FA_MIXER_BLOCK frames: longer calls run in blocks. */
    for (done = 0; done < frames; done += n) {
        n = frames - done < FA_MIXER_BLOCK ? frames - done : FA_MIXER_BLOCK;
        render_block(dev, out + (size_t)done * FA_DEVICE_CHANNELS, n, now_ns + (int64_t)((double)done * 1e9 / dev->rate));
    }
}

void fa_device_render_output(fa_device* dev, float* out, uint32_t channels, uint32_t frames, int64_t now_ns)
{
    struct fa_mixer* m = (struct fa_mixer*)fa_load_ptr(&dev->mixer);
    if (m) fa_mixer_render(m, dev, out, channels, frames, now_ns);
    else fa_device_render_at(dev, out, frames, now_ns);
}

static void data_callback(ma_device* d, void* output, const void* input, ma_uint32 frames)
{
    fa_device* dev = (fa_device*)d->pUserData;
    (void)input;
    fa_fetch_add64(&dev->epoch, 1);
    fa_device_render_output(dev, (float*)output, d->playback.channels, frames, fa_monotonic_ns());
    fa_fetch_add64(&dev->epoch, 1);
}

/* Output latency of the open ma_device (life_lock held, or nobody else can see it yet). */
static void store_latency(fa_device* d)
{
    ma_uint32 rate = d->device.playback.internalSampleRate;
    fa_store64(&d->latency_ns, rate == 0 ? 0 :
               (int64_t)((double)d->device.playback.internalPeriodSizeInFrames *
                         (double)d->device.playback.internalPeriods * 1e9 / (double)rate));
    fa_store32(&d->period_frames, (int32_t)d->device.playback.internalPeriodSizeInFrames);
}

/* miniaudio's thread (or the audio thread): only flags here, the event thread acts. */
static void notification_callback(const ma_device_notification* n)
{
    fa_device* dev = (fa_device*)n->pDevice->pUserData;
    if (!dev || fa_load32(&dev->reopening) || fa_load32(&dev->closing)) return;
    if (n->type == ma_device_notification_type_rerouted) {
        /* The system moved a default-device stream to the new default by itself (after
         * stopping it when the old one went away: that was no loss then). */
        fa_store32(&dev->lost, 0);
        fa_store32(&dev->rerouted, 1);
        fa_store32(&dev->changed, 1);
        fa_cond_broadcast(&dev->engine->event_cond);
    } else if (n->type == ma_device_notification_type_stopped && fa_load32(&dev->want_started)) {
        /* Stopped without fa_device_stop: unplugged or disabled. */
        fa_store64(&dev->lost_at, fa_monotonic_ns());
        fa_store32(&dev->lost, 1);
        fa_cond_broadcast(&dev->engine->event_cond);
    }
}

static ma_device_config device_config(fa_device* d, const ma_device_id* pid, uint32_t rate)
{
    fa_engine* engine = d->engine;
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.pDeviceID = pid;
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = d->open_channels;   /* 0 = the device's own (a mixer's cue on channels 3-4) */
    cfg.sampleRate = rate; /* 0 = native */
    cfg.periodSizeInMilliseconds = engine->config.period_ms;
    cfg.dataCallback = data_callback;
    cfg.notificationCallback = notification_callback;
    cfg.pUserData = d;
    cfg.noPreSilencedOutputBuffer = MA_TRUE;  /* the callback clears the buffer itself */
    cfg.wasapi.usage = ma_wasapi_usage_pro_audio; /* MMCSS "Pro Audio" scheduling on Windows */
    /* Shared-mode WASAPI runs the system mixer every 10 ms; two periods ahead of it
     * is enough with the callback only mixing decoded audio, and one period (10 ms)
     * less latency than miniaudio's default of three. A larger period_ms buys margin. */
    if (engine->context.backend == ma_backend_wasapi) cfg.periods = 2;
    if (d->open_mode == FA_LATENCY_EXCLUSIVE) {
        /* WASAPI exclusive (a mixer's opt-in live monitoring): the device's own format,
         * converted by miniaudio, and 3 ms periods, as measured in the mic latency spike. */
        cfg.playback.shareMode = ma_share_mode_exclusive;
        cfg.periodSizeInMilliseconds = 3;
        cfg.periods = 0;
    } else if (d->open_mode == FA_LATENCY_LOW) {
        /* The smallest period the system gives. On Windows that is the IAudioClient3
         * low-latency shared mode, which needs the device's own rate and format: the
         * first open asks for it (fa_device_open_with), a reopen keeps the mix rate the
         * streams' chains were built for. */
        if (cfg.periodSizeInMilliseconds == 0 || cfg.periodSizeInMilliseconds > 5) cfg.periodSizeInMilliseconds = 5;
        if (engine->context.backend == ma_backend_wasapi) {
            cfg.periodSizeInMilliseconds = 0;
            cfg.periodSizeInFrames = 16;
            cfg.wasapi.noAutoConvertSRC = MA_TRUE;
        }
    }
    return cfg;
}

/* Reopens a device that went away on the system default, at the same mix rate, so
 * the streams on it keep their chains, positions and clock. Caller holds life_lock. */
static void reopen_default_locked(fa_device* d, int64_t now)
{
    ma_device_config cfg;
    if (fa_load32(&d->closing)) return;
    d->open_has_id = 0;                  /* it follows the default from now on */
    cfg = device_config(d, NULL, d->rate);
    fa_store32(&d->reopening, 1);
    if (d->device_ok) {
        ma_device_uninit(&d->device);   /* waits for its callback: none runs after this */
        d->device_ok = 0;
    }
    if (ma_device_init(&d->engine->context, &cfg, &d->device) != MA_SUCCESS) {
        d->retry_at = now + 1000000000;   /* no output at all right now: try again in a second */
        fa_store32(&d->reopening, 0);
        return;
    }
    d->device_ok = 1;                    /* from now on it follows the default */
    store_latency(d);
    if (d->volume != 1.0f) ma_device_set_master_volume(&d->device, d->volume);
    fa_store32(&d->lost, 0);
    fa_store32(&d->reopening, 0);
    fa_store32(&d->changed, 2);          /* FA_EVENT_DEVICE_CHANGED, value 1 */
    if (fa_load32(&d->want_started) && ma_device_start(&d->device) != MA_SUCCESS) {
        fa_store32(&d->lost, 1);
        d->retry_at = now + 1000000000;
    }
}

fa_result fa_device_reopen_mode(fa_device* d, fa_latency_mode mode, void (*between)(void*, fa_latency_mode), void* user)
{
    ma_device_config cfg;
    fa_latency_mode old = d->open_mode;
    fa_result r = FA_OK;
    int ok;
    if (!fa_device_mode_supported(d->engine, mode)) return FA_ERROR_NOT_IMPLEMENTED;
    fa_mutex_lock(&d->life_lock);
    if (fa_load32(&d->closing)) {
        fa_mutex_unlock(&d->life_lock);
        return FA_ERROR_INVALID_ARG;
    }
    fa_store32(&d->reopening, 1);
    if (d->device_ok) {
        ma_device_uninit(&d->device);   /* waits for its callback: none runs until the reopen */
        d->device_ok = 0;
    }
    d->open_mode = mode;
    if (between) between(user, mode);
    cfg = device_config(d, d->open_has_id ? &d->open_id : NULL, d->rate);
    ok = ma_device_init(&d->engine->context, &cfg, &d->device) == MA_SUCCESS;
    if (!ok) {
        /* Not in this mode (exclusive: another app holds the device, or its format is
         * one miniaudio can't use): back to the old one. */
        r = FA_ERROR_DEVICE;
        d->open_mode = old;
        if (between) between(user, old);
        cfg = device_config(d, d->open_has_id ? &d->open_id : NULL, d->rate);
        ok = ma_device_init(&d->engine->context, &cfg, &d->device) == MA_SUCCESS;
    }
    if (ok) {
        d->device_ok = 1;
        store_latency(d);
        if (d->volume != 1.0f) ma_device_set_master_volume(&d->device, d->volume);
    }
    fa_store32(&d->reopening, 0);
    if (!ok || (fa_load32(&d->want_started) && ma_device_start(&d->device) != MA_SUCCESS)) {
        /* Gone meanwhile: the event thread reopens it on the default, as after a loss. */
        fa_store64(&d->lost_at, fa_monotonic_ns());
        fa_store32(&d->lost, 1);
        fa_cond_broadcast(&d->engine->event_cond);
    } else {
        fa_store32(&d->lost, 0);   /* open again: a loss before the switch is over */
    }
    fa_mutex_unlock(&d->life_lock);
    return r;
}

static int needs_service(fa_device* d, int64_t now)
{
    if (fa_load32(&d->rerouted)) return 1;
    return fa_load32(&d->lost) && now >= d->retry_at && now >= fa_load64(&d->lost_at) + FA_REROUTE_GRACE_NS;
}

static int tick_due(fa_device* d, int64_t now)
{
    int64_t t = fa_load64(&d->mixer_tick_at);
    return t != 0 && now >= t;
}

static int wants_service(fa_device* d, int64_t now)
{
    return needs_service(d, now) || fa_load32(&d->mixer_events) || tick_due(d, now);
}

/* Event thread (it looks every few milliseconds). */
void fa_devices_service(fa_engine* e)
{
    int64_t now = fa_monotonic_ns();
    for (;;) {
        fa_device* d;
        fa_device* pick = NULL;
        fa_mutex_lock(&e->list_lock);
        struct fa_mixer* mixer = NULL;
        for (d = e->devices; d; d = d->next)
            /* Only tried: fa_device_close holds it while it takes the device away. */
            if (wants_service(d, now) && fa_mutex_trylock(&d->life_lock)) { pick = d; break; }
        fa_mutex_unlock(&e->list_lock);
        if (!pick) return;
        {
            int64_t t = fa_load64(&pick->mixer_tick_at);
            /* A wake set meanwhile for later stays (the CAS fails, and it is not due yet). */
            int tick = t != 0 && now >= t && fa_cas64(&pick->mixer_tick_at, t, 0);
            if (fa_exchange32(&pick->mixer_events, 0) | tick) mixer = fa_mixer_hold(pick);
        }
        if (fa_exchange32(&pick->rerouted, 0) && pick->device_ok) store_latency(pick);
        if (needs_service(pick, now)) {
            if (pick->device_ok && ma_device_is_started(&pick->device)) fa_store32(&pick->lost, 0); /* rerouted meanwhile */
            else reopen_default_locked(pick, now);
            if (fa_load32(&pick->lost) && pick->retry_at <= now) pick->retry_at = now + 1000000000;
        }
        fa_mutex_unlock(&pick->life_lock);
        /* The mixer's events run without the device's lock: its callbacks may close it.
         * `pick` is not touched after this. */
        if (mixer) fa_mixer_service(mixer);
    }
}

/* Tests: what a backend does when the device goes away or the default changes. */
void fa_device_simulate(fa_device* d, int lost)
{
    ma_device_notification n;
    memset(&n, 0, sizeof(n));
    n.pDevice = &d->device;
    n.type = lost ? ma_device_notification_type_stopped : ma_device_notification_type_rerouted;
    fa_mutex_lock(&d->life_lock);           /* the backend is done with it before a reopen */
    if (lost && d->device_ok) ma_device_stop(&d->device);   /* as the backend does; want_started stays set */
    notification_callback(&n);
    fa_mutex_unlock(&d->life_lock);
}

/* Both waits yield for the first few milliseconds instead of sleeping: a callback
 * takes well under one, and Sleep(1) on Windows can take a whole timer tick (15.6 ms),
 * during which a stream the mixer skips (flushing) drops out. */
void fa_device_wait_quiescent(fa_device* dev)
{
    int64_t e = fa_load64(&dev->epoch), spin_until;
    if (!(e & 1)) return;                 /* not inside the callback: nothing to wait for */
    spin_until = fa_monotonic_ns() + 3000000;
    while (fa_load64(&dev->epoch) == e) fa_sleep_ms(fa_monotonic_ns() < spin_until ? 0 : 1);
}

/* Returns between two callbacks (or at once when none runs), so a join that makes
 * the mixer skip a stream for a moment starts right after a callback and is done long
 * before the next. Gives up after 20 ms: a callback that long has bigger trouble. */
void fa_device_wait_idle(fa_device* dev)
{
    int64_t until;
    if (!(fa_load64(&dev->epoch) & 1)) return;
    until = fa_monotonic_ns() + 20000000;
    while ((fa_load64(&dev->epoch) & 1) && fa_monotonic_ns() < until) fa_sleep_ms(0);
}

/* fa_voice_list and fa_bus_list share this layout: a count, then the pointers. The items are moved as bytes, so the
 * lists are only ever accessed as their own types. */
fa_result fa_device_publish_list(fa_device* dev, fa_atomic_ptr* slot, void* add, void* remove)
{
    const size_t head = offsetof(fa_voice_list, items), step = sizeof(void*);
    unsigned char* old = (unsigned char*)fa_load_ptr(slot);
    uint32_t n = 0, i, k = 0;
    unsigned char* out;
    if (old) memcpy(&n, old, sizeof n);
    out = (unsigned char*)malloc(sizeof(fa_voice_list) + (size_t)(n + 1) * step);
    if (!out) return FA_ERROR_OUT_OF_MEMORY;
    for (i = 0; i < n; i++) {
        void* item;
        memcpy(&item, old + head + i * step, step);
        if (item == remove || item == add) continue;
        memcpy(out + head + k++ * step, &item, step);
    }
    if (add) memcpy(out + head + k++ * step, &add, step);
    memcpy(out, &k, sizeof k);
    fa_exchange_ptr(slot, out);
    fa_device_wait_quiescent(dev);
    free(old);
    return FA_OK;
}

static fa_result publish_voices(fa_device* dev, fa_stream* add, fa_stream* remove)
{
    return fa_device_publish_list(dev, &dev->voices, add, remove);
}

fa_result fa_device_add_voice(fa_device* dev, fa_stream* s)
{
    fa_result r;
    fa_mutex_lock(&dev->lock);
    r = publish_voices(dev, s, NULL);
    fa_mutex_unlock(&dev->lock);
    return r;
}

void fa_device_remove_voice(fa_device* dev, fa_stream* s)
{
    fa_mutex_lock(&dev->lock);
    while (publish_voices(dev, NULL, s) != FA_OK) fa_sleep_ms(1); /* must not fail: retry on OOM */
    fa_mutex_unlock(&dev->lock);
}

FA_API fa_result FA_CALL fa_device_open(fa_engine* engine, const char* device_id, fa_device** out_device)
{
    return fa_device_open_with(engine, device_id, FA_DEVICE_CHANNELS, FA_LATENCY_NORMAL, out_device);
}

fa_result fa_device_parse_id(fa_engine* engine, fa_device_type type, const char* device_id, ma_device_id* out_id,
                             int* out_has)
{
    *out_has = 0;
    if (!device_id || !device_id[0]) return FA_OK;
    if (device_id[0] == '#') {
        ma_device_info* infos; ma_uint32 n; unsigned idx = (unsigned)atoi(device_id + 1);
        ma_mutex_lock(&engine->lock);
        if (list_devices(engine, type, &infos, &n) != MA_SUCCESS || idx >= n) {
            ma_mutex_unlock(&engine->lock);
            return FA_ERROR_NOT_FOUND;
        }
        *out_id = infos[idx].id;
        ma_mutex_unlock(&engine->lock);
    } else {
        size_t len = strlen(device_id), i;
        if ((len & 1) || len / 2 > sizeof(ma_device_id)) return FA_ERROR_INVALID_ARG;
        memset(out_id, 0, sizeof(*out_id));
        for (i = 0; i < len / 2; i++) {
            int hi = hexval(device_id[i * 2]), lo = hexval(device_id[i * 2 + 1]);
            if (hi < 0 || lo < 0) return FA_ERROR_INVALID_ARG;
            ((unsigned char*)out_id)[i] = (unsigned char)((hi << 4) | lo);
        }
    }
    *out_has = 1;
    return FA_OK;
}

fa_result fa_device_open_with(fa_engine* engine, const char* device_id, uint32_t channels, fa_latency_mode mode,
                              fa_device** out_device)
{
    fa_device* d;
    ma_device_config cfg;
    ma_device_id id;
    ma_device_id* pid = NULL;
    fa_result pr;
    int has_id;

    if (!engine || !out_device) return FA_ERROR_INVALID_ARG;
    *out_device = NULL;

    memset(&id, 0, sizeof(id));
    pr = fa_device_parse_id(engine, FA_DEVICE_PLAYBACK, device_id, &id, &has_id);
    if (pr != FA_OK) return pr;
    if (has_id) pid = &id;
    if (!fa_device_mode_supported(engine, mode)) return FA_ERROR_NOT_IMPLEMENTED;

    d = (fa_device*)calloc(1, sizeof(*d));
    if (!d) return FA_ERROR_OUT_OF_MEMORY;
    d->engine = engine;
    if (fa_mutex_init(&d->lock) != 0) { free(d); return FA_ERROR_GENERIC; }
    if (fa_mutex_init(&d->xf_lock) != 0) { fa_mutex_destroy(&d->lock); free(d); return FA_ERROR_GENERIC; }
    if (fa_mutex_init(&d->life_lock) != 0) {
        fa_mutex_destroy(&d->xf_lock); fa_mutex_destroy(&d->lock); free(d); return FA_ERROR_GENERIC;
    }
    d->volume = 1.0f;
    d->open_channels = channels;
    d->open_mode = mode;
    d->open_id = id;
    d->open_has_id = has_id;

    cfg = device_config(d, pid, mode == FA_LATENCY_LOW && engine->context.backend == ma_backend_wasapi
                                    ? 0 : engine->config.mix_sample_rate);
    if (ma_device_init(&engine->context, &cfg, &d->device) != MA_SUCCESS) {
        fa_mutex_destroy(&d->life_lock);
        fa_mutex_destroy(&d->xf_lock);
        fa_mutex_destroy(&d->lock);
        free(d);
        return FA_ERROR_DEVICE;
    }
    d->device_ok = 1;
    store_latency(d);
    d->rate = d->device.sampleRate;
    if (fa_limiter_init(&d->limiter, d->rate, FA_DEVICE_CHANNELS) != FA_OK) {
        ma_device_uninit(&d->device);
        fa_mutex_destroy(&d->life_lock);
        fa_mutex_destroy(&d->xf_lock);
        fa_mutex_destroy(&d->lock);
        free(d);
        return FA_ERROR_OUT_OF_MEMORY;
    }

    fa_mutex_lock(&engine->list_lock);
    d->next = engine->devices;
    engine->devices = d;
    fa_mutex_unlock(&engine->list_lock);
    *out_device = d;
    return FA_OK;
}

FA_API void FA_CALL fa_device_close(fa_device* device)
{
    fa_device** pp;
    fa_voice_list* list;
    struct fa_mixer* mixer;
    if (!device) return;
    mixer = (struct fa_mixer*)fa_load_ptr(&device->mixer);
    if (mixer) { fa_mixer_destroy_from_device(mixer); return; }   /* it closes the device */
    fa_encode_device_closing(device);
    fa_mutex_lock(&device->life_lock);     /* waits for a reopen in progress; none starts after */
    fa_store32(&device->closing, 1);
    fa_store32(&device->want_started, 0);
    if (device->device_ok) ma_device_stop(&device->device);
    fa_mutex_unlock(&device->life_lock);
    for (;;) {
        list = (fa_voice_list*)fa_load_ptr(&device->voices);
        if (!list || list->count == 0) break;
        fa_stream_detach(list->items[0]);
    }
    if (device->device_ok) ma_device_uninit(&device->device);   /* no more audio callbacks */
    /* Hand events still queued (ENDED, RAMP_DONE) to the engine before the queue goes. */
    fa_mutex_lock(&device->engine->list_lock);
    for (pp = &device->engine->devices; *pp; pp = &(*pp)->next)
        if (*pp == device) { *pp = device->next; break; }
    fa_event_rescue_device(device->engine, device);
    fa_mutex_unlock(&device->engine->list_lock);
    /* The event thread may have picked it before the unlink: wait until it lets go. */
    fa_mutex_lock(&device->life_lock);
    fa_mutex_unlock(&device->life_lock);
    fa_limiter_free(&device->limiter);
    fa_buses_free_all(device);
    free(fa_load_ptr(&device->voices));
    fa_tap_free((fa_tap*)fa_load_ptr(&device->tap));
    fa_mutex_destroy(&device->life_lock);
    fa_mutex_destroy(&device->xf_lock);
    fa_mutex_destroy(&device->lock);
    free(device);
}

/* Started means "wanted started": while a lost device waits to be reopened, it
 * starts again by itself once there is an output. */
FA_API fa_result FA_CALL fa_device_start(fa_device* device)
{
    fa_result r = FA_OK;
    if (!device) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&device->life_lock);
    fa_store32(&device->want_started, 1);
    if (device->device_ok && !fa_load32(&device->lost) && !ma_device_is_started(&device->device) &&
        ma_device_start(&device->device) != MA_SUCCESS) {
        fa_store32(&device->want_started, 0);
        fa_store32(&device->lost, 0);       /* a stop notification from the failed start is no loss */
        r = FA_ERROR_DEVICE;
    }
    fa_mutex_unlock(&device->life_lock);
    return r;
}

FA_API fa_result FA_CALL fa_device_stop(fa_device* device)
{
    fa_result r = FA_OK;
    if (!device) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&device->life_lock);
    fa_store32(&device->want_started, 0);
    if (device->device_ok && ma_device_is_started(&device->device) && ma_device_stop(&device->device) != MA_SUCCESS)
        r = FA_ERROR_DEVICE;
    fa_mutex_unlock(&device->life_lock);
    return r;
}

FA_API fa_result FA_CALL fa_device_is_started(fa_device* device, int32_t* out_started)
{
    if (!device || !out_started) return FA_ERROR_INVALID_ARG;
    *out_started = fa_load32(&device->want_started);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_device_set_volume(fa_device* device, float volume)
{
    fa_result r = FA_OK;
    if (!device || volume < 0.0f) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&device->life_lock);
    /* Kept while no output is open, and set again on a reopened one. */
    if (device->device_ok && ma_device_set_master_volume(&device->device, volume) != MA_SUCCESS) r = FA_ERROR_DEVICE;
    else device->volume = volume;
    fa_mutex_unlock(&device->life_lock);
    return r;
}

FA_API fa_result FA_CALL fa_device_get_volume(fa_device* device, float* out_volume)
{
    if (!device || !out_volume) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&device->life_lock);
    *out_volume = device->volume;
    fa_mutex_unlock(&device->life_lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_device_set_mute(fa_device* device, int32_t muted)
{
    if (!device) return FA_ERROR_INVALID_ARG;
    fa_store32(&device->muted, muted ? 1 : 0);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_device_get_mute(fa_device* device, int32_t* out_muted)
{
    if (!device || !out_muted) return FA_ERROR_INVALID_ARG;
    *out_muted = fa_load32(&device->muted);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_device_get_latency(fa_device* device, fa_time* out_latency)
{
    if (!device || !out_latency) return FA_ERROR_INVALID_ARG;
    *out_latency = fa_load64(&device->latency_ns);   /* the clock reads it: no lock */
    *out_latency += fa_device_align_ns(device);       /* a mixer's delay to line up its cue */
    if (*out_latency == 0) return FA_OK;
    if (device->rate) *out_latency += (fa_time)((double)device->limiter.delay * 1e9 / (double)device->rate);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_device_get_time(fa_device* device, fa_time* out_time)
{
    if (!device || !out_time) return FA_ERROR_INVALID_ARG;
    *out_time = (fa_time)((double)fa_load64(&device->clock) * 1e9 / (double)device->rate + 0.5);
    return FA_OK;
}

double fa_device_speaker_frame(fa_device* dev, int64_t now_ns)
{
    int64_t s1, s2, frame, t;
    fa_time latency = 0;
    double elapsed;
    do {
        s1 = fa_load64(&dev->cb_seq);
        frame = fa_load64(&dev->cb_frame);
        t = fa_load64(&dev->cb_time);
        s2 = fa_load64(&dev->cb_seq);
    } while ((s1 & 1) || s1 != s2);
    if (s1 == 0) return -1.0;             /* no callback yet: nothing heard */
    fa_device_get_latency(dev, &latency);
    elapsed = (double)(now_ns - t);   /* may be slightly negative: the smoothed time can run ahead */
    return (double)frame + (elapsed - (double)latency) * (double)dev->rate / 1e9;
}
