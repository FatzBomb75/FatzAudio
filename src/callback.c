/* Callback streams (fa_stream_open_callback): the decoder asks the app's
 * fa_stream_proc for audio whenever the stream wants more, like BASS's STREAMPROC.
 *
 * The proc runs where any decoder runs: on a decode worker (or the thread calling
 * fa_stream_read for decode-only streams), under the stream's decode_lock, so it is
 * never called twice at once nor after fa_stream_close. When it has nothing right
 * now (returns 0), `ready` keeps the workers away for FA_CALLBACK_RETRY_NS. */
#include "fa_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FA_CALLBACK_RETRY_NS 5000000   /* 5 ms, the polling worker's shortest sleep */

typedef struct callback_src {
    fa_stream_proc proc;
    void* user;
    int kind;                /* FA_SAMPLE_* of the app's frames */
    size_t frame_bytes;
    uint8_t* scratch;        /* the app's frames when they are not F32 */
    fa_frames scratch_frames;
    int ended;               /* the proc returned FA_STREAM_PROC_END */
    fa_atomic64 retry_at;    /* monotonic ns: the proc had nothing, ask again from then */
} callback_src;

static int callback_probe(const uint8_t* data, size_t size, size_t id3_size)
{
    (void)data; (void)size; (void)id3_size;
    return 0;   /* never detected from content */
}

static fa_result callback_open(fa_decoder* dec) { (void)dec; return FA_ERROR_UNSUPPORTED_FORMAT; }

static fa_result callback_read(fa_decoder* dec, float* out, fa_frames frames, fa_frames* got)
{
    callback_src* c = (callback_src*)dec->impl;
    uint32_t ch = dec->format.channels;
    void* buf = out;
    int64_t n;
    *got = 0;
    if (c->ended) return FA_ERROR_END_OF_STREAM;
    if (c->kind != FA_SAMPLE_F32) {
        if (frames > c->scratch_frames) {
            uint8_t* p = (uint8_t*)realloc(c->scratch, (size_t)frames * c->frame_bytes);
            if (!p) return FA_ERROR_OUT_OF_MEMORY;
            c->scratch = p;
            c->scratch_frames = frames;
        }
        buf = c->scratch;
    }
    n = c->proc(buf, frames, c->user);
    if (n < 0) {
        c->ended = 1;
        return FA_ERROR_END_OF_STREAM;
    }
    if (n == 0) {
        fa_store64(&c->retry_at, fa_monotonic_ns() + FA_CALLBACK_RETRY_NS);
        return FA_ERROR_BUSY;   /* nothing now: not the end */
    }
    if (n > frames) n = frames;
    if (c->kind == FA_SAMPLE_F32) {
        fa_zero_non_finite(out, (size_t)n * ch);
    } else {
        const void* planes[1];
        planes[0] = c->scratch;
        fa_convert_to_f32(planes, c->kind, 0, 0, ch, n, out);
    }
    *got = n;
    return FA_OK;
}

static fa_result callback_seek(fa_decoder* dec, fa_frames frame)
{
    (void)dec; (void)frame;
    return FA_ERROR_NOT_SEEKABLE;
}

static void callback_close(fa_decoder* dec)
{
    callback_src* c = (callback_src*)dec->impl;
    if (!c) return;
    free(c->scratch);
    free(c);
    dec->impl = NULL;
}

static int callback_ready(fa_decoder* dec)
{
    callback_src* c = (callback_src*)dec->impl;
    return fa_monotonic_ns() >= fa_load64(&c->retry_at);
}

static const fa_decoder_vtable callback_vtable = {
    "callback", "App callback (fa_stream_proc)", "", "",
    callback_probe, callback_open, callback_read, callback_seek, callback_close, 0, NULL, NULL
};

fa_result fa_callback_decoder_open(fa_decoder* dec, const fa_audio_format* format, fa_stream_proc proc, void* user)
{
    callback_src* c;
    int planar, kind;
    if (!format || !proc) return FA_ERROR_INVALID_ARG;
    kind = fa_sample_kind(format->sample_format, &planar);
    if (kind < 0 || planar || format->sample_rate < 1000 || format->sample_rate > 768000 ||
        format->channels == 0 || format->channels > 32)
        return FA_ERROR_INVALID_ARG;
    c = (callback_src*)calloc(1, sizeof(*c));
    if (!c) return FA_ERROR_OUT_OF_MEMORY;
    c->proc = proc;
    c->user = user;
    c->kind = kind;
    c->frame_bytes = (size_t)fa_sample_bytes(kind) * format->channels;
    memset(dec, 0, sizeof(*dec));
    dec->vt = &callback_vtable;
    dec->impl = c;
    dec->format.sample_format = FA_FORMAT_F32;
    dec->format.sample_rate = format->sample_rate;
    dec->format.channels = format->channels;
    dec->format.channel_mask = format->channel_mask;
    dec->length = -1;
    dec->seekable = 0;
    dec->ready = callback_ready;
    dec->anchor_pts = FA_PTS_NONE;
    snprintf(dec->codec, sizeof(dec->codec), "pcm");
    snprintf(dec->container, sizeof(dec->container), "callback");
    return FA_OK;
}
