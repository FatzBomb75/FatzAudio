/* The DJ mixer's setup check (fa_mixer_measure_latency): plays test sounds through the
 * speakers and listens on a mic, for the app's setup screen.
 *
 * One pass: the music fades out over 10 ms, the test signal plays on both channels after
 * the limiter (so the level is exact), and the mic's input is recorded as it leaves the
 * bridge, before its strip; then the music fades back. Both happen in the same mixer
 * blocks, so the recording's frame j and the signal's frame j left the mixer together and
 * the delay of a sound in the recording is, in frames, the whole way from the mixer to the
 * speaker, through the air and back in through the input and the bridge.
 *
 *   probe   three 5 ms chirps (1-6 kHz), at rising levels until the mic hears them
 *   full    five chirps, then 1.5 s of pink noise, at the level the probe found
 *
 * The chirps give the delay (the peak of the normalized cross-correlation, to the frame).
 * The noise gives the loop's transfer function, speaker to room to mic, from Welch
 * averages of the cross- and auto-spectra; its highest point (1/24-octave power average,
 * 100 Hz-10 kHz) is where a monitored mic would feed back first, and so sets the most mic
 * gain that stays 6 dB clear of it. A room's resonances sit several dB over its average,
 * so the average would be no guide (the mic latency spike howled at a broadband loop gain
 * of 0.12).
 *
 * The audio thread's part is fa_mic_check_record (mic.c, as the mics are pulled) and
 * fa_mic_check_render (mixer.c, after the limiter); this thread waits for a pass to
 * finish, then analyses it. */
#include "mixer.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CHIRP_MS 5.0
#define CHIRP_F0 1000.0
#define CHIRP_F1 6000.0
#define CHIRP_GAP_MS 150.0       /* between chirps */
#define LEAD_MS 50.0             /* silence before the first chirp */
#define MAX_LAG_MS 400.0         /* the slowest path looked for (Bluetooth is 150-300 ms) */
#define FADE_MS 10.0
#define NOISE_MS 1500.0
#define NOISE_CREST_DB 10.0      /* the noise's RMS below the chirps' peak */
#define HEARD_MATCH 0.5f         /* normalized correlation that counts as heard */
#define CLEAR_MATCH 0.9f         /* and heard clearly: the probes stop getting louder */
#define PLATEAU_MATCH 0.05f      /* less gained by 10 dB more: the room, not the noise, limits it */
#define AGREE_MS 2.0             /* chirps further from the median than this are echoes */
#define LEVEL_START_DB (-40.0f)
#define LEVEL_STEP_DB 10.0f
#define FEEDBACK_MARGIN_DB 6.0f
#define BAND_LOW_HZ 100.0
#define BAND_HIGH_HZ 10000.0
#define SETTLE_MS 500            /* after a mode switch: the input runs that long without an
                                    underrun (at most SETTLE_MAX_MS) */
#define SETTLE_MAX_MS 3000

static const double k_two_pi = 6.283185307179586;

FA_API void FA_CALL fa_latency_check_config_init(fa_latency_check_config* config)
{
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->struct_size = (uint32_t)sizeof(*config);
    config->max_level_db = -12.0f;
    config->max_monitor = 25000000;
}

/* ---- the audio thread's side -------------------------------------------------------- */

static int64_t check_rel(fa_mic_check* k, int64_t clock)
{
    if (k->t0 == FA_CHECK_UNSET) k->t0 = clock;   /* the block it begins on */
    return clock - k->t0;
}

void fa_mic_check_record(fa_mic_check* k, const float* in, int lane, uint32_t frames, int64_t clock)
{
    int64_t j = check_rel(k, clock) - k->fade, i;
    for (i = 0; i < (int64_t)frames; i++, j++)
        if (j >= 0 && j < k->rec_len) k->rec[j] = in[(size_t)i * CAPTURE_LANES + lane];
}

void fa_mic_check_render(fa_mic_check* k, float* master, uint32_t frames, int64_t clock)
{
    int64_t rel = check_rel(k, clock), end = 2 * k->fade + k->rec_len, i;
    if (fa_load32(&k->done)) return;
    for (i = 0; i < (int64_t)frames; i++, rel++) {
        int64_t j = rel - k->fade;
        float g, s = j >= 0 && j < k->signal_len ? k->signal[j] : 0.0f;
        if (rel < k->fade) g = 1.0f - (float)rel / (float)k->fade;
        else if (rel < k->fade + k->rec_len) g = 0.0f;
        else if (rel < end) g = (float)(rel - k->fade - k->rec_len) / (float)k->fade;
        else g = 1.0f;
        master[i * 2] = master[i * 2] * g + s;
        master[i * 2 + 1] = master[i * 2 + 1] * g + s;
    }
    if (rel >= end) fa_store32(&k->done, 1);
}

/* ---- signals -------------------------------------------------------------------------- */

typedef struct pass {
    float* signal;
    int64_t len, rec_len;
    int64_t chirp_at[5];
    int chirps;
    int64_t noise_at, noise_len; /* 0 = no noise */
    float* chirp;                /* one chirp at amplitude 1 */
    int64_t chirp_len;
} pass;

static void pass_free(pass* p)
{
    free(p->signal);
    free(p->chirp);
}

/* A probe (3 chirps) or a full pass (5 chirps and the noise), chirps peaking at `level`. */
static fa_result pass_make(pass* p, uint32_t rate, float level_db, int full)
{
    double amp = pow(10.0, level_db / 20.0), gap = CHIRP_GAP_MS * 0.001 * rate;
    int64_t i;
    int c;
    memset(p, 0, sizeof(*p));
    p->chirp_len = (int64_t)(rate * CHIRP_MS * 0.001);
    p->chirps = full ? 5 : 3;
    for (c = 0; c < p->chirps; c++) p->chirp_at[c] = (int64_t)(LEAD_MS * 0.001 * rate + c * gap);
    p->len = p->chirp_at[p->chirps - 1] + p->chirp_len;
    if (full) {
        p->noise_at = p->len + (int64_t)gap;
        p->noise_len = (int64_t)(NOISE_MS * 0.001 * rate);
        p->len = p->noise_at + p->noise_len;
    }
    p->rec_len = p->len + (int64_t)(MAX_LAG_MS * 0.001 * rate);
    p->signal = (float*)calloc((size_t)p->len, sizeof(float));
    p->chirp = (float*)malloc((size_t)p->chirp_len * sizeof(float));
    if (!p->signal || !p->chirp) {
        pass_free(p);
        return FA_ERROR_OUT_OF_MEMORY;
    }
    for (i = 0; i < p->chirp_len; i++) {
        double t = (double)i / rate, T = CHIRP_MS * 0.001;
        double ph = k_two_pi * (CHIRP_F0 * t + (CHIRP_F1 - CHIRP_F0) * t * t / (2.0 * T));
        double w = 0.5 - 0.5 * cos(k_two_pi * (double)i / (double)(p->chirp_len - 1));
        p->chirp[i] = (float)(w * sin(ph));
    }
    for (c = 0; c < p->chirps; c++)
        for (i = 0; i < p->chirp_len; i++) p->signal[p->chirp_at[c] + i] = (float)(amp * p->chirp[i]);
    if (full) {
        /* Pink noise (Paul Kellet's filter on a fixed white sequence), faded in and out. */
        uint32_t x = 0x9E3779B9u;
        double b0 = 0, b1 = 0, b2 = 0, sq = 0, g;
        int64_t fade = (int64_t)(FADE_MS * 0.001 * rate);
        float* n = p->signal + p->noise_at;
        for (i = 0; i < p->noise_len; i++) {
            double w;
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            w = (double)x / 2147483648.0 - 1.0;
            b0 = 0.99765 * b0 + w * 0.0990460;
            b1 = 0.96300 * b1 + w * 0.2965164;
            b2 = 0.57000 * b2 + w * 1.0526913;
            n[i] = (float)(b0 + b1 + b2 + w * 0.1848);
            sq += (double)n[i] * n[i];
        }
        g = amp * pow(10.0, -NOISE_CREST_DB / 20.0) / sqrt(sq / (double)p->noise_len);
        for (i = 0; i < p->noise_len; i++) {
            double f = i < fade ? (double)i / fade : i >= p->noise_len - fade ? (double)(p->noise_len - 1 - i) / fade : 1.0;
            n[i] = (float)(n[i] * g * f);
        }
    }
    return FA_OK;
}

/* ---- running a pass ------------------------------------------------------------------- */

/* Plays `p` and records the mic into `rec` (p->rec_len frames). FA_ERROR_DEVICE when the
 * output doesn't run (the mixer's device stopped or gone). */
static fa_result pass_run(struct fa_mixer* m, struct fa_mic* mic, const pass* p, float* rec)
{
    fa_mic_check k;
    uint32_t rate = m->device->rate;
    int64_t limit = fa_monotonic_ns() + (int64_t)((double)(p->rec_len + rate) * 1e9 / rate) + 2000000000;
    memset(&k, 0, sizeof(k));
    k.mic = mic;
    k.signal = p->signal;
    k.signal_len = p->len;
    k.rec = rec;
    k.rec_len = p->rec_len;
    k.fade = (int64_t)(FADE_MS * 0.001 * rate);
    k.t0 = FA_CHECK_UNSET;
    memset(rec, 0, (size_t)p->rec_len * sizeof(float));
    fa_exchange_ptr(&m->check, &k);
    while (!fa_load32(&k.done) && fa_monotonic_ns() < limit) fa_sleep_ms(5);
    fa_exchange_ptr(&m->check, NULL);
    fa_device_wait_quiescent(m->device);   /* the audio thread is past `k` */
    return fa_load32(&k.done) ? FA_OK : FA_ERROR_DEVICE;
}

/* ---- analysis ------------------------------------------------------------------------- */

/* How well the recording matches the chirp `lag` frames after `at`: the normalized
 * correlation, 0..1, for lags lo..hi-1 into out[0..hi-lo). Returns hi, cut to the lags
 * the recording holds (out is written only up to it). */
static int64_t match_chirp(const pass* p, const float* rec, int64_t at, int64_t lo, int64_t hi, float* out)
{
    double ce = 0.0, we = 0.0;
    int64_t d, i, n = p->chirp_len;
    if (hi > p->rec_len - n - at + 1) hi = p->rec_len - n - at + 1;
    for (i = 0; i < n; i++) {
        ce += (double)p->chirp[i] * p->chirp[i];
        we += (double)rec[at + lo + i] * rec[at + lo + i];
    }
    for (d = lo; d < hi; d++) {
        const float* r = rec + at + d;
        double acc = 0.0;
        for (i = 0; i < n; i++) acc += (double)r[i] * p->chirp[i];
        out[d - lo] = (float)(we > 1e-20 ? acc / sqrt(ce * we) : 0.0);
        if (d + 1 < hi) {
            we += (double)r[n] * r[n] - (double)r[0] * r[0];
            if (we < 0.0) we = 0.0;
        }
    }
    return hi;
}

static int cmp_i64(const void* a, const void* b)
{
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return x < y ? -1 : x > y;
}

/* The chirps' delay. The first chirp's window also holds the later chirps' arrivals and
 * the room's reflections, all later than its own: its delay is the earliest peak that
 * comes near the best one. The others are looked for around it; the delay is the median
 * of those heard, if at least `need` are. */
static int chirp_delay(const pass* p, const float* rec, uint32_t rate, int need, int64_t* out_delay, int64_t* out_spread,
                       float* out_worst)
{
    int64_t max_lag = (int64_t)(MAX_LAG_MS * 0.001 * rate), agree = (int64_t)(AGREE_MS * 0.001 * rate);
    int64_t lags[5], d0 = -1, i, best_at = 0;
    float* v = (float*)malloc((size_t)(max_lag + 2 * agree + 1) * sizeof(float));
    float best = 0.0f, worst = 1.0f;
    int c, n = 0;
    if (!v) return 0;
    match_chirp(p, rec, p->chirp_at[0], 0, max_lag, v);
    for (i = 0; i < max_lag; i++) if (v[i] > best) best = v[i];
    for (i = 0; i < max_lag && d0 < 0; i++)
        if (v[i] >= HEARD_MATCH && v[i] >= 0.8f * best) {
            int64_t e = i + p->chirp_len < max_lag ? i + p->chirp_len : max_lag, j;
            d0 = i;
            for (j = i; j < e; j++) if (v[j] > v[d0]) d0 = j;   /* the top of that peak */
        }
    if (d0 >= 0) {
        lags[n++] = d0;
        worst = v[d0];
    }
    for (c = 1; c < p->chirps && d0 >= 0; c++) {
        int64_t lo = d0 > agree ? d0 - agree : 0, hi = d0 + agree + 1;
        float top = 0.0f;
        hi = match_chirp(p, rec, p->chirp_at[c], lo, hi, v);
        for (i = 0; i < hi - lo; i++)
            if (v[i] > top) { top = v[i]; best_at = lo + i; }
        if (top >= HEARD_MATCH) lags[n++] = best_at;
        if (top < worst) worst = top;
    }
    free(v);
    if (n < need) return 0;
    qsort(lags, (size_t)n, sizeof(lags[0]), cmp_i64);
    *out_delay = lags[n / 2];
    *out_spread = lags[n - 1] - lags[0];
    if (out_worst) *out_worst = worst;
    return 1;
}

/* The loop's highest gain, speaker to mic (linear), from the noise: Welch's cross- and
 * auto-spectra of what played and what came back `delay` frames later. 0 when it can't
 * be measured. */
static double loop_peak(const pass* p, const float* rec, uint32_t rate, int64_t delay)
{
    uint32_t n = 1024, half, b, lo, hi;
    const fa_fft_tables* tb;
    float *xr = NULL, *xi = NULL, *yr = NULL, *yi = NULL;
    double *sxx = NULL, *syy = NULL, *sre = NULL, *sim = NULL, *h2 = NULL, peak = 0.0;
    int64_t at;
    int segs = 0;
    while (n < 16384 && n < (uint32_t)(rate * 0.17)) n *= 2;   /* about 6 Hz bins */
    half = n / 2;
    tb = fa_fft_tables_get(n);
    if (!tb || p->noise_len < (int64_t)n) return 0.0;
    xr = (float*)malloc(n * sizeof(float)); xi = (float*)malloc(n * sizeof(float));
    yr = (float*)malloc(n * sizeof(float)); yi = (float*)malloc(n * sizeof(float));
    sxx = (double*)calloc(half, sizeof(double)); sre = (double*)calloc(half, sizeof(double));
    sim = (double*)calloc(half, sizeof(double)); h2 = (double*)calloc(half, sizeof(double));
    syy = (double*)calloc(half, sizeof(double));
    if (!xr || !xi || !yr || !yi || !sxx || !syy || !sre || !sim || !h2) goto done;
    for (at = p->noise_at; at + n <= p->noise_at + p->noise_len; at += n / 2) {
        uint32_t i;
        for (i = 0; i < n; i++) {
            xr[i] = p->signal[at + i] * tb->hann[i];
            yr[i] = rec[at + delay + i] * tb->hann[i];
            xi[i] = yi[i] = 0.0f;
        }
        fa_fft(xr, xi, tb, n);
        fa_fft(yr, yi, tb, n);
        for (b = 0; b < half; b++) {
            /* conj(X) Y */
            sre[b] += (double)xr[b] * yr[b] + (double)xi[b] * yi[b];
            sim[b] += (double)xr[b] * yi[b] - (double)xi[b] * yr[b];
            sxx[b] += (double)xr[b] * xr[b] + (double)xi[b] * xi[b];
            syy[b] += (double)yr[b] * yr[b] + (double)yi[b] * yi[b];
        }
        segs++;
    }
    for (b = 1; b < half; b++) {
        /* |Sxy|^2 / Sxx^2 less the room noise's share: noise uncorrelated with the test
         * adds about Sxx x (its power) / segments to |Sxy|^2, which would read as gain
         * (and the highest of many noisy bins as a resonance). */
        double xy2 = sre[b] * sre[b] + sim[b] * sim[b], v;
        if (sxx[b] <= 0.0) continue;
        v = xy2 / (sxx[b] * sxx[b]) - fmax(0.0, syy[b] - xy2 / sxx[b]) / ((double)segs * sxx[b]);
        h2[b] = v > 0.0 ? v : 0.0;
    }
    lo = (uint32_t)(BAND_LOW_HZ * n / rate);
    hi = (uint32_t)(fmin(BAND_HIGH_HZ, 0.45 * rate) * n / rate);
    if (hi >= half) hi = half - 1;
    for (b = lo > 1 ? lo : 1; b <= hi; b++) {
        /* power average over +-1/48 octave: a 1/24-octave band around the bin */
        double f = (double)b, spread = f * (pow(2.0, 1.0 / 48.0) - 1.0), sum = 0.0;
        int64_t a = (int64_t)floor(f - spread), z = (int64_t)ceil(f + spread), k, cnt = 0;
        if (a < 1) a = 1;
        if (z >= (int64_t)half) z = half - 1;
        for (k = a; k <= z; k++, cnt++) sum += h2[k];
        if (cnt && sum / (double)cnt > peak) peak = sum / (double)cnt;
    }
    peak = sqrt(peak);
done:
    free(xr); free(xi); free(yr); free(yi);
    free(sxx); free(syy); free(sre); free(sim); free(h2);
    return peak;
}

/* ---- one mode ------------------------------------------------------------------------- */

static void measure_mode(struct fa_mixer* m, struct fa_mic* mic, const fa_latency_check_config* c,
                         fa_latency_check_mode* out)
{
    uint32_t rate = m->device->rate;
    float level = LEVEL_START_DB, master;
    float* rec = NULL;
    pass p;
    int64_t delay = 0, spread = 0, look = (int64_t)m->device->limiter.delay;
    int heard = 0, any = 0;
    float worst = 0.0f, heard_at = 0.0f, last_worst = -1.0f;
    out->result = FA_OK;
    /* What the OS's figures add up to (fa_mixer_get_latency before any measurement). */
    out->reported = fa_load64(&mic->cap->input_ns) + fa_load64(&mic->cap->bridge_ns) +
                    fa_load64(&m->device->latency_ns) + (fa_time)((double)look * 1e9 / rate);
    /* Probes, getting louder until the mic hears them clearly (the noise of the full pass
     * then stands well over the room's). */
    for (;;) {
        if (level > c->max_level_db) level = c->max_level_db;
        if (pass_make(&p, rate, level, 0) != FA_OK) { out->result = FA_ERROR_OUT_OF_MEMORY; return; }
        rec = (float*)malloc((size_t)p.rec_len * sizeof(float));
        out->result = rec ? pass_run(m, mic, &p, rec) : FA_ERROR_OUT_OF_MEMORY;
        if (out->result == FA_OK) heard = chirp_delay(&p, rec, rate, 2, &delay, &spread, &worst);
        free(rec);
        pass_free(&p);
        if (out->result != FA_OK) return;
        /* A real room's reflections can hold the match under CLEAR_MATCH at any level:
         * when 10 dB more didn't help, the quieter level was as clear, and it is used. */
        if (heard && last_worst >= 0.0f && worst < last_worst + PLATEAU_MATCH) break;
        if (heard) {
            any = 1;
            heard_at = level;
        }
        if ((heard && worst >= CLEAR_MATCH) || level >= c->max_level_db) break;
        last_worst = heard ? worst : -1.0f;
        level += LEVEL_STEP_DB;
    }
    if (!any) { out->result = FA_ERROR_NOT_FOUND; return; }
    level = heard_at;   /* the loudest probe heard (a louder one may have distorted) */
    /* The full pass at that level. */
    if (pass_make(&p, rate, level, 1) != FA_OK) { out->result = FA_ERROR_OUT_OF_MEMORY; return; }
    rec = (float*)malloc((size_t)p.rec_len * sizeof(float));
    out->result = rec ? pass_run(m, mic, &p, rec) : FA_ERROR_OUT_OF_MEMORY;
    if (out->result == FA_OK && !chirp_delay(&p, rec, rate, 3, &delay, &spread, NULL)) out->result = FA_ERROR_NOT_FOUND;
    if (out->result == FA_OK) {
        double h = loop_peak(&p, rec, rate, delay);
        master = fa_load_float(&m->lanes[ML_MASTER].current);
        /* The monitored mic goes into both channels at its gain, then the master gain:
         * the loop is gain x master x h, kept FEEDBACK_MARGIN_DB under 1. */
        out->max_gain_db = h > 0.0 && master > 0.0f
                           ? (float)(-20.0 * log10(h * master)) - FEEDBACK_MARGIN_DB : 40.0f;
        out->max_gain_db = clampf(out->max_gain_db, -20.0f, 40.0f);
        out->level_db = level;
        /* The chirps left after the limiter: the mic's way to the speaker also has its
         * look-ahead. */
        out->mic_to_speaker = (fa_time)((double)(delay + look) * 1e9 / rate);
        out->spread = (fa_time)((double)spread * 1e9 / rate);
    }
    free(rec);
    pass_free(&p);
}

/* ---- the API -------------------------------------------------------------------------- */

static int mode_available(struct fa_mixer* m, fa_latency_mode mode)
{
    return mode != FA_LATENCY_EXCLUSIVE || m->engine->context.backend == ma_backend_wasapi;
}

/* After a mode switch: the input and the bridge run again, steadily. A device can
 * deliver in fits for a moment after it opens, and a stall moves the bridge (and so the
 * delay) between the chirps. */
static void settle(struct fa_mic* mic)
{
    int64_t now = fa_monotonic_ns(), end = now + (int64_t)SETTLE_MAX_MS * 1000000;
    int64_t quiet_from = now;
    uint32_t under = fa_load32(&mic->cap->underruns), u;
    while ((now = fa_monotonic_ns()) < end && now - quiet_from < (int64_t)SETTLE_MS * 1000000) {
        fa_sleep_ms(20);
        u = fa_load32(&mic->cap->underruns);
        if (u != under) {
            under = u;
            quiet_from = fa_monotonic_ns();
        }
    }
}

FA_API fa_result FA_CALL fa_mixer_measure_latency(fa_mixer* mixer, fa_mic* mic, const fa_latency_check_config* config,
                                                  fa_latency_check* out_result)
{
    fa_latency_check_config c;
    fa_latency_check res;
    fa_latency_mode home, mode;
    uint32_t size, i, slot;
    fa_result r = FA_OK;
    const fa_latency_check_mode* pick = NULL;
    static const fa_latency_mode prefer[3] = { FA_LATENCY_LOW, FA_LATENCY_NORMAL, FA_LATENCY_EXCLUSIVE };
    if (!mixer || !mic || mic->mixer != mixer || !out_result || out_result->struct_size < sizeof(uint32_t))
        return FA_ERROR_INVALID_ARG;
    fa_latency_check_config_init(&c);
    if (config) {
        if (config->struct_size < sizeof(c)) return FA_ERROR_INVALID_ARG;
        c = *config;
    }
    if (c.flags & ~FA_CHECK_ALL_MODES) return FA_ERROR_INVALID_ARG;
    c.max_level_db = clampf(c.max_level_db != c.max_level_db ? -12.0f : c.max_level_db, -40.0f, -6.0f);
    if (c.max_monitor <= 0) c.max_monitor = 25000000;
    if (fa_exchange32(&mixer->checking, 1)) return FA_ERROR_BUSY;
    slot = mic->index;
    memset(&res, 0, sizeof(res));
    size = out_result->struct_size < sizeof(res) ? out_result->struct_size : (uint32_t)sizeof(res);
    res.struct_size = size;
    home = mixer->device->open_mode;
    for (i = 0; i < 3; i++) {
        fa_latency_check_mode* o = &res.modes[res.count];
        mode = (c.flags & FA_CHECK_ALL_MODES) ? (fa_latency_mode)i : home;
        if (!(c.flags & FA_CHECK_ALL_MODES) && i > 0) break;
        o->mode = mode;
        res.count++;
        if (!mode_available(mixer, mode)) { o->result = FA_ERROR_NOT_IMPLEMENTED; continue; }
        if (mode != mixer->device->open_mode) {
            o->result = fa_mixer_switch_latency_mode(mixer, mode);
            if (o->result != FA_OK) continue;
            settle(mic);
            measure_mode(mixer, mic, &c, o);
            /* Not heard right after a switch: once more, in case the input was still
             * settling. */
            if (o->result == FA_ERROR_NOT_FOUND) {
                settle(mic);
                measure_mode(mixer, mic, &c, o);
            }
        } else {
            measure_mode(mixer, mic, &c, o);
        }
        if (o->result == FA_OK) {
            /* Kept for this mic and mode: fa_mixer_get_latency and the vocals' alignment. */
            fa_mutex_lock(&mixer->lock);
            if (fa_load_ptr(&mixer->mics[slot]) == mic) fa_store64(&mic->measured[mode], o->mic_to_speaker);
            fa_mutex_unlock(&mixer->lock);
        }
    }
    /* Back to the mixer's own mode; when that fails the mixer stays in the last mode
     * measured, and the call says so (the measurements are still filled in). */
    if (mixer->device->open_mode != home) r = fa_mixer_switch_latency_mode(mixer, home);
    fa_store32(&mixer->checking, 0);
    /* Only the mixer's own mode measured, and that failed: the check's result. */
    if (res.count == 1 && res.modes[0].result != FA_OK && res.modes[0].result != FA_ERROR_NOT_FOUND)
        r = res.modes[0].result;
    /* The advice: the cheapest mode that is fast enough (exclusive only when shared is too
     * slow), else the fastest. */
    res.advice = FA_CHECK_NOT_HEARD;
    res.advised_mode = home;
    for (i = 0; i < 3 && !pick; i++) {
        uint32_t j;
        for (j = 0; j < res.count; j++)
            if (res.modes[j].mode == prefer[i] && res.modes[j].result == FA_OK &&
                res.modes[j].mic_to_speaker <= c.max_monitor) pick = &res.modes[j];
    }
    if (pick) {
        res.advice = pick->max_gain_db < 0.0f ? FA_CHECK_FEEDBACK : FA_CHECK_MONITOR;
        res.advised_mode = pick->mode;
    } else {
        for (i = 0; i < res.count; i++)
            if (res.modes[i].result == FA_OK && (!pick || res.modes[i].mic_to_speaker < pick->mic_to_speaker))
                pick = &res.modes[i];
        if (pick) {
            res.advice = FA_CHECK_SLOW;
            res.advised_mode = pick->mode;
        }
    }
    memcpy(out_result, &res, size);
    return r;
}
