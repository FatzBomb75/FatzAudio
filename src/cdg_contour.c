/* Contour mode: see cdg_contour.h.
 *
 * Tracing. The edges between CD+G pixels of different colours ("cracks") form a
 * graph on the pixel corners. A corner where one or three or four cracks meet ends a
 * chain; elsewhere a chain goes on. A corner with four cracks and two colours meeting
 * diagonally (a checkerboard X) is resolved: the colour that is scarcer in the 4x4
 * pixels around it connects across it (as a one-pixel diagonal line does), and the
 * chains turn around the other two pixels; when both are as common it stays an end.
 * A chain keeps one colour on its left and one on its right.
 *
 * Smoothing, on each chain's straight runs (pixel edges in one direction) and the
 * turns between them, by local rules only (so a change reaches a bounded distance):
 *  5: a "riser" is a run of length 1 whose two turns go opposite ways (one step of a
 *     staircase). Its two corners are cut: the line goes from half a pixel before it,
 *     through its middle, to half a pixel after it; a staircase of risers becomes a
 *     45-degree line. Every other corner stays.
 *  6: also, a run of 2-4 pixels between two risers (a shallow or steep staircase) is
 *     dropped, so the line goes from riser middle to riser middle along the slope;
 *     where a staircase ends, the line meets the run after it at the slope's own
 *     distance (half the run on the other side of the riser, up to 2 pixels).
 *  7, 8: also, every remaining corner is rounded (a circle arc, radius up to 0.5 and
 *     1 pixel), except a right-angle corner whose both runs are at least 2 (7) or 3
 *     (8) pixels long.
 *  9, 10: the level-6 outline is fitted with curves, after Selinger's published
 *     potrace smoothing: at each vertex a Bezier curve between points on its two legs
 *     (up to 2 pixels away), its control points set by how sharply the outline turns
 *     there. Right-angle corners with both runs at least 4 (9) or 3 (10) pixels long
 *     stay corners.
 * The ends of a chain do not move, and every point stays on the traced pixel edges or
 * within half a pixel of them, so neighbouring chains do not cross.
 *
 * Drawing. Every outline segment is drawn with exact-area coverage (as raster.c) into
 * a running sum along each output row, weighted by the difference between its two
 * colours (premultiplied), in fixed point: the sum is exact, the same in whatever
 * order the segments come, and each segment's part depends only on where it lies
 * relative to its own anchor corner, so a picture moved by whole pixels (a scroll)
 * draws the same pixels. A CD+G pixel that no segment touches is its own colour; the
 * sum starts there again (or at the screen's left edge, with the colour of the row's
 * first pixel), so a pixel's value depends only on the segments near it. */
#include "cdg_contour.h"
#include "raster.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define W CDG_SCALE_W
#define H CDG_SCALE_H
#define FIX 65536.0f                  /* coverage fixed point */

enum { K_FIXED, K_CORNER, K_CUT, K_MID };

typedef struct ct_seg {
    int16_t ax, ay;                   /* anchor corner */
    int16_t cx0, cx1;                 /* cells it touches, x */
    uint8_t l, r;                     /* colours left and right of (x0, y0) -> (x1, y1) */
    float x0, y0, x1, y1;             /* CD+G pixels from the anchor */
} ct_seg;

typedef struct ct_pt {
    int16_t ax, ay;
    float x, y;                       /* from the anchor */
    uint8_t kind;
    uint8_t legs;                     /* K_CORNER: the shorter run, up to 255 */
} ct_pt;

struct cdg_contours {
    int level;
    uint32_t n;
    ct_seg* seg;
    size_t nseg, seg_cap;
    uint32_t row_start[H + 1];
    uint32_t* row_idx;
    size_t idx_cap;
    uint8_t busy[H][W];
    /* Cracks: bit 0 there, bit 1 traced. hc[y][x + 1] is (x, y) -> (x + 1, y) and
     * vc[y + 1][x] is (x, y) -> (x, y + 1), with a row or column of none round them. */
    uint8_t hc[H + 1][W + 2];
    uint8_t vc[H + 2][W + 1];
    uint8_t* dirs;                    /* one chain's steps */
    size_t dir_cap;
    uint8_t* rdir;                    /* its runs */
    uint16_t* rlen;
    uint8_t* rflag;
    size_t run_cap, rlen_cap, rflag_cap;
    ct_pt* pt;
    size_t pt_cap;
    int oom;
};

static const int DX[4] = { 1, 0, -1, 0 }, DY[4] = { 0, 1, 0, -1 };

int cdg_contour_reach(int level)
{
    return level >= 9 ? 16 : level >= 7 ? 11 : level >= 6 ? 10 : 6;
}

static int grow(void** p, size_t* cap, size_t need, size_t elem)
{
    size_t c = *cap ? *cap : 256;
    void* q;
    if (need <= *cap) return 1;
    while (c < need) c *= 2;
    q = realloc(*p, c * elem);
    if (!q) return 0;
    *p = q;
    *cap = c;
    return 1;
}

/* ---- tracing ------------------------------------------------------------------------ */

typedef struct tracer {
    cdg_contours* ct;
    const uint8_t (*s)[W];
} tracer;

/* The crack from corner (x, y) in direction d (0 <= x <= W, 0 <= y <= H). */
static uint8_t* cb(cdg_contours* ct, int x, int y, int d)
{
    switch (d) {
    case 0: return &ct->hc[y][x + 1];
    case 2: return &ct->hc[y][x];
    case 1: return &ct->vc[y + 1][x];
    default: return &ct->vc[y][x];
    }
}

static int crack(const tracer* t, int x, int y, int d) { return *cb(t->ct, x, y, d) & 1; }
static uint8_t* seen_at(cdg_contours* ct, int x, int y, int d) { return cb(ct, x, y, d); }

/* Colours left and right of a step from (x, y) in direction d (y down). */
static void sides(const tracer* t, int x, int y, int d, uint8_t* l, uint8_t* r)
{
    const uint8_t(*s)[W] = t->s;
    switch (d) {
    case 0: *l = s[y - 1][x]; *r = s[y][x]; break;
    case 2: *l = s[y][x - 1]; *r = s[y - 1][x - 1]; break;
    case 1: *l = s[y][x]; *r = s[y][x - 1]; break;
    default: *l = s[y - 1][x - 1]; *r = s[y - 1][x]; break;
    }
}

static int degree(const tracer* t, int x, int y)
{
    const cdg_contours* ct = t->ct;
    return (ct->hc[y][x + 1] & 1) + (ct->hc[y][x] & 1) + (ct->vc[y + 1][x] & 1) + (ct->vc[y][x] & 1);
}

/* A corner with four cracks: the direction to go on in after arriving with
 * direction `in`, or -1 when it ends chains. */
static int x_through(const tracer* t, int x, int y, int in)
{
    const uint8_t(*s)[W] = t->s;
    uint8_t tl = s[y - 1][x - 1], tr = s[y - 1][x], bl = s[y][x - 1], br = s[y][x];
    int main_connects;   /* 1: TL-BR connect (the cracks turn around TR and BL) */
    if (tl == br && tr != bl) main_connects = 1;
    else if (tr == bl && tl != br) main_connects = 0;
    else if (tl == br && tr == bl) {
        int na = 0, nb = 0, i, j;
        for (j = y - 2; j < y + 2; j++)
            for (i = x - 2; i < x + 2; i++) {
                uint8_t c;
                if (i < 0 || j < 0 || i >= W || j >= H) continue;
                c = s[j][i];
                na += c == tl;
                nb += c == tr;
            }
        if (na == nb) return -1;
        main_connects = na < nb;
    } else {
        return -1;
    }
    /* Arriving with `in` means coming along the crack on side (in + 2). Around TR:
     * up (3) <-> right (0); around BL: down (1) <-> left (2); around TL: up <-> left;
     * around BR: right <-> down. */
    {
        int from = (in + 2) & 3;
        if (main_connects) {
            static const int pair[4] = { 3, 2, 1, 0 };   /* 0<->3, 1<->2 */
            return pair[from];
        } else {
            static const int pair[4] = { 1, 0, 3, 2 };   /* 0<->1, 2<->3 */
            return pair[from];
        }
    }
}

/* Ends a chain at corner (x, y) arrived at with direction `in`: -1, or the next
 * direction. */
static int next_dir(const tracer* t, int x, int y, int in)
{
    int deg = degree(t, x, y), d;
    if (deg == 2) {
        for (d = 0; d < 4; d++)
            if (d != ((in + 2) & 3) && crack(t, x, y, d)) return d;
        return -1;
    }
    if (deg == 4) return x_through(t, x, y, in);
    return -1;
}

/* 1 when corner (x, y) starts chains (a crack from it is not passed through). */
static int is_end(const tracer* t, int x, int y)
{
    int deg = degree(t, x, y);
    if (deg == 0 || deg == 2) return 0;
    if (deg == 4) return x_through(t, x, y, 0) < 0;
    return 1;
}

static void chain(tracer* t, int x0, int y0, int d0);

/* ---- smoothing ----------------------------------------------------------------------- */

typedef struct chain_view {
    int m, closed, level;
    const uint8_t* dir;
    const uint16_t* len;
    const uint8_t* flag;              /* per run: 1 riser, 2 skipped (once worked out) */
} chain_view;

static int run_ok(const chain_view* c, int i) { return c->closed || (i >= 0 && i < c->m); }
/* Run index i (at most m away from 0..m-1) on a loop. */
static int ri(const chain_view* c, int i) { return !c->closed ? i : i < 0 ? i + c->m : i >= c->m ? i - c->m : i; }

/* Turn at joint j (between run j - 1 and run j): +1, -1, 0 at a chain's end. */
static int turn(const chain_view* c, int j)
{
    int a, b, da, db;
    if (!c->closed && (j <= 0 || j >= c->m)) return 0;
    a = ri(c, j - 1);
    b = ri(c, j);
    da = c->dir[a];
    db = c->dir[b];
    return DX[da] * DY[db] - DY[da] * DX[db];
}

static int zrun(const chain_view* c, int i)
{
    int a, b;
    if (!run_ok(c, i)) return 0;
    a = turn(c, i);
    b = turn(c, i + 1);
    return a != 0 && b != 0 && a == -b;
}

static int riser_of(const chain_view* c, int i) { return zrun(c, i) && c->len[ri(c, i)] == 1; }
static int riser(const chain_view* c, int i) { return run_ok(c, i) && (c->flag[ri(c, i)] & 1); }

static int skip_of(const chain_view* c, int i)
{
    int len;
    if (c->level < 6 || !zrun(c, i)) return 0;
    len = c->len[ri(c, i)];
    return len >= 2 && len <= 4 && riser(c, i - 1) && riser(c, i + 1);
}

/* Distance from a riser's corner at which the line meets run `r`; `o` is the run on
 * the riser's other side. */
static int skip(const chain_view* c, int i) { return run_ok(c, i) && (c->flag[ri(c, i)] & 2); }

static float meet(const chain_view* c, int r, int o)
{
    float lr = (float)c->len[ri(c, r)], lo = run_ok(c, o) ? (float)c->len[ri(c, o)] : lr, e;
    if (c->level < 6) return 0.5f;
    e = lr < lo ? lr : lo;
    if (e > 4.0f) e = 4.0f;
    e *= 0.5f;
    if (e < 0.5f) e = 0.5f;
    if (e > 0.5f * lr) e = 0.5f * lr;
    return e;
}

/* Points are kept as the corner at or before them and an offset in [0, 1) (exact:
 * multiples of a half), so a point is stored the same whichever way its chain was
 * traced; segments and curves are made in one order whichever way they are met (see
 * add_seg and outline). A chain's direction depends on where it was found, which a
 * change far away can alter, so this keeps the drawing local. */
static void emit(cdg_contours* ct, size_t* np, int ax, int ay, float x, float y, int kind, int legs)
{
    ct_pt* p;
    float fx = floorf(x), fy = floorf(y);
    if (!grow((void**)&ct->pt, &ct->pt_cap, *np + 1, sizeof(ct_pt))) { ct->oom = 1; return; }
    ax += (int)fx;
    ay += (int)fy;
    x -= fx;
    y -= fy;
    p = &ct->pt[(*np)++];
    p->ax = (int16_t)ax;
    p->ay = (int16_t)ay;
    p->x = x;
    p->y = y;
    p->kind = (uint8_t)kind;
    p->legs = (uint8_t)(legs > 255 ? 255 : legs);
}

/* 1 when point a comes before b (by anchor, then offset). */
static int pt_less(const ct_pt* a, const ct_pt* b)
{
    if (a->ay != b->ay) return a->ay < b->ay;
    if (a->ax != b->ax) return a->ax < b->ax;
    if (a->y != b->y) return a->y < b->y;
    return a->x < b->x;
}

/* The segment a -> b (colour l on its left), stored from the earlier of the two. */
static void add_seg(cdg_contours* ct, const ct_pt* a, const ct_pt* b, uint8_t l, uint8_t r)
{
    ct_seg* s;
    const ct_pt* p = a;
    float px, py, x1, y1, lo, hi;
    if (pt_less(b, a)) {
        uint8_t t = l;
        p = b;
        b = a;
        l = r;
        r = t;
    }
    px = p->x;
    py = p->y;
    x1 = (float)(b->ax - p->ax) + b->x;
    y1 = (float)(b->ay - p->ay) + b->y;
    if (px == x1 && py == y1) return;
    if (!grow((void**)&ct->seg, &ct->seg_cap, ct->nseg + 1, sizeof(ct_seg))) { ct->oom = 1; return; }
    s = &ct->seg[ct->nseg++];
    s->ax = p->ax;
    s->ay = p->ay;
    s->x0 = px;
    s->y0 = py;
    s->x1 = x1;
    s->y1 = y1;
    s->l = l;
    s->r = r;
    lo = px < x1 ? px : x1;
    hi = px < x1 ? x1 : px;
    s->cx0 = (int16_t)(p->ax + (int)floorf(lo));
    s->cx1 = (int16_t)(p->ax + (int)floorf(hi));
    if (s->cx0 < 0) s->cx0 = 0;
    if (s->cx1 > W - 1) s->cx1 = W - 1;
}

/* The outline through points pt[0..np) (closed: back to pt[0]), rounded or fitted with
 * curves at its vertices for levels 7-10, as segments. */
static void outline(cdg_contours* ct, size_t np, int closed, uint8_t l, uint8_t r)
{
    int level = ct->level;
    size_t k;
    ct_pt cur, start;                 /* where the outline has got to, and began */
    int have = 0;
    float n = (float)ct->n;
    for (k = 0; k < np; k++) {
        const ct_pt* q = &ct->pt[k];
        const ct_pt *pp, *pn;
        float ux, uy, vx, vy, lu, lv, cross, h, alpha, kx;
        int sharp = level < 7 || q->kind == K_FIXED || (!closed && (k == 0 || k + 1 == np)) || np < 3;
        if (!sharp) {
            pp = &ct->pt[k == 0 ? np - 1 : k - 1];
            pn = &ct->pt[k + 1 == np ? 0 : k + 1];
            ux = (float)(pp->ax - q->ax) + pp->x - q->x;
            uy = (float)(pp->ay - q->ay) + pp->y - q->y;
            vx = (float)(pn->ax - q->ax) + pn->x - q->x;
            vy = (float)(pn->ay - q->ay) + pn->y - q->y;
            lu = sqrtf(ux * ux + uy * uy);
            lv = sqrtf(vx * vx + vy * vy);
            cross = ux * vy - uy * vx;
            if (lu < 1e-6f || lv < 1e-6f || fabsf(cross) < 1e-6f * lu * lv) sharp = 1;
            if (q->kind == K_CORNER && q->legs >= (level == 7 ? 2 : level == 8 ? 3 : level == 9 ? 4 : 3)) sharp = 1;
        }
        if (sharp) {
            if (have) add_seg(ct, &cur, q, l, r);
            else start = *q;
            cur = *q;
            have = 1;
            continue;
        }
        ux /= lu; uy /= lu; vx /= lv; vy /= lv;
        if (level <= 8) {
            float rmax = level == 7 ? 0.5f : 1.0f;
            h = 0.5f * (lu < lv ? lu : lv);
            if (h > rmax) h = rmax;
            alpha = 0.55f;
        } else {
            /* potrace: dd is the vertex's distance from the line between its
             * neighbours (legs taken up to 4 pixels), over that line's L1 length. */
            float cu = lu < 4.0f ? lu : 4.0f, cv = lv < 4.0f ? lv : 4.0f;
            float px = ux * cu, py = uy * cu, nx = vx * cv, ny = vy * cv;
            float dd = fabsf(px * ny - py * nx) / (fabsf(nx - px) + fabsf(ny - py));
            alpha = dd > 1.0f ? (1.0f - 1.0f / dd) / 0.75f : 0.0f;
            if (alpha < 0.55f) alpha = 0.55f;
            if (alpha > 1.0f) alpha = 1.0f;
            h = 0.5f * (lu < lv ? lu : lv);
            if (h > 2.0f) h = 2.0f;
        }
        {
            /* Bezier from A = Q + h u to B = Q + h v, controls at h (1 - alpha), worked
             * out from the earlier neighbour's side whichever way the chain runs. */
            ct_pt pts[9];
            int sw = pt_less(pn, pp), steps, i;
            float c = h * (1.0f - alpha), x0, y0, x1, y1, x2, y2, x3, y3, t;
            if (sw) {
                t = ux; ux = vx; vx = t;
                t = uy; uy = vy; vy = t;
            }
            x0 = q->x + ux * h; y0 = q->y + uy * h;
            x1 = q->x + ux * c; y1 = q->y + uy * c;
            x2 = q->x + vx * c; y2 = q->y + vy * c;
            x3 = q->x + vx * h; y3 = q->y + vy * h;
            kx = h * n * 0.25f;
            steps = kx < 2.0f ? 2 : kx > 8.0f ? 8 : (int)ceilf(kx);
            for (i = 0; i <= steps; i++) {
                float tt = (float)i / (float)steps, u = 1.0f - tt;
                pts[i] = *q;
                pts[i].kind = K_CUT;
                if (i == 0) {
                    pts[i].x = x0;
                    pts[i].y = y0;
                } else if (i == steps) {
                    pts[i].x = x3;
                    pts[i].y = y3;
                } else {
                    pts[i].x = u * u * u * x0 + 3 * u * u * tt * x1 + 3 * u * tt * tt * x2 + tt * tt * tt * x3;
                    pts[i].y = u * u * u * y0 + 3 * u * u * tt * y1 + 3 * u * tt * tt * y2 + tt * tt * tt * y3;
                }
            }
            for (i = 0; i <= steps; i++) {
                const ct_pt* b = &pts[sw ? steps - i : i];
                if (i == 0) {
                    if (have) add_seg(ct, &cur, b, l, r);
                    else start = *b;
                    have = 1;
                } else {
                    add_seg(ct, &cur, b, l, r);
                }
                cur = *b;
            }
        }
    }
    if (closed && have) add_seg(ct, &cur, &start, l, r);
}

/* Smooths one traced chain (runs dir/len from corner (x0, y0)) into segments. */
static void smooth_chain(cdg_contours* ct, int x0, int y0, int m, int closed, uint8_t l, uint8_t r)
{
    chain_view c;
    size_t np = 0;
    int i, jx = x0, jy = y0;
    c.m = m;
    c.closed = closed;
    c.level = ct->level;
    c.dir = ct->rdir;
    c.len = ct->rlen;
    c.flag = ct->rflag;
    for (i = 0; i < m; i++) ct->rflag[i] = (uint8_t)riser_of(&c, i);
    for (i = 0; i < m; i++) ct->rflag[i] |= (uint8_t)(skip_of(&c, i) << 1);
    for (i = 0; i <= m; i++) {
        int last = i == m;
        if (closed && last) break;
        if (ct->level == 0 || (!closed && (i == 0 || last))) {
            emit(ct, &np, jx, jy, 0, 0, K_FIXED, 0);
        } else {
            int p = i - 1, rp = riser(&c, p), rn = riser(&c, i);
            int dp = c.dir[ri(&c, p)], dn = c.dir[ri(&c, i)];
            if (!rp && !rn) {
                int a = c.len[ri(&c, p)], b = c.len[ri(&c, i)];
                emit(ct, &np, jx, jy, 0, 0, K_CORNER, a < b ? a : b);
            } else {
                if (!rp && !skip(&c, p)) {
                    float e = meet(&c, p, i + 1);
                    emit(ct, &np, jx, jy, -e * (float)DX[dp], -e * (float)DY[dp], K_CUT, 0);
                }
                if (!rn && !skip(&c, i)) {
                    float e = meet(&c, i, p - 1);
                    emit(ct, &np, jx, jy, e * (float)DX[dn], e * (float)DY[dn], K_CUT, 0);
                }
            }
        }
        if (last) break;
        if (ct->level > 0 && riser(&c, i)) {
            int d = c.dir[i];
            emit(ct, &np, jx, jy, 0.5f * (float)DX[d], 0.5f * (float)DY[d], K_MID, 0);
        }
        jx += DX[c.dir[i]] * c.len[i];
        jy += DY[c.dir[i]] * c.len[i];
    }
    if (ct->oom) return;
    if (ct->level >= 7) {
        outline(ct, np, closed, l, r);
    } else {
        size_t k;
        for (k = 0; k + 1 < np; k++) add_seg(ct, &ct->pt[k], &ct->pt[k + 1], l, r);
        if (closed && np > 1) add_seg(ct, &ct->pt[np - 1], &ct->pt[0], l, r);
    }
}

/* Traces the chain from corner (x0, y0) in direction d0, marks its cracks and smooths
 * it. */
static void chain(tracer* t, int x0, int y0, int d0)
{
    cdg_contours* ct = t->ct;
    size_t ns = 0, i;
    int x = x0, y = y0, d = d0, closed, m, sx, sy, rot;
    uint8_t l, r;
    sides(t, x0, y0, d0, &l, &r);
    for (;;) {
        uint8_t* sn = seen_at(ct, x, y, d);
        if (*sn & 2) break;
        *sn |= 2;
        if (!grow((void**)&ct->dirs, &ct->dir_cap, ns + 1, 1)) { ct->oom = 1; return; }
        ct->dirs[ns++] = (uint8_t)d;
        x += DX[d];
        y += DY[d];
        d = next_dir(t, x, y, d);
        if (d < 0) break;
    }
    closed = d >= 0 && x == x0 && y == y0;
    /* A loop starts at a turn, so its first and last runs are not one run. */
    sx = x0;
    sy = y0;
    rot = 0;
    if (closed) {
        for (i = 0; i < ns && ct->dirs[i] == ct->dirs[(i + ns - 1) % ns]; i++) {}
        rot = (int)(i < ns ? i : 0);
        for (i = 0; i < (size_t)rot; i++) {
            sx += DX[ct->dirs[i]];
            sy += DY[ct->dirs[i]];
        }
    }
    if (!grow((void**)&ct->rdir, &ct->run_cap, ns + 1, 1) ||
        !grow((void**)&ct->rlen, &ct->rlen_cap, ns + 1, sizeof(uint16_t)) ||
        !grow((void**)&ct->rflag, &ct->rflag_cap, ns + 1, 1)) {
        ct->oom = 1;
        return;
    }
    m = 0;
    for (i = 0; i < ns; i++) {
        uint8_t dd = ct->dirs[(i + (size_t)rot) % ns];
        if (m > 0 && ct->rdir[m - 1] == dd) {
            ct->rlen[m - 1]++;
        } else {
            ct->rdir[m] = dd;
            ct->rlen[m] = 1;
            m++;
        }
    }
    if (closed) {
        /* The colours as seen from the rotated start. */
        sides(t, sx, sy, ct->rdir[0], &l, &r);
    }
    smooth_chain(ct, sx, sy, m, closed, l, r);
}

/* ---- build ---------------------------------------------------------------------------- */

int cdg_contours_build(cdg_contours** pct, const uint8_t screen[CDG_SCALE_H][CDG_SCALE_W], int level, uint32_t n)
{
    cdg_contours* ct = *pct;
    tracer t;
    size_t i, total;
    int x, y, d;
    if (!ct) {
        ct = (cdg_contours*)calloc(1, sizeof(*ct));
        if (!ct) return 0;
        *pct = ct;
    }
    ct->level = level;
    ct->n = n;
    ct->nseg = 0;
    ct->oom = 0;
    memset(ct->hc, 0, sizeof(ct->hc));
    memset(ct->vc, 0, sizeof(ct->vc));
    for (y = 1; y < H; y++)
        for (x = 0; x < W; x++) ct->hc[y][x + 1] = screen[y - 1][x] != screen[y][x];
    for (y = 0; y < H; y++)
        for (x = 1; x < W; x++) ct->vc[y + 1][x] = screen[y][x - 1] != screen[y][x];
    t.ct = ct;
    t.s = screen;
    /* Chains with ends first, then loops (each from its first crack in scan order). */
    for (y = 0; y <= H; y++)
        for (x = 0; x <= W; x++) {
            if (!((ct->hc[y][x + 1] | ct->hc[y][x] | ct->vc[y + 1][x] | ct->vc[y][x]) & 1) || !is_end(&t, x, y)) continue;
            for (d = 0; d < 4; d++) {
                uint8_t c = *cb(ct, x, y, d);
                if ((c & 3) == 1) chain(&t, x, y, d);
            }
        }
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            if ((ct->hc[y][x + 1] & 3) == 1) chain(&t, x, y, 0);
            if ((ct->vc[y + 1][x] & 3) == 1) chain(&t, x, y, 1);
        }
    if (ct->oom) return 0;
    /* Busy cells and the segments of each row. */
    memset(ct->busy, 0, sizeof(ct->busy));
    memset(ct->row_start, 0, sizeof(ct->row_start));
    for (i = 0; i < ct->nseg; i++) {
        const ct_seg* s = &ct->seg[i];
        float lo = s->y0 < s->y1 ? s->y0 : s->y1, hi = s->y0 < s->y1 ? s->y1 : s->y0;
        int r0 = s->ay + (int)floorf(lo), r1 = s->ay + (int)ceilf(hi) - 1, r, c;
        if (lo == hi) {
            if (lo == floorf(lo)) continue;   /* on a row boundary: no effect */
            r1 = r0;
        } else {
            int a = r0 < 0 ? 0 : r0, b = r1 > H - 1 ? H - 1 : r1;
            for (r = a; r <= b; r++) ct->row_start[r + 1]++;
        }
        if (r0 < 0) r0 = 0;
        if (r1 > H - 1) r1 = H - 1;
        for (r = r0; r <= r1; r++)
            for (c = s->cx0; c <= s->cx1; c++) ct->busy[r][c] = 1;
    }
    for (y = 0; y < H; y++) ct->row_start[y + 1] += ct->row_start[y];
    total = ct->row_start[H];
    if (!grow((void**)&ct->row_idx, &ct->idx_cap, total + 1, sizeof(uint32_t))) return 0;
    {
        uint32_t fill[H];
        memcpy(fill, ct->row_start, sizeof(fill));
        for (i = 0; i < ct->nseg; i++) {
            const ct_seg* s = &ct->seg[i];
            float lo = s->y0 < s->y1 ? s->y0 : s->y1, hi = s->y0 < s->y1 ? s->y1 : s->y0;
            int r0 = s->ay + (int)floorf(lo), r1 = s->ay + (int)ceilf(hi) - 1, r;
            if (lo == hi) continue;
            if (r0 < 0) r0 = 0;
            if (r1 > H - 1) r1 = H - 1;
            for (r = r0; r <= r1; r++) ct->row_idx[fill[r]++] = (uint32_t)i;
        }
    }
    return 1;
}

void cdg_contours_free(cdg_contours* ct)
{
    if (!ct) return;
    free(ct->seg);
    free(ct->row_idx);
    free(ct->dirs);
    free(ct->rdir);
    free(ct->rlen);
    free(ct->rflag);
    free(ct->pt);
    free(ct);
}

/* ---- drawing -------------------------------------------------------------------------- */

typedef struct row_ctx {
    int32_t* acc;
    int limit;                        /* pixels from the run's start */
    int lo, hi;                       /* pixels written */
} row_ctx;

static void put(row_ctx* rc, int i, float v, const int32_t delta[4])
{
    int32_t q, *a;
    if (i < 0 || i >= rc->limit) return;
    q = (int32_t)lrintf(v * FIX);
    if (!q) return;
    a = rc->acc + (size_t)i * 4;
    a[0] += q * delta[0];
    a[1] += q * delta[1];
    a[2] += q * delta[2];
    a[3] += q * delta[3];
    if (i < rc->lo) rc->lo = i;
    if (i > rc->hi) rc->hi = i;
}

/* One segment's part in one output row: from x = a to b (output pixels from pixel
 * `base`), signed height d. As raster.c's row_edge. */
static void row_edge(row_ctx* rc, int base, float a, float b, float d, const int32_t delta[4])
{
    float lo = a < b ? a : b, hi = a < b ? b : a, width = hi - lo, prev = 0.0f;
    int i0 = (int)floorf(lo), i1 = (int)ceilf(hi), i;
    if (i1 <= i0) i1 = i0 + 1;
    for (i = i0; i <= i1; i++) {
        float cov = fa_edge_cover(i, lo, hi, width);
        put(rc, base + i, d * (cov - prev), delta);
        prev = cov;
    }
    if (prev < 1.0f) put(rc, base + i, d * (1.0f - prev), delta);
}

static uint8_t clamp8(int32_t v)
{
    v = (v + 32768) >> 16;
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

void cdg_contours_draw(const cdg_contours* ct, const uint8_t screen[CDG_SCALE_H][CDG_SCALE_W],
                       const uint8_t pm[CDG_SCALE_COLORS][4], uint8_t* out, size_t out_stride, int ox, int oy,
                       int sx0, int sy0, int sx1, int sy1, int32_t* acc)
{
    int n = (int)ct->n, r, c, j, k;
    if (sx0 < 0) sx0 = 0;
    if (sy0 < 0) sy0 = 0;
    if (sx1 > W) sx1 = W;
    if (sy1 > H) sy1 = H;
    if (sx0 >= sx1) return;
    /* The scratch is shared with smoothness 1-4: clear what a row can use (each row
     * clears what it wrote after it). */
    memset(acc, 0, (size_t)sx1 * (size_t)n * 4 * sizeof(int32_t));
    for (r = sy0; r < sy1; r++) {
        int s = sx0, any = 0;
        uint32_t a, b = ct->row_start[r + 1];
        row_ctx rc;
        for (c = sx0; c < sx1 && !any; c++) any = ct->busy[r][c];
        if (!any) {
            /* No outline here: blocks. */
            for (j = 0; j < n; j++) {
                uint8_t* o = out + ((size_t)(r - oy) * (size_t)n + (size_t)j) * out_stride + (size_t)(sx0 - ox) * (size_t)n * 4;
                for (c = sx0; c < sx1; c++)
                    for (k = 0; k < n; k++, o += 4) memcpy(o, pm[screen[r][c]], 4);
            }
            continue;
        }
        while (s > 0 && ct->busy[r][s]) s--;
        rc.acc = acc;
        rc.limit = (sx1 - s) * n;
        for (j = 0; j < n; j++) {
            uint8_t* o = out + ((size_t)(r - oy) * (size_t)n + (size_t)j) * out_stride;
            int32_t run[4];
            rc.lo = rc.limit;
            rc.hi = -1;
            for (a = ct->row_start[r]; a < b; a++) {
                const ct_seg* g = &ct->seg[ct->row_idx[a]];
                float x0 = g->x0 * (float)n, x1 = g->x1 * (float)n;
                float y0 = ((float)(g->ay - r) + g->y0) * (float)n, y1 = ((float)(g->ay - r) + g->y1) * (float)n;
                float dir = 1.0f, ya, yb, xa, xb;
                int32_t delta[4];
                if (g->cx1 < s || g->cx0 >= sx1) continue;
                if (y0 > y1) {
                    float tt;
                    tt = x0; x0 = x1; x1 = tt;
                    tt = y0; y0 = y1; y1 = tt;
                    dir = -1.0f;
                }
                ya = (float)j > y0 ? (float)j : y0;
                yb = (float)(j + 1) < y1 ? (float)(j + 1) : y1;
                if (yb <= ya) continue;
                xa = x0 + (x1 - x0) * ((ya - y0) / (y1 - y0));
                xb = x0 + (x1 - x0) * ((yb - y0) / (y1 - y0));
                for (k = 0; k < 4; k++) delta[k] = (int32_t)pm[g->l][k] - (int32_t)pm[g->r][k];
                row_edge(&rc, (g->ax - s) * n, xa, xb, dir * (yb - ya), delta);
            }
            for (k = 0; k < 4; k++) run[k] = (int32_t)pm[screen[r][s]][k] << 16;
            for (c = s; c < sx1; c++) {
                uint8_t* q = o + (size_t)(c - ox) * (size_t)n * 4;
                if (!ct->busy[r][c]) {
                    const uint8_t* col = pm[screen[r][c]];
                    for (k = 0; k < 4; k++) run[k] = (int32_t)col[k] << 16;
                    if (c >= sx0)
                        for (k = 0; k < n; k++) memcpy(q + (size_t)k * 4, col, 4);
                    continue;
                }
                for (k = 0; k < n; k++) {
                    const int32_t* v = acc + ((size_t)(c - s) * (size_t)n + (size_t)k) * 4;
                    run[0] += v[0];
                    run[1] += v[1];
                    run[2] += v[2];
                    run[3] += v[3];
                    if (c >= sx0) {
                        q[k * 4 + 0] = clamp8(run[0]);
                        q[k * 4 + 1] = clamp8(run[1]);
                        q[k * 4 + 2] = clamp8(run[2]);
                        q[k * 4 + 3] = clamp8(run[3]);
                    }
                }
            }
            if (rc.hi >= rc.lo) memset(acc + (size_t)rc.lo * 4, 0, (size_t)(rc.hi - rc.lo + 1) * 4 * sizeof(int32_t));
        }
    }
}
