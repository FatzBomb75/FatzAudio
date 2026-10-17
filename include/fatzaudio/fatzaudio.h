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
/* The device's sample clock: how much audio it has mixed since it was opened, in
 * nanoseconds (frames mixed / sample rate). It starts at 0, advances only while the
 * device runs, and never jumps. Every scheduled action on a stream or bus of this
 * device (fa_stream_play_at, ramps with a start time, queued streams) is placed on
 * this clock, to the exact frame. It moves in steps of one device period, so
 * schedule at least one period ahead: "now + 50 ms" is always safe. */
FA_API fa_result FA_CALL fa_device_get_time(fa_device* device, fa_time* out_time);

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

/* ---- Push streams and the FFmpeg hand-off --------------------------------
 *
 * For apps that already read the file themselves, typically with FFmpeg for the
 * video. Instead of decoding audio to PCM, converting it and keeping it in sync by
 * hand, hand FatzAudio what you already have and let the video follow its clock:
 *
 *   compressed packets   fa_stream_open_packets + fa_stream_push_packet: straight
 *                        from av_read_frame, no audio decoding in the app at all.
 *                        FatzAudio decodes them on its worker threads: PCM and MP3
 *                        built in, everything else (AAC, AC-3, E-AC-3, Opus, ...)
 *                        through the FFmpeg your app already loaded (never bundled).
 *   decoded frames       fa_stream_open_push + fa_stream_push_frame: AVFrame data
 *                        as it is (planar or interleaved, any sample format, any
 *                        rate), no swresample step.
 *
 * Both take FatzAudio's own small structs, so FFmpeg can be upgraded without
 * rebuilding FatzAudio. fatzaudio_ffmpeg.h fills them from AVCodecParameters,
 * AVPacket and AVFrame in one line each.
 *
 * Timestamps are yours: pass the packet or frame pts in its time base and
 * fa_stream_get_clock answers in the same timeline (nanoseconds), so a video
 * renderer shows the frame whose pts matches the clock. Gaps and jumps in the
 * timestamps move the clock with them; small jitter (under 15 ms) is ignored.
 *
 * Pushing never blocks. Use fa_stream_get_buffered to keep about a second queued
 * (the demuxer reads ahead until then). While the queue runs dry before
 * fa_stream_push_end, the stream is FA_STATE_BUFFERING (FA_EVENT_STALLED once audio
 * had played), and it resumes by itself (FA_EVENT_RESUMED) when ~100 ms are queued.
 * Push streams cannot be seeked: seek your demuxer, call fa_stream_push_flush, and
 * push from the new position. */

#define FA_PTS_NONE INT64_MIN   /* "no timestamp": the same value as FFmpeg's AV_NOPTS_VALUE */

typedef struct fa_rational {    /* the same layout as FFmpeg's AVRational */
    int32_t num;
    int32_t den;
} fa_rational;

/* Describes a compressed audio track: the fields of FFmpeg's AVCodecParameters
 * that decoding needs, plus the track's time base. */
typedef struct fa_codec_params {
    uint32_t struct_size;          /* sizeof(fa_codec_params) */
    const char* codec;             /* FFmpeg codec name: avcodec_get_name(codecpar->codec_id),
                                      e.g. "aac", "ac3", "eac3", "mp3", "opus", "flac",
                                      "pcm_s16le". Copied */
    uint32_t sample_rate;          /* codecpar->sample_rate */
    uint32_t channels;             /* codecpar->ch_layout.nb_channels (FFmpeg 5.1+) or ->channels */
    const void* extradata;         /* codecpar->extradata (AAC config, Opus header, ...); copied */
    size_t extradata_size;
    int64_t bit_rate;              /* optional */
    uint32_t block_align;          /* codecpar->block_align (needed by WMA, ADPCM, PCM) */
    uint32_t bits_per_coded_sample;/* optional */
    fa_rational time_base;         /* the track's time base (AVStream->time_base): the unit
                                      of fa_packet.pts and .duration. {0, 0} = nanoseconds */
} fa_codec_params;

FA_API void FA_CALL fa_codec_params_init(fa_codec_params* params);

/* One compressed packet, as av_read_frame returns it. The data is copied. */
typedef struct fa_packet {
    const void* data;              /* AVPacket->data */
    size_t size;                   /* AVPacket->size */
    int64_t pts;                   /* AVPacket->pts in the track's time base, FA_PTS_NONE = right
                                      after the previous packet */
    int64_t duration;              /* AVPacket->duration, 0 = unknown */
} fa_packet;

/* One block of PCM, as an AVFrame holds it. The samples are copied. */
typedef struct fa_audio_frame {
    fa_audio_format format;        /* of this frame: sample format (planar or interleaved),
                                      rate and channels. They may change from frame to
                                      frame; FatzAudio converts to the stream's format */
    const void* const* data;       /* planar: one pointer per channel (AVFrame->extended_data);
                                      interleaved: data[0] */
    fa_frames frames;              /* samples per channel (AVFrame->nb_samples) */
    int64_t pts;                   /* in time_base units, FA_PTS_NONE = right after the previous frame */
    fa_rational time_base;         /* {0, 0} = pts in nanoseconds */
} fa_audio_frame;

/* A push stream for PCM. `format` is the stream's own format (fa_stream_get_info):
 * frames in any other format are converted to it. The sample format part only
 * matters to fa_stream_push_pcm. */
FA_API fa_result FA_CALL fa_stream_open_push(fa_engine* engine, const fa_audio_format* format, const fa_open_options* options, fa_stream** out_stream);
/* PCM in the format given to fa_stream_open_push. planes: one per channel for
 * planar formats, planes[0] for interleaved ones. pts in nanoseconds or FA_PTS_NONE. */
FA_API fa_result FA_CALL fa_stream_push_pcm(fa_stream* stream, const void* const* planes, fa_frames frames, fa_time pts);
/* PCM in its own format (decoded AVFrames). Push streams only. */
FA_API fa_result FA_CALL fa_stream_push_frame(fa_stream* stream, const fa_audio_frame* frame);

/* A push stream for compressed packets of one audio track. Returns
 * FA_ERROR_PROVIDER_MISSING when the codec needs FFmpeg and none could be loaded
 * (see fa_ffmpeg_load), FA_ERROR_UNSUPPORTED_FORMAT when FFmpeg has no decoder for it.
 * The stream's format is the track's rate and channel count. */
FA_API fa_result FA_CALL fa_stream_open_packets(fa_engine* engine, const fa_codec_params* params, const fa_open_options* options, fa_stream** out_stream);
/* Queues a packet. A packet that fails to decode is skipped (FA_EVENT_DECODE_ERROR)
 * and playback continues. */
FA_API fa_result FA_CALL fa_stream_push_packet(fa_stream* stream, const fa_packet* packet);

/* No more data will come: the stream plays what is queued, then ends (FA_EVENT_ENDED).
 * fa_stream_push_flush makes it pushable again. */
FA_API fa_result FA_CALL fa_stream_push_end(fa_stream* stream);
/* Drops everything queued and decoded ahead, at once (the app seeked). The clock
 * holds where it was until audio of the new position plays. State is kept: a
 * playing stream goes on with whatever is pushed next. */
FA_API fa_result FA_CALL fa_stream_push_flush(fa_stream* stream);
/* How much pushed audio has not been heard yet (queued, decoded ahead, and in the
 * device buffer), as a duration. Packets count by their duration, or by the gap
 * to the next packet's pts. */
FA_API fa_result FA_CALL fa_stream_get_buffered(fa_stream* stream, fa_time* out_duration);

/* ---- FFmpeg at runtime ------------------------------------------------------
 * FatzAudio never links or bundles FFmpeg. When a packet stream needs a codec it
 * has no decoder of its own for, it uses the libavcodec and libavutil of your app:
 *   1. the ones already loaded in the process (your app's FFmpeg), else
 *   2. the ones the system loader finds (next to your executable, PATH, ...).
 * fa_ffmpeg_load(directory) loads them from a folder instead (call it first).
 * Supported: FFmpeg 4.0 to 8.x (libavcodec 58 to 62). */
typedef struct fa_ffmpeg_info {
    int32_t loaded;                /* 1 when FFmpeg is in use */
    uint32_t avcodec_version;      /* (major << 16) | (minor << 8) | micro, 0 if not loaded */
    uint32_t avutil_version;
    char avcodec_path[512];        /* where libavcodec was found (file name if unknown) */
} fa_ffmpeg_info;

/* directory NULL = steps 1 and 2 above. Returns FA_OK when FFmpeg is (already)
 * loaded, FA_ERROR_PROVIDER_MISSING if no usable libavcodec was found. Once
 * loaded, FFmpeg stays loaded for the life of the process. */
FA_API fa_result FA_CALL fa_ffmpeg_load(const char* directory);
FA_API fa_result FA_CALL fa_ffmpeg_get_info(fa_ffmpeg_info* out_info);

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

/* Routes a stream to a device, through `bus` (fa_bus_create) or straight into the
 * device's master mix (bus NULL). The decoder is not reopened: position, format
 * and parameters are kept. Attaching to the device it is already on only changes
 * the bus (instantly, without a gap); attaching to another device moves it there
 * (still keeping its position, and playing if it was), and a running ramp keeps
 * its remaining time.
 * Decode-only streams, devices of another engine, and a bus of another device
 * cannot be attached (FA_ERROR_INVALID_ARG). */
FA_API fa_result FA_CALL fa_stream_attach(fa_stream* stream, fa_device* device, fa_bus* bus);
/* Removes the stream from its device; it keeps the position heard so far. A
 * forward-only stream keeps the audio it had decoded ahead and plays it first on its
 * next attach, so nothing is skipped. FA_OK if not attached. */
FA_API fa_result FA_CALL fa_stream_detach(fa_stream* stream);
/* Starts or resumes an attached stream. A stopped or ended stream starts at its
 * FA_MARKER_CUE_IN (0 when unset); a paused one continues where it was. */
FA_API fa_result FA_CALL fa_stream_play(fa_stream* stream);
/* Like fa_stream_play, but the first frame is heard exactly at `device_time` on the
 * clock of the device the stream is attached to (fa_device_get_time). Until then
 * the state is FA_STATE_SCHEDULED; FA_EVENT_STARTED is raised when it starts.
 * A time already past starts it at once. Not attached: FA_ERROR_INVALID_ARG. */
FA_API fa_result FA_CALL fa_stream_play_at(fa_stream* stream, fa_time device_time);
/* Pause and stop take effect before they return: no further audio of the stream
 * is mixed after that, so the position read right after is final. Both also
 * cancel a scheduled or queued start. Stop rewinds to FA_MARKER_CUE_IN (or 0). */
FA_API fa_result FA_CALL fa_stream_pause(fa_stream* stream);
FA_API fa_result FA_CALL fa_stream_stop(fa_stream* stream);

typedef enum fa_stream_state {
    FA_STATE_STOPPED = 0, FA_STATE_PLAYING, FA_STATE_PAUSED, FA_STATE_BUFFERING, FA_STATE_ENDED,
    FA_STATE_SCHEDULED  /* waiting for its start: fa_stream_play_at, fa_crossfade or a queue */
} fa_stream_state;

FA_API fa_result FA_CALL fa_stream_get_state(fa_stream* stream, fa_stream_state* out_state);
/* Seeking a stopped stream makes it paused at the new position (stopped means "at
 * the start"). Seeking an attached stream drops the decode-ahead buffer and refills the first
 * ~100 ms on the calling thread before returning, so playback resumes at once. */
FA_API fa_result FA_CALL fa_stream_seek(fa_stream* stream, fa_frames frame);
/* Source frames already handed to the device (or read, for decode-only streams).
 * Frames decoded ahead into the buffer are not counted. */
FA_API fa_result FA_CALL fa_stream_get_position(fa_stream* stream, fa_frames* out_frame);

/* The presentation clock: the timestamp of the audio leaving the speaker right
 * now, in nanoseconds. Follow it with your video (or CD+G) renderer: show the
 * frame whose pts is closest to the clock, and the picture stays in sync by itself.
 *
 * - It includes the device latency (the device buffer and the limiter), so it is
 *   the time actually heard, not the time decoded or mixed.
 * - It moves smoothly between audio callbacks (interpolated from the device's
 *   sample clock), so it can be read every video frame.
 * - It stops when the stream pauses, stalls or ends (once the last audio has left
 *   the speaker), jumps to the target on a seek, and continues when playback does.
 * - File streams: the position in the file as time (0 = the start). Push and
 *   packet streams: in your own timeline, the pts you pushed.
 * Streams that are not attached report the time of their next frame. */
FA_API fa_result FA_CALL fa_stream_get_clock(fa_stream* stream, fa_time* out_pts);

/* Decode-only streams (FA_OPEN_DECODE_ONLY): pull interleaved F32 PCM in the
 * format reported by fa_stream_get_info. `buffer` must hold frames * channels floats.
 * Returns FA_OK with 1..frames frames read (short reads happen; keep calling), and
 * FA_ERROR_END_OF_STREAM with *out_frames_read == 0 once the end is reached.
 * Push and packet streams return FA_OK with 0 frames while nothing is queued, and
 * FA_ERROR_END_OF_STREAM after fa_stream_push_end once everything was read.
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
    FA_CURVE_EQUAL_POWER, /* sin rising, cos falling: out^2 + in^2 = 1 */
    FA_CURVE_LOG,         /* fast start, slow finish */
    FA_CURVE_EXP,         /* slow start, fast finish */
    FA_CURVE_SCURVE,      /* smoothstep */
    FA_CURVE_BEZIER,      /* uses bezier_x1/y1/x2/y2 control points, CSS cubic-bezier style */
    FA_CURVE_POINTS       /* your own shape: `points` are its values at evenly spaced
                             times from start to end, joined by straight lines */
} fa_curve;

#define FA_CURVE_MAX_POINTS 64

/* The shape of a ramp, fade or crossfade, plus a tag for its done event.
 * Zero-initialised it is a linear ramp with tag 0.
 * Shapes map progress 0..1 to 0..1; a ramp's value is start + (target - start) * shape.
 * FA_CURVE_POINTS: 2..FA_CURVE_MAX_POINTS values, normally from 0 to 1 (other values
 * are allowed, for example an overshoot). The array is copied by every call. */
typedef struct fa_ramp {
    fa_curve curve;
    float bezier_x1, bezier_y1, bezier_x2, bezier_y2;
    int64_t tag;            /* any value; returned in the FA_EVENT_RAMP_DONE event of this ramp */
    const float* points;    /* FA_CURVE_POINTS only */
    uint32_t point_count;
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
 * `device_time` 0 starts it now. Any other value starts it exactly at that time on
 * the clock of the stream's device (fa_device_get_time); the value holds until
 * then. If the stream is not playing at that moment, the ramp starts as soon as it
 * plays. A scheduled ramp on a stream that is not attached: FA_ERROR_INVALID_ARG. */
FA_API fa_result FA_CALL fa_stream_ramp_param(fa_stream* stream, fa_param param, float target, fa_time duration, fa_time device_time, const fa_ramp* ramp);

/* ---- Markers ---------------------------------------------------------- */

/* Markers are positions in the stream (source frames) that shape its playback.
 * All are optional; set one to FA_FRAMES_UNSET to clear it.
 *
 * CUE_IN          Where playback starts: fa_stream_play from stopped or ended,
 *                 fa_stream_stop, queues and crossfades all go here. Setting it on
 *                 a stopped stream moves the stream there. With FA_OPEN_LOOP the
 *                 stream loops from CUE_OUT back to CUE_IN.
 * CUE_OUT         Where playback ends: the stream ends there exactly (FA_EVENT_ENDED).
 * FADE_IN_END     Fades in from CUE_IN (or 0) to here, with the FA_FADE_IN shape.
 * FADE_OUT_START  Fades out from here to CUE_OUT (or the end), with the FA_FADE_OUT shape.
 * MIX_POINT       Where the stream queued after this one starts (fa_stream_set_next).
 *
 * Marker fades depend only on the position, so seeking into a fade plays it at the
 * right level, and they multiply with volume, crossfades, normalization and buses.
 * FA_EVENT_MARKER (value = the fa_marker) is raised when playback passes a marker.
 * Changing CUE_OUT while playing takes effect at once (the stream re-buffers from
 * the current position when audio past the new CUE_OUT was already decoded). */
typedef enum fa_marker {
    FA_MARKER_CUE_IN = 0,
    FA_MARKER_CUE_OUT,
    FA_MARKER_FADE_IN_END,
    FA_MARKER_FADE_OUT_START,
    FA_MARKER_MIX_POINT
} fa_marker;

/* "No marker": set a marker to FA_FRAMES_UNSET to clear it; get returns it for
 * markers that were never set. Any other negative frame is FA_ERROR_INVALID_ARG. */
#define FA_FRAMES_UNSET ((fa_frames)-1)

FA_API fa_result FA_CALL fa_stream_set_marker(fa_stream* stream, fa_marker marker, fa_frames frame);
FA_API fa_result FA_CALL fa_stream_get_marker(fa_stream* stream, fa_marker marker, fa_frames* out_frame);

typedef enum fa_fade { FA_FADE_IN = 0, FA_FADE_OUT } fa_fade;

/* Shape of the marker fades (the tag is ignored). The default for both is
 * FA_CURVE_EQUAL_POWER, so two overlapping marker fades keep constant loudness.
 * The fade-out is the fade-in shape played backwards (gain = shape(1 - t)). */
FA_API fa_result FA_CALL fa_stream_set_fade_curve(fa_stream* stream, fa_fade fade, const fa_ramp* shape);
/* points of a FA_CURVE_POINTS shape are not returned (points NULL, point_count set). */
FA_API fa_result FA_CALL fa_stream_get_fade_curve(fa_stream* stream, fa_fade fade, fa_ramp* out_shape);

/* ---- Crossfades and queues -------------------------------------------- */

/* Crossfades now: `to` starts (from CUE_IN if stopped or ended, else where it is)
 * and fades in while `from` fades out, both starting on the same frame, over
 * `duration`, with the shape in `ramp` (NULL = equal power). `from`'s gain is the
 * shape played backwards, so the pair is symmetric (equal power stays at constant
 * loudness). When the fade-out is done `from` ends (FA_STATE_ENDED, FA_EVENT_ENDED;
 * fa_stream_play starts it again from CUE_IN). Both raise FA_EVENT_RAMP_DONE with
 * ramp->tag when their fade is done.
 * `from` must be attached; `to` is attached to `from`'s device and bus if it is
 * not attached yet (another device: FA_ERROR_INVALID_ARG). The fades are crossfade
 * gains of their own, separate from FA_PARAM_VOLUME, so a user's volume setting is
 * kept. The start is one device period ahead (about 10 ms), so both streams are
 * guaranteed to begin on the same frame. */
FA_API fa_result FA_CALL fa_crossfade(fa_stream* from, fa_stream* to, fa_time duration, const fa_ramp* ramp);

/* Queues `next` to start automatically, sample-accurately, when `stream` reaches
 * its mix point:
 *   - its FA_MARKER_MIX_POINT, if set;
 *   - else `crossfade` before its end (CUE_OUT, or the length), if crossfade > 0
 *     and the end is known;
 *   - else right after its last frame: gapless playback.
 * With crossfade > 0, `stream` fades out over `crossfade` from the mix point (and
 * ends when the fade is done) unless it has a FADE_OUT_START marker, and `next`
 * fades in over `crossfade` unless it has a FADE_IN_END marker; markers win.
 * `shape` is the crossfade shape (NULL = equal power, tag ignored).
 * `next` starts at its CUE_IN (or where it is, if paused) and raises
 * FA_EVENT_STARTED. It is attached to `stream`'s device and bus if needed, and
 * reports FA_STATE_SCHEDULED while it waits. It may have a next of its own, so a
 * playlist can be queued as a chain. If `stream` has already ended, `next` starts
 * at once. next NULL removes the queued stream (it goes back to stopped).
 * `stream` must be attached; `next` must not be playing (FA_ERROR_BUSY) or on
 * another device (FA_ERROR_INVALID_ARG). Setting a new next replaces the old one. */
FA_API fa_result FA_CALL fa_stream_set_next(fa_stream* stream, fa_stream* next, fa_time crossfade, const fa_ramp* shape);
/* *out_next = the queued stream, or NULL (none, or it has started already). */
FA_API fa_result FA_CALL fa_stream_get_next(fa_stream* stream, fa_stream** out_next);

/* ---- Mix buses and the channel matrix ---------------------------------- */

/* A bus is a group of streams on one device with its own volume and pan: decks,
 * groups, a cue/preview channel. Stream gains, then bus gains, then the device
 * limiter. A DJ crossfader is two buses whose volumes are ramped or set in
 * opposite directions:
 *
 *     deck A -> bus 1 -+
 *                      +-> device master -> limiter -> speakers
 *     deck B -> bus 2 -+
 *
 * Bus volume (linear >= 0) and pan (-1..1) take fa_param values like streams, with
 * the same 5 ms smoothing, curves and scheduled starts; bus ramps run whenever the
 * device runs, and raise no events. Route a stream with fa_stream_attach(stream,
 * device, bus). Buses are released by fa_bus_destroy or fa_device_close; streams
 * still on a destroyed bus continue on the device's master mix. */
FA_API fa_result FA_CALL fa_bus_create(fa_device* device, fa_bus** out_bus);
FA_API void FA_CALL fa_bus_destroy(fa_bus* bus);
FA_API fa_result FA_CALL fa_bus_set_param(fa_bus* bus, fa_param param, float value);
FA_API fa_result FA_CALL fa_bus_get_param(fa_bus* bus, fa_param param, float* out_value);
FA_API fa_result FA_CALL fa_bus_ramp_param(fa_bus* bus, fa_param param, float target, fa_time duration, fa_time device_time, const fa_ramp* ramp);

/* Routes the stream's source channels to the device's channels with your own
 * gains: weights[in * out_channels + out] is the gain from source channel `in` to
 * device channel `out`. in_channels must be the source's channel count and
 * out_channels the device's (2). Use it for custom downmixes, swapping or muting
 * channels, or sending one channel of a karaoke track to both speakers.
 * weights NULL restores the default mapping. Takes effect at once (an attached
 * stream re-buffers from where it is). An attached forward-only source cannot
 * re-buffer: FA_ERROR_NOT_SEEKABLE (set the matrix before attaching it). */
FA_API fa_result FA_CALL fa_stream_set_matrix(fa_stream* stream, const float* weights, uint32_t in_channels, uint32_t out_channels);

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
 * FA_EVENT_STARTED         position               0
 *
 * Delivered today: ENDED, MARKER, LOUDNESS_FINAL, RAMP_DONE, STARTED, and STALLED, RESUMED and
 * DECODE_ERROR from push and packet streams. The others are raised by the features
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
    FA_EVENT_RAMP_DONE,       /* a ramp reached its target or was replaced */
    FA_EVENT_STARTED          /* a scheduled, crossfaded or queued start happened */
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
