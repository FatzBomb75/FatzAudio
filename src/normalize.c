/* Loudness normalization: engine settings, the per-stream gain and the
 * loudness API. The measuring is in loudness.c, the background scans in scan.c,
 * and the gain itself is applied (with its glide) in the mixer, stream.c.
 *
 * Lock order: stream api_lock -> engine list_lock -> stream norm_lock -> engine
 * scan_lock. Nothing that holds norm_lock posts events (that takes list_lock). */
#include "fa_internal.h"
#include <math.h>
#include <string.h>

#define FA_NORM_MAX_BOOST_DB 12.0f
#define FA_NORM_MAX_CUT_DB -30.0f

float fa_engine_norm_gain_db(fa_engine* e, double lufs)
{
    float g;
    if (fa_load32(&e->norm_mode) == FA_NORMALIZE_OFF) return 0.0f;
    if (!(lufs > -70.0)) return 0.0f;   /* unknown (NAN) or silence: leave it alone */
    g = (float)((double)fa_load_float(&e->norm_target) - lufs);
    if (g > FA_NORM_MAX_BOOST_DB) g = FA_NORM_MAX_BOOST_DB;
    if (g < FA_NORM_MAX_CUT_DB) g = FA_NORM_MAX_CUT_DB;
    return g;
}

float fa_stream_norm_target_db(struct fa_stream* s)
{
    int mode = fa_load32(&s->engine->norm_mode);
    float lufs;
    if (mode == FA_NORMALIZE_OFF || (s->flags & (FA_OPEN_NO_NORMALIZE | FA_OPEN_DECODE_ONLY))) return 0.0f;
    lufs = NAN;
    if (mode == FA_NORMALIZE_ALBUM) lufs = fa_load_float(&s->norm_album);
    if (lufs != lufs) lufs = fa_load_float(&s->norm_track);
    if (lufs != lufs) lufs = fa_load_float(&s->norm_est);
    return fa_engine_norm_gain_db(s->engine, lufs);
}

/* Starts tags/cache/scan for the stream once. */
void fa_stream_start_analysis(struct fa_stream* s)
{
    fa_mutex_lock(&s->norm_lock);
    if (!s->scan_job && !s->loud.is_final && !s->analysis_tried) {
        s->analysis_tried = 1;
        s->scan_job = fa_scan_start_stream(s, 1);
    }
    fa_mutex_unlock(&s->norm_lock);
}

void fa_stream_loudness_ready(struct fa_stream* s, const fa_loudness* l, double album_lufs)
{
    int post = 0;
    fa_mutex_lock(&s->norm_lock);
    if (s->loud.source != FA_LOUDNESS_APP) {
        if (l->integrated_lufs == l->integrated_lufs) {
            s->loud = *l;
            s->loud.gain_db = 0.0;
            fa_store_float(&s->norm_track, (float)l->integrated_lufs);
            post = 1;
        }
        if (album_lufs == album_lufs) fa_store_float(&s->norm_album, (float)album_lufs);
    }
    fa_mutex_unlock(&s->norm_lock);
    if (post) {
        fa_event ev;
        ev.type = FA_EVENT_LOUDNESS_FINAL;
        ev.stream = s;
        fa_mutex_lock(&s->api_lock);      /* the position reads attach state */
        ev.frame = fa_stream_position_now(s);
        fa_mutex_unlock(&s->api_lock);
        ev.value = 0;
        fa_engine_post_event(s->engine, &ev);
    }
}

/* ---- Public API ------------------------------------------------------------------ */

FA_API fa_result FA_CALL fa_engine_set_normalization(fa_engine* engine, fa_normalize_mode mode, double target_lufs)
{
    struct fa_stream* s;
    if (!engine || (int)mode < FA_NORMALIZE_OFF || mode > FA_NORMALIZE_ALBUM) return FA_ERROR_INVALID_ARG;
    if (mode != FA_NORMALIZE_OFF) {
        if (!(target_lufs >= -60.0 && target_lufs <= 0.0)) return FA_ERROR_INVALID_ARG;
        fa_store_float(&engine->norm_target, (float)target_lufs);
    }
    fa_store32(&engine->norm_mode, (int32_t)mode);
    if (mode != FA_NORMALIZE_OFF) {
        fa_mutex_lock(&engine->list_lock);   /* attached streams cannot be closed meanwhile */
        for (s = engine->streams; s; s = s->next)
            if (!(s->flags & FA_OPEN_NO_NORMALIZE)) fa_stream_start_analysis(s);
        fa_mutex_unlock(&engine->list_lock);
    }
    return FA_OK;
}

FA_API fa_result FA_CALL fa_engine_set_limiter(fa_engine* engine, fa_limiter_mode mode, double ceiling_dbtp)
{
    if (!engine || (int)mode < FA_LIMITER_AUTO || mode > FA_LIMITER_OFF) return FA_ERROR_INVALID_ARG;
    if (!(ceiling_dbtp >= -20.0 && ceiling_dbtp <= 0.0)) return FA_ERROR_INVALID_ARG;
    fa_store_float(&engine->limiter_ceiling, (float)ceiling_dbtp);
    fa_store32(&engine->limiter_mode, (int32_t)mode);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_loudness(fa_stream* stream, fa_loudness* out_loudness)
{
    if (!stream || !out_loudness) return FA_ERROR_INVALID_ARG;
    fa_stream_start_analysis(stream);
    fa_mutex_lock(&stream->norm_lock);
    if (stream->loud.is_final) {
        *out_loudness = stream->loud;
    } else {
        float est = fa_load_float(&stream->norm_est);
        memset(out_loudness, 0, sizeof(*out_loudness));
        out_loudness->integrated_lufs = est;
        out_loudness->range_lu = NAN;
        out_loudness->true_peak_dbtp = NAN;
        out_loudness->source = est == est ? FA_LOUDNESS_ESTIMATE : FA_LOUDNESS_NONE;
    }
    fa_mutex_unlock(&stream->norm_lock);
    out_loudness->gain_db = fa_stream_norm_target_db(stream);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_set_loudness(fa_stream* stream, const fa_loudness* loudness)
{
    fa_scan_job* job;
    if (!stream || !loudness || loudness->integrated_lufs != loudness->integrated_lufs) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->norm_lock);
    stream->loud = *loudness;
    stream->loud.is_final = 1;
    stream->loud.source = FA_LOUDNESS_APP;
    stream->loud.gain_db = 0.0;
    fa_store_float(&stream->norm_track, (float)loudness->integrated_lufs);
    fa_store_float(&stream->norm_album, NAN);   /* the app's value wins in album mode too */
    job = stream->scan_job;
    stream->scan_job = NULL;
    fa_mutex_unlock(&stream->norm_lock);
    fa_scan_release(job);
    return FA_OK;
}
