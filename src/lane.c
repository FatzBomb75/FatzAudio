/* Parameter lanes: one automatable value each (see fa_lane in fa_internal.h).
 * API threads post commands through a 2-slot mailbox; the audio thread takes the
 * newest one and runs it. Nothing here locks or allocates on the audio side. */
#include "fa_internal.h"
#include <string.h>

void fa_lane_init(fa_lane* l, float value)
{
    memset(l, 0, sizeof(*l));
    fa_store32(&l->mb.word, FA_CMD_CLAIMED); /* nothing pending */
    l->v.value = value;
    l->v.at = -1;
    l->mb.last_target = value;
    fa_store_float(&l->current, value);
}

int fa_lane_post(fa_lane* l, float target, int64_t duration_ns, const fa_shape* shape, int64_t tag,
                 int notify, int64_t at, int flags, int64_t* out_replaced_tag)
{
    fa_param_mailbox* m = &l->mb;
    int32_t old, seq, prev;
    fa_param_slot* slot;
    int has_start = 0, replaced = 0;
    float start = 0.0f;
    fa_shape linear;

    if (!shape) { memset(&linear, 0, sizeof(linear)); shape = &linear; }
    old = fa_load32(&m->word);
    seq = ((old & ~FA_CMD_CLAIMED) + 1) & ~FA_CMD_CLAIMED;
    slot = &m->slot[seq & 1];
    if (notify && !(old & FA_CMD_CLAIMED)) {
        /* A ramp replacing a command the mixer has not applied yet: keep the value a
         * plain set asked for, so "volume = 0, then ramp to 1" still fades in from 0.
         * (A plain set replacing one simply takes over, gliding from what is heard.) */
        if (!m->last_notify) { has_start = 1; start = m->last_target; }
        else if (m->last_has_start) { has_start = 1; start = m->last_start; }
    }
    fa_store32(&slot->has_start, has_start);
    fa_store_float(&slot->start, start);
    fa_store_float(&slot->target, target);
    fa_store64(&slot->frames_ns, duration_ns);
    fa_shape_store(&slot->shape, shape);
    fa_store64(&slot->tag, tag);
    fa_store32(&slot->notify, notify);
    fa_store64(&slot->at, at);
    fa_store32(&slot->flags, flags);

    /* Publish. If the audio thread never claimed the previous command, it never
     * started, so its RAMP_DONE is ours to raise (the audio thread will not). */
    for (;;) {
        prev = fa_load32(&m->word);
        if (fa_cas32(&m->word, prev, seq)) break;
    }
    if (!(prev & FA_CMD_CLAIMED) && m->last_notify) {
        replaced = 1;
        if (out_replaced_tag) *out_replaced_tag = m->last_tag;
    }
    m->last_notify = notify;
    m->last_tag = tag;
    m->last_target = target;
    m->last_has_start = has_start;
    m->last_start = start;
    return replaced;
}

float fa_lane_get(fa_lane* l)
{
    fa_param_mailbox* m = &l->mb;
    int pending = !(fa_load32(&m->word) & FA_CMD_CLAIMED);
    /* A plain set that the mixer has not picked up yet reads back as the new value. */
    if (pending && !m->last_notify) return m->last_target;
    if (pending && m->last_has_start) return m->last_start;
    return fa_load_float(&l->current);
}

int fa_lane_take(fa_lane* l, int audible, uint32_t rate, int64_t* out_replaced_tag)
{
    return fa_lane_take_after(l, INT64_MIN, audible, rate, out_replaced_tag);
}

int fa_lane_take_after(fa_lane* l, int64_t hold_upto, int audible, uint32_t rate, int64_t* out_replaced_tag)
{
    fa_param_mailbox* m = &l->mb;
    fa_param_voice* v = &l->v;
    fa_param_slot* slot;
    int32_t w = fa_load32(&m->word);
    float target, start;
    int64_t ns, tag, at;
    int notify, has_start, flags, replaced = 0;
    fa_shape shape;
    if (w & FA_CMD_CLAIMED) return 0;
    slot = &m->slot[w & 1];
    target = fa_load_float(&slot->target);
    ns = fa_load64(&slot->frames_ns);
    fa_shape_load(&slot->shape, &shape);
    tag = fa_load64(&slot->tag);
    notify = fa_load32(&slot->notify);
    has_start = fa_load32(&slot->has_start);
    start = fa_load_float(&slot->start);
    at = fa_load64(&slot->at);
    flags = fa_load32(&slot->flags);
    if (hold_upto != INT64_MIN && at <= hold_upto) return 0;   /* at < 0: at once */
    if (!fa_cas32(&m->word, w, w | FA_CMD_CLAIMED)) return 0; /* replaced meanwhile; take it next time */
    if (has_start) v->value = start;

    if (v->active && v->notify) { replaced = 1; if (out_replaced_tag) *out_replaced_tag = v->tag; }
    if (!notify && !audible) {
        /* A plain set while nothing was audible applies at once; no glide needed. */
        v->active = 0;
        v->value = target;
        return replaced;
    }
    v->active = 1;
    v->start = v->value;
    v->target = target;
    v->total = (int64_t)((double)ns * rate / 1e9 + 0.5);
    v->done = 0;
    v->at = at;
    v->shape = shape;
    v->flags = flags;
    v->notify = notify;
    v->tag = tag;
    return replaced;
}

float fa_lane_ramp_value(const fa_param_voice* v, int64_t done)
{
    float t, k;
    if (done >= v->total) return v->target;
    if (done < 0) done = 0;
    t = (float)((double)done / (double)v->total);
    k = (v->flags & FA_LANE_MIRROR) ? fa_shape_eval_mirror(&v->shape, t)
                                    : fa_shape_eval(&v->shape, t, v->target >= v->start);
    return v->start + (v->target - v->start) * k;
}

void fa_lane_start_ramp(fa_lane* l, float from, float target, int64_t frames, int64_t at,
                        const fa_shape* shape, int flags)
{
    fa_param_voice* v = &l->v;
    v->value = from;
    v->active = 1;
    v->start = from;
    v->target = target;
    v->total = frames;
    v->done = 0;
    v->at = at;
    v->shape = *shape;
    v->flags = flags;
    v->notify = 0;
    v->tag = 0;
}

float fa_lane_value_at_frame(const fa_param_voice* v, int64_t clock, int64_t frame, int from_here)
{
    int64_t begin;
    if (!v->active) return v->value;
    begin = v->at > clock ? v->at : clock;
    if (frame < begin) return v->value;
    if (frame == begin && (!from_here || v->done < v->total)) return v->value;  /* a zero-length ramp jumps here */
    return fa_lane_ramp_value(v, v->done + (frame - begin));
}

int fa_lane_advance(fa_param_voice* v, int64_t clock, int64_t frames)
{
    int64_t begin;
    if (!v->active) return 0;
    begin = v->at > clock ? v->at : clock;
    if (begin >= clock + frames) return 0;         /* not started yet */
    v->done += clock + frames - begin;
    v->at = -1;
    v->value = fa_lane_ramp_value(v, v->done);
    if (v->done >= v->total) { v->active = 0; v->value = v->target; return 1; }
    return 0;
}
