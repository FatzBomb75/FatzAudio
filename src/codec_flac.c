/* The built-in FLAC packet codec: one FLAC frame per packet (Matroska/WebM, MP4,
 * Ogg FLAC and packets pushed by the app), decoded by miniaudio's dr_flac, the
 * decoder .flac files use, so a track decodes the same in every container.
 *
 * dr_flac reads whole streams, so each frame is handed to it behind a "fLaC"
 * marker and a STREAMINFO block: a frame copy and a decoder set-up per packet,
 * and no second FLAC decoder in the library. */
#include "fa_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLAC_HEAD (4 + 4 + 34)          /* "fLaC", block header, STREAMINFO */
#define FLAC_SI 8                       /* STREAMINFO's offset in the head */
#define FLAC_MAX_FRAME (16u << 20)

typedef struct flac_state {
    uint8_t* buf;                       /* FLAC_HEAD, then the frame */
    size_t cap;
    uint32_t max_block;
    fa_drflac_cache cache;
} flac_state;

void fa_flac_streaminfo_format(const uint8_t* si, uint32_t* rate, uint32_t* channels, uint32_t* bits)
{
    *rate = ((uint32_t)si[10] << 12) | ((uint32_t)si[11] << 4) | ((uint32_t)si[12] >> 4);
    *channels = ((si[12] >> 1) & 7) + 1u;
    *bits = ((((uint32_t)si[12] & 1) << 4) | ((uint32_t)si[13] >> 4)) + 1;
}

static void set_format(uint8_t* si, uint32_t rate, uint32_t channels, uint32_t bits)
{
    si[10] = (uint8_t)(rate >> 12);
    si[11] = (uint8_t)(rate >> 4);
    si[12] = (uint8_t)(((rate & 15) << 4) | ((channels - 1) << 1) | ((bits - 1) >> 4));
    si[13] = (uint8_t)((((bits - 1) & 15) << 4) | (si[13] & 15));
}

/* The frame header's own channel count, bit depth and rate (where it gives
 * them) go into STREAMINFO: dr_flac rejects a frame whose channel count
 * differs from it. */
static void follow_frame(uint8_t* si, const uint8_t* d, size_t n)
{
    static const uint32_t rates[16] = { 0, 88200, 176400, 192000, 8000, 16000, 22050, 24000,
                                        32000, 44100, 48000, 96000, 0, 0, 0, 0 };
    static const uint32_t depths[8] = { 0, 8, 12, 0, 16, 20, 24, 32 };
    uint32_t rate, channels, bits, ca;
    if (n < 4) return;
    fa_flac_streaminfo_format(si, &rate, &channels, &bits);
    ca = d[3] >> 4;
    if (ca <= 7) channels = ca + 1;
    else if (ca <= 10) channels = 2;
    if (depths[(d[3] >> 1) & 7]) bits = depths[(d[3] >> 1) & 7];
    if (rates[d[2] & 15]) rate = rates[d[2] & 15];
    set_format(si, rate, channels, bits);
}

static fa_result flac_pkt_decode(fa_codec* c, const uint8_t* data, size_t size)
{
    flac_state* st = (flac_state*)c->impl;
    fa_result r;
    int64_t n;
    uint32_t channels = 0, rate = 0, bits;
    c->out_frames = 0;
    if (!data || size == 0) return FA_OK;           /* nothing held back */
    if (size < 4 || data[0] != 0xFF || (data[1] & 0xFE) != 0xF8) return FA_ERROR_UNSUPPORTED_FORMAT;
    if (size > FLAC_MAX_FRAME) return FA_ERROR_UNSUPPORTED_FORMAT;
    if (FLAC_HEAD + size > st->cap) {
        uint8_t* p = (uint8_t*)realloc(st->buf, FLAC_HEAD + size);
        if (!p) return FA_ERROR_OUT_OF_MEMORY;
        st->buf = p;
        st->cap = FLAC_HEAD + size;
    }
    follow_frame(st->buf + FLAC_SI, data, size);
    memcpy(st->buf + FLAC_HEAD, data, size);
    /* Room for STREAMINFO's channels (the frame's own, just copied in). */
    fa_flac_streaminfo_format(st->buf + FLAC_SI, &rate, &channels, &bits);
    r = fa_codec_reserve(c, st->max_block, channels);
    if (r != FA_OK) return r;
    channels = rate = 0;
    n = fa_drflac_decode_memory(st->buf, FLAC_HEAD + size, c->out, st->max_block, &channels, &rate, &st->cache);
    if (n < 0 || channels == 0) return FA_ERROR_UNSUPPORTED_FORMAT;
    c->channels = channels;
    if (rate) c->rate = rate;
    c->out_frames = (fa_frames)n;
    return FA_OK;
}

static void flac_pkt_flush(fa_codec* c)
{
    (void)c;                                        /* every frame stands alone */
}

static void flac_pkt_close(fa_codec* c)
{
    flac_state* st = (flac_state*)c->impl;
    if (st) {
        free(st->buf);
        free(st->cache.block);
        free(st);
    }
    c->impl = NULL;
}

static const fa_codec_vtable flac_vtable = { "flac", flac_pkt_decode, flac_pkt_flush, flac_pkt_close };

/* The 34-byte STREAMINFO inside the extradata forms in use: STREAMINFO alone
 * (MP4 dfLa, FFmpeg), "fLaC" and metadata blocks (Matroska), or one metadata
 * block with its header. NULL if there is none. */
static const uint8_t* find_streaminfo(const uint8_t* p, size_t n)
{
    if (!p) return NULL;
    if (n >= 4 && memcmp(p, "fLaC", 4) == 0) { p += 4; n -= 4; }
    if (n >= 38 && (p[0] & 0x7F) == 0 && p[1] == 0 && p[2] == 0 && p[3] == 34) return p + 4;
    if (n == 34) return p;
    return NULL;
}

fa_result fa_flac_codec_open(fa_codec* c, const fa_codec_params* p)
{
    flac_state* st;
    const uint8_t* found = find_streaminfo((const uint8_t*)p->extradata, p->extradata_size);
    uint8_t *h, *si;
    uint32_t rate, channels, bits;
    if (found) {
        fa_flac_streaminfo_format(found, &rate, &channels, &bits);
    } else {
        /* No STREAMINFO (raw packets pushed without extradata): made up from the
         * parameters, then from each frame's own header. */
        rate = p->sample_rate;
        channels = p->channels;
        bits = p->bits_per_coded_sample ? p->bits_per_coded_sample : 16;
    }
    if (rate == 0 || rate > 0xFFFFF || channels == 0 || channels > 8 || bits < 4 || bits > 32)
        return FA_ERROR_UNSUPPORTED_FORMAT;
    st = (flac_state*)calloc(1, sizeof(*st));
    if (!st) return FA_ERROR_OUT_OF_MEMORY;
    st->buf = (uint8_t*)malloc(FLAC_HEAD + 4096);
    if (!st->buf) { free(st); return FA_ERROR_OUT_OF_MEMORY; }
    st->cap = FLAC_HEAD + 4096;
    h = st->buf;
    si = h + FLAC_SI;
    memcpy(h, "fLaC", 4);
    h[4] = 0x80;                                    /* the last metadata block: STREAMINFO */
    h[5] = 0; h[6] = 0; h[7] = 34;
    if (found) memcpy(si, found, 34);
    else memset(si, 0, 34);
    set_format(si, rate, channels, bits);
    /* Block sizes as broad as FLAC allows unless STREAMINFO gives usable ones
     * (broken muxers write 0): dr_flac rejects frames above the maximum. */
    st->max_block = ((uint32_t)si[2] << 8) | si[3];
    if (st->max_block < 16 || (((uint32_t)si[0] << 8) | si[1]) < 16) {
        st->max_block = 65535;
        si[0] = 0; si[1] = 16;
        si[2] = 0xFF; si[3] = 0xFF;
    }
    /* Total samples unknown: dr_flac must not stop at a count. */
    si[13] &= 0xF0;
    memset(si + 14, 0, 4);
    c->vt = &flac_vtable;
    c->impl = st;
    c->rate = rate;
    c->channels = channels;
    snprintf(c->name, sizeof(c->name), "flac");
    return FA_OK;
}
