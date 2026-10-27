/* The MIDI lyric surface (lyric.h): lines laid out once with our font reader and
 * kept as masks (the effects' weights and the text's coverage, lyric_time.h says
 * where each line is), coloured and placed for each frame into a bitmap of the
 * surface's own, and copied into the app's buffer by changed area.
 *
 * A line is drawn at its full size (scale 1) with its effects into masks: shadow,
 * outline, coverage and bevel weights, and a panel's coverage. Colouring happens
 * when it is placed, so the wipe can turn any column from the unsung to the sung
 * colours (and an edge can sweep across a syllable) without drawing the text again.
 * Lines shown smaller (the lines around the active one), growing or shrinking (page
 * fades) or between pixels (scrolls) are resampled from the masks with a tent
 * filter as wide as the step, which at smaller sizes is an area average. A frame
 * redraws only what changed from the last: the columns a wipe changed (for a sweep,
 * only those its edge crossed), a countdown's arrows, or the lines that moved or
 * faded. */
#include "lyric.h"
#include "lyric_time.h"
#include "fa_platform.h"
#include "fa_util.h"
#include "font.h"
#include "font_find.h"
#include "raster.h"
#include "surface.h"
#include "surface_fx.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#  include <emmintrin.h>
#  define LY_SSE 1
#else
#  define LY_SSE 0
#endif

#define LY_DESIGN_W 1920u
#define LY_DESIGN_H 1080u
/* Line layers kept: every line a frame shows, and two more so a scroll or a page turn
 * does not draw again the line it just let go of. */
#define LY_CACHE (LY_MAX_SHOW + 2)
#define LY_MS 1000000LL
/* The intro's top and bottom bands, its card, the ending card. */
#define LY_PAGES 4
enum { PG_TOP = 0, PG_BOTTOM, PG_CARD, PG_END };
#define LY_MAX_INST (LY_MAX_SHOW + LY_PAGES)
/* ly_inst.line of a page. */
#define LY_PAGE_LINE 0x80000000u

/* ---- the style ------------------------------------------------------------------ */

FA_API void FA_CALL fa_lyric_style_init(fa_lyric_style* st, fa_lyric_preset preset)
{
    if (!st) return;
    memset(st, 0, sizeof(*st));
    st->struct_size = (uint32_t)sizeof(*st);
    st->font_size = 72.0f;
    st->font_weight = 700;
    st->text_color = 0xFFFFFFFFu;
    st->sung_color = 0xFFFFFF00u;
    st->vertical_position = 0.5f;
    st->box_width = 0.85f;
    st->line_pitch = 1.55f;
    st->lines_above = 2;
    st->lines_below = 2;
    st->above_opacity[0] = 0.5f;
    st->above_opacity[1] = 0.25f;
    st->next_line_opacity = 0.65f;
    st->after_next_factor = 0.55f;
    st->near_scale = 0.8f;
    st->far_scale = 0.6f;
    st->wipe = FA_WIPE_CROSSFADE;
    st->wipe_cap_ms = 600;
    st->wipe_share = 0.8f;
    st->instant_cut_ms = 150;
    st->lead_ms = 400;
    st->paragraph_gap_ms = 2000;
    st->sentence_gap_ms = 900;
    st->break_length = 42;
    st->scroll_ms = 1500;
    st->page_ms = 1000;
    st->page_hold_ms = 2000;
    st->page_preview_ms = 1000;
    st->page_overlap = 1;
    st->page_in_scale = 1.5f;
    st->page_out_scale = 0.75f;
    st->end_hold_ms = 5000;
    st->end_fade_ms = 1000;
    st->countdown = FA_COUNTDOWN_CLOCK;
    st->countdown_count = 4;
    st->countdown_ms = 5000;
    st->countdown_gap_ms = 6000;
    st->countdown_from = 0xFFFF0000u;
    st->countdown_to = 0xFF00C000u;
    st->countdown_size = 0.5f;
    st->panel_color = 0x80000000u;
    st->panel_padding = 0.25f;
    st->panel_radius = 0.2f;
    fa_surface_effects_init(&st->effects);
    /* Classic: MediaControl's look, a 1.75 px outline and a (2, 2) px shadow at 72 px. */
    st->effects.outline.on = 1;
    st->effects.outline.color = st->effects.outline.sung_color = 0xE0000000u;
    st->effects.outline.opacity = 1.0f;
    st->effects.outline.size = 0.2f;
    st->effects.outline.fade = 0.0f;
    st->effects.shadow.on = 1;
    st->effects.shadow.color = st->effects.shadow.sung_color = 0xA0000000u;
    st->effects.shadow.opacity = 1.0f;
    st->effects.shadow.size = 0.0f;
    st->effects.shadow.distance = 0.26f;
    st->effects.shadow.angle_degrees = 315.0f;
    st->intro_page = 1;
    st->ending_page = 1;
    st->intro_start_ms = 1000;
    st->intro_fade_ms = 1000;
    st->intro_clear_ms = 3000;
    st->intro_band_ms = 8000;
    st->ending_min_ms = 3000;
    st->ending_fade_ms = 750;
    st->card_color = 0xE00E131Cu;
    st->card_accent = 0xFF78DCE8u;
    st->title_color = 0xFFFFF4DCu;
    st->info_color = 0xFFD5DCE6u;
    st->copyright_color = 0xFFB8A87Au;
    switch (preset) {
    case FA_LYRIC_MODERN:
        st->wipe = FA_WIPE_SWEEP;
        st->sung_color = 0xFF4FD8FFu;
        st->effects.outline.size = 0.12f;
        st->effects.outline.color = st->effects.outline.sung_color = 0xC0000000u;
        st->effects.shadow.size = 0.5f;
        st->effects.shadow.fade = 1.0f;
        st->effects.shadow.distance = 0.3f;
        st->effects.shadow.color = st->effects.shadow.sung_color = 0x90000000u;
        break;
    case FA_LYRIC_NEON:
        st->wipe = FA_WIPE_SWEEP;
        st->text_color = 0xFFFFE6FAu;
        st->sung_color = 0xFFE6FFFFu;
        st->effects.outline.color = 0xFFFF2BD6u;
        st->effects.outline.sung_color = 0xFF2BF0FFu;
        st->effects.outline.size = 0.7f;
        st->effects.outline.fade = 0.85f;
        st->effects.shadow.on = 0;
        break;
    case FA_LYRIC_MINIMAL:
        st->lines_above = 1;
        st->lines_below = 1;
        st->countdown = FA_COUNTDOWN_OFF;
        st->effects.outline.on = 0;
        st->effects.shadow.size = 0.4f;
        st->effects.shadow.fade = 1.0f;
        st->effects.shadow.distance = 0.15f;
        st->effects.shadow.color = st->effects.shadow.sung_color = 0x80000000u;
        break;
    case FA_LYRIC_SUBTITLE:
        st->subtitle_mode = 1;
        st->wipe = FA_WIPE_SWEEP;
        st->font_size = 48.0f;
        st->font_weight = 600;
        st->vertical_position = 0.84f;
        st->line_pitch = 1.5f;
        st->panel_padding = 0.15f;
        st->lines_above = 0;
        st->lines_below = 1;
        st->next_line_opacity = 0.8f;
        st->near_scale = 1.0f;
        st->countdown = FA_COUNTDOWN_OFF;
        st->scroll_ms = 400;
        st->page_ms = 300;
        st->page_in_scale = 1.0f;
        st->page_out_scale = 1.0f;
        st->panel = 1;
        st->effects.outline.size = 0.15f;
        st->effects.shadow.on = 0;
        break;
    default:
        break;
    }
}

static int in(float v, float lo, float hi) { return v >= lo && v <= hi; }
static int ims(int32_t v, int32_t lo, int32_t hi) { return v >= lo && v <= hi; }

int fa_lyric_style_valid(const fa_lyric_style* s)
{
    return in(s->font_size, 6.0f, 240.0f) && ims(s->font_weight, 100, 900) && ims(s->font_explicit, 0, 1) &&
           (s->font_data == NULL || s->font_data_size > 0) && in(s->vertical_position, 0.0f, 1.0f) &&
           in(s->box_width, 0.2f, 1.0f) && in(s->line_pitch, 0.8f, 4.0f) && ims(s->lines_above, 0, 2) &&
           ims(s->lines_below, 0, 2) && in(s->above_opacity[0], 0.0f, 1.0f) && in(s->above_opacity[1], 0.0f, 1.0f) &&
           in(s->next_line_opacity, 0.0f, 1.0f) && in(s->after_next_factor, 0.0f, 1.0f) &&
           in(s->near_scale, 0.2f, 1.0f) && in(s->far_scale, 0.2f, 1.0f) &&
           ((uint32_t)s->wipe == FA_WIPE_CROSSFADE || (uint32_t)s->wipe == FA_WIPE_SWEEP) &&
           ims(s->wipe_cap_ms, 0, 2000) && in(s->wipe_share, 0.1f, 1.0f) && ims(s->instant_cut_ms, 0, 1000) &&
           ims(s->lead_ms, 0, 2000) && ims(s->paragraph_gap_ms, 0, 60000) && ims(s->sentence_gap_ms, 0, 60000) &&
           ims(s->break_length, 0, 1000) && ims(s->scroll_ms, 0, 4000) && ims(s->page_ms, 0, 4000) &&
           ims(s->page_hold_ms, 0, 10000) && ims(s->page_preview_ms, 0, 10000) && ims(s->page_overlap, 0, 1) &&
           in(s->page_in_scale, 1.0f, 2.0f) && in(s->page_out_scale, 0.25f, 1.0f) && ims(s->end_hold_ms, 0, 60000) &&
           ims(s->end_fade_ms, 0, 60000) && (uint32_t)s->countdown <= FA_COUNTDOWN_BEAT &&
           ims(s->countdown_count, 1, 8) && ims(s->countdown_ms, 1000, 20000) && ims(s->countdown_gap_ms, 0, 60000) &&
           in(s->countdown_size, 0.2f, 1.5f) && ims(s->panel, 0, 1) && in(s->panel_padding, 0.0f, 1.0f) &&
           in(s->panel_radius, 0.0f, 1.0f) && ims(s->subtitle_mode, 0, 1) && fa_fx_valid(&s->effects) &&
           ims(s->intro_page, 0, 1) && ims(s->ending_page, 0, 1) && ims(s->intro_start_ms, 0, 60000) &&
           ims(s->intro_fade_ms, 0, 60000) && ims(s->intro_clear_ms, 0, 60000) && ims(s->intro_band_ms, 0, 60000) &&
           ims(s->ending_min_ms, 0, 60000) && ims(s->ending_fade_ms, 0, 60000);
}

/* ---- the surface's state -------------------------------------------------------- */

/* A line drawn at full size: masks of nch bytes a pixel (shadow, outline, coverage,
 * bevel, and the panel when there is one). */
typedef struct ly_layer {
    int line;                     /* -1: free */
    uint64_t used;
    int w, h, nch;
    uint8_t* m;
    float cx, cy;                 /* the point placed at the line's centre */
    float text_x, text_y;         /* the text box's top-left */
    float* bnd;                   /* nseg + 1: crossfade column boundaries */
    float* gx;                    /* 2 * nseg: each syllable's text from .. to (sweep) */
    int* span;                    /* 2 * h: each row's drawn columns from .. to (0, 0: none) */
    uint8_t* px;                  /* w x h premultiplied BGRA: the masks coloured */
    ly_wipe pw;                   /* the wipe px is coloured for */
    int pw_valid;
    int failed;                   /* `line` could not be drawn: not tried again while cached */
} ly_layer;

/* A line or a page placed in one frame. */
typedef struct ly_inst {
    uint32_t line;                /* LY_PAGE_LINE + page for a page */
    int page;                     /* PG_*, or -1 for a line */
    int outgoing;
    float cx, cy, sc, op;         /* its centre in bitmap pixels */
    ly_wipe wipe;
    ly_count cd;
    int box[4];                   /* bitmap pixels it may touch */
    int abox[4];                  /* and its arrows (when shown) */
    ly_layer* layer;
} ly_inst;

typedef struct ly_song_text {
    char* text;
    fa_time time;
    uint32_t flags;
} ly_song_text;

struct fa_lyric {
    fa_lyric_style st;            /* its strings point to the copies below */
    char* family;
    char* path;
    void* data;
    /* The song. */
    ly_song_text* song;
    uint32_t nsong;
    fa_midi_mark* tempos;
    uint32_t ntempos;
    fa_time duration;
    fa_midi_credits cr;
    int named;                    /* the song names a title or an artist */
    /* The font, and a regular weight of its family for the pages' smaller text (NULL:
     * the font). */
    fa_font* font;
    float bold;                   /* synthetic bold, em */
    fa_font* font2;
    float bold2;
    char font_used[128];
    char* fallback;
    char* labels;
    char* label_buf;
    const char* label[6];         /* Length, Key, Tempo, Timing, Written By, Lyricist */
    /* Layout, for area_w x area_h. */
    int laid;
    uint32_t area_w, area_h;
    float em, pitch, anchor, line_h, asc;
    float tri, tri_w, arrow_gap, arrow_lift;
    int pad;
    int pages;                    /* an intro or ending page may show: the bitmap is the area */
    int block_w;                  /* the lyric block's width (a growing line fits it) */
    int bx, by, bw, bh;           /* the bitmap in the area */
    int content[4];
    uint64_t layout_revision;
    fa_fx_plan plan;
    int fx_on;
    void* scratch;
    size_t scratch_size;
    /* The lines. */
    ly_timeline tl;
    ly_params prm;
    double tempo;
    int built;
    float built_aspect;
    /* Drawing. */
    uint8_t* master;
    uint64_t revision;
    fa_surface_history hist;
    int full;                     /* the next frame redraws everything */
    ly_inst prev[LY_MAX_INST];
    uint32_t nprev;
    ly_layer cache[LY_CACHE];
    uint64_t stamp;
    ly_layer arrow;
    ly_layer pg[LY_PAGES];        /* drawn when first shown; placed with (cx, cy) = (0, 0) */
    int pg_failed[LY_PAGES];      /* could not be drawn (memory): left out until the next layout */
    float pg_x[LY_PAGES], pg_y[LY_PAGES];
    fa_path gpath;
    fa_raster ras;
    float* tmp;
    size_t tmp_cap;
    struct taps* cols;            /* draw_place's taps per output column, grown like tmp */
    size_t cols_cap;
};

static void layer_clear(ly_layer* L)
{
    free(L->m);
    free(L->bnd);
    free(L->gx);
    free(L->span);
    free(L->px);
    memset(L, 0, sizeof(*L));
    L->line = -1;
}

static void cache_clear(fa_lyric* l)
{
    int i;
    for (i = 0; i < LY_CACHE; i++) layer_clear(&l->cache[i]);
    layer_clear(&l->arrow);
    for (i = 0; i < LY_PAGES; i++) {
        layer_clear(&l->pg[i]);
        l->pg_failed[i] = 0;
    }
    l->nprev = 0;
}

/* ---- text ----------------------------------------------------------------------- */

/* The next code point of UTF-8 at s[*i] (U+FFFD for a bad sequence). */
static uint32_t next_cp(const char* s, uint32_t len, uint32_t* i)
{
    const unsigned char* u = (const unsigned char*)s;
    uint32_t c = u[*i], n, k, cp;
    if (c < 0x80) { (*i)++; return c; }
    if (c >= 0xC2 && c < 0xE0) { n = 1; cp = c & 0x1F; }
    else if (c >= 0xE0 && c < 0xF0) { n = 2; cp = c & 0x0F; }
    else if (c >= 0xF0 && c < 0xF5) { n = 3; cp = c & 0x07; }
    else { (*i)++; return 0xFFFD; }
    if (*i + n >= len) { (*i)++; return 0xFFFD; }
    for (k = 1; k <= n; k++) {
        if ((u[*i + k] & 0xC0) != 0x80) { (*i)++; return 0xFFFD; }
        cp = (cp << 6) | (u[*i + k] & 0x3F);
    }
    *i += n + 1;
    return cp;
}

/* Pen positions of a line's text, in em (font size 1): x[b] for each byte b that
 * starts a character, and x[len] the end. Glyphs into `g` (one per character, at
 * its first byte; others 0). Returns the width. */
static float shape_with(fa_font* font, float bold, const char* text, uint32_t len, float* x, uint32_t* g)
{
    const fa_font_info* in = fa_font_get_info(font);
    float upem = (float)in->units_per_em, pen = 0.0f, extra = 2.0f * bold;
    uint32_t i = 0, prev = 0;
    int have = 0;
    while (i < len) {
        uint32_t at = i, cp = next_cp(text, len, &i), gid = fa_font_glyph(font, cp), b;
        if (have) pen += (float)fa_font_kerning(font, prev, gid) / upem;
        if (x) {
            x[at] = pen;
            for (b = at + 1; b < i; b++) x[b] = pen;
        }
        if (g) {
            g[at] = gid;
            for (b = at + 1; b < i; b++) g[b] = UINT32_MAX;
        }
        pen += (float)fa_font_advance(font, gid) / upem + extra;
        prev = gid;
        have = 1;
    }
    if (x) x[len] = pen;
    return pen;
}

static float shape(const fa_lyric* l, const char* text, uint32_t len, float* x, uint32_t* g)
{
    return shape_with(l->font, l->bold, text, len, x, g);
}

static float measure_design(void* ctx, const char* text, uint32_t len)
{
    const fa_lyric* l = (const fa_lyric*)ctx;
    return shape(l, text, len, NULL, NULL) * l->st.font_size;
}

/* ---- the font ------------------------------------------------------------------- */

static const char* lang_of(const fa_lyric* l)
{
    uint32_t i, kana = 0, hangul = 0, han = 0;
    for (i = 0; i < l->nsong; i++) {
        const char* t = l->song[i].text;
        uint32_t len = (uint32_t)strlen(t), j = 0;
        while (j < len) {
            uint32_t cp = next_cp(t, len, &j);
            if ((cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x31F0 && cp <= 0x31FF)) kana++;
            else if ((cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3130 && cp <= 0x318F)) hangul++;
            else if (cp >= 0x4E00 && cp <= 0x9FFF) han++;
        }
    }
    if (kana) return "ja";
    if (hangul) return "ko";
    if (han) return "zh";
    return NULL;
}

/* Characters of the song the font has no glyph for, and all of them (not spaces). */
static void coverage(const fa_lyric* l, const fa_font* f, uint32_t* missing, uint32_t* total)
{
    uint32_t i;
    *missing = *total = 0;
    for (i = 0; i < l->nsong; i++) {
        const char* t = l->song[i].text;
        uint32_t len = (uint32_t)strlen(t), j = 0;
        while (j < len) {
            uint32_t cp = next_cp(t, len, &j);
            if (cp <= 0x20 || cp == 0x3000) continue;
            (*total)++;
            if (!fa_font_glyph(f, cp)) (*missing)++;
        }
    }
}

/* `data`: the lyric's own copy of st->font_data, which the font then reads in place
 * (it outlives the font: style_take frees both together). */
static fa_result font_open(fa_lyric* l, const fa_lyric_style* st, const void* data, fa_font** out, float* bold)
{
    fa_font* f = NULL;
    fa_result r;
    int want = st->font_weight;
    *out = NULL;
    if (st->font_data) {
        r = fa_font_open((const uint8_t*)data, st->font_data_size, st->font_face, 0, &f);
    } else if (st->font_path) {
        r = fa_font_open_file(st->font_path, st->font_face, &f);
    } else {
        r = st->font_family ? fa_font_find(st->font_family, want, 0, &f) : FA_ERROR_NOT_FOUND;
        if (r == FA_ERROR_NOT_FOUND) r = fa_font_find(NULL, want, 0, &f);
    }
    if (r != FA_OK) return r;
    if (!st->font_explicit) {
        /* A song the font mostly cannot show: the system's font for its script. */
        uint32_t miss, total;
        coverage(l, f, &miss, &total);
        if (miss * 2 > total) {
            const char* lang = lang_of(l);
            fa_font* alt = NULL;
            if (lang && fa_font_find_for_language(lang, want, 0, &alt) == FA_OK) {
                uint32_t miss2, total2;
                coverage(l, alt, &miss2, &total2);
                if (miss2 < miss) {
                    fa_font_close(f);
                    f = alt;
                } else {
                    fa_font_close(alt);
                }
            }
        }
    }
    {
        int have = fa_font_get_info(f)->weight;
        *bold = want - have >= 150 ? 0.02f * (float)(want - have) / 300.0f : 0.0f;
    }
    *out = f;
    return FA_OK;
}

/* ---- the song ------------------------------------------------------------------- */

static fa_result read_song(fa_lyric* l, fa_stream* s)
{
    fa_midi_info mi;
    fa_stream_info si;
    fa_midi_text t;
    uint32_t i, n = 0, count = 0;
    fa_result r = fa_stream_get_midi_info(s, &mi);
    if (r != FA_OK) return r;
    l->song = (ly_song_text*)calloc(mi.lyric_count ? mi.lyric_count : 1, sizeof(ly_song_text));
    if (!l->song) return FA_ERROR_OUT_OF_MEMORY;
    for (i = 0; i < mi.text_count && n < mi.lyric_count; i++) {
        if (fa_stream_get_midi_text(s, i, &t) != FA_OK || t.type != FA_MIDI_LYRIC) continue;
        l->song[n].text = fa_strdup(t.text);
        if (!l->song[n].text) return FA_ERROR_OUT_OF_MEMORY;
        l->song[n].time = t.time;
        l->song[n].flags = t.flags;
        l->nsong = ++n;   /* freed by fa_lyric_free from here on */
    }
    if (fa_stream_get_midi_marks(s, NULL, 0, &count) == FA_OK && count) {
        fa_midi_mark* all = (fa_midi_mark*)malloc(count * sizeof(fa_midi_mark));
        uint32_t got = 0, k = 0;
        if (!all) return FA_ERROR_OUT_OF_MEMORY;
        fa_stream_get_midi_marks(s, all, count, &got);
        for (i = 0; i < got && i < count; i++)
            if (all[i].type == FA_MIDI_MARK_TEMPO && all[i].value > 0) all[k++] = all[i];
        l->tempos = all;
        l->ntempos = k;
    }
    if (fa_stream_get_info(s, &si) == FA_OK && si.duration > 0) l->duration = si.duration;
    l->cr.struct_size = (uint32_t)sizeof(l->cr);
    if (fa_stream_get_midi_credits(s, &l->cr) != FA_OK) {
        memset(&l->cr, 0, sizeof(l->cr));
        l->cr.struct_size = (uint32_t)sizeof(l->cr);
    }
    l->named = l->cr.title[0] || l->cr.artist[0];
    return FA_OK;
}

static fa_time beat_at(void* ctx, fa_time at)
{
    const fa_lyric* l = (const fa_lyric*)ctx;
    int32_t us = 500000;
    uint32_t i;
    for (i = 0; i < l->ntempos && l->tempos[i].time <= at; i++) us = l->tempos[i].value;
    return (fa_time)us * 1000;
}

/* ---- the timeline --------------------------------------------------------------- */

static void params_make(const fa_lyric* l, double tempo, ly_params* p)
{
    const fa_lyric_style* s = &l->st;
    /* Animation lengths are real time: stream time runs `tempo` times faster. */
    double k = tempo > 0.0 ? tempo : 1.0;
#define RT(ms) ((fa_time)((double)(ms) * (double)LY_MS * k))
    memset(p, 0, sizeof(*p));
    p->lead = (fa_time)s->lead_ms * LY_MS;
    p->paragraph_gap = (fa_time)s->paragraph_gap_ms * LY_MS;
    p->sentence_gap = (fa_time)s->sentence_gap_ms * LY_MS;
    p->break_length = s->break_length;
    p->scroll = RT(s->scroll_ms);
    p->page = RT(s->page_ms);
    p->page_hold = RT(s->page_hold_ms);
    p->page_preview = RT(s->page_preview_ms);
    p->page_overlap = s->page_overlap;
    p->end_hold = RT(s->end_hold_ms);
    p->end_fade = RT(s->end_fade_ms);
    p->countdown = s->subtitle_mode ? FA_COUNTDOWN_OFF : (int)s->countdown;
    p->countdown_count = s->countdown_count;
    p->countdown_time = RT(s->countdown_ms);
    p->countdown_gap = (fa_time)s->countdown_gap_ms * LY_MS;
    p->wipe_cap = RT(s->wipe_cap_ms);
    p->instant_cut = RT(s->instant_cut_ms);
    p->wipe_share = s->wipe_share;
    p->lines_above = s->lines_above;
    p->lines_below = s->lines_below;
    p->above_opacity[0] = s->above_opacity[0];
    p->above_opacity[1] = s->above_opacity[1];
    p->next_opacity = s->next_line_opacity;
    p->after_next_factor = s->after_next_factor;
    p->near_scale = s->near_scale;
    p->far_scale = s->far_scale;
    p->page_in_scale = s->page_in_scale;
    p->page_out_scale = s->page_out_scale;
    p->duration = l->duration;
    p->intro = !s->subtitle_mode && s->intro_page && (l->named || (l->fallback && l->fallback[0]));
    p->ending = !s->subtitle_mode && s->ending_page && l->named;
    p->intro_start = RT(s->intro_start_ms);
    p->intro_fade = RT(s->intro_fade_ms);
    p->intro_clear = RT(s->intro_clear_ms);
    p->intro_band = RT(s->intro_band_ms);
    p->ending_min = RT(s->ending_min_ms);
    p->ending_fade = RT(s->ending_fade_ms);
#undef RT
}

/* The lines for the area's shape (lines are split to the box's width). */
static fa_result timeline_build(fa_lyric* l, float aspect, double tempo)
{
    ly_frag* f;
    uint32_t i;
    fa_result r;
    if (l->built && l->built_aspect == aspect) {
        if (l->tempo != tempo) {
            params_make(l, tempo, &l->prm);
            ly_schedule(&l->tl, &l->prm, beat_at, l);
            l->tempo = tempo;
        }
        return FA_OK;
    }
    ly_free(&l->tl);
    l->built = 0;
    params_make(l, tempo, &l->prm);
    f = (ly_frag*)malloc((l->nsong ? l->nsong : 1) * sizeof(ly_frag));
    if (!f) return FA_ERROR_OUT_OF_MEMORY;
    for (i = 0; i < l->nsong; i++) {
        f[i].text = l->song[i].text;
        f[i].time = l->song[i].time;
        f[i].flags = l->song[i].flags;
    }
    r = ly_build(&l->tl, f, l->nsong, &l->prm, measure_design, l, l->st.box_width * (float)LY_DESIGN_H * aspect, beat_at, l);
    free(f);
    if (r != FA_OK) return r;
    l->built = 1;
    l->built_aspect = aspect;
    l->tempo = tempo;
    return FA_OK;
}

/* ---- layout --------------------------------------------------------------------- */

static float stable_scale(const fa_lyric_style* s, int rel)
{
    int d = rel < 0 ? -rel : rel;
    return d == 0 ? 1.0f : d == 1 ? s->near_scale : d == 2 ? s->far_scale : s->far_scale * (2.0f / 3.0f);
}

static fa_result layout(fa_lyric* l, uint32_t aw, uint32_t ah, double tempo)
{
    const fa_lyric_style* s = &l->st;
    const fa_font_info* fi = fa_font_get_info(l->font);
    float W, H, k, half, row, top = 1e30f, bottom = -1e30f, boxw, x0, x1, reach;
    int rel, cd;
    fa_result r;
    if (!aw || !ah) {
        aw = LY_DESIGN_W;
        ah = LY_DESIGN_H;
    }
    r = timeline_build(l, (float)aw / (float)ah, tempo);
    if (r != FA_OK) return r;
    if (l->laid && l->area_w == aw && l->area_h == ah) return FA_OK;
    W = (float)aw;
    H = (float)ah;
    k = H / (float)LY_DESIGN_H;
    l->em = s->font_size * k;
    l->pitch = s->line_pitch * l->em;
    l->anchor = s->vertical_position * H;
    l->asc = (float)fi->ascender / (float)fi->units_per_em * l->em;
    l->line_h = (float)(fi->ascender - fi->descender) / (float)fi->units_per_em * l->em;
    if (!(l->line_h > 0.0f)) {
        l->asc = 0.9f * l->em;
        l->line_h = 1.2f * l->em;
    }
    l->fx_on = fa_fx_plan_make(&l->plan, &s->effects, 0.12f * l->em, 0.20f * l->em, 0.15f * l->em, 0.08f * l->em, 0);
    reach = (l->fx_on ? (float)l->plan.reach : 0.0f) + l->bold * l->em;
    if (s->panel && s->panel_padding * l->em + 1.0f > reach) reach = s->panel_padding * l->em + 1.0f;
    l->pad = (int)ceilf(reach) + 2;
    if (l->fx_on) {
        size_t need = fa_fx_scratch_bytes(&l->plan);
        if (need > l->scratch_size) {
            void* n = realloc(l->scratch, need);
            if (!n) return FA_ERROR_OUT_OF_MEMORY;
            l->scratch = n;
            l->scratch_size = need;
        }
    }
    cd = l->prm.countdown != FA_COUNTDOWN_OFF;
    l->tri = 0.6f * s->countdown_size * l->em;
    l->tri_w = 0.866f * l->tri;
    l->arrow_gap = 0.12f * l->em;
    l->arrow_lift = 0.1f * l->em;
    /* The block: every slot at the largest it is drawn (a new page grows in), the
     * lines leaving the top and coming in at the bottom as they scroll, and the
     * countdown row. */
    half = l->line_h / 2.0f + (float)l->pad;
    row = cd ? l->arrow_lift + l->tri + (float)l->pad : 0.0f;
    for (rel = -(s->lines_above + 1); rel <= s->lines_below + 1; rel++) {
        float sm = stable_scale(s, rel), cy = l->anchor + (float)rel * l->pitch;
        if (rel + 1 <= s->lines_below && stable_scale(s, rel + 1) > sm) sm = stable_scale(s, rel + 1);
        if (rel >= 0 && rel <= s->lines_below && stable_scale(s, rel) * s->page_in_scale > sm)
            sm = stable_scale(s, rel) * s->page_in_scale;
        if (cy - sm * (half + row) < top) top = cy - sm * (half + row);
        if (cy + sm * half > bottom) bottom = cy + sm * half;
    }
    boxw = s->box_width * W;
    x0 = W / 2.0f - boxw / 2.0f - (float)l->pad;
    x1 = W / 2.0f + boxw / 2.0f + (float)l->pad;
    if (x0 < 0.0f) x0 = 0.0f;
    if (x1 > W) x1 = W;
    if (top < 0.0f) top = 0.0f;
    if (bottom > H) bottom = H;
    /* The content: the lyric block less its padding. */
    {
        int t = (int)floorf(top), b = (int)ceilf(bottom), p = l->pad < (b - t) / 2 ? l->pad : 0;
        l->content[1] = t + p;
        l->content[3] = b - t - 2 * p;
    }
    /* Pages are drawn anywhere in the area: then the bitmap is all of it. */
    l->block_w = (int)ceilf(x1) - (int)floorf(x0);
    l->pages = l->tl.n && (l->prm.intro || l->prm.ending);
    if (l->pages) {
        x0 = top = 0.0f;
        x1 = W;
        bottom = H;
    }
    l->bx = (int)floorf(x0);
    l->by = (int)floorf(top);
    l->bw = (int)ceilf(x1) - l->bx;
    l->bh = (int)ceilf(bottom) - l->by;
    if (l->bw < 1) l->bw = 1;
    if (l->bh < 1) l->bh = 1;
    l->content[0] = (int)floorf(W / 2.0f - boxw / 2.0f) - l->bx;
    if (l->content[0] < 0) l->content[0] = 0;
    l->content[2] = (int)ceilf(boxw);
    if (l->content[0] + l->content[2] > l->bw) l->content[2] = l->bw - l->content[0];
    l->content[1] -= l->by;
    {
        uint8_t* m = (uint8_t*)realloc(l->master, (size_t)l->bw * (size_t)l->bh * 4);
        if (!m) return FA_ERROR_OUT_OF_MEMORY;
        l->master = m;
        memset(m, 0, (size_t)l->bw * (size_t)l->bh * 4);
    }
    cache_clear(l);
    /* Areas of the old layout mean nothing in the new one: every buffer is drawn in full. */
    memset(&l->hist, 0, sizeof(l->hist));
    l->area_w = aw;
    l->area_h = ah;
    l->laid = 1;
    l->full = 1;
    l->layout_revision++;
    return FA_OK;
}

/* Something changed that needs everything drawn again (style, effects). */
static void invalidate(fa_lyric* l, int lines)
{
    l->laid = 0;
    if (lines) {
        ly_free(&l->tl);
        l->built = 0;
    }
}

/* ---- drawing a line ------------------------------------------------------------- */

static int raster_shape(fa_lyric* l, int w, int h, uint8_t** cov)
{
    *cov = (uint8_t*)calloc((size_t)w * (size_t)h, 1);
    if (!*cov) return 0;
    if (!fa_raster_begin(&l->ras, w, h)) {
        free(*cov);
        *cov = NULL;
        return 0;
    }
    return 1;
}

/* Masks from a coverage plane: the effects (or the coverage alone), the synthetic
 * bold first. Takes `cov`. */
static int make_masks(fa_lyric* l, ly_layer* L, uint8_t* cov, int nch)
{
    size_t px = (size_t)L->w * (size_t)L->h, i;
    if (l->bold > 0.0f) {
        uint8_t* b = (uint8_t*)malloc(px);
        if (!b || !fa_fx_dilate(cov, (size_t)L->w, L->w, L->h, l->bold * l->em, b, (size_t)L->w)) {
            free(b);
            free(cov);
            return 0;
        }
        free(cov);
        cov = b;
    }
    L->nch = nch;
    L->m = (uint8_t*)calloc(px, (size_t)nch);
    if (!L->m) {
        free(cov);
        return 0;
    }
    if (l->fx_on) {
        fa_fx_masks(&l->plan, cov, (size_t)L->w, L->w, L->h, L->m, (size_t)L->w * (size_t)nch, nch, 0, 0, L->w, L->h,
                    l->scratch);
    } else {
        for (i = 0; i < px; i++) {
            L->m[i * (size_t)nch + 2] = cov[i];
            L->m[i * (size_t)nch + 3] = 128;
        }
    }
    free(cov);
    return 1;
}

/* Coverage of a rounded rectangle at pixel centres. */
static void panel_mask(ly_layer* L, float x0, float y0, float x1, float y1, float radius)
{
    float hw = (x1 - x0) / 2.0f, hh = (y1 - y0) / 2.0f, cx = (x0 + x1) / 2.0f, cy = (y0 + y1) / 2.0f;
    int x, y;
    if (radius > hw) radius = hw;
    if (radius > hh) radius = hh;
    for (y = 0; y < L->h; y++)
        for (x = 0; x < L->w; x++) {
            float qx = fabsf((float)x + 0.5f - cx) - (hw - radius), qy = fabsf((float)y + 0.5f - cy) - (hh - radius);
            float ox = qx > 0.0f ? qx : 0.0f, oy = qy > 0.0f ? qy : 0.0f;
            float d = sqrtf(ox * ox + oy * oy) + (qx > qy ? (qx < 0.0f ? qx : 0.0f) : (qy < 0.0f ? qy : 0.0f)) - radius;
            float c = 0.5f - d;
            L->m[((size_t)y * (size_t)L->w + (size_t)x) * 5 + 4] = (uint8_t)(c <= 0.0f ? 0 : c >= 1.0f ? 255 : (int)(c * 255.0f + 0.5f));
        }
}

/* Each row's columns with anything in them, so drawing skips the empty parts. */
static int spans_make(ly_layer* L)
{
    int x, y, c;
    L->span = (int*)malloc((size_t)L->h * 2 * sizeof(int));
    if (!L->span) return 0;
    for (y = 0; y < L->h; y++) {
        const uint8_t* r = L->m + (size_t)y * (size_t)L->w * (size_t)L->nch;
        int a = L->w, b = 0;
        for (x = 0; x < L->w; x++, r += L->nch) {
            int any = r[3] != 128;
            for (c = 0; c < L->nch && !any; c++) any = c != 3 && r[c];
            if (any) {
                if (x < a) a = x;
                b = x + 1;
            }
        }
        L->span[2 * y] = a < b ? a : 0;
        L->span[2 * y + 1] = b;
    }
    return 1;
}

static int layer_make(fa_lyric* l, uint32_t index, ly_layer* L)
{
    const ly_line* line = &l->tl.line[index];
    const fa_font_info* fi = fa_font_get_info(l->font);
    float size = l->em * line->fit, gs = size / (float)fi->units_per_em, tw, lh = l->line_h * line->fit, base;
    float* x = (float*)malloc(((size_t)line->text_len + 1) * sizeof(float));
    uint32_t* g = (uint32_t*)malloc(((size_t)line->text_len + 1) * sizeof(uint32_t));
    uint8_t* cov;
    uint32_t i, j;
    int nch = l->st.panel ? 5 : 4;
    layer_clear(L);
    if (!x || !g) goto fail;
    tw = shape(l, line->text, line->text_len, x, g) * size;
    if (tw < 1.0f) tw = 1.0f;
    /* Never taller than the area, nor wider than 8 times it (a long line without a
     * place to break is shrunk to fit later): a damaged font's metrics must not make a
     * layer of gigabytes. */
    if (!(tw <= 8.0f * (float)l->area_w) || !(lh <= (float)l->area_h)) goto fail;
    L->w = (int)ceilf(tw) + 2 * l->pad;
    L->h = (int)ceilf(lh) + 2 * l->pad;
    L->text_x = (float)l->pad;
    L->text_y = (float)l->pad;
    L->cx = (float)l->pad + tw / 2.0f;
    L->cy = (float)l->pad + lh / 2.0f;
    base = (float)l->pad + l->asc * line->fit;
    if (!raster_shape(l, L->w, L->h, &cov)) goto fail;
    for (i = 0; i < line->text_len; i++) {
        if (g[i] == UINT32_MAX || line->text[i] == ' ') continue;
        if (fa_font_outline(l->font, g[i], &l->gpath) != FA_OK || !l->gpath.count) continue;
        fa_raster_path(&l->ras, &l->gpath, gs, -gs, (float)l->pad + x[i] * size + l->bold * l->em, base);
    }
    fa_raster_end(&l->ras, cov, (size_t)L->w);
    if (!make_masks(l, L, cov, nch)) goto fail;
    if (l->st.panel) {
        float pp = l->st.panel_padding * l->em;
        panel_mask(L, L->text_x - pp, L->text_y - pp, L->text_x + tw + pp, L->text_y + lh + pp, l->st.panel_radius * l->em);
    }
    if (!spans_make(L)) goto fail;
    L->bnd = (float*)malloc(((size_t)line->nseg + 1) * sizeof(float));
    L->gx = (float*)malloc(((size_t)line->nseg * 2 + 1) * sizeof(float));
    if (!L->bnd || !L->gx) goto fail;
    for (j = 0; j < line->nseg; j++) {
        L->gx[2 * j] = (float)l->pad + x[line->seg[j].start] * size;
        L->gx[2 * j + 1] = (float)l->pad + x[line->seg[j].start + line->seg[j].len] * size;
    }
    L->bnd[0] = -1e9f;
    for (j = 1; j < line->nseg; j++) L->bnd[j] = (L->gx[2 * j - 1] + L->gx[2 * j]) / 2.0f;
    L->bnd[line->nseg] = 1e9f;
    free(x);
    free(g);
    L->line = (int)index;
    return 1;
fail:
    free(x);
    free(g);
    layer_clear(L);
    return 0;
}

static int arrow_make(fa_lyric* l)
{
    ly_layer* L = &l->arrow;
    fa_path p;
    fa_path_cmd c[4];
    uint8_t* cov;
    float x0 = (float)l->pad, y0 = (float)l->pad;
    if (L->m) return 1;
    layer_clear(L);
    L->w = (int)ceilf(l->tri_w) + 2 * l->pad;
    L->h = (int)ceilf(l->tri) + 2 * l->pad;
    L->cx = x0 + l->tri_w / 2.0f;
    L->cy = y0 + l->tri / 2.0f;
    memset(c, 0, sizeof(c));
    c[0].op = FA_PATH_MOVE; c[0].x = x0; c[0].y = y0;
    c[1].op = FA_PATH_LINE; c[1].x = x0; c[1].y = y0 + l->tri;
    c[2].op = FA_PATH_LINE; c[2].x = x0 + l->tri_w; c[2].y = y0 + l->tri / 2.0f;
    memset(&p, 0, sizeof(p));
    p.cmd = c;
    p.count = 3;
    if (!raster_shape(l, L->w, L->h, &cov)) return 0;
    fa_raster_path(&l->ras, &p, 1.0f, 1.0f, 0.0f, 0.0f);
    fa_raster_end(&l->ras, cov, (size_t)L->w);
    if (!make_masks(l, L, cov, 4) || !spans_make(L) || !(L->px = (uint8_t*)malloc((size_t)L->w * (size_t)L->h * 4))) {
        layer_clear(L);
        return 0;
    }
    L->line = 0;
    return 1;
}

static ly_layer* layer_get(fa_lyric* l, uint32_t index)
{
    int i, best = 0;
    for (i = 0; i < LY_CACHE; i++)
        if (l->cache[i].line == (int)index && (l->cache[i].m || l->cache[i].failed)) {
            l->cache[i].used = ++l->stamp;
            return l->cache[i].failed ? NULL : &l->cache[i];
        }
    /* The least recently used; a line that failed holds its entry like a drawn one, so
     * it is not tried again every frame (as pg_failed). */
#define LY_HELD(L) ((L)->m || (L)->failed)
    for (i = 1; i < LY_CACHE; i++)
        if (!LY_HELD(&l->cache[i]) || (LY_HELD(&l->cache[best]) && l->cache[i].used < l->cache[best].used)) best = i;
#undef LY_HELD
    if (!layer_make(l, index, &l->cache[best])) {
        l->cache[best].line = (int)index;
        l->cache[best].failed = 1;
    }
    l->cache[best].used = ++l->stamp;
    return l->cache[best].failed ? NULL : &l->cache[best];
}

/* ---- colours -------------------------------------------------------------------- */

/* Straight 0xAARRGGBB lerped, its alpha times `k`, premultiplied, B G R A 0..1. */
static void color_mix(uint32_t a, uint32_t b, float t, float k, float out[4])
{
    float ch[4];
    int i;
    for (i = 0; i < 4; i++) {
        float x = (float)((a >> (8 * i)) & 0xFF), y = (float)((b >> (8 * i)) & 0xFF);
        ch[i] = (x + (y - x) * t) / 255.0f;
    }
    /* ch: B, G, R, A */
    ch[3] *= k;
    out[0] = ch[0] * ch[3];
    out[1] = ch[1] * ch[3];
    out[2] = ch[2] * ch[3];
    out[3] = ch[3];
}

static void over(float acc[4], const float c[4], float w)
{
    float keep;
    if (w <= 0.0f) return;
    keep = 1.0f - c[3] * w;
    acc[0] = c[0] * w + acc[0] * keep;
    acc[1] = c[1] * w + acc[1] * keep;
    acc[2] = c[2] * w + acc[2] * keep;
    acc[3] = c[3] * w + acc[3] * keep;
}

/* How sung column u (its centre, layer pixels) of a line's layer is. */
static float sung_at(const fa_lyric* l, const ly_layer* L, const ly_line* line, const ly_wipe* w, float u)
{
    uint32_t j = 0;
    if (w->full == 0 && w->current < 0) return 0.0f;
    while (j + 1 < line->nseg + 1 && u >= L->bnd[j + 1]) j++;
    if ((int)j < w->full) return 1.0f;
    if ((int)j != w->current) return 0.0f;
    if (l->st.wipe == FA_WIPE_SWEEP) {
        float x0 = L->gx[2 * j], x1 = L->gx[2 * j + 1], edge = x0 + (x1 - x0) * w->mix, v = edge - u + 0.5f;
        if (w->mix >= 1.0f) return 1.0f;
        return v <= 0.0f ? 0.0f : v >= 1.0f ? 1.0f : v;
    }
    return w->mix;
}

static float seg_mix(const ly_wipe* w, uint32_t j)
{
    if ((int)j < w->full) return 1.0f;
    return (int)j == w->current ? w->mix : 0.0f;
}

/* Exactly: a frame left out for a tiny step would leave the next ones drawn from a
 * place a fresh draw would not use. */
static int same(float a, float b) { return a == b; }

/* Colours columns [c0, c1) of a layer's picture from its masks: a line's colours at
 * its wipe (`line` set) or one colour (an arrow). */
static void color_columns(const fa_lyric* l, ly_layer* L, const ly_line* line, const ly_wipe* w, uint32_t color,
                          int c0, int c1)
{
    const fa_surface_effects* e = &l->st.effects;
    float bev_hi[4], bev_sh[4], panel[4], fill[4], outl[4], shad[4];
    int x, y, nch = L->nch;
    color_mix(e->bevel.highlight, e->bevel.highlight, 0.0f, 1.0f, bev_hi);
    color_mix(e->bevel.shade, e->bevel.shade, 0.0f, 1.0f, bev_sh);
    color_mix(l->st.panel_color, l->st.panel_color, 0.0f, 1.0f, panel);
    if (c0 < 0) c0 = 0;
    if (c1 > L->w) c1 = L->w;
    for (x = c0; x < c1; x++) {
        if (line) {
            float sg = sung_at(l, L, line, w, (float)x + 0.5f);
            color_mix(l->st.text_color, l->st.sung_color, sg, 1.0f, fill);
            color_mix(e->outline.color, e->outline.sung_color, sg, e->outline.opacity, outl);
            color_mix(e->shadow.color, e->shadow.sung_color, sg, e->shadow.opacity, shad);
        } else {
            color_mix(color, color, 0.0f, 1.0f, fill);
            color_mix(e->outline.color, e->outline.color, 0.0f, e->outline.opacity, outl);
            color_mix(e->shadow.color, e->shadow.color, 0.0f, e->shadow.opacity, shad);
        }
        for (y = 0; y < L->h; y++) {
            const uint8_t* m = L->m + ((size_t)y * (size_t)L->w + (size_t)x) * (size_t)nch;
            uint8_t* d = L->px + ((size_t)y * (size_t)L->w + (size_t)x) * 4;
            float acc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            int c;
            if (nch == 5) over(acc, panel, (float)m[4] / 255.0f);
            if (l->fx_on && l->plan.shadow) over(acc, shad, (float)m[0] / 255.0f);
            if (l->fx_on && l->plan.outline) over(acc, outl, (float)m[1] / 255.0f);
            over(acc, fill, (float)m[2] / 255.0f);
            if (l->fx_on && l->plan.bevel) {
                float b = (float)((int)m[3] - 128) / 127.0f;
                if (b > 0.0f) over(acc, bev_hi, b);
                else over(acc, bev_sh, -b);
            }
            for (c = 0; c < 4; c++) {
                float v = acc[c] * 255.0f + 0.5f;
                d[c] = (uint8_t)(v >= 255.0f ? 255.0f : v <= 0.0f ? 0.0f : v);
            }
        }
    }
}

/* The layer columns [*c0, *c1) whose colour differs between wipes a and b: 0 when none.
 * A sweep whose edge only moved inside the current syllable: column x is sung by
 * edge - x clamped to 0..1, so only those between the two edges change. Otherwise the
 * syllables whose colour changed (exactly: a sweep's last columns turn when mix
 * reaches 1, and the gap before a syllable when it becomes the current one). */
static int changed_columns(const fa_lyric* l, const ly_layer* L, const ly_line* line, const ly_wipe* a,
                           const ly_wipe* b, int* c0, int* c1)
{
    if (l->st.wipe == FA_WIPE_SWEEP && a->current >= 0 && a->current == b->current && a->full == b->full &&
        a->mix < 1.0f && b->mix < 1.0f) {
        float x0 = L->gx[2 * a->current], x1 = L->gx[2 * a->current + 1];
        float e0 = x0 + (x1 - x0) * a->mix, e1 = x0 + (x1 - x0) * b->mix;
        if (same(a->mix, b->mix)) return 0;
        *c0 = (int)floorf(e0 < e1 ? e0 : e1) - 1;
        *c1 = (int)ceilf(e0 < e1 ? e1 : e0) + 1;
    } else {
        int first = -1, last = -1;
        uint32_t j;
        float u0, u1;
        for (j = 0; j < line->nseg; j++)
            if (seg_mix(a, j) != seg_mix(b, j) || (a->current == (int)j) != (b->current == (int)j)) {
                if (first < 0) first = (int)j;
                last = (int)j;
            }
        if (first < 0) return 0;
        u0 = first ? L->bnd[first] : 0.0f;
        u1 = (uint32_t)last + 1 < line->nseg ? L->bnd[last + 1] : (float)L->w;
        *c0 = (int)floorf(u0 - 0.5f);
        *c1 = (int)ceilf(u1 + 0.5f);
    }
    if (*c0 < 0) *c0 = 0;
    if (*c1 > L->w) *c1 = L->w;
    return *c0 < *c1;
}

/* Brings a line's picture to a wipe: only the columns whose colour changed. */
static int color_line(const fa_lyric* l, ly_layer* L, const ly_line* line, const ly_wipe* w)
{
    int c0, c1;
    if (!L->px) {
        L->px = (uint8_t*)malloc((size_t)L->w * (size_t)L->h * 4);
        if (!L->px) return 0;
        L->pw_valid = 0;
    }
    if (!L->pw_valid) {
        color_columns(l, L, line, w, 0, 0, L->w);
    } else if (changed_columns(l, L, line, &L->pw, w, &c0, &c1)) {
        color_columns(l, L, line, w, 0, c0, c1);
    }
    L->pw = *w;
    L->pw_valid = 1;
    return 1;
}

/* ---- placing a layer ------------------------------------------------------------ */

/* Taps of the tent filter for output pixel centres x + 0.5 of a layer placed with
 * its point `c` at `at`, scaled by sc: first source index, count, weights. */
typedef struct taps {
    int first, n;
    float w[16];
} taps;

static void taps_make(taps* t, float out_centre, float at, float c, float sc, int size)
{
    float u = (out_centre - at) / sc + c - 0.5f, rad = sc < 1.0f ? 1.0f / sc : 1.0f, sum = 0.0f;
    int a = (int)floorf(u - rad) + 1, b = (int)floorf(u + rad), i;
    if (b - a + 1 > 16) {
        a = (int)floorf(u) - 7;
        b = a + 15;
    }
    t->first = a;
    t->n = b - a + 1;
    if (t->n < 1) {
        t->n = 1;
        t->first = (int)floorf(u + 0.5f);
        t->w[0] = 1.0f;
        if (t->first < 0 || t->first >= size) t->n = 0;
        return;
    }
    for (i = 0; i < t->n; i++) {
        float d = fabsf((float)(a + i) - u) / rad, w = 1.0f - d;
        t->w[i] = w > 0.0f ? w : 0.0f;
        sum += t->w[i];
    }
    if (sum <= 0.0f) {
        t->n = 1;
        t->first = (int)floorf(u + 0.5f);
        t->w[0] = 1.0f;
        if (t->first < 0 || t->first >= size) t->n = 0;
        return;
    }
    for (i = 0; i < t->n; i++) t->w[i] /= sum;
    /* Only taps inside the layer with a weight: outside it is transparent. */
    while (t->n > 0 && (t->first < 0 || t->w[0] <= 0.0f)) {
        memmove(t->w, t->w + 1, (size_t)(t->n - 1) * sizeof(float));
        t->first++;
        t->n--;
    }
    while (t->n > 0 && (t->first + t->n > size || t->w[t->n - 1] <= 0.0f)) t->n--;
}

static int ensure_tmp(fa_lyric* l, size_t floats)
{
    float* n;
    if (floats <= l->tmp_cap) return 1;
    n = (float*)realloc(l->tmp, floats * sizeof(float));
    if (!n) return 0;
    l->tmp = n;
    l->tmp_cap = floats;
    return 1;
}

/* draw_place's column taps for `n` output columns; 0 = out of memory. */
static int ensure_cols(fa_lyric* l, size_t n)
{
    taps* c;
    if (n <= l->cols_cap) return 1;
    c = (taps*)realloc(l->cols, n * sizeof(taps));
    if (!c) return 0;
    l->cols = c;
    l->cols_cap = n;
    return 1;
}

#if LY_SSE
/* One RGBA pixel to four floats and back (clamped at 255, truncated): the SSE
 * paths below do the scalar arithmetic in the same order, so the result is the
 * same to the bit. */
static inline __m128 px_to_ps(const uint8_t* p)
{
    uint32_t v;
    __m128i z = _mm_setzero_si128(), x;
    memcpy(&v, p, 4);
    x = _mm_cvtsi32_si128((int)v);
    x = _mm_unpacklo_epi16(_mm_unpacklo_epi8(x, z), z);
    return _mm_cvtepi32_ps(x);
}
static inline void ps_to_px(uint8_t* p, __m128 m)
{
    __m128i x = _mm_cvttps_epi32(_mm_min_ps(m, _mm_set1_ps(255.0f)));
    uint32_t v = (uint32_t)_mm_cvtsi128_si32(_mm_packus_epi16(_mm_packs_epi32(x, x), x));
    memcpy(p, &v, 4);
}
#endif

/* Draws a layer's picture over the master inside [x0, x1) x [y0, y1), its point
 * (L->cx, L->cy) at (cx, cy), scaled by sc, at opacity op: resampled with the tent
 * filter, rows first, over the columns each row has anything in. */
static void draw_place(fa_lyric* l, const ly_layer* L, float cx, float cy, float sc, float op, int x0, int y0, int x1,
                       int y1)
{
    int x, y, i, ua, ub, k;
    int dx0 = (int)floorf(cx - L->cx * sc) - 2, dy0 = (int)floorf(cy - L->cy * sc) - 2;
    int dx1 = (int)ceilf(cx + ((float)L->w - L->cx) * sc) + 2, dy1 = (int)ceilf(cy + ((float)L->h - L->cy) * sc) + 2;
    taps* cols;
    if (dx0 > x0) x0 = dx0;
    if (dy0 > y0) y0 = dy0;
    if (dx1 < x1) x1 = dx1;
    if (dy1 < y1) y1 = dy1;
    if (x0 >= x1 || y0 >= y1 || op <= 0.0f) return;
    if (sc == 1.0f && cx - L->cx == floorf(cx - L->cx) && cy - L->cy == floorf(cy - L->cy)) {
        /* On whole pixels at full size (a page): the picture itself, at op. */
        int ox = (int)(cx - L->cx), oy = (int)(cy - L->cy);
        for (y = y0; y < y1; y++) {
            int v = y - oy, a, b;
            uint8_t* d;
            const uint8_t* src;
            if (v < 0 || v >= L->h) continue;
            a = L->span[2 * v] + ox;
            b = L->span[2 * v + 1] + ox;
            if (a < x0) a = x0;
            if (b > x1) b = x1;
            d = l->master + ((size_t)y * (size_t)l->bw + (size_t)a) * 4;
            src = L->px + ((size_t)v * (size_t)L->w + (size_t)(a - ox)) * 4;
#if LY_SSE
            {
                __m128 vop = _mm_set1_ps(op), v255 = _mm_set1_ps(255.0f), vh = _mm_set1_ps(0.5f), one = _mm_set1_ps(1.0f);
                for (x = a; x < b; x++, d += 4, src += 4) {
                    __m128 sp, dd, keep, m;
                    uint32_t sv;
                    memcpy(&sv, src, 4);
                    if (!sv) continue;
                    sp = px_to_ps(src); dd = px_to_ps(d);
                    keep = _mm_sub_ps(one, _mm_div_ps(_mm_mul_ps(_mm_shuffle_ps(sp, sp, 0xFF), vop), v255));
                    m = _mm_add_ps(_mm_add_ps(_mm_mul_ps(sp, vop), _mm_mul_ps(dd, keep)), vh);
                    ps_to_px(d, m);
                }
            }
#else
            for (x = a; x < b; x++, d += 4, src += 4) {
                float keep = 1.0f - (float)src[3] * op / 255.0f;
                for (k = 0; k < 4; k++) {
                    float m = (float)src[k] * op + (float)d[k] * keep + 0.5f;
                    d[k] = (uint8_t)(m >= 255.0f ? 255.0f : m);
                }
            }
#endif
        }
        return;
    }
    if (!ensure_cols(l, (size_t)(x1 - x0))) return;
    cols = l->cols;
    /* Per output column its taps; the source columns they read: [ua, ub). */
    ua = INT32_MAX;
    ub = INT32_MIN;
    for (x = x0; x < x1; x++) {
        taps* t = &cols[x - x0];
        taps_make(t, (float)x + 0.5f, cx, L->cx, sc, L->w);
        if (t->n && t->first < ua) ua = t->first;
        if (t->n && t->first + t->n > ub) ub = t->first + t->n;
    }
    if (ub <= ua || !ensure_tmp(l, (size_t)(ub - ua) * 4)) return;
    /* tmp is all 0 between rows: each row clears what it used. */
    memset(l->tmp, 0, (size_t)(ub - ua) * 4 * sizeof(float));
    for (y = y0; y < y1; y++) {
        taps rt;
        uint8_t* d = l->master + ((size_t)y * (size_t)l->bw + (size_t)x0) * 4;
        int sa = ub, sb = ua;
        taps_make(&rt, (float)y + 0.5f, cy, L->cy, sc, L->h);
        /* Down the column first: tmp[c] = the rows' weighted sum at source column c,
         * over the columns [sa, sb) that have anything. */
        for (k = 0; k < rt.n; k++) {
            int v = rt.first + k, a = L->span[2 * v], b = L->span[2 * v + 1];
            const uint8_t* src;
            float w = rt.w[k], *o;
            if (a < ua) a = ua;
            if (b > ub) b = ub;
            if (a >= b) continue;
            if (a < sa) sa = a;
            if (b > sb) sb = b;
            src = L->px + ((size_t)v * (size_t)L->w + (size_t)a) * 4;
            o = l->tmp + (size_t)(a - ua) * 4;
#if LY_SSE
            {
                __m128 vw = _mm_set1_ps(w);
                for (i = a; i < b; i++, src += 4, o += 4)
                    _mm_storeu_ps(o, _mm_add_ps(_mm_loadu_ps(o), _mm_mul_ps(vw, px_to_ps(src))));
            }
#else
            for (i = a; i < b; i++, src += 4, o += 4) {
                o[0] += w * (float)src[0];
                o[1] += w * (float)src[1];
                o[2] += w * (float)src[2];
                o[3] += w * (float)src[3];
            }
#endif
        }
        if (sa >= sb) continue;
        for (x = x0; x < x1; x++, d += 4) {
            const taps* t = &cols[x - x0];
#if LY_SSE
            __m128 acc = _mm_setzero_ps(), a3, kp;
            const float* s;
            if (!t->n || t->first >= sb || t->first + t->n <= sa) continue;
            s = l->tmp + (size_t)(t->first - ua) * 4;
            for (k = 0; k < t->n; k++, s += 4)
                acc = _mm_add_ps(acc, _mm_mul_ps(_mm_set1_ps(t->w[k]), _mm_loadu_ps(s)));
            a3 = _mm_shuffle_ps(acc, acc, 0xFF);
            if (_mm_cvtss_f32(a3) <= 0.0f) continue;
            kp = _mm_sub_ps(_mm_set1_ps(1.0f), _mm_div_ps(_mm_mul_ps(a3, _mm_set1_ps(op)), _mm_set1_ps(255.0f)));
            acc = _mm_add_ps(_mm_add_ps(_mm_mul_ps(acc, _mm_set1_ps(op)), _mm_mul_ps(px_to_ps(d), kp)), _mm_set1_ps(0.5f));
            ps_to_px(d, acc);
        }
#else
            float m0 = 0.0f, m1 = 0.0f, m2 = 0.0f, m3 = 0.0f, keep;
            const float* s;
            if (!t->n || t->first >= sb || t->first + t->n <= sa) continue;
            s = l->tmp + (size_t)(t->first - ua) * 4;
            for (k = 0; k < t->n; k++, s += 4) {
                float w = t->w[k];
                m0 += w * s[0];
                m1 += w * s[1];
                m2 += w * s[2];
                m3 += w * s[3];
            }
            if (m3 <= 0.0f) continue;
            /* Premultiplied over, the layer at opacity op. */
            keep = 1.0f - m3 * op / 255.0f;
            m0 = m0 * op + (float)d[0] * keep + 0.5f;
            m1 = m1 * op + (float)d[1] * keep + 0.5f;
            m2 = m2 * op + (float)d[2] * keep + 0.5f;
            m3 = m3 * op + (float)d[3] * keep + 0.5f;
            d[0] = (uint8_t)(m0 >= 255.0f ? 255.0f : m0);
            d[1] = (uint8_t)(m1 >= 255.0f ? 255.0f : m1);
            d[2] = (uint8_t)(m2 >= 255.0f ? 255.0f : m2);
            d[3] = (uint8_t)(m3 >= 255.0f ? 255.0f : m3);
        }
#endif
        memset(l->tmp + (size_t)(sa - ua) * 4, 0, (size_t)(sb - sa) * 4 * sizeof(float));
    }
}

/* ---- the intro and ending pages ------------------------------------------------- */

/* A line of a page's text: bytes [a, a + n), w pixels wide. */
typedef struct ly_tline {
    uint32_t a, n;
    float w;
} ly_tline;

#define LY_TLINES 16

/* A block of a page's text, centred in its width, wrapped between words and at "\n",
 * cut to the lines that fit max_h (one at least). */
typedef struct ly_text {
    fa_font* font;
    float bold, size, shadow, max_h;
    const char* s;
    uint32_t color;
    int italic;
    ly_tline ln[LY_TLINES];
    int n;
    float lh, asc, h;
} ly_text;

static float shape_px(const ly_text* t, uint32_t a, uint32_t n)
{
    return shape_with(t->font, t->bold, t->s + a, n, NULL, NULL) * t->size;
}

static void text_wrap(ly_text* t, float max_w)
{
    const fa_font_info* fi = fa_font_get_info(t->font);
    uint32_t len = (uint32_t)strlen(t->s), p = 0;
    t->n = 0;
    t->lh = (float)(fi->ascender - fi->descender) / (float)fi->units_per_em * t->size;
    t->asc = (float)fi->ascender / (float)fi->units_per_em * t->size;
    if (!(t->lh > 0.0f)) {
        t->lh = 1.2f * t->size;
        t->asc = 0.9f * t->size;
    }
    while (p < len && t->n < LY_TLINES) {
        uint32_t e = p, a = p;
        while (e < len && t->s[e] != '\n' && t->s[e] != '\r') e++;
        for (;;) {
            uint32_t end = 0, q;
            float w = 0.0f;
            while (a < e && t->s[a] == ' ') a++;
            if (a >= e || t->n >= LY_TLINES) break;
            /* The most words from a that fit; a word wider than the width alone. */
            for (q = a; q < e;) {
                uint32_t r = q, wq;
                float ww;
                while (r < e && t->s[r] == ' ') r++;
                if (r >= e) break;            /* only spaces left */
                while (r < e && t->s[r] != ' ') r++;
                wq = r;
                ww = shape_px(t, a, wq - a);
                if (end && ww > max_w) break;
                end = wq;
                w = ww;
                q = r;
            }
            t->ln[t->n].a = a;
            t->ln[t->n].n = end - a;
            t->ln[t->n].w = w;
            t->n++;
            a = end;
        }
        p = e + 1;
    }
    while (t->n > 1 && (float)t->n * t->lh > t->max_h) t->n--;
    t->h = (float)t->n * t->lh;
}

/* A page is drawn into floats, premultiplied B G R A 0..1, then made the layer's
 * picture. */
typedef struct ly_canvas {
    float* px;
    int w, h;
} ly_canvas;

/* A rounded rectangle's coverage at pixel centres: filled, or a `stroke` wide line
 * along its edge; colour 0xAARRGGBB with its alpha times k. */
static void cv_rect(ly_canvas* cv, float x0, float y0, float x1, float y1, float radius, uint32_t argb, float k, float stroke)
{
    float c[4], hw = (x1 - x0) / 2.0f, hh = (y1 - y0) / 2.0f, cx = (x0 + x1) / 2.0f, cy = (y0 + y1) / 2.0f;
    float reach = stroke / 2.0f + 1.0f;
    int x, y, xa = (int)floorf(x0 - reach), xb = (int)ceilf(x1 + reach), ya = (int)floorf(y0 - reach), yb = (int)ceilf(y1 + reach);
    if (hw <= 0.0f || hh <= 0.0f) return;
    if (radius > hw) radius = hw;
    if (radius > hh) radius = hh;
    if (radius < 0.0f) radius = 0.0f;
    if (xa < 0) xa = 0;
    if (ya < 0) ya = 0;
    if (xb > cv->w) xb = cv->w;
    if (yb > cv->h) yb = cv->h;
    color_mix(argb, argb, 0.0f, k, c);
    if (radius <= 0.0f && stroke <= 0.0f) {
        /* Square corners: each pixel's exact area, row times column. */
        for (y = ya; y < yb; y++) {
            float ry = fminf((float)y + 1.0f, y1) - fmaxf((float)y, y0);   /* at most 1, as rx */
            if (ry <= 0.0f) continue;
            for (x = xa; x < xb; x++) {
                float rx = fminf((float)x + 1.0f, x1) - fmaxf((float)x, x0);
                if (rx > 0.0f) over(cv->px + ((size_t)y * (size_t)cv->w + (size_t)x) * 4, c, rx * ry);
            }
        }
        return;
    }
    for (y = ya; y < yb; y++)
        for (x = xa; x < xb; x++) {
            float qx = fabsf((float)x + 0.5f - cx) - (hw - radius), qy = fabsf((float)y + 0.5f - cy) - (hh - radius);
            float ox = qx > 0.0f ? qx : 0.0f, oy = qy > 0.0f ? qy : 0.0f;
            float d = sqrtf(ox * ox + oy * oy) + (qx > qy ? (qx < 0.0f ? qx : 0.0f) : (qy < 0.0f ? qy : 0.0f)) - radius;
            float a = stroke > 0.0f ? stroke / 2.0f + 0.5f - fabsf(d) : 0.5f - d;
            if (a > 0.0f) over(cv->px + ((size_t)y * (size_t)cv->w + (size_t)x) * 4, c, a > 1.0f ? 1.0f : a);
        }
}

/* One line of text with its baseline at y, from x: a shadow (when `shadow` > 0, that
 * many pixels down and right, black at 82 %), then the text, each rasterized over the
 * line's own box. 0 when out of memory. */
static int cv_line(fa_lyric* l, ly_canvas* cv, const ly_text* t, uint32_t a, uint32_t n, float x, float y, float w)
{
    const fa_font_info* fi = fa_font_get_info(t->font);
    float gs = t->size / (float)fi->units_per_em, grow = t->bold * t->size;
    float* xs = (float*)malloc(((size_t)n + 1) * sizeof(float));
    uint32_t* g = (uint32_t*)malloc(((size_t)n + 1) * sizeof(uint32_t));
    int pass, ok = xs && g;
    /* Ink can reach past the line's box by the bold, the slant and a glyph's overhang. */
    int m = 2 + (int)ceilf(grow + 0.25f * t->size + (t->italic ? 0.2f * t->asc : 0.0f));
    if (ok) shape_with(t->font, t->bold, t->s + a, n, xs, g);
    for (pass = 0; pass < 2 && ok; pass++) {
        float c[4], d = pass ? 0.0f : t->shadow;
        int x0 = (int)floorf(x + d) - m, x1 = (int)ceilf(x + d + w) + m;
        int y0 = (int)floorf(y + d - t->asc) - m, y1 = (int)ceilf(y + d - t->asc + t->lh) + m, bw, bh, i, j;
        uint32_t k;
        uint8_t* cov;
        if (!pass && t->shadow <= 0.0f) continue;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > cv->w) x1 = cv->w;
        if (y1 > cv->h) y1 = cv->h;
        bw = x1 - x0;
        bh = y1 - y0;
        if (bw <= 0 || bh <= 0) continue;
        color_mix(pass ? t->color : 0xD0000000u, pass ? t->color : 0xD0000000u, 0.0f, 1.0f, c);
        if (!raster_shape(l, bw, bh, &cov)) {
            ok = 0;
            break;
        }
        for (k = 0; k < n; k++) {
            size_t q;
            if (g[k] == UINT32_MAX || t->s[a + k] == ' ') continue;
            if (fa_font_outline(t->font, g[k], &l->gpath) != FA_OK || !l->gpath.count) continue;
            if (t->italic)
                for (q = 0; q < l->gpath.count; q++) {
                    fa_path_cmd* pc = &l->gpath.cmd[q];
                    pc->x += 0.2f * pc->y;
                    pc->x1 += 0.2f * pc->y1;
                    pc->x2 += 0.2f * pc->y2;
                }
            fa_raster_path(&l->ras, &l->gpath, gs, -gs, x + xs[k] * t->size + grow + d - (float)x0, y + d - (float)y0);
        }
        fa_raster_end(&l->ras, cov, (size_t)bw);
        if (grow > 0.0f) {
            uint8_t* b = (uint8_t*)malloc((size_t)bw * (size_t)bh);
            if (!b || !fa_fx_dilate(cov, (size_t)bw, bw, bh, grow, b, (size_t)bw)) {
                free(b);
                free(cov);
                ok = 0;
                break;
            }
            free(cov);
            cov = b;
        }
        for (j = 0; j < bh; j++)
            for (i = 0; i < bw; i++) {
                uint8_t v = cov[(size_t)j * (size_t)bw + (size_t)i];
                if (v) over(cv->px + ((size_t)(y0 + j) * (size_t)cv->w + (size_t)(x0 + i)) * 4, c, (float)v / 255.0f);
            }
        free(cov);
    }
    free(xs);
    free(g);
    return ok;
}

/* A block of text from `top`, each line centred in [left, left + width). */
static int cv_text(fa_lyric* l, ly_canvas* cv, const ly_text* t, float left, float width, float top)
{
    int i;
    for (i = 0; i < t->n; i++)
        if (!cv_line(l, cv, t, t->ln[i].a, t->ln[i].n, left + (width - t->ln[i].w) / 2.0f, top + (float)i * t->lh + t->asc,
                     t->ln[i].w))
            return 0;
    return 1;
}

static void text_init(ly_text* t, fa_font* font, float bold, float size, const char* s, uint32_t color, float shadow,
                      float max_h)
{
    memset(t, 0, sizeof(*t));
    t->font = font;
    t->bold = bold;
    t->size = size;
    t->s = s;
    t->color = color;
    t->shadow = shadow;
    t->max_h = max_h;
}

/* Appends to a string of `cap` bytes, cutting what does not fit. */
static void cat(char* d, size_t cap, const char* s)
{
    size_t n = strlen(d), m = strlen(s);
    if (n + 1 >= cap) return;
    if (m > cap - 1 - n) {
        m = cap - 1 - n;
        while (m && ((unsigned char)s[m] & 0xC0) == 0x80) m--;   /* not inside a character */
    }
    memcpy(d + n, s, m);
    d[n + m] = 0;
}

#define LY_DOT "  \xE2\x80\xA2  "

/* Joins non-empty parts with `sep`. */
static void cat_part(char* d, size_t cap, const char* sep, const char* label, const char* value, int* any)
{
    if (!value[0]) return;
    if (*any) cat(d, cap, sep);
    if (label) {
        cat(d, cap, label);
        cat(d, cap, "  ");
    }
    cat(d, cap, value);
    *any = 1;
}

static int same_ci(const char* a, const char* b)
{
    for (; *a && *b; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return *a == *b;
}

/* "Length  3:25" (or 1:02:03), "" when the length is unknown. */
static void length_text(const fa_lyric* l, char* d, size_t cap)
{
    long long s;
    char num[32];
    d[0] = 0;
    if (l->duration <= 0) return;
    s = (long long)((l->duration + 500000000) / 1000000000);
    if (s >= 3600) snprintf(num, sizeof(num), "%lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60);
    else snprintf(num, sizeof(num), "%lld:%02lld", s / 60, s % 60);
    cat(d, cap, l->label[0]);
    cat(d, cap, "  ");
    cat(d, cap, num);
}

/* "Key  E minor  •  Tempo  120 bpm  •  Timing  4/4" */
static void details_text(const fa_lyric* l, char* d, size_t cap)
{
    int any = 0;
    d[0] = 0;
    cat_part(d, cap, LY_DOT, l->label[1], l->cr.key, &any);
    cat_part(d, cap, LY_DOT, l->label[2], l->cr.tempo, &any);
    cat_part(d, cap, LY_DOT, l->label[3], l->cr.time_signature, &any);
}

static void line_add(char* d, size_t cap, const char* s)
{
    if (!s[0]) return;
    if (d[0]) cat(d, cap, "\n");
    cat(d, cap, s);
}

/* The card's information lines: the writers, the year, the length, the key, tempo
 * and time signature, the song's other notes (the ending card: the year alone). */
static void info_text(const fa_lyric* l, int ending, char* d, size_t cap)
{
    char tmp[1024];
    const fa_midi_credits* c = &l->cr;
    d[0] = 0;
    if (!ending && l->named) {
        tmp[0] = 0;
        if (c->writer[0] && c->lyricist[0] && same_ci(c->writer, c->lyricist)) {
            cat(tmp, sizeof(tmp), l->label[4]);
            cat(tmp, sizeof(tmp), " / ");
            cat(tmp, sizeof(tmp), l->label[5]);
            cat(tmp, sizeof(tmp), ":  ");
            cat(tmp, sizeof(tmp), c->writer);
        } else {
            int any = 0;
            char lab[64];
            if (c->writer[0]) {
                lab[0] = 0;
                cat(lab, sizeof(lab) - 1, l->label[4]);
                cat(lab, sizeof(lab), ":");
                cat_part(tmp, sizeof(tmp), LY_DOT, lab, c->writer, &any);
            }
            if (c->lyricist[0]) {
                lab[0] = 0;
                cat(lab, sizeof(lab) - 1, l->label[5]);
                cat(lab, sizeof(lab), ":");
                cat_part(tmp, sizeof(tmp), LY_DOT, lab, c->lyricist, &any);
            }
        }
        line_add(d, cap, tmp);
    }
    line_add(d, cap, c->year);
    if (ending) return;
    length_text(l, tmp, sizeof(tmp));
    line_add(d, cap, tmp);
    details_text(l, tmp, sizeof(tmp));
    line_add(d, cap, tmp);
    if (l->named && c->other[0]) {
        /* Its lines joined with dots. */
        const char* p = c->other;
        int any = 0;
        tmp[0] = 0;
        while (*p) {
            char one[256];
            size_t n = strcspn(p, "\n");
            if (n >= sizeof(one)) {
                n = sizeof(one) - 1;
                while (n && ((unsigned char)p[n] & 0xC0) == 0x80) n--;
            }
            memcpy(one, p, n);
            one[n] = 0;
            cat_part(tmp, sizeof(tmp), LY_DOT, NULL, one, &any);
            p += strcspn(p, "\n");
            if (*p) p++;
        }
        line_add(d, cap, tmp);
    }
}

/* The canvas made the layer's picture, with its rows' spans. */
static int canvas_take(ly_layer* L, ly_canvas* cv)
{
    size_t i, px = (size_t)cv->w * (size_t)cv->h;
    int x, y;
    L->w = cv->w;
    L->h = cv->h;
    L->px = (uint8_t*)malloc(px * 4);
    L->span = (int*)malloc((size_t)cv->h * 2 * sizeof(int));
    if (!L->px || !L->span) return 0;
    for (i = 0; i < px * 4; i++) {
        float v = cv->px[i] * 255.0f + 0.5f;
        L->px[i] = (uint8_t)(v >= 255.0f ? 255.0f : v <= 0.0f ? 0.0f : v);
    }
    for (y = 0; y < cv->h; y++) {
        const uint8_t* r = L->px + (size_t)y * (size_t)cv->w * 4;
        int a = cv->w, b = 0;
        for (x = 0; x < cv->w; x++)
            if (r[x * 4 + 3]) {
                if (x < a) a = x;
                b = x + 1;
            }
        L->span[2 * y] = a < b ? a : 0;
        L->span[2 * y + 1] = b;
    }
    return 1;
}

/* Glass: a fill, a sheen over its top, a border line. */
static void glass(const fa_lyric* l, ly_canvas* cv, float x0, float y0, float x1, float y1, float r, float k, float fill_k)
{
    float in = 3.0f * k;
    cv_rect(cv, x0, y0, x1, y1, r, l->st.card_color, fill_k, 0.0f);
    cv_rect(cv, x0 + in, y0 + in, x1 - in, y0 + in + 0.42f * (y1 - y0), r - in > 1.0f ? r - in : 1.0f, 0x1F335770u, 1.0f, 0.0f);
}

/* The intro's or the ending's card, in the middle of the area. */
static int card_build(fa_lyric* l, ly_layer* L, int ending, float* at_x, float* at_y)
{
    float W = (float)l->area_w, H = (float)l->area_h, k = H / (float)LY_DESIGN_H, F = l->em;
    int fb = !l->named, ok, m = 2;
    fa_font* f2 = l->font2 ? l->font2 : l->font;
    float b2 = l->font2 ? l->bold2 : 0.0f;
    float boxw = W * (fb ? 0.5f : 0.72f), tw = boxw - 0.9f * F, pad = 0.45f * F, r = 0.22f * F, boxh, y;
    ly_text t[4];
    float gap[4];                 /* above each block */
    char info[2048];
    int i, n = 0;
    ly_canvas cv;
    info_text(l, ending, info, sizeof(info));
    gap[n] = 0.0f;
    text_init(&t[n++], l->font, l->bold, F * (fb ? 0.65f : 0.92f), fb ? l->fallback : l->cr.title, l->st.title_color, 2.2f * k,
              0.15f * H);
    if (l->cr.artist[0]) {
        gap[n] = 0.13f * F;
        text_init(&t[n++], l->font, l->bold, 0.55f * F, l->cr.artist, l->st.card_accent, 1.5f * k, 0.1f * H);
    }
    if (info[0]) {
        gap[n] = 0.2f * F;
        text_init(&t[n++], f2, b2, 0.31f * F, info, l->st.info_color, 1.0f * k, 0.24f * H);
    }
    if (l->cr.copyright[0]) {
        gap[n] = 0.2f * F;
        text_init(&t[n], f2, b2, 0.23f * F, l->cr.copyright, l->st.copyright_color, 1.0f * k, 0.08f * H);
        t[n++].italic = 1;
    }
    boxh = 2.0f * pad;
    for (i = 0; i < n; i++) {
        text_wrap(&t[i], tw);
        boxh += gap[i] + t[i].h;
    }
    cv.w = (int)ceilf(boxw + 8.0f * k) + 2 * m;
    cv.h = (int)ceilf(boxh + 10.0f * k) + 2 * m;
    if (!(boxh <= 2.0f * H)) return 0;   /* a damaged font's metrics: no card of gigabytes */
    cv.px = (float*)calloc((size_t)cv.w * (size_t)cv.h * 4, sizeof(float));
    if (!cv.px) return 0;
    cv_rect(&cv, (float)m + 8.0f * k, (float)m + 10.0f * k, (float)m + 8.0f * k + boxw, (float)m + 10.0f * k + boxh, r, 0xFF000000u,
            0.42f, 0.0f);
    glass(l, &cv, (float)m, (float)m, (float)m + boxw, (float)m + boxh, r, k, 1.0f);
    cv_rect(&cv, (float)m, (float)m, (float)m + boxw, (float)m + boxh, r, l->st.card_accent, 0.5f, 1.5f * k);
    y = (float)m + pad;
    ok = 1;
    for (i = 0; i < n && ok; i++) {
        y += gap[i];
        ok = cv_text(l, &cv, &t[i], (float)m + (boxw - tw) / 2.0f, tw, y);
        y += t[i].h;
    }
    ok = ok && canvas_take(L, &cv);
    free(cv.px);
    *at_x = floorf(W / 2.0f - boxw / 2.0f + 0.5f) - (float)m - (float)l->bx;
    *at_y = floorf(H / 2.0f - boxh / 2.0f + 0.5f) - (float)m - (float)l->by;
    return ok;
}

/* The intro's band at the top (title, then artist and year) or the bottom (the
 * length, key, tempo and time signature, the copyright). */
static int band_build(fa_lyric* l, ly_layer* L, int bottom, float* at_x, float* at_y)
{
    float W = (float)l->area_w, H = (float)l->area_h, k = H / (float)LY_DESIGN_H, F = l->em, bh, tw = 0.88f * W, line = 1.5f * k;
    int fb = !l->named, ok = 1;
    fa_font* f2 = l->font2 ? l->font2 : l->font;
    float b2 = l->font2 ? l->bold2 : 0.0f;
    char a[1024], b[1024];
    ly_text t;
    ly_canvas cv;
    bh = ceilf(H * (bottom ? 0.16f : 0.17f));
    cv.w = (int)W;
    cv.h = (int)bh;
    cv.px = (float*)calloc((size_t)cv.w * (size_t)cv.h * 4, sizeof(float));
    if (!cv.px) return 0;
    /* The bands are a little clearer than the card: 0.78 / 0.88 of its alpha. */
    cv_rect(&cv, 0.0f, 0.0f, W, bh, 0.0f, l->st.card_color, 0.886f, 0.0f);
    cv_rect(&cv, 0.0f, 0.0f, W, 0.45f * bh, 0.0f, 0x291F3345u, 1.0f, 0.0f);
    if (bottom) cv_rect(&cv, 0.0f, 0.0f, W, line, 0.0f, l->st.card_accent, 0.45f, 0.0f);
    else cv_rect(&cv, 0.0f, bh - 1.0f - line / 2.0f, W, bh - 1.0f + line / 2.0f, 0.0f, l->st.card_accent, 0.45f, 0.0f);
    if (!bottom) {
        float y = 0.12f * bh;
        int any = 0;
        text_init(&t, l->font, l->bold, F * (fb ? 0.52f : 0.68f), fb ? l->fallback : l->cr.title, l->st.title_color, 1.8f * k,
                  0.55f * bh);
        text_wrap(&t, tw);
        ok = cv_text(l, &cv, &t, (W - tw) / 2.0f, tw, y);
        y += t.h;
        a[0] = 0;
        cat_part(a, sizeof(a), LY_DOT, NULL, l->cr.artist, &any);
        cat_part(a, sizeof(a), LY_DOT, NULL, l->cr.year, &any);
        if (ok && a[0]) {
            text_init(&t, l->font, l->bold, 0.36f * F, a, l->st.card_accent, 1.2f * k, bh - y);
            text_wrap(&t, tw);
            ok = cv_text(l, &cv, &t, (W - tw) / 2.0f, tw, y);
        }
    } else {
        a[0] = 0;
        length_text(l, b, sizeof(b));
        line_add(a, sizeof(a), b);
        details_text(l, b, sizeof(b));
        line_add(a, sizeof(a), b);
        line_add(a, sizeof(a), l->cr.copyright);
        if (a[0]) {
            text_init(&t, f2, b2, 0.27f * F, a, l->st.info_color, 1.0f * k, 0.86f * bh);
            text_wrap(&t, tw);
            ok = cv_text(l, &cv, &t, (W - tw) / 2.0f, tw, 0.14f * bh);
        }
    }
    ok = ok && canvas_take(L, &cv);
    free(cv.px);
    *at_x = -(float)l->bx;
    *at_y = (bottom ? H - bh : 0.0f) - (float)l->by;
    return ok;
}

static ly_layer* page_get(fa_lyric* l, int k)
{
    ly_layer* L = &l->pg[k];
    int ok;
    if (L->px) return L;
    if (l->pg_failed[k]) return NULL;
    layer_clear(L);
    ok = k == PG_TOP || k == PG_BOTTOM ? band_build(l, L, k == PG_BOTTOM, &l->pg_x[k], &l->pg_y[k])
                                       : card_build(l, L, k == PG_END, &l->pg_x[k], &l->pg_y[k]);
    if (!ok) {
        layer_clear(L);
        l->pg_failed[k] = 1;
        return NULL;
    }
    L->line = k;
    return L;
}

/* ---- frames --------------------------------------------------------------------- */

static void box_of(const fa_lyric* l, const ly_layer* L, float cx, float cy, float sc, int out[4])
{
    out[0] = (int)floorf(cx - L->cx * sc) - 2;
    out[1] = (int)floorf(cy - L->cy * sc) - 2;
    out[2] = (int)ceilf(cx + ((float)L->w - L->cx) * sc) + 2;
    out[3] = (int)ceilf(cy + ((float)L->h - L->cy) * sc) + 2;
    if (out[0] < 0) out[0] = 0;
    if (out[1] < 0) out[1] = 0;
    if (out[2] > l->bw) out[2] = l->bw;
    if (out[3] > l->bh) out[3] = l->bh;
}

/* Arrow i's centre in bitmap pixels. */
static void arrow_at(const fa_lyric* l, const ly_inst* in, int i, float* x, float* y)
{
    const ly_layer* L = in->layer;
    float ax = L->text_x + (float)i * (l->tri_w + l->arrow_gap) + l->tri_w / 2.0f;
    float ay = L->text_y - l->arrow_lift - l->tri / 2.0f;
    *x = in->cx + (ax - L->cx) * in->sc;
    *y = in->cy + (ay - L->cy) * in->sc;
}

/* Page k at opacity op into *in: 1, or 0 when it could not be drawn (the lyrics go
 * on without it). */
static int page_inst(fa_lyric* l, int k, float op, ly_inst* in)
{
    ly_layer* L = page_get(l, k);
    if (!L) return 0;
    memset(in, 0, sizeof(*in));
    in->line = LY_PAGE_LINE + (uint32_t)k;
    in->page = k;
    in->layer = L;
    in->sc = 1.0f;
    in->op = op;
    in->cx = l->pg_x[k];
    in->cy = l->pg_y[k];
    box_of(l, L, in->cx, in->cy, 1.0f, in->box);
    return 1;
}

/* What shows at t, in drawing order: the intro's bands behind the lines, its card
 * and the ending card over them. */
static int frame_list(fa_lyric* l, fa_time t, ly_inst* out)
{
    ly_show sh[LY_MAX_SHOW];
    uint32_t n = ly_frame(&l->tl, &l->prm, t, sh), i, m = 0;
    float intro = ly_intro_at(&l->tl, &l->prm, t), ending = ly_ending_at(&l->tl, &l->prm, t);
    if (intro > 0.0f && l->tl.intro_bands) {
        m += (uint32_t)page_inst(l, PG_TOP, intro, &out[m]);
        m += (uint32_t)page_inst(l, PG_BOTTOM, intro, &out[m]);
    }
    for (i = 0; i < n; i++) {
        ly_inst* in = &out[m];
        ly_layer* L = layer_get(l, sh[i].line);
        float maxw = (float)l->block_w;
        if (!L) continue;   /* could not be drawn (failed): left out, as a page is */
        memset(in, 0, sizeof(*in));
        in->page = -1;
        in->line = sh[i].line;
        in->outgoing = sh[i].outgoing;
        in->layer = L;
        in->sc = sh[i].scale;
        if ((float)L->w * in->sc > maxw) in->sc = maxw / (float)L->w;   /* a big page-in still fits */
        in->op = sh[i].opacity;
        in->cx = (float)l->area_w / 2.0f - (float)l->bx;
        in->cy = l->anchor + sh[i].slot * l->pitch - (float)l->by;
        ly_wipe_at(&l->tl.line[in->line], &l->prm, t, &in->wipe);
        ly_countdown_at(&l->tl.line[in->line], &l->prm, t, &in->cd);
        box_of(l, L, in->cx, in->cy, in->sc, in->box);
        if (in->cd.shown) {
            int a;
            float ax, ay, hw = ((float)l->tri_w / 2.0f + (float)l->pad) * in->sc + 2.0f;
            float hh = ((float)l->tri / 2.0f + (float)l->pad) * in->sc + 2.0f;
            in->abox[0] = l->bw;
            in->abox[1] = l->bh;
            in->abox[2] = in->abox[3] = 0;
            for (a = 0; a < l->st.countdown_count; a++) {
                arrow_at(l, in, a, &ax, &ay);
                if ((int)floorf(ax - hw) < in->abox[0]) in->abox[0] = (int)floorf(ax - hw);
                if ((int)floorf(ay - hh) < in->abox[1]) in->abox[1] = (int)floorf(ay - hh);
                if ((int)ceilf(ax + hw) > in->abox[2]) in->abox[2] = (int)ceilf(ax + hw);
                if ((int)ceilf(ay + hh) > in->abox[3]) in->abox[3] = (int)ceilf(ay + hh);
            }
            if (in->abox[0] < 0) in->abox[0] = 0;
            if (in->abox[1] < 0) in->abox[1] = 0;
            if (in->abox[2] > l->bw) in->abox[2] = l->bw;
            if (in->abox[3] > l->bh) in->abox[3] = l->bh;
        }
        m++;
    }
    if (intro > 0.0f && !l->tl.intro_bands) m += (uint32_t)page_inst(l, PG_CARD, intro, &out[m]);
    if (ending > 0.0f) m += (uint32_t)page_inst(l, PG_END, ending, &out[m]);
    return (int)m;
}

/* What changed between two frames, as bitmap rectangles. */
static void diff(const fa_lyric* l, const ly_inst* a, int na, const ly_inst* b, int nb, fa_rects* out)
{
    int i, j;
    for (i = 0; i < na; i++) {
        const ly_inst* o = &a[i];
        const ly_inst* n = NULL;
        for (j = 0; j < nb; j++)
            if (b[j].line == o->line && b[j].outgoing == o->outgoing) n = &b[j];
        if (!n || !same(o->cx, n->cx) || !same(o->cy, n->cy) || !same(o->sc, n->sc) || !same(o->op, n->op) || o->layer != n->layer) {
            fa_rects_add(out, o->box[0], o->box[1], o->box[2], o->box[3]);
            if (o->cd.shown) fa_rects_add(out, o->abox[0], o->abox[1], o->abox[2], o->abox[3]);
            if (n) {
                fa_rects_add(out, n->box[0], n->box[1], n->box[2], n->box[3]);
                if (n->cd.shown) fa_rects_add(out, n->abox[0], n->abox[1], n->abox[2], n->abox[3]);
            }
            continue;
        }
        if (o->page >= 0) continue;
        /* In place: the syllables whose colour changed, and the arrows. */
        {
            const ly_line* line = &l->tl.line[o->line];
            int c0, c1;
            if (changed_columns(l, n->layer, line, &o->wipe, &n->wipe, &c0, &c1)) {
                /* The columns whose colour changed, and the output pixels whose filter
                 * taps reach them (rad source columns either side). */
                const ly_layer* L = n->layer;
                float rad = n->sc < 1.0f ? 1.0f / n->sc : 1.0f;
                int x0 = (int)floorf(n->cx + ((float)c0 - rad - L->cx) * n->sc) - 2;
                int x1 = (int)ceilf(n->cx + ((float)c1 + rad - L->cx) * n->sc) + 2;
                if (x0 < n->box[0]) x0 = n->box[0];
                if (x1 > n->box[2]) x1 = n->box[2];
                if (x0 < x1) fa_rects_add(out, x0, n->box[1], x1, n->box[3]);
            }
            if (o->cd.shown != n->cd.shown || o->cd.done != n->cd.done || !same(o->cd.progress, n->cd.progress) ||
                !same(o->cd.opacity, n->cd.opacity)) {
                if (o->cd.shown) fa_rects_add(out, o->abox[0], o->abox[1], o->abox[2], o->abox[3]);
                if (n->cd.shown) fa_rects_add(out, n->abox[0], n->abox[1], n->abox[2], n->abox[3]);
            }
        }
    }
    for (j = 0; j < nb; j++) {
        int found = 0;
        for (i = 0; i < na && !found; i++) found = a[i].line == b[j].line && a[i].outgoing == b[j].outgoing;
        if (found) continue;
        fa_rects_add(out, b[j].box[0], b[j].box[1], b[j].box[2], b[j].box[3]);
        if (b[j].cd.shown) fa_rects_add(out, b[j].abox[0], b[j].abox[1], b[j].abox[2], b[j].abox[3]);
    }
}

static void draw_rect(fa_lyric* l, const ly_inst* list, int n, const int r[4])
{
    int y, i, a;
    for (y = r[1]; y < r[3]; y++) memset(l->master + ((size_t)y * (size_t)l->bw + (size_t)r[0]) * 4, 0, (size_t)(r[2] - r[0]) * 4);
    for (i = 0; i < n; i++) {
        const ly_inst* in = &list[i];
        if (in->page >= 0) {
            if (in->box[0] < r[2] && r[0] < in->box[2] && in->box[1] < r[3] && r[1] < in->box[3])
                draw_place(l, in->layer, in->cx, in->cy, 1.0f, in->op, r[0], r[1], r[2], r[3]);
            continue;
        }
        if (in->box[0] < r[2] && r[0] < in->box[2] && in->box[1] < r[3] && r[1] < in->box[3] &&
            color_line(l, in->layer, &l->tl.line[in->line], &in->wipe))
            draw_place(l, in->layer, in->cx, in->cy, in->sc, in->op, r[0], r[1], r[2], r[3]);
        if (in->cd.shown && in->abox[0] < r[2] && r[0] < in->abox[2] && in->abox[1] < r[3] && r[1] < in->abox[3] &&
            arrow_make(l)) {
            for (a = 0; a < l->st.countdown_count; a++) {
                float ax, ay, t = a < in->cd.done ? 1.0f : a == in->cd.done ? in->cd.progress : 0.0f;
                /* The arrow's colour, red to green, lerped straight. */
                uint32_t c = 0;
                int sh;
                for (sh = 0; sh < 32; sh += 8) {
                    float c0 = (float)((l->st.countdown_from >> sh) & 0xFF), c1 = (float)((l->st.countdown_to >> sh) & 0xFF);
                    c |= (uint32_t)(c0 + (c1 - c0) * t + 0.5f) << sh;
                }
                color_columns(l, &l->arrow, NULL, NULL, c, 0, l->arrow.w);
                arrow_at(l, in, a, &ax, &ay);
                draw_place(l, &l->arrow, ax, ay, in->sc, in->op * in->cd.opacity, r[0], r[1], r[2], r[3]);
            }
        }
    }
}

static void to_rect(const int r[4], fa_rect* out)
{
    out->x = r[0];
    out->y = r[1];
    out->width = (uint32_t)(r[2] - r[0]);
    out->height = (uint32_t)(r[3] - r[1]);
}

/* Brings the master to `t`: the frame's revision goes up when anything changed. */
static fa_result render(fa_lyric* l, fa_time t)
{
    ly_inst now[LY_MAX_INST];
    fa_rects dirty;
    int n = frame_list(l, t, now), i;
    if (n < 0) return FA_ERROR_OUT_OF_MEMORY;
    fa_rects_clear(&dirty);
    if (l->full) {
        fa_rects_add(&dirty, 0, 0, l->bw, l->bh);
        l->full = 0;
    } else {
        diff(l, l->prev, (int)l->nprev, now, n, &dirty);
    }
    for (i = 0; i < dirty.n; i++) draw_rect(l, now, n, dirty.r[i]);
    memcpy(l->prev, now, (size_t)n * sizeof(ly_inst));
    l->nprev = (uint32_t)n;
    if (dirty.n) {
        l->revision++;
        fa_history_push(&l->hist, l->revision, &dirty);
    }
    return FA_OK;
}

/* ---- the surface ---------------------------------------------------------------- */

/* The labels, one per line, over the English ones. */
static void labels_split(fa_lyric* l)
{
    static const char* const english[6] = { "Length", "Key", "Tempo", "Timing", "Written By", "Lyricist" };
    char* p = l->label_buf;
    int i;
    for (i = 0; i < 6; i++) {
        char* e;
        l->label[i] = english[i];
        if (!p) continue;
        e = strchr(p, '\n');
        if (e) *e = 0;
        if (*p && p[strlen(p) - 1] == '\r') p[strlen(p) - 1] = 0;
        if (*p) l->label[i] = p;
        p = e ? e + 1 : NULL;
    }
}

static int same_str(const char* a, const char* b) { return a == b || (a && b && strcmp(a, b) == 0); }

/* The style asks for the font already open: only colours or placement changed. */
static int same_font(const fa_lyric* l, const fa_lyric_style* st)
{
    const fa_lyric_style* o = &l->st;
    if (!l->font || st->font_face != o->font_face || st->font_explicit != o->font_explicit ||
        st->font_weight != o->font_weight || !same_str(st->font_family, o->font_family) ||
        !same_str(st->font_path, o->font_path) || !st->font_data != !o->font_data)
        return 0;
    return !st->font_data || (st->font_data_size == o->font_data_size &&
                              (st->font_data == o->font_data || memcmp(st->font_data, o->font_data, st->font_data_size) == 0));
}

static fa_result style_take(fa_lyric* l, const fa_lyric_style* st)
{
    char* family = fa_strdup(st->font_family);
    char* path = fa_strdup(st->font_path);
    char* fallback = fa_strdup(st->fallback_title);
    char* labels = fa_strdup(st->labels);
    char* label_buf = fa_strdup(st->labels);
    void* data = NULL;
    fa_font *font = NULL, *font2 = NULL;
    float bold = 0.0f, bold2 = 0.0f;
    fa_result r = FA_OK;
    int keep = same_font(l, st);
    if ((st->font_family && !family) || (st->font_path && !path) || (st->fallback_title && !fallback) ||
        (st->labels && (!labels || !label_buf)))
        r = FA_ERROR_OUT_OF_MEMORY;
    if (keep) {
        font = l->font;
        font2 = l->font2;
        bold = l->bold;
        bold2 = l->bold2;
        data = l->data;
    } else if (r == FA_OK && st->font_data) {
        data = malloc(st->font_data_size);
        if (data) memcpy(data, st->font_data, st->font_data_size);
        else r = FA_ERROR_OUT_OF_MEMORY;
    }
    if (r == FA_OK && !keep) r = font_open(l, st, data, &font, &bold);
    if (r != FA_OK) {
        free(family);
        free(path);
        free(fallback);
        free(labels);
        free(label_buf);
        if (!keep) free(data);
        return r;
    }
    if (!keep && !st->font_data && !st->font_path && fa_font_get_info(font)->weight > 500) {
        /* The pages' smaller text in the family's regular weight (none: the font). */
        fa_lyric_style reg = *st;
        reg.font_weight = 400;
        if (font_open(l, &reg, NULL, &font2, &bold2) != FA_OK) font2 = NULL;
    }
    if (!keep) {
        if (l->font) fa_font_close(l->font);
        if (l->font2) fa_font_close(l->font2);
        free(l->data);
    }
    free(l->family);
    free(l->path);
    free(l->fallback);
    free(l->labels);
    free(l->label_buf);
    l->font = font;
    l->bold = bold;
    l->font2 = font2;
    l->bold2 = bold2;
    l->family = family;
    l->path = path;
    l->data = data;
    l->fallback = fallback;
    l->labels = labels;
    l->label_buf = label_buf;
    labels_split(l);
    l->st = *st;
    l->st.struct_size = (uint32_t)sizeof(l->st);
    l->st.font_family = family;
    l->st.font_path = path;
    l->st.font_data = data;
    l->st.fallback_title = fallback;
    l->st.labels = labels;
    {
        const char* name = fa_font_get_info(font)->family;
        size_t n = strlen(name);
        if (n >= sizeof(l->font_used)) n = sizeof(l->font_used) - 1;
        memcpy(l->font_used, name, n);
        l->font_used[n] = 0;
    }
    invalidate(l, 1);
    return FA_OK;
}

fa_result fa_lyric_new(fa_stream* stream, const fa_lyric_style* style, fa_lyric** out)
{
    fa_lyric* l;
    fa_result r;
    int i;
    *out = NULL;
    l = (fa_lyric*)calloc(1, sizeof(*l));
    if (!l) return FA_ERROR_OUT_OF_MEMORY;
    for (i = 0; i < LY_CACHE; i++) l->cache[i].line = -1;
    l->arrow.line = -1;
    for (i = 0; i < LY_PAGES; i++) l->pg[i].line = -1;
    l->tempo = 1.0;
    r = read_song(l, stream);
    if (r == FA_OK) r = style_take(l, style);
    if (r != FA_OK) {
        fa_lyric_free(l);
        return r;
    }
    *out = l;
    return FA_OK;
}

void fa_lyric_free(fa_lyric* l)
{
    uint32_t i;
    if (!l) return;
    cache_clear(l);
    ly_free(&l->tl);
    for (i = 0; i < l->nsong; i++) free(l->song[i].text);
    free(l->song);
    free(l->tempos);
    if (l->font) fa_font_close(l->font);
    if (l->font2) fa_font_close(l->font2);
    free(l->family);
    free(l->path);
    free(l->data);
    free(l->fallback);
    free(l->labels);
    free(l->label_buf);
    free(l->scratch);
    free(l->master);
    free(l->tmp);
    free(l->cols);
    fa_path_free(&l->gpath);
    fa_raster_free(&l->ras);
    free(l);
}

fa_result fa_lyric_set_style(fa_lyric* l, const fa_lyric_style* st) { return style_take(l, st); }

void fa_lyric_get_style(const fa_lyric* l, fa_lyric_style* out) { *out = l->st; }

void fa_lyric_set_effects(fa_lyric* l, const fa_surface_effects* e)
{
    if (e) {
        l->st.effects = *e;
    } else {
        fa_surface_effects_init(&l->st.effects);
    }
    invalidate(l, 0);
}

uint64_t fa_lyric_layout_revision(const fa_lyric* l) { return l->layout_revision; }

int fa_lyric_ready_for(const fa_lyric* l, uint32_t aw, uint32_t ah)
{
    if (!aw || !ah) {
        aw = LY_DESIGN_W;
        ah = LY_DESIGN_H;
    }
    return l->laid && l->built && l->area_w == aw && l->area_h == ah;
}

const fa_surface_history* fa_lyric_history(const fa_lyric* l) { return &l->hist; }

fa_result fa_lyric_get_layout(fa_lyric* l, uint32_t aw, uint32_t ah, fa_surface_layout* o)
{
    fa_result r = layout(l, aw, ah, l->tempo);
    if (r != FA_OK) return r;
    o->width = (uint32_t)l->bw;
    o->height = (uint32_t)l->bh;
    o->bitmap_x = l->bx;
    o->bitmap_y = l->by;
    o->content_x = (uint32_t)l->content[0];
    o->content_y = (uint32_t)l->content[1];
    o->content_width = (uint32_t)l->content[2];
    o->content_height = (uint32_t)l->content[3];
    o->scale = (float)l->area_h / (float)LY_DESIGN_H;
    memcpy(o->font_used, l->font_used, sizeof(o->font_used));
    o->padding = (uint32_t)l->pad;
    o->layout_revision = l->layout_revision;
    return FA_OK;
}

fa_result fa_lyric_update(fa_lyric* l, uint32_t aw, uint32_t ah, double tempo, fa_time time, uint8_t* dst,
                          int32_t dst_stride, uint32_t dst_w, uint32_t dst_h, uint64_t dst_revision, fa_surface_frame* fr)
{
    fa_rects copy;
    int64_t start = fa_monotonic_ns();
    int i, y, partial;
    fa_result r = layout(l, aw, ah, tempo);
    if (r != FA_OK) return r;
    if (dst_w != (uint32_t)l->bw || dst_h != (uint32_t)l->bh) return FA_ERROR_SIZE_MISMATCH;
    if (dst_stride < l->bw * 4) return FA_ERROR_INVALID_ARG;
    r = render(l, time);
    if (r != FA_OK) return r;
    fr->render_ns = fa_monotonic_ns() - start;
    partial = fa_history_since(&l->hist, l->revision, dst_revision, &copy);
    if (!partial) {
        fa_rects_clear(&copy);
        fa_rects_add(&copy, 0, 0, l->bw, l->bh);
    }
    for (i = 0; i < copy.n; i++) {
        const int* c = copy.r[i];
        for (y = c[1]; y < c[3]; y++)
            memcpy(dst + (size_t)y * (size_t)dst_stride + (size_t)c[0] * 4,
                   l->master + ((size_t)y * (size_t)l->bw + (size_t)c[0]) * 4, (size_t)(c[2] - c[0]) * 4);
    }
    fr->revision = l->revision;
    fr->base_revision = partial ? dst_revision : 0;
    fr->dirty_count = (uint32_t)copy.n;
    for (i = 0; i < copy.n; i++) to_rect(copy.r[i], &fr->dirty[i]);
    {
        int b[4];
        fa_rects_bounds(&copy, b);
        to_rect(b, &fr->dirty_bounds);
    }
    fr->time = time;
    fr->background_index = fr->border_index = -1;
    return FA_OK;
}
