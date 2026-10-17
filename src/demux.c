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

#define BR_CAP (256u * 1024u)
#define BR_MIN 512u     /* after a jump: enough for the next headers */

fa_result fa_br_init(fa_br* b, fa_source* src)
{
    memset(b, 0, sizeof(*b));
    b->buf = (uint8_t*)malloc(BR_CAP);
    if (!b->buf) return FA_ERROR_OUT_OF_MEMORY;
    b->src = src;
    b->cap = BR_CAP;
    b->chunk = BR_MIN;
    b->base = fa_source_tell(src);
    return FA_OK;
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
    if (!b->src->seekable && pos < b->base + (int64_t)b->len) return FA_ERROR_NOT_SEEKABLE;
    r = fa_source_seek(b->src, pos);
    if (r != FA_OK) return r;
    b->base = pos;
    b->len = b->off = 0;
    b->chunk = BR_MIN;
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
        got = fa_source_read(b->src, b->buf, b->chunk);
        if (got <= 0) break;
        b->len = (size_t)got;
        if (b->chunk < b->cap) b->chunk *= 2;  /* reading on in sequence: read further ahead */
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

typedef struct codec_info {
    const char* name;
    int packets_back;   /* packets decoded before a seek target */
    size_t bytes_back;
    int fixed;          /* every packet has the same duration (in practice) */
} codec_info;

static const codec_info codecs[] = {
    /* Transform codecs overlap each packet with the previous one. */
    { "aac", 1, 0, 1 }, { "ac3", 1, 0, 1 }, { "eac3", 1, 0, 1 }, { "dts", 1, 0, 1 },
    { "mp2", 1, 0, 1 }, { "mp1", 1, 0, 1 }, { "vorbis", 1, 0, 0 },
    /* An MP3 frame's data may start up to 511 bytes back, in earlier frames. */
    { "mp3", 1, 1024, 1 },
    /* Opus converges within its time pre-roll (seek_preroll_s). */
    { "opus", 1, 0, 1 },
    /* TrueHD decodes from a major sync, at least every 128 access units. */
    { "truehd", 130, 0, 0 }, { "mlp", 130, 0, 0 },
    { "flac", 0, 0, 1 }, { "alac", 0, 0, 1 },
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

static fa_result reserve(float** buf, fa_frames* cap, fa_frames frames, uint32_t channels)
{
    fa_frames need = frames * (fa_frames)channels;
    if (need > *cap) {
        fa_frames c = *cap ? *cap : 8192;
        float* p;
        while (c < need) c *= 2;
        p = (float*)realloc(*buf, (size_t)c * sizeof(float));
        if (!p) return FA_ERROR_OUT_OF_MEMORY;
        *buf = p;
        *cap = c;
    }
    return FA_OK;
}

/* The codec's output in a format other than the one reported at open (a stream
 * that changes mid-way): converted with miniaudio, into t->tmp. */
static fa_result convert(fa_track* t, const float** data, fa_frames* frames)
{
    uint32_t rate = t->codec.rate, ch = t->codec.channels, out_ch = t->dec->format.channels;
    ma_uint64 expect = 0;
    fa_frames done_in = 0, done_out = 0, n = *frames;
    fa_result r;
    if (!t->conv_ready || t->conv_rate != rate || t->conv_channels != ch) {
        ma_data_converter_config cc;
        if (t->conv_ready) { ma_data_converter_uninit(&t->conv, NULL); t->conv_ready = 0; }
        cc = ma_data_converter_config_init(ma_format_f32, ma_format_f32, ch, out_ch, rate, t->dec->format.sample_rate);
        if (ma_data_converter_init(&cc, NULL, &t->conv) != MA_SUCCESS) return FA_ERROR_OUT_OF_MEMORY;
        t->conv_ready = 1;
        t->conv_rate = rate;
        t->conv_channels = ch;
    }
    ma_data_converter_get_expected_output_frame_count(&t->conv, (ma_uint64)n, &expect);
    r = reserve(&t->tmp, &t->tmp_cap, (fa_frames)expect + 64, out_ch);
    if (r != FA_OK) return r;
    while (done_in < n) {
        ma_uint64 in_n = (ma_uint64)(n - done_in);
        ma_uint64 out_n = (ma_uint64)(t->tmp_cap / out_ch - done_out);
        if (out_n == 0) {
            r = reserve(&t->tmp, &t->tmp_cap, done_out * 2 + 64, out_ch);
            if (r != FA_OK) return r;
            continue;
        }
        ma_data_converter_process_pcm_frames(&t->conv, *data + (size_t)done_in * ch, &in_n,
                                             t->tmp + (size_t)done_out * out_ch, &out_n);
        done_in += (fa_frames)in_n;
        done_out += (fa_frames)out_n;
        if (in_n == 0 && out_n == 0) break;
    }
    *data = t->tmp;
    *frames = done_out;
    return FA_OK;
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

/* Lays the codec's output for packet `pk` on the timeline and keeps the part
 * that is handed out. */
static fa_result emit(fa_track* t, const fa_demux_packet* pk)
{
    fa_decoder* dec = t->dec;
    uint32_t ch = dec->format.channels;
    const float* data = t->codec.out;
    fa_frames m = t->codec.out_frames, d, dc, pos, lo, hi, zlo = 0, zhi = 0;
    int placing = t->place;
    fa_result r;

    if (t->codec.rate != dec->format.sample_rate || t->codec.channels != ch) {
        r = convert(t, &data, &m);
        if (r != FA_OK) return r;
    }
    /* The packet's duration: the codec's own count where it has one (exact), else
     * the container's if that is exact. */
    dc = pk->duration >= 0 ? (fa_frames)llround((double)pk->duration * t->fpt) : -1;
    d = packet_frames(t, pk->data, pk->size);
    if (d < 0 && t->p.precision == 0) d = dc;

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
            if (d > m) pos += d - m;
        } else {
            pos = t->next_pos;
        }
        /* Audio starts later than asked (an empty edit, a decoder that waits for a
         * sync frame): silence keeps the positions right. */
        if (pos > gap_from && pos - gap_from <= (fa_frames)dec->format.sample_rate * 30) {
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
    r = reserve(&t->buf, &t->buf_cap, (zhi - zlo) + (hi - lo), ch);
    if (r != FA_OK) return r;
    if (zhi > zlo) memset(t->buf, 0, (size_t)(zhi - zlo) * ch * sizeof(float));
    if (hi > lo)
        memcpy(t->buf + (size_t)(zhi - zlo) * ch, data + (size_t)(lo - pos) * ch, (size_t)(hi - lo) * ch * sizeof(float));
    t->count = (zhi - zlo) + (hi - lo);
    t->off = 0;
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
        r = t->ops->next(t->ctx, &pk);
        if (r == FA_ERROR_END_OF_STREAM) { t->eof = 1; break; }
        if (r != FA_OK) return r;
        r = t->codec.vt->decode(&t->codec, pk.data, pk.size);
        if (r == FA_ERROR_OUT_OF_MEMORY) return r;
        if (r != FA_OK || t->codec.out_frames <= 0 || t->codec.rate == 0 || t->codec.channels == 0) {
            /* A corrupt packet is skipped. A codec that never makes a sound from
             * this track's first packets cannot decode it. */
            if (!t->format_known && ++silent > 256) return FA_ERROR_UNSUPPORTED_FORMAT;
            continue;
        }
        if (!t->format_known) learn_format(t);
        r = emit(t, &pk);
        if (r != FA_OK) return r;
    }
    return FA_OK;
}

fa_result fa_track_open(fa_track* t, fa_decoder* dec, const fa_demux_ops* ops, void* ctx, const fa_track_params* p)
{
    const codec_info* ci;
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
    /* The codec name is a static string; the extradata was only needed to open it. */
    t->p.codec.extradata = NULL;
    t->p.codec.extradata_size = 0;
    ci = find_codec(p->codec.codec);
    t->packets_back = ci ? ci->packets_back : pcm_bytes(p->codec.codec) > 0 ? 0 : 1;
    t->bytes_back = ci ? ci->bytes_back : 0;
    if (strcmp(p->codec.codec, "opus") == 0 && t->p.seek_preroll_s <= 0) t->p.seek_preroll_s = 0.08;
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
            memcpy(out + (size_t)produced * ch, t->buf + (size_t)t->off * ch, (size_t)n * ch * sizeof(float));
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
    if (t->codec.vt->flush) t->codec.vt->flush(&t->codec);
    if (t->conv_ready) ma_data_converter_reset(&t->conv);
    t->count = t->off = 0;
    t->eof = 0;
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
    if (t->conv_ready) ma_data_converter_uninit(&t->conv, NULL);
    free(t->buf);
    free(t->tmp);
    memset(t, 0, sizeof(*t));
}

/* ---- AAC ---------------------------------------------------------------------------- */

static const uint32_t aac_rates[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                        22050, 16000, 12000, 11025, 8000, 7350 };

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
        if (aac_rates[i] == rate) { put_bits(w, (uint32_t)i, 4); return; }
    put_bits(w, 15, 4);
    put_bits(w, rate, 24);
}

/* ISO/IEC 14496-3 1.6.2.1 AudioSpecificConfig with a GASpecificConfig. */
size_t fa_aac_build_asc(uint8_t out[8], int object_type, uint32_t rate, uint32_t channels, uint32_t sbr_rate)
{
    bitw w;
    uint32_t cfg = channels <= 6 ? channels : channels == 8 ? 7 : 2;
    memset(out, 0, 8);
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
