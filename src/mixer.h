/* The DJ mixer's internals, shared by mixer.c (decks, crossfader, master, cue) and
 * mic.c (microphones and their input bridges). */
#ifndef FA_MIXER_H
#define FA_MIXER_H
#include "fa_internal.h"
#include "tempo.h"

#define MIXER_MAX_DECKS 2
#define MIXER_DEZIPPER_NS 5000000
#define MIXER_SUB 16             /* gains are exact every 16 frames, linear in between */
#define ISO_LOW_HZ 250.0
#define ISO_HIGH_HZ 2500.0
#define ISO_SECTIONS 9
#define DECK_TEMPO_LATENCY_MS 60

static const double k_pi = 3.14159265358979323846;

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

enum { DL_GAIN = 0, DL_TRIM, DL_LOW, DL_MID, DL_HIGH, DL_CUE, DECK_LANES };
enum { ML_XF = 0, ML_MASTER, ML_CUE_MIX, ML_CUE_GAIN, MIX_LANES };

typedef struct biquad {
    float b0, b1, b2, a1, a2;
} biquad;

/* One sample through a section (transposed direct form II). */
static inline float fa_biquad_run(const biquad* q, float* z, float x)
{
    float y = q->b0 * x + z[0];
    z[0] = q->b1 * x - q->a1 * y + z[1];
    z[1] = q->b2 * x - q->a2 * y;
    return y;
}

struct fa_deck {
    struct fa_mixer* mixer;
    uint32_t index;
    fa_bus* bus;                 /* its streams mix into bus->buf */
    fa_atomic_ptr stream;        /* fa_stream*; written under the mixer lock, or cleared when it leaves */
    fa_frames cue_point;         /* mixer lock; FA_FRAMES_UNSET = the stream's CUE_IN */
    fa_lane lanes[DECK_LANES];
    fa_atomic32 side;            /* FA_DECK_XFADER_SIDE */
    fa_atomic_ptr tap;           /* fa_tap*, the channel meter (post fader) */
    /* audio thread */
    float side_w[3];             /* thru, A, B: glide between sides over 5 ms */
    float z[ISO_SECTIONS][2][2]; /* isolator filter memory, per section and channel */
};

struct fa_mixer {
    fa_engine* engine;
    fa_device* device;
    fa_mutex lock;               /* API calls */
    uint32_t deck_count;
    struct fa_deck decks[MIXER_MAX_DECKS];
    fa_lane lanes[MIX_LANES];
    fa_atomic32 curve;           /* fa_xfader_curve */
    fa_cue_mode cue_mode;
    uint32_t cue_channel;
    /* The isolator: LR4 low/high at 250 Hz (2 + 2 sections), LR4 low/high at 2.5 kHz on
     * the upper band (2 + 2), and an allpass at 2.5 kHz on the low band so the three
     * sum flat. */
    biquad lp1, hp1, lp2, hp2, ap2;
    /* audio thread */
    float master[FA_MIXER_BLOCK * FA_DEVICE_CHANNELS];
    float cue[FA_MIXER_BLOCK * FA_DEVICE_CHANNELS];   /* the cue sends, scaled by (1 - mix) x gain */
    float cue_master[FA_MIXER_BLOCK];                 /* mix x gain: the master's share of the cue */
    /* Mics (mic.c): slots published to the audio thread, which runs them each block. */
    fa_atomic_ptr mics[FA_MIC_MAX];                   /* struct fa_mic*; changed under the lock */
    struct fa_mic* removed[FA_MIC_MAX];               /* fa_mixer_destroy's mics, freed with the mixer
                                                         (a running callback may still hold one) */
    struct fa_capture* captures;                      /* the open inputs (lock) */
    uint64_t block;                                   /* audio thread: fa_mixer_process calls */
    /* Events (fa_mixer_set_event_callback), run by the event thread (fa_mixer_service). */
    fa_mutex cb_lock;
    fa_mixer_event_callback callback;
    void* callback_user;
    fa_atomic32 busy;                                 /* the event thread is in fa_mixer_service */
    fa_atomic32 doomed;                               /* destroyed from inside its own callback */
};


/* ---- parameter lanes on the device clock (audio thread) ---- */

static inline void take_lanes(fa_lane* lanes, int n, uint32_t rate)
{
    int i;
    for (i = 0; i < n; i++) fa_lane_take(&lanes[i], 1, rate, NULL);
}

static inline void end_lanes(fa_lane* lanes, int n, int64_t clock, int64_t frames)
{
    int i;
    for (i = 0; i < n; i++) {
        fa_lane_advance(&lanes[i].v, clock, frames);
        fa_store_float(&lanes[i].current, lanes[i].v.value);
    }
}

/* Values of `lanes` at the start and the end of [at, at + n). */
static inline void lane_span(const fa_lane* lanes, int n, int64_t clock, int64_t at, int64_t len, float* v0, float* v1)
{
    int i;
    for (i = 0; i < n; i++) {
        v0[i] = fa_lane_value_at_frame(&lanes[i].v, clock, at, 1);
        v1[i] = fa_lane_value_at_frame(&lanes[i].v, clock, at + len, 0);
    }
}

/* ---- mics (mic.c) ---------------------------------------------------------------
 * An input device, shared by the mics that read it. Its callback copies the two
 * channels the mics want ("lanes") into a ring; the master callback takes them out
 * through a resampler whose ratio follows the ring's fill, so the bridge holds a
 * steady few milliseconds whatever the two clocks' drift and rates. */
#define CAPTURE_LANES 2
#define CAPTURE_LANE_ALL (-1)    /* every channel summed */
#define CAPTURE_LANE_FREE (-2)

typedef struct fa_capture {
    struct fa_capture* next;     /* the mixer's captures (mixer lock) */
    struct fa_mixer* mixer;
    char* id;                    /* as asked for; "" = the default input */
    ma_device device;
    int device_ok;
    int users;                   /* mics on it (mixer lock) */
    int lane_users[CAPTURE_LANES];  /* mics reading each lane (mixer lock) */
    uint32_t channels, rate;     /* the input as opened */
    fa_atomic32 lane_src[CAPTURE_LANES];  /* the channel each lane carries, or CAPTURE_LANE_* */
    /* The ring: CAPTURE_LANES floats per frame, written by the input callback, read by
     * the master callback. Positions count frames and wrap. */
    float* ring;
    uint32_t ring_frames;        /* a power of two */
    fa_atomic32 wpos, rpos;
    fa_atomic32 stopping;        /* fa_mic_remove or a test is stopping it: not a loss */
    fa_atomic32 lost;            /* 1 = gone (each mic reports it once: lost_reported) */
    fa_atomic32 overruns, underruns;
    fa_atomic64 input_ns;        /* the input buffer the OS reported */
    fa_atomic64 bridge_ns;       /* what the bridge holds on average (its target) */
    /* master callback */
    fa_varispeed vs;
    uint64_t pulled;             /* fa_mixer.block of the last pull, + 1 */
    int primed;
    uint32_t out_rate;
    double nominal;              /* input rate / output rate */
    double target;               /* input frames held: ring plus resampler */
    double avg;                  /* the fill, smoothed over about a second */
    float out[FA_MIXER_BLOCK * CAPTURE_LANES];
} fa_capture;

enum { MIC_GAIN = 0, MIC_MUTE, MIC_PAN, MIC_MONITOR, MIC_LOW_CUT, MIC_LANES };

struct fa_mic {
    float buf[FA_MIXER_BLOCK * FA_DEVICE_CHANNELS];   /* first: aligned as the EQ's vector loads want */
    struct fa_mixer* mixer;
    uint32_t index;              /* its slot */
    fa_capture* cap;
    int lane;
    uint32_t flags;
    int lost_reported;           /* FA_EVENT_INPUT_LOST raised (mixer lock) */
    fa_lane lanes[MIC_LANES];
    fa_atomic32 gate_db;         /* float bits */
    fa_eq_shared eq;
    fa_atomic_ptr tap;           /* fa_tap*, the meter */
    /* audio thread */
    fa_eq_state eq_st;
    biquad hp;                   /* the low cut */
    float hp_z[2];
    float env;                   /* the gate's level follower */
    float gate;                  /* the gate's gain, 0..1 */
    int64_t gate_hold;           /* frames the gate stays open below its close level */
    int64_t warm;                /* frames of silence before it fades in */
    float fade;                  /* the fade-in, 0..1 */
};

/* mixer.c: wait until no mixer callback runs (returns at once on the event thread). */
void fa_mixer_wait_callbacks(struct fa_mixer* m);
/* After fa_mixer_service: lets go of fa_mixer_hold (frees a mixer its callback destroyed). */
void fa_mixer_release(struct fa_mixer* m);

/* mic.c, called by mixer.c. */
void fa_mixer_mics_process(struct fa_mixer* m, fa_device* dev, float* out, uint32_t frames);   /* audio thread */
void fa_mixer_mics_free(struct fa_mixer* m);       /* fa_mixer_destroy: removes every mic */
void fa_mixer_mics_free_removed(struct fa_mixer* m);   /* free_mixer: frees what fa_mixer_mics_free removed */
/* RBJ cookbook sections at Q 1/sqrt(2): 0 low-pass, 1 high-pass, 2 allpass. */
biquad fa_mixer_design(int type, double hz, double rate);

/* Tests: drive a mic's input by hand. take_over stops the input device and makes the
 * bridge expect `rate` and `channels`; feed is what the input callback does; lose is
 * what the backend's notification does when the device goes away. */
void fa_mic_test_take_over(fa_mic* mic, uint32_t rate, uint32_t channels);
void fa_mic_test_feed(fa_mic* mic, const float* frames, uint32_t n);
void fa_mic_test_lose(fa_mic* mic);
void fa_mic_test_stats(fa_mic* mic, double* out_target, double* out_fill, int32_t* out_underruns);

#endif
