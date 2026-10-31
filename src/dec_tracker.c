/* Tracker modules (MOD, S3M, XM, IT, MPTM, MTM, UMX and the other formats
 * libopenmpt reads), played by libopenmpt (vendored in third_party/libopenmpt,
 * linked in statically). Also the tracker stream API: module information and
 * texts, order/row seeking and live render settings.
 *
 * The module plays once (no repeat): the length is libopenmpt's duration of the
 * song, and exactly that many frames come out (padded with silence or cut, so
 * the length the stream reports is the length it plays). Looping is the stream's
 * own (fa_stream_set_loop), from the start.
 *
 * Rendering is cut into blocks of FA_TRACKER_BLOCK frames from the last seek, so
 * the read size changes nothing. A seek asks libopenmpt for the row at or before
 * the target (it restores the channels' state there, sample positions included)
 * and renders on to the exact frame without handing it out.
 *
 * Live settings (interpolation, stereo separation, volume ramping, Amiga
 * resampler emulation, channel mute) are atomics the decode thread applies at the
 * start of each block. The module itself is only touched by whoever holds the
 * stream's decode_lock (the decode thread, or a seek).
 *
 * The row log: as it renders (in steps of FA_TRACKER_STEP frames), the decode
 * thread notes the frame where each row starts. The song plays once, so a frame
 * always belongs to the same row: the log only has to say which frames were
 * decoded. Each seek starts a new run (the frames from the seek on); a lookup uses
 * the newest run that covers the frame. The audio thread reads it to raise
 * FA_EVENT_TRACKER_ROW, the API to tell the row heard now. Single writer (the
 * decode_lock holder), lock-free readers: entries and runs are atomics published
 * by their write counters; FA_TRACKER_ROWS entries are many seconds of song, far
 * more than the decode-ahead, so a reader never sees one being reused. */
#include "fa_internal.h"
#include "libopenmpt/libopenmpt.h"
#include "libopenmpt/libopenmpt_ext.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FA_TRACKER_BLOCK 512
#define FA_TRACKER_MAX_FILE ((size_t)128 << 20)
#define FA_TRACKER_MAX_SECONDS (6.0 * 3600.0)
#define FA_TRACKER_MUTE_CHANNELS 64
#define FA_TRACKER_AMPLIFY_MIN (-6000)   /* millibel */
#define FA_TRACKER_AMPLIFY_MAX 6000
#define FA_TRACKER_STEP 64          /* row starts are noted to within this many frames */
#define FA_TRACKER_ROWS 4096        /* row log entries (a power of two) */
#define FA_TRACKER_RUNS 8           /* runs kept (seeks) */
#define FA_TRACKER_NEW_ORDER ((int64_t)1 << 32)

typedef struct fa_tracker_row { fa_atomic64 tag, frame, value; } fa_tracker_row;   /* value: order << 16 | row, + NEW_ORDER */
/* An instrument or sync event of a row: value = run << 48 | kind << 32 | channel << 16 | data. */
#define FA_TRACKER_CELLS 2048       /* cell event entries kept (a power of two) */
#define FA_TRACKER_KIND_INSTRUMENT 1
#define FA_TRACKER_KIND_SYNC 2
#define FA_TRACKER_CMD_MIDI 31      /* libopenmpt's internal command for the Zxx macro (IT, S3M) */
#define FA_TRACKER_NOTE_MAX 128
typedef struct fa_tracker_cell { fa_atomic64 tag, frame, value; } fa_tracker_cell;
typedef struct fa_tracker_run { fa_atomic64 tag, first, start, end; } fa_tracker_run;

struct fa_tracker {
    openmpt_module_ext* ext;
    openmpt_module* mod;
    openmpt_module_ext_interface_interactive interactive;
    openmpt_module_ext_interface_interactive3 interactive3;
    int has_interactive, has_interactive3;
    uint32_t rate;
    fa_frames length;
    fa_frames pos;              /* frames handed out (the stream position) */
    int ended;                  /* libopenmpt has no more: silence up to length */

    float blk[FA_TRACKER_BLOCK * 2];
    int blk_len, blk_off;

    fa_tracker_info info;
    char* message;
    char** names[FA_TRACKER_TEXT_SUBSONG + 1];   /* per text type, by index */
    uint32_t name_count[FA_TRACKER_TEXT_SUBSONG + 1];

    /* Set by the API, applied by the decode thread. */
    fa_atomic32 interpolation, separation, ramping, amiga, amplify;
    fa_atomic64 mute;           /* bit n = channel n */
    int32_t interpolation_now, separation_now, ramping_now, amiga_now, amplify_now;
    uint64_t mute_now;
    fa_atomic32 row_events;     /* FA_TRACKER_ROW_EVENTS */
    fa_atomic32 cell_events;    /* FA_TRACKER_CELL_EVENTS */

    /* Channel volumes set by the API (under the stream's decode_lock): a seek
     * puts the module back in its own state at the target, so they are set again. */
    double chan_vol[FA_TRACKER_MUTE_CHANNELS];
    uint64_t chan_vol_set;      /* bit n = channel n has one */

    /* Row log (see the top). */
    fa_tracker_row rows[FA_TRACKER_ROWS];
    fa_tracker_run runs[FA_TRACKER_RUNS];
    fa_atomic64 rows_w, runs_w;
    fa_tracker_cell cells[FA_TRACKER_CELLS];   /* instrument and sync events of the rows, in the same runs */
    fa_atomic64 cells_w;
    int64_t last_value;         /* the run's last entry, -1 = none yet */
    int log_after;              /* at open: note the first row after the first read */

    /* Grid recording (fa_tracker_grid_start): every new row with its tempo. */
    struct grid_row* grid_rows;
    size_t grid_n, grid_cap;
    int grid_on, grid_failed;
};

typedef struct grid_row {
    fa_frames frame;            /* noted within FA_TRACKER_STEP frames after the row starts */
    double bpm;                 /* libopenmpt's tempo of the row, in beats */
    int32_t rows_per_beat;
} grid_row;

/* ---- Row log ---- */

/* Each entry and run carries its index (`tag`): -1 while the writer fills it,
 * then the index. A reader takes a field only if the tag reads the index it
 * wants before and after, so a slot the writer reuses meanwhile is seen as gone. */

static void log_new_run(struct fa_tracker* t, fa_frames start)
{
    int64_t k = fa_load64(&t->runs_w);
    fa_tracker_run* r = &t->runs[k % FA_TRACKER_RUNS];
    fa_store64(&r->tag, -1);
    fa_store64(&r->first, fa_load64(&t->rows_w));
    fa_store64(&r->start, start);
    fa_store64(&r->end, start);
    fa_store64(&r->tag, k);
    fa_store64(&t->runs_w, k + 1);
    t->last_value = -1;
}

static void set_run_end(struct fa_tracker* t, fa_frames end)
{
    fa_store64(&t->runs[(fa_load64(&t->runs_w) - 1) % FA_TRACKER_RUNS].end, end);
}

#define FA_TRACKER_GRID_MAX 100000  /* segments of an exact grid, at most */

static void grid_note(struct fa_tracker* t, fa_frames frame)
{
    grid_row* g;
    int32_t rpb = openmpt_module_get_pattern_rows_per_beat(t->mod, openmpt_module_get_current_pattern(t->mod));
    if (t->grid_failed) return;
    if (t->grid_n == t->grid_cap) {
        size_t cap = t->grid_cap ? t->grid_cap * 2 : 1024;
        grid_row* grown = (grid_row*)realloc(t->grid_rows, cap * sizeof(grid_row));
        if (!grown) { t->grid_failed = 1; return; }
        t->grid_rows = grown;
        t->grid_cap = cap;
    }
    g = &t->grid_rows[t->grid_n++];
    g->frame = frame;
    g->bpm = openmpt_module_get_current_estimated_bpm(t->mod);
    g->rows_per_beat = rpb > 0 ? rpb : 4;
}

/* Instrument and sync events of the row libopenmpt is on (FA_TRACKER_CELL_EVENTS), read
 * from the pattern data: a note with an instrument number, and the Zxx macro command. */
static void note_cell(struct fa_tracker* t, fa_frames frame, int kind, int32_t channel, int32_t data)
{
    int64_t i = fa_load64(&t->cells_w);
    fa_tracker_cell* e = &t->cells[i & (FA_TRACKER_CELLS - 1)];
    fa_store64(&e->tag, -1);
    fa_store64(&e->frame, frame);
    fa_store64(&e->value, (int64_t)((uint64_t)((fa_load64(&t->runs_w) - 1) & 0xFFFF) << 48) | (int64_t)kind << 32 |
                              (int64_t)(channel & 0xFFFF) << 16 | (data & 0xFFFF));
    fa_store64(&e->tag, i);
    fa_store64(&t->cells_w, i + 1);
}

static void note_cells(struct fa_tracker* t, fa_frames frame)
{
    int32_t mask = fa_load32(&t->cell_events), pat, row, ch, n;
    if (!mask) return;
    pat = openmpt_module_get_current_pattern(t->mod);
    row = openmpt_module_get_current_row(t->mod);
    n = (int32_t)t->info.channels;
    for (ch = 0; ch < n; ch++) {
        if (mask & 1) {
            int32_t instr = openmpt_module_get_pattern_row_channel_command(t->mod, pat, row, ch, OPENMPT_MODULE_COMMAND_INSTRUMENT);
            int32_t note = instr ? openmpt_module_get_pattern_row_channel_command(t->mod, pat, row, ch, OPENMPT_MODULE_COMMAND_NOTE) : 0;
            if (instr && note >= 1 && note <= FA_TRACKER_NOTE_MAX) note_cell(t, frame, FA_TRACKER_KIND_INSTRUMENT, ch, instr - 1);
        }
        if ((mask & 2) &&
            openmpt_module_get_pattern_row_channel_command(t->mod, pat, row, ch, OPENMPT_MODULE_COMMAND_EFFECT) == FA_TRACKER_CMD_MIDI)
            note_cell(t, frame, FA_TRACKER_KIND_SYNC, ch,
                      openmpt_module_get_pattern_row_channel_command(t->mod, pat, row, ch, OPENMPT_MODULE_COMMAND_PARAMETER));
    }
}

/* The row playing at `frame` (libopenmpt's current one), noted if it is new. */
static void log_row(struct fa_tracker* t, fa_frames frame)
{
    int32_t order = openmpt_module_get_current_order(t->mod), row = openmpt_module_get_current_row(t->mod);
    int64_t v, i, last = t->last_value & 0xFFFFFFFF;
    fa_tracker_row* e;
    if (order < 0 || row < 0) return;
    v = ((int64_t)order << 16) | (row & 0xFFFF);
    if (t->last_value >= 0 && v == last) return;
    /* The first row of an order: the order changed, or the song went back to
     * row 0 of the same one (a jump to itself); a run's first entry when it is
     * a row 0. */
    if (t->last_value < 0 ? row == 0
                          : order != (int32_t)(last >> 16) || (row == 0 && (last & 0xFFFF) != 0))
        v |= FA_TRACKER_NEW_ORDER;
    i = fa_load64(&t->rows_w);
    e = &t->rows[i & (FA_TRACKER_ROWS - 1)];
    fa_store64(&e->tag, -1);
    fa_store64(&e->frame, frame);
    fa_store64(&e->value, v);
    fa_store64(&e->tag, i);
    fa_store64(&t->rows_w, i + 1);
    t->last_value = v;
    note_cells(t, frame);
    if (t->grid_on) grid_note(t, frame);
}

/* A run from `start`, where libopenmpt now is (after a seek): its row is known
 * at once, also when nothing is rendered (a seek that lands on a row start). */
static void log_start(struct fa_tracker* t, fa_frames start)
{
    t->log_after = 0;
    log_new_run(t, start);
    log_row(t, start);
    set_run_end(t, start + 1);
}

/* Entry i, if it is still entry i (see the tags above). */
static int read_row(const struct fa_tracker* t, int64_t i, fa_frames* frame, int64_t* value)
{
    fa_tracker_row* e = (fa_tracker_row*)&t->rows[i & (FA_TRACKER_ROWS - 1)];
    if (fa_load64(&e->tag) != i) return 0;
    *frame = fa_load64(&e->frame);
    *value = fa_load64(&e->value);
    return fa_load64(&e->tag) == i;
}

typedef struct run_view { int64_t first, last, k; fa_frames start, end; } run_view;

/* The newest run that decoded `frame`, with entries in it, as its entry range. */
static int find_run(const struct fa_tracker* t, fa_frames frame, run_view* out)
{
    int64_t rw = fa_load64((fa_atomic64*)&t->rows_w), kw = fa_load64((fa_atomic64*)&t->runs_w), k, next = rw;
    for (k = kw - 1; k >= 0 && k >= kw - FA_TRACKER_RUNS; k--) {
        fa_tracker_run* r = (fa_tracker_run*)&t->runs[k % FA_TRACKER_RUNS];
        int64_t f;
        fa_frames start, end;
        if (fa_load64(&r->tag) != k) return 0;   /* reused: older runs are gone too */
        f = fa_load64(&r->first);
        start = fa_load64(&r->start);
        end = fa_load64(&r->end);
        if (fa_load64(&r->tag) != k) return 0;
        if (start <= frame && frame < end && next > f) {
            out->first = f > rw - FA_TRACKER_ROWS + 1 ? f : rw - FA_TRACKER_ROWS + 1;
            out->last = next - 1;
            out->k = k;
            out->start = start;
            out->end = end;
            if (out->last >= out->first) return 1;
        }
        next = f;
    }
    return 0;
}

int fa_tracker_row_at(const struct fa_tracker* t, fa_frames frame, uint32_t* order, uint32_t* row)
{
    run_view rv;
    int64_t i, v;
    fa_frames f;
    if (!find_run(t, frame, &rv)) return 0;
    for (i = rv.last; i >= rv.first; i--) {
        if (!read_row(t, i, &f, &v)) return 0;
        if (f <= frame) {
            *order = (uint32_t)((v >> 16) & 0xFFFF);
            *row = (uint32_t)(v & 0xFFFF);
            return 1;
        }
    }
    return 0;
}

int fa_tracker_row_events_on(const struct fa_tracker* t)
{
    return fa_load32((fa_atomic32*)&t->row_events) != 0;
}

uint32_t fa_tracker_rows_in(const struct fa_tracker* t, fa_frames from, fa_frames to, fa_frames* frames,
                            int64_t* values, uint32_t cap)
{
    int32_t mode = fa_load32((fa_atomic32*)&t->row_events);
    uint32_t n = 0;
    if (mode == 0) return 0;
    /* Run by run: a run covers only what it decoded, and a later stretch of the
     * range may be in an older run (a loop shorter than the decode-ahead). */
    while (from < to && n < cap) {
        run_view rv;
        int64_t i, v;
        fa_frames f, stop;
        if (!find_run(t, from, &rv)) break;
        stop = rv.end < to ? rv.end : to;
        /* Back from the newest entry to the first one before `from` (the decoder
         * is only a little ahead, so this is a few entries), then forward. */
        for (i = rv.last; i > rv.first; i--)
            if (!read_row(t, i, &f, &v) || f < from) break;
        for (; i <= rv.last && n < cap; i++) {
            if (!read_row(t, i, &f, &v)) continue;
            if (f < from) continue;
            if (f >= stop) break;
            if (mode == 2 && !(v & FA_TRACKER_NEW_ORDER)) continue;
            frames[n] = f;
            values[n++] = v & 0xFFFFFFFF;
        }
        if (n == cap) break;
        from = stop;
    }
    return n;
}

int fa_tracker_cell_events_on(const struct fa_tracker* t)
{
    return fa_load32((fa_atomic32*)&t->cell_events) != 0;
}

/* The instrument and sync events of the rows starting in [from, to) (values: kind << 32 |
 * channel << 16 | data), from the newest run that decoded each stretch, like rows_in. */
uint32_t fa_tracker_cells_in(const struct fa_tracker* t, fa_frames from, fa_frames to, fa_frames* frames,
                             int64_t* values, uint32_t cap)
{
    uint32_t n = 0;
    if (fa_load32((fa_atomic32*)&t->cell_events) == 0) return 0;
    while (from < to && n < cap) {
        run_view rv;
        int64_t w = fa_load64((fa_atomic64*)&t->cells_w), i, lo = w;
        fa_frames stop;
        if (!find_run(t, from, &rv)) break;
        stop = rv.end < to ? rv.end : to;
        /* Entries of one run are contiguous, in frame order: back to the first one at or after `from`. */
        for (i = w - 1; i >= 0 && i >= w - FA_TRACKER_CELLS; i--) {
            fa_tracker_cell* e = (fa_tracker_cell*)&t->cells[i & (FA_TRACKER_CELLS - 1)];
            int64_t f, v;
            int16_t d;
            if (fa_load64(&e->tag) != i) break;
            f = fa_load64(&e->frame);
            v = fa_load64(&e->value);
            if (fa_load64(&e->tag) != i) break;
            d = (int16_t)(((v >> 48) - rv.k) & 0xFFFF);
            if (d > 0) continue;       /* a newer run */
            if (d < 0 || f < from) break;
            lo = i;
        }
        for (i = lo; i < w && n < cap; i++) {
            fa_tracker_cell* e = (fa_tracker_cell*)&t->cells[i & (FA_TRACKER_CELLS - 1)];
            int64_t f, v;
            if (fa_load64(&e->tag) != i) break;
            f = fa_load64(&e->frame);
            v = fa_load64(&e->value);
            if (fa_load64(&e->tag) != i) break;
            if ((int16_t)(((v >> 48) - rv.k) & 0xFFFF) != 0 || f >= stop) break;
            frames[n] = f;
            values[n++] = v & 0xFFFFFFFFFFLL;
        }
        from = stop;
    }
    return n;
}

struct fa_tracker* fa_tracker_of(fa_decoder* dec)
{
    return dec && dec->vt == &fa_decoder_tracker ? (struct fa_tracker*)dec->impl : NULL;
}

/* The signature of a common format: such a header beats the frame-sync
 * scanners (MP3, AC-3, ADTS), which can match raw sample data later in the window. */
static int has_magic(const uint8_t* d, size_t n)
{
    static const char* const mod_tags[] = { "M.K.", "M!K!", "M&K!", "FLT4", "FLT8", "CD81", "OKTA", "OCTA" };
    size_t i;
    if (n >= 17 && memcmp(d, "Extended Module: ", 17) == 0) return 1;   /* XM */
    if (n >= 4 && (memcmp(d, "IMPM", 4) == 0 || memcmp(d, "MO3", 3) == 0)) return 1;   /* IT, MPTM, MO3 */
    if (n >= 48 && memcmp(d + 44, "SCRM", 4) == 0) return 1;   /* S3M */
    if (n >= 1084) {
        const uint8_t* t = d + 1080;   /* 31-sample MOD */
        for (i = 0; i < sizeof(mod_tags) / sizeof(mod_tags[0]); i++)
            if (memcmp(t, mod_tags[i], 4) == 0) return 1;
        if (t[0] >= '1' && t[0] <= '9' && memcmp(t + 1, "CHN", 3) == 0) return 1;
        if (t[0] >= '1' && t[0] <= '9' && t[1] >= '0' && t[1] <= '9' && (memcmp(t + 2, "CH", 2) == 0 || memcmp(t + 2, "CN", 2) == 0))
            return 1;
    }
    return 0;
}

int fa_probe_tracker(const uint8_t* data, size_t size, size_t id3_size)
{
    int r;
    (void)id3_size;
    if (size == 0) return 0;
    r = openmpt_probe_file_header_without_filesize(OPENMPT_PROBE_FILE_HEADER_FLAGS_DEFAULT, data, size,
                                                   openmpt_log_func_silent, NULL, openmpt_error_func_ignore, NULL,
                                                   NULL, NULL);
    /* Some formats have no magic number (15-sample MODs) and the window may be
     * too short to tell: low scores, so a format that is sure wins. */
    if (r == OPENMPT_PROBE_FILE_HEADER_RESULT_SUCCESS) return has_magic(data, size) ? 90 : 30;
    if (r == OPENMPT_PROBE_FILE_HEADER_RESULT_WANTMOREDATA) return 2;
    return 0;
}

/* libopenmpt reads the file through the source itself (seeking as it needs), so the
 * whole file is never held a second time next to what libopenmpt keeps. */
typedef struct src_ctx { fa_source* src; fa_result err; } src_ctx;   /* err: the first read error */

static size_t src_read(void* stream, void* dst, size_t bytes)
{
    src_ctx* c = (src_ctx*)stream;
    int64_t got = fa_source_read(c->src, dst, bytes);
    if (got < 0 && c->err == FA_OK) c->err = (fa_result)got;
    return got > 0 ? (size_t)got : 0;
}

static int src_seek(void* stream, int64_t offset, int whence)
{
    fa_source* src = ((src_ctx*)stream)->src;
    int64_t base = whence == OPENMPT_STREAM_SEEK_CUR ? fa_source_tell(src) : whence == OPENMPT_STREAM_SEEK_END ? src->length : 0;
    if (whence != OPENMPT_STREAM_SEEK_SET && whence != OPENMPT_STREAM_SEEK_CUR && whence != OPENMPT_STREAM_SEEK_END) return -1;
    if (base < 0 || offset < -base || offset > src->length - base) return -1;
    return fa_source_seek(src, base + offset) == FA_OK ? 0 : -1;
}

static int64_t src_tell(void* stream) { return fa_source_tell(((src_ctx*)stream)->src); }

/* Checks that the source is a module (a weak probe is checked again with the real file
 * size) and leaves it at the start. */
static fa_result check_source(fa_source* src)
{
    /* A live or unknown-length source is never a module. */
    if (src->length <= 0 || src->length > (int64_t)FA_TRACKER_MAX_FILE) return FA_ERROR_UNSUPPORTED_FORMAT;
    {
        uint8_t head[4096];
        size_t h = 0, want = openmpt_probe_file_header_get_recommended_size();
        int pr;
        if (want > sizeof(head)) want = sizeof(head);
        while (h < want) {
            int64_t got = fa_source_read(src, head + h, want - h);
            if (got < 0) return (fa_result)got;
            if (got == 0) break;
            h += (size_t)got;
        }
        pr = openmpt_probe_file_header(OPENMPT_PROBE_FILE_HEADER_FLAGS_DEFAULT, head, h, (uint64_t)src->length,
                                       openmpt_log_func_silent, NULL, openmpt_error_func_ignore, NULL, NULL, NULL);
        if (pr != OPENMPT_PROBE_FILE_HEADER_RESULT_SUCCESS) return FA_ERROR_UNSUPPORTED_FORMAT;
        if (fa_source_seek(src, 0) != FA_OK) return FA_ERROR_NOT_SEEKABLE;
    }
    return FA_OK;
}

/* `in` (UTF-8, from libopenmpt) into `out`, cut at a character boundary. */
static void utf8_copy(char* out, size_t cap, const char* in)
{
    size_t n = strlen(in);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)in[n] & 0xC0) == 0x80) n--;
    }
    memcpy(out, in, n);
    out[n] = 0;
}

static void copy_meta(openmpt_module* mod, const char* key, char* out, size_t cap)
{
    const char* v = openmpt_module_get_metadata(mod, key);
    out[0] = 0;
    if (v) {
        utf8_copy(out, cap, v);
        openmpt_free_string(v);
    }
}

static char* dup_free(const char* s)
{
    char* d;
    if (!s) return NULL;
    d = fa_strdup(s);
    openmpt_free_string(s);
    return d;
}

static fa_result load_names(struct fa_tracker* t, int type, uint32_t count)
{
    uint32_t i;
    if (!count) return FA_OK;
    t->names[type] = (char**)calloc(count, sizeof(char*));
    if (!t->names[type]) return FA_ERROR_OUT_OF_MEMORY;
    t->name_count[type] = count;
    for (i = 0; i < count; i++) {
        const char* s = NULL;
        switch (type) {
        case FA_TRACKER_TEXT_INSTRUMENT: s = openmpt_module_get_instrument_name(t->mod, (int32_t)i); break;
        case FA_TRACKER_TEXT_SAMPLE: s = openmpt_module_get_sample_name(t->mod, (int32_t)i); break;
        case FA_TRACKER_TEXT_CHANNEL: s = openmpt_module_get_channel_name(t->mod, (int32_t)i); break;
        case FA_TRACKER_TEXT_PATTERN: s = openmpt_module_get_pattern_name(t->mod, (int32_t)i); break;
        case FA_TRACKER_TEXT_SUBSONG: s = openmpt_module_get_subsong_name(t->mod, (int32_t)i); break;
        default: break;
        }
        t->names[type][i] = dup_free(s);
        if (s && !t->names[type][i]) return FA_ERROR_OUT_OF_MEMORY;
    }
    return FA_OK;
}

static int32_t count_of(int32_t n) { return n > 0 ? n : 0; }

static void tracker_free(struct fa_tracker* t)
{
    int k;
    uint32_t i;
    if (!t) return;
    if (t->ext) openmpt_module_ext_destroy(t->ext);
    free(t->grid_rows);
    free(t->message);
    for (k = 0; k <= FA_TRACKER_TEXT_SUBSONG; k++) {
        for (i = 0; i < t->name_count[k]; i++) free(t->names[k][i]);
        free(t->names[k]);
    }
    free(t);
}

/* The settings the API changed since the last block. */
static void take_controls(struct fa_tracker* t)
{
    int32_t v;
    uint64_t mute;
    v = fa_load32(&t->interpolation);
    if (v != t->interpolation_now) {
        openmpt_module_set_render_param(t->mod, OPENMPT_MODULE_RENDER_INTERPOLATIONFILTER_LENGTH, v);
        t->interpolation_now = v;
    }
    v = fa_load32(&t->separation);
    if (v != t->separation_now) {
        openmpt_module_set_render_param(t->mod, OPENMPT_MODULE_RENDER_STEREOSEPARATION_PERCENT, v);
        t->separation_now = v;
    }
    v = fa_load32(&t->ramping);
    if (v != t->ramping_now) {
        openmpt_module_set_render_param(t->mod, OPENMPT_MODULE_RENDER_VOLUMERAMPING_STRENGTH, v);
        t->ramping_now = v;
    }
    v = fa_load32(&t->amiga);
    if (v != t->amiga_now) {
        openmpt_module_ctl_set_boolean(t->mod, "render.resampler.emulate_amiga", v);
        t->amiga_now = v;
    }
    v = fa_load32(&t->amplify);
    if (v != t->amplify_now) {
        openmpt_module_set_render_param(t->mod, OPENMPT_MODULE_RENDER_MASTERGAIN_MILLIBEL, v);
        t->amplify_now = v;
    }
    mute = (uint64_t)fa_load64(&t->mute);
    if (mute != t->mute_now && t->has_interactive) {
        int32_t ch;
        for (ch = 0; ch < (int32_t)t->info.channels && ch < FA_TRACKER_MUTE_CHANNELS; ch++)
            if (((mute ^ t->mute_now) >> ch) & 1)
                t->interactive.set_channel_mute_status(t->ext, ch, (int)((mute >> ch) & 1));
        t->mute_now = mute;
    }
}

/* The next block: libopenmpt's audio, then silence up to the length. */
static void render_block(struct fa_tracker* t)
{
    fa_frames n = t->length - t->pos;
    size_t got = 0;
    if (n > FA_TRACKER_BLOCK) n = FA_TRACKER_BLOCK;
    take_controls(t);
    while (!t->ended && got < (size_t)n) {
        size_t step = (size_t)n - got, k;
        if (step > FA_TRACKER_STEP) step = FA_TRACKER_STEP;
        if (!t->log_after) log_row(t, t->pos + (fa_frames)got);
        k = openmpt_module_read_interleaved_float_stereo(t->mod, (int32_t)t->rate, step, t->blk + got * 2);
        if (t->log_after) {
            log_row(t, t->pos + (fa_frames)got);
            t->log_after = 0;
        }
        got += k;
        if (k < step) t->ended = 1;
    }
    set_run_end(t, t->pos + n);
    if (got < (size_t)n) memset(t->blk + got * 2, 0, ((size_t)n - got) * 2 * sizeof(float));
    t->blk_len = (int)n;
    t->blk_off = 0;
}

static fa_result tracker_read(fa_decoder* dec, float* out, fa_frames frames, fa_frames* got)
{
    struct fa_tracker* t = (struct fa_tracker*)dec->impl;
    fa_frames done = 0;
    while (done < frames) {
        fa_frames k;
        if (t->blk_off == t->blk_len) {
            if (t->pos >= t->length) break;
            render_block(t);
        }
        k = t->blk_len - t->blk_off;
        if (k > frames - done) k = frames - done;
        memcpy(out + done * 2, t->blk + (size_t)t->blk_off * 2, (size_t)k * 2 * sizeof(float));
        t->blk_off += (int)k;
        t->pos += k;
        done += k;
    }
    *got = done;
    return done > 0 ? FA_OK : FA_ERROR_END_OF_STREAM;
}

static fa_result tracker_seek(fa_decoder* dec, fa_frames frame)
{
    struct fa_tracker* t = (struct fa_tracker*)dec->impl;
    double at;
    fa_frames from;
    if (frame < 0) frame = 0;
    if (frame > t->length) frame = t->length;
    t->blk_len = t->blk_off = 0;
    t->ended = 0;
    /* libopenmpt lands on the start of a row, the first one starting at or after
     * the time asked: ask earlier until it lands before the target. */
    {
        double want = (double)frame / t->rate, back = 0.25, ask = want;
        for (;;) {
            at = openmpt_module_set_position_seconds(t->mod, ask);
            from = at > 0 ? (fa_frames)llround(at * t->rate) : 0;
            if (from <= frame || ask <= 0) break;
            ask = want - back > 0 ? want - back : 0;
            back *= 2;
        }
        if (from > frame) from = frame;   /* not reached: the start is after it (cannot happen at 0) */
    }
    t->pos = from;
    if (t->has_interactive && t->chan_vol_set) {
        int32_t ch;
        for (ch = 0; ch < FA_TRACKER_MUTE_CHANNELS && ch < (int32_t)t->info.channels; ch++)
            if ((t->chan_vol_set >> ch) & 1) t->interactive.set_channel_volume(t->ext, ch, t->chan_vol[ch]);
    }
    log_start(t, from);
    /* On to the exact frame, without handing anything out. */
    while (t->pos < frame) {
        fa_frames skip = frame - t->pos;
        render_block(t);
        if (skip >= t->blk_len) {
            t->pos += t->blk_len;
            t->blk_len = t->blk_off = 0;
        } else {
            t->blk_off = (int)skip;
            t->pos += skip;
        }
    }
    return FA_OK;
}

static void tracker_close(fa_decoder* dec)
{
    tracker_free((struct fa_tracker*)dec->impl);
    dec->impl = NULL;
}

static fa_result tracker_open(fa_decoder* dec)
{
    struct fa_tracker* t;
    openmpt_stream_callbacks cb;
    src_ctx ctx;
    int err = 0;
    double duration;
    const char* s;
    fa_result r = check_source(dec->src);
    if (r != FA_OK) return r;
    t = (struct fa_tracker*)calloc(1, sizeof(*t));
    if (!t) return FA_ERROR_OUT_OF_MEMORY;
    cb.read = src_read;
    cb.seek = src_seek;
    cb.tell = src_tell;
    ctx.src = dec->src;
    ctx.err = FA_OK;
    t->ext = openmpt_module_ext_create(cb, &ctx, openmpt_log_func_silent, NULL, openmpt_error_func_store,
                                       &err, &err, NULL, NULL);
    if (t->ext && ctx.err != FA_OK) {   /* a read failed but libopenmpt carried on with what it had */
        openmpt_module_ext_destroy(t->ext);
        t->ext = NULL;
    }
    if (!t->ext) {
        free(t);
        if (ctx.err != FA_OK) return ctx.err;
        return err == OPENMPT_ERROR_OUT_OF_MEMORY ? FA_ERROR_OUT_OF_MEMORY : FA_ERROR_UNSUPPORTED_FORMAT;
    }
    t->mod = openmpt_module_ext_get_module(t->ext);
    t->has_interactive = openmpt_module_ext_get_interface(t->ext, LIBOPENMPT_EXT_C_INTERFACE_INTERACTIVE,
                                                          &t->interactive, sizeof(t->interactive)) != 0;
    t->has_interactive3 = openmpt_module_ext_get_interface(t->ext, LIBOPENMPT_EXT_C_INTERFACE_INTERACTIVE3,
                                                           &t->interactive3, sizeof(t->interactive3)) != 0;
    t->rate = dec->midi_rate ? dec->midi_rate : FA_MIDI_DEFAULT_RATE;
    openmpt_module_set_repeat_count(t->mod, 0);
    /* Subsongs are the stream's audio tracks (fa_open_options.audio_track). A
     * track past the last fails in fa_decoder_open (FA_ERROR_NOT_FOUND). */
    dec->track_count = openmpt_module_get_num_subsongs(t->mod);
    if (dec->track_count < 1) dec->track_count = 1;
    t->info.subsong = dec->audio_track > 0 && dec->audio_track < dec->track_count ? dec->audio_track : 0;
    if (t->info.subsong > 0) openmpt_module_select_subsong(t->mod, t->info.subsong);
    duration = openmpt_module_get_duration_seconds(t->mod);
    if (!(duration > 0)) duration = 0;
    if (duration > FA_TRACKER_MAX_SECONDS) duration = FA_TRACKER_MAX_SECONDS;
    t->length = (fa_frames)llround(duration * t->rate);
    if (t->length <= 0) { tracker_free(t); return FA_ERROR_UNSUPPORTED_FORMAT; }
    /* Before the first read libopenmpt reports order 0 even when the song
     * starts later (a "+++" skip at order 0): the first row is noted after it. */
    log_new_run(t, 0);
    t->log_after = 1;

    /* Defaults, as libopenmpt has them (the Amiga resampler off: it costs about
     * 19 times as much on MOD files). */
    t->interpolation_now = 0;
    t->separation_now = 100;
    t->ramping_now = -1;
    t->amiga_now = 0;
    openmpt_module_ctl_set_boolean(t->mod, "render.resampler.emulate_amiga", 0);
    /* A seek also puts every playing sample where it would be, not at its start. */
    openmpt_module_ctl_set_boolean(t->mod, "seek.sync_samples", 1);
    fa_store32(&t->interpolation, 0);
    fa_store32(&t->separation, 100);
    fa_store32(&t->ramping, -1);
    fa_store32(&t->amiga, 0);
    fa_store32(&t->amplify, 0);
    fa_store64(&t->mute, 0);

    t->info.struct_size = sizeof(t->info);
    copy_meta(t->mod, "type", t->info.type, sizeof(t->info.type));
    copy_meta(t->mod, "type_long", t->info.type_name, sizeof(t->info.type_name));
    copy_meta(t->mod, "tracker", t->info.tracker, sizeof(t->info.tracker));
    copy_meta(t->mod, "title", t->info.title, sizeof(t->info.title));
    copy_meta(t->mod, "artist", t->info.artist, sizeof(t->info.artist));
    t->info.channels = (uint32_t)count_of(openmpt_module_get_num_channels(t->mod));
    t->info.orders = (uint32_t)count_of(openmpt_module_get_num_orders(t->mod));
    t->info.patterns = (uint32_t)count_of(openmpt_module_get_num_patterns(t->mod));
    t->info.instruments = (uint32_t)count_of(openmpt_module_get_num_instruments(t->mod));
    t->info.samples = (uint32_t)count_of(openmpt_module_get_num_samples(t->mod));
    t->info.subsongs = (uint32_t)count_of(openmpt_module_get_num_subsongs(t->mod));
    /* The restart point is per subsong; -1 is not accepted here (it throws and
       leaks the stored error string), so ask for the one playing. */
    t->info.restart_order = openmpt_module_get_restart_order(t->mod, t->info.subsong);
    t->info.restart_row = openmpt_module_get_restart_row(t->mod, t->info.subsong);
    t->info.duration = duration;
    s = openmpt_module_get_metadata(t->mod, "message_raw");
    t->message = dup_free(s);
    if (s && !t->message) r = FA_ERROR_OUT_OF_MEMORY;
    if (r == FA_OK) r = load_names(t, FA_TRACKER_TEXT_INSTRUMENT, t->info.instruments);
    if (r == FA_OK) r = load_names(t, FA_TRACKER_TEXT_SAMPLE, t->info.samples);
    if (r == FA_OK) r = load_names(t, FA_TRACKER_TEXT_CHANNEL, t->info.channels);
    if (r == FA_OK) r = load_names(t, FA_TRACKER_TEXT_PATTERN, t->info.patterns);
    if (r == FA_OK) r = load_names(t, FA_TRACKER_TEXT_SUBSONG, t->info.subsongs);
    if (r != FA_OK) { tracker_free(t); return r; }

    dec->impl = t;
    dec->format.sample_rate = t->rate;
    dec->format.channels = 2;
    dec->length = t->length;
    dec->seekable = 1;
    snprintf(dec->codec, sizeof(dec->codec), "%s", t->info.type[0] ? t->info.type : "tracker");
    snprintf(dec->container, sizeof(dec->container), "tracker");
    return FA_OK;
}

const fa_decoder_vtable fa_decoder_tracker = {
    "tracker", "Tracker modules (MOD, S3M, XM, IT, MPTM, MTM, UMX and more; libopenmpt)",
    "mod;s3m;xm;it;mptm;mtm;umx;669;amf;ams;dbm;digi;dmf;dsm;far;gdm;imf;j2b;mdl;med;mo3;mt2;okt;plm;psm;ptm;sfx;stm;ult;wow",
    "audio/x-mod;audio/mod;audio/x-s3m;audio/s3m;audio/x-xm;audio/xm;audio/x-it;audio/it",
    fa_probe_tracker, tracker_open, tracker_read, tracker_seek, tracker_close, 0, NULL, NULL
};

/* ---- Tracker stream API -------------------------------------------------------- */

FA_API fa_result FA_CALL fa_stream_get_tracker_info(fa_stream* stream, fa_tracker_info* out_info)
{
    uint32_t want;
    if (!stream || !out_info || out_info->struct_size < offsetof(fa_tracker_info, duration) + sizeof(double) ||
        !stream->tracker)
        return FA_ERROR_INVALID_ARG;
    want = out_info->struct_size < sizeof(fa_tracker_info) ? out_info->struct_size : (uint32_t)sizeof(fa_tracker_info);
    memcpy(out_info, &stream->tracker->info, want);
    out_info->struct_size = want;
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_tracker_text(fa_stream* stream, fa_tracker_text_type type, uint32_t index,
                                                    char* buffer, size_t capacity, size_t* out_length)
{
    struct fa_tracker* t;
    const char* s = NULL;
    size_t n;
    if (out_length) *out_length = 0;
    if (buffer && capacity) buffer[0] = 0;
    if (!stream || !(t = stream->tracker) || (capacity && !buffer)) return FA_ERROR_INVALID_ARG;
    if (type == FA_TRACKER_TEXT_MESSAGE) {
        if (index != 0) return FA_ERROR_NOT_FOUND;
        s = t->message;
    } else if (type >= FA_TRACKER_TEXT_INSTRUMENT && type <= FA_TRACKER_TEXT_SUBSONG) {
        if (index >= t->name_count[type]) return FA_ERROR_NOT_FOUND;
        s = t->names[type][index];
    } else {
        return FA_ERROR_INVALID_ARG;
    }
    if (!s) s = "";
    n = strlen(s);
    if (out_length) *out_length = n;
    if (capacity) utf8_copy(buffer, capacity, s);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_tracker_time(fa_stream* stream, uint32_t order, uint32_t row, double* out_seconds)
{
    double at;
    if (out_seconds) *out_seconds = -1.0;
    if (!stream || !stream->tracker || !out_seconds) return FA_ERROR_INVALID_ARG;
    if (order >= stream->tracker->info.orders) return FA_ERROR_NOT_FOUND;
    fa_mutex_lock(&stream->decode_lock);
    at = openmpt_module_get_time_at_position(stream->tracker->mod, (int32_t)order, (int32_t)row);
    fa_mutex_unlock(&stream->decode_lock);
    if (!(at >= 0)) return FA_ERROR_NOT_FOUND;   /* never played (skipped, or not a row of that pattern) */
    *out_seconds = at;
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_seek_tracker(fa_stream* stream, uint32_t order, uint32_t row)
{
    double at;
    fa_result r = fa_stream_get_tracker_time(stream, order, row, &at);
    if (r != FA_OK) return r;
    return fa_stream_seek(stream, (fa_frames)llround(at * stream->tracker->rate));
}

FA_API fa_result FA_CALL fa_stream_set_tracker_param(fa_stream* stream, fa_tracker_param param, int32_t value)
{
    struct fa_tracker* t;
    if (!stream || !(t = stream->tracker)) return FA_ERROR_INVALID_ARG;
    switch (param) {
    case FA_TRACKER_INTERPOLATION:
        if (value != 0 && value != 1 && value != 2 && value != 4 && value != 8) return FA_ERROR_INVALID_ARG;
        fa_store32(&t->interpolation, value);
        return FA_OK;
    case FA_TRACKER_STEREO_SEPARATION:
        if (value < 0 || value > 200) return FA_ERROR_INVALID_ARG;
        fa_store32(&t->separation, value);
        return FA_OK;
    case FA_TRACKER_VOLUME_RAMPING:
        if (value < -1 || value > 10) return FA_ERROR_INVALID_ARG;
        fa_store32(&t->ramping, value);
        return FA_OK;
    case FA_TRACKER_AMIGA_RESAMPLER:
        fa_store32(&t->amiga, value ? 1 : 0);
        return FA_OK;
    case FA_TRACKER_ROW_EVENTS:
        if (value < 0 || value > 2) return FA_ERROR_INVALID_ARG;
        fa_store32(&t->row_events, value);
        return FA_OK;
    case FA_TRACKER_AMPLIFY:
        if (value < FA_TRACKER_AMPLIFY_MIN || value > FA_TRACKER_AMPLIFY_MAX) return FA_ERROR_INVALID_ARG;
        fa_store32(&t->amplify, value);
        return FA_OK;
    case FA_TRACKER_CELL_EVENTS:
        if (value < 0 || value > 3) return FA_ERROR_INVALID_ARG;
        fa_store32(&t->cell_events, value);
        return FA_OK;
    default:
        return FA_ERROR_INVALID_ARG;
    }
}

FA_API fa_result FA_CALL fa_stream_get_tracker_param(fa_stream* stream, fa_tracker_param param, int32_t* out_value)
{
    struct fa_tracker* t;
    if (!stream || !out_value || !(t = stream->tracker)) return FA_ERROR_INVALID_ARG;
    switch (param) {
    case FA_TRACKER_INTERPOLATION: *out_value = fa_load32(&t->interpolation); return FA_OK;
    case FA_TRACKER_STEREO_SEPARATION: *out_value = fa_load32(&t->separation); return FA_OK;
    case FA_TRACKER_VOLUME_RAMPING: *out_value = fa_load32(&t->ramping); return FA_OK;
    case FA_TRACKER_AMIGA_RESAMPLER: *out_value = fa_load32(&t->amiga); return FA_OK;
    case FA_TRACKER_ROW_EVENTS: *out_value = fa_load32(&t->row_events); return FA_OK;
    case FA_TRACKER_AMPLIFY: *out_value = fa_load32(&t->amplify); return FA_OK;
    case FA_TRACKER_CELL_EVENTS: *out_value = fa_load32(&t->cell_events); return FA_OK;
    default: return FA_ERROR_INVALID_ARG;
    }
}

FA_API fa_result FA_CALL fa_stream_get_tracker_position(fa_stream* stream, uint32_t* out_order, uint32_t* out_row)
{
    fa_time clock;
    fa_frames frame;
    uint32_t order, row;
    fa_result r;
    if (out_order) *out_order = 0;
    if (out_row) *out_row = 0;
    if (!stream || !stream->tracker || !out_order || !out_row) return FA_ERROR_INVALID_ARG;
    r = fa_stream_get_clock(stream, &clock);
    if (r != FA_OK) return r;
    frame = (fa_frames)(((double)clock * stream->tracker->rate + 5e8) / 1e9);
    if (!fa_tracker_row_at(stream->tracker, frame, &order, &row)) return FA_ERROR_NOT_FOUND;
    *out_order = order;
    *out_row = row;
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_set_tracker_channel_mute(fa_stream* stream, uint32_t channel, int32_t muted)
{
    fa_atomic64* mask;
    int64_t old, bit;
    if (!stream || !stream->tracker || channel >= stream->tracker->info.channels || channel >= FA_TRACKER_MUTE_CHANNELS)
        return FA_ERROR_INVALID_ARG;
    if (!stream->tracker->has_interactive) return FA_ERROR_NOT_IMPLEMENTED;
    mask = &stream->tracker->mute;
    bit = (int64_t)((uint64_t)1 << channel);
    do {
        old = fa_load64(mask);
    } while (!fa_cas64(mask, old, muted ? (old | bit) : (old & ~bit)));
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_tracker_channel_mute(fa_stream* stream, uint32_t channel, int32_t* out_muted)
{
    if (!stream || !out_muted || !stream->tracker || channel >= stream->tracker->info.channels ||
        channel >= FA_TRACKER_MUTE_CHANNELS)
        return FA_ERROR_INVALID_ARG;
    *out_muted = (int32_t)(((uint64_t)fa_load64(&stream->tracker->mute) >> channel) & 1);
    return FA_OK;
}

/* ---- Live module controls ------------------------------------------------------- */

/* These change the module's own state, so they take the stream's decode lock; they are
 * heard after the decode-ahead buffer. */

static struct fa_tracker* live_of(fa_stream* stream) { return stream ? stream->tracker : NULL; }

FA_API fa_result FA_CALL fa_stream_set_tracker_tempo(fa_stream* stream, double bpm)
{
    struct fa_tracker* t = live_of(stream);
    int ok;
    if (!t || !(bpm >= 32.0 && bpm <= 512.0)) return FA_ERROR_INVALID_ARG;
    if (!t->has_interactive3) return FA_ERROR_NOT_IMPLEMENTED;
    fa_mutex_lock(&stream->decode_lock);
    ok = t->interactive3.set_current_tempo2(t->ext, bpm);
    fa_mutex_unlock(&stream->decode_lock);
    return ok ? FA_OK : FA_ERROR_INVALID_ARG;
}

FA_API fa_result FA_CALL fa_stream_get_tracker_tempo(fa_stream* stream, double* out_bpm)
{
    struct fa_tracker* t = live_of(stream);
    if (!t || !out_bpm) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->decode_lock);
    *out_bpm = openmpt_module_get_current_tempo2(t->mod);
    fa_mutex_unlock(&stream->decode_lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_set_tracker_speed(fa_stream* stream, int32_t ticks_per_row)
{
    struct fa_tracker* t = live_of(stream);
    int ok;
    if (!t || ticks_per_row < 1 || ticks_per_row > 255) return FA_ERROR_INVALID_ARG;
    if (!t->has_interactive) return FA_ERROR_NOT_IMPLEMENTED;
    fa_mutex_lock(&stream->decode_lock);
    ok = t->interactive.set_current_speed(t->ext, ticks_per_row);
    fa_mutex_unlock(&stream->decode_lock);
    return ok ? FA_OK : FA_ERROR_INVALID_ARG;
}

FA_API fa_result FA_CALL fa_stream_get_tracker_speed(fa_stream* stream, int32_t* out_ticks_per_row)
{
    struct fa_tracker* t = live_of(stream);
    if (!t || !out_ticks_per_row) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->decode_lock);
    *out_ticks_per_row = openmpt_module_get_current_speed(t->mod);
    fa_mutex_unlock(&stream->decode_lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_set_tracker_global_volume(fa_stream* stream, double volume)
{
    struct fa_tracker* t = live_of(stream);
    int ok;
    if (!t || !(volume >= 0.0 && volume <= 1.0)) return FA_ERROR_INVALID_ARG;
    if (!t->has_interactive) return FA_ERROR_NOT_IMPLEMENTED;
    fa_mutex_lock(&stream->decode_lock);
    ok = t->interactive.set_global_volume(t->ext, volume);
    fa_mutex_unlock(&stream->decode_lock);
    return ok ? FA_OK : FA_ERROR_INVALID_ARG;
}

FA_API fa_result FA_CALL fa_stream_get_tracker_global_volume(fa_stream* stream, double* out_volume)
{
    struct fa_tracker* t = live_of(stream);
    if (!t || !out_volume) return FA_ERROR_INVALID_ARG;
    if (!t->has_interactive) return FA_ERROR_NOT_IMPLEMENTED;
    fa_mutex_lock(&stream->decode_lock);
    *out_volume = t->interactive.get_global_volume(t->ext);
    fa_mutex_unlock(&stream->decode_lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_set_tracker_channel_volume(fa_stream* stream, uint32_t channel, double volume)
{
    struct fa_tracker* t = live_of(stream);
    int ok;
    if (!t || channel >= t->info.channels || channel >= FA_TRACKER_MUTE_CHANNELS || !(volume >= 0.0 && volume <= 1.0))
        return FA_ERROR_INVALID_ARG;
    if (!t->has_interactive) return FA_ERROR_NOT_IMPLEMENTED;
    fa_mutex_lock(&stream->decode_lock);
    ok = t->interactive.set_channel_volume(t->ext, (int32_t)channel, volume);
    if (ok) {
        t->chan_vol[channel] = volume;
        t->chan_vol_set |= (uint64_t)1 << channel;
    }
    fa_mutex_unlock(&stream->decode_lock);
    return ok ? FA_OK : FA_ERROR_INVALID_ARG;
}

FA_API fa_result FA_CALL fa_stream_get_tracker_channel_volume(fa_stream* stream, uint32_t channel, double* out_volume)
{
    struct fa_tracker* t = live_of(stream);
    if (!t || !out_volume || channel >= t->info.channels || channel >= FA_TRACKER_MUTE_CHANNELS) return FA_ERROR_INVALID_ARG;
    if (!t->has_interactive) return FA_ERROR_NOT_IMPLEMENTED;
    fa_mutex_lock(&stream->decode_lock);
    *out_volume = t->interactive.get_channel_volume(t->ext, (int32_t)channel);
    fa_mutex_unlock(&stream->decode_lock);
    return FA_OK;
}

/* Instruments, or the samples of a module without instruments (MOD, S3M). */
static uint32_t instrument_count(const struct fa_tracker* t) { return t->info.instruments ? t->info.instruments : t->info.samples; }

FA_API fa_result FA_CALL fa_stream_set_tracker_instrument_mute(fa_stream* stream, uint32_t instrument, int32_t muted)
{
    struct fa_tracker* t = live_of(stream);
    if (!t || instrument >= instrument_count(t)) return FA_ERROR_INVALID_ARG;
    if (!t->has_interactive) return FA_ERROR_NOT_IMPLEMENTED;
    fa_mutex_lock(&stream->decode_lock);
    t->interactive.set_instrument_mute_status(t->ext, (int32_t)instrument, muted ? 1 : 0);
    fa_mutex_unlock(&stream->decode_lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_tracker_instrument_mute(fa_stream* stream, uint32_t instrument, int32_t* out_muted)
{
    struct fa_tracker* t = live_of(stream);
    if (!t || !out_muted || instrument >= instrument_count(t)) return FA_ERROR_INVALID_ARG;
    if (!t->has_interactive) return FA_ERROR_NOT_IMPLEMENTED;
    fa_mutex_lock(&stream->decode_lock);
    *out_muted = t->interactive.get_instrument_mute_status(t->ext, (int32_t)instrument) ? 1 : 0;
    fa_mutex_unlock(&stream->decode_lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_tracker_orders(fa_stream* stream, uint32_t* patterns, uint32_t capacity,
                                                      uint32_t* out_count)
{
    struct fa_tracker* t = live_of(stream);
    uint32_t i;
    if (out_count) *out_count = 0;
    if (!t || !out_count || (capacity && !patterns)) return FA_ERROR_INVALID_ARG;
    *out_count = t->info.orders;
    for (i = 0; i < capacity && i < t->info.orders; i++) {
        int32_t p = openmpt_module_get_order_pattern(t->mod, (int32_t)i);
        patterns[i] = p < 0 ? 0xFFFFu : (uint32_t)p;
    }
    return FA_OK;
}

/* ---- Exact beat grid ------------------------------------------------------------ */

fa_result fa_tracker_grid_start(struct fa_tracker* t)
{
    if (t->pos != 0 || t->grid_on) return FA_ERROR_INVALID_ARG;
    t->grid_on = 1;
    return FA_OK;
}

/* Rows are only noted to within FA_TRACKER_STEP frames: a row noted at frame F
 * started in [F - STEP, F - 1] (the song's first row at 0). Within a run of rows
 * at one tempo every row lasts the same, exactly known from the tempo, so each
 * noted row narrows down where the run started; a run ends where the tempo
 * changes or the rows stop fitting (a pattern delay). Each run is a segment;
 * beats count rows. A tempo change can land after the first tick of its row (MOD
 * files change it on the second tick, as ProTracker did): the segment then starts
 * at the next row, where the new tempo holds for a whole row. */
fa_result fa_tracker_grid_finish(struct fa_tracker* t, fa_frames length, fa_grid_data* out)
{
    size_t i, n = 0, cap, first = 0;
    double beat = 0.0, run_beat = 0.0, lo = 0.0, hi = 0.0, len = 0.0;
    memset(out, 0, sizeof(*out));
    t->grid_on = 0;
    if (t->grid_failed) return FA_ERROR_OUT_OF_MEMORY;
    if (t->grid_n == 0 || !(t->grid_rows[0].bpm > 0.0)) return FA_ERROR_NOT_FOUND;
    cap = t->grid_n < FA_TRACKER_GRID_MAX ? t->grid_n : FA_TRACKER_GRID_MAX;
    out->segs = (fa_tempo_segment*)malloc(cap * sizeof(fa_tempo_segment));
    if (!out->segs) return FA_ERROR_OUT_OF_MEMORY;
    for (i = 0; i <= t->grid_n; i++) {
        const grid_row* r = i < t->grid_n ? &t->grid_rows[i] : NULL;
        int same = 0;
        double k = 0.0, nlo = 0.0, nhi = 0.0;
        if (r && i > first) {
            const grid_row* f = &t->grid_rows[first];
            k = (double)(i - first);
            nlo = (double)(r->frame - FA_TRACKER_STEP) - k * len;
            nhi = (double)(r->frame - 1) - k * len;
            if (nlo < lo) nlo = lo;
            if (nhi > hi) nhi = hi;
            same = r->bpm == f->bpm && r->rows_per_beat == f->rows_per_beat && nlo <= nhi + 2.0;
        }
        if (same) {
            lo = nlo;
            hi = nhi;
        } else {
            if (i > 0) {   /* the run that ends here */
                if (n == cap) break;
                out->segs[n].start = (fa_frames)llround((lo + hi) / 2.0);
                out->segs[n].bpm = t->grid_rows[first].bpm;
                out->segs[n].beat = run_beat;
                n++;
            }
            if (!r) break;
            first = i;
            run_beat = beat;
            len = r->bpm > 0.0 ? 60.0 / (r->bpm * r->rows_per_beat) * t->rate : 0.0;
            lo = i == 0 ? 0.0 : (double)(r->frame - FA_TRACKER_STEP);
            hi = i == 0 ? 0.0 : (double)(r->frame - 1);
        }
        if (r) beat += 1.0 / r->rows_per_beat;
    }
    /* A row without a tempo (cannot happen with libopenmpt's estimate) ends it all. */
    while (n > 0 && !(out->segs[n - 1].bpm > 0.0)) n--;
    if (n == 0) { fa_grid_data_free(out); return FA_ERROR_NOT_FOUND; }
    out->grid.struct_size = sizeof(fa_beat_grid);
    out->grid.sample_rate = t->rate;
    out->grid.first_beat = out->segs[0].start;
    out->grid.beats_per_bar = 4;
    out->grid.confidence = 1.0f;
    out->grid.flags = FA_GRID_EXACT;
    out->grid.segment_count = (uint32_t)n;
    fa_grid_data_finish(out, length);
    return FA_OK;
}

fa_result fa_tracker_grid_render(fa_decoder* dec, fa_grid_data* out, fa_atomic32* cancel)
{
    struct fa_tracker* t = fa_tracker_of(dec);
    float* buf;
    fa_frames got;
    fa_result r;
    memset(out, 0, sizeof(*out));
    if (!t) return FA_ERROR_INVALID_ARG;
    r = fa_tracker_grid_start(t);
    if (r != FA_OK) return r;
    /* The timing does not depend on how samples are interpolated: the cheapest. */
    fa_store32(&t->interpolation, 1);
    buf = (float*)malloc(4096 * 2 * sizeof(float));
    if (!buf) return FA_ERROR_OUT_OF_MEMORY;
    while (tracker_read(dec, buf, 4096, &got) == FA_OK && got > 0)
        if (cancel && fa_load32(cancel)) break;
    free(buf);
    if (cancel && fa_load32(cancel)) return FA_ERROR_BUSY;
    return fa_tracker_grid_finish(t, t->length, out);
}
