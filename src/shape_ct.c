/* The OS shaper on macOS and iOS (shape.h): a Core Text line, which applies the
 * Unicode bidi algorithm and the font's OpenType (and AAT) tables. The font is
 * handed over from memory (macOS 10.13, iOS 11). Where Core Text falls back to
 * another font for a character our font lacks, that glyph is our .notdef, as in
 * our own layout. */
#include "shape.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreText/CoreText.h>
#include <stdlib.h>

const char* fa_text_os_name(void) { return "Core Text"; }

typedef struct ct_state {
    CTFontRef font;
    CFDictionaryRef attrs;
} ct_state;

void fa_text_os_release(void* state)
{
    ct_state* st = (ct_state*)state;
    if (!st) return;
    if (st->attrs) CFRelease(st->attrs);
    if (st->font) CFRelease(st->font);
    free(st);
}

static ct_state* state_for(fa_font* font, void** slot)
{
    const uint8_t* data;
    size_t size;
    uint32_t index;
    CFDataRef bytes;
    CFArrayRef descs;
    ct_state* st = (ct_state*)*slot;
    if (st == FA_TEXT_STATE_FAILED) return NULL;
    if (st) return st;
    fa_font_source(font, &data, &size, &index);
    if (size > 0x7FFFFFFF) {
        *slot = FA_TEXT_STATE_FAILED;
        return NULL;
    }
    st = (ct_state*)calloc(1, sizeof(*st));
    if (!st) return NULL;
    /* A copy: Core Text may keep the font after we close ours. */
    bytes = CFDataCreate(NULL, data, (CFIndex)size);
    descs = bytes ? CTFontManagerCreateFontDescriptorsFromData(bytes) : NULL;
    if (descs && (CFIndex)index < CFArrayGetCount(descs)) {
        CTFontDescriptorRef d = (CTFontDescriptorRef)CFArrayGetValueAtIndex(descs, (CFIndex)index);
        st->font = CTFontCreateWithFontDescriptor(d, (CGFloat)fa_font_get_info(font)->units_per_em, NULL);
    }
    if (descs) CFRelease(descs);
    if (bytes) CFRelease(bytes);
    if (st->font) {
        const void* keys[1] = { kCTFontAttributeName };
        const void* vals[1] = { st->font };
        st->attrs = CFDictionaryCreate(NULL, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    }
    if (!st->attrs) {
        fa_text_os_release(st);
        *slot = FA_TEXT_STATE_FAILED;
        return NULL;
    }
    *slot = st;
    return st;
}

fa_result fa_text_os(fa_font* font, void** slot, const char* text, uint32_t len, fa_text_buf* out)
{
    ct_state* st = state_for(font, slot);
    uint16_t* u16;
    uint32_t n = 0, *byte_of = NULL;
    CFStringRef str = NULL;
    CFAttributedStringRef as = NULL;
    CTLineRef line = NULL;
    CFArrayRef runs;
    CFIndex r, nr;
    CGGlyph* glyphs = NULL;
    CGPoint* pos = NULL;
    CGSize* adv = NULL;
    CFIndex* idx = NULL;
    CFIndex cap = 0;
    fa_result res = FA_ERROR_OUT_OF_MEMORY;
    if (!st) return FA_ERROR_NOT_FOUND;
    u16 = fa_text_utf16(text, len, &n, &byte_of);
    if (!u16) return FA_ERROR_OUT_OF_MEMORY;
    str = CFStringCreateWithCharacters(NULL, (const UniChar*)u16, (CFIndex)n);
    as = str ? CFAttributedStringCreate(NULL, str, st->attrs) : NULL;
    line = as ? CTLineCreateWithAttributedString(as) : NULL;
    if (!line) goto done;
    runs = CTLineGetGlyphRuns(line);
    nr = runs ? CFArrayGetCount(runs) : 0;
    /* The runs come left to right; positions are the line's own. */
    for (r = 0; r < nr; r++) {
        CTRunRef run = (CTRunRef)CFArrayGetValueAtIndex(runs, r);
        CFIndex gc = CTRunGetGlyphCount(run), i;
        int rtl = (CTRunGetStatus(run) & kCTRunStatusRightToLeft) != 0;
        CTFontRef rf = (CTFontRef)CFDictionaryGetValue(CTRunGetAttributes(run), kCTFontAttributeName);
        int ours = rf && CFEqual(rf, st->font);
        if (gc <= 0) continue;
        if (gc > cap) {
            free(glyphs);
            free(pos);
            free(adv);
            free(idx);
            cap = gc;
            glyphs = (CGGlyph*)malloc((size_t)cap * sizeof(*glyphs));
            pos = (CGPoint*)malloc((size_t)cap * sizeof(*pos));
            adv = (CGSize*)malloc((size_t)cap * sizeof(*adv));
            idx = (CFIndex*)malloc((size_t)cap * sizeof(*idx));
            if (!glyphs || !pos || !adv || !idx) goto done;
        }
        CTRunGetGlyphs(run, CFRangeMake(0, 0), glyphs);
        CTRunGetPositions(run, CFRangeMake(0, 0), pos);
        CTRunGetAdvances(run, CFRangeMake(0, 0), adv);
        CTRunGetStringIndices(run, CFRangeMake(0, 0), idx);
        for (i = 0; i < gc; i++) {
            uint32_t at = idx[i] >= 0 && (uint32_t)idx[i] < n ? byte_of[idx[i]] : 0;
            /* 0xFFFF: a glyph Core Text removed (a joiner); another font's glyph: ours lacks it. */
            uint32_t g = !ours ? 0 : glyphs[i] == 0xFFFF ? UINT32_MAX : glyphs[i];
            if (!fa_text_push(out, g, at, (float)pos[i].x, (float)adv[i].width, 0.0f, (float)pos[i].y, rtl)) goto done;
        }
    }
    res = FA_OK;
done:
    free(glyphs);
    free(pos);
    free(adv);
    free(idx);
    if (line) CFRelease(line);
    if (as) CFRelease(as);
    if (str) CFRelease(str);
    free(u16);
    free(byte_of);
    return res;
}
