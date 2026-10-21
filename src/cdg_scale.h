/* Drawing a CD+G screen at a whole-number multiple, for the presentation options:
 * smoothness 0-4 and edge blend, into a premultiplied 4-byte-per-pixel bitmap. */
#ifndef FA_CDG_SCALE_H
#define FA_CDG_SCALE_H

#include <stddef.h>
#include <stdint.h>

#define CDG_SCALE_W 300
#define CDG_SCALE_H 216
#define CDG_SCALE_COLORS 17      /* the 16 palette entries and 16 = transparent */
#define CDG_SCALE_MAX_N 64
/* The scratch cdg_scale_rect needs at multiple n: three output rows across the screen. */
#define CDG_SCALE_ROWS_BYTES(n) ((size_t)3 * CDG_SCALE_W * (n) * 4 * sizeof(uint16_t))

typedef struct cdg_scaler {
    uint32_t n;                   /* output pixels per CD+G pixel */
    int smooth;                   /* smoothing in use: phase tables below */
    int edge_blend;
    /* Per output pixel phase p (0..n-1) inside a CD+G pixel c: the two CD+G pixels it
     * blends are c + off[p] and c + off[p] + 1, the second with weight w[p] / 256. */
    int8_t off[CDG_SCALE_MAX_N];
    uint16_t w[CDG_SCALE_MAX_N];
    /* How far a changed CD+G pixel reaches in what is drawn: across an edge and along
     * it (edge blend follows edges for a few pixels). */
    int reach_across, reach_along;
} cdg_scaler;

/* smoothness 0..4 (higher draws as 4), edge_blend 0/1, n 1..CDG_SCALE_MAX_N. */
void cdg_scaler_init(cdg_scaler* sc, uint32_t n, int smoothness, int edge_blend);

/* Draws screen pixels [sx0, sx1) x [sy0, sy1) of `screen` (colour indices 0..16)
 * into `out`, whose top-left pixel is screen pixel (ox, oy): pixel (sx, sy) covers
 * out[(sy - oy) * n .. + n][(sx - ox) * n .. + n]. `pm` is the premultiplied colour
 * of each index, in the output's byte order. Reads neighbours outside the rectangle
 * (up to the reach) from `screen`, writes only inside it. `rows` is scratch of
 * CDG_SCALE_ROWS_BYTES(sc->n). */
void cdg_scale_rect(const cdg_scaler* sc, const uint8_t screen[CDG_SCALE_H][CDG_SCALE_W],
                    const uint8_t pm[CDG_SCALE_COLORS][4], uint8_t* out, size_t out_stride, int ox, int oy,
                    int sx0, int sy0, int sx1, int sy1, uint16_t* rows);

#endif
