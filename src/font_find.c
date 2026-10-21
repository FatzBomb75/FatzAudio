/* Finding installed fonts (font_find.h). Each OS gives candidate files (and faces);
 * our own reader reads their names and styles, and the nearest face of the
 * family wins. */
#include "font_find.h"
#include "fa_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <dirent.h>
#  include <sys/stat.h>
#endif
#ifdef __APPLE__
#  include <CoreFoundation/CoreFoundation.h>
#  include <CoreText/CoreText.h>
#endif

#define FIND_MAX_PATH 1024

static int g_scan_only;

void fa_font_find_set_scan_only(int scan_only) { g_scan_only = scan_only; }

/* ---- matching ------------------------------------------------------------------- */

/* Family names compare without case, spaces, hyphens and underscores. */
static int family_eq(const char* a, const char* b)
{
    for (;;) {
        while (*a == ' ' || *a == '-' || *a == '_') a++;
        while (*b == ' ' || *b == '-' || *b == '_') b++;
        if (!*a || !*b) return !*a && !*b;
        {
            unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
            if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
            if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
            if (ca != cb) return 0;
        }
        a++;
        b++;
    }
}

static int face_score(const fa_font_info* in, int weight, int italic)
{
    int d = in->weight - weight;
    return (d < 0 ? -d : d) + (!in->italic != !italic ? 1000 : 0);
}

typedef struct best_face {
    char path[FIND_MAX_PATH];
    uint32_t index;
    int score;                        /* -1: none yet */
} best_face;

static void consider(best_face* b, const char* path, uint32_t index, const fa_font_info* in, int weight, int italic)
{
    int s = face_score(in, weight, italic);
    size_t n;
    if (b->score >= 0 && s >= b->score) return;
    n = strlen(path);
    if (n >= sizeof(b->path)) return;
    memcpy(b->path, path, n + 1);
    b->index = index;
    b->score = s;
}

/* Every face of the file at `path` of `family` (any family when NULL). */
static void probe_file(best_face* b, const char* path, const char* family, int weight, int italic)
{
    FILE* fp = fa_fopen_utf8(path, "rb");
    uint32_t faces = 1, i;
    if (!fp) return;
    for (i = 0; i < faces && i < 64; i++) {
        fa_font_info in;
        if (fa_font_probe(fp, &faces, i, &in) != FA_OK) continue;
        if (!family || family_eq(family, in.family) || family_eq(family, in.legacy_family))
            consider(b, path, i, &in, weight, italic);
    }
    fclose(fp);
}

static fa_result open_best(const best_face* b, fa_font** out)
{
    if (b->score < 0) return FA_ERROR_NOT_FOUND;
    return fa_font_open_file(b->path, b->index, out);
}

/* ---- the font folders (Linux without fontconfig, Android before 10) --------------- */

static int font_ext(const char* name)
{
    size_t n = strlen(name);
    const char* e = n > 4 ? name + n - 4 : "";
    return !strcmp(e, ".ttf") || !strcmp(e, ".otf") || !strcmp(e, ".ttc") || !strcmp(e, ".otc") ||
           !strcmp(e, ".TTF") || !strcmp(e, ".OTF") || !strcmp(e, ".TTC");
}

#ifndef _WIN32
static void scan_dir(best_face* b, const char* dir, int depth, const char* family, int weight, int italic)
{
    DIR* d;
    struct dirent* e;
    if (depth > 5 || !(d = opendir(dir))) return;
    while ((e = readdir(d)) != NULL) {
        char path[FIND_MAX_PATH];
        struct stat st;
        if (e->d_name[0] == '.') continue;
        if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= sizeof(path)) continue;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) scan_dir(b, path, depth + 1, family, weight, italic);
        else if (S_ISREG(st.st_mode) && font_ext(e->d_name)) probe_file(b, path, family, weight, italic);
    }
    closedir(d);
}

static void scan_folders(best_face* b, const char* family, int weight, int italic)
{
#  if defined(__ANDROID__)
    static const char* dirs[] = { "/system/fonts", "/product/fonts", "/system/product/fonts" };
#  elif defined(__APPLE__)
    static const char* dirs[] = { "/System/Library/Fonts", "/Library/Fonts" };
#  else
    static const char* dirs[] = { "/usr/share/fonts", "/usr/local/share/fonts" };
#  endif
    const char* home = getenv("HOME");
    size_t i;
    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) scan_dir(b, dirs[i], 0, family, weight, italic);
    if (home) {
        char p[FIND_MAX_PATH];
        snprintf(p, sizeof(p), "%s/.local/share/fonts", home);
        scan_dir(b, p, 0, family, weight, italic);
        snprintf(p, sizeof(p), "%s/.fonts", home);
        scan_dir(b, p, 0, family, weight, italic);
#  ifdef __APPLE__
        snprintf(p, sizeof(p), "%s/Library/Fonts", home);
        scan_dir(b, p, 0, family, weight, italic);
#  endif
    }
}
#endif

/* ---- Windows: the registry's font list -------------------------------------------- */

#ifdef _WIN32
/* 1 when one of the "&"-separated names before " (TrueType)" starts with `family`. */
static int registry_name_has(const char* value, const char* family)
{
    const char* p = value;
    while (*p) {
        const char *a = p, *f = family;
        while (*p == ' ') p++;
        a = p;
        /* Does this name start with the family (as family_eq compares)? */
        for (;;) {
            while (*a == ' ' || *a == '-' || *a == '_') a++;
            while (*f == ' ' || *f == '-' || *f == '_') f++;
            if (!*f) {
                if (!*a || *a == ' ' || *a == '(' || *a == '&') return 1;
                break;
            }
            if (!*a) break;
            {
                unsigned char ca = (unsigned char)*a, cb = (unsigned char)*f;
                if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
                if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
                if (ca != cb) break;
            }
            a++;
            f++;
        }
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }
    return 0;
}

static void registry_fonts(best_face* b, HKEY root, const char* family, int weight, int italic)
{
    HKEY key;
    DWORD i;
    wchar_t windir[MAX_PATH];
    if (RegOpenKeyExW(root, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts", 0, KEY_READ, &key) != ERROR_SUCCESS) return;
    if (!GetWindowsDirectoryW(windir, MAX_PATH)) windir[0] = 0;
    for (i = 0;; i++) {
        wchar_t name[512], data[MAX_PATH];
        DWORD nlen = 512, dlen = sizeof(data), type;
        char uname[1024], upath[FIND_MAX_PATH];
        LONG r = RegEnumValueW(key, i, name, &nlen, NULL, &type, (BYTE*)data, &dlen);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS || type != REG_SZ) continue;
        /* A registry string need not end in a terminator. */
        data[dlen / sizeof(wchar_t) < MAX_PATH ? dlen / sizeof(wchar_t) : MAX_PATH - 1] = 0;
        if (!WideCharToMultiByte(CP_UTF8, 0, name, -1, uname, sizeof(uname), NULL, NULL)) continue;
        if (family && !registry_name_has(uname, family)) continue;
        if (wcschr(data, L'\\')) {
            if (!WideCharToMultiByte(CP_UTF8, 0, data, -1, upath, sizeof(upath), NULL, NULL)) continue;
        } else {
            static const wchar_t fonts[] = L"\\Fonts\\";
            wchar_t full[MAX_PATH * 2];
            size_t a = wcslen(windir), f = sizeof(fonts) / sizeof(wchar_t) - 1, d = wcslen(data);
            if (a + f + d >= MAX_PATH * 2) continue;
            memcpy(full, windir, a * sizeof(wchar_t));
            memcpy(full + a, fonts, f * sizeof(wchar_t));
            memcpy(full + a + f, data, (d + 1) * sizeof(wchar_t));
            if (!WideCharToMultiByte(CP_UTF8, 0, full, -1, upath, sizeof(upath), NULL, NULL)) continue;
        }
        if (font_ext(upath)) probe_file(b, upath, family, weight, italic);
    }
    RegCloseKey(key);
}

static void os_family(best_face* b, const char* family, int weight, int italic)
{
    registry_fonts(b, HKEY_LOCAL_MACHINE, family, weight, italic);
    registry_fonts(b, HKEY_CURRENT_USER, family, weight, italic);   /* fonts installed for this user */
}

static const char* const* language_families(const char* lang)
{
    static const char* ja[] = { "Yu Gothic", "Meiryo", "MS Gothic", NULL };
    static const char* ko[] = { "Malgun Gothic", "Gulim", NULL };
    static const char* sc[] = { "Microsoft YaHei", "DengXian", "SimSun", NULL };
    static const char* tc[] = { "Microsoft JhengHei", "MingLiU", NULL };
    static const char* ui[] = { "Segoe UI", "Arial", "Tahoma", NULL };
    if (!lang) return ui;
    if (!strncmp(lang, "ja", 2)) return ja;
    if (!strncmp(lang, "ko", 2)) return ko;
    if (!strncmp(lang, "zh-Hant", 7) || !strncmp(lang, "zh-TW", 5) || !strncmp(lang, "zh-HK", 5)) return tc;
    if (!strncmp(lang, "zh", 2)) return sc;
    return ui;
}

/* ---- Apple: CoreText ----------------------------------------------------------- */

#elif defined(__APPLE__)
static void os_family(best_face* b, const char* family, int weight, int italic)
{
    CFStringRef name;
    if (g_scan_only) { scan_folders(b, family, weight, italic); return; }
    name = CFStringCreateWithCString(NULL, family, kCFStringEncodingUTF8);
    CFDictionaryRef attrs;
    CTFontDescriptorRef desc;
    CFArrayRef all;
    CFIndex i;
    const void* keys[1];
    const void* vals[1];
    if (!name) return;
    keys[0] = kCTFontFamilyNameAttribute;
    vals[0] = name;
    attrs = CFDictionaryCreate(NULL, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    desc = attrs ? CTFontDescriptorCreateWithAttributes(attrs) : NULL;
    all = desc ? CTFontDescriptorCreateMatchingFontDescriptors(desc, NULL) : NULL;
    for (i = 0; all && i < CFArrayGetCount(all); i++) {
        CTFontDescriptorRef d = (CTFontDescriptorRef)CFArrayGetValueAtIndex(all, i);
        CFURLRef url = (CFURLRef)CTFontDescriptorCopyAttribute(d, kCTFontURLAttribute);
        char path[FIND_MAX_PATH];
        if (!url) continue;
        if (CFURLGetFileSystemRepresentation(url, true, (UInt8*)path, sizeof(path)) && font_ext(path))
            probe_file(b, path, family, weight, italic);
        CFRelease(url);
    }
    if (all) CFRelease(all);
    if (desc) CFRelease(desc);
    if (attrs) CFRelease(attrs);
    CFRelease(name);
}

static const char* const* language_families(const char* lang)
{
    static const char* ja[] = { "Hiragino Sans", "Hiragino Kaku Gothic ProN", NULL };
    static const char* ko[] = { "Apple SD Gothic Neo", NULL };
    static const char* sc[] = { "PingFang SC", "Heiti SC", "STHeiti", NULL };
    static const char* tc[] = { "PingFang TC", "Heiti TC", NULL };
    static const char* ui[] = { "Helvetica Neue", "Helvetica", "Arial", NULL };
    if (!lang) return ui;
    if (!strncmp(lang, "ja", 2)) return ja;
    if (!strncmp(lang, "ko", 2)) return ko;
    if (!strncmp(lang, "zh-Hant", 7) || !strncmp(lang, "zh-TW", 5) || !strncmp(lang, "zh-HK", 5)) return tc;
    if (!strncmp(lang, "zh", 2)) return sc;
    return ui;
}

/* ---- Android: the system font matcher (API 29), else the font folders ----------- */

#elif defined(__ANDROID__)
typedef struct AFontMatcher AFontMatcher;
typedef struct AFont AFont;
static struct {
    int tried;
    AFontMatcher* (*create)(void);
    void (*destroy)(AFontMatcher*);
    void (*set_style)(AFontMatcher*, uint16_t, int);
    void (*set_locales)(AFontMatcher*, const char*);
    AFont* (*match)(const AFontMatcher*, const char*, const uint16_t*, uint32_t, uint32_t*);
    const char* (*path)(const AFont*);
    size_t (*index)(const AFont*);
    void (*close)(AFont*);
} g_afm;
static fa_lazy_mutex g_afm_lock;

static int afm_load(void)
{
    fa_lazy_mutex_lock(&g_afm_lock);
    if (!g_afm.tried) {
        void* lib = fa_lib_open("libandroid.so", 0);
        g_afm.tried = 1;
        if (lib) {
            g_afm.create = (AFontMatcher * (*)(void)) fa_lib_sym(lib, "AFontMatcher_create");
            g_afm.destroy = (void (*)(AFontMatcher*))fa_lib_sym(lib, "AFontMatcher_destroy");
            g_afm.set_style = (void (*)(AFontMatcher*, uint16_t, int))fa_lib_sym(lib, "AFontMatcher_setStyle");
            g_afm.set_locales = (void (*)(AFontMatcher*, const char*))fa_lib_sym(lib, "AFontMatcher_setLocales");
            g_afm.match = (AFont * (*)(const AFontMatcher*, const char*, const uint16_t*, uint32_t, uint32_t*))
                fa_lib_sym(lib, "AFontMatcher_match");
            g_afm.path = (const char* (*)(const AFont*))fa_lib_sym(lib, "AFont_getFontFilePath");
            g_afm.index = (size_t(*)(const AFont*))fa_lib_sym(lib, "AFont_getCollectionIndex");
            g_afm.close = (void (*)(AFont*))fa_lib_sym(lib, "AFont_close");
            if (!g_afm.create || !g_afm.destroy || !g_afm.set_style || !g_afm.set_locales || !g_afm.match ||
                !g_afm.path || !g_afm.index || !g_afm.close)
                g_afm.create = NULL;
        }
    }
    fa_lazy_mutex_unlock(&g_afm_lock);
    return g_afm.create != NULL;
}

/* The matcher's face for a family (or a generic name) and a language; its file
 * and face become the candidate, checked against `family` when given. */
static void afm_match(best_face* b, const char* name, const char* locales, const char* check_family, int weight, int italic)
{
    static const uint16_t latin[] = { 'a' }, kana[] = { 0x3042 }, hangul[] = { 0xAC00 }, han[] = { 0x4E2D };
    const uint16_t* text = latin;
    AFontMatcher* m;
    AFont* f;
    uint32_t run = 0;
    if (!afm_load() || !(m = g_afm.create())) return;
    g_afm.set_style(m, (uint16_t)weight, italic);
    if (locales) {
        g_afm.set_locales(m, locales);
        text = !strncmp(locales, "ja", 2) ? kana : !strncmp(locales, "ko", 2) ? hangul : !strncmp(locales, "zh", 2) ? han : latin;
    }
    f = g_afm.match(m, name, text, 1, &run);
    if (f) {
        const char* path = g_afm.path(f);
        FILE* fp = path ? fa_fopen_utf8(path, "rb") : NULL;
        if (fp) {
            fa_font_info in;
            uint32_t idx = (uint32_t)g_afm.index(f);
            if (fa_font_probe(fp, NULL, idx, &in) == FA_OK &&
                (!check_family || family_eq(check_family, in.family) || family_eq(check_family, in.legacy_family))) {
                fclose(fp);
                fp = NULL;
                consider(b, path, idx, &in, weight, italic);
            }
            if (fp) fclose(fp);
        }
        g_afm.close(f);
    }
    g_afm.destroy(m);
}

static void os_family(best_face* b, const char* family, int weight, int italic)
{
    if (afm_load()) afm_match(b, family, NULL, family, weight, italic);
    if (b->score < 0) scan_folders(b, family, weight, italic);
}

static const char* const* language_families(const char* lang)
{
    static const char* ja[] = { "Noto Sans CJK JP", "Noto Sans JP", "Droid Sans Japanese", "Droid Sans Fallback", NULL };
    static const char* ko[] = { "Noto Sans CJK KR", "Noto Sans KR", "Droid Sans Fallback", NULL };
    static const char* sc[] = { "Noto Sans CJK SC", "Noto Sans SC", "Droid Sans Fallback", NULL };
    static const char* tc[] = { "Noto Sans CJK TC", "Noto Sans TC", "Droid Sans Fallback", NULL };
    static const char* ui[] = { "Roboto", "Noto Sans", "Droid Sans", NULL };
    if (!lang) return ui;
    if (!strncmp(lang, "ja", 2)) return ja;
    if (!strncmp(lang, "ko", 2)) return ko;
    if (!strncmp(lang, "zh-Hant", 7) || !strncmp(lang, "zh-TW", 5) || !strncmp(lang, "zh-HK", 5)) return tc;
    if (!strncmp(lang, "zh", 2)) return sc;
    return ui;
}

/* ---- Linux and the rest: fontconfig when the system has it, else the folders ------- */

#else
typedef struct FcPattern FcPattern;
typedef struct FcObjectSet FcObjectSet;
typedef struct FcFontSet { int nfont; int sfont; FcPattern** fonts; } FcFontSet;
static struct {
    int tried;
    int (*init)(void);
    FcPattern* (*pattern_create)(void);
    void (*pattern_destroy)(FcPattern*);
    int (*add_string)(FcPattern*, const char*, const unsigned char*);
    int (*get_string)(const FcPattern*, const char*, int, unsigned char**);
    int (*get_integer)(const FcPattern*, const char*, int, int*);
    FcObjectSet* (*object_set_build)(const char*, ...);
    void (*object_set_destroy)(FcObjectSet*);
    FcFontSet* (*font_list)(void*, FcPattern*, FcObjectSet*);
    void (*font_set_destroy)(FcFontSet*);
    int (*substitute)(void*, FcPattern*, int);
    void (*default_substitute)(FcPattern*);
    FcPattern* (*font_match)(void*, FcPattern*, int*);
} g_fc;
static fa_lazy_mutex g_fc_lock;

static int fc_load(void)
{
    fa_lazy_mutex_lock(&g_fc_lock);
    if (!g_fc.tried) {
        void* lib = fa_lib_open("libfontconfig.so.1", 0);
        g_fc.tried = 1;
        if (lib) {
            g_fc.init = (int (*)(void))fa_lib_sym(lib, "FcInit");
            g_fc.pattern_create = (FcPattern * (*)(void)) fa_lib_sym(lib, "FcPatternCreate");
            g_fc.pattern_destroy = (void (*)(FcPattern*))fa_lib_sym(lib, "FcPatternDestroy");
            g_fc.add_string = (int (*)(FcPattern*, const char*, const unsigned char*))fa_lib_sym(lib, "FcPatternAddString");
            g_fc.get_string = (int (*)(const FcPattern*, const char*, int, unsigned char**))fa_lib_sym(lib, "FcPatternGetString");
            g_fc.get_integer = (int (*)(const FcPattern*, const char*, int, int*))fa_lib_sym(lib, "FcPatternGetInteger");
            g_fc.object_set_build = (FcObjectSet * (*)(const char*, ...)) fa_lib_sym(lib, "FcObjectSetBuild");
            g_fc.object_set_destroy = (void (*)(FcObjectSet*))fa_lib_sym(lib, "FcObjectSetDestroy");
            g_fc.font_list = (FcFontSet * (*)(void*, FcPattern*, FcObjectSet*)) fa_lib_sym(lib, "FcFontList");
            g_fc.font_set_destroy = (void (*)(FcFontSet*))fa_lib_sym(lib, "FcFontSetDestroy");
            g_fc.substitute = (int (*)(void*, FcPattern*, int))fa_lib_sym(lib, "FcConfigSubstitute");
            g_fc.default_substitute = (void (*)(FcPattern*))fa_lib_sym(lib, "FcDefaultSubstitute");
            g_fc.font_match = (FcPattern * (*)(void*, FcPattern*, int*)) fa_lib_sym(lib, "FcFontMatch");
            if (!g_fc.init || !g_fc.pattern_create || !g_fc.pattern_destroy || !g_fc.add_string || !g_fc.get_string ||
                !g_fc.get_integer || !g_fc.object_set_build || !g_fc.object_set_destroy || !g_fc.font_list ||
                !g_fc.font_set_destroy || !g_fc.substitute || !g_fc.default_substitute || !g_fc.font_match || !g_fc.init())
                g_fc.init = NULL;
        }
    }
    fa_lazy_mutex_unlock(&g_fc_lock);
    return g_fc.init != NULL;
}

/* Every face fontconfig lists for the family. Its whole list is read (from its
 * cache, no font files) and the names compared as family_eq does. */
static void fc_family(best_face* b, const char* family, int weight, int italic)
{
    FcPattern* p = g_fc.pattern_create();
    FcObjectSet* os = g_fc.object_set_build("file", "index", "family", (char*)NULL);
    FcFontSet* set = NULL;
    int i;
    if (p && os) set = g_fc.font_list(NULL, p, os);
    for (i = 0; set && i < set->nfont; i++) {
        unsigned char *file = NULL, *name = NULL;
        int index = 0, k, named = 0;
        FILE* fp;
        fa_font_info in;
        for (k = 0; !named && k < 16 && g_fc.get_string(set->fonts[i], "family", k, &name) == 0; k++)
            named = name && family_eq(family, (const char*)name);
        if (!named) continue;
        if (g_fc.get_string(set->fonts[i], "file", 0, &file) != 0 || !file) continue;
        g_fc.get_integer(set->fonts[i], "index", 0, &index);
        if (!font_ext((const char*)file) || !(fp = fa_fopen_utf8((const char*)file, "rb"))) continue;
        /* The low 16 bits are the face; the rest names a variable font's instance. */
        if (fa_font_probe(fp, NULL, (uint32_t)(index & 0xFFFF), &in) == FA_OK &&
            (family_eq(family, in.family) || family_eq(family, in.legacy_family)))
            consider(b, (const char*)file, (uint32_t)(index & 0xFFFF), &in, weight, italic);
        fclose(fp);
    }
    if (set) g_fc.font_set_destroy(set);
    if (os) g_fc.object_set_destroy(os);
    if (p) g_fc.pattern_destroy(p);
}

/* The family fontconfig picks for a generic name and a language. */
static int fc_pick(const char* generic, const char* lang, char* out, size_t cap)
{
    FcPattern *p = g_fc.pattern_create(), *m = NULL;
    unsigned char* fam = NULL;
    int result = 0, ok = 0;
    if (!p) return 0;
    g_fc.add_string(p, "family", (const unsigned char*)generic);
    if (lang) g_fc.add_string(p, "lang", (const unsigned char*)lang);
    g_fc.substitute(NULL, p, 0);       /* FcMatchPattern */
    g_fc.default_substitute(p);
    m = g_fc.font_match(NULL, p, &result);
    if (m && g_fc.get_string(m, "family", 0, &fam) == 0 && fam && strlen((const char*)fam) < cap) {
        memcpy(out, fam, strlen((const char*)fam) + 1);
        ok = 1;
    }
    if (m) g_fc.pattern_destroy(m);
    g_fc.pattern_destroy(p);
    return ok;
}

static void os_family(best_face* b, const char* family, int weight, int italic)
{
    if (!g_scan_only && fc_load()) fc_family(b, family, weight, italic);
    else scan_folders(b, family, weight, italic);
}

static const char* const* language_families(const char* lang)
{
    static const char* ja[] = { "Noto Sans CJK JP", "Noto Sans JP", "Source Han Sans JP", "IPAexGothic", "IPAGothic",
                                "TakaoGothic", "VL Gothic", "WenQuanYi Zen Hei", "Droid Sans Fallback", NULL };
    static const char* ko[] = { "Noto Sans CJK KR", "Noto Sans KR", "Source Han Sans KR", "NanumGothic", "UnDotum", NULL };
    static const char* sc[] = { "Noto Sans CJK SC", "Noto Sans SC", "Source Han Sans SC", "WenQuanYi Zen Hei",
                                "WenQuanYi Micro Hei", "Droid Sans Fallback", NULL };
    static const char* tc[] = { "Noto Sans CJK TC", "Noto Sans TC", "Source Han Sans TC", "WenQuanYi Zen Hei",
                                "AR PL UMing TW", "Droid Sans Fallback", NULL };
    static const char* ui[] = { "DejaVu Sans", "Noto Sans", "Liberation Sans", "Cantarell", "FreeSans", "Roboto", NULL };
    if (!lang) return ui;
    if (!strncmp(lang, "ja", 2)) return ja;
    if (!strncmp(lang, "ko", 2)) return ko;
    if (!strncmp(lang, "zh-Hant", 7) || !strncmp(lang, "zh-TW", 5) || !strncmp(lang, "zh-HK", 5)) return tc;
    if (!strncmp(lang, "zh", 2)) return sc;
    return ui;
}
#endif

/* ---- the API -------------------------------------------------------------------- */

static fa_result find_in_list(const char* const* families, int weight, int italic, fa_font** out)
{
    for (; *families; families++) {
        best_face b;
        b.score = -1;
        os_family(&b, *families, weight, italic);
        if (b.score >= 0) return open_best(&b, out);
    }
    return FA_ERROR_NOT_FOUND;
}

/* A fontconfig or Android matcher pick for a generic family and language. */
static fa_result find_generic(const char* lang, int weight, int italic, fa_font** out)
{
#if defined(__ANDROID__)
    best_face b;
    b.score = -1;
    if (!g_scan_only && afm_load()) {
        char loc[32];
        if (lang) {
            snprintf(loc, sizeof(loc), "%s", !strcmp(lang, "zh-Hans") ? "zh-CN" : !strcmp(lang, "zh-Hant") ? "zh-TW" : lang);
            afm_match(&b, "sans-serif", loc, NULL, weight, italic);
        } else {
            afm_match(&b, "sans-serif", NULL, NULL, weight, italic);
        }
        if (b.score >= 0) return open_best(&b, out);
    }
#elif !defined(_WIN32) && !defined(__APPLE__)
    char fam[256];
    if (!g_scan_only && fc_load()) {
        const char* fl = !lang ? NULL : !strcmp(lang, "zh-Hans") ? "zh-cn" : !strcmp(lang, "zh-Hant") ? "zh-tw" : lang;
        if (fc_pick("sans-serif", fl, fam, sizeof(fam))) {
            best_face b;
            b.score = -1;
            fc_family(&b, fam, weight, italic);
            if (b.score >= 0) return open_best(&b, out);
        }
    }
#else
    (void)lang; (void)weight; (void)italic; (void)out;
#endif
    return FA_ERROR_NOT_FOUND;
}

fa_result fa_font_find(const char* family, int weight, int italic, fa_font** out)
{
    best_face b;
    if (!out) return FA_ERROR_INVALID_ARG;
    *out = NULL;
    if (weight < 1) weight = 400;
    if (weight > 1000) weight = 1000;
    if (!family || !*family) {
        fa_result r = find_generic(NULL, weight, italic, out);
        return r == FA_OK ? r : find_in_list(language_families(NULL), weight, italic, out);
    }
    b.score = -1;
    os_family(&b, family, weight, italic);
    return open_best(&b, out);
}

fa_result fa_font_find_for_language(const char* language, int weight, int italic, fa_font** out)
{
    fa_result r;
    if (!out) return FA_ERROR_INVALID_ARG;
    *out = NULL;
    if (weight < 1) weight = 400;
    if (weight > 1000) weight = 1000;
    /* The lists first: the OS matcher's pick for a language can be a font that
     * only covers part of it. */
    r = find_in_list(language_families(language), weight, italic, out);
    if (r == FA_OK) return r;
    return find_generic(language, weight, italic, out);
}
