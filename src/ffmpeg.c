/* FFmpeg at runtime: libavcodec + libavutil of the app, never linked or bundled.
 *
 * Nothing here includes an FFmpeg header. Functions are looked up by name, and
 * structs are only touched through their leading fields, which have kept the same
 * layout from FFmpeg 4.0 to 8.x:
 *   AVCodecParameters  codec_type, codec_id, codec_tag, extradata, extradata_size
 *   AVPacket           buf, pts, dts, data, size
 *   AVFrame            data[8], linesize[8], extended_data, width, height,
 *                      nb_samples, format
 *   AVCodecDescriptor  id
 * Everything else (rate, channels, block align, ...) goes through AVOptions, which
 * are looked up by name and so do not depend on struct layouts either. */
#include "fa_internal.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the struct prefixes ---------------------------------------------------- */

typedef struct ff_codecpar_prefix {
    int codec_type;
    int codec_id;
    uint32_t codec_tag;
    uint8_t* extradata;
    int extradata_size;
} ff_codecpar_prefix;

typedef struct ff_packet_prefix {
    void* buf;
    int64_t pts;
    int64_t dts;
    uint8_t* data;
    int size;
} ff_packet_prefix;

typedef struct ff_frame_prefix {
    uint8_t* data[8];
    int linesize[8];
    uint8_t** extended_data;
    int width, height;
    int nb_samples;
    int format;
} ff_frame_prefix;

typedef struct ff_descriptor_prefix {
    int id;
} ff_descriptor_prefix;

typedef struct ff_channel_layout {   /* AVChannelLayout (FFmpeg 5.1+), a public, fixed layout */
    int order;
    int nb_channels;
    uint64_t mask;
    void* opaque;
} ff_channel_layout;

#define FF_MEDIA_TYPE_AUDIO 1
#define FF_INPUT_PADDING 64
#define FF_NOPTS INT64_MIN
#define FF_ERROR_EOF (-(int)(('E') | ('O' << 8) | ('F' << 16) | ((unsigned)' ' << 24)))

/* ---- the functions ------------------------------------------------------------ */

typedef struct ff_api {
    unsigned (*avcodec_version)(void);
    unsigned (*avutil_version)(void);
    const void* (*avcodec_descriptor_get_by_name)(const char* name);
    const void* (*avcodec_find_decoder)(int id);
    void* (*avcodec_alloc_context3)(const void* codec);
    void (*avcodec_free_context)(void** ctx);
    void* (*avcodec_parameters_alloc)(void);
    void (*avcodec_parameters_free)(void** par);
    int (*avcodec_parameters_to_context)(void* ctx, const void* par);
    int (*avcodec_open2)(void* ctx, const void* codec, void** options);
    int (*avcodec_send_packet)(void* ctx, const void* pkt);
    int (*avcodec_receive_frame)(void* ctx, void* frame);
    void (*avcodec_flush_buffers)(void* ctx);
    void* (*av_packet_alloc)(void);
    void (*av_packet_free)(void** pkt);
    void* (*av_frame_alloc)(void);
    void (*av_frame_free)(void** frame);
    void (*av_frame_unref)(void* frame);
    void* (*av_mallocz)(size_t size);
    int (*av_opt_set_int)(void* obj, const char* name, int64_t val, int flags);
    int (*av_opt_get_int)(void* obj, const char* name, int flags, int64_t* out);
    int (*av_opt_set)(void* obj, const char* name, const char* val, int flags);
    /* FFmpeg 5.1+ (the channel layout API); NULL before. */
    int (*av_opt_set_chlayout)(void* obj, const char* name, const ff_channel_layout* l, int flags);
    int (*av_opt_get_chlayout)(void* obj, const char* name, int flags, ff_channel_layout* l);
    void (*av_channel_layout_default)(ff_channel_layout* l, int nb_channels);
    void (*av_channel_layout_uninit)(ff_channel_layout* l);
} ff_api;

static ff_api ff;
static int ff_state;               /* 0 = not tried, 1 = loaded, -1 = tried and failed */
static char ff_path[512];

/* ---- loading --------------------------------------------------------------------- */

typedef struct ff_libs { void* avcodec; void* avutil; } ff_libs;

/* File names of libavcodec major `m` and its libavutil (major m - 2). */
static void lib_names(int m, char* codec, char* util, size_t cap)
{
#if defined(_WIN32)
    snprintf(codec, cap, "avcodec-%d.dll", m);
    snprintf(util, cap, "avutil-%d.dll", m - 2);
#elif defined(__APPLE__)
    snprintf(codec, cap, "libavcodec.%d.dylib", m);
    snprintf(util, cap, "libavutil.%d.dylib", m - 2);
#else
    snprintf(codec, cap, "libavcodec.so.%d", m);
    snprintf(util, cap, "libavutil.so.%d", m - 2);
#endif
}

#define FF_MAJOR_MIN 58   /* FFmpeg 4.0 */
#define FF_MAJOR_MAX 62   /* FFmpeg 8.x */

/* mode 0: already loaded in the process; 1: the system's search path; 2: `dir`. */
static int find_libs(int mode, const char* dir, ff_libs* out)
{
    int m;
    for (m = FF_MAJOR_MAX; m >= FF_MAJOR_MIN; m--) {
        char codec[64], util[64], pc[700], pu[700];
        void *lc, *lu;
        lib_names(m, codec, util, sizeof(codec));
        if (mode == 2) {
#ifdef _WIN32
            const char* sep = "\\";
#else
            const char* sep = "/";
#endif
            size_t n = strlen(dir);
            if (n > 0 && (dir[n - 1] == '/' || dir[n - 1] == '\\')) sep = "";
            snprintf(pc, sizeof(pc), "%s%s%s", dir, sep, codec);
            snprintf(pu, sizeof(pu), "%s%s%s", dir, sep, util);
        } else {
            snprintf(pc, sizeof(pc), "%s", codec);
            snprintf(pu, sizeof(pu), "%s", util);
        }
        /* libavutil first: libavcodec depends on it (on Windows, from its folder). */
        lu = fa_lib_open(pu, mode == 0);
        if (!lu) continue;
        lc = fa_lib_open(pc, mode == 0);
        if (!lc) continue;
        out->avcodec = lc;
        out->avutil = lu;
        return 1;
    }
    return 0;
}

static int bind_api(const ff_libs* l)
{
    ff_api a;
    memset(&a, 0, sizeof(a));
#define C(name) (*(void**)&a.name = fa_lib_sym(l->avcodec, #name))
#define U(name) (*(void**)&a.name = fa_lib_sym(l->avutil, #name))
    C(avcodec_version); C(avcodec_descriptor_get_by_name); C(avcodec_find_decoder);
    C(avcodec_alloc_context3); C(avcodec_free_context); C(avcodec_parameters_alloc);
    C(avcodec_parameters_free); C(avcodec_parameters_to_context); C(avcodec_open2);
    C(avcodec_send_packet); C(avcodec_receive_frame); C(avcodec_flush_buffers);
    C(av_packet_alloc); C(av_packet_free);
    U(avutil_version); U(av_frame_alloc); U(av_frame_free); U(av_frame_unref); U(av_mallocz);
    U(av_opt_set_int); U(av_opt_get_int); U(av_opt_set);
    U(av_opt_set_chlayout); U(av_opt_get_chlayout); U(av_channel_layout_default); U(av_channel_layout_uninit);
#undef C
#undef U
    if (!a.avcodec_version || !a.avutil_version || !a.avcodec_descriptor_get_by_name || !a.avcodec_find_decoder ||
        !a.avcodec_alloc_context3 || !a.avcodec_free_context || !a.avcodec_parameters_alloc ||
        !a.avcodec_parameters_free || !a.avcodec_parameters_to_context || !a.avcodec_open2 ||
        !a.avcodec_send_packet || !a.avcodec_receive_frame || !a.avcodec_flush_buffers ||
        !a.av_packet_alloc || !a.av_packet_free || !a.av_frame_alloc || !a.av_frame_free ||
        !a.av_frame_unref || !a.av_mallocz || !a.av_opt_set_int || !a.av_opt_get_int || !a.av_opt_set)
        return 0;
    if ((a.avcodec_version() >> 16) < FF_MAJOR_MIN) return 0;
    /* The channel layout API is all or nothing. */
    if (!a.av_opt_set_chlayout || !a.av_opt_get_chlayout || !a.av_channel_layout_default || !a.av_channel_layout_uninit) {
        a.av_opt_set_chlayout = NULL; a.av_opt_get_chlayout = NULL;
        a.av_channel_layout_default = NULL; a.av_channel_layout_uninit = NULL;
    }
    ff = a;
    return 1;
}

/* One lock for the whole library. */
static fa_lazy_mutex ff_lock_storage;

static void ff_lock(void) { fa_lazy_mutex_lock(&ff_lock_storage); }
static void ff_unlock(void) { fa_lazy_mutex_unlock(&ff_lock_storage); }

/* Caller holds the lock. */
static int try_load(int mode, const char* dir)
{
    ff_libs l;
    if (!find_libs(mode, dir, &l)) return 0;
    if (!bind_api(&l)) return 0;
    fa_lib_where(l.avcodec, "avcodec_version", ff_path, sizeof(ff_path));
    if (!ff_path[0]) {
        char codec[64], util[64];
        lib_names((int)(ff.avcodec_version() >> 16), codec, util, sizeof(codec));
        snprintf(ff_path, sizeof(ff_path), "%s", codec);
    }
    ff_state = 1;
    return 1;
}

int fa_ffmpeg_available(void)
{
    int ok;
    ff_lock();
    if (ff_state == 0) {
        if (!try_load(0, NULL) && !try_load(1, NULL)) ff_state = -1;
    }
    ok = ff_state == 1;
    ff_unlock();
    return ok;
}

FA_API fa_result FA_CALL fa_ffmpeg_load(const char* directory)
{
    fa_result r;
    ff_lock();
    if (ff_state != 1) {
        int ok = directory && directory[0] ? try_load(2, directory) : (try_load(0, NULL) || try_load(1, NULL));
        if (!ok && ff_state == 0 && !(directory && directory[0])) ff_state = -1;
    }
    r = ff_state == 1 ? FA_OK : FA_ERROR_PROVIDER_MISSING;
    ff_unlock();
    return r;
}

FA_API fa_result FA_CALL fa_ffmpeg_get_info(fa_ffmpeg_info* out)
{
    if (!out) return FA_ERROR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    ff_lock();
    if (ff_state == 1) {
        out->loaded = 1;
        out->avcodec_version = ff.avcodec_version();
        out->avutil_version = ff.avutil_version();
        snprintf(out->avcodec_path, sizeof(out->avcodec_path), "%s", ff_path);
    }
    ff_unlock();
    return FA_OK;
}

/* ---- the codec ------------------------------------------------------------------- */

/* AVSampleFormat (stable values) -> FA_SAMPLE_* and planar. */
static int av_kind(int fmt, int* planar)
{
    static const int kinds[] = { FA_SAMPLE_U8, FA_SAMPLE_S16, FA_SAMPLE_S32, FA_SAMPLE_F32, FA_SAMPLE_F64 };
    *planar = 0;
    if (fmt >= 0 && fmt <= 4) return kinds[fmt];
    if (fmt >= 5 && fmt <= 9) { *planar = 1; return kinds[fmt - 5]; }
    if (fmt == 10) return FA_SAMPLE_S64;
    if (fmt == 11) { *planar = 1; return FA_SAMPLE_S64; }
    return -1;
}

typedef struct ff_codec {
    void* ctx;
    void* pkt;
    void* frame;
    int drained;   /* the end-of-input packet was sent */
} ff_codec;

static int ctx_channels(void* ctx)
{
    int64_t v = 0;
    if (ff.av_opt_get_chlayout) {
        ff_channel_layout l;
        memset(&l, 0, sizeof(l));
        if (ff.av_opt_get_chlayout(ctx, "ch_layout", 0, &l) >= 0) {
            int n = l.nb_channels;
            ff.av_channel_layout_uninit(&l);
            return n;
        }
        return 0;
    }
    if (ff.av_opt_get_int(ctx, "ac", 0, &v) < 0) return 0;
    return (int)v;
}

/* Moves every frame the decoder has ready into c->out. */
static fa_result drain(fa_codec* c, ff_codec* f)
{
    for (;;) {
        ff_frame_prefix* fr;
        int64_t rate = 0;
        int ch, kind, planar, ret = ff.avcodec_receive_frame(f->ctx, f->frame);
        if (ret == -EAGAIN || ret == FF_ERROR_EOF) return FA_OK;
        if (ret < 0) return FA_ERROR_UNSUPPORTED_FORMAT;
        fr = (ff_frame_prefix*)f->frame;
        ff.av_opt_get_int(f->ctx, "ar", 0, &rate);
        ch = ctx_channels(f->ctx);
        kind = av_kind(fr->format, &planar);
        if (kind >= 0 && ch > 0 && ch <= 32 && rate > 0 && fr->nb_samples > 0) {
            if (c->out_frames > 0 && ((uint32_t)ch != c->channels || (uint32_t)rate != c->rate)) {
                /* The format changed inside one packet's output (it never does in
                 * practice): keep the newer audio only. */
                c->out_frames = 0;
            }
            c->channels = (uint32_t)ch;
            c->rate = (uint32_t)rate;
            if (fa_codec_reserve(c, fr->nb_samples, c->channels) != FA_OK) {
                ff.av_frame_unref(f->frame);
                return FA_ERROR_OUT_OF_MEMORY;
            }
            fa_convert_to_f32((const void* const*)fr->extended_data, kind, planar, 0, c->channels,
                              fr->nb_samples, c->out + (size_t)c->out_frames * c->channels);
            c->out_frames += fr->nb_samples;
        }
        ff.av_frame_unref(f->frame);
    }
}

static fa_result ff_decode(fa_codec* c, const uint8_t* data, size_t size)
{
    ff_codec* f = (ff_codec*)c->impl;
    ff_packet_prefix* p = (ff_packet_prefix*)f->pkt;
    uint8_t* copy;
    fa_result r = FA_OK;
    int ret;
    c->out_frames = 0;
    if (!data && size == 0) {
        /* The input ended: an empty packet makes the decoder give back what it
         * still holds. Once drained it stays so until ff_flush. */
        if (f->drained) return FA_OK;
        f->drained = 1;
        ret = ff.avcodec_send_packet(f->ctx, NULL);
        if (ret < 0 && ret != FF_ERROR_EOF) return FA_OK;
        return drain(c, f);
    }
    if (size == 0 || size > 0x7FFFFFF0) return FA_OK;
    if (f->drained) return FA_OK;  /* nothing more after the end until a flush */
    /* Decoders read past the end in blocks: the data needs zeroed padding. */
    copy = (uint8_t*)malloc(size + FF_INPUT_PADDING);
    if (!copy) return FA_ERROR_OUT_OF_MEMORY;
    memcpy(copy, data, size);
    memset(copy + size, 0, FF_INPUT_PADDING);
    p->buf = NULL;                 /* not reference counted: avcodec copies it */
    p->data = copy;
    p->size = (int)size;
    p->pts = FF_NOPTS;
    p->dts = FF_NOPTS;
    for (;;) {
        ret = ff.avcodec_send_packet(f->ctx, f->pkt);
        if (ret == -EAGAIN) {      /* output first, then the packet again */
            r = drain(c, f);
            if (r != FA_OK) break;
            continue;
        }
        if (ret < 0) r = FA_ERROR_UNSUPPORTED_FORMAT;  /* corrupt packet: skipped */
        else r = drain(c, f);
        break;
    }
    p->data = NULL;
    p->size = 0;
    free(copy);
    return r;
}

static void ff_flush(fa_codec* c)
{
    ff_codec* f = (ff_codec*)c->impl;
    ff.avcodec_flush_buffers(f->ctx);
    f->drained = 0;
}

static void ff_close(fa_codec* c)
{
    ff_codec* f = (ff_codec*)c->impl;
    if (!f) return;
    if (f->frame) ff.av_frame_free(&f->frame);
    if (f->pkt) ff.av_packet_free(&f->pkt);
    if (f->ctx) ff.avcodec_free_context(&f->ctx);
    free(f);
    c->impl = NULL;
}

static const fa_codec_vtable ff_vtable = { "ffmpeg", ff_decode, ff_flush, ff_close };

fa_result fa_ffmpeg_open_codec(fa_codec* c, const fa_codec_params* p)
{
    const ff_descriptor_prefix* desc;
    const void* codec;
    void* par;
    ff_codecpar_prefix* pp;
    ff_codec* f;
    int ret;
    if (!fa_ffmpeg_available()) return FA_ERROR_PROVIDER_MISSING;
    desc = (const ff_descriptor_prefix*)ff.avcodec_descriptor_get_by_name(p->codec);
    if (!desc) return FA_ERROR_UNSUPPORTED_FORMAT;
    codec = ff.avcodec_find_decoder(desc->id);
    if (!codec) return FA_ERROR_UNSUPPORTED_FORMAT;

    f = (ff_codec*)calloc(1, sizeof(*f));
    if (!f) return FA_ERROR_OUT_OF_MEMORY;
    c->impl = f;
    c->vt = &ff_vtable;
    f->ctx = ff.avcodec_alloc_context3(codec);
    f->pkt = ff.av_packet_alloc();
    f->frame = ff.av_frame_alloc();
    par = ff.avcodec_parameters_alloc();
    if (!f->ctx || !f->pkt || !f->frame || !par) {
        if (par) ff.avcodec_parameters_free(&par);
        ff_close(c);
        return FA_ERROR_OUT_OF_MEMORY;
    }
    pp = (ff_codecpar_prefix*)par;
    pp->codec_type = FF_MEDIA_TYPE_AUDIO;
    pp->codec_id = desc->id;
    if (p->extradata && p->extradata_size > 0 && p->extradata_size < (1u << 28)) {
        pp->extradata = (uint8_t*)ff.av_mallocz(p->extradata_size + FF_INPUT_PADDING);
        if (!pp->extradata) { ff.avcodec_parameters_free(&par); ff_close(c); return FA_ERROR_OUT_OF_MEMORY; }
        memcpy(pp->extradata, p->extradata, p->extradata_size);
        pp->extradata_size = (int)p->extradata_size;
    }
    ret = ff.avcodec_parameters_to_context(f->ctx, par);  /* copies the extradata */
    ff.avcodec_parameters_free(&par);
    if (ret < 0) { ff_close(c); return FA_ERROR_UNSUPPORTED_FORMAT; }

    if (p->sample_rate) ff.av_opt_set_int(f->ctx, "ar", p->sample_rate, 0);
    if (p->channels) {
        if (ff.av_opt_set_chlayout) {
            ff_channel_layout l;
            memset(&l, 0, sizeof(l));
            ff.av_channel_layout_default(&l, (int)p->channels);
            ff.av_opt_set_chlayout(f->ctx, "ch_layout", &l, 0);
            ff.av_channel_layout_uninit(&l);
        } else {
            ff.av_opt_set_int(f->ctx, "ac", p->channels, 0);
        }
    }
    if (p->block_align) ff.av_opt_set_int(f->ctx, "block_align", p->block_align, 0);
    if (p->bits_per_coded_sample) ff.av_opt_set_int(f->ctx, "bits_per_coded_sample", p->bits_per_coded_sample, 0);
    if (p->bit_rate > 0) ff.av_opt_set_int(f->ctx, "b", p->bit_rate, 0);
    ff.av_opt_set(f->ctx, "request_sample_fmt", "flt", 0);  /* saves a conversion where offered */
    {
        /* Packets carry no timestamps, but a time base stops decoders that trim
         * samples (Opus pre-skip, AAC priming) from logging a warning on each open. */
        char tb[32];
        snprintf(tb, sizeof(tb), "1/%u", p->sample_rate ? p->sample_rate : 48000u);
        ff.av_opt_set(f->ctx, "pkt_timebase", tb, 0);
    }

    ff_lock();                     /* avcodec_open2 was not thread-safe in old versions */
    ret = ff.avcodec_open2(f->ctx, codec, NULL);
    ff_unlock();
    if (ret < 0) { ff_close(c); return FA_ERROR_UNSUPPORTED_FORMAT; }
    c->rate = p->sample_rate;
    c->channels = p->channels;
    return FA_OK;
}
