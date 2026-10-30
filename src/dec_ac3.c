/* Dolby Digital and Dolby Digital Plus: the packet codec ("ac3", "eac3") used by
 * MP4, Matroska and the FFmpeg hand-off, and the reader of raw .ac3 / .ec3
 * streams (files, web streams, AC-3 in HLS). The decoding itself is ac3.c.
 *
 * The codec gathers bytes into access units (an independent frame of program 0
 * with what follows it up to the next one), so packets split anywhere still
 * decode. FFmpeg is never used: ac3.c decodes every coding tool.
 *
 * The raw reader hands out one access unit per packet. Local files are indexed
 * once by hopping from header to header (exact length and seeks); network files
 * seek by byte position from the bitrate; live streams read straight through,
 * sliding over junk to the next frame whose successor is a frame too. */
#include "ac3.h"
#include "demux.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AC3_MAX_AU 65536            /* bytes: several 4 KB E-AC-3 frames */
#define AC3_RESYNC 65536            /* junk bytes skipped looking for a frame */
#define AC3_MAX_INDEX (1 << 24)     /* index limit: 149 hours at 32 ms per unit */

/* ---- codec -------------------------------------------------------------------- */

typedef struct ac3_codec {
    fa_ac3* dec;
    uint8_t* buf;          /* bytes not decoded yet */
    size_t len, cap;
    float* pcm;
    size_t pcm_cap;
    float* pend;           /* a decoded unit in a new format, handed out next call */
    size_t pend_cap;
    fa_frames pend_frames;
    uint32_t pend_channels, pend_rate;
    int has_dep;           /* the stream has dependent substreams */
} ac3_codec;

static fa_result append_out(fa_codec* c, const float* pcm, fa_frames frames, uint32_t channels, uint32_t rate)
{
    fa_result r;
    if (frames <= 0) return FA_OK;
    r = fa_codec_reserve(c, frames, channels);
    if (r != FA_OK) return r;
    c->channels = channels;
    c->rate = rate;
    memcpy(c->out + (size_t)c->out_frames * channels, pcm, (size_t)frames * channels * sizeof(float));
    c->out_frames += frames;
    return FA_OK;
}

/* Keeps decoded audio whose format differs from what this call hands out. */
static fa_result hold(ac3_codec* st, const float* pcm, fa_frames frames, uint32_t channels, uint32_t rate)
{
    size_t need = (size_t)frames * channels;
    if (need > st->pend_cap) {
        float* p = (float*)realloc(st->pend, need * sizeof(float));
        if (!p) return FA_ERROR_OUT_OF_MEMORY;
        st->pend = p;
        st->pend_cap = need;
    }
    memcpy(st->pend, pcm, need * sizeof(float));
    st->pend_frames = frames;
    st->pend_channels = channels;
    st->pend_rate = rate;
    return FA_OK;
}

/* Output in a new format while this call already has audio: it waits for the next call. */
static int format_change(const fa_codec* c, uint32_t channels, uint32_t rate)
{
    return c->out_frames > 0 && (channels != c->channels || rate != c->rate);
}

/* Length of the access unit at buf[pos..len): 0 if it is not complete yet (more
 * bytes may follow), or if `ending` and no frame is complete. Skips junk before it. */
static size_t next_unit(ac3_codec* st, size_t* pos, int ending)
{
    for (;;) {
        fa_ac3_header h;
        size_t q;
        if (*pos + 8 > st->len) return 0;
        if (!fa_ac3_parse_header(st->buf + *pos, st->len - *pos, &h)) {
            (*pos)++;
            continue;
        }
        if (!fa_ac3_starts_unit(&h)) {
            /* A frame without its independent one (joined mid-unit): skipped whole. */
            if (h.eac3 && h.strmtyp == 1) st->has_dep = 1;
            if (h.frame_size > st->len - *pos) {
                if (ending) *pos = st->len;
                return 0;
            }
            *pos += h.frame_size;
            continue;
        }
        if (h.frame_size > st->len - *pos) {
            if (ending) { *pos = st->len; }
            return 0;
        }
        q = *pos + h.frame_size;
        /* Dependent frames and other programs belong to this unit. */
        for (;;) {
            fa_ac3_header n;
            if (q + 8 > st->len) {
                /* The end of what we have: the unit ends here unless bytes of a
                 * frame are pending (more of it comes with the next packet), or the
                 * stream has dependent frames and this unit has none yet. (Packets
                 * that split units this way lose the dependent frames of the first
                 * unit only: containers keep a unit in one packet, and waiting
                 * before anything is known would shift their timing.) */
                if (!ending && (q < st->len || (st->has_dep && q == *pos + h.frame_size))) return 0;
                break;
            }
            if (!fa_ac3_parse_header(st->buf + q, st->len - q, &n) || fa_ac3_starts_unit(&n)) break;
            if (n.eac3 && n.strmtyp == 1) st->has_dep = 1;
            if (n.frame_size > st->len - q) {
                if (!ending) return 0;
                break;
            }
            q += n.frame_size;
        }
        return q - *pos;
    }
}

static fa_result ac3c_decode(fa_codec* c, const uint8_t* data, size_t size)
{
    ac3_codec* st = (ac3_codec*)c->impl;
    size_t pos = 0;
    int ending = !data && size == 0;
    c->out_frames = 0;
    if (st->pend_frames > 0) {
        fa_result r = append_out(c, st->pend, st->pend_frames, st->pend_channels, st->pend_rate);
        if (r != FA_OK) return r;
        st->pend_frames = 0;
    }
    if (st->len + size > st->cap) {
        size_t cap = st->cap ? st->cap : 4096;
        uint8_t* b;
        while (cap < st->len + size) cap *= 2;
        b = (uint8_t*)realloc(st->buf, cap);
        if (!b) return FA_ERROR_OUT_OF_MEMORY;
        st->buf = b;
        st->cap = cap;
    }
    if (size > 0) memcpy(st->buf + st->len, data, size);
    st->len += size;
    /* Units until the data runs out, or one comes out in a new format (it is
     * held, and the bytes after it wait for the next call). */
    while (st->pend_frames == 0) {
        size_t n = next_unit(st, &pos, ending);
        int frames = 0, channels = 0;
        uint32_t rate = 0;
        fa_result r;
        if (n == 0) break;
        {
            /* The usual case, one unit per packet: straight into the output. */
            int direct = c->out_frames == 0;
            size_t cap = direct ? (size_t)c->out_cap : st->pcm_cap;
            float** dst = direct ? &c->out : &st->pcm;
            r = fa_ac3_decode(st->dec, st->buf + pos, n, c->dynamic_range ? fa_load32(c->dynamic_range) : FA_AC3_DRC_OFF,
                              dst, &cap, &frames, &channels, &rate);
            if (direct) c->out_cap = (fa_frames)cap;
            else st->pcm_cap = cap;
            if (r == FA_ERROR_OUT_OF_MEMORY) return r;
            pos += n;
            if (frames <= 0) continue;
            if (direct) {
                c->out_frames = frames;
                c->channels = (uint32_t)channels;
                c->rate = rate;
                r = FA_OK;
            } else {
                r = format_change(c, (uint32_t)channels, rate) ? hold(st, st->pcm, frames, (uint32_t)channels, rate)
                                                                : append_out(c, st->pcm, frames, (uint32_t)channels, rate);
            }
        }
        if (r != FA_OK) return r;
    }
    if (pos > st->len) pos = st->len;
    if (st->len > pos) memmove(st->buf, st->buf + pos, st->len - pos);   /* buf is NULL before the first byte */
    st->len -= pos;
    /* Garbage that never syncs (bytes held back behind a new format are not). */
    if (st->pend_frames == 0 && st->len > AC3_MAX_AU) st->len = 0;
    return FA_OK;
}

static void ac3c_flush(fa_codec* c)
{
    ac3_codec* st = (ac3_codec*)c->impl;
    st->len = 0;
    st->pend_frames = 0;
    fa_ac3_reset(st->dec);
}

static void ac3c_close(fa_codec* c)
{
    ac3_codec* st = (ac3_codec*)c->impl;
    if (!st) return;
    fa_ac3_destroy(st->dec);
    free(st->buf);
    free(st->pcm);
    free(st->pend);
    free(st);
    c->impl = NULL;
}

static const fa_codec_vtable ac3c_vtable = { "ac3", ac3c_decode, ac3c_flush, ac3c_close };

fa_result fa_ac3_codec_open(fa_codec* c, const fa_codec_params* p)
{
    ac3_codec* st = (ac3_codec*)calloc(1, sizeof(*st));
    if (!st) return FA_ERROR_OUT_OF_MEMORY;
    st->dec = fa_ac3_create();
    if (!st->dec) { free(st); return FA_ERROR_OUT_OF_MEMORY; }
    c->impl = st;
    c->vt = &ac3c_vtable;
    c->rate = p->sample_rate;
    c->channels = p->channels;
    return FA_OK;
}

/* Samples in a packet (program 0's blocks), for containers that round timestamps. */
int64_t fa_ac3_packet_samples(const uint8_t* d, size_t n, uint32_t* out_rate)
{
    size_t pos = 0;
    int64_t samples = 0;
    while (pos + 8 <= n) {
        fa_ac3_header h;
        if (!fa_ac3_parse_header(d + pos, n - pos, &h)) break;
        if (fa_ac3_starts_unit(&h)) {
            samples += (int64_t)h.blocks * 256;
            *out_rate = h.rate;
        }
        pos += h.frame_size;
    }
    return samples > 0 ? samples : -1;
}

/* ---- raw stream reader ---------------------------------------------------------------- */

typedef struct ac3_unit {
    int64_t off;
    int64_t pts;
} ac3_unit;

typedef struct ac3_raw {
    fa_br br;
    fa_track track;
    uint8_t* data;          /* the unit being handed out */
    size_t data_cap;
    uint8_t h[10];          /* bytes of the next header already read (10: an ID3 tag header) */
    size_t h_n;
    int64_t data_start;
    int64_t pts;            /* of the next unit, in samples at `rate` */
    uint32_t rate;
    int eac3;
    int in_flow;            /* the next byte is where the last frame ended */
    int may_seek;
    ac3_unit* index;
    size_t nindex;
    int64_t cbr;            /* every unit one frame of this size (no index needed) */
    int64_t cbr_units;
    int cbr_samples;
    double bytes_per_sample;
    int64_t est_samples;
} ac3_raw;

/* Frames chained from d[i]: `want` headers, each where the previous frame ends
 * (a frame reaching the end of the window counts). */
static int chain(const uint8_t* d, size_t n, size_t i, int want)
{
    int k;
    for (k = 0; k < want; k++) {
        fa_ac3_header h;
        if (i + 8 > n) return k > 0;
        if (!fa_ac3_parse_header(d + i, n - i, &h)) return 0;
        i += h.frame_size;
    }
    return 1;
}

static int raw_probe(const uint8_t* d, size_t n, size_t id3)
{
    size_t i;
    if (id3 >= n) return 0;
    for (i = id3; i + 8 <= n && i < id3 + 4096; i++) {
        fa_ac3_header h;
        if (!fa_ac3_parse_header(d + i, n - i, &h)) continue;
        if (i + h.frame_size + 8 <= n) {
            if (!chain(d, n, i, 3)) continue;
            return i == id3 ? 95 : 70;
        }
        if (i == id3) return 50;
    }
    return 0;
}

static int fill_h(ac3_raw* a, size_t n)
{
    if (a->h_n < n) a->h_n += fa_br_read(&a->br, a->h + a->h_n, n - a->h_n);
    return a->h_n >= n;
}

/* Is a frame (header h at stream position `at`) followed by another frame, or by
 * the end? 1 yes, 0 no, -1 can't tell without waiting for the network. */
static int followed(ac3_raw* a, int64_t at, const fa_ac3_header* h)
{
    fa_source* src = a->br.src;
    const uint8_t* d;
    size_t n = fa_source_peek_held(src, &d);
    int64_t next = at + (int64_t)h->frame_size;
    fa_ac3_header h2;
    if (src->length > 0 && next == src->length) return 1;
    if (next + 8 <= (int64_t)n) return fa_ac3_parse_header(d + next, n - (size_t)next, &h2);
    if (a->may_seek) {
        uint8_t nh[8];
        int ok;
        if (fa_br_seek(&a->br, next) != FA_OK) return 0;
        ok = fa_br_read(&a->br, nh, 8) == 8 && fa_ac3_parse_header(nh, 8, &h2);
        if (fa_br_seek(&a->br, at) != FA_OK) return 0;
        a->h_n = 0;
        if (!fill_h(a, 8)) return 0;
        return ok;
    }
    return -1;
}

/* Leaves the next frame's header in a->h. Right after a frame any header is
 * taken; after junk, a seek or at the start it must be followed by another frame
 * where that can be checked. ID3 tags (between HLS segments) are skipped. */
static fa_result raw_sync(ac3_raw* a, fa_ac3_header* out)
{
    size_t skipped = 0;
    for (;;) {
        if (!fill_h(a, 8)) return FA_ERROR_END_OF_STREAM;
        if (fa_ac3_parse_header(a->h, 8, out)) {
            if (a->in_flow && skipped == 0) break;
            if (followed(a, fa_br_tell(&a->br) - (int64_t)a->h_n, out) != 0) break;
        } else if (memcmp(a->h, "ID3", 3) == 0 && fill_h(a, 10)) {
            size_t tag = fa_probe_id3v2_size(a->h, 10);
            if (tag >= 10) {
                int64_t rest = (int64_t)tag - 10;
                a->h_n = 0;
                while (rest > 0) {
                    uint8_t tmp[512];
                    size_t k = rest < (int64_t)sizeof(tmp) ? (size_t)rest : sizeof(tmp);
                    size_t g = fa_br_read(&a->br, tmp, k);
                    if (g == 0) break;
                    rest -= (int64_t)g;
                }
                continue;
            }
        }
        if (++skipped > AC3_RESYNC) return FA_ERROR_END_OF_STREAM;
        memmove(a->h, a->h + 1, a->h_n - 1);
        a->h_n--;
    }
    a->in_flow = 1;
    return FA_OK;
}

/* Reads the frame whose header is in a->h into a->data at `at`. */
static fa_result read_frame(ac3_raw* a, const fa_ac3_header* h, size_t at)
{
    size_t have;
    if (at + h->frame_size > a->data_cap) {
        size_t c = a->data_cap ? a->data_cap : 4096;    /* an AC-3 or E-AC-3 frame is at most 4096 bytes */
        uint8_t* p;
        while (c < at + h->frame_size) c *= 2;
        p = (uint8_t*)realloc(a->data, c);
        if (!p) return FA_ERROR_OUT_OF_MEMORY;
        a->data = p;
        a->data_cap = c;
    }
    have = a->h_n < h->frame_size ? a->h_n : h->frame_size;
    memcpy(a->data + at, a->h, have);
    memmove(a->h, a->h + have, a->h_n - have);
    a->h_n -= have;
    if (fa_br_read(&a->br, a->data + at + have, h->frame_size - have) != h->frame_size - have)
        return FA_ERROR_END_OF_STREAM;
    return FA_OK;
}

static fa_result raw_next(void* ctx, fa_demux_packet* out)
{
    ac3_raw* a = (ac3_raw*)ctx;
    fa_ac3_header h;
    size_t len = 0;
    int64_t samples = 0;
    fa_result r;
    /* The unit's first frame: frames of other programs or dependent frames
     * without their base (a stream joined mid-way) are passed over. */
    for (;;) {
        r = raw_sync(a, &h);
        if (r != FA_OK) return r;
        if (fa_ac3_starts_unit(&h)) break;
        r = read_frame(a, &h, 0);
        if (r != FA_OK) return r;
    }
    r = read_frame(a, &h, 0);
    if (r != FA_OK) return r;
    len = h.frame_size;
    samples = (int64_t)h.blocks * 256;
    if (h.rate != a->rate) samples = samples * a->rate / h.rate;
    /* What follows up to the next unit. */
    for (;;) {
        fa_ac3_header n;
        if (!fill_h(a, 8) || !fa_ac3_parse_header(a->h, 8, &n) || fa_ac3_starts_unit(&n)) break;
        if (len + n.frame_size > AC3_MAX_AU) break;
        if (read_frame(a, &n, len) != FA_OK) break;
        len += n.frame_size;
    }
    out->data = a->data;
    out->size = len;
    out->pts = a->pts;
    out->duration = samples;
    a->pts += samples;
    return FA_OK;
}

static fa_result raw_seek_packets(void* ctx, int64_t t, int packets_back, size_t bytes_back)
{
    ac3_raw* a = (ac3_raw*)ctx;
    int64_t off, pts;
    fa_result r;
    (void)bytes_back;
    if (a->index) {
        size_t lo = 0, hi = a->nindex;
        while (hi - lo > 1) {
            size_t mid = lo + (hi - lo) / 2;
            if (a->index[mid].pts <= t) lo = mid;
            else hi = mid;
        }
        lo = lo >= (size_t)packets_back ? lo - (size_t)packets_back : 0;
        off = a->index[lo].off;
        pts = a->index[lo].pts;
    } else if (a->cbr) {
        int64_t u = t > 0 ? t / a->cbr_samples : 0;
        if (u >= a->cbr_units) u = a->cbr_units - 1;
        u = u >= packets_back ? u - packets_back : 0;
        off = a->data_start + u * a->cbr;
        pts = u * a->cbr_samples;
    } else {
        if (!a->br.src->seekable || a->bytes_per_sample <= 0) return FA_ERROR_NOT_SEEKABLE;
        pts = t - (int64_t)packets_back * 1536;
        if (pts < 0) pts = 0;
        off = a->data_start + (int64_t)((double)pts * a->bytes_per_sample);
        if (a->br.src->length > 0 && off >= a->br.src->length) off = a->br.src->length - 1;
    }
    r = fa_br_seek(&a->br, off);
    if (r != FA_OK) return r;
    a->h_n = 0;
    a->pts = pts;
    a->in_flow = a->index != NULL || a->cbr;
    a->may_seek = 1;
    return FA_OK;
}

static const fa_demux_ops raw_ops = { raw_next, raw_seek_packets };

/* Local files where every frame is a unit of one size (constant bit rate, one
 * substream: nearly every AC-3 file): the data is a whole number of frames, and
 * the second, the last and 32 frames spread in between have the first one's
 * size and rate. Then unit k is at data_start + k * size, with no index to
 * build. (A stretch of other frames that happens to keep every checked frame in
 * place goes unnoticed: seeks into it land on the next frame, a little off.) */
#define AC3_CBR_CHECKS 32
static int check_cbr(ac3_raw* a, const fa_ac3_header* first, int64_t* out_end)
{
    int64_t len = a->br.src->length, size = (int64_t)first->frame_size, n;
    int i;
    if (len <= a->data_start || size <= 0 || (len - a->data_start) % size != 0) return 0;
    n = (len - a->data_start) / size;
    for (i = 0; i <= AC3_CBR_CHECKS + 1; i++) {
        uint8_t hb[8];
        fa_ac3_header h;
        int64_t at = i == 0 ? 1 : i == AC3_CBR_CHECKS + 1 ? n - 1 : n * i / (AC3_CBR_CHECKS + 1);
        if (at >= n) at = n - 1;
        if (fa_br_seek(&a->br, a->data_start + at * size) != FA_OK || fa_br_read(&a->br, hb, 8) != 8 ||
            !fa_ac3_parse_header(hb, 8, &h) || !fa_ac3_starts_unit(&h) || h.frame_size != first->frame_size ||
            h.rate != first->rate || h.blocks != first->blocks || h.eac3 != first->eac3)
            return 0;
    }
    a->cbr = size;
    a->cbr_units = n;
    a->cbr_samples = first->blocks * 256;
    a->bytes_per_sample = (double)size / (double)a->cbr_samples;
    *out_end = n * a->cbr_samples;
    return 1;
}

/* Local files: every unit's offset and time, hopping from header to header (junk
 * between frames is slid over). Very long files fall back to estimates. */
static fa_result build_index(ac3_raw* a, int64_t* out_end)
{
    size_t cap = 0;
    int64_t pos = a->data_start, pts = 0, len = a->br.src->length;
    for (;;) {
        fa_ac3_header h;
        uint8_t hb[8];
        if (fa_br_seek(&a->br, pos) != FA_OK || fa_br_read(&a->br, hb, 8) != 8) break;
        if (!fa_ac3_parse_header(hb, 8, &h)) {
            int64_t p, stop = pos + AC3_RESYNC;
            int found = 0;
            for (p = pos + 1; p < stop && (len <= 0 || p + 8 <= len); p++) {
                if (fa_br_seek(&a->br, p) != FA_OK || fa_br_read(&a->br, hb, 8) != 8) break;
                if (fa_ac3_parse_header(hb, 8, &h)) {
                    uint8_t nb[8];
                    fa_ac3_header h2;
                    int64_t next = p + (int64_t)h.frame_size;
                    if ((len > 0 && next == len) ||
                        (fa_br_seek(&a->br, next) == FA_OK && fa_br_read(&a->br, nb, 8) == 8 && fa_ac3_parse_header(nb, 8, &h2))) {
                        found = 1;
                        break;
                    }
                }
            }
            if (!found) break;   /* the end (tags, junk) */
            pos = p;
        }
        if (len > 0 && pos + (int64_t)h.frame_size > len) break;   /* cut short */
        if (fa_ac3_starts_unit(&h)) {
            if (a->nindex == cap) {
                ac3_unit* u;
                if (cap >= AC3_MAX_INDEX) {
                    a->bytes_per_sample = (double)(pos - a->data_start) / (double)pts;
                    free(a->index);
                    a->index = NULL;
                    a->nindex = 0;
                    *out_end = FA_DEMUX_NO_PTS;
                    return FA_OK;
                }
                cap = cap ? cap * 2 : 1024;
                u = (ac3_unit*)realloc(a->index, cap * sizeof(*u));
                if (!u) { free(a->index); a->index = NULL; a->nindex = 0; return FA_ERROR_OUT_OF_MEMORY; }
                a->index = u;
            }
            a->index[a->nindex].off = pos;
            a->index[a->nindex].pts = pts;
            a->nindex++;
            pts += h.rate == a->rate ? (int64_t)h.blocks * 256 : (int64_t)h.blocks * 256 * a->rate / h.rate;
        }
        pos += (int64_t)h.frame_size;
    }
    *out_end = pts;
    if (a->nindex == 0) {
        free(a->index);
        a->index = NULL;
        return FA_ERROR_UNSUPPORTED_FORMAT;
    }
    if (pts > 0) a->bytes_per_sample = (double)(pos - a->data_start) / (double)pts;
    return FA_OK;
}

/* Network files and the app's callbacks: bytes per sample over the first units. */
static void estimate_rate(ac3_raw* a)
{
    int64_t pos = a->data_start, bytes = 0, samples = 0;
    int k;
    for (k = 0; k < 64 && bytes < 262144; k++) {
        uint8_t hb[8];
        fa_ac3_header h;
        if (fa_br_seek(&a->br, pos) != FA_OK || fa_br_read(&a->br, hb, 8) != 8) break;
        if (!fa_ac3_parse_header(hb, 8, &h)) break;
        bytes += (int64_t)h.frame_size;
        if (fa_ac3_starts_unit(&h)) samples += (int64_t)h.blocks * 256;
        pos += (int64_t)h.frame_size;
    }
    if (samples >= 4096) a->bytes_per_sample = (double)bytes / (double)samples;
}

static void raw_free(ac3_raw* a)
{
    fa_br_free(&a->br);
    free(a->data);
    free(a->index);
    free(a);
}

static fa_result raw_open(fa_decoder* dec)
{
    fa_source* src = dec->src;
    ac3_raw* a = (ac3_raw*)calloc(1, sizeof(*a));
    fa_track_params tp;
    fa_ac3_header h;
    int64_t end = FA_DEMUX_NO_PTS;
    fa_result r;
    if (!a) return FA_ERROR_OUT_OF_MEMORY;
    r = fa_br_init(&a->br, src);
    if (r != FA_OK) { free(a); return r; }
    a->may_seek = src->kind != FA_SOURCE_IO;
    /* The first unit sets the time base. */
    for (;;) {
        r = raw_sync(a, &h);
        if (r != FA_OK) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto fail; }
        if (fa_ac3_starts_unit(&h)) break;
        r = read_frame(a, &h, 0);   /* a dependent frame before the first unit */
        if (r != FA_OK) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto fail; }
    }
    a->data_start = fa_br_tell(&a->br) - (int64_t)a->h_n;
    a->rate = h.rate;
    a->eac3 = h.eac3;

    if (src->seekable) {
        if (src->kind != FA_SOURCE_IO && !src->remote) {
            if (!check_cbr(a, &h, &end)) r = build_index(a, &end);
        }
        else estimate_rate(a);
        if (r != FA_OK) goto fail;
        if (!a->index && !a->cbr && a->bytes_per_sample > 0 && src->length > a->data_start)
            a->est_samples = (int64_t)((double)(src->length - a->data_start) / a->bytes_per_sample);
        r = fa_br_seek(&a->br, a->data_start);
        if (r != FA_OK) goto fail;
        a->h_n = 0;
    }
    a->may_seek = src->kind != FA_SOURCE_IO;

    memset(&tp, 0, sizeof(tp));
    fa_codec_params_init(&tp.codec);
    tp.codec.codec = h.eac3 ? "eac3" : "ac3";
    tp.codec.sample_rate = h.rate;
    tp.codec.channels = (uint32_t)h.channels;
    tp.time_base = 1.0 / h.rate;
    tp.start = 0;
    tp.end = end != FA_DEMUX_NO_PTS ? end : a->est_samples > 0 ? a->est_samples : FA_DEMUX_NO_PTS;
    tp.clip_to_end = a->index != NULL || a->cbr;
    tp.exact_durations = 1;
    tp.last_pts = FA_DEMUX_NO_PTS;

    r = fa_track_open(&a->track, dec, &raw_ops, a, &tp);
    if (r != FA_OK) goto fail;
    dec->impl = a;
    dec->seekable = a->index != NULL || a->cbr || (src->seekable && a->bytes_per_sample > 0);
    snprintf(dec->container, sizeof(dec->container), "%s", h.eac3 ? "ec3" : "ac3");
    return FA_OK;
fail:
    raw_free(a);
    return r;
}

static fa_result raw_read(fa_decoder* dec, float* out, fa_frames frames, fa_frames* got)
{
    return fa_track_read(&((ac3_raw*)dec->impl)->track, out, frames, got);
}

static fa_result raw_seek(fa_decoder* dec, fa_frames frame)
{
    ac3_raw* a = (ac3_raw*)dec->impl;
    if (!dec->seekable) return FA_ERROR_NOT_SEEKABLE;
    return fa_track_seek(&a->track, frame);
}

static void raw_close(fa_decoder* dec)
{
    ac3_raw* a = (ac3_raw*)dec->impl;
    if (!a) return;
    fa_track_close(&a->track);
    raw_free(a);
    dec->impl = NULL;
}

const fa_decoder_vtable fa_decoder_ac3 = {
    "ac3", "Dolby Digital (AC-3) and Dolby Digital Plus (E-AC-3)", "ac3;ec3;eac3;a52;ddp",
    "audio/ac3;audio/eac3;audio/vnd.dolby.dd-raw;audio/x-ac3;audio/x-eac3",
    raw_probe, raw_open, raw_read, raw_seek, raw_close, 1, NULL, NULL
};
