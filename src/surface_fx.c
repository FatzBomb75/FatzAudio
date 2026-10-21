/* Surface effects: the shape's distance field and what is drawn from it.
 *
 * The shape is the bitmap's alpha. Distances come from an exact Euclidean distance
 * transform: the vertical distance to the shape by a sweep down and one up the
 * window (row by row, so memory is read in order), then along each row the lower
 * envelope of parabolas (Felzenszwalb and Huttenlocher). It keeps the nearest pixel
 * of the shape, so the distance is corrected by that pixel's coverage: an edge pixel a
 * third covered puts the edge a sixth of a pixel past its centre, not half a pixel.
 * Distances beyond the effects' reach are not needed, so vertical distances past it
 * count as none, and rows far from the shape cost almost nothing. Outside the shape
 * the field gives the outline and (at an offset) the shadow; inside, its gradient
 * gives the bevel. A rectangle is drawn in tiles, each over a window grown by the
 * reach, so the scratch stays small. */
#include "surface_fx.h"

#include <math.h>
#include <string.h>

#define FX_TILE 512
#define FX_FAR 1e18f              /* no shape pixel within reach */
#define FX_NONE 32767             /* vertical distance: none within reach */

static int unit(float v) { return v >= 0.0f && v <= 1.0f; }

int fa_fx_valid(const fa_surface_effects* e)
{
    const fa_fx_outline* o = &e->outline;
    const fa_fx_shadow* s = &e->shadow;
    const fa_fx_bevel* b = &e->bevel;
    return (o->on == 0 || o->on == 1) && unit(o->opacity) && unit(o->size) && unit(o->fade) &&
           (s->on == 0 || s->on == 1) && unit(s->opacity) && unit(s->size) && unit(s->distance) && unit(s->fade) &&
           isfinite(s->angle_degrees) && (b->on == 0 || b->on == 1) && unit(b->strength) && unit(b->size) &&
           unit(b->fade) && isfinite(b->light_angle_degrees);
}

/* 0xAARRGGBB, its alpha times `opacity`, premultiplied, in the buffer's order. */
static void color_pm(uint32_t c, float opacity, int rgba, float out[4])
{
    float a = (float)(c >> 24) / 255.0f * opacity;
    float r = (float)((c >> 16) & 0xFF) / 255.0f * a, g = (float)((c >> 8) & 0xFF) / 255.0f * a;
    float b = (float)(c & 0xFF) / 255.0f * a;
    out[0] = rgba ? r : b;
    out[1] = g;
    out[2] = rgba ? b : r;
    out[3] = a;
}

int fa_fx_plan_make(fa_fx_plan* p, const fa_surface_effects* e, float outline, float shadow_size,
                    float shadow_distance, float bevel, int rgba)
{
    const double rad = 3.14159265358979323846 / 180.0;
    float w = e->outline.size * outline, reach = 0.0f;
    memset(p, 0, sizeof(*p));
    p->outline = e->outline.on;
    p->shadow = e->shadow.on;
    p->bevel = e->bevel.on && e->bevel.strength > 0.0f;
    if (!p->outline && !p->shadow && !p->bevel) return 0;
    /* Solid to size * (1 - fade), eased out to size; half a pixel either side
     * anti-aliases a hard edge. */
    p->o_lo = w * (1.0f - e->outline.fade) - 0.5f;
    p->o_hi = w + 0.5f;
    color_pm(e->outline.color, e->outline.opacity, rgba, p->o_pm);
    if (p->outline) reach = p->o_hi;
    {
        float base = p->outline ? w : 0.0f, size = e->shadow.size * shadow_size;
        float dist = e->shadow.distance * shadow_distance, a = (float)(e->shadow.angle_degrees * rad);
        p->s_lo = base + size * (1.0f - e->shadow.fade) - 0.5f;
        p->s_hi = base + size + 0.5f;
        p->s_dx = dist * cosf(a);
        p->s_dy = -dist * sinf(a);
        color_pm(e->shadow.color, e->shadow.opacity, rgba, p->s_pm);
        /* The quick reject at the nearest pixel looks up to 2.5 pixels further. */
        if (p->shadow && p->s_hi + dist + 2.5f > reach) reach = p->s_hi + dist + 2.5f;
    }
    {
        float size = e->bevel.size * bevel, a = (float)(e->bevel.light_angle_degrees * rad);
        p->b_hi = size;
        p->b_lo = size * (1.0f - e->bevel.fade);
        if (p->b_hi - p->b_lo < 1.0f) p->b_lo = p->b_hi - 1.0f;
        p->b_lx = cosf(a);
        p->b_ly = -sinf(a);
        p->b_strength = e->bevel.strength;
        color_pm(e->bevel.highlight, 1.0f, rgba, p->b_hi_pm);
        color_pm(e->bevel.shade, 1.0f, rgba, p->b_sh_pm);
        if (p->bevel && p->b_hi + 1.0f > reach) reach = p->b_hi + 1.0f;
    }
    p->reach = (int)ceilf(reach) + 2;
    return 1;
}

static int window_side(const fa_fx_plan* p) { return FX_TILE + 2 * p->reach; }

size_t fa_fx_scratch_bytes(const fa_fx_plan* p)
{
    size_t n = (size_t)window_side(p), planes = (size_t)(p->bevel ? 2 : 1);
    /* g and cv, the distance planes, and the line buffers. */
    return n * n * (sizeof(int16_t) + 1) + planes * n * n * sizeof(float) +
           n * (3 * sizeof(float) + sizeof(int16_t) + sizeof(int) + 1) + 2 * sizeof(float) + 64;
}

typedef struct fx_scratch {
    int16_t* g;                   /* vertical distance to the shape (FX_NONE: beyond reach) */
    uint8_t* cv;                  /* and that shape pixel's coverage */
    float* dout;                  /* distance outside the shape (negative inside) */
    float* din;                   /* distance inside it (bevel only) */
    float *f, *dd, *z;
    int16_t* arg;
    int* v;
    uint8_t* rows;                /* rows of the window the inside field is wanted for */
} fx_scratch;

static void* take(uint8_t** at, size_t bytes)
{
    void* p = *at;
    *at += (bytes + 7) & ~(size_t)7;
    return p;
}

static void scratch_split(const fa_fx_plan* p, void* mem, fx_scratch* s)
{
    size_t n = (size_t)window_side(p);
    uint8_t* at = (uint8_t*)mem;
    s->g = (int16_t*)take(&at, n * n * sizeof(int16_t));
    s->cv = (uint8_t*)take(&at, n * n);
    s->dout = (float*)take(&at, n * n * sizeof(float));
    s->din = p->bevel ? (float*)take(&at, n * n * sizeof(float)) : NULL;
    s->f = (float*)take(&at, n * sizeof(float));
    s->dd = (float*)take(&at, n * sizeof(float));
    s->z = (float*)take(&at, (n + 1) * sizeof(float));
    s->arg = (int16_t*)take(&at, n * sizeof(int16_t));
    s->v = (int*)take(&at, n * sizeof(int));
    s->rows = (uint8_t*)take(&at, n);
}

/* One dimension: d[q] = min over sites j of (q - j)^2 + f[j] (f[j] >= FX_FAR: not a
 * site), arg[q] = that j; FX_FAR everywhere when there is none. */
static void edt_1d(const float* f, int n, float* d, int16_t* arg, int* v, float* z)
{
    int q, k = -1;
    for (q = 0; q < n; q++) {
        float s;
        if (f[q] >= FX_FAR) continue;
        if (k < 0) {
            k = 0;
            v[0] = q;
            z[0] = -FX_FAR;
            z[1] = FX_FAR;
            continue;
        }
        for (;;) {
            int r = v[k];
            s = ((f[q] + (float)q * (float)q) - (f[r] + (float)r * (float)r)) / (float)(2 * q - 2 * r);
            if (s > z[k]) break;
            k--;          /* z[0] is -FX_FAR: never below 0 */
        }
        k++;
        v[k] = q;
        z[k] = s;
        z[k + 1] = FX_FAR;
    }
    if (k < 0) {
        for (q = 0; q < n; q++) { d[q] = FX_FAR; arg[q] = 0; }
        return;
    }
    k = 0;
    for (q = 0; q < n; q++) {
        float t;
        while (z[k + 1] < (float)q) k++;
        t = (float)(q - v[k]);
        d[q] = t * t + f[v[k]];
        arg[q] = (int16_t)v[k];
    }
}

/* The distance field of the window (wn x hn at `a`, alpha bytes 4 apart) into `out`,
 * as far as `cap` pixels: inside = 0: the distance outside the shape, from the pixels
 * with any coverage; inside = 1: the distance inside it, from the pixels not fully
 * covered. Corrected by the nearest pixel's coverage; FX_FAR when there is none
 * within reach. `rows` (inside only): 1 for the rows wanted, NULL = all. */
static void field(const uint8_t* a, size_t stride, int wn, int hn, int inside, int cap, const uint8_t* rows,
                  fx_scratch* s, float* out)
{
    int x, y;
    /* Down, then up: the vertical distance to the nearest shape pixel, and its
     * coverage. */
    for (y = 0; y < hn; y++) {
        const uint8_t* ar = a + (size_t)y * stride;
        int16_t* g = s->g + (size_t)y * wn;
        uint8_t* cv = s->cv + (size_t)y * wn;
        const int16_t* gu = y ? g - wn : NULL;
        const uint8_t* cvu = y ? cv - wn : NULL;
        for (x = 0; x < wn; x++) {
            uint8_t c = ar[(size_t)x * 4];
            if (inside ? c < 255 : c > 0) {
                g[x] = 0;
                cv[x] = c;
            } else if (gu && gu[x] < cap) {
                g[x] = (int16_t)(gu[x] + 1);
                cv[x] = cvu[x];
            } else {
                g[x] = FX_NONE;
            }
        }
    }
    for (y = hn - 2; y >= 0; y--) {
        int16_t* g = s->g + (size_t)y * wn;
        uint8_t* cv = s->cv + (size_t)y * wn;
        const int16_t* gd = g + wn;
        const uint8_t* cvd = cv + wn;
        for (x = 0; x < wn; x++)
            if (gd[x] < cap && gd[x] + 1 < g[x]) {
                g[x] = (int16_t)(gd[x] + 1);
                cv[x] = cvd[x];
            }
    }
    /* Along each row: the nearest of those. */
    for (y = 0; y < hn; y++) {
        const int16_t* g = s->g + (size_t)y * wn;
        const uint8_t* cv = s->cv + (size_t)y * wn;
        float* o = out + (size_t)y * wn;
        int any = 0;
        if (rows && !rows[y]) continue;
        for (x = 0; x < wn; x++) {
            if (g[x] == FX_NONE) {
                s->f[x] = FX_FAR;
            } else {
                s->f[x] = (float)g[x] * (float)g[x];
                any = 1;
            }
        }
        if (!any) {
            for (x = 0; x < wn; x++) o[x] = FX_FAR;
            continue;
        }
        edt_1d(s->f, wn, s->dd, s->arg, s->v, s->z);
        for (x = 0; x < wn; x++) {
            float cov = (float)cv[s->arg[x]] * (1.0f / 255.0f);
            o[x] = sqrtf(s->dd[x]) + (inside ? cov - 0.5f : 0.5f - cov);
        }
    }
}

static float ease_out(float lo, float hi, float d)
{
    float t;
    if (d <= lo) return 1.0f;
    if (d >= hi) return 0.0f;
    t = (d - lo) / (hi - lo);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

/* The outside distance at (x + fx, y + fy), 0 <= fx, fy < 1, between pixels (beyond
 * the window: no shape). */
static float sample(const float* d, int wn, int hn, int x, int y, float fx, float fy)
{
    float v[4];
    int i;
    if (x >= 0 && y >= 0 && x + 1 < wn && y + 1 < hn) {
        const float* r = d + (size_t)y * wn + x;
        v[0] = r[0];
        v[1] = r[1];
        v[2] = r[wn];
        v[3] = r[wn + 1];
    } else {
        for (i = 0; i < 4; i++) {
            int xx = x + (i & 1), yy = y + (i >> 1);
            v[i] = xx < 0 || yy < 0 || xx >= wn || yy >= hn ? FX_FAR : d[(size_t)yy * wn + xx];
        }
    }
    if (v[0] >= FX_FAR || v[1] >= FX_FAR || v[2] >= FX_FAR || v[3] >= FX_FAR) {
        float m = v[0];
        for (i = 1; i < 4; i++)
            if (v[i] < m) m = v[i];
        return m >= FX_FAR ? FX_FAR : m + 1.0f;   /* at the edge of what is known: about */
    }
    return (v[0] * (1.0f - fx) + v[1] * fx) * (1.0f - fy) + (v[2] * (1.0f - fx) + v[3] * fx) * fy;
}

/* `layer` (premultiplied, alpha k) over `acc`. */
static void over(float acc[4], const float layer[4], float k)
{
    float keep = 1.0f - layer[3] * k;
    int i;
    for (i = 0; i < 4; i++) acc[i] = layer[i] * k + acc[i] * keep;
}

static void render_tile(const fa_fx_plan* p, const uint8_t* src, size_t src_stride, int w, int h, uint8_t* dst,
                        size_t dst_stride, int x0, int y0, int x1, int y1, fx_scratch* s)
{
    int wx0 = x0 - p->reach > 0 ? x0 - p->reach : 0, wy0 = y0 - p->reach > 0 ? y0 - p->reach : 0;
    int wx1 = x1 + p->reach < w ? x1 + p->reach : w, wy1 = y1 + p->reach < h ? y1 + p->reach : h;
    int wn = wx1 - wx0, hn = wy1 - wy0, x, y;
    /* The shadow at a pixel is the field at (pixel - offset): whole pixels and a
     * fraction, the same everywhere. */
    int sox = (int)floorf(-p->s_dx), soy = (int)floorf(-p->s_dy);
    float sfx = -p->s_dx - (float)sox, sfy = -p->s_dy - (float)soy;
    int snx = sox + (sfx >= 0.5f), sny = soy + (sfy >= 0.5f);
    const uint8_t* wa = src + (size_t)wy0 * src_stride + (size_t)wx0 * 4 + 3;
    field(wa, src_stride, wn, hn, 0, p->reach, NULL, s, s->dout);
    if (p->bevel) {
        /* Inside: only rows next to the shape matter. */
        for (y = 0; y < hn; y++) {
            const uint8_t* ar = wa + (size_t)y * src_stride;
            int any = 0;
            for (x = 0; x < wn && !any; x++) any = ar[(size_t)x * 4] != 0;
            s->rows[y] = (uint8_t)any;
        }
        for (y = 0; y < hn; y++)
            if (s->rows[y] == 1) {
                if (y > 0 && !s->rows[y - 1]) s->rows[y - 1] = 2;
                if (y + 1 < hn && !s->rows[y + 1]) s->rows[y + 1] = 2;
            }
        field(wa, src_stride, wn, hn, 1, p->reach, s->rows, s, s->din);
    }
    for (y = y0; y < y1; y++) {
        const uint8_t* sp = src + (size_t)y * src_stride + (size_t)x0 * 4;
        uint8_t* dp = dst + (size_t)y * dst_stride + (size_t)x0 * 4;
        int ly = y - wy0;
        for (x = x0; x < x1; x++, sp += 4, dp += 4) {
            int lx = x - wx0, i;
            size_t at = (size_t)ly * wn + lx;
            float acc[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, shape[4], a = (float)sp[3] / 255.0f;
            if (sp[3] == 255 && !p->bevel) {
                memcpy(dp, sp, 4);    /* the shape hides what is under it */
                continue;
            }
            if (!sp[3] && (!p->outline || s->dout[at] >= p->o_hi)) {
                /* Outside the shape and the outline: the shadow only, if it is near.
                 * The field changes by at most about a pixel and a half per pixel
                 * (coverage included), so 2.5 past the shadow's end at the nearest
                 * pixel is past it at all four that sample() would blend. */
                int qx = lx + snx, qy = ly + sny;
                if (!p->shadow || qx < 0 || qy < 0 || qx >= wn || qy >= hn ||
                    s->dout[(size_t)qy * wn + qx] >= p->s_hi + 2.5f) {
                    memset(dp, 0, 4);
                    continue;
                }
            }
            if (p->shadow) {
                float d = sample(s->dout, wn, hn, lx + sox, ly + soy, sfx, sfy);
                over(acc, p->s_pm, ease_out(p->s_lo, p->s_hi, d));
            }
            if (p->outline) over(acc, p->o_pm, ease_out(p->o_lo, p->o_hi, s->dout[at]));
            for (i = 0; i < 4; i++) shape[i] = (float)sp[i] / 255.0f;
            over(acc, shape, 1.0f);
            if (p->bevel && a > 0.0f) {
                float din = s->din[at], wgt = ease_out(p->b_lo, p->b_hi, din) * p->b_strength;
                if (wgt > 0.0f) {
                    /* The field rises inward; the edge faces the other way. */
                    int xl = lx > 0 ? lx - 1 : lx, xr = lx + 1 < wn ? lx + 1 : lx;
                    int yu = ly > 0 ? ly - 1 : ly, yd = ly + 1 < hn ? ly + 1 : ly;
                    float gx = (s->din[(size_t)ly * wn + xr] - s->din[(size_t)ly * wn + xl]) / (float)(xr - xl > 0 ? xr - xl : 1);
                    float gy = (s->din[(size_t)yd * wn + lx] - s->din[(size_t)yu * wn + lx]) / (float)(yd - yu > 0 ? yd - yu : 1);
                    float len = sqrtf(gx * gx + gy * gy);
                    if (len > 1e-3f && s->din[(size_t)ly * wn + xr] < FX_FAR && s->din[(size_t)ly * wn + xl] < FX_FAR &&
                        s->din[(size_t)yd * wn + lx] < FX_FAR && s->din[(size_t)yu * wn + lx] < FX_FAR) {
                        float facing = -(gx * p->b_lx + gy * p->b_ly) / len;
                        if (facing > 0.0f) over(acc, p->b_hi_pm, facing * wgt * a);
                        else over(acc, p->b_sh_pm, -facing * wgt * a);
                    }
                }
            }
            for (i = 0; i < 4; i++) {
                float v = acc[i] * 255.0f + 0.5f;
                dp[i] = (uint8_t)(v >= 255.0f ? 255.0f : v <= 0.0f ? 0.0f : v);
            }
        }
    }
}

void fa_fx_render(const fa_fx_plan* p, const uint8_t* src, size_t src_stride, int w, int h, uint8_t* dst,
                  size_t dst_stride, int x0, int y0, int x1, int y1, void* scratch)
{
    fx_scratch s;
    int tx, ty;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    if (x0 >= x1 || y0 >= y1) return;
    scratch_split(p, scratch, &s);
    for (ty = y0; ty < y1; ty += FX_TILE)
        for (tx = x0; tx < x1; tx += FX_TILE)
            render_tile(p, src, src_stride, w, h, dst, dst_stride, tx, ty, tx + FX_TILE < x1 ? tx + FX_TILE : x1,
                        ty + FX_TILE < y1 ? ty + FX_TILE : y1, &s);
}
