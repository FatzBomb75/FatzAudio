#include "fa_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FA_MIX_CHUNK 512

FA_API void FA_CALL fa_open_options_init(fa_open_options* options)
{
    if (!options) return;
    memset(options, 0, sizeof(*options));
    options->struct_size = (uint32_t)sizeof(*options);
    options->audio_track = -1;
}

/* ---- open / close ------------------------------------------------------- */

static void stream_free(struct fa_stream* s)
{
    if (s->converter_ready) ma_data_converter_uninit(&s->converter, NULL);
    if (s->decoder_ready) fa_decoder_close(&s->decoder);
    fa_source_close(&s->source); /* calls the app's io close exactly once */
    ma_mutex_uninit(&s->lock);
    free(s->in_buf);
    free(s);
}

static fa_result stream_create(fa_engine* engine, const fa_open_options* options, struct fa_stream** out)
{
    int i;
    struct fa_stream* s = (struct fa_stream*)calloc(1, sizeof(*s));
    if (!s) return FA_ERROR_OUT_OF_MEMORY;
    if (ma_mutex_init(&s->lock) != MA_SUCCESS) { free(s); return FA_ERROR_GENERIC; }
    s->engine = engine;
    s->flags = options ? options->flags : 0;
    s->volume = 1.0f;
    s->pan = 0.0f;
    s->state = FA_STATE_STOPPED;
    for (i = 0; i < FA_MARKER_COUNT; i++) s->markers[i] = FA_FRAMES_UNSET;
    *out = s;
    return FA_OK;
}

/* Detects the format and opens the decoder. On failure the source is NOT closed
 * here, so open_io can hand ownership back to the caller untouched. */
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
    if (r != FA_OK) { stream_free(s); return r; }
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
    if (r != FA_OK) { stream_free(s); return r; }
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
        stream_free(s);
        return r;
    }
    *out_stream = s;
    return FA_OK;
}

static void detach_locked(struct fa_stream* s);

FA_API void FA_CALL fa_stream_close(fa_stream* stream)
{
    if (!stream) return;
    fa_stream_detach(stream);
    stream_free(stream);
}

/* ---- info --------------------------------------------------------------- */

FA_API fa_result FA_CALL fa_stream_get_info(fa_stream* stream, fa_stream_info* out_info)
{
    const fa_decoder* d;
    if (!stream || !out_info) return FA_ERROR_INVALID_ARG;
    memset(out_info, 0, sizeof(*out_info));
    ma_mutex_lock(&stream->lock);
    d = &stream->decoder;
    out_info->format = d->format;
    snprintf(out_info->codec, sizeof(out_info->codec), "%s", d->codec);
    snprintf(out_info->container, sizeof(out_info->container), "%s", d->container);
    out_info->length_frames = d->length >= 0 ? d->length : -1;
    out_info->duration = (d->length >= 0 && d->format.sample_rate > 0)
        ? (fa_time)((double)d->length * 1e9 / (double)d->format.sample_rate + 0.5) : -1;
    out_info->seekable = d->seekable;
    out_info->audio_track_count = 1;
    out_info->has_cdg = 0;
    ma_mutex_unlock(&stream->lock);
    return FA_OK;
}

/* ---- attach / transport ------------------------------------------------- */

/* Resets the voice chain (converter state, pending input) after a seek or attach. */
static void reset_chain_locked(struct fa_stream* s)
{
    s->in_count = s->in_offset = 0;
    if (s->converter_ready) ma_data_converter_reset(&s->converter);
}

static void unlink_from_device(struct fa_stream* s, fa_device* dev)
{
    struct fa_stream** pp;
    for (pp = &dev->streams; *pp; pp = &(*pp)->next) {
        if (*pp == s) { *pp = s->next; break; }
    }
    s->next = NULL;
}

/* Caller holds the device lock and the stream lock. */
static void detach_locked(struct fa_stream* s)
{
    unlink_from_device(s, s->device);
    s->device = NULL;
    if (s->converter_ready) { ma_data_converter_uninit(&s->converter, NULL); s->converter_ready = 0; }
    /* Frames decoded but not yet played are dropped; continue from the played position. */
    if (s->in_count > 0) fa_decoder_seek(&s->decoder, s->position);
    s->in_count = s->in_offset = 0;
    if (s->state == FA_STATE_PLAYING) s->state = FA_STATE_PAUSED;
}

FA_API fa_result FA_CALL fa_stream_attach(fa_stream* stream, fa_device* device, fa_bus* bus)
{
    ma_data_converter_config cc;
    if (!stream || !device) return FA_ERROR_INVALID_ARG;
    if (bus) return FA_ERROR_NOT_IMPLEMENTED;
    if (stream->flags & FA_OPEN_DECODE_ONLY) return FA_ERROR_INVALID_ARG;
    if (stream->device == device) return FA_OK;
    if (stream->device) fa_stream_detach(stream);

    if (!stream->in_buf) {
        stream->in_cap = FA_MIX_CHUNK;
        stream->in_buf = (float*)malloc((size_t)stream->in_cap * stream->decoder.format.channels * sizeof(float));
        if (!stream->in_buf) return FA_ERROR_OUT_OF_MEMORY;
    }

    ma_mutex_lock(&device->lock);
    ma_mutex_lock(&stream->lock);
    cc = ma_data_converter_config_init(ma_format_f32, ma_format_f32,
                                       stream->decoder.format.channels, FA_DEVICE_CHANNELS,
                                       stream->decoder.format.sample_rate, device->device.sampleRate);
    if (ma_data_converter_init(&cc, NULL, &stream->converter) != MA_SUCCESS) {
        ma_mutex_unlock(&stream->lock);
        ma_mutex_unlock(&device->lock);
        return FA_ERROR_OUT_OF_MEMORY;
    }
    stream->converter_ready = 1;
    reset_chain_locked(stream);
    stream->device = device;
    stream->next = device->streams;
    device->streams = stream;
    ma_mutex_unlock(&stream->lock);
    ma_mutex_unlock(&device->lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_detach(fa_stream* stream)
{
    fa_device* dev;
    if (!stream) return FA_ERROR_INVALID_ARG;
    dev = stream->device;
    if (!dev) return FA_OK;
    ma_mutex_lock(&dev->lock);
    ma_mutex_lock(&stream->lock);
    detach_locked(stream);
    ma_mutex_unlock(&stream->lock);
    ma_mutex_unlock(&dev->lock);
    return FA_OK;
}

static fa_result require_attached(struct fa_stream* s)
{
    if (!s) return FA_ERROR_INVALID_ARG;
    if (!s->device) return FA_ERROR_INVALID_ARG;
    return FA_OK;
}

static fa_result seek_locked(struct fa_stream* s, fa_frames frame)
{
    fa_result r = fa_decoder_seek(&s->decoder, frame);
    if (r != FA_OK) return r;
    s->position = frame;
    reset_chain_locked(s);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_play(fa_stream* stream)
{
    fa_result r = require_attached(stream);
    if (r != FA_OK) return r;
    ma_mutex_lock(&stream->lock);
    if (stream->state == FA_STATE_ENDED) seek_locked(stream, 0);
    stream->state = FA_STATE_PLAYING;
    ma_mutex_unlock(&stream->lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_pause(fa_stream* stream)
{
    fa_result r = require_attached(stream);
    if (r != FA_OK) return r;
    ma_mutex_lock(&stream->lock);
    if (stream->state == FA_STATE_PLAYING) stream->state = FA_STATE_PAUSED;
    ma_mutex_unlock(&stream->lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_stop(fa_stream* stream)
{
    fa_result r = require_attached(stream);
    if (r != FA_OK) return r;
    ma_mutex_lock(&stream->lock);
    stream->state = FA_STATE_STOPPED;
    if (stream->decoder.seekable) seek_locked(stream, 0);
    ma_mutex_unlock(&stream->lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_state(fa_stream* stream, fa_stream_state* out_state)
{
    if (!stream || !out_state) return FA_ERROR_INVALID_ARG;
    ma_mutex_lock(&stream->lock);
    *out_state = stream->state;
    ma_mutex_unlock(&stream->lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_seek(fa_stream* stream, fa_frames frame)
{
    fa_result r;
    if (!stream || frame < 0) return FA_ERROR_INVALID_ARG;
    ma_mutex_lock(&stream->lock);
    r = seek_locked(stream, frame);
    if (r == FA_OK && stream->state == FA_STATE_ENDED) stream->state = FA_STATE_PAUSED;
    ma_mutex_unlock(&stream->lock);
    return r;
}

FA_API fa_result FA_CALL fa_stream_get_position(fa_stream* stream, fa_frames* out_frame)
{
    if (!stream || !out_frame) return FA_ERROR_INVALID_ARG;
    ma_mutex_lock(&stream->lock);
    *out_frame = stream->position;
    ma_mutex_unlock(&stream->lock);
    return FA_OK;
}

/* ---- parameters --------------------------------------------------------- */

FA_API fa_result FA_CALL fa_stream_set_param(fa_stream* stream, fa_param param, float value)
{
    if (!stream) return FA_ERROR_INVALID_ARG;
    if (param == FA_PARAM_VOLUME) {
        if (!(value >= 0.0f)) return FA_ERROR_INVALID_ARG;
        ma_mutex_lock(&stream->lock); stream->volume = value; ma_mutex_unlock(&stream->lock);
        return FA_OK;
    }
    if (param == FA_PARAM_PAN) {
        if (value != value) return FA_ERROR_INVALID_ARG;
        if (value < -1.0f) value = -1.0f;
        if (value > 1.0f) value = 1.0f;
        ma_mutex_lock(&stream->lock); stream->pan = value; ma_mutex_unlock(&stream->lock);
        return FA_OK;
    }
    return FA_ERROR_NOT_IMPLEMENTED;
}

FA_API fa_result FA_CALL fa_stream_get_param(fa_stream* stream, fa_param param, float* out_value)
{
    if (!stream || !out_value) return FA_ERROR_INVALID_ARG;
    if (param == FA_PARAM_VOLUME) {
        ma_mutex_lock(&stream->lock); *out_value = stream->volume; ma_mutex_unlock(&stream->lock);
        return FA_OK;
    }
    if (param == FA_PARAM_PAN) {
        ma_mutex_lock(&stream->lock); *out_value = stream->pan; ma_mutex_unlock(&stream->lock);
        return FA_OK;
    }
    return FA_ERROR_NOT_IMPLEMENTED;
}

/* ---- markers ------------------------------------------------------------ */

FA_API fa_result FA_CALL fa_stream_set_marker(fa_stream* stream, fa_marker marker, fa_frames frame)
{
    if (!stream || (int)marker < 0 || (int)marker >= FA_MARKER_COUNT) return FA_ERROR_INVALID_ARG;
    if (frame < 0 && frame != FA_FRAMES_UNSET) return FA_ERROR_INVALID_ARG;
    ma_mutex_lock(&stream->lock);
    stream->markers[marker] = frame;
    ma_mutex_unlock(&stream->lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_marker(fa_stream* stream, fa_marker marker, fa_frames* out_frame)
{
    if (!stream || !out_frame || (int)marker < 0 || (int)marker >= FA_MARKER_COUNT) return FA_ERROR_INVALID_ARG;
    ma_mutex_lock(&stream->lock);
    *out_frame = stream->markers[marker];
    ma_mutex_unlock(&stream->lock);
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
    ma_mutex_lock(&stream->lock);
    r = fa_decoder_read(&stream->decoder, (float*)buffer, frames, &got);
    stream->position += got;
    ma_mutex_unlock(&stream->lock);
    *out_frames_read = got;
    return r;
}

/* ---- mixing (audio thread) ---------------------------------------------- */

void fa_stream_mix_into(struct fa_stream* s, float* out, ma_uint32 frames)
{
    float tmp[FA_MIX_CHUNK * FA_DEVICE_CHANNELS];
    ma_uint32 done = 0;
    int just_looped = 0;
    uint32_t ch;

    ma_mutex_lock(&s->lock);
    ch = s->decoder.format.channels;
    if (s->state == FA_STATE_PLAYING && s->converter_ready) {
        float gl = s->volume, gr = s->volume;
        if (s->pan > 0.0f) gl *= 1.0f - s->pan;
        else if (s->pan < 0.0f) gr *= 1.0f + s->pan;

        while (done < frames) {
            ma_uint64 in_n, out_n;
            ma_uint32 i;
            if (s->in_count == 0) {
                fa_frames got = 0;
                fa_decoder_read(&s->decoder, s->in_buf, s->in_cap, &got);
                s->in_offset = 0;
                s->in_count = got;
                if (got == 0) {
                    if ((s->flags & FA_OPEN_LOOP) && !just_looped && seek_locked(s, 0) == FA_OK) {
                        just_looped = 1;
                        continue;
                    }
                    s->state = FA_STATE_ENDED;
                    break;
                }
                just_looped = 0;
            }
            in_n = (ma_uint64)s->in_count;
            out_n = frames - done;
            if (out_n > FA_MIX_CHUNK) out_n = FA_MIX_CHUNK;
            ma_data_converter_process_pcm_frames(&s->converter, s->in_buf + s->in_offset * ch, &in_n, tmp, &out_n);
            s->in_offset += (fa_frames)in_n;
            s->in_count -= (fa_frames)in_n;
            s->position += (fa_frames)in_n;
            for (i = 0; i < (ma_uint32)out_n; i++) {
                out[(done + i) * 2 + 0] += tmp[i * 2 + 0] * gl;
                out[(done + i) * 2 + 1] += tmp[i * 2 + 1] * gr;
            }
            done += (ma_uint32)out_n;
            if (in_n == 0 && out_n == 0) break; /* converter made no progress; avoid spinning */
        }
    }
    ma_mutex_unlock(&s->lock);
}
