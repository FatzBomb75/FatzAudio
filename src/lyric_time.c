/* The lyric surface's timeline (lyric_time.h). The rules are those of MediaControl's
 * design reference (docs/requests/2026-10-20_MediaControl_KaraokeDesignReference.md),
 * with its fixed numbers as settings and its quirks handled as the plan says:
 * lines too wide are split at a word when they are built (Q4), the 42-character rule
 * also breaks before a syllable that starts with a space (Q8), and a line leaving the
 * top of the window fades out as it scrolls instead of vanishing. */
#include "lyric_time.h"

#include <stdlib.h>
#include <string.h>

/* ---- easing ------------------------------------------------------------------------ */

static float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

float ly_smoothstep(float t)
{
    t = clamp01(t);
    return t * t * (3.0f - 2.0f * t);
}

float ly_ease_in_cubic(float t)
{
    t = clamp01(t);
    return t * t * t;
}

float ly_ease_in_out_cubic(float t)
{
    float u;
    t = clamp01(t);
    if (t < 0.5f) return 4.0f * t * t * t;
    u = -2.0f * t + 2.0f;
    return 1.0f - u * u * u / 2.0f;
}

float ly_ease_in_out_quint(float t)
{
    float u;
    t = clamp01(t);
    if (t < 0.5f) return 16.0f * t * t * t * t * t;
    u = -2.0f * t + 2.0f;
    return 1.0f - u * u * u * u * u / 2.0f;
}

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

/* (t - a) / (b - a), clamped; `empty` when b <= a. */
static float progress(fa_time t, fa_time a, fa_time b, float empty)
{
    if (b <= a) return empty;
    return clamp01((float)((double)(t - a) / (double)(b - a)));
}

/* ---- building lines ------------------------------------------------------------- */

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

typedef struct part {
    size_t off;                   /* in the raw text */
    fa_time time;
} part;

typedef struct builder {
    char* raw;
    size_t len, cap;
    part* parts;
    size_t nparts, pcap;
    ly_line* lines;
    size_t nlines, lcap;
    int failed;
} builder;

static void* grow(void* p, size_t* cap, size_t need, size_t item, int* failed)
{
    size_t c = *cap ? *cap : 16;
    void* n;
    if (need <= *cap) return p;
    while (c < need) c *= 2;
    n = realloc(p, c * item);
    if (!n) { *failed = 1; return NULL; }
    *cap = c;
    return n;
}

static int raw_append(builder* b, const char* s, fa_time t)
{
    size_t n = strlen(s);
    char* r;
    part* pp;
    r = (char*)grow(b->raw, &b->cap, b->len + n + 1, 1, &b->failed);
    if (!r) return 0;
    b->raw = r;
    pp = (part*)grow(b->parts, &b->pcap, b->nparts + 1, sizeof(part), &b->failed);
    if (!pp) return 0;
    b->parts = pp;
    b->parts[b->nparts].off = b->len;
    b->parts[b->nparts].time = t;
    b->nparts++;
    memcpy(b->raw + b->len, s, n + 1);
    b->len += n;
    return 1;
}

static int raw_has_text(const builder* b)
{
    size_t i;
    for (i = 0; i < b->len; i++)
        if (!is_space(b->raw[i])) return 1;
    return 0;
}

/* Code points, as the 42-character rule counts. */
static size_t raw_chars(const builder* b)
{
    size_t i, n = 0;
    for (i = 0; i < b->len; i++)
        if (((unsigned char)b->raw[i] & 0xC0) != 0x80) n++;
    return n;
}

static int raw_ends_space(const builder* b) { return b->len > 0 && is_space(b->raw[b->len - 1]); }

static int raw_ends_sentence(const builder* b)
{
    size_t n = b->len;
    char c;
    while (n > 0 && is_space(b->raw[n - 1])) n--;
    if (!n) return 0;
    c = b->raw[n - 1];
    return c == '.' || c == '!' || c == '?' || c == ':' || c == ';';
}

static ly_line* add_line(builder* b)
{
    ly_line* l = (ly_line*)grow(b->lines, &b->lcap, b->nlines + 1, sizeof(ly_line), &b->failed);
    if (!l) return NULL;
    b->lines = l;
    l = &b->lines[b->nlines++];
    memset(l, 0, sizeof(*l));
    l->fit = 1.0f;
    return l;
}

/* The parts so far as one line: single spaces, a syllable per part over its first to
 * last character. */
static void flush(builder* b, int page)
{
    char* out;
    ly_seg* seg;
    size_t i, o = 0, ns = 0;
    int pending = 0;
    ly_line* l;
    if (!b->nparts) return;
    if (!raw_has_text(b)) {
        b->len = 0;
        b->nparts = 0;
        return;
    }
    out = (char*)malloc(b->len + 1);
    seg = (ly_seg*)malloc(b->nparts * sizeof(ly_seg));
    if (!out || !seg) {
        free(out);
        free(seg);
        b->failed = 1;
        return;
    }
    for (i = 0; i < b->nparts; i++) {
        size_t from = b->parts[i].off, to = i + 1 < b->nparts ? b->parts[i + 1].off : b->len, k;
        long first = -1;
        size_t last = 0;
        for (k = from; k < to; k++) {
            char c = b->raw[k];
            if (is_space(c)) {
                if (o) pending = 1;
                continue;
            }
            if (pending) {
                out[o++] = ' ';
                pending = 0;
            }
            if (first < 0) first = (long)o;
            out[o++] = c;
            last = o;
        }
        if (first >= 0) {
            seg[ns].start = (uint32_t)first;
            seg[ns].len = (uint32_t)(last - (size_t)first);
            seg[ns].time = b->parts[i].time;
            ns++;
        }
    }
    out[o] = 0;
    b->len = 0;
    b->nparts = 0;
    l = add_line(b);
    if (!l) {
        free(out);
        free(seg);
        return;
    }
    l->text = out;
    l->text_len = (uint32_t)o;
    l->seg = seg;
    l->nseg = (uint32_t)ns;
    l->page = page;
}

/* Splits the lines wider than `max_width` at the spaces between syllables: each row
 * becomes a line of its own, on the same page. A row of one word that is still too
 * wide is drawn smaller (`fit`). */
static int split_wide(ly_timeline* tl, ly_measure measure, void* ctx, float max_width)
{
    uint32_t i;
    for (i = 0; i < tl->n; i++) {
        ly_line* l = &tl->line[i];
        uint32_t a = 0, rows = 0, r;
        uint32_t starts[256];
        if (measure(ctx, l->text, l->text_len) <= max_width || l->nseg < 2) {
            if (l->nseg && measure(ctx, l->text, l->text_len) > max_width)
                l->fit = max_width / measure(ctx, l->text, l->text_len);
            continue;
        }
        /* Rows: from syllable a, as many syllables as fit, ending before a space. */
        while (a < l->nseg && rows < 256) {
            uint32_t j, best = 0;
            starts[rows++] = a;
            for (j = a + 1; j <= l->nseg; j++) {
                int brk = j == l->nseg || (l->seg[j].start > 0 && l->text[l->seg[j].start - 1] == ' ');
                uint32_t end = l->seg[j - 1].start + l->seg[j - 1].len;
                if (!brk) continue;
                if (measure(ctx, l->text + l->seg[a].start, end - l->seg[a].start) <= max_width || !best) best = j;
                else break;
            }
            a = best;
        }
        if (rows < 2) {
            l->fit = max_width / measure(ctx, l->text, l->text_len);
            continue;
        }
        {
            ly_line* n = (ly_line*)realloc(tl->line, (tl->n + rows - 1) * sizeof(ly_line));
            ly_line old;
            if (!n) return 0;
            tl->line = n;
            l = &tl->line[i];
            old = *l;
            memmove(&tl->line[i + rows], &tl->line[i + 1], (tl->n - i - 1) * sizeof(ly_line));
            tl->n += rows - 1;
            for (r = 0; r < rows; r++) {
                uint32_t s0 = starts[r], s1 = r + 1 < rows ? starts[r + 1] : old.nseg, k;
                uint32_t b0 = old.seg[s0].start, b1 = old.seg[s1 - 1].start + old.seg[s1 - 1].len;
                ly_line* row = &tl->line[i + r];
                float w;
                memset(row, 0, sizeof(*row));
                row->page = old.page;
                row->fit = 1.0f;
                row->text = (char*)malloc(b1 - b0 + 1);
                row->seg = (ly_seg*)malloc((s1 - s0) * sizeof(ly_seg));
                if (!row->text || !row->seg) {
                    /* Keep the timeline consistent for ly_free. */
                    for (k = r + 1; k < rows; k++) memset(&tl->line[i + k], 0, sizeof(ly_line));
                    free(old.text);
                    free(old.seg);
                    return 0;
                }
                memcpy(row->text, old.text + b0, b1 - b0);
                row->text[b1 - b0] = 0;
                row->text_len = b1 - b0;
                for (k = s0; k < s1; k++) {
                    row->seg[k - s0] = old.seg[k];
                    row->seg[k - s0].start -= b0;
                }
                row->nseg = s1 - s0;
                w = measure(ctx, row->text, row->text_len);
                if (w > max_width) row->fit = max_width / w;
            }
            free(old.text);
            free(old.seg);
            i += rows - 1;
        }
    }
    return 1;
}

static int frag_cmp(const void* a, const void* b)
{
    const ly_frag* x = *(const ly_frag* const*)a;
    const ly_frag* y = *(const ly_frag* const*)b;
    if (x->time != y->time) return x->time < y->time ? -1 : 1;
    return x < y ? -1 : x > y;   /* stable: the array's order */
}

fa_result ly_build(ly_timeline* tl, const ly_frag* frags, uint32_t count, const ly_params* p, ly_measure measure,
                   void* measure_ctx, float max_width, ly_beat beat, void* beat_ctx)
{
    builder b;
    const ly_frag** order;
    fa_time prev = 0;
    int page = 0, have_prev = 0;
    uint32_t i;
    memset(tl, 0, sizeof(*tl));
    memset(&b, 0, sizeof(b));
    order = (const ly_frag**)malloc((count ? count : 1) * sizeof(*order));
    if (!order) return FA_ERROR_OUT_OF_MEMORY;
    for (i = 0; i < count; i++) order[i] = &frags[i];
    qsort(order, count, sizeof(*order), frag_cmp);
    for (i = 0; i < count && !b.failed; i++) {
        const ly_frag* f = order[i];
        fa_time t = f->time - p->lead < 0 ? 0 : f->time - p->lead;
        if (f->flags & FA_MIDI_NEW_PARAGRAPH) {
            flush(&b, page);
            page++;
        } else if (f->flags & FA_MIDI_NEW_LINE) {
            /* The song's own line mark: the gaps are not looked at (as MediaControl). */
            flush(&b, page);
        } else if (have_prev && raw_has_text(&b)) {
            fa_time gap = t - prev;
            if (gap >= p->paragraph_gap) {
                flush(&b, page);
                page++;
            } else if (gap >= p->sentence_gap && raw_ends_sentence(&b)) {
                flush(&b, page);
            } else if (p->break_length > 0 && raw_chars(&b) >= (size_t)p->break_length &&
                       (raw_ends_space(&b) || is_space(f->text[0]))) {
                flush(&b, page);
            }
        }
        raw_append(&b, f->text, t);
        prev = t;
        have_prev = 1;
    }
    if (!b.failed) flush(&b, page);
    free(order);
    free(b.raw);
    free(b.parts);
    tl->line = b.lines;
    tl->n = (uint32_t)b.nlines;
    if (b.failed) {
        ly_free(tl);
        return FA_ERROR_OUT_OF_MEMORY;
    }
    if (measure && max_width > 0.0f && !split_wide(tl, measure, measure_ctx, max_width)) {
        ly_free(tl);
        return FA_ERROR_OUT_OF_MEMORY;
    }
    for (i = 0; i < tl->n; i++) {
        ly_line* l = &tl->line[i];
        uint32_t k;
        l->first = l->seg[0].time;
        l->last = l->seg[0].time;
        for (k = 1; k < l->nseg; k++) {
            if (l->seg[k].time < l->first) l->first = l->seg[k].time;
            if (l->seg[k].time > l->last) l->last = l->seg[k].time;
        }
    }
    ly_schedule(tl, p, beat, beat_ctx);
    return FA_OK;
}

void ly_free(ly_timeline* tl)
{
    uint32_t i;
    for (i = 0; i < tl->n; i++) {
        free(tl->line[i].text);
        free(tl->line[i].seg);
    }
    free(tl->line);
    memset(tl, 0, sizeof(*tl));
}

/* ---- timing --------------------------------------------------------------------- */

#define LY_SECOND 1000000000LL

static fa_time max_t(fa_time a, fa_time b) { return a > b ? a : b; }
static fa_time min_t(fa_time a, fa_time b) { return a < b ? a : b; }

/* The whole countdown before a line, 0 when it has none. */
static fa_time countdown_span(const ly_line* l, const ly_params* p)
{
    return l->countdown ? l->cd_period * (p->countdown_count + 1) : 0;
}

void ly_schedule(ly_timeline* tl, const ly_params* p, ly_beat beat, void* beat_ctx)
{
    uint32_t i;
    fa_time last_end;
    for (i = 0; i < tl->n; i++) {
        ly_line* l = &tl->line[i];
        fa_time prev_last = i ? tl->line[i - 1].last : 0;
        l->countdown = p->countdown != FA_COUNTDOWN_OFF && l->first - prev_last > p->countdown_gap;
        if (p->countdown == FA_COUNTDOWN_BEAT) l->cd_period = beat ? beat(beat_ctx, l->first) : LY_SECOND / 2;
        else l->cd_period = p->countdown_time / (p->countdown_count + 1);
        if (l->cd_period <= 0) l->cd_period = 1;
    }
    for (i = 0; i < tl->n; i++) {
        ly_line* l = &tl->line[i];
        l->clears = 0;
        l->out0 = l->out1 = l->in0 = l->in1 = 0;
        if (i == 0) {
            /* The first page fades in, done a moment (or the countdown) before the
             * first syllable. */
            fa_time done = max_t(0, l->first - (l->countdown ? countdown_span(l, p) : p->page_preview));
            l->t1 = done;
            l->t0 = max_t(0, done - p->page);
            l->cue = 0;
            continue;
        }
        if (l->page != tl->line[i - 1].page) {
            fa_time end = tl->line[i - 1].last, start = l->first, total = start - end;
            l->clears = 1;
            if (total <= 0) {
                l->out0 = l->out1 = end;
                l->in0 = l->in1 = start;
            } else {
                fa_time lead = l->countdown ? min_t(countdown_span(l, p), total)
                                            : min_t(p->page_preview, max_t(0, total - 2 * p->page));
                fa_time ready = start - lead, window = max_t(0, ready - end), fade = min_t(p->page, window);
                fa_time hold = window >= 2 * p->page ? min_t(p->page_hold, window - 2 * fade) : 0;
                l->out0 = end + hold;
                l->out1 = l->out0 + fade;
                l->in1 = ready;
                l->in0 = ready - fade;
                if (!p->page_overlap && l->out1 > l->in0) {
                    l->in0 = l->out1;
                    l->in1 = l->in0 + fade;
                }
            }
            l->t0 = l->out0;
            l->t1 = l->in1;
            l->cue = l->out0;
        } else {
            /* Scroll: done when the line is to be shown (its countdown included),
             * or in the middle of the gap when that is shorter than the scroll,
             * never before the line above has started. */
            const ly_line* c = &tl->line[i - 1];
            fa_time show = max_t(0, l->first - countdown_span(l, p)), gap = show - c->last;
            fa_time start = gap >= p->scroll ? show - p->scroll : c->last + gap / 2 - p->scroll / 2;
            if (start < c->first) start = c->first;
            l->t0 = start;
            l->t1 = start + p->scroll;
            l->cue = start;
        }
        if (l->cue <= tl->line[i - 1].cue) l->cue = tl->line[i - 1].cue + 1;
    }
    tl->duration = p->duration;
    if (!tl->n) {
        tl->end_fade0 = tl->end_fade1 = 0;
        return;
    }
    last_end = tl->line[tl->n - 1].last;
    for (i = 0; i < tl->n; i++)
        if (tl->line[i].last > last_end) last_end = tl->line[i].last;
    if (tl->duration <= 0) tl->duration = last_end + 3 * LY_SECOND;
    tl->end_fade0 = last_end + p->end_hold;
    tl->end_fade1 = tl->end_fade0 + p->end_fade;
    if (tl->end_fade1 > tl->duration) {
        tl->end_fade1 = tl->duration;
        tl->end_fade0 = max_t(last_end, tl->duration - p->end_fade);
    }
}

/* ---- one position --------------------------------------------------------------- */

static float stable_opacity(const ly_params* p, int rel)
{
    if (rel == 0) return 1.0f;
    if (rel < 0) return -rel <= p->lines_above ? p->above_opacity[-rel - 1] : 0.0f;
    if (rel > p->lines_below) return 0.0f;
    return rel == 1 ? p->next_opacity : p->next_opacity * p->after_next_factor;
}

static float stable_scale(const ly_params* p, int rel)
{
    int d = rel < 0 ? -rel : rel;
    return d == 0 ? 1.0f : d == 1 ? p->near_scale : d == 2 ? p->far_scale : p->far_scale * (2.0f / 3.0f);
}

static uint32_t active_line(const ly_timeline* tl, fa_time t)
{
    uint32_t lo = 0, hi = tl->n;
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if (tl->line[mid].cue <= t) lo = mid;
        else hi = mid;
    }
    return lo;
}

static void put(ly_show* out, uint32_t* n, uint32_t line, int outgoing, float slot, float scale, float opacity)
{
    if (opacity <= 0.0f || *n >= LY_MAX_SHOW) return;
    out[*n].line = line;
    out[*n].outgoing = outgoing;
    out[*n].slot = slot;
    out[*n].scale = scale;
    out[*n].opacity = opacity > 1.0f ? 1.0f : opacity;
    (*n)++;
}

uint32_t ly_frame(const ly_timeline* tl, const ly_params* p, fa_time t, ly_show out[LY_MAX_SHOW])
{
    uint32_t n = 0, a;
    float end = 1.0f;
    const ly_line* al;
    int j;
    if (!tl->n) return 0;
    if (tl->end_fade1 > tl->end_fade0 && t >= tl->end_fade0)
        end = t >= tl->end_fade1 ? 0.0f : 1.0f - ly_smoothstep(progress(t, tl->end_fade0, tl->end_fade1, 1.0f));
    if (end <= 0.0f) return 0;
    a = active_line(tl, t);
    al = &tl->line[a];
    if (al->clears) {
        int last = (int)a - 1, pg = tl->line[last].page;
        float fo = ly_ease_in_cubic(progress(t, al->out0, al->out1, 1.0f));
        float fi = al->in1 > al->in0 ? ly_ease_in_out_cubic(progress(t, al->in0, al->in1, 1.0f)) : (t >= al->in1 ? 1.0f : 0.0f);
        for (j = last - p->lines_above; j <= last; j++) {
            int rel = j - last;
            if (j < 0 || tl->line[j].page != pg) continue;
            put(out, &n, (uint32_t)j, 1, (float)rel, stable_scale(p, rel) * lerpf(1.0f, p->page_out_scale, fo),
                stable_opacity(p, rel) * (1.0f - fo) * end);
        }
        for (j = (int)a - p->lines_above; j <= (int)a + p->lines_below; j++) {
            int rel = j - (int)a;
            if (j < 0 || (uint32_t)j >= tl->n || tl->line[j].page != al->page) continue;
            put(out, &n, (uint32_t)j, 0, (float)rel, stable_scale(p, rel) * lerpf(p->page_in_scale, 1.0f, fi),
                stable_opacity(p, rel) * fi * end);
        }
        return n;
    }
    {
        float s = ly_smoothstep(progress(t, al->t0, al->t1, 1.0f));
        if (a > 0) {
            /* Every line moves up a slot; the one leaving the top fades out on the way. */
            for (j = (int)a - p->lines_above - 1; j <= (int)a + p->lines_below; j++) {
                int rel = j - (int)a;
                if (j < 0 || (uint32_t)j >= tl->n || tl->line[j].page != al->page) continue;
                put(out, &n, (uint32_t)j, 0, lerpf((float)(rel + 1), (float)rel, s),
                    lerpf(stable_scale(p, rel + 1), stable_scale(p, rel), s),
                    lerpf(stable_opacity(p, rel + 1), stable_opacity(p, rel), s) * end);
            }
        } else {
            for (j = 0; j <= p->lines_below; j++) {
                if ((uint32_t)j >= tl->n || tl->line[j].page != al->page) continue;
                put(out, &n, (uint32_t)j, 0, (float)j, stable_scale(p, j), stable_opacity(p, j) * s * end);
            }
        }
    }
    return n;
}

void ly_wipe_at(const ly_line* l, const ly_params* p, fa_time t, ly_wipe* out)
{
    uint32_t k = 0;
    int c;
    fa_time d;
    float frac;
    while (k < l->nseg && l->seg[k].time <= t) k++;
    out->full = 0;
    out->current = -1;
    out->mix = 0.0f;
    if (!k) return;
    c = (int)k - 1;
    /* The syllable's wipe: a share of the time to the next one, at most the cap,
     * none (an instant cut) when that is very short. */
    if (p->wipe_cap <= 0) {
        d = 0;
    } else if ((uint32_t)c + 1 >= l->nseg) {
        d = p->wipe_cap;
    } else {
        fa_time gap = l->seg[c + 1].time - l->seg[c].time;
        d = gap <= 0 ? 0 : (fa_time)((double)gap * p->wipe_share);
        if (d > p->wipe_cap) d = p->wipe_cap;
        if (d < p->instant_cut) d = 0;
    }
    frac = d <= 0 ? 1.0f : clamp01((float)((double)(t - l->seg[c].time) / (double)d));
    if (frac >= 1.0f) {
        out->full = (int)k;
        return;
    }
    out->full = c;
    out->current = c;
    out->mix = ly_ease_in_out_quint(frac);
}

void ly_countdown_at(const ly_line* l, const ly_params* p, fa_time t, ly_count* out)
{
    fa_time span = countdown_span(l, p), rem = l->first - t;
    memset(out, 0, sizeof(*out));
    if (!l->countdown || rem <= 0 || rem > span) return;
    out->shown = 1;
    if (rem > l->cd_period) {
        double e = (double)(span - rem) / (double)l->cd_period;
        out->done = (int)e;
        if (out->done > p->countdown_count) out->done = p->countdown_count;
        out->progress = (float)(e - (double)out->done);
        out->opacity = 1.0f;
    } else {
        out->done = p->countdown_count;
        out->opacity = (float)((double)rem / (double)l->cd_period);
    }
}
