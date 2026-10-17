/* Opus and Vorbis packet codecs, built in: libopus and libvorbis (BSD), compiled
 * into the library (third_party/opus, third_party/vorbis). Both decode to F32.
 *
 * Opus: the OpusHead of RFC 7845 (Ogg's first packet, Matroska's CodecPrivate,
 * MP4's dOps rewritten) sets up a multistream decoder for any channel mapping
 * family libopus decodes without extra state (0, 1, 2 and 255; family 3 goes to
 * FFmpeg). The header's output gain is applied, and its pre-skip is dropped from
 * the first packets after opening, like FFmpeg does.
 *
 * Vorbis: the three headers come Xiph-laced (Matroska, MP4 and FFmpeg's
 * extradata) or each with a 16-bit length in front.
 *
 * Both specs number multichannel layouts in their own order (L C R ...); the
 * output is the library's standard order for the channel count (see ac3.c), with
 * quadraphonic sound as 5 channels and a silent centre. */
#include "fa_internal.h"
#include <opus_multistream.h>
#include <vorbis/codec.h>
#include <stdlib.h>
#include <string.h>

/* out[i] = in[map[i]]; -1 = silent. Vorbis channel order -> the library's order. */
static const int8_t xiph_order[9][8] = {
    { 0 },
    { 0 },
    { 0, 1 },
    { 0, 2, 1 },
    { 0, 1, -1, 2, 3 },                 /* quad -> L R (C) BL BR */
    { 0, 2, 1, 3, 4 },
    { 0, 2, 1, 5, 3, 4 },
    { 0, 2, 1, 6, 5, 3, 4 },
    { 0, 2, 1, 7, 5, 6, 3, 4 },
};

/* Output channels for `in` coded channels in Vorbis order. */
static uint32_t xiph_out_channels(uint32_t in) { return in == 4 ? 5 : in; }

/* Interleaves planes (Vorbis) or reorders interleaved frames (Opus, planes NULL)
 * from `in` channels in Vorbis order into `out`. */
static void xiph_reorder(const float* const* planes, const float* inter, uint32_t in, fa_frames frames, float* out)
{
    uint32_t oc = xiph_out_channels(in), c;
    fa_frames f;
    for (c = 0; c < oc; c++) {
        int src = in <= 8 ? xiph_order[in][c] : (int)c;
        float* o = out + c;
        if (src < 0) {
            for (f = 0; f < frames; f++, o += oc) *o = 0.0f;
        } else if (planes) {
            const float* p = planes[src];
            for (f = 0; f < frames; f++, o += oc) *o = p[f];
        } else {
            const float* p = inter + src;
            for (f = 0; f < frames; f++, o += oc, p += in) *o = *p;
        }
    }
}

static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* ---- Opus ---------------------------------------------------------------------- */

#define OPUS_MAX_FRAME 5760          /* 120 ms at 48 kHz */

typedef struct opus_head {
    uint32_t channels, preskip, streams, coupled;
    int16_t gain;                    /* Q7.8 dB */
    int family;
    uint8_t mapping[255];
} opus_head;

/* RFC 7845, 5.1. 0 if the header is not one libopus can decode here. */
static int parse_opus_head(const uint8_t* p, size_t n, opus_head* h)
{
    uint32_t i;
    memset(h, 0, sizeof(*h));
    if (n < 19 || memcmp(p, "OpusHead", 8) != 0 || (p[8] >> 4) != 0 || p[9] == 0) return 0;
    h->channels = p[9];
    h->preskip = le16(p + 10);
    h->gain = (int16_t)le16(p + 16);
    h->family = p[18];
    if (h->family == 0) {
        if (h->channels > 2) return 0;
        h->streams = 1;
        h->coupled = h->channels - 1;
        h->mapping[0] = 0;
        h->mapping[1] = 1;
        return 1;
    }
    /* Family 3 (projection) needs its demixing matrix: FFmpeg's job. */
    if (h->family == 3 || n < 21 + (size_t)h->channels) return 0;
    h->streams = p[19];
    h->coupled = p[20];
    if (h->streams == 0 || h->coupled > h->streams || h->streams + h->coupled > 255) return 0;
    for (i = 0; i < h->channels; i++) {
        h->mapping[i] = p[21 + i];
        if (h->mapping[i] != 255 && h->mapping[i] >= h->streams + h->coupled) return 0;
    }
    if (h->family == 1 && h->channels > 8) return 0;
    return 1;
}

typedef struct opus_state {
    OpusMSDecoder* dec;
    opus_head head;
    uint32_t skip;                   /* pre-skip frames still to drop */
    int reorder;                     /* family 1: Vorbis channel order */
    float pcm[OPUS_MAX_FRAME * 8];   /* decoded, before reordering (families 0 and 1) */
    float* wide;                     /* more than 8 channels: decoded straight to here */
    size_t wide_cap;
} opus_state;

static fa_result opus_pkt_decode(fa_codec* c, const uint8_t* data, size_t size)
{
    opus_state* st = (opus_state*)c->impl;
    uint32_t in = st->head.channels;
    float* pcm = st->pcm;
    int n;
    fa_result r;
    c->out_frames = 0;
    if (!data || size == 0) return FA_OK;           /* nothing held back */
    if (size > 0x7FFFFFFF) return FA_ERROR_UNSUPPORTED_FORMAT;
    if (in > 8) {
        size_t need = (size_t)OPUS_MAX_FRAME * in;
        if (need > st->wide_cap) {
            float* w = (float*)realloc(st->wide, need * sizeof(float));
            if (!w) return FA_ERROR_OUT_OF_MEMORY;
            st->wide = w;
            st->wide_cap = need;
        }
        pcm = st->wide;
    }
    n = opus_multistream_decode_float(st->dec, data, (opus_int32)size, pcm, OPUS_MAX_FRAME, 0);
    if (n < 0) return FA_ERROR_UNSUPPORTED_FORMAT;  /* corrupt packet: skipped */
    if (st->skip > 0) {
        uint32_t k = st->skip < (uint32_t)n ? st->skip : (uint32_t)n;
        st->skip -= k;
        pcm += (size_t)k * in;
        n -= (int)k;
    }
    if (n == 0) return FA_OK;
    r = fa_codec_reserve(c, n, c->channels);
    if (r != FA_OK) return r;
    if (st->reorder) xiph_reorder(NULL, pcm, in, n, c->out);
    else memcpy(c->out, pcm, (size_t)n * in * sizeof(float));
    c->out_frames = n;
    return FA_OK;
}

static void opus_pkt_flush(fa_codec* c)
{
    opus_state* st = (opus_state*)c->impl;
    opus_multistream_decoder_ctl(st->dec, OPUS_RESET_STATE);
}

static void opus_pkt_close(fa_codec* c)
{
    opus_state* st = (opus_state*)c->impl;
    if (!st) return;
    opus_multistream_decoder_destroy(st->dec);
    free(st->wide);
    free(st);
    c->impl = NULL;
}

static const fa_codec_vtable opus_vtable = { "opus", opus_pkt_decode, opus_pkt_flush, opus_pkt_close };

static fa_result opus_codec_open(fa_codec* c, const fa_codec_params* p)
{
    opus_state* st;
    opus_head h;
    int err = 0;
    if (p->extradata && p->extradata_size > 0) {
        if (!parse_opus_head((const uint8_t*)p->extradata, p->extradata_size, &h)) return FA_ERROR_UNSUPPORTED_FORMAT;
    } else {
        /* No header (a raw packet stream): mono or stereo from the parameters. */
        if (p->channels < 1 || p->channels > 2) return FA_ERROR_UNSUPPORTED_FORMAT;
        memset(&h, 0, sizeof(h));
        h.channels = p->channels;
        h.streams = 1;
        h.coupled = p->channels - 1;
        h.mapping[1] = 1;
    }
    st = (opus_state*)calloc(1, sizeof(*st));
    if (!st) return FA_ERROR_OUT_OF_MEMORY;
    st->head = h;
    st->dec = opus_multistream_decoder_create(48000, (int)h.channels, (int)h.streams, (int)h.coupled, h.mapping, &err);
    if (!st->dec || err != OPUS_OK) { free(st); return FA_ERROR_UNSUPPORTED_FORMAT; }
    if (h.gain != 0) opus_multistream_decoder_ctl(st->dec, OPUS_SET_GAIN(h.gain));
    st->skip = h.preskip;
    st->reorder = h.family == 1 && h.channels > 2;
    c->impl = st;
    c->vt = &opus_vtable;
    c->rate = 48000;
    c->channels = st->reorder ? xiph_out_channels(h.channels) : h.channels;
    return FA_OK;
}

/* ---- Vorbis ----------------------------------------------------------------------- */

typedef struct vorbis_state {
    vorbis_info vi;
    vorbis_comment vc;
    vorbis_dsp_state vd;
    vorbis_block vb;
    int dsp_ready, block_ready;
    ogg_int64_t packetno;
} vorbis_state;

int fa_xiph_split_headers(const uint8_t* p, size_t n, const uint8_t* out[3], size_t out_len[3])
{
    size_t pos, i;
    if (n >= 6 && p[0] == 0 && p[1] >= 30) {
        /* FFmpeg's other layout: each header with a 16-bit big-endian length. */
        for (pos = 0, i = 0; i < 3; i++) {
            size_t len;
            if (pos + 2 > n) return 0;
            len = ((size_t)p[pos] << 8) | p[pos + 1];
            pos += 2;
            if (len > n - pos) return 0;
            out[i] = p + pos;
            out_len[i] = len;
            pos += len;
        }
        return 1;
    }
    /* Xiph lacing: count - 1, the sizes of all but the last (255 runs), the data. */
    if (n < 1 || p[0] != 2) return 0;
    pos = 1;
    for (i = 0; i < 2; i++) {
        size_t len = 0;
        for (;;) {
            if (pos >= n) return 0;
            len += p[pos];
            if (p[pos++] != 255) break;
        }
        out_len[i] = len;
    }
    if (out_len[0] > n - pos || out_len[1] > n - pos - out_len[0]) return 0;
    out[0] = p + pos;
    out[1] = out[0] + out_len[0];
    out[2] = out[1] + out_len[1];
    out_len[2] = n - pos - out_len[0] - out_len[1];
    return 1;
}

static void vorbis_free_state(vorbis_state* st)
{
    if (st->block_ready) vorbis_block_clear(&st->vb);
    if (st->dsp_ready) vorbis_dsp_clear(&st->vd);
    vorbis_comment_clear(&st->vc);
    vorbis_info_clear(&st->vi);
    free(st);
}

static fa_result vorbis_pkt_decode(fa_codec* c, const uint8_t* data, size_t size)
{
    vorbis_state* st = (vorbis_state*)c->impl;
    ogg_packet op;
    float** pcm;
    int n;
    c->out_frames = 0;
    if (!data || size == 0) return FA_OK;           /* nothing held back */
    if (size > 0x7FFFFFFF) return FA_ERROR_UNSUPPORTED_FORMAT;
    memset(&op, 0, sizeof(op));
    op.packet = (unsigned char*)data;
    op.bytes = (long)size;
    op.granulepos = -1;
    op.packetno = st->packetno++;
    if (data[0] & 1) return FA_OK;                  /* a header packet: not audio */
    if (vorbis_synthesis(&st->vb, &op) != 0) return FA_ERROR_UNSUPPORTED_FORMAT;
    if (vorbis_synthesis_blockin(&st->vd, &st->vb) != 0) return FA_ERROR_UNSUPPORTED_FORMAT;
    while ((n = vorbis_synthesis_pcmout(&st->vd, &pcm)) > 0) {
        fa_result r = fa_codec_reserve(c, n, c->channels);
        if (r != FA_OK) return r;
        xiph_reorder((const float* const*)pcm, NULL, (uint32_t)st->vi.channels, n,
                     c->out + (size_t)c->out_frames * c->channels);
        c->out_frames += n;
        vorbis_synthesis_read(&st->vd, n);
    }
    return FA_OK;
}

static void vorbis_pkt_flush(fa_codec* c)
{
    vorbis_state* st = (vorbis_state*)c->impl;
    vorbis_synthesis_restart(&st->vd);
}

static void vorbis_pkt_close(fa_codec* c)
{
    if (c->impl) vorbis_free_state((vorbis_state*)c->impl);
    c->impl = NULL;
}

static const fa_codec_vtable vorbis_vtable = { "vorbis", vorbis_pkt_decode, vorbis_pkt_flush, vorbis_pkt_close };

static fa_result vorbis_codec_open(fa_codec* c, const fa_codec_params* p)
{
    const uint8_t* hdr[3];
    size_t len[3];
    vorbis_state* st;
    int i;
    if (!p->extradata || !fa_xiph_split_headers((const uint8_t*)p->extradata, p->extradata_size, hdr, len))
        return FA_ERROR_UNSUPPORTED_FORMAT;
    st = (vorbis_state*)calloc(1, sizeof(*st));
    if (!st) return FA_ERROR_OUT_OF_MEMORY;
    vorbis_info_init(&st->vi);
    vorbis_comment_init(&st->vc);
    for (i = 0; i < 3; i++) {
        ogg_packet op;
        memset(&op, 0, sizeof(op));
        op.packet = (unsigned char*)hdr[i];
        op.bytes = (long)len[i];
        op.b_o_s = i == 0;
        op.packetno = i;
        op.granulepos = -1;
        if (len[i] > 0x7FFFFFFF || vorbis_synthesis_headerin(&st->vi, &st->vc, &op) != 0) {
            vorbis_free_state(st);
            return FA_ERROR_UNSUPPORTED_FORMAT;
        }
    }
    if (st->vi.channels < 1 || st->vi.rate < 1 || vorbis_synthesis_init(&st->vd, &st->vi) != 0) {
        vorbis_free_state(st);
        return FA_ERROR_UNSUPPORTED_FORMAT;
    }
    st->dsp_ready = 1;
    if (vorbis_block_init(&st->vd, &st->vb) != 0) {
        vorbis_free_state(st);
        return FA_ERROR_OUT_OF_MEMORY;
    }
    st->block_ready = 1;
    st->packetno = 3;
    c->impl = st;
    c->vt = &vorbis_vtable;
    c->rate = (uint32_t)st->vi.rate;
    c->channels = xiph_out_channels((uint32_t)st->vi.channels);
    return FA_OK;
}

/* ---- dispatch ------------------------------------------------------------------- */

fa_result fa_xiph_codec_open(fa_codec* c, const fa_codec_params* p)
{
    if (strcmp(p->codec, "opus") == 0) return opus_codec_open(c, p);
    if (strcmp(p->codec, "vorbis") == 0) return vorbis_codec_open(c, p);
    return FA_ERROR_UNSUPPORTED_FORMAT;
}
