/* Tag readers: ID3v2.2/2.3/2.4, ID3v1, APEv2, FLAC metadata blocks, MP4 (iTunes
 * ilst atoms), RIFF INFO, AIFF text chunks and Matroska tags and attachments.
 * Vorbis comments and FLAC pictures are parsed in tags.c, Ogg headers in
 * demux_ogg.c and ASF objects in demux_asf.c. Every reader delivers to a
 * fa_tag_sink (fa_internal.h) and reads only the parts the sink wants: with no
 * picture callback, pictures are skipped with a seek.
 *
 * Written from the public format documents: id3.org's ID3v2.2.0, ID3v2.3.0 and
 * ID3v2.4.0 structure and frame documents, the APEv2 specification on
 * wiki.hydrogenaud.io, the FLAC format (RFC 9639), Apple's QuickTime File Format
 * metadata atoms, the RIFF INFO list (Multimedia Programming Interface and Data
 * Specifications 1.0), the AIFF 1.3 specification and Matroska (RFC 9559). */
#include "fa_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG_MAX_FRAME (64u << 20)     /* one ID3 frame, MP4 atom or attachment read into memory */
#define TAG_MAX_TEXT (16u << 20)      /* a whole Vorbis comment block or ilst entry */
#define MKV_MAX_ELEMENTS 200000       /* top-level elements visited before giving up */

static int read_exact(fa_source* src, void* buf, size_t n) { return fa_source_read(src, buf, n) == (int64_t)n; }
static uint32_t synchsafe(const uint8_t* p) { return ((uint32_t)(p[0] & 0x7f) << 21) | ((uint32_t)(p[1] & 0x7f) << 14) | ((uint32_t)(p[2] & 0x7f) << 7) | (p[3] & 0x7f); }

/* Reads n bytes at `pos` into a malloc'ed, NUL-terminated block. */
static uint8_t* read_block(fa_source* src, int64_t pos, size_t n)
{
    uint8_t* b;
    if (n > TAG_MAX_FRAME || fa_source_seek(src, pos) != FA_OK) return NULL;
    b = (uint8_t*)malloc(n + 2);
    if (!b) return NULL;
    if (!read_exact(src, b, n)) { free(b); return NULL; }
    b[n] = b[n + 1] = 0;
    return b;
}

/* ---- text ------------------------------------------------------------------------- */

static int valid_utf8(const uint8_t* s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        size_t m, k;
        if (c < 0x80) { i++; continue; }
        m = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0;
        if (m == 0 || i + m >= n) return 0;
        for (k = 1; k <= m; k++) if ((s[i + k] & 0xC0) != 0x80) return 0;
        i += m + 1;
    }
    return 1;
}

/* Up to the first NUL: UTF-8 as it is, else Latin-1 converted. Malloc'ed. */
static char* text_8bit(const uint8_t* p, size_t n, int latin1)
{
    size_t len = 0, i, k = 0;
    char* out;
    while (len < n && p[len]) len++;
    if (!latin1 && valid_utf8(p, len)) {
        out = (char*)malloc(len + 1);
        if (!out) return NULL;
        memcpy(out, p, len);
        out[len] = '\0';
        return out;
    }
    out = (char*)malloc(len * 2 + 1);
    if (!out) return NULL;
    for (i = 0; i < len; i++) {
        uint8_t c = p[i];
        if (c < 0x80) out[k++] = (char)c;
        else { out[k++] = (char)(0xC0 | (c >> 6)); out[k++] = (char)(0x80 | (c & 63)); }
    }
    out[k] = '\0';
    return out;
}

static size_t put_utf8(char* out, uint32_t u)
{
    if (u < 0x80) { out[0] = (char)u; return 1; }
    if (u < 0x800) { out[0] = (char)(0xC0 | (u >> 6)); out[1] = (char)(0x80 | (u & 63)); return 2; }
    if (u < 0x10000) {
        out[0] = (char)(0xE0 | (u >> 12)); out[1] = (char)(0x80 | ((u >> 6) & 63)); out[2] = (char)(0x80 | (u & 63));
        return 3;
    }
    out[0] = (char)(0xF0 | (u >> 18)); out[1] = (char)(0x80 | ((u >> 12) & 63));
    out[2] = (char)(0x80 | ((u >> 6) & 63)); out[3] = (char)(0x80 | (u & 63));
    return 4;
}

/* UTF-16 (`big` endian unless a BOM says otherwise) to UTF-8 up to a NUL. Malloc'ed. */
static char* text_utf16(const uint8_t* p, size_t n, int big)
{
    size_t i = 0, k = 0;
    char* out = (char*)malloc(n / 2 * 3 + 1);
    if (!out) return NULL;
    while (i + 1 < n) {
        uint32_t u = big ? fa_be16(p + i) : fa_le16(p + i);
        i += 2;
        if (u == 0) break;
        if (u == 0xFEFF) continue;
        if (u == 0xFFFE) { big = !big; continue; }
        if (u >= 0xD800 && u < 0xDC00 && i + 1 < n) {
            uint32_t lo = big ? fa_be16(p + i) : fa_le16(p + i);
            if (lo >= 0xDC00 && lo < 0xE000) { u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00); i += 2; }
            else u = 0xFFFD;
        } else if (u >= 0xD800 && u < 0xE000) {
            u = 0xFFFD;
        }
        k += put_utf8(out + k, u);
    }
    out[k] = '\0';
    return out;
}

/* ---- ID3v2 ------------------------------------------------------------------------- */

/* The size of a string in ID3 encoding `enc` including its terminator (or up to n). */
static size_t id3_strlen(int enc, const uint8_t* p, size_t n)
{
    size_t i = 0;
    if (enc == 1 || enc == 2) {
        while (i + 1 < n && (p[i] || p[i + 1])) i += 2;
        return i + 1 < n ? i + 2 : n;
    }
    while (i < n && p[i]) i++;
    return i < n ? i + 1 : n;
}

static char* id3_string(int enc, const uint8_t* p, size_t n)
{
    if (enc == 1) return text_utf16(p, n, 0);
    if (enc == 2) return text_utf16(p, n, 1);
    return text_8bit(p, n, enc == 0);
}

/* Every string of an ID3 text field, joined with "; " (ID3v2.4 lists, empty
 * strings dropped). Malloc'ed. */
static char* id3_strings(int enc, const uint8_t* p, size_t n)
{
    size_t pos = 0, k = 0, cap = n * 3 + 1;
    char* out = (char*)malloc(cap);
    if (!out) return NULL;
    out[0] = '\0';
    while (pos < n) {
        size_t len = id3_strlen(enc, p + pos, n - pos);
        char* s = id3_string(enc, p + pos, len);
        if (s && s[0]) {
            size_t m = strlen(s);
            if (k + m + 3 > cap) { free(s); break; }
            if (k) { out[k++] = ';'; out[k++] = ' '; }
            memcpy(out + k, s, m + 1);
            k += m;
        }
        free(s);
        pos += len;
    }
    return out;
}

/* ID3v2.2 frame IDs as their ID3v2.3 equivalents, so keys are the same for both. */
static const char* const id3v22_ids[][2] = {
    { "TT1", "TIT1" }, { "TT2", "TIT2" }, { "TT3", "TIT3" }, { "TP1", "TPE1" }, { "TP2", "TPE2" },
    { "TP3", "TPE3" }, { "TP4", "TPE4" }, { "TAL", "TALB" }, { "TCM", "TCOM" }, { "TCO", "TCON" },
    { "TYE", "TYER" }, { "TRK", "TRCK" }, { "TPA", "TPOS" }, { "TCR", "TCOP" }, { "TSS", "TSSE" },
    { "TEN", "TENC" }, { "TBP", "TBPM" }, { "TXT", "TEXT" }, { "TLA", "TLAN" }, { "TPB", "TPUB" },
    { "TOA", "TOPE" }, { "TOT", "TOAL" }, { "TOR", "TORY" }, { "TDA", "TDAT" }, { "TKE", "TKEY" },
    { "TXX", "TXXX" }, { "COM", "COMM" }, { "ULT", "USLT" }, { "WXX", "WXXX" }, { "PIC", "APIC" },
    { "WAR", "WOAR" }, { "WAS", "WOAS" }, { "WAF", "WOAF" }, { "WCM", "WCOM" }, { "WCP", "WCOP" },
    { "WPB", "WPUB" }
};

static void id3_frame(int ver, const char* id, const uint8_t* p, size_t n, fa_tag_sink* sink)
{
    int enc;
    if (n == 0) return;
    if (id[0] == 'T') {
        enc = p[0];
        if (strcmp(id, "TXXX") == 0) {
            size_t dl = id3_strlen(enc, p + 1, n - 1);
            char* desc = id3_string(enc, p + 1, dl);
            char* val = id3_strings(enc, p + 1 + dl, n - 1 - dl);
            if (desc && val && desc[0]) sink->text(sink, FA_TAG_FORMAT_ID3V2, desc, val);
            free(desc);
            free(val);
        } else {
            char* val = id3_strings(enc, p + 1, n - 1);
            if (val) sink->text(sink, FA_TAG_FORMAT_ID3V2, id, val);
            free(val);
        }
    } else if (id[0] == 'W') {
        char* url;
        if (strcmp(id, "WXXX") == 0) {
            size_t dl;
            enc = p[0];
            dl = 1 + id3_strlen(enc, p + 1, n - 1);
            url = dl < n ? text_8bit(p + dl, n - dl, 1) : NULL;
        } else {
            url = text_8bit(p, n, 1);
        }
        if (url) sink->text(sink, FA_TAG_FORMAT_ID3V2, id, url);
        free(url);
    } else if (strcmp(id, "COMM") == 0 || strcmp(id, "USLT") == 0) {
        size_t dl;
        char *desc, *val;
        if (n < 4) return;
        enc = p[0];
        dl = id3_strlen(enc, p + 4, n - 4);
        desc = id3_string(enc, p + 4, dl);
        val = id3_strings(enc, p + 4 + dl, n - 4 - dl);
        if (desc && val) {
            char key[300];
            if (desc[0]) snprintf(key, sizeof(key), "%s:%s", id, desc);
            else snprintf(key, sizeof(key), "%s", id);
            sink->text(sink, FA_TAG_FORMAT_ID3V2, key, val);
        }
        free(desc);
        free(val);
    } else if (strcmp(id, "APIC") == 0 && sink->picture) {
        char mime[64];
        size_t pos = 1, dl;
        int type;
        char* desc;
        enc = p[0];
        if (ver == 2) {
            /* PIC: a three-letter image format instead of a MIME type. */
            if (n < 5) return;
            snprintf(mime, sizeof(mime), "%s", memcmp(p + 1, "PNG", 3) == 0 ? "image/png" :
                     memcmp(p + 1, "JPG", 3) == 0 ? "image/jpeg" : "");
            pos = 4;
        } else {
            size_t ml = 0;
            while (pos + ml < n && p[pos + ml]) ml++;
            if (pos + ml >= n) return;
            snprintf(mime, sizeof(mime), "%.*s", (int)(ml < 63 ? ml : 63), (const char*)(p + pos));
            pos += ml + 1;
        }
        if (pos >= n) return;
        type = p[pos++];
        dl = id3_strlen(enc, p + pos, n - pos);
        desc = id3_string(enc, p + pos, dl);
        pos += dl;
        if (desc && pos <= n) sink->picture(sink, FA_TAG_FORMAT_ID3V2, type, mime, desc, p + pos, n - pos);
        free(desc);
    }
}

/* Removes the 0x00 that unsynchronisation puts after every 0xFF. Returns the new size. */
static size_t id3_unsync(uint8_t* p, size_t n)
{
    size_t i, k = 0;
    for (i = 0; i < n; i++) {
        p[k++] = p[i];
        if (p[i] == 0xFF && i + 1 < n && p[i + 1] == 0x00) i++;
    }
    return k;
}

/* The frames of a tag in [pos, end) of `src`. */
static void id3_frames(fa_source* src, int64_t pos, int64_t end, int ver, int unsync_all, fa_tag_sink* sink)
{
    int hl = ver == 2 ? 6 : 10;
    while (pos + hl <= end) {
        uint8_t fh[10];
        char id[5];
        int64_t fsize;
        int flags = 0, is_pic, wanted;
        if (fa_source_seek(src, pos) != FA_OK || !read_exact(src, fh, (size_t)hl)) return;
        if (fh[0] == 0) return;                          /* padding */
        if (ver == 2) {
            size_t i;
            fsize = fa_be24(fh + 3);
            memcpy(id, fh, 3);
            id[3] = '\0';
            for (i = 0; i < sizeof(id3v22_ids) / sizeof(id3v22_ids[0]); i++)
                if (memcmp(fh, id3v22_ids[i][0], 3) == 0) { memcpy(id, id3v22_ids[i][1], 5); break; }
        } else {
            memcpy(id, fh, 4);
            id[4] = '\0';
            /* ID3v2.4 sizes are synchsafe, but some writers store them plain. */
            fsize = ver == 4 && !((fh[4] | fh[5] | fh[6] | fh[7]) & 0x80) ? synchsafe(fh + 4) : fa_be32(fh + 4);
            if (ver == 4 && pos + hl + fsize + hl <= end && fsize >= 0x80) {
                uint8_t nx[4];
                int64_t plain = fa_be32(fh + 4);
                /* If the synchsafe size leads nowhere but the plain one leads to a frame, take that. */
                if (fa_source_seek(src, pos + hl + fsize) == FA_OK && read_exact(src, nx, 4) &&
                    !((nx[0] >= 'A' && nx[0] <= 'Z') || (nx[0] >= '0' && nx[0] <= '9') || nx[0] == 0) &&
                    pos + hl + plain <= end && fa_source_seek(src, pos + hl + plain) == FA_OK && read_exact(src, nx, 4) &&
                    ((nx[0] >= 'A' && nx[0] <= 'Z') || nx[0] == 0))
                    fsize = plain;
            }
            flags = fh[9];
        }
        if (fsize <= 0 || pos + hl + fsize > end) return;
        is_pic = strcmp(id, "APIC") == 0;
        wanted = is_pic ? sink->picture != NULL && fsize <= (int64_t)TAG_MAX_FRAME
                        : (id[0] == 'T' || id[0] == 'W' || !strcmp(id, "COMM") || !strcmp(id, "USLT")) &&
                          (uint64_t)fsize <= (uint64_t)sink->max_text + 64;
        /* Compressed or encrypted frames are not read. */
        if (ver == 3 && (flags & 0xC0)) wanted = 0;
        if (ver == 4 && (flags & 0x0C)) wanted = 0;
        if (wanted) {
            uint8_t* body = read_block(src, pos + hl, (size_t)fsize);
            if (body) {
                uint8_t* p = body;
                size_t n = (size_t)fsize;
                if (ver == 3 && (flags & 0x20) && n > 0) { p++; n--; }               /* group ID */
                if (ver == 4) {
                    if ((flags & 0x40) && n > 0) { p++; n--; }                     /* group ID */
                    if ((flags & 0x01) && n >= 4) { p += 4; n -= 4; }              /* data length */
                    if ((flags & 0x02) || unsync_all) n = id3_unsync(p, n);
                }
                id3_frame(ver, id, p, n, sink);
                free(body);
            }
        }
        pos += hl + fsize;
    }
}

/* The ID3v2 tag at `at`: delivered to `sink` (NULL = only measured). Returns its
 * whole size, header and footer included, 0 if there is none. */
static int64_t read_id3v2(fa_source* src, int64_t at, fa_tag_sink* sink)
{
    uint8_t h[10];
    int ver, flags;
    int64_t size, pos, end;
    if (fa_source_seek(src, at) != FA_OK || !read_exact(src, h, 10)) return 0;
    if (memcmp(h, "ID3", 3) != 0 || h[3] < 2 || h[3] > 4 || ((h[6] | h[7] | h[8] | h[9]) & 0x80)) return 0;
    ver = h[3];
    flags = h[5];
    size = synchsafe(h + 6);
    end = at + 10 + size;
    if (!sink) return 10 + size + ((flags & 0x10) ? 10 : 0);
    pos = at + 10;
    if (ver < 4 && (flags & 0x80)) {
        /* The whole tag is unsynchronised: undo it in memory, then read from there. */
        uint8_t* body = read_block(src, pos, (size_t)size);
        if (body) {
            fa_source mem;
            size_t n = id3_unsync(body, (size_t)size);
            size_t skip = 0;
            if (ver == 3 && (flags & 0x40) && n >= 4) skip = 4 + fa_be32(body);
            if (skip < n && fa_source_open_memory_ref(&mem, body + skip, n - skip) == FA_OK) {
                id3_frames(&mem, 0, (int64_t)(n - skip), ver, 0, sink);
                fa_source_close(&mem);
            }
            free(body);
        }
    } else {
        if (ver >= 3 && (flags & 0x40)) {                /* extended header */
            uint8_t e[4];
            if (!read_exact(src, e, 4)) return end - at;
            pos += ver == 4 ? synchsafe(e) : (int64_t)fa_be32(e) + 4;
        }
        id3_frames(src, pos, end, ver, ver == 4 && (flags & 0x80), sink);
    }
    return 10 + size + ((flags & 0x10) ? 10 : 0);
}

/* ---- ID3v1 ------------------------------------------------------------------------- */

static void v1_field(fa_tag_sink* sink, const char* key, const uint8_t* p, size_t n)
{
    char buf[31];
    char* s;
    while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == 0)) n--;
    if (n == 0) return;
    memcpy(buf, p, n);
    buf[n] = '\0';
    s = text_8bit((const uint8_t*)buf, n, 1);
    if (s) sink->text(sink, FA_TAG_FORMAT_ID3V1, key, s);
    free(s);
}

/* Returns 128 if the file ends with an ID3v1 tag, else 0. */
static int64_t read_id3v1(fa_source* src, fa_tag_sink* sink)
{
    uint8_t t[128];
    if (src->length < 128 || fa_source_seek(src, src->length - 128) != FA_OK || !read_exact(src, t, 128)) return 0;
    if (memcmp(t, "TAG", 3) != 0) return 0;
    if (!sink) return 128;
    v1_field(sink, "TITLE", t + 3, 30);
    v1_field(sink, "ARTIST", t + 33, 30);
    v1_field(sink, "ALBUM", t + 63, 30);
    v1_field(sink, "YEAR", t + 93, 4);
    if (t[125] == 0 && t[126] != 0) {                    /* ID3v1.1: a track number */
        char num[8];
        v1_field(sink, "COMMENT", t + 97, 28);
        snprintf(num, sizeof(num), "%d", t[126]);
        sink->text(sink, FA_TAG_FORMAT_ID3V1, "TRACK", num);
    } else {
        v1_field(sink, "COMMENT", t + 97, 30);
    }
    if (t[127] != 255) {
        char num[8];
        snprintf(num, sizeof(num), "%d", t[127]);
        sink->text(sink, FA_TAG_FORMAT_ID3V1, "GENRE", num);
    }
    return 128;
}

/* ---- APEv2 ------------------------------------------------------------------------- */

/* The APE tag whose footer is at `footer_pos`. Returns the tag's whole size
 * (header included), 0 if there is no footer there. */
static int64_t read_ape_at(fa_source* src, int64_t footer_pos, fa_tag_sink* sink)
{
    uint8_t f[32];
    uint32_t size, count, i, tflags;
    int64_t pos, total;
    if (footer_pos < 0 || fa_source_seek(src, footer_pos) != FA_OK || !read_exact(src, f, 32)) return 0;
    if (memcmp(f, "APETAGEX", 8) != 0) return 0;
    size = fa_le32(f + 12);                  /* items + footer */
    count = fa_le32(f + 16);
    tflags = fa_le32(f + 20);
    if (size < 32 || (int64_t)size > footer_pos + 32) return 0;
    total = (int64_t)size + ((tflags & 0x80000000u) ? 32 : 0);
    if (!sink) return total;
    pos = footer_pos + 32 - (int64_t)size;
    for (i = 0; i < count && pos < footer_pos; i++) {
        uint8_t ih[8];
        char key[256];
        size_t k = 0;
        uint32_t vlen, iflags;
        if (fa_source_seek(src, pos) != FA_OK || !read_exact(src, ih, 8)) break;
        vlen = fa_le32(ih);
        iflags = fa_le32(ih + 4);
        pos += 8;
        for (;;) {                        /* key, NUL-terminated */
            uint8_t c;
            if (!read_exact(src, &c, 1)) return total;
            pos++;
            if (c == 0) break;
            if (k + 1 < sizeof(key)) key[k++] = (char)c;
        }
        key[k] = '\0';
        if (pos + vlen > footer_pos) break;
        if (((iflags >> 1) & 3) == 0 && vlen <= sink->max_text) {
            uint8_t* v = read_block(src, pos, vlen);
            if (v) {
                /* A list of values is separated by NULs. */
                uint32_t j;
                char* s;
                for (j = 0; j + 1 < vlen; j++) if (v[j] == 0) v[j] = 1;
                s = text_8bit(v, vlen, 0);
                if (s) {
                    char* out = (char*)malloc(strlen(s) * 2 + 1);
                    if (out) {
                        size_t a, b = 0;
                        for (a = 0; s[a]; a++) {
                            if (s[a] == 1) { out[b++] = ';'; out[b++] = ' '; }
                            else out[b++] = s[a];
                        }
                        out[b] = '\0';
                        sink->text(sink, FA_TAG_FORMAT_APE, key, out);
                        free(out);
                    }
                    free(s);
                }
                free(v);
            }
        } else if (((iflags >> 1) & 3) == 1 && sink->picture && vlen <= TAG_MAX_FRAME &&
                   fa_tags_key_is(key, "Cover Art (Front)") + fa_tags_key_is(key, "Cover Art (Back)") +
                   fa_tags_key_is(key, "Cover Art (Other)")) {
            /* Binary: a file name, NUL, the image. */
            uint8_t* v = read_block(src, pos, vlen);
            if (v) {
                uint32_t nl = 0;
                while (nl < vlen && v[nl]) nl++;
                if (nl < vlen) {
                    const uint8_t* img = v + nl + 1;
                    size_t isz = vlen - nl - 1;
                    int type = fa_tags_key_is(key, "Cover Art (Front)") ? FA_PICTURE_FRONT_COVER :
                               fa_tags_key_is(key, "Cover Art (Back)") ? FA_PICTURE_BACK_COVER : FA_PICTURE_OTHER;
                    sink->picture(sink, FA_TAG_FORMAT_APE, type, fa_tags_image_mime(img, isz), "", img, isz);
                }
                free(v);
            }
        }
        pos += vlen;
    }
    return total;
}

/* The APE tag at the end of the file (before an ID3v1 tag if there is one).
 * Returns its size, 0 if none. */
static int64_t read_ape(fa_source* src, int64_t v1, fa_tag_sink* sink)
{
    if (src->length < 32 + v1) return 0;
    return read_ape_at(src, src->length - v1 - 32, sink);
}

/* ---- FLAC -------------------------------------------------------------------------- */

/* The metadata blocks of a FLAC file at `start`. Returns the offset of the first
 * frame (0 if not FLAC); *bits = bits per sample from STREAMINFO. */
static int64_t read_flac(fa_source* src, int64_t start, fa_tag_sink* sink, uint32_t* bits)
{
    uint8_t h[4];
    int64_t pos = start + 4;
    if (fa_source_seek(src, start) != FA_OK || !read_exact(src, h, 4) || memcmp(h, "fLaC", 4) != 0) return 0;
    for (;;) {
        int last, type;
        uint32_t len;
        if (fa_source_seek(src, pos) != FA_OK || !read_exact(src, h, 4)) return 0;
        last = h[0] & 0x80;
        type = h[0] & 0x7f;
        len = fa_be24(h + 1);
        if (type == 0 && len >= 34 && bits) {
            uint8_t si[34];
            if (read_exact(src, si, 34)) *bits = (uint32_t)(((si[12] & 1) << 4) | (si[13] >> 4)) + 1;
        } else if (type == 4 && sink && len <= TAG_MAX_TEXT) {
            uint8_t* buf = read_block(src, pos + 4, len);
            if (buf) fa_tags_parse_vorbis_comments(buf, len, sink);
            free(buf);
        } else if (type == 6 && sink && sink->picture && len <= TAG_MAX_FRAME) {
            uint8_t* buf = read_block(src, pos + 4, len);
            if (buf) fa_tags_parse_flac_picture(buf, len, FA_TAG_FORMAT_VORBIS, sink);
            free(buf);
        }
        pos += 4 + (int64_t)len;
        if (last || type == 127) return pos;
    }
}

/* ---- MP4 --------------------------------------------------------------------------- */

typedef struct mp4_box {
    uint32_t type;
    int64_t pos, data, end;     /* header, body and end offsets */
} mp4_box;

static int mp4_next(fa_source* src, int64_t pos, int64_t end, mp4_box* b)
{
    uint8_t h[16];
    uint64_t size;
    int64_t hl = 8;
    if (pos + 8 > end || fa_source_seek(src, pos) != FA_OK || !read_exact(src, h, 8)) return 0;
    size = fa_be32(h);
    b->type = fa_be32(h + 4);
    if (size == 1) {
        if (!read_exact(src, h + 8, 8)) return 0;
        size = ((uint64_t)fa_be32(h + 8) << 32) | fa_be32(h + 12);
        hl = 16;
    } else if (size == 0) {
        size = (uint64_t)(end - pos);
    }
    if (size < (uint64_t)hl || size > (uint64_t)(end - pos)) return 0;
    b->pos = pos;
    b->data = pos + hl;
    b->end = pos + (int64_t)size;
    return 1;
}

static int mp4_find(fa_source* src, int64_t pos, int64_t end, uint32_t type, mp4_box* out)
{
    mp4_box b;
    while (mp4_next(src, pos, end, &b)) {
        if (b.type == type) { *out = b; return 1; }
        pos = b.end;
    }
    return 0;
}

/* An ilst entry's key: Mac Roman 0xA9 ("©") as UTF-8, other bytes as they are. */
static void mp4_key(uint32_t type, char* out)
{
    int i, k = 0;
    for (i = 3; i >= 0; i--) {
        uint8_t c = (uint8_t)(type >> (8 * i));
        if (c == 0xA9) { out[k++] = (char)0xC2; out[k++] = (char)0xA9; }
        else out[k++] = (char)(c >= 0x20 && c < 0x7F ? c : '?');
    }
    out[k] = '\0';
}

/* One ilst entry (in memory): its data atoms, and mean / name for "----". */
static void mp4_item(uint32_t type, const uint8_t* p, size_t n, fa_tag_sink* sink)
{
    char key[260];
    size_t pos = 0;
    mp4_key(type, key);
    while (pos + 8 <= n) {
        uint32_t size = fa_be32(p + pos), atom = fa_be32(p + pos + 4);
        const uint8_t* d = p + pos + 8;
        size_t dl;
        if (size < 8 || size > n - pos) return;
        dl = size - 8;
        if (atom == FA_FOURCC('n', 'a', 'm', 'e') && dl >= 4) {
            snprintf(key, sizeof(key), "%.*s", (int)(dl - 4 < 255 ? dl - 4 : 255), (const char*)(d + 4));
        } else if (atom == FA_FOURCC('d', 'a', 't', 'a') && dl >= 8) {
            uint32_t dt = fa_be32(d) & 0xFFFFFF;
            const uint8_t* v = d + 8;
            size_t vl = dl - 8;
            char num[32];
            if (dt == 1) {
                char* s = text_8bit(v, vl, 0);
                if (s) sink->text(sink, FA_TAG_FORMAT_MP4, key, s);
                free(s);
            } else if (dt == 2) {
                char* s = text_utf16(v, vl, 1);
                if (s) sink->text(sink, FA_TAG_FORMAT_MP4, key, s);
                free(s);
            } else if ((dt == 13 || dt == 14 || dt == 27 || (dt == 0 && type == FA_FOURCC('c', 'o', 'v', 'r'))) && sink->picture) {
                const char* mime = dt == 13 ? "image/jpeg" : dt == 14 ? "image/png" : dt == 27 ? "image/bmp" : fa_tags_image_mime(v, vl);
                sink->picture(sink, FA_TAG_FORMAT_MP4, FA_PICTURE_FRONT_COVER, mime, "", v, vl);
            } else if ((dt == 0 || dt == 21) && (type == FA_FOURCC('t', 'r', 'k', 'n') || type == FA_FOURCC('d', 'i', 's', 'k')) && vl >= 6) {
                uint16_t no = fa_be16(v + 2), total = fa_be16(v + 4);
                if (total) snprintf(num, sizeof(num), "%u/%u", no, total);
                else snprintf(num, sizeof(num), "%u", no);
                if (no || total) sink->text(sink, FA_TAG_FORMAT_MP4, key, num);
            } else if ((dt == 0 || dt == 21 || dt == 22) && vl >= 1 && vl <= 8) {
                /* Integers: tmpo (BPM), gnre (ID3 genre + 1), cpil, rtng, ... */
                uint64_t u = 0;
                size_t i;
                for (i = 0; i < vl; i++) u = (u << 8) | v[i];
                if (dt == 21 && (v[0] & 0x80)) {
                    /* Sign-extend: two's complement of the stored width. */
                    if (vl < 8) u |= ~(uint64_t)0 << (8 * vl);
                    snprintf(num, sizeof(num), "%lld", (long long)u);
                }
                else snprintf(num, sizeof(num), "%llu", (unsigned long long)u);
                sink->text(sink, FA_TAG_FORMAT_MP4, key, num);
            }
        }
        pos += size;
    }
}

/* moov/udta/meta/ilst (or moov/meta/ilst): read entry by entry, so cover art
 * can be skipped without reading it. */
static void read_mp4(fa_source* src, fa_tag_sink* sink)
{
    mp4_box moov, udta, meta, ilst, it;
    int64_t len = src->length, pos;
    int found = 0;
    if (!mp4_find(src, 0, len, FA_FOURCC('m', 'o', 'o', 'v'), &moov)) return;
    if (mp4_find(src, moov.data, moov.end, FA_FOURCC('u', 'd', 't', 'a'), &udta) &&
        mp4_find(src, udta.data, udta.end, FA_FOURCC('m', 'e', 't', 'a'), &meta))
        found = 1;
    else if (mp4_find(src, moov.data, moov.end, FA_FOURCC('m', 'e', 't', 'a'), &meta))
        found = 1;
    if (!found) return;
    {
        /* ISO meta is a full box (4 bytes of version and flags); QuickTime's is not. */
        uint8_t h[8];
        int64_t first = meta.data;
        if (fa_source_seek(src, meta.data) == FA_OK && read_exact(src, h, 8) && fa_be32(h + 4) != FA_FOURCC('h', 'd', 'l', 'r')) first += 4;
        if (!mp4_find(src, first, meta.end, FA_FOURCC('i', 'l', 's', 't'), &ilst)) return;
    }
    pos = ilst.data;
    while (mp4_next(src, pos, ilst.end, &it)) {
        int64_t n = it.end - it.data;
        int pic = it.type == FA_FOURCC('c', 'o', 'v', 'r');
        if (pic ? sink->picture != NULL && n <= (int64_t)TAG_MAX_FRAME : n <= (int64_t)sink->max_text + 64) {
            uint8_t* body = read_block(src, it.data, (size_t)n);
            if (body) mp4_item(it.type, body, (size_t)n, sink);
            free(body);
        }
        pos = it.end;
    }
}

/* ---- RIFF and AIFF ----------------------------------------------------------------- */

static void text_chunk(fa_source* src, int format, const char* key, int64_t pos, uint32_t size, fa_tag_sink* sink)
{
    uint8_t* b;
    char* s;
    if (size == 0 || size > sink->max_text) return;
    b = read_block(src, pos, size);
    if (!b) return;
    s = text_8bit(b, size, 0);
    if (s && s[0]) sink->text(sink, format, key, s);
    free(s);
    free(b);
}

/* WAV/RF64 (any RIFF form, so RMID too): an "id3 " chunk first, then LIST/INFO. */
static void read_riff(fa_source* src, fa_tag_sink* sink)
{
    uint8_t h[12];
    int64_t pos = 12, end = src->length, id3 = -1, info = -1, data64 = -1;
    uint32_t info_size = 0;
    int rf64;
    int n = 0;
    if (fa_source_seek(src, 0) != FA_OK || !read_exact(src, h, 12)) return;
    rf64 = memcmp(h, "RF64", 4) == 0;
    while (pos + 8 <= end && n++ < 10000) {
        uint8_t c[8];
        int64_t size;
        if (fa_source_seek(src, pos) != FA_OK || !read_exact(src, c, 8)) break;
        size = fa_le32(c + 4);
        if (rf64 && memcmp(c, "ds64", 4) == 0 && size >= 16) {
            uint8_t d[16];
            if (read_exact(src, d, 16)) data64 = (int64_t)(((uint64_t)fa_le32(d + 12) << 32) | fa_le32(d + 8));
        }
        if (memcmp(c, "data", 4) == 0 && size == 0xFFFFFFFF && data64 >= 0) size = data64;
        if (size > end - pos) break;    /* runs past the file (also keeps pos from overflowing) */
        if (memcmp(c, "id3 ", 4) == 0 || memcmp(c, "ID3 ", 4) == 0) id3 = pos + 8;
        if (memcmp(c, "LIST", 4) == 0 && size >= 4) {
            uint8_t t[4];
            if (read_exact(src, t, 4) && memcmp(t, "INFO", 4) == 0) { info = pos + 12; info_size = (uint32_t)size - 4; }
        }
        pos += 8 + size + (size & 1);
    }
    if (id3 >= 0) read_id3v2(src, id3, sink);
    if (info >= 0) {
        int64_t p = info, e = info + info_size;
        while (p + 8 <= e) {
            uint8_t c[8];
            char key[5];
            uint32_t size;
            if (fa_source_seek(src, p) != FA_OK || !read_exact(src, c, 8)) break;
            size = fa_le32(c + 4);
            if (p + 8 + size > e) break;
            memcpy(key, c, 4);
            key[4] = '\0';
            text_chunk(src, FA_TAG_FORMAT_RIFF, key, p + 8, size, sink);
            p += 8 + size + (size & 1);
        }
    }
}

/* AIFF/AIFC: an "ID3 " chunk first, then NAME, AUTH, "(c) " and ANNO. */
static void read_aiff(fa_source* src, fa_tag_sink* sink)
{
    int64_t pos = 12, end = src->length, id3 = -1;
    int64_t at[64];
    uint32_t sz[64];
    char ids[64][5];
    int nt = 0, n = 0, i;
    while (pos + 8 <= end && n++ < 10000) {
        uint8_t c[8];
        uint32_t size;
        if (fa_source_seek(src, pos) != FA_OK || !read_exact(src, c, 8)) break;
        size = fa_be32(c + 4);
        if (memcmp(c, "ID3 ", 4) == 0 || memcmp(c, "id3 ", 4) == 0) id3 = pos + 8;
        else if ((memcmp(c, "NAME", 4) == 0 || memcmp(c, "AUTH", 4) == 0 || memcmp(c, "(c) ", 4) == 0 ||
                  memcmp(c, "ANNO", 4) == 0) && nt < 64) {
            at[nt] = pos + 8;
            sz[nt] = size;
            memcpy(ids[nt], c, 4);
            ids[nt][4] = '\0';
            nt++;
        }
        pos += 8 + (int64_t)size + (size & 1);
    }
    if (id3 >= 0) read_id3v2(src, id3, sink);
    for (i = 0; i < nt; i++) text_chunk(src, FA_TAG_FORMAT_AIFF, ids[i], at[i], sz[i], sink);
}

/* ---- Matroska ---------------------------------------------------------------------- */

#define EBML_UNKNOWN (-1)
#define MKV_SEGMENT 0x18538067u
#define MKV_SEEKHEAD 0x114D9B74u
#define MKV_SEEK 0x4DBBu
#define MKV_SEEKID 0x53ABu
#define MKV_SEEKPOS 0x53ACu
#define MKV_INFO 0x1549A966u
#define MKV_TITLE 0x7BA9u
#define MKV_TAGS 0x1254C367u
#define MKV_TAG 0x7373u
#define MKV_TARGETS 0x63C0u
#define MKV_TTV 0x68CAu
#define MKV_SIMPLETAG 0x67C8u
#define MKV_TAGNAME 0x45A3u
#define MKV_TAGSTRING 0x4487u
#define MKV_ATTACHMENTS 0x1941A469u
#define MKV_ATTACHEDFILE 0x61A7u
#define MKV_FILEDESC 0x467Eu
#define MKV_FILENAME 0x466Eu
#define MKV_FILEMIME 0x4660u
#define MKV_FILEDATA 0x465Cu
#define MKV_CLUSTER 0x1F43B675u

typedef struct ebml_el {
    uint32_t id;
    int64_t data, size;      /* size EBML_UNKNOWN when not given */
} ebml_el;

/* An element header at `pos`. */
static int ebml_read(fa_source* src, int64_t pos, ebml_el* e)
{
    uint8_t b[12];
    int n, i, len;
    int64_t got;
    uint64_t v;
    if (fa_source_seek(src, pos) != FA_OK) return 0;
    got = fa_source_read(src, b, sizeof(b));
    if (got < 2) return 0;
    for (len = 1; len <= 4 && !(b[0] & (0x80 >> (len - 1))); len++) {}
    if (len > 4 || len >= got) return 0;
    e->id = 0;
    for (i = 0; i < len; i++) e->id = (e->id << 8) | b[i];
    for (n = 1; n <= 8 && !(b[len] & (0x80 >> (n - 1))); n++) {}
    if (n > 8 || len + n > got) return 0;
    v = b[len] & (0xFF >> n);
    {
        int all_ones = v == (uint64_t)(0xFF >> n);
        for (i = 1; i < n; i++) { v = (v << 8) | b[len + i]; if (b[len + i] != 0xFF) all_ones = 0; }
        e->size = all_ones ? EBML_UNKNOWN : (int64_t)v;
    }
    e->data = pos + len + n;
    return 1;
}

/* In-memory element walk, for the bodies of Tag and SimpleTag elements. */
static int mem_el(const uint8_t** cur, const uint8_t* end, uint32_t* id, const uint8_t** body, size_t* size)
{
    const uint8_t* p = *cur;
    int len, n, i;
    uint64_t v;
    if (end - p < 2) return 0;
    for (len = 1; len <= 4 && !(p[0] & (0x80 >> (len - 1))); len++) {}
    if (len > 4 || end - p < len + 1) return 0;
    *id = 0;
    for (i = 0; i < len; i++) *id = (*id << 8) | p[i];
    for (n = 1; n <= 8 && !(p[len] & (0x80 >> (n - 1))); n++) {}
    if (n > 8 || end - p < len + n) return 0;
    v = p[len] & (0xFF >> n);
    for (i = 1; i < n; i++) v = (v << 8) | p[len + i];
    if (v > (uint64_t)(end - p - len - n)) return 0;
    *body = p + len + n;
    *size = (size_t)v;
    *cur = *body + v;
    return 1;
}

static uint64_t mem_uint(const uint8_t* p, size_t n)
{
    uint64_t v = 0;
    size_t i;
    for (i = 0; i < n && i < 8; i++) v = (v << 8) | p[i];
    return v;
}

/* A SimpleTag and the ones nested in it ("ARTIST/SORT_WITH"). */
static void mkv_simple(const uint8_t* p, size_t n, const char* prefix, int depth, fa_tag_sink* sink)
{
    const uint8_t *cur = p, *body;
    size_t size;
    uint32_t id;
    char key[256];
    char* name = NULL;
    key[0] = '\0';
    while (mem_el(&cur, p + n, &id, &body, &size)) {
        if (id == MKV_TAGNAME && !name) {
            name = text_8bit(body, size, 0);
            if (name) snprintf(key, sizeof(key), "%s%s%s", prefix, prefix[0] ? "/" : "", name);
        } else if (id == MKV_TAGSTRING && name && size <= sink->max_text) {
            char* s = text_8bit(body, size, 0);
            if (s && s[0]) sink->text(sink, FA_TAG_FORMAT_MATROSKA, key, s);
            free(s);
        } else if (id == MKV_SIMPLETAG && name && depth < 4) {
            mkv_simple(body, size, key, depth + 1, sink);
        }
    }
    free(name);
}

/* Tags: global tags only (Targets without track, edition, chapter or attachment
 * UIDs; those describe one track of a video or are muxer statistics). Album-level
 * tags (TargetTypeValue 50 and up, when the file also has track-level ones) get
 * the prefix "ALBUM/". */
static void mkv_tags(const uint8_t* p, size_t n, fa_tag_sink* sink)
{
    int pass, has_track_level = 0;
    for (pass = 0; pass < 2; pass++) {
        const uint8_t *cur = p, *tag;
        size_t tsize;
        uint32_t id;
        while (mem_el(&cur, p + n, &id, &tag, &tsize)) {
            const uint8_t *c2 = tag, *b;
            size_t bs;
            uint32_t id2;
            uint64_t ttv = 50;
            int targeted = 0;
            if (id != MKV_TAG) continue;
            while (mem_el(&c2, tag + tsize, &id2, &b, &bs)) {
                if (id2 == MKV_TARGETS) {
                    const uint8_t *c3 = b, *b3;
                    size_t s3;
                    uint32_t id3;
                    while (mem_el(&c3, b + bs, &id3, &b3, &s3)) {
                        if (id3 == MKV_TTV) ttv = mem_uint(b3, s3);
                        else if (id3 == 0x63C5 || id3 == 0x63C9 || id3 == 0x63C4 || id3 == 0x63C6) {
                            if (mem_uint(b3, s3) != 0) targeted = 1;
                        }
                    }
                }
            }
            if (targeted) continue;
            if (pass == 0) { if (ttv < 50) has_track_level = 1; continue; }
            c2 = tag;
            while (mem_el(&c2, tag + tsize, &id2, &b, &bs))
                if (id2 == MKV_SIMPLETAG) mkv_simple(b, bs, has_track_level && ttv >= 50 ? "ALBUM" : "", 0, sink);
        }
    }
}

/* One AttachedFile: an image becomes a picture (named "cover*": the front cover). */
static void mkv_attachment(fa_source* src, const ebml_el* af, fa_tag_sink* sink)
{
    int64_t pos = af->data, end = af->data + af->size, data_pos = -1, data_size = 0;
    char mime[64] = "", name[128] = "", desc[256] = "";
    ebml_el e;
    while (pos < end && ebml_read(src, pos, &e) && e.size >= 0 && e.data + e.size <= end) {
        if ((e.id == MKV_FILEMIME || e.id == MKV_FILENAME || e.id == MKV_FILEDESC) && e.size < 256) {
            uint8_t* b = read_block(src, e.data, (size_t)e.size);
            if (b) {
                char* dst = e.id == MKV_FILEMIME ? mime : e.id == MKV_FILENAME ? name : desc;
                size_t cap = e.id == MKV_FILEMIME ? sizeof(mime) : e.id == MKV_FILENAME ? sizeof(name) : sizeof(desc);
                snprintf(dst, cap, "%s", (const char*)b);
                free(b);
            }
        } else if (e.id == MKV_FILEDATA) {
            data_pos = e.data;
            data_size = e.size;
        }
        pos = e.data + e.size;
    }
    if (data_pos < 0 || strncmp(mime, "image/", 6) != 0 || data_size > (int64_t)TAG_MAX_FRAME) return;
    {
        uint8_t* b = read_block(src, data_pos, (size_t)data_size);
        if (b) {
            int type = fa_tags_key_is(name, "cover.jpg") || fa_tags_key_is(name, "cover.png") ||
                       fa_tags_key_is(name, "cover.jpeg") || fa_tags_key_is(name, "cover.webp")
                ? FA_PICTURE_FRONT_COVER : FA_PICTURE_OTHER;
            sink->picture(sink, FA_TAG_FORMAT_MATROSKA, type, mime, desc[0] ? desc : name, b, (size_t)data_size);
            free(b);
        }
    }
}

static void mkv_element(fa_source* src, const ebml_el* e, fa_tag_sink* sink, char* title, size_t title_cap)
{
    if (e->size < 0) return;
    if (e->id == MKV_TAGS && e->size <= (int64_t)TAG_MAX_TEXT) {
        uint8_t* b = read_block(src, e->data, (size_t)e->size);
        if (b) mkv_tags(b, (size_t)e->size, sink);
        free(b);
    } else if (e->id == MKV_INFO && e->size <= (1 << 20)) {
        uint8_t* b = read_block(src, e->data, (size_t)e->size);
        if (b) {
            const uint8_t *cur = b, *body;
            size_t size;
            uint32_t id;
            while (mem_el(&cur, b + e->size, &id, &body, &size))
                if (id == MKV_TITLE) snprintf(title, title_cap, "%.*s", (int)(size < title_cap ? size : title_cap - 1), (const char*)body);
            free(b);
        }
    } else if (e->id == MKV_ATTACHMENTS && sink->picture) {
        int64_t pos = e->data, end = e->data + e->size;
        ebml_el af;
        while (pos < end && ebml_read(src, pos, &af) && af.size >= 0) {
            if (af.id == MKV_ATTACHEDFILE) mkv_attachment(src, &af, sink);
            pos = af.data + af.size;
        }
    }
}

/* Top-level Info, Tags and Attachments: found by walking the segment until the
 * first cluster, and through the SeekHead (Tags are usually written after the
 * clusters). Without a SeekHead entry, clusters of known size are skipped. */
static void read_mkv(fa_source* src, fa_tag_sink* sink)
{
    ebml_el hdr, seg, e;
    int64_t pos, seg_end, want[8];
    int nwant = 0, visited_tags = 0, visited_att = 0, visited_info = 0, have_seek_tags = 0, count = 0, i;
    char title[512] = "";
    if (!ebml_read(src, 0, &hdr) || hdr.id != 0x1A45DFA3u || hdr.size < 0) return;
    if (!ebml_read(src, hdr.data + hdr.size, &seg) || seg.id != MKV_SEGMENT) return;
    seg_end = seg.size >= 0 ? seg.data + seg.size : src->length;
    if (seg_end > src->length) seg_end = src->length;
    pos = seg.data;
    while (pos < seg_end && count++ < MKV_MAX_ELEMENTS && ebml_read(src, pos, &e)) {
        if (e.id == MKV_SEEKHEAD && e.size >= 0 && e.size < (1 << 20)) {
            uint8_t* b = read_block(src, e.data, (size_t)e.size);
            if (b) {
                const uint8_t *cur = b, *body;
                size_t size;
                uint32_t id;
                while (mem_el(&cur, b + e.size, &id, &body, &size)) {
                    const uint8_t *c2 = body, *b2;
                    size_t s2;
                    uint32_t id2, sid = 0;
                    int64_t spos = -1;
                    if (id != MKV_SEEK) continue;
                    while (mem_el(&c2, body + size, &id2, &b2, &s2)) {
                        if (id2 == MKV_SEEKID) sid = (uint32_t)mem_uint(b2, s2);
                        else if (id2 == MKV_SEEKPOS) spos = (int64_t)mem_uint(b2, s2);
                    }
                    if ((sid == MKV_TAGS || sid == MKV_ATTACHMENTS || sid == MKV_INFO) && spos >= 0 && spos < src->length && nwant < 8) {
                        want[nwant++] = seg.data + spos;
                        if (sid == MKV_TAGS) have_seek_tags = 1;
                    }
                }
                free(b);
            }
        } else if (e.id == MKV_TAGS || e.id == MKV_ATTACHMENTS || e.id == MKV_INFO) {
            mkv_element(src, &e, sink, title, sizeof(title));
            if (e.id == MKV_TAGS) visited_tags = 1;
            else if (e.id == MKV_ATTACHMENTS) visited_att = 1;
            else visited_info = 1;
        } else if (e.id == MKV_CLUSTER && have_seek_tags) {
            break;
        }
        if (e.size < 0) break;                           /* unknown size: cannot skip it */
        pos = e.data + e.size;
    }
    for (i = 0; i < nwant; i++) {
        if (!ebml_read(src, want[i], &e)) continue;
        if ((e.id == MKV_TAGS && visited_tags) || (e.id == MKV_ATTACHMENTS && visited_att) || (e.id == MKV_INFO && visited_info)) continue;
        if (e.id == MKV_TAGS || e.id == MKV_ATTACHMENTS || e.id == MKV_INFO) mkv_element(src, &e, sink, title, sizeof(title));
        if (e.id == MKV_TAGS) visited_tags = 1;
        else if (e.id == MKV_ATTACHMENTS) visited_att = 1;
        else if (e.id == MKV_INFO) visited_info = 1;
    }
    /* The segment title last: tags win over it. */
    if (title[0]) {
        char* s = text_8bit((const uint8_t*)title, strlen(title), 0);
        if (s) sink->text(sink, FA_TAG_FORMAT_MATROSKA, "INFO/TITLE", s);
        free(s);
    }
}

/* ---- the dispatcher ---------------------------------------------------------------- */

static int is_mp4(const uint8_t* h, size_t n)
{
    static const char* const types[] = { "ftyp", "moov", "mdat", "free", "skip", "wide", "pnot" };
    size_t i;
    if (n < 8) return 0;
    for (i = 0; i < sizeof(types) / sizeof(types[0]); i++) if (memcmp(h + 4, types[i], 4) == 0) return 1;
    return 0;
}

void fa_tags_collect(fa_source* src, fa_tag_sink* sink)
{
    const uint8_t* head;
    size_t n;
    int64_t id3, v1;
    if (!src->seekable || src->length <= 0) return;
    if (fa_asf_read_tags(src, sink)) return;
    n = fa_source_peek(src, &head);
    if (is_mp4(head, n)) { read_mp4(src, sink); return; }
    if (n >= 4 && head[0] == 0x1A && head[1] == 0x45 && head[2] == 0xDF && head[3] == 0xA3) { read_mkv(src, sink); return; }
    if (n >= 12 && (memcmp(head, "RIFF", 4) == 0 || memcmp(head, "RF64", 4) == 0)) {
        read_riff(src, sink);
    } else if (n >= 12 && memcmp(head, "FORM", 4) == 0 && (memcmp(head + 8, "AIFF", 4) == 0 || memcmp(head + 8, "AIFC", 4) == 0)) {
        read_aiff(src, sink);
    } else if (n >= 4 && memcmp(head, "OggS", 4) == 0) {
        fa_ogg_read_tags(src, sink);
        return;
    } else {
        /* MP3, AAC, AC-3, FLAC: FLAC's own comments win over an ID3v2 tag in front
         * of them; then ID3v2. */
        id3 = read_id3v2(src, 0, NULL);
        read_flac(src, id3, sink, NULL);
        if (id3 > 0) read_id3v2(src, 0, sink);
    }
    /* APEv2 and ID3v1 at the end of any of these (some tools tag WAV that way too). */
    v1 = read_id3v1(src, NULL);
    read_ape(src, v1, sink);
    read_id3v1(src, sink);
}

void fa_tags_audio_span(fa_source* src, int64_t* out_start, int64_t* out_end, uint32_t* out_bits)
{
    int64_t id3, flac, v1, ape;
    *out_start = 0;
    *out_end = src->length;
    *out_bits = 0;
    if (!src->seekable || src->length <= 0) return;
    id3 = read_id3v2(src, 0, NULL);
    flac = read_flac(src, id3, NULL, out_bits);
    *out_start = flac > 0 ? flac : id3;
    v1 = read_id3v1(src, NULL);
    ape = read_ape(src, v1, NULL);
    *out_end = src->length - v1 - ape;
    if (*out_end < *out_start) *out_end = *out_start;
}
