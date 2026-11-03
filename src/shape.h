/* Text shaping for the lyric surface: a line's glyphs and their places, with the
 * operating system's own shaper for the scripts that need one (Arabic joining,
 * Hebrew and Arabic right to left, Indic reordering, marks): DirectWrite on
 * Windows, Core Text on macOS and iOS, the system's HarfBuzz and FriBidi on Linux
 * (loaded at runtime), the platform's TextRunShaper on Android 12 and later
 * (through the app's Java VM). Nothing is bundled. Text that needs no shaping
 * (Latin, Greek, Cyrillic, CJK, Hangul, symbols), and any text where the system
 * has no shaper, is laid out by our own font reader: one glyph per character from
 * the cmap, left to right, with pair kerning. The glyphs always come from our
 * font reader's face (font.h), so a line is drawn the same way everywhere. */
#ifndef FA_SHAPE_H
#define FA_SHAPE_H

#include "font.h"

typedef struct fa_shaped_glyph {
    uint32_t glyph;               /* in the font; 0 (.notdef) where it has none; UINT32_MAX: nothing to draw */
    float x, y;                   /* its origin from the line's left end and baseline, em (font size 1), y up */
} fa_shaped_glyph;

typedef struct fa_shaped {
    fa_shaped_glyph* glyph;
    uint32_t count;
    float width;                  /* em */
    /* For the character that starts at each byte (bytes inside a character repeat
     * it), `len` of each: its left and right edge on the line, em, and 1 when it
     * reads right to left. A ligature's width is shared between its characters. */
    float* left;
    float* right;
    uint8_t* rtl;
    int shaped;                   /* 1: laid out by the OS's shaper */
} fa_shaped;

/* Lays out text[0..len) (UTF-8) in `font`. `spacing` (em) is added after each
 * character by our own layout (synthetic bold); the OS's shaper leaves it out, since
 * it would break joined scripts apart. FA_ERROR_OUT_OF_MEMORY or FA_OK. */
fa_result fa_text_shape(fa_font* font, const char* text, uint32_t len, float spacing, fa_shaped* out);
/* The width fa_text_shape would give, em. */
float fa_text_width(fa_font* font, const char* text, uint32_t len, float spacing);
void fa_shaped_free(fa_shaped* shaped);
/* The OS shaper this process uses: "DirectWrite", "Core Text", "HarfBuzz",
 * "TextRunShaper"; "" when the system has none (or tests turned it off). */
const char* fa_shaper_name(void);
/* font.c, when a font closes: its shaper state. */
void fa_text_release(void* state);
/* Tests: 1 = lay everything out with our own reader. */
void fa_text_set_disabled(int disabled);
/* 1 when text[0..len) has a character our own layout cannot place well. */
int fa_text_needs_shaping(const char* text, uint32_t len);
/* The next code point of UTF-8 text[*i..len) (U+FFFD for a bad sequence); *i moves past it. */
uint32_t fa_text_next_cp(const char* s, uint32_t len, uint32_t* i);

/* ---- the OS shaper's half (shape_dwrite.cpp, shape_ct.c, shape_hb.c, shape_android.c) ---- */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fa_text_glyph {
    uint32_t glyph;
    uint32_t cluster;             /* the byte its cluster's first character starts at */
    float pen, adv;               /* font units, left to right */
    float dx, dy;                 /* the glyph from the pen, y up */
    uint8_t rtl;
} fa_text_glyph;

typedef struct fa_text_buf {
    fa_text_glyph* g;            /* in visual order, left to right */
    uint32_t n, cap;
    /* A shaper that knows each character's edges but not the clusters (Android) fills
     * these (len each, font units) and sets `edges`; cluster is then unused. */
    float* left;
    float* right;
    uint8_t* rtl;
    int edges;
    uint32_t len;
} fa_text_buf;

/* Appends a glyph; 0 when out of memory. */
int fa_text_push(fa_text_buf* b, uint32_t glyph, uint32_t cluster, float pen, float adv, float dx, float dy, int rtl);
/* Allocates b->left, right and rtl for b->len bytes; 0 when out of memory. */
int fa_text_edges_alloc(fa_text_buf* b);
/* text as UTF-16 (n16 units) and, for each unit and one past the end, the byte its
 * character starts at. NULL when out of memory. Free both with free(). */
uint16_t* fa_text_utf16(const char* text, uint32_t len, uint32_t* n16, uint32_t** byte_of);
/* 1 when the paragraph's first strong character reads right to left (Unicode bidi
 * P2-P3 with a simplified character table). */
int fa_text_rtl_paragraph(const char* text, uint32_t len);
/* Visual order of runs with these bidi levels (Unicode bidi L2): order[i] is the run
 * shown i-th from the left. */
void fa_text_visual_order(const uint8_t* levels, uint32_t n, uint32_t* order);

/* A font slot (fa_font_shaper_slot) whose font the platform's shaper could not take:
 * not tried again. */
#define FA_TEXT_STATE_FAILED ((void*)1)

/* The platform's shaper. FA_OK: `out` holds the line; FA_ERROR_NOT_FOUND: no shaper
 * here, or it could not take this font or text (our own layout is used);
 * FA_ERROR_OUT_OF_MEMORY. `state` is the font's slot (fa_font_shaper_slot). */
fa_result fa_text_os(fa_font* font, void** state, const char* text, uint32_t len, fa_text_buf* out);
const char* fa_text_os_name(void);
void fa_text_os_release(void* state);
/* Windows and Android (font_find.c): the font the system falls back to for text[0..len),
 * as a family name (DirectWrite) or a file and face (Android). 0 when it cannot tell. */
int fa_text_os_fallback(const char* text, uint32_t len, int weight, int italic, char* family, size_t family_cap,
                         char* path, size_t path_cap, uint32_t* index);

#ifdef __cplusplus
}
#endif

#endif
