/* The OS shaper on Android (shape.h): the platform's own text stack through the
 * app's Java VM, nothing bundled. java.text.Bidi splits the line into runs of one
 * direction; android.graphics.text.TextRunShaper (Android 12, API 31) shapes each
 * run with our font (a Typeface made from its bytes); Paint.getRunAdvance gives each
 * character's edges (the shaper does not report clusters). Before Android 12, or in
 * a process without a VM (a command-line program), lyrics use our own layout. */
#if defined(__ANDROID__)
#include "shape.h"
#include "android_jni.h"
#include "fa_platform.h"

#include <stdlib.h>
#include <string.h>

#define BIDI_DEFAULT_LTR (-2)      /* java.text.Bidi.DIRECTION_DEFAULT_LEFT_TO_RIGHT */

static struct {
    int tried, ok;
    jclass byte_buffer, font_builder, family_builder, fallback_builder, paint, typeface, shaper, glyphs, bidi, file, object;
    jmethodID allocate_direct;
    jmethodID font_builder_init, font_builder_ttc, font_builder_build;
    jmethodID family_builder_init, family_builder_build;
    jmethodID fallback_init, fallback_build;
    jmethodID paint_init, paint_typeface, paint_size, paint_linear, paint_subpixel, paint_run_advance;
    jmethodID typeface_create;
    jfieldID typeface_default;
    jmethodID shape_run;
    jmethodID glyph_count, glyph_id, glyph_x, glyph_y, glyph_font;
    jmethodID font_file, font_ttc, file_path, equals;
    jmethodID bidi_init, bidi_count, bidi_start, bidi_limit, bidi_level;
} J;
static fa_lazy_mutex g_lock;

static int clear(JNIEnv* env)
{
    if (!(*env)->ExceptionCheck(env)) return 0;
    (*env)->ExceptionClear(env);
    return 1;
}

static jclass global_class(JNIEnv* env, const char* name)
{
    jclass local = (*env)->FindClass(env, name), g;
    if (!local) {
        clear(env);
        return NULL;
    }
    g = (jclass)(*env)->NewGlobalRef(env, local);
    (*env)->DeleteLocalRef(env, local);
    return g;
}

static int load(JNIEnv* env)
{
    int ok = 1;
    fa_lazy_mutex_lock(&g_lock);
    if (J.tried) {
        ok = J.ok;
        fa_lazy_mutex_unlock(&g_lock);
        return ok;
    }
    J.tried = 1;
    /* TextRunShaper first: it is what needs Android 12. */
    J.shaper = global_class(env, "android/graphics/text/TextRunShaper");
    J.glyphs = global_class(env, "android/graphics/text/PositionedGlyphs");
    J.byte_buffer = global_class(env, "java/nio/ByteBuffer");
    J.font_builder = global_class(env, "android/graphics/fonts/Font$Builder");
    J.family_builder = global_class(env, "android/graphics/fonts/FontFamily$Builder");
    J.fallback_builder = global_class(env, "android/graphics/Typeface$CustomFallbackBuilder");
    J.paint = global_class(env, "android/graphics/Paint");
    J.typeface = global_class(env, "android/graphics/Typeface");
    J.bidi = global_class(env, "java/text/Bidi");
    J.file = global_class(env, "java/io/File");
    J.object = global_class(env, "java/lang/Object");
    ok = J.shaper && J.glyphs && J.byte_buffer && J.font_builder && J.family_builder && J.fallback_builder && J.paint &&
         J.typeface && J.bidi && J.file && J.object;
    /* Each lookup only while none failed: no JNI call may run with an exception pending. */
#define M(dst, cls, name, sig) do { if (ok && !(dst = (*env)->GetMethodID(env, cls, name, sig))) ok = 0; } while (0)
#define SM(dst, cls, name, sig) do { if (ok && !(dst = (*env)->GetStaticMethodID(env, cls, name, sig))) ok = 0; } while (0)
    SM(J.allocate_direct, J.byte_buffer, "allocateDirect", "(I)Ljava/nio/ByteBuffer;");
    M(J.font_builder_init, J.font_builder, "<init>", "(Ljava/nio/ByteBuffer;)V");
    M(J.font_builder_ttc, J.font_builder, "setTtcIndex", "(I)Landroid/graphics/fonts/Font$Builder;");
    M(J.font_builder_build, J.font_builder, "build", "()Landroid/graphics/fonts/Font;");
    M(J.family_builder_init, J.family_builder, "<init>", "(Landroid/graphics/fonts/Font;)V");
    M(J.family_builder_build, J.family_builder, "build", "()Landroid/graphics/fonts/FontFamily;");
    M(J.fallback_init, J.fallback_builder, "<init>", "(Landroid/graphics/fonts/FontFamily;)V");
    M(J.fallback_build, J.fallback_builder, "build", "()Landroid/graphics/Typeface;");
    M(J.paint_init, J.paint, "<init>", "()V");
    M(J.paint_typeface, J.paint, "setTypeface", "(Landroid/graphics/Typeface;)Landroid/graphics/Typeface;");
    M(J.paint_size, J.paint, "setTextSize", "(F)V");
    M(J.paint_linear, J.paint, "setLinearText", "(Z)V");
    M(J.paint_subpixel, J.paint, "setSubpixelText", "(Z)V");
    M(J.paint_run_advance, J.paint, "getRunAdvance", "([CIIIIZI)F");
    SM(J.typeface_create, J.typeface, "create", "(Landroid/graphics/Typeface;IZ)Landroid/graphics/Typeface;");
    if (ok && !(J.typeface_default = (*env)->GetStaticFieldID(env, J.typeface, "DEFAULT", "Landroid/graphics/Typeface;"))) ok = 0;
    SM(J.shape_run, J.shaper, "shapeTextRun", "([CIIIIFFZLandroid/graphics/Paint;)Landroid/graphics/text/PositionedGlyphs;");
    M(J.glyph_count, J.glyphs, "glyphCount", "()I");
    M(J.glyph_id, J.glyphs, "getGlyphId", "(I)I");
    M(J.glyph_x, J.glyphs, "getGlyphX", "(I)F");
    M(J.glyph_y, J.glyphs, "getGlyphY", "(I)F");
    M(J.glyph_font, J.glyphs, "getFont", "(I)Landroid/graphics/fonts/Font;");
    {
        jclass font = ok ? (*env)->FindClass(env, "android/graphics/fonts/Font") : NULL;
        if (!font) ok = 0;
        M(J.font_file, font, "getFile", "()Ljava/io/File;");
        M(J.font_ttc, font, "getTtcIndex", "()I");
        if (font) (*env)->DeleteLocalRef(env, font);
    }
    M(J.file_path, J.file, "getAbsolutePath", "()Ljava/lang/String;");
    M(J.equals, J.object, "equals", "(Ljava/lang/Object;)Z");
    M(J.bidi_init, J.bidi, "<init>", "(Ljava/lang/String;I)V");
    M(J.bidi_count, J.bidi, "getRunCount", "()I");
    M(J.bidi_start, J.bidi, "getRunStart", "(I)I");
    M(J.bidi_limit, J.bidi, "getRunLimit", "(I)I");
    M(J.bidi_level, J.bidi, "getRunLevel", "(I)I");
#undef M
#undef SM
    clear(env);
    J.ok = ok;
    fa_lazy_mutex_unlock(&g_lock);
    return ok;
}

const char* fa_text_os_name(void)
{
    JNIEnv* env = fa_android_env();
    return env && load(env) ? "TextRunShaper" : NULL;
}

typedef struct an_state {
    jobject font;                 /* android.graphics.fonts.Font of our bytes */
    jobject paint;                /* a Paint with a Typeface of only that font, at units_per_em */
} an_state;

void fa_text_os_release(void* state)
{
    an_state* st = (an_state*)state;
    JNIEnv* env;
    if (!st) return;
    env = fa_android_env();
    if (env) {
        if (st->font) (*env)->DeleteGlobalRef(env, st->font);
        if (st->paint) (*env)->DeleteGlobalRef(env, st->paint);
    }
    free(st);
}

/* A Paint for `tf` at `size`, without hinting's rounding. NULL on failure. */
static jobject new_paint(JNIEnv* env, jobject tf, float size)
{
    jobject p = (*env)->NewObject(env, J.paint, J.paint_init);
    if (!p || clear(env)) return NULL;
    /* No JNI call may run with an exception pending. */
    (*env)->CallObjectMethod(env, p, J.paint_typeface, tf);
    if (clear(env)) return NULL;
    (*env)->CallVoidMethod(env, p, J.paint_size, (jfloat)size);
    if (clear(env)) return NULL;
    (*env)->CallVoidMethod(env, p, J.paint_linear, JNI_TRUE);
    if (clear(env)) return NULL;
    (*env)->CallVoidMethod(env, p, J.paint_subpixel, JNI_TRUE);
    return clear(env) ? NULL : p;
}

static an_state* state_for(JNIEnv* env, fa_font* font, void** slot)
{
    const uint8_t* data;
    size_t size;
    uint32_t index;
    jobject buf, fb, f = NULL, famb, fam, tfb, tf, paint = NULL;
    void* addr;
    an_state* st = (an_state*)*slot;
    if (*slot == FA_TEXT_STATE_FAILED) return NULL;
    if (st) return st;
    fa_font_source(font, &data, &size, &index);
    if (size > 0x7FFFFFFF) {
        *slot = FA_TEXT_STATE_FAILED;
        return NULL;
    }
    /* A copy in a buffer the VM owns: the system may keep the font after ours closes. */
    buf = (*env)->CallStaticObjectMethod(env, J.byte_buffer, J.allocate_direct, (jint)size);
    if (!buf || clear(env) || !(addr = (*env)->GetDirectBufferAddress(env, buf))) return NULL;
    memcpy(addr, data, size);
    fb = (*env)->NewObject(env, J.font_builder, J.font_builder_init, buf);
    if (fb && !clear(env)) {
        (*env)->CallObjectMethod(env, fb, J.font_builder_ttc, (jint)index);
        if (!clear(env)) f = (*env)->CallObjectMethod(env, fb, J.font_builder_build);   /* throws IOException */
        if (clear(env)) f = NULL;
    }
    clear(env);
    if (f) {
        famb = (*env)->NewObject(env, J.family_builder, J.family_builder_init, f);
        fam = famb && !clear(env) ? (*env)->CallObjectMethod(env, famb, J.family_builder_build) : NULL;
        tfb = fam && !clear(env) ? (*env)->NewObject(env, J.fallback_builder, J.fallback_init, fam) : NULL;
        tf = tfb && !clear(env) ? (*env)->CallObjectMethod(env, tfb, J.fallback_build) : NULL;
        if (tf && !clear(env)) paint = new_paint(env, tf, (float)fa_font_get_info(font)->units_per_em);
        clear(env);
    }
    if (!paint) {
        /* The platform could not take the font (Font.Builder threw, say): not tried again. */
        *slot = FA_TEXT_STATE_FAILED;
        return NULL;
    }
    st = (an_state*)calloc(1, sizeof(*st));
    if (!st) return NULL;
    st->font = (*env)->NewGlobalRef(env, f);
    st->paint = (*env)->NewGlobalRef(env, paint);
    if (!st->font || !st->paint) {
        fa_text_os_release(st);
        return NULL;
    }
    *slot = st;
    return st;
}

/* The runs of one direction, in logical order: starts (UTF-16 units) and levels. */
static int bidi_runs(JNIEnv* env, const uint16_t* u16, uint32_t n, uint32_t** starts, uint8_t** levels, uint32_t* count)
{
    jstring s = (*env)->NewString(env, (const jchar*)u16, (jsize)n);
    jobject b = s && !clear(env) ? (*env)->NewObject(env, J.bidi, J.bidi_init, s, (jint)BIDI_DEFAULT_LTR) : NULL;
    jint nr, i;
    if (!b || clear(env)) return 0;
    nr = (*env)->CallIntMethod(env, b, J.bidi_count);
    if (clear(env) || nr < 1) return 0;
    *starts = (uint32_t*)malloc(((size_t)nr + 1) * sizeof(uint32_t));
    *levels = (uint8_t*)malloc((size_t)nr + 1);
    if (!*starts || !*levels) return 0;
    for (i = 0; i < nr; i++) {
        jint a = (*env)->CallIntMethod(env, b, J.bidi_start, i), lv;
        if (clear(env)) return 0;
        lv = (*env)->CallIntMethod(env, b, J.bidi_level, i);
        if (clear(env) || a < 0 || (uint32_t)a > n) return 0;
        (*starts)[i] = (uint32_t)a;
        (*levels)[i] = (uint8_t)(lv & 0x7F);
    }
    (*starts)[nr] = n;
    *count = (uint32_t)nr;
    return 1;
}

fa_result fa_text_os(fa_font* font, void** slot, const char* text, uint32_t len, fa_text_buf* out)
{
    JNIEnv* env = fa_android_env();
    an_state* st;
    uint16_t* u16 = NULL;
    uint32_t n = 0, *byte_of = NULL, *rs = NULL, *order = NULL, nr = 0, r, i;
    uint8_t* rl = NULL;
    jcharArray chars;
    fa_result res = FA_ERROR_NOT_FOUND;
    float pen = 0.0f;
    if (!env || !load(env) || (*env)->PushLocalFrame(env, 64) != JNI_OK) return FA_ERROR_NOT_FOUND;
    st = state_for(env, font, slot);
    if (!st) goto done;
    res = FA_ERROR_OUT_OF_MEMORY;
    u16 = fa_text_utf16(text, len, &n, &byte_of);
    if (!u16 || !fa_text_edges_alloc(out)) goto done;
    res = FA_ERROR_NOT_FOUND;
    chars = (*env)->NewCharArray(env, (jsize)n);
    if (!chars || clear(env)) goto done;
    (*env)->SetCharArrayRegion(env, chars, 0, (jsize)n, (const jchar*)u16);
    if (!bidi_runs(env, u16, n, &rs, &rl, &nr)) goto done;
    order = (uint32_t*)malloc(((size_t)nr + 1) * sizeof(uint32_t));
    if (!order) {
        res = FA_ERROR_OUT_OF_MEMORY;
        goto done;
    }
    fa_text_visual_order(rl, nr, order);
    for (r = 0; r < nr; r++) {
        uint32_t k = order[r], a = rs[k], b = rs[k + 1];
        jboolean rtl = (rl[k] & 1) ? JNI_TRUE : JNI_FALSE;
        jobject pg;
        jint gc, g;
        float w;
        if (a >= b) continue;
        pg = (*env)->CallStaticObjectMethod(env, J.shaper, J.shape_run, chars, (jint)a, (jint)(b - a), (jint)a,
                                            (jint)(b - a), (jfloat)pen, (jfloat)0.0f, rtl, st->paint);
        if (!pg || clear(env)) goto done;
        gc = (*env)->CallIntMethod(env, pg, J.glyph_count);
        if (clear(env)) goto done;
        for (g = 0; g < gc; g++) {
            jint id;
            jfloat x, y;
            jobject gf;
            int ours;
            /* No JNI call may run with an exception pending. */
            id = (*env)->CallIntMethod(env, pg, J.glyph_id, g);
            if (clear(env)) goto done;
            x = (*env)->CallFloatMethod(env, pg, J.glyph_x, g);
            if (clear(env)) goto done;
            y = (*env)->CallFloatMethod(env, pg, J.glyph_y, g);
            if (clear(env)) goto done;
            gf = (*env)->CallObjectMethod(env, pg, J.glyph_font, g);
            if (clear(env)) goto done;
            /* A glyph of a system font: ours lacks the character. */
            ours = gf && ((*env)->IsSameObject(env, gf, st->font) ||
                          (*env)->CallBooleanMethod(env, gf, J.equals, st->font));
            if (clear(env)) ours = 0;
            if (gf) (*env)->DeleteLocalRef(env, gf);
            /* y down on Android. Edges come below, so no cluster or advance. */
            if (!fa_text_push(out, ours ? (uint32_t)id : 0, 0, (float)x, 0.0f, 0.0f, -(float)y, rtl)) {
                res = FA_ERROR_OUT_OF_MEMORY;
                goto done;
            }
        }
        (*env)->DeleteLocalRef(env, pg);
        /* Each character's edges: the advance to its start and to its end, from the
         * run's start (its right end when it reads right to left). */
        w = (*env)->CallFloatMethod(env, st->paint, J.paint_run_advance, chars, (jint)a, (jint)b, (jint)a, (jint)b, rtl,
                                    (jint)b);
        if (clear(env)) goto done;
        for (i = a; i < b;) {
            uint32_t e = i + 1, q;
            float s0, s1;
            if (e < b && u16[i] >= 0xD800 && u16[i] < 0xDC00) e++;   /* a surrogate pair */
            s0 = (*env)->CallFloatMethod(env, st->paint, J.paint_run_advance, chars, (jint)a, (jint)b, (jint)a, (jint)b,
                                         rtl, (jint)i);
            if (clear(env)) goto done;
            s1 = (*env)->CallFloatMethod(env, st->paint, J.paint_run_advance, chars, (jint)a, (jint)b, (jint)a, (jint)b,
                                         rtl, (jint)e);
            if (clear(env)) goto done;
            for (q = byte_of[i]; q < byte_of[e]; q++) {
                out->left[q] = rtl ? pen + w - s1 : pen + s0;
                out->right[q] = rtl ? pen + w - s0 : pen + s1;
                out->rtl[q] = (uint8_t)(rtl == JNI_TRUE);
            }
            i = e;
        }
        pen += w;
    }
    out->edges = 1;
    res = FA_OK;
done:
    clear(env);
    (*env)->PopLocalFrame(env, NULL);
    free(u16);
    free(byte_of);
    free(rs);
    free(rl);
    free(order);
    return res;
}

int fa_text_os_fallback(const char* text, uint32_t len, int weight, int italic, char* family, size_t family_cap,
                         char* path, size_t path_cap, uint32_t* index)
{
    JNIEnv* env = fa_android_env();
    uint16_t* u16;
    uint32_t n = 0, *byte_of = NULL;
    int ok = 0;
    (void)family;
    (void)family_cap;
    if (!env || !path_cap || !load(env)) return 0;
    u16 = fa_text_utf16(text, len, &n, &byte_of);
    if (!u16) return 0;
    if ((*env)->PushLocalFrame(env, 16) == JNI_OK) {
        /* The font the system draws the text with in its default typeface. */
        jobject def = (*env)->GetStaticObjectField(env, J.typeface, J.typeface_default);
        jobject tf = def && !clear(env) ? (*env)->CallStaticObjectMethod(env, J.typeface, J.typeface_create, def,
                                                                          (jint)(weight < 1 ? 400 : weight > 1000 ? 1000 : weight),
                                                                          italic ? JNI_TRUE : JNI_FALSE)
                                        : NULL;
        jobject paint = tf && !clear(env) ? new_paint(env, tf, 64.0f) : NULL;
        jcharArray chars = paint ? (*env)->NewCharArray(env, (jsize)n) : NULL;
        jobject pg = NULL, f = NULL, file = NULL;
        jstring p = NULL;
        if (chars && !clear(env)) {
            (*env)->SetCharArrayRegion(env, chars, 0, (jsize)n, (const jchar*)u16);
            pg = (*env)->CallStaticObjectMethod(env, J.shaper, J.shape_run, chars, (jint)0, (jint)n, (jint)0, (jint)n,
                                                (jfloat)0.0f, (jfloat)0.0f,
                                                fa_text_rtl_paragraph(text, len) ? JNI_TRUE : JNI_FALSE, paint);
        }
        if (pg && !clear(env) && (*env)->CallIntMethod(env, pg, J.glyph_count) > 0 && !clear(env))
            f = (*env)->CallObjectMethod(env, pg, J.glyph_font, (jint)0);
        if (f && !clear(env)) file = (*env)->CallObjectMethod(env, f, J.font_file);
        if (file && !clear(env)) p = (jstring)(*env)->CallObjectMethod(env, file, J.file_path);
        if (p && !clear(env)) {
            const char* s = (*env)->GetStringUTFChars(env, p, NULL);
            if (s) {
                size_t l = strlen(s);
                jint ttc = (*env)->CallIntMethod(env, f, J.font_ttc);
                if (!clear(env) && l < path_cap && ttc >= 0) {
                    memcpy(path, s, l + 1);
                    *index = (uint32_t)ttc;
                    ok = 1;
                }
                (*env)->ReleaseStringUTFChars(env, p, s);
            }
        }
        clear(env);
        (*env)->PopLocalFrame(env, NULL);
    }
    free(u16);
    free(byte_of);
    return ok;
}
#endif
