/* HLS (RFC 8216): playlists, variant choice, MPEG-TS demuxing, AES-128, and the
 * producer that turns a playlist into one continuous stream of audio bytes in
 * the net window. The stream's decoder is the HLS decoder at the end of this
 * file: it opens the format found in the segments (MP3, AAC, AC-3, fMP4, ...)
 * through the normal registry, and seeks on-demand playlists by segment. */
#include "net.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FA_HLS_LIVE_START 3          /* live: start this many segments from the end */
#define FA_HLS_SEGMENT_RETRIES 3

/* ---- playlists ------------------------------------------------------------- */

/* Value of `key` in an attribute list (KEY=value,KEY="quoted, value"), into out. */
static int attr(const char* list, const char* key, char* out, size_t cap)
{
    size_t klen = strlen(key);
    const char* p = list;
    while (*p) {
        const char* name = p;
        const char* eq = p;
        while (*eq && *eq != '=' && *eq != ',') eq++;
        if (*eq != '=') { p = *eq ? eq + 1 : eq; continue; }
        {
            const char* v = eq + 1;
            const char* end;
            int quoted = *v == '"';
            if (quoted) { v++; end = strchr(v, '"'); if (!end) end = v + strlen(v); }
            else end = v + strcspn(v, ",");
            if ((size_t)(eq - name) == klen && fa_strncasecmp_ascii(name, key, klen) == 0) {
                size_t n = (size_t)(end - v);
                if (n >= cap) n = cap - 1;
                memcpy(out, v, n);
                out[n] = 0;
                return 1;
            }
            p = end;
            if (quoted && *p == '"') p++;
            if (*p == ',') p++;
        }
    }
    return 0;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_iv(const char* s, uint8_t iv[16])
{
    size_t len, i;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    len = strlen(s);
    if (len == 0 || len > 32) return 0;
    memset(iv, 0, 16);
    /* Right-aligned: shorter values are left-padded with zeros. */
    for (i = 0; i < len; i++) {
        int v = hexval(s[len - 1 - i]);
        if (v < 0) return 0;
        iv[15 - i / 2] |= (uint8_t)(i % 2 ? v << 4 : v);
    }
    return 1;
}

void fa_hls_playlist_free(fa_hls_playlist* p)
{
    int i;
    for (i = 0; i < p->variant_count; i++) { free(p->variant_uri[i]); free(p->variant_codecs[i]); }
    free(p->variant_uri);
    free(p->variant_bandwidth);
    free(p->variant_codecs);
    free(p->audio_group_uri);
    for (i = 0; i < p->segment_count; i++) {
        free(p->segments[i].uri);
        free(p->segments[i].key_uri);
        free(p->segments[i].map_uri);
    }
    free(p->segments);
    memset(p, 0, sizeof(*p));
}

fa_result fa_hls_parse(const char* text, size_t n, const char* base, fa_hls_playlist* out)
{
    const char* line = text;
    const char* end = text + n;
    char tmp[FA_NET_URL_MAX], resolved[FA_NET_URL_MAX];
    int seg_cap = 0, var_cap = 0;
    /* State carried to the next URI line. */
    double extinf = -1.0;
    int pending_variant = 0, discontinuity = 0, key_method = 0, has_iv = 0, audio_default = 0;
    uint32_t bandwidth = 0;
    char* codecs = NULL;
    char* key_uri = NULL;
    char* map_uri = NULL;
    uint8_t iv[16];
    int64_t br_len = -1, br_off = 0, next_br_off = 0;
    int64_t seq;
    double t = 0.0;
    fa_result r = FA_OK;

    memset(out, 0, sizeof(*out));
    memset(iv, 0, sizeof(iv));
    while (line < end && (*line == ' ' || *line == '\r' || *line == '\n' || (unsigned char)*line == 0xEF ||
                          (unsigned char)*line == 0xBB || (unsigned char)*line == 0xBF))
        line++;   /* whitespace and a UTF-8 BOM */
    if ((size_t)(end - line) < 7 || strncmp(line, "#EXTM3U", 7) != 0) return FA_ERROR_UNSUPPORTED_FORMAT;
    seq = 0;

    while (line < end) {
        const char* le = line;
        size_t len;
        while (le < end && *le != '\r' && *le != '\n') le++;
        len = (size_t)(le - line);
        if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
        memcpy(tmp, line, len);
        tmp[len] = 0;
        while (len > 0 && (tmp[len - 1] == ' ' || tmp[len - 1] == '\t')) tmp[--len] = 0;
        line = le;
        while (line < end && (*line == '\r' || *line == '\n')) line++;
        if (len == 0) continue;

        if (tmp[0] == '#') {
            if (strncmp(tmp, "#EXT-X-STREAM-INF:", 18) == 0) {
                char v[256];
                pending_variant = 1;
                out->is_master = 1;
                bandwidth = attr(tmp + 18, "BANDWIDTH", v, sizeof(v)) ? (uint32_t)strtoul(v, NULL, 10) : 0;
                free(codecs);
                codecs = attr(tmp + 18, "CODECS", v, sizeof(v)) ? fa_strdup(v) : NULL;
            } else if (strncmp(tmp, "#EXT-X-MEDIA:", 13) == 0) {
                char type[32], uri[FA_NET_URL_MAX], def[8];
                out->is_master = 1;
                if (attr(tmp + 13, "TYPE", type, sizeof(type)) && strcmp(type, "AUDIO") == 0 &&
                    attr(tmp + 13, "URI", uri, sizeof(uri)) && fa_url_resolve(base, uri, resolved, sizeof(resolved)) == FA_OK) {
                    int is_default = attr(tmp + 13, "DEFAULT", def, sizeof(def)) && strcmp(def, "YES") == 0;
                    if (!out->audio_group_uri || (is_default && !audio_default)) {
                        free(out->audio_group_uri);
                        out->audio_group_uri = fa_strdup(resolved);
                        audio_default = is_default;
                    }
                }
            } else if (strncmp(tmp, "#EXTINF:", 8) == 0) {
                extinf = atof(tmp + 8);
            } else if (strncmp(tmp, "#EXT-X-TARGETDURATION:", 22) == 0) {
                out->target_duration = atof(tmp + 22);
            } else if (strncmp(tmp, "#EXT-X-MEDIA-SEQUENCE:", 22) == 0) {
                seq = out->media_sequence = strtoll(tmp + 22, NULL, 10);
            } else if (strncmp(tmp, "#EXT-X-DISCONTINUITY", 20) == 0 && tmp[20] == 0) {
                discontinuity = 1;
            } else if (strncmp(tmp, "#EXT-X-ENDLIST", 14) == 0) {
                out->endlist = 1;
            } else if (strncmp(tmp, "#EXT-X-BYTERANGE:", 17) == 0) {
                char* at;
                br_len = strtoll(tmp + 17, &at, 10);
                br_off = *at == '@' ? strtoll(at + 1, NULL, 10) : next_br_off;
            } else if (strncmp(tmp, "#EXT-X-KEY:", 11) == 0) {
                char v[FA_NET_URL_MAX];
                free(key_uri);
                key_uri = NULL;
                key_method = 0;
                has_iv = 0;
                if (attr(tmp + 11, "METHOD", v, sizeof(v))) {
                    if (strcmp(v, "AES-128") == 0) key_method = 1;
                    else if (strcmp(v, "NONE") != 0) { r = FA_ERROR_PROTECTED_CONTENT; break; }  /* SAMPLE-AES, DRM */
                }
                if (key_method && attr(tmp + 11, "URI", v, sizeof(v)) && fa_url_resolve(base, v, resolved, sizeof(resolved)) == FA_OK)
                    key_uri = fa_strdup(resolved);
                if (key_method && !key_uri) { r = FA_ERROR_UNSUPPORTED_FORMAT; break; }
                if (attr(tmp + 11, "IV", v, sizeof(v))) has_iv = parse_iv(v, iv);
            } else if (strncmp(tmp, "#EXT-X-MAP:", 11) == 0) {
                char v[FA_NET_URL_MAX];
                free(map_uri);
                map_uri = NULL;
                if (attr(tmp + 11, "URI", v, sizeof(v)) && fa_url_resolve(base, v, resolved, sizeof(resolved)) == FA_OK)
                    map_uri = fa_strdup(resolved);
            }
            continue;
        }

        /* A URI line. */
        if (fa_url_resolve(base, tmp, resolved, sizeof(resolved)) != FA_OK) { pending_variant = 0; extinf = -1; continue; }
        if (pending_variant) {
            if (out->variant_count == var_cap) {
                int cap = var_cap ? var_cap * 2 : 8;
                char** u = (char**)realloc(out->variant_uri, sizeof(char*) * (size_t)cap);
                uint32_t* b;
                char** c;
                if (!u) { r = FA_ERROR_OUT_OF_MEMORY; break; }
                out->variant_uri = u;
                b = (uint32_t*)realloc(out->variant_bandwidth, sizeof(uint32_t) * (size_t)cap);
                if (!b) { r = FA_ERROR_OUT_OF_MEMORY; break; }
                out->variant_bandwidth = b;
                c = (char**)realloc(out->variant_codecs, sizeof(char*) * (size_t)cap);
                if (!c) { r = FA_ERROR_OUT_OF_MEMORY; break; }
                out->variant_codecs = c;
                var_cap = cap;
            }
            out->variant_uri[out->variant_count] = fa_strdup(resolved);
            out->variant_bandwidth[out->variant_count] = bandwidth;
            out->variant_codecs[out->variant_count] = codecs;
            codecs = NULL;
            out->variant_count++;
            pending_variant = 0;
            continue;
        }
        {
            fa_hls_segment* s;
            if (out->segment_count == seg_cap) {
                int cap = seg_cap ? seg_cap * 2 : 64;
                fa_hls_segment* ns = (fa_hls_segment*)realloc(out->segments, sizeof(fa_hls_segment) * (size_t)cap);
                if (!ns) { r = FA_ERROR_OUT_OF_MEMORY; break; }
                out->segments = ns;
                seg_cap = cap;
            }
            s = &out->segments[out->segment_count++];
            memset(s, 0, sizeof(*s));
            s->uri = fa_strdup(resolved);
            s->duration = extinf >= 0 ? extinf : out->target_duration;
            s->start = t;
            s->seq = seq++;
            s->discontinuity = discontinuity;
            s->key_method = key_method;
            s->key_uri = fa_strdup(key_uri);
            s->has_iv = has_iv;
            memcpy(s->iv, iv, 16);
            s->map_uri = fa_strdup(map_uri);
            s->byte_offset = br_len >= 0 ? br_off : 0;
            s->byte_length = br_len;
            if (br_len >= 0) next_br_off = br_off + br_len;
            br_len = -1;
            t += s->duration;
            extinf = -1.0;
            discontinuity = 0;
        }
    }
    free(codecs);
    free(key_uri);
    free(map_uri);
    out->total_duration = t;
    if (r != FA_OK) { fa_hls_playlist_free(out); return r; }
    if (!out->is_master && out->segment_count == 0 && out->endlist) { fa_hls_playlist_free(out); return FA_ERROR_UNSUPPORTED_FORMAT; }
    return FA_OK;
}

static int audio_only_codecs(const char* c)
{
    return c && !strstr(c, "avc") && !strstr(c, "hvc") && !strstr(c, "hev") && !strstr(c, "vp0") && !strstr(c, "av01") &&
           !strstr(c, "dvh");
}

int fa_hls_pick_variant(const fa_hls_playlist* p, uint32_t max_bw)
{
    int i, best = -1, lowest = -1, any_audio_only = 0;
    for (i = 0; i < p->variant_count; i++) if (audio_only_codecs(p->variant_codecs[i])) any_audio_only = 1;
    for (i = 0; i < p->variant_count; i++) {
        uint32_t bw = p->variant_bandwidth[i];
        if (any_audio_only && !audio_only_codecs(p->variant_codecs[i])) continue;
        if (lowest < 0 || bw < p->variant_bandwidth[lowest]) lowest = i;
        if (max_bw && bw > max_bw) continue;
        if (best < 0 || bw > p->variant_bandwidth[best]) best = i;
    }
    return best >= 0 ? best : lowest;
}

/* ---- MPEG-TS ------------------------------------------------------------------ */

void fa_ts_init(fa_ts_demux* d)
{
    memset(d, 0, sizeof(*d));
    d->pmt_pid = -1;
    d->audio_pid = -1;
}

static int audio_stream_type(int type, const uint8_t* desc, int desc_len)
{
    int i;
    if (type == 0x03 || type == 0x04 || type == 0x0F || type == 0x81 || type == 0x87) return type;
    if (type == 0x06) {   /* DVB private data: AC-3 / E-AC-3 descriptors */
        for (i = 0; i + 2 <= desc_len; i += 2 + desc[i + 1]) {
            if (desc[i] == 0x6A) return 0x81;
            if (desc[i] == 0x7A) return 0x87;
        }
    }
    return 0;
}

/* A PSI section starting at the payload (pointer field first). */
static void ts_psi(fa_ts_demux* d, const uint8_t* p, int n, int is_pmt)
{
    int ptr, slen, i;
    if (n < 1) return;
    ptr = p[0];
    p += 1 + ptr;
    n -= 1 + ptr;
    if (n < 8) return;
    slen = ((p[1] & 0x0F) << 8) | p[2];
    if (slen + 3 < n) n = slen + 3;
    if (!is_pmt) {
        if (p[0] != 0x00) return;
        for (i = 8; i + 4 <= n - 4; i += 4) {
            int program = (p[i] << 8) | p[i + 1];
            if (program != 0) { d->pmt_pid = ((p[i + 2] & 0x1F) << 8) | p[i + 3]; return; }
        }
    } else {
        int info;
        if (p[0] != 0x02 || n < 12) return;
        info = ((p[10] & 0x0F) << 8) | p[11];
        for (i = 12 + info; i + 5 <= n - 4;) {
            int type = p[i], pid = ((p[i + 1] & 0x1F) << 8) | p[i + 2];
            int es_len = ((p[i + 3] & 0x0F) << 8) | p[i + 4];
            int at = audio_stream_type(type, p + i + 5, i + 5 + es_len <= n ? es_len : 0);
            if (at && d->audio_pid < 0) { d->audio_pid = pid; d->stream_type = at; }
            i += 5 + es_len;
        }
    }
}

static int ts_packet(fa_ts_demux* d, const uint8_t* pk, int (*out)(void*, const uint8_t*, size_t), void* user)
{
    int pid = ((pk[1] & 0x1F) << 8) | pk[2];
    int pusi = pk[1] & 0x40;
    int afc = (pk[3] >> 4) & 3;
    int off = 4;
    if (afc == 0 || afc == 2) return 0;                /* no payload */
    if (afc == 3) off += 1 + pk[4];
    if (off >= 188) return 0;
    if (pid == 0) { if (pusi) ts_psi(d, pk + off, 188 - off, 0); return 0; }
    if (pid == d->pmt_pid) { if (pusi && d->audio_pid < 0) ts_psi(d, pk + off, 188 - off, 1); return 0; }
    if (pid != d->audio_pid) return 0;
    {
        const uint8_t* p = pk + off;
        int n = 188 - off;
        if (pusi) { d->in_pes = 1; d->pes_head_len = 0; d->pes_header_left = -1; }
        if (!d->in_pes) return 0;
        if (d->pes_header_left < 0) {
            /* Gather the fixed 9 bytes, then skip the optional header they announce. */
            while (n > 0 && d->pes_head_len < 9) { d->pes_head[d->pes_head_len++] = *p++; n--; }
            if (d->pes_head_len < 9) return 0;
            if (d->pes_head[0] != 0 || d->pes_head[1] != 0 || d->pes_head[2] != 1) { d->in_pes = 0; return 0; }
            d->pes_header_left = d->pes_head[8];
        }
        if (d->pes_header_left > 0) {
            int k = d->pes_header_left < n ? d->pes_header_left : n;
            d->pes_header_left -= k;
            p += k;
            n -= k;
        }
        if (n > 0 && out(user, p, (size_t)n)) return 1;
    }
    return 0;
}

int fa_ts_feed(fa_ts_demux* d, const uint8_t* data, size_t n, int (*out)(void*, const uint8_t*, size_t), void* user)
{
    while (n > 0) {
        if (d->carry_len > 0 || n < 188) {
            size_t k = 188 - d->carry_len < n ? 188 - d->carry_len : n;
            memcpy(d->carry + d->carry_len, data, k);
            d->carry_len += k;
            data += k;
            n -= k;
            if (d->carry_len < 188) return 0;
            d->carry_len = 0;
            if (d->carry[0] == 0x47) { if (ts_packet(d, d->carry, out, user)) return 1; }
            continue;
        }
        if (data[0] != 0x47) {                        /* lost sync: find the next packet start */
            const uint8_t* s = (const uint8_t*)memchr(data + 1, 0x47, n - 1);
            if (!s) return 0;
            n -= (size_t)(s - data);
            data = s;
            continue;
        }
        if (ts_packet(d, data, out, user)) return 1;
        data += 188;
        n -= 188;
    }
    return 0;
}

/* ---- AES-128 (FIPS-197), decryption only ------------------------------------------ */

static const uint8_t sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,
    0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,
    0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,
    0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,
    0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,
    0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }
static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t r = 0;
    while (b) { if (b & 1) r ^= a; a = xtime(a); b >>= 1; }
    return r;
}

/* Decryption tables (FIPS-197 5.3, as 32-bit words): td[0][x] is the InvMixColumns
 * column of InvSubBytes(x) in the first row, td[1..3] the same rotated a row at a
 * time. Built once per process. Words are big-endian: a column's row 0 is the top
 * byte. */
static uint32_t td[4][256];
static uint8_t inv_sbox[256];
static fa_atomic32 aes_tables_state;   /* 0 not done, 1 in progress, 2 done */

static void aes_init_tables(void)
{
    int i, k;
    if (fa_load32(&aes_tables_state) == 2) return;
    if (!fa_cas32(&aes_tables_state, 0, 1)) {
        while (fa_load32(&aes_tables_state) != 2) fa_sleep_ms(1);
        return;
    }
    for (i = 0; i < 256; i++) inv_sbox[sbox[i]] = (uint8_t)i;
    for (i = 0; i < 256; i++) {
        uint8_t x = inv_sbox[i];
        td[0][i] = (uint32_t)gmul(x, 14) << 24 | (uint32_t)gmul(x, 9) << 16 | (uint32_t)gmul(x, 13) << 8 | gmul(x, 11);
        for (k = 1; k < 4; k++) td[k][i] = td[k - 1][i] >> 8 | td[k - 1][i] << 24;
    }
    fa_store32(&aes_tables_state, 2);
}

static void aes_expand(const uint8_t key[16], uint8_t rk[176])
{
    uint8_t rcon = 1;
    int i;
    memcpy(rk, key, 16);
    for (i = 16; i < 176; i += 4) {
        uint8_t t[4];
        memcpy(t, rk + i - 4, 4);
        if (i % 16 == 0) {
            uint8_t x = t[0];
            t[0] = (uint8_t)(sbox[t[1]] ^ rcon); t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[x];
            rcon = xtime(rcon);
        }
        rk[i] = rk[i - 16] ^ t[0]; rk[i + 1] = rk[i - 15] ^ t[1]; rk[i + 2] = rk[i - 14] ^ t[2]; rk[i + 3] = rk[i - 13] ^ t[3];
    }
}

/* The round keys of the equivalent inverse cipher (FIPS-197 5.3.5), made once per
 * segment: the encryption keys in reverse order, InvMixColumns applied to all but
 * the first and last. */
typedef struct aes_ctx { uint32_t dk[44]; } aes_ctx;

static void aes_init(aes_ctx* c, const uint8_t key[16])
{
    uint8_t rk[176];
    int round, i;
    aes_init_tables();
    aes_expand(key, rk);
    for (round = 0; round <= 10; round++) {
        for (i = 0; i < 4; i++) {
            const uint8_t* w = rk + (10 - round) * 16 + i * 4;
            uint32_t v = fa_be32(w);
            if (round > 0 && round < 10)
                v = td[0][sbox[w[0]]] ^ td[1][sbox[w[1]]] ^ td[2][sbox[w[2]]] ^ td[3][sbox[w[3]]];
            c->dk[round * 4 + i] = v;
        }
    }
}

static void aes_put_be32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* One block in place. InvShiftRows moves row r of column c to column c + r, so
 * column c takes row r from column c - r. */
static void aes_decrypt_block(const aes_ctx* c, uint8_t b[16])
{
    const uint32_t* k = c->dk;
    uint32_t s0 = fa_be32(b) ^ k[0], s1 = fa_be32(b + 4) ^ k[1], s2 = fa_be32(b + 8) ^ k[2], s3 = fa_be32(b + 12) ^ k[3];
    uint32_t t0, t1, t2, t3;
    int round;
    for (round = 1; round < 10; round++) {
        k += 4;
        t0 = td[0][s0 >> 24] ^ td[1][(s3 >> 16) & 0xFF] ^ td[2][(s2 >> 8) & 0xFF] ^ td[3][s1 & 0xFF] ^ k[0];
        t1 = td[0][s1 >> 24] ^ td[1][(s0 >> 16) & 0xFF] ^ td[2][(s3 >> 8) & 0xFF] ^ td[3][s2 & 0xFF] ^ k[1];
        t2 = td[0][s2 >> 24] ^ td[1][(s1 >> 16) & 0xFF] ^ td[2][(s0 >> 8) & 0xFF] ^ td[3][s3 & 0xFF] ^ k[2];
        t3 = td[0][s3 >> 24] ^ td[1][(s2 >> 16) & 0xFF] ^ td[2][(s1 >> 8) & 0xFF] ^ td[3][s0 & 0xFF] ^ k[3];
        s0 = t0; s1 = t1; s2 = t2; s3 = t3;
    }
    k += 4;
#define AES_LAST(a, b2, c2, d) \
    ((uint32_t)inv_sbox[(a) >> 24] << 24 | (uint32_t)inv_sbox[((b2) >> 16) & 0xFF] << 16 | \
     (uint32_t)inv_sbox[((c2) >> 8) & 0xFF] << 8 | inv_sbox[(d) & 0xFF])
    aes_put_be32(b, AES_LAST(s0, s3, s2, s1) ^ k[0]);
    aes_put_be32(b + 4, AES_LAST(s1, s0, s3, s2) ^ k[1]);
    aes_put_be32(b + 8, AES_LAST(s2, s1, s0, s3) ^ k[2]);
    aes_put_be32(b + 12, AES_LAST(s3, s2, s1, s0) ^ k[3]);
#undef AES_LAST
}

static void aes_cbc_decrypt(const aes_ctx* c, uint8_t iv[16], uint8_t* data, size_t n)
{
    uint8_t prev[16];
    size_t i, b;
    for (b = 0; b + 16 <= n; b += 16) {
        memcpy(prev, data + b, 16);
        aes_decrypt_block(c, data + b);
        for (i = 0; i < 16; i++) data[b + i] ^= iv[i];
        memcpy(iv, prev, 16);
    }
}

void fa_aes128_cbc_decrypt(const uint8_t key[16], uint8_t iv[16], uint8_t* data, size_t n)
{
    aes_ctx c;
    aes_init(&c, key);
    aes_cbc_decrypt(&c, iv, data, n);
}

/* ---- the producer ------------------------------------------------------------------- */

typedef struct seg_sink {
    fa_net* n;
    int kind;                 /* 0 undecided, 1 TS, 2 raw */
    fa_ts_demux ts;
    size_t id3_skip;          /* bytes of a leading ID3 tag still to drop (packed audio) */
    uint8_t head[10];
    size_t head_len;
    /* AES-128 */
    int aes;
    aes_ctx key;
    uint8_t iv[16];
    uint8_t pend[16 + 4096];
    size_t pend_len;
    int stopped;
    /* A retry after a dropped connection fetches the segment again from its start:
     * the first `skip` output bytes were already delivered. */
    uint64_t skip, delivered;
} seg_sink;

/* Audio bytes of the segment, in order, to the window. */
static int sink_write(seg_sink* s, const uint8_t* p, size_t n)
{
    if (s->skip > 0) {
        size_t k = s->skip < n ? (size_t)s->skip : n;
        s->skip -= k;
        p += k;
        n -= k;
    }
    if (n == 0) return 0;
    if (fa_net_write(s->n, p, n)) return 1;
    s->delivered += n;
    return 0;
}

static int sink_ts_out(void* user, const uint8_t* p, size_t n) { return sink_write((seg_sink*)user, p, n); }

/* Plain (decrypted) segment bytes. */
static int sink_plain(seg_sink* s, const uint8_t* p, size_t n)
{
    while (s->kind == 0 && n > 0) {
        /* Decide from the first bytes: TS packets, or audio (after an ID3 tag). */
        s->head[s->head_len++] = *p++;
        n--;
        if (s->head[0] == 0x47) {
            s->kind = 1;
            if (fa_ts_feed(&s->ts, s->head, s->head_len, sink_ts_out, s)) return 1;
            break;
        }
        if (s->head_len == 10 || s->head[0] != 'I' || (s->head_len >= 2 && s->head[1] != 'D') || (s->head_len >= 3 && s->head[2] != '3')) {
            size_t tag = fa_probe_id3v2_size(s->head, s->head_len);
            s->kind = 2;
            if (tag > 0) s->id3_skip = tag - s->head_len;
            else if (sink_write(s, s->head, s->head_len)) return 1;
            break;
        }
    }
    if (n == 0) return 0;
    if (s->kind == 1) return fa_ts_feed(&s->ts, p, n, sink_ts_out, s);
    if (s->id3_skip > 0) {
        size_t k = s->id3_skip < n ? s->id3_skip : n;
        s->id3_skip -= k;
        p += k;
        n -= k;
    }
    return n > 0 ? sink_write(s, p, n) : 0;
}

static int sink_data(void* user, const uint8_t* p, size_t n)
{
    seg_sink* s = (seg_sink*)user;
    if (!s->aes) { s->stopped = sink_plain(s, p, n); return s->stopped; }
    /* Decrypt whole blocks, always holding the last one back: it carries the padding. */
    while (n > 0) {
        size_t k = sizeof(s->pend) - s->pend_len < n ? sizeof(s->pend) - s->pend_len : n;
        memcpy(s->pend + s->pend_len, p, k);
        s->pend_len += k;
        p += k;
        n -= k;
        if (s->pend_len > 16) {
            size_t ready = ((s->pend_len - 1) / 16) * 16;
            aes_cbc_decrypt(&s->key, s->iv, s->pend, ready);
            if (sink_plain(s, s->pend, ready)) { s->stopped = 1; return 1; }
            memmove(s->pend, s->pend + ready, s->pend_len - ready);
            s->pend_len -= ready;
        }
    }
    return 0;
}

static int sink_finish(seg_sink* s)
{
    if (s->aes && s->pend_len == 16) {
        uint8_t pad;
        aes_cbc_decrypt(&s->key, s->iv, s->pend, 16);
        pad = s->pend[15];
        if (pad >= 1 && pad <= 16) return sink_plain(s, s->pend, 16u - pad);
    }
    return 0;
}

/* EXT-X-BYTERANGE: passes on only the segment's bytes. */
typedef struct range_limit {
    seg_sink* sink;
    int64_t offset;           /* the segment's first byte in the resource */
    int64_t skip;             /* bytes before it in this response (a server that ignored Range) */
    int64_t left;
    int bad;                  /* the response does not contain the segment */
} range_limit;

static int limited_head(void* user, const fa_http_response* resp)
{
    range_limit* lim = (range_limit*)user;
    if (resp->status >= 400) return 0;                    /* fa_http_fetch reports it */
    if (resp->status == 206 && resp->range_start <= lim->offset) lim->skip = lim->offset - resp->range_start;
    else if (resp->status == 200) lim->skip = lim->offset;
    else { lim->bad = 1; return 1; }
    return 0;
}

static int limited_data(void* user, const uint8_t* p, size_t n)
{
    range_limit* lim = (range_limit*)user;
    if (lim->skip > 0) {
        size_t k = lim->skip < (int64_t)n ? (size_t)lim->skip : n;
        lim->skip -= (int64_t)k;
        p += k;
        n -= k;
        if (n == 0) return 0;
    }
    if (lim->left <= 0) return 1;
    if ((int64_t)n > lim->left) n = (size_t)lim->left;
    lim->left -= (int64_t)n;
    if (sink_data(lim->sink, p, n)) return 1;
    return lim->left <= 0;
}

typedef struct hls_state {
    fa_hls_playlist pl;
    char media_url[FA_NET_URL_MAX];
    char key_uri[FA_NET_URL_MAX];
    uint8_t key[16];
    int have_key;
    char map_uri[FA_NET_URL_MAX];
    int64_t next_seq;
    int ts_continuity;        /* keep the TS demuxer across segments of one timeline */
    fa_ts_demux ts;
} hls_state;

static fa_result load_media(fa_net* n, hls_state* h)
{
    char* text = NULL;
    size_t len = 0;
    char final_url[FA_NET_URL_MAX];
    fa_result r = fa_net_get_small(n, h->media_url, 0, -1, &text, &len, final_url, sizeof(final_url));
    fa_hls_playlist pl;
    if (r != FA_OK) return r;
    r = fa_hls_parse(text, len, final_url, &pl);
    free(text);
    if (r != FA_OK) return r;
    if (pl.is_master) { fa_hls_playlist_free(&pl); return FA_ERROR_UNSUPPORTED_FORMAT; }
    fa_hls_playlist_free(&h->pl);
    h->pl = pl;
    return FA_OK;
}

/* Downloads one segment into the window. `delivered` counts the audio bytes it
 * has written over all tries, so a retry continues where the last one stopped. */
static fa_result fetch_segment(fa_net* n, hls_state* h, const fa_hls_segment* seg, uint64_t* delivered)
{
    seg_sink sink;
    fa_http_request req;
    fa_result r;
    memset(&sink, 0, sizeof(sink));
    sink.n = n;
    sink.skip = *delivered;
    if (h->ts_continuity) sink.ts = h->ts;
    else fa_ts_init(&sink.ts);
    if (seg->key_method == 1) {
        if (!h->have_key || strcmp(h->key_uri, seg->key_uri) != 0) {
            char* key = NULL;
            size_t klen = 0;
            r = fa_net_get_small(n, seg->key_uri, 0, -1, &key, &klen, NULL, 0);
            if (r != FA_OK) return r;
            if (klen != 16) { free(key); return FA_ERROR_UNSUPPORTED_FORMAT; }
            memcpy(h->key, key, 16);
            free(key);
            snprintf(h->key_uri, sizeof(h->key_uri), "%s", seg->key_uri);
            h->have_key = 1;
        }
        sink.aes = 1;
        aes_init(&sink.key, h->key);
        if (seg->has_iv) memcpy(sink.iv, seg->iv, 16);
        else { int i; for (i = 0; i < 8; i++) sink.iv[15 - i] = (uint8_t)((uint64_t)seg->seq >> (8 * i)); }
    }
    if (seg->map_uri && strcmp(h->map_uri, seg->map_uri) != 0) {
        /* fMP4: the init section goes in front of the first media segment that uses it. */
        char* init = NULL;
        size_t ilen = 0;
        r = fa_net_get_small(n, seg->map_uri, 0, -1, &init, &ilen, NULL, 0);
        if (r != FA_OK) return r;
        if (fa_net_write(n, (const uint8_t*)init, ilen)) { free(init); return FA_ERROR_BUSY; }
        free(init);
        snprintf(h->map_uri, sizeof(h->map_uri), "%s", seg->map_uri);
    }
    fa_net_request(n, &req, seg->uri);
    if (seg->byte_length >= 0) {
        range_limit lim;
        memset(&lim, 0, sizeof(lim));
        lim.sink = &sink;
        lim.offset = seg->byte_offset;
        lim.left = seg->byte_length;
        req.range_start = seg->byte_offset;
        r = fa_http_fetch(&req, limited_head, limited_data, &lim);
        if (r == FA_OK && (lim.bad || lim.left > 0)) r = FA_ERROR_NETWORK;   /* not the whole segment */
    } else {
        r = fa_http_fetch(&req, NULL, sink_data, &sink);
    }
    if (!sink.stopped && r == FA_OK && sink_finish(&sink)) sink.stopped = 1;
    *delivered += sink.delivered;
    if (sink.stopped) return FA_ERROR_BUSY;
    if (r != FA_OK) return r;
    h->ts = sink.ts;            /* only a complete segment moves the demuxer on */
    h->ts_continuity = 1;
    return FA_OK;
}

/* Index of the segment holding `seconds` (VOD). */
static int segment_at(const fa_hls_playlist* pl, double seconds)
{
    int i;
    for (i = 0; i < pl->segment_count; i++)
        if (seconds < pl->segments[i].start + pl->segments[i].duration) return i;
    return pl->segment_count > 0 ? pl->segment_count - 1 : 0;
}

/* Handles a pending seek: the window starts over at 0 with the segment holding
 * the target. Returns the segment index, or -1 if none is pending. */
static int take_seek(fa_net* n, hls_state* h)
{
    int idx = -1;
    fa_mutex_lock(&n->lock);
    if (n->hls_seek) {
        idx = segment_at(&h->pl, n->hls_seek_to);
        n->hls_seek_done = h->pl.segment_count > 0 ? h->pl.segments[idx].start : 0.0;
        n->win_start = n->win_end = n->read_pos = 0;
        n->finished = 0;
        n->starving = 1;
        n->hls_seek = 0;
        fa_store32(&n->cancel, 0);
        fa_cond_broadcast(&n->cond);
    }
    fa_mutex_unlock(&n->lock);
    if (idx >= 0) { h->ts_continuity = 0; h->map_uri[0] = 0; }
    return idx;
}

fa_result fa_hls_run(fa_net* n, const char* text, size_t len, const char* url)
{
    hls_state* h = (hls_state*)calloc(1, sizeof(*h));
    fa_hls_playlist top;
    fa_result r;
    int idx, quit = 0;
    if (!h) return FA_ERROR_OUT_OF_MEMORY;
    r = fa_hls_parse(text, len, url, &top);
    if (r != FA_OK) { free(h); return r; }
    if (top.is_master) {
        int v = fa_hls_pick_variant(&top, n->cfg.hls_max_bandwidth);
        if (top.audio_group_uri) snprintf(h->media_url, sizeof(h->media_url), "%s", top.audio_group_uri);
        else if (v >= 0) snprintf(h->media_url, sizeof(h->media_url), "%s", top.variant_uri[v]);
        fa_hls_playlist_free(&top);
        if (!h->media_url[0]) { free(h); return FA_ERROR_UNSUPPORTED_FORMAT; }
        r = load_media(n, h);
        if (r != FA_OK) { fa_hls_playlist_free(&h->pl); free(h); return r; }
    } else {
        h->pl = top;
        snprintf(h->media_url, sizeof(h->media_url), "%s", url);
    }
    fa_mutex_lock(&n->lock);
    n->hls_duration = h->pl.endlist ? h->pl.total_duration : -1.0;
    n->live = !h->pl.endlist;
    snprintf(n->final_url, sizeof(n->final_url), "%s", h->media_url);
    fa_mutex_unlock(&n->lock);

    idx = h->pl.endlist ? 0 : (h->pl.segment_count > FA_HLS_LIVE_START ? h->pl.segment_count - FA_HLS_LIVE_START : 0);
    h->next_seq = h->pl.segment_count > 0 ? h->pl.segments[idx].seq : h->pl.media_sequence;

    while (!quit) {
        int s = take_seek(n, h);
        if (s >= 0) { idx = s; h->next_seq = h->pl.segments[idx].seq; }
        fa_mutex_lock(&n->lock);
        quit = n->quit;
        fa_mutex_unlock(&n->lock);
        if (quit) break;

        if (idx < h->pl.segment_count) {
            const fa_hls_segment* seg = &h->pl.segments[idx];
            int tries;
            uint64_t delivered = 0;
            if (seg->discontinuity) { h->ts_continuity = 0; h->map_uri[0] = 0; }
            for (tries = 0; tries < FA_HLS_SEGMENT_RETRIES; tries++) {
                r = fetch_segment(n, h, seg, &delivered);
                if (r == FA_OK || r == FA_ERROR_BUSY) break;
                if (fa_net_wait(n, 500u << tries)) break;
            }
            if (r == FA_ERROR_BUSY) continue;          /* a seek or close interrupted it */
            if (r != FA_OK) {
                fa_mutex_lock(&n->lock);
                n->error = r;
                fa_mutex_unlock(&n->lock);
                if (h->pl.endlist) break;              /* VOD: a missing segment ends it */
            }
            h->next_seq = seg->seq + 1;
            idx++;
            continue;
        }
        if (h->pl.endlist) {
            /* Everything is downloaded: wait for a seek or the end. */
            fa_mutex_lock(&n->lock);
            n->finished = 1;
            fa_cond_broadcast(&n->cond);
            fa_mutex_unlock(&n->lock);
            while (!fa_net_wait(n, 200)) { }
            continue;
        }
        /* Live: reload when new segments are due. */
        {
            uint32_t wait_ms = (uint32_t)(h->pl.target_duration > 0 ? h->pl.target_duration * 500.0 : 2000.0);
            int i;
            if (fa_net_wait(n, wait_ms)) continue;
            r = load_media(n, h);
            if (r != FA_OK) {
                fa_mutex_lock(&n->lock);
                n->error = r;
                fa_mutex_unlock(&n->lock);
                continue;
            }
            idx = h->pl.segment_count;
            for (i = 0; i < h->pl.segment_count; i++)
                if (h->pl.segments[i].seq >= h->next_seq) { idx = i; break; }
            /* Fell behind the live window (segments expired): continue at its start. */
            if (h->pl.segment_count > 0 && h->pl.segments[0].seq > h->next_seq) { idx = 0; h->ts_continuity = 0; }
            if (h->pl.endlist) {
                fa_mutex_lock(&n->lock);
                n->live = 0;
                fa_mutex_unlock(&n->lock);
            }
        }
    }
    fa_hls_playlist_free(&h->pl);
    free(h);
    return FA_OK;
}

fa_result fa_net_hls_seek(fa_net* n, double seconds, double* out_start)
{
    fa_result r = FA_OK;
    fa_mutex_lock(&n->lock);
    if (!n->hls || n->hls_duration < 0) { fa_mutex_unlock(&n->lock); return FA_ERROR_NOT_SEEKABLE; }
    n->hls_seek = 1;
    n->hls_seek_to = seconds;
    fa_store32(&n->cancel, 1);
    fa_cond_broadcast(&n->cond);
    fa_mutex_unlock(&n->lock);
    fa_http_abort(&n->cancel);
    fa_mutex_lock(&n->lock);
    while (n->hls_seek && !n->quit && !n->thread_done)
        fa_cond_wait_ms(&n->cond, &n->lock, 50);
    if (n->hls_seek) r = FA_ERROR_NETWORK;
    *out_start = n->hls_seek_done;
    fa_mutex_unlock(&n->lock);
    return r;
}

/* ---- the HLS decoder -----------------------------------------------------------------
 * Wraps the decoder of the segments' format. A seek restarts the producer at the
 * segment holding the target, opens a fresh inner decoder on the new data, and
 * decodes up to the exact frame. */

typedef struct hls_dec {
    fa_net* net;
    fa_source src;
    fa_decoder inner;
    int inner_ready;
    fa_decoder_env env;           /* the stream's, for the inner decoder */
} hls_dec;

static fa_result hls_open_inner(hls_dec* h)
{
    fa_io_callbacks io;
    fa_result r;
    fa_net_io(h->net, &io);
    io.close = NULL;                     /* the stream owns the net */
    fa_source_open_io(&h->src, &io, h->net);
    h->src.remote = 1;
    r = fa_decoder_open(&h->inner, &h->src, NULL, NULL, -1, &h->env);   /* -1: the default track */
    if (r != FA_OK) return r == FA_ERROR_PROTECTED_CONTENT || r == FA_ERROR_PROVIDER_MISSING ? r : FA_ERROR_UNSUPPORTED_FORMAT;
    h->inner_ready = 1;
    return FA_OK;
}

static fa_result hls_open(fa_decoder* dec)
{
    hls_dec* h = (hls_dec*)calloc(1, sizeof(*h));
    double duration;
    fa_result r;
    if (!h) return FA_ERROR_OUT_OF_MEMORY;
    h->net = (fa_net*)dec->src->io_user;
    h->env.dynamic_range = dec->dynamic_range;
    h->env.soundfont = NULL;              /* MIDI is never an HLS segment format */
    h->env.midi_rate = dec->midi_rate;
    h->env.text_code_page = dec->text_code_page;
    r = hls_open_inner(h);
    if (r != FA_OK) { fa_source_close(&h->src); free(h); return r; }
    dec->impl = h;
    dec->format = h->inner.format;
    fa_mutex_lock(&h->net->lock);
    duration = h->net->hls_duration;
    fa_mutex_unlock(&h->net->lock);
    dec->seekable = duration >= 0;
    dec->length = duration >= 0 ? (fa_frames)(duration * dec->format.sample_rate + 0.5) - h->inner.lead_trim : -1;
    if (duration >= 0 && dec->length < 0) dec->length = 0;
    snprintf(dec->codec, sizeof(dec->codec), "%s", h->inner.codec);
    snprintf(dec->container, sizeof(dec->container), "hls");
    return FA_OK;
}

static fa_result hls_read(fa_decoder* dec, float* out, fa_frames frames, fa_frames* got)
{
    hls_dec* h = (hls_dec*)dec->impl;
    if (!h->inner_ready) { *got = 0; return FA_ERROR_IO; }
    return fa_decoder_read(&h->inner, out, frames, got);
}

static fa_result hls_seek(fa_decoder* dec, fa_frames frame)
{
    hls_dec* h = (hls_dec*)dec->impl;
    double start = 0.0;
    fa_frames skip;
    float* scratch;
    fa_result r = fa_net_hls_seek(h->net, (double)frame / dec->format.sample_rate, &start);
    if (r != FA_OK) return r;
    if (h->inner_ready) fa_decoder_close(&h->inner);
    h->inner_ready = 0;
    fa_source_close(&h->src);
    r = hls_open_inner(h);
    if (r != FA_OK) return r;
    if (h->inner.format.sample_rate != dec->format.sample_rate || h->inner.format.channels != dec->format.channels) {
        fa_decoder_close(&h->inner);
        h->inner_ready = 0;
        return FA_ERROR_UNSUPPORTED_FORMAT;
    }
    /* From the segment start to the exact frame. */
    skip = frame - (fa_frames)(start * dec->format.sample_rate + 0.5);
    if (skip <= 0) return FA_OK;
    scratch = (float*)malloc(1024 * sizeof(float) * dec->format.channels);
    if (!scratch) return FA_ERROR_OUT_OF_MEMORY;
    while (skip > 0) {
        fa_frames got = 0;
        fa_decoder_read(&h->inner, scratch, skip < 1024 ? skip : 1024, &got);
        if (got <= 0) break;
        skip -= got;
    }
    free(scratch);
    return FA_OK;
}

static void hls_close(fa_decoder* dec)
{
    hls_dec* h = (hls_dec*)dec->impl;
    if (!h) return;
    if (h->inner_ready) fa_decoder_close(&h->inner);
    fa_source_close(&h->src);
    free(h);
    dec->impl = NULL;
}

static int hls_probe(const uint8_t* data, size_t size, size_t id3) { (void)data; (void)size; (void)id3; return 0; }

/* Not in the registry: URL streams that turn out to be HLS use it directly. */
const fa_decoder_vtable fa_decoder_hls = {
    "hls", "HTTP Live Streaming", "m3u8", "application/vnd.apple.mpegurl;application/x-mpegurl",
    hls_probe, hls_open, hls_read, hls_seek, hls_close, 0, NULL, NULL
};
