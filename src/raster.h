/* Exact-area rasterizer: anti-aliased coverage of paths (font.h) from signed area
 * accumulation, with sub-pixel positioning and no hinting. Overlapping contours of
 * the same direction stay solid (coverage is clamped), as in glyphs made of
 * overlapping parts. */
#ifndef FA_RASTER_H
#define FA_RASTER_H

#include "font.h"
#include <stddef.h>
#include <stdint.h>

typedef struct fa_raster {
    float* acc;
    int w, h;
    size_t cap;
} fa_raster;

/* Starts a w x h coverage image (cleared). 0 when out of memory. */
int fa_raster_begin(fa_raster* r, int w, int h);
/* Adds `path` placed by x' = x * sx + dx, y' = y * sy + dy (pixels, y down: pass
 * a negative sy for font units). */
void fa_raster_path(fa_raster* r, const fa_path* path, float sx, float sy, float dx, float dy);
/* Adds one edge, pixels. */
void fa_raster_line(fa_raster* r, float x0, float y0, float x1, float y1);
/* Writes the coverage, 0..255, one byte per pixel. */
void fa_raster_end(fa_raster* r, uint8_t* out, size_t stride);
void fa_raster_free(fa_raster* r);

#endif
