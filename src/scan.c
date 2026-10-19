/* Background loudness analysis.
 *
 * A job is one file. Its first task ("prepare") looks for loudness tags, then
 * for a cached result, and otherwise plans the scan: the file is cut into
 * chunks at 100 ms sub-block boundaries, each scanned by its own decoder on a
 * scan thread, with one second of warm-up before its start so the filters are
 * settled. The thread finishing the last chunk joins the sub-block lists,
 * computes the result, writes the cache and hands it to the stream.
 *
 * Only audio is decoded: the scanner reads through the same decoder registry as
 * playback, so container formats that hold video (MP4, Matroska) are scanned
 * through their audio track alone once their demuxers are in the registry.
 *
 * Jobs run in the order they were queued; all chunks of a file are in flight at
 * once before the next file starts, so the file asked for first is done first. */
#include "fa_internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FA_SCAN_READ 4096          /* frames per decoder read */
#define FA_SCAN_MIN_CHUNK_S 20     /* shorter chunks would spend too much on warm-up */
#define FA_SCAN_WARMUP_S 1
#define FA_SCAN_MAX_THREADS 16
#define FA_KEY_BLOCK 65536
#define FA_CACHE_MAGIC "FatzAudio-loudness 2"   /* bumped whenever the measurement changes */

typedef struct scan_chunk {
    double* subs;
    int64_t nsubs;
    float peak;
    fa_result r;
} scan_chunk;

struct fa_scan_job {
    fa_engine* engine;
    struct fa_stream* stream;      /* NULL for fa_scan_loudness_path */
    char* path;                    /* the source: a file ... */
    int64_t path_offset, path_length; /* ... or these bytes of it (a stored ZIP entry) ... */
    const char* name_hint;         /* path, or the ZIP entry's name; NULL for memory */
    char entry_name[260];
    const uint8_t* mem;            /* ... or the memory of a memory stream */
    size_t mem_size;
    int32_t audio_track;
    int use_tags;
    fa_atomic32 cancel;
    fa_decoder_env env;            /* MIDI: the soundfont (a reference) and rate; no dynamic
                                      range compression: scans measure the full range */

    /* Under engine->scan_lock. */
    int prepare_taken, plan_ready, finished;
    int merging;                   /* a thread is merging and delivering the chunks */
    int nchunks, next_chunk, chunks_done;
    fa_scan_job* next;

    /* Written by the prepare task before plan_ready, read-only afterwards. */
    uint32_t rate, channels;
    fa_frames chunk_frames;
    scan_chunk* chunks;
    int have_key;
    uint64_t key;
    int64_t key_size;
    double album_lufs;

    fa_loudness result;
    fa_result status;
};

/* ---- Content key and cache --------------------------------------------------- */

/* FNV-1a, 64-bit: content keys here and soundfont keys (dec_midi.c). */
uint64_t fa_fnv1a(uint64_t h, const uint8_t* p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ULL; }
    return h;
}

/* A fingerprint of the content: its size plus the first, middle and last 64 KB.
 * Cheap even for multi-GB video files, and independent of the file's name. */
static int content_key(fa_source* src, uint64_t* out)
{
    uint8_t* buf;
    uint8_t lenb[8];
    int64_t len = src->length, offs[3];
    uint64_t h = 0xcbf29ce484222325ULL;
    int i;
    if (len <= 0) return 0;
    buf = (uint8_t*)malloc(FA_KEY_BLOCK);
    if (!buf) return 0;
    for (i = 0; i < 8; i++) lenb[i] = (uint8_t)((uint64_t)len >> (i * 8));
    h = fa_fnv1a(h, lenb, 8);
    offs[0] = 0;
    offs[1] = len / 2 - FA_KEY_BLOCK / 2;
    offs[2] = len - FA_KEY_BLOCK;
    for (i = 0; i < 3; i++) {
        int64_t got;
        if (offs[i] < 0) offs[i] = 0;
        if (fa_source_seek(src, offs[i]) != FA_OK) { free(buf); return 0; }
        got = fa_source_read(src, buf, FA_KEY_BLOCK);
        if (got < 0) { free(buf); return 0; }
        h = fa_fnv1a(h, buf, (size_t)got);
    }
    free(buf);
    *out = h;
    return 1;
}

/* A MIDI file sounds as its soundfont and rate make it: both go into the key, so
 * a scan with other instruments is not mistaken for this one. Other formats keep
 * the key of their content alone. */
static uint64_t midi_key(fa_source* src, const fa_decoder_env* env, uint64_t key)
{
    const uint8_t* head;
    size_t n = fa_source_peek(src, &head);
    uint8_t extra[12];
    uint64_t sf;
    int i;
    if (!env->soundfont || !fa_probe_midi(head, n, 0)) return key;
    sf = fa_soundfont_key(env->soundfont);
    for (i = 0; i < 8; i++) extra[i] = (uint8_t)(sf >> (i * 8));
    for (i = 0; i < 4; i++) extra[8 + i] = (uint8_t)(env->midi_rate >> (i * 8));
    return fa_fnv1a(key, extra, sizeof(extra));
}

static void cache_path(const fa_scan_job* j, char* out, size_t cap)
{
    const char* dir = j->engine->config.cache_directory;
    size_t n = strlen(dir);
    const char* sep = (n > 0 && (dir[n - 1] == '/' || dir[n - 1] == '\\')) ? "" : "/";
    snprintf(out, cap, "%s%s%016llx-%lld-%d.loudness", dir, sep, (unsigned long long)j->key,
             (long long)j->key_size, (int)j->audio_track);
}

/* Values are stored as integers in thousandths so the file never depends on
 * the C locale's decimal separator. */
static void write_value(FILE* f, double v)
{
    if (v != v) fputs(" nan", f);
    else if (v < -1e6) fputs(" -inf", f);
    else fprintf(f, " %lld", (long long)floor(v * 1000.0 + 0.5));
}

static int read_value(const char** p, double* out)
{
    const char* s = *p;
    long long v = 0;
    int neg = 0, digits = 0;
    while (*s == ' ') s++;
    if (strncmp(s, "nan", 3) == 0) { *out = NAN; *p = s + 3; return 1; }
    if (strncmp(s, "-inf", 4) == 0) { *out = -HUGE_VAL; *p = s + 4; return 1; }
    if (*s == '-') { neg = 1; s++; }
    for (; *s >= '0' && *s <= '9'; s++, digits++) v = v * 10 + (*s - '0');
    if (!digits) return 0;
    *out = (double)(neg ? -v : v) / 1000.0;
    *p = s;
    return 1;
}

static int cache_read(const fa_scan_job* j, fa_loudness* out)
{
    char path[1024], line[256];
    const char* p;
    FILE* f;
    int ok;
    cache_path(j, path, sizeof(path));
    f = fa_fopen_utf8(path, "rb");
    if (!f) return 0;
    ok = fgets(line, sizeof(line), f) && strncmp(line, FA_CACHE_MAGIC, strlen(FA_CACHE_MAGIC)) == 0 &&
         fgets(line, sizeof(line), f);
    fclose(f);
    if (!ok) return 0;
    p = line;
    if (!read_value(&p, &out->integrated_lufs) || !read_value(&p, &out->range_lu) || !read_value(&p, &out->true_peak_dbtp))
        return 0;
    return 1;
}

static void cache_write(const fa_scan_job* j, const fa_loudness* l)
{
    char path[1024], tmp[1100];
    FILE* f;
    fa_mkdir_utf8(j->engine->config.cache_directory);
    cache_path(j, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.%llx.tmp", path, (unsigned long long)fa_thread_current_id());
    f = fa_fopen_utf8(tmp, "wb");
    if (!f) return;
    fprintf(f, "%s\n", FA_CACHE_MAGIC);
    write_value(f, l->integrated_lufs);
    write_value(f, l->range_lu);
    write_value(f, l->true_peak_dbtp);
    fputc('\n', f);
    if (fclose(f) != 0 || fa_replace_file(tmp, path) != 0) remove(tmp);
}

/* ---- Tasks -------------------------------------------------------------------- */

static fa_result open_source(const fa_scan_job* j, fa_source* src)
{
    if (!j->path) return fa_source_open_memory_ref(src, j->mem, j->mem_size);
    return fa_source_open_path_window(src, j->path, j->path_offset, j->path_length);
}

static void set_result(fa_scan_job* j, double lufs, double range, double peak, fa_loudness_source source)
{
    j->result.integrated_lufs = lufs;
    j->result.range_lu = range;
    j->result.true_peak_dbtp = peak;
    j->result.is_final = 1;
    j->result.source = (int32_t)source;
    j->status = FA_OK;
}

/* Returns 1 when the job is decided (tags, cache or error), 0 when chunks are planned. */
static int run_prepare(fa_scan_job* j)
{
    fa_source src;
    fa_decoder dec;
    fa_result r;
    int fast;
    fa_frames length;

    if (fa_load32(&j->cancel)) return 1;
    r = open_source(j, &src);
    if (r != FA_OK) { j->status = r; return 1; }

    if (j->use_tags) {
        fa_tag_loudness tags;
        fa_tags_read_loudness(&src, &tags);
        j->album_lufs = tags.album_lufs;
        if (tags.track_lufs == tags.track_lufs) {
            set_result(j, tags.track_lufs, NAN, tags.track_peak_db, FA_LOUDNESS_TAGS);
            fa_source_close(&src);
            return 1;
        }
    }
    if (j->engine->config.cache_directory && j->engine->config.cache_directory[0] && content_key(&src, &j->key)) {
        j->key = midi_key(&src, &j->env, j->key);
        j->have_key = 1;
        j->key_size = src.length;
        if (cache_read(j, &j->result)) {
            set_result(j, j->result.integrated_lufs, j->result.range_lu, j->result.true_peak_dbtp, FA_LOUDNESS_CACHE);
            fa_source_close(&src);
            return 1;
        }
    }

    fa_source_seek(&src, 0);
    r = fa_decoder_open(&dec, &src, j->name_hint, NULL, j->audio_track, &j->env);
    if (r != FA_OK) { j->status = r; fa_source_close(&src); return 1; }
    j->rate = dec.format.sample_rate;
    j->channels = dec.format.channels;
    length = dec.length;
    fast = dec.vt->fast_seek && dec.seekable && length > 0;
    fa_decoder_close(&dec);
    fa_source_close(&src);

    if (fast) {
        fa_frames sublen = (j->rate + 5) / 10;
        fa_frames per = length / ((fa_frames)j->engine->scan_thread_count * 2 + 2);
        fa_frames min = (fa_frames)j->rate * FA_SCAN_MIN_CHUNK_S;
        fa_frames chunk = per > min ? per : min;
        chunk = (chunk + sublen - 1) / sublen * sublen;
        j->chunk_frames = chunk;
        j->nchunks = (int)((length + chunk - 1) / chunk);
        if (j->nchunks < 1) j->nchunks = 1;
    } else {
        j->chunk_frames = 0;
        j->nchunks = 1;
    }
    j->chunks = (scan_chunk*)calloc((size_t)j->nchunks, sizeof(scan_chunk));
    if (!j->chunks) { j->status = FA_ERROR_OUT_OF_MEMORY; return 1; }
    return 0;
}

static void run_chunk(fa_scan_job* j, int k)
{
    scan_chunk* c = &j->chunks[k];
    fa_source src;
    fa_decoder dec;
    fa_meter m;
    float* buf = NULL;
    fa_frames start = (fa_frames)k * j->chunk_frames, warm = 0, remaining;
    fa_result r;

    c->r = FA_OK;
    if (fa_load32(&j->cancel)) return;
    r = open_source(j, &src);
    if (r != FA_OK) { c->r = r; return; }
    r = fa_decoder_open(&dec, &src, j->name_hint, NULL, j->audio_track, &j->env);
    if (r != FA_OK) { c->r = r; fa_source_close(&src); return; }
    if (dec.format.sample_rate != j->rate || dec.format.channels != j->channels) r = FA_ERROR_UNSUPPORTED_FORMAT;
    if (r == FA_OK && k > 0) {
        warm = (fa_frames)j->rate * FA_SCAN_WARMUP_S;
        if (warm > start) warm = start;
        r = fa_decoder_seek(&dec, start - warm);
    }
    if (r == FA_OK) r = fa_meter_init(&m, j->rate, j->channels, 1, 0);
    if (r != FA_OK) { c->r = r; fa_decoder_close(&dec); fa_source_close(&src); return; }
    m.skip = warm;
    buf = (float*)malloc((size_t)FA_SCAN_READ * j->channels * sizeof(float));
    if (!buf) r = FA_ERROR_OUT_OF_MEMORY;

    remaining = k == j->nchunks - 1 ? INT64_MAX : j->chunk_frames + warm;
    while (r == FA_OK && remaining > 0 && !fa_load32(&j->cancel)) {
        fa_frames want = remaining < FA_SCAN_READ ? remaining : FA_SCAN_READ, got = 0;
        fa_result rr = fa_decoder_read(&dec, buf, want, &got);
        if (got <= 0) { if (rr != FA_ERROR_END_OF_STREAM && rr != FA_OK) r = rr; break; }
        r = fa_meter_process(&m, buf, got);
        remaining -= got;
    }
    free(buf);
    /* The file's last chunk measures the peaks between its final samples too. */
    if (r == FA_OK && k == j->nchunks - 1 && !fa_load32(&j->cancel)) fa_truepeak_flush(m.tp);
    if (r == FA_OK) {
        c->subs = m.subs;          /* take the list; the meter must not free it */
        c->nsubs = m.nsubs;
        c->peak = m.tp->peak;
        m.subs = NULL;
    }
    c->r = r;
    fa_meter_free(&m);
    fa_decoder_close(&dec);
    fa_source_close(&src);
}

static void run_merge(fa_scan_job* j)
{
    int64_t total = 0, at = 0;
    double* all;
    float peak = 0.0f;
    int k;
    for (k = 0; k < j->nchunks; k++) {
        if (j->chunks[k].r != FA_OK) { j->status = j->chunks[k].r; return; }
        total += j->chunks[k].nsubs;
        if (j->chunks[k].peak > peak) peak = j->chunks[k].peak;
    }
    all = (double*)malloc((size_t)(total > 0 ? total : 1) * sizeof(double));
    if (!all) { j->status = FA_ERROR_OUT_OF_MEMORY; return; }
    for (k = 0; k < j->nchunks; k++) {
        if (j->chunks[k].nsubs) memcpy(all + at, j->chunks[k].subs, (size_t)j->chunks[k].nsubs * sizeof(double));
        at += j->chunks[k].nsubs;
    }
    set_result(j, fa_loudness_integrated(all, total), fa_loudness_range(all, total),
               peak > 0.0f ? 20.0 * log10((double)peak) : -HUGE_VAL, FA_LOUDNESS_SCAN);
    free(all);
    if (j->have_key) cache_write(j, &j->result);
}

/* ---- Scheduling ----------------------------------------------------------------- */

/* Caller holds scan_lock. After this the job may be freed by its owner. */
static void finish_locked(fa_engine* e, fa_scan_job* j)
{
    fa_scan_job** pp;
    for (pp = &e->scan_queue; *pp; pp = &(*pp)->next)
        if (*pp == j) { *pp = j->next; break; }
    j->next = NULL;
    j->finished = 1;
    fa_cond_broadcast(&e->scan_cond);
}

/* Hands the result to the stream, outside scan_lock. */
static void deliver(fa_scan_job* j)
{
    if (j->stream && !fa_load32(&j->cancel)) {
        if (j->status == FA_OK) fa_stream_loudness_ready(j->stream, &j->result, j->album_lufs);
        else if (j->album_lufs == j->album_lufs) {
            fa_loudness none;
            memset(&none, 0, sizeof(none));
            none.integrated_lufs = NAN;
            fa_stream_loudness_ready(j->stream, &none, j->album_lufs);
        }
    }
}

/* Takes and runs one task (of `only` if given). Caller holds scan_lock; it is
 * released while the task runs. Returns 0 if there was nothing to do. */
static int run_one_locked(fa_engine* e, fa_scan_job* only)
{
    fa_scan_job* j;
    int task = -2;
    for (j = e->scan_queue; j; j = j->next) {
        if (only && j != only) continue;
        if (!j->prepare_taken) { j->prepare_taken = 1; task = -1; break; }
        if (j->plan_ready && j->next_chunk < j->nchunks) { task = j->next_chunk++; break; }
    }
    if (task == -2) return 0;
    fa_mutex_unlock(&e->scan_lock);

    if (task == -1) {
        int decided = run_prepare(j);
        if (decided) deliver(j);
        fa_mutex_lock(&e->scan_lock);
        if (decided || fa_load32(&j->cancel)) finish_locked(e, j);
        else { j->plan_ready = 1; fa_cond_broadcast(&e->scan_cond); }
        return 1;
    }

    run_chunk(j, task);
    fa_mutex_lock(&e->scan_lock);
    if (++j->chunks_done == j->nchunks) {
        j->merging = 1;            /* fa_scan_release must wait for us now */
        fa_mutex_unlock(&e->scan_lock);
        if (!fa_load32(&j->cancel)) { run_merge(j); deliver(j); }
        fa_mutex_lock(&e->scan_lock);
        finish_locked(e, j);
    }
    return 1;
}

static void scan_thread_main(void* arg)
{
    fa_engine* e = (fa_engine*)arg;
    fa_mutex_lock(&e->scan_lock);
    while (!fa_load32(&e->quit)) {
        if (!run_one_locked(e, NULL)) fa_cond_wait_ms(&e->scan_cond, &e->scan_lock, 100);
    }
    fa_mutex_unlock(&e->scan_lock);
}

/* Starts the scan threads on first use: one per core, minus one left for
 * playback, at least one. */
static void start_threads_locked(fa_engine* e)
{
    uint32_t n = fa_cpu_count(), i;
    if (e->scan_started) return;
    e->scan_started = 1;
    n = n > 1 ? n - 1 : 1;
    if (n > FA_SCAN_MAX_THREADS) n = FA_SCAN_MAX_THREADS;
    e->scan_threads = (fa_thread*)calloc(n, sizeof(fa_thread));
    if (!e->scan_threads) return;
    for (i = 0; i < n; i++) {
        if (fa_thread_create(&e->scan_threads[i], scan_thread_main, e) != 0) break;
        e->scan_thread_count++;
    }
}

static void enqueue(fa_engine* e, fa_scan_job* j)
{
    fa_scan_job** pp;
    fa_mutex_lock(&e->scan_lock);
    start_threads_locked(e);
    for (pp = &e->scan_queue; *pp; pp = &(*pp)->next) {}
    *pp = j;
    fa_cond_broadcast(&e->scan_cond);
    fa_mutex_unlock(&e->scan_lock);
}

static fa_scan_job* job_new(fa_engine* e)
{
    fa_scan_job* j = (fa_scan_job*)calloc(1, sizeof(*j));
    if (!j) return NULL;
    j->engine = e;
    j->audio_track = -1;
    j->album_lufs = NAN;
    j->status = FA_ERROR_GENERIC;
    j->result.integrated_lufs = j->result.range_lu = j->result.true_peak_dbtp = NAN;
    j->env.midi_rate = fa_engine_midi_rate(e);
    return j;
}

static void job_free(fa_scan_job* j)
{
    int k;
    if (!j) return;
    if (j->chunks) for (k = 0; k < j->nchunks; k++) free(j->chunks[k].subs);
    free(j->chunks);
    free(j->path);
    fa_soundfont_release(j->env.soundfont);
    free(j);
}

static char* dup_string(const char* s)
{
    size_t n = strlen(s) + 1;
    char* d = (char*)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

fa_scan_job* fa_scan_start_stream(struct fa_stream* s, int use_tags)
{
    fa_scan_job* j;
    if (!s->path && s->source.kind != FA_SOURCE_MEMORY) return NULL; /* app I/O: cannot read it twice */
    j = job_new(s->engine);
    if (!j) return NULL;
    j->stream = s;
    j->use_tags = use_tags;
    j->audio_track = s->audio_track;
    j->env.soundfont = fa_soundfont_retain(s->soundfont);
    if (s->path) {
        j->path = dup_string(s->path);
        if (!j->path) { job_free(j); return NULL; }
        j->path_offset = s->path_offset;
        j->path_length = s->path_length;
        memcpy(j->entry_name, s->path_name, sizeof(j->entry_name));
        j->name_hint = s->path_length > 0 ? j->entry_name : j->path;
    } else {
        j->mem = s->source.mem;
        j->mem_size = s->source.mem_size;
    }
    enqueue(s->engine, j);
    return j;
}

void fa_scan_release(fa_scan_job* j)
{
    fa_engine* e;
    if (!j) return;
    e = j->engine;
    fa_mutex_lock(&e->scan_lock);
    fa_store32(&j->cancel, 1);
    if (!j->finished) {
        if (!j->prepare_taken) finish_locked(e, j);
        else if (j->plan_ready) {
            /* Chunks nobody started are dropped; running ones stop at their next
             * read. If none is running and nobody is merging, the job is done here;
             * otherwise the thread that ends the last chunk (or the merge) finishes it. */
            j->chunks_done += j->nchunks - j->next_chunk;
            j->next_chunk = j->nchunks;
            if (j->chunks_done == j->nchunks && !j->merging) finish_locked(e, j);
        }
    }
    while (!j->finished) fa_cond_wait_ms(&e->scan_cond, &e->scan_lock, 10);
    fa_mutex_unlock(&e->scan_lock);
    job_free(j);
}

void fa_scan_shutdown(fa_engine* e)
{
    uint32_t i;
    fa_mutex_lock(&e->scan_lock);
    fa_cond_broadcast(&e->scan_cond);
    fa_mutex_unlock(&e->scan_lock);
    for (i = 0; i < e->scan_thread_count; i++) fa_thread_join(&e->scan_threads[i]);
    free(e->scan_threads);
    e->scan_threads = NULL;
    e->scan_thread_count = 0;
}

FA_API fa_result FA_CALL fa_scan_loudness_path(fa_engine* engine, const char* path, fa_loudness* out_loudness)
{
    fa_scan_job* j;
    fa_result r;
    if (!engine || !path || !out_loudness) return FA_ERROR_INVALID_ARG;
    j = job_new(engine);
    if (!j) return FA_ERROR_OUT_OF_MEMORY;
    j->path = dup_string(path);
    if (!j->path) { job_free(j); return FA_ERROR_OUT_OF_MEMORY; }
    j->name_hint = j->path;
    j->env.soundfont = fa_engine_soundfont(engine);
    if (!j->env.soundfont) {
        /* A MIDI file and no soundfont from the app: the OS's own instruments, as a stream would play it. */
        uint8_t head[64];
        size_t n = 0;
        FILE* f = fa_fopen_utf8(path, "rb");
        if (f) { n = fread(head, 1, sizeof(head), f); fclose(f); }
        if (fa_probe_midi(head, n, 0)) j->env.soundfont = fa_engine_system_soundfont(engine);
    }
    enqueue(engine, j);

    /* The calling thread scans too, so a standalone scan uses every core. */
    fa_mutex_lock(&engine->scan_lock);
    while (!j->finished) {
        if (!run_one_locked(engine, j) && !j->finished) fa_cond_wait_ms(&engine->scan_cond, &engine->scan_lock, 10);
    }
    fa_mutex_unlock(&engine->scan_lock);

    r = j->status;
    if (r == FA_OK) {
        *out_loudness = j->result;
        out_loudness->gain_db = fa_engine_norm_gain_db(engine, j->result.integrated_lufs);
    }
    job_free(j);
    return r;
}
