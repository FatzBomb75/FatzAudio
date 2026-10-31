/* Capture streams (fa_stream_open_capture): an input device as an ordinary stream
 * source, for recording (fa_encoder_encode_stream on a decode-only stream), reading
 * (fa_stream_read) or playing it like any other stream.
 *
 * The input runs from the open on its own clock. Its callback copies the frames into
 * a ring (one producer, one reader); the decoder takes them out under the stream's
 * decode_lock. The first read, and the first read after the ring overflowed (nobody
 * read for a while: a paused stream), drops what waited, so the stream always
 * continues with live input rather than a backlog. Input that stops without being
 * asked to (unplugged) ends the stream once the ring is empty and it has not come
 * back for FA_CAPTURE_LOST_NS. */
#include "fa_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FA_CAPTURE_RING_MS 1000   /* plus, attached, half the stream's buffer: workers top it up in turns */
#define FA_CAPTURE_LOST_NS 1000000000   /* a default input the system moves comes back sooner */

typedef struct capture_src {
    ma_device device;
    int device_ok;
    fa_ring ring;               /* callback -> reader */
    fa_atomic64 dropped;        /* frames the full ring could not take */
    int64_t dropped_seen;       /* reader: `dropped` at the last catch-up */
    int reading;                /* reader: the first read happened */
    fa_atomic32 stopping;       /* close: the stop that follows is ours */
    fa_atomic64 lost_at;        /* monotonic ns the input stopped by itself, 0 = running */
} capture_src;

static void capture_data(ma_device* d, void* out, const void* in, ma_uint32 frames)
{
    capture_src* c = (capture_src*)d->pUserData;
    int64_t room;
    (void)out;
    if (!c || !in) return;
    room = fa_ring_writable(&c->ring);
    if (room > (int64_t)frames) room = frames;
    fa_ring_write(&c->ring, (const float*)in, room);
    if (room < (int64_t)frames) fa_fetch_add64(&c->dropped, (int64_t)frames - room);
}

/* The backend's thread: an input that stops without being asked to has gone away; a
 * default input the backend moves to the new default, or starts again, records on. */
static void capture_notify(const ma_device_notification* n)
{
    capture_src* c = (capture_src*)n->pDevice->pUserData;
    if (!c || fa_load32(&c->stopping)) return;
    if (n->type == ma_device_notification_type_stopped) fa_store64(&c->lost_at, fa_monotonic_ns());
    else if (n->type == ma_device_notification_type_rerouted || n->type == ma_device_notification_type_started)
        fa_store64(&c->lost_at, 0);
}

static int capture_gone(capture_src* c)
{
    int64_t at = fa_load64(&c->lost_at);
    return at != 0 && fa_monotonic_ns() - at >= FA_CAPTURE_LOST_NS;
}

static int capture_probe(const uint8_t* data, size_t size, size_t id3_size)
{
    (void)data; (void)size; (void)id3_size;
    return 0;   /* never detected from content */
}

static fa_result capture_open_vt(fa_decoder* dec) { (void)dec; return FA_ERROR_UNSUPPORTED_FORMAT; }

static fa_result capture_read(fa_decoder* dec, float* out, fa_frames frames, fa_frames* got)
{
    capture_src* c = (capture_src*)dec->impl;
    int64_t avail, dropped = fa_load64(&c->dropped);
    const float *p1, *p2;
    int64_t n1, n2;
    uint32_t ch = dec->format.channels;
    *got = 0;
    if (!c->reading || dropped != c->dropped_seen) {
        /* Live from here: what waited before the first read or since an overflow goes. */
        fa_ring_advance_read(&c->ring, fa_ring_readable(&c->ring));
        c->dropped_seen = dropped;
        c->reading = 1;
    }
    avail = fa_ring_readable(&c->ring);
    if (avail == 0) return capture_gone(c) ? FA_ERROR_END_OF_STREAM : FA_ERROR_BUSY;
    if (avail > frames) avail = frames;
    fa_ring_regions(&c->ring, avail, &p1, &n1, &p2, &n2);
    memcpy(out, p1, (size_t)n1 * ch * sizeof(float));
    if (n2 > 0) memcpy(out + (size_t)n1 * ch, p2, (size_t)n2 * ch * sizeof(float));
    fa_ring_advance_read(&c->ring, avail);
    *got = avail;
    return FA_OK;
}

static fa_result capture_seek(fa_decoder* dec, fa_frames frame)
{
    (void)dec; (void)frame;
    return FA_ERROR_NOT_SEEKABLE;
}

static void capture_close(fa_decoder* dec)
{
    capture_src* c = (capture_src*)dec->impl;
    if (!c) return;
    if (c->device_ok) {
        fa_store32(&c->stopping, 1);
        ma_device_uninit(&c->device);   /* waits for its callback */
    }
    fa_ring_free(&c->ring);
    free(c);
    dec->impl = NULL;
}

/* Input waiting, or the input gone for good (the read then ends the stream). */
static int capture_ready(fa_decoder* dec)
{
    capture_src* c = (capture_src*)dec->impl;
    return fa_ring_readable(&c->ring) > 0 || capture_gone(c);
}

static const fa_decoder_vtable capture_vtable = {
    "capture", "Input device", "", "",
    capture_probe, capture_open_vt, capture_read, capture_seek, capture_close, 0, NULL, NULL
};

fa_result fa_capture_decoder_open(fa_decoder* dec, fa_engine* engine, const fa_capture_options* options, uint32_t extra_ms)
{
    fa_capture_options o;
    capture_src* c;
    ma_device_config cfg;
    ma_device_id did;
    ma_result mr;
    fa_result r;
    int has_id;
    fa_capture_options_init(&o);
    if (options) {
        if (options->struct_size < sizeof(uint32_t)) return FA_ERROR_INVALID_ARG;
        memcpy(&o, options, options->struct_size < sizeof(o) ? options->struct_size : sizeof(o));
    }
    if ((o.sample_rate && (o.sample_rate < 8000 || o.sample_rate > 384000)) || o.channels > 32 ||
        (o.flags & ~(uint32_t)FA_CAPTURE_OS_PROCESSING))
        return FA_ERROR_INVALID_ARG;
    r = fa_device_parse_id(engine, FA_DEVICE_CAPTURE, o.device_id, &did, &has_id);
    if (r != FA_OK) return r;
    c = (capture_src*)calloc(1, sizeof(*c));
    if (!c) return FA_ERROR_OUT_OF_MEMORY;
    cfg = ma_device_config_init(ma_device_type_capture);
    cfg.capture.pDeviceID = has_id ? &did : NULL;
    cfg.capture.format = ma_format_f32;
    cfg.capture.channels = o.channels;     /* 0 = the device's own */
    cfg.sampleRate = o.sample_rate;        /* likewise */
    cfg.periodSizeInMilliseconds = engine->config.period_ms;
    cfg.dataCallback = capture_data;
    cfg.notificationCallback = capture_notify;
    cfg.pUserData = c;
    if (!(o.flags & FA_CAPTURE_OS_PROCESSING)) {   /* as the mixer's mics (mic.c) */
        cfg.aaudio.inputPreset = ma_aaudio_input_preset_unprocessed;
        cfg.opensl.recordingPreset = ma_opensl_recording_preset_voice_unprocessed;
        cfg.wasapi.rawCapture = MA_TRUE;
    }
    mr = ma_device_init(&engine->context, &cfg, &c->device);
    if (mr != MA_SUCCESS) {
        free(c);
        return mr == MA_ACCESS_DENIED ? FA_ERROR_PERMISSION_DENIED : FA_ERROR_DEVICE;
    }
    c->device_ok = 1;
    r = fa_ring_init(&c->ring, c->device.capture.channels,
                     (int64_t)c->device.sampleRate * (FA_CAPTURE_RING_MS + (int64_t)extra_ms) / 1000);
    if (r == FA_OK && ma_device_start(&c->device) != MA_SUCCESS) r = FA_ERROR_DEVICE;
    memset(dec, 0, sizeof(*dec));
    dec->vt = &capture_vtable;
    dec->impl = c;
    if (r != FA_OK) { capture_close(dec); dec->vt = NULL; return r; }
    dec->format.sample_format = FA_FORMAT_F32;
    dec->format.sample_rate = c->device.sampleRate;
    dec->format.channels = c->device.capture.channels;
    dec->length = -1;
    dec->seekable = 0;
    dec->ready = capture_ready;
    dec->anchor_pts = FA_PTS_NONE;
    snprintf(dec->codec, sizeof(dec->codec), "pcm");
    snprintf(dec->container, sizeof(dec->container), "capture");
    return FA_OK;
}

FA_API void FA_CALL fa_capture_options_init(fa_capture_options* options)
{
    if (!options) return;
    memset(options, 0, sizeof(*options));
    options->struct_size = (uint32_t)sizeof(*options);
}
