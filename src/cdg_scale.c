/* Drawing a CD+G screen at a whole-number multiple: see cdg_scale.h.
 *
 * Smoothness 0 is nearest (each CD+G pixel an n x n block). 1-4 are "sharp
 * bilinear": nearest inside a pixel, and a band at each pixel boundary where the two
 * colours blend: two output pixels wide at 1 (the pixel either side), then 0.25, 0.5
 * and 1 CD+G pixel, never narrower than level 1 (4 is plain bilinear). All of it in premultiplied
 * colour, so a colour next to the transparent background fades out without a halo.
 *
 * Edge blend is morphological anti-aliasing (after Reshetov's MLAA) on the CD+G
 * grid: an edge is where two neighbouring pixels have different colours. Each
 * straight run of one edge (the same two colours on either side) is looked at from
 * both ends: where the colour on one side steps over the edge at an end (an L, Z or
 * U shape), a line runs from half a pixel off the edge at that end back to the edge,
 * over half the run (both ends step) or all of it (one end does), at most
 * EDGE_SPAN pixels; the output pixels between that line and the edge take the other
 * colour by the area they cover. The grid is exact, so no threshold is needed and
 * nothing else is touched. */
#include "cdg_scale.h"
#include "cdg_contour.h"
#include <string.h>

#define EDGE_SPAN 4               /* CD+G pixels a step's line reaches along its edge */

void cdg_scaler_init(cdg_scaler* sc, uint32_t n, int smoothness, int edge_blend)
{
    cdg_scaler_init_look(sc, n, n, smoothness, edge_blend);
}

void cdg_scaler_init_look(cdg_scaler* sc, uint32_t n, uint32_t look, int smoothness, int edge_blend)
{
    static const float band[5] = { 0.0f, 0.0f, 0.25f, 0.5f, 1.0f };
    uint32_t p;
    float width;
    struct cdg_contours* ct = sc->ct;
    int contour = smoothness >= 5 && smoothness <= 10 ? smoothness : 0, failed = sc->ct_failed;
    memset(sc, 0, sizeof(*sc));
    sc->ct = ct;
    sc->ct_failed = failed;
    if (n < 1) n = 1;
    if (n > CDG_SCALE_MAX_N) n = CDG_SCALE_MAX_N;
    if (look < 1 || look > n) look = n;
    if (smoothness < 0) smoothness = 0;
    if (smoothness > 4) smoothness = 4;   /* contour mode falls back to 4 without memory */
    if (contour) edge_blend = 0;          /* nothing left for it to do */
    sc->n = n;
    sc->smooth = smoothness > 0;
    sc->edge_blend = edge_blend != 0;
    /* At least two output pixels wide, so the pixel either side of a boundary blends. */
    width = band[smoothness] > 2.0f / (float)look ? band[smoothness] : 2.0f / (float)look;
    if (width > 1.0f) width = 1.0f;
    for (p = 0; p < n; p++) {
        /* Output pixel centre u inside its CD+G pixel; t = position between the
         * centres of the two CD+G pixels it falls between. */
        float u = ((float)p + 0.5f) / (float)n, t;
        if (!sc->smooth) continue;
        if (u < 0.5f) { sc->off[p] = -1; t = u + 0.5f; }
        else { sc->off[p] = 0; t = u - 0.5f; }
        t = (t - 0.5f) / width + 0.5f;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        sc->w[p] = (uint16_t)(t * 256.0f + 0.5f);
    }
    sc->reach_across = sc->smooth || sc->edge_blend ? 1 : 0;
    sc->reach_along = sc->edge_blend ? 2 * EDGE_SPAN + 2 : sc->reach_across;
    if (contour) {
        /* An outline bends round corners: the reach is the same every way. */
        sc->contour = contour;
        sc->reach_across = sc->reach_along = cdg_contour_reach(contour);
    }
}

int cdg_scale_prepare(cdg_scaler* sc, const uint8_t screen[CDG_SCALE_H][CDG_SCALE_W])
{
    int was;
    if (!sc->contour) return 0;
    was = !sc->ct_failed;
    sc->ct_ok = cdg_contours_build(&sc->ct, screen, sc->contour, sc->n);
    if (!sc->ct_ok) sc->ct_failed = 1;
    if (sc->ct_ok && !was) {
        sc->ct_failed = 0;
        return 1;
    }
    return 0;
}

void cdg_scaler_free(cdg_scaler* sc)
{
    cdg_contours_free(sc->ct);
    sc->ct = NULL;
    sc->ct_ok = 0;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Smooth rows: the horizontal blend of screen row `sy` for output columns from
 * sx0 * n on, kept at 16 bits (colour * 256) so the vertical blend rounds once. */
static void blend_row(const cdg_scaler* sc, const uint8_t* row, const uint8_t pm[CDG_SCALE_COLORS][4], int sx0,
                      int sx1, uint16_t* out)
{
    uint32_t n = sc->n, i, k;
    int sx;
    for (sx = sx0; sx < sx1; sx++)
        for (i = 0; i < n; i++, out += 4) {
            uint8_t a = row[clampi(sx + sc->off[i], 0, CDG_SCALE_W - 1)];
            uint8_t b = row[clampi(sx + sc->off[i] + 1, 0, CDG_SCALE_W - 1)];
            uint32_t w = sc->w[i];
            if (a == b || w == 0) { for (k = 0; k < 4; k++) out[k] = (uint16_t)(pm[a][k] << 8); continue; }
            if (w == 256) { for (k = 0; k < 4; k++) out[k] = (uint16_t)(pm[b][k] << 8); continue; }
            for (k = 0; k < 4; k++) out[k] = (uint16_t)(pm[a][k] * (256 - w) + pm[b][k] * w);
        }
}

static void base_pass(const cdg_scaler* sc, const uint8_t screen[CDG_SCALE_H][CDG_SCALE_W],
                      const uint8_t pm[CDG_SCALE_COLORS][4], uint8_t* out, size_t stride, int ox, int oy,
                      int sx0, int sy0, int sx1, int sy1, uint16_t* rows)
{
    /* rows, smooth: the blended screen rows sy - 1, sy, sy + 1 */
    uint32_t n = sc->n, i, j, w = (uint32_t)(sx1 - sx0) * n;
    int have[3] = { INT32_MIN, INT32_MIN, INT32_MIN }, sx, sy;
    for (sy = sy0; sy < sy1; sy++) {
        uint8_t* block_row = out + (size_t)(sy - oy) * n * stride;
        int need = 0;
        /* Sharp, or flat (all neighbours one colour, so every blend is too): blocks. */
        for (sx = sx0; sx < sx1; sx++) {
            uint8_t c = screen[sy][sx];
            uint8_t* block = block_row + (size_t)(sx - ox) * n * 4;
            if (sc->smooth) {
                int xa = clampi(sx - 1, 0, CDG_SCALE_W - 1), xb = clampi(sx + 1, 0, CDG_SCALE_W - 1);
                int ya = clampi(sy - 1, 0, CDG_SCALE_H - 1), yb = clampi(sy + 1, 0, CDG_SCALE_H - 1);
                if (!(screen[ya][xa] == c && screen[ya][sx] == c && screen[ya][xb] == c && screen[sy][xa] == c &&
                      screen[sy][xb] == c && screen[yb][xa] == c && screen[yb][sx] == c && screen[yb][xb] == c)) {
                    need = 1;
                    continue;
                }
            }
            for (i = 0; i < n; i++) memcpy(block + i * 4, pm[c], 4);
            for (j = 1; j < n; j++) memcpy(block + j * stride, block, (size_t)n * 4);
        }
        if (!need) continue;
        /* The blended rows this screen row's output reads, made once each. */
        for (j = 0; j < 3; j++) {
            int r = clampi(sy - 1 + (int)j, 0, CDG_SCALE_H - 1), k;
            if (have[j] == r) continue;
            for (k = 0; k < 3 && have[k] != r; k++) {}
            if (k < 3) {
                memmove(rows + (size_t)j * w * 4, rows + (size_t)k * w * 4, (size_t)w * 4 * sizeof(uint16_t));
            } else {
                blend_row(sc, screen[r], pm, sx0, sx1, rows + (size_t)j * w * 4);
            }
            have[j] = r;
        }
        for (j = 0; j < n; j++) {
            int ra = clampi(sy + sc->off[j], 0, CDG_SCALE_H - 1) - (sy - 1);
            int rb = clampi(sy + sc->off[j] + 1, 0, CDG_SCALE_H - 1) - (sy - 1);
            const uint16_t* ta = rows + (size_t)clampi(ra, 0, 2) * w * 4;
            const uint16_t* tb = rows + (size_t)clampi(rb, 0, 2) * w * 4;
            uint32_t wy = sc->w[j];
            uint8_t* px = block_row + j * stride;
            for (sx = sx0; sx < sx1; sx++) {
                uint8_t c = screen[sy][sx];
                size_t at = (size_t)(sx - sx0) * n * 4;
                uint8_t* q = px + (size_t)(sx - ox) * n * 4;
                int xa = clampi(sx - 1, 0, CDG_SCALE_W - 1), xb = clampi(sx + 1, 0, CDG_SCALE_W - 1);
                int ya = clampi(sy - 1, 0, CDG_SCALE_H - 1), yb = clampi(sy + 1, 0, CDG_SCALE_H - 1);
                if (screen[ya][xa] == c && screen[ya][sx] == c && screen[ya][xb] == c && screen[sy][xa] == c &&
                    screen[sy][xb] == c && screen[yb][xa] == c && screen[yb][sx] == c && screen[yb][xb] == c)
                    continue;   /* flat: done above */
                if (wy == 0 || ta == tb) {
                    for (i = 0; i < n * 4; i++) q[i] = (uint8_t)((ta[at + i] + 128) >> 8);
                } else if (wy == 256) {
                    for (i = 0; i < n * 4; i++) q[i] = (uint8_t)((tb[at + i] + 128) >> 8);
                } else {
                    for (i = 0; i < n * 4; i++)
                        q[i] = (uint8_t)((ta[at + i] * (256 - wy) + tb[at + i] * wy + 32768) >> 16);
                }
            }
        }
    }
}

/* ---- edge blend ---------------------------------------------------------------------- */

typedef struct edge_ctx {
    const cdg_scaler* sc;
    uint8_t* out;
    size_t stride;
    int ox, oy, sx0, sy0, sx1, sy1;
} edge_ctx;

/* One end of a run on the edge between cells e and e + 1 (rows, or columns when
 * `vertical`): the run's end is the boundary `at` along the edge, the run lies on
 * the side `dir` (+1: from `at` on, -1: before it). Over `d` cells from `at`, the
 * area between the edge and a line from half a cell off it (at `at`) back to it
 * (at d) takes colour `c`, in cell e (side 0) or e + 1 (side 1). */
static void blend_end(const edge_ctx* m, int vertical, int e, int at, int dir, float d, int side, const uint8_t c[4])
{
    uint32_t n = m->sc->n, k, j;
    int cell = side ? e + 1 : e, q, q0, q1;
    if (dir > 0) { q0 = at; q1 = at + (int)(d + 0.999f); }
    else { q0 = at - (int)(d + 0.999f); q1 = at; }
    for (q = q0; q < q1; q++) {
        int sx = vertical ? cell : q, sy = vertical ? q : cell;
        uint8_t* block;
        if (sx < m->sx0 || sx >= m->sx1 || sy < m->sy0 || sy >= m->sy1) continue;
        block = m->out + (size_t)(sy - m->oy) * n * m->stride + (size_t)(sx - m->ox) * n * 4;
        for (k = 0; k < n; k++) {
            float pos = (float)q + ((float)k + 0.5f) / (float)n;
            float dist = dir > 0 ? pos - (float)at : (float)at - pos, depth;
            if (dist < 0.0f || dist >= d) continue;
            depth = 0.5f * (1.0f - dist / d);
            for (j = 0; j < n; j++) {
                /* Sub-pixel j across the cell, as distance from the edge. */
                float u0 = (float)(side ? j : n - 1 - j) / (float)n, u1 = u0 + 1.0f / (float)n;
                float cov = ((depth < u1 ? depth : u1) - u0) * (float)n;
                uint8_t* px;
                uint32_t a, ch;
                if (cov <= 0.0f) continue;
                if (cov > 1.0f) cov = 1.0f;
                a = (uint32_t)(cov * 256.0f + 0.5f);
                px = vertical ? block + (size_t)k * m->stride + (size_t)j * 4 : block + (size_t)j * m->stride + (size_t)k * 4;
                for (ch = 0; ch < 4; ch++) px[ch] = (uint8_t)((px[ch] * (256 - a) + c[ch] * a + 128) >> 8);
            }
        }
    }
}

enum { END_NONE, END_NEAR, END_FAR };

/* Edges between rows (vertical = 0) or columns (1). */
static void edge_pass(const edge_ctx* m, int vertical, const uint8_t screen[CDG_SCALE_H][CDG_SCALE_W],
                      const uint8_t pm[CDG_SCALE_COLORS][4])
{
    int across = vertical ? CDG_SCALE_W : CDG_SCALE_H, along = vertical ? CDG_SCALE_H : CDG_SCALE_W;
    int e0 = (vertical ? m->sx0 : m->sy0) - 1, e1 = vertical ? m->sx1 : m->sy1;
    int lo = vertical ? m->sy0 : m->sx0, hi = vertical ? m->sy1 : m->sx1, e;
    if (e0 < 0) e0 = 0;
    if (e1 > across - 1) e1 = across - 1;
    for (e = e0; e < e1; e++) {
        int p = 0;
#define CELL(q, side) (vertical ? screen[q][e + (side)] : screen[e + (side)][q])
        while (p < along) {
            uint8_t a, b;
            int start, end, len, t0, t1;
            float d0, d1;
            if (CELL(p, 0) == CELL(p, 1)) { p++; continue; }
            a = CELL(p, 0);
            b = CELL(p, 1);
            start = p;
            while (p < along && CELL(p, 0) == a && CELL(p, 1) == b) p++;
            end = p;
            if (end <= lo || start >= hi) continue;   /* blends stay inside the run */
            /* At each end: does side 1's colour step into side 0 (NEAR), or side 0's
             * into side 1 (FAR)? */
            t0 = start == 0 ? END_NONE
               : CELL(start - 1, 0) == b && CELL(start - 1, 1) == b ? END_NEAR
               : CELL(start - 1, 0) == a && CELL(start - 1, 1) == a ? END_FAR : END_NONE;
            t1 = end == along ? END_NONE
               : CELL(end, 0) == b && CELL(end, 1) == b ? END_NEAR
               : CELL(end, 0) == a && CELL(end, 1) == a ? END_FAR : END_NONE;
            len = end - start;
            d0 = t1 != END_NONE ? 0.5f * (float)len : (float)len;
            d1 = t0 != END_NONE ? 0.5f * (float)len : (float)len;
            if (d0 > EDGE_SPAN) d0 = EDGE_SPAN;
            if (d1 > EDGE_SPAN) d1 = EDGE_SPAN;
            if (t0 == END_NEAR) blend_end(m, vertical, e, start, 1, d0, 0, pm[b]);
            if (t0 == END_FAR) blend_end(m, vertical, e, start, 1, d0, 1, pm[a]);
            if (t1 == END_NEAR) blend_end(m, vertical, e, end, -1, d1, 0, pm[b]);
            if (t1 == END_FAR) blend_end(m, vertical, e, end, -1, d1, 1, pm[a]);
        }
#undef CELL
    }
}

void cdg_scale_rect(const cdg_scaler* sc, const uint8_t screen[CDG_SCALE_H][CDG_SCALE_W],
                    const uint8_t pm[CDG_SCALE_COLORS][4], uint8_t* out, size_t out_stride, int ox, int oy,
                    int sx0, int sy0, int sx1, int sy1, uint16_t* rows)
{
    edge_ctx m;
    if (sx0 < 0) sx0 = 0;
    if (sy0 < 0) sy0 = 0;
    if (sx1 > CDG_SCALE_W) sx1 = CDG_SCALE_W;
    if (sy1 > CDG_SCALE_H) sy1 = CDG_SCALE_H;
    if (sx0 >= sx1 || sy0 >= sy1) return;
    if (sc->contour && sc->ct_ok) {
        cdg_contours_draw(sc->ct, screen, pm, out, out_stride, ox, oy, sx0, sy0, sx1, sy1, (int32_t*)(void*)rows);
        return;
    }
    base_pass(sc, screen, pm, out, out_stride, ox, oy, sx0, sy0, sx1, sy1, rows);
    if (!sc->edge_blend) return;
    m.sc = sc;
    m.out = out;
    m.stride = out_stride;
    m.ox = ox;
    m.oy = oy;
    m.sx0 = sx0;
    m.sy0 = sy0;
    m.sx1 = sx1;
    m.sy1 = sy1;
    edge_pass(&m, 0, screen, pm);
    edge_pass(&m, 1, screen, pm);
}
