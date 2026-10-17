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

FA_API void FA_CALL fa_open_options_init(fa_open_options* options)
{
    if (!options) return;
    memset(options, 0, sizeof(*options));
    options->struct_size = (uint32_t)sizeof(*options);
    options->audio_track = -1;
}

static int is_attached(struct fa_stream* s) { return s->device != NULL; }

/* ---- open / close ------------------------------------------------------- */

void fa_stream_free_memory(struct fa_stream* s)
{
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
    s->in_cap = s->out_cap = s->in_count = s->in_offset = 0;
    s->decoded = 0;
    s->draining = 0;
}

/* Frees a stream that was never published to anyone (failed open). */
static void stream_discard(struct fa_stream* s)
{
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
    s->engine = engine;
    s->flags = options ? options->flags : 0;
    s->buffer_ms = FA_DEFAULT_BUFFER_MS;
    if (options && options->struct_size >= sizeof(fa_open_options) && options->buffer_ms) {
        s->buffer_ms = options->buffer_ms;
        if (s->buffer_ms < 50) s->buffer_ms = 50;
        if (s->buffer_ms > 10000) s->buffer_ms = 10000;
    }
    fa_store32(&s->state, FA_STATE_STOPPED);
    for (i = 0; i < FA_MARKER_COUNT; i++) s->markers[i] = FA_FRAMES_UNSET;
    for (i = 0; i < FA_MIX_PARAMS; i++) {
        fa_store32(&s->mailbox[i].word, FA_CMD_CLAIMED); /* nothing pending */
        s->voice[i].value = i == FA_PARAM_VOLUME ? 1.0f : 0.0f;
        s->mailbox[i].last_target = s->voice[i].value;
        fa_store_float(&s->current[i], s->voice[i].value);
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
    return FA_OK;
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
    if (r != FA_OK) { stream_discard(s); return r; }
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
    *out_stream = s;
    return FA_OK;
}

FA_API void FA_CALL fa_stream_close(fa_stream* stream)
{
    fa_engine* e;
    uintptr_t self;
    if (!stream) return;
    e = stream->engine;
    fa_stream_detach(stream);

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

/* Decodes, converts to the device format and appends to the ring until it is
 * full, `max_frames` were produced, or the source ended. Caller holds decode_lock. */
void fa_stream_fill(struct fa_stream* s, int64_t max_frames)
{
    uint32_t ch = s->decoder.format.channels;
    int64_t produced = 0;
    int just_looped = 0;
    if (!s->ring.data || fa_load32(&s->eof)) return;

    while (produced < max_frames) {
        ma_uint64 in_n, out_n;
        int64_t room = fa_ring_writable(&s->ring);
        if (room <= 0) break;
        if (s->in_count == 0) {
            fa_frames got = 0;
            if (s->draining) {                    /* the resampler has given back everything */
                fa_store32(&s->eof, 1);
                break;
            }
            fa_decoder_read(&s->decoder, s->in_buf, s->in_cap, &got);
            s->in_offset = 0;
            s->in_count = got;
            s->decoded += got;
            if (got == 0) {
                ma_uint64 latency;
                if ((s->flags & FA_OPEN_LOOP) && !just_looped && fa_decoder_seek(&s->decoder, 0) == FA_OK) {
                    just_looped = 1; /* a second empty read right after looping = empty source */
                    continue;            /* the converter keeps its state: the loop joins seamlessly */
                }
                /* The resampler still holds the last few frames: push them out with
                 * silence (its input latency) before calling it the end. */
                latency = ma_data_converter_get_input_latency(&s->converter);
                if (latency > 0) {
                    if (latency > (ma_uint64)s->in_cap) latency = (ma_uint64)s->in_cap;
                    memset(s->in_buf, 0, (size_t)latency * ch * sizeof(float));
                    s->in_count = (fa_frames)latency;
                    s->draining = 1;
                    continue;
                }
                fa_store32(&s->eof, 1);
                break;
            }
            just_looped = 0;
        }
        in_n = (ma_uint64)s->in_count;
        out_n = (ma_uint64)(room < s->out_cap ? room : s->out_cap);
        ma_data_converter_process_pcm_frames(&s->converter, s->in_buf + (size_t)s->in_offset * ch, &in_n,
                                             s->out_buf, &out_n);
        if (out_n > 0) fa_ring_write(&s->ring, s->out_buf, (int64_t)out_n);
        s->in_offset += (fa_frames)in_n;
        s->in_count -= (fa_frames)in_n;
        produced += (int64_t)out_n;
        if (in_n == 0 && out_n == 0) break; /* converter stalled; try again later */
    }
}

/* Source position from the frames the device has consumed. Exact for a fixed
 * rate ratio; the resampler's few frames of delay are not subtracted. */
static fa_frames attached_position(struct fa_stream* s)
{
    fa_frames base = fa_load64(&s->seek_base);
    int64_t played = fa_load64(&s->played);
    fa_frames pos = base + (fa_frames)((double)played * (double)s->decoder.format.sample_rate / (double)s->dev_rate + 0.5);
    if ((s->flags & FA_OPEN_LOOP) && s->decoder.length > 0) pos %= s->decoder.length;
    else if (s->decoder.length >= 0 && pos > s->decoder.length) pos = s->decoder.length;
    return pos;
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
    cc = ma_data_converter_config_init(ma_format_f32, ma_format_f32, ch, FA_DEVICE_CHANNELS,
                                       s->decoder.format.sample_rate, dev->rate);
    if (ma_data_converter_init(&cc, NULL, &s->converter) != MA_SUCCESS) return FA_ERROR_OUT_OF_MEMORY;
    s->converter_ready = 1;
    s->in_cap = FA_DECODE_CHUNK;
    s->out_cap = FA_DECODE_CHUNK * 2;
    s->in_buf = (float*)malloc((size_t)s->in_cap * ch * sizeof(float));
    s->out_buf = (float*)malloc((size_t)s->out_cap * FA_DEVICE_CHANNELS * sizeof(float));
    if (!s->in_buf || !s->out_buf || fa_ring_init(&s->ring, FA_DEVICE_CHANNELS, capacity) != FA_OK) {
        release_chain(s);
        return FA_ERROR_OUT_OF_MEMORY;
    }
    return FA_OK;
}

/* Caller holds api_lock. */
static void detach_locked(struct fa_stream* s)
{
    fa_device* dev = s->device;
    fa_frames pos;
    if (!dev) return;
    fa_device_remove_voice(dev, s);          /* waits until the audio thread let go */
    fa_engine_track_stream(s->engine, s, 0);
    fa_mutex_lock(&s->decode_lock);          /* waits for a worker still filling it */
    pos = attached_position(s);
    s->dev_rate = 0;
    s->device = NULL;
    /* Continue later from what was actually heard, not from what was decoded ahead.
     * A forward-only source cannot go back: the audio buffered ahead is skipped, and
     * the position says where decoding really continues. */
    if (fa_decoder_seek(&s->decoder, pos) != FA_OK && !s->decoder.seekable)
        pos = fa_load64(&s->seek_base) + s->decoded;
    fa_store64(&s->position, pos);
    release_chain(s);
    fa_store32(&s->eof, 0);
    fa_mutex_unlock(&s->decode_lock);
    fa_cas32(&s->state, FA_STATE_PLAYING, FA_STATE_PAUSED);
}

FA_API fa_result FA_CALL fa_stream_attach(fa_stream* stream, fa_device* device, fa_bus* bus)
{
    fa_result r;
    int was_playing;
    if (!stream || !device) return FA_ERROR_INVALID_ARG;
    if (bus) return FA_ERROR_NOT_IMPLEMENTED;
    if (stream->flags & FA_OPEN_DECODE_ONLY) return FA_ERROR_INVALID_ARG;
    if (device->engine != stream->engine) return FA_ERROR_INVALID_ARG; /* events and cleanup are per engine */

    fa_mutex_lock(&stream->api_lock);
    if (stream->device == device) { fa_mutex_unlock(&stream->api_lock); return FA_OK; }
    was_playing = stream->device && fa_load32(&stream->state) == FA_STATE_PLAYING;
    if (stream->device) detach_locked(stream);  /* pauses it */

    fa_mutex_lock(&stream->decode_lock);
    r = build_chain_locked(stream, device);
    if (r == FA_OK) {
        fa_frames pos = fa_load64(&stream->position);
        int p;
        /* Ramp counters are in device frames: keep a running ramp's remaining time
         * when moving to a device with another rate. Nothing mixes this stream now. */
        for (p = 0; p < FA_MIX_PARAMS; p++) {
            fa_param_voice* v = &stream->voice[p];
            if (v->active && stream->voice_rate && stream->voice_rate != device->rate) {
                double k = (double)device->rate / (double)stream->voice_rate;
                v->total = (int64_t)((double)v->total * k + 0.5);
                v->done = (int64_t)((double)v->done * k + 0.5);
            }
        }
        stream->voice_rate = device->rate;
        stream->dev_rate = device->rate;
        fa_store64(&stream->seek_base, pos);
        fa_store64(&stream->played, 0);
        fa_store32(&stream->eof, 0);
        fa_store32(&stream->flushing, 0);
        fa_stream_fill(stream, (int64_t)device->rate * FA_PREFILL_MS / 1000);
    }
    fa_mutex_unlock(&stream->decode_lock);
    if (r == FA_OK) {
        stream->device = device;
        /* Track it first: from the moment the voice is live, events it spills must be found. */
        fa_engine_track_stream(stream->engine, stream, 1);
        r = fa_device_add_voice(device, stream);
        if (r == FA_OK) {
            if (was_playing) fa_cas32(&stream->state, FA_STATE_PAUSED, FA_STATE_PLAYING); /* a move keeps playing */
        } else {
            fa_engine_track_stream(stream->engine, stream, 0);
            stream->device = NULL;
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

FA_API fa_result FA_CALL fa_stream_play(fa_stream* stream)
{
    fa_result r = FA_OK;
    if (!stream) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->api_lock);
    if (!is_attached(stream)) r = FA_ERROR_INVALID_ARG;
    else {
        if (fa_load32(&stream->state) == FA_STATE_ENDED) seek_locked(stream, 0);
        fa_store32(&stream->state, FA_STATE_PLAYING);
    }
    fa_mutex_unlock(&stream->api_lock);
    return r;
}

FA_API fa_result FA_CALL fa_stream_pause(fa_stream* stream)
{
    fa_result r = FA_OK;
    if (!stream) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->api_lock);
    if (!is_attached(stream)) r = FA_ERROR_INVALID_ARG;
    else if (fa_cas32(&stream->state, FA_STATE_PLAYING, FA_STATE_PAUSED))
        fa_device_wait_quiescent(stream->device); /* once we return, nothing more is mixed */
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
        if (stream->decoder.seekable) seek_locked(stream, 0);
        else fa_device_wait_quiescent(stream->device);
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
    if (r == FA_OK) fa_cas32(&stream->state, FA_STATE_ENDED, FA_STATE_PAUSED);
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

static fa_result post_param(struct fa_stream* s, fa_param param, float target, fa_time duration_ns,
                            const fa_ramp* ramp, int notify)
{
    fa_param_mailbox* m = &s->mailbox[param];
    int32_t old, seq;
    fa_param_slot* slot;
    float b[4] = { 0, 0, 0, 0 };
    int i, has_start = 0;
    float start = 0.0f;

    fa_mutex_lock(&s->api_lock);
    old = fa_load32(&m->word);
    seq = ((old & ~FA_CMD_CLAIMED) + 1) & ~FA_CMD_CLAIMED;
    slot = &m->slot[seq & 1];
    if (notify && !(old & FA_CMD_CLAIMED)) {
        /* A ramp replacing a command the mixer has not applied yet: keep the value a
         * plain set asked for, so "volume = 0, then ramp to 1" still fades in from 0.
         * (A plain set replacing one simply takes over, gliding from what is heard.) */
        if (!m->last_notify) { has_start = 1; start = m->last_target; }
        else if (m->last_has_start) { has_start = 1; start = m->last_start; }
    }
    fa_store32(&slot->has_start, has_start);
    fa_store_float(&slot->start, start);
    if (ramp) { b[0] = ramp->bezier_x1; b[1] = ramp->bezier_y1; b[2] = ramp->bezier_x2; b[3] = ramp->bezier_y2; }
    fa_store_float(&slot->target, target);
    fa_store64(&slot->frames_ns, duration_ns);
    fa_store32(&slot->curve, ramp ? (int32_t)ramp->curve : FA_CURVE_LINEAR);
    for (i = 0; i < 4; i++) fa_store_float(&slot->bezier[i], b[i]);
    fa_store64(&slot->tag, ramp ? ramp->tag : 0);
    fa_store32(&slot->notify, notify);

    /* Publish. If the audio thread never claimed the previous command, it never
     * started, so its RAMP_DONE is ours to raise (the audio thread will not). Its
     * sequence number is taken before the new command becomes visible, so it is
     * delivered before anything the new command raises. */
    {
        int32_t prev, done_seq;
        fa_mutex_lock(&s->engine->list_lock);
        done_seq = fa_fetch_add32(&s->engine->event_seq, 1);
        for (;;) {
            prev = fa_load32(&m->word);
            if (fa_cas32(&m->word, prev, seq)) break;
        }
        if (!(prev & FA_CMD_CLAIMED) && m->last_notify) {
            fa_event ev;
            ev.type = FA_EVENT_RAMP_DONE;
            ev.stream = s;
            ev.frame = fa_stream_position_now(s);
            ev.value = m->last_tag;
            fa_engine_post_event_locked(s->engine, &ev, done_seq);
        }
        fa_mutex_unlock(&s->engine->list_lock);
    }
    m->last_notify = notify;
    m->last_tag = ramp ? ramp->tag : 0;
    m->last_target = target;
    m->last_has_start = has_start;
    m->last_start = start;
    fa_mutex_unlock(&s->api_lock);
    return FA_OK;
}

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

FA_API fa_result FA_CALL fa_stream_set_param(fa_stream* stream, fa_param param, float value)
{
    fa_result r;
    if (!stream) return FA_ERROR_INVALID_ARG;
    r = check_param(param, &value);
    if (r != FA_OK) return r;
    return post_param(stream, param, value, FA_DEZIPPER_NS, NULL, 0);
}

FA_API fa_result FA_CALL fa_stream_ramp_param(fa_stream* stream, fa_param param, float target, fa_time duration, fa_time engine_time, const fa_ramp* ramp)
{
    fa_result r;
    if (!stream || duration < 0) return FA_ERROR_INVALID_ARG;
    if (ramp && ((int)ramp->curve < 0 || ramp->curve > FA_CURVE_BEZIER)) return FA_ERROR_INVALID_ARG;
    r = check_param(param, &target);
    if (r != FA_OK) return r;
    if (engine_time != 0) return FA_ERROR_NOT_IMPLEMENTED;
    return post_param(stream, param, target, duration, ramp, 1);
}

FA_API fa_result FA_CALL fa_stream_get_param(fa_stream* stream, fa_param param, float* out_value)
{
    fa_param_mailbox* m;
    float v = 0.0f;
    fa_result r;
    if (!stream || !out_value) return FA_ERROR_INVALID_ARG;
    r = check_param(param, &v);
    if (r != FA_OK) return r;
    m = &stream->mailbox[param];
    fa_mutex_lock(&stream->api_lock);
    /* A plain set that the mixer has not picked up yet reads back as the new value. */
    if (!(fa_load32(&m->word) & FA_CMD_CLAIMED) && !m->last_notify) *out_value = m->last_target;
    else if (!(fa_load32(&m->word) & FA_CMD_CLAIMED) && m->last_has_start) *out_value = m->last_start;
    else *out_value = fa_load_float(&stream->current[param]);
    fa_mutex_unlock(&stream->api_lock);
    return FA_OK;
}

/* ---- markers ------------------------------------------------------------ */

FA_API fa_result FA_CALL fa_stream_set_marker(fa_stream* stream, fa_marker marker, fa_frames frame)
{
    if (!stream || (int)marker < 0 || (int)marker >= FA_MARKER_COUNT) return FA_ERROR_INVALID_ARG;
    if (frame < 0 && frame != FA_FRAMES_UNSET) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->api_lock);
    stream->markers[marker] = frame;
    fa_mutex_unlock(&stream->api_lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_marker(fa_stream* stream, fa_marker marker, fa_frames* out_frame)
{
    if (!stream || !out_frame || (int)marker < 0 || (int)marker >= FA_MARKER_COUNT) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->api_lock);
    *out_frame = stream->markers[marker];
    fa_mutex_unlock(&stream->api_lock);
    return FA_OK;
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

/* Ends a ramp that has run its course (or had zero length) at its target. */
static void finish_ramp(struct fa_stream* s, fa_device* dev, fa_param_voice* v)
{
    v->active = 0;
    v->value = v->target;
    if (v->notify) push_event(dev, s, FA_EVENT_RAMP_DONE, v->tag);
}

/* Takes a new parameter command if one is waiting (see fa_param_mailbox). */
static void take_command(struct fa_stream* s, fa_device* dev, int p, int playing)
{
    fa_param_mailbox* m = &s->mailbox[p];
    fa_param_voice* v = &s->voice[p];
    fa_param_slot* slot;
    int32_t w = fa_load32(&m->word);
    float target, start, b[4];
    int64_t ns, tag;
    int curve, notify, has_start, i;
    if (w & FA_CMD_CLAIMED) return;
    slot = &m->slot[w & 1];
    target = fa_load_float(&slot->target);
    ns = fa_load64(&slot->frames_ns);
    curve = fa_load32(&slot->curve);
    for (i = 0; i < 4; i++) b[i] = fa_load_float(&slot->bezier[i]);
    tag = fa_load64(&slot->tag);
    notify = fa_load32(&slot->notify);
    has_start = fa_load32(&slot->has_start);
    start = fa_load_float(&slot->start);
    if (!fa_cas32(&m->word, w, w | FA_CMD_CLAIMED)) return; /* replaced meanwhile; take it next time */
    if (has_start) v->value = start;

    if (v->active && v->notify) push_event(dev, s, FA_EVENT_RAMP_DONE, v->tag); /* replaced */
    if (!notify && !(playing && s->was_audible)) {
        /* A plain set while nothing was audible applies at once; no glide needed. */
        v->active = 0;
        v->value = target;
        return;
    }
    v->active = 1;
    v->start = v->value;
    v->target = target;
    v->total = (int64_t)((double)ns * dev->rate / 1e9 + 0.5);
    v->done = 0;
    v->curve = curve;
    memcpy(v->bezier, b, sizeof(b));
    v->notify = notify;
    v->tag = tag;
}

/* Value of parameter p after `frames` more frames of the active ramp. */
static float ramp_value(fa_param_voice* v, int64_t at)
{
    float t;
    if (at >= v->total) return v->target;
    t = (float)((double)at / (double)v->total);
    return v->start + (v->target - v->start) * fa_curve_eval(v->curve, v->bezier, t, v->target >= v->start);
}

static void stereo_gains(float vol, float pan, float* gl, float* gr)
{
    *gl = vol * (pan > 0.0f ? 1.0f - pan : 1.0f);
    *gr = vol * (pan < 0.0f ? 1.0f + pan : 1.0f);
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

void fa_stream_mix(struct fa_stream* s, fa_device* dev, float* out, uint32_t frames)
{
    int64_t avail, n, done = 0;
    int p, playing;
    if (fa_load32(&s->flushing)) return;
    playing = fa_load32(&s->state) == FA_STATE_PLAYING;
    for (p = 0; p < FA_MIX_PARAMS; p++) {
        take_command(s, dev, p, playing);
        /* A zero-length ramp is a jump: the first sample already uses the target. */
        if (s->voice[p].active && s->voice[p].done >= s->voice[p].total) finish_ramp(s, dev, &s->voice[p]);
    }
    if (!playing) {
        for (p = 0; p < FA_MIX_PARAMS; p++) fa_store_float(&s->current[p], s->voice[p].value);
        s->was_audible = 0;
        return;
    }
    s->was_audible = 1;

    avail = fa_ring_readable(&s->ring);
    n = avail < (int64_t)frames ? avail : (int64_t)frames;

    while (done < n) {
        /* Blocks of FA_RAMP_BLOCK frames: gains are exact at block edges and
         * interpolated linearly inside, so ramps and changes are click-free. */
        int64_t blk = n - done < FA_RAMP_BLOCK ? n - done : FA_RAMP_BLOCK;
        float v0[FA_MIX_PARAMS], v1[FA_MIX_PARAMS], l0, r0, l1, r1;
        const float *p1, *p2;
        int64_t n1, n2;
        for (p = 0; p < FA_MIX_PARAMS; p++) {   /* end blocks exactly where ramps end */
            fa_param_voice* v = &s->voice[p];
            if (v->active && v->total - v->done > 0 && v->total - v->done < blk) blk = v->total - v->done;
        }
        for (p = 0; p < FA_MIX_PARAMS; p++) {
            fa_param_voice* v = &s->voice[p];
            v0[p] = v->value;
            v1[p] = v->active ? ramp_value(v, v->done + blk) : v->value;
        }
        stereo_gains(v0[FA_PARAM_VOLUME], v0[FA_PARAM_PAN], &l0, &r0);
        stereo_gains(v1[FA_PARAM_VOLUME], v1[FA_PARAM_PAN], &l1, &r1);
        fa_ring_regions(&s->ring, blk, &p1, &n1, &p2, &n2);
        mix_region(out + done * 2, p1, n1, l0, r0, l0 + (l1 - l0) * (float)n1 / (float)blk, r0 + (r1 - r0) * (float)n1 / (float)blk);
        if (n2 > 0) mix_region(out + (done + n1) * 2, p2, n2, l0 + (l1 - l0) * (float)n1 / (float)blk,
                               r0 + (r1 - r0) * (float)n1 / (float)blk, l1, r1);
        fa_ring_advance_read(&s->ring, blk);
        fa_fetch_add64(&s->played, blk);
        done += blk;

        for (p = 0; p < FA_MIX_PARAMS; p++) {
            fa_param_voice* v = &s->voice[p];
            v->value = v1[p];
            if (!v->active) continue;
            v->done += blk;
            if (v->done >= v->total) finish_ramp(s, dev, v);
        }
    }
    for (p = 0; p < FA_MIX_PARAMS; p++) fa_store_float(&s->current[p], s->voice[p].value);

    if (n < (int64_t)frames) {
        if (fa_load32(&s->eof) && fa_ring_readable(&s->ring) == 0) {
            if (fa_cas32(&s->state, FA_STATE_PLAYING, FA_STATE_ENDED)) push_event(dev, s, FA_EVENT_ENDED, 0);
        } else {
            fa_fetch_add32(&s->underruns, 1);
        }
    }
}
