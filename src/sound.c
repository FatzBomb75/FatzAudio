/* Sounds (fa_sound_*): a short sound decoded once into memory and played as many
 * overlapping voices, BASS_Sample* style.
 *
 * Every voice is an ordinary stream whose decoder reads the sound's one PCM buffer
 * (reference-counted, so a voice closed late keeps it alive), which gives each copy
 * all of a stream's features: volume, pan, buses, ramps, effects, events. The audio
 * thread sees nothing new. The pool itself lives on API threads under the sound's
 * lock: which voice a play uses, the voice limit and the steal rule.
 *
 * Lock order: sound->lock, then a voice's stream locks. fa_stream_close of a voice
 * takes sound->lock (fa_sound_forget), so the sound never closes a voice while holding
 * it: it takes the voice out of its list first, then closes it. */
#include "fa_internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FA_SOUND_DEFAULT_VOICES 8
#define FA_SOUND_MAX_BYTES ((uint64_t)1 << 30)   /* 1 GB of float PCM */

/* The decoded sound, shared by the sound and the decoders of its voices. */
typedef struct fa_sound_pcm {
    fa_atomic32 refs;
    float* data;
    fa_frames frames;
    uint32_t channels, rate;
    uint64_t channel_mask;
} fa_sound_pcm;

typedef struct fa_sound_voice {
    fa_stream* s;
    uint64_t seq;          /* when it last started (sound->seq): oldest = smallest */
} fa_sound_voice;

struct fa_sound {
    fa_engine* engine;
    fa_mutex lock;
    fa_sound_pcm* pcm;
    uint32_t max_voices;
    int32_t steal;
    fa_sound_voice* v;     /* lock: the pool */
    uint32_t count, cap;
    fa_stream** retired;   /* lock: voices over a lowered limit, stopped, kept
                              until fa_sound_close so pointers to them stay valid */
    uint32_t retired_count, retired_cap;
    uint64_t seq;
};

static fa_sound_pcm* pcm_retain(fa_sound_pcm* p)
{
    fa_fetch_add32(&p->refs, 1);
    return p;
}

static void pcm_release(fa_sound_pcm* p)
{
    if (p && fa_fetch_add32(&p->refs, -1) == 1) {
        free(p->data);
        free(p);
    }
}

/* ---- the voice decoder -------------------------------------------------------- */

typedef struct pcm_reader {
    fa_sound_pcm* pcm;
    fa_frames pos;
} pcm_reader;

static int pcm_probe(const uint8_t* data, size_t size, size_t id3_size)
{
    (void)data; (void)size; (void)id3_size;
    return 0;   /* never detected from content */
}

static fa_result pcm_open(fa_decoder* dec) { (void)dec; return FA_ERROR_UNSUPPORTED_FORMAT; }

static fa_result pcm_read(fa_decoder* dec, float* out, fa_frames frames, fa_frames* got)
{
    pcm_reader* r = (pcm_reader*)dec->impl;
    fa_frames n = r->pcm->frames - r->pos;
    if (n > frames) n = frames;
    *got = 0;
    if (n <= 0) return FA_ERROR_END_OF_STREAM;
    memcpy(out, r->pcm->data + (size_t)r->pos * r->pcm->channels, (size_t)n * r->pcm->channels * sizeof(float));
    r->pos += n;
    *got = n;
    return FA_OK;
}

static fa_result pcm_seek(fa_decoder* dec, fa_frames frame)
{
    pcm_reader* r = (pcm_reader*)dec->impl;
    if (frame < 0) return FA_ERROR_INVALID_ARG;
    r->pos = frame < r->pcm->frames ? frame : r->pcm->frames;
    return FA_OK;
}

static void pcm_close(fa_decoder* dec)
{
    pcm_reader* r = (pcm_reader*)dec->impl;
    if (!r) return;
    pcm_release(r->pcm);
    free(r);
    dec->impl = NULL;
}

static const fa_decoder_vtable pcm_vtable = {
    "sound", "A sound's PCM in memory (fa_sound)", "", "",
    pcm_probe, pcm_open, pcm_read, pcm_seek, pcm_close, 1, NULL, NULL
};

static fa_result pcm_decoder_open(fa_decoder* dec, fa_sound_pcm* pcm)
{
    pcm_reader* r = (pcm_reader*)calloc(1, sizeof(*r));
    if (!r) return FA_ERROR_OUT_OF_MEMORY;
    r->pcm = pcm_retain(pcm);
    memset(dec, 0, sizeof(*dec));
    dec->vt = &pcm_vtable;
    dec->impl = r;
    dec->format.sample_format = FA_FORMAT_F32;
    dec->format.sample_rate = pcm->rate;
    dec->format.channels = pcm->channels;
    dec->format.channel_mask = pcm->channel_mask;
    dec->length = pcm->frames;
    dec->seekable = 1;
    dec->anchor_pts = FA_PTS_NONE;
    dec->audio_bytes = -1;
    dec->file_size = -1;
    dec->bits_per_sample = 32;
    snprintf(dec->codec, sizeof(dec->codec), "pcm");
    snprintf(dec->container, sizeof(dec->container), "sound");
    return FA_OK;
}

/* ---- loading ------------------------------------------------------------------ */

FA_API void FA_CALL fa_sound_options_init(fa_sound_options* options)
{
    if (!options) return;
    memset(options, 0, sizeof(*options));
    options->struct_size = sizeof(*options);
    options->max_voices = FA_SOUND_DEFAULT_VOICES;
    options->steal = FA_SOUND_STEAL_OLDEST;
}

/* The fields of `o` this caller's struct has (struct_size), checked. */
static fa_result read_options(const fa_sound_options* o, uint32_t* max_voices, int32_t* steal,
                              const fa_open_options** open)
{
    *max_voices = FA_SOUND_DEFAULT_VOICES;
    *steal = FA_SOUND_STEAL_OLDEST;
    if (open) *open = NULL;
    if (!o) return FA_OK;
    if (o->struct_size >= offsetof(fa_sound_options, max_voices) + sizeof(o->max_voices) && o->max_voices) {
        if (o->max_voices > FA_SOUND_MAX_VOICES) return FA_ERROR_INVALID_ARG;
        *max_voices = o->max_voices;
    }
    if (o->struct_size >= offsetof(fa_sound_options, steal) + sizeof(o->steal)) {
        if (o->steal < FA_SOUND_STEAL_OLDEST || o->steal > FA_SOUND_STEAL_NONE) return FA_ERROR_INVALID_ARG;
        *steal = o->steal;
    }
    if (open && o->struct_size >= offsetof(fa_sound_options, open) + sizeof(o->open)) *open = o->open;
    return FA_OK;
}

/* Takes `pcm` (released on failure). max_voices and steal come from read_options. */
static fa_result sound_create(fa_engine* engine, fa_sound_pcm* pcm, uint32_t max_voices, int32_t steal, fa_sound** out)
{
    fa_sound* snd = (fa_sound*)calloc(1, sizeof(*snd));
    if (!snd) { pcm_release(pcm); return FA_ERROR_OUT_OF_MEMORY; }
    if (fa_mutex_init(&snd->lock) != 0) { free(snd); pcm_release(pcm); return FA_ERROR_GENERIC; }
    snd->engine = engine;
    snd->pcm = pcm;
    snd->max_voices = max_voices;
    snd->steal = steal;
    *out = snd;
    return FA_OK;
}

static fa_sound_pcm* pcm_new(uint32_t rate, uint32_t channels, uint64_t mask)
{
    fa_sound_pcm* p = (fa_sound_pcm*)calloc(1, sizeof(*p));
    if (!p) return NULL;
    fa_store32(&p->refs, 1);
    p->rate = rate;
    p->channels = channels;
    p->channel_mask = mask;
    return p;
}

/* Reads a decode-only stream to its end into a new PCM buffer. */
static fa_result decode_all(fa_stream* st, fa_sound_pcm** out)
{
    fa_stream_info info;
    fa_sound_pcm* p;
    fa_frames cap, n = 0;
    uint64_t frame_bytes;
    fa_result r = fa_stream_get_info(st, &info);
    if (r != FA_OK) return r;
    if (info.format.channels == 0 || info.format.sample_rate == 0) return FA_ERROR_UNSUPPORTED_FORMAT;
    p = pcm_new(info.format.sample_rate, info.format.channels, info.format.channel_mask);
    if (!p) return FA_ERROR_OUT_OF_MEMORY;
    frame_bytes = (uint64_t)info.format.channels * sizeof(float);
    if (info.length_frames > 0 && (uint64_t)info.length_frames * frame_bytes > FA_SOUND_MAX_BYTES) {
        pcm_release(p);
        return FA_ERROR_TOO_LARGE;
    }
    cap = info.length_frames > 0 ? info.length_frames + 1 : 65536;   /* +1: a read that finds the end */
    for (;;) {
        fa_frames got = 0;
        if (n == cap) {
            fa_frames more = cap * 2;
            float* q;
            if ((uint64_t)more * frame_bytes > FA_SOUND_MAX_BYTES) more = (fa_frames)(FA_SOUND_MAX_BYTES / frame_bytes);
            if (more <= cap) { pcm_release(p); return FA_ERROR_TOO_LARGE; }
            q = (float*)realloc(p->data, (size_t)(more * (fa_frames)frame_bytes));
            if (!q) { pcm_release(p); return FA_ERROR_OUT_OF_MEMORY; }
            p->data = q;
            cap = more;
        } else if (!p->data) {
            p->data = (float*)malloc((size_t)(cap * (fa_frames)frame_bytes));
            if (!p->data) { pcm_release(p); return FA_ERROR_OUT_OF_MEMORY; }
        }
        r = fa_stream_read(st, p->data + (size_t)n * info.format.channels, cap - n, &got);
        if (r != FA_OK && r != FA_ERROR_END_OF_STREAM) { pcm_release(p); return r; }
        if (got <= 0) break;   /* the end (a read that gives nothing is the end too) */
        n += got;
    }
    if (n == 0) { pcm_release(p); return FA_ERROR_UNSUPPORTED_FORMAT; }   /* nothing to play */
    p->frames = n;
    if (n < cap) {
        float* q = (float*)realloc(p->data, (size_t)(n * (fa_frames)frame_bytes));
        if (q) p->data = q;
    }
    *out = p;
    return FA_OK;
}

/* The app's open options (`given`, may be NULL) as a decode-only open. */
static void load_open_options(const fa_open_options* given, fa_open_options* o)
{
    fa_open_options_init(o);
    if (given) {
        size_t n = given->struct_size < sizeof(*o) ? given->struct_size : sizeof(*o);
        memcpy(o, given, n);
        o->struct_size = sizeof(*o);
    }
    o->flags = FA_OPEN_DECODE_ONLY | FA_OPEN_NO_NORMALIZE | FA_OPEN_NO_CDG;
}

/* Decodes the open decode-only stream `st` (closed here) into a new sound. */
static fa_result load_from(fa_engine* engine, fa_stream* st, uint32_t max_voices, int32_t steal, fa_sound** out)
{
    fa_sound_pcm* pcm = NULL;
    fa_result r = decode_all(st, &pcm);
    fa_stream_close(st);
    if (r != FA_OK) return r;
    return sound_create(engine, pcm, max_voices, steal, out);
}

FA_API fa_result FA_CALL fa_sound_load_path(fa_engine* engine, const char* path, const fa_sound_options* options, fa_sound** out_sound)
{
    fa_open_options o;
    const fa_open_options* given;
    fa_stream* st = NULL;
    fa_result r;
    uint32_t mv;
    int32_t steal;
    if (out_sound) *out_sound = NULL;
    if (!engine || !path || !out_sound) return FA_ERROR_INVALID_ARG;
    r = read_options(options, &mv, &steal, &given);
    if (r != FA_OK) return r;
    load_open_options(given, &o);
    r = fa_stream_open_path(engine, path, &o, &st);
    if (r != FA_OK) return r;
    return load_from(engine, st, mv, steal, out_sound);
}

FA_API fa_result FA_CALL fa_sound_load_memory(fa_engine* engine, const void* data, size_t size, const fa_sound_options* options,
                                              fa_sound** out_sound)
{
    fa_open_options o;
    const fa_open_options* given;
    fa_stream* st = NULL;
    fa_result r;
    uint32_t mv;
    int32_t steal;
    if (out_sound) *out_sound = NULL;
    if (!engine || !data || size == 0 || !out_sound) return FA_ERROR_INVALID_ARG;
    r = read_options(options, &mv, &steal, &given);
    if (r != FA_OK) return r;
    load_open_options(given, &o);
    r = fa_stream_open_memory(engine, data, size, &o, &st);
    if (r != FA_OK) return r;
    return load_from(engine, st, mv, steal, out_sound);
}

FA_API fa_result FA_CALL fa_sound_load_pcm(fa_engine* engine, const fa_audio_format* format, const void* data, fa_frames frames,
                                           const fa_sound_options* options, fa_sound** out_sound)
{
    fa_sound_pcm* p;
    int planar, kind;
    const void* planes[1];
    uint32_t mv;
    int32_t steal;
    fa_result r;
    if (out_sound) *out_sound = NULL;
    if (!engine || !format || !data || frames <= 0 || !out_sound) return FA_ERROR_INVALID_ARG;
    kind = fa_sample_kind(format->sample_format, &planar);
    if (kind < 0 || planar || format->sample_rate < 1000 || format->sample_rate > 768000 ||
        format->channels == 0 || format->channels > 32)
        return FA_ERROR_INVALID_ARG;
    r = read_options(options, &mv, &steal, NULL);
    if (r != FA_OK) return r;
    if ((uint64_t)frames > FA_SOUND_MAX_BYTES / ((uint64_t)format->channels * sizeof(float))) return FA_ERROR_TOO_LARGE;
    p = pcm_new(format->sample_rate, format->channels, format->channel_mask);
    if (!p) return FA_ERROR_OUT_OF_MEMORY;
    p->data = (float*)malloc((size_t)frames * format->channels * sizeof(float));
    if (!p->data) { pcm_release(p); return FA_ERROR_OUT_OF_MEMORY; }
    planes[0] = data;
    fa_convert_to_f32(planes, kind, 0, 0, format->channels, frames, p->data);
    p->frames = frames;
    return sound_create(engine, p, mv, steal, out_sound);
}

/* ---- voices ------------------------------------------------------------------- */

void fa_sound_forget(fa_sound* snd, fa_stream* voice)
{
    uint32_t i;
    fa_mutex_lock(&snd->lock);
    for (i = 0; i < snd->count; i++)
        if (snd->v[i].s == voice) {
            memmove(&snd->v[i], &snd->v[i + 1], (size_t)(snd->count - i - 1) * sizeof(snd->v[0]));
            snd->count--;
            break;
        }
    for (i = 0; i < snd->retired_count; i++)
        if (snd->retired[i] == voice) {
            snd->retired[i] = snd->retired[--snd->retired_count];
            break;
        }
    fa_mutex_unlock(&snd->lock);
}

/* Takes voice i out of the list (lock held), for the caller to close after unlocking. */
static fa_stream* take_voice_locked(fa_sound* snd, uint32_t i)
{
    fa_stream* s = snd->v[i].s;
    fa_exchange_ptr(&s->sound, NULL);   /* its close must not come back here */
    memmove(&snd->v[i], &snd->v[i + 1], (size_t)(snd->count - i - 1) * sizeof(snd->v[0]));
    snd->count--;
    return s;
}

FA_API void FA_CALL fa_sound_close(fa_sound* sound)
{
    fa_sound_voice* v;
    uint32_t i, n;
    if (!sound) return;
    fa_mutex_lock(&sound->lock);
    v = sound->v;
    n = sound->count;
    for (i = 0; i < n; i++) fa_exchange_ptr(&v[i].s->sound, NULL);   /* their close must not come back here */
    sound->v = NULL;
    sound->count = sound->cap = 0;
    for (i = 0; i < sound->retired_count; i++) fa_exchange_ptr(&sound->retired[i]->sound, NULL);
    fa_mutex_unlock(&sound->lock);
    for (i = 0; i < n; i++) fa_stream_close(v[i].s);
    for (i = 0; i < sound->retired_count; i++) fa_stream_close(sound->retired[i]);
    free(v);
    free(sound->retired);
    pcm_release(sound->pcm);
    fa_mutex_destroy(&sound->lock);
    free(sound);
}

FA_API fa_result FA_CALL fa_sound_get_info(fa_sound* sound, fa_sound_info* out_info)
{
    uint32_t i;
    if (!sound || !out_info) return FA_ERROR_INVALID_ARG;
    memset(out_info, 0, sizeof(*out_info));
    out_info->sample_rate = sound->pcm->rate;
    out_info->channels = sound->pcm->channels;
    out_info->frames = sound->pcm->frames;
    out_info->memory_bytes = (uint64_t)sound->pcm->frames * sound->pcm->channels * sizeof(float);
    fa_mutex_lock(&sound->lock);
    out_info->max_voices = sound->max_voices;
    out_info->steal = sound->steal;
    out_info->voices = sound->count;
    for (i = 0; i < sound->count; i++) out_info->playing += (uint32_t)fa_stream_voice_busy(sound->v[i].s);
    fa_mutex_unlock(&sound->lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_sound_set_options(fa_sound* sound, const fa_sound_options* options)
{
    uint32_t max_voices, i;
    int32_t steal;
    fa_result r;
    if (!sound || !options) return FA_ERROR_INVALID_ARG;
    r = read_options(options, &max_voices, &steal, NULL);
    if (r != FA_OK) return r;
    fa_mutex_lock(&sound->lock);
    if (sound->count > max_voices && sound->retired_count + sound->count - max_voices > sound->retired_cap) {
        uint32_t cap = sound->retired_count + sound->count - max_voices;
        fa_stream** p = (fa_stream**)realloc(sound->retired, cap * sizeof(*p));
        if (!p) { fa_mutex_unlock(&sound->lock); return FA_ERROR_OUT_OF_MEMORY; }
        sound->retired = p;
        sound->retired_cap = cap;
    }
    sound->max_voices = max_voices;
    sound->steal = steal;
    /* Over the limit: free voices go first (the newest of them), then the oldest busy
     * ones. They stop (still attached) and stay until fa_sound_close. */
    while (sound->count > sound->max_voices) {
        uint32_t pick = sound->count;
        fa_stream* s;
        for (i = sound->count; i-- > 0;)
            if (!fa_stream_voice_busy(sound->v[i].s)) { pick = i; break; }
        if (pick == sound->count) {
            pick = 0;
            for (i = 1; i < sound->count; i++)
                if (sound->v[i].seq < sound->v[pick].seq) pick = i;
        }
        s = sound->v[pick].s;
        memmove(&sound->v[pick], &sound->v[pick + 1], (size_t)(sound->count - pick - 1) * sizeof(sound->v[0]));
        sound->count--;
        sound->retired[sound->retired_count++] = s;
        if (fa_stream_voice_busy(s)) fa_stream_stop(s);   /* stays attached, stopped: nothing more is heard */
    }
    fa_mutex_unlock(&sound->lock);
    return FA_OK;
}

static float voice_volume(fa_stream* s)
{
    float v = 1.0f;
    fa_stream_get_param(s, FA_PARAM_VOLUME, &v);
    return v;
}

/* The voice for a new copy (lock held): a free one, preferably already on `device`;
 * else a new one under the limit (*out_new); else one taken over by the steal rule
 * (*out_stolen: it is busy). -1 = none. */
static int64_t pick_voice_locked(fa_sound* snd, fa_device* device, int* out_new, int* out_stolen, fa_result* out_r)
{
    uint32_t i;
    int64_t free_any = -1, pick;
    *out_new = 0;
    *out_stolen = 0;
    *out_r = FA_OK;
    for (i = 0; i < snd->count; i++) {
        fa_stream* s = snd->v[i].s;
        if (fa_stream_voice_busy(s)) continue;
        if (fa_load_ptr(&s->clock_device) == device) return i;
        if (free_any < 0) free_any = i;
    }
    if (free_any >= 0) return free_any;
    if (snd->count < snd->max_voices) {
        fa_decoder dec;
        fa_stream* s;
        if (snd->count == snd->cap) {
            uint32_t cap = snd->cap ? snd->cap * 2 : 4;
            fa_sound_voice* v = (fa_sound_voice*)realloc(snd->v, cap * sizeof(*v));
            if (!v) { *out_r = FA_ERROR_OUT_OF_MEMORY; return -1; }
            snd->v = v;
            snd->cap = cap;
        }
        *out_r = pcm_decoder_open(&dec, snd->pcm);
        if (*out_r != FA_OK) return -1;
        *out_r = fa_stream_open_voice(snd->engine, &dec, snd, &s);
        if (*out_r != FA_OK) return -1;
        snd->v[snd->count].s = s;
        snd->v[snd->count].seq = 0;
        *out_new = 1;
        return snd->count++;
    }
    if (snd->steal == FA_SOUND_STEAL_NONE || snd->count == 0) { *out_r = FA_ERROR_BUSY; return -1; }
    pick = 0;
    if (snd->steal == FA_SOUND_STEAL_QUIETEST) {
        float quietest = voice_volume(snd->v[0].s);
        for (i = 1; i < snd->count; i++) {
            float v = voice_volume(snd->v[i].s);
            if (v < quietest || (v == quietest && snd->v[i].seq < snd->v[pick].seq)) { quietest = v; pick = i; }
        }
    } else {
        for (i = 1; i < snd->count; i++)
            if (snd->v[i].seq < snd->v[pick].seq) pick = i;
    }
    *out_stolen = 1;
    return pick;
}

FA_API fa_result FA_CALL fa_sound_play(fa_sound* sound, fa_device* device, fa_bus* bus, float volume, float pan,
                                       fa_stream** out_voice)
{
    fa_stream* s;
    fa_stream* failed = NULL;
    int64_t i;
    int created, stolen;
    int32_t loop = 0;
    fa_result r;
    if (out_voice) *out_voice = NULL;
    if (!sound || !device || device->engine != sound->engine || (bus && bus->device != device)) return FA_ERROR_INVALID_ARG;
    if (!(volume >= 0.0f) || pan != pan) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&sound->lock);
    i = pick_voice_locked(sound, device, &created, &stolen, &r);
    if (i < 0) { fa_mutex_unlock(&sound->lock); return r; }
    s = sound->v[i].s;
    if (!created) {
        /* A copy taken over stops at once; a reused one plays through once. */
        if (stolen) fa_stream_stop(s);
        if (fa_stream_get_loop(s, &loop) == FA_OK && loop) fa_stream_set_loop(s, 0);
    }
    r = fa_stream_attach(s, device, bus);
    if (r == FA_OK) r = fa_stream_voice_start(s, volume, pan);
    if (r == FA_OK) {
        sound->v[i].seq = ++sound->seq;
        if (out_voice) *out_voice = s;
    } else if (created) {
        failed = take_voice_locked(sound, (uint32_t)i);
    }
    fa_mutex_unlock(&sound->lock);
    if (failed) fa_stream_close(failed);
    return r;
}

FA_API fa_result FA_CALL fa_sound_stop(fa_sound* sound)
{
    uint32_t i;
    if (!sound) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&sound->lock);
    for (i = 0; i < sound->count; i++)
        if (fa_stream_voice_busy(sound->v[i].s)) fa_stream_stop(sound->v[i].s);
    fa_mutex_unlock(&sound->lock);
    return FA_OK;
}

static int by_seq(const void* a, const void* b)
{
    uint64_t x = ((const fa_sound_voice*)a)->seq, y = ((const fa_sound_voice*)b)->seq;
    return x < y ? -1 : x > y;
}

FA_API fa_result FA_CALL fa_sound_get_voices(fa_sound* sound, fa_stream** out_voices, uint32_t capacity, uint32_t* out_count)
{
    fa_sound_voice* busy;
    uint32_t i, n = 0;
    if (!sound || !out_count || (capacity && !out_voices)) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&sound->lock);
    busy = (fa_sound_voice*)malloc((sound->count ? sound->count : 1) * sizeof(*busy));
    if (!busy) { fa_mutex_unlock(&sound->lock); return FA_ERROR_OUT_OF_MEMORY; }
    for (i = 0; i < sound->count; i++)
        if (fa_stream_voice_busy(sound->v[i].s)) busy[n++] = sound->v[i];
    fa_mutex_unlock(&sound->lock);
    qsort(busy, n, sizeof(*busy), by_seq);
    for (i = 0; i < n && i < capacity; i++) out_voices[i] = busy[i].s;
    free(busy);
    *out_count = n;
    return FA_OK;
}

/* Tests: the PCM a voice reads (NULL for other streams), and how many hold it. */
const float* fa_sound_voice_data(fa_stream* voice, int32_t* out_refs)
{
    pcm_reader* r;
    if (!voice->decoder_ready || voice->decoder.vt != &pcm_vtable) return NULL;
    r = (pcm_reader*)voice->decoder.impl;
    if (out_refs) *out_refs = fa_load32(&r->pcm->refs);
    return r->pcm->data;
}
