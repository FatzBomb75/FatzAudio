/* Text shaping (shape.h): our own layout for simple text and the common half of the
 * OS shapers: the per-character edges a wipe needs, from the shaper's clusters. */
#include "shape.h"
#include "fa_platform.h"

#include <stdlib.h>
#include <string.h>

static fa_atomic32 g_disabled;

void fa_text_set_disabled(int disabled) { fa_store32(&g_disabled, disabled ? 1 : 0); }

const char* fa_shaper_name(void)
{
    const char* n = fa_load32(&g_disabled) ? NULL : fa_text_os_name();
    return n ? n : "";
}

void fa_text_release(void* state)
{
    if (state != FA_TEXT_STATE_FAILED) fa_text_os_release(state);
}

/* The next code point of UTF-8 at s[*i] (U+FFFD for a bad sequence): as lyric.c. */
uint32_t fa_text_next_cp(const char* s, uint32_t len, uint32_t* i)
{
    const unsigned char* u = (const unsigned char*)s;
    uint32_t c = u[*i], n, k, cp;
    if (c < 0x80) { (*i)++; return c; }
    if (c >= 0xC2 && c < 0xE0) { n = 1; cp = c & 0x1F; }
    else if (c >= 0xE0 && c < 0xF0) { n = 2; cp = c & 0x0F; }
    else if (c >= 0xF0 && c < 0xF5) { n = 3; cp = c & 0x07; }
    else { (*i)++; return 0xFFFD; }
    if (*i + n >= len) { (*i)++; return 0xFFFD; }
    for (k = 1; k <= n; k++) {
        if ((u[*i + k] & 0xC0) != 0x80) { (*i)++; return 0xFFFD; }
        cp = (cp << 6) | (u[*i + k] & 0x3F);
    }
    *i += n + 1;
    return cp;
}

/* Characters our own layout places as well as a shaper would: one glyph each, left
 * to right, no marks to position. */
static int simple_cp(uint32_t c)
{
    if (c < 0x0300) return 1;                                       /* Latin */
    if (c >= 0x0370 && c <= 0x0482) return 1;                       /* Greek, Cyrillic */
    if (c >= 0x048A && c <= 0x052F) return 1;
    if (c >= 0x1E00 && c <= 0x1FFF) return 1;                       /* precomposed Latin, Greek */
    if (c >= 0x2000 && c <= 0x200B) return 1;                       /* spaces (not ZWJ, marks, embeddings) */
    if (c >= 0x2010 && c <= 0x2027) return 1;
    if (c >= 0x2030 && c <= 0x205F) return 1;
    if (c >= 0x2070 && c <= 0x20CF) return 1;
    if (c >= 0x2100 && c <= 0x2BFF) return 1;                       /* symbols, arrows, shapes */
    if (c >= 0x2E80 && c <= 0x9FFF)                                 /* CJK, kana */
        return !(c >= 0x302A && c <= 0x302F) && !(c >= 0x3099 && c <= 0x309A);
    if (c >= 0xA000 && c <= 0xA4CF) return 1;                       /* Yi */
    if (c >= 0xAC00 && c <= 0xD7AF) return 1;                       /* Hangul */
    if (c >= 0xF900 && c <= 0xFAFF) return 1;
    if (c >= 0xFE30 && c <= 0xFE4F) return 1;
    if (c >= 0xFF00 && c <= 0xFFEF) return 1;                       /* full and half width */
    if (c >= 0x1F000 && c <= 0x1F3FA) return 1;                     /* emoji, without skin tones */
    if (c >= 0x1F400 && c <= 0x1FAFF) return 1;
    if (c >= 0x20000 && c <= 0x3FFFF) return 1;                     /* CJK extensions */
    return 0;
}

int fa_text_needs_shaping(const char* text, uint32_t len)
{
    uint32_t i = 0;
    while (i < len) {
        if ((unsigned char)text[i] < 0x80) { i++; continue; }
        if (!simple_cp(fa_text_next_cp(text, len, &i))) return 1;
    }
    return 0;
}

/* Strong right-to-left characters (bidi R and AL), roughly: Hebrew, Arabic, Syriac,
 * Thaana, NKo and the like, without their marks and Arabic digits (weak). */
static int rtl_cp(uint32_t c)
{
    if ((c >= 0x0591 && c <= 0x05C7) || (c >= 0x0600 && c <= 0x0605) || (c >= 0x0610 && c <= 0x061A) ||
        (c >= 0x064B && c <= 0x066C) || c == 0x0670 || (c >= 0x06D6 && c <= 0x06ED) || (c >= 0x06F0 && c <= 0x06F9))
        return 0;
    return (c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF) ||
           (c >= 0x10800 && c <= 0x10FFF) || (c >= 0x1E800 && c <= 0x1EFFF);
}

/* Letters and the like (bidi class L), roughly: digits, punctuation, symbols, marks
 * and spaces are not strong. */
static int ltr_cp(uint32_t c)
{
    if (c < 0x80) return (c | 0x20) >= 'a' && (c | 0x20) <= 'z';
    if (c < 0x0300) return c >= 0xC0 && c != 0xD7 && c != 0xF7;
    if (c < 0x0370) return 0;                                       /* combining marks */
    if (c < 0x0590) return c < 0x0483 || c > 0x0489;
    if (c >= 0x0900 && c < 0x2000) return 1;                        /* Indic, Southeast Asian, ... (their marks too) */
    if (c >= 0x2C00 && c < 0x2E00) return 1;
    if (c >= 0x3040 && c < 0xD800) return 1;
    if (c >= 0xF900 && c < 0xFB1D) return 1;
    if (c >= 0xFF21 && c < 0xFFE0) return !(c > 0xFF3A && c < 0xFF41) && !(c > 0xFF5A && c < 0xFF66);
    return c >= 0x10000 && !rtl_cp(c) && !(c >= 0x1F000 && c < 0x20000);
}

int fa_text_rtl_paragraph(const char* text, uint32_t len)
{
    uint32_t i = 0;
    while (i < len) {
        uint32_t c = fa_text_next_cp(text, len, &i);
        if (rtl_cp(c)) return 1;
        if (ltr_cp(c)) return 0;
    }
    return 0;
}

void fa_text_visual_order(const uint8_t* levels, uint32_t n, uint32_t* order)
{
    uint32_t i, a, b;
    uint8_t hi = 0, lo = 255, lv;
    for (i = 0; i < n; i++) {
        order[i] = i;
        if (levels[i] > hi) hi = levels[i];
        if (levels[i] < lo) lo = levels[i];
    }
    if (!(lo & 1)) lo++;
    /* From the highest level down to the lowest odd one: reverse every sequence of
     * runs at that level or higher. */
    for (lv = hi; lv >= lo && lv > 0; lv--) {
        for (i = 0; i < n;) {
            if (levels[order[i]] < lv) { i++; continue; }
            a = i;
            while (i < n && levels[order[i]] >= lv) i++;
            for (b = i - 1; a < b; a++, b--) {
                uint32_t t = order[a];
                order[a] = order[b];
                order[b] = t;
            }
        }
    }
}

uint16_t* fa_text_utf16(const char* text, uint32_t len, uint32_t* n16, uint32_t** byte_of)
{
    uint16_t* u = (uint16_t*)malloc(((size_t)len + 1) * sizeof(uint16_t));
    uint32_t* map = (uint32_t*)malloc(((size_t)len + 1) * sizeof(uint32_t));
    uint32_t i = 0, n = 0;
    if (!u || !map) {
        free(u);
        free(map);
        return NULL;
    }
    while (i < len) {
        uint32_t at = i, c = fa_text_next_cp(text, len, &i);
        if (c >= 0x10000) {
            c -= 0x10000;
            map[n] = at;
            u[n++] = (uint16_t)(0xD800 + (c >> 10));
            map[n] = at;
            u[n++] = (uint16_t)(0xDC00 + (c & 0x3FF));
        } else {
            map[n] = at;
            u[n++] = (uint16_t)c;
        }
    }
    map[n] = len;
    u[n] = 0;
    *n16 = n;
    *byte_of = map;
    return u;
}

int fa_text_push(fa_text_buf* b, uint32_t glyph, uint32_t cluster, float pen, float adv, float dx, float dy, int rtl)
{
    if (b->n == b->cap) {
        uint32_t cap = b->cap ? b->cap * 2 : 64;
        fa_text_glyph* g = (fa_text_glyph*)realloc(b->g, (size_t)cap * sizeof(*g));
        if (!g) return 0;
        b->g = g;
        b->cap = cap;
    }
    b->g[b->n].glyph = glyph;
    b->g[b->n].cluster = cluster;
    b->g[b->n].pen = pen;
    b->g[b->n].adv = adv;
    b->g[b->n].dx = dx;
    b->g[b->n].dy = dy;
    b->g[b->n].rtl = (uint8_t)(rtl != 0);
    b->n++;
    return 1;
}

int fa_text_edges_alloc(fa_text_buf* b)
{
    size_t n = b->len ? b->len : 1;
    b->left = (float*)calloc(n, sizeof(float));
    b->right = (float*)calloc(n, sizeof(float));
    b->rtl = (uint8_t*)calloc(n, 1);
    return b->left && b->right && b->rtl;
}

static void buf_free(fa_text_buf* b)
{
    free(b->g);
    free(b->left);
    free(b->right);
    free(b->rtl);
    memset(b, 0, sizeof(*b));
}

void fa_shaped_free(fa_shaped* s)
{
    free(s->glyph);
    free(s->left);
    free(s->right);
    free(s->rtl);
    memset(s, 0, sizeof(*s));
}

static int out_alloc(fa_shaped* s, uint32_t len, uint32_t glyphs)
{
    size_t n = len ? len : 1;
    s->glyph = (fa_shaped_glyph*)malloc((glyphs ? glyphs : 1) * sizeof(fa_shaped_glyph));
    s->left = (float*)calloc(n, sizeof(float));
    s->right = (float*)calloc(n, sizeof(float));
    s->rtl = (uint8_t*)calloc(n, 1);
    return s->glyph && s->left && s->right && s->rtl;
}

/* Our own layout: a glyph per character from the cmap, left to right, pair kerning.
 * A character runs from its pen position to the next one's (kerning included). */
static float own_layout(fa_font* font, const char* text, uint32_t len, float spacing, fa_shaped* out)
{
    const fa_font_info* in = fa_font_get_info(font);
    float upem = (float)in->units_per_em, pen = 0.0f;
    uint32_t i = 0, prev = 0, last = 0, b;
    int have = 0;
    while (i < len) {
        uint32_t at = i, cp = fa_text_next_cp(text, len, &i), gid = fa_font_glyph(font, cp);
        if (have) pen += (float)fa_font_kerning(font, prev, gid) / upem;
        if (out) {
            if (have)
                for (b = last; b < at; b++) out->right[b] = pen;
            for (b = at; b < i; b++) out->left[b] = pen;
            /* A space draws nothing, also in a font without a space glyph. */
            out->glyph[out->count].glyph = cp == ' ' ? UINT32_MAX : gid;
            out->glyph[out->count].x = pen;
            out->glyph[out->count].y = 0.0f;
            out->count++;
        }
        pen += (float)fa_font_advance(font, gid) / upem + spacing;
        prev = gid;
        last = at;
        have = 1;
    }
    if (out && have)
        for (b = last; b < len; b++) out->right[b] = pen;
    return pen;
}

/* The edges of each character from the shaper's clusters: a cluster spans its
 * glyphs' advances, and its characters share that evenly (right to left in a
 * right-to-left run). */
static int cluster_edges(const fa_text_buf* b, const char* text, uint32_t len, float k, fa_shaped* s)
{
    uint32_t i, c, j;
    float* lo = s->left;
    float* hi = s->right;
    uint8_t* seen = (uint8_t*)calloc(len ? len : 1, 1);
    if (!seen) return 0;
    /* Each cluster's extent, kept at the cluster's first byte. */
    for (i = 0; i < b->n; i++) {
        const fa_text_glyph* g = &b->g[i];
        float a = g->pen * k, e = (g->pen + g->adv) * k;
        c = g->cluster < len ? g->cluster : 0;
        if (e < a) { float t = a; a = e; e = t; }
        if (!seen[c]) {
            lo[c] = a;
            hi[c] = e;
            s->rtl[c] = g->rtl;
            seen[c] = 1;
        } else {
            if (a < lo[c]) lo[c] = a;
            if (e > hi[c]) hi[c] = e;
        }
    }
    /* Spread each cluster over its characters. */
    for (c = 0; c < len;) {
        uint32_t end = c + 1, chars = 0, at, n;
        float a, e, w;
        int r;
        if (!seen[c]) {
            /* No glyph names this byte as a cluster start (a shaper that left it out):
             * an empty edge where the previous character ends. */
            float x = c ? (s->rtl[c - 1] ? lo[c - 1] : hi[c - 1]) : 0.0f;
            lo[c] = hi[c] = x;
            s->rtl[c] = c ? s->rtl[c - 1] : 0;
            c++;
            continue;
        }
        while (end < len && !seen[end]) end++;
        for (at = c; at < end;) {
            fa_text_next_cp(text, end, &at);
            chars++;
        }
        a = lo[c];
        e = hi[c];
        r = s->rtl[c];
        w = (e - a) / (float)(chars ? chars : 1);
        for (at = c, n = 0; at < end; n++) {
            uint32_t from = at;
            float x0 = r ? e - w * (float)(n + 1) : a + w * (float)n;
            fa_text_next_cp(text, end, &at);
            for (j = from; j < at; j++) {
                lo[j] = x0;
                hi[j] = x0 + w;
                s->rtl[j] = (uint8_t)r;
            }
        }
        c = end;
    }
    free(seen);
    return 1;
}

/* The line's width from the shaper's glyphs (and its edges, when it gave them), em. */
static float buf_width(const fa_text_buf* b, float k)
{
    float w = 0.0f;
    uint32_t i;
    for (i = 0; i < b->n; i++)
        if ((b->g[i].pen + b->g[i].adv) * k > w) w = (b->g[i].pen + b->g[i].adv) * k;
    if (b->edges)
        for (i = 0; i < b->len; i++)
            if (b->right[i] * k > w) w = b->right[i] * k;
    return w;
}

fa_result fa_text_shape(fa_font* font, const char* text, uint32_t len, float spacing, fa_shaped* out)
{
    memset(out, 0, sizeof(*out));
    if (len && !fa_load32(&g_disabled) && fa_text_needs_shaping(text, len)) {
        fa_text_buf b;
        fa_result r;
        memset(&b, 0, sizeof(b));
        b.len = len;
        r = fa_text_os(font, fa_font_shaper_slot(font), text, len, &b);
        if (r == FA_OK) {
            float k = 1.0f / (float)fa_font_get_info(font)->units_per_em;
            uint32_t i;
            if (!out_alloc(out, len, b.n)) {
                buf_free(&b);
                fa_shaped_free(out);
                return FA_ERROR_OUT_OF_MEMORY;
            }
            for (i = 0; i < b.n; i++) {
                const fa_text_glyph* g = &b.g[i];
                /* A space draws nothing, also in a font without a space glyph. */
                out->glyph[i].glyph = !g->glyph && !b.edges && g->cluster < len && text[g->cluster] == ' ' ? UINT32_MAX : g->glyph;
                out->glyph[i].x = (g->pen + g->dx) * k;
                out->glyph[i].y = g->dy * k;
            }
            out->count = b.n;
            if (b.edges) {
                for (i = 0; i < len; i++) {
                    out->left[i] = b.left[i] * k;
                    out->right[i] = b.right[i] * k;
                    out->rtl[i] = b.rtl[i];
                }
            } else if (!cluster_edges(&b, text, len, k, out)) {
                buf_free(&b);
                fa_shaped_free(out);
                return FA_ERROR_OUT_OF_MEMORY;
            }
            out->width = buf_width(&b, k);
            out->shaped = 1;
            buf_free(&b);
            return FA_OK;
        }
        buf_free(&b);
        if (r == FA_ERROR_OUT_OF_MEMORY) return r;
    }
    if (!out_alloc(out, len, len)) {
        fa_shaped_free(out);
        return FA_ERROR_OUT_OF_MEMORY;
    }
    out->width = own_layout(font, text, len, spacing, out);
    return FA_OK;
}

float fa_text_width(fa_font* font, const char* text, uint32_t len, float spacing)
{
    /* As fa_text_shape, without the glyphs and edges it would lay out. */
    if (len && !fa_load32(&g_disabled) && fa_text_needs_shaping(text, len)) {
        fa_text_buf b;
        float w = 0.0f;
        int ok;
        memset(&b, 0, sizeof(b));
        b.len = len;
        ok = fa_text_os(font, fa_font_shaper_slot(font), text, len, &b) == FA_OK;
        if (ok) w = buf_width(&b, 1.0f / (float)fa_font_get_info(font)->units_per_em);
        buf_free(&b);
        if (ok) return w;
    }
    return own_layout(font, text, len, spacing, NULL);
}
