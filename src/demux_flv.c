/* Flash Video files (.flv), the audio only. Written from Adobe's "Video File Format
 * Specification, Version 10.1" (the FLV header, tags and AUDIODATA).
 *
 * A file is a 9-byte header ("FLV", version, flags, header size), then tags, each
 * preceded by the size of the one before it: an 11-byte tag header (type, data size,
 * timestamp in ms, stream id) and its data. An audio tag (type 8) starts with a byte
 * saying the format, rate, sample size and channels; AAC adds a byte telling a
 * configuration (the AudioSpecificConfig, once at the start) from a raw frame.
 *
 * Supported: AAC, MP3 (also the 8 kHz form), PCM (8 and 16 bit), µ-law and A-law.
 * Nellymoser, Speex, ADPCM and the like are reported as unsupported. Every tag header
 * of a seekable file is read once at open to list the audio tags (a hop over each video
 * tag: about 0.1 s for an hour of 25 frames a second); a forward-only source plays as
 * the tags come. Tag timestamps are milliseconds; the codec's own frame grid fixes
 * the rounding. */
#include "demux.h"
#include "pkt_index.h"
#include <stdlib.h>
#include <string.h>

#define FLV_MAX_TAG (32u << 20)
#define FLV_MAX_PACKETS (1u << 25)

typedef struct flv {
    fa_br br;
    fa_track track;
    uint8_t* data;
    size_t data_cap;
    fa_pkt_index pk;
    int indexed;
    size_t cur;
    int format;                 /* SoundFormat of the stream's tags */
    int64_t pos;                /* forward only: the next tag's previous-size field */
    int64_t audio_bytes;
} flv;

static void flv_free(flv* f)
{
    fa_br_free(&f->br);
    free(f->data);
    fa_pkt_free(&f->pk);
    free(f);
}

/* One audio tag, read at `pos` (its previous-size field). Returns 1 with the fields, 0 at the
 * end of the data or damage; *kind is 0 for a tag of another type (skipped). */
typedef struct flv_tag {
    int type;
    uint32_t size;      /* the data */
    int64_t ts;
    int64_t data_at;
    int64_t next;
    uint8_t first[2];   /* the audio tag's first two data bytes */
    int have;           /* how many of them */
} flv_tag;

static int read_tag(flv* f, int64_t pos, flv_tag* t)
{
    uint8_t h[15];
    int64_t length = f->br.src->length;
    if (length > 0 && pos + 15 > length) return 0;
    if (fa_br_seek(&f->br, pos) != FA_OK || fa_br_read(&f->br, h, 15) != 15) return 0;
    t->type = h[4] & 0x1F;
    if (h[4] & 0x20) t->type |= 0x100;     /* encrypted */
    t->size = ((uint32_t)h[5] << 16) | ((uint32_t)h[6] << 8) | h[7];
    t->ts = ((int64_t)h[8] << 16) | ((int64_t)h[9] << 8) | h[10] | ((int64_t)h[11] << 24);
    t->data_at = pos + 15;
    t->next = t->data_at + t->size;
    t->have = 0;
    if ((t->type & 0xFF) == 8 && t->size > 0) {
        size_t want = t->size > 1 ? 2 : 1;
        t->have = (int)fa_br_read(&f->br, t->first, want);
    }
    if (length > 0 && t->next > length) return 0;      /* a cut tag */
    return 1;
}

/* ---- packets ----------------------------------------------------------------------- */

static fa_result flv_next(void* ctx, fa_demux_packet* out)
{
    flv* f = (flv*)ctx;
    fa_result r;
    if (f->indexed) {
        fa_pkt p;
        if (f->cur >= f->pk.n) return FA_ERROR_END_OF_STREAM;
        fa_pkt_get(&f->pk, f->cur, &p);
        r = fa_br_seek(&f->br, p.off);
        if (r != FA_OK) return FA_ERROR_END_OF_STREAM;
        r = fa_br_read_alloc(&f->br, p.size, FLV_MAX_TAG, &f->data, &f->data_cap);
        if (r != FA_OK) return r == FA_ERROR_OUT_OF_MEMORY ? r : FA_ERROR_END_OF_STREAM;
        out->data = f->data;
        out->size = p.size;
        out->pts = p.pts;
        out->duration = p.dur;
        f->cur++;
        return FA_OK;
    }
    for (;;) {
        flv_tag t;
        size_t skip;
        if (!read_tag(f, f->pos, &t)) return FA_ERROR_END_OF_STREAM;
        f->pos = t.next;
        if (t.type != 8 || t.have < 1 || (t.first[0] >> 4) != f->format) continue;
        skip = f->format == 10 ? 2 : 1;
        if (f->format == 10 && (t.have < 2 || t.first[1] != 1)) continue;     /* a configuration or the end mark */
        if (t.size <= skip || t.size > FLV_MAX_TAG) continue;
        r = fa_br_seek(&f->br, t.data_at + (int64_t)skip);
        if (r != FA_OK) return FA_ERROR_END_OF_STREAM;
        r = fa_br_read_alloc(&f->br, t.size - skip, FLV_MAX_TAG, &f->data, &f->data_cap);
        if (r != FA_OK) return r == FA_ERROR_OUT_OF_MEMORY ? r : FA_ERROR_END_OF_STREAM;
        out->data = f->data;
        out->size = t.size - skip;
        out->pts = t.ts;
        out->duration = -1;
        return FA_OK;
    }
}

static fa_result flv_seek_packets(void* ctx, int64_t t, int packets_back, size_t bytes_back)
{
    flv* f = (flv*)ctx;
    if (!f->indexed) return FA_ERROR_NOT_SEEKABLE;
    f->cur = fa_pkt_seek_back(&f->pk, t, packets_back, bytes_back);
    return FA_OK;
}

static const fa_demux_ops flv_ops = { flv_next, flv_seek_packets };

/* ---- open ------------------------------------------------------------------------------ */

static const uint32_t flv_rates[4] = { 5512, 11025, 22050, 44100 };

static fa_result flv_open(fa_decoder* dec)
{
    fa_source* src = dec->src;
    flv* f = (flv*)calloc(1, sizeof(flv));
    uint8_t head[9], asc[64];
    size_t asc_len = 0;
    int64_t pos;
    int found = 0, have_asc = 0, header_byte = 0;
    int64_t first_pos = 0;
    fa_track_params tp;
    const char* name = NULL;
    uint32_t rate = 0, channels = 0, bits = 0;
    fa_result r;

    if (!f) return FA_ERROR_OUT_OF_MEMORY;
    r = fa_br_init(&f->br, src);
    if (r != FA_OK) { free(f); return r; }
    if (fa_br_read(&f->br, head, 9) != 9 || memcmp(head, "FLV", 3) != 0 || head[3] != 1 || !(head[4] & 4)) {
        r = FA_ERROR_UNSUPPORTED_FORMAT;
        goto fail;
    }
    pos = (int64_t)fa_be32(head + 5);
    if (pos < 9 || pos > (1 << 20)) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto fail; }

    /* The first audio tag (and for AAC its configuration) names the codec. */
    for (;;) {
        flv_tag t;
        int fmt;
        if (!read_tag(f, pos, &t)) break;
        if (t.type & 0x100 && (t.type & 0xFF) == 8) { r = FA_ERROR_PROTECTED_CONTENT; goto fail; }
        if (t.type == 8 && t.have >= 1) {
            fmt = t.first[0] >> 4;
            if (!found) {
                f->format = fmt;
                found = 1;
                first_pos = pos;
                channels = (t.first[0] & 1) ? 2 : 1;
                rate = flv_rates[(t.first[0] >> 2) & 3];
                bits = (t.first[0] & 2) ? 16 : 8;
                if (fmt == 14) rate = 8000;
            }
            if (fmt == 10 && f->format == 10 && t.have >= 2 && t.first[1] == 0 && !have_asc && t.size > 2 && t.size - 2 <= sizeof(asc)) {
                if (fa_br_seek(&f->br, t.data_at + 2) == FA_OK && fa_br_read(&f->br, asc, t.size - 2) == t.size - 2) {
                    asc_len = t.size - 2;
                    have_asc = 1;
                    first_pos = t.next;     /* the frames follow the configuration */
                    break;
                }
            }
            if (fmt != 10) break;
        }
        pos = t.next;
        if (pos > (int64_t)1 << 40) break;
    }
    if (!found) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto fail; }
    dec->track_count = 1;
    if (dec->audio_track > 0) { r = FA_ERROR_NOT_FOUND; goto fail; }

    switch (f->format) {
    case 2: case 14: name = "mp3"; header_byte = 1; break;
    case 10:
        name = "aac";
        header_byte = 2;
        if (!have_asc) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto fail; }
        break;
    case 0: name = bits == 16 ? "pcm_s16be" : "pcm_u8"; header_byte = 1; break;
    case 3: name = bits == 16 ? "pcm_s16le" : "pcm_u8"; header_byte = 1; break;
    case 7: name = "pcm_alaw"; bits = 8; header_byte = 1; break;
    case 8: name = "pcm_mulaw"; bits = 8; header_byte = 1; break;
    default: break;
    }
    if (!name) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto fail; }

    memset(&tp, 0, sizeof(tp));
    fa_codec_params_init(&tp.codec);
    tp.codec.codec = name;
    if (have_asc && asc_len >= 2) {
        /* audioObjectType (5 bits), samplingFrequencyIndex (4), channelConfiguration (4). */
        unsigned aot = asc[0] >> 3, idx = ((unsigned)(asc[0] & 7) << 1) | (asc[1] >> 7), cfg = (asc[1] >> 3) & 15;
        if (aot != 31 && idx < 13) rate = fa_aac_rates[idx];
        if (aot != 31 && cfg >= 1 && cfg <= 7) channels = cfg == 7 ? 8 : cfg;
        tp.codec.extradata = asc;
        tp.codec.extradata_size = asc_len;
    }
    tp.codec.sample_rate = rate;
    tp.codec.channels = channels;
    if (strncmp(name, "pcm_", 4) == 0) {
        tp.codec.block_align = channels * (bits / 8);
        tp.codec.bits_per_coded_sample = bits;
    }
    tp.time_base = 1e-3;
    tp.start = 0;
    tp.end = FA_DEMUX_NO_PTS;
    tp.last_pts = FA_DEMUX_NO_PTS;
    tp.precision = 1.0;

    f->indexed = src->seekable;
    if (f->indexed) {
        /* Every audio tag of the stream: offset and size of its data past the audio header, and
         * its time. A packet lasts until the next one starts; the last one as long as the one
         * before it. */
        int have_prev = 0;
        int64_t p_off = 0, p_ts = 0, last_dur = 0, at = first_pos;
        uint32_t p_size = 0;
        for (;;) {
            flv_tag t;
            int is_audio;
            if (!read_tag(f, at, &t)) break;
            at = t.next;
            is_audio = t.type == 8 && t.have >= 1 && (t.first[0] >> 4) == f->format && t.size > (uint32_t)header_byte &&
                       (f->format != 10 || (t.have >= 2 && t.first[1] == 1)) && t.size <= FLV_MAX_TAG;
            if (!is_audio) continue;
            if (have_prev) {
                int64_t d = t.ts - p_ts;
                if (d < 0) { d = 0; t.ts = p_ts; }
                last_dur = d;
                r = fa_pkt_add(&f->pk, p_off, p_size, p_ts, d > 0xFFFFFFFFLL ? 0xFFFFFFFFu : (uint32_t)d);
                if (r != FA_OK) goto fail;
            }
            p_off = t.data_at + header_byte;
            p_size = t.size - (uint32_t)header_byte;
            p_ts = t.ts;
            have_prev = 1;
            f->audio_bytes += p_size;
            if (f->pk.n >= FLV_MAX_PACKETS) break;
        }
        if (have_prev) {
            r = fa_pkt_add(&f->pk, p_off, p_size, p_ts, (uint32_t)last_dur);
            if (r != FA_OK) goto fail;
            tp.last_pts = p_ts;
            tp.last_end = p_ts + last_dur;
            tp.last_exact = 0;
            /* PCM: the last packet's own samples are exact, where its time is only in ms. The
             * other codecs have frames of one size, which the track takes from its grid. */
            if (strncmp(name, "pcm_", 4) == 0 && fa_br_seek(&f->br, p_off) == FA_OK &&
                fa_br_read_alloc(&f->br, p_size, FLV_MAX_TAG, &f->data, &f->data_cap) == FA_OK) {
                uint32_t out_rate = 0;
                int64_t n = fa_demux_packet_samples(name, channels, rate, f->data, p_size, &out_rate);
                if (n > 0 && out_rate > 0) {
                    tp.last_end = p_ts + (int64_t)((double)n * 1000.0 / out_rate + 0.5);
                    tp.last_exact = 1;
                }
            }
            tp.end = tp.last_end;
        }
        fa_pkt_trim(&f->pk);
        if (f->pk.n == 0) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto fail; }
    } else {
        f->pos = first_pos;
    }

    r = fa_track_open(&f->track, dec, &flv_ops, f, &tp);
    if (r != FA_OK) goto fail;
    dec->impl = f;
    dec->seekable = f->indexed;
    dec->audio_bytes = f->indexed ? f->audio_bytes : -1;
    snprintf(dec->container, sizeof(dec->container), "flv");
    return FA_OK;
fail:
    flv_free(f);
    return r;
}

static fa_result flv_read(fa_decoder* dec, float* out, fa_frames frames, fa_frames* got)
{
    return fa_track_read(&((flv*)dec->impl)->track, out, frames, got);
}

static fa_result flv_seek(fa_decoder* dec, fa_frames frame)
{
    if (!dec->seekable) return FA_ERROR_NOT_SEEKABLE;
    return fa_track_seek(&((flv*)dec->impl)->track, frame);
}

static void flv_close(fa_decoder* dec)
{
    flv* f = (flv*)dec->impl;
    if (!f) return;
    fa_track_close(&f->track);
    flv_free(f);
    dec->impl = NULL;
}

/* "FLV", version 1, flags with the audio bit. */
static int flv_probe(const uint8_t* d, size_t n, size_t id3)
{
    (void)id3;
    return n >= 9 && memcmp(d, "FLV", 3) == 0 && d[3] == 1 && (d[4] & 4) ? 100 : 0;
}

const fa_decoder_vtable fa_decoder_flv = {
    "flv", "Flash Video (audio track: AAC, MP3, PCM)", "flv", "video/x-flv",
    flv_probe, flv_open, flv_read, flv_seek, flv_close, 1, NULL, NULL
};
