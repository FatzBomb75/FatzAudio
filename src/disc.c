/* Audio CDs: drives, table of contents, CD-Text, disc ids, error-tolerant reads,
 * and the decoder behind fa_stream_open_disc. Backends (disc_*.c) only move
 * bytes; everything a disc means is worked out here. */
#include "disc.h"
#include "demux.h"
#include "net.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CD_TEXT_FIELDS 8
#define DISC_MAX_DRIVES 32
#define DISC_CHUNK_SECTORS 32       /* one decoder read from the drive: ~0.43 s */
#define CDTEXT_MAX_BYTES (4 + 18 * 8 * 256)

struct fa_disc {
    const fa_disc_ops* ops;
    void* h;
    fa_mutex lock;                  /* every backend call and everything below */
    fa_atomic32 refs;               /* the app's handle + one per open track stream */
    fa_disc_options opts;

    int locked;
    int c2_ok;                      /* the drive returns C2 pointers (cleared when it refuses) */
    int sub_ok;                     /* the drive returns subcode with audio (likewise) */
    int c2_sub_ok;                  /* ... and both in one read (some drives do one or the other) */
    int last_state;                 /* FA_CD_* from the last test_ready, -1 = never asked */
    uint32_t change_count;
    fa_atomic32 generation;         /* bumped whenever the disc may have changed */

    int toc_valid;
    fa_disc_toc toc;
    char* text[FA_DISC_MAX_TRACKS + 1][CD_TEXT_FIELDS];   /* [0] = album; NULL = none */
    uint8_t code_tried[FA_DISC_MAX_TRACKS + 1];          /* subcode asked for the CODE field */
    uint8_t* bust;                  /* one sector, for cache-busting reads */
};

/* ---- small helpers ---------------------------------------------------------------- */

void fa_disc_copy_trimmed(char* out, size_t cap, const uint8_t* in, size_t n)
{
    size_t i, len = 0;
    if (cap == 0) return;
    for (i = 0; i < n && in[i]; i++) len = i + 1;
    while (len > 0 && in[len - 1] == ' ') len--;
    if (len >= cap) len = cap - 1;
    memcpy(out, in, len);
    out[len] = 0;
}

static int ends_with_ci(const char* s, const char* suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && fa_strncasecmp_ascii(s + n - m, suffix, m) == 0;
}

static void disc_release(fa_disc* d)
{
    int t, f;
    if (fa_fetch_add32(&d->refs, -1) != 1) return;
    if (d->ops->close) d->ops->close(d->h);
    for (t = 0; t <= FA_DISC_MAX_TRACKS; t++)
        for (f = 0; f < CD_TEXT_FIELDS; f++) free(d->text[t][f]);
    free(d->bust);
    fa_mutex_destroy(&d->lock);
    free(d);
}

static void clear_text(fa_disc* d)
{
    int t, f;
    for (t = 0; t <= FA_DISC_MAX_TRACKS; t++) {
        for (f = 0; f < CD_TEXT_FIELDS; f++) { free(d->text[t][f]); d->text[t][f] = NULL; }
        d->code_tried[t] = 0;
    }
}

/* The disc may be another one from now on: forget what was read from it. */
static void disc_forget(fa_disc* d)
{
    if (d->toc_valid) clear_text(d);
    d->toc_valid = 0;
    fa_fetch_add32(&d->generation, 1);
}

/* ---- SHA-1 (MusicBrainz disc ids) ---------------------------------------------- */

typedef struct sha1_ctx { uint32_t h[5]; uint64_t len; uint8_t buf[64]; size_t n; } sha1_ctx;

static uint32_t rol(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }

static void sha1_block(sha1_ctx* c, const uint8_t* p)
{
    uint32_t w[80], a, b, cc, d, e, t;
    int i;
    for (i = 0; i < 16; i++) w[i] = fa_be32(p + i * 4);
    for (i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4];
    for (i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & cc) | (~b & d); k = 0x5A827999u; }
        else if (i < 40) { f = b ^ cc ^ d; k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDCu; }
        else { f = b ^ cc ^ d; k = 0xCA62C1D6u; }
        t = rol(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = rol(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

static void sha1_init(sha1_ctx* c)
{
    c->h[0] = 0x67452301u; c->h[1] = 0xEFCDAB89u; c->h[2] = 0x98BADCFEu; c->h[3] = 0x10325476u; c->h[4] = 0xC3D2E1F0u;
    c->len = 0; c->n = 0;
}

static void sha1_update(sha1_ctx* c, const void* data, size_t n)
{
    const uint8_t* p = (const uint8_t*)data;
    c->len += n;
    while (n > 0) {
        size_t k = 64 - c->n;
        if (k > n) k = n;
        memcpy(c->buf + c->n, p, k);
        c->n += k; p += k; n -= k;
        if (c->n == 64) { sha1_block(c, c->buf); c->n = 0; }
    }
}

static void sha1_final(sha1_ctx* c, uint8_t out[20])
{
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80, zero = 0, lenb[8];
    int i;
    sha1_update(c, &pad, 1);
    while (c->n != 56) sha1_update(c, &zero, 1);
    for (i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(c, lenb, 8);
    for (i = 0; i < 20; i++) out[i] = (uint8_t)(c->h[i / 4] >> (24 - 8 * (i % 4)));
}

/* ---- disc ids ---------------------------------------------------------------------- */

static int digit_sum(int64_t n)
{
    int s = 0;
    while (n > 0) { s += (int)(n % 10); n /= 10; }
    return s;
}

/* freedb: every track of the TOC, and the TOC's lead-out. */
static void make_freedb(fa_disc_toc* toc)
{
    int i, n = 0;
    size_t len;
    int64_t first = toc->tracks[0].start_sector + FA_CD_PREGAP, lead = toc->leadout_sector + FA_CD_PREGAP;
    for (i = 0; i < toc->track_count; i++) n += digit_sum((toc->tracks[i].start_sector + FA_CD_PREGAP) / 75);
    toc->freedb_id = ((uint32_t)(n % 255) << 24) | ((uint32_t)(lead / 75 - first / 75) << 8) | (uint32_t)toc->track_count;
    len = (size_t)snprintf(toc->freedb_query, sizeof(toc->freedb_query), "%08x %d", toc->freedb_id, toc->track_count);
    for (i = 0; i < toc->track_count && len < sizeof(toc->freedb_query); i++)
        len += (size_t)snprintf(toc->freedb_query + len, sizeof(toc->freedb_query) - len, " %lld",
                                (long long)(toc->tracks[i].start_sector + FA_CD_PREGAP));
    if (len < sizeof(toc->freedb_query))
        snprintf(toc->freedb_query + len, sizeof(toc->freedb_query) - len, " %lld", (long long)(lead / 75));
}

/* MusicBrainz: the audio session only (an Enhanced CD's data track is left out and
 * its lead-out moved back by the session gap). `last` and `leadout` say where it ends. */
static void make_musicbrainz(fa_disc_toc* toc, int last, int64_t leadout)
{
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._";
    char text[8 * 102 + 8];
    uint8_t dig[20];
    sha1_ctx c;
    size_t len;
    int t, i, o = 0;
    len = (size_t)snprintf(text, sizeof(text), "%02X%02X%08X", toc->first_track, last, (unsigned)(leadout + FA_CD_PREGAP));
    for (t = 1; t <= 99; t++) {
        uint32_t off = 0;
        int k = t - toc->first_track;
        if (t <= last && k >= 0 && k < toc->track_count) off = (uint32_t)(toc->tracks[k].start_sector + FA_CD_PREGAP);
        len += (size_t)snprintf(text + len, sizeof(text) - len, "%08X", off);
    }
    sha1_init(&c);
    sha1_update(&c, text, len);
    sha1_final(&c, dig);
    for (i = 0; i < 21; i += 3) {
        uint32_t v = ((uint32_t)dig[i] << 16) | ((uint32_t)(i + 1 < 20 ? dig[i + 1] : 0) << 8) | (uint32_t)(i + 2 < 20 ? dig[i + 2] : 0);
        toc->musicbrainz_id[o++] = b64[(v >> 18) & 63];
        toc->musicbrainz_id[o++] = b64[(v >> 12) & 63];
        toc->musicbrainz_id[o++] = i + 1 < 20 ? b64[(v >> 6) & 63] : '-';
        toc->musicbrainz_id[o++] = i + 2 < 20 ? b64[v & 63] : '-';
    }
    toc->musicbrainz_id[o] = 0;
}

/* ---- CD-Text ------------------------------------------------------------------------ */

uint16_t fa_cdtext_crc(const uint8_t* p)
{
    uint16_t crc = 0;
    int i, b;
    for (i = 0; i < 16; i++) {
        crc ^= (uint16_t)(p[i] << 8);
        for (b = 0; b < 8; b++) crc = (uint16_t)((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return (uint16_t)~crc;
}

/* ISO 8859-1 (and ASCII) to UTF-8. */
static char* latin1_to_utf8(const uint8_t* s, size_t n)
{
    char* out = (char*)malloc(n * 2 + 1);
    size_t i, o = 0;
    if (!out) return NULL;
    for (i = 0; i < n; i++) {
        uint8_t ch = s[i];
        if (ch < 0x80) out[o++] = (char)ch;
        else { out[o++] = (char)(0xC0 | (ch >> 6)); out[o++] = (char)(0x80 | (ch & 0x3F)); }
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    out[o] = 0;
    return out;
}

static int pack_ok(const uint8_t* p)
{
    uint16_t stored = (uint16_t)((p[16] << 8) | p[17]);
    return stored == 0 || stored == fa_cdtext_crc(p);  /* some drives return zeroed CRCs */
}

/* Parses the packs of a READ TOC format 5 response into d->text. Only single-byte
 * blocks are read (ISO 8859-1 or ASCII); the first such block wins. */
static int parse_cdtext(fa_disc* d, const uint8_t* resp, uint32_t len)
{
    uint32_t npacks, i;
    const uint8_t* packs = resp + 4;
    int block = -1, b, found = 0;
    if (len < 4 + 18) return 0;
    npacks = (len - 4) / 18;

    /* Pick the block: its size-information pack (0x8F, sequence 0 of the block)
     * names the character set. Without one, block 0 is taken as ISO 8859-1. */
    for (b = 0; b < 8 && block < 0; b++) {
        int seen = 0, charset = -1;
        for (i = 0; i < npacks; i++) {
            const uint8_t* p = packs + i * 18;
            if (((p[3] >> 4) & 7) != b || !pack_ok(p)) continue;
            seen = 1;
            if (p[0] == 0x8F && p[1] == 0 && charset < 0) charset = p[4];
            if (p[3] & 0x80) charset = 0x80;   /* double-byte characters */
        }
        if (seen && (charset < 0 || charset == 0x00 || charset == 0x01)) block = b;
    }
    if (block < 0) return 0;

    for (b = 0x80; b <= 0x8E; b++) {
        uint8_t* buf;
        size_t n = 0, pos;
        int field, track = -1;
        char* prev = NULL;
        if (b >= 0x86 && b != 0x87 && b != 0x8E) continue;   /* disc id, binary and reserved packs */
        field = b <= 0x85 ? b - 0x80 : (b == 0x87 ? FA_DISC_TEXT_GENRE : FA_DISC_TEXT_CODE);
        buf = (uint8_t*)malloc(npacks * 12 + 1);
        if (!buf) return found;
        for (i = 0; i < npacks; i++) {
            const uint8_t* p = packs + i * 18;
            if (p[0] != b || ((p[3] >> 4) & 7) != block || !pack_ok(p)) continue;
            if (track < 0) track = p[1] & 0x7F;
            memcpy(buf + n, p + 4, 12);
            n += 12;
        }
        buf[n] = 0;
        if (track < 0) { free(buf); continue; }
        pos = 0;
        if (field == FA_DISC_TEXT_GENRE) pos = 2;   /* a 2-byte genre code comes first */
        while (pos < n && track <= FA_DISC_MAX_TRACKS) {
            size_t end = pos;
            while (end < n && buf[end]) end++;
            if (end - pos == 1 && buf[pos] == 0x09 && prev) {
                /* TAB: same as the previous track */
                size_t k = strlen(prev) + 1;
                char* copy = (char*)malloc(k);
                if (copy) { memcpy(copy, prev, k); free(d->text[track][field]); d->text[track][field] = copy; }
            } else if (end > pos) {
                char* s = latin1_to_utf8(buf + pos, end - pos);
                if (s && s[0]) {
                    free(d->text[track][field]);
                    d->text[track][field] = s;
                    prev = s;
                    found = 1;
                } else {
                    free(s);
                }
            }
            /* The album's string is followed by the first track's, which need not be 1. */
            track = track == 0 && d->toc.first_track > 1 ? d->toc.first_track : track + 1;
            pos = end + 1;
            if (field == FA_DISC_TEXT_GENRE) break;   /* album only */
        }
        free(buf);
    }
    return found;
}

/* ---- TOC ---------------------------------------------------------------------------- */

/* Asks the drive whether a disc is in and whether it changed, counts the change,
 * and forgets the old disc. An OS call that fails for want of a disc counts as
 * "no disc", not as an error. */
static fa_result poll_locked(fa_disc* d, int* out_state)
{
    int state = FA_CD_NO_DISC, changed = 0, ready;
    fa_result r = d->ops->test_ready(d->h, &state, &changed);
    if (r == FA_ERROR_NO_DISC) { state = FA_CD_NO_DISC; changed = 0; r = FA_OK; }
    if (r != FA_OK) return r;
    ready = state == FA_CD_READY;
    if (d->last_state >= 0 && (changed || (d->last_state == FA_CD_READY) != ready)) d->change_count++;
    if (changed || !ready) disc_forget(d);
    d->last_state = state;
    *out_state = state;
    return FA_OK;
}

static fa_result read_toc_locked(fa_disc* d)
{
    uint8_t* resp;
    uint32_t len = 0, i, n;
    int last_session_track = 0, k, last_audio;
    int64_t audio_lead;
    fa_disc_toc* toc = &d->toc;
    fa_result r;
    int state;

    r = poll_locked(d, &state);
    if (r != FA_OK) return r;
    if (state != FA_CD_READY) return FA_ERROR_NO_DISC;
    if (d->toc_valid) return FA_OK;

    resp = (uint8_t*)malloc(CDTEXT_MAX_BYTES);
    if (!resp) return FA_ERROR_OUT_OF_MEMORY;
    memset(toc, 0, sizeof(*toc));
    clear_text(d);

    r = d->ops->read_toc(d->h, 0, 1, resp, 4 + 8 * 100, &len);
    if (r != FA_OK || len < 4 + 16) { free(resp); return r == FA_OK || r == FA_ERROR_IO ? FA_ERROR_NO_DISC : r; }
    n = (len - 4) / 8;
    toc->first_track = resp[2];
    toc->last_track = resp[3];
    for (i = 0; i < n; i++) {
        const uint8_t* e = resp + 4 + i * 8;
        int64_t lba = (int32_t)fa_be32(e + 4);
        if (e[2] == 0xAA) { toc->leadout_sector = lba; continue; }
        if (e[2] < 1 || e[2] > 99 || toc->track_count >= FA_DISC_MAX_TRACKS) continue;
        {
            fa_disc_track* t = &toc->tracks[toc->track_count++];
            t->number = e[2];
            t->is_audio = (e[1] & 0x04) == 0;
            t->preemphasis = (e[1] & 0x01) != 0;
            t->copy_permitted = (e[1] & 0x02) != 0;
            t->four_channel = (e[1] & 0x08) != 0;
            t->start_sector = lba;
        }
    }
    if (toc->track_count == 0 || toc->leadout_sector <= toc->tracks[0].start_sector) { free(resp); return FA_ERROR_NO_DISC; }

    /* Session info: the first track of the last session. */
    if (d->ops->read_toc(d->h, 1, 0, resp, 12, &len) == FA_OK && len >= 12 && resp[3] > resp[2])
        last_session_track = resp[4 + 2];

    for (k = 0; k < toc->track_count; k++) {
        fa_disc_track* t = &toc->tracks[k];
        int64_t end = k + 1 < toc->track_count ? toc->tracks[k + 1].start_sector : toc->leadout_sector;
        /* An audio track right before a later session also loses the gap between them. */
        if (k + 1 < toc->track_count && toc->tracks[k + 1].number == last_session_track) end -= FA_CD_SESSION_GAP;
        t->sectors = end > t->start_sector ? end - t->start_sector : 0;
        t->frames = t->sectors * FA_DISC_SECTOR_FRAMES;
        t->duration = (fa_time)(t->sectors * 1000000000LL / 75);
        if (t->is_audio) { toc->audio_track_count++; toc->duration += t->duration; }
    }

    make_freedb(toc);
    last_audio = toc->tracks[toc->track_count - 1].number;
    audio_lead = toc->leadout_sector;
    {
        fa_disc_track* lt = &toc->tracks[toc->track_count - 1];
        if (!lt->is_audio && lt->number == last_session_track && toc->track_count > 1) {
            last_audio = lt->number - 1;
            audio_lead = lt->start_sector - FA_CD_SESSION_GAP;
        }
    }
    make_musicbrainz(toc, last_audio, audio_lead);

    if (d->ops->read_toc(d->h, 5, 0, resp, CDTEXT_MAX_BYTES, &len) == FA_OK)
        toc->has_cd_text = parse_cdtext(d, resp, len);
    free(resp);
    d->toc_valid = 1;
    return FA_OK;
}

static const fa_disc_track* find_track(const fa_disc_toc* toc, int32_t number)
{
    int k = number - toc->first_track;
    if (k < 0 || k >= toc->track_count || toc->tracks[k].number != number) return NULL;
    return &toc->tracks[k];
}

/* ---- audio reads ---------------------------------------------------------------- */

static int c2_clean(const uint8_t* c2)
{
    int i;
    for (i = 0; i < FA_CD_C2_BYTES; i++) if (c2[i]) return 0;
    return 1;
}

/* One sector again, after reading a distant one so the drive's cache cannot hand
 * back the same bad copy. */
static fa_result reread_sector(fa_disc* d, int64_t lba, uint32_t areas, uint8_t* out)
{
    int64_t far_lba = lba >= 2000 ? lba - 2000 : lba + 2000;
    if (far_lba < d->toc.leadout_sector && d->bust)
        d->ops->read_cd(d->h, far_lba, 1, FA_CD_AREA_AUDIO, d->bust);
    return d->ops->read_cd(d->h, lba, 1, areas, out);
}

/* After the drive refused a read with C2 pointers and/or subcode: the same read
 * with less, to learn what it cannot do. Updates *c2 / *sub and the drive's flags
 * on success. A drive that reads either one but not both keeps both: reads that
 * want subcode then go without C2. */
static fa_result read_with_less(fa_disc* d, int64_t at, uint32_t n, uint8_t* raw, int* c2, int* sub)
{
    if (*c2 && *sub) {
        if (d->ops->read_cd(d->h, at, n, FA_CD_AREA_AUDIO | FA_CD_AREA_SUB, raw) == FA_OK) {
            d->c2_sub_ok = 0;
            *c2 = 0;
            return FA_OK;
        }
        if (d->ops->read_cd(d->h, at, n, FA_CD_AREA_AUDIO | FA_CD_AREA_C2, raw) == FA_OK) {
            d->sub_ok = 0;
            *sub = 0;
            return FA_OK;
        }
    }
    if (d->ops->read_cd(d->h, at, n, FA_CD_AREA_AUDIO, raw) != FA_OK) return FA_ERROR_NOT_IMPLEMENTED;
    if (*c2) d->c2_ok = 0;
    if (*sub) d->sub_ok = 0;
    *c2 = *sub = 0;
    return FA_OK;
}

/* Reads `count` sectors from `lba` as 16-bit stereo samples into `out` (count *
 * 1176 values), using `raw` (DISC_CHUNK_SECTORS full sectors) as scratch. With
 * `sub_out`, the sectors' subcode too (count * 96 bytes; *out_sub = 1 when the
 * drive delivered it).
 * Sectors outside [lo, hi), the audio around the track, read as silence. *out_bad
 * counts sectors that stayed damaged (played as read) or unreadable (silence).
 * The disc lock is held per drive command, not for the whole chunk, so status
 * polls and the tray never wait for a long run of retries. */
static fa_result disc_read_audio(fa_disc* d, int32_t gen, int64_t lba, uint32_t count, int64_t lo, int64_t hi,
                                 uint8_t* raw, int16_t* out, uint8_t* sub_out, int* out_sub, uint32_t* out_bad)
{
    uint32_t done = 0;
    int sub_all = sub_out != NULL;  /* subcode came with every read (outside the audio: zeros) */
    *out_bad = 0;
    memset(out, 0, (size_t)count * FA_CD_SECTOR_BYTES);
    if (sub_out) memset(sub_out, 0, (size_t)count * FA_CD_SUB_BYTES);

    while (done < count) {
        int64_t at = lba + done;
        uint32_t n = count - done, i, stride, areas, sub_at;
        int c2, sub;
        fa_result r;
        if (at < lo || at >= hi) { done++; continue; }   /* silence */
        if (at + n > hi) n = (uint32_t)(hi - at);
        if (n > DISC_CHUNK_SECTORS) n = DISC_CHUNK_SECTORS;

        fa_mutex_lock(&d->lock);
        if (fa_load32(&d->generation) != gen) { fa_mutex_unlock(&d->lock); return FA_ERROR_NO_DISC; }
        sub = sub_out && d->sub_ok;
        c2 = d->c2_ok && (!sub || d->c2_sub_ok);
        r = d->ops->read_cd(d->h, at, n, FA_CD_AREA_AUDIO | (c2 ? FA_CD_AREA_C2 : 0) | (sub ? FA_CD_AREA_SUB : 0), raw);
        /* The drive refuses C2 pointers or subcode, not this read: go on without. */
        if (r == FA_ERROR_NOT_IMPLEMENTED && (c2 || sub) && read_with_less(d, at, n, raw, &c2, &sub) == FA_OK) r = FA_OK;
        if (r == FA_ERROR_NO_DISC) disc_forget(d);
        fa_mutex_unlock(&d->lock);
        if (r == FA_ERROR_NO_DISC) return r;
        areas = FA_CD_AREA_AUDIO | (c2 ? FA_CD_AREA_C2 : 0) | (sub ? FA_CD_AREA_SUB : 0);
        sub_at = FA_CD_SECTOR_BYTES + (c2 ? FA_CD_C2_BYTES : 0);
        if (!sub) sub_all = 0;
        stride = sub_at + (sub ? FA_CD_SUB_BYTES : 0);

        for (i = 0; i < n; i++) {
            uint8_t* sec = raw + (size_t)i * stride;
            int ok = r == FA_OK && (!c2 || c2_clean(sec + FA_CD_SECTOR_BYTES));
            if (!ok) {
                /* Damaged, or the whole command failed: this sector on its own, again.
                 * After a failed command each sector needs one read before retries. */
                int readable = r == FA_OK;
                uint32_t attempts = d->opts.retries + (r == FA_OK ? 0 : 1), tries;
                for (tries = 0; tries < attempts && !ok; tries++) {
                    fa_result rr;
                    fa_mutex_lock(&d->lock);
                    if (fa_load32(&d->generation) != gen) rr = FA_ERROR_NO_DISC;
                    else if ((rr = reread_sector(d, at + i, areas, sec)) == FA_ERROR_NO_DISC) disc_forget(d);
                    fa_mutex_unlock(&d->lock);
                    if (rr == FA_ERROR_NO_DISC) return rr;
                    if (rr == FA_OK) {
                        readable = 1;
                        ok = !c2 || c2_clean(sec + FA_CD_SECTOR_BYTES);
                    }
                }
                if (!ok) {
                    (*out_bad)++;
                    if (!readable) memset(sec, 0, stride);
                }
            }
            if (sub) memcpy(sub_out + (size_t)(done + i) * FA_CD_SUB_BYTES, sec + sub_at, FA_CD_SUB_BYTES);
            {
                int16_t* o = out + (size_t)(done + i) * (FA_CD_SECTOR_BYTES / 2);
                int k;
                for (k = 0; k < FA_CD_SECTOR_BYTES / 2; k++) o[k] = (int16_t)(sec[2 * k] | (sec[2 * k + 1] << 8));
            }
        }
        done += n;
    }
    if (out_sub) *out_sub = sub_all;
    return FA_OK;
}

/* ---- the track decoder -------------------------------------------------------- */

typedef struct disc_dec {
    fa_disc* disc;
    struct fa_stream* stream;
    int64_t start_lba;
    int32_t gen;
    int32_t offset;
    fa_frames pos;                  /* next frame of the track */
    int64_t lo, hi;                 /* the readable audio around the track (sectors) */
    int16_t* cache;                 /* DISC_CHUNK_SECTORS sectors of samples */
    uint8_t* raw;                   /* DISC_CHUNK_SECTORS raw sectors with C2 and subcode */
    uint8_t* sub;                   /* DISC_CHUNK_SECTORS sectors of subcode */
    fa_disc_cdg* cdg;               /* CD+G from the subcode; NULL = not read */
    int64_t cache_lba;
    uint32_t cache_n;               /* sectors in the cache, 0 = empty */
    int deemph;
    float b0, b1, a1;               /* de-emphasis filter */
    float x1[2], y1[2];
} disc_dec;

/* CD de-emphasis: the 50 us / 15 us shelf. The pole is matched (z = e^(-T/t1)), the
 * zero is placed so that the gain is exact at DC (0 dB) and at half the sample
 * rate (where the analog curve is still falling), which keeps the whole response
 * within a fraction of a dB of the analog one. */
static void deemph_init(disc_dec* x, double rate)
{
    const double pi = 3.14159265358979323846, t1 = 50e-6, t2 = 15e-6;
    double w = pi * rate;
    double target = sqrt((1.0 + w * w * t2 * t2) / (1.0 + w * w * t1 * t1));
    double p = exp(-1.0 / (t1 * rate));
    double ratio = target * (1.0 + p) / (1.0 - p);
    double q = (ratio - 1.0) / (ratio + 1.0);
    double g = (1.0 - p) / (1.0 - q);
    x->b0 = (float)g;
    x->b1 = (float)(-g * q);
    x->a1 = (float)(-p);
}

static int64_t floor_div(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

static void post_read_error(disc_dec* x)
{
    fa_event ev;
    if (!x->stream) return;
    ev.type = FA_EVENT_DECODE_ERROR;
    ev.stream = x->stream;
    ev.frame = fa_stream_position_now(x->stream);
    ev.value = FA_ERROR_IO;
    fa_engine_post_event(x->stream->engine, &ev);
}

static fa_result disc_dec_read(fa_decoder* dec, float* out, fa_frames frames, fa_frames* got)
{
    disc_dec* x = (disc_dec*)dec->impl;
    *got = 0;
    /* Another disc (or the same one put back): this stream is over. */
    if (fa_load32(&x->disc->generation) != x->gen) return FA_ERROR_NO_DISC;
    while (*got < frames && x->pos < dec->length) {
        int64_t abs = x->start_lba * FA_DISC_SECTOR_FRAMES + x->pos + x->offset;
        int64_t idx, take, i;
        if (!x->cache_n || abs < x->cache_lba * FA_DISC_SECTOR_FRAMES ||
            abs >= (x->cache_lba + x->cache_n) * FA_DISC_SECTOR_FRAMES) {
            uint32_t bad = 0;
            fa_result r;
            x->cache_lba = floor_div(abs, FA_DISC_SECTOR_FRAMES);
            x->cache_n = 0;
            int got_sub = 0, want_sub = fa_disc_cdg_wanted(x->cdg);
            r = disc_read_audio(x->disc, x->gen, x->cache_lba, DISC_CHUNK_SECTORS, x->lo, x->hi, x->raw, x->cache,
                                want_sub ? x->sub : NULL, &got_sub, &bad);
            if (r != FA_OK) return *got > 0 ? FA_OK : r;
            if (got_sub) fa_disc_cdg_feed(x->cdg, x->cache_lba, DISC_CHUNK_SECTORS, x->sub);
            else if (want_sub) { fa_disc_cdg_free(x->cdg); x->cdg = NULL; }   /* the drive can't */
            x->cache_n = DISC_CHUNK_SECTORS;
            if (bad) post_read_error(x);
        }
        idx = abs - x->cache_lba * FA_DISC_SECTOR_FRAMES;
        take = (x->cache_lba + x->cache_n) * FA_DISC_SECTOR_FRAMES - abs;
        if (take > frames - *got) take = frames - *got;
        if (take > dec->length - x->pos) take = dec->length - x->pos;
        for (i = 0; i < take; i++) {
            float* o = out + (*got + i) * 2;
            int c;
            for (c = 0; c < 2; c++) {
                float v = (float)x->cache[(idx + i) * 2 + c] / 32768.0f;
                if (x->deemph) {
                    float y = x->b0 * v + x->b1 * x->x1[c] - x->a1 * x->y1[c];
                    x->x1[c] = v;
                    x->y1[c] = y;
                    v = y;
                }
                o[c] = v;
            }
        }
        *got += take;
        x->pos += take;
    }
    return *got > 0 ? FA_OK : FA_ERROR_END_OF_STREAM;
}

static fa_result disc_dec_seek(fa_decoder* dec, fa_frames frame)
{
    disc_dec* x = (disc_dec*)dec->impl;
    x->pos = frame;
    x->x1[0] = x->x1[1] = x->y1[0] = x->y1[1] = 0.0f;
    return FA_OK;
}

static void disc_dec_close(fa_decoder* dec)
{
    disc_dec* x = (disc_dec*)dec->impl;
    if (!x) return;
    disc_release(x->disc);
    free(x->cache);
    free(x->raw);
    free(x->sub);
    fa_disc_cdg_free(x->cdg);
    free(x);
    dec->impl = NULL;
}

static int disc_dec_probe(const uint8_t* data, size_t size, size_t id3) { (void)data; (void)size; (void)id3; return 0; }
static fa_result disc_dec_open(fa_decoder* dec) { (void)dec; return FA_ERROR_UNSUPPORTED_FORMAT; }

/* Not in the registry: only fa_stream_open_disc creates it. */
static const fa_decoder_vtable disc_decoder = {
    "cdda", "Audio CD track", "cda", "", disc_dec_probe, disc_dec_open,
    disc_dec_read, disc_dec_seek, disc_dec_close, 1, NULL
};

fa_result fa_disc_decoder_open(fa_decoder* dec, fa_source* src, fa_disc* d, int32_t track, struct fa_stream* stream)
{
    const fa_disc_track* t;
    disc_dec* x;
    fa_result r;
    if (!dec || !d) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&d->lock);
    r = read_toc_locked(d);
    if (r != FA_OK) { fa_mutex_unlock(&d->lock); return r; }
    t = find_track(&d->toc, track);
    if (!t) { fa_mutex_unlock(&d->lock); return FA_ERROR_NOT_FOUND; }
    if (!t->is_audio || t->four_channel || t->sectors <= 0) { fa_mutex_unlock(&d->lock); return FA_ERROR_UNSUPPORTED_FORMAT; }
    x = (disc_dec*)calloc(1, sizeof(*x));
    if (x) {
        x->cache = (int16_t*)malloc((size_t)DISC_CHUNK_SECTORS * FA_CD_SECTOR_BYTES);
        x->raw = (uint8_t*)malloc((size_t)DISC_CHUNK_SECTORS * (FA_CD_SECTOR_BYTES + FA_CD_C2_BYTES + FA_CD_SUB_BYTES));
        x->sub = (uint8_t*)malloc((size_t)DISC_CHUNK_SECTORS * FA_CD_SUB_BYTES);
    }
    if (!x || !x->cache || !x->raw || !x->sub) {
        fa_mutex_unlock(&d->lock);
        if (x) { free(x->cache); free(x->raw); free(x->sub); free(x); }
        return FA_ERROR_OUT_OF_MEMORY;
    }
    {
        /* Offset reads may reach into the neighbouring audio tracks, but never into a
         * data track, a session's lead-out or the gap before the next session. */
        int k = (int)(t - d->toc.tracks), first = k, last = k;
        while (first > 0 && d->toc.tracks[first - 1].is_audio) first--;
        while (last + 1 < d->toc.track_count && d->toc.tracks[last + 1].is_audio) last++;
        x->lo = d->toc.tracks[first].start_sector;
        x->hi = d->toc.tracks[last].start_sector + d->toc.tracks[last].sectors;
    }
    x->disc = d;
    x->stream = stream;
    x->start_lba = t->start_sector;
    x->gen = fa_load32(&d->generation);
    x->offset = d->opts.read_offset;
    x->deemph = t->preemphasis && !(d->opts.flags & FA_DISC_NO_DEEMPHASIS);
    if (stream && d->sub_ok && !(stream->flags & FA_OPEN_NO_CDG))
        x->cdg = fa_disc_cdg_create(stream, t->start_sector, t->sectors);
    deemph_init(x, 44100.0);
    memset(dec, 0, sizeof(*dec));
    dec->vt = &disc_decoder;
    dec->src = src;
    dec->impl = x;
    dec->format.sample_format = FA_FORMAT_F32;
    dec->format.sample_rate = 44100;
    dec->format.channels = 2;
    dec->length = t->frames;
    dec->seekable = 1;
    dec->audio_track = -1;
    dec->track_count = 1;
    snprintf(dec->codec, sizeof(dec->codec), "cdda");
    snprintf(dec->container, sizeof(dec->container), "cd");
    fa_fetch_add32(&d->refs, 1);
    fa_mutex_unlock(&d->lock);
    return FA_OK;
}

int fa_disc_cda_track(const uint8_t* p, size_t n)
{
    int track;
    if (n < 44 || memcmp(p, "RIFF", 4) != 0 || memcmp(p + 8, "CDDAfmt ", 8) != 0) return 0;
    track = p[22] | (p[23] << 8);
    return track >= 1 && track <= 99 ? track : 0;
}

/* ---- drives ------------------------------------------------------------------------ */

static fa_atomic32 g_list_lock;
static fa_disc_drive_info g_drives[DISC_MAX_DRIVES];
static uint32_t g_drive_count;

static void list_lock(void) { while (!fa_cas32(&g_list_lock, 0, 1)) fa_sleep_ms(1); }
static void list_unlock(void) { fa_store32(&g_list_lock, 0); }

FA_API fa_result FA_CALL fa_disc_drive_count(uint32_t* out_count)
{
    char (*ids)[256];
    fa_disc_drive_info* found;
    uint32_t n, i;
    if (!out_count) return FA_ERROR_INVALID_ARG;
    ids = (char (*)[256])malloc(sizeof(*ids) * DISC_MAX_DRIVES);
    found = (fa_disc_drive_info*)calloc(DISC_MAX_DRIVES, sizeof(*found));
    if (!ids || !found) { free(ids); free(found); return FA_ERROR_OUT_OF_MEMORY; }
    /* Probing a drive can take seconds; the list lock is taken only to publish. */
    n = fa_disc_platform_list(ids, DISC_MAX_DRIVES);
    if (n > DISC_MAX_DRIVES) n = DISC_MAX_DRIVES;
    for (i = 0; i < n; i++) {
        fa_disc_drive_info* info = &found[i];
        const fa_disc_ops* ops;
        void* h;
        memcpy(info->id, ids[i], sizeof(info->id));
        info->id[sizeof(info->id) - 1] = 0;
        if (fa_disc_platform_open(ids[i], &ops, &h) == FA_OK) {
            if (ops->inquiry) ops->inquiry(h, info->vendor, info->product, info->revision);
            ops->close(h);
        }
        {
            char name[sizeof(info->name)];
            if (info->vendor[0] && info->product[0]) snprintf(name, sizeof(name), "%s %s", info->vendor, info->product);
            else if (info->product[0]) snprintf(name, sizeof(name), "%s", info->product);
            else fa_disc_copy_trimmed(name, sizeof(name), (const uint8_t*)info->id, strlen(info->id));
            memcpy(info->name, name, sizeof(name));
        }
    }
    list_lock();
    memcpy(g_drives, found, sizeof(*found) * DISC_MAX_DRIVES);
    g_drive_count = n;
    list_unlock();
    free(ids);
    free(found);
    *out_count = n;
    return FA_OK;
}

FA_API fa_result FA_CALL fa_disc_drive_get_info(uint32_t index, fa_disc_drive_info* out_info)
{
    fa_result r = FA_OK;
    if (!out_info) return FA_ERROR_INVALID_ARG;
    list_lock();
    if (index >= g_drive_count) r = FA_ERROR_NOT_FOUND;
    else *out_info = g_drives[index];
    list_unlock();
    return r;
}

/* ---- open / close ---------------------------------------------------------------- */

FA_API void FA_CALL fa_disc_options_init(fa_disc_options* options)
{
    if (!options) return;
    memset(options, 0, sizeof(*options));
    options->struct_size = (uint32_t)sizeof(*options);
    options->retries = 5;
}

fa_result fa_disc_open_ops(const fa_disc_ops* ops, void* h, const fa_disc_options* options, fa_disc** out_disc)
{
    fa_disc* d = (fa_disc*)calloc(1, sizeof(*d));
    if (!d || fa_mutex_init(&d->lock) != 0) {
        free(d);
        ops->close(h);
        return d ? FA_ERROR_GENERIC : FA_ERROR_OUT_OF_MEMORY;
    }
    d->ops = ops;
    d->h = h;
    fa_disc_options_init(&d->opts);
    if (options && options->struct_size >= offsetof(fa_disc_options, flags) + sizeof(options->flags)) {
        /* Fields added later are read only when options->struct_size covers them. */
        d->opts.read_offset = options->read_offset;
        d->opts.retries = options->retries > 100 ? 100 : options->retries;
        d->opts.flags = options->flags;
    }
    d->c2_ok = !(d->opts.flags & FA_DISC_NO_C2);
    d->sub_ok = !(d->opts.flags & FA_DISC_NO_CDG);
    d->c2_sub_ok = 1;
    d->last_state = -1;
    d->bust = (uint8_t*)malloc(FA_CD_SECTOR_BYTES + FA_CD_C2_BYTES + FA_CD_SUB_BYTES);
    fa_store32(&d->refs, 1);
    *out_disc = d;
    return FA_OK;
}

FA_API fa_result FA_CALL fa_disc_open(const char* drive_id, const fa_disc_options* options, fa_disc** out_disc)
{
    const fa_disc_ops* ops;
    void* h;
    fa_result r;
    char first[1][256];
    if (!out_disc) return FA_ERROR_INVALID_ARG;
    *out_disc = NULL;
    if (options && options->struct_size < offsetof(fa_disc_options, flags) + sizeof(options->flags)) return FA_ERROR_INVALID_ARG;
    if (!drive_id) {
        if (fa_disc_platform_list(first, 1) == 0) return FA_ERROR_NOT_FOUND;
        drive_id = first[0];
    }
    if (ends_with_ci(drive_id, ".cue")) r = fa_disc_image_open(drive_id, &ops, &h);
    else r = fa_disc_platform_open(drive_id, &ops, &h);
    if (r != FA_OK) return r;
    return fa_disc_open_ops(ops, h, options, out_disc);
}

FA_API void FA_CALL fa_disc_close(fa_disc* disc)
{
    if (!disc) return;
    fa_mutex_lock(&disc->lock);
    if (disc->locked && disc->ops->lock) disc->ops->lock(disc->h, 0);
    disc->locked = 0;
    fa_mutex_unlock(&disc->lock);
    disc_release(disc);
}

/* ---- status and TOC ---------------------------------------------------------------- */

FA_API fa_result FA_CALL fa_disc_get_status(fa_disc* disc, fa_disc_status* out_status)
{
    int state = FA_CD_NO_DISC;
    fa_result r;
    if (!disc || !out_status) return FA_ERROR_INVALID_ARG;
    memset(out_status, 0, sizeof(*out_status));
    fa_mutex_lock(&disc->lock);
    r = poll_locked(disc, &state);
    if (r == FA_OK) {
        out_status->state = (fa_disc_state)state;
        out_status->locked = disc->locked;
        out_status->change_count = disc->change_count;
    }
    fa_mutex_unlock(&disc->lock);
    return r;
}

FA_API fa_result FA_CALL fa_disc_get_toc(fa_disc* disc, fa_disc_toc* out_toc)
{
    fa_result r;
    if (!disc || !out_toc) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&disc->lock);
    r = read_toc_locked(disc);
    if (r == FA_OK) *out_toc = disc->toc;
    else memset(out_toc, 0, sizeof(*out_toc));
    fa_mutex_unlock(&disc->lock);
    return r;
}

FA_API fa_result FA_CALL fa_disc_get_text(fa_disc* disc, int32_t track, fa_disc_text field, char* out_text, size_t text_size)
{
    fa_result r;
    const char* s;
    if (!disc || !out_text || text_size == 0 || track < 0 || track > FA_DISC_MAX_TRACKS ||
        (int)field < 0 || (int)field >= CD_TEXT_FIELDS)
        return FA_ERROR_INVALID_ARG;
    out_text[0] = 0;
    fa_mutex_lock(&disc->lock);
    r = read_toc_locked(disc);
    if (r == FA_OK && track > 0 && !find_track(&disc->toc, track)) r = FA_ERROR_NOT_FOUND;
    if (r == FA_OK && field == FA_DISC_TEXT_CODE && !disc->text[track][field] && !disc->code_tried[track] &&
        disc->ops->read_code) {
        char code[32];
        disc->code_tried[track] = 1;
        if (disc->ops->read_code(disc->h, track == 0 ? FA_CD_CODE_MCN : FA_CD_CODE_ISRC, track, code, sizeof(code)) == FA_OK &&
            code[0]) {
            size_t n = strlen(code) + 1;
            disc->text[track][field] = (char*)malloc(n);
            if (disc->text[track][field]) memcpy(disc->text[track][field], code, n);
        }
    }
    if (r == FA_OK) {
        s = disc->text[track][field];
        if (!s) r = FA_ERROR_NOT_FOUND;
        else {
            /* Truncate on a UTF-8 character boundary. */
            size_t n = strlen(s);
            if (n >= text_size) {
                n = text_size - 1;
                while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
            }
            memcpy(out_text, s, n);
            out_text[n] = 0;
        }
    }
    fa_mutex_unlock(&disc->lock);
    return r;
}

/* ---- drive control ------------------------------------------------------------------ */

FA_API fa_result FA_CALL fa_disc_eject(fa_disc* disc)
{
    fa_result r;
    if (!disc) return FA_ERROR_INVALID_ARG;
    if (!disc->ops->tray) return FA_ERROR_NOT_IMPLEMENTED;
    fa_mutex_lock(&disc->lock);
    if (disc->ops->lock) disc->ops->lock(disc->h, 0);
    disc->locked = 0;
    r = disc->ops->tray(disc->h, 1);
    disc_forget(disc);
    fa_mutex_unlock(&disc->lock);
    return r;
}

FA_API fa_result FA_CALL fa_disc_load(fa_disc* disc)
{
    fa_result r;
    if (!disc) return FA_ERROR_INVALID_ARG;
    if (!disc->ops->tray) return FA_ERROR_NOT_IMPLEMENTED;
    fa_mutex_lock(&disc->lock);
    r = disc->ops->tray(disc->h, 0);
    fa_mutex_unlock(&disc->lock);
    return r;
}

FA_API fa_result FA_CALL fa_disc_set_locked(fa_disc* disc, int32_t locked)
{
    fa_result r;
    if (!disc) return FA_ERROR_INVALID_ARG;
    if (!disc->ops->lock) return FA_ERROR_NOT_IMPLEMENTED;
    fa_mutex_lock(&disc->lock);
    r = disc->ops->lock(disc->h, locked ? 1 : 0);
    if (r == FA_OK) disc->locked = locked ? 1 : 0;
    fa_mutex_unlock(&disc->lock);
    return r;
}

FA_API fa_result FA_CALL fa_disc_set_speed(fa_disc* disc, float speed)
{
    fa_result r;
    uint32_t kbps;
    if (!disc || !(speed >= 0.0f) || speed > 100.0f) return FA_ERROR_INVALID_ARG;
    if (!disc->ops->set_speed) return FA_ERROR_NOT_IMPLEMENTED;
    kbps = speed <= 0.0f ? 0xFFFFu : (uint32_t)(speed * 176.4f + 0.5f);
    fa_mutex_lock(&disc->lock);
    r = disc->ops->set_speed(disc->h, kbps);
    fa_mutex_unlock(&disc->lock);
    return r;
}

FA_API fa_result FA_CALL fa_disc_get_speed(fa_disc* disc, float* out_speed)
{
    fa_result r;
    uint32_t kbps = 0;
    if (!disc || !out_speed) return FA_ERROR_INVALID_ARG;
    *out_speed = 0.0f;
    if (!disc->ops->get_speed) return FA_ERROR_NOT_IMPLEMENTED;
    fa_mutex_lock(&disc->lock);
    r = disc->ops->get_speed(disc->h, &kbps);
    fa_mutex_unlock(&disc->lock);
    if (r == FA_OK) *out_speed = (float)kbps / 176.4f;
    return r;
}
