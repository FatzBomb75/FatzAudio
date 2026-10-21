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
/* Scratch fa_fx_render needs for the plan. */
size_t fa_fx_scratch_bytes(const fa_fx_plan* p);
/* Draws the effects of `src` (w x h, premultiplied 4 bytes per pixel, alpha last)
 * into `dst` (the same size) over [x0, x1) x [y0, y1): reads `src` up to the reach
 * around it, writes only inside it. */
void fa_fx_render(const fa_fx_plan* p, const uint8_t* src, size_t src_stride, int w, int h, uint8_t* dst,
                  size_t dst_stride, int x0, int y0, int x1, int y1, void* scratch);

#endif
