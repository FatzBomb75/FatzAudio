/* Push streams: the app hands over PCM frames or compressed packets; the decode
 * workers turn them into the stream's format through the push decoder below,
 * which the rest of the engine treats like any other decoder.
 *
 * Threads
 * - API threads push into the queue under push->lock (copying the data, so the
 *   app's buffers are free when the call returns).
 * - The decode worker (or fa_stream_read) runs the push decoder under the
 *   stream's decode_lock: it pops items, decodes packets, converts to the stream's
 *   rate and channel count, and tracks the timestamps.
 * - The timeline: every chunk the decoder hands out either continues the previous
 *   one or starts a new anchor (dec->anchor_pts) at the app's pts, when the
 *   timestamps jump by more than FA_PUSH_JITTER_NS. fa_stream_fill records anchors
 *   in the stream's pts map, which the mixer turns into the presentation clock. */
#include "fa_internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FA_PUSH_JITTER_NS 15000000     /* timestamp differences below 15 ms are jitter */
#define FA_PUSH_MAX_BYTES (256u << 20) /* a runaway producer gets FA_ERROR_BUSY */

typedef struct fa_push_item {
    struct fa_push_item* next;
    int is_packet;
    int64_t pts;                 /* ns, FA_PTS_NONE */
    uint32_t rate, channels;     /* PCM items: F32 interleaved */
    fa_frames frames;
    int64_t dur_ns;              /* counted in queued_ns: the packet's duration or a guess */
    int dur_known;               /* dur_ns was given, not guessed */
    size_t size;                 /* bytes of data */
    /* data follows */
} fa_push_item;

struct fa_push {
    struct fa_stream* stream;
    fa_mutex lock;               /* the queue and the push-side bookkeeping */
    fa_push_item *head, *tail;
    size_t bytes;
    fa_atomic32 count;           /* items queued */
    fa_atomic32 ended;           /* fa_stream_push_end */
    fa_atomic32 has_buffered;    /* decoded frames waiting in buf (worker) */
    int packets;                 /* packet stream (else PCM) */
    fa_audio_format pcm_format;  /* fa_stream_push_pcm's format */
    fa_rational tb;              /* packets: the track's time base */
    int64_t queued_ns;           /* audio in the queue (fa_stream_get_buffered) */
    int64_t last_dur;            /* duration of the latest packet (ns), the guess for one without */

    /* Worker side, under the stream's decode_lock. */
    fa_codec codec;
    int codec_ready;
    uint32_t rate, channels;     /* the stream's format */
    ma_data_converter conv;
    int conv_ready;
    uint32_t conv_rate, conv_channels;
    float* buf;                  /* converted frames not handed out yet */
    fa_frames buf_cap, buf_count, buf_off;
    int64_t pending_anchor;      /* the next frame handed out starts the timeline here (ns), FA_PTS_NONE = it continues */
    int64_t next_pts;            /* pts of the next frame handed out */
    int need_anchor;             /* the next chunk starts the timeline (first one, after a flush) */
    int hold_pending;            /* ... and sets the clock's hold value */
    fa_atomic64 out_pts;         /* next_pts, readable by any thread */
    fa_atomic64 out_buffered;    /* frames in buf, readable by any thread */
    int drained;                 /* the codec was told the input ended and gave back the rest */
};

static int64_t to_ns(int64_t v, fa_rational tb)
{
    if (tb.num <= 0 || tb.den <= 0) return v;
    return (int64_t)llround((double)v * (double)tb.num * 1e9 / (double)tb.den);
}

/* ---- creation (stream.c calls these at open) -------------------------------------- */

static fa_result push_new(struct fa_stream* s, struct fa_push** out)
{
    struct fa_push* p = (struct fa_push*)calloc(1, sizeof(*p));
    if (!p) return FA_ERROR_OUT_OF_MEMORY;
    if (fa_mutex_init(&p->lock) != 0) { free(p); return FA_ERROR_GENERIC; }
    p->stream = s;
    p->pending_anchor = FA_PTS_NONE;
    p->need_anchor = 1;
    p->hold_pending = 1;
    fa_store64(&p->out_pts, 0);
    *out = p;
    return FA_OK;
}

static void decoder_setup(struct fa_stream* s, uint32_t rate, uint32_t channels, const char* codec, const char* container)
{
    fa_decoder* d = &s->decoder;
    memset(d, 0, sizeof(*d));
    d->vt = &fa_decoder_push;
    d->src = &s->source;
    d->impl = s->push;
    d->format.sample_format = FA_FORMAT_F32;
    d->format.sample_rate = rate;
    d->format.channels = channels;
    d->length = -1;
    d->seekable = 0;
    d->anchor_pts = FA_PTS_NONE;
    snprintf(d->codec, sizeof(d->codec), "%s", codec);
    snprintf(d->container, sizeof(d->container), "%s", container);
    s->push->rate = rate;
    s->push->channels = channels;
}

fa_result fa_push_init_pcm(struct fa_stream* s, const fa_audio_format* f)
{
    int planar;
    fa_result r;
    if (f->sample_rate < 1000 || f->sample_rate > 768000 || f->channels == 0 || f->channels > 32) return FA_ERROR_INVALID_ARG;
    if (fa_sample_kind(f->sample_format, &planar) < 0) return FA_ERROR_INVALID_ARG;
    r = push_new(s, &s->push);
    if (r != FA_OK) return r;
    s->push->pcm_format = *f;
    decoder_setup(s, f->sample_rate, f->channels, "pcm", "push");
    return FA_OK;
}

fa_result fa_push_init_packets(struct fa_stream* s, const fa_codec_params* params)
{
    struct fa_push* p;
    fa_result r;
    r = push_new(s, &s->push);
    if (r != FA_OK) return r;
    p = s->push;
    p->packets = 1;
    p->tb = params->time_base;
    r = fa_codec_open(&p->codec, params);
    if (r != FA_OK) return r;    /* the caller frees the push with the stream */
    p->codec_ready = 1;
    decoder_setup(s, params->sample_rate, params->channels, params->codec, "packets");
    return FA_OK;
}

void fa_push_free(struct fa_push* p)
{
    fa_push_item* it;
    if (!p) return;
    for (it = p->head; it;) { fa_push_item* n = it->next; free(it); it = n; }
    if (p->codec_ready) fa_codec_close(&p->codec);
    if (p->conv_ready) ma_data_converter_uninit(&p->conv, NULL);
    free(p->buf);
    fa_mutex_destroy(&p->lock);
    free(p);
}

/* ---- the push side (API threads) ----------------------------------------------------- */

static fa_result enqueue(struct fa_push* p, fa_push_item* it, int64_t dur_ns)
{
    fa_mutex_lock(&p->lock);
    if (fa_load32(&p->ended)) { fa_mutex_unlock(&p->lock); free(it); return FA_ERROR_END_OF_STREAM; }
    if (p->bytes + it->size > FA_PUSH_MAX_BYTES) { fa_mutex_unlock(&p->lock); free(it); return FA_ERROR_BUSY; }
    it->next = NULL;
    if (p->tail) p->tail->next = it; else p->head = it;
    p->tail = it;
    p->bytes += it->size;
    /* For fa_stream_get_buffered: durations, never timestamps, so a jump in the
     * timeline doesn't count as audio. A packet without one counts like the
     * previous one until it is decoded. */
    if (dur_ns > 0) p->last_dur = dur_ns;
    it->dur_known = dur_ns > 0;
    it->dur_ns = dur_ns > 0 ? dur_ns : p->last_dur;
    p->queued_ns += it->dur_ns;
    fa_fetch_add32(&p->count, 1);
    fa_mutex_unlock(&p->lock);
    fa_engine_wake_workers(p->stream->engine);
    return FA_OK;
}

static fa_result push_frames(struct fa_stream* s, const fa_audio_format* f, const void* const* data,
                             fa_frames frames, int64_t pts_ns)
{
    struct fa_push* p = s->push;
    fa_push_item* it;
    int planar, kind;
    size_t bytes;
    kind = fa_sample_kind(f->sample_format, &planar);
    if (kind < 0 || f->channels == 0 || f->channels > 32 || f->sample_rate < 1000 || f->sample_rate > 768000)
        return FA_ERROR_INVALID_ARG;
    if (frames == 0) return FA_OK;
    if (frames < 0 || frames > (fa_frames)1 << 24 || !data || !data[0]) return FA_ERROR_INVALID_ARG;
    if (planar) {
        uint32_t c;
        for (c = 0; c < f->channels; c++) if (!data[c]) return FA_ERROR_INVALID_ARG;
    }
    bytes = (size_t)frames * f->channels * sizeof(float);
    it = (fa_push_item*)malloc(sizeof(*it) + bytes);
    if (!it) return FA_ERROR_OUT_OF_MEMORY;
    memset(it, 0, sizeof(*it));
    it->pts = pts_ns;
    it->rate = f->sample_rate;
    it->channels = f->channels;
    it->frames = frames;
    it->size = bytes;
    fa_convert_to_f32(data, kind, planar, 0, f->channels, frames, (float*)(it + 1));
    return enqueue(p, it, (int64_t)((double)frames * 1e9 / (double)f->sample_rate + 0.5));
}

FA_API fa_result FA_CALL fa_stream_push_pcm(fa_stream* stream, const void* const* planes, fa_frames frames, fa_time pts)
{
    if (!stream || !stream->push || stream->push->packets) return FA_ERROR_INVALID_ARG;
    return push_frames(stream, &stream->push->pcm_format, planes, frames, pts);
}

FA_API fa_result FA_CALL fa_stream_push_frame(fa_stream* stream, const fa_audio_frame* frame)
{
    if (!stream || !frame || !stream->push || stream->push->packets) return FA_ERROR_INVALID_ARG;
    return push_frames(stream, &frame->format, frame->data, frame->frames,
                       frame->pts == FA_PTS_NONE ? FA_PTS_NONE : to_ns(frame->pts, frame->time_base));
}

FA_API fa_result FA_CALL fa_stream_push_packet(fa_stream* stream, const fa_packet* packet)
{
    struct fa_push* p;
    fa_push_item* it;
    if (!stream || !packet || !stream->push || !stream->push->packets) return FA_ERROR_INVALID_ARG;
    p = stream->push;
    if (packet->size == 0) return FA_OK;
    if (!packet->data || packet->size > (64u << 20)) return FA_ERROR_INVALID_ARG;
    it = (fa_push_item*)malloc(sizeof(*it) + packet->size);
    if (!it) return FA_ERROR_OUT_OF_MEMORY;
    memset(it, 0, sizeof(*it));
    it->is_packet = 1;
    it->pts = packet->pts == FA_PTS_NONE ? FA_PTS_NONE : to_ns(packet->pts, p->tb);
    it->size = packet->size;
    memcpy(it + 1, packet->data, packet->size);
    return enqueue(p, it, packet->duration > 0 ? to_ns(packet->duration, p->tb) : 0);
}

FA_API fa_result FA_CALL fa_stream_push_end(fa_stream* stream)
{
    if (!stream || !stream->push) return FA_ERROR_INVALID_ARG;
    fa_store32(&stream->push->ended, 1);
    fa_engine_wake_workers(stream->engine);
    return FA_OK;
}

int fa_push_has_input(struct fa_push* p)
{
    return fa_load32(&p->count) > 0 || fa_load32(&p->has_buffered) || fa_load32(&p->ended);
}

int64_t fa_push_next_pts(struct fa_push* p) { return fa_load64(&p->out_pts); }

/* Pushed audio not handed to the stream yet: queued, and decoded but not read. */
int64_t fa_push_buffered_ns(struct fa_push* p)
{
    int64_t q;
    fa_mutex_lock(&p->lock);
    q = p->queued_ns;
    fa_mutex_unlock(&p->lock);
    return q + (int64_t)((double)fa_load64(&p->out_buffered) * 1e9 / (double)p->rate + 0.5);
}

/* Drops the queue and everything decoded; the next chunk starts a new timeline.
 * Caller holds the stream's decode_lock. */
void fa_push_flush_locked(struct fa_push* p, int keep_hold)
{
    fa_push_item* it;
    fa_mutex_lock(&p->lock);
    it = p->head;
    p->head = p->tail = NULL;
    p->bytes = 0;
    fa_store32(&p->count, 0);
    fa_store32(&p->ended, 0);
    p->queued_ns = 0;
    fa_mutex_unlock(&p->lock);
    while (it) { fa_push_item* n = it->next; free(it); it = n; }
    if (p->codec_ready && p->codec.vt->flush) p->codec.vt->flush(&p->codec);
    if (p->conv_ready) ma_data_converter_reset(&p->conv);
    p->buf_count = p->buf_off = 0;
    fa_store64(&p->out_buffered, 0);
    p->pending_anchor = FA_PTS_NONE;
    fa_store32(&p->has_buffered, 0);
    p->drained = 0;
    p->need_anchor = 1;
    /* Once audio has played, the clock holds at the last time heard until the
     * new audio is heard; before that it shows the first pts pushed. */
    p->hold_pending = !keep_hold;
}

/* A new ring starts (attach, a rebuilt chain): the next frame handed out must
 * anchor the timeline again. Caller holds decode_lock. */
void fa_push_restart_timeline(struct fa_push* p)
{
    if (p->pending_anchor == FA_PTS_NONE && !p->need_anchor) p->pending_anchor = p->next_pts;
}

/* ---- the push decoder (worker side) ----------------------------------------------------- */

static fa_result buf_reserve(struct fa_push* p, fa_frames frames)
{
    fa_frames need = frames * (fa_frames)p->channels;
    if (need > p->buf_cap) {
        fa_frames cap = p->buf_cap ? p->buf_cap : 8192;
        float* b;
        while (cap < need) cap *= 2;
        b = (float*)realloc(p->buf, (size_t)cap * sizeof(float));
        if (!b) return FA_ERROR_OUT_OF_MEMORY;
        p->buf = b;
        p->buf_cap = cap;
    }
    return FA_OK;
}

/* Puts `frames` frames of `in` (rate, channels) into buf, in the stream's format. */
static fa_result convert_into_buf(struct fa_push* p, const float* in, fa_frames frames, uint32_t rate, uint32_t channels)
{
    fa_result r;
    if (rate == p->rate && channels == p->channels) {
        r = buf_reserve(p, frames);
        if (r != FA_OK) return r;
        memcpy(p->buf, in, (size_t)frames * channels * sizeof(float));
        p->buf_count = frames;
        return FA_OK;
    }
    if (!p->conv_ready || p->conv_rate != rate || p->conv_channels != channels) {
        ma_data_converter_config cc;
        if (p->conv_ready) { ma_data_converter_uninit(&p->conv, NULL); p->conv_ready = 0; }
        cc = ma_data_converter_config_init(ma_format_f32, ma_format_f32, channels, p->channels, rate, p->rate);
        if (ma_data_converter_init(&cc, NULL, &p->conv) != MA_SUCCESS) return FA_ERROR_OUT_OF_MEMORY;
        p->conv_ready = 1;
        p->conv_rate = rate;
        p->conv_channels = channels;
    }
    {
        ma_uint64 expect = 0;
        fa_frames done_in = 0;
        ma_data_converter_get_expected_output_frame_count(&p->conv, (ma_uint64)frames, &expect);
        r = buf_reserve(p, (fa_frames)expect + 64);
        if (r != FA_OK) return r;
        p->buf_count = 0;
        while (done_in < frames) {
            ma_uint64 in_n = (ma_uint64)(frames - done_in);
            ma_uint64 out_n = (ma_uint64)(p->buf_cap / p->channels - p->buf_count);
            if (out_n == 0) {
                r = buf_reserve(p, p->buf_count * 2 + 64);
                if (r != FA_OK) return r;
                continue;
            }
            ma_data_converter_process_pcm_frames(&p->conv, in + (size_t)done_in * channels, &in_n,
                                                 p->buf + (size_t)p->buf_count * p->channels, &out_n);
            done_in += (fa_frames)in_n;
            p->buf_count += (fa_frames)out_n;
            if (in_n == 0 && out_n == 0) break;
        }
    }
    return FA_OK;
}

static void post_decode_error(struct fa_push* p, fa_result err)
{
    fa_event ev;
    ev.type = FA_EVENT_DECODE_ERROR;
    ev.stream = p->stream;
    ev.frame = fa_stream_position_now(p->stream);
    ev.value = err;
    fa_engine_post_event(p->stream->engine, &ev);
}

/* Puts a decoded block into buf and decides whether it starts a new piece of the timeline. */
static void take_block(struct fa_push* p, const float* pcm, fa_frames n, uint32_t rate, uint32_t ch, int64_t pts)
{
    p->buf_count = p->buf_off = 0;
    if (n > 0 && rate > 0 && ch > 0) {
        if (convert_into_buf(p, pcm, n, rate, ch) != FA_OK) p->buf_count = 0;
    }
    if (p->buf_count > 0) {
        /* Does this chunk continue the timeline or start a new piece of it? */
        if (p->need_anchor) {
            p->pending_anchor = pts != FA_PTS_NONE ? pts : p->next_pts;
            p->need_anchor = 0;
        } else if (pts != FA_PTS_NONE && (pts - p->next_pts > FA_PUSH_JITTER_NS || p->next_pts - pts > FA_PUSH_JITTER_NS)) {
            p->pending_anchor = pts;
        }
        fa_store32(&p->has_buffered, 1);
    }
}

/* Pops one item and decodes it into buf. 0 = the queue is empty. */
static int refill(struct fa_push* p)
{
    fa_push_item* it;
    fa_mutex_lock(&p->lock);
    it = p->head;
    if (it) {
        p->head = it->next;
        if (!p->head) p->tail = NULL;
        p->bytes -= it->size;
        p->queued_ns -= it->dur_ns;
        fa_fetch_add32(&p->count, -1);
    }
    fa_mutex_unlock(&p->lock);
    if (!it) {
        /* The end: the codec gives back what it still holds (delayed output). */
        if (p->codec_ready && !p->drained && fa_load32(&p->ended) && fa_load32(&p->count) == 0) {
            p->drained = 1;
            if (p->codec.vt->decode(&p->codec, NULL, 0) == FA_OK && p->codec.out_frames > 0) {
                take_block(p, p->codec.out, p->codec.out_frames, p->codec.rate, p->codec.channels, FA_PTS_NONE);
                return 1;
            }
        }
        return 0;
    }

    if (it->is_packet) {
        fa_result r = p->codec.vt->decode(&p->codec, (const uint8_t*)(it + 1), it->size);
        if (r != FA_OK) {
            post_decode_error(p, r);
            free(it);
            return 1;
        }
        if (p->codec.out_frames > 0 && p->codec.rate > 0 && !it->dur_known) {
            /* No duration given: the next packet without one is guessed from this one. */
            fa_mutex_lock(&p->lock);
            p->last_dur = (int64_t)((double)p->codec.out_frames * 1e9 / (double)p->codec.rate + 0.5);
            fa_mutex_unlock(&p->lock);
        }
        take_block(p, p->codec.out, p->codec.out_frames, p->codec.rate, p->codec.channels, it->pts);
    } else {
        take_block(p, (const float*)(it + 1), it->frames, it->rate, it->channels, it->pts);
    }
    free(it);
    return 1;
}

static fa_result push_read(fa_decoder* dec, float* out, fa_frames frames, fa_frames* got)
{
    struct fa_push* p = (struct fa_push*)dec->impl;
    fa_frames produced = 0;
    double ns_per_frame = 1e9 / (double)p->rate;
    dec->anchor_pts = FA_PTS_NONE;
    while (produced < frames) {
        if (p->buf_off < p->buf_count) {
            fa_frames n = p->buf_count - p->buf_off;
            if (p->pending_anchor != FA_PTS_NONE) {
                if (produced > 0) break;          /* an anchor always starts a read */
                dec->anchor_pts = p->pending_anchor;
                p->next_pts = p->pending_anchor;
                p->pending_anchor = FA_PTS_NONE;
                if (p->hold_pending) {
                    p->hold_pending = 0;
                    fa_clock_set_hold(&p->stream->clock, dec->anchor_pts);
                }
            }
            if (n > frames - produced) n = frames - produced;
            memcpy(out + (size_t)produced * p->channels, p->buf + (size_t)p->buf_off * p->channels,
                   (size_t)n * p->channels * sizeof(float));
            p->buf_off += n;
            produced += n;
            p->next_pts += (int64_t)((double)n * ns_per_frame + 0.5);
            if (p->buf_off >= p->buf_count) fa_store32(&p->has_buffered, 0);
            continue;
        }
        if (!refill(p)) break;
    }
    fa_store64(&p->out_pts, p->next_pts);
    fa_store64(&p->out_buffered, p->buf_count - p->buf_off);
    *got = produced;
    if (produced > 0) return FA_OK;
    if (fa_load32(&p->ended) && fa_load32(&p->count) == 0) return FA_ERROR_END_OF_STREAM;
    return FA_ERROR_BUSY;     /* nothing queued yet: not the end */
}

static fa_result push_seek(fa_decoder* dec, fa_frames frame)
{
    (void)dec; (void)frame;
    return FA_ERROR_NOT_SEEKABLE;
}

static void push_close(fa_decoder* dec) { dec->impl = NULL; }  /* freed with the stream */
static int push_probe(const uint8_t* d, size_t n, size_t id3) { (void)d; (void)n; (void)id3; return 0; }
static fa_result push_open(fa_decoder* dec) { (void)dec; return FA_ERROR_UNSUPPORTED_FORMAT; }

const fa_decoder_vtable fa_decoder_push = {
    "push", "App-supplied PCM or packets", "", "",
    push_probe, push_open, push_read, push_seek, push_close, 0
};
