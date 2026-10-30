/* Packet codecs: one compressed packet in, interleaved F32 out. The built-in ones
 * (PCM in all its byte layouts, MP3, AAC in aac.c, AC-3 and E-AC-3 in dec_ac3.c,
 * Opus and Vorbis in codec_xiph.c, FLAC in codec_flac.c, ALAC in codec_alac.c) need
 * nothing else. The operating system's decoders come next: Media Foundation on
 * Windows for Windows Media Audio (codec_mf.c), and for xHE-AAC, which aac.c does
 * not decode, AudioToolbox on macOS and iOS (codec_at.c) and MediaCodec on Android
 * (codec_mediacodec.c). Every other codec is unsupported (FA_ERROR_UNSUPPORTED_FORMAT):
 * an app with its own decoder for it pushes the decoded frames instead
 * (fa_stream_push_frame). */
#include "fa_internal.h"
#include "demux.h"
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

/* Unsigned integers of 2, 4 and 8 bytes in the given byte order. */
static uint32_t rd16(const uint8_t* p, int big) { return big ? fa_be16(p) : fa_le16(p); }
static uint32_t rd32(const uint8_t* p, int big) { return big ? fa_be32(p) : fa_le32(p); }
static uint64_t rd64(const uint8_t* p, int big) { return big ? fa_be64(p) : fa_le64(p); }

/* n samples of one kind, `step` bytes apart, to floats `out_step` apart. One
 * loop per kind, so the format is not looked at again for every sample.
 * `swap`: the bytes are in the other order than this machine's. */
static void convert_run(const uint8_t* p, size_t step, int kind, int swap, float* out, size_t out_step, size_t n)
{
    int big = swap ? !native_big_endian() : native_big_endian();
    size_t i;
    switch (kind) {
    case FA_SAMPLE_U8:
        for (i = 0; i < n; i++) out[i * out_step] = ((float)p[i * step] - 128.0f) * (1.0f / 128.0f);
        break;
    case FA_SAMPLE_S8:
        for (i = 0; i < n; i++) out[i * out_step] = (float)(int8_t)p[i * step] * (1.0f / 128.0f);
        break;
    case FA_SAMPLE_S16:
        for (i = 0; i < n; i++) out[i * out_step] = (float)(int16_t)rd16(p + i * step, big) * (1.0f / 32768.0f);
        break;
    case FA_SAMPLE_S24:
        /* Packed 3 bytes, assembled unsigned: shifting a byte into a signed int's
         * sign bit is undefined. */
        for (i = 0; i < n; i++) {
            const uint8_t* q = p + i * step;
            uint32_t u = big ? ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8)
                             : ((uint32_t)q[2] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[0] << 8);
            out[i * out_step] = (float)((int32_t)u >> 8) * (1.0f / 8388608.0f);
        }
        break;
    case FA_SAMPLE_S32:
        for (i = 0; i < n; i++)
            out[i * out_step] = (float)((double)(int32_t)rd32(p + i * step, big) * (1.0 / 2147483648.0));
        break;
    case FA_SAMPLE_S64:
        for (i = 0; i < n; i++)
            out[i * out_step] = (float)((double)(int64_t)rd64(p + i * step, big) * (1.0 / 9223372036854775808.0));
        break;
    case FA_SAMPLE_F32:
        for (i = 0; i < n; i++) {
            uint32_t u = rd32(p + i * step, big);
            float v;
            memcpy(&v, &u, 4);
            out[i * out_step] = (u & 0x7F800000u) == 0x7F800000u ? 0.0f : v;
        }
        break;
    default:
        for (i = 0; i < n; i++) {
            uint64_t u = rd64(p + i * step, big);
            double v;
            float f;
            memcpy(&v, &u, 8);
            f = (float)v;
            out[i * out_step] = f - f == 0.0f ? f : 0.0f;   /* NaN, infinity or out of float range */
        }
        break;
    }
}

void fa_zero_non_finite(float* p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        uint32_t u;
        memcpy(&u, p + i, 4);
        if ((u & 0x7F800000u) == 0x7F800000u) p[i] = 0.0f;
    }
}

void fa_convert_to_f32(const void* const* planes, int kind, int planar, int big_endian,
                       uint32_t channels, fa_frames frames, float* out)
{
    size_t bytes = (size_t)sample_bytes(kind);
    int swap = big_endian && !native_big_endian();
    uint32_t c;
    if (kind == FA_SAMPLE_F32 && !planar && !swap) {
        memcpy(out, planes[0], (size_t)frames * channels * sizeof(float));
        fa_zero_non_finite(out, (size_t)frames * channels);
        return;
    }
    if (planar)
        for (c = 0; c < channels; c++)
            convert_run((const uint8_t*)planes[c], bytes, kind, swap, out + c, channels, (size_t)frames);
    else
        convert_run((const uint8_t*)planes[0], bytes, kind, swap, out, 1, (size_t)frames * channels);
}

/* ---- shared helpers ----------------------------------------------------------- */

fa_result fa_codec_reserve(fa_codec* c, fa_frames frames, uint32_t channels)
{
    fa_frames need = (c->out_frames + frames) * (fa_frames)channels;
    if (need > c->out_cap) {
        /* The first packet sets the size (most codecs keep theirs); doubling after. */
        fa_frames cap = c->out_cap ? c->out_cap : (need > 4096 ? need : 4096);
        float* p;
        while (cap < need) cap *= 2;
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

/* Runs `frames` frames of `in` through the converter, writing from (*buf)[done]. */
static fa_result conv_run(fa_pcm_conv* c, const float* in, fa_frames frames, uint32_t out_channels, float** buf,
                          fa_frames* cap, fa_frames* done)
{
    ma_uint64 expect = 0;
    fa_frames done_in = 0;
    fa_result r;
    ma_data_converter_get_expected_output_frame_count(&c->conv, (ma_uint64)frames, &expect);
    r = fa_float_reserve(buf, cap, *done + (fa_frames)expect + 64, out_channels);
    if (r != FA_OK) return r;
    while (done_in < frames) {
        ma_uint64 in_n = (ma_uint64)(frames - done_in);
        ma_uint64 out_n = (ma_uint64)(*cap / out_channels - *done);
        if (out_n == 0) {
            r = fa_float_reserve(buf, cap, *done * 2 + 64, out_channels);
            if (r != FA_OK) return r;
            continue;
        }
        ma_data_converter_process_pcm_frames(&c->conv, in + (size_t)done_in * c->channels, &in_n,
                                             *buf + (size_t)*done * out_channels, &out_n);
        done_in += (fa_frames)in_n;
        *done += (fa_frames)out_n;
        if (in_n == 0 && out_n == 0) break;
    }
    if (frames > 0) c->pending = 1;
    return FA_OK;
}

/* Gets the frames the resampler still holds out (its look-ahead) by feeding it
 * its input latency in silence, writing from (*buf)[done]; then starts it over. */
static fa_result conv_drain(fa_pcm_conv* c, uint32_t out_channels, float** buf, fa_frames* cap, fa_frames* done)
{
    static const float zeros[1024];
    fa_frames left, step;
    fa_result r = FA_OK;
    if (!c->ready || !c->pending) return FA_OK;
    step = (fa_frames)(sizeof(zeros) / sizeof(zeros[0]) / c->channels);
    /* More channels than a frame of the zeros holds: the tail is dropped. */
    left = step > 0 ? (fa_frames)ma_data_converter_get_input_latency(&c->conv) : 0;
    while (left > 0 && r == FA_OK) {
        fa_frames n = left < step ? left : step;
        r = conv_run(c, zeros, n, out_channels, buf, cap, done);
        left -= n;
    }
    ma_data_converter_reset(&c->conv);
    c->pending = 0;
    return r;
}

fa_result fa_pcm_convert(fa_pcm_conv* c, const float* in, fa_frames frames, uint32_t rate, uint32_t channels,
                         uint32_t out_rate, uint32_t out_channels, float** buf, fa_frames* cap, fa_frames* out_frames)
{
    fa_frames done = 0;
    fa_result r;
    *out_frames = 0;
    /* What the converter holds of the previous format comes out first. */
    if (c->ready && (c->rate != rate || c->channels != channels || (rate == out_rate && channels == out_channels))) {
        r = conv_drain(c, out_channels, buf, cap, &done);
        if (r != FA_OK) return r;
    }
    if (rate == out_rate && channels == out_channels) {
        r = fa_float_reserve(buf, cap, done + frames, out_channels);
        if (r != FA_OK) return r;
        memcpy(*buf + (size_t)done * out_channels, in, (size_t)frames * channels * sizeof(float));
        *out_frames = done + frames;
        return FA_OK;
    }
    if (!c->ready || c->rate != rate || c->channels != channels) {
        ma_data_converter_config cc;
        if (c->ready) { ma_data_converter_uninit(&c->conv, NULL); c->ready = 0; }
        cc = ma_data_converter_config_init(ma_format_f32, ma_format_f32, channels, out_channels, rate, out_rate);
        cc.resampling.algorithm = ma_resample_algorithm_custom;   /* the sinc resampler (resample_device.c) */
        cc.resampling.pBackendVTable = &fa_sinc_resampler;
        if (ma_data_converter_init(&cc, NULL, &c->conv) != MA_SUCCESS) {
            /* Beyond its widest ratio: miniaudio's linear one. */
            cc.resampling.algorithm = ma_resample_algorithm_linear;
            cc.resampling.pBackendVTable = NULL;
            if (ma_data_converter_init(&cc, NULL, &c->conv) != MA_SUCCESS) return FA_ERROR_OUT_OF_MEMORY;
        }
        c->ready = 1;
        c->pending = 0;
        c->rate = rate;
        c->channels = channels;
    }
    r = conv_run(c, in, frames, out_channels, buf, cap, &done);
    *out_frames = done;
    return r;
}

fa_result fa_pcm_conv_flush(fa_pcm_conv* c, uint32_t out_channels, float** buf, fa_frames* cap, fa_frames* out_frames)
{
    return conv_drain(c, out_channels, buf, cap, out_frames);
}

fa_frames fa_pcm_conv_held(fa_pcm_conv* c)
{
    return c->ready && c->pending ? (fa_frames)ma_data_converter_get_output_latency(&c->conv) : 0;
}

void fa_pcm_conv_reset(fa_pcm_conv* c)
{
    if (c->ready) ma_data_converter_reset(&c->conv);
    c->pending = 0;
}

void fa_pcm_conv_free(fa_pcm_conv* c)
{
    if (c->ready) ma_data_converter_uninit(&c->conv, NULL);
    c->ready = 0;
    c->pending = 0;
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

typedef struct mp3_state {
    void* dec;
    uint8_t* buf;        /* bytes not decoded yet (a frame can span packets) */
    size_t len, cap;
} mp3_state;

/* MPEG-1 sampling rates by header index (halved for MPEG-2, quartered for 2.5). */
static const uint32_t mpa_rates[3] = { 44100, 48000, 32000 };

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
    rate = mpa_rates[sri] >> (ver == 3 ? 0 : ver == 2 ? 1 : 2);
    kbps = br[v2][layer - 1][bri];
    if (layer == 1) return (long)((12 * kbps * 1000 / rate + pad) * 4);
    if (layer == 3 && v2) return (long)(72 * kbps * 1000 / rate + pad);
    return (long)(144 * kbps * 1000 / rate + pad);
}

/* Channels and rate of the frame whose (valid) header is at h. */
static void mpa_format(const uint8_t* h, uint32_t* channels, uint32_t* rate)
{
    int ver = (h[1] >> 3) & 3;
    *channels = (h[3] >> 6) == 3 ? 1u : 2u;
    *rate = mpa_rates[(h[2] >> 2) & 3] >> (ver == 3 ? 0 : ver == 2 ? 1 : 2);
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
        if (c->out_frames > 0) {
            /* A format change: hand out what we have first. Checked before decoding,
             * as the decoder's bit reservoir takes in the frame's tail when it runs. */
            uint32_t fch, frate;
            mpa_format(st->buf + pos, &fch, &frate);
            if (fch != c->channels || frate != c->rate) break;
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
        if (c->out_frames > 0 && ((uint32_t)ch != c->channels || (uint32_t)rate != c->rate)) {
            pos += flen > 0 ? (size_t)flen : (size_t)used;  /* disagrees with its header: dropped */
            continue;
        }
        if (fa_codec_reserve(c, frames, (uint32_t)ch) != FA_OK) return FA_ERROR_OUT_OF_MEMORY;
        c->channels = (uint32_t)ch;
        c->rate = (uint32_t)rate;
        memcpy(c->out + (size_t)c->out_frames * c->channels, pcm, (size_t)frames * ch * sizeof(float));
        c->out_frames += frames;
        pos += flen > 0 ? (size_t)flen : (size_t)used;
    }
    if (pos > 0) memmove(st->buf, st->buf + pos, st->len - pos);
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

#if defined(_WIN32) || defined(__APPLE__) || defined(__ANDROID__)
#define FA_OS_CODECS 1
#else
#define FA_OS_CODECS 0
#endif

#if FA_OS_CODECS

/* ---- The system's decoders ------------------------------------------------------- */

static fa_result os_codec_open(fa_codec* c, const fa_codec_params* p)
{
#if defined(_WIN32)
    return fa_mf_codec_open(c, p);
#elif defined(__APPLE__)
    return fa_at_codec_open(c, p);
#else
    return fa_mc_codec_open(c, p);
#endif
}

#endif /* FA_OS_CODECS */

fa_result fa_codec_open(fa_codec* c, const fa_codec_params* p)
{
    size_t i;
    fa_result r = FA_ERROR_UNSUPPORTED_FORMAT;
    memset(c, 0, sizeof(*c));
    snprintf(c->name, sizeof(c->name), "%s", p->codec);
    for (i = 0; i < sizeof(pcm_layouts) / sizeof(pcm_layouts[0]); i++)
        if (strcmp(p->codec, pcm_layouts[i].name) == 0) break;
    if (i < sizeof(pcm_layouts) / sizeof(pcm_layouts[0])) r = pcm_open(c, p, &pcm_layouts[i]);
    else if (strcmp(p->codec, "mp3") == 0) r = mp3_open(c, p);
    else if (strcmp(p->codec, "ac3") == 0 || strcmp(p->codec, "eac3") == 0) r = fa_ac3_codec_open(c, p);
    else if (strcmp(p->codec, "flac") == 0) r = fa_flac_codec_open(c, p);
    else if (strcmp(p->codec, "opus") == 0 || strcmp(p->codec, "vorbis") == 0) r = fa_xiph_codec_open(c, p);
    else if (strcmp(p->codec, "alac") == 0) r = fa_alac_codec_open(c, p);
    else {
        /* AAC is built in: AAC-LC, HE-AAC, HE-AAC v2, Main and LTP, in ADTS or after
         * an AudioSpecificConfig. */
        if (strcmp(p->codec, "aac") == 0) {
            r = fa_aac_codec_open(c, p);
            if (r == FA_ERROR_UNSUPPORTED_FORMAT) {
                memset(c, 0, sizeof(*c));
                snprintf(c->name, sizeof(c->name), "%s", p->codec);
            }
        }
        /* The system's own decoders: Windows Media Audio, and xHE-AAC where the
         * system has it. Anything else is unsupported. */
#if FA_OS_CODECS
        if (r == FA_ERROR_UNSUPPORTED_FORMAT) r = os_codec_open(c, p);
#endif
    }
    if (r != FA_OK) {
        free(c->out);
        memset(c, 0, sizeof(*c));
    }
    return r;
}
