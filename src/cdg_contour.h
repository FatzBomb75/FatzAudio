/* Contour mode for CD+G presentation scaling (smoothness 5-10): each colour region's
 * boundary is traced along the CD+G pixel edges, smoothed by local rules (see
 * cdg_contour.c) and drawn at the output size with exact-area anti-aliasing. */
#ifndef FA_CDG_CONTOUR_H
#define FA_CDG_CONTOUR_H

#include "cdg_scale.h"
#include <stddef.h>
#include <stdint.h>

typedef struct cdg_contours cdg_contours;

/* How far (CD+G pixels, in every direction) a changed pixel can change what level
 * `level` draws. */
int cdg_contour_reach(int level);

/* Traces and smooths `screen` (colour indices 0..16) for drawing at multiple n.
 * level 5..10; 0 keeps the traced pixel edges as they are (nearest, for tests).
 * *ct is made or reused; 0 when out of memory (*ct is then left without contours). */
int cdg_contours_build(cdg_contours** ct, const uint8_t screen[CDG_SCALE_H][CDG_SCALE_W], int level, uint32_t n);
void cdg_contours_free(cdg_contours* ct);

/* As cdg_scale_rect, from the contours of the last build of the same screen. `acc`
 * is scratch of CDG_CONTOUR_ACC_BYTES(n). */
#define CDG_CONTOUR_ACC_BYTES(n) (((size_t)CDG_SCALE_W * (n) + 4) * 4 * sizeof(int32_t))
void cdg_contours_draw(const cdg_contours* ct, const uint8_t screen[CDG_SCALE_H][CDG_SCALE_W],
                       const uint8_t pm[CDG_SCALE_COLORS][4], uint8_t* out, size_t out_stride, int ox, int oy,
                       int sx0, int sy0, int sx1, int sy1, int32_t* acc);

#endif
