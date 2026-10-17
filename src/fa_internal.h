#ifndef FA_INTERNAL_H
#define FA_INTERNAL_H

#include "fatzaudio/fatzaudio.h"
#include "miniaudio.h"
#include "fa_platform.h"
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
    fa_result error;   /* first read failure of the app's callbacks or the file, else FA_OK */
    int prefix_filled;
    size_t prefix_len;
    uint8_t prefix[FA_PROBE_BYTES];
} fa_source;

fa_result fa_source_open_path(fa_source* src, const char* path);
fa_result fa_source_open_memory(fa_source* src, const void* data, size_t size);
/* Like fa_source_open_memory but without a copy: `data` must outlive the source. */
fa_result fa_source_open_memory_ref(fa_source* src, const void* data, size_t size);
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
    /* 1 if seeking costs about the same wherever it lands (an index or binary
     * search), so the loudness scanner can split files into chunks. 0 if a seek
     * decodes from the start (MP3 without a seek table): scanned in one pass. */
    int fast_seek;
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

/* dr_wav glue (miniaudio_impl.c: dr_wav's API is only visible there).
 * `sequential` = stop at the audio data, for sources that cannot seek back. */
fa_result fa_drwav_open(fa_source* src, int sequential, void** out_impl,
                        uint32_t* out_rate, uint32_t* out_channels, uint64_t* out_frames);
uint64_t fa_drwav_read(void* impl, float* out, uint64_t frames);
int fa_drwav_seek(void* impl, uint64_t frame);
void fa_drwav_close(void* impl);

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

/* ---- Ring buffer (ring.c): single producer (worker), single consumer (audio) */

typedef struct fa_ring {
    float* data;
    uint32_t channels;
    int64_t capacity;     /* frames */
    fa_atomic64 write;    /* total frames written (producer) */
    fa_atomic64 read;     /* total frames read (consumer) */
} fa_ring;

fa_result fa_ring_init(fa_ring* r, uint32_t channels, int64_t capacity);
void fa_ring_free(fa_ring* r);
int64_t fa_ring_readable(fa_ring* r);
int64_t fa_ring_writable(fa_ring* r);
void fa_ring_write(fa_ring* r, const float* src, int64_t frames); /* frames <= writable */
/* Consumer: up to two contiguous regions holding `frames` readable frames. */
void fa_ring_regions(fa_ring* r, int64_t frames, const float** p1, int64_t* n1, const float** p2, int64_t* n2);
void fa_ring_advance_read(fa_ring* r, int64_t frames);
/* Only while neither side can run (stream flushing and decode lock held). */
void fa_ring_reset(fa_ring* r);

/* ---- Curves (curve.c) ---------------------------------------------------- */

/* Shape of a ramp at progress t (0..1) -> 0..1. `rising` tells the equal-power
 * curve whether the value goes up (sin) or down (1 - cos). */
float fa_curve_eval(int curve, const float bezier[4], float t, int rising);

/* ---- Loudness (loudness.c) ------------------------------------------------ */

#define FA_TP_PHASES 4
#define FA_TP_TAPS 12
#define FA_RUNNING_SUBS 150     /* running estimate window: 15 s of 100 ms sub-blocks */

/* Polyphase true-peak detector (BS.1770 Annex 2 style). */
typedef struct fa_truepeak {
    uint32_t channels;
    int phases;                 /* 4 below 88.2 kHz, 2 below 176.4 kHz, else 1 */
    float coef[FA_TP_PHASES][FA_TP_TAPS];
    float bound;                /* largest sum of |coef| of a phase */
    float* hist;                /* per channel: 2 x FA_TP_TAPS */
    int pos;
    float peak;                 /* largest |value| measured so far, linear */
} fa_truepeak;

fa_result fa_truepeak_init(fa_truepeak* tp, uint32_t rate, uint32_t channels);
void fa_truepeak_free(fa_truepeak* tp);
/* One interleaved frame in; the largest |interpolated value| it completes out. */
float fa_truepeak_push(fa_truepeak* tp, const float* frame);

/* K-weighted energy meter: one mean-square energy per 100 ms sub-block. */
typedef struct fa_meter {
    uint32_t rate, channels;
    double b[2][3], a[2][3];
    double* z;                  /* filter state, 4 per channel */
    double* weight;             /* channel weights */
    int64_t sublen, subfill;
    double subacc;
    double* subs;               /* sub-block energies */
    int64_t nsubs, cap;         /* ring mode: nsubs counts every sub-block ever */
    int ring;
    int64_t skip;               /* frames to filter without measuring (warm-up) */
    int with_peak;
    fa_truepeak tp;
} fa_meter;

/* ring_subs > 0 keeps only the newest ring_subs sub-blocks (running estimate). */
fa_result fa_meter_init(fa_meter* m, uint32_t rate, uint32_t channels, int with_peak, int64_t ring_subs);
void fa_meter_free(fa_meter* m);
void fa_meter_reset(fa_meter* m);
fa_result fa_meter_process(fa_meter* m, const float* interleaved, int64_t frames);
double fa_meter_running_lufs(const fa_meter* m);   /* ring mode; NAN until 400 ms measured */
double fa_meter_peak_dbtp(const fa_meter* m);
double fa_loudness_integrated(const double* subs, int64_t n); /* -HUGE_VAL for silence */
double fa_loudness_range(const double* subs, int64_t n);

/* ---- Gain tags (tags.c) ----------------------------------------------------- */

/* Loudness stored in ReplayGain 2 / R128 tags, converted to LUFS. NAN = absent. */
typedef struct fa_tag_loudness {
    double track_lufs, album_lufs;
    double track_peak_db, album_peak_db;   /* sample peak in dBFS */
} fa_tag_loudness;

/* Reads ID3v2 (TXXX), FLAC Vorbis comments and APEv2. Moves the read position. */
void fa_tags_read_loudness(fa_source* src, fa_tag_loudness* out);
/* A Vorbis comment block (FLAC, Ogg Vorbis/Opus): vendor, count, "KEY=value" items. */
void fa_tags_parse_vorbis_comments(const uint8_t* data, size_t size, fa_tag_loudness* out);

/* ---- Loudness scans (scan.c) ----------------------------------------------- */

typedef struct fa_scan_job fa_scan_job;

/* Starts the background analysis of a stream (tags, then cache, then a chunked
 * scan on the scan threads). Results arrive via fa_stream_loudness_ready. */
fa_scan_job* fa_scan_start_stream(struct fa_stream* s, int use_tags);
/* Cancels if still running, waits until no scan thread uses it, frees it. */
void fa_scan_release(fa_scan_job* job);
void fa_scan_shutdown(fa_engine* engine);
/* Gain normalization gives a stream of loudness `lufs` with the engine's current settings. */
float fa_engine_norm_gain_db(fa_engine* engine, double lufs);

/* ---- Limiter (limiter.c) ----------------------------------------------------- */

/* Look-ahead true-peak limiter on a device's output. */
typedef struct fa_limiter {
    uint32_t channels;
    int attack;                 /* box-filter length, frames */
    int window;                 /* sliding-minimum window, frames */
    int delay;                  /* audio delay, frames */
    float* audio;               /* delay line, `delay` frames */
    int apos;
    float* box;                 /* last `attack` minima */
    int bpos;
    double box_sum;
    int64_t* dq_idx;            /* monotonic deque for the sliding minimum */
    float* dq_val;
    int dq_head, dq_count, dq_cap;
    int64_t n;
    float gain;                 /* after release smoothing */
    float release;              /* per-frame release coefficient */
    fa_truepeak tp;
    float reduction;            /* smallest gain of the last block (for metering) */
} fa_limiter;

fa_result fa_limiter_init(fa_limiter* l, uint32_t rate, uint32_t channels);
void fa_limiter_free(fa_limiter* l);
/* In place. ceiling <= 0 disables limiting (the delay stays, so toggling is click-free). */
void fa_limiter_process(fa_limiter* l, float* frames, uint32_t count, float ceiling);

/* ---- Events (event.c) ----------------------------------------------------- */

#define FA_EVENT_QUEUE_SIZE 1024 /* power of two */
#define FA_EVENT_SPILL 6          /* per stream: 2 params x (replaced + done) + ENDED, plus slack */

/* An event plus its engine-wide sequence number. Events from the audio thread and
 * from API threads travel different paths; the event thread sorts each round by
 * `seq`, so callbacks for a stream arrive in the order the events happened. */
typedef struct fa_qevent {
    fa_event ev;
    int32_t seq;
} fa_qevent;

/* Single producer (audio thread), single consumer (event thread). */
typedef struct fa_event_queue {
    fa_qevent items[FA_EVENT_QUEUE_SIZE];
    fa_atomic32 head;
    fa_atomic32 tail;
} fa_event_queue;

int fa_event_queue_push(fa_event_queue* q, const fa_qevent* ev); /* 0 if full */
int fa_event_queue_pop(fa_event_queue* q, fa_qevent* out);

/* From the audio thread: never blocks or allocates. If the device queue is full the
 * event goes to the stream's spill slots, which the event thread also drains, so
 * ENDED and RAMP_DONE are not lost (a RampAsync waits on them). */
void fa_audio_post_event(struct fa_device* d, struct fa_stream* s, fa_event_type type, int64_t frame, int64_t value);

/* From any non-audio thread. The _locked form takes a sequence number the caller
 * got with fa_fetch_add32(&engine->event_seq, 1) while holding list_lock. */
void fa_engine_post_event(fa_engine* engine, const fa_event* ev);
void fa_engine_post_event_locked(fa_engine* engine, const fa_event* ev, int32_t seq);

/* Moves events from the device queues and spill slots to the pending list, so a
 * slow event callback cannot let them fill up. Caller holds list_lock; decode
 * workers and the event thread both call it. Returns how many were moved. */
uint32_t fa_event_collect_locked(fa_engine* e);

/* Moves a device's queued events (and a stream's spilled ones) to the engine's
 * pending list. Caller holds list_lock and the audio thread must not be using them. */
void fa_event_rescue_device(fa_engine* e, struct fa_device* d);
void fa_event_rescue_stream(fa_engine* e, struct fa_stream* s);

/* ---- Engine, devices, streams ------------------------------------------- */

#define FA_MAX_WORKERS 8
#define FA_MIX_PARAMS 2          /* VOLUME and PAN are applied by the mixer */

struct fa_engine {
    fa_engine_config config;
    ma_context context;
    ma_mutex lock;               /* guards device enumeration scratch (ma_context device lists) */

    fa_mutex list_lock;          /* streams, devices, graveyard, pending events */
    fa_cond work_cond;           /* wakes decode workers */
    fa_cond event_cond;          /* wakes the event thread */
    struct fa_stream* streams;   /* attached streams the workers keep filled */
    struct fa_device* devices;
    struct fa_stream* graveyard; /* closed streams, freed by the event thread */
    fa_qevent* pending;          /* events posted by API/worker threads */
    uint32_t pending_count, pending_cap;

    fa_thread workers[FA_MAX_WORKERS];
    uint32_t worker_count;
    fa_thread event_thread;
    fa_atomic64 event_thread_id;
    fa_atomic32 event_seq;       /* next fa_qevent.seq */
    fa_atomic32 quit;
    int destroy_on_event_thread; /* fa_engine_destroy ran inside a callback: the event
                                    thread finishes the teardown when it exits */

    /* Normalization and limiter settings, read by the audio thread. */
    fa_atomic32 norm_mode;       /* fa_normalize_mode */
    fa_atomic32 norm_target;     /* float LUFS */
    fa_atomic32 limiter_mode;    /* fa_limiter_mode */
    fa_atomic32 limiter_ceiling; /* float dBTP */

    /* Loudness scan threads (scan.c) */
    fa_mutex scan_lock;
    fa_cond scan_cond;           /* work queued or a job finished */
    fa_scan_job* scan_queue;
    fa_thread* scan_threads;
    uint32_t scan_thread_count;
    int scan_started;
};

typedef struct fa_voice_list {
    uint32_t count;
    struct fa_stream* items[1];
} fa_voice_list;

struct fa_device {
    fa_engine* engine;
    ma_device device;
    uint32_t rate;
    fa_mutex lock;               /* serializes voice list edits (API threads only) */
    fa_atomic_ptr voices;        /* fa_voice_list*, read by the audio callback */
    fa_atomic64 epoch;           /* odd while the audio callback runs */
    fa_event_queue events;       /* audio callback -> event thread */
    fa_atomic32 spilled;         /* 1 = some stream on this device used its spill slots */
    fa_atomic32 publishing;      /* audio thread is between taking a sequence number and publishing */
    fa_limiter limiter;          /* audio thread only */
    struct fa_device* next;      /* engine->devices */
};

/* One parameter change, published to the audio thread through a 2-slot mailbox.
 * `word` holds the sequence number of the newest command plus FA_CMD_CLAIMED once
 * the audio thread took it; a single atomic exchange/CAS on it decides who reports
 * a replaced ramp, so every ramp gets exactly one RAMP_DONE. */
#define FA_CMD_CLAIMED 0x40000000

typedef struct fa_param_slot {
    fa_atomic32 target;          /* float bits */
    fa_atomic64 frames_ns;       /* duration in ns */
    fa_atomic32 curve;
    fa_atomic32 bezier[4];       /* float bits */
    fa_atomic64 tag;
    fa_atomic32 notify;          /* 1 = ramp (raise RAMP_DONE), 0 = plain set */
    fa_atomic32 has_start;       /* 1 = jump to `start` first: the value of a set this */
    fa_atomic32 start;           /*     command replaced before the mixer applied it */
} fa_param_slot;

typedef struct fa_param_mailbox {
    fa_atomic32 word;
    fa_param_slot slot[2];
    /* API side, under api_lock: what the newest command was. */
    int last_notify;
    int64_t last_tag;
    float last_target;
    int last_has_start;
    float last_start;
} fa_param_mailbox;

/* Audio-thread state of one parameter. */
typedef struct fa_param_voice {
    float value;
    int active;
    float start, target;
    int64_t total, done;         /* frames */
    int curve;
    float bezier[4];
    int notify;
    int64_t tag;
} fa_param_voice;

struct fa_stream {
    fa_engine* engine;
    uint32_t flags;
    uint32_t buffer_ms;
    fa_mutex api_lock;           /* serializes API calls that reconfigure the stream */
    fa_mutex decode_lock;        /* held while decoding (worker or API); never by the audio thread */
    fa_mutex cb_lock;            /* event callback + user */

    fa_source source;
    fa_decoder decoder;
    int decoder_ready;

    fa_atomic32 state;           /* fa_stream_state */
    fa_atomic64 position;        /* source frames, when not attached */
    fa_frames markers[FA_MARKER_COUNT];

    /* Decode-ahead, valid while attached. Worker-owned under decode_lock. */
    fa_device* device;           /* written under api_lock */
    uint32_t dev_rate;
    fa_ring ring;
    ma_data_converter converter;
    int converter_ready;
    float* in_buf;
    fa_frames in_cap, in_count, in_offset;
    fa_frames decoded;           /* source frames read from the decoder since seek_base */
    int draining;                /* the source ended; in_buf holds silence that flushes the resampler */
    float* out_buf;
    fa_frames out_cap;
    fa_atomic32 eof;             /* the worker has written the last frame */
    fa_atomic32 flushing;        /* 1 = the audio thread must not touch the ring */
    fa_atomic64 seek_base;       /* source frame at the start of the ring's data */
    fa_atomic64 played;          /* device frames consumed since seek_base (audio thread) */
    fa_atomic32 underruns;

    /* Parameters */
    fa_param_mailbox mailbox[FA_MIX_PARAMS];
    fa_param_voice voice[FA_MIX_PARAMS]; /* audio thread only */
    uint32_t voice_rate;                 /* device rate the ramp counters are in */
    int was_audible;                     /* audio thread only: mixed in the previous callback */
    fa_atomic32 current[FA_MIX_PARAMS];  /* float bits, published by the audio thread */

    /* Normalization */
    char* path;                  /* for scans of path streams; NULL otherwise */
    int32_t audio_track;
    fa_mutex norm_lock;          /* scan_job, loud, loud_source */
    fa_scan_job* scan_job;
    int analysis_tried;
    fa_loudness loud;            /* final measurement (valid when loud.is_final) */
    fa_atomic32 norm_track;      /* float LUFS used for gain, NAN = unknown */
    fa_atomic32 norm_album;      /* float LUFS from album tags, NAN = unknown */
    fa_atomic32 norm_est;        /* float LUFS running estimate, NAN = none yet */
    fa_meter run_meter;          /* running estimate; worker-owned under decode_lock */
    int run_meter_ready;
    float norm_db;               /* audio thread: gain being applied, dB */

    /* Events */
    fa_event_callback callback;
    void* callback_user;
    fa_atomic32 in_callback;
    fa_atomic32 closed;
    fa_qevent spill[FA_EVENT_SPILL];     /* written by the audio thread when the queue is full */
    fa_atomic32 spill_full[FA_EVENT_SPILL];

    struct fa_stream* next;      /* engine->streams or engine->graveyard */
};

/* device.c */
void fa_device_wait_quiescent(fa_device* device);
fa_result fa_device_add_voice(fa_device* device, struct fa_stream* stream);
void fa_device_remove_voice(fa_device* device, struct fa_stream* stream);

/* stream.c */
void fa_stream_mix(struct fa_stream* s, fa_device* dev, float* out, uint32_t frames); /* audio thread */
int fa_stream_needs_work(struct fa_stream* s);
void fa_stream_fill(struct fa_stream* s, int64_t max_frames);  /* caller holds decode_lock */
fa_frames fa_stream_position_now(struct fa_stream* s);
void fa_stream_free_memory(struct fa_stream* s);               /* event thread, from the graveyard */

float fa_stream_norm_target_db(struct fa_stream* s);          /* any thread */
void fa_stream_start_analysis(struct fa_stream* s);           /* idempotent */
/* From a scan thread: the final loudness of `s` (album_lufs NAN if unknown). */
void fa_stream_loudness_ready(struct fa_stream* s, const fa_loudness* l, double album_lufs);

/* engine.c */
void fa_engine_wake_workers(fa_engine* engine);
void fa_engine_track_stream(fa_engine* engine, struct fa_stream* s, int add);

#endif
