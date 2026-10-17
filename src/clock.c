/* The presentation clock: which timestamp is leaving the speaker right now.
 *
 * The audio thread records, for every stretch of a stream it mixes, the device
 * frames it went to and the presentation times they carried. A device frame is
 * heard one device latency after its callback started, so the frame at the
 * speaker is "callback start frame + elapsed time - latency"; looking it up in the
 * records gives its timestamp. Pauses, stalls and the end need no special cases:
 * between records the clock holds at the last timestamp that played, and before
 * the first record after a seek it holds at the seek target. */
#include "fa_internal.h"
#include <string.h>

void fa_clock_reset(fa_clock* c, int64_t hold_pts)
{
    int i;
    fa_fetch_add32(&c->epoch, 1);         /* odd: readers retry */
    fa_store64(&c->count, 0);
    for (i = 0; i < FA_CLOCK_HISTORY; i++) fa_store64(&c->rec[i].idx, -1);
    fa_store64(&c->hold, hold_pts);
    fa_fetch_add32(&c->epoch, 1);
}

void fa_clock_set_hold(fa_clock* c, int64_t hold_pts)
{
    fa_store64(&c->hold, hold_pts);
}

void fa_clock_record(fa_clock* c, int64_t dev, int64_t n, int64_t pts0, int64_t pts1)
{
    int64_t k = fa_load64(&c->count);
    fa_clock_rec* r;
    if (n <= 0) return;
    if (k > 0) {
        /* Continues the previous record exactly (same rate): extend it instead, so
         * the history covers more time. */
        fa_clock_rec* p = &c->rec[(k - 1) % FA_CLOCK_HISTORY];
        int64_t pd = fa_load64(&p->dev), pn = fa_load64(&p->n), p0 = fa_load64(&p->pts0), p1 = fa_load64(&p->pts1);
        if (pd + pn == dev && p1 - pts0 <= 2 && pts0 - p1 <= 2 && pn > 0) {
            double slope_prev = (double)(p1 - p0) / (double)pn, slope = (double)(pts1 - pts0) / (double)n;
            if (slope_prev - slope < 0.05 && slope - slope_prev < 0.05 && pn < ((int64_t)1 << 20)) {
                int64_t s = fa_load64(&p->seq);
                fa_store64(&p->seq, s + 1);       /* odd: readers retry */
                fa_store64(&p->n, pn + n);
                fa_store64(&p->pts1, pts1);
                fa_store64(&p->seq, s + 2);       /* a new even value: a reader that saw the old one retries */
                return;
            }
        }
    }
    r = &c->rec[k % FA_CLOCK_HISTORY];
    {
        int64_t s = fa_load64(&r->seq);
        fa_store64(&r->seq, s + 1);
        fa_store64(&r->idx, k);
        fa_store64(&r->dev, dev);
        fa_store64(&r->n, n);
        fa_store64(&r->pts0, pts0);
        fa_store64(&r->pts1, pts1);
        fa_store64(&r->seq, s + 2);
    }
    fa_store64(&c->count, k + 1);
}

/* Reads record k; 0 if it was overwritten, or written meanwhile. */
static int read_rec(fa_clock* c, int64_t k, int64_t* dev, int64_t* n, int64_t* p0, int64_t* p1)
{
    fa_clock_rec* r = &c->rec[k % FA_CLOCK_HISTORY];
    int64_t s1 = fa_load64(&r->seq), s2;
    if ((s1 & 1) || fa_load64(&r->idx) != k) return 0;
    *dev = fa_load64(&r->dev);
    *n = fa_load64(&r->n);
    *p0 = fa_load64(&r->pts0);
    *p1 = fa_load64(&r->pts1);
    s2 = fa_load64(&r->seq);
    return s1 == s2 && fa_load64(&r->idx) == k;
}

int64_t fa_clock_at(fa_clock* c, double frame)
{
    for (;;) {
        int32_t e1 = fa_load32(&c->epoch);
        int64_t cnt, k, result, lo;
        int have_oldest = 0, found = 0, torn = 0;
        int64_t oldest_pts = 0;
        if (e1 & 1) { fa_sleep_ms(0); continue; }
        cnt = fa_load64(&c->count);
        result = fa_load64(&c->hold);
        lo = cnt > FA_CLOCK_HISTORY ? cnt - FA_CLOCK_HISTORY : 0;
        for (k = cnt - 1; k >= lo; k--) {
            int64_t dev, n, p0, p1;
            if (!read_rec(c, k, &dev, &n, &p0, &p1)) {
                /* The newest record can be mid-extension: read it again. Older ones
                 * being overwritten means we went past the history. */
                if (k == cnt - 1) { torn = 1; break; }
                break;
            }
            if ((double)dev <= frame) {
                if (frame >= (double)(dev + n)) result = p1;    /* after it: holds at its end */
                else result = p0 + (int64_t)((double)(p1 - p0) * (frame - (double)dev) / (double)n);
                found = 1;
                break;
            }
            have_oldest = 1;
            oldest_pts = p0;
        }
        if (torn) continue;
        /* Before the first record is heard the clock holds (the seek target, the
         * last time heard before a flush); only when the records before the oldest
         * one kept were overwritten is the oldest one's start the best answer. */
        if (!found && have_oldest && cnt > FA_CLOCK_HISTORY) result = oldest_pts;
        if (fa_load32(&c->epoch) == e1) return result;
    }
}

int64_t fa_clock_mixed_end(fa_clock* c)
{
    for (;;) {
        int32_t e1 = fa_load32(&c->epoch);
        int64_t cnt, dev = 0, n = 0, p0, p1, end = -1;
        if (e1 & 1) { fa_sleep_ms(0); continue; }
        cnt = fa_load64(&c->count);
        if (cnt > 0) {
            if (!read_rec(c, cnt - 1, &dev, &n, &p0, &p1)) { fa_sleep_ms(0); continue; }   /* mid-write: again */
            end = dev + n;
        }
        if (fa_load32(&c->epoch) == e1) return end;
    }
}

/* ---- pts map ------------------------------------------------------------------ */

void fa_ptsmap_reset(fa_ptsmap* m)
{
    fa_store64(&m->write, 0);
    fa_store64(&m->read, 0);
    m->cur_idx = 0;
    m->cur_pts = 0;
}

void fa_ptsmap_add(fa_ptsmap* m, int64_t idx, int64_t pts)
{
    int64_t w = fa_load64(&m->write);
    if (w - fa_load64(&m->read) >= FA_PTSMAP_SIZE) return;  /* 64 jumps in one buffer: drop it */
    m->idx[w % FA_PTSMAP_SIZE] = idx;
    m->pts[w % FA_PTSMAP_SIZE] = pts;
    fa_store64(&m->write, w + 1);
}

int64_t fa_ptsmap_pts(fa_ptsmap* m, int64_t idx, double ns_per_frame, int64_t* out_next)
{
    int64_t r = fa_load64(&m->read), w = fa_load64(&m->write);
    while (r < w && m->idx[r % FA_PTSMAP_SIZE] <= idx) {
        m->cur_idx = m->idx[r % FA_PTSMAP_SIZE];
        m->cur_pts = m->pts[r % FA_PTSMAP_SIZE];
        r++;
    }
    fa_store64(&m->read, r);
    *out_next = r < w ? m->idx[r % FA_PTSMAP_SIZE] : INT64_MAX;
    return m->cur_pts + (int64_t)((double)(idx - m->cur_idx) * ns_per_frame + 0.5);
}
