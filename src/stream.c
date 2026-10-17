#include "fa_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

FA_API void FA_CALL fa_open_options_init(fa_open_options* options)
{
    if (!options) return;
    memset(options, 0, sizeof(*options));
    options->struct_size = (uint32_t)sizeof(*options);
    options->audio_track = -1;
}

/* ---- decoder helpers ---------------------------------------------------- */

static void sniff(const uint8_t* d, size_t n, char* codec, char* container, size_t cap)
{
    codec[0] = container[0] = '\0';
    if (n >= 4 && memcmp(d, "RIFF", 4) == 0) { snprintf(codec, cap, "pcm"); snprintf(container, cap, "wav"); }
    else if (n >= 4 && memcmp(d, "fLaC", 4) == 0) { snprintf(codec, cap, "flac"); snprintf(container, cap, "flac"); }
    else if (n >= 3 && (memcmp(d, "ID3", 3) == 0 || (d[0] == 0xFF && (d[1] & 0xE0) == 0xE0))) {
        snprintf(codec, cap, "mp3"); snprintf(container, cap, "mp3");
    }
}

/* Initialises s->decoder from owned memory or path with the given output layout (0 = native). */
static fa_result init_decoder(struct fa_stream* s, ma_uint32 channels, ma_uint32 rate)
{
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, channels, rate);
    ma_result r;
    if (s->memory) {
        r = ma_decoder_init_memory(s->memory, s->memory_size, &cfg, &s->decoder);
    } else {
#ifdef _WIN32
        int wn = MultiByteToWideChar(CP_UTF8, 0, s->path, -1, NULL, 0);
        wchar_t* w;
        if (wn <= 0) return FA_ERROR_INVALID_ARG;
        w = (wchar_t*)malloc((size_t)wn * sizeof(wchar_t));
        if (!w) return FA_ERROR_OUT_OF_MEMORY;
        MultiByteToWideChar(CP_UTF8, 0, s->path, -1, w, wn);
        r = ma_decoder_init_file_w(w, &cfg, &s->decoder);
        free(w);
#else
        r = ma_decoder_init_file(s->path, &cfg, &s->decoder);
#endif
    }
    if (r == MA_SUCCESS) { s->decoder_ready = 1; return FA_OK; }
    if (r == MA_DOES_NOT_EXIST || r == MA_ACCESS_DENIED) return FA_ERROR_NOT_FOUND;
    if (r == MA_OUT_OF_MEMORY) return FA_ERROR_OUT_OF_MEMORY;
    if (r == MA_NO_BACKEND || r == MA_INVALID_FILE) return FA_ERROR_UNSUPPORTED_FORMAT;
    return FA_ERROR_IO;
}

static void stream_free(struct fa_stream* s)
{
    if (s->decoder_ready) ma_decoder_uninit(&s->decoder);
    ma_mutex_uninit(&s->lock);
    free(s->memory);
    free(s->path);
    free(s);
}

static fa_result stream_create(fa_engine* engine, const fa_open_options* options, struct fa_stream** out)
{
    struct fa_stream* s = (struct fa_stream*)calloc(1, sizeof(*s));
    if (!s) return FA_ERROR_OUT_OF_MEMORY;
    if (ma_mutex_init(&s->lock) != MA_SUCCESS) { free(s); return FA_ERROR_GENERIC; }
    s->engine = engine;
    s->flags = options ? options->flags : 0;
    s->volume = 1.0f;
    s->pan = 0.0f;
    s->state = FA_STATE_STOPPED;
    *out = s;
    return FA_OK;
}

static fa_result stream_finish_open(struct fa_stream* s, struct fa_stream** out_stream)
{
    fa_result r = init_decoder(s, 0, 0);
    if (r != FA_OK) { stream_free(s); return r; }
    *out_stream = s;
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_open_path(fa_engine* engine, const char* path, const fa_open_options* options, fa_stream** out_stream)
{
    struct fa_stream* s; fa_result r;
    if (!engine || !path || !out_stream) return FA_ERROR_INVALID_ARG;
    *out_stream = NULL;
    r = stream_create(engine, options, &s);
    if (r != FA_OK) return r;
    s->path = (char*)malloc(strlen(path) + 1);
    if (!s->path) { stream_free(s); return FA_ERROR_OUT_OF_MEMORY; }
    memcpy(s->path, path, strlen(path) + 1);
    {
        FILE* f = fopen(path, "rb");
        if (f) {
            uint8_t head[4]; size_t n = fread(head, 1, 4, f);
            sniff(head, n, s->codec, s->container, sizeof(s->codec));
            fclose(f);
        }
    }
    return stream_finish_open(s, out_stream);
}

FA_API fa_result FA_CALL fa_stream_open_memory(fa_engine* engine, const void* data, size_t size, const fa_open_options* options, fa_stream** out_stream)
{
    struct fa_stream* s; fa_result r;
    if (!engine || !data || size == 0 || !out_stream) return FA_ERROR_INVALID_ARG;
    *out_stream = NULL;
    r = stream_create(engine, options, &s);
    if (r != FA_OK) return r;
    s->memory = (uint8_t*)malloc(size);
    if (!s->memory) { stream_free(s); return FA_ERROR_OUT_OF_MEMORY; }
    memcpy(s->memory, data, size);
    s->memory_size = size;
    sniff(s->memory, size, s->codec, s->container, sizeof(s->codec));
    return stream_finish_open(s, out_stream);
}

FA_API void FA_CALL fa_stream_close(fa_stream* stream)
{
    fa_device* dev;
    if (!stream) return;
    dev = stream->device;
    if (dev) {
        struct fa_stream** pp;
        ma_mutex_lock(&dev->lock);
        for (pp = &dev->streams; *pp; pp = &(*pp)->next) {
            if (*pp == stream) { *pp = stream->next; break; }
        }
        ma_mutex_unlock(&dev->lock);
    }
    stream_free(stream);
}

/* ---- info --------------------------------------------------------------- */

FA_API fa_result FA_CALL fa_stream_get_info(fa_stream* stream, fa_stream_info* out_info)
{
    ma_uint64 len = 0;
    if (!stream || !out_info) return FA_ERROR_INVALID_ARG;
    memset(out_info, 0, sizeof(*out_info));
    ma_mutex_lock(&stream->lock);
    out_info->format.sample_format = FA_FORMAT_F32;
    out_info->format.sample_rate = stream->decoder.outputSampleRate;
    out_info->format.channels = stream->decoder.outputChannels;
    snprintf(out_info->codec, sizeof(out_info->codec), "%s", stream->codec);
    snprintf(out_info->container, sizeof(out_info->container), "%s", stream->container);
    if (ma_decoder_get_length_in_pcm_frames(&stream->decoder, &len) == MA_SUCCESS && len > 0)
        out_info->length_frames = (fa_frames)len;
    else
        out_info->length_frames = -1;
    out_info->seekable = 1;
    out_info->audio_track_count = 1;
    out_info->has_cdg = 0;
    ma_mutex_unlock(&stream->lock);
    return FA_OK;
}

/* ---- attach / transport ------------------------------------------------- */

FA_API fa_result FA_CALL fa_stream_attach(fa_stream* stream, fa_device* device, fa_bus* bus)
{
    fa_result r;
    if (!stream || !device) return FA_ERROR_INVALID_ARG;
    if (bus) return FA_ERROR_NOT_IMPLEMENTED;
    if (stream->flags & FA_OPEN_DECODE_ONLY) return FA_ERROR_INVALID_ARG;
    if (stream->device) return FA_ERROR_BUSY;

    ma_mutex_lock(&device->lock);
    ma_mutex_lock(&stream->lock);
    /* Re-open the decoder converted to the device layout (stereo f32 at the device rate).
     * TODO: preserve the position across attach; for now it restarts at frame 0. */
    ma_decoder_uninit(&stream->decoder);
    stream->decoder_ready = 0;
    r = init_decoder(stream, FA_DEVICE_CHANNELS, device->device.sampleRate);
    if (r == FA_OK) {
        stream->device = device;
        stream->state = FA_STATE_STOPPED;
        stream->next = device->streams;
        device->streams = stream;
    }
    ma_mutex_unlock(&stream->lock);
    ma_mutex_unlock(&device->lock);
    return r;
}

static fa_result require_attached(struct fa_stream* s)
{
    if (!s) return FA_ERROR_INVALID_ARG;
    if (!s->device) return FA_ERROR_INVALID_ARG;
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_play(fa_stream* stream)
{
    fa_result r = require_attached(stream);
    if (r != FA_OK) return r;
    ma_mutex_lock(&stream->lock);
    if (stream->state == FA_STATE_ENDED) ma_decoder_seek_to_pcm_frame(&stream->decoder, 0);
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
    ma_decoder_seek_to_pcm_frame(&stream->decoder, 0);
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
    ma_result r;
    if (!stream || frame < 0) return FA_ERROR_INVALID_ARG;
    ma_mutex_lock(&stream->lock);
    r = ma_decoder_seek_to_pcm_frame(&stream->decoder, (ma_uint64)frame);
    if (r == MA_SUCCESS && stream->state == FA_STATE_ENDED) stream->state = FA_STATE_PAUSED;
    ma_mutex_unlock(&stream->lock);
    return r == MA_SUCCESS ? FA_OK : FA_ERROR_NOT_SEEKABLE;
}

FA_API fa_result FA_CALL fa_stream_get_position(fa_stream* stream, fa_frames* out_frame)
{
    ma_uint64 cur = 0;
    if (!stream || !out_frame) return FA_ERROR_INVALID_ARG;
    ma_mutex_lock(&stream->lock);
    ma_decoder_get_cursor_in_pcm_frames(&stream->decoder, &cur);
    ma_mutex_unlock(&stream->lock);
    *out_frame = (fa_frames)cur;
    return FA_OK;
}

/* ---- parameters --------------------------------------------------------- */

FA_API fa_result FA_CALL fa_stream_set_param(fa_stream* stream, fa_param param, float value)
{
    if (!stream) return FA_ERROR_INVALID_ARG;
    if (param == FA_PARAM_VOLUME) {
        if (value < 0.0f) return FA_ERROR_INVALID_ARG;
        ma_mutex_lock(&stream->lock); stream->volume = value; ma_mutex_unlock(&stream->lock);
        return FA_OK;
    }
    if (param == FA_PARAM_PAN) {
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

/* ---- decode-only read --------------------------------------------------- */

FA_API fa_result FA_CALL fa_stream_read(fa_stream* stream, void* buffer, fa_frames frames, fa_frames* out_frames_read)
{
    ma_uint64 got = 0;
    ma_result r;
    if (!stream || !buffer || frames < 0 || !out_frames_read) return FA_ERROR_INVALID_ARG;
    *out_frames_read = 0;
    if (stream->device) return FA_ERROR_BUSY; /* attached streams are pulled by the device */
    if (frames == 0) return FA_OK;
    ma_mutex_lock(&stream->lock);
    r = ma_decoder_read_pcm_frames(&stream->decoder, buffer, (ma_uint64)frames, &got);
    ma_mutex_unlock(&stream->lock);
    *out_frames_read = (fa_frames)got;
    if (got == 0) return (r == MA_AT_END || r == MA_SUCCESS) ? FA_ERROR_END_OF_STREAM : FA_ERROR_IO;
    return FA_OK;
}

/* ---- mixing (audio thread) ---------------------------------------------- */

void fa_stream_mix_into(struct fa_stream* s, float* out, ma_uint32 frames)
{
    float tmp[512 * FA_DEVICE_CHANNELS];
    ma_uint32 done = 0;
    int just_looped = 0;

    ma_mutex_lock(&s->lock);
    if (s->state == FA_STATE_PLAYING) {
        float gl = s->volume, gr = s->volume;
        if (s->pan > 0.0f) gl *= 1.0f - s->pan;
        else if (s->pan < 0.0f) gr *= 1.0f + s->pan;

        while (done < frames) {
            ma_uint32 chunk = frames - done;
            ma_uint64 got = 0;
            ma_uint32 i;
            if (chunk > 512) chunk = 512;
            ma_decoder_read_pcm_frames(&s->decoder, tmp, chunk, &got);
            for (i = 0; i < (ma_uint32)got; i++) {
                out[(done + i) * 2 + 0] += tmp[i * 2 + 0] * gl;
                out[(done + i) * 2 + 1] += tmp[i * 2 + 1] * gr;
            }
            done += (ma_uint32)got;
            if (got < chunk) {
                if ((s->flags & FA_OPEN_LOOP) && !(got == 0 && just_looped)) {
                    ma_decoder_seek_to_pcm_frame(&s->decoder, 0);
                    just_looped = (got == 0);
                    continue;
                }
                s->state = FA_STATE_ENDED;
                break;
            }
            just_looped = 0;
        }
    }
    ma_mutex_unlock(&s->lock);
}
