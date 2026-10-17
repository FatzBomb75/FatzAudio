/* ASF (Advanced Systems Format): .wma, .asf and .wmv files, audio streams only.
 * Written from the Advanced Systems Format Specification, revision 1.20.
 *
 * A file is a Header Object (stream descriptions, tags), a Data Object of
 * fixed-size data packets, and optional index objects after it. Every packet
 * carries payloads; a payload is a fragment of one media object of one stream
 * (or, "compressed", several whole small objects). Each media object of the
 * chosen audio stream is reassembled here and handed to the track decoder
 * (demux.c) as one packet, at its presentation time in milliseconds. Times in
 * the file are offset by the preroll; the track starts there.
 *
 * Codecs: Windows Media Audio 1, 2, 9 Professional, 9 Lossless and Voice, plus
 * the PCM, MP3 and AC-3 that some ASF files carry. The codec decides by name
 * (codec.c): Media Foundation's decoder on Windows, else the app's FFmpeg.
 *
 * Seeking needs a seekable source. The Simple Index Object (when the file has one
 * and is local) gives a packet at or before the target; a search over the packets'
 * send times (interpolation, then halving) narrows it; reading on from there finds
 * the media object. Forward-only sources and broadcast streams play straight
 * through. DRM-protected content (Content Encryption objects, encrypted streams)
 * is refused with FA_ERROR_PROTECTED_CONTENT: it cannot be decoded without the
 * license, which only Windows' own DRM components hold. */
#include "demux.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ASF_MAX_HEADER (64u << 20)      /* header objects (cover art in tags) */
#define ASF_MAX_PACKET (1u << 20)
#define ASF_MAX_OBJECT (16u << 20)
#define ASF_MAX_STREAMS 128
#define ASF_SCAN_RING 64
#define ASF_MAX_INDEX (1u << 24)

typedef uint8_t asf_guid[16];

/* GUIDs as stored: the first three fields little-endian. */
#define G(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) { \
    (uint8_t)(a), (uint8_t)((a) >> 8), (uint8_t)((a) >> 16), (uint8_t)((uint32_t)(a) >> 24), \
    (uint8_t)(b), (uint8_t)((b) >> 8), (uint8_t)(c), (uint8_t)((c) >> 8), d0, d1, d2, d3, d4, d5, d6, d7 }

static const asf_guid g_header = G(0x75B22630, 0x668E, 0x11CF, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C);
static const asf_guid g_data = G(0x75B22636, 0x668E, 0x11CF, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C);
static const asf_guid g_simple_index = G(0x33000890, 0xE5B1, 0x11CF, 0x89, 0xF4, 0x00, 0xA0, 0xC9, 0x03, 0x49, 0xCB);
static const asf_guid g_file_props = G(0x8CABDCA1, 0xA947, 0x11CF, 0x8E, 0xE4, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65);
static const asf_guid g_stream_props = G(0xB7DC0791, 0xA9B7, 0x11CF, 0x8E, 0xE6, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65);
static const asf_guid g_header_ext = G(0x5FBF03B5, 0xA92E, 0x11CF, 0x8E, 0xE3, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65);
static const asf_guid g_content_desc = G(0x75B22633, 0x668E, 0x11CF, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C);
static const asf_guid g_ext_content_desc = G(0xD2D0A440, 0xE307, 0x11D2, 0x97, 0xF0, 0x00, 0xA0, 0xC9, 0x5E, 0xA8, 0x50);
static const asf_guid g_content_encryption = G(0x2211B3FB, 0xBD23, 0x11D2, 0xB4, 0xB7, 0x00, 0xA0, 0xC9, 0x55, 0xFC, 0x6E);
static const asf_guid g_ext_content_encryption = G(0x298AE614, 0x2622, 0x4C17, 0xB9, 0x35, 0xDA, 0xE0, 0x7E, 0xE9, 0x28, 0x9C);
static const asf_guid g_adv_content_encryption = G(0x43058533, 0x6981, 0x49E6, 0x9B, 0x74, 0xAD, 0x12, 0xCB, 0x86, 0xD5, 0x8C);
static const asf_guid g_metadata = G(0xC5F8CBEA, 0x5BAF, 0x4877, 0x84, 0x67, 0xAA, 0x8C, 0x44, 0xFA, 0x4C, 0xCA);
static const asf_guid g_metadata_library = G(0x44231C94, 0x9498, 0x49D1, 0xA1, 0x41, 0x1D, 0x13, 0x4E, 0x45, 0x70, 0x54);
static const asf_guid g_ext_stream_props = G(0x14E6A5CB, 0xC672, 0x4332, 0x83, 0x99, 0xA9, 0x69, 0x52, 0x06, 0x5B, 0x5A);
static const asf_guid g_audio_media = G(0xF8699E40, 0x5B4D, 0x11CF, 0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B);
static const asf_guid g_audio_spread = G(0xBFC3CD50, 0x618F, 0x11CF, 0x8B, 0xB2, 0x00, 0xAA, 0x00, 0xB4, 0xE2, 0x20);

static int guid_is(const uint8_t* p, const asf_guid g) { return memcmp(p, g, 16) == 0; }
static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t le64(const uint8_t* p) { return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32); }

/* The next child object in [*cur, end): its GUID, body and body size. 0 at the
 * end or when the size does not fit. */
static int next_object(const uint8_t** cur, const uint8_t* end, const uint8_t** guid, const uint8_t** body, size_t* len)
{
    uint64_t size;
    if ((size_t)(end - *cur) < 24) return 0;
    size = le64(*cur + 16);
    if (size < 24 || size > (uint64_t)(end - *cur)) return 0;
    *guid = *cur;
    *body = *cur + 24;
    *len = (size_t)size - 24;
    *cur += (size_t)size;
    return 1;
}

/* ---- text ----------------------------------------------------------------------------- */

/* UTF-16LE (`n` bytes, up to a NUL) to UTF-8, truncated to fit `cap` on a whole character. */
static void utf16_to_utf8(const uint8_t* p, size_t n, char* out, size_t cap)
{
    size_t i = 0, k = 0;
    if (cap == 0) return;
    while (i + 1 < n) {
        uint32_t u = le16(p + i);
        uint8_t b[4];
        size_t m;
        i += 2;
        if (u == 0) break;
        if (u >= 0xD800 && u < 0xDC00 && i + 1 < n && le16(p + i) >= 0xDC00 && le16(p + i) < 0xE000) {
            u = 0x10000 + ((u - 0xD800) << 10) + (le16(p + i) - 0xDC00u);
            i += 2;
        } else if (u >= 0xD800 && u < 0xE000) {
            u = 0xFFFD;
        }
        if (u < 0x80) { b[0] = (uint8_t)u; m = 1; }
        else if (u < 0x800) { b[0] = (uint8_t)(0xC0 | (u >> 6)); b[1] = (uint8_t)(0x80 | (u & 63)); m = 2; }
        else if (u < 0x10000) {
            b[0] = (uint8_t)(0xE0 | (u >> 12)); b[1] = (uint8_t)(0x80 | ((u >> 6) & 63)); b[2] = (uint8_t)(0x80 | (u & 63)); m = 3;
        } else {
            b[0] = (uint8_t)(0xF0 | (u >> 18)); b[1] = (uint8_t)(0x80 | ((u >> 12) & 63));
            b[2] = (uint8_t)(0x80 | ((u >> 6) & 63)); b[3] = (uint8_t)(0x80 | (u & 63)); m = 4;
        }
        if (k + m + 1 > cap) break;
        memcpy(out + k, b, m);
        k += m;
    }
    out[k] = '\0';
}

/* ---- tags --------------------------------------------------------------------------------- */

static void tag_item(fa_asf_tags* t, const uint8_t* name, size_t name_len, const uint8_t* value, size_t value_len)
{
    char key[128], text[512];
    utf16_to_utf8(name, name_len, key, sizeof(key));
    utf16_to_utf8(value, value_len, text, sizeof(text));
    fa_tags_handle(key, text, &t->loudness);
    if (!t->title[0] && fa_tags_key_is(key, "Title")) utf16_to_utf8(value, value_len, t->title, sizeof(t->title));
    if (!t->artist[0] && (fa_tags_key_is(key, "Author") || fa_tags_key_is(key, "WM/AlbumArtist")))
        utf16_to_utf8(value, value_len, t->artist, sizeof(t->artist));
}

/* Content Description: title, author, copyright, description, rating. */
static void parse_content_desc(const uint8_t* p, size_t n, fa_asf_tags* t)
{
    size_t len[5], pos = 10, i;
    if (n < 10) return;
    for (i = 0; i < 5; i++) len[i] = le16(p + 2 * i);
    if (len[0] > 0 && len[0] <= n - pos) utf16_to_utf8(p + pos, len[0], t->title, sizeof(t->title));
    pos += len[0];
    if (len[1] > 0 && pos <= n && len[1] <= n - pos) utf16_to_utf8(p + pos, len[1], t->artist, sizeof(t->artist));
}

/* Extended Content Description: name / type / value items; strings are type 0. */
static void parse_ext_content_desc(const uint8_t* p, size_t n, fa_asf_tags* t)
{
    size_t pos = 2;
    uint32_t count, i;
    if (n < 2) return;
    count = le16(p);
    for (i = 0; i < count; i++) {
        size_t name_len, value_len, type;
        const uint8_t* name;
        if (n - pos < 2) return;
        name_len = le16(p + pos);
        pos += 2;
        if (n - pos < name_len + 4) return;
        name = p + pos;
        pos += name_len;
        type = le16(p + pos);
        value_len = le16(p + pos + 2);
        pos += 4;
        if (n - pos < value_len) return;
        if (type == 0) tag_item(t, name, name_len, p + pos, value_len);
        pos += value_len;
    }
}

/* Metadata and Metadata Library: per-stream items with 32-bit value sizes. */
static void parse_metadata(const uint8_t* p, size_t n, fa_asf_tags* t)
{
    size_t pos = 2;
    uint32_t count, i;
    if (n < 2) return;
    count = le16(p);
    for (i = 0; i < count; i++) {
        size_t name_len, type;
        uint32_t value_len;
        if (n - pos < 12) return;
        name_len = le16(p + pos + 4);
        type = le16(p + pos + 6);
        value_len = le32(p + pos + 8);
        pos += 12;
        if (n - pos < name_len || n - pos - name_len < value_len) return;
        if (type == 0) tag_item(t, p + pos, name_len, p + pos + name_len, value_len);
        pos += name_len + value_len;
    }
}

/* The children of a Header Extension Object's body. */
static int ext_children(const uint8_t* p, size_t n, const uint8_t** first, const uint8_t** end)
{
    uint32_t size;
    if (n < 22) return 0;
    size = le32(p + 18);
    if (size > n - 22) return 0;
    *first = p + 22;
    *end = p + 22 + size;
    return 1;
}

static void parse_tag_objects(const uint8_t* cur, const uint8_t* end, fa_asf_tags* t, int depth)
{
    const uint8_t *guid, *body;
    size_t len;
    while (next_object(&cur, end, &guid, &body, &len)) {
        if (guid_is(guid, g_content_desc)) parse_content_desc(body, len, t);
        else if (guid_is(guid, g_ext_content_desc)) parse_ext_content_desc(body, len, t);
        else if (guid_is(guid, g_metadata) || guid_is(guid, g_metadata_library)) parse_metadata(body, len, t);
        else if (guid_is(guid, g_header_ext) && depth == 0) {
            const uint8_t *c2, *e2;
            if (ext_children(body, len, &c2, &e2)) parse_tag_objects(c2, e2, t, 1);
        }
    }
}

int fa_asf_read_tags(fa_source* src, fa_asf_tags* out)
{
    uint8_t h[30];
    uint8_t* body;
    uint64_t size;
    memset(out, 0, sizeof(*out));
    out->loudness.track_lufs = out->loudness.album_lufs = NAN;
    out->loudness.track_peak_db = out->loudness.album_peak_db = NAN;
    if (fa_source_seek(src, 0) != FA_OK || fa_source_read(src, h, 30) != 30 || !guid_is(h, g_header)) return 0;
    size = le64(h + 16);
    if (size < 30 || size - 30 > ASF_MAX_HEADER) return 1;
    body = (uint8_t*)malloc((size_t)(size - 30) + 1);
    if (!body) return 1;
    if (fa_source_read(src, body, (size_t)(size - 30)) == (int64_t)(size - 30))
        parse_tag_objects(body, body + (size - 30), out, 0);
    free(body);
    return 1;
}

/* ---- the demuxer ------------------------------------------------------------------------- */

typedef struct asf_audio {
    uint32_t number;
    const char* codec;
    uint32_t rate, channels, block_align, bits, avg_bytes;
    const uint8_t* extra;
    size_t extra_len;
    uint32_t span, vp_len, chunk_len;   /* audio spread: scrambled objects */
    int encrypted;
} asf_audio;

/* Where a media object starts: its first payload (and sub-payload). */
typedef struct asf_ref {
    int64_t packet;
    int payload, sub;
    int64_t pts;
    size_t size;
} asf_ref;

typedef struct asf {
    fa_br br;
    fa_track track;
    uint32_t stream;            /* the chosen stream's number */
    uint32_t span, vp_len, chunk_len;
    int64_t preroll;            /* ms */
    int64_t data_start;         /* the first packet */
    int64_t data_end;           /* after the last one, -1 = unknown (broadcast) */
    int64_t npackets;           /* -1 = unknown */
    int64_t send_end;           /* send time of the end (ms), -1 = unknown */
    uint32_t packet_size;
    /* The packet being parsed. */
    uint8_t* pkt;
    int64_t pkt_no;             /* -1 = none loaded */
    int64_t next_no;            /* the next packet to load */
    size_t pos, end;
    int multiple, left, len_type, payload;
    uint8_t prop;
    /* A compressed payload being split into its objects. */
    int in_sub;
    size_t sub_pos, sub_end;
    int64_t sub_pts;
    uint32_t sub_delta;
    int sub;
    /* Restarting at an object (seeks): payloads before it are skipped. */
    int64_t skip_packet;
    int skip_payload, skip_sub;
    /* The media object being put together. */
    uint8_t* obj;
    size_t obj_cap, obj_size, obj_got;
    uint32_t obj_num;
    int obj_active;
    int64_t obj_pts;
    asf_ref obj_ref;
    uint8_t* unscrambled;
    size_t unscrambled_cap;
    /* Simple Index: packet numbers every `index_ms`. */
    uint32_t* index;
    uint32_t nindex;
    int64_t index_ms;
} asf;

static size_t var_size(int type) { return type == 3 ? 4 : (size_t)type; }

static uint32_t read_var(const uint8_t* p, int type)
{
    switch (type) {
    case 1: return p[0];
    case 2: return le16(p);
    case 3: return le32(p);
    default: return 0;
    }
}

/* Parses the header of the packet in a->pkt (payload parsing information). 0 if
 * it is not a valid packet. */
static int parse_packet(asf* a, int64_t* out_send)
{
    const uint8_t* p = a->pkt;
    size_t n = a->packet_size, i = 0, need, len;
    uint8_t lf;
    uint32_t plen = 0, pad;
    int len_type, seq_type, pad_type;
    if (p[0] & 0x80) {
        /* Error correction data: its length in the low bits (the length type must be 0). */
        if (p[0] & 0x60) return 0;
        i = 1 + (size_t)(p[0] & 0x0F);
    }
    if (i + 2 > n) return 0;
    lf = p[i];
    a->prop = p[i + 1];
    i += 2;
    len_type = (lf >> 5) & 3;
    seq_type = (lf >> 1) & 3;
    pad_type = (lf >> 3) & 3;
    need = var_size(len_type) + var_size(seq_type) + var_size(pad_type) + 6;
    if (n - i < need) return 0;
    if (len_type) plen = read_var(p + i, len_type);
    i += var_size(len_type) + var_size(seq_type);
    pad = read_var(p + i, pad_type);
    i += var_size(pad_type);
    if (out_send) *out_send = le32(p + i);
    i += 6;                                   /* send time, duration */
    /* An explicit length shorter than the packet: the rest is padding too. */
    len = len_type && plen > 0 && plen < n ? plen : n;
    if (pad > len || len - pad < i) return 0;
    a->end = len - pad;
    a->multiple = lf & 1;
    a->left = 1;
    a->len_type = 0;
    if (a->multiple) {
        if (i >= a->end) return 0;
        a->left = p[i] & 0x3F;
        a->len_type = p[i] >> 6;
        i++;
    }
    a->pos = i;
    a->payload = 0;
    a->in_sub = 0;
    return 1;
}

/* Reads packet `no` into a->pkt. FA_ERROR_END_OF_STREAM after the last one. A
 * packet whose header does not parse is left with no payloads. */
static fa_result load_packet(asf* a, int64_t no)
{
    int64_t at = a->data_start + no * (int64_t)a->packet_size;
    if (a->npackets >= 0 && no >= a->npackets) return FA_ERROR_END_OF_STREAM;
    if (a->data_end >= 0 && at + (int64_t)a->packet_size > a->data_end) return FA_ERROR_END_OF_STREAM;
    if (fa_br_tell(&a->br) != at) {
        fa_result r = fa_br_seek(&a->br, at);
        if (r != FA_OK) return r == FA_ERROR_NOT_SEEKABLE ? r : FA_ERROR_END_OF_STREAM;
    }
    if (fa_br_read(&a->br, a->pkt, a->packet_size) != a->packet_size) return FA_ERROR_END_OF_STREAM;
    a->pkt_no = no;
    a->next_no = no + 1;
    if (!parse_packet(a, NULL)) { a->left = 0; a->in_sub = 0; }
    return FA_OK;
}

/* Undoes audio spread: the object's chunks were written column by column. */
static const uint8_t* unscramble(asf* a, const uint8_t* d, size_t n)
{
    size_t per = a->vp_len / a->chunk_len, chunks = n / a->chunk_len, k;
    if (a->span <= 1 || n != (size_t)a->vp_len * a->span || a->vp_len % a->chunk_len != 0) return d;
    if (n > a->unscrambled_cap) {
        uint8_t* p = (uint8_t*)realloc(a->unscrambled, n);
        if (!p) return d;
        a->unscrambled = p;
        a->unscrambled_cap = n;
    }
    for (k = 0; k < chunks; k++) {
        size_t row = k / a->span, col = k % a->span, from = row + col * per;
        memcpy(a->unscrambled + k * a->chunk_len, d + from * a->chunk_len, a->chunk_len);
    }
    return a->unscrambled;
}

static fa_result reserve_obj(asf* a, size_t n)
{
    if (n > a->obj_cap) {
        size_t c = a->obj_cap ? a->obj_cap : 4096;
        uint8_t* p;
        while (c < n) c *= 2;
        p = (uint8_t*)realloc(a->obj, c);
        if (!p) return FA_ERROR_OUT_OF_MEMORY;
        a->obj = p;
        a->obj_cap = c;
    }
    return FA_OK;
}

/* Is this payload before the restart point? */
static int skipped(const asf* a, int sub)
{
    if (a->skip_packet != a->pkt_no) return 0;
    return a->payload < a->skip_payload || (a->payload == a->skip_payload && sub < a->skip_sub);
}

/* The next whole media object of the chosen stream in a->obj (size, pts, where it
 * started). FA_ERROR_END_OF_STREAM at the end. */
static fa_result read_object(asf* a, asf_ref* out)
{
    for (;;) {
        const uint8_t* p = a->pkt;
        uint32_t stream, num, off, rep_len, len;
        const uint8_t* rep;
        size_t need;

        /* Objects of a compressed payload, one at a time. */
        while (a->in_sub) {
            size_t n;
            int sub = a->sub++;
            if (a->sub_pos >= a->sub_end) { a->in_sub = 0; a->payload++; break; }
            n = p[a->sub_pos++];
            if (n > a->sub_end - a->sub_pos) { a->in_sub = 0; a->payload++; break; }
            if (n == 0 || skipped(a, sub)) { a->sub_pos += n; a->sub_pts += a->sub_delta; continue; }
            if (reserve_obj(a, n) != FA_OK) return FA_ERROR_OUT_OF_MEMORY;
            memcpy(a->obj, p + a->sub_pos, n);
            a->sub_pos += n;
            out->packet = a->pkt_no;
            out->payload = a->payload;
            out->sub = sub;
            out->pts = a->sub_pts;
            out->size = n;
            a->sub_pts += a->sub_delta;
            a->obj_active = 0;
            return FA_OK;
        }

        if (a->left <= 0 || a->pos >= a->end) {
            fa_result r = load_packet(a, a->next_no);
            if (r != FA_OK) return r;
            continue;
        }
        a->left--;
        /* Payload header: stream, object number, offset, replicated data, length. */
        need = 1 + var_size((a->prop >> 4) & 3) + var_size((a->prop >> 2) & 3) + var_size(a->prop & 3);
        if (a->end - a->pos < need) { a->left = 0; continue; }
        stream = p[a->pos] & 0x7F;
        a->pos++;
        num = read_var(p + a->pos, (a->prop >> 4) & 3);
        a->pos += var_size((a->prop >> 4) & 3);
        off = read_var(p + a->pos, (a->prop >> 2) & 3);
        a->pos += var_size((a->prop >> 2) & 3);
        rep_len = read_var(p + a->pos, a->prop & 3);
        a->pos += var_size(a->prop & 3);
        if (rep_len > a->end - a->pos) { a->left = 0; continue; }
        rep = p + a->pos;
        a->pos += rep_len;
        len = (uint32_t)(a->end - a->pos);
        if (a->multiple && a->len_type) {
            if (a->end - a->pos < var_size(a->len_type)) { a->left = 0; continue; }
            len = read_var(p + a->pos, a->len_type);
            a->pos += var_size(a->len_type);
            if (len > a->end - a->pos) { a->left = 0; continue; }
        }

        if (stream != a->stream || (rep_len != 1 && skipped(a, 0))) {
            a->pos += len;
            a->payload++;
            continue;
        }
        if (rep_len == 1) {
            /* Compressed: `off` is the presentation time, rep[0] the step between objects. */
            a->in_sub = 1;
            a->sub = 0;
            a->sub_pos = a->pos;
            a->sub_end = a->pos + len;
            a->sub_pts = off;
            a->sub_delta = rep[0];
            a->pos += len;
            continue;
        }
        {
            size_t size = rep_len >= 8 ? le32(rep) : len;
            int64_t pts = rep_len >= 8 ? (int64_t)le32(rep + 4) : FA_DEMUX_NO_PTS;
            const uint8_t* d = p + a->pos;
            a->pos += len;
            a->payload++;
            if (off == 0) {
                if (size == 0 || size > ASF_MAX_OBJECT) { a->obj_active = 0; continue; }
                if (reserve_obj(a, size) != FA_OK) return FA_ERROR_OUT_OF_MEMORY;
                a->obj_active = 1;
                a->obj_num = num;
                a->obj_size = size;
                a->obj_got = 0;
                a->obj_pts = pts;
                a->obj_ref.packet = a->pkt_no;
                a->obj_ref.payload = a->payload - 1;
                a->obj_ref.sub = 0;
            } else if (!a->obj_active || num != a->obj_num || off != a->obj_got) {
                a->obj_active = 0;             /* a fragment is missing (joined mid-object, damage) */
                continue;
            }
            if (len > a->obj_size - a->obj_got) { a->obj_active = 0; continue; }
            memcpy(a->obj + a->obj_got, d, len);
            a->obj_got += len;
            if (a->obj_got < a->obj_size) continue;
            a->obj_active = 0;
            *out = a->obj_ref;
            out->pts = a->obj_pts;
            out->size = a->obj_size;
            return FA_OK;
        }
    }
}

static fa_result asf_next(void* ctx, fa_demux_packet* out)
{
    asf* a = (asf*)ctx;
    asf_ref ref;
    fa_result r = read_object(a, &ref);
    if (r != FA_OK) return r;
    out->data = a->span > 1 && a->chunk_len > 0 ? unscramble(a, a->obj, ref.size) : a->obj;
    out->size = ref.size;
    out->pts = ref.pts;
    out->duration = -1;
    return FA_OK;
}

/* Makes read_object start again at `ref` (or at the start of packet `packet`). */
static fa_result restart_at(asf* a, int64_t packet, int payload, int sub)
{
    fa_result r;
    a->obj_active = 0;
    a->skip_packet = packet;
    a->skip_payload = payload;
    a->skip_sub = sub;
    r = load_packet(a, packet);
    if (r == FA_ERROR_END_OF_STREAM) {           /* past the end: nothing more to read */
        a->left = 0;
        a->in_sub = 0;
        a->next_no = packet;
        return FA_OK;
    }
    return r;
}

/* Send time of packet `no` (ms), or of the first readable packet up to 8 after it;
 * -1 if none. */
static int64_t send_time(asf* a, int64_t no)
{
    int k;
    for (k = 0; k < 8 && (a->npackets < 0 || no + k < a->npackets); k++) {
        int64_t at = a->data_start + (no + k) * (int64_t)a->packet_size, s;
        if (fa_br_seek(&a->br, at) != FA_OK) return -1;
        if (fa_br_read(&a->br, a->pkt, a->packet_size) != a->packet_size) return -1;
        a->pkt_no = -1;                          /* a->pkt no longer holds the parsed packet */
        if (parse_packet(a, &s)) return s;
    }
    return -1;
}

/* The last packet sent at or before `target` (ms), at or after `lo`. */
static int64_t find_packet(asf* a, int64_t lo, int64_t target)
{
    int64_t hi = a->npackets, s_lo = send_time(a, lo), s_hi = a->send_end;
    int step = 0;
    if (s_lo < 0 || s_lo > target) return lo;
    if (s_hi <= s_lo) s_hi = -1;
    while (hi - lo > 1) {
        int64_t mid, s;
        /* Send times grow about linearly: guess by interpolation every other step,
         * halving in between keeps the worst case logarithmic. */
        if ((step++ & 1) == 0 && s_hi > s_lo) {
            mid = lo + (int64_t)((double)(target - s_lo) / (double)(s_hi - s_lo) * (double)(hi - lo));
            if (mid <= lo) mid = lo + 1;
            if (mid >= hi) mid = hi - 1;
        } else {
            mid = lo + (hi - lo) / 2;
        }
        s = send_time(a, mid);
        if (s < 0 || s > target) {
            hi = mid;
            if (s >= 0) s_hi = s;
        } else {
            lo = mid;
            s_lo = s;
        }
    }
    return lo;
}

static fa_result asf_seek_packets(void* ctx, int64_t t, int packets_back, size_t bytes_back)
{
    asf* a = (asf*)ctx;
    int64_t back = 0, lower = 0;
    if (!a->br.src->seekable || a->npackets <= 0) return FA_ERROR_NOT_SEEKABLE;
    if (a->index && a->index_ms > 0) {
        /* The index's times may or may not include the preroll: taking it as not
         * including it, a second early, stays at or before the target either way. */
        int64_t at = t - a->preroll - 1000, e = at > 0 ? at / a->index_ms : 0;
        if (e >= (int64_t)a->nindex) e = (int64_t)a->nindex - 1;
        if (e >= 0 && (int64_t)a->index[e] < a->npackets) lower = a->index[e];
    }
    for (;;) {
        /* Objects are sent up to a preroll before they play. */
        int64_t first = find_packet(a, lower, t - a->preroll - back);
        asf_ref ring[ASF_SCAN_RING], next;
        int count = 0, head = 0, have_next = 0, j = 0, n = 0;
        size_t bytes = 0;
        fa_result r = restart_at(a, first, 0, 0);
        memset(&next, 0, sizeof(next));
        if (r != FA_OK) return r;
        for (;;) {
            asf_ref ref;
            r = read_object(a, &ref);
            if (r == FA_ERROR_OUT_OF_MEMORY) return r;
            if (r != FA_OK) break;
            if (ref.pts != FA_DEMUX_NO_PTS && ref.pts > t) { next = ref; have_next = 1; break; }
            ring[head] = ref;
            head = (head + 1) % ASF_SCAN_RING;
            if (count < ASF_SCAN_RING) count++;
        }
        if (count > 0) {
            j = (head - 1 + ASF_SCAN_RING) % ASF_SCAN_RING;
            while (n + 1 < count && (n < packets_back || bytes < bytes_back)) {
                j = (j - 1 + ASF_SCAN_RING) % ASF_SCAN_RING;
                n++;
                bytes += ring[j].size;
            }
        }
        if ((count == 0 || n < packets_back || bytes < bytes_back) && first > lower) {
            /* Not enough objects at or before the target from there (the packets play
             * later, or hold only the rest of an object): start further back. */
            back = back ? back * 2 : 1000;
            continue;
        }
        if ((count == 0 || n < packets_back || bytes < bytes_back) && lower > 0) {
            lower = 0;                           /* the index entry was not early enough */
            continue;
        }
        if (count == 0) {
            if (have_next) return restart_at(a, next.packet, next.payload, next.sub);
            return FA_OK;                        /* past the end: nothing more to read */
        }
        return restart_at(a, ring[j].packet, ring[j].payload, ring[j].sub);
    }
}

static const fa_demux_ops asf_ops = { asf_next, asf_seek_packets };

/* ---- header ----------------------------------------------------------------------------- */

/* Codec of a WAVEFORMATEX format tag. */
static const char* codec_name(uint16_t tag, uint32_t bits, const uint8_t* extra, size_t extra_len)
{
    switch (tag) {
    case 0x0160: return "wmav1";
    case 0x0161: return "wmav2";
    case 0x0162: return "wmapro";
    case 0x0163: return "wmalossless";
    case 0x000A: return "wmavoice";
    case 0x0055: return "mp3";
    case 0x0050: return "mp2";
    case 0x2000: return "ac3";
    case 0x0003: return bits == 64 ? "pcm_f64le" : "pcm_f32le";
    case 0xFFFE:
        /* WAVE_FORMAT_EXTENSIBLE: the subformat GUID starts with the real tag. */
        if (extra_len >= 22) return codec_name(le16(extra + 6), bits, NULL, 0);
        return NULL;
    case 0x0001:
        return bits == 8 ? "pcm_u8" : bits == 16 ? "pcm_s16le" : bits == 24 ? "pcm_s24le" : bits == 32 ? "pcm_s32le" : NULL;
    default: return NULL;
    }
}

/* A Stream Properties Object: an audio stream is added to the list. */
static void parse_stream(const uint8_t* p, size_t n, asf_audio* list, int* count)
{
    uint32_t ts_len, ec_len, number, k;
    const uint8_t *ts, *ec;
    asf_audio s;
    if (n < 54 || !guid_is(p, g_audio_media)) return;
    ts_len = le32(p + 40);
    ec_len = le32(p + 44);
    if (ts_len > n - 54 || ec_len > n - 54 - ts_len) return;
    number = le16(p + 48) & 0x7F;
    for (k = 0; k < (uint32_t)*count; k++)
        if (list[k].number == number) return;
    if (*count >= ASF_MAX_STREAMS || number == 0) return;
    memset(&s, 0, sizeof(s));
    s.number = number;
    s.encrypted = (le16(p + 48) & 0x8000) != 0;
    ts = p + 54;
    ec = ts + ts_len;
    if (ts_len >= 16) {
        uint16_t tag = le16(ts);
        s.channels = le16(ts + 2);
        s.rate = le32(ts + 4);
        s.avg_bytes = le32(ts + 8);
        s.block_align = le16(ts + 12);
        s.bits = le16(ts + 14);
        if (ts_len >= 18) {
            size_t cb = le16(ts + 16);
            s.extra = ts + 18;
            s.extra_len = cb < ts_len - 18 ? cb : ts_len - 18;
        }
        s.codec = codec_name(tag, s.bits, s.extra, s.extra_len);
    }
    if (guid_is(p + 16, g_audio_spread) && ec_len >= 5) {
        s.span = ec[0];
        s.vp_len = le16(ec + 1);
        s.chunk_len = le16(ec + 3);
        if (s.chunk_len == 0 || s.vp_len == 0) s.span = 0;
    }
    list[(*count)++] = s;
}

typedef struct asf_header {
    asf_audio streams[ASF_MAX_STREAMS];
    int count;
    int all_streams;            /* Stream Properties Objects of any type */
    int protected_content;
    int have_props;
    uint64_t play_100ns, send_100ns, preroll_ms, packets;
    uint32_t flags, min_packet, max_packet;
} asf_header;

static void parse_header_objects(const uint8_t* cur, const uint8_t* end, asf_header* h, int depth)
{
    const uint8_t *guid, *body;
    size_t len;
    while (next_object(&cur, end, &guid, &body, &len)) {
        if (guid_is(guid, g_file_props) && len >= 80) {
            h->have_props = 1;
            h->packets = le64(body + 32);
            h->play_100ns = le64(body + 40);
            h->send_100ns = le64(body + 48);
            h->preroll_ms = le64(body + 56);
            h->flags = le32(body + 64);
            h->min_packet = le32(body + 68);
            h->max_packet = le32(body + 72);
        } else if (guid_is(guid, g_stream_props)) {
            h->all_streams++;
            parse_stream(body, len, h->streams, &h->count);
        } else if (guid_is(guid, g_content_encryption) || guid_is(guid, g_ext_content_encryption) ||
                   guid_is(guid, g_adv_content_encryption)) {
            h->protected_content = 1;
        } else if (guid_is(guid, g_header_ext) && depth == 0) {
            const uint8_t *c2, *e2;
            if (ext_children(body, len, &c2, &e2)) parse_header_objects(c2, e2, h, 1);
        } else if (guid_is(guid, g_ext_stream_props) && len >= 64) {
            /* Names and payload extension systems, then maybe a Stream Properties Object. */
            size_t pos = 64, k, names = le16(body + 60), exts = le16(body + 62);
            const uint8_t *g2, *b2, *c2;
            size_t l2;
            for (k = 0; k < names && pos <= len; k++) {
                if (len - pos < 4) { pos = len + 1; break; }
                pos += 4 + (size_t)le16(body + pos + 2);
            }
            for (k = 0; k < exts && pos <= len; k++) {
                if (len - pos < 22) { pos = len + 1; break; }
                pos += 22 + (size_t)le32(body + pos + 18);
            }
            if (pos >= len) continue;
            c2 = body + pos;
            if (next_object(&c2, body + len, &g2, &b2, &l2) && guid_is(g2, g_stream_props)) {
                h->all_streams++;
                parse_stream(b2, l2, h->streams, &h->count);
            }
        }
    }
}

/* Simple Index Object(s) after the Data Object: the first one is kept. Local
 * seekable files only (a network file would need a request to the end). */
static fa_result read_index(asf* a, int64_t from)
{
    fa_source* src = a->br.src;
    int k;
    for (k = 0; k < 16; k++) {
        uint8_t h[24 + 32];
        uint64_t size;
        if (src->length > 0 && from + 24 > src->length) break;
        if (fa_br_seek(&a->br, from) != FA_OK || fa_br_read(&a->br, h, 24) != 24) break;
        size = le64(h + 16);
        if (size < 24 || size > (uint64_t)INT64_MAX - (uint64_t)from) break;
        if (guid_is(h, g_simple_index) && size >= 56) {
            uint64_t interval, count, i;
            if (fa_br_read(&a->br, h + 24, 32) != 32) break;
            interval = le64(h + 40);
            count = le32(h + 52);
            if (count > (size - 56) / 6) count = (size - 56) / 6;
            if (count > ASF_MAX_INDEX || interval < 10000 || count == 0) break;
            a->index = (uint32_t*)malloc((size_t)count * sizeof(uint32_t));
            if (!a->index) return FA_ERROR_OUT_OF_MEMORY;
            for (i = 0; i < count; i++) {
                uint8_t e[6];
                if (fa_br_read(&a->br, e, 6) != 6) break;
                a->index[i] = le32(e);
                /* Entries never go back; a damaged one would mislead the search. */
                if (i > 0 && a->index[i] < a->index[i - 1]) a->index[i] = a->index[i - 1];
            }
            a->nindex = (uint32_t)i;
            a->index_ms = (int64_t)(interval / 10000);
            if (a->nindex == 0) { free(a->index); a->index = NULL; }
            break;
        }
        from += (int64_t)size;
    }
    return FA_OK;
}

/* The track's last media object, for its length: the play duration in the File
 * Properties is the whole file's (other streams may run longer), and only
 * milliseconds. Local files; reads the last packets, further back if needed. */
static int find_last(asf* a, const fa_codec_params* cp, fa_track_params* tp, int other_streams)
{
    int64_t chunk = 16, from;
    asf_ref last, prev;
    int have = 0, have_prev = 0;
    memset(&last, 0, sizeof(last));
    memset(&prev, 0, sizeof(prev));
    for (;;) {
        from = a->npackets > chunk ? a->npackets - chunk : 0;
        if (restart_at(a, from, 0, 0) != FA_OK) return 0;
        for (;;) {
            asf_ref ref;
            if (read_object(a, &ref) != FA_OK) break;
            if (ref.pts == FA_DEMUX_NO_PTS) continue;
            prev = last;
            have_prev = have;
            last = ref;
            have = 1;
        }
        if (have || from == 0 || chunk * (int64_t)a->packet_size > (8 << 20)) break;
        chunk *= 4;
    }
    if (!have) return 0;
    {
        uint32_t rate = 0;
        int64_t n;
        /* The object's own samples where the codec tells (PCM, MP3, AC-3): exact. */
        restart_at(a, last.packet, last.payload, last.sub);
        if (read_object(a, &last) != FA_OK) return 0;
        n = fa_demux_packet_samples(cp->codec, cp->channels, cp->sample_rate, a->obj, last.size, &rate);
        if (n >= 0 && rate > 0) {
            tp->last_pts = last.pts;
            tp->last_end = last.pts + (int64_t)((double)n * 1000.0 / rate + 0.5);
            tp->last_exact = 1;
            return 1;
        }
    }
    /* Else as long as the one before; only worth it over the file's duration when
     * other streams make that one too long. */
    if (!other_streams || !have_prev || last.pts < prev.pts) return 0;
    tp->last_pts = last.pts;
    tp->last_end = last.pts + (last.pts - prev.pts);
    tp->last_exact = 0;
    tp->last_trim = 0;
    return 1;
}

static void asf_free(asf* a)
{
    fa_br_free(&a->br);
    free(a->pkt);
    free(a->obj);
    free(a->unscrambled);
    free(a->index);
    free(a);
}

static fa_result asf_open(fa_decoder* dec)
{
    fa_source* src = dec->src;
    asf* a = (asf*)calloc(1, sizeof(asf));
    asf_header* h = (asf_header*)calloc(1, sizeof(asf_header));
    uint8_t head[50];
    uint8_t* body = NULL;
    size_t body_cap = 0;
    uint64_t size, data_size;
    const asf_audio* s;
    fa_track_params tp;
    int broadcast;
    fa_result r;

    if (!a || !h) { free(a); free(h); return FA_ERROR_OUT_OF_MEMORY; }
    r = fa_br_init(&a->br, src);
    if (r != FA_OK) { free(a); free(h); return r; }
    a->pkt_no = -1;
    a->skip_packet = -1;

    /* Header Object: read whole (the Data Object follows it). */
    if (fa_br_read(&a->br, head, 30) != 30 || !guid_is(head, g_header)) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto fail; }
    size = le64(head + 16);
    if (size < 30 || size - 30 > ASF_MAX_HEADER) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto fail; }
    r = fa_br_read_alloc(&a->br, (size_t)(size - 30), ASF_MAX_HEADER, &body, &body_cap);
    if (r != FA_OK) { r = r == FA_ERROR_OUT_OF_MEMORY ? r : FA_ERROR_UNSUPPORTED_FORMAT; goto fail; }
    parse_header_objects(body, body + (size - 30), h, 0);

    dec->track_count = h->count;
    if (!h->have_props || h->count == 0) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto fail; }
    if (dec->audio_track >= h->count) { r = FA_ERROR_NOT_FOUND; goto fail; }
    s = &h->streams[dec->audio_track < 0 ? 0 : dec->audio_track];
    /* Protected content: there is no way to decode it here (see the top). */
    if (h->protected_content || s->encrypted) { r = FA_ERROR_PROTECTED_CONTENT; goto fail; }
    if (!s->codec) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto fail; }
    if (h->min_packet != h->max_packet || h->min_packet == 0 || h->min_packet > ASF_MAX_PACKET) {
        r = FA_ERROR_UNSUPPORTED_FORMAT;
        goto fail;
    }
    broadcast = (h->flags & 1) != 0;
    a->stream = s->number;
    a->packet_size = h->min_packet;
    a->preroll = h->preroll_ms < (1u << 30) ? (int64_t)h->preroll_ms : 0;
    a->span = s->span;
    a->vp_len = s->vp_len;
    a->chunk_len = s->chunk_len;
    a->pkt = (uint8_t*)malloc(a->packet_size);
    if (!a->pkt) { r = FA_ERROR_OUT_OF_MEMORY; goto fail; }

    /* Data Object header, then the packets. */
    if (fa_br_tell(&a->br) != (int64_t)size && fa_br_seek(&a->br, (int64_t)size) != FA_OK) {
        r = FA_ERROR_UNSUPPORTED_FORMAT;
        goto fail;
    }
    if (fa_br_read(&a->br, head, 50) != 50 || !guid_is(head, g_data)) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto fail; }
    data_size = le64(head + 16);
    a->data_start = (int64_t)size + 50;
    a->data_end = -1;
    a->npackets = -1;
    if (data_size >= 50 && data_size < (uint64_t)INT64_MAX / 2) a->data_end = (int64_t)size + (int64_t)data_size;
    if (le64(head + 40) > 0 && le64(head + 40) < (1ull << 40)) a->npackets = (int64_t)le64(head + 40);
    else if (!broadcast && h->packets > 0 && h->packets < (1ull << 40)) a->npackets = (int64_t)h->packets;
    if (a->npackets < 0 && a->data_end > a->data_start)
        a->npackets = (a->data_end - a->data_start) / a->packet_size;
    if (a->npackets < 0 && src->length > a->data_start)
        a->npackets = (src->length - a->data_start) / a->packet_size;
    a->send_end = !broadcast && h->send_100ns > 0 ? (int64_t)(h->send_100ns / 10000) : -1;
    a->next_no = 0;

    memset(&tp, 0, sizeof(tp));
    tp.last_pts = FA_DEMUX_NO_PTS;
    fa_codec_params_init(&tp.codec);
    tp.codec.codec = s->codec;
    tp.codec.sample_rate = s->rate;
    tp.codec.channels = s->channels;
    /* The Simple Index after the data, and the last object. Local files only. */
    if (src->seekable && !src->remote && a->npackets > 0) {
        if (a->data_end > 0) {
            r = read_index(a, a->data_end);
            if (r != FA_OK) goto fail;
        }
        if (!find_last(a, &tp.codec, &tp, h->all_streams > 1)) tp.last_pts = FA_DEMUX_NO_PTS;
        a->obj_active = a->in_sub = a->left = 0;
        a->pkt_no = -1;
        a->skip_packet = -1;
        a->next_no = 0;
        r = fa_br_seek(&a->br, a->data_start);
        if (r != FA_OK) goto fail;
    }

    tp.codec.extradata = s->extra_len ? s->extra : NULL;
    tp.codec.extradata_size = s->extra_len;
    tp.codec.block_align = s->block_align;
    tp.codec.bit_rate = (int64_t)s->avg_bytes * 8;
    tp.codec.bits_per_coded_sample = s->bits;
    tp.time_base = 1e-3;
    tp.start = a->preroll;
    tp.end = FA_DEMUX_NO_PTS;
    /* Play duration counts the preroll; it is 100 ns exact, presentation times are ms. */
    if (!broadcast && h->play_100ns > 0 && h->play_100ns / 10000 > h->preroll_ms && h->play_100ns < (1ull << 62))
        tp.end = (int64_t)((h->play_100ns + 5000) / 10000);
    tp.precision = 1.0;
    r = fa_track_open(&a->track, dec, &asf_ops, a, &tp);
    if (r != FA_OK) goto fail;
    free(body);
    free(h);
    dec->impl = a;
    dec->seekable = src->seekable && a->npackets > 0;
    snprintf(dec->container, sizeof(dec->container), "asf");
    return FA_OK;
fail:
    free(body);
    free(h);
    asf_free(a);
    return r;
}

static fa_result asf_read(fa_decoder* dec, float* out, fa_frames frames, fa_frames* got)
{
    return fa_track_read(&((asf*)dec->impl)->track, out, frames, got);
}

static fa_result asf_seek(fa_decoder* dec, fa_frames frame)
{
    if (!dec->seekable) return FA_ERROR_NOT_SEEKABLE;
    return fa_track_seek(&((asf*)dec->impl)->track, frame);
}

static void asf_close(fa_decoder* dec)
{
    asf* a = (asf*)dec->impl;
    if (!a) return;
    fa_track_close(&a->track);
    asf_free(a);
    dec->impl = NULL;
}

static void asf_lag(fa_decoder* dec, int64_t* out_bytes, int64_t* out_frames)
{
    asf* a = (asf*)dec->impl;
    *out_bytes = (int64_t)(a->br.len - a->br.off) + (a->pkt_no >= 0 ? (int64_t)(a->end - a->pos) : 0);
    *out_frames = a->track.count - a->track.off;
}

static int asf_probe(const uint8_t* d, size_t n, size_t id3)
{
    (void)id3;
    if (n < 30 || !guid_is(d, g_header)) return 0;
    return le64(d + 16) >= 30 ? 100 : 0;
}

const fa_decoder_vtable fa_decoder_asf = {
    "asf", "ASF and Windows Media Audio (.wma, .asf, .wmv; audio streams)", "wma;asf;wmv",
    "audio/x-ms-wma;video/x-ms-asf;video/x-ms-wmv;application/vnd.ms-asf;audio/x-ms-asf",
    asf_probe, asf_open, asf_read, asf_seek, asf_close, 1, asf_lag, NULL
};
