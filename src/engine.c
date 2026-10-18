#include "fa_internal.h"
#include <stdlib.h>
#include <string.h>

FA_API uint32_t FA_CALL fa_version(void)
{
    return ((uint32_t)FA_VERSION_MAJOR << 16) | ((uint32_t)FA_VERSION_MINOR << 8) | (uint32_t)FA_VERSION_PATCH;
}

FA_API const char* FA_CALL fa_result_string(fa_result result)
{
    switch (result) {
    case FA_OK: return "ok";
    case FA_ERROR_GENERIC: return "generic error";
    case FA_ERROR_INVALID_ARG: return "invalid argument";
    case FA_ERROR_OUT_OF_MEMORY: return "out of memory";
    case FA_ERROR_NOT_FOUND: return "not found";
    case FA_ERROR_UNSUPPORTED_FORMAT: return "unsupported format";
    case FA_ERROR_DEVICE: return "device error";
    case FA_ERROR_IO: return "i/o error";
    case FA_ERROR_NETWORK: return "network error";
    case FA_ERROR_NOT_SEEKABLE: return "not seekable";
    case FA_ERROR_BUSY: return "busy";
    case FA_ERROR_END_OF_STREAM: return "end of stream";
    case FA_ERROR_NOT_IMPLEMENTED: return "not implemented";
    case FA_ERROR_PROVIDER_MISSING: return "provider missing";
    case FA_ERROR_NO_DISC: return "no disc";
    case FA_ERROR_NO_SOUNDFONT: return "no soundfont for MIDI";
    case FA_ERROR_PROTECTED_CONTENT: return "protected content";
    case FA_ERROR_TOO_LARGE: return "too large to unpack in memory";
    default: return "unknown result";
    }
}

FA_API void FA_CALL fa_engine_config_init(fa_engine_config* config)
{
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->struct_size = (uint32_t)sizeof(*config);
}

/* ---- Decode workers ------------------------------------------------------- */

void fa_engine_wake_workers(fa_engine* e)
{
    fa_mutex_lock(&e->list_lock);
    fa_cond_broadcast(&e->work_cond);
    fa_mutex_unlock(&e->list_lock);
}

void fa_engine_track_stream(fa_engine* e, struct fa_stream* s, int add)
{
    struct fa_stream** pp;
    fa_mutex_lock(&e->list_lock);
    for (pp = &e->streams; *pp; pp = &(*pp)->next)
        if (*pp == s) { *pp = s->next; break; }
    s->next = NULL;
    /* Leaving the list: events it spilled would no longer be found. */
    if (!add) fa_event_rescue_stream(e, s);
    if (add) {
        s->next = e->streams;
        e->streams = s;
        fa_cond_broadcast(&e->work_cond);
    }
    fa_mutex_unlock(&e->list_lock);
}

/* Each worker repeatedly takes the attached stream with the emptiest buffer that
 * nobody else is decoding (try-lock on its decode lock) and tops it up. A stream
 * another worker is busy with is skipped, so one slow source never keeps idle
 * workers from the others. With nothing to do a worker sleeps until woken or
 * 5 ms pass; the audio thread never has to signal anything. */
static void worker_main(void* arg)
{
    fa_engine* e = (fa_engine*)arg;
    fa_mutex_lock(&e->list_lock);
    while (!fa_load32(&e->quit)) {
        struct fa_stream* s;
        struct fa_stream* pick = NULL;
        double best = 2.0;
        fa_event_collect_locked(e);  /* keeps the audio thread's event queues drained */
        for (s = e->streams; s; s = s->next) {
            double fill;
            if (!fa_stream_needs_work(s)) continue;
            fill = (double)fa_ring_readable(&s->ring) / (double)s->ring.capacity;
            if (fill >= best || !fa_mutex_trylock(&s->decode_lock)) continue;
            if (pick) fa_mutex_unlock(&pick->decode_lock);
            best = fill;
            pick = s;
        }
        if (pick) {
            fa_mutex_unlock(&e->list_lock);
            /* Top up to full, in turns of at most half a buffer so one stream
             * cannot starve the others. */
            fa_stream_fill(pick, pick->ring.capacity / 2);
            fa_mutex_unlock(&pick->decode_lock);
            fa_mutex_lock(&e->list_lock);
            continue;
        }
        fa_cond_wait_ms(&e->work_cond, &e->list_lock, 5);
    }
    fa_mutex_unlock(&e->list_lock);
}

void fa_event_thread_main(void* arg);

static char* copy_string(const char* s)
{
    size_t len = strlen(s) + 1;
    char* d = (char*)malloc(len);
    if (d) memcpy(d, s, len);
    return d;
}

/* The app's temp directory gets a folder of FatzAudio's own, so the sweep never
 * touches the app's files. One that cannot be made now is tried again when a song
 * needs it (FA_ERROR_IO then). */
static fa_result temp_folder_prepare(fa_engine* e)
{
    const char* dir = e->config.temp_directory;
    size_t n = strlen(dir);
    int sep = dir[n - 1] == '/' || dir[n - 1] == '\\';
    char* folder = (char*)malloc(n + 16);
    if (!folder) return FA_ERROR_OUT_OF_MEMORY;
    snprintf(folder, n + 16, "%s%sfatzaudio-tmp", dir, sep ? "" : "/");
    if (fa_mkdir_utf8(dir) == 0 && fa_mkdir_utf8(folder) == 0) fa_temp_folder_sweep(folder);
    e->temp_folder = folder;
    return FA_OK;
}

static void free_strings(fa_engine* e)
{
    free((char*)e->config.cache_directory);
    free((char*)e->config.temp_directory);
    free(e->temp_folder);
}

FA_API fa_result FA_CALL fa_engine_create(const fa_engine_config* config, fa_engine** out_engine)
{
    fa_engine* e;
    ma_result r;
    uint32_t i, n;
    if (!out_engine) return FA_ERROR_INVALID_ARG;
    *out_engine = NULL;

    e = (fa_engine*)calloc(1, sizeof(*e));
    if (!e) return FA_ERROR_OUT_OF_MEMORY;
    /* Copy only the part of the config the caller's struct_size covers, so callers
     * compiled against an older, shorter struct keep working. */
    fa_engine_config_init(&e->config);
    if (config) {
        size_t sz = config->struct_size;
        if (sz == 0 || sz > sizeof(e->config)) sz = sizeof(e->config);
        memcpy(&e->config, config, sz);
        e->config.struct_size = (uint32_t)sizeof(e->config);
    }
    /* Keep our own copies: the caller's strings (marshalled ones, in .NET) may not outlive this call. */
    if (e->config.cache_directory) {
        e->config.cache_directory = copy_string(e->config.cache_directory);
        if (!e->config.cache_directory) { free(e); return FA_ERROR_OUT_OF_MEMORY; }
    }
    if (e->config.temp_directory && e->config.temp_directory[0]) {
        e->config.temp_directory = copy_string(e->config.temp_directory);
        if (!e->config.temp_directory || temp_folder_prepare(e) != FA_OK) { free_strings(e); free(e); return FA_ERROR_OUT_OF_MEMORY; }
    } else {
        e->config.temp_directory = NULL;
    }

    if (ma_mutex_init(&e->lock) != MA_SUCCESS) { free_strings(e); free(e); return FA_ERROR_GENERIC; }
    if (fa_mutex_init(&e->scan_lock) != 0) { ma_mutex_uninit(&e->lock); free_strings(e); free(e); return FA_ERROR_GENERIC; }
    if (fa_cond_init(&e->scan_cond) != 0) {
        fa_mutex_destroy(&e->scan_lock); ma_mutex_uninit(&e->lock); free_strings(e); free(e);
        return FA_ERROR_GENERIC;
    }
    fa_store32(&e->norm_mode, FA_NORMALIZE_OFF);
    fa_store_float(&e->norm_target, -16.0f);
    fa_store32(&e->limiter_mode, FA_LIMITER_AUTO);
    fa_store_float(&e->limiter_ceiling, -1.0f);
    if (fa_mutex_init(&e->list_lock) != 0 || fa_cond_init(&e->work_cond) != 0 || fa_cond_init(&e->event_cond) != 0) {
        fa_cond_destroy(&e->event_cond); fa_cond_destroy(&e->work_cond); fa_mutex_destroy(&e->list_lock);
        fa_cond_destroy(&e->scan_cond); fa_mutex_destroy(&e->scan_lock);
        ma_mutex_uninit(&e->lock);
        free_strings(e); free(e);
        return FA_ERROR_GENERIC;
    }
    if (e->config.flags & FA_ENGINE_NULL_OUTPUT) {
        ma_backend null_backend = ma_backend_null;
        r = ma_context_init(&null_backend, 1, NULL, &e->context);
    } else {
        r = ma_context_init(NULL, 0, NULL, &e->context);
    }
    if (r != MA_SUCCESS) {
        fa_cond_destroy(&e->event_cond); fa_cond_destroy(&e->work_cond); fa_mutex_destroy(&e->list_lock);
        fa_cond_destroy(&e->scan_cond); fa_mutex_destroy(&e->scan_lock);
        ma_mutex_uninit(&e->lock);
        free_strings(e); free(e);
        return FA_ERROR_DEVICE;
    }

    n = e->config.worker_threads;
    if (n == 0) n = fa_cpu_count();
    if (n > FA_MAX_WORKERS) n = FA_MAX_WORKERS;
    for (i = 0; i < n; i++) {
        if (fa_thread_create(&e->workers[i], worker_main, e) != 0) break;
        e->worker_count++;
    }
    if (e->worker_count == 0 || fa_thread_create(&e->event_thread, fa_event_thread_main, e) != 0) {
        fa_engine_destroy(e);
        return FA_ERROR_GENERIC;
    }
    *out_engine = e;
    return FA_OK;
}

void fa_engine_free(fa_engine* e)
{
    ma_context_uninit(&e->context);
    ma_mutex_uninit(&e->lock);
    fa_cond_destroy(&e->scan_cond);
    fa_mutex_destroy(&e->scan_lock);
    fa_cond_destroy(&e->event_cond);
    fa_cond_destroy(&e->work_cond);
    fa_mutex_destroy(&e->list_lock);
    free(e->pending);
    free_strings(e);
    fa_soundfont_release(e->soundfont);
    free(e);
}

FA_API void FA_CALL fa_engine_destroy(fa_engine* engine)
{
    uint32_t i;
    int on_event_thread;
    if (!engine) return;
    /* Devices left open would keep calling into freed streams: close them first. */
    while (engine->devices) fa_device_close(engine->devices);

    on_event_thread = engine->event_thread.impl &&
        (int64_t)fa_thread_current_id() == fa_load64(&engine->event_thread_id);
    fa_store32(&engine->quit, 1);
    fa_mutex_lock(&engine->list_lock);
    if (on_event_thread) engine->destroy_on_event_thread = 1;
    fa_cond_broadcast(&engine->work_cond);
    fa_cond_broadcast(&engine->event_cond);
    fa_mutex_unlock(&engine->list_lock);
    for (i = 0; i < engine->worker_count; i++) fa_thread_join(&engine->workers[i]);
    fa_scan_shutdown(engine);
    if (on_event_thread) {
        /* Called from an event callback: the event thread cannot join itself. It
         * finishes delivering, then frees the engine as it exits. */
        fa_thread_detach(&engine->event_thread);
        return;
    }
    fa_thread_join(&engine->event_thread); /* delivers what is queued and frees closed streams */
    fa_engine_free(engine);
}
