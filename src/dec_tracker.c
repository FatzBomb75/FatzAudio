/* Tracker modules (MOD, S3M, XM, IT, MPTM, MTM, UMX and the other formats
 * libopenmpt reads), played by libopenmpt (vendored in third_party/libopenmpt,
 * linked in statically). Also the tracker stream API: module information and
 * texts, order/row seeking and live render settings.
 *
 * The module plays once (no repeat): the length is libopenmpt's duration of the
 * song, and exactly that many frames come out (padded with silence or cut, so
 * the length the stream reports is the length it plays). Looping is the stream's
 * own (fa_stream_set_loop), from the start.
 *
 * Rendering is cut into blocks of FA_TRACKER_BLOCK frames from the last seek, so
 * the read size changes nothing. A seek asks libopenmpt for the row at or before
 * the target (it restores the channels' state there, sample positions included)
 * and renders on to the exact frame without handing it out.
 *
 * Live settings (interpolation, stereo separation, volume ramping, Amiga
 * resampler emulation, channel mute) are atomics the decode thread applies at the
 * start of each block. The module itself is only touched by whoever holds the
 * stream's decode_lock (the decode thread, or a seek). */
#include "fa_internal.h"
#include "libopenmpt/libopenmpt.h"
#include "libopenmpt/libopenmpt_ext.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FA_TRACKER_BLOCK 512
#define FA_TRACKER_MAX_FILE ((size_t)128 << 20)
#define FA_TRACKER_MAX_SECONDS (6.0 * 3600.0)
#define FA_TRACKER_MUTE_CHANNELS 64

struct fa_tracker {
    openmpt_module_ext* ext;
    openmpt_module* mod;
    openmpt_module_ext_interface_interactive interactive;
    int has_interactive;
    uint32_t rate;
    fa_frames length;
    fa_frames pos;              /* frames handed out (the stream position) */
    int ended;                  /* libopenmpt has no more: silence up to length */

    float blk[FA_TRACKER_BLOCK * 2];
    int blk_len, blk_off;

    fa_tracker_info info;
    char* message;
    char** names[FA_TRACKER_TEXT_SUBSONG + 1];   /* per text type, by index */
    uint32_t name_count[FA_TRACKER_TEXT_SUBSONG + 1];

    /* Set by the API, applied by the decode thread. */
    fa_atomic32 interpolation, separation, ramping, amiga;
    fa_atomic64 mute;           /* bit n = channel n */
    int32_t interpolation_now, separation_now, ramping_now, amiga_now;
    uint64_t mute_now;
};

struct fa_tracker* fa_tracker_of(fa_decoder* dec)
{
    return dec && dec->vt == &fa_decoder_tracker ? (struct fa_tracker*)dec->impl : NULL;
}

int fa_probe_tracker(const uint8_t* data, size_t size, size_t id3_size)
{
    int r;
    (void)id3_size;
    if (size == 0) return 0;
    r = openmpt_probe_file_header_without_filesize(OPENMPT_PROBE_FILE_HEADER_FLAGS_DEFAULT, data, size,
                                                   openmpt_log_func_silent, NULL, openmpt_error_func_ignore, NULL,
                                                   NULL, NULL);
    /* Some formats have no magic number (15-sample MODs) and the window may be
     * too short to tell: low scores, so a format that is sure wins. */
    if (r == OPENMPT_PROBE_FILE_HEADER_RESULT_SUCCESS) return 30;
    if (r == OPENMPT_PROBE_FILE_HEADER_RESULT_WANTMOREDATA) return 2;
    return 0;
}

static fa_result read_source(fa_source* src, uint8_t** out, size_t* out_size)
{
    size_t cap, n = 0;
    uint8_t* buf;
    *out = NULL;
    *out_size = 0;
    if (src->length > (int64_t)FA_TRACKER_MAX_FILE) return FA_ERROR_UNSUPPORTED_FORMAT;
    cap = (src->length > 0 ? (size_t)src->length : 65536) + 1;
    buf = (uint8_t*)malloc(cap);
    if (!buf) return FA_ERROR_OUT_OF_MEMORY;
    for (;;) {
        int64_t got;
        if (n == cap) {
            uint8_t* grown;
            if (cap > FA_TRACKER_MAX_FILE) { free(buf); return FA_ERROR_UNSUPPORTED_FORMAT; }
            cap = cap * 2 > FA_TRACKER_MAX_FILE + 1 ? FA_TRACKER_MAX_FILE + 1 : cap * 2;
            grown = (uint8_t*)realloc(buf, cap);
            if (!grown) { free(buf); return FA_ERROR_OUT_OF_MEMORY; }
            buf = grown;
        }
        got = fa_source_read(src, buf + n, cap - n);
        if (got < 0) { free(buf); return (fa_result)got; }
        if (got == 0) break;
        n += (size_t)got;
    }
    *out = buf;
    *out_size = n;
    return FA_OK;
}

/* `in` (UTF-8, from libopenmpt) into `out`, cut at a character boundary. */
static void utf8_copy(char* out, size_t cap, const char* in)
{
    size_t n = strlen(in);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)in[n] & 0xC0) == 0x80) n--;
    }
    memcpy(out, in, n);
    out[n] = 0;
}

static void copy_meta(openmpt_module* mod, const char* key, char* out, size_t cap)
{
    const char* v = openmpt_module_get_metadata(mod, key);
    out[0] = 0;
    if (v) {
        utf8_copy(out, cap, v);
        openmpt_free_string(v);
    }
}

static char* dup_free(const char* s)
{
    char* d;
    size_t n;
    if (!s) return NULL;
    n = strlen(s);
    d = (char*)malloc(n + 1);
    if (d) memcpy(d, s, n + 1);
    openmpt_free_string(s);
    return d;
}

static fa_result load_names(struct fa_tracker* t, int type, uint32_t count)
{
    uint32_t i;
    if (!count) return FA_OK;
    t->names[type] = (char**)calloc(count, sizeof(char*));
    if (!t->names[type]) return FA_ERROR_OUT_OF_MEMORY;
    t->name_count[type] = count;
    for (i = 0; i < count; i++) {
        const char* s = NULL;
        switch (type) {
        case FA_TRACKER_TEXT_INSTRUMENT: s = openmpt_module_get_instrument_name(t->mod, (int32_t)i); break;
        case FA_TRACKER_TEXT_SAMPLE: s = openmpt_module_get_sample_name(t->mod, (int32_t)i); break;
        case FA_TRACKER_TEXT_CHANNEL: s = openmpt_module_get_channel_name(t->mod, (int32_t)i); break;
        case FA_TRACKER_TEXT_PATTERN: s = openmpt_module_get_pattern_name(t->mod, (int32_t)i); break;
        case FA_TRACKER_TEXT_SUBSONG: s = openmpt_module_get_subsong_name(t->mod, (int32_t)i); break;
        default: break;
        }
        t->names[type][i] = dup_free(s);
        if (s && !t->names[type][i]) return FA_ERROR_OUT_OF_MEMORY;
    }
    return FA_OK;
}

static int32_t count_of(int32_t n) { return n > 0 ? n : 0; }

static void tracker_free(struct fa_tracker* t)
{
    int k;
    uint32_t i;
    if (!t) return;
    if (t->ext) openmpt_module_ext_destroy(t->ext);
    free(t->message);
    for (k = 0; k <= FA_TRACKER_TEXT_SUBSONG; k++) {
        for (i = 0; i < t->name_count[k]; i++) free(t->names[k][i]);
        free(t->names[k]);
    }
    free(t);
}

/* The settings the API changed since the last block. */
static void take_controls(struct fa_tracker* t)
{
    int32_t v;
    uint64_t mute;
    v = fa_load32(&t->interpolation);
    if (v != t->interpolation_now) {
        openmpt_module_set_render_param(t->mod, OPENMPT_MODULE_RENDER_INTERPOLATIONFILTER_LENGTH, v);
        t->interpolation_now = v;
    }
    v = fa_load32(&t->separation);
    if (v != t->separation_now) {
        openmpt_module_set_render_param(t->mod, OPENMPT_MODULE_RENDER_STEREOSEPARATION_PERCENT, v);
        t->separation_now = v;
    }
    v = fa_load32(&t->ramping);
    if (v != t->ramping_now) {
        openmpt_module_set_render_param(t->mod, OPENMPT_MODULE_RENDER_VOLUMERAMPING_STRENGTH, v);
        t->ramping_now = v;
    }
    v = fa_load32(&t->amiga);
    if (v != t->amiga_now) {
        openmpt_module_ctl_set_boolean(t->mod, "render.resampler.emulate_amiga", v);
        t->amiga_now = v;
    }
    mute = (uint64_t)fa_load64(&t->mute);
    if (mute != t->mute_now && t->has_interactive) {
        int32_t ch;
        for (ch = 0; ch < (int32_t)t->info.channels && ch < FA_TRACKER_MUTE_CHANNELS; ch++)
            if (((mute ^ t->mute_now) >> ch) & 1)
                t->interactive.set_channel_mute_status(t->ext, ch, (int)((mute >> ch) & 1));
        t->mute_now = mute;
    }
}

/* The next block: libopenmpt's audio, then silence up to the length. */
static void render_block(struct fa_tracker* t)
{
    fa_frames n = t->length - t->pos;
    size_t got = 0;
    if (n > FA_TRACKER_BLOCK) n = FA_TRACKER_BLOCK;
    take_controls(t);
    if (!t->ended) {
        got = openmpt_module_read_interleaved_float_stereo(t->mod, (int32_t)t->rate, (size_t)n, t->blk);
        if (got < (size_t)n) t->ended = 1;
    }
    if (got < (size_t)n) memset(t->blk + got * 2, 0, ((size_t)n - got) * 2 * sizeof(float));
    t->blk_len = (int)n;
    t->blk_off = 0;
}

static fa_result tracker_read(fa_decoder* dec, float* out, fa_frames frames, fa_frames* got)
{
    struct fa_tracker* t = (struct fa_tracker*)dec->impl;
    fa_frames done = 0;
    while (done < frames) {
        fa_frames k;
        if (t->blk_off == t->blk_len) {
            if (t->pos >= t->length) break;
            render_block(t);
        }
        k = t->blk_len - t->blk_off;
        if (k > frames - done) k = frames - done;
        memcpy(out + done * 2, t->blk + (size_t)t->blk_off * 2, (size_t)k * 2 * sizeof(float));
        t->blk_off += (int)k;
        t->pos += k;
        done += k;
    }
    *got = done;
    return done > 0 ? FA_OK : FA_ERROR_END_OF_STREAM;
}

static fa_result tracker_seek(fa_decoder* dec, fa_frames frame)
{
    struct fa_tracker* t = (struct fa_tracker*)dec->impl;
    double at;
    fa_frames from;
    if (frame < 0) frame = 0;
    if (frame > t->length) frame = t->length;
    t->blk_len = t->blk_off = 0;
    t->ended = 0;
    /* libopenmpt lands on the start of a row, the first one starting at or after
     * the time asked: ask earlier until it lands before the target. */
    {
        double want = (double)frame / t->rate, back = 0.25, ask = want;
        for (;;) {
            at = openmpt_module_set_position_seconds(t->mod, ask);
            from = at > 0 ? (fa_frames)llround(at * t->rate) : 0;
            if (from <= frame || ask <= 0) break;
            ask = want - back > 0 ? want - back : 0;
            back *= 2;
        }
        if (from > frame) from = frame;   /* not reached: the start is after it (cannot happen at 0) */
    }
    t->pos = from;
    /* On to the exact frame, without handing anything out. */
    while (t->pos < frame) {
        fa_frames skip = frame - t->pos;
        render_block(t);
        if (skip >= t->blk_len) {
            t->pos += t->blk_len;
            t->blk_len = t->blk_off = 0;
        } else {
            t->blk_off = (int)skip;
            t->pos += skip;
        }
    }
    return FA_OK;
}

static void tracker_close(fa_decoder* dec)
{
    tracker_free((struct fa_tracker*)dec->impl);
    dec->impl = NULL;
}

static fa_result tracker_open(fa_decoder* dec)
{
    struct fa_tracker* t;
    uint8_t* bytes;
    size_t size;
    int err = 0;
    double duration;
    const char* s;
    fa_result r = read_source(dec->src, &bytes, &size);
    if (r != FA_OK) return r;
    t = (struct fa_tracker*)calloc(1, sizeof(*t));
    if (!t) { free(bytes); return FA_ERROR_OUT_OF_MEMORY; }
    t->ext = openmpt_module_ext_create_from_memory(bytes, size, openmpt_log_func_silent, NULL, openmpt_error_func_store,
                                                   &err, &err, NULL, NULL);
    free(bytes);   /* libopenmpt keeps its own copy of what it needs */
    if (!t->ext) {
        free(t);
        return err == OPENMPT_ERROR_OUT_OF_MEMORY ? FA_ERROR_OUT_OF_MEMORY : FA_ERROR_UNSUPPORTED_FORMAT;
    }
    t->mod = openmpt_module_ext_get_module(t->ext);
    t->has_interactive = openmpt_module_ext_get_interface(t->ext, LIBOPENMPT_EXT_C_INTERFACE_INTERACTIVE,
                                                          &t->interactive, sizeof(t->interactive)) != 0;
    t->rate = dec->midi_rate ? dec->midi_rate : FA_MIDI_DEFAULT_RATE;
    openmpt_module_set_repeat_count(t->mod, 0);
    duration = openmpt_module_get_duration_seconds(t->mod);
    if (!(duration > 0)) duration = 0;
    if (duration > FA_TRACKER_MAX_SECONDS) duration = FA_TRACKER_MAX_SECONDS;
    t->length = (fa_frames)llround(duration * t->rate);
    if (t->length <= 0) { tracker_free(t); return FA_ERROR_UNSUPPORTED_FORMAT; }

    /* Defaults, as libopenmpt has them. */
    t->interpolation_now = 0;
    t->separation_now = 100;
    t->ramping_now = -1;
    t->amiga_now = 1;
    openmpt_module_ctl_set_boolean(t->mod, "render.resampler.emulate_amiga", 1);
    /* A seek also puts every playing sample where it would be, not at its start. */
    openmpt_module_ctl_set_boolean(t->mod, "seek.sync_samples", 1);
    fa_store32(&t->interpolation, 0);
    fa_store32(&t->separation, 100);
    fa_store32(&t->ramping, -1);
    fa_store32(&t->amiga, 1);
    fa_store64(&t->mute, 0);

    t->info.struct_size = sizeof(t->info);
    copy_meta(t->mod, "type", t->info.type, sizeof(t->info.type));
    copy_meta(t->mod, "type_long", t->info.type_name, sizeof(t->info.type_name));
    copy_meta(t->mod, "tracker", t->info.tracker, sizeof(t->info.tracker));
    copy_meta(t->mod, "title", t->info.title, sizeof(t->info.title));
    copy_meta(t->mod, "artist", t->info.artist, sizeof(t->info.artist));
    t->info.channels = (uint32_t)count_of(openmpt_module_get_num_channels(t->mod));
    t->info.orders = (uint32_t)count_of(openmpt_module_get_num_orders(t->mod));
    t->info.patterns = (uint32_t)count_of(openmpt_module_get_num_patterns(t->mod));
    t->info.instruments = (uint32_t)count_of(openmpt_module_get_num_instruments(t->mod));
    t->info.samples = (uint32_t)count_of(openmpt_module_get_num_samples(t->mod));
    t->info.subsongs = (uint32_t)count_of(openmpt_module_get_num_subsongs(t->mod));
    /* The restart point is per subsong; -1 is not accepted here (it throws and
       leaks the stored error string), so ask for the selected one. */
    {
        int32_t sub = openmpt_module_get_selected_subsong(t->mod);
        if (sub < 0) sub = 0;
        t->info.restart_order = openmpt_module_get_restart_order(t->mod, sub);
        t->info.restart_row = openmpt_module_get_restart_row(t->mod, sub);
    }
    t->info.duration = duration;
    s = openmpt_module_get_metadata(t->mod, "message_raw");
    t->message = dup_free(s);
    if (s && !t->message) r = FA_ERROR_OUT_OF_MEMORY;
    if (r == FA_OK) r = load_names(t, FA_TRACKER_TEXT_INSTRUMENT, t->info.instruments);
    if (r == FA_OK) r = load_names(t, FA_TRACKER_TEXT_SAMPLE, t->info.samples);
    if (r == FA_OK) r = load_names(t, FA_TRACKER_TEXT_CHANNEL, t->info.channels);
    if (r == FA_OK) r = load_names(t, FA_TRACKER_TEXT_PATTERN, t->info.patterns);
    if (r == FA_OK) r = load_names(t, FA_TRACKER_TEXT_SUBSONG, t->info.subsongs);
    if (r != FA_OK) { tracker_free(t); return r; }

    dec->impl = t;
    dec->format.sample_rate = t->rate;
    dec->format.channels = 2;
    dec->length = t->length;
    dec->seekable = 1;
    snprintf(dec->codec, sizeof(dec->codec), "%s", t->info.type[0] ? t->info.type : "tracker");
    snprintf(dec->container, sizeof(dec->container), "tracker");
    return FA_OK;
}

const fa_decoder_vtable fa_decoder_tracker = {
    "tracker", "Tracker modules (MOD, S3M, XM, IT, MPTM, MTM, UMX and more; libopenmpt)",
    "mod;s3m;xm;it;mptm;mtm;umx;669;amf;ams;dbm;digi;dmf;dsm;far;gdm;imf;j2b;mdl;med;mo3;mt2;okt;plm;psm;ptm;sfx;stm;ult;wow",
    "audio/x-mod;audio/mod;audio/x-s3m;audio/s3m;audio/x-xm;audio/xm;audio/x-it;audio/it",
    fa_probe_tracker, tracker_open, tracker_read, tracker_seek, tracker_close, 0, NULL, NULL
};

/* ---- Tracker stream API -------------------------------------------------------- */

FA_API fa_result FA_CALL fa_stream_get_tracker_info(fa_stream* stream, fa_tracker_info* out_info)
{
    uint32_t want;
    if (!stream || !out_info || out_info->struct_size < offsetof(fa_tracker_info, duration) + sizeof(double) ||
        !stream->tracker)
        return FA_ERROR_INVALID_ARG;
    want = out_info->struct_size < sizeof(fa_tracker_info) ? out_info->struct_size : (uint32_t)sizeof(fa_tracker_info);
    memcpy(out_info, &stream->tracker->info, want);
    out_info->struct_size = want;
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_tracker_text(fa_stream* stream, fa_tracker_text_type type, uint32_t index,
                                                    char* buffer, size_t capacity, size_t* out_length)
{
    struct fa_tracker* t;
    const char* s = NULL;
    size_t n;
    if (out_length) *out_length = 0;
    if (buffer && capacity) buffer[0] = 0;
    if (!stream || !(t = stream->tracker) || (capacity && !buffer)) return FA_ERROR_INVALID_ARG;
    if (type == FA_TRACKER_TEXT_MESSAGE) {
        if (index != 0) return FA_ERROR_NOT_FOUND;
        s = t->message;
    } else if (type >= FA_TRACKER_TEXT_INSTRUMENT && type <= FA_TRACKER_TEXT_SUBSONG) {
        if (index >= t->name_count[type]) return FA_ERROR_NOT_FOUND;
        s = t->names[type][index];
    } else {
        return FA_ERROR_INVALID_ARG;
    }
    if (!s) s = "";
    n = strlen(s);
    if (out_length) *out_length = n;
    if (capacity) utf8_copy(buffer, capacity, s);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_tracker_time(fa_stream* stream, uint32_t order, uint32_t row, double* out_seconds)
{
    double at;
    if (out_seconds) *out_seconds = -1.0;
    if (!stream || !stream->tracker || !out_seconds) return FA_ERROR_INVALID_ARG;
    if (order >= stream->tracker->info.orders) return FA_ERROR_NOT_FOUND;
    fa_mutex_lock(&stream->decode_lock);
    at = openmpt_module_get_time_at_position(stream->tracker->mod, (int32_t)order, (int32_t)row);
    fa_mutex_unlock(&stream->decode_lock);
    if (!(at >= 0)) return FA_ERROR_NOT_FOUND;   /* never played (skipped, or not a row of that pattern) */
    *out_seconds = at;
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_seek_tracker(fa_stream* stream, uint32_t order, uint32_t row)
{
    double at;
    fa_result r = fa_stream_get_tracker_time(stream, order, row, &at);
    if (r != FA_OK) return r;
    return fa_stream_seek(stream, (fa_frames)llround(at * stream->tracker->rate));
}

FA_API fa_result FA_CALL fa_stream_set_tracker_param(fa_stream* stream, fa_tracker_param param, int32_t value)
{
    struct fa_tracker* t;
    if (!stream || !(t = stream->tracker)) return FA_ERROR_INVALID_ARG;
    switch (param) {
    case FA_TRACKER_INTERPOLATION:
        if (value != 0 && value != 1 && value != 2 && value != 4 && value != 8) return FA_ERROR_INVALID_ARG;
        fa_store32(&t->interpolation, value);
        return FA_OK;
    case FA_TRACKER_STEREO_SEPARATION:
        if (value < 0 || value > 200) return FA_ERROR_INVALID_ARG;
        fa_store32(&t->separation, value);
        return FA_OK;
    case FA_TRACKER_VOLUME_RAMPING:
        if (value < -1 || value > 10) return FA_ERROR_INVALID_ARG;
        fa_store32(&t->ramping, value);
        return FA_OK;
    case FA_TRACKER_AMIGA_RESAMPLER:
        fa_store32(&t->amiga, value ? 1 : 0);
        return FA_OK;
    default:
        return FA_ERROR_INVALID_ARG;
    }
}

FA_API fa_result FA_CALL fa_stream_get_tracker_param(fa_stream* stream, fa_tracker_param param, int32_t* out_value)
{
    struct fa_tracker* t;
    if (!stream || !out_value || !(t = stream->tracker)) return FA_ERROR_INVALID_ARG;
    switch (param) {
    case FA_TRACKER_INTERPOLATION: *out_value = fa_load32(&t->interpolation); return FA_OK;
    case FA_TRACKER_STEREO_SEPARATION: *out_value = fa_load32(&t->separation); return FA_OK;
    case FA_TRACKER_VOLUME_RAMPING: *out_value = fa_load32(&t->ramping); return FA_OK;
    case FA_TRACKER_AMIGA_RESAMPLER: *out_value = fa_load32(&t->amiga); return FA_OK;
    default: return FA_ERROR_INVALID_ARG;
    }
}

FA_API fa_result FA_CALL fa_stream_set_tracker_channel_mute(fa_stream* stream, uint32_t channel, int32_t muted)
{
    fa_atomic64* mask;
    int64_t old, bit;
    if (!stream || !stream->tracker || channel >= stream->tracker->info.channels || channel >= FA_TRACKER_MUTE_CHANNELS)
        return FA_ERROR_INVALID_ARG;
    mask = &stream->tracker->mute;
    bit = (int64_t)((uint64_t)1 << channel);
    do {
        old = fa_load64(mask);
    } while (!fa_cas64(mask, old, muted ? (old | bit) : (old & ~bit)));
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_tracker_channel_mute(fa_stream* stream, uint32_t channel, int32_t* out_muted)
{
    if (!stream || !out_muted || !stream->tracker || channel >= stream->tracker->info.channels ||
        channel >= FA_TRACKER_MUTE_CHANNELS)
        return FA_ERROR_INVALID_ARG;
    *out_muted = (int32_t)(((uint64_t)fa_load64(&stream->tracker->mute) >> channel) & 1);
    return FA_OK;
}
