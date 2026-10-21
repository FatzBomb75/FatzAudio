/* Shared parts of the drawing surfaces: see surface.h. */
#include "surface.h"
#include <string.h>

#define PRESENT_MIN_W 300
#define PRESENT_MIN_H 216
#define PRESENT_MAX_W 3840
#define PRESENT_MAX_H 2160

static int64_t area_of(const int r[4]) { return (int64_t)(r[2] - r[0]) * (r[3] - r[1]); }

static void unite(int a[4], const int b[4])
{
    if (b[0] < a[0]) a[0] = b[0];
    if (b[1] < a[1]) a[1] = b[1];
    if (b[2] > a[2]) a[2] = b[2];
    if (b[3] > a[3]) a[3] = b[3];
}

/* a and b overlap, or together are exactly their bounding box. */
static int mergeable(const int a[4], const int b[4])
{
    int u[4];
    if (a[0] < b[2] && b[0] < a[2] && a[1] < b[3] && b[1] < a[3]) return 1;
    memcpy(u, a, sizeof(u));
    unite(u, b);
    return area_of(u) == area_of(a) + area_of(b);
}

void fa_rects_clear(fa_rects* s) { s->n = 0; }

void fa_rects_add(fa_rects* s, int x0, int y0, int x1, int y1)
{
    int c[4], t[FA_SURFACE_MAX_DIRTY + 1][4], i, j, m, bi = 0, bj = 1, again = 1;
    int64_t best = INT64_MAX;
    if (x0 >= x1 || y0 >= y1) return;
    c[0] = x0; c[1] = y0; c[2] = x1; c[3] = y1;
    /* A merge can make the new rectangle reach others: repeat until it stops growing. */
    while (again) {
        again = 0;
        for (i = 0; i < s->n; i++) {
            if (!mergeable(s->r[i], c)) continue;
            unite(c, s->r[i]);
            memmove(s->r[i], s->r[i + 1], (size_t)(s->n - i - 1) * sizeof(s->r[0]));
            s->n--;
            again = 1;
            break;
        }
    }
    if (s->n < FA_SURFACE_MAX_DIRTY) {
        memcpy(s->r[s->n++], c, sizeof(c));
        return;
    }
    /* Full: merge the pair (the new one included) whose bounding box adds the least
     * area, and add that box again, as it may now reach others. */
    memcpy(t, s->r, sizeof(s->r));
    memcpy(t[FA_SURFACE_MAX_DIRTY], c, sizeof(c));
    m = FA_SURFACE_MAX_DIRTY + 1;
    for (i = 0; i < m; i++)
        for (j = i + 1; j < m; j++) {
            int u[4];
            int64_t waste;
            memcpy(u, t[i], sizeof(u));
            unite(u, t[j]);
            waste = area_of(u) - area_of(t[i]) - area_of(t[j]);
            if (waste < best) { best = waste; bi = i; bj = j; }
        }
    unite(t[bi], t[bj]);
    memcpy(c, t[bi], sizeof(c));
    s->n = 0;
    for (i = 0; i < m; i++)
        if (i != bi && i != bj) memcpy(s->r[s->n++], t[i], sizeof(c));
    fa_rects_add(s, c[0], c[1], c[2], c[3]);
}

void fa_rects_add_all(fa_rects* s, const fa_rects* from)
{
    int i;
    for (i = 0; i < from->n; i++) fa_rects_add(s, from->r[i][0], from->r[i][1], from->r[i][2], from->r[i][3]);
}

void fa_rects_bounds(const fa_rects* s, int out[4])
{
    int i;
    memset(out, 0, 4 * sizeof(int));
    if (s->n == 0) return;
    memcpy(out, s->r[0], 4 * sizeof(int));
    for (i = 1; i < s->n; i++) unite(out, s->r[i]);
}

void fa_history_push(fa_surface_history* h, uint64_t rev, const fa_rects* area)
{
    h->rev[rev % FA_SURFACE_HISTORY] = rev;
    h->area[rev % FA_SURFACE_HISTORY] = *area;
}

int fa_history_since(const fa_surface_history* h, uint64_t now, uint64_t since, fa_rects* out)
{
    uint64_t r;
    fa_rects_clear(out);
    if (since == 0 || since > now || now - since > FA_SURFACE_HISTORY) return 0;
    for (r = since + 1; r <= now; r++) {
        if (h->rev[r % FA_SURFACE_HISTORY] != r) return 0;
        fa_rects_add_all(out, &h->area[r % FA_SURFACE_HISTORY]);
    }
    return 1;
}

fa_result fa_presentation_size_clamp(uint32_t* width, uint32_t* height)
{
    double w = *width, h = *height, k;
    if (*width == 0 && *height == 0) return FA_OK;
    if (*width == 0 || *height == 0) return FA_ERROR_INVALID_ARG;
    k = PRESENT_MAX_W / w < PRESENT_MAX_H / h ? PRESENT_MAX_W / w : PRESENT_MAX_H / h;
    if (k < 1.0) { w *= k; h *= k; }
    k = PRESENT_MIN_W / w > PRESENT_MIN_H / h ? PRESENT_MIN_W / w : PRESENT_MIN_H / h;
    if (k > 1.0) { w *= k; h *= k; }
    *width = (uint32_t)(w + 0.5);
    *height = (uint32_t)(h + 0.5);
    if (*width > PRESENT_MAX_W) *width = PRESENT_MAX_W;
    if (*height > PRESENT_MAX_H) *height = PRESENT_MAX_H;
    if (*width < PRESENT_MIN_W) *width = PRESENT_MIN_W;
    if (*height < PRESENT_MIN_H) *height = PRESENT_MIN_H;
    return FA_OK;
}
