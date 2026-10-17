/* MIDI: Standard MIDI Files (format 0, 1 and 2; .mid, .midi, karaoke .kar) and
 * RIFF MIDI (.rmi), parsed by TinyMidiLoader and played by TinySoundFont with a
 * SoundFont 2 (both vendored in third_party/tinysoundfont, patched for untrusted
 * files; see the README there). Also the soundfont objects and the MIDI stream
 * API (texts, transpose, channel mute).
 *
 * Rendering is cut into blocks of FA_MIDI_BLOCK frames aligned to the start of
 * the song, and each block is split again only where an event falls. The synth
 * therefore sees the same calls whatever sizes the stream is read in, and a seek
 * renders from the start of the target's block: what comes out after a seek is
 * the same as reading through, wherever no note was held across the target.
 *
 * A seek replays the events before the target into a fresh synth without
 * rendering: programs, controllers, pitch bends and the pedal are restored,
 * notes are not (they would start from their attack, as a new hit).
 *
 * Live controls (transpose, channel mute) are atomics the decode thread reads at
 * the start of each block. */
#include "fa_internal.h"
#include "tsf.h"
#include "tml.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FA_MIDI_BLOCK 64                  /* frames; TinySoundFont's effect block */
#define FA_MIDI_MAX_FILE ((size_t)16 << 20)   /* the largest real MIDI files are a few MB */
#define FA_MIDI_MAX_SOUNDFONT ((size_t)1 << 30)
#define FA_MIDI_MAX_SECONDS (6.0 * 3600.0)  /* events later than this are dropped */
#define FA_MIDI_TAIL_S 1.0                /* after the last event: the last notes ring out */
#define FA_MIDI_VOICES 256                /* polyphony per stream (oldest released voices go first) */
#define FA_MIDI_GAIN_DB 0.0f
#define FA_MIDI_DRUMS 9                   /* channel 10 */
#define FA_MIDI_TRANSPOSE_MAX 48

/* ---- Soundfonts ---------------------------------------------------------------- */

struct fa_soundfont {
    fa_atomic32 refs;
    fa_mutex lock;              /* tsf_copy and tsf_close of copies share a plain counter */
    tsf* bank;                  /* the loaded font; never plays itself, streams play copies */
    uint64_t key;
    uint64_t sample_memory;
    uint32_t presets;
    char name[256];
};

static uint32_t rd32le(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* The bank's name: RIFF sfbk > LIST INFO > INAM. Also the size of the sample data
 * (smpl in LIST sdta), each kept as float. */
static void sf2_scan(const uint8_t* d, size_t n, char* name, size_t cap, uint64_t* sample_memory)
{
    size_t pos = 12;
    name[0] = '\0';
    *sample_memory = 0;
    if (n < 12) return;
    while (pos + 8 <= n) {
        uint32_t size = rd32le(d + pos + 4);
        size_t body = pos + 8, end;
        if (size > n - body) size = (uint32_t)(n - body);
        end = body + size;
        if (memcmp(d + pos, "LIST", 4) == 0 && size >= 4) {
            size_t sub = body + 4;
            int info = memcmp(d + body, "INFO", 4) == 0, sdta = memcmp(d + body, "sdta", 4) == 0;
            while (sub + 8 <= end) {
                uint32_t len = rd32le(d + sub + 4);
                if (len > end - sub - 8) len = (uint32_t)(end - sub - 8);
                if (info && memcmp(d + sub, "INAM", 4) == 0) {
                    size_t k = 0;
                    while (k < len && k + 1 < cap && d[sub + 8 + k]) { name[k] = (char)d[sub + 8 + k]; k++; }
                    name[k] = '\0';
                }
                if (sdta && memcmp(d + sub, "smpl", 4) == 0) *sample_memory = (uint64_t)(len / 2) * sizeof(float);
                sub += 8 + (size_t)len + (len & 1);
            }
        }
        pos = end + (size & 1);
    }
}

/* The name in UTF-8: INAM is meant to be ASCII; anything else is read as Latin-1. */
static void latin1_to_utf8(const char* in, char* out, size_t cap);

static fa_result soundfont_from_memory(const uint8_t* data, size_t size, fa_soundfont** out)
{
    fa_soundfont* f;
    char raw[256];
    uint8_t lenb[8];
    size_t mid;
    int i;
    *out = NULL;
    if (size < 12 || size > FA_MIDI_MAX_SOUNDFONT || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "sfbk", 4) != 0)
        return FA_ERROR_UNSUPPORTED_FORMAT;
    f = (fa_soundfont*)calloc(1, sizeof(*f));
    if (!f) return FA_ERROR_OUT_OF_MEMORY;
    if (fa_mutex_init(&f->lock) != 0) { free(f); return FA_ERROR_GENERIC; }
    f->bank = tsf_load_memory(data, (int)size);
    if (!f->bank || tsf_get_presetcount(f->bank) <= 0) {
        if (f->bank) tsf_close(f->bank);
        fa_mutex_destroy(&f->lock);
        free(f);
        return FA_ERROR_UNSUPPORTED_FORMAT;
    }
    f->presets = (uint32_t)tsf_get_presetcount(f->bank);
    sf2_scan(data, size, raw, sizeof(raw), &f->sample_memory);
    latin1_to_utf8(raw, f->name, sizeof(f->name));
    /* Fingerprint: size, then the first, middle and last 64 KB (like loudness keys). */
    for (i = 0; i < 8; i++) lenb[i] = (uint8_t)((uint64_t)size >> (i * 8));
    f->key = fa_fnv1a(0xcbf29ce484222325ULL, lenb, 8);
    f->key = fa_fnv1a(f->key, data, size < 65536 ? size : 65536);
    mid = size > 65536 ? size / 2 - 32768 : 0;
    f->key = fa_fnv1a(f->key, data + mid, size - mid < 65536 ? size - mid : 65536);
    f->key = fa_fnv1a(f->key, data + (size > 65536 ? size - 65536 : 0), size < 65536 ? size : 65536);
    fa_store32(&f->refs, 1);
    *out = f;
    return FA_OK;
}

FA_API fa_result FA_CALL fa_soundfont_open_memory(const void* data, size_t size, fa_soundfont** out_font)
{
    if (out_font) *out_font = NULL;
    if (!data || size == 0 || !out_font) return FA_ERROR_INVALID_ARG;
    return soundfont_from_memory((const uint8_t*)data, size, out_font);
}

FA_API fa_result FA_CALL fa_soundfont_open_path(const char* path, fa_soundfont** out_font)
{
    fa_source src;
    uint8_t* bytes = NULL;
    size_t size = 0;
    fa_result r;
    if (out_font) *out_font = NULL;
    if (!path || !out_font) return FA_ERROR_INVALID_ARG;
    r = fa_source_open_path(&src, path);
    if (r != FA_OK) return r;
    r = fa_source_read_all(&src, FA_MIDI_MAX_SOUNDFONT, &bytes, &size);
    fa_source_close(&src);
    if (r != FA_OK) return r;
    r = soundfont_from_memory(bytes, size, out_font);
    free(bytes);
    return r;
}

fa_soundfont* fa_soundfont_retain(fa_soundfont* f)
{
    if (f) fa_fetch_add32(&f->refs, 1);
    return f;
}

void fa_soundfont_release(fa_soundfont* f)
{
    if (!f || fa_fetch_add32(&f->refs, -1) != 1) return;
    /* The last reference: every stream's copy has been closed already. */
    tsf_close(f->bank);
    fa_mutex_destroy(&f->lock);
    free(f);
}

FA_API void FA_CALL fa_soundfont_close(fa_soundfont* font)
{
    fa_soundfont_release(font);
}

uint64_t fa_soundfont_key(const fa_soundfont* f)
{
    return f ? f->key : 0;
}

FA_API fa_result FA_CALL fa_soundfont_get_info(fa_soundfont* font, fa_soundfont_info* out_info)
{
    if (!font || !out_info) return FA_ERROR_INVALID_ARG;
    memset(out_info, 0, sizeof(*out_info));
    snprintf(out_info->name, sizeof(out_info->name), "%s", font->name);
    out_info->preset_count = font->presets;
    out_info->sample_memory = font->sample_memory;
    return FA_OK;
}

FA_API fa_result FA_CALL fa_engine_set_soundfont(fa_engine* engine, fa_soundfont* font)
{
    fa_soundfont* old;
    if (!engine) return FA_ERROR_INVALID_ARG;
    fa_soundfont_retain(font);
    fa_mutex_lock(&engine->list_lock);
    old = engine->soundfont;
    engine->soundfont = font;
    fa_mutex_unlock(&engine->list_lock);
    fa_soundfont_release(old);
    return FA_OK;
}

fa_soundfont* fa_engine_soundfont(fa_engine* engine)
{
    fa_soundfont* f;
    fa_mutex_lock(&engine->list_lock);
    f = fa_soundfont_retain(engine->soundfont);
    fa_mutex_unlock(&engine->list_lock);
    return f;
}

uint32_t fa_engine_midi_rate(const fa_engine* engine)
{
    uint32_t r = engine->config.mix_sample_rate;
    return r >= 8000 && r <= 192000 ? r : FA_MIDI_DEFAULT_RATE;
}

/* A synth of its own for a stream, sharing the font's presets and samples. */
static tsf* synth_new(fa_soundfont* f, uint32_t rate)
{
    tsf* t;
    int ch;
    fa_mutex_lock(&f->lock);
    t = tsf_copy(f->bank);
    fa_mutex_unlock(&f->lock);
    if (!t) return NULL;
    tsf_set_output(t, TSF_STEREO_INTERLEAVED, (int)rate, FA_MIDI_GAIN_DB);
    /* Voices are allocated here, never while rendering. */
    if (!tsf_set_max_voices(t, FA_MIDI_VOICES)) goto fail;
    for (ch = 0; ch < 16; ch++) {
        /* Channels start on program 0 (drums: the standard kit) at the General MIDI
         * default volume of 100. */
        if (!tsf_channel_set_presetnumber(t, ch, 0, ch == FA_MIDI_DRUMS) && !tsf_channel_set_presetindex(t, ch, 0)) goto fail;
        if (!tsf_channel_midi_control(t, ch, TML_VOLUME_MSB, 100)) goto fail;
    }
    return t;
fail:
    fa_mutex_lock(&f->lock);
    tsf_close(t);
    fa_mutex_unlock(&f->lock);
    return NULL;
}

static void synth_free(fa_soundfont* f, tsf* t)
{
    if (!t) return;
    fa_mutex_lock(&f->lock);
    tsf_close(t);
    fa_mutex_unlock(&f->lock);
}

/* ---- Text --------------------------------------------------------------------- */

/* Windows-1252 0x80..0x9F (the rest of the high half is Latin-1). */
static const uint16_t cp1252[32] = {
    0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
    0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178
};

static int valid_utf8(const uint8_t* s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        size_t k, len;
        if (c < 0x80) { i++; continue; }
        if (c >= 0xC2 && c <= 0xDF) len = 2;
        else if (c >= 0xE0 && c <= 0xEF) len = 3;
        else if (c >= 0xF0 && c <= 0xF4) len = 4;
        else return 0;
        if (n - i < len) return 0;
        for (k = 1; k < len; k++) if ((s[i + k] & 0xC0) != 0x80) return 0;
        /* No overlong forms, UTF-16 surrogates or code points above U+10FFFF. */
        if ((c == 0xE0 && s[i + 1] < 0xA0) || (c == 0xED && s[i + 1] > 0x9F) ||
            (c == 0xF0 && s[i + 1] < 0x90) || (c == 0xF4 && s[i + 1] > 0x8F)) return 0;
        i += len;
    }
    return 1;
}

/* Text as UTF-8 into a malloc'ed string: kept if it is valid UTF-8, else read as
 * Windows-1252. Control characters other than CR, LF and tab are dropped. */
static char* text_utf8(const char* in, size_t n)
{
    const uint8_t* s = (const uint8_t*)in;
    int utf8 = valid_utf8(s, n);
    char* out = (char*)malloc(n * 3 + 1);
    size_t i, k = 0;
    if (!out) return NULL;
    for (i = 0; i < n; i++) {
        uint32_t c = s[i];
        if (c == 0) break;
        if (c < 0x20 && c != '\r' && c != '\n' && c != '\t') continue;
        if (c < 0x80 || utf8) { out[k++] = (char)c; continue; }
        if (c < 0xA0) c = cp1252[c - 0x80];
        if (c < 0x800) { out[k++] = (char)(0xC0 | (c >> 6)); out[k++] = (char)(0x80 | (c & 0x3F)); }
        else { out[k++] = (char)(0xE0 | (c >> 12)); out[k++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[k++] = (char)(0x80 | (c & 0x3F)); }
    }
    out[k] = '\0';
    return out;
}

static void latin1_to_utf8(const char* in, char* out, size_t cap)
{
    char* u = text_utf8(in, strlen(in));
    snprintf(out, cap, "%s", u ? u : "");
    free(u);
}

/* Copies UTF-8 `in` into `out` (cap bytes), cutting at a character boundary. */
static void copy_utf8(char* out, size_t cap, const char* in)
{
    size_t n = strlen(in);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((uint8_t)in[n] & 0xC0) == 0x80) n--;
    }
    memcpy(out, in, n);
    out[n] = '\0';
}

typedef struct midi_text {
    fa_frames frame;
    int32_t type;               /* fa_midi_text_type */
    uint32_t flags;
    int32_t track;
    char* text;                 /* UTF-8 */
} midi_text;

/* ---- The decoder ---------------------------------------------------------------- */

enum { EV_NOTE_OFF, EV_NOTE_ON, EV_CONTROL, EV_PROGRAM, EV_PITCH };

typedef struct midi_event {
    fa_frames frame;
    uint16_t value;             /* velocity, controller value, program or pitch bend */
    uint8_t type, channel, key; /* key: note or controller number */
} midi_event;

struct fa_midi {
    fa_soundfont* font;         /* a reference */
    tsf* synth;
    uint32_t rate;
    fa_frames length;

    midi_event* ev;
    size_t nev, next;           /* next: first event not applied yet */

    midi_text* texts;
    fa_frames* text_frames;     /* the same frames, for the audio thread */
    uint32_t ntexts, nlyrics;
    fa_midi_info info;

    /* Rendering: blk holds frames [rpos - blk_len, rpos); blk_off of them were read. */
    float blk[FA_MIDI_BLOCK * 2];
    int blk_len, blk_off;
    fa_frames rpos;

    /* Notes started and not ended yet, per channel and key as the file has them:
     * how many, and the (transposed) key they sound on. */
    uint8_t depth[16][128];
    uint8_t sounding[16][128];
    int transpose_now;          /* as applied by the decode thread */
    uint32_t mute_now;

    fa_atomic32 transpose;      /* set by the API */
    fa_atomic32 mute;           /* bit n = channel n */
};

struct fa_midi* fa_midi_of(fa_decoder* dec)
{
    return dec && dec->vt == &fa_decoder_midi ? (struct fa_midi*)dec->impl : NULL;
}

const fa_frames* fa_midi_text_frames(const struct fa_midi* m, uint32_t* out_count)
{
    *out_count = m ? m->ntexts : 0;
    return m ? m->text_frames : NULL;
}

/* Ends every voice of `key` started for (channel, key-as-in-the-file). */
static void end_notes(struct fa_midi* m, int ch, int key)
{
    while (m->depth[ch][key] > 0) {
        tsf_channel_note_off(m->synth, ch, m->sounding[ch][key]);
        m->depth[ch][key]--;
    }
}

static void forget_channel(struct fa_midi* m, int ch)
{
    memset(m->depth[ch], 0, sizeof(m->depth[ch]));
}

/* Applies one event. `silent`: replaying before a seek target, no notes start. */
static void apply_event(struct fa_midi* m, const midi_event* e, int silent)
{
    int ch = e->channel, key = e->key;
    switch (e->type) {
    case EV_NOTE_ON: {
        int play = key;
        if (silent || (m->mute_now >> ch) & 1) return;
        if (ch != FA_MIDI_DRUMS) play += m->transpose_now;
        if (play < 0 || play > 127) return;
        if (m->depth[ch][key] == 255) return;   /* every note needs its note-off counted */
        /* The same key again while held on another pitch (the transpose changed):
         * end those first, so no note is left hanging. */
        if (m->depth[ch][key] > 0 && m->sounding[ch][key] != play) end_notes(m, ch, key);
        tsf_channel_note_on(m->synth, ch, play, (float)e->value / 127.0f);
        m->sounding[ch][key] = (uint8_t)play;
        m->depth[ch][key]++;
        return;
    }
    case EV_NOTE_OFF:
        if (m->depth[ch][key] > 0) {
            tsf_channel_note_off(m->synth, ch, m->sounding[ch][key]);
            m->depth[ch][key]--;
        }
        return;
    case EV_CONTROL:
        tsf_channel_midi_control(m->synth, ch, key, e->value);
        if (key == TML_ALL_SOUND_OFF || key == TML_ALL_NOTES_OFF) forget_channel(m, ch);
        return;
    case EV_PROGRAM:
        tsf_channel_set_presetnumber(m->synth, ch, e->value, ch == FA_MIDI_DRUMS);
        return;
    case EV_PITCH:
        tsf_channel_set_pitchwheel(m->synth, ch, e->value);
        return;
    default:
        return;
    }
}

/* Takes the API's controls at a block start. A channel muted now goes silent at
 * once (a 10 ms release, no click). */
static void take_controls(struct fa_midi* m)
{
    uint32_t mute = (uint32_t)fa_load32(&m->mute);
    int ch;
    m->transpose_now = fa_load32(&m->transpose);
    for (ch = 0; ch < 16; ch++) {
        if (!((mute & ~m->mute_now) >> ch & 1)) continue;
        tsf_channel_sounds_off_all(m->synth, ch);
        forget_channel(m, ch);
    }
    m->mute_now = mute;
}

/* Renders the block starting at rpos (a multiple of FA_MIDI_BLOCK). */
static void render_block(struct fa_midi* m)
{
    fa_frames start = m->rpos, n = m->length - start, done = 0;
    if (n > FA_MIDI_BLOCK) n = FA_MIDI_BLOCK;
    take_controls(m);
    while (done < n) {
        fa_frames upto = n;
        while (m->next < m->nev && m->ev[m->next].frame <= start + done) apply_event(m, &m->ev[m->next++], 0);
        if (m->next < m->nev && m->ev[m->next].frame < start + n) upto = m->ev[m->next].frame - start;
        tsf_render_float(m->synth, m->blk + done * 2, (int)(upto - done), 0);
        done = upto;
    }
    m->blk_len = (int)n;
    m->blk_off = 0;
    m->rpos = start + n;
}

static fa_result midi_read(fa_decoder* dec, float* out, fa_frames frames, fa_frames* got)
{
    struct fa_midi* m = (struct fa_midi*)dec->impl;
    fa_frames done = 0;
    while (done < frames) {
        fa_frames k;
        if (m->blk_off == m->blk_len) {
            if (m->rpos >= m->length) break;
            render_block(m);
        }
        k = m->blk_len - m->blk_off;
        if (k > frames - done) k = frames - done;
        memcpy(out + done * 2, m->blk + (size_t)m->blk_off * 2, (size_t)k * 2 * sizeof(float));
        m->blk_off += (int)k;
        done += k;
    }
    *got = done;
    return done > 0 ? FA_OK : FA_ERROR_END_OF_STREAM;
}

static fa_result midi_seek(fa_decoder* dec, fa_frames frame)
{
    struct fa_midi* m = (struct fa_midi*)dec->impl;
    fa_frames block = frame - frame % FA_MIDI_BLOCK;
    tsf* fresh = synth_new(m->font, m->rate);
    if (!fresh) return FA_ERROR_OUT_OF_MEMORY;
    synth_free(m->font, m->synth);
    m->synth = fresh;
    memset(m->depth, 0, sizeof(m->depth));
    m->mute_now = 0;
    /* Everything before the target's block, without sound; what is at its first
     * frame is applied by render_block, as when playing through. */
    for (m->next = 0; m->next < m->nev && m->ev[m->next].frame < block; m->next++) apply_event(m, &m->ev[m->next], 1);
    m->rpos = block;
    m->blk_len = m->blk_off = 0;
    if (frame < m->length) {
        render_block(m);
        m->blk_off = (int)(frame - block);
    } else {
        m->rpos = m->length;   /* at the end: nothing more to read */
    }
    return FA_OK;
}

static void midi_free(struct fa_midi* m)
{
    uint32_t i;
    if (!m) return;
    if (m->synth) synth_free(m->font, m->synth);
    fa_soundfont_release(m->font);
    for (i = 0; i < m->ntexts; i++) free(m->texts[i].text);
    free(m->texts);
    free(m->text_frames);
    free(m->ev);
    free(m);
}

static void midi_close(fa_decoder* dec)
{
    midi_free((struct fa_midi*)dec->impl);
    dec->impl = NULL;
}

/* The whole source, however it is given (its length may be unknown). */
static fa_result read_source(fa_source* src, uint8_t** out, size_t* out_size)
{
    /* One byte of slack: the read that finds the end needs no extra room, and a
     * file of exactly FA_MIDI_MAX_FILE bytes fits. */
    size_t cap, n = 0;
    uint8_t* buf;
    *out = NULL;
    *out_size = 0;
    if (src->length > (int64_t)FA_MIDI_MAX_FILE) return FA_ERROR_UNSUPPORTED_FORMAT;
    cap = (src->length > 0 ? (size_t)src->length : 65536) + 1;
    buf = (uint8_t*)malloc(cap);
    if (!buf) return FA_ERROR_OUT_OF_MEMORY;
    for (;;) {
        int64_t got;
        if (n == cap) {
            uint8_t* grown;
            if (cap > FA_MIDI_MAX_FILE) { free(buf); return FA_ERROR_UNSUPPORTED_FORMAT; }
            cap = cap * 2 > FA_MIDI_MAX_FILE + 1 ? FA_MIDI_MAX_FILE + 1 : cap * 2;
            grown = (uint8_t*)realloc(buf, cap);
            if (!grown) { free(buf); return FA_ERROR_OUT_OF_MEMORY; }
            buf = grown;
        }
        got = fa_source_read(src, buf + n, cap - n);
        if (got < 0) { free(buf); return (fa_result)got; }
        if (got == 0) break;
        n += (size_t)got;
    }
    *out = buf;
    *out_size = n;
    return FA_OK;
}

/* The Standard MIDI File inside `d`: the file itself, or the "data" chunk of a
 * RIFF RMID file. NULL if there is none. */
static const uint8_t* find_smf(const uint8_t* d, size_t n, size_t* out_n)
{
    size_t pos;
    if (n >= 14 && memcmp(d, "MThd", 4) == 0) { *out_n = n; return d; }
    if (n < 12 || memcmp(d, "RIFF", 4) != 0 || memcmp(d + 8, "RMID", 4) != 0) return NULL;
    for (pos = 12; pos + 8 <= n;) {
        uint32_t size = rd32le(d + pos + 4);
        if (size > n - pos - 8) size = (uint32_t)(n - pos - 8);   /* a truncated file: what is there */
        if (memcmp(d + pos, "data", 4) == 0 && size >= 14 && memcmp(d + pos + 8, "MThd", 4) == 0) {
            *out_n = size;
            return d + pos + 8;
        }
        pos += 8 + (size_t)size + (size & 1);
    }
    return NULL;
}

static int starts_with(const char* s, const char* prefix) { return strncmp(s, prefix, strlen(prefix)) == 0; }

static fa_result add_text(struct fa_midi* m, uint32_t* cap, fa_frames frame, int type, uint32_t flags, int track, char* text)
{
    if (m->ntexts == *cap) {
        uint32_t ncap = *cap ? *cap * 2 : 64;
        midi_text* grown = (midi_text*)realloc(m->texts, ncap * sizeof(midi_text));
        if (!grown) { free(text); return FA_ERROR_OUT_OF_MEMORY; }
        m->texts = grown;
        *cap = ncap;
    }
    m->texts[m->ntexts].frame = frame;
    m->texts[m->ntexts].type = type;
    m->texts[m->ntexts].flags = flags;
    m->texts[m->ntexts].track = track;
    m->texts[m->ntexts].text = text;
    m->ntexts++;
    return FA_OK;
}

/* Cleans one syllable: "/" or "\" in front (karaoke files) and CR/LF in front
 * break before it; CR/LF at its end break before the next one (*pending). The
 * marks are removed from `t` in place. */
/* CR (with or without LF) ends a line, LF alone a paragraph (RP-026). */
static uint32_t break_flags(int cr, int lf)
{
    if (lf && !cr) return FA_MIDI_NEW_PARAGRAPH | FA_MIDI_NEW_LINE;
    return cr || lf ? FA_MIDI_NEW_LINE : 0;
}

static uint32_t lyric_flags(char* t, uint32_t* pending)
{
    uint32_t flags = *pending;
    size_t n, i = 0;
    int cr = 0, lf = 0;
    *pending = 0;
    for (; t[i] == '\r' || t[i] == '\n'; i++) { if (t[i] == '\r') cr = 1; else lf = 1; }
    flags |= break_flags(cr, lf);
    if (t[i] == '/') { flags |= FA_MIDI_NEW_LINE; i++; }
    else if (t[i] == '\\') { flags |= FA_MIDI_NEW_PARAGRAPH | FA_MIDI_NEW_LINE; i++; }
    memmove(t, t + i, strlen(t + i) + 1);
    n = strlen(t);
    cr = lf = 0;
    while (n > 0 && (t[n - 1] == '\r' || t[n - 1] == '\n')) {
        if (t[n - 1] == '\r') cr = 1; else lf = 1;
        t[--n] = '\0';
    }
    *pending = break_flags(cr, lf);
    return flags;
}

/* Builds the event list, the texts and the info from the parsed file. */
static fa_result build(struct fa_midi* m, tml_message* first, const uint8_t* smf)
{
    tml_message* msg;
    size_t nev = 0, k = 0;
    uint32_t cap = 0, i, pending = 0;
    int have_lyrics = 0, words_track = -1, karaoke = 0, ntitle = 0, words_are_text;
    uint32_t* text_per_track;
    fa_frames last = 0;
    fa_result r = FA_OK;
    double rate = (double)m->rate;

    m->info.format = smf[9];
    m->info.tracks = ((uint32_t)smf[10] << 8) | smf[11];
    text_per_track = (uint32_t*)calloc(m->info.tracks ? m->info.tracks : 1, sizeof(uint32_t));
    if (!text_per_track) return FA_ERROR_OUT_OF_MEMORY;

    /* Which text events are the lyrics: lyric events if the file has any; else, in
     * a .kar file, the text events of its "Words" track, or of the track with the
     * most text events. */
    for (msg = first; msg; msg = msg->next) {
        if (msg->seconds > FA_MIDI_MAX_SECONDS) break;
        if (msg->type == TML_NOTE_ON || msg->type == TML_NOTE_OFF || msg->type == TML_CONTROL_CHANGE ||
            msg->type == TML_PROGRAM_CHANGE || msg->type == TML_PITCH_BEND)
            nev++;
        if (msg->type == TML_LYRIC) have_lyrics = 1;
        if (msg->type == TML_TRACK_NAME && words_track < 0 && strcmp(msg->text, "Words") == 0) words_track = msg->track;
        if (msg->type == TML_TEXT) {
            if (starts_with(msg->text, "@K") || starts_with(msg->text, "@T")) karaoke = 1;
            if (msg->text[0] != '@' && msg->track < m->info.tracks) text_per_track[msg->track]++;
        }
    }
    m->info.karaoke = karaoke || words_track >= 0;
    words_are_text = m->info.karaoke && !have_lyrics;
    if (words_are_text) {
        if (words_track < 0) {
            uint32_t best = 0, t;
            for (t = 0; t < m->info.tracks; t++) if (text_per_track[t] > best) { best = text_per_track[t]; words_track = (int)t; }
        }
    }
    free(text_per_track);

    m->ev = (midi_event*)malloc((nev ? nev : 1) * sizeof(midi_event));
    if (!m->ev) return FA_ERROR_OUT_OF_MEMORY;
    for (msg = first; msg && r == FA_OK; msg = msg->next) {
        fa_frames frame;
        if (msg->seconds > FA_MIDI_MAX_SECONDS) break;
        frame = (fa_frames)(msg->seconds * rate + 0.5);
        if (frame > last) last = frame;
        switch (msg->type) {
        case TML_NOTE_ON: case TML_NOTE_OFF: case TML_CONTROL_CHANGE: case TML_PROGRAM_CHANGE: case TML_PITCH_BEND: {
            midi_event* e = &m->ev[k++];
            e->frame = frame;
            e->channel = msg->channel & 15;
            e->key = (uint8_t)(msg->key & 0x7F);
            e->value = (uint16_t)(msg->velocity & 0x7F);
            switch (msg->type) {
            case TML_NOTE_ON: e->type = msg->velocity ? EV_NOTE_ON : EV_NOTE_OFF; break;
            case TML_NOTE_OFF: e->type = EV_NOTE_OFF; break;
            case TML_CONTROL_CHANGE: e->type = EV_CONTROL; break;
            case TML_PROGRAM_CHANGE: e->type = EV_PROGRAM; e->value = (uint16_t)(msg->program & 0x7F); break;
            default: e->type = EV_PITCH; e->value = (uint16_t)(msg->pitch_bend & 0x3FFF); break;
            }
            break;
        }
        case TML_TEXT: case TML_COPYRIGHT: case TML_TRACK_NAME: case TML_INST_NAME:
        case TML_LYRIC: case TML_MARKER: case TML_CUE_POINT: {
            int type;
            uint32_t flags = 0;
            char* text = text_utf8(msg->text, msg->text_length);
            if (!text) { r = FA_ERROR_OUT_OF_MEMORY; break; }
            switch (msg->type) {
            case TML_LYRIC: type = FA_MIDI_LYRIC; break;
            case TML_TEXT:
                type = words_are_text && msg->track == words_track && msg->text[0] != '@' ? FA_MIDI_LYRIC : FA_MIDI_TEXT;
                break;
            case TML_MARKER: type = FA_MIDI_MARKER; break;
            case TML_CUE_POINT: type = FA_MIDI_CUE; break;
            case TML_TRACK_NAME: type = FA_MIDI_TRACK_NAME; break;
            case TML_INST_NAME: type = FA_MIDI_INSTRUMENT_NAME; break;
            default: type = FA_MIDI_COPYRIGHT; break;
            }
            if (type == FA_MIDI_LYRIC) {
                flags = lyric_flags(text, &pending);
                if (!text[0]) { pending |= flags; free(text); break; }   /* only line marks: they go to the next syllable */
                m->nlyrics++;
            } else if (type == FA_MIDI_TEXT && starts_with(text, "@T")) {
                /* .kar header: the first @T is the title, the second the artist. */
                if (ntitle == 0) copy_utf8(m->info.title, sizeof(m->info.title), text + 2);
                else if (ntitle == 1) copy_utf8(m->info.artist, sizeof(m->info.artist), text + 2);
                ntitle++;
            } else if (type == FA_MIDI_COPYRIGHT && !m->info.copyright[0]) {
                copy_utf8(m->info.copyright, sizeof(m->info.copyright), text);
            } else if (type == FA_MIDI_TRACK_NAME && msg->track == 0 && !m->info.title[0] && ntitle == 0) {
                copy_utf8(m->info.title, sizeof(m->info.title), text);
            }
            r = add_text(m, &cap, frame, type, flags, msg->track, text);
            break;
        }
        default:
            break;   /* tempo (already in the times), end of track (its time counts) */
        }
    }
    if (r != FA_OK) return r;
    m->nev = k;
    m->length = last + (fa_frames)(FA_MIDI_TAIL_S * rate);
    m->info.text_count = m->ntexts;
    m->info.lyric_count = m->nlyrics;
    if (m->ntexts) {
        m->text_frames = (fa_frames*)malloc(m->ntexts * sizeof(fa_frames));
        if (!m->text_frames) return FA_ERROR_OUT_OF_MEMORY;
        for (i = 0; i < m->ntexts; i++) m->text_frames[i] = m->texts[i].frame;
    }
    return FA_OK;
}

static fa_result midi_open(fa_decoder* dec)
{
    struct fa_midi* m;
    uint8_t* bytes;
    size_t size, smf_size = 0;
    const uint8_t* smf;
    tml_message* first;
    int rmid;
    fa_result r = read_source(dec->src, &bytes, &size);
    if (r != FA_OK) return r;
    smf = find_smf(bytes, size, &smf_size);
    if (!smf || smf_size > (size_t)0x7FFFFFFF) { free(bytes); return FA_ERROR_UNSUPPORTED_FORMAT; }
    rmid = smf != bytes;
    /* A MIDI file it is: without instruments it cannot be played. */
    if (!dec->soundfont) { free(bytes); return FA_ERROR_NO_SOUNDFONT; }
    first = tml_load_memory(smf, (int)smf_size);
    if (!first) { free(bytes); return FA_ERROR_UNSUPPORTED_FORMAT; }

    m = (struct fa_midi*)calloc(1, sizeof(*m));
    if (!m) { tml_free(first); free(bytes); return FA_ERROR_OUT_OF_MEMORY; }
    m->rate = dec->midi_rate ? dec->midi_rate : FA_MIDI_DEFAULT_RATE;
    m->font = fa_soundfont_retain(dec->soundfont);
    r = build(m, first, smf);
    tml_free(first);
    free(bytes);
    if (r == FA_OK) {
        m->synth = synth_new(m->font, m->rate);
        if (!m->synth) r = FA_ERROR_OUT_OF_MEMORY;
    }
    if (r != FA_OK) { midi_free(m); return r; }

    dec->impl = m;
    dec->format.sample_rate = m->rate;
    dec->format.channels = 2;
    dec->length = m->length;
    dec->seekable = 1;
    snprintf(dec->codec, sizeof(dec->codec), "midi");
    snprintf(dec->container, sizeof(dec->container), "%s", rmid ? "rmid" : "smf");
    return FA_OK;
}

const fa_decoder_vtable fa_decoder_midi = {
    "midi", "Standard MIDI File and RIFF MIDI (SoundFont 2 synthesizer)", "mid;midi;kar;rmi;smf",
    "audio/midi;audio/x-midi;audio/mid;audio/sp-midi",
    fa_probe_midi, midi_open, midi_read, midi_seek, midi_close, 0, NULL, NULL
};

/* ---- MIDI stream API ---------------------------------------------------------- */

FA_API fa_result FA_CALL fa_stream_get_midi_info(fa_stream* stream, fa_midi_info* out_info)
{
    if (out_info) memset(out_info, 0, sizeof(*out_info));
    if (!stream || !out_info || !stream->midi) return FA_ERROR_INVALID_ARG;
    *out_info = stream->midi->info;
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_midi_text(fa_stream* stream, uint32_t index, fa_midi_text* out_text)
{
    const midi_text* t;
    if (out_text) memset(out_text, 0, sizeof(*out_text));
    if (!stream || !out_text || !stream->midi) return FA_ERROR_INVALID_ARG;
    if (index >= stream->midi->ntexts) return FA_ERROR_NOT_FOUND;
    t = &stream->midi->texts[index];
    out_text->frame = t->frame;
    out_text->time = (fa_time)((double)t->frame * 1e9 / (double)stream->midi->rate + 0.5);
    out_text->type = t->type;
    out_text->flags = t->flags;
    out_text->track = t->track;
    copy_utf8(out_text->text, sizeof(out_text->text), t->text);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_set_midi_transpose(fa_stream* stream, int32_t semitones)
{
    if (!stream || !stream->midi || semitones < -FA_MIDI_TRANSPOSE_MAX || semitones > FA_MIDI_TRANSPOSE_MAX)
        return FA_ERROR_INVALID_ARG;
    fa_store32(&stream->midi->transpose, semitones);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_midi_transpose(fa_stream* stream, int32_t* out_semitones)
{
    if (!stream || !out_semitones || !stream->midi) return FA_ERROR_INVALID_ARG;
    *out_semitones = fa_load32(&stream->midi->transpose);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_set_midi_channel_mute(fa_stream* stream, uint32_t channel, int32_t muted)
{
    fa_atomic32* mask;
    int32_t old, bit;
    if (!stream || !stream->midi || channel > 15) return FA_ERROR_INVALID_ARG;
    mask = &stream->midi->mute;
    bit = (int32_t)(1u << channel);
    do {
        old = fa_load32(mask);
    } while (!fa_cas32(mask, old, muted ? (old | bit) : (old & ~bit)));
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_midi_channel_mute(fa_stream* stream, uint32_t channel, int32_t* out_muted)
{
    if (!stream || !out_muted || !stream->midi || channel > 15) return FA_ERROR_INVALID_ARG;
    *out_muted = (fa_load32(&stream->midi->mute) >> channel) & 1;
    return FA_OK;
}
