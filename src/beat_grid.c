/* Beat grids: the storage shared by analyses and streams, and a stream's grid
 * (the app's, else the exact one of a MIDI file or tracker module). */
#include "fa_internal.h"
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define FA_GRID_FLAGS_ALL (FA_GRID_CONSTANT | FA_GRID_EXACT | FA_GRID_EDITED | FA_GRID_BARS)
#define FA_GRID_SIZE_V1 (offsetof(fa_beat_grid, segment_count) + sizeof(uint32_t))

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

fa_result fa_grid_data_copy(fa_grid_data* dst, const fa_grid_data* src)
{
    size_t bytes = (size_t)src->grid.segment_count * sizeof(fa_tempo_segment);
    memset(dst, 0, sizeof(*dst));
    dst->segs = (fa_tempo_segment*)malloc(bytes ? bytes : 1);
    if (!dst->segs) return FA_ERROR_OUT_OF_MEMORY;
    if (bytes) memcpy(dst->segs, src->segs, bytes);
    dst->grid = src->grid;
    return FA_OK;
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
static fa_result file_grid(struct fa_stream* s, fa_grid_data* out)
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
    r = fa_decoder_open(&dec, &src, NULL, NULL, s->audio_track, &env);
    if (r == FA_OK) {
        r = fa_tracker_of(&dec) ? fa_tracker_grid_render(&dec, out) : FA_ERROR_NOT_FOUND;
        fa_decoder_close(&dec);
    }
    fa_source_close(&src);
    return r;
}

FA_API fa_result FA_CALL fa_stream_set_beat_grid(fa_stream* stream, const fa_beat_grid* grid,
                                                 const fa_tempo_segment* segments, uint32_t count)
{
    fa_grid_data g;
    uint32_t i;
    if (!stream) return FA_ERROR_INVALID_ARG;
    if (!grid) {
        fa_mutex_lock(&stream->grid_lock);
        fa_grid_data_free(&stream->grid);
        stream->grid_state = 0;
        fa_mutex_unlock(&stream->grid_lock);
        return FA_OK;
    }
    if (grid->struct_size < FA_GRID_SIZE_V1 || (count && !segments) || !(grid->bpm >= 1.0 && grid->bpm <= 1000.0) ||
        grid->sample_rate == 0 || grid->sample_rate > 1000000 || grid->beats_per_bar == 0 || grid->beats_per_bar > 64 ||
        grid->bar_offset >= grid->beats_per_bar || (grid->flags & ~(uint32_t)FA_GRID_FLAGS_ALL) ||
        !(grid->confidence >= 0.0f && grid->confidence <= 1.0f))
        return FA_ERROR_INVALID_ARG;
    for (i = 0; i < count; i++) {
        if (!(segments[i].bpm >= 1.0 && segments[i].bpm <= 1000.0) || !isfinite(segments[i].beat)) return FA_ERROR_INVALID_ARG;
        if (i && (segments[i].start <= segments[i - 1].start || segments[i].beat < segments[i - 1].beat))
            return FA_ERROR_INVALID_ARG;
    }
    memset(&g, 0, sizeof(g));
    memcpy(&g.grid, grid, FA_GRID_SIZE_V1);
    g.grid.struct_size = sizeof(fa_beat_grid);
    g.grid.segment_count = count ? count : 1;
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

FA_API fa_result FA_CALL fa_stream_get_beat_grid(fa_stream* stream, fa_beat_grid* out_grid,
                                                 fa_tempo_segment* out_segments, uint32_t capacity,
                                                 uint32_t* out_count)
{
    fa_result r = FA_OK;
    if (out_count) *out_count = 0;
    if (!stream || (capacity && !out_segments) || (out_grid && out_grid->struct_size < sizeof(uint32_t)))
        return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->grid_lock);
    if (stream->grid_state == 0) {
        r = file_grid(stream, &stream->grid);
        if (r == FA_OK) stream->grid_state = 1;
        else if (r == FA_ERROR_NOT_FOUND || r == FA_ERROR_UNSUPPORTED_FORMAT) stream->grid_state = 2;
    }
    if (stream->grid_state == 1) r = fa_grid_data_get(&stream->grid, out_grid, out_segments, capacity, out_count);
    else if (stream->grid_state == 2) r = FA_ERROR_NOT_FOUND;
    fa_mutex_unlock(&stream->grid_lock);
    return r;
}
