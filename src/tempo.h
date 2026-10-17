/* Tempo, pitch and rate (FA_PARAM_TEMPO, FA_PARAM_PITCH, FA_PARAM_RATE).
 *
 * Plain C types only: stretch.cpp (C++) includes this too.
 *
 * The chain, run by the decode workers (or fa_stream_read) between the decoder and
 * the device-rate converter:
 *
 *   source frames -> stretcher -> varispeed -> converter
 *
 * With p = 2^(pitch / 12): the stretcher changes the duration by tempo / p (pitch
 * untouched), the varispeed resamples by p x rate, which moves the pitch by exactly
 * that and the speed back to tempo x rate. Keeping formants, the stretcher moves the
 * spectral envelope by 1 / p, so the resampling puts it back where it was.
 *
 * Each stage stays out of the way (bit-exact pass-through, no latency) until its
 * parameter first leaves its neutral value, and then stays in until the stream is
 * restarted (seek, stop, attach), so moving a slider through 1.0 does not click. */
#ifndef FA_TEMPO_H
#define FA_TEMPO_H

#include "fatzaudio/fatzaudio.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FA_TEMPO_PI 3.14159265358979323846
#define FA_TEMPO_MAX_CHANNELS 32

/* ---- Stretcher (stretch.cpp, Signalsmith Stretch) ------------------------ */

typedef struct fa_stretch fa_stretch;

/* NULL when out of memory. `fast`: the cheaper preset (shorter blocks). */
fa_stretch* fa_stretch_create(uint32_t channels, uint32_t rate, int fast);
void fa_stretch_destroy(fa_stretch* s);
/* Moves the spectral envelope (formants) by `factor`, frequencies staying where
 * they are; 1 = untouched. */
void fa_stretch_set_formants(fa_stretch* s, double factor);
/* Input frames fa_stretch_start needs at time ratio `ratio` (input/output). */
int fa_stretch_start_length(const fa_stretch* s, double ratio);
int fa_stretch_input_latency(const fa_stretch* s);
int fa_stretch_output_latency(const fa_stretch* s);
/* Starts the stretcher on `frames` input frames (fa_stretch_start_length), so the
 * output of the next fa_stretch_process begins exactly at the first of them. */
int fa_stretch_start(fa_stretch* s, const float* in, int frames);
int fa_stretch_process(fa_stretch* s, const float* in, int in_frames, float* out, int out_frames);
/* The output still held, with no more input (call after feeding input latency of silence). */
int fa_stretch_flush(fa_stretch* s, float* out, int frames, double ratio);

/* ---- Varispeed resampler (resample.c) ------------------------------------ */

typedef struct fa_varispeed {
    uint32_t channels;
    float* table;       /* half the windowed-sinc kernel, VS_PHASES steps per tap */
    float* buf;         /* input frames, interleaved */
    int64_t cap, count;
    double pos;         /* input time of the next output frame */
} fa_varispeed;

fa_result fa_varispeed_init(fa_varispeed* v, uint32_t channels);
void fa_varispeed_free(fa_varispeed* v);
void fa_varispeed_reset(fa_varispeed* v);
/* Starts over with `history` (the frames just before the next input) as the past:
 * the first output frame is the first frame written next, so engaging mid-stream
 * joins without a seam. */
fa_result fa_varispeed_prime(fa_varispeed* v, const float* history, int64_t frames);
/* Appends input; `in` NULL appends silence. */
fa_result fa_varispeed_write(fa_varispeed* v, const float* in, int64_t frames);
/* Output frames at `ratio` input frames per output frame, as far as the input
 * reaches (the kernel looks ahead). */
int64_t fa_varispeed_read(fa_varispeed* v, double ratio, float* out, int64_t max_frames);
/* Input frames written but not yet reached by the output (fractional). */
double fa_varispeed_pending(const fa_varispeed* v);
/* History frames priming needs, and the lookahead to flush at the end. */
int64_t fa_varispeed_tail(void);

/* ---- The chain (tempo.c) -------------------------------------------------- */

typedef struct fa_tempo {
    uint32_t channels, rate;
    int fast, keep_formants;

    /* Stretcher stage */
    fa_stretch* st;
    int st_fast;                /* the quality st was made with */
    int st_on, priming;
    float* prime;               /* input gathered before the stretcher can start */
    int64_t prime_n, prime_need, prime_cap;
    double prime_src;           /* source position of prime[0] */
    double frac;                /* output frames owed (fractional part) */
    double st_src;              /* source position of the stretcher's next output frame */
    int64_t xfade;              /* frames left of the crossfade from the dry signal */
    int64_t xfade_pos;
    double xfade_ratio;         /* source frames per output frame during the crossfade */
    float* sbuf;
    int64_t scap;

    /* Varispeed stage */
    fa_varispeed vs;
    int vs_ready, vs_on;
    float* tail;                /* the last stage-one frames (history for engaging) */
    int64_t tail_n;
    float* obuf;
    int64_t ocap;

    double in_src;              /* source position of the next input frame */
    double out_src;             /* source position of the next output frame */
    double last_tempo;          /* source frames per stage-one frame, last run */
    double formants;            /* the stretcher's formant factor, last run */
    int64_t produced;           /* output frames since the last reset */
} fa_tempo;

fa_result fa_tempo_init(fa_tempo* t, uint32_t channels, uint32_t rate);
void fa_tempo_free(fa_tempo* t);
/* Back to pass-through, the next input frame being source position `src`. */
void fa_tempo_reset(fa_tempo* t, double src);
/* keep_formants applies at once; a new quality when the stretcher next engages
 * (after a reset). */
void fa_tempo_configure(fa_tempo* t, int fast, int keep_formants);
/* Is any stage engaged? Until then fa_tempo_run hands back its input. */
int fa_tempo_engaged(const fa_tempo* t);
/* Runs `frames` input frames at tempo (multiplier), pitch (semitones) and rate
 * (multiplier). *out / *out_frames: the output (valid until the next call; it may
 * be `in` itself). *src0 / *src1: the source positions of the first output frame
 * and of the one after the last. */
fa_result fa_tempo_run(fa_tempo* t, const float* in, int64_t frames, double tempo, double pitch, double rate,
                       const float** out, int64_t* out_frames, double* src0, double* src1);
/* The end of the source: everything still held. Afterwards, reset before reuse. */
fa_result fa_tempo_drain(fa_tempo* t, double tempo, double pitch, double rate,
                         const float** out, int64_t* out_frames, double* src0, double* src1);
/* Source frames inside the chain: given to it but not yet output. */
double fa_tempo_held(const fa_tempo* t);

#ifdef __cplusplus
}
#endif

#endif /* FA_TEMPO_H */
