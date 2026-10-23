/* Musical key from audio (clean room, from published methods), and key names.
 *
 * Every half window (an FFT of about 0.17 s) the spectral peaks from 55 Hz to
 * 5 kHz are placed on a pitch circle of 120 steps (10 cents each), weighted by
 * their magnitude; each frame counts equally (its weights sum to 1), so loud
 * passages do not outvote quiet ones. At the end the circle gives the tuning (the
 * mean offset of the peaks from equal temperament at A = 440 Hz, as songs are
 * often a little sharp or flat) and is folded into the 12 pitch classes around
 * the tuned semitones. The key is the one of the 24 whose Krumhansl-Kessler
 * profile (major or minor, rotated to each tonic) correlates best with that
 * chroma. The confidence grows with the correlation and with its lead over the
 * runner-up. */
#include "fa_internal.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FINE 120                 /* pitch circle steps: 10 cents */
#define KEY_MIN_HZ 55.0
#define KEY_MAX_HZ 5000.0
#define KEY_MIN_FRAMES 8

struct fa_chroma {
    uint32_t rate, n, hop;
    const fa_fft_tables* tb;
    float* ring;
    uint32_t ring_pos, until_hop;
    float *re, *im, *mag;
    uint32_t kmin, kmax;
    double fine[FINE];
    int64_t frames;
};

static const char* const g_major_names[12] = { "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
static const char* const g_minor_names[12] = { "Cm", "C#m", "Dm", "Ebm", "Em", "Fm", "F#m", "Gm", "G#m", "Am", "Bbm", "Bm" };
static void key_make(int32_t tonic, int32_t minor, fa_key_info* k);

static const double g_major[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
static const double g_minor[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };

fa_chroma* fa_chroma_new(uint32_t rate)
{
    fa_chroma* c;
    uint32_t n = 1024;
    if (rate == 0) return NULL;
    while ((double)n < rate * 0.17 && n < (1u << FA_FFT_MAX_LOG2)) n <<= 1;
    c = (fa_chroma*)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->rate = rate;
    c->n = n;
    c->hop = n / 2;
    c->until_hop = c->hop;
    c->tb = fa_fft_tables_get(n);
    c->ring = (float*)calloc(n, sizeof(float));
    c->re = (float*)malloc((size_t)n * sizeof(float));
    c->im = (float*)malloc((size_t)n * sizeof(float));
    c->mag = (float*)malloc((size_t)(n / 2 + 1) * sizeof(float));
    c->kmin = (uint32_t)(KEY_MIN_HZ * n / rate);
    c->kmax = (uint32_t)(KEY_MAX_HZ * n / rate);
    if (c->kmin < 2) c->kmin = 2;
    if (c->kmax > n / 2 - 2) c->kmax = n / 2 - 2;
    if (!c->tb || !c->ring || !c->re || !c->im || !c->mag) { fa_chroma_free(c); return NULL; }
    return c;
}

void fa_chroma_free(fa_chroma* c)
{
    if (!c) return;
    free(c->ring);
    free(c->re);
    free(c->im);
    free(c->mag);
    free(c);
}

static void chroma_frame(fa_chroma* c)
{
    uint32_t i, k, n = c->n;
    double fr[FINE], total = 0.0;
    float top = 0.0f;
    for (i = 0; i < n; i++) {
        c->re[i] = c->ring[(c->ring_pos + i) % n] * c->tb->hann[i];
        c->im[i] = 0.0f;
    }
    fa_fft(c->re, c->im, c->tb, n);
    for (k = c->kmin - 1; k <= c->kmax + 1; k++) {
        c->mag[k] = sqrtf(c->re[k] * c->re[k] + c->im[k] * c->im[k]);
        if (c->mag[k] > top) top = c->mag[k];
    }
    /* Below about -80 dBFS the frame is silence. */
    if (top < 1e-4f * (float)c->tb->hann_sum) return;
    memset(fr, 0, sizeof(fr));
    for (k = c->kmin; k <= c->kmax; k++) {
        float m = c->mag[k];
        double a, b, d, delta = 0.0, f, p;
        int j;
        if (m < 0.01f * top || m <= c->mag[k - 1] || m < c->mag[k + 1]) continue;
        a = log(c->mag[k - 1] + 1e-12);
        b = log(c->mag[k + 1] + 1e-12);
        d = a - 2.0 * log(m) + b;
        if (d < 0.0) delta = 0.5 * (a - b) / d;
        f = ((double)k + delta) * c->rate / n;
        p = 12.0 * log2(f / 440.0) + 69.0;          /* MIDI note number */
        j = (int)floor(p * 10.0 + 0.5) % FINE;
        if (j < 0) j += FINE;
        fr[j] += m;
        total += m;
    }
    if (total <= 0.0) return;
    for (i = 0; i < FINE; i++) c->fine[i] += fr[i] / total;
    c->frames++;
}

void fa_chroma_process(fa_chroma* c, const float* mono, int64_t frames)
{
    int64_t i;
    for (i = 0; i < frames; i++) {
        c->ring[c->ring_pos] = mono[i];
        c->ring_pos = (c->ring_pos + 1) % c->n;
        if (--c->until_hop == 0) {
            c->until_hop = c->hop;
            chroma_frame(c);
        }
    }
}

static double correlate(const double* x, const double* prof, int rot)
{
    double mx = 0, mp = 0, sxy = 0, sxx = 0, spp = 0;
    int i;
    for (i = 0; i < 12; i++) { mx += x[i]; mp += prof[i]; }
    mx /= 12.0;
    mp /= 12.0;
    for (i = 0; i < 12; i++) {
        double a = x[(i + rot) % 12] - mx, b = prof[i] - mp;
        sxy += a * b; sxx += a * a; spp += b * b;
    }
    return sxx > 0.0 && spp > 0.0 ? sxy / sqrt(sxx * spp) : 0.0;
}

int fa_chroma_key(fa_chroma* c, fa_key_info* out)
{
    double sr = 0.0, si = 0.0, offset, chroma[12], best = -2.0, second = -2.0, conf;
    int i, pc, tonic = 0, minor = 0;
    memset(out, 0, sizeof(*out));
    if (c->frames < KEY_MIN_FRAMES) return 0;
    /* Tuning: the mean position of the peaks within their semitone. */
    for (i = 0; i < FINE; i++) {
        double ang = 6.283185307179586 * (double)(i % 10) / 10.0;
        sr += c->fine[i] * cos(ang);
        si += c->fine[i] * sin(ang);
    }
    offset = atan2(si, sr) / 6.283185307179586 * 10.0;   /* steps, -5..5 */
    memset(chroma, 0, sizeof(chroma));
    for (i = 0; i < FINE; i++) {
        double semis = ((double)i - offset) / 10.0;      /* tuned semitone, 0 = C */
        for (pc = 0; pc < 12; pc++) {
            double d = semis - pc;
            d -= 12.0 * floor(d / 12.0 + 0.5);
            d = fabs(d);
            if (d < 0.5) chroma[pc] += c->fine[i] * (1.0 - 2.0 * d);
        }
    }
    for (pc = 0; pc < 12; pc++)
        for (i = 0; i < 2; i++) {
            double r = correlate(chroma, i ? g_minor : g_major, pc);
            if (r > best) { second = best; best = r; tonic = pc; minor = i; }
            else if (r > second) second = r;
        }
    if (best < 0.2) return 0;
    conf = best * ((best - second) / 0.1 > 1.0 ? 1.0 : (best - second) / 0.1);
    key_make(tonic, minor, out);
    out->confidence = (float)(conf < 0.0 ? 0.0 : conf > 1.0 ? 1.0 : conf);
    return 1;
}


static void key_make(int32_t tonic, int32_t minor, fa_key_info* k)
{
    int major_tonic, number;
    memset(k, 0, sizeof(*k));
    k->struct_size = sizeof(fa_key_info);
    k->tonic = tonic;
    k->minor = minor;
    k->confidence = 1.0f;
    snprintf(k->name, sizeof(k->name), "%s", minor ? g_minor_names[tonic] : g_major_names[tonic]);
    /* Camelot: C major is 8B and each fifth up adds one; a minor key has its
     * relative major's number (A minor = 8A). */
    major_tonic = minor ? (tonic + 3) % 12 : tonic;
    number = (major_tonic * 7 + 7) % 12 + 1;
    k->camelot[0] = (char)(number >= 10 ? '1' : '0' + number);
    k->camelot[1] = (char)(number >= 10 ? '0' + number - 10 : minor ? 'A' : 'B');
    k->camelot[2] = (char)(number >= 10 ? (minor ? 'A' : 'B') : 0);
    k->camelot[3] = 0;
}

FA_API fa_result FA_CALL fa_key_make(int32_t tonic, int32_t minor, fa_key_info* out_key)
{
    fa_key_info k;
    if (!out_key || out_key->struct_size < sizeof(uint32_t) || tonic < 0 || tonic > 11 || (minor != 0 && minor != 1))
        return FA_ERROR_INVALID_ARG;
    key_make(tonic, minor, &k);
    fa_copy_sized(out_key, &k, sizeof(k));
    return FA_OK;
}

FA_API fa_result FA_CALL fa_key_transpose(const fa_key_info* key, int32_t semitones, fa_key_info* out_key)
{
    fa_key_info k;
    int32_t tonic;
    if (!key || !out_key || out_key->struct_size < sizeof(uint32_t) || key->struct_size < offsetof(fa_key_info, confidence) + sizeof(float) ||
        key->tonic < 0 || key->tonic > 11 || (key->minor != 0 && key->minor != 1))
        return FA_ERROR_INVALID_ARG;
    tonic = (int32_t)(((int64_t)key->tonic + semitones) % 12);
    if (tonic < 0) tonic += 12;
    key_make(tonic, key->minor, &k);
    k.confidence = key->confidence;
    fa_copy_sized(out_key, &k, sizeof(k));
    return FA_OK;
}
