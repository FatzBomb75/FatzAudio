/* Matroska and WebM audio tracks, written from RFC 8794 (EBML), the Matroska
 * specification (RFC 9559, element list at matroska.org) and the WebM container
 * guidelines.
 *
 * The segment's header elements (Info, Tracks, SeekHead, Cues) are read into
 * memory; clusters are walked element by element, reading only the block headers
 * of other tracks and skipping their data. Unknown-size segments and clusters
 * (live WebM) end where the next top-level element starts. Seeks start from the
 * closest earlier cluster known from Cues or from clusters seen while reading,
 * then scan block headers to the packet holding the target.
 *
 * Matroska timestamps are rounded to the TimestampScale (1 ms by default); the
 * track decoder snaps them to the packet grid of constant-duration codecs. */
#include "demux.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Element IDs (with their length marker bits, as written in the file). */
#define ID_EBML          0x1A45DFA3u
#define ID_DOCTYPE       0x4282u
#define ID_SEGMENT       0x18538067u
#define ID_SEEKHEAD      0x114D9B74u
#define ID_SEEK          0x4DBBu
#define ID_SEEKID        0x53ABu
#define ID_SEEKPOSITION  0x53ACu
#define ID_INFO          0x1549A966u
#define ID_TIMESTAMPSCALE 0x2AD7B1u
#define ID_DURATION      0x4489u
#define ID_TRACKS        0x1654AE6Bu
#define ID_TRACKENTRY    0xAEu
#define ID_TRACKNUMBER   0xD7u
#define ID_TRACKTYPE     0x83u
#define ID_FLAGENABLED   0xB9u
#define ID_FLAGDEFAULT   0x88u
#define ID_CODECID       0x86u
#define ID_CODECPRIVATE  0x63A2u
#define ID_CODECDELAY    0x56AAu
#define ID_SEEKPREROLL   0x56BBu
#define ID_AUDIO         0xE1u
#define ID_SAMPLINGFREQ  0xB5u
#define ID_OUTSAMPLINGFREQ 0x78B5u
#define ID_CHANNELS      0x9Fu
#define ID_BITDEPTH      0x6264u
#define ID_CONTENTENCODINGS 0x6D80u
#define ID_CONTENTENCODING 0x6240u
#define ID_CONTENTENCODINGTYPE 0x5033u
#define ID_CONTENTCOMPRESSION 0x5034u
#define ID_CONTENTCOMPALGO 0x4254u
#define ID_CONTENTCOMPSETTINGS 0x4255u
#define ID_CUES          0x1C53BB6Bu
#define ID_CUEPOINT      0xBBu
#define ID_CUETIME       0xB3u
#define ID_CUETRACKPOSITIONS 0xB7u
#define ID_CUECLUSTERPOSITION 0xF1u
#define ID_CLUSTER       0x1F43B675u
#define ID_TIMESTAMP     0xE7u
#define ID_SIMPLEBLOCK   0xA3u
#define ID_BLOCKGROUP    0xA0u
#define ID_BLOCK         0xA1u
#define ID_BLOCKDURATION 0x9Bu
#define ID_DISCARDPADDING 0x75A2u
#define ID_TAGS          0x1254C367u
#define ID_CHAPTERS      0x1043A770u
#define ID_ATTACHMENTS   0x1941A469u

#define MKV_MAX_HEADER (64u << 20)    /* Tracks, Cues, ... read into memory */
#define MKV_MAX_BLOCK (64u << 20)
#define MKV_MAX_LACE 256
#define MKV_SCAN_RING 64
#define MKV_MAX_INDEX (1u << 20)

typedef struct mkv_ref {          /* where a block of the track is, for seeking */
    int64_t elem;                 /* SimpleBlock or BlockGroup element */
    int64_t cluster, cluster_end, cluster_ts;
    int64_t pts;                  /* ns */
    uint32_t size;
} mkv_ref;

typedef struct mkv_mark {         /* a cluster at `pos` starts at or before `ts` (ns) */
    int64_t pos, ts;
} mkv_mark;

typedef struct mkv {
    fa_br br;
    fa_track track;
    int64_t seg_start, seg_end;   /* segment data; end -1 = unknown */
    int64_t first_cluster;        /* -1 = none */
    uint64_t scale;               /* ns per timestamp unit */
    double duration;              /* Info Duration in timestamp units, < 0 = unknown */
    uint64_t track_num;
    int64_t codec_delay, seek_preroll;   /* ns */
    uint8_t* strip;               /* header stripping (ContentCompAlgo 3) */
    size_t strip_len;
    uint8_t* extra;
    size_t extra_len;
    mkv_mark* index;              /* sorted by pos */
    size_t nindex, index_cap;
    /* Reader state. */
    int in_cluster;
    int64_t cluster_pos, cluster_end, cluster_ts;
    /* The current block and its frames. */
    uint8_t* blk;
    size_t blk_cap, blk_len;
    size_t lace_off[MKV_MAX_LACE], lace_size[MKV_MAX_LACE];
    int nframes, frame_i;
    int64_t blk_pts, blk_dur, blk_discard;
    uint8_t* frame;
    size_t frame_cap;
} mkv;

/* ---- EBML in memory ------------------------------------------------------------------ */

typedef struct ebml_el {
    uint32_t id;
    const uint8_t* p;
    size_t len;
} ebml_el;

/* Variable-length integer (RFC 8794 4): length from the leading zero bits. */
static int vint_len(uint8_t first)
{
    int n = 1;
    while (n <= 8 && !(first & (0x80 >> (n - 1)))) n++;
    return n;
}

static int mem_next(const uint8_t** cur, const uint8_t* end, ebml_el* e)
{
    const uint8_t* p = *cur;
    size_t n = (size_t)(end - p), il, sl, i;
    uint64_t size;
    int unknown = 1;
    if (n < 2) return 0;
    il = (size_t)vint_len(p[0]);
    if (il > 4 || il >= n) return 0;
    e->id = 0;
    for (i = 0; i < il; i++) e->id = (e->id << 8) | p[i];
    sl = (size_t)vint_len(p[il]);
    if (sl > 8 || il + sl > n) return 0;
    size = p[il] & (0xFFu >> sl);
    if (size != (0xFFu >> sl)) unknown = 0;
    for (i = 1; i < sl; i++) {
        if (p[il + i] != 0xFF) unknown = 0;
        size = (size << 8) | p[il + i];
    }
    p += il + sl;
    n -= il + sl;
    if (unknown || size > n) size = n;   /* unknown or cut short: to the end of the parent */
    e->p = p;
    e->len = (size_t)size;
    *cur = p + size;
    return 1;
}

static uint64_t el_uint(const ebml_el* e)
{
    uint64_t v = 0;
    size_t i;
    for (i = 0; i < e->len && i < 8; i++) v = (v << 8) | e->p[i];
    return v;
}

static int64_t el_sint(const ebml_el* e)
{
    uint64_t v = el_uint(e);
    size_t n = e->len < 8 ? e->len : 8;
    if (n > 0 && n < 8 && (e->p[0] & 0x80)) v |= ~(uint64_t)0 << (n * 8);
    return (int64_t)v;
}

static double el_float(const ebml_el* e)
{
    if (e->len == 4) {
        union { uint32_t u; float f; } v;
        v.u = fa_be32(e->p);
        return v.f;
    }
    if (e->len == 8) {
        union { uint64_t u; double d; } v;
        v.u = fa_be64(e->p);
        return v.d;
    }
    return 0.0;
}

static int el_is(const ebml_el* e, const char* s)
{
    size_t n = strlen(s), len = e->len;
    while (len > 0 && e->p[len - 1] == 0) len--;   /* strings may be zero-padded */
    return len == n && memcmp(e->p, s, n) == 0;
}

static int el_starts(const ebml_el* e, const char* s)
{
    size_t n = strlen(s);
    return e->len >= n && memcmp(e->p, s, n) == 0;
}

/* ---- EBML from the file ------------------------------------------------------------------ */

/* Reads an element header at the reader's position. *size = -1: unknown size. */
static int read_header(mkv* m, uint32_t* id, int64_t* size, int64_t* data)
{
    uint8_t b[12];
    int il, sl, i, unknown = 1;
    uint64_t s;
    if (fa_br_read(&m->br, b, 1) < 1) return 0;
    il = vint_len(b[0]);
    if (il > 4) return 0;
    if (fa_br_read(&m->br, b + 1, (size_t)il) < (size_t)il) return 0;   /* rest of the ID + size's first byte */
    *id = 0;
    for (i = 0; i < il; i++) *id = (*id << 8) | b[i];
    sl = vint_len(b[il]);
    if (sl > 8) return 0;
    if (sl > 1 && fa_br_read(&m->br, b + il + 1, (size_t)sl - 1) < (size_t)sl - 1) return 0;
    s = b[il] & (0xFFu >> sl);
    if (s != (0xFFu >> sl)) unknown = 0;
    for (i = 1; i < sl; i++) {
        if (b[il + i] != 0xFF) unknown = 0;
        s = (s << 8) | b[il + i];
    }
    if (s > (uint64_t)INT64_MAX / 4) return 0;
    *size = unknown ? -1 : (int64_t)s;
    *data = fa_br_tell(&m->br);
    return 1;
}

/* Reads a whole element body (known size) into a new buffer. */
static fa_result read_body(mkv* m, int64_t size, uint8_t** out)
{
    uint8_t* p;
    if (size < 0 || size > (int64_t)MKV_MAX_HEADER) return FA_ERROR_UNSUPPORTED_FORMAT;
    if (m->br.src->length > 0 && size > m->br.src->length) return FA_ERROR_UNSUPPORTED_FORMAT;
    p = (uint8_t*)malloc((size_t)size + 1);
    if (!p) return FA_ERROR_OUT_OF_MEMORY;
    if (fa_br_read(&m->br, p, (size_t)size) < (size_t)size) { free(p); return FA_ERROR_END_OF_STREAM; }
    *out = p;
    return FA_OK;
}

static int is_top_level(uint32_t id)
{
    return id == ID_CLUSTER || id == ID_CUES || id == ID_TAGS || id == ID_CHAPTERS || id == ID_ATTACHMENTS ||
           id == ID_INFO || id == ID_TRACKS || id == ID_SEEKHEAD || id == ID_EBML || id == ID_SEGMENT;
}

/* ---- cluster index ------------------------------------------------------------------------ */

static void index_add(mkv* m, int64_t pos, int64_t ts)
{
    size_t lo = 0, hi = m->nindex;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (m->index[mid].pos < pos) lo = mid + 1;
        else hi = mid;
    }
    if (lo < m->nindex && m->index[lo].pos == pos) {
        if (ts < m->index[lo].ts) m->index[lo].ts = ts;
        return;
    }
    if (m->nindex == m->index_cap) {
        size_t cap = m->index_cap ? m->index_cap * 2 : 256;
        mkv_mark* p;
        if (m->nindex >= MKV_MAX_INDEX) return;
        p = (mkv_mark*)realloc(m->index, cap * sizeof(mkv_mark));
        if (!p) return;                       /* the index only speeds seeks up */
        m->index = p;
        m->index_cap = cap;
    }
    memmove(m->index + lo + 1, m->index + lo, (m->nindex - lo) * sizeof(mkv_mark));
    m->index[lo].pos = pos;
    m->index[lo].ts = ts;
    m->nindex++;
}

static void parse_cues(mkv* m, const uint8_t* p, size_t len)
{
    const uint8_t* cur = p;
    ebml_el cp;
    while (mem_next(&cur, p + len, &cp)) {
        const uint8_t* c2 = cp.p;
        ebml_el e;
        int64_t ts = -1;
        if (cp.id != ID_CUEPOINT) continue;
        while (mem_next(&c2, cp.p + cp.len, &e)) {
            if (e.id == ID_CUETIME) ts = (int64_t)el_uint(&e);
            else if (e.id == ID_CUETRACKPOSITIONS && ts >= 0) {
                const uint8_t* c3 = e.p;
                ebml_el f;
                while (mem_next(&c3, e.p + e.len, &f))
                    if (f.id == ID_CUECLUSTERPOSITION && el_uint(&f) < (uint64_t)INT64_MAX / 4)
                        index_add(m, m->seg_start + (int64_t)el_uint(&f), ts * (int64_t)m->scale);
            }
        }
    }
}

/* ---- tracks ---------------------------------------------------------------------------------- */

typedef struct mkv_entry {
    uint64_t number, type;
    int enabled, is_default;
    ebml_el codec_id, priv;
    int has_priv;
    double rate, out_rate;
    uint64_t channels, bits;
    int64_t codec_delay, seek_preroll;
    int bad_encoding;
    ebml_el strip;
    int has_strip;
} mkv_entry;

static void parse_entry(const ebml_el* te, mkv_entry* t)
{
    const uint8_t* cur = te->p;
    ebml_el e;
    memset(t, 0, sizeof(*t));
    t->enabled = 1;
    t->is_default = 1;
    t->rate = 8000.0;
    t->channels = 1;
    while (mem_next(&cur, te->p + te->len, &e)) {
        switch (e.id) {
        case ID_TRACKNUMBER: t->number = el_uint(&e); break;
        case ID_TRACKTYPE: t->type = el_uint(&e); break;
        case ID_FLAGENABLED: t->enabled = el_uint(&e) != 0; break;
        case ID_FLAGDEFAULT: t->is_default = el_uint(&e) != 0; break;
        case ID_CODECID: t->codec_id = e; break;
        case ID_CODECPRIVATE: t->priv = e; t->has_priv = e.len > 0; break;
        case ID_CODECDELAY: t->codec_delay = (int64_t)(el_uint(&e) & 0x3FFFFFFFFFFFFFFFull); break;
        case ID_SEEKPREROLL: t->seek_preroll = (int64_t)(el_uint(&e) & 0x3FFFFFFFFFFFFFFFull); break;
        case ID_AUDIO: {
            const uint8_t* c2 = e.p;
            ebml_el a;
            while (mem_next(&c2, e.p + e.len, &a)) {
                if (a.id == ID_SAMPLINGFREQ) t->rate = el_float(&a);
                else if (a.id == ID_OUTSAMPLINGFREQ) t->out_rate = el_float(&a);
                else if (a.id == ID_CHANNELS) t->channels = el_uint(&a);
                else if (a.id == ID_BITDEPTH) t->bits = el_uint(&a);
            }
            break;
        }
        case ID_CONTENTENCODINGS: {
            /* Only header stripping is understood; zlib, bzip2, LZO or encryption is not. */
            const uint8_t* c2 = e.p;
            ebml_el enc;
            while (mem_next(&c2, e.p + e.len, &enc)) {
                const uint8_t* c3 = enc.p;
                ebml_el f;
                if (enc.id != ID_CONTENTENCODING) continue;
                while (mem_next(&c3, enc.p + enc.len, &f)) {
                    if (f.id == ID_CONTENTENCODINGTYPE && el_uint(&f) != 0) t->bad_encoding = 1;
                    else if (f.id == ID_CONTENTCOMPRESSION) {
                        const uint8_t* c4 = f.p;
                        ebml_el g;
                        uint64_t algo = 0;
                        while (mem_next(&c4, f.p + f.len, &g)) {
                            if (g.id == ID_CONTENTCOMPALGO) algo = el_uint(&g);
                            else if (g.id == ID_CONTENTCOMPSETTINGS) { t->strip = g; t->has_strip = 1; }
                        }
                        if (algo != 3) t->bad_encoding = 1;
                    }
                }
            }
            break;
        }
        default: break;
        }
    }
}

static int set_extra(mkv* m, const uint8_t* p, size_t n)
{
    free(m->extra);
    m->extra = (uint8_t*)malloc(n ? n : 1);
    if (!m->extra) return 0;
    memcpy(m->extra, p, n);
    m->extra_len = n;
    return 1;
}

static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static const char* pcm_name(int is_float, int big, uint64_t bits)
{
    if (is_float) return bits == 64 ? "pcm_f64le" : bits == 32 ? "pcm_f32le" : NULL;
    switch (bits) {
    case 8: return "pcm_u8";             /* 8-bit Matroska PCM is unsigned in either byte order */
    case 16: return big ? "pcm_s16be" : "pcm_s16le";
    case 24: return big ? "pcm_s24be" : "pcm_s24le";
    case 32: return big ? "pcm_s32be" : "pcm_s32le";
    default: return NULL;
    }
}

/* CodecID -> codec parameters. */
static fa_result map_codec(mkv* m, const mkv_entry* t, fa_codec_params* cp)
{
    const ebml_el* id = &t->codec_id;
    const char* name = NULL;
    uint32_t rate = t->rate > 0 && t->rate < 1e7 ? (uint32_t)(t->rate + 0.5) : 0;
    uint32_t channels = t->channels > 0 && t->channels <= 255 ? (uint32_t)t->channels : 0;
    uint32_t block_align = 0;

    if (el_starts(id, "A_AAC")) {
        name = "aac";
        if (t->has_priv) {
            if (!set_extra(m, t->priv.p, t->priv.len)) return FA_ERROR_OUT_OF_MEMORY;
        } else {
            /* Legacy IDs carry the profile in the name: build the config. */
            uint8_t asc[FA_AAC_ASC_MAX];
            int aot = 2;
            uint32_t sbr = t->out_rate > t->rate ? (uint32_t)(t->out_rate + 0.5) : 0;
            if (el_starts(id, "A_AAC/MPEG2/MAIN") || el_starts(id, "A_AAC/MPEG4/MAIN")) aot = 1;
            else if (el_starts(id, "A_AAC/MPEG2/SSR") || el_starts(id, "A_AAC/MPEG4/SSR")) aot = 3;
            else if (el_starts(id, "A_AAC/MPEG4/LTP")) aot = 4;
            if (id->len >= 4 && memcmp(id->p + id->len - 4, "/SBR", 4) == 0 && sbr == 0) sbr = rate * 2;
            if (!set_extra(m, asc, fa_aac_build_asc(asc, aot, rate, channels, sbr))) return FA_ERROR_OUT_OF_MEMORY;
        }
    } else if (el_starts(id, "A_AC3")) name = "ac3";
    else if (el_is(id, "A_EAC3")) name = "eac3";
    else if (el_starts(id, "A_DTS")) name = "dts";
    else if (el_is(id, "A_TRUEHD")) name = "truehd";
    else if (el_is(id, "A_MLP")) name = "mlp";
    else if (el_is(id, "A_MPEG/L3")) name = "mp3";
    else if (el_is(id, "A_MPEG/L2")) name = "mp2";
    else if (el_is(id, "A_MPEG/L1")) name = "mp1";
    else if (el_is(id, "A_OPUS")) {
        name = "opus";
        if (t->has_priv) {
            if (!set_extra(m, t->priv.p, t->priv.len)) return FA_ERROR_OUT_OF_MEMORY;
        } else if (channels >= 1 && channels <= 2) {
            /* RFC 7845 OpusHead for mono or stereo (mapping family 0). */
            uint8_t h[19];
            uint32_t pre = (uint32_t)((double)t->codec_delay * 48000.0 / 1e9 + 0.5);
            memcpy(h, "OpusHead", 8);
            h[8] = 1;
            h[9] = (uint8_t)channels;
            h[10] = (uint8_t)pre; h[11] = (uint8_t)(pre >> 8);
            h[12] = (uint8_t)rate; h[13] = (uint8_t)(rate >> 8); h[14] = (uint8_t)(rate >> 16); h[15] = (uint8_t)(rate >> 24);
            h[16] = h[17] = h[18] = 0;
            if (!set_extra(m, h, sizeof(h))) return FA_ERROR_OUT_OF_MEMORY;
        } else {
            return FA_ERROR_UNSUPPORTED_FORMAT;
        }
        rate = 48000;
    } else if (el_is(id, "A_VORBIS") || el_is(id, "A_FLAC")) {
        name = el_is(id, "A_VORBIS") ? "vorbis" : "flac";
        if (!t->has_priv) return FA_ERROR_UNSUPPORTED_FORMAT;
        if (!set_extra(m, t->priv.p, t->priv.len)) return FA_ERROR_OUT_OF_MEMORY;
    } else if (el_is(id, "A_ALAC")) {
        /* The decoder wants an 'alac' box around the ALACSpecificConfig. */
        uint8_t box[12 + 64];
        const uint8_t* cfg = t->priv.p;
        size_t n = t->priv.len;
        if (n == 28 && fa_be32(cfg) == 0) { cfg += 4; n -= 4; }   /* with the FullBox version */
        if (!t->has_priv || n < 24 || n > 64) return FA_ERROR_UNSUPPORTED_FORMAT;
        box[0] = 0; box[1] = 0; box[2] = 0; box[3] = (uint8_t)(12 + n);
        memcpy(box + 4, "alac", 4);
        memset(box + 8, 0, 4);
        memcpy(box + 12, cfg, n);
        if (!set_extra(m, box, 12 + n)) return FA_ERROR_OUT_OF_MEMORY;
        name = "alac";
    } else if (el_is(id, "A_PCM/INT/LIT") || el_is(id, "A_PCM/INT/BIG") || el_is(id, "A_PCM/FLOAT/IEEE")) {
        name = pcm_name(el_is(id, "A_PCM/FLOAT/IEEE"), el_is(id, "A_PCM/INT/BIG"), t->bits);
        if (!name) return FA_ERROR_UNSUPPORTED_FORMAT;
        block_align = channels * (uint32_t)(t->bits / 8);
    } else if (el_is(id, "A_MS/ACM")) {
        /* A WAVEFORMATEX (little endian) in CodecPrivate. */
        const uint8_t* w = t->priv.p;
        uint32_t tag, bits;
        if (!t->has_priv || t->priv.len < 16) return FA_ERROR_UNSUPPORTED_FORMAT;
        tag = le16(w);
        channels = le16(w + 2);
        rate = le32(w + 4);
        bits = le16(w + 14);
        if (tag == 0xFFFE && t->priv.len >= 18 + 22) tag = le16(w + 18 + 6);
        if (tag == 1 || tag == 3) {
            name = pcm_name(tag == 3, 0, bits);
            block_align = channels * (bits / 8);
        } else if (tag == 0x55) name = "mp3";
        else if (tag == 0x50) name = "mp2";
        else if (tag == 0x2000) name = "ac3";
        else if (tag == 0x2001) name = "dts";
        if (!name) return FA_ERROR_UNSUPPORTED_FORMAT;
    } else {
        return FA_ERROR_UNSUPPORTED_FORMAT;
    }
    if (strncmp(name, "pcm_", 4) == 0 && (channels == 0 || block_align == 0)) return FA_ERROR_UNSUPPORTED_FORMAT;
    cp->codec = name;
    cp->sample_rate = rate;
    cp->channels = channels;
    cp->extradata = m->extra;
    cp->extradata_size = m->extra_len;
    cp->block_align = block_align;
    if (t->bits) cp->bits_per_coded_sample = (uint32_t)t->bits;
    return FA_OK;
}

/* Picks audio track `want` (-1 = the default one) and counts them. */
static fa_result parse_tracks(mkv* m, const uint8_t* p, size_t len, int32_t want, int* count, fa_codec_params* cp)
{
    const uint8_t* cur = p;
    ebml_el te;
    mkv_entry t, chosen, first, def;
    int n = 0, have = 0, have_first = 0, have_def = 0;
    memset(&chosen, 0, sizeof(chosen));
    memset(&first, 0, sizeof(first));
    memset(&def, 0, sizeof(def));
    while (mem_next(&cur, p + len, &te)) {
        if (te.id != ID_TRACKENTRY) continue;
        parse_entry(&te, &t);
        if (t.type != 2 || t.number == 0) continue;
        if (n == want) { chosen = t; have = 1; }
        if (!have_first) { first = t; have_first = 1; }
        if (!have_def && t.enabled && t.is_default) { def = t; have_def = 1; }
        n++;
    }
    *count = n;
    if (n == 0) return FA_ERROR_UNSUPPORTED_FORMAT;
    if (want < 0) { chosen = have_def ? def : first; have = 1; }
    if (!have) return FA_ERROR_NOT_FOUND;
    if (chosen.bad_encoding) return FA_ERROR_UNSUPPORTED_FORMAT;
    m->track_num = chosen.number;
    m->codec_delay = chosen.codec_delay;
    m->seek_preroll = chosen.seek_preroll;
    if (chosen.has_strip && chosen.strip.len > 0) {
        m->strip = (uint8_t*)malloc(chosen.strip.len);
        if (!m->strip) return FA_ERROR_OUT_OF_MEMORY;
        memcpy(m->strip, chosen.strip.p, chosen.strip.len);
        m->strip_len = chosen.strip.len;
    }
    return map_codec(m, &chosen, cp);
}

/* ---- blocks ---------------------------------------------------------------------------------- */

/* Splits the block in m->blk (header of `hlen` bytes, `flags`) into its laced frames. */
static int split_lacing(mkv* m, size_t hlen, uint8_t flags)
{
    const uint8_t* b = m->blk;
    size_t len = m->blk_len, p = hlen, sum = 0;
    int count, i, lacing = (flags >> 1) & 3;
    if (lacing == 0) {
        m->lace_off[0] = hlen;
        m->lace_size[0] = len - hlen;
        m->nframes = 1;
        return 1;
    }
    if (p >= len) return 0;
    count = b[p++] + 1;
    if (lacing == 1) {                       /* Xiph: sizes as runs of 255 */
        for (i = 0; i < count - 1; i++) {
            size_t s = 0;
            uint8_t v;
            do {
                if (p >= len) return 0;
                v = b[p++];
                s += v;
            } while (v == 255);
            m->lace_size[i] = s;
            sum += s;
        }
    } else if (lacing == 3) {                /* EBML: a size, then signed differences */
        int64_t prev = 0;
        for (i = 0; i < count - 1; i++) {
            int l, k;
            uint64_t v;
            int64_t s;
            if (p >= len) return 0;
            l = vint_len(b[p]);
            if (l > 8 || p + (size_t)l > len) return 0;
            v = b[p] & (0xFFu >> l);
            for (k = 1; k < l; k++) v = (v << 8) | b[p + (size_t)k];
            p += (size_t)l;
            if (i == 0) s = (int64_t)v;
            else s = prev + ((int64_t)v - (((int64_t)1 << (7 * l - 1)) - 1));
            if (s < 0 || s > (int64_t)len) return 0;
            m->lace_size[i] = (size_t)s;
            prev = s;
            sum += (size_t)s;
        }
    } else {                                 /* fixed: equal sizes */
        size_t each = (len - p) / (size_t)count;
        if (each * (size_t)count != len - p) return 0;
        for (i = 0; i < count - 1; i++) m->lace_size[i] = each;
        sum = each * (size_t)(count - 1);
    }
    if (p + sum > len) return 0;
    m->lace_size[count - 1] = len - p - sum;
    for (i = 0; i < count; i++) {
        m->lace_off[i] = p;
        p += m->lace_size[i];
    }
    m->nframes = count;
    return 1;
}

/* The block body at the reader (`size` bytes): 1 = ours (read if `full`, else
 * just noted in *ref), 0 = another track's (skipped), <0 = error. */
static int take_block(mkv* m, int64_t elem, int64_t size, int full, mkv_ref* ref, int64_t end)
{
    uint8_t h[12];
    size_t got, hl;
    int l, k;
    uint64_t track;
    int64_t pts;
    if (size < 4 || size > (int64_t)MKV_MAX_BLOCK || (m->br.src->length > 0 && size > m->br.src->length)) {
        fa_br_seek(&m->br, end);
        return 0;
    }
    got = fa_br_read(&m->br, h, size < 12 ? (size_t)size : 12);
    if (got < 4) return -1;
    l = vint_len(h[0]);
    if (l > 8 || (size_t)l + 3 > got) { fa_br_seek(&m->br, end); return 0; }
    track = h[0] & (0xFFu >> l);
    for (k = 1; k < l; k++) track = (track << 8) | h[k];
    if (track != m->track_num) { fa_br_seek(&m->br, end); return 0; }
    hl = (size_t)l + 3;
    pts = (m->cluster_ts + (int16_t)fa_be16(h + l)) * (int64_t)m->scale;
    if (!full) {
        ref->elem = elem;
        ref->cluster = m->cluster_pos;
        ref->cluster_end = m->cluster_end;
        ref->cluster_ts = m->cluster_ts;
        ref->pts = pts;
        ref->size = (uint32_t)size;
        fa_br_seek(&m->br, end);
        return 1;
    }
    if ((size_t)size + 1 > m->blk_cap) {
        size_t cap = m->blk_cap ? m->blk_cap : 4096;
        uint8_t* p;
        while (cap < (size_t)size + 1) cap *= 2;
        p = (uint8_t*)realloc(m->blk, cap);
        if (!p) return -2;
        m->blk = p;
        m->blk_cap = cap;
    }
    memcpy(m->blk, h, got);
    if (fa_br_read(&m->br, m->blk + got, (size_t)size - got) < (size_t)size - got) return -1;
    m->blk_len = (size_t)size;
    m->blk_pts = pts;
    if (!split_lacing(m, hl, h[l + 2])) { m->nframes = 0; fa_br_seek(&m->br, end); return 0; }
    m->frame_i = 0;
    fa_br_seek(&m->br, end);
    return 1;
}

/* Walks the clusters to the next block of the track. `full`: read it (frames
 * ready in m), else only note where it is (*ref). */
static fa_result step(mkv* m, int full, mkv_ref* ref)
{
    for (;;) {
        uint32_t id;
        int64_t size, data, pos;
        pos = fa_br_tell(&m->br);
        if (m->in_cluster && m->cluster_end >= 0 && pos >= m->cluster_end) m->in_cluster = 0;
        if (m->seg_end >= 0 && pos >= m->seg_end) return FA_ERROR_END_OF_STREAM;
        if (!read_header(m, &id, &size, &data)) return FA_ERROR_END_OF_STREAM;
        /* An unknown-size cluster ends where a top-level element begins. */
        if (m->in_cluster && is_top_level(id)) m->in_cluster = 0;
        if (!m->in_cluster) {
            if (id == ID_CLUSTER) {
                m->in_cluster = 1;
                m->cluster_pos = pos;
                m->cluster_end = size < 0 ? -1 : data + size;
                m->cluster_ts = 0;
                continue;
            }
            if (id == ID_EBML || id == ID_SEGMENT || size < 0) return FA_ERROR_END_OF_STREAM;
            if (fa_br_seek(&m->br, data + size) != FA_OK) return FA_ERROR_END_OF_STREAM;
            continue;
        }
        if (size < 0) return FA_ERROR_END_OF_STREAM;    /* only clusters may have an unknown size here */
        if (id == ID_TIMESTAMP) {
            uint8_t b[8];
            uint64_t v = 0;
            size_t i, n = size > 8 ? 8 : (size_t)size;
            if (fa_br_read(&m->br, b, n) < n) return FA_ERROR_END_OF_STREAM;
            for (i = 0; i < n; i++) v = (v << 8) | b[i];
            if (v > (uint64_t)INT64_MAX / (m->scale + 1)) return FA_ERROR_END_OF_STREAM;
            m->cluster_ts = (int64_t)v;
            index_add(m, m->cluster_pos, m->cluster_ts * (int64_t)m->scale);
            fa_br_seek(&m->br, data + size);
            continue;
        }
        if (id == ID_SIMPLEBLOCK) {
            int r = take_block(m, pos, size, full, ref, data + size);
            if (r == -2) return FA_ERROR_OUT_OF_MEMORY;
            if (r < 0) return FA_ERROR_END_OF_STREAM;
            if (r == 1 && (!full || m->nframes > 0)) {
                m->blk_dur = -1;
                m->blk_discard = 0;
                return FA_OK;
            }
            continue;
        }
        if (id == ID_BLOCKGROUP) {
            /* Block, BlockDuration and DiscardPadding may come in any order. */
            int64_t end = data + size, dur = -1, discard = 0;
            int ours = 0;
            while (fa_br_tell(&m->br) < end) {
                uint32_t cid;
                int64_t cs, cd;
                if (!read_header(m, &cid, &cs, &cd) || cs < 0 || cd + cs > end) break;
                if (cid == ID_BLOCK) {
                    int r = take_block(m, pos, cs, full, ref, cd + cs);
                    if (r == -2) return FA_ERROR_OUT_OF_MEMORY;
                    if (r < 0) return FA_ERROR_END_OF_STREAM;
                    if (r == 0) break;                    /* another track's group */
                    ours = !full || m->nframes > 0;
                    if (!full) break;
                } else if (cid == ID_BLOCKDURATION || cid == ID_DISCARDPADDING) {
                    uint8_t b[8];
                    ebml_el e;
                    size_t n = cs > 8 ? 8 : (size_t)cs;
                    if (fa_br_read(&m->br, b, n) < n) return FA_ERROR_END_OF_STREAM;
                    e.p = b;
                    e.len = n;
                    if (cid == ID_BLOCKDURATION) dur = (int64_t)(el_uint(&e) & 0xFFFFFFFFFFFFull) * (int64_t)m->scale;
                    else discard = el_sint(&e);
                    fa_br_seek(&m->br, cd + cs);
                } else {
                    fa_br_seek(&m->br, cd + cs);
                }
            }
            fa_br_seek(&m->br, end);
            if (ours) {
                m->blk_dur = dur;
                m->blk_discard = discard > 0 ? discard : 0;
                return FA_OK;
            }
            continue;
        }
        if (fa_br_seek(&m->br, data + size) != FA_OK) return FA_ERROR_END_OF_STREAM;
    }
}

static fa_result mkv_next(void* ctx, fa_demux_packet* out)
{
    mkv* m = (mkv*)ctx;
    while (m->frame_i >= m->nframes) {
        fa_result r = step(m, 1, NULL);
        if (r != FA_OK) return r;
    }
    {
        int i = m->frame_i++;
        const uint8_t* d = m->blk + m->lace_off[i];
        size_t n = m->lace_size[i];
        if (m->strip_len > 0) {
            /* Header stripping: the removed bytes go back in front of every frame. */
            if (m->strip_len + n > m->frame_cap) {
                size_t cap = m->strip_len + n + 64;
                uint8_t* p = (uint8_t*)realloc(m->frame, cap);
                if (!p) return FA_ERROR_OUT_OF_MEMORY;
                m->frame = p;
                m->frame_cap = cap;
            }
            memcpy(m->frame, m->strip, m->strip_len);
            memcpy(m->frame + m->strip_len, d, n);
            d = m->frame;
            n += m->strip_len;
        }
        out->data = d;
        out->size = n;
        out->pts = i == 0 ? m->blk_pts : FA_DEMUX_NO_PTS;
        out->duration = m->nframes == 1 ? m->blk_dur : -1;
        out->trim_end = i == m->nframes - 1 ? m->blk_discard : 0;
    }
    return FA_OK;
}

static void restart_at(mkv* m, const mkv_ref* r)
{
    m->in_cluster = 1;
    m->cluster_pos = r->cluster;
    m->cluster_end = r->cluster_end;
    m->cluster_ts = r->cluster_ts;
    m->nframes = m->frame_i = 0;
    fa_br_seek(&m->br, r->elem);
}

static fa_result mkv_seek_packets(void* ctx, int64_t t, int packets_back, size_t bytes_back)
{
    mkv* m = (mkv*)ctx;
    mkv_ref ring[MKV_SCAN_RING], next;
    size_t lo = 0, hi = m->nindex;
    int64_t start = m->first_cluster;
    int count = 0, head = 0, have_next = 0, j, n = 0;
    size_t bytes = 0;
    if (!m->br.src->seekable) return FA_ERROR_NOT_SEEKABLE;
    if (start < 0) return FA_OK;
    /* The last known cluster at or before t, and one more back so the packets
     * before the target are found too. */
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (m->index[mid].ts <= t) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= 2) start = m->index[lo - 2].pos;
    else if (lo == 1) start = m->index[0].pos;
    if (start < m->first_cluster) start = m->first_cluster;

    m->in_cluster = 0;
    m->nframes = m->frame_i = 0;
    if (fa_br_seek(&m->br, start) != FA_OK) return FA_ERROR_IO;
    for (;;) {
        mkv_ref r;
        if (step(m, 0, &r) != FA_OK) break;
        if (r.pts > t) { next = r; have_next = 1; break; }
        ring[head] = r;
        head = (head + 1) % MKV_SCAN_RING;
        if (count < MKV_SCAN_RING) count++;
    }
    if (count == 0) {
        if (have_next) restart_at(m, &next);           /* the target is before the first packet */
        return FA_OK;                                  /* else: past the end, nothing more to read */
    }
    j = (head - 1 + MKV_SCAN_RING) % MKV_SCAN_RING;
    while (n + 1 < count && (n < packets_back || bytes < bytes_back)) {
        j = (j - 1 + MKV_SCAN_RING) % MKV_SCAN_RING;
        n++;
        bytes += ring[j].size;
    }
    restart_at(m, &ring[j]);
    return FA_OK;
}

static const fa_demux_ops mkv_ops = { mkv_next, mkv_seek_packets };

/* The track's end from its last block, scanned for from the last cluster the
 * index knows (the whole file when it is small and has no Cues). Leaves the
 * reader anywhere. */
static int find_last_block(mkv* m, const fa_codec_params* cp, fa_track_params* tp)
{
    mkv_ref r, last, prev;
    int have = 0, have_prev = 0, i;
    int64_t from = m->first_cluster, dur = 0;
    if (!m->br.src->seekable || from < 0) return 0;
    if (m->nindex > 0 && m->index[m->nindex - 1].pos > from) from = m->index[m->nindex - 1].pos;
    else if (m->br.src->length < 0 || m->br.src->length > (16 << 20)) return 0;
    memset(&last, 0, sizeof(last));
    memset(&prev, 0, sizeof(prev));
    m->in_cluster = 0;
    m->nframes = m->frame_i = 0;
    if (fa_br_seek(&m->br, from) != FA_OK) return 0;
    while (step(m, 0, &r) == FA_OK) {
        prev = last;
        have_prev = have;
        last = r;
        have = 1;
    }
    if (!have) return 0;
    restart_at(m, &last);
    if (step(m, 1, NULL) != FA_OK) return 0;
    /* The codec's own count is exact; BlockDuration is rounded (and some muxers
     * already take the padding off it). */
    for (i = 0; i < m->nframes && dur >= 0; i++) {
        uint32_t rate = 0;
        int64_t n = fa_demux_packet_samples(cp->codec, cp->channels, cp->sample_rate,
                                            m->blk + m->lace_off[i], m->lace_size[i], &rate);
        dur = n < 0 || rate == 0 ? -1 : dur + (int64_t)((double)n * 1e9 / rate + 0.5);
    }
    tp->last_exact = dur >= 0;
    tp->last_trim = m->blk_discard;
    if (dur >= 0) dur -= m->blk_discard;
    else if (m->blk_dur >= 0) dur = m->blk_dur;
    else if (have_prev) dur = last.pts - prev.pts;   /* as long as the one before */
    else return 0;
    tp->last_pts = last.pts;
    tp->last_end = last.pts + (dur > 0 ? dur : 0);
    return 1;
}

static void mkv_free(mkv* m)
{
    fa_br_free(&m->br);
    free(m->strip);
    free(m->extra);
    free(m->index);
    free(m->blk);
    free(m->frame);
    free(m);
}

static void parse_info(mkv* m, const uint8_t* p, size_t len)
{
    const uint8_t* cur = p;
    ebml_el e;
    while (mem_next(&cur, p + len, &e)) {
        if (e.id == ID_TIMESTAMPSCALE) {
            uint64_t s = el_uint(&e);
            if (s > 0 && s <= 1000000000ull) m->scale = s;
        } else if (e.id == ID_DURATION) {
            double d = el_float(&e);
            if (d > 0 && d < 1e18) m->duration = d;
        }
    }
}

static fa_result mkv_open(fa_decoder* dec)
{
    fa_source* src = dec->src;
    mkv* m = (mkv*)calloc(1, sizeof(mkv));
    uint32_t id;
    int64_t size, data, cues_pos = -1, tracks_pos = -1;
    int webm = 0, have_tracks = 0, have_cues = 0, count = 0;
    fa_track_params tp;
    uint8_t* body = NULL;
    fa_result r;

    if (!m) return FA_ERROR_OUT_OF_MEMORY;
    r = fa_br_init(&m->br, src);
    if (r != FA_OK) { free(m); return r; }
    m->scale = 1000000;
    m->duration = -1;
    m->first_cluster = -1;
    memset(&tp, 0, sizeof(tp));
    fa_codec_params_init(&tp.codec);

    /* EBML header: the DocType says Matroska or WebM. */
    if (!read_header(m, &id, &size, &data) || id != ID_EBML || size < 0 || size > 4096) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto fail; }
    r = read_body(m, size, &body);
    if (r != FA_OK) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto fail; }
    {
        const uint8_t* cur = body;
        ebml_el e;
        int ok = 0;
        while (mem_next(&cur, body + size, &e))
            if (e.id == ID_DOCTYPE) {
                if (el_is(&e, "webm")) ok = webm = 1;
                else if (el_is(&e, "matroska")) ok = 1;
            }
        free(body);
        body = NULL;
        if (!ok) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto fail; }
    }
    /* The segment (anything else in front of it is skipped). */
    for (;;) {
        if (!read_header(m, &id, &size, &data)) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto fail; }
        if (id == ID_SEGMENT) break;
        if (size < 0 || fa_br_seek(&m->br, data + size) != FA_OK) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto fail; }
    }
    m->seg_start = data;
    m->seg_end = size < 0 ? -1 : data + size;

    /* Header elements up to the first cluster. */
    for (;;) {
        int64_t pos = fa_br_tell(&m->br);
        if (m->seg_end >= 0 && pos >= m->seg_end) break;
        if (!read_header(m, &id, &size, &data)) break;
        if (id == ID_CLUSTER) { m->first_cluster = pos; break; }
        if (size < 0) break;
        if (id == ID_INFO || id == ID_TRACKS || id == ID_SEEKHEAD || id == ID_CUES) {
            r = read_body(m, size, &body);
            if (r == FA_ERROR_OUT_OF_MEMORY) goto fail;
            if (r != FA_OK) break;
            if (id == ID_INFO) parse_info(m, body, (size_t)size);
            else if (id == ID_CUES) { parse_cues(m, body, (size_t)size); have_cues = 1; }
            else if (id == ID_TRACKS) {
                r = parse_tracks(m, body, (size_t)size, dec->audio_track, &count, &tp.codec);
                have_tracks = 1;
                if (r != FA_OK) goto fail;
            } else {
                const uint8_t* cur = body;
                ebml_el s;
                while (mem_next(&cur, body + size, &s)) {
                    const uint8_t* c2 = s.p;
                    ebml_el f;
                    uint32_t sid = 0;
                    int64_t spos = -1;
                    if (s.id != ID_SEEK) continue;
                    while (mem_next(&c2, s.p + s.len, &f)) {
                        if (f.id == ID_SEEKID) { size_t i; sid = 0; for (i = 0; i < f.len && i < 4; i++) sid = (sid << 8) | f.p[i]; }
                        else if (f.id == ID_SEEKPOSITION && el_uint(&f) < (uint64_t)INT64_MAX / 4) spos = m->seg_start + (int64_t)el_uint(&f);
                    }
                    if (sid == ID_CUES) cues_pos = spos;
                    else if (sid == ID_TRACKS) tracks_pos = spos;
                }
            }
            free(body);
            body = NULL;
        } else if (fa_br_seek(&m->br, data + size) != FA_OK) {
            break;
        }
    }
    /* Tracks or Cues stored after the clusters: only reachable by seeking. */
    if (src->seekable && ((!have_tracks && tracks_pos >= 0) || (!have_cues && cues_pos >= 0))) {
        int64_t want[2];
        int i;
        want[0] = have_tracks ? -1 : tracks_pos;
        want[1] = have_cues ? -1 : cues_pos;
        for (i = 0; i < 2; i++) {
            if (want[i] < 0 || (src->length > 0 && want[i] >= src->length)) continue;
            if (fa_br_seek(&m->br, want[i]) != FA_OK || !read_header(m, &id, &size, &data) || size < 0) continue;
            if (id != (i == 0 ? ID_TRACKS : ID_CUES)) continue;
            r = read_body(m, size, &body);
            if (r == FA_ERROR_OUT_OF_MEMORY) goto fail;
            if (r != FA_OK) continue;
            if (i == 0) {
                r = parse_tracks(m, body, (size_t)size, dec->audio_track, &count, &tp.codec);
                have_tracks = 1;
                if (r != FA_OK) goto fail;
            } else {
                parse_cues(m, body, (size_t)size);
            }
            free(body);
            body = NULL;
        }
    }
    dec->track_count = count;
    if (!have_tracks) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto fail; }

    tp.last_pts = FA_DEMUX_NO_PTS;
    if (!find_last_block(m, &tp.codec, &tp)) tp.last_pts = FA_DEMUX_NO_PTS;

    /* Back to the first cluster. */
    m->in_cluster = 0;
    m->nframes = m->frame_i = 0;
    if (m->first_cluster >= 0) {
        r = fa_br_seek(&m->br, m->first_cluster);
        if (r != FA_OK) goto fail;
    }

    tp.time_base = 1e-9;
    tp.start = m->codec_delay;
    tp.end = m->duration > 0 ? (int64_t)(m->duration * (double)m->scale + 0.5) : FA_DEMUX_NO_PTS;
    tp.clip_to_end = 0;           /* timestamps are rounded: the end is only a length */
    tp.precision = (double)m->scale;
    tp.seek_preroll_s = (double)m->seek_preroll / 1e9;
    if (m->first_cluster < 0) m->seg_end = fa_br_tell(&m->br);   /* no clusters: nothing to read */
    r = fa_track_open(&m->track, dec, &mkv_ops, m, &tp);
    if (r != FA_OK) goto fail;
    dec->impl = m;
    dec->seekable = src->seekable;
    snprintf(dec->container, sizeof(dec->container), "%s", webm ? "webm" : "matroska");
    return FA_OK;
fail:
    free(body);
    mkv_free(m);
    return r;
}

static fa_result mkv_read(fa_decoder* dec, float* out, fa_frames frames, fa_frames* got)
{
    return fa_track_read(&((mkv*)dec->impl)->track, out, frames, got);
}

static fa_result mkv_seek(fa_decoder* dec, fa_frames frame)
{
    return fa_track_seek(&((mkv*)dec->impl)->track, frame);
}

static void mkv_close(fa_decoder* dec)
{
    mkv* m = (mkv*)dec->impl;
    if (!m) return;
    fa_track_close(&m->track);
    mkv_free(m);
    dec->impl = NULL;
}

/* EBML magic and a Matroska or WebM DocType in the header. */
static int mkv_probe(const uint8_t* d, size_t n, size_t id3)
{
    const uint8_t* cur;
    ebml_el hdr, e;
    (void)id3;
    if (n < 4 || fa_be32(d) != ID_EBML) return 0;
    cur = d;
    if (!mem_next(&cur, d + n, &hdr)) return 0;
    cur = hdr.p;
    while (mem_next(&cur, hdr.p + hdr.len, &e))
        if (e.id == ID_DOCTYPE && (el_is(&e, "matroska") || el_is(&e, "webm"))) return 100;
    return 0;
}

const fa_decoder_vtable fa_decoder_mkv = {
    "matroska", "Matroska and WebM (audio tracks)", "mkv;mka;webm;weba",
    "video/x-matroska;audio/x-matroska;video/webm;audio/webm",
    mkv_probe, mkv_open, mkv_read, mkv_seek, mkv_close, 1, NULL, NULL
};
