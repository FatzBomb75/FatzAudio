/* Streams: opening, decode-ahead buffering, the per-voice mix, transport,
 * parameters and events.
 *
 * Threads that touch a stream
 * - API threads: hold api_lock for anything that reconfigures the stream.
 * - Decode workers: hold decode_lock while filling the ring (fa_stream_fill).
 * - The audio thread: fa_stream_mix only. It reads the ring and atomics, never locks.
 *
 * To change what the audio thread reads (seek, detach), an API thread sets
 * `flushing` or removes the voice, waits until the audio callback has passed
 * (fa_device_wait_quiescent), then takes decode_lock so no worker runs either. */
#include "net.h"
#include "cdg.h"
#include "disc.h"
#include "encode.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FA_DECODE_CHUNK 1024
#define FA_CONVERT_CHUNK (FA_DECODE_CHUNK * 2)   /* most device frames one converter call makes */
#define FA_DEFAULT_BUFFER_MS 500
#define FA_PREFILL_MS 100
#define FA_DEZIPPER_NS 5000000   /* plain parameter changes glide over 5 ms */
#define FA_RAMP_BLOCK 16         /* ramp curves are evaluated every 16 frames */
#define FA_NORM_GLIDE_DOWN_DB_S 12.0f
#define FA_NORM_GLIDE_UP_DB_S 6.0f
#define FA_TEMPO_LATENCY_MS 150  /* decode-ahead while tempo, pitch or rate is in use */
#define FA_TEMPO_CHUNK 512       /* output frames per chain run, about: how finely ramps step */
#define FA_REVERSE_BLOCK_MS 500  /* reversed: source decoded per seek */
#define FA_STR_(x) #x
#define FA_STR(x) FA_STR_(x)
#define FA_NET_USER_AGENT "FatzAudio/" FA_STR(FA_VERSION_MAJOR) "." FA_STR(FA_VERSION_MINOR) "." FA_STR(FA_VERSION_PATCH)

FA_API void FA_CALL fa_open_options_init(fa_open_options* options)
{
    if (!options) return;
    memset(options, 0, sizeof(*options));
    options->struct_size = (uint32_t)sizeof(*options);
    options->audio_track = -1;
}

static int is_attached(struct fa_stream* s) { return s->device != NULL; }

/* A push stream's timestamps: made at open for every stream with `push`, and only
 * for those, so the sites below that run for push streams only never see NULL. */
static fa_push_times* ptimes_of(struct fa_stream* s) { return s->ptimes; }

/* A push stream's pts map, NULL for other streams. */
static fa_ptsmap* ptsmap_of(struct fa_stream* s) { return ptimes_of(s) ? &ptimes_of(s)->map : NULL; }

/* The tempo position map, NULL until the stream's first chain was made. */
static fa_tmap* tmap_of(struct fa_stream* s) { return (fa_tmap*)fa_load_ptr(&s->tmap); }

/* The user DSP chain, on whichever thread decodes (caller holds decode_lock). */
static void run_dsps(struct fa_stream* s, float* buf, fa_frames n, uint32_t channels, uint32_t rate)
{
    fa_dsp_list* list = (fa_dsp_list*)fa_load_ptr(&s->dsps);
    uint32_t i;
    if (!list || n <= 0) return;
    for (i = 0; i < list->count; i++) list->items[i].callback(buf, n, channels, rate, list->items[i].user);
}

static fa_frames marker(struct fa_stream* s, int m) { return fa_load64(&s->markers[m]); }

/* ---- open / close ------------------------------------------------------- */

void fa_stream_free_memory(struct fa_stream* s)
{
    free(s->carry);
    free(s->rev_buf);
    free(tmap_of(s));
    free(fa_load_ptr(&s->eq));
    free(s->ptimes);
    free(fa_load_ptr(&s->dsps));
    fa_tap_free((fa_tap*)fa_load_ptr(&s->tap));
    fa_grid_data_free(&s->grid);
    fa_mutex_destroy(&s->grid_lock);
    fa_mutex_destroy(&s->tempo_lock);
    fa_mutex_destroy(&s->cdg_lock);
    fa_mutex_destroy(&s->norm_lock);
    fa_mutex_destroy(&s->cb_lock);
    fa_mutex_destroy(&s->decode_lock);
    fa_mutex_destroy(&s->api_lock);
    free(s);
}

static void release_chain(struct fa_stream* s)
{
    if (s->converter_ready) { ma_data_converter_uninit(&s->converter, NULL); s->converter_ready = 0; }
    fa_ring_free(&s->ring);
    free(s->in_buf); s->in_buf = NULL;
    free(s->mat_buf); s->mat_buf = NULL;
    free(s->hist); s->hist = NULL;
    s->hist_cap = 0;
    s->hist_chain = 0;
    s->chain_failed = 0;
    if (s->tempo) { fa_tempo_free(s->tempo); free(s->tempo); s->tempo = NULL; }
    free(s->rd_in); s->rd_in = NULL;
    s->rd_out = NULL;
    s->rd_count = s->rd_off = 0;
    s->chain_buf = NULL;
    s->tempo_drained = 0;
    s->jumped_back = 0;
    fa_store32(&s->tempo_live, 0);
    s->in_cap = s->in_count = s->in_offset = 0;
    s->decoded = 0;
    s->draining = 0;
}

/* Frees a stream that was never published to anyone (failed open). */
static void stream_discard(struct fa_stream* s)
{
    if (s->run_meter_ready) fa_meter_free(&s->run_meter);
    fa_stream_cdg_close(s);
    free(s->path);
    free(s->matrix);
    s->midi = NULL;
    s->tracker = NULL;
    if (s->decoder_ready) fa_decoder_close(&s->decoder);
    fa_soundfont_release(s->soundfont);
    fa_push_free(s->push);
    fa_source_close(&s->source);
    fa_net_close(s->net);
    fa_stream_free_memory(s);
}

static fa_result stream_create(fa_engine* engine, const fa_open_options* options, struct fa_stream** out)
{
    int i;
    struct fa_stream* s = (struct fa_stream*)calloc(1, sizeof(*s));
    if (!s) return FA_ERROR_OUT_OF_MEMORY;
    if (fa_mutex_init(&s->api_lock) != 0) { free(s); return FA_ERROR_GENERIC; }
    if (fa_mutex_init(&s->decode_lock) != 0) { fa_mutex_destroy(&s->api_lock); free(s); return FA_ERROR_GENERIC; }
    if (fa_mutex_init(&s->cb_lock) != 0) {
        fa_mutex_destroy(&s->decode_lock); fa_mutex_destroy(&s->api_lock); free(s);
        return FA_ERROR_GENERIC;
    }
    if (fa_mutex_init(&s->norm_lock) != 0) {
        fa_mutex_destroy(&s->cb_lock); fa_mutex_destroy(&s->decode_lock); fa_mutex_destroy(&s->api_lock); free(s);
        return FA_ERROR_GENERIC;
    }
    if (fa_mutex_init(&s->cdg_lock) != 0) {
        fa_mutex_destroy(&s->norm_lock); fa_mutex_destroy(&s->cb_lock); fa_mutex_destroy(&s->decode_lock);
        fa_mutex_destroy(&s->api_lock); free(s);
        return FA_ERROR_GENERIC;
    }
    if (fa_mutex_init(&s->tempo_lock) != 0) {
        fa_mutex_destroy(&s->cdg_lock); fa_mutex_destroy(&s->norm_lock); fa_mutex_destroy(&s->cb_lock);
        fa_mutex_destroy(&s->decode_lock); fa_mutex_destroy(&s->api_lock); free(s);
        return FA_ERROR_GENERIC;
    }
    if (fa_mutex_init(&s->grid_lock) != 0) {
        fa_mutex_destroy(&s->tempo_lock); fa_mutex_destroy(&s->cdg_lock); fa_mutex_destroy(&s->norm_lock);
        fa_mutex_destroy(&s->cb_lock); fa_mutex_destroy(&s->decode_lock); fa_mutex_destroy(&s->api_lock); free(s);
        return FA_ERROR_GENERIC;
    }
    for (i = 0; i < FA_TEMPO_PARAMS; i++) fa_tparam_init(&s->tparams[i], FA_PARAM_RATE + i == FA_PARAM_PITCH ? 0.0f : 1.0f);
    s->tempo_latency_ms = FA_TEMPO_LATENCY_MS;
    s->engine = engine;
    s->flags = options ? options->flags : 0;
    fa_store32(&s->loop, (s->flags & FA_OPEN_LOOP) ? 1 : 0);
    s->mute_next = -1;               /* nothing mixed yet: the mute starts as set */
    fa_store64(&s->seek_pending, -1);
    s->buffer_ms = FA_DEFAULT_BUFFER_MS;
    if (options && options->struct_size >= offsetof(fa_open_options, buffer_ms) + sizeof(options->buffer_ms) &&
        options->buffer_ms) {
        s->buffer_ms = options->buffer_ms;
        if (s->buffer_ms < 50) s->buffer_ms = 50;
        if (s->buffer_ms > 10000) s->buffer_ms = 10000;
    }
    s->audio_track = options ? options->audio_track : -1;
    if (options && options->struct_size >= offsetof(fa_open_options, dynamic_range) + sizeof(options->dynamic_range) &&
        options->dynamic_range >= FA_DYNAMIC_RANGE_FULL && options->dynamic_range <= FA_DYNAMIC_RANGE_NIGHT)
        fa_store32(&s->dynamic_range, options->dynamic_range);
    /* MIDI files are played with the stream's soundfont, else the engine's. */
    if (options && options->struct_size >= offsetof(fa_open_options, soundfont) + sizeof(options->soundfont) &&
        options->soundfont)
        s->soundfont = fa_soundfont_retain(options->soundfont);
    else
        s->soundfont = fa_engine_soundfont(engine);
    if (options && options->struct_size >= offsetof(fa_open_options, text_code_page) + sizeof(options->text_code_page) &&
        (options->text_code_page == FA_CODE_PAGE_UTF8 || options->text_code_page == FA_CODE_PAGE_WINDOWS_1252 ||
         options->text_code_page == FA_CODE_PAGE_SHIFT_JIS))
        s->text_code_page = options->text_code_page;
    s->loud.integrated_lufs = s->loud.range_lu = s->loud.true_peak_dbtp = NAN;
    fa_store_float(&s->norm_track, NAN);
    fa_store_float(&s->norm_album, NAN);
    fa_store_float(&s->norm_est, NAN);
    fa_store32(&s->state, FA_STATE_STOPPED);
    fa_store64(&s->meta_frame, INT64_MAX);
    for (i = 0; i < FA_MARKER_COUNT; i++) fa_store64(&s->markers[i], FA_FRAMES_UNSET);
    for (i = 0; i < FA_POSITION_EVENT_SLOTS; i++) fa_store64(&s->pos_events[i], FA_FRAMES_UNSET);
    for (i = 0; i < FA_STREAM_LANES; i++) fa_lane_init(&s->lanes[i], i == FA_PARAM_PAN ? 0.0f : 1.0f);
    {
        fa_shape eq;
        fa_shape_from_ramp(NULL, FA_CURVE_EQUAL_POWER, &eq);
        fa_shape_store(&s->fade_shape[FA_FADE_IN], &eq);
        fa_shape_store(&s->fade_shape[FA_FADE_OUT], &eq);
        fa_shape_store(&s->xfade_shape, &eq);
    }
    *out = s;
    return FA_OK;
}

/* On failure the source is NOT closed here, so open_io can hand ownership back. */
static fa_result stream_open_decoder(struct fa_stream* s, const char* name_hint, const fa_open_options* options)
{
    fa_decoder_env env;
    fa_result r;
    env.dynamic_range = &s->dynamic_range;
    if (!s->soundfont) {
        /* A MIDI file and no soundfont from the app: the OS's own instruments. */
        const uint8_t* head;
        size_t n = fa_source_peek(&s->source, &head);
        if (fa_probe_midi(head, n, 0)) s->soundfont = fa_engine_system_soundfont(s->engine);
    }
    env.soundfont = s->soundfont;
    env.midi_rate = fa_engine_midi_rate(s->engine);
    env.text_code_page = s->text_code_page;
    r = fa_decoder_open(&s->decoder, &s->source, name_hint, options ? options->mime_hint : NULL,
                        options ? options->audio_track : -1, &env);
    if (r != FA_OK) return r;
    if ((s->flags & FA_OPEN_PRESCAN) && s->decoder.vt->prescan) s->decoder.vt->prescan(&s->decoder);
    s->decoder_ready = 1;
    fa_source_drop_prefix(&s->source);
    s->midi = fa_midi_of(&s->decoder);
    s->tracker = fa_tracker_of(&s->decoder);
    if (!s->midi) {
        /* Only MIDI streams use the soundfont: don't keep a replaced bank alive. */
        s->decoder.soundfont = NULL;
        fa_soundfont_release(s->soundfont);
        s->soundfont = NULL;
    }
    /* MIDI and tracker live controls are heard after the decode-ahead: keep it short unless
     * the app chose its own. */
    if ((s->midi || s->tracker) && !(options && options->struct_size >= offsetof(fa_open_options, buffer_ms) + sizeof(options->buffer_ms) &&
                     options->buffer_ms))
        s->buffer_ms = FA_MIDI_BUFFER_MS;
    /* Running loudness estimate for playback; decode-only streams are never normalized. */
    if (!(s->flags & FA_OPEN_DECODE_ONLY) &&
        fa_meter_init(&s->run_meter, s->decoder.format.sample_rate, s->decoder.format.channels, 0, FA_RUNNING_SUBS) == FA_OK)
        s->run_meter_ready = 1;
    return FA_OK;
}

/* Analysis starts at open when normalization is on, so the exact gain is often
 * known before playback even starts. */
static void stream_opened(struct fa_stream* s)
{
    if (fa_load32(&s->engine->norm_mode) != FA_NORMALIZE_OFF && !(s->flags & (FA_OPEN_NO_NORMALIZE | FA_OPEN_DECODE_ONLY)))
        fa_stream_start_analysis(s);
}


static int source_is_zip(fa_source* src)
{
    const uint8_t* p;
    size_t n = fa_source_peek(src, &p);
    return fa_zip_probe(p, n);
}

/* Prefers entries named like the .cdg ("song.mp3" for "song.cdg"). */
static int same_stem(const char* a, const char* cdg_name)
{
    size_t n = strlen(cdg_name) - 4; /* without ".cdg" */
    return strncmp(a, cdg_name, n) == 0 && a[n] == '.';
}

/* Moves s->source (the archive) to `arc`, ready to open one of its entries. */
static void take_archive(struct fa_stream* s, fa_source* arc)
{
    *arc = s->source;
    memset(&s->source, 0, sizeof(s->source));
}

static uint64_t zip_memory_limit(const fa_engine* engine)
{
    return engine->config.zip_memory_limit_mb ? (uint64_t)engine->config.zip_memory_limit_mb << 20
                                              : FA_ZIP_DEFAULT_MEMORY_LIMIT;
}

/* Entry `e`, unpacked into a temp file in the engine's folder (deleted when closed). */
static fa_result unpack_to_temp(struct fa_stream* s, fa_source* arc, const fa_zip_entry* e)
{
    fa_engine* engine = s->engine;
    FILE* f;
    fa_result r;
    if (!engine->temp_folder) return FA_ERROR_TOO_LARGE;
    /* Made again here: the app may have created the directory after the engine. */
    fa_mkdir_utf8(engine->config.temp_directory);
    fa_mkdir_utf8(engine->temp_folder);
    f = fa_temp_file_create(engine->temp_folder);
    if (!f) return FA_ERROR_IO;
    r = fa_zip_extract_file(arc, e, f);
    if (r != FA_OK || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return r != FA_OK ? r : FA_ERROR_IO; }
    memset(&s->source, 0, sizeof(s->source));
    s->source.kind = FA_SOURCE_FILE;
    s->source.file = f;
    s->source.seekable = 1;
    s->source.length = (int64_t)e->size;
    return FA_OK;
}

/* Makes s->source entry `e` of the archive `arc`. A stored entry is a window on the
 * archive itself, which moves into s->source (*moved = 1); a compressed one is
 * unpacked into memory, or into a temp file past the memory limit (or when memory
 * runs out). */
static fa_result open_zip_entry(struct fa_stream* s, fa_source* arc, const fa_zip_entry* e, int* moved)
{
    int64_t offset;
    uint8_t* data;
    fa_result r;
    *moved = 0;
    if (e->method == 0) {
        r = fa_zip_entry_offset(arc, e, &offset);
        if (r != FA_OK) return r;
        s->source = *arc;
        r = fa_source_set_window(&s->source, offset, (int64_t)e->size);
        if (r != FA_OK) { *arc = s->source; memset(&s->source, 0, sizeof(s->source)); return r; }
        *moved = 1;
        return FA_OK;
    }
    r = fa_zip_extract(arc, e, zip_memory_limit(s->engine), &data);
    if (r == FA_OK) {
        fa_source_open_memory_ref(&s->source, data, (size_t)e->size);
        s->source.owned = data;
        return FA_OK;
    }
    if (r != FA_ERROR_TOO_LARGE && r != FA_ERROR_OUT_OF_MEMORY) return r;
    if (r == FA_ERROR_OUT_OF_MEMORY && !s->engine->temp_folder) return r;
    return unpack_to_temp(s, arc, e);
}

/* Undoes open_zip_entry after the decoder refused the entry. FA_ERROR_IO when the
 * archive cannot be read as a whole again (app callbacks that failed to seek). */
static fa_result close_zip_entry(struct fa_stream* s, fa_source* arc, int moved)
{
    fa_result r = FA_OK;
    if (s->decoder_ready) { fa_decoder_close(&s->decoder); s->decoder_ready = 0; }
    if (moved) {
        r = fa_source_set_window(&s->source, 0, s->source.whole_length);
        *arc = s->source;
        memset(&s->source, 0, sizeof(s->source));
    } else {
        fa_source_close(&s->source);
    }
    return r;
}

/* A ZIP archive in s->source (karaoke downloads: song.mp3 + song.cdg). Only its
 * directory and the entries used are read. The audio entry becomes the stream's
 * source, the .cdg entry its graphics. On failure s->source is the archive again. */
static fa_result open_zip(struct fa_stream* s, const fa_open_options* options, int* in_place)
{
    fa_source arc;
    fa_zip_entry* entries;
    fa_cdg_data* graphics = NULL;
    uint32_t count, i, pass;
    int cdg = -1, moved = 0;
    fa_result r, skipped = FA_ERROR_UNSUPPORTED_FORMAT;
    *in_place = 0;
    r = fa_zip_list(&s->source, &entries, &count);
    if (r != FA_OK) return r;
    take_archive(s, &arc);
    s->zip_size = arc.length;
    for (i = 0; i < count && cdg < 0; i++)
        if (fa_cdg_is_cdg_name(entries[i].name)) cdg = (int)i;
    if (cdg >= 0 && !(s->flags & FA_OPEN_NO_CDG)) {
        uint8_t* bytes;
        /* Read first: a stored audio entry takes the archive's source with it.
         * Damaged graphics leave a playable song: the stream simply has none. */
        if (fa_zip_extract(&arc, &entries[cdg], zip_memory_limit(s->engine), &bytes) == FA_OK &&
            fa_cdg_data_adopt(bytes, (size_t)entries[cdg].size, &graphics) != FA_OK)
            graphics = NULL;
    }
    r = FA_ERROR_UNSUPPORTED_FORMAT;
    for (pass = 0; pass < 2 && r != FA_OK && r != FA_ERROR_NOT_FOUND && r != FA_ERROR_IO; pass++) {
        for (i = 0; i < count && r != FA_OK; i++) {
            fa_result opened;
            int preferred = cdg >= 0 && same_stem(entries[i].name, entries[cdg].name);
            if ((int)i == cdg || fa_cdg_is_cdg_name(entries[i].name) || preferred != (pass == 0)) continue;
            opened = open_zip_entry(s, &arc, &entries[i], &moved);
            if (opened != FA_OK) {
                /* Too big to unpack, out of memory or a temp file that failed is worth
                 * telling the app, unless another entry plays. */
                if (opened == FA_ERROR_TOO_LARGE || opened == FA_ERROR_OUT_OF_MEMORY || opened == FA_ERROR_IO)
                    if (skipped == FA_ERROR_UNSUPPORTED_FORMAT || opened == FA_ERROR_TOO_LARGE) skipped = opened;
                continue;
            }
            r = stream_open_decoder(s, entries[i].name, options);
            if (r == FA_OK) { snprintf(s->path_name, sizeof(s->path_name), "%s", entries[i].name); break; }
            if (close_zip_entry(s, &arc, moved) != FA_OK) { r = FA_ERROR_IO; moved = 0; break; }
            moved = 0;
            /* The audio_track asked for is missing: no other entry is tried. */
            if (r == FA_ERROR_NOT_FOUND) break;
        }
    }
    free(entries);
    if (r == FA_ERROR_UNSUPPORTED_FORMAT) r = skipped;
    if (r != FA_OK) {
        fa_cdg_data_free(graphics);
        s->source = arc;   /* stream_discard closes it (and the app's callbacks) */
        return r;
    }
    if (!moved) {
        /* The audio was unpacked: the archive is no longer read, but the app's
         * callbacks are closed with the stream, as for any other source. */
        if (arc.io.close) {
            s->source.io.close = arc.io.close;
            s->source.io_user = arc.io_user;
            arc.io.close = NULL;
        }
        fa_source_close(&arc);
    }
    *in_place = moved;
    if (graphics) fa_stream_cdg_attach(s, graphics);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_open_disc(fa_engine* engine, fa_disc* disc, int32_t track, const fa_open_options* options, fa_stream** out_stream)
{
    struct fa_stream* s; fa_result r;
    if (out_stream) *out_stream = NULL;
    if (!engine || !disc || !out_stream) return FA_ERROR_INVALID_ARG;
    r = stream_create(engine, options, &s);
    if (r != FA_OK) return r;
    r = fa_disc_decoder_open(&s->decoder, &s->source, disc, track, s);
    if (r != FA_OK) { stream_discard(s); return r; }
    s->decoder_ready = 1;
    if (!(s->flags & FA_OPEN_DECODE_ONLY) &&
        fa_meter_init(&s->run_meter, s->decoder.format.sample_rate, s->decoder.format.channels, 0, FA_RUNNING_SUBS) == FA_OK)
        s->run_meter_ready = 1;
    stream_opened(s);
    *out_stream = s;
    return FA_OK;
}

/* A TrackNN.cda file: the drive is the path's drive letter (Windows only). */
static fa_result open_cda(fa_engine* engine, const char* path, int track, const fa_open_options* options, fa_stream** out_stream)
{
#ifdef _WIN32
    char drive[3];
    fa_disc* disc;
    fa_result r;
    if (!((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) || path[1] != ':')
        return FA_ERROR_UNSUPPORTED_FORMAT;
    drive[0] = path[0]; drive[1] = ':'; drive[2] = 0;
    r = fa_disc_open(drive, NULL, &disc);
    if (r != FA_OK) return r;
    r = fa_stream_open_disc(engine, disc, track, options, out_stream);
    fa_disc_close(disc);   /* the stream keeps the drive open */
    return r;
#else
    (void)engine; (void)path; (void)track; (void)options; (void)out_stream;
    return FA_ERROR_UNSUPPORTED_FORMAT;
#endif
}

FA_API fa_result FA_CALL fa_stream_open_path(fa_engine* engine, const char* path, const fa_open_options* options, fa_stream** out_stream)
{
    struct fa_stream* s; fa_result r;
    int zip = 0, in_place = 0;
    if (!engine || !path || !out_stream) return FA_ERROR_INVALID_ARG;
    *out_stream = NULL;
    r = stream_create(engine, options, &s);
    if (r != FA_OK) return r;
    r = fa_source_open_path(&s->source, path);
    if (r == FA_OK && source_is_zip(&s->source)) {
        zip = 1;
        r = open_zip(s, options, &in_place);
    } else if (r == FA_OK) {
        const uint8_t* head;
        size_t n = fa_source_peek(&s->source, &head);
        int track = fa_disc_cda_track(head, n);
        if (track > 0) {
            /* D:\Track01.cda: the track it stands for, on the drive it is on. */
            stream_discard(s);
            return open_cda(engine, path, track, options, out_stream);
        }
        r = stream_open_decoder(s, path, options);
    }
    /* Loudness scans and tags reopen path streams by name: a stored ZIP entry as
     * its bytes in the archive. An unpacked one cannot be reopened. */
    if (r == FA_OK && (!zip || in_place)) {
        size_t n = strlen(path) + 1;
        s->path = (char*)malloc(n);
        if (s->path) memcpy(s->path, path, n);
        else r = FA_ERROR_OUT_OF_MEMORY;
        if (in_place) { s->path_offset = s->source.base; s->path_length = s->source.length; }
    }
    if (r != FA_OK) { stream_discard(s); return r; }
    if (!zip && !(s->flags & FA_OPEN_NO_CDG)) {
        fa_cdg_data* graphics = fa_cdg_find_sibling(path);
        if (graphics) fa_stream_cdg_attach(s, graphics);
    }
    stream_opened(s);
    *out_stream = s;
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_open_memory(fa_engine* engine, const void* data, size_t size, const fa_open_options* options, fa_stream** out_stream)
{
    struct fa_stream* s; fa_result r;
    if (!engine || !data || size == 0 || !out_stream) return FA_ERROR_INVALID_ARG;
    *out_stream = NULL;
    r = stream_create(engine, options, &s);
    if (r != FA_OK) return r;
    r = fa_source_open_memory(&s->source, data, size);
    if (r == FA_OK && source_is_zip(&s->source)) {
        int in_place;
        /* A stored entry plays from the copy of the archive, a compressed one
         * from its own unpacked copy (the archive's is then freed). */
        r = open_zip(s, options, &in_place);
    } else if (r == FA_OK) {
        r = stream_open_decoder(s, NULL, options);
    }
    if (r != FA_OK) { stream_discard(s); return r; }
    stream_opened(s);
    *out_stream = s;
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_open_io(fa_engine* engine, const fa_io_callbacks* io, void* user, const fa_open_options* options, fa_stream** out_stream)
{
    struct fa_stream* s; fa_result r;
    if (out_stream) *out_stream = NULL;
    if (!engine || !io || !io->read || !out_stream) return FA_ERROR_INVALID_ARG;
    r = stream_create(engine, options, &s);
    if (r != FA_OK) return r;
    fa_source_open_io(&s->source, io, user);
    if (source_is_zip(&s->source)) {
        int in_place;
        r = open_zip(s, options, &in_place);
    } else {
        r = stream_open_decoder(s, NULL, options);
    }
    if (r != FA_OK) {
        s->source.io.close = NULL; /* failure: the caller keeps ownership of `user` */
        stream_discard(s);
        return r;
    }
    stream_opened(s);
    *out_stream = s;
    return FA_OK;
}

/* Push and packet streams: no source, the push decoder instead (push.c). */
static fa_result open_pushed(struct fa_stream* s)
{
    s->ptimes = (fa_push_times*)calloc(1, sizeof(*s->ptimes));   /* an empty map */
    if (!s->ptimes) return FA_ERROR_OUT_OF_MEMORY;
    s->decoder_ready = 1;
    if (!(s->flags & FA_OPEN_DECODE_ONLY) &&
        fa_meter_init(&s->run_meter, s->decoder.format.sample_rate, s->decoder.format.channels, 0, FA_RUNNING_SUBS) == FA_OK)
        s->run_meter_ready = 1;
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_open_push(fa_engine* engine, const fa_audio_format* format, const fa_open_options* options, fa_stream** out_stream)
{
    struct fa_stream* s; fa_result r;
    if (out_stream) *out_stream = NULL;
    if (!engine || !format || !out_stream) return FA_ERROR_INVALID_ARG;
    if (options && options->flags & FA_OPEN_LOOP) return FA_ERROR_INVALID_ARG;
    r = stream_create(engine, options, &s);
    if (r != FA_OK) return r;
    r = fa_push_init_pcm(s, format);
    if (r == FA_OK) r = open_pushed(s);
    if (r != FA_OK) { stream_discard(s); return r; }
    stream_opened(s);
    *out_stream = s;
    return FA_OK;
}

FA_API void FA_CALL fa_codec_params_init(fa_codec_params* params)
{
    if (!params) return;
    memset(params, 0, sizeof(*params));
    params->struct_size = (uint32_t)sizeof(*params);
}

FA_API fa_result FA_CALL fa_stream_open_packets(fa_engine* engine, const fa_codec_params* params, const fa_open_options* options, fa_stream** out_stream)
{
    struct fa_stream* s; fa_result r;
    if (out_stream) *out_stream = NULL;
    if (!engine || !params || !out_stream || params->struct_size < sizeof(fa_codec_params)) return FA_ERROR_INVALID_ARG;
    if (!params->codec || !params->codec[0] || strlen(params->codec) >= 32) return FA_ERROR_INVALID_ARG;
    if (params->sample_rate < 1000 || params->sample_rate > 768000 || params->channels == 0 || params->channels > 32)
        return FA_ERROR_INVALID_ARG;
    if (params->extradata_size > 0 && !params->extradata) return FA_ERROR_INVALID_ARG;
    if (options && options->flags & FA_OPEN_LOOP) return FA_ERROR_INVALID_ARG;
    r = stream_create(engine, options, &s);
    if (r != FA_OK) return r;
    r = fa_push_init_packets(s, params);
    if (r == FA_OK) r = open_pushed(s);
    if (r != FA_OK) { stream_discard(s); return r; }
    stream_opened(s);
    *out_stream = s;
    return FA_OK;
}

static char* copy_str(const char* text, fa_result* r)
{
    char* c = fa_strdup(text);
    if (text && !c) *r = FA_ERROR_OUT_OF_MEMORY;
    return c;
}

FA_API void FA_CALL fa_net_options_init(fa_net_options* options)
{
    if (!options) return;
    memset(options, 0, sizeof(*options));
    options->struct_size = (uint32_t)sizeof(*options);
}

/* The settings of fa_open_options.net, with defaults, into a config the download
 * buffer owns. Older callers' shorter structs are read only as far as they go. */
static fa_result net_config(const char* url, const fa_open_options* options, fa_net_config* cfg)
{
    fa_net_options o;
    fa_result r = FA_OK;
    fa_net_options_init(&o);
    if (options && options->struct_size >= offsetof(fa_open_options, net) + sizeof(options->net) && options->net) {
        size_t sz = options->net->struct_size;
        if (sz < sizeof(uint32_t)) return FA_ERROR_INVALID_ARG;
        if (sz > sizeof(o)) sz = sizeof(o);
        memcpy(&o, options->net, sz);
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->url = copy_str(url, &r);
    cfg->user_agent = copy_str(o.user_agent ? o.user_agent : FA_NET_USER_AGENT, &r);
    cfg->headers = copy_str(o.headers, &r);
    cfg->proxy = copy_str(o.proxy, &r);
    cfg->timeout_ms = o.timeout_ms ? o.timeout_ms : FA_NET_DEFAULT_TIMEOUT_MS;
    cfg->prebuffer_ms = o.prebuffer_ms ? o.prebuffer_ms : FA_NET_DEFAULT_PREBUFFER_MS;
    cfg->reconnect_ms = (o.flags & FA_NET_NO_RECONNECT) ? 0 : (o.reconnect_ms ? o.reconnect_ms : FA_NET_DEFAULT_RECONNECT_MS);
    cfg->flags = o.flags;
    cfg->hls_max_bandwidth = o.hls_max_bandwidth;
    if (cfg->prebuffer_ms > 30000) cfg->prebuffer_ms = 30000;
    if (r != FA_OK) { free(cfg->url); free(cfg->user_agent); free(cfg->headers); free(cfg->proxy); }
    return r;
}

FA_API fa_result FA_CALL fa_stream_open_url(fa_engine* engine, const char* url, const fa_open_options* options, fa_stream** out_stream)
{
    struct fa_stream* s;
    fa_net_config cfg;
    fa_io_callbacks io;
    fa_url parsed;
    fa_result r;
    if (out_stream) *out_stream = NULL;
    if (!engine || !url || !out_stream) return FA_ERROR_INVALID_ARG;
    if (fa_url_parse(url, &parsed) != FA_OK) return FA_ERROR_INVALID_ARG;
    r = net_config(url, options, &cfg);
    if (r != FA_OK) return r;
    r = stream_create(engine, options, &s);
    if (r != FA_OK) { free(cfg.url); free(cfg.user_agent); free(cfg.headers); free(cfg.proxy); return r; }
    r = fa_net_open(&cfg, &s->net);           /* takes the config's strings */
    if (r == FA_OK) {
        fa_net_io(s->net, &io);
        io.close = NULL;                      /* the stream closes the net itself */
        fa_source_open_io(&s->source, &io, s->net);
        s->source.remote = 1;
        if (fa_net_is_hls(s->net)) {
            s->decoder.vt = &fa_decoder_hls;
            s->decoder.src = &s->source;
            s->decoder.length = -1;
            r = fa_decoder_hls.open(&s->decoder);
            if (r == FA_OK) {
                s->decoder.format.sample_format = FA_FORMAT_F32;
                s->decoder_ready = 1;
                if (options && options->audio_track > 0) r = FA_ERROR_NOT_FOUND;
                else if (!(s->flags & FA_OPEN_DECODE_ONLY) &&
                         fa_meter_init(&s->run_meter, s->decoder.format.sample_rate, s->decoder.format.channels, 0,
                                       FA_RUNNING_SUBS) == FA_OK)
                    s->run_meter_ready = 1;
            } else {
                memset(&s->decoder, 0, sizeof(s->decoder));
            }
        } else {
            char mime[128];
            fa_open_options o;
            fa_open_options_init(&o);
            if (options) memcpy(&o, options, options->struct_size < sizeof(o) ? options->struct_size : sizeof(o));
            /* The server's Content-Type is a tie-breaker when the app gave none. */
            if (!o.mime_hint && fa_net_get_header(s->net, "Content-Type", mime, sizeof(mime)) == FA_OK) {
                mime[strcspn(mime, "; ")] = 0;
                o.mime_hint = mime;
            }
            r = stream_open_decoder(s, parsed.path, &o);
        }
    }
    if (r == FA_OK) fa_net_set_sample_rate(s->net, s->decoder.format.sample_rate);
    if (r != FA_OK) { stream_discard(s); return r; }
    stream_opened(s);
    *out_stream = s;
    return FA_OK;
}

FA_API void FA_CALL fa_stream_close(fa_stream* stream)
{
    fa_scan_job* job;
    fa_engine* e;
    uintptr_t self;
    if (!stream) return;
    e = stream->engine;
    fa_mixer_stream_leaving(stream, NULL);   /* the mixer's event thread lets go of a deck's stream */
    fa_stream_cdg_detach(stream);   /* CD+G surface calls on other threads finish first */
    fa_stream_grid_closing(stream);   /* a module's grid rendered in the background stops */
    /* A worker or an encoder may be waiting in a network read while holding
     * decode_lock: end the download first so the read returns. */
    fa_net_interrupt(stream->net);
    fa_encode_stream_closing(stream);   /* encoders recording or reading it lose their source */
    fa_stream_detach_internal(stream);   /* off its deck already (above) */

    /* A scan of this stream reads its file or memory: stop it before they go. */
    fa_mutex_lock(&stream->norm_lock);
    job = stream->scan_job;
    stream->scan_job = NULL;
    fa_mutex_unlock(&stream->norm_lock);
    fa_scan_release(job);

    /* Stop event delivery and wait for a callback that is running right now,
     * unless we are that callback. */
    fa_store32(&stream->closed, 1);
    self = fa_thread_current_id();
    if ((int64_t)self != fa_load64(&e->event_thread_id))
        while (fa_load32(&stream->in_callback)) fa_sleep_ms(1);

    fa_mutex_lock(&stream->decode_lock);
    release_chain(stream);
    stream->midi = NULL;
    stream->tracker = NULL;
    if (stream->decoder_ready) { fa_decoder_close(&stream->decoder); stream->decoder_ready = 0; }
    fa_soundfont_release(stream->soundfont);
    stream->soundfont = NULL;
    fa_push_free(stream->push);
    stream->push = NULL;
    fa_source_close(&stream->source); /* the app's io close, exactly once */
    fa_net_close(stream->net);
    stream->net = NULL;
    if (stream->run_meter_ready) { fa_meter_free(&stream->run_meter); stream->run_meter_ready = 0; }
    free(stream->path);
    stream->path = NULL;
    free(stream->matrix);
    stream->matrix = NULL;
    fa_mutex_unlock(&stream->decode_lock);
    fa_stream_cdg_close(stream); /* its surfaces stay until the app destroys them */

    /* The memory goes to the event thread, which frees it after delivering (and
     * skipping) every event already queued for this stream. */
    fa_mutex_lock(&e->list_lock);
    stream->next = e->graveyard;
    e->graveyard = stream;
    fa_cond_broadcast(&e->event_cond);
    fa_mutex_unlock(&e->list_lock);
}

/* ---- info --------------------------------------------------------------- */

FA_API fa_result FA_CALL fa_stream_get_info(fa_stream* stream, fa_stream_info* out_info)
{
    if (!stream || !out_info) return FA_ERROR_INVALID_ARG;
    fa_decoder_get_info(&stream->decoder, out_info); /* fixed after open */
    if (stream->zip_size > 0) out_info->file_size = stream->zip_size;
    out_info->has_cdg = fa_stream_cdg_present(stream);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_set_dynamic_range(fa_stream* stream, fa_dynamic_range mode)
{
    if (!stream || mode < FA_DYNAMIC_RANGE_FULL || mode > FA_DYNAMIC_RANGE_NIGHT) return FA_ERROR_INVALID_ARG;
    fa_store32(&stream->dynamic_range, (int32_t)mode);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_dynamic_range(fa_stream* stream, fa_dynamic_range* out_mode)
{
    if (!stream || !out_mode) return FA_ERROR_INVALID_ARG;
    *out_mode = (fa_dynamic_range)fa_load32(&stream->dynamic_range);
    return FA_OK;
}

/* ---- web stream info ----------------------------------------------------- */

FA_API fa_result FA_CALL fa_stream_get_metadata(fa_stream* stream, fa_metadata* out_metadata)
{
    fa_net_meta m;
    const char* dash;
    if (!stream || !out_metadata) return FA_ERROR_INVALID_ARG;
    memset(out_metadata, 0, sizeof(*out_metadata));
    if (!stream->net) return FA_OK;
    fa_net_get_station(stream->net, out_metadata);
    fa_net_meta_at(stream->net, fa_stream_position_now(stream), &m);
    if (m.seq == 0) return FA_OK;
    snprintf(out_metadata->stream_title, sizeof(out_metadata->stream_title), "%s", m.title);
    snprintf(out_metadata->url, sizeof(out_metadata->url), "%s", m.url);
    out_metadata->seq = m.seq;
    dash = strstr(m.title, " - ");
    if (dash) {
        int len = (int)(dash - m.title);
        snprintf(out_metadata->artist, sizeof(out_metadata->artist), "%.*s", len, m.title);
        snprintf(out_metadata->title, sizeof(out_metadata->title), "%.767s", dash + 3);
    } else {
        snprintf(out_metadata->title, sizeof(out_metadata->title), "%.767s", m.title);
    }
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_net_status(fa_stream* stream, fa_net_status* out_status)
{
    double bpf, secs = 0.0;
    if (out_status) memset(out_status, 0, sizeof(*out_status));
    if (!stream || !out_status || !stream->net) return FA_ERROR_INVALID_ARG;
    fa_net_get_status(stream->net, out_status);
    bpf = fa_net_bytes_per_frame(stream->net);
    if (bpf > 0 && stream->decoder.format.sample_rate)
        secs = (double)out_status->buffered_bytes / bpf / (double)stream->decoder.format.sample_rate;
    if (stream->dev_rate && stream->ring.data && !fa_load32(&stream->flushing))
        secs += (double)fa_ring_readable(&stream->ring) / (double)stream->dev_rate;
    out_status->buffered = (fa_time)(secs * 1e9);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_http_header(fa_stream* stream, const char* name, char* out_value, size_t value_size)
{
    if (out_value && value_size) out_value[0] = 0;
    if (!stream || !name || !out_value || value_size == 0) return FA_ERROR_INVALID_ARG;
    if (!stream->net) return FA_ERROR_NOT_FOUND;
    return fa_net_get_header(stream->net, name, out_value, value_size);
}

/* ---- tempo, pitch and rate: the decode side (the chain is in tempo.c) ----------- */

static int tempo_slot(fa_param param) { return (int)param - (int)FA_PARAM_RATE; }

static void tempo_ramp_done(struct fa_stream* s, int64_t tag)
{
    fa_event ev;
    ev.type = FA_EVENT_RAMP_DONE;
    ev.stream = s;
    ev.frame = fa_stream_position_now(s);
    ev.value = tag;
    fa_engine_post_event(s->engine, &ev);
}

static double linear_position(struct fa_stream* s, int64_t played);
static fa_frames wrap_position(struct fa_stream* s, fa_frames pos);

/* Raises RAMP_DONE for the waiting ramp ends playback reached (`played` or, with
 * `all`, every one): from the audio thread (dev set, the event's frame the
 * position heard at `played`) or from a detach or a full table (dev NULL). */
static void heard_ramps_due(struct fa_stream* s, fa_device* dev, int64_t played, int all)
{
    int i;
    if (fa_load32(&s->heard_waiting) == 0) return;   /* the audio thread's usual case */
    for (i = 0; i < FA_HEARD_RAMPS; i++) {
        int64_t tag;
        if (fa_load32(&s->heard_ramps[i].state) != 2 || (!all && s->heard_ramps[i].at > played)) continue;
        if (!fa_cas32(&s->heard_ramps[i].state, 2, 3)) continue;
        tag = s->heard_ramps[i].tag;
        fa_store32(&s->heard_ramps[i].state, 0);
        fa_fetch_add32(&s->heard_waiting, -1);
        if (dev) fa_audio_post_event(dev, s, FA_EVENT_RAMP_DONE, wrap_position(s, (fa_frames)(linear_position(s, played) + 0.5)), tag);
        else tempo_ramp_done(s, tag);
    }
}

/* A ramp that ended at chain frame `at` in audio decoded ahead: its RAMP_DONE
 * waits until that frame is heard (heard_ramps_due). Played frames count device
 * frames from the same start as chain frames (timeline_reset), so the chain frame
 * converts by the rates. Caller holds decode_lock (the only writer). */
static void ramp_done_when_heard(struct fa_stream* s, int64_t at, int64_t tag)
{
    int i;
    if (!s->dev_rate) {   /* decode-only: heard as it is read */
        tempo_ramp_done(s, tag);
        return;
    }
    for (i = 0; i < FA_HEARD_RAMPS; i++) {
        if (!fa_cas32(&s->heard_ramps[i].state, 0, 1)) continue;
        s->heard_ramps[i].at = (int64_t)floor((double)at * (double)s->dev_rate / (double)s->decoder.format.sample_rate + 0.5);
        s->heard_ramps[i].tag = tag;
        fa_fetch_add32(&s->heard_waiting, 1);
        fa_store32(&s->heard_ramps[i].state, 2);
        return;
    }
    /* The table is full (many short ramps within the decode-ahead): the waiting
     * ones and this one are raised now, early but in order. */
    heard_ramps_due(s, NULL, 0, 1);
    tempo_ramp_done(s, tag);
}

/* The played-frame count restarts from 0 at `played` (timeline_reset): waiting
 * ramp ends move with it, and those already reached are raised. Caller holds
 * decode_lock with the audio thread kept out. */
static void heard_ramps_rebase(struct fa_stream* s, int64_t played)
{
    int i;
    for (i = 0; i < FA_HEARD_RAMPS; i++)
        if (fa_load32(&s->heard_ramps[i].state) == 2) s->heard_ramps[i].at -= played;
    heard_ramps_due(s, NULL, 0, 0);
}

/* Does anything need the chain: a value off neutral, or a ramp? Caller holds tempo_lock. */
static int tempo_wanted_locked(struct fa_stream* s)
{
    int i;
    for (i = 0; i < FA_TEMPO_PARAMS; i++)
        if (s->tparams[i].active || s->tparams[i].value != (FA_PARAM_RATE + i == FA_PARAM_PITCH ? 0.0f : 1.0f))
            return 1;
    return 0;
}

static int tempo_wanted(struct fa_stream* s)
{
    int w;
    fa_mutex_lock(&s->tempo_lock);
    w = tempo_wanted_locked(s);
    fa_mutex_unlock(&s->tempo_lock);
    return w;
}

/* The values at chain frame `at` (raising RAMP_DONE for ramps that finished).
 * Returns 1 when the chain is wanted. Takes tempo_lock. */
static int tempo_values(struct fa_stream* s, int64_t at, double* tempo, double* pitch, double* rate)
{
    float v[FA_TEMPO_PARAMS];
    int64_t tags[FA_TEMPO_PARAMS], ends[FA_TEMPO_PARAMS];
    int done[FA_TEMPO_PARAMS], i, wanted;
    fa_mutex_lock(&s->tempo_lock);
    for (i = 0; i < FA_TEMPO_PARAMS; i++) {
        tags[i] = 0;
        done[i] = fa_tparam_eval(&s->tparams[i], at, &v[i], &tags[i], &ends[i]);
    }
    wanted = tempo_wanted_locked(s);
    fa_mutex_unlock(&s->tempo_lock);
    for (i = 0; i < FA_TEMPO_PARAMS; i++)
        if (done[i]) ramp_done_when_heard(s, ends[i], tags[i]);
    *rate = v[tempo_slot(FA_PARAM_RATE)];
    *tempo = v[tempo_slot(FA_PARAM_TEMPO)];
    *pitch = v[tempo_slot(FA_PARAM_PITCH)];
    return wanted;
}

/* Makes the history of a forward-only stream `more` frames longer, keeping what it
 * holds where hist_write expects it. Returns 0 if out of memory (nothing changes).
 * Caller holds decode_lock. */
static int hist_grow(struct fa_stream* s, fa_frames more)
{
    uint32_t ch = s->decoder.format.channels;
    fa_frames old = s->hist_cap, cap = old + more, i;
    float* h = (float*)malloc((size_t)cap * ch * sizeof(float));
    if (!h) return 0;
    for (i = s->decoded > old ? s->decoded - old : 0; i < s->decoded;) {
        fa_frames a = i % old, b = i % cap, n = s->decoded - i;
        if (old - a < n) n = old - a;
        if (cap - b < n) n = cap - b;
        memcpy(h + (size_t)b * ch, s->hist + (size_t)a * ch, (size_t)n * ch * sizeof(float));
        i += n;
    }
    free(s->hist);
    s->hist = h;
    s->hist_cap = cap;
    return 1;
}

/* The chain, created on first use with the input starting at source position
 * `src`. NULL when it cannot be made (out of memory, too many channels): the
 * audio then plays unprocessed. Caller holds decode_lock. */
static fa_tempo* tempo_chain(struct fa_stream* s, uint32_t channels, double src)
{
    fa_tempo* t;
    int fast, formants, transients;
    uint32_t posts;
    if (s->tempo) return s->tempo;
    fa_mutex_lock(&s->tempo_lock);
    posts = s->tempo_posts;
    fast = s->tempo_fast;
    formants = s->tempo_formants;
    transients = s->tempo_transients;
    fa_mutex_unlock(&s->tempo_lock);
    /* Out of memory last time: unprocessed until the next tempo, pitch or rate command,
     * rather than trying again on every chunk. */
    if (s->chain_failed && posts == s->chain_failed_posts) return NULL;
    /* A forward-only stream's history also covers what the chain holds (once). */
    if (s->hist && !s->hist_chain) {
        if (!hist_grow(s, s->decoder.format.sample_rate / 2)) goto fail;
        s->hist_chain = 1;
    }
    if (!tmap_of(s)) {
        /* Readers take the map when it appears: empty, as with no map. */
        fa_tmap* m = (fa_tmap*)calloc(1, sizeof(*m));
        if (!m) goto fail;
        fa_exchange_ptr(&s->tmap, m);
    }
    t = (fa_tempo*)calloc(1, sizeof(*t));
    if (!t) goto fail;
    if (fa_tempo_init(t, channels, s->decoder.format.sample_rate) != FA_OK) {
        free(t);
        goto fail;
    }
    fa_tempo_configure(t, fast, formants, transients);
    fa_tempo_reset(t, src);
    s->tempo = t;
    s->chain_failed = 0;
    return t;
fail:
    s->chain_failed = 1;
    s->chain_failed_posts = posts;
    return NULL;
}

/* Restarts the map from played frames to source positions at `pos`, with the
 * ring and the chain empty. Caller holds decode_lock (and keeps the audio thread out). */
static void timeline_reset(struct fa_stream* s, fa_frames pos)
{
    /* Ramps count chain frames from here on, starting at what is heard now. */
    int64_t heard = s->tempo_frames;
    int i;
    heard_ramps_rebase(s, fa_load64(&s->played));
    if (is_attached(s) && s->dev_rate)
        heard = (int64_t)((double)fa_load64(&s->played) * (double)s->decoder.format.sample_rate / (double)s->dev_rate + 0.5);
    fa_mutex_lock(&s->tempo_lock);
    for (i = 0; i < FA_TEMPO_PARAMS; i++) fa_tparam_rebase(&s->tparams[i], heard);
    fa_mutex_unlock(&s->tempo_lock);
    fa_store64(&s->played, 0);
    fa_store64(&s->seek_base, pos);
    fa_store32(&s->reverse, fa_load32(&s->reverse_want));
    s->rev_n = 0;
    s->rev_pos = pos;
    fa_tmap_reset(tmap_of(s));
    s->tempo_frames = 0;
    s->tempo_drained = 0;
    s->jumped_back = 0;
    fa_store32(&s->eq_restart, 1);
    s->chain_buf = NULL;
    s->rd_count = s->rd_off = 0;
    if (s->tempo) {
        int fast, formants, transients;
        fa_mutex_lock(&s->tempo_lock);
        fast = s->tempo_fast;
        formants = s->tempo_formants;
        transients = s->tempo_transients;
        fa_mutex_unlock(&s->tempo_lock);
        fa_tempo_reset(s->tempo, (double)pos);
        fa_tempo_configure(s->tempo, fast, formants, transients);
    }
    fa_store32(&s->tempo_live, tempo_wanted(s));
}

/* Notes the source positions of chain output about to enter the converter. */
static void tempo_note(struct fa_stream* s, int64_t frames, double src0, double src1)
{
    double sr = (double)s->decoder.format.sample_rate, dr = (double)s->dev_rate;
    if (frames > 0 && fa_tempo_engaged(s->tempo))
        fa_tmap_add(tmap_of(s), (int64_t)floor((double)s->tempo_frames * dr / sr + 0.5), src0,
                    (src1 - src0) / (double)frames * sr / dr);
    s->tempo_frames += frames;
}

/* Attached streams: runs a chunk (`*buf`, `*n` frames of `channels`) through the
 * chain; afterwards they are what the converter gets. Caller holds decode_lock. */
static void tempo_chunk(struct fa_stream* s, const float** buf, fa_frames* n, uint32_t channels)
{
    double tempo, pitch, rate, src0, src1;
    const float* out;
    int64_t got;
    int wanted = tempo_values(s, s->tempo_frames, &tempo, &pitch, &rate);
    if ((!s->tempo && !wanted) ||
        !tempo_chain(s, channels, (double)(fa_load64(&s->seek_base) + s->decoded - *n)) ||
        fa_tempo_run(s->tempo, *buf, *n, tempo, pitch, rate, &out, &got, &src0, &src1) != FA_OK) {
        s->tempo_frames += *n;   /* plays unprocessed */
        return;
    }
    tempo_note(s, got, src0, src1);
    fa_store32(&s->tempo_live, fa_tempo_engaged(s->tempo) || wanted);
    *buf = out;
    *n = got;
}

/* TEMPO x RATE as set now (the target of a ramp in progress is not reached yet). */
static double current_speed(struct fa_stream* s)
{
    double speed;
    fa_mutex_lock(&s->tempo_lock);
    speed = (double)s->tparams[tempo_slot(FA_PARAM_TEMPO)].value * (double)s->tparams[tempo_slot(FA_PARAM_RATE)].value;
    fa_mutex_unlock(&s->tempo_lock);
    return speed > 0.0 ? speed : 1.0;
}

/* TEMPO x RATE in effect at chain frame `at` (mid-ramp included), or 1 while the
 * chain plays the audio unchanged (neither `engaged` nor wanted). */
static double speed_now(struct fa_stream* s, int64_t at, int engaged)
{
    double tempo, pitch, rate;
    int wanted = tempo_values(s, at, &tempo, &pitch, &rate);
    return (engaged || wanted) && tempo * rate > 0.0 ? tempo * rate : 1.0;
}

/* Source frames per chain run while it is engaged: about FA_TEMPO_CHUNK frames of
 * output, so ramps step evenly in time and the position map (one segment per run
 * while values change) covers the decode-ahead buffer even at high speeds. */
static fa_frames tempo_step(struct fa_stream* s)
{
    double speed = current_speed(s);
    return speed > 1.0 ? (fa_frames)(FA_TEMPO_CHUNK * speed) : FA_TEMPO_CHUNK;
}

/* How full the decode-ahead buffer is kept: shorter while the chain is in use, so
 * tempo and pitch changes are heard soon. */
static int64_t fill_target(struct fa_stream* s)
{
    int64_t cap = s->ring.capacity;
    if (fa_load32(&s->tempo_live) && s->dev_rate) {
        int64_t t = (int64_t)s->dev_rate * s->tempo_latency_ms / 1000;
        if (t < cap) cap = t;
    }
    return cap;
}

/* ---- decode-ahead (worker threads) -------------------------------------- */

/* A push stream with nothing pushed: no decoding until a push (which wakes the workers). */
static int push_waiting(struct fa_stream* s)
{
    return s->push && !fa_push_has_input(s->push) && !fa_load32(&s->staged);
}

int fa_stream_needs_work(struct fa_stream* s)
{
    if (s->ring.data && fa_load64(&s->seek_pending) >= 0) return 1;
    if (!s->ring.data || fa_load32(&s->eof) || fa_load32(&s->flushing)) return 0;
    if (push_waiting(s)) return 0;   /* nothing pushed yet */
    if (fa_ring_readable(&s->ring) > fill_target(s) * 3 / 4) return 0;
    if (fa_tmap_full(tmap_of(s))) return 0;   /* waits for playback to catch up */
    /* Web streams: only once the bytes are downloaded, so a worker never waits. */
    return !s->net || fa_net_can_decode(s->net, NULL);
}

/* How long the polling worker may sleep before this stream could want it
 * (worker_main): a quarter of what its ring still holds, so it is looked at several
 * times before it could run dry. 0: nothing to decode until an API call (which
 * wakes the workers): no ring, flushing, ended, or a push stream with nothing pushed. */
uint32_t fa_stream_poll_ms(struct fa_stream* s)
{
    int64_t ms;
    if (!s->ring.data || !s->dev_rate) return 0;
    if (fa_load64(&s->seek_pending) >= 0) return FA_WORKER_POLL_MIN_MS;
    /* Flushing ends with unflush, which wakes them (a failed rebuild leaves it set). */
    if (fa_load32(&s->flushing) || fa_load32(&s->eof) || push_waiting(s)) return 0;
    ms = (int64_t)fa_ring_readable(&s->ring) * 1000 / s->dev_rate / 4;
    if (ms < FA_WORKER_POLL_MIN_MS) return FA_WORKER_POLL_MIN_MS;
    return ms > FA_WORKER_POLL_MAX_MS ? FA_WORKER_POLL_MAX_MS : (uint32_t)ms;
}

/* Posts FA_EVENT_METADATA for the title in effect at `heard` if nobody has yet. */
static void announce_title(struct fa_stream* s, int64_t heard)
{
    int64_t seq = fa_net_meta_seq_at(s->net, heard), was = fa_load64(&s->meta_announced);
    if (seq > was && fa_cas64(&s->meta_announced, was, seq)) {
        fa_event ev;
        ev.type = FA_EVENT_METADATA;
        ev.stream = s;
        ev.frame = heard;
        ev.value = seq;
        fa_engine_post_event(s->engine, &ev);
    }
}

/* Web streams, after `got` frames were decoded, ending at source frame `cursor`:
 * places new ICY titles on the timeline and arms the next one for the audio thread. */
static void net_decoded(struct fa_stream* s, fa_frames got, fa_frames cursor, fa_frames heard)
{
    int64_t lag_bytes = 0, lag_frames = 0, seq = 0, next;
    fa_net_note_decoded(s->net, got);
    fa_decoder_lag(&s->decoder, &lag_bytes, &lag_frames);
    next = fa_net_meta_update(s->net, cursor + lag_frames, lag_bytes, heard, &seq);
    announce_title(s, heard);
    if (next != INT64_MAX) {
        fa_store64(&s->meta_seq_armed, seq);  /* seq first: the audio thread reads the frame first */
        fa_store64(&s->meta_frame, next);
    }
}

/* Feeds the running loudness estimate until the final value is known, once
 * anything wants it (normalization on, or the app asked for the loudness): with
 * normalization off, the default, the meter would cost about 7% of decoding for
 * nothing. */
static void update_estimate(struct fa_stream* s, const float* frames, fa_frames count)
{
    int64_t before;
    float track = fa_load_float(&s->norm_track);
    if (!s->run_meter_ready || track == track) return;
    if (!fa_load32(&s->est_wanted) && fa_load32(&s->engine->norm_mode) == FA_NORMALIZE_OFF) return;
    before = s->run_meter.nsubs;
    fa_meter_process(&s->run_meter, frames, count);
    if (s->run_meter.nsubs != before) {
        double est = fa_meter_running_lufs(&s->run_meter);
        if (est == est) fa_store_float(&s->norm_est, (float)est);
    }
}

/* The deck loop (fa_stream_set_loop_region) in force: out 0 = none. */
static const fa_frames* region_of(struct fa_stream* s) { return s->region[fa_load32(&s->region_idx) & 1]; }

/* Sets the deck loop; out 0 = none. Caller holds decode_lock (and api_lock, or is the
 * worker carrying out a restart). */
static void region_store(struct fa_stream* s, fa_frames in, fa_frames out)
{
    uint32_t next = (uint32_t)fa_load32(&s->region_idx) + 1;
    s->region[next & 1][0] = out > 0 ? in : 0;
    s->region[next & 1][1] = out > 0 ? out : 0;
    fa_store32(&s->region_idx, (int32_t)next);
}

/* Only a seekable source loops (looping seeks back); a forward-only one ends. */
static int loops(struct fa_stream* s)
{
    return (fa_load32(&s->loop) || region_of(s)[1] > 0) && s->decoder.seekable;
}

/* Where the song ends: CUE_OUT, else the length; -1 = unknown. */
static fa_frames song_end(struct fa_stream* s)
{
    fa_frames out = marker(s, FA_MARKER_CUE_OUT);
    if (out != FA_FRAMES_UNSET && (s->decoder.length < 0 || out < s->decoder.length)) return out;
    return s->decoder.length;
}

/* Where playback ends or loops back from (a deck loop's out point, else the song's
 * end), and where it loops back to. */
static fa_frames end_frame(struct fa_stream* s)
{
    const fa_frames* r = region_of(s);
    return r[1] > 0 ? r[1] : song_end(s);
}

static fa_frames loop_start(struct fa_stream* s)
{
    const fa_frames* r = region_of(s);
    fa_frames in, end;
    if (r[1] > 0) return r[0];
    in = marker(s, FA_MARKER_CUE_IN);
    end = end_frame(s);
    return (in != FA_FRAMES_UNSET && (end < 0 || in < end)) ? in : 0;
}

/* ---- reverse ------------------------------------------------------------------ */

static int reversed(struct fa_stream* s) { return fa_load32(&s->reverse); }

/* A linear frame of the timeline as a source position, and back: the same forward,
 * mirrored about seek_base reversed (see `reverse`). */
static double mirror(struct fa_stream* s, double x)
{
    return reversed(s) ? 2.0 * (double)fa_load64(&s->seek_base) - x : x;
}

/* Reversed: hands out up to `want` source frames going back from rev_pos, last
 * first. Each block of FA_REVERSE_BLOCK_MS is decoded forward after a seek to its
 * start. The source ends (got 0) at the loop start, else CUE_IN or 0 (as forward
 * it ends at the loop end, else CUE_OUT or the length). Caller holds decode_lock. */
static fa_result reverse_read(struct fa_stream* s, float* out, fa_frames want, fa_frames* got)
{
    uint32_t ch = s->decoder.format.channels;
    fa_frames k, i, stop = loop_start(s);
    *got = 0;
    /* Checked on every call: CUE_IN may have moved up into the block decoded already. */
    if (s->rev_pos <= stop) {
        s->rev_n = 0;
        return FA_ERROR_END_OF_STREAM;
    }
    if (s->rev_n == 0) {
        fa_frames lo, n = 0;
        fa_result r;
        if (!s->rev_buf) {
            fa_frames cap = (fa_frames)s->decoder.format.sample_rate * FA_REVERSE_BLOCK_MS / 1000;
            s->rev_buf = (float*)malloc((size_t)cap * ch * sizeof(float));
            if (!s->rev_buf) return FA_ERROR_OUT_OF_MEMORY;
            s->rev_cap = cap;
        }
        lo = s->rev_pos - s->rev_cap > stop ? s->rev_pos - s->rev_cap : stop;
        r = fa_decoder_seek(&s->decoder, lo);
        if (r != FA_OK) return r;
        while (n < s->rev_pos - lo) {
            fa_frames m = 0;
            r = fa_decoder_read(&s->decoder, s->rev_buf + (size_t)n * ch, s->rev_pos - lo - n, &m);
            if (m <= 0) break;
            n += m;
        }
        if (n == 0) return r == FA_OK ? FA_ERROR_END_OF_STREAM : r;
        s->rev_n = n;
        s->rev_pos = lo + n;   /* a source shorter than it said: from where it ends */
    }
    k = want < s->rev_n ? want : s->rev_n;
    if (k > s->rev_pos - stop) k = s->rev_pos - stop;
    for (i = 0; i < k; i++)
        memcpy(out + (size_t)i * ch, s->rev_buf + (size_t)(s->rev_n - 1 - i) * ch, ch * sizeof(float));
    s->rev_n -= k;
    s->rev_pos -= k;
    *got = k;
    return FA_OK;
}

/* The source's next frames in the timeline's direction. Caller holds decode_lock. */
static fa_result source_read(struct fa_stream* s, float* out, fa_frames want, fa_frames* got)
{
    if (reversed(s)) return reverse_read(s, out, want, got);
    return fa_decoder_read(&s->decoder, out, want, got);
}

/* The custom channel matrix: in_buf (source channels) -> mat_buf (device channels). */
static void apply_matrix(struct fa_stream* s, const float* in, fa_frames n)
{
    uint32_t ch = s->decoder.format.channels, i, o;
    fa_frames f;
    for (f = 0; f < n; f++) {
        for (o = 0; o < FA_DEVICE_CHANNELS; o++) {
            float acc = 0.0f;
            for (i = 0; i < ch; i++) acc += in[(size_t)f * ch + i] * s->matrix[i * FA_DEVICE_CHANNELS + o];
            s->mat_buf[(size_t)f * FA_DEVICE_CHANNELS + o] = acc;
        }
    }
}

/* Appends source frames to the history of a forward-only stream (see `hist`). */
static void hist_write(struct fa_stream* s, const float* in, fa_frames n)
{
    uint32_t ch = s->decoder.format.channels;
    fa_frames at, first;
    if (n > s->hist_cap) { in += (size_t)(n - s->hist_cap) * ch; n = s->hist_cap; }
    at = s->decoded % s->hist_cap;
    first = s->hist_cap - at < n ? s->hist_cap - at : n;
    memcpy(s->hist + (size_t)at * ch, in, (size_t)first * ch * sizeof(float));
    memcpy(s->hist, in + (size_t)first * ch, (size_t)(n - first) * ch * sizeof(float));
}

/* Detaching a forward-only stream: moves the `unheard` most recent source frames from
 * the history to the front of `carry`, so they play on the next attach. Caller holds
 * decode_lock. Returns 0 if it could not (out of memory), and nothing changes. */
static int keep_unheard(struct fa_stream* s, fa_frames unheard)
{
    uint32_t ch = s->decoder.format.channels;
    fa_frames left = s->carry_count, total, start, first;
    float* buf;
    if (unheard > s->decoded) unheard = s->decoded;
    if (unheard > s->hist_cap) unheard = s->hist_cap;
    if (unheard <= 0) return 1;
    total = unheard + left;
    buf = (float*)malloc((size_t)total * ch * sizeof(float));
    if (!buf) return 0;
    start = (s->decoded - unheard) % s->hist_cap;
    first = s->hist_cap - start < unheard ? s->hist_cap - start : unheard;
    memcpy(buf, s->hist + (size_t)start * ch, (size_t)first * ch * sizeof(float));
    memcpy(buf + (size_t)first * ch, s->hist, (size_t)(unheard - first) * ch * sizeof(float));
    if (left > 0) memcpy(buf + (size_t)unheard * ch, s->carry + (size_t)s->carry_offset * ch, (size_t)left * ch * sizeof(float));
    free(s->carry);
    s->carry = buf;
    s->carry_count = total;
    s->carry_offset = 0;
    return 1;
}

/* Decodes, converts to the device format and appends to the ring until it is
 * full, `max_frames` were produced, or the source ended (at CUE_OUT, if set).
 * Caller holds decode_lock. */
void fa_stream_fill(struct fa_stream* s, int64_t max_frames)
{
    uint32_t ch = s->decoder.format.channels;
    uint32_t conv_ch = s->matrix ? FA_DEVICE_CHANNELS : ch;
    int64_t produced = 0;
    int just_looped = 0;
    if (!s->ring.data || fa_load32(&s->eof)) return;

    while (produced < max_frames) {
        ma_uint64 in_n, out_n;
        const float* conv_in;
        float* dst;
        int64_t contig, room = fa_ring_writable(&s->ring), target = fill_target(s) - fa_ring_readable(&s->ring);
        if (target < room) room = target;
        if (room <= 0) break;
        if (s->in_count == 0) {
            fa_frames got = 0, want = s->in_cap, end = end_frame(s);
            const float* chunk;
            if (s->tempo && fa_tempo_engaged(s->tempo)) {
                if (fa_tmap_full(tmap_of(s))) break;
                fa_frames step = tempo_step(s);
                if (want > step) want = step;   /* ramps step finely */
            }
            fa_result rr = FA_OK;
            int from_carry;
            if (s->draining) {                    /* the resampler has given back everything */
                fa_store32(&s->eof, 1);
                break;
            }
            if (end >= 0 && !reversed(s) && s->src_pos + want > end) want = end > s->src_pos ? end - s->src_pos : 0;
            from_carry = s->carry_count > 0;
            if (from_carry && want > 0) {         /* audio decoded ahead before a detach */
                got = s->carry_count < want ? s->carry_count : want;
                memcpy(s->in_buf, s->carry + (size_t)s->carry_offset * ch, (size_t)got * ch * sizeof(float));
                s->carry_offset += got;
                s->carry_count -= got;
            } else if (want > 0) {
                if (s->net && !fa_net_can_decode(s->net, NULL)) break;  /* not downloaded yet */
                rr = source_read(s, s->in_buf, want, &got);
            }
            if (got == 0 && rr == FA_ERROR_BUSY) break;   /* push stream: nothing queued yet */
            if (got > 0 && !from_carry && s->push && s->decoder.anchor_pts != FA_PTS_NONE) {
                /* The app's timeline restarts at this chunk: note at which ring frame
                 * (what the resampler still holds comes out first), and at which
                 * source frame for a later attach. */
                int64_t at = fa_load64(&s->ring.write), lat = (int64_t)ma_data_converter_get_output_latency(&s->converter);
                /* What the chain holds comes out at the speed now in use. */
                if (s->tempo) {
                    double speed = speed_now(s, s->tempo_frames, fa_tempo_engaged(s->tempo));
                    lat += (int64_t)(fa_tempo_held(s->tempo) / speed * (double)s->dev_rate / (double)s->decoder.format.sample_rate + 0.5);
                }
                /* The ring's very first frames are the resampler's start-up, ahead of
                 * this chunk: they start the timeline too (at its pts, never at 0 or
                 * before the clock's hold). */
                fa_ptsmap_add(ptsmap_of(s), at == 0 && fa_load64(&ptsmap_of(s)->write) == 0 ? 0 : at + lat, s->decoder.anchor_pts);
                ptimes_of(s)->anchor_pos[s->anchors % FA_SRC_ANCHORS] = s->src_pos;
                ptimes_of(s)->anchor_pts[s->anchors % FA_SRC_ANCHORS] = s->decoder.anchor_pts;
                s->anchors++;
            }
            if (s->hist) hist_write(s, s->in_buf, got);
            s->in_offset = 0;
            s->in_count = got;
            s->decoded += got;
            s->src_pos = reversed(s) ? s->rev_pos : s->src_pos + got;
            if (s->net && !from_carry && got > 0) net_decoded(s, got, s->src_pos, fa_stream_position_now(s));
            if (got == 0) {
                fa_frames back = reversed(s) ? end_frame(s) : loop_start(s);
                ma_uint64 latency;
                /* Reversed, the loop goes from its start back to its end. */
                if (loops(s) && !just_looped && (reversed(s) ? back > loop_start(s) && (s->rev_pos = back, s->rev_n = 0, 1)
                                                             : fa_decoder_seek(&s->decoder, back) == FA_OK)) {
                    just_looped = 1; /* a second empty read right after looping = empty source */
                    s->jumped_back = 1;
                    s->src_pos = back;
                    continue;        /* the converter keeps its state: the loop joins seamlessly */
                }
                /* The chain still holds its last frames: they come out first. */
                if (s->tempo && fa_tempo_engaged(s->tempo) && !s->tempo_drained) {
                    double tempo, pitch, rate, src0, src1;
                    const float* tail;
                    int64_t n = 0;
                    s->tempo_drained = 1;
                    tempo_values(s, s->tempo_frames, &tempo, &pitch, &rate);
                    if (fa_tempo_drain(s->tempo, tempo, pitch, rate, &tail, &n, &src0, &src1) == FA_OK && n > 0) {
                        tempo_note(s, n, src0, src1);
                        s->chain_buf = tail;
                        s->in_offset = 0;
                        s->in_count = n;
                        continue;
                    }
                }
                s->chain_buf = NULL;
                /* The resampler still holds the last few frames: push them out with
                 * silence (its input latency) before calling it the end. */
                latency = ma_data_converter_get_input_latency(&s->converter);
                if (latency > 0) {
                    if (latency > (ma_uint64)s->in_cap) latency = (ma_uint64)s->in_cap;
                    memset(s->in_buf, 0, (size_t)latency * ch * sizeof(float));
                    if (s->matrix) memset(s->mat_buf, 0, (size_t)latency * conv_ch * sizeof(float));
                    s->in_count = (fa_frames)latency;
                    s->draining = 1;
                    continue;
                }
                fa_store32(&s->eof, 1);
                break;
            }
            just_looped = 0;
            if (!from_carry) update_estimate(s, s->in_buf, got); /* carried audio was measured */
            if (s->matrix) apply_matrix(s, s->in_buf, got);
            chunk = s->matrix ? s->mat_buf : s->in_buf;
            tempo_chunk(s, &chunk, &got, conv_ch);
            s->chain_buf = chunk != (s->matrix ? s->mat_buf : s->in_buf) ? chunk : NULL;
            s->in_count = got;
            if (got == 0) continue;               /* the stretcher is gathering its start */
        }
        /* Converted straight into the ring, up to its wrap; published once the DSPs ran. */
        dst = fa_ring_write_region(&s->ring, &contig);
        if (contig < room) room = contig;
        conv_in = s->chain_buf ? s->chain_buf : s->matrix ? s->mat_buf : s->in_buf;
        in_n = (ma_uint64)s->in_count;
        out_n = (ma_uint64)(room < FA_CONVERT_CHUNK ? room : FA_CONVERT_CHUNK);
        ma_data_converter_process_pcm_frames(&s->converter, conv_in + (size_t)s->in_offset * conv_ch, &in_n,
                                             dst, &out_n);
        if (out_n > 0) {
            run_dsps(s, dst, (fa_frames)out_n, FA_DEVICE_CHANNELS, s->dev_rate);
            fa_ring_commit(&s->ring, (int64_t)out_n);
        }
        s->in_offset += (fa_frames)in_n;
        s->in_count -= (fa_frames)in_n;
        produced += (int64_t)out_n;
        if (in_n == 0 && out_n == 0) break; /* converter stalled; try again later */
    }
    /* Frames read but not in the ring yet still need a worker, even with nothing
     * more pushed (fa_stream_needs_work runs without decode_lock). */
    fa_store32(&s->staged, s->in_count > 0 || s->carry_count > 0);
}

/* Source position (not wrapped) after `played` device frames since seek_base. */
static double linear_position(struct fa_stream* s, int64_t played)
{
    return mirror(s, fa_tmap_pos(tmap_of(s), played, (double)fa_load64(&s->seek_base),
                                 (double)s->decoder.format.sample_rate / (double)s->dev_rate, NULL, NULL));
}

/* Device frames from `played` until the linear source position reaches `target`. */
static int64_t frames_until(struct fa_stream* s, int64_t played, double target)
{
    return fa_tmap_frames_to(tmap_of(s), played, mirror(s, target), (double)fa_load64(&s->seek_base),
                             (double)s->decoder.format.sample_rate / (double)s->dev_rate);
}

/* Wraps a linear position into the loop (CUE_IN..CUE_OUT) of looping streams, and
 * clamps it to the end of other streams. Reversed, the loop wraps at its start and
 * other streams stop at theirs. */
static fa_frames wrap_position(struct fa_stream* s, fa_frames pos)
{
    fa_frames end = end_frame(s);
    if (reversed(s)) {
        fa_frames back = loop_start(s), len = end - back;
        if (loops(s) && len > 0 && pos < back) return end - 1 - (back - 1 - pos) % len;
        return pos < back ? back : pos;
    }
    if (loops(s) && end > 0 && pos >= end) {
        fa_frames back = loop_start(s), len = end - back;
        return len > 0 ? back + (pos - end) % len : back;
    }
    if (end >= 0 && pos > end) pos = end;
    return pos;
}

/* Source position from the frames the device has consumed. Exact for a fixed
 * rate ratio; the resampler's few frames of delay are not subtracted. */
static fa_frames attached_position(struct fa_stream* s)
{
    return wrap_position(s, (fa_frames)(linear_position(s, fa_load64(&s->played)) + 0.5));
}

fa_frames fa_stream_position_now(struct fa_stream* s)
{
    return s->dev_rate ? attached_position(s) : fa_load64(&s->position);
}

/* Where the stream is going: a pending fa_stream_seek_async target, else the
 * position heard. What restarts and the public getters use. */
static fa_frames target_position(struct fa_stream* s)
{
    int64_t pending = fa_load64(&s->seek_pending);
    return pending >= 0 ? pending : fa_stream_position_now(s);
}

/* After a restart: the mixer may play again, unless a newer async target came
 * in meanwhile (a worker carries that out next). The workers are woken: the ring
 * holds only the prefill now, and the polling one may be sleeping for longer (it
 * skips ended streams, see fa_stream_poll_ms). */
static void unflush(struct fa_stream* s)
{
    fa_store32(&s->flushing, fa_load64(&s->seek_pending) >= 0);
    fa_engine_wake_workers(s->engine);
}

/* Drops buffered audio and restarts decoding at `frame`. Caller holds api_lock and
 * decode_lock, and the audio thread is kept out (flushing set, or not attached). */
static fa_result restart_expecting_locked(struct fa_stream* s, fa_frames frame, int64_t pending)
{
    fa_result r = FA_OK;
    int64_t hold = 0;
    if (s->push) {
        /* Push streams cannot seek: only the buffers start over (a rebuilt chain). */
        hold = fa_stream_clock_at(s, fa_monotonic_ns());
        frame = fa_stream_position_now(s);
        fa_push_restart_timeline(s->push);
    } else {
        /* After a reversed timeline the decoder is wherever its last block was. */
        if (frame != fa_stream_position_now(s) || s->in_count > 0 || fa_ring_readable(&s->ring) > 0 || fa_load32(&s->eof) ||
            reversed(s))
            r = fa_decoder_seek(&s->decoder, frame);
        if (r != FA_OK) return r;
        hold = (int64_t)((double)frame * 1e9 / (double)s->decoder.format.sample_rate + 0.5);
    }
    /* A seek past a deck loop's out point leaves the loop (reversed: before its in point). */
    if (region_of(s)[1] > 0 && (fa_load32(&s->reverse_want) ? frame < region_of(s)[0] : frame >= region_of(s)[1]))
        region_store(s, 0, 0);
    fa_store64(&s->position, frame);
    s->src_pos = frame;
    timeline_reset(s, frame);
    fa_ptsmap_reset(ptsmap_of(s));
    fa_clock_reset(&s->clock, hold);
    if (s->net) {
        fa_net_meta_reset_frames(s->net, frame);
        fa_store64(&s->meta_frame, INT64_MAX);
    }
    if (s->ring.data) {
        fa_ring_reset(&s->ring);
        s->in_count = s->in_offset = 0;
        s->decoded = 0;
        s->draining = 0;
        if (s->converter_ready) ma_data_converter_reset(&s->converter);
        fa_store32(&s->eof, 0);
        /* Prefill so playback resumes without waiting for a worker. */
        fa_stream_fill(s, (int64_t)s->dev_rate * FA_PREFILL_MS / 1000);
    }
    fa_cas64(&s->seek_pending, pending, -1);   /* done or replaced, unless a newer one came meanwhile */
    return FA_OK;
}

static fa_result restart_at_locked(struct fa_stream* s, fa_frames frame)
{
    return restart_expecting_locked(s, frame, fa_load64(&s->seek_pending));
}

/* ---- attach / detach ---------------------------------------------------- */

static fa_result build_chain_locked(struct fa_stream* s, fa_device* dev)
{
    ma_data_converter_config cc;
    uint32_t ch = s->decoder.format.channels;
    int64_t capacity = (int64_t)dev->rate * s->buffer_ms / 1000;
    if (capacity < 2048) capacity = 2048;

    release_chain(s);
    cc = ma_data_converter_config_init(ma_format_f32, ma_format_f32, s->matrix ? FA_DEVICE_CHANNELS : ch,
                                       FA_DEVICE_CHANNELS, s->decoder.format.sample_rate, dev->rate);
    cc.resampling.algorithm = ma_resample_algorithm_custom;   /* the sinc resampler (resample_device.c) */
    cc.resampling.pBackendVTable = &fa_sinc_resampler;
    if (ma_data_converter_init(&cc, NULL, &s->converter) != MA_SUCCESS) {
        /* Beyond its widest ratio: miniaudio's linear one. */
        cc.resampling.algorithm = ma_resample_algorithm_linear;
        cc.resampling.pBackendVTable = NULL;
        if (ma_data_converter_init(&cc, NULL, &s->converter) != MA_SUCCESS) return FA_ERROR_OUT_OF_MEMORY;
    }
    s->converter_ready = 1;
    s->in_cap = FA_DECODE_CHUNK;
    s->in_buf = (float*)malloc((size_t)s->in_cap * ch * sizeof(float));
    if (s->matrix) s->mat_buf = (float*)malloc((size_t)s->in_cap * FA_DEVICE_CHANNELS * sizeof(float));
    if (!s->decoder.seekable) {
        /* Everything not yet heard: the ring, the input chunk and the resampler's delay
         * (and what the tempo chain holds, added when the chain is made). */
        s->hist_cap = (fa_frames)((double)capacity * s->decoder.format.sample_rate / dev->rate) + 2 * s->in_cap + 256;
        s->hist = (float*)malloc((size_t)s->hist_cap * ch * sizeof(float));
    }
    if (!s->in_buf || (s->matrix && !s->mat_buf) || (!s->decoder.seekable && !s->hist) ||
        fa_ring_init(&s->ring, FA_DEVICE_CHANNELS, capacity) != FA_OK) {
        release_chain(s);
        return FA_ERROR_OUT_OF_MEMORY;
    }
    return FA_OK;
}

/* ---- queues: references between streams ----------------------------------- */

/* A queued stream that will not start after all goes back to stopped. */
static void unqueue(struct fa_stream* n)
{
    if (n && fa_load64(&n->start_frame) == FA_START_QUEUED)
        fa_cas32(&n->state, FA_STATE_SCHEDULED, FA_STATE_STOPPED);
}

/* Drops every queue link to and from `s` on its device. The audio thread may be
 * starting one right now; the caller waits for quiescence before freeing anything.
 * Caller holds s->api_lock; takes the device lock. */
static void drop_queue_links(struct fa_stream* s)
{
    fa_device* dev = s->device;
    fa_voice_list* list;
    uint32_t i;
    unqueue((struct fa_stream*)fa_exchange_ptr(&s->queued, NULL));
    if (!dev) return;
    fa_mutex_lock(&dev->lock);
    list = (fa_voice_list*)fa_load_ptr(&dev->voices);
    if (list)
        for (i = 0; i < list->count; i++) fa_cas_ptr(&list->items[i]->queued, s, NULL);
    fa_mutex_unlock(&dev->lock);
}

/* ---- attach / detach ---------------------------------------------------- */

/* Routes the stream to `bus` (NULL = the master mix); a mixer deck it leaves is empty then. */
static void set_bus(struct fa_stream* s, fa_bus* bus)
{
    fa_bus* old = (fa_bus*)fa_exchange_ptr(&s->bus, bus);
    if (old && old != bus && old->deck) fa_mixer_bus_left(old, s);
}

/* Caller holds api_lock. */
static void detach_locked(struct fa_stream* s)
{
    fa_device* dev = s->device;
    fa_frames pos;
    if (!dev) return;
    /* Lock-free clock readers stop using the device before it can go away; until
     * the exact position is stored below they read the time heard now. */
    if (!s->push)
        fa_store64(&s->position, (fa_frames)((double)fa_stream_clock_at(s, fa_monotonic_ns()) *
                                             (double)s->decoder.format.sample_rate / 1e9 + 0.5));
    fa_exchange_ptr(&s->clock_device, NULL);
    while (fa_load32(&s->clock_readers) != 0) fa_sleep_ms(0);
    drop_queue_links(s);
    fa_device_remove_voice(dev, s);          /* waits until the audio thread let go */
    fa_engine_track_stream(s->engine, s, 0);
    fa_mutex_lock(&s->decode_lock);          /* waits for a worker still filling it */
    heard_ramps_due(s, NULL, 0, 1);          /* the next attach starts at their end values */
    pos = target_position(s);                /* a pending fa_stream_seek_async included */
    fa_store64(&s->seek_pending, -1);
    s->dev_rate = 0;
    s->device = NULL;
    set_bus(s, NULL);
    /* Continue later from what was actually heard, not from what was decoded ahead.
     * A forward-only source cannot go back, so it keeps the audio decoded ahead and
     * plays it first on the next attach. */
    if (s->decoder.seekable) fa_decoder_seek(&s->decoder, pos);
    else if (!keep_unheard(s, fa_load64(&s->seek_base) + s->decoded - pos)) {
        pos = fa_load64(&s->seek_base) + s->decoded; /* out of memory: skip it as before */
    }
    fa_store64(&s->position, pos);
    s->src_pos = pos;
    release_chain(s);
    fa_store32(&s->eof, 0);
    fa_mutex_unlock(&s->decode_lock);
    fa_cas32(&s->state, FA_STATE_PLAYING, FA_STATE_PAUSED);
    fa_cas32(&s->state, FA_STATE_BUFFERING, FA_STATE_PAUSED);
    if (fa_load64(&s->start_frame) == FA_START_QUEUED) fa_cas32(&s->state, FA_STATE_SCHEDULED, FA_STATE_STOPPED);
    else fa_cas32(&s->state, FA_STATE_SCHEDULED, FA_STATE_PAUSED);
}

/* The pts of source frame `pos` of a push stream, from the timeline restarts noted
 * by fa_stream_fill; *ok = 0 when the restart before it is no longer known. */
static int64_t source_pts(struct fa_stream* s, int64_t pos, int* ok)
{
    int64_t k, lo = s->anchors > FA_SRC_ANCHORS ? s->anchors - FA_SRC_ANCHORS : 0;
    double sr = (double)s->decoder.format.sample_rate;
    for (k = s->anchors - 1; k >= lo; k--) {
        int64_t ap = ptimes_of(s)->anchor_pos[k % FA_SRC_ANCHORS];
        if (ap <= pos) {
            *ok = 1;
            return ptimes_of(s)->anchor_pts[k % FA_SRC_ANCHORS] + (int64_t)((double)(pos - ap) * 1e9 / sr + 0.5);
        }
    }
    *ok = 0;
    return 0;
}

/* The pts of the first frame kept from before a detach (carry), the next one heard. */
static int64_t carry_first_pts(struct fa_stream* s)
{
    int ok = 0;
    int64_t first = source_pts(s, s->src_pos, &ok);
    if (!ok) first = fa_push_next_pts(s->push) - (int64_t)((double)s->carry_count * 1e9 / (double)s->decoder.format.sample_rate + 0.5);
    return first;
}

/* Attaching a push stream: the audio kept from before a detach (carry) plays first,
 * with the timestamps it had, timeline jumps inside it included; the decoder's own
 * output follows. Returns the pts of the first frame. Caller holds decode_lock,
 * with the pts map just reset and the ring empty. */
static int64_t carry_timeline(struct fa_stream* s, uint32_t dev_rate)
{
    double ratio = (double)dev_rate / (double)s->decoder.format.sample_rate;
    int64_t pos = s->src_pos, end = pos + s->carry_count, k, lo, first;
    if (s->carry_count <= 0) return fa_push_next_pts(s->push);
    ratio /= speed_now(s, 0, 0);   /* the chain plays it at that speed */
    first = carry_first_pts(s);
    fa_ptsmap_add(ptsmap_of(s), 0, first);
    lo = s->anchors > FA_SRC_ANCHORS ? s->anchors - FA_SRC_ANCHORS : 0;
    for (k = lo; k < s->anchors; k++) {
        int64_t ap = ptimes_of(s)->anchor_pos[k % FA_SRC_ANCHORS];
        if (ap > pos && ap < end)
            fa_ptsmap_add(ptsmap_of(s), (int64_t)((double)(ap - pos) * ratio + 0.5), ptimes_of(s)->anchor_pts[k % FA_SRC_ANCHORS]);
    }
    return first;
}

FA_API fa_result FA_CALL fa_stream_attach(fa_stream* stream, fa_device* device, fa_bus* bus)
{
    if (!stream || !device) return FA_ERROR_INVALID_ARG;
    /* The argument checks of attach_internal first: a call refused for them leaves the
     * stream on its deck. */
    if ((bus && bus->device != device) || (stream->flags & FA_OPEN_DECODE_ONLY) || device->engine != stream->engine)
        return FA_ERROR_INVALID_ARG;
    fa_mixer_stream_leaving(stream, bus);   /* off a deck first, under the mixer lock */
    return fa_stream_attach_internal(stream, device, bus);
}

fa_result fa_stream_attach_internal(fa_stream* stream, fa_device* device, fa_bus* bus)
{
    fa_result r;
    int was_playing;
    if (!stream || !device) return FA_ERROR_INVALID_ARG;
    if (bus && bus->device != device) return FA_ERROR_INVALID_ARG;
    if (stream->flags & FA_OPEN_DECODE_ONLY) return FA_ERROR_INVALID_ARG;
    if (device->engine != stream->engine) return FA_ERROR_INVALID_ARG; /* events and cleanup are per engine */

    fa_mutex_lock(&stream->api_lock);
    if (stream->reader) {   /* fa_encoder_encode_stream reads it */
        fa_mutex_unlock(&stream->api_lock);
        return FA_ERROR_BUSY;
    }
    if (stream->device == device) {
        set_bus(stream, bus);  /* the mixer picks it up on its next block */
        fa_mutex_unlock(&stream->api_lock);
        return FA_OK;
    }
    was_playing = stream->device && (fa_load32(&stream->state) == FA_STATE_PLAYING ||
                                     fa_load32(&stream->state) == FA_STATE_BUFFERING);
    if (stream->device) detach_locked(stream);  /* pauses it */

    fa_mutex_lock(&stream->decode_lock);
    r = build_chain_locked(stream, device);
    if (r == FA_OK) {
        fa_frames pos = fa_load64(&stream->position);
        int p;
        /* Ramp counters are in device frames: keep a running ramp's remaining time
         * when moving to a device with another rate. Nothing mixes this stream now. */
        for (p = 0; p < FA_STREAM_LANES; p++) {
            fa_param_voice* v = &stream->lanes[p].v;
            if (v->active && stream->voice_rate && stream->voice_rate != device->rate) {
                double k = (double)device->rate / (double)stream->voice_rate;
                v->total = (int64_t)((double)v->total * k + 0.5);
                v->done = (int64_t)((double)v->done * k + 0.5);
            }
            v->at = -1;  /* a start time on the old device's clock means nothing here */
        }
        stream->voice_rate = device->rate;
        stream->dev_rate = device->rate;
        stream->src_pos = pos;
        timeline_reset(stream, pos);
        fa_store32(&stream->eof, 0);
        fa_store32(&stream->flushing, 0);
        fa_ptsmap_reset(ptsmap_of(stream));
        if (stream->push) {
            int64_t first = carry_timeline(stream, device->rate);
            fa_push_restart_timeline(stream->push);
            fa_clock_reset(&stream->clock, first);
        } else {
            fa_clock_reset(&stream->clock, (int64_t)((double)pos * 1e9 / (double)stream->decoder.format.sample_rate + 0.5));
        }
        fa_stream_fill(stream, (int64_t)device->rate * FA_PREFILL_MS / 1000);
    }
    fa_mutex_unlock(&stream->decode_lock);
    if (r == FA_OK) {
        stream->device = device;
        set_bus(stream, bus);
        fa_tap_fit(&stream->tap, device);   /* a meter set up before, for another device */
        /* Track it first: from the moment the voice is live, events it spills must be found. */
        fa_engine_track_stream(stream->engine, stream, 1);
        r = fa_device_add_voice(device, stream);
        if (r == FA_OK) {
            fa_exchange_ptr(&stream->clock_device, device);
            if (was_playing) fa_cas32(&stream->state, FA_STATE_PAUSED, FA_STATE_PLAYING); /* a move keeps playing */
            stream_opened(stream);
        } else {
            fa_engine_track_stream(stream->engine, stream, 0);
            stream->device = NULL;
            set_bus(stream, NULL);
            fa_mutex_lock(&stream->decode_lock);
            stream->dev_rate = 0;
            release_chain(stream);
            fa_mutex_unlock(&stream->decode_lock);
        }
    }
    fa_mutex_unlock(&stream->api_lock);
    return r;
}

FA_API fa_result FA_CALL fa_stream_detach(fa_stream* stream)
{
    if (!stream) return FA_ERROR_INVALID_ARG;
    fa_mixer_stream_leaving(stream, NULL);   /* off a deck first, under the mixer lock */
    fa_stream_detach_internal(stream);
    return FA_OK;
}

void fa_stream_detach_internal(fa_stream* stream)
{
    fa_mutex_lock(&stream->api_lock);
    detach_locked(stream);
    fa_mutex_unlock(&stream->api_lock);
}

/* ---- lanes (parameters) ---------------------------------------------------- */

/* Posts a command to one of the stream's lanes. Caller holds api_lock. */
static void post_lane_locked(struct fa_stream* s, int lane, float target, int64_t ns, const fa_shape* shape,
                             int64_t tag, int notify, int64_t at, int flags)
{
    int64_t replaced_tag = 0;
    int32_t done_seq;
    int replaced;
    /* A replaced ramp that never started gets its RAMP_DONE from here. Its sequence
     * number is taken before the new command becomes visible, so it is delivered
     * before anything the new command raises. The event queues are then emptied:
     * that bounds what the mixer can raise for this stream before the next
     * collection, so its spill slots never overflow (see FA_EVENT_SPILL). */
    fa_mutex_lock(&s->engine->list_lock);
    done_seq = fa_fetch_add32(&s->engine->event_seq, 1);
    replaced = fa_lane_post(&s->lanes[lane], target, ns, shape, tag, notify, at, flags, &replaced_tag);
    fa_event_collect_locked(s->engine);
    if (replaced) {
        fa_event ev;
        ev.type = FA_EVENT_RAMP_DONE;
        ev.stream = s;
        ev.frame = fa_stream_position_now(s);
        ev.value = replaced_tag;
        fa_engine_post_event_locked(s->engine, &ev, done_seq);
    }
    fa_mutex_unlock(&s->engine->list_lock);
}

/* Device frame of a device time; caller holds api_lock and the stream is attached. */
static int64_t device_frame(struct fa_stream* s, fa_time t)
{
    return (int64_t)((double)t * (double)s->device->rate / 1e9 + 0.5);
}

/* ---- transport ----------------------------------------------------------- */

/* Caller holds api_lock. */
/* `loop` >= 0 also sets looping, while neither the mixer nor a worker can see the
 * decode-ahead audio made for the old setting, and restarts at the position heard
 * (read once the mixer stopped, so nothing plays twice); `frame` is then ignored. */
static fa_result seek_loop_locked(struct fa_stream* s, fa_frames frame, int loop)
{
    fa_result r;
    int32_t old_loop = fa_load32(&s->loop);
    if (!s->decoder.seekable) return FA_ERROR_NOT_SEEKABLE;
    if (loop < 0 && (frame < 0 || (s->decoder.length >= 0 && frame > s->decoder.length))) return FA_ERROR_INVALID_ARG;
    if (is_attached(s)) {
        fa_store32(&s->flushing, 1);
        fa_device_wait_quiescent(s->device);   /* the mixer now skips this stream */
    }
    fa_mutex_lock(&s->decode_lock);
    if (loop >= 0) {
        frame = target_position(s);          /* wrapped by the old setting */
        fa_store32(&s->loop, loop);
    }
    r = restart_at_locked(s, frame);
    if (r != FA_OK && loop >= 0) fa_store32(&s->loop, old_loop);
    fa_mutex_unlock(&s->decode_lock);
    unflush(s);
    return r;
}

static fa_result seek_locked(struct fa_stream* s, fa_frames frame) { return seek_loop_locked(s, frame, -1); }

/* Back to the start (CUE_IN) with the crossfade gain reset, for a fresh play.
 * Caller holds api_lock. */
static void rewind_locked(struct fa_stream* s)
{
    fa_frames in = marker(s, FA_MARKER_CUE_IN);
    if (in == FA_FRAMES_UNSET) in = 0;
    if (fa_load32(&s->reverse_want) && song_end(s) >= 0) in = song_end(s);   /* reversed: from the end */
    if (s->decoder.seekable && (in != fa_stream_position_now(s) || fa_load32(&s->eof)))
        seek_locked(s, in);
    post_lane_locked(s, FA_LANE_FADE, 1.0f, 0, NULL, 0, 0, -1, 0);
}

/* Empties the event queues before playback can raise another ENDED or STARTED,
 * which keeps the stream's spill slots from overflowing (see FA_EVENT_SPILL). */
static void collect_events(struct fa_stream* s)
{
    fa_mutex_lock(&s->engine->list_lock);
    fa_event_collect_locked(s->engine);
    fa_mutex_unlock(&s->engine->list_lock);
}

/* Caller holds api_lock; the stream is attached. */
static void start_locked(struct fa_stream* s, int64_t at_frame)
{
    int32_t st = fa_load32(&s->state);
    if (st == FA_STATE_ENDED || st == FA_STATE_STOPPED) rewind_locked(s);
    collect_events(s);
    if (at_frame < 0) {
        fa_store32(&s->state, FA_STATE_PLAYING);
        return;
    }
    fa_store64(&s->start_frame, at_frame);
    fa_store32(&s->state, FA_STATE_SCHEDULED);
}

FA_API fa_result FA_CALL fa_stream_play(fa_stream* stream)
{
    fa_result r = FA_OK;
    if (!stream) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->api_lock);
    if (!is_attached(stream)) r = FA_ERROR_INVALID_ARG;
    else start_locked(stream, -1);
    fa_mutex_unlock(&stream->api_lock);
    return r;
}

FA_API fa_result FA_CALL fa_stream_play_at(fa_stream* stream, fa_time device_time)
{
    fa_result r = FA_OK;
    if (!stream || device_time < 0) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->api_lock);
    if (!is_attached(stream)) r = FA_ERROR_INVALID_ARG;
    else start_locked(stream, device_frame(stream, device_time));
    fa_mutex_unlock(&stream->api_lock);
    return r;
}

FA_API fa_result FA_CALL fa_stream_pause(fa_stream* stream)
{
    fa_result r = FA_OK;
    if (!stream) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->api_lock);
    if (!is_attached(stream)) r = FA_ERROR_INVALID_ARG;
    else {
        /* The mixer may start a scheduled stream, or a push or web stream may start or
         * stop buffering, meanwhile: retry until the state we saw is the one we replaced. */
        for (;;) {
            int32_t st = fa_load32(&stream->state);
            if (st != FA_STATE_PLAYING && st != FA_STATE_SCHEDULED && st != FA_STATE_BUFFERING) break;
            if (fa_cas32(&stream->state, st, FA_STATE_PAUSED)) {
                fa_device_wait_quiescent(stream->device); /* once we return, nothing more is mixed */
                break;
            }
        }
    }
    fa_mutex_unlock(&stream->api_lock);
    return r;
}

FA_API fa_result FA_CALL fa_stream_stop(fa_stream* stream)
{
    fa_result r = FA_OK;
    if (!stream) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->api_lock);
    if (!is_attached(stream)) r = FA_ERROR_INVALID_ARG;
    else {
        fa_store32(&stream->state, FA_STATE_STOPPED);
        fa_device_wait_quiescent(stream->device);  /* once we return, nothing more is mixed */
        if (stream->decoder.seekable) rewind_locked(stream);
    }
    fa_mutex_unlock(&stream->api_lock);
    return r;
}

FA_API fa_result FA_CALL fa_stream_get_state(fa_stream* stream, fa_stream_state* out_state)
{
    if (!stream || !out_state) return FA_ERROR_INVALID_ARG;
    *out_state = (fa_stream_state)fa_load32(&stream->state);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_seek(fa_stream* stream, fa_frames frame)
{
    fa_result r;
    if (!stream || frame < 0) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->api_lock);
    r = seek_locked(stream, frame);
    if (r == FA_OK) {
        /* Stopped and ended mean "at the start" and "at the end": a seek leaves both. */
        fa_cas32(&stream->state, FA_STATE_ENDED, FA_STATE_PAUSED);
        fa_cas32(&stream->state, FA_STATE_STOPPED, FA_STATE_PAUSED);
    }
    fa_mutex_unlock(&stream->api_lock);
    return r;
}

/* The audio decoded ahead was made for the old setting. It stays valid while the
 * decoder has not reached the loop end yet (turning on) or not jumped back yet
 * (turning off); otherwise the stream restarts at the position heard, the way a seek
 * does, with the new setting. */
FA_API fa_result FA_CALL fa_stream_set_loop(fa_stream* stream, int32_t on)
{
    fa_result r = FA_OK;
    fa_frames end, head;
    int restart = 0;
    if (!stream) return FA_ERROR_INVALID_ARG;
    on = on ? 1 : 0;
    if (on && (stream->push || !stream->decoder.seekable)) return FA_ERROR_NOT_SEEKABLE;
    fa_mutex_lock(&stream->api_lock);
    if (fa_load32(&stream->loop) != on) {
        fa_mutex_lock(&stream->decode_lock);
        if (stream->ring.data && fa_load32(&stream->state) != FA_STATE_ENDED && region_of(stream)[1] == 0) {
            end = end_frame(stream);
            head = fa_load64(&stream->seek_base) + stream->decoded;
            restart = on ? (fa_load32(&stream->eof) || stream->draining || stream->tempo_drained || (end >= 0 && head >= end))
                         : stream->jumped_back;
            if (reversed(stream)) restart = 1;   /* the decode-ahead may be past either setting's turn */
        }
        if (!restart) fa_store32(&stream->loop, on);
        fa_mutex_unlock(&stream->decode_lock);
        if (restart) r = seek_loop_locked(stream, 0, on);
    }
    fa_mutex_unlock(&stream->api_lock);
    return r;
}

FA_API fa_result FA_CALL fa_stream_get_loop(fa_stream* stream, int32_t* out_on)
{
    if (!stream || !out_on) return FA_ERROR_INVALID_ARG;
    *out_on = fa_load32(&stream->loop) && stream->decoder.seekable;   /* FA_OPEN_LOOP on a forward-only source: no */
    return FA_OK;
}

/* Changing direction restarts at the position heard, as a seek does: what was
 * decoded ahead was decoded the other way. */
FA_API fa_result FA_CALL fa_stream_set_reverse(fa_stream* stream, int32_t reverse)
{
    fa_result r = FA_OK;
    if (!stream) return FA_ERROR_INVALID_ARG;
    reverse = reverse ? 1 : 0;
    if (reverse && (stream->push || !stream->decoder.seekable)) return FA_ERROR_NOT_SEEKABLE;
    if (reverse && stream->net) return FA_ERROR_NOT_IMPLEMENTED;   /* a range request per block */
    fa_mutex_lock(&stream->api_lock);
    if (fa_load32(&stream->reverse_want) != reverse) {
        fa_store32(&stream->reverse_want, reverse);
        r = seek_loop_locked(stream, 0, fa_load32(&stream->loop));
        if (r != FA_OK) fa_store32(&stream->reverse_want, !reverse);
    }
    fa_mutex_unlock(&stream->api_lock);
    return r;
}

FA_API fa_result FA_CALL fa_stream_get_reverse(fa_stream* stream, int32_t* out_reverse)
{
    if (!stream || !out_reverse) return FA_ERROR_INVALID_ARG;
    *out_reverse = fa_load32(&stream->reverse_want);
    return FA_OK;
}

/* ---- deck loops -------------------------------------------------------------
 * A deck loop changes what the decode-ahead audio should be. Audio the ring already
 * holds is kept up to a join point J: where playback reaches the new out point, or,
 * when the change applies at once, FA_LOOP_MARGIN_MS ahead of the mix (so the refill
 * has time and nothing goes silent). Past J the ring and the position map are cut,
 * the chain restarts at the new source frame, and the decode continues from there.
 * The mixer is kept out only while the ring and the map are cut (microseconds), not
 * while decoding, unlike a seek. Positions stay linear across the join when the
 * new setting allows it, so the loop raises FA_EVENT_LOOPED and markers as usual. */
#define FA_LOOP_MARGIN_MS 15
/* An out point the mix passed less than this ago still loops back as if it had
 * jumped there (keeping the loop's phase); one further behind jumps to the in point. */
#define FA_LOOP_LATE_MS 500

/* Restarts the decode side at source frame `x`, its output entering the ring at
 * ring frame `j` with linear position `lin`. Caller holds api_lock and decode_lock;
 * the ring has been cut at `j`. */
static fa_result rejoin_chain_locked(struct fa_stream* s, int64_t j, double lin, fa_frames x)
{
    double sr = (double)s->decoder.format.sample_rate, dr = (double)s->dev_rate;
    fa_result r = fa_decoder_seek(&s->decoder, x);
    s->in_count = s->in_offset = 0;
    s->chain_buf = NULL;
    s->draining = 0;
    s->tempo_drained = 0;
    s->src_pos = x;
    s->decoded = (fa_frames)(lin + 0.5) - fa_load64(&s->seek_base);
    s->jumped_back = (fa_frames)(lin + 0.5) != x;
    if (s->converter_ready) ma_data_converter_reset(&s->converter);
    /* Tempo ramps go by chain frames: the chain carries on from the frame heard at j. */
    s->tempo_frames = (int64_t)((double)j * sr / dr + 0.5);
    if (s->tempo) {
        int fast, formants, transients;
        fa_mutex_lock(&s->tempo_lock);
        fast = s->tempo_fast;
        formants = s->tempo_formants;
        transients = s->tempo_transients;
        fa_mutex_unlock(&s->tempo_lock);
        fa_tempo_reset(s->tempo, lin);
        fa_tempo_configure(s->tempo, fast, formants, transients);
    }
    if (r != FA_OK) {
        fa_store32(&s->eof, 1);   /* cannot continue: the stream ends at j */
        return r;
    }
    fa_store32(&s->eof, 0);
    fa_stream_fill(s, (int64_t)s->dev_rate * FA_PREFILL_MS / 1000);
    return FA_OK;
}

fa_result fa_stream_set_loop_region(struct fa_stream* s, fa_frames in, fa_frames out)
{
    fa_result r = FA_OK;
    const fa_frames* cur;
    int on = out > in;
    if (!s || in < 0) return FA_ERROR_INVALID_ARG;
    if (!on) in = out = 0;
    if (on && (s->push || !s->decoder.seekable)) return FA_ERROR_NOT_SEEKABLE;
    if (on && s->decoder.length >= 0 && out > s->decoder.length) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&s->api_lock);
    cur = region_of(s);
    if (cur[0] == in && cur[1] == out) { fa_mutex_unlock(&s->api_lock); return FA_OK; }
    if (!is_attached(s) || !s->ring.data || fa_load32(&s->state) == FA_STATE_ENDED ||
        fa_load64(&s->seek_pending) >= 0 || reversed(s)) {
        /* Nothing decoded ahead for the old setting (an async seek will start over).
         * Reversed, what is decoded ahead is dropped: the stream restarts where it is
         * heard, or at the out point when that is before the loop. */
        fa_frames pos = fa_stream_position_now(s);
        int restart = reversed(s) && is_attached(s) && s->ring.data && fa_load32(&s->state) != FA_STATE_ENDED;
        fa_mutex_lock(&s->decode_lock);
        region_store(s, in, out);
        fa_mutex_unlock(&s->decode_lock);
        if (restart) r = seek_locked(s, on && pos <= in ? out : pos);
        else if (on && (reversed(s) ? pos <= in : pos >= out) && fa_load32(&s->state) != FA_STATE_ENDED)
            r = seek_locked(s, reversed(s) ? out : in);   /* outside the loop on the side already passed */
        fa_mutex_unlock(&s->api_lock);
        return r;
    }
    fa_mutex_lock(&s->decode_lock);   /* no worker decodes meanwhile */
    {
        double sr = (double)s->decoder.format.sample_rate, dr = (double)s->dev_rate;
        double head = (double)(fa_load64(&s->seek_base) + s->decoded), lj, lin;
        fa_frames old_end = end_frame(s), a, x, len = out - in;
        int old_loops = loops(s) && old_end > 0;
        int64_t played, write, j, margin = (int64_t)s->dev_rate * FA_LOOP_MARGIN_MS / 1000;
        fa_tmap* m = tmap_of(s);
        if (!s->jumped_back && (!old_loops || head < (double)old_end) && (!on || head < (double)out)) {
            /* The decoder has not reached either setting's turn yet: it takes the new one there. */
            region_store(s, in, out);
            fa_mutex_unlock(&s->decode_lock);
            fa_mutex_unlock(&s->api_lock);
            return FA_OK;
        }
        if (!m) {
            /* The join needs a position map (readers take it when it appears, empty). */
            m = (fa_tmap*)calloc(1, sizeof(*m));
            if (!m) { fa_mutex_unlock(&s->decode_lock); fa_mutex_unlock(&s->api_lock); return FA_ERROR_OUT_OF_MEMORY; }
            fa_exchange_ptr(&s->tmap, m);
        }
        fa_device_wait_idle(s->device);        /* between callbacks, so none skips the stream */
        fa_store32(&s->flushing, 1);
        fa_device_wait_quiescent(s->device);   /* the mixer skips the stream until unflush */
        played = fa_load64(&s->played);
        write = fa_load64(&s->ring.write);
        j = played + margin < write ? played + margin : write;
        lj = linear_position(s, j);
        a = wrap_position(s, (fa_frames)(lj + 0.5));   /* where the old setting plays at j */
        if (on && (fa_frames)(lj + 0.5) == a && lj < (double)out && linear_position(s, write) > (double)out) {
            /* The new out point is ahead in the ring: join exactly there. */
            int64_t jo = played + frames_until(s, played, (double)out);
            if (jo > j && jo <= write) { j = jo; lj = linear_position(s, j); a = (fa_frames)(lj + 0.5); }
        }
        region_store(s, in, out);
        if (on && a >= out && a - out < (fa_frames)(sr * FA_LOOP_LATE_MS / 1000.0)) {
            /* Just past the out point: continue as if it had looped there. */
            lin = (fa_frames)(lj + 0.5) == a ? lj : (double)a;
            x = in + (a - out) % len;
        } else if (on && a >= out) {
            lin = (double)in;   /* far past it: to the in point */
            x = in;
        } else {
            lin = (fa_frames)(lj + 0.5) == a ? lj : (double)a;
            x = a;
        }
        fa_store64(&s->ring.write, j);
        fa_tmap_truncate(m, j);
        fa_tmap_add(m, j, lin, sr / dr * speed_now(s, (int64_t)((double)j * sr / dr + 0.5), s->tempo && fa_tempo_engaged(s->tempo)));
        unflush(s);
        r = rejoin_chain_locked(s, j, lin, x);
    }
    fa_mutex_unlock(&s->decode_lock);
    fa_mutex_unlock(&s->api_lock);
    return r;
}

void fa_stream_get_loop_region(struct fa_stream* s, fa_frames* out_in, fa_frames* out_out)
{
    /* region_idx counts the changes: if it did not move during the copy, no second
     * change rewrote the slot being read. The second read is a read-modify-write so the
     * copy cannot move past it. */
    int32_t idx;
    do {
        idx = fa_load32(&s->region_idx);
        *out_in = s->region[idx & 1][0];
        *out_out = s->region[idx & 1][1];
    } while (fa_fetch_add32(&s->region_idx, 0) != idx);
}

/* Where fa_stream_jump joins the ring (decode_lock held), for the mix at `played`:
 * more than `write` while `at` (or the margin) is not decoded yet. */
static int64_t jump_point(struct fa_stream* s, fa_frames at, int64_t played, int64_t write)
{
    int64_t j = played + (int64_t)s->dev_rate * FA_LOOP_MARGIN_MS / 1000;
    if (j <= write && at >= 0) {
        double lp = linear_position(s, played);
        double target = lp + (double)(at - wrap_position(s, (fa_frames)(lp + 0.5)));
        if (target > linear_position(s, write)) j = write + 1;   /* not decoded yet */
        else if (target > linear_position(s, j)) j = played + frames_until(s, played, target);
    }
    return j;
}

/* A one-off jump (sync.c), joined like a deck loop: where playback reaches source
 * frame `at` (as heard, so inside a deck loop too), it continues `delta` frames
 * further on (or back), with positions following the jump. The join is at `at` when
 * that is decoded ahead and at least FA_LOOP_MARGIN_MS ahead of the mix; `at` < 0 or
 * already that close joins a margin ahead of the mix. FA_ERROR_BUSY while `at` (or
 * the margin) is not decoded yet, or the stream is not playing from its ring: try
 * again later. */
fa_result fa_stream_jump(struct fa_stream* s, fa_frames at, fa_frames delta)
{
    fa_result r = FA_ERROR_BUSY;
    if (!s) return FA_ERROR_INVALID_ARG;
    if (s->push || !s->decoder.seekable) return FA_ERROR_NOT_SEEKABLE;
    fa_mutex_lock(&s->api_lock);
    if (reversed(s) && is_attached(s) && fa_load32(&s->state) == FA_STATE_PLAYING) {
        /* Reversed: at once, as a seek (the join is made for forward playback). */
        fa_frames x = target_position(s) + delta, last = s->decoder.length > 0 ? s->decoder.length - 1 : -1;
        if (x < 0) x = 0;
        if (last >= 0 && x > last) x = last;
        r = seek_locked(s, x);
        fa_mutex_unlock(&s->api_lock);
        return r;
    }
    if (!is_attached(s) || !s->ring.data || fa_load32(&s->state) != FA_STATE_PLAYING || fa_load64(&s->seek_pending) >= 0) {
        fa_mutex_unlock(&s->api_lock);
        return FA_ERROR_BUSY;
    }
    fa_mutex_lock(&s->decode_lock);   /* no worker decodes meanwhile */
    {
        double sr = (double)s->decoder.format.sample_rate, dr = (double)s->dev_rate, lj, lin;
        int64_t played, write = fa_load64(&s->ring.write), j;
        fa_frames a, x, last = s->decoder.length > 0 ? s->decoder.length - 1 : -1;
        fa_tmap* m = tmap_of(s);
        if (jump_point(s, at, fa_load64(&s->played), write) > write) {
            /* Not decoded yet: asked again until it is (sync.c every few ms), so leave
             * the mix alone instead of flushing it each time. */
            fa_mutex_unlock(&s->decode_lock);
            fa_mutex_unlock(&s->api_lock);
            return FA_ERROR_BUSY;
        }
        if (!m) {
            m = (fa_tmap*)calloc(1, sizeof(*m));
            if (!m) { fa_mutex_unlock(&s->decode_lock); fa_mutex_unlock(&s->api_lock); return FA_ERROR_OUT_OF_MEMORY; }
            fa_exchange_ptr(&s->tmap, m);
        }
        fa_device_wait_idle(s->device);        /* between callbacks, so none skips the stream */
        fa_store32(&s->flushing, 1);
        fa_device_wait_quiescent(s->device);   /* the mixer skips the stream until unflush */
        played = fa_load64(&s->played);
        j = jump_point(s, at, played, write);
        if (j > write) {
            unflush(s);
        } else {
            lj = linear_position(s, j);
            a = wrap_position(s, (fa_frames)(lj + 0.5));   /* where it plays at j */
            x = a + delta;
            if (x < 0) x = 0;
            if (last >= 0 && x > last) x = last;
            lin = (double)x + (lj - floor(lj + 0.5));      /* keeps the fraction of a frame */
            fa_store64(&s->ring.write, j);
            fa_tmap_truncate(m, j);
            fa_tmap_add(m, j, lin, sr / dr * speed_now(s, (int64_t)((double)j * sr / dr + 0.5), s->tempo && fa_tempo_engaged(s->tempo)));
            unflush(s);
            r = rejoin_chain_locked(s, j, lin, x);
        }
    }
    fa_mutex_unlock(&s->decode_lock);
    fa_mutex_unlock(&s->api_lock);
    return r;
}

FA_API fa_result FA_CALL fa_stream_set_mute(fa_stream* stream, int32_t muted)
{
    if (!stream) return FA_ERROR_INVALID_ARG;
    fa_store32(&stream->muted, muted ? 1 : 0);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_mute(fa_stream* stream, int32_t* out_muted)
{
    if (!stream || !out_muted) return FA_ERROR_INVALID_ARG;
    *out_muted = fa_load32(&stream->muted);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_position(fa_stream* stream, fa_frames* out_frame)
{
    if (!stream || !out_frame) return FA_ERROR_INVALID_ARG;
    /* A pending seek's target, without waiting for the worker carrying it out. */
    if ((*out_frame = fa_load64(&stream->seek_pending)) >= 0) return FA_OK;
    fa_mutex_lock(&stream->api_lock);
    *out_frame = fa_stream_position_now(stream);
    fa_mutex_unlock(&stream->api_lock);
    return FA_OK;
}

static int64_t clock_on_device(struct fa_stream* s, fa_device* dev, int64_t now_ns)
{
    return fa_clock_at(&s->clock, fa_device_speaker_frame(dev, now_ns));
}

/* File streams that are not attached: the position, as time. */
static int64_t clock_of_position(struct fa_stream* s)
{
    return (int64_t)((double)fa_load64(&s->position) * 1e9 / (double)s->decoder.format.sample_rate + 0.5);
}

/* The time of a pending fa_stream_seek_async, or -1. */
static int64_t pending_clock(struct fa_stream* s)
{
    int64_t pending = fa_load64(&s->seek_pending);
    return pending < 0 ? -1 : (int64_t)((double)pending * 1e9 / (double)s->decoder.format.sample_rate + 0.5);
}

int64_t fa_stream_clock_at(struct fa_stream* s, int64_t now_ns)
{
    int64_t pending = pending_clock(s);
    if (pending >= 0) return pending;
    if (s->device) return clock_on_device(s, s->device, now_ns);
    if (s->push) {
        /* Detached: audio decoded but never heard waits in carry, and is heard next. */
        if (s->carry_count > 0) return carry_first_pts(s);
        return fa_push_next_pts(s->push);
    }
    return clock_of_position(s);
}

/* No lock while attached or for file streams: a render loop calls this every vsync,
 * and must not wait behind a seek (which holds api_lock while it refills) or a
 * worker. Detached push streams read their carry, which the API lock guards. */
FA_API fa_result FA_CALL fa_stream_get_clock(fa_stream* stream, fa_time* out_pts)
{
    fa_device* dev;
    if (!stream || !out_pts) return FA_ERROR_INVALID_ARG;
    if ((*out_pts = pending_clock(stream)) >= 0) return FA_OK;
    fa_fetch_add32(&stream->clock_readers, 1);
    dev = (fa_device*)fa_load_ptr(&stream->clock_device);
    if (dev) *out_pts = clock_on_device(stream, dev, fa_monotonic_ns());
    fa_fetch_add32(&stream->clock_readers, -1);
    if (dev) return FA_OK;
    if (!stream->push) {
        *out_pts = clock_of_position(stream);
        return FA_OK;
    }
    fa_mutex_lock(&stream->api_lock);
    *out_pts = fa_stream_clock_at(stream, fa_monotonic_ns());
    fa_mutex_unlock(&stream->api_lock);
    return FA_OK;
}

/* ---- push streams ---------------------------------------------------------- */

FA_API fa_result FA_CALL fa_stream_push_flush(fa_stream* stream)
{
    int64_t hold;
    if (!stream || !stream->push) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->api_lock);
    hold = fa_stream_clock_at(stream, fa_monotonic_ns());
    if (is_attached(stream)) {
        fa_store32(&stream->flushing, 1);
        fa_device_wait_quiescent(stream->device);   /* the mixer now skips this stream */
    }
    fa_mutex_lock(&stream->decode_lock);
    fa_push_flush_locked(stream->push, fa_load64(&stream->clock.count) > 0);
    /* Audio kept from a detach is decoded-ahead audio too. */
    stream->carry_count = stream->carry_offset = 0;
    stream->anchors = 0;
    fa_store32(&stream->staged, 0);
    if (stream->ring.data) {
        fa_frames pos = fa_stream_position_now(stream);
        fa_ring_reset(&stream->ring);
        stream->in_count = stream->in_offset = 0;
        stream->decoded = 0;
        stream->draining = 0;
        if (stream->converter_ready) ma_data_converter_reset(&stream->converter);
        fa_store32(&stream->eof, 0);
        timeline_reset(stream, pos);
        fa_store64(&stream->position, pos);
        stream->src_pos = pos;
    }
    fa_store64(&stream->played_audible, 0);
    fa_ptsmap_reset(ptsmap_of(stream));
    fa_clock_reset(&stream->clock, hold);
    fa_mutex_unlock(&stream->decode_lock);
    fa_store32(&stream->flushing, 0);
    fa_mutex_unlock(&stream->api_lock);
    return FA_OK;
}

/* Counted by durations, never by timestamps, so jumps in the app's timeline don't
 * count as audio: the push queue and what its decoder holds, what the stream read
 * but hasn't put in its ring, the ring, and what the device has yet to play. */
FA_API fa_result FA_CALL fa_stream_get_buffered(fa_stream* stream, fa_time* out_duration)
{
    if (!stream || !out_duration) return FA_ERROR_INVALID_ARG;
    *out_duration = 0;
    if (!stream->push) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->api_lock);
    *out_duration = fa_stream_buffered_at(stream, fa_monotonic_ns());
    fa_mutex_unlock(&stream->api_lock);
    return FA_OK;
}

int64_t fa_stream_buffered_at(struct fa_stream* stream, int64_t now_ns)
{
    int64_t total;
    double sr = (double)stream->decoder.format.sample_rate;
    total = fa_push_buffered_ns(stream->push);
    fa_mutex_lock(&stream->decode_lock);
    total += (int64_t)((double)(stream->in_count + stream->carry_count) * 1e9 / sr + 0.5);
    if (stream->ring.data && stream->dev_rate > 0)
        total += (int64_t)((double)fa_ring_readable(&stream->ring) * 1e9 / (double)stream->dev_rate + 0.5);
    fa_mutex_unlock(&stream->decode_lock);
    if (stream->device && stream->dev_rate > 0) {
        /* In the device buffer: what was mixed and hasn't reached the speaker yet,
         * whatever the state now (the last frames before a stall or the end too). */
        int64_t end = fa_clock_mixed_end(&stream->clock);
        if (end >= 0) {
            double ahead = (double)end - fa_device_speaker_frame(stream->device, now_ns);
            if (ahead > 0.0) total += (int64_t)(ahead * 1e9 / (double)stream->dev_rate + 0.5);
        }
    }
    return total;
}

/* ---- parameters ---------------------------------------------------------- */

static fa_result check_param(fa_param param, float* value)
{
    if (param == FA_PARAM_VOLUME) {
        if (!(*value >= 0.0f)) return FA_ERROR_INVALID_ARG;
        return FA_OK;
    }
    if (param == FA_PARAM_PAN) {
        if (*value != *value) return FA_ERROR_INVALID_ARG;
        if (*value < -1.0f) *value = -1.0f;
        if (*value > 1.0f) *value = 1.0f;
        return FA_OK;
    }
    if (param == FA_PARAM_RATE || param == FA_PARAM_TEMPO) {
        if (!(*value > 0.0f)) return FA_ERROR_INVALID_ARG;
        if (*value < 0.1f) *value = 0.1f;
        if (*value > 10.0f) *value = 10.0f;
        return FA_OK;
    }
    if (param == FA_PARAM_PITCH) {
        if (*value != *value) return FA_ERROR_INVALID_ARG;
        if (*value < -36.0f) *value = -36.0f;
        if (*value > 36.0f) *value = 36.0f;
        return FA_OK;
    }
    return FA_ERROR_INVALID_ARG;
}

/* Buses mix finished audio: they have volume and pan only. */
fa_result fa_param_check(fa_param param, float* value)
{
    if (param != FA_PARAM_VOLUME && param != FA_PARAM_PAN) return FA_ERROR_INVALID_ARG;
    return check_param(param, value);
}

static int is_tempo_param(fa_param param)
{
    return param == FA_PARAM_RATE || param == FA_PARAM_TEMPO || param == FA_PARAM_PITCH;
}

/* A set (duration 0, notify 0) or ramp of RATE, TEMPO or PITCH. Caller holds api_lock. */
static fa_result post_tempo_locked(struct fa_stream* s, fa_param param, float target, fa_time duration,
                                   const fa_shape* shape, int64_t tag, int notify, fa_time device_time)
{
    double sr = (double)s->decoder.format.sample_rate;
    int64_t len = (int64_t)((double)duration * sr / 1e9 + 0.5), start = FA_TPARAM_NOW, old_tag = 0;
    int replaced, wanted, held;
    if (s->decoder.format.channels > FA_TEMPO_MAX_CHANNELS && !s->matrix &&
        (notify || len > 0 || target != (param == FA_PARAM_PITCH ? 0.0f : 1.0f)))
        return FA_ERROR_UNSUPPORTED_FORMAT;   /* more channels than the chain takes */
    if (device_time != 0 && is_attached(s)) {
        /* The ring frame playing at that device time, as a chain frame. */
        int64_t ring = fa_load64(&s->played) + (device_frame(s, device_time) - fa_load64(&s->device->clock));
        start = ring > 0 ? (int64_t)((double)ring * sr / (double)s->dev_rate + 0.5) : 0;
    }
    /* A first use may restart the stream (below). Workers are kept out from the post
     * on: one still decoding the old timeline, up to the whole decode-ahead buffer
     * past what is heard, would start (or even finish) the ramp there. */
    held = !fa_load32(&s->tempo_live) && is_attached(s) && s->decoder.seekable && fa_load32(&s->state) != FA_STATE_ENDED;
    if (held) fa_mutex_lock(&s->decode_lock);
    fa_mutex_lock(&s->tempo_lock);
    replaced = fa_tparam_post(&s->tparams[tempo_slot(param)], target, len, shape, tag, notify, start, &old_tag);
    s->tempo_posts++;
    wanted = tempo_wanted_locked(s);
    fa_mutex_unlock(&s->tempo_lock);
    if (replaced) tempo_ramp_done(s, old_tag);
    if (wanted && !fa_load32(&s->tempo_live)) {
        fa_store32(&s->tempo_live, 1);
        if (held) {
            /* First use: decode again from what is heard now, so the change does not
             * wait for the whole decode-ahead buffer to play out. (The audio thread
             * never takes decode_lock, so waiting for it here is safe.) */
            fa_frames pos;
            fa_store32(&s->flushing, 1);
            fa_device_wait_quiescent(s->device);   /* the mixer now skips this stream */
            pos = target_position(s);              /* after the mixer let go: nothing more is consumed */
            restart_at_locked(s, pos);
            unflush(s);
        }
    }
    if (held) fa_mutex_unlock(&s->decode_lock);
    return FA_OK;
}

fa_result fa_stream_ramp_tempo_quiet(struct fa_stream* s, float target, fa_time duration)
{
    fa_result r = check_param(FA_PARAM_TEMPO, &target);
    if (r != FA_OK || duration < 0) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&s->api_lock);
    r = post_tempo_locked(s, FA_PARAM_TEMPO, target, duration, NULL, 0, 0, 0);
    fa_mutex_unlock(&s->api_lock);
    return r;
}

FA_API fa_result FA_CALL fa_stream_set_param(fa_stream* stream, fa_param param, float value)
{
    fa_result r;
    if (!stream) return FA_ERROR_INVALID_ARG;
    r = check_param(param, &value);
    if (r != FA_OK) return r;
    fa_mutex_lock(&stream->api_lock);
    if (is_tempo_param(param)) r = post_tempo_locked(stream, param, value, 0, NULL, 0, 0, 0);
    else post_lane_locked(stream, param, value, FA_DEZIPPER_NS, NULL, 0, 0, -1, 0);
    fa_mutex_unlock(&stream->api_lock);
    return r;
}

FA_API fa_result FA_CALL fa_stream_ramp_param(fa_stream* stream, fa_param param, float target, fa_time duration, fa_time device_time, const fa_ramp* ramp)
{
    fa_result r;
    fa_shape shape;
    if (!stream || duration < 0 || device_time < 0) return FA_ERROR_INVALID_ARG;
    r = fa_shape_from_ramp(ramp, FA_CURVE_LINEAR, &shape);
    if (r != FA_OK) return r;
    r = check_param(param, &target);
    if (r != FA_OK) return r;
    fa_mutex_lock(&stream->api_lock);
    if (device_time != 0 && !is_attached(stream)) r = FA_ERROR_INVALID_ARG;
    else if (is_tempo_param(param))
        r = post_tempo_locked(stream, param, target, duration, &shape, ramp ? ramp->tag : 0, 1, device_time);
    else post_lane_locked(stream, param, target, duration, &shape, ramp ? ramp->tag : 0, 1,
                          device_time ? device_frame(stream, device_time) : -1, 0);
    fa_mutex_unlock(&stream->api_lock);
    return r;
}

FA_API fa_result FA_CALL fa_stream_get_param(fa_stream* stream, fa_param param, float* out_value)
{
    if (!stream || !out_value) return FA_ERROR_INVALID_ARG;
    if (param != FA_PARAM_VOLUME && param != FA_PARAM_PAN && !is_tempo_param(param)) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->api_lock);
    if (is_tempo_param(param)) {
        fa_mutex_lock(&stream->tempo_lock);
        *out_value = stream->tparams[tempo_slot(param)].value;
        fa_mutex_unlock(&stream->tempo_lock);
    } else {
        *out_value = fa_lane_get(&stream->lanes[param]);
    }
    fa_mutex_unlock(&stream->api_lock);
    return FA_OK;
}

/* ---- tempo options ------------------------------------------------------- */

FA_API void FA_CALL fa_tempo_options_init(fa_tempo_options* options)
{
    if (!options) return;
    memset(options, 0, sizeof(*options));
    options->struct_size = (uint32_t)sizeof(*options);
    options->quality = FA_STRETCH_BEST;
    options->latency_ms = FA_TEMPO_LATENCY_MS;
}

FA_API fa_result FA_CALL fa_stream_set_tempo_options(fa_stream* stream, const fa_tempo_options* options)
{
    fa_tempo_options o;
    int fast, transients, rebuild;
    uint32_t lat;
    if (!stream) return FA_ERROR_INVALID_ARG;
    fa_tempo_options_init(&o);
    if (options) {
        if (options->struct_size < offsetof(fa_tempo_options, latency_ms)) return FA_ERROR_INVALID_ARG;
        o.quality = options->quality;
        o.keep_formants = options->keep_formants;
        if (options->struct_size >= offsetof(fa_tempo_options, latency_ms) + sizeof(options->latency_ms))
            o.latency_ms = options->latency_ms;
    }
    if (o.quality != FA_STRETCH_BEST && o.quality != FA_STRETCH_FAST && o.quality != FA_STRETCH_TRANSIENTS)
        return FA_ERROR_INVALID_ARG;
    fast = o.quality == FA_STRETCH_FAST;
    transients = o.quality == FA_STRETCH_TRANSIENTS;
    lat = o.latency_ms ? o.latency_ms : FA_TEMPO_LATENCY_MS;
    if (lat < 50) lat = 50;
    if (lat > stream->buffer_ms) lat = stream->buffer_ms;
    fa_mutex_lock(&stream->api_lock);
    fa_mutex_lock(&stream->tempo_lock);
    rebuild = fast != stream->tempo_fast || transients != stream->tempo_transients;
    stream->tempo_fast = fast;
    stream->tempo_transients = transients;
    stream->tempo_formants = o.keep_formants != 0;
    stream->tempo_latency_ms = lat;
    fa_mutex_unlock(&stream->tempo_lock);
    fa_mutex_lock(&stream->decode_lock);
    if (stream->tempo) fa_tempo_configure(stream->tempo, fast, o.keep_formants, transients);
    rebuild = rebuild && stream->tempo && stream->tempo->st_on;
    fa_mutex_unlock(&stream->decode_lock);
    /* Another stretcher: start it from where the stream is heard. */
    if (rebuild && stream->decoder.seekable && fa_load32(&stream->state) != FA_STATE_ENDED)
        seek_locked(stream, fa_stream_position_now(stream));
    fa_mutex_unlock(&stream->api_lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_tempo_options(fa_stream* stream, fa_tempo_options* out_options)
{
    if (!stream || !out_options) return FA_ERROR_INVALID_ARG;
    fa_tempo_options_init(out_options);
    fa_mutex_lock(&stream->tempo_lock);
    out_options->quality = stream->tempo_fast ? FA_STRETCH_FAST : stream->tempo_transients ? FA_STRETCH_TRANSIENTS : FA_STRETCH_BEST;
    out_options->keep_formants = stream->tempo_formants;
    out_options->latency_ms = stream->tempo_latency_ms;
    fa_mutex_unlock(&stream->tempo_lock);
    return FA_OK;
}

/* ---- markers ------------------------------------------------------------ */

FA_API fa_result FA_CALL fa_stream_set_marker(fa_stream* stream, fa_marker which, fa_frames frame)
{
    fa_frames old;
    if (!stream || (int)which < 0 || (int)which >= FA_MARKER_COUNT) return FA_ERROR_INVALID_ARG;
    if (frame < 0 && frame != FA_FRAMES_UNSET) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->api_lock);
    old = fa_exchange64(&stream->markers[which], frame);
    if (which == FA_MARKER_CUE_IN && fa_load32(&stream->state) == FA_STATE_STOPPED && stream->decoder.seekable) {
        /* Stopped means "at the start": move there. */
        fa_frames in = frame == FA_FRAMES_UNSET ? 0 : frame;
        if (stream->decoder.length < 0 || in <= stream->decoder.length) seek_locked(stream, in);
    }
    if (which == FA_MARKER_CUE_OUT && old != frame && is_attached(stream) && stream->decoder.seekable) {
        /* Audio decoded ahead may already run past the new end (or stop short of a
         * later one): decode again from where playback is. */
        fa_frames pos = fa_stream_position_now(stream);
        if (frame == FA_FRAMES_UNSET || pos < frame) seek_locked(stream, pos);
    }
    fa_mutex_unlock(&stream->api_lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_marker(fa_stream* stream, fa_marker which, fa_frames* out_frame)
{
    if (!stream || !out_frame || (int)which < 0 || (int)which >= FA_MARKER_COUNT) return FA_ERROR_INVALID_ARG;
    *out_frame = marker(stream, which);
    return FA_OK;
}

/* ---- position events ------------------------------------------------------ */

FA_API fa_result FA_CALL fa_stream_add_position_event(fa_stream* stream, fa_frames frame)
{
    fa_result r = FA_ERROR_BUSY;
    int i;
    if (!stream || frame < FA_FRAMES_UNSET) return FA_ERROR_INVALID_ARG;
    if (frame != FA_FRAMES_UNSET && stream->decoder.length >= 0 && frame > stream->decoder.length)
        return FA_ERROR_INVALID_ARG;
    /* api_lock serializes adds and clears; the mixer only ever frees a slot (CAS). */
    fa_mutex_lock(&stream->api_lock);
    if (frame == FA_FRAMES_UNSET) {
        for (i = 0; i < FA_POSITION_EVENT_SLOTS; i++)
            if (fa_exchange64(&stream->pos_events[i], FA_FRAMES_UNSET) != FA_FRAMES_UNSET)
                fa_fetch_add32(&stream->pos_armed, -1);
        r = FA_OK;
    } else {
        int armed = 0;
        for (i = 0; i < FA_POSITION_EVENT_SLOTS && !armed; i++) armed = fa_load64(&stream->pos_events[i]) == frame;
        if (!armed) {
            fa_fetch_add32(&stream->pos_armed, 1);   /* first, so the mixer never skips an armed slot */
            for (i = 0; i < FA_POSITION_EVENT_SLOTS && !armed; i++)
                armed = fa_cas64(&stream->pos_events[i], FA_FRAMES_UNSET, frame);
            if (!armed) fa_fetch_add32(&stream->pos_armed, -1);
        }
        r = armed ? FA_OK : FA_ERROR_BUSY;
    }
    fa_mutex_unlock(&stream->api_lock);
    return r;
}

FA_API fa_result FA_CALL fa_stream_set_fade_curve(fa_stream* stream, fa_fade fade, const fa_ramp* shape)
{
    fa_shape sh;
    fa_result r;
    if (!stream || (fade != FA_FADE_IN && fade != FA_FADE_OUT)) return FA_ERROR_INVALID_ARG;
    r = fa_shape_from_ramp(shape, FA_CURVE_EQUAL_POWER, &sh);
    if (r != FA_OK) return r;
    fa_mutex_lock(&stream->api_lock);
    fa_shape_store(&stream->fade_shape[fade], &sh);
    fa_mutex_unlock(&stream->api_lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_fade_curve(fa_stream* stream, fa_fade fade, fa_ramp* out_shape)
{
    fa_shape sh;
    if (!stream || !out_shape || (fade != FA_FADE_IN && fade != FA_FADE_OUT)) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->api_lock);
    fa_shape_load(&stream->fade_shape[fade], &sh);
    fa_mutex_unlock(&stream->api_lock);
    fa_shape_to_ramp(&sh, out_shape);
    return FA_OK;
}

/* ---- crossfades and queues -------------------------------------------------- */

/* Makes sure `next` plays on `stream`'s device, through the same bus. */
static fa_result join_device(struct fa_stream* stream, struct fa_stream* next, fa_device** out_dev)
{
    fa_device* dev;
    fa_bus* bus;
    fa_device* other;
    fa_mutex_lock(&stream->api_lock);
    dev = stream->device;
    bus = (fa_bus*)fa_load_ptr(&stream->bus);
    fa_mutex_unlock(&stream->api_lock);
    if (!dev) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&next->api_lock);
    other = next->device;
    fa_mutex_unlock(&next->api_lock);
    if (other && other != dev) return FA_ERROR_INVALID_ARG;
    if (!other) {
        fa_result r;
        if (bus && bus->deck) return FA_ERROR_INVALID_ARG;   /* a deck holds one stream: fa_deck_load */
        r = fa_stream_attach(next, dev, bus);
        if (r != FA_OK) return r;
    }
    *out_dev = dev;
    return FA_OK;
}

/* Two streams' api_locks, always in the same (address) order. */
static void lock_pair(struct fa_stream* a, struct fa_stream* b)
{
    if ((uintptr_t)a > (uintptr_t)b) { struct fa_stream* t = a; a = b; b = t; }
    fa_mutex_lock(&a->api_lock);
    fa_mutex_lock(&b->api_lock);
}

static void unlock_pair(struct fa_stream* a, struct fa_stream* b)
{
    fa_mutex_unlock(&a->api_lock);
    fa_mutex_unlock(&b->api_lock);
}

FA_API fa_result FA_CALL fa_crossfade(fa_stream* from, fa_stream* to, fa_time duration, const fa_ramp* ramp)
{
    fa_shape shape;
    fa_device* dev;
    fa_result r;
    int64_t at, tag;
    if (!from || !to || from == to || duration < 0) return FA_ERROR_INVALID_ARG;
    if (from->engine != to->engine || (to->flags & FA_OPEN_DECODE_ONLY)) return FA_ERROR_INVALID_ARG;
    r = fa_shape_from_ramp(ramp, FA_CURVE_EQUAL_POWER, &shape);
    if (r != FA_OK) return r;
    r = join_device(from, to, &dev);
    if (r != FA_OK) return r;
    tag = ramp ? ramp->tag : 0;

    /* Rewind `to` first: that seeks and prefills, which must not eat into the lead time. */
    fa_mutex_lock(&to->api_lock);
    if (to->device != dev) r = FA_ERROR_INVALID_ARG;  /* moved away meanwhile */
    else if (fa_load32(&to->state) == FA_STATE_ENDED || fa_load32(&to->state) == FA_STATE_STOPPED) rewind_locked(to);
    fa_mutex_unlock(&to->api_lock);
    if (r != FA_OK) return r;

    /* Then publish the start and both fades back to back, holding both streams and,
     * while posting, the device's xf_lock, for the start of the next callback: no
     * callback takes commands meanwhile, and one that starts anyway leaves every
     * command due by then for the callback after it, which starts both together. */
    lock_pair(from, to);
    if (to->device != dev || from->device != dev) r = FA_ERROR_INVALID_ARG;
    else {
        int32_t st = fa_load32(&to->state);
        if (st != FA_STATE_PLAYING) {
            if (st == FA_STATE_ENDED || st == FA_STATE_STOPPED) rewind_locked(to);  /* changed meanwhile */
            collect_events(to);
        }
        fa_mutex_lock(&dev->xf_lock);
        at = fa_load64(&dev->clock);
        if (st != FA_STATE_PLAYING) {
            fa_store64(&to->start_frame, at);
            fa_store32(&to->state, FA_STATE_SCHEDULED);
            post_lane_locked(to, FA_LANE_FADE, 0.0f, 0, NULL, 0, 0, -1, 0);
        }
        post_lane_locked(to, FA_LANE_FADE, 1.0f, duration, &shape, tag, 1, at, 0);
        post_lane_locked(from, FA_LANE_FADE, 0.0f, duration, &shape, tag, 1, at, FA_LANE_MIRROR | FA_LANE_END_STREAM);
        fa_mutex_unlock(&dev->xf_lock);
    }
    unlock_pair(from, to);
    return r;
}

FA_API fa_result FA_CALL fa_stream_set_next(fa_stream* stream, fa_stream* next, fa_time crossfade, const fa_ramp* shape)
{
    fa_shape sh;
    fa_device* dev;
    fa_result r;
    struct fa_stream* old;
    int start_now = 0;
    if (!stream || next == stream || crossfade < 0) return FA_ERROR_INVALID_ARG;
    if (!next) {
        fa_mutex_lock(&stream->api_lock);
        unqueue((struct fa_stream*)fa_exchange_ptr(&stream->queued, NULL));
        fa_mutex_unlock(&stream->api_lock);
        return FA_OK;
    }
    if (next->engine != stream->engine || (next->flags & FA_OPEN_DECODE_ONLY)) return FA_ERROR_INVALID_ARG;
    r = fa_shape_from_ramp(shape, FA_CURVE_EQUAL_POWER, &sh);
    if (r != FA_OK) return r;
    if (fa_load32(&next->state) == FA_STATE_PLAYING || fa_load32(&next->state) == FA_STATE_BUFFERING) return FA_ERROR_BUSY;
    r = join_device(stream, next, &dev);
    if (r != FA_OK) return r;

    /* Ready to go: positioned at its cue-in and buffered, waiting for the trigger. */
    fa_mutex_lock(&next->api_lock);
    if (next->device != dev) r = FA_ERROR_INVALID_ARG;
    else if (fa_load32(&next->state) == FA_STATE_PLAYING || fa_load32(&next->state) == FA_STATE_BUFFERING) r = FA_ERROR_BUSY;
    else {
        int32_t st = fa_load32(&next->state);
        if (st == FA_STATE_ENDED || st == FA_STATE_STOPPED) rewind_locked(next);
        collect_events(next);
        fa_store64(&next->start_frame, FA_START_QUEUED);
        fa_store32(&next->state, FA_STATE_SCHEDULED);
    }
    fa_mutex_unlock(&next->api_lock);
    if (r != FA_OK) return r;

    fa_mutex_lock(&stream->api_lock);
    if (stream->device != dev) r = FA_ERROR_INVALID_ARG;
    else {
        fa_store64(&stream->xfade_ns, crossfade);
        fa_shape_store(&stream->xfade_shape, &sh);
        old = (struct fa_stream*)fa_exchange_ptr(&stream->queued, next);
        if (old != next) unqueue(old);
        start_now = fa_load32(&stream->state) == FA_STATE_ENDED;
    }
    fa_mutex_unlock(&stream->api_lock);
    if (r != FA_OK) { unqueue(next); return r; }

    if (start_now && fa_cas_ptr(&stream->queued, next, NULL)) {
        /* Too late for a transition: the stream has already ended. Start `next` on
         * the next callback, through the scheduled start (which raises STARTED). */
        fa_mutex_lock(&next->api_lock);
        if (fa_load32(&next->state) == FA_STATE_SCHEDULED && fa_load64(&next->start_frame) == FA_START_QUEUED)
            fa_store64(&next->start_frame, 0);
        fa_mutex_unlock(&next->api_lock);
    }
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_next(fa_stream* stream, fa_stream** out_next)
{
    if (!stream || !out_next) return FA_ERROR_INVALID_ARG;
    *out_next = (fa_stream*)fa_load_ptr(&stream->queued);
    return FA_OK;
}

/* ---- channel matrix ----------------------------------------------------- */

FA_API fa_result FA_CALL fa_stream_set_matrix(fa_stream* stream, const float* weights, uint32_t in_channels, uint32_t out_channels)
{
    float* m = NULL;
    fa_result r = FA_OK;
    uint32_t i;
    if (!stream) return FA_ERROR_INVALID_ARG;
    if (weights) {
        if (in_channels != stream->decoder.format.channels || out_channels != FA_DEVICE_CHANNELS) return FA_ERROR_INVALID_ARG;
        for (i = 0; i < in_channels * out_channels; i++)
            if (!(weights[i] == weights[i]) || fabsf(weights[i]) > 1000.0f) return FA_ERROR_INVALID_ARG;
        m = (float*)malloc((size_t)in_channels * out_channels * sizeof(float));
        if (!m) return FA_ERROR_OUT_OF_MEMORY;
        memcpy(m, weights, (size_t)in_channels * out_channels * sizeof(float));
    }
    fa_mutex_lock(&stream->api_lock);
    if (is_attached(stream) && !stream->decoder.seekable) {
        r = FA_ERROR_NOT_SEEKABLE;  /* the new chain would have to decode from where playback is */
        free(m);
    } else if (is_attached(stream)) {
        fa_frames pos;
        fa_store32(&stream->flushing, 1);
        fa_device_wait_quiescent(stream->device);
        pos = target_position(stream);         /* after the mixer let go: nothing more is consumed */
        fa_engine_track_stream(stream->engine, stream, 0);  /* workers look at the ring we replace */
        fa_mutex_lock(&stream->decode_lock);
        free(stream->matrix);
        stream->matrix = m;
        r = build_chain_locked(stream, stream->device);
        if (r == FA_OK) {
            /* restart_at_locked only seeks the decoder when needed: force it, the
             * buffers are new. */
            fa_store64(&stream->seek_base, pos);
            fa_store64(&stream->played, 0);
            fa_store32(&stream->eof, 1);
            r = restart_at_locked(stream, pos);
        }
        fa_mutex_unlock(&stream->decode_lock);
        fa_engine_track_stream(stream->engine, stream, 1);
        if (r == FA_OK) unflush(stream);
        /* On failure the stream stays silent (flushing) until the next attach. */
    } else {
        fa_mutex_lock(&stream->decode_lock);
        free(stream->matrix);
        stream->matrix = m;
        fa_mutex_unlock(&stream->decode_lock);
    }
    fa_mutex_unlock(&stream->api_lock);
    return r;
}

/* ---- events ------------------------------------------------------------- */

FA_API fa_result FA_CALL fa_stream_set_event_callback(fa_stream* stream, fa_event_callback callback, void* user)
{
    if (!stream) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->cb_lock);
    stream->callback = callback;
    stream->callback_user = user;
    fa_mutex_unlock(&stream->cb_lock);
    /* Let a callback that already copied the old pointer finish first. */
    if ((int64_t)fa_thread_current_id() != fa_load64(&stream->engine->event_thread_id))
        while (fa_load32(&stream->in_callback)) fa_sleep_ms(1);
    return FA_OK;
}

/* ---- decode-only read --------------------------------------------------- */

/* Decode-only streams with the tempo chain in use: decodes, processes and hands
 * out what the chain made. Caller holds decode_lock. */
static fa_result read_chain_locked(struct fa_stream* s, float* out, fa_frames frames, fa_frames* got)
{
    uint32_t ch = s->decoder.format.channels;
    fa_result r = FA_OK;
    while (*got < frames) {
        double tempo, pitch, rate, src0, src1;
        const float* res;
        int64_t made = 0;
        fa_frames n = 0;
        if (s->rd_count > 0) {
            fa_frames k = s->rd_count < frames - *got ? s->rd_count : frames - *got;
            memcpy(out + (size_t)*got * ch, s->rd_out + (size_t)s->rd_off * ch, (size_t)k * ch * sizeof(float));
            s->rd_off += k;
            s->rd_count -= k;
            *got += k;
            fa_store64(&s->position, (fa_frames)floor(mirror(s, s->rd_src0 + s->rd_slope * (double)s->rd_off) + 0.5));
            continue;
        }
        if (s->tempo_drained) return FA_ERROR_END_OF_STREAM;
        tempo_values(s, s->tempo_frames, &tempo, &pitch, &rate);
        if (!s->rd_in) {
            s->rd_in = (float*)malloc((size_t)FA_TEMPO_CHUNK * ch * sizeof(float));
            if (!s->rd_in) return FA_ERROR_OUT_OF_MEMORY;
        }
        r = source_read(s, s->rd_in, FA_TEMPO_CHUNK, &n);
        if (n > 0) {
            r = fa_tempo_run(s->tempo, s->rd_in, n, tempo, pitch, rate, &res, &made, &src0, &src1);
            if (r != FA_OK) return r;
            if (s->net) net_decoded(s, n, (fa_frames)s->tempo->in_src, fa_load64(&s->position));
        } else if (r == FA_ERROR_BUSY) {
            return FA_OK;                        /* push stream: nothing queued yet */
        } else if (r == FA_OK || r == FA_ERROR_END_OF_STREAM) {
            s->tempo_drained = 1;
            r = fa_tempo_drain(s->tempo, tempo, pitch, rate, &res, &made, &src0, &src1);
            if (r != FA_OK) return r;
        } else {
            return r;
        }
        s->tempo_frames += made;
        s->rd_out = res;
        s->rd_off = 0;
        s->rd_count = made;
        s->rd_src0 = src0;
        s->rd_slope = made > 0 ? (src1 - src0) / (double)made : 0.0;
        if (made == 0) fa_store64(&s->position, (fa_frames)floor(mirror(s, src1) + 0.5));
    }
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_read(fa_stream* stream, void* buffer, fa_frames frames, fa_frames* out_frames_read)
{
    fa_result r;
    fa_frames got = 0;
    double tempo, pitch, rate;
    if (!stream || !buffer || frames < 0 || !out_frames_read) return FA_ERROR_INVALID_ARG;
    *out_frames_read = 0;
    if (stream->device) return FA_ERROR_BUSY; /* attached streams are pulled by the device */
    if (frames == 0) return FA_OK;
    fa_mutex_lock(&stream->decode_lock);
    if ((tempo_values(stream, stream->tempo_frames, &tempo, &pitch, &rate) || stream->tempo) &&
        tempo_chain(stream, stream->decoder.format.channels, mirror(stream, (double)fa_load64(&stream->position)))) {
        r = read_chain_locked(stream, (float*)buffer, frames, &got);
        if (got > 0 && r == FA_ERROR_END_OF_STREAM) r = FA_OK;
        run_dsps(stream, (float*)buffer, got, stream->decoder.format.channels, stream->decoder.format.sample_rate);
        fa_mutex_unlock(&stream->decode_lock);
        *out_frames_read = got;
        return r;
    }
    r = source_read(stream, (float*)buffer, frames, &got);
    fa_fetch_add64(&stream->position, reversed(stream) ? -got : got);
    stream->tempo_frames += got;
    if (stream->net && got > 0) {
        /* Decode-only: what is read now is what the app "hears". */
        fa_frames pos = fa_load64(&stream->position);
        net_decoded(stream, got, pos, pos);
    }
    run_dsps(stream, (float*)buffer, got, stream->decoder.format.channels, stream->decoder.format.sample_rate);
    fa_mutex_unlock(&stream->decode_lock);
    *out_frames_read = got;
    if (r == FA_ERROR_BUSY) r = FA_OK;   /* push stream: nothing queued yet */
    return r;
}

/* ---- mixing (audio thread) ----------------------------------------------- */

static void push_event(fa_device* dev, struct fa_stream* s, fa_event_type type, int64_t value)
{
    fa_audio_post_event(dev, s, type, attached_position(s), value);
}

static void stereo_gains(float vol, float pan, float* gl, float* gr)
{
    *gl = vol * (pan > 0.0f ? 1.0f - pan : 1.0f);
    *gr = vol * (pan < 0.0f ? 1.0f + pan : 1.0f);
}

void fa_bus_gains_at(struct fa_bus* b, fa_device* dev, int64_t off, int from_here, float* gl, float* gr)
{
    int64_t clock = fa_load64(&dev->clock);
    stereo_gains(fa_lane_value_at_frame(&b->lanes[FA_PARAM_VOLUME].v, clock, clock + off, from_here),
                 fa_lane_value_at_frame(&b->lanes[FA_PARAM_PAN].v, clock, clock + off, from_here), gl, gr);
}

/* Shortens a block so it ends where a bus ramp starts or ends: gains are exact
 * at block edges only. */
static int64_t bus_block(struct fa_bus* b, int64_t clock, int64_t now, int64_t blk)
{
    int p;
    for (p = 0; p < FA_MIX_PARAMS; p++) {
        const fa_param_voice* v = &b->lanes[p].v;
        int64_t begin, end;
        if (!v->active) continue;
        begin = v->at > clock ? v->at : clock;
        end = begin + (v->total - v->done);
        if (begin > now && begin - now < blk) blk = begin - now;
        else if (begin <= now && end > now && end - now < blk) blk = end - now;
    }
    return blk;
}

static void mix_region(float* out, const float* in, int64_t n, float l0, float r0, float l1, float r1)
{
    int64_t i;
    if (l0 == l1 && r0 == r1) {
        for (i = 0; i < n; i++) { out[i * 2] += in[i * 2] * l0; out[i * 2 + 1] += in[i * 2 + 1] * r0; }
    } else {
        float dl = (l1 - l0) / (float)n, dr = (r1 - r0) / (float)n;
        for (i = 0; i < n; i++) {
            out[i * 2] += in[i * 2] * (l0 + dl * (float)i);
            out[i * 2 + 1] += in[i * 2 + 1] * (r0 + dr * (float)i);
        }
    }
}

static float db_to_gain(float db) { return db == 0.0f ? 1.0f : fa_db_to_lin(db); }

/* Moves the normalization gain toward its target: down at most 12 dB/s, up at
 * most 6 dB/s, so a corrected estimate glides instead of jumping. */
static float norm_glide(float db, float target, int64_t frames, uint32_t rate)
{
    float step;
    if (db == target) return db;
    step = (float)frames / (float)rate * (target < db ? FA_NORM_GLIDE_DOWN_DB_S : FA_NORM_GLIDE_UP_DB_S);
    if (target < db) return db - step > target ? db - step : target;
    return db + step < target ? db + step : target;
}

/* Marker positions and fade shapes, read once per callback. */
typedef struct mix_markers {
    fa_frames m[FA_MARKER_COUNT];
    fa_frames fade_in_from, fade_in_to, fade_out_from, fade_out_to; /* -1 = no fade */
    fa_shape in_shape, out_shape;
} mix_markers;

static void load_markers(struct fa_stream* s, mix_markers* mk)
{
    int i;
    for (i = 0; i < FA_MARKER_COUNT; i++) mk->m[i] = marker(s, i);
    mk->fade_in_from = mk->fade_in_to = mk->fade_out_from = mk->fade_out_to = -1;
    if (mk->m[FA_MARKER_FADE_IN_END] != FA_FRAMES_UNSET) {
        fa_frames a = mk->m[FA_MARKER_CUE_IN] != FA_FRAMES_UNSET ? mk->m[FA_MARKER_CUE_IN] : 0;
        if (mk->m[FA_MARKER_FADE_IN_END] > a) {
            mk->fade_in_from = a;
            mk->fade_in_to = mk->m[FA_MARKER_FADE_IN_END];
            fa_shape_load(&s->fade_shape[FA_FADE_IN], &mk->in_shape);
        }
    }
    if (mk->m[FA_MARKER_FADE_OUT_START] != FA_FRAMES_UNSET) {
        fa_frames e = song_end(s);
        if (e > mk->m[FA_MARKER_FADE_OUT_START]) {
            mk->fade_out_from = mk->m[FA_MARKER_FADE_OUT_START];
            mk->fade_out_to = e;
            fa_shape_load(&s->fade_shape[FA_FADE_OUT], &mk->out_shape);
        }
    }
}

/* Gain of the marker fades at source position `pos`. */
static float marker_gain(const mix_markers* mk, double pos)
{
    float g = 1.0f;
    if (mk->fade_in_to >= 0 && pos < (double)mk->fade_in_to) {
        double t = (pos - (double)mk->fade_in_from) / (double)(mk->fade_in_to - mk->fade_in_from);
        g *= t <= 0.0 ? fa_shape_eval(&mk->in_shape, 0.0f, 1) : fa_shape_eval(&mk->in_shape, (float)t, 1);
    }
    if (mk->fade_out_to >= 0 && pos > (double)mk->fade_out_from) {
        double t = (pos - (double)mk->fade_out_from) / (double)(mk->fade_out_to - mk->fade_out_from);
        g *= fa_shape_eval(&mk->out_shape, t >= 1.0 ? 0.0f : (float)(1.0 - t), 1);
    }
    return g;
}

/* Source position for the fades: wrapped into the loop, not clamped. */
static double fade_position(struct fa_stream* s, int64_t played)
{
    double lin = linear_position(s, played);
    fa_frames end = end_frame(s);
    if (reversed(s)) {
        fa_frames back = loop_start(s), len = end - back;
        if (loops(s) && len > 0 && lin < (double)back) return (double)end - fmod((double)back - lin, (double)len);
        return lin;
    }
    if (loops(s) && end > 0 && lin >= (double)end) {
        fa_frames back = loop_start(s), len = end - back;
        if (len > 0) return (double)back + fmod(lin - (double)end, (double)len);
    }
    return lin;
}

/* FA_EVENT_MARKER for every marker passed between `played0` and `played1`. */
static void marker_events(struct fa_stream* s, fa_device* dev, const mix_markers* mk, int64_t played0, int64_t played1, int ended)
{
    fa_frames p0 = (fa_frames)(linear_position(s, played0) + 0.5);
    fa_frames p1 = (fa_frames)(linear_position(s, played1) + 0.5);
    fa_frames end = end_frame(s), back = loop_start(s), w0, w1;
    int looping = loops(s) && end > 0 && end > back;
    int i;
    if ((p1 <= p0 && !ended) || reversed(s)) return;
    w0 = wrap_position(s, p0);
    w1 = wrap_position(s, p1);
    for (i = 0; i < FA_MARKER_COUNT; i++) {
        fa_frames m = mk->m[i];
        int hit;
        if (m == FA_FRAMES_UNSET) continue;
        /* The loop end (CUE_OUT) is passed each time playback wraps. */
        if (looping && p1 - p0 >= end - back) hit = m >= back && m <= end;
        else if (looping && w1 < w0) hit = (m >= w0 && m <= end) || (m >= back && m < w1);
        else hit = m >= w0 && (m < w1 || (ended && m == w1));
        if (hit) fa_audio_post_event(dev, s, FA_EVENT_MARKER, m, i);
    }
    if (looping && p1 >= end) {
        /* Every jump between p0 and p1: at end, end + len, ... in linear frames. */
        fa_frames len = end - back, k0 = p0 < end ? 0 : (p0 - end) / len + 1, k;
        for (k = k0; k <= (p1 - end) / len; k++) fa_audio_post_event(dev, s, FA_EVENT_LOOPED, end, back);
    }
}

/* MIDI streams: FA_EVENT_MIDI_TEXT (value = the text's index) for every text event
 * whose frame playback passed between `played0` and `played1`. The frames are
 * fixed and sorted, so a binary search finds the first one; wrapping into a loop
 * gives two ranges. Best effort, like markers. */
static void post_texts(struct fa_stream* s, fa_device* dev, const fa_frames* at, uint32_t n, fa_frames from, fa_frames to)
{
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (at[mid] < from) lo = mid + 1; else hi = mid;
    }
    for (; lo < n && at[lo] < to; lo++) fa_audio_post_event(dev, s, FA_EVENT_MIDI_TEXT, at[lo], (int64_t)lo);
}

static void post_midi_texts(struct fa_stream* s, fa_device* dev, fa_frames from, fa_frames to)
{
    uint32_t n;
    const fa_frames* at = fa_midi_text_frames(s->midi, &n);
    if (n > 0) post_texts(s, dev, at, n, from, to);
}

/* Tracker modules: FA_EVENT_TRACKER_ROW (value = order << 16 | row) for the rows
 * starting between `from` and `to` that FA_TRACKER_ROW_EVENTS asks for. */
static void post_tracker_rows(struct fa_stream* s, fa_device* dev, fa_frames from, fa_frames to)
{
    fa_frames at[32];
    int64_t v[32];
    uint32_t n, i;
    do {
        n = fa_tracker_rows_in(s->tracker, from, to, at, v, 32);
        for (i = 0; i < n; i++) fa_audio_post_event(dev, s, FA_EVENT_TRACKER_ROW, at[i], v[i]);
        if (n > 0) from = at[n - 1] + 1;
    } while (n == 32);
}

/* Events tied to fixed source frames (MIDI texts, tracker rows): `post` for each
 * range of source frames playback passed between `played0` and `played1`;
 * wrapping into a loop gives two ranges. */
static void frame_events(struct fa_stream* s, fa_device* dev, int64_t played0, int64_t played1, int ended,
                         void (*post)(struct fa_stream*, fa_device*, fa_frames, fa_frames))
{
    fa_frames p0, p1, end, back, w0, w1;
    p0 = (fa_frames)(linear_position(s, played0) + 0.5);
    p1 = (fa_frames)(linear_position(s, played1) + 0.5);
    if (p1 <= p0 && !ended) return;
    end = end_frame(s);
    back = loop_start(s);
    w0 = wrap_position(s, p0);
    w1 = wrap_position(s, p1);
    if (loops(s) && end > back && p1 - p0 >= end - back) {
        post(s, dev, back, end);
    } else if (loops(s) && end > back && w1 < w0) {
        post(s, dev, w0, end);
        post(s, dev, back, w1);
    } else {
        post(s, dev, w0, ended ? w1 + 1 : w1);
    }
}

/* FA_EVENT_POSITION for each armed frame heard between `played0` and `played1`, in
 * the order heard; raising one frees its slot. Forward, the frames heard are
 * [w0, w1) (with w1 itself at the end); a wrap into the loop gives [w0, end] and
 * [back, w1), counting the loop end as passed like markers do. Reversed, they are
 * [w1, w0), or [back, w0) and [w1, end] where the loop wraps at its start; the end
 * also counts when playback starts there. */
static void position_events(struct fa_stream* s, fa_device* dev, int64_t played0, int64_t played1, int ended)
{
    fa_frames p0, p1, end, back, len, w0, w1, at[FA_POSITION_EVENT_SLOTS];
    int64_t dist[FA_POSITION_EVENT_SLOTS];
    int rev, looping, whole, i, j, n = 0;
    if (fa_load32(&s->pos_armed) <= 0) return;
    rev = reversed(s);
    p0 = (fa_frames)(linear_position(s, played0) + 0.5);
    p1 = (fa_frames)(linear_position(s, played1) + 0.5);
    if (rev ? p1 >= p0 : (p1 <= p0 && !ended)) return;
    end = end_frame(s);
    back = loop_start(s);
    len = end - back;
    looping = loops(s) && end > 0 && len > 0;
    whole = looping && (rev ? p0 - p1 : p1 - p0) >= len;   /* a whole turn or more: every frame */
    w0 = wrap_position(s, p0);
    w1 = wrap_position(s, p1);
    for (i = 0; i < FA_POSITION_EVENT_SLOTS; i++) {
        fa_frames m = fa_load64(&s->pos_events[i]);
        int64_t d;
        int hit;
        if (m == FA_FRAMES_UNSET) continue;
        if (rev) {
            /* The end itself is reached where playback starts from it or wraps to it. */
            if (whole) hit = m >= back && m <= end;
            else if (looping && w1 > w0) hit = (m >= back && m < w0) || (m >= w1 && m <= end);
            else hit = m >= w1 && (m < w0 || (m == w0 && w0 == end));
            d = m < w0 ? w0 - 1 - m : m == w0 ? -1 : (w0 - back) + (end - m);
        } else {
            if (whole) hit = m >= back && m <= end;
            else if (looping && w1 < w0) hit = (m >= w0 && m <= end) || (m >= back && m < w1);
            else hit = m >= w0 && (m < w1 || (ended && m == w1));
            d = m >= w0 ? m - w0 : (end + 1 - w0) + (m - back);
        }
        if (!hit || !fa_cas64(&s->pos_events[i], m, FA_FRAMES_UNSET)) continue;
        fa_fetch_add32(&s->pos_armed, -1);
        for (j = n++; j > 0 && dist[j - 1] > d; j--) { at[j] = at[j - 1]; dist[j] = dist[j - 1]; }
        at[j] = m;
        dist[j] = d;
    }
    for (i = 0; i < n; i++) fa_audio_post_event(dev, s, FA_EVENT_POSITION, at[i], 0);
}

static void heard_events(struct fa_stream* s, fa_device* dev, int64_t played0, int64_t played1, int ended)
{
    heard_ramps_due(s, dev, played1, ended);
    position_events(s, dev, played0, played1, ended);
    if (reversed(s)) return;   /* texts and rows are raised playing forward only */
    if (s->midi) frame_events(s, dev, played0, played1, ended, post_midi_texts);
    if (s->tracker && fa_tracker_row_events_on(s->tracker)) frame_events(s, dev, played0, played1, ended, post_tracker_rows);
}

/* Ends the stream from the audio thread (its data ran out, or a crossfade-out
 * finished). A crossfade still running ends with it: it raises its RAMP_DONE (so
 * whoever waits for the crossfade is not left waiting), and a later play starts
 * at full crossfade gain. */
static void end_stream(struct fa_stream* s, fa_device* dev)
{
    fa_param_voice* v = &s->lanes[FA_LANE_FADE].v;
    if (!fa_cas32(&s->state, FA_STATE_PLAYING, FA_STATE_ENDED)) return;
    heard_ramps_due(s, dev, fa_load64(&s->played), 1);
    if (v->active) {
        if (v->notify) push_event(dev, s, FA_EVENT_RAMP_DONE, v->tag);
        v->active = 0;
    }
    v->value = 1.0f;
    fa_store_float(&s->lanes[FA_LANE_FADE].current, 1.0f);
    push_event(dev, s, FA_EVENT_ENDED, 0);
}

/* A lane's ramp ran its course. Returns 1 when that ends the stream. */
static int finish_lane(struct fa_stream* s, fa_device* dev, fa_param_voice* v)
{
    v->active = 0;
    v->value = v->target;
    if (v->notify) push_event(dev, s, FA_EVENT_RAMP_DONE, v->tag);
    if (v->flags & FA_LANE_END_STREAM) {
        v->value = 1.0f;  /* a later play starts at full crossfade gain */
        return 1;
    }
    return 0;
}

/* Device frames from `played` until the stream reaches its CUE_OUT, or -1 when no
 * cue-out applies (unset, or a looping stream). The mixer stops there even when
 * audio past it was buffered already (a cue-out moved while playing). */
static int64_t frames_to_cue_out(struct fa_stream* s, int64_t played)
{
    fa_frames out = marker(s, FA_MARKER_CUE_OUT);
    if (out == FA_FRAMES_UNSET || loops(s) || reversed(s)) return -1;
    return frames_until(s, played, (double)out);
}

/* Frames from now until `s` reaches its mix point, or -1 if not within `frames`. */
static int64_t frames_to_mix_point(struct fa_stream* s, int64_t frames)
{
    /* Reversed, it hands over where it ends (CUE_IN or the start), with no crossfade. */
    fa_frames mix = reversed(s) ? FA_FRAMES_UNSET : marker(s, FA_MARKER_MIX_POINT);
    int64_t xf_ns = fa_load64(&s->xfade_ns);
    double sr = (double)s->decoder.format.sample_rate;
    /* A loop has no end to count back from: it hands over at its MIX_POINT only. */
    if (mix == FA_FRAMES_UNSET && xf_ns > 0 && !loops(s) && !reversed(s)) {
        fa_frames end = end_frame(s);
        if (end >= 0) {
            mix = end - (fa_frames)((double)xf_ns * sr / 1e9 + 0.5);
            if (mix < 0) mix = 0;
        }
    }
    if (mix != FA_FRAMES_UNSET) {
        int64_t played = fa_load64(&s->played), k;
        double dist = (double)mix - fade_position(s, played);
        if (dist < 0.0 && loops(s)) {
            fa_frames end = end_frame(s), back = loop_start(s);
            if (end > back) dist += (double)(end - back);
        }
        k = dist <= 0.0 ? 0 : frames_until(s, played, linear_position(s, played) + dist);
        if (k < frames) return k;
    }
    /* At the end: gapless (or the fallback when the end was not known in advance). */
    if (!loops(s)) {
        int64_t avail = fa_ring_readable(&s->ring), left = -1;
        int64_t cut = frames_to_cue_out(s, fa_load64(&s->played));
        if (fa_load32(&s->eof)) left = avail;
        if (cut >= 0 && cut <= avail && (left < 0 || cut < left)) left = cut;
        if (left >= 0 && left < frames) return left;
    }
    return -1;
}

/* Applies a command waiting on the fade lane now, so the queue's ramp installed
 * next is not overwritten by it, and raises RAMP_DONE for a ramp it replaces. */
static void settle_fade_lane(struct fa_stream* s, fa_device* dev, int audible)
{
    fa_param_voice* v = &s->lanes[FA_LANE_FADE].v;
    int64_t tag = 0;
    if (fa_lane_take_after(&s->lanes[FA_LANE_FADE], dev->xf_busy ? fa_load64(&dev->clock) : INT64_MIN, audible,
                           dev->rate, &tag))
        push_event(dev, s, FA_EVENT_RAMP_DONE, tag);
    if (v->active && v->notify) push_event(dev, s, FA_EVENT_RAMP_DONE, v->tag);
    v->active = 0;
}

int fa_stream_check_queue(struct fa_stream* s, fa_device* dev, uint32_t frames)
{
    struct fa_stream* next = (struct fa_stream*)fa_load_ptr(&s->queued);
    int64_t k, at, xf_ns, clock = fa_load64(&dev->clock), base = 0;
    int32_t st = fa_load32(&s->state);
    if (!next || fa_load32(&s->flushing) || !s->dev_rate) return 0;
    if (st == FA_STATE_SCHEDULED) {
        /* Starts within this callback (a queue transition found earlier in this
         * callback): its own mix point may fall in it too. */
        int64_t start = fa_load64(&s->start_frame);
        if (start == FA_START_QUEUED || start >= clock + (int64_t)frames) return 0;
        base = start > clock ? start - clock : 0;
    } else if (st != FA_STATE_PLAYING) {
        return 0;
    }
    k = frames_to_mix_point(s, (int64_t)frames - base);
    if (k < 0) return 0;
    if (!fa_cas_ptr(&s->queued, next, NULL)) return 0;   /* changed by an API thread meanwhile */
    if (fa_load32(&next->state) != FA_STATE_SCHEDULED || fa_load64(&next->start_frame) != FA_START_QUEUED) return 0;
    at = clock + base + k;
    xf_ns = fa_load64(&s->xfade_ns);
    if (xf_ns > 0) {
        int64_t xf = (int64_t)((double)xf_ns * dev->rate / 1e9 + 0.5);
        fa_shape shape;
        fa_shape_load(&s->xfade_shape, &shape);
        if (marker(s, FA_MARKER_FADE_OUT_START) == FA_FRAMES_UNSET) {
            settle_fade_lane(s, dev, st == FA_STATE_PLAYING && s->was_audible);
            fa_lane_start_ramp(&s->lanes[FA_LANE_FADE], s->lanes[FA_LANE_FADE].v.value, 0.0f, xf, at, &shape,
                               FA_LANE_MIRROR | FA_LANE_END_STREAM);
        }
        if (marker(next, FA_MARKER_FADE_IN_END) == FA_FRAMES_UNSET) {
            settle_fade_lane(next, dev, 0);
            fa_lane_start_ramp(&next->lanes[FA_LANE_FADE], 0.0f, 1.0f, xf, at, &shape, 0);
        }
    }
    fa_store64(&next->start_frame, at);
    return 1;
}

/* ---- presentation clock (audio thread) -------------------------------------------- */

/* Presentation time of the frame `played` frames into the ring, in *out_ns the
 * nanoseconds each further frame adds, and in *out_next the next played count
 * where that mapping stops being linear (a loop wrap, a tempo change, or a jump in
 * a push stream's timestamps); INT64_MAX if none. */
static int64_t pts_at_played(struct fa_stream* s, int64_t played, int64_t* out_next, double* out_ns)
{
    double sr = (double)s->decoder.format.sample_rate, dr = (double)s->dev_rate;
    double pos, lin, slope;
    fa_frames end;
    if (s->push) {
        /* The app's timestamp at the last timeline restart, plus the source audio
         * heard since then (tempo and rate included, from the position map). */
        fa_ptsmap* m = ptsmap_of(s);
        double base = (double)fa_load64(&s->seek_base);
        int64_t anchor = fa_ptsmap_pts(m, played, 0.0, out_next), next;
        if (!m->cur_src_ok) {
            m->cur_src = fa_tmap_pos(tmap_of(s), m->cur_idx, base, sr / dr, NULL, NULL);
            m->cur_src_ok = 1;
        }
        lin = fa_tmap_pos(tmap_of(s), played, base, sr / dr, &slope, &next);
        if (next < *out_next) *out_next = next;
        *out_ns = slope * 1e9 / sr;
        return anchor + (int64_t)((lin - m->cur_src) * 1e9 / sr + 0.5);
    }
    lin = mirror(s, fa_tmap_pos(tmap_of(s), played, (double)fa_load64(&s->seek_base), sr / dr, &slope, out_next));
    *out_ns = (reversed(s) ? -slope : slope) * 1e9 / sr;
    pos = fade_position(s, played);
    end = end_frame(s);
    if (reversed(s)) {
        fa_frames back = loop_start(s);
        if (loops(s) && end > back && pos > (double)back) {   /* device frames until the loop wraps */
            int64_t wrap = played + frames_until(s, played, lin - (pos - (double)back));
            if (wrap > played && wrap < *out_next) *out_next = wrap;
        } else if (pos < (double)back) {
            pos = (double)back;                            /* the resampler's tail after the start */
        }
    } else if (end > 0 && loops(s)) {
        if ((double)end > pos) {                           /* device frames until the loop wraps */
            int64_t wrap = played + frames_until(s, played, lin + ((double)end - pos));
            if (wrap > played && wrap < *out_next) *out_next = wrap;
        }
    } else if (end >= 0 && pos > (double)end) {
        pos = (double)end;                                 /* the resampler's tail after the end */
    }
    return (int64_t)(pos * 1e9 / sr + 0.5);
}

/* Notes that device frames [dev0, dev0 + n) played the stream from `played0`. */
static void record_clock(struct fa_stream* s, int64_t dev0, int64_t played0, int64_t n)
{
    double ns_per_frame;
    int64_t p = played0, stop = played0 + n;
    fa_frames end = end_frame(s);
    int rev = reversed(s);
    int64_t end_pts = (!s->push && end >= 0 && !loops(s) && !rev)
        ? (int64_t)((double)end * 1e9 / (double)s->decoder.format.sample_rate + 0.5) : INT64_MAX;
    int64_t start_pts = rev && !loops(s)   /* reversed, the clock stops at the start */
        ? (int64_t)((double)loop_start(s) * 1e9 / (double)s->decoder.format.sample_rate + 0.5) : INT64_MIN;
    while (p < stop) {
        int64_t next, seg, pts0, pts1;
        pts0 = pts_at_played(s, p, &next, &ns_per_frame);
        seg = next > p && next < stop ? next : stop;
        pts1 = pts0 + (int64_t)floor((double)(seg - p) * ns_per_frame + 0.5);
        if (pts1 > end_pts) pts1 = end_pts > pts0 ? end_pts : pts0;
        if (pts1 < start_pts) pts1 = start_pts < pts0 ? start_pts : pts0;
        fa_clock_record(&s->clock, dev0 + (p - played0), seg - p, pts0, pts1);
        p = seg;
    }
}

/* A buffering push or web stream plays on once this much is decoded ahead. */
static int64_t prebuffer_frames(struct fa_stream* s)
{
    int64_t want = (int64_t)s->dev_rate * FA_PREFILL_MS / 1000;
    return want < s->ring.capacity / 2 ? want : s->ring.capacity / 2;
}

/* Web streams: FA_EVENT_METADATA when playback reaches the armed title. */
static void net_title_heard(struct fa_stream* s, fa_device* dev)
{
    int64_t at = fa_load64(&s->meta_frame), seq, was;
    if (at == INT64_MAX || attached_position(s) < at) return;
    seq = fa_load64(&s->meta_seq_armed);
    fa_cas64(&s->meta_frame, at, INT64_MAX);
    was = fa_load64(&s->meta_announced);
    if (seq > was && fa_cas64(&s->meta_announced, was, seq)) push_event(dev, s, FA_EVENT_METADATA, seq);
}

/* Whether the gains stay the same over the `n` frames from `played`: no lane or
 * bus ramp (none due either), no mute or normalization glide, and the marker fades
 * out of reach. A looping stream must not wrap in them either, as blocks end at the
 * loop end (where the fades jump, and where the equalizer's blocks restart). */
static int steady_gain(struct fa_stream* s, const mix_markers* mk, struct fa_bus* bus, int64_t played, int64_t n,
                       int looping, fa_frames loop_end)
{
    double pos0, lin0, pos1;
    int p;
    for (p = 0; p < FA_STREAM_LANES; p++) if (s->lanes[p].v.active) return 0;
    if (bus) for (p = 0; p < FA_MIX_PARAMS; p++) if (bus->lanes[p].v.active) return 0;
    if (!looping && mk->fade_in_to < 0 && mk->fade_out_to < 0) return 1;
    pos0 = fade_position(s, played);
    lin0 = linear_position(s, played);
    if (looping) {
        int64_t to_end = frames_until(s, played, lin0 + ((double)loop_end - pos0));
        if (to_end <= n) return 0;
    }
    /* Positions only grow until the wrap; a frame of margin covers the rounding of
     * the block-by-block sums. */
    pos1 = pos0 + (linear_position(s, played + n) - lin0);
    if (mk->fade_in_to >= 0 && pos0 < (double)mk->fade_in_to) return 0;
    if (mk->fade_out_to >= 0 && pos1 + 1.0 > (double)mk->fade_out_from) return 0;
    return 1;
}

/* stream_mix's loop for steady gains (steady_gain): the same samples, mixed in one
 * pass, with the ring, the play count and the tempo map moved once. The equalizer
 * still runs in blocks of FA_RAMP_BLOCK, as its glides and state go by block (eq is
 * NULL when it is off). */
static void mix_steady(struct fa_stream* s, fa_device* dev, float* out, int64_t now, int64_t off, int64_t played, int64_t n,
                       float norm_lin, float mute, const mix_markers* mk, struct fa_bus* bus, fa_stream_eq* eq, fa_tap* tap)
{
    float g, l, r, bl = 1.0f, br = 1.0f;
    const float *p1, *p2;
    int64_t n1, n2, done;
    g = s->lanes[FA_PARAM_VOLUME].v.value * s->lanes[FA_LANE_FADE].v.value * marker_gain(mk, fade_position(s, played));
    g *= 1.0f - mute;
    stereo_gains(g * norm_lin, s->lanes[FA_PARAM_PAN].v.value, &l, &r);
    if (bus) {
        fa_bus_gains_at(bus, dev, off, 1, &bl, &br);
        l *= bl; r *= br;
    }
    fa_ring_regions(&s->ring, n, &p1, &n1, &p2, &n2);
    if (eq) {
        for (done = 0; done < n;) {
            float eqb[FA_RAMP_BLOCK * FA_DEVICE_CHANNELS];
            int64_t blk = n - done < FA_RAMP_BLOCK ? n - done : FA_RAMP_BLOCK, k = 0;
            if (done < n1) {
                k = n1 - done < blk ? n1 - done : blk;
                memcpy(eqb, p1 + done * FA_DEVICE_CHANNELS, (size_t)k * FA_DEVICE_CHANNELS * sizeof(float));
            }
            if (k < blk)
                memcpy(eqb + k * FA_DEVICE_CHANNELS, p2 + (done + k - n1) * FA_DEVICE_CHANNELS,
                       (size_t)(blk - k) * FA_DEVICE_CHANNELS * sizeof(float));
            fa_eq_process(&eq->st, eqb, blk);
            if (tap) fa_tap_write(tap, now + done, eqb, blk);
            mix_region(out + done * 2, eqb, blk, l, r, l, r);
            done += blk;
        }
    } else {
        if (tap) {
            fa_tap_write(tap, now, p1, n1);
            if (n2 > 0) fa_tap_write(tap, now + n1, p2, n2);
        }
        mix_region(out, p1, n1, l, r, l, r);
        if (n2 > 0) mix_region(out + n1 * 2, p2, n2, l, r, l, r);
    }
    fa_ring_advance_read(&s->ring, n);
    fa_fetch_add64(&s->played, n);
    fa_tmap_release(tmap_of(s), played + n);
}

/* Mixes `s` into `out`; *mixed_off and *mixed_n say which frames of the callback
 * carried its audio. */
static void stream_mix(struct fa_stream* s, fa_device* dev, float* out, uint32_t frames, int64_t* mixed_off,
                       int64_t* mixed_n)
{
    int64_t avail, n, done = 0, clock, off = 0, played0, hold;
    int p, playing, audible, ended = 0, at_cue_out = 0, looping;
    fa_frames loop_end, loop_back;
    int32_t state;
    float norm_target, norm_lin, mute_target, mute0, mute1;
    mix_markers mk;
    struct fa_bus* bus;
    int eq_on;
    fa_stream_eq* eq;
    fa_tap* tap;
    *mixed_off = 0;
    *mixed_n = 0;
    if (fa_load32(&s->flushing)) return;
    clock = fa_load64(&dev->clock);

    /* fa_crossfade is posting (fa_device.xf_lock): starts and commands due now wait
     * for the next callback, which sees all of them. */
    hold = dev->xf_busy ? clock : INT64_MIN;
    state = fa_load32(&s->state);
    if (state == FA_STATE_SCHEDULED) {
        int64_t start = fa_load64(&s->start_frame);
        if (start != FA_START_QUEUED && start < clock + (int64_t)frames && start > hold &&
            fa_cas32(&s->state, FA_STATE_SCHEDULED, FA_STATE_PLAYING)) {
            off = start > clock ? start - clock : 0;
            state = FA_STATE_PLAYING;
            push_event(dev, s, FA_EVENT_STARTED, 0);
        }
    }
    if (state == FA_STATE_BUFFERING &&
        (fa_load32(&s->eof) || fa_ring_readable(&s->ring) >= prebuffer_frames(s)) &&
        fa_cas32(&s->state, FA_STATE_BUFFERING, FA_STATE_PLAYING)) {
        state = FA_STATE_PLAYING;
        if (s->stalled) { s->stalled = 0; push_event(dev, s, FA_EVENT_RESUMED, 0); }
    }
    playing = state == FA_STATE_PLAYING;
    audible = playing && s->was_audible;
    for (p = 0; p < FA_STREAM_LANES; p++) {
        int64_t tag = 0;
        fa_param_voice* v = &s->lanes[p].v;
        if (fa_lane_take_after(&s->lanes[p], hold, audible, dev->rate, &tag)) push_event(dev, s, FA_EVENT_RAMP_DONE, tag);
        /* A zero-length ramp that has started is a jump: the first sample already uses the target. */
        if (playing && v->active && v->done >= v->total && (v->at < 0 || v->at <= clock + off))
            if (finish_lane(s, dev, v)) { end_stream(s, dev); playing = 0; }
    }
    if (!playing) {
        for (p = 0; p < FA_STREAM_LANES; p++) fa_store_float(&s->lanes[p].current, s->lanes[p].v.value);
        s->was_audible = 0;
        return;
    }
    norm_target = fa_stream_norm_target_db(s);
    if (!s->was_audible) s->norm_db = norm_target; /* nothing heard yet: start at the right gain */
    s->was_audible = 1;
    norm_lin = db_to_gain(s->norm_db);
    load_markers(s, &mk);
    played0 = fa_load64(&s->played);
    bus = (struct fa_bus*)fa_load_ptr(&s->bus);
    tap = (fa_tap*)fa_load_ptr(&s->tap);

    avail = fa_ring_readable(&s->ring);
    n = avail < (int64_t)frames - off ? avail : (int64_t)frames - off;
    {
        int64_t cut = frames_to_cue_out(s, played0);
        if (cut >= 0 && cut <= n) { n = cut; at_cue_out = 1; }
    }
    loop_end = end_frame(s);
    loop_back = loop_start(s);
    looping = loops(s) && loop_end > 0 && loop_end > loop_back;
    mute_target = (float)fa_load32(&s->muted);
    /* Glides only while audio flows: a stream (re)starting starts at the setting, so
     * one muted before it plays never sounds. */
    mute0 = s->mute_next == clock + off ? s->mute_level : mute_target;
    /* The filters' memory belongs to the audio just before: clear it when the
     * timeline restarted, or when this mix doesn't continue the last one. */
    eq = (fa_stream_eq*)fa_load_ptr(&s->eq);
    eq_on = eq && fa_eq_begin(&eq->st, &eq->shared, dev->rate,
                              (fa_load32(&s->eq_restart) && fa_exchange32(&s->eq_restart, 0)) | (s->mute_next != clock + off));

    if (n > 0 && !fa_load32(&dev->mix_blockwise) && mute0 == mute_target && s->norm_db == norm_target &&
        steady_gain(s, &mk, bus, played0, n, looping, loop_end)) {
        mix_steady(s, dev, out + off * 2, clock + off, off, played0, n, norm_lin, mute0, &mk, bus, eq_on ? eq : NULL, tap);
        done = n;
    }
    while (done < n && !ended) {
        /* Blocks of FA_RAMP_BLOCK frames: gains are exact at block edges and
         * interpolated linearly inside, so ramps and changes are click-free. Blocks
         * also end exactly where a ramp starts or ends. */
        int64_t blk = n - done < FA_RAMP_BLOCK ? n - done : FA_RAMP_BLOCK;
        int64_t now = clock + off + done, pl = played0 + done;
        float v0[FA_STREAM_LANES], v1[FA_STREAM_LANES], g0, g1, l0, r0, l1, r1;
        float bl0 = 1.0f, br0 = 1.0f, bl1 = 1.0f, br1 = 1.0f;
        double pos0, pos1, lin0;
        const float *p1, *p2;
        int64_t n1, n2;
        for (p = 0; p < FA_STREAM_LANES; p++) {
            /* A zero-length ramp due here is a jump: this block already uses its target. */
            fa_param_voice* v = &s->lanes[p].v;
            if (v->active && v->at <= now && v->done >= v->total) {
                v->at = -1;
                if (finish_lane(s, dev, v)) ended = 1;
            }
        }
        if (ended) break;
        for (p = 0; p < FA_STREAM_LANES; p++) {
            fa_param_voice* v = &s->lanes[p].v;
            if (!v->active) continue;
            if (v->at > now) { if (v->at - now < blk) blk = v->at - now; }
            else if (v->total - v->done > 0 && v->total - v->done < blk) blk = v->total - v->done;
        }
        if (bus) blk = bus_block(bus, clock, now, blk);
        pos0 = fade_position(s, pl);
        lin0 = linear_position(s, pl);
        if (looping) {
            /* Blocks end at the loop end, so the fades are not interpolated toward
             * the next turn's level before the wrap. */
            int64_t to_end = frames_until(s, pl, lin0 + ((double)loop_end - pos0));
            if (to_end > 0 && to_end < blk) blk = to_end;
        }
        pos1 = pos0 + (linear_position(s, pl + blk) - lin0);
        for (p = 0; p < FA_STREAM_LANES; p++) {
            fa_param_voice* v = &s->lanes[p].v;
            v0[p] = v->value;
            v1[p] = (v->active && v->at <= now) ? fa_lane_ramp_value(v, v->done + blk) : v->value;
        }
        g0 = v0[FA_PARAM_VOLUME] * v0[FA_LANE_FADE] * marker_gain(&mk, pos0);
        g1 = v1[FA_PARAM_VOLUME] * v1[FA_LANE_FADE] * marker_gain(&mk, pos1);
        mute1 = fa_glide(mute0, mute_target, blk, dev->rate);
        g0 *= 1.0f - mute0;
        g1 *= 1.0f - mute1;
        mute0 = mute1;
        {
            float nd1 = norm_glide(s->norm_db, norm_target, blk, dev->rate);
            float nl1 = nd1 == s->norm_db ? norm_lin : db_to_gain(nd1);
            stereo_gains(g0 * norm_lin, v0[FA_PARAM_PAN], &l0, &r0);
            stereo_gains(g1 * nl1, v1[FA_PARAM_PAN], &l1, &r1);
            s->norm_db = nd1;
            norm_lin = nl1;
        }
        if (bus) {
            fa_bus_gains_at(bus, dev, off + done, 1, &bl0, &br0);
            fa_bus_gains_at(bus, dev, off + done + blk, 0, &bl1, &br1);
            l0 *= bl0; r0 *= br0; l1 *= bl1; r1 *= br1;
        }
        fa_ring_regions(&s->ring, blk, &p1, &n1, &p2, &n2);
        if (eq_on) {
            /* The equalizer works on a copy: the ring is read-only here. */
            float eqb[FA_RAMP_BLOCK * FA_DEVICE_CHANNELS];
            memcpy(eqb, p1, (size_t)n1 * FA_DEVICE_CHANNELS * sizeof(float));
            if (n2 > 0) memcpy(eqb + n1 * FA_DEVICE_CHANNELS, p2, (size_t)n2 * FA_DEVICE_CHANNELS * sizeof(float));
            fa_eq_process(&eq->st, eqb, blk);
            if (tap) fa_tap_write(tap, now, eqb, blk);
            mix_region(out + (off + done) * 2, eqb, blk, l0, r0, l1, r1);
        } else {
            if (tap) {
                fa_tap_write(tap, now, p1, n1);
                if (n2 > 0) fa_tap_write(tap, now + n1, p2, n2);
            }
            float* o = out + (off + done) * 2;
            float lm = l0 + (l1 - l0) * (float)n1 / (float)blk, rm = r0 + (r1 - r0) * (float)n1 / (float)blk;
            mix_region(o, p1, n1, l0, r0, lm, rm);
            if (n2 > 0) mix_region(o + n1 * 2, p2, n2, lm, rm, l1, r1);
        }
        fa_ring_advance_read(&s->ring, blk);
        fa_fetch_add64(&s->played, blk);
        fa_tmap_release(tmap_of(s), pl + blk);
        done += blk;

        for (p = 0; p < FA_STREAM_LANES; p++) {
            fa_param_voice* v = &s->lanes[p].v;
            v->value = v1[p];
            if (!v->active || v->at > now) continue;
            v->at = -1;
            v->done += blk;
            if (v->done >= v->total && finish_lane(s, dev, v)) ended = 1;
        }
    }
    for (p = 0; p < FA_STREAM_LANES; p++) fa_store_float(&s->lanes[p].current, s->lanes[p].v.value);
    s->mute_level = mute0;
    s->mute_next = clock + off + done;
    *mixed_off = off;
    *mixed_n = done;
    if (done > 0) {
        record_clock(s, clock + off, played0, done);
        fa_fetch_add64(&s->played_audible, done);
    }
    if (s->net && done > 0) net_title_heard(s, dev);

    if (ended) {
        marker_events(s, dev, &mk, played0, played0 + done, 0);
        heard_events(s, dev, played0, played0 + done, 0);
        end_stream(s, dev);
        return;
    }
    if (at_cue_out) {
        marker_events(s, dev, &mk, played0, played0 + done, 1);
        heard_events(s, dev, played0, played0 + done, 1);
        end_stream(s, dev);
        return;
    }
    if (n < (int64_t)frames - off) {
        if (fa_load32(&s->eof) && fa_ring_readable(&s->ring) == 0) {
            marker_events(s, dev, &mk, played0, played0 + done, 1);
            heard_events(s, dev, played0, played0 + done, 1);
            end_stream(s, dev);
            return;
        }
        fa_fetch_add32(&s->underruns, 1);
        /* A push or web stream that ran dry waits for more: the app's next data, or
         * the network (whose download buffer only hands data on once prebuffer_ms are
         * in, so the ring then refills at once). */
        if ((s->push || s->net) && fa_cas32(&s->state, FA_STATE_PLAYING, FA_STATE_BUFFERING) &&
            fa_load64(s->push ? &s->played_audible : &s->played) > 0 && !s->stalled) {
            s->stalled = 1;
            push_event(dev, s, FA_EVENT_STALLED, 0);
        }
    }
    marker_events(s, dev, &mk, played0, played0 + done, 0);
    heard_events(s, dev, played0, played0 + done, 0);
}

void fa_stream_mix(struct fa_stream* s, fa_device* dev, float* out, uint32_t frames)
{
    fa_enc_list* encs = (fa_enc_list*)fa_load_ptr(&s->encoders);
    int64_t off, n, i;
    uint32_t k;
    if (!encs || (int64_t)frames > encs->scratch_cap) {
        stream_mix(s, dev, out, frames, &off, &n);
        if (encs)
            for (k = 0; k < encs->count; k++) fa_encode_drop(encs->items[k], n);
        return;
    }
    /* Recorded: the stream's own mix first, then added to the output. */
    memset(encs->scratch, 0, (size_t)frames * FA_DEVICE_CHANNELS * sizeof(float));
    stream_mix(s, dev, encs->scratch, frames, &off, &n);
    if (n <= 0) return;
    for (i = off * FA_DEVICE_CHANNELS; i < (off + n) * FA_DEVICE_CHANNELS; i++) out[i] += encs->scratch[i];
    for (k = 0; k < encs->count; k++) fa_encode_push(encs->items[k], encs->scratch + off * FA_DEVICE_CHANNELS, n, dev->rate);
}

/* ---- user DSP ---------------------------------------------------------------- */

/* Publishes `next` and frees the old list once no decoder can still be running it. */
static void replace_dsps_locked(struct fa_stream* s, fa_dsp_list* next)
{
    fa_dsp_list* old = (fa_dsp_list*)fa_exchange_ptr(&s->dsps, next);
    fa_mutex_lock(&s->decode_lock);   /* waits for a worker or reader in the old list */
    fa_mutex_unlock(&s->decode_lock);
    free(old);
}

static fa_dsp_list* dsp_list_alloc(uint32_t count)
{
    fa_dsp_list* l = (fa_dsp_list*)malloc(sizeof(fa_dsp_list) + (count ? count - 1 : 0) * sizeof(fa_dsp_entry));
    if (l) l->count = count;
    return l;
}

FA_API fa_result FA_CALL fa_stream_add_dsp(fa_stream* stream, fa_dsp_callback callback, void* user, int32_t priority, uint32_t* out_dsp_id)
{
    fa_dsp_list *old, *next;
    uint32_t n, i, at;
    if (!stream || !callback) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->api_lock);
    old = (fa_dsp_list*)fa_load_ptr(&stream->dsps);
    n = old ? old->count : 0;
    next = dsp_list_alloc(n + 1);
    if (!next) { fa_mutex_unlock(&stream->api_lock); return FA_ERROR_OUT_OF_MEMORY; }
    /* Higher priority first; a new one goes after those of the same priority. */
    for (at = 0; at < n && old->items[at].priority >= priority; at++) {}
    for (i = 0; i < at; i++) next->items[i] = old->items[i];
    next->items[at].callback = callback;
    next->items[at].user = user;
    next->items[at].priority = priority;
    next->items[at].id = ++stream->dsp_next_id;
    for (i = at; i < n; i++) next->items[i + 1] = old->items[i];
    if (out_dsp_id) *out_dsp_id = next->items[at].id;
    replace_dsps_locked(stream, next);
    fa_mutex_unlock(&stream->api_lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_remove_dsp(fa_stream* stream, uint32_t dsp_id)
{
    fa_dsp_list *old, *next = NULL;
    uint32_t i, j = 0, found = 0;
    if (!stream) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->api_lock);
    old = (fa_dsp_list*)fa_load_ptr(&stream->dsps);
    for (i = 0; old && i < old->count; i++) found |= old->items[i].id == dsp_id;
    if (!found) { fa_mutex_unlock(&stream->api_lock); return FA_ERROR_INVALID_ARG; }
    if (old->count > 1) {
        next = dsp_list_alloc(old->count - 1);
        if (!next) { fa_mutex_unlock(&stream->api_lock); return FA_ERROR_OUT_OF_MEMORY; }
        for (i = 0; i < old->count; i++)
            if (old->items[i].id != dsp_id) next->items[j++] = old->items[i];
    }
    replace_dsps_locked(stream, next);
    fa_mutex_unlock(&stream->api_lock);
    return FA_OK;
}

/* ---- seeking without waiting ------------------------------------------------- */

/* Attached streams: notes the target and returns; a worker drops the buffers and
 * refills from there. A newer target replaces one not carried out yet, so dragging
 * a slider costs one seek per worker pass, not one per call. */
FA_API fa_result FA_CALL fa_stream_seek_async(fa_stream* stream, fa_frames frame)
{
    fa_result r = FA_OK;
    int64_t old;
    if (!stream || frame < 0) return FA_ERROR_INVALID_ARG;
    /* While a target is pending, a newer one only replaces it, without the API lock:
     * a worker holds that lock while it seeks, which can take a network round trip. */
    old = fa_load64(&stream->seek_pending);
    if (old >= 0 && frame <= fa_load64(&stream->seek_max) && fa_cas64(&stream->seek_pending, old, frame)) {
        fa_store32(&stream->flushing, 1);
        return FA_OK;
    }
    fa_mutex_lock(&stream->api_lock);
    if (!stream->decoder.seekable) r = FA_ERROR_NOT_SEEKABLE;
    else if (stream->decoder.length >= 0 && frame > stream->decoder.length) r = FA_ERROR_INVALID_ARG;
    else if (!is_attached(stream)) r = seek_locked(stream, frame);   /* nothing plays: no wait to save */
    else {
        fa_store64(&stream->seek_max, stream->decoder.length >= 0 ? stream->decoder.length : INT64_MAX);
        fa_store32(&stream->flushing, 1);   /* the mixer stops at its next callback */
        fa_store64(&stream->seek_pending, frame);
        fa_mutex_lock(&stream->engine->list_lock);
        fa_cond_broadcast(&stream->engine->work_cond);
        fa_mutex_unlock(&stream->engine->list_lock);
    }
    if (r == FA_OK) {
        fa_cas32(&stream->state, FA_STATE_ENDED, FA_STATE_PAUSED);
        fa_cas32(&stream->state, FA_STATE_STOPPED, FA_STATE_PAUSED);
    }
    fa_mutex_unlock(&stream->api_lock);
    return r;
}

/* api_lock is taken second here (API threads take it first), so only tried: an API
 * call in progress finishes, and the seek runs on a later pass. */
int fa_stream_work(struct fa_stream* s, int64_t max_frames)
{
    if (fa_load64(&s->seek_pending) >= 0) {
        int64_t frame = -1;
        fa_result r = FA_OK;
        if (!fa_mutex_trylock(&s->api_lock)) return 0;
        if (is_attached(s)) {
            fa_store32(&s->flushing, 1);
            fa_device_wait_quiescent(s->device);   /* the mixer now skips this stream */
            /* Read after the wait: a newer target may have replaced it meanwhile. */
            frame = fa_load64(&s->seek_pending);
            if (frame >= 0 && (r = restart_expecting_locked(s, frame, frame)) != FA_OK)
                fa_cas64(&s->seek_pending, frame, -1);  /* playback goes on where it was */
            unflush(s);
        }
        fa_mutex_unlock(&s->api_lock);
        if (frame >= 0 && r != FA_OK) {
            fa_event ev;
            ev.type = FA_EVENT_SEEK_FAILED;
            ev.stream = s;
            ev.frame = frame;
            ev.value = r;
            fa_engine_post_event(s->engine, &ev);
        }
        return 1;
    }
    fa_stream_fill(s, max_frames);
    return 1;
}
