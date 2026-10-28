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
/* Writes the coverage, 0..255, one byte per pixel. */
void fa_raster_end(fa_raster* r, uint8_t* out, size_t stride);
void fa_raster_free(fa_raster* r);

/* The antiderivative of clamp(z, 0, 1). */
static inline float fa_edge_ramp(float z) { return z <= 0.0f ? 0.0f : z >= 1.0f ? z - 0.5f : 0.5f * z * z; }

/* How much of pixels 0..i an edge crossing a row from x = lo to x = hi (width = hi - lo) covers, 0..1: the box
 * filter's integral. */
static inline float fa_edge_cover(int i, float lo, float hi, float width)
{
    if (width < 1e-6f) {
        float z = (float)(i + 1) - lo;
        return z <= 0.0f ? 0.0f : z >= 1.0f ? 1.0f : z;
    }
    return (fa_edge_ramp((float)(i + 1) - lo) - fa_edge_ramp((float)(i + 1) - hi)) / width;
}

#endif
