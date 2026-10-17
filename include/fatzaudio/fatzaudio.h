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
 *
 * Threads
 * - Each engine runs decode worker threads (fa_engine_config.worker_threads) and
 *   one event thread. Workers decode, resample and channel-convert every attached
 *   stream ahead of time into a lock-free ring buffer (fa_open_options.buffer_ms).
 * - The device's audio callback only mixes those buffers: it takes no locks,
 *   allocates nothing, does no I/O and calls no user code.
 * - Event callbacks (fa_stream_set_event_callback) run on the engine's event
 *   thread, never on the audio thread, so they may call any FatzAudio function.
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

typedef enum fa_engine_flags {
    /* Devices play into a silent "null" output that runs in real time on its own
     * thread. For automated tests, CI machines without audio hardware, and servers. */
    FA_ENGINE_NULL_OUTPUT = 1 << 0
} fa_engine_flags;

typedef struct fa_engine_config {
    uint32_t struct_size;         /* sizeof(fa_engine_config) */
    uint32_t mix_sample_rate;     /* 0 = device native */
    uint32_t period_ms;           /* 0 = backend default */
    uint32_t worker_threads;      /* decode worker threads, 0 = number of cores (max 8) */
    const char* cache_directory;  /* loudness scan cache (one small file per scanned
                                     file, keyed by content), NULL = disabled.
                                     Created if missing (one level).
                                     The string is copied */
    uint32_t flags;               /* fa_engine_flags */
} fa_engine_config;

FA_API void FA_CALL fa_engine_config_init(fa_engine_config* config);
FA_API fa_result FA_CALL fa_engine_create(const fa_engine_config* config, fa_engine** out_engine);
/* Close every stream and device of the engine before destroying it. */
FA_API void FA_CALL fa_engine_destroy(fa_engine* engine);

/* ---- Supported formats -------------------------------------------------- */

/* Everything the built-in decoder registry can open. Replaces BASS_PluginGetInfo:
 * there are no plugins, every format is compiled in. */
typedef struct fa_format_info {
    char name[32];          /* registry name, e.g. "flac" */
    char description[64];   /* e.g. "Free Lossless Audio Codec" */
    char extensions[64];    /* typical file extensions, ';'-separated, e.g. "flac;fla" */
    char mime_types[128];   /* ';'-separated */
} fa_format_info;

FA_API uint32_t FA_CALL fa_format_count(void);
FA_API fa_result FA_CALL fa_format_get_info(uint32_t index, fa_format_info* out_info);

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
/* Detaches every stream still attached (they keep their position) and closes the device. */
FA_API void FA_CALL fa_device_close(fa_device* device);
/* Start and stop are idempotent: starting a running device or stopping a stopped
 * one returns FA_OK and does nothing. Attached streams keep their position. */
FA_API fa_result FA_CALL fa_device_start(fa_device* device);
FA_API fa_result FA_CALL fa_device_stop(fa_device* device);
/* *out_started = 1 while the device is running, else 0. */
FA_API fa_result FA_CALL fa_device_is_started(fa_device* device, int32_t* out_started);
FA_API fa_result FA_CALL fa_device_set_volume(fa_device* device, float volume);
FA_API fa_result FA_CALL fa_device_get_volume(fa_device* device, float* out_volume);
FA_API fa_result FA_CALL fa_device_get_latency(fa_device* device, fa_time* out_latency);

/* ---- Opening streams ---------------------------------------------------- */

/* Custom I/O for fa_stream_open_io.
 *
 * read   Required. Copy up to `bytes` bytes into `buffer` and return how many were
 *        copied; 0 means end of data, a negative fa_result means an I/O error.
 *        Short reads are fine.
 * seek   Optional. Move to the absolute byte `offset` and return FA_OK, or an error.
 *        NULL means the source can only be read forwards: the stream still opens
 *        (the first few KB are buffered for format detection) but cannot seek.
 * length Optional. Total size in bytes, or a negative value / NULL if unknown.
 * close  Optional. Called exactly once, when the stream is closed with
 *        fa_stream_close, after the last read/seek/length call. Release `user` here.
 *
 * Lifetime of `user`: the stream takes ownership only when fa_stream_open_io
 * returns FA_OK. If it fails, `close` is NOT called and the caller still owns
 * `user`. The callbacks are never called concurrently for one stream, but they
 * may be called from engine worker threads, not only the thread that opened it.
 * The fa_io_callbacks struct itself is copied, so it may live on the stack. */
typedef struct fa_io_callbacks {
    int64_t (FA_CALL *read)(void* user, void* buffer, size_t bytes);
    fa_result (FA_CALL *seek)(void* user, int64_t offset);
    int64_t (FA_CALL *length)(void* user);
    void (FA_CALL *close)(void* user);
} fa_io_callbacks;

typedef enum fa_open_flags {
    FA_OPEN_DECODE_ONLY = 1 << 0,  /* not attached to a device; pull with fa_stream_read */
    FA_OPEN_LOOP        = 1 << 1,
    FA_OPEN_PRESCAN     = 1 << 2,  /* build exact seek index / length before returning */
    FA_OPEN_NO_NORMALIZE = 1 << 3  /* opt this stream out of engine normalization
                                      (jingles, sound effects, already-mastered mixes) */
} fa_open_flags;

typedef struct fa_open_options {
    uint32_t struct_size;
    uint32_t flags;          /* fa_open_flags */
    int32_t audio_track;     /* for containers with several audio tracks, -1 = default;
                                FA_ERROR_NOT_FOUND if the track does not exist */
    const char* mime_hint;   /* optional, e.g. "audio/flac"; only used when the content
                                itself is ambiguous. Content sniffing always wins */
    uint32_t buffer_ms;      /* decode-ahead buffer while attached to a device,
                                0 = default (500 ms), clamped to 50..10000 */
} fa_open_options;

FA_API void FA_CALL fa_open_options_init(fa_open_options* options);

/* One entry point per source kind. The format is detected from the content
 * (magic bytes and frame sync, ID3v2 tags skipped), never from the file name;
 * the extension and mime_hint are only tie-breakers when the content is
 * ambiguous. There are no per-format open calls and no plugins.
 * Returns FA_ERROR_UNSUPPORTED_FORMAT when no built-in decoder recognises the data.
 * options may be NULL (defaults). fa_stream_open_memory copies the data. */
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
    fa_audio_format format;      /* the source's own decoded format (always F32 interleaved,
                                    at the source's rate and channel count). It does not
                                    change when the stream is attached to a device; the
                                    voice chain converts to the device format */
    char codec[32];              /* "mp3", "flac", "eac3", ... */
    char container[32];          /* "mp4", "matroska", "ogg", "raw", ... */
    fa_frames length_frames;     /* -1 if unknown (live) */
    int32_t seekable;
    int32_t audio_track_count;
    int32_t has_cdg;             /* CD+G side stream present */
    fa_time duration;            /* length in nanoseconds, -1 if unknown */
} fa_stream_info;

FA_API fa_result FA_CALL fa_stream_get_info(fa_stream* stream, fa_stream_info* out_info);

/* ---- Transport and position -------------------------------------------- */

/* Routes a stream to a device (bus NULL = the device's master mix). The decoder
 * is not reopened: position, format and parameters are kept. Attaching to the
 * device it is already on is a no-op; attaching to another device moves it there
 * (still keeping its position, and playing if it was), and a running ramp keeps
 * its remaining time.
 * Decode-only streams, and devices of another engine, cannot be attached
 * (FA_ERROR_INVALID_ARG). */
FA_API fa_result FA_CALL fa_stream_attach(fa_stream* stream, fa_device* device, fa_bus* bus);
/* Removes the stream from its device; it keeps its position. FA_OK if not attached. */
FA_API fa_result FA_CALL fa_stream_detach(fa_stream* stream);
FA_API fa_result FA_CALL fa_stream_play(fa_stream* stream);
FA_API fa_result FA_CALL fa_stream_play_at(fa_stream* stream, fa_time engine_time); /* sample-accurate scheduled start */
/* Pause and stop take effect before they return: no further audio of the stream
 * is mixed after that, so the position read right after is final. */
FA_API fa_result FA_CALL fa_stream_pause(fa_stream* stream);
FA_API fa_result FA_CALL fa_stream_stop(fa_stream* stream);

typedef enum fa_stream_state {
    FA_STATE_STOPPED = 0, FA_STATE_PLAYING, FA_STATE_PAUSED, FA_STATE_BUFFERING, FA_STATE_ENDED
} fa_stream_state;

FA_API fa_result FA_CALL fa_stream_get_state(fa_stream* stream, fa_stream_state* out_state);
/* Seeking an attached stream drops the decode-ahead buffer and refills the first
 * ~100 ms on the calling thread before returning, so playback resumes at once. */
FA_API fa_result FA_CALL fa_stream_seek(fa_stream* stream, fa_frames frame);
/* Source frames already handed to the device (or read, for decode-only streams).
 * Frames decoded ahead into the buffer are not counted. */
FA_API fa_result FA_CALL fa_stream_get_position(fa_stream* stream, fa_frames* out_frame);

/* Presentation time of the audio currently leaving the speaker: the stream
 * position converted to nanoseconds, minus the device latency (never below 0).
 * Video and CD+G renderers should follow this clock. Streams that are not
 * attached report their position. */
FA_API fa_result FA_CALL fa_stream_get_clock(fa_stream* stream, fa_time* out_pts);
FA_API fa_result FA_CALL fa_engine_get_time(fa_engine* engine, fa_time* out_time);

/* Decode-only streams (FA_OPEN_DECODE_ONLY): pull interleaved F32 PCM in the
 * format reported by fa_stream_get_info. `buffer` must hold frames * channels floats.
 * Returns FA_OK with 1..frames frames read (short reads happen; keep calling), and
 * FA_ERROR_END_OF_STREAM with *out_frames_read == 0 once the end is reached.
 * Returns FA_ERROR_BUSY for streams attached to a device (the device pulls them). */
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
    int64_t tag;   /* any value; returned in the FA_EVENT_RAMP_DONE event of this ramp */
} fa_ramp;

/* Volume (linear gain >= 0) and pan (-1..1) are applied in the mixer, sample-accurately
 * and without zipper noise (changes are smoothed over 5 ms). RATE, TEMPO and PITCH
 * return FA_ERROR_NOT_IMPLEMENTED until the tempo/pitch phase.
 * fa_stream_get_param returns the value being applied right now (mid-ramp values
 * included); a value just set with fa_stream_set_param is returned immediately. */
FA_API fa_result FA_CALL fa_stream_set_param(fa_stream* stream, fa_param param, float value);
FA_API fa_result FA_CALL fa_stream_get_param(fa_stream* stream, fa_param param, float* out_value);
/* Ramps a parameter from its current value to `target` over `duration` with the
 * curve in `ramp` (NULL = linear, tag 0). Ramps advance with playback: they run
 * while the stream is playing on a running device and wait while it is paused,
 * stopped or not attached, so "ramp to 1, then play" gives a fade-in.
 * A new ramp or fa_stream_set_param on the same parameter replaces a running ramp.
 * Every ramp raises exactly one FA_EVENT_RAMP_DONE (value = ramp->tag): when it
 * reaches the target, or when it is replaced.
 * engine_time must be 0 (start now); scheduled starts come with the crossfade phase. */
FA_API fa_result FA_CALL fa_stream_ramp_param(fa_stream* stream, fa_param param, float target, fa_time duration, fa_time engine_time, const fa_ramp* ramp);

/* ---- Markers and crossfade -------------------------------------------- */

typedef enum fa_marker {
    FA_MARKER_CUE_IN = 0,
    FA_MARKER_CUE_OUT,
    FA_MARKER_FADE_IN_END,
    FA_MARKER_FADE_OUT_START,
    FA_MARKER_MIX_POINT /* where the next queued stream starts */
} fa_marker;

/* "No marker": set a marker to FA_FRAMES_UNSET to clear it; get returns it for
 * markers that were never set. Any other negative frame is FA_ERROR_INVALID_ARG. */
#define FA_FRAMES_UNSET ((fa_frames)-1)

FA_API fa_result FA_CALL fa_stream_set_marker(fa_stream* stream, fa_marker marker, fa_frames frame);
FA_API fa_result FA_CALL fa_stream_get_marker(fa_stream* stream, fa_marker marker, fa_frames* out_frame);

FA_API fa_result FA_CALL fa_crossfade(fa_stream* from, fa_stream* to, fa_time duration, const fa_ramp* ramp);

/* ---- Normalization ---------------------------------------------------- */

/* Loudness normalization makes every stream play at the same perceived loudness
 * (ITU-R BS.1770-4 / EBU R128). Turn it on once for the engine; it then applies
 * to every stream played on any device, except streams opened with
 * FA_OPEN_NO_NORMALIZE and decode-only streams (fa_stream_read returns the
 * source's own samples).
 *
 * Where a stream's loudness comes from, best first:
 *   1. fa_stream_set_loudness (a value your app stored earlier),
 *   2. ReplayGain 2 / R128 tags in the file (ID3v2 TXXX, FLAC Vorbis comments, APEv2),
 *   3. the scan cache (fa_engine_config.cache_directory), keyed by file content,
 *   4. a background scan: only the audio is decoded, the file is split into chunks
 *      scanned on all cores at once, and the result is written to the cache,
 *   5. while none of those is known yet, and for live or app-I/O streams that
 *      cannot be scanned: a running estimate over the last 15 s of decoded audio.
 * Playback never waits for any of this. It starts at the best value known at that
 * moment and glides to the exact gain when the scan finishes (at most 12 dB/s
 * down, 6 dB/s up), so no jump is heard. FA_EVENT_LOUDNESS_FINAL is raised then.
 *
 * Gain = target - loudness, limited to -30..+12 dB. A boost can push peaks over
 * full scale, so each device also runs a true-peak limiter (fa_engine_set_limiter). */
typedef enum fa_normalize_mode {
    FA_NORMALIZE_OFF = 0,  /* default */
    FA_NORMALIZE_TRACK,    /* each stream to the target */
    FA_NORMALIZE_ALBUM     /* album gain from tags when present (keeps an album's
                              quiet and loud songs in proportion), else track */
} fa_normalize_mode;

typedef enum fa_loudness_source {
    FA_LOUDNESS_NONE = 0,  /* nothing known yet */
    FA_LOUDNESS_ESTIMATE,  /* running estimate from the audio decoded so far */
    FA_LOUDNESS_TAGS,      /* ReplayGain / R128 tags */
    FA_LOUDNESS_CACHE,     /* an earlier scan of the same content */
    FA_LOUDNESS_SCAN,      /* a full scan */
    FA_LOUDNESS_APP        /* fa_stream_set_loudness */
} fa_loudness_source;

typedef struct fa_loudness {
    double integrated_lufs;  /* integrated loudness; -INFINITY for digital silence */
    double range_lu;         /* loudness range (EBU Tech 3342) */
    double true_peak_dbtp;   /* true peak; from tags this is the sample peak */
    int32_t is_final;        /* 1 = TAGS, CACHE, SCAN or APP; 0 = estimate or nothing yet */
    int32_t source;          /* fa_loudness_source */
    double gain_db;          /* gain normalization applies to this stream with the
                                engine's current settings (0 when off or opted out) */
} fa_loudness;
/* Values that are not known are NAN (for example range and peak of an estimate). */

/* target_lufs: -16 suits music players, -14 matches streaming services, -23 is
 * EBU broadcast. Valid range -60..0. Takes effect on playing streams at once,
 * with the same glide. Starts the analysis of streams that are attached. */
FA_API fa_result FA_CALL fa_engine_set_normalization(fa_engine* engine, fa_normalize_mode mode, double target_lufs);

typedef enum fa_limiter_mode {
    FA_LIMITER_AUTO = 0,   /* default: on while normalization is on */
    FA_LIMITER_ON,
    FA_LIMITER_OFF
} fa_limiter_mode;

/* The output limiter of every device: true-peak detection (4x oversampled),
 * 1.5 ms look-ahead, smooth release. It never touches audio below the ceiling.
 * ceiling_dbtp: -1 recommended (valid -20..0). The look-ahead adds about 1.7 ms of
 * latency, always (also while off), so turning it on or off never clicks;
 * fa_device_get_latency includes it. */
FA_API fa_result FA_CALL fa_engine_set_limiter(fa_engine* engine, fa_limiter_mode mode, double ceiling_dbtp);

/* Best loudness known for the stream right now, and the gain it gets. Starts the
 * analysis if it has not run yet, even with normalization off, so apps can use
 * it for their own library. Never blocks on a scan. */
FA_API fa_result FA_CALL fa_stream_get_loudness(fa_stream* stream, fa_loudness* out_loudness);
/* Supplies a loudness your app measured or stored before (integrated_lufs is
 * required; range and peak may be NAN). It wins over tags, cache and scans, and
 * cancels a scan in progress. */
FA_API fa_result FA_CALL fa_stream_set_loudness(fa_stream* stream, const fa_loudness* loudness);
/* Standalone scan, no playback: decodes only the audio, in chunks on all cores,
 * and blocks until done. Uses and fills the cache. Tags are ignored (this measures). */
FA_API fa_result FA_CALL fa_scan_loudness_path(fa_engine* engine, const char* path, fa_loudness* out_loudness);

/* ---- Events ----------------------------------------------------------- */

/* Meaning of fa_event.frame and fa_event.value per type. "Position" is the stream
 * position (fa_stream_get_position) when the event happened.
 *
 * type                     frame                  value
 * FA_EVENT_ENDED           position at the end    0
 * FA_EVENT_POSITION        the requested frame    0
 * FA_EVENT_MARKER          the marker's frame     the fa_marker
 * FA_EVENT_METADATA        position               0
 * FA_EVENT_STALLED         position               0
 * FA_EVENT_RESUMED         position               0
 * FA_EVENT_DECODE_ERROR    position               the fa_result of the error
 * FA_EVENT_DEVICE_CHANGED  position               0
 * FA_EVENT_LOUDNESS_FINAL  position               0
 * FA_EVENT_RAMP_DONE       position               fa_ramp.tag of the finished ramp
 *
 * Delivered today: ENDED, RAMP_DONE and LOUDNESS_FINAL. The others are raised by the features
 * that produce them as those land (see docs/features). */
typedef enum fa_event_type {
    FA_EVENT_ENDED = 0,
    FA_EVENT_POSITION,        /* reached a requested frame */
    FA_EVENT_MARKER,
    FA_EVENT_METADATA,        /* tags / ICY title changed */
    FA_EVENT_STALLED,
    FA_EVENT_RESUMED,
    FA_EVENT_DECODE_ERROR,    /* recovered: data skipped, playback continues */
    FA_EVENT_DEVICE_CHANGED,
    FA_EVENT_LOUDNESS_FINAL,  /* the stream's final loudness is known (fa_stream_get_loudness) */
    FA_EVENT_RAMP_DONE        /* a ramp reached its target or was replaced */
} fa_event_type;

typedef struct fa_event {
    fa_event_type type;
    fa_stream* stream;
    fa_frames frame;
    int64_t value;
} fa_event;

typedef void (FA_CALL *fa_event_callback)(const fa_event* event, void* user);

/* One callback per stream (NULL removes it). Callbacks run on the engine's event
 * thread, in order, shortly after the event (within a few milliseconds); never on
 * the audio thread. A callback may call any FatzAudio function, including
 * fa_stream_close on its own stream, or fa_engine_destroy once the engine's
 * streams are closed (the event thread then finishes the teardown after the
 * callback returns). ENDED and RAMP_DONE are never dropped.
 * When this function or fa_stream_close returns, the previous callback is no
 * longer running (unless you call them from inside that callback), so its `user`
 * data can be freed. */
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
