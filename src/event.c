/* Event delivery. The audio thread pushes into a per-device lock-free queue; API
 * and worker threads post into an engine list under a mutex. The engine's event
 * thread drains both and calls the user's callbacks, so user code never runs on
 * the audio thread. It also frees closed streams, after their last event. */
#include "fa_internal.h"
#include <stdlib.h>
#include <string.h>

int fa_event_queue_push(fa_event_queue* q, const fa_qevent* ev)
{
    int32_t h = fa_load32(&q->head);
    if (h - fa_load32(&q->tail) >= FA_EVENT_QUEUE_SIZE) return 0;
    q->items[h & (FA_EVENT_QUEUE_SIZE - 1)] = *ev;
    fa_store32(&q->head, h + 1);
    return 1;
}

int fa_event_queue_pop(fa_event_queue* q, fa_qevent* out)
{
    int32_t t = fa_load32(&q->tail);
    if (t == fa_load32(&q->head)) return 0;
    *out = q->items[t & (FA_EVENT_QUEUE_SIZE - 1)];
    fa_store32(&q->tail, t + 1);
    return 1;
}

void fa_audio_post_event(struct fa_device* d, struct fa_stream* s, fa_event_type type, int64_t frame, int64_t value)
{
    fa_qevent q;
    int i;
    q.ev.type = type;
    q.ev.stream = s;
    q.ev.frame = frame;
    q.ev.value = value;
    q.seq = fa_fetch_add32(&d->engine->event_seq, 1);
    if (fa_event_queue_push(&d->events, &q)) return;
    for (i = 0; i < FA_EVENT_SPILL; i++) {
        if (fa_load32(&s->spill_full[i])) continue;
        s->spill[i] = q;
        fa_store32(&s->spill_full[i], 1);
        fa_store32(&d->spilled, 1);
        return;
    }
    /* More than FA_EVENT_SPILL events for one stream in one round with the queue
     * full as well: cannot happen with two mixed parameters (see FA_EVENT_SPILL). */
}

/* Caller holds list_lock. */
static void pending_add(fa_engine* e, const fa_qevent* q)
{
    if (e->pending_count == e->pending_cap) {
        uint32_t cap = e->pending_cap ? e->pending_cap * 2 : 64;
        fa_qevent* p = (fa_qevent*)realloc(e->pending, cap * sizeof(fa_qevent));
        if (!p) return;
        e->pending = p;
        e->pending_cap = cap;
    }
    e->pending[e->pending_count++] = *q;
}

void fa_engine_post_event(fa_engine* e, const fa_event* ev)
{
    fa_qevent q;
    q.ev = *ev;
    fa_mutex_lock(&e->list_lock);
    q.seq = fa_fetch_add32(&e->event_seq, 1);
    pending_add(e, &q);
    fa_cond_broadcast(&e->event_cond);
    fa_mutex_unlock(&e->list_lock);
}

void fa_event_rescue_stream(fa_engine* e, struct fa_stream* s)
{
    int i;
    for (i = 0; i < FA_EVENT_SPILL; i++) {
        if (!fa_load32(&s->spill_full[i])) continue;
        pending_add(e, &s->spill[i]);
        fa_store32(&s->spill_full[i], 0);
    }
}

void fa_event_rescue_device(fa_engine* e, struct fa_device* d)
{
    fa_qevent q;
    while (fa_event_queue_pop(&d->events, &q)) pending_add(e, &q);
    fa_cond_broadcast(&e->event_cond);
}

static int by_seq(const void* a, const void* b)
{
    int32_t d = ((const fa_qevent*)a)->seq - ((const fa_qevent*)b)->seq; /* wraps safely */
    return d < 0 ? -1 : d > 0;
}

static void deliver(const fa_event* ev)
{
    struct fa_stream* s = ev->stream;
    fa_event_callback cb;
    void* user;
    /* Announce before checking `closed`: fa_stream_close sets `closed` and then
     * waits for in_callback == 0, so either it sees us here or we see it closed. */
    fa_store32(&s->in_callback, 1);
    if (!fa_load32(&s->closed)) {
        fa_mutex_lock(&s->cb_lock);
        cb = s->callback;
        user = s->callback_user;
        fa_mutex_unlock(&s->cb_lock);
        if (cb) cb(ev, user);
    }
    fa_store32(&s->in_callback, 0);
}

void fa_engine_free(fa_engine* e);

void fa_event_thread_main(void* arg)
{
    fa_engine* e = (fa_engine*)arg;
    fa_qevent* batch = NULL;
    uint32_t cap = 0;
    fa_store64(&e->event_thread_id, (int64_t)fa_thread_current_id());

    fa_mutex_lock(&e->list_lock);
    for (;;) {
        struct fa_stream* grave;
        struct fa_stream* s;
        struct fa_device* d;
        uint32_t n, i, want;
        int quit = fa_load32(&e->quit), spilled = 0;

        /* Streams closed before this point are freed after this round, which
         * delivers (or skips) every event queued for them: a closed stream was
         * detached first, so nothing more can be queued for it. */
        grave = e->graveyard;
        e->graveyard = NULL;

        /* Take everything queued so far, from every source, then deliver it in
         * the order it happened. */
        want = e->pending_count;
        for (d = e->devices; d; d = d->next) {
            want += FA_EVENT_QUEUE_SIZE;
            if (fa_exchange32(&d->spilled, 0)) spilled = 1;
        }
        if (spilled) for (s = e->streams; s; s = s->next) want += FA_EVENT_SPILL;
        if (want > cap) {
            fa_qevent* p = (fa_qevent*)realloc(batch, want * sizeof(fa_qevent));
            if (p) { batch = p; cap = want; }
        }
        n = 0;
        if (cap >= want) {
            if (e->pending_count) memcpy(batch, e->pending, e->pending_count * sizeof(fa_qevent));
            n = e->pending_count;
            e->pending_count = 0;
            for (d = e->devices; d; d = d->next)
                while (fa_event_queue_pop(&d->events, &batch[n])) n++;
            if (spilled)
                for (s = e->streams; s; s = s->next)
                    for (i = 0; i < FA_EVENT_SPILL; i++)
                        if (fa_load32(&s->spill_full[i])) {
                            batch[n++] = s->spill[i];
                            fa_store32(&s->spill_full[i], 0);
                        }
        } else if (grave) {
            /* Out of memory: nothing delivered, so nothing may be freed yet. */
            struct fa_stream* last = grave;
            while (last->next) last = last->next;
            last->next = e->graveyard;
            e->graveyard = grave;
            grave = NULL;
        }
        fa_mutex_unlock(&e->list_lock);

        if (n > 1) qsort(batch, n, sizeof(fa_qevent), by_seq);
        for (i = 0; i < n; i++) deliver(&batch[i].ev);
        while (grave) {
            struct fa_stream* next = grave->next;
            fa_stream_free_memory(grave);
            grave = next;
        }

        fa_mutex_lock(&e->list_lock);
        if (quit && e->pending_count == 0 && !e->graveyard) break;
        if (e->pending_count == 0) fa_cond_wait_ms(&e->event_cond, &e->list_lock, 5);
    }
    fa_mutex_unlock(&e->list_lock);
    free(batch);
    if (e->destroy_on_event_thread) fa_engine_free(e); /* see fa_engine_destroy */
}
