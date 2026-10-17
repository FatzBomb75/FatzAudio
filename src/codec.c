/* Packet codecs: one compressed packet in, interleaved F32 out. The built-in ones
 * (PCM in all its byte layouts, MP3, AC-3 and E-AC-3 in dec_ac3.c, Opus and
 * Vorbis in codec_xiph.c, FLAC in codec_flac.c) need nothing else; Windows Media
 * Audio uses Media Foundation on Windows (codec_mf.c); every other codec goes to
 * the app's FFmpeg, loaded at runtime (ffmpeg.c). */
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

fa_result fa_float_reserve(float** buf, fa_frames* cap, fa_frames frames, uint32_t channels)
{
    fa_frames need = frames * (fa_frames)channels;
    if (need > *cap) {
        fa_frames c = *cap ? *cap : 8192;
        float* p;
        while (c < need) c *= 2;
        p = (float*)realloc(*buf, (size_t)c * sizeof(float));
        if (!p) return FA_ERROR_OUT_OF_MEMORY;
        *buf = p;
        *cap = c;
    }
    return FA_OK;
}

fa_result fa_pcm_convert(fa_pcm_conv* c, const float* in, fa_frames frames, uint32_t rate, uint32_t channels,
                         uint32_t out_rate, uint32_t out_channels, float** buf, fa_frames* cap, fa_frames* out_frames)
{
    ma_uint64 expect = 0;
    fa_frames done_in = 0, done_out = 0;
    fa_result r;
    *out_frames = 0;
    if (!c->ready || c->rate != rate || c->channels != channels) {
        ma_data_converter_config cc;
        if (c->ready) { ma_data_converter_uninit(&c->conv, NULL); c->ready = 0; }
        cc = ma_data_converter_config_init(ma_format_f32, ma_format_f32, channels, out_channels, rate, out_rate);
        if (ma_data_converter_init(&cc, NULL, &c->conv) != MA_SUCCESS) return FA_ERROR_OUT_OF_MEMORY;
        c->ready = 1;
        c->rate = rate;
        c->channels = channels;
    }
    ma_data_converter_get_expected_output_frame_count(&c->conv, (ma_uint64)frames, &expect);
    r = fa_float_reserve(buf, cap, (fa_frames)expect + 64, out_channels);
    if (r != FA_OK) return r;
    while (done_in < frames) {
        ma_uint64 in_n = (ma_uint64)(frames - done_in);
        ma_uint64 out_n = (ma_uint64)(*cap / out_channels - done_out);
        if (out_n == 0) {
            r = fa_float_reserve(buf, cap, done_out * 2 + 64, out_channels);
            if (r != FA_OK) return r;
            continue;
        }
        ma_data_converter_process_pcm_frames(&c->conv, in + (size_t)done_in * channels, &in_n,
                                             *buf + (size_t)done_out * out_channels, &out_n);
        done_in += (fa_frames)in_n;
        done_out += (fa_frames)out_n;
        if (in_n == 0 && out_n == 0) break;
    }
    *out_frames = done_out;
    return FA_OK;
}

void fa_pcm_conv_reset(fa_pcm_conv* c)
{
    if (c->ready) ma_data_converter_reset(&c->conv);
}

void fa_pcm_conv_free(fa_pcm_conv* c)
{
    if (c->ready) ma_data_converter_uninit(&c->conv, NULL);
    c->ready = 0;
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
    uint8_t carry[32 * 8];  /* the bytes of a frame split across two packets (32 channels of 64 bits) */
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
    if (!data || size == 0) return FA_OK;   /* the end: no delayed output */
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
    if (p->sample_rate == 0 || p->channels == 0 || p->channels > 32) return FA_ERROR_UNSUPPORTED_FORMAT;
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

/* Length of the MPEG audio frame whose header is at h (4 bytes): 0 if h is not
 * a valid header, -1 for free-format frames (length not in the header). */
static long mpa_frame_len(const uint8_t* h)
{
    static const uint16_t br[2][3][15] = {
        { { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448 },     /* MPEG-1 layer I */
          { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384 },        /* layer II */
          { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 } },       /* layer III */
        { { 0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256 },        /* MPEG-2/2.5 layer I */
          { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 },
          { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 } } };
    static const uint32_t sr[3] = { 44100, 48000, 32000 };
    int ver, layer, bri, sri, pad, v2;
    uint32_t rate, kbps;
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return 0;
    ver = (h[1] >> 3) & 3;              /* 0 = 2.5, 2 = 2, 3 = 1 */
    layer = 4 - ((h[1] >> 1) & 3);      /* 1..3, 4 = reserved */
    bri = h[2] >> 4;
    sri = (h[2] >> 2) & 3;
    pad = (h[2] >> 1) & 1;
    if (ver == 1 || layer == 4 || bri == 15 || sri == 3) return 0;
    if (bri == 0) return -1;
    v2 = ver != 3;
    rate = sr[sri] >> (ver == 3 ? 0 : ver == 2 ? 1 : 2);
    kbps = br[v2][layer - 1][bri];
    if (layer == 1) return (long)((12 * kbps * 1000 / rate + pad) * 4);
    if (layer == 3 && v2) return (long)(72 * kbps * 1000 / rate + pad);
    return (long)(144 * kbps * 1000 / rate + pad);
}

/* A tag after the last frame (ID3v1, ID3v2, APE, Lyrics3). */
static int is_tag(const uint8_t* h)
{
    return memcmp(h, "TAG", 3) == 0 || memcmp(h, "ID3", 3) == 0 || memcmp(h, "APET", 4) == 0 || memcmp(h, "LYRI", 4) == 0;
}

/* Bytes pass through a FIFO and go to the decoder one whole frame at a time: a
 * frame split across packets waits for the rest instead of being skipped. */
static fa_result mp3_decode(fa_codec* c, const uint8_t* data, size_t size)
{
    mp3_state* st = (mp3_state*)c->impl;
    size_t pos = 0;
    int ending = !data && size == 0;
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
    if (size > 0) memcpy(st->buf + st->len, data, size);
    st->len += size;
    for (;;) {
        float pcm[1152 * 2];
        int frames = 0, ch = 0, rate = 0, used;
        long flen;
        size_t avail;
        /* Sync: the next valid header whose frame is followed by another header
         * (or ends the data we have). Junk before it is dropped. */
        while (pos + 4 <= st->len) {
            flen = mpa_frame_len(st->buf + pos);
            if (flen == 0) { pos++; continue; }
            if (flen > 0 && pos + (size_t)flen + 4 <= st->len && mpa_frame_len(st->buf + pos + flen) == 0 &&
                !is_tag(st->buf + pos + flen)) { pos++; continue; }
            break;
        }
        if (pos + 4 > st->len) {
            if (pos > st->len) pos = st->len;
            if (!ending && st->len - pos < 4) break;   /* keep a possible header start */
            pos = st->len;
            break;
        }
        flen = mpa_frame_len(st->buf + pos);
        avail = st->len - pos;
        if (flen > 0 && (size_t)flen > avail) {
            if (ending) pos = st->len;                  /* a cut-off last frame */
            break;                                      /* the rest comes with the next packet */
        }
        /* Free format: the decoder finds the length itself from the next header. */
        used = fa_mp3dec_decode(st->dec, st->buf + pos, flen > 0 ? (int)flen : (int)avail, pcm, &frames, &ch, &rate);
        if (used <= 0) {
            if (flen < 0 && !ending) break;             /* needs the next header first */
            pos += flen > 0 ? (size_t)flen : 1;
            continue;
        }
        if (frames <= 0 || ch <= 0) {                   /* not audio after all (a false sync) */
            pos += flen > 0 ? (size_t)flen : (size_t)used;
            continue;
        }
        if (c->out_frames > 0 && ((uint32_t)ch != c->channels || (uint32_t)rate != c->rate))
            break;                                      /* format change: hand out what we have first */
        if (fa_codec_reserve(c, frames, (uint32_t)ch) != FA_OK) return FA_ERROR_OUT_OF_MEMORY;
        c->channels = (uint32_t)ch;
        c->rate = (uint32_t)rate;
        memcpy(c->out + (size_t)c->out_frames * c->channels, pcm, (size_t)frames * ch * sizeof(float));
        c->out_frames += frames;
        pos += flen > 0 ? (size_t)flen : (size_t)used;
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
    for (i = 0; i < sizeof(pcm_layouts) / sizeof(pcm_layouts[0]); i++) {
        if (strcmp(p->codec, pcm_layouts[i].name) == 0) {
            r = pcm_open(c, p, &pcm_layouts[i]);
            if (r != FA_ERROR_UNSUPPORTED_FORMAT) return r;
            break;                         /* more channels than ours: FFmpeg may take it */
        }
    }
    if (strcmp(p->codec, "mp3") == 0) return mp3_open(c, p);
    if (strcmp(p->codec, "ac3") == 0 || strcmp(p->codec, "eac3") == 0) return fa_ac3_codec_open(c, p);
    if (strcmp(p->codec, "flac") == 0) {
        /* Built in; without STREAMINFO or usable parameters it goes to FFmpeg. */
        r = fa_flac_codec_open(c, p);
        if (r != FA_ERROR_UNSUPPORTED_FORMAT) return r;
        memset(c, 0, sizeof(*c));
        snprintf(c->name, sizeof(c->name), "%s", p->codec);
    }
    if (strcmp(p->codec, "opus") == 0 || strcmp(p->codec, "vorbis") == 0) {
        /* Built in; what libopus cannot set up alone (Opus projection) goes to FFmpeg. */
        r = fa_xiph_codec_open(c, p);
        if (r != FA_ERROR_UNSUPPORTED_FORMAT) return r;
        memset(c, 0, sizeof(*c));
        snprintf(c->name, sizeof(c->name), "%s", p->codec);
    }
#ifdef _WIN32
    r = fa_mf_codec_open(c, p);
    if (r != FA_ERROR_UNSUPPORTED_FORMAT) return r;   /* else not WMA, or no Media Foundation: FFmpeg */
    memset(c, 0, sizeof(*c));
    snprintf(c->name, sizeof(c->name), "%s", p->codec);
#endif
    r = fa_ffmpeg_open_codec(c, p);
    if (r != FA_OK) {
        free(c->out);
        memset(c, 0, sizeof(*c));
    }
    return r;
}
