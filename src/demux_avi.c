/* AVI files (.avi), the audio streams only. Written from Microsoft's "AVI RIFF File
 * Reference" (the AVIMAINHEADER, AVISTREAMHEADER and idx1 layouts), the OpenDML AVI
 * File Format Extensions, version 1.02 (the super index 'indx', the standard index
 * 'ix##', the RIFF 'AVIX' extension for files over 1 GB) and the WAVEFORMATEX
 * description for the audio format.
 *
 * A file is a RIFF list: 'hdrl' (one 'strl' per stream, each with a stream header
 * 'strh' and a format 'strf'), then the 'movi' list holding the data chunks named
 * by the stream number and the kind (the audio of the second stream is "01wb"),
 * then the old index 'idx1'. OpenDML files continue in further RIFF 'AVIX' lists and
 * index each stream with 'indx' in its 'strl'.
 *
 * The packet list of the audio stream comes from, in this order: the OpenDML index,
 * 'idx1' (its offsets count from the 'movi' list or from the file start; both are
 * told apart by looking at the chunk an entry points to), or a walk over the 'movi'
 * list. A forward-only source has no list: its chunks are read as they come.
 *
 * Time. The stream header gives a time unit (dwScale / dwRate seconds). With a
 * sample size, each unit is that many bytes of the stream's data (so a chunk may
 * hold part of a frame: the decoder's packet parser joins them). Without one, each
 * chunk is one unit (variable bit rate MP3: 1152 samples). PCM, whose unit is its
 * own frame, takes the rate from its format instead of trusting the header.
 *
 * Supported audio: PCM (8 to 32 bit, float), µ-law, A-law, MP3, MP2, AC-3, AAC (raw
 * with an AudioSpecificConfig, or ADTS) and, where the system has a decoder, Windows
 * Media Audio. DTS and others are named but reported as unsupported; ADPCM is
 * unsupported. */
#include "demux.h"
#include "pkt_index.h"
#include <stdlib.h>
#include <string.h>

#define AVI_MAX_HDRL (16u << 20)
#define AVI_MAX_PACKET (32u << 20)
#define AVI_MAX_AUDIO 16        /* audio streams listed */
#define AVI_MAX_STREAMS 100     /* the stream number is two digits */
#define AVI_MAX_IX (1u << 16)   /* entries of a super index */
#define AVI_MAX_PACKETS (1u << 25)
#define AVI_MAX_EXTRA 4096

typedef struct avi_audio {
    int number;                 /* among all streams: the digits in the chunk ids */
    uint32_t tag, channels, rate, avg_bytes, block_align, bits;
    uint8_t* extra;
    size_t extra_len;
    uint32_t scale, srate, start, length, sample_size;   /* the stream header */
    uint64_t* ix_off;           /* OpenDML: where the standard indexes are (file offsets of their chunks) */
    size_t n_ix;
} avi_audio;

typedef struct avi {
    fa_br br;
    fa_track track;
    uint8_t* data;
    size_t data_cap;
    fa_pkt_index pk;
    int indexed;
    size_t cur;
    uint8_t asc[FA_AAC_ASC_MAX];
    int number;                 /* the chosen stream */
    uint32_t sample_size;       /* bytes per time unit, 0 = one unit per chunk */
    int64_t cum;                /* bytes of the stream so far (indexing and streaming) */
    int64_t chunks;             /* chunks so far */
    int64_t pos;                /* streaming: where the next chunk header is */
    int64_t audio_bytes;
    int full;                   /* the list stopped growing (add_chunk) */
} avi;

static void free_audio(avi_audio* a)
{
    free(a->extra);
    free(a->ix_off);
    memset(a, 0, sizeof(*a));
}

static void avi_free(avi* a)
{
    fa_br_free(&a->br);
    free(a->data);
    fa_pkt_free(&a->pk);
    free(a);
}

/* ---- the codec -------------------------------------------------------------------- */

static const char* pcm_for(uint32_t tag, uint32_t bits)
{
    if (tag == 3) return bits == 32 ? "pcm_f32le" : bits == 64 ? "pcm_f64le" : NULL;
    return bits == 8 ? "pcm_u8" : bits == 16 ? "pcm_s16le" : bits == 24 ? "pcm_s24le" : bits == 32 ? "pcm_s32le" : NULL;
}

/* The codec of a WAVEFORMATEX tag, NULL if the AVI demuxer has no name for it. */
static const char* name_for(const avi_audio* s, uint32_t tag)
{
    switch (tag) {
    case 0x0001: case 0x0003: return pcm_for(tag, s->bits);
    case 0x0006: return "pcm_alaw";
    case 0x0007: return "pcm_mulaw";
    case 0x0050: return "mp2";
    case 0x0055: return "mp3";
    case 0x00FF: case 0x706D: case 0x1600: return "aac";
    case 0x2000: return "ac3";
    case 0x2001: return "dts";
    case 0x0160: return "wmav1";
    case 0x0161: return "wmav2";
    case 0x0162: return "wmapro";
    case 0x0163: return "wmalossless";
    case 0x000A: return "wmavoice";
    default: return NULL;
    }
}

/* ---- the header ------------------------------------------------------------------- */

typedef struct avi_header {
    avi_audio audio[AVI_MAX_AUDIO];
    int n_audio;
    int streams;                /* strl lists of any kind */
} avi_header;

static void free_header(avi_header* h)
{
    int i;
    for (i = 0; i < h->n_audio; i++) free_audio(&h->audio[i]);
}

/* A stream list: strh, strf, and the OpenDML index. `base` is the file offset of p. */
static fa_result parse_strl(avi_header* h, const uint8_t* p, size_t n, int64_t base)
{
    const uint8_t *strh = NULL, *strf = NULL, *indx = NULL;
    size_t strh_len = 0, strf_len = 0, indx_len = 0, pos = 0;
    int number = h->streams++;
    avi_audio s;
    while (pos + 8 <= n) {
        uint32_t size = fa_le32(p + pos + 4);
        const uint8_t* body = p + pos + 8;
        if (size > n - pos - 8) size = (uint32_t)(n - pos - 8);
        if (memcmp(p + pos, "strh", 4) == 0 && !strh) { strh = body; strh_len = size; }
        else if (memcmp(p + pos, "strf", 4) == 0 && !strf) { strf = body; strf_len = size; }
        else if (memcmp(p + pos, "indx", 4) == 0 && !indx) { indx = body; indx_len = size; }
        pos += 8 + (size_t)size + (size & 1);
    }
    if (!strh || strh_len < 48 || memcmp(strh, "auds", 4) != 0 || !strf || strf_len < 16) return FA_OK;
    if (h->n_audio >= AVI_MAX_AUDIO || number >= AVI_MAX_STREAMS) return FA_OK;
    memset(&s, 0, sizeof(s));
    s.number = number;
    s.scale = fa_le32(strh + 20);
    s.srate = fa_le32(strh + 24);
    s.start = fa_le32(strh + 28);
    s.length = fa_le32(strh + 32);
    s.sample_size = fa_le32(strh + 44);
    s.tag = fa_le16(strf);
    s.channels = fa_le16(strf + 2);
    s.rate = fa_le32(strf + 4);
    s.avg_bytes = fa_le32(strf + 8);
    s.block_align = fa_le16(strf + 12);
    s.bits = fa_le16(strf + 14);
    if (strf_len >= 18) {
        size_t cb = fa_le16(strf + 16);
        if (cb > strf_len - 18) cb = strf_len - 18;
        if (cb > AVI_MAX_EXTRA) cb = AVI_MAX_EXTRA;
        if (cb > 0) {
            s.extra = (uint8_t*)malloc(cb);
            if (!s.extra) return FA_ERROR_OUT_OF_MEMORY;
            memcpy(s.extra, strf + 18, cb);
            s.extra_len = cb;
        }
    }
    /* indx: wLongsPerEntry, bIndexSubType, bIndexType, nEntriesInUse, dwChunkId, 3 reserved
     * words, then entries. Type 0 is a super index of 16-byte entries (offset of an 'ix##'
     * chunk, its size, duration); type 1 is a standard index right here. */
    if (indx && indx_len >= 24) {
        uint32_t type = indx[3], count = fa_le32(indx + 4), i;
        if (type == 0 && fa_le16(indx) == 4) {
            if (count > (indx_len - 24) / 16) count = (uint32_t)((indx_len - 24) / 16);
            if (count > AVI_MAX_IX) count = AVI_MAX_IX;
            if (count > 0) {
                s.ix_off = (uint64_t*)malloc(count * sizeof(uint64_t));
                if (!s.ix_off) { free_audio(&s); return FA_ERROR_OUT_OF_MEMORY; }
                for (i = 0; i < count; i++) s.ix_off[i] = fa_le64(indx + 24 + (size_t)i * 16);
                s.n_ix = count;
            }
        } else if (type == 1 && fa_le16(indx) == 2) {
            s.ix_off = (uint64_t*)malloc(sizeof(uint64_t));
            if (!s.ix_off) { free_audio(&s); return FA_ERROR_OUT_OF_MEMORY; }
            s.ix_off[0] = (uint64_t)(base + (indx - p) - 8);   /* the indx chunk's header */
            s.n_ix = 1;
        }
    }
    h->audio[h->n_audio++] = s;
    return FA_OK;
}

static fa_result parse_hdrl(avi_header* h, const uint8_t* p, size_t n, int64_t base)
{
    size_t pos = 0;
    while (pos + 8 <= n) {
        uint32_t size = fa_le32(p + pos + 4);
        if (size > n - pos - 8) size = (uint32_t)(n - pos - 8);
        if (memcmp(p + pos, "LIST", 4) == 0 && size >= 4 && memcmp(p + pos + 8, "strl", 4) == 0) {
            fa_result r = parse_strl(h, p + pos + 12, size - 4, base + (int64_t)pos + 12);
            if (r != FA_OK) return r;
        }
        pos += 8 + (size_t)size + (size & 1);
    }
    return FA_OK;
}

/* ---- the packet list ----------------------------------------------------------------- */

/* Adds a chunk of the stream: the time follows from the bytes so far or the chunk count. */
static fa_result add_chunk(avi* a, int64_t off, uint32_t size)
{
    int64_t pts, end;
    if (size == 0) return FA_OK;
    /* A chunk too big for a packet, or too many chunks: what came before plays, the rest is cut. */
    if (size > AVI_MAX_PACKET || a->pk.n >= AVI_MAX_PACKETS) { a->full = 1; return FA_OK; }
    if (a->sample_size) {
        pts = a->cum / a->sample_size;
        end = (a->cum + size) / a->sample_size;
    } else {
        pts = a->chunks;
        end = pts + 1;
    }
    a->cum += size;
    a->chunks++;
    return fa_pkt_add(&a->pk, off, size, pts, (uint32_t)(end - pts));
}

/* One standard index ('ix##' chunk or an indx of that kind) at file offset `at`. */
static fa_result read_std_index(avi* a, int64_t at, const uint8_t id[4])
{
    uint8_t h[32];
    uint32_t count, room, i;
    uint64_t base;
    int64_t length = a->br.src->length;
    if (fa_br_seek(&a->br, at) != FA_OK || fa_br_read(&a->br, h, 32) != 32) return FA_OK;
    /* chunk header (8), wLongsPerEntry, subtype, type, count, chunk id, base offset, reserved */
    if (fa_le16(h + 8) != 2 || h[11] != 1) return FA_OK;
    if (memcmp(h + 16, id, 4) != 0) return FA_OK;   /* another stream's index */
    count = fa_le32(h + 12);
    base = fa_le64(h + 20);
    room = fa_le32(h + 4) > 24 ? (fa_le32(h + 4) - 24) / 8 : 0;   /* entries the chunk can hold */
    if (count > room) count = room;
    for (i = 0; i < count; i++) {
        uint8_t e[8];
        uint64_t off;
        uint32_t size;
        fa_result r;
        if (fa_br_read(&a->br, e, 8) != 8) break;
        off = base + fa_le32(e);
        size = fa_le32(e + 4) & 0x7FFFFFFFu;
        if (off > (uint64_t)INT64_MAX || (length > 0 && (int64_t)off + (int64_t)size > length)) break;   /* cut */
        r = add_chunk(a, (int64_t)off, size);
        if (r != FA_OK || a->full) return r;
    }
    return FA_OK;
}

/* idx1: 16-byte entries (chunk id, flags, offset, size). `movi` is the file offset of the
 * 'movi' fourcc. The base of the offsets is found from the stream's first chunk. */
static fa_result read_idx1(avi* a, int64_t at, uint32_t size, int64_t movi, const uint8_t id[4])
{
    int64_t base = movi, length = a->br.src->length, n = size / 16, i;
    int checked = 0;
    if (fa_br_seek(&a->br, at) != FA_OK) return FA_OK;
    for (i = 0; i < n; i++) {
        uint8_t e[16], c[8];
        int64_t off;
        uint32_t sz;
        fa_result r;
        if (fa_br_read(&a->br, e, 16) != 16) break;
        if (memcmp(e, id, 4) != 0) continue;
        sz = fa_le32(e + 12);
        off = (int64_t)fa_le32(e + 8);
        if (!checked) {
            /* Which base puts a chunk header of this id and size there? */
            int64_t keep = fa_br_tell(&a->br), cand[2], k;
            int found = 0;
            cand[0] = movi + off;
            cand[1] = off;
            for (k = 0; k < 2 && !found; k++) {
                if (fa_br_seek(&a->br, cand[k]) == FA_OK && fa_br_read(&a->br, c, 8) == 8 &&
                    memcmp(c, id, 4) == 0 && fa_le32(c + 4) == sz) {
                    base = k == 0 ? movi : 0;
                    found = 1;
                }
            }
            checked = 1;
            if (!found) return FA_ERROR_UNSUPPORTED_FORMAT;
            if (fa_br_seek(&a->br, keep) != FA_OK) break;
        }
        if (length > 0 && base + off + 8 + (int64_t)sz > length) break;   /* cut */
        r = add_chunk(a, base + off + 8, sz);
        if (r != FA_OK || a->full) return r;
    }
    return FA_OK;
}

/* Walks the data from `pos`: LIST and RIFF lists are entered, other chunks skipped. */
static fa_result walk_data(avi* a, int64_t pos, const uint8_t id[4])
{
    int64_t length = a->br.src->length;
    for (;;) {
        uint8_t h[8];
        uint32_t size;
        if (length > 0 && pos + 8 > length) break;
        if (fa_br_seek(&a->br, pos) != FA_OK || fa_br_read(&a->br, h, 8) != 8) break;
        size = fa_le32(h + 4);
        if (memcmp(h, "LIST", 4) == 0 || memcmp(h, "RIFF", 4) == 0) {
            pos += 12;
            continue;
        }
        if (memcmp(h, id, 4) == 0) {
            fa_result r;
            if (length > 0 && pos + 8 + (int64_t)size > length) break;
            r = add_chunk(a, pos + 8, size);
            if (r != FA_OK || a->full) return r;
        } else if (!(h[0] >= '0' && h[0] <= '9') && memcmp(h, "JUNK", 4) != 0 && memcmp(h, "idx1", 4) != 0 &&
                   memcmp(h, "ix", 2) != 0 && memcmp(h, "PAD ", 4) != 0 && memcmp(h, "indx", 4) != 0) {
            break;   /* not a chunk of an AVI: the data ended */
        }
        pos += 8 + (int64_t)size + (size & 1);
    }
    return FA_OK;
}

/* ---- packets ------------------------------------------------------------------------ */

static fa_result avi_next(void* ctx, fa_demux_packet* out)
{
    avi* a = (avi*)ctx;
    fa_result r;
    if (a->indexed) {
        fa_pkt p;
        if (a->cur >= a->pk.n) return FA_ERROR_END_OF_STREAM;
        fa_pkt_get(&a->pk, a->cur, &p);
        r = fa_br_seek(&a->br, p.off);
        if (r != FA_OK) return FA_ERROR_END_OF_STREAM;
        r = fa_br_read_alloc(&a->br, p.size, AVI_MAX_PACKET, &a->data, &a->data_cap);
        if (r != FA_OK) return r == FA_ERROR_OUT_OF_MEMORY ? r : FA_ERROR_END_OF_STREAM;
        out->data = a->data;
        out->size = p.size;
        out->pts = p.pts;
        out->duration = p.dur;
        a->cur++;
        return FA_OK;
    }
    /* Forward only: the next chunk of the stream. */
    for (;;) {
        uint8_t h[8];
        uint32_t size;
        if (fa_br_seek(&a->br, a->pos) != FA_OK || fa_br_read(&a->br, h, 8) != 8) return FA_ERROR_END_OF_STREAM;
        size = fa_le32(h + 4);
        if (memcmp(h, "LIST", 4) == 0 || memcmp(h, "RIFF", 4) == 0) { a->pos += 12; continue; }
        a->pos += 8 + (int64_t)size + (size & 1);
        if (h[0] - '0' == a->number / 10 && h[1] - '0' == a->number % 10 && h[2] == 'w' && h[3] == 'b' && size > 0) {
            if (size > AVI_MAX_PACKET) return FA_ERROR_END_OF_STREAM;
            r = fa_br_read_alloc(&a->br, size, AVI_MAX_PACKET, &a->data, &a->data_cap);
            if (r != FA_OK) return r == FA_ERROR_OUT_OF_MEMORY ? r : FA_ERROR_END_OF_STREAM;
            out->data = a->data;
            out->size = size;
            out->pts = FA_DEMUX_NO_PTS;
            out->duration = -1;
            return FA_OK;
        }
        if (memcmp(h, "idx1", 4) == 0) return FA_ERROR_END_OF_STREAM;
    }
}

static fa_result avi_seek_packets(void* ctx, int64_t t, int packets_back, size_t bytes_back)
{
    avi* a = (avi*)ctx;
    if (!a->indexed) return FA_ERROR_NOT_SEEKABLE;
    a->cur = fa_pkt_seek_back(&a->pk, t, packets_back, bytes_back);
    return FA_OK;
}

static const fa_demux_ops avi_ops = { avi_next, avi_seek_packets };

/* ---- open ---------------------------------------------------------------------------- */

static fa_result avi_open(fa_decoder* dec)
{
    fa_source* src = dec->src;
    avi* a = (avi*)calloc(1, sizeof(avi));
    avi_header* h = (avi_header*)calloc(1, sizeof(avi_header));
    uint8_t head[12], id[4];
    uint8_t* hdrl = NULL;
    size_t hdrl_cap = 0, hdrl_len = 0;
    int64_t pos = 12, movi = -1, idx1 = -1, length = src->length;
    uint32_t idx1_size = 0;
    int have_hdrl = 0, top = 0;
    const avi_audio* s;
    fa_track_params tp;
    fa_codec_params* cp;
    const char* name;
    uint32_t tag;
    double tb;
    fa_result r;

    if (!a || !h) { free(a); free(h); return FA_ERROR_OUT_OF_MEMORY; }
    r = fa_br_init(&a->br, src);
    if (r != FA_OK) { free(a); free(h); return r; }
    if (fa_br_read(&a->br, head, 12) != 12 || memcmp(head, "RIFF", 4) != 0 || memcmp(head + 8, "AVI ", 4) != 0) {
        r = FA_ERROR_UNSUPPORTED_FORMAT;
        goto fail;
    }

    /* The top-level chunks: the header list is read whole; 'movi' and 'idx1' are noted. */
    while (top++ < 1000) {
        uint8_t c[12];
        uint32_t size;
        if (length > 0 && pos + 8 > length) break;
        if (fa_br_seek(&a->br, pos) != FA_OK || fa_br_read(&a->br, c, 8) != 8) break;
        size = fa_le32(c + 4);
        if (memcmp(c, "LIST", 4) == 0) {
            if (fa_br_read(&a->br, c + 8, 4) != 4) break;
            if (memcmp(c + 8, "hdrl", 4) == 0 && !have_hdrl && size >= 4) {
                if (size - 4 > AVI_MAX_HDRL) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto fail; }
                r = fa_br_read_alloc(&a->br, size - 4, AVI_MAX_HDRL, &hdrl, &hdrl_cap);
                if (r != FA_OK) { r = r == FA_ERROR_OUT_OF_MEMORY ? r : FA_ERROR_UNSUPPORTED_FORMAT; goto fail; }
                hdrl_len = size - 4;
                r = parse_hdrl(h, hdrl, hdrl_len, pos + 12);
                if (r != FA_OK) goto fail;
                have_hdrl = 1;
            } else if (memcmp(c + 8, "movi", 4) == 0 && movi < 0) {
                movi = pos + 8;
                /* Nothing readable after it, or a size that lies (a capture that was cut): the
                 * list runs to the end and has no index behind it. */
                if (!src->seekable || size < 4 || (length > 0 && (int64_t)size > length - pos - 8)) break;
            }
        } else if (memcmp(c, "idx1", 4) == 0 && idx1 < 0) {
            idx1 = pos + 8;
            idx1_size = size;
        }
        pos += 8 + (int64_t)size + (size & 1);
    }
    if (!have_hdrl || movi < 0) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto fail; }

    dec->track_count = h->n_audio;
    if (h->n_audio == 0) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto fail; }
    if (dec->audio_track >= h->n_audio) { r = FA_ERROR_NOT_FOUND; goto fail; }
    s = &h->audio[dec->audio_track < 0 ? 0 : dec->audio_track];
    a->number = s->number;
    id[0] = (uint8_t)('0' + s->number / 10);
    id[1] = (uint8_t)('0' + s->number % 10);
    id[2] = 'w';
    id[3] = 'b';

    tag = s->tag;
    if (tag == 0xFFFE && s->extra_len >= 22) tag = fa_le16(s->extra + 6);   /* the subformat GUID starts with the tag */
    name = name_for(s, tag);
    if (!name || s->channels == 0 || s->channels > 32 || s->rate == 0 || s->rate > FA_MAX_SAMPLE_RATE) {
        r = FA_ERROR_UNSUPPORTED_FORMAT;
        goto fail;
    }

    memset(&tp, 0, sizeof(tp));
    fa_codec_params_init(&tp.codec);
    cp = &tp.codec;
    cp->codec = name;
    cp->sample_rate = s->rate;
    cp->channels = s->channels;
    cp->bit_rate = (int64_t)s->avg_bytes * 8;
    cp->bits_per_coded_sample = s->bits;
    cp->block_align = s->block_align;
    if (strncmp(name, "pcm_", 4) == 0) {
        uint32_t bytes = name[4] == 'm' || name[4] == 'a' ? 1 : s->bits / 8;
        cp->block_align = s->channels * bytes;
        if (name[4] == 'm' || name[4] == 'a') cp->bits_per_coded_sample = 8;
        a->sample_size = cp->block_align;
        tb = 1.0 / s->rate;
    } else {
        a->sample_size = s->sample_size;
        if (s->scale == 0 || s->srate == 0) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto fail; }
        tb = (double)s->scale / (double)s->srate;
    }
    if (strcmp(name, "aac") == 0 && tag != 0x1600) {
        if (s->extra_len >= 2) {
            cp->extradata = s->extra;
            cp->extradata_size = s->extra_len;
        } else {
            cp->extradata_size = fa_aac_build_asc(a->asc, 2, s->rate, s->channels, 0);
            cp->extradata = a->asc;
        }
    } else if (strncmp(name, "wma", 3) == 0 && s->extra_len > 0) {
        cp->extradata = s->extra;
        cp->extradata_size = s->extra_len;
    }

    /* The packet list. */
    a->indexed = 0;
    if (src->seekable) {
        size_t i;
        a->indexed = 1;
        r = FA_OK;
        for (i = 0; i < s->n_ix && r == FA_OK; i++) r = read_std_index(a, (int64_t)s->ix_off[i], id);
        if (r == FA_OK && a->pk.n == 0 && idx1 >= 0) r = read_idx1(a, idx1, idx1_size, movi, id);
        if (r == FA_OK && a->pk.n == 0) r = walk_data(a, movi + 4, id);
        if (r != FA_OK) goto fail;
        fa_pkt_trim(&a->pk);
        if (a->pk.n == 0) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto fail; }
        a->audio_bytes = a->cum;
    } else {
        a->pos = movi + 4;
    }

    tp.time_base = tb;
    tp.start = 0;
    tp.last_pts = FA_DEMUX_NO_PTS;
    if (a->indexed) {
        int64_t total = a->sample_size ? a->cum / a->sample_size : a->chunks;
        tp.end = total;
        tp.clip_to_end = strncmp(name, "pcm_", 4) == 0;
        /* A variable-rate chunk of MPEG audio or AC-3 says how long it is: the last one. */
        if (a->sample_size == 0 && a->pk.n > 0) {
            fa_pkt p;
            fa_pkt_get(&a->pk, a->pk.n - 1, &p);
            if (p.size <= AVI_MAX_PACKET && fa_br_seek(&a->br, p.off) == FA_OK &&
                fa_br_read_alloc(&a->br, p.size, AVI_MAX_PACKET, &a->data, &a->data_cap) == FA_OK) {
                uint32_t out_rate = 0;
                int64_t n = fa_demux_packet_samples(name, s->channels, s->rate, a->data, p.size, &out_rate);
                if (n > 0 && out_rate > 0) {
                    tp.last_pts = p.pts;
                    tp.last_end = p.pts + (int64_t)((double)n / out_rate / tb + 0.5);
                    tp.last_exact = 1;
                }
            }
        }
    } else {
        tp.end = s->length > 0 && s->length < (1u << 31) ? (int64_t)s->length : FA_DEMUX_NO_PTS;
    }

    r = fa_track_open(&a->track, dec, &avi_ops, a, &tp);
    if (r != FA_OK) goto fail;
    free(hdrl);
    free_header(h);
    free(h);
    dec->impl = a;
    dec->seekable = a->indexed;
    dec->audio_bytes = a->indexed ? a->audio_bytes : -1;
    snprintf(dec->container, sizeof(dec->container), "avi");
    return FA_OK;
fail:
    free(hdrl);
    free_header(h);
    free(h);
    avi_free(a);
    return r;
}

static fa_result avi_read(fa_decoder* dec, float* out, fa_frames frames, fa_frames* got)
{
    return fa_track_read(&((avi*)dec->impl)->track, out, frames, got);
}

static fa_result avi_seek(fa_decoder* dec, fa_frames frame)
{
    if (!dec->seekable) return FA_ERROR_NOT_SEEKABLE;
    return fa_track_seek(&((avi*)dec->impl)->track, frame);
}

static void avi_close(fa_decoder* dec)
{
    avi* a = (avi*)dec->impl;
    if (!a) return;
    fa_track_close(&a->track);
    avi_free(a);
    dec->impl = NULL;
}

/* "RIFF", a size, "AVI ". */
static int avi_probe(const uint8_t* d, size_t n, size_t id3)
{
    (void)id3;
    return n >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "AVI ", 4) == 0 ? 100 : 0;
}

const fa_decoder_vtable fa_decoder_avi = {
    "avi", "AVI video files (audio track)", "avi",
    "video/x-msvideo;video/avi;video/msvideo",
    avi_probe, avi_open, avi_read, avi_seek, avi_close, 1, NULL, NULL
};
