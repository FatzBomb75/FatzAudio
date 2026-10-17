/* Packet codecs: one compressed packet in, interleaved F32 out. The built-in ones
 * (PCM in all its byte layouts, MP3) need nothing else; every other codec goes
 * to the app's FFmpeg, loaded at runtime (ffmpeg.c). */
#include "fa_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- sample conversion ------------------------------------------------------ */

int fa_sample_kind(fa_sample_format f, int* out_planar)
{
    *out_planar = 0;
    switch (f) {
    case FA_FORMAT_F32: return FA_SAMPLE_F32;
    case FA_FORMAT_S16: return FA_SAMPLE_S16;
    case FA_FORMAT_S24: return FA_SAMPLE_S24;
    case FA_FORMAT_S32: return FA_SAMPLE_S32;
    case FA_FORMAT_U8: return FA_SAMPLE_U8;
    case FA_FORMAT_F64: return FA_SAMPLE_F64;
    case FA_FORMAT_F32_PLANAR: *out_planar = 1; return FA_SAMPLE_F32;
    case FA_FORMAT_S16_PLANAR: *out_planar = 1; return FA_SAMPLE_S16;
    case FA_FORMAT_S32_PLANAR: *out_planar = 1; return FA_SAMPLE_S32;
    case FA_FORMAT_F64_PLANAR: *out_planar = 1; return FA_SAMPLE_F64;
    default: return -1;
    }
}

static int sample_bytes(int kind)
{
    static const int b[] = { 1, 1, 2, 3, 4, 8, 4, 8 };
    return b[kind];
}

static int native_big_endian(void)
{
    const uint16_t one = 1;
    return *(const uint8_t*)&one == 0;
}

/* One sample at `p` as float. `swap`: the bytes are in the other order. */
static float sample_at(const uint8_t* p, int kind, int swap)
{
    uint8_t t[8];
    int n = sample_bytes(kind), i;
    if (swap && n > 1 && kind != FA_SAMPLE_S24) {
        for (i = 0; i < n; i++) t[i] = p[n - 1 - i];
        p = t;
    }
    switch (kind) {
    case FA_SAMPLE_U8: return ((float)p[0] - 128.0f) * (1.0f / 128.0f);
    case FA_SAMPLE_S8: return (float)(int8_t)p[0] * (1.0f / 128.0f);
    case FA_SAMPLE_S16: { int16_t v; memcpy(&v, p, 2); return (float)v * (1.0f / 32768.0f); }
    case FA_SAMPLE_S24: {
        /* Packed 3 bytes: little endian unless the data is big endian. */
        int big = swap ? !native_big_endian() : native_big_endian();
        /* Assembled unsigned: shifting a byte into a signed int's sign bit is undefined. */
        uint32_t u = big ? ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8)
                         : ((uint32_t)p[2] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[0] << 8);
        int32_t v = (int32_t)u;
        return (float)(v >> 8) * (1.0f / 8388608.0f);
    }
    case FA_SAMPLE_S32: { int32_t v; memcpy(&v, p, 4); return (float)((double)v * (1.0 / 2147483648.0)); }
    case FA_SAMPLE_S64: { int64_t v; memcpy(&v, p, 8); return (float)((double)v * (1.0 / 9223372036854775808.0)); }
    case FA_SAMPLE_F32: { float v; memcpy(&v, p, 4); return v; }
    default: { double v; memcpy(&v, p, 8); return (float)v; }
    }
}

void fa_convert_to_f32(const void* const* planes, int kind, int planar, int big_endian,
                       uint32_t channels, fa_frames frames, float* out)
{
    int bytes = sample_bytes(kind);
    int swap = big_endian && !native_big_endian();
    fa_frames f;
    uint32_t c;
    if (kind == FA_SAMPLE_F32 && !planar && !swap) {
        memcpy(out, planes[0], (size_t)frames * channels * sizeof(float));
        return;
    }
    if (planar) {
        for (c = 0; c < channels; c++) {
            const uint8_t* p = (const uint8_t*)planes[c];
            if (kind == FA_SAMPLE_F32 && !swap)
                for (f = 0; f < frames; f++) memcpy(&out[(size_t)f * channels + c], p + (size_t)f * 4, 4);
            else
                for (f = 0; f < frames; f++) out[(size_t)f * channels + c] = sample_at(p + (size_t)f * bytes, kind, swap);
        }
    } else {
        const uint8_t* p = (const uint8_t*)planes[0];
        size_t i, n = (size_t)frames * channels;
        for (i = 0; i < n; i++) out[i] = sample_at(p + i * (size_t)bytes, kind, swap);
    }
}

/* ---- shared helpers ----------------------------------------------------------- */

fa_result fa_codec_reserve(fa_codec* c, fa_frames frames, uint32_t channels)
{
    fa_frames need = c->out_frames + frames;
    if (need * (fa_frames)channels > c->out_cap) {
        fa_frames cap = c->out_cap ? c->out_cap : 4096;
        float* p;
        while (cap < need * (fa_frames)channels) cap *= 2;
        p = (float*)realloc(c->out, (size_t)cap * sizeof(float));
        if (!p) return FA_ERROR_OUT_OF_MEMORY;
        c->out = p;
        c->out_cap = cap;
    }
    return FA_OK;
}

void fa_codec_close(fa_codec* c)
{
    if (c->vt && c->vt->close) c->vt->close(c);
    free(c->out);
    memset(c, 0, sizeof(*c));
}

/* ---- PCM ------------------------------------------------------------------------ */

typedef struct pcm_layout {
    const char* name;
    int kind, big_endian;
} pcm_layout;

static const pcm_layout pcm_layouts[] = {
    { "pcm_u8", FA_SAMPLE_U8, 0 },     { "pcm_s8", FA_SAMPLE_S8, 0 },
    { "pcm_s16le", FA_SAMPLE_S16, 0 }, { "pcm_s16be", FA_SAMPLE_S16, 1 },
    { "pcm_s24le", FA_SAMPLE_S24, 0 }, { "pcm_s24be", FA_SAMPLE_S24, 1 },
    { "pcm_s32le", FA_SAMPLE_S32, 0 }, { "pcm_s32be", FA_SAMPLE_S32, 1 },
    { "pcm_s64le", FA_SAMPLE_S64, 0 }, { "pcm_s64be", FA_SAMPLE_S64, 1 },
    { "pcm_f32le", FA_SAMPLE_F32, 0 }, { "pcm_f32be", FA_SAMPLE_F32, 1 },
    { "pcm_f64le", FA_SAMPLE_F64, 0 }, { "pcm_f64be", FA_SAMPLE_F64, 1 },
};

typedef struct pcm_state {
    const pcm_layout* layout;
    uint8_t carry[64];   /* the bytes of a frame split across two packets */
    size_t carry_len;
} pcm_state;

static fa_result pcm_decode(fa_codec* c, const uint8_t* data, size_t size)
{
    pcm_state* st = (pcm_state*)c->impl;
    size_t frame_bytes = (size_t)sample_bytes(st->layout->kind) * c->channels, whole;
    fa_frames n;
    fa_result r;
    const void* plane[1];
    c->out_frames = 0;
    /* Finish a frame left over from the previous packet first. */
    if (st->carry_len > 0) {
        size_t k = frame_bytes - st->carry_len;
        if (k > size) k = size;
        memcpy(st->carry + st->carry_len, data, k);
        st->carry_len += k;
        data += k;
        size -= k;
        if (st->carry_len == frame_bytes) {
            r = fa_codec_reserve(c, 1, c->channels);
            if (r != FA_OK) return r;
            plane[0] = st->carry;
            fa_convert_to_f32(plane, st->layout->kind, 0, st->layout->big_endian, c->channels, 1, c->out);
            c->out_frames = 1;
            st->carry_len = 0;
        }
    }
    n = (fa_frames)(size / frame_bytes);
    whole = (size_t)n * frame_bytes;
    if (n > 0) {
        r = fa_codec_reserve(c, n, c->channels);
        if (r != FA_OK) return r;
        plane[0] = data;
        fa_convert_to_f32(plane, st->layout->kind, 0, st->layout->big_endian, c->channels, n,
                          c->out + (size_t)c->out_frames * c->channels);
        c->out_frames += n;
    }
    if (size > whole) {
        memcpy(st->carry, data + whole, size - whole);
        st->carry_len = size - whole;
    }
    return FA_OK;
}

static void pcm_flush(fa_codec* c) { ((pcm_state*)c->impl)->carry_len = 0; }
static void free_impl(fa_codec* c) { free(c->impl); c->impl = NULL; }

static const fa_codec_vtable pcm_vtable = { "pcm", pcm_decode, pcm_flush, free_impl };

static fa_result pcm_open(fa_codec* c, const fa_codec_params* p, const pcm_layout* layout)
{
    pcm_state* st;
    if (p->sample_rate == 0 || p->channels == 0 || p->channels > 8) return FA_ERROR_UNSUPPORTED_FORMAT;
    st = (pcm_state*)calloc(1, sizeof(*st));
    if (!st) return FA_ERROR_OUT_OF_MEMORY;
    st->layout = layout;
    c->impl = st;
    c->vt = &pcm_vtable;
    c->rate = p->sample_rate;
    c->channels = p->channels;
    return FA_OK;
}

/* ---- MP3 (minimp3 inside miniaudio) -------------------------------------------- */

/* dr_mp3's frame decoder lives in miniaudio_impl.c, where its API is visible. */
void* fa_mp3dec_create(void);
/* Decodes the first frame in data[0..size); returns the bytes used (0 = needs more
 * data) and writes up to 1152 interleaved frames to pcm. */
int fa_mp3dec_decode(void* dec, const uint8_t* data, int size, float* pcm, int* out_frames,
                     int* out_channels, int* out_rate);
void fa_mp3dec_reset(void* dec);

typedef struct mp3_state {
    void* dec;
    uint8_t* buf;        /* bytes not decoded yet (a frame can span packets) */
    size_t len, cap;
} mp3_state;

static fa_result mp3_decode(fa_codec* c, const uint8_t* data, size_t size)
{
    mp3_state* st = (mp3_state*)c->impl;
    size_t pos = 0;
    c->out_frames = 0;
    if (st->len + size > st->cap) {
        size_t cap = st->cap ? st->cap : 8192;
        uint8_t* b;
        while (cap < st->len + size) cap *= 2;
        b = (uint8_t*)realloc(st->buf, cap);
        if (!b) return FA_ERROR_OUT_OF_MEMORY;
        st->buf = b;
        st->cap = cap;
    }
    memcpy(st->buf + st->len, data, size);
    st->len += size;
    for (;;) {
        float pcm[1152 * 2];
        int frames = 0, ch = 0, rate = 0, used;
        if (pos >= st->len) break;
        used = fa_mp3dec_decode(st->dec, st->buf + pos, (int)(st->len - pos), pcm, &frames, &ch, &rate);
        if (used <= 0) break;
        pos += (size_t)used;
        if (frames <= 0 || ch <= 0) continue;   /* skipped junk or an ID3 tag */
        if (c->out_frames > 0 && ((uint32_t)ch != c->channels || (uint32_t)rate != c->rate)) {
            pos -= (size_t)used;                /* format change: hand out what we have first */
            break;
        }
        if (fa_codec_reserve(c, frames, (uint32_t)ch) != FA_OK) return FA_ERROR_OUT_OF_MEMORY;
        c->channels = (uint32_t)ch;
        c->rate = (uint32_t)rate;
        memcpy(c->out + (size_t)c->out_frames * c->channels, pcm, (size_t)frames * ch * sizeof(float));
        c->out_frames += frames;
    }
    memmove(st->buf, st->buf + pos, st->len - pos);
    st->len -= pos;
    if (st->len > 65536) st->len = 0;           /* garbage that never syncs: drop it */
    return FA_OK;
}

static void mp3_flush(fa_codec* c)
{
    mp3_state* st = (mp3_state*)c->impl;
    st->len = 0;
    fa_mp3dec_reset(st->dec);
}

static void mp3_close(fa_codec* c)
{
    mp3_state* st = (mp3_state*)c->impl;
    if (!st) return;
    free(st->dec);
    free(st->buf);
    free(st);
    c->impl = NULL;
}

static const fa_codec_vtable mp3_vtable = { "mp3", mp3_decode, mp3_flush, mp3_close };

static fa_result mp3_open(fa_codec* c, const fa_codec_params* p)
{
    mp3_state* st = (mp3_state*)calloc(1, sizeof(*st));
    if (!st) return FA_ERROR_OUT_OF_MEMORY;
    st->dec = fa_mp3dec_create();
    if (!st->dec) { free(st); return FA_ERROR_OUT_OF_MEMORY; }
    c->impl = st;
    c->vt = &mp3_vtable;
    c->rate = p->sample_rate;
    c->channels = p->channels;
    return FA_OK;
}

/* ---- dispatch ------------------------------------------------------------------- */

fa_result fa_codec_open(fa_codec* c, const fa_codec_params* p)
{
    size_t i;
    fa_result r;
    memset(c, 0, sizeof(*c));
    snprintf(c->name, sizeof(c->name), "%s", p->codec);
    for (i = 0; i < sizeof(pcm_layouts) / sizeof(pcm_layouts[0]); i++)
        if (strcmp(p->codec, pcm_layouts[i].name) == 0) return pcm_open(c, p, &pcm_layouts[i]);
    if (strcmp(p->codec, "mp3") == 0) return mp3_open(c, p);
    r = fa_ffmpeg_open_codec(c, p);
    if (r != FA_OK) {
        free(c->out);
        memset(c, 0, sizeof(*c));
    }
    return r;
}
