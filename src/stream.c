/* Streams: opening, decode-ahead buffering, the per-voice mix, transport,
 * parameters and events.
 *
 * Threads that touch a stream
 * - API threads: hold api_lock for anything that reconfigures the stream.
 * - Decode workers: hold decode_lock while filling the ring (fa_stream_fill).
 * - The audio thread: fa_stream_mix only. It reads the ring and atomics, never locks.
 *
 * To change what the audio thread reads (seek, detach), an API thread sets
 * `flushing` or removes the voice, waits until the audio callback has passed
 * (fa_device_wait_quiescent), then takes decode_lock so no worker runs either. */
#include "fa_internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FA_DECODE_CHUNK 1024
#define FA_DEFAULT_BUFFER_MS 500
#define FA_PREFILL_MS 100
#define FA_DEZIPPER_NS 5000000   /* plain parameter changes glide over 5 ms */
#define FA_RAMP_BLOCK 16         /* ramp curves are evaluated every 16 frames */
#define FA_NORM_GLIDE_DOWN_DB_S 12.0f
#define FA_NORM_GLIDE_UP_DB_S 6.0f

FA_API void FA_CALL fa_open_options_init(fa_open_options* options)
{
    if (!options) return;
    memset(options, 0, sizeof(*options));
    options->struct_size = (uint32_t)sizeof(*options);
    options->audio_track = -1;
}

static int is_attached(struct fa_stream* s) { return s->device != NULL; }

static fa_frames marker(struct fa_stream* s, int m) { return fa_load64(&s->markers[m]); }

/* ---- open / close ------------------------------------------------------- */

void fa_stream_free_memory(struct fa_stream* s)
{
    fa_mutex_destroy(&s->norm_lock);
    fa_mutex_destroy(&s->cb_lock);
    fa_mutex_destroy(&s->decode_lock);
    fa_mutex_destroy(&s->api_lock);
    free(s);
}

static void release_chain(struct fa_stream* s)
{
    if (s->converter_ready) { ma_data_converter_uninit(&s->converter, NULL); s->converter_ready = 0; }
    fa_ring_free(&s->ring);
    free(s->in_buf); s->in_buf = NULL;
    free(s->out_buf); s->out_buf = NULL;
    free(s->mat_buf); s->mat_buf = NULL;
    s->in_cap = s->out_cap = s->in_count = s->in_offset = 0;
    s->decoded = 0;
    s->draining = 0;
}

/* Frees a stream that was never published to anyone (failed open). */
static void stream_discard(struct fa_stream* s)
{
    if (s->run_meter_ready) fa_meter_free(&s->run_meter);
    free(s->path);
    free(s->matrix);
    if (s->decoder_ready) fa_decoder_close(&s->decoder);
    fa_source_close(&s->source);
    fa_stream_free_memory(s);
}

static fa_result stream_create(fa_engine* engine, const fa_open_options* options, struct fa_stream** out)
{
    int i;
    struct fa_stream* s = (struct fa_stream*)calloc(1, sizeof(*s));
    if (!s) return FA_ERROR_OUT_OF_MEMORY;
    if (fa_mutex_init(&s->api_lock) != 0) { free(s); return FA_ERROR_GENERIC; }
    if (fa_mutex_init(&s->decode_lock) != 0) { fa_mutex_destroy(&s->api_lock); free(s); return FA_ERROR_GENERIC; }
    if (fa_mutex_init(&s->cb_lock) != 0) {
        fa_mutex_destroy(&s->decode_lock); fa_mutex_destroy(&s->api_lock); free(s);
        return FA_ERROR_GENERIC;
    }
    if (fa_mutex_init(&s->norm_lock) != 0) {
        fa_mutex_destroy(&s->cb_lock); fa_mutex_destroy(&s->decode_lock); fa_mutex_destroy(&s->api_lock); free(s);
        return FA_ERROR_GENERIC;
    }
    s->engine = engine;
    s->flags = options ? options->flags : 0;
    s->buffer_ms = FA_DEFAULT_BUFFER_MS;
    if (options && options->struct_size >= sizeof(fa_open_options) && options->buffer_ms) {
        s->buffer_ms = options->buffer_ms;
        if (s->buffer_ms < 50) s->buffer_ms = 50;
        if (s->buffer_ms > 10000) s->buffer_ms = 10000;
    }
    s->audio_track = options ? options->audio_track : -1;
    s->loud.integrated_lufs = s->loud.range_lu = s->loud.true_peak_dbtp = NAN;
    fa_store_float(&s->norm_track, NAN);
    fa_store_float(&s->norm_album, NAN);
    fa_store_float(&s->norm_est, NAN);
    fa_store32(&s->state, FA_STATE_STOPPED);
    for (i = 0; i < FA_MARKER_COUNT; i++) fa_store64(&s->markers[i], FA_FRAMES_UNSET);
    for (i = 0; i < FA_STREAM_LANES; i++) fa_lane_init(&s->lanes[i], i == FA_PARAM_PAN ? 0.0f : 1.0f);
    {
        fa_shape eq;
        fa_shape_from_ramp(NULL, FA_CURVE_EQUAL_POWER, &eq);
        fa_shape_store(&s->fade_shape[FA_FADE_IN], &eq);
        fa_shape_store(&s->fade_shape[FA_FADE_OUT], &eq);
        fa_shape_store(&s->xfade_shape, &eq);
    }
    *out = s;
    return FA_OK;
}

/* On failure the source is NOT closed here, so open_io can hand ownership back. */
static fa_result stream_open_decoder(struct fa_stream* s, const char* name_hint, const fa_open_options* options)
{
    fa_result r = fa_decoder_open(&s->decoder, &s->source, name_hint, options ? options->mime_hint : NULL);
    if (r != FA_OK) return r;
    s->decoder_ready = 1;
    if (options && options->audio_track > 0) return FA_ERROR_NOT_FOUND; /* single-track formats only so far */
    /* Running loudness estimate for playback; decode-only streams are never normalized. */
    if (!(s->flags & FA_OPEN_DECODE_ONLY) &&
        fa_meter_init(&s->run_meter, s->decoder.format.sample_rate, s->decoder.format.channels, 0, FA_RUNNING_SUBS) == FA_OK)
        s->run_meter_ready = 1;
    return FA_OK;
}

/* Analysis starts at open when normalization is on, so the exact gain is often
 * known before playback even starts. */
static void stream_opened(struct fa_stream* s)
{
    if (fa_load32(&s->engine->norm_mode) != FA_NORMALIZE_OFF && !(s->flags & (FA_OPEN_NO_NORMALIZE | FA_OPEN_DECODE_ONLY)))
        fa_stream_start_analysis(s);
}

FA_API fa_result FA_CALL fa_stream_open_path(fa_engine* engine, const char* path, const fa_open_options* options, fa_stream** out_stream)
{
    struct fa_stream* s; fa_result r;
    if (!engine || !path || !out_stream) return FA_ERROR_INVALID_ARG;
    *out_stream = NULL;
    r = stream_create(engine, options, &s);
    if (r != FA_OK) return r;
    r = fa_source_open_path(&s->source, path);
    if (r == FA_OK) r = stream_open_decoder(s, path, options);
    if (r == FA_OK) {
        size_t n = strlen(path) + 1;
        s->path = (char*)malloc(n);
        if (s->path) memcpy(s->path, path, n);
        else r = FA_ERROR_OUT_OF_MEMORY;
    }
    if (r != FA_OK) { stream_discard(s); return r; }
    stream_opened(s);
    *out_stream = s;
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_open_memory(fa_engine* engine, const void* data, size_t size, const fa_open_options* options, fa_stream** out_stream)
{
    struct fa_stream* s; fa_result r;
    if (!engine || !data || size == 0 || !out_stream) return FA_ERROR_INVALID_ARG;
    *out_stream = NULL;
    r = stream_create(engine, options, &s);
    if (r != FA_OK) return r;
    r = fa_source_open_memory(&s->source, data, size);
    if (r == FA_OK) r = stream_open_decoder(s, NULL, options);
    if (r != FA_OK) { stream_discard(s); return r; }
    stream_opened(s);
    *out_stream = s;
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_open_io(fa_engine* engine, const fa_io_callbacks* io, void* user, const fa_open_options* options, fa_stream** out_stream)
{
    struct fa_stream* s; fa_result r;
    if (out_stream) *out_stream = NULL;
    if (!engine || !io || !io->read || !out_stream) return FA_ERROR_INVALID_ARG;
    r = stream_create(engine, options, &s);
    if (r != FA_OK) return r;
    fa_source_open_io(&s->source, io, user);
    r = stream_open_decoder(s, NULL, options);
    if (r != FA_OK) {
        s->source.io.close = NULL; /* failure: the caller keeps ownership of `user` */
        stream_discard(s);
        return r;
    }
    stream_opened(s);
    *out_stream = s;
    return FA_OK;
}

FA_API void FA_CALL fa_stream_close(fa_stream* stream)
{
    fa_scan_job* job;
    fa_engine* e;
    uintptr_t self;
    if (!stream) return;
    e = stream->engine;
    fa_stream_detach(stream);

    /* A scan of this stream reads its file or memory: stop it before they go. */
    fa_mutex_lock(&stream->norm_lock);
    job = stream->scan_job;
    stream->scan_job = NULL;
    fa_mutex_unlock(&stream->norm_lock);
    fa_scan_release(job);

    /* Stop event delivery and wait for a callback that is running right now,
     * unless we are that callback. */
    fa_store32(&stream->closed, 1);
    self = fa_thread_current_id();
    if ((int64_t)self != fa_load64(&e->event_thread_id))
        while (fa_load32(&stream->in_callback)) fa_sleep_ms(1);

    fa_mutex_lock(&stream->decode_lock);
    release_chain(stream);
    if (stream->decoder_ready) { fa_decoder_close(&stream->decoder); stream->decoder_ready = 0; }
    fa_source_close(&stream->source); /* the app's io close, exactly once */
    if (stream->run_meter_ready) { fa_meter_free(&stream->run_meter); stream->run_meter_ready = 0; }
    free(stream->path);
    stream->path = NULL;
    free(stream->matrix);
    stream->matrix = NULL;
    fa_mutex_unlock(&stream->decode_lock);

    /* The memory goes to the event thread, which frees it after delivering (and
     * skipping) every event already queued for this stream. */
    fa_mutex_lock(&e->list_lock);
    stream->next = e->graveyard;
    e->graveyard = stream;
    fa_cond_broadcast(&e->event_cond);
    fa_mutex_unlock(&e->list_lock);
}

/* ---- info --------------------------------------------------------------- */

FA_API fa_result FA_CALL fa_stream_get_info(fa_stream* stream, fa_stream_info* out_info)
{
    const fa_decoder* d;
    if (!stream || !out_info) return FA_ERROR_INVALID_ARG;
    memset(out_info, 0, sizeof(*out_info));
    d = &stream->decoder; /* fixed after open */
    out_info->format = d->format;
    snprintf(out_info->codec, sizeof(out_info->codec), "%s", d->codec);
    snprintf(out_info->container, sizeof(out_info->container), "%s", d->container);
    out_info->length_frames = d->length >= 0 ? d->length : -1;
    out_info->duration = (d->length >= 0 && d->format.sample_rate > 0)
        ? (fa_time)((double)d->length * 1e9 / (double)d->format.sample_rate + 0.5) : -1;
    out_info->seekable = d->seekable;
    out_info->audio_track_count = 1;
    out_info->has_cdg = 0;
    return FA_OK;
}

/* ---- decode-ahead (worker threads) -------------------------------------- */

int fa_stream_needs_work(struct fa_stream* s)
{
    if (!s->ring.data || fa_load32(&s->eof) || fa_load32(&s->flushing)) return 0;
    return fa_ring_writable(&s->ring) >= s->ring.capacity / 4;
}

/* Feeds the running loudness estimate until the final value is known. */
static void update_estimate(struct fa_stream* s, const float* frames, fa_frames count)
{
    int64_t before;
    float track = fa_load_float(&s->norm_track);
    if (!s->run_meter_ready || track == track) return;
    before = s->run_meter.nsubs;
    fa_meter_process(&s->run_meter, frames, count);
    if (s->run_meter.nsubs != before) {
        double est = fa_meter_running_lufs(&s->run_meter);
        if (est == est) fa_store_float(&s->norm_est, (float)est);
    }
}

/* Where the stream ends (CUE_OUT, else the length; -1 = unknown) and where it loops back to. */
static fa_frames end_frame(struct fa_stream* s)
{
    fa_frames out = marker(s, FA_MARKER_CUE_OUT);
    if (out != FA_FRAMES_UNSET && (s->decoder.length < 0 || out < s->decoder.length)) return out;
    return s->decoder.length;
}

static fa_frames loop_start(struct fa_stream* s)
{
    fa_frames in = marker(s, FA_MARKER_CUE_IN), end = end_frame(s);
    return (in != FA_FRAMES_UNSET && (end < 0 || in < end)) ? in : 0;
}

/* The custom channel matrix: in_buf (source channels) -> mat_buf (device channels). */
static void apply_matrix(struct fa_stream* s, const float* in, fa_frames n)
{
    uint32_t ch = s->decoder.format.channels, i, o;
    fa_frames f;
    for (f = 0; f < n; f++) {
        for (o = 0; o < FA_DEVICE_CHANNELS; o++) {
            float acc = 0.0f;
            for (i = 0; i < ch; i++) acc += in[(size_t)f * ch + i] * s->matrix[i * FA_DEVICE_CHANNELS + o];
            s->mat_buf[(size_t)f * FA_DEVICE_CHANNELS + o] = acc;
        }
    }
}

/* Decodes, converts to the device format and appends to the ring until it is
 * full, `max_frames` were produced, or the source ended (at CUE_OUT, if set).
 * Caller holds decode_lock. */
void fa_stream_fill(struct fa_stream* s, int64_t max_frames)
{
    uint32_t ch = s->decoder.format.channels;
    uint32_t conv_ch = s->matrix ? FA_DEVICE_CHANNELS : ch;
    int64_t produced = 0;
    int just_looped = 0;
    if (!s->ring.data || fa_load32(&s->eof)) return;

    while (produced < max_frames) {
        ma_uint64 in_n, out_n;
        const float* conv_in;
        int64_t room = fa_ring_writable(&s->ring);
        if (room <= 0) break;
        if (s->in_count == 0) {
            fa_frames got = 0, want = s->in_cap, end = end_frame(s);
            if (s->draining) {                    /* the resampler has given back everything */
                fa_store32(&s->eof, 1);
                break;
            }
            if (end >= 0 && s->src_pos + want > end) want = end - s->src_pos;
            if (want > 0) fa_decoder_read(&s->decoder, s->in_buf, want, &got);
            s->in_offset = 0;
            s->in_count = got;
            s->decoded += got;
            s->src_pos += got;
            if (got == 0) {
                fa_frames back = loop_start(s);
                ma_uint64 latency;
                if ((s->flags & FA_OPEN_LOOP) && !just_looped && fa_decoder_seek(&s->decoder, back) == FA_OK) {
                    just_looped = 1; /* a second empty read right after looping = empty source */
                    s->src_pos = back;
                    continue;        /* the converter keeps its state: the loop joins seamlessly */
                }
                /* The resampler still holds the last few frames: push them out with
                 * silence (its input latency) before calling it the end. */
                latency = ma_data_converter_get_input_latency(&s->converter);
                if (latency > 0) {
                    if (latency > (ma_uint64)s->in_cap) latency = (ma_uint64)s->in_cap;
                    memset(s->in_buf, 0, (size_t)latency * ch * sizeof(float));
                    if (s->matrix) memset(s->mat_buf, 0, (size_t)latency * conv_ch * sizeof(float));
                    s->in_count = (fa_frames)latency;
                    s->draining = 1;
                    continue;
                }
                fa_store32(&s->eof, 1);
                break;
            }
            just_looped = 0;
            update_estimate(s, s->in_buf, got);
            if (s->matrix) apply_matrix(s, s->in_buf, got);
        }
        conv_in = s->matrix ? s->mat_buf : s->in_buf;
        in_n = (ma_uint64)s->in_count;
        out_n = (ma_uint64)(room < s->out_cap ? room : s->out_cap);
        ma_data_converter_process_pcm_frames(&s->converter, conv_in + (size_t)s->in_offset * conv_ch, &in_n,
                                             s->out_buf, &out_n);
        if (out_n > 0) fa_ring_write(&s->ring, s->out_buf, (int64_t)out_n);
        s->in_offset += (fa_frames)in_n;
        s->in_count -= (fa_frames)in_n;
        produced += (int64_t)out_n;
        if (in_n == 0 && out_n == 0) break; /* converter stalled; try again later */
    }
}

/* Source position (not wrapped) after `played` device frames since seek_base. */
static double linear_position(struct fa_stream* s, int64_t played)
{
    return (double)fa_load64(&s->seek_base) + (double)played * (double)s->decoder.format.sample_rate / (double)s->dev_rate;
}

/* Wraps a linear position into the loop (CUE_IN..CUE_OUT) of looping streams, and
 * clamps it to the end of other streams. */
static fa_frames wrap_position(struct fa_stream* s, fa_frames pos)
{
    fa_frames end = end_frame(s);
    if ((s->flags & FA_OPEN_LOOP) && end > 0 && pos >= end) {
        fa_frames back = loop_start(s), len = end - back;
        return len > 0 ? back + (pos - end) % len : back;
    }
    if (end >= 0 && pos > end) pos = end;
    return pos;
}

/* Source position from the frames the device has consumed. Exact for a fixed
 * rate ratio; the resampler's few frames of delay are not subtracted. */
static fa_frames attached_position(struct fa_stream* s)
{
    return wrap_position(s, (fa_frames)(linear_position(s, fa_load64(&s->played)) + 0.5));
}

fa_frames fa_stream_position_now(struct fa_stream* s)
{
    return s->dev_rate ? attached_position(s) : fa_load64(&s->position);
}

/* Drops buffered audio and restarts decoding at `frame`. Caller holds api_lock and
 * decode_lock, and the audio thread is kept out (flushing set, or not attached). */
static fa_result restart_at_locked(struct fa_stream* s, fa_frames frame)
{
    fa_result r = FA_OK;
    if (frame != fa_stream_position_now(s) || s->in_count > 0 || fa_ring_readable(&s->ring) > 0 || fa_load32(&s->eof))
        r = fa_decoder_seek(&s->decoder, frame);
    if (r != FA_OK) return r;
    fa_store64(&s->position, frame);
    s->src_pos = frame;
    if (s->ring.data) {
        fa_ring_reset(&s->ring);
        s->in_count = s->in_offset = 0;
        s->decoded = 0;
        s->draining = 0;
        if (s->converter_ready) ma_data_converter_reset(&s->converter);
        fa_store32(&s->eof, 0);
        fa_store64(&s->played, 0);
        fa_store64(&s->seek_base, frame);
        /* Prefill so playback resumes without waiting for a worker. */
        fa_stream_fill(s, (int64_t)s->dev_rate * FA_PREFILL_MS / 1000);
    }
    return FA_OK;
}

/* ---- attach / detach ---------------------------------------------------- */

static fa_result build_chain_locked(struct fa_stream* s, fa_device* dev)
{
    ma_data_converter_config cc;
    uint32_t ch = s->decoder.format.channels;
    int64_t capacity = (int64_t)dev->rate * s->buffer_ms / 1000;
    if (capacity < 2048) capacity = 2048;

    release_chain(s);
    cc = ma_data_converter_config_init(ma_format_f32, ma_format_f32, s->matrix ? FA_DEVICE_CHANNELS : ch,
                                       FA_DEVICE_CHANNELS, s->decoder.format.sample_rate, dev->rate);
    if (ma_data_converter_init(&cc, NULL, &s->converter) != MA_SUCCESS) return FA_ERROR_OUT_OF_MEMORY;
    s->converter_ready = 1;
    s->in_cap = FA_DECODE_CHUNK;
    s->out_cap = FA_DECODE_CHUNK * 2;
    s->in_buf = (float*)malloc((size_t)s->in_cap * ch * sizeof(float));
    s->out_buf = (float*)malloc((size_t)s->out_cap * FA_DEVICE_CHANNELS * sizeof(float));
    if (s->matrix) s->mat_buf = (float*)malloc((size_t)s->in_cap * FA_DEVICE_CHANNELS * sizeof(float));
    if (!s->in_buf || !s->out_buf || (s->matrix && !s->mat_buf) || fa_ring_init(&s->ring, FA_DEVICE_CHANNELS, capacity) != FA_OK) {
        release_chain(s);
        return FA_ERROR_OUT_OF_MEMORY;
    }
    return FA_OK;
}

/* ---- queues: references between streams ----------------------------------- */

/* A queued stream that will not start after all goes back to stopped. */
static void unqueue(struct fa_stream* n)
{
    if (n && fa_load64(&n->start_frame) == FA_START_QUEUED)
        fa_cas32(&n->state, FA_STATE_SCHEDULED, FA_STATE_STOPPED);
}

/* Drops every queue link to and from `s` on its device. The audio thread may be
 * starting one right now; the caller waits for quiescence before freeing anything.
 * Caller holds s->api_lock; takes the device lock. */
static void drop_queue_links(struct fa_stream* s)
{
    fa_device* dev = s->device;
    fa_voice_list* list;
    uint32_t i;
    unqueue((struct fa_stream*)fa_exchange_ptr(&s->queued, NULL));
    if (!dev) return;
    fa_mutex_lock(&dev->lock);
    list = (fa_voice_list*)fa_load_ptr(&dev->voices);
    if (list)
        for (i = 0; i < list->count; i++) fa_cas_ptr(&list->items[i]->queued, s, NULL);
    fa_mutex_unlock(&dev->lock);
}

/* ---- attach / detach ---------------------------------------------------- */

/* Caller holds api_lock. */
static void detach_locked(struct fa_stream* s)
{
    fa_device* dev = s->device;
    fa_frames pos;
    if (!dev) return;
    drop_queue_links(s);
    fa_device_remove_voice(dev, s);          /* waits until the audio thread let go */
    fa_engine_track_stream(s->engine, s, 0);
    fa_mutex_lock(&s->decode_lock);          /* waits for a worker still filling it */
    pos = attached_position(s);
    s->dev_rate = 0;
    s->device = NULL;
    fa_exchange_ptr(&s->bus, NULL);
    /* Continue later from what was actually heard, not from what was decoded ahead.
     * A forward-only source cannot go back: the audio buffered ahead is skipped, and
     * the position says where decoding really continues. */
    if (fa_decoder_seek(&s->decoder, pos) != FA_OK && !s->decoder.seekable)
        pos = fa_load64(&s->seek_base) + s->decoded;
    fa_store64(&s->position, pos);
    s->src_pos = pos;
    release_chain(s);
    fa_store32(&s->eof, 0);
    fa_mutex_unlock(&s->decode_lock);
    fa_cas32(&s->state, FA_STATE_PLAYING, FA_STATE_PAUSED);
    if (fa_load64(&s->start_frame) == FA_START_QUEUED) fa_cas32(&s->state, FA_STATE_SCHEDULED, FA_STATE_STOPPED);
    else fa_cas32(&s->state, FA_STATE_SCHEDULED, FA_STATE_PAUSED);
}

FA_API fa_result FA_CALL fa_stream_attach(fa_stream* stream, fa_device* device, fa_bus* bus)
{
    fa_result r;
    int was_playing;
    if (!stream || !device) return FA_ERROR_INVALID_ARG;
    if (bus && bus->device != device) return FA_ERROR_INVALID_ARG;
    if (stream->flags & FA_OPEN_DECODE_ONLY) return FA_ERROR_INVALID_ARG;
    if (device->engine != stream->engine) return FA_ERROR_INVALID_ARG; /* events and cleanup are per engine */

    fa_mutex_lock(&stream->api_lock);
    if (stream->device == device) {
        fa_exchange_ptr(&stream->bus, bus);  /* the mixer picks it up on its next block */
        fa_mutex_unlock(&stream->api_lock);
        return FA_OK;
    }
    was_playing = stream->device && fa_load32(&stream->state) == FA_STATE_PLAYING;
    if (stream->device) detach_locked(stream);  /* pauses it */

    fa_mutex_lock(&stream->decode_lock);
    r = build_chain_locked(stream, device);
    if (r == FA_OK) {
        fa_frames pos = fa_load64(&stream->position);
        int p;
        /* Ramp counters are in device frames: keep a running ramp's remaining time
         * when moving to a device with another rate. Nothing mixes this stream now. */
        for (p = 0; p < FA_STREAM_LANES; p++) {
            fa_param_voice* v = &stream->lanes[p].v;
            if (v->active && stream->voice_rate && stream->voice_rate != device->rate) {
                double k = (double)device->rate / (double)stream->voice_rate;
                v->total = (int64_t)((double)v->total * k + 0.5);
                v->done = (int64_t)((double)v->done * k + 0.5);
            }
            v->at = -1;  /* a start time on the old device's clock means nothing here */
        }
        stream->voice_rate = device->rate;
        stream->dev_rate = device->rate;
        stream->src_pos = pos;
        fa_store64(&stream->seek_base, pos);
        fa_store64(&stream->played, 0);
        fa_store32(&stream->eof, 0);
        fa_store32(&stream->flushing, 0);
        fa_stream_fill(stream, (int64_t)device->rate * FA_PREFILL_MS / 1000);
    }
    fa_mutex_unlock(&stream->decode_lock);
    if (r == FA_OK) {
        stream->device = device;
        fa_exchange_ptr(&stream->bus, bus);
        /* Track it first: from the moment the voice is live, events it spills must be found. */
        fa_engine_track_stream(stream->engine, stream, 1);
        r = fa_device_add_voice(device, stream);
        if (r == FA_OK) {
            if (was_playing) fa_cas32(&stream->state, FA_STATE_PAUSED, FA_STATE_PLAYING); /* a move keeps playing */
            stream_opened(stream);
        } else {
            fa_engine_track_stream(stream->engine, stream, 0);
            stream->device = NULL;
            fa_exchange_ptr(&stream->bus, NULL);
            fa_mutex_lock(&stream->decode_lock);
            stream->dev_rate = 0;
            release_chain(stream);
            fa_mutex_unlock(&stream->decode_lock);
        }
    }
    fa_mutex_unlock(&stream->api_lock);
    return r;
}

FA_API fa_result FA_CALL fa_stream_detach(fa_stream* stream)
{
    if (!stream) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->api_lock);
    detach_locked(stream);
    fa_mutex_unlock(&stream->api_lock);
    return FA_OK;
}

/* ---- lanes (parameters) ---------------------------------------------------- */

/* Posts a command to one of the stream's lanes. Caller holds api_lock. */
static void post_lane_locked(struct fa_stream* s, int lane, float target, int64_t ns, const fa_shape* shape,
                             int64_t tag, int notify, int64_t at, int flags)
{
    int64_t replaced_tag = 0;
    int32_t done_seq;
    int replaced;
    /* A replaced ramp that never started gets its RAMP_DONE from here. Its sequence
     * number is taken before the new command becomes visible, so it is delivered
     * before anything the new command raises. The event queues are then emptied:
     * that bounds what the mixer can raise for this stream before the next
     * collection, so its spill slots never overflow (see FA_EVENT_SPILL). */
    fa_mutex_lock(&s->engine->list_lock);
    done_seq = fa_fetch_add32(&s->engine->event_seq, 1);
    replaced = fa_lane_post(&s->lanes[lane], target, ns, shape, tag, notify, at, flags, &replaced_tag);
    fa_event_collect_locked(s->engine);
    if (replaced) {
        fa_event ev;
        ev.type = FA_EVENT_RAMP_DONE;
        ev.stream = s;
        ev.frame = fa_stream_position_now(s);
        ev.value = replaced_tag;
        fa_engine_post_event_locked(s->engine, &ev, done_seq);
    }
    fa_mutex_unlock(&s->engine->list_lock);
}

/* Device frame of a device time; caller holds api_lock and the stream is attached. */
static int64_t device_frame(struct fa_stream* s, fa_time t)
{
    return (int64_t)((double)t * (double)s->device->rate / 1e9 + 0.5);
}

/* ---- transport ----------------------------------------------------------- */

/* Caller holds api_lock. */
static fa_result seek_locked(struct fa_stream* s, fa_frames frame)
{
    fa_result r;
    if (!s->decoder.seekable) return FA_ERROR_NOT_SEEKABLE;
    if (frame < 0 || (s->decoder.length >= 0 && frame > s->decoder.length)) return FA_ERROR_INVALID_ARG;
    if (is_attached(s)) {
        fa_store32(&s->flushing, 1);
        fa_device_wait_quiescent(s->device);   /* the mixer now skips this stream */
    }
    fa_mutex_lock(&s->decode_lock);
    r = restart_at_locked(s, frame);
    fa_mutex_unlock(&s->decode_lock);
    fa_store32(&s->flushing, 0);
    return r;
}

/* Back to the start (CUE_IN) with the crossfade gain reset, for a fresh play.
 * Caller holds api_lock. */
static void rewind_locked(struct fa_stream* s)
{
    fa_frames in = marker(s, FA_MARKER_CUE_IN);
    if (in == FA_FRAMES_UNSET) in = 0;
    if (s->decoder.seekable && (in != fa_stream_position_now(s) || fa_load32(&s->eof)))
        seek_locked(s, in);
    post_lane_locked(s, FA_LANE_FADE, 1.0f, 0, NULL, 0, 0, -1, 0);
}

/* Empties the event queues before playback can raise another ENDED or STARTED,
 * which keeps the stream's spill slots from overflowing (see FA_EVENT_SPILL). */
static void collect_events(struct fa_stream* s)
{
    fa_mutex_lock(&s->engine->list_lock);
    fa_event_collect_locked(s->engine);
    fa_mutex_unlock(&s->engine->list_lock);
}

/* Caller holds api_lock; the stream is attached. */
static void start_locked(struct fa_stream* s, int64_t at_frame)
{
    int32_t st = fa_load32(&s->state);
    if (st == FA_STATE_ENDED || st == FA_STATE_STOPPED) rewind_locked(s);
    collect_events(s);
    if (at_frame < 0) {
        fa_store32(&s->state, FA_STATE_PLAYING);
        return;
    }
    fa_store64(&s->start_frame, at_frame);
    fa_store32(&s->state, FA_STATE_SCHEDULED);
}

FA_API fa_result FA_CALL fa_stream_play(fa_stream* stream)
{
    fa_result r = FA_OK;
    if (!stream) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->api_lock);
    if (!is_attached(stream)) r = FA_ERROR_INVALID_ARG;
    else start_locked(stream, -1);
    fa_mutex_unlock(&stream->api_lock);
    return r;
}

FA_API fa_result FA_CALL fa_stream_play_at(fa_stream* stream, fa_time device_time)
{
    fa_result r = FA_OK;
    if (!stream || device_time < 0) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->api_lock);
    if (!is_attached(stream)) r = FA_ERROR_INVALID_ARG;
    else start_locked(stream, device_frame(stream, device_time));
    fa_mutex_unlock(&stream->api_lock);
    return r;
}

FA_API fa_result FA_CALL fa_stream_pause(fa_stream* stream)
{
    fa_result r = FA_OK;
    if (!stream) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->api_lock);
    if (!is_attached(stream)) r = FA_ERROR_INVALID_ARG;
    else {
        /* The mixer may start a scheduled stream meanwhile: retry until the state
         * we saw is the one we replaced. */
        for (;;) {
            int32_t st = fa_load32(&stream->state);
            if (st != FA_STATE_PLAYING && st != FA_STATE_SCHEDULED) break;
            if (fa_cas32(&stream->state, st, FA_STATE_PAUSED)) {
                fa_device_wait_quiescent(stream->device); /* once we return, nothing more is mixed */
                break;
            }
        }
    }
    fa_mutex_unlock(&stream->api_lock);
    return r;
}

FA_API fa_result FA_CALL fa_stream_stop(fa_stream* stream)
{
    fa_result r = FA_OK;
    if (!stream) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->api_lock);
    if (!is_attached(stream)) r = FA_ERROR_INVALID_ARG;
    else {
        fa_store32(&stream->state, FA_STATE_STOPPED);
        fa_device_wait_quiescent(stream->device);  /* once we return, nothing more is mixed */
        if (stream->decoder.seekable) rewind_locked(stream);
    }
    fa_mutex_unlock(&stream->api_lock);
    return r;
}

FA_API fa_result FA_CALL fa_stream_get_state(fa_stream* stream, fa_stream_state* out_state)
{
    if (!stream || !out_state) return FA_ERROR_INVALID_ARG;
    *out_state = (fa_stream_state)fa_load32(&stream->state);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_seek(fa_stream* stream, fa_frames frame)
{
    fa_result r;
    if (!stream || frame < 0) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->api_lock);
    r = seek_locked(stream, frame);
    if (r == FA_OK) {
        /* Stopped and ended mean "at the start" and "at the end": a seek leaves both. */
        fa_cas32(&stream->state, FA_STATE_ENDED, FA_STATE_PAUSED);
        fa_cas32(&stream->state, FA_STATE_STOPPED, FA_STATE_PAUSED);
    }
    fa_mutex_unlock(&stream->api_lock);
    return r;
}

FA_API fa_result FA_CALL fa_stream_get_position(fa_stream* stream, fa_frames* out_frame)
{
    if (!stream || !out_frame) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->api_lock);
    *out_frame = fa_stream_position_now(stream);
    fa_mutex_unlock(&stream->api_lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_clock(fa_stream* stream, fa_time* out_pts)
{
    fa_time latency = 0, pts;
    fa_frames pos;
    if (!stream || !out_pts) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->api_lock);
    pos = fa_stream_position_now(stream);
    if (stream->device) fa_device_get_latency(stream->device, &latency);
    fa_mutex_unlock(&stream->api_lock);
    pts = (fa_time)((double)pos * 1e9 / (double)stream->decoder.format.sample_rate) - latency;
    *out_pts = pts > 0 ? pts : 0;
    return FA_OK;
}

/* ---- parameters ---------------------------------------------------------- */

static fa_result check_param(fa_param param, float* value)
{
    if (param == FA_PARAM_VOLUME) {
        if (!(*value >= 0.0f)) return FA_ERROR_INVALID_ARG;
        return FA_OK;
    }
    if (param == FA_PARAM_PAN) {
        if (*value != *value) return FA_ERROR_INVALID_ARG;
        if (*value < -1.0f) *value = -1.0f;
        if (*value > 1.0f) *value = 1.0f;
        return FA_OK;
    }
    if ((int)param >= 0 && param <= FA_PARAM_PITCH) return FA_ERROR_NOT_IMPLEMENTED;
    return FA_ERROR_INVALID_ARG;
}

fa_result fa_param_check(fa_param param, float* value) { return check_param(param, value); }

FA_API fa_result FA_CALL fa_stream_set_param(fa_stream* stream, fa_param param, float value)
{
    fa_result r;
    if (!stream) return FA_ERROR_INVALID_ARG;
    r = check_param(param, &value);
    if (r != FA_OK) return r;
    fa_mutex_lock(&stream->api_lock);
    post_lane_locked(stream, param, value, FA_DEZIPPER_NS, NULL, 0, 0, -1, 0);
    fa_mutex_unlock(&stream->api_lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_ramp_param(fa_stream* stream, fa_param param, float target, fa_time duration, fa_time device_time, const fa_ramp* ramp)
{
    fa_result r;
    fa_shape shape;
    if (!stream || duration < 0 || device_time < 0) return FA_ERROR_INVALID_ARG;
    r = fa_shape_from_ramp(ramp, FA_CURVE_LINEAR, &shape);
    if (r != FA_OK) return r;
    r = check_param(param, &target);
    if (r != FA_OK) return r;
    fa_mutex_lock(&stream->api_lock);
    if (device_time != 0 && !is_attached(stream)) r = FA_ERROR_INVALID_ARG;
    else post_lane_locked(stream, param, target, duration, &shape, ramp ? ramp->tag : 0, 1,
                          device_time ? device_frame(stream, device_time) : -1, 0);
    fa_mutex_unlock(&stream->api_lock);
    return r;
}

FA_API fa_result FA_CALL fa_stream_get_param(fa_stream* stream, fa_param param, float* out_value)
{
    float v = 0.0f;
    fa_result r;
    if (!stream || !out_value) return FA_ERROR_INVALID_ARG;
    r = check_param(param, &v);
    if (r != FA_OK) return r;
    fa_mutex_lock(&stream->api_lock);
    *out_value = fa_lane_get(&stream->lanes[param]);
    fa_mutex_unlock(&stream->api_lock);
    return FA_OK;
}

/* ---- markers ------------------------------------------------------------ */

FA_API fa_result FA_CALL fa_stream_set_marker(fa_stream* stream, fa_marker which, fa_frames frame)
{
    fa_frames old;
    if (!stream || (int)which < 0 || (int)which >= FA_MARKER_COUNT) return FA_ERROR_INVALID_ARG;
    if (frame < 0 && frame != FA_FRAMES_UNSET) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->api_lock);
    old = fa_exchange64(&stream->markers[which], frame);
    if (which == FA_MARKER_CUE_IN && fa_load32(&stream->state) == FA_STATE_STOPPED && stream->decoder.seekable) {
        /* Stopped means "at the start": move there. */
        fa_frames in = frame == FA_FRAMES_UNSET ? 0 : frame;
        if (stream->decoder.length < 0 || in <= stream->decoder.length) seek_locked(stream, in);
    }
    if (which == FA_MARKER_CUE_OUT && old != frame && is_attached(stream) && stream->decoder.seekable) {
        /* Audio decoded ahead may already run past the new end (or stop short of a
         * later one): decode again from where playback is. */
        fa_frames pos = fa_stream_position_now(stream);
        if (frame == FA_FRAMES_UNSET || pos < frame) seek_locked(stream, pos);
    }
    fa_mutex_unlock(&stream->api_lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_marker(fa_stream* stream, fa_marker which, fa_frames* out_frame)
{
    if (!stream || !out_frame || (int)which < 0 || (int)which >= FA_MARKER_COUNT) return FA_ERROR_INVALID_ARG;
    *out_frame = marker(stream, which);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_set_fade_curve(fa_stream* stream, fa_fade fade, const fa_ramp* shape)
{
    fa_shape sh;
    fa_result r;
    if (!stream || (fade != FA_FADE_IN && fade != FA_FADE_OUT)) return FA_ERROR_INVALID_ARG;
    r = fa_shape_from_ramp(shape, FA_CURVE_EQUAL_POWER, &sh);
    if (r != FA_OK) return r;
    fa_mutex_lock(&stream->api_lock);
    fa_shape_store(&stream->fade_shape[fade], &sh);
    fa_mutex_unlock(&stream->api_lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_fade_curve(fa_stream* stream, fa_fade fade, fa_ramp* out_shape)
{
    fa_shape sh;
    if (!stream || !out_shape || (fade != FA_FADE_IN && fade != FA_FADE_OUT)) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->api_lock);
    fa_shape_load(&stream->fade_shape[fade], &sh);
    fa_mutex_unlock(&stream->api_lock);
    fa_shape_to_ramp(&sh, out_shape);
    return FA_OK;
}

/* ---- crossfades and queues -------------------------------------------------- */

/* Makes sure `next` plays on `stream`'s device, through the same bus. */
static fa_result join_device(struct fa_stream* stream, struct fa_stream* next, fa_device** out_dev)
{
    fa_device* dev;
    fa_bus* bus;
    fa_device* other;
    fa_mutex_lock(&stream->api_lock);
    dev = stream->device;
    bus = (fa_bus*)fa_load_ptr(&stream->bus);
    fa_mutex_unlock(&stream->api_lock);
    if (!dev) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&next->api_lock);
    other = next->device;
    fa_mutex_unlock(&next->api_lock);
    if (other && other != dev) return FA_ERROR_INVALID_ARG;
    if (!other) {
        fa_result r = fa_stream_attach(next, dev, bus);
        if (r != FA_OK) return r;
    }
    *out_dev = dev;
    return FA_OK;
}

/* Two streams' api_locks, always in the same (address) order. */
static void lock_pair(struct fa_stream* a, struct fa_stream* b)
{
    if ((uintptr_t)a > (uintptr_t)b) { struct fa_stream* t = a; a = b; b = t; }
    fa_mutex_lock(&a->api_lock);
    fa_mutex_lock(&b->api_lock);
}

static void unlock_pair(struct fa_stream* a, struct fa_stream* b)
{
    fa_mutex_unlock(&a->api_lock);
    fa_mutex_unlock(&b->api_lock);
}

FA_API fa_result FA_CALL fa_crossfade(fa_stream* from, fa_stream* to, fa_time duration, const fa_ramp* ramp)
{
    fa_shape shape;
    fa_device* dev;
    fa_result r;
    int64_t at, tag;
    if (!from || !to || from == to || duration < 0) return FA_ERROR_INVALID_ARG;
    if (from->engine != to->engine || (to->flags & FA_OPEN_DECODE_ONLY)) return FA_ERROR_INVALID_ARG;
    r = fa_shape_from_ramp(ramp, FA_CURVE_EQUAL_POWER, &shape);
    if (r != FA_OK) return r;
    r = join_device(from, to, &dev);
    if (r != FA_OK) return r;
    tag = ramp ? ramp->tag : 0;

    /* Rewind `to` first: that seeks and prefills, which must not eat into the lead time. */
    fa_mutex_lock(&to->api_lock);
    if (to->device != dev) r = FA_ERROR_INVALID_ARG;  /* moved away meanwhile */
    else if (fa_load32(&to->state) == FA_STATE_ENDED || fa_load32(&to->state) == FA_STATE_STOPPED) rewind_locked(to);
    fa_mutex_unlock(&to->api_lock);
    if (r != FA_OK) return r;

    /* Then publish the start and both fades back to back, holding both streams, one
     * period ahead: a callback that is running now has taken its commands already,
     * and the next one starts at `at` with both in place. */
    lock_pair(from, to);
    if (to->device != dev || from->device != dev) r = FA_ERROR_INVALID_ARG;
    else {
        int32_t st = fa_load32(&to->state);
        at = fa_load64(&dev->clock) + fa_load32(&dev->period);
        if (st != FA_STATE_PLAYING) {
            if (st == FA_STATE_ENDED || st == FA_STATE_STOPPED) rewind_locked(to);  /* changed meanwhile */
            collect_events(to);
            fa_store64(&to->start_frame, at);
            fa_store32(&to->state, FA_STATE_SCHEDULED);
            post_lane_locked(to, FA_LANE_FADE, 0.0f, 0, NULL, 0, 0, -1, 0);
        }
        post_lane_locked(to, FA_LANE_FADE, 1.0f, duration, &shape, tag, 1, at, 0);
        post_lane_locked(from, FA_LANE_FADE, 0.0f, duration, &shape, tag, 1, at, FA_LANE_MIRROR | FA_LANE_END_STREAM);
    }
    unlock_pair(from, to);
    return r;
}

FA_API fa_result FA_CALL fa_stream_set_next(fa_stream* stream, fa_stream* next, fa_time crossfade, const fa_ramp* shape)
{
    fa_shape sh;
    fa_device* dev;
    fa_result r;
    struct fa_stream* old;
    int start_now = 0;
    if (!stream || next == stream || crossfade < 0) return FA_ERROR_INVALID_ARG;
    if (!next) {
        fa_mutex_lock(&stream->api_lock);
        unqueue((struct fa_stream*)fa_exchange_ptr(&stream->queued, NULL));
        fa_mutex_unlock(&stream->api_lock);
        return FA_OK;
    }
    if (next->engine != stream->engine || (next->flags & FA_OPEN_DECODE_ONLY)) return FA_ERROR_INVALID_ARG;
    r = fa_shape_from_ramp(shape, FA_CURVE_EQUAL_POWER, &sh);
    if (r != FA_OK) return r;
    if (fa_load32(&next->state) == FA_STATE_PLAYING) return FA_ERROR_BUSY;
    r = join_device(stream, next, &dev);
    if (r != FA_OK) return r;

    /* Ready to go: positioned at its cue-in and buffered, waiting for the trigger. */
    fa_mutex_lock(&next->api_lock);
    if (next->device != dev) r = FA_ERROR_INVALID_ARG;
    else if (fa_load32(&next->state) == FA_STATE_PLAYING) r = FA_ERROR_BUSY;
    else {
        int32_t st = fa_load32(&next->state);
        if (st == FA_STATE_ENDED || st == FA_STATE_STOPPED) rewind_locked(next);
        collect_events(next);
        fa_store64(&next->start_frame, FA_START_QUEUED);
        fa_store32(&next->state, FA_STATE_SCHEDULED);
    }
    fa_mutex_unlock(&next->api_lock);
    if (r != FA_OK) return r;

    fa_mutex_lock(&stream->api_lock);
    if (stream->device != dev) r = FA_ERROR_INVALID_ARG;
    else {
        fa_store64(&stream->xfade_ns, crossfade);
        fa_shape_store(&stream->xfade_shape, &sh);
        old = (struct fa_stream*)fa_exchange_ptr(&stream->queued, next);
        if (old != next) unqueue(old);
        start_now = fa_load32(&stream->state) == FA_STATE_ENDED;
    }
    fa_mutex_unlock(&stream->api_lock);
    if (r != FA_OK) { unqueue(next); return r; }

    if (start_now && fa_cas_ptr(&stream->queued, next, NULL)) {
        /* Too late for a transition: the stream has already ended. Start `next` on
         * the next callback, through the scheduled start (which raises STARTED). */
        fa_mutex_lock(&next->api_lock);
        if (fa_load32(&next->state) == FA_STATE_SCHEDULED && fa_load64(&next->start_frame) == FA_START_QUEUED)
            fa_store64(&next->start_frame, 0);
        fa_mutex_unlock(&next->api_lock);
    }
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_next(fa_stream* stream, fa_stream** out_next)
{
    if (!stream || !out_next) return FA_ERROR_INVALID_ARG;
    *out_next = (fa_stream*)fa_load_ptr(&stream->queued);
    return FA_OK;
}

/* ---- channel matrix ----------------------------------------------------- */

FA_API fa_result FA_CALL fa_stream_set_matrix(fa_stream* stream, const float* weights, uint32_t in_channels, uint32_t out_channels)
{
    float* m = NULL;
    fa_result r = FA_OK;
    uint32_t i;
    if (!stream) return FA_ERROR_INVALID_ARG;
    if (weights) {
        if (in_channels != stream->decoder.format.channels || out_channels != FA_DEVICE_CHANNELS) return FA_ERROR_INVALID_ARG;
        for (i = 0; i < in_channels * out_channels; i++)
            if (!(weights[i] == weights[i]) || fabsf(weights[i]) > 1000.0f) return FA_ERROR_INVALID_ARG;
        m = (float*)malloc((size_t)in_channels * out_channels * sizeof(float));
        if (!m) return FA_ERROR_OUT_OF_MEMORY;
        memcpy(m, weights, (size_t)in_channels * out_channels * sizeof(float));
    }
    fa_mutex_lock(&stream->api_lock);
    if (is_attached(stream) && !stream->decoder.seekable) {
        r = FA_ERROR_NOT_SEEKABLE;  /* the new chain would have to decode from where playback is */
        free(m);
    } else if (is_attached(stream)) {
        fa_frames pos;
        fa_store32(&stream->flushing, 1);
        fa_device_wait_quiescent(stream->device);
        pos = fa_stream_position_now(stream);  /* after the mixer let go: nothing more is consumed */
        fa_engine_track_stream(stream->engine, stream, 0);  /* workers look at the ring we replace */
        fa_mutex_lock(&stream->decode_lock);
        free(stream->matrix);
        stream->matrix = m;
        r = build_chain_locked(stream, stream->device);
        if (r == FA_OK) {
            /* restart_at_locked only seeks the decoder when needed: force it, the
             * buffers are new. */
            fa_store64(&stream->seek_base, pos);
            fa_store64(&stream->played, 0);
            fa_store32(&stream->eof, 1);
            r = restart_at_locked(stream, pos);
        }
        fa_mutex_unlock(&stream->decode_lock);
        fa_engine_track_stream(stream->engine, stream, 1);
        if (r == FA_OK) fa_store32(&stream->flushing, 0);
        /* On failure the stream stays silent (flushing) until the next attach. */
    } else {
        fa_mutex_lock(&stream->decode_lock);
        free(stream->matrix);
        stream->matrix = m;
        fa_mutex_unlock(&stream->decode_lock);
    }
    fa_mutex_unlock(&stream->api_lock);
    return r;
}

/* ---- events ------------------------------------------------------------- */

FA_API fa_result FA_CALL fa_stream_set_event_callback(fa_stream* stream, fa_event_callback callback, void* user)
{
    if (!stream) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->cb_lock);
    stream->callback = callback;
    stream->callback_user = user;
    fa_mutex_unlock(&stream->cb_lock);
    /* Let a callback that already copied the old pointer finish first. */
    if ((int64_t)fa_thread_current_id() != fa_load64(&stream->engine->event_thread_id))
        while (fa_load32(&stream->in_callback)) fa_sleep_ms(1);
    return FA_OK;
}

/* ---- decode-only read --------------------------------------------------- */

FA_API fa_result FA_CALL fa_stream_read(fa_stream* stream, void* buffer, fa_frames frames, fa_frames* out_frames_read)
{
    fa_result r;
    fa_frames got = 0;
    if (!stream || !buffer || frames < 0 || !out_frames_read) return FA_ERROR_INVALID_ARG;
    *out_frames_read = 0;
    if (stream->device) return FA_ERROR_BUSY; /* attached streams are pulled by the device */
    if (frames == 0) return FA_OK;
    fa_mutex_lock(&stream->decode_lock);
    r = fa_decoder_read(&stream->decoder, (float*)buffer, frames, &got);
    fa_fetch_add64(&stream->position, got);
    fa_mutex_unlock(&stream->decode_lock);
    *out_frames_read = got;
    return r;
}

/* ---- mixing (audio thread) ----------------------------------------------- */

static void push_event(fa_device* dev, struct fa_stream* s, fa_event_type type, int64_t value)
{
    fa_audio_post_event(dev, s, type, attached_position(s), value);
}

static void stereo_gains(float vol, float pan, float* gl, float* gr)
{
    *gl = vol * (pan > 0.0f ? 1.0f - pan : 1.0f);
    *gr = vol * (pan < 0.0f ? 1.0f + pan : 1.0f);
}

void fa_bus_gains_at(struct fa_bus* b, fa_device* dev, int64_t off, int from_here, float* gl, float* gr)
{
    int64_t clock = fa_load64(&dev->clock);
    stereo_gains(fa_lane_value_at_frame(&b->lanes[FA_PARAM_VOLUME].v, clock, clock + off, from_here),
                 fa_lane_value_at_frame(&b->lanes[FA_PARAM_PAN].v, clock, clock + off, from_here), gl, gr);
}

/* Shortens a block so it ends where a bus ramp starts or ends: gains are exact
 * at block edges only. */
static int64_t bus_block(struct fa_bus* b, int64_t clock, int64_t now, int64_t blk)
{
    int p;
    for (p = 0; p < FA_MIX_PARAMS; p++) {
        const fa_param_voice* v = &b->lanes[p].v;
        int64_t begin, end;
        if (!v->active) continue;
        begin = v->at > clock ? v->at : clock;
        end = begin + (v->total - v->done);
        if (begin > now && begin - now < blk) blk = begin - now;
        else if (begin <= now && end > now && end - now < blk) blk = end - now;
    }
    return blk;
}

static void mix_region(float* out, const float* in, int64_t n, float l0, float r0, float l1, float r1)
{
    int64_t i;
    if (l0 == l1 && r0 == r1) {
        for (i = 0; i < n; i++) { out[i * 2] += in[i * 2] * l0; out[i * 2 + 1] += in[i * 2 + 1] * r0; }
    } else {
        float dl = (l1 - l0) / (float)n, dr = (r1 - r0) / (float)n;
        for (i = 0; i < n; i++) {
            out[i * 2] += in[i * 2] * (l0 + dl * (float)i);
            out[i * 2 + 1] += in[i * 2 + 1] * (r0 + dr * (float)i);
        }
    }
}

static float db_to_gain(float db) { return db == 0.0f ? 1.0f : powf(10.0f, db / 20.0f); }

/* Moves the normalization gain toward its target: down at most 12 dB/s, up at
 * most 6 dB/s, so a corrected estimate glides instead of jumping. */
static float norm_glide(float db, float target, int64_t frames, uint32_t rate)
{
    float step;
    if (db == target) return db;
    step = (float)frames / (float)rate * (target < db ? FA_NORM_GLIDE_DOWN_DB_S : FA_NORM_GLIDE_UP_DB_S);
    if (target < db) return db - step > target ? db - step : target;
    return db + step < target ? db + step : target;
}

/* Marker positions and fade shapes, read once per callback. */
typedef struct mix_markers {
    fa_frames m[FA_MARKER_COUNT];
    fa_frames fade_in_from, fade_in_to, fade_out_from, fade_out_to; /* -1 = no fade */
    fa_shape in_shape, out_shape;
} mix_markers;

static void load_markers(struct fa_stream* s, mix_markers* mk)
{
    int i;
    for (i = 0; i < FA_MARKER_COUNT; i++) mk->m[i] = marker(s, i);
    mk->fade_in_from = mk->fade_in_to = mk->fade_out_from = mk->fade_out_to = -1;
    if (mk->m[FA_MARKER_FADE_IN_END] != FA_FRAMES_UNSET) {
        fa_frames a = mk->m[FA_MARKER_CUE_IN] != FA_FRAMES_UNSET ? mk->m[FA_MARKER_CUE_IN] : 0;
        if (mk->m[FA_MARKER_FADE_IN_END] > a) {
            mk->fade_in_from = a;
            mk->fade_in_to = mk->m[FA_MARKER_FADE_IN_END];
            fa_shape_load(&s->fade_shape[FA_FADE_IN], &mk->in_shape);
        }
    }
    if (mk->m[FA_MARKER_FADE_OUT_START] != FA_FRAMES_UNSET) {
        fa_frames e = end_frame(s);
        if (e > mk->m[FA_MARKER_FADE_OUT_START]) {
            mk->fade_out_from = mk->m[FA_MARKER_FADE_OUT_START];
            mk->fade_out_to = e;
            fa_shape_load(&s->fade_shape[FA_FADE_OUT], &mk->out_shape);
        }
    }
}

/* Gain of the marker fades at source position `pos`. */
static float marker_gain(const mix_markers* mk, double pos)
{
    float g = 1.0f;
    if (mk->fade_in_to >= 0 && pos < (double)mk->fade_in_to) {
        double t = (pos - (double)mk->fade_in_from) / (double)(mk->fade_in_to - mk->fade_in_from);
        g *= t <= 0.0 ? fa_shape_eval(&mk->in_shape, 0.0f, 1) : fa_shape_eval(&mk->in_shape, (float)t, 1);
    }
    if (mk->fade_out_to >= 0 && pos > (double)mk->fade_out_from) {
        double t = (pos - (double)mk->fade_out_from) / (double)(mk->fade_out_to - mk->fade_out_from);
        g *= fa_shape_eval(&mk->out_shape, t >= 1.0 ? 0.0f : (float)(1.0 - t), 1);
    }
    return g;
}

/* Source position for the fades: wrapped into the loop, not clamped. */
static double fade_position(struct fa_stream* s, int64_t played)
{
    double lin = linear_position(s, played);
    fa_frames end = end_frame(s);
    if ((s->flags & FA_OPEN_LOOP) && end > 0 && lin >= (double)end) {
        fa_frames back = loop_start(s), len = end - back;
        if (len > 0) return (double)back + fmod(lin - (double)end, (double)len);
    }
    return lin;
}

/* FA_EVENT_MARKER for every marker passed between `played0` and `played1`. */
static void marker_events(struct fa_stream* s, fa_device* dev, const mix_markers* mk, int64_t played0, int64_t played1, int ended)
{
    fa_frames p0 = (fa_frames)(linear_position(s, played0) + 0.5);
    fa_frames p1 = (fa_frames)(linear_position(s, played1) + 0.5);
    fa_frames end = end_frame(s), back = loop_start(s), w0, w1;
    int looping = (s->flags & FA_OPEN_LOOP) && end > 0 && end > back;
    int i;
    if (p1 <= p0 && !ended) return;
    w0 = wrap_position(s, p0);
    w1 = wrap_position(s, p1);
    for (i = 0; i < FA_MARKER_COUNT; i++) {
        fa_frames m = mk->m[i];
        int hit;
        if (m == FA_FRAMES_UNSET) continue;
        /* The loop end (CUE_OUT) is passed each time playback wraps. */
        if (looping && p1 - p0 >= end - back) hit = m >= back && m <= end;
        else if (looping && w1 < w0) hit = (m >= w0 && m <= end) || (m >= back && m < w1);
        else hit = m >= w0 && (m < w1 || (ended && m == w1));
        if (hit) fa_audio_post_event(dev, s, FA_EVENT_MARKER, m, i);
    }
}

/* Ends the stream from the audio thread (its data ran out, or a crossfade-out
 * finished). A crossfade still running ends with it: it raises its RAMP_DONE (so
 * whoever waits for the crossfade is not left waiting), and a later play starts
 * at full crossfade gain. */
static void end_stream(struct fa_stream* s, fa_device* dev)
{
    fa_param_voice* v = &s->lanes[FA_LANE_FADE].v;
    if (!fa_cas32(&s->state, FA_STATE_PLAYING, FA_STATE_ENDED)) return;
    if (v->active) {
        if (v->notify) push_event(dev, s, FA_EVENT_RAMP_DONE, v->tag);
        v->active = 0;
    }
    v->value = 1.0f;
    fa_store_float(&s->lanes[FA_LANE_FADE].current, 1.0f);
    push_event(dev, s, FA_EVENT_ENDED, 0);
}

/* A lane's ramp ran its course. Returns 1 when that ends the stream. */
static int finish_lane(struct fa_stream* s, fa_device* dev, fa_param_voice* v)
{
    v->active = 0;
    v->value = v->target;
    if (v->notify) push_event(dev, s, FA_EVENT_RAMP_DONE, v->tag);
    if (v->flags & FA_LANE_END_STREAM) {
        v->value = 1.0f;  /* a later play starts at full crossfade gain */
        return 1;
    }
    return 0;
}

/* Device frames from `played` until the stream reaches its CUE_OUT, or -1 when no
 * cue-out applies (unset, or a looping stream). The mixer stops there even when
 * audio past it was buffered already (a cue-out moved while playing). */
static int64_t frames_to_cue_out(struct fa_stream* s, int64_t played)
{
    fa_frames out = marker(s, FA_MARKER_CUE_OUT);
    double d;
    if (out == FA_FRAMES_UNSET || (s->flags & FA_OPEN_LOOP)) return -1;
    d = ((double)out - linear_position(s, played)) * (double)s->dev_rate / (double)s->decoder.format.sample_rate;
    return d <= 0.0 ? 0 : (int64_t)ceil(d - 1e-9);
}

/* Frames from now until `s` reaches its mix point, or -1 if not within `frames`. */
static int64_t frames_to_mix_point(struct fa_stream* s, int64_t frames)
{
    fa_frames mix = marker(s, FA_MARKER_MIX_POINT);
    int64_t xf_ns = fa_load64(&s->xfade_ns);
    double sr = (double)s->decoder.format.sample_rate, dr = (double)s->dev_rate;
    /* A loop has no end to count back from: it hands over at its MIX_POINT only. */
    if (mix == FA_FRAMES_UNSET && xf_ns > 0 && !(s->flags & FA_OPEN_LOOP)) {
        fa_frames end = end_frame(s);
        if (end >= 0) {
            mix = end - (fa_frames)((double)xf_ns * sr / 1e9 + 0.5);
            if (mix < 0) mix = 0;
        }
    }
    if (mix != FA_FRAMES_UNSET) {
        double dist = (double)mix - fade_position(s, fa_load64(&s->played));
        int64_t k;
        if (dist < 0.0 && (s->flags & FA_OPEN_LOOP)) {
            fa_frames end = end_frame(s), back = loop_start(s);
            if (end > back) dist += (double)(end - back);
        }
        k = dist <= 0.0 ? 0 : (int64_t)ceil(dist * dr / sr - 1e-9);
        if (k < frames) return k;
    }
    /* At the end: gapless (or the fallback when the end was not known in advance). */
    if (!(s->flags & FA_OPEN_LOOP)) {
        int64_t avail = fa_ring_readable(&s->ring), left = -1;
        int64_t cut = frames_to_cue_out(s, fa_load64(&s->played));
        if (fa_load32(&s->eof)) left = avail;
        if (cut >= 0 && cut <= avail && (left < 0 || cut < left)) left = cut;
        if (left >= 0 && left < frames) return left;
    }
    return -1;
}

/* Applies a command waiting on the fade lane now, so the queue's ramp installed
 * next is not overwritten by it, and raises RAMP_DONE for a ramp it replaces. */
static void settle_fade_lane(struct fa_stream* s, fa_device* dev, int audible)
{
    fa_param_voice* v = &s->lanes[FA_LANE_FADE].v;
    int64_t tag = 0;
    if (fa_lane_take(&s->lanes[FA_LANE_FADE], audible, dev->rate, &tag)) push_event(dev, s, FA_EVENT_RAMP_DONE, tag);
    if (v->active && v->notify) push_event(dev, s, FA_EVENT_RAMP_DONE, v->tag);
    v->active = 0;
}

int fa_stream_check_queue(struct fa_stream* s, fa_device* dev, uint32_t frames)
{
    struct fa_stream* next = (struct fa_stream*)fa_load_ptr(&s->queued);
    int64_t k, at, xf_ns, clock = fa_load64(&dev->clock), base = 0;
    int32_t st = fa_load32(&s->state);
    if (!next || fa_load32(&s->flushing) || !s->dev_rate) return 0;
    if (st == FA_STATE_SCHEDULED) {
        /* Starts within this callback (a queue transition found earlier in this
         * callback): its own mix point may fall in it too. */
        int64_t start = fa_load64(&s->start_frame);
        if (start == FA_START_QUEUED || start >= clock + (int64_t)frames) return 0;
        base = start > clock ? start - clock : 0;
    } else if (st != FA_STATE_PLAYING) {
        return 0;
    }
    k = frames_to_mix_point(s, (int64_t)frames - base);
    if (k < 0) return 0;
    if (!fa_cas_ptr(&s->queued, next, NULL)) return 0;   /* changed by an API thread meanwhile */
    if (fa_load32(&next->state) != FA_STATE_SCHEDULED || fa_load64(&next->start_frame) != FA_START_QUEUED) return 0;
    at = clock + base + k;
    xf_ns = fa_load64(&s->xfade_ns);
    if (xf_ns > 0) {
        int64_t xf = (int64_t)((double)xf_ns * dev->rate / 1e9 + 0.5);
        fa_shape shape;
        fa_shape_load(&s->xfade_shape, &shape);
        if (marker(s, FA_MARKER_FADE_OUT_START) == FA_FRAMES_UNSET) {
            settle_fade_lane(s, dev, st == FA_STATE_PLAYING && s->was_audible);
            fa_lane_start_ramp(&s->lanes[FA_LANE_FADE], s->lanes[FA_LANE_FADE].v.value, 0.0f, xf, at, &shape,
                               FA_LANE_MIRROR | FA_LANE_END_STREAM);
        }
        if (marker(next, FA_MARKER_FADE_IN_END) == FA_FRAMES_UNSET) {
            settle_fade_lane(next, dev, 0);
            fa_lane_start_ramp(&next->lanes[FA_LANE_FADE], 0.0f, 1.0f, xf, at, &shape, 0);
        }
    }
    fa_store64(&next->start_frame, at);
    return 1;
}

void fa_stream_mix(struct fa_stream* s, fa_device* dev, float* out, uint32_t frames)
{
    int64_t avail, n, done = 0, clock, off = 0, played0;
    int p, playing, audible, ended = 0, at_cue_out = 0, looping;
    fa_frames loop_end, loop_back;
    double sr = (double)s->decoder.format.sample_rate, dr = (double)s->dev_rate;
    int32_t state;
    float norm_target, norm_lin;
    mix_markers mk;
    struct fa_bus* bus;
    if (fa_load32(&s->flushing)) return;
    clock = fa_load64(&dev->clock);

    state = fa_load32(&s->state);
    if (state == FA_STATE_SCHEDULED) {
        int64_t start = fa_load64(&s->start_frame);
        if (start != FA_START_QUEUED && start < clock + (int64_t)frames &&
            fa_cas32(&s->state, FA_STATE_SCHEDULED, FA_STATE_PLAYING)) {
            off = start > clock ? start - clock : 0;
            state = FA_STATE_PLAYING;
            push_event(dev, s, FA_EVENT_STARTED, 0);
        }
    }
    playing = state == FA_STATE_PLAYING;
    audible = playing && s->was_audible;
    for (p = 0; p < FA_STREAM_LANES; p++) {
        int64_t tag = 0;
        fa_param_voice* v = &s->lanes[p].v;
        if (fa_lane_take(&s->lanes[p], audible, dev->rate, &tag)) push_event(dev, s, FA_EVENT_RAMP_DONE, tag);
        /* A zero-length ramp that has started is a jump: the first sample already uses the target. */
        if (playing && v->active && v->done >= v->total && (v->at < 0 || v->at <= clock + off))
            if (finish_lane(s, dev, v)) { end_stream(s, dev); playing = 0; }
    }
    if (!playing) {
        for (p = 0; p < FA_STREAM_LANES; p++) fa_store_float(&s->lanes[p].current, s->lanes[p].v.value);
        s->was_audible = 0;
        return;
    }
    norm_target = fa_stream_norm_target_db(s);
    if (!s->was_audible) s->norm_db = norm_target; /* nothing heard yet: start at the right gain */
    s->was_audible = 1;
    norm_lin = db_to_gain(s->norm_db);
    load_markers(s, &mk);
    played0 = fa_load64(&s->played);
    bus = (struct fa_bus*)fa_load_ptr(&s->bus);

    avail = fa_ring_readable(&s->ring);
    n = avail < (int64_t)frames - off ? avail : (int64_t)frames - off;
    {
        int64_t cut = frames_to_cue_out(s, played0);
        if (cut >= 0 && cut <= n) { n = cut; at_cue_out = 1; }
    }
    loop_end = end_frame(s);
    loop_back = loop_start(s);
    looping = (s->flags & FA_OPEN_LOOP) && loop_end > 0 && loop_end > loop_back;

    while (done < n && !ended) {
        /* Blocks of FA_RAMP_BLOCK frames: gains are exact at block edges and
         * interpolated linearly inside, so ramps and changes are click-free. Blocks
         * also end exactly where a ramp starts or ends. */
        int64_t blk = n - done < FA_RAMP_BLOCK ? n - done : FA_RAMP_BLOCK;
        int64_t now = clock + off + done, pl = played0 + done;
        float v0[FA_STREAM_LANES], v1[FA_STREAM_LANES], g0, g1, l0, r0, l1, r1;
        float bl0 = 1.0f, br0 = 1.0f, bl1 = 1.0f, br1 = 1.0f;
        double pos0, pos1;
        const float *p1, *p2;
        int64_t n1, n2;
        for (p = 0; p < FA_STREAM_LANES; p++) {
            /* A zero-length ramp due here is a jump: this block already uses its target. */
            fa_param_voice* v = &s->lanes[p].v;
            if (v->active && v->at <= now && v->done >= v->total) {
                v->at = -1;
                if (finish_lane(s, dev, v)) ended = 1;
            }
        }
        if (ended) break;
        for (p = 0; p < FA_STREAM_LANES; p++) {
            fa_param_voice* v = &s->lanes[p].v;
            if (!v->active) continue;
            if (v->at > now) { if (v->at - now < blk) blk = v->at - now; }
            else if (v->total - v->done > 0 && v->total - v->done < blk) blk = v->total - v->done;
        }
        if (bus) blk = bus_block(bus, clock, now, blk);
        pos0 = fade_position(s, pl);
        if (looping) {
            /* Blocks end at the loop end, so the fades are not interpolated toward
             * the next turn's level before the wrap. */
            int64_t to_end = (int64_t)ceil(((double)loop_end - pos0) * dr / sr - 1e-9);
            if (to_end > 0 && to_end < blk) blk = to_end;
        }
        pos1 = pos0 + (double)blk * sr / dr;
        for (p = 0; p < FA_STREAM_LANES; p++) {
            fa_param_voice* v = &s->lanes[p].v;
            v0[p] = v->value;
            v1[p] = (v->active && v->at <= now) ? fa_lane_ramp_value(v, v->done + blk) : v->value;
        }
        g0 = v0[FA_PARAM_VOLUME] * v0[FA_LANE_FADE] * marker_gain(&mk, pos0);
        g1 = v1[FA_PARAM_VOLUME] * v1[FA_LANE_FADE] * marker_gain(&mk, pos1);
        {
            float nd1 = norm_glide(s->norm_db, norm_target, blk, dev->rate);
            float nl1 = nd1 == s->norm_db ? norm_lin : db_to_gain(nd1);
            stereo_gains(g0 * norm_lin, v0[FA_PARAM_PAN], &l0, &r0);
            stereo_gains(g1 * nl1, v1[FA_PARAM_PAN], &l1, &r1);
            s->norm_db = nd1;
            norm_lin = nl1;
        }
        if (bus) {
            fa_bus_gains_at(bus, dev, off + done, 1, &bl0, &br0);
            fa_bus_gains_at(bus, dev, off + done + blk, 0, &bl1, &br1);
            l0 *= bl0; r0 *= br0; l1 *= bl1; r1 *= br1;
        }
        fa_ring_regions(&s->ring, blk, &p1, &n1, &p2, &n2);
        {
            float* o = out + (off + done) * 2;
            float lm = l0 + (l1 - l0) * (float)n1 / (float)blk, rm = r0 + (r1 - r0) * (float)n1 / (float)blk;
            mix_region(o, p1, n1, l0, r0, lm, rm);
            if (n2 > 0) mix_region(o + n1 * 2, p2, n2, lm, rm, l1, r1);
        }
        fa_ring_advance_read(&s->ring, blk);
        fa_fetch_add64(&s->played, blk);
        done += blk;

        for (p = 0; p < FA_STREAM_LANES; p++) {
            fa_param_voice* v = &s->lanes[p].v;
            v->value = v1[p];
            if (!v->active || v->at > now) continue;
            v->at = -1;
            v->done += blk;
            if (v->done >= v->total && finish_lane(s, dev, v)) ended = 1;
        }
    }
    for (p = 0; p < FA_STREAM_LANES; p++) fa_store_float(&s->lanes[p].current, s->lanes[p].v.value);

    if (ended) {
        marker_events(s, dev, &mk, played0, played0 + done, 0);
        end_stream(s, dev);
        return;
    }
    if (at_cue_out) {
        marker_events(s, dev, &mk, played0, played0 + done, 1);
        end_stream(s, dev);
        return;
    }
    if (n < (int64_t)frames - off) {
        if (fa_load32(&s->eof) && fa_ring_readable(&s->ring) == 0) {
            marker_events(s, dev, &mk, played0, played0 + done, 1);
            end_stream(s, dev);
            return;
        }
        fa_fetch_add32(&s->underruns, 1);
    }
    marker_events(s, dev, &mk, played0, played0 + done, 0);
}
