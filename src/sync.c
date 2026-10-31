/* Deck sync: tempo matching, beat and bar alignment, the lock, fa_deck_play_synced,
 * beat loops and the beat info.
 *
 * Everything is measured where the decks are heard: each deck's presentation clock
 * (fa_stream_clock_at) gives its heard source frame, its grid turns that into a beat,
 * and the speaker frame (fa_device_speaker_frame) at the same instant ties both to the
 * device clock that fa_stream_play_at schedules on. Both decks share that device, so
 * aligning their mixed frames aligns what is heard, master delay included.
 *
 * A playing deck is pulled into phase by bending its tempo (small errors) or by a
 * jump on its next beat (fa_stream_jump, seamless like a loop). A deck playing
 * backwards hears its beats counting down: its phase is measured on that count, and
 * its next beat is the one below. The lock runs on the
 * engine's event thread, about 20 times a second (fa_mixer_tick), under the mixer
 * lock, like the deck API. Nothing here runs on the audio thread. */
#include "mixer.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SYNC_FLAGS_ALL (FA_SYNC_TEMPO | FA_SYNC_BEAT | FA_SYNC_BAR | FA_SYNC_LOCK)
#define SYNC_PULL_MS 30.0          /* phase errors below this are bent in, above it jumped */
#define SYNC_BEND_MAX 0.02         /* the most a pull bends the tempo */
#define SYNC_SETTLED_MS 1.0        /* a one-off pull is done within this, two ticks in a row */
#define SYNC_JUMP_TICK_NS 5000000  /* a waiting jump is tried this often */
#define SYNC_START_LEAD_NS 50000000   /* fa_deck_play_synced starts at least this far ahead */

int64_t fa_mixer_now(struct fa_mixer* m) { return m->test_now ? m->test_now : fa_monotonic_ns(); }

void fa_mixer_wake(struct fa_mixer* m, int64_t at)
{
    fa_device* dev = m->device;
    if (at <= 0) at = 1;
    for (;;) {
        int64_t t = fa_load64(&dev->mixer_tick_at);
        if (t != 0 && t <= at) return;
        if (fa_cas64(&dev->mixer_tick_at, t, at)) break;
    }
    fa_wake_signal(&dev->engine->event_wake);   /* sooner than it planned to look */
}

/* ---- grids ---------------------------------------------------------------------- */

/* The stream's grid into the deck's buffer. */
static fa_result load_grid(struct fa_deck* d, fa_stream* s, fa_grid_view* v)
{
    uint32_t n = 0;
    fa_result r;
    memset(v, 0, sizeof(*v));
    for (;;) {
        v->g.struct_size = sizeof(v->g);
        r = fa_stream_beat_grid_now(s, &v->g, d->segs, d->segs_cap, &n);   /* a module's: BUSY at first */
        if (r != FA_OK) return r;
        if (n <= d->segs_cap) break;
        {   /* the map grew (or is read for the first time) */
            fa_tempo_segment* p = (fa_tempo_segment*)realloc(d->segs, (size_t)n * sizeof(*p));
            if (!p) return FA_ERROR_OUT_OF_MEMORY;
            d->segs = p;
            d->segs_cap = n;
        }
    }
    if (n == 0 || s->decoder.format.sample_rate == 0) return FA_ERROR_NOT_FOUND;
    v->segs = d->segs;
    v->n = n;
    v->scale = (double)v->g.sample_rate / (double)s->decoder.format.sample_rate;
    return FA_OK;
}

static double frames_per_beat(const fa_grid_view* v, uint32_t i) { return 60.0 * (double)v->g.sample_rate / v->segs[i].bpm; }

/* The last segment starting at or before `frame` (the first one before it). */
static uint32_t seg_at_frame(const fa_grid_view* v, double frame)
{
    uint32_t lo = 0, hi = v->n;
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if ((double)v->segs[mid].start <= frame) lo = mid;
        else hi = mid;
    }
    return lo;
}

static uint32_t seg_at_beat(const fa_grid_view* v, double beat)
{
    uint32_t lo = 0, hi = v->n;
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if (v->segs[mid].beat <= beat) lo = mid;
        else hi = mid;
    }
    return lo;
}

double fa_grid_beat_at(const fa_grid_view* v, double frame)
{
    uint32_t i = seg_at_frame(v, frame);
    return v->segs[i].beat + (frame - (double)v->segs[i].start) / frames_per_beat(v, i);
}

double fa_grid_frame_at(const fa_grid_view* v, double beat)
{
    uint32_t i = seg_at_beat(v, beat);
    return (double)v->segs[i].start + (beat - v->segs[i].beat) * frames_per_beat(v, i);
}

/* Source frame of beat `beat`. */
static double src_of_beat(const fa_grid_view* v, double beat) { return fa_grid_frame_at(v, beat) / v->scale; }

double fa_grid_next_downbeat(const fa_grid_view* v, double beat)
{
    double bpb = (double)v->g.beats_per_bar, off = (double)v->g.bar_offset;
    return off + bpb * ceil((beat - off) / bpb - 1e-6);
}

/* ---- what a deck plays now ---------------------------------------------------------- */

fa_result fa_deck_look(struct fa_deck* d, int64_t now, fa_deck_now* o)
{
    fa_stream* s = (fa_stream*)fa_load_ptr(&d->stream);
    float tempo = 1.0f, rate = 1.0f;
    fa_result r;
    memset(o, 0, sizeof(*o));
    if (!s) return FA_ERROR_NOT_FOUND;
    if ((r = load_grid(d, s, &o->v)) != FA_OK) return r;
    o->s = s;
    o->sr = (double)s->decoder.format.sample_rate;
    fa_mutex_lock(&s->api_lock);
    o->src = (double)fa_stream_clock_at(s, now) * o->sr / 1e9;
    o->state = fa_load32(&s->state);
    o->reversed = fa_load32(&s->reverse);
    fa_mutex_unlock(&s->api_lock);
    fa_stream_get_param(s, FA_PARAM_TEMPO, &tempo);
    fa_stream_get_param(s, FA_PARAM_RATE, &rate);
    o->tempo = tempo;
    o->rate = rate;
    o->beat = fa_grid_beat_at(&o->v, o->src * o->v.scale);
    o->grid_bpm = o->v.segs[seg_at_frame(&o->v, o->src * o->v.scale)].bpm;
    o->bpm = o->grid_bpm * o->tempo * o->rate;
    return FA_OK;
}

static int playing(const fa_deck_now* n) { return n->state == FA_STATE_PLAYING; }

/* ---- tempo and phase ------------------------------------------------------------------ */

double fa_sync_tempo_for(double bpm, double grid_bpm, double rate, double range, double* out_factor)
{
    double t = bpm / (grid_bpm * rate), c[3], f[3];
    int i;
    c[0] = t; f[0] = 1.0;
    /* Then whichever of double and half time is nearer the deck's own tempo. */
    if (fabs(log(2.0 * t)) < fabs(log(0.5 * t))) { c[1] = 2.0 * t; f[1] = 0.5; c[2] = 0.5 * t; f[2] = 2.0; }
    else { c[1] = 0.5 * t; f[1] = 2.0; c[2] = 2.0 * t; f[2] = 0.5; }
    for (i = 0; i < 3; i++) {
        if (c[i] >= 1.0 - range - 1e-9 && c[i] <= 1.0 + range + 1e-9) {
            *out_factor = f[i];
            return c[i];
        }
    }
    return 0.0;
}

/* Beats heard since `off` (the bar offset, or 0): counting down when reversed. */
static double heard_beats(const fa_deck_now* n, double off) { return n->reversed ? off - n->beat : n->beat - off; }

/* The phase error of `dn` against `ln`, in the leader's beats (+ = the deck is ahead),
 * wrapped to the nearest beat (BAR: the nearest bar). */
static double phase_error(const fa_deck_now* dn, const fa_deck_now* ln, double factor, uint32_t flags)
{
    double x, p;
    if (flags & FA_SYNC_BAR) {
        double lb = (double)ln->v.g.beats_per_bar, db = (double)dn->v.g.beats_per_bar * factor;
        x = heard_beats(dn, (double)dn->v.g.bar_offset) * factor - heard_beats(ln, (double)ln->v.g.bar_offset);
        p = lb < db ? lb : db;
    } else {
        x = heard_beats(dn, 0.0) * factor - heard_beats(ln, 0.0);
        p = factor < 1.0 ? factor : 1.0;
    }
    return x - p * floor(x / p + 0.5);
}

/* The next beat (BAR: downbeat) heard from `beat` on, in the direction played. */
static double next_beat(const fa_grid_view* v, double beat, int reversed, int bar)
{
    double bpb = (double)v->g.beats_per_bar, off = (double)v->g.bar_offset;
    if (!reversed) return bar ? fa_grid_next_downbeat(v, beat) : ceil(beat - 1e-6);
    return bar ? off + bpb * floor((beat - off) / bpb + 1e-6) : floor(beat + 1e-6);
}

static double error_ms(double leader_beats, const fa_deck_now* ln) { return leader_beats * 60000.0 / ln->bpm; }

static void set_tempo(struct fa_deck* d, fa_stream* s, double tempo)
{
    float t = (float)tempo;
    if (t == d->sync_tempo) return;
    if (fa_stream_set_param(s, FA_PARAM_TEMPO, t) == FA_OK) d->sync_tempo = t;
}

void fa_deck_sync_end(struct fa_deck* d, int restore)
{
    fa_stream* s = (fa_stream*)fa_load_ptr(&d->stream);
    if (!d->leader) return;
    /* A bend still on: the tempo goes back to what sync wanted. */
    if (restore && s && s == d->sync_s && d->sync_tempo != (float)d->sync_base)
        fa_stream_set_param(s, FA_PARAM_TEMPO, (float)d->sync_base);
    d->leader = NULL;
    d->sync_flags = 0;
    d->settling = 0;
    d->jumping = 0;
    d->sync_s = d->sync_ls = NULL;
}

void fa_deck_sync_loaded(struct fa_deck* d)
{
    struct fa_mixer* m = d->mixer;
    uint32_t i;
    for (i = 0; i < m->deck_count; i++) {
        struct fa_deck* o = &m->decks[i];
        if (o == d) fa_deck_sync_end(o, 0);           /* its stream is another one now */
        else if (o->leader == d) fa_deck_sync_end(o, 1);
    }
}

void fa_deck_sync_set(struct fa_deck* d, struct fa_deck* leader, uint32_t flags, double factor, double tempo,
                      int64_t hold_until)
{
    d->leader = leader;
    d->sync_flags = flags;
    d->sync_factor = factor;
    d->sync_base = tempo;
    d->sync_tempo = (float)tempo;
    d->sync_s = (fa_stream*)fa_load_ptr(&d->stream);
    d->sync_ls = (fa_stream*)fa_load_ptr(&leader->stream);
    d->settling = 0;
    d->settled = 0;
    d->jumping = 0;
    d->hold_until = hold_until;
    if (flags & FA_SYNC_LOCK) fa_mixer_wake(d->mixer, 1);
}

/* One look at a synced deck: tempo (LOCK follows the leader), then phase. Returns 1
 * while it wants the next tick soon (a jump waits). */
static int sync_tick(struct fa_deck* d, int64_t now)
{
    struct fa_deck* l = d->leader;
    fa_deck_now dn, ln;
    double base = d->sync_base, bend = 0.0;
    uint32_t f = d->sync_flags;
    if (!l) return 0;
    if ((fa_stream*)fa_load_ptr(&d->stream) != d->sync_s || (fa_stream*)fa_load_ptr(&l->stream) != d->sync_ls) {
        fa_deck_sync_end(d, 0);
        return 0;
    }
    if (!(f & FA_SYNC_LOCK) && !d->settling) return 0;
    if (fa_deck_look(d, now, &dn) != FA_OK || fa_deck_look(l, now, &ln) != FA_OK) return 0;
    if ((f & FA_SYNC_TEMPO) && (f & FA_SYNC_LOCK)) {
        double range = (double)fa_load_float(&d->sync_range);
        base = ln.bpm / d->sync_factor / (dn.grid_bpm * dn.rate);
        if (base < 1.0 - range) base = 1.0 - range;
        if (base > 1.0 + range) base = 1.0 + range;
        d->sync_base = base;
    }
    if (d->settling && !(playing(&dn) && playing(&ln)))   /* the pull's time starts once both play */
        d->settle_end = now + (int64_t)(8.0 * 60e9 / ln.bpm) + 1000000000;
    /* A jump worked out for the other direction (of either deck) is dropped: measured anew. */
    if (d->jumping && (dn.reversed != d->jump_rev || ln.reversed != d->jump_lrev)) d->jumping = 0;
    if ((f & FA_SYNC_BEAT) && playing(&dn) && playing(&ln) && now >= d->hold_until) {
        if (d->jumping) {
            fa_result r = fa_stream_jump(dn.s, d->jump_at, d->jump_by);
            double ahead = dn.reversed ? dn.src - (double)d->jump_at : (double)d->jump_at - dn.src;
            if (r == FA_OK || (r == FA_ERROR_BUSY && ahead < -dn.sr)) {
                /* Done (or missed by a second: measured anew). Measure again once heard. */
                d->jumping = 0;
                d->hold_until = now + (int64_t)(ahead / (dn.sr * dn.tempo * dn.rate) * 1e9) + 150000000;
            } else if (r != FA_ERROR_BUSY) {
                d->jumping = 0;
            }
            if (d->jumping) return 1;
        } else {
            double x = phase_error(&dn, &ln, d->sync_factor, f), ms = error_ms(x, &ln), xd = x / d->sync_factor;
            if (fabs(ms) >= SYNC_PULL_MS) {
                /* Too far to bend: jump on the deck's next beat by the error (reversed, the
                 * beat below, and back up the song to fall behind). */
                double nb = dn.reversed ? floor(dn.beat - 1e-6) : ceil(dn.beat + 1e-6), fpb = 60.0 * dn.sr / dn.grid_bpm;
                d->jump_at = (fa_frames)floor(src_of_beat(&dn.v, nb) + 0.5);
                d->jump_by = (fa_frames)floor((dn.reversed ? xd : -xd) * fpb + 0.5);
                d->jump_rev = dn.reversed;
                d->jump_lrev = ln.reversed;
                d->jumping = 1;
                d->settled = 0;
                set_tempo(d, dn.s, base);
                return 1;
            }
            /* Pulled in over about two beats. */
            bend = -xd / 2.0;
            if (bend > SYNC_BEND_MAX) bend = SYNC_BEND_MAX;
            if (bend < -SYNC_BEND_MAX) bend = -SYNC_BEND_MAX;
            if (d->settling) {
                d->settled = fabs(ms) < SYNC_SETTLED_MS ? d->settled + 1 : 0;
                if (d->settled >= 2 || now >= d->settle_end) {
                    d->settling = 0;
                    bend = 0.0;
                }
            }
        }
    }
    set_tempo(d, dn.s, base * (1.0 + bend));
    return 0;
}

void fa_mixer_tick(struct fa_mixer* m, int64_t now, fa_mixer_event* ev, uint32_t* n)
{
    int64_t next = INT64_MAX;
    uint32_t i;
    for (i = 0; i < m->deck_count; i++) {
        struct fa_deck* d = &m->decks[i];
        int soon = sync_tick(d, now);
        if (soon) { if (now + SYNC_JUMP_TICK_NS < next) next = now + SYNC_JUMP_TICK_NS; }
        else if (d->leader && ((d->sync_flags & FA_SYNC_LOCK) || d->settling) && now + MIXER_TICK_NS < next)
            next = now + MIXER_TICK_NS;
    }
    fa_automix_tick(m, now, ev, n, &next);
    if (next != INT64_MAX && !m->test_ticks) fa_mixer_wake(m, next);
}

/* ---- API ------------------------------------------------------------------------- */

/* The tempo and factor for syncing `dn` to `ln` (TEMPO, else the deck's own tempo,
 * with the factor that fits its heard BPM best). */
static fa_result pick_tempo(struct fa_deck* d, const fa_deck_now* dn, const fa_deck_now* ln, uint32_t flags,
                            double* tempo, double* factor)
{
    if (flags & FA_SYNC_TEMPO) {
        *tempo = fa_sync_tempo_for(ln->bpm, dn->grid_bpm, dn->rate, (double)fa_load_float(&d->sync_range), factor);
        return *tempo > 0.0 ? FA_OK : FA_ERROR_OUT_OF_RANGE;
    }
    {   /* the nearest of 1, 2 and 1/2 */
        double r = ln->bpm / dn->bpm;
        *tempo = dn->tempo;
        *factor = fabs(log(r)) <= fabs(log(r / 2.0)) && fabs(log(r)) <= fabs(log(r * 2.0)) ? 1.0
                : fabs(log(r / 2.0)) < fabs(log(r * 2.0)) ? 2.0 : 0.5;
    }
    return FA_OK;
}

static fa_result check_pair(fa_deck* deck, fa_deck* leader, uint32_t flags)
{
    if (!deck || (flags & ~(uint32_t)SYNC_FLAGS_ALL)) return FA_ERROR_INVALID_ARG;
    if (leader && (leader == deck || leader->mixer != deck->mixer)) return FA_ERROR_INVALID_ARG;
    return FA_OK;
}

/* Caller holds the mixer lock; both decks have streams and grids. */
static fa_result sync_now(struct fa_deck* d, struct fa_deck* l, uint32_t flags, int64_t now)
{
    fa_deck_now dn, ln;
    double tempo, factor;
    fa_result r;
    if ((r = fa_deck_look(d, now, &dn)) != FA_OK || (r = fa_deck_look(l, now, &ln)) != FA_OK) return r;
    if (d->leader && d->sync_s == dn.s) dn.tempo = d->sync_base;   /* a bend still on is not the deck's tempo */
    if ((r = pick_tempo(d, &dn, &ln, flags, &tempo, &factor)) != FA_OK) return r;
    fa_deck_sync_end(d, 1);
    if ((flags & FA_SYNC_TEMPO) && fa_stream_set_param(dn.s, FA_PARAM_TEMPO, (float)tempo) != FA_OK)
        return FA_ERROR_GENERIC;
    fa_deck_sync_set(d, l, flags, factor, tempo, 0);
    if ((flags & FA_SYNC_BEAT) && !(flags & FA_SYNC_LOCK)) {
        /* A one-off pull: about eight beats at most. */
        d->settling = 1;
        d->settle_end = now + (int64_t)(8.0 * 60e9 / ln.bpm) + 1000000000;
    }
    if ((flags & FA_SYNC_BEAT) && sync_tick(d, now)) fa_mixer_wake(d->mixer, now + SYNC_JUMP_TICK_NS);
    else if (d->settling) fa_mixer_wake(d->mixer, now + MIXER_TICK_NS);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_deck_sync(fa_deck* deck, fa_deck* leader, uint32_t flags)
{
    fa_result r;
    if ((r = check_pair(deck, leader, flags)) != FA_OK) return r;
    if (flags & FA_SYNC_BAR) flags |= FA_SYNC_BEAT;
    fa_mutex_lock(&deck->mixer->lock);
    if (!leader || !flags) {
        fa_deck_sync_end(deck, 1);
        r = FA_OK;
    } else {
        r = sync_now(deck, leader, flags, fa_mixer_now(deck->mixer));
    }
    fa_mutex_unlock(&deck->mixer->lock);
    return r;
}

FA_API fa_result FA_CALL fa_deck_play_synced(fa_deck* deck, fa_deck* leader, uint32_t flags)
{
    struct fa_mixer* m;
    fa_device* dev;
    fa_deck_now dn, ln;
    double tempo, factor, c, nb, pre, speaker, k, step, fs = 0.0, lead;
    int64_t now;
    int i;
    fa_result r;
    if ((r = check_pair(deck, leader, flags)) != FA_OK) return r;
    if (!leader) return FA_ERROR_INVALID_ARG;
    flags |= FA_SYNC_BEAT;
    m = deck->mixer;
    dev = m->device;
    fa_mutex_lock(&m->lock);
    now = fa_mixer_now(m);
    if ((r = fa_deck_look(deck, now, &dn)) != FA_OK || (r = fa_deck_look(leader, now, &ln)) != FA_OK) goto done;
    if (playing(&dn)) { r = sync_now(deck, leader, flags, now); goto done; }
    if (!playing(&ln)) { r = FA_ERROR_INVALID_ARG; goto done; }
    if ((r = pick_tempo(deck, &dn, &ln, flags, &tempo, &factor)) != FA_OK) goto done;
    speaker = fa_device_speaker_frame(dev, now);
    if (speaker < 0.0) { r = FA_ERROR_INVALID_ARG; goto done; }    /* the device has not played yet */
    fa_deck_sync_end(deck, 0);
    if (dn.state == FA_STATE_SCHEDULED) fa_stream_pause(dn.s);
    if ((flags & FA_SYNC_TEMPO) && fa_stream_set_param(dn.s, FA_PARAM_TEMPO, (float)tempo) != FA_OK) { r = FA_ERROR_GENERIC; goto done; }
    /* Where it starts, and the beat it lands with. */
    if (dn.state == FA_STATE_PAUSED) { fa_frames p = 0; fa_stream_get_position(dn.s, &p); c = (double)p; }
    else c = (double)fa_deck_cue_point_now(deck);
    /* Seconds from the start to the beat it lands with (reversed: the beats count down). */
    nb = next_beat(&dn.v, fa_grid_beat_at(&dn.v, c * dn.v.scale), dn.reversed, (flags & FA_SYNC_BAR) != 0);
    pre = (dn.reversed ? c - src_of_beat(&dn.v, nb) : src_of_beat(&dn.v, nb) - c) / (dn.sr * tempo * dn.rate);
    /* The leader's beats (BAR: downbeats) as device frames; the first one far enough ahead. */
    k = next_beat(&ln.v, ln.beat, ln.reversed, (flags & FA_SYNC_BAR) != 0);
    step = ((flags & FA_SYNC_BAR) ? (double)ln.v.g.beats_per_bar : 1.0) * (ln.reversed ? -1.0 : 1.0);
    lead = (double)fa_load64(&dev->clock) + (double)SYNC_START_LEAD_NS * dev->rate / 1e9;
    for (i = 0; i < 100000; i++, k += step) {
        double d = src_of_beat(&ln.v, k) - ln.src;
        double at = speaker + (ln.reversed ? -d : d) / (ln.sr * ln.tempo * ln.rate) * dev->rate;
        fs = at - pre * dev->rate;
        if (fs >= lead) break;
    }
    if (fabs(c - (double)fa_stream_position_now(dn.s)) >= 0.5 || dn.state == FA_STATE_STOPPED || dn.state == FA_STATE_ENDED)
        if ((r = fa_stream_seek(dn.s, (fa_frames)c)) != FA_OK) goto done;
    r = fa_stream_play_at(dn.s, (fa_time)floor(fs * 1e9 / dev->rate + 0.5));
    if (r == FA_OK)
        fa_deck_sync_set(deck, leader, flags, factor, (flags & FA_SYNC_TEMPO) ? tempo : dn.tempo,
                         now + (int64_t)((fs - speaker) * 1e9 / dev->rate) + 100000000);
done:
    fa_mutex_unlock(&m->lock);
    return r;
}

FA_API fa_result FA_CALL fa_deck_set_loop_beats(fa_deck* deck, double beats)
{
    struct fa_mixer* m;
    fa_deck_now dn;
    fa_frames in = 0, out = 0;
    double start;
    fa_result r;
    if (!deck || !(beats >= 0.0) || beats > 32.0 || (beats > 0.0 && beats < 1.0 / 16.0 - 1e-9)) return FA_ERROR_INVALID_ARG;
    m = deck->mixer;
    fa_mutex_lock(&m->lock);
    if (!fa_load_ptr(&deck->stream)) { fa_mutex_unlock(&m->lock); return FA_ERROR_NOT_FOUND; }
    if (beats == 0.0) {
        r = fa_stream_set_loop_region((fa_stream*)fa_load_ptr(&deck->stream), 0, 0);
        fa_mutex_unlock(&m->lock);
        return r;
    }
    if ((r = fa_deck_look(deck, fa_mixer_now(m), &dn)) != FA_OK) { fa_mutex_unlock(&m->lock); return r; }
    fa_stream_get_loop_region(dn.s, &in, &out);
    if (out > in) {
        /* Halving and doubling keep the start (reversed: the end, where it is entered). */
        start = dn.reversed ? fa_grid_beat_at(&dn.v, (double)out * dn.v.scale) - beats
                            : fa_grid_beat_at(&dn.v, (double)in * dn.v.scale);
    } else {
        double q = beats < 1.0 ? beats : 1.0;
        /* From the beat heard now on: reversed, the loop ends there and lies below. */
        start = dn.reversed ? ceil((dn.beat - 1e-6) / q) * q - beats : floor((dn.beat + 1e-6) / q) * q;
    }
    while (src_of_beat(&dn.v, start) < -0.5) start += 1.0;   /* before the song: its first beat in it */
    in = (fa_frames)floor(src_of_beat(&dn.v, start) + 0.5);
    out = (fa_frames)floor(src_of_beat(&dn.v, start + beats) + 0.5);
    if (dn.s->decoder.length > 0 && out > dn.s->decoder.length) out = dn.s->decoder.length;
    r = out > in ? fa_stream_set_loop_region(dn.s, in, out) : FA_ERROR_INVALID_ARG;
    fa_mutex_unlock(&m->lock);
    return r;
}

FA_API fa_result FA_CALL fa_deck_beat_jump(fa_deck* deck, double beats)
{
    struct fa_mixer* m;
    fa_deck_now dn;
    fa_frames in = 0, out = 0, last;
    double delta;
    fa_result r;
    int began;
    if (!deck || !(fabs(beats) >= 1.0 / 16.0 - 1e-9 && fabs(beats) <= 1024.0)) return FA_ERROR_INVALID_ARG;
    m = deck->mixer;
    fa_mutex_lock(&m->lock);
    if ((r = fa_deck_look(deck, fa_mixer_now(m), &dn)) != FA_OK) goto done;
    if (dn.s->push || !dn.s->decoder.seekable) { r = FA_ERROR_NOT_SEEKABLE; goto done; }
    if (deck->jumping) { r = FA_ERROR_BUSY; goto done; }   /* sync's own jump waits for its beat */
    /* Whole grid beats from the beat heard now: the phase is kept, also on a tempo map. */
    delta = floor(src_of_beat(&dn.v, dn.beat + beats) - src_of_beat(&dn.v, dn.beat) + 0.5);
    last = dn.s->decoder.length > 0 ? dn.s->decoder.length : -1;
    fa_stream_get_loop_region(dn.s, &in, &out);
    if (out > in) {   /* the loop moves along: within the song, its length kept */
        fa_frames len = out - in, nin = in + (fa_frames)delta;
        if (nin < 0) nin = 0;
        if (last > 0 && nin + len > last) nin = last - len;
        if (nin < 0) nin = 0;
        delta = (double)(nin - in);
        in = nin;
        out = nin + len;
    }
    if (!playing(&dn)) {
        fa_frames at = 0;
        double p;
        fa_stream_get_position(dn.s, &at);   /* a pending async seek's target counts */
        p = (double)at + delta;
        if (p < 0.0) p = 0.0;
        if (last > 0 && p > (double)last) p = (double)last;
        r = fa_stream_seek(dn.s, (fa_frames)p);
        if (r == FA_OK && out > in) r = fa_stream_set_loop_region(dn.s, in, out);
        goto done;
    }
    /* The jump, and a loop moved with it at the same seamless join (with slip mode on,
     * the deck goes on counting where it would be: fa_deck_slip_return comes back). */
    began = fa_stream_slip_begin(dn.s);
    r = out > in ? fa_stream_jump_loop(dn.s, (fa_frames)delta, in, out) : fa_stream_jump(dn.s, -1, (fa_frames)delta);
    if (began && r != FA_OK && r != FA_ERROR_BUSY) fa_stream_slip_cancel(dn.s);   /* no jump: no slip (BUSY is tried again) */
done:
    fa_mutex_unlock(&m->lock);
    return r;
}

FA_API fa_result FA_CALL fa_deck_get_beat_info(fa_deck* deck, fa_deck_beat_info* out_info)
{
    fa_deck_beat_info info;
    fa_deck_now dn, ln;
    fa_result r;
    int64_t now;
    if (!deck || !out_info || out_info->struct_size < sizeof(uint32_t)) return FA_ERROR_INVALID_ARG;
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    fa_mutex_lock(&deck->mixer->lock);
    now = fa_mixer_now(deck->mixer);
    r = fa_deck_look(deck, now, &dn);
    if (r == FA_OK) {
        info.bpm = dn.bpm;
        info.beat = dn.beat;
        info.bar = (dn.beat - (double)dn.v.g.bar_offset) / (double)dn.v.g.beats_per_bar;
        if (deck->leader && (deck->sync_flags & FA_SYNC_BEAT) && fa_deck_look(deck->leader, now, &ln) == FA_OK)
            info.phase_error_ms = error_ms(phase_error(&dn, &ln, deck->sync_factor, deck->sync_flags), &ln);
        info.synced = deck->leader != NULL;
        info.locked = deck->leader && (deck->sync_flags & FA_SYNC_LOCK);
    }
    fa_mutex_unlock(&deck->mixer->lock);
    if (r == FA_ERROR_BUSY) r = FA_ERROR_NOT_FOUND;   /* a module's grid is still being worked out */
    if (r == FA_OK) fa_copy_sized(out_info, &info, sizeof(info));
    return r;
}
