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
#define FA_VARISPEED_HALF 48     /* sinc taps on each side at ratio <= 1 (resample.c) */
#define FA_VARISPEED_MAX_RATIO 80.0  /* widest kernel the history keeps (ratio = input/output) */

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
    const float* table; /* half the windowed-sinc kernel, VS_PHASES steps per tap (shared) */
    float* buf;         /* input frames, interleaved */
    int64_t cap, count;
    int64_t keep;       /* input frames kept behind the output time (fa_varispeed_set_keep) */
    double pos;         /* input time of the next output frame */
} fa_varispeed;

fa_result fa_varispeed_init(fa_varispeed* v, uint32_t channels);
/* History kept behind the output time: by default enough for the widest ratio
 * (FA_PARAM_RATE moves it); a resampler at one fixed ratio needs only its width. */
void fa_varispeed_set_keep(fa_varispeed* v, int64_t frames);
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
/* Input frames the kernel reaches past the output time at `ratio`: an output at
 * input time t needs frames up to floor(t) + width. */
int64_t fa_varispeed_width(double ratio);
/* A fixed ratio rate_in / rate_out = num / den read through precomputed kernels:
 * one row of 2 x width coefficients per output phase (output time i0 + k / den),
 * for inputs i0 - width + 1 .. i0 + width, oldest first. The same kernel as
 * fa_varispeed_read, worked out once instead of per output frame. Shared by every
 * resampler at that ratio; kept for the life of the process. */
typedef struct fa_vs_phases {
    uint32_t num, den;
    int64_t width;
    float* coef;
    struct fa_vs_phases* next;
} fa_vs_phases;
/* NULL when the ratio has too many phases (or no memory): use fa_varispeed_read. */
const fa_vs_phases* fa_varispeed_phases(uint32_t rate_in, uint32_t rate_out);
/* Like fa_varispeed_read at ratio num / den; `phase` (0 .. den - 1) is the
 * fraction of the output time the caller keeps between calls (with v->pos's
 * whole part). `out` NULL only moves the output time along. */
int64_t fa_varispeed_read_phased(fa_varispeed* v, const fa_vs_phases* p, uint32_t* phase, float* out, int64_t max_frames);
/* Input frames written but not yet reached by the output (fractional). */
double fa_varispeed_pending(const fa_varispeed* v);
/* History frames priming needs, and the lookahead to flush at the end. */
int64_t fa_varispeed_tail(void);

/* ---- The chain (tempo.c) -------------------------------------------------- */

#define FA_TR_HISTORY 8            /* transient mode: hops of energy an onset is compared with */
#define FA_TR_ONSETS 16            /* onsets waiting to be played (more are dropped) */

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

    /* Transient mode: the input around each onset replaces the stretcher's output
     * there, so hits keep their attack with no pre-echo (tempo.c). */
    int transients;
    int st_transients;          /* transients as the stretcher engaged (state below is for it) */
    float* raw;                 /* the stretcher's input, from source position raw_base */
    int64_t raw_n, raw_cap, raw_base;
    double hop_e;               /* onset detector: energy of the hop being summed */
    int hop_fill;
    float hop_hist[FA_TR_HISTORY];   /* the last hops' energies */
    int hop_count;
    struct {
        int64_t u;              /* source position of the onset */
        double v;               /* output frame (since engaging) it lands on; once fixed */
        int fixed;
    } onsets[FA_TR_ONSETS];     /* onsets not yet passed */
    int onset_n;
    int64_t last_onset;
    int64_t st_out;             /* stage-one output frames since engaging */
} fa_tempo;

fa_result fa_tempo_init(fa_tempo* t, uint32_t channels, uint32_t rate);
void fa_tempo_free(fa_tempo* t);
/* Back to pass-through, the next input frame being source position `src`. */
void fa_tempo_reset(fa_tempo* t, double src);
/* keep_formants applies at once; a new quality when the stretcher next engages
 * (after a reset). */
void fa_tempo_configure(fa_tempo* t, int fast, int keep_formants, int transients);
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
