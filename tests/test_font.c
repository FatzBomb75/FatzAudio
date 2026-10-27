/* The font reader and rasterizer (src/font.c, src/raster.c) on the fonts of
 * tests/data/fonts (make_fonts.py says what each glyph is), plus random damage.
 * With FA_TEST_FONT_DIR set, every font in that folder is also opened and drawn
 * in full (a local check on real fonts; nothing is compared). */
#include "font.h"
#include "font_find.h"
#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#  include <dirent.h>
#endif

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; return; } } while (0)
#define CHECK_EQ(a, b) do { long long va_ = (long long)(a), vb_ = (long long)(b); if (va_ != vb_) { \
    fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_failures++; return; } } while (0)
#define CHECK_NEAR(a, b, tol) do { double va_ = (a), vb_ = (b); if (fabs(va_ - vb_) > (tol)) { \
    fprintf(stderr, "  FAIL %s:%d: %s ~ %s (%.6f vs %.6f)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_failures++; return; } } while (0)
#define RUN(t) do { int before_ = g_failures; printf("%s\n", #t); t(); if (g_failures == before_) printf("  ok\n"); } while (0)

/* Sanitizer builds damage fewer copies (CI time). */
#if defined(__SANITIZE_ADDRESS__)
#  define FA_TEST_SANITIZED 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define FA_TEST_SANITIZED 1
#  endif
#endif
#ifndef FA_TEST_SANITIZED
#  define FA_TEST_SANITIZED 0
#endif
#if FA_TEST_SANITIZED
/* fontconfig keeps its configuration for the life of the process. */
const char* __lsan_default_suppressions(void);
__attribute__((visibility("default"))) const char* __lsan_default_suppressions(void) { return "leak:libfontconfig.so\nleak:libexpat.so\n"; }
#endif

typedef struct blob {
    uint8_t* p;
    size_t n;
} blob;

static blob load(const char* name)
{
    char path[1024];
    blob b = { NULL, 0 };
    FILE* f;
    long len;
    snprintf(path, sizeof(path), "%s/fonts/%s", FA_TEST_DATA_DIR, name);
    f = fopen(path, "rb");
    if (!f) return b;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    b.p = (uint8_t*)malloc((size_t)len);
    if (b.p && fread(b.p, 1, (size_t)len, f) == (size_t)len) b.n = (size_t)len;
    fclose(f);
    return b;
}

static fa_font* open_font(const char* name, uint32_t index)
{
    blob b = load(name);
    fa_font* f = NULL;
    if (!b.n) return NULL;
    if (fa_font_open(b.p, b.n, index, 1, &f) != FA_OK) return NULL;
    return f;
}

/* Draws glyph `g` at 1 pixel = 10 font units, baseline at y = 80, moved by (dx, 0):
 * a 80 x 100 coverage image. */
static uint8_t* draw(const fa_font* f, uint32_t g, float dx)
{
    fa_path p = { 0 };
    fa_raster r = { 0 };
    uint8_t* img = (uint8_t*)calloc(80 * 100, 1);
    if (!img) return NULL;
    if (fa_font_outline(f, g, &p) != FA_OK || !fa_raster_begin(&r, 80, 100)) { free(img); fa_path_free(&p); return NULL; }
    fa_raster_path(&r, &p, 0.1f, -0.1f, dx, 80.0f);
    fa_raster_end(&r, img, 80);
    fa_raster_free(&r);
    fa_path_free(&p);
    return img;
}

static double coverage_sum(const uint8_t* img)
{
    double s = 0;
    int i;
    for (i = 0; i < 80 * 100; i++) s += img[i];
    return s / 255.0;
}

/* Pixel (x, y) of a drawn image; font units x / 10, (800 - y) / 10. */
static int px(const uint8_t* img, int x, int y) { return img[y * 80 + x]; }

static void check_square_a(const uint8_t* a)
{
    /* x 100..600, y 0..700 units: pixels 10..59, rows 10..79, on pixel edges. */
    CHECK_EQ(px(a, 10, 10), 255);
    CHECK_EQ(px(a, 59, 79), 255);
    CHECK_EQ(px(a, 9, 40), 0);
    CHECK_EQ(px(a, 60, 40), 0);
    CHECK_EQ(px(a, 30, 9), 0);
    CHECK_EQ(px(a, 30, 80), 0);
    CHECK_NEAR(coverage_sum(a), 50.0 * 70.0, 0.5);
}

static void test_info_and_maps(void)
{
    static const char* names[] = { "tt.ttf", "tt_gpos.ttf", "cff.otf", "cid.otf" };
    unsigned i;
    for (i = 0; i < 4; i++) {
        fa_font* f = open_font(names[i], 0);
        const fa_font_info* in;
        CHECK(f);
        in = fa_font_get_info(f);
        CHECK(strcmp(in->family, "FA Test") == 0);
        CHECK(strcmp(in->style, "Regular") == 0);
        CHECK_EQ(in->units_per_em, 1000);
        CHECK_EQ(in->ascender, 800);
        CHECK_EQ(in->descender, -200);
        CHECK_EQ(in->cap_height, 700);
        CHECK_EQ(in->x_height, 500);
        CHECK_EQ(in->weight, 400);
        CHECK_EQ(in->cff, i >= 2);
        CHECK_EQ(in->glyph_count, 7);
        CHECK_EQ(fa_font_glyph(f, 'A'), 2);
        CHECK_EQ(fa_font_glyph(f, 'V'), 5);
        CHECK_EQ(fa_font_glyph(f, ' '), 1);
        CHECK_EQ(fa_font_glyph(f, 0x1F600), 6);   /* cmap format 12 */
        CHECK_EQ(fa_font_glyph(f, 'Z'), 0);
        CHECK_EQ(fa_font_glyph(f, 0x10FFFF), 0);
        CHECK_EQ(fa_font_advance(f, 2), 700);
        CHECK_EQ(fa_font_advance(f, 5), 600);
        CHECK_EQ(fa_font_advance(f, 1), 250);
        CHECK_EQ(fa_font_advance(f, 99), 0);
        fa_font_close(f);
    }
}

static void test_kerning(void)
{
    fa_font* k = open_font("tt.ttf", 0);
    fa_font* g = open_font("tt_gpos.ttf", 0);
    fa_font* c = open_font("cff.otf", 0);
    CHECK(k && g && c);
    /* kern table */
    CHECK_EQ(fa_font_kerning(k, 2, 5), -80);
    CHECK_EQ(fa_font_kerning(k, 5, 2), -40);
    CHECK_EQ(fa_font_kerning(k, 2, 2), 0);
    /* GPOS: a pair in an extension lookup, then classes */
    CHECK_EQ(fa_font_kerning(g, 2, 5), -80);
    CHECK_EQ(fa_font_kerning(g, 4, 5), -30);   /* O V */
    CHECK_EQ(fa_font_kerning(g, 3, 5), -30);   /* B V */
    CHECK_EQ(fa_font_kerning(g, 5, 2), 0);
    CHECK_EQ(fa_font_kerning(g, 4, 2), 0);
    CHECK_EQ(fa_font_kerning(c, 2, 5), 0);     /* no kerning at all */
    fa_font_close(k);
    fa_font_close(g);
    fa_font_close(c);
}

static void test_outlines(void)
{
    fa_font* t = open_font("tt.ttf", 0);
    fa_font* c = open_font("cff.otf", 0);
    fa_font* cid = open_font("cid.otf", 0);
    uint8_t *a, *b, *o, *s;
    fa_path p = { 0 };
    CHECK(t && c && cid);

    /* The square, TrueType and CFF (from a local subroutine, after a hintmask), and CID. */
    a = draw(t, 2, 0);
    CHECK(a);
    check_square_a(a);
    b = draw(c, 2, 0);
    CHECK(b);
    CHECK(memcmp(a, b, 80 * 100) == 0);
    free(b);
    b = draw(cid, 2, 0);
    CHECK(b);
    CHECK(memcmp(a, b, 80 * 100) == 0);
    free(b);
    /* Half a pixel right: the edge columns are half covered, the area is the same. */
    b = draw(t, 2, 0.5f);
    CHECK(b);
    CHECK_EQ(px(b, 10, 40), 128);
    CHECK_EQ(px(b, 60, 40), 128);
    CHECK_EQ(px(b, 30, 40), 255);
    CHECK_NEAR(coverage_sum(b), 50.0 * 70.0, 0.5);
    free(b);
    free(a);

    /* A circle of radius 30 px: all off-curve points (TrueType), four cubics from a
     * global subroutine (CFF). */
    o = draw(t, 4, 0);
    CHECK(o);
    CHECK_NEAR(coverage_sum(o), 3.14159265 * 900.0, 3.14159265 * 900.0 * 0.004);
    CHECK_EQ(px(o, 35, 45), 255);              /* the centre */
    CHECK_EQ(px(o, 3, 45), 0);
    free(o);
    o = draw(c, 4, 0);
    CHECK(o);
    CHECK_NEAR(coverage_sum(o), 3.14159265 * 900.0, 3.14159265 * 900.0 * 0.003);
    free(o);
    o = draw(cid, 4, 0);
    CHECK(o);
    CHECK_NEAR(coverage_sum(o), 3.14159265 * 900.0, 3.14159265 * 900.0 * 0.003);
    free(o);

    /* A square with a square hole. */
    s = draw(t, 6, 0);
    CHECK(s);
    CHECK_NEAR(coverage_sum(s), 60.0 * 60.0 - 20.0 * 20.0, 0.5);
    CHECK_EQ(px(s, 40, 40), 0);
    CHECK_EQ(px(s, 15, 40), 255);
    free(s);
    s = draw(c, 6, 0);
    CHECK(s);
    CHECK_NEAR(coverage_sum(s), 60.0 * 60.0 - 20.0 * 20.0, 0.5);
    free(s);

    /* The composite: "A" at half size, moved (200, 100): x 250..500, y 100..450. */
    b = draw(t, 3, 0);
    CHECK(b);
    CHECK_NEAR(coverage_sum(b), 25.0 * 35.0, 0.5);
    CHECK_EQ(px(b, 25, 35), 255);
    CHECK_EQ(px(b, 49, 69), 255);
    CHECK_EQ(px(b, 24, 50), 0);
    CHECK_EQ(px(b, 30, 70), 0);
    free(b);
    /* The CFF "B": a flex bump over x 100..700, rising to y 640 in the middle. */
    b = draw(c, 3, 0);
    CHECK(b);
    CHECK_EQ(px(b, 40, 18), 255);              /* x 400..410, y 610..620 */
    CHECK_EQ(px(b, 40, 15), 0);                /* y 640..650 */
    CHECK_EQ(px(b, 69, 40), 255);
    free(b);

    /* An empty glyph and a glyph out of range. */
    CHECK_EQ(fa_font_outline(t, 1, &p), FA_OK);
    CHECK_EQ(p.count, 0);
    CHECK_EQ(fa_font_outline(c, 1, &p), FA_OK);
    CHECK_EQ(p.count, 0);
    CHECK_EQ(fa_font_outline(t, 7, &p), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_font_outline(t, 2, &p), FA_OK);
    CHECK(p.count == 5 && p.cmd[0].op == FA_PATH_MOVE);
    CHECK(p.xmin == 100 && p.xmax == 600 && p.ymin == 0 && p.ymax == 700);
    fa_path_free(&p);
    fa_font_close(t);
    fa_font_close(c);
    fa_font_close(cid);
}

static void test_collection(void)
{
    blob b = load("pair.ttc");
    fa_font *f0 = NULL, *f1 = NULL, *f2 = NULL;
    CHECK(b.n);
    CHECK_EQ(fa_font_face_count(b.p, b.n), 2);
    CHECK_EQ(fa_font_open(b.p, b.n, 0, 0, &f0), FA_OK);
    CHECK_EQ(fa_font_open(b.p, b.n, 1, 0, &f1), FA_OK);
    CHECK_EQ(fa_font_open(b.p, b.n, 2, 0, &f2), FA_ERROR_NOT_FOUND);
    CHECK(!fa_font_get_info(f0)->cff && fa_font_get_info(f1)->cff);
    CHECK_EQ(fa_font_kerning(f0, 2, 5), -80);
    {
        uint8_t* a = draw(f1, 2, 0);
        CHECK(a);
        check_square_a(a);
        free(a);
    }
    fa_font_close(f0);
    fa_font_close(f1);
    CHECK_EQ(fa_font_face_count((const uint8_t*)"not a font at all", 17), 0);
    CHECK_EQ(fa_font_open((const uint8_t*)"not a font at all", 17, 0, 0, &f0), FA_ERROR_UNSUPPORTED_FORMAT);
    CHECK(f0 == NULL);
    free(b.p);
}

/* Composites eight deep, 64 to a level, over an empty glyph: refused at once
 * instead of drawing 64^8 nothing. */
static void test_composite_bomb(void)
{
    fa_font* f = open_font("bomb.ttf", 0);
    fa_path p = { 0 };
    CHECK(f);
    CHECK_EQ(fa_font_outline(f, fa_font_glyph(f, 'A'), &p), FA_ERROR_UNSUPPORTED_FORMAT);
    CHECK_EQ(p.count, 0);
    CHECK_EQ(fa_font_outline(f, 2, &p), FA_OK);    /* one level: 64 empty components */
    fa_path_free(&p);
    fa_font_close(f);
}

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put16(uint8_t* p, int v) { p[0] = (uint8_t)((unsigned)v >> 8); p[1] = (uint8_t)v; }

/* The offset of table `tag` in a single-face file, or 0. */
static size_t table_at(const blob* b, const char* tag)
{
    uint32_t n = (uint32_t)b->p[4] << 8 | b->p[5], i;
    for (i = 0; i < n && 12 + 16 * (size_t)(i + 1) <= b->n; i++)
        if (memcmp(b->p + 12 + 16 * i, tag, 4) == 0) return be32(b->p + 12 + 16 * i + 8);
    return 0;
}

/* A damaged face with a tiny em and the largest ascender and descender: the usual
 * 0.9 / 1.2 em instead of a line 4000 ems tall. */
static void test_bad_metrics(void)
{
    blob b = load("tt.ttf");
    size_t head, hhea, os2;
    fa_font* f = NULL;
    const fa_font_info* in;
    CHECK(b.n);
    head = table_at(&b, "head");
    hhea = table_at(&b, "hhea");
    os2 = table_at(&b, "OS/2");
    CHECK(head && hhea && head + 20 <= b.n && hhea + 10 <= b.n);
    put16(b.p + head + 18, 16);
    put16(b.p + hhea + 4, 32767);
    put16(b.p + hhea + 6, -32767);
    put16(b.p + hhea + 8, 30000);
    if (os2 && os2 + 74 <= b.n) {
        put16(b.p + os2 + 68, 32767);
        put16(b.p + os2 + 70, -32767);
    }
    CHECK_EQ(fa_font_open(b.p, b.n, 0, 1, &f), FA_OK);
    in = fa_font_get_info(f);
    CHECK_EQ(in->units_per_em, 16);
    CHECK_EQ(in->ascender, 14);
    CHECK_EQ(in->descender, -5);
    CHECK_EQ(in->line_gap, 0);
    fa_font_close(f);
}

static uint32_t g_rand = 12345;
static uint32_t rnd(void)
{
    g_rand = g_rand * 1103515245u + 12345u;
    return g_rand >> 8;
}

/* Everything a layout does with a font, on any input: must not crash or hang. */
static void exercise(const uint8_t* data, size_t size)
{
    uint32_t faces = fa_font_face_count(data, size), i;
    for (i = 0; i < faces && i < 4; i++) {
        fa_font* f = NULL;
        fa_path p = { 0 };
        fa_raster r = { 0 };
        uint8_t img[48 * 48];
        uint32_t g, n, c;
        if (fa_font_open(data, size, i, 0, &f) != FA_OK) continue;
        n = fa_font_get_info(f)->glyph_count;
        for (c = 0; c < 0x80; c++) (void)fa_font_glyph(f, c);
        (void)fa_font_glyph(f, 0x1F600);
        for (g = 0; g < n && g < 64; g++) {
            (void)fa_font_advance(f, g);
            (void)fa_font_kerning(f, g, (g + 3) % n);
            if (fa_font_outline(f, g, &p) == FA_OK && fa_raster_begin(&r, 48, 48)) {
                fa_raster_path(&r, &p, 0.04f, -0.04f, 2.0f, 40.0f);
                fa_raster_end(&r, img, 48);
            }
        }
        fa_raster_free(&r);
        fa_path_free(&p);
        fa_font_close(f);
    }
}

static void test_damage(void)
{
    static const char* names[] = { "tt.ttf", "tt_gpos.ttf", "cff.otf", "cid.otf", "pair.ttc", "bomb.ttf" };
    int rounds = FA_TEST_SANITIZED ? 300 : 3000;
    unsigned i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        blob b = load(names[i]);
        uint8_t* copy;
        int k;
        CHECK(b.n);
        copy = (uint8_t*)malloc(b.n);
        CHECK(copy);
        for (k = 0; k < (strcmp(names[i], "bomb.ttf") ? rounds : rounds / 10); k++) {   /* bomb: slow by design */
            size_t len = b.n, j;
            int hits = 1 + (int)(rnd() % 8);
            memcpy(copy, b.p, b.n);
            for (j = 0; j < (size_t)hits; j++) {
                size_t at = rnd() % b.n;
                switch (rnd() % 4) {
                case 0: copy[at] = (uint8_t)rnd(); break;
                case 1: copy[at] ^= (uint8_t)(1u << (rnd() % 8)); break;
                case 2: copy[at] = rnd() & 1 ? 0xFF : 0x00; break;
                default: len = at + 1; break;    /* cut short */
                }
            }
            exercise(copy, len);
        }
        free(copy);
        free(b.p);
    }
}

/* Installed fonts: a family every desktop OS here has, by weight and style; a
 * family nobody has; the UI font; a Japanese font when the system has one. */
static void check_find(void)
{
#if defined(_WIN32) || defined(__APPLE__)
    const char* family = "Arial";
    int required = 1;
#elif defined(__ANDROID__)
    const char* family = "Roboto";
    int required = 0;
#else
    const char* family = "DejaVu Sans";
    int required = 0;
#endif
    fa_font* f = NULL;
    fa_result r = fa_font_find(family, 700, 0, &f);
    if (r == FA_ERROR_NOT_FOUND && !required) {
        printf("  (%s is not installed)\n", family);
    } else {
        const fa_font_info* in;
        CHECK_EQ(r, FA_OK);
        in = fa_font_get_info(f);
        CHECK(!strcmp(in->family, family) || !strcmp(in->legacy_family, family));
#if defined(__ANDROID__)
        /* Android's Roboto is one variable font, read at its default (regular)
         * instance: the caller draws the bold synthetically. */
        CHECK(!in->italic);
#else
        CHECK(in->weight >= 600 && !in->italic);
#endif
        CHECK_EQ(in->width, 5);                         /* not a condensed face of the family */
        CHECK(fa_font_glyph(f, 'A') != 0);
        fa_font_close(f);
        CHECK_EQ(fa_font_find(family, 400, 1, &f), FA_OK);
        in = fa_font_get_info(f);
        printf("  %s: bold and %s %s (weight %d)\n", family, in->family, in->style, in->weight);
        CHECK(in->weight < 600);
        CHECK_EQ(in->width, 5);
#if defined(_WIN32) || defined(__APPLE__)
        CHECK(in->italic);
#endif
        fa_font_close(f);
        /* Case, spaces and hyphens do not matter. */
        {
            char odd[64];
            size_t i, n = strlen(family);
            for (i = 0; i <= n; i++) odd[i] = (char)(family[i] >= 'a' && family[i] <= 'z' ? family[i] - 32 : family[i] == ' ' ? '-' : family[i]);
            CHECK_EQ(fa_font_find(odd, 400, 0, &f), FA_OK);
            fa_font_close(f);
        }
    }
    f = (fa_font*)1;
    CHECK_EQ(fa_font_find("No Such Family Anywhere 7", 400, 0, &f), FA_ERROR_NOT_FOUND);
    CHECK(f == NULL);
    r = fa_font_find(NULL, 400, 0, &f);
    if (r == FA_OK) {
        printf("  UI font: %s %s\n", fa_font_get_info(f)->family, fa_font_get_info(f)->style);
        CHECK(fa_font_glyph(f, 'a') != 0);
        fa_font_close(f);
    } else {
        CHECK(!required);
    }
    r = fa_font_find_for_language("ja", 400, 0, &f);
    if (r == FA_OK) {
        printf("  Japanese: %s %s\n", fa_font_get_info(f)->family, fa_font_get_info(f)->style);
        CHECK(fa_font_glyph(f, 0x3042) != 0);          /* hiragana a */
        fa_font_close(f);
    } else {
        printf("  (no Japanese font)\n");
        CHECK_EQ(r, FA_ERROR_NOT_FOUND);
    }
}

static void test_find(void)
{
    check_find();
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__ANDROID__)
    /* The same through the font folders, without fontconfig. */
    fa_font_find_set_scan_only(1);
    check_find();
    fa_font_find_set_scan_only(0);
#endif
}

/* Local check: FA_TEST_FONT_DIR=/usr/share/fonts/truetype/dejavu and so on. */
static void test_font_dir(void)
{
#if !defined(_WIN32)
    const char* dir = getenv("FA_TEST_FONT_DIR");
    DIR* d;
    struct dirent* e;
    int opened = 0;
    if (!dir) { printf("  (FA_TEST_FONT_DIR not set)\n"); return; }
    d = opendir(dir);
    CHECK(d);
    while ((e = readdir(d)) != NULL) {
        char path[2048];
        fa_font* f = NULL;
        size_t n = strlen(e->d_name);
        if (n < 5 || (strcmp(e->d_name + n - 4, ".ttf") && strcmp(e->d_name + n - 4, ".otf") &&
                      strcmp(e->d_name + n - 4, ".ttc") && strcmp(e->d_name + n - 4, ".otc")))
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (fa_font_open_file(path, 0, &f) == FA_OK) {
            const fa_font_info* in = fa_font_get_info(f);
            fa_path p = { 0 };
            uint32_t g, bad = 0;
            for (g = 0; g < in->glyph_count; g++)
                if (fa_font_outline(f, g, &p) != FA_OK) bad++;
            printf("  %-40s %-24s %-12s %5u glyphs%s, %u unreadable\n", e->d_name, in->family, in->style,
                   in->glyph_count, in->cff ? " CFF" : "", bad);
            fa_path_free(&p);
            fa_font_close(f);
            opened++;
        } else {
            printf("  %-40s not opened\n", e->d_name);
        }
    }
    closedir(d);
    CHECK(opened > 0);
#endif
}

/* Faces opened from one unchanged file share a buffer, freed when the last closes;
 * a file changed on disk gets a new one. */
static void test_shared_buffers(void)
{
    char a[1024], b[1024];
    const char* tmp = "font_share_test.ttf";
    fa_font *f1 = NULL, *f2 = NULL, *f3 = NULL, *f4 = NULL;
    blob x, y;
    FILE* fp;
    CHECK_EQ(fa_font_shared_buffers(), 0);
    snprintf(a, sizeof(a), "%s/fonts/pair.ttc", FA_TEST_DATA_DIR);
    snprintf(b, sizeof(b), "%s/fonts/tt.ttf", FA_TEST_DATA_DIR);
    CHECK_EQ(fa_font_open_file(a, 0, &f1), FA_OK);
    CHECK_EQ(fa_font_open_file(a, 1, &f2), FA_OK);
    CHECK_EQ(fa_font_shared_buffers(), 1);
    CHECK_EQ(fa_font_open_file(b, 0, &f3), FA_OK);
    CHECK_EQ(fa_font_shared_buffers(), 2);
    fa_font_close(f1);
    CHECK_EQ(fa_font_shared_buffers(), 2);
    CHECK(fa_font_get_info(f2)->glyph_count > 0);   /* still readable */
    fa_font_close(f3);
    CHECK_EQ(fa_font_shared_buffers(), 1);
    fa_font_close(f2);
    CHECK_EQ(fa_font_shared_buffers(), 0);
    /* The same path with other contents is a new buffer; the old face keeps its own. */
    x = load("tt.ttf");
    y = load("cff.otf");
    CHECK(x.n && y.n);
    fp = fopen(tmp, "wb");
    CHECK(fp);
    fwrite(x.p, 1, x.n, fp);
    fclose(fp);
    CHECK_EQ(fa_font_open_file(tmp, 0, &f1), FA_OK);
    fp = fopen(tmp, "wb");
    CHECK(fp);
    fwrite(y.p, 1, y.n, fp);
    fclose(fp);
    CHECK_EQ(fa_font_open_file(tmp, 0, &f4), FA_OK);
    CHECK_EQ(fa_font_shared_buffers(), 2);
    CHECK(!fa_font_get_info(f1)->cff && fa_font_get_info(f4)->cff);
    fa_font_close(f4);
    fa_font_close(f1);
    CHECK_EQ(fa_font_shared_buffers(), 0);
    /* Same size, one byte different (the last byte of a table nothing reads): still a new buffer. */
    CHECK_EQ(fa_font_open_file(tmp, 0, &f1), FA_OK);
    y.p[y.n - 1] ^= 0x55;
    fp = fopen(tmp, "wb");
    CHECK(fp);
    fwrite(y.p, 1, y.n, fp);
    fclose(fp);
    CHECK_EQ(fa_font_open_file(tmp, 0, &f4), FA_OK);
    CHECK_EQ(fa_font_shared_buffers(), 2);
    fa_font_close(f4);
    fa_font_close(f1);
    CHECK_EQ(fa_font_shared_buffers(), 0);
    remove(tmp);
    free(x.p);
    free(y.p);
    /* A file that is no font leaves nothing behind. */
    CHECK(fa_font_open_file(tmp, 0, &f1) != FA_OK);
    CHECK_EQ(fa_font_shared_buffers(), 0);
}

int main(void)
{
    RUN(test_info_and_maps);
    RUN(test_kerning);
    RUN(test_outlines);
    RUN(test_collection);
    RUN(test_shared_buffers);
    RUN(test_composite_bomb);
    RUN(test_bad_metrics);
    RUN(test_damage);
    RUN(test_find);
    RUN(test_font_dir);
    if (g_failures) { fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    printf("all passed\n");
    return 0;
}
