/* Event delivery. The audio thread pushes into a per-device lock-free queue; API
 * and worker threads post into an engine list under a mutex. The engine's event
 * thread drains both and calls the user's callbacks, so user code never runs on
 * the audio thread. It also frees closed streams, after their last event. */
#include "fa_internal.h"
#include <stdlib.h>
#include <string.h>

int fa_event_queue_push(fa_event_queue* q, const fa_event* ev)
{
    int32_t h = fa_load32(&q->head);
    if (h - fa_load32(&q->tail) >= FA_EVENT_QUEUE_SIZE) {
        fa_fetch_add32(&q->dropped, 1);
        return 0;
    }
    q->items[h & (FA_EVENT_QUEUE_SIZE - 1)] = *ev;
    fa_store32(&q->head, h + 1);
    return 1;
}

int fa_event_queue_pop(fa_event_queue* q, fa_event* out)
{
    int32_t t = fa_load32(&q->tail);
    if (t == fa_load32(&q->head)) return 0;
    *out = q->items[t & (FA_EVENT_QUEUE_SIZE - 1)];
    fa_store32(&q->tail, t + 1);
    return 1;
}

void fa_engine_post_event(fa_engine* e, const fa_event* ev)
{
    fa_mutex_lock(&e->list_lock);
    if (e->pending_count == e->pending_cap) {
        uint32_t cap = e->pending_cap ? e->pending_cap * 2 : 64;
        fa_event* p = (fa_event*)realloc(e->pending, cap * sizeof(fa_event));
        if (!p) { fa_mutex_unlock(&e->list_lock); return; }
        e->pending = p;
        e->pending_cap = cap;
    }
    e->pending[e->pending_count++] = *ev;
    fa_cond_broadcast(&e->event_cond);
    fa_mutex_unlock(&e->list_lock);
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

#define FA_EVENT_BATCH 512

void fa_event_thread_main(void* arg)
{
    fa_engine* e = (fa_engine*)arg;
    fa_event* batch = (fa_event*)malloc(FA_EVENT_BATCH * sizeof(fa_event));
    fa_store64(&e->event_thread_id, (int64_t)fa_thread_current_id());

    fa_mutex_lock(&e->list_lock);
    for (;;) {
        struct fa_stream* grave;
        struct fa_device* d;
        uint32_t n = 0, i;
        int quit = fa_load32(&e->quit);

        /* Streams closed before this point are freed after this round of events,
         * which includes every event queued before they were closed. */
        grave = e->graveyard;
        e->graveyard = NULL;
        for (i = 0; i < e->pending_count && n < FA_EVENT_BATCH && batch; i++) batch[n++] = e->pending[i];
        if (batch && i < e->pending_count) memmove(e->pending, e->pending + i, (e->pending_count - i) * sizeof(fa_event));
        e->pending_count -= batch ? i : 0;
        for (d = e->devices; d && batch; d = d->next)
            while (n < FA_EVENT_BATCH && fa_event_queue_pop(&d->events, &batch[n])) n++;
        fa_mutex_unlock(&e->list_lock);

        for (i = 0; i < n; i++) deliver(&batch[i]);
        if (n == FA_EVENT_BATCH && grave) {
            /* Events may still be queued for these streams: free them next round. */
            struct fa_stream* last = grave;
            while (last->next) last = last->next;
            fa_mutex_lock(&e->list_lock);
            last->next = e->graveyard;
            e->graveyard = grave;
            fa_mutex_unlock(&e->list_lock);
            grave = NULL;
        }
        while (grave) {
            struct fa_stream* next = grave->next;
            fa_stream_free_memory(grave);
            grave = next;
        }

        fa_mutex_lock(&e->list_lock);
        if (quit && e->pending_count == 0 && !e->graveyard) break;
        if (n < FA_EVENT_BATCH && e->pending_count == 0) fa_cond_wait_ms(&e->event_cond, &e->list_lock, 5);
    }
    fa_mutex_unlock(&e->list_lock);
    free(batch);
}
