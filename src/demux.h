/* Shared parts of the container demuxers (demux_mp4.c, demux_mkv.c): a buffered
 * reader over fa_source and the track decoder that turns one track's packets into
 * the decoder's F32 frames (fa_codec), with sample-accurate seeking. */
#ifndef FA_DEMUX_H
#define FA_DEMUX_H

#include "fa_internal.h"

#define FA_DEMUX_NO_PTS INT64_MIN

/* ---- Buffered reader --------------------------------------------------------
 * Small reads (box and element headers) come from a buffer. The read-ahead
 * starts small after every jump and grows while reading goes on in sequence, so
 * skipping over video costs little more than the headers. */

typedef struct fa_br {
    fa_source* src;
    uint8_t* buf;
    size_t cap;     /* allocated */
    size_t chunk;   /* next refill size */
    size_t len;     /* valid bytes in buf */
    size_t off;     /* read cursor in buf */
    int64_t base;   /* file offset of buf[0]; the source sits at base + len */
} fa_br;

fa_result fa_br_init(fa_br* b, fa_source* src);
void fa_br_free(fa_br* b);
int64_t fa_br_tell(const fa_br* b);
/* Forward-only sources can only go forward (or back inside the buffer). */
fa_result fa_br_seek(fa_br* b, int64_t pos);
/* Bytes read: fewer than n only at the end of the data or on an error. */
size_t fa_br_read(fa_br* b, void* out, size_t n);
/* Reads n bytes into *buf (grown as needed, at most `max`). FA_ERROR_END_OF_STREAM
 * if the data ends first. */
fa_result fa_br_read_alloc(fa_br* b, size_t n, size_t max, uint8_t** buf, size_t* cap);

/* Big-endian helpers. */
static inline uint16_t fa_be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t fa_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline uint64_t fa_be64(const uint8_t* p) { return ((uint64_t)fa_be32(p) << 32) | fa_be32(p + 4); }

/* ---- Track decoder -------------------------------------------------------------- */

typedef struct fa_demux_packet {
    const uint8_t* data;
    size_t size;
    int64_t pts;       /* time base units, FA_DEMUX_NO_PTS = right after the previous packet */
    int64_t duration;  /* time base units, -1 = unknown */
    int64_t trim_end;  /* time base units to drop from the end of this packet's audio */
} fa_demux_packet;

typedef struct fa_demux_ops {
    /* The next packet of the selected track, valid until the next call.
     * FA_ERROR_END_OF_STREAM at the end (also for data cut short). */
    fa_result (*next)(void* ctx, fa_demux_packet* out);
    /* Makes `next` return, as the first packet to decode, the one holding time `t`
     * moved back by at least `packets_back` packets and until the packets in front
     * of it hold `bytes_back` bytes (decoders with a bit reservoir). */
    fa_result (*seek)(void* ctx, int64_t t, int packets_back, size_t bytes_back);
} fa_demux_ops;

typedef struct fa_track_params {
    fa_codec_params codec;   /* name, container rate and channels, extradata */
    double time_base;        /* seconds per pts unit */
    int64_t start;           /* pts of the first frame to play (priming, codec delay) */
    double lead_s;           /* silence before it (an initial empty edit) */
    int64_t end;             /* pts after the last frame, FA_DEMUX_NO_PTS = unknown */
    int clip_to_end;         /* `end` is exact: drop decoded frames past it */
    double precision;        /* how far container timestamps may be off (pts units), 0 = exact */
    double seek_preroll_s;   /* decode at least this much before a seek target (Opus) */
    int exact_durations;     /* packet durations are exact (MP4): audio a packet decodes
                                beyond its duration is cut (a shortened last packet) */
    /* The last packet, when the demuxer looked at the end of the file: the length
     * follows from it (snapped like any other position). FA_DEMUX_NO_PTS = unknown. */
    int64_t last_pts;
    int64_t last_end;        /* pts after its audio */
    int last_exact;          /* last_end is exact, else rounded: then a packet of the codec's
                                constant size, less last_trim, is used where there is one */
    int64_t last_trim;
} fa_track_params;

typedef struct fa_track {
    fa_decoder* dec;
    const fa_demux_ops* ops;
    void* ctx;
    fa_codec codec;
    int codec_ready;
    fa_track_params p;
    int packets_back;
    size_t bytes_back;
    double fpt;               /* output frames per pts unit */
    fa_frames lead;           /* lead_s in frames */
    fa_frames length;         /* -1 = unknown */
    /* Decoded, trimmed frames in the decoder's format, waiting to be read. */
    float* buf;
    fa_frames buf_cap, count, off;
    fa_frames next_pos;       /* position right after the last decoded packet */
    fa_frames drop_before;    /* frames before this position are not handed out */
    int place;                /* the next packet with audio fixes the position (open, seek) */
    int eof;
    int format_known;
    /* Container timestamps that are rounded (Matroska) are snapped to the grid of
     * a codec with a constant packet duration, which makes seeks exact there. */
    fa_frames grid, grid_origin;
    int grid_state;           /* 0 = not learned yet, 1 = holds, 2 = odd packet seen, -1 = off */
    /* When the codec's output changes format mid-stream. */
    ma_data_converter conv;
    int conv_ready;
    uint32_t conv_rate, conv_channels;
    float* tmp;
    fa_frames tmp_cap;
} fa_track;

/* Opens the codec and decodes up to the first audio, so dec->format is the real
 * output format (HE-AAC doubles the container's rate). Sets dec->format, length,
 * codec name and fast_seek-related fields. On failure everything is released. */
fa_result fa_track_open(fa_track* t, fa_decoder* dec, const fa_demux_ops* ops, void* ctx, const fa_track_params* p);
fa_result fa_track_read(fa_track* t, float* out, fa_frames frames, fa_frames* got);
fa_result fa_track_seek(fa_track* t, fa_frames frame);
void fa_track_close(fa_track* t);

/* Samples in packet `d` from its own bytes (PCM, Opus, MPEG audio), at *out_rate;
 * -1 if the codec does not tell. */
int64_t fa_demux_packet_samples(const char* codec, uint32_t channels, uint32_t rate,
                                const uint8_t* d, size_t n, uint32_t* out_rate);

/* AudioSpecificConfig for AAC from the container's rate and channels, for files
 * that do not carry one. `sbr_rate` > rate signals HE-AAC. Returns its size. */
/* At most 5 + 28 + 4 + 28 + 5 + 3 = 73 bits: two explicit 24-bit rates with SBR. */
#define FA_AAC_ASC_MAX 16
size_t fa_aac_build_asc(uint8_t out[FA_AAC_ASC_MAX], int object_type, uint32_t rate, uint32_t channels, uint32_t sbr_rate);

#endif
