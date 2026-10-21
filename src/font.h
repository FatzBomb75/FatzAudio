/* Fonts for the lyric surface: our own reader of TrueType (glyf) and CFF outlines,
 * TrueType collections, cmap, metrics and pair kerning (kern and GPOS), and an
 * exact-area rasterizer (raster.h). Every read of the file is bounds-checked: the
 * app can hand us any user's font. Behind this interface so another reader could
 * replace it without touching the layout code. */
#ifndef FA_FONT_H
#define FA_FONT_H

#include "fatzaudio/fatzaudio.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct fa_font fa_font;

typedef struct fa_font_info {
    int units_per_em;
    int ascender, descender, line_gap; /* hhea (OS/2 typo with USE_TYPO_METRICS), descender < 0 */
    int cap_height, x_height;          /* OS/2 version 2 and later, else 0 */
    int weight;                        /* 100..900, OS/2 usWeightClass (400 without) */
    int italic, bold;                  /* OS/2 fsSelection, else head macStyle */
    int cff;                           /* outlines are CFF (cubic) */
    int monospace;
    uint32_t glyph_count;
    char family[128];                  /* UTF-8: typographic family (name 16), else name 1 */
    char style[64];                    /* typographic subfamily (17), else 2 */
    char legacy_family[128];           /* name 1 (as Windows groups faces: "Arial Black") */
} fa_font_info;

/* Path commands in font units, y up. A contour starts at each MOVE and is closed. */
enum { FA_PATH_MOVE, FA_PATH_LINE, FA_PATH_QUAD, FA_PATH_CUBIC };

typedef struct fa_path_cmd {
    uint8_t op;
    float x, y;                        /* the end point */
    float x1, y1, x2, y2;              /* control points: QUAD uses 1, CUBIC 1 and 2 */
} fa_path_cmd;

typedef struct fa_path {
    fa_path_cmd* cmd;
    size_t count, cap;
    float xmin, ymin, xmax, ymax;      /* of every point, control points included */
} fa_path;

/* Faces in a font file: 1 for a .ttf/.otf, n for a .ttc/.otc, 0 when it is not one. */
uint32_t fa_font_face_count(const uint8_t* data, size_t size);
/* Opens face `index` of the file in `data`, which must outlive the font; with
 * `owned` the font frees it (free()) when closed, also when the open fails.
 * FA_ERROR_UNSUPPORTED_FORMAT for anything else (CFF2, bitmap-only fonts). */
fa_result fa_font_open(const uint8_t* data, size_t size, uint32_t index, int owned, fa_font** out);
/* Reads the whole file (UTF-8 path) and opens face `index`. */
fa_result fa_font_open_file(const char* path, uint32_t index, fa_font** out);
void fa_font_close(fa_font* font);
/* Reads only face `index`'s names, weight and style from an open file (for
 * finding fonts without loading them): units, metrics and glyph_count are not
 * filled in. Sets *face_count (may be NULL) when the file could be read. */
fa_result fa_font_probe(FILE* fp, uint32_t* face_count, uint32_t index, fa_font_info* out);

const fa_font_info* fa_font_get_info(const fa_font* font);
/* The glyph for a Unicode code point, 0 (.notdef) when the font has none. */
uint32_t fa_font_glyph(const fa_font* font, uint32_t codepoint);
/* Advance width, font units. */
int32_t fa_font_advance(const fa_font* font, uint32_t glyph);
/* Pair kerning to add to the left glyph's advance, font units: GPOS 'kern' pairs,
 * else the kern table. */
int32_t fa_font_kerning(const fa_font* font, uint32_t left, uint32_t right);
/* Appends the glyph's outline to `path` (cleared first). FA_OK with no commands
 * for an empty glyph (a space); FA_ERROR_UNSUPPORTED_FORMAT for a glyph that cannot
 * be read. */
fa_result fa_font_outline(const fa_font* font, uint32_t glyph, fa_path* path);

void fa_path_clear(fa_path* path);
void fa_path_free(fa_path* path);

#endif
