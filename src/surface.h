/* Shared parts of the drawing surfaces (CD+G now; lyrics later): changed-area sets,
 * the revision history that lets update_into draw only what changed since an older
 * frame, and the presentation size rules. */
#ifndef FA_SURFACE_H
#define FA_SURFACE_H

#include "fatzaudio/fatzaudio.h"
#include <stdint.h>

/* Up to FA_SURFACE_MAX_DIRTY rectangles, x0 y0 x1 y1 (end exclusive). A rectangle
 * that overlaps one already there, or completes it to a rectangle, is merged into it;
 * when the set is full, the two whose bounding box wastes the least area merge. */
typedef struct fa_rects {
    int n;
    int r[FA_SURFACE_MAX_DIRTY][4];
} fa_rects;

void fa_rects_clear(fa_rects* s);
void fa_rects_add(fa_rects* s, int x0, int y0, int x1, int y1);
void fa_rects_add_all(fa_rects* s, const fa_rects* from);
/* The bounding box; all 0 for an empty set. */
void fa_rects_bounds(const fa_rects* s, int out[4]);

/* The changed areas of a surface's last FA_SURFACE_HISTORY revisions. */
typedef struct fa_surface_history {
    uint64_t rev[FA_SURFACE_HISTORY];   /* entry k holds the revision rev[k] (k = rev % HISTORY) */
    fa_rects area[FA_SURFACE_HISTORY];
} fa_surface_history;

void fa_history_push(fa_surface_history* h, uint64_t rev, const fa_rects* area);
/* What changed from revision `since` to `now`: 1 with `out` the union when the
 * history covers it (empty when since == now), 0 when a full draw is needed
 * (since 0, unknown, in the future or too old). */
int fa_history_since(const fa_surface_history* h, uint64_t now, uint64_t since, fa_rects* out);

/* fa_surface_set_presentation_size's rule: 0 x 0 unsets; one of them 0 is invalid;
 * else scaled, keeping the shape, into 300x216 .. 3840x2160 (as far as the shape
 * allows: a shape wider or taller than both limits allow is cut to the maximum). */
fa_result fa_presentation_size_clamp(uint32_t* width, uint32_t* height);

#endif
