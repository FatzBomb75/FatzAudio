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
 * With effects on, the replay stops FA_MIDI_PREROLL_S before the target instead
 * and the synth renders from there, unheard, so the reverb and chorus tails (and
 * notes still ringing) of the time before the target are heard after it; held
 * notes restart at that earlier point.
 *
 * Controllers, pressure and the soundfont's modulators are TinySoundFont's (mod wheel
 * and pressure vibrato, CC71-78 and their part NRPNs, portamento, sostenuto, soft
 * pedal); this file passes them on, tracks sostenuto for the seek replay and resets
 * them with the GM/GS/XG resets.
 *
 * SysEx: GM, GM2, GS and XG resets, GS rhythm parts and XG drum parts, XG and GM2
 * drum banks (bank select MSB 127 and 120), master volume (universal, GS, XG),
 * master fine and coarse tuning, GS and XG reverb, chorus, delay and variation
 * settings (midi_fx.c, midi_var.c), the XG insertion effects and the GS EFX with the
 * parts' EFX switches (midi_efx.c), the GS and XG part
 * volume, pan, reverb, chorus and delay/variation sends and vibrato, filter and envelope offsets,
 * GS and XG scale tuning, GS drum maps and XG drum setups (per-key drum edits, kept
 * in TinySoundFont: tsf_set_drum_param). Other SysEx is ignored.
 *
 * Reverb and chorus (src/midi_fx.c) are fed by each voice's send levels: CC91
 * and CC93 (40 and 0 after a reset, as on GS and GM2 devices) and the
 * soundfont's effect-send generators. They are on unless the app turns them off.
 * After a GS or XG reset there is a third effect: CC94 sends to the GS delay or the
 * XG variation (system mode), or the XG variation takes one part's whole sound
 * (insertion mode). The XG insertion effects 1 and 2 (03 0n xx) take one part each and
 * the GS EFX (40 03 xx) the parts switched to it (40 4x 22): tsf renders the parts of
 * each chain of insertion blocks apart, fa_midi_fx_insert puts them through it and adds
 * them back with the chain's sends.
 *
 * Live controls (transpose, channel mute, effects) are atomics the decode thread reads at
 * the start of each block. */
#include "fa_internal.h"
#include "tsf.h"
#include "tml.h"
#include "sjis_table.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FA_MIDI_BLOCK 64                  /* frames; TinySoundFont's effect block */
#define FA_MIDI_MAX_FILE ((size_t)16 << 20)   /* the largest real MIDI files are a few MB */
#define FA_MIDI_MAX_SOUNDFONT ((size_t)1 << 30)
#define FA_MIDI_MAX_SECONDS (6.0 * 3600.0)  /* events later than this are dropped */
#define FA_MIDI_TAIL_S 1.0                /* after the last event, when not measured (tags without a soundfont) */
#define FA_MIDI_TAIL_MAX_S 10.0           /* the longest tail measured after the last event (notes never ended) */
#define FA_MIDI_TAIL_QUIET_S 1.0          /* this long under FA_MIDI_TAIL_FLOOR ends the measured tail */
#define FA_MIDI_TAIL_FLOOR 1.0e-4f        /* -80 dBFS: the song has died away */
#define FA_MIDI_VOICES 256                /* polyphony per stream (oldest released voices go first) */
#define FA_MIDI_GAIN_DB 0.0f
#define FA_MIDI_DRUMS 9                   /* channel 10 */
#define FA_MIDI_TRANSPOSE_MAX 48
#define FA_MIDI_SYSEX_MAX 64              /* longer SysEx (sample dumps, bulk data) is dropped by the parser */
#define FA_MIDI_GAIN_RAMP 64              /* frames a master volume change glides over */
#define FA_MIDI_PREROLL_S 1.0             /* rendered unheard before a seek target (effects on) */

/* ---- Soundfonts ---------------------------------------------------------------- */

struct fa_soundfont {
    fa_atomic32 refs;
    fa_mutex lock;              /* tsf_copy and tsf_close of copies share a plain counter */
    tsf* bank;                  /* the loaded font; never plays itself, streams play copies */
    uint64_t key;
    uint64_t sample_memory;
    uint32_t presets;
    char name[256];
    fa_soundfont** members;     /* a stack: the soundfonts its presets play from (references) */
    uint32_t nmembers;
};

/* The bank's name: RIFF sfbk > LIST INFO > INAM. Also the size of the sample data
 * (smpl in LIST sdta), kept as 16 bits. */
static void sf2_scan(const uint8_t* d, size_t n, char* name, size_t cap, uint64_t* sample_memory)
{
    size_t pos = 12;
    name[0] = '\0';
    *sample_memory = 0;
    if (n < 12) return;
    while (pos + 8 <= n) {
        uint32_t size = fa_le32(d + pos + 4);
        size_t body = pos + 8, end;
        if (size > n - body) size = (uint32_t)(n - body);
        end = body + size;
        if (memcmp(d + pos, "LIST", 4) == 0 && size >= 4) {
            size_t sub = body + 4;
            int info = memcmp(d + body, "INFO", 4) == 0, sdta = memcmp(d + body, "sdta", 4) == 0;
            while (sub + 8 <= end) {
                uint32_t len = fa_le32(d + sub + 4);
                if (len > end - sub - 8) len = (uint32_t)(end - sub - 8);
                if (info && memcmp(d + sub, "INAM", 4) == 0) {
                    size_t k = 0;
                    while (k < len && k + 1 < cap && d[sub + 8 + k]) { name[k] = (char)d[sub + 8 + k]; k++; }
                    name[k] = '\0';
                }
                if (sdta && memcmp(d + sub, "smpl", 4) == 0) *sample_memory = (uint64_t)(len / 2) * sizeof(int16_t);
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
    /* The last reference: every stream's copy has been closed already. A stack's
     * members go after it (it plays their regions and samples). */
    tsf_close(f->bank);
    while (f->nmembers) fa_soundfont_release(f->members[--f->nmembers]);
    free(f->members);
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

FA_API fa_result FA_CALL fa_soundfont_get_preset(fa_soundfont* font, uint32_t index, fa_soundfont_preset* out_preset)
{
    int bank = 0, program = 0;
    char raw[24];
    if (out_preset) memset(out_preset, 0, sizeof(*out_preset));
    if (!font || !out_preset || index >= font->presets || !tsf_get_preset_bank_number(font->bank, (int)index, &bank, &program))
        return FA_ERROR_INVALID_ARG;
    out_preset->bank = bank;
    out_preset->program = program;
    snprintf(raw, sizeof(raw), "%s", tsf_get_presetname(font->bank, (int)index));
    latin1_to_utf8(raw, out_preset->name, sizeof(out_preset->name));
    return FA_OK;
}

/* ---- Stacks ---------------------------------------------------------------------- */

FA_API void FA_CALL fa_soundfont_stack_entry_init(fa_soundfont_stack_entry* entry, fa_soundfont* font)
{
    if (!entry) return;
    memset(entry, 0, sizeof(*entry));
    entry->font = font;
    entry->bank = entry->program = entry->to_bank = entry->to_program = FA_SOUNDFONT_ALL;
    entry->volume = 1.0f;
}

typedef struct stack_pick {
    struct tsf_stack_preset p;
    uint32_t order;             /* entry, then the font's preset index: lower wins */
} stack_pick;

static int stack_pick_compare(const void* a, const void* b)
{
    const stack_pick *x = (const stack_pick*)a, *y = (const stack_pick*)b;
    if (x->p.bank != y->p.bank) return x->p.bank < y->p.bank ? -1 : 1;
    if (x->p.preset != y->p.preset) return x->p.preset < y->p.preset ? -1 : 1;
    return x->order < y->order ? -1 : x->order > y->order;
}

static int stack_number_ok(int32_t v, int32_t max) { return v == FA_SOUNDFONT_ALL || (v >= 0 && v <= max); }

FA_API fa_result FA_CALL fa_soundfont_open_stack(const fa_soundfont_stack_entry* entries, uint32_t count, fa_soundfont** out_font)
{
    stack_pick* picks = NULL;
    struct tsf_stack_preset* list = NULL;
    fa_soundfont* f = NULL;
    size_t total = 0, n = 0, kept = 0, i;
    uint32_t e;
    fa_result r = FA_ERROR_OUT_OF_MEMORY;
    if (out_font) *out_font = NULL;
    if (!entries || count == 0 || count > 1024 || !out_font) return FA_ERROR_INVALID_ARG;
    for (e = 0; e < count; e++) {
        const fa_soundfont_stack_entry* x = &entries[e];
        if (!x->font || !stack_number_ok(x->bank, 16383) || !stack_number_ok(x->program, 127) ||
            !stack_number_ok(x->to_bank, 16383) || !stack_number_ok(x->to_program, 127) ||
            !(x->volume >= 0.0f && x->volume <= 16.0f))
            return FA_ERROR_INVALID_ARG;
        total += x->font->presets;
    }
    picks = (stack_pick*)malloc((total ? total : 1) * sizeof(*picks));
    if (!picks) return FA_ERROR_OUT_OF_MEMORY;
    for (e = 0; e < count; e++) {
        const fa_soundfont_stack_entry* x = &entries[e];
        float gain_db = x->volume > 0.0f ? 20.0f * log10f(x->volume) : -200.0f;
        for (i = 0; i < x->font->presets; i++) {
            int bank = 0, program = 0;
            stack_pick* k;
            if (!tsf_get_preset_bank_number(x->font->bank, (int)i, &bank, &program)) continue;
            if ((x->bank != FA_SOUNDFONT_ALL && bank != x->bank) || (x->program != FA_SOUNDFONT_ALL && program != x->program)) continue;
            k = &picks[n++];
            k->p.font = x->font->bank;
            k->p.presetIndex = (int)i;
            k->p.bank = x->to_bank != FA_SOUNDFONT_ALL ? x->to_bank : bank;
            k->p.preset = x->to_program != FA_SOUNDFONT_ALL ? x->to_program : program;
            k->p.gainDB = gain_db;
            k->order = (uint32_t)n;   /* entries in order, each font's presets in order */
        }
    }
    if (n == 0) { r = FA_ERROR_NOT_FOUND; goto done; }
    /* By bank and program, the winner first; the others with the same numbers go. */
    qsort(picks, n, sizeof(*picks), stack_pick_compare);
    list = (struct tsf_stack_preset*)malloc(n * sizeof(*list));
    f = (fa_soundfont*)calloc(1, sizeof(*f));
    if (!list || !f) goto done;
    for (i = 0; i < n; i++)
        if (i == 0 || picks[i].p.bank != picks[i - 1].p.bank || picks[i].p.preset != picks[i - 1].p.preset) list[kept++] = picks[i].p;
    f->members = (fa_soundfont**)malloc(count * sizeof(*f->members));
    if (!f->members) goto done;
    if (fa_mutex_init(&f->lock) != 0) { free(f->members); f->members = NULL; r = FA_ERROR_GENERIC; goto done; }
    f->bank = tsf_load_stack(list, (int)kept);
    if (!f->bank) { fa_mutex_destroy(&f->lock); free(f->members); f->members = NULL; goto done; }
    f->presets = (uint32_t)kept;
    f->key = 0xcbf29ce484222325ULL;
    for (e = 0; e < count; e++) {
        const fa_soundfont_stack_entry* x = &entries[e];
        uint32_t j;
        int32_t nums[5];
        int seen = 0;
        for (j = 0; j < f->nmembers; j++) seen |= f->members[j] == x->font;
        if (!seen) f->sample_memory += x->font->sample_memory;
        f->members[f->nmembers++] = fa_soundfont_retain(x->font);
        /* The key (loudness caches): each member's, then how it is used. */
        nums[0] = x->bank; nums[1] = x->program; nums[2] = x->to_bank; nums[3] = x->to_program;
        memcpy(&nums[4], &x->volume, sizeof(float));
        f->key = fa_fnv1a(f->key, (const uint8_t*)&x->font->key, sizeof(x->font->key));
        f->key = fa_fnv1a(f->key, (const uint8_t*)nums, sizeof(nums));
    }
    snprintf(f->name, sizeof(f->name), "%s", entries[0].font->name);
    fa_store32(&f->refs, 1);
    *out_font = f;
    f = NULL;
    r = FA_OK;
done:
    free(f);
    free(list);
    free(picks);
    return r;
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

/* How a file's text is read (detected once per file, or forced by the app). AUTO
 * keeps each event that is valid UTF-8 and reads the others as Windows-1252, as
 * files of mixed origin need. */
enum { CONV_AUTO, CONV_1252, CONV_SJIS };

/* Shift-JIS (code page 932) double-byte character, 0 if unassigned. */
static uint32_t sjis_char(uint32_t lead, uint32_t trail)
{
    uint8_t row = sjis_row[lead & 0xFF];
    if (row == 0xFF) return 0;
    if (trail >= 0x40 && trail <= 0x7E) return sjis_map[row][trail - 0x40];
    if (trail >= 0x80 && trail <= 0xFC) return sjis_map[row][trail - 0x80 + 63];
    return 0;
}

/* A lead and trail byte at s[i] that make one Shift-JIS character, mapped or not
 * (an unmapped one, such as a user-defined character, is still two bytes). */
static int sjis_pair(const uint8_t* s, size_t n, size_t i)
{
    uint32_t c = s[i], t = i + 1 < n ? s[i + 1] : 0;
    return ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) &&
           ((t >= 0x40 && t <= 0x7E) || (t >= 0x80 && t <= 0xFC));
}

/* Text as UTF-8 into a malloc'ed string, read as `conv`. A UTF-8 byte order mark
 * is dropped, and so are control characters other than CR, LF and tab. */
static char* text_utf8_as(const char* in, size_t n, int conv)
{
    const uint8_t* s = (const uint8_t*)in;
    int utf8;
    char* out;
    size_t i, k = 0;
    if (n >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF) { s += 3; n -= 3; }
    utf8 = conv == CONV_AUTO && fa_utf8_valid(s, n);
    out = (char*)malloc(n * 3 + 1);
    if (!out) return NULL;
    for (i = 0; i < n; i++) {
        uint32_t c = s[i];
        if (c == 0) break;
        if (c < 0x20 && c != '\r' && c != '\n' && c != '\t') continue;
        if (c < 0x80 || utf8) { out[k++] = (char)c; continue; }
        if (conv == CONV_SJIS) {
            if (c >= 0xA1 && c <= 0xDF) c = 0xFF61 + (c - 0xA1);       /* half-width katakana */
            else if (sjis_pair(s, n, i)) { c = sjis_char(c, s[i + 1]); if (!c) c = 0xFFFD; i++; }
            else c = 0xFFFD;
        } else if (c < 0xA0) {
            c = cp1252[c - 0x80];
        }
        k += fa_put_utf8(out + k, c);
    }
    out[k] = '\0';
    return out;
}

static char* text_utf8(const char* in, size_t n) { return text_utf8_as(in, n, CONV_AUTO); }

/* Counts, for one text event read as Shift-JIS: double-byte characters, those in the
 * rows of kana and Japanese punctuation (81-83, most of any Japanese lyric), and
 * bytes that cannot be Shift-JIS. A lead byte that ends the event is not counted:
 * files often cut a character between two syllable events. */
static void sjis_score(const uint8_t* s, size_t n, uint32_t* pairs, uint32_t* kana, uint32_t* bad)
{
    size_t i;
    for (i = 0; i < n; i++) {
        uint32_t c = s[i];
        if (c < 0x80 || (c >= 0xA1 && c <= 0xDF)) continue;
        if (sjis_pair(s, n, i)) {
            (*pairs)++;
            if (c >= 0x81 && c <= 0x83 && sjis_char(c, s[i + 1])) (*kana)++;
            i++;
        } else if (!(i + 1 == n && ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)))) {
            (*bad)++;
        }
    }
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

static void latin1_to_utf8(const char* in, char* out, size_t cap)
{
    char* u = text_utf8(in, strlen(in));
    copy_utf8(out, cap, u ? u : "");
    free(u);
}

typedef struct midi_text {
    fa_frames frame;
    int32_t type;               /* fa_midi_text_type */
    uint32_t flags;
    int32_t track;
    char* text;                 /* UTF-8 */
} midi_text;

/* ---- The decoder ---------------------------------------------------------------- */

enum { EV_NOTE_OFF, EV_NOTE_ON, EV_CONTROL, EV_PROGRAM, EV_PITCH, EV_SYSEX, EV_CHANNEL_PRESSURE, EV_KEY_PRESSURE };

#define FA_MIDI_NO_TRACK 0xFFFF   /* sent events: no track volume */

typedef struct midi_event {
    fa_frames frame;
    uint16_t value;             /* velocity, controller value, program, pitch bend, pressure; SysEx: index in sysex */
    uint16_t track;             /* MTrk chunk, FA_MIDI_NO_TRACK for sent events */
    uint8_t type, channel, key; /* key: note or controller number */
} midi_event;

typedef struct midi_sysex {
    uint8_t len;                    /* after the F0 */
    uint8_t d[1 + FA_MIDI_SYSEX_MAX];   /* the F0, then up to and with the F7 */
} midi_sysex;

/* An event the app sent (fa_stream_send_midi_events): queued until the synth takes it. */
typedef struct live_event {
    midi_event e;               /* track FA_MIDI_NO_TRACK; frame FA_MIDI_NOW or a source frame */
    uint8_t* sysex;             /* SysEx: the message after its F0 (malloc'd), `size` bytes */
    uint32_t size;
} live_event;

/* What the file's resets made of the synth. */
enum { MODE_GM, MODE_GM2, MODE_GS, MODE_XG };

struct fa_midi {
    int smpte;                  /* SMPTE time division, or no file: no tempo, so no beat grid */
    fa_soundfont* font;         /* a reference */
    tsf* synth;
    uint32_t rate;
    fa_frames length;
    fa_frames last_event;       /* the time of the last event (end of track included) */

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
    fa_midi_credits credits;    /* struct_size unset: the API sets the caller's */
    int32_t text_code_page;     /* forced by the app (FA_CODE_PAGE_*), 0 = detect */

    /* Rendering: blk holds frames [rpos - blk_len, rpos); blk_off of them were read. */
    float blk[FA_MIDI_BLOCK * 2];
    float rev[FA_MIDI_BLOCK * 2], cho[FA_MIDI_BLOCK * 2];   /* the block's reverb and chorus sends */
    float var[FA_MIDI_BLOCK * 2];                            /* CC94 sends */
    float ins[FA_MIDI_CHAINS][FA_MIDI_BLOCK * 2];             /* the dry sound of the parts with each insertion chain */
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
    uint8_t drum_kit[16];       /* the drum key edits a drum part plays with: GS drum map, XG drum setup (0..3) */
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
    /* Sostenuto (CC66): on, and the keys that were down when it was pressed (it holds
     * those, and only those, after their note-offs; pedal_held then marks them too). */
    uint8_t sost[16];
    uint8_t sost_latched[16][128];
    /* A CC84 (portamento control: the key the next note glides from) met while
     * replaying before a seek and not yet used by a note there; -1 none. */
    int16_t porta_ctrl[16];

    uint16_t held_track[16][128];   /* replaying: the track of the loudest strike (its volume) */

    fa_atomic32 transpose;      /* set by the API */
    fa_atomic32 mute;           /* bit n = channel n */
    fa_atomic32 effects;        /* reverb and chorus on (1, the default) or off */

    /* Sent events. Senders take send_lock among themselves and write the queue (FA_MIDI_QUEUE
     * entries, allocated by the first send) from qw on; the decode thread reads from qr up to
     * qw without a lock and keeps what is not due yet in `pend`, in frame order (equal
     * frames as sent), from pend_head on. */
    int realtime;               /* fa_stream_open_midi: no file, endless */
    fa_mutex send_lock;
    int send_lock_ready;
    fa_atomic_ptr queue;        /* live_event[FA_MIDI_QUEUE] */
    fa_atomic32 qw, qr;
    live_event* pend;
    size_t npend, pend_head, pend_cap;
    fa_frames sent_from;        /* a seek's target: sent events wait for it (not in the unheard pre-roll) */

    /* Track volumes (float bits, 1 = normal), one per track. */
    fa_atomic32* track_vol;
    uint32_t ntrack_vol;

    /* fa_stream_get_midi_value: each channel's controllers, program, bend and pressure as the
     * synth has them after the last block; refreshed for the channels events touched. */
    fa_atomic32 val_cc[16][128];
    fa_atomic32 val_program[16], val_bend[16], val_pressure[16];
    uint32_t dirty;             /* bit n: channel n changed since the last refresh */
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
    memset(m->sost_latched[ch], 0, sizeof(m->sost_latched[ch]));
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

/* The program in the channel's bank, else in bank 0 (drums: the standard kit).
 * A soundfont without it plays program 0 (drums: kit 0), else its first preset,
 * rather than whatever the channel played before. XG SFX kits (bank 126) come from
 * the soundfont's bank 126 (where XG soundfonts keep them), else its drum bank 126,
 * else the GS SFX kit (drum kit 56), else as any other drum kit (the standard kit). */
static void select_program(struct fa_midi* m, int ch)
{
    m->drums[ch] = (uint8_t)is_drum_channel(m, ch);
    tsf_channel_set_drum_kit(m->synth, ch, m->drums[ch] ? m->drum_kit[ch] : -1);
    if (m->mode == MODE_XG && m->bank_msb[ch] == 126) {
        int idx = tsf_get_presetindex(m->synth, 126, m->program[ch]);
        if (idx < 0) idx = tsf_get_presetindex(m->synth, 128 | 126, m->program[ch]);   /* as tsf's drum lookup has it */
        if (idx < 0) idx = tsf_get_presetindex(m->synth, 128, 56);
        if (idx >= 0) { tsf_channel_set_presetindex(m->synth, ch, idx); return; }
    }
    if (!tsf_channel_set_presetnumber(m->synth, ch, m->program[ch], m->drums[ch]) &&
        !tsf_channel_set_presetnumber(m->synth, ch, 0, m->drums[ch]))
        tsf_channel_set_presetindex(m->synth, ch, 0);
}

static void sync_bank(struct fa_midi* m, int ch);

/* Defaults, as after a GM/GS/XG reset at `frame` (and at the start: GM). */
static void reset_channels(struct fa_midi* m, int mode, fa_frames frame)
{
    int ch, cc;
    m->mode = mode;
    memset(m->gs_rhythm, 0, sizeof(m->gs_rhythm));
    memset(m->bank_msb, 0, sizeof(m->bank_msb));
    memset(m->program, 0, sizeof(m->program));
    memset(m->drum_kit, 0, sizeof(m->drum_kit));
    memset(m->porta_ctrl, 0xFF, sizeof(m->porta_ctrl));   /* -1: none */
    m->gs_rhythm[FA_MIDI_DRUMS] = mode == MODE_GM || mode == MODE_GS;   /* XG and GM2: the bank decides */
    if (mode == MODE_XG) m->bank_msb[FA_MIDI_DRUMS] = 127;
    if (mode == MODE_GM2) {
        memset(m->bank_msb, 121, sizeof(m->bank_msb));
        m->bank_msb[FA_MIDI_DRUMS] = 120;
    }
    set_master_gain(m, 1.0f, frame);
    m->tune_fine = m->tune_coarse = 0.0f;
    fa_midi_fx_defaults(m->fx, mode == MODE_GS ? FA_MIDI_FX_GS : mode == MODE_XG ? FA_MIDI_FX_XG : FA_MIDI_FX_GM);
    if (!m->synth) return;
    tsf_set_tuning(m->synth, 0.0f);
    tsf_reset_drum_kit(m->synth, -1);
    for (ch = 0; ch < 16; ch++) {
        int note;
        /* Notes end, as on a GS or XG device (and a seek after it restarts none). */
        tsf_channel_note_off_all(m->synth, ch);
        forget_channel(m, ch);
        tsf_channel_midi_control(m->synth, ch, TML_ALL_CTRL_OFF, 0);
        tsf_channel_midi_control(m->synth, ch, TML_VOLUME_MSB, 100);
        tsf_channel_set_sustain(m->synth, ch, 0);
        tsf_channel_set_pitchwheel(m->synth, ch, 8192);
        tsf_channel_midi_control(m->synth, ch, 91, 40);   /* reverb send: the GM2 / GS default */
        tsf_channel_midi_control(m->synth, ch, 93, 0);
        tsf_channel_midi_control(m->synth, ch, 94, 0);   /* delay / variation send */
        /* Sound controllers (CC71-78, the part NRPNs) back to normal, portamento time 0.
         * Channel pressure adds vibrato on GM devices only: GS, XG and GM2 default to none
         * (BASSMIDI: GS none, its own default 50 cents). */
        for (cc = 71; cc <= 78; cc++) tsf_channel_midi_control(m->synth, ch, cc, 64);
        tsf_channel_midi_control(m->synth, ch, TML_PORTAMENTO_TIME_MSB, 0);
        tsf_channel_set_pressure_vibrato(m->synth, ch, mode == MODE_GM ? 50.0f : 0.0f);
        for (note = 0; note < 12; note++) tsf_channel_set_scale_tuning(m->synth, ch, note, 0.0f);
        sync_bank(m, ch);
        m->pedal[ch] = 0;
        m->sost[ch] = 0;
        select_program(m, ch);
    }
}

/* The bank tsf looks presets up in: bank select MSB alone, as tsf takes it. */
static void sync_bank(struct fa_midi* m, int ch)
{
    tsf_channel_set_bank(m->synth, ch, m->bank_msb[ch] ? (0x8000 | m->bank_msb[ch]) : 0);
}

/* GS rhythm part or XG part mode, and the drum map or setup a drum part uses. In XG
 * and GM2 the bank decides (XG: bank select 127 makes a drum part, as the part mode
 * does), so there it sets the bank. */
static void set_drum_part(struct fa_midi* m, int ch, int drum, int kit)
{
    m->drum_kit[ch] = (uint8_t)kit;
    if (m->mode == MODE_XG || m->mode == MODE_GM2) {
        m->bank_msb[ch] = (uint8_t)(m->mode == MODE_XG ? (drum ? 127 : 0) : (drum ? 120 : 121));
        sync_bank(m, ch);
    } else {
        m->gs_rhythm[ch] = (uint8_t)drum;
    }
    select_program(m, ch);
}

/* A part's sends, level and pan, as the controllers set them (pan 0 is "random"
 * on GS and XG devices: the middle here). */
static void part_control(struct fa_midi* m, int ch, int cc, int v)
{
    if (cc == TML_PAN_MSB && v == 0) v = 64;
    tsf_channel_midi_control(m->synth, ch, cc, v);
}

/* The MIDI channel of GS part block `block` (40 1x, 40 4x): 0 is part 10, 1-9 parts 1-9, A-F parts 11-16. */
static int gs_block_channel(int block) { return block == 0 ? 9 : block <= 9 ? block - 1 : block; }

/* GS part parameters 40 1x `a`. Block 0 is part 10, 1..9 parts 1..9, A..F 11..16.
 * 30-37 (vibrato, filter, envelope offsets, 64 = normal) are what the sound controllers
 * CC71-78 set. */
static void gs_part(struct fa_midi* m, int block, int a, int v)
{
    static const uint8_t sound_cc[8] = { 76, 77, 74, 71, 73, 75, 72, 78 };
    int ch = gs_block_channel(block);
    switch (a) {
    case 0x15: set_drum_part(m, ch, v != 0, v == 2); break;   /* use for rhythm part: off, map 1, map 2 */
    case 0x19: part_control(m, ch, TML_VOLUME_MSB, v); break;
    case 0x1C: part_control(m, ch, TML_PAN_MSB, v); break;
    case 0x21: part_control(m, ch, 93, v); break;      /* chorus send */
    case 0x22: part_control(m, ch, 91, v); break;      /* reverb send */
    case 0x2C: part_control(m, ch, 94, v); break;      /* delay send (SC-88) */
    default:
        if (a >= 0x30 && a <= 0x37) part_control(m, ch, sound_cc[a - 0x30], v);
        else if (a >= 0x40 && a <= 0x4B) tsf_channel_set_scale_tuning(m->synth, ch, a - 0x40, (float)(v - 64));   /* C .. B, cents */
        break;
    }
}

/* XG multi part parameters 08 `part` `a`; 15-1C as GS's 30-37 above. */
static void xg_part(struct fa_midi* m, int ch, int a, int v)
{
    static const uint8_t sound_cc[8] = { 76, 77, 78, 74, 71, 73, 75, 72 };
    switch (a) {
    case 0x07: set_drum_part(m, ch, v != 0, v >= 3 && v <= 5 ? v - 2 : 0); break;   /* part mode: normal, drum, drum setup 1..4 */
    case 0x0B: part_control(m, ch, TML_VOLUME_MSB, v); break;
    case 0x0E: part_control(m, ch, TML_PAN_MSB, v); break;
    case 0x12: part_control(m, ch, 93, v); break;      /* chorus send */
    case 0x13: part_control(m, ch, 91, v); break;      /* reverb send */
    case 0x14: part_control(m, ch, 94, v); break;      /* variation send */
    default:
        if (a >= 0x15 && a <= 0x1C) part_control(m, ch, sound_cc[a - 0x15], v);
        else if (a >= 0x41 && a <= 0x4C) tsf_channel_set_scale_tuning(m->synth, ch, a - 0x41, (float)(v - 64));   /* C .. B, cents */
        break;
    }
}

/* GS drum map parameters 41 `map``a` `key` (SC-88: two maps). The play note number is
 * the pitch the key sounds at, kept as an offset; the rest are as tsf takes them. */
static void gs_drum(struct fa_midi* m, int map, int a, int key, int v)
{
    static const int8_t param[10] = { -1, TSF_DRUM_PITCH_COARSE, TSF_DRUM_LEVEL, TSF_DRUM_GROUP, TSF_DRUM_PAN,
                                      TSF_DRUM_REVERB, TSF_DRUM_CHORUS, TSF_DRUM_NOTE_OFF, TSF_DRUM_NOTE_ON,
                                      TSF_DRUM_VARIATION };
    if (a >= 10 || param[a] < 0 || key > 127) return;   /* 0: the map's name */
    if (a == 1) { v += 64 - key; v = v < 0 ? 0 : v > 127 ? 127 : v; }
    tsf_set_drum_param(m->synth, map, key, param[a], v);
}

/* XG drum setup parameters 3`setup` `key` `a`. EG rates are turned into times (higher
 * is faster); decay 2 is what the release does to a drum note. */
static void xg_drum(struct fa_midi* m, int setup, int key, int a, int v)
{
    static const int8_t param[16] = { TSF_DRUM_PITCH_COARSE, TSF_DRUM_PITCH_FINE, TSF_DRUM_LEVEL, TSF_DRUM_GROUP,
                                      TSF_DRUM_PAN, TSF_DRUM_REVERB, TSF_DRUM_CHORUS, TSF_DRUM_VARIATION, -1, TSF_DRUM_NOTE_OFF,
                                      TSF_DRUM_NOTE_ON, TSF_DRUM_CUTOFF, TSF_DRUM_RESONANCE, TSF_DRUM_ATTACK,
                                      TSF_DRUM_DECAY, TSF_DRUM_RELEASE };
    if (a >= 16 || param[a] < 0 || key > 127) return;   /* 8: key assign */
    if (a >= 0x0D) v = v ? 128 - v : 127;
    tsf_set_drum_param(m->synth, setup, key, param[a], v);
}

/* Sostenuto pressed or released (also while replaying before a seek): the keys down
 * now are the ones it holds. On release, the keys it alone held are no longer held. */
static void set_sostenuto(struct fa_midi* m, int ch, int on)
{
    int key;
    if (on == m->sost[ch]) return;
    m->sost[ch] = (uint8_t)on;
    for (key = 0; key < 128; key++) {
        if (on) { m->sost_latched[ch][key] = m->depth[ch][key] > 0; continue; }
        if (m->sost_latched[ch][key] && !m->pedal[ch]) m->pedal_held[ch][key] = 0;
        m->sost_latched[ch][key] = 0;
    }
}

/* A key as it sounds now (transposed, drums aside); -1 if out of range. */
static int transposed_key(const struct fa_midi* m, int ch, int key)
{
    int play = key + (m->drums[ch] ? 0 : m->transpose_now);
    return play >= 0 && play <= 127 ? play : -1;
}

/* The key a poly pressure message is for, as it sounds; -1 if none. While replaying
 * before a seek, `sounding` is left from before it: the key restart_held will play. */
static int pressure_key(const struct fa_midi* m, int ch, int key, int silent)
{
    return !silent && m->depth[ch][key] > 0 ? m->sounding[ch][key] : transposed_key(m, ch, key);
}

/* The SysEx messages FatzAudio acts on (after the F0; device numbers are ignored,
 * checksums are not checked). GS and XG messages may carry several data bytes for
 * consecutive addresses. A message cut short (its F7 where a value should be) is
 * ignored: data bytes are 7-bit. */
static void apply_sysex(struct fa_midi* m, const uint8_t* d, size_t n, fa_frames frame)
{
    if (n >= 4 && d[0] == 0x7E && d[2] == 0x09) {                    /* GM System On / GM2 On */
        if (d[3] == 0x01) reset_channels(m, MODE_GM, frame);
        else if (d[3] == 0x03) reset_channels(m, MODE_GM2, frame);
    } else if (n >= 6 && d[0] == 0x7F && d[2] == 0x04) {             /* universal real-time device control */
        int v = d[4] | (d[5] << 7);
        if ((d[4] | d[5]) & 0x80) return;
        if (d[3] == 0x01) set_master_gain(m, (float)v / 16383.0f, frame);   /* master volume */
        else if (d[3] == 0x03) { m->tune_fine = ((float)v - 8192.0f) / 8192.0f; tsf_set_tuning(m->synth, m->tune_coarse + m->tune_fine); }
        else if (d[3] == 0x04) { m->tune_coarse = (float)(d[5] - 64); tsf_set_tuning(m->synth, m->tune_coarse + m->tune_fine); }
    } else if (n >= 8 && d[0] == 0x41 && d[2] == 0x42 && d[3] == 0x12) {   /* Roland GS, DT1 */
        int a1 = d[4], a2 = d[5], a3 = d[6], v = d[7];
        int end = (int)n - (d[n - 1] == 0xF7), nd = end - 8 > 1 ? end - 8 : 1, i;   /* data bytes: before the checksum (and F7) */
        for (i = 0; i < nd; i++) if (d[7 + i] & 0x80) return;
        if ((a1 == 0x40 && a2 == 0x00 && a3 == 0x7F) || (a1 == 0x00 && a2 == 0x00 && a3 == 0x7F)) {
            reset_channels(m, MODE_GS, frame);                        /* GS reset, system mode set */
        } else if (a1 == 0x40 && a2 == 0x00 && a3 == 0x04) {
            set_master_gain(m, (float)v / 127.0f, frame);
        } else if (a1 == 0x40 && a2 == 0x01) {                        /* reverb and chorus */
            for (i = 0; i < nd; i++) fa_midi_fx_gs(m->fx, a3 + i, d[7 + i]);
        } else if (a1 == 0x40 && a2 == 0x03) {                        /* insertion effect (EFX, SC-88 Pro) */
            for (i = 0; i < nd; i++) fa_midi_fx_gs_efx(m->fx, a3 + i, d[7 + i]);
        } else if (a1 == 0x40 && (a2 & 0xF0) == 0x40) {               /* part EFX switch (40 4x 22) */
            for (i = 0; i < nd; i++) if (a3 + i == 0x22) fa_midi_fx_gs_part_efx(m->fx, gs_block_channel(a2 & 0x0F), d[7 + i] != 0);
        } else if (a1 == 0x40 && (a2 & 0xF0) == 0x10) {
            for (i = 0; i < nd; i++) gs_part(m, a2 & 0x0F, a3 + i, d[7 + i]);
        } else if (a1 == 0x41 && a2 < 0x20) {                          /* drum maps: consecutive keys */
            for (i = 0; i < nd; i++) gs_drum(m, a2 >> 4, a2 & 0x0F, a3 + i, d[7 + i]);
        }
    } else if (n >= 7 && d[0] == 0x43 && (d[1] & 0xF0) == 0x10 && d[2] == 0x4C) {   /* Yamaha XG, parameter change */
        int a1 = d[3], a2 = d[4], a3 = d[5], v = d[6];
        int end = (int)n - (d[n - 1] == 0xF7), nd = end - 6 > 1 ? end - 6 : 1, i;   /* data bytes: before the F7, if any */
        for (i = 0; i < nd; i++) if (d[6 + i] & 0x80) return;
        if (a1 == 0x00 && a2 == 0x00 && a3 == 0x7E) reset_channels(m, MODE_XG, frame);   /* XG System On */
        else if (a1 == 0x00 && a2 == 0x00 && a3 == 0x04) set_master_gain(m, (float)v / 127.0f, frame);
        else if (a1 == 0x00 && a2 == 0x00 && a3 == 0x7D) tsf_reset_drum_kit(m->synth, v);   /* drum setup reset */
        else if (a1 == 0x02 && a2 == 0x01) for (i = 0; i < nd; i++) fa_midi_fx_xg(m->fx, a3 + i, d[6 + i]);   /* effects */
        else if (a1 == 0x03 && a2 < 2) for (i = 0; i < nd; i++) fa_midi_fx_xg_insertion(m->fx, a2, a3 + i, d[6 + i]);   /* insertion 1, 2 */
        else if (a1 == 0x08 && a2 < 16) for (i = 0; i < nd; i++) xg_part(m, a2, a3 + i, d[6 + i]);
        else if ((a1 & 0xF0) == 0x30 && a1 < 0x34) for (i = 0; i < nd; i++) xg_drum(m, a1 & 0x0F, a2, a3 + i, d[6 + i]);   /* drum setups */
    }
}

/* A track's volume now (1 for sent events and tracks past the file's). */
static float track_volume(const struct fa_midi* m, unsigned track)
{
    return track < m->ntrack_vol ? fa_load_float(&m->track_vol[track]) : 1.0f;
}

/* Starts a note at the track's volume (dB added to the voices' level). 0 if the track is
 * silent and nothing started. */
static int track_note_on(struct fa_midi* m, int ch, int play, int vel, unsigned track)
{
    float g = track_volume(m, track);
    if (!(g > 0.0f)) return 0;
    if (g != 1.0f) tsf_set_note_gain(m->synth, 20.0f * log10f(g));
    tsf_channel_note_on(m->synth, ch, play, (float)vel / 127.0f);
    if (g != 1.0f) tsf_set_note_gain(m->synth, 0.0f);
    return 1;
}

/* Applies one event. `silent`: replaying before a seek target, no notes start. */
static void apply_event(struct fa_midi* m, const midi_event* e, int silent)
{
    int ch = e->channel, key = e->key;
    /* Notes and key pressure change none of the values fa_stream_get_midi_value reads. */
    if (e->type == EV_SYSEX) m->dirty = 0xFFFFu;
    else if (e->type != EV_NOTE_ON && e->type != EV_NOTE_OFF && e->type != EV_KEY_PRESSURE) m->dirty |= 1u << ch;
    switch (e->type) {
    case EV_NOTE_ON: {
        int play = key;
        if (silent) {
            /* Held at the target if no note-off comes before it (restart_held). */
            if (m->depth[ch][key] == 0 || m->held_vel[ch][key] < e->value) {
                m->held_vel[ch][key] = (uint8_t)e->value;
                m->held_track[ch][key] = e->track;
            }
            if (m->depth[ch][key] < 255) m->depth[ch][key]++;
            m->pedal_held[ch][key] = 0;
            m->porta_ctrl[ch] = -1;     /* a CC84 before it was for this note */
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
        if (!track_note_on(m, ch, play, e->value, e->track)) return;   /* a silent track: no voice, no count */
        m->sounding[ch][key] = (uint8_t)play;
        m->depth[ch][key]++;
        return;
    }
    case EV_NOTE_OFF:
        if (m->depth[ch][key] == 0) return;
        m->depth[ch][key]--;
        if (!silent && m->merged[ch][key]) m->merged[ch][key]--;   /* a restarted key: its one voice rings on */
        else if (!silent) tsf_channel_note_off(m->synth, ch, m->sounding[ch][key]);
        else if (m->depth[ch][key] == 0 && (m->pedal[ch] || (m->sost[ch] && m->sost_latched[ch][key]))) m->pedal_held[ch][key] = 1;
        return;
    case EV_CONTROL:
        if (key == TML_PORTAMENTO_CTRL) {   /* names a key: transposed as the notes are */
            if (silent) m->porta_ctrl[ch] = (int16_t)e->value;   /* restart_held passes it on if unused */
            else if ((key = transposed_key(m, ch, e->value)) >= 0) tsf_channel_midi_control(m->synth, ch, TML_PORTAMENTO_CTRL, key);
            return;
        }
        tsf_channel_midi_control(m->synth, ch, key, e->value);
        if (key == TML_BANK_SELECT_MSB) m->bank_msb[ch] = (uint8_t)e->value;   /* takes effect with the next program */
        if (key == TML_ALL_CTRL_OFF) {
            sync_bank(m, ch);   /* tsf clears the bank; the bank select stays (RP-015) */
            set_sostenuto(m, ch, 0);
            m->porta_ctrl[ch] = -1;   /* tsf forgets its CC84 too */
        }
        if (key == TML_SUSTAIN_SWITCH) {
            m->pedal[ch] = e->value >= 64;
            if (!m->pedal[ch]) {   /* what sostenuto still holds stays held */
                int k;
                for (k = 0; k < 128; k++) m->pedal_held[ch][k] &= (uint8_t)(m->sost[ch] && m->sost_latched[ch][k]);
            }
        }
        if (key == TML_SOSTENUTO_SWITCH) set_sostenuto(m, ch, e->value >= 64);
        if (key == TML_ALL_SOUND_OFF || key == TML_ALL_NOTES_OFF) forget_channel(m, ch);
        return;
    case EV_CHANNEL_PRESSURE:
        tsf_channel_set_pressure(m->synth, ch, e->value);
        return;
    case EV_KEY_PRESSURE:
        if ((key = pressure_key(m, ch, key, silent)) >= 0) tsf_channel_set_key_pressure(m->synth, ch, key, e->value);
        return;
    case EV_PROGRAM:
        m->program[ch] = (uint8_t)e->value;
        select_program(m, ch);
        return;
    case EV_PITCH:
        tsf_channel_set_pitchwheel(m->synth, ch, e->value);
        return;
    case EV_SYSEX:
        if (e->value < m->nsysex) apply_sysex(m, m->sysex[e->value].d + 1, m->sysex[e->value].len, e->frame);
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

/* ---- Sent events (decode thread) ---------------------------------------------- */

/* Moves what the senders queued into `pend`, in frame order. Events due before `now`
 * (FA_MIDI_NOW, frames already rendered, or before a seek target) are given `now` (the
 * seek target while the pre-roll before it renders), so they play at once in the order
 * they were sent. */
static void take_sent(struct fa_midi* m, fa_frames now)
{
    live_event* q = (live_event*)fa_load_ptr(&m->queue);
    uint32_t r, w;
    if (now < m->sent_from) now = m->sent_from;
    if (!q) return;
    r = (uint32_t)fa_load32(&m->qr);
    w = (uint32_t)fa_load32(&m->qw);
    if (r == w) return;
    if (m->pend_head == m->npend) m->pend_head = m->npend = 0;
    for (; r != w; r++) {
        live_event ev = q[r & (FA_MIDI_QUEUE - 1)];
        size_t i;
        if (ev.e.frame < now) ev.e.frame = now;
        if (m->npend == m->pend_cap) {
            live_event* p;
            size_t cap = m->pend_cap ? m->pend_cap * 2 : 256;
            if (m->pend_head > 0) {   /* room at the front first */
                memmove(m->pend, m->pend + m->pend_head, (m->npend - m->pend_head) * sizeof(*m->pend));
                m->npend -= m->pend_head;
                m->pend_head = 0;
            }
            if (m->npend == m->pend_cap) {
                p = cap <= ((size_t)1 << 22) ? (live_event*)realloc(m->pend, cap * sizeof(*p)) : NULL;
                if (!p) { free(ev.sysex); continue; }   /* out of memory: the event is lost */
                m->pend = p;
                m->pend_cap = cap;
            }
        }
        /* Mostly sent in order: found from the end. */
        for (i = m->npend; i > m->pend_head && m->pend[i - 1].e.frame > ev.e.frame; i--) {}
        memmove(m->pend + i + 1, m->pend + i, (m->npend - i) * sizeof(*m->pend));
        m->pend[i] = ev;
        m->npend++;
    }
    fa_store32(&m->qr, (int32_t)w);
}

/* Applies the next pending event. */
static void apply_sent(struct fa_midi* m)
{
    live_event* ev = &m->pend[m->pend_head++];
    if (ev->e.type == EV_SYSEX) {
        m->dirty = 0xFFFFu;
        if (ev->sysex) apply_sysex(m, ev->sysex, ev->size, ev->e.frame);
        free(ev->sysex);
        ev->sysex = NULL;
    } else {
        apply_event(m, &ev->e, 0);
    }
}

static void free_sent(struct fa_midi* m)
{
    live_event* q = (live_event*)fa_load_ptr(&m->queue);
    size_t i;
    if (q) {
        uint32_t r, w = (uint32_t)fa_load32(&m->qw);
        for (r = (uint32_t)fa_load32(&m->qr); r != w; r++) free(q[r & (FA_MIDI_QUEUE - 1)].sysex);
        free(q);
    }
    for (i = m->pend_head; i < m->npend; i++) free(m->pend[i].sysex);
    free(m->pend);
}

/* The values fa_stream_get_midi_value reads, for the channels events changed. */
static void refresh_values(struct fa_midi* m)
{
    int ch, cc;
    for (ch = 0; ch < 16; ch++) {
        if (!(m->dirty >> ch & 1u)) continue;
        for (cc = 0; cc < 128; cc++) fa_store32(&m->val_cc[ch][cc], tsf_channel_get_midi_control(m->synth, ch, cc));
        fa_store32(&m->val_program[ch], m->program[ch]);
        fa_store32(&m->val_bend[ch], tsf_channel_get_pitchwheel(m->synth, ch));
        fa_store32(&m->val_pressure[ch], tsf_channel_get_pressure(m->synth, ch));
    }
    m->dirty = 0;
}

/* A channel's controller for the insertion effects' controls (fa_midi_fx_controls). */
static float control_of(void* user, int ch, int src)
{
    tsf* t = (tsf*)user;
    if (src == FA_MIDI_CTL_BEND) return (float)(tsf_channel_get_pitchwheel(t, ch) - 8192) / 8192.0f;
    if (src == FA_MIDI_CTL_PRESSURE) return (float)tsf_channel_get_pressure(t, ch) / 127.0f;
    return (float)tsf_channel_get_midi_control(t, ch, src) / 127.0f;
}

/* Renders the block starting at rpos (a multiple of FA_MIDI_BLOCK). */
static void render_block(struct fa_midi* m)
{
    fa_frames start = m->rpos, n = m->length - start, done = 0;
    if (n > FA_MIDI_BLOCK) n = FA_MIDI_BLOCK;
    fa_frames i;
    unsigned used = 0;          /* the insertion chains rendered into ins this block */
    int chain_part[FA_MIDI_CHAINS];   /* a part rendered into each (its sends are the chain's) */
    take_controls(m);
    take_sent(m, start);
    if (m->effects_now) {
        memset(m->rev, 0, sizeof(m->rev));
        memset(m->cho, 0, sizeof(m->cho));
        memset(m->var, 0, sizeof(m->var));
    }
    while (done < n) {
        fa_frames upto = n;
        /* The file's events first, then those sent for the same frame. */
        for (;;) {
            if (m->next < m->nev && m->ev[m->next].frame <= start + done) apply_event(m, &m->ev[m->next++], 0);
            else if (m->pend_head < m->npend && m->pend[m->pend_head].e.frame <= start + done) apply_sent(m);
            else break;
        }
        if (m->next < m->nev && m->ev[m->next].frame < start + n) upto = m->ev[m->next].frame - start;
        if (m->pend_head < m->npend && m->pend[m->pend_head].e.frame < start + upto) upto = m->pend[m->pend_head].e.frame - start;
        if (m->effects_now) {
            int route = fa_midi_fx_route(m->fx), ch, any = 0;   /* SysEx in the block may change it */
            signed char slot[16];
            float* bufs[FA_MIDI_CHAINS];
            for (ch = 0; ch < 16; ch++) {
                int c = fa_midi_fx_chain(m->fx, ch);
                slot[ch] = (signed char)(c - 1);
                if (!c) continue;
                any = 1;
                if (!(used >> (c - 1) & 1u)) memset(m->ins[c - 1], 0, sizeof(m->ins[0]));
                used |= 1u << (c - 1);
                chain_part[c - 1] = ch;
            }
            for (ch = 0; ch < FA_MIDI_CHAINS; ch++) bufs[ch] = m->ins[ch] + done * 2;
            tsf_render_float_fx(m->synth, m->blk + done * 2, m->rev + done * 2, m->cho + done * 2,
                                route == -1 ? m->var + done * 2 : NULL, any ? bufs : NULL, slot, (int)(upto - done), 0);
        } else {
            tsf_render_float(m->synth, m->blk + done * 2, (int)(upto - done), 0);
        }
        done = upto;
    }
    /* The effects run on the whole block, so their output does not depend on
     * where events split it; master volume is applied after them, to the wet
     * sound too. An insertion part goes through the variation first, then on to
     * the reverb and chorus at the part's sends (CC91, CC93). */
    if (m->effects_now) {
        int c;
        if (used) fa_midi_fx_controls(m->fx, control_of, m->synth);
        /* The GS EFX is one block and the last: a chain that ends in it (the XG variation's
         * part switched to the EFX too) goes through its other blocks into the EFX's own
         * input, so the EFX runs once on the sum. */
        for (c = 1; c <= FA_MIDI_CHAINS; c++) {
            const int e = FA_MIDI_CHAIN_EFX - 1;
            if (c == FA_MIDI_CHAIN_EFX || !(c & FA_MIDI_CHAIN_EFX) || !(used >> (c - 1) & 1u)) continue;
            if (!(used >> e & 1u)) {
                memset(m->ins[e], 0, sizeof(m->ins[0]));
                used |= 1u << e;
                chain_part[e] = chain_part[c - 1];
            }
            fa_midi_fx_insert(m->fx, c & ~FA_MIDI_CHAIN_EFX, m->ins[c - 1], m->ins[e], m->rev, m->cho, NULL, 0.0f, 0.0f, 0.0f, (int)n, start);
            used &= ~(1u << (c - 1));
        }
        for (c = 1; c <= FA_MIDI_CHAINS; c++) {
            int p = chain_part[c - 1];
            float rs, cs, vs;
            if (!(used >> (c - 1) & 1u)) continue;
            fa_midi_fx_insert_sends(m->fx, c, tsf_channel_get_midi_control(m->synth, p, 91), tsf_channel_get_midi_control(m->synth, p, 93),
                                    tsf_channel_get_midi_control(m->synth, p, 94), &rs, &cs, &vs);
            fa_midi_fx_insert(m->fx, c, m->ins[c - 1], m->blk, m->rev, m->cho, m->var, rs, cs, vs, (int)n, start);
        }
        fa_midi_fx_process(m->fx, m->rev, m->cho, m->var, m->blk, (int)n, start);
    }
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
    if (m->dirty) refresh_values(m);
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
    int ch, key, pass;
    for (ch = 0; ch < 16; ch++) {
        int porta, from = m->porta_ctrl[ch] >= 0 ? transposed_key(m, ch, m->porta_ctrl[ch]) : -1;
        m->porta_ctrl[ch] = -1;
        if (m->drums[ch] || (m->mute_now >> ch) & 1) {
            forget_channel(m, ch);
            if (from >= 0) tsf_channel_midi_control(m->synth, ch, TML_PORTAMENTO_CTRL, from);
            continue;
        }
        /* No portamento between the restarted notes. Sostenuto latches the notes it held
         * (restarted first), not the others. */
        porta = tsf_channel_get_midi_control(m->synth, ch, TML_PORTAMENTO_SWITCH);
        if (porta) tsf_channel_midi_control(m->synth, ch, TML_PORTAMENTO_SWITCH, 0);
        if (m->sost[ch]) tsf_channel_set_sostenuto(m->synth, ch, 0);
        for (pass = 0; pass < 2; pass++) {
            for (key = 0; key < 128; key++) {
                int play = key + m->transpose_now;
                int held = m->depth[ch][key];
                if (!held && !m->pedal_held[ch][key]) continue;
                if ((m->sost[ch] && m->sost_latched[ch][key]) != !pass) continue;
                if (play < 0 || play > 127) { m->depth[ch][key] = 0; continue; }
                /* One voice, at the loudest of the strikes still held; it ends with the
                 * last of their note-offs. */
                if (!track_note_on(m, ch, play, m->held_vel[ch][key], m->held_track[ch][key])) {
                    m->depth[ch][key] = 0;   /* a silent track: no voice to end */
                    m->pedal_held[ch][key] = 0;
                    continue;
                }
                m->sounding[ch][key] = (uint8_t)play;
                m->merged[ch][key] = (uint8_t)(held > 1 ? held - 1 : 0);
                if (!held && pass == 1) tsf_channel_note_off(m->synth, ch, play);
            }
            if (pass == 0 && m->sost[ch]) {
                tsf_channel_set_sostenuto(m->synth, ch, 1);
                for (key = 0; key < 128; key++) {
                    int play = key + m->transpose_now;
                    if (m->sost_latched[ch][key] && !m->depth[ch][key] && m->pedal_held[ch][key] && play >= 0 && play <= 127)
                        tsf_channel_note_off(m->synth, ch, play);
                }
            }
        }
        if (porta) tsf_channel_midi_control(m->synth, ch, TML_PORTAMENTO_SWITCH, porta);
        /* A CC84 no note used before the target is for the next note after it. */
        if (from >= 0) tsf_channel_midi_control(m->synth, ch, TML_PORTAMENTO_CTRL, from);
    }
}

static fa_result seek_to(struct fa_midi* m, fa_frames frame)
{
    fa_frames block = frame - frame % FA_MIDI_BLOCK, from = block;
    tsf* fresh;
    if (m->realtime) return FA_ERROR_NOT_SEEKABLE;
    fresh = synth_new(m->font, m->rate);
    if (!fresh) return FA_ERROR_OUT_OF_MEMORY;
    synth_free(m->font, m->synth);
    m->synth = fresh;
    fa_midi_fx_reset(m->fx);
    start_song(m);   /* also forgets every held note */
    m->dirty = 0xFFFFu;
    /* The controls the API has set now, for the replay (transposed keys) and restart_held. */
    m->transpose_now = fa_load32(&m->transpose);
    m->mute_now = (uint32_t)fa_load32(&m->mute);
    /* With effects on, the time before the target is rendered and dropped, so the
     * reverb and chorus of what was playing just before ring on into it. */
    if (fa_load32(&m->effects) && frame < m->length) {
        fa_frames k = (fa_frames)(FA_MIDI_PREROLL_S * m->rate);
        from = block - (k - k % FA_MIDI_BLOCK);
        if (from < 0) from = 0;
    }
    /* Everything before that block, without sound; what is at its first frame is
     * applied by render_block, as when playing through. */
    for (m->next = 0; m->next < m->nev && m->ev[m->next].frame < from; m->next++) apply_event(m, &m->ev[m->next], 1);
    restart_held(m);
    /* Sent events wait for the target: the pre-roll before it is rendered unheard. Pending
     * ones before it move to it (in order: they are sorted by frame). */
    m->sent_from = frame < m->length ? frame : m->length;
    {
        size_t i;
        for (i = m->pend_head; i < m->npend && m->pend[i].e.frame < m->sent_from; i++) m->pend[i].e.frame = m->sent_from;
    }
    m->rpos = from;
    m->blk_len = m->blk_off = 0;
    if (frame == 0) {
        /* The start: the first read renders it (with the controls set by then). */
    } else if (frame < m->length) {
        while (m->rpos < block) render_block(m);
        render_block(m);
        m->blk_off = (int)(frame - block);
    } else {
        m->rpos = m->length;   /* at the end: nothing more to read */
    }
    return FA_OK;
}

static fa_result midi_seek(fa_decoder* dec, fa_frames frame)
{
    return seek_to((struct fa_midi*)dec->impl, frame);
}

/* The song's length: until what the last events leave sounding (release envelopes, the
 * reverb and chorus) has died away, at most FA_MIDI_TAIL_MAX_S after the last event. The
 * end is rendered once, unheard, from a seek to the last event (which plays the second
 * before it, so the notes released there are at their real level): blocks follow until
 * FA_MIDI_TAIL_QUIET_S in a row stay under FA_MIDI_TAIL_FLOOR, and the length ends after
 * the last block above it. Then the song is put back at its start. Cost: what playing the
 * last 2 to 3 seconds costs, as a seek does. */
static fa_result measure_length(struct fa_midi* m)
{
    fa_frames last = m->last_event, limit = last + (fa_frames)(FA_MIDI_TAIL_MAX_S * m->rate), end = last, quiet = 0;
    fa_frames need = (fa_frames)(FA_MIDI_TAIL_QUIET_S * m->rate);
    fa_result r;
    m->length = limit;
    r = seek_to(m, last);
    if (r != FA_OK) return r;
    m->blk_off = 0;   /* the block with the last event is measured whole */
    for (;;) {
        float peak = 0.0f;
        int i;
        for (i = 0; i < m->blk_len * 2; i++) {
            float a = fabsf(m->blk[i]);
            if (a > peak) peak = a;
        }
        if (peak >= FA_MIDI_TAIL_FLOOR) { end = m->rpos; quiet = 0; }
        else quiet += m->blk_len;
        if (quiet >= need || m->rpos >= limit) break;
        render_block(m);
    }
    m->length = end > last ? end : last;
    r = seek_to(m, 0);
    if (r == FA_OK) refresh_values(m);   /* the values of the start, not of the end */
    return r;
}

static void midi_free(struct fa_midi* m)
{
    uint32_t i;
    if (!m) return;
    if (m->synth) synth_free(m->font, m->synth);
    fa_midi_fx_free(m->fx);
    fa_soundfont_release(m->font);
    free_sent(m);
    free(m->track_vol);
    if (m->send_lock_ready) fa_mutex_destroy(&m->send_lock);
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
    return fa_source_read_rest(src, FA_MIDI_MAX_FILE, out, out_size);
}

/* The Standard MIDI File inside `d`: the file itself, or the "data" chunk of a
 * RIFF RMID file. NULL if there is none. */
static const uint8_t* find_smf(const uint8_t* d, size_t n, size_t* out_n)
{
    size_t pos;
    if (n >= 14 && memcmp(d, "MThd", 4) == 0) { *out_n = n; return d; }
    if (n < 12 || memcmp(d, "RIFF", 4) != 0 || memcmp(d + 8, "RMID", 4) != 0) return NULL;
    for (pos = 12; pos + 8 <= n;) {
        uint32_t size = fa_le32(d + pos + 4);
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

/* CR (with or without LF) ends a line, LF alone a paragraph (RP-026). */
static uint32_t break_flags(int cr, int lf)
{
    if (lf && !cr) return FA_MIDI_NEW_PARAGRAPH | FA_MIDI_NEW_LINE;
    return cr || lf ? FA_MIDI_NEW_LINE : 0;
}

/* Chord lines and comments of karaoke files, not words to sing: Tune1000 "%..."
 * and Solton "<...>". */
static int is_chord_line(const char* t)
{
    size_t n;
    while (*t == '\r' || *t == '\n' || *t == '/' || *t == '\\') t++;
    if (*t == '%') return 1;
    if (*t != '<') return 0;
    n = strlen(t);
    while (n > 0 && (t[n - 1] == '\r' || t[n - 1] == '\n' || t[n - 1] == ' ')) n--;
    return n > 1 && t[n - 1] == '>';
}

/* Adds one lyric event as syllables. Line and paragraph marks are taken out and
 * given as flags to the syllable after them: CR and LF anywhere, "/" and "\" at the
 * start and, in karaoke files (`kar`), anywhere, where they split the event into
 * syllables of the same time ("word/next"), as do a Solton "<" at the start. Marks
 * with nothing after them go to the next event (*pending). Takes `text`. */
static fa_result add_lyric(struct fa_midi* m, uint32_t* cap, fa_frames frame, int track, char* text,
                           uint32_t* pending, int kar)
{
    size_t n = strlen(text), j = 0, from = 0;
    uint32_t flags = *pending;
    fa_result r = FA_OK;
    *pending = 0;
    while (r == FA_OK) {
        int lead = j == 0, cr = 0, lf = 0;
        uint32_t marks = 0;
        size_t k = j;
        /* A run of marks at k. */
        for (; k < n; k++) {
            char c = text[k];
            if (c == '\r') cr = 1;
            else if (c == '\n') lf = 1;
            else if (c == '/' && (lead || kar)) marks |= FA_MIDI_NEW_LINE;
            else if (c == '\\' && (lead || kar)) marks |= FA_MIDI_NEW_PARAGRAPH | FA_MIDI_NEW_LINE;
            else if (c == '<' && lead && kar) marks |= FA_MIDI_NEW_PARAGRAPH | FA_MIDI_NEW_LINE;   /* clear screen */
            else break;
        }
        if (k == j && j < n) { j++; continue; }   /* an ordinary character */
        if (j > from) {
            /* The syllable before the marks. */
            char* piece = (char*)malloc(j - from + 1);
            if (!piece) { r = FA_ERROR_OUT_OF_MEMORY; break; }
            memcpy(piece, text + from, j - from);
            piece[j - from] = '\0';
            r = add_text(m, cap, frame, FA_MIDI_LYRIC, flags, track, piece);
            if (r != FA_OK) break;
            m->nlyrics++;
            flags = 0;
        }
        flags |= break_flags(cr, lf) | marks;
        if (k >= n) { *pending = flags; break; }
        j = from = k;
        j++;   /* the first character of the next syllable */
    }
    free(text);
    return r;
}

/* ---- song credits ("@I" lines) ---- */

static int has_prefix_ci(const char* s, const char* prefix)
{
    for (; *prefix; s++, prefix++)
        if (!*s || (*s | 0x20) != (*prefix | 0x20)) return 0;
    return 1;
}

static int contains_ci(const char* s, const char* word)
{
    for (; *s; s++) if (has_prefix_ci(s, word)) return 1;
    return 0;
}

static int looks_like_copyright(const char* s)
{
    return contains_ci(s, "copyright") || contains_ci(s, "(c)") || strstr(s, "\xc2\xa9") != NULL;
}

/* Adds a line to credits.other unless it is there already (any case). */
static void add_other(fa_midi_credits* c, const char* line)
{
    size_t len = strlen(c->other), n = strlen(line);
    const char* p = c->other;
    if (!n) return;
    while (*p) {
        const char* e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l == n && has_prefix_ci(p, line)) return;
        p += l + (e ? 1 : 0);
    }
    if (len + n + 2 > sizeof(c->other)) return;
    if (len) c->other[len++] = '\n';
    memcpy(c->other + len, line, n + 1);
}

/* An "@I" line: "Label: value" into its field, else a year or a copyright by its
 * look, else credits.other. The longer label of two with the same start comes first. */
static void add_info_line(fa_midi_credits* c, const char* line)
{
    static const struct { const char* label; size_t off, cap; } labels[] = {
#define F(field) offsetof(fa_midi_credits, field), sizeof(((fa_midi_credits*)0)->field)
        { "copyright information", F(copyright) }, { "copyright", F(copyright) },
        { "performed by", F(artist) }, { "performer", F(artist) }, { "artist", F(artist) },
        { "written by", F(writer) }, { "writer", F(writer) }, { "composer", F(writer) }, { "music by", F(writer) },
        { "lyrics by", F(lyricist) }, { "lyricist", F(lyricist) }, { "lyrics", F(lyricist) },
        { "title", F(title) }, { "song", F(title) },
        { "year", F(year) }, { "date", F(year) },
        { "key", F(key) },
        { "tempo", F(tempo) }, { "bpm", F(tempo) },
        { "time signature", F(time_signature) }, { "timing", F(time_signature) }, { "meter", F(time_signature) },
#undef F
    };
    size_t i, n;
    while (*line == ' ') line++;
    /* "Words and music by", "Lyrics & music by", "Music and lyrics by": both. */
    if ((has_prefix_ci(line, "words") || has_prefix_ci(line, "lyrics") || has_prefix_ci(line, "music")) &&
        (contains_ci(line, " and ") || contains_ci(line, " & "))) {
        const char* v = line;
        for (; *v; v++)
            if (has_prefix_ci(v, " by ") || has_prefix_ci(v, ":")) break;
        if (*v && (contains_ci(line, "music") && (contains_ci(line, "lyrics") || contains_ci(line, "words")))) {
            v += *v == ':' ? 1 : 4;
            while (*v == ' ') v++;
            if (*v && !c->writer[0]) copy_utf8(c->writer, sizeof(c->writer), v);
            if (*v && !c->lyricist[0]) copy_utf8(c->lyricist, sizeof(c->lyricist), v);
            return;
        }
    }
    for (i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
        size_t l = strlen(labels[i].label);
        const char* v;
        char* field;
        if (!has_prefix_ci(line, labels[i].label)) continue;
        v = line + l;
        if (*v && *v != ' ' && *v != ':' && *v != '-' && *v != '=') continue;   /* "keyboards" is not "key" */
        if (has_prefix_ci(v, " and ") || has_prefix_ci(v, " & ")) continue;     /* "Lyrics and arrangement..." */
        while (*v == ' ' || *v == ':' || *v == '-' || *v == '=') v++;
        field = (char*)c + labels[i].off;
        if (*v && !field[0]) copy_utf8(field, labels[i].cap, v);
        return;
    }
    n = strlen(line);
    if (looks_like_copyright(line)) { if (!c->copyright[0]) copy_utf8(c->copyright, sizeof(c->copyright), line); return; }
    if (n == 4 && line[0] >= '1' && line[0] <= '2' && atoi(line) >= 1800 && atoi(line) <= 2200 &&
        line[1] >= '0' && line[1] <= '9' && line[2] >= '0' && line[2] <= '9' && line[3] >= '0' && line[3] <= '9') {
        if (!c->year[0]) copy_utf8(c->year, sizeof(c->year), line);
        return;
    }
    add_other(c, line);
}

/* Builds the event list, the texts and the info from the parsed file. */
static fa_result build(struct fa_midi* m, tml_message* first, const uint8_t* smf)
{
    tml_message* msg;
    size_t nev = 0, k = 0;
    uint32_t cap = 0, i, pending = 0, nsysex = 0;
    int words_track = -1, karaoke = 0, ntitle = 0, words_are_text, conv = CONV_AUTO, any_high = 0, all_utf8 = 1;
    uint32_t nlyric_events = 0, best_syl = 0, best_syl_track = 0, sj_pairs = 0, sj_kana = 0, sj_bad = 0;
    uint32_t* text_per_track;
    uint32_t* syl_per_track;
    fa_frames last = 0;
    fa_result r = FA_OK;
    double rate = (double)m->rate;

    m->info.format = smf[9];
    m->smpte = (smf[12] & 0x80) != 0;
    m->info.tracks = ((uint32_t)smf[10] << 8) | smf[11];
    text_per_track = (uint32_t*)calloc(m->info.tracks ? m->info.tracks : 1, 2 * sizeof(uint32_t));
    if (!text_per_track) return FA_ERROR_OUT_OF_MEMORY;
    syl_per_track = text_per_track + (m->info.tracks ? m->info.tracks : 1);

    /* Which text events are the lyrics: lyric events if the file has enough of them;
     * else, in a .kar file, the text events of its "Words" track, or of the track
     * with the most text events; else the track with the most syllable-like text
     * events (short, not "@" headers or chord lines), if it has plenty of them. */
    for (msg = first; msg; msg = msg->next) {
        if (msg->seconds > FA_MIDI_MAX_SECONDS) break;
        if (msg->type == TML_NOTE_ON || msg->type == TML_NOTE_OFF || msg->type == TML_CONTROL_CHANGE ||
            msg->type == TML_PROGRAM_CHANGE || msg->type == TML_PITCH_BEND ||
            msg->type == TML_CHANNEL_PRESSURE || msg->type == TML_KEY_PRESSURE)
            nev++;
        if (msg->type == TML_SYSEX && msg->text && nsysex < 65535) { nev++; nsysex++; }
        if (msg->type == TML_LYRIC && msg->text_length > 0 && msg->text[0] != '%') nlyric_events++;
        if (msg->type == TML_TRACK_NAME && words_track < 0 && strcmp(msg->text, "Words") == 0) words_track = msg->track;
        if (msg->type == TML_TEXT) {
            if (starts_with(msg->text, "@K") || starts_with(msg->text, "@T")) karaoke = 1;
            if (msg->text[0] != '@' && msg->track < m->info.tracks) {
                text_per_track[msg->track]++;
                if (msg->text[0] != '%' && msg->text_length <= 32) syl_per_track[msg->track]++;
            }
        }
        if (msg->text && (msg->type == TML_TEXT || msg->type == TML_LYRIC || msg->type == TML_COPYRIGHT ||
                          msg->type == TML_TRACK_NAME || msg->type == TML_INST_NAME || msg->type == TML_MARKER ||
                          msg->type == TML_CUE_POINT)) {
            const uint8_t* t = (const uint8_t*)msg->text;
            size_t n = (size_t)msg->text_length, i;
            if (n >= 3 && t[0] == 0xEF && t[1] == 0xBB && t[2] == 0xBF) { t += 3; n -= 3; }
            for (i = 0; i < n && t[i] < 0x80; i++) {}
            if (i < n) {
                any_high = 1;
                if (!fa_utf8_valid(t, n)) all_utf8 = 0;
                sjis_score(t, n, &sj_pairs, &sj_kana, &sj_bad);
            }
        }
    }
    for (i = 0; i < m->info.tracks; i++)
        if (syl_per_track[i] > best_syl) { best_syl = syl_per_track[i]; best_syl_track = i; }
    m->info.karaoke = karaoke || words_track >= 0;
    if (nlyric_events == 0 && m->info.karaoke) {
        words_are_text = 1;
        if (words_track < 0) {
            uint32_t best = 0, t;
            for (t = 0; t < m->info.tracks; t++) if (text_per_track[t] > best) { best = text_per_track[t]; words_track = (int)t; }
        }
    } else {
        /* A few stray lyric events don't make a file's lyrics. */
        words_are_text = best_syl >= 16 && nlyric_events < 8 && best_syl >= 4 * nlyric_events &&
                         best_syl * 10 >= text_per_track[best_syl_track] * 6;
        if (words_are_text) { words_track = (int)best_syl_track; m->info.karaoke = 1; }
    }
    free(text_per_track);

    /* The text encoding, for the whole file: what the app says, else UTF-8 if all of
     * it is, else Shift-JIS when nearly every byte reads as Shift-JIS (a stray byte
     * in 50 characters is let pass) and enough of it is kana (Windows-1252 accents
     * often pair up as Shift-JIS kanji, never as kana), else Windows-1252. */
    switch (m->text_code_page) {
    case FA_CODE_PAGE_UTF8: m->credits.code_page = FA_CODE_PAGE_UTF8; break;
    case FA_CODE_PAGE_WINDOWS_1252: conv = CONV_1252; m->credits.code_page = FA_CODE_PAGE_WINDOWS_1252; break;
    case FA_CODE_PAGE_SHIFT_JIS: conv = CONV_SJIS; m->credits.code_page = FA_CODE_PAGE_SHIFT_JIS; break;
    default:
        if (!any_high || all_utf8) m->credits.code_page = FA_CODE_PAGE_UTF8;
        else if (sj_bad * 50 <= sj_pairs && sj_pairs >= 2 && sj_kana * 4 >= sj_pairs) { conv = CONV_SJIS; m->credits.code_page = FA_CODE_PAGE_SHIFT_JIS; }
        else m->credits.code_page = FA_CODE_PAGE_WINDOWS_1252;
        break;
    }

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
        case TML_NOTE_ON: case TML_NOTE_OFF: case TML_CONTROL_CHANGE: case TML_PROGRAM_CHANGE: case TML_PITCH_BEND:
        case TML_CHANNEL_PRESSURE: case TML_KEY_PRESSURE: {
            midi_event* e = &m->ev[k++];
            e->frame = frame;
            e->track = msg->track;
            e->channel = msg->channel & 15;
            e->key = (uint8_t)(msg->key & 0x7F);
            e->value = (uint16_t)(msg->velocity & 0x7F);
            switch (msg->type) {
            case TML_NOTE_ON: e->type = msg->velocity ? EV_NOTE_ON : EV_NOTE_OFF; break;
            case TML_NOTE_OFF: e->type = EV_NOTE_OFF; break;
            case TML_CONTROL_CHANGE: e->type = EV_CONTROL; break;
            case TML_PROGRAM_CHANGE: e->type = EV_PROGRAM; e->value = (uint16_t)(msg->program & 0x7F); break;
            case TML_CHANNEL_PRESSURE: e->type = EV_CHANNEL_PRESSURE; e->value = (uint16_t)(msg->channel_pressure & 0x7F); break;
            case TML_KEY_PRESSURE: e->type = EV_KEY_PRESSURE; e->value = (uint16_t)(msg->key_pressure & 0x7F); break;
            default: e->type = EV_PITCH; e->value = (uint16_t)(msg->pitch_bend & 0x3FFF); break;
            }
            break;
        }
        case TML_TEXT: case TML_COPYRIGHT: case TML_TRACK_NAME: case TML_INST_NAME:
        case TML_LYRIC: case TML_MARKER: case TML_CUE_POINT: {
            int type;
            uint32_t flags = 0;
            char* text = text_utf8_as(msg->text, msg->text_length, conv);
            if (!text) { r = FA_ERROR_OUT_OF_MEMORY; break; }
            switch (msg->type) {
            case TML_LYRIC: type = words_are_text ? FA_MIDI_TEXT : FA_MIDI_LYRIC; break;
            case TML_TEXT:
                type = words_are_text && msg->track == words_track && msg->text[0] != '@' ? FA_MIDI_LYRIC : FA_MIDI_TEXT;
                break;
            case TML_MARKER: type = FA_MIDI_MARKER; break;
            case TML_CUE_POINT: type = FA_MIDI_CUE; break;
            case TML_TRACK_NAME: type = FA_MIDI_TRACK_NAME; break;
            case TML_INST_NAME: type = FA_MIDI_INSTRUMENT_NAME; break;
            default: type = FA_MIDI_COPYRIGHT; break;
            }
            if (type == FA_MIDI_LYRIC && is_chord_line(text)) type = FA_MIDI_TEXT;
            if (type == FA_MIDI_LYRIC) {
                r = add_lyric(m, &cap, frame, msg->track, text, &pending, m->info.karaoke);
                break;
            } else if (type == FA_MIDI_TEXT && starts_with(text, "@T")) {
                /* .kar header: the first @T is the title, the second the artist, a
                 * third the copyright or other information. */
                if (ntitle == 0) copy_utf8(m->info.title, sizeof(m->info.title), text + 2);
                else if (ntitle == 1) copy_utf8(m->info.artist, sizeof(m->info.artist), text + 2);
                else if (ntitle == 2 && looks_like_copyright(text + 2) && !m->credits.copyright[0])
                    copy_utf8(m->credits.copyright, sizeof(m->credits.copyright), text + 2);
                else add_other(&m->credits, text + 2);
                ntitle++;
            } else if (type == FA_MIDI_TEXT && (starts_with(text, "@I") || starts_with(text, "@i"))) {
                add_info_line(&m->credits, text + 2);
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
                x->d[0] = 0xF0;
                memcpy(x->d + 1, msg->text, x->len);
                e->frame = frame;
                e->track = msg->track;
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
    m->last_event = last;
    m->length = last + (fa_frames)(FA_MIDI_TAIL_S * rate);
    m->info.text_count = m->ntexts;
    m->info.lyric_count = m->nlyrics;
    /* Credits: the file's own events win over "@I" lines. */
    {
        fa_midi_credits* c = &m->credits;
        int have[3] = { 0, 0, 0 };
        /* "@T" lines win over "@I", which win over a track name. */
        if (m->info.title[0] && (ntitle >= 1 || !c->title[0])) copy_utf8(c->title, sizeof(c->title), m->info.title);
        if (m->info.artist[0]) copy_utf8(c->artist, sizeof(c->artist), m->info.artist);
        if (m->info.copyright[0]) copy_utf8(c->copyright, sizeof(c->copyright), m->info.copyright);
        for (i = 0; i < m->nmarks; i++) {
            const fa_midi_mark* k = &m->marks[i];
            char* field = k->type == FA_MIDI_MARK_TEMPO ? c->tempo
                        : k->type == FA_MIDI_MARK_TIME_SIGNATURE ? c->time_signature : c->key;
            if (k->type < 0 || k->type > 2 || have[k->type]) continue;
            have[k->type] = 1;
            copy_utf8(field, 32, k->text);
        }
    }
    if (m->ntexts) {
        m->text_frames = (fa_frames*)malloc(m->ntexts * sizeof(fa_frames));
        if (!m->text_frames) return FA_ERROR_OUT_OF_MEMORY;
        for (i = 0; i < m->ntexts; i++) m->text_frames[i] = m->texts[i].frame;
    }
    return FA_OK;
}

/* Reads and parses the whole file: the song's events, texts and length, no synth.
 * `no_font`: a MIDI file it is, but it cannot be played (FA_ERROR_NO_SOUNDFONT). */
static fa_result midi_parse(fa_source* src, uint32_t rate, int32_t code_page, int no_font, struct fa_midi** out, int* out_rmid)
{
    struct fa_midi* m;
    uint8_t* bytes;
    size_t size, smf_size = 0;
    const uint8_t* smf;
    tml_message* first;
    fa_result r = read_source(src, &bytes, &size);
    *out = NULL;
    if (r != FA_OK) return r;
    smf = find_smf(bytes, size, &smf_size);
    if (!smf || smf_size > (size_t)0x7FFFFFFF) { free(bytes); return FA_ERROR_UNSUPPORTED_FORMAT; }
    *out_rmid = smf != bytes;
    if (no_font) { free(bytes); return FA_ERROR_NO_SOUNDFONT; }
    first = tml_load_memory(smf, (int)smf_size);
    if (!first) { free(bytes); return FA_ERROR_UNSUPPORTED_FORMAT; }
    m = (struct fa_midi*)calloc(1, sizeof(*m));
    if (!m) { tml_free(first); free(bytes); return FA_ERROR_OUT_OF_MEMORY; }
    m->rate = rate;
    m->text_code_page = code_page;
    r = build(m, first, smf);
    tml_free(first);
    free(bytes);
    if (r != FA_OK) { midi_free(m); return r; }
    *out = m;
    return FA_OK;
}

/* The synth, effects and controls of a parsed song (or of none: a real-time stream). */
static fa_result midi_start(struct fa_midi* m, fa_soundfont* font)
{
    uint32_t i;
    m->font = fa_soundfont_retain(font);
    m->synth = synth_new(m->font, m->rate);
    m->fx = fa_midi_fx_new(m->rate);
    fa_store32(&m->effects, 1);
    m->effects_now = 1;
    if (fa_mutex_init(&m->send_lock) != 0) return FA_ERROR_GENERIC;
    m->send_lock_ready = 1;
    m->ntrack_vol = m->info.tracks;
    if (m->ntrack_vol) {
        m->track_vol = (fa_atomic32*)malloc(m->ntrack_vol * sizeof(fa_atomic32));
        if (!m->track_vol) return FA_ERROR_OUT_OF_MEMORY;
        for (i = 0; i < m->ntrack_vol; i++) fa_store_float(&m->track_vol[i], 1.0f);
    }
    if (!m->synth || !m->fx) return FA_ERROR_OUT_OF_MEMORY;
    start_song(m);
    m->dirty = 0xFFFFu;
    refresh_values(m);
    return FA_OK;
}

static void midi_publish(fa_decoder* dec, struct fa_midi* m, const char* container)
{
    dec->impl = m;
    dec->format.sample_rate = m->rate;
    dec->format.channels = 2;
    dec->length = m->realtime ? -1 : m->length;
    dec->seekable = !m->realtime;
    snprintf(dec->codec, sizeof(dec->codec), "midi");
    snprintf(dec->container, sizeof(dec->container), "%s", container);
}

static fa_result midi_open(fa_decoder* dec)
{
    struct fa_midi* m;
    int rmid = 0;
    fa_result r;
    r = midi_parse(dec->src, dec->midi_rate ? dec->midi_rate : FA_MIDI_DEFAULT_RATE, dec->text_code_page, !dec->soundfont, &m, &rmid);
    if (r != FA_OK) return r;
    r = midi_start(m, dec->soundfont);
    if (r == FA_OK) r = measure_length(m);
    if (r != FA_OK) { midi_free(m); return r; }
    midi_publish(dec, m, rmid ? "rmid" : "smf");
    return FA_OK;
}

/* The properties and title, artist and copyright of a MIDI file, read without a
 * soundfont (the decoder needs one to open). FA_OK, `info` filled (if given) and the
 * items delivered; an error if `src` is not a MIDI file. */
fa_result fa_midi_read_tags(fa_source* src, fa_stream_info* info, fa_tag_sink* sink)
{
    struct fa_midi* m;
    int rmid = 0;
    fa_result r = midi_parse(src, FA_MIDI_DEFAULT_RATE, 0, 0, &m, &rmid);
    if (r != FA_OK) return r;
    if (info) {
        fa_decoder dec;
        memset(&dec, 0, sizeof(dec));
        dec.format.sample_rate = m->rate;
        dec.format.channels = 2;
        dec.format.sample_format = FA_FORMAT_F32;
        dec.length = m->length;
        dec.seekable = 1;
        dec.track_count = 1;
        dec.audio_bytes = -1;
        dec.file_size = src->length;
        snprintf(dec.codec, sizeof(dec.codec), "midi");
        snprintf(dec.container, sizeof(dec.container), "%s", rmid ? "rmid" : "smf");
        fa_decoder_get_info(&dec, info);
    }
    if (sink) {
        if (m->info.title[0]) sink->text(sink, FA_TAG_FORMAT_MIDI, "TITLE", m->info.title);
        if (m->info.artist[0]) sink->text(sink, FA_TAG_FORMAT_MIDI, "ARTIST", m->info.artist);
        if (m->info.copyright[0]) sink->text(sink, FA_TAG_FORMAT_MIDI, "COPYRIGHT", m->info.copyright);
    }
    midi_free(m);
    return FA_OK;
}

const fa_decoder_vtable fa_decoder_midi = {
    "midi", "Standard MIDI File and RIFF MIDI (SoundFont 2 synthesizer)", "mid;midi;kar;rmi;smf",
    "audio/midi;audio/x-midi;audio/mid;audio/sp-midi",
    fa_probe_midi, midi_open, midi_read, midi_seek, midi_close, 0, NULL, NULL
};

/* ---- Real-time MIDI streams and lists of events --------------------------------- */

#define FA_MIDI_SENT_SYSEX_MAX 65536   /* bytes of one sent SysEx message */

static int midi_ready(fa_decoder* dec) { (void)dec; return 1; }   /* always has audio to give */

/* Checks an app's event and turns it into ours (SysEx not copied). */
static fa_result event_of(const fa_midi_event* in, midi_event* out)
{
    int32_t t = in->type, v = in->value, p = in->param;
    if (in->frame < 0 && in->frame != FA_MIDI_NOW) return FA_ERROR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->frame = in->frame;
    out->track = FA_MIDI_NO_TRACK;
    if (t == FA_MIDI_EVENT_SYSEX) {
        if (!in->data || in->size < 2 || in->size > FA_MIDI_SENT_SYSEX_MAX || in->data[0] != 0xF0) return FA_ERROR_INVALID_ARG;
        out->type = EV_SYSEX;
        return FA_OK;
    }
    if (in->channel < 0 || in->channel > 15 || t < FA_MIDI_EVENT_NOTE_OFF || t > FA_MIDI_EVENT_PITCH_BEND) return FA_ERROR_INVALID_ARG;
    if (t == FA_MIDI_EVENT_NOTE_OFF || t == FA_MIDI_EVENT_NOTE_ON || t == FA_MIDI_EVENT_KEY_PRESSURE || t == FA_MIDI_EVENT_CONTROL) {
        if (p < 0 || p > 127) return FA_ERROR_INVALID_ARG;
    }
    if (t == FA_MIDI_EVENT_NOTE_OFF) v = 0;
    if (v < 0 || v > (t == FA_MIDI_EVENT_PITCH_BEND ? 16383 : 127)) return FA_ERROR_INVALID_ARG;
    out->channel = (uint8_t)in->channel;
    out->key = (uint8_t)(t == FA_MIDI_EVENT_PROGRAM || t == FA_MIDI_EVENT_CHANNEL_PRESSURE || t == FA_MIDI_EVENT_PITCH_BEND ? 0 : p);
    out->value = (uint16_t)v;
    switch (t) {
    case FA_MIDI_EVENT_NOTE_OFF: out->type = EV_NOTE_OFF; break;
    case FA_MIDI_EVENT_NOTE_ON: out->type = v ? EV_NOTE_ON : EV_NOTE_OFF; break;
    case FA_MIDI_EVENT_KEY_PRESSURE: out->type = EV_KEY_PRESSURE; break;
    case FA_MIDI_EVENT_CONTROL: out->type = EV_CONTROL; break;
    case FA_MIDI_EVENT_PROGRAM: out->type = EV_PROGRAM; break;
    case FA_MIDI_EVENT_CHANNEL_PRESSURE: out->type = EV_CHANNEL_PRESSURE; break;
    default: out->type = EV_PITCH; break;
    }
    return FA_OK;
}

/* Sorts events by frame, equal frames kept in their order (a bottom-up merge sort). */
static fa_result sort_events(midi_event* ev, size_t n)
{
    size_t width, i;
    midi_event *a = ev, *b, *t;
    for (i = 1; i < n && ev[i - 1].frame <= ev[i].frame; i++) {}
    if (i >= n) return FA_OK;   /* in order already, as lists mostly are */
    b = (midi_event*)malloc(n * sizeof(*b));
    if (!b) return FA_ERROR_OUT_OF_MEMORY;
    for (width = 1; width < n; width *= 2) {
        for (i = 0; i < n; i += 2 * width) {
            size_t lo = i, mid = i + width < n ? i + width : n, hi = i + 2 * width < n ? i + 2 * width : n;
            size_t x = lo, y = mid, k = lo;
            while (x < mid && y < hi) b[k++] = a[y].frame < a[x].frame ? a[y++] : a[x++];
            while (x < mid) b[k++] = a[x++];
            while (y < hi) b[k++] = a[y++];
        }
        t = a; a = b; b = t;
    }
    if (a != ev) { memcpy(ev, a, n * sizeof(*ev)); free(a); }
    else free(b);
    return FA_OK;
}

/* A list of events as a song: like a parsed file without texts. */
static fa_result build_events(struct fa_midi* m, const fa_midi_event* events, uint32_t count)
{
    uint32_t i, k = 0, nsysex = 0, tracks = 0;
    fa_frames last = 0;
    for (i = 0; i < count; i++) if (events[i].type == FA_MIDI_EVENT_SYSEX) nsysex++;
    if (nsysex > 65535) return FA_ERROR_INVALID_ARG;
    m->ev = (midi_event*)malloc((count ? count : 1) * sizeof(midi_event));
    if (nsysex) m->sysex = (midi_sysex*)malloc(nsysex * sizeof(midi_sysex));
    if (!m->ev || (nsysex && !m->sysex)) return FA_ERROR_OUT_OF_MEMORY;
    for (i = 0; i < count; i++) {
        midi_event* e = &m->ev[k];
        fa_result r = event_of(&events[i], e);
        if (r == FA_OK && (events[i].track < 0 || events[i].track >= FA_MIDI_NO_TRACK)) r = FA_ERROR_INVALID_ARG;
        if (r != FA_OK) return r;
        if (e->frame < 0) e->frame = 0;
        e->track = (uint16_t)events[i].track;
        if ((uint32_t)e->track + 1 > tracks) tracks = (uint32_t)e->track + 1;
        if (e->type == EV_SYSEX) {
            midi_sysex* x = &m->sysex[m->nsysex];
            uint32_t n = events[i].size - 1;
            if (n > FA_MIDI_SYSEX_MAX) continue;   /* bulk data: dropped, as files' is */
            x->len = (uint8_t)n;
            memcpy(x->d, events[i].data, n + 1);
            e->value = (uint16_t)m->nsysex++;
        }
        if (e->frame > last) last = e->frame;
        k++;
    }
    m->nev = k;
    if (sort_events(m->ev, k) != FA_OK) return FA_ERROR_OUT_OF_MEMORY;
    m->info.tracks = tracks;
    m->last_event = last;
    m->length = last + (fa_frames)(FA_MIDI_TAIL_S * m->rate);
    return FA_OK;
}

fa_result fa_midi_decoder_open_events(fa_decoder* dec, fa_soundfont* font, uint32_t rate, int realtime,
                                      const fa_midi_event* events, uint32_t count)
{
    struct fa_midi* m;
    fa_result r;
    if (!font) return FA_ERROR_NO_SOUNDFONT;
    m = (struct fa_midi*)calloc(1, sizeof(*m));
    if (!m) return FA_ERROR_OUT_OF_MEMORY;
    m->rate = rate;
    m->realtime = realtime;
    m->smpte = 1;   /* no tempo events: no beat grid from the events */
    m->credits.code_page = FA_CODE_PAGE_UTF8;
    if (realtime) {
        m->length = INT64_MAX / 4;   /* endless */
        r = FA_OK;
    } else {
        r = build_events(m, events, count);
    }
    if (r == FA_OK) r = midi_start(m, font);
    if (r == FA_OK && !realtime) r = measure_length(m);
    if (r != FA_OK) { midi_free(m); return r; }
    memset(dec, 0, sizeof(*dec));
    dec->vt = &fa_decoder_midi;
    dec->anchor_pts = FA_PTS_NONE;
    if (realtime) dec->ready = midi_ready;
    midi_publish(dec, m, realtime ? "realtime" : "events");
    return FA_OK;
}

/* ---- MIDI stream API ---------------------------------------------------------- */

FA_API fa_result FA_CALL fa_stream_get_midi_info(fa_stream* stream, fa_midi_info* out_info)
{
    if (out_info) memset(out_info, 0, sizeof(*out_info));
    if (!stream || !out_info || !stream->midi) return FA_ERROR_INVALID_ARG;
    *out_info = stream->midi->info;
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_midi_credits(fa_stream* stream, fa_midi_credits* out_credits)
{
    uint32_t size;
    if (!stream || !out_credits || !stream->midi || out_credits->struct_size < sizeof(uint32_t)) return FA_ERROR_INVALID_ARG;
    size = out_credits->struct_size < sizeof(fa_midi_credits) ? out_credits->struct_size : (uint32_t)sizeof(fa_midi_credits);
    memcpy((char*)out_credits + sizeof(uint32_t), (const char*)&stream->midi->credits + sizeof(uint32_t), size - sizeof(uint32_t));
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

/* ---- Exact beat grid ---------------------------------------------------------- */

/* Beats are quarter notes, as MIDI tempos count them; a tempo change opens a
 * segment. Bars are known when the file keeps one time signature from the start,
 * or has none (4/4 then, as the standard says). */
fa_result fa_midi_exact_grid(const struct fa_midi* m, fa_grid_data* out)
{
    uint32_t i, n = 0, sigs = 0;
    int32_t num = 4, den = 4, late = 0;
    double us = 500000.0, beat = 0.0;     /* 120 bpm until the first tempo event */
    fa_frames at = 0;
    memset(out, 0, sizeof(*out));
    if (m->smpte) return FA_ERROR_NOT_FOUND;
    out->segs = (fa_tempo_segment*)malloc(((size_t)m->nmarks + 1) * sizeof(fa_tempo_segment));
    if (!out->segs) return FA_ERROR_OUT_OF_MEMORY;
    out->segs[n].start = 0;
    out->segs[n].bpm = 120.0;
    out->segs[n].beat = 0.0;
    n++;
    for (i = 0; i < m->nmarks; i++) {
        const fa_midi_mark* k = &m->marks[i];
        if (k->type == FA_MIDI_MARK_TIME_SIGNATURE) {
            if (sigs == 0 && k->frame > 0) late = 1;
            if (sigs == 0 || k->value != num || k->value2 != den) sigs++;
            num = k->value;
            den = k->value2;
            continue;
        }
        if (k->type != FA_MIDI_MARK_TEMPO || k->value <= 0) continue;
        beat += (double)(k->frame - at) / m->rate * 1e6 / us;
        if (fabs(beat - floor(beat + 0.5)) < 1e-4) beat = floor(beat + 0.5);
        at = k->frame;
        /* Under 600 µs a quarter is over FA_GRID_MAX_BPM, which fa_grid_valid refuses
         * (and with it the analysis cache, rewritten on every analysis). */
        us = k->value < 600 ? 600.0 : (double)k->value;
        if (out->segs[n - 1].start == at) n--;         /* a later event at the same time wins */
        else if (out->segs[n - 1].bpm == 60000000.0 / us) continue;   /* the same tempo again */
        out->segs[n].start = at;
        out->segs[n].bpm = 60000000.0 / us;
        out->segs[n].beat = beat;
        n++;
    }
    out->grid.struct_size = sizeof(fa_beat_grid);
    out->grid.sample_rate = m->rate;
    out->grid.first_beat = 0;
    out->grid.confidence = 1.0f;
    out->grid.flags = FA_GRID_EXACT;
    out->grid.segment_count = n;
    out->grid.beats_per_bar = 4;
    if (sigs <= 1 && !late && (num * 4) % den == 0 && num * 4 / den >= 1) {
        out->grid.beats_per_bar = (uint32_t)(num * 4 / den);
        out->grid.flags |= FA_GRID_BARS;
    }
    fa_grid_data_finish(out, m->length);
    return FA_OK;
}

/* ---- Sent events, the file's events, values and track volume -------------------- */

FA_API fa_result FA_CALL fa_stream_send_midi_events(fa_stream* stream, const fa_midi_event* events, uint32_t count)
{
    struct fa_midi* m;
    live_event* q;
    uint32_t i, w, r;
    fa_result res = FA_OK;
    if (!stream || !stream->midi || (count && !events) || count > FA_MIDI_QUEUE) return FA_ERROR_INVALID_ARG;
    for (i = 0; i < count; i++) {
        midi_event e;
        if (event_of(&events[i], &e) != FA_OK) return FA_ERROR_INVALID_ARG;
    }
    if (!count) return FA_OK;
    m = stream->midi;
    fa_mutex_lock(&m->send_lock);
    q = (live_event*)fa_load_ptr(&m->queue);
    if (!q) {
        q = (live_event*)calloc(FA_MIDI_QUEUE, sizeof(live_event));
        if (q) fa_exchange_ptr(&m->queue, q);
    }
    w = (uint32_t)fa_load32(&m->qw);
    r = (uint32_t)fa_load32(&m->qr);
    if (!q) res = FA_ERROR_OUT_OF_MEMORY;
    else if (w - r + count > FA_MIDI_QUEUE) res = FA_ERROR_BUSY;
    for (i = 0; res == FA_OK && i < count; i++) {
        live_event* x = &q[(w + i) & (FA_MIDI_QUEUE - 1)];
        event_of(&events[i], &x->e);
        x->sysex = NULL;
        x->size = 0;
        if (x->e.type == EV_SYSEX) {
            x->sysex = (uint8_t*)malloc(events[i].size - 1);
            if (!x->sysex) {
                while (i-- > 0) { free(q[(w + i) & (FA_MIDI_QUEUE - 1)].sysex); q[(w + i) & (FA_MIDI_QUEUE - 1)].sysex = NULL; }
                res = FA_ERROR_OUT_OF_MEMORY;
                break;
            }
            memcpy(x->sysex, events[i].data + 1, events[i].size - 1);
            x->size = events[i].size - 1;
        }
    }
    if (res == FA_OK) fa_store32(&m->qw, (int32_t)(w + count));
    fa_mutex_unlock(&m->send_lock);
    return res;
}

/* Data bytes of a channel message (status 0x80..0xEF). */
static int data_bytes(int status) { int k = status >> 4; return k == 0xC || k == 0xD ? 1 : 2; }

FA_API fa_result FA_CALL fa_stream_send_midi(fa_stream* stream, fa_frames frame, const void* bytes, size_t size)
{
    const uint8_t* b = (const uint8_t*)bytes;
    fa_midi_event* ev;
    size_t i = 0, n = 0;
    int status = 0;
    fa_result r;
    if (!stream || !stream->midi || (size && !bytes) || (frame < 0 && frame != FA_MIDI_NOW)) return FA_ERROR_INVALID_ARG;
    if (!size) return FA_OK;
    ev = (fa_midi_event*)calloc(size < FA_MIDI_QUEUE ? size : FA_MIDI_QUEUE, sizeof(*ev));
    if (!ev) return FA_ERROR_OUT_OF_MEMORY;
    while (i < size) {
        uint8_t c = b[i];
        fa_midi_event* e;
        if (c >= 0xF8) { i++; continue; }                     /* real-time: anywhere, ignored */
        if (c == 0xF0) {                                      /* SysEx up to its F7 */
            size_t j = i + 1;
            while (j < size && (b[j] < 0x80 || b[j] >= 0xF8)) j++;
            status = 0;
            if (j >= size || b[j] != 0xF7) { i = j; continue; }   /* cut short: dropped */
            if (n == FA_MIDI_QUEUE) { free(ev); return FA_ERROR_INVALID_ARG; }
            e = &ev[n++];
            e->frame = frame;
            e->type = FA_MIDI_EVENT_SYSEX;
            e->data = b + i;
            e->size = (uint32_t)(j + 1 - i);
            /* Real-time bytes inside it stay in it (the synth then ignores the message). */
            i = j + 1;
            continue;
        }
        if (c >= 0xF1) {                                      /* system common: skipped with its data */
            status = 0;
            i++;
            while (i < size && b[i] < 0x80) i++;
            continue;
        }
        if (c >= 0x80) { status = c; i++; }
        else if (!status) { i++; continue; }                  /* data with no status */
        {
            int k = data_bytes(status);
            uint8_t d[2] = { 0, 0 };
            int got = 0;
            while (got < k && i < size) {
                if (b[i] >= 0xF8) { i++; continue; }
                if (b[i] >= 0x80) break;
                d[got++] = b[i++];
            }
            if (got < k) continue;                            /* incomplete: dropped */
            if (n == FA_MIDI_QUEUE) { free(ev); return FA_ERROR_INVALID_ARG; }
            e = &ev[n++];
            e->frame = frame;
            e->channel = status & 15;
            switch (status >> 4) {
            case 0x8: e->type = FA_MIDI_EVENT_NOTE_OFF; e->param = d[0]; break;
            case 0x9: e->type = FA_MIDI_EVENT_NOTE_ON; e->param = d[0]; e->value = d[1]; break;
            case 0xA: e->type = FA_MIDI_EVENT_KEY_PRESSURE; e->param = d[0]; e->value = d[1]; break;
            case 0xB: e->type = FA_MIDI_EVENT_CONTROL; e->param = d[0]; e->value = d[1]; break;
            case 0xC: e->type = FA_MIDI_EVENT_PROGRAM; e->value = d[0]; break;
            case 0xD: e->type = FA_MIDI_EVENT_CHANNEL_PRESSURE; e->value = d[0]; break;
            default: e->type = FA_MIDI_EVENT_PITCH_BEND; e->value = d[0] | (d[1] << 7); break;
            }
        }
    }
    r = fa_stream_send_midi_events(stream, ev, (uint32_t)n);
    free(ev);
    return r;
}

static int public_type(int t)
{
    switch (t) {
    case EV_NOTE_OFF: return FA_MIDI_EVENT_NOTE_OFF;
    case EV_NOTE_ON: return FA_MIDI_EVENT_NOTE_ON;
    case EV_KEY_PRESSURE: return FA_MIDI_EVENT_KEY_PRESSURE;
    case EV_CONTROL: return FA_MIDI_EVENT_CONTROL;
    case EV_PROGRAM: return FA_MIDI_EVENT_PROGRAM;
    case EV_CHANNEL_PRESSURE: return FA_MIDI_EVENT_CHANNEL_PRESSURE;
    case EV_PITCH: return FA_MIDI_EVENT_PITCH_BEND;
    default: return FA_MIDI_EVENT_SYSEX;
    }
}

FA_API fa_result FA_CALL fa_stream_get_midi_events(fa_stream* stream, int32_t track, int32_t type,
                                                   fa_midi_event* out_events, uint32_t capacity, uint32_t* out_count)
{
    const struct fa_midi* m;
    size_t i;
    uint32_t n = 0;
    if (out_count) *out_count = 0;
    if (!stream || !out_count || !stream->midi || (capacity && !out_events) || track < -1 ||
        type < -1 || type > FA_MIDI_EVENT_SYSEX)
        return FA_ERROR_INVALID_ARG;
    m = stream->midi;
    for (i = 0; i < m->nev; i++) {
        const midi_event* e = &m->ev[i];
        int t = public_type(e->type);
        if ((track >= 0 && e->track != track) || (type >= 0 && t != type)) continue;
        if (n < capacity) {
            fa_midi_event* o = &out_events[n];
            memset(o, 0, sizeof(*o));
            o->frame = e->frame;
            o->time = (fa_time)((double)e->frame * 1e9 / (double)m->rate + 0.5);
            o->type = t;
            o->track = e->track;
            if (t == FA_MIDI_EVENT_SYSEX) {
                const midi_sysex* x = &m->sysex[e->value];
                o->data = x->d;
                o->size = x->len + 1u;
            } else {
                o->channel = e->channel;
                o->param = t == FA_MIDI_EVENT_PROGRAM || t == FA_MIDI_EVENT_CHANNEL_PRESSURE || t == FA_MIDI_EVENT_PITCH_BEND ? 0 : e->key;
                o->value = e->value;
            }
        }
        if (n < UINT32_MAX) n++;
    }
    *out_count = n;
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_midi_value(fa_stream* stream, uint32_t channel, int32_t type, int32_t param,
                                                  int32_t* out_value)
{
    struct fa_midi* m;
    if (out_value) *out_value = 0;
    if (!stream || !out_value || !stream->midi || channel > 15) return FA_ERROR_INVALID_ARG;
    m = stream->midi;
    switch (type) {
    case FA_MIDI_EVENT_CONTROL:
        if (param < 0 || param > 127) return FA_ERROR_INVALID_ARG;
        *out_value = fa_load32(&m->val_cc[channel][param]);
        return FA_OK;
    case FA_MIDI_EVENT_PROGRAM: *out_value = fa_load32(&m->val_program[channel]); return FA_OK;
    case FA_MIDI_EVENT_PITCH_BEND: *out_value = fa_load32(&m->val_bend[channel]); return FA_OK;
    case FA_MIDI_EVENT_CHANNEL_PRESSURE: *out_value = fa_load32(&m->val_pressure[channel]); return FA_OK;
    default: return FA_ERROR_INVALID_ARG;
    }
}

FA_API fa_result FA_CALL fa_stream_set_midi_track_volume(fa_stream* stream, uint32_t track, float volume)
{
    if (!stream || !stream->midi || track >= stream->midi->ntrack_vol || !(volume >= 0.0f && volume <= 4.0f))
        return FA_ERROR_INVALID_ARG;
    fa_store_float(&stream->midi->track_vol[track], volume);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_midi_track_volume(fa_stream* stream, uint32_t track, float* out_volume)
{
    if (out_volume) *out_volume = 0.0f;
    if (!stream || !out_volume || !stream->midi || track >= stream->midi->ntrack_vol) return FA_ERROR_INVALID_ARG;
    *out_volume = fa_load_float(&stream->midi->track_vol[track]);
    return FA_OK;
}
