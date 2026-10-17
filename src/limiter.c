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
 * ceiling passes unchanged. Runs on the audio thread: no locks, no allocation. */
#include "fa_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FA_LIMITER_ATTACK_MS 1.5
#define FA_LIMITER_RELEASE_MS 150.0
#define FA_LIMITER_TP_DELAY 8   /* >= the oversampler's group delay (about 6 frames) */

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
    l->box = (float*)malloc((size_t)l->attack * sizeof(float));
    l->dq_idx = (int64_t*)malloc((size_t)l->dq_cap * sizeof(int64_t));
    l->dq_val = (float*)malloc((size_t)l->dq_cap * sizeof(float));
    if (!l->audio || !l->box || !l->dq_idx || !l->dq_val || fa_truepeak_init(&l->tp, rate, channels) != FA_OK) {
        fa_limiter_free(l);
        return FA_ERROR_OUT_OF_MEMORY;
    }
    {
        int i;
        for (i = 0; i < l->attack; i++) l->box[i] = 1.0f;
    }
    l->box_sum = (double)l->attack;
    l->gain = 1.0f;
    l->reduction = 1.0f;
    l->release = (float)exp(-1.0 / (rate * FA_LIMITER_RELEASE_MS / 1000.0));
    return FA_OK;
}

void fa_limiter_free(fa_limiter* l)
{
    free(l->audio);
    free(l->box);
    free(l->dq_idx);
    free(l->dq_val);
    fa_truepeak_free(&l->tp);
    memset(l, 0, sizeof(*l));
}

void fa_limiter_process(fa_limiter* l, float* x, uint32_t count, float ceiling)
{
    uint32_t i, c, ch = l->channels;
    float least = 1.0f;
    if (!l->audio) return;
    for (i = 0; i < count; i++) {
        float* frame = x + (size_t)i * ch;
        float* slot;
        float need = 1.0f, gmin, box_avg, g;

        if (ceiling > 0.0f) {
            float p = fa_truepeak_push(&l->tp, frame);
            for (c = 0; c < ch; c++) { float a = fabsf(frame[c]); if (a > p) p = a; }
            if (p > ceiling) need = ceiling / p;
        }

        /* Sliding minimum over the last `window` frames (monotonic deque). */
        while (l->dq_count > 0 && l->dq_val[(l->dq_head + l->dq_count - 1) % l->dq_cap] >= need) l->dq_count--;
        l->dq_idx[(l->dq_head + l->dq_count) % l->dq_cap] = l->n;
        l->dq_val[(l->dq_head + l->dq_count) % l->dq_cap] = need;
        l->dq_count++;
        while (l->dq_idx[l->dq_head] <= l->n - l->window) { l->dq_head = (l->dq_head + 1) % l->dq_cap; l->dq_count--; }
        gmin = l->dq_val[l->dq_head];

        /* Box average over `attack` minima. */
        l->box_sum += (double)gmin - (double)l->box[l->bpos];
        l->box[l->bpos] = gmin;
        l->bpos = (l->bpos + 1) % l->attack;
        if ((l->n & 0xffff) == 0) {       /* re-sum now and then so rounding cannot drift */
            int k;
            l->box_sum = 0.0;
            for (k = 0; k < l->attack; k++) l->box_sum += l->box[k];
        }
        box_avg = (float)(l->box_sum / (double)l->attack);
        if (box_avg > 1.0f) box_avg = 1.0f;

        /* Instant into reduction, smooth release out of it. */
        g = box_avg < l->gain ? box_avg : box_avg + (l->gain - box_avg) * l->release;
        l->gain = g;
        if (g < least) least = g;

        /* Delay line of `delay` frames: the frame from `delay` frames ago goes out. */
        slot = l->audio + (size_t)l->apos * ch;
        for (c = 0; c < ch; c++) {
            float in = frame[c], out = slot[c] * g;
            if (ceiling > 0.0f) {          /* guard against float rounding at the edge */
                if (out > ceiling) out = ceiling;
                else if (out < -ceiling) out = -ceiling;
            }
            slot[c] = in;
            frame[c] = out;
        }
        l->apos = (l->apos + 1) % l->delay;
        l->n++;
    }
    l->reduction = least;
}
