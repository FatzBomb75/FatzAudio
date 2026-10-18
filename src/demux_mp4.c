/* MP4 / M4A / MOV audio tracks, written from ISO/IEC 14496-12 (ISO base media
 * file format), 14496-14 and 14496-3 (esds, AudioSpecificConfig) and Apple's
 * QuickTime File Format.
 *
 * The movie box is read into memory and the chosen track's sample tables are
 * expanded into one packet list (offset, size, time). Only those bytes are read
 * afterwards: a video track's samples are never touched. PCM samples (one frame
 * each in QuickTime) are grouped into packets of up to 4096 frames. Fragmented
 * files (moof/traf/trun) are indexed up front when the source can seek, and read
 * fragment by fragment from forward-only sources. */
#include "demux.h"
#include <stdlib.h>
#include <string.h>

#define FOURCC(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#define MP4_MAX_BOX (1u << 30)        /* moov / moof read into memory */
#define MP4_MAX_PACKET (64u << 20)
#define MP4_PCM_FRAMES 4096

typedef struct mp4_pkt {
    int64_t off;
    int64_t pts;
    uint32_t size;
    uint32_t dur;
} mp4_pkt;

typedef struct mp4 {
    fa_br br;
    fa_track track;
    uint8_t* moov;
    size_t moov_len;
    mp4_pkt* pk;
    size_t npk, pk_cap, cur;
    int64_t end_pts;              /* after the last packet listed */
    int64_t limit;                /* bound for counts read from the file */
    uint32_t track_id, timescale, movie_timescale;
    uint32_t frame_bytes;         /* PCM: bytes per frame (packets are grouped) */
    int fragmented, lazy;
    int64_t next_box;             /* fragments: the next top-level box to look at */
    uint32_t trex_duration, trex_size;
    uint8_t* data;
    size_t data_cap;
    uint8_t* extra;               /* codec extradata built here */
    size_t extra_len;
} mp4;

/* ---- boxes in memory ---------------------------------------------------------------- */

typedef struct box {
    uint32_t type;
    const uint8_t* p;
    size_t len;
} box;

static int next_box(const uint8_t** cur, const uint8_t* end, box* b)
{
    size_t n = (size_t)(end - *cur), hdr = 8;
    uint64_t size;
    if (n < 8) return 0;
    size = fa_be32(*cur);
    b->type = fa_be32(*cur + 4);
    if (size == 1) {
        if (n < 16) return 0;
        size = fa_be64(*cur + 8);
        hdr = 16;
    } else if (size == 0) {
        size = n;
    }
    if (size < hdr) return 0;
    if (size > n) size = n;          /* cut short: keep what is there */
    b->p = *cur + hdr;
    b->len = (size_t)size - hdr;
    *cur += size;
    return 1;
}

static int find_box(const uint8_t* p, size_t len, uint32_t type, box* out)
{
    const uint8_t* cur = p;
    box b;
    while (next_box(&cur, p + len, &b))
        if (b.type == type) { *out = b; return 1; }
    return 0;
}

/* Follows a path of nested boxes, e.g. "mdia", "minf", "stbl". */
static int find_path(const box* in, const uint32_t* path, int depth, box* out)
{
    box b = *in;
    int i;
    for (i = 0; i < depth; i++)
        if (!find_box(b.p, b.len, path[i], &b)) return 0;
    *out = b;
    return 1;
}

/* ---- packet list ---------------------------------------------------------------------- */

static fa_result add_packet(mp4* m, int64_t off, uint32_t size, int64_t pts, uint32_t dur)
{
    if (m->frame_bytes && m->npk > 0) {
        mp4_pkt* last = &m->pk[m->npk - 1];
        /* PCM: one packet for neighbouring frames. */
        if (last->off + last->size == off && last->pts + last->dur == pts &&
            last->size + (uint64_t)size <= (uint64_t)MP4_PCM_FRAMES * m->frame_bytes &&
            (uint64_t)last->dur + dur < 0xFFFFFFFFu) {
            last->size += size;
            last->dur += dur;
            return FA_OK;
        }
    }
    if (m->npk == m->pk_cap) {
        size_t cap = m->pk_cap ? m->pk_cap * 2 : 1024;
        mp4_pkt* p;
        if ((int64_t)m->npk >= m->limit) return FA_ERROR_UNSUPPORTED_FORMAT;
        p = (mp4_pkt*)realloc(m->pk, cap * sizeof(mp4_pkt));
        if (!p) return FA_ERROR_OUT_OF_MEMORY;
        m->pk = p;
        m->pk_cap = cap;
    }
    m->pk[m->npk].off = off;
    m->pk[m->npk].size = size;
    m->pk[m->npk].pts = pts;
    m->pk[m->npk].dur = dur;
    m->npk++;
    return FA_OK;
}

/* Bytes of one PCM sample-table entry: whole frames as the table says, or one
 * frame where old QuickTime files say 1. */
static uint32_t pcm_sample_bytes(const mp4* m, uint32_t listed)
{
    if (listed >= m->frame_bytes && listed % m->frame_bytes == 0) return listed;
    return m->frame_bytes;
}

/* Expands stts / stsc / stsz (stz2) / stco (co64) into packets. */
static fa_result build_packets(mp4* m, const box* stbl)
{
    box stts, stsc, stsz, stco;
    int co64 = 0, z2 = 0;
    uint32_t n_stts, n_stsc, n_chunks, sample_size = 0, field = 32;
    uint64_t total, sample = 0, stts_i = 0, stts_left = 0, stsc_i = 0, chunk;
    uint32_t delta = 0;
    int64_t pts = 0;
    fa_result r;

    if (!find_box(stbl->p, stbl->len, FOURCC('s', 't', 't', 's'), &stts) || stts.len < 8) return FA_ERROR_UNSUPPORTED_FORMAT;
    if (!find_box(stbl->p, stbl->len, FOURCC('s', 't', 's', 'c'), &stsc) || stsc.len < 8) return FA_ERROR_UNSUPPORTED_FORMAT;
    if (!find_box(stbl->p, stbl->len, FOURCC('s', 't', 's', 'z'), &stsz)) {
        if (!find_box(stbl->p, stbl->len, FOURCC('s', 't', 'z', '2'), &stsz)) return FA_ERROR_UNSUPPORTED_FORMAT;
        z2 = 1;
    }
    if (!find_box(stbl->p, stbl->len, FOURCC('s', 't', 'c', 'o'), &stco)) {
        if (!find_box(stbl->p, stbl->len, FOURCC('c', 'o', '6', '4'), &stco)) return FA_ERROR_UNSUPPORTED_FORMAT;
        co64 = 1;
    }
    if (stsz.len < 12 || stco.len < 8) return FA_ERROR_UNSUPPORTED_FORMAT;

    /* Never trust a count beyond what its box holds. */
    n_stts = fa_be32(stts.p + 4);
    if (n_stts > (stts.len - 8) / 8) n_stts = (uint32_t)((stts.len - 8) / 8);
    n_stsc = fa_be32(stsc.p + 4);
    if (n_stsc > (stsc.len - 8) / 12) n_stsc = (uint32_t)((stsc.len - 8) / 12);
    n_chunks = fa_be32(stco.p + 4);
    if (n_chunks > (stco.len - 8) / (co64 ? 8 : 4)) n_chunks = (uint32_t)((stco.len - 8) / (co64 ? 8 : 4));
    total = fa_be32(stsz.p + 8);
    if (z2) {
        field = stsz.p[7];
        if (field != 4 && field != 8 && field != 16) return FA_ERROR_UNSUPPORTED_FORMAT;
        if (total > (uint64_t)(stsz.len - 12) * 8 / field) total = (uint64_t)(stsz.len - 12) * 8 / field;
    } else {
        sample_size = fa_be32(stsz.p + 4);
        if (sample_size == 0 && total > (stsz.len - 12) / 4) total = (stsz.len - 12) / 4;
    }
    if (n_stsc == 0 || n_chunks == 0) return FA_OK;   /* no samples */
    {
        /* Constant-size samples are not listed: bound their count by the file. */
        uint64_t each = sample_size ? sample_size : 1, len = m->br.src->length > 0 ? (uint64_t)m->br.src->length : (uint64_t)1 << 32;
        if (m->frame_bytes) each = pcm_sample_bytes(m, sample_size);
        if (!z2 && sample_size && total > len / each + 1) total = len / each + 1;
    }

    for (chunk = 0; chunk < n_chunks && sample < total; chunk++) {
        int64_t off = co64 ? (int64_t)fa_be64(stco.p + 8 + chunk * 8) : (int64_t)fa_be32(stco.p + 8 + chunk * 4);
        uint32_t spc, s;
        while (stsc_i + 1 < n_stsc && (uint64_t)fa_be32(stsc.p + 8 + (stsc_i + 1) * 12) <= chunk + 1) stsc_i++;
        spc = fa_be32(stsc.p + 8 + stsc_i * 12 + 4);
        if (off < 0) return FA_ERROR_UNSUPPORTED_FORMAT;
        for (s = 0; s < spc && sample < total; s++, sample++) {
            uint32_t size;
            if (sample_size) size = sample_size;
            else if (!z2) size = fa_be32(stsz.p + 12 + sample * 4);
            else if (field == 16) size = fa_be16(stsz.p + 12 + sample * 2);
            else if (field == 8) size = stsz.p[12 + sample];
            else size = (stsz.p[12 + sample / 2] >> ((sample & 1) ? 0 : 4)) & 15;
            if (m->frame_bytes) size = pcm_sample_bytes(m, size);
            while (stts_left == 0 && stts_i < n_stts) {
                stts_left = fa_be32(stts.p + 8 + stts_i * 8);
                delta = fa_be32(stts.p + 8 + stts_i * 8 + 4);
                stts_i++;
            }
            if (stts_left > 0) stts_left--;   /* past the table: keep the last duration */
            r = add_packet(m, off, size, pts, delta);
            if (r != FA_OK) return r;
            off += size;
            pts += delta;
            if (pts > INT64_MAX / 4) return FA_ERROR_UNSUPPORTED_FORMAT;
        }
    }
    m->end_pts = pts;
    return FA_OK;
}

/* Appends the samples of one movie fragment (moof at file offset moof_off). */
static fa_result parse_moof(mp4* m, const uint8_t* p, size_t len, int64_t moof_off)
{
    const uint8_t* cur = p;
    box traf;
    while (next_box(&cur, p + len, &traf)) {
        const uint8_t* c2 = traf.p;
        box b, tfhd;
        uint32_t flags, def_dur = m->trex_duration, def_size = m->trex_size;
        int64_t base = moof_off, next_off;
        int64_t pts = m->end_pts;
        int first = 1;
        size_t o;
        if (traf.type != FOURCC('t', 'r', 'a', 'f')) continue;
        if (!find_box(traf.p, traf.len, FOURCC('t', 'f', 'h', 'd'), &tfhd) || tfhd.len < 8) continue;
        flags = fa_be32(tfhd.p) & 0xFFFFFF;
        if (fa_be32(tfhd.p + 4) != m->track_id) continue;
        o = 8;
        if ((flags & 0x1) && o + 8 <= tfhd.len) { base = (int64_t)fa_be64(tfhd.p + o); o += 8; }
        if (flags & 0x2) o += 4;
        if ((flags & 0x8) && o + 4 <= tfhd.len) { def_dur = fa_be32(tfhd.p + o); o += 4; }
        if ((flags & 0x10) && o + 4 <= tfhd.len) { def_size = fa_be32(tfhd.p + o); }
        if (find_box(traf.p, traf.len, FOURCC('t', 'f', 'd', 't'), &b) && b.len >= 8)
            pts = b.p[0] == 1 && b.len >= 12 ? (int64_t)fa_be64(b.p + 4) : (int64_t)fa_be32(b.p + 4);
        next_off = base;
        while (next_box(&c2, traf.p + traf.len, &b)) {
            uint32_t tf, count, i, rec;
            int64_t off;
            if (b.type != FOURCC('t', 'r', 'u', 'n') || b.len < 8) continue;
            tf = fa_be32(b.p) & 0xFFFFFF;
            count = fa_be32(b.p + 4);
            o = 8;
            off = first ? base : next_off;
            if (tf & 0x1) { if (o + 4 > b.len) break; off = base + (int32_t)fa_be32(b.p + o); o += 4; }
            if (tf & 0x4) o += 4;
            rec = 4u * (((tf >> 8) & 1) + ((tf >> 9) & 1) + ((tf >> 10) & 1) + ((tf >> 11) & 1));
            if (o > b.len) break;
            if (rec > 0 && count > (b.len - o) / rec) count = (uint32_t)((b.len - o) / rec);
            if ((int64_t)count > m->limit) count = (uint32_t)m->limit;
            for (i = 0; i < count; i++) {
                uint32_t dur = def_dur, size = def_size;
                fa_result r;
                if (tf & 0x100) { dur = fa_be32(b.p + o); o += 4; }
                if (tf & 0x200) { size = fa_be32(b.p + o); o += 4; }
                if (tf & 0x400) o += 4;
                if (tf & 0x800) o += 4;
                if (m->frame_bytes) size = pcm_sample_bytes(m, size);
                if (off < 0) return FA_ERROR_UNSUPPORTED_FORMAT;
                r = add_packet(m, off, size, pts, dur);
                if (r != FA_OK) return r;
                off += size;
                pts += dur;
                if (pts > INT64_MAX / 4) return FA_ERROR_UNSUPPORTED_FORMAT;
            }
            next_off = off;
            first = 0;
        }
        m->end_pts = pts;
    }
    return FA_OK;
}

/* ---- top-level boxes in the file -------------------------------------------------- */

/* Reads the header of the box at `pos`: its type, total size (-1 = to the end)
 * and header length. 0 at the end of the data. */
static int read_box_header(mp4* m, int64_t pos, uint32_t* type, int64_t* size, int* hdr)
{
    uint8_t h[16];
    uint64_t s;
    if (fa_br_seek(&m->br, pos) != FA_OK) return 0;
    if (fa_br_read(&m->br, h, 8) < 8) return 0;
    s = fa_be32(h);
    *type = fa_be32(h + 4);
    *hdr = 8;
    if (s == 1) {
        if (fa_br_read(&m->br, h + 8, 8) < 8) return 0;
        s = fa_be64(h + 8);
        *hdr = 16;
    }
    if (s == 0) { *size = -1; return 1; }
    if (s < (uint64_t)*hdr || s > (uint64_t)INT64_MAX / 2) return 0;
    *size = (int64_t)s;
    return 1;
}

/* Reads the next fragment's samples. 1 = some were added, 0 = no more. */
static int load_fragment(mp4* m, fa_result* err)
{
    uint8_t* buf = NULL;
    size_t cap = 0;
    *err = FA_OK;
    while (m->next_box >= 0) {
        uint32_t type;
        int64_t size, pos = m->next_box;
        int hdr;
        size_t before = m->npk;
        if (!read_box_header(m, pos, &type, &size, &hdr)) break;
        m->next_box = size < 0 ? -1 : pos + size;
        if (type != FOURCC('m', 'o', 'o', 'f')) continue;
        if (size < 0 || size - hdr > (int64_t)MP4_MAX_BOX) break;
        *err = fa_br_read_alloc(&m->br, (size_t)(size - hdr), MP4_MAX_BOX, &buf, &cap);
        if (*err != FA_OK) {
            if (*err != FA_ERROR_OUT_OF_MEMORY) *err = FA_OK;   /* a cut file: it ends here */
            break;
        }
        if (m->lazy) m->npk = m->cur = before = 0;       /* forward-only: the past is gone anyway */
        *err = parse_moof(m, buf, (size_t)(size - hdr), pos);
        if (*err != FA_OK) break;
        if (m->lazy && m->next_box >= 0) {
            /* Step over the media data now, while its header is still ahead of us. */
            uint32_t t2;
            int64_t s2;
            int h2;
            if (read_box_header(m, m->next_box, &t2, &s2, &h2) && t2 == FOURCC('m', 'd', 'a', 't'))
                m->next_box = s2 < 0 ? -1 : m->next_box + s2;
        }
        if (m->npk > before) { free(buf); return 1; }
    }
    free(buf);
    return 0;
}

/* ---- sample descriptions ------------------------------------------------------------------ */

/* MPEG-4 descriptor length: up to four bytes of 7 bits. */
static size_t desc_len(const uint8_t* p, size_t n, size_t* used)
{
    size_t v = 0, i;
    for (i = 0; i < 4 && i < n; i++) {
        v = (v << 7) | (p[i] & 0x7F);
        if (!(p[i] & 0x80)) { *used = i + 1; return v; }
    }
    *used = i;
    return v;
}

/* esds (14496-1 ES_Descriptor): object type and DecoderSpecificInfo. */
static int parse_esds(const box* esds, uint8_t* oti, const uint8_t** dsi, size_t* dsi_len)
{
    const uint8_t* p = esds->p + 4;   /* FullBox */
    size_t n = esds->len >= 4 ? esds->len - 4 : 0, used, len;
    *dsi = NULL;
    *dsi_len = 0;
    if (n < 2 || p[0] != 0x03) return 0;
    len = desc_len(p + 1, n - 1, &used);
    p += 1 + used;
    n -= 1 + used;
    if (len < n) n = len;
    if (n < 3) return 0;
    {
        uint8_t fl = p[2];
        size_t skip = 3;
        if (fl & 0x80) skip += 2;
        if (fl & 0x40) skip += 1 + (n > 3 ? p[3] : 0);
        if (fl & 0x20) skip += 2;
        if (skip >= n) return 0;
        p += skip;
        n -= skip;
    }
    if (p[0] != 0x04 || n < 2) return 0;
    len = desc_len(p + 1, n - 1, &used);
    p += 1 + used;
    n -= 1 + used;
    if (len < n) n = len;
    if (n < 13) return 0;
    *oti = p[0];
    p += 13;
    n -= 13;
    if (n >= 2 && p[0] == 0x05) {
        len = desc_len(p + 1, n - 1, &used);
        if (1 + used <= n) {
            p += 1 + used;
            n -= 1 + used;
            *dsi = p;
            *dsi_len = len < n ? len : n;
        }
    }
    return 1;
}

static int plausible_box(const uint8_t* p, size_t n)
{
    uint32_t s;
    int i;
    if (n < 8) return n == 0;
    s = fa_be32(p);
    if (s < 8 || s > n) return 0;
    for (i = 4; i < 8; i++)
        if (p[i] < 0x20 || p[i] > 0x7E) return 0;
    return 1;
}

static int set_extra(mp4* m, const uint8_t* p, size_t n)
{
    free(m->extra);
    m->extra = (uint8_t*)malloc(n ? n : 1);
    if (!m->extra) return 0;
    memcpy(m->extra, p, n);
    m->extra_len = n;
    return 1;
}

/* The first sample description of the track: codec name, rate, channels, extradata. */
static fa_result parse_stsd(mp4* m, const box* stbl, fa_codec_params* cp)
{
    box stsd, wave, b;
    const uint8_t* e;
    const uint8_t* kids;
    size_t elen, klen;
    uint32_t fourcc, version, channels, bits, rate;
    int have_wave, little = -1, qt_float = 0, qt_big = 1;
    const char* name = NULL;

    if (!find_box(stbl->p, stbl->len, FOURCC('s', 't', 's', 'd'), &stsd) || stsd.len < 16) return FA_ERROR_UNSUPPORTED_FORMAT;
    {
        const uint8_t* cur = stsd.p + 8;
        box entry;
        if (!next_box(&cur, stsd.p + stsd.len, &entry)) return FA_ERROR_UNSUPPORTED_FORMAT;
        fourcc = entry.type;
        e = entry.p;
        elen = entry.len;
    }
    if (elen < 28) return FA_ERROR_UNSUPPORTED_FORMAT;
    version = fa_be16(e + 8);
    channels = fa_be16(e + 16);
    bits = fa_be16(e + 18);
    rate = fa_be32(e + 24) >> 16;
    kids = e + 28;
    klen = elen - 28;
    /* QuickTime sound descriptions 1 and 2 add fields before the child boxes (ISO
     * files keep version 0, or use version 1 without extra fields). */
    if (version == 1 && elen >= 44 && !plausible_box(kids, klen)) {
        kids = e + 44;
        klen = elen - 44;
    } else if (version == 2 && elen >= 64) {
        union { uint64_t u; double d; } r64;
        uint32_t lpcm_flags;
        r64.u = fa_be64(e + 32);
        if (r64.d > 0 && r64.d < 1e7) rate = (uint32_t)(r64.d + 0.5);
        channels = fa_be32(e + 40);
        bits = fa_be32(e + 48);
        lpcm_flags = fa_be32(e + 52);
        qt_float = (lpcm_flags & 1) != 0;
        qt_big = (lpcm_flags & 2) != 0;
        if (fourcc == FOURCC('l', 'p', 'c', 'm') && !(lpcm_flags & 4) && !qt_float && bits == 8) bits = 0x108; /* unsigned 8 */
        kids = e + 64;
        klen = elen - 64;
    }
    have_wave = find_box(kids, klen, FOURCC('w', 'a', 'v', 'e'), &wave);
    if (find_box(kids, klen, FOURCC('e', 'n', 'd', 'a'), &b) || (have_wave && find_box(wave.p, wave.len, FOURCC('e', 'n', 'd', 'a'), &b)))
        if (b.len >= 2) little = fa_be16(b.p) != 0;
    if (rate == 0) rate = m->timescale;

    /* Common Encryption ('enca' with the real format in 'sinf') and iTunes
     * FairPlay ('drms') need a licence this library does not have. */
    if (fourcc == FOURCC('e', 'n', 'c', 'a') || fourcc == FOURCC('d', 'r', 'm', 's')) return FA_ERROR_PROTECTED_CONTENT;

    switch (fourcc) {
    case FOURCC('m', 'p', '4', 'a'): {
        uint8_t oti = 0;
        const uint8_t* dsi;
        size_t dsi_len;
        if (!find_box(kids, klen, FOURCC('e', 's', 'd', 's'), &b) && !(have_wave && find_box(wave.p, wave.len, FOURCC('e', 's', 'd', 's'), &b)))
            return FA_ERROR_UNSUPPORTED_FORMAT;
        if (!parse_esds(&b, &oti, &dsi, &dsi_len)) return FA_ERROR_UNSUPPORTED_FORMAT;
        switch (oti) {
        case 0x40: case 0x66: case 0x67: case 0x68: name = "aac"; break;
        case 0x69: case 0x6B: name = "mp3"; break;
        case 0xA5: name = "ac3"; break;
        case 0xA6: name = "eac3"; break;
        case 0xA9: name = "dts"; break;
        case 0xDD: name = "vorbis"; break;
        case 0xAD: name = "opus"; break;
        default: return FA_ERROR_UNSUPPORTED_FORMAT;
        }
        if (dsi && dsi_len > 0 && !set_extra(m, dsi, dsi_len)) return FA_ERROR_OUT_OF_MEMORY;
        if (strcmp(name, "aac") == 0 && !m->extra) {
            uint8_t asc[FA_AAC_ASC_MAX];
            if (!set_extra(m, asc, fa_aac_build_asc(asc, 2, rate, channels, 0))) return FA_ERROR_OUT_OF_MEMORY;
        }
        break;
    }
    case FOURCC('.', 'm', 'p', '3'): case FOURCC('m', 's', 0, 0x55): name = "mp3"; break;
    case FOURCC('a', 'c', '-', '3'): case FOURCC('s', 'a', 'c', '3'): name = "ac3"; break;
    case FOURCC('e', 'c', '-', '3'): name = "eac3"; break;
    case FOURCC('d', 't', 's', 'c'): case FOURCC('d', 't', 's', 'h'): case FOURCC('d', 't', 's', 'l'):
    case FOURCC('d', 't', 's', 'e'): name = "dts"; break;
    case FOURCC('m', 'l', 'p', 'a'): name = "truehd"; break;
    case FOURCC('s', 'a', 'm', 'r'): name = "amrnb"; break;
    case FOURCC('s', 'a', 'w', 'b'): name = "amrwb"; break;
    case FOURCC('u', 'l', 'a', 'w'): name = "pcm_mulaw"; m->frame_bytes = channels; break;
    case FOURCC('a', 'l', 'a', 'w'): name = "pcm_alaw"; m->frame_bytes = channels; break;
    case FOURCC('O', 'p', 'u', 's'): {
        /* dOps (Opus in ISOBMFF) -> the OpusHead of RFC 7845, little endian. */
        uint8_t head[19 + 2 + 255];
        size_t n = 19;
        if (!find_box(kids, klen, FOURCC('d', 'O', 'p', 's'), &b) || b.len < 11) return FA_ERROR_UNSUPPORTED_FORMAT;
        memcpy(head, "OpusHead", 8);
        head[8] = 1;
        head[9] = b.p[1];
        head[10] = b.p[3]; head[11] = b.p[2];                         /* pre-skip */
        head[12] = b.p[7]; head[13] = b.p[6]; head[14] = b.p[5]; head[15] = b.p[4];
        head[16] = b.p[9]; head[17] = b.p[8];                         /* output gain */
        head[18] = b.p[10];                                           /* mapping family */
        if (b.p[10] != 0) {
            size_t k = 2 + (size_t)b.p[1];
            if (b.len < 11 + k) return FA_ERROR_UNSUPPORTED_FORMAT;
            memcpy(head + 19, b.p + 11, k);
            n += k;
        }
        if (!set_extra(m, head, n)) return FA_ERROR_OUT_OF_MEMORY;
        name = "opus";
        channels = b.p[1];
        rate = 48000;
        break;
    }
    case FOURCC('f', 'L', 'a', 'C'):
        /* dfLa: FullBox, then FLAC metadata blocks; the decoder wants STREAMINFO. */
        if (!find_box(kids, klen, FOURCC('d', 'f', 'L', 'a'), &b) || b.len < 8 + 34 || (b.p[4] & 0x7F) != 0)
            return FA_ERROR_UNSUPPORTED_FORMAT;
        if (!set_extra(m, b.p + 8, 34)) return FA_ERROR_OUT_OF_MEMORY;
        name = "flac";
        break;
    case FOURCC('a', 'l', 'a', 'c'): {
        /* The decoder wants the whole 'alac' child box, header included. */
        box a;
        if (!find_box(kids, klen, FOURCC('a', 'l', 'a', 'c'), &a)) {
            if (!have_wave || !find_box(wave.p, wave.len, FOURCC('a', 'l', 'a', 'c'), &a)) return FA_ERROR_UNSUPPORTED_FORMAT;
        }
        if (a.len < 28) return FA_ERROR_UNSUPPORTED_FORMAT;
        if (!set_extra(m, a.p - 8, a.len + 8)) return FA_ERROR_OUT_OF_MEMORY;
        name = "alac";
        break;
    }
    case FOURCC('r', 'a', 'w', ' '): name = "pcm_u8"; bits = 8; break;
    case FOURCC('t', 'w', 'o', 's'):
        name = bits == 8 ? "pcm_s8" : bits == 24 ? (little == 1 ? "pcm_s24le" : "pcm_s24be")
             : bits == 32 ? (little == 1 ? "pcm_s32le" : "pcm_s32be") : (little == 1 ? "pcm_s16le" : "pcm_s16be");
        break;
    case FOURCC('s', 'o', 'w', 't'):
        name = bits == 8 ? "pcm_s8" : bits == 24 ? "pcm_s24le" : bits == 32 ? "pcm_s32le" : "pcm_s16le";
        break;
    case FOURCC('i', 'n', '2', '4'): name = little == 1 ? "pcm_s24le" : "pcm_s24be"; bits = 24; break;
    case FOURCC('i', 'n', '3', '2'): name = little == 1 ? "pcm_s32le" : "pcm_s32be"; bits = 32; break;
    case FOURCC('f', 'l', '3', '2'): name = little == 1 ? "pcm_f32le" : "pcm_f32be"; bits = 32; break;
    case FOURCC('f', 'l', '6', '4'): name = little == 1 ? "pcm_f64le" : "pcm_f64be"; bits = 64; break;
    case FOURCC('l', 'p', 'c', 'm'):
        if (bits == 0x108) name = "pcm_u8", bits = 8;
        else if (qt_float) name = bits == 64 ? (qt_big ? "pcm_f64be" : "pcm_f64le") : (qt_big ? "pcm_f32be" : "pcm_f32le");
        else if (bits == 8) name = "pcm_s8";
        else if (bits == 16) name = qt_big ? "pcm_s16be" : "pcm_s16le";
        else if (bits == 24) name = qt_big ? "pcm_s24be" : "pcm_s24le";
        else if (bits == 32) name = qt_big ? "pcm_s32be" : "pcm_s32le";
        else return FA_ERROR_UNSUPPORTED_FORMAT;
        break;
    case FOURCC('i', 'p', 'c', 'm'): case FOURCC('f', 'p', 'c', 'm'): {
        /* ISO/IEC 23003-5: pcmC holds the byte order and the sample size. */
        int le, fl = fourcc == FOURCC('f', 'p', 'c', 'm');
        if (!find_box(kids, klen, FOURCC('p', 'c', 'm', 'C'), &b) || b.len < 6) return FA_ERROR_UNSUPPORTED_FORMAT;
        le = b.p[4] & 1;
        bits = b.p[5];
        if (fl) name = bits == 64 ? (le ? "pcm_f64le" : "pcm_f64be") : bits == 32 ? (le ? "pcm_f32le" : "pcm_f32be") : NULL;
        else name = bits == 16 ? (le ? "pcm_s16le" : "pcm_s16be") : bits == 24 ? (le ? "pcm_s24le" : "pcm_s24be")
                  : bits == 32 ? (le ? "pcm_s32le" : "pcm_s32be") : NULL;
        if (!name) return FA_ERROR_UNSUPPORTED_FORMAT;
        break;
    }
    default:
        return FA_ERROR_UNSUPPORTED_FORMAT;
    }
    if (strncmp(name, "pcm_", 4) == 0 && !m->frame_bytes) {
        if (channels == 0 || bits == 0 || bits % 8) return FA_ERROR_UNSUPPORTED_FORMAT;
        m->frame_bytes = channels * (bits / 8);
    }
    cp->codec = name;
    cp->sample_rate = rate;
    cp->channels = channels;
    cp->extradata = m->extra;
    cp->extradata_size = m->extra_len;
    if (m->frame_bytes) cp->block_align = m->frame_bytes;
    return FA_OK;
}

/* ---- the movie ---------------------------------------------------------------------- */

static int is_audio(const box* trak)
{
    static const uint32_t path[2] = { FOURCC('m', 'd', 'i', 'a'), FOURCC('h', 'd', 'l', 'r') };
    box h;
    return find_path(trak, path, 2, &h) && h.len >= 12 && fa_be32(h.p + 8) == FOURCC('s', 'o', 'u', 'n');
}

static int track_enabled(const box* trak)
{
    box t;
    return find_box(trak->p, trak->len, FOURCC('t', 'k', 'h', 'd'), &t) && t.len >= 4 && (fa_be32(t.p) & 1);
}

static uint32_t track_id(const box* trak)
{
    box t;
    if (!find_box(trak->p, trak->len, FOURCC('t', 'k', 'h', 'd'), &t) || t.len < 24) return 0;
    return fa_be32(t.p + (t.p[0] == 1 ? 20 : 12));
}

/* Picks audio track `want` (-1 = the first enabled one) and counts them. */
static int pick_track(const box* moov, int32_t want, box* out)
{
    const uint8_t* cur = moov->p;
    box b;
    int count = 0, first = -1, enabled = -1;
    box first_b, enabled_b;
    memset(&first_b, 0, sizeof(first_b));
    memset(&enabled_b, 0, sizeof(enabled_b));
    while (next_box(&cur, moov->p + moov->len, &b)) {
        if (b.type != FOURCC('t', 'r', 'a', 'k') || !is_audio(&b)) continue;
        if (count == want) *out = b;
        if (first < 0) { first = count; first_b = b; }
        if (enabled < 0 && track_enabled(&b)) { enabled = count; enabled_b = b; }
        count++;
    }
    if (want < 0 && count > 0) *out = enabled >= 0 ? enabled_b : first_b;
    return count;
}

static void mp4_free(mp4* m)
{
    fa_br_free(&m->br);
    free(m->moov);
    free(m->pk);
    free(m->data);
    free(m->extra);
    free(m);
}

static fa_result mp4_next(void* ctx, fa_demux_packet* out)
{
    mp4* m = (mp4*)ctx;
    const mp4_pkt* p;
    fa_result r;
    if (m->cur >= m->npk) {
        if (!m->lazy) return FA_ERROR_END_OF_STREAM;
        if (!load_fragment(m, &r)) return r != FA_OK ? r : FA_ERROR_END_OF_STREAM;
    }
    p = &m->pk[m->cur];
    r = fa_br_seek(&m->br, p->off);
    if (r == FA_ERROR_NOT_SEEKABLE) return r;          /* samples out of file order */
    if (r != FA_OK) return FA_ERROR_END_OF_STREAM;    /* past the end of a cut file */
    r = fa_br_read_alloc(&m->br, p->size, MP4_MAX_PACKET, &m->data, &m->data_cap);
    if (r != FA_OK) return r == FA_ERROR_OUT_OF_MEMORY ? r : FA_ERROR_END_OF_STREAM;
    out->data = m->data;
    out->size = p->size;
    out->pts = p->pts;
    out->duration = p->dur;
    m->cur++;
    return FA_OK;
}

static fa_result mp4_seek_packets(void* ctx, int64_t t, int packets_back, size_t bytes_back)
{
    mp4* m = (mp4*)ctx;
    size_t lo = 0, hi = m->npk, j;
    int n = 0;
    size_t bytes = 0;
    if (m->lazy) return FA_ERROR_NOT_SEEKABLE;
    /* Last packet starting at or before t. */
    while (hi - lo > 1) {
        size_t mid = lo + (hi - lo) / 2;
        if (m->pk[mid].pts <= t) lo = mid;
        else hi = mid;
    }
    j = lo;
    while (j > 0 && n < 64 && (n < packets_back || bytes < bytes_back)) {
        j--;
        n++;
        bytes += m->pk[j].size;
    }
    m->cur = j;
    return FA_OK;
}

static const fa_demux_ops mp4_ops = { mp4_next, mp4_seek_packets };

static fa_result mp4_open(fa_decoder* dec)
{
    fa_source* src = dec->src;
    mp4* m = (mp4*)calloc(1, sizeof(mp4));
    fa_track_params tp;
    int64_t pos = 0;
    int qt = 1, have_moov = 0, count;
    box moov, trak, b;
    fa_result r;
    static const uint32_t mdhd_path[2] = { FOURCC('m', 'd', 'i', 'a'), FOURCC('m', 'd', 'h', 'd') };
    static const uint32_t stbl_path[3] = { FOURCC('m', 'd', 'i', 'a'), FOURCC('m', 'i', 'n', 'f'), FOURCC('s', 't', 'b', 'l') };

    if (!m) return FA_ERROR_OUT_OF_MEMORY;
    r = fa_br_init(&m->br, src);
    if (r != FA_OK) { free(m); return r; }
    /* Counts read from the file never exceed what the file could hold (packets of
     * at least 4 bytes), nor 32 M packets. */
    m->limit = src->length > 0 ? src->length / 4 + 1024 : (int64_t)1 << 22;
    if (m->limit > ((int64_t)1 << 25)) m->limit = (int64_t)1 << 25;
    m->next_box = -1;

    /* Top level: find the movie box. */
    for (;;) {
        uint32_t type;
        int64_t size;
        int hdr;
        if (!read_box_header(m, pos, &type, &size, &hdr)) break;
        if (pos == 0 && type == FOURCC('f', 't', 'y', 'p')) {
            uint8_t brand[4];
            if (fa_br_read(&m->br, brand, 4) == 4) qt = fa_be32(brand) == FOURCC('q', 't', ' ', ' ');
        }
        if (type == FOURCC('m', 'o', 'o', 'v')) {
            int64_t len = size < 0 ? (src->length > 0 ? src->length - pos - hdr : 0) : size - hdr;
            if (len <= 0 || len > (int64_t)MP4_MAX_BOX || (src->length > 0 && len > src->length)) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto fail; }
            m->moov = (uint8_t*)malloc((size_t)len);
            if (!m->moov) { r = FA_ERROR_OUT_OF_MEMORY; goto fail; }
            m->moov_len = fa_br_read(&m->br, m->moov, (size_t)len);   /* a cut file keeps what is there */
            have_moov = 1;
            m->next_box = size < 0 ? -1 : pos + size;
            break;
        }
        if (type == FOURCC('m', 'd', 'a', 't') && !src->seekable) { r = FA_ERROR_NOT_SEEKABLE; goto fail; }
        if (size < 0) break;
        pos += size;
    }
    if (!have_moov) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto fail; }
    moov.type = FOURCC('m', 'o', 'o', 'v');
    moov.p = m->moov;
    moov.len = m->moov_len;

    if (find_box(moov.p, moov.len, FOURCC('m', 'v', 'h', 'd'), &b) && b.len >= 24)
        m->movie_timescale = fa_be32(b.p + (b.p[0] == 1 ? 20 : 12));
    count = pick_track(&moov, dec->audio_track, &trak);
    dec->track_count = count;
    if (count == 0 || (dec->audio_track >= 0 && dec->audio_track >= count)) {
        r = count == 0 ? FA_ERROR_UNSUPPORTED_FORMAT : FA_ERROR_NOT_FOUND;
        goto fail;
    }
    m->track_id = track_id(&trak);
    if (!find_path(&trak, mdhd_path, 2, &b) || b.len < 24) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto fail; }
    m->timescale = fa_be32(b.p + (b.p[0] == 1 ? 20 : 12));
    if (m->timescale == 0) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto fail; }

    {
        box stbl;
        if (!find_path(&trak, stbl_path, 3, &stbl)) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto fail; }
        memset(&tp, 0, sizeof(tp));
        fa_codec_params_init(&tp.codec);
        r = parse_stsd(m, &stbl, &tp.codec);
        if (r != FA_OK) goto fail;
        r = build_packets(m, &stbl);
        if (r != FA_OK) goto fail;
    }

    /* Fragments: trex holds the track's sample defaults. */
    if (find_box(moov.p, moov.len, FOURCC('m', 'v', 'e', 'x'), &b)) {
        const uint8_t* cur = b.p;
        box x;
        m->fragmented = 1;
        while (next_box(&cur, b.p + b.len, &x))
            if (x.type == FOURCC('t', 'r', 'e', 'x') && x.len >= 20 && fa_be32(x.p + 4) == m->track_id) {
                m->trex_duration = fa_be32(x.p + 12);
                m->trex_size = fa_be32(x.p + 16);
            }
        if (src->seekable) {
            while (load_fragment(m, &r)) {}
            if (r != FA_OK) goto fail;
        } else {
            m->lazy = 1;
        }
    }

    /* Edit list: initial empty edits are silence, the first real edit's media
     * time is where playback starts (encoder priming) and its duration where it
     * ends. Later edits are not followed. */
    tp.time_base = 1.0 / m->timescale;
    tp.start = 0;
    tp.end = m->lazy ? FA_DEMUX_NO_PTS : m->end_pts;
    {
        static const uint32_t elst_path[2] = { FOURCC('e', 'd', 't', 's'), FOURCC('e', 'l', 's', 't') };
        box e;
        if (find_path(&trak, elst_path, 2, &e) && e.len >= 8 && m->movie_timescale > 0) {
            int v1 = e.p[0] == 1;
            size_t esz = v1 ? 20 : 12, i, n = fa_be32(e.p + 4);
            double lead = 0;
            if (n > (e.len - 8) / esz) n = (e.len - 8) / esz;
            for (i = 0; i < n; i++) {
                const uint8_t* q = e.p + 8 + i * esz;
                uint64_t seg = v1 ? fa_be64(q) : fa_be32(q);
                int64_t mt = v1 ? (int64_t)fa_be64(q + 8) : (int64_t)(int32_t)fa_be32(q + 4);
                if (mt == -1) { lead += (double)seg / m->movie_timescale; continue; }
                tp.start = mt;
                tp.lead_s = lead;
                if (n - i == 1 && seg > 0) {
                    int64_t end = mt + (int64_t)((double)seg * m->timescale / m->movie_timescale + 0.5);
                    if (tp.end == FA_DEMUX_NO_PTS || end < tp.end) tp.end = end;
                }
                break;
            }
        }
    }
    tp.clip_to_end = tp.end != FA_DEMUX_NO_PTS;
    tp.exact_durations = 1;
    tp.last_pts = FA_DEMUX_NO_PTS;

    r = fa_track_open(&m->track, dec, &mp4_ops, m, &tp);
    if (r != FA_OK) goto fail;
    dec->impl = m;
    dec->seekable = src->seekable && !m->lazy;
    if (!m->lazy) {
        /* The track's own samples: exact even with video and other tracks in the file. */
        size_t i;
        int64_t total = 0;
        for (i = 0; i < m->npk; i++) total += m->pk[i].size;
        dec->audio_bytes = total;
    }
    snprintf(dec->container, sizeof(dec->container), "%s", qt ? "mov" : "mp4");
    return FA_OK;
fail:
    mp4_free(m);
    return r;
}

static fa_result mp4_read(fa_decoder* dec, float* out, fa_frames frames, fa_frames* got)
{
    return fa_track_read(&((mp4*)dec->impl)->track, out, frames, got);
}

static fa_result mp4_seek(fa_decoder* dec, fa_frames frame)
{
    return fa_track_seek(&((mp4*)dec->impl)->track, frame);
}

static void mp4_close(fa_decoder* dec)
{
    mp4* m = (mp4*)dec->impl;
    if (!m) return;
    fa_track_close(&m->track);
    mp4_free(m);
    dec->impl = NULL;
}

/* An ftyp box first, or one of the boxes old QuickTime files start with. */
static int mp4_probe(const uint8_t* d, size_t n, size_t id3)
{
    uint32_t size, type;
    (void)id3;
    if (n < 8) return 0;
    size = fa_be32(d);
    type = fa_be32(d + 4);
    if (type == FOURCC('f', 't', 'y', 'p')) return size >= 8 ? 100 : 0;
    if ((type == FOURCC('m', 'o', 'o', 'v') || type == FOURCC('m', 'd', 'a', 't') || type == FOURCC('f', 'r', 'e', 'e') ||
         type == FOURCC('w', 'i', 'd', 'e') || type == FOURCC('s', 'k', 'i', 'p')) && (size == 0 || size == 1 || size >= 8))
        return 100;
    return 0;
}

const fa_decoder_vtable fa_decoder_mp4 = {
    "mp4", "MP4, M4A, MOV (audio tracks)", "mp4;m4a;m4b;m4v;mov;3gp",
    "audio/mp4;video/mp4;audio/x-m4a;video/quicktime",
    mp4_probe, mp4_open, mp4_read, mp4_seek, mp4_close, 1, NULL, NULL
};
