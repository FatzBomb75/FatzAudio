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
 * reach, so the scratch stays small.
 *
 * The bevel is kept to half the width of the stroke it is on: the stroke's half width
 * near a pixel is the largest inside distance within twice the bevel's size of it,
 * and the bevel there is at most half of that, so the middle of a thin stroke keeps
 * its colour (a one-pixel stroke gets none). */
#include "surface_fx.h"
#include "fa_util.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
/* SSE2 or NEON (64-bit ARM: it has vsqrtq), with plain C for the rest and other
 * machines: the same operations in the same order everywhere, so the same results
 * (the build turns off fused multiply-adds for this file). */
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#  include <emmintrin.h>
#  define FX_SSE 1
#else
#  define FX_SSE 0
#endif
#if !FX_SSE && (defined(__aarch64__) || defined(_M_ARM64))
#  include <arm_neon.h>
#  define FX_NEON 1
#else
#  define FX_NEON 0
#endif

#define FX_TILE FA_FX_TILE
#define FX_FAR 1e18f              /* no shape pixel within reach */
#define FX_NONE 32767             /* vertical distance: none within reach */
#define FX_STRIP 64               /* columns max_cols works on at a time */

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
    const double rad = FA_PI / 180.0;
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
        /* The half width is looked for 2 * size around a pixel, from inside distances
         * up to 2 * size, which need the shape that far again. */
        if (p->bevel && 4.0f * p->b_hi + 1.0f > reach) reach = 4.0f * p->b_hi + 1.0f;
    }
    p->reach = (int)ceilf(reach) + 2;
    return 1;
}

static int window_side(const fa_fx_plan* p) { return FX_TILE + 2 * p->reach; }

/* Values in a line buffer: a row or column of the window and the reach either side. */
static size_t line_len(const fa_fx_plan* p) { return (size_t)window_side(p) + 2 * (size_t)p->reach + 4; }

size_t fa_fx_scratch_bytes(const fa_fx_plan* p)
{
    size_t n = (size_t)window_side(p), planes = (size_t)(p->bevel ? 3 : 1), l = line_len(p);
    /* g and cv, the distance planes, the line buffers, and max_cols' strips (bevel). */
    return n * n * (sizeof(int16_t) + 1) + planes * n * n * sizeof(float) + l * (3 * sizeof(float) + sizeof(int16_t) + sizeof(int) + 1) +
           (p->bevel ? (2 * l + 1) * FX_STRIP * sizeof(float) : 0) + sizeof(float) + 128;
}

typedef struct fx_scratch {
    int16_t* g;                   /* vertical distance to the shape (FX_NONE: beyond reach) */
    uint8_t* cv;                  /* and that shape pixel's coverage */
    float* dout;                  /* distance outside the shape (negative inside) */
    float* din;                   /* distance inside it (bevel only) */
    float* half;                  /* the stroke's half width near each pixel (bevel only) */
    float *f, *dd, *z;
    int16_t* arg;
    int* v;
    uint8_t* rows;                /* rows of the window the inside field is wanted for */
    float *pre, *suf, *neg;       /* max_cols' strips (bevel only) */
} fx_scratch;

static void* take(uint8_t** at, size_t bytes)
{
    void* p = *at;
    *at += (bytes + 7) & ~(size_t)7;
    return p;
}

static void scratch_split(const fa_fx_plan* p, void* mem, fx_scratch* s)
{
    size_t n = (size_t)window_side(p), l = line_len(p);
    uint8_t* at = (uint8_t*)mem;
    s->g = (int16_t*)take(&at, n * n * sizeof(int16_t));
    s->cv = (uint8_t*)take(&at, n * n);
    s->dout = (float*)take(&at, n * n * sizeof(float));
    s->din = p->bevel ? (float*)take(&at, n * n * sizeof(float)) : NULL;
    s->half = p->bevel ? (float*)take(&at, n * n * sizeof(float)) : NULL;
    s->f = (float*)take(&at, l * sizeof(float));
    s->dd = (float*)take(&at, l * sizeof(float));
    s->z = (float*)take(&at, (l + 1) * sizeof(float));
    s->arg = (int16_t*)take(&at, l * sizeof(int16_t));
    s->v = (int*)take(&at, l * sizeof(int));
    s->rows = (uint8_t*)take(&at, l);
    s->pre = p->bevel ? (float*)take(&at, l * FX_STRIP * sizeof(float)) : NULL;
    s->suf = p->bevel ? (float*)take(&at, l * FX_STRIP * sizeof(float)) : NULL;
    s->neg = p->bevel ? (float*)take(&at, FX_STRIP * sizeof(float)) : NULL;
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

/* The distance field of the window (wn x hn at `a`) into `out`,
 * as far as `cap` pixels (alpha bytes `step` apart: 1, or 4 with `a` at the last byte of
 * each pixel): inside = 0: the distance outside the shape, from the pixels
 * with any coverage; inside = 1: the distance inside it, from the pixels not fully
 * covered. Corrected by the nearest pixel's coverage; FX_FAR when there is none
 * within reach. `rows` (inside only): 1 for the rows wanted, NULL = all. `shaped`
 * (unless NULL): 1 for the rows with a pixel of the field's own shape, else 0. */
static void field(const uint8_t* a, size_t stride, int step, int wn, int hn, int inside, int cap, const uint8_t* rows,
                  fx_scratch* s, float* out, uint8_t* shaped)
{
    int x, y;
    /* Down, then up: the vertical distance to the nearest shape pixel, and its
     * coverage. A pixel with no shape within reach keeps whatever coverage: it is
     * never read. */
    for (y = 0; y < hn; y++) {
        const uint8_t* ar = a + (size_t)y * stride;
        int16_t* g = s->g + (size_t)y * wn;
        uint8_t* cv = s->cv + (size_t)y * wn;
        const int16_t* gu = y ? g - wn : NULL;
        const uint8_t* cvu = y ? cv - wn : NULL;
        int any = 0;
        x = 0;
#if FX_SSE
        {
            /* Eight pixels at a time; the first row as if the one above had nothing. */
            const __m128i z = _mm_setzero_si128(), vcap = _mm_set1_epi16((int16_t)cap), none = _mm_set1_epi16(FX_NONE);
            const __m128i full = _mm_set1_epi16(255), one = _mm_set1_epi16(1);
            __m128i seen = z;
            for (; x + 8 <= wn; x += 8) {
                __m128i c, site, up, cu, prop, gn, cn;
                if (step == 4) {
                    /* `a` is the alpha, the last byte of each pixel: whole pixels read. */
                    const uint8_t* px = ar - 3 + (size_t)x * 4;
                    __m128i lo = _mm_srli_epi32(_mm_loadu_si128((const __m128i*)(const void*)px), 24);
                    __m128i hi = _mm_srli_epi32(_mm_loadu_si128((const __m128i*)(const void*)(px + 16)), 24);
                    c = _mm_packs_epi32(lo, hi);
                } else {
                    c = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)(const void*)(ar + x)), z);
                }
                site = inside ? _mm_cmplt_epi16(c, full) : _mm_cmpgt_epi16(c, z);
                seen = _mm_or_si128(seen, site);
                if (gu) {
                    up = _mm_loadu_si128((const __m128i*)(const void*)(gu + x));
                    cu = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)(const void*)(cvu + x)), z);
                    prop = _mm_cmplt_epi16(up, vcap);
                    gn = _mm_or_si128(_mm_and_si128(prop, _mm_add_epi16(up, one)), _mm_andnot_si128(prop, none));
                } else {
                    cu = c;
                    gn = none;
                }
                gn = _mm_andnot_si128(site, gn);
                cn = _mm_or_si128(_mm_and_si128(site, c), _mm_andnot_si128(site, cu));
                _mm_storeu_si128((__m128i*)(void*)(g + x), gn);
                _mm_storel_epi64((__m128i*)(void*)(cv + x), _mm_packus_epi16(cn, cn));
            }
            any = _mm_movemask_epi8(seen);
        }
#elif FX_NEON
        {
            const int16x8_t vcap = vdupq_n_s16((int16_t)cap), none = vdupq_n_s16(FX_NONE), one = vdupq_n_s16(1);
            uint16x8_t seen = vdupq_n_u16(0);
            for (; x + 8 <= wn; x += 8) {
                uint16x8_t c, site, cn, cu;
                int16x8_t gn;
                if (step == 4) c = vmovl_u8(vld4_u8(ar - 3 + (size_t)x * 4).val[3]);
                else c = vmovl_u8(vld1_u8(ar + x));
                site = inside ? vcltq_u16(c, vdupq_n_u16(255)) : vcgtq_u16(c, vdupq_n_u16(0));
                seen = vorrq_u16(seen, site);
                if (gu) {
                    int16x8_t up = vld1q_s16(gu + x);
                    cu = vmovl_u8(vld1_u8(cvu + x));
                    gn = vbslq_s16(vcltq_s16(up, vcap), vaddq_s16(up, one), none);
                } else {
                    cu = c;
                    gn = none;
                }
                gn = vbicq_s16(gn, vreinterpretq_s16_u16(site));
                cn = vbslq_u16(site, c, cu);
                vst1q_s16(g + x, gn);
                vst1_u8(cv + x, vmovn_u16(cn));
            }
            any = vmaxvq_u16(seen) != 0;
        }
#endif
        for (; x < wn; x++) {
            uint8_t c = ar[(size_t)x * (size_t)step];
            if (inside ? c < 255 : c > 0) {
                g[x] = 0;
                cv[x] = c;
                any = 1;
            } else if (gu && gu[x] < cap) {
                g[x] = (int16_t)(gu[x] + 1);
                cv[x] = cvu[x];
            } else {
                g[x] = FX_NONE;
            }
        }
        if (shaped) shaped[y] = (uint8_t)(any != 0);
    }
    for (y = hn - 2; y >= 0; y--) {
        int16_t* g = s->g + (size_t)y * wn;
        uint8_t* cv = s->cv + (size_t)y * wn;
        const int16_t* gd = g + wn;
        const uint8_t* cvd = cv + wn;
        x = 0;
#if FX_SSE
        {
            const __m128i z = _mm_setzero_si128(), vcap = _mm_set1_epi16((int16_t)cap), one = _mm_set1_epi16(1);
            for (; x + 8 <= wn; x += 8) {
                __m128i d = _mm_loadu_si128((const __m128i*)(const void*)(gd + x));
                __m128i gv = _mm_loadu_si128((const __m128i*)(const void*)(g + x)), d1 = _mm_add_epi16(d, one);
                __m128i take = _mm_and_si128(_mm_cmplt_epi16(d, vcap), _mm_cmplt_epi16(d1, gv));
                __m128i cd = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)(const void*)(cvd + x)), z);
                __m128i cc = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)(const void*)(cv + x)), z);
                gv = _mm_or_si128(_mm_and_si128(take, d1), _mm_andnot_si128(take, gv));
                cc = _mm_or_si128(_mm_and_si128(take, cd), _mm_andnot_si128(take, cc));
                _mm_storeu_si128((__m128i*)(void*)(g + x), gv);
                _mm_storel_epi64((__m128i*)(void*)(cv + x), _mm_packus_epi16(cc, cc));
            }
        }
#elif FX_NEON
        {
            const int16x8_t vcap = vdupq_n_s16((int16_t)cap), one = vdupq_n_s16(1);
            for (; x + 8 <= wn; x += 8) {
                int16x8_t d = vld1q_s16(gd + x), gv = vld1q_s16(g + x), d1 = vaddq_s16(d, one);
                uint16x8_t take = vandq_u16(vcltq_s16(d, vcap), vcltq_s16(d1, gv));
                vst1q_s16(g + x, vbslq_s16(take, d1, gv));
                vst1_u8(cv + x, vbsl_u8(vmovn_u16(take), vld1_u8(cvd + x), vld1_u8(cv + x)));
            }
        }
#endif
        for (; x < wn; x++)
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
        x = 0;
#if FX_SSE
        {
            const __m128i z = _mm_setzero_si128(), none = _mm_set1_epi16(FX_NONE);
            const __m128 far = _mm_set1_ps(FX_FAR);
            __m128i seen = z;
            for (; x + 8 <= wn; x += 8) {
                __m128i gv = _mm_loadu_si128((const __m128i*)(const void*)(g + x)), no = _mm_cmpeq_epi16(gv, none);
                __m128i nlo = _mm_unpacklo_epi16(no, no), nhi = _mm_unpackhi_epi16(no, no);
                __m128 lo = _mm_cvtepi32_ps(_mm_unpacklo_epi16(gv, z)), hi = _mm_cvtepi32_ps(_mm_unpackhi_epi16(gv, z));
                lo = _mm_mul_ps(lo, lo);
                hi = _mm_mul_ps(hi, hi);
                lo = _mm_or_ps(_mm_and_ps(_mm_castsi128_ps(nlo), far), _mm_andnot_ps(_mm_castsi128_ps(nlo), lo));
                hi = _mm_or_ps(_mm_and_ps(_mm_castsi128_ps(nhi), far), _mm_andnot_ps(_mm_castsi128_ps(nhi), hi));
                _mm_storeu_ps(s->f + x, lo);
                _mm_storeu_ps(s->f + x + 4, hi);
                seen = _mm_or_si128(seen, _mm_andnot_si128(no, _mm_set1_epi16(-1)));
            }
            any = _mm_movemask_epi8(seen) != 0;
        }
#elif FX_NEON
        {
            const uint16x8_t none = vdupq_n_u16(FX_NONE);
            const float32x4_t far = vdupq_n_f32(FX_FAR);
            uint16x8_t seen = vdupq_n_u16(0);
            for (; x + 8 <= wn; x += 8) {
                uint16x8_t gv = vreinterpretq_u16_s16(vld1q_s16(g + x)), no = vceqq_u16(gv, none);
                /* The masks widened with their sign: all ones stay all ones. */
                uint32x4_t nlo = vreinterpretq_u32_s32(vmovl_s16(vreinterpret_s16_u16(vget_low_u16(no))));
                uint32x4_t nhi = vreinterpretq_u32_s32(vmovl_s16(vreinterpret_s16_u16(vget_high_u16(no))));
                float32x4_t lo = vcvtq_f32_u32(vmovl_u16(vget_low_u16(gv))), hi = vcvtq_f32_u32(vmovl_u16(vget_high_u16(gv)));
                lo = vmulq_f32(lo, lo);
                hi = vmulq_f32(hi, hi);
                vst1q_f32(s->f + x, vbslq_f32(nlo, far, lo));
                vst1q_f32(s->f + x + 4, vbslq_f32(nhi, far, hi));
                seen = vorrq_u16(seen, vmvnq_u16(no));
            }
            any = vmaxvq_u16(seen) != 0;
        }
#endif
        for (; x < wn; x++) {
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
        x = 0;
#if FX_SSE
        {
            const __m128 k = _mm_set1_ps(1.0f / 255.0f), h = _mm_set1_ps(0.5f);
            for (; x + 4 <= wn; x += 4) {
                __m128 cov = _mm_mul_ps(_mm_cvtepi32_ps(_mm_setr_epi32(cv[s->arg[x]], cv[s->arg[x + 1]], cv[s->arg[x + 2]],
                                                                       cv[s->arg[x + 3]])), k);
                __m128 corr = inside ? _mm_sub_ps(cov, h) : _mm_sub_ps(h, cov);
                _mm_storeu_ps(o + x, _mm_add_ps(_mm_sqrt_ps(_mm_loadu_ps(s->dd + x)), corr));
            }
        }
#elif FX_NEON
        {
            const float32x4_t k = vdupq_n_f32(1.0f / 255.0f), h = vdupq_n_f32(0.5f);
            for (; x + 4 <= wn; x += 4) {
                int32_t c4[4];
                float32x4_t cov, corr;
                c4[0] = cv[s->arg[x]];
                c4[1] = cv[s->arg[x + 1]];
                c4[2] = cv[s->arg[x + 2]];
                c4[3] = cv[s->arg[x + 3]];
                cov = vmulq_f32(vcvtq_f32_s32(vld1q_s32(c4)), k);
                corr = inside ? vsubq_f32(cov, h) : vsubq_f32(h, cov);
                vst1q_f32(o + x, vaddq_f32(vsqrtq_f32(vld1q_f32(s->dd + x)), corr));
            }
        }
#endif
        for (; x < wn; x++) {
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

/* A pixel being drawn, premultiplied, 0..1: with SSE2 or NEON the four channels at
 * once (the same operations, and no fused multiply-add, so the same results). */
#if FX_SSE
typedef __m128 fx_px;
static fx_px px_zero(void) { return _mm_setzero_ps(); }
#elif FX_NEON
typedef float32x4_t fx_px;
static fx_px px_zero(void) { return vdupq_n_f32(0.0f); }
#else
typedef struct fx_px { float c[4]; } fx_px;
static fx_px px_zero(void) { fx_px v = { { 0.0f, 0.0f, 0.0f, 0.0f } }; return v; }
#endif

/* `layer` (premultiplied, alpha k) over `acc`. */
static fx_px over(fx_px acc, const float layer[4], float k)
{
    float keep = 1.0f - layer[3] * k;
#if FX_SSE
    return _mm_add_ps(_mm_mul_ps(_mm_loadu_ps(layer), _mm_set1_ps(k)), _mm_mul_ps(acc, _mm_set1_ps(keep)));
#elif FX_NEON
    return vaddq_f32(vmulq_f32(vld1q_f32(layer), vdupq_n_f32(k)), vmulq_f32(acc, vdupq_n_f32(keep)));
#else
    int i;
    for (i = 0; i < 4; i++) acc.c[i] = layer[i] * k + acc.c[i] * keep;
    return acc;
#endif
}

#if FX_NEON
/* Four of o[i] = a[i] > b[i] ? a[i] : b[i], as _mm_max_ps (vmaxq_f32 differs at -0). */
static void o_max(float* o, const float* a, const float* b)
{
    float32x4_t va = vld1q_f32(a), vb = vld1q_f32(b);
    vst1q_f32(o, vbslq_f32(vcgtq_f32(va, vb), va, vb));
}
#endif

/* o[i] = the larger of a[i] and b[i]. */
static void max_of(float* o, const float* a, const float* b, int n)
{
    int i = 0;
#if FX_SSE
    for (; i + 4 <= n; i += 4) _mm_storeu_ps(o + i, _mm_max_ps(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i)));
#elif FX_NEON
    for (; i + 4 <= n; i += 4) o_max(o + i, a + i, b + i);
#endif
    for (; i < n; i++) o[i] = a[i] > b[i] ? a[i] : b[i];
}

/* The largest of each row's (n wide) values within r of each, in place: the row with
 * r of -FX_FAR either side in `e`, the largest of 2, 4, ... in turn, then of two
 * windows of a power of two that together cover 2r + 1. */
static void max_row(float* row, int n, int r, float* e)
{
    int k = 2 * r + 1, len = n + 2 * r, p, j;
    for (j = 0; j < r; j++) e[j] = e[n + r + j] = -FX_FAR;
    memcpy(e + r, row, (size_t)n * sizeof(float));
    for (p = 1; 2 * p <= k; p *= 2) {
        /* In order, so e[j + p] is read before it is changed. */
        j = 0;
#if FX_SSE
        for (; j + p + 4 <= len; j += 4) _mm_storeu_ps(e + j, _mm_max_ps(_mm_loadu_ps(e + j), _mm_loadu_ps(e + j + p)));
#elif FX_NEON
        for (; j + p + 4 <= len; j += 4) o_max(e + j, e + j, e + j + p);
#endif
        for (; j + p < len; j++) e[j] = e[j] > e[j + p] ? e[j] : e[j + p];
    }
    max_of(row, e, e + k - p, n);
}

/* The largest of each column's values within r rows, in place (van Herk and Gil and
 * Werman: the largest from the start of each block of 2r + 1 rows and to its end,
 * then of one of each), FX_STRIP columns at a time. */
static void max_cols(float* plane, int wn, int hn, int r, fx_scratch* s)
{
    int k = 2 * r + 1, len = hn + 2 * r, x0, j;
    for (j = 0; j < FX_STRIP; j++) s->neg[j] = -FX_FAR;
    for (x0 = 0; x0 < wn; x0 += FX_STRIP) {
        int sw = wn - x0 < FX_STRIP ? wn - x0 : FX_STRIP;
        for (j = 0; j < len; j++) {
            const float* in = j >= r && j - r < hn ? plane + (size_t)(j - r) * wn + x0 : s->neg;
            float* o = s->pre + (size_t)j * FX_STRIP;
            if (j % k == 0) memcpy(o, in, (size_t)sw * sizeof(float));
            else max_of(o, o - FX_STRIP, in, sw);
        }
        for (j = len - 1; j >= 0; j--) {
            const float* in = j >= r && j - r < hn ? plane + (size_t)(j - r) * wn + x0 : s->neg;
            float* o = s->suf + (size_t)j * FX_STRIP;
            if (j % k == k - 1 || j == len - 1) memcpy(o, in, (size_t)sw * sizeof(float));
            else max_of(o, o + FX_STRIP, in, sw);
        }
        for (j = 0; j < hn; j++)
            max_of(plane + (size_t)j * wn + x0, s->suf + (size_t)j * FX_STRIP, s->pre + (size_t)(j + k - 1) * FX_STRIP, sw);
    }
}

/* s->half: half the largest inside distance (up to 2 * b_hi) within 2 * b_hi of each
 * pixel, from the rows of s->din worked out (`rows`). */
static void half_widths(const fa_fx_plan* p, fx_scratch* s, int wn, int hn)
{
    float cap = 2.0f * p->b_hi;
    int r = (int)ceilf(cap), x, y;
    for (y = 0; y < hn; y++) {
        float* d = s->din + (size_t)y * wn;
        float* o = s->half + (size_t)y * wn;
        if (!s->rows[y]) {
            for (x = 0; x < wn; x++) o[x] = 0.0f;
            continue;
        }
        for (x = 0; x < wn; x++) o[x] = d[x] < cap ? d[x] : cap;
        max_row(o, wn, r, s->f);
    }
    max_cols(s->half, wn, hn, r, s);
    for (y = 0; y < hn; y++)
        for (x = 0; x < wn; x++) s->half[(size_t)y * wn + x] *= 0.5f;
}

/* The bevel's weight at inside distance `din`, kept to `half`. */
static float bevel_weight(const fa_fx_plan* p, float din, float half)
{
    float hi = p->b_hi, lo = p->b_lo;
    if (half < hi) {
        if (half <= 0.0f) return 0.0f;
        lo *= half / hi;
        hi = half;
    }
    return ease_out(lo, hi, din) * p->b_strength;
}

/* The alpha of n pixels (4 bytes each) into `ka`: 1 when one of them has colour and no
 * alpha. */
static int keep_alpha(const uint8_t* sp, uint8_t* ka, int n)
{
    int x = 0, mixed = 0;
#if FX_SSE
    const __m128i z = _mm_setzero_si128(), rgb = _mm_set1_epi32(0x00FFFFFF);
    __m128i bad = z;
    for (; x + 4 <= n; x += 4) {
        __m128i v = _mm_loadu_si128((const __m128i*)(const void*)(sp + (size_t)x * 4)), a = _mm_srli_epi32(v, 24);
        int32_t four;
        bad = _mm_or_si128(bad, _mm_andnot_si128(_mm_cmpeq_epi32(_mm_and_si128(v, rgb), z), _mm_cmpeq_epi32(a, z)));
        a = _mm_packs_epi32(a, a);
        four = _mm_cvtsi128_si32(_mm_packus_epi16(a, a));
        memcpy(ka + x, &four, 4);
    }
    mixed = _mm_movemask_epi8(bad) != 0;
#elif FX_NEON
    uint8x16_t bad = vdupq_n_u8(0);
    for (; x + 16 <= n; x += 16) {
        uint8x16x4_t v = vld4q_u8(sp + (size_t)x * 4);
        uint8x16_t rgb = vorrq_u8(vorrq_u8(v.val[0], v.val[1]), v.val[2]);
        bad = vorrq_u8(bad, vandq_u8(vtstq_u8(rgb, rgb), vceqq_u8(v.val[3], vdupq_n_u8(0))));
        vst1q_u8(ka + x, v.val[3]);
    }
    mixed = vmaxvq_u8(bad) != 0;
#endif
    for (; x < n; x++) {
        ka[x] = sp[(size_t)x * 4 + 3];
        if (!ka[x] && (sp[(size_t)x * 4] | sp[(size_t)x * 4 + 1] | sp[(size_t)x * 4 + 2])) mixed = 1;
    }
    return mixed;
}

/* 1 when n pixels have the alpha kept in `ka`, and none has colour and no alpha. */
static int same_alpha(const uint8_t* sp, const uint8_t* ka, int n)
{
    int x = 0;
#if FX_SSE
    const __m128i z = _mm_setzero_si128(), rgb = _mm_set1_epi32(0x00FFFFFF);
    for (; x + 4 <= n; x += 4) {
        __m128i v = _mm_loadu_si128((const __m128i*)(const void*)(sp + (size_t)x * 4)), a = _mm_srli_epi32(v, 24), k;
        int32_t four;
        memcpy(&four, ka + x, 4);
        k = _mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128(four), z), z);
        if (_mm_movemask_epi8(_mm_cmpeq_epi32(a, k)) != 0xFFFF ||
            _mm_movemask_epi8(_mm_andnot_si128(_mm_cmpeq_epi32(_mm_and_si128(v, rgb), z), _mm_cmpeq_epi32(a, z))))
            return 0;
    }
#elif FX_NEON
    for (; x + 16 <= n; x += 16) {
        uint8x16x4_t v = vld4q_u8(sp + (size_t)x * 4);
        uint8x16_t rgb = vorrq_u8(vorrq_u8(v.val[0], v.val[1]), v.val[2]);
        uint8x16_t bad = vandq_u8(vtstq_u8(rgb, rgb), vceqq_u8(v.val[3], vdupq_n_u8(0)));
        if (vmaxvq_u8(vorrq_u8(bad, vmvnq_u8(vceqq_u8(v.val[3], vld1q_u8(ka + x)))))) return 0;
    }
#endif
    for (; x < n; x++)
        if (sp[(size_t)x * 4 + 3] != ka[x] || (!sp[(size_t)x * 4 + 3] && (sp[(size_t)x * 4] | sp[(size_t)x * 4 + 1] | sp[(size_t)x * 4 + 2])))
            return 0;
    return 1;
}

/* The shape over `acc` (what is under it), then the bevel (`bk` > 0: the highlight's
 * weight, < 0: the shade's), into `dp`. */
static void finish(const fa_fx_plan* p, fx_px acc, const uint8_t* sp, float bk, uint8_t* dp)
{
    float shape[4];
    int i;
    for (i = 0; i < 4; i++) shape[i] = (float)sp[i] / 255.0f;
    acc = over(acc, shape, 1.0f);
    if (bk > 0.0f) acc = over(acc, p->b_hi_pm, bk);
    else if (bk < 0.0f) acc = over(acc, p->b_sh_pm, -bk);
#if FX_SSE
    {
        int32_t px;
        __m128i x;
        acc = _mm_add_ps(_mm_mul_ps(acc, _mm_set1_ps(255.0f)), _mm_set1_ps(0.5f));
        x = _mm_cvttps_epi32(_mm_min_ps(_mm_max_ps(acc, _mm_setzero_ps()), _mm_set1_ps(255.0f)));
        x = _mm_packs_epi32(x, x);
        px = _mm_cvtsi128_si32(_mm_packus_epi16(x, x));
        memcpy(dp, &px, 4);
    }
#elif FX_NEON
    {
        uint32_t px;
        uint16x4_t w;
        acc = vaddq_f32(vmulq_f32(acc, vdupq_n_f32(255.0f)), vdupq_n_f32(0.5f));
        w = vmovn_u32(vreinterpretq_u32_s32(vcvtq_s32_f32(vminq_f32(vmaxq_f32(acc, vdupq_n_f32(0.0f)), vdupq_n_f32(255.0f)))));
        px = vget_lane_u32(vreinterpret_u32_u8(vmovn_u16(vcombine_u16(w, w))), 0);
        memcpy(dp, &px, 4);
    }
#else
    for (i = 0; i < 4; i++) {
        float v = acc.c[i] * 255.0f + 0.5f;
        dp[i] = (uint8_t)(v >= 255.0f ? 255.0f : v <= 0.0f ? 0.0f : v);
    }
#endif
}

/* The window of a tile: the tile plus the effects' reach, clipped to the image. */
typedef struct fx_window { int x0, y0, w, h; } fx_window;

static fx_window tile_window(const fa_fx_plan* p, int w, int h, int x0, int y0, int x1, int y1)
{
    int wx0 = x0 - p->reach > 0 ? x0 - p->reach : 0, wy0 = y0 - p->reach > 0 ? y0 - p->reach : 0;
    int wx1 = x1 + p->reach < w ? x1 + p->reach : w, wy1 = y1 + p->reach < h ? y1 + p->reach : h;
    fx_window win;
    win.x0 = wx0;
    win.y0 = wy0;
    win.w = wx1 - wx0;
    win.h = wy1 - wy0;
    return win;
}

/* The shadow at a pixel is the field at (pixel - offset): whole pixels and a
 * fraction, the same everywhere. nx, ny: the nearest pixel. */
typedef struct fx_shadow { int ox, oy, nx, ny; float fx, fy; } fx_shadow;

static fx_shadow shadow_split(const fa_fx_plan* p)
{
    fx_shadow sh;
    sh.ox = (int)floorf(-p->s_dx);
    sh.oy = (int)floorf(-p->s_dy);
    sh.fx = -p->s_dx - (float)sh.ox;
    sh.fy = -p->s_dy - (float)sh.oy;
    sh.nx = sh.ox + (sh.fx >= 0.5f);
    sh.ny = sh.oy + (sh.fy >= 0.5f);
    return sh;
}

/* The distance fields of a window: outside the shape, and with a bevel inside it. */
static void tile_fields(const fa_fx_plan* p, const uint8_t* wa, size_t stride, int comp, const fx_window* win, fx_scratch* s)
{
    int y;
    field(wa, stride, comp, win->w, win->h, 0, p->reach, NULL, s, s->dout, p->bevel ? s->rows : NULL);
    if (p->bevel) {
        /* Inside: only rows next to the shape matter. */
        for (y = 0; y < win->h; y++)
            if (s->rows[y] == 1) {
                if (y > 0 && !s->rows[y - 1]) s->rows[y - 1] = 2;
                if (y + 1 < win->h && !s->rows[y + 1]) s->rows[y + 1] = 2;
            }
        field(wa, stride, comp, win->w, win->h, 1, p->reach, s->rows, s, s->din, NULL);
        half_widths(p, s, win->w, win->h);
    }
}

/* How much a pixel's edge faces the light (> 0: the highlight, < 0: the shade); 0 where the field has no usable slope. */
static float bevel_facing(const fa_fx_plan* p, const fx_scratch* s, int lx, int ly, int wn, int hn)
{
    /* The field rises inward; the edge faces the other way. */
    int xl = lx > 0 ? lx - 1 : lx, xr = lx + 1 < wn ? lx + 1 : lx;
    int yu = ly > 0 ? ly - 1 : ly, yd = ly + 1 < hn ? ly + 1 : ly;
    float gx = (s->din[(size_t)ly * wn + xr] - s->din[(size_t)ly * wn + xl]) / (float)(xr - xl > 0 ? xr - xl : 1);
    float gy = (s->din[(size_t)yd * wn + lx] - s->din[(size_t)yu * wn + lx]) / (float)(yd - yu > 0 ? yd - yu : 1);
    float len = sqrtf(gx * gx + gy * gy);
    if (len > 1e-3f && s->din[(size_t)ly * wn + xr] < FX_FAR && s->din[(size_t)ly * wn + xl] < FX_FAR &&
        s->din[(size_t)yd * wn + lx] < FX_FAR && s->din[(size_t)yu * wn + lx] < FX_FAR)
        return -(gx * p->b_lx + gy * p->b_ly) / len;
    return 0.0f;
}

static void render_tile(const fa_fx_plan* p, const uint8_t* src, size_t src_stride, int w, int h, uint8_t* dst,
                        size_t dst_stride, int x0, int y0, int x1, int y1, fx_scratch* s, fa_fx_keep* keep)
{
    fx_window win = tile_window(p, w, h, x0, y0, x1, y1);
    int wx0 = win.x0, wy0 = win.y0, wn = win.w, hn = win.h, x, y;
    fx_shadow sh = shadow_split(p);
    int sox = sh.ox, soy = sh.oy, snx = sh.nx, sny = sh.ny;
    float sfx = sh.fx, sfy = sh.fy;
    tile_fields(p, src + (size_t)wy0 * src_stride + (size_t)wx0 * 4 + 3, src_stride, 4, &win, s);
    for (y = y0; y < y1; y++) {
        const uint8_t* sp = src + (size_t)y * src_stride + (size_t)x0 * 4;
        uint8_t* dp = dst + (size_t)y * dst_stride + (size_t)x0 * 4;
        int ly = y - wy0;
        if (keep && keep_alpha(sp, keep->alpha + (size_t)y * keep->stride + x0, x1 - x0)) keep->mixed = 1;
        for (x = x0; x < x1; x++, sp += 4, dp += 4) {
            int lx = x - wx0;
            size_t at = (size_t)ly * wn + lx;
            fx_px acc = px_zero();
            float a = (float)sp[3] / 255.0f, bk = 0.0f;
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
                float d = sample(s->dout, wn, hn, lx + sox, ly + soy, sfx, sfy), sw = ease_out(p->s_lo, p->s_hi, d);
                if (keep) keep->shadow[(size_t)y * keep->stride + x] = sw;
                acc = over(acc, p->s_pm, sw);
            }
            if (p->outline) acc = over(acc, p->o_pm, ease_out(p->o_lo, p->o_hi, s->dout[at]));
            if (p->bevel && a > 0.0f) {
                float din = s->din[at], wgt = bevel_weight(p, din, s->half[at]);
                if (wgt > 0.0f) bk = bevel_facing(p, s, lx, ly, wn, hn) * wgt * a;   /* > 0: the highlight, < 0: the shade */
                if (keep) keep->bevel[(size_t)y * keep->stride + x] = bk;
            }
            finish(p, acc, sp, bk, dp);
        }
    }
}

void fa_fx_render(const fa_fx_plan* p, const uint8_t* src, size_t src_stride, int w, int h, uint8_t* dst,
                  size_t dst_stride, int x0, int y0, int x1, int y1, void* scratch)
{
    fa_fx_render_keep(p, src, src_stride, w, h, dst, dst_stride, x0, y0, x1, y1, scratch, NULL);
}

void fa_fx_render_keep(const fa_fx_plan* p, const uint8_t* src, size_t src_stride, int w, int h, uint8_t* dst,
                       size_t dst_stride, int x0, int y0, int x1, int y1, void* scratch, fa_fx_keep* keep)
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
                        ty + FX_TILE < y1 ? ty + FX_TILE : y1, &s, keep);
}

int fa_fx_recolor(const fa_fx_plan* p, const uint8_t* src, size_t src_stride, uint8_t* dst, size_t dst_stride,
                  int x0, int y0, int x1, int y1, const fa_fx_keep* keep)
{
    int x, y;
    if (keep->mixed) return 0;
    for (y = y0; y < y1; y++)
        if (!same_alpha(src + (size_t)y * src_stride + (size_t)x0 * 4, keep->alpha + (size_t)y * keep->stride + x0, x1 - x0))
            return 0;
    /* What fa_fx_render does, with the weights it kept. No coverage: only what is
     * under the shape, which did not change. In it, the field is the pixel's own
     * coverage (its own distance is 0), so the outline needs nothing kept. */
    for (y = y0; y < y1; y++) {
        const uint8_t* sp = src + (size_t)y * src_stride + (size_t)x0 * 4;
        uint8_t* dp = dst + (size_t)y * dst_stride + (size_t)x0 * 4;
        size_t at = (size_t)y * keep->stride + x0;
        for (x = x0; x < x1; x++, sp += 4, dp += 4, at++) {
            fx_px acc = px_zero();
            if (!sp[3]) continue;
            if (sp[3] == 255 && !p->bevel) {
                memcpy(dp, sp, 4);
                continue;
            }
            /* Fully covered: the shape hides what is under it (finish() gives the shape
             * whatever acc holds). */
            if (sp[3] < 255) {
                float cov = (float)sp[3] * (1.0f / 255.0f);
                if (p->shadow) acc = over(acc, p->s_pm, keep->shadow[at]);
                if (p->outline) acc = over(acc, p->o_pm, ease_out(p->o_lo, p->o_hi, 0.5f - cov));
            }
            finish(p, acc, sp, p->bevel ? keep->bevel[at] : 0.0f, dp);
        }
    }
    return 1;
}

/* ---- masks: the effects' weights, coloured later ------------------------------------ */

static void masks_tile(const fa_fx_plan* p, const uint8_t* src, size_t src_stride, int w, int h, uint8_t* dst,
                       size_t dst_stride, int nch, int x0, int y0, int x1, int y1, fx_scratch* s)
{
    fx_window win = tile_window(p, w, h, x0, y0, x1, y1);
    int wx0 = win.x0, wy0 = win.y0, wn = win.w, hn = win.h, x, y;
    fx_shadow sh = shadow_split(p);
    int sox = sh.ox, soy = sh.oy, snx = sh.nx, sny = sh.ny;
    float sfx = sh.fx, sfy = sh.fy;
    tile_fields(p, src + (size_t)wy0 * src_stride + (size_t)wx0, src_stride, 1, &win, s);
    for (y = y0; y < y1; y++) {
        const uint8_t* sp = src + (size_t)y * src_stride + (size_t)x0;
        uint8_t* dp = dst + (size_t)y * dst_stride + (size_t)x0 * (size_t)nch;
        int ly = y - wy0;
        for (x = x0; x < x1; x++, sp++, dp += nch) {
            int lx = x - wx0;
            size_t at = (size_t)ly * wn + lx;
            float a = (float)*sp / 255.0f, sw = 0.0f, ow = 0.0f, bv = 0.0f;
            dp[2] = *sp;
            if (p->shadow) {
                int qx = lx + snx, qy = ly + sny;
                if (qx >= 0 && qy >= 0 && qx < wn && qy < hn && s->dout[(size_t)qy * wn + qx] < p->s_hi + 2.5f)
                    sw = ease_out(p->s_lo, p->s_hi, sample(s->dout, wn, hn, lx + sox, ly + soy, sfx, sfy));
            }
            if (p->outline) ow = ease_out(p->o_lo, p->o_hi, s->dout[at]);
            if (p->bevel && a > 0.0f) {
                float din = s->din[at], wgt = bevel_weight(p, din, s->half[at]);
                if (wgt > 0.0f) bv = bevel_facing(p, s, lx, ly, wn, hn) * wgt * a;
            }
            dp[0] = (uint8_t)(sw * 255.0f + 0.5f);
            dp[1] = (uint8_t)(ow * 255.0f + 0.5f);
            bv = 128.0f + bv * 127.0f + 0.5f;
            dp[3] = (uint8_t)(bv < 1.0f ? 1.0f : bv > 255.0f ? 255.0f : bv);
        }
    }
}

void fa_fx_masks(const fa_fx_plan* p, const uint8_t* src, size_t src_stride, int w, int h, uint8_t* dst,
                 size_t dst_stride, int nch, int x0, int y0, int x1, int y1, void* scratch)
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
            masks_tile(p, src, src_stride, w, h, dst, dst_stride, nch, tx, ty, tx + FX_TILE < x1 ? tx + FX_TILE : x1,
                       ty + FX_TILE < y1 ? ty + FX_TILE : y1, &s);
}

int fa_fx_dilate(const uint8_t* src, size_t src_stride, int w, int h, float delta, uint8_t* dst, size_t dst_stride)
{
    fa_fx_plan p;
    fx_scratch s;
    void* mem;
    int tx, ty, x, y;
    memset(&p, 0, sizeof(p));
    p.reach = (int)ceilf(delta) + 2;
    mem = malloc(fa_fx_scratch_bytes(&p));
    if (!mem) return 0;
    scratch_split(&p, mem, &s);
    for (ty = 0; ty < h; ty += FX_TILE)
        for (tx = 0; tx < w; tx += FX_TILE) {
            int x1 = tx + FX_TILE < w ? tx + FX_TILE : w, y1 = ty + FX_TILE < h ? ty + FX_TILE : h;
            int wx0 = tx - p.reach > 0 ? tx - p.reach : 0, wy0 = ty - p.reach > 0 ? ty - p.reach : 0;
            int wx1 = x1 + p.reach < w ? x1 + p.reach : w, wy1 = y1 + p.reach < h ? y1 + p.reach : h;
            int wn = wx1 - wx0, hn = wy1 - wy0;
            field(src + (size_t)wy0 * src_stride + (size_t)wx0, src_stride, 1, wn, hn, 0, p.reach, NULL, &s, s.dout, NULL);
            for (y = ty; y < y1; y++)
                for (x = tx; x < x1; x++) {
                    float d = s.dout[(size_t)(y - wy0) * wn + (x - wx0)], v = ease_out(delta - 0.5f, delta + 0.5f, d) * 255.0f;
                    uint8_t o = src[(size_t)y * src_stride + x];
                    dst[(size_t)y * dst_stride + x] = (float)o >= v ? o : (uint8_t)(v + 0.5f);
                }
        }
    free(mem);
    return 1;
}
