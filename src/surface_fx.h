/* Surface effects (fa_surface_effects): outline, shadow and bevel from a distance
 * field of a premultiplied bitmap's alpha, for the CD+G surface now and the lyric
 * surface later. */
#ifndef FA_SURFACE_FX_H
#define FA_SURFACE_FX_H

#include "fatzaudio/fatzaudio.h"
#include <stddef.h>
#include <stdint.h>

/* The effects resolved to output pixels and the bitmap's byte order. */
typedef struct fa_fx_plan {
    int outline, shadow, bevel;
    float o_lo, o_hi;             /* outline alpha: 1 - smoothstep(o_lo, o_hi, distance) */
    float s_lo, s_hi;             /* shadow alpha, of the distance at the offset point */
    float s_dx, s_dy;             /* the shadow's offset, y down */
    float b_lo, b_hi;             /* bevel weight, of the distance inside */
    float b_lx, b_ly;             /* towards the light, y down */
    float b_strength;
    float o_pm[4], s_pm[4], b_hi_pm[4], b_sh_pm[4]; /* premultiplied, 0..1, buffer order */
    int reach;                    /* output pixels the effects reach beyond the shape */
} fa_fx_plan;

/* 1 when the effects are valid (fa_surface_set_effects' rule). */
int fa_fx_valid(const fa_surface_effects* e);
/* Resolves `e` for a surface whose size 1.0 is `outline`, `shadow_size`,
 * `shadow_distance` and `bevel` output pixels. `rgba`: the buffer is R, G, B, A
 * (else B, G, R, A). Returns 0 when no effect is on. */
int fa_fx_plan_make(fa_fx_plan* p, const fa_surface_effects* e, float outline, float shadow_size,
                    float shadow_distance, float bevel, int rgba);
/* fa_fx_render's tiles, output pixels: each works out the field around it. */
#define FA_FX_TILE 512
/* Scratch fa_fx_render needs for the plan. */
size_t fa_fx_scratch_bytes(const fa_fx_plan* p);
/* Draws the effects of `src` (w x h, premultiplied 4 bytes per pixel, alpha last)
 * into `dst` (the same size) over [x0, x1) x [y0, y1): reads `src` up to the reach
 * around it, writes only inside it. */
void fa_fx_render(const fa_fx_plan* p, const uint8_t* src, size_t src_stride, int w, int h, uint8_t* dst,
                  size_t dst_stride, int x0, int y0, int x1, int y1, void* scratch);

/* What fa_fx_render_keep worked out that does not depend on the colours of the shape,
 * for each pixel of `dst`: fa_fx_recolor then draws new colours of the same shape
 * without the distance field. The caller allocates the planes (`shadow` with the
 * plan's shadow, `bevel` with its bevel; stride `stride` pixels) and clears `mixed`. */
typedef struct fa_fx_keep {
    uint8_t* alpha;               /* the source's alpha as drawn */
    float* shadow;                /* the shadow's weight, where 0 < alpha < 255 */
    float* bevel;                 /* the bevel's weight where alpha > 0: + highlight, - shade */
    size_t stride;
    int mixed;                    /* a pixel had colour and no alpha: fa_fx_recolor can't */
} fa_fx_keep;
/* fa_fx_render, also filling `keep` (NULL: none) over [x0, x1) x [y0, y1). */
void fa_fx_render_keep(const fa_fx_plan* p, const uint8_t* src, size_t src_stride, int w, int h, uint8_t* dst,
                       size_t dst_stride, int x0, int y0, int x1, int y1, void* scratch, fa_fx_keep* keep);
/* Draws [x0, x1) x [y0, y1) of `dst` again for new colours of `src`, as
 * fa_fx_render_keep would, from what it kept with the same plan: 1 when done; 0, and
 * nothing drawn, when the shape changed there (the alpha differs from what was
 * kept), so the effects must be drawn again. Pixels outside the shape are left: the
 * effects there do not depend on its colours. */
int fa_fx_recolor(const fa_fx_plan* p, const uint8_t* src, size_t src_stride, uint8_t* dst, size_t dst_stride,
                  int x0, int y0, int x1, int y1, const fa_fx_keep* keep);

/* The effects' weights instead of colours, for a shape whose colours change (the
 * lyrics' wipe): `src` is a coverage plane (1 byte a pixel); each pixel of `dst`
 * (`nch` >= 4 bytes) gets the shadow's and the outline's weight (0..255), the
 * coverage, and the bevel (128 + -127..127: towards the highlight above 128, the
 * shade below), over [x0, x1) x [y0, y1). Drawn over each other in that order with
 * their colours, they give what fa_fx_render draws. Bytes past the fourth are left. */
void fa_fx_masks(const fa_fx_plan* p, const uint8_t* src, size_t src_stride, int w, int h, uint8_t* dst,
                 size_t dst_stride, int nch, int x0, int y0, int x1, int y1, void* scratch);
/* Grows the coverage plane `src` by `delta` pixels into `dst` (a synthetic bold).
 * 0 when out of memory. */
int fa_fx_dilate(const uint8_t* src, size_t src_stride, int w, int h, float delta, uint8_t* dst, size_t dst_stride);

#endif
