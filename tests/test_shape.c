/* Text shaping (src/shape.c) on tests/data/fonts/shape.ttf (make_fonts.py): our own
 * layout everywhere, and the OS shaper where the system has one (DirectWrite,
 * Core Text, HarfBuzz; Android's needs an app's Java VM, so a command-line test
 * there checks only our own layout). */
#include "fa_platform.h"
#include "font.h"
#include "font_find.h"
#include "shape.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Sanitizer builds: fontconfig keeps its configuration for the life of the process. */
#if defined(__SANITIZE_ADDRESS__)
#  define FA_TEST_SANITIZED 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define FA_TEST_SANITIZED 1
#  endif
#endif
#if defined(FA_TEST_SANITIZED)
const char* __lsan_default_suppressions(void);
__attribute__((visibility("default"))) const char* __lsan_default_suppressions(void) { return "leak:libfontconfig.so\nleak:libexpat.so\n"; }
#endif

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; return; } } while (0)
#define CHECK_EQ(a, b) do { long long va_ = (long long)(a), vb_ = (long long)(b); if (va_ != vb_) { \
    fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_failures++; return; } } while (0)
#define CHECK_NEAR(a, b, tol) do { double va_ = (a), vb_ = (b); if (fabs(va_ - vb_) > (tol)) { \
    fprintf(stderr, "  FAIL %s:%d: %s ~ %s (%.6f vs %.6f)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_failures++; return; } } while (0)
#define RUN(t) do { int before_ = g_failures; printf("%s\n", #t); t(); if (g_failures == before_) printf("  ok\n"); } while (0)

/* shape.ttf's glyphs (make_fonts.py's SHAPE_ADV order). */
enum { G_SPACE = 1, G_A, G_ACUTE, G_BEH, G_BEH_INIT, G_BEH_MEDI, G_BEH_FINA, G_LAM, G_LAM_INIT, G_LAM_MEDI, G_LAM_FINA,
       G_ALEF, G_ALEF_FINA, G_LAM_ALEF, G_LAM_ALEF_FINA, G_HE_ALEF, G_HE_BET };

#define BEH "\xD8\xA8"
#define LAM "\xD9\x84"
#define ALEF "\xD8\xA7"
#define HE_ALEF "\xD7\x90"
#define HE_BET "\xD7\x91"
#define ACUTE "\xCC\x81"

static fa_font* g_font;
static int g_os;   /* the system has a shaper this test can use */

static void open_font(void)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/fonts/shape.ttf", FA_TEST_DATA_DIR);
    if (fa_font_open_file(path, 0, &g_font) != FA_OK) g_font = NULL;
}

/* The glyphs from the left, with their x (em), as shaped. */
static int shape(const char* s, fa_shaped* out)
{
    return fa_text_shape(g_font, s, (uint32_t)strlen(s), 0.0f, out) == FA_OK;
}

/* Every character's edges inside the line, left before right. */
static int edges_sane(const fa_shaped* sh, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < len; i++)
        if (!(sh->left[i] <= sh->right[i] + 1e-4f) || sh->left[i] < -1e-4f || sh->right[i] > sh->width + 1e-4f) return 0;
    return 1;
}

/* Index of the glyph nearest the left with id g, or -1. */
static int find_glyph(const fa_shaped* sh, uint32_t g)
{
    uint32_t i;
    for (i = 0; i < sh->count; i++)
        if (sh->glyph[i].glyph == g) return (int)i;
    return -1;
}

static void test_needed(void)
{
    CHECK(!fa_text_needs_shaping("Hello, world", 12));
    CHECK(!fa_text_needs_shaping("\xE3\x81\x82\xE4\xB8\xAD", 6));                 /* kana, han */
    CHECK(!fa_text_needs_shaping("\xD0\x9F\xD1\x80\xD0\xB8", 6));                 /* Cyrillic */
    CHECK(fa_text_needs_shaping(BEH, 2));
    CHECK(fa_text_needs_shaping(HE_ALEF, 2));
    CHECK(fa_text_needs_shaping("A" ACUTE, 3));
    CHECK(fa_text_needs_shaping("\xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\xB7", 9));     /* Devanagari */
    CHECK(fa_text_rtl_paragraph("12 " BEH " A", 8));
    CHECK(!fa_text_rtl_paragraph("A " BEH, 4));
    CHECK(!fa_text_rtl_paragraph("123", 3));
    CHECK(!fa_text_rtl_paragraph("\xD9\xA1\xD9\xA2 hello", 10));                 /* Arabic-Indic digits are weak */
}

static void test_visual_order(void)
{
    /* L R R L at levels 0 1 1 0, and digits (2) inside a right-to-left run (1). */
    uint8_t lv[4] = { 0, 1, 1, 0 }, lv2[3] = { 1, 2, 1 };
    uint32_t o[4];
    fa_text_visual_order(lv, 4, o);
    CHECK(o[0] == 0 && o[1] == 2 && o[2] == 1 && o[3] == 3);
    fa_text_visual_order(lv2, 3, o);
    CHECK(o[0] == 2 && o[1] == 1 && o[2] == 0);
}

/* Our own layout: a glyph per character, left to right, as before shaping. */
static void test_own_layout(void)
{
    fa_shaped sh;
    CHECK(g_font);
    fa_text_set_disabled(1);
    CHECK(shape("AA", &sh));
    fa_text_set_disabled(0);
    CHECK_EQ(sh.shaped, 0);
    CHECK_EQ(sh.count, 2);
    CHECK_NEAR(sh.width, 1.2, 1e-6);
    CHECK_NEAR(sh.glyph[1].x, 0.6, 1e-6);
    CHECK_NEAR(sh.left[1], 0.6, 1e-6);
    CHECK_NEAR(sh.right[0], 0.6, 1e-6);
    fa_shaped_free(&sh);
    /* Arabic without a shaper: unjoined, in logical order (the documented fallback). */
    fa_text_set_disabled(1);
    CHECK(shape(BEH BEH, &sh));
    fa_text_set_disabled(0);
    CHECK_EQ(sh.count, 2);
    CHECK_EQ(sh.glyph[0].glyph, G_BEH);
    CHECK_EQ(sh.rtl[0], 0);
    CHECK_NEAR(sh.width, 1.0, 1e-6);
    fa_shaped_free(&sh);
    /* Latin never goes to the OS shaper: the same as before, with spacing. */
    CHECK(fa_text_shape(g_font, "AA", 2, 0.05f, &sh) == FA_OK);
    CHECK_EQ(sh.shaped, 0);
    CHECK_NEAR(sh.width, 1.3, 1e-6);
    fa_shaped_free(&sh);
}

static void test_arabic_joining(void)
{
    fa_shaped sh;
    if (!g_os) { printf("  (no shaper here)\n"); return; }
    CHECK(shape(BEH BEH, &sh));
    CHECK_EQ(sh.shaped, 1);
    CHECK_EQ(sh.count, 2);
    /* Right to left: the final form on the left, the initial one on the right. */
    CHECK_EQ(sh.glyph[sh.glyph[0].x < sh.glyph[1].x ? 0 : 1].glyph, G_BEH_FINA);
    CHECK_EQ(sh.glyph[sh.glyph[0].x < sh.glyph[1].x ? 1 : 0].glyph, G_BEH_INIT);
    CHECK_NEAR(sh.width, 0.84, 1e-3);
    CHECK_EQ(sh.rtl[0], 1);
    CHECK_NEAR(sh.left[0], 0.43, 1e-3);       /* the first character, on the right */
    CHECK_NEAR(sh.right[0], 0.84, 1e-3);
    CHECK_NEAR(sh.left[2], 0.0, 1e-3);
    CHECK_NEAR(sh.right[2], 0.43, 1e-3);
    CHECK(edges_sane(&sh, 4));
    fa_shaped_free(&sh);
    /* Three: initial, medial, final. */
    CHECK(shape(BEH BEH BEH, &sh));
    CHECK_EQ(sh.count, 3);
    CHECK(find_glyph(&sh, G_BEH_MEDI) >= 0);
    CHECK_NEAR(sh.width, 1.26, 1e-3);
    fa_shaped_free(&sh);
}

static void test_ligature(void)
{
    fa_shaped sh;
    if (!g_os) { printf("  (no shaper here)\n"); return; }
    CHECK(shape(LAM ALEF, &sh));
    CHECK_EQ(sh.count, 1);
    CHECK_EQ(sh.glyph[0].glyph, G_LAM_ALEF);
    CHECK_NEAR(sh.width, 0.64, 1e-3);
    /* The two characters share it: lam (first) on the right. */
    CHECK_NEAR(sh.left[0], 0.32, 2e-3);
    CHECK_NEAR(sh.right[0], 0.64, 2e-3);
    CHECK_NEAR(sh.left[2], 0.0, 2e-3);
    CHECK_NEAR(sh.right[2], 0.32, 2e-3);
    fa_shaped_free(&sh);
}

static void test_hebrew(void)
{
    fa_shaped sh;
    if (!g_os) { printf("  (no shaper here)\n"); return; }
    CHECK(shape(HE_ALEF HE_BET, &sh));
    CHECK_EQ(sh.count, 2);
    CHECK_EQ(sh.glyph[sh.glyph[0].x < sh.glyph[1].x ? 0 : 1].glyph, G_HE_BET);   /* bet on the left */
    CHECK_NEAR(sh.left[0], 0.54, 1e-3);
    CHECK_NEAR(sh.right[0], 1.06, 1e-3);
    CHECK_EQ(sh.rtl[2], 1);
    fa_shaped_free(&sh);
}

/* Bidi: a right-to-left word in a left-to-right line, and the other way round. */
static void test_mixed(void)
{
    fa_shaped sh;
    const char* s = "A " BEH BEH " A";
    if (!g_os) { printf("  (no shaper here)\n"); return; }
    CHECK(shape(s, &sh));
    CHECK_NEAR(sh.width, 0.6 + 0.25 + 0.84 + 0.25 + 0.6, 2e-3);
    CHECK_NEAR(sh.left[0], 0.0, 1e-3);                     /* the first A at the left */
    CHECK_NEAR(sh.left[7], sh.width - 0.6, 2e-3);          /* the last at the right */
    CHECK_EQ(sh.rtl[2], 1);
    CHECK_EQ(sh.rtl[0], 0);
    CHECK(sh.left[2] > sh.left[4]);                        /* the first beh right of the second */
    CHECK(edges_sane(&sh, (uint32_t)strlen(s)));
    fa_shaped_free(&sh);
    s = BEH BEH " A";
    CHECK(shape(s, &sh));
    CHECK_NEAR(sh.left[5], 0.0, 1e-3);                     /* an RTL line: the A at the left end */
    CHECK_NEAR(sh.right[0], sh.width, 2e-3);               /* the first beh at the right end */
    CHECK(edges_sane(&sh, (uint32_t)strlen(s)));
    fa_shaped_free(&sh);
}

static void test_mark(void)
{
    fa_shaped sh;
    int m;
    if (!g_os) { printf("  (no shaper here)\n"); return; }
    CHECK(shape("A" ACUTE, &sh));
    CHECK_EQ(sh.count, 2);
    m = find_glyph(&sh, G_ACUTE);
    CHECK(m >= 0);
    /* Placed by GPOS: its anchor (-150, 700) on A's (300, 700). */
    CHECK_NEAR(sh.glyph[m].x, 0.45, 2e-3);
    CHECK_NEAR(sh.glyph[m].y, 0.0, 2e-3);
    CHECK_NEAR(sh.width, 0.6, 1e-3);
    fa_shaped_free(&sh);
}

/* A font for text the app's font lacks: whatever the system has (not checked
 * where it has nothing). */
static void test_font_for_text(void)
{
    fa_font* f = NULL;
    fa_result r = fa_font_find_for_text(BEH LAM ALEF, 6, 400, 0, &f);
    if (r != FA_OK) { printf("  (the system named no font)\n"); return; }
    printf("  %s\n", fa_font_get_info(f)->family);
    CHECK(fa_font_glyph(f, 0x628) != 0);
    fa_font_close(f);
    CHECK(fa_font_find_for_text("", 0, 400, 0, &f) == FA_ERROR_NOT_FOUND);
    CHECK(fa_font_find_for_text("abc", 3, 400, 0, &f) == FA_ERROR_NOT_FOUND);
}

int main(void)
{
    const char* name;
    open_font();
    name = fa_shaper_name();
    g_os = g_font && name[0];
#if defined(__linux__) && !defined(__ANDROID__)
    /* HarfBuzz is loaded at runtime: the CI images have it, a bare system may not. */
#elif defined(_WIN32) || defined(__APPLE__)
    if (!g_os) {
        fprintf(stderr, "  FAIL: no shaper (%s)\n", name);
        g_failures++;
    }
#endif
    printf("shaper: %s\n", name[0] ? name : "(none)");
    RUN(test_needed);
    RUN(test_visual_order);
    RUN(test_own_layout);
    RUN(test_arabic_joining);
    RUN(test_ligature);
    RUN(test_hebrew);
    RUN(test_mixed);
    RUN(test_mark);
    RUN(test_font_for_text);
    if (g_font) fa_font_close(g_font);
    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
