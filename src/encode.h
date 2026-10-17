/* Encoders (encode.c) and their formats (enc_wav.c, enc_flac.c, enc_mp3.c).
 *
 * encode.c owns the encoder thread, the input queue, the taps on devices and
 * streams, and the conversion to the output format. A format only sees
 * interleaved float frames already at the output's rate and channel count, and
 * writes bytes through the sink. */
#ifndef FA_ENCODE_H
#define FA_ENCODE_H

#include "fa_internal.h"

/* ---- Output sink: a file or the app's callbacks, buffered ------------------- */

typedef struct fa_enc_sink {
    FILE* file;
    fa_encoder_output out;       /* out.write NULL when writing to `file` */
    void* user;
    int seekable;
    int64_t pos;                 /* where the next byte goes */
    int64_t written;             /* bytes handed to the file or callbacks (status) */
    uint8_t* buf;
    size_t fill;                 /* bytes in buf, which start at pos - fill */
    fa_result error;             /* sticky */
} fa_enc_sink;

fa_result fa_sink_write(fa_enc_sink* s, const void* data, size_t size);
fa_result fa_sink_write_u8(fa_enc_sink* s, uint32_t v);
fa_result fa_sink_write_le16(fa_enc_sink* s, uint32_t v);
fa_result fa_sink_write_le32(fa_enc_sink* s, uint32_t v);
fa_result fa_sink_write_le64(fa_enc_sink* s, uint64_t v);
fa_result fa_sink_write_be32(fa_enc_sink* s, uint32_t v);
/* Only when s->seekable. Flushes first. */
fa_result fa_sink_seek(fa_enc_sink* s, int64_t pos);
fa_result fa_sink_flush(fa_enc_sink* s);

/* ---- Tags, copied from the options ------------------------------------------- */

typedef struct fa_enc_tags {
    char* title;
    char* artist;
    char* album;
    char* genre;
    char* date;
    char* comment;
    uint32_t track;
} fa_enc_tags;

/* ---- Formats -------------------------------------------------------------------- */

typedef struct fa_enc_format {
    uint32_t rate, channels;
    uint32_t bits;               /* 16, 24 or 32 (float) for WAV/FLAC; 0 for MP3 */
    uint32_t bitrate_kbps;
    int32_t quality;             /* -1 = the format's default */
} fa_enc_format;

typedef struct fa_enc_backend {
    /* Called while the encoder is created: options the format cannot do. `channels`
     * and `rate` may be 0 (the input's, not known yet). */
    fa_result (*check)(const fa_enc_format* f);
    /* Adjusts the output format chosen for an input (MP3: allowed rates, at most 2 channels). */
    void (*choose)(fa_enc_format* f);
    /* Writes the header; *impl is the format's state. */
    fa_result (*begin)(void** impl, const fa_enc_format* f, const fa_enc_tags* tags, fa_enc_sink* sink);
    fa_result (*write)(void* impl, const float* frames, int64_t count, fa_enc_sink* sink);
    /* Flushes and completes the header (if the sink can seek). */
    fa_result (*finish)(void* impl, fa_enc_sink* sink);
    void (*free)(void* impl);
} fa_enc_backend;

extern const fa_enc_backend fa_enc_wav;
extern const fa_enc_backend fa_enc_flac;
extern const fa_enc_backend fa_enc_mp3;

/* Float to integer samples of `bits` (16 or 24), clipped. 16 bit is TPDF dithered
 * (`seed` is the dither's state), except samples already exactly on the 16-bit grid. */
void fa_enc_quantize(const float* in, int32_t* out, int64_t count, uint32_t bits, uint32_t* seed);

/* WAVE_FORMAT_EXTENSIBLE / FLAC speaker layout for a channel count. */
uint32_t fa_enc_channel_mask(uint32_t channels);

/* ---- LAME (enc_mp3.c) ------------------------------------------------------------ */

/* Loads LAME if it is not loaded yet (steps 1 and 2 of fa_lame_load). 1 when usable. */
int fa_lame_available(void);

/* ---- Taps, called by the device and the stream mixer ------------------------------ */

/* What the audio thread reads: the encoders recording a device or a stream.
 * Replaced as a whole (never edited) while the audio thread may read it. */
typedef struct fa_enc_list {
    uint32_t count;
    float* scratch;              /* streams: the stream's own mix, stereo */
    int64_t scratch_cap;         /* frames */
    struct fa_encoder* items[1];
} fa_enc_list;

/* Audio thread: stereo frames at device rate `rate`. Never blocks. */
void fa_encode_push(struct fa_encoder* e, const float* frames, int64_t count, uint32_t rate);
/* Audio thread: frames for an encoder that cannot take them (lost). */
void fa_encode_drop(struct fa_encoder* e, int64_t count);

/* The device or stream is going away: its encoders lose their source. Called at the
 * very start of fa_device_close / fa_stream_close, holding no locks. */
void fa_encode_device_closing(fa_device* d);
void fa_encode_stream_closing(fa_stream* s);

#endif
