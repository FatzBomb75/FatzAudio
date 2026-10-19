/* Variable-ratio resampler for FA_PARAM_RATE (varispeed: tempo and pitch move
 * together, like a record or tape played faster). Windowed-sinc interpolation with
 * a Kaiser window, 96 taps, from a 256-phase table interpolated linearly: about
 * -90 dB of aliasing and imaging, flat to 88% of the input's Nyquist frequency.
 * Speeding up (ratio > 1) widens the kernel by the ratio, which lowers the cutoff
 * to the output's Nyquist frequency, so nothing aliases. */
#include "tempo.h"
#include <math.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#define VS_HALF FA_VARISPEED_HALF  /* taps on each side at ratio <= 1 */
#define VS_PHASES 256              /* table steps per tap */
#define VS_CUTOFF 0.94             /* fraction of Nyquist */
#define VS_BETA 9.0                /* Kaiser window: ~ -90 dB sidelobes */
#define VS_MAX_RATIO FA_VARISPEED_MAX_RATIO /* widest kernel kept in the history: pitch +36 (x8) at rate 10 */
#define VS_KEEP ((int64_t)(VS_HALF * VS_MAX_RATIO) + 2)

/* Modified Bessel function of the first kind, order 0 (series). */
static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0, q = x * x / 4.0;
    int k;
    for (k = 1; k < 64; k++) {
        term *= q / ((double)k * (double)k);
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

fa_result fa_varispeed_init(fa_varispeed* v, uint32_t channels)
{
    int i, n = VS_HALF * VS_PHASES + 2;
    double i0b = bessel_i0(VS_BETA);
    memset(v, 0, sizeof(*v));
    v->channels = channels;
    v->table = (float*)malloc((size_t)n * sizeof(float));
    if (!v->table) return FA_ERROR_OUT_OF_MEMORY;
    for (i = 0; i < n; i++) {
        double t = (double)i / VS_PHASES, x = VS_CUTOFF * t, sinc, w, r = t / VS_HALF;
        sinc = x == 0.0 ? 1.0 : sin(FA_TEMPO_PI * x) / (FA_TEMPO_PI * x);
        w = r >= 1.0 ? 0.0 : bessel_i0(VS_BETA * sqrt(1.0 - r * r)) / i0b;
        v->table[i] = (float)(VS_CUTOFF * sinc * w);
    }
    return FA_OK;
}

void fa_varispeed_free(fa_varispeed* v)
{
    free(v->table);
    free(v->buf);
    memset(v, 0, sizeof(*v));
}

void fa_varispeed_reset(fa_varispeed* v)
{
    v->count = 0;
    v->pos = 0.0;
}

static fa_result reserve(fa_varispeed* v, int64_t frames)
{
    float* b;
    int64_t cap;
    if (v->count + frames <= v->cap) return FA_OK;
    /* Drop what no output can reach any more first. */
    if (v->pos > (double)VS_KEEP) {
        int64_t drop = (int64_t)v->pos - VS_KEEP;
        if (drop > v->count) drop = v->count;
        memmove(v->buf, v->buf + (size_t)drop * v->channels, (size_t)(v->count - drop) * v->channels * sizeof(float));
        v->count -= drop;
        v->pos -= (double)drop;
        if (v->count + frames <= v->cap) return FA_OK;
    }
    cap = v->cap ? v->cap : 4096;
    while (cap < v->count + frames) cap *= 2;
    b = (float*)realloc(v->buf, (size_t)cap * v->channels * sizeof(float));
    if (!b) return FA_ERROR_OUT_OF_MEMORY;
    v->buf = b;
    v->cap = cap;
    return FA_OK;
}

fa_result fa_varispeed_prime(fa_varispeed* v, const float* history, int64_t frames)
{
    fa_result r;
    v->count = 0;
    v->pos = 0.0;
    r = reserve(v, frames);
    if (r != FA_OK) return r;
    if (frames > 0) memcpy(v->buf, history, (size_t)frames * v->channels * sizeof(float));
    v->count = frames;
    v->pos = (double)frames;
    return FA_OK;
}

fa_result fa_varispeed_write(fa_varispeed* v, const float* in, int64_t frames)
{
    fa_result r = reserve(v, frames);
    if (r != FA_OK) return r;
    if (in) memcpy(v->buf + (size_t)v->count * v->channels, in, (size_t)frames * v->channels * sizeof(float));
    else memset(v->buf + (size_t)v->count * v->channels, 0, (size_t)frames * v->channels * sizeof(float));
    v->count += frames;
    return FA_OK;
}

/* Adds the taps on one side of the output time: input frames x, x - step, ...
 * (step -1 or +1 frame) at kernel positions t, t + dt, ... until the kernel ends. */
static void taps(const fa_varispeed* v, const float* x, int64_t avail, int dir, float t, float dt, float scale, float* acc)
{
    const float* table = v->table;
    uint32_t ch = v->channels, c;
    const float end = (float)(VS_HALF * VS_PHASES);
    int64_t m;
    for (m = 0; m < avail && t < end; m++, t += dt) {
        int32_t ti = (int32_t)t;
        float h = (table[ti] + (table[ti + 1] - table[ti]) * (t - (float)ti)) * scale;
        const float* f = x + (ptrdiff_t)dir * m * (ptrdiff_t)ch;
        for (c = 0; c < ch; c++) acc[c] += f[c] * h;
    }
}

int64_t fa_varispeed_width(double ratio)
{
    double scale = ratio > 1.0 ? 1.0 / ratio : 1.0;
    return (int64_t)ceil((double)VS_HALF / scale);
}

int64_t fa_varispeed_read(fa_varispeed* v, double ratio, float* out, int64_t max_frames)
{
    uint32_t ch = v->channels, c;
    double scale = ratio > 1.0 ? 1.0 / ratio : 1.0;
    int64_t width = fa_varispeed_width(ratio), n = 0;
    float steps = (float)(VS_PHASES * scale);
    while (n < max_frames) {
        int64_t i0 = (int64_t)floor(v->pos);
        float frac = (float)(v->pos - (double)i0), acc[FA_TEMPO_MAX_CHANNELS];
        if (i0 + width >= v->count) break;       /* needs input further ahead */
        for (c = 0; c < ch; c++) acc[c] = 0.0f;
        /* At or before the output time (i0, i0 - 1, ...), then after it (i0 + 1, ...). */
        taps(v, v->buf + (size_t)i0 * ch, i0 + 1, -1, frac * steps, steps, (float)scale, acc);
        taps(v, v->buf + (size_t)(i0 + 1) * ch, v->count - i0 - 1, 1, (1.0f - frac) * steps, steps, (float)scale, acc);
        for (c = 0; c < ch; c++) out[(size_t)n * ch + c] = acc[c];
        n++;
        v->pos += ratio;
    }
    return n;
}

double fa_varispeed_pending(const fa_varispeed* v)
{
    return (double)v->count - v->pos;
}

int64_t fa_varispeed_tail(void)
{
    return VS_KEEP;
}
