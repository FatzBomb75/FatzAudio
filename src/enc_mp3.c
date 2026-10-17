/* MP3 output through LAME (libmp3lame), loaded at runtime: never linked or bundled.
 *
 * Nothing here includes a LAME header: functions are looked up by name and only
 * take ints, floats and buffers, which have been stable since LAME 3.98. The tags
 * are an ID3v2.3 header written here (UTF-16, which every player reads); LAME's
 * own tagging is not used. When the output can seek, the LAME/Xing header (exact
 * length, encoder delay and padding for gapless playback, seek table) is written
 * over LAME's placeholder frame at the end. */
#include "encode.h"
#include <stdlib.h>
#include <string.h>

/* ---- the library --------------------------------------------------------------- */

typedef struct lame_api {
    void* (*lame_init)(void);
    int (*lame_close)(void* gfp);
    int (*lame_set_in_samplerate)(void* gfp, int rate);
    int (*lame_set_out_samplerate)(void* gfp, int rate);
    int (*lame_set_num_channels)(void* gfp, int channels);
    int (*lame_set_mode)(void* gfp, int mode);
    int (*lame_set_brate)(void* gfp, int kbps);
    int (*lame_set_VBR)(void* gfp, int vbr);
    int (*lame_set_VBR_q)(void* gfp, int q);
    int (*lame_set_bWriteVbrTag)(void* gfp, int on);
    int (*lame_init_params)(void* gfp);
    /* LAME 3.99+: range +-1.0 */
    int (*lame_encode_buffer_ieee_float)(void* gfp, const float* l, const float* r, int n, unsigned char* out, int size);
    /* LAME 3.98: range +-32768 */
    int (*lame_encode_buffer_float)(void* gfp, const float* l, const float* r, int n, unsigned char* out, int size);
    int (*lame_encode_flush)(void* gfp, unsigned char* out, int size);
    size_t (*lame_get_lametag_frame)(void* gfp, unsigned char* out, size_t size);
    const char* (*get_lame_version)(void);
} lame_api;

#define LAME_VBR_OFF 0
#define LAME_VBR_DEFAULT 4     /* vbr_mtrh */
#define LAME_MODE_JOINT 1
#define LAME_MODE_MONO 3

static lame_api lm;
static fa_lazy_mutex lm_lock;
static int lm_state;           /* 0 = not tried, 1 = loaded, -1 = tried and failed */
static char lm_path[512];

#if defined(_WIN32)
static const char* const lm_names[] = { "libmp3lame.dll", "libmp3lame-0.dll", "lame_enc.dll" };
#elif defined(__APPLE__)
static const char* const lm_names[] = { "libmp3lame.0.dylib", "libmp3lame.dylib", "/opt/homebrew/lib/libmp3lame.0.dylib",
                                        "/usr/local/lib/libmp3lame.0.dylib", "/opt/local/lib/libmp3lame.0.dylib" };
#else
static const char* const lm_names[] = { "libmp3lame.so.0", "libmp3lame.so" };
#endif

static int bind_lame(void* lib)
{
    lame_api a;
    memset(&a, 0, sizeof(a));
#define L(name) (*(void**)&a.name = fa_lib_sym(lib, #name))
    L(lame_init); L(lame_close); L(lame_set_in_samplerate); L(lame_set_out_samplerate); L(lame_set_num_channels);
    L(lame_set_mode); L(lame_set_brate); L(lame_set_VBR); L(lame_set_VBR_q); L(lame_set_bWriteVbrTag);
    L(lame_init_params); L(lame_encode_buffer_ieee_float); L(lame_encode_buffer_float); L(lame_encode_flush);
    L(lame_get_lametag_frame); L(get_lame_version);
#undef L
    if (!a.lame_init || !a.lame_close || !a.lame_set_in_samplerate || !a.lame_set_out_samplerate ||
        !a.lame_set_num_channels || !a.lame_set_mode || !a.lame_set_brate || !a.lame_set_VBR || !a.lame_set_VBR_q ||
        !a.lame_set_bWriteVbrTag || !a.lame_init_params || !a.lame_encode_flush || !a.get_lame_version ||
        (!a.lame_encode_buffer_ieee_float && !a.lame_encode_buffer_float))
        return 0;
    lm = a;
    return 1;
}

/* mode 0: already in the process; 1: the system's search path; 2: `dir`. Caller holds the lock. */
static int try_load(int mode, const char* dir)
{
    size_t i;
    for (i = 0; i < sizeof(lm_names) / sizeof(lm_names[0]); i++) {
        char path[700];
        void* lib;
        const char* name = lm_names[i];
        if (mode == 2) {
            size_t n = strlen(dir);
            const char* base = strrchr(name, '/');
            const char* sep = (n > 0 && (dir[n - 1] == '/' || dir[n - 1] == '\\')) ? "" :
#ifdef _WIN32
                "\\";
#else
                "/";
#endif
            snprintf(path, sizeof(path), "%s%s%s", dir, sep, base ? base + 1 : name);
        } else {
            if (mode == 0 && strchr(name, '/')) continue;
            snprintf(path, sizeof(path), "%s", name);
        }
        lib = fa_lib_open(path, mode == 0);
        if (!lib || !bind_lame(lib)) continue;
        fa_lib_where(lib, "lame_init", lm_path, sizeof(lm_path));
        if (!lm_path[0]) snprintf(lm_path, sizeof(lm_path), "%.511s", path);
        lm_state = 1;
        return 1;
    }
    return 0;
}

int fa_lame_available(void)
{
    int ok;
    fa_lazy_mutex_lock(&lm_lock);
    if (lm_state == 0 && !try_load(0, NULL) && !try_load(1, NULL)) lm_state = -1;
    ok = lm_state == 1;
    fa_lazy_mutex_unlock(&lm_lock);
    return ok;
}

FA_API fa_result FA_CALL fa_lame_load(const char* directory)
{
    fa_result r;
    fa_lazy_mutex_lock(&lm_lock);
    if (lm_state != 1) {
        int ok = directory && directory[0] ? try_load(2, directory) : (try_load(0, NULL) || try_load(1, NULL));
        if (!ok && lm_state == 0 && !(directory && directory[0])) lm_state = -1;
    }
    r = lm_state == 1 ? FA_OK : FA_ERROR_PROVIDER_MISSING;
    fa_lazy_mutex_unlock(&lm_lock);
    return r;
}

FA_API fa_result FA_CALL fa_lame_get_info(fa_lame_info* out)
{
    if (!out) return FA_ERROR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    fa_lazy_mutex_lock(&lm_lock);
    if (lm_state == 1) {
        const char* v = lm.get_lame_version();
        out->loaded = 1;
        snprintf(out->version, sizeof(out->version), "%s", v ? v : "");
        snprintf(out->path, sizeof(out->path), "%s", lm_path);
    }
    fa_lazy_mutex_unlock(&lm_lock);
    return FA_OK;
}

/* ---- ID3v2.3 ------------------------------------------------------------------------ */

/* UTF-8 -> UTF-16LE with a BOM, as ID3v2.3 text (encoding 1). Returns bytes written. */
static size_t utf16_text(const char* s, uint8_t* out, size_t cap)
{
    const uint8_t* p = (const uint8_t*)s;
    size_t n = 0;
    if (cap < 4) return 0;
    out[n++] = 0xFF;
    out[n++] = 0xFE;
    while (*p && n + 4 <= cap) {
        uint32_t c = *p++;
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (c >= 0x80 && c < 0xC0) { c = 0xFFFD; extra = 0; }
        else if (extra) c &= 0x3Fu >> extra;
        while (extra-- > 0) {
            if ((*p & 0xC0) != 0x80) { c = 0xFFFD; break; }
            c = (c << 6) | (*p++ & 0x3F);
        }
        if (c >= 0x10000) {
            c -= 0x10000;
            out[n++] = (uint8_t)(0xD800 | (c >> 10)); out[n++] = (uint8_t)((0xD800 | (c >> 10)) >> 8);
            out[n++] = (uint8_t)(0xDC00 | (c & 0x3FF)); out[n++] = (uint8_t)((0xDC00 | (c & 0x3FF)) >> 8);
        } else {
            out[n++] = (uint8_t)c;
            out[n++] = (uint8_t)(c >> 8);
        }
    }
    return n;
}

/* Appends one text frame to `tag` (size `*len`, capacity `cap`). COMM gets a language and an empty description. */
static void id3_frame(uint8_t* tag, size_t* len, size_t cap, const char id[4], const char* text)
{
    uint8_t body[2048];
    size_t n = 0, need;
    if (!text) return;
    body[n++] = 1;   /* UTF-16 with BOM */
    if (memcmp(id, "COMM", 4) == 0) {
        memcpy(body + n, "eng", 3); n += 3;
        body[n++] = 0xFF; body[n++] = 0xFE; body[n++] = 0; body[n++] = 0;   /* empty description */
    }
    n += utf16_text(text, body + n, sizeof(body) - n);
    need = 10 + n;
    if (*len + need > cap) return;
    memcpy(tag + *len, id, 4);
    tag[*len + 4] = (uint8_t)(n >> 24); tag[*len + 5] = (uint8_t)(n >> 16);
    tag[*len + 6] = (uint8_t)(n >> 8); tag[*len + 7] = (uint8_t)n;
    tag[*len + 8] = 0; tag[*len + 9] = 0;
    memcpy(tag + *len + 10, body, n);
    *len += need;
}

static fa_result write_id3(fa_enc_sink* s, const fa_enc_tags* t)
{
    uint8_t tag[16384];
    size_t len = 10;
    char track[16];
    uint32_t size;
    if (t->track) snprintf(track, sizeof(track), "%u", t->track);
    id3_frame(tag, &len, sizeof(tag), "TIT2", t->title);
    id3_frame(tag, &len, sizeof(tag), "TPE1", t->artist);
    id3_frame(tag, &len, sizeof(tag), "TALB", t->album);
    id3_frame(tag, &len, sizeof(tag), "TCON", t->genre);
    id3_frame(tag, &len, sizeof(tag), "TYER", t->date);
    id3_frame(tag, &len, sizeof(tag), "COMM", t->comment);
    id3_frame(tag, &len, sizeof(tag), "TRCK", t->track ? track : NULL);
    if (len == 10) return FA_OK;
    size = (uint32_t)(len - 10);
    memcpy(tag, "ID3", 3);
    tag[3] = 3; tag[4] = 0; tag[5] = 0;
    tag[6] = (uint8_t)((size >> 21) & 0x7F); tag[7] = (uint8_t)((size >> 14) & 0x7F);
    tag[8] = (uint8_t)((size >> 7) & 0x7F); tag[9] = (uint8_t)(size & 0x7F);
    return fa_sink_write(s, tag, len);
}

/* ---- the format ---------------------------------------------------------------------- */

#define MP3_BLOCK 4096
#define MP3_OUT_CAP (MP3_BLOCK * 5 / 4 + 7200)

typedef struct mp3_state {
    void* gfp;
    uint32_t channels;
    int64_t audio_start;         /* where LAME's first frame (the Xing placeholder) went */
    int tag_frame;               /* LAME writes a placeholder Xing frame */
    float* left;
    float* right;
    unsigned char* out;
} mp3_state;

static const uint32_t mp3_rates[9] = { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000 };

static int mp3_rate_ok(uint32_t r)
{
    int i;
    for (i = 0; i < 9; i++)
        if (mp3_rates[i] == r) return 1;
    return 0;
}

static fa_result mp3_check(const fa_enc_format* f)
{
    if (f->rate && !mp3_rate_ok(f->rate)) return FA_ERROR_INVALID_ARG;
    if (f->channels > 2) return FA_ERROR_INVALID_ARG;
    if (f->bitrate_kbps && (f->bitrate_kbps < 8 || f->bitrate_kbps > 320)) return FA_ERROR_INVALID_ARG;
    if (f->quality > 9) return FA_ERROR_INVALID_ARG;
    if (!fa_lame_available()) return FA_ERROR_PROVIDER_MISSING;
    return FA_OK;
}

static void mp3_choose(fa_enc_format* f)
{
    if (!mp3_rate_ok(f->rate)) {
        /* 88.2/176.4 kHz and other 44.1 kHz multiples -> 44.1 kHz, everything else -> 48 kHz */
        f->rate = (f->rate % 44100 == 0 || f->rate % 11025 == 0) ? 44100 : 48000;
    }
    if (f->channels > 2) f->channels = 2;
    f->bits = 0;
}

static fa_result mp3_begin(void** impl, const fa_enc_format* f, const fa_enc_tags* t, fa_enc_sink* s)
{
    mp3_state* m = (mp3_state*)calloc(1, sizeof(*m));
    fa_result r;
    if (!m) return FA_ERROR_OUT_OF_MEMORY;
    *impl = m;
    m->channels = f->channels;
    m->left = (float*)malloc(MP3_BLOCK * sizeof(float));
    m->right = (float*)malloc(MP3_BLOCK * sizeof(float));
    m->out = (unsigned char*)malloc(MP3_OUT_CAP);
    if (!m->left || !m->right || !m->out) return FA_ERROR_OUT_OF_MEMORY;
    m->gfp = lm.lame_init();
    if (!m->gfp) return FA_ERROR_OUT_OF_MEMORY;
    m->tag_frame = s->seekable && lm.lame_get_lametag_frame != NULL;
    lm.lame_set_in_samplerate(m->gfp, (int)f->rate);
    lm.lame_set_out_samplerate(m->gfp, (int)f->rate);
    lm.lame_set_num_channels(m->gfp, (int)f->channels);
    lm.lame_set_mode(m->gfp, f->channels == 1 ? LAME_MODE_MONO : LAME_MODE_JOINT);
    if (f->bitrate_kbps) {
        lm.lame_set_VBR(m->gfp, LAME_VBR_OFF);
        lm.lame_set_brate(m->gfp, (int)f->bitrate_kbps);
    } else {
        lm.lame_set_VBR(m->gfp, LAME_VBR_DEFAULT);
        lm.lame_set_VBR_q(m->gfp, f->quality < 0 ? 2 : f->quality);
    }
    lm.lame_set_bWriteVbrTag(m->gfp, m->tag_frame);
    if (lm.lame_init_params(m->gfp) < 0) return FA_ERROR_UNSUPPORTED_FORMAT;
    r = write_id3(s, t);
    m->audio_start = s->pos;
    return r;
}

static fa_result mp3_write(void* impl, const float* frames, int64_t count, fa_enc_sink* s)
{
    mp3_state* m = (mp3_state*)impl;
    while (count > 0) {
        int n = count < MP3_BLOCK ? (int)count : MP3_BLOCK, i, got;
        float scale = lm.lame_encode_buffer_ieee_float ? 1.0f : 32768.0f;
        if (m->channels == 2) {
            for (i = 0; i < n; i++) { m->left[i] = frames[2 * i] * scale; m->right[i] = frames[2 * i + 1] * scale; }
        } else {
            for (i = 0; i < n; i++) m->left[i] = frames[i] * scale;
        }
        got = lm.lame_encode_buffer_ieee_float
            ? lm.lame_encode_buffer_ieee_float(m->gfp, m->left, m->channels == 2 ? m->right : NULL, n, m->out, MP3_OUT_CAP)
            : lm.lame_encode_buffer_float(m->gfp, m->left, m->channels == 2 ? m->right : m->left, n, m->out, MP3_OUT_CAP);
        if (got < 0) return FA_ERROR_GENERIC;
        if (got > 0) {
            fa_result r = fa_sink_write(s, m->out, (size_t)got);
            if (r != FA_OK) return r;
        }
        frames += (size_t)n * m->channels;
        count -= n;
    }
    return FA_OK;
}

static fa_result mp3_finish(void* impl, fa_enc_sink* s)
{
    mp3_state* m = (mp3_state*)impl;
    int got = lm.lame_encode_flush(m->gfp, m->out, MP3_OUT_CAP);
    fa_result r = FA_OK;
    if (got < 0) return FA_ERROR_GENERIC;
    if (got > 0) r = fa_sink_write(s, m->out, (size_t)got);
    if (r == FA_OK && m->tag_frame) {
        size_t n = lm.lame_get_lametag_frame(m->gfp, m->out, MP3_OUT_CAP);
        if (n > 0 && n <= MP3_OUT_CAP) {
            int64_t end = s->pos;
            r = fa_sink_seek(s, m->audio_start);
            if (r == FA_OK) r = fa_sink_write(s, m->out, n);
            if (r == FA_OK) r = fa_sink_seek(s, end);
        }
    }
    return r;
}

static void mp3_free(void* impl)
{
    mp3_state* m = (mp3_state*)impl;
    if (m->gfp) lm.lame_close(m->gfp);
    free(m->left);
    free(m->right);
    free(m->out);
    free(m);
}

const fa_enc_backend fa_enc_mp3 = { mp3_check, mp3_choose, mp3_begin, mp3_write, mp3_finish, mp3_free };
