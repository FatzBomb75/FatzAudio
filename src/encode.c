/* Encoders: the public API, the encoder thread, taps on devices and streams, and
 * the conversion to the output format. The formats are in enc_wav.c, enc_flac.c
 * and enc_mp3.c.
 *
 * Data path:
 *   device/stream tap (audio thread) --+
 *   fa_encoder_write (app thread) -----+--> queue (fa_ring) --+
 *   fa_encoder_encode_stream (reads on the encoder thread) ----+--> channel mix
 *       --> resampler --> format --> sink (file or the app's callbacks)
 *
 * Locks, outermost first: tap_lock (which device or stream feeds which encoder),
 * a stream's api_lock, an encoder's read_lock, an encoder's lock. The audio thread
 * takes none of them: it reads the published encoder lists. */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L    /* fseeko under strict C11 */
#endif
#include "encode.h"
#include "mixer.h"
#include "net.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ENC_CHUNK 4096               /* frames converted at a time */
#define ENC_SINK_BUF 65536
#define ENC_STREAM_SCRATCH 16384     /* largest audio callback a stream tap records */
#define ENC_DEFAULT_QUEUE_MS 2000
#define ENC_IDLE_WAIT_MS 5           /* the audio thread cannot wake the encoder: it polls */
#define ENC_APP_WAIT_MS 1000         /* fa_encoder_write signals; this is only a safety net */

enum { SRC_NONE = 0, SRC_DEVICE, SRC_STREAM, SRC_READ, SRC_APP, SRC_DEAD, SRC_MIXER };

struct fa_encoder {
    const fa_enc_backend* be;
    void* impl;
    fa_enc_sink sink;
    fa_enc_tags tags;
    fa_enc_format req;           /* as asked: 0 = the input's */
    fa_enc_format fmt;           /* chosen when the source is attached */
    uint32_t queue_ms;

    fa_mutex lock;               /* error, state changes, waiting */
    fa_cond cond;
    fa_thread thread;
    int thread_running;
    int source;                  /* SRC_*: set once, under tap_lock */
    fa_device* device;           /* under tap_lock; NULL once the device closed */
    fa_stream* stream;           /* under tap_lock (SRC_READ: and read_lock) */
    struct fa_mixer* dj;         /* SRC_MIXER: under tap_lock; NULL once the mixer went */
    int dj_tap;                  /* fa_mixer_tap */
    fa_atomic64 dj_start;        /* the device frame it records from; DJ_WAITING until started */
    fa_mutex read_lock;          /* SRC_READ: held while the thread reads `stream` */
    fa_mutex write_lock;         /* SRC_APP: fixes the input format once */
    uint32_t in_rate, in_channels;

    fa_ring queue;
    fa_atomic32 paused;
    fa_atomic32 stop;            /* fa_encoder_close: drain the queue and end */
    fa_atomic32 state;           /* fa_encoder_state */
    fa_atomic64 dropped;
    fa_atomic64 frames_out;
    fa_atomic64 bytes;
    fa_result error;             /* first error, under lock */

    /* Conversion, encoder thread only */
    ma_channel_converter mixer;
    int mixer_ready;
    fa_varispeed rs;
    int resample;
    double ratio;                /* input frames per output frame */
    int64_t in_total;
    float* mixed;                /* ENC_CHUNK frames at the output's channels */
    float* resampled;
    float* chunk;                /* SRC_READ: ENC_CHUNK frames at the input's channels */
};

static fa_lazy_mutex tap_lock;

#define DJ_WAITING INT64_MAX

/* ---- sink ---------------------------------------------------------------------- */

static int file_seek(FILE* f, int64_t pos)
{
#if defined(_WIN32)
    return _fseeki64(f, pos, SEEK_SET);
#else
    return fseeko(f, (off_t)pos, SEEK_SET);
#endif
}

static fa_result sink_out(fa_enc_sink* s, const void* data, size_t size)
{
    if (size == 0) return FA_OK;
    if (s->file) return fwrite(data, 1, size, s->file) == size ? FA_OK : FA_ERROR_IO;
    {
        fa_result r = s->out.write(s->user, data, size);
        return r == FA_OK ? FA_OK : (r < 0 ? r : FA_ERROR_IO);
    }
}

fa_result fa_sink_flush(fa_enc_sink* s)
{
    if (s->error != FA_OK) return s->error;
    if (s->fill > 0) {
        fa_result r = sink_out(s, s->buf, s->fill);
        s->fill = 0;
        if (r != FA_OK) { s->error = r; return r; }
    }
    return FA_OK;
}

fa_result fa_sink_write(fa_enc_sink* s, const void* data, size_t size)
{
    if (s->error != FA_OK) return s->error;
    if (s->fill + size > ENC_SINK_BUF && fa_sink_flush(s) != FA_OK) return s->error;
    if (size >= ENC_SINK_BUF) {
        fa_result r = sink_out(s, data, size);
        if (r != FA_OK) { s->error = r; return r; }
    } else {
        memcpy(s->buf + s->fill, data, size);
        s->fill += size;
    }
    s->pos += (int64_t)size;
    if (s->pos > s->written) s->written = s->pos;
    return FA_OK;
}

fa_result fa_sink_write_u8(fa_enc_sink* s, uint32_t v)
{
    uint8_t b = (uint8_t)v;
    return fa_sink_write(s, &b, 1);
}

fa_result fa_sink_write_le16(fa_enc_sink* s, uint32_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    return fa_sink_write(s, b, 2);
}

fa_result fa_sink_write_le32(fa_enc_sink* s, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    return fa_sink_write(s, b, 4);
}

fa_result fa_sink_write_le64(fa_enc_sink* s, uint64_t v)
{
    fa_result r = fa_sink_write_le32(s, (uint32_t)v);
    return r != FA_OK ? r : fa_sink_write_le32(s, (uint32_t)(v >> 32));
}

fa_result fa_sink_seek(fa_enc_sink* s, int64_t pos)
{
    fa_result r;
    if (!s->seekable) return FA_ERROR_NOT_SEEKABLE;
    if (fa_sink_flush(s) != FA_OK) return s->error;
    if (s->file) r = file_seek(s->file, pos) == 0 ? FA_OK : FA_ERROR_IO;
    else r = s->out.seek(s->user, pos);
    if (r != FA_OK) { s->error = r < 0 ? r : FA_ERROR_IO; return s->error; }
    s->pos = pos;
    return FA_OK;
}

/* ---- shared helpers for the formats ------------------------------------------------- */

void fa_enc_quantize(const float* in, int32_t* out, int64_t count, uint32_t bits, uint32_t* seed)
{
    const double scale = bits == 16 ? 32768.0 : 8388608.0, hi = scale - 1.0, lo = -scale;
    int64_t i;
    uint32_t x = *seed;
    for (i = 0; i < count; i++) {
        double v = (double)in[i] * scale;
        /* Samples already on the 16-bit grid (a 16-bit source played at unity gain)
         * pass through unchanged, so transcoding stays lossless. */
        if (bits == 16 && v != floor(v)) {
            /* TPDF: the sum of two uniform values in [-0.5, 0.5), one LSB peak to peak each. */
            uint32_t a, b;
            x = x * 1664525u + 1013904223u; a = x >> 8;
            x = x * 1664525u + 1013904223u; b = x >> 8;
            v += ((double)a + (double)b) / 16777216.0 - 1.0;
        }
        v = floor(v + 0.5);
        if (v > hi) v = hi;
        if (v < lo) v = lo;
        out[i] = (int32_t)v;
    }
    *seed = x;
}

uint32_t fa_enc_channel_mask(uint32_t channels)
{
    /* FL FR FC LFE BL BR (FLC FRC) BC SL SR: the WAVE and FLAC defaults. */
    static const uint32_t masks[9] = { 0, 0x4, 0x3, 0x7, 0x33, 0x37, 0x3F, 0x70F, 0x63F };
    return channels < 9 ? masks[channels] : 0;
}

/* ---- options ------------------------------------------------------------------------ */

FA_API void FA_CALL fa_encoder_options_init(fa_encoder_options* o)
{
    if (!o) return;
    memset(o, 0, sizeof(*o));
    o->struct_size = (uint32_t)sizeof(*o);
    o->format = FA_ENCODE_WAV;
    o->quality = -1;
}

static char* dup_str(const char* s, int* oom)
{
    char* d;
    if (!s || !s[0]) return NULL;
    d = fa_strdup(s);
    if (!d) *oom = 1;
    return d;
}

static void free_tags(fa_enc_tags* t)
{
    free(t->title); free(t->artist); free(t->album); free(t->genre); free(t->date); free(t->comment);
    memset(t, 0, sizeof(*t));
}

static const fa_enc_backend* backend_for(fa_encode_format f)
{
    switch (f) {
    case FA_ENCODE_WAV: return &fa_enc_wav;
    case FA_ENCODE_FLAC: return &fa_enc_flac;
    case FA_ENCODE_MP3: return &fa_enc_mp3;
    default: return NULL;
    }
}

/* ---- create / destroy ------------------------------------------------------------- */

static void encoder_free(fa_encoder* e)
{
    if (e->impl) e->be->free(e->impl);
    if (e->mixer_ready) ma_channel_converter_uninit(&e->mixer, NULL);
    if (e->resample) fa_varispeed_free(&e->rs);
    fa_ring_free(&e->queue);
    free(e->mixed);
    free(e->resampled);
    free(e->chunk);
    free(e->sink.buf);
    free_tags(&e->tags);
    fa_cond_destroy(&e->cond);
    fa_mutex_destroy(&e->lock);
    fa_mutex_destroy(&e->read_lock);
    fa_mutex_destroy(&e->write_lock);
    free(e);
}

static fa_result encoder_new(const fa_encoder_options* options, fa_encoder** out)
{
    fa_encoder_options o;
    fa_encoder* e;
    const fa_enc_backend* be;
    fa_result r;
    int oom = 0;
    *out = NULL;
    fa_encoder_options_init(&o);
    if (options) {
        if (options->struct_size < sizeof(o)) return FA_ERROR_INVALID_ARG;
        o = *options;
    }
    be = backend_for(o.format);
    if (!be) return FA_ERROR_INVALID_ARG;
    e = (fa_encoder*)calloc(1, sizeof(*e));
    if (!e) return FA_ERROR_OUT_OF_MEMORY;
    if (fa_mutex_init(&e->lock) != 0) { free(e); return FA_ERROR_GENERIC; }
    if (fa_mutex_init(&e->read_lock) != 0) { fa_mutex_destroy(&e->lock); free(e); return FA_ERROR_GENERIC; }
    if (fa_mutex_init(&e->write_lock) != 0) {
        fa_mutex_destroy(&e->read_lock); fa_mutex_destroy(&e->lock); free(e);
        return FA_ERROR_GENERIC;
    }
    if (fa_cond_init(&e->cond) != 0) {
        fa_mutex_destroy(&e->write_lock); fa_mutex_destroy(&e->read_lock); fa_mutex_destroy(&e->lock); free(e);
        return FA_ERROR_GENERIC;
    }
    e->be = be;
    e->req.rate = o.sample_rate;
    e->req.channels = o.channels;
    e->req.bits = o.bits_per_sample ? o.bits_per_sample : 16;
    e->req.bitrate_kbps = o.bitrate_kbps;
    e->req.quality = o.quality;
    e->queue_ms = o.queue_ms ? o.queue_ms : ENC_DEFAULT_QUEUE_MS;
    e->tags.title = dup_str(o.title, &oom);
    e->tags.artist = dup_str(o.artist, &oom);
    e->tags.album = dup_str(o.album, &oom);
    e->tags.genre = dup_str(o.genre, &oom);
    e->tags.date = dup_str(o.date, &oom);
    e->tags.comment = dup_str(o.comment, &oom);
    e->tags.track = o.track_number;
    e->sink.buf = (uint8_t*)malloc(ENC_SINK_BUF);
    if (oom || !e->sink.buf) { encoder_free(e); return FA_ERROR_OUT_OF_MEMORY; }
    if (e->req.channels > FA_TEMPO_MAX_CHANNELS || e->req.rate > 768000 || e->queue_ms > 60000) {
        encoder_free(e);
        return FA_ERROR_INVALID_ARG;
    }
    r = be->check(&e->req);
    if (r != FA_OK) { encoder_free(e); return r; }
    *out = e;
    return FA_OK;
}

FA_API fa_result FA_CALL fa_encoder_create_path(const char* path, const fa_encoder_options* options, fa_encoder** out_encoder)
{
    fa_encoder* e;
    fa_result r;
    if (!path || !path[0] || !out_encoder) return FA_ERROR_INVALID_ARG;
    *out_encoder = NULL;
    r = encoder_new(options, &e);
    if (r != FA_OK) return r;
    e->sink.file = fa_fopen_utf8(path, "wb");
    if (!e->sink.file) { encoder_free(e); return FA_ERROR_IO; }
    e->sink.seekable = 1;
    *out_encoder = e;
    return FA_OK;
}

FA_API fa_result FA_CALL fa_encoder_create_output(const fa_encoder_output* output, void* user,
                                                  const fa_encoder_options* options, fa_encoder** out_encoder)
{
    fa_encoder* e;
    fa_result r;
    if (!output || !output->write || !out_encoder) return FA_ERROR_INVALID_ARG;
    *out_encoder = NULL;
    r = encoder_new(options, &e);
    if (r != FA_OK) return r;
    e->sink.out = *output;
    e->sink.user = user;
    e->sink.seekable = output->seek != NULL;
    *out_encoder = e;
    return FA_OK;
}

/* ---- errors and state ----------------------------------------------------------- */

static void set_error(fa_encoder* e, fa_result r)
{
    fa_mutex_lock(&e->lock);
    if (e->error == FA_OK) e->error = r;
    fa_store32(&e->state, FA_ENCODER_FAILED);
    fa_cond_broadcast(&e->cond);
    fa_mutex_unlock(&e->lock);
}

static void set_finished(fa_encoder* e)
{
    fa_mutex_lock(&e->lock);
    if (fa_load32(&e->state) == FA_ENCODER_RUNNING) fa_store32(&e->state, FA_ENCODER_FINISHED);
    fa_cond_broadcast(&e->cond);
    fa_mutex_unlock(&e->lock);
}

static int failed(fa_encoder* e) { return fa_load32(&e->state) == FA_ENCODER_FAILED; }

/* ---- conversion and encoding (encoder thread) ------------------------------------- */

static fa_result encode_out(fa_encoder* e, const float* frames, int64_t n)
{
    fa_result r;
    if (n <= 0) return FA_OK;
    r = e->be->write(e->impl, frames, n, &e->sink);
    fa_fetch_add64(&e->frames_out, n);
    fa_store64(&e->bytes, e->sink.written);
    return r;
}

/* Output frames owed for the input so far: exact lengths whatever the ratio. */
static int64_t owed(const fa_encoder* e)
{
    return (int64_t)floor((double)e->in_total * (double)e->fmt.rate / (double)e->in_rate + 0.5);
}

static fa_result drain_resampler(fa_encoder* e, int64_t limit)
{
    for (;;) {
        int64_t want = limit - fa_load64(&e->frames_out), got;
        fa_result r;
        if (want <= 0) return FA_OK;
        if (want > ENC_CHUNK) want = ENC_CHUNK;
        got = fa_varispeed_read(&e->rs, e->ratio, e->resampled, want);
        if (got <= 0) return FA_OK;
        r = encode_out(e, e->resampled, got);
        if (r != FA_OK) return r;
    }
}

/* `n` <= ENC_CHUNK input frames. */
static fa_result convert(fa_encoder* e, const float* in, int64_t n)
{
    const float* x = in;
    fa_result r;
    if (n <= 0) return FA_OK;
    if (e->mixer_ready) {
        ma_channel_converter_process_pcm_frames(&e->mixer, e->mixed, in, (ma_uint64)n);
        x = e->mixed;
    }
    e->in_total += n;
    if (!e->resample) return encode_out(e, x, n);
    r = fa_varispeed_write(&e->rs, x, n);
    if (r != FA_OK) return r;
    return drain_resampler(e, owed(e));
}

/* The end of the input: flush the resampler's lookahead with silence. */
static fa_result convert_end(fa_encoder* e)
{
    int i;
    if (!e->resample) return FA_OK;
    for (i = 0; i < 8 && fa_load64(&e->frames_out) < owed(e); i++) {   /* one round is normally enough */
        fa_result r = fa_varispeed_write(&e->rs, NULL, fa_varispeed_tail());
        if (r == FA_OK) r = drain_resampler(e, owed(e));
        if (r != FA_OK) return r;
    }
    return FA_OK;
}

/* Drains the queue; returns frames taken. */
static int64_t drain_queue(fa_encoder* e)
{
    int64_t total = 0;
    while (!failed(e)) {
        int64_t n = fa_ring_readable(&e->queue), n1, n2, k;
        const float *p1, *p2;
        fa_result r = FA_OK;
        if (n <= 0) break;
        if (n > ENC_CHUNK) n = ENC_CHUNK;
        fa_ring_regions(&e->queue, n, &p1, &n1, &p2, &n2);
        k = n1;
        r = convert(e, p1, n1);
        if (r == FA_OK && n2 > 0) { r = convert(e, p2, n2); k += n2; }
        fa_ring_advance_read(&e->queue, n);
        total += k;
        if (r != FA_OK) { set_error(e, r); break; }
    }
    return total;
}

/* SRC_READ: one chunk of the stream. 1 = more to come, 0 = the end, -1 = wait. */
static int read_chunk(fa_encoder* e)
{
    fa_frames got = 0;
    fa_result r;
    fa_mutex_lock(&e->read_lock);
    if (!e->stream) { fa_mutex_unlock(&e->read_lock); return 0; }   /* the stream was closed */
    /* A web stream is read only once enough is downloaded (as playback does), so a
     * stalled download never holds read_lock and fa_encoder_close returns at once. */
    if (e->stream->net && !fa_net_can_decode(e->stream->net, NULL)) { fa_mutex_unlock(&e->read_lock); return -1; }
    r = fa_stream_read(e->stream, e->chunk, ENC_CHUNK, &got);
    fa_mutex_unlock(&e->read_lock);
    if (got > 0) {
        fa_result c = convert(e, e->chunk, got);
        if (c != FA_OK) { set_error(e, c); return 0; }
    }
    if (r == FA_ERROR_END_OF_STREAM) return 0;
    if (r == FA_ERROR_BUSY) return -1;               /* attached meanwhile: wait for a detach */
    if (r != FA_OK) { set_error(e, r); return 0; }
    return got > 0 ? 1 : -1;                         /* a push stream with nothing queued yet */
}

static void encoder_thread(void* arg)
{
    fa_encoder* e = (fa_encoder*)arg;
    for (;;) {
        int idle;
        if (failed(e)) break;
        if (e->source == SRC_READ) {
            int k;
            if (fa_load32(&e->stop)) break;
            k = read_chunk(e);
            if (k == 0) {
                if (!failed(e)) {
                    fa_result r = convert_end(e);
                    if (r != FA_OK) set_error(e, r);
                    else set_finished(e);
                }
                break;
            }
            idle = k < 0;
        } else {
            int stopping = fa_load32(&e->stop);   /* read before draining: nothing is queued after it */
            idle = drain_queue(e) == 0;
            if (stopping && idle) break;
            if (!idle && e->source == SRC_APP) {   /* room again for a waiting fa_encoder_write */
                fa_mutex_lock(&e->lock);
                fa_cond_broadcast(&e->cond);
                fa_mutex_unlock(&e->lock);
            }
        }
        if (idle) {
            /* fa_encoder_write wakes the thread (it writes, then signals under the lock,
             * so the queue is checked again here); only the audio thread's taps and
             * fa_stream_read's "not yet" need polling. */
            int app = e->source == SRC_APP;
            fa_mutex_lock(&e->lock);
            if (!fa_load32(&e->stop) && !failed(e) && !(app && fa_ring_readable(&e->queue) > 0))
                fa_cond_wait_ms(&e->cond, &e->lock, app ? ENC_APP_WAIT_MS : ENC_IDLE_WAIT_MS);
            fa_mutex_unlock(&e->lock);
        }
    }
}

/* ---- attaching a source -------------------------------------------------------------- */

/* The source could not be connected after start(): the thread stops and the
 * encoder stays failed (its header may already be written). */
static fa_result fail_start(fa_encoder* e, fa_result r)
{
    fa_store32(&e->stop, 1);
    if (e->thread_running) {
        fa_thread_join(&e->thread);
        e->thread_running = 0;
    }
    e->source = SRC_DEAD;
    set_error(e, r);
    return r;
}

/* Fixes the input format, chooses the output, sets up conversion, writes the header
 * and starts the thread. Caller holds tap_lock and e->source is SRC_NONE. A bad
 * input format changes nothing; any later failure leaves the encoder failed. */
static fa_result start_steps(fa_encoder* e, int source)
{
    const fa_enc_format f = e->fmt;
    fa_result r;
    e->mixed = (float*)malloc((size_t)ENC_CHUNK * f.channels * sizeof(float));
    e->resampled = (float*)malloc((size_t)ENC_CHUNK * f.channels * sizeof(float));
    if (!e->mixed || !e->resampled) return FA_ERROR_OUT_OF_MEMORY;
    if (source == SRC_READ) {
        e->chunk = (float*)malloc((size_t)ENC_CHUNK * e->in_channels * sizeof(float));
        if (!e->chunk) return FA_ERROR_OUT_OF_MEMORY;
    } else {
        int64_t qcap = (int64_t)e->in_rate * e->queue_ms / 1000;
        if (qcap < ENC_CHUNK) qcap = ENC_CHUNK;
        r = fa_ring_init(&e->queue, e->in_channels, qcap);
        if (r != FA_OK) return r;
    }
    if (e->in_channels != f.channels) {
        ma_channel_converter_config cc = ma_channel_converter_config_init(ma_format_f32, e->in_channels, NULL, f.channels,
                                                                          NULL, ma_channel_mix_mode_default);
        if (ma_channel_converter_init(&cc, NULL, &e->mixer) != MA_SUCCESS) return FA_ERROR_OUT_OF_MEMORY;
        e->mixer_ready = 1;
    }
    if (e->in_rate != f.rate) {
        int64_t hist = fa_varispeed_tail();
        float* zeros = (float*)calloc((size_t)hist * f.channels, sizeof(float));
        if (!zeros) return FA_ERROR_OUT_OF_MEMORY;
        r = fa_varispeed_init(&e->rs, f.channels);
        if (r == FA_OK) {
            e->resample = 1;
            r = fa_varispeed_prime(&e->rs, zeros, hist);
        }
        free(zeros);
        if (r != FA_OK) return r;
        e->ratio = (double)e->in_rate / (double)f.rate;
    }
    r = e->be->begin(&e->impl, &f, &e->tags, &e->sink);
    if (r != FA_OK) return r;
    fa_store64(&e->bytes, e->sink.written);
    fa_store32(&e->state, FA_ENCODER_RUNNING);
    e->source = source;
    if (fa_thread_create(&e->thread, encoder_thread, e) != 0) return FA_ERROR_GENERIC;
    e->thread_running = 1;
    return FA_OK;
}

static fa_result start(fa_encoder* e, int source, uint32_t in_rate, uint32_t in_channels)
{
    fa_enc_format f = e->req;
    fa_result r;
    if (in_rate == 0 || in_rate > 768000 || in_channels == 0 || in_channels > FA_TEMPO_MAX_CHANNELS)
        return FA_ERROR_INVALID_ARG;
    if (!f.rate) f.rate = in_rate;
    if (!f.channels) f.channels = in_channels;
    e->be->choose(&f);
    if (f.rate * 80.0 < in_rate) return FA_ERROR_INVALID_ARG;   /* beyond the resampler's range */
    e->in_rate = in_rate;
    e->in_channels = in_channels;
    e->fmt = f;
    r = start_steps(e, source);
    if (r != FA_OK) fail_start(e, r);
    return r;
}

/* Publishes `slot`'s list with `add` added or `remove` taken out, then waits until
 * the audio thread of `dev` (if any) is done with the old one. */
static fa_result publish(fa_atomic_ptr* slot, fa_encoder* add, fa_encoder* remove, int scratch, fa_device* dev)
{
    fa_enc_list* old = (fa_enc_list*)fa_load_ptr(slot);
    fa_enc_list* list = NULL;
    uint32_t n = old ? old->count : 0, i, k = 0;
    if (n > 0 || add) {
        list = (fa_enc_list*)calloc(1, sizeof(fa_enc_list) + (size_t)n * sizeof(fa_encoder*));
        if (!list) return FA_ERROR_OUT_OF_MEMORY;
        for (i = 0; i < n; i++)
            if (old->items[i] != remove) list->items[k++] = old->items[i];
        if (add) list->items[k++] = add;
        list->count = k;
        if (k == 0) {
            free(list);
            list = NULL;
        } else if (scratch) {
            list->scratch = (float*)malloc((size_t)ENC_STREAM_SCRATCH * FA_DEVICE_CHANNELS * sizeof(float));
            if (!list->scratch) { free(list); return FA_ERROR_OUT_OF_MEMORY; }
            list->scratch_cap = ENC_STREAM_SCRATCH;
        }
    }
    fa_exchange_ptr(slot, list);
    if (dev) fa_device_wait_quiescent(dev);
    if (old) { free(old->scratch); free(old); }
    return FA_OK;
}

FA_API fa_result FA_CALL fa_encoder_attach_device(fa_encoder* encoder, fa_device* device)
{
    fa_result r;
    if (!encoder || !device) return FA_ERROR_INVALID_ARG;
    fa_lazy_mutex_lock(&tap_lock);
    if (encoder->source != SRC_NONE) { fa_lazy_mutex_unlock(&tap_lock); return FA_ERROR_BUSY; }
    r = start(encoder, SRC_DEVICE, device->rate, FA_DEVICE_CHANNELS);
    if (r == FA_OK) {
        encoder->device = device;
        r = publish(&device->encoders, encoder, NULL, 0, device);
        if (r != FA_OK) { encoder->device = NULL; fail_start(encoder, r); }
    }
    fa_lazy_mutex_unlock(&tap_lock);
    return r;
}

FA_API fa_result FA_CALL fa_encoder_attach_stream(fa_encoder* encoder, fa_stream* stream)
{
    fa_result r;
    if (!encoder || !stream) return FA_ERROR_INVALID_ARG;
    fa_lazy_mutex_lock(&tap_lock);
    if (encoder->source != SRC_NONE) { fa_lazy_mutex_unlock(&tap_lock); return FA_ERROR_BUSY; }
    fa_mutex_lock(&stream->api_lock);
    if (!stream->device) r = FA_ERROR_INVALID_ARG;
    else if (stream->reader) r = FA_ERROR_BUSY;
    else r = start(encoder, SRC_STREAM, stream->device->rate, FA_DEVICE_CHANNELS);
    if (r == FA_OK) {
        encoder->stream = stream;
        r = publish(&stream->encoders, encoder, NULL, 1, stream->device);
        if (r != FA_OK) { encoder->stream = NULL; fail_start(encoder, r); }
    }
    fa_mutex_unlock(&stream->api_lock);
    fa_lazy_mutex_unlock(&tap_lock);
    return r;
}

FA_API fa_result FA_CALL fa_encoder_attach_mixer(fa_encoder* encoder, fa_mixer* mixer, fa_mixer_tap tap)
{
    fa_result r;
    if (!encoder || !mixer || (tap != FA_MIXER_TAP_MASTER && tap != FA_MIXER_TAP_VOCALS)) return FA_ERROR_INVALID_ARG;
    fa_lazy_mutex_lock(&tap_lock);
    if (encoder->source != SRC_NONE) { fa_lazy_mutex_unlock(&tap_lock); return FA_ERROR_BUSY; }
    fa_store64(&encoder->dj_start, DJ_WAITING);
    encoder->dj_tap = (int)tap;
    r = start(encoder, SRC_MIXER, mixer->device->rate, FA_DEVICE_CHANNELS);
    if (r == FA_OK) {
        encoder->dj = mixer;
        r = publish(&mixer->encoders, encoder, NULL, 0, mixer->device);
        if (r != FA_OK) { encoder->dj = NULL; fail_start(encoder, r); }
    }
    fa_lazy_mutex_unlock(&tap_lock);
    return r;
}

FA_API fa_result FA_CALL fa_mixer_start_recording(fa_mixer* mixer, fa_encoder* const* encoders, uint32_t count,
                                                  fa_time device_time, uint32_t flags)
{
    fa_device* dev;
    int64_t now, at, shift, look;
    uint32_t i;
    fa_result r = FA_OK;
    if (!mixer || (count > 0 && !encoders) || (flags & ~FA_MIXER_REC_RAW_TIMING) || device_time < 0)
        return FA_ERROR_INVALID_ARG;
    dev = mixer->device;
    /* The master tap is after the limiter's look-ahead, the vocals tap before it: the
     * master starts that much later, so both files begin with what the mixer made on
     * frame `at`. Aligned, the vocals then start later by the whole round trip from
     * the mixer to the singer and back: the look-ahead, the master's OS buffer and
     * the mic's input (fa_mixer_latency.mic_monitor), since the singer hears the music
     * made on frame `at` only once it has left the limiter. */
    look = dev->limiter.delay;
    shift = flags & FA_MIXER_REC_RAW_TIMING ? 0 : fa_mixer_vocal_shift(mixer);
    fa_lazy_mutex_lock(&tap_lock);
    for (i = 0; i < count && r == FA_OK; i++) {
        fa_encoder* e = encoders[i];
        if (!e || e->source != SRC_MIXER || e->dj != mixer) r = FA_ERROR_INVALID_ARG;
        else if (fa_load64(&e->dj_start) != DJ_WAITING) r = FA_ERROR_BUSY;
    }
    if (r == FA_OK) {
        /* A block may be in the audio thread now, past the vocals tap but not yet at the
         * master's (the clock moves on at its end, by at most FA_MIXER_BLOCK): the
         * earliest start is one whole block ahead, so both taps see it. */
        now = fa_load64(&dev->clock) + FA_MIXER_BLOCK;
        at = device_time > 0 ? (int64_t)((double)device_time * (double)dev->rate / 1e9 + 0.5) : now;
        if (at < now) at = now;
        for (i = 0; i < count; i++)
            fa_store64(&encoders[i]->dj_start, at + (encoders[i]->dj_tap == FA_MIXER_TAP_VOCALS ? shift : look));
    }
    fa_lazy_mutex_unlock(&tap_lock);
    return r;
}

FA_API fa_result FA_CALL fa_encoder_encode_stream(fa_encoder* encoder, fa_stream* stream)
{
    fa_result r;
    if (!encoder || !stream) return FA_ERROR_INVALID_ARG;
    fa_lazy_mutex_lock(&tap_lock);
    if (encoder->source != SRC_NONE) { fa_lazy_mutex_unlock(&tap_lock); return FA_ERROR_BUSY; }
    fa_mutex_lock(&stream->api_lock);
    if (stream->device || stream->reader) r = FA_ERROR_BUSY;
    else {
        /* The stream is set before the thread starts reading it. */
        encoder->stream = stream;
        stream->reader = encoder;
        r = start(encoder, SRC_READ, stream->decoder.format.sample_rate, stream->decoder.format.channels);
        if (r != FA_OK) {
            encoder->stream = NULL;
            stream->reader = NULL;
        }
    }
    fa_mutex_unlock(&stream->api_lock);
    fa_lazy_mutex_unlock(&tap_lock);
    return r;
}

FA_API fa_result FA_CALL fa_encoder_write(fa_encoder* encoder, const float* interleaved, fa_frames frames,
                                          uint32_t sample_rate, uint32_t channels)
{
    fa_encoder* e = encoder;
    fa_frames done = 0;
    if (!e || frames < 0 || (frames > 0 && !interleaved)) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&e->write_lock);
    if (e->source == SRC_NONE) {
        fa_result r;
        fa_lazy_mutex_lock(&tap_lock);
        r = e->source == SRC_NONE ? start(e, SRC_APP, sample_rate, channels) : FA_ERROR_BUSY;
        fa_lazy_mutex_unlock(&tap_lock);
        if (r != FA_OK) { fa_mutex_unlock(&e->write_lock); return r; }
    } else if (e->source != SRC_APP) {
        fa_mutex_unlock(&e->write_lock);
        return FA_ERROR_BUSY;
    }
    if (sample_rate != e->in_rate || channels != e->in_channels) {
        fa_mutex_unlock(&e->write_lock);
        return FA_ERROR_INVALID_ARG;
    }
    while (done < frames) {
        int64_t room;
        fa_result err;
        if (failed(e) || fa_load32(&e->stop)) break;
        room = fa_ring_writable(&e->queue);
        if (room > frames - done) room = frames - done;
        if (room > 0) {
            fa_ring_write(&e->queue, interleaved + (size_t)done * channels, room);
            done += room;
            fa_mutex_lock(&e->lock);
            fa_cond_broadcast(&e->cond);
            fa_mutex_unlock(&e->lock);
            continue;
        }
        fa_mutex_lock(&e->lock);
        err = e->error;
        if (err == FA_OK) fa_cond_wait_ms(&e->cond, &e->lock, ENC_IDLE_WAIT_MS);
        fa_mutex_unlock(&e->lock);
    }
    fa_mutex_unlock(&e->write_lock);
    if (failed(e)) {
        fa_result r;
        fa_mutex_lock(&e->lock);
        r = e->error;
        fa_mutex_unlock(&e->lock);
        return r;
    }
    return done == frames ? FA_OK : FA_ERROR_BUSY;   /* closed meanwhile */
}

FA_API fa_result FA_CALL fa_encoder_set_paused(fa_encoder* encoder, int32_t paused)
{
    if (!encoder) return FA_ERROR_INVALID_ARG;
    fa_store32(&encoder->paused, paused ? 1 : 0);
    return FA_OK;
}

/* ---- taps (audio thread) ------------------------------------------------------------- */

void fa_encode_push(fa_encoder* e, const float* frames, int64_t count, uint32_t rate)
{
    int64_t room;
    if (count <= 0 || fa_load32(&e->paused) || fa_load32(&e->stop)) return;
    if (rate != e->in_rate || failed(e)) { fa_fetch_add64(&e->dropped, count); return; }
    room = fa_ring_writable(&e->queue);
    if (room > count) room = count;
    if (room > 0) fa_ring_write(&e->queue, frames, room);
    if (room < count) fa_fetch_add64(&e->dropped, count - room);
}

void fa_encode_mixer_push(struct fa_mixer* m, int tap, const float* frames, int64_t count, int64_t clock)
{
    fa_enc_list* encs = (fa_enc_list*)fa_load_ptr(&m->encoders);
    uint32_t i;
    if (!encs) return;
    for (i = 0; i < encs->count; i++) {
        fa_encoder* e = encs->items[i];
        int64_t from = fa_load64(&e->dj_start), off;
        if (e->dj_tap != tap || from >= clock + count) continue;
        off = from > clock ? from - clock : 0;
        fa_encode_push(e, frames + off * FA_DEVICE_CHANNELS, count - off, m->device->rate);
    }
}

void fa_encode_drop(fa_encoder* e, int64_t count)
{
    if (count > 0 && !fa_load32(&e->paused)) fa_fetch_add64(&e->dropped, count);
}

/* ---- the source goes away ---------------------------------------------------------- */

void fa_encode_device_closing(fa_device* d)
{
    fa_enc_list* list;
    uint32_t i;
    fa_lazy_mutex_lock(&tap_lock);
    list = (fa_enc_list*)fa_exchange_ptr(&d->encoders, NULL);
    fa_device_wait_quiescent(d);
    if (list) {
        for (i = 0; i < list->count; i++) list->items[i]->device = NULL;
        free(list);
    }
    fa_lazy_mutex_unlock(&tap_lock);
}

void fa_encode_mixer_closing(struct fa_mixer* m)
{
    fa_enc_list* list;
    uint32_t i;
    fa_lazy_mutex_lock(&tap_lock);
    list = (fa_enc_list*)fa_exchange_ptr(&m->encoders, NULL);
    fa_device_wait_quiescent(m->device);
    if (list) {
        for (i = 0; i < list->count; i++) list->items[i]->dj = NULL;
        free(list);
    }
    fa_lazy_mutex_unlock(&tap_lock);
}

void fa_encode_stream_closing(fa_stream* s)
{
    fa_enc_list* list;
    uint32_t i;
    fa_lazy_mutex_lock(&tap_lock);
    fa_mutex_lock(&s->api_lock);
    list = (fa_enc_list*)fa_load_ptr(&s->encoders);
    if (list)
        for (i = 0; i < list->count; i++) list->items[i]->stream = NULL;
    fa_exchange_ptr(&s->encoders, NULL);
    if (s->device) fa_device_wait_quiescent(s->device);
    if (list) { free(list->scratch); free(list); }
    if (s->reader) {
        fa_encoder* e = s->reader;
        fa_mutex_lock(&e->read_lock);   /* waits for a read in progress */
        e->stream = NULL;
        fa_mutex_unlock(&e->read_lock);
        s->reader = NULL;
    }
    fa_mutex_unlock(&s->api_lock);
    fa_lazy_mutex_unlock(&tap_lock);
}

/* Takes the encoder off its device or stream. */
static void detach_source(fa_encoder* e)
{
    fa_lazy_mutex_lock(&tap_lock);
    if (e->source == SRC_DEVICE && e->device) {
        while (publish(&e->device->encoders, NULL, e, 0, e->device) != FA_OK) fa_sleep_ms(1);
        e->device = NULL;
    } else if (e->source == SRC_MIXER && e->dj) {
        while (publish(&e->dj->encoders, NULL, e, 0, e->dj->device) != FA_OK) fa_sleep_ms(1);
        e->dj = NULL;
    } else if (e->source == SRC_STREAM && e->stream) {
        fa_stream* s = e->stream;
        fa_mutex_lock(&s->api_lock);
        while (publish(&s->encoders, NULL, e, 1, s->device) != FA_OK) fa_sleep_ms(1);
        fa_mutex_unlock(&s->api_lock);
        e->stream = NULL;
    } else if (e->source == SRC_READ && e->stream) {
        fa_stream* s = e->stream;
        fa_mutex_lock(&s->api_lock);
        fa_mutex_lock(&e->read_lock);
        e->stream = NULL;
        fa_mutex_unlock(&e->read_lock);
        s->reader = NULL;
        fa_mutex_unlock(&s->api_lock);
    }
    fa_lazy_mutex_unlock(&tap_lock);
}

/* ---- status, wait, close ----------------------------------------------------------- */

static void fill_format(fa_audio_format* f, fa_sample_format sf, uint32_t rate, uint32_t channels)
{
    f->sample_format = sf;
    f->sample_rate = rate;
    f->channels = channels;
    f->channel_mask = 0;
}

FA_API fa_result FA_CALL fa_encoder_get_status(fa_encoder* encoder, fa_encoder_status* out)
{
    fa_encoder* e = encoder;
    if (!e || !out) return FA_ERROR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    fa_mutex_lock(&e->lock);
    out->state = (fa_encoder_state)fa_load32(&e->state);
    out->error = e->error;
    fa_mutex_unlock(&e->lock);
    out->paused = fa_load32(&e->paused);
    out->frames = fa_load64(&e->frames_out);
    out->bytes = fa_load64(&e->bytes);
    out->dropped_frames = fa_load64(&e->dropped);
    if (out->state != FA_ENCODER_IDLE) {
        fa_sample_format sf = e->fmt.bits == 16 ? FA_FORMAT_S16 : e->fmt.bits == 24 ? FA_FORMAT_S24 : FA_FORMAT_F32;
        out->duration = (fa_time)((double)out->frames * 1e9 / (double)e->fmt.rate + 0.5);
        fill_format(&out->input, FA_FORMAT_F32, e->in_rate, e->in_channels);
        fill_format(&out->output, sf, e->fmt.rate, e->fmt.channels);
    }
    return FA_OK;
}

FA_API fa_result FA_CALL fa_encoder_wait(fa_encoder* encoder, fa_time timeout)
{
    fa_encoder* e = encoder;
    int64_t end;
    fa_result r = FA_ERROR_BUSY;
    if (!e) return FA_ERROR_INVALID_ARG;
    end = timeout < 0 ? INT64_MAX : fa_monotonic_ns() + timeout;
    fa_mutex_lock(&e->lock);
    for (;;) {
        int32_t st = fa_load32(&e->state);
        int64_t now, left;
        if (st == FA_ENCODER_FINISHED || st == FA_ENCODER_FAILED) { r = FA_OK; break; }
        now = fa_monotonic_ns();
        if (now >= end) break;
        left = (end - now) / 1000000 + 1;
        fa_cond_wait_ms(&e->cond, &e->lock, left > 100 ? 100 : (uint32_t)left);
    }
    fa_mutex_unlock(&e->lock);
    return r;
}

FA_API fa_result FA_CALL fa_encoder_close(fa_encoder* encoder)
{
    fa_encoder* e = encoder;
    fa_result r, f;
    if (!e) return FA_ERROR_INVALID_ARG;
    detach_source(e);
    /* Waits for fa_encoder_write: it sees `stop` and returns. */
    fa_store32(&e->stop, 1);
    fa_mutex_lock(&e->lock);
    fa_cond_broadcast(&e->cond);
    fa_mutex_unlock(&e->lock);
    fa_mutex_lock(&e->write_lock);
    fa_mutex_unlock(&e->write_lock);
    if (e->thread_running) {
        fa_thread_join(&e->thread);
        e->thread_running = 0;
    }
    /* The thread is gone: what follows runs alone. */
    if (e->impl && !failed(e)) {
        if (e->source != SRC_READ || fa_load32(&e->state) != FA_ENCODER_FINISHED) {
            r = convert_end(e);
            if (r != FA_OK) set_error(e, r);
        }
        if (!failed(e)) {
            r = e->be->finish(e->impl, &e->sink);
            if (r == FA_OK) r = fa_sink_flush(&e->sink);
            if (r != FA_OK) set_error(e, r);
        }
    } else if (!e->impl && e->source != SRC_DEAD && !failed(e)) {
        /* Never started: an empty file of the right format (header only). */
        fa_enc_format fmt = e->req;
        if (!fmt.rate) fmt.rate = 44100;
        if (!fmt.channels) fmt.channels = 2;
        e->be->choose(&fmt);
        e->fmt = fmt;
        r = e->be->begin(&e->impl, &fmt, &e->tags, &e->sink);
        if (r == FA_OK) r = e->be->finish(e->impl, &e->sink);
        if (r == FA_OK) r = fa_sink_flush(&e->sink);
        if (r != FA_OK) set_error(e, r);
    }
    fa_mutex_lock(&e->lock);
    r = e->error;
    fa_mutex_unlock(&e->lock);
    if (e->sink.file) {
        if (fclose(e->sink.file) != 0 && r == FA_OK) r = FA_ERROR_IO;
        e->sink.file = NULL;
    } else if (e->sink.out.close) {
        e->sink.out.close(e->sink.user);
    }
    f = r;
    encoder_free(e);
    return f;
}
