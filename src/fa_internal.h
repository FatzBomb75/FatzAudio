#ifndef FA_INTERNAL_H
#define FA_INTERNAL_H

#include "fatzaudio/fatzaudio.h"
#include "miniaudio.h"
#include "fa_platform.h"
#include "tempo.h"
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
    int64_t length;    /* bytes, -1 = unknown (of the window, when there is one) */
    int64_t base;      /* fa_source_set_window: where the window starts in the file, block or callbacks */
    int64_t whole_length; /* the length before the first window */
    int windowed;
    int seekable;
    fa_result error;   /* first read failure of the app's callbacks or the file, else FA_OK */
    int remote;        /* bytes come over the network (fa_stream_open_url): decoders avoid
                          reading far ahead or scanning the whole file */
    int prefix_filled;
    size_t prefix_len;
    const uint8_t* prefix;  /* the first bytes: prefix_buf, or the memory block itself */
    uint8_t* prefix_buf;
} fa_source;

fa_result fa_source_open_path(fa_source* src, const char* path);
fa_result fa_source_open_memory(fa_source* src, const void* data, size_t size);
/* Like fa_source_open_memory but without a copy: `data` must outlive the source. */
fa_result fa_source_open_memory_ref(fa_source* src, const void* data, size_t size);
fa_result fa_source_open_io(fa_source* src, const fa_io_callbacks* io, void* user);
/* Makes the source read only `size` bytes from `base` of the file, block or callbacks
 * underneath (a stored ZIP entry), as if they were the whole source; position 0,
 * first bytes read again. `base` counts from the start of the whole source, so a
 * later call moves or undoes the window. Needs a known length; app callbacks must
 * seek. */
fa_result fa_source_set_window(fa_source* src, int64_t base, int64_t size);
/* fa_source_open_path, then the window `offset`, `length` (none when length is 0):
 * how scans and tags reopen a stream's stored ZIP entry. */
fa_result fa_source_open_path_window(fa_source* src, const char* path, int64_t offset, int64_t length);
/* Returns the first bytes of the source (up to FA_PROBE_BYTES) without moving the read position. */
size_t fa_source_peek(fa_source* src, const uint8_t** out_data);
/* Frees the copy of a file's first bytes once the format is known (other sources
 * keep theirs: memory blocks have no copy, app callbacks may not go back). A later
 * fa_source_peek on a file reads them again, moving the file: call it only from
 * the thread that reads the source. */
void fa_source_drop_prefix(fa_source* src);
/* The first bytes if they are held now (0 after fa_source_drop_prefix), never reading. */
size_t fa_source_peek_held(const fa_source* src, const uint8_t** out_data);
int64_t fa_source_read(fa_source* src, void* buf, size_t n); /* bytes read, 0 at end, <0 fa_result */
fa_result fa_source_seek(fa_source* src, int64_t offset);
int64_t fa_source_tell(const fa_source* src);
/* The whole source in one malloc'ed block (archives, .cdg files); needs a known
 * length of at most `max_size`, else FA_ERROR_UNSUPPORTED_FORMAT. */
fa_result fa_source_read_all(fa_source* src, size_t max_size, uint8_t** out_data, size_t* out_size);
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
    /* Optional (NULL = 0, 0): see fa_decoder_lag. */
    void (*lag)(fa_decoder* dec, int64_t* out_bytes, int64_t* out_frames);
    /* Optional (FA_OPEN_PRESCAN): reads the whole file's layout now, so the
     * length is exact and every seek is fast. */
    void (*prescan)(fa_decoder* dec);
} fa_decoder_vtable;

struct fa_decoder {
    const fa_decoder_vtable* vt;
    fa_source* src;
    void* impl;
    fa_audio_format format;  /* always FA_FORMAT_F32 interleaved */
    fa_frames length;        /* -1 = unknown */
    /* Frames the decoder drops at the start of the data (ADTS priming): a duration
     * that a playlist gives for the same data (HLS) is that much longer. */
    fa_frames lead_trim;
    int seekable;
    char codec[32];
    char container[32];
    /* Push decoders: the pts (ns) of the first frame the last read returned, when
     * the timeline restarts there (a jump in the app's timestamps, or the very
     * first frame); FA_PTS_NONE when it simply continues. */
    int64_t anchor_pts;
    /* Containers: the audio track asked for (-1 = the file's default) and how
     * many audio tracks the file has (0 is read as 1). */
    int32_t audio_track;
    int32_t track_count;
    /* The stream's fa_dynamic_range, for codecs that apply one (Dolby); given to
     * fa_decoder_open. NULL = full range. */
    fa_atomic32* dynamic_range;
    /* MIDI, from fa_decoder_open's env (wrapping decoders such as HLS pass both on). */
    struct fa_soundfont* soundfont;   /* borrowed, NULL = none */
    uint32_t midi_rate;               /* MIDI and tracker modules: output rate */
    /* Properties (fa_stream_info), set by the decoder when it knows them;
     * fa_decoder_open fills the rest in for whole-file formats. */
    int64_t audio_bytes;              /* coded audio data, -1 = unknown */
    uint32_t bitrate;                 /* nominal bits per second, 0 = unknown */
    uint32_t bits_per_sample;         /* PCM and lossless, 0 = lossy or unknown */
    int64_t file_size;                /* -1 = unknown */
};

/* What a decoder may need from the stream that opens it. */
typedef struct fa_decoder_env {
    fa_atomic32* dynamic_range;       /* the stream's fa_dynamic_range (read while decoding),
                                         NULL = full range */
    struct fa_soundfont* soundfont;   /* MIDI: the instruments (the caller keeps it alive
                                         while the decoder is open), NULL = none */
    uint32_t midi_rate;               /* MIDI and tracker modules: output rate, 0 = FA_MIDI_DEFAULT_RATE */
} fa_decoder_env;

/* Detects the format of `src` and opens the matching decoder. `name_hint` (a path
 * or NULL) and `mime_hint` only break ties when the content is ambiguous.
 * `audio_track`: 0-based among the audio tracks, -1 = default; FA_ERROR_NOT_FOUND
 * if the file has no such track. `env` may be NULL (defaults). */
fa_result fa_decoder_open(fa_decoder* dec, fa_source* src, const char* name_hint, const char* mime_hint,
                          int32_t audio_track, const fa_decoder_env* env);
/* The decoder's part of fa_stream_info (everything but has_cdg). */
void fa_decoder_get_info(const fa_decoder* dec, fa_stream_info* out);
fa_result fa_decoder_read(fa_decoder* dec, float* out, fa_frames frames, fa_frames* got);
fa_result fa_decoder_seek(fa_decoder* dec, fa_frames frame);
void fa_decoder_close(fa_decoder* dec);

/* dr_wav glue (miniaudio_impl.c: dr_wav's API is only visible there).
 * `sequential` = stop at the audio data, for sources that cannot seek back. */
fa_result fa_drwav_open(fa_source* src, int sequential, void** out_impl,
                        uint32_t* out_rate, uint32_t* out_channels, uint64_t* out_frames);
/* Bits per sample (PCM and float only, else 0) and the size of the data chunk (-1 = unknown). */
void fa_drwav_props(void* impl, uint32_t* out_bits, int64_t* out_data_bytes);
uint64_t fa_drwav_read(void* impl, float* out, uint64_t frames);
int fa_drwav_seek(void* impl, uint64_t frame);
void fa_drwav_close(void* impl);

/* dr_mp3 glue for MP3 over the network: never scans the file, restarts at any byte. */
fa_result fa_drmp3_open(fa_source* src, void** out_impl, uint32_t* out_rate, uint32_t* out_channels,
                        uint64_t* out_frames, int64_t* out_data_start);
uint64_t fa_drmp3_read(void* impl, float* out, uint64_t frames);
fa_result fa_drmp3_restart(void* impl);
void fa_drmp3_lag(void* impl, int64_t* out_bytes, int64_t* out_frames);
void fa_drmp3_close(void* impl);
/* dr_flac's decoder set-up, kept from one packet to the next instead of a malloc
 * and free per FLAC frame (codec_flac.c). */
typedef struct fa_drflac_cache {
    void* block;
    size_t cap;
    int used;
} fa_drflac_cache;
/* One FLAC stream held in memory ("fLaC", STREAMINFO, a frame), decoded into `out`
 * (room for max_frames at STREAMINFO's channel count, which dr_flac holds every
 * frame to). Frames decoded, -1 if it is not FLAC. */
int64_t fa_drflac_decode_memory(const void* data, size_t size, float* out, uint64_t max_frames,
                                uint32_t* out_channels, uint32_t* out_rate, fa_drflac_cache* cache);
/* dr_mp3's frame decoder (minimp3) lives in miniaudio_impl.c, where its API is
 * visible. fa_mp3dec_decode decodes the first frame in data[0..size); returns the
 * bytes used (0 = needs more data) and writes up to 1152 interleaved frames to
 * pcm. Free with free(). */
void* fa_mp3dec_create(void);
int fa_mp3dec_decode(void* dec, const uint8_t* data, int size, float* pcm, int* out_frames,
                     int* out_channels, int* out_rate);
void fa_mp3dec_reset(void* dec);
/* MP3 through dr_mp3 (dec_miniaudio.c): network sources and free-format files. */
fa_result fa_mp3_open_fallback(fa_decoder* dec);

/* Bytes the decoder has read from its source but not decoded yet, and frames it has
 * decoded but not returned yet (0 when it does not know). Places ICY titles exactly. */
void fa_decoder_lag(fa_decoder* dec, int64_t* out_bytes, int64_t* out_frames);

/* Registry contents, defined in decoder.c. */
extern const fa_decoder_vtable* const fa_decoder_registry[];
extern const uint32_t fa_decoder_registry_count;

/* Format decoders backed by miniaudio's embedded dr_wav / dr_flac / dr_mp3 (dec_miniaudio.c). */
extern const fa_decoder_vtable fa_decoder_wav;
extern const fa_decoder_vtable fa_decoder_flac;
extern const fa_decoder_vtable fa_decoder_mp3;
/* Containers with packet codecs (demux_mp4.c, demux_mkv.c). */
extern const fa_decoder_vtable fa_decoder_mp4;
extern const fa_decoder_vtable fa_decoder_mkv;
/* Ogg Opus and Ogg Vorbis (demux_ogg.c). */
extern const fa_decoder_vtable fa_decoder_ogg;
/* Raw AC-3 / E-AC-3 streams (dec_ac3.c). */
extern const fa_decoder_vtable fa_decoder_ac3;
/* AAC in ADTS frames (demux_adts.c). */
extern const fa_decoder_vtable fa_decoder_adts;
/* Standard MIDI Files and RIFF MIDI, synthesized with a soundfont (dec_midi.c). */
extern const fa_decoder_vtable fa_decoder_midi;
/* Tracker modules, played by libopenmpt (dec_tracker.c). */
extern const fa_decoder_vtable fa_decoder_tracker;
/* ASF / Windows Media Audio (demux_asf.c). */
extern const fa_decoder_vtable fa_decoder_asf;

/* Probing helpers (probe.c). */
size_t fa_probe_id3v2_size(const uint8_t* data, size_t size);
int fa_probe_wav(const uint8_t* data, size_t size, size_t id3_size);
int fa_probe_flac(const uint8_t* data, size_t size, size_t id3_size);
int fa_probe_mp3(const uint8_t* data, size_t size, size_t id3_size);
int fa_probe_midi(const uint8_t* data, size_t size, size_t id3_size);
int fa_probe_tracker(const uint8_t* data, size_t size, size_t id3_size);
/* First MPEG audio Layer III frame at or after `start`: bitrate (kbps), sample rate.
 * 0 if none is found in the window. */
int fa_probe_mp3_header(const uint8_t* data, size_t size, size_t start, uint32_t* out_kbps, uint32_t* out_rate);

/* ---- MIDI (dec_midi.c) ------------------------------------------------------ */

#define FA_MIDI_DEFAULT_RATE 44100
#define FA_MIDI_BUFFER_MS 150       /* default decode-ahead of MIDI streams (live controls) */

/* Soundfont references: retain returns `f` (NULL-safe); the last release frees it. */
/* DLS banks (dls.c), turned into a SoundFont 2 image the synthesizer loads. */
int fa_dls_probe(const uint8_t* data, size_t size);
fa_result fa_dls_to_sf2(const uint8_t* data, size_t size, uint8_t** out_sf2, size_t* out_size);
/* SF3 (SoundFont 2 with Ogg Vorbis samples, src/sf3.c): whether `data` is one, and
 * the same bank as plain SoundFont 2 (malloc'ed) with the samples decoded. */
int fa_sf3_probe(const uint8_t* data, size_t size);
fa_result fa_sf3_to_sf2(const uint8_t* data, size_t size, uint8_t** out_sf2, size_t* out_size);

/* MIDI reverb and chorus (src/midi_fx.c), stereo at the stream's rate: process
 * adds the effects of the two send buses (interleaved, `frames` each) to `out`;
 * `song_frame` is the song position of the first frame (the chorus LFO follows
 * it). Reset clears the tails (a seek). */
typedef struct fa_midi_fx fa_midi_fx;
fa_midi_fx* fa_midi_fx_new(uint32_t rate);
void fa_midi_fx_free(fa_midi_fx* fx);
void fa_midi_fx_reset(fa_midi_fx* fx);
void fa_midi_fx_process(fa_midi_fx* fx, const float* reverb_in, const float* chorus_in, float* out, int frames, fa_frames song_frame);
struct fa_soundfont* fa_soundfont_retain(struct fa_soundfont* f);
void fa_soundfont_release(struct fa_soundfont* f);
/* A fingerprint of the soundfont's content, for the loudness cache. */
uint64_t fa_soundfont_key(const struct fa_soundfont* f);
uint64_t fa_fnv1a(uint64_t h, const uint8_t* p, size_t n);   /* scan.c */
/* The engine's soundfont, retained (release it), or NULL. */
struct fa_soundfont* fa_engine_soundfont(fa_engine* engine);
/* The OS's bank (fa_soundfont_open_system), loaded once per engine, retained, or
 * NULL (none, or FA_ENGINE_NO_SYSTEM_SOUNDFONT). */
struct fa_soundfont* fa_engine_system_soundfont(fa_engine* engine);
/* The MIDI rate of streams of `engine`. */
uint32_t fa_engine_midi_rate(const fa_engine* engine);

/* The MIDI state of an open MIDI decoder, NULL for other decoders. */
struct fa_midi* fa_midi_of(fa_decoder* dec);
/* The decoder's tracker state, or NULL when it is not a tracker module. */
struct fa_tracker* fa_tracker_of(fa_decoder* dec);
/* The row log (dec_tracker.c), lock-free for any thread: the row playing at a
 * source frame (0 when that frame is not decoded), and the rows starting in
 * [from, to) that FA_TRACKER_ROW_EVENTS asks for (frames and order << 16 | row
 * values, at most cap). */
int fa_tracker_row_at(const struct fa_tracker* t, fa_frames frame, uint32_t* order, uint32_t* row);
int fa_tracker_row_events_on(const struct fa_tracker* t);   /* FA_TRACKER_ROW_EVENTS is not 0 */
uint32_t fa_tracker_rows_in(const struct fa_tracker* t, fa_frames from, fa_frames to, fa_frames* frames,
                            int64_t* values, uint32_t cap);
/* Frames of the text events, sorted, fixed while the decoder is open (read by the
 * audio thread to raise FA_EVENT_MIDI_TEXT). */
const fa_frames* fa_midi_text_frames(const struct fa_midi* m, uint32_t* out_count);

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

/* ---- Presentation clock (clock.c) ------------------------------------------
 * The audio thread writes one record per stretch of a stream it mixes: device
 * frames [dev, dev + n) carried presentation times pts0..pts1. Any thread maps a
 * device frame (the one at the speaker now) back to a presentation time. Records
 * are seqlocked, so readers never block the audio thread. */

#define FA_CLOCK_HISTORY 128

typedef struct fa_clock_rec {
    fa_atomic64 seq;              /* odd while being written; changes on every write */
    fa_atomic64 idx;              /* which record (k) this slot holds, -1 = none */
    fa_atomic64 dev, n, pts0, pts1;
} fa_clock_rec;

typedef struct fa_clock {
    fa_clock_rec rec[FA_CLOCK_HISTORY];
    fa_atomic64 count;            /* records written since the last reset */
    fa_atomic64 hold;             /* reported until the first record reaches the speaker */
    fa_atomic32 epoch;            /* odd while being reset */
} fa_clock;

/* Clears the history; nothing may be recording (the audio thread is kept out). */
void fa_clock_reset(fa_clock* c, int64_t hold_pts);
void fa_clock_set_hold(fa_clock* c, int64_t hold_pts);  /* before the first record */
/* Audio thread. */
void fa_clock_record(fa_clock* c, int64_t dev, int64_t n, int64_t pts0, int64_t pts1);
/* Presentation time at device frame `frame` (fractional). */
int64_t fa_clock_at(fa_clock* c, double frame);
/* The device frame right after the last one recorded, -1 = none since the reset. */
int64_t fa_clock_mixed_end(fa_clock* c);

/* Where the timeline restarts inside a push stream's ring: at ring frame `idx`
 * (frames written since the ring was reset) the pts is `pts`. Single producer (the
 * decode worker, under decode_lock), single consumer (the audio thread). */
#define FA_PTSMAP_SIZE 64

#define FA_SRC_ANCHORS 64

typedef struct fa_ptsmap {
    int64_t idx[FA_PTSMAP_SIZE];
    int64_t pts[FA_PTSMAP_SIZE];
    fa_atomic64 write, read;
    int64_t cur_idx, cur_pts;     /* audio thread: the anchor in effect */
    double cur_src;               /* audio thread: source position heard at cur_idx */
    int cur_src_ok;               /* 0 = not looked up yet for this anchor */
} fa_ptsmap;

void fa_ptsmap_reset(fa_ptsmap* m);                           /* both sides kept out */
void fa_ptsmap_add(fa_ptsmap* m, int64_t idx, int64_t pts);   /* drops it when full */
/* Audio thread: pts at ring frame `idx` (rate: ns_per_frame per ring frame), and in
 * *out_next the next anchor after `idx` (INT64_MAX = none). */
int64_t fa_ptsmap_pts(fa_ptsmap* m, int64_t idx, double ns_per_frame, int64_t* out_next);

/* ---- Curves (curve.c) ---------------------------------------------------- */

/* Shape of a ramp at progress t (0..1) -> 0..1. `rising` tells the equal-power
 * curve whether the value goes up (sin) or down (1 - cos). */
float fa_curve_eval(int curve, const float bezier[4], float t, int rising);

/* A complete shape: a built-in curve, bezier control points, or a point list. */
typedef struct fa_shape {
    int curve;
    float bezier[4];
    int npoints;
    float points[FA_CURVE_MAX_POINTS];
} fa_shape;

/* The same, readable by the audio thread while an API thread writes it. A torn
 * read is possible only while it is being changed, and gives a valid shape. */
typedef struct fa_shape_atomic {
    fa_atomic32 curve;
    fa_atomic32 bezier[4];
    fa_atomic32 npoints;
    fa_atomic32 points[FA_CURVE_MAX_POINTS];
} fa_shape_atomic;

float fa_shape_eval(const fa_shape* sh, float t, int rising);
/* The shape played backwards and upside down: 1 - shape(1 - t). A falling value with
 * this shape is the mirror image of a rising one (crossfade pairs). */
float fa_shape_eval_mirror(const fa_shape* sh, float t);
/* Validates `r` (NULL = `fallback` curve) into `out`. FA_ERROR_INVALID_ARG if bad. */
fa_result fa_shape_from_ramp(const fa_ramp* r, int fallback, fa_shape* out);
void fa_shape_to_ramp(const fa_shape* sh, fa_ramp* out);  /* points left NULL */
void fa_shape_store(fa_shape_atomic* a, const fa_shape* sh);
void fa_shape_load(fa_shape_atomic* a, fa_shape* out);

/* ---- Tempo, pitch and rate (tempo.c) ---------------------------------------
 * The chain itself is in tempo.h. These parameters are applied where audio is
 * decoded, not in the mixer: a value takes effect at the chain's output head (the
 * end of the decode-ahead buffer), and ramps advance with the frames it makes. */

#define FA_TEMPO_PARAMS 3            /* FA_PARAM_RATE, FA_PARAM_TEMPO, FA_PARAM_PITCH */
#define FA_HEARD_RAMPS 16            /* ramp ends waiting to be heard (more: raised at once) */
#define FA_TPARAM_NOW INT64_MIN      /* a ramp start: wherever the chain is when it is first evaluated */

typedef struct fa_tparam {
    float value;                     /* in effect at the chain's head */
    float from, target;
    int64_t start;                   /* chain frame the ramp starts at (may be negative after a
                                        timeline reset), or FA_TPARAM_NOW */
    int64_t len;                     /* ramp length in chain frames */
    fa_shape shape;
    int64_t tag;
    int active, notify;
} fa_tparam;

void fa_tparam_init(fa_tparam* p, float value);
/* A plain set (len 0, notify 0) or a ramp; replaces what runs. Returns 1 when that
 * was a ramp still owing its RAMP_DONE (tag in *old_tag). */
int fa_tparam_post(fa_tparam* p, float target, int64_t len, const fa_shape* shape, int64_t tag, int notify,
                   int64_t start, int64_t* old_tag);
/* The value at chain frame `at`. Returns 1 when a ramp finished and owes its
 * RAMP_DONE (tag in *done_tag, the chain frame it ended at in *done_at). */
int fa_tparam_eval(fa_tparam* p, int64_t at, float* out_value, int64_t* done_tag, int64_t* done_at);
/* The chain's frame count starts again from 0 at what was chain frame `base`:
 * keeps a running or scheduled ramp on the same playback time. */
void fa_tparam_rebase(fa_tparam* p, int64_t base);

/* Played (ring) frames -> source positions, piecewise linear. The decode side adds
 * a segment wherever the slope (source frames per ring frame) changes; before the
 * first one, the base (seek_base, source rate / device rate) applies. The audio
 * thread releases segments it has played past; any thread may look up. */
#define FA_TMAP_SIZE 256
#define FA_TMAP_SLACK 16             /* released segments kept readable for late lookups */

typedef struct fa_tmap {
    int64_t at[FA_TMAP_SIZE];
    double src[FA_TMAP_SIZE];
    double slope[FA_TMAP_SIZE];
    fa_atomic64 write, read;
} fa_tmap;

void fa_tmap_reset(fa_tmap* m);
int fa_tmap_full(fa_tmap* m);
void fa_tmap_add(fa_tmap* m, int64_t at, double src, double slope);
/* Source position at ring frame `at`; *out_slope and *out_next (where the segment
 * ends, INT64_MAX = open) may be NULL. */
double fa_tmap_pos(fa_tmap* m, int64_t at, double base_src, double base_slope, double* out_slope, int64_t* out_next);
/* Ring frames from `at` until the source position reaches `target` (0 if it has). */
int64_t fa_tmap_frames_to(fa_tmap* m, int64_t at, double target, double base_src, double base_slope);
void fa_tmap_release(fa_tmap* m, int64_t at);   /* audio thread */

/* ---- Loudness (loudness.c) ------------------------------------------------ */

#define FA_TP_PHASES 4
#define FA_TP_TAPS 32
#define FA_RUNNING_SUBS 150     /* running estimate window: 15 s of 100 ms sub-blocks */

/* Polyphase true-peak detector (BS.1770 Annex 2 style). */
typedef struct fa_truepeak {
    uint32_t channels;
    int phases;                 /* 4 below 88.2 kHz, 2 below 176.4 kHz, else 1 */
    float coef[FA_TP_TAPS][FA_TP_PHASES];   /* tap-major: the phases of one tap side by side */
    float* wide;                /* SIMD builds: coef[t][p] repeated 4 times, [t][p][4] (block measuring) */
    float bound;                /* largest sum of |coef| of a phase */
    float* hist;                /* per channel: 2 x FA_TP_TAPS */
    int pos;
    float peak;                 /* largest |value| measured so far, linear */
} fa_truepeak;

fa_result fa_truepeak_init(fa_truepeak* tp, uint32_t rate, uint32_t channels);
void fa_truepeak_free(fa_truepeak* tp);
/* One interleaved frame in; the largest |interpolated value| it completes out. */
float fa_truepeak_push(fa_truepeak* tp, const float* frame);
/* Up to FA_TP_PUSH_BLOCK frames at once: peaks[i] = what fa_truepeak_push would
 * return for frame i, computed several frames at a time. When no value in the
 * block can exceed `floor_level`, only the history is updated and 0 is returned
 * (peaks untouched); else 1. */
#define FA_TP_PUSH_BLOCK 256
int fa_truepeak_push_block(fa_truepeak* tp, const float* x, uint32_t frames, float* peaks, float floor_level);
void fa_truepeak_flush(fa_truepeak* tp);

/* ---- Analysis taps (analysis.c) ------------------------------------------
 * What a stream or device played lately, by device frame, for levels and the
 * spectrum. Made on the first request (fa_tap_get, any thread); written by the
 * audio thread only; read without locks. */
typedef struct fa_tap {
    float* buf;               /* `frames` stereo frames, a ring by device frame */
    uint32_t frames;          /* power of two, about 2 s at the device rate */
    fa_atomic64 start;        /* first device frame written since the last jump */
    fa_atomic64 end;          /* device frame after the newest one written */
} fa_tap;
fa_tap* fa_tap_get(fa_atomic_ptr* slot, uint32_t rate);
/* `n` frames played from device frame `at`; skipped frames are silence. */
void fa_tap_write(fa_tap* t, int64_t at, const float* frames, int64_t n);
void fa_tap_free(fa_tap* t);

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

/* ---- Tags (tags.c, tags_read.c) ------------------------------------------- */

/* Where the tag readers deliver what they find. Readers call `text` for every
 * text item (UTF-8 key and value) and `picture` for every picture; with
 * `picture` NULL, picture data is skipped without being read, and text items
 * longer than `max_text` bytes are skipped too. */
typedef struct fa_tag_sink fa_tag_sink;
struct fa_tag_sink {
    void (*text)(fa_tag_sink* sink, int format, const char* key, const char* value);
    void (*picture)(fa_tag_sink* sink, int format, int type, const char* mime, const char* description,
                    const uint8_t* data, size_t size);
    size_t max_text;
};

/* Reads every tag of the source into `sink`, in priority order (see fa_tags_get).
 * Moves the read position; needs a seekable source. */
void fa_tags_collect(fa_source* src, fa_tag_sink* sink);
/* Where the audio of an MP3, AAC, AC-3 or FLAC file lies: after an ID3v2 tag and
 * FLAC's metadata blocks, before APEv2 and ID3v1 tags. *out_bits = FLAC's bits
 * per sample, else 0. Moves the read position. */
void fa_tags_audio_span(fa_source* src, int64_t* out_start, int64_t* out_end, uint32_t* out_bits);

/* Loudness stored in ReplayGain 2 / R128 tags, converted to LUFS. NAN = absent. */
typedef struct fa_tag_loudness {
    double track_lufs, album_lufs;
    double track_peak_db, album_peak_db;   /* sample peak in dBFS */
} fa_tag_loudness;

/* The loudness tags of any format fa_tags_collect reads. Moves the read position. */
void fa_tags_read_loudness(fa_source* src, fa_tag_loudness* out);
/* The comment header of an Ogg Opus, Vorbis or FLAC file (demux_ogg.c). */
void fa_ogg_read_tags(fa_source* src, fa_tag_sink* sink);
/* A Vorbis comment block (FLAC, Ogg Vorbis/Opus): vendor, count, "KEY=value"
 * items. METADATA_BLOCK_PICTURE items are delivered as pictures. */
void fa_tags_parse_vorbis_comments(const uint8_t* data, size_t size, fa_tag_sink* sink);
/* A FLAC PICTURE block body (also the base64 payload of METADATA_BLOCK_PICTURE). */
void fa_tags_parse_flac_picture(const uint8_t* data, size_t size, int format, fa_tag_sink* sink);
/* The MIME type of image data from its first bytes, "" if unknown. */
const char* fa_tags_image_mime(const uint8_t* data, size_t size);
/* One "KEY" / "value" tag item: the loudness keys are taken, others ignored. */
void fa_tags_handle(const char* key, const char* value, fa_tag_loudness* out);
int fa_tags_key_is(const char* key, const char* name);   /* ASCII case-insensitive */

/* ASF tags (demux_asf.c): Content Description, Extended Content Description and
 * Metadata (Library) items, WM/Picture included. Returns 0 if `src` is not ASF.
 * Moves the read position. */
int fa_asf_read_tags(fa_source* src, fa_tag_sink* sink);

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
    float* ceil_line;           /* the ceiling each delayed frame was measured against */
    float mix;                  /* 0 = bypassed, 1 = limiting; fades between them */
    float mix_step;
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
void fa_limiter_process(fa_limiter* l, float* frames, uint32_t count, float ceiling, int on);

/* ---- Events (event.c) ----------------------------------------------------- */

#define FA_EVENT_QUEUE_SIZE 1024 /* power of two */
/* Per stream. The API empties the queues and spill slots right after publishing a
 * parameter command and before starting or scheduling playback, so between two
 * collections the mixer raises at most, for each of the 3 lanes (volume, pan,
 * crossfade), one RAMP_DONE for the ramp the new command replaces and one for the
 * new command itself (3 x 2), plus two ENDED and two STARTED (one from before a
 * start that it had not posted yet, one after). Only those three types spill;
 * markers are best effort and dropped when the device queue is full. So the spill
 * slots always hold everything that must arrive, even with the device queue full. */
#define FA_EVENT_SPILL 10

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
 * event goes to the stream's spill slots, which are sized so that ENDED and RAMP_DONE
 * are never lost (a RampAsync waits on them; see FA_EVENT_SPILL). */
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
#define FA_LANE_FADE 2           /* stream lane: crossfade gain (fa_crossfade, queues) */
#define FA_STREAM_LANES 3
#define FA_START_QUEUED INT64_MAX /* start_frame of a queued stream: set by its predecessor */

struct fa_engine {
    fa_engine_config config;
    char* temp_folder;           /* config.temp_directory + "/fatzaudio-tmp", NULL = none */
    ma_context context;
    ma_mutex lock;               /* guards device enumeration scratch (ma_context device lists) */

    fa_mutex list_lock;          /* streams, devices, graveyard, pending events */
    fa_cond work_cond;           /* wakes decode workers */
    fa_cond event_cond;          /* wakes the event thread */
    struct fa_stream* streams;   /* attached streams the workers keep filled */
    struct fa_device* devices;
    struct fa_stream* graveyard; /* closed streams, freed by the event thread */
    struct fa_stream* cdg_watched; /* streams with CD+G change events (cdg.c), via cdg_watch_link */
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

    struct fa_soundfont* soundfont; /* fa_engine_set_soundfont, under list_lock */
    /* The OS's bank for MIDI streams without a soundfont: loaded by the first one
     * that needs it, under system_soundfont_lock (later ones wait for it);
     * system_soundfont_tried once it was found or found missing. */
    fa_mutex system_soundfont_lock;
    struct fa_soundfont* system_soundfont;
    int system_soundfont_tried;
};

typedef struct fa_voice_list {
    uint32_t count;
    struct fa_stream* items[1];
} fa_voice_list;

typedef struct fa_bus_list {
    uint32_t count;
    struct fa_bus* items[1];
} fa_bus_list;

struct fa_device {
    fa_engine* engine;
    ma_device device;
    uint32_t rate;
    fa_mutex lock;               /* serializes voice list edits (API threads only) */
    /* Held by the audio callback while it takes commands, and by fa_crossfade while it
     * posts a start and two fades for the current clock, so a callback sees all of
     * them or none. The callback only tries it: when fa_crossfade holds it, `xf_busy`
     * is set and the callback leaves every command and start due now or earlier for
     * the next callback (later ones can't be a crossfade's). */
    fa_mutex xf_lock;
    int xf_busy;                 /* audio thread only */
    fa_atomic_ptr voices;        /* fa_voice_list*, read by the audio callback */
    fa_atomic_ptr buses;         /* fa_bus_list*, read by the audio callback */
    fa_atomic64 clock;           /* frames mixed so far (start of the next callback) */
    /* When the last callback started: its first device frame and the (smoothed)
     * monotonic time, seqlocked by cb_seq. The presentation clock interpolates from it. */
    fa_atomic64 cb_seq, cb_frame, cb_time;
    int64_t cb_prev_time;        /* audio thread: smoothing state */
    uint32_t cb_prev_frames;
    fa_atomic64 epoch;           /* odd while the audio callback runs */
    fa_event_queue events;       /* audio callback -> event thread */
    fa_atomic32 spilled;         /* 1 = some stream on this device used its spill slots */
    fa_atomic32 publishing;      /* audio thread is between taking a sequence number and publishing */
    fa_limiter limiter;          /* audio thread only */
    fa_atomic32 muted;           /* fa_device_set_mute */
    float mute_level;            /* audio thread: 0 = audible .. 1 = silent */
    fa_atomic_ptr encoders;      /* fa_enc_list* recording the output (encode.c) */
    fa_atomic_ptr tap;           /* fa_tap*, the output after the limiter (analysis.c) */
    /* Device changes. `life_lock` guards `device` (ma_device) whenever it may be
     * reopened: the event thread holds it to reopen, the API calls that use it hold it. */
    fa_mutex life_lock;
    int device_ok;               /* life_lock: `device` is initialized */
    float volume;                /* life_lock: master volume, set again on a reopened device */
    fa_atomic32 want_started;    /* between fa_device_start and stop: a stop nobody asked for = lost */
    fa_atomic32 lost;            /* the device went away: the event thread reopens the default */
    fa_atomic64 lost_at;         /* when: the backend gets a moment to reroute a default device itself */
    fa_atomic32 rerouted;        /* the event thread reads the new latency */
    fa_atomic32 reopening;       /* stop notifications of the old device are expected */
    fa_atomic32 closing;
    fa_atomic32 changed;         /* 1 + value of the FA_EVENT_DEVICE_CHANGED the next callback raises */
    fa_atomic64 latency_ns;      /* output latency of `device`, readable without life_lock */
    int64_t retry_at;            /* event thread: when to try reopening again */
    struct fa_device* next;      /* engine->devices */
};

/* Event thread: reopens devices that went away on the system default. */
void fa_devices_service(fa_engine* e);
void fa_device_simulate(fa_device* d, int lost);   /* tests: a reroute (0) or a removal (1) */

/* Mute level (0 = audible, 1 = silent) after `n` frames gliding toward `target`:
 * a full mute or unmute takes 5 ms, like a volume change. */
static inline float fa_mute_glide(float level, float target, int64_t n, uint32_t rate)
{
    float step = rate ? (float)n / ((float)rate * 0.005f) : 1.0f;
    if (level < target) return level + step < target ? level + step : target;
    return level - step > target ? level - step : target;
}
/* ---- Equalizer (eq.c) --------------------------------------------------------
 * API threads write the bands (one at a time: `seq` odd while writing); the audio
 * thread copies them when `seq` changed and was even before and after its read. */
typedef struct fa_eq_shared {
    fa_atomic32 seq;
    fa_atomic32 count, enabled;
    fa_atomic32 type[FA_EQ_MAX_BANDS];
    fa_atomic32 freq[FA_EQ_MAX_BANDS], q[FA_EQ_MAX_BANDS], gain[FA_EQ_MAX_BANDS];  /* float bits */
} fa_eq_shared;

typedef struct fa_eq_bandstate {
    int32_t type;
    float f, q, g;               /* applied now (gliding) */
    float tf, tq, tg;            /* targets */
    float b0, b1, b2, a1, a2;
    float z[2][2];               /* per channel */
    int dirty;                   /* coefficients need a redesign */
} fa_eq_bandstate;

typedef struct fa_eq_state {     /* audio thread only */
    int32_t seen;                /* fa_eq_shared.seq taken last, -1 = none */
    uint32_t rate;
    int active;
    uint32_t blocks;             /* fa_eq_process calls, paces redesigns while gliding */
    fa_eq_bandstate band[FA_EQ_MAX_BANDS];
} fa_eq_state;

void fa_eq_shared_init(fa_eq_shared* sh);
void fa_eq_reset(fa_eq_state* st);
/* Start of a mix: takes new bands; `restart` clears the filter memory (the audio
 * does not continue the last mix). Returns 0 when the EQ is flat (skip it). */
int fa_eq_begin(fa_eq_state* st, fa_eq_shared* sh, uint32_t rate, int restart);
/* Glides and filters `n` stereo frames in place. */
void fa_eq_process(fa_eq_state* st, float* buf, int64_t n);

/* User DSP (fa_stream_add_dsp): an immutable list, replaced as a whole. */
typedef struct fa_dsp_entry {
    fa_dsp_callback callback;
    void* user;
    int32_t priority;
    uint32_t id;
} fa_dsp_entry;

typedef struct fa_dsp_list {
    uint32_t count;
    fa_dsp_entry items[1];
} fa_dsp_list;

/* ---- Parameter lanes (lane.c) --------------------------------------------
 * One automatable value (stream volume, pan, crossfade gain; bus volume, pan).
 * API threads post commands (a set, or a ramp with a shape and an optional start
 * frame) through a 2-slot mailbox; the audio thread takes the newest one at the
 * start of a callback and runs it sample-accurately.
 * `word` holds the sequence number of the newest command plus FA_CMD_CLAIMED once
 * the audio thread took it; a single atomic exchange/CAS on it decides who reports
 * a replaced ramp, so every ramp gets exactly one RAMP_DONE. */
#define FA_CMD_CLAIMED 0x40000000

enum {
    FA_LANE_MIRROR = 1,     /* falling crossfade side: shape 1 - s(1 - t) */
    FA_LANE_END_STREAM = 2  /* the stream ends when this ramp is done (crossfade out) */
};

typedef struct fa_param_slot {
    fa_atomic32 target;          /* float bits */
    fa_atomic64 frames_ns;       /* duration in ns */
    fa_shape_atomic shape;
    fa_atomic64 tag;
    fa_atomic32 notify;          /* 1 = ramp (raise RAMP_DONE), 0 = plain set */
    fa_atomic32 has_start;       /* 1 = jump to `start` first: the value of a set this */
    fa_atomic32 start;           /*     command replaced before the mixer applied it */
    fa_atomic64 at;              /* device frame where the ramp starts, -1 = at once */
    fa_atomic32 flags;           /* FA_LANE_* */
} fa_param_slot;

typedef struct fa_param_mailbox {
    fa_atomic32 word;
    fa_param_slot slot[2];
    /* API side, under the owner's lock: what the newest command was. */
    int last_notify;
    int64_t last_tag;
    float last_target;
    int last_has_start;
    float last_start;
} fa_param_mailbox;

/* Audio-thread state of one lane. */
typedef struct fa_param_voice {
    float value;
    int active;
    float start, target;
    int64_t total, done;         /* frames */
    int64_t at;                  /* device frame the ramp starts at; -1 = started */
    fa_shape shape;
    int flags;
    int notify;
    int64_t tag;
} fa_param_voice;

typedef struct fa_lane {
    fa_param_mailbox mb;
    fa_param_voice v;            /* audio thread only */
    fa_atomic32 current;         /* float bits, published by the audio thread */
} fa_lane;

void fa_lane_init(fa_lane* l, float value);
/* Posts a command; the caller holds the owner's lock. Returns 1 (and the tag) when
 * an earlier ramp the audio thread never started was replaced: its RAMP_DONE is
 * then the caller's to raise. */
int fa_lane_post(fa_lane* l, float target, int64_t duration_ns, const fa_shape* shape, int64_t tag,
                 int notify, int64_t at, int flags, int64_t* out_replaced_tag);
/* The value as the API sees it (a set not yet applied reads back at once). Caller holds the lock. */
float fa_lane_get(fa_lane* l);
/* Audio thread: takes the newest command. `audible` = the owner was heard in the
 * previous callback and is heard now (otherwise plain sets apply without a glide).
 * Returns 1 (and the tag) when a running ramp that raises RAMP_DONE was replaced. */
int fa_lane_take(fa_lane* l, int audible, uint32_t rate, int64_t* out_replaced_tag);
/* The same, but leaves a command that starts at once or by frame `hold_upto` where it
 * is (fa_device.xf_busy). */
int fa_lane_take_after(fa_lane* l, int64_t hold_upto, int audible, uint32_t rate, int64_t* out_replaced_tag);
/* Audio thread: value after `done` frames of the active ramp. */
float fa_lane_ramp_value(const fa_param_voice* v, int64_t done);
/* Audio thread: starts a ramp directly (queue transitions decided on the audio thread). */
void fa_lane_start_ramp(fa_lane* l, float from, float target, int64_t frames, int64_t at,
                        const fa_shape* shape, int flags);

/* Value of a lane at device frame `frame` within the current callback, for lanes that
 * run on the device clock (buses). `clock` is the frame at the callback start.
 * At the frame a zero-length ramp starts, `from_here` picks the value from there
 * on (the target; a block starting there) over the value up to it (a block ending there). */
float fa_lane_value_at_frame(const fa_param_voice* v, int64_t clock, int64_t frame, int from_here);
/* Advances a device-clock lane to clock + frames; returns 1 when its ramp finished. */
int fa_lane_advance(fa_param_voice* v, int64_t clock, int64_t frames);

struct fa_bus {
    fa_device* device;
    fa_lane lanes[FA_MIX_PARAMS];  /* VOLUME, PAN on the device clock */
};

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
    fa_atomic32 dynamic_range;   /* fa_dynamic_range, read by the codec on every packet */

    fa_atomic32 state;           /* fa_stream_state */
    fa_atomic64 position;        /* source frames, when not attached */
    fa_atomic64 markers[FA_MARKER_COUNT];  /* read by the worker and the audio thread */
    fa_shape_atomic fade_shape[2];         /* FA_FADE_IN, FA_FADE_OUT */
    fa_atomic32 loop;            /* 1 = loops (FA_OPEN_LOOP, fa_stream_set_loop); changed
                                    under decode_lock, or while the mixer skips the stream */
    fa_atomic32 muted;           /* fa_stream_set_mute */
    float mute_level;            /* audio thread: 0 = audible .. 1 = silent, glides to `muted` */
    int64_t mute_next;           /* audio thread: device frame right after the last one mixed
                                    (the mute and the equalizer glide only across continuous mixes) */

    /* Decode-ahead, valid while attached. Worker-owned under decode_lock. */
    fa_device* device;           /* written under api_lock */
    /* The device for fa_stream_get_clock, which takes no lock: set once the voice is
     * live, cleared first on detach, which then waits for clock_readers to drain. */
    fa_atomic_ptr clock_device;
    fa_atomic32 clock_readers;
    fa_atomic_ptr bus;           /* fa_bus*, NULL = master; read by the audio thread */
    uint32_t dev_rate;
    fa_ring ring;
    ma_data_converter converter;
    int converter_ready;
    float* in_buf;
    fa_frames in_cap, in_count, in_offset;
    fa_frames decoded;           /* source frames fed to the converter since seek_base */
    /* Forward-only sources only: a copy of the last source frames fed in (`hist`, a
     * circular buffer covering everything that can still be unheard), so a detach can
     * keep the audio decoded ahead in `carry`, which is fed in again before the
     * decoder on the next attach. `carry` lives across attaches; `hist` with the chain. */
    float* hist;
    fa_frames hist_cap;
    float* carry;
    fa_frames carry_count, carry_offset;
    int draining;                /* the source ended; in_buf holds silence that flushes the resampler */
    float* out_buf;
    fa_frames out_cap;
    float* mat_buf;              /* matrix output, 2 channels */
    float* matrix;               /* in x FA_DEVICE_CHANNELS weights, NULL = default; under
                                    api_lock + decode_lock */
    fa_frames src_pos;           /* source frame of the next decoder read (worker) */
    fa_atomic32 eof;             /* the worker has written the last frame */
    fa_atomic32 flushing;        /* 1 = the audio thread must not touch the ring */
    fa_atomic64 seek_pending;    /* fa_stream_seek_async target a worker carries out; -1 = none */
    fa_atomic64 seek_max;        /* the last frame a pending target may be replaced with, lock-free */
    fa_atomic64 seek_base;       /* source frame at the start of the ring's data */
    fa_atomic64 played;          /* device frames consumed since seek_base (audio thread) */
    fa_atomic32 underruns;

    /* Presentation clock (clock.c); pts map and push queue of push streams (push.c). */
    fa_clock clock;
    fa_ptsmap ptsmap;
    struct fa_push* push;        /* NULL for file, memory and I/O streams */
    fa_atomic64 played_audible;  /* frames played since the last flush (stall events) */
    int stalled;                 /* audio thread: FA_EVENT_STALLED raised, RESUMED owed */
    fa_eq_shared eq_shared;
    fa_eq_state eq;              /* audio thread */
    fa_atomic32 eq_restart;      /* 1 = the timeline restarted: clear the EQ's memory */
    fa_atomic_ptr dsps;          /* fa_dsp_list*, read by whoever decodes (under decode_lock) */
    uint32_t dsp_next_id;        /* under api_lock */
    /* Push streams, under decode_lock: the source frames at which the app's timeline
     * restarted, so audio kept across a detach (carry) keeps its timestamps. */
    int64_t anchor_pos[FA_SRC_ANCHORS], anchor_pts[FA_SRC_ANCHORS];
    int64_t anchors;             /* written since the last flush */
    fa_atomic32 staged;          /* in_buf or carry hold frames not in the ring yet */

    /* Parameters */
    fa_lane lanes[FA_STREAM_LANES];      /* VOLUME, PAN, crossfade gain */
    uint32_t voice_rate;                 /* device rate the ramp counters are in */
    int was_audible;                     /* audio thread only: mixed in the previous callback */

    /* Scheduling and queues */
    fa_atomic64 start_frame;     /* FA_STATE_SCHEDULED: device frame to start at */
    fa_atomic_ptr queued;        /* the stream to start at the mix point (fa_stream_set_next) */
    fa_atomic64 xfade_ns;        /* queue crossfade length */
    fa_shape_atomic xfade_shape;

    /* Normalization */
    char* path;                  /* for scans of path streams; NULL otherwise */
    int64_t path_offset;         /* the audio is path_length bytes from here in `path` */
    int64_t path_length;         /* (a stored ZIP entry); 0 = all of it */
    char path_name[260];         /* that entry's name, the decoder's hint; "" = none */
    int64_t zip_size;            /* bytes of the ZIP the audio came from, 0 = not a ZIP */
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

    /* Web streams (fa_stream_open_url): the download buffer, NULL for other streams.
     * ICY titles are announced when heard: a worker arms the source frame where the
     * next title starts, the audio thread posts FA_EVENT_METADATA when playback gets
     * there. `meta_announced` (the last seq posted) keeps workers and the audio
     * thread from posting a title twice. */
    struct fa_net* net;
    fa_atomic64 meta_frame;      /* INT64_MAX = nothing armed */
    fa_atomic64 meta_seq_armed;
    fa_atomic64 meta_announced;

    /* Tempo, pitch and rate (tempo.c). tparams and the options under tempo_lock;
     * the chain and its staging under decode_lock. */
    fa_mutex tempo_lock;
    fa_tparam tparams[FA_TEMPO_PARAMS];
    /* Tempo, pitch and rate ramps that ended in the decode-ahead audio: their
     * RAMP_DONE waits until playback reaches `at` (a played frame). state: 0 free,
     * 1 being written (by the decode side, under decode_lock), 2 waiting, 3 being
     * raised (by whoever claimed it: the audio thread, or a seek or detach). */
    struct {
        fa_atomic32 state;
        int64_t at, tag;
    } heard_ramps[FA_HEARD_RAMPS];
    fa_atomic32 heard_waiting;   /* slots in state 2 or about to be */
    int tempo_fast, tempo_formants, tempo_transients;
    uint32_t tempo_latency_ms;
    fa_tempo* tempo;             /* NULL until a parameter first leaves neutral */
    int tempo_drained;           /* the chain gave back its tail at the end of the source */
    int jumped_back;             /* a loop jumped back since the last restart (decode side) */
    int64_t tempo_frames;        /* chain output frames since the last restart */
    fa_tmap tmap;
    fa_atomic32 tempo_live;      /* the chain is engaged: decode-ahead is kept short */
    /* Decode-only streams: chain output not yet read */
    float* rd_in;
    const float* rd_out;
    int64_t rd_count, rd_off;
    double rd_src0, rd_slope;
    const float* chain_buf;      /* attached: the chain's output the converter reads, NULL = in_buf */

    /* CD+G graphics (cdg.c): the packs, the surfaces drawing them, and a generation
     * count that tells surfaces the graphics were replaced. Never touched by the
     * audio thread or the workers. */
    fa_mutex cdg_lock;
    struct fa_cdg_data* cdg;     /* NULL = no graphics */
    struct fa_cdg_surface* cdg_surfaces;
    uint32_t cdg_gen;
    fa_atomic64 cdg_offset;      /* fa_time */
    /* FA_EVENT_CDG_CHANGED (fa_stream_set_cdg_events): the event thread follows the
     * clock over the packs. cdg_events is read without the lock; the rest under it. */
    fa_atomic32 cdg_events;
    struct fa_stream* cdg_watch_link; /* engine->cdg_watched, under list_lock */
    int cdg_watch_listed;        /* under list_lock */
    size_t cdg_watch_next;       /* packs the app was last told about */
    uint32_t cdg_watch_gen;
    int cdg_watch_dirty;         /* report the whole screen next (enabled, packs rewritten) */
    int cdg_watch_shifted;       /* smooth-scroll offsets are not 0: tiles move */

    /* Encoders (encode.c): taps recording what the stream plays (fa_enc_list*, read
     * by the audio thread), and the one reading it as a decode-only stream. Both
     * under api_lock. */
    fa_atomic_ptr encoders;
    struct fa_encoder* reader;
    fa_atomic_ptr tap;           /* fa_tap*, the stream's audio as mixed (analysis.c) */
    /* MIDI: the soundfont the stream was opened with (a reference, NULL for other
     * formats or none) and the decoder's MIDI state (live controls, texts). */
    struct fa_soundfont* soundfont;
    struct fa_midi* midi;
    struct fa_tracker* tracker;  /* tracker modules: the decoder's state (dec_tracker.c) */

    struct fa_stream* next;      /* engine->streams or engine->graveyard */
};

/* device.c */
/* One audio callback: queue transitions, buses, every voice, the limiter, the clock.
 * The backend's callback calls it; white-box tests call it to play the audio thread. */
void fa_device_render(fa_device* device, float* out, uint32_t frames);
/* The same with an explicit callback start time (white-box tests). */
void fa_device_render_at(fa_device* device, float* out, uint32_t frames, int64_t now_ns);
/* Device frame at the speaker at monotonic time `now_ns` (latency included). */
double fa_device_speaker_frame(fa_device* device, int64_t now_ns);
void fa_device_wait_quiescent(fa_device* device);
fa_result fa_device_add_voice(fa_device* device, struct fa_stream* stream);
void fa_device_remove_voice(fa_device* device, struct fa_stream* stream);

/* stream.c */
/* Presentation clock of `s` at monotonic time `now_ns` (fa_stream_get_clock uses the
 * current time; tests pass their own). Caller holds api_lock. */
int64_t fa_stream_clock_at(struct fa_stream* s, int64_t now_ns);
/* fa_stream_get_buffered at monotonic time `now_ns` (push streams). Caller holds api_lock. */
int64_t fa_stream_buffered_at(struct fa_stream* s, int64_t now_ns);
void fa_stream_mix(struct fa_stream* s, fa_device* dev, float* out, uint32_t frames); /* audio thread */
/* Audio thread, before any voice is mixed: starts queued streams whose
 * predecessor reaches its mix point within this callback. */
/* Starts the queued stream when `s` reaches its mix point within this callback.
 * Returns 1 when it scheduled one (which may have a mix point in this callback too). */
int fa_stream_check_queue(struct fa_stream* s, fa_device* dev, uint32_t frames);
/* Validates (and clamps pan) a value for a stream or bus parameter. */
fa_result fa_param_check(fa_param param, float* value);
/* Bus gains (left, right) at frame offset `off` of the current callback, for a
 * block starting there (from_here 1) or ending there (0). */
void fa_bus_gains_at(struct fa_bus* b, fa_device* dev, int64_t off, int from_here, float* gl, float* gr);
int fa_stream_needs_work(struct fa_stream* s);
void fa_stream_fill(struct fa_stream* s, int64_t max_frames);  /* caller holds decode_lock */
/* Workers: a pending fa_stream_seek_async, then fa_stream_fill. Caller holds decode_lock.
 * Returns 0 when nothing could be done yet (the API lock was busy). */
int fa_stream_work(struct fa_stream* s, int64_t max_frames);
fa_frames fa_stream_position_now(struct fa_stream* s);
void fa_stream_free_memory(struct fa_stream* s);               /* event thread, from the graveyard */

float fa_stream_norm_target_db(struct fa_stream* s);          /* any thread */
void fa_stream_start_analysis(struct fa_stream* s);           /* idempotent */
/* From a scan thread: the final loudness of `s` (album_lufs NAN if unknown). */
void fa_stream_loudness_ready(struct fa_stream* s, const fa_loudness* l, double album_lufs);

/* bus.c (audio thread) */
void fa_buses_begin(fa_device* dev);                  /* takes new bus commands */
void fa_buses_end(fa_device* dev, uint32_t frames);   /* advances bus ramps */
void fa_buses_free_all(fa_device* dev);               /* fa_device_close, no callbacks running */

/* ---- Packet codecs (codec.c) -----------------------------------------------------
 * Decode one compressed packet at a time into interleaved F32: the built-in codecs
 * (PCM, MP3, AAC, AC-3, E-AC-3, Opus, Vorbis, FLAC, ALAC), then the systems' own
 * decoders (WMA on Windows, xHE-AAC). Nothing else is decoded. */

typedef struct fa_codec fa_codec;

typedef struct fa_codec_vtable {
    const char* name;
    /* Feeds one packet; afterwards codec->out holds out_frames decoded frames
     * (possibly 0) at codec->rate / codec->channels. */
    fa_result (*decode)(fa_codec* c, const uint8_t* data, size_t size);
    void (*flush)(fa_codec* c);
    void (*close)(fa_codec* c);
} fa_codec_vtable;

struct fa_codec {
    const fa_codec_vtable* vt;
    void* impl;
    float* out;
    fa_frames out_frames, out_cap;
    uint32_t rate, channels;
    char name[32];
    /* fa_dynamic_range of the stream, read on every packet (NULL = full range).
     * Set by the owner after fa_codec_open. */
    fa_atomic32* dynamic_range;
    /* Its audio can come out a packet or more after the packet that holds it (the
     * systems' decoders): the track decoder anchors the audio on the first packet. */
    int trails;
};

/* Opens a decoder for `p` (validated). FA_ERROR_UNSUPPORTED_FORMAT: nobody decodes it. */
fa_result fa_codec_open(fa_codec* c, const fa_codec_params* p);
void fa_codec_close(fa_codec* c);
/* Makes room for `frames` more frames in c->out (keeping what is there). */
fa_result fa_codec_reserve(fa_codec* c, fa_frames frames, uint32_t channels);

/* Device-rate conversion: the windowed-sinc resampler (resample.c) as a custom
 * backend for miniaudio's data converter (resample_device.c). */
extern ma_resampling_backend_vtable fa_sinc_resampler;

/* Float PCM from one rate and channel count to another (miniaudio's converter
 * with the sinc resampler), set up again whenever the input format changes. The
 * resampler holds back its look-ahead (about a millisecond), which comes out with
 * the next block, before the first block of another format, or on a flush. */
typedef struct fa_pcm_conv {
    ma_data_converter conv;
    int ready;
    int pending;                  /* audio went in since the last flush or reset */
    uint32_t rate, channels;      /* the input it is set up for */
} fa_pcm_conv;
/* Converts `frames` frames of `in` (rate, channels) to out_rate / out_channels into
 * *buf from its start, growing it (*cap counts floats); *out_frames = frames written.
 * Input already in the output format is copied (after what the converter held). */
fa_result fa_pcm_convert(fa_pcm_conv* c, const float* in, fa_frames frames, uint32_t rate, uint32_t channels,
                         uint32_t out_rate, uint32_t out_channels, float** buf, fa_frames* cap, fa_frames* out_frames);
/* The end of the input: appends the frames the converter still holds to the
 * *out_frames frames already in *buf, and adds them to *out_frames. */
fa_result fa_pcm_conv_flush(fa_pcm_conv* c, uint32_t out_channels, float** buf, fa_frames* cap, fa_frames* out_frames);
/* Output frames the converter holds back now (they come out after the next input). */
fa_frames fa_pcm_conv_held(fa_pcm_conv* c);
void fa_pcm_conv_reset(fa_pcm_conv* c);
void fa_pcm_conv_free(fa_pcm_conv* c);
/* Makes *buf hold at least `frames` frames of `channels`, keeping its contents (*cap counts floats). */
fa_result fa_float_reserve(float** buf, fa_frames* cap, fa_frames frames, uint32_t channels);

/* Sample conversion to interleaved F32. `kind`: FA_SAMPLE_* below. */
enum { FA_SAMPLE_U8 = 0, FA_SAMPLE_S8, FA_SAMPLE_S16, FA_SAMPLE_S24, FA_SAMPLE_S32, FA_SAMPLE_S64,
       FA_SAMPLE_F32, FA_SAMPLE_F64 };
/* planes: `channels` pointers when planar, else planes[0]. big_endian: byte order of
 * the data (packed formats from files); 0 = native order. */
void fa_convert_to_f32(const void* const* planes, int kind, int planar, int big_endian,
                       uint32_t channels, fa_frames frames, float* out);
/* fa_sample_format -> FA_SAMPLE_* and planar; -1 if invalid. */
int fa_sample_kind(fa_sample_format f, int* out_planar);

#ifdef _WIN32
/* codec_mf.c: Windows Media Audio (wmav1, wmav2, wmapro, wmalossless, wmavoice),
 * and AAC (LC, HE, HE v2) through Media Foundation's
 * decoders. FA_ERROR_UNSUPPORTED_FORMAT for any other codec, or when Media
 * Foundation or the decoder is not there (Windows N). */
fa_result fa_mf_codec_open(fa_codec* c, const fa_codec_params* p);
#endif
#ifdef __APPLE__
/* codec_at.c: AAC (LC, HE, HE v2, xHE where the system has it) through
 * AudioToolbox's AudioConverter. */
fa_result fa_at_codec_open(fa_codec* c, const fa_codec_params* p);
#endif
#ifdef __ANDROID__
/* codec_mediacodec.c: AAC through the NDK's AMediaCodec (libmediandk.so,
 * loaded at run time). */
fa_result fa_mc_codec_open(fa_codec* c, const fa_codec_params* p);
#endif

/* codec_os.c: stream details for the operating systems' AAC decoders. */
typedef struct fa_aac_asc_info {
    int object_type;            /* of the core: 2 = LC (after explicit SBR/PS signalling) */
    uint32_t rate;              /* core sampling rate */
    uint32_t ext_rate;          /* SBR output rate, 0 if not signalled */
    uint32_t channel_config;    /* 0: a program config element says */
    int sbr, ps;                /* 1 signalled, 0 signalled absent, -1 not said (implicit) */
} fa_aac_asc_info;
int fa_aac_parse_asc(const uint8_t* p, size_t n, fa_aac_asc_info* out);   /* 1 if valid */
uint32_t fa_aac_config_channels(uint32_t config);
/* The AudioSpecificConfig to hand an OS decoder (the extradata, else built for ADTS
 * frames); 0 if none can be had. */
size_t fa_aac_os_asc(const fa_codec_params* p, int he_guess, uint8_t* out, size_t cap);
/* Takes the header off a packet that is one whole ADTS frame: 1; not ADTS: 0;
 * several raw data blocks in the frame: -1. */
int fa_adts_strip(const uint8_t** data, size_t* size);
/* An MPEG-4 ES descriptor holding the AudioSpecificConfig (an 'esds' box's contents). */
size_t fa_aac_esds(const uint8_t* asc, size_t n, uint8_t* out, size_t cap);
/* From Core Audio channel labels to the library's channel order: src_of_dst[i] is the
 * decoder's channel for the library's channel i; 1 if it is not the identity. */
int fa_os_channel_map(const uint32_t* labels, uint32_t n, uint32_t* src_of_dst);

/* codec_flac.c: the built-in FLAC codec (dr_flac), one frame per packet.
 * FA_ERROR_UNSUPPORTED_FORMAT without STREAMINFO or usable parameters. */
fa_result fa_flac_codec_open(fa_codec* c, const fa_codec_params* p);
/* codec_alac.c: the built-in ALAC decoder, from the ALACSpecificConfig (bare, in
 * its 'alac' box, or after a 'frma' box) in the extradata. */
fa_result fa_alac_codec_open(fa_codec* c, const fa_codec_params* p);
typedef struct fa_alac_info {
    const uint8_t* config;      /* the 24-byte ALACSpecificConfig, within the extradata */
    uint32_t frame_length, rate, channels, bit_depth;
} fa_alac_info;
int fa_alac_parse_config(const uint8_t* p, size_t n, fa_alac_info* out);   /* 1 if valid */
/* Rate, channels and bit depth from a 34-byte FLAC STREAMINFO. */
void fa_flac_streaminfo_format(const uint8_t* si, uint32_t* rate, uint32_t* channels, uint32_t* bits);

/* codec_xiph.c: the built-in Opus and Vorbis codecs (libopus, libvorbis).
 * FA_ERROR_UNSUPPORTED_FORMAT for headers they cannot set up alone. */
fa_result fa_xiph_codec_open(fa_codec* c, const fa_codec_params* p);
/* Splits Vorbis extradata (Xiph lacing, or 16-bit lengths) into its three headers. */
int fa_xiph_split_headers(const uint8_t* p, size_t n, const uint8_t* out[3], size_t out_len[3]);

/* aac.c: the built-in AAC codec (AAC-LC, HE-AAC, HE-AAC v2): ADTS frames, or raw
 * access units after an AudioSpecificConfig in the extradata.
 * FA_ERROR_UNSUPPORTED_FORMAT for a configuration it does not decode (xHE-AAC). */
fa_result fa_aac_codec_open(fa_codec* c, const fa_codec_params* p);

/* dec_ac3.c: the built-in Dolby Digital / Dolby Digital Plus codec, and the
 * samples in an AC-3 or E-AC-3 packet from its frame headers (-1 if none). */
fa_result fa_ac3_codec_open(fa_codec* c, const fa_codec_params* p);
int64_t fa_ac3_packet_samples(const uint8_t* d, size_t n, uint32_t* out_rate);

/* ---- Push streams (push.c) ------------------------------------------------------ */

/* The push decoder (vtable of push streams' fa_decoder). */
extern const fa_decoder_vtable fa_decoder_push;
int fa_push_has_input(struct fa_push* p);        /* any thread */
void fa_push_free(struct fa_push* p);            /* fa_stream_close, under decode_lock */
fa_result fa_push_init_pcm(struct fa_stream* s, const fa_audio_format* f);
fa_result fa_push_init_packets(struct fa_stream* s, const fa_codec_params* params);
/* Under decode_lock: drop everything queued and decoded (fa_stream_push_flush), or
 * re-anchor the timeline at the next frame (a new ring). */
void fa_push_flush_locked(struct fa_push* p, int keep_hold);
void fa_push_restart_timeline(struct fa_push* p);
/* pts of the next frame the push decoder will hand out (ns). */
int64_t fa_push_next_pts(struct fa_push* p);
/* Total duration of everything pushed so far, as a pts end (ns); FA_PTS_NONE if nothing. */
int64_t fa_push_buffered_ns(struct fa_push* p);

/* engine.c */
void fa_engine_wake_workers(fa_engine* engine);
void fa_engine_track_stream(fa_engine* engine, struct fa_stream* s, int add);

#endif
