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

/* The one version number of the library, CMake project and .NET package (see
 * CHANGELOG.md: before 1.0 a minor bump may break the API). */
#define FA_VERSION_MAJOR 0
#define FA_VERSION_MINOR 2
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
    FA_ERROR_PROVIDER_MISSING = -13, /* e.g. LAME or libcurl not found at runtime */
    FA_ERROR_NO_DISC = -14,          /* the CD drive has no disc, its tray is open, or the
                                        disc was changed under an open stream */
    FA_ERROR_NO_SOUNDFONT = -15,     /* a MIDI file needs a soundfont: fa_engine_set_soundfont
                                        or fa_open_options.soundfont */
    FA_ERROR_PROTECTED_CONTENT = -16, /* DRM-protected or encrypted content (WMA DRM,
                                        FairPlay, encrypted MP4/MKV, SAMPLE-AES HLS) */
    FA_ERROR_TOO_LARGE = -17         /* a compressed ZIP entry is bigger than
                                        fa_engine_config.zip_memory_limit_mb and no
                                        temp_directory is set: extract it first */
};

typedef struct fa_engine fa_engine;   /* one per process is typical */
typedef struct fa_device fa_device;   /* an opened output device */
typedef struct fa_stream fa_stream;   /* any playable/decodable source */
typedef struct fa_bus fa_bus;         /* a mix bus (deck/group) */
typedef struct fa_soundfont fa_soundfont; /* SoundFont 2 instruments for MIDI files */
typedef struct fa_tags fa_tags;       /* the tags and pictures of a file */

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
    FA_ENGINE_NULL_OUTPUT = 1 << 0,
    /* MIDI files opened without a soundfont fail with FA_ERROR_NO_SOUNDFONT instead
     * of playing with the OS's instruments (fa_soundfont_open_system). */
    FA_ENGINE_NO_SYSTEM_SOUNDFONT = 1 << 1
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
    /* ZIP archives (MP3+G). Stored entries always play straight from the archive.
     * Compressed ones are unpacked into memory up to zip_memory_limit_mb (0 = 256).
     * A bigger one is unpacked into temp_directory when it is set (a "fatzaudio-tmp"
     * folder inside it, created if missing; the file is deleted when the stream
     * closes, also if the app crashes), else the open fails with FA_ERROR_TOO_LARGE.
     * The directory is created if missing (one level); FA_ERROR_IO when a song needs
     * it and it cannot be written. The string is copied. */
    const char* temp_directory;   /* NULL = never write temp files */
    uint32_t zip_memory_limit_mb;
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
/* Mutes everything the device plays, keeping its volume; glides over 5 ms. An
 * encoder recording the device (fa_encoder_attach_device) records the silence too. */
FA_API fa_result FA_CALL fa_device_set_mute(fa_device* device, int32_t muted);
FA_API fa_result FA_CALL fa_device_get_mute(fa_device* device, int32_t* out_muted);
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
 *        A ZIP archive (MP3+G) needs both seek and length: its directory is at
 *        the end (FA_ERROR_NOT_SEEKABLE otherwise).
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
    FA_OPEN_LOOP        = 1 << 1,  /* loop from CUE_OUT back to CUE_IN; fa_stream_set_loop
                                      turns it on or off later */
    FA_OPEN_PRESCAN     = 1 << 2,  /* build exact seek index / length before returning */
    FA_OPEN_NO_NORMALIZE = 1 << 3, /* opt this stream out of engine normalization
                                      (jingles, sound effects, already-mastered mixes) */
    FA_OPEN_NO_CDG       = 1 << 4  /* no CD+G graphics: fa_stream_open_path does not look
                                      for a matching .cdg next to the audio, and ZIP
                                      archives play without theirs (see "CD+G") */
} fa_open_flags;

typedef struct fa_net_options fa_net_options;

/* Dynamic range of Dolby Digital (AC-3) and Dolby Digital Plus (E-AC-3) audio.
 * These streams carry the broadcaster's compression gains for every block; the
 * decoder can apply them or play the mix as it was mastered. Other formats are
 * not affected. */
typedef enum fa_dynamic_range {
    FA_DYNAMIC_RANGE_FULL = 0,  /* default: the full mix, no compression */
    FA_DYNAMIC_RANGE_NORMAL,    /* the stream's compression ("line mode"): quiet parts
                                   lifted and loud ones tamed, as a home AV receiver does */
    FA_DYNAMIC_RANGE_NIGHT      /* heavy compression for listening at low volume ("RF
                                   mode"); streams without heavy gains use NORMAL's */
} fa_dynamic_range;

typedef struct fa_open_options {
    uint32_t struct_size;
    uint32_t flags;          /* fa_open_flags */
    int32_t audio_track;     /* for containers with several audio tracks, -1 = default;
                                FA_ERROR_NOT_FOUND if the track does not exist */
    const char* mime_hint;   /* optional, e.g. "audio/flac"; only used when the content
                                itself is ambiguous. Content sniffing always wins */
    uint32_t buffer_ms;      /* decode-ahead buffer while attached to a device,
                                0 = default (500 ms), clamped to 50..10000 */
    const fa_net_options* net; /* fa_stream_open_url only: connection settings,
                                NULL = defaults. Copied by the call */
    int32_t dynamic_range;   /* fa_dynamic_range for Dolby audio (default FULL); can be
                                changed later with fa_stream_set_dynamic_range */
    fa_soundfont* soundfont; /* MIDI files: the instruments to play them with, NULL = the
                                engine's (fa_engine_set_soundfont). The stream keeps its
                                own reference. Other formats ignore it */
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
/* Web streams: see "Web streams" below. */
FA_API fa_result FA_CALL fa_stream_open_url(fa_engine* engine, const char* url, const fa_open_options* options, fa_stream** out_stream);

/* ---- Push streams and the FFmpeg hand-off --------------------------------
 *
 * For apps that already read the file themselves, typically with FFmpeg for the
 * video. Instead of decoding audio to PCM, converting it and keeping it in sync by
 * hand, hand FatzAudio what you already have and let the video follow its clock:
 *
 *   compressed packets   fa_stream_open_packets + fa_stream_push_packet: straight
 *                        from av_read_frame, no audio decoding in the app at all.
 *                        FatzAudio decodes them on its worker threads (the codecs
 *                        at fa_stream_open_packets). It never uses FFmpeg itself.
 *   decoded frames       fa_stream_open_push + fa_stream_push_frame: AVFrame data
 *                        as it is (planar or interleaved, any sample format, any
 *                        rate), no swresample step. The way to play codecs FatzAudio
 *                        does not decode (DTS, TrueHD, ...) with your own FFmpeg.
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

/* A push stream for compressed packets of one audio track, decoded by the library:
 * PCM, MP3, AAC, AC-3, E-AC-3, Opus, Vorbis, FLAC, ALAC, and WMA on Windows.
 * FA_ERROR_UNSUPPORTED_FORMAT for any other codec (DTS, TrueHD, ...): decode those
 * with your own decoder and push the frames (fa_stream_open_push).
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
 * device buffer), as a duration. Counted from durations, never timestamps, so a
 * jump in the timeline adds nothing. A queued packet without a duration counts
 * like the one before it until it is decoded. */
FA_API fa_result FA_CALL fa_stream_get_buffered(fa_stream* stream, fa_time* out_duration);

FA_API void FA_CALL fa_stream_close(fa_stream* stream);

/* ---- Web streams --------------------------------------------------------
 *
 * fa_stream_open_url plays what is behind an http:// or https:// URL:
 *   - audio files on a web server: seekable when the server supports byte ranges
 *     (almost all do); only what is played is downloaded;
 *   - Icecast / Shoutcast radio: the song titles arrive as FA_EVENT_METADATA at
 *     the moment the new song is heard, not when it was downloaded;
 *   - HLS playlists (.m3u8), live or on demand, with MPEG-TS, fMP4 or plain audio
 *     segments and AES-128 encryption. On-demand HLS is seekable.
 * The format of the audio is detected from the content, as for files.
 *
 * Opening connects and waits for the first few KB (format detection), at most
 * timeout_ms. All further network I/O runs on a download thread of the stream:
 * decoding only starts on data that is already downloaded, so the audio thread
 * and the decode workers never wait for the network.
 *
 * Playback starts once prebuffer_ms of audio are buffered. When the network
 * cannot keep up, the stream goes to FA_STATE_BUFFERING (FA_EVENT_STALLED), and
 * back to playing (FA_EVENT_RESUMED) once prebuffer_ms are buffered again.
 * A dropped connection is re-established by itself for reconnect_ms: a file
 * continues at the byte where it stopped, radio picks up the live stream.
 * When reconnecting fails for that long, the stream plays what it has and ends.
 *
 * HTTPS uses the system's TLS stack (WinHTTP on Windows, URLSession on Apple
 * platforms, HttpURLConnection on Android, libcurl on Linux, loaded at runtime;
 * FA_ERROR_PROVIDER_MISSING if the system has none). Plain http:// uses
 * FatzAudio's own HTTP/1.1 client. */

typedef enum fa_net_flags {
    FA_NET_NO_RECONNECT = 1 << 0,  /* end the stream when the connection drops */
    FA_NET_NO_ICY       = 1 << 1   /* do not ask radio servers for song titles */
} fa_net_flags;

struct fa_net_options {
    uint32_t struct_size;          /* sizeof(fa_net_options) */
    const char* user_agent;        /* NULL = "FatzAudio/<version>" */
    const char* headers;           /* extra request headers, "Name: value" lines separated
                                      by "\r\n" or "\n" (cookies, authorization, ...); NULL = none */
    const char* proxy;             /* NULL = the system's setting (environment http_proxy /
                                      https_proxy, Windows and macOS proxy settings);
                                      "" = connect directly; "host:port" or
                                      "http://user:password@host:port" */
    uint32_t timeout_ms;           /* connecting, and the longest the server may stay
                                      silent; 0 = 10000 */
    uint32_t prebuffer_ms;         /* audio buffered before playback starts, and before it
                                      resumes after a stall; 0 = 1000 */
    uint32_t reconnect_ms;         /* how long to keep reconnecting after the connection
                                      drops; 0 = 30000 (FA_NET_NO_RECONNECT: never) */
    uint32_t flags;                /* fa_net_flags */
    uint32_t hls_max_bandwidth;    /* HLS: plays the best variant up to this many bits per
                                      second; 0 = the best one */
};

FA_API void FA_CALL fa_net_options_init(fa_net_options* options);

/* Android: HTTPS runs on the app's Java VM (java.net.HttpURLConnection, through
 * JNI). FatzAudio finds the VM by itself on Android 12 (API 31) and later; on
 * older versions, call this once with the app's JavaVM* (JNI_OnLoad's vm, or
 * JNIEnv's GetJavaVM) before opening https:// URLs. The .NET binding does it for
 * you. FA_ERROR_INVALID_ARG for NULL; FA_ERROR_NOT_IMPLEMENTED off Android. */
FA_API fa_result FA_CALL fa_android_set_java_vm(void* java_vm);

/* What a web stream is playing. Every string is UTF-8 and "" when unknown. Streams
 * that are not web streams return FA_OK with everything empty. */
typedef struct fa_metadata {
    char stream_title[1024];       /* the song heard now, as the station sends it
                                      (ICY StreamTitle), usually "Artist - Title" */
    char artist[256];              /* stream_title split at its first " - " */
    char title[768];               /* the rest; all of stream_title when there is no " - " */
    char url[512];                 /* ICY StreamUrl (often album art or the station's page) */
    char station[256];             /* icy-name */
    char genre[128];               /* icy-genre */
    char description[256];         /* icy-description */
    char station_url[512];         /* icy-url */
    char content_type[128];        /* of the audio, e.g. "audio/mpeg" */
    uint32_t bitrate_kbps;         /* icy-br, 0 = unknown */
    int64_t seq;                   /* 1, 2, ... for each title heard; 0 = none yet. The value
                                      of the matching FA_EVENT_METADATA */
} fa_metadata;

/* The metadata of what is heard right now: a title that is downloaded but still
 * in the buffer is not returned until playback reaches it. */
FA_API fa_result FA_CALL fa_stream_get_metadata(fa_stream* stream, fa_metadata* out_metadata);

typedef struct fa_net_status {
    int32_t connected;             /* 1 while a connection is open */
    int32_t http_status;           /* status code of the last response, 0 before one */
    int32_t is_live;               /* 1 = no known end (radio, live HLS) */
    int32_t is_hls;                /* 1 = an HLS playlist */
    int32_t seekable;              /* 1 = fa_stream_seek works */
    int32_t finished;              /* 1 = everything is downloaded, or reconnecting gave up */
    int64_t content_length;        /* bytes of the file, -1 = unknown or live */
    int64_t downloaded;            /* bytes received, over all connections */
    int64_t buffered_bytes;        /* downloaded and not decoded yet */
    fa_time buffered;              /* audio ready ahead of playback (download buffer and
                                      decode-ahead), estimated from the bitrate */
    uint32_t reconnects;           /* connections re-established after a drop */
    fa_result last_error;          /* why the last connection failed, FA_OK if none did */
    char url[2048];                /* the URL in use after redirects (HLS: the media playlist) */
} fa_net_status;

/* FA_ERROR_INVALID_ARG for streams that are not web streams. */
FA_API fa_result FA_CALL fa_stream_get_net_status(fa_stream* stream, fa_net_status* out_status);
/* A header of the last HTTP response, e.g. "Content-Type" or "icy-name" (any case).
 * FA_ERROR_NOT_FOUND when the response had no such header; truncated to fit. */
FA_API fa_result FA_CALL fa_stream_get_http_header(fa_stream* stream, const char* name, char* out_value, size_t value_size);

/* ---- Audio CDs ------------------------------------------------------------
 *
 * Audio CDs play like any other source: open the drive with fa_disc_open, read
 * its table of contents, and open a track with fa_stream_open_disc. The stream
 * is an ordinary seekable stream, so crossfades, queues (gapless track to track),
 * markers, normalization and buses all work on it unchanged.
 *
 * Supported on Windows, macOS and Linux. iOS and Android have no optical drives
 * (fa_disc_drive_count reports 0 there), but disc images still open everywhere.
 *
 * A disc image (a .cue sheet with its .bin files) opens exactly like a drive:
 * pass the path of the .cue file to fa_disc_open.
 *
 * Reading is error tolerant: sectors the drive flags as damaged (C2 error
 * pointers, when the drive reports them) are read again up to `retries` times;
 * a sector that stays unreadable plays as silence and raises FA_EVENT_DECODE_ERROR
 * (value FA_ERROR_IO) instead of stopping playback. Tracks recorded with
 * pre-emphasis are de-emphasized automatically.
 *
 * Drive access: on Linux the user needs read and write access to the drive's
 * device node (usually membership of the "cdrom" group); on Windows no
 * administrator rights are needed. */

typedef struct fa_disc fa_disc;   /* an opened CD drive or disc image */

typedef struct fa_disc_drive_info {
    char id[256];        /* pass to fa_disc_open: "D:" on Windows, "/dev/sr0" on Linux,
                            "disk2" on macOS */
    char name[128];      /* "vendor product", e.g. "ASUS DRW-24D5MT" */
    char vendor[16];
    char product[32];
    char revision[8];
} fa_disc_drive_info;

/* The optical drives of this computer. On macOS only drives with an audio CD in
 * them are listed. The list is built when fa_disc_drive_count is called. */
FA_API fa_result FA_CALL fa_disc_drive_count(uint32_t* out_count);
FA_API fa_result FA_CALL fa_disc_drive_get_info(uint32_t index, fa_disc_drive_info* out_info);

typedef enum fa_disc_flags {
    FA_DISC_NO_DEEMPHASIS = 1 << 0,  /* play pre-emphasized tracks as recorded */
    FA_DISC_NO_C2         = 1 << 1,  /* do not ask the drive for C2 error pointers
                                        (for drives that report them wrongly) */
    FA_DISC_NO_CDG        = 1 << 2   /* do not read the subcode, so tracks get no CD+G
                                        graphics (for drives that read it slowly) */
} fa_disc_flags;

typedef struct fa_disc_options {
    uint32_t struct_size;  /* sizeof(fa_disc_options) */
    int32_t read_offset;   /* the drive's read offset correction in samples, as listed in
                              the AccurateRip drive database (e.g. +6); 0 = none */
    uint32_t retries;      /* re-reads of a damaged sector before it plays as silence;
                              fa_disc_options_init sets 5 */
    uint32_t flags;        /* fa_disc_flags */
} fa_disc_options;

FA_API void FA_CALL fa_disc_options_init(fa_disc_options* options);

/* drive_id: an id from fa_disc_drive_get_info, the path of a .cue disc image, or
 * NULL for the first drive. options may be NULL (defaults). A drive opens with or
 * without a disc in it. FA_ERROR_NOT_FOUND when there is no such drive,
 * FA_ERROR_DEVICE when it cannot be opened (no permission). */
FA_API fa_result FA_CALL fa_disc_open(const char* drive_id, const fa_disc_options* options, fa_disc** out_disc);
/* Streams opened from the disc keep working until they are closed. */
FA_API void FA_CALL fa_disc_close(fa_disc* disc);

typedef enum fa_disc_state {
    FA_DISC_READY = 0,       /* a disc is in and readable */
    FA_DISC_NO_DISC,         /* tray closed, no disc */
    FA_DISC_TRAY_OPEN,
    FA_DISC_SPINNING_UP      /* a disc was just inserted; ready in a moment */
} fa_disc_state;

typedef struct fa_disc_status {
    fa_disc_state state;
    int32_t locked;          /* 1 = the eject button is disabled by fa_disc_set_locked */
    uint32_t change_count;   /* goes up by one each time a disc is removed or inserted,
                                as seen by fa_disc_get_status: poll it (every second or
                                so) to notice a new disc */
} fa_disc_status;

/* Asks the drive now; takes a few milliseconds. */
FA_API fa_result FA_CALL fa_disc_get_status(fa_disc* disc, fa_disc_status* out_status);

#define FA_DISC_MAX_TRACKS 99
#define FA_DISC_SECTOR_FRAMES 588   /* frames per sector; 75 sectors per second */

typedef struct fa_disc_track {
    int32_t number;          /* 1..99, as printed on the cover */
    int32_t is_audio;        /* 0 = a data track (CD-ROM part of an Enhanced CD): not playable */
    int32_t preemphasis;     /* 1 = recorded with pre-emphasis (de-emphasized on playback) */
    int32_t copy_permitted;
    int32_t four_channel;    /* 1 = a four-channel audio track (very rare; not playable) */
    int64_t start_sector;    /* where the track starts (its index 1), in sectors from the
                                start of the disc's program area */
    int64_t sectors;         /* length; the pregap of the next track is not included */
    fa_frames frames;        /* sectors * 588, the length of its stream */
    fa_time duration;
} fa_disc_track;

typedef struct fa_disc_toc {
    int32_t first_track;     /* number of the first track, usually 1 */
    int32_t last_track;
    int32_t track_count;     /* entries used in tracks[] */
    int32_t audio_track_count;
    int64_t leadout_sector;  /* end of the last session's program area */
    fa_time duration;        /* of all audio tracks */
    uint32_t freedb_id;      /* the freedb/CDDB disc id, e.g. 0x8a09ec0c */
    char freedb_query[1024]; /* "8a09ec0c 12 150 22767 ... 3631", the arguments of a CDDB
                                "cddb query" command */
    char musicbrainz_id[32]; /* the MusicBrainz disc id (28 characters) for
                                https://musicbrainz.org/cdtoc/<id> */
    int32_t has_cd_text;     /* 1 = fa_disc_get_text has titles from the disc */
    fa_disc_track tracks[FA_DISC_MAX_TRACKS]; /* tracks[i] is track first_track + i */
} fa_disc_toc;

/* The disc's table of contents. FA_ERROR_NO_DISC when no readable disc is in the
 * drive. Read once per disc and kept, so calling it again is free. */
FA_API fa_result FA_CALL fa_disc_get_toc(fa_disc* disc, fa_disc_toc* out_toc);

typedef enum fa_disc_text {
    FA_DISC_TEXT_TITLE = 0,
    FA_DISC_TEXT_PERFORMER,
    FA_DISC_TEXT_SONGWRITER,
    FA_DISC_TEXT_COMPOSER,
    FA_DISC_TEXT_ARRANGER,
    FA_DISC_TEXT_MESSAGE,
    FA_DISC_TEXT_GENRE,      /* album only */
    FA_DISC_TEXT_CODE        /* album: UPC/EAN (media catalog number); track: ISRC. Read from
                                the subcode when CD-Text has none (a short disc access) */
} fa_disc_text;

/* CD-Text of the album (track 0) or a track (its number), as UTF-8.
 * FA_ERROR_NOT_FOUND when the disc has no such text; truncated to fit. */
FA_API fa_result FA_CALL fa_disc_get_text(fa_disc* disc, int32_t track, fa_disc_text field, char* out_text, size_t text_size);

/* Opens the tray (unlocking it first) and closes it. Closing is not possible on
 * slot-loading and laptop drives (FA_ERROR_NOT_IMPLEMENTED or FA_ERROR_DEVICE). */
FA_API fa_result FA_CALL fa_disc_eject(fa_disc* disc);
FA_API fa_result FA_CALL fa_disc_load(fa_disc* disc);
/* 1 disables the drive's eject button until 0 or fa_disc_close. */
FA_API fa_result FA_CALL fa_disc_set_locked(fa_disc* disc, int32_t locked);
/* Read speed as a multiple of normal playback speed (1 = 176.4 kB/s); 0 = the
 * fastest. Slower is quieter and often reads damaged discs better. The drive
 * picks the nearest speed it supports; fa_disc_get_speed reports it. */
FA_API fa_result FA_CALL fa_disc_set_speed(fa_disc* disc, float speed);
FA_API fa_result FA_CALL fa_disc_get_speed(fa_disc* disc, float* out_speed);

/* A stream of one audio track (its number, as in fa_disc_toc). FA_ERROR_NOT_FOUND
 * when the disc has no such track, FA_ERROR_UNSUPPORTED_FORMAT for a data or
 * four-channel track, FA_ERROR_NO_DISC without a disc. The stream is 44.1 kHz
 * stereo, seekable, codec "cdda", container "cd". If the disc is taken out while
 * it plays, the stream ends (its last read fails with FA_ERROR_NO_DISC).
 * Normalization uses the running estimate: a disc is not read twice for a scan.
 * On CD+G discs the graphics are read with the audio and attached to the stream
 * as they are found (see CD+G below; FA_DISC_NO_CDG turns this off).
 *
 * fa_stream_open_path also opens Windows' TrackNN.cda files (D:\Track01.cda)
 * as the track they stand for. */
FA_API fa_result FA_CALL fa_stream_open_disc(fa_engine* engine, fa_disc* disc, int32_t track, const fa_open_options* options, fa_stream** out_stream);

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
    int64_t file_size;           /* bytes of the file (of the ZIP for an MP3+G archive),
                                    -1 if unknown (live web streams, app I/O without a size) */
    int64_t audio_bytes;         /* bytes of this audio stream's coded data, without tags,
                                    headers and other tracks; -1 if unknown. Exact for
                                    WAV/AIFF/CD and MP4, close for MP3, AAC, AC-3, FLAC,
                                    Ogg and single-track Matroska */
    uint32_t bitrate;            /* average bits per second: audio_bytes over the duration,
                                    else the codec's nominal rate; 0 if unknown */
    uint32_t bits_per_sample;    /* PCM and lossless sources (WAV, AIFF, FLAC, ALAC, CD):
                                    16, 24, ...; 0 for lossy codecs */
} fa_stream_info;

FA_API fa_result FA_CALL fa_stream_get_info(fa_stream* stream, fa_stream_info* out_info);

/* Dolby dynamic range (see fa_dynamic_range): applies to audio decoded from now on,
 * so a playing stream changes after its decode-ahead buffer (buffer_ms, 500 ms by
 * default). Streams of other formats accept and ignore it. */
FA_API fa_result FA_CALL fa_stream_set_dynamic_range(fa_stream* stream, fa_dynamic_range mode);
FA_API fa_result FA_CALL fa_stream_get_dynamic_range(fa_stream* stream, fa_dynamic_range* out_mode);

/* ---- Tags and cover art ------------------------------------------------
 *
 * Title, artist, album and the rest, plus embedded pictures, from every tag
 * format the library meets: ID3v2.2/2.3/2.4 and ID3v1 (MP3, AAC, AC-3, WAV,
 * AIFF), APEv2, Vorbis comments (FLAC, Ogg Vorbis, Opus, Ogg FLAC), MP4/M4A
 * (iTunes atoms), ASF (WMA), RIFF INFO (WAV), AIFF text chunks and Matroska/WebM
 * tags and attachments. A ZIP (MP3+G) is read through the audio file inside it.
 *
 * Reading needs no engine: a media library gets the tags, the cover and the
 * file's properties (duration, bitrate, channels, ...) in one call.
 * FA_TAGS_NO_PICTURES skips the picture data without reading it, and
 * FA_TAGS_NO_INFO skips the properties (no decoder is opened). A tag
 * object is immutable: every string and picture it hands out stays valid until
 * fa_tags_close, and it may be read from any thread.
 *
 * Web streams have no file tags: their titles come from fa_stream_get_metadata.
 * MIDI streams return the title, artist and copyright of fa_stream_get_midi_info. */

typedef enum fa_tag_field {
    FA_TAG_TITLE = 0,
    FA_TAG_ARTIST,
    FA_TAG_ALBUM,
    FA_TAG_ALBUM_ARTIST,
    FA_TAG_COMPOSER,
    FA_TAG_GENRE,          /* ID3 and MP4 genre numbers are turned into names */
    FA_TAG_DATE,           /* as written: "2024", "2024-05-17", ... */
    FA_TAG_TRACK,          /* "3" (a "3/12" in the file is split into TRACK and TRACK_TOTAL) */
    FA_TAG_TRACK_TOTAL,
    FA_TAG_DISC,
    FA_TAG_DISC_TOTAL,
    FA_TAG_COMMENT,
    FA_TAG_LYRICS,         /* unsynchronised lyrics */
    FA_TAG_COPYRIGHT,
    FA_TAG_ENCODER,        /* the software that made the file */
    FA_TAG_BPM,
    FA_TAG_FIELD_COUNT
} fa_tag_field;

/* Where an item or picture was found. */
typedef enum fa_tag_format {
    FA_TAG_FORMAT_ID3V2 = 1,
    FA_TAG_FORMAT_ID3V1,
    FA_TAG_FORMAT_APE,
    FA_TAG_FORMAT_VORBIS,    /* Vorbis comments, FLAC pictures */
    FA_TAG_FORMAT_MP4,
    FA_TAG_FORMAT_ASF,
    FA_TAG_FORMAT_RIFF,      /* WAV LIST/INFO */
    FA_TAG_FORMAT_AIFF,      /* NAME, AUTH, (c), ANNO */
    FA_TAG_FORMAT_MATROSKA,
    FA_TAG_FORMAT_MIDI
} fa_tag_format;

typedef enum fa_tags_flags {
    FA_TAGS_NO_PICTURES = 1 << 0,    /* skip picture data (faster for library scans) */
    FA_TAGS_NO_INFO = 1 << 1         /* skip the properties (fa_tags_get_info): no decoder
                                        is opened, only the tags are read */
} fa_tags_flags;

/* One tag item exactly as the file stores it. `key` is the format's own name:
 * an ID3v2 frame ID ("TIT2"; user-defined TXXX items use their description,
 * e.g. "REPLAYGAIN_TRACK_GAIN"), a Vorbis/APE key ("ARTIST"), an MP4 atom
 * ("\xC2\xA9nam" = "©nam"; freeform "----" atoms use their name), an ASF
 * attribute ("WM/AlbumTitle"), a RIFF/AIFF chunk ID ("INAM") or a Matroska tag
 * name ("TITLE", "ALBUM/TITLE" for album-level tags). Text is UTF-8; numbers
 * (track numbers, BPM) are written as text. A list of values is joined with "; ". */
typedef struct fa_tag_item {
    const char* key;
    const char* value;
    int32_t format;          /* fa_tag_format */
} fa_tag_item;

/* Picture types: the ID3v2 / FLAC numbering (0..20), which MP4 and Matroska
 * covers are mapped to. */
typedef enum fa_picture_type {
    FA_PICTURE_OTHER = 0,
    FA_PICTURE_FILE_ICON = 1,
    FA_PICTURE_FRONT_COVER = 3,
    FA_PICTURE_BACK_COVER = 4,
    FA_PICTURE_LEAFLET = 5,
    FA_PICTURE_MEDIA = 6,
    FA_PICTURE_ARTIST = 8
} fa_picture_type;

typedef struct fa_tag_picture {
    int32_t type;            /* fa_picture_type (any ID3v2 value 0..20) */
    const char* mime;        /* "image/jpeg", "image/png", ...; "" when not stated
                                nor recognisable from the data */
    const char* description; /* UTF-8, "" if none */
    const void* data;        /* the encoded image, as stored */
    size_t size;
    int32_t format;          /* fa_tag_format */
} fa_tag_picture;

/* Reads the tags of a file (UTF-8 path) or of a file in memory (not copied:
 * `data` is only used during the call). A file without tags, or of a format
 * with none, gives an empty tag object and FA_OK. FA_ERROR_NOT_FOUND when the
 * file is missing. Flags: fa_tags_flags. */
FA_API fa_result FA_CALL fa_tags_read_path(const char* path, uint32_t flags, fa_tags** out_tags);
FA_API fa_result FA_CALL fa_tags_read_memory(const void* data, size_t size, uint32_t flags, fa_tags** out_tags);
/* The tags of the file a stream plays (opened from a path or memory). Streams
 * of the app's I/O callbacks, web streams and CD tracks give empty tags. */
FA_API fa_result FA_CALL fa_stream_get_tags(fa_stream* stream, uint32_t flags, fa_tags** out_tags);
FA_API void FA_CALL fa_tags_close(fa_tags* tags);

/* A common field, whichever format it came from: UTF-8, "" when absent (never
 * NULL). With several tag formats in one file the richest wins: ID3v2 before
 * APEv2 before ID3v1; in WAV and AIFF, ID3v2 before the native chunks. */
FA_API const char* FA_CALL fa_tags_get(const fa_tags* tags, fa_tag_field field);
/* Every item in file order, index 0 .. count - 1 (FA_ERROR_NOT_FOUND past it). */
FA_API uint32_t FA_CALL fa_tags_get_item_count(const fa_tags* tags);
FA_API fa_result FA_CALL fa_tags_get_item(const fa_tags* tags, uint32_t index, fa_tag_item* out_item);
/* Embedded pictures in file order (none with FA_TAGS_NO_PICTURES). */
FA_API uint32_t FA_CALL fa_tags_get_picture_count(const fa_tags* tags);
FA_API fa_result FA_CALL fa_tags_get_picture(const fa_tags* tags, uint32_t index, fa_tag_picture* out_picture);
/* The front cover, else the first picture: FA_ERROR_NOT_FOUND when there is none. */
FA_API fa_result FA_CALL fa_tags_get_cover(const fa_tags* tags, fa_tag_picture* out_picture);
/* The file's properties: codec, container, channels, sample rate, duration,
 * bitrate, file and audio size, bits per sample, has_cdg (a .cdg next to the file
 * or in its ZIP), as fa_stream_get_info gives them for a stream of the file.
 * FA_ERROR_NOT_FOUND when they were not read (FA_TAGS_NO_INFO, or no decoder
 * recognised the file: MIDI files need a soundfont, so give a MIDI stream's tags
 * with fa_stream_get_tags). */
FA_API fa_result FA_CALL fa_tags_get_info(const fa_tags* tags, fa_stream_info* out_info);

/* ---- Transport and position -------------------------------------------- */

/* Routes a stream to a device, through `bus` (fa_bus_create) or straight into the
 * device's master mix (bus NULL). The decoder is not reopened: position, format
 * and parameters are kept. Attaching to the device it is already on only changes
 * the bus (instantly, without a gap); attaching to another device moves it there
 * (still keeping its position, and playing if it was), and a running ramp keeps
 * its remaining time.
 * Decode-only streams, devices of another engine, and a bus of another device
 * cannot be attached (FA_ERROR_INVALID_ARG), nor a stream an encoder is reading
 * (fa_encoder_encode_stream: FA_ERROR_BUSY). */
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

/* Looping on or off at any time, also while playing (FA_OPEN_LOOP sets it at open).
 * A looping stream plays from CUE_OUT (or the end) back to CUE_IN (or 0) without a
 * gap and raises FA_EVENT_LOOPED each time it jumps back (like markers, raised as the
 * audio is mixed, one device latency before it is heard). Turning it off lets
 * the stream play on to CUE_OUT and end. Only seekable streams loop: push, packet
 * and forward-only web streams return FA_ERROR_NOT_SEEKABLE for on = 1. */
FA_API fa_result FA_CALL fa_stream_set_loop(fa_stream* stream, int32_t on);
FA_API fa_result FA_CALL fa_stream_get_loop(fa_stream* stream, int32_t* out_on);

/* Mute: silent without touching FA_PARAM_VOLUME, so unmuting brings back the volume
 * (and any fade or ramp, which keeps running while muted). It glides over 5 ms, like
 * a volume change, so it never clicks. The stream keeps playing and its position,
 * clock, markers and events carry on as usual. */
FA_API fa_result FA_CALL fa_stream_set_mute(fa_stream* stream, int32_t muted);
FA_API fa_result FA_CALL fa_stream_get_mute(fa_stream* stream, int32_t* out_muted);

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
 * Streams that are not attached report the time of their next frame.
 *
 * Cheap and safe from a render loop: for attached streams and file streams it takes
 * no lock and never waits for a seek, a worker or the audio thread, so it can be
 * called every vsync (240 Hz and more) from any thread. Push and packet streams
 * that are not attached take the stream's lock briefly. */
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
    FA_PARAM_RATE,       /* playback speed, pitch follows (varispeed, like a turntable):
                            0.1..10, default 1 */
    FA_PARAM_TEMPO,      /* speed with the pitch kept: 0.1..10, default 1 (1.25 = 25% faster) */
    FA_PARAM_PITCH       /* key change with the speed kept, in semitones: -36..36,
                            default 0 (fractions allowed: 0.5 = a quarter tone) */
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
 * and without zipper noise (changes are smoothed over 5 ms).
 *
 * RATE, TEMPO and PITCH change the audio itself, so they are applied where the
 * stream is decoded, on a worker thread: a change is heard after the decode-ahead
 * buffer, which is kept short while any of them is in use (fa_tempo_options
 * latency_ms, 150 ms by default). They combine: the speed heard is TEMPO x RATE and
 * the pitch PITCH semitones plus what RATE adds. Out-of-range values are clamped.
 * They work on every stream, also decode-only ones (fa_stream_read returns the
 * processed audio), except that push and packet streams (fa_stream_open_push,
 * fa_stream_open_packets) take PITCH only: their timing is the app's, so TEMPO and
 * RATE return FA_ERROR_NOT_IMPLEMENTED there. Positions, markers, the clock and
 * CD+G graphics stay in source frames and follow the tempo by themselves.
 * Buses have no RATE, TEMPO or PITCH (FA_ERROR_INVALID_ARG), and streams of more
 * than 32 channels without a channel matrix return FA_ERROR_UNSUPPORTED_FORMAT.
 *
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

/* How tempo and pitch are processed (see FA_PARAM_TEMPO). */
typedef enum fa_stretch_quality {
    FA_STRETCH_BEST = 0,   /* default: 120 ms analysis blocks, the cleanest sound */
    FA_STRETCH_FAST        /* 100 ms blocks, about a third less CPU, slightly less clean */
} fa_stretch_quality;

typedef struct fa_tempo_options {
    uint32_t struct_size;          /* set by fa_tempo_options_init */
    fa_stretch_quality quality;
    int32_t keep_formants;         /* 1 = a pitch change keeps the character of voices
                                      (karaoke key changes without the "chipmunk" sound);
                                      default 0 */
    uint32_t latency_ms;           /* how far ahead the stream is decoded while RATE, TEMPO
                                      or PITCH is in use, so how soon a change is heard:
                                      50..buffer_ms, default 150 */
} fa_tempo_options;

FA_API void FA_CALL fa_tempo_options_init(fa_tempo_options* options);
/* keep_formants and latency_ms take effect at once. A new quality restarts the
 * processing from the position being heard (like a seek to it); on a stream that
 * cannot seek (live web streams, push streams) it applies after the next stop, seek
 * or attach. NULL restores the defaults. */
FA_API fa_result FA_CALL fa_stream_set_tempo_options(fa_stream* stream, const fa_tempo_options* options);
FA_API fa_result FA_CALL fa_stream_get_tempo_options(fa_stream* stream, fa_tempo_options* out_options);

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
 *   2. ReplayGain 2 / R128 tags in the file (ID3v2 TXXX, FLAC Vorbis comments, APEv2, WMA attributes),
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

/* ---- CD+G karaoke graphics ---------------------------------------------
 *
 * A CD+G track is a song plus a stream of graphics packs (300 per second) that
 * draw the lyrics. FatzAudio treats the graphics as a side stream of the audio:
 *   - fa_stream_open_path("song.mp3") also loads "song.cdg" from the same folder
 *     (MP3+G pairs; any audio format works, the .cdg extension may be in any case);
 *   - fa_stream_open_path / fa_stream_open_memory of a ZIP archive plays the audio
 *     file inside it and loads the .cdg next to it in the archive;
 *   - fa_stream_load_cdg_path / fa_stream_load_cdg_memory attach graphics to any
 *     other stream (web streams, FFmpeg hand-off, other file names);
 *   - fa_stream_open_disc reads them from a CD+G disc's subcode while the track
 *     plays (has_cdg turns 1 once they are found, usually within a second).
 * fa_stream_info.has_cdg is 1 when a stream has graphics.
 *
 * To show them, create a surface (fa_cdg_surface_create) and call
 * fa_cdg_surface_update from your render loop (each vsync, or a timer). It draws the
 * picture at the time the stream is heard at right now (fa_stream_get_clock), into
 * a pixel buffer in the format and size you chose, and tells you whether and where
 * it changed. You copy that to whatever you draw with: a Direct3D/OpenGL/Metal/
 * Vulkan texture, a WPF WriteableBitmap, a SkiaSharp or Android bitmap, a GDI DIB.
 * Pausing, seeking, looping, crossfades and tempo changes need no extra work: the
 * graphics always follow the audio clock. Several surfaces (a preview window and a
 * TV output, at different sizes) can show one stream.
 *
 * The CD+G instruction set is complete: memory and border preset, normal and XOR
 * tile blocks, scroll preset and copy with smooth-scroll offsets, transparent colour,
 * and both colour-table loads. CD+EG (extended graphics) packs are ignored. */

typedef struct fa_cdg_surface fa_cdg_surface;

/* Byte order in memory. BGRA32 matches Direct3D/DXGI B8G8R8A8, WPF Bgra32/Pbgra32,
 * SkiaSharp Bgra8888 and GDI DIBs; RGBA32 matches OpenGL/Vulkan/Metal RGBA8 and
 * Android ARGB_8888 bitmaps. INDEXED8 is one byte per pixel, a colour index 0..15
 * into fa_cdg_frame.palette. */
typedef enum fa_cdg_pixel_format {
    FA_CDG_PIXELS_BGRA32 = 0,
    FA_CDG_PIXELS_RGBA32,
    FA_CDG_PIXELS_INDEXED8
} fa_cdg_pixel_format;

/* The CD+G screen is 300x216 pixels; the picture area inside the border is 288x192
 * (6 pixels left and right, 12 top and bottom). */
#define FA_CDG_WIDTH 300
#define FA_CDG_HEIGHT 216
#define FA_CDG_PICTURE_WIDTH 288
#define FA_CDG_PICTURE_HEIGHT 192

typedef enum fa_cdg_flags {
    FA_CDG_NO_BORDER     = 1 << 0,  /* only the 288x192 picture area, without the border */
    FA_CDG_TRANSPARENT   = 1 << 1,  /* the background colour (that of the last screen clear)
                                       is fully transparent, and colours the disc marks as
                                       transparent are too: for lyrics over video */
    FA_CDG_PREMULTIPLIED = 1 << 2,  /* colour multiplied by alpha (WPF Pbgra32, Direct2D,
                                       most compositors). Without it alpha is straight */
    FA_CDG_SMOOTH        = 1 << 3,  /* bilinear scaling instead of sharp (nearest) pixels;
                                       ignored for INDEXED8 */
    FA_CDG_TRANSPARENT_BACKGROUND_ONLY = 1 << 4, /* like FA_CDG_TRANSPARENT, but only the
                                       background colour is transparent: transparent-colour
                                       packs are ignored (some discs set them in ways that
                                       punch holes in the lyrics). Wins over FA_CDG_TRANSPARENT */
    FA_CDG_HIDE_UNTIL_PRESET = 1 << 5, /* every colour is fully transparent until the first
                                       screen clear (memory preset), so a picture or video
                                       underneath shows instead of a black screen */
    FA_CDG_BACKGROUND_COLOR = 1 << 6, /* fa_cdg_surface_config.background_color replaces the
                                       background colour, alpha included */
    FA_CDG_BACKGROUND_OPACITY = 1 << 7 /* fa_cdg_surface_config.background_opacity sets how
                                       opaque the background colour is, whatever the flags
                                       above make it */
} fa_cdg_flags;

typedef struct fa_cdg_surface_config {
    uint32_t struct_size;              /* sizeof(fa_cdg_surface_config) */
    fa_cdg_pixel_format pixel_format;  /* default BGRA32 */
    uint32_t width, height;            /* output size in pixels; 0 = native (300x216, or
                                          288x192 with FA_CDG_NO_BORDER); max 8192 */
    uint32_t flags;                    /* fa_cdg_flags */
    uint32_t background_color;         /* 0xAARRGGBB, with FA_CDG_BACKGROUND_COLOR */
    uint32_t background_opacity;       /* 0..100 %, with FA_CDG_BACKGROUND_OPACITY: the
                                          background's alpha is this share of 255 (or of
                                          background_color's alpha) */
} fa_cdg_surface_config;

FA_API void FA_CALL fa_cdg_surface_config_init(fa_cdg_surface_config* config);

/* One rendered picture. `pixels` belongs to the surface: it stays valid and unchanged
 * until the next update of that surface or fa_cdg_surface_destroy. */
typedef struct fa_cdg_frame {
    const void* pixels;
    uint32_t width, height;
    int32_t stride;                    /* bytes from one row to the next */
    fa_cdg_pixel_format pixel_format;
    int32_t changed;                   /* 1 = the pixels or the palette differ from the
                                          previous update (always 1 on the first update) */
    int32_t palette_changed;           /* 1 = `palette` differs from the previous update. For
                                          INDEXED8 this alone changes the colours on screen,
                                          with an empty dirty rectangle */
    uint32_t dirty_x, dirty_y;         /* the pixels that changed, in output pixels; upload just
                                          this rectangle. All 0 when no pixel changed */
    uint32_t dirty_width, dirty_height;
    uint8_t palette[16][4];            /* R, G, B, A of colours 0..15 (straight alpha). A is
                                          255 unless a transparency or background flag or
                                          option is set; RGB never has alpha folded in */
    fa_time time;                      /* position on the graphics timeline that is shown */
    /* The decoder's own state, for apps that apply their own transparency policy: */
    int32_t background_index;          /* colour of the last memory preset (screen clear);
                                          -1 before the first one, or without graphics */
    int32_t border_index;              /* colour of the last border preset; -1 before one */
    uint8_t transparency[16];          /* from transparent-colour packs: 0 opaque .. 63
                                          invisible, whatever the surface's flags */
    uint8_t background[4];             /* R, G, B, A the background is drawn with (palette
                                          entry background_index, or background_color with
                                          FA_CDG_BACKGROUND_COLOR): to fill the area around
                                          a scaled picture. All 0 while background_index
                                          is -1 */
    uint64_t revision;                 /* goes up by one whenever an update changes a pixel
                                          (1 after the first update): compare it with the one
                                          a render target last uploaded */
    uint64_t palette_revision;         /* goes up whenever palette, background_index,
                                          border_index or transparency change */
} fa_cdg_frame;

/* What a stream's graphics are and where they come from. */
typedef enum fa_cdg_source {
    FA_CDG_SOURCE_NONE = 0,            /* no graphics */
    FA_CDG_SOURCE_FILE,                /* a .cdg file, a ZIP entry or a memory block */
    FA_CDG_SOURCE_DISC                 /* a CD+G disc's subcode, read while the track plays */
} fa_cdg_source;

typedef struct fa_cdg_info {
    fa_cdg_source source;
    uint64_t pack_count;               /* 300 per second (a disc track's whole length) */
    fa_time checkpoint_interval;       /* seeks replay at most this much (10 s, longer for
                                          files over 30 minutes) */
    uint32_t checkpoint_count;         /* built so far. Files: all of them once the stream's
                                          first surface exists (0 before; a few ms for a
                                          song). Discs: as the packs arrive */
    int32_t disc_parity;               /* discs: 1 = the subcode layout was chosen by the
                                          packs' P/Q parity, 0 = by counting instructions
                                          (drives that zero the parity). Either way the packs
                                          are applied as read, without correction. -1 for
                                          other sources */
} fa_cdg_info;

/* Attaches graphics to any stream, replacing what it had. The file or block is read
 * in full (memory is copied); path NULL removes the graphics. FA_ERROR_NOT_FOUND if
 * the file is missing, FA_ERROR_UNSUPPORTED_FORMAT if it holds no whole pack. */
FA_API fa_result FA_CALL fa_stream_load_cdg_path(fa_stream* stream, const char* path);
FA_API fa_result FA_CALL fa_stream_load_cdg_memory(fa_stream* stream, const void* data, size_t size);
/* Shifts the graphics against the audio: positive shows them later, negative earlier.
 * For files that are out of sync; default 0. */
FA_API fa_result FA_CALL fa_stream_set_cdg_offset(fa_stream* stream, fa_time offset);
FA_API fa_result FA_CALL fa_stream_get_cdg_offset(fa_stream* stream, fa_time* out_offset);
/* Never blocks on a load. A disc track reports FA_CDG_SOURCE_NONE until its graphics
 * are found. */
FA_API fa_result FA_CALL fa_stream_get_cdg_info(fa_stream* stream, fa_cdg_info* out_info);

/* config NULL = BGRA32, native size. A stream without graphics gives a blank
 * (black) picture until graphics are loaded. Destroy every surface you create, before
 * or after closing its stream (not while fa_stream_close runs): once the stream is
 * closed, updates return FA_ERROR_INVALID_ARG.
 *
 * Threads: one surface is updated from one thread at a time; different surfaces may
 * be updated from different threads. An update runs on the caller's thread and never
 * waits for the device or decode threads: it shares one short lock per stream with
 * graphics loads, disc pack writes and the stream's other surfaces. An update with
 * nothing new compares the palette and returns; it copies no pixels. */
FA_API fa_result FA_CALL fa_cdg_surface_create(fa_stream* stream, const fa_cdg_surface_config* config, fa_cdg_surface** out_surface);
FA_API void FA_CALL fa_cdg_surface_destroy(fa_cdg_surface* surface);
/* Draws the picture for the audio heard right now (fa_stream_get_clock). Cheap when
 * nothing changed: call it every frame and upload only when out_frame->changed. */
FA_API fa_result FA_CALL fa_cdg_surface_update(fa_cdg_surface* surface, fa_cdg_frame* out_frame);
/* Same, at a position you choose on the stream's timeline: for your own clock,
 * previews, or rendering to a video file.
 *
 * Timing, exactly: with g = time - the stream's CD+G offset (fa_stream_set_cdg_offset),
 * the picture shows packs [0, ceil(g * 300 / 1e9)) applied, g in ns. Pack n occupies
 * [n/300 s, (n+1)/300 s) and counts as soon as its slot starts: at g = 1 s exactly
 * packs 0..299 are applied, at 1 s + 1 ns packs 0..300. g <= 0 shows no pack.
 *
 * Seeking anywhere replays at most fa_cdg_info.checkpoint_interval of packs from a
 * checkpoint, even the first time and in parts never played (on a disc: in the parts
 * read so far). */
FA_API fa_result FA_CALL fa_cdg_surface_update_at(fa_cdg_surface* surface, fa_time time, fa_cdg_frame* out_frame);

/* ---- MIDI ---------------------------------------------------------------
 *
 * Standard MIDI Files (.mid, .midi, format 0, 1 and 2), karaoke .kar files and RIFF
 * MIDI (.rmi) open with the ordinary fa_stream_open_* calls, detected from the
 * content like any other format, and are played by a built-in SoundFont 2
 * synthesizer. They render to stereo F32 at the engine's mix_sample_rate, or
 * 44100 Hz when that is 0 (device native).
 *
 * The instruments come from a soundfont you load once and share:
 *
 *     fa_soundfont* gm;
 *     fa_soundfont_open_path("GeneralUser.sf2", &gm);
 *     fa_engine_set_soundfont(engine, gm);   (every MIDI stream opened from now on)
 *     fa_soundfont_close(gm);                (the engine keeps its own reference)
 *
 * fa_open_options.soundfont picks another one for a single stream. No soundfont
 * is built in. A MIDI file opened without one plays with the instruments the OS
 * ships for its own MIDI synthesizer (fa_soundfont_open_system: gm.dls on
 * Windows, gs_instruments.dls on macOS, a General MIDI .sf2 package on Linux),
 * loaded once per engine by the first MIDI stream that needs it. Where there is
 * none (Android, iOS), or with FA_ENGINE_NO_SYSTEM_SOUNDFONT, the open returns
 * FA_ERROR_NO_SOUNDFONT.
 * A soundfont is reference-counted: the engine and every stream using it hold a
 * reference, so it may be closed (or replaced on the engine) at any time and is
 * freed when the last user is done. Its samples are kept in memory as 32-bit
 * float (about twice the size of the .sf2 file).
 *
 * Seeking is sample-exact and fast: events up to the target are replayed without
 * rendering audio. Programs, controllers and pitch bends are restored; notes held
 * across the target are not restarted (they sound again from their next note-on).
 * The length is the time of the last event plus one second, so the last notes
 * ring out. Lyrics and other text events are listed by fa_stream_get_midi_text and
 * raised as FA_EVENT_MIDI_TEXT when they are heard. */

/* A SoundFont 2 (.sf2) or DLS (.dls, Level 1 and 2) bank from a UTF-8 path, or from
 * memory (copied). FA_ERROR_NOT_FOUND when the file is missing,
 * FA_ERROR_UNSUPPORTED_FORMAT when it is not a usable bank (SF3 and SFZ are not
 * supported yet). */
FA_API fa_result FA_CALL fa_soundfont_open_path(const char* path, fa_soundfont** out_font);
FA_API fa_result FA_CALL fa_soundfont_open_memory(const void* data, size_t size, fa_soundfont** out_font);
/* The instruments the operating system ships: %SystemRoot%\System32\drivers\gm.dls
 * (Windows), gs_instruments.dls in CoreAudio.component (macOS), the first General
 * MIDI .sf2 found under /usr/share/sounds/sf2 and /usr/share/soundfonts (Linux:
 * default-GM, default, FluidR3_GM, TimGM6mb, FluidR3_GS). FA_ERROR_NOT_FOUND when
 * there is none (Android, iOS, Linux without such a package). */
FA_API fa_result FA_CALL fa_soundfont_open_system(fa_soundfont** out_font);
/* Releases your reference. Streams and engines using the soundfont keep theirs. */
FA_API void FA_CALL fa_soundfont_close(fa_soundfont* font);

typedef struct fa_soundfont_info {
    char name[256];              /* the bank's name (INAM), "" if it has none */
    uint32_t preset_count;       /* instruments and drum kits */
    uint64_t sample_memory;      /* bytes of sample data held in memory */
} fa_soundfont_info;

FA_API fa_result FA_CALL fa_soundfont_get_info(fa_soundfont* font, fa_soundfont_info* out_info);

/* The soundfont for MIDI streams opened from now on (streams already open keep
 * theirs). The engine takes its own reference; NULL removes it. */
FA_API fa_result FA_CALL fa_engine_set_soundfont(fa_engine* engine, fa_soundfont* font);

typedef enum fa_midi_text_type {
    FA_MIDI_LYRIC = 0,       /* a syllable to sing: lyric events, or the words of a .kar file */
    FA_MIDI_TEXT,            /* any other text event (for .kar files, the "@" header lines) */
    FA_MIDI_MARKER,          /* marker events: song sections ("Verse", "Chorus") */
    FA_MIDI_CUE,             /* cue point events */
    FA_MIDI_TRACK_NAME,
    FA_MIDI_INSTRUMENT_NAME,
    FA_MIDI_COPYRIGHT
} fa_midi_text_type;

typedef enum fa_midi_text_flags {
    FA_MIDI_NEW_LINE = 1 << 0,       /* lyrics: a new line starts with this syllable */
    FA_MIDI_NEW_PARAGRAPH = 1 << 1   /* lyrics: a new verse or page starts with it */
} fa_midi_text_flags;

/* One text event. Lyrics come cleaned up: the line and paragraph marks of .kar
 * files ("/" and "\" before a syllable) and lyric events (CR and LF) are removed
 * from `text` and given as `flags` on the syllable that starts the new line, so
 * a karaoke screen joins the syllables of a line as they are and breaks before
 * the flagged ones. Spaces between words are kept as the file has them. */
typedef struct fa_midi_text {
    fa_frames frame;             /* where it is heard (source frames) */
    fa_time time;                /* the same, in nanoseconds */
    int32_t type;                /* fa_midi_text_type */
    uint32_t flags;              /* fa_midi_text_flags */
    int32_t track;               /* MTrk chunk it came from, 0-based */
    char text[256];              /* UTF-8 (Latin-1/Windows-1252 text is converted), truncated */
} fa_midi_text;

typedef struct fa_midi_info {
    char title[256];             /* .kar @T line, else the name of the first track */
    char artist[256];            /* the second .kar @T line, "" if none */
    char copyright[256];         /* copyright event */
    int32_t format;              /* Standard MIDI File format: 0, 1 or 2 */
    uint32_t tracks;             /* MTrk chunks */
    uint32_t text_count;         /* entries of fa_stream_get_midi_text (all types) */
    uint32_t lyric_count;        /* of those, FA_MIDI_LYRIC */
    int32_t karaoke;             /* 1 = a karaoke (.kar) file: "@K"/"@T" header lines or a
                                    "Words" track. Its words are the lyrics */
} fa_midi_info;

/* FA_ERROR_INVALID_ARG for streams that are not MIDI. */
FA_API fa_result FA_CALL fa_stream_get_midi_info(fa_stream* stream, fa_midi_info* out_info);
/* Text events in time order, index 0 .. text_count - 1 (FA_ERROR_NOT_FOUND past it). */
FA_API fa_result FA_CALL fa_stream_get_midi_text(fa_stream* stream, uint32_t index, fa_midi_text* out_text);

/* Live controls. Like FA_PARAM_PITCH they change the audio where it is rendered,
 * so a change is heard after the decode-ahead buffer: MIDI streams default to a
 * 150 ms buffer (fa_open_options.buffer_ms) to keep that short.
 *
 * Transpose: semitones -48..48 added to every note but the drums (channel 10),
 * from the next note on; held notes end at their own pitch. The soundfont plays
 * the transposed notes themselves, so a key change sounds as clean as the
 * original (unlike FA_PARAM_PITCH, which shifts the rendered audio).
 * Mute: channel 0..15 (MIDI channel 1..16) goes silent at once and stays silent
 * until unmuted: for a karaoke "guide melody off". Master volume is
 * FA_PARAM_VOLUME and the tempo is FA_PARAM_TEMPO, as for every stream.
 * FA_ERROR_INVALID_ARG for streams that are not MIDI or values out of range. */
FA_API fa_result FA_CALL fa_stream_set_midi_transpose(fa_stream* stream, int32_t semitones);
FA_API fa_result FA_CALL fa_stream_get_midi_transpose(fa_stream* stream, int32_t* out_semitones);
FA_API fa_result FA_CALL fa_stream_set_midi_channel_mute(fa_stream* stream, uint32_t channel, int32_t muted);
FA_API fa_result FA_CALL fa_stream_get_midi_channel_mute(fa_stream* stream, uint32_t channel, int32_t* out_muted);

/* ---- Events ----------------------------------------------------------- */

/* Meaning of fa_event.frame and fa_event.value per type. "Position" is the stream
 * position (fa_stream_get_position) when the event happened.
 *
 * type                     frame                  value
 * FA_EVENT_ENDED           position at the end    0
 * FA_EVENT_POSITION        the requested frame    0
 * FA_EVENT_MARKER          the marker's frame     the fa_marker
 * FA_EVENT_METADATA        position               fa_metadata.seq of the new title
 * FA_EVENT_STALLED         position               0
 * FA_EVENT_RESUMED         position               0
 * FA_EVENT_DECODE_ERROR    position               the fa_result of the error
 * FA_EVENT_DEVICE_CHANGED  position               0
 * FA_EVENT_LOUDNESS_FINAL  position               0
 * FA_EVENT_RAMP_DONE       position               fa_ramp.tag of the finished ramp
 * FA_EVENT_STARTED         position               0
 * FA_EVENT_MIDI_TEXT       the text's frame       index for fa_stream_get_midi_text
 * FA_EVENT_LOOPED          loop end it left       loop start it went to (CUE_IN or 0)
 *
 * Delivered today: ENDED, MARKER, LOOPED, LOUDNESS_FINAL, RAMP_DONE and STARTED; MIDI_TEXT from MIDI
 * streams played on a device (decode-only streams list them with fa_stream_get_midi_text); METADATA from web
 * streams; STALLED and RESUMED from push, packet and web streams; DECODE_ERROR from packet
 * and disc streams. The others are raised by the features that produce them as those land (see docs/features). */
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
    FA_EVENT_STARTED,         /* a scheduled, crossfaded or queued start happened */
    FA_EVENT_MIDI_TEXT,       /* a MIDI lyric or other text event is heard */
    FA_EVENT_LOOPED           /* a looping stream jumped back to its loop start */
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

/* ---- Encoding and recording ------------------------------------------------
 *
 * An encoder writes audio to a file, or hands the bytes to your callbacks, in one
 * of these formats:
 *   FA_ENCODE_WAV   built in: 16 or 24-bit PCM, or 32-bit float; RF64 past 4 GB
 *   FA_ENCODE_FLAC  built in: lossless, 16 or 24 bit, with an MD5 of the audio
 *   FA_ENCODE_MP3   LAME, loaded at runtime (fa_lame_load); never bundled
 *
 * It takes audio from exactly one source, chosen after it is created:
 *   fa_encoder_attach_device  everything a device plays (its master mix, after the
 *                             limiter): recording the output
 *   fa_encoder_attach_stream  one playing stream as it is heard (volume, pan, fades,
 *                             normalization and its bus applied): recording a deck
 *   fa_encoder_encode_stream  a stream that is not attached, read as fast as it
 *                             decodes: converting a file
 *   fa_encoder_write          your own PCM, from any thread
 * Several encoders may share a device or a stream (an MP3 and a WAV of the same
 * mix, say).
 *
 * Threads: each encoder has its own thread that converts, encodes and writes. The
 * audio thread only copies into the encoder's queue (fa_encoder_options.queue_ms)
 * and never waits; if the encoder falls that far behind real time (a stalled
 * network write), the audio that does not fit is dropped and counted in
 * fa_encoder_status.dropped_frames. Output callbacks run on the encoder's thread.
 *
 * Format conversion: the input is resampled (windowed sinc) and its channels mixed
 * to the output's when they differ. 16-bit output is dithered (TPDF).
 *
 * fa_encoder_close stops the source, encodes what is still queued, completes the
 * file (lengths, MD5, the MP3's LAME/Xing header) and returns the first error the
 * encoder hit. Closing the device or stream an encoder records just ends its
 * input; the encoder stays open until you close it. */

typedef struct fa_encoder fa_encoder;

typedef enum fa_encode_format {
    FA_ENCODE_WAV = 1,
    FA_ENCODE_FLAC = 2,
    FA_ENCODE_MP3 = 3
} fa_encode_format;

typedef struct fa_encoder_options {
    uint32_t struct_size;        /* sizeof(fa_encoder_options) */
    fa_encode_format format;     /* default FA_ENCODE_WAV */
    uint32_t sample_rate;        /* output rate, 0 = the input's. MP3 only has 8, 11.025,
                                    12, 16, 22.05, 24, 32, 44.1 and 48 kHz: 0 picks the
                                    input's when allowed, else the nearest of 44.1/48 kHz */
    uint32_t channels;           /* output channels, 0 = the input's (MP3: at most 2, so
                                    surround input is mixed down to stereo). Any count
                                    can be mixed to any other */
    uint32_t bits_per_sample;    /* WAV: 16, 24 or 32 (32 = float); FLAC: 16 or 24.
                                    0 = 16. MP3 ignores it */
    uint32_t bitrate_kbps;       /* MP3: constant bitrate, 8 to 320. 0 = variable bitrate at
                                    `quality` */
    int32_t quality;             /* -1 = default. MP3 VBR: 0 (best, ~245 kbps) to 9 (smallest),
                                    default 2 (~190 kbps). FLAC: 0 (fastest) to 8 (smallest),
                                    default 5. WAV ignores it */
    uint32_t queue_ms;           /* input buffered ahead of the encoder thread, 0 = 2000 ms */
    /* Tags (UTF-8, NULL = none): ID3v2.3 for MP3, Vorbis comments for FLAC, a
     * LIST/INFO chunk for WAV. Copied when the encoder is created. */
    const char* title;
    const char* artist;
    const char* album;
    const char* genre;
    const char* date;            /* "2026" or "2026-10-17" */
    const char* comment;
    uint32_t track_number;       /* 0 = none */
} fa_encoder_options;

FA_API void FA_CALL fa_encoder_options_init(fa_encoder_options* options);

/* Where the bytes go when you take them yourself (a network upload, a pipe, memory).
 * write  Required. Takes all `size` bytes and returns FA_OK, or an error, which
 *        stops the encoder (the error is reported by status and close).
 * seek   Optional. Moves the write position to `offset` bytes from the start, so the
 *        encoder can complete its header at the end. NULL = forward only: WAV keeps
 *        "length unknown" sizes, FLAC an unknown length and no MD5, MP3 no Xing header.
 * close  Optional. Called exactly once, by fa_encoder_close, after the last write.
 * `user` belongs to the encoder only when fa_encoder_create_output returns FA_OK.
 * The struct is copied. */
typedef struct fa_encoder_output {
    fa_result (FA_CALL *write)(void* user, const void* data, size_t size);
    fa_result (FA_CALL *seek)(void* user, int64_t offset);
    void (FA_CALL *close)(void* user);
} fa_encoder_output;

/* options NULL = defaults (16-bit WAV at the input's format). The file is created
 * (or replaced) right away; nothing is written until audio arrives. MP3 returns
 * FA_ERROR_PROVIDER_MISSING when LAME cannot be loaded. */
FA_API fa_result FA_CALL fa_encoder_create_path(const char* path, const fa_encoder_options* options, fa_encoder** out_encoder);
FA_API fa_result FA_CALL fa_encoder_create_output(const fa_encoder_output* output, void* user,
                                                  const fa_encoder_options* options, fa_encoder** out_encoder);

/* The source. Each encoder takes one (FA_ERROR_BUSY once it has one). The input
 * format is fixed then: a device's rate in stereo; for an attached stream, the
 * rate of the device it plays on, in stereo (FA_ERROR_INVALID_ARG if it is not
 * attached; while it plays on a device of another rate, nothing is recorded). */
FA_API fa_result FA_CALL fa_encoder_attach_device(fa_encoder* encoder, fa_device* device);
FA_API fa_result FA_CALL fa_encoder_attach_stream(fa_encoder* encoder, fa_stream* stream);
/* Reads the stream from its current position to its end on the encoder's thread,
 * at its own rate and channels, as fast as it decodes and encodes; tempo, pitch and
 * rate apply as in fa_stream_read. The stream must not be attached (FA_ERROR_BUSY);
 * leave it alone until the encoder finishes (fa_encoder_wait) or is closed. */
FA_API fa_result FA_CALL fa_encoder_encode_stream(fa_encoder* encoder, fa_stream* stream);
/* Your own interleaved float PCM. The first call fixes the input format; later
 * calls must use the same (FA_ERROR_INVALID_ARG). Blocks while the queue is full,
 * so it never drops audio. Calls must not overlap. */
FA_API fa_result FA_CALL fa_encoder_write(fa_encoder* encoder, const float* interleaved, fa_frames frames,
                                          uint32_t sample_rate, uint32_t channels);

/* Paused: input from a device or stream is skipped (not queued), so the recording
 * just continues where it left off when resumed. */
FA_API fa_result FA_CALL fa_encoder_set_paused(fa_encoder* encoder, int32_t paused);

typedef enum fa_encoder_state {
    FA_ENCODER_IDLE = 0,         /* no source yet */
    FA_ENCODER_RUNNING,
    FA_ENCODER_FINISHED,         /* fa_encoder_encode_stream reached the end of the stream */
    FA_ENCODER_FAILED            /* stopped by an error (fa_encoder_status.error) */
} fa_encoder_state;

typedef struct fa_encoder_status {
    fa_encoder_state state;
    fa_result error;             /* first error, FA_OK if none */
    int32_t paused;
    fa_frames frames;            /* output frames encoded so far */
    fa_time duration;            /* `frames` as time */
    int64_t bytes;               /* bytes written to the file or output */
    fa_frames dropped_frames;    /* input lost because the queue was full */
    fa_audio_format input;       /* fixed by the source; zeros while idle */
    fa_audio_format output;      /* sample_format FA_FORMAT_S16, S24 or F32 for WAV/FLAC; F32 for MP3 */
} fa_encoder_status;

FA_API fa_result FA_CALL fa_encoder_get_status(fa_encoder* encoder, fa_encoder_status* out_status);
/* Waits until the encoder is FINISHED or FAILED: FA_OK, or FA_ERROR_BUSY if it is
 * still running after `timeout` (ns; negative = no limit). */
FA_API fa_result FA_CALL fa_encoder_wait(fa_encoder* encoder, fa_time timeout);
/* Stops the source, encodes what is queued, completes and closes the output, and
 * frees the encoder. Returns the first error (FA_OK when the file is complete). */
FA_API fa_result FA_CALL fa_encoder_close(fa_encoder* encoder);

/* ---- LAME at runtime ---------------------------------------------------------
 * MP3 encoding uses libmp3lame (LGPL) from your app, never bundled:
 *   1. one already loaded in the process, else
 *   2. one the system loader finds: libmp3lame.dll / libmp3lame-0.dll / lame_enc.dll
 *      (Windows), libmp3lame.0.dylib (macOS, Homebrew and MacPorts folders too),
 *      libmp3lame.so.0 (Linux, Android).
 * fa_lame_load(directory) loads it from a folder instead (call it first). The first
 * MP3 encoder loads it otherwise. Supported: LAME 3.98 and later. */
typedef struct fa_lame_info {
    int32_t loaded;              /* 1 when LAME is in use */
    char version[32];            /* get_lame_version(), e.g. "3.100" */
    char path[512];              /* where it was found (file name if unknown) */
} fa_lame_info;

/* FA_OK when LAME is (already) loaded, FA_ERROR_PROVIDER_MISSING if none was found.
 * Once loaded it stays loaded for the life of the process. */
FA_API fa_result FA_CALL fa_lame_load(const char* directory);
FA_API fa_result FA_CALL fa_lame_get_info(fa_lame_info* out_info);

/* ---- Equalizer ------------------------------------------------------- */

/* A parametric equalizer on every stream: up to FA_EQ_MAX_BANDS bands, each a peak
 * (bell) or a low or high shelf. Use ten peaks on the octave frequencies for a
 * graphic EQ, or start from a preset (fa_eq_get_preset).
 *
 * - It runs in the mixer, before the stream's volume, fades and normalization gain,
 *   so a change is heard at once, not after the decode-ahead buffer. It works on
 *   every stream played on a device, push and packet streams included.
 * - Changes glide (about 10 ms), so moving a slider while playing never clicks.
 * - Off, flat, or a band at 0 dB costs nothing: such bands are skipped.
 * - Decode-only streams are not equalized (fa_stream_read returns the audio as
 *   decoded); use fa_stream_add_dsp there.
 *
 * fa_stream_set_eq replaces all bands (count 0 = no bands: off). Out-of-range values
 * are clamped: frequency 10 Hz to just under half the device rate, q 0.1 to 20,
 * gain -24 to +24 dB. */
#define FA_EQ_MAX_BANDS 16

typedef enum fa_eq_type {
    FA_EQ_PEAK = 0,     /* bell around frequency_hz; q sets the width (1.41 = one octave) */
    FA_EQ_LOW_SHELF,    /* everything below frequency_hz; q 0.71 is the usual slope */
    FA_EQ_HIGH_SHELF    /* everything above frequency_hz */
} fa_eq_type;

typedef struct fa_eq_band {
    fa_eq_type type;
    float frequency_hz;
    float q;
    float gain_db;
} fa_eq_band;

FA_API fa_result FA_CALL fa_stream_set_eq(fa_stream* stream, const fa_eq_band* bands, uint32_t count);
/* Copies up to `capacity` bands, as set (after clamping; the frequency is also capped
 * at just under half the device rate while playing); *out_count = how many there are. */
FA_API fa_result FA_CALL fa_stream_get_eq(fa_stream* stream, fa_eq_band* out_bands, uint32_t capacity, uint32_t* out_count);
/* One band's gain, for a slider: FA_ERROR_INVALID_ARG if `band` >= the band count. */
FA_API fa_result FA_CALL fa_stream_set_eq_gain(fa_stream* stream, uint32_t band, float gain_db);
/* Bypass without losing the bands: 0 glides to flat, 1 back to the bands. On by default. */
FA_API fa_result FA_CALL fa_stream_set_eq_enabled(fa_stream* stream, int32_t enabled);
FA_API fa_result FA_CALL fa_stream_get_eq_enabled(fa_stream* stream, int32_t* out_enabled);

/* Built-in presets: ten peak bands on the octave frequencies (31 Hz to 16 kHz,
 * q 1.41), so switching presets moves the same sliders. */
typedef enum fa_eq_preset {
    FA_EQ_PRESET_FLAT = 0,
    FA_EQ_PRESET_ROCK,
    FA_EQ_PRESET_POP,
    FA_EQ_PRESET_JAZZ,
    FA_EQ_PRESET_CLASSICAL,
    FA_EQ_PRESET_DANCE,
    FA_EQ_PRESET_HIP_HOP,
    FA_EQ_PRESET_ACOUSTIC,
    FA_EQ_PRESET_VOCAL,
    FA_EQ_PRESET_SPOKEN_WORD,
    FA_EQ_PRESET_BASS_BOOST,
    FA_EQ_PRESET_TREBLE_BOOST,
    FA_EQ_PRESET_LOUDNESS,     /* lows and highs up, for quiet listening */
    FA_EQ_PRESET_COUNT
} fa_eq_preset;

#define FA_EQ_PRESET_BANDS 10

/* Fills out_bands (room for FA_EQ_PRESET_BANDS) and *out_count. */
FA_API fa_result FA_CALL fa_eq_get_preset(fa_eq_preset preset, fa_eq_band* out_bands, uint32_t* out_count);
/* English display name ("Rock"), NULL for an unknown preset. */
FA_API const char* FA_CALL fa_eq_preset_name(fa_eq_preset preset);

/* ---- User DSP --------------------------------------------------------- */

/* In-place processing of interleaved float frames. Streams on a device: device
 * channels (2) at the device rate, after rate/tempo/pitch, before the equalizer,
 * volume, fades and normalization. Decode-only streams: the format fa_stream_read
 * returns. Runs where the stream is decoded, never on the audio thread, so it may
 * take a little time; it is heard after the decode-ahead buffer. That is usually an
 * engine worker, but the first 100 ms after fa_stream_seek or fa_stream_attach are
 * decoded on the thread making that call, and decode-only streams decode inside
 * fa_stream_read. It runs with the stream's locks held: don't call any fa_stream_*
 * function of that stream from inside it. */
typedef void (FA_CALL *fa_dsp_callback)(float* frames, fa_frames count, uint32_t channels, uint32_t sample_rate, void* user);

/* Higher priorities run first; equal ones in the order added. Safe to call while
 * playing. After fa_stream_remove_dsp returns, the callback is never called again. */
FA_API fa_result FA_CALL fa_stream_add_dsp(fa_stream* stream, fa_dsp_callback callback, void* user, int32_t priority, uint32_t* out_dsp_id);
FA_API fa_result FA_CALL fa_stream_remove_dsp(fa_stream* stream, uint32_t dsp_id);

/* ---- Analysis --------------------------------------------------------- */

FA_API fa_result FA_CALL fa_stream_get_levels(fa_stream* stream, fa_time window, float* out_peak, float* out_rms, uint32_t channels);

#ifdef __cplusplus
}
#endif

#endif /* FATZAUDIO_H */
