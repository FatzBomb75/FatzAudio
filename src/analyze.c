/* Track analysis: one decode of a file's audio feeds the beat tracker (beat.c),
 * the key finder (key.c), the waveform overview and the loudness meter, as asked.
 * MIDI files and tracker modules take their beat grid from the file instead.
 *
 * Results go to the scan cache directory, next to the loudness results and keyed
 * the same way (content, and for MIDI the soundfont and rate), one
 * "<key>.analysis" file per file and audio track. A file holds every kind
 * measured so far: a request for more measures the union and rewrites it, so
 * asking for kinds one at a time does not thrash. The file is binary,
 * little-endian, after a text magic line that changes whenever a measurement
 * does (then older files are simply measured again). */
#include "fa_internal.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FA_ANALYSIS_MAGIC "FatzAudio-analysis 1\n"   /* bumped whenever a measurement changes */
#define FA_ANALYSIS_READ 4096
#define FA_ANALYSIS_MAX_CACHE ((size_t)64 << 20)
#define FA_ANALYZE_ALL (FA_ANALYZE_LOUDNESS | FA_ANALYZE_BEATS | FA_ANALYZE_KEY | FA_ANALYZE_WAVEFORM)
#define FA_PI 3.14159265358979323846

struct fa_analysis {
    uint32_t kinds;              /* measured (fa_analysis_kind) */
    uint32_t flags;              /* fa_analysis_info_flags */
    uint32_t rate;
    fa_frames length;
    float min_bpm, max_bpm;      /* the range the grid was folded into */
    int have_grid, have_key, have_loudness;
    fa_grid_data grid;
    fa_key_info key;
    fa_waveform_point* wave;
    uint32_t nwave;
    fa_loudness loudness;
};

/* What to analyse: a file (or a window of it) or memory, and how to decode it. */
typedef struct job {
    fa_engine* engine;
    const char* path;
    int64_t path_offset, path_length;
    const char* name_hint;
    const uint8_t* mem;
    size_t mem_size;
    int32_t audio_track;
    fa_decoder_env env;
    fa_analysis_options opt;
} job;

FA_API void FA_CALL fa_analysis_options_init(fa_analysis_options* options)
{
    if (!options) return;
    memset(options, 0, sizeof(*options));
    options->struct_size = sizeof(*options);
    options->kinds = FA_ANALYZE_BEATS | FA_ANALYZE_KEY;
    options->min_bpm = 70.0f;
    options->max_bpm = 180.0f;
}

static int read_options(const fa_analysis_options* in, fa_analysis_options* out)
{
    fa_analysis_options_init(out);
    if (!in) return 1;
    if (in->struct_size < offsetof(fa_analysis_options, flags) + sizeof(uint32_t)) return 0;
    memcpy(out, in, offsetof(fa_analysis_options, flags) + sizeof(uint32_t));
    out->struct_size = sizeof(*out);
    if (out->kinds == 0 || (out->kinds & ~(uint32_t)FA_ANALYZE_ALL) || (out->flags & ~(uint32_t)FA_ANALYZE_NO_CACHE))
        return 0;
    if (!(out->min_bpm >= 30.0f && out->max_bpm <= 400.0f && out->max_bpm >= 1.9f * out->min_bpm)) return 0;
    return 1;
}

/* ---- Waveform -------------------------------------------------------------------- */

typedef struct biquad { double b0, b1, b2, a1, a2, z1, z2; } biquad;

/* Butterworth (Q 0.7071) low or high pass, RBJ cookbook. */
static void biquad_set(biquad* f, uint32_t rate, double hz, int high)
{
    double w = 2.0 * FA_PI * hz / rate, c = cos(w), alpha = sin(w) / (2.0 * 0.7071067811865476), a0 = 1.0 + alpha;
    memset(f, 0, sizeof(*f));
    if (high) { f->b0 = (1.0 + c) / 2.0; f->b1 = -(1.0 + c); }
    else { f->b0 = (1.0 - c) / 2.0; f->b1 = 1.0 - c; }
    f->b2 = f->b0;
    f->b0 /= a0; f->b1 /= a0; f->b2 /= a0;
    f->a1 = -2.0 * c / a0;
    f->a2 = (1.0 - alpha) / a0;
}

static double biquad_run(biquad* f, double x)
{
    double y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

typedef struct wave_state {
    uint32_t rate;
    biquad low, high, mid_hp, mid_lp;
    double peak, sl, sm, sh;
    int64_t in_point, frame;     /* frames in the current point, frames so far */
    int64_t point_end;           /* frame where the current point ends */
    fa_waveform_point* pts;
    uint32_t n, cap;
    int failed;
} wave_state;

static void wave_init(wave_state* w, uint32_t rate)
{
    memset(w, 0, sizeof(*w));
    w->rate = rate;
    biquad_set(&w->low, rate, 250.0, 0);
    biquad_set(&w->high, rate, 2500.0, 1);
    biquad_set(&w->mid_hp, rate, 250.0, 1);
    biquad_set(&w->mid_lp, rate, 2500.0, 0);
    w->point_end = rate / 100;
}

/* Stored with 16 bits, so a cached overview is the same as a fresh one. */
static float q16(double v)
{
    if (!(v > 0.0)) return 0.0f;
    if (v >= 1.0) return 1.0f;
    return (float)(floor(v * 65535.0 + 0.5) / 65535.0);
}

static void wave_point(wave_state* w)
{
    fa_waveform_point* p;
    double n = (double)(w->in_point > 0 ? w->in_point : 1);
    if (w->n == w->cap) {
        uint32_t cap = w->cap ? w->cap * 2 : 4096;
        fa_waveform_point* grown = (fa_waveform_point*)realloc(w->pts, (size_t)cap * sizeof(*grown));
        if (!grown) { w->failed = 1; return; }
        w->pts = grown;
        w->cap = cap;
    }
    p = &w->pts[w->n++];
    p->peak = q16(w->peak);
    p->low = q16(sqrt(2.0 * w->sl / n));
    p->mid = q16(sqrt(2.0 * w->sm / n));
    p->high = q16(sqrt(2.0 * w->sh / n));
    w->peak = w->sl = w->sm = w->sh = 0.0;
    w->in_point = 0;
    w->point_end = (int64_t)(((int64_t)w->n + 1) * w->rate / 100);
}

static void wave_process(wave_state* w, const float* in, const float* mono, int64_t frames, uint32_t channels)
{
    int64_t i;
    uint32_t c;
    for (i = 0; i < frames && !w->failed; i++) {
        double x = mono[i], l = biquad_run(&w->low, x), h = biquad_run(&w->high, x);
        double m = biquad_run(&w->mid_lp, biquad_run(&w->mid_hp, x));
        for (c = 0; c < channels; c++) {
            double a = fabs((double)in[i * channels + c]);
            if (a > w->peak) w->peak = a;
        }
        w->sl += l * l;
        w->sm += m * m;
        w->sh += h * h;
        w->in_point++;
        if (++w->frame == w->point_end) wave_point(w);
    }
}

/* ---- Cache ------------------------------------------------------------------------- */

typedef struct bytes { uint8_t* d; size_t n, cap; int failed; } bytes;

static void put(bytes* b, const void* p, size_t n)
{
    if (b->failed) return;
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        uint8_t* grown;
        while (cap < b->n + n) cap *= 2;
        grown = (uint8_t*)realloc(b->d, cap);
        if (!grown) { b->failed = 1; return; }
        b->d = grown;
        b->cap = cap;
    }
    memcpy(b->d + b->n, p, n);
    b->n += n;
}

static void put_u64(bytes* b, uint64_t v)
{
    uint8_t x[8];
    int i;
    for (i = 0; i < 8; i++) x[i] = (uint8_t)(v >> (8 * i));
    put(b, x, 8);
}

static void put_u32(bytes* b, uint32_t v)
{
    uint8_t x[4];
    int i;
    for (i = 0; i < 4; i++) x[i] = (uint8_t)(v >> (8 * i));
    put(b, x, 4);
}

static void put_u16(bytes* b, uint32_t v)
{
    uint8_t x[2];
    x[0] = (uint8_t)v;
    x[1] = (uint8_t)(v >> 8);
    put(b, x, 2);
}

static void put_f64(bytes* b, double v) { uint64_t u; memcpy(&u, &v, 8); put_u64(b, u); }
static void put_f32(bytes* b, float v) { uint32_t u; memcpy(&u, &v, 4); put_u32(b, u); }
static uint32_t to_u16(float v) { return (uint32_t)floor((double)v * 65535.0 + 0.5); }

typedef struct reader { const uint8_t* d; size_t n, at; int bad; } reader;

static const uint8_t* take(reader* r, size_t n)
{
    const uint8_t* p;
    if (r->bad || r->n - r->at < n) { r->bad = 1; return NULL; }
    p = r->d + r->at;
    r->at += n;
    return p;
}

static uint64_t get_u64(reader* r)
{
    const uint8_t* p = take(r, 8);
    uint64_t v = 0;
    int i;
    if (p) for (i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static uint32_t get_u32(reader* r)
{
    const uint8_t* p = take(r, 4);
    return p ? (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24 : 0;
}

static uint32_t get_u16(reader* r)
{
    const uint8_t* p = take(r, 2);
    return p ? (uint32_t)p[0] | (uint32_t)p[1] << 8 : 0;
}

static double get_f64(reader* r) { uint64_t u = get_u64(r); double v; memcpy(&v, &u, 8); return v; }
static float get_f32(reader* r) { uint32_t u = get_u32(r); float v; memcpy(&v, &u, 4); return v; }

static void encode(const fa_analysis* a, bytes* b)
{
    uint32_t i;
    put(b, FA_ANALYSIS_MAGIC, strlen(FA_ANALYSIS_MAGIC));
    put_u32(b, a->kinds);
    put_u32(b, a->rate);
    put_u64(b, (uint64_t)a->length);
    put_f32(b, a->min_bpm);
    put_f32(b, a->max_bpm);
    put_u32(b, (uint32_t)a->have_grid);
    if (a->have_grid) {
        const fa_beat_grid* g = &a->grid.grid;
        put_f64(b, g->bpm);
        put_u64(b, (uint64_t)g->first_beat);
        put_u32(b, g->sample_rate);
        put_u32(b, g->beats_per_bar);
        put_u32(b, g->bar_offset);
        put_f32(b, g->confidence);
        put_u32(b, g->flags);
        put_u32(b, g->segment_count);
        for (i = 0; i < g->segment_count; i++) {
            put_u64(b, (uint64_t)a->grid.segs[i].start);
            put_f64(b, a->grid.segs[i].bpm);
            put_f64(b, a->grid.segs[i].beat);
        }
    }
    put_u32(b, (uint32_t)a->have_key);
    if (a->have_key) {
        put_u32(b, (uint32_t)a->key.tonic);
        put_u32(b, (uint32_t)a->key.minor);
        put_f32(b, a->key.confidence);
    }
    put_u32(b, a->nwave);
    for (i = 0; i < a->nwave; i++) {
        put_u16(b, to_u16(a->wave[i].peak));
        put_u16(b, to_u16(a->wave[i].low));
        put_u16(b, to_u16(a->wave[i].mid));
        put_u16(b, to_u16(a->wave[i].high));
    }
    put_u32(b, (uint32_t)a->have_loudness);
    if (a->have_loudness) {
        put_f64(b, a->loudness.integrated_lufs);
        put_f64(b, a->loudness.range_lu);
        put_f64(b, a->loudness.true_peak_dbtp);
    }
}

static int decode(fa_analysis* a, const uint8_t* d, size_t n)
{
    reader r;
    uint32_t i, count;
    size_t m = strlen(FA_ANALYSIS_MAGIC);
    if (n < m || memcmp(d, FA_ANALYSIS_MAGIC, m) != 0) return 0;
    r.d = d; r.n = n; r.at = m; r.bad = 0;
    a->kinds = get_u32(&r);
    a->rate = get_u32(&r);
    a->length = (fa_frames)get_u64(&r);
    a->min_bpm = get_f32(&r);
    a->max_bpm = get_f32(&r);
    a->have_grid = get_u32(&r) != 0;
    if (a->have_grid) {
        fa_beat_grid* g = &a->grid.grid;
        g->struct_size = sizeof(*g);
        g->bpm = get_f64(&r);
        g->first_beat = (fa_frames)get_u64(&r);
        g->sample_rate = get_u32(&r);
        g->beats_per_bar = get_u32(&r);
        g->bar_offset = get_u32(&r);
        g->confidence = get_f32(&r);
        g->flags = get_u32(&r);
        count = get_u32(&r);
        if (r.bad || count == 0 || count > (r.n - r.at) / 24) return 0;
        a->grid.segs = (fa_tempo_segment*)malloc((size_t)count * sizeof(fa_tempo_segment));
        if (!a->grid.segs) return 0;
        g->segment_count = count;
        for (i = 0; i < count; i++) {
            a->grid.segs[i].start = (fa_frames)get_u64(&r);
            a->grid.segs[i].bpm = get_f64(&r);
            a->grid.segs[i].beat = get_f64(&r);
        }
    }
    a->have_key = get_u32(&r) != 0;
    if (a->have_key) {
        int32_t tonic = (int32_t)get_u32(&r), minor = (int32_t)get_u32(&r);
        float conf = get_f32(&r);
        a->key.struct_size = sizeof(a->key);
        if (r.bad || fa_key_make(tonic, minor, &a->key) != FA_OK) return 0;
        a->key.confidence = conf;
    }
    count = get_u32(&r);
    if (r.bad || count > (r.n - r.at) / 8) return 0;
    if (count) {
        a->wave = (fa_waveform_point*)malloc((size_t)count * sizeof(fa_waveform_point));
        if (!a->wave) return 0;
        a->nwave = count;
        for (i = 0; i < count; i++) {
            a->wave[i].peak = (float)(get_u16(&r) / 65535.0);
            a->wave[i].low = (float)(get_u16(&r) / 65535.0);
            a->wave[i].mid = (float)(get_u16(&r) / 65535.0);
            a->wave[i].high = (float)(get_u16(&r) / 65535.0);
        }
    }
    a->have_loudness = get_u32(&r) != 0;
    if (a->have_loudness) {
        a->loudness.integrated_lufs = get_f64(&r);
        a->loudness.range_lu = get_f64(&r);
        a->loudness.true_peak_dbtp = get_f64(&r);
        a->loudness.is_final = 1;
        a->loudness.source = FA_LOUDNESS_CACHE;
    }
    return !r.bad && r.at == r.n && (a->kinds & ~(uint32_t)FA_ANALYZE_ALL) == 0;
}

static void free_parts(fa_analysis* a)
{
    fa_grid_data_free(&a->grid);
    free(a->wave);
    a->wave = NULL;
    a->nwave = 0;
}

static int cache_read(const char* path, fa_analysis* a)
{
    FILE* f = fa_fopen_utf8(path, "rb");
    uint8_t* d = NULL;
    size_t n = 0, cap = 0;
    int ok = 0;
    if (!f) return 0;
    for (;;) {
        size_t got;
        if (n == cap) {
            uint8_t* grown;
            if (cap >= FA_ANALYSIS_MAX_CACHE) break;
            cap = cap ? cap * 2 : 65536;
            grown = (uint8_t*)realloc(d, cap);
            if (!grown) break;
            d = grown;
        }
        got = fread(d + n, 1, cap - n, f);
        n += got;
        if (got == 0) { ok = feof(f) != 0; break; }
    }
    fclose(f);
    if (ok) ok = decode(a, d, n);
    if (!ok) {
        free_parts(a);
        memset(a, 0, sizeof(*a));
    }
    free(d);
    return ok;
}

static void cache_write(const char* dir, const char* path, const fa_analysis* a)
{
    char tmp[1100];
    bytes b;
    FILE* f;
    memset(&b, 0, sizeof(b));
    encode(a, &b);
    if (b.failed) { free(b.d); return; }
    fa_mkdir_utf8(dir);
    snprintf(tmp, sizeof(tmp), "%s.%llx.tmp", path, (unsigned long long)fa_thread_current_id());
    f = fa_fopen_utf8(tmp, "wb");
    if (f) {
        int ok = fwrite(b.d, 1, b.n, f) == b.n;
        if (fclose(f) != 0 || !ok || fa_replace_file(tmp, path) != 0) remove(tmp);
    }
    free(b.d);
}

/* ---- The pass ------------------------------------------------------------------------ */

static fa_result open_source(const job* j, fa_source* src)
{
    if (!j->path) return fa_source_open_memory_ref(src, j->mem, j->mem_size);
    return fa_source_open_path_window(src, j->path, j->path_offset, j->path_length);
}

/* The file's BPM tag, 0 if none (whole files and memory only). */
static double tag_bpm(const job* j)
{
    fa_tags* tags = NULL;
    double v = 0.0;
    fa_result r;
    if (j->path && (j->path_offset || j->path_length)) return 0.0;
    r = j->path ? fa_tags_read_path(j->path, 0, &tags) : fa_tags_read_memory(j->mem, j->mem_size, 0, &tags);
    if (r == FA_OK && tags) {
        const char* s = fa_tags_get(tags, FA_TAG_BPM);
        if (s && *s) v = strtod(s, NULL);
        fa_tags_close(tags);
    }
    return v > 0.0 && v < 1000.0 ? v : 0.0;
}

static fa_result measure(const job* j, uint32_t kinds, fa_analysis* a)
{
    fa_source src;
    fa_decoder dec;
    fa_onset* onset = NULL;
    fa_chroma* chroma = NULL;
    struct fa_tracker* tracker;
    struct fa_midi* midi;
    wave_state wave;
    fa_meter meter;
    int have_meter = 0, want_wave = (kinds & FA_ANALYZE_WAVEFORM) != 0;
    float *buf = NULL, *mono = NULL;
    fa_frames total = 0;
    uint32_t ch, rate;
    fa_result r = open_source(j, &src);
    if (r != FA_OK) return r;
    r = fa_decoder_open(&dec, &src, j->name_hint, NULL, j->audio_track, &j->env);
    if (r != FA_OK) { fa_source_close(&src); return r; }
    rate = dec.format.sample_rate;
    ch = dec.format.channels;
    a->rate = rate;
    a->kinds = kinds;
    a->min_bpm = j->opt.min_bpm;
    a->max_bpm = j->opt.max_bpm;
    midi = fa_midi_of(&dec);
    tracker = fa_tracker_of(&dec);
    memset(&wave, 0, sizeof(wave));
    if (rate == 0 || ch == 0) r = FA_ERROR_UNSUPPORTED_FORMAT;
    if (r == FA_OK && (kinds & FA_ANALYZE_BEATS)) {
        if (midi) {
            r = fa_midi_exact_grid(midi, &a->grid);
            a->have_grid = r == FA_OK;
        } else if (tracker) {
            r = fa_tracker_grid_start(tracker);
        } else if (!(onset = fa_onset_new(rate))) {
            r = FA_ERROR_OUT_OF_MEMORY;
        }
    }
    if (r == FA_OK && (kinds & FA_ANALYZE_KEY) && !(chroma = fa_chroma_new(rate))) r = FA_ERROR_OUT_OF_MEMORY;
    if (r == FA_OK && want_wave) wave_init(&wave, rate);
    if (r == FA_OK && (kinds & FA_ANALYZE_LOUDNESS)) {
        r = fa_meter_init(&meter, rate, ch, 1, 0);
        have_meter = r == FA_OK;
    }
    if (r == FA_OK) {
        buf = (float*)malloc((size_t)FA_ANALYSIS_READ * ch * sizeof(float));
        mono = (float*)malloc((size_t)FA_ANALYSIS_READ * sizeof(float));
        if (!buf || !mono) r = FA_ERROR_OUT_OF_MEMORY;
    }
    while (r == FA_OK) {
        fa_frames got = 0, i;
        fa_result rr = fa_decoder_read(&dec, buf, FA_ANALYSIS_READ, &got);
        if (got <= 0) { if (rr != FA_ERROR_END_OF_STREAM && rr != FA_OK) r = rr; break; }
        for (i = 0; i < got; i++) {
            const float* f = buf + i * ch;
            float s = 0.0f;
            uint32_t c;
            for (c = 0; c < ch; c++) s += f[c];
            mono[i] = s / (float)ch;
        }
        if (onset) r = fa_onset_process(onset, mono, got);
        if (chroma) fa_chroma_process(chroma, mono, got);
        if (want_wave) wave_process(&wave, buf, mono, got, ch);
        if (have_meter && r == FA_OK) r = fa_meter_process(&meter, buf, got);
        total += got;
    }
    a->length = total;
    if (r == FA_OK && want_wave) {
        if (wave.in_point > 0) wave_point(&wave);
        if (wave.failed) r = FA_ERROR_OUT_OF_MEMORY;
        a->wave = wave.pts;
        a->nwave = wave.n;
        wave.pts = NULL;
    }
    if (r == FA_OK && tracker && (kinds & FA_ANALYZE_BEATS)) {
        r = fa_tracker_grid_finish(tracker, total, &a->grid);
        a->have_grid = r == FA_OK;
        if (r == FA_ERROR_NOT_FOUND) r = FA_OK;
    }
    if (r == FA_OK && onset)
        a->have_grid = fa_onset_grid(onset, j->opt.min_bpm, j->opt.max_bpm, tag_bpm(j), &a->grid);
    if (r == FA_OK && chroma) a->have_key = fa_chroma_key(chroma, &a->key);
    if (r == FA_OK && have_meter) {
        double peak;
        fa_truepeak_flush(meter.tp);
        peak = meter.tp->peak;
        a->loudness.integrated_lufs = fa_loudness_integrated(meter.subs, meter.nsubs);
        a->loudness.range_lu = fa_loudness_range(meter.subs, meter.nsubs);
        a->loudness.true_peak_dbtp = peak > 0.0 ? 20.0 * log10(peak) : -HUGE_VAL;
        a->loudness.is_final = 1;
        a->loudness.source = FA_LOUDNESS_SCAN;
        a->have_loudness = 1;
    }
    if (have_meter) fa_meter_free(&meter);
    free(wave.pts);
    free(buf);
    free(mono);
    fa_onset_free(onset);
    fa_chroma_free(chroma);
    fa_decoder_close(&dec);
    fa_source_close(&src);
    return r;
}

/* The cached result covers the request: every kind, and a grid folded into the same
 * tempo range (exact grids are never folded). */
static int covers(const fa_analysis* a, const fa_analysis_options* opt)
{
    if ((a->kinds & opt->kinds) != opt->kinds) return 0;
    if ((opt->kinds & FA_ANALYZE_BEATS) && a->have_grid && !(a->grid.grid.flags & FA_GRID_EXACT) &&
        (a->min_bpm != opt->min_bpm || a->max_bpm != opt->max_bpm))
        return 0;
    return 1;
}

static fa_result run(const job* j, fa_analysis** out)
{
    fa_analysis* a;
    fa_source src;
    uint64_t key = 0;
    int64_t key_size = 0;
    int use_cache = 0;
    uint32_t kinds = j->opt.kinds;
    char path[1024];
    const char* dir = j->engine->config.cache_directory;
    fa_result r;

    *out = NULL;
    a = (fa_analysis*)calloc(1, sizeof(*a));
    if (!a) return FA_ERROR_OUT_OF_MEMORY;
    if (dir && dir[0] && !(j->opt.flags & FA_ANALYZE_NO_CACHE)) {
        r = open_source(j, &src);
        if (r != FA_OK) { free(a); return r; }
        if (fa_scan_cache_key(&src, &j->env, &key)) {
            use_cache = 1;
            key_size = src.length;
        }
        fa_source_close(&src);
    }
    if (use_cache) {
        fa_scan_cache_path(dir, key, key_size, j->audio_track, "analysis", path, sizeof(path));
        if (cache_read(path, a)) {
            if (covers(a, &j->opt)) {
                a->flags |= FA_ANALYSIS_FROM_CACHE;
                if (a->have_loudness) a->loudness.gain_db = fa_engine_norm_gain_db(j->engine, a->loudness.integrated_lufs);
                *out = a;
                return FA_OK;
            }
            /* Measure what was there too, so the file keeps it (a grid is folded
             * into the new range). */
            kinds |= a->kinds;
            free_parts(a);
            memset(a, 0, sizeof(*a));
        }
    }
    r = measure(j, kinds, a);
    if (r != FA_OK) { fa_analysis_free(a); return r; }
    if (use_cache) {
        cache_write(dir, path, a);
        if (a->have_loudness) fa_scan_cache_write_loudness(dir, key, key_size, j->audio_track, &a->loudness);
    }
    if (a->have_loudness) a->loudness.gain_db = fa_engine_norm_gain_db(j->engine, a->loudness.integrated_lufs);
    *out = a;
    return FA_OK;
}

/* ---- API ------------------------------------------------------------------------------ */

FA_API fa_result FA_CALL fa_analyze_path(fa_engine* engine, const char* path, const fa_analysis_options* options,
                                         fa_analysis** out_analysis)
{
    job j;
    fa_result r;
    if (out_analysis) *out_analysis = NULL;
    if (!engine || !path || !out_analysis) return FA_ERROR_INVALID_ARG;
    memset(&j, 0, sizeof(j));
    if (!read_options(options, &j.opt)) return FA_ERROR_INVALID_ARG;
    j.engine = engine;
    j.path = path;
    j.name_hint = path;
    j.audio_track = -1;
    j.env.midi_rate = fa_engine_midi_rate(engine);
    j.env.soundfont = fa_engine_soundfont(engine);
    if (!j.env.soundfont) {
        /* A MIDI file and no soundfont from the app: the OS's instruments, as playback does. */
        uint8_t head[64];
        size_t n = 0;
        FILE* f = fa_fopen_utf8(path, "rb");
        if (f) { n = fread(head, 1, sizeof(head), f); fclose(f); }
        if (fa_probe_midi(head, n, 0)) j.env.soundfont = fa_engine_system_soundfont(engine);
    }
    r = run(&j, out_analysis);
    fa_soundfont_release(j.env.soundfont);
    return r;
}

FA_API fa_result FA_CALL fa_analyze_stream(fa_stream* stream, const fa_analysis_options* options,
                                           fa_analysis** out_analysis)
{
    job j;
    fa_result r;
    if (out_analysis) *out_analysis = NULL;
    if (!stream || !out_analysis) return FA_ERROR_INVALID_ARG;
    memset(&j, 0, sizeof(j));
    if (!read_options(options, &j.opt)) return FA_ERROR_INVALID_ARG;
    if (!stream->path && stream->source.kind != FA_SOURCE_MEMORY) return FA_ERROR_NOT_SEEKABLE;
    j.engine = stream->engine;
    j.audio_track = stream->audio_track;
    j.env.midi_rate = stream->midi || stream->tracker ? stream->decoder.format.sample_rate : fa_engine_midi_rate(stream->engine);
    j.env.soundfont = fa_soundfont_retain(stream->soundfont);
    if (stream->path) {
        j.path = stream->path;
        j.path_offset = stream->path_offset;
        j.path_length = stream->path_length;
        j.name_hint = stream->path_length > 0 ? stream->path_name : stream->path;
    } else {
        j.mem = stream->source.mem;
        j.mem_size = stream->source.mem_size;
    }
    r = run(&j, out_analysis);
    fa_soundfont_release(j.env.soundfont);
    return r;
}

FA_API void FA_CALL fa_analysis_free(fa_analysis* analysis)
{
    if (!analysis) return;
    free_parts(analysis);
    free(analysis);
}

FA_API fa_result FA_CALL fa_analysis_get_info(const fa_analysis* analysis, fa_analysis_info* out_info)
{
    fa_analysis_info info;
    if (!analysis || !out_info || out_info->struct_size < sizeof(uint32_t)) return FA_ERROR_INVALID_ARG;
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    info.kinds = analysis->kinds;
    info.flags = analysis->flags;
    info.sample_rate = analysis->rate;
    info.length = analysis->length;
    fa_copy_sized(out_info, &info, sizeof(info));
    return FA_OK;
}

FA_API fa_result FA_CALL fa_analysis_get_beat_grid(const fa_analysis* analysis, fa_beat_grid* out_grid)
{
    if (!analysis || !out_grid || out_grid->struct_size < sizeof(uint32_t)) return FA_ERROR_INVALID_ARG;
    if (!analysis->have_grid) return FA_ERROR_NOT_FOUND;
    return fa_grid_data_get(&analysis->grid, out_grid, NULL, 0, NULL);
}

FA_API fa_result FA_CALL fa_analysis_get_tempo_map(const fa_analysis* analysis, fa_tempo_segment* out_segments,
                                                   uint32_t capacity, uint32_t* out_count)
{
    if (out_count) *out_count = 0;
    if (!analysis || !out_count || (capacity && !out_segments)) return FA_ERROR_INVALID_ARG;
    if (!analysis->have_grid) return FA_ERROR_NOT_FOUND;
    return fa_grid_data_get(&analysis->grid, NULL, out_segments, capacity, out_count);
}

FA_API fa_result FA_CALL fa_analysis_get_key(const fa_analysis* analysis, fa_key_info* out_key)
{
    if (!analysis || !out_key || out_key->struct_size < sizeof(uint32_t)) return FA_ERROR_INVALID_ARG;
    if (!analysis->have_key) return FA_ERROR_NOT_FOUND;
    fa_copy_sized(out_key, &analysis->key, sizeof(analysis->key));
    return FA_OK;
}

FA_API fa_result FA_CALL fa_analysis_get_waveform(const fa_analysis* analysis, fa_waveform_point* out_points,
                                                  uint32_t capacity, uint32_t* out_count)
{
    uint32_t n;
    if (out_count) *out_count = 0;
    if (!analysis || !out_count || (capacity && !out_points)) return FA_ERROR_INVALID_ARG;
    if (!(analysis->kinds & FA_ANALYZE_WAVEFORM)) return FA_ERROR_NOT_FOUND;
    n = analysis->nwave < capacity ? analysis->nwave : capacity;
    if (n) memcpy(out_points, analysis->wave, (size_t)n * sizeof(fa_waveform_point));
    *out_count = analysis->nwave;
    return FA_OK;
}

FA_API fa_result FA_CALL fa_analysis_get_loudness(const fa_analysis* analysis, fa_loudness* out_loudness)
{
    if (!analysis || !out_loudness) return FA_ERROR_INVALID_ARG;
    if (!analysis->have_loudness) return FA_ERROR_NOT_FOUND;
    *out_loudness = analysis->loudness;
    return FA_OK;
}
