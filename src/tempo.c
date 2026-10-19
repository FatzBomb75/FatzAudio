/* Tempo, pitch and rate: the processing chain (see tempo.h), the parameter values
 * the decode side follows (fa_tparam), and the map from played frames back to
 * source positions (fa_tmap). */
#include "fa_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FA_TEMPO_XFADE 256   /* frames of crossfade from the dry signal when the stretcher engages */

/* Transient mode (all in output time, ms): the input replaces the stretcher's
 * output from PRE before an onset to POST after it, with FADE-long crossfades.
 * PRE covers the stretcher's pre-echo (it smears up to about a block ahead). */
#define FA_TR_PRE_MS 24.0
#define FA_TR_POST_MS 12.0
#define FA_TR_FADE_MS 4.0
#define FA_TR_HOP 128           /* onset detector hop, frames */
#define FA_TR_RISE 8.0f         /* an onset: a hop this many times the energy of the hops before */
#define FA_TR_FLOOR 3e-6f       /* and at least this energy (about -55 dB) */
#define FA_TR_MIN_HISTORY 4     /* hops seen before the first onset can be found */
#define FA_TR_GAP_MS 60.0       /* onsets closer than this are one */

static fa_result grow(float** buf, int64_t* cap, int64_t frames, uint32_t channels)
{
    float* b;
    int64_t n;
    if (frames <= *cap) return FA_OK;
    n = *cap > 0 ? *cap : 1024;
    while (n < frames) n *= 2;
    b = (float*)realloc(*buf, (size_t)n * channels * sizeof(float));
    if (!b) return FA_ERROR_OUT_OF_MEMORY;
    *buf = b;
    *cap = n;
    return FA_OK;
}

fa_result fa_tempo_init(fa_tempo* t, uint32_t channels, uint32_t rate)
{
    memset(t, 0, sizeof(*t));
    if (channels == 0 || channels > FA_TEMPO_MAX_CHANNELS || rate == 0) return FA_ERROR_INVALID_ARG;
    t->channels = channels;
    t->rate = rate;
    t->tail = (float*)malloc((size_t)fa_varispeed_tail() * channels * sizeof(float));
    if (!t->tail) return FA_ERROR_OUT_OF_MEMORY;
    fa_tempo_reset(t, 0.0);
    return FA_OK;
}

void fa_tempo_free(fa_tempo* t)
{
    if (t->st) fa_stretch_destroy(t->st);
    if (t->vs_ready) fa_varispeed_free(&t->vs);
    free(t->prime);
    free(t->sbuf);
    free(t->tail);
    free(t->obuf);
    free(t->raw);
    memset(t, 0, sizeof(*t));
}

void fa_tempo_reset(fa_tempo* t, double src)
{
    t->st_on = t->priming = 0;
    t->prime_n = t->prime_need = 0;
    t->frac = 0.0;
    t->xfade = t->xfade_pos = 0;
    t->vs_on = 0;
    t->tail_n = 0;
    t->in_src = t->out_src = t->st_src = src;
    t->last_tempo = 1.0;
    t->produced = 0;
}

void fa_tempo_configure(fa_tempo* t, int fast, int keep_formants, int transients)
{
    t->fast = fast != 0;
    t->keep_formants = keep_formants != 0;
    t->transients = transients != 0;
}

int fa_tempo_engaged(const fa_tempo* t) { return t->st_on || t->vs_on; }

double fa_tempo_held(const fa_tempo* t) { return t->in_src - t->out_src; }

/* Keeps the last stage-one frames, the history a varispeed engaging later needs. */
static void tail_push(fa_tempo* t, const float* in, int64_t n)
{
    uint32_t ch = t->channels;
    int64_t keep = fa_varispeed_tail();
    if (n >= keep) {
        memcpy(t->tail, in + (size_t)(n - keep) * ch, (size_t)keep * ch * sizeof(float));
        t->tail_n = keep;
        return;
    }
    if (t->tail_n + n > keep) {
        int64_t drop = t->tail_n + n - keep;
        memmove(t->tail, t->tail + (size_t)drop * ch, (size_t)(t->tail_n - drop) * ch * sizeof(float));
        t->tail_n -= drop;
    }
    memcpy(t->tail + (size_t)t->tail_n * ch, in, (size_t)n * ch * sizeof(float));
    t->tail_n += n;
}

/* Blends the first stretched frames in from the dry input, so engaging the
 * stretcher in the middle of playback does not click. */
static void xfade_in(fa_tempo* t, float* out, int64_t n)
{
    uint32_t ch = t->channels, c;
    int64_t i;
    for (i = 0; i < n && t->xfade > 0; i++, t->xfade--, t->xfade_pos++) {
        /* The dry frame at the same source time as output frame xfade_pos. */
        double at = (double)t->xfade_pos * t->xfade_ratio;
        int64_t k = (int64_t)at;
        float fr = (float)(at - (double)k);
        const float *d0, *d1;
        float w = 0.5f - 0.5f * (float)cos(FA_TEMPO_PI * ((double)t->xfade_pos + 0.5) / FA_TEMPO_XFADE);
        if (k + 1 >= t->prime_n) {
            k = t->prime_n > 1 ? t->prime_n - 2 : 0;
            fr = t->prime_n > 1 ? 1.0f : 0.0f;
        }
        d0 = t->prime + (size_t)k * ch;
        d1 = t->prime_n > 1 ? d0 + ch : d0;
        for (c = 0; c < ch; c++) {
            float dry = d0[c] + (d1[c] - d0[c]) * fr;
            out[(size_t)i * ch + c] = dry + (out[(size_t)i * ch + c] - dry) * w;
        }
    }
}

/* ---- transient mode ------------------------------------------------------ */

/* Appends the stretcher's input (n frames) and looks for onsets in it. What no
 * splice can reach any more (it reads at most PRE + FADE behind an onset still
 * ahead of the output) is dropped first. */
static fa_result raw_push(fa_tempo* t, const float* in, int64_t n)
{
    uint32_t ch = t->channels, c;
    double ms = t->rate / 1000.0, ratio = t->last_tempo > 0.0 ? t->last_tempo : 1.0;
    double window = (FA_TR_PRE_MS + FA_TR_POST_MS + 2.0 * FA_TR_FADE_MS) * ms, gap = FA_TR_GAP_MS * ms;
    int64_t keep_from = (int64_t)floor(t->st_src) - (int64_t)(4.0 * window * (ratio > 1.0 ? ratio : 1.0)) - 1;
    int64_t drop = keep_from - t->raw_base, i;
    fa_result r;
    if (drop > 0 && t->raw_n + n > t->raw_cap) {
        if (drop > t->raw_n) drop = t->raw_n;
        memmove(t->raw, t->raw + (size_t)drop * ch, (size_t)(t->raw_n - drop) * ch * sizeof(float));
        t->raw_n -= drop;
        t->raw_base += drop;
    }
    /* Twice what is kept, so the move above is rare. */
    r = grow(&t->raw, &t->raw_cap, 2 * (t->raw_n + n), ch);
    if (r != FA_OK) return r;
    memcpy(t->raw + (size_t)t->raw_n * ch, in, (size_t)n * ch * sizeof(float));
    /* Windows must not overlap in the output: at a speed-up they come closer. */
    if (gap < window * ratio) gap = window * ratio;
    for (i = 0; i < n; i++) {
        const float* f = in + (size_t)i * ch;
        double e = 0.0;
        for (c = 0; c < ch; c++) e += (double)f[c] * f[c];
        t->hop_e += e / ch;
        if (++t->hop_fill < FA_TR_HOP) continue;
        {
            float hop = (float)(t->hop_e / FA_TR_HOP), before = 0.0f;
            int64_t at = t->raw_base + t->raw_n + i + 1 - FA_TR_HOP;
            int k, m = t->hop_count < FA_TR_HISTORY ? t->hop_count : FA_TR_HISTORY;
            for (k = 0; k < m; k++) before += t->hop_hist[k];
            before = m ? before / (float)m : 0.0f;
            /* A few hops of history at least (about 10 ms), so engaging mid-note is no onset. */
            if (m >= FA_TR_MIN_HISTORY && hop > FA_TR_RISE * before + FA_TR_FLOOR &&
                (double)(at - t->last_onset) > gap && t->onset_n < FA_TR_ONSETS) {
                t->onsets[t->onset_n].u = at;
                t->onsets[t->onset_n].fixed = 0;
                t->onset_n++;
                t->last_onset = at;
            }
            t->hop_hist[t->hop_count % FA_TR_HISTORY] = hop;
            t->hop_count++;
            t->hop_e = 0.0;
            t->hop_fill = 0;
        }
    }
    t->raw_n += n;
    return FA_OK;
}

static float tr_weight(double d, double pre, double post, double fade)
{
    if (d < -pre - fade || d > post + fade) return 0.0f;
    if (d < -pre) return (float)(0.5 - 0.5 * cos(FA_TEMPO_PI * (d + pre + fade) / fade));
    if (d > post) return (float)(0.5 + 0.5 * cos(FA_TEMPO_PI * (d - post) / fade));
    return 1.0f;
}

/* The stretcher's output `out` (nout frames; the first at source position src0,
 * then `ratio` source frames per frame): around each onset, the input itself at
 * its own speed. Where an onset lands in the output is fixed once, as its window
 * comes in reach, so the input then advances exactly a frame per output frame
 * even while the tempo ramps. */
static void splice_transients(fa_tempo* t, float* out, int64_t nout, double src0, double ratio)
{
    uint32_t ch = t->channels, c;
    double ms = t->rate / 1000.0, pre = FA_TR_PRE_MS * ms, post = FA_TR_POST_MS * ms, fade = FA_TR_FADE_MS * ms;
    double out0 = (double)t->st_out;
    int k = 0, n;
    if (nout <= 0 || ratio <= 0.0) return;
    for (n = 0; n < t->onset_n; n++) {
        int64_t u = t->onsets[n].u, j, j0, j1;
        if (!t->onsets[n].fixed) {
            double v = out0 + ((double)u - src0) / ratio;
            if (v - pre - fade >= out0 + (double)nout) continue;         /* not in reach yet */
            t->onsets[n].v = v;
            t->onsets[n].fixed = 1;
            /* The whole window's input must be there, or the splice would start
             * or stop without its crossfade: skip this onset then. */
            if (v - pre - fade < out0 || (double)u - pre - fade < (double)t->raw_base ||
                (double)u + post + fade + 2.0 >= (double)(t->raw_base + t->raw_n)) {
                t->onsets[n].v = -1e300;
                continue;
            }
        }
        j0 = (int64_t)ceil(t->onsets[n].v - pre - fade - out0);
        j1 = (int64_t)floor(t->onsets[n].v + post + fade - out0);
        if (j0 < 0) j0 = 0;
        if (j1 >= nout) j1 = nout - 1;
        for (j = j0; j <= j1; j++) {
            double d = out0 + (double)j - t->onsets[n].v, q = (double)u + d - (double)t->raw_base;
            int64_t i = (int64_t)floor(q);
            float w = tr_weight(d, pre, post, fade), fr = (float)(q - (double)i);
            const float* r0;
            if (w <= 0.0f || i < 0 || i + 1 >= t->raw_n) continue;
            r0 = t->raw + (size_t)i * ch;
            for (c = 0; c < ch; c++) {
                float dry = r0[c] + (r0[c + ch] - r0[c]) * fr;
                out[(size_t)j * ch + c] += (dry - out[(size_t)j * ch + c]) * w;
            }
        }
    }
    /* Keep the onsets the next output can still reach. */
    for (n = 0; n < t->onset_n; n++)
        if (!t->onsets[n].fixed || t->onsets[n].v + post + fade >= out0 + (double)nout) t->onsets[k++] = t->onsets[n];
    t->onset_n = k;
}

static fa_result engage_stretcher(fa_stretch** st, fa_tempo* t, double tempo)
{
    if (*st && t->st_fast != t->fast) {    /* made for the other quality */
        fa_stretch_destroy(*st);
        *st = NULL;
    }
    if (!*st) {
        *st = fa_stretch_create(t->channels, t->rate, t->fast);
        if (!*st) return FA_ERROR_OUT_OF_MEMORY;
        t->st_fast = t->fast;
        t->formants = 1.0;
    }
    t->st_on = 1;
    t->priming = 1;
    t->prime_n = 0;
    t->prime_src = t->st_src;
    t->prime_need = fa_stretch_start_length(*st, tempo);
    t->frac = 0.0;
    t->xfade = t->produced > 0 ? FA_TEMPO_XFADE : 0;
    t->xfade_pos = 0;
    t->xfade_ratio = tempo;
    /* Transient mode: the input from prime[0] on (the first output frame). */
    t->raw_n = 0;
    t->raw_base = (int64_t)floor(t->prime_src + 0.5);
    t->hop_e = 0.0;
    t->hop_fill = t->hop_count = t->onset_n = 0;
    t->last_onset = INT64_MIN / 2;
    t->st_out = 0;
    t->st_transients = t->transients;   /* a change applies when it next engages */
    return FA_OK;
}

/* Stage one, the stretcher at time ratio `tempo` (source frames per output frame)
 * and formant factor `formants`: `in` -> *a. */
static fa_result stage_one(fa_tempo* t, const float* in, int64_t n, double tempo, double formants,
                           const float** a, int64_t* na)
{
    uint32_t ch = t->channels;
    double exact, src0;
    int64_t nout;
    fa_result r;
    *a = in;
    *na = n;
    if (!t->st_on) {
        if (tempo == 1.0 && formants == 1.0) {
            t->st_src += (double)n;
            t->last_tempo = 1.0;
            return FA_OK;
        }
        r = engage_stretcher(&t->st, t, tempo);
        if (r != FA_OK) return r;
    }
    t->last_tempo = tempo;
    if (formants != t->formants) {
        fa_stretch_set_formants(t->st, formants);
        t->formants = formants;
    }
    if (t->st_transients && n > 0) {
        r = raw_push(t, in, n);
        if (r != FA_OK) return r;
    }
    if (t->priming) {
        r = grow(&t->prime, &t->prime_cap, t->prime_n + n, ch);
        if (r != FA_OK) return r;
        memcpy(t->prime + (size_t)t->prime_n * ch, in, (size_t)n * ch * sizeof(float));
        t->prime_n += n;
        *na = 0;
        if (t->prime_n < t->prime_need) return FA_OK;
        if (!fa_stretch_start(t->st, t->prime, (int)t->prime_need)) return FA_ERROR_OUT_OF_MEMORY;
        t->priming = 0;
        /* What came in beyond the start is processed now (the start kept prime[]
         * as the dry signal for the crossfade; nothing overwrites it until the
         * stretcher engages again). */
        in = t->prime + (size_t)t->prime_need * ch;
        n = t->prime_n - t->prime_need;
        if (n == 0) return FA_OK;
    }
    src0 = t->st_src - t->frac * tempo;   /* the next output frame (see head) */
    exact = t->frac + (double)n / tempo;
    nout = (int64_t)exact;
    t->frac = exact - (double)nout;
    r = grow(&t->sbuf, &t->scap, nout > 0 ? nout : 1, ch);
    if (r != FA_OK) return r;
    if (!fa_stretch_process(t->st, in, (int)n, t->sbuf, (int)nout)) return FA_ERROR_OUT_OF_MEMORY;
    if (t->xfade > 0) xfade_in(t, t->sbuf, nout);
    /* With formants kept, the input is not what the stretcher's output sounds like. */
    if (t->st_transients && nout > 0 && formants == 1.0) splice_transients(t, t->sbuf, nout, src0, tempo);
    t->st_out += nout;
    t->st_src += (double)n;
    *a = t->sbuf;
    *na = nout;
    return FA_OK;
}

/* Stage two, the varispeed: `a` -> *out. `limit` > 0 stops the output there (the
 * end of the source, before the lookahead padding). */
static fa_result stage_two(fa_tempo* t, const float* a, int64_t na, double rate, const float** out, int64_t* nout)
{
    fa_result r;
    int64_t max;
    if (!t->vs_on) {
        if (rate == 1.0) {
            *out = a;
            *nout = na;
            if (na > 0) tail_push(t, a, na);
            return FA_OK;
        }
        if (!t->vs_ready) {
            r = fa_varispeed_init(&t->vs, t->channels);
            if (r != FA_OK) return r;
            t->vs_ready = 1;
        }
        r = fa_varispeed_prime(&t->vs, t->tail, t->tail_n);
        if (r != FA_OK) return r;
        t->vs_on = 1;
    }
    if (na > 0) {
        r = fa_varispeed_write(&t->vs, a, na);
        if (r != FA_OK) return r;
    }
    max = (int64_t)ceil(fa_varispeed_pending(&t->vs) / rate) + 2;
    r = grow(&t->obuf, &t->ocap, max, t->channels);
    if (r != FA_OK) return r;
    *nout = fa_varispeed_read(&t->vs, rate, t->obuf, max);
    *out = t->obuf;
    return FA_OK;
}

/* Source position of the next output frame. */
static double head(const fa_tempo* t)
{
    double h = t->st_src;
    if (t->st_on && !t->priming) h -= t->frac * t->last_tempo;   /* input not yet worth a whole frame */
    if (t->vs_on) h -= fa_varispeed_pending(&t->vs) * t->last_tempo;
    return h;
}

/* What each stage does for tempo, pitch (semitones) and rate (see tempo.h). */
static void split(const fa_tempo* t, double tempo, double pitch, double rate, double* a, double* f, double* b)
{
    double p = pitch == 0.0 ? 1.0 : pow(2.0, pitch / 12.0);
    *a = tempo / p;
    *b = p * rate;
    *f = t->keep_formants && p != 1.0 ? 1.0 / p : 1.0;
}

fa_result fa_tempo_run(fa_tempo* t, const float* in, int64_t frames, double tempo, double pitch, double rate,
                       const float** out, int64_t* out_frames, double* src0, double* src1)
{
    const float* a;
    int64_t na;
    double sa, sf, sb;
    fa_result r;
    *out = in;
    *out_frames = 0;
    *src0 = *src1 = t->out_src;
    t->in_src += (double)frames;
    split(t, tempo, pitch, rate, &sa, &sf, &sb);
    r = stage_one(t, in, frames, sa, sf, &a, &na);
    if (r == FA_OK) r = stage_two(t, a, na, sb, out, out_frames);
    if (r != FA_OK) {
        *out_frames = 0;
        return r;
    }
    t->out_src = head(t);
    if (t->out_src < *src0) t->out_src = *src0;
    *src1 = t->out_src;
    t->produced += *out_frames;
    return FA_OK;
}

fa_result fa_tempo_drain(fa_tempo* t, double tempo, double pitch, double rate,
                         const float** out, int64_t* out_frames, double* src0, double* src1)
{
    uint32_t ch = t->channels;
    const float* a = NULL;
    int64_t na = 0;
    double sa, sf, rate_b;
    fa_result r;
    *out = NULL;
    *out_frames = 0;
    *src0 = t->out_src;
    split(t, tempo, pitch, rate, &sa, &sf, &rate_b);
    rate = rate_b;
    if (t->st_on) {
        double ratio = t->priming ? sa : t->last_tempo;
        int64_t want = (int64_t)floor((t->in_src - t->st_src) / ratio + t->frac + 0.5), lat, m1, fl, total;
        if (t->priming) {
            /* Shorter than the start needs: start on what there is, padded with silence. */
            if (t->prime_n == 0) want = 0;
            r = grow(&t->prime, &t->prime_cap, t->prime_need, ch);
            if (r != FA_OK) return r;
            memset(t->prime + (size_t)t->prime_n * ch, 0, (size_t)(t->prime_need - t->prime_n) * ch * sizeof(float));
            if (sf != t->formants) {
                fa_stretch_set_formants(t->st, sf);
                t->formants = sf;
            }
            if (!fa_stretch_start(t->st, t->prime, (int)t->prime_need)) return FA_ERROR_OUT_OF_MEMORY;
            t->priming = 0;
            want = (int64_t)floor((double)t->prime_n / ratio + 0.5);
        }
        lat = fa_stretch_input_latency(t->st);
        m1 = (int64_t)floor((double)lat / ratio + 0.5);
        fl = fa_stretch_output_latency(t->st);
        total = m1 + fl;
        if (want > total) want = total;
        if (want < 0) want = 0;
        r = grow(&t->sbuf, &t->scap, total > lat ? total : lat, ch);
        if (r != FA_OK) return r;
        /* The README's ending: input latency of silence, then the held output. */
        r = grow(&t->obuf, &t->ocap, lat > 0 ? lat : 1, ch);
        if (r != FA_OK) return r;
        memset(t->obuf, 0, (size_t)lat * ch * sizeof(float));
        if (!fa_stretch_process(t->st, t->obuf, (int)lat, t->sbuf, (int)m1)) return FA_ERROR_OUT_OF_MEMORY;
        if (!fa_stretch_flush(t->st, t->sbuf + (size_t)m1 * ch, (int)fl, ratio)) return FA_ERROR_OUT_OF_MEMORY;
        if (t->xfade > 0) xfade_in(t, t->sbuf, want);
        a = t->sbuf;
        na = want;
        t->st_on = 0;
    }
    t->st_src = t->in_src;
    if (t->vs_on) {
        double real;
        int64_t max;
        if (na > 0) {
            r = fa_varispeed_write(&t->vs, a, na);
            if (r != FA_OK) return r;
        }
        real = fa_varispeed_pending(&t->vs);
        r = fa_varispeed_write(&t->vs, NULL, fa_varispeed_tail());
        if (r != FA_OK) return r;
        max = real > 0.0 ? (int64_t)ceil(real / rate - 1e-9) : 0;
        r = grow(&t->obuf, &t->ocap, max > 0 ? max : 1, ch);
        if (r != FA_OK) return r;
        *out_frames = fa_varispeed_read(&t->vs, rate, t->obuf, max);
        *out = t->obuf;
        t->vs_on = 0;
    } else {
        *out = a;
        *out_frames = na;
    }
    t->out_src = t->in_src;
    *src1 = t->out_src;
    t->produced += *out_frames;
    return FA_OK;
}

/* ---- parameter values on the decode side ------------------------------------ */

void fa_tparam_init(fa_tparam* p, float value)
{
    memset(p, 0, sizeof(*p));
    p->value = p->target = p->from = value;
    p->start = FA_TPARAM_NOW;
}

int fa_tparam_post(fa_tparam* p, float target, int64_t len, const fa_shape* shape, int64_t tag, int notify,
                   int64_t start, int64_t* old_tag)
{
    int replaced = p->active && p->notify;
    if (replaced) *old_tag = p->tag;
    p->target = target;
    p->tag = tag;
    p->notify = notify;
    if (shape) p->shape = *shape;
    else memset(&p->shape, 0, sizeof(p->shape));
    p->len = len;
    p->start = start;
    p->from = p->value;
    if (!notify && len <= 0) {
        p->value = target;  /* a plain set: in effect from the next frame the chain makes */
        p->active = 0;
    } else {
        p->active = 1;
    }
    return replaced;
}

int fa_tparam_eval(fa_tparam* p, int64_t at, float* out_value, int64_t* done_tag)
{
    double t;
    if (!p->active) {
        *out_value = p->value;
        return 0;
    }
    if (p->start == FA_TPARAM_NOW) p->start = at;
    if (at < p->start) {                      /* scheduled, not yet due */
        *out_value = p->value;
        p->from = p->value;
        return 0;
    }
    if (at == p->start) p->from = p->value;
    t = p->len > 0 ? (double)(at - p->start) / (double)p->len : 1.0;
    if (t >= 1.0) {
        p->value = p->target;
        p->active = 0;
        *out_value = p->value;
        *done_tag = p->tag;
        return p->notify;
    }
    p->value = p->from + (p->target - p->from) * fa_shape_eval(&p->shape, (float)t, p->target > p->from);
    *out_value = p->value;
    return 0;
}

void fa_tparam_rebase(fa_tparam* p, int64_t base)
{
    if (p->active && p->start != FA_TPARAM_NOW) p->start -= base;
}

/* ---- played frames -> source positions --------------------------------------- */

void fa_tmap_reset(fa_tmap* m)
{
    fa_store64(&m->write, 0);
    fa_store64(&m->read, 0);
}

int fa_tmap_full(fa_tmap* m)
{
    return fa_load64(&m->write) - fa_load64(&m->read) >= FA_TMAP_SIZE - FA_TMAP_SLACK;
}

void fa_tmap_add(fa_tmap* m, int64_t at, double src, double slope)
{
    int64_t w = fa_load64(&m->write);
    if (w > 0) {
        int64_t i = (w - 1) % FA_TMAP_SIZE;
        double predicted = m->src[i] + (double)(at - m->at[i]) * m->slope[i];
        if (fabs(m->slope[i] - slope) <= 1e-9 * slope && fabs(predicted - src) < 0.25) return;  /* continues it */
        if (at <= m->at[i]) {                 /* replaces a segment that never played */
            m->src[i] = src;
            m->slope[i] = slope;
            return;
        }
    }
    if (w - fa_load64(&m->read) >= FA_TMAP_SIZE - FA_TMAP_SLACK) return;  /* callers check fa_tmap_full first */
    m->at[w % FA_TMAP_SIZE] = at;
    m->src[w % FA_TMAP_SIZE] = src;
    m->slope[w % FA_TMAP_SIZE] = slope;
    fa_store64(&m->write, w + 1);
}

/* The newest segment starting at or before `at` (-1: before them all), and the
 * oldest one still readable in *lo. */
static int64_t tmap_find(fa_tmap* m, int64_t at, int64_t w, int64_t* lo)
{
    int64_t i, low = fa_load64(&m->read) - FA_TMAP_SLACK;
    if (low < w - FA_TMAP_SIZE + 1) low = w - FA_TMAP_SIZE + 1;
    if (low < 0) low = 0;
    *lo = low;
    if (low >= w || m->at[low % FA_TMAP_SIZE] > at) return -1;
    /* Segment starts only grow: binary search for the last one at or before `at`. */
    while (low < w - 1) {
        i = low + (w - low) / 2;
        if (m->at[i % FA_TMAP_SIZE] <= at) low = i;
        else w = i;
    }
    return low;
}

/* Segment `i` (or the base before all segments) as start/source/slope, and where it ends. */
static void tmap_segment(fa_tmap* m, int64_t i, int64_t lo, int64_t w, double base_src, double base_slope,
                         int64_t* a0, double* s0, double* k, int64_t* end)
{
    if (i < 0 && lo > 0 && lo < w) i = lo;   /* older than what is kept: extend the oldest kept */
    if (i < 0) {
        *a0 = 0;
        *s0 = base_src;
        *k = base_slope;
        *end = lo < w ? m->at[lo % FA_TMAP_SIZE] : INT64_MAX;
        return;
    }
    *a0 = m->at[i % FA_TMAP_SIZE];
    *s0 = m->src[i % FA_TMAP_SIZE];
    *k = m->slope[i % FA_TMAP_SIZE];
    *end = i + 1 < w ? m->at[(i + 1) % FA_TMAP_SIZE] : INT64_MAX;
}

double fa_tmap_pos(fa_tmap* m, int64_t at, double base_src, double base_slope, double* out_slope, int64_t* out_next)
{
    int64_t w = fa_load64(&m->write), lo = 0, a0, end;
    double s0, k;
    int64_t i = w > 0 ? tmap_find(m, at, w, &lo) : -1;
    if (w == 0) lo = 0;
    tmap_segment(m, i, lo, w, base_src, base_slope, &a0, &s0, &k, &end);
    if (out_slope) *out_slope = k;
    if (out_next) *out_next = end;
    return s0 + (double)(at - a0) * k;
}

int64_t fa_tmap_frames_to(fa_tmap* m, int64_t at, double target, double base_src, double base_slope)
{
    int64_t w = fa_load64(&m->write), lo = 0, i = -1, a0, end;
    double s0, k;
    if (w > 0) i = tmap_find(m, at, w, &lo);
    for (;;) {
        double x;
        tmap_segment(m, i, lo, w, base_src, base_slope, &a0, &s0, &k, &end);
        if (s0 + (double)((at > a0 ? at : a0) - a0) * k >= target) {
            x = (double)(at > a0 ? at : a0);
        } else {
            x = k > 0.0 ? (double)a0 + (target - s0) / k : HUGE_VAL;   /* a segment that holds still */
            if (end != INT64_MAX && x > (double)end) {
                i = i < 0 ? (lo > 0 && lo < w ? lo + 1 : lo) : i + 1;
                if (i >= w) i = w - 1;
                continue;
            }
        }
        x -= (double)at;
        if (x >= 9e18) return INT64_MAX;
        return x <= 0.0 ? 0 : (int64_t)ceil(x - 1e-9);
    }
}

void fa_tmap_release(fa_tmap* m, int64_t at)
{
    int64_t r = fa_load64(&m->read), w = fa_load64(&m->write);
    while (r + 1 < w && m->at[(r + 1) % FA_TMAP_SIZE] <= at) r++;
    fa_store64(&m->read, r);
}
