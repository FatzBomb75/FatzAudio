/* Event delivery. The audio thread pushes into a per-device lock-free queue (or,
 * when that is full, a stream's spill slots); API threads add to the engine's
 * pending list under list_lock. Decode workers and the event thread both move
 * queued events into the pending list every few milliseconds, so a slow user
 * callback does not let the queues fill up. The event thread then calls the
 * user's callbacks in sequence order, so user code never runs on the audio
 * thread. It also frees closed streams, after their last event. */
#include "fa_internal.h"
#include <stdlib.h>
#include <string.h>

/* Queue indices and sequence numbers wrap: do the arithmetic unsigned. */
static int32_t wrap_diff(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }

int fa_event_queue_push(fa_event_queue* q, const fa_qevent* ev)
{
    uint32_t h = (uint32_t)fa_load32(&q->head);
    if (h - (uint32_t)fa_load32(&q->tail) >= FA_EVENT_QUEUE_SIZE) return 0;
    q->items[h & (FA_EVENT_QUEUE_SIZE - 1)] = *ev;
    fa_store32(&q->head, (int32_t)(h + 1u));
    return 1;
}

int fa_event_queue_pop(fa_event_queue* q, fa_qevent* out)
{
    uint32_t t = (uint32_t)fa_load32(&q->tail);
    if (t == (uint32_t)fa_load32(&q->head)) return 0;
    *out = q->items[t & (FA_EVENT_QUEUE_SIZE - 1)];
    fa_store32(&q->tail, (int32_t)(t + 1u));
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
    /* `publishing` covers the gap between taking a sequence number and making the
     * event visible; the event thread waits it out before delivering (see below). */
    fa_fetch_add32(&d->publishing, 1);
    q.seq = fa_fetch_add32(&d->engine->event_seq, 1);
    if (!fa_event_queue_push(&d->events, &q)) {
        for (i = 0; i < FA_EVENT_SPILL; i++) {
            if (fa_load32(&s->spill_full[i])) continue;
            s->spill[i] = q;
            fa_store32(&s->spill_full[i], 1);
            fa_store32(&d->spilled, 1);
            break;
        }
        /* Not reached: FA_EVENT_SPILL covers everything one stream can raise
         * between two collections. */
    }
    fa_fetch_add32(&d->publishing, -1);
}

/* Caller holds list_lock. */
static int pending_add(fa_engine* e, const fa_qevent* q)
{
    if (e->pending_count == e->pending_cap) {
        uint32_t cap = e->pending_cap ? e->pending_cap * 2 : 64;
        fa_qevent* p = (fa_qevent*)realloc(e->pending, cap * sizeof(fa_qevent));
        if (!p) return 0;
        e->pending = p;
        e->pending_cap = cap;
    }
    e->pending[e->pending_count++] = *q;
    return 1;
}

void fa_engine_post_event_locked(fa_engine* e, const fa_event* ev, int32_t seq)
{
    fa_qevent q;
    q.ev = *ev;
    q.seq = seq;
    pending_add(e, &q);
    fa_cond_broadcast(&e->event_cond);
}

void fa_engine_post_event(fa_engine* e, const fa_event* ev)
{
    fa_mutex_lock(&e->list_lock);
    fa_engine_post_event_locked(e, ev, fa_fetch_add32(&e->event_seq, 1));
    fa_mutex_unlock(&e->list_lock);
}

void fa_event_rescue_stream(fa_engine* e, struct fa_stream* s)
{
    int i;
    for (i = 0; i < FA_EVENT_SPILL; i++) {
        if (!fa_load32(&s->spill_full[i])) continue;
        if (!pending_add(e, &s->spill[i])) return;
        fa_store32(&s->spill_full[i], 0);
    }
}

void fa_event_rescue_device(fa_engine* e, struct fa_device* d)
{
    fa_qevent q;
    while (fa_event_queue_pop(&d->events, &q)) pending_add(e, &q);
    fa_cond_broadcast(&e->event_cond);
}

uint32_t fa_event_collect_locked(fa_engine* e)
{
    struct fa_device* d;
    struct fa_stream* s;
    uint32_t before = e->pending_count;
    int spilled = 0;
    for (d = e->devices; d; d = d->next) {
        fa_qevent q;
        if (fa_exchange32(&d->spilled, 0)) spilled = 1;
        for (;;) {
            /* Make room first, so a popped event always has a place to go. */
            if (e->pending_count == e->pending_cap) {
                uint32_t cap = e->pending_cap ? e->pending_cap * 2 : 64;
                fa_qevent* p = (fa_qevent*)realloc(e->pending, cap * sizeof(fa_qevent));
                if (!p) break;
                e->pending = p;
                e->pending_cap = cap;
            }
            if (!fa_event_queue_pop(&d->events, &q)) break;
            e->pending[e->pending_count++] = q;
        }
    }
    if (spilled)
        for (s = e->streams; s; s = s->next) fa_event_rescue_stream(e, s);
    if (e->pending_count != before) fa_cond_broadcast(&e->event_cond);
    return e->pending_count - before;
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

static int by_seq(const void* a, const void* b)
{
    int32_t d = wrap_diff(((const fa_qevent*)a)->seq, ((const fa_qevent*)b)->seq);
    return d < 0 ? -1 : d > 0;
}

static int devices_publishing(fa_engine* e)
{
    struct fa_device* d;
    for (d = e->devices; d; d = d->next)
        if (fa_load32(&d->publishing)) return 1;
    return 0;
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
        struct fa_stream* grave = NULL;
        uint32_t n = 0, i;
        int quit = fa_load32(&e->quit);
        /* Every sequence number below `w` was taken before this point. API events
         * take theirs under list_lock (held), so those are all in `pending`; once no
         * audio thread is between taking a number and publishing it, the collection
         * below finds every audio event below `w` too. Delivering exactly those, in
         * order, means nothing earlier can still turn up later. */
        int32_t w = fa_load32(&e->event_seq);
        if (!devices_publishing(e)) {
            fa_event_collect_locked(e);
            if (e->pending_count > 1) qsort(e->pending, e->pending_count, sizeof(fa_qevent), by_seq);
            while (n < e->pending_count && wrap_diff(e->pending[n].seq, w) < 0) n++;
            if (n > cap) {
                fa_qevent* p = (fa_qevent*)realloc(batch, n * sizeof(fa_qevent));
                if (p) { batch = p; cap = n; }
            }
            if (n <= cap) {
                if (n) {
                    memcpy(batch, e->pending, n * sizeof(fa_qevent));
                    memmove(e->pending, e->pending + n, (e->pending_count - n) * sizeof(fa_qevent));
                    e->pending_count -= n;
                }
                /* Streams closed before `w` was read have all their events below it
                 * (a stream is detached, so silent, before it is closed): free them
                 * after this round. */
                grave = e->graveyard;
                e->graveyard = NULL;
            } else {
                n = 0; /* out of memory: try again next round */
            }
        }
        fa_mutex_unlock(&e->list_lock);

        for (i = 0; i < n; i++) deliver(&batch[i].ev);
        while (grave) {
            struct fa_stream* next = grave->next;
            fa_stream_free_memory(grave);
            grave = next;
        }

        fa_mutex_lock(&e->list_lock);
        if (quit && e->pending_count == 0 && !e->graveyard) break;
        if (e->pending_count == 0) fa_cond_wait_ms(&e->event_cond, &e->list_lock, 5);
        else if (n == 0) fa_cond_wait_ms(&e->event_cond, &e->list_lock, 1); /* an audio thread is mid-publish */
    }
    fa_mutex_unlock(&e->list_lock);
    free(batch);
    if (e->destroy_on_event_thread) fa_engine_free(e); /* see fa_engine_destroy */
}
