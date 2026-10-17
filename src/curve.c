/* Ramp and crossfade curves. Each maps progress t in 0..1 to a shape in 0..1;
 * the parameter value is start + (target - start) * shape. */
#include "fa_internal.h"
#include <math.h>

#define FA_PI 3.14159265358979323846

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
