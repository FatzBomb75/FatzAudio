/* The OS shaper on Windows (shape.h): DirectWrite's text analyzer, which finds each
 * run's script and direction (Unicode bidi) and shapes it with the font's OpenType
 * tables (Windows 10 1703 and later: the font is handed over from memory).
 * dwrite.dll is loaded at run time; without it, or on an older Windows, lyrics use
 * our own layout. C++ only for DirectWrite's COM interfaces: no exceptions, no
 * standard library. */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <dwrite_3.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "shape.h"
}

namespace {

typedef HRESULT(WINAPI* create_factory_fn)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**);

IDWriteFactory5* g_factory;
INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK load_once(PINIT_ONCE, PVOID, PVOID*)
{
    HMODULE dll = LoadLibraryExW(L"dwrite.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    create_factory_fn create;
    IUnknown* unk = NULL;
    if (!dll) dll = LoadLibraryW(L"dwrite.dll");   /* systems without the search flags */
    if (!dll) return TRUE;
    create = (create_factory_fn)(void*)GetProcAddress(dll, "DWriteCreateFactory");
    /* IDWriteFactory5 (Windows 10 1703): fonts from memory. Stays loaded for good. */
    if (!create || FAILED(create(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5), &unk)) || !unk) return TRUE;
    g_factory = static_cast<IDWriteFactory5*>(unk);
    return TRUE;
}

bool loaded()
{
    InitOnceExecuteOnce(&g_once, load_once, NULL, NULL);
    return g_factory != NULL;
}

/* A loader of its own for each font: an in-memory loader keeps every font given to it
 * until it is released, so the font's copy goes with it. */
struct dw_state {
    IDWriteInMemoryFontFileLoader* loader;
    bool registered;
    IDWriteFontFile* file;
    IDWriteFontFace* face;
    IDWriteTextAnalyzer* analyzer;
};

void state_free(dw_state* st)
{
    if (!st) return;
    if (st->analyzer) st->analyzer->Release();
    if (st->face) st->face->Release();
    if (st->file) st->file->Release();
    if (st->registered) g_factory->UnregisterFontFileLoader(st->loader);
    if (st->loader) st->loader->Release();
    free(st);
}

dw_state* state_for(fa_font* font, void** slot)
{
    const uint8_t* data;
    size_t size;
    uint32_t index;
    BOOL supported = FALSE;
    DWRITE_FONT_FILE_TYPE file_type;
    DWRITE_FONT_FACE_TYPE face_type;
    UINT32 faces = 0;
    dw_state* st = (dw_state*)*slot;
    if (*slot == FA_TEXT_STATE_FAILED) return NULL;
    if (st) return st;
    fa_font_source(font, &data, &size, &index);
    if (size > 0xFFFFFFFFu) {
        *slot = FA_TEXT_STATE_FAILED;
        return NULL;
    }
    st = (dw_state*)calloc(1, sizeof(*st));
    if (!st) return NULL;
    /* No owner: the loader keeps its own copy of the bytes, so nothing DirectWrite
     * caches can outlive them. */
    if (FAILED(g_factory->CreateInMemoryFontFileLoader(&st->loader)) || !st->loader ||
        FAILED(g_factory->RegisterFontFileLoader(st->loader)) || !(st->registered = true) ||
        FAILED(st->loader->CreateInMemoryFontFileReference(g_factory, data, (UINT32)size, NULL, &st->file)) ||
        FAILED(st->file->Analyze(&supported, &file_type, &face_type, &faces)) || !supported || index >= faces ||
        FAILED(g_factory->CreateFontFace(face_type, 1, &st->file, index, DWRITE_FONT_SIMULATIONS_NONE, &st->face)) ||
        FAILED(g_factory->CreateTextAnalyzer(&st->analyzer))) {
        /* Not tried again: each try would copy the whole font once more. */
        state_free(st);
        *slot = FA_TEXT_STATE_FAILED;
        return NULL;
    }
    *slot = st;
    return st;
}

/* The text for the analyzer: one paragraph, no locale, no number substitution. */
class source final : public IDWriteTextAnalysisSource {
public:
    source(const WCHAR* text, UINT32 len, bool rtl) : text_(text), len_(len), rtl_(rtl) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** obj) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteTextAnalysisSource)) {
            *obj = this;
            return S_OK;
        }
        *obj = NULL;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }    /* lives on the stack */
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32 pos, WCHAR const** text, UINT32* len) override
    {
        *text = pos < len_ ? text_ + pos : NULL;
        *len = pos < len_ ? len_ - pos : 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTextBeforePosition(UINT32 pos, WCHAR const** text, UINT32* len) override
    {
        *text = pos && pos <= len_ ? text_ : NULL;
        *len = pos && pos <= len_ ? pos : 0;
        return S_OK;
    }
    DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() override
    {
        return rtl_ ? DWRITE_READING_DIRECTION_RIGHT_TO_LEFT : DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
    }
    HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32 pos, UINT32* len, WCHAR const** locale) override
    {
        *locale = L"";
        *len = pos < len_ ? len_ - pos : 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetNumberSubstitution(UINT32 pos, UINT32* len, IDWriteNumberSubstitution** ns) override
    {
        *ns = NULL;
        *len = pos < len_ ? len_ - pos : 0;
        return S_OK;
    }

private:
    const WCHAR* text_;
    UINT32 len_;
    bool rtl_;
};

/* Each UTF-16 unit's script and bidi level. */
class sink final : public IDWriteTextAnalysisSink {
public:
    sink(DWRITE_SCRIPT_ANALYSIS* script, UINT8* level, UINT32 len) : script_(script), level_(level), len_(len) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** obj) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteTextAnalysisSink)) {
            *obj = this;
            return S_OK;
        }
        *obj = NULL;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE SetScriptAnalysis(UINT32 pos, UINT32 n, DWRITE_SCRIPT_ANALYSIS const* sa) override
    {
        for (UINT32 i = pos; i < pos + n && i < len_; i++) script_[i] = *sa;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetLineBreakpoints(UINT32, UINT32, DWRITE_LINE_BREAKPOINT const*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetBidiLevel(UINT32 pos, UINT32 n, UINT8, UINT8 resolved) override
    {
        for (UINT32 i = pos; i < pos + n && i < len_; i++) level_[i] = resolved;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetNumberSubstitution(UINT32, UINT32, IDWriteNumberSubstitution*) override { return S_OK; }

private:
    DWRITE_SCRIPT_ANALYSIS* script_;
    UINT8* level_;
    UINT32 len_;
};

/* Shapes units [a, b) as one run and appends its glyphs, left to right. */
fa_result shape_run(dw_state* st, const WCHAR* text, const uint32_t* byte_of, UINT32 a, UINT32 b, bool rtl,
                    const DWRITE_SCRIPT_ANALYSIS* sa, FLOAT em, float* pen, fa_text_buf* out)
{
    UINT32 n = b - a, cap = n * 3 / 2 + 16, count = 0, i;
    UINT16* cmap = (UINT16*)malloc(n * sizeof(UINT16));
    DWRITE_SHAPING_TEXT_PROPERTIES* tp = (DWRITE_SHAPING_TEXT_PROPERTIES*)malloc(n * sizeof(*tp));
    UINT16* gi = NULL;
    DWRITE_SHAPING_GLYPH_PROPERTIES* gp = NULL;
    FLOAT* adv = NULL;
    DWRITE_GLYPH_OFFSET* off = NULL;
    uint32_t* cl = NULL;
    fa_result r = FA_ERROR_OUT_OF_MEMORY;
    HRESULT hr;
    if (!cmap || !tp) goto done;
    for (;;) {
        free(gi);
        free(gp);
        gi = (UINT16*)malloc(cap * sizeof(UINT16));
        gp = (DWRITE_SHAPING_GLYPH_PROPERTIES*)malloc(cap * sizeof(*gp));
        if (!gi || !gp) goto done;
        hr = st->analyzer->GetGlyphs(text + a, n, st->face, FALSE, rtl, sa, NULL, NULL, NULL, NULL, 0, cap, cmap, tp, gi,
                                     gp, &count);
        if (hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) && cap < 0x1000000) {
            cap *= 2;
            continue;
        }
        break;
    }
    if (FAILED(hr)) {
        r = FA_ERROR_NOT_FOUND;
        goto done;
    }
    adv = (FLOAT*)malloc((count ? count : 1) * sizeof(FLOAT));
    off = (DWRITE_GLYPH_OFFSET*)malloc((count ? count : 1) * sizeof(*off));
    cl = (uint32_t*)malloc((count ? count : 1) * sizeof(uint32_t));
    if (!adv || !off || !cl) goto done;
    if (FAILED(st->analyzer->GetGlyphPlacements(text + a, cmap, tp, n, gi, gp, count, st->face, em, FALSE, rtl, sa, NULL,
                                                NULL, NULL, 0, adv, off))) {
        r = FA_ERROR_NOT_FOUND;
        goto done;
    }
    /* Each glyph's cluster: the first character mapped to its cluster's first glyph
     * (the cluster map runs in logical order, also right to left). */
    for (i = 0; i < count; i++) cl[i] = byte_of[a];
    for (i = 0; i < n; i++) {
        UINT32 g0 = cmap[i], g1, k;
        if (i && cmap[i] == cmap[i - 1]) continue;
        for (k = i + 1; k < n && cmap[k] == cmap[i]; k++) {}
        g1 = k < n ? cmap[k] : count;
        for (k = g0; k < g1 && k < count; k++) cl[k] = byte_of[a + i];
    }
    for (i = 0; i < count; i++) {
        UINT32 g = rtl ? count - 1 - i : i;
        float dx = rtl ? -off[g].advanceOffset : off[g].advanceOffset;
        if (!fa_text_push(out, gi[g], cl[g], *pen, adv[g], dx, off[g].ascenderOffset, rtl)) goto done;
        *pen += adv[g];
    }
    r = FA_OK;
done:
    free(cmap);
    free(tp);
    free(gi);
    free(gp);
    free(adv);
    free(off);
    free(cl);
    return r;
}

} // namespace

extern "C" const char* fa_text_os_name(void) { return loaded() ? "DirectWrite" : NULL; }

extern "C" void fa_text_os_release(void* state) { state_free((dw_state*)state); }

extern "C" fa_result fa_text_os(fa_font* font, void** slot, const char* text, uint32_t len, fa_text_buf* out)
{
    dw_state* st;
    uint16_t* u16;
    uint32_t n = 0, *byte_of = NULL, nr = 0, i, r0;
    DWRITE_SCRIPT_ANALYSIS* script = NULL;
    UINT8* level = NULL;
    UINT8* rl = NULL;
    uint32_t *rs = NULL, *order = NULL;
    fa_result r = FA_ERROR_OUT_OF_MEMORY;
    float pen = 0.0f;
    bool rtl_par;
    if (!loaded()) return FA_ERROR_NOT_FOUND;
    st = state_for(font, slot);
    if (!st) return FA_ERROR_NOT_FOUND;
    u16 = fa_text_utf16(text, len, &n, &byte_of);
    if (!u16) return FA_ERROR_OUT_OF_MEMORY;
    rtl_par = fa_text_rtl_paragraph(text, len) != 0;
    script = (DWRITE_SCRIPT_ANALYSIS*)calloc(n + 1, sizeof(*script));
    level = (UINT8*)malloc(n + 1);
    rs = (uint32_t*)malloc((n + 1) * sizeof(uint32_t));
    rl = (UINT8*)malloc(n + 1);
    order = (uint32_t*)malloc((n + 1) * sizeof(uint32_t));
    if (!script || !level || !rs || !rl || !order) goto done;
    memset(level, rtl_par ? 1 : 0, n + 1);
    {
        source src((const WCHAR*)u16, n, rtl_par);
        sink snk(script, level, n);
        if (FAILED(st->analyzer->AnalyzeScript(&src, 0, n, &snk)) || FAILED(st->analyzer->AnalyzeBidi(&src, 0, n, &snk))) {
            r = FA_ERROR_NOT_FOUND;
            goto done;
        }
    }
    /* Runs of one level and one script. */
    for (i = 0; i < n; i++)
        if (!i || level[i] != level[i - 1] || script[i].script != script[i - 1].script ||
            script[i].shapes != script[i - 1].shapes) {
            rs[nr] = i;
            rl[nr++] = level[i];
        }
    rs[nr] = n;
    fa_text_visual_order(rl, nr, order);
    for (r0 = 0; r0 < nr; r0++) {
        uint32_t k = order[r0];
        fa_result rr = shape_run(st, (const WCHAR*)u16, byte_of, rs[k], rs[k + 1], (rl[k] & 1) != 0, &script[rs[k]],
                                 (FLOAT)fa_font_get_info(font)->units_per_em, &pen, out);
        if (rr != FA_OK) {
            r = rr;
            goto done;
        }
    }
    r = FA_OK;
done:
    free(u16);
    free(byte_of);
    free(script);
    free(level);
    free(rs);
    free(rl);
    free(order);
    return r;
}

namespace {

/* The text for the font fallback. */
UINT16 weight_of(int w) { return (UINT16)(w < 1 ? 400 : w > 999 ? 999 : w); }

bool utf8_of(const WCHAR* w, char* out, size_t cap)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL);
    return n > 0;
}

} // namespace

extern "C" int fa_text_os_fallback(const char* text, uint32_t len, int weight, int italic, char* family,
                                    size_t family_cap, char* path, size_t path_cap, uint32_t* index)
{
    IDWriteFontFallback* fb = NULL;
    IDWriteFont* font = NULL;
    IDWriteFontFamily* fam = NULL;
    IDWriteLocalizedStrings* names = NULL;
    uint16_t* u16;
    uint32_t n = 0, *byte_of = NULL;
    UINT32 mapped = 0, idx = 0, wlen = 0;
    FLOAT scale = 1.0f;
    BOOL exists = FALSE;
    int ok = 0;
    (void)path;
    (void)path_cap;
    (void)index;
    if (!loaded() || !family_cap) return 0;
    u16 = fa_text_utf16(text, len, &n, &byte_of);
    if (!u16) return 0;
    {
        source src((const WCHAR*)u16, n, fa_text_rtl_paragraph(text, len) != 0);
        /* The system's fallback for the first characters the base font (Segoe UI) lacks. */
        if (SUCCEEDED(g_factory->GetSystemFontFallback(&fb)) && fb &&
            SUCCEEDED(fb->MapCharacters(&src, 0, n, NULL, L"Segoe UI", (DWRITE_FONT_WEIGHT)weight_of(weight),
                                        italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, &mapped, &font, &scale)) &&
            font && SUCCEEDED(font->GetFontFamily(&fam)) && SUCCEEDED(fam->GetFamilyNames(&names))) {
            WCHAR w[256];
            if (FAILED(names->FindLocaleName(L"en-us", &idx, &exists)) || !exists) idx = 0;
            if (SUCCEEDED(names->GetStringLength(idx, &wlen)) && wlen < 256 && SUCCEEDED(names->GetString(idx, w, 256)))
                ok = utf8_of(w, family, family_cap);
        }
    }
    if (names) names->Release();
    if (fam) fam->Release();
    if (font) font->Release();
    if (fb) fb->Release();
    free(u16);
    free(byte_of);
    return ok;
}
