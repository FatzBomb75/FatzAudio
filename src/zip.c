/* ZIP archives, for karaoke downloads that pack the audio and the .cdg together.
 * Written from the PKWARE APPNOTE (central directory, stored and deflate entries)
 * and RFC 1951 (DEFLATE). Archives are read from memory: karaoke ZIPs are a few MB. */
#include "cdg.h"
#include <stdlib.h>
#include <string.h>

#define ZIP_LOCAL_SIG 0x04034b50u
#define ZIP_CENTRAL_SIG 0x02014b50u
#define ZIP_END_SIG 0x06054b50u
#define ZIP_END_SIZE 22
#define ZIP_CENTRAL_SIZE 46
#define ZIP_LOCAL_SIZE 30

static uint32_t rd16(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const uint8_t* p) { return rd16(p) | (rd16(p + 2) << 16); }

/* ---- CRC-32 (IEEE 802.3, reflected) ---------------------------------------- */

uint32_t fa_crc32(const uint8_t* data, size_t size)
{
    uint32_t table[256], crc = 0xFFFFFFFFu, n, k, c;
    size_t i;
    /* Built per call: one call per extracted file, and no shared state. */
    for (n = 0; n < 256; n++) {
        c = n;
        for (k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        table[n] = c;
    }
    for (i = 0; i < size; i++) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* ---- DEFLATE ------------------------------------------------------------------ */

#define INF_MAX_BITS 15
#define INF_MAX_LITLEN 288
#define INF_MAX_DIST 30

typedef struct inf_huff {
    uint16_t count[INF_MAX_BITS + 1];   /* codes of each length */
    uint16_t symbol[INF_MAX_LITLEN];    /* symbols ordered by code */
} inf_huff;

typedef struct inf_state {
    const uint8_t* in;
    size_t in_size, in_pos;
    uint32_t bitbuf;
    int bitcnt;
    uint8_t* out;
    size_t out_size, out_pos;
    int error;                          /* ran past the input */
} inf_state;

static uint32_t inf_bits(inf_state* s, int need)
{
    uint32_t v;
    while (s->bitcnt < need) {
        if (s->in_pos >= s->in_size) { s->error = 1; return 0; }
        s->bitbuf |= (uint32_t)s->in[s->in_pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    v = s->bitbuf & ((1u << need) - 1u);
    s->bitbuf >>= need;
    s->bitcnt -= need;
    return v;
}

/* Canonical code from code lengths. Returns 0 for a complete or incomplete code,
 * -1 when over-subscribed (invalid). */
static int inf_build(inf_huff* h, const uint8_t* lengths, int n)
{
    uint16_t offs[INF_MAX_BITS + 1];
    int sym, len, left = 1;
    memset(h->count, 0, sizeof(h->count));
    for (sym = 0; sym < n; sym++) h->count[lengths[sym]]++;
    if (h->count[0] == n) return 0;
    for (len = 1; len <= INF_MAX_BITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;
    }
    offs[1] = 0;
    for (len = 1; len < INF_MAX_BITS; len++) offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    for (sym = 0; sym < n; sym++)
        if (lengths[sym]) h->symbol[offs[lengths[sym]]++] = (uint16_t)sym;
    return 0;
}

/* Huffman codes are packed most significant bit first: build the code bit by bit
 * and compare it with the first code of each length. */
static int inf_decode(inf_state* s, const inf_huff* h)
{
    int code = 0, first = 0, index = 0, len;
    for (len = 1; len <= INF_MAX_BITS; len++) {
        int count;
        code |= (int)inf_bits(s, 1);
        if (s->error) return -1;
        count = h->count[len];
        if (code - first < count) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1; /* a code the table does not have */
}

static const uint16_t len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                       35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                       3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                        8193, 12289, 16385, 24577 };
static const uint8_t dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int inf_codes(inf_state* s, const inf_huff* lit, const inf_huff* dist)
{
    for (;;) {
        int sym = inf_decode(s, lit);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (s->out_pos >= s->out_size) return -1;
            s->out[s->out_pos++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            size_t length, distance;
            int d;
            sym -= 257;
            if (sym >= 29) return -1;
            length = len_base[sym] + inf_bits(s, len_extra[sym]);
            d = inf_decode(s, dist);
            if (d < 0 || d >= 30) return -1;
            distance = dist_base[d] + inf_bits(s, dist_extra[d]);
            if (s->error || distance > s->out_pos || length > s->out_size - s->out_pos) return -1;
            /* Byte by byte: the copy may overlap what it writes (runs). */
            while (length--) { s->out[s->out_pos] = s->out[s->out_pos - distance]; s->out_pos++; }
        }
    }
}

static int inf_stored(inf_state* s)
{
    uint32_t len, nlen;
    s->bitbuf = 0;   /* skip to the byte boundary */
    s->bitcnt = 0;
    if (s->in_size - s->in_pos < 4) return -1;
    len = rd16(s->in + s->in_pos);
    nlen = rd16(s->in + s->in_pos + 2);
    s->in_pos += 4;
    if (len != (~nlen & 0xFFFFu)) return -1;
    if (s->in_size - s->in_pos < len || s->out_size - s->out_pos < len) return -1;
    memcpy(s->out + s->out_pos, s->in + s->in_pos, len);
    s->in_pos += len;
    s->out_pos += len;
    return 0;
}

static int inf_fixed(inf_state* s)
{
    inf_huff lit, dist;
    uint8_t lengths[INF_MAX_LITLEN];
    int i;
    for (i = 0; i < 144; i++) lengths[i] = 8;
    for (; i < 256; i++) lengths[i] = 9;
    for (; i < 280; i++) lengths[i] = 7;
    for (; i < INF_MAX_LITLEN; i++) lengths[i] = 8;
    inf_build(&lit, lengths, INF_MAX_LITLEN);
    for (i = 0; i < INF_MAX_DIST; i++) lengths[i] = 5;
    inf_build(&dist, lengths, INF_MAX_DIST);
    return inf_codes(s, &lit, &dist);
}

static int inf_dynamic(inf_state* s)
{
    static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    uint8_t lengths[INF_MAX_LITLEN + INF_MAX_DIST];
    inf_huff lencode, lit, dist;
    int nlen, ndist, ncode, i;
    nlen = (int)inf_bits(s, 5) + 257;
    ndist = (int)inf_bits(s, 5) + 1;
    ncode = (int)inf_bits(s, 4) + 4;
    if (s->error || nlen > 286 || ndist > INF_MAX_DIST) return -1;
    memset(lengths, 0, sizeof(lengths));
    for (i = 0; i < ncode; i++) lengths[order[i]] = (uint8_t)inf_bits(s, 3);
    if (s->error || inf_build(&lencode, lengths, 19) != 0) return -1;
    i = 0;
    while (i < nlen + ndist) {
        int sym = inf_decode(s, &lencode), repeat;
        uint8_t value = 0;
        if (sym < 0) return -1;
        if (sym < 16) { lengths[i++] = (uint8_t)sym; continue; }
        if (sym == 16) {
            if (i == 0) return -1;
            value = lengths[i - 1];
            repeat = 3 + (int)inf_bits(s, 2);
        } else if (sym == 17) {
            repeat = 3 + (int)inf_bits(s, 3);
        } else {
            repeat = 11 + (int)inf_bits(s, 7);
        }
        if (s->error || i + repeat > nlen + ndist) return -1;
        while (repeat--) lengths[i++] = value;
    }
    if (lengths[256] == 0) return -1; /* no end-of-block code */
    if (inf_build(&lit, lengths, nlen) != 0 || inf_build(&dist, lengths + nlen, ndist) != 0) return -1;
    return inf_codes(s, &lit, &dist);
}

fa_result fa_inflate(const uint8_t* in, size_t in_size, uint8_t* out, size_t out_size)
{
    inf_state s;
    int last;
    memset(&s, 0, sizeof(s));
    s.in = in; s.in_size = in_size;
    s.out = out; s.out_size = out_size;
    do {
        int type, r;
        last = (int)inf_bits(&s, 1);
        type = (int)inf_bits(&s, 2);
        if (s.error) return FA_ERROR_UNSUPPORTED_FORMAT;
        if (type == 0) r = inf_stored(&s);
        else if (type == 1) r = inf_fixed(&s);
        else if (type == 2) r = inf_dynamic(&s);
        else r = -1;
        if (r != 0 || s.error) return FA_ERROR_UNSUPPORTED_FORMAT;
    } while (!last);
    return s.out_pos == out_size ? FA_OK : FA_ERROR_UNSUPPORTED_FORMAT;
}

/* ---- Archives ------------------------------------------------------------------ */

int fa_zip_probe(const uint8_t* data, size_t size)
{
    return size >= 4 && rd32(data) == ZIP_LOCAL_SIG;
}

fa_result fa_zip_list(const uint8_t* zip, size_t zip_size, fa_zip_entry** out_entries, uint32_t* out_count)
{
    size_t end, pos, cd_end, scan_from;
    uint32_t total, i, n = 0;
    fa_zip_entry* entries;
    *out_entries = NULL;
    *out_count = 0;
    if (zip_size < ZIP_END_SIZE) return FA_ERROR_UNSUPPORTED_FORMAT;
    /* The end record is the last thing in the file, followed only by a comment of
     * at most 64 KB. */
    end = zip_size - ZIP_END_SIZE;
    scan_from = zip_size - ZIP_END_SIZE > 0xFFFF ? zip_size - ZIP_END_SIZE - 0xFFFF : 0;
    for (;;) {
        if (rd32(zip + end) == ZIP_END_SIG && end + ZIP_END_SIZE + rd16(zip + end + 20) == zip_size) break;
        if (end == scan_from) return FA_ERROR_UNSUPPORTED_FORMAT;
        end--;
    }
    total = rd16(zip + end + 10);
    pos = rd32(zip + end + 16);
    cd_end = pos + (size_t)rd32(zip + end + 12);
    /* 0xFFFF / 0xFFFFFFFF mean ZIP64; multi-disk archives have disk numbers. */
    if (total == 0xFFFF || rd32(zip + end + 16) == 0xFFFFFFFFu || rd16(zip + end + 4) != 0 ||
        cd_end > end || pos > cd_end)
        return FA_ERROR_UNSUPPORTED_FORMAT;
    entries = (fa_zip_entry*)calloc(total ? total : 1, sizeof(*entries));
    if (!entries) return FA_ERROR_OUT_OF_MEMORY;
    for (i = 0; i < total; i++) {
        const uint8_t* h = zip + pos;
        uint32_t flags, name_len, skip;
        fa_zip_entry* e = &entries[n];
        if (cd_end - pos < ZIP_CENTRAL_SIZE || rd32(h) != ZIP_CENTRAL_SIG) { free(entries); return FA_ERROR_UNSUPPORTED_FORMAT; }
        flags = rd16(h + 8);
        name_len = rd16(h + 28);
        skip = ZIP_CENTRAL_SIZE + name_len + rd16(h + 30) + rd16(h + 32);
        if (cd_end - pos < skip) { free(entries); return FA_ERROR_UNSUPPORTED_FORMAT; }
        e->method = rd16(h + 10);
        e->crc32 = rd32(h + 16);
        e->compressed_size = rd32(h + 20);
        e->size = rd32(h + 24);
        e->header_offset = rd32(h + 42);
        if (name_len >= sizeof(e->name)) name_len = sizeof(e->name) - 1;
        memcpy(e->name, h + ZIP_CENTRAL_SIZE, name_len);
        e->name[name_len] = '\0';
        pos += skip;
        if (e->compressed_size == 0xFFFFFFFFu || e->size == 0xFFFFFFFFu || e->header_offset == 0xFFFFFFFFu) {
            free(entries);
            return FA_ERROR_UNSUPPORTED_FORMAT;
        }
        /* Directories and encrypted entries are left out. */
        if (name_len == 0 || e->name[name_len - 1] == '/' || (flags & 1)) continue;
        n++;
    }
    *out_entries = entries;
    *out_count = n;
    return FA_OK;
}

fa_result fa_zip_extract(const uint8_t* zip, size_t zip_size, const fa_zip_entry* e, uint8_t** out_data)
{
    const uint8_t* h;
    size_t data_pos;
    uint8_t* out;
    fa_result r;
    *out_data = NULL;
    if (e->header_offset > zip_size || zip_size - e->header_offset < ZIP_LOCAL_SIZE) return FA_ERROR_UNSUPPORTED_FORMAT;
    h = zip + e->header_offset;
    if (rd32(h) != ZIP_LOCAL_SIG) return FA_ERROR_UNSUPPORTED_FORMAT;
    data_pos = (size_t)e->header_offset + ZIP_LOCAL_SIZE + rd16(h + 26) + rd16(h + 28);
    if (data_pos > zip_size || zip_size - data_pos < e->compressed_size) return FA_ERROR_UNSUPPORTED_FORMAT;
    if (e->method != 0 && e->method != 8) return FA_ERROR_UNSUPPORTED_FORMAT;
    if (e->method == 0 && e->compressed_size != e->size) return FA_ERROR_UNSUPPORTED_FORMAT;
    /* DEFLATE expands at most 1032:1 (258-byte matches in about two bits each), so a
     * larger declared size is a lie: refuse it before allocating. */
    if (e->method == 8 && e->size > e->compressed_size * 1032 + 1024) return FA_ERROR_UNSUPPORTED_FORMAT;
    out = (uint8_t*)malloc(e->size ? (size_t)e->size : 1);
    if (!out) return FA_ERROR_OUT_OF_MEMORY;
    if (e->method == 0) {
        memcpy(out, zip + data_pos, (size_t)e->size);
        r = FA_OK;
    } else {
        r = fa_inflate(zip + data_pos, (size_t)e->compressed_size, out, (size_t)e->size);
    }
    if (r == FA_OK && fa_crc32(out, (size_t)e->size) != e->crc32) r = FA_ERROR_UNSUPPORTED_FORMAT;
    if (r != FA_OK) { free(out); return r; }
    *out_data = out;
    return FA_OK;
}
