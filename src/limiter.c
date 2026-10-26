/* Look-ahead true-peak limiter for a device's output.
 *
 * For every incoming frame the required gain is ceiling / true peak (4x
 * oversampled, so peaks between samples count too). The gain applied is
 *   a sliding minimum of that over `window` frames,
 *   averaged over `attack` frames (a smooth fade into the reduction),
 *   then released slowly (one-pole, 150 ms),
 * and the audio is delayed by `delay` frames. With window = attack + G + 1 and
 * delay = attack + G (G covers the oversampler's own delay), every output frame
 * gets a gain no larger than what any peak near it requires. Audio below the
 * ceiling passes unchanged. Runs on the audio thread: no locks, no allocation.
 *
 * Detection runs all the time, also while the limiter is off, so turning it on
 * finds the gain already settled. On and off fade over 20 ms, and each delayed
 * frame keeps the ceiling it was measured against, so neither switching nor a
 * new ceiling ever clips audio already in the delay line. */
#include "fa_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FA_LIMITER_ATTACK_MS 1.5
#define FA_LIMITER_RELEASE_MS 150.0
#define FA_LIMITER_TP_DELAY 17  /* >= the oversampler's group delay (about 16 frames) */
#define FA_LIMITER_SWITCH_MS 20.0

fa_result fa_limiter_init(fa_limiter* l, uint32_t rate, uint32_t channels)
{
    memset(l, 0, sizeof(*l));
    l->channels = channels;
    l->attack = (int)(rate * FA_LIMITER_ATTACK_MS / 1000.0 + 0.5);
    if (l->attack < 1) l->attack = 1;
    l->window = l->attack + FA_LIMITER_TP_DELAY + 1;
    l->delay = l->attack + FA_LIMITER_TP_DELAY;
    l->dq_cap = l->window + 2;
    l->audio = (float*)calloc((size_t)l->delay * channels, sizeof(float));
    l->ceil_line = (float*)calloc((size_t)l->delay, sizeof(float));
    l->box = (float*)malloc((size_t)l->attack * sizeof(float));
    l->dq_idx = (int64_t*)malloc((size_t)l->dq_cap * sizeof(int64_t));
    l->dq_val = (float*)malloc((size_t)l->dq_cap * sizeof(float));
    if (!l->audio || !l->ceil_line || !l->box || !l->dq_idx || !l->dq_val || fa_truepeak_init(&l->tp, rate, channels) != FA_OK) {
        fa_limiter_free(l);
        return FA_ERROR_OUT_OF_MEMORY;
    }
    {
        int i;
        for (i = 0; i < l->attack; i++) l->box[i] = 1.0f;
    }
    l->box_sum = (double)l->attack;
    l->gain = 1.0f;
    l->release = (float)exp(-1.0 / (rate * FA_LIMITER_RELEASE_MS / 1000.0));
    l->mix_step = (float)(1000.0 / (rate * FA_LIMITER_SWITCH_MS));
    l->mix = -1.0f;                       /* set from the first call: no fade at start */
    return FA_OK;
}

void fa_limiter_free(fa_limiter* l)
{
    free(l->audio);
    free(l->ceil_line);
    free(l->box);
    free(l->dq_idx);
    free(l->dq_val);
    fa_truepeak_free(&l->tp);
    memset(l, 0, sizeof(*l));
}

/* One frame of gain smoothing and delay, for a frame whose true peak needs gain
 * `need`. Returns the gain applied to the frame going out. */
static float limit_frame(fa_limiter* l, float* frame, float need, float ceiling, float target)
{
    uint32_t c, ch = l->channels;
    float* slot;
    float gmin, box_avg, g, ge, limit;
    int tail;

    /* Sliding minimum over the last `window` frames (monotonic deque). The
     * indices wrap by comparison: head < dq_cap and count <= dq_cap. */
    for (;;) {
        if (l->dq_count == 0) break;
        tail = l->dq_head + l->dq_count - 1;
        if (tail >= l->dq_cap) tail -= l->dq_cap;
        if (l->dq_val[tail] < need) break;
        l->dq_count--;
    }
    tail = l->dq_head + l->dq_count;
    if (tail >= l->dq_cap) tail -= l->dq_cap;
    l->dq_idx[tail] = l->n;
    l->dq_val[tail] = need;
    l->dq_count++;
    while (l->dq_idx[l->dq_head] <= l->n - l->window) {
        if (++l->dq_head == l->dq_cap) l->dq_head = 0;
        l->dq_count--;
    }
    gmin = l->dq_val[l->dq_head];

    /* Box average over `attack` minima. */
    l->box_sum += (double)gmin - (double)l->box[l->bpos];
    l->box[l->bpos] = gmin;
    if (++l->bpos == l->attack) l->bpos = 0;
    if ((l->n & 0xffff) == 0) {       /* re-sum now and then so rounding cannot drift */
        int k;
        l->box_sum = 0.0;
        for (k = 0; k < l->attack; k++) l->box_sum += l->box[k];
    }
    box_avg = (float)(l->box_sum / (double)l->attack);
    if (box_avg > 1.0f) box_avg = 1.0f;

    /* Instant into reduction, smooth release out of it. A release step that no
     * longer moves the gain (float rounding, about 0.002 dB short of 1) lands on
     * the target, so the gain gets back to exactly 1. */
    if (box_avg < l->gain) g = box_avg;
    else {
        g = box_avg + (l->gain - box_avg) * l->release;
        if (g == l->gain) g = box_avg;
    }
    l->gain = g;

    /* Fade between bypass and limiting when switched. */
    if (l->mix < target) { l->mix += l->mix_step; if (l->mix > target) l->mix = target; }
    else if (l->mix > target) { l->mix -= l->mix_step; if (l->mix < target) l->mix = target; }
    ge = l->mix >= 1.0f ? g : 1.0f + l->mix * (g - 1.0f);

    /* Delay line of `delay` frames: the frame from `delay` frames ago goes out,
     * limited to the ceiling it was measured against. */
    slot = l->audio + (size_t)l->apos * ch;
    limit = l->ceil_line[l->apos];
    for (c = 0; c < ch; c++) {
        float in = frame[c], out = slot[c] * ge;
        if (l->mix >= 1.0f) {          /* guard against float rounding at the edge */
            if (out > limit) out = limit;
            else if (out < -limit) out = -limit;
        }
        slot[c] = in;
        frame[c] = out;
    }
    l->ceil_line[l->apos] = ceiling;
    if (++l->apos == l->delay) l->apos = 0;
    l->n++;
    return ge;
}

/* 1 when the gain has released all the way: the box average and the gain are 1. */
static int released(const fa_limiter* l)
{
    float box_avg = (float)(l->box_sum / (double)l->attack);
    return box_avg >= 1.0f && l->gain == 1.0f;
}

/* A block while nothing in the window needs a reduction, the gain has released
 * (see released) and no switch is fading: what limit_frame does with need 1 for
 * every frame, without the deque, box and release arithmetic (none of it moves). */
static void pass_block(fa_limiter* l, float* b, uint32_t n, float ceiling)
{
    uint32_t i, c, ch = l->channels;
    float ge = 1.0f;   /* the gain is 1 (released) */
    for (i = 0; i < n; i++) {
        float* slot = l->audio + (size_t)l->apos * ch;
        float* frame = b + (size_t)i * ch;
        float limit = l->ceil_line[l->apos];
        for (c = 0; c < ch; c++) {
            float in = frame[c], out = slot[c] * ge;
            if (l->mix >= 1.0f) {
                if (out > limit) out = limit;
                else if (out < -limit) out = -limit;
            }
            slot[c] = in;
            frame[c] = out;
        }
        l->ceil_line[l->apos] = ceiling;
        if (++l->apos == l->delay) l->apos = 0;
    }
    /* The deque holds just the newest frame's 1; the box is all ones, and its sum
     * re-summed where limit_frame would have (exactly `attack`). */
    if (((l->n + n - 1) >> 16) != ((l->n - 1) >> 16)) l->box_sum = (double)l->attack;
    l->n += n;
    l->dq_head = 0;
    l->dq_count = 1;
    l->dq_idx[0] = l->n - 1;
    l->dq_val[0] = 1.0f;
    l->bpos = (int)(((uint32_t)l->bpos + n) % (uint32_t)l->attack);
}

void fa_limiter_process(fa_limiter* l, float* x, uint32_t count, float ceiling, int on)
{
    uint32_t i, at, ch = l->channels;
    float target = on ? 1.0f : 0.0f;
    float peaks[FA_TP_PUSH_BLOCK];
    if (!l->audio) return;
    if (l->mix < 0.0f) l->mix = target;
    /* True peaks a block at a time (the same values as frame by frame); a block
     * that cannot reach the ceiling needs no reduction anywhere. */
    for (at = 0; at < count; at += FA_TP_PUSH_BLOCK) {
        uint32_t n = count - at < FA_TP_PUSH_BLOCK ? count - at : FA_TP_PUSH_BLOCK;
        float* b = x + (size_t)at * ch;
        int measured = fa_truepeak_push_block(&l->tp, b, n, peaks, ceiling);
        if (!measured && l->quiet >= l->window + l->attack && l->mix == target && released(l)) {
            pass_block(l, b, n, ceiling);
            continue;
        }
        for (i = 0; i < n; i++) {
            float need = measured && peaks[i] > ceiling ? ceiling / peaks[i] : 1.0f;
            limit_frame(l, b + (size_t)i * ch, need, ceiling, target);
            if (need != 1.0f) l->quiet = 0;
            else if (l->quiet < l->window + l->attack) l->quiet++;
        }
    }
}
