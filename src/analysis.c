/* Levels and spectrum of what a stream or a device is playing.
 *
 * A tap keeps what was played lately, indexed by device frame: a stream's own
 * audio as the audio thread mixes it (after its equalizer, before volume, pan,
 * fades and normalization), or a device's whole output after the limiter.
 * It is made on the first request, so streams nobody measures cost nothing.
 * Frames never written (before the stream started in a callback, an underrun,
 * a pause) read as silence. The audio thread writes without locks and readers
 * read without locks, staying clear of the part the next callbacks will write.
 *
 * Readers end their window at the frame the speaker plays now
 * (fa_device_speaker_frame), so meters line up with the sound. */
#include "fa_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FA_SPECTRUM_MIN_LOG2 6    /* 64 */
#define FA_SPECTRUM_MAX_LOG2 14   /* 16384 */
#define FA_TWO_PI 6.283185307179586

/* Frames for 1 s windows on `dev`: a window ends at the speaker, up to the latency
 * and a callback behind the newest frame written, and stays a guard of two
 * callbacks clear of what the next ones write (view_open). */
static uint32_t tap_frames(fa_device* dev)
{
    uint32_t rate = dev ? dev->rate : 48000, frames = 65536, cb = 0;
    int64_t need = rate, latency = 0;
    if (dev) {
        cb = (uint32_t)fa_load32(&dev->period_frames);
        if (dev->cb_prev_frames > cb) cb = dev->cb_prev_frames;   /* the callbacks seen, if bigger */
        latency = (int64_t)((double)fa_load64(&dev->latency_ns) * rate / 1e9 + 0.5);
    }
    need += latency + 3 * (int64_t)cb + 1024;
    while ((int64_t)frames < need && frames < (1u << 22)) frames <<= 1;
    return frames;
}

static fa_tap* tap_new(uint32_t frames)
{
    fa_tap* t = (fa_tap*)calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->frames = frames;
    t->buf = (float*)calloc((size_t)frames * FA_DEVICE_CHANNELS, sizeof(float));
    if (!t->buf) { free(t); return NULL; }
    return t;
}

fa_tap* fa_tap_get(fa_atomic_ptr* slot, fa_device* device)
{
    fa_tap* t = (fa_tap*)fa_load_ptr(slot);
    if (t) return t;
    t = tap_new(tap_frames(device));
    if (!t) return NULL;
    if (!fa_cas_ptr(slot, NULL, t)) {   /* another thread made one first */
        fa_tap_free(t);
        t = (fa_tap*)fa_load_ptr(slot);
    }
    return t;
}

void fa_tap_fit(fa_atomic_ptr* slot, fa_device* device)
{
    fa_tap* t = (fa_tap*)fa_load_ptr(slot), *bigger;
    if (!t || t->frames >= tap_frames(device)) return;
    bigger = tap_new(tap_frames(device));
    if (!bigger) return;   /* windows are shortened to what fits */
    fa_exchange_ptr(slot, bigger);
    fa_device_wait_quiescent(device);   /* a callback still writing the old one is done */
    fa_tap_free(t);
}

void fa_tap_free(fa_tap* t)
{
    if (!t) return;
    free(t->buf);
    free(t);
}

static void tap_put(fa_tap* t, int64_t at, const float* frames, int64_t n)
{
    while (n > 0) {
        int64_t i = at & (t->frames - 1);
        int64_t k = (int64_t)t->frames - i < n ? (int64_t)t->frames - i : n;
        if (frames) {
            memcpy(t->buf + i * 2, frames, (size_t)k * 2 * sizeof(float));
            frames += k * 2;
        } else {
            memset(t->buf + i * 2, 0, (size_t)k * 2 * sizeof(float));
        }
        at += k;
        n -= k;
    }
}

void fa_tap_write(fa_tap* t, int64_t at, const float* frames, int64_t n)
{
    int64_t end = fa_load64(&t->end);
    if (n <= 0) return;
    if (at < end || end == 0) {
        /* First write, or the stream moved to another device (a new clock). */
        fa_store64(&t->start, at);
    } else if (at > end) {
        int64_t gap_from = at - end > (int64_t)t->frames ? at - (int64_t)t->frames : end;
        tap_put(t, gap_from, NULL, at - gap_from);   /* not played: silence */
    }
    tap_put(t, at, frames, n);
    fa_store64(&t->end, at + n);
}

/* A window [from, from + n) of a tap. Frames outside [lo, hi) read as silence:
 * below `lo` they were never written or the next callbacks may overwrite them
 * during the read; from `hi` on they are not written yet. */
typedef struct tap_view {
    const fa_tap* t;
    int64_t from, n, lo, hi;
} tap_view;

/* `window` > 0: levels, n from the window, shortened to fit; else n = `frames`. */
static int view_open(tap_view* v, fa_tap* t, fa_device* dev, int64_t speaker, fa_time window, int64_t frames)
{
    int64_t end = fa_load64(&t->end), start = fa_load64(&t->start);
    int64_t guard = 2 * (int64_t)dev->cb_prev_frames + 1024;   /* the next callbacks write this far past `end` */
    int64_t n = window > 0 ? (int64_t)((double)window * dev->rate / 1e9) : frames;
    int64_t max_n = (int64_t)t->frames - (end > speaker ? end - speaker : 0) - guard;
    if (n < 1) n = 1;
    if (window > 0 && n > max_n) n = max_n;
    if (n < 1) return 0;
    v->t = t;
    v->from = speaker - n;
    v->n = n;
    v->lo = end + guard - (int64_t)t->frames;
    if (v->lo < start) v->lo = start;
    v->hi = end;
    return 1;
}

static void view_frame(const tap_view* v, int64_t f, float* l, float* r)
{
    int64_t at = v->from + f;
    if (at < v->lo || at >= v->hi) { *l = *r = 0.0f; return; }
    at &= v->t->frames - 1;
    *l = v->t->buf[at * 2];
    *r = v->t->buf[at * 2 + 1];
}

/* A device whose callbacks stopped (fa_device_stop, a lost device) is silent. */
static int device_live(fa_device* dev)
{
    int64_t last = fa_load64(&dev->cb_time);
    int64_t period_ns = (int64_t)((double)dev->cb_prev_frames * 1e9 / (dev->rate ? dev->rate : 48000));
    return last != 0 && fa_monotonic_ns() - last < 250000000 + 3 * period_ns;
}

/* ---- levels ------------------------------------------------------------- */

static void levels(const tap_view* v, float* peak, float* rms, uint32_t channels)
{
    double sq[2] = { 0.0, 0.0 };
    float pk[2] = { 0.0f, 0.0f };
    int64_t f;
    for (f = 0; f < v->n; f++) {
        float l, r;
        view_frame(v, f, &l, &r);
        if (fabsf(l) > pk[0]) pk[0] = fabsf(l);
        if (fabsf(r) > pk[1]) pk[1] = fabsf(r);
        sq[0] += (double)l * l;
        sq[1] += (double)r * r;
    }
    if (channels == 1) {
        if (peak) peak[0] = pk[0] > pk[1] ? pk[0] : pk[1];
        if (rms) rms[0] = (float)sqrt((sq[0] + sq[1]) / (2.0 * (double)v->n));
    } else {
        if (peak) { peak[0] = pk[0]; peak[1] = pk[1]; }
        if (rms) { rms[0] = (float)sqrt(sq[0] / (double)v->n); rms[1] = (float)sqrt(sq[1] / (double)v->n); }
    }
}

static void zero_levels(float* peak, float* rms, uint32_t channels)
{
    if (peak) memset(peak, 0, channels * sizeof(float));
    if (rms) memset(rms, 0, channels * sizeof(float));
}

/* ---- spectrum ----------------------------------------------------------- */

/* Per size: cos and sin of 2 pi k / n for k < n/2, the Hann window and its sum.
 * Made once and kept until the process ends, like the MDCT tables. */
typedef struct fft_tables {
    float* cs;
    float* sn;
    float* hann;
    double hann_sum;
} fft_tables;

static fa_atomic_ptr g_fft_tables[FA_SPECTRUM_MAX_LOG2 + 1];

static const fft_tables* tables_for(uint32_t n, int log2n)
{
    fft_tables* t = (fft_tables*)fa_load_ptr(&g_fft_tables[log2n]);
    uint32_t i;
    if (t) return t;
    t = (fft_tables*)calloc(1, sizeof(*t) + (size_t)n * 2 * sizeof(float));
    if (!t) return NULL;
    t->cs = (float*)(t + 1);
    t->sn = t->cs + n / 2;
    t->hann = t->sn + n / 2;
    for (i = 0; i < n / 2; i++) {
        t->cs[i] = (float)cos(FA_TWO_PI * i / n);
        t->sn[i] = (float)sin(FA_TWO_PI * i / n);
    }
    for (i = 0; i < n; i++) {
        t->hann[i] = (float)(0.5 - 0.5 * cos(FA_TWO_PI * i / n));
        t->hann_sum += t->hann[i];
    }
    if (!fa_cas_ptr(&g_fft_tables[log2n], NULL, t)) {
        free(t);
        t = (fft_tables*)fa_load_ptr(&g_fft_tables[log2n]);
    }
    return t;
}

/* In-place radix-2 FFT of n complex values. */
static void fft(float* re, float* im, const fft_tables* tb, uint32_t n)
{
    uint32_t i, j, len;
    for (i = 1, j = 0; i < n; i++) {
        uint32_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j |= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (len = 2; len <= n; len <<= 1) {
        uint32_t half = len >> 1, step = n / len, k;
        for (i = 0; i < n; i += len)
            for (k = 0; k < half; k++) {
                float wr = tb->cs[k * step], wi = -tb->sn[k * step];
                uint32_t a = i + k, b = a + half;
                float tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr; im[a] += ti;
            }
    }
}

static fa_result spectrum(const tap_view* v, const fft_tables* tb, uint32_t flags, float* bins)
{
    uint32_t n = (uint32_t)v->n, i;
    int window = !(flags & FA_SPECTRUM_NO_WINDOW);
    double wsum = window ? tb->hann_sum : (double)n;
    float* re = (float*)malloc((size_t)n * 2 * sizeof(float));
    float* im;
    if (!re) return FA_ERROR_OUT_OF_MEMORY;
    im = re + n;
    for (i = 0; i < n; i++) {
        float l, r, x;
        view_frame(v, i, &l, &r);
        x = (flags & FA_SPECTRUM_LEFT) ? l : (flags & FA_SPECTRUM_RIGHT) ? r : 0.5f * (l + r);
        re[i] = window ? x * tb->hann[i] : x;
        im[i] = 0.0f;
    }
    fft(re, im, tb, n);
    /* A full-scale sine centred on a bin reads 1.0 there. */
    for (i = 0; i < n / 2; i++)
        bins[i] = (float)(sqrt((double)re[i] * re[i] + (double)im[i] * im[i]) * (i ? 2.0 : 1.0) / wsum);
    free(re);
    return FA_OK;
}

/* log2 of a valid size, else -1. */
static int spectrum_size_log2(uint32_t size, uint32_t flags, const float* bins)
{
    int l = 0;
    if (!bins || size == 0 || (size & (size - 1))) return -1;
    while ((1u << l) < size) l++;
    if (l < FA_SPECTRUM_MIN_LOG2 || l > FA_SPECTRUM_MAX_LOG2) return -1;
    if ((flags & FA_SPECTRUM_LEFT) && (flags & FA_SPECTRUM_RIGHT)) return -1;
    if (flags & ~(uint32_t)(FA_SPECTRUM_LEFT | FA_SPECTRUM_RIGHT | FA_SPECTRUM_NO_WINDOW)) return -1;
    return l;
}

/* ---- API ---------------------------------------------------------------- */

/* Where a window ends: the frame the speaker plays now. A stream tap holds mix
 * frames; a device tap holds the limiter's output, `delay` frames behind the mix
 * it came from (fa_device_speaker_frame already counts that delay). */
static int64_t speaker_end(fa_device* dev, int after_limiter)
{
    double sp = fa_device_speaker_frame(dev, fa_monotonic_ns());
    return (int64_t)floor(sp) + (after_limiter ? (int64_t)dev->limiter.delay : 0);
}

typedef struct measure {
    fa_time window;           /* levels: > 0 */
    float *peak, *rms;
    uint32_t channels;
    uint32_t size;            /* spectrum: bins != NULL */
    int log2n;
    uint32_t flags;
    float *bins, *bin_hz;
} measure;

static fa_result run_measure(const measure* m, fa_tap* t, fa_device* dev, int after_limiter)
{
    tap_view v;
    if (device_live(dev) && view_open(&v, t, dev, speaker_end(dev, after_limiter), m->window, m->size)) {
        if (m->bins) {
            const fft_tables* tb = tables_for(m->size, m->log2n);
            if (!tb) return FA_ERROR_OUT_OF_MEMORY;
            if (m->bin_hz) *m->bin_hz = (float)dev->rate / m->size;
            return spectrum(&v, tb, m->flags, m->bins);
        }
        levels(&v, m->peak, m->rms, m->channels);
        return FA_OK;
    }
    return FA_ERROR_NOT_FOUND;   /* nothing playing */
}

static void zero_measure(const measure* m)
{
    if (m->bins) {
        memset(m->bins, 0, (m->size / 2) * sizeof(float));
        if (m->bin_hz) *m->bin_hz = 0.0f;
    } else {
        zero_levels(m->peak, m->rms, m->channels);
    }
}

/* api_lock keeps the stream's device from being closed underneath: fa_device_close
 * detaches every stream first, and detaching takes this lock. */
static fa_result stream_measure(struct fa_stream* s, const measure* m)
{
    fa_result r = FA_ERROR_NOT_FOUND;
    fa_device* dev;
    fa_mutex_lock(&s->api_lock);
    dev = s->device;
    {
        /* Made also before the stream is attached, so a meter set up first sees it start;
         * made bigger when the device's latency or callbacks grew past what it holds. */
        fa_tap* t;
        if (dev) fa_tap_fit(&s->tap, dev);
        t = fa_tap_get(&s->tap, dev);
        if (!t) r = FA_ERROR_OUT_OF_MEMORY;
        else if (dev && fa_load32(&s->state) == FA_STATE_PLAYING) r = run_measure(m, t, dev, 0);
    }
    fa_mutex_unlock(&s->api_lock);
    if (r == FA_ERROR_NOT_FOUND) { zero_measure(m); r = FA_OK; }
    return r;
}

static fa_result device_measure(fa_device* dev, const measure* m)
{
    fa_tap* t = fa_tap_get(&dev->tap, dev);
    fa_result r;
    if (!t) return FA_ERROR_OUT_OF_MEMORY;
    r = run_measure(m, t, dev, 1);
    if (r == FA_ERROR_NOT_FOUND) { zero_measure(m); r = FA_OK; }
    return r;
}

static int levels_args(measure* m, fa_time window, float* peak, float* rms, uint32_t channels)
{
    memset(m, 0, sizeof(*m));
    m->window = window;
    m->peak = peak;
    m->rms = rms;
    m->channels = channels;
    return window > 0 && (channels == 1 || channels == 2) && (peak || rms);
}

static int spectrum_args(measure* m, uint32_t size, uint32_t flags, float* bins, float* bin_hz)
{
    memset(m, 0, sizeof(*m));
    m->size = size;
    m->flags = flags;
    m->bins = bins;
    m->bin_hz = bin_hz;
    m->log2n = spectrum_size_log2(size, flags, bins);
    return m->log2n >= 0;
}

FA_API fa_result FA_CALL fa_stream_get_levels(fa_stream* stream, fa_time window, float* out_peak, float* out_rms, uint32_t channels)
{
    measure m;
    if (!levels_args(&m, window, out_peak, out_rms, channels) || !stream || (stream->flags & FA_OPEN_DECODE_ONLY))
        return FA_ERROR_INVALID_ARG;
    return stream_measure(stream, &m);
}

FA_API fa_result FA_CALL fa_stream_get_spectrum(fa_stream* stream, uint32_t size, uint32_t flags, float* out_bins, float* out_bin_hz)
{
    measure m;
    if (!spectrum_args(&m, size, flags, out_bins, out_bin_hz) || !stream || (stream->flags & FA_OPEN_DECODE_ONLY))
        return FA_ERROR_INVALID_ARG;
    return stream_measure(stream, &m);
}

FA_API fa_result FA_CALL fa_device_get_levels(fa_device* device, fa_time window, float* out_peak, float* out_rms, uint32_t channels)
{
    measure m;
    if (!levels_args(&m, window, out_peak, out_rms, channels) || !device) return FA_ERROR_INVALID_ARG;
    return device_measure(device, &m);
}

FA_API fa_result FA_CALL fa_device_get_spectrum(fa_device* device, uint32_t size, uint32_t flags, float* out_bins, float* out_bin_hz)
{
    measure m;
    if (!spectrum_args(&m, size, flags, out_bins, out_bin_hz) || !device) return FA_ERROR_INVALID_ARG;
    return device_measure(device, &m);
}

fa_result fa_tap_levels(fa_atomic_ptr* slot, fa_device* device, fa_time window, float* out_peak, float* out_rms, uint32_t channels)
{
    measure m;
    fa_tap* t;
    fa_result r;
    if (!levels_args(&m, window, out_peak, out_rms, channels) || !device) return FA_ERROR_INVALID_ARG;
    fa_tap_fit(slot, device);
    t = fa_tap_get(slot, device);
    if (!t) return FA_ERROR_OUT_OF_MEMORY;
    r = run_measure(&m, t, device, 0);
    if (r == FA_ERROR_NOT_FOUND) { zero_measure(&m); r = FA_OK; }
    return r;
}
