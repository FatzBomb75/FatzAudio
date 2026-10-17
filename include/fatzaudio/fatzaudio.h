/*
 * FatzAudio public C API.
 *
 * Conventions
 * - Every function returns fa_result (FA_OK == 0, negative on error) unless it
 *   is a pure getter documented otherwise. There is no thread-local "last error".
 * - All objects are opaque handles owned by the caller and released with the
 *   matching *_destroy / *_close call. There is no per-thread "current device".
 * - Time is expressed in fa_time (int64 nanoseconds) for clocks and markers,
 *   and in frames (int64, one frame = one sample per channel) for sample-exact
 *   positions.
 * - Strings are UTF-8.
 * - The API is thread-safe unless a function says otherwise. Callbacks run on
 *   engine threads and must not block.
 */
#ifndef FATZAUDIO_H
#define FATZAUDIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  if defined(FA_BUILDING)
#    define FA_API __declspec(dllexport)
#  elif defined(FA_STATIC)
#    define FA_API
#  else
#    define FA_API __declspec(dllimport)
#  endif
#  define FA_CALL __cdecl
#else
#  define FA_API __attribute__((visibility("default")))
#  define FA_CALL
#endif

#define FA_VERSION_MAJOR 0
#define FA_VERSION_MINOR 1
#define FA_VERSION_PATCH 0

/* ---- Basic types -------------------------------------------------------- */

typedef int32_t fa_result;
typedef int64_t fa_time;   /* nanoseconds */
typedef int64_t fa_frames; /* sample frames */

enum {
    FA_OK = 0,
    FA_ERROR_GENERIC = -1,
    FA_ERROR_INVALID_ARG = -2,
    FA_ERROR_OUT_OF_MEMORY = -3,
    FA_ERROR_NOT_FOUND = -4,
    FA_ERROR_UNSUPPORTED_FORMAT = -5,
    FA_ERROR_DEVICE = -6,
    FA_ERROR_IO = -7,
    FA_ERROR_NETWORK = -8,
    FA_ERROR_NOT_SEEKABLE = -9,
    FA_ERROR_BUSY = -10,
    FA_ERROR_END_OF_STREAM = -11,
    FA_ERROR_NOT_IMPLEMENTED = -12,
    FA_ERROR_PROVIDER_MISSING = -13 /* e.g. FFmpeg or LAME not found at runtime */
};

typedef struct fa_engine fa_engine;   /* one per process is typical */
typedef struct fa_device fa_device;   /* an opened output device */
typedef struct fa_stream fa_stream;   /* any playable/decodable source */
typedef struct fa_bus fa_bus;         /* a mix bus (deck/group) */

typedef enum fa_sample_format {
    FA_FORMAT_F32 = 0, /* interleaved float, the internal format */
    FA_FORMAT_S16,
    FA_FORMAT_S24,     /* packed 3 bytes */
    FA_FORMAT_S32,
    FA_FORMAT_U8,
    FA_FORMAT_F64,
    FA_FORMAT_F32_PLANAR,
    FA_FORMAT_S16_PLANAR,
    FA_FORMAT_S32_PLANAR,
    FA_FORMAT_F64_PLANAR
} fa_sample_format;

typedef struct fa_audio_format {
    fa_sample_format sample_format;
    uint32_t sample_rate;
    uint32_t channels;
    uint64_t channel_mask; /* speaker layout bits, 0 = default for channel count */
} fa_audio_format;

/* ---- Library ------------------------------------------------------------ */

FA_API uint32_t FA_CALL fa_version(void); /* (major << 16) | (minor << 8) | patch */
FA_API const char* FA_CALL fa_result_string(fa_result result);

typedef struct fa_engine_config {
    uint32_t struct_size;         /* sizeof(fa_engine_config) */
    uint32_t mix_sample_rate;     /* 0 = device native */
    uint32_t period_ms;           /* 0 = backend default */
    uint32_t worker_threads;      /* for scanning/decoding, 0 = number of cores */
    const char* cache_directory;  /* loudness/seek-index cache, NULL = disabled */
} fa_engine_config;

FA_API void FA_CALL fa_engine_config_init(fa_engine_config* config);
FA_API fa_result FA_CALL fa_engine_create(const fa_engine_config* config, fa_engine** out_engine);
FA_API void FA_CALL fa_engine_destroy(fa_engine* engine);

/* ---- Devices ------------------------------------------------------------ */

typedef enum fa_device_type { FA_DEVICE_PLAYBACK = 1, FA_DEVICE_CAPTURE = 2 } fa_device_type;

typedef struct fa_device_info {
    char id[256];      /* stable backend id, pass to fa_device_open */
    char name[256];
    fa_device_type type;
    int32_t is_default;
    uint32_t native_sample_rate;
    uint32_t native_channels;
} fa_device_info;

FA_API fa_result FA_CALL fa_device_count(fa_engine* engine, fa_device_type type, uint32_t* out_count);
FA_API fa_result FA_CALL fa_device_get_info(fa_engine* engine, fa_device_type type, uint32_t index, fa_device_info* out_info);

/* device_id NULL = system default; follows default-device changes automatically. */
FA_API fa_result FA_CALL fa_device_open(fa_engine* engine, const char* device_id, fa_device** out_device);
FA_API void FA_CALL fa_device_close(fa_device* device);
FA_API fa_result FA_CALL fa_device_start(fa_device* device);
FA_API fa_result FA_CALL fa_device_stop(fa_device* device);
FA_API fa_result FA_CALL fa_device_set_volume(fa_device* device, float volume);
FA_API fa_result FA_CALL fa_device_get_volume(fa_device* device, float* out_volume);
FA_API fa_result FA_CALL fa_device_get_latency(fa_device* device, fa_time* out_latency);

/* ---- Opening streams ---------------------------------------------------- */

/* Custom I/O. Return bytes read, 0 at end, negative on error. */
typedef struct fa_io_callbacks {
    int64_t (FA_CALL *read)(void* user, void* buffer, size_t bytes);
    fa_result (FA_CALL *seek)(void* user, int64_t offset); /* absolute; NULL = not seekable */
    int64_t (FA_CALL *length)(void* user);                 /* NULL or <0 = unknown */
    void (FA_CALL *close)(void* user);                     /* may be NULL */
} fa_io_callbacks;

typedef enum fa_open_flags {
    FA_OPEN_DECODE_ONLY = 1 << 0,  /* not attached to a device; pull with fa_stream_read */
    FA_OPEN_LOOP        = 1 << 1,
    FA_OPEN_PRESCAN     = 1 << 2,  /* build exact seek index / length before returning */
    FA_OPEN_NO_NORMALIZE = 1 << 3  /* opt this stream out of engine normalization */
} fa_open_flags;

typedef struct fa_open_options {
    uint32_t struct_size;
    uint32_t flags;          /* fa_open_flags */
    int32_t audio_track;     /* for containers with several audio tracks, -1 = default */
    const char* mime_hint;   /* optional, content sniffing still wins */
} fa_open_options;

FA_API void FA_CALL fa_open_options_init(fa_open_options* options);

/* One entry point per source kind; format is detected from content. */
FA_API fa_result FA_CALL fa_stream_open_path(fa_engine* engine, const char* path, const fa_open_options* options, fa_stream** out_stream);
FA_API fa_result FA_CALL fa_stream_open_memory(fa_engine* engine, const void* data, size_t size, const fa_open_options* options, fa_stream** out_stream);
FA_API fa_result FA_CALL fa_stream_open_io(fa_engine* engine, const fa_io_callbacks* io, void* user, const fa_open_options* options, fa_stream** out_stream);
FA_API fa_result FA_CALL fa_stream_open_url(fa_engine* engine, const char* url, const fa_open_options* options, fa_stream** out_stream);

/* Push stream: the app supplies PCM (any fa_sample_format, planar or interleaved). */
FA_API fa_result FA_CALL fa_stream_open_push(fa_engine* engine, const fa_audio_format* format, const fa_open_options* options, fa_stream** out_stream);
FA_API fa_result FA_CALL fa_stream_push_pcm(fa_stream* stream, const void* const* planes, fa_frames frames, fa_time pts);

FA_API void FA_CALL fa_stream_close(fa_stream* stream);

/* ---- Stream info -------------------------------------------------------- */

typedef struct fa_stream_info {
    fa_audio_format format;      /* decoded output format */
    char codec[32];              /* "mp3", "flac", "eac3", ... */
    char container[32];          /* "mp4", "matroska", "ogg", "raw", ... */
    fa_frames length_frames;     /* -1 if unknown (live) */
    int32_t seekable;
    int32_t audio_track_count;
    int32_t has_cdg;             /* CD+G side stream present */
} fa_stream_info;

FA_API fa_result FA_CALL fa_stream_get_info(fa_stream* stream, fa_stream_info* out_info);

/* ---- Transport and position -------------------------------------------- */

FA_API fa_result FA_CALL fa_stream_attach(fa_stream* stream, fa_device* device, fa_bus* bus); /* bus may be NULL = master */
FA_API fa_result FA_CALL fa_stream_play(fa_stream* stream);
FA_API fa_result FA_CALL fa_stream_play_at(fa_stream* stream, fa_time engine_time); /* sample-accurate scheduled start */
FA_API fa_result FA_CALL fa_stream_pause(fa_stream* stream);
FA_API fa_result FA_CALL fa_stream_stop(fa_stream* stream);

typedef enum fa_stream_state {
    FA_STATE_STOPPED = 0, FA_STATE_PLAYING, FA_STATE_PAUSED, FA_STATE_BUFFERING, FA_STATE_ENDED
} fa_stream_state;

FA_API fa_result FA_CALL fa_stream_get_state(fa_stream* stream, fa_stream_state* out_state);
FA_API fa_result FA_CALL fa_stream_seek(fa_stream* stream, fa_frames frame);
FA_API fa_result FA_CALL fa_stream_get_position(fa_stream* stream, fa_frames* out_frame);

/* Presentation time of the audio currently leaving the speaker (device latency
 * included). Video and CD+G renderers should follow this clock. */
FA_API fa_result FA_CALL fa_stream_get_clock(fa_stream* stream, fa_time* out_pts);
FA_API fa_result FA_CALL fa_engine_get_time(fa_engine* engine, fa_time* out_time);

/* Decode-only streams: pull PCM in the stream's output format. */
FA_API fa_result FA_CALL fa_stream_read(fa_stream* stream, void* buffer, fa_frames frames, fa_frames* out_frames_read);

/* ---- Parameters and envelopes ----------------------------------------- */

typedef enum fa_param {
    FA_PARAM_VOLUME = 0, /* linear gain */
    FA_PARAM_PAN,        /* -1..1 */
    FA_PARAM_RATE,       /* playback rate multiplier (pitch follows) */
    FA_PARAM_TEMPO,      /* tempo multiplier, pitch preserved */
    FA_PARAM_PITCH       /* semitones, tempo preserved */
} fa_param;

typedef enum fa_curve {
    FA_CURVE_LINEAR = 0,
    FA_CURVE_EQUAL_POWER,
    FA_CURVE_LOG,
    FA_CURVE_EXP,
    FA_CURVE_SCURVE,
    FA_CURVE_BEZIER /* uses bezier_x1/y1/x2/y2 control points, CSS cubic-bezier style */
} fa_curve;

typedef struct fa_ramp {
    fa_curve curve;
    float bezier_x1, bezier_y1, bezier_x2, bezier_y2;
} fa_ramp;

FA_API fa_result FA_CALL fa_stream_set_param(fa_stream* stream, fa_param param, float value);
FA_API fa_result FA_CALL fa_stream_get_param(fa_stream* stream, fa_param param, float* out_value);
/* Ramp a parameter to target over duration, starting at engine_time (0 = now). */
FA_API fa_result FA_CALL fa_stream_ramp_param(fa_stream* stream, fa_param param, float target, fa_time duration, fa_time engine_time, const fa_ramp* ramp);

/* ---- Markers and crossfade -------------------------------------------- */

typedef enum fa_marker {
    FA_MARKER_CUE_IN = 0,
    FA_MARKER_CUE_OUT,
    FA_MARKER_FADE_IN_END,
    FA_MARKER_FADE_OUT_START,
    FA_MARKER_MIX_POINT /* where the next queued stream starts */
} fa_marker;

FA_API fa_result FA_CALL fa_stream_set_marker(fa_stream* stream, fa_marker marker, fa_frames frame);
FA_API fa_result FA_CALL fa_stream_get_marker(fa_stream* stream, fa_marker marker, fa_frames* out_frame);

FA_API fa_result FA_CALL fa_crossfade(fa_stream* from, fa_stream* to, fa_time duration, const fa_ramp* ramp);

/* ---- Normalization ---------------------------------------------------- */

typedef enum fa_normalize_mode {
    FA_NORMALIZE_OFF = 0,
    FA_NORMALIZE_TRACK,  /* per-stream integrated loudness */
    FA_NORMALIZE_ALBUM   /* use album gain tags when present */
} fa_normalize_mode;

typedef struct fa_loudness {
    double integrated_lufs;
    double range_lu;
    double true_peak_dbtp;
    int32_t is_final; /* 0 = estimate (scan in progress), 1 = complete */
} fa_loudness;

FA_API fa_result FA_CALL fa_engine_set_normalization(fa_engine* engine, fa_normalize_mode mode, double target_lufs);
FA_API fa_result FA_CALL fa_stream_get_loudness(fa_stream* stream, fa_loudness* out_loudness);
/* Standalone scan (no playback): fastest path, audio track only, all cores. */
FA_API fa_result FA_CALL fa_scan_loudness_path(fa_engine* engine, const char* path, fa_loudness* out_loudness);

/* ---- Events ----------------------------------------------------------- */

typedef enum fa_event_type {
    FA_EVENT_ENDED = 0,
    FA_EVENT_POSITION,        /* reached a requested frame */
    FA_EVENT_MARKER,
    FA_EVENT_METADATA,        /* tags / ICY title changed */
    FA_EVENT_STALLED,
    FA_EVENT_RESUMED,
    FA_EVENT_DECODE_ERROR,    /* recovered: data skipped, playback continues */
    FA_EVENT_DEVICE_CHANGED,
    FA_EVENT_LOUDNESS_FINAL
} fa_event_type;

typedef struct fa_event {
    fa_event_type type;
    fa_stream* stream;
    fa_frames frame;
    int64_t value;
} fa_event;

typedef void (FA_CALL *fa_event_callback)(const fa_event* event, void* user);

FA_API fa_result FA_CALL fa_stream_set_event_callback(fa_stream* stream, fa_event_callback callback, void* user);
FA_API fa_result FA_CALL fa_stream_add_position_event(fa_stream* stream, fa_frames frame);

/* ---- User DSP --------------------------------------------------------- */

/* In-place processing of interleaved float frames in the stream's format. */
typedef void (FA_CALL *fa_dsp_callback)(float* frames, fa_frames count, uint32_t channels, void* user);

FA_API fa_result FA_CALL fa_stream_add_dsp(fa_stream* stream, fa_dsp_callback callback, void* user, int32_t priority, uint32_t* out_dsp_id);
FA_API fa_result FA_CALL fa_stream_remove_dsp(fa_stream* stream, uint32_t dsp_id);

/* ---- Analysis --------------------------------------------------------- */

FA_API fa_result FA_CALL fa_stream_get_levels(fa_stream* stream, fa_time window, float* out_peak, float* out_rms, uint32_t channels);

#ifdef __cplusplus
}
#endif

#endif /* FATZAUDIO_H */
