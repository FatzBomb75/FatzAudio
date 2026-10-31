/* Fuzz target: the font reader (font.c) and the rasterizer behind the lyric surface.
 * Opens every face (up to 4) of a .ttf/.otf/.ttc: names and metrics, cmap lookups,
 * advances, kerning (kern and GPOS), TrueType glyf and CFF charstring outlines,
 * each drawn as the lyric surface draws glyphs; then the probe that font lookup uses. */
#include "font.h"
#include "raster.h"
#include <stdio.h>
#include <stdlib.h>

#define BOX 64

static const uint32_t k_chars[] = { 'A', 'V', 'a', 'g', ' ', '0', 0xE9, 0x3042, 0x4E2D, 0xAC00, 0x1F600, 0xFFFF, 0x10FFFF };

static void draw(fa_raster* r, const fa_font* f, uint32_t glyph, fa_path* p)
{
    const fa_font_info* in = fa_font_get_info(f);
    float s = 48.0f / (float)(in->units_per_em > 0 ? in->units_per_em : 1000);
    if (fa_font_outline(f, glyph, p) != FA_OK || !p->count) return;
    fa_raster_path(r, p, s, -s, 8.0f, 52.0f);
    fa_raster_path(r, p, s * 0.37f, -s * 0.37f, 30.3f, 20.7f);   /* small, at a sub-pixel offset */
}

static void exercise(const fa_font* f)
{
    const fa_font_info* in = fa_font_get_info(f);
    uint32_t g[sizeof(k_chars) / sizeof(k_chars[0])], i, n, j;
    fa_path p = { 0 };
    fa_raster r = { 0 };
    static uint8_t cov[BOX * BOX];
    n = (uint32_t)(sizeof(k_chars) / sizeof(k_chars[0]));
    for (i = 0; i < n; i++) {
        g[i] = fa_font_glyph(f, k_chars[i]);
        (void)fa_font_advance(f, g[i]);
    }
    for (i = 0; i + 1 < n; i++) (void)fa_font_kerning(f, g[i], g[i + 1]);
    (void)fa_font_kerning(f, 0xFFFFu, 0x10000u);
    if (!fa_raster_begin(&r, BOX, BOX)) return;
    for (i = 0; i < n; i++) draw(&r, f, g[i], &p);
    /* glyphs by index too: ones no character maps to, the last, and one past it */
    for (j = 0; j < in->glyph_count && j < 24; j++) draw(&r, f, j, &p);
    if (in->glyph_count) draw(&r, f, in->glyph_count - 1, &p);
    draw(&r, f, in->glyph_count, &p);
    fa_raster_end(&r, cov, BOX);
    fa_raster_free(&r);
    fa_path_free(&p);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    uint32_t faces = fa_font_face_count(data, size), i;
    FILE* fp;
    for (i = 0; i < faces && i < 4; i++) {
        fa_font* f = NULL;
        if (fa_font_open(data, size, i, 0, &f) != FA_OK) continue;
        exercise(f);
        fa_font_close(f);
    }
    if ((fp = tmpfile()) != NULL) {
        uint32_t count = 0;
        fa_font_info info;
        if (fwrite(data, 1, size, fp) == size) {
            rewind(fp);
            (void)fa_font_probe(fp, &count, 0, &info);
            if (count > 1) { rewind(fp); (void)fa_font_probe(fp, NULL, count - 1, &info); }
        }
        fclose(fp);
    }
    return 0;
}
