#include "encode.h"
#include <math.h>
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

void fa_device_render_at(fa_device* dev, float* out, uint32_t frames, int64_t now_ns)
{
    fa_voice_list* list;
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
        for (i = 0; i < list->count; i++) fa_stream_mix(list->items[i], dev, out, frames);
    }
    if (!dev->xf_busy) fa_mutex_unlock(&dev->xf_lock);   /* commands taken */
    fa_buses_end(dev, frames);
    fa_limiter_process(&dev->limiter, out, frames,
                       powf(10.0f, fa_load_float(&dev->engine->limiter_ceiling) / 20.0f), limiter_on(dev->engine));
    {
        fa_enc_list* encs = (fa_enc_list*)fa_load_ptr(&dev->encoders);
        if (encs)
            for (i = 0; i < encs->count; i++) fa_encode_push(encs->items[i], out, frames, dev->rate);
    }
    fa_fetch_add64(&dev->clock, frames);
}

static void data_callback(ma_device* d, void* output, const void* input, ma_uint32 frames)
{
    fa_device* dev = (fa_device*)d->pUserData;
    (void)input;
    fa_fetch_add64(&dev->epoch, 1);
    fa_device_render(dev, (float*)output, frames);
    fa_fetch_add64(&dev->epoch, 1);
}

void fa_device_wait_quiescent(fa_device* dev)
{
    int64_t e = fa_load64(&dev->epoch);
    if (!(e & 1)) return;                 /* not inside the callback: nothing to wait for */
    while (fa_load64(&dev->epoch) == e) fa_sleep_ms(1);
}

/* Publishes a new voice list; the old one is freed once the callback is done with it. */
static fa_result publish_voices(fa_device* dev, fa_stream* add, fa_stream* remove)
{
    fa_voice_list* old = (fa_voice_list*)fa_load_ptr(&dev->voices);
    uint32_t n = old ? old->count : 0, i, k = 0;
    fa_voice_list* list = (fa_voice_list*)malloc(sizeof(fa_voice_list) + (size_t)(n + 1) * sizeof(fa_stream*));
    if (!list) return FA_ERROR_OUT_OF_MEMORY;
    for (i = 0; i < n; i++)
        if (old->items[i] != remove && old->items[i] != add) list->items[k++] = old->items[i];
    if (add) list->items[k++] = add;
    list->count = k;
    fa_exchange_ptr(&dev->voices, list);
    fa_device_wait_quiescent(dev);
    free(old);
    return FA_OK;
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
    fa_device* d;
    ma_device_config cfg;
    ma_device_id id;
    ma_device_id* pid = NULL;

    if (!engine || !out_device) return FA_ERROR_INVALID_ARG;
    *out_device = NULL;

    if (device_id && device_id[0]) {
        if (device_id[0] == '#') {
            ma_device_info* infos; ma_uint32 n; unsigned idx = (unsigned)atoi(device_id + 1);
            ma_mutex_lock(&engine->lock);
            if (list_devices(engine, FA_DEVICE_PLAYBACK, &infos, &n) != MA_SUCCESS || idx >= n) {
                ma_mutex_unlock(&engine->lock);
                return FA_ERROR_NOT_FOUND;
            }
            id = infos[idx].id;
            ma_mutex_unlock(&engine->lock);
        } else {
            size_t len = strlen(device_id), i;
            if ((len & 1) || len / 2 > sizeof(ma_device_id)) return FA_ERROR_INVALID_ARG;
            memset(&id, 0, sizeof(id));
            for (i = 0; i < len / 2; i++) {
                int hi = hexval(device_id[i * 2]), lo = hexval(device_id[i * 2 + 1]);
                if (hi < 0 || lo < 0) return FA_ERROR_INVALID_ARG;
                ((unsigned char*)&id)[i] = (unsigned char)((hi << 4) | lo);
            }
        }
        pid = &id;
    }

    d = (fa_device*)calloc(1, sizeof(*d));
    if (!d) return FA_ERROR_OUT_OF_MEMORY;
    d->engine = engine;
    if (fa_mutex_init(&d->lock) != 0) { free(d); return FA_ERROR_GENERIC; }
    if (fa_mutex_init(&d->xf_lock) != 0) { fa_mutex_destroy(&d->lock); free(d); return FA_ERROR_GENERIC; }

    cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.pDeviceID = pid;
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = FA_DEVICE_CHANNELS;
    cfg.sampleRate = engine->config.mix_sample_rate; /* 0 = native */
    cfg.periodSizeInMilliseconds = engine->config.period_ms;
    cfg.dataCallback = data_callback;
    cfg.pUserData = d;
    cfg.noPreSilencedOutputBuffer = MA_TRUE;  /* the callback clears the buffer itself */
    cfg.wasapi.usage = ma_wasapi_usage_pro_audio; /* MMCSS "Pro Audio" scheduling on Windows */
    /* Shared-mode WASAPI runs the system mixer every 10 ms; two periods ahead of it
     * is enough with the callback only mixing decoded audio, and one period (10 ms)
     * less latency than miniaudio's default of three. A larger period_ms buys margin. */
    if (engine->context.backend == ma_backend_wasapi) cfg.periods = 2;

    if (ma_device_init(&engine->context, &cfg, &d->device) != MA_SUCCESS) {
        fa_mutex_destroy(&d->xf_lock);
        fa_mutex_destroy(&d->lock);
        free(d);
        return FA_ERROR_DEVICE;
    }
    d->rate = d->device.sampleRate;
    if (fa_limiter_init(&d->limiter, d->rate, FA_DEVICE_CHANNELS) != FA_OK) {
        ma_device_uninit(&d->device);
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
    if (!device) return;
    fa_encode_device_closing(device);
    ma_device_stop(&device->device);
    for (;;) {
        list = (fa_voice_list*)fa_load_ptr(&device->voices);
        if (!list || list->count == 0) break;
        fa_stream_detach(list->items[0]);
    }
    ma_device_uninit(&device->device);       /* no more audio callbacks */
    /* Hand events still queued (ENDED, RAMP_DONE) to the engine before the queue goes. */
    fa_mutex_lock(&device->engine->list_lock);
    for (pp = &device->engine->devices; *pp; pp = &(*pp)->next)
        if (*pp == device) { *pp = device->next; break; }
    fa_event_rescue_device(device->engine, device);
    fa_mutex_unlock(&device->engine->list_lock);
    fa_limiter_free(&device->limiter);
    fa_buses_free_all(device);
    free(fa_load_ptr(&device->voices));
    fa_mutex_destroy(&device->xf_lock);
    fa_mutex_destroy(&device->lock);
    free(device);
}

FA_API fa_result FA_CALL fa_device_start(fa_device* device)
{
    if (!device) return FA_ERROR_INVALID_ARG;
    if (ma_device_is_started(&device->device)) return FA_OK;
    return ma_device_start(&device->device) == MA_SUCCESS ? FA_OK : FA_ERROR_DEVICE;
}

FA_API fa_result FA_CALL fa_device_stop(fa_device* device)
{
    if (!device) return FA_ERROR_INVALID_ARG;
    if (!ma_device_is_started(&device->device)) return FA_OK;
    return ma_device_stop(&device->device) == MA_SUCCESS ? FA_OK : FA_ERROR_DEVICE;
}

FA_API fa_result FA_CALL fa_device_is_started(fa_device* device, int32_t* out_started)
{
    if (!device || !out_started) return FA_ERROR_INVALID_ARG;
    *out_started = ma_device_is_started(&device->device) ? 1 : 0;
    return FA_OK;
}

FA_API fa_result FA_CALL fa_device_set_volume(fa_device* device, float volume)
{
    if (!device || volume < 0.0f) return FA_ERROR_INVALID_ARG;
    return ma_device_set_master_volume(&device->device, volume) == MA_SUCCESS ? FA_OK : FA_ERROR_DEVICE;
}

FA_API fa_result FA_CALL fa_device_get_volume(fa_device* device, float* out_volume)
{
    if (!device || !out_volume) return FA_ERROR_INVALID_ARG;
    return ma_device_get_master_volume(&device->device, out_volume) == MA_SUCCESS ? FA_OK : FA_ERROR_DEVICE;
}

FA_API fa_result FA_CALL fa_device_get_latency(fa_device* device, fa_time* out_latency)
{
    ma_uint32 rate;
    if (!device || !out_latency) return FA_ERROR_INVALID_ARG;
    rate = device->device.playback.internalSampleRate;
    if (rate == 0) { *out_latency = 0; return FA_OK; }
    *out_latency = (fa_time)((double)device->device.playback.internalPeriodSizeInFrames *
                             (double)device->device.playback.internalPeriods * 1e9 / (double)rate);
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
