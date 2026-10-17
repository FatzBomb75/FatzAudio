#ifndef FA_INTERNAL_H
#define FA_INTERNAL_H

#include "fatzaudio/fatzaudio.h"
#include "miniaudio.h"
#include <stdio.h>

#define FA_DEVICE_CHANNELS 2
#define FA_PROBE_BYTES 8192
#define FA_MARKER_COUNT 5

/* ---- Sources (source.c) ------------------------------------------------- */

enum { FA_SOURCE_FILE = 1, FA_SOURCE_MEMORY, FA_SOURCE_IO };

typedef struct fa_source {
    int kind;
    FILE* file;
    const uint8_t* mem;
    size_t mem_size;
    uint8_t* owned;
    fa_io_callbacks io;
    void* io_user;
    int64_t pos;       /* logical read position */
    int64_t raw_pos;   /* position of the app's callbacks (FA_SOURCE_IO) */
    int64_t length;    /* bytes, -1 = unknown */
    int seekable;
    int prefix_filled;
    size_t prefix_len;
    uint8_t prefix[FA_PROBE_BYTES];
} fa_source;

fa_result fa_source_open_path(fa_source* src, const char* path);
fa_result fa_source_open_memory(fa_source* src, const void* data, size_t size);
fa_result fa_source_open_io(fa_source* src, const fa_io_callbacks* io, void* user);
/* Returns the first bytes of the source (up to FA_PROBE_BYTES) without moving the read position. */
size_t fa_source_peek(fa_source* src, const uint8_t** out_data);
int64_t fa_source_read(fa_source* src, void* buf, size_t n); /* bytes read, 0 at end, <0 fa_result */
fa_result fa_source_seek(fa_source* src, int64_t offset);
int64_t fa_source_tell(const fa_source* src);
void fa_source_close(fa_source* src);

/* ---- Decoder registry (decoder.c) --------------------------------------- */

typedef struct fa_decoder fa_decoder;

/* One entry per format. Adding a format = one more entry in the registry table;
 * nothing else in the engine changes. */
typedef struct fa_decoder_vtable {
    const char* name;
    const char* description;
    const char* extensions;  /* ';'-separated, lower case */
    const char* mime_types;  /* ';'-separated */
    /* Confidence 0..100 that `data` (the start of the source) is this format.
     * `id3_size` is the size of a leading ID3v2 tag, already parsed. */
    int (*probe)(const uint8_t* data, size_t size, size_t id3_size);
    fa_result (*open)(fa_decoder* dec);
    /* Interleaved F32 in dec->format. FA_OK with *got > 0, or FA_ERROR_END_OF_STREAM with *got == 0. */
    fa_result (*read)(fa_decoder* dec, float* out, fa_frames frames, fa_frames* got);
    fa_result (*seek)(fa_decoder* dec, fa_frames frame);
    void (*close)(fa_decoder* dec);
} fa_decoder_vtable;

struct fa_decoder {
    const fa_decoder_vtable* vt;
    fa_source* src;
    void* impl;
    fa_audio_format format;  /* always FA_FORMAT_F32 interleaved */
    fa_frames length;        /* -1 = unknown */
    int seekable;
    char codec[32];
    char container[32];
};

/* Detects the format of `src` and opens the matching decoder. `name_hint` (a path
 * or NULL) and `mime_hint` only break ties when the content is ambiguous. */
fa_result fa_decoder_open(fa_decoder* dec, fa_source* src, const char* name_hint, const char* mime_hint);
fa_result fa_decoder_read(fa_decoder* dec, float* out, fa_frames frames, fa_frames* got);
fa_result fa_decoder_seek(fa_decoder* dec, fa_frames frame);
void fa_decoder_close(fa_decoder* dec);

/* Registry contents, defined in decoder.c. */
extern const fa_decoder_vtable* const fa_decoder_registry[];
extern const uint32_t fa_decoder_registry_count;

/* Format decoders backed by miniaudio's embedded dr_wav / dr_flac / dr_mp3 (dec_miniaudio.c). */
extern const fa_decoder_vtable fa_decoder_wav;
extern const fa_decoder_vtable fa_decoder_flac;
extern const fa_decoder_vtable fa_decoder_mp3;

/* Probing helpers (probe.c). */
size_t fa_probe_id3v2_size(const uint8_t* data, size_t size);
int fa_probe_wav(const uint8_t* data, size_t size, size_t id3_size);
int fa_probe_flac(const uint8_t* data, size_t size, size_t id3_size);
int fa_probe_mp3(const uint8_t* data, size_t size, size_t id3_size);

/* ---- Engine, devices, streams ------------------------------------------- */

struct fa_engine {
    fa_engine_config config;
    ma_context context;
    ma_mutex lock; /* guards device enumeration scratch (ma_context device lists) */
};

struct fa_stream;

struct fa_device {
    fa_engine* engine;
    ma_device device;
    ma_mutex lock;             /* guards the attached stream list */
    struct fa_stream* streams; /* singly linked list of attached streams */
};

struct fa_stream {
    fa_engine* engine;
    fa_source source;
    fa_decoder decoder;
    int decoder_ready;
    ma_mutex lock;             /* guards decoder, state and params */
    uint32_t flags;
    fa_stream_state state;
    float volume;
    float pan;
    fa_frames markers[FA_MARKER_COUNT];
    fa_frames position;        /* source frames handed to the voice chain */

    /* Voice chain while attached: source format -> device format. */
    fa_device* device;         /* NULL = decode-only or unattached */
    ma_data_converter converter;
    int converter_ready;
    float* in_buf;             /* decoded source frames waiting for the converter */
    fa_frames in_cap, in_count, in_offset;
    struct fa_stream* next;
};

/* device.c */
void fa_device_mix(fa_device* device, float* out, ma_uint32 frames);

/* stream.c: caller holds the device lock; stream lock is taken inside. */
void fa_stream_mix_into(struct fa_stream* stream, float* out, ma_uint32 frames);

#endif
