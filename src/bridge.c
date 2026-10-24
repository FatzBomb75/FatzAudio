/* The bridge between two audio clocks (mixer.h): a ring written by one device's
 * callback and read by another's through the varispeed resampler (resample.c).
 *
 * The reader takes the input out at the ratio of the two rates, nudged by up to
 * max_adjust so that what the bridge holds stays at its target: drift between the two
 * crystals is absorbed without a click, and so is a different rate (a 44.1 kHz device
 * on a 48 kHz one). The target is one writer period, one reader period, the
 * resampler's look-ahead and a millisecond; an underrun raises it by 2 ms (up to
 * 50 ms), so a system with jittery callbacks settles at what it needs. On top of it the
 * bridge can hold a delay (`extra`): small changes of it are steered in, larger ones
 * fill up with silence or drop at once. */
#include "mixer.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define BRIDGE_SETTLE_S 2.0      /* a fill error is worked off over about this long */
#define BRIDGE_JUMP_MS 30.0      /* more over the target than this is dropped at once */
#define BRIDGE_STEP_MS 2.0       /* an underrun raises the target by this much */
#define BRIDGE_MAX_MS 50.0
#define BRIDGE_GLIDE_MS 5.0      /* a change of `extra` up to this is steered in, not jumped */
#define VS_PREALLOC 8192         /* resampler frames reserved up front (at least): no allocation later */

fa_result fa_bridge_init(fa_bridge* b, double max_adjust)
{
    memset(b, 0, sizeof(*b));
    b->max_adjust = max_adjust;
    return fa_varispeed_init(&b->vs, BRIDGE_CHANNELS);
}

void fa_bridge_free(fa_bridge* b)
{
    fa_varispeed_free(&b->vs);
    free(b->ring);
    b->ring = NULL;
}

fa_result fa_bridge_reserve(fa_bridge* b, uint32_t out_rate)
{
    int64_t prealloc;
    double most = (double)b->in_rate / (double)out_rate * (1.0 + b->max_adjust);
    b->out_rate = 0;                     /* the next pull sets the bridge up anew */
    /* One block's input at the steepest ratio, the look-ahead on both sides and what the
     * resampler keeps behind its position. */
    fa_varispeed_set_keep(&b->vs, 64);
    prealloc = (int64_t)ceil((double)FA_MIXER_BLOCK * most) + 2 * fa_varispeed_width(most) + 64 + 16;
    if (prealloc < VS_PREALLOC) prealloc = VS_PREALLOC;
    if (fa_varispeed_write(&b->vs, NULL, prealloc) != FA_OK) return FA_ERROR_OUT_OF_MEMORY;
    fa_varispeed_reset(&b->vs);
    return FA_OK;
}

fa_result fa_bridge_size(fa_bridge* b, uint32_t in_rate, uint32_t out_rate, double seconds)
{
    uint32_t frames;
    b->in_rate = in_rate;
    for (frames = 1024; frames < (uint32_t)((double)in_rate * seconds); frames *= 2) {}
    if (frames != b->ring_frames || !b->ring) {
        free(b->ring);
        b->ring_frames = frames;
        b->ring = (float*)calloc((size_t)frames * BRIDGE_CHANNELS, sizeof(float));
    }
    fa_store32(&b->wpos, 0);
    fa_store32(&b->rpos, 0);
    fa_store32(&b->reset, 0);
    if (!b->ring) return FA_ERROR_OUT_OF_MEMORY;
    return fa_bridge_reserve(b, out_rate);   /* the reader never allocates */
}

void fa_bridge_write(fa_bridge* b, const float* frames, uint32_t n)
{
    uint32_t w, i;
    n = fa_bridge_space(b, n, &w);
    for (i = 0; i < n; i++) {   /* at most two runs: up to the ring's end, then from its start */
        uint32_t at = (w + i) & (b->ring_frames - 1), run = b->ring_frames - at;
        if (run > n - i) run = n - i;
        memcpy(b->ring + (size_t)at * BRIDGE_CHANNELS, frames + (size_t)i * BRIDGE_CHANNELS,
               (size_t)run * BRIDGE_CHANNELS * sizeof(float));
        i += run - 1;
    }
    fa_bridge_commit(b, w, n);
}

double fa_bridge_fill(fa_bridge* b)
{
    return (double)((uint32_t)fa_load32(&b->wpos) - (uint32_t)fa_load32(&b->rpos)) + fa_varispeed_pending(&b->vs);
}

static void publish_held(fa_bridge* b)
{
    fa_store64(&b->held_ns, (int64_t)((b->base + b->grown) * 1e9 / (double)b->in_rate));
}

static void setup(fa_bridge* b, uint32_t out_rate, double in_period, double out_period)
{
    double width, inp = in_period, outp = out_period;
    b->out_rate = out_rate;
    b->in_period = in_period;
    b->out_period = out_period;
    b->nominal = (double)b->in_rate / (double)out_rate;
    width = (double)fa_varispeed_width(b->nominal * (1.0 + b->max_adjust)) + 2.0;
    if (inp <= 0.0) inp = (double)b->in_rate * 0.01;
    if (outp <= 0.0) outp = (double)out_rate * 0.01;
    b->base = inp + outp * b->nominal + width + (double)b->in_rate * 0.001;
    b->grown = 0.0;
    b->extra_now = fa_load32(&b->extra);
    b->target = b->base + (double)b->extra_now;
    b->primed = 0;
    fa_varispeed_reset(&b->vs);
    publish_held(b);
}

/* Drops `n` frames from the ring (the reader's side). */
static void ring_skip(fa_bridge* b, uint32_t n)
{
    fa_store32(&b->rpos, (int32_t)((uint32_t)fa_load32(&b->rpos) + n));
}

int64_t fa_bridge_pull(fa_bridge* b, float* out, uint32_t frames, uint32_t out_rate, double in_period,
                       double out_period, int hold)
{
    uint32_t w, r, avail, mask = b->ring_frames - 1;
    double fill, err, adj, ratio;
    int64_t need, got;
    int32_t extra;
    if (b->out_rate != out_rate || b->in_period != in_period || b->out_period != out_period)
        setup(b, out_rate, in_period, out_period);
    if (fa_load32(&b->reset) && fa_exchange32(&b->reset, 0)) {
        /* Start over: what is held goes, and the bridge fills up again. */
        fa_store32(&b->rpos, fa_load32(&b->wpos));
        fa_varispeed_reset(&b->vs);
        b->primed = 0;
    }
    extra = fa_load32(&b->extra);
    if (extra != b->extra_now) {
        /* A new delay: a small change is steered in, a larger one jumps (below). */
        double step = (double)(extra - b->extra_now);
        b->extra_now = extra;
        b->target += step;
        if (fabs(step) > (double)b->in_rate * BRIDGE_GLIDE_MS / 1000.0) b->primed = 0;
    }
    w = (uint32_t)fa_load32(&b->wpos);
    r = (uint32_t)fa_load32(&b->rpos);
    avail = w - r;
    fill = (double)avail + fa_varispeed_pending(&b->vs);
    if (!b->primed) {
        if (fill < b->target) {   /* filling up: silence until the target is held */
            memset(out, 0, (size_t)frames * BRIDGE_CHANNELS * sizeof(float));
            return 0;
        }
        if (fill > b->target) {   /* more than that (the writer ran while we did not): drop it */
            uint32_t drop = (uint32_t)(fill - b->target);
            if (drop > avail) drop = avail;
            ring_skip(b, drop);
            r += drop;
            avail -= drop;
            fill -= (double)drop;
        }
        b->primed = 1;
        b->avg = fill;
    }
    b->avg += (fill - b->avg) * fmin(1.0, (double)frames / (double)out_rate);
    err = b->avg - b->target;
    if (!hold && err > (double)b->in_rate * BRIDGE_JUMP_MS / 1000.0) {
        /* Far over (the reader stalled): back to the target now, not over seconds.
         * The average lags the fill, which may already be under the target. */
        uint32_t drop = fill > b->target ? (uint32_t)(fill - b->target) : 0;
        if (drop > avail) drop = avail;
        ring_skip(b, drop);
        r += drop;
        avail -= drop;
        fill -= (double)drop;
        b->avg = fill;
        err = fill - b->target;
    }
    adj = hold ? 0.0 : err / ((double)b->in_rate * BRIDGE_SETTLE_S);
    if (adj > b->max_adjust) adj = b->max_adjust;
    if (adj < -b->max_adjust) adj = -b->max_adjust;
    ratio = b->nominal * (1.0 + adj);
    /* Feed the resampler what this block reads, plus its look-ahead. */
    need = (int64_t)ceil((double)frames * ratio) + fa_varispeed_width(ratio) + 2 - (int64_t)fa_varispeed_pending(&b->vs);
    while (need > 0 && avail > 0) {
        uint32_t at = r & mask, chunk = avail;
        if (chunk > b->ring_frames - at) chunk = b->ring_frames - at;
        if ((int64_t)chunk > need) chunk = (uint32_t)need;
        if (fa_varispeed_write(&b->vs, b->ring + (size_t)at * BRIDGE_CHANNELS, chunk) != FA_OK) break;
        r += chunk;
        avail -= chunk;
        need -= chunk;
    }
    fa_store32(&b->rpos, (int32_t)r);
    got = fa_varispeed_read(&b->vs, ratio, out, frames);
    if (got < (int64_t)frames) {
        /* Ran dry: the rest is silence, and the bridge holds more from now on. */
        double most = (double)b->in_rate * BRIDGE_MAX_MS / 1000.0;
        memset(out + (size_t)got * BRIDGE_CHANNELS, 0, (size_t)(frames - got) * BRIDGE_CHANNELS * sizeof(float));
        fa_fetch_add32(&b->underruns, 1);
        if (b->grown + b->base < most) {
            double step = fmin((double)b->in_rate * BRIDGE_STEP_MS / 1000.0, most - b->grown - b->base);
            b->grown += step;
            b->target += step;
        }
        b->primed = 0;
        publish_held(b);
    }
    return got;
}
