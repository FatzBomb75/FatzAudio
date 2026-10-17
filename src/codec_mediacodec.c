/* AAC and ALAC through Android's MediaCodec (the NDK's AMediaCodec): the
 * device's own decoders, one packet in, interleaved float out.
 *
 * libmediandk.so is loaded at run time (Android 5.0, API 21, and later), never
 * linked, so the library loads on any Android; its functions are declared here
 * rather than taken from the NDK's headers. When anything is missing (the
 * library, a decoder for the type, the configuration), opening returns
 * FA_ERROR_UNSUPPORTED_FORMAT and codec.c tries the app's FFmpeg instead.
 *  - AAC: "audio/mp4a-latm" with the AudioSpecificConfig as "csd-0"; ADTS frames
 *    lose their header first (codec_os.c).
 *  - ALAC: "audio/alac" (Android 13 and later decode it) with the 24-byte
 *    ALACSpecificConfig as "csd-0".
 * Float output is asked for (Android 7.0 and later honour "pcm-encoding"); 16-bit
 * output is converted. MediaCodec's channel order is WAVE's, the library's, for
 * AAC; ALAC of more than two channels goes to FFmpeg (see fa_mc_codec_open).
 * MediaCodec works asynchronously: output can trail input by a packet or more,
 * which the track decoder allows for (it anchors the audio on the first packet). */
#if defined(__ANDROID__)

#include "fa_internal.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

typedef struct AMediaCodec AMediaCodec;
typedef struct AMediaFormat AMediaFormat;
typedef int32_t media_status_t;
typedef struct mc_buffer_info {   /* AMediaCodecBufferInfo */
    int32_t offset;
    int32_t size;
    int64_t presentationTimeUs;
    uint32_t flags;
} mc_buffer_info;

#define MC_INFO_TRY_AGAIN_LATER (-1)
#define MC_INFO_OUTPUT_FORMAT_CHANGED (-2)
#define MC_INFO_OUTPUT_BUFFERS_CHANGED (-3)
#define MC_FLAG_END_OF_STREAM 4u
#define MC_PCM_16BIT 2
#define MC_PCM_FLOAT 4

static struct {
    void* lib;
    AMediaCodec* (*create_decoder)(const char* mime);
    media_status_t (*codec_delete)(AMediaCodec*);
    media_status_t (*configure)(AMediaCodec*, const AMediaFormat*, void* surface, void* crypto, uint32_t flags);
    media_status_t (*start)(AMediaCodec*);
    media_status_t (*stop)(AMediaCodec*);
    media_status_t (*flush)(AMediaCodec*);
    ssize_t (*dequeue_input)(AMediaCodec*, int64_t timeout_us);
    uint8_t* (*input_buffer)(AMediaCodec*, size_t idx, size_t* out_size);
    /* The offset is an off_t of the platform's ABI: long (32 bits on 32-bit Android). */
    media_status_t (*queue_input)(AMediaCodec*, size_t idx, long offset, size_t size, uint64_t time, uint32_t flags);
    ssize_t (*dequeue_output)(AMediaCodec*, mc_buffer_info* info, int64_t timeout_us);
    uint8_t* (*output_buffer)(AMediaCodec*, size_t idx, size_t* out_size);
    media_status_t (*release_output)(AMediaCodec*, size_t idx, int render);   /* bool render */
    AMediaFormat* (*output_format)(AMediaCodec*);
    AMediaFormat* (*format_new)(void);
    media_status_t (*format_delete)(AMediaFormat*);
    void (*set_string)(AMediaFormat*, const char* name, const char* value);
    void (*set_int32)(AMediaFormat*, const char* name, int32_t value);
    void (*set_buffer)(AMediaFormat*, const char* name, const void* data, size_t size);
    int (*get_int32)(AMediaFormat*, const char* name, int32_t* out);          /* bool */
    int ok;
} mc;

static pthread_once_t mc_once = PTHREAD_ONCE_INIT;

#define MC_SYM(field, name) (*(void**)&mc.field = dlsym(mc.lib, name))

static void mc_load(void)
{
    mc.lib = dlopen("libmediandk.so", RTLD_NOW | RTLD_LOCAL);
    if (!mc.lib) return;
    mc.ok = MC_SYM(create_decoder, "AMediaCodec_createDecoderByType") && MC_SYM(codec_delete, "AMediaCodec_delete") &&
            MC_SYM(configure, "AMediaCodec_configure") && MC_SYM(start, "AMediaCodec_start") &&
            MC_SYM(stop, "AMediaCodec_stop") && MC_SYM(flush, "AMediaCodec_flush") &&
            MC_SYM(dequeue_input, "AMediaCodec_dequeueInputBuffer") &&
            MC_SYM(input_buffer, "AMediaCodec_getInputBuffer") &&
            MC_SYM(queue_input, "AMediaCodec_queueInputBuffer") &&
            MC_SYM(dequeue_output, "AMediaCodec_dequeueOutputBuffer") &&
            MC_SYM(output_buffer, "AMediaCodec_getOutputBuffer") &&
            MC_SYM(release_output, "AMediaCodec_releaseOutputBuffer") &&
            MC_SYM(output_format, "AMediaCodec_getOutputFormat") && MC_SYM(format_new, "AMediaFormat_new") &&
            MC_SYM(format_delete, "AMediaFormat_delete") && MC_SYM(set_string, "AMediaFormat_setString") &&
            MC_SYM(set_int32, "AMediaFormat_setInt32") && MC_SYM(set_buffer, "AMediaFormat_setBuffer") &&
            MC_SYM(get_int32, "AMediaFormat_getInt32");
    /* The library stays loaded for the life of the process. */
}

typedef struct mc_state {
    AMediaCodec* codec;
    int started;
    int adts;
    int eos_sent, drained;
    int encoding;           /* MC_PCM_* of the output */
    uint32_t rate, channels;
    uint32_t pkt_frames, pkt_rate;   /* one packet's frames at its coded rate */
    int64_t pkt_count;               /* packets queued: their timestamps */
} mc_state;

/* The output format after a change (or at the start). */
static void read_format(mc_state* st)
{
    AMediaFormat* f = mc.output_format(st->codec);
    int32_t v = 0;
    if (!f) return;
    if (mc.get_int32(f, "sample-rate", &v) && v > 0) st->rate = (uint32_t)v;
    if (mc.get_int32(f, "channel-count", &v) && v > 0 && v <= 32) st->channels = (uint32_t)v;
    st->encoding = mc.get_int32(f, "pcm-encoding", &v) ? v : MC_PCM_16BIT;
    mc.format_delete(f);
}

/* Takes what output is ready; `wait_us` for the first one (draining). */
static fa_result pull(fa_codec* c, mc_state* st, int64_t wait_us)
{
    int rounds = 0;
    for (;;) {
        mc_buffer_info info;
        ssize_t idx = mc.dequeue_output(st->codec, &info, rounds == 0 ? wait_us : 0);
        if (idx == MC_INFO_TRY_AGAIN_LATER) return FA_OK;
        if (idx == MC_INFO_OUTPUT_FORMAT_CHANGED) {
            /* Audio already out is in the old format: hand it out first. */
            if (c->out_frames > 0) { read_format(st); return FA_OK; }
            read_format(st);
            continue;
        }
        if (idx == MC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
        if (idx < 0) return c->out_frames > 0 ? FA_OK : FA_ERROR_UNSUPPORTED_FORMAT;
        {
            size_t cap = 0;
            uint8_t* buf = mc.output_buffer(st->codec, (size_t)idx, &cap);
            int bytes = st->encoding == MC_PCM_FLOAT ? 4 : 2;
            if (buf && info.size > 0 && info.offset >= 0 && (size_t)info.offset + (size_t)info.size <= cap &&
                st->channels > 0 && st->rate > 0 && (c->out_frames == 0 || (c->rate == st->rate && c->channels == st->channels))) {
                fa_frames frames = (fa_frames)((size_t)info.size / ((size_t)bytes * st->channels));
                if (frames > 0) {
                    const void* plane[1];
                    if (fa_codec_reserve(c, frames, st->channels) != FA_OK) {
                        mc.release_output(st->codec, (size_t)idx, 0);
                        return FA_ERROR_OUT_OF_MEMORY;
                    }
                    c->rate = st->rate;
                    c->channels = st->channels;
                    plane[0] = buf + info.offset;
                    fa_convert_to_f32(plane, bytes == 4 ? FA_SAMPLE_F32 : FA_SAMPLE_S16, 0, 0, c->channels, frames,
                                      c->out + (size_t)c->out_frames * c->channels);
                    c->out_frames += frames;
                }
            }
            mc.release_output(st->codec, (size_t)idx, 0);
        }
        if (info.flags & MC_FLAG_END_OF_STREAM) { st->drained = 1; return FA_OK; }
        if (++rounds > 64) return FA_OK;
    }
}

/* The next input's timestamp. */
static int64_t time_us(const mc_state* st)
{
    return st->pkt_rate ? st->pkt_count * st->pkt_frames * 1000000 / st->pkt_rate : 0;
}

static fa_result mc_decode(fa_codec* c, const uint8_t* data, size_t size)
{
    mc_state* st = (mc_state*)c->impl;
    ssize_t idx;
    int tries;
    c->out_frames = 0;
    if (!data && size == 0) {
        /* The end: an empty buffer marked end of stream, then what the decoder holds. */
        if (st->drained) return FA_OK;
        if (!st->eos_sent) {
            /* Output first while every input buffer is busy: it is handed out now,
             * and the end of stream goes in on a later call. */
            idx = mc.dequeue_input(st->codec, 100000);
            if (idx < 0) {
                fa_result r = pull(c, st, 0);
                if (c->out_frames == 0) st->drained = 1;   /* stuck: give up */
                return r;
            }
            mc.queue_input(st->codec, (size_t)idx, 0, 0, (uint64_t)time_us(st), MC_FLAG_END_OF_STREAM);
            st->eos_sent = 1;
        }
        {
            fa_result r = pull(c, st, 100000);
            if (c->out_frames == 0) st->drained = 1;   /* nothing more within the wait */
            return r;
        }
    }
    if (size == 0 || size > 0x7FFFFFF0 || st->eos_sent) return FA_OK;
    if (st->adts && fa_adts_strip(&data, &size) < 0) return FA_ERROR_UNSUPPORTED_FORMAT;
    /* An input buffer, freeing output while none is free. */
    for (tries = 0;; tries++) {
        idx = mc.dequeue_input(st->codec, 10000);
        if (idx >= 0 || tries >= 50) break;
        if (pull(c, st, 0) == FA_ERROR_OUT_OF_MEMORY) return FA_ERROR_OUT_OF_MEMORY;
    }
    if (idx < 0) return c->out_frames > 0 ? FA_OK : FA_ERROR_UNSUPPORTED_FORMAT;
    {
        size_t cap = 0;
        uint8_t* in = mc.input_buffer(st->codec, (size_t)idx, &cap);
        if (!in || cap < size) {
            mc.queue_input(st->codec, (size_t)idx, 0, 0, (uint64_t)time_us(st), 0);
            return c->out_frames > 0 ? FA_OK : FA_ERROR_UNSUPPORTED_FORMAT;
        }
        memcpy(in, data, size);
        mc.queue_input(st->codec, (size_t)idx, 0, size, (uint64_t)time_us(st), 0);
        st->pkt_count++;
    }
    {
        fa_result r = pull(c, st, 0);
        if (r == FA_ERROR_OUT_OF_MEMORY) return r;
        return FA_OK;
    }
}

static void mc_flush(fa_codec* c)
{
    mc_state* st = (mc_state*)c->impl;
    /* Also after the end of stream: a flush makes the codec take input again. */
    mc.flush(st->codec);
    st->eos_sent = 0;
    st->drained = 0;
}

static void mc_free(mc_state* st)
{
    if (!st) return;
    if (st->codec) {
        if (st->started) mc.stop(st->codec);
        mc.codec_delete(st->codec);
    }
    free(st);
}

static void mc_close(fa_codec* c)
{
    mc_free((mc_state*)c->impl);
    c->impl = NULL;
}

static const fa_codec_vtable mc_vtable = { "mediacodec", mc_decode, mc_flush, mc_close };

fa_result fa_mc_codec_open(fa_codec* c, const fa_codec_params* p)
{
    uint8_t csd[256];
    size_t csd_n;
    const char* mime;
    uint32_t rate, ch, frames;
    mc_state* st;
    AMediaFormat* f;
    int aac = strcmp(p->codec, "aac") == 0;

    if (aac) {
        fa_aac_asc_info info;
        mime = "audio/mp4a-latm";
        csd_n = fa_aac_os_asc(p, 0, csd, sizeof(csd));
        if (csd_n == 0 || !fa_aac_parse_asc(csd, csd_n, &info)) return FA_ERROR_UNSUPPORTED_FORMAT;
        rate = info.rate;
        frames = 1024;
        ch = fa_aac_config_channels(info.channel_config);
        if (ch == 0) ch = p->channels;
    } else if (strcmp(p->codec, "alac") == 0) {
        fa_alac_info a;
        mime = "audio/alac";
        if (!fa_alac_parse_config((const uint8_t*)p->extradata, p->extradata_size, &a)) return FA_ERROR_UNSUPPORTED_FORMAT;
        memcpy(csd, a.config, 24);
        csd_n = 24;
        rate = a.rate;
        ch = a.channels;
        frames = a.frame_length;
        /* ALAC's own channel order (centre first) is not WAVE's, and what the
         * system's decoder gives is not documented: FFmpeg reorders. */
        if (ch > 2) return FA_ERROR_UNSUPPORTED_FORMAT;
    } else {
        return FA_ERROR_UNSUPPORTED_FORMAT;
    }
    if (rate == 0 || ch == 0 || ch > 32) return FA_ERROR_UNSUPPORTED_FORMAT;
    pthread_once(&mc_once, mc_load);
    if (!mc.ok) return FA_ERROR_UNSUPPORTED_FORMAT;

    st = (mc_state*)calloc(1, sizeof(*st));
    if (!st) return FA_ERROR_OUT_OF_MEMORY;
    st->adts = aac;
    st->rate = rate;
    st->channels = ch;
    st->pkt_frames = frames;
    st->pkt_rate = rate;
    st->encoding = MC_PCM_16BIT;
    st->codec = mc.create_decoder(mime);
    f = st->codec ? mc.format_new() : NULL;
    if (!f) { mc_free(st); return FA_ERROR_UNSUPPORTED_FORMAT; }
    mc.set_string(f, "mime", mime);
    mc.set_int32(f, "sample-rate", (int32_t)rate);
    mc.set_int32(f, "channel-count", (int32_t)ch);
    mc.set_int32(f, "pcm-encoding", MC_PCM_FLOAT);
    if (aac) mc.set_int32(f, "is-adts", 0);
    mc.set_buffer(f, "csd-0", csd, csd_n);
    if (mc.configure(st->codec, f, NULL, NULL, 0) != 0 || mc.start(st->codec) != 0) {
        mc.format_delete(f);
        mc_free(st);
        return FA_ERROR_UNSUPPORTED_FORMAT;
    }
    mc.format_delete(f);
    st->started = 1;
    read_format(st);
    c->impl = st;
    c->vt = &mc_vtable;
    c->trails = 1;
    c->rate = st->rate;
    c->channels = st->channels;
    return FA_OK;
}

#endif /* __ANDROID__ */
