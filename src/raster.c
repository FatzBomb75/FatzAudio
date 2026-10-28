/* Exact-area coverage. Each edge adds, to every pixel it crosses and every pixel
 * to its right, the signed fraction of that pixel it bounds: for one row of
 * pixels, an edge from x = a to x = b over a height h (signed by direction)
 * covers pixel column i by h times the mean, over the edge, of clamp(i + 1 - X,
 * 0, 1). That rises from 0 left of the edge to h right of it, so only the change
 * between neighbouring columns is stored, and a running sum along the row at the
 * end gives the winding-weighted area of each pixel. */
#include "raster.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RASTER_TOLERANCE 0.04f        /* flattening error, pixels */
#define RASTER_MAX_STEPS 256
/* Curve pieces of one path: a glyph has at most FONT_MAX_COMMANDS commands, and 256
 * pieces each would cost a crafted glyph seconds. Past it, curves are drawn as single
 * lines (the outline stays closed, only coarser). */
#define RASTER_MAX_SEGMENTS (1 << 20)
/* Pixel cells (rows crossed plus columns spanned, per edge) of one path, about one
 * step of row_edge each. The busiest glyph of a CJK font needs 12k at a 480-pixel em
 * (the largest lyric size at 2160p); a crafted glyph of 400k lines across a 2160p
 * layer would need 10^9 (9 s). Past it, the rest of the path is left out: such a
 * glyph draws wrong, but in milliseconds. */
#define RASTER_MAX_CELLS (1L << 22)
/* Accumulator size (floats) kept however small the next layer is. */
#define RASTER_KEEP_CELLS (1 << 20)

int fa_raster_begin(fa_raster* r, int w, int h)
{
    size_t need;
    if (w <= 0 || h <= 0) { r->w = r->h = 0; return 1; }
    if (w > 65536 || h > 65536 || (size_t)(w + 2) > SIZE_MAX / sizeof(float) / (size_t)h) return 0;
    need = (size_t)(w + 2) * (size_t)h;
    /* Grows to fit; a layer much smaller than the biggest so far (a damaged font's huge metrics) gives the rest back. */
    if (need > r->cap || (r->cap > RASTER_KEEP_CELLS && r->cap / 4 > need)) {
        float* n = (float*)realloc(r->acc, need * sizeof(float));
        if (!n) {
            if (need > r->cap) return 0;
        } else {
            r->acc = n;
            r->cap = need;
        }
    }
    r->w = w;
    r->h = h;
    memset(r->acc, 0, need * sizeof(float));
    return 1;
}

/* Adds the edge's part in one row: from x = a to x = b, signed height d. */
static void row_edge(float* row, int w, float a, float b, float d)
{
    float lo = a < b ? a : b, hi = a < b ? b : a, width = hi - lo, prev = 0.0f;
    int i0, i1, i;
    if (lo < 0.0f) lo = 0.0f;
    if (hi < 0.0f) hi = 0.0f;
    if (lo > (float)w) lo = (float)w;
    if (hi > (float)w) hi = (float)w;
    width = hi - lo;
    i0 = (int)lo;
    i1 = (int)ceilf(hi);
    if (i1 <= i0) i1 = i0 + 1;
    for (i = i0; i <= i1 && i <= w; i++) {
        float cov = fa_edge_cover(i, lo, hi, width);
        row[i] += d * (cov - prev);
        prev = cov;
    }
    if (i <= w + 1 && prev < 1.0f) row[i] += d * (1.0f - prev);
}

/* Adds one edge, pixels. */
static void raster_line(fa_raster* r, float x0, float y0, float x1, float y1)
{
    float dir = 1.0f, ya, yb;
    int y, ystart, yend;
    if (!r->w || y0 == y1 || !isfinite(x0) || !isfinite(y0) || !isfinite(x1) || !isfinite(y1)) return;
    if (y0 > y1) {
        float t;
        t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
        dir = -1.0f;
    }
    if (y1 <= 0.0f || y0 >= (float)r->h) return;
    ystart = y0 < 0.0f ? 0 : (int)y0;
    yend = y1 > (float)r->h ? r->h : (int)ceilf(y1);
    for (y = ystart; y < yend; y++) {
        float xa, xb;
        ya = (float)y > y0 ? (float)y : y0;
        yb = (float)(y + 1) < y1 ? (float)(y + 1) : y1;
        if (yb <= ya) continue;
        /* Interpolated, so a near-horizontal edge cannot overflow to inf * 0. */
        xa = x0 + (x1 - x0) * ((ya - y0) / (y1 - y0));
        xb = x0 + (x1 - x0) * ((yb - y0) / (y1 - y0));
        if (!isfinite(xa) || !isfinite(xb)) continue;
        row_edge(r->acc + (size_t)y * (size_t)(r->w + 2), r->w, xa, xb, dir * (yb - ya));
    }
}

/* The cells raster_line steps through for this edge (the same clipping). */
static long line_cells(const fa_raster* r, float x0, float y0, float x1, float y1)
{
    float lo = x0 < x1 ? x0 : x1, hi = x0 < x1 ? x1 : x0, ya = y0 < y1 ? y0 : y1, yb = y0 < y1 ? y1 : y0;
    if (!(lo < (float)r->w) || !(hi > 0.0f)) lo = hi = 0.0f;   /* off the side, or NaN */
    if (lo < 0.0f) lo = 0.0f;
    if (hi > (float)r->w) hi = (float)r->w;
    if (!(ya < (float)r->h) || !(yb > 0.0f)) return 1;
    if (ya < 0.0f) ya = 0.0f;
    if (yb > (float)r->h) yb = (float)r->h;
    return (long)(yb - ya) + (long)(hi - lo) + 2;
}

/* Adds an edge of a path while its cell budget lasts; 0 once it is spent. */
static int path_line(fa_raster* r, long* cells, float x0, float y0, float x1, float y1)
{
    *cells -= line_cells(r, x0, y0, x1, y1);
    if (*cells < 0) return 0;
    raster_line(r, x0, y0, x1, y1);
    return 1;
}

static int steps_for(float dev, long* budget)
{
    float f;
    int n;
    if (!(dev < 1e8f)) n = RASTER_MAX_STEPS;   /* also NaN */
    else {
        f = ceilf(sqrtf(dev / RASTER_TOLERANCE));
        n = f < 1.0f ? 1 : f > (float)RASTER_MAX_STEPS ? RASTER_MAX_STEPS : (int)f;
    }
    if (n > *budget) n = *budget > 1 ? (int)*budget : 1;
    *budget -= n;
    return n;
}

void fa_raster_path(fa_raster* r, const fa_path* p, float sx, float sy, float dx, float dy)
{
    float cx = 0, cy = 0, mx = 0, my = 0;
    int open = 0;
    long budget = RASTER_MAX_SEGMENTS, cells = RASTER_MAX_CELLS;
    size_t i;
    for (i = 0; i < p->count; i++) {
        const fa_path_cmd* c = &p->cmd[i];
        float x = c->x * sx + dx, y = c->y * sy + dy;
        switch (c->op) {
        case FA_PATH_MOVE:
            if (open && !path_line(r, &cells, cx, cy, mx, my)) return;
            mx = cx = x;
            my = cy = y;
            open = 1;
            continue;
        case FA_PATH_LINE:
            if (!path_line(r, &cells, cx, cy, x, y)) return;
            break;
        case FA_PATH_QUAD: {
            float x1 = c->x1 * sx + dx, y1 = c->y1 * sy + dy;
            float ex = cx - 2 * x1 + x, ey = cy - 2 * y1 + y;
            int n = steps_for(0.25f * sqrtf(ex * ex + ey * ey), &budget), k;
            float px = cx, py = cy;
            for (k = 1; k <= n; k++) {
                float t = (float)k / (float)n, u = 1 - t;
                float qx = u * u * cx + 2 * u * t * x1 + t * t * x, qy = u * u * cy + 2 * u * t * y1 + t * t * y;
                if (k == n) { qx = x; qy = y; }
                if (!path_line(r, &cells, px, py, qx, qy)) return;
                px = qx;
                py = qy;
            }
            break;
        }
        case FA_PATH_CUBIC: {
            float x1 = c->x1 * sx + dx, y1 = c->y1 * sy + dy, x2 = c->x2 * sx + dx, y2 = c->y2 * sy + dy;
            float ax = cx - 2 * x1 + x2, ay = cy - 2 * y1 + y2, bx = x1 - 2 * x2 + x, by = y1 - 2 * y2 + y;
            float da = sqrtf(ax * ax + ay * ay), db = sqrtf(bx * bx + by * by);
            int n = steps_for(0.75f * (da > db ? da : db), &budget), k;
            float px = cx, py = cy;
            for (k = 1; k <= n; k++) {
                float t = (float)k / (float)n, u = 1 - t;
                float qx = u * u * u * cx + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x;
                float qy = u * u * u * cy + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y;
                if (k == n) { qx = x; qy = y; }
                if (!path_line(r, &cells, px, py, qx, qy)) return;
                px = qx;
                py = qy;
            }
            break;
        }
        default:
            break;
        }
        cx = x;
        cy = y;
    }
    if (open) path_line(r, &cells, cx, cy, mx, my);
}

void fa_raster_end(fa_raster* r, uint8_t* out, size_t stride)
{
    int x, y;
    for (y = 0; y < r->h; y++) {
        const float* row = r->acc + (size_t)y * (size_t)(r->w + 2);
        uint8_t* o = out + (size_t)y * stride;
        float sum = 0.0f;
        for (x = 0; x < r->w; x++) {
            float a;
            sum += row[x];
            a = fabsf(sum);
            o[x] = (uint8_t)(a >= 1.0f ? 255 : (int)(a * 255.0f + 0.5f));
        }
    }
}

void fa_raster_free(fa_raster* r)
{
    free(r->acc);
    memset(r, 0, sizeof(*r));
}
