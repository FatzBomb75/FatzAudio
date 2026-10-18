/* ZIP archives, for karaoke downloads that pack the audio and the .cdg together.
 * Written from the PKWARE APPNOTE (central directory, stored and deflate entries)
 * and RFC 1951 (DEFLATE). Archives are read through a source: only the directory
 * and the entries asked for are read, never the whole file. */
#include "cdg.h"
#include <stdlib.h>
#include <string.h>

#define ZIP_LOCAL_SIG 0x04034b50u
#define ZIP_CENTRAL_SIG 0x02014b50u
#define ZIP_END_SIG 0x06054b50u
#define ZIP_END_SIZE 22
#define ZIP_CENTRAL_SIZE 46
#define ZIP_LOCAL_SIZE 30
#define ZIP64_END_SIG 0x06064b50u
#define ZIP64_END_SIZE 56
#define ZIP64_LOCATOR_SIG 0x07064b50u
#define ZIP64_LOCATOR_SIZE 20
#define ZIP_MAX_DIRECTORY ((uint64_t)16 << 20)

static uint32_t rd16(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const uint8_t* p) { return rd16(p) | (rd16(p + 2) << 16); }

/* ---- CRC-32 (IEEE 802.3, reflected) ---------------------------------------- */

/* `crc` is the CRC of the bytes before `data` (0 to start). */
static uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t size)
{
    uint32_t table[256], n, k, c;
    size_t i;
    /* Built per call: a few calls per extracted file, and no shared state. */
    for (n = 0; n < 256; n++) {
        c = n;
        for (k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        table[n] = c;
    }
    crc ^= 0xFFFFFFFFu;
    for (i = 0; i < size; i++) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

uint32_t fa_crc32(const uint8_t* data, size_t size) { return crc32_update(0, data, size); }

/* ---- DEFLATE ------------------------------------------------------------------ */

#define INF_MAX_BITS 15
#define INF_MAX_LITLEN 288
#define INF_MAX_DIST 30

typedef struct inf_huff {
    uint16_t count[INF_MAX_BITS + 1];   /* codes of each length */
    uint16_t symbol[INF_MAX_LITLEN];    /* symbols ordered by code */
} inf_huff;

#define INF_WINDOW 32768                 /* the farthest a match reaches back */
#define INF_IN_CHUNK 65536               /* input read from a source at a time */
#define INF_OUT_CHUNK (1u << 20)         /* output kept before writing to a file */

/* Input comes from memory or is pulled from a source; output goes to one buffer
 * of the whole size, or through a 1 MB buffer into a file, keeping the last 32 KB
 * for matches to copy from. */
typedef struct inf_state {
    const uint8_t* in;
    size_t in_size, in_pos;             /* the input chunk at hand */
    fa_source* src;                     /* NULL: `in` is all of it */
    uint64_t src_left;                  /* compressed bytes not read from src yet */
    uint8_t* in_buf;
    uint32_t bitbuf;
    int bitcnt;
    uint8_t* out;
    size_t out_cap, out_pos;
    uint64_t flushed;                   /* bytes written to `sink` before out[0] */
    uint64_t total;                     /* the size the output must have */
    FILE* sink;
    uint32_t crc;                       /* of the flushed bytes */
    int error;                          /* ran past the input */
    int io_error;                       /* the source or the file failed */
} inf_state;

static int inf_refill(inf_state* s)
{
    int64_t got;
    size_t want;
    if (!s->src || s->src_left == 0) return 0;
    want = s->src_left < INF_IN_CHUNK ? (size_t)s->src_left : INF_IN_CHUNK;
    got = fa_source_read(s->src, s->in_buf, want);
    if (got <= 0) { s->io_error = got < 0; return 0; }
    s->src_left -= (uint64_t)got;
    s->in = s->in_buf;
    s->in_size = (size_t)got;
    s->in_pos = 0;
    return 1;
}

/* Room for one more output byte: writes out all but the last 32 KB when full. */
static int inf_room(inf_state* s)
{
    size_t keep, n;
    if (s->out_pos < s->out_cap) return 0;
    if (!s->sink) return -1;
    keep = INF_WINDOW;
    n = s->out_pos - keep;
    if (fwrite(s->out, 1, n, s->sink) != n) { s->io_error = 1; return -1; }
    s->crc = crc32_update(s->crc, s->out, n);
    memmove(s->out, s->out + n, keep);
    s->flushed += n;
    s->out_pos = keep;
    return 0;
}

static uint64_t inf_left(const inf_state* s) { return s->total - s->flushed - s->out_pos; }

static uint32_t inf_bits(inf_state* s, int need)
{
    uint32_t v;
    while (s->bitcnt < need) {
        if (s->in_pos >= s->in_size && !inf_refill(s)) { s->error = 1; return 0; }
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
            if (inf_left(s) == 0 || inf_room(s)) return -1;
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
            /* After a flush out_pos is at least INF_WINDOW, as far as a match reaches. */
            if (s->error || distance > s->out_pos || length > inf_left(s)) return -1;
            while (length) {
                size_t n;
                if (inf_room(s)) return -1;
                n = s->out_cap - s->out_pos;
                if (n > length) n = length;
                length -= n;
                /* Byte by byte: the copy may overlap what it writes (runs). */
                while (n--) { s->out[s->out_pos] = s->out[s->out_pos - distance]; s->out_pos++; }
            }
        }
    }
}

/* Copies `len` input bytes to the output as they are (stored blocks and entries). */
static int inf_copy(inf_state* s, uint64_t len)
{
    while (len) {
        size_t n;
        if (s->in_pos >= s->in_size && !inf_refill(s)) return -1;
        if (inf_room(s)) return -1;
        n = s->in_size - s->in_pos;
        if (n > len) n = (size_t)len;
        if (n > s->out_cap - s->out_pos) n = s->out_cap - s->out_pos;
        memcpy(s->out + s->out_pos, s->in + s->in_pos, n);
        s->in_pos += n;
        s->out_pos += n;
        len -= n;
    }
    return 0;
}

static int inf_stored(inf_state* s)
{
    uint32_t len, nlen;
    s->bitbuf = 0;   /* skip to the byte boundary (fewer than 8 bits are held) */
    s->bitcnt = 0;
    len = inf_bits(s, 16);
    nlen = inf_bits(s, 16);
    if (s->error || len != (~nlen & 0xFFFFu) || len > inf_left(s)) return -1;
    return inf_copy(s, len);
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

static fa_result inf_run(inf_state* s)
{
    int last;
    do {
        int type, r;
        last = (int)inf_bits(s, 1);
        type = (int)inf_bits(s, 2);
        if (s->error) break;
        if (type == 0) r = inf_stored(s);
        else if (type == 1) r = inf_fixed(s);
        else if (type == 2) r = inf_dynamic(s);
        else r = -1;
        if (r != 0 || s->error) { s->error = 1; break; }
    } while (!last);
    if (s->io_error) return FA_ERROR_IO;
    return !s->error && inf_left(s) == 0 ? FA_OK : FA_ERROR_UNSUPPORTED_FORMAT;
}

fa_result fa_inflate(const uint8_t* in, size_t in_size, uint8_t* out, size_t out_size)
{
    inf_state s;
    memset(&s, 0, sizeof(s));
    s.in = in; s.in_size = in_size;
    s.out = out; s.out_cap = out_size;
    s.total = out_size;
    return inf_run(&s);
}

/* ---- Archives ------------------------------------------------------------------ */

int fa_zip_probe(const uint8_t* data, size_t size)
{
    return size >= 4 && rd32(data) == ZIP_LOCAL_SIG;
}

/* Exactly n bytes at `offset`; a short read is a damaged (cut) archive. */
static fa_result read_at(fa_source* src, int64_t offset, void* buf, size_t n)
{
    size_t done = 0;
    if (fa_source_seek(src, offset) != FA_OK) return FA_ERROR_IO;
    while (done < n) {
        int64_t got = fa_source_read(src, (uint8_t*)buf + done, n - done);
        if (got < 0) return FA_ERROR_IO;
        if (got == 0) return FA_ERROR_UNSUPPORTED_FORMAT;
        done += (size_t)got;
    }
    return FA_OK;
}

/* ZIP64 (APPNOTE 4.3.14 to 4.3.15 and 4.5.3): the end record holds 0xFFFF or
 * 0xFFFFFFFF where a value does not fit, and the real ones follow in a ZIP64 end
 * record, found through a locator right before the end record. */
static uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

/* The directory's place, from the end record at tail[end] (tail starts at `tail_at`). */
static fa_result find_directory(fa_source* src, const uint8_t* tail, size_t end, int64_t tail_at,
                                uint64_t* total, uint64_t* cd_at, uint64_t* cd_size)
{
    uint8_t z[ZIP64_END_SIZE];
    uint64_t z_at, limit = (uint64_t)tail_at + end;
    fa_result r;
    *total = rd16(tail + end + 10);
    *cd_size = rd32(tail + end + 12);
    *cd_at = rd32(tail + end + 16);
    if (rd16(tail + end + 4) != 0 || rd16(tail + end + 6) != 0) return FA_ERROR_UNSUPPORTED_FORMAT; /* multi-disk */
    /* Without a locator the values are real ones (an archive of exactly 65,535 entries). */
    if ((*total == 0xFFFF || *cd_size == 0xFFFFFFFFu || *cd_at == 0xFFFFFFFFu) && end >= ZIP64_LOCATOR_SIZE &&
        rd32(tail + end - ZIP64_LOCATOR_SIZE) == ZIP64_LOCATOR_SIG) {
        const uint8_t* loc = tail + end - ZIP64_LOCATOR_SIZE;
        if (rd32(loc + 4) != 0 || rd32(loc + 16) != 1) return FA_ERROR_UNSUPPORTED_FORMAT;
        z_at = rd64(loc + 8);
        if (z_at > limit - ZIP64_LOCATOR_SIZE || limit - ZIP64_LOCATOR_SIZE - z_at < ZIP64_END_SIZE)
            return FA_ERROR_UNSUPPORTED_FORMAT;
        r = read_at(src, (int64_t)z_at, z, sizeof(z));
        if (r != FA_OK) return r;
        if (rd32(z) != ZIP64_END_SIG || rd32(z + 16) != 0 || rd32(z + 20) != 0 || rd64(z + 24) != rd64(z + 32))
            return FA_ERROR_UNSUPPORTED_FORMAT;
        *total = rd64(z + 32);
        *cd_size = rd64(z + 40);
        *cd_at = rd64(z + 48);
        limit = z_at;
    }
    if (*cd_at > limit || *cd_size > limit - *cd_at) return FA_ERROR_UNSUPPORTED_FORMAT;
    return FA_OK;
}

/* Replaces the 32-bit sizes and offset that read 0xFFFFFFFF by the ZIP64 extra
 * field's (0x0001): in the order uncompressed size, compressed size, header offset,
 * each present only when its 32-bit field is 0xFFFFFFFF. */
static int apply_zip64_extra(fa_zip_entry* e, const uint8_t* extra, size_t extra_len, int need_size,
                             int need_comp, int need_offset)
{
    if (!need_size && !need_comp && !need_offset) return 0;
    while (extra_len >= 4) {
        uint32_t id = rd16(extra), len = rd16(extra + 2);
        if (len > extra_len - 4) return -1;
        if (id == 0x0001) {
            const uint8_t* f = extra + 4;
            uint32_t left = len;
            if (need_size) { if (left < 8) return -1; e->size = rd64(f); f += 8; left -= 8; }
            if (need_comp) { if (left < 8) return -1; e->compressed_size = rd64(f); f += 8; left -= 8; }
            if (need_offset) { if (left < 8) return -1; e->header_offset = rd64(f); }
            return 0;
        }
        extra += 4 + len;
        extra_len -= 4 + len;
    }
    return -1;
}

fa_result fa_zip_list(fa_source* src, fa_zip_entry** out_entries, uint32_t* out_count)
{
    uint8_t* tail = NULL;
    uint8_t* cd = NULL;
    size_t tail_len, end, scan_from, pos;
    int64_t len = src->length, tail_at;
    uint64_t total, i, cd_at, cd_size;
    uint32_t n = 0, cap = 0;
    fa_zip_entry* entries = NULL;
    fa_result r;
    *out_entries = NULL;
    *out_count = 0;
    if (len < 0 || !src->seekable) return FA_ERROR_NOT_SEEKABLE;
    if (len < ZIP_END_SIZE) return FA_ERROR_UNSUPPORTED_FORMAT;
    /* The end record is the last thing in the file, followed only by a comment of
     * at most 64 KB; the ZIP64 locator sits right before it: only that much of the
     * end is read. */
    tail_len = len > ZIP_END_SIZE + ZIP64_LOCATOR_SIZE + 0xFFFF ? ZIP_END_SIZE + ZIP64_LOCATOR_SIZE + 0xFFFF : (size_t)len;
    tail_at = len - (int64_t)tail_len;
    tail = (uint8_t*)malloc(tail_len);
    if (!tail) return FA_ERROR_OUT_OF_MEMORY;
    r = read_at(src, tail_at, tail, tail_len);
    if (r != FA_OK) goto done;
    r = FA_ERROR_UNSUPPORTED_FORMAT;
    end = tail_len - ZIP_END_SIZE;
    scan_from = 0;
    for (;;) {
        if (rd32(tail + end) == ZIP_END_SIG && end + ZIP_END_SIZE + rd16(tail + end + 20) == tail_len) break;
        if (end == scan_from) goto done;
        end--;
    }
    r = find_directory(src, tail, end, tail_at, &total, &cd_at, &cd_size);
    if (r != FA_OK) goto done;
    r = FA_ERROR_UNSUPPORTED_FORMAT;
    /* A real directory holds every entry it counts; karaoke ones are a few hundred bytes. */
    if (total > cd_size / ZIP_CENTRAL_SIZE || cd_size > ZIP_MAX_DIRECTORY || total > UINT32_MAX) goto done;
    cd = (uint8_t*)malloc(cd_size ? (size_t)cd_size : 1);
    if (!cd) { r = FA_ERROR_OUT_OF_MEMORY; goto done; }
    r = read_at(src, (int64_t)cd_at, cd, (size_t)cd_size);
    if (r != FA_OK) goto done;
    r = FA_ERROR_UNSUPPORTED_FORMAT;
    pos = 0;
    for (i = 0; i < total; i++) {
        const uint8_t* h = cd + pos;
        uint32_t flags, name_len, extra_len, skip, comp32, size32, off32;
        fa_zip_entry* e;
        if (cd_size - pos < ZIP_CENTRAL_SIZE || rd32(h) != ZIP_CENTRAL_SIG) goto done;
        flags = rd16(h + 8);
        name_len = rd16(h + 28);
        extra_len = rd16(h + 30);
        skip = ZIP_CENTRAL_SIZE + name_len + extra_len + rd16(h + 32);
        if (cd_size - pos < skip) goto done;
        /* Directories and encrypted entries are left out. */
        if (name_len == 0 || h[ZIP_CENTRAL_SIZE + name_len - 1] == '/' || (flags & 1)) { pos += skip; continue; }
        if (n == cap) {
            fa_zip_entry* grown;
            cap = cap ? cap * 2 : 16;
            if (cap > total) cap = (uint32_t)total;
            grown = (fa_zip_entry*)realloc(entries, (size_t)cap * sizeof(*entries));
            if (!grown) { r = FA_ERROR_OUT_OF_MEMORY; goto done; }
            entries = grown;
        }
        e = &entries[n];
        memset(e, 0, sizeof(*e));
        e->method = rd16(h + 10);
        e->crc32 = rd32(h + 16);
        comp32 = rd32(h + 20);
        size32 = rd32(h + 24);
        off32 = rd32(h + 42);
        e->compressed_size = comp32;
        e->size = size32;
        e->header_offset = off32;
        if (apply_zip64_extra(e, h + ZIP_CENTRAL_SIZE + name_len, extra_len, size32 == 0xFFFFFFFFu,
                              comp32 == 0xFFFFFFFFu, off32 == 0xFFFFFFFFu) != 0)
            goto done;
        if (name_len >= sizeof(e->name)) name_len = sizeof(e->name) - 1;
        memcpy(e->name, h + ZIP_CENTRAL_SIZE, name_len);
        e->name[name_len] = '\0';
        pos += skip;
        n++;
    }
    *out_entries = entries;
    *out_count = n;
    entries = NULL;
    r = FA_OK;
done:
    free(entries);
    free(cd);
    free(tail);
    return r;
}

fa_result fa_zip_entry_offset(fa_source* src, const fa_zip_entry* e, int64_t* out_offset)
{
    uint8_t h[ZIP_LOCAL_SIZE];
    uint64_t data_pos, len = (uint64_t)src->length;
    fa_result r;
    *out_offset = 0;
    if (src->length < 0 || e->header_offset > len || len - e->header_offset < ZIP_LOCAL_SIZE) return FA_ERROR_UNSUPPORTED_FORMAT;
    r = read_at(src, (int64_t)e->header_offset, h, sizeof(h));
    if (r != FA_OK) return r;
    if (rd32(h) != ZIP_LOCAL_SIG) return FA_ERROR_UNSUPPORTED_FORMAT;
    data_pos = e->header_offset + ZIP_LOCAL_SIZE + rd16(h + 26) + rd16(h + 28);
    if (data_pos > len || len - data_pos < e->compressed_size) return FA_ERROR_UNSUPPORTED_FORMAT;
    if (e->method != 0 && e->method != 8) return FA_ERROR_UNSUPPORTED_FORMAT;
    if (e->method == 0 && e->compressed_size != e->size) return FA_ERROR_UNSUPPORTED_FORMAT;
    /* DEFLATE expands at most 1032:1 (258-byte matches in about two bits each), so a
     * larger declared size is a lie: refuse it before allocating. */
    if (e->method == 8 && e->size / 1032 > e->compressed_size + 1024) return FA_ERROR_UNSUPPORTED_FORMAT;
    *out_offset = (int64_t)data_pos;
    return FA_OK;
}

/* Unpacks entry `e` into `out` (out_cap bytes; the whole entry, or INF_OUT_CHUNK
 * when writing to `sink`), CRC checked. */
static fa_result unpack(fa_source* src, const fa_zip_entry* e, uint8_t* out, size_t out_cap, FILE* sink)
{
    inf_state s;
    int64_t data_pos;
    fa_result r = fa_zip_entry_offset(src, e, &data_pos);
    if (r != FA_OK) return r;
    if (fa_source_seek(src, data_pos) != FA_OK) return FA_ERROR_IO;
    memset(&s, 0, sizeof(s));
    s.out = out;
    s.out_cap = out_cap;
    s.total = e->size;
    s.sink = sink;
    if (src->kind == FA_SOURCE_MEMORY) {
        s.in = src->mem + data_pos;   /* no copy of a block already in memory */
        s.in_size = (size_t)e->compressed_size;
    } else {
        s.src = src;
        s.src_left = e->compressed_size;
        s.in_buf = (uint8_t*)malloc(INF_IN_CHUNK);
        if (!s.in_buf) return FA_ERROR_OUT_OF_MEMORY;
    }
    if (e->method == 8) {
        r = inf_run(&s);
    } else {
        r = inf_copy(&s, e->size) == 0 ? FA_OK : s.io_error ? FA_ERROR_IO : FA_ERROR_UNSUPPORTED_FORMAT;
    }
    free(s.in_buf);
    if (r == FA_OK && sink) {
        if (fwrite(s.out, 1, s.out_pos, sink) != s.out_pos || fflush(sink) != 0) r = FA_ERROR_IO;
    }
    if (r == FA_OK && crc32_update(s.crc, s.out, s.out_pos) != e->crc32) r = FA_ERROR_UNSUPPORTED_FORMAT;
    return r;
}

fa_result fa_zip_extract(fa_source* src, const fa_zip_entry* e, uint64_t max_size, uint8_t** out_data)
{
    uint8_t* out;
    fa_result r;
    *out_data = NULL;
    if (e->size > max_size || e->size > (uint64_t)(SIZE_MAX - 1)) return FA_ERROR_TOO_LARGE;
    out = (uint8_t*)malloc(e->size ? (size_t)e->size : 1);
    if (!out) return FA_ERROR_OUT_OF_MEMORY;
    r = unpack(src, e, out, (size_t)e->size, NULL);
    if (r != FA_OK) { free(out); return r; }
    *out_data = out;
    return FA_OK;
}

fa_result fa_zip_extract_file(fa_source* src, const fa_zip_entry* e, FILE* f)
{
    uint8_t* out = (uint8_t*)malloc(INF_OUT_CHUNK);
    fa_result r;
    if (!out) return FA_ERROR_OUT_OF_MEMORY;
    r = unpack(src, e, out, INF_OUT_CHUNK, f);
    free(out);
    return r;
}
