/* Lock-free single-producer/single-consumer ring of interleaved float frames.
 * The counters only grow, so "readable = write - read" needs no wrap handling. */
#include "fa_internal.h"
#include <stdlib.h>
#include <string.h>

fa_result fa_ring_init(fa_ring* r, uint32_t channels, int64_t capacity) { return fa_ring_init_kept(r, channels, capacity, 0); }

fa_result fa_ring_init_kept(fa_ring* r, uint32_t channels, int64_t capacity, int64_t keep)
{
    memset(r, 0, sizeof(*r));
    r->data = (float*)calloc((size_t)(capacity + keep) * channels, sizeof(float));
    if (!r->data) return FA_ERROR_OUT_OF_MEMORY;
    r->channels = channels;
    r->capacity = capacity;
    r->size = capacity + keep;
    return FA_OK;
}

void fa_ring_free(fa_ring* r)
{
    free(r->data);
    memset(r, 0, sizeof(*r));
}

int64_t fa_ring_readable(fa_ring* r) { return fa_load64(&r->write) - fa_load64(&r->read); }
int64_t fa_ring_writable(fa_ring* r) { return r->capacity - fa_ring_readable(r); }

void fa_ring_write(fa_ring* r, const float* src, int64_t frames)
{
    int64_t w = fa_load64(&r->write);
    int64_t at = w % r->size;
    int64_t first = r->size - at < frames ? r->size - at : frames;
    size_t ch = r->channels;
    memcpy(r->data + (size_t)at * ch, src, (size_t)first * ch * sizeof(float));
    if (frames > first)
        memcpy(r->data, src + (size_t)first * ch, (size_t)(frames - first) * ch * sizeof(float));
    fa_store64(&r->write, w + frames); /* publishes the samples to the consumer */
}

float* fa_ring_write_region(fa_ring* r, int64_t* out_frames)
{
    int64_t w = fa_load64(&r->write);
    int64_t at = w % r->size, room = fa_ring_writable(r);
    *out_frames = r->size - at < room ? r->size - at : room;
    return r->data + (size_t)at * r->channels;
}

void fa_ring_commit(fa_ring* r, int64_t frames)
{
    fa_store64(&r->write, fa_load64(&r->write) + frames); /* publishes the samples to the consumer */
}

void fa_ring_regions(fa_ring* r, int64_t frames, const float** p1, int64_t* n1, const float** p2, int64_t* n2)
{
    int64_t rd = fa_load64(&r->read);
    int64_t at = rd % r->size;
    int64_t first = r->size - at < frames ? r->size - at : frames;
    *p1 = r->data + (size_t)at * r->channels;
    *n1 = first;
    *p2 = r->data;
    *n2 = frames - first;
}

void fa_ring_advance_read(fa_ring* r, int64_t frames)
{
    fa_store64(&r->read, fa_load64(&r->read) + frames);
}

const float* fa_ring_frame(fa_ring* r, int64_t idx)
{
    return r->data + (size_t)(idx % r->size) * r->channels;
}

void fa_ring_reset(fa_ring* r)
{
    fa_store64(&r->read, 0);
    fa_store64(&r->write, 0);
}
