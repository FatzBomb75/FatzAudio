/* Ramp and crossfade curves. Each maps progress t in 0..1 to a shape in 0..1;
 * the parameter value is start + (target - start) * shape. */
#include "fa_internal.h"
#include <math.h>
#include <string.h>

/* CSS cubic-bezier: P0 = (0,0), P1 = (x1,y1), P2 = (x2,y2), P3 = (1,1).
 * Solve x(u) = t for u (Newton, then bisection as a fallback), return y(u). */
static double bez(double a1, double a2, double u)
{
    double v = 1.0 - u;
    return 3.0 * v * v * u * a1 + 3.0 * v * u * u * a2 + u * u * u;
}

static double bez_dx(double a1, double a2, double u)
{
    double v = 1.0 - u;
    return 3.0 * v * v * a1 + 6.0 * v * u * (a2 - a1) + 3.0 * u * u * (1.0 - a2);
}

static float bezier_eval(const float b[4], float t)
{
    double x1 = b[0], y1 = b[1], x2 = b[2], y2 = b[3];
    double u = t, lo = 0.0, hi = 1.0;
    int i;
    for (i = 0; i < 8; i++) {
        double x = bez(x1, x2, u) - t, d = bez_dx(x1, x2, u);
        if (fabs(x) < 1e-6) return (float)bez(y1, y2, u);
        if (fabs(d) < 1e-6) break;
        u -= x / d;
        if (u < 0.0 || u > 1.0) break;
    }
    for (i = 0; i < 40; i++) {
        u = 0.5 * (lo + hi);
        if (bez(x1, x2, u) < t) lo = u; else hi = u;
    }
    return (float)bez(y1, y2, u);
}

float fa_curve_eval(int curve, const float bezier[4], float t, int rising)
{
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    switch (curve) {
    case FA_CURVE_EQUAL_POWER:
        /* Rising follows sin, falling follows cos, so a fade-out paired with a
         * fade-in keeps out^2 + in^2 = 1 (constant power). */
        return rising ? (float)sin(t * FA_PI * 0.5) : (float)(1.0 - cos(t * FA_PI * 0.5));
    case FA_CURVE_LOG:
        return (float)log10(1.0 + 9.0 * t);              /* fast start, slow finish */
    case FA_CURVE_EXP:
        return (float)((pow(10.0, t) - 1.0) / 9.0);      /* slow start, fast finish */
    case FA_CURVE_SCURVE:
        return t * t * (3.0f - 2.0f * t);                /* smoothstep */
    case FA_CURVE_BEZIER:
        return bezier_eval(bezier, t);
    case FA_CURVE_LINEAR:
    default:
        return t;
    }
}

/* Point list: values at evenly spaced times, joined by straight lines. */
static float points_eval(const fa_shape* sh, float t)
{
    float x = t * (float)(sh->npoints - 1);
    int i = (int)x;
    if (i >= sh->npoints - 1) return sh->points[sh->npoints - 1];
    return sh->points[i] + (sh->points[i + 1] - sh->points[i]) * (x - (float)i);
}

float fa_shape_eval(const fa_shape* sh, float t, int rising)
{
    if (sh->curve == FA_CURVE_POINTS) {
        if (t <= 0.0f) return sh->points[0];
        if (t >= 1.0f) return sh->points[sh->npoints - 1];
        return points_eval(sh, t);
    }
    return fa_curve_eval(sh->curve, sh->bezier, t, rising);
}

float fa_shape_eval_mirror(const fa_shape* sh, float t)
{
    return 1.0f - fa_shape_eval(sh, 1.0f - t, 1);
}

fa_result fa_shape_from_ramp(const fa_ramp* r, int fallback, fa_shape* out)
{
    uint32_t i;
    memset(out, 0, sizeof(*out));
    if (!r) { out->curve = fallback; return FA_OK; }
    if ((int)r->curve < 0 || r->curve > FA_CURVE_POINTS) return FA_ERROR_INVALID_ARG;
    out->curve = r->curve;
    if (r->curve == FA_CURVE_BEZIER) {
        if (!(r->bezier_x1 >= 0.0f && r->bezier_x1 <= 1.0f && r->bezier_x2 >= 0.0f && r->bezier_x2 <= 1.0f))
            return FA_ERROR_INVALID_ARG;
        if (r->bezier_y1 != r->bezier_y1 || r->bezier_y2 != r->bezier_y2) return FA_ERROR_INVALID_ARG;
        out->bezier[0] = r->bezier_x1; out->bezier[1] = r->bezier_y1;
        out->bezier[2] = r->bezier_x2; out->bezier[3] = r->bezier_y2;
    }
    if (r->curve == FA_CURVE_POINTS) {
        if (!r->points || r->point_count < 2 || r->point_count > FA_CURVE_MAX_POINTS) return FA_ERROR_INVALID_ARG;
        for (i = 0; i < r->point_count; i++) {
            if (!(r->points[i] == r->points[i]) || fabsf(r->points[i]) > 1e6f) return FA_ERROR_INVALID_ARG;
            out->points[i] = r->points[i];
        }
        out->npoints = (int)r->point_count;
    }
    return FA_OK;
}

void fa_shape_to_ramp(const fa_shape* sh, fa_ramp* out)
{
    memset(out, 0, sizeof(*out));
    out->curve = (fa_curve)sh->curve;
    out->bezier_x1 = sh->bezier[0]; out->bezier_y1 = sh->bezier[1];
    out->bezier_x2 = sh->bezier[2]; out->bezier_y2 = sh->bezier[3];
    out->point_count = (uint32_t)sh->npoints;
}

void fa_shape_store(fa_shape_atomic* a, const fa_shape* sh)
{
    int i;
    fa_store32(&a->curve, sh->curve);
    for (i = 0; i < 4; i++) fa_store_float(&a->bezier[i], sh->bezier[i]);
    for (i = 0; i < sh->npoints; i++) fa_store_float(&a->points[i], sh->points[i]);
    fa_store32(&a->npoints, sh->npoints);
}

void fa_shape_load(fa_shape_atomic* a, fa_shape* out)
{
    int i;
    out->curve = fa_load32(&a->curve);
    for (i = 0; i < 4; i++) out->bezier[i] = fa_load_float(&a->bezier[i]);
    out->npoints = fa_load32(&a->npoints);
    if (out->npoints < 0 || out->npoints > FA_CURVE_MAX_POINTS) out->npoints = 0;
    for (i = 0; i < out->npoints; i++) out->points[i] = fa_load_float(&a->points[i]);
    /* A point list caught half-written has fewer than 2 points: fall back to linear. */
    if (out->curve == FA_CURVE_POINTS && out->npoints < 2) out->curve = FA_CURVE_LINEAR;
}
