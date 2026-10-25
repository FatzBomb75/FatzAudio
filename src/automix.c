/* Auto-mix: decks 1 and 2 take turns. The event thread watches the playing deck
 * (fa_mixer_tick, about 20 times a second) and, a couple of seconds before its mix
 * point, schedules the whole transition on the device clock: the incoming deck's
 * start (fa_stream_play_at) and the crossfader ramp. Beat-aligned when both grids are
 * good and the tempos are within reach (the incoming deck is locked to the outgoing
 * one meanwhile), otherwise a plain timed crossfade. Then it waits for the start and
 * the end to be heard, raising the events, and stops the outgoing deck. All of it
 * under the mixer lock; nothing here runs on the audio thread. */
#include "mixer.h"
#include <math.h>
#include <string.h>

#define AUTOMIX_FLAGS_ALL (FA_AUTOMIX_MATCH_TEMPO | FA_AUTOMIX_ALIGN_BEATS | FA_AUTOMIX_BASS_SWAP | FA_AUTOMIX_RESTORE_TEMPO)
#define AUTOMIX_COMMIT_NS 2000000000LL     /* a transition is scheduled this far ahead */
#define AUTOMIX_EMPTY_NS 30000000000LL     /* DECK_EMPTY this long before a mix */
#define AUTOMIX_LEAD_NS 50000000           /* the earliest start: this far after the device clock */
#define AUTOMIX_SAME_TEMPO 0.002           /* without MATCH_TEMPO, beats align only this close */
#define AUTOMIX_SWAP_S 0.5                 /* a bass swap that is not beat-aligned lasts this long */

FA_API void FA_CALL fa_automix_options_init(fa_automix_options* options)
{
    if (!options) return;
    memset(options, 0, sizeof(*options));
    options->struct_size = (uint32_t)sizeof(*options);
    options->length = 8000000000LL;
    options->flags = FA_AUTOMIX_MATCH_TEMPO | FA_AUTOMIX_ALIGN_BEATS;
    options->max_tempo_change = 0.08f;
    options->min_confidence = 0.5f;
    options->restore_time = 20000000000LL;
}

static fa_stream* stream_of(struct fa_mixer* m, uint32_t i) { return (fa_stream*)fa_load_ptr(&m->decks[i].stream); }

static int32_t state_of(fa_stream* s) { return s ? fa_load32(&s->state) : FA_STATE_STOPPED; }

/* The deck auto-mix mixes out of: the playing one; with both playing, the one the
 * crossfader favours. -1 when none plays. */
static int outgoing_deck(struct fa_mixer* m)
{
    int a = state_of(stream_of(m, 0)) == FA_STATE_PLAYING, b = state_of(stream_of(m, 1)) == FA_STATE_PLAYING;
    if (a && b) {
        float x = fa_lane_get(&m->lanes[ML_XF]);
        int side0 = fa_load32(&m->decks[0].side);
        return (side0 == 2) == (x < 0.5f) ? 1 : 0;   /* deck 1's side open more: it is heard most */
    }
    return a ? 0 : b ? 1 : -1;
}

/* The idle deck has nothing new: no stream, or still the song mixed out last. */
static int deck_empty(struct fa_mixer* m, uint32_t i)
{
    fa_stream* s = stream_of(m, i);
    int32_t st = state_of(s);
    return !s || (s == m->am.mixed_out && (st == FA_STATE_STOPPED || st == FA_STATE_ENDED));
}

static void push(fa_mixer_event* ev, uint32_t* n, struct fa_mixer* m, fa_event_type type, uint32_t index, int64_t value)
{
    fa_mixer_event* e = &ev[(*n)++];
    memset(e, 0, sizeof(*e));
    e->type = type;
    e->mixer = m;
    e->index = index;
    e->value = value;
}

/* A transition worked out from now. */
typedef struct am_plan {
    double dt;                   /* seconds from now until the outgoing deck reaches the mix point */
    double len;                  /* seconds the transition lasts */
    int aligned;
    double tempo, factor;        /* for the incoming deck; tempo 0 = leave it */
    double in_start;             /* the incoming deck's source frame to start at */
    double mix_beat;             /* aligned: the outgoing deck's downbeat it starts on */
    fa_deck_now on, in;          /* as looked at; grids valid only when `grids` */
    int grids;
} am_plan;

/* Plans the transition from deck `o` to deck `i`, `skip` = as soon as possible. 0
 * when it can't be planned (the outgoing song's end is not known). */
static int plan(struct fa_mixer* m, uint32_t o, uint32_t i, int skip, int64_t now, am_plan* p)
{
    const fa_automix_options* opt = &m->am.o;
    struct fa_deck* din = &m->decks[i];
    fa_stream* s = stream_of(m, o);
    fa_frames end, mark = FA_FRAMES_UNSET, len_frames = s ? s->decoder.length : -1;
    double speed, mix;
    memset(p, 0, sizeof(*p));
    if (!s) return 0;
    /* An empty incoming deck still gets a plan: the time to the mix point (DECK_EMPTY). */
    p->grids = stream_of(m, i) && fa_deck_look(&m->decks[o], now, &p->on) == FA_OK && fa_deck_look(din, now, &p->in) == FA_OK &&
               p->on.v.g.confidence >= opt->min_confidence && p->in.v.g.confidence >= opt->min_confidence;
    if (!p->grids) {   /* the heard position and tempo still count */
        float t = 1.0f, r = 1.0f;
        memset(&p->on, 0, sizeof(p->on));
        p->on.s = s;
        p->on.sr = (double)s->decoder.format.sample_rate;
        fa_mutex_lock(&s->api_lock);
        p->on.src = (double)fa_stream_clock_at(s, now) * p->on.sr / 1e9;
        fa_mutex_unlock(&s->api_lock);
        fa_stream_get_param(s, FA_PARAM_TEMPO, &t);
        fa_stream_get_param(s, FA_PARAM_RATE, &r);
        p->on.tempo = t;
        p->on.rate = r;
    }
    speed = p->on.tempo * p->on.rate;
    p->in_start = stream_of(m, i) ? (double)fa_deck_cue_point_now(din) : 0.0;
    if (p->grids) {
        double range = (double)opt->max_tempo_change;
        if (opt->flags & FA_AUTOMIX_MATCH_TEMPO) {
            p->tempo = fa_sync_tempo_for(p->on.bpm, p->in.grid_bpm, p->in.rate, range, &p->factor);
            p->aligned = p->tempo > 0.0;
        } else {
            double r = p->on.bpm / p->in.bpm;
            p->factor = 1.0;
            p->aligned = fabs(r - 1.0) < AUTOMIX_SAME_TEMPO;
        }
        if (!(opt->flags & FA_AUTOMIX_ALIGN_BEATS)) p->aligned = 0;
    }
    p->len = (double)opt->length / 1e9;
    if (p->grids && opt->length_beats > 0.0f) p->len = (double)opt->length_beats * 60.0 / p->on.bpm;
    if (p->aligned) {   /* whole bars */
        double bpb = (double)p->on.v.g.beats_per_bar, bars = floor(p->len * p->on.bpm / 60.0 / bpb + 0.5);
        if (bars < 1.0) bars = 1.0;
        p->len = bars * bpb * 60.0 / p->on.bpm;
    }
    if (skip) {
        mix = p->on.src;
    } else {
        if (fa_stream_get_marker(s, FA_MARKER_MIX_POINT, &mark) != FA_OK || mark == FA_FRAMES_UNSET)
            fa_stream_get_marker(s, FA_MARKER_FADE_OUT_START, &mark);
        if (mark != FA_FRAMES_UNSET) {
            mix = (double)mark;
        } else {
            if (fa_stream_get_marker(s, FA_MARKER_CUE_OUT, &end) != FA_OK || end == FA_FRAMES_UNSET ||
                (len_frames >= 0 && end > len_frames))
                end = len_frames;
            if (end < 0) return 0;
            mix = (double)end - p->len * p->on.sr * speed;
            if (mix < 0.0) mix = 0.0;
        }
    }
    if (p->aligned) {
        double bpb = (double)p->on.v.g.beats_per_bar, off = (double)p->on.v.g.bar_offset;
        double b = fa_grid_beat_at(&p->on.v, mix * p->on.v.scale);
        p->mix_beat = skip ? fa_grid_next_downbeat(&p->on.v, b) : off + bpb * floor((b - off) / bpb + 0.5);
        mix = fa_grid_frame_at(&p->on.v, p->mix_beat) / p->on.v.scale;
        {   /* the incoming deck on its first downbeat at or after its cue point */
            double ib = fa_grid_next_downbeat(&p->in.v, fa_grid_beat_at(&p->in.v, p->in_start * p->in.v.scale));
            p->in_start = fa_grid_frame_at(&p->in.v, ib) / p->in.v.scale;
            if (p->in_start < 0.0) p->in_start = 0.0;
        }
    }
    p->dt = (mix - p->on.src) / (p->on.sr * speed);
    return 1;
}

/* The bass swap: the incoming deck's low killed now (it is not heard yet), and from the
 * middle (a beat of the outgoing deck when aligned) the lows cross over `swap` frames.
 * Ramps on the device clock, like the crossfader's. */
static void swap_post(struct fa_mixer* m, struct fa_deck* dout, struct fa_deck* din, const am_plan* p, int64_t start,
                      int64_t len, double rate)
{
    fa_automix* am = &m->am;
    double beat = 60.0 / p->on.bpm * rate, swap = p->aligned ? beat : AUTOMIX_SWAP_S * rate, mid = (double)len / 2.0;
    int64_t at;
    if (swap > (double)len / 4.0 && !p->aligned) swap = (double)len / 4.0;
    if (swap > (double)len / 2.0) swap = (double)len / 2.0;
    if (p->aligned) mid = floor(mid / beat + 0.5) * beat;
    if (mid + swap > (double)len) mid = (double)len - swap;
    at = start + (int64_t)floor(mid + 0.5);
    am->low_in = fa_lane_get(&din->lanes[DL_LOW]);
    am->low_out = fa_lane_get(&dout->lanes[DL_LOW]);
    /* A plain set of no length, then the ramp from it (a ramp posted before the set is
     * taken still starts from the set's value). */
    fa_lane_post(&din->lanes[DL_LOW], FA_DECK_EQ_KILL_DB, 0, NULL, 0, 0, -1, 0, NULL);
    fa_lane_post(&din->lanes[DL_LOW], am->low_in, (int64_t)(swap / rate * 1e9), NULL, 0, 1, at, 0, NULL);
    fa_lane_post(&dout->lanes[DL_LOW], FA_DECK_EQ_KILL_DB, (int64_t)(swap / rate * 1e9), NULL, 0, 1, at, 0, NULL);
    am->swap = 1;
}

/* The lows back to their settings from before the swap: the outgoing deck's (stopped) when
 * the transition is done, both when it is cut short. */
static void swap_undo(struct fa_mixer* m, int both)
{
    fa_automix* am = &m->am;
    if (!am->swap) return;
    am->swap = 0;
    fa_lane_post(&m->decks[am->out_deck].lanes[DL_LOW], am->low_out, MIXER_DEZIPPER_NS, NULL, 0, 0, -1, 0, NULL);
    if (both) fa_lane_post(&m->decks[am->in_deck].lanes[DL_LOW], am->low_in, MIXER_DEZIPPER_NS, NULL, 0, 0, -1, 0, NULL);
}

/* A transition ends before it is done: the lows go back, the lock ends, no glide. */
static void cut_short(struct fa_mixer* m)
{
    fa_automix* am = &m->am;
    struct fa_deck* din = &m->decks[am->in_deck];
    if (am->aligned && din->leader) fa_deck_sync_end(din, 1);
    swap_undo(m, 1);
    am->restore = 0;
    am->phase = AM_WATCH;
}

/* Schedules the planned transition from deck `o` to deck `i`. */
static void commit(struct fa_mixer* m, uint32_t o, uint32_t i, am_plan* p, int64_t now)
{
    fa_device* dev = m->device;
    struct fa_deck* din = &m->decks[i];
    fa_stream* in = stream_of(m, i);
    double rate = (double)dev->rate, speaker = fa_device_speaker_frame(dev, now), fs, lead, speed;
    int side_in = fa_load32(&din->side), side_out = fa_load32(&m->decks[o].side);
    int32_t st = state_of(in);
    int64_t len;
    if (speaker < 0.0) return;   /* the device has not played yet */
    speed = p->on.tempo * p->on.rate;
    lead = (double)fa_load64(&dev->clock) + (double)AUTOMIX_LEAD_NS * rate / 1e9;
    fs = speaker + p->dt * rate;
    if (p->aligned) {
        int k;
        double bpb = (double)p->on.v.g.beats_per_bar;
        for (k = 0; fs < lead && k < 10000; k++) {   /* too soon: the next bar */
            p->mix_beat += bpb;
            fs = speaker + (fa_grid_frame_at(&p->on.v, p->mix_beat) / p->on.v.scale - p->on.src) / (p->on.sr * speed) * rate;
        }
    } else if (fs < lead) {
        fs = lead;
    }
    len = (int64_t)(p->len * rate + 0.5);
    if (st != FA_STATE_PLAYING) {
        if (st == FA_STATE_SCHEDULED) fa_stream_pause(in);
        if (p->tempo > 0.0) fa_stream_set_param(in, FA_PARAM_TEMPO, (float)p->tempo);
        if (fa_stream_seek(in, (fa_frames)floor(p->in_start + 0.5)) != FA_OK ||
            fa_stream_play_at(in, (fa_time)floor(fs * 1e9 / rate + 0.5)) != FA_OK) {
            /* It cannot start: counts as empty, so DECK_EMPTY asks the app for another song. */
            fa_stream_stop(in);
            m->am.mixed_out = in;
            return;
        }
    }
    if (side_in != 0 && side_out != 0 && side_in != side_out) {
        fa_shape shape;
        fa_shape_from_ramp(NULL, FA_CURVE_LINEAR, &shape);
        fa_lane_post(&m->lanes[ML_XF], side_in == 1 ? 0.0f : 1.0f, (int64_t)(p->len * 1e9), &shape, 0, 1,
                     (int64_t)floor(fs + 0.5), 0, NULL);
    }
    m->am.swap = m->am.restore = 0;
    if (st != FA_STATE_PLAYING) {   /* only a deck auto-mix starts gets the swap, the glide and the lock */
        if (m->am.o.flags & FA_AUTOMIX_BASS_SWAP) swap_post(m, &m->decks[o], din, p, (int64_t)floor(fs + 0.5), len, rate);
        m->am.restore = (m->am.o.flags & FA_AUTOMIX_RESTORE_TEMPO) && p->tempo > 0.0 && fabs(p->tempo - p->in.tempo) > 1e-6;
        m->am.restore_to = (float)p->in.tempo;
        m->am.restore_ns = m->am.o.restore_time;
        if (p->aligned) {
            /* Locked to the outgoing deck until the transition is done, measured once heard. */
            fa_deck_sync_end(din, 0);
            /* Tempo follows only when MATCH_TEMPO set it (within max_tempo_change). */
            fa_deck_sync_set(din, &m->decks[o], (p->tempo > 0.0 ? FA_SYNC_TEMPO : 0) | FA_SYNC_BAR | FA_SYNC_BEAT | FA_SYNC_LOCK,
                             p->factor, p->tempo > 0.0 ? p->tempo : p->in.tempo, now + (int64_t)((fs - speaker) / rate * 1e9) + 150000000);
        }
    }
    m->am.phase = AM_SCHEDULED;
    m->am.out_deck = o;
    m->am.in_deck = i;
    m->am.out_s = stream_of(m, o);
    m->am.in_s = in;
    m->am.start = (int64_t)floor(fs + 0.5);
    m->am.end = m->am.start + len;
    m->am.aligned = p->aligned && st != FA_STATE_PLAYING;
}

void fa_automix_tick(struct fa_mixer* m, int64_t now, fa_mixer_event* ev, uint32_t* n, int64_t* next)
{
    fa_automix* am = &m->am;
    int o;
    uint32_t i;
    am_plan p;
    if (!am->on && am->phase == AM_WATCH) return;
    if (now + MIXER_TICK_NS < *next) *next = now + MIXER_TICK_NS;
    if (am->phase != AM_WATCH) {
        double heard = fa_device_speaker_frame(m->device, now);
        struct fa_deck* din = &m->decks[am->in_deck];
        int32_t ist;
        if (stream_of(m, am->out_deck) != am->out_s || stream_of(m, am->in_deck) != am->in_s ||
            (ist = state_of(am->in_s)) == FA_STATE_STOPPED || ist == FA_STATE_PAUSED) {
            /* A deck was loaded, or the incoming one stopped, meanwhile: the transition is the app's now. */
            cut_short(m);
            return;
        }
        if (am->phase == AM_SCHEDULED && heard >= (double)am->start) {
            am->phase = AM_RUNNING;
            if (am->on) push(ev, n, m, FA_EVENT_AUTOMIX_STARTED, am->in_deck, am->in_deck);
        }
        if (am->phase == AM_RUNNING && heard >= (double)am->end) {
            fa_stream_stop(am->out_s);
            /* Stays at the tempo it has, or glides back from there (a bend still on included). */
            if (am->aligned && din->leader) fa_deck_sync_end(din, !am->restore);
            if (am->restore) fa_stream_ramp_tempo_quiet(am->in_s, am->restore_to, am->restore_ns);
            am->restore = 0;
            swap_undo(m, 0);
            am->mixed_out = am->out_s;
            am->phase = AM_WATCH;
            if (am->on) {
                push(ev, n, m, FA_EVENT_AUTOMIX_DONE, am->in_deck, am->in_deck);
                push(ev, n, m, FA_EVENT_DECK_EMPTY, am->out_deck, 0);
            }
        }
        return;
    }
    o = outgoing_deck(m);
    if (o < 0) { am->skip = 0; return; }
    i = (uint32_t)(1 - o);
    if (!plan(m, (uint32_t)o, i, am->skip, now, &p)) { am->skip = 0; return; }
    if (deck_empty(m, i)) {
        fa_stream* out = stream_of(m, (uint32_t)o);
        if (p.dt * 1e9 <= (double)AUTOMIX_EMPTY_NS && am->warned != out) {
            am->warned = out;
            push(ev, n, m, FA_EVENT_DECK_EMPTY, i, 0);
        }
        am->skip = 0;
        return;
    }
    if (!am->skip && p.dt * 1e9 > (double)AUTOMIX_COMMIT_NS) return;
    am->skip = 0;
    commit(m, (uint32_t)o, i, &p, now);
}

void fa_automix_loaded(struct fa_mixer* m, struct fa_deck* d, fa_stream* s)
{
    fa_automix* am = &m->am;
    if (am->phase != AM_WATCH && (d == &m->decks[am->out_deck] || d == &m->decks[am->in_deck])) {
        /* The same address may be a new stream (closed and reopened): the transition ends here. */
        cut_short(m);
    }
    if (!s) return;
    if (m->am.mixed_out == s) m->am.mixed_out = NULL;
    if (m->am.warned == s) m->am.warned = NULL;
}

FA_API fa_result FA_CALL fa_mixer_set_automix(fa_mixer* mixer, const fa_automix_options* options)
{
    fa_automix_options o;
    if (!mixer) return FA_ERROR_INVALID_ARG;
    if (options) {
        if (options->struct_size < sizeof(uint32_t)) return FA_ERROR_INVALID_ARG;
        fa_automix_options_init(&o);
        fa_copy_sized(&o, options, sizeof(o));
        o.struct_size = sizeof(o);
        if (o.length < 500000000LL || o.length > 60000000000LL || !(o.length_beats == 0.0f || (o.length_beats >= 1.0f && o.length_beats <= 256.0f)) ||
            (o.flags & ~(uint32_t)AUTOMIX_FLAGS_ALL) || !(o.max_tempo_change >= 0.0f && o.max_tempo_change <= 0.5f) ||
            !(o.min_confidence >= 0.0f && o.min_confidence <= 1.0f) || o.restore_time < 0 || o.restore_time > 300000000000LL)
            return FA_ERROR_INVALID_ARG;
    }
    fa_mutex_lock(&mixer->lock);
    if (options) {
        if (!mixer->am.on) mixer->am.warned = NULL;
        mixer->am.o = o;
        mixer->am.on = 1;
        if (!mixer->test_ticks) fa_mixer_wake(mixer, 1);
    } else {
        mixer->am.on = 0;
        mixer->am.skip = 0;
    }
    fa_mutex_unlock(&mixer->lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_mixer_automix_now(fa_mixer* mixer)
{
    fa_result r = FA_OK;
    int o;
    if (!mixer) return FA_ERROR_INVALID_ARG;
    fa_mutex_lock(&mixer->lock);
    o = outgoing_deck(mixer);
    if (!mixer->am.on || o < 0 || deck_empty(mixer, (uint32_t)(1 - o))) r = FA_ERROR_NOT_FOUND;
    else if (mixer->am.phase != AM_WATCH) r = FA_ERROR_BUSY;
    else {
        mixer->am.skip = 1;
        if (!mixer->test_ticks) fa_mixer_wake(mixer, 1);
    }
    fa_mutex_unlock(&mixer->lock);
    return r;
}
