/* The stream equalizer (fa_stream_set_eq) and its presets.
 *
 * API threads write the bands into fa_eq_shared (one writer at a time, seqlocked);
 * the audio thread takes a consistent copy at the start of each mix and glides its
 * own bands toward it. Each band is an RBJ "Audio EQ Cookbook" biquad in transposed
 * direct form II. A band at 0 dB is an identity and is skipped (its state cleared,
 * which an identity biquad keeps at zero anyway), so a flat or bypassed EQ costs
 * nothing. */
#include "fa_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#  include <emmintrin.h>
#endif

#define EQ_GLIDE_S 0.010f       /* time constant of a change */
#define EQ_MIN_HZ 10.0f
#define EQ_MAX_DB 24.0f

/* ---- shared (API side) ------------------------------------------------------ */

/* Writers hold the stream's api_lock; seq tells the audio thread a write is under way. */
static void begin_write(fa_eq_shared* sh) { fa_fetch_add32(&sh->seq, 1); }
static void end_write(fa_eq_shared* sh) { fa_fetch_add32(&sh->seq, 1); }

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

void fa_eq_shared_init(fa_eq_shared* sh)
{
    memset(sh, 0, sizeof(*sh));
    fa_store32(&sh->enabled, 1);
}

static void write_band(fa_eq_shared* sh, uint32_t i, const fa_eq_band* b)
{
    float q = b->q > 0.0f ? b->q : 1.41421356f;
    fa_store32(&sh->type[i], (int32_t)b->type);
    fa_store_float(&sh->freq[i], clampf(b->frequency_hz, EQ_MIN_HZ, 100000.0f));
    fa_store_float(&sh->q[i], clampf(q, 0.1f, 20.0f));
    fa_store_float(&sh->gain[i], clampf(b->gain_db, -EQ_MAX_DB, EQ_MAX_DB));
}

/* The stream's EQ, made on first use: a stream that never sets one carries none.
 * Caller holds api_lock. NULL if out of memory. */
static fa_stream_eq* eq_make_locked(fa_stream* stream)
{
    fa_stream_eq* eq = (fa_stream_eq*)fa_load_ptr(&stream->eq);
    if (eq) return eq;
    eq = (fa_stream_eq*)calloc(1, sizeof(*eq));
    if (!eq) return NULL;
    fa_eq_shared_init(&eq->shared);
    fa_eq_reset(&eq->st);
    fa_exchange_ptr(&stream->eq, eq);   /* complete before the audio thread can see it */
    return eq;
}

fa_result fa_eq_check_bands(const fa_eq_band* bands, uint32_t count)
{
    uint32_t i;
    if (count > FA_EQ_MAX_BANDS || (count > 0 && !bands)) return FA_ERROR_INVALID_ARG;
    for (i = 0; i < count; i++)
        if ((int)bands[i].type < FA_EQ_PEAK || (int)bands[i].type > FA_EQ_HIGH_SHELF ||
            !(bands[i].frequency_hz == bands[i].frequency_hz) || !(bands[i].gain_db == bands[i].gain_db) ||
            !(bands[i].q == bands[i].q))
            return FA_ERROR_INVALID_ARG;   /* unknown type or NaN */
    return FA_OK;
}

void fa_eq_shared_set(fa_eq_shared* sh, const fa_eq_band* bands, uint32_t count)
{
    uint32_t i;
    begin_write(sh);
    for (i = 0; i < count; i++) write_band(sh, i, &bands[i]);
    fa_store32(&sh->count, (int32_t)count);
    end_write(sh);
}

uint32_t fa_eq_shared_get(fa_eq_shared* sh, fa_eq_band* out_bands, uint32_t capacity)
{
    uint32_t i, n = sh ? (uint32_t)fa_load32(&sh->count) : 0;
    for (i = 0; i < n && i < capacity; i++) {
        out_bands[i].type = (fa_eq_type)fa_load32(&sh->type[i]);
        out_bands[i].frequency_hz = fa_load_float(&sh->freq[i]);
        out_bands[i].q = fa_load_float(&sh->q[i]);
        out_bands[i].gain_db = fa_load_float(&sh->gain[i]);
    }
    return n;
}

FA_API fa_result FA_CALL fa_stream_set_eq(fa_stream* stream, const fa_eq_band* bands, uint32_t count)
{
    fa_stream_eq* eq;
    if (!stream || fa_eq_check_bands(bands, count) != FA_OK) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->api_lock);
    /* No bands on a stream without an EQ changes nothing: it stays without one. */
    eq = count == 0 ? (fa_stream_eq*)fa_load_ptr(&stream->eq) : eq_make_locked(stream);
    if (eq) fa_eq_shared_set(&eq->shared, bands, count);
    fa_mutex_unlock(&stream->api_lock);
    return eq || count == 0 ? FA_OK : FA_ERROR_OUT_OF_MEMORY;
}

FA_API fa_result FA_CALL fa_stream_get_eq(fa_stream* stream, fa_eq_band* out_bands, uint32_t capacity, uint32_t* out_count)
{
    fa_stream_eq* eq;
    uint32_t n;
    if (!stream || !out_count || (capacity > 0 && !out_bands)) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->api_lock);   /* keeps writers out; the audio thread sees no change */
    eq = (fa_stream_eq*)fa_load_ptr(&stream->eq);
    n = fa_eq_shared_get(eq ? &eq->shared : NULL, out_bands, capacity);
    fa_mutex_unlock(&stream->api_lock);
    *out_count = n;
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_set_eq_gain(fa_stream* stream, uint32_t band, float gain_db)
{
    fa_result r = FA_OK;
    fa_stream_eq* eq;
    if (!stream || !(gain_db == gain_db)) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->api_lock);
    eq = (fa_stream_eq*)fa_load_ptr(&stream->eq);
    if (!eq || band >= (uint32_t)fa_load32(&eq->shared.count)) r = FA_ERROR_INVALID_ARG;
    else {
        begin_write(&eq->shared);
        fa_store_float(&eq->shared.gain[band], clampf(gain_db, -EQ_MAX_DB, EQ_MAX_DB));
        end_write(&eq->shared);
    }
    fa_mutex_unlock(&stream->api_lock);
    return r;
}

FA_API fa_result FA_CALL fa_stream_set_eq_enabled(fa_stream* stream, int32_t enabled)
{
    fa_stream_eq* eq;
    if (!stream) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&stream->api_lock);
    /* A stream without an EQ reads as enabled: enabling it changes nothing. */
    eq = enabled ? (fa_stream_eq*)fa_load_ptr(&stream->eq) : eq_make_locked(stream);
    if (eq) {
        begin_write(&eq->shared);
        fa_store32(&eq->shared.enabled, enabled ? 1 : 0);
        end_write(&eq->shared);
    }
    fa_mutex_unlock(&stream->api_lock);
    return eq || enabled ? FA_OK : FA_ERROR_OUT_OF_MEMORY;
}

FA_API fa_result FA_CALL fa_stream_get_eq_enabled(fa_stream* stream, int32_t* out_enabled)
{
    fa_stream_eq* eq;
    if (!stream || !out_enabled) return FA_ERROR_INVALID_ARG;
    eq = (fa_stream_eq*)fa_load_ptr(&stream->eq);
    *out_enabled = eq ? fa_load32(&eq->shared.enabled) : 1;   /* none yet: on, with no bands */
    return FA_OK;
}

/* ---- presets ---------------------------------------------------------------- */

static const float k_octaves[FA_EQ_PRESET_BANDS] = { 31.25f, 62.5f, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };

static const struct { const char* name; signed char db[FA_EQ_PRESET_BANDS]; } k_presets[FA_EQ_PRESET_COUNT] = {
    { "Flat",         {  0,  0,  0,  0,  0,  0,  0,  0,  0,  0 } },
    { "Rock",         {  5,  4,  3,  1, -1, -1,  1,  3,  4,  5 } },
    { "Pop",          { -1,  1,  3,  4,  4,  2,  0, -1, -1, -1 } },
    { "Jazz",         {  3,  2,  1,  2, -1, -1,  0,  1,  2,  3 } },
    { "Classical",    {  0,  0,  0,  0,  0,  0, -2, -3, -3, -4 } },
    { "Dance",        {  6,  5,  2,  0,  0, -2, -2, -1,  1,  2 } },
    { "Hip-hop",      {  5,  5,  3,  1, -1, -1,  1,  0,  1,  2 } },
    { "Acoustic",     {  3,  3,  2,  1,  1,  1,  2,  2,  2,  1 } },
    { "Vocal",        { -2, -2, -1,  1,  3,  4,  3,  1,  0, -1 } },
    { "Spoken word",  { -4, -3, -2,  0,  2,  3,  3,  2,  0, -2 } },
    { "Bass boost",   {  7,  6,  4,  2,  0,  0,  0,  0,  0,  0 } },
    { "Treble boost", {  0,  0,  0,  0,  0,  1,  2,  4,  5,  6 } },
    { "Loudness",     {  5,  4,  2,  0, -1, -1,  0,  2,  4,  5 } },
    { "Live vocal",   { -4, -3, -1, -2, -1,  0,  2,  3,  2,  1 } },
};

FA_API fa_result FA_CALL fa_eq_get_preset(fa_eq_preset preset, fa_eq_band* out_bands, uint32_t* out_count)
{
    uint32_t i;
    if ((int)preset < 0 || preset >= FA_EQ_PRESET_COUNT || !out_bands || !out_count) return FA_ERROR_INVALID_ARG;
    for (i = 0; i < FA_EQ_PRESET_BANDS; i++) {
        out_bands[i].type = FA_EQ_PEAK;
        out_bands[i].frequency_hz = k_octaves[i];
        out_bands[i].q = 1.41421356f;
        out_bands[i].gain_db = (float)k_presets[preset].db[i];
    }
    *out_count = FA_EQ_PRESET_BANDS;
    return FA_OK;
}

FA_API const char* FA_CALL fa_eq_preset_name(fa_eq_preset preset)
{
    if ((int)preset < 0 || preset >= FA_EQ_PRESET_COUNT) return NULL;
    return k_presets[preset].name;
}

/* ---- audio thread ------------------------------------------------------------- */

void fa_eq_reset(fa_eq_state* st)
{
    memset(st, 0, sizeof(*st));
    st->seen = -1;
}

static void design(fa_eq_bandstate* b, uint32_t rate)
{
    double f = b->f, w0, cw, sw, alpha, A, sq, b0, b1, b2, a0, a1, a2;
    if (f > 0.49 * rate) f = 0.49 * rate;
    w0 = 2.0 * FA_PI * f / (double)rate;
    cw = cos(w0);
    sw = sin(w0);
    alpha = sw / (2.0 * (double)b->q);
    A = pow(10.0, (double)b->g / 40.0);
    switch (b->type) {
    case FA_EQ_LOW_SHELF:
        sq = 2.0 * sqrt(A) * alpha;
        b0 = A * ((A + 1) - (A - 1) * cw + sq);
        b1 = 2 * A * ((A - 1) - (A + 1) * cw);
        b2 = A * ((A + 1) - (A - 1) * cw - sq);
        a0 = (A + 1) + (A - 1) * cw + sq;
        a1 = -2 * ((A - 1) + (A + 1) * cw);
        a2 = (A + 1) + (A - 1) * cw - sq;
        break;
    case FA_EQ_HIGH_SHELF:
        sq = 2.0 * sqrt(A) * alpha;
        b0 = A * ((A + 1) + (A - 1) * cw + sq);
        b1 = -2 * A * ((A - 1) + (A + 1) * cw);
        b2 = A * ((A + 1) + (A - 1) * cw - sq);
        a0 = (A + 1) - (A - 1) * cw + sq;
        a1 = 2 * ((A - 1) - (A + 1) * cw);
        a2 = (A + 1) - (A - 1) * cw - sq;
        break;
    default:
        b0 = 1 + alpha * A;
        b1 = -2 * cw;
        b2 = 1 - alpha * A;
        a0 = 1 + alpha / A;
        a1 = -2 * cw;
        a2 = 1 - alpha / A;
        break;
    }
    b->b0 = (float)(b0 / a0);
    b->b1 = (float)(b1 / a0);
    b->b2 = (float)(b2 / a0);
    b->a1 = (float)(a1 / a0);
    b->a2 = (float)(a2 / a0);
}

int fa_eq_begin(fa_eq_state* st, fa_eq_shared* sh, uint32_t rate, int restart)
{
    int32_t s1 = fa_load32(&sh->seq);
    uint32_t i;
    if (restart)   /* new audio (seek, flush, attach): no ringing from the old audio */
        for (i = 0; i < FA_EQ_MAX_BANDS; i++) memset(st->band[i].z, 0, sizeof(st->band[i].z));
    if (rate != st->rate) {
        /* New device rate: same bands, new coefficients, fresh state. */
        st->rate = rate;
        for (i = 0; i < FA_EQ_MAX_BANDS; i++) {
            memset(st->band[i].z, 0, sizeof(st->band[i].z));
            st->band[i].dirty = 1;
        }
    }
    if (s1 != st->seen && !(s1 & 1)) {
        fa_eq_bandstate t[FA_EQ_MAX_BANDS];
        int32_t n = fa_load32(&sh->count), on = fa_load32(&sh->enabled);
        for (i = 0; i < FA_EQ_MAX_BANDS; i++) {
            if ((int32_t)i < n) {
                t[i].type = fa_load32(&sh->type[i]);
                t[i].tf = fa_load_float(&sh->freq[i]);
                t[i].tq = fa_load_float(&sh->q[i]);
                t[i].tg = on ? fa_load_float(&sh->gain[i]) : 0.0f;
            } else {
                t[i].type = st->band[i].type;
                t[i].tf = st->band[i].tf;
                t[i].tq = st->band[i].tq;
                t[i].tg = 0.0f;   /* removed: glides out */
            }
        }
        if (fa_load32(&sh->seq) == s1) {   /* not written meanwhile: take it */
            for (i = 0; i < FA_EQ_MAX_BANDS; i++) {
                fa_eq_bandstate* b = &st->band[i];
                int idle = b->g == 0.0f && b->tg == 0.0f;
                if (idle || t[i].type != b->type) {
                    /* Idle bands (and a changed type) start from the new shape at 0 dB. */
                    if (t[i].type != b->type && !idle) memset(b->z, 0, sizeof(b->z));
                    b->type = t[i].type;
                    b->f = t[i].tf;
                    b->q = t[i].tq;
                    b->dirty = 1;
                }
                b->tf = t[i].tf;
                b->tq = t[i].tq;
                b->tg = t[i].tg;
            }
            st->seen = s1;
        }
    }
    st->active = 0;
    for (i = 0; i < FA_EQ_MAX_BANDS; i++)
        if (st->band[i].g != 0.0f || st->band[i].tg != 0.0f) { st->active = 1; break; }
    return st->active;
}

static float glide(float cur, float target, float k, float snap)
{
    float d = target - cur;
    return (d < snap && d > -snap) ? target : cur + d * k;
}

/* Filters n stereo frames through the `nb` active bands, one frame through every
 * band before the next frame (the bands' recursions overlap instead of each
 * waiting on its own last result), left and right side by side in one vector. The
 * arithmetic per channel is the scalar loop's, in the same order. */
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
static void filter_bands(fa_eq_bandstate** act, uint32_t nb, float* buf, int64_t n)
{
    __m128 b0[FA_EQ_MAX_BANDS], b1[FA_EQ_MAX_BANDS], b2[FA_EQ_MAX_BANDS], a1[FA_EQ_MAX_BANDS], a2[FA_EQ_MAX_BANDS];
    __m128 z0[FA_EQ_MAX_BANDS], z1[FA_EQ_MAX_BANDS];
    uint32_t i;
    int64_t j;
    for (i = 0; i < nb; i++) {
        const fa_eq_bandstate* b = act[i];
        b0[i] = _mm_set1_ps(b->b0); b1[i] = _mm_set1_ps(b->b1); b2[i] = _mm_set1_ps(b->b2);
        a1[i] = _mm_set1_ps(b->a1); a2[i] = _mm_set1_ps(b->a2);
        z0[i] = _mm_setr_ps(b->z[0][0], b->z[1][0], 0.0f, 0.0f);
        z1[i] = _mm_setr_ps(b->z[0][1], b->z[1][1], 0.0f, 0.0f);
    }
    for (j = 0; j < n; j++) {
        __m128 x = _mm_castpd_ps(_mm_load_sd((const double*)(const void*)(buf + j * 2)));
        for (i = 0; i < nb; i++) {
            __m128 y = _mm_add_ps(_mm_mul_ps(b0[i], x), z0[i]);
            z0[i] = _mm_add_ps(_mm_sub_ps(_mm_mul_ps(b1[i], x), _mm_mul_ps(a1[i], y)), z1[i]);
            z1[i] = _mm_sub_ps(_mm_mul_ps(b2[i], x), _mm_mul_ps(a2[i], y));
            x = y;
        }
        _mm_store_sd((double*)(void*)(buf + j * 2), _mm_castps_pd(x));
    }
    for (i = 0; i < nb; i++) {
        float lo[4], hi[4];
        _mm_storeu_ps(lo, z0[i]);
        _mm_storeu_ps(hi, z1[i]);
        act[i]->z[0][0] = lo[0]; act[i]->z[1][0] = lo[1];
        act[i]->z[0][1] = hi[0]; act[i]->z[1][1] = hi[1];
    }
}
#else
static void filter_bands(fa_eq_bandstate** act, uint32_t nb, float* buf, int64_t n)
{
    uint32_t i;
    for (i = 0; i < nb; i++) {
        fa_eq_bandstate* b = act[i];
        const float b0 = b->b0, b1 = b->b1, b2 = b->b2, a1 = b->a1, a2 = b->a2;
        float z00 = b->z[0][0], z01 = b->z[0][1], z10 = b->z[1][0], z11 = b->z[1][1];
        int64_t j;
        for (j = 0; j < n; j++) {
            float x0 = buf[j * 2], x1 = buf[j * 2 + 1];
            float y0 = b0 * x0 + z00, y1 = b0 * x1 + z10;
            z00 = b1 * x0 - a1 * y0 + z01;
            z01 = b2 * x0 - a2 * y0;
            z10 = b1 * x1 - a1 * y1 + z11;
            z11 = b2 * x1 - a2 * y1;
            buf[j * 2] = y0;
            buf[j * 2 + 1] = y1;
        }
        b->z[0][0] = z00; b->z[0][1] = z01; b->z[1][0] = z10; b->z[1][1] = z11;
    }
}
#endif

void fa_eq_process(fa_eq_state* st, float* buf, int64_t n)
{
    uint32_t i, nb = 0;
    float k = -1.0f;                     /* the glide step, worked out when a band glides */
    fa_eq_bandstate* act[FA_EQ_MAX_BANDS];
    st->blocks++;
    for (i = 0; i < FA_EQ_MAX_BANDS; i++) {
        fa_eq_bandstate* b = &st->band[i];
        if (b->g == 0.0f && b->tg == 0.0f) continue;
        if (b->g != b->tg || b->f != b->tf || b->q != b->tq) {
            if (k < 0.0f) k = 1.0f - expf(-(float)n / (EQ_GLIDE_S * (float)st->rate));
            b->g = glide(b->g, b->tg, k, 0.001f);
            b->f = glide(b->f, b->tf, k, b->tf * 0.0001f);
            b->q = glide(b->q, b->tq, k, 0.0001f);
            b->dirty = 1;
        }
        if (b->g == 0.0f && b->tg == 0.0f) {   /* just reached flat: idle from here */
            memset(b->z, 0, sizeof(b->z));
            continue;
        }
        /* While gliding, redesign every fourth call (64 frames): smooth enough, and
         * the trig stays off most blocks. */
        if (b->dirty && (b->g == b->tg || (st->blocks & 3) == 0)) { design(b, st->rate); b->dirty = 0; }
        act[nb++] = b;
    }
    if (nb == 0) return;
    filter_bands(act, nb, buf, n);
    /* Flush denormals so a long silence doesn't slow the audio thread down. */
    for (i = 0; i < nb; i++) {
        float* z = &act[i]->z[0][0];
        int c;
        for (c = 0; c < 4; c++) if (fabsf(z[c]) < 1e-20f) z[c] = 0.0f;
    }
}
