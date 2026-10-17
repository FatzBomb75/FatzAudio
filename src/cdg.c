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
 * pack for the time it is asked for. Moving forward applies the new packs; moving
 * back restores the last checkpoint before the target (one every 30 s of graphics,
 * taken on the way forward) and replays from there, so a seek costs at most 30 s of
 * packs, a fraction of a millisecond. Everything runs on the caller's thread under
 * the stream's cdg_lock; the audio thread never sees any of it. */
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
#define CDG_CHECKPOINT_PACKS (30 * FA_CDG_PACKS_PER_SECOND)

typedef struct cdg_state {
    uint8_t screen[FA_CDG_HEIGHT][FA_CDG_WIDTH];  /* colour indices */
    uint8_t rgb[16][3];
    uint8_t transparency[16];     /* 0 = opaque .. 63 = invisible (transparent colour packs) */
    uint8_t bg;                   /* colour of the last memory preset */
    uint8_t hoff, voff;           /* smooth-scroll offsets, 0..5 and 0..11 */
    int clean;                    /* nothing drawn since the last memory preset */
    size_t next;                  /* packs applied so far */
    int dx0, dy0, dx1, dy1;       /* screen area changed since the last render; empty if dx0 >= dx1 */
} cdg_state;

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
    uint8_t shown_hoff, shown_voff;
    uint16_t* xmap;               /* output column -> window column (sharp scaling) */
    uint16_t* ymap;               /* output row -> window row */
    cdg_state st;
    cdg_state* checkpoints;       /* [k] = state after (k + 1) * CDG_CHECKPOINT_PACKS packs */
    size_t checkpoint_count;
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
    d->packs = bytes;
    d->count = size / FA_CDG_PACK_BYTES;
    *out = d;
    return FA_OK;
}

void fa_cdg_data_free(fa_cdg_data* data)
{
    if (!data) return;
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
    st->clean = 1;
    mark_all(st);
}

static void memory_preset(cdg_state* st, const uint8_t* d)
{
    uint8_t color = d[0] & 0x0F;
    /* Discs repeat the preset (repeat count in d[1]) in case one is damaged; a file
     * has no damage, so a repeat of a screen that is still clean changes nothing. */
    if ((d[1] & 0x0F) != 0 && st->clean && st->bg == color) return;
    memset(st->screen, color, sizeof(st->screen));
    st->bg = color;
    st->clean = 1;
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

static void palette_rgba(const fa_cdg_surface* f, uint8_t out[16][4])
{
    int i;
    for (i = 0; i < 16; i++) {
        uint8_t a = 255;
        if (f->flags & FA_CDG_TRANSPARENT)
            a = i == f->st.bg ? 0 : (uint8_t)(255 - (f->st.transparency[i] * 255 + 31) / 63);
        out[i][0] = f->st.rgb[i][0];
        out[i][1] = f->st.rgb[i][1];
        out[i][2] = f->st.rgb[i][2];
        out[i][3] = a;
    }
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

static void render_rect(fa_cdg_surface* f, const uint8_t pal[16][4], uint32_t ox0, uint32_t oy0, uint32_t ox1, uint32_t oy1)
{
    uint32_t pm[16][4];   /* premultiplied palette */
    uint32_t ox, oy, i;
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
            /* Bilinear on premultiplied colour, in 8-bit fixed point. Pixel centres
             * map to pixel centres. */
            int64_t fy = ((int64_t)(2 * oy + 1) * f->win_h * 256) / (2 * f->height) - 128;
            int32_t y0, y1, ty;
            if (fy < 0) fy = 0;
            y0 = (int32_t)(fy >> 8);
            ty = (int32_t)(fy & 255);
            if ((uint32_t)y0 >= f->win_h - 1) { y0 = (int32_t)f->win_h - 1; ty = 0; }
            y1 = (uint32_t)y0 + 1 < f->win_h ? y0 + 1 : y0;
            for (ox = ox0; ox < ox1; ox++) {
                int64_t fx = ((int64_t)(2 * ox + 1) * f->win_w * 256) / (2 * f->width) - 128;
                int32_t x0, x1, tx, k;
                uint32_t acc[4];
                const uint32_t *a, *b, *c, *d;
                if (fx < 0) fx = 0;
                x0 = (int32_t)(fx >> 8);
                tx = (int32_t)(fx & 255);
                if ((uint32_t)x0 >= f->win_w - 1) { x0 = (int32_t)f->win_w - 1; tx = 0; }
                x1 = (uint32_t)x0 + 1 < f->win_w ? x0 + 1 : x0;
                a = pm[window_index(f, (uint32_t)x0, (uint32_t)y0)];
                b = pm[window_index(f, (uint32_t)x1, (uint32_t)y0)];
                c = pm[window_index(f, (uint32_t)x0, (uint32_t)y1)];
                d = pm[window_index(f, (uint32_t)x1, (uint32_t)y1)];
                for (k = 0; k < 4; k++) {
                    uint32_t top = a[k] * (uint32_t)(256 - tx) + b[k] * (uint32_t)tx;
                    uint32_t bottom = c[k] * (uint32_t)(256 - tx) + d[k] * (uint32_t)tx;
                    acc[k] = (top * (uint32_t)(256 - ty) + bottom * (uint32_t)ty + 32768) >> 16;
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
        if (n > data->count - first) n = data->count - first;
        memcpy(data->packs + first * FA_CDG_PACK_BYTES, packs, n * FA_CDG_PACK_BYTES);
        /* Normally the packs arrive well ahead of what is shown; after a seek they
         * may land behind a surface, which goes back to replay them. */
        for (f = s->cdg_surfaces; f; f = f->next)
            if (f->st.next > first && first < f->stale_from) f->stale_from = first;
    }
    fa_mutex_unlock(&s->cdg_lock);
    return ok;
}

void fa_stream_cdg_close(struct fa_stream* s)
{
    fa_cdg_surface* f;
    fa_mutex_lock(&s->cdg_lock);
    /* Surfaces belong to the app: they outlive the stream until destroyed, and
     * only refuse to update from now on. */
    for (f = s->cdg_surfaces; f; f = f->next) f->stream = NULL;
    s->cdg_surfaces = NULL;
    fa_cdg_data_free(s->cdg);
    s->cdg = NULL;
    fa_mutex_unlock(&s->cdg_lock);
}

static void surface_free(fa_cdg_surface* f)
{
    free(f->checkpoints);
    free(f->xmap);
    free(f->ymap);
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

FA_API fa_result FA_CALL fa_cdg_surface_create(fa_stream* stream, const fa_cdg_surface_config* config, fa_cdg_surface** out_surface)
{
    fa_cdg_surface_config c;
    fa_cdg_surface* f;
    uint32_t bpp, i;
    if (out_surface) *out_surface = NULL;
    if (!stream || !out_surface) return FA_ERROR_INVALID_ARG;
    fa_cdg_surface_config_init(&c);
    if (config) {
        if (config->struct_size < sizeof(c)) return FA_ERROR_INVALID_ARG;
        c = *config;
    }
    if ((uint32_t)c.pixel_format > FA_CDG_PIXELS_INDEXED8 || c.width > CDG_MAX_OUTPUT || c.height > CDG_MAX_OUTPUT ||
        (c.flags & ~(uint32_t)(FA_CDG_NO_BORDER | FA_CDG_TRANSPARENT | FA_CDG_PREMULTIPLIED | FA_CDG_SMOOTH)))
        return FA_ERROR_INVALID_ARG;
    f = (fa_cdg_surface*)calloc(1, sizeof(*f));
    if (!f) return FA_ERROR_OUT_OF_MEMORY;
    f->stream = stream;
    f->format = c.pixel_format;
    f->flags = c.flags;
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
    state_reset(&f->st);
    f->stale_from = SIZE_MAX;
    fa_mutex_lock(&stream->cdg_lock);
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
    s = surface->stream;
    if (s) {
        fa_mutex_lock(&s->cdg_lock);
        for (link = &s->cdg_surfaces; *link; link = &(*link)->next)
            if (*link == surface) { *link = surface->next; break; }
        fa_mutex_unlock(&s->cdg_lock);
    }
    surface_free(surface);
}

/* Packs shown at `time` (ns): pack n arrives during [n/300 s, (n+1)/300 s), so all
 * packs that started before `time` are applied. */
static size_t packs_at(fa_time time, size_t count)
{
    int64_t n;
    if (time <= 0) return 0;
    if (time > (INT64_MAX - 999999999) / FA_CDG_PACKS_PER_SECOND) return count;
    n = (time * FA_CDG_PACKS_PER_SECOND + 999999999) / 1000000000;
    return (uint64_t)n < (uint64_t)count ? (size_t)n : count;
}

/* Moving back: the newest checkpoint at or before `target`, else the start. */
static void rewind_to(fa_cdg_surface* f, size_t target)
{
    size_t k = target / CDG_CHECKPOINT_PACKS;
    if (k > f->checkpoint_count) k = f->checkpoint_count;
    if (k == 0) { state_reset(&f->st); return; }
    f->st = f->checkpoints[k - 1];
    mark_all(&f->st);
}

static void save_checkpoint(fa_cdg_surface* f)
{
    cdg_state* grown;
    if (f->st.next / CDG_CHECKPOINT_PACKS != f->checkpoint_count + 1) return; /* already have it */
    grown = (cdg_state*)realloc(f->checkpoints, (f->checkpoint_count + 1) * sizeof(*grown));
    if (!grown) return; /* without it, seeking back just replays more */
    f->checkpoints = grown;
    f->checkpoints[f->checkpoint_count++] = f->st;
}

static void drop_checkpoints(fa_cdg_surface* f)
{
    free(f->checkpoints);
    f->checkpoints = NULL;
    f->checkpoint_count = 0;
}

static fa_result surface_update(fa_cdg_surface* f, fa_time time, fa_cdg_frame* out)
{
    struct fa_stream* s = f->stream;
    size_t target;
    fa_time offset;
    if (!s) return FA_ERROR_INVALID_ARG; /* its stream was closed */
    fa_mutex_lock(&s->cdg_lock);
    offset = fa_load64(&s->cdg_offset);
    /* time - offset, saturated */
    if (offset > 0 && time < INT64_MIN + offset) time = INT64_MIN;
    else if (offset < 0 && time > INT64_MAX + offset) time = INT64_MAX;
    else time -= offset;
    if (f->gen != s->cdg_gen) { state_reset(&f->st); drop_checkpoints(f); f->gen = s->cdg_gen; f->stale_from = SIZE_MAX; }
    if (f->stale_from != SIZE_MAX) {
        /* Packs from stale_from changed: only checkpoints before them still hold. */
        size_t keep = f->stale_from / CDG_CHECKPOINT_PACKS;
        if (keep < f->checkpoint_count) f->checkpoint_count = keep;
        if (f->st.next > f->stale_from) rewind_to(f, f->stale_from);
        f->stale_from = SIZE_MAX;
    }
    target = s->cdg ? packs_at(time, s->cdg->count) : 0;
    if (target < f->st.next) rewind_to(f, target);
    while (f->st.next < target) {
        apply_pack(&f->st, s->cdg->packs + f->st.next * FA_CDG_PACK_BYTES);
        f->st.next++;
        if (f->st.next % CDG_CHECKPOINT_PACKS == 0) save_checkpoint(f);
    }
    fa_mutex_unlock(&s->cdg_lock);
    /* Drawing uses only this surface's state: other surfaces and loads need not wait. */
    render(f, out);
    out->time = time;
    return FA_OK;
}

FA_API fa_result FA_CALL fa_cdg_surface_update(fa_cdg_surface* surface, fa_cdg_frame* out_frame)
{
    fa_time now;
    fa_result r;
    if (!surface || !out_frame || !surface->stream) return FA_ERROR_INVALID_ARG;
    r = fa_stream_get_clock(surface->stream, &now);
    if (r != FA_OK) return r;
    return surface_update(surface, now, out_frame);
}

FA_API fa_result FA_CALL fa_cdg_surface_update_at(fa_cdg_surface* surface, fa_time time, fa_cdg_frame* out_frame)
{
    if (!surface || !out_frame) return FA_ERROR_INVALID_ARG;
    return surface_update(surface, time, out_frame);
}
