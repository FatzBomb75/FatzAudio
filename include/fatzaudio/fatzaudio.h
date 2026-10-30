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
    FA_ERROR_TOO_LARGE = -17,        /* a compressed ZIP entry is bigger than
                                        fa_engine_config.zip_memory_limit_mb and no
                                        temp_directory is set: extract it first */
    FA_ERROR_SIZE_MISMATCH = -18,    /* fa_surface_update_into: the buffer is not the
                                        size fa_surface_get_layout reports */
    FA_ERROR_NOT_ENOUGH_CHANNELS = -19, /* FA_CUE_CHANNELS: the output device has no channels
                                        for the cue (fewer than cue_channel + 2); a mic's
                                        channel that its input device does not have */
    FA_ERROR_PERMISSION_DENIED = -20 /* the OS refused the microphone: the app has no
                                        permission (Android RECORD_AUDIO, the macOS and iOS
                                        microphone prompt, Windows privacy settings) */,
    FA_ERROR_OUT_OF_RANGE = -21      /* fa_deck_sync: the tempo it needs is outside the deck's
                                        FA_DECK_SYNC_RANGE, also at half or double time */
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

/* device_id NULL = system default; follows default-device changes where the system
 * reroutes (WASAPI, Core Audio). A started device that goes away (unplugged) is
 * reopened on the default at the same rate, its streams playing on; every stream
 * on it gets FA_EVENT_DEVICE_CHANGED (value 0 = rerouted, 1 = reopened). */
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
    int32_t text_code_page;  /* MIDI files: how their lyrics and other text are encoded.
                                0 = detect (default): UTF-8, Shift-JIS or Windows-1252,
                                decided for the whole file. FA_CODE_PAGE_* forces one;
                                other values are read as 0. With UTF-8 (detected or
                                forced) and detected Windows-1252, each event that is
                                valid UTF-8 stays UTF-8 and the others are read as
                                Windows-1252; forced Windows-1252 and Shift-JIS read
                                every event that way */
} fa_open_options;

/* fa_open_options.text_code_page, and fa_midi_credits.code_page. */
#define FA_CODE_PAGE_UTF8 65001
#define FA_CODE_PAGE_WINDOWS_1252 1252
#define FA_CODE_PAGE_SHIFT_JIS 932

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

/* Receives a web stream's raw bytes as they are downloaded, to save a station while
 * it plays (fa_net_options.download_proc, fa_stream_set_download_callback). The bytes
 * are the ones the decoder reads: a file as the server sends it, radio without its
 * ICY title blocks, HLS the audio of the segments (decrypted; from MPEG-TS segments
 * the audio stream inside). Called on the stream's download thread, in order; the
 * download waits while it runs, so return quickly. */
typedef void (FA_CALL *fa_download_proc)(void* user, const void* data, size_t size);

struct fa_net_options {
    uint32_t struct_size;          /* sizeof(fa_net_options) */
    const char* user_agent;        /* NULL = "FatzAudio/<version>" */
    const char* headers;           /* extra request headers, "Name: value" lines separated
                                      by "\r\n" or "\n" (cookies, authorization, ...); NULL = none.
                                      Authorization and Cookie go only to the URL's own origin
                                      (not to other servers a redirect or playlist leads to).
                                      Cookies the servers set are kept for the stream and sent
                                      back to them, after this Cookie line */
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
                                      second (the audio rendition of that variant's AUDIO
                                      group when it names one); 0 = the best one */
    fa_download_proc download_proc; /* the stream's raw bytes from the first one on (what
                                      the open reads to detect the format included);
                                      NULL = none. fa_stream_set_download_callback changes it */
    void* download_user;           /* download_proc's `user` */
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
/* Starts (or, with NULL, stops) handing a web stream's raw bytes to `proc` from the
 * next downloaded byte on (fa_download_proc). Once it returns, the previous callback
 * is not called again (a call in progress has finished), so its `user` can be freed;
 * it can also be called from inside the callback. A seek that restarts the download
 * elsewhere continues with the bytes from there. FA_ERROR_INVALID_ARG for streams
 * that are not web streams. */
FA_API fa_result FA_CALL fa_stream_set_download_callback(fa_stream* stream, fa_download_proc proc, void* user);

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
 * MIDI streams return the title, artist and copyright of fa_stream_get_midi_info,
 * tracker modules their title, artist and song message (COMMENT). */

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
    FA_TAG_FORMAT_MIDI,
    FA_TAG_FORMAT_TRACKER    /* a tracker module's title, artist and song message */
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
/* Like fa_stream_seek, but returns at once: an engine worker drops the buffer and
 * refills from `frame`, while the stream is silent. Until then the position and
 * the clock already report `frame`. A newer call replaces a target not reached
 * yet, so dragging a seek bar costs one seek per worker pass, not one per call.
 * Use it from a UI thread, and for web streams, whose seek waits for the network.
 * Any other call that moves the stream (fa_stream_seek, play from stopped, a
 * detach) carries out or replaces it. If the seek fails (a web server refusing
 * the range), playback goes on where it was and FA_EVENT_SEEK_FAILED is raised
 * (frame = the target, value = the error). Streams that are not attached seek at
 * once, as with fa_stream_seek. */
FA_API fa_result FA_CALL fa_stream_seek_async(fa_stream* stream, fa_frames frame);
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

/* Reverse playback (bass_fx's BASS_FX_ReverseCreate and BASS_ATTRIB_REVERSE_DIR), on
 * or off at any time, also while playing. Reversed, the stream plays backwards from
 * where it is: the position and the clock count down, and it ends (FA_EVENT_ENDED)
 * at CUE_IN or the start. Playing from stopped or ended starts at CUE_OUT or the end.
 * A loop (fa_stream_set_loop, a deck loop) wraps from its start to its end. Tempo,
 * pitch, rate, volume, effects and the fade markers work as forward. Changing the
 * direction restarts at the position heard, as fa_stream_seek does. While reversed,
 * marker, loop, MIDI text and tracker row events are not raised (position events
 * are), CUE_OUT does not end the stream, a queued stream starts when this one ends
 * (no crossfade), deck sync does not pull the deck into phase, and fa_deck_beat_jump
 * and deck loop changes take effect at once rather than on a beat. The source is
 * decoded forward in half-second blocks, one seek each. Push, packet and
 * forward-only streams return FA_ERROR_NOT_SEEKABLE, web streams
 * FA_ERROR_NOT_IMPLEMENTED. */
FA_API fa_result FA_CALL fa_stream_set_reverse(fa_stream* stream, int32_t reverse);
FA_API fa_result FA_CALL fa_stream_get_reverse(fa_stream* stream, int32_t* out_reverse);

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
 * They work on every stream, also decode-only and push and packet ones
 * (fa_stream_read returns the processed audio). Positions, markers, the clock and
 * CD+G graphics stay in source frames and follow the tempo by themselves; on push
 * and packet streams the clock keeps the app's timestamps and runs at the speed
 * heard, so video following it stays in sync.
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
    FA_STRETCH_FAST,       /* 100 ms blocks, about a third less CPU, slightly less clean */
    FA_STRETCH_TRANSIENTS  /* BEST, with the input itself played around each onset (drum
                              hits, plucks): no pre-echo before hits, which stay sharp
                              and on time; for drum-heavy material. Notes starting under
                              a held chord can get a faint seam */
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
 * kept. Both start with the device's next callback, so they are guaranteed to
 * begin on the same frame. */
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

/* ---- DJ mixer ----------------------------------------------------------- */

/* A DJ mixer: two decks on one output device, each with a channel fader, a trim,
 * an isolator EQ with kills and a meter, a crossfader between them, a master gain
 * and a limiter that is always on, plus a headphone cue.
 *
 *   deck: your stream (decoded, tempo/pitch, its own EQ, volume, fades)
 *     -> trim -> isolator EQ (low / mid / high) -+-> cue send (FA_DECK_CUE, pre-fader)
 *                                                '-> fader -> meter -> crossfader side
 *   decks -> master gain -> limiter -> master output (channels 1-2)
 *   cue = (1 - CUE_MIX) x cue sends + CUE_MIX x master, x CUE_GAIN -> headphones
 *
 * Decks hold ordinary streams: play, pause, seek, tempo, pitch, markers, CD+G and
 * lyrics work on them as on any stream, and every deck shares the master device's
 * clock, so fa_stream_play_at on two decks starts them on the same frame.
 * The mixer opens and owns its output device (fa_mixer_get_device): use it for the
 * clock, levels, spectrum and recording of the master. Closing that device (or the
 * engine) destroys the mixer. */
typedef struct fa_mixer fa_mixer;
typedef struct fa_deck fa_deck;

typedef enum fa_cue_mode {
    FA_CUE_NONE = 0,
    FA_CUE_CHANNELS,   /* channels 3-4 of the master device (DJ controllers and many USB
                          interfaces have 4 outputs): the same clock, no drift, no delay */
    FA_CUE_DEVICE,     /* a second output device (cue_device_id), such as USB headphones: its
                          own clock, bridged and lined up with the master (below) */
    FA_CUE_SPLIT       /* one stereo output and a Y-cable: the master in mono on the left,
                          the cue in mono on the right */
} fa_cue_mode;

typedef enum fa_latency_mode {
    FA_LATENCY_NORMAL = 0, /* the engine's period (fa_engine_config.period_ms) */
    FA_LATENCY_LOW,        /* the smallest shared-mode period the system gives: WASAPI's
                              low-latency mode on Windows (device rate, needs a driver that
                              offers small periods), about 5 ms elsewhere */
    FA_LATENCY_EXCLUSIVE   /* Windows WASAPI exclusive, for live mic monitoring: 3 ms periods
                              for the output and the mic inputs. Other apps can't use those
                              devices meanwhile, and the Windows volume no longer applies
                              (the mixer's gains and fa_device_set_volume still do).
                              FA_ERROR_NOT_IMPLEMENTED on other systems; FA_ERROR_DEVICE when
                              the device can't be had (another app holds it) */
} fa_latency_mode;

typedef struct fa_mixer_config {
    uint32_t        struct_size;   /* set by fa_mixer_config_init */
    uint32_t        deck_count;    /* 2 (default); more decks are planned */
    const char*     device_id;     /* the master output (fa_device_info.id), NULL = the default
                                      device (and follows it when it changes) */
    fa_cue_mode     cue_mode;      /* default FA_CUE_NONE */
    const char*     cue_device_id; /* FA_CUE_DEVICE: the headphones (fa_device_info.id of a
                                      playback device), NULL = the default output */
    uint32_t        cue_channel;   /* FA_CUE_CHANNELS: the first of the two cue channels,
                                      counted from 0; 0 = 2 (channels 3 and 4) */
    fa_latency_mode latency;       /* default FA_LATENCY_LOW */
    uint32_t        flags;         /* FA_MIXER_* */
} fa_mixer_config;

#define FA_MIXER_CUE_LOWEST_LATENCY (1u << 0)  /* FA_CUE_DEVICE: don't line the two outputs up
                                                   (neither is delayed) */

/* The headphones on a second device (FA_CUE_DEVICE). They run on their own crystal, so
 * the cue reaches them through a small buffer that a resampler keeps at a steady fill,
 * moving its rate by at most 0.05% (about 1 cent), whatever the drift between the two
 * devices or their rates (a 48 kHz master and 44.1 kHz headphones). The two outputs are
 * then lined up: the faster one is delayed by the difference of their latencies, so the
 * master heard in the headphones (FA_MIX_CUE_MIX) lands with the speakers in the room.
 * fa_mixer_latency says which output was delayed, and by how much.
 * - While any mic is monitored (FA_MIC_MONITOR), the master is never delayed: when the
 *   master is the faster output, alignment is skipped (FA_CUE_ALIGN_SKIPPED), and a delay
 *   already on the master is taken off the moment a mic is monitored.
 * - A delay on the master is part of what is heard: fa_stream_get_clock (so CD+G and
 *   lyric surfaces, and video following the clock), meters, fa_device_get_latency and
 *   fa_mixer_latency.mic_monitor include it. fa_stream_get_position, which counts frames
 *   handed to the device, does not include any output latency.
 * - The OS's latency figures are not exact: FA_MIX_CUE_TRIM_MS corrects them by ear.
 * - A change of alignment (a trim, a mic turned to monitored, other headphones) makes a
 *   short gap in the output it delays; small changes (under 5 ms) in the headphones glide.
 * Headphones that go away go silent and raise FA_EVENT_CUE_LOST; the master plays on
 * untouched (keeping any delay) until fa_mixer_set_cue_device opens headphones again.
 * Windows, macOS, Linux and Android (where the system may route both outputs to one
 * place); FA_ERROR_NOT_IMPLEMENTED on iOS (use FA_CUE_SPLIT). */

FA_API void      FA_CALL fa_mixer_config_init(fa_mixer_config* config);
/* Opens the output device (and the headphones of FA_CUE_DEVICE) and starts it.
 * FA_ERROR_NOT_ENOUGH_CHANNELS when FA_CUE_CHANNELS asks for channels the device does
 * not have; the headphones' open error (FA_ERROR_DEVICE, FA_ERROR_NOT_FOUND) when they
 * won't open. */
FA_API fa_result FA_CALL fa_mixer_create(fa_engine* engine, const fa_mixer_config* config, fa_mixer** out_mixer);
/* Unloads the decks (their streams stay open, detached) and closes the device. */
FA_API void      FA_CALL fa_mixer_destroy(fa_mixer* mixer);
FA_API fa_result FA_CALL fa_mixer_get_device(fa_mixer* mixer, fa_device** out_device);
FA_API fa_result FA_CALL fa_mixer_get_deck(fa_mixer* mixer, uint32_t index, fa_deck** out_deck);
/* The device's output channels, and the first cue channel (-1 = no cue channels, as
 * with FA_CUE_DEVICE). */
FA_API fa_result FA_CALL fa_mixer_get_output_channels(fa_mixer* mixer, uint32_t* out_channels, int32_t* out_cue_channel);
/* Moves the master output and the mics' inputs to another latency mode, as if the mixer
 * were made with it: the devices close and open again (a moment of silence) and playback
 * carries on where it was. When the mode can't be had the mixer stays in its own and the
 * result is that mode's error (fa_latency_mode); an input that won't open in the new
 * mode raises FA_EVENT_INPUT_LOST. FA_ERROR_BUSY while fa_mixer_measure_latency runs. */
FA_API fa_result FA_CALL fa_mixer_set_latency_mode(fa_mixer* mixer, fa_latency_mode mode);
FA_API fa_result FA_CALL fa_mixer_get_latency_mode(fa_mixer* mixer, fa_latency_mode* out_mode);
/* FA_CUE_DEVICE: opens other headphones (fa_device_info.id, NULL = the default output), or
 * the same again after FA_EVENT_CUE_LOST; the cue is silent meanwhile and is lined up
 * anew. A malformed id is FA_ERROR_INVALID_ARG and changes nothing; when the device won't
 * open the cue stays silent and the result is the open error.
 * FA_ERROR_INVALID_ARG when the mixer was made without FA_CUE_DEVICE. The headphones
 * follow the latency mode (fa_mixer_set_latency_mode), with FA_LATENCY_LOW's period in
 * exclusive mode: they are reopened with the master. */
FA_API fa_result FA_CALL fa_mixer_set_cue_device(fa_mixer* mixer, const char* device_id);

typedef enum fa_mix_param {
    FA_MIX_CROSSFADER = 0, /* 0 = side A only, 0.5 = the middle (default), 1 = side B only */
    FA_MIX_MASTER_GAIN,    /* linear, 0..4, default 1; before the limiter */
    FA_MIX_CUE_MIX,        /* 0 = cue only (default) .. 1 = master only */
    FA_MIX_CUE_GAIN,       /* linear, 0..4, default 1 */
    FA_MIX_CUE_TRIM_MS     /* FA_CUE_DEVICE: -250..250, default 0, added to the master's latency
                              when the outputs are lined up: raise it when the headphones sound
                              early against the speakers, lower it when they sound late. Takes
                              effect at once; not rampable */
} fa_mix_param;

/* Sets glide over 5 ms; ramps take the curve in `ramp` and start at `device_time` on
 * the device clock (0 = at once), as fa_stream_ramp_param does. Out-of-range values
 * are clamped. Mixer and deck ramps raise no events. */
FA_API fa_result FA_CALL fa_mixer_set_param(fa_mixer* mixer, fa_mix_param param, float value);
FA_API fa_result FA_CALL fa_mixer_get_param(fa_mixer* mixer, fa_mix_param param, float* out_value);
FA_API fa_result FA_CALL fa_mixer_ramp_param(fa_mixer* mixer, fa_mix_param param, float target,
                                             fa_time duration, fa_time device_time, const fa_ramp* ramp);

typedef enum fa_xfader_curve {
    FA_XFADER_CONSTANT_POWER = 0, /* default: steady loudness over the travel, each side
                                     -3 dB in the middle */
    FA_XFADER_LINEAR              /* each side -6 dB in the middle: a slight dip */
} fa_xfader_curve;
FA_API fa_result FA_CALL fa_mixer_set_crossfader_curve(fa_mixer* mixer, fa_xfader_curve curve);
FA_API fa_result FA_CALL fa_mixer_get_crossfader_curve(fa_mixer* mixer, fa_xfader_curve* out_curve);

/* ---- Decks ---- */

#define FA_DECK_LOAD_FORCE  (1u << 0)  /* replace a stream that is playing and heard */
#define FA_DECK_EQ_KILL_DB  (-60.0f)   /* an EQ band at this setting is silent */

/* Puts your stream on the deck: it is attached to the mixer's device through the
 * deck (from another device or deck too, keeping its position). The deck does not
 * own it: close it after unloading. NULL unloads (the stream is detached and its
 * deck loop cleared). FA_ERROR_BUSY while the current stream plays and the deck is
 * heard (fader up and the crossfader not closed on its side), unless
 * FA_DECK_LOAD_FORCE. Loading also shortens the stream's decode-ahead to 60 ms
 * (fa_tempo_options.latency_ms) when it is not playing, so tempo and pitch moves
 * are heard quickly; the cue point goes back to the stream's CUE_IN. A deck holds
 * one stream: fa_crossfade and fa_stream_set_next from a deck's stream to a stream
 * that is not attached yet are FA_ERROR_INVALID_ARG (load it on a deck instead).
 * A deck's stream may be closed from any thread: it leaves its deck first, under the
 * mixer's lock, so a deck call running on another thread finishes before it goes. */
FA_API fa_result FA_CALL fa_deck_load(fa_deck* deck, fa_stream* stream, uint32_t flags);
FA_API fa_result FA_CALL fa_deck_get_stream(fa_deck* deck, fa_stream** out_stream);  /* NULL = empty */

typedef enum fa_deck_param {
    FA_DECK_GAIN = 0,      /* the channel fader, linear 0..1, default 1 */
    FA_DECK_TRIM_DB,       /* -12..+12, default 0, on top of loudness normalization */
    FA_DECK_EQ_LOW_DB,     /* isolator bands, FA_DECK_EQ_KILL_DB (silent)..+6, default 0 */
    FA_DECK_EQ_MID_DB,
    FA_DECK_EQ_HIGH_DB,
    FA_DECK_XFADER_SIDE,   /* 0 = thru (not on the crossfader), 1 = side A, 2 = side B;
                              default deck 1 on A, deck 2 on B. Not rampable. */
    FA_DECK_CUE,           /* 1 = sent to the headphones, 0 = not (default). Not rampable. */
    FA_DECK_SYNC_RANGE     /* the most sync (fa_deck_sync, auto-mix uses its own) may move the
                              deck's TEMPO away from 1: 0.01..1, default 0.16 (+-16%). Not rampable. */
} fa_deck_param;
FA_API fa_result FA_CALL fa_deck_set_param(fa_deck* deck, fa_deck_param param, float value);
FA_API fa_result FA_CALL fa_deck_get_param(fa_deck* deck, fa_deck_param param, float* out_value);
FA_API fa_result FA_CALL fa_deck_ramp_param(fa_deck* deck, fa_deck_param param, float target,
                                            fa_time duration, fa_time device_time, const fa_ramp* ramp);

/* The cue point (CDJ style): fa_deck_cue pauses the deck and goes back to it.
 * Default: the stream's CUE_IN marker, else 0. FA_FRAMES_UNSET restores the default. */
FA_API fa_result FA_CALL fa_deck_set_cue_point(fa_deck* deck, fa_frames source_frame);
FA_API fa_result FA_CALL fa_deck_get_cue_point(fa_deck* deck, fa_frames* out_source_frame);
FA_API fa_result FA_CALL fa_deck_cue(fa_deck* deck);

/* A seamless loop between two source frames, at any time, also while playing. It is
 * made where the deck is decoded, so it follows tempo and pitch, raises
 * FA_EVENT_LOOPED at each turn, and the position and clock report the loop as
 * heard. Setting an out point the deck has just passed (up to 0.5 s ago, as a
 * "loop out" button does) loops back at once, keeping the loop's phase; one further
 * behind jumps to the in point. out <= in turns the loop off (playback carries on
 * from where it is); a seek past the out point also leaves the loop. The deck loop
 * replaces the CUE_IN..CUE_OUT loop of fa_stream_set_loop while it is on. Only
 * seekable streams loop (FA_ERROR_NOT_SEEKABLE). */
FA_API fa_result FA_CALL fa_deck_set_loop(fa_deck* deck, fa_frames loop_in, fa_frames loop_out);
FA_API fa_result FA_CALL fa_deck_get_loop(fa_deck* deck, fa_frames* out_loop_in, fa_frames* out_loop_out);  /* 0, 0 = none */

/* The deck's channel meter: after trim, EQ and fader, before the crossfader, as
 * heard now (like fa_stream_get_levels). Zeros while the deck is silent. */
FA_API fa_result FA_CALL fa_deck_get_levels(fa_deck* deck, fa_time window, float* out_peak, float* out_rms,
                                            uint32_t channels);

/* ---- Sync, beat loops and auto-mix ----
 *
 * Both decks need a beat grid (fa_stream_set_beat_grid from an analysis, or the exact
 * grid of a MIDI file or tracker module): FA_ERROR_NOT_FOUND otherwise. Beats are
 * measured where the decks are heard (fa_stream_get_clock), so they line up in the
 * room, including any master delay. A deck's heard tempo is its grid's BPM where it
 * plays x FA_PARAM_TEMPO x FA_PARAM_RATE. */
typedef enum fa_sync_flags {
    FA_SYNC_TEMPO = 1 << 0,   /* match the leader's heard BPM (half or double time when that
                                 keeps the deck within FA_DECK_SYNC_RANGE) */
    FA_SYNC_BEAT  = 1 << 1,   /* align beats */
    FA_SYNC_BAR   = 1 << 2,   /* align bars (implies BEAT); bars are known for exact grids, other
                                 grids count beat 0 as a downbeat (fa_beat_grid.bar_offset) */
    FA_SYNC_LOCK  = 1 << 3    /* keep it so: follows the leader's tempo moves and tempo maps */
} fa_sync_flags;

/* Syncs `deck` to `leader` (the other deck). TEMPO sets the deck's FA_PARAM_TEMPO (pitch is
 * kept: that is the stretcher). BEAT on a playing deck pulls a small phase error (under
 * 30 ms) in by bending the tempo by up to 2% for about two beats, and jumps a larger
 * one away on the deck's next beat (a seamless join, like a loop); on a deck that is not
 * playing it applies once it plays (or start it with fa_deck_play_synced). Without LOCK
 * the sync is done once; with LOCK the engine's event thread keeps the deck on the
 * leader's beat (about 20 times a second), so the deck's FA_PARAM_TEMPO moves by itself.
 * leader NULL or flags 0 ends a sync (the tempo stays where it is). Loading a stream on
 * either deck ends it too. FA_ERROR_OUT_OF_RANGE (nothing changes) when the tempo is out
 * of reach; FA_ERROR_INVALID_ARG for the deck itself as leader or another mixer's deck. */
FA_API fa_result FA_CALL fa_deck_sync(fa_deck* deck, fa_deck* leader, uint32_t flags);
/* Starts a deck that is not playing so that its next beat (BAR: downbeat) lands exactly
 * on one of the leader's, at least 50 ms from now: a stopped deck from its cue point, a
 * paused one from where it is. TEMPO matches the tempo first; LOCK keeps it locked
 * afterwards. The leader must be playing (FA_ERROR_INVALID_ARG otherwise). A deck that
 * plays already is synced as fa_deck_sync does. */
FA_API fa_result FA_CALL fa_deck_play_synced(fa_deck* deck, fa_deck* leader, uint32_t flags);
/* A loop of `beats` beats (1/16..32, e.g. 1, 2, 4, 8, 16) on the grid: from the beat
 * heard now (for loops shorter than a beat, the last whole part of it), or, while a deck
 * loop is on, from its in point (so halving and doubling keep the loop's start). 0 turns
 * the loop off (fa_deck_set_loop with 0, 0). The deck must be playing or paused with a
 * stream on it (FA_ERROR_NOT_FOUND when empty or without a grid). */
FA_API fa_result FA_CALL fa_deck_set_loop_beats(fa_deck* deck, double beats);
/* Beat jump: moves the deck `beats` beats on its grid (negative = back; 1/16..1024 either
 * way, e.g. 1, 4, 16, 32), at once and seamlessly (a join like a loop's). The distance is
 * whole grid beats from where the deck is heard, so a jump of whole beats keeps the beat
 * exactly where it was: a synced deck stays on the leader's beat. While a deck loop is on,
 * the loop moves with it (the same beats), as a CDJ's loop move does. On a paused or
 * stopped deck it moves the position. A jump past the song's end ends it; one before its
 * start lands on frame 0. A locked deck (FA_SYNC_LOCK) is pulled back on its next beat
 * unless the jump is whole beats (with FA_SYNC_BAR: whole bars). FA_ERROR_NOT_FOUND when
 * the deck is empty or has no grid; FA_ERROR_BUSY while sync's own jump or the previous
 * beat jump waits to be decoded (try again a moment later); FA_ERROR_NOT_SEEKABLE for live
 * streams. */
FA_API fa_result FA_CALL fa_deck_beat_jump(fa_deck* deck, double beats);

typedef struct fa_deck_beat_info {
    uint32_t struct_size;      /* sizeof(fa_deck_beat_info) */
    double   bpm;              /* heard: the grid's tempo where it plays x TEMPO x RATE */
    double   beat;             /* heard position in beats (fractional; beat 0 = first_beat) */
    double   bar;              /* heard position in bars (from bar_offset) */
    double   phase_error_ms;   /* against the leader while synced with BEAT or BAR (+ = this
                                  deck is ahead), else 0 */
    int32_t  locked;           /* 1 while synced with FA_SYNC_LOCK */
    int32_t  synced;           /* 1 while synced to a leader (fa_deck_sync) */
} fa_deck_beat_info;
/* FA_ERROR_NOT_FOUND when the deck is empty or its stream has no grid. */
FA_API fa_result FA_CALL fa_deck_get_beat_info(fa_deck* deck, fa_deck_beat_info* out_info);

/* Auto-mix: decks 1 and 2 take turns. While a deck plays, auto-mix starts the other one
 * at the playing deck's mix point and moves the crossfader across over the transition,
 * all scheduled on the device clock (sample-accurate, whatever the app's UI thread does).
 *  - The mix point: the outgoing stream's MIX_POINT marker, else its FADE_OUT_START,
 *    else its end (CUE_OUT, or the length) minus the transition's length.
 *  - The incoming deck starts at its cue point (fa_deck_set_cue_point, default CUE_IN).
 *  - Beat-aligned (ALIGN_BEATS, both grids at least min_confidence, the tempo within
 *    reach): the incoming deck starts at its first downbeat at or after its cue point, on
 *    the outgoing deck's downbeat nearest the mix point, the transition lasts whole bars,
 *    and the incoming deck is locked to the outgoing one until it is done.
 *  - Otherwise (no grid, low confidence, tempos too far apart) it is a plain timed
 *    crossfade: auto-mix never fails unattended.
 *  - The crossfader is ramped with its curve (fa_mixer_set_crossfader_curve) toward the
 *    incoming deck's side; decks on the same side, or thru, are started but not faded.
 *  - BASS_SWAP: the incoming deck's low band is killed until the middle of the transition
 *    (on a beat when beat-aligned), then the two decks' lows swap over one beat (0.5 s, or
 *    a quarter of a short transition, when not aligned): the outgoing deck's low goes to
 *    FA_DECK_EQ_KILL_DB as the incoming one's comes back to where it was. Auto-mix owns
 *    both decks' FA_DECK_EQ_LOW_DB meanwhile; when it is done (or cut short) both go back
 *    to their settings from before. Only for an incoming deck that auto-mix starts.
 *  - RESTORE_TEMPO: when the transition is done, an incoming deck whose tempo auto-mix
 *    moved (MATCH_TEMPO) glides back to the tempo it had before, over restore_time. Its
 *    FA_PARAM_TEMPO ramps (no FA_EVENT_RAMP_DONE is raised); setting the tempo, a sync with
 *    FA_SYNC_TEMPO or a new stream takes over from the glide (a sync without TEMPO does not).
 *  - When the transition is done, the outgoing deck is stopped.
 * Mixer events (fa_mixer_set_event_callback): FA_EVENT_AUTOMIX_STARTED when the incoming
 * deck is heard, FA_EVENT_AUTOMIX_DONE when the crossfader is across (index and value =
 * the incoming deck), and FA_EVENT_DECK_EMPTY (index = the idle deck) when a transition is
 * done, and again 30 s before the next mix point while the idle deck still holds the song
 * that was mixed out (or nothing). The app owns the playlist: load the next song with
 * fa_deck_load. With nothing new loaded the outgoing song plays to its end. Auto-mix
 * needs the outgoing song's length (a file, not a live stream), or a MIX_POINT or
 * FADE_OUT_START marker. */
typedef enum fa_automix_flags {
    FA_AUTOMIX_MATCH_TEMPO   = 1 << 0,  /* bring the incoming deck to the outgoing BPM (within
                                           max_tempo_change, half or double time allowed) */
    FA_AUTOMIX_ALIGN_BEATS   = 1 << 1,  /* start on a bar, length in whole bars */
    FA_AUTOMIX_BASS_SWAP     = 1 << 2,  /* the incoming low killed until the middle, then the lows
                                           swap over one beat */
    FA_AUTOMIX_RESTORE_TEMPO = 1 << 3   /* afterwards the incoming deck glides back to its own tempo */
} fa_automix_flags;

typedef struct fa_automix_options {
    uint32_t struct_size;        /* set by fa_automix_options_init */
    fa_time  length;             /* 0.5..60 s, default 8 s */
    float    length_beats;       /* used instead of length when > 0 and both grids are good
                                    (e.g. 32); 1..256 */
    uint32_t flags;              /* fa_automix_flags, default MATCH_TEMPO | ALIGN_BEATS */
    float    max_tempo_change;   /* 0..0.5, default 0.08: the most MATCH_TEMPO moves TEMPO from 1 */
    float    min_confidence;     /* 0..1, default 0.5: grids below it count as unknown */
    fa_time  restore_time;       /* RESTORE_TEMPO's glide, 0..300 s (0 = at once), default 20 s */
} fa_automix_options;
FA_API void      FA_CALL fa_automix_options_init(fa_automix_options* options);
/* Turns auto-mix on with `options` (or changes them for the next transition), NULL = off.
 * Turning it off leaves a transition already scheduled to finish, without events.
 * FA_ERROR_INVALID_ARG for values out of range or unknown flags. */
FA_API fa_result FA_CALL fa_mixer_set_automix(fa_mixer* mixer, const fa_automix_options* options);
/* Skip: starts the next transition now (on the next bar when beat-aligned).
 * FA_ERROR_NOT_FOUND when auto-mix is off, no deck plays or the other deck has nothing new
 * loaded; FA_ERROR_BUSY while a transition runs. */
FA_API fa_result FA_CALL fa_mixer_automix_now(fa_mixer* mixer);

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

/* ---- Track analysis: beat grid, key, waveform ---------------------------
 *
 * One decode of the audio gives any mix of these (kinds):
 *   - BEATS: a constant beat grid (tempo in BPM, the frame of the first beat, a
 *     confidence). MIDI files and tracker modules get their exact grid from the
 *     file instead (FA_GRID_EXACT): no audio analysis, and tempo changes included
 *     as segments.
 *   - KEY: tonic and mode, the name ("F#m") and the Camelot code ("11A").
 *   - WAVEFORM: peak and low/mid/high band levels every 10 ms, for drawing.
 *   - LOUDNESS: the same measurement as fa_scan_loudness_path, in the same pass.
 * Results are cached in fa_engine_config.cache_directory, keyed by the file's
 * content like loudness scans, so a song is analysed once.
 *
 * Accuracy, measured on General MIDI renders (docs/features/analysis.md has the
 * report): steady music gets its tempo within 0.05 BPM and beats within about
 * 2 ms; a tempo that changes or drifts (a live drummer) gets a tempo map that
 * stays within about 10 ms of the beats. Half-time songs may read at half tempo.
 * Real recordings do worse than renders, the key most of all (misses are mostly the
 * relative major/minor or a fifth away). Show the confidence, and let users correct
 * the grid (fa_stream_set_beat_grid). Bars (the meter, and which beat starts a bar)
 * are found from the harmony and the kicks when that is clear (FA_GRID_BARS);
 * otherwise a grid says 4 beats and that beat 0 starts a bar. */
typedef enum fa_analysis_kind {
    FA_ANALYZE_LOUDNESS = 1 << 0,
    FA_ANALYZE_BEATS    = 1 << 1,
    FA_ANALYZE_KEY      = 1 << 2,
    FA_ANALYZE_WAVEFORM = 1 << 3
} fa_analysis_kind;

typedef enum fa_analysis_flags {
    FA_ANALYZE_NO_CACHE = 1 << 0     /* neither read nor write the cache */
} fa_analysis_flags;

typedef struct fa_analysis_options {
    uint32_t struct_size;    /* sizeof(fa_analysis_options) */
    uint32_t kinds;          /* fa_analysis_kind bits; default BEATS | KEY */
    float min_bpm;           /* preferred tempo range: a tempo outside it is doubled or
                                halved into it (default 70) ... */
    float max_bpm;           /* ... 180; min_bpm >= 30, max_bpm <= 400 and at least
                                1.9 x min_bpm. MIDI and tracker grids are never folded */
    uint32_t flags;          /* fa_analysis_flags */
} fa_analysis_options;

FA_API void FA_CALL fa_analysis_options_init(fa_analysis_options* options);

typedef struct fa_analysis fa_analysis;

/* Analyses a file and blocks until done (about 1.5 s for a 4-minute song on one
 * core; call it from a worker thread, and run several at once for a library).
 * MIDI files use the engine's soundfont, else the OS's instruments, as playback
 * does. options NULL = defaults. FA_ERROR_UNSUPPORTED_FORMAT for files no decoder
 * reads. */
FA_API fa_result FA_CALL fa_analyze_path(fa_engine* engine, const char* path, const fa_analysis_options* options,
                                         fa_analysis** out_analysis);
/* The same for an open stream's file or memory (its own decoder, so playback is not
 * touched; MIDI files with the stream's soundfont). FA_ERROR_NOT_SEEKABLE for web,
 * push and app I/O streams, which cannot be read twice. */
FA_API fa_result FA_CALL fa_analyze_stream(fa_stream* stream, const fa_analysis_options* options,
                                           fa_analysis** out_analysis);
FA_API void FA_CALL fa_analysis_free(fa_analysis* analysis);

typedef enum fa_analysis_info_flags {
    FA_ANALYSIS_FROM_CACHE = 1 << 0  /* read from the cache, not measured now */
} fa_analysis_info_flags;

typedef struct fa_analysis_info {
    uint32_t struct_size;    /* sizeof(fa_analysis_info) */
    uint32_t kinds;          /* the kinds it holds (fa_analysis_kind) */
    uint32_t flags;          /* fa_analysis_info_flags */
    uint32_t sample_rate;    /* of the frames in grids and of the file's audio */
    fa_frames length;        /* frames of audio analysed */
} fa_analysis_info;

FA_API fa_result FA_CALL fa_analysis_get_info(const fa_analysis* analysis, fa_analysis_info* out_info);

typedef enum fa_grid_flags {
    FA_GRID_CONSTANT = 1 << 0,  /* one tempo for the whole song (segment_count 1) */
    FA_GRID_EXACT    = 1 << 1,  /* from the file (MIDI tempo events, tracker rows), not measured */
    FA_GRID_EDITED   = 1 << 2,  /* set by the app, e.g. after the user corrected it */
    FA_GRID_BARS     = 1 << 3   /* beats_per_bar and bar_offset are known (MIDI files
                                   with one time signature from the start, or found
                                   clearly in the audio); else they are 4 and 0 */
} fa_grid_flags;

typedef struct fa_beat_grid {
    uint32_t struct_size;    /* sizeof(fa_beat_grid) */
    double bpm;              /* the main tempo: the one that lasts longest */
    fa_frames first_beat;    /* source frame of the first beat heard (the grid goes on
                                backwards from it at the first segment's tempo) */
    uint32_t sample_rate;    /* of the source frames */
    uint32_t beats_per_bar;  /* 4, or what the file's time signature says */
    uint32_t bar_offset;     /* number of the beat (0..beats_per_bar - 1) that starts a bar,
                                counting first_beat as 0 */
    float confidence;        /* 0..1; 1 for exact grids */
    uint32_t flags;          /* fa_grid_flags */
    uint32_t segment_count;  /* tempo segments; 1 = constant tempo */
} fa_beat_grid;

/* A stretch of the song at one tempo. A constant grid is one segment starting at
 * first_beat with beat 0. Beat numbers count from first_beat (0) and are not whole
 * where a tempo change falls between beats. */
typedef struct fa_tempo_segment {
    fa_frames start;         /* source frame */
    double bpm;
    double beat;             /* beat number at `start` */
} fa_tempo_segment;

typedef struct fa_waveform_point {   /* one per 10 ms of audio */
    float peak;              /* 0..1, the highest sample of both channels */
    float low, mid, high;    /* 0..1, RMS (sine = 1 at full scale) below 250 Hz, 250 Hz to
                                2.5 kHz, above 2.5 kHz: for coloured waveforms */
} fa_waveform_point;

typedef struct fa_key_info {
    uint32_t struct_size;    /* sizeof(fa_key_info) */
    int32_t tonic;           /* 0 = C, 1 = C#/Db .. 11 = B */
    int32_t minor;           /* 1 = minor, 0 = major */
    float confidence;        /* 0..1 */
    char name[8];            /* "C", "F#m", "Bb", "Ebm" (the usual spelling of each key) */
    char camelot[4];         /* "8B" (C major) .. "12A" */
} fa_key_info;

/* Each getter returns FA_ERROR_NOT_FOUND when its kind was not asked for; the grid
 * and key also when the audio has no steady beat (silence, a few seconds of speech)
 * or no key (silence, drums only). */
FA_API fa_result FA_CALL fa_analysis_get_beat_grid(const fa_analysis* analysis, fa_beat_grid* out_grid);
/* Copies up to `capacity` segments (NULL with capacity 0 to count them). */
FA_API fa_result FA_CALL fa_analysis_get_tempo_map(const fa_analysis* analysis, fa_tempo_segment* out_segments,
                                                   uint32_t capacity, uint32_t* out_count);
FA_API fa_result FA_CALL fa_analysis_get_key(const fa_analysis* analysis, fa_key_info* out_key);
/* Copies up to `capacity` points, point i covering the 10 ms from i * 10 ms
 * (NULL with capacity 0 to count them). */
FA_API fa_result FA_CALL fa_analysis_get_waveform(const fa_analysis* analysis, fa_waveform_point* out_points,
                                                  uint32_t capacity, uint32_t* out_count);
/* gain_db is what normalization would apply with the engine's settings at the time
 * of the analysis. */
FA_API fa_result FA_CALL fa_analysis_get_loudness(const fa_analysis* analysis, fa_loudness* out_loudness);

/* The key after a pitch shift of `semitones` (a karaoke key change): tonic moved,
 * name and Camelot code to match, confidence kept. out_key may be `key`. */
FA_API fa_result FA_CALL fa_key_transpose(const fa_key_info* key, int32_t semitones, fa_key_info* out_key);
/* The key of tonic 0..11 and mode, confidence 1: for keys from the app's library. */
FA_API fa_result FA_CALL fa_key_make(int32_t tonic, int32_t minor, fa_key_info* out_key);

/* A stream's beat grid, for decks (sync, beat loops, aligned auto-mix): one from an
 * analysis, one the user corrected, or the app's own. grid NULL removes it (a MIDI or
 * tracker stream then gets its file's grid again). With
 * count 0 the grid is constant (one segment at first_beat, beat 0); otherwise
 * `segments` (count of them, starts rising, bpm 0.1..100000) are the tempo map and
 * grid->segment_count is ignored. Frames are the grid's sample_rate. The flags
 * are kept as given, except FA_GRID_CONSTANT, which is set when there is one
 * segment and cleared otherwise. */
FA_API fa_result FA_CALL fa_stream_set_beat_grid(fa_stream* stream, const fa_beat_grid* grid,
                                                 const fa_tempo_segment* segments, uint32_t count);
/* The grid set last, else, for MIDI files and tracker modules, the exact one from
 * the file (for a module, the first call renders it once without sound, about 0.25 s
 * for a 4-minute song), else FA_ERROR_NOT_FOUND. Segments as for
 * fa_analysis_get_tempo_map; out_segments may be NULL with capacity 0. */
FA_API fa_result FA_CALL fa_stream_get_beat_grid(fa_stream* stream, fa_beat_grid* out_grid,
                                                 fa_tempo_segment* out_segments, uint32_t capacity,
                                                 uint32_t* out_count);

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

/* A CD+G surface is an fa_surface (see Surfaces below): the fa_cdg_surface_*
 * functions and the fa_surface_* ones both take it. */
typedef struct fa_cdg_surface fa_cdg_surface;
typedef fa_cdg_surface fa_surface;

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
    uint64_t revision;                 /* goes up whenever an update changes a pixel (1 after
                                          the first update; by more than one when
                                          fa_surface_update_into drew changes in between):
                                          compare it with the one a render target last
                                          uploaded */
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
                                          (drives that zero the parity). With 1, a pack with
                                          one damaged symbol is corrected and one with more
                                          is dropped; with 0 packs are applied as read. -1
                                          for other sources */
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

/* Push model, for apps that would rather be told than poll (off by default). With it
 * on, the engine's event thread follows the stream's clock over the packs and raises
 * FA_EVENT_CDG_CHANGED (on the stream's event callback) whenever the picture heard
 * changes: lyrics being coloured in, a screen clear, a colour change, a seek, new
 * graphics. At most one event per few milliseconds, covering all packs since the
 * last. On the event, call fa_cdg_surface_update for your surfaces (from your render
 * thread, or in the callback) and upload what they report; the surfaces themselves
 * stay yours. The first event after turning it on covers the whole screen.
 * fa_event.frame is the number of packs shown, fa_event.value what changed, in
 * native 300x216 screen pixels (a surface's dirty rectangle is in its own pixels):
 *   FA_CDG_CHANGE_X/Y/WIDTH/HEIGHT(value)  the area (0 wide: only colours changed)
 *   FA_CDG_CHANGE_PALETTE(value)           1 = colours or transparency changed */
FA_API fa_result FA_CALL fa_stream_set_cdg_events(fa_stream* stream, int32_t enabled);
#define FA_CDG_CHANGE_X(value)       ((uint32_t)((value) & 0xFFF))
#define FA_CDG_CHANGE_Y(value)       ((uint32_t)(((value) >> 12) & 0xFFF))
#define FA_CDG_CHANGE_WIDTH(value)   ((uint32_t)(((value) >> 24) & 0xFFF))
#define FA_CDG_CHANGE_HEIGHT(value)  ((uint32_t)(((value) >> 36) & 0xFFF))
#define FA_CDG_CHANGE_PALETTE(value) ((int32_t)(((value) >> 48) & 1))

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
 * nothing changed: call it every frame and upload only when out_frame->changed. The
 * first call allocates the surface's pixels (FA_ERROR_OUT_OF_MEMORY if it can't). */
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

/* ---- Surfaces -----------------------------------------------------------
 *
 * The functions every karaoke surface has (a CD+G surface and the MIDI lyric
 * surface are both surfaces). The main one draws into a buffer of yours, such as a
 * mapped upload buffer or a texture's staging memory, instead of handing out the
 * surface's own pixels: fa_surface_update_into. With two or three buffers in
 * rotation it draws only the area that changed since the frame the buffer holds.
 *
 *     fa_surface_layout lay = { sizeof(lay) };
 *     fa_surface_get_layout(surface, &lay);           // the buffer size it wants
 *     ... every frame, with buffer b that last held frame rev[b] (0 = never):
 *     fa_surface_frame fr = { sizeof(fr) };
 *     fa_surface_update_into(surface, FA_SURFACE_NOW, mem[b], stride, lay.width,
 *                            lay.height, rev[b], &fr);
 *     rev[b] = fr.revision;   // upload fr.dirty[0 .. dirty_count - 1] of buffer b
 *
 * Threads, lifetime and timing are those of fa_cdg_surface_update: one thread at a
 * time per surface, the caller's thread, never waiting for decoding or the device. */

#define FA_SURFACE_NOW INT64_MIN       /* as `time`: the stream's clock (fa_stream_get_clock) */
#define FA_SURFACE_HISTORY 8           /* how many recent revisions update_into can draw from */
#define FA_SURFACE_MAX_DIRTY 8

typedef struct fa_rect {
    int32_t x, y;
    uint32_t width, height;
} fa_rect;

/* Which part of the 300x216 CD+G screen is shown. */
typedef enum fa_safety_zone {
    FA_SAFETY_SHOW = 0,                /* all of it, border included */
    FA_SAFETY_REMOVE,                  /* the 288x192 picture area (FA_CDG_NO_BORDER) */
    FA_SAFETY_AUTO,                    /* REMOVE, unless the song draws in the border area
                                          (tiles or scrolls; a border preset's solid colour
                                          does not count): then SHOW */
    FA_SAFETY_SUPER                    /* the box around everything the song ever draws other
                                          than the background, on the 6x12 tile grid */
} fa_safety_zone;

typedef struct fa_surface_layout {
    uint32_t struct_size;              /* sizeof(fa_surface_layout) */
    uint32_t width, height;            /* the buffer update_into draws into, in pixels */
    int32_t bitmap_x, bitmap_y;        /* its top-left inside the presentation area (0, 0
                                          without a presentation size) */
    uint32_t content_x, content_y;     /* inside the buffer: the picture, without padding */
    uint32_t content_width, content_height; /* and margin */
    float scale;                       /* CD+G: the whole-number multiple drawn with
                                          presentation scaling on; 1 without it, whatever the
                                          output size */
    uint32_t crop_x, crop_y, crop_width, crop_height; /* CD+G: the part of the 300x216
                                          screen shown, in CD+G pixels */
    fa_safety_zone safety_zone;        /* CD+G: the zone in use, SHOW or REMOVE for AUTO; SHOW,
                                          REMOVE or SUPER for SUPER */
    int32_t safety_zone_resolved;      /* 1, or 0 when AUTO or SUPER could not look at the
                                          song (a disc, or no graphics yet) and SHOW is used */
    char font_used[128];               /* lyrics: the font family drawn with; "" for CD+G */
    uint32_t margin, padding;          /* in buffer pixels, on each side: the transparent
                                          margin of presentation scaling (one CD+G pixel) and
                                          fa_cdg_presentation.padding */
    uint64_t layout_revision;          /* goes up whenever any of the above changes (then
                                          reallocate, and the next draw is a full one) */
} fa_surface_layout;

typedef struct fa_surface_frame {
    uint32_t struct_size;              /* sizeof(fa_surface_frame) */
    uint64_t revision;                 /* of the picture now in the buffer: pass it back as
                                          dst_revision the next time this buffer is used */
    uint64_t base_revision;            /* the dirty rectangles are the change from this one
                                          (= dst_revision); 0 = a full draw */
    uint64_t layout_revision;
    uint32_t dirty_count;              /* 0 = nothing was drawn: the buffer was up to date */
    fa_rect dirty[FA_SURFACE_MAX_DIRTY]; /* the pixels drawn, in buffer pixels: upload these */
    fa_rect dirty_bounds;              /* the box around them */
    fa_time time;                      /* position on the graphics timeline shown (as
                                          fa_cdg_frame.time) */
    int64_t draw_ns;                   /* how long this call took */
    int64_t render_ns;                 /* of that, drawing pixels (with render-ahead: the
                                          time the frame took on the worker) */
    int32_t behind;                    /* render-ahead: 1 = the frame shown is more than
                                          1/60 s older than `time` (the workers are not
                                          keeping up), of a later position (after a seek
                                          back) or of graphics since replaced; a newer
                                          one follows. `time` above is the position it
                                          shows. Always 0 without render-ahead */
    /* CD+G, as in fa_cdg_frame (for colouring INDEXED8 output on the GPU): */
    int32_t background_index, border_index;
    uint8_t background[4];
    uint8_t palette[16][4];            /* R, G, B, A, straight alpha */
    uint8_t transparency[16];          /* 0 opaque .. 63 invisible */
    uint64_t palette_revision;
} fa_surface_frame;

/* Destroys a surface of any kind (for CD+G the same as fa_cdg_surface_destroy). */
FA_API void FA_CALL fa_surface_destroy(fa_surface* surface);
/* The size of the area the app shows the surface in, in physical pixels, for the
 * scaled presentations (CD+G presentation scaling, lyrics). 0 x 0 unsets it (the
 * default). The shape is kept and the size brought into 300x216 .. 3840x2160. A CD+G
 * picture without presentation scaling ignores it: it keeps its output size. */
FA_API fa_result FA_CALL fa_surface_set_presentation_size(fa_surface* surface, uint32_t width, uint32_t height);
/* Off (0) by default. On, a surface that renders into a bitmap of its own (CD+G with
 * presentation scaling; lyrics) has the engine's worker threads render its next frame
 * ahead, paced by the times update_into is called with, and update_into copies the
 * finished frame that fits `time` best. It does not wait for a worker that is
 * rendering: it shows the frame it has, and fa_surface_frame.behind says when that one
 * is late. When no worker is rendering and there is no fitting frame (the first, the
 * first after a change of layout or options, after a seek or new graphics, or more
 * than 1/60 s late) it draws the frame on the caller's thread. A lyric surface draws
 * a late frame there only when it is more than 100 ms late: below that it shows the
 * late frame (behind = 1) and drops it, since drawing here would leave the worker
 * behind again. Calls that change the
 * surface (options, sizes, the layout, fa_cdg_surface_update) wait for a running
 * render, at most one frame. It takes two more buffers of the layout's size. The
 * classic CD+G picture ignores it: it is drawn on the caller's thread either way. Off
 * frees the buffers. */
FA_API fa_result FA_CALL fa_surface_set_render_ahead(fa_surface* surface, int32_t enabled);
/* Fills `out_layout` up to its struct_size and sets struct_size to the bytes filled. */
FA_API fa_result FA_CALL fa_surface_get_layout(fa_surface* surface, fa_surface_layout* out_layout);
/* Draws the picture at `time` (FA_SURFACE_NOW = the audio heard now) into `dst`, the
 * same pixels fa_cdg_surface_update gives: the surface's pixel format, options and
 * output size, or what its presentation options (below) make of them. dst_width x
 * dst_height must be the layout's width x height (else FA_ERROR_SIZE_MISMATCH,
 * nothing drawn: get the layout again); dst_stride is bytes per row, at least width x
 * bytes per pixel.
 *
 * dst_revision is the revision dst holds (fa_surface_frame.revision of the call that
 * last drew into it; 0 = unknown). When it is one of the last FA_SURFACE_HISTORY
 * revisions, only the pixels that changed since then are drawn; otherwise all of
 * them. The rest of dst is not touched, so it must still hold that frame. out_frame
 * is filled up to its struct_size, which is set to the bytes filled. */
FA_API fa_result FA_CALL fa_surface_update_into(fa_surface* surface, fa_time time, void* dst, int32_t dst_stride,
                                                uint32_t dst_width, uint32_t dst_height, uint64_t dst_revision,
                                                fa_surface_frame* out_frame);

/* ---- Surface effects -----------------------------------------------------
 *
 * Outline, shadow and bevel around what a surface draws, from one distance field of
 * its shape: a true stroke (round, even, any width), a soft or hard shadow of the
 * shape and its outline, and a bevel lit from one side. Drawn shadow first, then the
 * outline, the shape, and the bevel over it, all premultiplied against transparent.
 *
 * CD+G: the shape is every pixel that is not the background (or transparent), and the
 * effects apply with presentation scaling on (fa_cdg_presentation.scale); the classic
 * picture ignores them. Effects reaching past the edge of the picture are cut there.
 * Sizes and distances run 0..1, 1 the maximum, in units of the surface:
 *
 *                outline size   shadow size   shadow distance   bevel size
 *     CD+G       1.5 px         2 px          2 px              1 px     (CD+G pixels)
 *     lyrics     0.12 em        0.20 em       0.15 em           0.08 em
 *
 * Colours are 0xAARRGGBB (straight alpha) and `opacity` multiplies their alpha.
 * `fade` 0..1 is the part of the size that eases out to nothing: 0 a hard edge, 1 a
 * glow. Angles are in degrees, counter-clockwise from the right (315 = down right).
 * `sung_color` is for lyrics; CD+G ignores it. */

typedef struct fa_fx_outline {
    int32_t on;
    uint32_t color, sung_color;
    float opacity, size, fade;
} fa_fx_outline;

typedef struct fa_fx_shadow {
    int32_t on;
    uint32_t color, sung_color;
    float opacity;
    float size;                        /* how far it spreads beyond the shape and outline */
    float distance, angle_degrees;     /* where it falls */
    float fade;
} fa_fx_shadow;

typedef struct fa_fx_bevel {
    int32_t on;
    uint32_t highlight, shade;         /* the sides facing the light, and away from it */
    float strength;                    /* 0..1 */
    float size;                        /* how far in from the edge */
    float light_angle_degrees;         /* where the light comes from */
    float fade;
} fa_fx_bevel;

typedef struct fa_surface_effects {
    uint32_t struct_size;              /* sizeof(fa_surface_effects) */
    fa_fx_shadow shadow;
    fa_fx_outline outline;
    fa_fx_bevel bevel;
} fa_surface_effects;

/* All off, with these values ready for when they are turned on: outline black, size
 * 0.5; shadow black at 60 %, distance 0.5 at 315 degrees, size 0; bevel white and
 * black at 50 %, strength 0.5, size 0.5, light from 135 degrees, fade 0.5. */
FA_API void FA_CALL fa_surface_effects_init(fa_surface_effects* effects);
/* Sets the effects (NULL = none, the default). FA_ERROR_INVALID_ARG for values out
 * of range (on not 0/1, opacity, size, distance, strength or fade outside 0..1, an
 * angle that is not finite). The next frame is drawn again in full. */
FA_API fa_result FA_CALL fa_surface_set_effects(fa_surface* surface, const fa_surface_effects* effects);
/* The effects in use (all off when none were set), up to out_effects->struct_size. */
FA_API fa_result FA_CALL fa_surface_get_effects(fa_surface* surface, fa_surface_effects* out_effects);

/* ---- CD+G presentation options ------------------------------------------
 *
 * How a CD+G surface presents the picture, on top of its fa_cdg_surface_config. A
 * surface without them (the default) draws exactly as before.
 *
 *     fa_cdg_presentation p;
 *     fa_cdg_presentation_init(&p);
 *     p.scale = 1;                       // full colour at a whole multiple
 *     p.safety_zone = FA_SAFETY_AUTO;
 *     p.smoothness = 2;
 *     fa_cdg_surface_set_presentation(surface, &p);
 *     fa_surface_set_presentation_size(surface, 1920, 1080);
 *
 * Scale off: the configured pixel format, output size and flags, with the safety
 * zone's crop and the padding around it (transparent: alpha 0, or index
 * FA_CDG_INDEX_TRANSPARENT in INDEXED8). Smoothness and edge blend are ignored.
 *
 * Scale on: the cropped picture at a whole-number multiple N, the one nearest the
 * presentation size, rounded up (1920x1080: SHOW is 5x, 1080 high; REMOVE is 6x,
 * 1152 high, which the app shrinks slightly). The buffer is premultiplied 32-bit
 * colour (RGBA with an RGBA32 config, BGRA otherwise) with a transparent background
 * (fa_surface_frame.background is the colour to fill behind it; the background flags
 * only change that report) and, from the outside in: padding, a transparent margin of
 * one CD+G pixel, the picture. Without a presentation size N is 1. The config's
 * output size, FA_CDG_SMOOTH and FA_CDG_PREMULTIPLIED do not apply; the transparency
 * flags and FA_CDG_HIDE_UNTIL_PRESET do.
 *
 * Changing them, the presentation size included, may change the layout (a new
 * layout_revision, a full draw next). A scroll of the whole screen moves the
 * surface's picture rather than drawing it again, and smooth-scroll offsets only
 * move what is copied out of it. */

#define FA_CDG_INDEX_TRANSPARENT 16    /* INDEXED8: padding, and the border with
                                          FA_CDG_TRANSPARENT_BORDER */

typedef enum fa_cdg_presentation_flags {
    FA_CDG_TRANSPARENT_BORDER = 1 << 0 /* the border area is transparent too; its colour is
                                          still reported in border_index */
} fa_cdg_presentation_flags;

typedef struct fa_cdg_presentation {
    uint32_t struct_size;              /* sizeof(fa_cdg_presentation) */
    int32_t scale;                     /* 0 = off (the configured format and size), 1 = on */
    fa_safety_zone safety_zone;        /* default SHOW; replaces FA_CDG_NO_BORDER */
    float auto_ignore_seconds;         /* AUTO: drawing in the border before this time on the
                                          graphics timeline does not count (title cards) */
    int32_t super_centered;            /* SUPER: the same crop on both sides */
    uint32_t padding;                  /* CD+G pixels of transparent space on each side,
                                          0..32 */
    int32_t smoothness;                /* 0..10, scale on only. 0 sharp pixels, 1-4 softer
                                          (4 = bilinear), 5-10 traced outlines: 5 diagonal
                                          steps straightened, 6 shallow slopes too, 7-8
                                          corners rounded, 9-10 curves */
    int32_t edge_blend;                /* 0/1, scale on only: anti-aliases stair steps along
                                          the edges between colours (MLAA); no effect at
                                          smoothness 5-10, which are smooth already */
    uint32_t flags;                    /* fa_cdg_presentation_flags */
} fa_cdg_presentation;

/* Defaults: scale off, SHOW, nothing else. */
FA_API void FA_CALL fa_cdg_presentation_init(fa_cdg_presentation* presentation);
/* NULL goes back to the classic picture of the surface's config. Takes effect at the
 * next update or fa_surface_get_layout. */
FA_API fa_result FA_CALL fa_cdg_surface_set_presentation(fa_surface* surface, const fa_cdg_presentation* presentation);
/* The options in use; without any set, the classic picture's (SHOW, or REMOVE with
 * FA_CDG_NO_BORDER). Fills up to struct_size. */
FA_API fa_result FA_CALL fa_cdg_surface_get_presentation(fa_surface* surface, fa_cdg_presentation* out_presentation);

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
 * rendering audio. Programs, controllers, pitch bends and SysEx settings are
 * restored, and notes held across the target (by their key or the sustain pedal)
 * start again from their attack, so a held chord is heard after a seek into it
 * (drum notes are not restarted). A seek clears the reverb and chorus tails, so
 * where no note is held and the reverb of earlier notes has died away (or with
 * fa_stream_set_midi_effects off), what follows a seek is exactly what playing
 * through gives.
 *
 * Reverb and chorus are built in and on: each channel's CC91 / CC93 and the
 * soundfont's effect sends set how much of it goes to them (fa_stream_set_midi_effects).
 *
 * SysEx: GM, GM2, GS and XG resets; GS "use for rhythm part" and XG part mode set
 * drum channels other than channel 10; bank select MSB 127 (XG) and 120 (GM2)
 * pick drum kits; master volume (universal, GS and XG) and master fine and coarse
 * tuning take effect. Other SysEx is ignored.
 * The length is the time of the last event plus one second, so the last notes
 * ring out. Lyrics and other text events are listed by fa_stream_get_midi_text and
 * raised as FA_EVENT_MIDI_TEXT when they are heard. */

/* A SoundFont 2 (.sf2), SF3 (.sf3, SoundFont 2 with Ogg Vorbis samples, decoded
 * at load) or DLS (.dls, Level 1 and 2) bank from a UTF-8 path, or from memory
 * (copied). FA_ERROR_NOT_FOUND when the file is missing,
 * FA_ERROR_UNSUPPORTED_FORMAT when it is not a usable bank (SFZ is not
 * supported). */
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

/* A MIDI file's song information, for intro and ending pages: the .kar "@T" and
 * "@I" header lines and the file's own copyright, tempo and signature events. Empty
 * strings for what the file does not say. All UTF-8. */
typedef struct fa_midi_credits {
    uint32_t struct_size;        /* sizeof(fa_midi_credits) */
    int32_t code_page;           /* how the file's text was read: FA_CODE_PAGE_* */
    char title[256];             /* the first "@T" line, else "@I Title", else the first track name */
    char artist[256];            /* the second "@T" line, else "@I Artist" / "Performer" */
    char writer[256];            /* "@I Written by", "Composer", "Music by", "Writer",
                                    "Words and music by" */
    char lyricist[256];          /* "@I Lyrics by", "Lyricist", "Lyrics", "Words and music by" */
    char year[32];               /* "@I Year", "Date", or an unlabelled year 1800..2200 */
    char copyright[256];         /* the copyright event, else a copyright "@I" or third "@T" line */
    char key[32];                /* the first key signature ("E minor"), else "@I Key" */
    char tempo[32];              /* the first tempo ("120 bpm"), else "@I Tempo" / "BPM" */
    char time_signature[32];     /* the first time signature ("3/4"), else "@I Timing" */
    char other[1024];            /* other "@I" lines and "@T" lines after the second, one
                                    per line ("\n"), without repeats */
} fa_midi_credits;

/* Fills `out_credits` up to its struct_size. FA_ERROR_INVALID_ARG for streams that
 * are not MIDI. */
FA_API fa_result FA_CALL fa_stream_get_midi_credits(fa_stream* stream, fa_midi_credits* out_credits);
/* Text events in time order, index 0 .. text_count - 1 (FA_ERROR_NOT_FOUND past it). */
FA_API fa_result FA_CALL fa_stream_get_midi_text(fa_stream* stream, uint32_t index, fa_midi_text* out_text);

typedef enum fa_midi_mark_type {
    FA_MIDI_MARK_TEMPO = 0,          /* value = microseconds per quarter note; text "120 bpm" */
    FA_MIDI_MARK_TIME_SIGNATURE,     /* value / value2 = numerator / denominator; text "3/4" */
    FA_MIDI_MARK_KEY_SIGNATURE       /* value = sharps (negative: flats), value2 = 1 minor, 0 major; text "E minor" */
} fa_midi_mark_type;

/* A tempo change or a time or key signature of the file. Frames and times are
 * where it is heard; FA_PARAM_TEMPO stretches the stream on top of the file's own
 * tempo and changes neither. */
typedef struct fa_midi_mark {
    fa_frames frame;             /* source frames */
    fa_time time;                /* the same, in nanoseconds */
    int32_t type;                /* fa_midi_mark_type */
    int32_t track;               /* MTrk chunk it came from, 0-based */
    int32_t value;               /* see fa_midi_mark_type */
    int32_t value2;
    char text[32];               /* readable form, UTF-8 */
} fa_midi_mark;

/* Copies up to `capacity` marks, in time order, into `out_marks` (NULL with capacity
 * 0 to count them) and gives how many the file has in *out_count. Malformed
 * signature events are left out. FA_ERROR_INVALID_ARG for streams that are not MIDI. */
FA_API fa_result FA_CALL fa_stream_get_midi_marks(fa_stream* stream, fa_midi_mark* out_marks, uint32_t capacity, uint32_t* out_count);

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
/* Reverb and chorus: on by default, following each channel's CC91 / CC93 (reverb
 * 40 and chorus 0 until the file sets them) and the soundfont's effect sends.
 * Off plays the dry synth (BASS_MIDI_NOFX); the tails stop at once. Live, like
 * the controls above. */
FA_API fa_result FA_CALL fa_stream_set_midi_effects(fa_stream* stream, int32_t enabled);
FA_API fa_result FA_CALL fa_stream_get_midi_effects(fa_stream* stream, int32_t* out_enabled);

/* ---- MIDI lyric surface ---------------------------------------------------
 *
 * A karaoke surface (see Surfaces) that draws a MIDI or .kar song's lyrics: the
 * active line in the middle, the lines before and after it smaller and fainter,
 * each syllable coloured in as it is sung, lines scrolling up as the song moves on
 * and pages fading out and in, a countdown before a line that follows a long gap.
 * Text is drawn with FatzAudio's own font reader and rasterizer (no system text
 * engine), premultiplied BGRA against transparent, with the surface effects.
 *
 *     fa_lyric_style st;
 *     fa_lyric_style_init(&st, FA_LYRIC_CLASSIC);
 *     st.font_family = "Segoe UI";
 *     fa_surface* lyrics;
 *     fa_lyric_surface_create(stream, &st, &lyrics);
 *     fa_surface_set_presentation_size(lyrics, 1920, 1080);
 *     ... then fa_surface_get_layout and fa_surface_update_into as for any surface.
 *
 * Sizes are given for a presentation area 1080 pixels high and scale with its
 * height (fa_surface_layout.scale = height / 1080); without a presentation size the
 * surface draws for 1920x1080. The bitmap keeps one size for a presentation size
 * and style, and fa_surface_layout.bitmap_x and bitmap_y place it: the whole area
 * when the song has an intro or ending page (see intro_page), else the lyric block
 * (every line slot, the countdown row and the page fade-in included). The layout's
 * content rectangle is the box the lines fit in. Everything drawn is a function of
 * the position, so seeks and loops need nothing; lyric times follow the stream's
 * clock, and the animations run in real time (their lengths follow FA_PARAM_TEMPO).
 *
 * The song's text is fa_stream_get_midi_text's lyrics: its line and paragraph marks
 * break lines and pages, and so do the gaps: a pause of paragraph_gap_ms starts a
 * new page, one of sentence_gap_ms after ". ! ? : ;" a new line, and a line of
 * break_length characters breaks at the next space. A line still wider than the box
 * is split between words when the lines are built (each row is a line of its own,
 * with its own wipe, on the same page); a single word wider than the box is drawn
 * smaller. */

typedef enum fa_lyric_preset {
    FA_LYRIC_CLASSIC = 0,              /* MediaControl's look: white, sung yellow, a black
                                          outline and shadow, a cross-fading wipe */
    FA_LYRIC_MODERN,                   /* a sweeping wipe, a soft shadow */
    FA_LYRIC_NEON,                     /* glowing outlines, a sweeping wipe */
    FA_LYRIC_MINIMAL,                  /* one line above and below, a light shadow, no
                                          countdown */
    FA_LYRIC_SUBTITLE                  /* two lines in a band at the bottom on a panel, no
                                          countdown */
} fa_lyric_preset;

typedef enum fa_lyric_wipe {
    FA_WIPE_CROSSFADE = 0,             /* the syllable changes colour as a whole */
    FA_WIPE_SWEEP                      /* an edge sweeps across the syllable */
} fa_lyric_wipe;

typedef enum fa_lyric_countdown {
    FA_COUNTDOWN_OFF = 0,
    FA_COUNTDOWN_CLOCK,                /* an arrow a second (countdown_ms / (count + 1)) */
    FA_COUNTDOWN_BEAT                  /* an arrow a beat, from the song's tempo */
} fa_lyric_countdown;

/* "em" below is font_size. Times are milliseconds of real time. */
typedef struct fa_lyric_style {
    uint32_t struct_size;              /* sizeof(fa_lyric_style) */
    /* The font: a file (font_path, UTF-8), a file in memory (font_data, copied), or an
     * installed family (font_family, as fa_font_find); with none of them, or when the
     * family is not installed, the system's UI font. A song whose text the font
     * mostly cannot show is drawn with the system's font for its script (Japanese,
     * Korean, Chinese) unless font_explicit is 1. fa_surface_layout.font_used names
     * the family drawn with. */
    const char* font_family;
    const char* font_path;
    const void* font_data;
    size_t font_data_size;
    uint32_t font_face;                /* the face of a collection (.ttc); 0 */
    int32_t font_explicit;             /* 0 */
    float font_size;                   /* pixels at 1080: 72 (6..240) */
    int32_t font_weight;               /* 100..900: 700. A family without a face that
                                          heavy gets a synthetic bold */
    uint32_t text_color, sung_color;   /* 0xAARRGGBB: white, yellow */
    /* Placement */
    float vertical_position;           /* the active line's centre, 0 top .. 1 bottom: 0.5 */
    float box_width;                   /* of the area's width, for lines: 0.85 (0.2..1) */
    float line_pitch;                  /* em between line centres: 1.55 (0.8..4) */
    int32_t lines_above, lines_below;  /* 0..2: 2, 2 */
    float above_opacity[2];            /* the lines 1 and 2 above: 0.5, 0.25 */
    float next_line_opacity;           /* the line below: 0.65 */
    float after_next_factor;           /* the one below that: next_line_opacity times
                                          this, 0.55 */
    float near_scale, far_scale;       /* lines 1 and 2 away: 0.8, 0.6 (0.2..1) */
    /* The wipe */
    fa_lyric_wipe wipe;                /* Classic and Minimal: CROSSFADE; others: SWEEP */
    int32_t wipe_cap_ms;               /* a syllable's wipe takes wipe_share of the time to
                                          the next one, at most this: 600 (0..2000; 0 =
                                          instant) */
    float wipe_share;                  /* 0.8 (0.1..1) */
    int32_t instant_cut_ms;            /* shorter wipes are instant: 150 (0..1000) */
    int32_t lead_ms;                   /* syllables are coloured this early: 400 (0..2000) */
    /* Lines and pages */
    int32_t paragraph_gap_ms;          /* 2000 (0..60000) */
    int32_t sentence_gap_ms;           /* 900 (0..60000) */
    int32_t break_length;              /* characters; 0 = no rule: 42 (0..1000) */
    int32_t scroll_ms;                 /* a line scrolling up: 1500 (0..4000) */
    int32_t page_ms;                   /* a page fading out or in: 1000 (0..4000) */
    int32_t page_hold_ms;              /* a page stays this long after its last syllable
                                          when there is time: 2000 (0..10000) */
    int32_t page_preview_ms;           /* a new page is in place this early: 1000 (0..10000) */
    int32_t page_overlap;              /* 1: the fades may overlap, so new lyrics are never
                                          late; 0: the new page waits for the old to go */
    float page_in_scale;               /* a new page grows in from this: 1.5 (1..2) */
    float page_out_scale;              /* an old page shrinks to this: 0.75 (0.25..1) */
    int32_t end_hold_ms, end_fade_ms;  /* the lyrics fade out this long after the last
                                          syllable, over this: 5000, 1000 (0..60000) */
    /* The countdown before a line after a long gap */
    fa_lyric_countdown countdown;      /* CLOCK */
    int32_t countdown_count;           /* arrows: 4 (1..8) */
    int32_t countdown_ms;              /* CLOCK: the whole countdown, the arrows and a last
                                          fade: 5000 (1000..20000) */
    int32_t countdown_gap_ms;          /* after a gap this long: 6000 (0..60000) */
    uint32_t countdown_from, countdown_to; /* an arrow's colour before and after: red, green */
    float countdown_size;              /* em: 0.5 (0.2..1.5) */
    /* A panel behind each line */
    int32_t panel;                     /* 0 */
    uint32_t panel_color;              /* 0xAARRGGBB: black at 50 % */
    float panel_padding, panel_radius; /* em: 0.25, 0.2 (0..1) */
    int32_t subtitle_mode;             /* 1: no countdown, no intro or ending page */
    fa_surface_effects effects;        /* with sung colours; as fa_surface_set_effects */
    /* The intro and ending pages: the song's credits (fa_stream_get_midi_credits) on
     * smoked glass. The intro fades in at intro_start_ms; when the lyrics (or their
     * countdown) start before intro_band_ms it is a band at the top (title, artist and
     * year) and one at the bottom (length, key, tempo and time signature, copyright)
     * until intro_band_ms, else a card in the middle (also the writers and the
     * song's other notes) gone intro_clear_ms before the lyrics; with no time to fade
     * in and out before then, no intro. The ending card (title, artist, year,
     * copyright) follows the lyrics' fade when ending_min_ms of the song is left. A song naming neither title nor artist gets an intro with
     * fallback_title (the file's name, say) and no ending; with no fallback_title,
     * neither. */
    int32_t intro_page, ending_page;   /* 1, 1 */
    const char* fallback_title;        /* UTF-8; NULL */
    const char* labels;                /* UTF-8, one per line: "Length", "Key", "Tempo",
                                          "Timing", "Written By", "Lyricist"; NULL = these */
    int32_t intro_start_ms, intro_fade_ms; /* 1000, 1000 (0..60000) */
    int32_t intro_clear_ms;            /* 3000 (0..60000) */
    int32_t intro_band_ms;             /* 8000 (0..60000) */
    int32_t ending_min_ms, ending_fade_ms; /* 3000, 750 (0..60000) */
    uint32_t card_color;               /* the glass, 0xAARRGGBB: 0xE00E131C */
    uint32_t card_accent;              /* the border and the artist: 0xFF78DCE8 */
    uint32_t title_color, info_color, copyright_color; /* 0xFFFFF4DC, 0xFFD5DCE6, 0xFFB8A87A */
} fa_lyric_style;

/* The style of a preset. Strings and font_data are NULL (the UI font). */
FA_API void FA_CALL fa_lyric_style_init(fa_lyric_style* style, fa_lyric_preset preset);
/* A lyric surface for a MIDI stream; NULL style = Classic. FA_ERROR_INVALID_ARG for a
 * stream that is not MIDI or a style out of range, FA_ERROR_NOT_FOUND when no font
 * could be opened (font_path missing, or no system font), FA_ERROR_UNSUPPORTED_FORMAT
 * for font_data or a font_path that is not a font. A song without lyrics draws
 * nothing. Destroy it with fa_surface_destroy; it outlives its stream, drawing
 * nothing more once the stream is closed. */
FA_API fa_result FA_CALL fa_lyric_surface_create(fa_stream* stream, const fa_lyric_style* style, fa_surface** out_surface);
/* Changes the style: the lines are built again and the next frame is drawn in full
 * (the layout changes with sizes and fonts). Errors as fa_lyric_surface_create; on an
 * error the old style stays. fa_surface_set_effects sets style.effects alone. */
FA_API fa_result FA_CALL fa_lyric_surface_set_style(fa_surface* surface, const fa_lyric_style* style);
/* The style in use, up to out_style->struct_size. Its strings and font_data point into
 * the surface, valid until the next set_style or the surface is destroyed. */
FA_API fa_result FA_CALL fa_lyric_surface_get_style(fa_surface* surface, fa_lyric_style* out_style);

/* ---- Tracker modules ------------------------------------------------------
 *
 * MOD, S3M, XM, IT, MPTM, MTM, UMX (Unreal music) and the other formats
 * libopenmpt reads (669, AMF, AMS, DBM, DIGI, DMF, DSM, FAR, GDM, IMF, J2B, MDL,
 * MED, MT2, OKT, PLM, PSM, PTM, STM, ULT and more) open with the ordinary
 * fa_stream_open_* calls, detected from the content. They render to stereo F32
 * at the engine's mix_sample_rate, or 44100 Hz when that is 0 (device native),
 * like MIDI. libopenmpt is built in: no add-on library.
 *
 * The length is the song played once (the module's own loop back to its restart
 * position is not followed: fa_stream_set_loop loops the whole song, from the
 * start). Seeking lands on the exact frame: libopenmpt restores the state at the
 * row that holds the target, which is very close to playing through but not
 * always identical (an effect that depends on everything before it, such as a
 * long pitch slide, may sound slightly different for a moment).
 *
 * MO3 files whose samples are MP3 or Ogg Vorbis compressed are not supported
 * (FA_ERROR_UNSUPPORTED_FORMAT); MO3 files with plain samples play. */

typedef struct fa_tracker_info {
    uint32_t struct_size;        /* set to sizeof(fa_tracker_info) before the call */
    char type[16];               /* the format's usual extension: "mod", "xm", "it", "s3m", ... */
    char type_name[64];          /* "ProTracker MOD", "Impulse Tracker", ... */
    char tracker[64];            /* the program that saved it, when known */
    char title[128];             /* UTF-8, "" when it has none */
    char artist[128];
    uint32_t channels, orders, patterns, instruments, samples, subsongs;
    int32_t restart_order, restart_row;   /* where the song's own loop goes back to */
    double duration;             /* seconds, played once (the subsong playing) */
    /* Added after the first version: an older, shorter struct is filled up to here. */
    int32_t subsong;             /* the subsong playing: fa_open_options.audio_track, 0 by default */
} fa_tracker_info;

FA_API fa_result FA_CALL fa_stream_get_tracker_info(fa_stream* stream, fa_tracker_info* out_info);

typedef enum fa_tracker_text_type {
    FA_TRACKER_TEXT_MESSAGE = 0,     /* the song message (index 0), "" when it has none */
    FA_TRACKER_TEXT_INSTRUMENT = 1,  /* index 0..instruments-1 */
    FA_TRACKER_TEXT_SAMPLE = 2,      /* index 0..samples-1 */
    FA_TRACKER_TEXT_CHANNEL = 3,     /* index 0..channels-1 */
    FA_TRACKER_TEXT_PATTERN = 4,     /* index 0..patterns-1 */
    FA_TRACKER_TEXT_SUBSONG = 5      /* index 0..subsongs-1 */
} fa_tracker_text_type;

/* A text of the module in UTF-8: up to capacity - 1 bytes and a terminating 0
 * (cut at a character boundary); *out_length (optional) is its full length in
 * bytes, so a call with capacity 0 asks for the size. FA_ERROR_NOT_FOUND for an
 * index out of range; FA_ERROR_INVALID_ARG for streams that are not modules. */
FA_API fa_result FA_CALL fa_stream_get_tracker_text(fa_stream* stream, fa_tracker_text_type type, uint32_t index,
                                                    char* buffer, size_t capacity, size_t* out_length);

/* Order and row: the time (seconds from the start) where the song plays that row
 * first, and a seek there (fa_stream_seek of that frame). FA_ERROR_NOT_FOUND
 * when the song never plays it (a skipped order, a row past the pattern, or an
 * order of a later subsong: the stream plays the first subsong). */
FA_API fa_result FA_CALL fa_stream_get_tracker_time(fa_stream* stream, uint32_t order, uint32_t row, double* out_seconds);
FA_API fa_result FA_CALL fa_stream_seek_tracker(fa_stream* stream, uint32_t order, uint32_t row);

/* Render settings, live (heard after the decode-ahead buffer, 150 ms by default
 * for modules as for MIDI):
 * FA_TRACKER_INTERPOLATION       0 libopenmpt's default (8-tap sinc), 1 none, 2 linear, 4 cubic, 8 sinc
 * FA_TRACKER_STEREO_SEPARATION   percent, 0 (mono) .. 200; 100 by default
 * FA_TRACKER_VOLUME_RAMPING      -1 default, 0 off (may click), 1..10 softer
 * FA_TRACKER_AMIGA_RESAMPLER     0 (default) off; 1 Amiga MOD files sound as on an Amiga's Paula chip
 *                                (about 19 times the CPU on those files)
 * FA_TRACKER_ROW_EVENTS          0 (default) none; 1 FA_EVENT_TRACKER_ROW for every row heard;
 *                                2 only for the first row of each order
 * FA_ERROR_INVALID_ARG for other values and for streams that are not modules. */
typedef enum fa_tracker_param {
    FA_TRACKER_INTERPOLATION = 0,
    FA_TRACKER_STEREO_SEPARATION = 1,
    FA_TRACKER_VOLUME_RAMPING = 2,
    FA_TRACKER_AMIGA_RESAMPLER = 3,
    FA_TRACKER_ROW_EVENTS = 4
} fa_tracker_param;
FA_API fa_result FA_CALL fa_stream_set_tracker_param(fa_stream* stream, fa_tracker_param param, int32_t value);
FA_API fa_result FA_CALL fa_stream_get_tracker_param(fa_stream* stream, fa_tracker_param param, int32_t* out_value);

/* The order and row heard right now (at fa_stream_get_clock), to within about
 * 2 ms of the row's start. FA_ERROR_NOT_FOUND while that audio is not decoded
 * yet (a stream that was never played or read, or just after a seek). Cheap and
 * lock-free, like the clock: fine to call every video frame. */
FA_API fa_result FA_CALL fa_stream_get_tracker_position(fa_stream* stream, uint32_t* out_order, uint32_t* out_row);

/* Mutes or unmutes channel 0..channels-1 (the first 64), live.
 * FA_ERROR_NOT_IMPLEMENTED if libopenmpt offers no channel control for the module. */
FA_API fa_result FA_CALL fa_stream_set_tracker_channel_mute(fa_stream* stream, uint32_t channel, int32_t muted);
FA_API fa_result FA_CALL fa_stream_get_tracker_channel_mute(fa_stream* stream, uint32_t channel, int32_t* out_muted);

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
 * FA_EVENT_DEVICE_CHANGED  position               0 = moved to the new default, 1 = device lost, reopened on the default
 * FA_EVENT_LOUDNESS_FINAL  position               0
 * FA_EVENT_RAMP_DONE       position               fa_ramp.tag of the finished ramp
 * FA_EVENT_STARTED         position               0
 * FA_EVENT_MIDI_TEXT       the text's frame       index for fa_stream_get_midi_text
 * FA_EVENT_LOOPED          loop end it left       loop start it went to (CUE_IN or 0)
 * FA_EVENT_CDG_CHANGED     CD+G packs now shown   what changed: FA_CDG_CHANGE_* (CD+G above)
 * FA_EVENT_SEEK_FAILED     the target not reached the fa_result (fa_stream_seek_async)
 * FA_EVENT_TRACKER_ROW     the row's first frame  order << 16 | row (FA_TRACKER_ROW_EVENTS)
 *
 * Delivered today: ENDED, POSITION, MARKER, LOOPED, CDG_CHANGED (when asked for), LOUDNESS_FINAL, RAMP_DONE, STARTED, SEEK_FAILED and DEVICE_CHANGED; MIDI_TEXT from MIDI
 * streams played on a device (decode-only streams list them with fa_stream_get_midi_text); TRACKER_ROW from
 * tracker modules played on a device (when asked for); METADATA from web
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
    FA_EVENT_LOOPED,          /* a looping stream jumped back to its loop start */
    FA_EVENT_CDG_CHANGED,     /* the CD+G picture heard now changed (fa_stream_set_cdg_events) */
    FA_EVENT_SEEK_FAILED,     /* fa_stream_seek_async could not reach `frame` (value = the fa_result) */
    FA_EVENT_TRACKER_ROW,     /* a tracker module row is heard (FA_TRACKER_ROW_EVENTS) */
    FA_EVENT_INPUT_LOST,      /* a mixer mic's input device went away (fa_mixer_set_event_callback) */
    FA_EVENT_CUE_LOST,        /* a mixer's headphone device (FA_CUE_DEVICE) went away */
    FA_EVENT_AUTOMIX_STARTED, /* auto-mix: the incoming deck is heard (fa_mixer_set_automix) */
    FA_EVENT_AUTOMIX_DONE,    /* auto-mix: the transition is done */
    FA_EVENT_DECK_EMPTY       /* auto-mix: the idle deck needs its next song */
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

/* Raises FA_EVENT_POSITION once, the first time playback passes source frame
 * `frame` (frame = the requested frame, value 0); the request is then gone. Add it
 * again from the callback to hear about the next pass (a loop's next turn, or after
 * seeking back). Frame-exact like markers: it follows tempo, rate and loops, and is
 * raised as that audio is mixed, about one device latency before it is heard.
 * Unlike markers it is raised playing reversed too, when the position counts down
 * past the frame. A seek over the frame doesn't raise it: it stays armed. Streams
 * played on a device only (decode-only streams raise none).
 * Up to 64 frames are armed at once per stream; adding one that is already armed
 * does nothing. FA_FRAMES_UNSET removes all of the stream's armed frames.
 * FA_ERROR_INVALID_ARG for any other negative frame or one past a known length;
 * FA_ERROR_BUSY when 64 are armed. Best effort like markers: dropped in the rare
 * case that the device's event queue is full. */
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

/* Recording a DJ mixer: stems that line up. An encoder attached to a mixer tap records
 * nothing until fa_mixer_start_recording starts it, together with the others, on one
 * device frame, so the files line up in any editor (stereo at the mixer's rate).
 *   FA_MIXER_TAP_MASTER  what the speakers play, after the limiter (as
 *                        fa_encoder_attach_device on the mixer's device)
 *   FA_MIXER_TAP_VOCALS  both mics with their reverb and echo, before the master gain;
 *                        a mic with monitor off is recorded dry
 * The vocals start later by the mics' input latency plus the master's output latency
 * (fa_mixer_latency.mic_monitor, the larger of the two mics): the voice in the file
 * then lines up with the music as the singer heard it. FA_MIXER_REC_RAW_TIMING starts
 * them on the same frame instead (the voice as it reached the mixer). */
typedef enum fa_mixer_tap {
    FA_MIXER_TAP_MASTER = 0,
    FA_MIXER_TAP_VOCALS
} fa_mixer_tap;
#define FA_MIXER_REC_RAW_TIMING (1u << 0)
FA_API fa_result FA_CALL fa_encoder_attach_mixer(fa_encoder* encoder, fa_mixer* mixer, fa_mixer_tap tap);
/* Starts `encoders` (all attached to this mixer, none started yet: FA_ERROR_INVALID_ARG,
 * FA_ERROR_BUSY) at `device_time` on the mixer device's clock (fa_device_get_time); 0,
 * or a time less than one mixer block (2048 frames, about 43 ms at 48 kHz)
 * ahead, starts that one block ahead. */
FA_API fa_result FA_CALL fa_mixer_start_recording(fa_mixer* mixer, fa_encoder* const* encoders, uint32_t count,
                                                  fa_time device_time, uint32_t flags);
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
    FA_EQ_PRESET_LIVE_VOCAL,   /* a live singing mic: less boom and mud, more presence and air */
    FA_EQ_PRESET_COUNT
} fa_eq_preset;

#define FA_EQ_PRESET_BANDS 10

/* Fills out_bands (room for FA_EQ_PRESET_BANDS) and *out_count. */
FA_API fa_result FA_CALL fa_eq_get_preset(fa_eq_preset preset, fa_eq_band* out_bands, uint32_t* out_count);
/* English display name ("Rock"), NULL for an unknown preset. */
FA_API const char* FA_CALL fa_eq_preset_name(fa_eq_preset preset);

/* ---- DJ mixer: microphones ------------------------------------------ */

/* Up to two karaoke mics, mixed into the master before the master gain and the
 * limiter:
 *
 *   input device -> bridge (drift-corrected) -> low cut -> gain -> gate -> EQ -> meter
 *     -> mute -> pan -> master (when monitored), and sends into a shared reverb and echo
 *   host mics (FA_MIC_DUCKS_MUSIC) duck the music; the vocals can be recorded
 *
 * A mic reads one channel of a capture device (or all its channels summed). Two mics
 * on one 2-input interface share the device. The input runs on its own clock, so it
 * reaches the master through a small buffer that a resampler keeps at a steady fill,
 * whatever the two clocks' drift or rates; fa_mixer_get_latency reports what that
 * and the devices add. Mics ask the OS for unprocessed capture (no automatic gain,
 * noise suppression or echo cancelling, which spoil singing and add latency) where
 * the OS offers it: Windows' RAW capture stream (on drivers that support it) and
 * Android's UNPROCESSED input preset; elsewhere the input as the OS gives it. The app asks for the microphone permission (Android manifest and prompt,
 * macOS and iOS usage strings); without it adding a mic is FA_ERROR_PERMISSION_DENIED
 * (on systems that refuse at open; some deliver silence instead). An input that goes
 * away goes silent and raises FA_EVENT_INPUT_LOST; the mix carries on. */
typedef struct fa_mic fa_mic;

#define FA_MIC_MAX           2
#define FA_MIC_DUCKS_MUSIC   (1u << 0)  /* a host mic: its voice ducks the music (fa_mixer_set_ducking);
                                           leave it off for singers */
#define FA_MIC_OS_PROCESSING (1u << 1)  /* keep the OS's voice processing (calls, not singing); two
                                           mics on one device share its input, so the first mic's
                                           choice holds for both */

typedef struct fa_mic_config {
    uint32_t    struct_size;  /* set by fa_mic_config_init */
    const char* device_id;    /* the input (fa_device_info.id of an FA_DEVICE_CAPTURE device),
                                 NULL = the default input */
    int32_t     channel;      /* the input channel, from 0; -1 = all channels summed (default) */
    int32_t     monitor;      /* 1 = heard through the master (default); 0 = metered (and, with
                                 the recording step, recorded) only: for a hardware mixer */
    uint32_t    flags;        /* FA_MIC_* */
} fa_mic_config;

FA_API void      FA_CALL fa_mic_config_init(fa_mic_config* config);
/* At most FA_MIC_MAX mics per mixer (FA_ERROR_BUSY for a third). The mic fades in once
 * its input runs. FA_ERROR_NOT_ENOUGH_CHANNELS when `channel` is not on the device. */
FA_API fa_result FA_CALL fa_mixer_add_mic(fa_mixer* mixer, const fa_mic_config* config, fa_mic** out_mic);
/* Removes the mic, and closes its input device when no other mic uses it; also from the
 * mixer's event callback, the event's own mic included. fa_mixer_destroy removes the
 * mixer's mics. */
FA_API void      FA_CALL fa_mic_remove(fa_mic* mic);
/* The mic's slot, 0 or 1 (fa_mixer_latency.mic_*, fa_mixer_event.index). */
FA_API fa_result FA_CALL fa_mic_get_index(fa_mic* mic, uint32_t* out_index);

typedef enum fa_mic_param {
    FA_MIC_GAIN_DB = 0,  /* -20..+40, default 0 */
    FA_MIC_MUTE,         /* 1 = muted, 0 = not (default); glides over 5 ms */
    FA_MIC_PAN,          /* -1 (left) .. 1 (right), default 0; balance, as stream pan */
    FA_MIC_GATE_DB,      /* the gate's threshold, -100 (off) .. 0, default -50: opens at the
                            threshold, closes 6 dB below it after 100 ms, so breaths and room
                            noise stay out between phrases */
    FA_MIC_REVERB_SEND,  /* 0..1, default 0.2: into the shared reverb (fa_mixer_set_mic_effects) */
    FA_MIC_ECHO_SEND,    /* 0..1, default 0: into the shared echo */
    FA_MIC_MONITOR,      /* 1 = heard through the master, 0 = not (from fa_mic_config.monitor) */
    FA_MIC_LOW_CUT       /* 1 = an 80 Hz high-pass against handling noise and rumble (default),
                            0 = off */
} fa_mic_param;
/* Sets glide over 5 ms. Out-of-range values are clamped. */
FA_API fa_result FA_CALL fa_mic_set_param(fa_mic* mic, fa_mic_param param, float value);
FA_API fa_result FA_CALL fa_mic_get_param(fa_mic* mic, fa_mic_param param, float* out_value);
/* The mic's EQ: the same bands, limits and presets as a stream's (fa_stream_set_eq,
 * fa_eq_get_preset; FA_EQ_PRESET_LIVE_VOCAL is made for mics). Off by default. */
FA_API fa_result FA_CALL fa_mic_set_eq(fa_mic* mic, const fa_eq_band* bands, uint32_t count);
FA_API fa_result FA_CALL fa_mic_get_eq(fa_mic* mic, fa_eq_band* out_bands, uint32_t capacity, uint32_t* out_count);
/* The mic's meter: after gain, gate and EQ, before mute and pan, as heard now. */
FA_API fa_result FA_CALL fa_mic_get_levels(fa_mic* mic, fa_time window, float* out_peak, float* out_rms);

/* What the mixer's outputs and inputs add up to, in ns. Times the OS does not report
 * are estimated from the device's buffer sizes. */
typedef enum fa_cue_align {
    FA_CUE_ALIGN_NONE = 0,   /* nothing delayed: no second-device cue (or no headphones open
                                yet), or the two outputs already line up (within 1 ms) */
    FA_CUE_ALIGN_CUE,        /* the headphones are delayed by cue_alignment */
    FA_CUE_ALIGN_MASTER,     /* the master is delayed by cue_alignment (part of `master`) */
    FA_CUE_ALIGN_SKIPPED,    /* the master is the faster output but a mic is monitored, so it is
                                not delayed: the headphones are cue - master late */
    FA_CUE_ALIGN_OFF         /* FA_MIXER_CUE_LOWEST_LATENCY */
} fa_cue_align;

typedef struct fa_mixer_latency {
    uint32_t struct_size;              /* sizeof(fa_mixer_latency): filled up to it */
    fa_time  master;                   /* the master output: the OS's buffer, the limiter's
                                          look-ahead and any alignment delay */
    fa_time  cue;                      /* the headphones: the OS's buffer (the cue is not
                                          limited); for FA_CUE_DEVICE also the bridge and any
                                          alignment delay */
    fa_time  cue_alignment;            /* the delay added to line up a second-device cue */
    fa_time  mic_input[FA_MIC_MAX];    /* input buffer plus the bridge; -1 = no mic in the slot */
    fa_time  mic_monitor[FA_MIC_MAX];  /* mic to speaker: measured by fa_mixer_measure_latency in
                                          the mixer's mode when it has been, else mic_input plus
                                          master; -1 = no mic. Includes a delay on the master */
    int32_t  cue_align;                /* fa_cue_align: which output cue_alignment delays */
} fa_mixer_latency;
FA_API fa_result FA_CALL fa_mixer_get_latency(fa_mixer* mixer, fa_mixer_latency* out_latency);

/* The setup check, for the app's setup screen: plays short test sounds through the
 * speakers, listens on `mic`, and says how long the mic takes to reach the speakers,
 * how much mic gain the room takes before it feeds back, and whether to monitor the
 * mics at all. Put the mic where the singer will stand. It blocks for about 3 s per
 * mode measured (call it from a worker thread); the music and the mics are silenced
 * while the test sounds play and come back after. The sounds start quiet (-40 dBFS)
 * and get louder until the mic hears them, up to max_level_db. Keep the mic in the
 * mixer until it returns.
 *
 * Afterwards the mixer uses the measured time for that mic and mode (mic_monitor
 * above, and the vocals' alignment in fa_mixer_start_recording) instead of the OS's
 * figures, until the mic is removed. */
#define FA_CHECK_ALL_MODES (1u << 0)  /* measure each latency mode the system has, switching the
                                         mixer to it (fa_mixer_set_latency_mode) and back after;
                                         else only the mixer's own mode */
typedef struct fa_latency_check_config {
    uint32_t struct_size;    /* set by fa_latency_check_config_init */
    uint32_t flags;          /* FA_CHECK_* */
    float    max_level_db;   /* the loudest test sound, dBFS, -40..-6, default -12. In exclusive
                                mode the Windows volume no longer applies, so the same level
                                can be much louder than in shared mode */
    fa_time  max_monitor;    /* mic to speaker that still feels live, default 25 ms */
} fa_latency_check_config;

typedef struct fa_latency_check_mode {
    fa_latency_mode mode;
    fa_result result;        /* FA_OK; FA_ERROR_NOT_IMPLEMENTED (not on this system),
                                FA_ERROR_DEVICE (it would not open, or the output isn't
                                running), FA_ERROR_NOT_FOUND (the mic didn't hear the test) */
    fa_time  mic_to_speaker; /* measured: the input, the bridge, the limiter, the output and
                                the air from the speaker back to the mic (3 ms per metre) */
    fa_time  spread;         /* how far the test sounds' times differed */
    fa_time  reported;       /* what the OS's figures add up to, for comparison */
    float    max_gain_db;    /* the most FA_MIC_GAIN_DB that stays 6 dB clear of feedback, with
                                the master gain as it is now and the mic's EQ flat (an EQ
                                boost takes that much off); -20..40 */
    float    level_db;       /* the test level the mic heard */
} fa_latency_check_mode;

typedef enum fa_check_advice {
    FA_CHECK_MONITOR = 0,    /* monitor the mics, in advised_mode */
    FA_CHECK_SLOW,           /* monitor off: every mode measured is over max_monitor (an
                                audible slap-back); advised_mode is the fastest */
    FA_CHECK_FEEDBACK,       /* monitor off: the room feeds back below 0 dB of mic gain;
                                move the mic away from the speakers, or point it away */
    FA_CHECK_NOT_HEARD       /* the mic didn't hear the speakers: is it on and unmuted, are
                                the speakers up, is it a headset? */
} fa_check_advice;

typedef struct fa_latency_check {
    uint32_t struct_size;            /* sizeof(fa_latency_check): filled up to it */
    uint32_t count;                  /* entries in modes */
    fa_latency_check_mode modes[3];  /* normal, low, exclusive (or only the mixer's mode) */
    fa_check_advice advice;
    fa_latency_mode advised_mode;    /* the cheapest mode fast enough: shared before
                                        exclusive, which takes the devices from other apps */
} fa_latency_check;

FA_API void      FA_CALL fa_latency_check_config_init(fa_latency_check_config* config);
/* FA_ERROR_BUSY while another check runs on the mixer. When only the mixer's own mode
 * is measured and it fails other than by not being heard, that is the result. With
 * FA_CHECK_ALL_MODES, when the mixer can't go back to its own mode afterwards it stays in
 * the last mode measured and the result is that error (out_result is still filled in);
 * otherwise FA_OK with each mode's result inside. */
FA_API fa_result FA_CALL fa_mixer_measure_latency(fa_mixer* mixer, fa_mic* mic, const fa_latency_check_config* config,
                                                  fa_latency_check* out_result);

/* One shared reverb and one echo for both mics, like the effects unit of a karaoke
 * mixer. Each mic sends to them (FA_MIC_REVERB_SEND, FA_MIC_ECHO_SEND) and their returns
 * join the mics in the master, in the middle. They take what is heard: a mic with
 * monitor off sends nothing (and is recorded dry). */
typedef struct fa_mic_effects {
    uint32_t struct_size;    /* set by fa_mic_effects_init */
    float reverb_size;       /* 0 (small room) .. 1 (hall), default 0.5 */
    float reverb_damping;    /* 0 (bright) .. 1 (dark), default 0.5 */
    float echo_time_ms;      /* 20..1000, default 250; a change glides, bending the repeats'
                                pitch for a moment as a tape echo does */
    float echo_feedback;     /* 0..0.9, default 0.35: how much of each repeat comes back */
    float echo_tone;         /* 0 (dark) .. 1 (bright), default 0.6: a low-pass in the
                                feedback, 1 kHz .. 16 kHz, so later repeats get duller */
} fa_mic_effects;
FA_API void      FA_CALL fa_mic_effects_init(fa_mic_effects* effects);
/* Out-of-range values are clamped. */
FA_API fa_result FA_CALL fa_mixer_set_mic_effects(fa_mixer* mixer, const fa_mic_effects* effects);
FA_API fa_result FA_CALL fa_mixer_get_mic_effects(fa_mixer* mixer, fa_mic_effects* out_effects);

/* Ducking: while a mic flagged FA_MIC_DUCKS_MUSIC (a host) speaks above the threshold,
 * the music (the decks after the crossfader, and other streams on the mixer's device)
 * goes down by depth_db; it comes back after hold and release. Singers are not
 * flagged, so the music never dips under a singer. A muted host does not duck; a host
 * with monitor off still does. On with these defaults from the start; NULL turns it
 * off (depth 0). */
typedef struct fa_ducking {
    uint32_t struct_size;    /* set by fa_ducking_init */
    float    depth_db;       /* -40..0, default -12; 0 = off */
    fa_time  attack;         /* 1 ms..1 s, default 50 ms: how fast the music goes down */
    fa_time  hold;           /* 0..5 s, default 300 ms: how long it stays down after the voice */
    fa_time  release;        /* 10 ms..5 s, default 600 ms: how fast it comes back */
    float    threshold_db;   /* -60..0, default -40: the voice level that ducks */
} fa_ducking;
FA_API void      FA_CALL fa_ducking_init(fa_ducking* ducking);
FA_API fa_result FA_CALL fa_mixer_set_ducking(fa_mixer* mixer, const fa_ducking* ducking);
FA_API fa_result FA_CALL fa_mixer_get_ducking(fa_mixer* mixer, fa_ducking* out_ducking);

/* Mixer events. They run on the engine's event thread, like stream events, and may call
 * any FatzAudio function. fa_mixer_set_event_callback, fa_mic_remove and
 * fa_mixer_destroy return once no callback for the mixer is running (unless called
 * from inside one), so `user` can then be freed.
 *
 * type                 index       mic                        value
 * FA_EVENT_INPUT_LOST  the slot    the mic (valid in the call) 0
 * FA_EVENT_CUE_LOST    0           NULL                       0
 * FA_EVENT_AUTOMIX_STARTED, FA_EVENT_AUTOMIX_DONE
 *                      the incoming deck  NULL                the incoming deck
 * FA_EVENT_DECK_EMPTY  the idle deck  NULL                    0 */
typedef struct fa_mixer_event {
    fa_event_type type;
    fa_mixer* mixer;
    uint32_t index;
    fa_mic* mic;
    int64_t value;
} fa_mixer_event;
typedef void (FA_CALL *fa_mixer_event_callback)(const fa_mixer_event* event, void* user);
FA_API fa_result FA_CALL fa_mixer_set_event_callback(fa_mixer* mixer, fa_mixer_event_callback callback, void* user);

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

/* Peak and RMS (linear, 1.0 = full scale) of the last `window` of audio being
 * heard now: for a VU meter, call it from a timer. channels 1 gives one value for
 * both sides, 2 gives left and right. A stream's levels are of its own audio,
 * after its equalizer and DSP, before volume, pan, fades and normalization (as
 * BASS_ChannelGetLevel); a device's are of its whole output after the limiter.
 * The window ends at the audio the speaker plays now. Zeros while the stream is
 * not playing or the device is stopped. The first call starts recording, so it
 * reads silence; a window over about 1 s is shortened. Either output may be NULL.
 * Decode-only streams: FA_ERROR_INVALID_ARG (measure what fa_stream_read gives). */
FA_API fa_result FA_CALL fa_stream_get_levels(fa_stream* stream, fa_time window, float* out_peak, float* out_rms, uint32_t channels);
FA_API fa_result FA_CALL fa_device_get_levels(fa_device* device, fa_time window, float* out_peak, float* out_rms, uint32_t channels);

typedef enum fa_spectrum_flags {
    FA_SPECTRUM_LEFT      = 1 << 0,  /* the left channel only (default: both, mixed) */
    FA_SPECTRUM_RIGHT     = 1 << 1,  /* the right channel only */
    FA_SPECTRUM_NO_WINDOW = 1 << 2   /* no Hann window (BASS_FFT_NOWINDOW) */
} fa_spectrum_flags;

/* The magnitude spectrum of the last `size` frames being heard now, for a
 * visualizer: `size` is a power of two from 64 to 16384, and `out_bins` gets
 * size / 2 values, bin k centred on k * bin_hz (DC first). A full-scale sine
 * reads 1.0 in its bin (lower when it falls between two). Same source, timing
 * and zeros as the levels above; out_bin_hz (may be NULL) is the device rate
 * divided by size, 0 while nothing plays. Bigger sizes give finer
 * frequencies and slower response: 2048 at 48 kHz is 23.4 Hz per bin over 43 ms. */
FA_API fa_result FA_CALL fa_stream_get_spectrum(fa_stream* stream, uint32_t size, uint32_t flags, float* out_bins, float* out_bin_hz);
FA_API fa_result FA_CALL fa_device_get_spectrum(fa_device* device, uint32_t size, uint32_t flags, float* out_bins, float* out_bin_hz);

#ifdef __cplusplus
}
#endif

#endif /* FATZAUDIO_H */
