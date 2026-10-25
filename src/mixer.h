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
static inline float clip1(float v) { return v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v; }

enum { DL_GAIN = 0, DL_TRIM, DL_LOW, DL_MID, DL_HIGH, DL_CUE, DECK_LANES };
enum { ML_XF = 0, ML_MASTER, ML_CUE_MIX, ML_CUE_GAIN, MIX_LANES };
enum { MFX_REVERB_SIZE = 0, MFX_REVERB_DAMPING, MFX_ECHO_MS, MFX_ECHO_FEEDBACK, MFX_ECHO_TONE, MFX_COUNT };

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
    fa_atomic32 sync_range;      /* FA_DECK_SYNC_RANGE, float bits */
    /* Sync (sync.c), mixer lock: leader NULL = not synced. */
    struct fa_deck* leader;
    uint32_t sync_flags;         /* fa_sync_flags */
    double sync_factor;          /* the leader's beats per deck beat: 1, 2 (half time) or 0.5 */
    double sync_base;            /* the TEMPO sync wants, before a phase bend */
    float sync_tempo;            /* the TEMPO it set last */
    fa_stream* sync_s;           /* the deck's and the leader's streams then: a new load ends it */
    fa_stream* sync_ls;
    int settling;                /* a one-off phase pull runs (BEAT without LOCK) */
    int settled;                 /* ticks in a row within 1 ms */
    int64_t settle_end;          /* monotonic ns: the pull gives up */
    int64_t hold_until;          /* monotonic ns: not measured before a jump is heard */
    int jumping;                 /* a jump waits for its beat to be decoded */
    fa_frames jump_at, jump_by;
    fa_tempo_segment* segs;      /* the grid's tempo map, read into here (sync.c) */
    uint32_t segs_cap;
    /* audio thread */
    float side_w[3];             /* thru, A, B: glide between sides over 5 ms */
    float z[ISO_SECTIONS][2][2]; /* isolator filter memory, per section and channel */
};

/* Auto-mix (automix.c), mixer lock. */
enum { AM_WATCH = 0, AM_SCHEDULED, AM_RUNNING };
typedef struct fa_automix {
    fa_automix_options o;
    int on;
    int phase;                   /* AM_*: watching the playing deck, or a transition is on */
    int skip;                    /* fa_mixer_automix_now: start the next one now */
    uint32_t out_deck, in_deck;
    fa_stream* out_s;            /* the transition's streams (a new load cancels its events) */
    fa_stream* in_s;
    int64_t start, end;          /* device frames: the incoming deck is heard, the fade is done */
    int aligned;                 /* beat-aligned: the incoming deck is locked meanwhile */
    int swap;                    /* BASS_SWAP posted: both decks' lows are auto-mix's until done */
    float low_in, low_out;       /* their FA_DECK_EQ_LOW_DB before */
    int restore;                 /* RESTORE_TEMPO: the incoming deck glides back to restore_to when done */
    float restore_to;
    fa_time restore_ns;          /* restore_time as committed */
    fa_stream* mixed_out;        /* the last outgoing song: the idle deck still holding it is empty */
    fa_stream* warned;           /* the outgoing song DECK_EMPTY was raised for before its mix */
} fa_automix;

/* The master's alignment delay (master callback): a delay line after the limiter; a
 * change fades out, jumps and fades in. */
typedef struct fa_master_delay {
    float* buf;                  /* FA_DEVICE_CHANNELS floats per frame */
    uint32_t len;                /* a power of two */
    uint32_t pos;
    int64_t cur, want;           /* frames */
    float gain;
} fa_master_delay;

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
    uint32_t flags;              /* FA_MIXER_* */
    struct fa_cue_dev* cue_dev;  /* FA_CUE_DEVICE */
    fa_master_delay mdelay;      /* FA_CUE_DEVICE: the master's alignment (audio thread) */
    fa_atomic32 cue_trim;        /* FA_MIX_CUE_TRIM_MS, float bits */
    fa_atomic64 align_ns;        /* published by the audio thread: fa_mixer_latency.cue_alignment */
    fa_atomic32 align_kind;      /* fa_cue_align */
    int64_t align_now;           /* audio thread: frames, + the cue delayed, - the master */
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
    fa_atomic_ptr bus;                                /* struct fa_mic_bus*, made with the first mic (lock) */
    fa_atomic32 fx[MFX_COUNT];                        /* fa_mic_effects, float bits */
    fa_atomic32 duck_depth, duck_threshold;           /* fa_ducking, float bits */
    fa_atomic64 duck_attack, duck_hold, duck_release; /* ns */
    fa_atomic_ptr encoders;                           /* fa_enc_list* on the taps (encode.c) */
    fa_atomic_ptr check;                              /* struct fa_mic_check*: a setup check pass */
    fa_atomic32 checking;                             /* fa_mixer_measure_latency runs */
    struct fa_mic_check* check_now;                   /* audio thread: `check` for this block */
    uint64_t block;                                   /* audio thread: fa_mixer_process calls */
    /* Events (fa_mixer_set_event_callback), run by the event thread (fa_mixer_service). */
    fa_mutex cb_lock;
    fa_mixer_event_callback callback;
    void* callback_user;
    fa_automix am;                                    /* automix.c, mixer lock */
    int64_t test_now;                                 /* tests: the time sync reads; 0 = the monotonic clock */
    int test_ticks;                                   /* tests: sync and auto-mix ticks run by hand */
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

/* ---- the bridge between two clocks (bridge.c) --------------------------------------
 * One device's callback writes a ring; another device's callback reads it through the
 * varispeed resampler at the ratio of their rates, nudged so that what the bridge holds
 * stays at its target: drift between two crystals and different rates are absorbed
 * without a click. Mic inputs reach the master through one (the master reads); the
 * second-device cue leaves it through another (the master writes). Positions count
 * frames and wrap; nothing allocates or locks once it is sized. */
#define BRIDGE_CHANNELS 2

typedef struct fa_bridge {
    float* ring;                 /* BRIDGE_CHANNELS floats per frame */
    uint32_t ring_frames;        /* a power of two */
    fa_atomic32 wpos, rpos;
    fa_atomic32 overruns, underruns;
    fa_atomic64 held_ns;         /* what it holds on average (its target, less `extra`) */
    fa_atomic32 extra;           /* input frames held on top of the target: a delay (the cue's
                                    alignment), set by any thread, taken by the reader */
    fa_atomic32 reset;           /* the reader drops what is held and fills up again */
    double max_adjust;           /* the most the ratio is nudged */
    /* reader */
    fa_varispeed vs;
    uint32_t in_rate, out_rate;  /* in_rate set by the owner while no reader runs */
    double in_period, out_period;
    int primed;
    int32_t extra_now;           /* `extra` in the target */
    double nominal;              /* in_rate / out_rate */
    double base;                 /* the periods, the resampler's look-ahead and 1 ms */
    double grown;                /* added by underruns, up to BRIDGE_MAX_MS */
    double target;               /* base + grown + extra_now */
    double avg;                  /* the fill, smoothed over about a second */
} fa_bridge;

fa_result fa_bridge_init(fa_bridge* b, double max_adjust);
void fa_bridge_free(fa_bridge* b);
/* Owner, while no reader runs: a ring of at least `seconds` at `in_rate`, and the
 * resampler's buffer for reads of up to FA_MIXER_BLOCK frames at `out_rate`. */
fa_result fa_bridge_size(fa_bridge* b, uint32_t in_rate, uint32_t out_rate, double seconds);
/* Owner, while no reader runs (a writer may): the resampler's buffer for another out_rate. */
fa_result fa_bridge_reserve(fa_bridge* b, uint32_t out_rate);
/* Writer: room for up to `n` frames from position *out_w (fewer when full: an overrun). */
static inline uint32_t fa_bridge_space(fa_bridge* b, uint32_t n, uint32_t* out_w)
{
    uint32_t w = (uint32_t)fa_load32(&b->wpos), r = (uint32_t)fa_load32(&b->rpos), space = b->ring_frames - (w - r);
    if (n > space) {
        fa_fetch_add32(&b->overruns, 1);
        n = space;
    }
    *out_w = w;
    return n;
}
static inline float* fa_bridge_frame(fa_bridge* b, uint32_t pos)
{
    return b->ring + (size_t)(pos & (b->ring_frames - 1)) * BRIDGE_CHANNELS;
}
static inline void fa_bridge_commit(fa_bridge* b, uint32_t w, uint32_t n) { fa_store32(&b->wpos, (int32_t)(w + n)); }
void fa_bridge_write(fa_bridge* b, const float* frames, uint32_t n);
/* Reader: `frames` (at most FA_MIXER_BLOCK) at out_rate into `out`; what it can't give is
 * silence. The periods are the writer's (input frames) and the reader's (output frames),
 * 0 = unknown. `hold` keeps the ratio at the nominal one (a setup check timing the input:
 * a drift correction would bend the delay it measures). Returns the frames given. */
int64_t fa_bridge_pull(fa_bridge* b, float* out, uint32_t frames, uint32_t out_rate, double in_period,
                       double out_period, int hold);
/* Reader side or owner: input frames held now (ring plus resampler). */
double fa_bridge_fill(fa_bridge* b);

/* ---- mics (mic.c) ---------------------------------------------------------------
 * An input device, shared by the mics that read it. Its callback copies the two
 * channels the mics want ("lanes") into its bridge; the master callback takes them out. */
#define CAPTURE_LANES BRIDGE_CHANNELS
#define CAPTURE_LANE_ALL (-1)    /* every channel summed */
#define CAPTURE_LANE_FREE (-2)

typedef struct fa_capture {
    struct fa_capture* next;     /* the mixer's captures (mixer lock) */
    struct fa_mixer* mixer;
    char* id;                    /* as asked for; "" = the default input */
    ma_device device;
    int device_ok;
    int users;                   /* mics on it (mixer lock) */
    int os_processing;           /* FA_MIC_OS_PROCESSING of the mic that opened it */
    int test_owned;              /* fa_mic_test_take_over: the test is its input */
    int mode_lost;               /* lost by the latency switch in progress (mixer lock): opened
                                    again if the switch falls back to the old mode */
    int lane_users[CAPTURE_LANES];  /* mics reading each lane (mixer lock) */
    uint32_t channels, rate;     /* the input as opened */
    fa_atomic32 lane_src[CAPTURE_LANES];  /* the channel each lane carries, or CAPTURE_LANE_* */
    fa_bridge br;                /* written by the input callback, read by the master callback */
    fa_atomic32 stopping;        /* fa_mic_remove or a test is stopping it: not a loss */
    fa_atomic32 lost;            /* 1 = gone (each mic reports it once: lost_reported) */
    fa_atomic64 input_ns;        /* the input buffer the OS reported */
    /* master callback */
    uint64_t pulled;             /* fa_mixer.block of the last pull, + 1 */
    float out[FA_MIXER_BLOCK * CAPTURE_LANES];
} fa_capture;

/* ---- the second-device cue (cue.c) -------------------------------------------------
 * The master callback lays the cue out into a bridge; the headphones' callback reads it.
 * Made once with the mixer (FA_CUE_DEVICE) and kept: only the device opens and closes. */
typedef struct fa_cue_dev {
    struct fa_mixer* mixer;
    ma_device device;
    int device_ok;               /* mixer lock */
    int test_owned;              /* fa_mixer_test_cue_take_over: the test reads it */
    int lost_reported;           /* FA_EVENT_CUE_LOST raised (mixer lock) */
    char* id;                    /* as asked for; "" = the default output (mixer lock) */
    fa_bridge br;
    fa_atomic32 stopping;        /* closing it: not a loss */
    fa_atomic32 lost;
    fa_atomic32 running;         /* the headphones are open: the master lines up with them */
    fa_atomic32 rate, period;    /* the headphones as opened (frames, at their rate) */
    fa_atomic64 os_ns;           /* their buffer, as the OS reports it */
    float cue[FA_MIXER_BLOCK * FA_DEVICE_CHANNELS];   /* master callback: this block's cue */
} fa_cue_dev;

enum { MIC_GAIN = 0, MIC_MUTE, MIC_PAN, MIC_MONITOR, MIC_LOW_CUT, MIC_REVERB, MIC_ECHO, MIC_LANES };

/* What the mics share (mic_fx.c): the bus they add into, the effects and the ducker.
 * Made with the mixer's first mic, at the device's rate, and kept until the mixer goes,
 * so the effects' tails ring out after a mic is removed. Audio thread only. */
#define ECHO_MAX_MS 1000.0
typedef struct fa_mic_bus {
    float vox[FA_MIXER_BLOCK * FA_DEVICE_CHANNELS];    /* into the master: heard mics and the returns */
    float dry[FA_MIXER_BLOCK * FA_DEVICE_CHANNELS];    /* mics with monitor off (the vocals tap only) */
    float rsend[FA_MIXER_BLOCK * FA_DEVICE_CHANNELS];  /* into the reverb */
    float esend[FA_MIXER_BLOCK];                       /* into the echo */
    float duck[FA_MIXER_BLOCK];                        /* the hosts' voice, then the ducker's dB */
    uint32_t rate;
    int hosts;                   /* host mics this block */
    fa_midi_fx* reverb;
    float* echo;                 /* the echo's delay line */
    uint32_t echo_len, echo_pos;
    double echo_delay;           /* frames, gliding to the set time */
    float echo_fb, echo_lp;      /* the feedback (gliding) and its tone filter's state */
    int echo_idle;
    int64_t echo_quiet;          /* frames since its input was non-zero or its output audible */
    float duck_env, duck_db;     /* the hosts' level follower; the music's gain now */
    int64_t duck_hold;
} fa_mic_bus;

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
    fa_atomic64 measured[3];     /* mic to speaker per fa_latency_mode, from the setup check; 0 = not measured */
};

/* A pass of the setup check (mic_check.c): `signal` plays after the limiter, from
 * `fade` frames after the block it begins on (t0), while `mic`'s input is recorded into
 * `rec`; the music fades out before and back after. */
#define FA_CHECK_UNSET INT64_MIN
typedef struct fa_mic_check {
    struct fa_mic* mic;
    const float* signal;
    int64_t signal_len;
    float* rec;
    int64_t rec_len;
    int64_t fade;
    int64_t t0;                  /* audio thread */
    fa_atomic32 done;            /* the music is back: the pass is over */
} fa_mic_check;
/* Audio thread: the mic's bridged input (lane `lane` of CAPTURE_LANES), and the master
 * after the limiter. */
void fa_mic_check_record(fa_mic_check* k, const float* in, int lane, uint32_t frames, int64_t clock);
void fa_mic_check_render(fa_mic_check* k, float* master, uint32_t frames, int64_t clock);

/* cue.c */
fa_result fa_mixer_cue_create(struct fa_mixer* m, const char* id);   /* fa_mixer_create */
void fa_mixer_cue_free(struct fa_mixer* m);                           /* the device closes */
void fa_mixer_cue_reopen(struct fa_mixer* m, fa_latency_mode mode);  /* master closed, mixer lock */
/* Master callback, FA_CUE_DEVICE: the cue (stereo; the device mute glides from mute0 to
 * mute1 over the block) into the bridge, the alignment, and the master's delay on
 * `master` in place. */
void fa_mixer_cue_render(struct fa_mixer* m, fa_device* dev, float* master, uint32_t frames, float mute0,
                         float mute1);
/* fa_mixer_get_latency: the cue's latency, the alignment, its kind. */
void fa_mixer_cue_latency(struct fa_mixer* m, fa_time* cue, fa_time* alignment, int32_t* kind);
/* fa_mixer_service, mixer lock: whether a loss is to be reported (once). */
int fa_mixer_cue_take_loss(struct fa_mixer* m);

/* mixer.c: wait until no mixer callback runs (returns at once on the event thread). */
void fa_mixer_wait_callbacks(struct fa_mixer* m);
/* After fa_mixer_service: lets go of fa_mixer_hold (frees a mixer its callback destroyed). */
void fa_mixer_release(struct fa_mixer* m);

/* ---- sync and auto-mix (sync.c, automix.c), event thread and API, mixer lock ---- */
#define MIXER_TICK_NS 50000000   /* sync locks and auto-mix look 20 times a second */
#define MIXER_EVENTS_MAX (FA_MIC_MAX + 8)
/* A deck's grid as read from its stream, in grid frames (scale = grid frames per source frame). */
typedef struct fa_grid_view {
    fa_beat_grid g;
    const fa_tempo_segment* segs;
    uint32_t n;
    double scale;
} fa_grid_view;
/* What a deck plays, as heard at monotonic time `now`. */
typedef struct fa_deck_now {
    fa_stream* s;
    fa_grid_view v;
    double sr;                   /* the stream's rate */
    double src;                  /* heard source frame */
    double tempo, rate;          /* FA_PARAM_TEMPO and FA_PARAM_RATE as set */
    double beat;                 /* heard position in beats */
    double grid_bpm;             /* the grid's tempo there */
    double bpm;                  /* heard: grid_bpm x tempo x rate */
    int32_t state;               /* fa_stream_state */
} fa_deck_now;
int64_t fa_mixer_now(struct fa_mixer* m);
/* The event thread looks at the mixer again at monotonic `at` (the sooner one wins). */
void fa_mixer_wake(struct fa_mixer* m, int64_t at);
/* FA_ERROR_NOT_FOUND for an empty deck or one without a grid. */
fa_result fa_deck_look(struct fa_deck* d, int64_t now, fa_deck_now* out);
double fa_grid_beat_at(const fa_grid_view* v, double frame);   /* grid frames */
double fa_grid_frame_at(const fa_grid_view* v, double beat);
/* The first downbeat at or after `beat` (bars count from bar_offset). */
double fa_grid_next_downbeat(const fa_grid_view* v, double beat);
/* The TEMPO that makes a deck of `grid_bpm` (at `rate`) heard at `bpm`, or at half or
 * double time when only that is within `range` of 1; 0 when none is. *out_factor = the
 * target's beats per deck beat. */
double fa_sync_tempo_for(double bpm, double grid_bpm, double rate, double range, double* out_factor);
/* Locks `d` to `leader` from now (TEMPO already set to `tempo`): no measuring before `hold_until`. */
void fa_deck_sync_set(struct fa_deck* d, struct fa_deck* leader, uint32_t flags, double factor, double tempo,
                      int64_t hold_until);
/* Ends a deck's sync; restore = puts back the TEMPO a phase bend moved. */
void fa_deck_sync_end(struct fa_deck* d, int restore);
/* fa_deck_load: syncs with `d` end (its stream is another now). */
void fa_deck_sync_loaded(struct fa_deck* d);
/* The cue point in force of a deck with a stream (mixer.c). */
fa_frames fa_deck_cue_point_now(struct fa_deck* d);
/* Event thread (fa_mixer_service) or a test: one look at the syncs and auto-mix; events go to ev[*n]. */
void fa_mixer_tick(struct fa_mixer* m, int64_t now, fa_mixer_event* ev, uint32_t* n);
void fa_automix_tick(struct fa_mixer* m, int64_t now, fa_mixer_event* ev, uint32_t* n, int64_t* next);
/* fa_deck_load: `s` (may be NULL) is a new song on deck `d` to auto-mix, even at an old
 * address; a transition with `d` in it is the app's now. */
void fa_automix_loaded(struct fa_mixer* m, struct fa_deck* d, fa_stream* s);
/* Tests: tick by hand at `now` (the event thread leaves the mixer's ticks alone from then
 * on), delivering the events to the callback before it returns. */
void fa_mixer_test_tick(fa_mixer* m, int64_t now);

/* mic.c, called by mixer.c. */
void fa_mixer_mics_process(struct fa_mixer* m, fa_device* dev, float* out, uint32_t frames);   /* audio thread */
void fa_mixer_mics_free(struct fa_mixer* m);       /* fa_mixer_destroy: removes every mic */
void fa_mixer_mics_free_removed(struct fa_mixer* m);   /* free_mixer: frees what fa_mixer_mics_free removed */
void fa_mixer_captures_reopen(struct fa_mixer* m, fa_latency_mode mode);   /* fa_mixer_set_latency_mode */
/* fa_mixer_set_latency_mode without the setup check's guard (mixer.c; mic_check.c uses it). */
fa_result fa_mixer_switch_latency_mode(struct fa_mixer* m, fa_latency_mode mode);
/* mic_fx.c */
fa_result fa_mic_bus_make(struct fa_mixer* m);     /* lock: the bus, once */
void fa_mic_bus_free(fa_mic_bus* b);
void fa_mixer_fx_defaults(struct fa_mixer* m);     /* create: effects and ducking defaults */
/* Audio thread, after the mics added into the bus: the effects, the ducking of `out`
 * (the music so far), then the mics into `out` and the vocals tap. */
void fa_mic_bus_process(struct fa_mixer* m, fa_mic_bus* b, int64_t clock, float* out, uint32_t frames);
/* Frames the vocals tap starts later by (fa_mixer_start_recording). */
int64_t fa_mixer_vocal_shift(struct fa_mixer* m);
/* encode.c: audio thread, `frames` of a tap starting at device frame `clock`. */
void fa_encode_mixer_push(struct fa_mixer* m, int tap, const float* frames, int64_t count, int64_t clock);
/* encode.c: fa_mixer_destroy: the encoders on its taps lose their source. */
void fa_encode_mixer_closing(struct fa_mixer* m);

/* RBJ cookbook sections at Q 1/sqrt(2): 0 low-pass, 1 high-pass, 2 allpass. */
biquad fa_mixer_design(int type, double hz, double rate);

/* Tests: drive a mic's input by hand. take_over stops the input device and makes the
 * bridge expect `rate` and `channels`; feed is what the input callback does; lose is
 * what the backend's notification does when the device goes away. */
void fa_mic_test_take_over(fa_mic* mic, uint32_t rate, uint32_t channels);
void fa_mic_test_feed(fa_mic* mic, const float* frames, uint32_t n);
void fa_mic_test_lose(fa_mic* mic);
void fa_mic_test_stats(fa_mic* mic, double* out_target, double* out_fill, int32_t* out_underruns);
/* Tests: drive the headphones by hand. take_over stops their device and makes the bridge
 * read at `rate` in periods of `period` frames, with `os_ns` as their reported latency;
 * pull is what their callback does; lose is the backend's notification. */
void fa_mixer_test_cue_take_over(fa_mixer* m, uint32_t rate, uint32_t period, fa_time os_ns);
void fa_mixer_test_cue_pull(fa_mixer* m, float* out, uint32_t frames);
void fa_mixer_test_cue_lose(fa_mixer* m);
void fa_mixer_test_cue_stats(fa_mixer* m, double* out_target, double* out_fill, int32_t* out_underruns,
                             int32_t* out_overruns);

#endif
