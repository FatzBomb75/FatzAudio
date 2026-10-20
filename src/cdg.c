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
    int dx0, dy0, dx1, dy1;       /* screen area changed since the last render; empty if dx0 >= dx1 */
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
};

struct fa_cdg_surface {
    struct fa_stream* stream;
    struct fa_cdg_surface* next;  /* stream->cdg_surfaces, under cdg_lock */
    fa_cdg_pixel_format format;
    uint32_t flags;
    uint32_t width, height;       /* output */
    uint32_t win_x, win_y, win_w, win_h; /* the part of the screen shown */
    uint8_t* pixels;
    int32_t stride;
    uint32_t gen;                 /* stream->cdg_gen the state belongs to */
    int drawn;                    /* the pixels hold a whole picture */
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

static void mark(cdg_state* st, int x0, int y0, int x1, int y1)
{
    if (st->dx0 >= st->dx1) { st->dx0 = x0; st->dy0 = y0; st->dx1 = x1; st->dy1 = y1; return; }
    if (x0 < st->dx0) st->dx0 = x0;
    if (y0 < st->dy0) st->dy0 = y0;
    if (x1 > st->dx1) st->dx1 = x1;
    if (y1 > st->dy1) st->dy1 = y1;
}

static void mark_all(cdg_state* st) { mark(st, 0, 0, FA_CDG_WIDTH, FA_CDG_HEIGHT); }

static void state_reset(cdg_state* st)
{
    memset(st, 0, sizeof(*st));
    st->border = -1;
    st->clean = 1;
    mark_all(st);
}

static void memory_preset(cdg_state* st, const uint8_t* d)
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
        return;
    }
    memset(st->screen, color, sizeof(st->screen));
    st->bg = color;
    st->preset = 1;
    st->clean = 1;
    st->repeat = repeat;
    st->preset_at = st->next;
    mark_all(st);
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
    if (hcmd == 1 || hcmd == 2 || vcmd == 1 || vcmd == 2) { st->clean = 0; mark_all(st); }
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

static void apply_pack(cdg_state* st, const uint8_t* pack)
{
    const uint8_t* d = pack + 4; /* command, instruction, 2 parity bytes, 16 data bytes */
    int i;
    if ((pack[0] & 0x3F) != CDG_MODE_GRAPHICS) return;
    switch (pack[1] & 0x3F) {
    case CDG_MEMORY_PRESET: memory_preset(st, d); break;
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
    default: break;
    }
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
    st->dx0 = st->dy0 = st->dx1 = st->dy1 = 0;
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

/* Runs the index's decoder over packs [build.next, upto), saving each checkpoint. */
static void index_advance(struct fa_cdg_index* ix, const fa_cdg_data* d, size_t upto)
{
    cdg_state* st = &ix->build;
    while (st->next < upto) {
        apply_pack(st, d->packs + st->next * FA_CDG_PACK_BYTES);
        st->next++;
        if (st->next % ix->step == 0 && st->next / ix->step == ix->count + 1 && ix->count < ix->cap) {
            st->dx0 = st->dy0 = st->dx1 = st->dy1 = 0;
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

/* Colour index of window pixel (x, y): the picture area shows the screen shifted by
 * the smooth-scroll offsets, the border does not move. */
static uint8_t window_index(const fa_cdg_surface* f, uint32_t x, uint32_t y)
{
    uint32_t sx = f->win_x + x, sy = f->win_y + y;
    if (sx >= CDG_BORDER_X && sx < FA_CDG_WIDTH - CDG_BORDER_X && sy >= CDG_BORDER_Y && sy < FA_CDG_HEIGHT - CDG_BORDER_Y) {
        sx += f->st.hoff;
        sy += f->st.voff;
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

static void render_rect(fa_cdg_surface* f, const uint8_t pal[16][4], uint32_t ox0, uint32_t oy0, uint32_t ox1, uint32_t oy1)
{
    uint32_t pm[16][4];   /* premultiplied palette */
    uint32_t ox, oy, i, cy0 = UINT32_MAX, cy1 = UINT32_MAX;
    uint8_t flat[16][4], r0[FA_CDG_WIDTH], r1[FA_CDG_WIDTH];   /* smooth: output pixels, window rows cy0, cy1 */
    int have_flat = 0;
    if (ox0 >= ox1 || oy0 >= oy1) return;
    for (i = 0; i < 16; i++) {
        pm[i][3] = pal[i][3];
        pm[i][0] = (pal[i][0] * pm[i][3] + 127) / 255;
        pm[i][1] = (pal[i][1] * pm[i][3] + 127) / 255;
        pm[i][2] = (pal[i][2] * pm[i][3] + 127) / 255;
    }
    for (oy = oy0; oy < oy1; oy++) {
        uint8_t* row = f->pixels + (size_t)oy * (size_t)f->stride;
        if (f->format == FA_CDG_PIXELS_INDEXED8) {
            for (ox = ox0; ox < ox1; ox++) row[ox] = window_index(f, f->xmap[ox], f->ymap[oy]);
        } else if (!(f->flags & FA_CDG_SMOOTH) || (f->width == f->win_w && f->height == f->win_h)) {
            /* Sharp pixels: straight from the palette in the output's byte order. */
            uint8_t lut[16][4];
            uint32_t wy = f->ymap[oy];
            int premul = (f->flags & FA_CDG_PREMULTIPLIED) != 0;
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
                for (i = 0; i < 16; i++) store_pixel(f, flat[i], pm[i][0], pm[i][1], pm[i][2], pm[i][3]);
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

/* Draws what changed since the last render; fills the frame's dirty rectangle. */
static void render(fa_cdg_surface* f, fa_cdg_frame* out)
{
    cdg_state* st = &f->st;
    uint8_t pal[16][4];
    uint32_t ox0 = 0, oy0 = 0, ox1 = 0, oy1 = 0;
    int palette_changed, full;
    palette_rgba(f, pal);
    palette_changed = !f->drawn || memcmp(pal, f->shown_palette, sizeof(pal)) != 0;
    /* Indexed pixels do not depend on the palette; colour pixels do. Smooth-scroll
     * offsets move the whole picture area. */
    full = !f->drawn || st->hoff != f->shown_hoff || st->voff != f->shown_voff ||
           (palette_changed && f->format != FA_CDG_PIXELS_INDEXED8);
    if (!full && st->dx0 < st->dx1) {
        int64_t x0, y0, x1, y1;
        if (st->hoff || st->voff) {
            full = 1; /* rare (only while a scroll runs): redraw all rather than map the shift */
        } else {
            int pad = (f->flags & FA_CDG_SMOOTH) && f->format != FA_CDG_PIXELS_INDEXED8 ? 1 : 0;
            x0 = (int64_t)st->dx0 - f->win_x - pad;
            y0 = (int64_t)st->dy0 - f->win_y - pad;
            x1 = (int64_t)st->dx1 - f->win_x + pad;
            y1 = (int64_t)st->dy1 - f->win_y + pad;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > (int64_t)f->win_w) x1 = f->win_w;
            if (y1 > (int64_t)f->win_h) y1 = f->win_h;
            if (x0 < x1 && y0 < y1) {
                /* Output pixels whose source lies in the window rectangle (a superset). */
                ox0 = (uint32_t)(x0 * f->width / f->win_w);
                oy0 = (uint32_t)(y0 * f->height / f->win_h);
                ox1 = (uint32_t)((x1 * f->width + f->win_w - 1) / f->win_w);
                oy1 = (uint32_t)((y1 * f->height + f->win_h - 1) / f->win_h);
                if (ox1 > f->width) ox1 = f->width;
                if (oy1 > f->height) oy1 = f->height;
            }
        }
    }
    if (full) { ox0 = oy0 = 0; ox1 = f->width; oy1 = f->height; }
    if (ox0 < ox1 && oy0 < oy1) render_rect(f, pal, ox0, oy0, ox1, oy1);
    f->drawn = 1;
    f->shown_hoff = st->hoff;
    f->shown_voff = st->voff;
    memcpy(f->shown_palette, pal, sizeof(pal));
    st->dx0 = st->dy0 = st->dx1 = st->dy1 = 0;

    out->pixels = f->pixels;
    out->width = f->width;
    out->height = f->height;
    out->stride = f->stride;
    out->pixel_format = f->format;
    out->palette_changed = palette_changed;
    out->changed = (ox0 < ox1 && oy0 < oy1) || palette_changed;
    if (ox0 < ox1 && oy0 < oy1) {
        out->dirty_x = ox0; out->dirty_y = oy0; out->dirty_width = ox1 - ox0; out->dirty_height = oy1 - oy0;
    } else {
        out->dirty_x = out->dirty_y = out->dirty_width = out->dirty_height = 0;
    }
    memcpy(out->palette, pal, sizeof(pal));

    out->background_index = st->preset ? st->bg : -1;
    out->border_index = st->border;
    memcpy(out->transparency, st->transparency, sizeof(out->transparency));
    if (st->preset) memcpy(out->background, pal[st->bg], 4);
    else memset(out->background, 0, 4);
    if (out->dirty_width) f->revision++;
    if (palette_changed || out->background_index != f->shown_bg || out->border_index != f->shown_border ||
        memcmp(st->transparency, f->shown_transparency, sizeof(st->transparency)) != 0)
        f->palette_revision++;
    f->shown_bg = out->background_index;
    f->shown_border = out->border_index;
    memcpy(f->shown_transparency, st->transparency, sizeof(st->transparency));
    out->revision = f->revision;
    out->palette_revision = f->palette_revision;
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

void fa_stream_cdg_detach(struct fa_stream* s)
{
    fa_cdg_surface* f;
    fa_lazy_mutex_lock(&g_bind);
    fa_mutex_lock(&s->cdg_lock);
    /* Surfaces belong to the app: they outlive the stream until destroyed, and
     * only refuse to update from now on. */
    for (f = s->cdg_surfaces; f; f = f->next) f->stream = NULL;
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
    free(f->xmap);
    free(f->ymap);
    free(f->xlerp);
    free(f->ylerp);
    free(f->pixels);
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
    uint32_t bpp, i;
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
    f->stream = stream;
    f->format = c.pixel_format;
    f->flags = c.flags;
    f->bg_color = c.background_color;
    f->bg_opacity = c.background_opacity;
    if (c.flags & FA_CDG_NO_BORDER) {
        f->win_x = CDG_BORDER_X; f->win_y = CDG_BORDER_Y;
        f->win_w = FA_CDG_PICTURE_WIDTH; f->win_h = FA_CDG_PICTURE_HEIGHT;
    } else {
        f->win_w = FA_CDG_WIDTH; f->win_h = FA_CDG_HEIGHT;
    }
    f->width = c.width ? c.width : f->win_w;
    f->height = c.height ? c.height : f->win_h;
    bpp = f->format == FA_CDG_PIXELS_INDEXED8 ? 1 : 4;
    f->stride = (int32_t)(f->width * bpp);
    f->pixels = (uint8_t*)calloc((size_t)f->height, (size_t)f->stride);
    f->xmap = (uint16_t*)malloc(f->width * sizeof(uint16_t));
    f->ymap = (uint16_t*)malloc(f->height * sizeof(uint16_t));
    if (!f->pixels || !f->xmap || !f->ymap) { surface_free(f); return FA_ERROR_OUT_OF_MEMORY; }
    for (i = 0; i < f->width; i++) f->xmap[i] = (uint16_t)((uint64_t)i * f->win_w / f->width);
    for (i = 0; i < f->height; i++) f->ymap[i] = (uint16_t)((uint64_t)i * f->win_h / f->height);
    if (f->flags & FA_CDG_SMOOTH) {
        f->xlerp = (uint32_t*)malloc(f->width * sizeof(uint32_t));
        f->ylerp = (uint32_t*)malloc(f->height * sizeof(uint32_t));
        if (!f->xlerp || !f->ylerp) { surface_free(f); return FA_ERROR_OUT_OF_MEMORY; }
        for (i = 0; i < f->width; i++) f->xlerp[i] = lerp_map(i, f->width, f->win_w);
        for (i = 0; i < f->height; i++) f->ylerp[i] = lerp_map(i, f->height, f->win_h);
    }
    state_reset(&f->st);
    f->stale_from = SIZE_MAX;
    fa_mutex_lock(&stream->cdg_lock);
    index_build(stream->cdg);
    f->gen = stream->cdg_gen;
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
    int x0 = st->dx0, y0 = st->dy0, x1 = st->dx1, y1 = st->dy1, y;
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
    st->dx0 = st->dy0 = st->dx1 = st->dy1 = 0;
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

/* `s` is the surface's stream, held by the caller. */
static fa_result surface_update(fa_cdg_surface* f, struct fa_stream* s, fa_time time, fa_cdg_frame* out)
{
    size_t target;
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
        f->stale_from = SIZE_MAX;
    }
    target = s->cdg ? packs_at(time, s->cdg->count) : 0;
    decode_to(f, s->cdg, target);
    fa_mutex_unlock(&s->cdg_lock);
    /* Drawing uses only this surface's state: other surfaces and loads need not wait. */
    render(f, out);
    out->time = time;
    return FA_OK;
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
    if (!surface || !out_frame) return FA_ERROR_INVALID_ARG;
    s = surface_hold(surface);
    if (!s) return FA_ERROR_INVALID_ARG; /* its stream was closed */
    r = fa_stream_get_clock(s, &now);
    if (r == FA_OK) r = surface_update(surface, s, now, out_frame);
    stream_release(s);
    return r;
}

FA_API fa_result FA_CALL fa_cdg_surface_update_at(fa_cdg_surface* surface, fa_time time, fa_cdg_frame* out_frame)
{
    struct fa_stream* s;
    fa_result r;
    if (!surface || !out_frame) return FA_ERROR_INVALID_ARG;
    s = surface_hold(surface);
    if (!s) return FA_ERROR_INVALID_ARG; /* its stream was closed */
    r = surface_update(surface, s, time, out_frame);
    stream_release(s);
    return r;
}
