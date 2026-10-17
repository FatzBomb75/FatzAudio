#ifndef FA_INTERNAL_H
#define FA_INTERNAL_H

#include "fatzaudio/fatzaudio.h"
#include "miniaudio.h"

#define FA_DEVICE_CHANNELS 2

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
    ma_decoder decoder;
    int decoder_ready;
    ma_mutex lock;             /* guards decoder, state and params */
    uint8_t* memory;           /* owned copy for open_memory */
    size_t memory_size;
    char* path;                /* owned copy for open_path */
    uint32_t flags;
    char codec[32];
    char container[32];
    fa_stream_state state;
    float volume;
    float pan;
    fa_device* device;         /* NULL = decode-only or unattached */
    struct fa_stream* next;
};

/* device.c */
void fa_device_mix(fa_device* device, float* out, ma_uint32 frames);

/* stream.c: caller holds the device lock; stream lock is taken inside. */
void fa_stream_mix_into(struct fa_stream* stream, float* out, ma_uint32 frames);

#endif
