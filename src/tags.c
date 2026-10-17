/* Loudness tags: ReplayGain 2 (REPLAYGAIN_TRACK_GAIN etc., reference -18 LUFS)
 * and R128 (R128_TRACK_GAIN, Q7.8 dB relative to -23 LUFS), read from ID3v2 TXXX
 * frames, FLAC and Ogg Vorbis comments (OpusTags too), APEv2 tags and ASF (.wma)
 * attributes (demux_asf.c). Only the frames that can hold them are read; pictures
 * and other large frames are skipped with a seek. */
#include "fa_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FA_TAG_TEXT_MAX 512

static int ieq(const char* a, const char* b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x = (char)(x - 'a' + 'A');
        if (y >= 'a' && y <= 'z') y = (char)(y - 'a' + 'A');
        if (x != y) return 0;
    }
    return *a == *b;
}

/* "-6.54 dB" -> -6.54, independent of the C locale. NAN if no number. */
static double parse_number(const char* s)
{
    double v = 0.0, scale = 1.0;
    int neg = 0, digits = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '+' || *s == '-') neg = *s++ == '-';
    for (; *s >= '0' && *s <= '9'; s++, digits++) v = v * 10.0 + (*s - '0');
    if (*s == '.' || *s == ',') {
        for (s++; *s >= '0' && *s <= '9'; s++, digits++) { scale /= 10.0; v += (*s - '0') * scale; }
    }
    if (!digits) return NAN;
    return neg ? -v : v;
}

int fa_tags_key_is(const char* key, const char* name) { return ieq(key, name); }

void fa_tags_handle(const char* key, const char* value, fa_tag_loudness* out)
{
    double v = parse_number(value);
    if (v != v) return;
    if (ieq(key, "REPLAYGAIN_TRACK_GAIN")) out->track_lufs = -18.0 - v;
    else if (ieq(key, "REPLAYGAIN_ALBUM_GAIN")) out->album_lufs = -18.0 - v;
    else if (ieq(key, "R128_TRACK_GAIN")) out->track_lufs = -23.0 - v / 256.0;
    else if (ieq(key, "R128_ALBUM_GAIN")) out->album_lufs = -23.0 - v / 256.0;
    else if (ieq(key, "REPLAYGAIN_TRACK_PEAK") && v > 0.0) out->track_peak_db = 20.0 * log10(v);
    else if (ieq(key, "REPLAYGAIN_ALBUM_PEAK") && v > 0.0) out->album_peak_db = 20.0 * log10(v);
}

static int read_exact(fa_source* src, void* buf, size_t n) { return fa_source_read(src, buf, n) == (int64_t)n; }
static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint32_t le32(const uint8_t* p) { return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0]; }
static uint32_t synchsafe(const uint8_t* p) { return ((uint32_t)(p[0] & 0x7f) << 21) | ((uint32_t)(p[1] & 0x7f) << 14) | ((uint32_t)(p[2] & 0x7f) << 7) | (p[3] & 0x7f); }

/* ---- ID3v2 ------------------------------------------------------------------- */

/* Decodes an ID3 text payload to ASCII with '\0' between strings (non-ASCII
 * characters become '?': the keys and numbers we look for are ASCII). */
static size_t id3_text(const uint8_t* p, size_t n, char* out, size_t cap)
{
    size_t i = 0, k = 0;
    int enc;
    if (n == 0) return 0;
    enc = p[i++];
    if (enc == 1 || enc == 2) {
        int big = enc == 2;
        while (i + 1 < n && k + 1 < cap) {
            unsigned u = big ? ((unsigned)p[i] << 8 | p[i + 1]) : ((unsigned)p[i + 1] << 8 | p[i]);
            i += 2;
            if (u == 0xFEFF) continue;                       /* BOM in our byte order */
            if (u == 0xFFFE) { big = !big; continue; }       /* BOM in the other one */
            out[k++] = u == 0 ? '\0' : u < 0x80 ? (char)u : '?';
        }
    } else {
        while (i < n && k + 1 < cap) {
            uint8_t c = p[i++];
            out[k++] = c < 0x80 ? (char)c : '?';
        }
    }
    out[k] = '\0';
    return k;
}

/* Returns the size of the whole tag (header included), 0 if there is none. */
static int64_t read_id3v2(fa_source* src, fa_tag_loudness* out)
{
    uint8_t h[10];
    int ver, flags;
    int64_t size, pos, end;
    if (fa_source_seek(src, 0) != FA_OK || !read_exact(src, h, 10)) return 0;
    if (memcmp(h, "ID3", 3) != 0 || h[3] < 2 || h[3] > 4) return 0;
    ver = h[3];
    flags = h[5];
    size = synchsafe(h + 6);
    end = 10 + size;
    if (flags & 0x10) end += 10;          /* footer */
    if (ver < 4 && (flags & 0x80)) return end; /* whole-tag unsynchronisation: rare, skip */

    pos = 10;
    if (flags & 0x40) {                   /* extended header */
        uint8_t e[4];
        if (!read_exact(src, e, 4)) return end;
        pos += ver == 4 ? synchsafe(e) : (int64_t)be32(e) + 4;
    }
    while (pos + (ver == 2 ? 6 : 10) <= 10 + size) {
        uint8_t fh[10];
        int64_t fsize;
        int is_txxx;
        int hl = ver == 2 ? 6 : 10;
        if (fa_source_seek(src, pos) != FA_OK || !read_exact(src, fh, (size_t)hl)) break;
        if (fh[0] == 0) break;            /* padding */
        if (ver == 2) {
            fsize = ((int64_t)fh[3] << 16) | ((int64_t)fh[4] << 8) | fh[5];
            is_txxx = memcmp(fh, "TXX", 3) == 0;
        } else {
            fsize = ver == 4 ? synchsafe(fh + 4) : be32(fh + 4);
            /* Compressed or encrypted frames cannot be read as text. */
            is_txxx = memcmp(fh, "TXXX", 4) == 0 && !(fh[9] & (ver == 4 ? 0x0c : 0xc0));
        }
        if (fsize <= 0 || pos + hl + fsize > 10 + size) break;
        if (is_txxx && fsize < FA_TAG_TEXT_MAX) {
            uint8_t raw[FA_TAG_TEXT_MAX];
            char text[FA_TAG_TEXT_MAX];
            size_t n;
            if (read_exact(src, raw, (size_t)fsize)) {
                n = id3_text(raw, (size_t)fsize, text, sizeof(text));
                if (n > 0) {
                    size_t klen = strlen(text);
                    if (klen < n) fa_tags_handle(text, text + klen + 1, out);
                }
            }
        }
        pos += hl + fsize;
    }
    return end;
}

/* ---- Vorbis comments (FLAC) -------------------------------------------------- */

void fa_tags_parse_vorbis_comments(const uint8_t* p, size_t n, fa_tag_loudness* out)
{
    size_t pos = 0;
    uint32_t count, i;
    if (n < 8) return;
    pos = 4 + (size_t)le32(p);            /* vendor string */
    if (pos + 4 > n) return;
    count = le32(p + pos);
    pos += 4;
    for (i = 0; i < count && pos + 4 <= n; i++) {
        uint32_t len = le32(p + pos);
        pos += 4;
        if (len > n - pos) return;
        if (len < FA_TAG_TEXT_MAX) {
            char text[FA_TAG_TEXT_MAX];
            char* eq;
            memcpy(text, p + pos, len);
            text[len] = '\0';
            eq = strchr(text, '=');
            if (eq) { *eq = '\0'; fa_tags_handle(text, eq + 1, out); }
        }
        pos += len;
    }
}

static void read_flac(fa_source* src, int64_t start, fa_tag_loudness* out)
{
    uint8_t h[4];
    int64_t pos = start + 4;
    if (fa_source_seek(src, start) != FA_OK || !read_exact(src, h, 4) || memcmp(h, "fLaC", 4) != 0) return;
    for (;;) {
        int last, type;
        uint32_t len;
        if (fa_source_seek(src, pos) != FA_OK || !read_exact(src, h, 4)) return;
        last = h[0] & 0x80;
        type = h[0] & 0x7f;
        len = ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | h[3];
        if (type == 4) {
            /* The whole block, so tags after long lyrics or cue sheets are found.
             * Blocks over 1 MB are not plausible comments and are skipped. */
            uint8_t* buf = len <= 1u << 20 ? (uint8_t*)malloc(len ? len : 1) : NULL;
            if (buf && read_exact(src, buf, len)) fa_tags_parse_vorbis_comments(buf, len, out);
            free(buf);
            return;
        }
        if (last) return;
        pos += 4 + (int64_t)len;
    }
}

/* ---- APEv2 ------------------------------------------------------------------- */

static int read_ape_at(fa_source* src, int64_t footer_pos, fa_tag_loudness* out)
{
    uint8_t f[32];
    uint32_t size, count, i;
    int64_t pos;
    if (footer_pos < 0 || fa_source_seek(src, footer_pos) != FA_OK || !read_exact(src, f, 32)) return 0;
    if (memcmp(f, "APETAGEX", 8) != 0) return 0;
    size = le32(f + 12);                  /* items + footer */
    count = le32(f + 16);
    if (size < 32 || size > (1u << 24)) return 1;
    pos = footer_pos + 32 - (int64_t)size;
    for (i = 0; i < count; i++) {
        uint8_t ih[8];
        char key[256];
        size_t k = 0;
        uint32_t vlen;
        if (pos < 0 || fa_source_seek(src, pos) != FA_OK || !read_exact(src, ih, 8)) return 1;
        vlen = le32(ih);
        pos += 8;
        for (;;) {                        /* key, '\0'-terminated */
            uint8_t c;
            if (!read_exact(src, &c, 1)) return 1;
            pos++;
            if (c == 0) break;
            if (k + 1 < sizeof(key)) key[k++] = (char)c;
        }
        key[k] = '\0';
        if (vlen < FA_TAG_TEXT_MAX) {
            char val[FA_TAG_TEXT_MAX];
            if (!read_exact(src, val, vlen)) return 1;
            val[vlen] = '\0';
            fa_tags_handle(key, val, out);
        }
        pos += vlen;
    }
    return 1;
}

static void read_ape(fa_source* src, fa_tag_loudness* out)
{
    int64_t len = src->length;
    uint8_t t[3];
    if (len < 32) return;
    if (read_ape_at(src, len - 32, out)) return;
    /* An ID3v1 tag may follow the APE tag. */
    if (len >= 128 + 32 && fa_source_seek(src, len - 128) == FA_OK && read_exact(src, t, 3) && memcmp(t, "TAG", 3) == 0)
        read_ape_at(src, len - 128 - 32, out);
}

void fa_tags_read_loudness(fa_source* src, fa_tag_loudness* out)
{
    int64_t id3;
    out->track_lufs = out->album_lufs = NAN;
    out->track_peak_db = out->album_peak_db = NAN;
    if (!src->seekable) return;
    {
        /* ASF (.wma): Content Description and Extended Content Description / Metadata items. */
        fa_asf_tags asf;
        if (fa_asf_read_tags(src, &asf)) { *out = asf.loudness; return; }
    }
    id3 = read_id3v2(src, out);
    read_flac(src, id3, out);
    fa_ogg_read_loudness(src, out);
    read_ape(src, out);
}
