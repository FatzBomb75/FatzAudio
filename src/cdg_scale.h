/* Drawing a CD+G screen at a whole-number multiple, for the presentation options:
 * smoothness 0-10 and edge blend, into a premultiplied 4-byte-per-pixel bitmap
 * (5-10 in cdg_contour.c). */
#ifndef FA_CDG_SCALE_H
#define FA_CDG_SCALE_H

#include <stddef.h>
#include <stdint.h>

#define CDG_SCALE_W 300
#define CDG_SCALE_H 216
#define CDG_SCALE_COLORS 17      /* the 16 palette entries and 16 = transparent */
#define CDG_SCALE_MAX_N 64
/* The scratch cdg_scale_rect needs at multiple n: three output rows across the screen
 * (smoothness 1-4), or one row of 32-bit sums and a little (5-10). */
#define CDG_SCALE_ROWS_BYTES(n) ((size_t)3 * CDG_SCALE_W * (n) * 4 * sizeof(uint16_t) + 64)

struct cdg_contours;

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
    int contour;                  /* smoothness 5..10: contour mode (else 0) */
    struct cdg_contours* ct;      /* its outlines, from cdg_scale_prepare */
    int ct_ok;                    /* 0: none (out of memory), drawn as smoothness 4 */
    int ct_failed;                /* some of the picture was drawn that way */
} cdg_scaler;

/* smoothness 0..10, edge_blend 0/1 (no effect at 5..10), n 1..CDG_SCALE_MAX_N. Keeps
 * the contour buffers of an earlier init, so `sc` must be zeroed before the first. */
void cdg_scaler_init(cdg_scaler* sc, uint32_t n, int smoothness, int edge_blend);
/* As cdg_scaler_init, drawing at n with the blend of size `look` (n a multiple of it),
 * so the picture box-filtered down to `look` looks as it does drawn there. */
void cdg_scaler_init_look(cdg_scaler* sc, uint32_t n, uint32_t look, int smoothness, int edge_blend);
/* Before drawing rectangles of a changed `screen` (contour mode traces it). Returns 1
 * when the whole screen must be drawn: contours are back after a trace that ran out
 * of memory (drawn as smoothness 4 meanwhile). */
int cdg_scale_prepare(cdg_scaler* sc, const uint8_t screen[CDG_SCALE_H][CDG_SCALE_W]);
void cdg_scaler_free(cdg_scaler* sc);

/* Draws screen pixels [sx0, sx1) x [sy0, sy1) of `screen` (colour indices 0..16)
 * into `out`, whose top-left pixel is screen pixel (ox, oy): pixel (sx, sy) covers
 * out[(sy - oy) * n .. + n][(sx - ox) * n .. + n]. `pm` is the premultiplied colour
 * of each index, in the output's byte order. Reads neighbours outside the rectangle
 * (up to the reach) from `screen`, writes only inside it. `rows` is scratch of
 * CDG_SCALE_ROWS_BYTES(sc->n), aligned for 32-bit values. */
void cdg_scale_rect(const cdg_scaler* sc, const uint8_t screen[CDG_SCALE_H][CDG_SCALE_W],
                    const uint8_t pm[CDG_SCALE_COLORS][4], uint8_t* out, size_t out_stride, int ox, int oy,
                    int sx0, int sy0, int sx1, int sy1, uint16_t* rows);

#endif
