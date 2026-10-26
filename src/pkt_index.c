/* Compact packet list for the MP4 and CAF demuxers; see pkt_index.h. */
#include "pkt_index.h"
#include <stdlib.h>
#include <string.h>

#define PKT_GROUP 64
#define PKT_BREAK 0x80000000u

static size_t groups(size_t n) { return (n + PKT_GROUP - 1) / PKT_GROUP; }

/* Resizes every per-packet array to `cap` packets (cap >= n). */
static fa_result set_cap(fa_pkt_index* x, size_t cap)
{
    uint32_t* size;
    fa_pkt_anchor* anchor;
    size_t g = groups(cap) ? groups(cap) : 1;
    if (cap == 0) cap = 1;
    size = (uint32_t*)realloc(x->size, cap * sizeof(*size));
    if (!size) goto fail;
    x->size = size;
    if (x->dur32) {
        uint32_t* d = (uint32_t*)realloc(x->dur32, cap * sizeof(*d));
        if (!d) goto fail;
        x->dur32 = d;
    } else {
        uint16_t* d = (uint16_t*)realloc(x->dur16, cap * sizeof(*d));
        if (!d) goto fail;
        x->dur16 = d;
    }
    anchor = (fa_pkt_anchor*)realloc(x->anchor, g * sizeof(*anchor));
    if (!anchor) goto fail;
    x->anchor = anchor;
    x->cap = cap;
    return FA_OK;
fail:
    /* A shrink may have gone through for some arrays: the room left is what all have. */
    if (cap < x->cap) x->cap = cap;
    return FA_ERROR_OUT_OF_MEMORY;
}

fa_result fa_pkt_reserve(fa_pkt_index* x, size_t n)
{
    return n > x->cap ? set_cap(x, n) : FA_OK;
}

/* Stores a duration, moving to 32-bit durations the first time one needs it. */
static fa_result set_dur(fa_pkt_index* x, size_t i, uint32_t dur)
{
    if (x->dur32) { x->dur32[i] = dur; return FA_OK; }
    if (dur > 0xFFFFu) {
        uint32_t* d = (uint32_t*)malloc(x->cap * sizeof(*d));
        size_t k;
        if (!d) return FA_ERROR_OUT_OF_MEMORY;
        for (k = 0; k < x->n; k++) d[k] = x->dur16[k];
        free(x->dur16);
        x->dur16 = NULL;
        x->dur32 = d;
        d[i] = dur;
        return FA_OK;
    }
    x->dur16[i] = (uint16_t)dur;
    return FA_OK;
}

uint32_t fa_pkt_size(const fa_pkt_index* x, size_t i) { return x->size[i] & ~PKT_BREAK; }
uint32_t fa_pkt_dur(const fa_pkt_index* x, size_t i) { return x->dur32 ? x->dur32[i] : x->dur16[i]; }

fa_result fa_pkt_add(fa_pkt_index* x, int64_t off, uint32_t size, int64_t pts, uint32_t dur)
{
    size_t i = x->n;
    uint32_t flag = 0;
    fa_result r;
    if (size > FA_PKT_MAX_SIZE) return FA_ERROR_UNSUPPORTED_FORMAT;
    if (i == x->cap) {
        r = set_cap(x, x->cap ? x->cap * 2 : 1024);
        if (r != FA_OK) return r;
    }
    if (i % PKT_GROUP == 0) {
        fa_pkt_anchor* a = &x->anchor[i / PKT_GROUP];
        a->off = off;
        a->pts = pts;
        a->brk = x->nbrk;
    } else if (off != x->end_off || pts != x->end_pts) {
        if (x->nbrk == x->brk_cap) {
            size_t cap = x->brk_cap ? x->brk_cap * 2 : 64;
            fa_pkt_break* b = (fa_pkt_break*)realloc(x->brk, cap * sizeof(*b));
            if (!b) return FA_ERROR_OUT_OF_MEMORY;
            x->brk = b;
            x->brk_cap = cap;
        }
        x->brk[x->nbrk].off = off;
        x->brk[x->nbrk].pts = pts;
        x->nbrk++;
        flag = PKT_BREAK;
    }
    r = set_dur(x, i, dur);
    if (r != FA_OK) return r;
    x->size[i] = size | flag;
    x->n++;
    x->end_off = off + size;
    x->end_pts = pts + dur;
    return FA_OK;
}

fa_result fa_pkt_extend_last(fa_pkt_index* x, uint32_t size, uint32_t dur)
{
    size_t i = x->n - 1;
    uint32_t s = fa_pkt_size(x, i), d = fa_pkt_dur(x, i);
    fa_result r;
    if ((uint64_t)s + size > FA_PKT_MAX_SIZE) return FA_ERROR_UNSUPPORTED_FORMAT;
    r = set_dur(x, i, d + dur);
    if (r != FA_OK) return r;
    x->size[i] = (s + size) | (x->size[i] & PKT_BREAK);
    x->end_off += size;
    x->end_pts += dur;
    return FA_OK;
}

void fa_pkt_get(const fa_pkt_index* x, size_t i, fa_pkt* out)
{
    size_t j = i - i % PKT_GROUP;
    const fa_pkt_anchor* a = &x->anchor[j / PKT_GROUP];
    int64_t off = a->off, pts = a->pts;
    size_t b = a->brk;
    for (; j < i; j++) {
        off += fa_pkt_size(x, j);
        pts += fa_pkt_dur(x, j);
        if (x->size[j + 1] & PKT_BREAK) { off = x->brk[b].off; pts = x->brk[b].pts; b++; }
    }
    out->off = off;
    out->pts = pts;
    out->size = fa_pkt_size(x, i);
    out->dur = fa_pkt_dur(x, i);
}

size_t fa_pkt_find(const fa_pkt_index* x, int64_t t)
{
    size_t lo = 0, hi = groups(x->n), j, end, best;
    const fa_pkt_anchor* a;
    int64_t pts;
    size_t b;
    if (x->n == 0) return 0;
    /* The last group starting at or before t (the first when none does). */
    while (hi - lo > 1) {
        size_t mid = lo + (hi - lo) / 2;
        if (x->anchor[mid].pts <= t) lo = mid;
        else hi = mid;
    }
    a = &x->anchor[lo];
    j = best = lo * PKT_GROUP;
    end = j + PKT_GROUP < x->n ? j + PKT_GROUP : x->n;
    pts = a->pts;
    b = a->brk;
    for (j++; j < end; j++) {
        pts += fa_pkt_dur(x, j - 1);
        if (x->size[j] & PKT_BREAK) pts = x->brk[b++].pts;
        if (pts > t) break;
        best = j;
    }
    return best;
}

size_t fa_pkt_seek_back(const fa_pkt_index* x, int64_t t, int packets_back, size_t bytes_back)
{
    size_t j = fa_pkt_find(x, t), bytes = 0;
    int n = 0;
    while (j > 0 && n < 64 && (n < packets_back || bytes < bytes_back)) {
        j--;
        n++;
        bytes += fa_pkt_size(x, j);
    }
    return j;
}

int64_t fa_pkt_total_bytes(const fa_pkt_index* x)
{
    int64_t total = 0;
    size_t i;
    for (i = 0; i < x->n; i++) total += fa_pkt_size(x, i);
    return total;
}

void fa_pkt_trim(fa_pkt_index* x)
{
    if (x->n && x->n < x->cap) (void)set_cap(x, x->n);   /* keeps the old room if that fails */
    if (x->nbrk < x->brk_cap) {
        fa_pkt_break* b = x->nbrk ? (fa_pkt_break*)realloc(x->brk, x->nbrk * sizeof(*b)) : NULL;
        if (b || !x->nbrk) {
            if (!x->nbrk) free(x->brk);
            x->brk = b;
            x->brk_cap = x->nbrk;
        }
    }
}

void fa_pkt_clear(fa_pkt_index* x)
{
    x->n = 0;
    x->nbrk = 0;
    x->end_off = x->end_pts = 0;
}

void fa_pkt_free(fa_pkt_index* x)
{
    free(x->size);
    free(x->dur16);
    free(x->dur32);
    free(x->anchor);
    free(x->brk);
    memset(x, 0, sizeof(*x));
}
