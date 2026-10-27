/* MP3 files: our own framing around minimp3's frame decoder (from miniaudio).
 *
 * Seeking is exact and fast. The decoder learns where every frame starts by
 * hopping from header to header (no decoding), keeps the offset of every 16th
 * frame, and on a seek decodes only the few frames before the target that the
 * bit reservoir and the filter bank overlap need. A file with a Xing/Info header
 * is hopped lazily, up to where a seek goes; a file without one is hopped whole
 * at open for its length (as FA_OPEN_PRESCAN does for any file).
 *
 * Frame k always holds decoded samples [k * spf, (k + 1) * spf), even when a
 * damaged frame decodes to nothing (it plays as silence), so a seek lands on
 * the same audio as decoding from the start. The LAME header's encoder delay
 * and padding are trimmed; without one only the decoder's own 529-sample delay
 * is, so the first sample is the first one the encoder was given.
 *
 * Network sources and free-format files use dr_mp3 (dec_miniaudio.c). */
#include "fa_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MP3_WINDOW 4096          /* bytes read at a time; a frame and the next header are at most 2885 */
#define MP3_INDEX_STEP 16        /* frames per index entry */
#define MP3_RESYNC 65536         /* junk bytes skipped looking for the next frame */
#define MP3_DECODER_DELAY 529    /* samples the synthesis filter bank delays the audio by */
#define MP3_PREROLL_MAX 14       /* frames that can feed the bit reservoir of a frame */

typedef struct mp3_hdr {
    int mpeg1;
    uint32_t rate;
    int channels;
    int size;                    /* bytes, with padding */
    int payload;                 /* bytes after header, CRC and side info */
    uint8_t key[3];              /* version, layer and rate bits: must match the first frame */
} mp3_hdr;

typedef struct mp3_dec {
    void* mp3;                   /* minimp3 (fa_mp3dec_*, miniaudio_impl.c) */
    fa_source* src;
    uint8_t win[MP3_WINDOW];
    int64_t win_pos;             /* file offset of win[0] */
    size_t win_len;
    int win_eof;                 /* the source ended at win_pos + win_len */
    float pcm[1152 * 2];
    int pcm_n, pcm_at;           /* samples per channel in pcm[] and consumed */
    int64_t frame;               /* the frame in pcm[] (-1 none) */
    int64_t next_off;            /* where frame + 1 starts, -1 past the end */
    uint8_t key[3];
    int spf;                     /* samples per frame */
    uint32_t channels;
    int64_t skip;                /* decoded samples before the audio (delays) */
    int64_t end;                 /* decoded samples up to the end of the audio, -1 = to the last frame */
    /* Frame offsets: frames 0..known-1 have been visited in order from the start,
     * frame `known` starts at known_off (-1: the file ends after known frames). */
    int64_t* index;              /* offset of frame i * MP3_INDEX_STEP */
    size_t index_n, index_cap;
    int64_t known, known_off;
} mp3_dec;

static const uint16_t kbps_v1[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
static const uint16_t kbps_v2[16] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 };
static const uint32_t rates[3] = { 44100, 48000, 32000 };

/* A Layer III header with a bitrate (free format is left to dr_mp3). */
static int parse_hdr(const uint8_t* p, mp3_hdr* h)
{
    int version, br, sr, pad, side;
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return 0;
    version = (p[1] >> 3) & 3;           /* 0 MPEG-2.5, 2 MPEG-2, 3 MPEG-1 */
    br = p[2] >> 4;
    sr = (p[2] >> 2) & 3;
    if (version == 1 || ((p[1] >> 1) & 3) != 1 || br == 0 || br == 15 || sr == 3 || (p[3] & 3) == 2) return 0;
    pad = (p[2] >> 1) & 1;
    h->mpeg1 = version == 3;
    h->rate = rates[sr] >> (version == 3 ? 0 : version == 2 ? 1 : 2);
    h->channels = (p[3] >> 6) == 3 ? 1 : 2;
    h->size = h->mpeg1 ? 144000 * kbps_v1[br] / (int)h->rate + pad : 72000 * kbps_v2[br] / (int)h->rate + pad;
    side = h->mpeg1 ? (h->channels == 1 ? 17 : 32) : (h->channels == 1 ? 9 : 17);
    h->payload = h->size - 4 - ((p[1] & 1) ? 0 : 2) - side;
    h->key[0] = p[1] & 0xFE;             /* sync, version, layer */
    h->key[1] = p[2] & 0x0C;             /* rate */
    h->key[2] = 0;
    return h->payload > 0;
}

static int same_stream(const mp3_dec* m, const mp3_hdr* h)
{
    return h->key[0] == m->key[0] && h->key[1] == m->key[1];
}

/* Makes win[] hold the file from `off` on, at least `need` bytes unless the file
 * ends first. Returns a pointer to `off` and the bytes there in *avail. */
static const uint8_t* window(mp3_dec* m, int64_t off, size_t need, size_t* avail)
{
    if (off >= m->win_pos && off <= m->win_pos + (int64_t)m->win_len) {
        size_t at = (size_t)(off - m->win_pos);
        if (m->win_len - at >= need || m->win_eof) {
            *avail = m->win_len - at;
            return m->win + at;
        }
        memmove(m->win, m->win + at, m->win_len - at);
        m->win_len -= at;
        m->win_pos = off;
    } else {
        m->win_pos = off;
        m->win_len = 0;
        m->win_eof = 0;
    }
    if (fa_source_tell(m->src) != m->win_pos + (int64_t)m->win_len &&
        fa_source_seek(m->src, m->win_pos + (int64_t)m->win_len) != FA_OK)
        m->win_eof = 1;
    while (!m->win_eof && m->win_len < MP3_WINDOW) {
        int64_t r = fa_source_read(m->src, m->win + m->win_len, MP3_WINDOW - m->win_len);
        if (r <= 0) m->win_eof = 1;
        else m->win_len += (size_t)r;
    }
    *avail = m->win_len;
    return m->win;
}

/* The frame at `off`, or the next one after junk: a header of this stream that
 * is followed by another one (or by the end of the file). Returns its offset, or
 * -1 at the end. */
static int64_t locate(mp3_dec* m, int64_t off, mp3_hdr* h)
{
    int64_t limit = off + MP3_RESYNC;
    size_t n;
    const uint8_t* p = window(m, off, 4, &n);
    if (n >= 4 && parse_hdr(p, h) && same_stream(m, h)) return off;
    for (off++; off < limit; off++) {
        mp3_hdr next;
        p = window(m, off, 4, &n);
        if (n < 4) return -1;
        if (!parse_hdr(p, h) || !same_stream(m, h)) continue;
        p = window(m, off, (size_t)h->size + 4, &n);
        if (n == (size_t)h->size && m->win_eof) return off;          /* the last frame */
        if (n >= (size_t)h->size + 4 && parse_hdr(p + h->size, &next) && same_stream(m, &next)) return off;
    }
    return -1;
}

/* Records that frame `known` starts at `off` and moves past it. */
static int learn(mp3_dec* m, int64_t off, const mp3_hdr* h)
{
    if (m->known % MP3_INDEX_STEP == 0) {
        if (m->index_n == m->index_cap) {
            size_t cap = m->index_cap ? m->index_cap * 2 : 256;
            int64_t* p = (int64_t*)realloc(m->index, cap * sizeof(int64_t));
            if (!p) return 0;
            m->index = p;
            m->index_cap = cap;
        }
        m->index[m->index_n++] = off;
    }
    m->known++;
    m->known_off = off + h->size;
    return 1;
}

/* Hops from header to header until frame `frame` is known or the file ends. */
static fa_result hop_to(mp3_dec* m, int64_t frame)
{
    while (m->known <= frame && m->known_off >= 0) {
        mp3_hdr h;
        int64_t off = locate(m, m->known_off, &h);
        if (off < 0) { m->known_off = -1; break; }
        if (!learn(m, off, &h)) return FA_ERROR_OUT_OF_MEMORY;
    }
    return FA_OK;
}

/* All frames known: the length is now exact. */
static void hop_all(fa_decoder* dec)
{
    mp3_dec* m = (mp3_dec*)dec->impl;
    if (!dec->seekable || hop_to(m, INT64_MAX - 1) != FA_OK) return;
    if (m->end < 0 || m->end > m->known * m->spf) m->end = m->known * m->spf;
    dec->length = m->end > m->skip ? (fa_frames)(m->end - m->skip) : 0;
}

/* Decodes the frame at `off` into pcm[] as frame number `frame`. */
static void decode_at(mp3_dec* m, int64_t frame, int64_t off, const mp3_hdr* h)
{
    size_t n;
    const uint8_t* p = window(m, off, (size_t)h->size, &n);
    int got = 0, channels = 0, rate = 0, i;
    if (n >= (size_t)h->size) fa_mp3dec_decode(m->mp3, p, h->size, m->pcm, &got, &channels, &rate);
    if (got != m->spf) {
        /* Damaged, or its bit reservoir is missing: silence keeps the timeline. */
        memset(m->pcm, 0, (size_t)m->spf * m->channels * sizeof(float));
    } else if ((uint32_t)channels != m->channels) {
        if (m->channels == 2)
            for (i = m->spf - 1; i >= 0; i--) m->pcm[2 * i] = m->pcm[2 * i + 1] = m->pcm[i];
        else
            for (i = 0; i < m->spf; i++) m->pcm[i] = 0.5f * (m->pcm[2 * i] + m->pcm[2 * i + 1]);
    }
    m->frame = frame;
    m->pcm_n = m->spf;
    m->pcm_at = 0;
    m->next_off = off + h->size;
}

/* Decodes the frame after the one in pcm[]. 0 at the end of the file. */
static int decode_next(mp3_dec* m)
{
    mp3_hdr h = { 0 };
    int64_t frame = m->frame + 1, off;
    if (m->next_off < 0) return 0;
    off = locate(m, m->next_off, &h);
    if (off < 0) {
        m->next_off = -1;
        if (m->known == frame) m->known_off = -1;
        return 0;
    }
    if (m->known == frame && !learn(m, off, &h)) return 0;
    decode_at(m, frame, off, &h);
    return 1;
}

static fa_result mp3_read(fa_decoder* dec, float* out, fa_frames frames, fa_frames* got)
{
    mp3_dec* m = (mp3_dec*)dec->impl;
    fa_frames done = 0;
    while (done < frames) {
        int64_t at, n;
        if (m->pcm_at >= m->pcm_n && !decode_next(m)) break;
        at = m->frame * m->spf + m->pcm_at;
        if (at < m->skip) {
            int64_t s = m->skip - at;
            m->pcm_at += (int)(s < m->pcm_n - m->pcm_at ? s : m->pcm_n - m->pcm_at);
            continue;
        }
        if (m->end >= 0 && at >= m->end) break;
        n = m->pcm_n - m->pcm_at;
        if (n > frames - done) n = frames - done;
        if (m->end >= 0 && n > m->end - at) n = m->end - at;
        memcpy(out + (size_t)done * m->channels, m->pcm + (size_t)m->pcm_at * m->channels,
               (size_t)n * m->channels * sizeof(float));
        m->pcm_at += (int)n;
        done += n;
    }
    *got = done;
    return done > 0 ? FA_OK : FA_ERROR_END_OF_STREAM;
}

static fa_result mp3_seek(fa_decoder* dec, fa_frames frame)
{
    mp3_dec* m = (mp3_dec*)dec->impl;
    int64_t at = (int64_t)frame + m->skip, target, k, off, offs[MP3_PREROLL_MAX + 1];
    mp3_hdr hdrs[MP3_PREROLL_MAX + 1], h = { 0 };
    int n = 0, i, first, need, have;
    fa_result r;
    if (!dec->seekable) return FA_ERROR_NOT_SEEKABLE;
    if (frame < 0) return FA_ERROR_INVALID_ARG;
    target = at / m->spf;
    /* Close ahead: decoding on costs less than starting over. */
    if (m->frame >= 0 && m->pcm_n > 0 && target >= m->frame && target <= m->frame + 2) {
        while (m->frame < target && decode_next(m)) {}
        m->pcm_at = m->frame == target ? (int)(at - target * m->spf) : m->pcm_n;
        return FA_OK;
    }
    r = hop_to(m, target);
    if (r != FA_OK) return r;
    if (target >= m->known) {         /* past the end: reads return the end */
        m->frame = m->known - 1;
        m->pcm_n = m->pcm_at = 0;
        m->next_off = -1;
        return FA_OK;
    }
    /* The offsets of the MP3_PREROLL_MAX frames before the target, hopping from
     * the nearest index entry. */
    k = target - MP3_PREROLL_MAX;
    if (k < 0) k = 0;
    k -= k % MP3_INDEX_STEP;
    off = m->index[k / MP3_INDEX_STEP];
    for (; k <= target; k++) {
        off = locate(m, off, &h);
        if (off < 0) return FA_ERROR_IO;   /* the frames were there when hopped */
        if (target - k <= MP3_PREROLL_MAX) {
            offs[n] = off;
            hdrs[n++] = h;
        }
        off += h.size;
    }
    /* Frames target-2 and target-1 must decode fully (the filter bank remembers
     * two MPEG-2 frames back), so the frames before them must hold the largest
     * bit reservoir the stream allows. */
    first = n - 3;
    if (first < 0) first = 0;
    need = h.mpeg1 ? 511 : 255;
    for (have = 0; first > 0 && have < need;) have += hdrs[--first].payload;
    fa_mp3dec_reset(m->mp3);
    for (i = first; i < n; i++) decode_at(m, target - (n - 1 - i), offs[i], &hdrs[i]);
    m->pcm_at = (int)(at - target * m->spf);
    return FA_OK;
}

static void mp3_free(mp3_dec* m)
{
    if (!m) return;
    free(m->index);
    free(m->mp3);
    free(m);
}

static void mp3_close(fa_decoder* dec)
{
    mp3_free((mp3_dec*)dec->impl);
    dec->impl = NULL;
}

/* The Xing/Info frame (a LAME or FFmpeg header instead of audio): frame count,
 * encoder delay and padding. Returns 1 if the frame at p is one. */
static int xing(const uint8_t* p, const mp3_hdr* h, int64_t* frames, int* delay, int* padding)
{
    const uint8_t* t = p + 4 + ((p[1] & 1) ? 0 : 2) + (h->mpeg1 ? (h->channels == 1 ? 17 : 32) : (h->channels == 1 ? 9 : 17));
    const uint8_t* end = p + h->size;
    uint32_t flags;
    if (t + 8 > end) return 0;
    if (memcmp(t, "Xing", 4) != 0 && memcmp(t, "Info", 4) != 0) return 0;
    flags = fa_be32(t + 4);
    t += 8;
    if (flags & 1) {
        if (t + 4 > end) return 1;
        *frames = ((int64_t)t[0] << 24) | ((int64_t)t[1] << 16) | ((int64_t)t[2] << 8) | t[3];
        t += 4;
    }
    if (flags & 2) t += 4;
    if (flags & 4) t += 100;
    if (flags & 8) t += 4;
    /* LAME extension: 9 bytes of encoder name, then 12 more, then 12 bits of delay and 12 of padding. */
    if (t + 24 <= end && t[0]) {
        *delay = (t[21] << 4) | (t[22] >> 4);
        *padding = ((t[22] & 0x0F) << 8) | t[23];
    }
    return 1;
}

static fa_result mp3_open(fa_decoder* dec)
{
    const uint8_t* head;
    size_t n = fa_source_peek(dec->src, &head);
    mp3_dec* m;
    mp3_hdr h = { 0 };
    int64_t off, id3, xframes = -1;
    int delay = -1, padding = 0, found = 0;
    const uint8_t* p;
    size_t avail;

    if (dec->src->remote) return fa_mp3_open_fallback(dec);
    m = (mp3_dec*)calloc(1, sizeof(mp3_dec));
    if (!m) return FA_ERROR_OUT_OF_MEMORY;
    m->src = dec->src;
    m->frame = -1;
    m->win_pos = -1;
    m->mp3 = fa_mp3dec_create();
    if (!m->mp3) { free(m); return FA_ERROR_OUT_OF_MEMORY; }

    /* The first frame: a header followed by another of the same stream (or by
     * the end of the file). */
    id3 = (int64_t)fa_probe_id3v2_size(head, n);
    for (off = id3; off <= id3 + MP3_RESYNC; off++) {
        mp3_hdr next;
        p = window(m, off, 4, &avail);
        if (avail < 4) break;
        if (!parse_hdr(p, &h)) continue;
        p = window(m, off, (size_t)h.size + 4, &avail);
        if (avail == (size_t)h.size && m->win_eof) { found = 1; break; }
        if (avail >= (size_t)h.size + 4 && parse_hdr(p + h.size, &next) && next.key[0] == h.key[0] &&
            next.key[1] == h.key[1]) { found = 1; break; }
    }
    if (!found) {
        /* No Layer III frames with a bitrate (free format), or a lone frame
         * before a tag: dr_mp3 decides. */
        mp3_free(m);
        fa_source_seek(dec->src, 0);
        return fa_mp3_open_fallback(dec);
    }
    m->key[0] = h.key[0];
    m->key[1] = h.key[1];
    m->spf = h.mpeg1 ? 1152 : 576;
    m->channels = (uint32_t)h.channels;
    p = window(m, off, (size_t)h.size, &avail);
    if (avail >= (size_t)h.size && xing(p, &h, &xframes, &delay, &padding)) {
        off += h.size;
        dec->lead_bytes = h.size;   /* not audio: left out of the bitrate */
    }
    m->known_off = off;
    m->next_off = off;

    if (delay >= 0) {
        m->skip = delay + MP3_DECODER_DELAY;
        if (xframes >= 0) {
            m->end = xframes * m->spf - (padding > MP3_DECODER_DELAY ? padding - MP3_DECODER_DELAY : 0);
            if (m->end < m->skip) m->end = m->skip;
        } else {
            m->end = -1;
        }
    } else {
        m->skip = MP3_DECODER_DELAY;
        m->end = xframes >= 0 ? xframes * m->spf : -1;
    }

    dec->impl = m;
    dec->format.sample_rate = h.rate;
    dec->format.channels = m->channels;
    dec->seekable = dec->src->seekable;
    snprintf(dec->codec, sizeof(dec->codec), "mp3");
    if (m->end >= 0) dec->length = m->end > m->skip ? (fa_frames)(m->end - m->skip) : 0;
    else hop_all(dec);   /* no frame count in the file: counted once now */
    return FA_OK;
}

const fa_decoder_vtable fa_decoder_mp3 = {
    "mp3", "MPEG-1/2/2.5 Layer III", "mp3",
    "audio/mpeg;audio/mp3;audio/x-mp3",
    fa_probe_mp3, mp3_open, mp3_read, mp3_seek, mp3_close, 1, NULL, hop_all
};
