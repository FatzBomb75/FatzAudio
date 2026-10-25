/* Font reader: TrueType and OpenType (CFF) faces, TrueType collections, cmap
 * formats 4, 6 and 12, metrics, kerning from GPOS pair adjustment and the kern
 * table, and glyph outlines (glyf quadratic contours with composites, CFF Type 2
 * charstrings with subroutines and flex). Written from the OpenType specification
 * and Adobe's CFF (TN 5176) and Type 2 charstring (TN 5177) documents.
 *
 * Every read goes through a bounds-checked view: a read past the end returns 0, a
 * sub-view is cut to what is there. Outlines have limits on points, commands,
 * composite depth and subroutine nesting, so a damaged or hostile file can give a
 * wrong glyph but never read outside the file or loop for long. */
#include "font.h"
#include "fa_platform.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FONT_MAX_FILE (512u * 1024u * 1024u)
#define FONT_MAX_POINTS 65536          /* points in one simple glyph */
#define FONT_MAX_COMMANDS 400000       /* path commands of one glyph, composites included */
#define FONT_MAX_DEPTH 8               /* composite nesting */
#define FONT_MAX_COMPONENTS 4096       /* components drawn for one glyph, nested ones included */
#define FONT_MAX_COORD 1e7f            /* larger coordinates (font units) mean a damaged glyph */
#define KERN_MAX_SUBTABLES 1024        /* subtables looked at for one pair */
#define CFF_MAX_STACK 48
#define CFF_MAX_SUBR_DEPTH 10
#define CFF_MAX_OPS 200000             /* operators per glyph, subroutines included */
#define KERN_MAX_LOOKUPS 64

/* ---- bounds-checked views --------------------------------------------------- */

typedef struct view {
    const uint8_t* p;
    size_t n;
} view;

static const view k_none = { NULL, 0 };

static int has(view v, size_t o, size_t len) { return o <= v.n && v.n - o >= len; }
static uint32_t u8(view v, size_t o) { return has(v, o, 1) ? v.p[o] : 0; }
static uint32_t u16(view v, size_t o) { return has(v, o, 2) ? (uint32_t)(v.p[o] << 8 | v.p[o + 1]) : 0; }
static int32_t s16(view v, size_t o) { return (int16_t)u16(v, o); }
static uint32_t u24(view v, size_t o) { return has(v, o, 3) ? (uint32_t)v.p[o] << 16 | (uint32_t)v.p[o + 1] << 8 | v.p[o + 2] : 0; }
static uint32_t u32(view v, size_t o)
{
    return has(v, o, 4) ? (uint32_t)v.p[o] << 24 | (uint32_t)v.p[o + 1] << 16 | (uint32_t)v.p[o + 2] << 8 | v.p[o + 3] : 0;
}
/* Bytes [o, o + len) of v, cut to what v has. */
static view sub(view v, size_t o, size_t len)
{
    view r = { NULL, 0 };
    if (o > v.n) return r;
    r.p = v.p + o;
    r.n = len < v.n - o ? len : v.n - o;
    return r;
}
static view from(view v, size_t o) { return sub(v, o, (size_t)-1); }

/* ---- the face ------------------------------------------------------------------ */

typedef struct cff_font {
    view cff;
    size_t charstrings;               /* INDEX offsets within cff */
    size_t gsubrs;
    uint32_t gsubr_count;
    view lsubrs;                      /* the local Subrs INDEX (from its start) of a non-CID font */
    int cid;
    size_t fdarray;                   /* INDEX */
    view fdselect;
} cff_font;

struct fa_font {
    view file;
    int owned;
    fa_font_info info;
    view head, hhea, hmtx, maxp, loca, glyf, cmap, kern, gpos, os2, name, post;
    int loca_long;
    uint32_t hmetrics;
    int cmap_format;
    int cmap_symbol;
    cff_font cff;
    uint32_t kern_lookups[KERN_MAX_LOOKUPS];
    uint32_t kern_lookup_count;
    view gpos_lookups;                /* the LookupList */
};

static uint32_t tag(const char* t) { return (uint32_t)t[0] << 24 | (uint32_t)t[1] << 16 | (uint32_t)t[2] << 8 | (uint32_t)t[3]; }

static int is_sfnt(uint32_t v) { return v == 0x00010000u || v == tag("OTTO") || v == tag("true"); }

uint32_t fa_font_face_count(const uint8_t* data, size_t size)
{
    view f = { data, size };
    uint32_t v = u32(f, 0);
    if (v == tag("ttcf")) {
        uint32_t n = u32(f, 8);
        return n && n <= (size - 12) / 4 ? n : 0;
    }
    return is_sfnt(v) && size >= 12 ? 1 : 0;
}

/* ---- names ---------------------------------------------------------------------- */

static void put_utf8(char* out, size_t cap, size_t* at, uint32_t c)
{
    uint8_t b[4];
    size_t n, i;
    if (c < 0x80) { b[0] = (uint8_t)c; n = 1; }
    else if (c < 0x800) { b[0] = (uint8_t)(0xC0 | c >> 6); b[1] = (uint8_t)(0x80 | (c & 0x3F)); n = 2; }
    else if (c < 0x10000) {
        b[0] = (uint8_t)(0xE0 | c >> 12); b[1] = (uint8_t)(0x80 | ((c >> 6) & 0x3F)); b[2] = (uint8_t)(0x80 | (c & 0x3F)); n = 3;
    } else {
        b[0] = (uint8_t)(0xF0 | c >> 18); b[1] = (uint8_t)(0x80 | ((c >> 12) & 0x3F));
        b[2] = (uint8_t)(0x80 | ((c >> 6) & 0x3F)); b[3] = (uint8_t)(0x80 | (c & 0x3F)); n = 4;
    }
    if (*at + n >= cap) return;
    for (i = 0; i < n; i++) out[(*at)++] = (char)b[i];
    out[*at] = 0;
}

/* A score for a name record: Windows Unicode US English first, then any English,
 * then Unicode platform, then Mac Roman; 0 = unusable. */
static int name_score(uint32_t platform, uint32_t encoding, uint32_t language)
{
    if (platform == 3 && (encoding == 1 || encoding == 10)) return language == 0x409 ? 6 : (language & 0x3FF) == 9 ? 5 : 3;
    if (platform == 0) return 4;
    if (platform == 1 && encoding == 0) return language == 0 ? 2 : 1;
    return 0;
}

static int read_name(view name, uint32_t id, char* out, size_t cap)
{
    uint32_t count = u16(name, 2), storage = u16(name, 4), i, best_rec = 0;
    int best = 0;
    size_t at = 0;
    view s;
    out[0] = 0;
    for (i = 0; i < count; i++) {
        size_t r = 6 + (size_t)i * 12;
        int sc;
        if (!has(name, r, 12) || u16(name, r + 6) != id) continue;
        sc = name_score(u16(name, r), u16(name, r + 2), u16(name, r + 4));
        if (sc > best) { best = sc; best_rec = i; }
    }
    if (!best) return 0;
    {
        size_t r = 6 + (size_t)best_rec * 12;
        uint32_t platform = u16(name, r), len = u16(name, r + 8), off = u16(name, r + 10);
        s = sub(name, (size_t)storage + off, len);
        if (platform == 1) {
            for (i = 0; i < s.n; i++) put_utf8(out, cap, &at, s.p[i] < 0x80 ? s.p[i] : '?');
        } else {
            for (i = 0; i + 1 < s.n; i += 2) {
                uint32_t c = u16(s, i);
                if (c >= 0xD800 && c < 0xDC00 && i + 3 < s.n) {
                    uint32_t lo = u16(s, i + 2);
                    if (lo >= 0xDC00 && lo < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); i += 2; }
                }
                put_utf8(out, cap, &at, c ? c : '?');
            }
        }
    }
    return out[0] != 0;
}

/* ---- cmap ----------------------------------------------------------------------- */

static int cmap_rank(uint32_t platform, uint32_t encoding, uint32_t format)
{
    if (format == 12 && (platform == 3 && encoding == 10)) return 7;
    if (format == 12 && platform == 0) return 6;
    if (format == 4 && platform == 3 && encoding == 1) return 5;
    if (format == 4 && platform == 0) return 4;
    if (format == 6 && (platform == 0 || (platform == 3 && encoding == 1))) return 3;
    if (format == 4 && platform == 3 && encoding == 0) return 2;   /* symbol */
    return 0;
}

static void pick_cmap(fa_font* f, view cmap)
{
    uint32_t count = u16(cmap, 2), i;
    int best = 0;
    for (i = 0; i < count; i++) {
        size_t r = 4 + (size_t)i * 8;
        uint32_t platform = u16(cmap, r), encoding = u16(cmap, r + 2), off = u32(cmap, r + 4);
        view t = from(cmap, off);
        uint32_t format = u16(t, 0);
        int rank = cmap_rank(platform, encoding, format);
        if (rank > best) {
            best = rank;
            f->cmap = t;          /* declared lengths are often wrong: read to the end of cmap */
            f->cmap_format = (int)format;
            f->cmap_symbol = rank == 2;
        }
    }
}

static uint32_t cmap_lookup(const fa_font* f, uint32_t c)
{
    view t = f->cmap;
    if (f->cmap_format == 4) {
        uint32_t segs = u16(t, 6) / 2, lo = 0, hi = segs;
        size_t ends = 14, starts = 16 + (size_t)segs * 2, deltas = starts + (size_t)segs * 2, ranges = deltas + (size_t)segs * 2;
        if (c > 0xFFFF) return 0;
        while (lo < hi) {           /* the first segment whose end is >= c */
            uint32_t mid = (lo + hi) / 2;
            if (u16(t, ends + (size_t)mid * 2) < c) lo = mid + 1; else hi = mid;
        }
        if (lo >= segs) return 0;
        {
            uint32_t start = u16(t, starts + (size_t)lo * 2), delta = u16(t, deltas + (size_t)lo * 2);
            size_t ro_at = ranges + (size_t)lo * 2;
            uint32_t ro = u16(t, ro_at), g;
            if (c < start) return 0;
            if (!ro) return (c + delta) & 0xFFFF;
            g = u16(t, ro_at + ro + (size_t)(c - start) * 2);
            return g ? (g + delta) & 0xFFFF : 0;
        }
    }
    if (f->cmap_format == 12) {
        uint32_t groups = u32(t, 12), lo = 0, hi;
        if (!has(t, 16, (size_t)groups * 12)) groups = (uint32_t)((t.n > 16 ? t.n - 16 : 0) / 12);
        hi = groups;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            size_t g = 16 + (size_t)mid * 12;
            if (u32(t, g + 4) < c) lo = mid + 1;
            else if (u32(t, g) > c) hi = mid;
            else return u32(t, g + 8) + (c - u32(t, g));
        }
        return 0;
    }
    if (f->cmap_format == 6) {
        uint32_t first = u16(t, 6), count = u16(t, 8);
        return c >= first && c - first < count ? u16(t, 10 + (size_t)(c - first) * 2) : 0;
    }
    return 0;
}

uint32_t fa_font_glyph(const fa_font* f, uint32_t c)
{
    uint32_t g;
    if (!f) return 0;
    g = cmap_lookup(f, c);
    if (!g && f->cmap_symbol && c < 0x100) g = cmap_lookup(f, 0xF000 + c);
    return g < f->info.glyph_count ? g : 0;
}

/* ---- metrics -------------------------------------------------------------------- */

int32_t fa_font_advance(const fa_font* f, uint32_t g)
{
    if (!f || !f->hmetrics || g >= f->info.glyph_count) return 0;
    if (g >= f->hmetrics) g = f->hmetrics - 1;
    return (int32_t)u16(f->hmtx, (size_t)g * 4);
}

/* ---- kerning -------------------------------------------------------------------- */

/* The coverage index of glyph g, -1 when not covered. */
static int32_t coverage(view c, uint32_t g)
{
    uint32_t format = u16(c, 0), n = u16(c, 2), lo = 0, hi = n;
    if (format == 1) {
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2, v = u16(c, 4 + (size_t)mid * 2);
            if (v < g) lo = mid + 1; else if (v > g) hi = mid; else return (int32_t)mid;
        }
    } else if (format == 2) {
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            size_t r = 4 + (size_t)mid * 6;
            if (u16(c, r + 2) < g) lo = mid + 1;
            else if (u16(c, r) > g) hi = mid;
            else return (int32_t)(u16(c, r + 4) + g - u16(c, r));
        }
    }
    return -1;
}

static uint32_t class_of(view c, uint32_t g)
{
    uint32_t format = u16(c, 0);
    if (format == 1) {
        uint32_t start = u16(c, 2), n = u16(c, 4);
        return g >= start && g - start < n ? u16(c, 6 + (size_t)(g - start) * 2) : 0;
    }
    if (format == 2) {
        uint32_t n = u16(c, 2), lo = 0, hi = n;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            size_t r = 4 + (size_t)mid * 6;
            if (u16(c, r + 2) < g) lo = mid + 1;
            else if (u16(c, r) > g) hi = mid;
            else return u16(c, r + 4);
        }
    }
    return 0;
}

static uint32_t popcount8(uint32_t v)
{
    uint32_t n = 0;
    for (v &= 0xFF; v; v &= v - 1) n++;
    return n;
}

/* The first value record's XAdvance at `at`, 0 when its format has none. */
static int32_t x_advance(view v, size_t at, uint32_t format)
{
    return format & 4 ? s16(v, at + 2 * popcount8(format & 3)) : 0;
}

/* A PairPos subtable: 1 with *out set when it applies to the pair. */
static int pair_pos(view st, uint32_t left, uint32_t right, int32_t* out)
{
    uint32_t format = u16(st, 0), vf1 = u16(st, 4), vf2 = u16(st, 6);
    size_t s1 = 2 * popcount8(vf1), s2 = 2 * popcount8(vf2);
    int32_t ci = coverage(from(st, u16(st, 2)), left);
    if (ci < 0) return 0;
    if (format == 1) {
        view set;
        uint32_t n, lo = 0, hi;
        if ((uint32_t)ci >= u16(st, 8)) return 0;
        set = from(st, u16(st, 10 + (size_t)ci * 2));
        n = u16(set, 0);
        hi = n;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            size_t r = 2 + (size_t)mid * (2 + s1 + s2);
            uint32_t second = u16(set, r);
            if (second < right) lo = mid + 1;
            else if (second > right) hi = mid;
            else { *out = x_advance(set, r + 2, vf1); return 1; }
        }
        return 0;
    }
    if (format == 2) {
        uint32_t c1 = class_of(from(st, u16(st, 8)), left), c2 = class_of(from(st, u16(st, 10)), right);
        uint32_t n1 = u16(st, 12), n2 = u16(st, 14);
        if (c1 >= n1 || c2 >= n2) return 1;
        *out = x_advance(st, 16 + ((size_t)c1 * n2 + c2) * (s1 + s2), vf1);
        return 1;
    }
    return 0;
}

static int32_t gpos_kerning(const fa_font* f, uint32_t left, uint32_t right)
{
    int32_t total = 0;
    uint32_t i, j, visits = 0;
    for (i = 0; i < f->kern_lookup_count; i++) {
        view lk = from(f->gpos_lookups, u16(f->gpos_lookups, 2 + (size_t)f->kern_lookups[i] * 2));
        uint32_t type = u16(lk, 0), count = u16(lk, 4);
        if (type != 2 && type != 9) continue;    /* only pair adjustment kerns */
        for (j = 0; j < count; j++) {
            view st = from(lk, u16(lk, 6 + (size_t)j * 2));
            int32_t v = 0;
            if (++visits > KERN_MAX_SUBTABLES) return total;
            if (type == 9) {
                if (u16(st, 2) != 2) continue;
                st = from(st, u32(st, 4));
            }
            if (pair_pos(st, left, right, &v)) { total += v; break; }
        }
    }
    return total;
}

static int32_t kern_table(const fa_font* f, uint32_t left, uint32_t right)
{
    view k = f->kern;
    uint32_t n = u16(k, 2), i, key = left << 16 | right;
    size_t at = 4;
    int32_t total = 0;
    if (u16(k, 0) != 0) return 0;     /* Apple's version 1 table is not read */
    for (i = 0; i < n && has(k, at, 6); i++) {
        uint32_t len = u16(k, at + 2), cov = u16(k, at + 4);
        view st;
        /* A format 0 subtable over 64 KB has its 16-bit length wrapped: size it by its pairs. */
        if ((cov >> 8) == 0 && len < 14 + u16(k, at + 6) * 6u) len = 14 + u16(k, at + 6) * 6u;
        st = sub(k, at, len < 6 ? 6 : len);
        /* format 0, horizontal, neither minimum nor cross-stream */
        if ((cov >> 8) == 0 && (cov & 7) == 1) {
            uint32_t pairs = u16(st, 6), lo = 0, hi = pairs;
            while (lo < hi) {
                uint32_t mid = (lo + hi) / 2;
                size_t r = 14 + (size_t)mid * 6;
                uint32_t v = u32(st, r);
                if (v < key) lo = mid + 1;
                else if (v > key) hi = mid;
                else { total += s16(st, r + 4); break; }
            }
        }
        if (len < 6) break;
        at += len;
    }
    return total;
}

int32_t fa_font_kerning(const fa_font* f, uint32_t left, uint32_t right)
{
    if (!f) return 0;
    if (f->kern_lookup_count) return gpos_kerning(f, left, right);
    return f->kern.n ? kern_table(f, left, right) : 0;
}

static void find_kern_lookups(fa_font* f)
{
    view g = f->gpos, features;
    uint32_t n, i, j, k;
    if (u16(g, 0) != 1) return;
    features = from(g, u16(g, 6));
    f->gpos_lookups = from(g, u16(g, 8));
    n = u16(features, 0);
    for (i = 0; i < n; i++) {
        size_t r = 2 + (size_t)i * 6;
        view feat;
        uint32_t count;
        if (u32(features, r) != tag("kern")) continue;
        feat = from(features, u16(features, r + 4));
        count = u16(feat, 2);
        for (j = 0; j < count; j++) {
            uint32_t idx = u16(feat, 4 + (size_t)j * 2);
            if (idx >= u16(f->gpos_lookups, 0)) continue;
            for (k = 0; k < f->kern_lookup_count && f->kern_lookups[k] != idx; k++) {}
            if (k == f->kern_lookup_count && k < KERN_MAX_LOOKUPS) f->kern_lookups[f->kern_lookup_count++] = idx;
        }
    }
    /* Lookups apply in list order. */
    for (i = 1; i < f->kern_lookup_count; i++)
        for (j = i; j > 0 && f->kern_lookups[j - 1] > f->kern_lookups[j]; j--) {
            uint32_t t = f->kern_lookups[j];
            f->kern_lookups[j] = f->kern_lookups[j - 1];
            f->kern_lookups[j - 1] = t;
        }
}

/* ---- paths ---------------------------------------------------------------------- */

typedef struct xform {
    float a, b, c, d, dx, dy;         /* x' = a x + c y + dx, y' = b x + d y + dy */
} xform;

static const xform k_identity = { 1, 0, 0, 1, 0, 0 };

static void apply(const xform* t, float x, float y, float* ox, float* oy)
{
    *ox = t->a * x + t->c * y + t->dx;
    *oy = t->b * x + t->d * y + t->dy;
}

static void grow_box(fa_path* p, float x, float y)
{
    if (p->count == 0 && p->xmin > p->xmax) { p->xmin = p->xmax = x; p->ymin = p->ymax = y; return; }
    if (x < p->xmin) p->xmin = x;
    if (x > p->xmax) p->xmax = x;
    if (y < p->ymin) p->ymin = y;
    if (y > p->ymax) p->ymax = y;
}

/* Appends a command with points in font units through `t`; 0 when out of room. */
static int emit(fa_path* p, const xform* t, int op, float x1, float y1, float x2, float y2, float x, float y)
{
    fa_path_cmd* c;
    if (!(fabsf(x) < FONT_MAX_COORD && fabsf(y) < FONT_MAX_COORD && fabsf(x1) < FONT_MAX_COORD &&
          fabsf(y1) < FONT_MAX_COORD && fabsf(x2) < FONT_MAX_COORD && fabsf(y2) < FONT_MAX_COORD))
        return 0;                     /* also NaN */
    if (p->count >= FONT_MAX_COMMANDS) return 0;
    if (p->count == p->cap) {
        size_t cap = p->cap ? p->cap * 2 : 64;
        fa_path_cmd* n = (fa_path_cmd*)realloc(p->cmd, cap * sizeof(*n));
        if (!n) return 0;
        p->cmd = n;
        p->cap = cap;
    }
    c = &p->cmd[p->count];
    memset(c, 0, sizeof(*c));
    c->op = (uint8_t)op;
    apply(t, x, y, &c->x, &c->y);
    grow_box(p, c->x, c->y);
    if (op == FA_PATH_QUAD || op == FA_PATH_CUBIC) { apply(t, x1, y1, &c->x1, &c->y1); grow_box(p, c->x1, c->y1); }
    if (op == FA_PATH_CUBIC) { apply(t, x2, y2, &c->x2, &c->y2); grow_box(p, c->x2, c->y2); }
    p->count++;
    return 1;
}

void fa_path_clear(fa_path* p)
{
    p->count = 0;
    p->xmin = p->ymin = 1.0f;
    p->xmax = p->ymax = 0.0f;           /* empty: min > max */
}

void fa_path_free(fa_path* p)
{
    free(p->cmd);
    memset(p, 0, sizeof(*p));
}

/* ---- glyf ----------------------------------------------------------------------- */

static view glyf_data(const fa_font* f, uint32_t g)
{
    view none = { NULL, 0 };
    uint32_t a, b;
    if (g >= f->info.glyph_count) return none;
    if (f->loca_long) { a = u32(f->loca, (size_t)g * 4); b = u32(f->loca, (size_t)g * 4 + 4); }
    else { a = u16(f->loca, (size_t)g * 2) * 2; b = u16(f->loca, (size_t)g * 2 + 2) * 2; }
    if (b <= a) return none;
    return sub(f->glyf, a, b - a);
}

/* One simple glyph's contours. */
static fa_result glyf_simple(view gd, int contours, fa_path* p, const xform* t)
{
    uint32_t points, i, flags_at, ilen;
    uint8_t* fl;
    float *xs, *ys;
    size_t at;
    int32_t v = 0;
    int c, ok = 1;
    if (!has(gd, 10, (size_t)contours * 2 + 2)) return FA_ERROR_UNSUPPORTED_FORMAT;
    points = u16(gd, 10 + (size_t)(contours - 1) * 2) + 1;
    for (c = 1; c < contours; c++)
        if (u16(gd, 10 + (size_t)c * 2) < u16(gd, 10 + (size_t)(c - 1) * 2)) return FA_ERROR_UNSUPPORTED_FORMAT;
    if (points > FONT_MAX_POINTS) return FA_ERROR_UNSUPPORTED_FORMAT;
    ilen = u16(gd, 10 + (size_t)contours * 2);
    flags_at = 12 + (uint32_t)contours * 2 + ilen;
    fl = (uint8_t*)malloc(points);
    xs = (float*)malloc(points * sizeof(float));
    ys = (float*)malloc(points * sizeof(float));
    if (!fl || !xs || !ys) { free(fl); free(xs); free(ys); return FA_ERROR_OUT_OF_MEMORY; }
    at = flags_at;
    for (i = 0; i < points;) {
        uint32_t b = u8(gd, at++), rep = 0;
        if (b & 8) rep = u8(gd, at++);
        if (!has(gd, at - 1, 1)) { ok = 0; break; }
        fl[i++] = (uint8_t)b;
        while (rep-- && i < points) fl[i++] = (uint8_t)b;
    }
    for (i = 0; ok && i < points; i++) {
        uint32_t b = fl[i];
        if (b & 2) { int32_t d = (int32_t)u8(gd, at++); v += b & 16 ? d : -d; }
        else if (!(b & 16)) { v += s16(gd, at); at += 2; }
        xs[i] = (float)v;
    }
    v = 0;
    for (i = 0; ok && i < points; i++) {
        uint32_t b = fl[i];
        if (b & 4) { int32_t d = (int32_t)u8(gd, at++); v += b & 32 ? d : -d; }
        else if (!(b & 32)) { v += s16(gd, at); at += 2; }
        ys[i] = (float)v;
    }
    if (ok && at > gd.n) ok = 0;
    for (c = 0, i = 0; ok && c < contours; c++) {
        uint32_t end = u16(gd, 10 + (size_t)c * 2), first = i, n, k, s0;
        float sx, sy, cx = 0, cy = 0;
        int pending = 0, any_on;
        if (end < first) continue;    /* an empty contour */
        n = end - first + 1;
        /* Start on the first on-curve point, else between the last and first points. */
        for (s0 = 0; s0 < n && !(fl[first + s0] & 1); s0++) {}
        any_on = s0 < n;
        if (any_on) { sx = xs[first + s0]; sy = ys[first + s0]; }
        else { s0 = n - 1; sx = (xs[first] + xs[end]) * 0.5f; sy = (ys[first] + ys[end]) * 0.5f; }
        ok = emit(p, t, FA_PATH_MOVE, 0, 0, 0, 0, sx, sy);
        for (k = 1; ok && k <= n; k++) {
            uint32_t idx = first + (s0 + k) % n;
            float x = xs[idx], y = ys[idx];
            if (fl[idx] & 1) {
                ok = pending ? emit(p, t, FA_PATH_QUAD, cx, cy, 0, 0, x, y) : emit(p, t, FA_PATH_LINE, 0, 0, 0, 0, x, y);
                pending = 0;
            } else {
                if (pending) ok = emit(p, t, FA_PATH_QUAD, cx, cy, 0, 0, (cx + x) * 0.5f, (cy + y) * 0.5f);
                cx = x;
                cy = y;
                pending = 1;
            }
        }
        if (ok && pending) ok = emit(p, t, FA_PATH_QUAD, cx, cy, 0, 0, sx, sy);
        i = end + 1;
    }
    free(fl);
    free(xs);
    free(ys);
    return ok ? FA_OK : FA_ERROR_UNSUPPORTED_FORMAT;
}

static fa_result glyf_outline(const fa_font* f, uint32_t g, fa_path* p, const xform* t, int depth, int* budget)
{
    view gd = glyf_data(f, g);
    int contours;
    size_t at = 10;
    uint32_t flags;
    if (!gd.n) return FA_OK;           /* empty glyph */
    contours = s16(gd, 0);
    if (contours > 0) return glyf_simple(gd, contours, p, t);
    if (contours == 0) return FA_OK;
    if (depth >= FONT_MAX_DEPTH) return FA_ERROR_UNSUPPORTED_FORMAT;
    do {
        uint32_t child;
        float a1, a2, sa = 1, sb = 0, sc = 0, sd = 1;
        xform ct;
        fa_result r;
        if (!has(gd, at, 4) || --*budget < 0) return FA_ERROR_UNSUPPORTED_FORMAT;
        flags = u16(gd, at);
        child = u16(gd, at + 2);
        at += 4;
        if (flags & 1) { a1 = (float)s16(gd, at); a2 = (float)s16(gd, at + 2); at += 4; }
        else { a1 = (float)(int8_t)u8(gd, at); a2 = (float)(int8_t)u8(gd, at + 1); at += 2; }
        if (flags & 8) { sa = sd = (float)s16(gd, at) / 16384.0f; at += 2; }
        else if (flags & 0x40) { sa = (float)s16(gd, at) / 16384.0f; sd = (float)s16(gd, at + 2) / 16384.0f; at += 4; }
        else if (flags & 0x80) {
            sa = (float)s16(gd, at) / 16384.0f; sb = (float)s16(gd, at + 2) / 16384.0f;
            sc = (float)s16(gd, at + 4) / 16384.0f; sd = (float)s16(gd, at + 6) / 16384.0f; at += 8;
        }
        if (!(flags & 2)) a1 = a2 = 0;  /* matched points: not supported, placed at the origin */
        else if (flags & 0x800 && !(flags & 0x1000)) {
            /* a scaled offset (Apple's convention) */
            float x = a1, y = a2;
            a1 = sa * x + sc * y;
            a2 = sb * x + sd * y;
        }
        /* The child's own transform, then the parent's. */
        ct.a = t->a * sa + t->c * sb;
        ct.b = t->b * sa + t->d * sb;
        ct.c = t->a * sc + t->c * sd;
        ct.d = t->b * sc + t->d * sd;
        apply(t, a1, a2, &ct.dx, &ct.dy);
        r = glyf_outline(f, child, p, &ct, depth + 1, budget);
        if (r != FA_OK) return r;
    } while (flags & 0x20);
    return FA_OK;
}

/* ---- CFF ------------------------------------------------------------------------ */

/* An INDEX at `at`: its count, and where it ends (0 when it does not fit). */
static uint32_t index_count(view v, size_t at) { return u16(v, at); }

static size_t index_end(view v, size_t at)
{
    uint32_t count = u16(v, at), os = u8(v, at + 2), last;
    size_t data;
    if (!count) return has(v, at, 2) ? at + 2 : 0;
    if (os < 1 || os > 4) return 0;
    data = at + 3 + (size_t)(count + 1) * os - 1;
    last = os == 1 ? u8(v, at + 3 + (size_t)count) : os == 2 ? u16(v, at + 3 + (size_t)count * 2)
         : os == 3 ? u24(v, at + 3 + (size_t)count * 3) : u32(v, at + 3 + (size_t)count * 4);
    return has(v, data, last) ? data + last : 0;
}

static view index_get(view v, size_t at, uint32_t i)
{
    view none = { NULL, 0 };
    uint32_t count = u16(v, at), os = u8(v, at + 2), a, b;
    size_t offs = at + 3, data;
    if (i >= count || os < 1 || os > 4) return none;
    data = offs + (size_t)(count + 1) * os - 1;
#define OFF(k) (os == 1 ? u8(v, offs + (size_t)(k)) : os == 2 ? u16(v, offs + (size_t)(k) * 2) \
                : os == 3 ? u24(v, offs + (size_t)(k) * 3) : u32(v, offs + (size_t)(k) * 4))
    a = OFF(i);
    b = OFF(i + 1);
#undef OFF
    if (b < a || a < 1) return none;
    return sub(v, data + a, b - a);
}

/* Parses a DICT and returns the operands of `op` (two-byte ops as 1200 + second byte). */
static int dict_find(view d, uint32_t op, double* args, int max)
{
    double stack[48];
    int n = 0;
    size_t at = 0;
    while (at < d.n) {
        uint32_t b = u8(d, at);
        if (b <= 21) {
            uint32_t o = b;
            at++;
            if (b == 12) o = 1200 + u8(d, at++);
            if (o == op) {
                int i;
                for (i = 0; i < n && i < max; i++) args[i] = stack[i];
                return n;
            }
            n = 0;
            continue;
        }
        if (n >= 48) return 0;
        if (b >= 32 && b <= 246) { stack[n++] = (double)((int32_t)b - 139); at++; }
        else if (b >= 247 && b <= 250) { stack[n++] = (double)(((int32_t)b - 247) * 256 + (int32_t)u8(d, at + 1) + 108); at += 2; }
        else if (b >= 251 && b <= 254) { stack[n++] = (double)(-((int32_t)b - 251) * 256 - (int32_t)u8(d, at + 1) - 108); at += 2; }
        else if (b == 28) { stack[n++] = (double)s16(d, at + 1); at += 3; }
        else if (b == 29) { stack[n++] = (double)(int32_t)u32(d, at + 1); at += 5; }
        else if (b == 30) {
            /* A real number: nibbles, read only to skip (no needed operator takes one). */
            stack[n++] = 0;
            at++;
            while (at < d.n) {
                uint32_t v = u8(d, at++);
                if ((v & 0x0F) == 0x0F || (v >> 4) == 0x0F) break;
            }
        } else return 0;
    }
    return 0;
}

static view private_subrs(view cff, view top_or_fd)
{
    view none = { NULL, 0 };
    double a[2], s[1];
    view priv;
    if (dict_find(top_or_fd, 18, a, 2) != 2 || a[0] < 0 || a[1] < 0) return none;
    priv = sub(cff, (size_t)a[1], (size_t)a[0]);
    if (dict_find(priv, 19, s, 1) != 1 || s[0] <= 0) return none;
    if (!priv.p) return none;
    return from(cff, (size_t)(priv.p - cff.p) + (size_t)s[0]);
}

static fa_result cff_open(fa_font* f, view cff)
{
    size_t names, tops, strings, gsubrs;
    double a[2];
    view top;
    cff_font* c = &f->cff;
    if (u8(cff, 0) != 1) return FA_ERROR_UNSUPPORTED_FORMAT;   /* CFF2 is not read */
    names = u8(cff, 2);
    tops = index_end(cff, names);
    if (!tops) return FA_ERROR_UNSUPPORTED_FORMAT;
    strings = index_end(cff, tops);
    if (!strings) return FA_ERROR_UNSUPPORTED_FORMAT;
    gsubrs = index_end(cff, strings);
    if (!gsubrs || !index_end(cff, gsubrs)) return FA_ERROR_UNSUPPORTED_FORMAT;
    top = index_get(cff, tops, 0);
    if (dict_find(top, 1206, a, 1) == 1 && a[0] != 2) return FA_ERROR_UNSUPPORTED_FORMAT;
    if (dict_find(top, 17, a, 1) != 1 || a[0] <= 0) return FA_ERROR_UNSUPPORTED_FORMAT;
    c->cff = cff;
    c->charstrings = (size_t)a[0];
    if (!index_end(cff, c->charstrings)) return FA_ERROR_UNSUPPORTED_FORMAT;
    c->gsubrs = gsubrs;
    c->gsubr_count = index_count(cff, gsubrs);
    if (dict_find(top, 1230, a, 2) > 0) {
        double fd[1], sel[1];
        c->cid = 1;
        if (dict_find(top, 1236, fd, 1) != 1 || dict_find(top, 1237, sel, 1) != 1 || fd[0] <= 0 || sel[0] <= 0)
            return FA_ERROR_UNSUPPORTED_FORMAT;
        c->fdarray = (size_t)fd[0];
        if (!index_end(cff, c->fdarray)) return FA_ERROR_UNSUPPORTED_FORMAT;
        c->fdselect = from(cff, (size_t)sel[0]);
    } else {
        c->lsubrs = private_subrs(cff, top);
    }
    return FA_OK;
}

static uint32_t fd_of(const cff_font* c, uint32_t g)
{
    view s = c->fdselect;
    uint32_t format = u8(s, 0);
    if (format == 0) return u8(s, 1 + (size_t)g);
    if (format == 3) {
        uint32_t n = u16(s, 1), lo = 0, hi = n;
        while (lo < hi) {               /* the last range whose first glyph is <= g */
            uint32_t mid = (lo + hi) / 2;
            if (u16(s, 3 + (size_t)mid * 3) <= g) lo = mid + 1; else hi = mid;
        }
        if (lo == 0 || g >= u16(s, 3 + (size_t)n * 3)) return 0;
        return u8(s, 3 + (size_t)(lo - 1) * 3 + 2);
    }
    return 0;
}

static uint32_t subr_bias(uint32_t count) { return count < 1240 ? 107 : count < 33900 ? 1131 : 32768; }

typedef struct t2 {
    const cff_font* c;
    view lsubrs;
    uint32_t lsubr_count;
    fa_path* p;
    const xform* t;
    float st[CFF_MAX_STACK];
    int sp;
    float x, y;
    int open;                         /* a contour is open */
    int stems;
    int ops;
    int done;
    int failed;
} t2;

static void t2_move(t2* s, float dx, float dy)
{
    s->x += dx;
    s->y += dy;
    if (!emit(s->p, s->t, FA_PATH_MOVE, 0, 0, 0, 0, s->x, s->y)) s->failed = 1;
    s->open = 1;
}

static void t2_line(t2* s, float dx, float dy)
{
    s->x += dx;
    s->y += dy;
    if (!s->open) s->failed = 1;      /* drawing before a moveto */
    else if (!emit(s->p, s->t, FA_PATH_LINE, 0, 0, 0, 0, s->x, s->y)) s->failed = 1;
}

static void t2_curve(t2* s, float dx1, float dy1, float dx2, float dy2, float dx3, float dy3)
{
    float x1 = s->x + dx1, y1 = s->y + dy1, x2 = x1 + dx2, y2 = y1 + dy2;
    s->x = x2 + dx3;
    s->y = y2 + dy3;
    if (!s->open) s->failed = 1;
    else if (!emit(s->p, s->t, FA_PATH_CUBIC, x1, y1, x2, y2, s->x, s->y)) s->failed = 1;
}

static void t2_run(t2* s, view cs, int depth)
{
    size_t at = 0;
    if (depth > CFF_MAX_SUBR_DEPTH) { s->failed = 1; return; }
    while (at < cs.n && !s->done && !s->failed) {
        uint32_t b = u8(cs, at++);
        float* a = s->st;
        int n = s->sp, i = 0;
        if (++s->ops > CFF_MAX_OPS) { s->failed = 1; return; }
        if (b >= 32 || b == 28) {
            float v;
            if (b == 28) { v = (float)s16(cs, at); at += 2; }
            else if (b <= 246) v = (float)((int32_t)b - 139);
            else if (b <= 250) { v = (float)(((int32_t)b - 247) * 256 + (int32_t)u8(cs, at) + 108); at++; }
            else if (b <= 254) { v = (float)(-((int32_t)b - 251) * 256 - (int32_t)u8(cs, at) - 108); at++; }
            else { v = (float)(int32_t)u32(cs, at) / 65536.0f; at += 4; }
            if (at > cs.n || s->sp >= CFF_MAX_STACK) { s->failed = 1; return; }
            s->st[s->sp++] = v;
            continue;
        }
        switch (b) {
        case 1: case 3: case 18: case 23:          /* stems */
            s->stems += n / 2;
            break;
        case 19: case 20:                          /* hintmask, cntrmask */
            s->stems += n / 2;
            at += (size_t)(s->stems + 7) / 8;
            break;
        case 21:                                   /* rmoveto */
            if (n < 2) { s->failed = 1; return; }
            t2_move(s, a[n - 2], a[n - 1]);
            break;
        case 22:                                   /* hmoveto */
            if (n < 1) { s->failed = 1; return; }
            t2_move(s, a[n - 1], 0);
            break;
        case 4:                                    /* vmoveto */
            if (n < 1) { s->failed = 1; return; }
            t2_move(s, 0, a[n - 1]);
            break;
        case 5:                                    /* rlineto */
            for (; i + 1 < n; i += 2) t2_line(s, a[i], a[i + 1]);
            break;
        case 6: case 7: {                          /* hlineto, vlineto */
            int h = b == 6;
            for (; i < n; i++, h = !h) t2_line(s, h ? a[i] : 0, h ? 0 : a[i]);
            break;
        }
        case 8:                                    /* rrcurveto */
            for (; i + 5 < n; i += 6) t2_curve(s, a[i], a[i + 1], a[i + 2], a[i + 3], a[i + 4], a[i + 5]);
            break;
        case 27: {                                 /* hhcurveto */
            float dy1 = 0;
            if (n & 1) dy1 = a[i++];
            for (; i + 3 < n; i += 4, dy1 = 0) t2_curve(s, a[i], dy1, a[i + 1], a[i + 2], a[i + 3], 0);
            break;
        }
        case 26: {                                 /* vvcurveto */
            float dx1 = 0;
            if (n & 1) dx1 = a[i++];
            for (; i + 3 < n; i += 4, dx1 = 0) t2_curve(s, dx1, a[i], a[i + 1], a[i + 2], 0, a[i + 3]);
            break;
        }
        case 30: case 31: {                        /* vhcurveto, hvcurveto */
            int h = b == 31;
            for (; i + 3 < n; i += 4, h = !h) {
                float last = n - i == 5 ? a[i + 4] : 0;
                if (h) t2_curve(s, a[i], 0, a[i + 1], a[i + 2], last, a[i + 3]);
                else t2_curve(s, 0, a[i], a[i + 1], a[i + 2], a[i + 3], last);
            }
            break;
        }
        case 24:                                   /* rcurveline */
            for (; i + 7 < n; i += 6) t2_curve(s, a[i], a[i + 1], a[i + 2], a[i + 3], a[i + 4], a[i + 5]);
            if (i + 1 < n) t2_line(s, a[i], a[i + 1]);
            break;
        case 25:                                   /* rlinecurve */
            for (; i + 7 < n; i += 2) t2_line(s, a[i], a[i + 1]);
            if (i + 5 < n) t2_curve(s, a[i], a[i + 1], a[i + 2], a[i + 3], a[i + 4], a[i + 5]);
            break;
        case 10: case 29: {                        /* callsubr, callgsubr */
            int32_t idx;
            view body;
            if (n < 1) { s->failed = 1; return; }
            s->sp--;
            if (!(fabsf(a[n - 1]) < 65536.0f)) { s->failed = 1; return; }
            if (b == 10) {
                idx = (int32_t)a[n - 1] + (int32_t)subr_bias(s->lsubr_count);
                body = idx >= 0 ? index_get(s->lsubrs, 0, (uint32_t)idx) : k_none;
            } else {
                idx = (int32_t)a[n - 1] + (int32_t)subr_bias(s->c->gsubr_count);
                body = idx >= 0 ? index_get(s->c->cff, s->c->gsubrs, (uint32_t)idx) : k_none;
            }
            if (!body.p) { s->failed = 1; return; }
            t2_run(s, body, depth + 1);
            continue;                              /* the stack carries on */
        }
        case 11:                                   /* return */
            return;
        case 14:                                   /* endchar (seac accents are not drawn) */
            s->done = 1;
            break;
        case 12: {
            uint32_t e = u8(cs, at++);
            float sx = s->x, sy = s->y;
            switch (e) {
            case 0:                                /* dotsection: a hint, nothing to draw */
                break;
            case 35:                               /* flex */
                if (n < 13) { s->failed = 1; return; }
                t2_curve(s, a[0], a[1], a[2], a[3], a[4], a[5]);
                t2_curve(s, a[6], a[7], a[8], a[9], a[10], a[11]);
                break;
            case 34:                               /* hflex */
                if (n < 7) { s->failed = 1; return; }
                t2_curve(s, a[0], 0, a[1], a[2], a[3], 0);
                t2_curve(s, a[4], 0, a[5], sy - s->y, a[6], 0);
                break;
            case 36:                               /* hflex1 */
                if (n < 9) { s->failed = 1; return; }
                t2_curve(s, a[0], a[1], a[2], a[3], a[4], 0);
                t2_curve(s, a[5], 0, a[6], a[7], a[8], sy - (s->y + a[7]));
                break;
            case 37: {                             /* flex1 */
                float dx = 0, dy = 0;
                int k;
                if (n < 11) { s->failed = 1; return; }
                for (k = 0; k < 10; k += 2) { dx += a[k]; dy += a[k + 1]; }
                t2_curve(s, a[0], a[1], a[2], a[3], a[4], a[5]);
                if (fabsf(dx) > fabsf(dy)) t2_curve(s, a[6], a[7], a[8], a[9], a[10], sy - s->y - a[7] - a[9]);
                else t2_curve(s, a[6], a[7], a[8], a[9], sx - s->x - a[6] - a[8], a[10]);
                break;
            }
            case 9: if (n >= 1) { a[n - 1] = fabsf(a[n - 1]); continue; } s->failed = 1; return;      /* abs */
            case 10: if (n >= 2) { a[n - 2] += a[n - 1]; s->sp--; continue; } s->failed = 1; return;  /* add */
            case 11: if (n >= 2) { a[n - 2] -= a[n - 1]; s->sp--; continue; } s->failed = 1; return;  /* sub */
            case 12: if (n >= 2 && a[n - 1] != 0) { a[n - 2] /= a[n - 1]; s->sp--; continue; } s->failed = 1; return;
            case 14: if (n >= 1) { a[n - 1] = -a[n - 1]; continue; } s->failed = 1; return;           /* neg */
            case 18: if (n >= 1) { s->sp--; continue; } s->failed = 1; return;                        /* drop */
            case 24: if (n >= 2) { a[n - 2] *= a[n - 1]; s->sp--; continue; } s->failed = 1; return;  /* mul */
            case 26: if (n >= 1 && a[n - 1] >= 0) { a[n - 1] = sqrtf(a[n - 1]); continue; } s->failed = 1; return;
            case 27: if (n >= 1 && n < CFF_MAX_STACK) { a[n] = a[n - 1]; s->sp++; continue; } s->failed = 1; return;
            case 28: if (n >= 2) { float t = a[n - 1]; a[n - 1] = a[n - 2]; a[n - 2] = t; continue; } s->failed = 1; return;
            default:
                s->failed = 1;                     /* other operators (put, get, random, ...) are not used by fonts in practice */
                return;
            }
            break;
        }
        default:
            s->failed = 1;
            return;
        }
        s->sp = 0;
    }
}

static fa_result cff_outline(const fa_font* f, uint32_t g, fa_path* p, const xform* t)
{
    const cff_font* c = &f->cff;
    t2 s;
    view cs = index_get(c->cff, c->charstrings, g);
    if (!cs.p) return FA_ERROR_UNSUPPORTED_FORMAT;
    memset(&s, 0, sizeof(s));
    s.c = c;
    s.p = p;
    s.t = t;
    if (c->cid) {
        uint32_t fd = fd_of(c, g);
        view fdd = index_get(c->cff, c->fdarray, fd);
        s.lsubrs = private_subrs(c->cff, fdd);
    } else {
        s.lsubrs = c->lsubrs;
    }
    s.lsubr_count = index_count(s.lsubrs, 0);
    t2_run(&s, cs, 0);
    return s.failed ? FA_ERROR_UNSUPPORTED_FORMAT : FA_OK;
}

fa_result fa_font_outline(const fa_font* f, uint32_t g, fa_path* p)
{
    fa_result r;
    if (!f || !p) return FA_ERROR_INVALID_ARG;
    fa_path_clear(p);
    if (g >= f->info.glyph_count) return FA_ERROR_INVALID_ARG;
    if (f->info.cff) {
        r = cff_outline(f, g, p, &k_identity);
    } else {
        int budget = FONT_MAX_COMPONENTS;
        r = glyf_outline(f, g, p, &k_identity, 0, &budget);
    }
    if (r != FA_OK) fa_path_clear(p);
    return r;
}

/* ---- opening -------------------------------------------------------------------- */

static void read_info(fa_font* f)
{
    fa_font_info* in = &f->info;
    uint32_t fs = 0;
    in->units_per_em = (int)u16(f->head, 18);
    if (in->units_per_em < 16 || in->units_per_em > 16384) in->units_per_em = 1000;
    in->ascender = s16(f->hhea, 4);
    in->descender = s16(f->hhea, 6);
    in->line_gap = s16(f->hhea, 8);
    in->weight = 400;
    in->width = 5;
    if (u16(f->head, 44) & 1) { in->bold = 1; in->weight = 700; }
    if (u16(f->head, 44) & 2) in->italic = 1;
    if (f->os2.n >= 78) {
        uint32_t w = u16(f->os2, 4);
        fs = u16(f->os2, 62);
        if (w >= 1 && w <= 1000) in->weight = (int)(w < 10 ? w * 100 : w);
        w = u16(f->os2, 6);
        if (w >= 1 && w <= 9) in->width = (int)w;
        in->italic = fs & 1 ? 1 : (fs & 0x200 ? 1 : 0);
        in->bold = fs & 0x20 ? 1 : 0;
        if (fs & 0x80 || (!in->ascender && !in->descender)) {
            in->ascender = s16(f->os2, 68);
            in->descender = s16(f->os2, 70);
            in->line_gap = s16(f->os2, 72);
        }
        if (u16(f->os2, 0) >= 2 && f->os2.n >= 90) {
            in->x_height = s16(f->os2, 86);
            in->cap_height = s16(f->os2, 88);
        }
    }
    if (in->descender > 0) in->descender = -in->descender;
    /* A damaged face's metrics can make a line thousands of ems tall (lyric layers are
     * sized from them): outside 0.5-3 em, or an ascender outside 0-2.5 em, the usual
     * 0.9 em ascender and 1.2 em line instead. */
    {
        int u = in->units_per_em, a = in->ascender, h = in->ascender - in->descender;
        if (2 * h < u || h > 3 * u || a < 0 || 2 * a > 5 * u) {
            in->ascender = (9 * u + 5) / 10;
            in->descender = in->ascender - (12 * u + 5) / 10;
        }
        if (in->line_gap < 0 || in->line_gap > 2 * u) in->line_gap = 0;
    }
    in->monospace = u32(f->post, 12) != 0;
    read_name(f->name, 1, in->legacy_family, sizeof(in->legacy_family));
    if (!read_name(f->name, 16, in->family, sizeof(in->family))) memcpy(in->family, in->legacy_family, sizeof(in->family));
    if (!read_name(f->name, 17, in->style, sizeof(in->style))) read_name(f->name, 2, in->style, sizeof(in->style));
}

fa_result fa_font_open(const uint8_t* data, size_t size, uint32_t index, int owned, fa_font** out)
{
    view file = { data, size }, face;
    fa_font* f;
    uint32_t v, tables, i;
    size_t base = 0;
    view cff = { NULL, 0 };
    fa_result r = FA_ERROR_UNSUPPORTED_FORMAT;
    if (!out) { if (owned) free((void*)data); return FA_ERROR_INVALID_ARG; }
    *out = NULL;
    if (!data) return FA_ERROR_INVALID_ARG;
    if (index >= fa_font_face_count(data, size)) { if (owned) free((void*)data); return index ? FA_ERROR_NOT_FOUND : FA_ERROR_UNSUPPORTED_FORMAT; }
    if (u32(file, 0) == tag("ttcf")) base = u32(file, 12 + (size_t)index * 4);
    face = from(file, base);
    v = u32(face, 0);
    if (!is_sfnt(v)) { if (owned) free((void*)data); return FA_ERROR_UNSUPPORTED_FORMAT; }
    f = (fa_font*)calloc(1, sizeof(*f));
    if (!f) { if (owned) free((void*)data); return FA_ERROR_OUT_OF_MEMORY; }
    f->file = file;
    f->owned = owned;
    tables = u16(face, 4);
    for (i = 0; i < tables; i++) {
        size_t rec = 12 + (size_t)i * 16;
        uint32_t t = u32(face, rec), off = u32(face, rec + 8), len = u32(face, rec + 12);
        /* Table offsets count from the start of the file, also in a collection. */
        view tv = sub(file, off, len);
        if (!has(face, rec, 16)) break;
        if (t == tag("head")) f->head = tv;
        else if (t == tag("hhea")) f->hhea = tv;
        else if (t == tag("hmtx")) f->hmtx = tv;
        else if (t == tag("maxp")) f->maxp = tv;
        else if (t == tag("loca")) f->loca = tv;
        else if (t == tag("glyf")) f->glyf = tv;
        else if (t == tag("cmap")) f->cmap = tv;
        else if (t == tag("kern")) f->kern = tv;
        else if (t == tag("GPOS")) f->gpos = tv;
        else if (t == tag("OS/2")) f->os2 = tv;
        else if (t == tag("name")) f->name = tv;
        else if (t == tag("post")) f->post = tv;
        else if (t == tag("CFF ")) cff = tv;
    }
    f->info.glyph_count = u16(f->maxp, 4);
    f->hmetrics = u16(f->hhea, 34);
    if (f->hmetrics > f->info.glyph_count) f->hmetrics = f->info.glyph_count;
    if ((size_t)f->hmetrics * 4 > f->hmtx.n) f->hmetrics = (uint32_t)(f->hmtx.n / 4);
    if (!f->head.n || !f->hhea.n || !f->maxp.n || !f->cmap.n || !f->info.glyph_count) goto fail;
    pick_cmap(f, f->cmap);
    if (!f->cmap_format) goto fail;
    if (cff.n) {
        r = cff_open(f, cff);
        if (r != FA_OK) goto fail;
        f->info.cff = 1;
        if (index_count(cff, f->cff.charstrings) < f->info.glyph_count) f->info.glyph_count = index_count(cff, f->cff.charstrings);
    } else {
        uint32_t need;
        r = FA_ERROR_UNSUPPORTED_FORMAT;
        if (!f->glyf.n || !f->loca.n) goto fail;  /* bitmap-only or CFF2 */
        f->loca_long = s16(f->head, 50) == 1;
        need = (f->info.glyph_count + 1) * (f->loca_long ? 4u : 2u);
        if (f->loca.n < need) {
            uint32_t fit = (uint32_t)(f->loca.n / (f->loca_long ? 4u : 2u));
            if (fit < 2) goto fail;
            f->info.glyph_count = fit - 1;
        }
    }
    read_info(f);
    if (f->gpos.n) find_kern_lookups(f);
    *out = f;
    return FA_OK;
fail:
    if (r == FA_OK) r = FA_ERROR_UNSUPPORTED_FORMAT;
    fa_font_close(f);
    return r;
}

fa_result fa_font_open_file(const char* path, uint32_t index, fa_font** out)
{
    FILE* fp;
    long len;
    uint8_t* data;
    if (!out) return FA_ERROR_INVALID_ARG;
    *out = NULL;
    if (!path) return FA_ERROR_INVALID_ARG;
    fp = fa_fopen_utf8(path, "rb");
    if (!fp) return FA_ERROR_NOT_FOUND;
    if (fseek(fp, 0, SEEK_END) != 0 || (len = ftell(fp)) < 12 || (unsigned long)len > FONT_MAX_FILE || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return FA_ERROR_UNSUPPORTED_FORMAT;
    }
    data = (uint8_t*)malloc((size_t)len);
    if (!data) { fclose(fp); return FA_ERROR_OUT_OF_MEMORY; }
    if (fread(data, 1, (size_t)len, fp) != (size_t)len) { fclose(fp); free(data); return FA_ERROR_IO; }
    fclose(fp);
    return fa_font_open(data, (size_t)len, index, 1, out);
}

/* Reads `len` bytes at `off` of the file into a new buffer (NULL when it cannot). */
static uint8_t* read_at(FILE* fp, uint32_t off, uint32_t len)
{
    uint8_t* b;
    if (!len || len > 4u * 1024u * 1024u || fseek(fp, (long)off, SEEK_SET) != 0) return NULL;
    b = (uint8_t*)malloc(len);
    if (b && fread(b, 1, len, fp) != len) { free(b); b = NULL; }
    return b;
}

fa_result fa_font_probe(FILE* fp, uint32_t* face_count, uint32_t index, fa_font_info* out)
{
    uint8_t hdr[12], *dir = NULL, *bufs[5] = { NULL, NULL, NULL, NULL, NULL };
    static const char* want[5] = { "head", "hhea", "OS/2", "name", "post" };
    view h = { hdr, sizeof(hdr) }, d;
    uint32_t base = 0, tables, i, k, faces = 1;
    fa_font tmp;
    fa_result r = FA_ERROR_UNSUPPORTED_FORMAT;
    if (fseek(fp, 0, SEEK_SET) != 0 || fread(hdr, 1, 12, fp) != 12) return FA_ERROR_UNSUPPORTED_FORMAT;
    if (u32(h, 0) == tag("ttcf")) {
        uint8_t off[4];
        view o = { off, 4 };
        faces = u32(h, 8);
        if (!faces || faces > 4096 || index >= faces) { if (face_count) *face_count = faces; return FA_ERROR_NOT_FOUND; }
        if (fseek(fp, 12 + (long)index * 4, SEEK_SET) != 0 || fread(off, 1, 4, fp) != 4) return FA_ERROR_UNSUPPORTED_FORMAT;
        base = u32(o, 0);
        if (fseek(fp, (long)base, SEEK_SET) != 0 || fread(hdr, 1, 12, fp) != 12) return FA_ERROR_UNSUPPORTED_FORMAT;
    } else if (index) {
        return FA_ERROR_NOT_FOUND;
    }
    if (face_count) *face_count = faces;
    if (!is_sfnt(u32(h, 0))) return FA_ERROR_UNSUPPORTED_FORMAT;
    tables = u16(h, 4);
    if (!tables || tables > 512) return FA_ERROR_UNSUPPORTED_FORMAT;
    dir = read_at(fp, base + 12, tables * 16);
    if (!dir) return FA_ERROR_UNSUPPORTED_FORMAT;
    d.p = dir;
    d.n = (size_t)tables * 16;
    memset(&tmp, 0, sizeof(tmp));
    for (i = 0; i < tables; i++) {
        uint32_t t = u32(d, (size_t)i * 16);
        for (k = 0; k < 5; k++)
            if (t == tag(want[k]) && !bufs[k]) {
                uint32_t len = u32(d, (size_t)i * 16 + 12);
                if (len > 65536 && k != 3) len = 65536;
                bufs[k] = read_at(fp, u32(d, (size_t)i * 16 + 8), len);
                if (bufs[k]) {
                    view v = { bufs[k], len };
                    if (k == 0) tmp.head = v; else if (k == 1) tmp.hhea = v; else if (k == 2) tmp.os2 = v;
                    else if (k == 3) tmp.name = v; else tmp.post = v;
                }
            }
    }
    if (tmp.head.n && tmp.name.n) {
        read_info(&tmp);
        *out = tmp.info;
        r = FA_OK;
    }
    for (k = 0; k < 5; k++) free(bufs[k]);
    free(dir);
    return r;
}

void fa_font_close(fa_font* f)
{
    if (!f) return;
    if (f->owned) free((void*)f->file.p);
    free(f);
}

const fa_font_info* fa_font_get_info(const fa_font* f) { return f ? &f->info : NULL; }
