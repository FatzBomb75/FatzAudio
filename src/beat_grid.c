/* Beat grids: the storage shared by analyses and streams, and a stream's grid
 * (the app's, else the exact one of a MIDI file or tracker module). */
#include "fa_internal.h"
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define FA_GRID_FLAGS_ALL (FA_GRID_CONSTANT | FA_GRID_EXACT | FA_GRID_EDITED | FA_GRID_BARS)
#define FA_GRID_SIZE_V1 (offsetof(fa_beat_grid, segment_count) + sizeof(uint32_t))
#define FA_GRID_MIN_BPM 0.1        /* MIDI tempos reach 3.6 BPM, tracker ones go lower ... */
#define FA_GRID_MAX_BPM 100000.0   /* ... and far higher (speed 1, tempo 255) */

static int bpm_ok(double bpm) { return bpm >= FA_GRID_MIN_BPM && bpm <= FA_GRID_MAX_BPM; }

int fa_grid_valid(const fa_beat_grid* g, const fa_tempo_segment* segs, uint32_t count)
{
    uint32_t i;
    if (!bpm_ok(g->bpm) || g->sample_rate == 0 || g->sample_rate > 1000000 || g->beats_per_bar == 0 ||
        g->beats_per_bar > 64 || g->bar_offset >= g->beats_per_bar || (g->flags & ~(uint32_t)FA_GRID_FLAGS_ALL) ||
        !(g->confidence >= 0.0f && g->confidence <= 1.0f))
        return 0;
    for (i = 0; i < count; i++) {
        if (!bpm_ok(segs[i].bpm) || !isfinite(segs[i].beat)) return 0;
        if (i && (segs[i].start <= segs[i - 1].start || segs[i].beat < segs[i - 1].beat)) return 0;
    }
    return 1;
}

void fa_grid_data_free(fa_grid_data* g)
{
    free(g->segs);
    memset(g, 0, sizeof(*g));
}

void fa_grid_data_finish(fa_grid_data* g, fa_frames length)
{
    uint32_t i, n = g->grid.segment_count;
    double best = -1.0;
    g->grid.bpm = n ? g->segs[0].bpm : 0.0;
    for (i = 0; i < n; i++) {
        fa_frames end = i + 1 < n ? g->segs[i + 1].start : length;
        double len = (double)(end - g->segs[i].start);
        if (len > best) { best = len; g->grid.bpm = g->segs[i].bpm; }
    }
    if (n == 1) g->grid.flags |= FA_GRID_CONSTANT;
    else g->grid.flags &= ~(uint32_t)FA_GRID_CONSTANT;
}

fa_result fa_grid_data_get(const fa_grid_data* g, fa_beat_grid* out_grid, fa_tempo_segment* out_segments,
                           uint32_t capacity, uint32_t* out_count)
{
    uint32_t n = g->grid.segment_count < capacity ? g->grid.segment_count : capacity;
    if (out_grid) fa_copy_sized(out_grid, &g->grid, sizeof(g->grid));
    if (n) memcpy(out_segments, g->segs, (size_t)n * sizeof(fa_tempo_segment));
    if (out_count) *out_count = g->grid.segment_count;
    return FA_OK;
}

/* ---- A stream's grid ---------------------------------------------------------------- */

/* The exact grid of the stream's file: MIDI from its events, a module from a silent
 * render of its own copy (playback is not touched). */
static fa_result file_grid(struct fa_stream* s, fa_grid_data* out, fa_atomic32* cancel)
{
    fa_source src;
    fa_decoder dec;
    fa_decoder_env env;
    fa_result r;
    memset(out, 0, sizeof(*out));
    if (s->midi) return fa_midi_exact_grid(s->midi, out);
    if (!s->tracker) return FA_ERROR_NOT_FOUND;
    if (s->path) r = fa_source_open_path_window(&src, s->path, s->path_offset, s->path_length);
    else if (s->source.kind == FA_SOURCE_MEMORY) r = fa_source_open_memory_ref(&src, s->source.mem, s->source.mem_size);
    else return FA_ERROR_NOT_FOUND;
    if (r != FA_OK) return r;
    memset(&env, 0, sizeof(env));
    env.midi_rate = s->decoder.format.sample_rate;
    r = fa_decoder_open(&dec, &src, s->path && s->path_length > 0 ? s->path_name : s->path, NULL, s->audio_track, &env);
    if (r == FA_OK) {
        r = fa_tracker_of(&dec) ? fa_tracker_grid_render(&dec, out, cancel) : FA_ERROR_NOT_FOUND;
        fa_decoder_close(&dec);
    }
    fa_source_close(&src);
    return r;
}

FA_API fa_result FA_CALL fa_stream_set_beat_grid(fa_stream* stream, const fa_beat_grid* grid,
                                                 const fa_tempo_segment* segments, uint32_t count)
{
    fa_grid_data g;
    if (!stream) return FA_ERROR_INVALID_ARG;
    if (!grid) {
        fa_mutex_lock(&stream->grid_lock);
        fa_grid_data_free(&stream->grid);
        stream->grid_state = 0;
        fa_mutex_unlock(&stream->grid_lock);
        return FA_OK;
    }
    if (grid->struct_size < FA_GRID_SIZE_V1 || (count && !segments) || !fa_grid_valid(grid, segments, count))
        return FA_ERROR_INVALID_ARG;
    memset(&g, 0, sizeof(g));
    memcpy(&g.grid, grid, FA_GRID_SIZE_V1);
    g.grid.struct_size = sizeof(fa_beat_grid);
    g.grid.segment_count = count ? count : 1;
    if (g.grid.segment_count == 1) g.grid.flags |= FA_GRID_CONSTANT;
    else g.grid.flags &= ~(uint32_t)FA_GRID_CONSTANT;
    g.segs = (fa_tempo_segment*)malloc((size_t)g.grid.segment_count * sizeof(fa_tempo_segment));
    if (!g.segs) return FA_ERROR_OUT_OF_MEMORY;
    if (count) {
        memcpy(g.segs, segments, (size_t)count * sizeof(fa_tempo_segment));
    } else {
        g.segs[0].start = grid->first_beat;
        g.segs[0].bpm = grid->bpm;
        g.segs[0].beat = 0.0;
    }
    fa_mutex_lock(&stream->grid_lock);
    fa_grid_data_free(&stream->grid);
    stream->grid = g;
    stream->grid_state = 1;
    fa_mutex_unlock(&stream->grid_lock);
    return FA_OK;
}

/* A render's result (grid_lock held): kept unless the app set a grid meanwhile. Any
 * failure counts as none, so it is not tried again on every request. */
static void grid_done(struct fa_stream* s, fa_grid_data* g, fa_result r)
{
    if (s->grid_state != 3) {
        fa_grid_data_free(g);
        return;
    }
    if (r == FA_OK) {
        s->grid = *g;
        s->grid_state = 1;
    } else {
        fa_grid_data_free(g);
        s->grid_state = 2;
    }
}

static void grid_thread(void* arg)
{
    struct fa_stream* s = (struct fa_stream*)arg;
    fa_grid_data g;
    fa_result r = file_grid(s, &g, &s->grid_cancel);
    fa_mutex_lock(&s->grid_lock);
    grid_done(s, &g, r);
    fa_mutex_unlock(&s->grid_lock);
    fa_fetch_add32(&s->grid_jobs, -1);
}

/* grid_lock held. A module renders in the background, never under grid_lock: the
 * mixer asks under its own lock, which the event thread and every deck call need.
 * `wait`: the app's call, which may take the time and waits for that render; otherwise
 * the answer is FA_ERROR_BUSY until it is done. */
static fa_result grid_get_locked(struct fa_stream* s, fa_beat_grid* out_grid, fa_tempo_segment* out_segments,
                                 uint32_t capacity, uint32_t* out_count, int wait)
{
    fa_result r = FA_OK;
    if (s->grid_state == 0 && s->tracker) {
        fa_thread t;
        s->grid_state = 3;
        fa_fetch_add32(&s->grid_jobs, 1);
        if (fa_thread_create(&t, grid_thread, s) == 0) {
            fa_thread_detach(&t);
        } else {
            fa_fetch_add32(&s->grid_jobs, -1);
            s->grid_state = 0;   /* no thread: rendered here after all */
        }
    }
    while (s->grid_state == 3 && wait) {
        fa_mutex_unlock(&s->grid_lock);
        fa_sleep_ms(5);
        fa_mutex_lock(&s->grid_lock);
    }
    if (s->grid_state == 0) {
        fa_grid_data g;
        s->grid_state = 3;
        r = file_grid(s, &g, NULL);
        grid_done(s, &g, r);
    }
    if (s->grid_state == 1) r = fa_grid_data_get(&s->grid, out_grid, out_segments, capacity, out_count);
    else if (s->grid_state == 2) r = FA_ERROR_NOT_FOUND;
    else r = FA_ERROR_BUSY;
    return r;
}

FA_API fa_result FA_CALL fa_stream_get_beat_grid(fa_stream* stream, fa_beat_grid* out_grid,
                                                 fa_tempo_segment* out_segments, uint32_t capacity,
                                                 uint32_t* out_count)
{
    fa_result r;
    if (out_count) *out_count = 0;
    if (!stream || (capacity && !out_segments) || (out_grid && out_grid->struct_size < sizeof(uint32_t)))
        return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->grid_lock);
    r = grid_get_locked(stream, out_grid, out_segments, capacity, out_count, 1);
    fa_mutex_unlock(&stream->grid_lock);
    return r;
}

fa_result fa_stream_beat_grid_now(struct fa_stream* stream, fa_beat_grid* out_grid,
                                  fa_tempo_segment* out_segments, uint32_t capacity, uint32_t* out_count)
{
    fa_result r;
    if (out_count) *out_count = 0;
    fa_mutex_lock(&stream->grid_lock);
    r = grid_get_locked(stream, out_grid, out_segments, capacity, out_count, 0);
    fa_mutex_unlock(&stream->grid_lock);
    return r;
}

void fa_stream_grid_closing(struct fa_stream* stream)
{
    fa_store32(&stream->grid_cancel, 1);
    while (fa_load32(&stream->grid_jobs)) fa_sleep_ms(1);
}
