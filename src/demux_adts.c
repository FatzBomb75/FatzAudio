/* AAC in ADTS frames (.aac files, AAC internet radio, AAC in HLS segments),
 * written from ISO/IEC 13818-7 (adts_fixed_header, adts_variable_header) and
 * 14496-3 (sampling frequency index, channel configuration).
 *
 * Each ADTS frame is one packet for the "aac" codec, header included: the codec
 * reads the configuration from it, so CRCs, several raw data blocks per frame and
 * in-band channel layouts need nothing here. Every frame holds 1024 samples per
 * raw data block at the header's rate (HE-AAC decodes to twice that rate; the
 * track decoder follows the codec's output).
 *
 * Local files are indexed once by hopping from header to header, which gives an
 * exact length and exact seeks. Network files seek by byte position, estimated
 * from the frame sizes at the start, like MP3 without a seek table. Live streams
 * and forward-only sources read straight through; junk between frames (a stream
 * joined mid-frame, a damaged frame, ID3 tags between HLS segments) is skipped by
 * sliding to the next valid header. */
#include "demux.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ADTS_HEADER 7
#define ADTS_RESYNC 65536            /* junk bytes skipped looking for a header */
#define ADTS_MAX_FRAMES (1 << 24)    /* index limit: 97 hours at 48 kHz */
#define ADTS_PRIMING 1024            /* samples dropped at the start */

typedef struct adts_hdr {
    size_t len;          /* whole frame, header included */
    uint32_t rate;
    uint32_t channels;   /* 0: the layout is inside the frame (a program config element) */
    uint32_t samples;    /* 1024 per raw data block */
    uint8_t config;      /* sampling frequency index and channel configuration */
} adts_hdr;

typedef struct adts_frame {
    int64_t off;
    int64_t pts;
} adts_frame;

typedef struct adts {
    fa_br br;
    fa_track track;
    uint8_t h[10];       /* bytes of the next header already read */
    size_t h_n;
    uint8_t* data;
    size_t data_cap;
    int64_t data_start;  /* the first frame */
    int64_t pts;         /* time of the next frame, in samples at base_rate */
    uint32_t base_rate;  /* the first frame's rate: the time base */
    uint8_t config;      /* of the last frame: a header found after junk must match it */
    int config_set;
    int in_flow;         /* the next byte is where the last frame ended */
    int may_seek;        /* look ahead by seeking: local sources, or after a network seek */
    adts_frame* index;   /* every frame (local files) */
    size_t nindex;
    double bytes_per_sample;  /* network files: seek estimate */
    int64_t est_samples;
} adts;

static const uint32_t adts_rates[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                         22050, 16000, 12000, 11025, 8000, 7350 };

/* A frame's duration in the time base (frames at another rate after a change). */
static int64_t frame_time(const adts* a, const adts_hdr* h)
{
    return h->rate == a->base_rate ? (int64_t)h->samples : (int64_t)h->samples * a->base_rate / h->rate;
}

/* Parses the header at p (7 bytes). 0 if it is not one. */
static int adts_parse(const uint8_t* p, adts_hdr* out)
{
    static const uint32_t layout[8] = { 0, 1, 2, 3, 4, 5, 6, 8 };
    uint32_t sfi, cfg, prot_absent;
    size_t len;
    if (p[0] != 0xFF || (p[1] & 0xF6) != 0xF0) return 0;   /* syncword, layer 00 */
    prot_absent = p[1] & 1;
    sfi = (p[2] >> 2) & 15;
    if (sfi >= 13) return 0;
    cfg = ((uint32_t)(p[2] & 1) << 2) | (p[3] >> 6);
    len = ((size_t)(p[3] & 3) << 11) | ((size_t)p[4] << 3) | (p[5] >> 5);
    if (len < (size_t)ADTS_HEADER + (prot_absent ? 0 : 2) + 1) return 0;
    out->len = len;
    out->rate = adts_rates[sfi];
    out->channels = layout[cfg];
    out->samples = 1024u * ((p[6] & 3) + 1u);
    out->config = (uint8_t)((sfi << 3) | cfg);
    return 1;
}

/* A run of `want` chained headers starting at d[i]? A header at the very end of
 * the window counts when it is at least the start of one. */
static int adts_chain(const uint8_t* d, size_t n, size_t i, int want)
{
    adts_hdr h, first;
    int k;
    for (k = 0; k < want; k++) {
        if (i + ADTS_HEADER > n) return k > 0;
        if (!adts_parse(d + i, &h)) return 0;
        if (k == 0) first = h;
        else if (h.config != first.config) return 0;
        i += h.len;
    }
    return 1;
}

static int adts_probe(const uint8_t* d, size_t n, size_t id3)
{
    size_t i;
    if (id3 >= n) return 0;
    for (i = id3; i + ADTS_HEADER <= n && i < id3 + 4096; i++) {
        adts_hdr h;
        if (!adts_parse(d + i, &h)) continue;
        if (i + h.len + ADTS_HEADER <= n) {
            if (!adts_chain(d, n, i, 3)) continue;   /* random 0xFFF bytes, keep looking */
            return i == id3 ? 95 : 70;
        }
        if (i == id3) return 50;     /* one frame fills the window */
    }
    return 0;
}

/* ---- reading frames ----------------------------------------------------------------- */

/* Tops up a->h to `n` bytes. */
static int fill_h(adts* a, size_t n)
{
    if (a->h_n < n) a->h_n += fa_br_read(&a->br, a->h + a->h_n, n - a->h_n);
    return a->h_n >= n;
}

static void skip_bytes(adts* a, int64_t n)
{
    uint8_t tmp[512];
    while (n > 0) {
        size_t k = n < (int64_t)sizeof(tmp) ? (size_t)n : sizeof(tmp);
        size_t got = fa_br_read(&a->br, tmp, k);
        if (got == 0) break;
        n -= (int64_t)got;
    }
}

/* Is the header in a->h (at stream position `at`) followed by another one of the
 * same configuration, or by the end of the data? 1 yes, 0 no, -1 can't tell
 * without waiting for the network. Looks in the source's first bytes when they
 * cover it, else seeks where that costs nothing. */
static int chained(adts* a, int64_t at, const adts_hdr* h)
{
    fa_source* src = a->br.src;
    const uint8_t* d;
    size_t n = fa_source_peek(src, &d);
    int64_t next = at + (int64_t)h->len;
    adts_hdr h2;
    if (src->length > 0 && next == src->length) return 1;
    if (next + ADTS_HEADER <= (int64_t)n)
        return adts_parse(d + next, &h2) && h2.config == h->config;
    if (a->may_seek) {
        uint8_t nh[ADTS_HEADER];
        size_t got;
        int ok;
        if (fa_br_seek(&a->br, next) != FA_OK) return 0;
        got = fa_br_read(&a->br, nh, ADTS_HEADER);
        ok = got == ADTS_HEADER && adts_parse(nh, &h2) && h2.config == h->config;
        if (fa_br_seek(&a->br, at) != FA_OK) return 0;
        a->h_n = 0;
        if (!fill_h(a, ADTS_HEADER)) return 0;
        return ok;
    }
    return -1;
}

/* Leaves the next frame's header in a->h, skipping ID3 tags. Right after a frame
 * any valid header is taken (the configuration may change between frames, at an
 * HLS discontinuity say); after junk, a seek or at the start a header must match
 * the configuration and be followed by another one, where that can be checked. */
static fa_result adts_sync(adts* a, adts_hdr* out)
{
    size_t skipped = 0;
    for (;;) {
        if (!fill_h(a, ADTS_HEADER)) return FA_ERROR_END_OF_STREAM;
        if (adts_parse(a->h, out)) {
            if (a->in_flow && skipped == 0) break;
            if (!a->config_set || out->config == a->config) {
                int64_t at = fa_br_tell(&a->br) - (int64_t)a->h_n;
                if (chained(a, at, out) != 0) break;
            }
        } else if (memcmp(a->h, "ID3", 3) == 0 && fill_h(a, 10)) {
            size_t tag = fa_probe_id3v2_size(a->h, 10);
            if (tag >= 10) {
                a->h_n = 0;
                skip_bytes(a, (int64_t)tag - 10);
                continue;
            }
        }
        if (++skipped > ADTS_RESYNC) return FA_ERROR_END_OF_STREAM;
        memmove(a->h, a->h + 1, a->h_n - 1);
        a->h_n--;
    }
    a->config = out->config;
    a->config_set = 1;
    a->in_flow = 1;
    return FA_OK;
}

static fa_result adts_next(void* ctx, fa_demux_packet* out)
{
    adts* a = (adts*)ctx;
    adts_hdr h;
    fa_result r = adts_sync(a, &h);
    size_t have;
    if (r != FA_OK) return r;
    if (h.len + 1 > a->data_cap) {   /* h.len < 8192: 13 bits */
        size_t c = a->data_cap ? a->data_cap : 4096;
        uint8_t* p;
        while (c < h.len + 1) c *= 2;
        p = (uint8_t*)realloc(a->data, c);
        if (!p) return FA_ERROR_OUT_OF_MEMORY;
        a->data = p;
        a->data_cap = c;
    }
    have = a->h_n < h.len ? a->h_n : h.len;
    memcpy(a->data, a->h, have);
    memmove(a->h, a->h + have, a->h_n - have);
    a->h_n -= have;
    if (fa_br_read(&a->br, a->data + have, h.len - have) != h.len - have) return FA_ERROR_END_OF_STREAM;
    out->data = a->data;
    out->size = h.len;
    out->pts = a->pts;
    out->duration = frame_time(a, &h);
    a->pts += out->duration;
    return FA_OK;
}

static fa_result adts_seek_packets(void* ctx, int64_t t, int packets_back, size_t bytes_back)
{
    adts* a = (adts*)ctx;
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
    } else {
        if (!a->br.src->seekable || a->bytes_per_sample <= 0) return FA_ERROR_NOT_SEEKABLE;
        pts = t - (int64_t)packets_back * 1024;
        if (pts < 0) pts = 0;
        off = a->data_start + (int64_t)((double)pts * a->bytes_per_sample);
        if (a->br.src->length > 0 && off >= a->br.src->length) off = a->br.src->length - 1;
    }
    r = fa_br_seek(&a->br, off);
    if (r != FA_OK) return r;
    a->h_n = 0;
    a->pts = pts;
    a->in_flow = a->index != NULL;   /* an estimated position is in the middle of a frame */
    a->may_seek = 1;
    return FA_OK;
}

static const fa_demux_ops adts_ops = { adts_next, adts_seek_packets };

/* A frame at `pos` of a local source: a matching header followed by another one
 * (or the end); 0 if not. */
static int frame_at(adts* a, int64_t pos, adts_hdr* h)
{
    if (fa_br_seek(&a->br, pos) != FA_OK) return 0;
    a->h_n = 0;
    if (!fill_h(a, ADTS_HEADER) || !adts_parse(a->h, h) || h->config != a->config) return 0;
    return chained(a, pos, h) == 1;
}

/* Local files: every frame's offset and time, by hopping from header to header.
 * ID3 tags and junk between frames are skipped like adts_sync does. Very long
 * files fall back to estimates (a->index stays NULL). */
static fa_result build_index(adts* a, int64_t* out_end)
{
    size_t cap = 0;
    int64_t pos = a->data_start, pts = 0, len = a->br.src->length;
    fa_result r = FA_OK;
    for (;;) {
        adts_hdr hd;
        if (fa_br_seek(&a->br, pos) != FA_OK) break;
        a->h_n = 0;
        if (!fill_h(a, ADTS_HEADER)) break;
        /* A header right after a frame may change the configuration if another follows it. */
        if (!adts_parse(a->h, &hd) || (hd.config != a->config && chained(a, pos, &hd) != 1)) {
            int64_t p, stop = pos + ADTS_RESYNC;
            if (memcmp(a->h, "ID3", 3) == 0 && fill_h(a, 10)) {
                size_t tag = fa_probe_id3v2_size(a->h, 10);
                if (tag >= 10) { pos += (int64_t)tag; continue; }
            }
            for (p = pos + 1; p < stop && (len <= 0 || p + ADTS_HEADER <= len); p++)
                if (frame_at(a, p, &hd)) break;
            if (p >= stop || (len > 0 && p + ADTS_HEADER > len)) break;   /* the end (ID3v1, APE tags) */
            pos = p;
        }
        if (len > 0 && pos + (int64_t)hd.len > len) break;   /* cut short */
        if (a->nindex == cap) {
            adts_frame* f;
            if (cap >= ADTS_MAX_FRAMES) {
                a->bytes_per_sample = (double)(pos - a->data_start) / (double)pts;
                free(a->index);
                a->index = NULL;
                a->nindex = 0;
                *out_end = FA_DEMUX_NO_PTS;
                return FA_OK;
            }
            cap = cap ? cap * 2 : 1024;
            f = (adts_frame*)realloc(a->index, cap * sizeof(*f));
            if (!f) { r = FA_ERROR_OUT_OF_MEMORY; break; }
            a->index = f;
        }
        a->index[a->nindex].off = pos;
        a->index[a->nindex].pts = pts;
        a->nindex++;
        a->config = hd.config;
        pos += (int64_t)hd.len;
        pts += frame_time(a, &hd);
    }
    *out_end = pts;
    if (r == FA_OK && a->nindex == 0) r = FA_ERROR_UNSUPPORTED_FORMAT;
    if (r != FA_OK) {
        free(a->index);
        a->index = NULL;
        a->nindex = 0;
    }
    return r;
}

/* Network files and the app's callbacks: average bytes per sample over the first
 * frames, for the length and seeks. They follow the first one in the download. */
static void estimate_rate(adts* a)
{
    int64_t pos = a->data_start, bytes = 0, samples = 0;
    int k;
    for (k = 0; k < 64 && bytes < 65536; k++) {
        uint8_t h[ADTS_HEADER];
        adts_hdr hd;
        if (fa_br_seek(&a->br, pos) != FA_OK || fa_br_read(&a->br, h, ADTS_HEADER) != ADTS_HEADER) break;
        if (!adts_parse(h, &hd) || hd.config != a->config) break;
        bytes += (int64_t)hd.len;
        samples += hd.samples;
        pos += (int64_t)hd.len;
    }
    if (samples >= 4096) a->bytes_per_sample = (double)bytes / (double)samples;
}

static void adts_free(adts* a)
{
    fa_br_free(&a->br);
    free(a->data);
    free(a->index);
    free(a);
}

static fa_result adts_open(fa_decoder* dec)
{
    fa_source* src = dec->src;
    adts* a = (adts*)calloc(1, sizeof(adts));
    fa_track_params tp;
    adts_hdr h;
    int64_t end = FA_DEMUX_NO_PTS;
    fa_result r;
    if (!a) return FA_ERROR_OUT_OF_MEMORY;
    r = fa_br_init(&a->br, src);
    if (r != FA_OK) { free(a); return r; }

    /* The first frame (after any tag or junk), whose header sets the configuration. */
    a->may_seek = src->kind != FA_SOURCE_IO;
    r = adts_sync(a, &h);
    if (r != FA_OK) { r = FA_ERROR_UNSUPPORTED_FORMAT; goto fail; }
    a->data_start = fa_br_tell(&a->br) - (int64_t)a->h_n;
    a->base_rate = h.rate;

    if (src->seekable) {
        /* Files and memory are indexed; the app's callbacks and the network estimate. */
        if (src->kind != FA_SOURCE_IO) r = build_index(a, &end);
        else estimate_rate(a);
        if (r != FA_OK) goto fail;
        if (a->bytes_per_sample > 0 && src->length > a->data_start)
            a->est_samples = (int64_t)((double)(src->length - a->data_start) / a->bytes_per_sample);
        r = fa_br_seek(&a->br, a->data_start);
        if (r != FA_OK) goto fail;
        a->h_n = 0;
        a->config = h.config;
    }
    a->may_seek = src->kind != FA_SOURCE_IO;

    memset(&tp, 0, sizeof(tp));
    fa_codec_params_init(&tp.codec);
    tp.codec.codec = "aac";
    tp.codec.sample_rate = h.rate;
    tp.codec.channels = h.channels ? h.channels : 2;
    tp.time_base = 1.0 / h.rate;
    /* ADTS has no field for the encoder delay. Every AAC encoder primes with at
     * least one frame (1024 samples), and a decoder's first frame is half an
     * overlap anyway, also when joining a radio stream: start after it. */
    tp.start = ADTS_PRIMING;
    tp.end = end != FA_DEMUX_NO_PTS ? end : a->est_samples > 0 ? a->est_samples : FA_DEMUX_NO_PTS;
    tp.clip_to_end = a->index != NULL;
    tp.exact_durations = 0;
    tp.last_pts = FA_DEMUX_NO_PTS;

    r = fa_track_open(&a->track, dec, &adts_ops, a, &tp);
    if (r != FA_OK) goto fail;
    dec->impl = a;
    dec->seekable = a->index != NULL || (src->seekable && a->bytes_per_sample > 0);
    dec->lead_trim = (fa_frames)llround(ADTS_PRIMING * a->track.fpt);
    snprintf(dec->container, sizeof(dec->container), "adts");
    return FA_OK;
fail:
    adts_free(a);
    return r;
}

static fa_result adts_read(fa_decoder* dec, float* out, fa_frames frames, fa_frames* got)
{
    return fa_track_read(&((adts*)dec->impl)->track, out, frames, got);
}

static fa_result adts_seek(fa_decoder* dec, fa_frames frame)
{
    adts* a = (adts*)dec->impl;
    if (!dec->seekable) return FA_ERROR_NOT_SEEKABLE;
    return fa_track_seek(&a->track, frame);
}

static void adts_close(fa_decoder* dec)
{
    adts* a = (adts*)dec->impl;
    if (!a) return;
    fa_track_close(&a->track);
    adts_free(a);
    dec->impl = NULL;
}

static void adts_lag(fa_decoder* dec, int64_t* out_bytes, int64_t* out_frames)
{
    adts* a = (adts*)dec->impl;
    *out_bytes = (int64_t)(a->br.len - a->br.off) + (int64_t)a->h_n;
    *out_frames = a->track.count - a->track.off;
}

const fa_decoder_vtable fa_decoder_adts = {
    "aac", "AAC in ADTS frames (.aac, AAC radio, AAC in HLS)", "aac;adts",
    "audio/aac;audio/aacp;audio/x-aac;audio/vnd.dlna.adts;audio/x-hx-aac-adts",
    adts_probe, adts_open, adts_read, adts_seek, adts_close, 1, adts_lag, NULL
};
