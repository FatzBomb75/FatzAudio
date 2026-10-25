/* Core Audio Format (.caf) files, written from Apple's Core Audio Format
 * Specification 1.0 (file header, chunks, 'desc', 'kuki', 'pakt', 'data').
 *
 * A file is a header followed by chunks of a 4-byte type and a signed 64-bit size,
 * everything big-endian. 'desc' says what the audio is, 'kuki' holds the codec's
 * magic cookie, 'data' the audio (its size is -1 when it runs to the end of the
 * file) and 'pakt' the packet table: the sizes of the packets, and for codecs with
 * packets of varying length their durations, as variable-length quantities. It
 * also gives the priming and remainder frames, which are cut from the start and
 * the end. Chunks may come in any order, so a seekable source is read through to
 * its last chunk; a forward-only one is read up to the 'data' chunk.
 *
 * Supported: ALAC (the cookie is ALAC's, as in MP4), AAC (LC, HE-AAC and HE-AAC v2;
 * the cookie is an MPEG-4 ES descriptor holding the AudioSpecificConfig, as in an
 * MP4 esds box), linear PCM (integer and float, either byte order) and MP3.
 * Everything else (AAC-LD and ELD, µ-law and A-law, other codecs) is reported as
 * unsupported. The channel layout chunk is not used: ALAC
 * says its layout in its cookie, the other formats are mono and stereo or in
 * WAVE's order. Packets of constant size (PCM) are found by arithmetic; the others
 * from a table built from 'pakt'. */
#include "demux.h"
#include <stdlib.h>
#include <string.h>

#define FOURCC(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#define CAF_MAX_KUKI (1u << 20)
#define CAF_MAX_PAKT (1u << 28)
#define CAF_MAX_PACKETS (1 << 25)
#define CAF_MAX_PACKET (64u << 20)
#define CAF_PCM_FRAMES 4096     /* frames per packet when PCM is read in groups */

typedef struct caf_pkt {
    int64_t off;
    int64_t pts;
    uint32_t size;
    uint32_t dur;
} caf_pkt;

typedef struct caf {
    fa_br br;
    fa_track track;
    uint8_t* kuki;
    size_t kuki_len;
    uint8_t asc[FA_AAC_ASC_MAX];  /* AAC with no config in a cookie: built from 'desc' */
    uint8_t* data;
    size_t data_cap;
    caf_pkt* pk;              /* packets from 'pakt' (NULL: constant size) */
    size_t npk, cur;
    uint32_t frame_bytes;     /* constant size: bytes per frame */
    int64_t data_off, data_len;   /* the audio; data_len -1 = to the end of the source */
} caf;

static void caf_free(caf* c)
{
    fa_br_free(&c->br);
    free(c->kuki);
    free(c->data);
    free(c->pk);
    free(c);
}

/* A variable-length quantity: 7 bits per byte, the high bit set while more follow. */
static int read_vlq(const uint8_t* p, size_t n, size_t* pos, uint64_t* out)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < 9 && *pos < n; i++) {
        uint8_t b = p[(*pos)++];
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80)) { *out = v; return 1; }
    }
    return 0;
}

typedef struct caf_desc {
    double rate;
    uint32_t format, flags, bytes_per_packet, frames_per_packet, channels, bits;
} caf_desc;

/* The ES_Descriptor in an AAC cookie. Apple's tools write it bare (it is what
 * AudioToolbox takes as the cookie); some writers keep the esds box's version and
 * flags in front of it, or the whole box. */
static int aac_cookie(const uint8_t* k, size_t n, uint8_t* oti, const uint8_t** asc, size_t* asc_len)
{
    size_t at = 0;
    *asc = NULL;
    *asc_len = 0;
    if (!k || n < 2) return 0;
    if (k[0] != 0x03) {
        if (n > 12 && memcmp(k + 4, "esds", 4) == 0 && k[12] == 0x03) at = 12;
        else if (n > 4 && k[4] == 0x03) at = 4;
        else return 0;
    }
    return fa_mp4_es_descriptor(k + at, n - at, oti, asc, asc_len);
}

/* The codec for the description. `constant` is set for constant-size formats. */
static fa_result codec_for(caf* c, const caf_desc* d, fa_codec_params* cp, int* constant)
{
    const char* name = NULL;
    const uint8_t* asc = NULL;
    size_t asc_len = 0;
    uint32_t rate;
    if (!(d->rate >= 1 && d->rate <= 768000) || d->channels == 0 || d->channels > 32) return FA_ERROR_UNSUPPORTED_FORMAT;
    rate = (uint32_t)(d->rate + 0.5);
    *constant = 0;
    switch (d->format) {
    case FOURCC('l', 'p', 'c', 'm'): {
        int fl = (d->flags & 1) != 0, le = (d->flags & 2) != 0;
        uint32_t bits = d->bits;
        if (bits == 0 || bits % 8 || d->bytes_per_packet != d->channels * (bits / 8) || d->frames_per_packet != 1)
            return FA_ERROR_UNSUPPORTED_FORMAT;
        if (fl) name = bits == 64 ? (le ? "pcm_f64le" : "pcm_f64be") : bits == 32 ? (le ? "pcm_f32le" : "pcm_f32be") : NULL;
        else name = bits == 8 ? "pcm_s8" : bits == 16 ? (le ? "pcm_s16le" : "pcm_s16be") : bits == 24 ? (le ? "pcm_s24le" : "pcm_s24be")
                  : bits == 32 ? (le ? "pcm_s32le" : "pcm_s32be") : NULL;
        *constant = 1;
        break;
    }
    case FOURCC('a', 'l', 'a', 'c'):
        if (!c->kuki) return FA_ERROR_UNSUPPORTED_FORMAT;
        name = "alac";
        break;
    case FOURCC('.', 'm', 'p', '3'):
        name = "mp3";
        break;
    case FOURCC('a', 'a', 'c', ' '):     /* AAC-LC (also Main and LTP) */
    case FOURCC('a', 'a', 'c', 'h'):     /* HE-AAC: the rate and frames per packet are after SBR */
    case FOURCC('a', 'a', 'c', 'p'): {   /* HE-AAC v2 */
        uint8_t oti = 0x40;     /* no cookie: MPEG-4 AAC, the config from 'desc' */
        if (c->kuki && (!aac_cookie(c->kuki, c->kuki_len, &oti, &asc, &asc_len) || !(oti == 0x40 || (oti >= 0x66 && oti <= 0x68))))
            return FA_ERROR_UNSUPPORTED_FORMAT;
        if (!asc || asc_len == 0) {
            /* MPEG-2 AAC says its profile in the object type: Main, LC, SSR. */
            int aot = oti == 0x66 ? 1 : oti == 0x68 ? 3 : 2;
            if (d->format != FOURCC('a', 'a', 'c', ' ')) return FA_ERROR_UNSUPPORTED_FORMAT;
            asc_len = fa_aac_build_asc(c->asc, aot, rate, d->channels, 0);
            asc = c->asc;
        }
        name = "aac";
        break;
    }
    default:
        break;
    }
    if (!name) return FA_ERROR_UNSUPPORTED_FORMAT;
    cp->codec = name;
    cp->sample_rate = rate;
    cp->channels = d->channels;
    if (d->format == FOURCC('a', 'l', 'a', 'c')) {
        cp->extradata = c->kuki;
        cp->extradata_size = c->kuki_len;
    } else if (asc) {
        cp->extradata = asc;
        cp->extradata_size = asc_len;
    }
    if (*constant) cp->block_align = d->bytes_per_packet;
    return FA_OK;
}

static fa_result caf_next(void* ctx, fa_demux_packet* out)
{
    caf* c = (caf*)ctx;
    fa_result r;
    if (c->pk) {
        const caf_pkt* p;
        if (c->cur >= c->npk) return FA_ERROR_END_OF_STREAM;
        p = &c->pk[c->cur];
        r = fa_br_seek(&c->br, p->off);
        if (r == FA_ERROR_NOT_SEEKABLE) return r;
        if (r != FA_OK) return FA_ERROR_END_OF_STREAM;
        r = fa_br_read_alloc(&c->br, p->size, CAF_MAX_PACKET, &c->data, &c->data_cap);
        if (r != FA_OK) return r == FA_ERROR_OUT_OF_MEMORY ? r : FA_ERROR_END_OF_STREAM;
        out->data = c->data;
        out->size = p->size;
        out->pts = p->pts;
        out->duration = p->dur;
        c->cur++;
        return FA_OK;
    } else {
        int64_t start = (int64_t)c->cur * CAF_PCM_FRAMES * c->frame_bytes, want = (int64_t)CAF_PCM_FRAMES * c->frame_bytes, got;
        if (c->data_len >= 0) {
            if (start >= c->data_len) return FA_ERROR_END_OF_STREAM;
            if (want > c->data_len - start) want = c->data_len - start;
        }
        r = fa_br_seek(&c->br, c->data_off + start);
        if (r == FA_ERROR_NOT_SEEKABLE) return r;
        if (r != FA_OK) return FA_ERROR_END_OF_STREAM;
        got = (int64_t)fa_br_read(&c->br, c->data, (size_t)want);
        got -= got % c->frame_bytes;    /* a cut file keeps its whole frames */
        if (got <= 0) return FA_ERROR_END_OF_STREAM;
        out->data = c->data;
        out->size = (size_t)got;
        out->pts = (int64_t)c->cur * CAF_PCM_FRAMES;
        out->duration = got / c->frame_bytes;
        c->cur++;
        return FA_OK;
    }
}

static fa_result caf_seek_packets(void* ctx, int64_t t, int packets_back, size_t bytes_back)
{
    caf* c = (caf*)ctx;
    if (c->pk) {
        size_t lo = 0, hi = c->npk, j;
        int n = 0;
        size_t bytes = 0;
        if (c->npk == 0) { c->cur = 0; return FA_OK; }
        while (hi - lo > 1) {
            size_t mid = lo + (hi - lo) / 2;
            if (c->pk[mid].pts <= t) lo = mid;
            else hi = mid;
        }
        j = lo;
        while (j > 0 && n < 64 && (n < packets_back || bytes < bytes_back)) {
            j--;
            n++;
            bytes += c->pk[j].size;
        }
        c->cur = j;
    } else {
        int64_t j = t / CAF_PCM_FRAMES - packets_back;
        c->cur = j > 0 ? (size_t)j : 0;
    }
    return FA_OK;
}

static const fa_demux_ops caf_ops = { caf_next, caf_seek_packets };

/* Builds the packet list from a 'pakt' chunk. Sets the total frames in *total. */
static fa_result build_table(caf* c, const caf_desc* d, const uint8_t* pakt, size_t pakt_len, int64_t* total)
{
    int64_t n = (int64_t)fa_be64(pakt);
    size_t pos = 24, i, cap;
    int64_t off = c->data_off, pts = 0;
    /* Never more entries than the table can hold: one VLQ byte or more for each of the
     * size and the duration that are not constant. */
    size_t per = (size_t)(d->bytes_per_packet == 0) + (size_t)(d->frames_per_packet == 0);
    size_t most = per ? (pakt_len - 24) / per : pakt_len;
    if (n < 0 || (uint64_t)n > most) n = (int64_t)most;
    if (n > CAF_MAX_PACKETS) n = CAF_MAX_PACKETS;
    cap = (size_t)n;
    c->pk = (caf_pkt*)malloc((cap ? cap : 1) * sizeof(caf_pkt));
    if (!c->pk) return FA_ERROR_OUT_OF_MEMORY;
    for (i = 0; i < cap; i++) {
        uint64_t size = d->bytes_per_packet, dur = d->frames_per_packet;
        if (size == 0 && !read_vlq(pakt, pakt_len, &pos, &size)) break;
        if (dur == 0 && !read_vlq(pakt, pakt_len, &pos, &dur)) break;
        if (size > CAF_MAX_PACKET || dur > (1u << 24)) return FA_ERROR_UNSUPPORTED_FORMAT;
        if (c->data_len >= 0 && off + (int64_t)size > c->data_off + c->data_len) break;   /* a cut file */
        c->pk[i].off = off;
        c->pk[i].pts = pts;
        c->pk[i].size = (uint32_t)size;
        c->pk[i].dur = (uint32_t)dur;
        off += (int64_t)size;
        pts += (int64_t)dur;
    }
    c->npk = i;
    *total = pts;
    return FA_OK;
}

static fa_result caf_open(fa_decoder* dec)
{
    fa_source* src = dec->src;
    caf* c = (caf*)calloc(1, sizeof(caf));
    uint8_t h[12], desc[32];
    uint8_t* pakt = NULL;
    size_t pakt_len = 0;
    int64_t pos = 8, total = -1, valid = 0, priming = 0, remainder = 0;
    int have_desc = 0, have_data = 0, constant = 0;
    caf_desc d;
    fa_track_params tp;
    fa_result r;

    if (!c) return FA_ERROR_OUT_OF_MEMORY;
    r = fa_br_init(&c->br, src);
    if (r != FA_OK) { free(c); return r; }
    c->data_len = -1;
    memset(&d, 0, sizeof(d));

    for (;;) {
        uint32_t type;
        int64_t size;
        if (fa_br_seek(&c->br, pos) != FA_OK || fa_br_read(&c->br, h, 12) != 12) break;
        type = fa_be32(h);
        size = (int64_t)fa_be64(h + 4);
        if (type == FOURCC('d', 'e', 's', 'c') && size >= 32 && !have_desc) {
            uint64_t bits;
            if (fa_br_read(&c->br, desc, 32) != 32) break;
            bits = fa_be64(desc);
            memcpy(&d.rate, &bits, sizeof(d.rate));
            d.format = fa_be32(desc + 8);
            d.flags = fa_be32(desc + 12);
            d.bytes_per_packet = fa_be32(desc + 16);
            d.frames_per_packet = fa_be32(desc + 20);
            d.channels = fa_be32(desc + 24);
            d.bits = fa_be32(desc + 28);
            have_desc = 1;
        } else if (type == FOURCC('k', 'u', 'k', 'i') && size > 0 && size <= (int64_t)CAF_MAX_KUKI && !c->kuki) {
            c->kuki = (uint8_t*)malloc((size_t)size);
            if (!c->kuki) { r = FA_ERROR_OUT_OF_MEMORY; goto fail; }
            c->kuki_len = fa_br_read(&c->br, c->kuki, (size_t)size);
        } else if (type == FOURCC('p', 'a', 'k', 't') && size >= 24 && size <= (int64_t)CAF_MAX_PAKT && !pakt) {
            int64_t want = size;    /* never more than the source holds: a lying size must not allocate 256 MB */
            size_t cap = 0;
            if (src->length > 0 && want > src->length - pos - 12) want = src->length - pos - 12;
            if (want < 24) want = 0;
            /* In pieces that double, so a source of unknown length allocates only what
             * it really holds. */
            while ((int64_t)pakt_len < want) {
                size_t step, got;
                if (pakt_len == cap) {
                    uint8_t* np;
                    cap = cap ? cap * 2 : 65536;
                    if ((int64_t)cap > want) cap = (size_t)want;
                    np = (uint8_t*)realloc(pakt, cap);
                    if (!np) { r = FA_ERROR_OUT_OF_MEMORY; goto fail; }
                    pakt = np;
                }
                step = cap - pakt_len;
                got = fa_br_read(&c->br, pakt + pakt_len, step);
                pakt_len += got;
                if (got < step) break;
            }
            if (pakt_len < 24) { free(pakt); pakt = NULL; pakt_len = 0; }
        } else if (type == FOURCC('d', 'a', 't', 'a') && !have_data) {
            if (!have_desc) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto fail; }
            have_data = 1;
            c->data_off = pos + 12 + 4;    /* after the edit count */
            if (size < 0) c->data_len = src->length > 0 ? (src->length > c->data_off ? src->length - c->data_off : 0) : -1;
            else c->data_len = size >= 4 ? size - 4 : 0;
            if (c->data_len >= 0 && src->length > 0 && c->data_len > src->length - c->data_off)
                c->data_len = src->length - c->data_off > 0 ? src->length - c->data_off : 0;   /* cut file */
            if (size < 0 || !src->seekable) break;     /* nothing readable after it */
        } else if (size < 0) {
            break;
        }
        if (size > INT64_MAX - pos - 12) break;
        pos += 12 + size;
    }
    if (!have_desc || !have_data) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto fail; }

    memset(&tp, 0, sizeof(tp));
    fa_codec_params_init(&tp.codec);
    r = codec_for(c, &d, &tp.codec, &constant);
    if (r != FA_OK) goto fail;

    if (pakt) {
        valid = (int64_t)fa_be64(pakt + 8);
        if (valid > ((int64_t)1 << 56)) valid = 0;     /* nonsense; also keeps priming + valid from overflowing */
        priming = (int32_t)fa_be32(pakt + 16);
        remainder = (int32_t)fa_be32(pakt + 20);
        if (priming < 0) priming = 0;
        if (remainder < 0) remainder = 0;
    }
    if (constant) {
        c->frame_bytes = d.bytes_per_packet;
        c->data = (uint8_t*)malloc((size_t)CAF_PCM_FRAMES * c->frame_bytes);
        if (!c->data) { r = FA_ERROR_OUT_OF_MEMORY; goto fail; }
        c->data_cap = (size_t)CAF_PCM_FRAMES * c->frame_bytes;
        if (c->data_len >= 0) total = c->data_len / c->frame_bytes;
    } else {
        if (!pakt) { r = src->seekable ? FA_ERROR_UNSUPPORTED_FORMAT : FA_ERROR_NOT_SEEKABLE; goto fail; }
        r = build_table(c, &d, pakt, pakt_len, &total);
        if (r != FA_OK) goto fail;
        if (valid <= 0 && total >= 0) valid = total - priming - remainder;
    }
    if (valid > 0 && total >= 0 && priming + valid > total) valid = total - priming;
    tp.time_base = 1.0 / tp.codec.sample_rate;
    tp.start = priming;
    tp.end = valid > 0 ? priming + valid : (total >= 0 ? total : FA_DEMUX_NO_PTS);
    tp.clip_to_end = tp.end != FA_DEMUX_NO_PTS;
    tp.last_pts = FA_DEMUX_NO_PTS;
    free(pakt);
    pakt = NULL;

    r = fa_track_open(&c->track, dec, &caf_ops, c, &tp);
    if (r != FA_OK) goto fail;
    dec->impl = c;
    dec->seekable = src->seekable;
    dec->audio_bytes = c->data_len;
    snprintf(dec->container, sizeof(dec->container), "caf");
    return FA_OK;
fail:
    free(pakt);
    caf_free(c);
    return r;
}

static fa_result caf_read(fa_decoder* dec, float* out, fa_frames frames, fa_frames* got)
{
    return fa_track_read(&((caf*)dec->impl)->track, out, frames, got);
}

static fa_result caf_seek(fa_decoder* dec, fa_frames frame)
{
    return fa_track_seek(&((caf*)dec->impl)->track, frame);
}

static void caf_close(fa_decoder* dec)
{
    caf* c = (caf*)dec->impl;
    if (!c) return;
    fa_track_close(&c->track);
    caf_free(c);
    dec->impl = NULL;
}

/* "caff", version 1. */
static int caf_probe(const uint8_t* d, size_t n, size_t id3)
{
    (void)id3;
    return n >= 8 && memcmp(d, "caff", 4) == 0 && fa_be16(d + 4) == 1 ? 100 : 0;
}

const fa_decoder_vtable fa_decoder_caf = {
    "caf", "Core Audio Format (ALAC, AAC, PCM, MP3)", "caf", "audio/x-caf;audio/caf",
    caf_probe, caf_open, caf_read, caf_seek, caf_close, 1, NULL, NULL
};
