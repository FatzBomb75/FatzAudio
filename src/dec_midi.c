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
 * rendering: programs, controllers, pitch bends, the pedal and SysEx state are
 * restored, and notes held across the target (by a key or the pedal) start again
 * from their attack, as BASSMIDI does, so a held chord or pad is not silent after
 * a seek into it. Drum notes are not restarted (they would sound as a new hit).
 *
 * SysEx: GM, GM2, GS and XG resets, GS rhythm parts and XG drum parts, XG and GM2
 * drum banks (bank select MSB 127 and 120), master volume (universal, GS, XG)
 * and master fine and coarse tuning. Other SysEx is ignored.
 *
 * Reverb and chorus (src/midi_fx.c) are fed by each voice's send levels: CC91
 * and CC93 (40 and 0 after a reset, as on GS and GM2 devices) and the
 * soundfont's effect-send generators. They are on unless the app turns them off.
 * A seek clears their tails: what is heard after a seek equals reading through
 * once the reverb of the notes before the target would have died away.
 *
 * Live controls (transpose, channel mute, effects) are atomics the decode thread reads at
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
#define FA_MIDI_SYSEX_MAX 64              /* longer SysEx (sample dumps, bulk data) is dropped by the parser */
#define FA_MIDI_GAIN_RAMP 64              /* frames a master volume change glides over */

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
    if (size <= FA_MIDI_MAX_SOUNDFONT && fa_dls_probe(data, size)) {
        /* A DLS bank (Windows' gm.dls, macOS's gs_instruments.dls): as SoundFont 2. */
        uint8_t* sf2 = NULL;
        size_t sf2_size = 0;
        fa_result r = fa_dls_to_sf2(data, size, &sf2, &sf2_size);
        if (r != FA_OK) return r;
        r = sf2_size <= FA_MIDI_MAX_SOUNDFONT ? soundfont_from_memory(sf2, sf2_size, out) : FA_ERROR_UNSUPPORTED_FORMAT;
        free(sf2);
        return r;
    }
    if (size < 12 || size > FA_MIDI_MAX_SOUNDFONT || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "sfbk", 4) != 0)
        return FA_ERROR_UNSUPPORTED_FORMAT;
    if (fa_sf3_probe(data, size)) {
        /* SF3 (Ogg Vorbis samples, MuseScore's banks): as SoundFont 2. */
        uint8_t* sf2 = NULL;
        size_t sf2_size = 0;
        fa_result r = fa_sf3_to_sf2(data, size, &sf2, &sf2_size);
        if (r != FA_OK) return r;
        r = sf2_size <= FA_MIDI_MAX_SOUNDFONT && !fa_sf3_probe(sf2, sf2_size) ? soundfont_from_memory(sf2, sf2_size, out)
                                                                              : FA_ERROR_UNSUPPORTED_FORMAT;
        free(sf2);
        return r;
    }
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

FA_API fa_result FA_CALL fa_soundfont_open_system(fa_soundfont** out_font)
{
    char path[1024];
    fa_result last = FA_ERROR_NOT_FOUND;
    int i;
    if (out_font) *out_font = NULL;
    if (!out_font) return FA_ERROR_INVALID_ARG;
    for (i = 0; fa_system_soundfont_path(i, path, sizeof(path)); i++) {
        fa_result r = fa_soundfont_open_path(path, out_font);
        if (r == FA_OK) return FA_OK;
        if (r != FA_ERROR_NOT_FOUND) last = r;   /* there but not usable: say why */
    }
    return last;
}

fa_soundfont* fa_engine_system_soundfont(fa_engine* engine)
{
    fa_soundfont* f = NULL;
    if (engine->config.flags & FA_ENGINE_NO_SYSTEM_SOUNDFONT) return NULL;
    /* One load at a time: streams opening together wait for the first one's bank
     * instead of each loading their own. A failure other than "there is none"
     * (out of memory, a locked file) is tried again by the next stream. */
    fa_mutex_lock(&engine->system_soundfont_lock);
    if (!engine->system_soundfont_tried) {
        fa_result r = fa_soundfont_open_system(&engine->system_soundfont);
        if (r == FA_OK || r == FA_ERROR_NOT_FOUND) engine->system_soundfont_tried = 1;
    }
    f = fa_soundfont_retain(engine->system_soundfont);
    fa_mutex_unlock(&engine->system_soundfont_lock);
    return f;
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

enum { EV_NOTE_OFF, EV_NOTE_ON, EV_CONTROL, EV_PROGRAM, EV_PITCH, EV_SYSEX };

typedef struct midi_event {
    fa_frames frame;
    uint16_t value;             /* velocity, controller value, program, pitch bend; SysEx: index in sysex */
    uint8_t type, channel, key; /* key: note or controller number */
} midi_event;

typedef struct midi_sysex {
    uint8_t len;
    uint8_t d[FA_MIDI_SYSEX_MAX];   /* after the F0, up to and with the F7 */
} midi_sysex;

/* What the file's resets made of the synth. */
enum { MODE_GM, MODE_GM2, MODE_GS, MODE_XG };

struct fa_midi {
    fa_soundfont* font;         /* a reference */
    tsf* synth;
    uint32_t rate;
    fa_frames length;

    midi_event* ev;
    size_t nev, next;           /* next: first event not applied yet */
    midi_sysex* sysex;
    uint32_t nsysex;

    midi_text* texts;
    fa_frames* text_frames;     /* the same frames, for the audio thread */
    uint32_t ntexts, nlyrics;
    fa_midi_mark* marks;        /* tempo and signatures */
    uint32_t nmarks, marks_cap;
    fa_midi_info info;

    /* Rendering: blk holds frames [rpos - blk_len, rpos); blk_off of them were read. */
    float blk[FA_MIDI_BLOCK * 2];
    float rev[FA_MIDI_BLOCK * 2], cho[FA_MIDI_BLOCK * 2];   /* the block's reverb and chorus sends */
    fa_midi_fx* fx;
    int effects_now;            /* as applied by the decode thread */
    int blk_len, blk_off;
    fa_frames rpos;

    /* Notes started and not ended yet, per channel and key as the file has them:
     * how many, and the (transposed) key they sound on. */
    uint8_t depth[16][128];
    uint8_t sounding[16][128];
    int transpose_now;          /* as applied by the decode thread */
    uint32_t mute_now;

    /* Channel state the synth needs from us: which channels are drums. */
    int mode;                   /* MODE_* */
    uint8_t program[16];
    uint8_t bank_msb[16];
    uint8_t gs_rhythm[16];      /* GS "use for rhythm part", XG part mode: 1 = a drum part */
    uint8_t drums[16];          /* the channel plays drum kits now */
    /* Master volume SysEx: from `gain_from` at `gain_at` to `gain_to` over
     * FA_MIDI_GAIN_RAMP frames, then `gain_to`. */
    float gain_from, gain_to;
    fa_frames gain_at;
    float tune_fine, tune_coarse;  /* master tuning SysEx, semitones */

    /* Replaying before a seek target: depth counts the notes held by a key (none
     * sounds yet); the loudest velocity, and the notes only the pedal holds, are
     * kept to start them again at the target. A key held by several note-ons
     * restarts as one voice: `merged` note-offs end nothing, the last one ends it. */
    uint8_t held_vel[16][128];
    uint8_t pedal_held[16][128];
    uint8_t merged[16][128];
    uint8_t pedal[16];

    fa_atomic32 transpose;      /* set by the API */
    fa_atomic32 mute;           /* bit n = channel n */
    fa_atomic32 effects;        /* reverb and chorus on (1, the default) or off */
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
    memset(m->pedal_held[ch], 0, sizeof(m->pedal_held[ch]));
    memset(m->merged[ch], 0, sizeof(m->merged[ch]));
}

/* Master volume at `frame` (at or after the last change). Frames past the ramp,
 * gain_at + FA_MIDI_GAIN_RAMP on, are all gain_to. */
static float gain_at(const struct fa_midi* m, fa_frames frame)
{
    fa_frames k = frame - m->gain_at;
    if (k >= FA_MIDI_GAIN_RAMP || m->gain_from == m->gain_to) return m->gain_to;
    return m->gain_from + (m->gain_to - m->gain_from) * (float)k / (float)FA_MIDI_GAIN_RAMP;
}

static void set_master_gain(struct fa_midi* m, float gain, fa_frames frame)
{
    m->gain_from = gain_at(m, frame);
    m->gain_to = gain;
    m->gain_at = frame;
}

static void reset_channels(struct fa_midi* m, int mode, fa_frames frame);

/* A fresh synth at the song's start, at open and before a seek's replay. */
static void start_song(struct fa_midi* m)
{
    m->gain_from = m->gain_to = 1.0f;
    m->gain_at = 0;
    reset_channels(m, MODE_GM, 0);
}

/* Whether a channel plays drum kits: GS rhythm parts (channel 10 unless SysEx
 * says otherwise), XG drum parts and bank 127 (126: SFX kits), GM2 bank 120. */
static int is_drum_channel(const struct fa_midi* m, int ch)
{
    switch (m->mode) {
    case MODE_XG: return m->bank_msb[ch] == 127 || m->bank_msb[ch] == 126;
    case MODE_GM2: return m->bank_msb[ch] == 120;
    default: return m->gs_rhythm[ch];
    }
}

static void select_program(struct fa_midi* m, int ch)
{
    m->drums[ch] = (uint8_t)is_drum_channel(m, ch);
    tsf_channel_set_presetnumber(m->synth, ch, m->program[ch], m->drums[ch]);
}

static void sync_bank(struct fa_midi* m, int ch);

/* Defaults, as after a GM/GS/XG reset at `frame` (and at the start: GM). */
static void reset_channels(struct fa_midi* m, int mode, fa_frames frame)
{
    int ch;
    m->mode = mode;
    memset(m->gs_rhythm, 0, sizeof(m->gs_rhythm));
    memset(m->bank_msb, 0, sizeof(m->bank_msb));
    memset(m->program, 0, sizeof(m->program));
    m->gs_rhythm[FA_MIDI_DRUMS] = mode == MODE_GM || mode == MODE_GS;   /* XG and GM2: the bank decides */
    if (mode == MODE_XG) m->bank_msb[FA_MIDI_DRUMS] = 127;
    if (mode == MODE_GM2) {
        memset(m->bank_msb, 121, sizeof(m->bank_msb));
        m->bank_msb[FA_MIDI_DRUMS] = 120;
    }
    set_master_gain(m, 1.0f, frame);
    m->tune_fine = m->tune_coarse = 0.0f;
    if (!m->synth) return;
    tsf_set_tuning(m->synth, 0.0f);
    for (ch = 0; ch < 16; ch++) {
        /* Notes end, as on a GS or XG device (and a seek after it restarts none). */
        tsf_channel_note_off_all(m->synth, ch);
        forget_channel(m, ch);
        tsf_channel_midi_control(m->synth, ch, TML_ALL_CTRL_OFF, 0);
        tsf_channel_midi_control(m->synth, ch, TML_VOLUME_MSB, 100);
        tsf_channel_set_sustain(m->synth, ch, 0);
        tsf_channel_set_pitchwheel(m->synth, ch, 8192);
        tsf_channel_midi_control(m->synth, ch, 91, 40);   /* reverb send: the GM2 / GS default */
        tsf_channel_midi_control(m->synth, ch, 93, 0);
        sync_bank(m, ch);
        m->pedal[ch] = 0;
        select_program(m, ch);
    }
}

/* The bank tsf looks presets up in: bank select MSB alone, as tsf takes it. */
static void sync_bank(struct fa_midi* m, int ch)
{
    tsf_channel_set_bank(m->synth, ch, m->bank_msb[ch] ? (0x8000 | m->bank_msb[ch]) : 0);
}

/* GS rhythm part or XG part mode. In XG and GM2 the bank decides (XG: bank select
 * 127 makes a drum part, as the part mode does), so there it sets the bank. */
static void set_drum_part(struct fa_midi* m, int ch, int drum)
{
    if (m->mode == MODE_XG || m->mode == MODE_GM2) {
        m->bank_msb[ch] = (uint8_t)(m->mode == MODE_XG ? (drum ? 127 : 0) : (drum ? 120 : 121));
        sync_bank(m, ch);
    } else {
        m->gs_rhythm[ch] = (uint8_t)drum;
    }
    select_program(m, ch);
}

/* The SysEx messages FatzAudio acts on (after the F0; device numbers are ignored,
 * checksums are not checked). */
static void apply_sysex(struct fa_midi* m, const uint8_t* d, size_t n, fa_frames frame)
{
    if (n >= 4 && d[0] == 0x7E && d[2] == 0x09) {                    /* GM System On / GM2 On */
        if (d[3] == 0x01) reset_channels(m, MODE_GM, frame);
        else if (d[3] == 0x03) reset_channels(m, MODE_GM2, frame);
    } else if (n >= 6 && d[0] == 0x7F && d[2] == 0x04) {             /* universal real-time device control */
        int v = d[4] | (d[5] << 7);
        if (d[3] == 0x01) set_master_gain(m, (float)v / 16383.0f, frame);   /* master volume */
        else if (d[3] == 0x03) { m->tune_fine = ((float)v - 8192.0f) / 8192.0f; tsf_set_tuning(m->synth, m->tune_coarse + m->tune_fine); }
        else if (d[3] == 0x04) { m->tune_coarse = (float)(d[5] - 64); tsf_set_tuning(m->synth, m->tune_coarse + m->tune_fine); }
    } else if (n >= 8 && d[0] == 0x41 && d[2] == 0x42 && d[3] == 0x12) {   /* Roland GS, DT1 */
        int a1 = d[4], a2 = d[5], a3 = d[6], v = d[7];
        if ((a1 == 0x40 && a2 == 0x00 && a3 == 0x7F) || (a1 == 0x00 && a2 == 0x00 && a3 == 0x7F)) {
            reset_channels(m, MODE_GS, frame);                        /* GS reset, system mode set */
        } else if (a1 == 0x40 && a2 == 0x00 && a3 == 0x04) {
            set_master_gain(m, (float)v / 127.0f, frame);
        } else if (a1 == 0x40 && (a2 & 0xF0) == 0x10 && a3 == 0x15) {
            /* Use for rhythm part: block 0 is part 10, 1..9 parts 1..9, A..F 11..16. */
            int block = a2 & 0x0F;
            int ch = block == 0 ? 9 : block <= 9 ? block - 1 : block;
            set_drum_part(m, ch, v != 0);
        }
    } else if (n >= 7 && d[0] == 0x43 && (d[1] & 0xF0) == 0x10 && d[2] == 0x4C) {   /* Yamaha XG, parameter change */
        int a1 = d[3], a2 = d[4], a3 = d[5], v = d[6];
        if (a1 == 0x00 && a2 == 0x00 && a3 == 0x7E) reset_channels(m, MODE_XG, frame);   /* XG System On */
        else if (a1 == 0x00 && a2 == 0x00 && a3 == 0x04) set_master_gain(m, (float)v / 127.0f, frame);
        else if (a1 == 0x08 && a2 < 16 && a3 == 0x07) set_drum_part(m, a2, v != 0);   /* part mode */
    }
}

/* Applies one event. `silent`: replaying before a seek target, no notes start. */
static void apply_event(struct fa_midi* m, const midi_event* e, int silent)
{
    int ch = e->channel, key = e->key;
    switch (e->type) {
    case EV_NOTE_ON: {
        int play = key;
        if (silent) {
            /* Held at the target if no note-off comes before it (restart_held). */
            if (m->depth[ch][key] == 0 || m->held_vel[ch][key] < e->value) m->held_vel[ch][key] = (uint8_t)e->value;
            if (m->depth[ch][key] < 255) m->depth[ch][key]++;
            m->pedal_held[ch][key] = 0;
            return;
        }
        if ((m->mute_now >> ch) & 1) return;
        if (!m->drums[ch]) play += m->transpose_now;
        if (play < 0 || play > 127) return;
        if (m->depth[ch][key] == 255) return;   /* every note needs its note-off counted */
        /* The same key again while held on another pitch (the transpose changed):
         * end those first, so no note is left hanging. */
        if (m->depth[ch][key] > 0 && m->sounding[ch][key] != play) end_notes(m, ch, key);
        m->merged[ch][key] = 0;     /* a voice of its own: every note-off ends one again */
        tsf_channel_note_on(m->synth, ch, play, (float)e->value / 127.0f);
        m->sounding[ch][key] = (uint8_t)play;
        m->depth[ch][key]++;
        return;
    }
    case EV_NOTE_OFF:
        if (m->depth[ch][key] == 0) return;
        m->depth[ch][key]--;
        if (!silent && m->merged[ch][key]) m->merged[ch][key]--;   /* a restarted key: its one voice rings on */
        else if (!silent) tsf_channel_note_off(m->synth, ch, m->sounding[ch][key]);
        else if (m->depth[ch][key] == 0 && m->pedal[ch]) m->pedal_held[ch][key] = 1;
        return;
    case EV_CONTROL:
        tsf_channel_midi_control(m->synth, ch, key, e->value);
        if (key == TML_BANK_SELECT_MSB) m->bank_msb[ch] = (uint8_t)e->value;   /* takes effect with the next program */
        if (key == TML_ALL_CTRL_OFF) sync_bank(m, ch);   /* tsf clears the bank; the bank select stays (RP-015) */
        if (key == TML_SUSTAIN_SWITCH) {
            m->pedal[ch] = e->value >= 64;
            if (!m->pedal[ch]) memset(m->pedal_held[ch], 0, sizeof(m->pedal_held[ch]));
        }
        if (key == TML_ALL_SOUND_OFF || key == TML_ALL_NOTES_OFF) forget_channel(m, ch);
        return;
    case EV_PROGRAM:
        m->program[ch] = (uint8_t)e->value;
        select_program(m, ch);
        return;
    case EV_PITCH:
        tsf_channel_set_pitchwheel(m->synth, ch, e->value);
        return;
    case EV_SYSEX:
        if (e->value < m->nsysex) apply_sysex(m, m->sysex[e->value].d, m->sysex[e->value].len, e->frame);
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
    int ch, effects;
    m->transpose_now = fa_load32(&m->transpose);
    for (ch = 0; ch < 16; ch++) {
        if (!((mute & ~m->mute_now) >> ch & 1)) continue;
        tsf_channel_sounds_off_all(m->synth, ch);
        forget_channel(m, ch);
    }
    m->mute_now = mute;
    effects = fa_load32(&m->effects);
    if (m->effects_now && !effects) fa_midi_fx_reset(m->fx);   /* off: the tails stop too */
    m->effects_now = effects;
}

/* Renders the block starting at rpos (a multiple of FA_MIDI_BLOCK). */
static void render_block(struct fa_midi* m)
{
    fa_frames start = m->rpos, n = m->length - start, done = 0;
    if (n > FA_MIDI_BLOCK) n = FA_MIDI_BLOCK;
    fa_frames i;
    take_controls(m);
    if (m->effects_now) {
        memset(m->rev, 0, sizeof(m->rev));
        memset(m->cho, 0, sizeof(m->cho));
    }
    while (done < n) {
        fa_frames upto = n;
        while (m->next < m->nev && m->ev[m->next].frame <= start + done) apply_event(m, &m->ev[m->next++], 0);
        if (m->next < m->nev && m->ev[m->next].frame < start + n) upto = m->ev[m->next].frame - start;
        if (m->effects_now)
            tsf_render_float_fx(m->synth, m->blk + done * 2, m->rev + done * 2, m->cho + done * 2, (int)(upto - done), 0);
        else
            tsf_render_float(m->synth, m->blk + done * 2, (int)(upto - done), 0);
        done = upto;
    }
    /* The effects run on the whole block, so their output does not depend on
     * where events split it; master volume is applied after them, to the wet
     * sound too. */
    if (m->effects_now) fa_midi_fx_process(m->fx, m->rev, m->cho, m->blk, (int)n, start);
    for (i = 0; i < n && start + i < m->gain_at + FA_MIDI_GAIN_RAMP; i++) {   /* the ramp */
        float g = gain_at(m, start + i);
        m->blk[i * 2] *= g;
        m->blk[i * 2 + 1] *= g;
    }
    if (m->gain_to != 1.0f)
        for (; i < n; i++) { m->blk[i * 2] *= m->gain_to; m->blk[i * 2 + 1] *= m->gain_to; }
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

/* After a seek's replay: the notes held at the target start again (from their
 * attack), with the controls the API has set now. A note only the pedal holds is
 * released at once, so the pedal (restored by the replay) keeps it ringing. */
static void restart_held(struct fa_midi* m)
{
    int ch, key;
    m->transpose_now = fa_load32(&m->transpose);
    m->mute_now = (uint32_t)fa_load32(&m->mute);
    for (ch = 0; ch < 16; ch++) {
        if (m->drums[ch] || (m->mute_now >> ch) & 1) { forget_channel(m, ch); continue; }
        for (key = 0; key < 128; key++) {
            int play = key + m->transpose_now;
            int held = m->depth[ch][key];
            if (!held && !m->pedal_held[ch][key]) continue;
            if (play < 0 || play > 127) { m->depth[ch][key] = 0; continue; }
            /* One voice, at the loudest of the strikes still held; it ends with the
             * last of their note-offs. */
            tsf_channel_note_on(m->synth, ch, play, (float)m->held_vel[ch][key] / 127.0f);
            m->sounding[ch][key] = (uint8_t)play;
            m->merged[ch][key] = (uint8_t)(held > 1 ? held - 1 : 0);
            if (!held) tsf_channel_note_off(m->synth, ch, play);
        }
    }
}

static fa_result midi_seek(fa_decoder* dec, fa_frames frame)
{
    struct fa_midi* m = (struct fa_midi*)dec->impl;
    fa_frames block = frame - frame % FA_MIDI_BLOCK;
    tsf* fresh = synth_new(m->font, m->rate);
    if (!fresh) return FA_ERROR_OUT_OF_MEMORY;
    synth_free(m->font, m->synth);
    m->synth = fresh;
    fa_midi_fx_reset(m->fx);   /* the reverb tail of what came before is not heard */
    start_song(m);   /* also forgets every held note */
    /* Everything before the target's block, without sound; what is at its first
     * frame is applied by render_block, as when playing through. */
    for (m->next = 0; m->next < m->nev && m->ev[m->next].frame < block; m->next++) apply_event(m, &m->ev[m->next], 1);
    restart_held(m);
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
    fa_midi_fx_free(m->fx);
    fa_soundfont_release(m->font);
    for (i = 0; i < m->ntexts; i++) free(m->texts[i].text);
    free(m->texts);
    free(m->text_frames);
    free(m->ev);
    free(m->sysex);
    free(m->marks);
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

/* Tempo and signature events as marks: their numbers and a readable text ("120
 * bpm", "3/4", "E minor"). 0: not one of them, or malformed (left out). */
static int mark_of(const tml_message* msg, fa_midi_mark* k)
{
    static const char* major[15] = { "Cb", "Gb", "Db", "Ab", "Eb", "Bb", "F", "C", "G", "D", "A", "E", "B", "F#", "C#" };
    static const char* minor[15] = { "Ab", "Eb", "Bb", "F", "C", "G", "D", "A", "E", "B", "F#", "C#", "G#", "D#", "A#" };
    const uint8_t* d = (const uint8_t*)msg->text;
    memset(k, 0, sizeof(*k));
    k->track = msg->track;
    if (msg->type == TML_SET_TEMPO) {
        int us = tml_get_tempo_value((tml_message*)msg);
        double bpm;
        if (us <= 0) return 0;
        bpm = 60000000.0 / us;
        k->type = FA_MIDI_MARK_TEMPO;
        k->value = us;
        if (fabs(bpm - floor(bpm + 0.5)) < 0.005) snprintf(k->text, sizeof(k->text), "%.0f bpm", bpm);
        else snprintf(k->text, sizeof(k->text), "%.2f bpm", bpm);
    } else if (msg->type == TML_TIME_SIGNATURE) {
        if (!d || msg->text_length < 2 || d[1] > 6 || d[0] == 0) return 0;
        k->type = FA_MIDI_MARK_TIME_SIGNATURE;
        k->value = d[0];
        k->value2 = 1 << d[1];
        snprintf(k->text, sizeof(k->text), "%d/%d", k->value, k->value2);
    } else if (msg->type == TML_KEY_SIGNATURE) {
        int sf;
        if (!d || msg->text_length < 2 || d[1] > 1) return 0;
        sf = (int)(int8_t)d[0];
        if (sf < -7 || sf > 7) return 0;
        k->type = FA_MIDI_MARK_KEY_SIGNATURE;
        k->value = sf;
        k->value2 = d[1];
        snprintf(k->text, sizeof(k->text), "%s %s", d[1] ? minor[sf + 7] : major[sf + 7], d[1] ? "minor" : "major");
    } else {
        return 0;
    }
    return 1;
}

static fa_result add_mark(struct fa_midi* m, const fa_midi_mark* k)
{
    if (m->nmarks == m->marks_cap) {
        uint32_t ncap = m->marks_cap ? m->marks_cap * 2 : 16;
        fa_midi_mark* grown = (fa_midi_mark*)realloc(m->marks, ncap * sizeof(fa_midi_mark));
        if (!grown) return FA_ERROR_OUT_OF_MEMORY;
        m->marks = grown;
        m->marks_cap = ncap;
    }
    m->marks[m->nmarks++] = *k;
    return FA_OK;
}

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
    uint32_t cap = 0, i, pending = 0, nsysex = 0;
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
        if (msg->type == TML_SYSEX && msg->text && nsysex < 65535) { nev++; nsysex++; }
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
    if (nsysex) {
        m->sysex = (midi_sysex*)malloc(nsysex * sizeof(midi_sysex));
        if (!m->sysex) return FA_ERROR_OUT_OF_MEMORY;
    }
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
        case TML_SYSEX:
            if (msg->text && m->nsysex < nsysex) {
                midi_event* e = &m->ev[k++];
                midi_sysex* x = &m->sysex[m->nsysex];
                x->len = (uint8_t)(msg->text_length < FA_MIDI_SYSEX_MAX ? msg->text_length : FA_MIDI_SYSEX_MAX);
                memcpy(x->d, msg->text, x->len);
                e->frame = frame;
                e->type = EV_SYSEX;
                e->channel = e->key = 0;
                e->value = (uint16_t)m->nsysex++;
            }
            break;
        case TML_SET_TEMPO: case TML_TIME_SIGNATURE: case TML_KEY_SIGNATURE: {
            fa_midi_mark mark;
            /* SMPTE time division: the times don't follow the tempo, so it means nothing. */
            if (msg->type == TML_SET_TEMPO && (smf[12] & 0x80)) break;
            if (mark_of(msg, &mark)) {
                mark.frame = frame;
                mark.time = (fa_time)((double)frame * 1e9 / rate + 0.5);
                r = add_mark(m, &mark);
            }
            break;
        }
        default:
            break;   /* end of track (its time counts) */
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
        m->fx = fa_midi_fx_new(m->rate);
        fa_store32(&m->effects, 1);
        m->effects_now = 1;
        if (!m->synth || !m->fx) r = FA_ERROR_OUT_OF_MEMORY;
        else start_song(m);
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

FA_API fa_result FA_CALL fa_stream_get_midi_marks(fa_stream* stream, fa_midi_mark* out_marks, uint32_t capacity, uint32_t* out_count)
{
    uint32_t n;
    if (out_count) *out_count = 0;
    if (!stream || !out_count || !stream->midi || (capacity && !out_marks)) return FA_ERROR_INVALID_ARG;
    n = stream->midi->nmarks < capacity ? stream->midi->nmarks : capacity;
    if (n) memcpy(out_marks, stream->midi->marks, n * sizeof(fa_midi_mark));
    *out_count = stream->midi->nmarks;
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

FA_API fa_result FA_CALL fa_stream_set_midi_effects(fa_stream* stream, int32_t enabled)
{
    if (!stream || !stream->midi) return FA_ERROR_INVALID_ARG;
    fa_store32(&stream->midi->effects, enabled ? 1 : 0);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_midi_effects(fa_stream* stream, int32_t* out_enabled)
{
    if (!stream || !out_enabled || !stream->midi) return FA_ERROR_INVALID_ARG;
    *out_enabled = fa_load32(&stream->midi->effects);
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
