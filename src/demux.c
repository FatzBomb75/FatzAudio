/* Shared parts of the container demuxers: the buffered reader and the track
 * decoder (see demux.h).
 *
 * Timeline of a track: frame 0 is the packet time `start` (an edit list's media
 * time, Matroska's CodecDelay), after `lead` frames of silence (an empty edit).
 * Each packet's audio is laid at its container timestamp only where the timeline
 * starts (open, after a seek); from there on packets follow each other, so a
 * sequential decode never gains or loses a frame to timestamp rounding. Frames
 * before 0, before a seek target or after an exact end are decoded and dropped. */
#include "demux.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- buffered reader ----------------------------------------------------------- */

/* Read-ahead: up to 256 KB from the app's callbacks (which may be the network),
 * 16 KB from files, memory and web streams, where reads are cheap: the OS caches
 * files, and a web stream reads from its download window, which already is the
 * read-ahead. The buffer grows with the read-ahead, so a stream that only reads
 * headers, or one jumping around, stays small. */
#define BR_CAP_SLOW (256u * 1024u)
#define BR_CAP_FAST (16u * 1024u)
#define BR_MIN 512u     /* after a jump: enough for the next headers */
#define BR_HOP 8192     /* a forward hop up to this far keeps the read-ahead */

fa_result fa_br_init(fa_br* b, fa_source* src)
{
    memset(b, 0, sizeof(*b));
    b->buf = (uint8_t*)malloc(BR_MIN);
    if (!b->buf) return FA_ERROR_OUT_OF_MEMORY;
    b->src = src;
    b->cap = BR_MIN;
    b->max = src->kind == FA_SOURCE_IO && !src->remote ? BR_CAP_SLOW : BR_CAP_FAST;
    b->chunk = BR_MIN;
    b->base = fa_source_tell(src);
    return FA_OK;
}

void fa_br_init_block(fa_br* b, fa_source* src, uint8_t* data, size_t n, int64_t base)
{
    memset(b, 0, sizeof(*b));
    b->src = src;
    b->buf = data;
    b->cap = b->len = b->max = b->chunk = n;
    b->base = base;
    b->fixed = 1;
}

void fa_br_free(fa_br* b)
{
    free(b->buf);
    memset(b, 0, sizeof(*b));
}

int64_t fa_br_tell(const fa_br* b) { return b->base + (int64_t)b->off; }

fa_result fa_br_seek(fa_br* b, int64_t pos)
{
    fa_result r;
    if (pos < 0) return FA_ERROR_INVALID_ARG;
    if (pos >= b->base && pos <= b->base + (int64_t)b->len) {
        b->off = (size_t)(pos - b->base);
        return FA_OK;
    }
    if (b->fixed) return FA_ERROR_IO;
    if (!b->src->seekable && pos < b->base + (int64_t)b->len) return FA_ERROR_NOT_SEEKABLE;
    r = fa_source_seek(b->src, pos);
    if (r != FA_OK) return r;
    /* A hop of a frame or so forward (from header to header) keeps reading
     * ahead; a jump starts small again. */
    if (pos < b->base + (int64_t)b->len || pos - (b->base + (int64_t)b->len) > (int64_t)BR_HOP) b->chunk = BR_MIN;
    b->base = pos;
    b->len = b->off = 0;
    return FA_OK;
}

size_t fa_br_read(fa_br* b, void* out, size_t n)
{
    uint8_t* o = (uint8_t*)out;
    size_t done = 0;
    while (done < n) {
        size_t avail = b->len - b->off;
        int64_t got;
        if (avail > 0) {
            size_t k = avail < n - done ? avail : n - done;
            memcpy(o + done, b->buf + b->off, k);
            b->off += k;
            done += k;
            continue;
        }
        if (b->fixed) break;
        b->base += (int64_t)b->len;
        b->len = b->off = 0;
        if (n - done >= b->chunk) {
            /* A big read (a packet) goes straight to the caller. */
            got = fa_source_read(b->src, o + done, n - done);
            if (got <= 0) break;
            b->base += got;
            done += (size_t)got;
            continue;
        }
        if (b->chunk > b->cap) {
            /* Short of memory, it reads ahead less. */
            uint8_t* p = (uint8_t*)realloc(b->buf, b->chunk);
            if (p) { b->buf = p; b->cap = b->chunk; }
            else b->chunk = b->max = b->cap;
        }
        got = fa_source_read(b->src, b->buf, b->chunk);
        if (got <= 0) break;
        b->len = (size_t)got;
        if (b->chunk < b->max) b->chunk *= 2;  /* reading on in sequence: read further ahead */
    }
    return done;
}

fa_result fa_br_read_alloc(fa_br* b, size_t n, size_t max, uint8_t** buf, size_t* cap)
{
    if (n > max) return FA_ERROR_UNSUPPORTED_FORMAT;
    if (n + 1 > *cap) {
        size_t c = *cap ? *cap : 4096;
        uint8_t* p;
        while (c < n + 1) c *= 2;
        p = (uint8_t*)realloc(*buf, c);
        if (!p) return FA_ERROR_OUT_OF_MEMORY;
        *buf = p;
        *cap = c;
    }
    return fa_br_read(b, *buf, n) == n ? FA_OK : FA_ERROR_END_OF_STREAM;
}

/* ---- codec knowledge -------------------------------------------------------------- */

#define OPUS_PREROLL_S 0.3     /* decoded before an Opus seek target, at least */

typedef struct codec_info {
    const char* name;
    int packets_back;   /* packets decoded before a seek target */
    size_t bytes_back;
    int fixed;          /* every packet has the same duration (in practice) */
} codec_info;

static const codec_info codecs[] = {
    /* Transform codecs overlap each packet with the previous one. */
    { "ac3", 1, 0, 1 }, { "eac3", 1, 0, 1 }, { "dts", 1, 0, 1 },
    { "mp2", 1, 0, 1 }, { "mp1", 1, 0, 1 }, { "vorbis", 1, 0, 0 },
    /* An MP3 frame's data may start up to 511 bytes back, in earlier frames. */
    { "mp3", 1, 1024, 1 },
    /* AAC: one packet settles the MDCT overlap, but SBR and PS carry envelopes,
     * parameters and smoothing from frame to frame (and Main and LTP their
     * predictors). After 12 packets the SBR and PS test streams come out as a
     * straight decode's, except for the noise generators (PNS, SBR noise and
     * sine index), which run through the whole stream. */
    { "aac", 12, 0, 1 },
    /* Opus converges within its time pre-roll (seek_preroll_s, OPUS_PREROLL_S). */
    { "opus", 1, 0, 1 },
    /* TrueHD decodes from a major sync, at least every 128 access units. */
    { "truehd", 130, 0, 0 }, { "mlp", 130, 0, 0 },
    /* Lossless frames stand alone, but a container's rounded timestamp (Matroska's
     * milliseconds) can put the frame holding the target one packet later. */
    { "flac", 1, 0, 1 }, { "alac", 1, 0, 1 },
    /* WMA 1 and 2: every block decodes to the same count; an ASF media object holds
     * the same number of blocks. WMA Pro and Lossless frames may start in the packet
     * before (a bit reservoir across packets). */
    { "wmav1", 2, 0, 1 }, { "wmav2", 2, 0, 1 }, { "wmapro", 2, 0, 0 }, { "wmalossless", 2, 0, 0 },
};

static const codec_info* find_codec(const char* name)
{
    size_t i;
    for (i = 0; i < sizeof(codecs) / sizeof(codecs[0]); i++)
        if (strcmp(codecs[i].name, name) == 0) return &codecs[i];
    return NULL;
}

/* Bytes per sample of a "pcm_*" codec name, 0 if not PCM. */
static uint32_t pcm_bytes(const char* name)
{
    uint32_t bits = 0;
    const char* p;
    if (strncmp(name, "pcm_", 4) != 0) return 0;
    for (p = name + 5; *p >= '0' && *p <= '9'; p++) bits = bits * 10 + (uint32_t)(*p - '0');
    return bits / 8;
}

/* Samples in an Opus packet (RFC 6716, 3.1), at 48 kHz. */
static int64_t opus_samples(const uint8_t* d, size_t n)
{
    static const int silk[4] = { 480, 960, 1920, 2880 };
    static const int hybrid[2] = { 480, 960 };
    static const int celt[4] = { 120, 240, 480, 960 };
    int cfg, per, count;
    if (n < 1) return -1;
    cfg = d[0] >> 3;
    per = cfg < 12 ? silk[cfg & 3] : cfg < 16 ? hybrid[cfg & 1] : celt[cfg & 3];
    switch (d[0] & 3) {
    case 0: count = 1; break;
    case 1: case 2: count = 2; break;
    default:
        if (n < 2) return -1;
        count = d[1] & 63;
        break;
    }
    return (int64_t)per * count;
}

/* Samples in an MPEG audio frame, from its header. */
static int64_t mpa_samples(const uint8_t* d, size_t n, uint32_t* rate)
{
    static const uint32_t rates[3] = { 44100, 48000, 32000 };
    uint32_t version, layer, sr;
    if (n < 4 || d[0] != 0xFF || (d[1] & 0xE0) != 0xE0) return -1;
    version = (d[1] >> 3) & 3;
    layer = (d[1] >> 1) & 3;
    sr = (d[2] >> 2) & 3;
    if (version == 1 || layer == 0 || sr == 3) return -1;
    *rate = rates[sr] >> (version == 3 ? 0 : version == 2 ? 1 : 2);
    if (layer == 3) return 384;                    /* Layer I */
    if (layer == 2) return 1152;                   /* Layer II */
    return version == 3 ? 1152 : 576;              /* Layer III */
}

/* The block size in a FLAC frame header; -1 = not a frame. */
static int64_t flac_samples(const uint8_t* d, size_t n)
{
    unsigned code, ones = 0;
    size_t at;
    if (n < 6 || d[0] != 0xFF || (d[1] & 0xFE) != 0xF8) return -1;
    code = d[2] >> 4;
    if (code == 0) return -1;
    if (code == 1) return 192;
    if (code <= 5) return 576 << (code - 2);
    if (code >= 8) return 256 << (code - 8);
    /* 6 and 7: the size follows the UTF-8 style coded frame or sample number. */
    while (ones < 8 && (d[4] & (0x80u >> ones))) ones++;
    if (ones == 1 || ones > 7) return -1;
    at = 4 + (ones ? ones : 1);
    if (code == 6) return at < n ? (int64_t)d[at] + 1 : -1;
    return at + 1 < n ? (((int64_t)d[at] << 8) | d[at + 1]) + 1 : -1;
}

int64_t fa_demux_packet_samples(const char* codec, uint32_t channels, uint32_t rate,
                                const uint8_t* d, size_t n, uint32_t* out_rate)
{
    uint32_t pb = pcm_bytes(codec);
    *out_rate = 0;
    if (pb > 0 && channels > 0) {
        *out_rate = rate;
        return (int64_t)(n / ((size_t)pb * channels));
    }
    if (strcmp(codec, "opus") == 0) {
        *out_rate = 48000;
        return opus_samples(d, n);
    }
    if (strcmp(codec, "mp3") == 0 || strcmp(codec, "mp2") == 0 || strcmp(codec, "mp1") == 0)
        return mpa_samples(d, n, out_rate);
    if (strcmp(codec, "ac3") == 0 || strcmp(codec, "eac3") == 0) return fa_ac3_packet_samples(d, n, out_rate);
    if (strcmp(codec, "flac") == 0 && rate > 0) {
        *out_rate = rate;
        return flac_samples(d, n);
    }
    /* ALAC: only a partial frame (a file's last) says its size; a whole one holds
     * the frame length of the codec's configuration. The first element's header:
     * type (3 bits), tag (4), unused (12), partial (1), shift (2), verbatim (1),
     * then the 32-bit count. */
    if (strcmp(codec, "alac") == 0 && rate > 0 && n >= 7) {
        uint32_t type = d[0] >> 5;
        if ((type == 0 || type == 1 || type == 3) && (d[2] & 0x10)) {
            uint64_t v = ((uint64_t)d[2] << 32) | ((uint64_t)d[3] << 24) | ((uint64_t)d[4] << 16) |
                         ((uint64_t)d[5] << 8) | d[6];
            *out_rate = rate;
            return (int64_t)((v >> 1) & 0xFFFFFFFFu);
        }
    }
    return -1;
}

/* Duration of a packet from its own bytes, in output frames; -1 if unknown. */
static fa_frames packet_frames(const fa_track* t, const uint8_t* d, size_t n)
{
    uint32_t out_rate = t->dec->format.sample_rate, rate = 0;
    int64_t f = fa_demux_packet_samples(t->p.codec.codec, t->p.codec.channels, t->p.codec.sample_rate, d, n, &rate);
    if (f < 0 || rate == 0) return -1;
    return rate == out_rate ? f : (fa_frames)llround((double)f * out_rate / rate);
}

/* ---- the track decoder -------------------------------------------------------------- */

/* The codec's output in a format other than the one reported at open (a stream
 * that changes mid-way): converted into t->tmp. The resampler holds back its
 * look-ahead; that comes out with the next block, or here when the packet ends
 * the audio (`last`: its trim must cut the true end). */
static fa_result convert(fa_track* t, const float** data, fa_frames* frames, int last)
{
    uint32_t ch = t->dec->format.channels;
    fa_result r = fa_pcm_convert(&t->conv, *data, *frames, t->codec.rate, t->codec.channels,
                                 t->dec->format.sample_rate, ch, &t->tmp, &t->tmp_cap, frames);
    if (r == FA_OK && last) r = fa_pcm_conv_flush(&t->conv, ch, &t->tmp, &t->tmp_cap, frames);
    *data = t->tmp;
    return r;
}

/* Snaps an estimated packet position to the codec's packet grid, when the
 * container's timestamp could be that far off. */
static fa_frames snap(const fa_track* t, fa_frames est)
{
    double prec = t->p.precision * t->fpt + 1.0;
    if (t->grid_state >= 1 && t->p.precision > 0 && (double)t->grid > 2.0 * prec) {
        double k = floor((double)(est - t->grid_origin) / (double)t->grid + 0.5);
        fa_frames s = t->grid_origin + (fa_frames)k * t->grid;
        if (fabs((double)(s - est)) <= prec) return s;
    }
    return est;
}

/* The first audio fixes the decoder's format and with it the frame timeline. */
static void learn_format(fa_track* t)
{
    fa_decoder* dec = t->dec;
    dec->format.sample_rate = t->codec.rate;
    dec->format.channels = t->codec.channels;
    t->fpt = (double)dec->format.sample_rate * t->p.time_base;
    t->lead = (fa_frames)llround(t->p.lead_s * dec->format.sample_rate);
    t->length = -1;
    if (t->p.end != FA_DEMUX_NO_PTS) {
        t->length = (fa_frames)llround((double)(t->p.end - t->p.start) * t->fpt) + t->lead;
        if (t->length < 0) t->length = 0;
    }
    t->format_known = 1;
}

/* The most silence one gap may add, in samples (256 MB): 30 s of 32 channels at 48 kHz
 * or of 8 channels at 192 kHz fits. */
#define FA_GAP_MAX_SAMPLES ((fa_frames)1 << 26)

/* The codec gave audio this track can place. Rates over 768 kHz are corrupt
 * headers (the gap silence in emit() scales with the rate). */
static int codec_has_audio(const fa_track* t)
{
    return t->codec.out_frames > 0 && t->codec.rate != 0 && t->codec.rate <= 768000 && t->codec.channels != 0;
}

/* Lays the codec's output for packet `pk` on the timeline and keeps the part
 * that is handed out. */
static fa_result emit(fa_track* t, const fa_demux_packet* pk)
{
    fa_decoder* dec = t->dec;
    uint32_t ch = dec->format.channels;
    const float* data = t->codec.out;
    fa_frames m = t->codec.out_frames, d, dc, pos, lo, hi, zlo = 0, zhi = 0, held = 0;
    int placing = t->place;
    fa_result r;

    if (t->codec.rate != dec->format.sample_rate || t->codec.channels != ch || t->conv.pending) {
        r = convert(t, &data, &m, pk->trim_end > 0 && !t->trails);   /* a trailing decoder still holds audio */
        if (r != FA_OK) return r;
        held = fa_pcm_conv_held(&t->conv);   /* this packet's, still in the resampler */
    }
    /* The packet's duration: the codec's own count where it has one (exact), else
     * the container's if that is exact. */
    dc = pk->duration >= 0 ? (fa_frames)llround((double)pk->duration * t->fpt) : -1;
    d = packet_frames(t, pk->data, pk->size);
    if (d < 0 && t->p.precision == 0) d = dc;
    /* A system decoder's output may be an earlier packet's: the count is its own. */
    if (t->codec.trails) d = dc = -1;

    if (placing) {
        fa_frames gap_from = t->next_pos;
        if (pk->pts != FA_DEMUX_NO_PTS) {
            pos = (fa_frames)llround((double)(pk->pts - t->p.start) * t->fpt) + t->lead;
            if (t->grid_state == 0) {
                const codec_info* ci = find_codec(t->p.codec.codec);
                int fixed = (ci && ci->fixed) || pcm_bytes(t->p.codec.codec) > 0;
                t->grid = d > 0 ? d : m;
                t->grid_origin = pos;
                t->grid_state = (fixed && t->p.precision > 0 && t->grid > 0) ? 1 : -1;
            } else {
                pos = snap(t, pos);
            }
            /* Fewer frames than the packet holds: the codec dropped its start
             * (Opus pre-skip applied by the decoder itself). */
            if (d > m + held) pos += d - m - held;
        } else {
            pos = t->next_pos;
        }
        /* Audio starts later than asked (an empty edit, a decoder that waits for a
         * sync frame): silence keeps the positions right. */
        if (pos > gap_from && pos - gap_from <= (fa_frames)dec->format.sample_rate * 30 &&
            (pos - gap_from) * (fa_frames)ch <= FA_GAP_MAX_SAMPLES) {
            zlo = gap_from > t->drop_before ? gap_from : t->drop_before;
            zhi = pos;
            if (t->p.clip_to_end && t->length >= 0 && zhi > t->length) zhi = t->length;
            if (zhi < zlo) zhi = zlo;
        }
        t->place = 0;
    } else {
        pos = t->next_pos;
        if (t->grid_state >= 1) {
            fa_frames size = d > 0 ? d : m;
            if (t->grid_state == 2) t->grid_state = -1;       /* the odd one was not the last */
            else if (size != t->grid) t->grid_state = 2;
        }
    }
    if (t->p.exact_durations && dc >= 0 && m > dc) m = dc;
    if (pk->trim_end > 0) {
        fa_frames k = (fa_frames)llround((double)pk->trim_end * t->fpt);
        m = k >= m ? 0 : m - k;
    }
    t->next_pos = pos + m;

    lo = pos > t->drop_before ? pos : t->drop_before;
    hi = pos + m;
    if (t->p.clip_to_end && t->length >= 0 && hi > t->length) hi = t->length;
    if (hi < lo) hi = lo;
    if (zhi > zlo) {
        r = fa_float_reserve(&t->buf, &t->buf_cap, (zhi - zlo) + (hi - lo), ch);
        if (r != FA_OK) return r;
        memset(t->buf, 0, (size_t)(zhi - zlo) * ch * sizeof(float));
        if (hi > lo)
            memcpy(t->buf + (size_t)(zhi - zlo) * ch, data + (size_t)(lo - pos) * ch, (size_t)(hi - lo) * ch * sizeof(float));
        t->view = t->buf;
    } else {
        /* Handed out from the codec's own buffer, which stays until the next packet. */
        t->view = hi > lo ? data + (size_t)(lo - pos) * ch : NULL;
    }
    t->count = (zhi - zlo) + (hi - lo);
    t->off = 0;
    return FA_OK;
}

/* The frames the resampler still holds at the end of the track, after the last ones. */
static fa_result emit_tail(fa_track* t)
{
    uint32_t ch = t->dec->format.channels;
    fa_frames n = 0, pos = t->next_pos, lo, hi;
    fa_result r = fa_pcm_conv_flush(&t->conv, ch, &t->tmp, &t->tmp_cap, &n);
    t->count = t->off = 0;
    if (r != FA_OK || n == 0) return r;
    t->next_pos = pos + n;
    lo = pos > t->drop_before ? pos : t->drop_before;
    hi = pos + n;
    if (t->p.clip_to_end && t->length >= 0 && hi > t->length) hi = t->length;
    if (hi < lo) hi = lo;
    t->view = hi > lo ? t->tmp + (size_t)(lo - pos) * ch : NULL;
    t->count = hi - lo;
    return FA_OK;
}

/* The codec's settings, from its parameters (open, and a chained stream's next song). */
static void codec_settings(fa_track* t, const fa_codec_params* p)
{
    const codec_info* ci = find_codec(p->codec);
    t->p.codec = *p;
    /* The codec name is a static string; the extradata was only needed to open it. */
    t->p.codec.extradata = NULL;
    t->p.codec.extradata_size = 0;
    t->packets_back = ci ? ci->packets_back : pcm_bytes(p->codec) > 0 ? 0 : 1;
    t->bytes_back = ci ? ci->bytes_back : 0;
    /* Opus asks for 80 ms (RFC 7845 4.6), after which the audio can still be only
     * 30 dB from a straight decode; after 300 ms it is within about 80 dB. This
     * overrides a container's shorter SeekPreRoll (WebM writes 80 ms). */
    if (strcmp(p->codec, "opus") == 0 && t->p.seek_preroll_s < OPUS_PREROLL_S) t->p.seek_preroll_s = OPUS_PREROLL_S;
    /* The systems' decoders can give their first audio a packet or more late
     * (MediaCodec decodes asynchronously). */
    t->trails = t->codec.trails;
}

/* A chained stream's next song: its own codec, set up from its own headers. */
static fa_result reopen_codec(fa_track* t, const fa_codec_params* p)
{
    fa_result r;
    if (t->codec_ready) fa_codec_close(&t->codec);
    t->codec_ready = 0;
    r = fa_codec_open(&t->codec, p);
    if (r != FA_OK) return r;
    t->codec_ready = 1;
    t->codec.dynamic_range = t->dec->dynamic_range;
    codec_settings(t, p);
    return FA_OK;
}

/* Decodes packets until there are frames to hand out or the track ends. */
static fa_result fill(fa_track* t)
{
    int silent = 0;
    while (t->off >= t->count && !t->eof) {
        fa_demux_packet pk;
        fa_result r;
        memset(&pk, 0, sizeof(pk));
        pk.pts = FA_DEMUX_NO_PTS;
        pk.duration = -1;
        r = t->draining ? FA_ERROR_END_OF_STREAM : t->ops->next(t->ctx, &pk);
        if (r == FA_ERROR_END_OF_STREAM) {
            /* The codec gives back what it still holds (decoders with delayed
             * output), one block per call until it has nothing left. */
            t->draining = 1;
            if (t->codec_ready && t->codec.vt->decode(&t->codec, NULL, 0) == FA_OK && codec_has_audio(t)) {
                /* A trailing decoder may give a short file's first audio only now. */
                if (!t->format_known) learn_format(t);
                if (t->quiet && t->place) pk.pts = t->quiet_pts;
                t->quiet = 0;
                r = emit(t, &pk);
                if (r != FA_OK) return r;
                continue;
            }
            /* Then the resampler's last frames. */
            if (t->conv.pending && t->format_known && !t->place) {
                r = emit_tail(t);
                if (r != FA_OK) return r;
                if (t->count > 0) continue;
            }
            /* The odd-sized packet was the last one: the grid holds. */
            if (t->grid_state == 2) t->grid_state = 1;
            t->eof = 1;
            break;
        }
        if (r != FA_OK) return r;
        if (pk.reopen) {
            r = reopen_codec(t, pk.reopen);
            if (r != FA_OK) { t->eof = 1; return r; }    /* the next song cannot be decoded */
        }
        if (!t->codec_ready) return FA_ERROR_UNSUPPORTED_FORMAT;
        r = t->codec.vt->decode(&t->codec, pk.data, pk.size);
        if (r == FA_ERROR_OUT_OF_MEMORY) return r;
        if (r != FA_OK || !codec_has_audio(t)) {
            if (r == FA_OK && t->place && t->trails && !t->quiet && pk.pts != FA_DEMUX_NO_PTS) {
                t->quiet = 1;
                t->quiet_pts = pk.pts;
            }
            /* A corrupt packet is skipped. A codec that never makes a sound from
             * this track's first packets cannot decode it. */
            if (!t->format_known && ++silent > 256) return FA_ERROR_UNSUPPORTED_FORMAT;
            continue;
        }
        if (!t->format_known) learn_format(t);
        /* A decoder whose output trails its input: this audio starts where the
         * first packet fed since the flush does. */
        if (t->quiet && t->place) pk.pts = t->quiet_pts;
        t->quiet = 0;
        r = emit(t, &pk);
        if (r != FA_OK) return r;
    }
    return FA_OK;
}

fa_result fa_track_open(fa_track* t, fa_decoder* dec, const fa_demux_ops* ops, void* ctx, const fa_track_params* p)
{
    fa_result r;
    memset(t, 0, sizeof(*t));
    t->dec = dec;
    t->ops = ops;
    t->ctx = ctx;
    t->p = *p;
    t->length = -1;
    r = fa_codec_open(&t->codec, &p->codec);
    if (r != FA_OK) return r;
    t->codec_ready = 1;
    t->codec.dynamic_range = dec->dynamic_range;
    codec_settings(t, &p->codec);
    t->place = 1;

    r = fill(t);
    if (r == FA_OK && !t->format_known) {
        /* No audio at all (an empty track): what the container says. */
        if (p->codec.sample_rate == 0 || p->codec.channels == 0) r = FA_ERROR_UNSUPPORTED_FORMAT;
        else {
            t->codec.rate = p->codec.sample_rate;
            t->codec.channels = p->codec.channels;
            learn_format(t);
            t->length = 0;
        }
    }
    if (r != FA_OK) {
        fa_track_close(t);
        return r;
    }
    if (p->last_pts != FA_DEMUX_NO_PTS && p->last_end >= p->last_pts && t->format_known) {
        fa_frames at = (fa_frames)llround((double)(p->last_pts - p->start) * t->fpt) + t->lead;
        fa_frames last = (fa_frames)llround((double)(p->last_end - p->last_pts) * t->fpt);
        if (!p->last_exact && t->grid_state >= 1 && t->p.precision > 0) {
            last = t->grid - (fa_frames)llround((double)p->last_trim * t->fpt);
            if (last < 0) last = 0;
        }
        t->length = snap(t, at) + last;
        if (t->length < 0) t->length = 0;
    }
    snprintf(dec->codec, sizeof(dec->codec), "%s", p->codec.codec);
    dec->length = t->length;
    if (p->codec.bit_rate > 0 && p->codec.bit_rate < 0xFFFFFFFF) dec->bitrate = (uint32_t)p->codec.bit_rate;
    /* Bit depth only means something for PCM and lossless codecs. */
    if (strncmp(dec->codec, "pcm", 3) == 0 || strcmp(dec->codec, "flac") == 0 || strcmp(dec->codec, "alac") == 0 ||
        strcmp(dec->codec, "wmalossless") == 0)
        dec->bits_per_sample = p->codec.bits_per_coded_sample;
    return FA_OK;
}

fa_result fa_track_read(fa_track* t, float* out, fa_frames frames, fa_frames* got)
{
    uint32_t ch = t->dec->format.channels;
    fa_frames produced = 0;
    while (produced < frames) {
        if (t->off < t->count) {
            fa_frames n = t->count - t->off;
            if (n > frames - produced) n = frames - produced;
            memcpy(out + (size_t)produced * ch, t->view + (size_t)t->off * ch, (size_t)n * ch * sizeof(float));
            t->off += n;
            produced += n;
            continue;
        }
        if (t->eof) break;
        {
            fa_result r = fill(t);
            if (r != FA_OK) {
                if (produced > 0) break;      /* the error comes back on the next read */
                *got = 0;
                return r;
            }
        }
    }
    *got = produced;
    return produced > 0 ? FA_OK : FA_ERROR_END_OF_STREAM;
}

fa_result fa_track_seek(fa_track* t, fa_frames frame)
{
    fa_frames want = frame - (fa_frames)llround(t->p.seek_preroll_s * t->dec->format.sample_rate);
    double at = (double)t->p.start + (double)(want - t->lead) / t->fpt;
    fa_result r;
    /* Without a codec (a chained song that could not be opened) the demuxer
     * reopens the right one for the target. */
    if (t->codec_ready && t->codec.vt->flush) t->codec.vt->flush(&t->codec);
    fa_pcm_conv_reset(&t->conv);
    t->count = t->off = 0;
    t->eof = t->draining = 0;
    t->quiet = 0;
    /* The packet holding `want`; a hair of slack keeps exact boundaries exact. */
    r = t->ops->seek(t->ctx, (int64_t)floor(at + 1e-6), t->packets_back, t->bytes_back);
    t->place = 1;
    t->next_pos = frame;
    t->drop_before = frame;
    return r;
}

void fa_track_close(fa_track* t)
{
    if (t->codec_ready) fa_codec_close(&t->codec);
    fa_pcm_conv_free(&t->conv);
    free(t->buf);
    free(t->tmp);
    memset(t, 0, sizeof(*t));
}

/* ---- AAC ---------------------------------------------------------------------------- */

/* fa_aac_rates is in aac.c. */

typedef struct bitw { uint8_t* p; size_t bit; } bitw;

static void put_bits(bitw* w, uint32_t v, int n)
{
    while (n-- > 0) {
        if ((v >> n) & 1) w->p[w->bit >> 3] |= (uint8_t)(0x80 >> (w->bit & 7));
        w->bit++;
    }
}

static void put_rate(bitw* w, uint32_t rate)
{
    int i;
    for (i = 0; i < 13; i++)
        if (fa_aac_rates[i] == rate) { put_bits(w, (uint32_t)i, 4); return; }
    put_bits(w, 15, 4);
    put_bits(w, rate, 24);
}

/* ISO/IEC 14496-3 1.6.2.1 AudioSpecificConfig with a GASpecificConfig. */
size_t fa_aac_build_asc(uint8_t out[FA_AAC_ASC_MAX], int object_type, uint32_t rate, uint32_t channels, uint32_t sbr_rate)
{
    bitw w;
    /* channelConfiguration: 1-6 as is, 7 means 7.1; any other count is 0, "the
     * stream's own program config element says", never a guessed layout. */
    uint32_t cfg = channels <= 6 ? channels : channels == 8 ? 7 : 0;
    memset(out, 0, FA_AAC_ASC_MAX);
    w.p = out;
    w.bit = 0;
    if (sbr_rate > rate) {
        put_bits(&w, 5, 5);               /* explicit SBR: the core's config follows */
        put_rate(&w, rate);
        put_bits(&w, cfg, 4);
        put_rate(&w, sbr_rate);
        put_bits(&w, (uint32_t)object_type, 5);
    } else {
        put_bits(&w, (uint32_t)object_type, 5);
        put_rate(&w, rate);
        put_bits(&w, cfg, 4);
    }
    put_bits(&w, 0, 3);                   /* 1024-sample frames, no core coder, no extension */
    return (w.bit + 7) / 8;
}
