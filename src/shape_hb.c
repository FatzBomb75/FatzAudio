/* The OS shaper on Linux (shape.h): the system's FriBidi for the direction of each
 * run (Unicode bidi) and HarfBuzz for the glyphs, both loaded at runtime
 * (libfribidi.so.0 and libharfbuzz.so.0, which every desktop with GTK has: Pango
 * needs both). Nothing is linked: a system without either gets our own layout. */
#include "shape.h"
#include "fa_platform.h"

#include <stdlib.h>
#include <string.h>

typedef struct hb_blob_t hb_blob_t;
typedef struct hb_face_t hb_face_t;
typedef struct hb_font_t hb_font_t;
typedef struct hb_buffer_t hb_buffer_t;
typedef struct hb_unicode_funcs_t hb_unicode_funcs_t;
typedef struct hb_glyph_info_t { uint32_t codepoint, mask, cluster, var1, var2; } hb_glyph_info_t;
typedef struct hb_glyph_position_t { int32_t x_advance, y_advance, x_offset, y_offset; uint32_t var; } hb_glyph_position_t;
enum { HB_MEMORY_MODE_READONLY = 1, HB_DIRECTION_LTR = 4, HB_DIRECTION_RTL = 5 };
#define HB_TAG(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define HB_SCRIPT_COMMON HB_TAG('Z', 'y', 'y', 'y')
#define HB_SCRIPT_INHERITED HB_TAG('Z', 'i', 'n', 'h')
#define HB_SCRIPT_UNKNOWN HB_TAG('Z', 'z', 'z', 'z')
#define FRIBIDI_PAR_ON 0x40u

static struct {
    int tried, ok;
    hb_blob_t* (*blob_create)(const char*, unsigned, int, void*, void (*)(void*));
    void (*blob_destroy)(hb_blob_t*);
    hb_face_t* (*face_create)(hb_blob_t*, unsigned);
    void (*face_destroy)(hb_face_t*);
    hb_font_t* (*font_create)(hb_face_t*);
    void (*font_destroy)(hb_font_t*);
    void (*font_set_scale)(hb_font_t*, int, int);
    void (*ot_font_set_funcs)(hb_font_t*);
    hb_buffer_t* (*buffer_create)(void);
    void (*buffer_destroy)(hb_buffer_t*);
    void (*buffer_clear)(hb_buffer_t*);
    void (*buffer_add_utf8)(hb_buffer_t*, const char*, int, unsigned, int);
    void (*buffer_set_direction)(hb_buffer_t*, int);
    void (*buffer_set_script)(hb_buffer_t*, uint32_t);
    void (*buffer_guess)(hb_buffer_t*);
    int (*buffer_ok)(hb_buffer_t*);
    void (*shape)(hb_font_t*, hb_buffer_t*, const void*, unsigned);
    hb_glyph_info_t* (*infos)(hb_buffer_t*, unsigned*);
    hb_glyph_position_t* (*positions)(hb_buffer_t*, unsigned*);
    hb_unicode_funcs_t* (*unicode_default)(void);
    uint32_t (*unicode_script)(hb_unicode_funcs_t*, uint32_t);
    /* FriBidi */
    void (*bidi_types)(const uint32_t*, int, uint32_t*);
    void (*bracket_types)(const uint32_t*, int, const uint32_t*, uint32_t*);
    signed char (*levels)(const uint32_t*, const uint32_t*, int, uint32_t*, signed char*);
} g_hb;
static fa_lazy_mutex g_hb_lock;

#define SYM(field, name) (*(void**)&g_hb.field = fa_lib_sym(lib, name))

static int hb_load(void)
{
    fa_lazy_mutex_lock(&g_hb_lock);
    if (!g_hb.tried) {
        void* lib = fa_lib_open("libharfbuzz.so.0", 0);
        g_hb.tried = 1;
        if (lib) {
            g_hb.ok = SYM(blob_create, "hb_blob_create") && SYM(blob_destroy, "hb_blob_destroy") &&
                      SYM(face_create, "hb_face_create") && SYM(face_destroy, "hb_face_destroy") &&
                      SYM(font_create, "hb_font_create") && SYM(font_destroy, "hb_font_destroy") &&
                      SYM(font_set_scale, "hb_font_set_scale") && SYM(buffer_create, "hb_buffer_create") &&
                      SYM(buffer_destroy, "hb_buffer_destroy") && SYM(buffer_clear, "hb_buffer_clear_contents") &&
                      SYM(buffer_add_utf8, "hb_buffer_add_utf8") && SYM(buffer_set_direction, "hb_buffer_set_direction") &&
                      SYM(buffer_set_script, "hb_buffer_set_script") && SYM(buffer_guess, "hb_buffer_guess_segment_properties") &&
                      SYM(buffer_ok, "hb_buffer_allocation_successful") && SYM(shape, "hb_shape") &&
                      SYM(infos, "hb_buffer_get_glyph_infos") && SYM(positions, "hb_buffer_get_glyph_positions") &&
                      SYM(unicode_default, "hb_unicode_funcs_get_default") && SYM(unicode_script, "hb_unicode_script");
            /* HarfBuzz before 2.0 needs its OpenType functions set on each font. */
            SYM(ot_font_set_funcs, "hb_ot_font_set_funcs");
            if (!g_hb.ok) fa_lib_close(lib, 0);
        }
        if (g_hb.ok) {
            /* HarfBuzz stays loaded (its functions are kept) even when FriBidi is missing. */
            lib = fa_lib_open("libfribidi.so.0", 0);
            g_hb.ok = lib && SYM(bidi_types, "fribidi_get_bidi_types") && SYM(levels, "fribidi_get_par_embedding_levels_ex");
            if (g_hb.ok) SYM(bracket_types, "fribidi_get_bracket_types");   /* 1.0 and later */
            else if (lib) fa_lib_close(lib, 0);
        }
    }
    fa_lazy_mutex_unlock(&g_hb_lock);
    return g_hb.ok;
}

const char* fa_text_os_name(void) { return hb_load() ? "HarfBuzz" : NULL; }

typedef struct hb_state {
    hb_blob_t* blob;
    hb_face_t* face;
    hb_font_t* font;
    hb_buffer_t* buf;
} hb_state;

void fa_text_os_release(void* state)
{
    hb_state* st = (hb_state*)state;
    if (!st) return;
    if (st->buf) g_hb.buffer_destroy(st->buf);
    if (st->font) g_hb.font_destroy(st->font);
    if (st->face) g_hb.face_destroy(st->face);
    if (st->blob) g_hb.blob_destroy(st->blob);
    free(st);
}

static hb_state* state_for(fa_font* font, void** slot)
{
    const uint8_t* data;
    size_t size;
    uint32_t index;
    int upem = fa_font_get_info(font)->units_per_em;
    hb_state* st = (hb_state*)*slot;
    if (st == FA_TEXT_STATE_FAILED) return NULL;
    if (st) return st;
    fa_font_source(font, &data, &size, &index);
    if (size > 0x7FFFFFFF) {
        *slot = FA_TEXT_STATE_FAILED;
        return NULL;
    }
    st = (hb_state*)calloc(1, sizeof(*st));
    if (!st) return NULL;
    /* The font's bytes outlive the state: it is freed when the font closes. */
    st->blob = g_hb.blob_create((const char*)data, (unsigned)size, HB_MEMORY_MODE_READONLY, NULL, NULL);
    st->face = st->blob ? g_hb.face_create(st->blob, index) : NULL;
    st->font = st->face ? g_hb.font_create(st->face) : NULL;
    st->buf = g_hb.buffer_create();
    if (!st->font || !st->buf || !g_hb.buffer_ok(st->buf)) {
        fa_text_os_release(st);
        *slot = FA_TEXT_STATE_FAILED;
        return NULL;
    }
    if (g_hb.ot_font_set_funcs) g_hb.ot_font_set_funcs(st->font);
    g_hb.font_set_scale(st->font, upem, upem);
    *slot = st;
    return st;
}

static int weak_script(uint32_t s) { return s == HB_SCRIPT_COMMON || s == HB_SCRIPT_INHERITED || s == HB_SCRIPT_UNKNOWN; }

fa_result fa_text_os(fa_font* font, void** slot, const char* text, uint32_t len, fa_text_buf* out)
{
    hb_state* st;
    uint32_t *cp = NULL, *at = NULL, *types = NULL, *brackets = NULL, *script = NULL, *rs = NULL, *order = NULL;
    signed char* lv = NULL;
    uint8_t* rl = NULL;
    uint32_t n = 0, i, nr = 0, r;
    fa_result res = FA_ERROR_OUT_OF_MEMORY;
    float pen = 0.0f;
    if (!hb_load() || len > 0x7FFFFFFF) return FA_ERROR_NOT_FOUND;
    st = state_for(font, slot);
    if (!st) return FA_ERROR_NOT_FOUND;
    cp = (uint32_t*)malloc(((size_t)len + 1) * sizeof(uint32_t));
    at = (uint32_t*)malloc(((size_t)len + 1) * sizeof(uint32_t));
    script = (uint32_t*)malloc(((size_t)len + 1) * sizeof(uint32_t));
    lv = (signed char*)malloc((size_t)len + 1);
    if (!cp || !at || !script || !lv) goto done;
    for (i = 0; i < len;) {
        at[n] = i;
        cp[n++] = fa_text_next_cp(text, len, &i);
    }
    at[n] = len;
    /* Each character's bidi level, the paragraph's direction from its first strong
     * character. */
    {
        uint32_t base = FRIBIDI_PAR_ON;
        types = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
        if (g_hb.bracket_types) brackets = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
        if (!types || (g_hb.bracket_types && !brackets)) goto done;
        g_hb.bidi_types(cp, (int)n, types);
        if (brackets) g_hb.bracket_types(cp, (int)n, types, brackets);
        if (!g_hb.levels(types, brackets, (int)n, &base, lv)) {
            res = FA_ERROR_NOT_FOUND;
            goto done;
        }
    }
    /* Each character's script: marks and the common characters take their run's. */
    {
        hb_unicode_funcs_t* uf = g_hb.unicode_default();
        uint32_t cur = HB_SCRIPT_UNKNOWN;
        for (i = 0; i < n; i++) {
            uint32_t s = g_hb.unicode_script(uf, cp[i]);
            script[i] = weak_script(s) ? cur : (cur = s);
        }
        /* Leading common characters take the first script. */
        for (i = 0; i < n && weak_script(script[i]); i++) {}
        if (i < n) {
            uint32_t first = script[i];
            for (i = 0; i < n && weak_script(script[i]); i++) script[i] = first;
        }
    }
    /* Runs of one level and one script. */
    rs = (uint32_t*)malloc(((size_t)n + 1) * sizeof(uint32_t));
    rl = (uint8_t*)malloc((size_t)n + 1);
    order = (uint32_t*)malloc(((size_t)n + 1) * sizeof(uint32_t));
    if (!rs || !rl || !order) goto done;
    for (i = 0; i < n; i++)
        if (!i || lv[i] != lv[i - 1] || script[i] != script[i - 1]) {
            rs[nr] = i;
            rl[nr++] = (uint8_t)(lv[i] < 0 ? 0 : lv[i]);
        }
    rs[nr] = n;
    fa_text_visual_order(rl, nr, order);
    for (r = 0; r < nr; r++) {
        uint32_t k = order[r], a = rs[k], b = rs[k + 1], gn = 0, pn = 0, j;
        int rtl = rl[k] & 1;
        hb_glyph_info_t* gi;
        hb_glyph_position_t* gp;
        g_hb.buffer_clear(st->buf);
        g_hb.buffer_add_utf8(st->buf, text, (int)len, at[a], (int)(at[b] - at[a]));
        g_hb.buffer_set_direction(st->buf, rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
        if (!weak_script(script[a])) g_hb.buffer_set_script(st->buf, script[a]);
        g_hb.buffer_guess(st->buf);
        g_hb.shape(st->font, st->buf, NULL, 0);
        if (!g_hb.buffer_ok(st->buf)) goto done;
        gi = g_hb.infos(st->buf, &gn);
        gp = g_hb.positions(st->buf, &pn);
        if (pn < gn) gn = pn;
        for (j = 0; j < gn; j++) {
            if (!fa_text_push(out, gi[j].codepoint, gi[j].cluster, pen, (float)gp[j].x_advance, (float)gp[j].x_offset,
                               (float)gp[j].y_offset, rtl))
                goto done;
            pen += (float)gp[j].x_advance;
        }
    }
    res = FA_OK;
done:
    free(cp);
    free(at);
    free(types);
    free(brackets);
    free(script);
    free(lv);
    free(rs);
    free(rl);
    free(order);
    return res;
}
