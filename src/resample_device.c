/* Device-rate conversion (a stream at 44.1 kHz on a 48 kHz device, ...) with the
 * windowed-sinc resampler of FA_PARAM_RATE (resample.c), plugged into
 * miniaudio's data converter as a custom resampling backend. One resampler for
 * everything, and cleaner than miniaudio's linear one: about -90 dB of aliasing
 * and imaging instead of audible images near the top of the band.
 *
 * Timing: output frame k is input time k * ratio exactly (no delay), but the
 * kernel looks ahead `width` input frames, which is the input latency the
 * stream feeds as silence at the end to get the last frames out. Ratios above
 * FA_VARISPEED_MAX_RATIO (a 768 kHz source on an 8 kHz device) are refused: the
 * stream then falls back to miniaudio's linear resampler. */
#include "fa_internal.h"
#include <math.h>
#include <stdlib.h>

typedef struct sinc_backend {
    fa_varispeed vs;
    double ratio;               /* input frames per output frame */
    int64_t width;              /* input frames the kernel reaches past the output time */
} sinc_backend;

static int set_ratio(sinc_backend* b, ma_uint32 rate_in, ma_uint32 rate_out)
{
    double ratio;
    if (rate_in == 0 || rate_out == 0) return 0;
    ratio = (double)rate_in / (double)rate_out;
    if (ratio > FA_VARISPEED_MAX_RATIO) return 0;   /* wider than the history keeps */
    b->ratio = ratio;
    b->width = fa_varispeed_width(ratio);
    return 1;
}

/* Outputs the input written so far reaches: k >= 0 with floor(pos + k * ratio) + width < count. */
static ma_uint64 outputs_for(const sinc_backend* b, double count)
{
    double room = count - (double)b->width - b->vs.pos;
    return room > 0 ? (ma_uint64)ceil(room / b->ratio) : 0;
}

/* Input still to write for `outputs` more frames. */
static ma_uint64 input_for(const sinc_backend* b, ma_uint64 outputs)
{
    double need;
    if (outputs == 0) return 0;
    need = floor(b->vs.pos + (double)(outputs - 1) * b->ratio) + (double)b->width + 1.0 - (double)b->vs.count;
    return need > 0 ? (ma_uint64)need : 0;
}

static ma_result sinc_heap_size(void* user, const ma_resampler_config* config, size_t* out)
{
    (void)user;
    (void)config;
    *out = 0;   /* allocates its own */
    return MA_SUCCESS;
}

static ma_result sinc_init(void* user, const ma_resampler_config* config, void* heap, ma_resampling_backend** out)
{
    sinc_backend* b;
    (void)user;
    (void)heap;
    if (config->format != ma_format_f32 || config->channels == 0 || config->channels > FA_TEMPO_MAX_CHANNELS)
        return MA_INVALID_ARGS;
    b = (sinc_backend*)calloc(1, sizeof(*b));
    if (!b) return MA_OUT_OF_MEMORY;
    if (!set_ratio(b, config->sampleRateIn, config->sampleRateOut)) {
        free(b);
        return MA_INVALID_ARGS;
    }
    if (fa_varispeed_init(&b->vs, config->channels) != FA_OK) {
        free(b);
        return MA_OUT_OF_MEMORY;
    }
    *out = b;
    return MA_SUCCESS;
}

static void sinc_uninit(void* user, ma_resampling_backend* backend, const ma_allocation_callbacks* alloc)
{
    sinc_backend* b = (sinc_backend*)backend;
    (void)user;
    (void)alloc;
    if (!b) return;
    fa_varispeed_free(&b->vs);
    free(b);
}

static ma_result sinc_process(void* user, ma_resampling_backend* backend, const void* in, ma_uint64* in_count,
                              void* out, ma_uint64* out_count)
{
    sinc_backend* b = (sinc_backend*)backend;
    ma_uint64 take, made = 0;
    (void)user;
    if (!in_count || !out_count) return MA_INVALID_ARGS;
    /* Take only the input these outputs need, so nothing piles up (as latency)
     * when the output side is the short one. */
    take = input_for(b, *out_count);
    if (take > *in_count) take = *in_count;
    if (take > 0 && fa_varispeed_write(&b->vs, (const float*)in, (int64_t)take) != FA_OK) return MA_OUT_OF_MEMORY;
    if (out) {
        made = (ma_uint64)fa_varispeed_read(&b->vs, b->ratio, (float*)out, (int64_t)*out_count);
    } else {
        /* Nobody wants the samples: only move the output time along. */
        while (made < *out_count && (int64_t)floor(b->vs.pos) + b->width < b->vs.count) {
            b->vs.pos += b->ratio;
            made++;
        }
    }
    *in_count = take;
    *out_count = made;
    return MA_SUCCESS;
}

static ma_result sinc_set_rate(void* user, ma_resampling_backend* backend, ma_uint32 rate_in, ma_uint32 rate_out)
{
    (void)user;
    return set_ratio((sinc_backend*)backend, rate_in, rate_out) ? MA_SUCCESS : MA_INVALID_ARGS;
}

/* The silence to feed at the end to get the last frames out. */
static ma_uint64 sinc_input_latency(void* user, const ma_resampling_backend* backend)
{
    (void)user;
    return (ma_uint64)((const sinc_backend*)backend)->width;
}

/* What it holds now, in output frames: the input written past the output time. */
static ma_uint64 sinc_output_latency(void* user, const ma_resampling_backend* backend)
{
    const sinc_backend* b = (const sinc_backend*)backend;
    double held = fa_varispeed_pending(&b->vs);
    (void)user;
    return held > 0 ? (ma_uint64)(held / b->ratio + 0.5) : 0;
}

static ma_result sinc_required_input(void* user, const ma_resampling_backend* backend, ma_uint64 out_frames,
                                     ma_uint64* in_frames)
{
    (void)user;
    *in_frames = input_for((const sinc_backend*)backend, out_frames);
    return MA_SUCCESS;
}

static ma_result sinc_expected_output(void* user, const ma_resampling_backend* backend, ma_uint64 in_frames,
                                      ma_uint64* out_frames)
{
    const sinc_backend* b = (const sinc_backend*)backend;
    (void)user;
    *out_frames = outputs_for(b, (double)b->vs.count + (double)in_frames);
    return MA_SUCCESS;
}

static ma_result sinc_reset(void* user, ma_resampling_backend* backend)
{
    (void)user;
    fa_varispeed_reset(&((sinc_backend*)backend)->vs);
    return MA_SUCCESS;
}

ma_resampling_backend_vtable fa_sinc_resampler = {
    sinc_heap_size, sinc_init, sinc_uninit, sinc_process, sinc_set_rate, sinc_input_latency,
    sinc_output_latency, sinc_required_input, sinc_expected_output, sinc_reset
};
