#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L    /* opendir/readdir under strict C11 */
#endif
/* CD+G karaoke graphics: the pack decoder, surfaces that draw it into an app's
 * pixel format, and the glue that keeps it on the stream's audio clock.
 *
 * Written from the public CD+G description (Red Book subcode channels R..W:
 * 24-byte packs, 300 per second, mode 1 "TV graphics"). A .cdg file is those packs
 * one after another, so pack n is shown at n / 300 s of the audio.
 *
 * Every surface decodes on its own (a 300x216 index screen per surface) up to the
 * pack for the time it is asked for. Moving forward applies the new packs. The
 * stream's graphics carry checkpoints, the decoder state every 10 s of packs, shared
 * by all its surfaces: built in full when a file's first surface is created, and as
 * the packs arrive for discs. A seek anywhere, back or forward, the first one too, restores the last
 * checkpoint before the target and replays at most 10 s of packs, a fraction of a
 * millisecond. Updates run on the caller's thread under the stream's cdg_lock; the
 * audio thread never sees any of it. */
#include "cdg.h"
#include "cdg_scale.h"
#include "lyric.h"
#include "surface_fx.h"
#include "surface.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#  include <dirent.h>
#endif

#define CDG_MODE_GRAPHICS 9
#define CDG_MEMORY_PRESET 1
#define CDG_BORDER_PRESET 2
#define CDG_TILE_NORMAL 6
#define CDG_SCROLL_PRESET 20
#define CDG_SCROLL_COPY 24
#define CDG_TRANSPARENT 28
#define CDG_COLORS_LOW 30
#define CDG_COLORS_HIGH 31
#define CDG_TILE_XOR 38

#define CDG_TILE_W 6
#define CDG_TILE_H 12
#define CDG_ROWS 18
#define CDG_COLS 50
#define CDG_BORDER_X 6
#define CDG_BORDER_Y 12
#define CDG_MAX_OUTPUT 8192
#define CDG_FX_HELPERS 3           /* engine workers that help draw the effects, at most */
#define CDG_FX_PIECES 384          /* pieces of the effects shared out at once */
#define CDG_FX_SHARE (128 * 128)   /* output pixels of effects below which they are not shared */
#define CDG_KEEP_BLOCK 24          /* screen pixels a block of a colour-only effects update spans */
#define CDG_CHECKPOINT_PACKS (10 * FA_CDG_PACKS_PER_SECOND)
#define CDG_CONFIG_V1_SIZE 20       /* fa_cdg_surface_config up to `flags` */
#define CDG_ALL_FLAGS (FA_CDG_NO_BORDER | FA_CDG_TRANSPARENT | FA_CDG_PREMULTIPLIED | FA_CDG_SMOOTH | \
                       FA_CDG_TRANSPARENT_BACKGROUND_ONLY | FA_CDG_HIDE_UNTIL_PRESET | \
                       FA_CDG_BACKGROUND_COLOR | FA_CDG_BACKGROUND_OPACITY)
#define CDG_REPEAT_SPAN 32        /* packs: the copies of one memory preset arrive within this */
#define CDG_MAX_CHECKPOINTS 180   /* ~6 MB; longer graphics space them out further */
/* fa_event.value of FA_EVENT_CDG_CHANGED; the header's FA_CDG_CHANGE_* take it apart. */
#define FA_CDG_CHANGE_MAKE(x, y, w, h, pal) \
    ((int64_t)(x) | ((int64_t)(y) << 12) | ((int64_t)(w) << 24) | ((int64_t)(h) << 36) | ((int64_t)((pal) != 0) << 48))

typedef struct cdg_state {
    uint8_t screen[FA_CDG_HEIGHT][FA_CDG_WIDTH];  /* colour indices */
    uint8_t rgb[16][3];
    uint8_t transparency[16];     /* 0 = opaque .. 63 = invisible (transparent colour packs) */
    uint8_t bg;                   /* colour of the last memory preset */
    uint8_t hoff, voff;           /* smooth-scroll offsets, 0..5 and 0..11 */
    int8_t preset;                /* a memory preset was applied (bg is real) */
    int8_t border;                /* colour of the last border preset, -1 = none */
    int clean;                    /* nothing drawn since the last memory preset */
    int8_t repeat;                /* repeat count of the last memory preset */
    size_t preset_at;             /* pack index of the last memory preset */
    size_t next;                  /* packs applied so far */
    fa_rects dirty;               /* screen area changed since the last render, after */
    int shift_x, shift_y;         /* the whole screen moved this far since then (scrolls) */
} cdg_state;

/* A saved cdg_state: colour indices are 4 bits, so two share a byte (the left one in
 * the low nibble), half the memory of a full state. The changed area is not kept. */
typedef struct cdg_checkpoint {
    uint8_t screen[FA_CDG_HEIGHT][FA_CDG_WIDTH / 2];
    uint8_t rgb[16][3];
    uint8_t transparency[16];
    uint8_t bg, hoff, voff;
    int8_t preset, border, repeat;
    int clean;
    size_t preset_at;
    size_t next;
} cdg_checkpoint;

/* Checkpoints of one fa_cdg_data, under the stream's cdg_lock once attached. */
struct fa_cdg_index {
    size_t step;                  /* packs between checkpoints */
    cdg_checkpoint* at;           /* [k] = state after (k + 1) * step packs */
    size_t count;                 /* valid checkpoints */
    size_t cap;
    cdg_state build;              /* discs: the decoder running over the packs written */
    size_t high;                  /* discs: packs written up to here; the decoder
                                     catches up with it a little at each write */
    /* The safety-zone pre-pass (files), from the same walk: */
    uint8_t ink[CDG_ROWS][CDG_COLS]; /* tiles now holding something other than the background */
    size_t border_ink;            /* 1 + the last pack that left that in a border tile; 0 = none */
    int box[4];                   /* tiles that ever held it: col0, row0, col1, row1 (empty: 0s) */
};

/* A surface's layout: what its buffer holds and where (layout_want, layout_apply). */
typedef struct cdg_layout {
    uint32_t crop[4];             /* x, y, width, height: the part of the screen shown */
    int zone, resolved, scaled;
    uint32_t n;                   /* scaled: the multiple */
    uint32_t pic_w, pic_h;        /* the picture, in buffer pixels */
    uint32_t pic_x, pic_y;        /* its top-left in the buffer (padding and margin) */
    uint32_t buf_w, buf_h;        /* the whole buffer */
    uint32_t pad;                 /* the padding alone, in buffer pixels */
} cdg_layout;

/* Render-ahead: a finished frame in a buffer of the layout's size. */
typedef struct cdg_slot {
    uint8_t* px;                  /* stride: the surface's */
    uint64_t revision;            /* the picture it holds, 0 = none */
    uint32_t gen;                 /* stream->cdg_gen it was decoded from */
    int stale;                    /* drawn from disc packs since rewritten */
    size_t target;                /* packs applied */
    fa_time time;                 /* graphics time */
    int64_t render_ns;
    uint8_t pal[16][4];
    int32_t bg, border;
    uint8_t background[4];
    uint8_t transparency[16];
    uint64_t palette_revision;
    /* A lyric surface's frame: its layout and tempo, and the history as of it. */
    uint64_t layout_rev;
    float tempo;
    fa_surface_history hist;
} cdg_slot;

struct fa_cdg_surface {
    struct fa_stream* stream;
    struct fa_cdg_surface* next;  /* stream->cdg_surfaces, under cdg_lock */
    fa_cdg_pixel_format format;
    uint32_t flags;
    uint32_t cfg_w, cfg_h;        /* the config's output size (0 = the crop's) */
    int has_pres;                 /* fa_cdg_surface_set_presentation gave options */
    fa_cdg_presentation pres;
    int tb;                       /* FA_CDG_TRANSPARENT_BORDER */
    cdg_layout lay;               /* in use; lay.buf_w 0 before the first */
    fa_atomic64 layout_revision;  /* written under work_lock; update_into reads it without */
    uint32_t width, height;       /* the picture's output size (= lay.pic_w, pic_h) */
    uint32_t win_x, win_y, win_w, win_h; /* the part of the screen shown (= lay.crop) */
    uint8_t* pixels;              /* fa_cdg_surface_update's picture; made by its first call */
    int32_t stride;               /* of a row of the buffer, and the least update_into takes */
    uint64_t pixels_revision;     /* the revision `pixels` holds (0 = none) */
    uint8_t pixels_palette[16][4]; /* the palette of fa_cdg_surface_update's last frame */
    int pixels_drawn;
    fa_surface_history history;   /* the output area each recent revision changed */
    uint32_t present_w, present_h; /* fa_surface_set_presentation_size */
    int render_ahead;
    uint32_t ra_w, ra_h;          /* lyric render-ahead: the slots' size and layout */
    uint64_t ra_layout;
    uint32_t gen;                 /* stream->cdg_gen the state belongs to */
    int drawn;                    /* a revision exists: the state was turned into pixels once */
    uint8_t shown_palette[16][4]; /* as of the last render */
    uint8_t shown_transparency[16];
    int shown_bg, shown_border;
    uint8_t shown_hoff, shown_voff;
    uint64_t revision, palette_revision;
    uint32_t bg_color;            /* config.background_color, 0xAARRGGBB */
    uint32_t bg_opacity;          /* config.background_opacity, % */
    uint16_t* xmap;               /* output column -> window column (sharp scaling) */
    uint16_t* ymap;               /* output row -> window row */
    uint32_t* xlerp;              /* smooth scaling: output column -> lerp_map */
    uint32_t* ylerp;              /* output row -> lerp_map */
    cdg_state st;
    size_t stale_from;            /* packs from here were rewritten (SIZE_MAX = none), under cdg_lock */
    /* Presentation scaling: the screen area [mx0, mx0 + mw) x [my0, my0 + mh) drawn at
     * sc.n (the crop, and what smooth-scroll offsets can bring into it), from which
     * update_into copies. */
    cdg_scaler sc;
    uint8_t* master;
    size_t master_stride;
    int mx0, my0, mw, mh;
    int master_tb;                /* its border was drawn transparent */
    uint8_t master_pm[CDG_SCALE_COLORS][4]; /* the colours it was drawn with */
    uint8_t eidx[FA_CDG_HEIGHT][FA_CDG_WIDTH]; /* the screen as drawn: 16 = transparent border */
    uint16_t* rows;               /* the scaler's scratch rows (cdg_scale_rect) */
    /* Effects (fa_surface_set_effects), with presentation scaling: drawn from the
     * master into fxm, which update_into then copies from. */
    int has_fx;
    fa_surface_effects fx;
    int fx_on;                    /* fxm holds the master with the effects */
    int fx_wide;                  /* ... computed over the whole master (smooth-scroll offsets) */
    uint8_t* fxm;
    void* fx_scratch;
    size_t fx_scratch_size;
    /* What the effects worked out that does not depend on the colours (for fxm, or f2
     * with the 2N pass): where the shape stays, a change of colours is a blend. */
    fa_fx_keep keep;
    fa_fx_plan keep_plan;         /* the plan it holds */
    int keep_ok;                  /* it holds all of the effects shown */
    /* Engine workers help draw the effects (fx_run), each with scratch of its own. */
    fa_engine* eng;               /* the stream's, set by sync_state */
    void* fx_help[CDG_FX_HELPERS];
    size_t fx_help_size[CDG_FX_HELPERS];
    uint16_t* rows_help[CDG_FX_HELPERS]; /* ... and to draw the master (scale_draw) */
    size_t rows_help_size[CDG_FX_HELPERS];
    /* At N 1-3 the effects are worked out at 2N and box-filtered down (plan 3.3): m2
     * is the master drawn at 2N (by sc2), f2 its effects. */
    int x2;
    cdg_scaler sc2;
    uint8_t *m2, *f2;
    size_t m2_stride;
    uint16_t* rows2;
    /* Render-ahead with presentation scaling: an engine worker renders the next frame
     * into one slot while update_into copies from the one shown. work_lock guards all
     * of the surface but what pub_lock does; every surface call and a worker's job hold
     * it. pub_lock: which slot is which, ra_time, the history and revision. */
    fa_mutex work_lock, pub_lock;
    cdg_slot slot[2];
    int front, ready;             /* the slot shown, the one finished next; -1 = none */
    fa_time ra_time;              /* the stream time the worker renders for */
    fa_time ra_last;              /* update_into's last time */
    struct fa_cdg_surface* ra_next; /* engine->cdg_render, under g_bind */
    int ra_queued;                /* under g_bind */
    fa_atomic32 ra_running;       /* workers that took the surface off the queue, not done */
    /* A MIDI lyric surface (lyric.c) instead of CD+G: on the stream's surface list
     * like the others, so closing the stream detaches it too; the CD+G state above
     * stays unused. */
    struct fa_lyric* lyric;
};

/* ---- data --------------------------------------------------------------------- */

fa_result fa_cdg_data_adopt(uint8_t* bytes, size_t size, fa_cdg_data** out)
{
    fa_cdg_data* d;
    *out = NULL;
    if (size < FA_CDG_PACK_BYTES) { free(bytes); return FA_ERROR_UNSUPPORTED_FORMAT; }
    d = (fa_cdg_data*)malloc(sizeof(*d));
    if (!d) { free(bytes); return FA_ERROR_OUT_OF_MEMORY; }
    memset(d, 0, sizeof(*d));
    d->packs = bytes;
    d->count = size / FA_CDG_PACK_BYTES;
    *out = d;
    return FA_OK;
}

static void index_free(struct fa_cdg_index* ix);

void fa_cdg_data_free(fa_cdg_data* data)
{
    if (!data) return;
    index_free(data->index);
    free(data->packs);
    free(data);
}

fa_result fa_cdg_data_from_path(const char* path, fa_cdg_data** out)
{
    fa_source src;
    uint8_t* bytes;
    size_t size;
    fa_result r;
    *out = NULL;
    r = fa_source_open_path(&src, path);
    if (r != FA_OK) return r;
    /* A song's .cdg is a few MB (300 packs of 24 bytes per second). */
    r = fa_source_read_all(&src, (size_t)256 << 20, &bytes, &size);
    fa_source_close(&src);
    if (r != FA_OK) return r;
    return fa_cdg_data_adopt(bytes, size, out);
}

static int ends_with_cdg(const char* name, size_t len)
{
    return len >= 4 && name[len - 4] == '.' &&
           (name[len - 3] | 0x20) == 'c' && (name[len - 2] | 0x20) == 'd' && (name[len - 1] | 0x20) == 'g';
}

int fa_cdg_is_cdg_name(const char* name) { return ends_with_cdg(name, strlen(name)); }

#if !defined(_WIN32)
/* Case-sensitive file systems: any spelling of the extension ("song.cDg"). */
static fa_cdg_data* find_any_case(const char* stem_path, size_t stem)
{
    size_t dir_len = stem, base_len;
    const char* base;
    char* dir;
    DIR* d;
    struct dirent* e;
    fa_cdg_data* data = NULL;
    while (dir_len > 0 && stem_path[dir_len - 1] != '/') dir_len--;
    base = stem_path + dir_len;
    base_len = stem - dir_len;
    dir = (char*)malloc(dir_len + 2);
    if (!dir) return NULL;
    if (dir_len) memcpy(dir, stem_path, dir_len); else dir[dir_len++] = '.';
    dir[dir_len] = '\0';
    d = opendir(dir);
    free(dir);
    if (!d) return NULL;
    while (!data && (e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n == base_len + 4 && memcmp(e->d_name, base, base_len) == 0 && ends_with_cdg(e->d_name, n)) {
            char* p = (char*)malloc(stem + 5);
            if (!p) break;
            memcpy(p, stem_path, stem);
            memcpy(p + stem, e->d_name + base_len, 5);
            if (fa_cdg_data_from_path(p, &data) != FA_OK) data = NULL;
            free(p);
        }
    }
    closedir(d);
    return data;
}
#endif

fa_cdg_data* fa_cdg_find_sibling(const char* audio_path)
{
    static const char* const exts[] = { ".cdg", ".CDG" };
    size_t len = strlen(audio_path), stem = len, i;
    char* p;
    fa_cdg_data* data = NULL;
    if (ends_with_cdg(audio_path, len)) return NULL;
    while (stem > 0 && audio_path[stem - 1] != '/' && audio_path[stem - 1] != '\\' && audio_path[stem - 1] != '.') stem--;
    if (stem == 0 || audio_path[stem - 1] != '.') stem = len + 1; /* no extension: append */
    stem--;
    p = (char*)malloc(stem + 5);
    if (!p) return NULL;
    memcpy(p, audio_path, stem);
    /* Windows and macOS ignore case, so the first try finds any spelling there. */
    for (i = 0; i < sizeof(exts) / sizeof(exts[0]) && !data; i++) {
        memcpy(p + stem, exts[i], 5);
        if (fa_cdg_data_from_path(p, &data) != FA_OK) data = NULL;
    }
#if !defined(_WIN32)
    if (!data) data = find_any_case(p, stem);
#endif
    free(p);
    return data;
}

/* ---- the pack decoder ------------------------------------------------------------ */

static void mark(cdg_state* st, int x0, int y0, int x1, int y1) { fa_rects_add(&st->dirty, x0, y0, x1, y1); }

static void mark_all(cdg_state* st) { mark(st, 0, 0, FA_CDG_WIDTH, FA_CDG_HEIGHT); }

static void state_reset(cdg_state* st)
{
    memset(st, 0, sizeof(*st));
    st->border = -1;
    st->clean = 1;
    mark_all(st);
}

/* Returns 0 for a repeat that was skipped. */
static int memory_preset(cdg_state* st, const uint8_t* d)
{
    uint8_t color = d[0] & 0x0F;
    int8_t repeat = (int8_t)(d[1] & 0x0F);
    /* Discs send a preset several times (repeat count 0, 1, 2.. in d[1]) in case one
     * copy is damaged: one clear, not several. A copy of the preset just applied (a
     * higher count, a few packs later) is skipped even if something was drawn between
     * the copies; so is any repeat of a screen that is still clean. The window is
     * short, so a later clear whose first copies were lost on a disc still clears. */
    if (repeat != 0 && st->preset && st->bg == color &&
        (st->clean || (repeat > st->repeat && st->next - st->preset_at <= CDG_REPEAT_SPAN))) {
        st->repeat = repeat;
        st->preset_at = st->next;
        return 0;
    }
    memset(st->screen, color, sizeof(st->screen));
    st->bg = color;
    st->preset = 1;
    st->clean = 1;
    st->repeat = repeat;
    st->preset_at = st->next;
    mark_all(st);
    return 1;
}

static void border_preset(cdg_state* st, const uint8_t* d)
{
    uint8_t color = d[0] & 0x0F;
    int y;
    for (y = 0; y < FA_CDG_HEIGHT; y++) {
        if (y < CDG_BORDER_Y || y >= FA_CDG_HEIGHT - CDG_BORDER_Y) {
            memset(st->screen[y], color, FA_CDG_WIDTH);
        } else {
            memset(st->screen[y], color, CDG_BORDER_X);
            memset(st->screen[y] + FA_CDG_WIDTH - CDG_BORDER_X, color, CDG_BORDER_X);
        }
    }
    st->border = (int8_t)color;
    st->clean = 0;
    mark_all(st);
}

static void tile(cdg_state* st, const uint8_t* d, int xor_mode)
{
    uint8_t c0 = d[0] & 0x0F, c1 = d[1] & 0x0F;
    int row = d[2] & 0x1F, col = d[3] & 0x3F, x0, y0, i, j;
    if (row >= CDG_ROWS || col >= CDG_COLS) return;
    x0 = col * CDG_TILE_W;
    y0 = row * CDG_TILE_H;
    for (i = 0; i < CDG_TILE_H; i++) {
        uint8_t bits = d[4 + i] & 0x3F;
        uint8_t* line = st->screen[y0 + i] + x0;
        for (j = 0; j < CDG_TILE_W; j++) {
            uint8_t c = (bits >> (5 - j)) & 1 ? c1 : c0;
            line[j] = xor_mode ? (uint8_t)((line[j] ^ c) & 0x0F) : c;
        }
    }
    st->clean = 0;
    mark(st, x0, y0, x0 + CDG_TILE_W, y0 + CDG_TILE_H);
}

/* The whole screen moved by (dx, dy): so does what was marked as changed, and the
 * band that came in is marked. A surface that keeps its picture moves it too. */
static void moved(cdg_state* st, int dx, int dy)
{
    fa_rects old = st->dirty;
    int i;
    fa_rects_clear(&st->dirty);
    for (i = 0; i < old.n; i++) {
        int x0 = old.r[i][0] + dx, y0 = old.r[i][1] + dy, x1 = old.r[i][2] + dx, y1 = old.r[i][3] + dy;
        mark(st, x0 < 0 ? 0 : x0, y0 < 0 ? 0 : y0, x1 > FA_CDG_WIDTH ? FA_CDG_WIDTH : x1,
             y1 > FA_CDG_HEIGHT ? FA_CDG_HEIGHT : y1);
    }
    if (dx > 0) mark(st, 0, 0, dx, FA_CDG_HEIGHT);
    if (dx < 0) mark(st, FA_CDG_WIDTH + dx, 0, FA_CDG_WIDTH, FA_CDG_HEIGHT);
    if (dy > 0) mark(st, 0, 0, FA_CDG_WIDTH, dy);
    if (dy < 0) mark(st, 0, FA_CDG_HEIGHT + dy, FA_CDG_WIDTH, FA_CDG_HEIGHT);
    st->shift_x += dx;
    st->shift_y += dy;
}

/* Scroll by a tile: the whole screen moves; what leaves one side comes back on the
 * other (copy) or is filled with `color` (preset). */
static void scroll(cdg_state* st, const uint8_t* d, int copy)
{
    uint8_t color = d[0] & 0x0F;
    int hcmd = (d[1] >> 4) & 3, vcmd = (d[2] >> 4) & 3;
    uint8_t hoff = d[1] & 0x07, voff = d[2] & 0x0F;
    int y;
    if (hoff > CDG_TILE_W - 1) hoff = CDG_TILE_W - 1;
    if (voff > CDG_TILE_H - 1) voff = CDG_TILE_H - 1;
    if (hcmd == 1 || hcmd == 2) {
        for (y = 0; y < FA_CDG_HEIGHT; y++) {
            uint8_t* line = st->screen[y];
            uint8_t edge[CDG_TILE_W];
            if (hcmd == 1) { /* right */
                memcpy(edge, line + FA_CDG_WIDTH - CDG_TILE_W, CDG_TILE_W);
                memmove(line + CDG_TILE_W, line, FA_CDG_WIDTH - CDG_TILE_W);
                if (copy) memcpy(line, edge, CDG_TILE_W); else memset(line, color, CDG_TILE_W);
            } else {         /* left */
                memcpy(edge, line, CDG_TILE_W);
                memmove(line, line + CDG_TILE_W, FA_CDG_WIDTH - CDG_TILE_W);
                if (copy) memcpy(line + FA_CDG_WIDTH - CDG_TILE_W, edge, CDG_TILE_W);
                else memset(line + FA_CDG_WIDTH - CDG_TILE_W, color, CDG_TILE_W);
            }
        }
    }
    if (vcmd == 1 || vcmd == 2) {
        uint8_t edge[CDG_TILE_H][FA_CDG_WIDTH];
        size_t band = sizeof(edge), rest = (size_t)(FA_CDG_HEIGHT - CDG_TILE_H) * FA_CDG_WIDTH;
        if (vcmd == 1) { /* down */
            memcpy(edge, st->screen[FA_CDG_HEIGHT - CDG_TILE_H], band);
            memmove(st->screen[CDG_TILE_H], st->screen[0], rest);
            if (copy) memcpy(st->screen[0], edge, band); else memset(st->screen[0], color, band);
        } else {         /* up */
            memcpy(edge, st->screen[0], band);
            memmove(st->screen[0], st->screen[CDG_TILE_H], rest);
            if (copy) memcpy(st->screen[FA_CDG_HEIGHT - CDG_TILE_H], edge, band);
            else memset(st->screen[FA_CDG_HEIGHT - CDG_TILE_H], color, band);
        }
    }
    if (hcmd == 1 || hcmd == 2 || vcmd == 1 || vcmd == 2) {
        st->clean = 0;
        moved(st, hcmd == 1 ? CDG_TILE_W : hcmd == 2 ? -CDG_TILE_W : 0,
              vcmd == 1 ? CDG_TILE_H : vcmd == 2 ? -CDG_TILE_H : 0);
    }
    st->hoff = hoff;
    st->voff = voff;
}

static void load_colors(cdg_state* st, const uint8_t* d, int base)
{
    int i;
    for (i = 0; i < 8; i++) {
        /* 12-bit colour over two 6-bit bytes: rrrrgg ggbbbb */
        uint8_t hi = d[2 * i] & 0x3F, lo = d[2 * i + 1] & 0x3F;
        uint8_t r = (uint8_t)((hi >> 2) & 0x0F);
        uint8_t g = (uint8_t)(((hi & 0x03) << 2) | ((lo >> 4) & 0x03));
        uint8_t b = (uint8_t)(lo & 0x0F);
        uint8_t* c = st->rgb[base + i];
        c[0] = (uint8_t)(r * 17); c[1] = (uint8_t)(g * 17); c[2] = (uint8_t)(b * 17);
    }
}

/* Returns the instruction applied, 0 for none (or a skipped repeat of a clear). */
static int apply_pack(cdg_state* st, const uint8_t* pack)
{
    const uint8_t* d = pack + 4; /* command, instruction, 2 parity bytes, 16 data bytes */
    int i, op = pack[1] & 0x3F;
    if ((pack[0] & 0x3F) != CDG_MODE_GRAPHICS) return 0;
    switch (op) {
    case CDG_MEMORY_PRESET: if (!memory_preset(st, d)) op = 0; break;
    case CDG_BORDER_PRESET: border_preset(st, d); break;
    case CDG_TILE_NORMAL: tile(st, d, 0); break;
    case CDG_TILE_XOR: tile(st, d, 1); break;
    case CDG_SCROLL_PRESET: scroll(st, d, 0); break;
    case CDG_SCROLL_COPY: scroll(st, d, 1); break;
    case CDG_TRANSPARENT:
        for (i = 0; i < 16; i++) st->transparency[i] = d[i] & 0x3F;
        break;
    case CDG_COLORS_LOW: load_colors(st, d, 0); break;
    case CDG_COLORS_HIGH: load_colors(st, d, 8); break;
    default: op = 0; break;
    }
    return op;
}

/* ---- checkpoints ------------------------------------------------------------------ */

static void checkpoint_save(cdg_checkpoint* c, const cdg_state* st)
{
    int y, x;
    for (y = 0; y < FA_CDG_HEIGHT; y++)
        for (x = 0; x < FA_CDG_WIDTH / 2; x++)
            c->screen[y][x] = (uint8_t)(st->screen[y][2 * x] | st->screen[y][2 * x + 1] << 4);
    memcpy(c->rgb, st->rgb, sizeof(c->rgb));
    memcpy(c->transparency, st->transparency, sizeof(c->transparency));
    c->bg = st->bg;
    c->hoff = st->hoff;
    c->voff = st->voff;
    c->preset = st->preset;
    c->border = st->border;
    c->clean = st->clean;
    c->repeat = st->repeat;
    c->preset_at = st->preset_at;
    c->next = st->next;
}

static void checkpoint_row(const cdg_checkpoint* c, int y, uint8_t* row)
{
    int x;
    for (x = 0; x < FA_CDG_WIDTH / 2; x++) {
        row[2 * x] = c->screen[y][x] & 0x0F;
        row[2 * x + 1] = c->screen[y][x] >> 4;
    }
}

/* The state saved in `c`, with nothing marked as changed. */
static void checkpoint_load(const cdg_checkpoint* c, cdg_state* st)
{
    int y;
    for (y = 0; y < FA_CDG_HEIGHT; y++) checkpoint_row(c, y, st->screen[y]);
    memcpy(st->rgb, c->rgb, sizeof(st->rgb));
    memcpy(st->transparency, c->transparency, sizeof(st->transparency));
    st->bg = c->bg;
    st->hoff = c->hoff;
    st->voff = c->voff;
    st->preset = c->preset;
    st->border = c->border;
    st->clean = c->clean;
    st->repeat = c->repeat;
    st->preset_at = c->preset_at;
    st->next = c->next;
    fa_rects_clear(&st->dirty);
    st->shift_x = st->shift_y = 0;
}

static void index_free(struct fa_cdg_index* ix)
{
    if (!ix) return;
    free(ix->at);
    free(ix);
}

/* 10 s of packs, more for graphics longer than CDG_MAX_CHECKPOINTS of those. */
static size_t index_step(size_t count)
{
    size_t per = (size_t)CDG_CHECKPOINT_PACKS * CDG_MAX_CHECKPOINTS;
    return CDG_CHECKPOINT_PACKS * (count > per ? (count + per - 1) / per : 1);
}

static struct fa_cdg_index* index_new(size_t count)
{
    struct fa_cdg_index* ix = (struct fa_cdg_index*)calloc(1, sizeof(*ix));
    if (!ix) return NULL;
    ix->step = index_step(count);
    ix->cap = count / ix->step;
    if (ix->cap) {
        ix->at = (cdg_checkpoint*)malloc(ix->cap * sizeof(cdg_checkpoint));
        if (!ix->at) { free(ix); return NULL; }
    }
    state_reset(&ix->build);
    return ix;
}

/* ---- the safety-zone pre-pass ------------------------------------------------------ */

static int border_tile(int row, int col) { return row == 0 || row == CDG_ROWS - 1 || col == 0 || col == CDG_COLS - 1; }

/* Tile (row, col) was drawn, or moved: record whether it holds something other than
 * the background. */
static void ink_note(struct fa_cdg_index* ix, int row, int col, int ink)
{
    int* b = ix->box;
    ix->ink[row][col] = (uint8_t)ink;
    if (!ink) return;
    if (border_tile(row, col)) ix->border_ink = ix->build.next + 1;
    if (b[0] >= b[2]) { b[0] = col; b[1] = row; b[2] = col + 1; b[3] = row + 1; return; }
    if (col < b[0]) b[0] = col;
    if (row < b[1]) b[1] = row;
    if (col + 1 > b[2]) b[2] = col + 1;
    if (row + 1 > b[3]) b[3] = row + 1;
}

static int tile_ink(const cdg_state* st, int row, int col)
{
    int y, x;
    for (y = row * CDG_TILE_H; y < (row + 1) * CDG_TILE_H; y++)
        for (x = col * CDG_TILE_W; x < (col + 1) * CDG_TILE_W; x++)
            if (st->screen[y][x] != st->bg) return 1;
    return 0;
}

/* After the index's decoder applied instruction `op` (data `d`): what the AUTO and
 * SUPER safety zones need. Drawing is tiles and scrolls; a border preset's solid
 * colour does not count, so tiles are followed rather than pixels compared. */
static void prepass(struct fa_cdg_index* ix, const cdg_state* st, const uint8_t* d, int op)
{
    switch (op) {
    case CDG_MEMORY_PRESET:
        memset(ix->ink, 0, sizeof(ix->ink));
        break;
    case CDG_TILE_NORMAL:
    case CDG_TILE_XOR: {
        int row = d[2] & 0x1F, col = d[3] & 0x3F;
        if (row < CDG_ROWS && col < CDG_COLS) ink_note(ix, row, col, tile_ink(st, row, col));
        break;
    }
    case CDG_SCROLL_PRESET:
    case CDG_SCROLL_COPY: {
        int hcmd = (d[1] >> 4) & 3, vcmd = (d[2] >> 4) & 3, r, c;
        uint8_t was[CDG_ROWS][CDG_COLS], fill = (uint8_t)((d[0] & 0x0F) != st->bg);
        if (hcmd != 1 && hcmd != 2 && vcmd != 1 && vcmd != 2) break;
        memcpy(was, ix->ink, sizeof(was));
        for (r = 0; r < CDG_ROWS; r++)
            for (c = 0; c < CDG_COLS; c++) {
                int fr = vcmd == 1 ? r - 1 : vcmd == 2 ? r + 1 : r, fc = hcmd == 1 ? c - 1 : hcmd == 2 ? c + 1 : c;
                int wrapped = fr < 0 || fr >= CDG_ROWS || fc < 0 || fc >= CDG_COLS;
                fr = (fr + CDG_ROWS) % CDG_ROWS;
                fc = (fc + CDG_COLS) % CDG_COLS;
                ink_note(ix, r, c, wrapped && op == CDG_SCROLL_PRESET ? fill : was[fr][fc]);
            }
        break;
    }
    default:
        break;
    }
}

/* Runs the index's decoder over packs [build.next, upto), saving each checkpoint. */
static void index_advance(struct fa_cdg_index* ix, const fa_cdg_data* d, size_t upto)
{
    cdg_state* st = &ix->build;
    while (st->next < upto) {
        const uint8_t* p = d->packs + st->next * FA_CDG_PACK_BYTES;
        int op = apply_pack(st, p);
        if (op) prepass(ix, st, p + 4, op);
        st->next++;
        if (st->next % ix->step == 0 && st->next / ix->step == ix->count + 1 && ix->count < ix->cap) {
            fa_rects_clear(&st->dirty);
            st->shift_x = st->shift_y = 0;
            checkpoint_save(&ix->at[ix->count++], st);
        }
    }
}

/* Files and memory blocks: every checkpoint at once, when the stream's first surface
 * needs them (under cdg_lock, a few milliseconds for a song), so opening songs that
 * are never drawn costs nothing. Without memory for it, seeks replay from the start. */
static void index_build(fa_cdg_data* d)
{
    struct fa_cdg_index* ix;
    if (!d || d->index || d->from_disc) return;
    ix = index_new(d->count);
    if (!ix) return;
    index_advance(ix, d, d->count);
    d->index = ix;
}

/* Discs: packs [first, first + n) were written, `changed` if they differ from what
 * was there. Checkpoints after a change no longer hold: the decoder goes back to the
 * last one that does. It then runs over this write and at most 10 s more towards
 * the packs written so far, so no write holds the lock long. */
static void index_written(fa_cdg_data* d, size_t first, size_t n, int changed)
{
    struct fa_cdg_index* ix = d->index;
    size_t upto;
    if (!ix) return;
    if (changed && first < ix->build.next) {
        size_t keep = first / ix->step;
        if (keep < ix->count) ix->count = keep;
        if (ix->count) checkpoint_load(&ix->at[ix->count - 1], &ix->build); else state_reset(&ix->build);
    }
    if (first + n > ix->high) ix->high = first + n;
    upto = first + n > ix->build.next ? first + n : ix->build.next;
    upto += CDG_CHECKPOINT_PACKS;
    if (upto > ix->high) upto = ix->high;
    index_advance(ix, d, upto);
}

/* ---- rendering ------------------------------------------------------------------- */

static int in_picture_area(uint32_t sx, uint32_t sy)
{
    return sx >= CDG_BORDER_X && sx < FA_CDG_WIDTH - CDG_BORDER_X && sy >= CDG_BORDER_Y && sy < FA_CDG_HEIGHT - CDG_BORDER_Y;
}

/* Colour index of window pixel (x, y): the picture area shows the screen shifted by
 * the smooth-scroll offsets, the border does not move (FA_CDG_INDEX_TRANSPARENT with
 * a transparent border). */
static uint8_t window_index(const fa_cdg_surface* f, uint32_t x, uint32_t y)
{
    uint32_t sx = f->win_x + x, sy = f->win_y + y;
    if (in_picture_area(sx, sy)) {
        sx += f->st.hoff;
        sy += f->st.voff;
    } else if (f->tb) {
        return FA_CDG_INDEX_TRANSPARENT;
    }
    return f->st.screen[sy][sx];
}

/* The background colour with FA_CDG_BACKGROUND_COLOR / FA_CDG_BACKGROUND_OPACITY
 * applied; `rgba` holds the colour the flags above give it. */
static void background_options(const fa_cdg_surface* f, uint8_t rgba[4])
{
    if (f->flags & FA_CDG_BACKGROUND_COLOR) {
        rgba[0] = (uint8_t)(f->bg_color >> 16);
        rgba[1] = (uint8_t)(f->bg_color >> 8);
        rgba[2] = (uint8_t)f->bg_color;
        rgba[3] = (uint8_t)(f->bg_color >> 24);
    }
    if (f->flags & FA_CDG_BACKGROUND_OPACITY) {
        uint32_t base = f->flags & FA_CDG_BACKGROUND_COLOR ? rgba[3] : 255;
        rgba[3] = (uint8_t)((base * f->bg_opacity + 50) / 100);
    }
}

static void palette_rgba(const fa_cdg_surface* f, uint8_t out[16][4])
{
    int i;
    int bg_only = (f->flags & FA_CDG_TRANSPARENT_BACKGROUND_ONLY) != 0;
    int hidden = (f->flags & FA_CDG_HIDE_UNTIL_PRESET) && !f->st.preset;
    for (i = 0; i < 16; i++) {
        uint8_t a = 255;
        if (bg_only)
            a = i == f->st.bg ? 0 : 255;
        else if (f->flags & FA_CDG_TRANSPARENT)
            a = i == f->st.bg ? 0 : (uint8_t)(255 - (f->st.transparency[i] * 255 + 31) / 63);
        out[i][0] = f->st.rgb[i][0];
        out[i][1] = f->st.rgb[i][1];
        out[i][2] = f->st.rgb[i][2];
        out[i][3] = a;
    }
    if (f->st.preset) background_options(f, out[f->st.bg]);
    if (hidden)
        for (i = 0; i < 16; i++) out[i][3] = 0;
}

static void store_pixel(const fa_cdg_surface* f, uint8_t* px, uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    /* r, g, b come in premultiplied */
    if (!(f->flags & FA_CDG_PREMULTIPLIED)) {
        if (a == 0) { r = g = b = 0; }
        else if (a < 255) { r = (r * 255 + a / 2) / a; g = (g * 255 + a / 2) / a; b = (b * 255 + a / 2) / a; }
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
    }
    if (f->format == FA_CDG_PIXELS_BGRA32) { px[0] = (uint8_t)b; px[1] = (uint8_t)g; px[2] = (uint8_t)r; }
    else { px[0] = (uint8_t)r; px[1] = (uint8_t)g; px[2] = (uint8_t)b; }
    px[3] = (uint8_t)a;
}

/* Smooth scaling's source position for output pixel `o` of `n` (a window of `w`):
 * window pixel << 8 | the weight of the next one, in 8-bit fixed point. Pixel
 * centres map to pixel centres. */
static uint32_t lerp_map(uint32_t o, uint32_t n, uint32_t w)
{
    int64_t fp = ((int64_t)(2 * o + 1) * w * 256) / (2 * n) - 128;
    uint32_t p, t;
    if (fp < 0) fp = 0;
    p = (uint32_t)(fp >> 8);
    t = (uint32_t)(fp & 255);
    if (p >= w - 1) { p = w - 1; t = 0; }
    return p << 8 | t;
}

static void render_rect(const fa_cdg_surface* f, const uint8_t pal[16][4], uint8_t* dst, int32_t stride,
                        uint32_t ox0, uint32_t oy0, uint32_t ox1, uint32_t oy1)
{
    uint32_t pm[CDG_SCALE_COLORS][4];   /* premultiplied palette; 16 = transparent */
    uint32_t ox, oy, i, cy0 = UINT32_MAX, cy1 = UINT32_MAX;
    uint8_t flat[CDG_SCALE_COLORS][4], r0[FA_CDG_WIDTH], r1[FA_CDG_WIDTH];   /* smooth: output pixels, window rows cy0, cy1 */
    int have_flat = 0;
    if (ox0 >= ox1 || oy0 >= oy1) return;
    memset(pm[16], 0, sizeof(pm[16]));
    for (i = 0; i < 16; i++) {
        pm[i][3] = pal[i][3];
        pm[i][0] = (pal[i][0] * pm[i][3] + 127) / 255;
        pm[i][1] = (pal[i][1] * pm[i][3] + 127) / 255;
        pm[i][2] = (pal[i][2] * pm[i][3] + 127) / 255;
    }
    for (oy = oy0; oy < oy1; oy++) {
        uint8_t* row = dst + (size_t)oy * (size_t)stride;
        if (f->format == FA_CDG_PIXELS_INDEXED8) {
            for (ox = ox0; ox < ox1; ox++) row[ox] = window_index(f, f->xmap[ox], f->ymap[oy]);
        } else if (!(f->flags & FA_CDG_SMOOTH) || (f->width == f->win_w && f->height == f->win_h)) {
            /* Sharp pixels: straight from the palette in the output's byte order. */
            uint8_t lut[CDG_SCALE_COLORS][4];
            uint32_t wy = f->ymap[oy];
            int premul = (f->flags & FA_CDG_PREMULTIPLIED) != 0;
            memset(lut[16], 0, sizeof(lut[16]));
            for (i = 0; i < 16; i++) {
                uint32_t r = premul ? pm[i][0] : pal[i][0], g = premul ? pm[i][1] : pal[i][1];
                uint32_t b = premul ? pm[i][2] : pal[i][2];
                int bgra = f->format == FA_CDG_PIXELS_BGRA32;
                lut[i][0] = (uint8_t)(bgra ? b : r); lut[i][1] = (uint8_t)g; lut[i][2] = (uint8_t)(bgra ? r : b);
                lut[i][3] = pal[i][3];
            }
            for (ox = ox0; ox < ox1; ox++)
                memcpy(row + ox * 4, lut[window_index(f, f->xmap[ox], wy)], 4);
        } else {
            /* Bilinear on premultiplied colour, in 8-bit fixed point (lerp_map).
             * Where all four source pixels are one colour the blend is that colour,
             * so those pixels (most of a picture) come straight from `flat`. */
            uint32_t y0 = f->ylerp[oy] >> 8, ty = f->ylerp[oy] & 255, y1 = y0 + 1 < f->win_h ? y0 + 1 : y0;
            if (!have_flat) {
                for (i = 0; i < CDG_SCALE_COLORS; i++) store_pixel(f, flat[i], pm[i][0], pm[i][1], pm[i][2], pm[i][3]);
                have_flat = 1;
            }
            if (y0 != cy0 || y1 != cy1) {
                /* Only the window columns this rectangle reads; the row below becomes
                 * the row above as the output moves down. */
                uint32_t xa = f->xlerp[ox0] >> 8, xb = (f->xlerp[ox1 - 1] >> 8) + 1;
                if (xb > f->win_w - 1) xb = f->win_w - 1;
                if (y0 == cy1) memcpy(r0 + xa, r1 + xa, xb - xa + 1);
                else for (i = xa; i <= xb; i++) r0[i] = window_index(f, i, y0);
                for (i = xa; i <= xb; i++) r1[i] = y1 == y0 ? r0[i] : window_index(f, i, y1);
                cy0 = y0;
                cy1 = y1;
            }
            for (ox = ox0; ox < ox1; ox++) {
                uint32_t x0 = f->xlerp[ox] >> 8, tx = f->xlerp[ox] & 255, x1 = x0 + 1 < f->win_w ? x0 + 1 : x0, k;
                uint32_t acc[4];
                uint8_t ia = r0[x0], ib = r0[x1], ic = r1[x0], id = r1[x1];
                const uint32_t *a, *b, *c, *d;
                if (ia == ib && ia == ic && ia == id) { memcpy(row + ox * 4, flat[ia], 4); continue; }
                a = pm[ia]; b = pm[ib]; c = pm[ic]; d = pm[id];
                for (k = 0; k < 4; k++) {
                    uint32_t top = a[k] * (256 - tx) + b[k] * tx;
                    uint32_t bottom = c[k] * (256 - tx) + d[k] * tx;
                    acc[k] = (top * (256 - ty) + bottom * ty + 32768) >> 16;
                }
                store_pixel(f, row + ox * 4, acc[0], acc[1], acc[2], acc[3]);
            }
        }
    }
}

static uint32_t bytes_per_pixel(const fa_cdg_surface* f)
{
    return !f->lay.scaled && f->format == FA_CDG_PIXELS_INDEXED8 ? 1 : 4;
}

/* The pixel format of the buffer: presentation scaling draws colour. */
static fa_cdg_pixel_format out_format(const fa_cdg_surface* f)
{
    if (!f->lay.scaled) return f->format;
    return f->format == FA_CDG_PIXELS_RGBA32 ? FA_CDG_PIXELS_RGBA32 : FA_CDG_PIXELS_BGRA32;
}

/* Buffer rectangle r (x0 y0 x1 y1) outside the picture: transparent. */
static void fill_padding(const fa_cdg_surface* f, uint8_t* dst, int32_t stride, const int r[4])
{
    const cdg_layout* l = &f->lay;
    int x0 = r[0], y0 = r[1], x1 = r[2], y1 = r[3], y;
    int px0 = (int)l->pic_x, py0 = (int)l->pic_y, px1 = px0 + (int)l->pic_w, py1 = py0 + (int)l->pic_h;
    uint32_t bpp = bytes_per_pixel(f);
    int value = bpp == 1 ? FA_CDG_INDEX_TRANSPARENT : 0;
    if (x0 >= px0 && x1 <= px1 && y0 >= py0 && y1 <= py1) return;
    for (y = y0; y < y1; y++) {
        uint8_t* row = dst + (size_t)y * (size_t)stride;
        if (y < py0 || y >= py1) { memset(row + (size_t)x0 * bpp, value, (size_t)(x1 - x0) * bpp); continue; }
        if (x0 < px0) memset(row + (size_t)x0 * bpp, value, (size_t)((x1 < px0 ? x1 : px0) - x0) * bpp);
        if (x1 > px1) { int a = x0 > px1 ? x0 : px1; memset(row + (size_t)a * bpp, value, (size_t)(x1 - a) * bpp); }
    }
}

/* ---- presentation scaling ------------------------------------------------------- */

/* The colours the master is drawn with: premultiplied, in the buffer's byte order,
 * the background transparent, 16 transparent. */
static void scaled_palette(const fa_cdg_surface* f, const uint8_t pal[16][4], uint8_t pm[CDG_SCALE_COLORS][4])
{
    int i, rgba = out_format(f) == FA_CDG_PIXELS_RGBA32;
    for (i = 0; i < 16; i++) {
        uint32_t a = i == f->st.bg ? 0 : pal[i][3];
        uint8_t r = (uint8_t)((pal[i][0] * a + 127) / 255), g = (uint8_t)((pal[i][1] * a + 127) / 255);
        uint8_t b = (uint8_t)((pal[i][2] * a + 127) / 255);
        pm[i][0] = rgba ? r : b;
        pm[i][1] = g;
        pm[i][2] = rgba ? b : r;
        pm[i][3] = (uint8_t)a;
    }
    memset(pm[16], 0, sizeof(pm[16]));
}

static void master_add(const fa_cdg_surface* f, fa_rects* m, int x0, int y0, int x1, int y1)
{
    if (x0 < f->mx0) x0 = f->mx0;
    if (y0 < f->my0) y0 = f->my0;
    if (x1 > f->mx0 + f->mw) x1 = f->mx0 + f->mw;
    if (y1 > f->my0 + f->mh) y1 = f->my0 + f->mh;
    fa_rects_add(m, x0, y0, x1, y1);
}

/* Screen area [x0, x1) x [y0, y1) changed: the master pixels that depend on it. */
static void master_mark(const fa_cdg_surface* f, fa_rects* m, int x0, int y0, int x1, int y1)
{
    int a = f->sc.reach_across, l = f->sc.reach_along;
    master_add(f, m, x0 - l, y0 - a, x1 + l, y1 + a);
    if (l != a) master_add(f, m, x0 - a, y0 - l, x1 + a, y1 + l);
}

/* Moves the rows (h of them, `len` bytes from byte `from`, to byte `to`) of a plane
 * down by `dy` rows (up when negative). */
static void shift_plane(uint8_t* b, size_t stride, int h, int dy, size_t len, size_t to, size_t from)
{
    int y;
    if (dy > 0) {
        for (y = h - 1; y >= dy; y--) memmove(b + (size_t)y * stride + to, b + (size_t)(y - dy) * stride + from, len);
    } else {
        for (y = 0; y < h + dy; y++) memmove(b + (size_t)y * stride + to, b + (size_t)(y - dy) * stride + from, len);
    }
}

/* The screen moved by (dx, dy) CD+G pixels: the master moves with it, and what came
 * in at its edges is marked for drawing, as is what now lies next to the screen's
 * own edge (drawn there without a neighbour). */
static void master_shift(fa_cdg_surface* f, int dx, int dy, fa_rects* m)
{
    int n = (int)f->sc.n, dxp = dx * n, dyp = dy * n, w = f->mw * n, h = f->mh * n;
    int mx1 = f->mx0 + f->mw, my1 = f->my0 + f->mh;
    size_t len = (size_t)(w - (dxp < 0 ? -dxp : dxp)), to = (size_t)(dxp > 0 ? dxp : 0);
    size_t from = (size_t)(dxp < 0 ? -dxp : 0);
    /* The effects move with the picture; with the 2N pass the N master is not shown. */
    if (!(f->fx_on && f->x2)) shift_plane(f->master, f->master_stride, h, dyp, len * 4, to * 4, from * 4);
    if (f->fx_on) shift_plane(f->fxm, f->master_stride, h, dyp, len * 4, to * 4, from * 4);
    if (f->fx_on && f->x2) {
        /* The 2N master moves too (its effects are drawn again where they changed). */
        shift_plane(f->m2, f->m2_stride, 2 * h, 2 * dyp, 8 * len, 8 * to, 8 * from);
    }
    if (f->fx_on && f->keep_ok) {
        /* What the effects kept, likewise, and with the 2N pass its effects, which a
         * blend leaves where nothing is under the shape. */
        int k = f->x2 ? 2 : 1;
        fa_fx_keep* kp = &f->keep;
        if (f->x2) shift_plane(f->f2, f->m2_stride, 2 * h, 2 * dyp, 8 * len, 8 * to, 8 * from);
        shift_plane(kp->alpha, kp->stride, k * h, k * dyp, k * len, k * to, k * from);
        if (kp->shadow)
            shift_plane((uint8_t*)kp->shadow, kp->stride * sizeof(float), k * h, k * dyp, k * len * sizeof(float),
                        k * to * sizeof(float), k * from * sizeof(float));
        if (kp->bevel)
            shift_plane((uint8_t*)kp->bevel, kp->stride * sizeof(float), k * h, k * dyp, k * len * sizeof(float),
                        k * to * sizeof(float), k * from * sizeof(float));
    }
    if (dx > 0) master_mark(f, m, f->mx0, f->my0, f->mx0 + dx, my1);
    if (dx < 0) master_mark(f, m, mx1 + dx, f->my0, mx1, my1);
    if (dy > 0) master_mark(f, m, f->mx0, f->my0, mx1, f->my0 + dy);
    if (dy < 0) master_mark(f, m, f->mx0, my1 + dy, mx1, my1);
    /* Edge blend treats a run that ends at the screen's edge differently: content that
     * moved towards an edge is drawn again within reach of it (clipped to the master). */
    if (dx > 0) master_mark(f, m, FA_CDG_WIDTH - 1, f->my0, FA_CDG_WIDTH, my1);
    if (dx < 0) master_mark(f, m, 0, f->my0, 1, my1);
    if (dy > 0) master_mark(f, m, f->mx0, FA_CDG_HEIGHT - 1, mx1, FA_CDG_HEIGHT);
    if (dy < 0) master_mark(f, m, f->mx0, 0, mx1, 1);
}

/* The border area, moved by (dx, dy). */
static void mark_border(const fa_cdg_surface* f, fa_rects* m, int dx, int dy)
{
    master_mark(f, m, dx, dy, FA_CDG_WIDTH + dx, CDG_BORDER_Y + dy);
    master_mark(f, m, dx, FA_CDG_HEIGHT - CDG_BORDER_Y + dy, FA_CDG_WIDTH + dx, FA_CDG_HEIGHT + dy);
    master_mark(f, m, dx, CDG_BORDER_Y + dy, CDG_BORDER_X + dx, FA_CDG_HEIGHT - CDG_BORDER_Y + dy);
    master_mark(f, m, FA_CDG_WIDTH - CDG_BORDER_X + dx, CDG_BORDER_Y + dy, FA_CDG_WIDTH + dx, FA_CDG_HEIGHT - CDG_BORDER_Y + dy);
}

/* View rectangle [x0, x1) x [y0, y1) (window coordinates on the screen), as buffer
 * pixels, into `area`. */
static void view_to_buffer(const fa_cdg_surface* f, fa_rects* area, int x0, int y0, int x1, int y1)
{
    int n = (int)f->lay.n, cx = (int)f->win_x, cy = (int)f->win_y;
    if (x0 < cx) x0 = cx;
    if (y0 < cy) y0 = cy;
    if (x1 > cx + (int)f->win_w) x1 = cx + (int)f->win_w;
    if (y1 > cy + (int)f->win_h) y1 = cy + (int)f->win_h;
    if (x0 >= x1 || y0 >= y1) return;
    fa_rects_add(area, (int)f->lay.pic_x + (x0 - cx) * n, (int)f->lay.pic_y + (y0 - cy) * n,
                 (int)f->lay.pic_x + (x1 - cx) * n, (int)f->lay.pic_y + (y1 - cy) * n);
}

/* What the effects keep; fx_help_free: the helpers' scratch. */
static void keep_free(fa_cdg_surface* f)
{
    free(f->keep.alpha);
    free(f->keep.shadow);
    free(f->keep.bevel);
    memset(&f->keep, 0, sizeof(f->keep));
    f->keep_ok = 0;
}

static void fx_help_free(fa_cdg_surface* f)
{
    int i;
    for (i = 0; i < CDG_FX_HELPERS; i++) {
        free(f->fx_help[i]);
        f->fx_help[i] = NULL;
        f->fx_help_size[i] = 0;
    }
}

/* f2 [x0, x1) x [y0, y1) at N (fxm pixels) into fxm, each pixel the mean of its four. */
static void box_down(fa_cdg_surface* f, int x0, int y0, int x1, int y1)
{
    int xx, yy, k;
    for (yy = y0; yy < y1; yy++) {
        const uint8_t* a = f->f2 + (size_t)(2 * yy) * f->m2_stride + (size_t)(2 * x0) * 4;
        const uint8_t* b = a + f->m2_stride;
        uint8_t* o = f->fxm + (size_t)yy * f->master_stride + (size_t)x0 * 4;
        for (xx = x0; xx < x1; xx++, a += 8, b += 8, o += 4)
            for (k = 0; k < 4; k++) o[k] = (uint8_t)((a[k] + a[k + 4] + b[k] + b[k + 4] + 2) >> 2);
    }
}

/* The effects to draw with presentation scaling, into `plan`, with their buffers
 * ready; 0 when there are none (or no memory for them: drawn without). */
static int fx_ready(fa_cdg_surface* f, fa_fx_plan* plan)
{
    int x2 = f->sc.n <= 3;
    float n = (float)(x2 ? 2 * f->sc.n : f->sc.n);
    size_t need;
    if (!f->has_fx) {
        keep_free(f);
        fx_help_free(f);
    }
    if (!f->has_fx && f->m2) {
        /* Effects off: the 2N buffers go (a full redraw follows when they come back). */
        free(f->m2);
        free(f->f2);
        free(f->rows2);
        cdg_scaler_free(&f->sc2);
        f->m2 = f->f2 = NULL;
        f->rows2 = NULL;
        f->x2 = 0;
    }
    if (!f->has_fx ||
        !fa_fx_plan_make(plan, &f->fx, 1.5f * n, 2.0f * n, 2.0f * n, n, out_format(f) == FA_CDG_PIXELS_RGBA32))
        return 0;
    if (!f->fxm) {
        f->fxm = (uint8_t*)malloc(f->master_stride * (size_t)f->mh * f->sc.n);
        if (!f->fxm) return 0;
    }
    if (x2 && !f->m2) {
        uint32_t n2 = 2 * f->sc.n;
        f->m2_stride = 2 * f->master_stride;
        f->m2 = (uint8_t*)malloc(f->m2_stride * (size_t)f->mh * n2);
        f->f2 = (uint8_t*)malloc(f->m2_stride * (size_t)f->mh * n2);
        f->rows2 = (uint16_t*)malloc(CDG_SCALE_ROWS_BYTES(n2));
        if (!f->m2 || !f->f2 || !f->rows2) {
            free(f->m2);
            free(f->f2);
            free(f->rows2);
            f->m2 = f->f2 = NULL;
            f->rows2 = NULL;
            return 0;
        }
    }
    f->x2 = x2;
    if (!f->keep.alpha || !f->keep.shadow != !plan->shadow || !f->keep.bevel != !plan->bevel) {
        /* What the effects keep, for the plan's effects (without it they are drawn in
         * full every time). */
        size_t stride = (x2 ? f->m2_stride : f->master_stride) / 4, px = stride * (size_t)f->mh * f->sc.n * (x2 ? 2 : 1);
        keep_free(f);
        f->keep.alpha = (uint8_t*)malloc(px);
        f->keep.shadow = plan->shadow ? (float*)malloc(px * sizeof(float)) : NULL;
        f->keep.bevel = plan->bevel ? (float*)malloc(px * sizeof(float)) : NULL;
        f->keep.stride = stride;
        if (!f->keep.alpha || (plan->shadow && !f->keep.shadow) || (plan->bevel && !f->keep.bevel)) keep_free(f);
    }
    need = fa_fx_scratch_bytes(plan);
    if (need > f->fx_scratch_size) {
        free(f->fx_scratch);
        f->fx_scratch = malloc(need);
        f->fx_scratch_size = f->fx_scratch ? need : 0;
        if (!f->fx_scratch) return 0;
    }
    return 1;
}

/* Drawing the effects in pieces, on this thread and idle engine workers at once
 * (fa_engine_parallel): each piece has its own scratch and writes only its own pixels
 * of fxm, f2 and what is kept. */
typedef struct fx_job {
    fa_cdg_surface* f;
    const fa_fx_plan* plan;
    int w, h;                     /* the effects' source, fxm pixels */
    int (*piece)[4];              /* fx_render: fxm pixels; fx_blend: screen pixels */
    int* ok;                      /* fx_blend: 1 for each piece blended */
    void* scratch[1 + CDG_FX_HELPERS];
    fa_fx_keep keep[1 + CDG_FX_HELPERS]; /* f->keep, each with its own `mixed` */
} fx_job;

static void fx_render_piece(void* ctx, int item, int slot)
{
    fx_job* j = (fx_job*)ctx;
    fa_cdg_surface* f = j->f;
    const int* r = j->piece[item];
    fa_fx_keep* kp = j->keep[slot].alpha ? &j->keep[slot] : NULL;
    if (!f->x2) {
        fa_fx_render_keep(j->plan, f->master, f->master_stride, j->w, j->h, f->fxm, f->master_stride, r[0], r[1], r[2],
                          r[3], j->scratch[slot], kp);
        return;
    }
    /* At 2N, then each output pixel the mean of its four. */
    fa_fx_render_keep(j->plan, f->m2, f->m2_stride, 2 * j->w, 2 * j->h, f->f2, f->m2_stride, 2 * r[0], 2 * r[1], 2 * r[2],
                      2 * r[3], j->scratch[slot], kp);
    box_down(f, r[0], r[1], r[2], r[3]);
}

static fa_atomic32 g_blends;

int32_t fa_cdg_test_blends(void) { return fa_load32(&g_blends); }

static void fx_blend_piece(void* ctx, int item, int slot)
{
    fx_job* j = (fx_job*)ctx;
    fa_cdg_surface* f = j->f;
    const int* r = j->piece[item];
    int n = (int)f->sc.n, k = f->x2 ? 2 : 1;
    int x0 = (r[0] - f->mx0) * n, y0 = (r[1] - f->my0) * n, x1 = (r[2] - f->mx0) * n, y1 = (r[3] - f->my0) * n;
    (void)slot;
    j->ok[item] = fa_fx_recolor(j->plan, f->x2 ? f->m2 : f->master, f->x2 ? f->m2_stride : f->master_stride,
                                f->x2 ? f->f2 : f->fxm, f->x2 ? f->m2_stride : f->master_stride, k * x0, k * y0, k * x1,
                                k * y1, &f->keep);
    if (!j->ok[item]) return;
    if (f->x2) box_down(f, x0, y0, x1, y1);
    fa_fetch_add32(&g_blends, 1);
}

/* Runs `run` over j->piece[0 .. count): shared with idle workers when there is enough
 * to draw (`px` output pixels), with as many as have scratch when `scratch` (a blend
 * needs none). */
static void fx_run(fx_job* j, void (*run)(void*, int, int), int count, int64_t px, int scratch)
{
    fa_cdg_surface* f = j->f;
    fa_par_job pj;
    int i, slots = 0;
    memset(&pj, 0, sizeof(pj));
    if (f->eng && count > 1 && px >= CDG_FX_SHARE) {
        int want = (int)f->eng->worker_count < CDG_FX_HELPERS ? (int)f->eng->worker_count : CDG_FX_HELPERS;
        for (; slots < want; slots++) {
            if (scratch && f->fx_help_size[slots] < f->fx_scratch_size) {
                free(f->fx_help[slots]);
                f->fx_help[slots] = malloc(f->fx_scratch_size);
                f->fx_help_size[slots] = f->fx_help[slots] ? f->fx_scratch_size : 0;
                if (!f->fx_help[slots]) break;
            }
        }
    }
    j->scratch[0] = f->fx_scratch;
    for (i = 0; i <= slots; i++) {
        if (i) j->scratch[i] = f->fx_help[i - 1];
        j->keep[i] = f->keep;
        j->keep[i].mixed = 0;
    }
    pj.run = run;
    pj.ctx = j;
    pj.count = count;
    pj.slots = slots;
    fa_engine_parallel(f->eng, &pj);
    for (i = 0; i <= slots; i++) f->keep.mixed |= j->keep[i].mixed;
}

/* The effects of the master rectangles `m` drawn again, as far as they reach. */
static void fx_draw(fa_cdg_surface* f, const fa_fx_plan* plan, const fa_rects* e, int mx1, int my1)
{
    int piece[CDG_FX_PIECES][4], i, count = 0, n = (int)f->sc.n, k = f->x2 ? 2 : 1;
    int tile = FA_FX_TILE / k, ways = 1 + (f->eng ? (int)f->eng->worker_count : 0);
    int64_t px = 0;
    fx_job j;
    memset(&j, 0, sizeof(j));
    j.f = f;
    j.plan = plan;
    j.w = (mx1 - f->mx0) * n;
    j.h = (my1 - f->my0) * n;
    j.piece = piece;
    if (ways > 1 + CDG_FX_HELPERS) ways = 1 + CDG_FX_HELPERS;
    for (i = 0; i < e->n; i++) {
        int x0 = (e->r[i][0] - f->mx0) * n, y0 = (e->r[i][1] - f->my0) * n;
        int x1 = (e->r[i][2] - f->mx0) * n, y1 = (e->r[i][3] - f->my0) * n, x, y, cols, rows, bh = tile;
        if (x1 > j.w) x1 = j.w;
        if (y1 > j.h) y1 = j.h;
        if (x0 >= x1 || y0 >= y1) continue;
        /* Tiles as fa_fx_render's, or bands of them when there are too few to share:
         * each piece works out the field around it as far as the effects reach. */
        cols = (x1 - x0 + tile - 1) / tile;
        rows = (y1 - y0 + tile - 1) / tile;
        if (cols * rows < ways && (int64_t)(x1 - x0) * (y1 - y0) * k * k >= CDG_FX_SHARE) {
            int want = (ways + cols - 1) / cols, least = (plan->reach + k - 1) / k;
            bh = (y1 - y0 + want - 1) / want;
            if (bh < least) bh = least;
            if (bh > tile) bh = tile;
        }
        for (y = y0; y < y1; y += bh)
            for (x = x0; x < x1; x += tile) {
                if (count == CDG_FX_PIECES) {
                    fx_run(&j, fx_render_piece, count, px, 1);
                    count = 0;
                    px = 0;
                }
                piece[count][0] = x;
                piece[count][1] = y;
                piece[count][2] = x + tile < x1 ? x + tile : x1;
                piece[count][3] = y + bh < y1 ? y + bh : y1;
                px += (int64_t)(piece[count][2] - x) * (piece[count][3] - y) * k * k;
                count++;
            }
    }
    if (count) fx_run(&j, fx_render_piece, count, px, 1);
}

/* Blends pieces [0, count) of `j`, then sorts them: blended into `done`, the others'
 * effects to draw again as far as they reach into `e`. */
static void fx_blend_all(fx_job* j, int count, int64_t px, int rc, fa_rects* done, fa_rects* e)
{
    int b;
    if (!count) return;
    fx_run(j, fx_blend_piece, count, px, 0);
    for (b = 0; b < count; b++) {
        const int* r = j->piece[b];
        if (j->ok[b]) fa_rects_add(done, r[0], r[1], r[2], r[3]);
        else master_add(j->f, e, r[0] - rc, r[1] - rc, r[2] + rc, r[3] + rc);
    }
}

/* Block by block of the master rectangles `m`: where the shape is the same, its new
 * colours are blended with what the effects kept (into `done`); elsewhere the effects
 * must be drawn again as far as they reach (into `e`, drawn afterwards, so over any
 * blend they reach). */
static void fx_recolor(fa_cdg_surface* f, const fa_fx_plan* plan, const fa_rects* m, int mx1, int my1, int rc,
                       fa_rects* done, fa_rects* e)
{
    int piece[CDG_FX_PIECES][4], ok[CDG_FX_PIECES], i, count = 0, n = (int)f->sc.n, k = f->x2 ? 2 : 1;
    int64_t px = 0;
    fx_job j;
    memset(&j, 0, sizeof(j));
    j.f = f;
    j.plan = plan;
    j.piece = piece;
    j.ok = ok;
    for (i = 0; i < m->n; i++) {
        int bx, by;
        for (by = m->r[i][1]; by < m->r[i][3] && by < my1; by += CDG_KEEP_BLOCK)
            for (bx = m->r[i][0]; bx < m->r[i][2] && bx < mx1; bx += CDG_KEEP_BLOCK) {
                int ex = bx + CDG_KEEP_BLOCK < m->r[i][2] ? bx + CDG_KEEP_BLOCK : m->r[i][2];
                int ey = by + CDG_KEEP_BLOCK < m->r[i][3] ? by + CDG_KEEP_BLOCK : m->r[i][3];
                if (count == CDG_FX_PIECES) {
                    fx_blend_all(&j, count, px, rc, done, e);
                    count = 0;
                    px = 0;
                }
                piece[count][0] = bx;
                piece[count][1] = by;
                piece[count][2] = ex < mx1 ? ex : mx1;
                piece[count][3] = ey < my1 ? ey : my1;
                px += (int64_t)(piece[count][2] - bx) * (piece[count][3] - by) * n * n * k * k;
                count++;
            }
    }
    fx_blend_all(&j, count, px, rc, done, e);
}

/* The colours changed from those the master was drawn with: the master pixels drawn
 * from a screen pixel whose colour changed, a band of tile rows at a time, its
 * columns that have one in runs (gaps of up to a tile joined). The background is
 * transparent whatever its colour, so a fade redraws only what is on it. */
static void recolor_mark(const fa_cdg_surface* f, const uint8_t pm[CDG_SCALE_COLORS][4], int tb, fa_rects* m)
{
    uint8_t changed[CDG_SCALE_COLORS];
    int i, x, y, y0, xa = tb ? CDG_BORDER_X : 0, xb = tb ? FA_CDG_WIDTH - CDG_BORDER_X : FA_CDG_WIDTH;
    for (i = 0; i < CDG_SCALE_COLORS; i++) changed[i] = (uint8_t)(memcmp(pm[i], f->master_pm[i], 4) != 0);
    for (y0 = 0; y0 < FA_CDG_HEIGHT; y0 += CDG_TILE_H) {
        uint8_t col[FA_CDG_WIDTH];
        int ya = FA_CDG_HEIGHT, yb = 0, start = -1, last = 0;
        memset(col, 0, sizeof(col));
        for (y = y0; y < y0 + CDG_TILE_H; y++) {
            const uint8_t* row = f->st.screen[y];
            int any = 0;
            /* A transparent border is drawn as 16, which never changes. */
            if (tb && (y < CDG_BORDER_Y || y >= FA_CDG_HEIGHT - CDG_BORDER_Y)) continue;
            for (x = xa; x < xb; x++)
                if (changed[row[x]]) col[x] = any = 1;
            if (any && y < ya) ya = y;
            if (any) yb = y + 1;
        }
        if (ya >= yb) continue;
        for (x = xa; x < xb; x++) {
            if (!col[x]) continue;
            if (start >= 0 && x - last > CDG_TILE_W) {
                master_mark(f, m, start, ya, last + 1, yb);
                start = -1;
            }
            if (start < 0) start = x;
            last = x;
        }
        if (start >= 0) master_mark(f, m, start, ya, last + 1, yb);
    }
}

/* Drawing master rectangles with `sc`, on this thread and idle engine workers at
 * once (fa_engine_parallel) when there is enough: in bands of screen rows, each with
 * scratch rows of its own. A band writes only its own pixels and draws them as the
 * whole rectangle would (cdg_scale_rect reads its neighbours from the screen). */
typedef struct scale_job {
    fa_cdg_surface* f;
    const cdg_scaler* sc;
    const uint8_t (*pm)[4];
    uint8_t* out;
    size_t stride;
    int (*piece)[4];
    uint16_t* rows[1 + CDG_FX_HELPERS];
} scale_job;

static void scale_piece(void* ctx, int item, int slot)
{
    scale_job* j = (scale_job*)ctx;
    const int* r = j->piece[item];
    cdg_scale_rect(j->sc, (const uint8_t(*)[CDG_SCALE_W])j->f->eidx, j->pm, j->out, j->stride, j->f->mx0, j->f->my0, r[0],
                   r[1], r[2], r[3], j->rows[slot]);
}

static void scale_draw(fa_cdg_surface* f, const cdg_scaler* sc, const uint8_t pm[CDG_SCALE_COLORS][4], uint8_t* out,
                       size_t stride, uint16_t* rows, const fa_rects* m)
{
    int piece[CDG_FX_PIECES][4], i, y, count = 0, slots = 0, n = (int)sc->n, band = FA_CDG_HEIGHT;
    int64_t px = 0;
    size_t need = CDG_SCALE_ROWS_BYTES(sc->n);
    scale_job j;
    fa_par_job pj;
    memset(&j, 0, sizeof(j));
    memset(&pj, 0, sizeof(pj));
    for (i = 0; i < m->n; i++) px += (int64_t)(m->r[i][2] - m->r[i][0]) * (m->r[i][3] - m->r[i][1]) * n * n;
    if (f->eng && f->eng->worker_count && px >= CDG_FX_SHARE) {
        int want = (int)f->eng->worker_count < CDG_FX_HELPERS ? (int)f->eng->worker_count : CDG_FX_HELPERS;
        for (; slots < want; slots++) {
            if (f->rows_help_size[slots] < need) {
                free(f->rows_help[slots]);
                f->rows_help[slots] = (uint16_t*)malloc(need);
                f->rows_help_size[slots] = f->rows_help[slots] ? need : 0;
                if (!f->rows_help[slots]) break;
            }
            j.rows[1 + slots] = f->rows_help[slots];
        }
        if (slots) band = CDG_TILE_H;
    }
    j.f = f;
    j.sc = sc;
    j.pm = pm;
    j.out = out;
    j.stride = stride;
    j.piece = piece;
    j.rows[0] = rows;
    pj.run = scale_piece;
    pj.ctx = &j;
    pj.slots = slots;
    /* At most FA_SURFACE_MAX_DIRTY rectangles of FA_CDG_HEIGHT / CDG_TILE_H bands. */
    for (i = 0; i < m->n; i++)
        for (y = m->r[i][1]; y < m->r[i][3] && count < CDG_FX_PIECES; y += band) {
            piece[count][0] = m->r[i][0];
            piece[count][1] = y;
            piece[count][2] = m->r[i][2];
            piece[count][3] = y + band < m->r[i][3] ? y + band : m->r[i][3];
            count++;
        }
    pj.count = count;
    fa_engine_parallel(f->eng, &pj);
}

/* advance() with presentation scaling: the master is brought up to the decoder state
 * (moved for scrolls, the changed areas and what they reach drawn again) and `area`
 * gets the buffer pixels that changed. */
static void advance_scaled(fa_cdg_surface* f, const uint8_t pal[16][4], fa_rects* area)
{
    cdg_state* st = &f->st;
    uint8_t pm[CDG_SCALE_COLORS][4];
    fa_rects m, e;
    fa_fx_plan plan;
    int tb = f->tb && !st->hoff && !st->voff, i, y;
    int moved = st->shift_x || st->shift_y, full, view_full, fx = fx_ready(f, &plan), keep;
    /* The effects see the crop, or with smooth-scroll offsets everything they can show. */
    int wide = st->hoff || st->voff;
    scaled_palette(f, pal, pm);
    full = !f->drawn || abs(st->shift_x) >= f->mw || abs(st->shift_y) >= f->mh || fx != f->fx_on ||
           (fx && wide != f->fx_wide);
    /* What the effects kept still holds unless they or the master were drawn afresh;
     * new colours alone keep it. */
    keep = fx && f->keep_ok && f->drawn && f->fx_on && wide == f->fx_wide && abs(st->shift_x) < f->mw &&
           abs(st->shift_y) < f->mh && memcmp(&plan, &f->keep_plan, sizeof(plan)) == 0;
    if (!keep) f->keep_ok = 0;
    f->fx_on = fx;
    f->fx_wide = wide;
    fa_rects_clear(&m);
    if (full) {
        fa_rects_add(&m, f->mx0, f->my0, f->mx0 + f->mw, f->my0 + f->mh);
    } else {
        if (moved) master_shift(f, st->shift_x, st->shift_y, &m);
        for (i = 0; i < st->dirty.n; i++) master_mark(f, &m, st->dirty.r[i][0], st->dirty.r[i][1], st->dirty.r[i][2], st->dirty.r[i][3]);
        /* A transparent border stays where it is while the picture moves under it, and
         * the transparent pixels the move carried into the picture are drawn again. */
        if (tb != f->master_tb || (tb && moved)) mark_border(f, &m, 0, 0);
        if (f->master_tb && moved) mark_border(f, &m, st->shift_x, st->shift_y);
        /* New colours: what is drawn from them. */
        if (memcmp(pm, f->master_pm, sizeof(pm)) != 0) recolor_mark(f, pm, tb, &m);
    }
    if (m.n) {
        memcpy(f->eidx, st->screen, sizeof(f->eidx));
        if (tb)
            for (y = 0; y < FA_CDG_HEIGHT; y++) {
                if (y < CDG_BORDER_Y || y >= FA_CDG_HEIGHT - CDG_BORDER_Y) {
                    memset(f->eidx[y], FA_CDG_INDEX_TRANSPARENT, FA_CDG_WIDTH);
                } else {
                    memset(f->eidx[y], FA_CDG_INDEX_TRANSPARENT, CDG_BORDER_X);
                    memset(f->eidx[y] + FA_CDG_WIDTH - CDG_BORDER_X, FA_CDG_INDEX_TRANSPARENT, CDG_BORDER_X);
                }
            }
        /* With the 2N pass the effects draw the picture from the 2N master, so the N
         * one is not shown (a change of effects draws everything again). */
        cdg_scaler* sc = fx && f->x2 ? &f->sc2 : &f->sc;
        if (cdg_scale_prepare(sc, (const uint8_t(*)[CDG_SCALE_W])f->eidx)) {
            fa_rects_clear(&m);
            fa_rects_add(&m, f->mx0, f->my0, f->mx0 + f->mw, f->my0 + f->mh);
        }
        scale_draw(f, sc, (const uint8_t(*)[4])pm, sc == &f->sc2 ? f->m2 : f->master,
                   sc == &f->sc2 ? f->m2_stride : f->master_stride, sc == &f->sc2 ? f->rows2 : f->rows, &m);
    }
    f->master_tb = tb;
    memcpy(f->master_pm, pm, sizeof(pm));
    if (fx) {
        /* The effects of what changed, as far as they reach; after a move also along
         * the edges, where what reached in from outside the master has gone. */
        int n = (int)f->sc.n, pn = f->x2 ? 2 * n : n, rc = (plan.reach + pn - 1) / pn + 1;
        int mx1 = f->mx0 + (wide ? f->mw : (int)f->win_w), my1 = f->my0 + (wide ? f->mh : (int)f->win_h);
        fa_rects done;
        fa_rects_clear(&e);
        fa_rects_clear(&done);
        if (keep) fx_recolor(f, &plan, &m, mx1, my1, rc, &done, &e);
        else
            for (i = 0; i < m.n; i++) master_add(f, &e, m.r[i][0] - rc, m.r[i][1] - rc, m.r[i][2] + rc, m.r[i][3] + rc);
        if (moved && !full) {
            int bx = rc + abs(st->shift_x), by = rc + abs(st->shift_y);
            if (st->shift_x) {
                master_add(f, &e, f->mx0, f->my0, f->mx0 + bx, my1);
                master_add(f, &e, mx1 - bx, f->my0, mx1, my1);
            }
            if (st->shift_y) {
                master_add(f, &e, f->mx0, f->my0, mx1, f->my0 + by);
                master_add(f, &e, f->mx0, my1 - by, mx1, my1);
            }
        }
        /* A full draw rewrites all that is kept, so only its own pixels can be mixed. */
        if (full && !keep) f->keep.mixed = 0;
        fx_draw(f, &plan, &e, mx1, my1);
        if (full && !keep && f->keep.alpha) {
            /* All of it was drawn: from now on what was kept holds. */
            f->keep_ok = 1;
            f->keep_plan = plan;
        }
        fa_rects_add_all(&e, &done);
        m = e;                    /* what changed in what is shown */
    }
    /* A move or new offsets change where everything is shown. */
    view_full = full || moved || st->hoff != f->shown_hoff || st->voff != f->shown_voff;
    if (view_full) {
        fa_rects_add(area, (int)f->lay.pic_x, (int)f->lay.pic_y, (int)(f->lay.pic_x + f->lay.pic_w),
                     (int)(f->lay.pic_y + f->lay.pic_h));
        return;
    }
    for (i = 0; i < m.n; i++) {
        /* The screen area is shown as it is in the border, and shifted by the offsets
         * inside it: both, clipped (more than needed at worst). */
        view_to_buffer(f, area, m.r[i][0], m.r[i][1], m.r[i][2], m.r[i][3]);
        if (st->hoff || st->voff)
            view_to_buffer(f, area, m.r[i][0] - st->hoff, m.r[i][1] - st->voff, m.r[i][2] - st->hoff, m.r[i][3] - st->voff);
    }
}

/* Copies buffer rectangle r (x0 y0 x1 y1) from the master: the border as it is on the
 * screen, the picture area shifted by the smooth-scroll offsets, padding and margin
 * transparent. A transparent border while the offsets are set is not in the master
 * (it holds the screen's colours there, which the offsets can bring into view). */
static void copy_out(const fa_cdg_surface* f, uint8_t* dst, int32_t stride, const int r[4])
{
    const cdg_layout* l = &f->lay;
    int n = (int)l->n, oy, x0 = r[0], x1 = r[2];
    int a = x0 > (int)l->pic_x ? x0 : (int)l->pic_x;
    int b = x1 < (int)(l->pic_x + l->pic_w) ? x1 : (int)(l->pic_x + l->pic_w);
    /* Where the picture area starts and ends in buffer x. */
    int i0 = (int)l->pic_x + (CDG_BORDER_X - (int)f->win_x) * n, i1 = (int)l->pic_x + (FA_CDG_WIDTH - CDG_BORDER_X - (int)f->win_x) * n;
    int zero_border = f->tb && (f->st.hoff || f->st.voff);
    fill_padding(f, dst, stride, r);
    if (i0 < a) i0 = a;
    if (i0 > b) i0 = b;
    if (i1 < i0) i1 = i0;
    if (i1 > b) i1 = b;
    if (a >= b) return;
    for (oy = r[1]; oy < r[3]; oy++) {
        uint8_t* row = dst + (size_t)oy * (size_t)stride;
        int vy, sub, seg;
        if (oy < (int)l->pic_y || oy >= (int)(l->pic_y + l->pic_h)) continue;
        vy = (int)f->win_y + (oy - (int)l->pic_y) / n;
        sub = (oy - (int)l->pic_y) % n;
        for (seg = 0; seg < 3; seg++) {
            int sa = seg == 0 ? a : seg == 1 ? i0 : i1, sb = seg == 0 ? i0 : seg == 1 ? i1 : b, sy = vy, col;
            int shifted = seg == 1 && vy >= CDG_BORDER_Y && vy < FA_CDG_HEIGHT - CDG_BORDER_Y;
            const uint8_t* src;
            if (sa >= sb) continue;
            if (!shifted && zero_border) { memset(row + (size_t)sa * 4, 0, (size_t)(sb - sa) * 4); continue; }
            col = (sa - (int)l->pic_x) + ((int)f->win_x - f->mx0) * n;
            if (shifted) { sy += f->st.voff; col += f->st.hoff * n; }
            src = (f->fx_on ? f->fxm : f->master) + (size_t)((sy - f->my0) * n + sub) * f->master_stride + (size_t)col * 4;
            memcpy(row + (size_t)sa * 4, src, (size_t)(sb - sa) * 4);
        }
    }
}

/* ---- updates ------------------------------------------------------------------------ */

/* advance() without presentation scaling: the output pixels the changed screen area
 * maps to. */
static void advance_classic(fa_cdg_surface* f, int palette_changed, fa_rects* area)
{
    cdg_state* st = &f->st;
    int i, px = (int)f->lay.pic_x, py = (int)f->lay.pic_y;
    /* Indexed pixels do not depend on the palette; colour pixels do. Smooth-scroll
     * offsets move the whole picture area; so does a scroll (the whole screen moves),
     * and anything drawn while the offsets are set (rare: redraw all rather than map
     * the shift). */
    if (st->hoff != f->shown_hoff || st->voff != f->shown_voff || st->shift_x || st->shift_y ||
        (palette_changed && f->format != FA_CDG_PIXELS_INDEXED8) || (st->dirty.n && (st->hoff || st->voff))) {
        fa_rects_add(area, px, py, px + (int)f->width, py + (int)f->height);
        return;
    }
    {
        int pad = (f->flags & FA_CDG_SMOOTH) && f->format != FA_CDG_PIXELS_INDEXED8 ? 1 : 0;
        for (i = 0; i < st->dirty.n; i++) {
            const int* d = st->dirty.r[i];
            int64_t x0 = (int64_t)d[0] - f->win_x - pad, y0 = (int64_t)d[1] - f->win_y - pad;
            int64_t x1 = (int64_t)d[2] - f->win_x + pad, y1 = (int64_t)d[3] - f->win_y + pad;
            uint32_t ox0, oy0, ox1, oy1;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > (int64_t)f->win_w) x1 = f->win_w;
            if (y1 > (int64_t)f->win_h) y1 = f->win_h;
            if (x0 >= x1 || y0 >= y1) continue;
            /* Output pixels whose source lies in the window rectangle (a superset). */
            ox0 = (uint32_t)(x0 * f->width / f->win_w);
            oy0 = (uint32_t)(y0 * f->height / f->win_h);
            ox1 = (uint32_t)((x1 * f->width + f->win_w - 1) / f->win_w);
            oy1 = (uint32_t)((y1 * f->height + f->win_h - 1) / f->win_h);
            if (ox1 > f->width) ox1 = f->width;
            if (oy1 > f->height) oy1 = f->height;
            fa_rects_add(area, px + (int)ox0, py + (int)oy0, px + (int)ox1, py + (int)oy1);
        }
    }
}

/* Brings the surface's picture up to the decoder state: works out the buffer pixels
 * that changed since the last call (everything when nothing was drawn yet), and when
 * any did, takes the next revision and records the area in the history. `pal` gets
 * the colours to draw with. Draws nothing into a buffer: draw() does, into whichever
 * buffer. */
static void advance(fa_cdg_surface* f, uint8_t pal[16][4])
{
    cdg_state* st = &f->st;
    fa_rects area;
    int palette_changed, bg, border;
    palette_rgba(f, pal);
    palette_changed = !f->drawn || memcmp(pal, f->shown_palette, 16 * 4) != 0;
    fa_rects_clear(&area);
    if (!f->drawn) fa_rects_add(&area, 0, 0, (int)f->lay.buf_w, (int)f->lay.buf_h);
    if (f->lay.scaled) advance_scaled(f, pal, &area);
    else advance_classic(f, palette_changed, &area);
    fa_rects_clear(&st->dirty);
    st->shift_x = st->shift_y = 0;
    f->drawn = 1;
    f->shown_hoff = st->hoff;
    f->shown_voff = st->voff;
    if (area.n) {
        fa_mutex_lock(&f->pub_lock);   /* update_into reads them while a worker renders */
        f->revision++;
        fa_history_push(&f->history, f->revision, &area);
        fa_mutex_unlock(&f->pub_lock);
    }
    bg = st->preset ? st->bg : -1;
    border = st->border;
    if (palette_changed || bg != f->shown_bg || border != f->shown_border ||
        memcmp(st->transparency, f->shown_transparency, sizeof(st->transparency)) != 0)
        f->palette_revision++;
    memcpy(f->shown_palette, pal, 16 * 4);
    f->shown_bg = bg;
    f->shown_border = border;
    memcpy(f->shown_transparency, st->transparency, sizeof(st->transparency));
}

/* Draws into `dst`, which holds the picture of revision `since` (0 = unknown): the
 * area changed since then, or everything. `drawn` gets what was drawn; returns 0
 * for a full draw. */
static int draw(const fa_cdg_surface* f, const uint8_t pal[16][4], uint8_t* dst, int32_t stride, uint64_t since,
                fa_rects* drawn)
{
    const cdg_layout* l = &f->lay;
    int i, partial = fa_history_since(&f->history, f->revision, since, drawn);
    uint32_t bpp = bytes_per_pixel(f);
    if (!partial) {
        fa_rects_clear(drawn);
        fa_rects_add(drawn, 0, 0, (int)l->buf_w, (int)l->buf_h);
    }
    for (i = 0; i < drawn->n; i++) {
        const int* r = drawn->r[i];
        int x0, y0, x1, y1;
        if (l->scaled) { copy_out(f, dst, stride, r); continue; }
        fill_padding(f, dst, stride, r);
        x0 = r[0] > (int)l->pic_x ? r[0] - (int)l->pic_x : 0;
        y0 = r[1] > (int)l->pic_y ? r[1] - (int)l->pic_y : 0;
        x1 = r[2] - (int)l->pic_x;
        y1 = r[3] - (int)l->pic_y;
        if (x1 > (int)f->width) x1 = (int)f->width;
        if (y1 > (int)f->height) y1 = (int)f->height;
        if (x0 < x1 && y0 < y1)
            render_rect(f, pal, dst + (size_t)l->pic_y * (size_t)stride + (size_t)l->pic_x * bpp, stride,
                        (uint32_t)x0, (uint32_t)y0, (uint32_t)x1, (uint32_t)y1);
    }
    return partial;
}

/* The decoder's state for a frame. */
static void frame_state(const fa_cdg_surface* f, const uint8_t pal[16][4], int32_t* bg, int32_t* border,
                        uint8_t background[4], uint8_t transparency[16])
{
    *bg = f->st.preset ? f->st.bg : -1;
    *border = f->st.border;
    memcpy(transparency, f->st.transparency, 16);
    if (f->st.preset) memcpy(background, pal[f->st.bg], 4);
    else memset(background, 0, 4);
}

/* fa_cdg_surface_update: the surface's own pixels, up to date. */
static fa_result render(fa_cdg_surface* f, fa_cdg_frame* out)
{
    uint8_t pal[16][4];
    fa_rects drawn;
    int b[4], palette_changed;
    if (!f->pixels) {
        f->pixels = (uint8_t*)calloc((size_t)f->lay.buf_h, (size_t)f->stride);
        if (!f->pixels) return FA_ERROR_OUT_OF_MEMORY;
        f->pixels_revision = 0;
    }
    advance(f, pal);
    draw(f, pal, f->pixels, f->stride, f->pixels_revision, &drawn);
    f->pixels_revision = f->revision;
    palette_changed = !f->pixels_drawn || memcmp(pal, f->pixels_palette, sizeof(pal)) != 0;
    f->pixels_drawn = 1;
    memcpy(f->pixels_palette, pal, sizeof(pal));
    fa_rects_bounds(&drawn, b);

    out->pixels = f->pixels;
    out->width = f->lay.buf_w;
    out->height = f->lay.buf_h;
    out->stride = f->stride;
    out->pixel_format = out_format(f);
    out->palette_changed = palette_changed;
    out->changed = drawn.n > 0 || palette_changed;
    out->dirty_x = (uint32_t)b[0];
    out->dirty_y = (uint32_t)b[1];
    out->dirty_width = (uint32_t)(b[2] - b[0]);
    out->dirty_height = (uint32_t)(b[3] - b[1]);
    memcpy(out->palette, pal, sizeof(pal));
    frame_state(f, pal, &out->background_index, &out->border_index, out->background, out->transparency);
    out->revision = f->revision;
    out->palette_revision = f->palette_revision;
    return FA_OK;
}

/* ---- layout ---------------------------------------------------------------------- */

/* The crop of the surface's safety zone; AUTO and SUPER from the stream's pre-pass
 * (under cdg_lock). */
static void zone_crop(const fa_cdg_surface* f, const fa_cdg_data* d, cdg_layout* l)
{
    static const uint32_t show[4] = { 0, 0, FA_CDG_WIDTH, FA_CDG_HEIGHT };
    static const uint32_t remove[4] = { CDG_BORDER_X, CDG_BORDER_Y, FA_CDG_PICTURE_WIDTH, FA_CDG_PICTURE_HEIGHT };
    const struct fa_cdg_index* ix = d && !d->from_disc ? d->index : NULL;
    int zone = f->has_pres ? (int)f->pres.safety_zone : (f->flags & FA_CDG_NO_BORDER) ? FA_SAFETY_REMOVE : FA_SAFETY_SHOW;
    l->resolved = 1;
    if ((zone == FA_SAFETY_AUTO || zone == FA_SAFETY_SUPER) && !ix) {
        zone = FA_SAFETY_SHOW;
        l->resolved = 0;
    }
    if (zone == FA_SAFETY_AUTO) {
        double ignore = ceil((double)f->pres.auto_ignore_seconds * FA_CDG_PACKS_PER_SECOND);
        zone = (double)ix->border_ink > ignore ? FA_SAFETY_SHOW : FA_SAFETY_REMOVE;
    }
    l->zone = zone;
    if (zone == FA_SAFETY_SUPER) {
        int c0 = ix->box[0], r0 = ix->box[1], c1 = ix->box[2], r1 = ix->box[3];
        if (c0 >= c1) {   /* nothing drawn: the picture area */
            memcpy(l->crop, remove, sizeof(remove));
            return;
        }
        if (f->pres.super_centered) {
            int dc = c0 < CDG_COLS - c1 ? c0 : CDG_COLS - c1, dr = r0 < CDG_ROWS - r1 ? r0 : CDG_ROWS - r1;
            c0 = dc; c1 = CDG_COLS - dc; r0 = dr; r1 = CDG_ROWS - dr;
        }
        l->crop[0] = (uint32_t)(c0 * CDG_TILE_W);
        l->crop[1] = (uint32_t)(r0 * CDG_TILE_H);
        l->crop[2] = (uint32_t)((c1 - c0) * CDG_TILE_W);
        l->crop[3] = (uint32_t)((r1 - r0) * CDG_TILE_H);
        return;
    }
    memcpy(l->crop, zone == FA_SAFETY_REMOVE ? remove : show, sizeof(show));
}

/* The multiple for presentation scaling: the uniform fit of the crop into the
 * presentation size, rounded up; smaller when the buffer or the master would get
 * larger than CDG_MAX_OUTPUT. */
static uint32_t scale_for(const fa_cdg_surface* f, uint32_t cw, uint32_t ch, uint32_t padding)
{
    uint32_t n = 1, nw, nh;
    if (f->present_w) {
        nw = (f->present_w + cw - 1) / cw;
        nh = (f->present_h + ch - 1) / ch;
        n = nw < nh ? nw : nh;
    }
    if (n > CDG_SCALE_MAX_N) n = CDG_SCALE_MAX_N;
    while (n > 1 && ((cw + 2 * (padding + 1) + CDG_TILE_W) * n > CDG_MAX_OUTPUT ||
                     (ch + 2 * (padding + 1) + CDG_TILE_H) * n > CDG_MAX_OUTPUT))
        n--;
    return n;
}

/* The layout the surface's settings and the stream's graphics call for (under
 * cdg_lock: reads the pre-pass). */
static void layout_want(const fa_cdg_surface* f, const fa_cdg_data* d, cdg_layout* l)
{
    uint32_t padding = f->has_pres ? f->pres.padding : 0;
    memset(l, 0, sizeof(*l));
    zone_crop(f, d, l);
    l->scaled = f->has_pres && f->pres.scale;
    if (l->scaled) {
        l->n = scale_for(f, l->crop[2], l->crop[3], padding);
        l->pic_w = l->crop[2] * l->n;
        l->pic_h = l->crop[3] * l->n;
        l->pic_x = l->pic_y = (padding + 1) * l->n;
        l->pad = padding * l->n;
    } else {
        l->n = 1;
        l->pic_w = f->cfg_w ? f->cfg_w : l->crop[2];
        l->pic_h = f->cfg_h ? f->cfg_h : l->crop[3];
        l->pic_x = (padding * l->pic_w + l->crop[2] / 2) / l->crop[2];
        l->pic_y = (padding * l->pic_h + l->crop[3] / 2) / l->crop[3];
        /* A tight crop drawn large would make the padding huge. */
        if (l->pic_x > CDG_MAX_OUTPUT / 4) l->pic_x = CDG_MAX_OUTPUT / 4;
        if (l->pic_y > CDG_MAX_OUTPUT / 4) l->pic_y = CDG_MAX_OUTPUT / 4;
        l->pad = l->pic_x;
    }
    l->buf_w = l->pic_w + 2 * l->pic_x;
    l->buf_h = l->pic_h + 2 * l->pic_y;
}

/* Render-ahead: forgets the slots' pictures (keeping the buffers), so the next
 * update_into draws a frame itself. Caller holds work_lock. */
static void ra_forget(fa_cdg_surface* f)
{
    fa_mutex_lock(&f->pub_lock);
    f->front = f->ready = -1;
    f->slot[0].revision = f->slot[1].revision = 0;
    fa_mutex_unlock(&f->pub_lock);
}

/* Under pub_lock too: update_into copies from a slot without work_lock while a
 * worker or another thread's setter holds it. */
static void ra_free(fa_cdg_surface* f)
{
    fa_mutex_lock(&f->pub_lock);
    f->front = f->ready = -1;
    f->slot[0].revision = f->slot[1].revision = 0;
    free(f->slot[0].px);
    free(f->slot[1].px);
    f->slot[0].px = f->slot[1].px = NULL;
    fa_mutex_unlock(&f->pub_lock);
}

static void free_maps(fa_cdg_surface* f)
{
    int i;
    ra_free(f);
    free(f->xmap);
    free(f->ymap);
    free(f->xlerp);
    free(f->ylerp);
    free(f->master);
    free(f->rows);
    cdg_scaler_free(&f->sc);
    free(f->fxm);
    free(f->fx_scratch);
    keep_free(f);
    fx_help_free(f);
    free(f->m2);
    free(f->f2);
    free(f->rows2);
    cdg_scaler_free(&f->sc2);
    f->m2 = f->f2 = NULL;
    f->rows2 = NULL;
    f->x2 = 0;
    f->fxm = NULL;
    f->fx_scratch = NULL;
    f->fx_scratch_size = 0;
    f->fx_on = 0;
    f->xmap = f->ymap = NULL;
    f->xlerp = f->ylerp = NULL;
    f->master = NULL;
    f->rows = NULL;
    for (i = 0; i < CDG_FX_HELPERS; i++) {
        free(f->rows_help[i]);
        f->rows_help[i] = NULL;
        f->rows_help_size[i] = 0;
    }
}

/* Puts layout `l` in place when it differs from the one in use: new maps or master, a
 * new layout_revision, and the next draw of any buffer is a full one. The scaler
 * follows the options either way. On failure the surface keeps no layout (the next
 * call tries again). */
static fa_result layout_apply(fa_cdg_surface* f, const cdg_layout* l)
{
    int smooth = f->has_pres ? f->pres.smoothness : 0, blend = f->has_pres ? f->pres.edge_blend : 0;
    uint32_t i;
    if (l->scaled) {
        cdg_scaler_init(&f->sc, l->n, smooth, blend);
        /* The effects' 2N master at N 1-3, blended as at N. */
        if (l->n <= 3) cdg_scaler_init_look(&f->sc2, 2 * l->n, l->n, smooth, blend);
    }
    if (f->lay.buf_w && memcmp(l, &f->lay, sizeof(*l)) == 0) return FA_OK;
    free_maps(f);
    free(f->pixels);
    f->pixels = NULL;
    /* update_into without work_lock reads the size and history under pub_lock. */
    fa_mutex_lock(&f->pub_lock);
    f->lay = *l;
    f->stride = (int32_t)(l->buf_w * bytes_per_pixel(f));
    memset(&f->history, 0, sizeof(f->history));
    fa_mutex_unlock(&f->pub_lock);
    f->win_x = l->crop[0];
    f->win_y = l->crop[1];
    f->win_w = l->crop[2];
    f->win_h = l->crop[3];
    f->width = l->pic_w;
    f->height = l->pic_h;
    f->drawn = 0;
    fa_store64(&f->layout_revision, fa_load64(&f->layout_revision) + 1);
    if (l->scaled) {
        int x1 = (int)(l->crop[0] + l->crop[2]) + CDG_TILE_W - 1, y1 = (int)(l->crop[1] + l->crop[3]) + CDG_TILE_H - 1;
        f->mx0 = (int)l->crop[0];
        f->my0 = (int)l->crop[1];
        f->mw = (x1 < FA_CDG_WIDTH ? x1 : FA_CDG_WIDTH) - f->mx0;
        f->mh = (y1 < FA_CDG_HEIGHT ? y1 : FA_CDG_HEIGHT) - f->my0;
        f->master_stride = (size_t)f->mw * l->n * 4;
        f->master = (uint8_t*)malloc(f->master_stride * (size_t)f->mh * l->n);
        f->rows = (uint16_t*)malloc(CDG_SCALE_ROWS_BYTES(l->n));
        if (!f->master || !f->rows) goto oom;
        return FA_OK;
    }
    f->xmap = (uint16_t*)malloc(f->width * sizeof(uint16_t));
    f->ymap = (uint16_t*)malloc(f->height * sizeof(uint16_t));
    if (!f->xmap || !f->ymap) goto oom;
    for (i = 0; i < f->width; i++) f->xmap[i] = (uint16_t)((uint64_t)i * f->win_w / f->width);
    for (i = 0; i < f->height; i++) f->ymap[i] = (uint16_t)((uint64_t)i * f->win_h / f->height);
    if (f->flags & FA_CDG_SMOOTH) {
        f->xlerp = (uint32_t*)malloc(f->width * sizeof(uint32_t));
        f->ylerp = (uint32_t*)malloc(f->height * sizeof(uint32_t));
        if (!f->xlerp || !f->ylerp) goto oom;
        for (i = 0; i < f->width; i++) f->xlerp[i] = lerp_map(i, f->width, f->win_w);
        for (i = 0; i < f->height; i++) f->ylerp[i] = lerp_map(i, f->height, f->win_h);
    }
    return FA_OK;
oom:
    free_maps(f);
    fa_mutex_lock(&f->pub_lock);
    memset(&f->lay, 0, sizeof(f->lay));
    fa_mutex_unlock(&f->pub_lock);
    return FA_ERROR_OUT_OF_MEMORY;
}

/* ---- the public API ------------------------------------------------------------------ */

FA_API void FA_CALL fa_cdg_surface_config_init(fa_cdg_surface_config* config)
{
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->struct_size = (uint32_t)sizeof(*config);
    config->pixel_format = FA_CDG_PIXELS_BGRA32;
}

void fa_stream_cdg_attach(struct fa_stream* s, fa_cdg_data* data)
{
    fa_cdg_data* old;
    fa_mutex_lock(&s->cdg_lock);
    old = s->cdg;
    s->cdg = data;
    s->cdg_gen++;
    fa_mutex_unlock(&s->cdg_lock);
    fa_cdg_data_free(old);
}

fa_result fa_cdg_data_blank(size_t count, fa_cdg_data** out)
{
    uint8_t* bytes;
    *out = NULL;
    if (count == 0 || count > ((size_t)256 << 20) / FA_CDG_PACK_BYTES) return FA_ERROR_INVALID_ARG;
    bytes = (uint8_t*)calloc(count, FA_CDG_PACK_BYTES);
    if (!bytes) return FA_ERROR_OUT_OF_MEMORY;
    return fa_cdg_data_adopt(bytes, count * FA_CDG_PACK_BYTES, out);
}

int fa_stream_cdg_attach_if_none(struct fa_stream* s, fa_cdg_data* data, uint32_t* out_token)
{
    int ok;
    if (!data->index) data->index = index_new(data->count);
    fa_mutex_lock(&s->cdg_lock);
    ok = s->cdg == NULL;
    if (ok) {
        s->cdg = data;
        *out_token = ++s->cdg_gen;
    }
    fa_mutex_unlock(&s->cdg_lock);
    if (!ok) fa_cdg_data_free(data);
    return ok;
}

int fa_stream_cdg_write(struct fa_stream* s, uint32_t token, size_t first, const uint8_t* packs, size_t n)
{
    fa_cdg_surface* f;
    fa_cdg_data* data;
    int ok;
    fa_mutex_lock(&s->cdg_lock);
    data = s->cdg;
    ok = data && s->cdg_gen == token;   /* any load since bumps the generation */
    if (ok && first < data->count && n > 0) {
        uint8_t* at = data->packs + first * FA_CDG_PACK_BYTES;
        if (n > data->count - first) n = data->count - first;
        /* A part read again after a seek usually holds the same packs: nothing to redo. */
        if (memcmp(at, packs, n * FA_CDG_PACK_BYTES) != 0) {
            memcpy(at, packs, n * FA_CDG_PACK_BYTES);
            /* Normally the packs arrive well ahead of what is shown; after a seek they
             * may land behind a surface, which goes back to replay them. */
            for (f = s->cdg_surfaces; f; f = f->next)
                if (f->st.next > first && first < f->stale_from) f->stale_from = first;
            if (s->cdg_watch_next > first) s->cdg_watch_dirty = 1;
            index_written(data, first, n, 1);
        } else {
            index_written(data, first, n, 0);
        }
    }
    fa_mutex_unlock(&s->cdg_lock);
    return ok;
}

static int watch_list(struct fa_stream* s, int on);

/* A surface's `stream` is read and cleared under g_bind. A surface call holds the
 * stream (cdg_users) while it runs, and fa_stream_cdg_detach waits for those calls,
 * so a stream closed on another thread is never torn down under one. */
static fa_lazy_mutex g_bind;

static struct fa_stream* surface_hold(fa_cdg_surface* f)
{
    struct fa_stream* s;
    fa_lazy_mutex_lock(&g_bind);
    s = f->stream;
    if (s) fa_fetch_add32(&s->cdg_users, 1);
    fa_lazy_mutex_unlock(&g_bind);
    return s;
}

static void stream_release(struct fa_stream* s) { fa_fetch_add32(&s->cdg_users, -1); }

/* Render-ahead's queue, engine->cdg_render, is guarded by g_bind too: a queued
 * surface has a stream (closing it takes the surface off), and a running job holds
 * that stream, so the engine is there for as long as either is. Caller holds g_bind. */
static void ra_unqueue(fa_engine* e, fa_cdg_surface* f)
{
    fa_cdg_surface** pp;
    if (!f->ra_queued) return;
    for (pp = &e->cdg_render; *pp; pp = &(*pp)->ra_next)
        if (*pp == f) { *pp = f->ra_next; break; }
    f->ra_queued = 0;
    fa_fetch_add32(&e->cdg_render_count, -1);
}

/* Asks a worker to render ahead for the surface (its stream `s` held by the caller).
 * Not with one worker: a full 2160p frame with effects takes it 50-220 ms, longer than a
 * tempo stream's buffer target, so the audio could run dry; update_into draws then. */
static void ra_request(fa_cdg_surface* f, struct fa_stream* s)
{
    fa_engine* e = s->engine;
    fa_cdg_surface** pp;
    int add;
    if (e->worker_count < 2) return;
    fa_lazy_mutex_lock(&g_bind);
    add = !f->ra_queued && f->stream;
    if (add) {
        for (pp = &e->cdg_render; *pp; pp = &(*pp)->ra_next) {}
        *pp = f;
        f->ra_next = NULL;
        f->ra_queued = 1;
        fa_fetch_add32(&e->cdg_render_count, 1);
    }
    fa_lazy_mutex_unlock(&g_bind);
    if (add) {
        fa_mutex_lock(&e->list_lock);
        fa_cond_signal(&e->work_cond);
        fa_mutex_unlock(&e->list_lock);
    }
}

void fa_stream_cdg_detach(struct fa_stream* s)
{
    fa_cdg_surface* f;
    fa_lazy_mutex_lock(&g_bind);
    fa_mutex_lock(&s->cdg_lock);
    /* Surfaces belong to the app: they outlive the stream until destroyed, and
     * only refuse to update from now on. */
    for (f = s->cdg_surfaces; f; f = f->next) {
        ra_unqueue(s->engine, f);
        f->stream = NULL;
    }
    s->cdg_surfaces = NULL;
    fa_mutex_unlock(&s->cdg_lock);
    fa_lazy_mutex_unlock(&g_bind);
    /* A call that got the stream before this finishes first (it may read the clock). */
    while (fa_load32(&s->cdg_users)) fa_sleep_ms(1);
}

void fa_stream_cdg_close(struct fa_stream* s)
{
    fa_cdg_data* d;
    watch_list(s, 0);
    fa_stream_cdg_detach(s);
    fa_mutex_lock(&s->cdg_lock);
    d = s->cdg;
    s->cdg = NULL;
    fa_mutex_unlock(&s->cdg_lock);
    fa_cdg_data_free(d);
}

static void surface_free(fa_cdg_surface* f)
{
    fa_lyric_free(f->lyric);
    free_maps(f);
    free(f->pixels);
    fa_mutex_destroy(&f->pub_lock);
    fa_mutex_destroy(&f->work_lock);
    free(f);
}

int fa_stream_cdg_present(struct fa_stream* s)
{
    int present;
    fa_mutex_lock(&s->cdg_lock);
    present = s->cdg != NULL;
    fa_mutex_unlock(&s->cdg_lock);
    return present;
}

FA_API fa_result FA_CALL fa_stream_load_cdg_path(fa_stream* stream, const char* path)
{
    fa_cdg_data* data = NULL;
    if (!stream) return FA_ERROR_INVALID_ARG;
    if (path) {
        fa_result r = fa_cdg_data_from_path(path, &data);
        if (r != FA_OK) return r;
    }
    fa_stream_cdg_attach(stream, data);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_load_cdg_memory(fa_stream* stream, const void* data, size_t size)
{
    fa_cdg_data* d;
    uint8_t* copy;
    fa_result r;
    if (!stream || !data) return FA_ERROR_INVALID_ARG;
    if (size < FA_CDG_PACK_BYTES) return FA_ERROR_UNSUPPORTED_FORMAT;
    copy = (uint8_t*)malloc(size);
    if (!copy) return FA_ERROR_OUT_OF_MEMORY;
    memcpy(copy, data, size);
    r = fa_cdg_data_adopt(copy, size, &d);
    if (r != FA_OK) return r;
    fa_stream_cdg_attach(stream, d);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_set_cdg_offset(fa_stream* stream, fa_time offset)
{
    if (!stream) return FA_ERROR_INVALID_ARG;
    fa_store64(&stream->cdg_offset, offset);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_cdg_offset(fa_stream* stream, fa_time* out_offset)
{
    if (!stream || !out_offset) return FA_ERROR_INVALID_ARG;
    *out_offset = fa_load64(&stream->cdg_offset);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_cdg_info(fa_stream* stream, fa_cdg_info* out_info)
{
    const fa_cdg_data* d;
    if (!stream || !out_info) return FA_ERROR_INVALID_ARG;
    memset(out_info, 0, sizeof(*out_info));
    out_info->disc_parity = -1;
    fa_mutex_lock(&stream->cdg_lock);
    d = stream->cdg;
    if (d) {
        out_info->source = d->from_disc ? FA_CDG_SOURCE_DISC : FA_CDG_SOURCE_FILE;
        out_info->pack_count = d->count;
        out_info->checkpoint_interval = (fa_time)index_step(d->count) * 1000000000 / FA_CDG_PACKS_PER_SECOND;
        if (d->index) out_info->checkpoint_count = (uint32_t)d->index->count;
        if (d->from_disc) out_info->disc_parity = d->disc_parity;
    }
    fa_mutex_unlock(&stream->cdg_lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_cdg_surface_create(fa_stream* stream, const fa_cdg_surface_config* config, fa_cdg_surface** out_surface)
{
    fa_cdg_surface_config c;
    fa_cdg_surface* f;
    cdg_layout want;
    fa_result r;
    if (out_surface) *out_surface = NULL;
    if (!stream || !out_surface) return FA_ERROR_INVALID_ARG;
    fa_cdg_surface_config_init(&c);
    if (config) {
        /* Apps built before the background fields pass the shorter struct. */
        if (config->struct_size < CDG_CONFIG_V1_SIZE) return FA_ERROR_INVALID_ARG;
        memcpy(&c, config, config->struct_size < sizeof(c) ? config->struct_size : sizeof(c));
    }
    if ((uint32_t)c.pixel_format > FA_CDG_PIXELS_INDEXED8 || c.width > CDG_MAX_OUTPUT || c.height > CDG_MAX_OUTPUT ||
        (c.flags & ~(uint32_t)CDG_ALL_FLAGS) ||
        ((c.flags & FA_CDG_BACKGROUND_OPACITY) && c.background_opacity > 100))
        return FA_ERROR_INVALID_ARG;
    f = (fa_cdg_surface*)calloc(1, sizeof(*f));
    if (!f) return FA_ERROR_OUT_OF_MEMORY;
    if (fa_mutex_init(&f->work_lock) != 0) { free(f); return FA_ERROR_OUT_OF_MEMORY; }
    if (fa_mutex_init(&f->pub_lock) != 0) {
        fa_mutex_destroy(&f->work_lock);
        free(f);
        return FA_ERROR_OUT_OF_MEMORY;
    }
    f->front = f->ready = -1;
    f->stream = stream;
    f->format = c.pixel_format;
    f->flags = c.flags;
    f->bg_color = c.background_color;
    f->bg_opacity = c.background_opacity;
    f->cfg_w = c.width;
    f->cfg_h = c.height;
    state_reset(&f->st);
    f->stale_from = SIZE_MAX;
    fa_mutex_lock(&stream->cdg_lock);
    index_build(stream->cdg);
    f->gen = stream->cdg_gen;
    layout_want(f, stream->cdg, &want);
    fa_mutex_unlock(&stream->cdg_lock);
    r = layout_apply(f, &want);
    if (r != FA_OK) { surface_free(f); return r; }
    fa_mutex_lock(&stream->cdg_lock);
    f->next = stream->cdg_surfaces;
    stream->cdg_surfaces = f;
    fa_mutex_unlock(&stream->cdg_lock);
    *out_surface = f;
    return FA_OK;
}

FA_API void FA_CALL fa_cdg_surface_destroy(fa_cdg_surface* surface)
{
    struct fa_stream* s;
    fa_cdg_surface** link;
    if (!surface) return;
    fa_lazy_mutex_lock(&g_bind);
    if (surface->stream) ra_unqueue(surface->stream->engine, surface);
    fa_lazy_mutex_unlock(&g_bind);
    while (fa_load32(&surface->ra_running)) fa_sleep_ms(1);   /* a worker's job finishes */
    s = surface_hold(surface);
    if (s) {
        fa_mutex_lock(&s->cdg_lock);
        for (link = &s->cdg_surfaces; *link; link = &(*link)->next)
            if (*link == surface) { *link = surface->next; break; }
        fa_mutex_unlock(&s->cdg_lock);
        stream_release(s);
    }
    surface_free(surface);
}

/* Packs shown at `time` (ns): pack n arrives during [n/300 s, (n+1)/300 s), so all
 * packs that started before `time` are applied: [0, ceil(time * 300 / 1e9)), the rule
 * the header documents. */
static size_t packs_at(fa_time time, size_t count)
{
    int64_t n;
    if (time <= 0) return 0;
    if (time > (INT64_MAX - 999999999) / FA_CDG_PACKS_PER_SECOND) return count;
    n = (time * FA_CDG_PACKS_PER_SECOND + 999999999) / 1000000000;
    return (uint64_t)n < (uint64_t)count ? (size_t)n : count;
}

/* Replaces the surface's state with `from` (NULL = the start), marking only the
 * screen area that differs, so a seek uploads no more than it has to. */
static void restore(fa_cdg_surface* f, const cdg_checkpoint* from)
{
    cdg_state* st = &f->st;
    fa_rects keep = st->dirty;
    int keep_x = st->shift_x, keep_y = st->shift_y, x0 = 0, y0 = 0, x1 = 0, y1 = 0, y;
    uint8_t b[FA_CDG_WIDTH];
    if (!from) memset(b, 0, sizeof(b));
    for (y = 0; y < FA_CDG_HEIGHT; y++) {
        const uint8_t* a = st->screen[y];
        int l = 0, r = FA_CDG_WIDTH;
        if (from) checkpoint_row(from, y, b);
        if (memcmp(a, b, FA_CDG_WIDTH) == 0) continue;
        while (a[l] == b[l]) l++;
        while (a[r - 1] == b[r - 1]) r--;
        if (x0 >= x1) { x0 = l; y0 = y; x1 = r; y1 = y + 1; continue; }
        if (l < x0) x0 = l;
        if (r > x1) x1 = r;
        if (y < y0) y0 = y;
        if (y + 1 > y1) y1 = y + 1;
    }
    if (from) checkpoint_load(from, st); else state_reset(st);
    /* What is marked and moved stays: the new screen differs from the moved picture
     * only there and where it differs from the screen it replaces. */
    st->dirty = keep;
    st->shift_x = keep_x;
    st->shift_y = keep_y;
    if (x0 < x1) mark(st, x0, y0, x1, y1);
}

/* The newest checkpoint at or before pack `pos` (or the start). */
static void rewind_before(fa_cdg_surface* f, const fa_cdg_data* d, size_t pos)
{
    const struct fa_cdg_index* ix = d ? d->index : NULL;
    size_t k = ix ? pos / ix->step : 0;
    if (ix && k > ix->count) k = ix->count;
    restore(f, k > 0 ? &ix->at[k - 1] : NULL);
}

/* Brings the surface's state to `target` packs: from the newest checkpoint at or
 * before it when that saves replaying (any seek back, or a jump forward past one),
 * then pack by pack. */
static void decode_to(fa_cdg_surface* f, const fa_cdg_data* d, size_t target)
{
    const struct fa_cdg_index* ix = d ? d->index : NULL;
    size_t k = ix ? target / ix->step : 0;
    if (ix && k > ix->count) k = ix->count;
    if (target < f->st.next || (k > 0 && k * ix->step > f->st.next))
        rewind_before(f, d, target);
    while (f->st.next < target) {
        apply_pack(&f->st, d->packs + f->st.next * FA_CDG_PACK_BYTES);
        f->st.next++;
    }
}

/* `time` on the stream's timeline minus its CD+G offset, saturated. */
static fa_time graphics_time(struct fa_stream* s, fa_time time)
{
    fa_time offset = fa_load64(&s->cdg_offset);
    if (offset > 0 && time < INT64_MIN + offset) return INT64_MIN;
    if (offset < 0 && time > INT64_MAX + offset) return INT64_MAX;
    return time - offset;
}

/* Brings the surface's decoder to `time` on the stream's timeline; returns the
 * graphics time. `want` gets the layout to apply. `s` is the surface's stream, held
 * by the caller. */
static fa_time sync_state(fa_cdg_surface* f, struct fa_stream* s, fa_time time, cdg_layout* want)
{
    size_t target;
    int i;
    f->eng = s->engine;           /* the stream is held: its engine is there while drawing */
    fa_mutex_lock(&s->cdg_lock);
    time = graphics_time(s, time);
    if (f->gen != s->cdg_gen) {
        index_build(s->cdg);      /* graphics loaded since the surface was created */
        restore(f, NULL);
        f->gen = s->cdg_gen;
        f->stale_from = SIZE_MAX;
    }
    if (f->stale_from != SIZE_MAX) {
        /* Packs from stale_from changed: back to the checkpoint before them (the
         * stream's own were redone), and on from there to the target below. */
        if (f->st.next > f->stale_from) rewind_before(f, s->cdg, f->stale_from);
        fa_mutex_lock(&f->pub_lock);   /* render-ahead's frames drawn from them, too */
        for (i = 0; i < 2; i++)
            if (f->slot[i].target > f->stale_from) f->slot[i].stale = 1;
        fa_mutex_unlock(&f->pub_lock);
        f->stale_from = SIZE_MAX;
    }
    target = s->cdg ? packs_at(time, s->cdg->count) : 0;
    decode_to(f, s->cdg, target);
    layout_want(f, s->cdg, want);
    fa_mutex_unlock(&s->cdg_lock);
    return time;
}

static fa_result surface_update(fa_cdg_surface* f, struct fa_stream* s, fa_time time, fa_cdg_frame* out)
{
    cdg_layout want;
    fa_result r;
    time = sync_state(f, s, time, &want);
    /* Drawing uses only this surface's state: other surfaces and loads need not wait. */
    r = layout_apply(f, &want);
    if (r == FA_OK) r = render(f, out);
    out->time = time;
    return r;
}

/* ---- render-ahead ------------------------------------------------------------------ */

/* Without a frame for this long before, update_into asks for the next frame this far
 * ahead (a 60 Hz display); a longer gap than RA_LEAD_MAX is a seek, not a pace. */
#define RA_LEAD_DEFAULT ((fa_time)16666667)
#define RA_LEAD_MAX ((fa_time)100000000)
/* Packs between two frames looked at to tell whether a frame is still exact. */
#define RA_LOOK_PACKS 600
/* A frame this many packs older than wanted (1/60 s) is still on time. */
#define RA_ON_TIME_PACKS 5

static int ra_on(const fa_cdg_surface* f) { return f->render_ahead && f->lay.scaled; }

/* 1 when packs [a, b) may change the picture (anything but tiles, scrolls, presets
 * and colours does not). Caller holds cdg_lock. */
static int packs_visible(const fa_cdg_data* d, size_t a, size_t b)
{
    if (b - a > RA_LOOK_PACKS) return 1;
    for (; a < b; a++) {
        const uint8_t* p = d->packs + a * FA_CDG_PACK_BYTES;
        int op = p[1] & 0x3F;
        if ((p[0] & 0x3F) != CDG_MODE_GRAPHICS) continue;
        if (op == CDG_TILE_NORMAL || op == CDG_TILE_XOR || op == CDG_SCROLL_PRESET || op == CDG_SCROLL_COPY ||
            op == CDG_MEMORY_PRESET || op == CDG_BORDER_PRESET || op == CDG_TRANSPARENT || op == CDG_COLORS_LOW ||
            op == CDG_COLORS_HIGH)
            return 1;
    }
    return 0;
}

#define FIT_NONE 0                /* no slot, or a picture of other graphics */
#define FIT_AHEAD 1               /* of a later position */
#define FIT_BEHIND 2              /* of an earlier position */
#define FIT_CLOSE 3               /* of an earlier position, on time (RA_ON_TIME_PACKS) */
#define FIT_EXACT 4               /* the picture of `target` packs */

/* How slot k's picture fits the picture of `target` packs. Caller holds cdg_lock and
 * pub_lock. */
static int slot_fit(const fa_cdg_surface* f, const struct fa_stream* s, int k, size_t target)
{
    const cdg_slot* sl;
    const fa_cdg_data* d = s->cdg;
    if (k < 0) return FIT_NONE;
    sl = &f->slot[k];
    if (!sl->revision || sl->stale || sl->gen != s->cdg_gen || f->stale_from != SIZE_MAX) return FIT_NONE;
    if (sl->target == target) return FIT_EXACT;
    if (!d) return FIT_NONE;
    if (sl->target < target) {
        if (!packs_visible(d, sl->target, target)) return FIT_EXACT;
        return target - sl->target <= RA_ON_TIME_PACKS ? FIT_CLOSE : FIT_BEHIND;
    }
    return packs_visible(d, target, sl->target) ? FIT_AHEAD : FIT_EXACT;
}

/* Renders the decoder's state into slot k. Caller holds work_lock, and k is neither
 * the slot shown nor the one finished next. */
static void ra_render(fa_cdg_surface* f, int k, fa_time gtime, int64_t start)
{
    cdg_slot* sl = &f->slot[k];
    uint8_t pal[16][4];
    fa_rects drawn;
    advance(f, pal);
    draw(f, (const uint8_t(*)[4])pal, sl->px, f->stride, sl->revision, &drawn);
    sl->revision = f->revision;
    sl->gen = f->gen;
    sl->stale = 0;
    sl->target = f->st.next;
    sl->time = gtime;
    memcpy(sl->pal, pal, sizeof(pal));
    frame_state(f, (const uint8_t(*)[4])pal, &sl->bg, &sl->border, sl->background, sl->transparency);
    sl->palette_revision = f->palette_revision;
    sl->render_ns = fa_monotonic_ns() - start;
}

/* A worker's job: the frame for ra_time into the free slot, finished next. A new
 * layout waits for the app's next update_into. */
static void lyric_ra_job(fa_cdg_surface* f, struct fa_stream* s);

static void ra_job(fa_cdg_surface* f, struct fa_stream* s)
{
    cdg_layout want;
    fa_time t = 0, gtime;
    int64_t start;
    int k = -1;
    if (f->lyric) {
        lyric_ra_job(f, s);
        return;
    }
    fa_mutex_lock(&f->work_lock);
    if (ra_on(f) && f->slot[0].px) {
        fa_mutex_lock(&f->pub_lock);
        if (f->front >= 0 && f->ready < 0) k = 1 - f->front;
        t = f->ra_time;
        fa_mutex_unlock(&f->pub_lock);
    }
    if (k >= 0) {
        start = fa_monotonic_ns();
        gtime = sync_state(f, s, t, &want);
        if (memcmp(&want, &f->lay, sizeof(want)) == 0) {
            ra_render(f, k, gtime, start);
            fa_mutex_lock(&f->pub_lock);
            f->ready = k;
            fa_mutex_unlock(&f->pub_lock);
        }
    }
    fa_mutex_unlock(&f->work_lock);
}

int fa_cdg_render_one(fa_engine* e)
{
    fa_cdg_surface* f;
    struct fa_stream* s = NULL;
    fa_lazy_mutex_lock(&g_bind);
    f = e->cdg_render;
    if (f) {
        ra_unqueue(e, f);
        s = f->stream;            /* queued surfaces have one */
        fa_fetch_add32(&s->cdg_users, 1);
        fa_fetch_add32(&f->ra_running, 1);   /* a count: the app may queue it again meanwhile */
    }
    fa_lazy_mutex_unlock(&g_bind);
    if (!f) return 0;
    ra_job(f, s);
    fa_fetch_add32(&f->ra_running, -1);  /* at 0 fa_cdg_surface_destroy may free it */
    stream_release(s);
    return 1;
}

static void to_rect(const int r[4], fa_rect* out)
{
    out->x = r[0];
    out->y = r[1];
    out->width = (uint32_t)(r[2] - r[0]);
    out->height = (uint32_t)(r[3] - r[1]);
}

#define RA_NEED_LOCK 1            /* ra_show: a frame must be drawn here, and work_lock is not held */

/* update_into with render-ahead: picks the finished frame that fits `time` best,
 * copies what changed since dst_revision from it, and asks a worker for the next one.
 * When there is no frame yet (the first, or after a change of layout or options) or
 * the best is late by more than 1/60 s, ahead of `time` or of other graphics, the
 * frame for `time` is drawn here instead, which needs work_lock (`held`); without it
 * (a worker is rendering) a frame that exists is shown, flagged behind. The layout in
 * use is read: a worker never changes it. */
static int ra_show(fa_cdg_surface* f, struct fa_stream* s, fa_time time, uint8_t* dst, int32_t dst_stride,
                   uint32_t dst_width, uint32_t dst_height, uint64_t dst_revision, int held, fa_surface_frame* fr,
                   fa_result* result)
{
    const cdg_slot* sl;
    fa_rects drawn;
    fa_time lead, ahead_time;
    size_t target = 0, ahead = 0;
    int i, fit = FIT_NONE, need, partial, b[4], drew = 0;
    uint32_t bpp = 4;
    *result = FA_OK;
    if (held && !f->slot[0].px) {
        size_t size = (size_t)f->lay.buf_h * (size_t)f->stride;
        f->slot[0].px = (uint8_t*)malloc(size);
        f->slot[1].px = (uint8_t*)malloc(size);
        if (!f->slot[0].px || !f->slot[1].px) {
            ra_free(f);
            *result = FA_ERROR_OUT_OF_MEMORY;
            return 0;
        }
    }
    /* The pace of the calls: paused or back, the frame of `time` itself. */
    if (time <= f->ra_last) lead = 0;
    else if ((uint64_t)time - (uint64_t)f->ra_last > (uint64_t)RA_LEAD_MAX) lead = RA_LEAD_DEFAULT;
    else lead = time - f->ra_last;
    /* Half a pack short of the next call's time: a frame a hair ahead of it would
     * wait a whole call longer to be shown. */
    if (lead > 1000000000 / FA_CDG_PACKS_PER_SECOND / 2) lead -= 1000000000 / FA_CDG_PACKS_PER_SECOND / 2;
    ahead_time = time > INT64_MAX - lead ? INT64_MAX : time + lead;
    for (;;) {
        int ff, fr_fit, k;
        fa_mutex_lock(&s->cdg_lock);
        if (s->cdg) {
            target = packs_at(graphics_time(s, time), s->cdg->count);
            ahead = packs_at(graphics_time(s, ahead_time), s->cdg->count);
        }
        fa_mutex_lock(&f->pub_lock);
        /* The ready frame replaces the one shown when it fits `time` better. */
        ff = slot_fit(f, s, f->front, target);
        fr_fit = slot_fit(f, s, f->ready, target);
        if (fr_fit != FIT_NONE &&
            (fr_fit > ff || (fr_fit == ff && (fr_fit == FIT_EXACT ||
                                                (fr_fit != FIT_AHEAD && f->slot[f->ready].target > f->slot[f->front].target) ||
                                                (fr_fit == FIT_AHEAD && f->slot[f->ready].target < f->slot[f->front].target))))) {
            f->front = f->ready;
            f->ready = -1;
            ff = fr_fit;
        } else if (f->ready >= 0 && !(fr_fit == FIT_AHEAD && f->slot[f->ready].target <= ahead)) {
            f->ready = -1;        /* of no use: the worker renders another */
        }
        fit = ff;
        if ((f->front >= 0 && ff >= FIT_CLOSE) || drew) break;
        if (!held) {
            if (f->front >= 0) break;   /* shown late rather than waiting for the worker */
            fa_mutex_unlock(&f->pub_lock);
            fa_mutex_unlock(&s->cdg_lock);
            return RA_NEED_LOCK;
        }
        /* Draw the frame for `time` here (no worker is rendering: the lock is held). */
        f->ready = -1;
        k = f->front < 0 ? 0 : 1 - f->front;
        fa_mutex_unlock(&f->pub_lock);
        fa_mutex_unlock(&s->cdg_lock);
        {
            int64_t start = fa_monotonic_ns();
            cdg_layout want;
            fa_time gtime = sync_state(f, s, time, &want);
            if (memcmp(&want, &f->lay, sizeof(want)) != 0) {
                *result = FA_ERROR_SIZE_MISMATCH;   /* the layout changed just now: get it again */
                return 0;
            }
            ra_render(f, k, gtime, start);
            fa_mutex_lock(&f->pub_lock);
            f->front = k;
            fa_mutex_unlock(&f->pub_lock);
        }
        drew = 1;
    }
    /* The frame's description while the slots cannot change. */
    sl = &f->slot[f->front];
    partial = fa_history_since(&f->history, sl->revision, dst_revision, &drawn);
    fr->revision = sl->revision;
    fr->time = sl->time;
    fr->render_ns = sl->render_ns;
    fr->behind = fit < FIT_CLOSE;
    fr->background_index = sl->bg;
    fr->border_index = sl->border;
    memcpy(fr->background, sl->background, sizeof(fr->background));
    memcpy(fr->palette, sl->pal, sizeof(fr->palette));
    memcpy(fr->transparency, sl->transparency, sizeof(fr->transparency));
    fr->palette_revision = sl->palette_revision;
    /* The next frame is wanted unless one is shown or on its way for `ahead`. */
    need = f->ready < 0 && slot_fit(f, s, f->front, ahead) != FIT_EXACT;
    f->ra_time = ahead_time;
    fa_mutex_unlock(&s->cdg_lock);
    /* Without work_lock another thread's setter may change the layout or free the
     * slots, both under pub_lock: the size is checked, and the copy made, under it.
     * A worker draws only into the other slot meanwhile. */
    if (!held && (dst_width != f->lay.buf_w || dst_height != f->lay.buf_h || dst_stride < f->stride)) {
        fa_mutex_unlock(&f->pub_lock);
        *result = dst_stride < f->stride && dst_width == f->lay.buf_w && dst_height == f->lay.buf_h
                      ? FA_ERROR_INVALID_ARG : FA_ERROR_SIZE_MISMATCH;
        return 0;
    }
    if (!partial) {
        fa_rects_clear(&drawn);
        fa_rects_add(&drawn, 0, 0, (int)f->lay.buf_w, (int)f->lay.buf_h);
    }
    for (i = 0; i < drawn.n; i++) {
        const int* r = drawn.r[i];
        int y;
        for (y = r[1]; y < r[3]; y++)
            memcpy(dst + (size_t)y * (size_t)dst_stride + (size_t)r[0] * bpp,
                   sl->px + (size_t)y * (size_t)f->stride + (size_t)r[0] * bpp, (size_t)(r[2] - r[0]) * bpp);
    }
    fa_mutex_unlock(&f->pub_lock);
    f->ra_last = time;
    if (need) ra_request(f, s);
    fr->base_revision = partial ? dst_revision : 0;
    fr->dirty_count = (uint32_t)drawn.n;
    for (i = 0; i < drawn.n; i++) to_rect(drawn.r[i], &fr->dirty[i]);
    fa_rects_bounds(&drawn, b);
    to_rect(b, &fr->dirty_bounds);
    return 0;
}

/* ---- change events ---------------------------------------------------------------- */

/* Adds the stream to (or takes it off) the engine's list the event thread watches,
 * and sets cdg_events with it, both under list_lock so they always agree. A closed
 * stream is never added: 0 then. */
static int watch_list(struct fa_stream* s, int on)
{
    fa_engine* e = s->engine;
    struct fa_stream** link;
    int ok = 1;
    if (!e) return 0;   /* a stream that failed to open */
    fa_mutex_lock(&e->list_lock);
    if (on && fa_load32(&s->closed)) {
        ok = 0;
    } else if (on && !s->cdg_watch_listed) {
        s->cdg_watch_link = e->cdg_watched;
        e->cdg_watched = s;
        s->cdg_watch_listed = 1;
    } else if (!on && s->cdg_watch_listed) {
        for (link = &e->cdg_watched; *link; link = &(*link)->cdg_watch_link)
            if (*link == s) { *link = s->cdg_watch_link; break; }
        s->cdg_watch_listed = 0;
    }
    if (ok) fa_store32(&s->cdg_events, on ? 1 : 0);
    fa_mutex_unlock(&e->list_lock);
    return ok;
}

FA_API fa_result FA_CALL fa_stream_set_cdg_events(fa_stream* stream, int32_t enabled)
{
    if (!stream) return FA_ERROR_INVALID_ARG;
    if (enabled) {
        /* Before the stream is watched, so the first round sees this. */
        fa_mutex_lock(&stream->cdg_lock);
        stream->cdg_watch_dirty = 1;   /* the first event asks for a whole picture */
        stream->cdg_watch_gen = stream->cdg_gen;
        index_build(stream->cdg);      /* a seek report reads the checkpoints */
        fa_mutex_unlock(&stream->cdg_lock);
    }
    return watch_list(stream, enabled != 0) ? FA_OK : FA_ERROR_INVALID_ARG;
}

/* 1 when a scroll pack's data sets smooth-scroll offsets other than 0. */
static int scroll_shifted(const uint8_t* d) { return (d[1] & 0x07) != 0 || (d[2] & 0x0F) != 0; }

/* Smooth-scroll offsets in force after packs [0, upto): from the last scroll pack,
 * looking back no further than the checkpoint before `upto`. */
static int shifted_at(const fa_cdg_data* d, size_t upto)
{
    const struct fa_cdg_index* ix = d->index;
    size_t k = ix ? upto / ix->step : 0, base;
    if (ix && k > ix->count) k = ix->count;
    base = k ? k * ix->step : 0;
    while (upto > base) {
        const uint8_t* p = d->packs + --upto * FA_CDG_PACK_BYTES;
        int op = p[1] & 0x3F;
        if ((p[0] & 0x3F) == CDG_MODE_GRAPHICS && (op == CDG_SCROLL_PRESET || op == CDG_SCROLL_COPY))
            return scroll_shifted(p + 4);
    }
    return k ? ix->at[k - 1].hoff != 0 || ix->at[k - 1].voff != 0 : 0;
}

static void grow(int* r, int x0, int y0, int x1, int y1)
{
    if (r[0] >= r[2]) { r[0] = x0; r[1] = y0; r[2] = x1; r[3] = y1; return; }
    if (x0 < r[0]) r[0] = x0;
    if (y0 < r[1]) r[1] = y0;
    if (x1 > r[2]) r[2] = x1;
    if (y1 > r[3]) r[3] = y1;
}

/* What packs [from, to) change on the screen, without decoding them: the area tiles
 * cover, the whole screen for clears, borders and scrolls, and `palette` for colour
 * and transparency loads. Over-reports at worst (a repeated clear). */
static void pack_changes(struct fa_stream* s, const fa_cdg_data* d, size_t from, size_t to, int* r, int* palette)
{
    for (; from < to; from++) {
        const uint8_t* p = d->packs + from * FA_CDG_PACK_BYTES;
        const uint8_t* dd = p + 4;
        if ((p[0] & 0x3F) != CDG_MODE_GRAPHICS) continue;
        switch (p[1] & 0x3F) {
        case CDG_TILE_NORMAL:
        case CDG_TILE_XOR: {
            int row = dd[2] & 0x1F, col = dd[3] & 0x3F;
            if (row >= CDG_ROWS || col >= CDG_COLS) break;
            if (s->cdg_watch_shifted) grow(r, 0, 0, FA_CDG_WIDTH, FA_CDG_HEIGHT);
            else grow(r, col * CDG_TILE_W, row * CDG_TILE_H, (col + 1) * CDG_TILE_W, (row + 1) * CDG_TILE_H);
            break;
        }
        case CDG_SCROLL_PRESET:
        case CDG_SCROLL_COPY:
            s->cdg_watch_shifted = scroll_shifted(dd);
            grow(r, 0, 0, FA_CDG_WIDTH, FA_CDG_HEIGHT);   /* the picture moves */
            break;
        case CDG_MEMORY_PRESET:
        case CDG_BORDER_PRESET:
            grow(r, 0, 0, FA_CDG_WIDTH, FA_CDG_HEIGHT);
            break;
        case CDG_TRANSPARENT:
        case CDG_COLORS_LOW:
        case CDG_COLORS_HIGH:
            *palette = 1;
            break;
        default:
            break;
        }
    }
}

void fa_stream_cdg_watch(struct fa_stream* s)
{
    fa_time now;
    fa_cdg_data* d;
    size_t target;
    int r[4] = { 0, 0, 0, 0 }, palette = 0;
    /* Like a callback: fa_stream_close sets `closed`, then waits for in_callback 0
     * before it frees what the clock reads, so either it sees us or we see it. */
    fa_store32(&s->in_callback, 1);
    if (!fa_load32(&s->cdg_events) || fa_load32(&s->closed) || fa_stream_get_clock(s, &now) != FA_OK) {
        fa_store32(&s->in_callback, 0);
        return;
    }
    fa_mutex_lock(&s->cdg_lock);
    d = s->cdg;
    target = d ? packs_at(graphics_time(s, now), d->count) : 0;
    if (s->cdg_watch_gen != s->cdg_gen || s->cdg_watch_dirty || target < s->cdg_watch_next ||
        target - s->cdg_watch_next > CDG_CHECKPOINT_PACKS) {
        /* New graphics, rewritten packs, a seek back or a long jump: the whole screen. */
        grow(r, 0, 0, FA_CDG_WIDTH, FA_CDG_HEIGHT);
        palette = 1;
        s->cdg_watch_shifted = d ? shifted_at(d, target) : 0;
        s->cdg_watch_gen = s->cdg_gen;
        s->cdg_watch_dirty = 0;
    } else if (d) {
        pack_changes(s, d, s->cdg_watch_next, target, r, &palette);
    }
    s->cdg_watch_next = target;
    fa_mutex_unlock(&s->cdg_lock);
    fa_store32(&s->in_callback, 0);
    if (r[0] < r[2] || palette) {
        fa_event ev;
        ev.type = FA_EVENT_CDG_CHANGED;
        ev.stream = s;
        ev.frame = (fa_frames)target;
        ev.value = FA_CDG_CHANGE_MAKE(r[0], r[1], r[2] - r[0], r[3] - r[1], palette);
        fa_engine_post_event(s->engine, &ev);
    }
}

FA_API fa_result FA_CALL fa_cdg_surface_update(fa_cdg_surface* surface, fa_cdg_frame* out_frame)
{
    struct fa_stream* s;
    fa_time now;
    fa_result r;
    if (!surface || !out_frame || surface->lyric) return FA_ERROR_INVALID_ARG;
    s = surface_hold(surface);
    if (!s) return FA_ERROR_INVALID_ARG; /* its stream was closed */
    r = fa_stream_get_clock(s, &now);
    fa_mutex_lock(&surface->work_lock);
    if (r == FA_OK) r = surface_update(surface, s, now, out_frame);
    fa_mutex_unlock(&surface->work_lock);
    stream_release(s);
    return r;
}

FA_API fa_result FA_CALL fa_cdg_surface_update_at(fa_cdg_surface* surface, fa_time time, fa_cdg_frame* out_frame)
{
    struct fa_stream* s;
    fa_result r;
    if (!surface || !out_frame || surface->lyric) return FA_ERROR_INVALID_ARG;
    s = surface_hold(surface);
    if (!s) return FA_ERROR_INVALID_ARG; /* its stream was closed */
    fa_mutex_lock(&surface->work_lock);
    r = surface_update(surface, s, time, out_frame);
    fa_mutex_unlock(&surface->work_lock);
    stream_release(s);
    return r;
}

/* ---- fa_surface ----------------------------------------------------------------- */

FA_API void FA_CALL fa_surface_destroy(fa_surface* surface) { fa_cdg_surface_destroy(surface); }

FA_API fa_result FA_CALL fa_surface_set_presentation_size(fa_surface* surface, uint32_t width, uint32_t height)
{
    fa_result r;
    if (!surface) return FA_ERROR_INVALID_ARG;
    r = fa_presentation_size_clamp(&width, &height);
    if (r != FA_OK) return r;
    /* Used by presentation scaling, at the next update or layout. */
    fa_mutex_lock(&surface->work_lock);
    /* A lyric surface's slots are of the old area: the next update_into draws. */
    if (surface->lyric && (surface->present_w != width || surface->present_h != height)) ra_forget(surface);
    surface->present_w = width;
    surface->present_h = height;
    fa_mutex_unlock(&surface->work_lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_surface_set_render_ahead(fa_surface* surface, int32_t enabled)
{
    if (!surface || (enabled != 0 && enabled != 1)) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&surface->work_lock);
    if (!enabled) ra_free(surface);
    surface->render_ahead = enabled;
    fa_mutex_unlock(&surface->work_lock);
    return FA_OK;
}

/* The layout the surface's settings call for now, in place; `s` is its stream, held,
 * or NULL when it was closed. Caller holds work_lock. */
static fa_result layout_refresh(fa_cdg_surface* f, struct fa_stream* s)
{
    cdg_layout want;
    if (!s) {
        /* Its stream was closed: the layout in use, or the one without graphics. */
        if (f->lay.buf_w) return FA_OK;
        layout_want(f, NULL, &want);
        return layout_apply(f, &want);
    }
    fa_mutex_lock(&s->cdg_lock);
    if (f->gen != s->cdg_gen) index_build(s->cdg);   /* graphics loaded since: their pre-pass */
    layout_want(f, s->cdg, &want);
    fa_mutex_unlock(&s->cdg_lock);
    return layout_apply(f, &want);
}

FA_API fa_result FA_CALL fa_surface_get_layout(fa_surface* surface, fa_surface_layout* out_layout)
{
    struct fa_stream* s;
    fa_surface_layout l;
    const cdg_layout* c;
    fa_result r;
    if (!surface || !out_layout || out_layout->struct_size < sizeof(uint32_t)) return FA_ERROR_INVALID_ARG;
    if (surface->lyric) {
        memset(&l, 0, sizeof(l));
        fa_mutex_lock(&surface->work_lock);
        r = fa_lyric_get_layout(surface->lyric, surface->present_w, surface->present_h, &l);
        if (r == FA_OK && l.layout_revision != surface->ra_layout) ra_forget(surface);
        fa_store64(&surface->layout_revision, (int64_t)l.layout_revision);
        fa_mutex_unlock(&surface->work_lock);
        if (r != FA_OK) return r;
        l.struct_size = out_layout->struct_size < sizeof(l) ? out_layout->struct_size : (uint32_t)sizeof(l);
        memcpy(out_layout, &l, l.struct_size);
        return FA_OK;
    }
    s = surface_hold(surface);
    fa_mutex_lock(&surface->work_lock);
    r = layout_refresh(surface, s);
    if (s) stream_release(s);
    if (r != FA_OK) {
        fa_mutex_unlock(&surface->work_lock);
        return r;
    }
    c = &surface->lay;
    memset(&l, 0, sizeof(l));
    l.struct_size = out_layout->struct_size < sizeof(l) ? out_layout->struct_size : (uint32_t)sizeof(l);
    l.width = c->buf_w;
    l.height = c->buf_h;
    if (c->scaled && surface->present_w) {
        l.bitmap_x = ((int32_t)surface->present_w - (int32_t)c->buf_w) / 2;
        l.bitmap_y = ((int32_t)surface->present_h - (int32_t)c->buf_h) / 2;
    }
    l.content_x = c->pic_x;
    l.content_y = c->pic_y;
    l.content_width = c->pic_w;
    l.content_height = c->pic_h;
    l.scale = (float)c->n;
    l.crop_x = c->crop[0];
    l.crop_y = c->crop[1];
    l.crop_width = c->crop[2];
    l.crop_height = c->crop[3];
    l.safety_zone = (fa_safety_zone)c->zone;
    l.safety_zone_resolved = c->resolved;
    l.margin = c->scaled ? c->n : 0;
    l.padding = c->pad;
    l.layout_revision = (uint64_t)fa_load64(&surface->layout_revision);
    fa_mutex_unlock(&surface->work_lock);
    memcpy(out_layout, &l, l.struct_size);
    return FA_OK;
}

/* ---- render-ahead for a lyric surface ----
 *
 * The same two slots: a slot is a buffer of the lyric bitmap that fa_lyric_update
 * brings to a time from the revision it holds, with a copy of the history as of that
 * revision so update_into can copy from it to the app's buffer without the lyric's
 * lock. A frame fits a time when it is of that time or up to 1/60 s before it (or a
 * hair after); lyrics move continuously, so there is no "same picture" test as for
 * CD+G packs. A later frame is shown, flagged behind, while the worker catches up;
 * only with none of the layout, one ahead of `time` or one a seek away is the frame
 * drawn on the caller's thread. */

#define LY_RA_CLOSE ((fa_time)16666667)

/* LY_TEMPO_STEP: the tempo a lyric surface schedules with, in steps of 0.25%. During
 * a ramp, or a DJ sync, the stream's tempo moves every block; with the exact value
 * every slot misses (lyric_fit) and the timeline is scheduled again each frame. The
 * tempo only sets the animations' lengths (params_make), where 0.25% is at most a
 * millisecond or two of a scroll or page turn; the words' times are the stream's own. */
#define LY_TEMPO_STEP 1.0025

/* Read under tempo_lock alone: fa_stream_get_param takes api_lock, which a seek holds
 * across its prefill, and the render thread and workers would wait for it. */
static float stream_tempo(struct fa_stream* s)
{
    float tempo;
    double q;
    fa_mutex_lock(&s->tempo_lock);
    tempo = s->tparams[FA_PARAM_TEMPO - FA_PARAM_RATE].value;
    fa_mutex_unlock(&s->tempo_lock);
    if (!(tempo > 0.0f)) return 1.0f;
    q = floor(log((double)tempo) / log(LY_TEMPO_STEP) + 0.5);
    return (float)pow(LY_TEMPO_STEP, q);
}

/* Slot k's fit for `time`; caller holds pub_lock. */
static int lyric_fit(const fa_cdg_surface* f, int k, fa_time time, float tempo)
{
    const cdg_slot* sl;
    if (k < 0) return FIT_NONE;
    sl = &f->slot[k];
    if (!sl->revision || sl->layout_rev != f->ra_layout || sl->tempo != tempo) return FIT_NONE;
    if (time == sl->time) return FIT_EXACT;
    if (time > sl->time) return (uint64_t)time - (uint64_t)sl->time <= (uint64_t)LY_RA_CLOSE ? FIT_CLOSE : FIT_BEHIND;
    return (uint64_t)sl->time - (uint64_t)time <= (uint64_t)(LY_RA_CLOSE / 2) ? FIT_CLOSE : FIT_AHEAD;
}

/* Draws the frame for `time` into slot k. Caller holds work_lock; k is neither the
 * slot shown nor the one finished next. */
static fa_result lyric_ra_render(fa_cdg_surface* f, int k, fa_time time, float tempo)
{
    cdg_slot* sl = &f->slot[k];
    fa_surface_frame fr;
    int64_t start = fa_monotonic_ns();
    fa_result r;
    memset(&fr, 0, sizeof(fr));
    r = fa_lyric_update(f->lyric, f->present_w, f->present_h, (double)tempo, time, sl->px, (int32_t)f->ra_w * 4, f->ra_w,
                        f->ra_h, sl->revision, &fr);
    if (r != FA_OK) return r;
    sl->revision = fr.revision;
    sl->time = time;
    sl->tempo = tempo;
    sl->layout_rev = fa_lyric_layout_revision(f->lyric);
    sl->hist = *fa_lyric_history(f->lyric);
    sl->render_ns = fa_monotonic_ns() - start;
    return FA_OK;
}

/* A worker's job: the frame for ra_time into the free slot. A new layout (a new
 * presentation size or style) waits for the app's next update_into. */
static void lyric_ra_job(fa_cdg_surface* f, struct fa_stream* s)
{
    fa_time t = 0;
    float tempo = stream_tempo(s);
    int k = -1;
    fa_mutex_lock(&f->work_lock);
    if (f->render_ahead && f->slot[0].px && fa_lyric_ready_for(f->lyric, f->present_w, f->present_h) &&
        fa_lyric_layout_revision(f->lyric) == f->ra_layout) {
        fa_mutex_lock(&f->pub_lock);
        if (f->front >= 0 && f->ready < 0) k = 1 - f->front;
        t = f->ra_time;
        fa_mutex_unlock(&f->pub_lock);
    }
    if (k >= 0 && lyric_ra_render(f, k, t, tempo) == FA_OK) {
        fa_mutex_lock(&f->pub_lock);
        f->ready = k;
        fa_mutex_unlock(&f->pub_lock);
    }
    fa_mutex_unlock(&f->work_lock);
}

/* update_into for a lyric surface with render-ahead (as ra_show for CD+G). `held`:
 * work_lock is held, so the layout is current and a frame can be drawn here. */
static fa_result lyric_ra_update(fa_cdg_surface* f, struct fa_stream* s, fa_time time, uint8_t* dst, int32_t dst_stride,
                                 uint32_t dst_w, uint32_t dst_h, uint64_t dst_revision, int held, fa_surface_frame* fr)
{
    const cdg_slot* sl;
    fa_rects copy;
    fa_time lead, ahead_time;
    float tempo = stream_tempo(s);
    int i, fit, need, partial, b[4];
    if (held) {
        fa_surface_layout lay;
        fa_result r;
        memset(&lay, 0, sizeof(lay));
        r = fa_lyric_get_layout(f->lyric, f->present_w, f->present_h, &lay);
        if (r != FA_OK) return r;
        fa_store64(&f->layout_revision, (int64_t)lay.layout_revision);
        if (lay.layout_revision != f->ra_layout || !f->slot[0].px) {
            size_t size = (size_t)lay.width * (size_t)lay.height * 4;
            uint8_t *p0, *p1;
            ra_free(f);
            p0 = (uint8_t*)malloc(size);
            p1 = (uint8_t*)malloc(size);
            if (!p0 || !p1) {
                free(p0);
                free(p1);
                return FA_ERROR_OUT_OF_MEMORY;
            }
            /* Under pub_lock: an update_into without work_lock checks them there. */
            fa_mutex_lock(&f->pub_lock);
            f->ra_layout = lay.layout_revision;
            f->ra_w = lay.width;
            f->ra_h = lay.height;
            f->slot[0].px = p0;
            f->slot[1].px = p1;
            fa_mutex_unlock(&f->pub_lock);
        }
    }
    /* The pace of the calls, as for CD+G. */
    if (time <= f->ra_last) lead = 0;
    else if ((uint64_t)time - (uint64_t)f->ra_last > (uint64_t)RA_LEAD_MAX) lead = RA_LEAD_DEFAULT;
    else lead = time - f->ra_last;
    ahead_time = time > INT64_MAX - lead ? INT64_MAX : time + lead;
    fa_mutex_lock(&f->pub_lock);
    /* Without work_lock another thread's setter may have changed the slots (always
     * under pub_lock): the caller takes the lock and comes back. */
    if (!f->slot[0].px || dst_w != f->ra_w || dst_h != f->ra_h) {
        fa_mutex_unlock(&f->pub_lock);
        return held ? FA_ERROR_SIZE_MISMATCH : FA_ERROR_BUSY;
    }
    if (dst_stride < (int32_t)f->ra_w * 4) {
        fa_mutex_unlock(&f->pub_lock);
        return FA_ERROR_INVALID_ARG;
    }
    for (;;) {
        int ff = lyric_fit(f, f->front, time, tempo), rf = lyric_fit(f, f->ready, time, tempo), k;
        if (rf != FIT_NONE && (rf > ff || (rf == ff && (rf == FIT_AHEAD ? f->slot[f->ready].time < f->slot[f->front].time
                                                     : f->slot[f->ready].time > f->slot[f->front].time)))) {
            f->front = f->ready;
            f->ready = -1;
            ff = rf;
        } else if (f->ready >= 0 && !(rf == FIT_AHEAD && f->slot[f->ready].time <= ahead_time)) {
            f->ready = -1;        /* of no use: the worker renders another */
        }
        fit = ff;
        /* On time; or late by less than a seek while a worker brings the next: lyrics
         * drop a frame rather than draw it here (the worker would fall behind again). */
        if (f->front >= 0 &&
            (ff >= FIT_CLOSE || (ff == FIT_BEHIND && (uint64_t)time - (uint64_t)f->slot[f->front].time <= (uint64_t)RA_LEAD_MAX)))
            break;
        if (!held) {
            if (f->front >= 0 && ff != FIT_NONE) break;   /* shown late rather than waiting */
            fa_mutex_unlock(&f->pub_lock);
            return FA_ERROR_BUSY;                          /* the caller takes the lock and comes back */
        }
        /* Draw the frame for `time` here: no worker is rendering (the lock is held). */
        f->ready = -1;
        k = f->front < 0 ? 0 : 1 - f->front;
        fa_mutex_unlock(&f->pub_lock);
        {
            fa_result r = lyric_ra_render(f, k, time, tempo);
            if (r != FA_OK) return r;
        }
        fa_mutex_lock(&f->pub_lock);
        f->front = k;
        fit = FIT_EXACT;
        break;
    }
    sl = &f->slot[f->front];
    partial = fa_history_since(&sl->hist, sl->revision, dst_revision, &copy);
    fr->revision = sl->revision;
    fr->base_revision = partial ? dst_revision : 0;
    fr->time = sl->time;
    fr->render_ns = sl->render_ns;
    fr->behind = fit < FIT_CLOSE;
    fr->background_index = fr->border_index = -1;
    need = f->ready < 0 && sl->time != ahead_time;
    f->ra_time = ahead_time;
    if (!partial) {
        fa_rects_clear(&copy);
        fa_rects_add(&copy, 0, 0, (int)f->ra_w, (int)f->ra_h);
    }
    /* Still under pub_lock, which a setter takes to free the slots; a worker draws
     * only into the other slot. */
    for (i = 0; i < copy.n; i++) {
        const int* r = copy.r[i];
        int y;
        for (y = r[1]; y < r[3]; y++)
            memcpy(dst + (size_t)y * (size_t)dst_stride + (size_t)r[0] * 4, sl->px + ((size_t)y * f->ra_w + (size_t)r[0]) * 4,
                   (size_t)(r[2] - r[0]) * 4);
    }
    fa_mutex_unlock(&f->pub_lock);
    f->ra_last = time;
    if (need) ra_request(f, s);
    fr->dirty_count = (uint32_t)copy.n;
    for (i = 0; i < copy.n; i++) to_rect(copy.r[i], &fr->dirty[i]);
    fa_rects_bounds(&copy, b);
    to_rect(b, &fr->dirty_bounds);
    return FA_OK;
}

/* update_into with work_lock held. */
static fa_result update_held(fa_cdg_surface* f, struct fa_stream* s, fa_time time, uint8_t* dst, int32_t dst_stride,
                             uint32_t dst_width, uint32_t dst_height, uint64_t dst_revision, fa_surface_frame* fr)
{
    cdg_layout want;
    uint8_t pal[16][4];
    fa_rects drawn;
    int64_t drawing;
    int b[4], i, partial;
    fa_result r;
    if (f->render_ahead) {
        r = layout_refresh(f, s);
        if (r != FA_OK) return r;
        if (f->lay.scaled) {
            if (dst_width != f->lay.buf_w || dst_height != f->lay.buf_h) return FA_ERROR_SIZE_MISMATCH;
            if (dst_stride < f->stride) return FA_ERROR_INVALID_ARG;
            ra_show(f, s, time, dst, dst_stride, dst_width, dst_height, dst_revision, 1, fr, &r);
            return r;
        }
    }
    time = sync_state(f, s, time, &want);
    r = layout_apply(f, &want);
    if (r != FA_OK) return r;
    if (dst_width != f->lay.buf_w || dst_height != f->lay.buf_h) return FA_ERROR_SIZE_MISMATCH;
    if (dst_stride < f->stride) return FA_ERROR_INVALID_ARG;
    advance(f, pal);
    drawing = fa_monotonic_ns();
    partial = draw(f, (const uint8_t(*)[4])pal, dst, dst_stride, dst_revision, &drawn);
    fr->revision = f->revision;
    fr->base_revision = partial ? dst_revision : 0;
    fr->dirty_count = (uint32_t)drawn.n;
    for (i = 0; i < drawn.n; i++) to_rect(drawn.r[i], &fr->dirty[i]);
    fa_rects_bounds(&drawn, b);
    to_rect(b, &fr->dirty_bounds);
    fr->time = time;
    fr->render_ns = fa_monotonic_ns() - drawing;
    frame_state(f, (const uint8_t(*)[4])pal, &fr->background_index, &fr->border_index, fr->background, fr->transparency);
    memcpy(fr->palette, pal, sizeof(pal));
    fr->palette_revision = f->palette_revision;
    return FA_OK;
}

FA_API fa_result FA_CALL fa_surface_update_into(fa_surface* surface, fa_time time, void* dst, int32_t dst_stride,
                                                uint32_t dst_width, uint32_t dst_height, uint64_t dst_revision,
                                                fa_surface_frame* out_frame)
{
    struct fa_stream* s;
    fa_surface_frame fr;
    int64_t start = fa_monotonic_ns();
    fa_result r = FA_OK;
    if (!surface || !dst || !out_frame || out_frame->struct_size < sizeof(uint32_t)) return FA_ERROR_INVALID_ARG;
    s = surface_hold(surface);
    if (!s) return FA_ERROR_INVALID_ARG; /* its stream was closed */
    memset(&fr, 0, sizeof(fr));
    if (time == FA_SURFACE_NOW) r = fa_stream_get_clock(s, &time);
    if (r == FA_OK && surface->lyric && surface->render_ahead) {
        if (fa_mutex_trylock(&surface->work_lock)) {
            r = lyric_ra_update(surface, s, time, (uint8_t*)dst, dst_stride, dst_width, dst_height, dst_revision, 1, &fr);
            fa_mutex_unlock(&surface->work_lock);
        } else {
            /* A worker is rendering ahead: what it finished; else wait for it. */
            r = lyric_ra_update(surface, s, time, (uint8_t*)dst, dst_stride, dst_width, dst_height, dst_revision, 0, &fr);
            if (r == FA_ERROR_BUSY) {
                fa_mutex_lock(&surface->work_lock);
                r = lyric_ra_update(surface, s, time, (uint8_t*)dst, dst_stride, dst_width, dst_height, dst_revision, 1, &fr);
                fa_mutex_unlock(&surface->work_lock);
            }
        }
    } else if (r == FA_OK && surface->lyric) {
        float tempo = stream_tempo(s);
        fa_mutex_lock(&surface->work_lock);
        r = fa_lyric_update(surface->lyric, surface->present_w, surface->present_h, (double)tempo, time, (uint8_t*)dst,
                            dst_stride, dst_width, dst_height, dst_revision, &fr);
        fa_store64(&surface->layout_revision, (int64_t)fa_lyric_layout_revision(surface->lyric));
        fa_mutex_unlock(&surface->work_lock);
    } else if (r == FA_OK) {
        if (fa_mutex_trylock(&surface->work_lock)) {
            r = update_held(surface, s, time, (uint8_t*)dst, dst_stride, dst_width, dst_height, dst_revision, &fr);
            fa_mutex_unlock(&surface->work_lock);
        } else {
            /* A worker is rendering ahead, or another thread's setter holds the lock:
             * what was finished, in the layout in use (ra_show checks it). */
            if (ra_show(surface, s, time, (uint8_t*)dst, dst_stride, dst_width, dst_height, dst_revision, 0, &fr, &r) ==
                RA_NEED_LOCK) {
                fa_mutex_lock(&surface->work_lock);
                r = update_held(surface, s, time, (uint8_t*)dst, dst_stride, dst_width, dst_height, dst_revision, &fr);
                fa_mutex_unlock(&surface->work_lock);
            }
        }
    }
    stream_release(s);
    if (r != FA_OK) return r;
    fr.struct_size = out_frame->struct_size < sizeof(fr) ? out_frame->struct_size : (uint32_t)sizeof(fr);
    fr.layout_revision = (uint64_t)fa_load64(&surface->layout_revision);
    fr.draw_ns = fa_monotonic_ns() - start;
    memcpy(out_frame, &fr, fr.struct_size);
    return FA_OK;
}

/* ---- CD+G presentation options ------------------------------------------------------- */

#define CDG_MAX_PADDING 32

FA_API void FA_CALL fa_cdg_presentation_init(fa_cdg_presentation* presentation)
{
    if (!presentation) return;
    memset(presentation, 0, sizeof(*presentation));
    presentation->struct_size = (uint32_t)sizeof(*presentation);
    presentation->safety_zone = FA_SAFETY_SHOW;
}

FA_API fa_result FA_CALL fa_cdg_surface_set_presentation(fa_surface* surface, const fa_cdg_presentation* presentation)
{
    fa_cdg_presentation p;
    if (!surface || surface->lyric) return FA_ERROR_INVALID_ARG;
    if (!presentation) {
        fa_mutex_lock(&surface->work_lock);
        surface->has_pres = 0;
        surface->tb = 0;
        surface->drawn = 0;
        ra_forget(surface);
        fa_mutex_unlock(&surface->work_lock);
        return FA_OK;
    }
    if (presentation->struct_size < sizeof(uint32_t)) return FA_ERROR_INVALID_ARG;
    fa_cdg_presentation_init(&p);
    memcpy(&p, presentation, presentation->struct_size < sizeof(p) ? presentation->struct_size : sizeof(p));
    if ((p.scale != 0 && p.scale != 1) || (uint32_t)p.safety_zone > FA_SAFETY_SUPER ||
        !(p.auto_ignore_seconds >= 0.0f && p.auto_ignore_seconds <= 86400.0f) ||
        (p.super_centered != 0 && p.super_centered != 1) || p.padding > CDG_MAX_PADDING || p.smoothness < 0 ||
        p.smoothness > 10 || (p.edge_blend != 0 && p.edge_blend != 1) || (p.flags & ~(uint32_t)FA_CDG_TRANSPARENT_BORDER))
        return FA_ERROR_INVALID_ARG;
    p.struct_size = (uint32_t)sizeof(p);
    fa_mutex_lock(&surface->work_lock);
    surface->pres = p;
    surface->has_pres = 1;
    surface->tb = (p.flags & FA_CDG_TRANSPARENT_BORDER) != 0;
    surface->drawn = 0;   /* drawn again in full; a new layout too when it changes */
    ra_forget(surface);   /* and shown at once, not after a worker's frame */
    fa_mutex_unlock(&surface->work_lock);
    return FA_OK;
}

FA_API void FA_CALL fa_surface_effects_init(fa_surface_effects* effects)
{
    if (!effects) return;
    memset(effects, 0, sizeof(*effects));
    effects->struct_size = (uint32_t)sizeof(*effects);
    effects->outline.color = effects->outline.sung_color = 0xFF000000u;
    effects->outline.opacity = 1.0f;
    effects->outline.size = 0.5f;
    effects->shadow.color = effects->shadow.sung_color = 0xFF000000u;
    effects->shadow.opacity = 0.6f;
    effects->shadow.distance = 0.5f;
    effects->shadow.angle_degrees = 315.0f;
    effects->bevel.highlight = 0x80FFFFFFu;
    effects->bevel.shade = 0x80000000u;
    effects->bevel.strength = 0.5f;
    effects->bevel.size = 0.5f;
    effects->bevel.light_angle_degrees = 135.0f;
    effects->bevel.fade = 0.5f;
}

FA_API fa_result FA_CALL fa_surface_set_effects(fa_surface* surface, const fa_surface_effects* effects)
{
    fa_surface_effects e;
    if (!surface) return FA_ERROR_INVALID_ARG;
    memset(&e, 0, sizeof(e));
    if (effects) {
        if (effects->struct_size < sizeof(uint32_t)) return FA_ERROR_INVALID_ARG;
        fa_surface_effects_init(&e);
        memcpy(&e, effects, effects->struct_size < sizeof(e) ? effects->struct_size : sizeof(e));
        e.struct_size = (uint32_t)sizeof(e);
        if (!fa_fx_valid(&e)) return FA_ERROR_INVALID_ARG;
    }
    fa_mutex_lock(&surface->work_lock);
    if (surface->lyric) fa_lyric_set_effects(surface->lyric, effects ? &e : NULL);
    surface->has_fx = effects != NULL;
    if (effects) surface->fx = e;
    surface->drawn = 0;   /* drawn again in full */
    ra_forget(surface);
    fa_mutex_unlock(&surface->work_lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_surface_get_effects(fa_surface* surface, fa_surface_effects* out_effects)
{
    fa_surface_effects e;
    uint32_t size;
    if (!surface || !out_effects || out_effects->struct_size < sizeof(uint32_t)) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&surface->work_lock);
    if (surface->lyric) {
        fa_lyric_style st;
        fa_lyric_get_style(surface->lyric, &st);
        e = st.effects;
    } else if (surface->has_fx) {
        e = surface->fx;
    } else {
        fa_surface_effects_init(&e);
    }
    fa_mutex_unlock(&surface->work_lock);
    size = out_effects->struct_size < sizeof(e) ? out_effects->struct_size : (uint32_t)sizeof(e);
    e.struct_size = size;
    memcpy(out_effects, &e, size);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_cdg_surface_get_presentation(fa_surface* surface, fa_cdg_presentation* out_presentation)
{
    fa_cdg_presentation p;
    uint32_t size;
    if (!surface || surface->lyric || !out_presentation || out_presentation->struct_size < sizeof(uint32_t))
        return FA_ERROR_INVALID_ARG;
    if (surface->has_pres) {
        p = surface->pres;
    } else {
        fa_cdg_presentation_init(&p);
        if (surface->flags & FA_CDG_NO_BORDER) p.safety_zone = FA_SAFETY_REMOVE;
    }
    size = out_presentation->struct_size < sizeof(p) ? out_presentation->struct_size : (uint32_t)sizeof(p);
    p.struct_size = size;
    memcpy(out_presentation, &p, size);
    return FA_OK;
}

/* ---- MIDI lyric surface ------------------------------------------------------------ */

/* The app's style over Classic's (a shorter, older struct keeps Classic's tail). */
static fa_result lyric_style_read(const fa_lyric_style* style, fa_lyric_style* out)
{
    fa_lyric_style_init(out, FA_LYRIC_CLASSIC);
    if (!style) return FA_OK;
    if (style->struct_size < sizeof(uint32_t)) return FA_ERROR_INVALID_ARG;
    memcpy(out, style, style->struct_size < sizeof(*out) ? style->struct_size : sizeof(*out));
    out->struct_size = (uint32_t)sizeof(*out);
    out->effects.struct_size = (uint32_t)sizeof(out->effects);
    return fa_lyric_style_valid(out) ? FA_OK : FA_ERROR_INVALID_ARG;
}

FA_API fa_result FA_CALL fa_lyric_surface_create(fa_stream* stream, const fa_lyric_style* style, fa_surface** out_surface)
{
    fa_lyric_style st;
    fa_midi_info mi;
    fa_cdg_surface* f;
    fa_result r;
    if (out_surface) *out_surface = NULL;
    if (!stream || !out_surface || fa_stream_get_midi_info(stream, &mi) != FA_OK) return FA_ERROR_INVALID_ARG;
    r = lyric_style_read(style, &st);
    if (r != FA_OK) return r;
    f = (fa_cdg_surface*)calloc(1, sizeof(*f));
    if (!f) return FA_ERROR_OUT_OF_MEMORY;
    if (fa_mutex_init(&f->work_lock) != 0) { free(f); return FA_ERROR_OUT_OF_MEMORY; }
    if (fa_mutex_init(&f->pub_lock) != 0) {
        fa_mutex_destroy(&f->work_lock);
        free(f);
        return FA_ERROR_OUT_OF_MEMORY;
    }
    f->front = f->ready = -1;
    f->stream = stream;
    f->stale_from = SIZE_MAX;
    r = fa_lyric_new(stream, &st, &f->lyric);
    if (r != FA_OK) { surface_free(f); return r; }
    fa_mutex_lock(&stream->cdg_lock);
    f->next = stream->cdg_surfaces;
    stream->cdg_surfaces = f;
    fa_mutex_unlock(&stream->cdg_lock);
    *out_surface = f;
    return FA_OK;
}

FA_API fa_result FA_CALL fa_lyric_surface_set_style(fa_surface* surface, const fa_lyric_style* style)
{
    fa_lyric_style st;
    fa_result r;
    if (!surface || !surface->lyric || !style) return FA_ERROR_INVALID_ARG;
    r = lyric_style_read(style, &st);
    if (r != FA_OK) return r;
    fa_mutex_lock(&surface->work_lock);
    r = fa_lyric_set_style(surface->lyric, &st);
    if (r == FA_OK) ra_forget(surface);   /* the slots show the old style */
    fa_mutex_unlock(&surface->work_lock);
    return r;
}

FA_API fa_result FA_CALL fa_lyric_surface_get_style(fa_surface* surface, fa_lyric_style* out_style)
{
    fa_lyric_style st;
    uint32_t size;
    if (!surface || !surface->lyric || !out_style || out_style->struct_size < sizeof(uint32_t)) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&surface->work_lock);
    fa_lyric_get_style(surface->lyric, &st);
    fa_mutex_unlock(&surface->work_lock);
    size = out_style->struct_size < sizeof(st) ? out_style->struct_size : (uint32_t)sizeof(st);
    st.struct_size = size;
    memcpy(out_style, &st, size);
    return FA_OK;
}
